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

/*
1. mov dx, 0x64 - BA 64 00
2. mov ax, 0xff - B8 ff 00
3. out dx, al   - EE
4. hlt          - 0xf4
*/

const uint8_t code[] = {
    0xba, 0x64, 0x00,
    0xb8, 0xff, 0x00,
    0xee,
    0xf4
};

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

    // make a vCPU
    int vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, (unsigned long)0);

    if(vcpufd == -1)
        err(1, "vCPU failed to be created");


    // assign memory
    void *mem = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if(mem == MAP_FAILED)
        err(1, "VM memory allocation failed");

    // put the code in memory
    memcpy(mem, code, sizeof(code));

    // set up the VM struct

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0x1000,
        .memory_size = 0x1000,
        .userspace_addr = (uint64_t)mem,
    };
    ret = ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);

    if(ret == -1)
        err(1, "VM struct fail");


    // get memory size
    int mmap_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, NULL);

    struct kvm_run *run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);

    // set the registers
    struct kvm_sregs sregs;

    ret = ioctl(vcpufd, KVM_GET_SREGS, &sregs);
    if(ret == -1)
        err(1, "KVM_GET_SREGS");
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    ret = ioctl(vcpufd, KVM_SET_SREGS, &sregs);
    if(ret == -1)
        err(1, "KVM_SET_SREGS");

    struct kvm_regs regs = {
	.rip = 0x1000,
	.rflags = 0x2,
    };
    ret = ioctl(vcpufd, KVM_SET_REGS, &regs);
    if(ret == -1)
        err(1, "KVM_SET_REGS");


    // run the VM
    int output;
    while(1){
        
        ret = ioctl(vcpufd, KVM_RUN, NULL);
        if(ret == -1)
            err(1, "KVM_RUN");

        switch(run->exit_reason){
            case KVM_EXIT_HLT:
                printf("Exit successful\n");
                return 0;
                break;
            case KVM_EXIT_IO:
                output = *(unsigned char *)(((char *)run) + run->io.data_offset);
                printf("OUTPUT : %d\n", output);
                break;
        }
    }
    return 0;
}