SRCS=main.c

# Every file in the initramfs tree: if any changes, the archive rebuilds.
INITRAMFS_FILES=$(shell find initramfs -type f)

all: main initramfs.cpio.gz

main: $(SRCS)
	gcc $(SRCS) -o main -pthread

# Pack the initramfs tree into the gzip'd cpio archive main.c loads.
# cpio must run from inside the tree so paths are relative (./bin, ./init ...).
initramfs.cpio.gz: $(INITRAMFS_FILES)
	cd initramfs && find . | cpio -o -H newc 2>/dev/null | gzip > ../initramfs.cpio.gz

clean:
	rm -f main initramfs.cpio.gz
