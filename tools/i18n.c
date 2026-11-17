/* FolioOS — ferramenta de traduções (roda no PC de desenvolvimento, não no pen-drive)
 *
 *   i18n extract <saída.pot> <fonte.c>...     extrai L("..."), N_("...") e LN("...", "...", n)
 *   i18n update  <modelo.pot> <idioma.po>...  acrescenta os textos novos (vazios), mantém as
 *                                              traduções existentes, marca as obsoletas com #~
 *   i18n check   <modelo.pot> <idioma.po>...  confere formatos (%s, %d…) e cobertura; sai com
 *                                              erro se um formato não bate ou se --strict e faltar algo
 *
 * O texto-fonte é inglês. Escapes C nos literais (\n, \", \xNN) são convertidos para os bytes
 * reais e regravados no .po só com os escapes que o gettext entende (\n \t \" \\). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct { char *id, *plural, *refs; char **strs; int nstr; int fuzzy, used; char *comments; } Entry;
typedef struct { Entry *e; int n, cap; char *header; } Cat;

static char *xstrdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); memcpy(d, s, n); return d; }
static char *slurp(const char *path, long *len) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    b[n] = 0; fclose(f); if (len) *len = n; return b;
}
static Entry *cat_find(Cat *c, const char *id) { for (int i = 0; i < c->n; i++) if (!strcmp(c->e[i].id, id)) return &c->e[i]; return 0; }
static Entry *cat_add(Cat *c, const char *id) {
    if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 256; c->e = realloc(c->e, sizeof(Entry) * (size_t)c->cap); }
    Entry *e = &c->e[c->n++]; memset(e, 0, sizeof *e); e->id = xstrdup(id); return e;
}

/* ------------------------------------------------------------ extração do código C */
typedef struct { char *s; size_t n, cap; } Str;
static void sput(Str *s, char c) { if (s->n + 2 > s->cap) { s->cap = s->cap ? s->cap * 2 : 128; s->s = realloc(s->s, s->cap); } s->s[s->n++] = c; s->s[s->n] = 0; }

/* lê literais adjacentes ("a" "b") a partir de *p; devolve 1 se leu algum */
static int c_strings(const char **pp, Str *out, int *line) {
    const char *p = *pp; int got = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { if (*p == '\n') (*line)++; p++; }
        if (*p != '"') break;
        p++; got = 1;
        while (*p && *p != '"') {
            if (*p == '\\') {
                p++;
                switch (*p) {
                case 'n': sput(out, '\n'); p++; break;
                case 't': sput(out, '\t'); p++; break;
                case 'r': sput(out, '\r'); p++; break;
                case 'x': { p++; int v = 0, k = 0; while (k < 2 && isxdigit((unsigned char)*p)) { v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower(*p) - 'a' + 10)); p++; k++; } sput(out, (char)v); break; }
                default:
                    if (*p >= '0' && *p <= '7') { int v = 0, k = 0; while (k < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; k++; } sput(out, (char)v); }
                    else { sput(out, *p); p++; }
                }
            } else { if (*p == '\n') (*line)++; sput(out, *p++); }
        }
        if (*p == '"') p++;
    }
    *pp = p;
    return got;
}

static void add_ref(Entry *e, const char *file, int line) {
    char r[300]; snprintf(r, sizeof r, "%s:%d", file, line);
    if (!e->refs) { e->refs = xstrdup(r); return; }
    if (strlen(e->refs) > 400) return;
    size_t n = strlen(e->refs) + strlen(r) + 2; char *x = malloc(n);
    snprintf(x, n, "%s %s", e->refs, r); free(e->refs); e->refs = x;
}

static void extract_file(Cat *c, const char *path) {
    char *src = slurp(path, 0); if (!src) { fprintf(stderr, "i18n: não abriu %s\n", path); exit(1); }
    const char *disp = path;
    int line = 1;
    for (const char *p = src; *p; p++) {
        if (*p == '\n') { line++; continue; }
        if (p[0] == '/' && p[1] == '*') { p += 2; while (*p && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') line++; p++; } if (!*p) break; p++; continue; }
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; if (!*p) break; line++; continue; }
        if (*p == '\'') { p++; while (*p && *p != '\'') { if (*p == '\\') p++; p++; } if (!*p) break; continue; }
        if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; if (*p == '\n') line++; p++; } if (!*p) break; continue; }
        int kind = 0, kl = 0;
        if (p > src && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
        if (!strncmp(p, "LN(", 3)) { kind = 2; kl = 3; }
        else if (!strncmp(p, "N_(", 3)) { kind = 1; kl = 3; }
        else if (!strncmp(p, "L(", 2)) { kind = 1; kl = 2; }
        if (!kind) continue;
        const char *q = p + kl; int l0 = line;
        Str a = {0}, b = {0};
        if (!c_strings(&q, &a, &line)) { free(a.s); continue; }
        if (kind == 2) {
            while (*q == ' ' || *q == '\t' || *q == '\n') { if (*q == '\n') line++; q++; }
            if (*q != ',') { free(a.s); continue; }
            q++;
            if (!c_strings(&q, &b, &line)) { free(a.s); continue; }
        }
        if (a.n) {
            Entry *e = cat_find(c, a.s);
            if (!e) { e = cat_add(c, a.s); if (kind == 2) e->plural = xstrdup(b.s ? b.s : ""); }
            add_ref(e, disp, l0);
        }
        free(a.s); free(b.s);
        p = q - 1;
    }
    free(src);
}

/* ------------------------------------------------------------ escrita de .pot/.po */
static void po_str(FILE *f, const char *kw, const char *s) {
    /* textos com \n viram várias linhas, como o gettext faz */
    const char *nl = strchr(s, '\n');
    int multi = nl && nl[1] != 0;          /* tem quebra de linha antes do fim */
    fprintf(f, "%s ", kw);
    if (multi) fprintf(f, "\"\"\n");
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '\n': fputs("\\n", f); if (multi && p[1]) fputs("\"\n\"", f); break;
        case '\t': fputs("\\t", f); break;
        case '\r': fputs("\\r", f); break;
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        default: fputc(*p, f);
        }
    }
    fputs("\"\n", f);
}

static void write_entry(FILE *f, Entry *e, int with_refs, int nplurals, int obsolete) {
    if (e->comments) fputs(e->comments, f);
    if (with_refs && e->refs) fprintf(f, "#: %s\n", e->refs);
    if (e->fuzzy) fputs("#, fuzzy\n", f);
    if (!e->comments || !strstr(e->comments, "c-format")) {
        if (strchr(e->id, '%') || (e->plural && strchr(e->plural, '%'))) fputs("#, c-format\n", f);
    }
    if (obsolete) fputs("#~ ", f);
    po_str(f, "msgid", e->id);
    if (e->plural) {
        if (obsolete) fputs("#~ ", f);
        po_str(f, "msgid_plural", e->plural);
        int n = nplurals > 0 ? nplurals : 2;
        for (int i = 0; i < n; i++) {
            char kw[24]; snprintf(kw, sizeof kw, "msgstr[%d]", i);
            if (obsolete) fputs("#~ ", f);
            po_str(f, kw, i < e->nstr ? e->strs[i] : "");
        }
    } else {
        if (obsolete) fputs("#~ ", f);
        po_str(f, "msgstr", e->nstr > 0 ? e->strs[0] : "");
    }
    fputc('\n', f);
}

/* ------------------------------------------------------------ leitura de .po */
static void po_unescape(const char *p, Str *o) {
    p = strchr(p, '"'); if (!p) return; p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) { p++; sput(o, *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p); p++; }
        else sput(o, *p++);
    }
}
static int read_po(const char *path, Cat *c) {
    long len; char *src = slurp(path, &len); if (!src) return 0;
    char *p = src;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    Str id = {0}, pl = {0}, cur[8] = {{0}}; int nstr = 0, state = 0, fuzzy = 0, have = 0;
    Str comments = {0}, refs = {0};
    sput(&id, 0); id.n = 0;
#define FLUSH() do { if (have) { if (!id.n) { c->header = xstrdup(cur[0].s ? cur[0].s : ""); } else { \
        Entry *e = cat_add(c, id.s); e->plural = pl.n || state == 4 ? xstrdup(pl.s ? pl.s : "") : 0; if (!pl.n && e->plural && !*e->plural) { free(e->plural); e->plural = 0; } \
        e->nstr = nstr; e->strs = malloc(sizeof(char *) * 8); for (int k = 0; k < nstr; k++) e->strs[k] = xstrdup(cur[k].s ? cur[k].s : ""); \
        e->fuzzy = fuzzy; e->comments = comments.n ? xstrdup(comments.s) : 0; e->refs = refs.n ? xstrdup(refs.s) : 0; } } \
        id.n = 0; if (id.s) id.s[0] = 0; pl.n = 0; if (pl.s) pl.s[0] = 0; for (int k = 0; k < 8; k++) { cur[k].n = 0; if (cur[k].s) cur[k].s[0] = 0; } \
        nstr = 0; fuzzy = 0; have = 0; comments.n = 0; if (comments.s) comments.s[0] = 0; refs.n = 0; if (refs.s) refs.s[0] = 0; state = 0; } while (0)
    while (*p) {
        char *ls = p; while (*p && *p != '\n') p++;
        char save = *p; *p = 0;
        char *l = ls; while (*l == ' ' || *l == '\t') l++;
        size_t ll = strlen(l); while (ll && (l[ll - 1] == '\r' || l[ll - 1] == ' ')) l[--ll] = 0;
        if (l[0] == '#') {
            if (l[1] == '~') { /* obsoleta: descarta */ }
            else if (l[1] == ',' && strstr(l, "fuzzy")) { if (state == 3) FLUSH(); fuzzy = 1; }
            else if (l[1] == ':') {          /* referências (arquivo:linha): o .pot as leva para cada .po */
                if (state == 3) FLUSH();
                if (refs.n) sput(&refs, ' ');
                for (char *q = l + 2; *q; q++) if (!(q == l + 2 && *q == ' ')) sput(&refs, *q);
            }
            else { if (state == 3) FLUSH(); if (l[1] != ',') { for (char *q = l; *q; q++) sput(&comments, *q); sput(&comments, '\n'); } }
        } else if (!strncmp(l, "msgid_plural", 12)) { state = 4; po_unescape(l + 12, &pl); }
        else if (!strncmp(l, "msgid", 5)) { if (state == 3) FLUSH(); state = 1; have = 1; po_unescape(l + 5, &id); }
        else if (!strncmp(l, "msgstr", 6)) {
            int k = 0; if (l[6] == '[') k = atoi(l + 7);
            if (k < 0 || k > 7) k = 0;
            state = 3; if (k + 1 > nstr) nstr = k + 1; po_unescape(l + 6, &cur[k]);
        } else if (l[0] == '"') {
            if (state == 1) po_unescape(l, &id); else if (state == 4) po_unescape(l, &pl); else if (state == 3) po_unescape(l, &cur[nstr - 1]);
        }
        *p = save; if (*p) p++;
    }
    if (state == 3) FLUSH();
#undef FLUSH
    free(src);
    return 1;
}

/* ------------------------------------------------------------ formatos */
/* assinatura dos argumentos de um texto de formato, na ordem das posições: "%s %d" → "sd";
 * "%2$d %1$s" → "sd". Assim uma tradução pode trocar a ordem com %1$s/%2$d. */
static void sig(const char *s, char *out, int cap) {
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
    for (int i = 0; i < n && k < cap - 1; i++) out[k++] = by_pos[i] ? by_pos[i] : '?';
    out[k] = 0;
}
static int fmt_ok(const char *a, const char *b) { char x[32], y[32]; sig(a, x, 32); sig(b, y, 32); return !strcmp(x, y); }

static int header_nplurals(const char *h) {
    const char *p = h ? strstr(h, "nplurals=") : 0;
    return p ? atoi(p + 9) : 2;
}

/* ------------------------------------------------------------ comandos */
static const char *POT_HEADER =
    "Project-Id-Version: FolioOS\\n\"\n\"Report-Msgid-Bugs-To: https://github.com/\\n\"\n"
    "\"MIME-Version: 1.0\\n\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n\"Content-Transfer-Encoding: 8bit\\n\"\n"
    "\"Language: \\n\"\n\"Plural-Forms: nplurals=2; plural=(n != 1);\\n\"\n"
    "\"X-Language-Name: \\n\"\n\"X-Keyboard: us\\n\"\n\"X-Quotes: “”‘’\\n\"\n\"X-Fonts: \\n\"\n\"X-Date-Format: {month} {d}, {yyyy}\\n";

static int cmd_extract(int argc, char **argv) {
    Cat c = {0};
    for (int i = 3; i < argc; i++) extract_file(&c, argv[i]);
    FILE *f = fopen(argv[2], "wb"); if (!f) { perror(argv[2]); return 1; }
    fprintf(f, "# FolioOS — translation template. Do not translate this file: copy it to <language>.po.\n");
    fprintf(f, "# Translator guide: docs/TRANSLATING.md\n#\n#, fuzzy\nmsgid \"\"\nmsgstr \"\"\n\"%s\"\n\n", POT_HEADER);
    for (int i = 0; i < c.n; i++) write_entry(f, &c.e[i], 1, 2, 0);
    fclose(f);
    printf("i18n: %d textos em %s\n", c.n, argv[2]);
    return 0;
}

static int cmd_update(int argc, char **argv) {
    Cat pot = {0}; if (!read_po(argv[2], &pot)) { fprintf(stderr, "i18n: não abriu %s\n", argv[2]); return 1; }
    for (int a = 3; a < argc; a++) {
        Cat po = {0}; read_po(argv[a], &po);
        int np = header_nplurals(po.header), added = 0, gone = 0;
        FILE *f = fopen(argv[a], "wb"); if (!f) { perror(argv[a]); return 1; }
        fputs("msgid \"\"\nmsgstr \"\"\n", f);
        /* cabeçalho: uma linha por campo */
        const char *h = po.header ? po.header : "";
        for (const char *p = h; *p; ) {
            const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p + 1) : strlen(p);
            char tmp[512]; snprintf(tmp, sizeof tmp, "%.*s", (int)n, p);
            fputc('"', f);
            for (char *q = tmp; *q; q++) { if (*q == '\n') fputs("\\n", f); else if (*q == '"') fputs("\\\"", f); else if (*q == '\\') fputs("\\\\", f); else fputc(*q, f); }
            fputs("\"\n", f);
            p += n;
        }
        fputc('\n', f);
        for (int i = 0; i < pot.n; i++) {
            Entry *t = &pot.e[i], *e = cat_find(&po, t->id);
            Entry out = *t; out.comments = 0; out.fuzzy = 0;
            if (e) { out.strs = e->strs; out.nstr = e->nstr; out.fuzzy = e->fuzzy; out.comments = e->comments; e->used = 1; }
            else { out.strs = 0; out.nstr = 0; added++; }
            write_entry(f, &out, 1, np, 0);
        }
        for (int i = 0; i < po.n; i++) if (!po.e[i].used && po.e[i].nstr && po.e[i].strs[0][0]) { write_entry(f, &po.e[i], 0, np, 1); gone++; }
        fclose(f);
        printf("i18n: %s: %d novos, %d obsoletos\n", argv[a], added, gone);
    }
    return 0;
}

static int cmd_check(int argc, char **argv) {
    int strict = 0, first = 3;
    if (argc > 3 && !strcmp(argv[3], "--strict")) { strict = 1; first = 4; }
    Cat pot = {0}; if (!read_po(argv[2], &pot)) { fprintf(stderr, "i18n: não abriu %s\n", argv[2]); return 1; }
    int bad = 0;
    for (int a = first; a < argc; a++) {
        Cat po = {0}; if (!read_po(argv[a], &po)) { fprintf(stderr, "i18n: não abriu %s\n", argv[a]); bad = 1; continue; }
        int np = header_nplurals(po.header), done = 0, miss = 0, fbad = 0;
        if (!po.header || !strstr(po.header, "X-Language-Name:")) { fprintf(stderr, "  %s: cabeçalho sem X-Language-Name\n", argv[a]); fbad++; }
        for (int i = 0; i < pot.n; i++) {
            Entry *t = &pot.e[i], *e = cat_find(&po, t->id);
            int ok = e && !e->fuzzy && e->nstr > 0;
            if (ok) for (int k = 0; k < e->nstr; k++) if (!e->strs[k][0]) ok = 0;
            if (ok && t->plural && e->nstr < np) { fprintf(stderr, "  %s: \"%s\": %d formas de plural (o idioma pede %d)\n", argv[a], t->id, e->nstr, np); fbad++; ok = 0; }
            if (!ok) { miss++; if (strict && miss <= 8) fprintf(stderr, "  %s: falta \"%.60s\"\n", argv[a], t->id); continue; }
            for (int k = 0; k < e->nstr; k++) {
                const char *ref = (k > 0 && t->plural) ? t->plural : t->id;
                if (!fmt_ok(ref, e->strs[k]) && !(t->plural && fmt_ok(t->id, e->strs[k]))) {
                    fprintf(stderr, "  %s: formato diferente em \"%.60s\" -> \"%.60s\"\n", argv[a], t->id, e->strs[k]); fbad++;
                }
            }
            done++;
        }
        printf("  %-28s %3d%%  (%d de %d)%s\n", argv[a], pot.n ? done * 100 / pot.n : 100, done, pot.n, fbad ? "  ERROS" : "");
        if (fbad || (strict && miss)) bad = 1;
    }
    return bad;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "extract")) return cmd_extract(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "update")) return cmd_update(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "check")) return cmd_check(argc, argv);
    fprintf(stderr, "uso: i18n extract <out.pot> <src.c>... | update <pot> <po>... | check <pot> [--strict] <po>...\n");
    return 2;
}
