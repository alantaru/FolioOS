/* FOLIO v4 — documento: array de parágrafos UTF-8 + desfazer/refazer agrupado */
#include "folio.h"

static void para_reserve(Para *p, int need) {
    if (need + 1 <= p->cap) return;
    int nc = MAX(need + 1, p->cap * 2);
    if (nc < 32) nc = 32;
    p->s = mem_realloc(p->s, (usize)nc);
    p->cap = nc;
}
static void para_touch(Para *p) { p->lay_key = 0; p->words_ok = false; }
static void para_free(Para *p) { mem_free(p->s); mem_free(p->lines); mem_free(p->sty); memset(p, 0, sizeof *p); }

static void para_set(Para *p, const char *s, int n) {
    para_reserve(p, n);
    if (n) memcpy(p->s, s, (usize)n);
    p->len = n; p->s[n] = 0;
    para_touch(p);
}

static void doc_reserve(Doc *d, int n) {
    if (n <= d->cap) return;
    int nc = MAX(n, d->cap * 2);
    if (nc < 64) nc = 64;
    d->p = mem_realloc(d->p, sizeof(Para) * (usize)nc);
    memset(d->p + d->cap, 0, sizeof(Para) * (usize)(nc - d->cap));
    d->cap = nc;
}

static void undo_clear(Doc *d) {
    for (int i = 0; i < d->nundo; i++) mem_free(d->undo[i].text);
    d->nundo = d->uredo = 0;
}

void doc_init(Doc *d) {
    memset(d, 0, sizeof *d);
    doc_reserve(d, 64);
    d->n = 1;
    para_set(&d->p[0], "", 0);
}

void doc_free(Doc *d) {
    for (int i = 0; i < d->cap; i++) if (d->p[i].s || d->p[i].lines) para_free(&d->p[i]);
    mem_free(d->p);
    undo_clear(d);
    mem_free(d->undo);
    memset(d, 0, sizeof *d);
}

void doc_set(Doc *d, const char *t, usize len) {
    doc_free(d);
    doc_init(d);
    usize i = 0;
    if (len >= 3 && (u8)t[0] == 0xEF && (u8)t[1] == 0xBB && (u8)t[2] == 0xBF) i = 3;
    int np = 0;
    while (true) {
        usize s = i;
        while (i < len && t[i] != '\n') i++;
        usize e = i;
        if (e > s && t[e - 1] == '\r') e--;
        doc_reserve(d, np + 1);
        /* remove \r soltos e tabs viram 4 espaços? mantemos tab */
        para_set(&d->p[np], t + s, (int)(e - s));
        np++;
        if (i >= len) break;
        i++;
    }
    d->n = np;
    d->modified = false;
    d->version++;
}

char *doc_text(Doc *d, usize *len) {
    usize total = 0;
    for (int i = 0; i < d->n; i++) total += (usize)d->p[i].len + 1;
    char *out = mem_alloc(total + 1);
    usize o = 0;
    for (int i = 0; i < d->n; i++) {
        memcpy(out + o, d->p[i].s, (usize)d->p[i].len); o += (usize)d->p[i].len;
        if (i + 1 < d->n) out[o++] = '\n';
    }
    if (o > 0 && out[o - 1] != '\n') out[o++] = '\n';
    out[o] = 0;
    *len = o;
    return out;
}

int pos_cmp(Pos a, Pos b) { return a.p != b.p ? (a.p < b.p ? -1 : 1) : (a.o < b.o ? -1 : a.o > b.o ? 1 : 0); }
Pos pos_min(Pos a, Pos b) { return pos_cmp(a, b) <= 0 ? a : b; }
Pos pos_max(Pos a, Pos b) { return pos_cmp(a, b) >= 0 ? a : b; }
Pos doc_clamp(Doc *d, Pos p) {
    p.p = CLAMP(p.p, 0, d->n - 1);
    p.o = CLAMP(p.o, 0, d->p[p.p].len);
    while (p.o > 0 && p.o < d->p[p.p].len && ((u8)d->p[p.p].s[p.o] & 0xC0) == 0x80) p.o--;
    return p;
}
Pos doc_end(Doc *d) { Pos p = { d->n - 1, d->p[d->n - 1].len }; return p; }

char *doc_slice(Doc *d, Pos a, Pos b, int *len) {
    if (pos_cmp(a, b) > 0) { Pos t = a; a = b; b = t; }
    Buf o = {0};
    for (int i = a.p; i <= b.p; i++) {
        int s = i == a.p ? a.o : 0, e = i == b.p ? b.o : d->p[i].len;
        buf_put(&o, d->p[i].s + s, (usize)(e - s));
        if (i < b.p) buf_put(&o, "\n", 1);
    }
    if (!o.s) buf_put(&o, "", 0);
    *len = (int)o.len;
    return o.s;
}

/* ------------------------------------------------------------ undo */
static UndoOp *undo_push(Doc *d) {
    for (int i = d->uredo; i < d->nundo; i++) mem_free(d->undo[i].text);
    d->nundo = d->uredo;
    if (d->nundo >= d->ucap) {
        d->ucap = d->ucap ? d->ucap * 2 : 256;
        d->undo = mem_realloc(d->undo, sizeof(UndoOp) * (usize)d->ucap);
    }
    if (!d->group_open) { d->group++; d->group_open = true; }
    UndoOp *u = &d->undo[d->nundo++];
    d->uredo = d->nundo;
    memset(u, 0, sizeof *u);
    u->group = d->group;
    return u;
}
void doc_undo_break(Doc *d) { d->group_open = false; }

static Pos pos_after(Pos a, const char *s, int len) {
    for (int i = 0; i < len; i++) { if (s[i] == '\n') { a.p++; a.o = 0; } else a.o++; }
    return a;
}

static void record(Doc *d, int type, Pos a, const char *s, int len) {
    if (d->group_open && d->uredo == d->nundo && d->nundo > 0) {
        UndoOp *t = &d->undo[d->nundo - 1];
        if (t->group == d->group && t->type == type && len < 64) {
            if (type == 1 && pos_cmp(pos_after(t->a, t->text, t->len), a) == 0) {
                t->text = mem_realloc(t->text, (usize)(t->len + len + 1));
                memcpy(t->text + t->len, s, (usize)len); t->len += len; t->text[t->len] = 0;
                return;
            }
            if (type == 2 && pos_cmp(pos_after(a, s, len), t->a) == 0) {      /* backspace */
                char *n = mem_alloc((usize)(t->len + len + 1));
                memcpy(n, s, (usize)len); memcpy(n + len, t->text, (usize)t->len);
                mem_free(t->text); t->text = n; t->len += len; t->text[t->len] = 0; t->a = a;
                return;
            }
            if (type == 2 && pos_cmp(a, t->a) == 0) {                          /* delete */
                t->text = mem_realloc(t->text, (usize)(t->len + len + 1));
                memcpy(t->text + t->len, s, (usize)len); t->len += len; t->text[t->len] = 0;
                return;
            }
        }
    }
    UndoOp *u = undo_push(d);
    u->type = (u8)type; u->a = a; u->text = str_ndup(s, len); u->len = len;
}

/* ------------------------------------------------------------ edição */
Pos doc_insert(Doc *d, Pos at, const char *s, int len, bool rec) {
    at = doc_clamp(d, at);
    if (len <= 0) return at;
    if (rec) record(d, 1, at, s, len);
    d->modified = true; d->version++;
    int nl = 0;
    for (int i = 0; i < len; i++) if (s[i] == '\n') nl++;
    Para *p = &d->p[at.p];
    if (!nl) {
        para_reserve(p, p->len + len);
        memmove(p->s + at.o + len, p->s + at.o, (usize)(p->len - at.o));
        memcpy(p->s + at.o, s, (usize)len);
        p->len += len; p->s[p->len] = 0;
        para_touch(p);
        Pos r = { at.p, at.o + len };
        return r;
    }
    doc_reserve(d, d->n + nl);
    p = &d->p[at.p];
    char *tail = str_ndup(p->s + at.o, p->len - at.o);
    int tail_len = p->len - at.o;
    memmove(&d->p[at.p + 1 + nl], &d->p[at.p + 1], sizeof(Para) * (usize)(d->n - at.p - 1));
    memset(&d->p[at.p + 1], 0, sizeof(Para) * (usize)nl);
    d->n += nl;
    int i = 0, seg = 0;
    int cur = at.p;
    while (seg <= nl) {
        int st = i;
        while (i < len && s[i] != '\n') i++;
        int sl = i - st;
        Para *q = &d->p[cur];
        if (seg == 0) {
            q->len = at.o;
            para_reserve(q, q->len + sl);
            memcpy(q->s + q->len, s + st, (usize)sl);
            q->len += sl; q->s[q->len] = 0;
            para_touch(q);
        } else {
            para_set(q, s + st, sl);
        }
        seg++; i++;
        if (seg <= nl) cur++;
    }
    Para *last = &d->p[cur];
    Pos r = { cur, last->len };
    para_reserve(last, last->len + tail_len);
    memcpy(last->s + last->len, tail, (usize)tail_len);
    last->len += tail_len; last->s[last->len] = 0;
    para_touch(last);
    mem_free(tail);
    return r;
}

void doc_delete(Doc *d, Pos a, Pos b, bool rec) {
    a = doc_clamp(d, a); b = doc_clamp(d, b);
    if (pos_cmp(a, b) > 0) { Pos t = a; a = b; b = t; }
    if (pos_cmp(a, b) == 0) return;
    if (rec) { int n; char *t = doc_slice(d, a, b, &n); record(d, 2, a, t, n); mem_free(t); }
    d->modified = true; d->version++;
    Para *pa = &d->p[a.p];
    if (a.p == b.p) {
        memmove(pa->s + a.o, pa->s + b.o, (usize)(pa->len - b.o));
        pa->len -= b.o - a.o; pa->s[pa->len] = 0;
        para_touch(pa);
        return;
    }
    Para *pb = &d->p[b.p];
    int tl = pb->len - b.o;
    para_reserve(pa, a.o + tl);
    memcpy(pa->s + a.o, pb->s + b.o, (usize)tl);
    pa->len = a.o + tl; pa->s[pa->len] = 0;
    para_touch(pa);
    for (int i = a.p + 1; i <= b.p; i++) para_free(&d->p[i]);
    int rm = b.p - a.p;
    memmove(&d->p[a.p + 1], &d->p[b.p + 1], sizeof(Para) * (usize)(d->n - b.p - 1));
    d->n -= rm;
    memset(&d->p[d->n], 0, sizeof(Para) * (usize)rm);
}

bool doc_undo(Doc *d, Pos *caret) {
    if (d->uredo <= 0) return false;
    u32 g = d->undo[d->uredo - 1].group;
    while (d->uredo > 0 && d->undo[d->uredo - 1].group == g) {
        UndoOp *u = &d->undo[--d->uredo];
        if (u->type == 1) { doc_delete(d, u->a, pos_after(u->a, u->text, u->len), false); *caret = u->a; }
        else { *caret = doc_insert(d, u->a, u->text, u->len, false); }
    }
    d->group_open = false;
    return true;
}

bool doc_redo(Doc *d, Pos *caret) {
    if (d->uredo >= d->nundo) return false;
    u32 g = d->undo[d->uredo].group;
    while (d->uredo < d->nundo && d->undo[d->uredo].group == g) {
        UndoOp *u = &d->undo[d->uredo++];
        if (u->type == 1) *caret = doc_insert(d, u->a, u->text, u->len, false);
        else { doc_delete(d, u->a, pos_after(u->a, u->text, u->len), false); *caret = u->a; }
    }
    d->group_open = false;
    return true;
}

/* ------------------------------------------------------------ navegação */
Pos doc_left(Doc *d, Pos p) {
    if (p.o > 0) { p.o = utf8_prev(d->p[p.p].s, p.o); return p; }
    if (p.p > 0) { p.p--; p.o = d->p[p.p].len; }
    return p;
}
Pos doc_right(Doc *d, Pos p) {
    Para *q = &d->p[p.p];
    if (p.o < q->len) { p.o = utf8_next(q->s, q->len, p.o); return p; }
    if (p.p + 1 < d->n) { p.p++; p.o = 0; }
    return p;
}
static u32 cp_at(Doc *d, Pos p) { u32 c; Para *q = &d->p[p.p]; if (p.o >= q->len) return '\n'; utf8_decode(q->s, q->len, p.o, &c); return c; }
static u32 cp_before(Doc *d, Pos p) { if (p.o == 0) return '\n'; Pos l = p; l.o = utf8_prev(d->p[p.p].s, p.o); return cp_at(d, l); }

Pos doc_word_left(Doc *d, Pos p) {
    if (p.o == 0) return doc_left(d, p);
    while (p.o > 0 && !cp_is_word(cp_before(d, p))) p.o = utf8_prev(d->p[p.p].s, p.o);
    while (p.o > 0 && cp_is_word(cp_before(d, p))) p.o = utf8_prev(d->p[p.p].s, p.o);
    return p;
}
Pos doc_word_right(Doc *d, Pos p) {
    Para *q = &d->p[p.p];
    if (p.o >= q->len) return doc_right(d, p);
    while (p.o < q->len && cp_is_word(cp_at(d, p))) p.o = utf8_next(q->s, q->len, p.o);
    while (p.o < q->len && !cp_is_word(cp_at(d, p))) p.o = utf8_next(q->s, q->len, p.o);
    return p;
}

/* ------------------------------------------------------------ estatísticas */
int para_words(Para *p) {
    if (p->words_ok) return p->words;
    int w = 0; bool in = false, has_alnum = false;
    for (int i = 0; i < p->len; ) {
        u32 c; i += utf8_decode(p->s, p->len, i, &c);
        if (cp_is_space(c)) { if (in && has_alnum) w++; in = false; has_alnum = false; }
        else { in = true; if (cp_is_word(c) && c != '_' && c != '\'') has_alnum = true; }
    }
    if (in && has_alnum) w++;
    p->words = w; p->words_ok = true;
    return w;
}
int doc_words(Doc *d) { int w = 0; for (int i = 0; i < d->n; i++) w += para_words(&d->p[i]); return w; }
int doc_chars(Doc *d) {
    int c = 0;
    for (int i = 0; i < d->n; i++) { Para *p = &d->p[i]; for (int k = 0; k < p->len; k++) if (((u8)p->s[k] & 0xC0) != 0x80) c++; }
    return c;
}

/* ------------------------------------------------------------ Buf */
void buf_put(Buf *b, const void *s, usize n) {
    if (b->len + n + 1 > b->cap) {
        usize nc = MAX(b->cap * 2, b->len + n + 1);
        if (nc < 256) nc = 256;
        b->s = mem_realloc(b->s, nc); b->cap = nc;
    }
    if (n) memcpy(b->s + b->len, s, n);
    b->len += n; b->s[b->len] = 0;
}
void buf_puts(Buf *b, const char *s) { buf_put(b, s, (usize)str_len(s)); }
void buf_printf(Buf *b, const char *f, ...) {
    char tmp[1024]; va_list ap; va_start(ap, f); int n = vfmt(tmp, sizeof tmp, f, ap); va_end(ap);
    buf_put(b, tmp, (usize)MIN(n, (int)sizeof tmp - 1));
}
void buf_xml(Buf *b, const char *s, int n) {
    if (n < 0) n = str_len(s);
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '&') buf_puts(b, "&amp;");
        else if (c == '<') buf_puts(b, "&lt;");
        else if (c == '>') buf_puts(b, "&gt;");
        else if (c == '"') buf_puts(b, "&quot;");
        else if ((u8)c < 32 && c != '\t' && c != '\n') continue;
        else buf_put(b, &c, 1);
    }
}
void buf_free(Buf *b) { mem_free(b->s); b->s = 0; b->len = b->cap = 0; }
