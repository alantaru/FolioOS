/* FOLIO v4 — base: libc mínima, heap, strings, formatação, matemática, UTF-8 */
#include "folio.h"

/* ---------------------------------------------------------------- libc */
#ifdef FOLIO_FREESTANDING
#if !defined(__x86_64__) && !defined(__i386__)
/* ARM e outras: C simples. A barreira vazia em cada volta impede o compilador de trocar o laço
 * por uma chamada a memcpy/memset (o que aqui viraria recursão infinita). */
#define NOIDIOM __asm__ volatile("" ::: "memory")
void *memcpy(void *d, const void *s, size_t n) {
    u8 *dp = d; const u8 *sp = s;
    if ((((uintptr_t)dp | (uintptr_t)sp) & 7) == 0)
        for (; n >= 8; n -= 8, dp += 8, sp += 8) { *(u64 *)dp = *(const u64 *)sp; NOIDIOM; }
    for (; n; n--) { *dp++ = *sp++; NOIDIOM; }
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    u8 *dp = d; const u8 *sp = s;
    if (dp == sp || n == 0) return d;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    while (n--) { dp[n] = sp[n]; NOIDIOM; }
    return d;
}
void *memset(void *d, int c, size_t n) {
    u8 *dp = d;
    if (((uintptr_t)dp & 7) == 0) {
        u64 v = (u64)(u8)c * 0x0101010101010101ull;
        for (; n >= 8; n -= 8, dp += 8) { *(u64 *)dp = v; NOIDIOM; }
    }
    for (; n; n--) { *dp++ = (u8)c; NOIDIOM; }
    return d;
}
#else
/* cópias em palavras de 8 (ou 4) bytes: rápidas também em CPUs sem ERMSB e em memória sem cache */
void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    if (n >= 32) {
#if defined(__x86_64__)
        size_t q = n >> 3; n &= 7;
        __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(q) : : "memory");
#else
        size_t q = n >> 2; n &= 3;
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(q) : : "memory");
#endif
    }
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}
void *memmove(void *d, const void *s, size_t n) {
    u8 *dp = d; const u8 *sp = s;
    if (dp == sp || n == 0) return d;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    /* sobreposição para trás: cópia descendente */
    void *de = dp + n - 1; const void *se = sp + n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(de), "+S"(se), "+c"(n) : : "memory", "cc");
    return d;
}
void *memset(void *d, int c, size_t n) {
    void *r = d;
    if (n >= 32) {
#if defined(__x86_64__)
        u64 v = (u64)(u8)c * 0x0101010101010101ull; size_t q = n >> 3; n &= 7;
        __asm__ volatile("rep stosq" : "+D"(d), "+c"(q) : "a"(v) : "memory");
#else
        u32 v = (u32)(u8)c * 0x01010101u; size_t q = n >> 2; n &= 3;
        __asm__ volatile("rep stosl" : "+D"(d), "+c"(q) : "a"(v) : "memory");
#endif
    }
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
#endif
int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
void *malloc(size_t n) { return mem_alloc(n); }
void  free(void *p) { mem_free(p); }
void *realloc(void *p, size_t n) { return mem_realloc(p, n); }
#endif

/* ---------------------------------------------------------------- heap
 * Alocador first-fit com boundary tags e coalescência.
 * Cabeçalho de 16 bytes: tamanho do bloco (bit0 = em uso) e tamanho do anterior. */
/* Cabeçalho de 16 bytes em TODAS as arquiteturas: os tamanhos são múltiplos de 16 (HSZ mascara os 4 bits
 * baixos). Em 32 bits, dois usize dão só 8 bytes; sem o enchimento, cada bloco "perdia" 8 bytes na conta
 * e o alocador entregava memória em uso (o BOOTIA32.EFI desenhava a tela por cima das fontes). */
#if __SIZEOF_POINTER__ == 8
typedef struct HBlk { usize size, prev; } HBlk;
#else
typedef struct HBlk { usize size, prev, pad[2]; } HBlk;
#endif
_Static_assert(sizeof(HBlk) == 16, "HBlk precisa de 16 bytes em toda arquitetura");
typedef struct HFree { HBlk h; struct HFree *next, *prevf; } HFree;
static u8 *heap_end;
static HFree *free_list;
static usize used_bytes, total_bytes;

#define HSZ(b)   ((b)->size & ~(usize)15)
#define HUSED(b) ((b)->size & 1)

static void fl_remove(HFree *f) {
    if (f->prevf) f->prevf->next = f->next; else free_list = f->next;
    if (f->next) f->next->prevf = f->prevf;
}
static void fl_push(HFree *f) {
    f->prevf = 0; f->next = free_list;
    if (free_list) free_list->prevf = f;
    free_list = f;
}

void heap_init(void *mem, usize size) {
    u8 *p = (u8 *)(((uintptr_t)mem + 15) & ~(uintptr_t)15);
    size -= (usize)(p - (u8 *)mem);
    size &= ~(usize)15;
    heap_end = p + size - sizeof(HBlk);
    HFree *f = (HFree *)p;
    f->h.size = size - sizeof(HBlk); f->h.prev = 0;
    HBlk *sent = (HBlk *)heap_end; sent->size = 1; sent->prev = f->h.size;
    free_list = 0; fl_push(f);
    total_bytes = size; used_bytes = 0;
}

void *mem_alloc(usize n) {
    if (n == 0) n = 1;
    usize need = ((n + 15) & ~(usize)15) + sizeof(HBlk);
    if (need < sizeof(HFree) + 16) need = (sizeof(HFree) + 16 + 15) & ~(usize)15;   /* múltiplo de 16 também em 32 bits */
    for (HFree *f = free_list; f; f = f->next) {
        usize sz = HSZ(&f->h);
        if (sz < need) continue;
        fl_remove(f);
        if (sz - need >= sizeof(HFree) + 32) {
            HFree *rest = (HFree *)((u8 *)f + need);
            rest->h.size = sz - need; rest->h.prev = need;
            HBlk *after = (HBlk *)((u8 *)rest + HSZ(&rest->h));
            after->prev = HSZ(&rest->h);
            fl_push(rest);
            f->h.size = need;
        }
        f->h.size |= 1;
        used_bytes += HSZ(&f->h);
        return (u8 *)f + sizeof(HBlk);
    }
    return 0;
}

void *mem_zalloc(usize n) { void *p = mem_alloc(n); if (p) memset(p, 0, n); return p; }

void mem_free(void *ptr) {
    if (!ptr) return;
    HFree *f = (HFree *)((u8 *)ptr - sizeof(HBlk));
    f->h.size &= ~(usize)1;
    used_bytes -= HSZ(&f->h);
    HBlk *next = (HBlk *)((u8 *)f + HSZ(&f->h));
    if (!HUSED(next)) {
        fl_remove((HFree *)next);
        f->h.size += HSZ(next);
    }
    if (f->h.prev) {
        HBlk *prev = (HBlk *)((u8 *)f - f->h.prev);
        if (!HUSED(prev)) {
            fl_remove((HFree *)prev);
            prev->size += HSZ(&f->h);
            f = (HFree *)prev;
        }
    }
    HBlk *after = (HBlk *)((u8 *)f + HSZ(&f->h));
    after->prev = HSZ(&f->h);
    fl_push(f);
}

void *mem_realloc(void *ptr, usize n) {
    if (!ptr) return mem_alloc(n);
    HBlk *b = (HBlk *)((u8 *)ptr - sizeof(HBlk));
    usize have = HSZ(b) - sizeof(HBlk);
    if (have >= n) return ptr;
    void *q = mem_alloc(n + n / 2);
    if (!q) return 0;
    memcpy(q, ptr, have);
    mem_free(ptr);
    return q;
}
usize heap_used(void) { return used_bytes; }
usize heap_total(void) { return total_bytes; }

/* ---------------------------------------------------------------- strings */
int str_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
bool str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
bool str_ieq(const char *a, const char *b) { while (*a && lc((u8)*a) == lc((u8)*b)) { a++; b++; } return lc((u8)*a) == lc((u8)*b); }

int str_cmp_i(const char *a, const char *b) {
    /* comparação "natural": números comparados por valor, sem caixa, sem acento */
    int i = 0, j = 0, la = str_len(a), lb = str_len(b);
    while (i < la && j < lb) {
        if (a[i] >= '0' && a[i] <= '9' && b[j] >= '0' && b[j] <= '9') {
            u64 va = 0, vb = 0;
            while (i < la && a[i] >= '0' && a[i] <= '9') va = va * 10 + (u64)(a[i++] - '0');
            while (j < lb && b[j] >= '0' && b[j] <= '9') vb = vb * 10 + (u64)(b[j++] - '0');
            if (va != vb) return va < vb ? -1 : 1;
            continue;
        }
        u32 ca, cb;
        i += utf8_decode(a, la, i, &ca);
        j += utf8_decode(b, lb, j, &cb);
        ca = cp_fold(ca); cb = cp_fold(cb);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return (la - i) - (lb - j);
}

void str_copy(char *d, const char *s, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    /* não cortar no meio de um caractere UTF-8 */
    while (i > 0 && s[i] && ((u8)d[i - 1] & 0xC0) == 0x80) i--;
    if (i > 0 && s[i] && ((u8)d[i - 1] & 0xC0) == 0xC0) i--;
    d[i] = 0;
}
void str_cat(char *d, const char *s, int cap) { int n = str_len(d); if (n < cap) str_copy(d + n, s, cap - n); }
bool str_starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return false; return true; }
bool str_ends_i(const char *s, const char *suf) {
    int a = str_len(s), b = str_len(suf);
    return a >= b && str_ieq(s + a - b, suf);
}
char *str_ndup(const char *s, int n) { char *d = mem_alloc((usize)n + 1); memcpy(d, s, (usize)n); d[n] = 0; return d; }
char *str_dup(const char *s) { return str_ndup(s, str_len(s)); }

int str_find_i(const char *h, int hl, const char *n, int nl, int from) {
    if (nl <= 0) return -1;
    for (int i = MAX(from, 0); i + nl <= hl; ) {
        int a = i, b = 0; bool ok = true;
        while (b < nl) {
            if (a >= hl) { ok = false; break; }
            u32 ca, cb;
            a += utf8_decode(h, hl, a, &ca);
            b += utf8_decode(n, nl, b, &cb);
            if (cp_fold(ca) != cp_fold(cb)) { ok = false; break; }
        }
        if (ok) return i;
        i = utf8_next(h, hl, i);
    }
    return -1;
}

int str_to_int(const char *s) {
    int v = 0, sg = 1;
    while (*s == ' ') s++;
    if (*s == '-') { sg = -1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v * sg;
}

/* ---------------------------------------------------------------- fmt
 * Subconjunto de printf: %d %i %u %x %X %f %s %c %%, largura, precisão, '-', '0', l/ll/z.
 * Aceita argumentos posicionais (%2$s, %1$d), como o gettext: as traduções podem mudar a ordem
 * das palavras. O separador decimal de %f vem do idioma (X-Decimal no .po; padrão "."). */
typedef struct { char *b; int cap, n; } Out;
static void out_c(Out *o, char c) { if (o->n < o->cap - 1) o->b[o->n] = c; o->n++; }

char fmt_decimal = '.';

#define FMT_MAXARGS 16
typedef union { i64 i; u64 u; double d; const char *s; } FmtArg;

/* lê uma especificação a partir de f (logo depois do '%'); devolve o ponteiro após o tipo */
typedef struct { int pos; bool left, zero; int width, prec, lng; char conv; } Spec;
static const char *parse_spec(const char *f, Spec *s) {
    memset(s, 0, sizeof *s); s->prec = -1;
    const char *p = f; int n = 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    if (*p == '$' && n > 0) { s->pos = n; f = p + 1; }
    while (*f == '-' || *f == '0') { if (*f == '-') s->left = true; else s->zero = true; f++; }
    while (*f >= '0' && *f <= '9') s->width = s->width * 10 + (*f++ - '0');
    if (*f == '.') { f++; s->prec = 0; while (*f >= '0' && *f <= '9') s->prec = s->prec * 10 + (*f++ - '0'); }
    while (*f == 'l') { s->lng++; f++; }
    if (*f == 'z') { s->lng = 2; f++; }
    s->conv = *f ? *f++ : 0;
    return f;
}

bool dbg_on;
void dbg(const char *f, ...) {
    if (!dbg_on) return;
    char b[512]; va_list ap; va_start(ap, f); int n = vfmt(b, (int)sizeof b - 3, f, ap); va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof b - 3) n = (int)sizeof b - 3;
    b[n] = '\r'; b[n + 1] = '\n'; b[n + 2] = 0;
    pal_log(b);
}
int vfmt(char *buf, int cap, const char *f, va_list ap);
int vfmt(char *buf, int cap, const char *f, va_list ap) {
    /* 1ª passada: tipos de cada argumento, pela posição (explícita ou na ordem) */
    char types[FMT_MAXARGS] = {0}; int lngs[FMT_MAXARGS] = {0}, nargs = 0, seq = 0;
    for (const char *p = f; *p; ) {
        if (*p++ != '%') continue;
        if (*p == '%') { p++; continue; }
        Spec s; p = parse_spec(p, &s);
        if (!s.conv) break;
        int idx = s.pos ? s.pos - 1 : seq++;
        if (idx < 0 || idx >= FMT_MAXARGS) continue;
        types[idx] = s.conv; lngs[idx] = s.lng;
        if (idx + 1 > nargs) nargs = idx + 1;
    }
    FmtArg args[FMT_MAXARGS];
    for (int i = 0; i < nargs; i++) {
        char c = types[i];
        if (c == 'd' || c == 'i') args[i].i = lngs[i] >= 2 ? va_arg(ap, i64) : lngs[i] == 1 ? (i64)va_arg(ap, long) : va_arg(ap, int);
        else if (c == 'u' || c == 'x' || c == 'X') args[i].u = lngs[i] >= 2 ? va_arg(ap, u64) : lngs[i] == 1 ? (u64)va_arg(ap, unsigned long) : va_arg(ap, unsigned);
        else if (c == 'f') args[i].d = va_arg(ap, double);
        else if (c == 's') args[i].s = va_arg(ap, const char *);
        else if (c == 'c') args[i].i = va_arg(ap, int);
        else args[i].i = 0;   /* posição sem uso: não consome nada (não há como saber o tipo) */
    }
    /* 2ª passada: escreve */
    Out o = { buf, cap, 0 };
    seq = 0;
    while (*f) {
        if (*f != '%') { out_c(&o, *f++); continue; }
        f++;
        if (*f == '%') { out_c(&o, '%'); f++; continue; }
        Spec s; f = parse_spec(f, &s);
        int idx = s.pos ? s.pos - 1 : seq++;
        FmtArg a; a.i = 0; if (idx >= 0 && idx < FMT_MAXARGS) a = args[idx];
        char tmp[48]; int tl = 0; const char *str = tmp;
        char c = s.conv;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X') {
            u64 v; bool neg = false;
            if (c == 'd' || c == 'i') { i64 sv = a.i; if (sv < 0) { neg = true; v = (u64)(-sv); } else v = (u64)sv; }
            else v = a.u;
            int base = (c == 'x' || c == 'X') ? 16 : 10;
            const char *dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char rev[32]; int rl = 0;
            do { rev[rl++] = dig[v % (u64)base]; v /= (u64)base; } while (v);
            if (neg) tmp[tl++] = '-';
            while (rl) tmp[tl++] = rev[--rl];
        } else if (c == 'f') {
            double v = a.d; int prec = s.prec < 0 ? 2 : MIN(s.prec, 12);
            if (v < 0) { tmp[tl++] = '-'; v = -v; }
            double r = 0.5; for (int i = 0; i < prec; i++) r /= 10; v += r;
            u64 ip = (u64)v; double fp = v - (double)ip;
            char rev[24]; int rl = 0;
            do { rev[rl++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
            while (rl) tmp[tl++] = rev[--rl];
            if (prec > 0) { tmp[tl++] = fmt_decimal; for (int i = 0; i < prec; i++) { fp *= 10; int d = (int)fp; tmp[tl++] = (char)('0' + d); fp -= d; } }
        } else if (c == 's') {
            str = a.s ? a.s : "";
            tl = str_len(str); if (s.prec >= 0 && s.prec < tl) tl = s.prec;
        } else if (c == 'c') {
            tl = utf8_encode((u32)a.i, tmp);
        } else if (c) { tmp[tl++] = c; }
        int pad = s.width - tl;
        if (!s.left) while (pad-- > 0) out_c(&o, s.zero ? '0' : ' ');
        for (int i = 0; i < tl; i++) out_c(&o, str[i]);
        if (s.left) while (pad-- > 0) out_c(&o, ' ');
    }
    if (cap > 0) buf[o.n < cap ? o.n : cap - 1] = 0;
    return o.n;
}
int fmt(char *buf, int cap, const char *f, ...) {
    va_list ap; va_start(ap, f); int n = vfmt(buf, cap, f, ap); va_end(ap); return n;
}

/* ---------------------------------------------------------------- caminhos ("/" separado) */
const char *path_basename(const char *p) {
    const char *b = p;
    for (; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}
void path_join(char *out, int cap, const char *dir, const char *name) {
    char tmp[512];
    str_copy(tmp, dir, sizeof tmp);
    int n = str_len(tmp);
    if (n == 0 || tmp[n - 1] != '/') str_cat(tmp, "/", sizeof tmp);
    str_cat(tmp, name[0] == '/' ? name + 1 : name, sizeof tmp);
    str_copy(out, tmp, cap);
}
void path_parent(char *p) {
    int n = str_len(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    while (n > 0 && p[n - 1] != '/') n--;
    if (n <= 1) { p[0] = '/'; p[1] = 0; return; }
    p[n - 1] = 0;
}
const char *path_ext(const char *p) {
    const char *b = path_basename(p), *e = 0;
    for (; *b; b++) if (*b == '.') e = b;
    return e ? e : b;
}

/* ---------------------------------------------------------------- matemática */
float f_abs(float x) { return x < 0 ? -x : x; }
float f_sqrt(float x) { return x <= 0 ? 0 : __builtin_sqrtf(x); }
float f_floor(float x) {
    if (x > 1e9f || x < -1e9f) return x;
    i64 i = (i64)x; float f = (float)i;
    return (f > x) ? f - 1 : f;
}
float f_ceil(float x) { float f = f_floor(x); return f < x ? f + 1 : f; }
float f_round(float x) { return f_floor(x + 0.5f); }
float f_exp(float x) {
    if (x < -80) return 0;
    if (x > 80) x = 80;
    int k = (int)f_floor(x * 1.44269504f);
    float r = x - (float)k * 0.69314718f;
    float s = 1 + r * (1 + r * (0.5f + r * (1.0f / 6 + r * (1.0f / 24 + r * (1.0f / 120 + r / 720)))));
    union { float f; u32 u; } v; v.u = (u32)(k + 127) << 23;
    if (k < -126) return 0;
    return s * v.f;
}
static float f_log(float x) {
    if (x <= 0) return -80;
    union { float f; u32 u; } v; v.f = x;
    int e = (int)((v.u >> 23) & 255) - 127;
    v.u = (v.u & 0x7FFFFF) | 0x3F800000;   /* m em [1,2) */
    float m = v.f, t = (m - 1) / (m + 1), t2 = t * t;
    float l = 2 * t * (1 + t2 * (1.0f / 3 + t2 * (1.0f / 5 + t2 * (1.0f / 7 + t2 / 9))));
    return l + (float)e * 0.69314718f;
}
float f_pow(float x, float y) { if (x <= 0) return 0; return f_exp(y * f_log(x)); }
float f_sin(float x) {
    const float PI = 3.14159265f;
    x = x - 2 * PI * f_floor((x + PI) / (2 * PI));
    float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}
static u32 rng_state = 0x9E3779B9u;
u32 rng_next(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }
float ease_out(float t) { t = CLAMP(t, 0.0f, 1.0f); float u = 1 - t; return 1 - u * u * u; }

Rect rect_clip(Rect a, Rect b) {
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    return rect(x0, y0, x1 - x0, y1 - y0);
}

/* ---------------------------------------------------------------- UTF-8 */
int utf8_decode(const char *s, int len, int i, u32 *cp) {
    const u8 *p = (const u8 *)s + i; int n = len - i;
    if (n <= 0) { *cp = 0; return 1; }
    u8 c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && n >= 2 && (p[1] & 0xC0) == 0x80) { *cp = ((u32)(c & 0x1F) << 6) | (p[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0 && n >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((u32)(c & 0x0F) << 12) | ((u32)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); return 3;
    }
    if ((c & 0xF8) == 0xF0 && n >= 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = ((u32)(c & 0x07) << 18) | ((u32)(p[1] & 0x3F) << 12) | ((u32)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); return 4;
    }
    /* byte inválido: trata como Latin-1 (arquivos antigos em ANSI) */
    *cp = c; return 1;
}
int utf8_encode(u32 cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
int utf8_prev(const char *s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((u8)s[i] & 0xC0) == 0x80) i--;
    return i;
}
int utf8_next(const char *s, int len, int i) {
    if (i >= len) return len;
    u32 cp; return i + utf8_decode(s, len, i, &cp);
}
u32 cp_lower(u32 c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    return c;
}
u32 cp_fold(u32 c) {
    c = cp_lower(c);
    if (c >= 0xE0 && c <= 0xE5) return 'a';
    if (c == 0xE7) return 'c';
    if (c >= 0xE8 && c <= 0xEB) return 'e';
    if (c >= 0xEC && c <= 0xEF) return 'i';
    if (c == 0xF1) return 'n';
    if ((c >= 0xF2 && c <= 0xF6) || c == 0xF8) return 'o';
    if (c >= 0xF9 && c <= 0xFC) return 'u';
    if (c == 0xFD || c == 0xFF) return 'y';
    return c;
}
bool cp_is_word(u32 c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= 0xC0 && c != 0xD7 && c != 0xF7 && c < 0x2000) || c == '_' || c == '\'' || c == 0x2019;
}
bool cp_is_space(u32 c) { return c == ' ' || c == '\t' || c == 0xA0 || c == '\n' || c == '\r'; }

/* ---------------------------------------------------------------- divisão 64-bit em IA32 (substitui libgcc) */
#if defined(FOLIO_FREESTANDING) && defined(__i386__)
u64 __udivmoddi4(u64 n, u64 d, u64 *rem) {
    u64 q = 0, r = 0;
    if (d == 0) { if (rem) *rem = 0; return 0; }
    if ((d >> 32) == 0 && (n >> 32) == 0) { u32 a = (u32)n, b = (u32)d; if (rem) *rem = a % b; return a / b; }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (u64)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
u64 __udivdi3(u64 n, u64 d) { return __udivmoddi4(n, d, 0); }
u64 __umoddi3(u64 n, u64 d) { u64 r; __udivmoddi4(n, d, &r); return r; }
i64 __divdi3(i64 a, i64 b) {
    bool neg = (a < 0) != (b < 0);
    u64 q = __udivmoddi4(a < 0 ? (u64)-a : (u64)a, b < 0 ? (u64)-b : (u64)b, 0);
    return neg ? -(i64)q : (i64)q;
}
i64 __moddi3(i64 a, i64 b) {
    u64 r; __udivmoddi4(a < 0 ? (u64)-a : (u64)a, b < 0 ? (u64)-b : (u64)b, &r);
    return a < 0 ? -(i64)r : (i64)r;
}
#endif
