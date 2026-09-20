#include <err.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <pthread.h>

struct termios original_terminal;
int is_atty = 0;
int vmfd;

struct uart {
    pthread_mutex_t lock;
    int vmfd;

    uint8_t ier, lcr, mcr, fcr, scr, dll, dlm;
    uint8_t rx_byte;
    int rx_ready;
    int thre_int;
    int irq_level;
};

static struct uart uart;

void restore_terminal(){
    if(is_atty)
        tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal);
}

void recompute_function(){
    int desired = 0;
    if (uart.mcr & 0x08){
        if ((uart.ier & 0x01) && uart.rx_ready)
            desired = 1;
        else if ((uart.ier & 0x02) && uart.thre_int)
            desired = 1;
    }

    if (desired != uart.irq_level) {
        uart.irq_level = desired;
        struct kvm_irq_level lvl = {.irq = 4, .level = desired};
        ioctl(vmfd, KVM_IRQ_LINE, &lvl);
    }
}

void *input_function(void *args){

    char c;

    while(1){
        ssize_t rt = read(STDIN_FILENO, &c, 1);

        if(rt == 0) break;

        if(rt < 0){
            if(errno == EINTR) continue;
            break;
        }

        int ok = 0;
        while(!ok){
            pthread_mutex_lock(&uart.lock);

            // wait for it
            if(uart.rx_ready == 0){
                uart.rx_byte = c;
                uart.rx_ready = 1;
                recompute_function();
                ok = 1;
            }
            pthread_mutex_unlock(&uart.lock);

            if(ok == 0){
                usleep(200);
            }
        }
        
    }
    return NULL;
}

int main(void) {
    int ret;

    // send sigpipe if pipe goes away
    signal(SIGPIPE, SIG_IGN);

    // put terminal into raw mode

    // first copy the current state of the terminal into a struct
    if(isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &original_terminal) == 0){
        is_atty = 1;
        // make another termios struct to be put into raw mode
        struct termios current_terminal = original_terminal;

        cfmakeraw(&current_terminal);

        // make it take effect
        tcsetattr(STDIN_FILENO, TCSANOW, &current_terminal);

        atexit(restore_terminal);
    }
    

    // open KVM
    int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);

    if(kvm == -1)
        err(1, "KVM cannot be opened");

    // check for API version
    ret = ioctl(kvm, KVM_GET_API_VERSION, NULL);

    if (ret == -1)
	    err(1, "KVM_GET_API_VERSION");
    if (ret != 12)
	    errx(1, "KVM_GET_API_VERSION %d, expected 12", ret);

    // make a VM
    vmfd = ioctl(kvm, KVM_CREATE_VM, (unsigned long)0);

    if(vmfd == -1)
        err(1, "VM failed to be created");

    ret = ioctl(vmfd, KVM_SET_TSS_ADDR, 0xffffd000);

    uint64_t map_addr = 0xffffc000;
    ret = ioctl(vmfd, KVM_SET_IDENTITY_MAP_ADDR, &map_addr);


    ret = ioctl(vmfd, KVM_CREATE_IRQCHIP, 0);
    struct kvm_pit_config pit = { .flags = 0 };
    ret = ioctl(vmfd, KVM_CREATE_PIT2, &pit);
    
    // make a vCPU
    int vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, (unsigned long)0);

    if(vcpufd == -1)
        err(1, "vCPU failed to be created");

    // assign memory (512 MB) -- Step1
    void *mem = mmap(NULL, 0x20000000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if(mem == MAP_FAILED)
        err(1, "VM memory allocation failed");

    FILE *img = fopen("vmlinuz", "rb");

    if(img == NULL)
        err(1, "bzImage failed to open");

    // see the size of the bzImage

    fseek(img, 0, SEEK_END);
    long sz = ftell(img);

    // move to offset
    fseek(img, 0x5000, SEEK_SET);

    ret = fread(mem + 0x100000, 1, sz - 0x5000, img);
    if(1L * ret != sz - 0x5000)
        err(1, "fread read less than needed");


    FILE *initramfs = fopen("initramfs.cpio.gz", "rb");

    if(initramfs == NULL){
        err(1, "initramfs failed to open");
    }

    fseek(initramfs, 0, SEEK_END);
    sz = ftell(initramfs);

    fseek(initramfs, 0, SEEK_SET);

    ret = fread(mem + 0x10000000, 1, sz, initramfs);

    if(1L * ret != sz)
        err(1, "fread read less than needed - initramfs");

    // build the boot_params
    fseek(img, 0x1f1, SEEK_SET);
    
    ret = fread(mem + 0x101f1, 1, 0x7b, img);
    if(1L * ret != 0x7b)
        err(1, "fread read less than needed");

    // set up the boot_params fields

    // type_of_loader
    *(char *)(mem + 0x10210) = 0xFF;

    // cmd_line_ptr
    char *aux_str = (char *)(mem + 0x20000);
    strcpy(aux_str, "earlyprintk=serial,ttyS0 console=ttyS0");
    *(uint32_t *)(mem + 0x10228) = 0x20000;

    // set up gdt
    *(uint64_t *)(mem + 0x30000) = 0x0; // null
    *(uint64_t *)(mem + 0x30010) = 0x00cf9a000000ffff; // code
    *(uint64_t *)(mem + 0x30018) = 0x00cf92000000ffff; // data

    // set the e820

    *(uint64_t *)(mem + 0x102d0) = 0x0;          // addr
    *(uint64_t *)(mem + 0x102d8) = 0x20000000;   // size
    *(uint32_t *)(mem + 0x102e0) = 1;            // type
    *(uint8_t  *)(mem + 0x101e8) = 1;            // e820_entries = 1

    // tell the VM about its initramfs

    *(uint32_t *)(mem + 0x10218) = 0x10000000;
    *(uint32_t *)(mem + 0x1021c) = sz;

    ret = fclose(img);
    if(ret == -1)
        err(1, "Failed to close file");

    ret = fclose(initramfs);
    if(ret == -1){
        err(1, "Failed to close file");
    }
    
    // set up the VM struct
    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0x0, // start at guest physical address 0
        .memory_size = 0x20000000,
        .userspace_addr = (uint64_t)mem,
    };
    ret = ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);

    if(ret == -1)
        err(1, "VM struct fail");

    // get memory size
    int mmap_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, NULL);
    if(mmap_size == -1)
        err(1, "mmap_size");

    struct kvm_run *run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);
    if(run == NULL)
        err(1, "run mmap");

    // set the registers
    struct kvm_sregs sregs;

    ret = ioctl(vcpufd, KVM_GET_SREGS, &sregs);
    if(ret == -1)
        err(1, "KVM_GET_SREGS");

    memset(&sregs.cs, 0, sizeof(sregs.cs));
    sregs.cs.base = 0;
    sregs.cs.selector = 0x10;
    sregs.cs.type = 0xb;
    sregs.cs.s = 1;
    sregs.cs.present = 1;
    sregs.cs.db = 1;
    sregs.cs.g = 1;
    sregs.cs.dpl = 0;
    sregs.cs.limit = 0xffffffff;
    sregs.cs.l = 0;

    memset(&sregs.ds, 0, sizeof(sregs.ds));
    sregs.ds.selector = 0x18;
    sregs.ds.base = 0;
    sregs.ds.type = 0x3;
    sregs.ds.s = 1;
    sregs.ds.present = 1;
    sregs.ds.db = 1;
    sregs.ds.g = 1;
    sregs.ds.dpl = 0;
    sregs.ds.limit = 0xffffffff;
    sregs.ds.l = 0;

    memset(&sregs.ss, 0, sizeof(sregs.ss));
    sregs.ss.selector = 0x18;
    sregs.ss.base = 0;
    sregs.ss.type = 0x3;
    sregs.ss.s = 1;
    sregs.ss.present = 1;
    sregs.ss.db = 1;
    sregs.ss.g = 1;
    sregs.ss.dpl = 0;
    sregs.ss.limit = 0xffffffff;
    sregs.ss.l = 0;

    memset(&sregs.es, 0, sizeof(sregs.es));
    sregs.es.selector = 0x18;
    sregs.es.base = 0;
    sregs.es.type = 0x3;
    sregs.es.s = 1;
    sregs.es.present = 1;
    sregs.es.db = 1;
    sregs.es.g = 1;
    sregs.es.dpl = 0;
    sregs.es.limit = 0xffffffff;
    sregs.es.l = 0;
    
    sregs.cr0 |= 0x1;
    sregs.gdt.base = 0x30000;
    sregs.gdt.limit = 0x1f;
    ret = ioctl(vcpufd, KVM_SET_SREGS, &sregs);
    if(ret == -1)
        err(1, "KVM_SET_SREGS");

    struct kvm_cpuid2 *cpuid = calloc(1, sizeof(struct kvm_cpuid2) + 100 * sizeof(struct kvm_cpuid_entry2));
    cpuid->nent = 100;   // tell KVM: I gave you room for 100 entries
    ret = ioctl(kvm, KVM_GET_SUPPORTED_CPUID, cpuid);   // on the KVM fd, not vmfd!
    if (ret == -1) err(1, "KVM_GET_SUPPORTED_CPUID");
    // now cpuid->nent holds the real count, and cpuid->entries[] is filled

    ret = ioctl(vcpufd, KVM_SET_CPUID2, cpuid);   // on the VCPU fd!
    if (ret == -1) err(1, "KVM_SET_CPUID2");

    struct kvm_regs regs = {
    .rsi = 0x10000,
	.rip = 0x100000,
	.rflags = 0x2,
    .rbp = 0,
    .rdi = 0,
    .rbx = 0,
    };
    ret = ioctl(vcpufd, KVM_SET_REGS, &regs);
    if(ret == -1)
        err(1, "KVM_SET_REGS");


    // struct kvm_regs verify;
    // ioctl(vcpufd, KVM_GET_REGS, &verify);
    //fprintf(stderr, "before run: RIP=0x%llx\n", verify.rip);


    // right before the while loop
    // unsigned char *p = (unsigned char *)mem + 0x100000;
    //fprintf(stderr, "bytes at 0x100000: %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3]);
    // run the VM

    // int iter_counter = 0;
    struct kvm_regs r;
    struct kvm_sregs sr;

    // set up uart

    memset(&uart, 0, sizeof(uart));
    pthread_mutex_init(&uart.lock, NULL);
    uart.irq_level = -1;
    uart.vmfd = vmfd;

    // set up the input thread

    pthread_t input_thread;
    pthread_create(&input_thread, NULL, input_function, NULL);

    while(1){
        
        ret = ioctl(vcpufd, KVM_RUN, NULL);
        if(ret == -1) {
            // if the KVM_RUN is interrupted by the other thread
            if(errno == EINTR){
                continue;
            }
            err(1, "KVM_RUN");
        }

        switch(run->exit_reason){
            case KVM_EXIT_HLT:
                printf("Exit successful\r\n");
                
                ioctl(vcpufd, KVM_GET_REGS, &r);
                ioctl(vcpufd, KVM_GET_SREGS, &sr);
                // fprintf(stderr, "ON EXIT : RIP=0x%llx CS=0x%x CR0=0x%llx\n", r.rip, sr.cs.selector, sr.cr0);
                // printf("Number of iterations : %d\n", iter_counter);

                // in the HLT case, before returning:
                // unsigned char *hp = (unsigned char *)mem + r.rip;
                // fprintf(stderr, "bytes at halt RIP: %02x %02x %02x %02x\n", hp[0], hp[1], hp[2], hp[3]);

                return 0;
                break;
            case KVM_EXIT_IO:
                //fprintf(stderr, "IO port=0x%x dir=%d\n", run->io.port, run->io.direction);
                // output = *(unsigned char *)(((char *)run) + run->io.data_offset);

                // first check if it is in range

                if(run->io.port >= 0x3f8 && run->io.port <= 0x3ff){
                    // lock the mutex

                    pthread_mutex_lock(&uart.lock);


                    int reg = run->io.port - 0x3f8;
                    uint8_t *data = (uint8_t *)run + run->io.data_offset;
                    uint8_t dlab = uart.lcr & 0x80;

                    if(run->io.direction == KVM_EXIT_IO_IN){

                        switch(reg){
                            case 0:
                                if(dlab) *data = uart.dll;
                                else {
                                    *data = uart.rx_ready ? uart.rx_byte : 0;
                                    uart.rx_ready = 0;
                                    recompute_function();
                                }
                                break;
                            case 1:
                                if(dlab) *data = uart.dlm;
                                else *data = uart.ier;
                                break;
                            case 2:
                                if((uart.ier & 0x01) && uart.rx_ready) *data = 0x04;
                                else if((uart.ier & 0x02) && uart.thre_int) *data = 0x02, uart.thre_int = 0;
                                else *data = 0x01;
                                recompute_function();
                                break;
                            case 3:
                                *data = uart.lcr;
                                break;
                            case 4:
                                *data = uart.mcr;
                                break;
                            case 5:
                                *data = 0x60 | (uart.rx_ready ? 0x01 : 0);
                                break;
                            case 6:
                                *data = 0xb0;
                                break;
                            case 7:
                                *data = uart.scr;
                                break;
                        }

                    } else{

                        switch(reg){
                            case 0:
                                if(dlab) uart.dll = *data;
                                else {
                                    putchar(*data);
                                    fflush(stdout);
                                    uart.thre_int = 1;
                                    recompute_function();
                                }
                                break;
                            case 1:
                                if(dlab) uart.dlm = *data;
                                else {
                                    uint8_t old_ier = uart.ier;
                                    uart.ier = *data;
                                    if((old_ier & 0x02) == 0 && (uart.ier & 0x02)){
                                        uart.thre_int = 1;
                                    }
                                    recompute_function();
                                }
                                break;
                            case 2:
                                uart.fcr = *data;
                                break;
                            case 3:
                                uart.lcr = *data;
                                break;
                            case 4:
                                uart.mcr = *data;
                                recompute_function();
                                break;
                            case 7:
                                uart.scr = *data;
                                break;

                        }
                    }
                    // unlock the mutex

                    pthread_mutex_unlock(&uart.lock);
                }

                break;
            case KVM_EXIT_MMIO:
                if(!run->mmio.is_write){
                    *run->mmio.data = 0;
                }
                break;
            default:
                printf("unexpected exit reason: %d\r\n", run->exit_reason);
                ioctl(vcpufd, KVM_GET_REGS, &r);
                ioctl(vcpufd, KVM_GET_SREGS, &sr);
                // fprintf(stderr, "RIP=0x%llx CS=0x%x CR0=0x%llx\n", r.rip, sr.cs.selector, sr.cr0);
                // printf("Number of iterations : %d\n", iter_counter);
                
                return 1;
                break;
        }
        // iter_counter++;
    }

    // printf("Number of iterations : %d\n", iter_counter);


    return 0;
}