/* The built-in font: Noto Sans Regular (SIL Open Font License 1.1, see
 * ../font/OFL.txt), as outlines converted by tools/mkfont.py.  Read-only data
 * in the module's image. */
#ifndef SEL_FONT_FILE
#error "the Makefile passes SEL_FONT_FILE (the .glf path)"
#endif
__asm__(".section .rodata.selfont,\"a\",@progbits\n"
        ".balign 16\n"
        ".globl sel_font_glf\n"
        ".hidden sel_font_glf\n"
        "sel_font_glf:\n"
        ".incbin \"" SEL_FONT_FILE "\"\n"
        ".globl sel_font_glf_end\n"
        ".hidden sel_font_glf_end\n"
        "sel_font_glf_end:\n"
        ".previous\n");
