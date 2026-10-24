# ghostldr core: freestanding x86-64 blob for the PS4 process inside SWRR,
# its runtime extension (ext/) and the game selector (selector/), both served
# by run.py at boot.
# LLVM 18's bin directory (Homebrew's llvm@18 unless given: make LLVM=/path/to/llvm/bin)
LLVM ?= /opt/homebrew/opt/llvm@18/bin
export LLVM
CC := $(LLVM)/clang
LD := $(LLVM)/ld.lld
OBJCOPY := $(LLVM)/llvm-objcopy
OUT ?= ../../build/ghostldr

# the extension must be built against the same headers as the core
HEADERS := include/ghost.h include/ghost_api.h
ABI := $(shell cat $(HEADERS) | cksum | cut -d' ' -f1)

CFLAGS := --target=x86_64-pc-freebsd12-elf -O2 -g0 -std=gnu11 -Wall -Wextra -Werror \
	-Wno-unused-parameter -ffreestanding -fno-builtin -nostdlib -fPIC -fvisibility=hidden \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fcf-protection=none -mno-sse3 -mstackrealign -Iinclude -DGHOST_ABI=$(ABI)u
LDFLAGS := -pie --no-dynamic-linker -z norelro -z notext --pack-dyn-relocs=none

ifeq ($(PROBE),1)
CFLAGS += -DGHOSTLDR_PROBE
endif
SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,$(OUT)/obj/%.o,$(SRCS))
# extension: ext/src/*.c (one file per HLE family) plus its own copy of rt.c
EXT_SRCS := $(wildcard ext/src/*.c)
# route B for titles on USB drives (docs/usb.md): the extension's own USB
# storage stack over libSceUsbd.  Off unless USB_RAW=1: SWRR's libSceUsbd
# lists no devices on a 13.60 PS5, and the drive is mounted instead (approot.c)
USB_RAW_SRCS := $(addprefix ext/src/,usb_bot.c blk.c part.c fsutil.c exfat.c fat32.c usbfs.c usb_sce.c)
ifeq ($(USB_RAW),1)
override EXT_CFLAGS += -DGHOST_USB_RAW
else
EXT_SRCS := $(filter-out $(USB_RAW_SRCS),$(EXT_SRCS))
endif
# rebuild the extension when EXT_CFLAGS change (USB_RAW=1 and back).  Done
# while reading this file, before make looks at any timestamp: make 3.81's
# are whole seconds, so a stamp-file prerequisite misses back-to-back builds.
ifneq ($(shell cat $(OUT)/ext.cflags 2>/dev/null),$(strip $(EXT_CFLAGS)))
$(shell mkdir -p $(OUT) && rm -rf $(OUT)/ext $(OUT)/ext.elf && echo '$(strip $(EXT_CFLAGS))' > $(OUT)/ext.cflags)
endif
EXT_OBJS := $(patsubst ext/src/%.c,$(OUT)/ext/%.o,$(EXT_SRCS)) $(OUT)/ext/rt.o
NAMES := ../../build/nid/known_names.txt
EXTRA_NAMES := $(wildcard ../data/extra_names.txt)

all: $(OUT)/core.bin $(OUT)/ext.bin $(OUT)/selector.bin $(OUT)/pump.bin

$(OUT)/obj/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# relink when a source file is added or removed, not only when one changes
$(OUT)/%.objs: FORCE
	@mkdir -p $(dir $@)
	@echo '$(if $(filter core,$*),$(OBJS),$(EXT_OBJS))' | cmp -s - $@ || \
		echo '$(if $(filter core,$*),$(OBJS),$(EXT_OBJS))' > $@

$(OUT)/core.elf: $(OBJS) $(OUT)/core.objs core.ld
	$(LD) $(LDFLAGS) -T core.ld -o $@ $(OBJS)

$(OUT)/core.bin: $(OUT)/core.elf
	$(OBJCOPY) -O binary $< $@
	@python3 ../tools/core_info.py $< $@

# EXT_CFLAGS: build switches for extension families (e.g. -DNPO_PERSONA_A=1)
$(OUT)/ext/%.o: ext/src/%.c $(wildcard ext/include/*.h) $(wildcard ext/src/*.h) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EXT_CFLAGS) -Iext/include -c $< -o $@

$(OUT)/ext/rt.o: src/rt.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/ext.elf: $(EXT_OBJS) $(OUT)/ext.objs ext/ext.ld
	$(LD) $(LDFLAGS) -T ext/ext.ld -o $@ $(EXT_OBJS)

$(OUT)/ext.bin: $(OUT)/ext.elf ../tools/mkext.py ../tools/elf64.py ../tools/gen_nids.py ../tools/orbis_elf.py \
		$(NAMES) $(EXTRA_NAMES)
	python3 ../tools/mkext.py --abi $(ABI) $< $@

$(NAMES):
	@echo "missing $@: run tools/fetch_deps.sh" >&2; exit 1

include selector/selector.mk

# the ghostfs installer's pump (pump/pump.c), a flat blob with no relocations.
# The installer sends the checked-in copy, ../data/gfs_pump.bin: `make pump`
# refreshes it (ghostldr/tests/test_gfs_pump.py checks that they match).
$(OUT)/pump/pump.o: pump/pump.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/pump/pump.elf: $(OUT)/pump/pump.o pump/pump.ld
	$(LD) -T pump/pump.ld -o $@ $<

$(OUT)/pump.bin: $(OUT)/pump/pump.elf
	$(OBJCOPY) -O binary $< $@

pump: $(OUT)/pump.bin
	install -m 644 $< ../data/gfs_pump.bin

clean:
	rm -rf $(OUT)

FORCE:

.PHONY: all clean pump FORCE
