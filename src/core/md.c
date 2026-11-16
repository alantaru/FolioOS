/* FOLIO v4 — Markdown: classificação de linhas, estilos inline, árvore de blocos */
#include "folio.h"

static bool is_alnum(u8 c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80; }
static bool is_ws(u8 c) { return c == ' ' || c == '\t'; }

LineKind md_line(const char *s, int len, bool in_code) {
    LineKind k = { LK_PARA, 0, 0, false, 0, 0 };
    int i = 0, ind = 0;
    while (i < len && is_ws((u8)s[i])) { ind += s[i] == '\t' ? 4 : 1; i++; }
    bool fence = (len - i >= 3) && ((s[i] == '`' && s[i + 1] == '`' && s[i + 2] == '`') || (s[i] == '~' && s[i + 1] == '~' && s[i + 2] == '~'));
    if (in_code) { k.kind = fence ? LK_FENCE : LK_CODE; return k; }
    if (i >= len) { k.kind = LK_BLANK; return k; }
    if (fence) { k.kind = LK_FENCE; return k; }
    k.indent = ind / 2;
    char c = s[i];
    if (c == '#') {
        int h = 0; while (i + h < len && s[i + h] == '#') h++;
        if (h <= 6 && (i + h == len || s[i + h] == ' ')) {
            k.kind = LK_H; k.level = (u8)h; k.prefix = MIN(i + h + 1, len); return k;
        }
    }
    if (c == '>') {
        int j = i, lv = 0;
        while (j < len && (s[j] == '>' || s[j] == ' ')) { if (s[j] == '>') lv++; j++; }
        k.kind = LK_QUOTE; k.level = (u8)lv; k.prefix = j; return k;
    }
    if (c == '-' || c == '*' || c == '_') {
        int n = 0, j = i; bool only = true;
        for (; j < len; j++) { if (s[j] == c) n++; else if (!is_ws((u8)s[j])) { only = false; break; } }
        if (only && n >= 3) { k.kind = LK_HR; return k; }
    }
    if ((c == '-' || c == '*' || c == '+') && i + 1 < len && s[i + 1] == ' ') {
        int j = i + 2;
        if (j + 3 <= len && s[j] == '[' && (s[j + 1] == ' ' || s[j + 1] == 'x' || s[j + 1] == 'X') && s[j + 2] == ']'
            && (j + 3 == len || s[j + 3] == ' ')) {
            k.kind = LK_TASK; k.checked = s[j + 1] != ' '; k.prefix = MIN(j + 4, len); return k;
        }
        k.kind = LK_ULIST; k.prefix = j; return k;
    }
    if (c >= '0' && c <= '9') {
        int j = i, num = 0;
        while (j < len && j - i < 9 && s[j] >= '0' && s[j] <= '9') num = num * 10 + (s[j++] - '0');
        if (j < len && (s[j] == '.' || s[j] == ')') && (j + 1 == len || s[j + 1] == ' ')) {
            k.kind = LK_OLIST; k.number = num; k.prefix = MIN(j + 2, len); return k;
        }
    }
    if (c == '|') { k.kind = LK_TABLE; return k; }
    return k;
}

/* ---------------------------------------------------------------- inline */
void md_inline(const char *s, int len, u8 *sty) {
    memset(sty, 0, (usize)len);
    u8 *used = mem_zalloc((usize)len + 1);
    /* 1) escapes, código e links (têm precedência) */
    for (int i = 0; i < len; i++) {
        if (used[i]) continue;
        u8 c = (u8)s[i];
        if (c == '\\' && i + 1 < len && (u8)s[i + 1] < 0x80 && !is_alnum((u8)s[i + 1]) && s[i + 1] != ' ') {
            sty[i] |= ST_MARK; used[i] = used[i + 1] = 2; i++; continue;
        }
        if (c == '`') {
            int n = 0; while (i + n < len && s[i + n] == '`') n++;
            int j = i + n, close = -1;
            while (j < len) {
                if (s[j] == '`') { int m = 0; while (j + m < len && s[j + m] == '`') m++; if (m == n) { close = j; break; } j += m; }
                else j++;
            }
            if (close >= 0) {
                for (int k = i; k < close + n; k++) { sty[k] |= ST_CODE; used[k] = 1; }
                for (int k = 0; k < n; k++) { sty[i + k] |= ST_MARK; sty[close + k] |= ST_MARK; }
                i = close + n - 1;
            } else i += n - 1;
            continue;
        }
        if (c == '[' || (c == '!' && i + 1 < len && s[i + 1] == '[')) {
            int b = c == '!' ? i + 1 : i;
            int j = b + 1, depth = 1;
            while (j < len && depth) { if (s[j] == '[') depth++; else if (s[j] == ']') depth--; if (depth) j++; }
            if (j < len && j + 1 < len && s[j + 1] == '(') {
                int k = j + 2; while (k < len && s[k] != ')' && s[k] != ' ') k++;
                if (k < len && s[k] == ')') {
                    for (int q = i; q <= b; q++) { sty[q] |= ST_MARK; used[q] = 1; }
                    for (int q = b + 1; q < j; q++) sty[q] |= ST_LINK;
                    sty[j] |= ST_MARK; used[j] = 1;
                    sty[j + 1] |= ST_MARK; used[j + 1] = 1;
                    for (int q = j + 2; q < k; q++) { sty[q] |= ST_MARK | ST_HIDE; used[q] = 1; }
                    sty[k] |= ST_MARK; used[k] = 1;
                    i = j; /* o texto do link continua sendo analisado (ênfase dentro) */
                }
            }
        }
    }
    /* 2) ênfase *, _, ~~ */
    for (int i = 0; i < len; i++) {
        if (used[i]) continue;
        u8 c = (u8)s[i];
        if (c != '*' && c != '_' && c != '~') continue;
        int n = 0; while (i + n < len && s[i + n] == (char)c && !used[i + n]) n++;
        if (c == '~' && n != 2) { i += n - 1; continue; }
        if (n > 3) { i += n - 1; continue; }
        u8 after = i + n < len ? (u8)s[i + n] : ' ';
        u8 before = i > 0 ? (u8)s[i - 1] : ' ';
        bool left_flank = !is_ws(after);
        if (c == '_' && is_alnum(before)) left_flank = false;
        if (!left_flank) { i += n - 1; continue; }
        int close = -1;
        for (int j = i + n; j < len; j++) {
            if (used[j] == 1 && !(sty[j] & ST_LINK)) continue;
            if (s[j] != (char)c) continue;
            int m = 0; while (j + m < len && s[j + m] == (char)c) m++;
            u8 pb = (u8)s[j - 1], na = j + m < len ? (u8)s[j + m] : ' ';
            bool right_flank = !is_ws(pb) && j > i + n;
            if (c == '_' && is_alnum(na)) right_flank = false;
            if (right_flank && m >= n) { close = j + (m - n); break; }
            j += m - 1;
        }
        if (close < 0) { i += n - 1; continue; }
        u8 st = c == '~' ? ST_STRIKE : n == 1 ? ST_ITALIC : n == 2 ? ST_BOLD : (ST_BOLD | ST_ITALIC);
        for (int k = 0; k < n; k++) { sty[i + k] |= ST_MARK; used[i + k] = 1; sty[close + k] |= ST_MARK; used[close + k] = 1; }
        for (int k = i + n; k < close; k++) sty[k] |= st;
        i += n - 1;
    }
    /* marcas herdam o estilo vizinho para desenhar com a mesma fonte */
    for (int i = 0; i < len; i++) if (used[i] == 2) { /* caractere escapado: literal */ }
    mem_free(used);
}

int md_runs(const char *s, int len, MdRun *runs, int max) {
    u8 *sty = mem_alloc((usize)len + 1);
    md_inline(s, len, sty);
    int n = 0;
    for (int i = 0; i < len && n < max; ) {
        if (sty[i] & ST_MARK) { i++; continue; }
        u8 st = sty[i] & (ST_BOLD | ST_ITALIC | ST_CODE | ST_STRIKE | ST_LINK);
        int j = i;
        while (j < len && !(sty[j] & ST_MARK) && (sty[j] & (ST_BOLD | ST_ITALIC | ST_CODE | ST_STRIKE | ST_LINK)) == st) j++;
        MdRun *r = &runs[n++];
        r->s = s + i; r->len = j - i; r->sty = st; r->url = 0; r->url_len = 0;
        if (st & ST_LINK) {
            int k = j; while (k < len && (sty[k] & ST_LINK)) k++;
            while (k < len && (sty[k] & ST_MARK) && !(sty[k] & ST_HIDE)) k++;
            int u0 = k; while (k < len && (sty[k] & ST_HIDE)) k++;
            if (k > u0) { r->url = s + u0; r->url_len = k - u0; }
        }
        i = j;
    }
    mem_free(sty);
    return n;
}

char *md_plain(const char *s, int len, int *outlen) {
    u8 *sty = mem_alloc((usize)len + 1);
    md_inline(s, len, sty);
    char *o = mem_alloc((usize)len + 1); int n = 0;
    for (int i = 0; i < len; i++) if (!(sty[i] & ST_MARK)) o[n++] = s[i];
    o[n] = 0; *outlen = n;
    mem_free(sty);
    return o;
}

/* ---------------------------------------------------------------- blocos */
static int cur_src;
static MdBlock *push(MdDoc *m, int kind) {
    if (m->n >= m->cap) { m->cap = m->cap ? m->cap * 2 : 64; m->b = mem_realloc(m->b, sizeof(MdBlock) * (usize)m->cap); }
    MdBlock *b = &m->b[m->n++];
    memset(b, 0, sizeof *b);
    b->kind = (u8)kind;
    b->src = cur_src;
    return b;
}
static void append(MdBlock *b, const char *s, int n, char sep) {
    int extra = (b->len && sep) ? 1 : 0;
    b->text = mem_realloc(b->text, (usize)(b->len + n + extra + 1));
    if (extra) b->text[b->len++] = sep;
    memcpy(b->text + b->len, s, (usize)n);
    b->len += n; b->text[b->len] = 0;
}

void md_parse(Doc *d, MdDoc *m) {
    memset(m, 0, sizeof *m);
    bool in_code = false;
    MdBlock *cur = 0;   /* bloco de parágrafo/citação/lista que pode continuar */
    for (int i = 0; i < d->n; i++) {
        Para *p = &d->p[i];
        LineKind k = md_line(p->s, p->len, in_code);
        cur_src = i;
        if (k.kind == LK_FENCE) {
            if (!in_code) { in_code = true; cur = push(m, MB_CODE); cur->text = str_dup(""); }
            else { in_code = false; cur = 0; }
            continue;
        }
        if (in_code) { if (cur) append(cur, p->s, p->len, cur->len || cur->number ? '\n' : 0); if (cur) cur->number = 1; continue; }
        const char *t = p->s + k.prefix; int tl = p->len - k.prefix;
        while (tl > 0 && is_ws((u8)t[0])) { t++; tl--; }
        bool hard = p->len >= 2 && p->s[p->len - 1] == ' ' && p->s[p->len - 2] == ' ';
        while (tl > 0 && is_ws((u8)t[tl - 1])) tl--;
        if (tl > 0 && t[tl - 1] == '\\') { tl--; hard = true; }
        switch (k.kind) {
        case LK_BLANK: cur = 0; break;
        case LK_HR: push(m, MB_HR); cur = 0; break;
        case LK_H: {
            while (tl > 0 && t[tl - 1] == '#') tl--;
            while (tl > 0 && is_ws((u8)t[tl - 1])) tl--;
            MdBlock *b = push(m, MB_H); b->level = k.level; append(b, t, tl, 0);
            if (!m->title && k.level == 1) m->title = str_ndup(t, tl);
            cur = 0; break;
        }
        case LK_QUOTE:
            if (cur && cur->kind == MB_QUOTE) append(cur, t, tl, tl ? ' ' : '\n');
            else { cur = push(m, MB_QUOTE); cur->level = k.level; append(cur, t, tl, 0); }
            break;
        case LK_ULIST: case LK_OLIST: case LK_TASK:
            cur = push(m, k.kind == LK_ULIST ? MB_ULIST : k.kind == LK_OLIST ? MB_OLIST : MB_TASK);
            cur->indent = k.indent; cur->number = k.number; cur->checked = k.checked;
            append(cur, t, tl, 0);
            break;
        case LK_TABLE:
            if (cur && cur->kind == MB_TABLE) append(cur, p->s, p->len, '\n');
            else { cur = push(m, MB_TABLE); append(cur, p->s, p->len, 0); }
            break;
        default:
            /* para escritores, cada linha é um parágrafo (igual ao editor);
               só continua um item de lista quando a linha vem recuada */
            if (cur && (cur->kind == MB_ULIST || cur->kind == MB_OLIST || cur->kind == MB_TASK) && p->len && (p->s[0] == ' ' || p->s[0] == '\t'))
                append(cur, t, tl, ' ');
            else { cur = push(m, MB_PARA); append(cur, t, tl, 0); }
            break;
        }
        if (cur && hard && cur->kind == MB_PARA) cur->number = -1; else if (cur && cur->kind == MB_PARA) cur->number = 0;
    }
    if (!m->title) {
        for (int i = 0; i < m->n; i++) if (m->b[i].len) {
            int pl; char *pt = md_plain(m->b[i].text, MIN(m->b[i].len, 80), &pl);
            m->title = pt; break;
        }
    }
    if (!m->title) m->title = str_dup(L("Untitled"));
}

void md_free(MdDoc *m) {
    for (int i = 0; i < m->n; i++) mem_free(m->b[i].text);
    mem_free(m->b); mem_free(m->title);
    memset(m, 0, sizeof *m);
}
