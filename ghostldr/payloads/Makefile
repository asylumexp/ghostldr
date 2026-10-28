# Payloads for ghostldr's own ELF loader (docs/elfldr.md), into build/ghostldr/:
#
#   resign.elf   the console side of ghostldr.py resign (docs/save-resign.md)
#
#   make OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain [LLVM=/path/to/llvm/bin]
#
# Linked as the OpenOrbis samples are and sent as ld.lld writes them.  The
# extension's functions it calls (ghostldr_save_race_*) are left for the ELF
# loader to bind by name (-z undefs).
LLVM ?= /opt/homebrew/opt/llvm@18/bin
OUT ?= ../../build/ghostldr
TC = $(OO_PS4_TOOLCHAIN)
CFLAGS = --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wextra -Wno-parentheses -isysroot $(TC) \
	-isystem $(TC)/include
LDFLAGS = -m elf_x86_64 -pie --script $(TC)/link.x --eh-frame-hdr -L$(TC)/lib -z undefs

all: $(OUT)/resign.elf

$(OUT)/%.o: %.c
	@test -n "$(TC)" || { echo "set OO_PS4_TOOLCHAIN to the OpenOrbis PS4 toolchain" >&2; exit 1; }
	@mkdir -p $(OUT)
	$(LLVM)/clang $(CFLAGS) -c $< -o $@

$(OUT)/resign.elf: $(OUT)/resign.o
	$(LLVM)/ld.lld $< -o $@ $(LDFLAGS) -lc -lkernel -lSceUserService $(TC)/lib/crt1.o

clean:
	rm -f $(OUT)/resign.o $(OUT)/resign.elf

.PHONY: all clean
.SECONDARY:
