/* FolioOS — layouts de teclado como arquivos de dados (FOLIO/TECLADOS/<código>.txt)
 *
 * O firmware UEFI entrega cada tecla como se o teclado fosse americano (US). Um layout diz, em
 * texto simples, o que cada tecla US deve virar. Qualquer pessoa cria um layout novo copiando
 * um arquivo da pasta; nada disso está no código. Formato (UTF-8, uma regra por linha):
 *
 *     name    Deutsch (QWERTZ)        nome mostrado (no próprio idioma)
 *     map     y z                     tecla US → caractere (com Shift: use o caractere US com Shift)
 *     altgr   q @                     o mesmo, com AltGr
 *     dead    ´ ` ^                   saídas que são teclas mortas (acento + letra)
 *     compose ´ a á                   tecla morta + letra → resultado
 *     include _compose.txt            lê outro arquivo (as composições comuns)
 *     ime     hangul                  método de entrada: hangul (coreano, 2 conjuntos) ou kana
 *                                     (japonês, romaji → hiragana/katakana); Ctrl+Espaço liga/desliga
 *     fonts   _LANGUAGE/NotoSansKR.ttf  fontes de que o texto digitado precisa (em FOLIO/FONTS)
 *
 * Um caractere pode ser escrito literalmente, como U+00E9 ou como "space". Linhas que começam
 * com # são comentários. Arquivos que começam com _ não aparecem na lista (são incluídos). */
#include "folio.h"

#define KB_DIR      paths_dir(DIR_KEYBOARDS)
#define MAX_LAYOUTS 64
#define MAX_COMPOSE 1024
#define MAX_DEAD    24

typedef struct { char code[24]; char name[64]; } LayoutName;
static LayoutName names[MAX_LAYOUTS];
static int nnames;

/* layout ativo */
static char act_code[24];
static u32  kmap[128], kalt[128];
static u32  deads[MAX_DEAD]; static int ndead;
static struct { u32 d, c, r; } comp[MAX_COMPOSE]; static int ncomp;
static u32  dead;   /* tecla morta pendente */
static int  act_ime;              /* IME_* pedido pelo layout */
static char act_fonts[160];

/* ------------------------------------------------------------ leitura */
static bool next_tok(const char **p, const char *end, char *out, int cap) {
    const char *s = *p;
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    if (s >= end) { *p = s; return false; }
    int n = 0;
    while (s < end && *s != ' ' && *s != '\t') { if (n < cap - 1) out[n++] = *s; s++; }
    out[n] = 0; *p = s;
    return true;
}
static u32 tok_cp(const char *t) {
    if (!t[0]) return 0;
    if (str_eq(t, "space")) return ' ';
    if ((t[0] == 'U' || t[0] == 'u') && t[1] == '+' && t[2]) {
        u32 v = 0;
        for (const char *c = t + 2; *c; c++) {
            int d = (*c >= '0' && *c <= '9') ? *c - '0' : (*c >= 'a' && *c <= 'f') ? *c - 'a' + 10 : (*c >= 'A' && *c <= 'F') ? *c - 'A' + 10 : -1;
            if (d < 0) return 0;
            v = v * 16 + (u32)d;
        }
        return v;
    }
    u32 cp; utf8_decode(t, str_len(t), 0, &cp);
    return cp;
}

static void read_name(const char *path, char *name_out, int name_cap) {
    u8 *d; usize len;
    if (!pal_read(app_boot_vol(), path, &d, &len)) return;
    const char *p = (const char *)d, *end = p + len;
    if (len >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) p += 3;
    while (p < end) {
        const char *ls = p; while (p < end && *p != '\n') p++;
        const char *le = p; if (p < end) p++;
        while (le > ls && (le[-1] == '\r' || le[-1] == ' ' || le[-1] == '\t')) le--;
        const char *q = ls;
        char kw[16];
        if (!next_tok(&q, le, kw, sizeof kw) || !str_eq(kw, "name")) continue;
        while (q < le && (*q == ' ' || *q == '\t')) q++;
        int n = MIN((int)(le - q), name_cap - 1);
        memcpy(name_out, q, (usize)n); name_out[n] = 0;
        break;
    }
    mem_free(d);
}

static bool has_dots(const char *s) { for (; *s; s++) if (*s == '/' || *s == '\\' || (s[0] == '.' && s[1] == '.')) return true; return false; }

/* aplica as regras de um arquivo ao layout ativo */
static void apply(const char *path, int depth) {
    u8 *d; usize len;
    if (depth > 3 || !pal_read(app_boot_vol(), path, &d, &len)) return;
    const char *p = (const char *)d, *end = p + len;
    if (len >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) p += 3;
    while (p < end) {
        const char *ls = p; while (p < end && *p != '\n') p++;
        const char *le = p; if (p < end) p++;
        while (le > ls && (le[-1] == '\r' || le[-1] == ' ' || le[-1] == '\t')) le--;
        const char *q = ls;
        char kw[16], a[48], b[48], c[48];
        if (!next_tok(&q, le, kw, sizeof kw) || kw[0] == '#') continue;
        if (str_eq(kw, "map") || str_eq(kw, "altgr")) {
            if (!next_tok(&q, le, a, sizeof a) || !next_tok(&q, le, b, sizeof b)) continue;
            u32 from = tok_cp(a), to = tok_cp(b);
            if (from && from < 128 && to) (kw[0] == 'm' ? kmap : kalt)[from] = to;
        } else if (str_eq(kw, "dead")) {
            while (next_tok(&q, le, a, sizeof a)) { u32 v = tok_cp(a); if (v && ndead < MAX_DEAD) deads[ndead++] = v; }
        } else if (str_eq(kw, "compose")) {
            if (!next_tok(&q, le, a, sizeof a) || !next_tok(&q, le, b, sizeof b) || !next_tok(&q, le, c, sizeof c)) continue;
            if (ncomp < MAX_COMPOSE) { comp[ncomp].d = tok_cp(a); comp[ncomp].c = tok_cp(b); comp[ncomp].r = tok_cp(c); ncomp++; }
        } else if (str_eq(kw, "ime")) {
            if (!next_tok(&q, le, a, sizeof a)) continue;
            act_ime = str_ieq(a, "hangul") ? IME_HANGUL : str_ieq(a, "kana") ? IME_KANA : IME_NONE;
        } else if (str_eq(kw, "fonts")) {
            while (q < le && (*q == ' ' || *q == '\t')) q++;
            int n = MIN((int)(le - q), (int)sizeof act_fonts - 1);
            memcpy(act_fonts, q, (usize)n); act_fonts[n] = 0;
        } else if (str_eq(kw, "include")) {
            if (!next_tok(&q, le, a, sizeof a) || a[0] == '/' || has_dots(a)) continue;
            char sub[160]; path_join(sub, sizeof sub, KB_DIR, a);
            apply(sub, depth + 1);
        }
    }
    mem_free(d);
}

/* ------------------------------------------------------------ lista */
void kbd_scan(void) {
    nnames = 0;
    FileEntry *e; int n = pal_list(app_boot_vol(), KB_DIR, &e);
    for (int i = 0; i < n && nnames < MAX_LAYOUTS; i++) {
        if (e[i].dir || e[i].name[0] == '_' || !str_ends_i(e[i].name, ".txt")) continue;
        LayoutName *ln = &names[nnames];
        int bl = str_len(e[i].name) - 4;
        if (bl <= 0 || bl >= (int)sizeof ln->code) continue;
        memcpy(ln->code, e[i].name, (usize)bl); ln->code[bl] = 0;
        for (char *c = ln->code; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
        char path[160]; path_join(path, sizeof path, KB_DIR, e[i].name);
        ln->name[0] = 0;
        read_name(path, ln->name, sizeof ln->name);
        if (!ln->name[0]) str_copy(ln->name, ln->code, sizeof ln->name);
        nnames++;
    }
    if (n > 0) mem_free(e);
    /* ordem alfabética pelo nome mostrado */
    for (int i = 0; i < nnames; i++)
        for (int j = i + 1; j < nnames; j++)
            if (str_cmp_i(names[i].name, names[j].name) > 0) { LayoutName t = names[i]; names[i] = names[j]; names[j] = t; }
}

int kbd_count(void) { return nnames; }
const char *kbd_name(int i) { return i >= 0 && i < nnames ? names[i].name : "US"; }
const char *kbd_code(int i) { return i >= 0 && i < nnames ? names[i].code : "us"; }
int kbd_find(const char *code) {
    for (int i = 0; i < nnames; i++) if (str_ieq(names[i].code, code)) return i;
    return -1;
}
const char *kbd_active(void) { return act_code; }
int kbd_ime(void) { return act_ime; }
const char *kbd_fonts(void) { return act_fonts; }

/* carrega um layout; código desconhecido (ou pasta ausente) = teclado US sem alterações */
void kbd_select(const char *code) {
    memset(kmap, 0, sizeof kmap); memset(kalt, 0, sizeof kalt);
    ndead = ncomp = 0; dead = 0; act_ime = IME_NONE; act_fonts[0] = 0;
    str_copy(act_code, code && code[0] ? code : "us", sizeof act_code);
    for (char *c = act_code; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    if (has_dots(act_code)) { fonts_set_kbd(""); ime_setup(); return; }
    char fn[40], path[160]; fmt(fn, sizeof fn, "%s.txt", act_code); path_join(path, sizeof path, KB_DIR, fn);
    apply(path, 0);
    fonts_set_kbd(act_fonts);     /* coreano/japonês: a fonte vem junto com o teclado */
    ime_setup();
}

/* ------------------------------------------------------------ tradução das teclas */
static bool is_dead_cp(u32 c) { for (int i = 0; i < ndead; i++) if (deads[i] == c) return true; return false; }
static u32 compose(u32 d, u32 c) {
    for (int i = 0; i < ncomp; i++) if (comp[i].d == d && comp[i].c == c) return comp[i].r;
    return 0;
}

/* Converte um evento "US cru" do firmware para o layout escolhido. Devolve 0, 1 ou 2 codepoints. */
int kbd_translate(Event *e, u32 out[2]) {
    u32 c = e->ch;
    if (!c || (e->mods & MOD_CTRL)) { out[0] = c; return c ? 1 : 0; }
    bool altgr = (e->mods & MOD_ALTGR) != 0;
    u32 m = c < 128 ? (altgr ? kalt[c] : kmap[c]) : 0;
    if (m) c = m;
    bool is_dead = is_dead_cp(c);
    if (dead) {
        u32 d = dead; dead = 0;
        if (c == ' ') { out[0] = d; return 1; }               /* morta + espaço = o próprio sinal */
        if (is_dead && c == d) { out[0] = d; return 1; }
        u32 r = compose(d, c);
        if (r) { out[0] = r; return 1; }
        if (is_dead) { dead = c; out[0] = d; return 1; }
        out[0] = d; out[1] = c; return 2;
    }
    if (is_dead) { dead = c; return 0; }
    out[0] = c; return 1;
}
u32 kbd_pending(void) { return dead; }
