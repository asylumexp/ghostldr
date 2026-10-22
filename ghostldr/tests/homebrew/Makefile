# hbprobe: ghostldr's test homebrew (hbprobe.c), built with the OpenOrbis PS4
# toolchain (not part of this repo: point OO_PS4_TOOLCHAIN at it).
#
#   make OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain [CREATE_FSELF=/path/to/create-fself]
#
# Two app0 trees in build/homebrew/:
#   hbprobe/app0       musl's malloc (as OpenOrbis builds it); no param.sfo, no sce_module
#   hbprobe-sys/app0   malloc from the system's libSceLibcInternal; a param.sfo (HBPR00002)
# Pack and install them like any title (docs/compatibility.md, "Homebrew").
LLVM ?= /opt/homebrew/opt/llvm@18/bin
OO_PS4_TOOLCHAIN ?= $(error set OO_PS4_TOOLCHAIN to the OpenOrbis PS4 toolchain)
CREATE_FSELF ?= $(OO_PS4_TOOLCHAIN)/bin/$(if $(filter Darwin,$(shell uname -s)),macos/create-fself-macos,linux/create-fself)
OUT ?= ../../../build/homebrew
TC := $(OO_PS4_TOOLCHAIN)
CFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wextra -Wno-unused-parameter \
	-isysroot $(TC) -isystem $(TC)/include
LDFLAGS := -m elf_x86_64 -pie --script $(TC)/link.x --eh-frame-hdr -L$(TC)/lib
LIBS := -lkernel -lSceVideoOut -lScePad -lSceUserService

all: $(OUT)/hbprobe/app0/eboot.bin $(OUT)/hbprobe-sys/app0/eboot.bin

$(OUT)/%/hbprobe.o: hbprobe.c
	@mkdir -p $(dir $@)
	$(LLVM)/clang $(CFLAGS) -c $< -o $@

# musl's malloc (libc.a) unless the system's libSceLibcInternal comes first
$(OUT)/hbprobe/hbprobe.elf: $(OUT)/hbprobe/hbprobe.o
	$(LLVM)/ld.lld $< -o $@ $(LDFLAGS) -lc $(LIBS) $(TC)/lib/crt1.o
$(OUT)/hbprobe-sys/hbprobe.elf: $(OUT)/hbprobe-sys/hbprobe.o
	$(LLVM)/ld.lld $< -o $@ $(LDFLAGS) -lSceLibcInternal -lc $(LIBS) $(TC)/lib/crt1.o

$(OUT)/%/app0/eboot.bin: $(OUT)/%/hbprobe.elf mkdata.py
	@mkdir -p $(dir $@)
	cd $(dir $<) && OO_PS4_TOOLCHAIN=$(abspath $(TC)) $(abspath $(CREATE_FSELF)) -in=hbprobe.elf -out=hbprobe.oelf \
		--eboot app0/eboot.bin --paid 0x3800000000000011
	python3 mkdata.py $(dir $@) $(if $(findstring -sys,$*),HBPR00002)

clean:
	rm -rf $(OUT)/hbprobe $(OUT)/hbprobe-sys

.PHONY: all clean
.SECONDARY:
