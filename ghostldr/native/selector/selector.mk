# The game selector, a runtime module (included by ../Makefile): selector/src
# plus its own copy of rt.c, linked into one read-only image (sel.ld) and
# packed by tools/mksel.py.  run.py serves build/ghostldr/selector.bin at boot.
SEL_SRCS := $(wildcard selector/src/*.c)
SEL_OBJS := $(patsubst selector/src/%.c,$(OUT)/sel/%.o,$(SEL_SRCS)) $(OUT)/sel/rt.o
SEL_FONT := selector/font/notosans.glf
SEL_CFLAGS := $(CFLAGS) -fno-math-errno -Iselector/src -DSEL_FONT_FILE='"$(abspath $(SEL_FONT))"'

$(OUT)/sel/%.o: selector/src/%.c selector/src/sel.h selector/src/plat.h $(HEADERS) include/sel_api.h
	@mkdir -p $(dir $@)
	$(CC) $(SEL_CFLAGS) -c $< -o $@

$(OUT)/sel/fontdata.o: $(SEL_FONT)

$(OUT)/sel/rt.o: src/rt.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# relink when a source file is added or removed (like core.objs and ext.objs)
$(OUT)/sel.objs: FORCE
	@mkdir -p $(dir $@)
	@echo '$(SEL_OBJS)' | cmp -s - $@ || echo '$(SEL_OBJS)' > $@

$(OUT)/selector.elf: $(SEL_OBJS) $(OUT)/sel.objs selector/sel.ld
	$(LD) $(LDFLAGS) -T selector/sel.ld -o $@ $(SEL_OBJS)

$(OUT)/selector.bin: $(OUT)/selector.elf ../tools/mksel.py
	python3 ../tools/mksel.py --abi $(ABI) $< $@
