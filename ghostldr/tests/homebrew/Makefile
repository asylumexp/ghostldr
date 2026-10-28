# hbprobe: ghostldr's test homebrew (hbprobe.c), built with the OpenOrbis PS4
# toolchain (not part of this repo: point OO_PS4_TOOLCHAIN at it).
#
#   make OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain [CREATE_FSELF=/path/to/create-fself]
#
# Three app0 trees in build/homebrew/:
#   hbprobe/app0       musl's malloc (as OpenOrbis builds it); no param.sfo, no sce_module
#   hbprobe-sys/app0   malloc from the system's libSceLibcInternal; a param.sfo (HBPR00002)
#   hbrel/app0         hbrel.c (HBRL00106): save data memory, its own prx (hbmod.c:
#                      sce_module/libhbmod.prx, imported, and libhbdyn.prx, loaded at
#                      run time) and Piglet's GLES2
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

all: $(OUT)/hbprobe/app0/eboot.bin $(OUT)/hbprobe-sys/app0/eboot.bin hbrel

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

# ---- hbrel and its two modules ----
HBREL := $(OUT)/hbrel
FSELF = OO_PS4_TOOLCHAIN=$(abspath $(TC)) $(abspath $(CREATE_FSELF))

hbrel: $(HBREL)/app0/eboot.bin $(HBREL)/app0/sce_module/libhbmod.prx $(HBREL)/app0/sce_module/libhbdyn.prx

$(HBREL)/hbmod.o: hbmod.c
	@mkdir -p $(dir $@)
	$(LLVM)/clang $(CFLAGS) -c $< -o $@
$(HBREL)/hbdyn.o: hbmod.c
	@mkdir -p $(dir $@)
	$(LLVM)/clang $(CFLAGS) -DHBDYN -c $< -o $@
$(HBREL)/hbrel.o: hbrel.c
	@mkdir -p $(dir $@)
	$(LLVM)/clang $(CFLAGS) -c $< -o $@
# what the executable and libhbdyn link against: libhbmod's symbols as a plain shared object
$(HBREL)/libhbmod.so: hbmod.c
	@mkdir -p $(dir $@)
	$(LLVM)/clang -target x86_64-pc-linux-gnu -shared -nostdlib -fPIC -fuse-ld=lld -Wl,-soname,libhbmod.so $< -o $@
$(HBREL)/libhbmod.elf: $(HBREL)/hbmod.o
	$(LLVM)/ld.lld $< -o $@ $(LDFLAGS) -lkernel $(TC)/lib/crtlib.o
$(HBREL)/libhbdyn.elf: $(HBREL)/hbdyn.o $(HBREL)/libhbmod.so
	$(LLVM)/ld.lld $< -o $@ $(LDFLAGS) -L$(HBREL) -lkernel -lhbmod $(TC)/lib/crtlib.o
$(HBREL)/hbrel.elf: $(HBREL)/hbrel.o $(HBREL)/libhbmod.so
	$(LLVM)/ld.lld $< -o $@ $(LDFLAGS) -L$(HBREL) -lc -lkernel -lSceSaveData -lScePigletv2VSH -lSceUserService \
		-lSceSystemService -lhbmod $(TC)/lib/crt1.o
$(HBREL)/app0/sce_module/%.prx: $(HBREL)/%.elf
	@mkdir -p $(dir $@)
	cd $(HBREL) && $(FSELF) -in=$*.elf -out=$*.oelf --lib=app0/sce_module/$*.prx --paid 0x3800000000000011 \
		--library-path=$(abspath $(HBREL))
$(HBREL)/app0/eboot.bin: $(HBREL)/hbrel.elf mkdata.py
	@mkdir -p $(dir $@)
	cd $(HBREL) && $(FSELF) -in=hbrel.elf -out=hbrel.oelf --eboot app0/eboot.bin --paid 0x3800000000000011 \
		--library-path=$(abspath $(HBREL))
	python3 mkdata.py $(HBREL)/app0 HBRL00106 --sfo-only --name hbrel

clean:
	rm -rf $(OUT)/hbprobe $(OUT)/hbprobe-sys $(HBREL)

.PHONY: all clean hbrel
.SECONDARY:
