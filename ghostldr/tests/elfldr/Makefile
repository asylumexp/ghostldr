# The ELF loader's sample payloads (docs/elfldr.md), into build/elfldr/:
#
#   hello.elf          OpenOrbis: printf, threads with TLS, malloc, exit code 7
#   echo.elf           OpenOrbis: a TCP echo server on port 9025
#   freestanding.elf   no libc: LLVM alone, everything through the entry's dlsym
#   fault.elf          no libc: writes to address 0 (the loader ends its thread)
#   hello.self         hello as create-fself makes it (make self): Orbis tables
#
#   make OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain [LLVM=/path/to/llvm/bin]
#   make freestanding          (needs no OpenOrbis toolchain)
#
# Then, with `run.py elfldr` serving on the console:
#
#   nc PS5 9021 < ../../../build/elfldr/hello.elf
#   python3 ../../host/elf_send.py ../../../build/elfldr/hello.elf
#
# The OpenOrbis ELFs are linked as the toolchain's samples are (link.x,
# crt1.o, -pie), and sent as they come out of ld.lld: no create-fself.
LLVM ?= /opt/homebrew/opt/llvm@18/bin
OUT ?= ../../../build/elfldr
TC = $(OO_PS4_TOOLCHAIN)
CREATE_FSELF ?= $(TC)/bin/$(if $(filter Darwin,$(shell uname -s)),macos/create-fself-macos,linux/create-fself)
OO_CFLAGS = --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wextra -isysroot $(TC) -isystem $(TC)/include
OO_LDFLAGS = -m elf_x86_64 -pie --script $(TC)/link.x --eh-frame-hdr -L$(TC)/lib
FREE_CFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -O2 -Wall -Wextra -ffreestanding -nostdlib -fno-builtin \
	-fno-stack-protector -fno-asynchronous-unwind-tables
FREE_LDFLAGS := -m elf_x86_64 -pie --no-dynamic-linker -e payload_main -z max-page-size=0x4000 -z separate-code

all: $(OUT)/hello.elf $(OUT)/echo.elf $(OUT)/freestanding.elf $(OUT)/fault.elf
freestanding: $(OUT)/freestanding.elf $(OUT)/fault.elf
self: $(OUT)/hello.self

$(OUT)/%.o: %.c
	@test -n "$(TC)" || { echo "set OO_PS4_TOOLCHAIN to the OpenOrbis PS4 toolchain (or: make freestanding)" >&2; exit 1; }
	@mkdir -p $(OUT)
	$(LLVM)/clang $(OO_CFLAGS) -c $< -o $@

$(OUT)/%.elf: $(OUT)/%.o
	$(LLVM)/ld.lld $< -o $@ $(OO_LDFLAGS) -lc -lkernel $(TC)/lib/crt1.o

$(OUT)/freestanding.elf $(OUT)/fault.elf: $(OUT)/%.elf: %.c
	@mkdir -p $(OUT)
	$(LLVM)/clang $(FREE_CFLAGS) -c $< -o $(OUT)/$*.o
	$(LLVM)/ld.lld $(OUT)/$*.o -o $@ $(FREE_LDFLAGS)

$(OUT)/hello.self: $(OUT)/hello.elf
	cd $(OUT) && OO_PS4_TOOLCHAIN=$(abspath $(TC)) $(abspath $(CREATE_FSELF)) -in=hello.elf -out=hello.oelf \
		--eboot hello.self --paid 0x3800000000000011

clean:
	rm -f $(addprefix $(OUT)/,hello.o hello.elf echo.o echo.elf freestanding.o freestanding.elf fault.o fault.elf \
		hello.oelf hello.self)

.PHONY: all freestanding self clean
.SECONDARY:
