# ghostldr core: freestanding x86-64 blob for the PS4 process inside SWRR.
# LLVM 18's bin directory (Homebrew's llvm@18 unless given: make LLVM=/path/to/llvm/bin)
LLVM ?= /opt/homebrew/opt/llvm@18/bin
export LLVM
CC := $(LLVM)/clang
LD := $(LLVM)/ld.lld
OBJCOPY := $(LLVM)/llvm-objcopy
OUT ?= ../../build/ghostldr

CFLAGS := --target=x86_64-pc-freebsd12-elf -O2 -g0 -std=gnu11 -Wall -Wextra -Werror \
	-Wno-unused-parameter -ffreestanding -fno-builtin -nostdlib -fPIC -fvisibility=hidden \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fcf-protection=none -mno-sse3 -mstackrealign -Iinclude

ifeq ($(PROBE),1)
CFLAGS += -DGHOSTLDR_PROBE
endif
SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,$(OUT)/obj/%.o,$(SRCS)) $(OUT)/obj/nids.o
APP0 ?= ../../build/ghost/app0

all: $(OUT)/core.bin

$(OUT)/obj/%.o: src/%.c include/ghost.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/gen/nids.c: ../tools/gen_nids.py ../tools/orbis_elf.py
	python3 ../tools/gen_nids.py $@ $(APP0)

$(OUT)/obj/nids.o: $(OUT)/gen/nids.c include/ghost.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/core.elf: $(OBJS) core.ld
	$(LD) -pie --no-dynamic-linker -z norelro -z notext --pack-dyn-relocs=none \
		-T core.ld -o $@ $(OBJS)

$(OUT)/core.bin: $(OUT)/core.elf
	$(OBJCOPY) -O binary $< $@
	@python3 ../tools/core_info.py $< $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
