# SimpleHypervisor

A type-2 hypervisor written directly against the Linux **KVM API**, with no QEMU
and no libraries. It boots an unmodified Linux kernel to an interactive BusyBox
shell in about a second.

## Quick Overview

### What :

Two standalone programs, in increasing order of complexity:

- **Level1** : the smallest possible virtual machine. It creates a VM with one
vCPU and 4 KiB of memory, runs eight bytes of hand-assembled real-mode code that
writes to an I/O port and halts, and prints the value the guest sent.

- **Level2** : a complete VMM. It loads a real `bzImage` and an initramfs into
guest memory, hand-builds the structures the Linux boot protocol expects, puts
the vCPU straight into 32-bit protected mode, and emulates a 16550 UART so the
guest has a console. The result is a shell prompt in your terminal.

There is no disk and no networking. The guest's entire filesystem is the
initramfs, unpacked into RAM.

### Why :

To understand virtualization from the bottom up: what the hardware provides,
what the kernel provides through KVM, and what a virtual machine monitor has to
do itself. Everything that QEMU normally hides is written out explicitly here.

## Usage

### Prerequisites

- A CPU with virtualization support (Intel VT-x or AMD-V), enabled in firmware.

- Access to `/dev/kvm` :

```bash
sudo usermod -aG kvm $USER     # log out and back in afterwards
```

- Build tools and BusyBox for the initramfs :

```bash
sudo apt install build-essential cpio
```

### Level1

```bash
cd Level1
make
./main
```

Expected output :

```
OUTPUT : 255
Exit successful
```


### Level2

A kernel image named `vmlinuz` must be present in the `Level2` directory. It is
not committed to the repository because of its size. Any recent x86-64 bzImage
works, for example the one your distribution already ships :

```bash
cp /boot/vmlinuz-$(uname -r) Level2/vmlinuz
```

Then :

```bash
cd Level2
make
./main
```

`make` compiles the VMM and packs the `initramfs/` directory into
`initramfs.cpio.gz`. The kernel boots, mounts `/dev`, `/proc` and `/sys`, and
gives you a shell. Press `Ctrl-D` to exit the shell, and it respawns.

## Functionality

### The KVM model

KVM splits the work in two. The kernel puts the physical CPU into guest mode, so
guest instructions execute at native speed, and handles everything privileged.
Everything else, meaning memory, boot and devices, is this program's job.

The API is a hierarchy of three file descriptors, and every call goes to the
level that owns what it changes :

| Descriptor | Obtained by | Owns |
|---|---|---|
| system | `open("/dev/kvm")` | the subsystem, capability queries |
| VM | `ioctl(kvm, KVM_CREATE_VM)` | guest memory, interrupt controllers |
| vCPU | `ioctl(vmfd, KVM_CREATE_VCPU)` | registers, the run loop |

### Guest memory

512 MiB of anonymous memory is mapped in the host process and handed to the
guest as its physical RAM starting at guest address 0 :

```c
struct kvm_userspace_memory_region region = {
    .slot = 0,
    .guest_phys_addr = 0x0,
    .memory_size = 0x20000000,
    .userspace_addr = (uint64_t)mem,
};
ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);
```

After this registration, guest physical address `X` is simply `mem + X` in the
host, which is what makes the loader code below readable. KVM programs the
processor's second-level address translation (EPT on Intel, NPT on AMD) so that
guest physical accesses resolve into this mapping.

### The boot protocol

Linux does not start at an arbitrary entry point. It expects a specific machine
state, described in `Documentation/arch/x86/boot.rst`. Level2 builds that state
by hand :

- The protected-mode part of the `bzImage` is loaded at guest physical
`0x100000` (1 MiB), past the real-mode setup code at the start of the file.

- The initramfs archive is loaded at `0x10000000` (256 MiB).

- A `boot_params` structure ("the zero page") is assembled at `0x10000`. The
setup header is copied out of the kernel image itself, and then the fields the
bootloader is responsible for are filled in : `type_of_loader`, `cmd_line_ptr`,
`ramdisk_image` and `ramdisk_size`, and a one-entry `e820` memory map.

- The kernel command line, placed at `0x20000`, is
`earlyprintk=serial,ttyS0 console=ttyS0`, which puts the console on the
emulated serial port.

- A three-entry GDT (null, flat code, flat data) is written at `0x30000`, the
segment registers are set to match, and `CR0.PE` is set, so the vCPU begins in
32-bit protected mode with a flat address space.

- `RSI` points at `boot_params` and `RIP` at `0x100000`, as the 32-bit boot
entry requires.

### The run loop

```c
while (1) {
    ioctl(vcpufd, KVM_RUN, NULL);
    switch (run->exit_reason) { ... }
}
```

`KVM_RUN` executes the guest until it does something that cannot complete
without help, then returns. The reason, and any associated data, arrive in a
`struct kvm_run` shared page mapped from the vCPU descriptor. This is
trap-and-emulate: run natively, trap rarely, emulate, resume.

### The emulated 16550 UART

The guest has no display and no keyboard, so the console is a serial port. A
16550-compatible UART is emulated at the standard COM1 address `0x3f8`, wired to
ISA interrupt line 4.

A device model is a function that answers reads and writes the way the real chip
would. When the guest executes `out` or `in` on ports `0x3f8` to `0x3ff`, the
vCPU exits with `KVM_EXIT_IO`, and the handler services the access using the
offset, the direction, and the data pointer found in the shared page.

Registers implemented :

| Offset | Read | Write |
|---|---|---|
| 0 | received byte, or divisor low when DLAB is set | transmit byte, or divisor low |
| 1 | interrupt enable, or divisor high | interrupt enable, or divisor high |
| 2 | interrupt identification | FIFO control (stored, not modelled) |
| 3 | line control | line control, bit 7 is DLAB |
| 4 | modem control | modem control, bit 3 is OUT2 |
| 5 | line status | read-only |
| 6 | modem status | read-only |
| 7 | scratch | scratch |

Transmission is immediate: a write to the data register calls `putchar` and
flushes, so the line status register permanently reports an empty transmitter
and the guest never stalls.

Interrupts are recomputed after every state change. The line is asserted only
when OUT2 connects it to the bus and an enabled condition holds, either a byte
waiting to be received or the transmitter newly free, and `KVM_IRQ_LINE` is
issued only when the level actually changes. The interrupt controllers
themselves are emulated inside the kernel, requested with `KVM_CREATE_IRQCHIP`,
alongside a `KVM_CREATE_PIT2` timer that the guest expects to find.

Host input runs on its own thread, because `KVM_RUN` blocks while the guest
executes. The thread reads standard input one byte at a time, stores it, and
raises the interrupt. A mutex protects the device state, which is now shared
between the vCPU thread and the input thread. The host terminal is put in raw
mode with `cfmakeraw` so keystrokes reach the guest immediately, and an `atexit`
handler restores the original settings.

## Known limitations

These are deliberate simplifications, not oversights :

- The offset of the protected-mode kernel inside the `bzImage` is hardcoded to
`0x5000` rather than computed from `setup_sects` at offset `0x1f1`, so kernels
with a differently sized setup area will not boot.

- The `e820` map reports one usable region covering all 512 MiB, including the
legacy hole below 1 MiB that a real BIOS marks as reserved.

- One vCPU only. There are no MP tables and no ACPI, so the guest never
discovers the I/O APIC and runs in legacy PIC mode.

- The UART has no FIFOs and a single byte of receive buffer, so the input thread
applies back-pressure by polling. There is no modelling of overrun, parity or
framing errors.

- I/O ports outside the UART range are ignored, leaving stale data in the shared
page where real hardware would return `0xff`.

## Acknowledgments

### Sources

- The Linux kernel documentation : `Documentation/virt/kvm/api.rst` and
`Documentation/arch/x86/boot.rst`.

- Josh Triplett, "Using the KVM API", LWN.net.

- The TI PC16550D datasheet.

- Intel 64 and IA-32 Architectures Software Developer's Manual, volume 3.

### AI USE

The VMM, the boot protocol implementation and the UART device model are written
by me. Earlier in the project I used an AI assistant as a tutor, to explain
specifications and point at the relevant documentation rather than to produce
code. One earlier revision of the UART was AI-generated; I removed it and
rewrote the device myself so that every line here is one I can explain.
