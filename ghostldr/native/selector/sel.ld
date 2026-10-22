/* The game selector module: one read-only, executable image linked at 0.
 * The core copies it into the emulator's JIT code buffer as is, so it must
 * have no writable sections and no relocations (tools/mksel.py checks):
 * all state lives on the stack and in the selector's own heap. */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(sel_main)

PHDRS { image PT_LOAD FLAGS(5); dyn PT_DYNAMIC; }

SECTIONS
{
    . = 0;
    .text : { *(.text.entry) *(.text .text.*) } :image
    .rodata : { *(.rodata .rodata.*) } :image
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :image
    .got : { *(.got .got.*) } :image
    .data : { *(.data .data.*) } :image
    .bss : { *(.bss .bss.* COMMON) } :image
    .rela.dyn : { *(.rela.dyn .rela.*) } :image
    .dynsym : { *(.dynsym) } :image
    .dynstr : { *(.dynstr) } :image
    .hash : { *(.hash) } :image
    .gnu.hash : { *(.gnu.hash) } :image
    .dynamic : { *(.dynamic) } :image :dyn
    . = ALIGN(16);
    __image_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "selector: writable static data is not allowed (keep state in struct sel)")
ASSERT(SIZEOF(.rela.dyn) == 0, "selector: no relocations allowed (no pointers in static data)")
