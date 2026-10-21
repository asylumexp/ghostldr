/* Flat, self-relocating ghostldr core.
 * [code+rodata+rela] at 0 (goes in JIT RX memory), then RW data at a 16 KiB
 * aligned offset (normal RW memory mapped right after the RX block). */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(ghost_entry)

PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); dyn PT_DYNAMIC; }

SECTIONS
{
    . = 0;
    __image_start = .;
    .text : { *(.text.entry) *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    .rela.dyn : { __rela_start = .; *(.rela.dyn .rela.*) __rela_end = .; } :text
    .dynsym : { *(.dynsym) } :text
    .dynstr : { *(.dynstr) } :text
    .hash : { *(.hash) } :text
    .gnu.hash : { *(.gnu.hash) } :text
    __text_end = .;
    . = ALIGN(0x4000);
    __data_start = .;
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :data
    .data : { *(.data .data.*) } :data
    .got : { *(.got .got.*) } :data
    .dynamic : { *(.dynamic) } :data :dyn
    .bss : { *(.bss .bss.* COMMON) } :data
    . = ALIGN(16);
    __image_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
