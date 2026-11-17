/* FOLIO v4 — fonte embutida (garantia de texto legível mesmo sem a pasta FOLIO/FONTES) */
#if defined(__APPLE__)
/* Mach-O (simulador macOS): todo símbolo C leva "_" e os dados constantes ficam em __TEXT,__const */
#define SYM(x) "_" #x
#define EMBED_SECTION ".section __TEXT,__const\n"
#elif defined(__i386__)
#define SYM(x) "_" #x
#define EMBED_SECTION ".section .rdata,\"dr\"\n"
#else
#define SYM(x) #x
#define EMBED_SECTION ".section .rdata,\"dr\"\n"
#endif
__asm__(
    EMBED_SECTION
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
