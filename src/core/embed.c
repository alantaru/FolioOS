/* FOLIO v4 — fonte embutida (garantia de texto legível mesmo sem a pasta FOLIO/FONTES) */
#if defined(__i386__)
#define SYM(x) "_" #x
#else
#define SYM(x) #x
#endif
__asm__(
    ".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".globl " SYM(embed_font_regular) "\n" SYM(embed_font_regular) ":\n"
    ".incbin \"assets/fonts/AtkinsonHyperlegible-Regular.ttf\"\n"
    ".globl " SYM(embed_font_regular_end) "\n" SYM(embed_font_regular_end) ":\n"
    ".balign 16\n"
    ".globl " SYM(embed_font_bold) "\n" SYM(embed_font_bold) ":\n"
    ".incbin \"assets/fonts/AtkinsonHyperlegible-Bold.ttf\"\n"
    ".globl " SYM(embed_font_bold_end) "\n" SYM(embed_font_bold_end) ":\n"
    ".text\n"
);
