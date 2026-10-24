# Small console-side helper payloads (dev only: need a jailbroken console with elfldr on 9021).
PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
export LLVM_CONFIG ?= /opt/homebrew/opt/llvm@18/bin/llvm-config
PS5_HOST ?=
PS5_PORT ?= 9021

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS := -Wall -Wextra -Werror -O2
OUT := ../../build/payloads

ELF := $(OUT)/launch_cusa03492.elf $(OUT)/launch_cusa03474.elf $(OUT)/df.elf $(OUT)/unjail_swrr.elf $(OUT)/dumpmod.elf $(OUT)/kill_cusa03474.elf $(OUT)/kill_cusa03492.elf $(OUT)/savewatch_backup.elf $(OUT)/savewatch_install.elf $(OUT)/ps.elf \
	$(OUT)/launch_cusa07992.elf $(OUT)/kill_cusa07992.elf

all: $(ELF)

$(OUT)/launch_cusa03492.elf: launch_title.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DTITLE_ID='"CUSA03492"' -o $@ $^ -lSceSystemService -lSceUserService

$(OUT)/launch_cusa03474.elf: launch_title.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DTITLE_ID='"CUSA03474"' -o $@ $^ -lSceSystemService -lSceUserService

$(OUT)/df.elf: df.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $^

$(OUT)/unjail_swrr.elf: unjail.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $^

$(OUT)/dumpmod.elf: dumpmod.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $^

$(OUT)/savewatch_backup.elf: savewatch.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DMODE_BACKUP -o $@ $^

$(OUT)/savewatch_install.elf: savewatch.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DMODE_INSTALL -o $@ $^

$(OUT)/kill_cusa03474.elf: launch_title.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DKILL_ONLY -DTITLE_ID='"CUSA03474"' -o $@ $^ -lSceSystemService -lSceUserService

clean:
	rm -f $(ELF)

.PHONY: all clean

$(OUT)/ps.elf: ps.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $^

$(OUT)/kill_cusa03492.elf: launch_title.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DKILL_ONLY -DTITLE_ID='"CUSA03492"' -o $@ $^ -lSceSystemService -lSceUserService

# Jak X EU (UltraC0re's host)
$(OUT)/launch_cusa07992.elf: launch_title.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DTITLE_ID='"CUSA07992"' -o $@ $^ -lSceSystemService -lSceUserService

$(OUT)/kill_cusa07992.elf: launch_title.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DKILL_ONLY -DTITLE_ID='"CUSA07992"' -o $@ $^ -lSceSystemService -lSceUserService
