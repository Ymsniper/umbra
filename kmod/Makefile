# Kbuild for suite_kmod.
#   make                build against the running kernel
#   make clean          remove build artifacts
#   sudo insmod suite_kmod.ko    load it   (creates /dev/suite_kmod)
#   sudo rmmod  suite_kmod       unload it
#
# The toolchain is detected, not asked for. A module has to be built with the
# same compiler as the kernel it loads into, and the kernel records which that
# was, so there is no reason to make you remember it. Override with
# `make LLVM=0` or `make LLVM=1` if the detection is ever wrong.
obj-m += suite_kmod.o

KVER := $(shell uname -r)
KDIR := /lib/modules/$(KVER)/build
PWD  := $(shell pwd)

# The kernel's own .config is authoritative; /proc/version is the fallback for
# trees that ship without one.
CC_IS_CLANG := $(shell grep -qs '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config && echo y)
ifneq ($(CC_IS_CLANG),y)
CC_IS_CLANG := $(shell grep -qs 'clang version' /proc/version && \
                       ! test -r $(KDIR)/.config && echo y)
endif

ifeq ($(CC_IS_CLANG),y)
LLVM ?= 1
export LLVM
TOOLCHAIN := clang (LLVM=$(LLVM))
else
TOOLCHAIN := gcc
endif

# Distro-appropriate hint when the headers are missing, rather than the kernel
# build system's rather opaque failure.
DISTRO := $(shell . /etc/os-release 2>/dev/null; echo $$ID $$ID_LIKE)
ifneq (,$(findstring arch,$(DISTRO)))
HDR_HINT := sudo pacman -S --needed linux-headers   # or your kernel's -headers package
else ifneq (,$(findstring debian,$(DISTRO)))
HDR_HINT := sudo apt install linux-headers-$(KVER)
else ifneq (,$(findstring ubuntu,$(DISTRO)))
HDR_HINT := sudo apt install linux-headers-$(KVER)
else ifneq (,$(findstring fedora,$(DISTRO)))
HDR_HINT := sudo dnf install kernel-devel-$(KVER)
else
HDR_HINT := install the kernel headers package for $(KVER)
endif

.PHONY: all clean check

all: check
	@echo "  kernel   $(KVER)"
	@echo "  toolchain $(TOOLCHAIN)"
	$(MAKE) -C $(KDIR) M=$(PWD) modules

check:
	@test -d "$(KDIR)" || { \
	  echo "kernel headers not found for $(KVER)"; \
	  echo "  looked in: $(KDIR)"; \
	  echo "  install:   $(HDR_HINT)"; \
	  exit 1; }

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
