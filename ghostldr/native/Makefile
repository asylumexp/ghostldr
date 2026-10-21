# ghostldr core: freestanding x86-64 blob for the PS4 process inside SWRR,
# and its runtime extension (ext/, served by run.py at boot).
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
EXT_OBJS := $(patsubst ext/src/%.c,$(OUT)/ext/%.o,$(EXT_SRCS)) $(OUT)/ext/rt.o
NAMES := ../../build/nid/known_names.txt
EXTRA_NAMES := $(wildcard ../data/extra_names.txt)

all: $(OUT)/core.bin $(OUT)/ext.bin

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

$(OUT)/ext/%.o: ext/src/%.c $(wildcard ext/include/*.h) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iext/include -c $< -o $@

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

clean:
	rm -rf $(OUT)

FORCE:

.PHONY: all clean FORCE
