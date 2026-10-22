/* ghostldr runtime extension: a flat PIC image relocated by the core (ext.c).
 * [code+rodata] at 0 goes in executable memory, RW data from the next 16 KiB
 * boundary (the core punches those pages RW).  tools/mkext.py turns the ELF
 * into build/ghostldr/ext.bin.  The dynamic-linking tables (.rela.dyn and
 * the symbol tables) sit after the image in a segment that is not loaded:
 * mkext.py reads the relocations from the section and the image does not
 * carry them (24 bytes per pointer the extension holds). */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(ext_init)

PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); dyn PT_DYNAMIC; meta PT_NULL; }

SECTIONS
{
    . = 0;
    .text : { *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    . = ALIGN(0x4000);
    __ext_data_start = .;
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :data
    /* one struct ext_family pointer per ext/src file (EXT_FAMILY in ext.h) */
    ext_families : { __ext_families_start = .; KEEP(*(ext_families)) __ext_families_end = .; } :data
    .data : { *(.data .data.*) } :data
    .got : { *(.got .got.*) } :data
    .dynamic : { *(.dynamic) } :data :dyn
    .bss : { *(.bss .bss.* COMMON) } :data
    . = ALIGN(0x4000);
    __ext_image_end = .;
    .rela.dyn : { *(.rela.dyn .rela.*) } :meta
    .dynsym : { *(.dynsym) } :meta
    .dynstr : { *(.dynstr) } :meta
    .hash : { *(.hash) } :meta
    .gnu.hash : { *(.gnu.hash) } :meta
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
