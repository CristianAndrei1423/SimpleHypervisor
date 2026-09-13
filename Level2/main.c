#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/* --- COM1 16550 UART, wired to ISA IRQ 4, single-byte receive --- */

#define COM1_BASE 0x3f8
#define COM1_IRQ  4

struct uart {
    pthread_mutex_t lock;
    int vmfd;

    uint8_t ier;   /* 0x3f9 interrupt enable   */
    uint8_t lcr;   /* 0x3fb line control (b7=DLAB) */
    uint8_t mcr;   /* 0x3fc modem control (b3=OUT2) */
    uint8_t fcr;   /* FIFO control (write of 0x3fa) */
    uint8_t dll;   /* divisor latch low  (0x3f8, DLAB=1) */
    uint8_t dlm;   /* divisor latch high (0x3f9, DLAB=1) */

    uint8_t rx_byte;   /* one pending input byte */
    int rx_ready;      /* is rx_byte valid? */

    int thre_int;      /* TX-holding-empty interrupt latched */
    int irq_level;     /* last level driven on IRQ 4 */
};

static struct uart uart;

/* Drive IRQ 4 to match the UART's interrupt state. Call with lock held. */
static void uart_update_irq(struct uart *u) {
    int assert = 0;
    if (u->mcr & 0x08) {                            /* OUT2 gates INTR to the bus */
        if ((u->ier & 0x01) && u->rx_ready)        assert = 1;  /* RX data ready */
        else if ((u->ier & 0x02) && u->thre_int)   assert = 1;  /* TX buffer empty */
    }
    if (assert != u->irq_level) {
        u->irq_level = assert;
        struct kvm_irq_level lvl = { .irq = COM1_IRQ, .level = assert };
        ioctl(u->vmfd, KVM_IRQ_LINE, &lvl);
    }
}

/* Handle one KVM_EXIT_IO to a COM1 register (0x3f8..0x3ff). */
static void uart_io(struct uart *u, struct kvm_run *run) {
    uint8_t *data = (uint8_t *)run + run->io.data_offset;
    int reg  = run->io.port - COM1_BASE;
    int dlab = u->lcr & 0x80;

    pthread_mutex_lock(&u->lock);

    if (run->io.direction == KVM_EXIT_IO_IN) {          /* guest reads */
        uint8_t val = 0;
        switch (reg) {
        case 0:                                         /* RBR / DLL */
            if (dlab) val = u->dll;
            else {
                val = u->rx_ready ? u->rx_byte : 0;
                u->rx_ready = 0;
                uart_update_irq(u);
            }
            break;
        case 1: val = dlab ? u->dlm : u->ier; break;    /* IER / DLM */
        case 2:                                         /* IIR: why interrupted */
            if ((u->ier & 0x01) && u->rx_ready)         val = 0x04;
            else if ((u->ier & 0x02) && u->thre_int) {  val = 0x02; u->thre_int = 0; }
            else                                        val = 0x01;
            uart_update_irq(u);
            break;
        case 3: val = u->lcr; break;
        case 4: val = u->mcr; break;
        case 5: val = 0x60 | (u->rx_ready ? 0x01 : 0); break; /* LSR: THRE|TEMT|DR */
        case 6: val = 0xb0; break;                      /* MSR: DCD|DSR|CTS */
        default: val = 0; break;
        }
        *data = val;
    } else {                                            /* guest writes */
        uint8_t val = *data;
        switch (reg) {
        case 0:                                         /* THR / DLL */
            if (dlab) u->dll = val;
            else { putchar(val); fflush(stdout); u->thre_int = 1; uart_update_irq(u); }
            break;
        case 1:                                         /* IER / DLM */
            if (dlab) u->dlm = val;
            else {
                uint8_t old = u->ier; u->ier = val;
                if ((val & 0x02) && !(old & 0x02)) u->thre_int = 1;
                uart_update_irq(u);
            }
            break;
        case 2: u->fcr = val; break;                    /* FCR */
        case 3: u->lcr = val; break;                    /* LCR */
        case 4: u->mcr = val; uart_update_irq(u); break;/* MCR (OUT2 may change) */
        default: break;                                 /* LSR/MSR read-only, etc. */
        }
    }

    pthread_mutex_unlock(&u->lock);
}

/* Blocks on stdin; each byte becomes a guest RX byte + IRQ 4.
   Runs on its own thread because the vCPU thread is parked in KVM_RUN. */
static void *input_thread(void *arg) {
    struct uart *u = arg;
    uint8_t c;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == 0) break;                              /* EOF */
        if (n < 0) { if (errno == EINTR) continue; break; }

        for (;;) {                                      /* wait for guest to take prev byte */
            pthread_mutex_lock(&u->lock);
            if (!u->rx_ready) {
                u->rx_byte = c;
                u->rx_ready = 1;
                uart_update_irq(u);
                pthread_mutex_unlock(&u->lock);
                break;
            }
            pthread_mutex_unlock(&u->lock);
            usleep(200);
        }
    }
    return NULL;
}

/* --- host terminal raw mode so keystrokes reach the guest --- */

static struct termios orig_termios;
static int termios_saved;

static void restore_termios(void) {
    if (termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

static void setup_termios(void) {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &orig_termios) == 0) {
        termios_saved = 1;
        struct termios raw = orig_termios;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        atexit(restore_termios);
    }
}

int main(void) {

    // open KVM
    int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);

    if(kvm == -1)
        err(1, "KVM cannot be opened");

    // check for API version
    int ret = ioctl(kvm, KVM_GET_API_VERSION, NULL);

    if (ret == -1)
	    err(1, "KVM_GET_API_VERSION");
    if (ret != 12)
	    errx(1, "KVM_GET_API_VERSION %d, expected 12", ret);

    // make a VM
    int vmfd = ioctl(kvm, KVM_CREATE_VM, (unsigned long)0);

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
    cpuid->nent = 100;
    ret = ioctl(kvm, KVM_GET_SUPPORTED_CPUID, cpuid);   // on the KVM fd, not vmfd!
    if (ret == -1) err(1, "KVM_GET_SUPPORTED_CPUID");

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

    /* --- serial: init UART state, raw terminal, spawn input thread --- */
    memset(&uart, 0, sizeof(uart));
    pthread_mutex_init(&uart.lock, NULL);
    uart.vmfd = vmfd;
    uart.irq_level = -1;             /* force first KVM_IRQ_LINE to take effect */

    setup_termios();
    signal(SIGPIPE, SIG_IGN);

    pthread_t tid;
    pthread_create(&tid, NULL, input_thread, &uart);

    // run the VM
    struct kvm_regs r;
    struct kvm_sregs sr;

    while(1){

        ret = ioctl(vcpufd, KVM_RUN, NULL);
        if(ret == -1){
            if(errno == EINTR) continue;
            err(1, "KVM_RUN");
        }

        switch(run->exit_reason){
            case KVM_EXIT_HLT:
                ioctl(vcpufd, KVM_GET_REGS, &r);
                ioctl(vcpufd, KVM_GET_SREGS, &sr);
                fprintf(stderr, "\r\nHLT: RIP=0x%llx CS=0x%x CR0=0x%llx\r\n",
                        r.rip, sr.cs.selector, sr.cr0);
                return 0;
            case KVM_EXIT_IO:
                if(run->io.port >= COM1_BASE && run->io.port <= COM1_BASE + 7)
                    uart_io(&uart, run);
                break;
            case KVM_EXIT_MMIO:
                if(!run->mmio.is_write)
                    *run->mmio.data = 0;
                break;
            default:
                fprintf(stderr, "\r\nunexpected exit reason: %d\r\n", run->exit_reason);
                ioctl(vcpufd, KVM_GET_REGS, &r);
                ioctl(vcpufd, KVM_GET_SREGS, &sr);
                fprintf(stderr, "RIP=0x%llx CS=0x%x CR0=0x%llx\r\n",
                        r.rip, sr.cs.selector, sr.cr0);
                return 1;
        }
    }

    return 0;
}
