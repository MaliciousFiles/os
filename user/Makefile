# Builds every user test's /sbin/init: tN.dir/sbin/init from the sources in
# tN.dir/src, with the tiny libc (libc/, headers in libc/include) and the
# test helpers (testing/). The binaries are checked in, the kernel build
# doesn't run this. After changing one, remove build/tN.data so the disk
# image is made again.
#
# Statically linked, non-relocatable (ET_EXEC) x86-64 ELFs: what the
# kernel's ELF loader (kernel/elf.h) handles. Everything is compiled in one
# command, no object files or archives.

CC = gcc
# -fno-tree-loop-distribute-patterns: GCC would turn the loops in memcpy and
# memset into calls to themselves
CFLAGS = -O2 -Wall -Werror -ffreestanding -fno-pie -fno-stack-protector \
         -fcf-protection=none -fno-asynchronous-unwind-tables \
         -fno-tree-loop-distribute-patterns -Ilibc/include -Itesting
LDFLAGS = -static -nostdlib -no-pie -s -Wl,--build-id=none \
          -Wl,-z,noexecstack -Wl,-z,noseparate-code

LIBC = $(wildcard libc/*.c libc/*.S)
TESTING = $(wildcard testing/*.S)
HEADERS = $(wildcard libc/include/*.h libc/include/*/*.h testing/*.h)

# every test directory with a src/ in it
INITS = $(patsubst %/src,%/sbin/init,$(wildcard ../*.dir/src))

all: $(INITS)

.SECONDEXPANSION:
../%/sbin/init: $$(wildcard ../$$*/src/*.c) $(LIBC) $(TESTING) $(HEADERS) \
                Makefile
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(wildcard ../$*/src/*.c) $(LIBC) \
	      $(TESTING)

clean:
	rm -f $(INITS)

.PHONY: all clean
