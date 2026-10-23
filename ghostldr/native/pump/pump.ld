/* the installer pump: code and rodata only, entry at 0 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(gfs_pump)

SECTIONS
{
    . = 0;
    .text : { *(.text.entry) *(.text .text.*) *(.rodata .rodata.*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
