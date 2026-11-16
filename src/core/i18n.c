/* FolioOS — traduções da interface (gettext simplificado, sem nada de idioma no código)
 *
 * O texto-fonte da interface é inglês, escrito no código e marcado com:
 *     L("Save")                         traduz na hora
 *     N_("Save")                        só marca (tabelas estáticas; traduzidas com L() ao desenhar)
 *     LN("%d word", "%d words", n)      plural, segundo a regra do idioma (Plural-Forms)
 *
 * Cada idioma é um arquivo .po comum em FOLIO/LANGUAGES/<código>.po (Poedit, Weblate, Crowdin
 * ou qualquer editor de texto). Para acrescentar um idioma basta colocar o arquivo na pasta: a
 * lista de idiomas, o nome mostrado e as preferências do idioma vêm do cabeçalho do .po:
 *
 *     "Language: fr\n"
 *     "Plural-Forms: nplurals=2; plural=(n > 1);\n"
 *     "X-Language-Name: Français\n"           nome no próprio idioma
 *     "X-Keyboard: fr\n"                       layout padrão (FOLIO/KEYBOARDS/fr.txt)
 *     "X-Quotes: «»“”\n"                       aspas tipográficas: dupla abre/fecha, simples abre/fecha
 *     "X-Fonts: CJK/NotoSansSC.ttf\n"          fontes extras para este alfabeto (em FOLIO/FONTS)
 *     "X-Date-Format: {d} {month} {yyyy}\n"    ordem da data ({d} {dd} {month} {mm} {yyyy})
 *
 * Segurança: uma tradução cujos especificadores (%s, %d, %02d…) não batem com os do original é
 * descartada ao carregar, para que um .po errado nunca derrube o sistema. Entradas "#, fuzzy" e
 * msgstr vazios também são ignorados (fica o original). */
#include "folio.h"

#define MAX_LANGS 64
#define HDR_BYTES 4096

typedef struct {
    char code[16];
    char name[48];
    char kb[24];
    char quotes[24];
    char fonts[160];
    char datefmt[48];
    char decimal[4];
} Lang;

static Lang langs[MAX_LANGS];
static int  nlangs;

/* ------------------------------------------------------------ tabela de traduções */
typedef struct { u32 h; const char *id, *str; int nstr; } Entry;   /* str: formas de plural separadas por \0 */
static Entry *tab;
static int    cap, used;
static char  *arena;
static int    cur;                     /* idioma ativo (0 = inglês, o original) */
static char   plural_expr[96];
static int    nplurals = 2;
static u32    quotes[4] = { 0x201C, 0x201D, 0x2018, 0x2019 };

static u32 hash(const char *s) {
    u32 h = 2166136261u;
    while (*s) { h ^= (u8)*s++; h *= 16777619u; }
    return h ? h : 1;
}

static void put(const char *id, const char *str, int nstr) {
    if (used * 2 >= cap) return;
    u32 h = hash(id);
    for (u32 i = h & (u32)(cap - 1);; i = (i + 1) & (u32)(cap - 1)) {
        if (!tab[i].h) { tab[i].h = h; tab[i].id = id; tab[i].str = str; tab[i].nstr = nstr; used++; return; }
        if (tab[i].h == h && str_eq(tab[i].id, id)) { tab[i].str = str; tab[i].nstr = nstr; return; }
    }
}
static const Entry *find(const char *s) {
    if (!tab || !s || !s[0]) return 0;
    u32 h = hash(s);
    for (u32 i = h & (u32)(cap - 1); tab[i].h; i = (i + 1) & (u32)(cap - 1))
        if (tab[i].h == h && str_eq(tab[i].id, s)) return &tab[i];
    return 0;
}

const char *L(const char *s) {
    const Entry *e = find(s);
    return e ? e->str : s;
}

/* ------------------------------------------------------------ plural (expressão C do gettext) */
typedef struct { const char *p; long n; } PExpr;
static long p_ternary(PExpr *x);
static void p_ws(PExpr *x) { while (*x->p == ' ' || *x->p == '\t') x->p++; }
static long p_primary(PExpr *x) {
    p_ws(x);
    if (*x->p == '(') { x->p++; long v = p_ternary(x); p_ws(x); if (*x->p == ')') x->p++; return v; }
    if (*x->p == '!') { x->p++; return !p_primary(x); }
    if (*x->p == 'n') { x->p++; return x->n; }
    long v = 0; while (*x->p >= '0' && *x->p <= '9') v = v * 10 + (*x->p++ - '0');
    return v;
}
static long p_mul(PExpr *x) {
    long v = p_primary(x);
    for (;;) { p_ws(x); char c = *x->p;
        if (c == '%' || c == '*' || c == '/') { x->p++; long r = p_primary(x); v = c == '*' ? v * r : r ? (c == '%' ? v % r : v / r) : 0; }
        else return v; }
}
static long p_add(PExpr *x) {
    long v = p_mul(x);
    for (;;) { p_ws(x); char c = *x->p;
        if (c == '+' || c == '-') { x->p++; long r = p_mul(x); v = c == '+' ? v + r : v - r; } else return v; }
}
static long p_rel(PExpr *x) {
    long v = p_add(x);
    for (;;) { p_ws(x);
        if (x->p[0] == '<' && x->p[1] == '=') { x->p += 2; v = v <= p_add(x); }
        else if (x->p[0] == '>' && x->p[1] == '=') { x->p += 2; v = v >= p_add(x); }
        else if (x->p[0] == '<') { x->p++; v = v < p_add(x); }
        else if (x->p[0] == '>') { x->p++; v = v > p_add(x); }
        else return v; }
}
static long p_eq(PExpr *x) {
    long v = p_rel(x);
    for (;;) { p_ws(x);
        if (x->p[0] == '=' && x->p[1] == '=') { x->p += 2; v = v == p_rel(x); }
        else if (x->p[0] == '!' && x->p[1] == '=') { x->p += 2; v = v != p_rel(x); }
        else return v; }
}
static long p_and(PExpr *x) { long v = p_eq(x); for (;;) { p_ws(x); if (x->p[0] == '&' && x->p[1] == '&') { x->p += 2; long r = p_eq(x); v = v && r; } else return v; } }
static long p_or(PExpr *x)  { long v = p_and(x); for (;;) { p_ws(x); if (x->p[0] == '|' && x->p[1] == '|') { x->p += 2; long r = p_and(x); v = v || r; } else return v; } }
static long p_ternary(PExpr *x) {
    long c = p_or(x); p_ws(x);
    if (*x->p != '?') return c;
    x->p++; long a = p_ternary(x); p_ws(x);
    if (*x->p == ':') x->p++;
    long b = p_ternary(x);
    return c ? a : b;
}
int i18n_plural_index(const char *expr, long n) {
    if (!expr || !expr[0]) return n != 1;
    PExpr x = { expr, n };
    return (int)p_ternary(&x);
}

const char *LN(const char *one, const char *many, long n) {
    const Entry *e = find(one);
    if (!e) return n == 1 ? one : many;
    /* idiomas de forma única (chinês, japonês, coreano…): nplurals=1, plural=0 → sempre a 1ª */
    int k = CLAMP(i18n_plural_index(plural_expr, n), 0, e->nstr - 1);
    const char *s = e->str;
    for (int i = 0; i < k; i++) s += str_len(s) + 1;
    return s;
}

/* ------------------------------------------------------------ formatos */
/* assinatura dos argumentos de um texto de formato, na ordem das posições: "%s %d" → "sd";
 * "%2$d %1$s" → "sd". Assim uma tradução pode trocar a ordem com %1$s/%2$d. */
static void spec_sig(const char *s, char *out, int cap_) {
    char by_pos[16]; int n = 0, seq = 0;
    for (int i = 0; i < 16; i++) by_pos[i] = 0;
    for (; *s; s++) {
        if (*s != '%') continue;
        s++;
        if (*s == '%') continue;
        int pos = 0; const char *q = s;
        while (*q >= '0' && *q <= '9') pos = pos * 10 + (*q++ - '0');
        if (*q == '$' && pos > 0) s = q + 1; else pos = 0;
        while (*s && !((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))) s++;
        while (*s == 'l' || *s == 'z') s++;
        if (!*s) break;
        int idx = pos ? pos - 1 : seq++;
        if (idx >= 0 && idx < 16) { by_pos[idx] = *s; if (idx + 1 > n) n = idx + 1; }
    }
    int k = 0;
    for (int i = 0; i < n && k < cap_ - 1; i++) out[k++] = by_pos[i] ? by_pos[i] : '?';
    out[k] = 0;
}
bool i18n_format_ok(const char *id, const char *str) {
    char a[32], b[32];
    spec_sig(id, a, sizeof a); spec_sig(str, b, sizeof b);
    return str_eq(a, b);
}

/* ------------------------------------------------------------ leitor de .po */
static const char *po_string(const char *p, const char *end, Buf *out) {
    p++;
    while (p < end && *p != '"') {
        char c = *p++;
        if (c == '\\' && p < end) {
            char e = *p++;
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
        }
        buf_put(out, &c, 1);
    }
    return p < end ? p + 1 : p;
}

/* chama emit(id, plural_id, strs, nstrs, fuzzy) para cada entrada; strs = formas separadas por \0 */
typedef void (*PoEmit)(const char *id, const char *id_plural, const char *strs, int nstrs, bool fuzzy, void *ctx);

int i18n_parse_po(const char *d, usize len, PoEmit emit, void *ctx) {
    const char *p = d, *end = d + len;
    if (len >= 3 && (u8)d[0] == 0xEF && (u8)d[1] == 0xBB && (u8)d[2] == 0xBF) p += 3;
    Buf id = {0}, idp = {0}, st = {0};
    int state = 0;            /* 0 nada, 1 msgid, 2 msgid_plural, 3 msgstr */
    int nstr = 0, n = 0;
    bool fuzzy = false, next_fuzzy = false, have = false;
    char z = 0;
#define FLUSH() do { if (have && state == 3) { buf_put(&id, &z, 1); buf_put(&idp, &z, 1); buf_put(&st, &z, 1); \
        emit(id.s, idp.s, st.s, nstr, fuzzy, ctx); n++; } } while (0)
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        const char *le = p;
        if (p < end) p++;
        while (ls < le && (*ls == ' ' || *ls == '\t')) ls++;
        while (le > ls && (le[-1] == '\r' || le[-1] == ' ' || le[-1] == '\t')) le--;
        if (ls >= le) continue;
        if (*ls == '#') {
            if (ls + 1 < le && ls[1] == ',') for (const char *q = ls; q + 5 <= le; q++) if (memcmp(q, "fuzzy", 5) == 0) next_fuzzy = true;
            continue;
        }
        const char *q = ls;
        while (q < le && *q != ' ' && *q != '"') q++;
        int kl = (int)(q - ls);
        const char *qs = q; while (qs < le && *qs != '"') qs++;
        if (kl == 5 && memcmp(ls, "msgid", 5) == 0) {
            FLUSH();
            id.len = idp.len = st.len = 0; nstr = 0; have = true; state = 1;
            fuzzy = next_fuzzy; next_fuzzy = false;
            if (qs < le) po_string(qs, le, &id);
        } else if (kl == 12 && memcmp(ls, "msgid_plural", 12) == 0) {
            state = 2;
            if (qs < le) po_string(qs, le, &idp);
        } else if (kl >= 6 && memcmp(ls, "msgstr", 6) == 0) {
            if (state == 3) buf_put(&st, &z, 1);     /* msgstr[1], msgstr[2]…: separa as formas */
            state = 3; nstr++;
            if (qs < le) po_string(qs, le, &st);
        } else if (*ls == '"') {
            Buf *b = state == 1 ? &id : state == 2 ? &idp : state == 3 ? &st : 0;
            if (b) po_string(ls, le, b);
        }
    }
    FLUSH();
#undef FLUSH
    buf_free(&id); buf_free(&idp); buf_free(&st);
    return n;
}

/* campo "Chave: valor" do cabeçalho (msgstr da entrada vazia) */
static bool header_field(const char *hdr, const char *key, char *out, int cap_) {
    int kl = str_len(key);
    for (const char *l = hdr; *l; ) {
        const char *e = l; while (*e && *e != '\n') e++;
        if (e - l > kl && memcmp(l, key, (usize)kl) == 0 && l[kl] == ':') {
            const char *v = l + kl + 1; while (v < e && *v == ' ') v++;
            int n = MIN((int)(e - v), cap_ - 1);
            memcpy(out, v, (usize)n); out[n] = 0;
            while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\r')) out[--n] = 0;
            return true;
        }
        l = *e ? e + 1 : e;
    }
    return false;
}

/* ------------------------------------------------------------ descoberta dos idiomas */
typedef struct { Buf hdr; bool got; } HdrCtx;
static void hdr_emit(const char *id, const char *idp, const char *strs, int nstr, bool fuzzy, void *c) {
    (void)idp; (void)nstr; (void)fuzzy;
    HdrCtx *h = c;
    if (!h->got && !id[0]) { buf_puts(&h->hdr, strs); h->got = true; }
}

static void read_header(const char *path, Lang *lg) {
    u64 size; PalFile *f = pal_fopen(app_boot_vol(), path, &size);
    if (!f) return;
    char *buf = mem_alloc(HDR_BYTES + 1);
    int n = pal_fread(f, buf, HDR_BYTES);
    pal_fclose(f);
    if (n <= 0) { mem_free(buf); return; }
    buf[n] = 0;
    HdrCtx h = {0};
    i18n_parse_po(buf, (usize)n, hdr_emit, &h);
    if (h.got) {
        char *hd = h.hdr.s;
        header_field(hd, "X-Language-Name", lg->name, sizeof lg->name);
        header_field(hd, "X-Keyboard", lg->kb, sizeof lg->kb);
        header_field(hd, "X-Quotes", lg->quotes, sizeof lg->quotes);
        header_field(hd, "X-Fonts", lg->fonts, sizeof lg->fonts);
        header_field(hd, "X-Date-Format", lg->datefmt, sizeof lg->datefmt);
        header_field(hd, "X-Decimal", lg->decimal, sizeof lg->decimal);
    }
    buf_free(&h.hdr);
    mem_free(buf);
}

void i18n_scan(void) {
    nlangs = 0;
    /* 0: o original (inglês), que vive no código */
    Lang *src = &langs[nlangs++];
    memset(src, 0, sizeof *src);
    str_copy(src->code, "en", sizeof src->code);
    str_copy(src->name, "English", sizeof src->name);
    str_copy(src->kb, "us", sizeof src->kb);
    FileEntry *e; int n = pal_list(app_boot_vol(), paths_dir(DIR_LANGUAGES), &e);
    for (int i = 0; i < n && nlangs < MAX_LANGS; i++) {
        if (e[i].dir || !str_ends_i(e[i].name, ".po") || e[i].name[0] == '_') continue;
        int bl = str_len(e[i].name) - 3;
        if (bl <= 0 || bl >= 16) continue;
        Lang *lg = &langs[nlangs];
        memset(lg, 0, sizeof *lg);
        memcpy(lg->code, e[i].name, (usize)bl); lg->code[bl] = 0;
        /* FAT costuma devolver o nome em maiúsculas: normaliza para "pt-BR", "zh-CN", "fr" */
        for (char *c = lg->code; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
        for (char *c = lg->code; *c; c++) if (*c == '-' || *c == '_') { *c = '-'; for (char *d = c + 1; *d; d++) if (*d >= 'a' && *d <= 'z') *d -= 32; break; }
        char path[160]; path_join(path, sizeof path, paths_dir(DIR_LANGUAGES), e[i].name);
        read_header(path, lg);
        if (!lg->name[0]) str_copy(lg->name, lg->code, sizeof lg->name);
        if (str_ieq(lg->code, "en")) { *src = *lg; continue; }          /* en.po pode ajustar o original */
        nlangs++;
    }
    if (n > 0) mem_free(e);
    /* ordem alfabética pelo nome (o original fica primeiro) */
    for (int i = 1; i < nlangs; i++)
        for (int j = i + 1; j < nlangs; j++)
            if (str_cmp_i(langs[i].name, langs[j].name) > 0) { Lang t = langs[i]; langs[i] = langs[j]; langs[j] = t; }
}

int lang_count(void) { return nlangs; }
const char *lang_code(int i) { return langs[CLAMP(i, 0, nlangs - 1)].code; }
const char *lang_name(int i) { return langs[CLAMP(i, 0, nlangs - 1)].name; }
const char *lang_keyboard(int i) { return langs[CLAMP(i, 0, nlangs - 1)].kb; }
const char *lang_fonts(int i) { return langs[CLAMP(i, 0, nlangs - 1)].fonts; }

/* "en-US", "pt_BR", "es-419", "zh-Hans-CN"…: exato, depois só o idioma, depois qualquer variante */
int lang_find(const char *code) {
    if (!code || !code[0]) return -1;
    char norm[16]; int k = 0;
    for (; code[k] && k < 15; k++) norm[k] = code[k] == '_' ? '-' : code[k];
    norm[k] = 0;
    for (int i = 0; i < nlangs; i++) if (str_ieq(langs[i].code, norm)) return i;
    char base[8]; k = 0;
    while (norm[k] && norm[k] != '-' && k < 7) { base[k] = norm[k]; k++; }
    base[k] = 0;
    for (int i = 0; i < nlangs; i++) if (str_ieq(langs[i].code, base)) return i;
    for (int i = 0; i < nlangs; i++) {
        const char *c = langs[i].code; int bl = str_len(base);
        if (str_len(c) > bl && c[bl] == '-' && memcmp(c, base, (usize)bl) == 0) return i;
        if (str_len(c) > bl && c[bl] == '-' && str_find_i(c, bl, base, bl, 0) == 0) return i;
    }
    return -1;
}

/* ------------------------------------------------------------ carregar um idioma */
typedef struct { Buf data; int n; char header[512]; } LoadCtx;
static void load_emit(const char *id, const char *idp, const char *strs, int nstr, bool fuzzy, void *c) {
    LoadCtx *x = c;
    if (!id[0]) { str_copy(x->header, strs, sizeof x->header); return; }
    if (fuzzy || nstr < 1 || !strs[0]) return;
    /* confere o formato de cada forma (plural: contra o msgid_plural, que pode ter %d) */
    const char *s = strs;
    for (int i = 0; i < nstr; i++) {
        const char *ref = (i > 0 && idp && idp[0]) ? idp : id;
        if (!s[0] || !i18n_format_ok(ref, s)) return;
        s += str_len(s) + 1;
    }
    usize slen = (usize)(s - strs);
    buf_put(&x->data, id, (usize)str_len(id) + 1);
    char nb = (char)nstr; buf_put(&x->data, &nb, 1);
    buf_put(&x->data, strs, slen);
    x->n++;
}

static void clear(void) {
    if (tab) mem_free(tab);
    if (arena) mem_free(arena);
    tab = 0; arena = 0; cap = used = 0;
}

static u32 nth_cp(const char *s, int n, u32 dflt) {
    int len = str_len(s), i = 0;
    for (int k = 0; i < len; k++) { u32 cp; i += utf8_decode(s, len, i, &cp); if (k == n) return cp; }
    return dflt;
}

static void apply_meta(const Lang *lg) {
    static const u32 dq[4] = { 0x201C, 0x201D, 0x2018, 0x2019 };
    for (int i = 0; i < 4; i++) quotes[i] = lg->quotes[0] ? nth_cp(lg->quotes, i, dq[i]) : dq[i];
    fmt_decimal = lg->decimal[0] ? lg->decimal[0] : '.';
    fonts_set_lang(lg->fonts);
}

bool i18n_set(int idx) {
    if (nlangs == 0) i18n_scan();
    idx = CLAMP(idx, 0, nlangs - 1);
    if (idx == 0) {
        clear(); cur = 0; plural_expr[0] = 0; nplurals = 2;
        apply_meta(&langs[0]);
        ui.redraw = true;
        return true;
    }
    char name[32], path[160];
    fmt(name, sizeof name, "%s.po", langs[idx].code);
    path_join(path, sizeof path, paths_dir(DIR_LANGUAGES), name);
    u8 *d; usize len;
    if (!pal_read(app_boot_vol(), path, &d, &len)) return false;
    LoadCtx x = {0};
    i18n_parse_po((const char *)d, len, load_emit, &x);
    mem_free(d);
    clear();
    if (x.n > 0) {
        arena = x.data.s; x.data.s = 0;
        cap = 64; while (cap < x.n * 2 + 2) cap <<= 1;
        tab = mem_zalloc(sizeof(Entry) * (usize)cap);
        const char *q = arena;
        for (int i = 0; i < x.n; i++) {
            const char *id = q; q += str_len(q) + 1;
            int ns = (u8)*q++;
            const char *st = q;
            for (int k = 0; k < ns; k++) q += str_len(q) + 1;
            put(id, st, ns);
        }
    }
    buf_free(&x.data);
    plural_expr[0] = 0; nplurals = 2;
    char pf[160];
    if (header_field(x.header, "Plural-Forms", pf, sizeof pf)) {
        const char *p = pf; while (*p && memcmp(p, "plural=", 7) != 0) p++;
        if (*p) { str_copy(plural_expr, p + 7, sizeof plural_expr); int l = str_len(plural_expr); while (l > 0 && (plural_expr[l - 1] == ';' || plural_expr[l - 1] == ' ')) plural_expr[--l] = 0; }
        const char *np = pf; while (*np && memcmp(np, "nplurals=", 9) != 0) np++;
        if (*np) nplurals = CLAMP(str_to_int(np + 9), 1, 6);
    }
    cur = idx;
    apply_meta(&langs[idx]);
    ui.redraw = true;
    return true;
}

int  i18n_current(void) { return cur; }
const char *i18n_code(void) { return nlangs ? langs[cur].code : "en"; }
int  i18n_loaded(void) { return used; }
u32  i18n_quote(int i) { return quotes[CLAMP(i, 0, 3)]; }

/* data por extenso no formato do idioma: X-Date-Format, padrão "{month} {d}, {yyyy}" */
void i18n_date(char *out, int cap_, int day, int month, int year) {
    static const char *months[] = { N_("January"), N_("February"), N_("March"), N_("April"), N_("May"), N_("June"),
                                    N_("July"), N_("August"), N_("September"), N_("October"), N_("November"), N_("December") };
    const char *f = (nlangs && langs[cur].datefmt[0]) ? langs[cur].datefmt : "{month} {d}, {yyyy}";
    int o = 0;
    for (const char *p = f; *p && o < cap_ - 1; ) {
        char tmp[48]; const char *ins = 0;
        if (*p == '{') {
            const char *e = p + 1; while (*e && *e != '}') e++;
            int kl = (int)(e - p - 1);
            if (*e == '}') {
                if (kl == 1 && p[1] == 'd') { fmt(tmp, sizeof tmp, "%d", day); ins = tmp; }
                else if (kl == 2 && memcmp(p + 1, "dd", 2) == 0) { fmt(tmp, sizeof tmp, "%02d", day); ins = tmp; }
                else if (kl == 2 && memcmp(p + 1, "mm", 2) == 0) { fmt(tmp, sizeof tmp, "%02d", month); ins = tmp; }
                else if (kl == 5 && memcmp(p + 1, "month", 5) == 0) ins = L(months[CLAMP(month, 1, 12) - 1]);
                else if (kl == 4 && memcmp(p + 1, "yyyy", 4) == 0) { fmt(tmp, sizeof tmp, "%d", year); ins = tmp; }
                if (ins) { for (const char *c = ins; *c && o < cap_ - 1; c++) out[o++] = *c; p = e + 1; continue; }
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}
