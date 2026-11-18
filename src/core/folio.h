/*
 * FOLIO v4 — Máquina de escrita digital bare-metal.
 * Cabeçalho central do núcleo independente de plataforma.
 *
 * O núcleo não depende de SO nem de libc: tudo que toca hardware passa
 * pela PAL (Platform Abstraction Layer) declarada abaixo, implementada em
 * src/platform/uefi (máquina real) e src/platform/win32 (simulador).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

typedef uint8_t  u8;  typedef int8_t  i8;
typedef uint16_t u16; typedef int16_t i16;
typedef uint32_t u32; typedef int32_t i32;
typedef uint64_t u64; typedef int64_t i64;
typedef size_t usize;

#define FOLIO_VERSION "4.3"
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define CLAMP(v,lo,hi) ((v)<(lo)?(lo):((v)>(hi)?(hi):(v)))
#define COUNT(a) ((int)(sizeof(a)/sizeof((a)[0])))
#define UNUSED(x) ((void)(x))
static inline int iabs(int x) { return x < 0 ? -x : x; }

/* ======================================================================
 * base.c — memória, strings, formatação, matemática
 * ====================================================================== */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

void  heap_init(void *mem, usize size);
void *mem_alloc(usize n);
void *mem_zalloc(usize n);
void *mem_realloc(void *p, usize n);
void  mem_free(void *p);
usize heap_used(void);
usize heap_total(void);

int   str_len(const char *s);
bool  str_eq(const char *a, const char *b);
bool  str_ieq(const char *a, const char *b);
int   str_cmp_i(const char *a, const char *b);       /* natural, case-insensitive */
void  str_copy(char *d, const char *s, int cap);
void  str_cat(char *d, const char *s, int cap);
bool  str_starts(const char *s, const char *pre);
bool  str_ends_i(const char *s, const char *suf);
char *str_dup(const char *s);
char *str_ndup(const char *s, int n);
int   str_find_i(const char *hay, int hlen, const char *needle, int nlen, int from);
int   str_to_int(const char *s);
int   fmt(char *buf, int cap, const char *f, ...);
int   vfmt(char *buf, int cap, const char *f, va_list ap);
/* registro de depuração pela porta serial (debug_serial=1 no CONFIG.INI; o laboratório QEMU grava num arquivo) */
extern bool dbg_on;
void  pal_log(const char *s);
void  dbg(const char *f, ...);
extern char fmt_decimal;              /* separador decimal de %f (do idioma) */
const char *path_basename(const char *p);
void  path_join(char *out, int cap, const char *dir, const char *name);
void  path_parent(char *p);
const char *path_ext(const char *p);   /* aponta para ".ext" ou "" */

float f_sqrt(float x);
float f_floor(float x);
float f_ceil(float x);
float f_abs(float x);
float f_round(float x);
float f_pow(float x, float y);
float f_sin(float x);
float f_exp(float x);
u32   rng_next(void);
float ease_out(float t);

/* UTF-8 */
int   utf8_decode(const char *s, int len, int i, u32 *cp);   /* retorna bytes consumidos (>=1) */
int   utf8_encode(u32 cp, char *out);
int   utf8_prev(const char *s, int i);
int   utf8_next(const char *s, int len, int i);
u32   cp_lower(u32 cp);
u32   cp_fold(u32 cp);   /* minúscula + sem acento (busca) */
bool  cp_is_word(u32 cp);
bool  cp_is_space(u32 cp);

/* ======================================================================
 * Tipos gráficos
 * ====================================================================== */
typedef struct { int x, y, w, h; } Rect;
typedef u32 Color;   /* 0xAARRGGBB */
#define RGB(c)        (0xFF000000u | (u32)(c))
#define ALPHA(c,a)    (((u32)(c) & 0x00FFFFFFu) | ((u32)(a) << 24))
static inline Rect rect(int x, int y, int w, int h) { Rect r = {x, y, w, h}; return r; }
static inline bool rect_has(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static inline Rect rect_inset(Rect r, int d) { return rect(r.x + d, r.y + d, r.w - 2*d, r.h - 2*d); }
Rect  rect_clip(Rect a, Rect b);

typedef struct { u32 *px; int w, h, stride; } Surface;

/* ======================================================================
 * PAL — Platform Abstraction Layer
 * ====================================================================== */
enum {
    KEY_ESC = 0x110000, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_DELETE, KEY_INSERT,
    KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12
};
enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4, MOD_ALTGR = 8 };
enum { EV_NONE, EV_KEY, EV_MOVE, EV_DOWN, EV_UP, EV_WHEEL, EV_QUIT };

typedef struct {
    int  type;
    int  key;        /* KEY_* para teclas especiais */
    u32  ch;         /* codepoint (texto) — com CTRL vem a letra minúscula */
    int  mods;
    int  x, y;       /* posição do ponteiro */
    int  button;     /* 0 esq, 1 dir, 2 meio */
    int  wheel;      /* +1 = rolar para cima */
    bool us_raw;     /* caractere segundo layout US do firmware (precisa remapear) */
} Event;

typedef struct { int year, month, day, hour, min, sec; } DateTime;
typedef struct { char label[64]; bool boot; bool readonly; u64 size, free; } VolInfo;
typedef struct { char name[256]; bool dir; u64 size; DateTime mtime; } FileEntry;
typedef struct PalFile PalFile;

Surface    *pal_screen(void);
void        pal_present(Rect r);
bool        pal_poll(Event *e);
void        pal_wait(int ms);
u64         pal_ms(void);
void        pal_now(DateTime *t);
const char *pal_name(void);
void        pal_power(int action);      /* 0 = desligar, 1 = reiniciar */
void        pal_set_pointer_speed(float s);

int   pal_vol_count(void);
bool  pal_vol_info(int vol, VolInfo *out);
int   pal_list(int vol, const char *path, FileEntry **out);   /* -1 erro */
bool  pal_read(int vol, const char *path, u8 **data, usize *len);
bool  pal_write(int vol, const char *path, const void *data, usize len);
bool  pal_mkdir(int vol, const char *path);
bool  pal_remove(int vol, const char *path);
bool  pal_rename(int vol, const char *path, const char *new_name);
bool  pal_exists(int vol, const char *path, bool *is_dir);
PalFile *pal_fopen(int vol, const char *path, u64 *size);
int   pal_fread(PalFile *f, void *buf, int n);
bool  pal_fseek(PalFile *f, u64 pos);
void  pal_fclose(PalFile *f);

bool  pal_audio_start(int rate);          /* PCM estéreo s16 */
int   pal_audio_space(void);              /* quadros que podem ser escritos agora */
void  pal_audio_write(const i16 *frames, int n);
void  pal_audio_stop(void);
const char *pal_audio_info(void);

/* ======================================================================
 * gfx.c — rasterização 2D com anti-aliasing
 * ====================================================================== */
void  gfx_target(Surface *s);
Surface *gfx_surface(void);
void  gfx_clip(Rect r);
Rect  gfx_get_clip(void);
void  gfx_push_clip(Rect r);
void  gfx_pop_clip(void);
void  gfx_set_opacity(float a);         /* multiplicador global */
Color col_mix(Color a, Color b, float t);
Color col_alpha(Color c, float a);
Color col_lum_shift(Color c, float amount);
float col_luma(Color c);
void  gfx_fill(Color c);
void  gfx_rect(Rect r, Color c);
void  gfx_rrect(Rect r, float rad, Color c);
void  gfx_rrect_line(Rect r, float rad, float th, Color c);
void  gfx_shadow(Rect r, float rad, float blur, Color c);
void  gfx_gradient_v(Rect r, Color top, Color bottom);
void  gfx_vignette(Rect r, Color c, float strength);
void  gfx_mask(int x, int y, const u8 *m, int w, int h, int pitch, Color c);
void  gfx_circle(float cx, float cy, float rad, Color c);
void  gfx_ring(float cx, float cy, float rad, float th, Color c);
void  gfx_line(float x0, float y0, float x1, float y1, float th, Color c);
void  gfx_arc(float cx, float cy, float rad, float th, float frac, Color c);

/* rasterizador de contornos (fontes e ícones vetoriais) */
typedef struct { float *acc; int w, h; float ox, oy, sx, sy, shear; } Raster;
void  raster_begin(Raster *r, int w, int h);
void  raster_line(Raster *r, float x0, float y0, float x1, float y1);
void  raster_quad(Raster *r, float x0, float y0, float x1, float y1, float x2, float y2);
void  raster_cubic(Raster *r, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3);
u8   *raster_end(Raster *r);   /* devolve máscara w*h alocada */

/* ======================================================================
 * font.c — TrueType (glyf) + kerning GPOS/kern + cache de glifos
 * ====================================================================== */
typedef struct Face Face;
typedef struct {
    u8   *bmp;
    i16   w, h, ox, oy;    /* deslocamento do canto sup. esq. a partir da origem/baseline */
} Glyph;

Face  *face_load(const u8 *data, usize len, bool owned);
Face  *face_variant(Face *base, bool synth_bold, bool synth_italic);
void   face_names(Face *f, char *family, int fcap, char *style, int scap);
int    face_weight(Face *f);
bool   face_is_italic(Face *f);
bool   face_is_mono(Face *f);
u16    face_gid(Face *f, u32 cp);
float  face_adv(Face *f, u16 gid, float px);
float  face_kern(Face *f, u16 a, u16 b, float px);
float  face_ascent(Face *f, float px);
float  face_descent(Face *f, float px);   /* positivo */
float  face_xheight(Face *f, float px);
const Glyph *face_glyph(Face *f, u16 gid, float px, int subpx);
void   glyph_cache_trim(void);

enum { FS_REG = 0, FS_BOLD = 1, FS_ITALIC = 2, FS_BOLDITALIC = 3 };
typedef struct {
    char  name[48];
    Face *f[4];
    bool  mono;
    bool  embedded;
} Family;

void    fonts_init(void);
void    fonts_scan(int vol, const char *dir);
int     font_count(void);
Family *font_get(int i);
int     font_find(const char *name);
Family *font_ui(void);
void    font_set_ui(int i);        /* fonte da interface escolhida pelo tema (-1 = padrão) */
Family *font_mono(void);
Face   *fam_face(Family *f, int style);

float  text_width(Face *f, float px, const char *s, int len);
float  draw_text(Face *f, float px, float x, float baseline, const char *s, int len, Color c);
float  draw_text_c(Face *f, float px, Rect r, const char *s, Color c);  /* centralizado */
int    text_fit(Face *f, float px, const char *s, int len, float maxw);  /* bytes que cabem */
void   draw_text_ellipsis(Face *f, float px, float x, float baseline, float maxw, const char *s, Color c);

/* ======================================================================
 * icons.c — ícones vetoriais desenhados com o rasterizador
 * ====================================================================== */
enum {
    IC_MENU, IC_FILE, IC_FOLDER, IC_SAVE, IC_EXPORT, IC_EYE, IC_FOCUS, IC_PALETTE, IC_MUSIC,
    IC_SETTINGS, IC_CLOSE, IC_PLAY, IC_PAUSE, IC_NEXT, IC_PREV, IC_SEARCH, IC_LIST, IC_TIMER,
    IC_UP, IC_PLUS, IC_TRASH, IC_EDIT, IC_DRIVE, IC_CHECK, IC_SHUFFLE, IC_REPEAT, IC_VOLUME,
    IC_HELP, IC_POWER, IC_TYPEWRITER, IC_DOC, IC_CHART, IC_BACK, IC_COMMAND, IC_CHEVRON,
    IC_HOME, IC_MOON, IC_STAR, IC_FWD, IC_TABS, IC_COUNT
};
void icon_draw(int icon, float cx, float cy, float size, Color c);
void cursor_draw(int kind, int x, int y, float scale);   /* 0 seta, 1 I-beam, 2 mão */

/* ======================================================================
 * theme.c
 * ====================================================================== */
typedef struct {
    const char *name;
    bool  dark;
    Color desk, paper, text, muted, faint, accent, select, caret;
    Color chrome, chrome_text, border, panel, panel_text, heading, code_bg, mark;
    bool  paper_sheet;    /* desenha "folha" sobre a mesa */
    float grain;          /* vinheta suave da mesa */
    const char *font_text, *font_ui;   /* temas do usuário: fontes que acompanham o tema (0 = não muda) */
    bool  custom;
} Theme;
#define THEME_NCOLORS 16           /* desk .. mark, em sequência na struct */
#define MAX_CUSTOM_THEMES 32
int          theme_count(void);
int          theme_builtin_count(void);
const Theme *theme_get(int i);
int          theme_find(const char *name);
void         themes_scan(int vol);                       /* FOLIO\TEMAS\*.INI */
int          theme_save_custom(const Theme *t, const char *name, const char *ftext, const char *fui, const char *old_name);
bool         theme_delete_custom(int i);
Color       *theme_color(Theme *t, int k);
const char  *theme_color_key(int k);
bool         color_parse(const char *s, Color *out);     /* "#RRGGBB" */
void         color_to_hsl(Color c, float *h, float *s, float *l);
Color        color_from_hsl(float h, float s, float l);
extern const Theme *T;   /* tema ativo */

/* ======================================================================
 * doc.c — documento (array de parágrafos) com desfazer/refazer
 * ====================================================================== */
typedef struct { int p, o; } Pos;
typedef struct { int start, end; float w; } VLine;

typedef struct {
    char  *s; int len, cap;
    /* cache de layout */
    u32    lay_key;
    VLine *lines; int nlines, lcap;
    u8    *sty;  int scap;
    float  height, lh, size;
    u8     kind, level, in_code, hang;
    float  hang_w, indent;
    int    words; bool words_ok;
} Para;

typedef struct {
    u8   type;      /* 1 insert, 2 delete */
    Pos  a;
    char *text; int len;
    Pos  caret;
    u32  group;
} UndoOp;

typedef struct {
    Para   *p; int n, cap;
    bool    modified;
    u32     version;
    UndoOp *undo; int nundo, ucap, uredo;   /* uredo = topo válido p/ refazer */
    u32     group, last_group_ms;
    bool    group_open;
} Doc;

void  doc_init(Doc *d);
void  doc_free(Doc *d);
void  doc_set(Doc *d, const char *text, usize len);
char *doc_text(Doc *d, usize *len);
char *doc_slice(Doc *d, Pos a, Pos b, int *len);
Pos   doc_insert(Doc *d, Pos at, const char *s, int len, bool rec);
void  doc_delete(Doc *d, Pos a, Pos b, bool rec);
int   pos_cmp(Pos a, Pos b);
Pos   pos_min(Pos a, Pos b);
Pos   pos_max(Pos a, Pos b);
Pos   doc_clamp(Doc *d, Pos p);
Pos   doc_left(Doc *d, Pos p);
Pos   doc_right(Doc *d, Pos p);
Pos   doc_word_left(Doc *d, Pos p);
Pos   doc_word_right(Doc *d, Pos p);
Pos   doc_end(Doc *d);
void  doc_undo_break(Doc *d);
bool  doc_undo(Doc *d, Pos *caret);
bool  doc_redo(Doc *d, Pos *caret);
int   doc_words(Doc *d);
int   doc_chars(Doc *d);
int   para_words(Para *p);

/* ======================================================================
 * md.c — Markdown (estilo ao vivo + árvore para pré-visualização/exportação)
 * ====================================================================== */
enum { ST_BOLD = 1, ST_ITALIC = 2, ST_CODE = 4, ST_STRIKE = 8, ST_LINK = 16, ST_MARK = 32, ST_HIDE = 64 };
enum { LK_PARA, LK_BLANK, LK_H, LK_QUOTE, LK_ULIST, LK_OLIST, LK_TASK, LK_FENCE, LK_CODE, LK_HR, LK_TABLE };

typedef struct { u8 kind, level; int prefix; bool checked; int indent; int number; } LineKind;
LineKind md_line(const char *s, int len, bool in_code);
void     md_inline(const char *s, int len, u8 *sty);

enum { MB_PARA, MB_H, MB_QUOTE, MB_ULIST, MB_OLIST, MB_TASK, MB_CODE, MB_HR, MB_TABLE };
typedef struct { u8 kind, level; bool checked; int number, indent; char *text; int len; int src; } MdBlock;   /* src: parágrafo de origem */
typedef struct { MdBlock *b; int n, cap; char *title; } MdDoc;
void md_parse(Doc *d, MdDoc *out);
void md_free(MdDoc *m);
typedef struct { const char *s; int len; u8 sty; const char *url; int url_len; } MdRun;
int  md_runs(const char *s, int len, MdRun *runs, int max);   /* remove marcação */
char *md_plain(const char *s, int len, int *outlen);

/* tex.c — fontes LaTeX (.tex .bib .sty .cls): editadas como texto, com realce por linha */
enum { SYN_MD, SYN_TEX };                      /* sintaxe do documento (AppDoc.syntax) */
enum { ST_TEX = 128, TX_CMD = 1, TX_BRACE = 2, TX_COMMENT = 3, TX_CLASS = 7, TX_MATH = 8 };   /* p->sty no modo TEX: ST_TEX | classe | TX_MATH */
int   doc_syntax_from_path(const char *path);  /* .tex .bib .sty .cls -> SYN_TEX; o resto (e sem nome) -> SYN_MD */
bool  path_editable(const char *path);         /* abre/grava como texto: .md .txt (e sinônimos) e fontes LaTeX */
void  tex_line(const char *s, int len, u8 *sty);   /* uma passada, sem alocar, estado só da linha */

/* ======================================================================
 * export.c / zip.c
 * ====================================================================== */
enum { FMT_MD, FMT_TXT, FMT_HTML, FMT_RTF, FMT_DOCX, FMT_ODT, FMT_EPUB, FMT_COUNT };
typedef struct { const char *ext; const char *name; const char *desc; bool editable; } FormatInfo;
const FormatInfo *format_info(int fmt);
int   format_from_path(const char *path);
bool  export_doc(Doc *d, int fmt, const char *title, u8 **out, usize *len);

typedef struct { u8 *buf; usize len, cap; u8 *cdir; usize clen, ccap; int count; } Zip;
void  zip_begin(Zip *z);
void  zip_add(Zip *z, const char *name, const void *data, usize len);
void  zip_end(Zip *z, u8 **out, usize *len);
u32   crc32(const void *data, usize len);

/* buffer de texto crescente */
typedef struct { char *s; usize len, cap; } Buf;
void buf_put(Buf *b, const void *s, usize n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *f, ...);
void buf_xml(Buf *b, const char *s, int n);   /* escapa &<>" */
void buf_free(Buf *b);

/* diagnóstico e ajustes de hardware (PAL) */
void pal_diag(Buf *o);                   /* texto com tudo o que o hardware informou */
void pal_set_touch_mode(int m);
void pal_audio_set_gpio(int on);
void pal_audio_probe(void);              /* detecta a placa de som (pode levar ~0,5 s) */
void pal_video_fast(bool on);
void pal_set_rotation(int pref);         /* 0 automática, 1..4 = 0/90/180/270° (aplicada entre quadros) */
int  pal_rotation(void);                 /* rotação em uso, 0..3 */
void pal_set_video_mode(const char *pref);   /* "" automática, "max" ou "LxA" */
void pal_video_mode(int *w, int *h);     /* modo físico em uso */
int  pal_video_modes(int *ws, int *hs, int max);
float pal_display_dpi(void);             /* 0 se o monitor não informar o tamanho */            /* framebuffer em write-combining (MTRR) */

/* ======================================================================
 * ui.c — gerenciador de janelas + widgets imediatos
 * ====================================================================== */
typedef struct Window Window;
typedef void (*WinBody)(Window *w, Rect body);
struct Window {
    const char *title;
    int    icon;
    Rect   r;
    bool   open, modal, wants_keys, resizable, no_titlebar;
    float  anim;        /* 0..1 abertura */
    int    min_w, min_h;
    WinBody body;
    void  (*on_close)(Window *w);
    int    z;
    bool   centered_once;
    /* navegação por teclado: item com foco (-1 = nenhum), itens contados no último quadro e
     * o tipo do item focado (decide se Enter/Espaço/setas são dele ou do campo/lista) */
    int    nav, nav_n, nav_cnt, nav_kind;
};

typedef struct {
    int   mx, my, pmx, pmy;
    bool  down, pressed, released, rpressed, dbl, triple;
    bool  mdown, mpressed, mreleased;    /* botão do meio */
    int   wheel;
    Event keys[64]; int nkeys;
    bool  moved, typed;
    u64   now, last_click_ms;
    int   click_count, last_click_x, last_click_y;
    float dt;
} Input;

typedef struct {
    Input  in;
    float  scale;
    int    W, H;
    Window *wins[32]; int nwins;
    Window *hot_win;           /* janela sob o mouse (NULL = base) */
    Window *key_win;           /* janela com foco de teclado (NULL = editor) */
    Window *cur_win;           /* janela em desenho */
    void  *active;             /* widget arrastando */
    void  *focus;              /* campo de texto focado */
    int    focus_caret, focus_sel;
    Window *drag_win; int drag_dx, drag_dy; bool resizing;
    const char *tip; Rect tip_r; u64 tip_since; const void *tip_id;
    int    cursor;             /* 0 seta, 1 texto, 2 mão */
    bool   redraw;
    int    wake_ms;            /* sem redraw: pedir o próximo quadro daqui a N ms (0 = sem pedido) */
    bool   modal_block;
    int    block_top, block_bottom;   /* faixas das barras visíveis (não repassam clique ao editor) */
    float  menu_anim;
    Window *sticky;            /* janela que não fecha ao clicar fora (editor de temas: mexer no texto para ver o tema) */
    /* navegação sem mouse */
    bool   nav_show;           /* contorno de foco visível (usou o teclado; some ao clicar) */
    bool   nav_fire;           /* Enter/Espaço para o item focado neste quadro */
    int    nav_adj;            /* ←/→ para o item focado neste quadro (-1, 0, +1) */
    bool   nav_skip;           /* widgets internos de um item composto não contam como itens */
    bool   nav_reveal;         /* o foco mudou: a janela rola até o item */
    bool   nav_found;          /* o item focado foi desenhado neste quadro (nav_r vale) */
    Rect   nav_r;
} UI;
extern UI ui;

#define S(v) ((v) * ui.scale)
#define SI(v) ((int)((v) * ui.scale + 0.5f))

void   ui_begin_frame(void);
void   ui_end_frame(void);
void   wm_add(Window *w);
void   wm_open(Window *w);
void   wm_close(Window *w);
void   wm_toggle(Window *w);
void   wm_raise(Window *w);
Window *wm_top(void);
bool   wm_any_modal(void);
void   wm_hit_test(void);
void   wm_draw_all(void);
bool   ui_mouse_ok(void);           /* camada atual recebe mouse? */
bool   ui_hover(Rect r);
bool   ui_clicked(Rect r);
bool   ui_take_key(Event *e);
bool   ui_key(int key);
const char *editor_clipboard(void);
int    app_mods(void);
u16    face_gid_fb(Face **f, u32 cp);
void   ui_tooltip(const void *id, Rect r, const char *text);
/* navegação por teclado (Tab/Shift+Tab, setas, Enter/Espaço) nas janelas */
enum { NAV_ITEM, NAV_ADJ, NAV_TEXT, NAV_LIST };
bool   ui_nav(Rect r, int kind);           /* registra um item; true = é o item com foco */
bool   ui_item(Rect r);                     /* item clicável genérico: clique ou Enter/Espaço */
void   ui_reveal(float *scroll, Rect view); /* rola para mostrar o item focado (depois de desenhar) */
void   wm_focus(Window *w);                 /* foco de teclado para a janela (0 = texto) */
void   wm_cycle_focus(void);                /* Ctrl+F6: texto → janelas abertas → texto */
bool   wm_kb_key(Event *e);                 /* Alt+F7 mover / Alt+F8 redimensionar a janela pelo teclado */

enum { BTN_GHOST, BTN_PRIMARY, BTN_DANGER, BTN_SUBTLE };
bool   ui_button(Rect r, const char *label, int style);
bool   ui_icon_btn(Rect r, int icon, bool active, const char *tip);
bool   ui_toggle(Rect r, const char *label, bool *v);
bool   ui_slider(Rect r, float *v, float lo, float hi, const char *label, const char *valtxt);
bool   ui_textfield(Rect r, char *buf, int cap, const char *placeholder, bool autofocus);
bool   ui_segmented(Rect r, const char **items, int n, int *sel);
void   ui_label(float x, float y, const char *s, Color c, float px, bool bold);
void   ui_section(Rect *cursor, const char *title);
void   ui_scrollbar(Rect track, float *scroll, float content, float view);
void   ui_panel(Rect r);
void   ui_toast(const char *msg);
void   ui_draw_toasts(void);
Face  *ui_font(bool bold);
float  ui_px(void);

/* ======================================================================
 * app.c — estado global
 * ====================================================================== */
#define MAX_TABS 16
enum { FOCUS_OFF, FOCUS_SENTENCE, FOCUS_PARAGRAPH, FOCUS_LINE };
/* keyboard.c — layouts de teclado como dados (FOLIO/KEYBOARDS/<code>.txt) */
void kbd_scan(void);
int  kbd_count(void);
const char *kbd_name(int i);
const char *kbd_code(int i);
int  kbd_find(const char *code);
void kbd_select(const char *code);
const char *kbd_active(void);
int  kbd_ime(void);                 /* método de entrada pedido pelo layout (IME_*) */
const char *kbd_fonts(void);        /* fontes pedidas pelo layout (linha fonts) */
/* ime.c: métodos de entrada (coreano hangul, japonês romaji → kana) */
enum { IME_NONE, IME_HANGUL, IME_KANA };
typedef struct { void (*del_back)(int ncp, void *ctx); void (*insert)(const char *s, int len, void *ctx); void *ctx; } ImeTarget;
void  ime_setup(void);               /* depois de trocar o layout */
bool  ime_on(void);                  /* compondo (o layout tem IME e ele está ligado) */
void  ime_toggle(void);              /* Ctrl+Espaço */
const char *ime_badge(void);         /* "가", "あ", "ア", "A" ou "" (sem IME) */
bool  ime_feed(u32 ch, ImeTarget *t);    /* true = a tecla foi usada pelo IME */
bool  ime_backspace(ImeTarget *t);       /* true = apagou dentro da composição */
void  ime_commit(ImeTarget *t);          /* fecha a composição (tecla de navegação, clique, Enter…) */
bool  ime_composing(void);
void  fonts_set_kbd(const char *files);  /* fontes de reserva pedidas pelo teclado */
void  editor_ime_commit(void);

/* paths.c — pastas e arquivos do pen-drive (nomes neutros; pastas antigas em português valem) */
enum { DIR_FOLIO, DIR_DOCS, DIR_MUSIC, DIR_FONTS, DIR_THEMES, DIR_TABS, DIR_VERSIONS, DIR_LANGUAGES, DIR_KEYBOARDS,
       FILE_CONFIG, FILE_DRAFT, FILE_DIAG, PATH_COUNT };
void paths_init(int vol);
const char *paths_dir(int id);
const char *paths_show(int id);          /* "FOLIO\THEMES", para mensagens */
void paths_ensure(int vol, int id);
const char *pathfmt(const char *f, int id);   /* L("... %s ...") com o nome da pasta */

/* ======================================================================
 * i18n.c — traduções da interface (FOLIO/LANGUAGES/<code>.po); o original (inglês) está no código
 * ====================================================================== */
const char *L(const char *s);                               /* traduz */
const char *LN(const char *one, const char *many, long n);  /* plural, pela regra do idioma */
#define N_(s) (s)                                           /* só marca (tabelas estáticas) */
void  i18n_scan(void);                    /* descobre os .po da pasta */
int   lang_count(void);                   /* 0 = inglês (original) */
const char *lang_code(int i);
const char *lang_name(int i);             /* no próprio idioma (X-Language-Name) */
const char *lang_keyboard(int i);         /* X-Keyboard */
const char *lang_fonts(int i);            /* X-Fonts */
int   lang_find(const char *code);        /* aceita "en-US", "pt_BR", "es-419"… */
bool  i18n_set(int idx);
int   i18n_current(void);
const char *i18n_code(void);
int   i18n_loaded(void);
u32   i18n_quote(int i);                  /* 0/1 aspas duplas, 2/3 simples */
void  i18n_date(char *out, int cap, int day, int month, int year);
int   i18n_plural_index(const char *expr, long n);
bool  i18n_format_ok(const char *id, const char *str);
int   i18n_parse_po(const char *d, usize len, void (*emit)(const char *id, const char *id_plural, const char *strs, int nstrs, bool fuzzy, void *ctx), void *ctx);
bool  pal_lang(char *out, int cap);       /* idioma do firmware/sistema ("en-US"), se houver */
void  fonts_set_lang(const char *files);  /* fontes extras do idioma (X-Fonts), como reserva */
bool  fonts_lang_ok(void);               /* as fontes pedidas pelo idioma foram encontradas */

typedef struct {
    int   theme, font;
    float font_size, line_height, column, para_space;
    int   focus_mode;
    bool  typewriter, smart_typo, hemingway, show_stats, autohide, paper, md_live;
    bool  caret_blink, smooth_caret, hide_markup;
    bool  first_indent;             /* recuo de primeira linha, como em livro */
    bool  full_width;               /* tela cheia: texto em toda a largura, sem folha */
    char  kb[24];       /* teclado=: código do layout (arquivo FOLIO/TECLADOS/<código>.txt) */
    int   volume;            /* 0..100 */
    int   goal;              /* palavras por sessão */
    int   sprint_min;
    int   read_wpm;          /* leitura: palavras por minuto (rolagem automática e tempo restante) */
    int   autosave_s;
    float ui_scale;          /* 0 = automático */
    float pointer_speed;
    int   touch_mode;        /* 0 automático, 1 touchpad, 2 tela de toque/tablet */
    bool  audio_gpio;        /* liga amplificadores externos pelos GPIO do codec */
    bool  video_fast;        /* vídeo em write-combining */
    int   rotation;          /* 0 automática, 1..4 = 0°, 90°, 180°, 270° */
    char  video_mode[16];    /* "" automática (nativa), "max" ou "LxA" */
    bool  shuffle, repeat;
    char  last_path[512]; int last_vol; int last_p, last_o;
    int   today_date, today_words;  /* palavras escritas hoje (antes da sessão atual) */
    char  recent[6][300];           /* "vol|caminho" */
    int   recent_pos[6][2];         /* onde parou em cada recente (parágrafo, coluna) */
    bool  last_reading;             /* estava no modo leitura ao sair */
    /* abas (opcional): vários documentos abertos, lembrados entre reinícios */
    bool  tabs;
    int   ntab_saved, tab_active;
    char  tab_list[MAX_TABS][300];  /* "vol|caminho" ou "*|N" (sem título: FOLIO\ABAS\ABA-N.MD) */
    int   tab_pos[MAX_TABS][2];
    bool  splash;                   /* tela de abertura FolioOS */
    int   splash_secs;              /* duração da abertura, em segundos (abertura_segundos=) */
    char  lang[16];                 /* idioma=: código (pt-BR, en, es…) ou "auto" */
    bool  no_mouse;                 /* mouse=0: mouse e touchpad desligados, só teclado */
} Prefs;
extern Prefs prefs;

typedef struct {
    Doc   doc;
    int   vol;
    char  path[512];
    char  title[128];
    Pos   caret, anchor;
    bool  has_path;
    u64   last_save_ms, last_edit_ms;
    int   session_start_words;
    u64   session_start_ms;
    u8    syntax;        /* SYN_MD / SYN_TEX, pelo nome do arquivo */
    bool  crlf;          /* o arquivo usava \r\n (mantido ao gravar fontes LaTeX) */
} AppDoc;
extern AppDoc app;

void app_init(void);
bool app_frame(void);          /* devolve true se precisa de outro quadro já */
void app_event(Event *e);
bool app_should_quit(void);
void app_request_redraw(void);
void app_set_theme(int i);
void app_apply_font(void);
void app_open_path(int vol, const char *path);
bool app_save(void);
void app_save_as_done(int vol, const char *path, int fmt);
void app_sync_syntax(void);            /* depois de trocar app.path: sintaxe do documento pelo nome */
void app_new_doc(void);
void app_status(const char *msg);
int  app_boot_vol(void);
/* abas */
void app_tabs_enable(bool on);
void app_set_language(int idx, bool save);
void app_tabs_to_prefs(void);           /* copia as abas abertas para prefs (antes de gravar o CONFIG.INI) */
int  app_tab_count(void);
int  app_tab_current(void);
void app_tab_switch(int i);
void app_tab_new(void);
void app_tab_close(int i);
void app_flush_all(void);               /* grava abas modificadas (arquivo ou rascunho da aba) */
/* splash */
#define SPLASH_SECS_DEFAULT 10
bool splash_active(void);
void splash_draw(void);
void splash_skip(void);
void splash_start(void);

/* editor.c */
void  editor_invalidate(void);
void  editor_draw(Rect area, float chrome_alpha);
void  editor_key(Event *e);
void  editor_mouse(Rect area);
void  editor_scroll_to_caret(bool center);
void  editor_goto(Pos p);
void  editor_insert_text(const char *s, int len);
void  editor_select_all(void);
void  editor_copy(bool cut);
void  editor_paste(void);
float editor_caret_screen_y(void);
void  editor_find_highlight(const char *needle);
bool  editor_find_next(const char *needle, bool backwards);
int   editor_replace_all(const char *needle, const char *rep);
bool  editor_replace_one(const char *needle, const char *rep);
void  editor_reset_view(void);
float editor_get_scroll(void);
void  editor_set_scroll(float y);
bool  editor_chapter(int dir);
void  editor_jump_last_edit(void);
void  editor_dup_para(void);
void  editor_del_para(void);
void  editor_cycle_case(void);
bool  editor_animating(void);

/* preview.c */
void  preview_open(void);
void  preview_draw(Rect area, float chrome_a);
void  preview_key(Event *e);
bool  preview_active(void);
void  preview_close(void);
void  preview_leave(void);               /* fecha a leitura levando o cursor ao trecho lido */
int   preview_top_para(void);
void  preview_goto_para(int p, bool top);
bool  preview_chapter(int dir);
bool  preview_info(char *chap, int cap, int *pct, int *chap_left, int *left);
bool  preview_auto_on(void);
bool  preview_auto_idle(u64 idle_ms);   /* rolando sozinho com o mouse parado: barras podem sumir */
void  preview_toggle_auto(void);
bool  preview_stop_motion(void);         /* Esc: para arrasto/rolagem automática; nunca liga */
Rect  preview_bar_rect(void);            /* controles da leitura, no centro da barra de status */
enum { BAR_PREV, BAR_PLAY, BAR_SLOWER, BAR_SPEED, BAR_FASTER, BAR_NEXT, BAR_COUNT };
Rect  preview_bar_button(int i);         /* para os testes */
float preview_scroll_pos(void);          /* para os testes */
void  preview_draw_bar(float chrome_a);  /* barra de leitura + marcador do botão do meio */

/* filedlg.c */
enum { FD_OPEN, FD_SAVE };
void  filedlg_show(int mode);
extern Window win_files;

/* music.c */
void  music_init(int vol);
void  music_update(void);
void  music_toggle(void);
void  music_next(int dir);
bool  music_playing(void);
bool  music_available(void);
const char *music_now(void);
void  music_chime(void);
void  app_paper_sheet(Rect sheet);      /* desenha a folha de papel (guardada no fundo) */
bool  app_sheet_on(void);                /* folha de papel visível (tema + preferência + modo página) */
float app_text_w(float area_w);         /* largura do texto: coluna (página) ou quase toda a tela */
void  app_save_diag(void);              /* grava FOLIO\DIAGNOSTICO.TXT */
void  app_remember_pos(void);            /* guarda onde parou no documento atual (recentes) */
extern Window win_music;

/* settings.c */
void  prefs_defaults(void);
void  prefs_load(void);
void  prefs_save(void);

/* diálogos genéricos (filedlg.c) */
typedef void (*ConfirmFn)(int choice);   /* 0 cancelar, 1 sim, 2 alternativa */
typedef void (*PromptFn)(const char *text);
void dialog_confirm(const char *title, const char *msg, const char *yes, const char *alt, ConfirmFn fn);
void dialog_prompt(const char *title, const char *initial, PromptFn fn);

/* extras de PAL / app */
int   pal_mods(void);
int   pal_audio_rate(void);
int   app_words_today(void);
void  app_run(void);
int   kbd_translate(Event *e, u32 out[2]);
u32   kbd_pending(void);
