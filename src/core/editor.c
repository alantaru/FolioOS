/* FOLIO v4 — editor: layout, markdown ao vivo, foco, máquina de escrever, seleção */
#include "folio.h"

static u32   lay_gen = 1;
static float *py; static int pycap;          /* y acumulado de cada parágrafo (coordenadas do documento) */
static float doc_h, top_pad, bot_pad;
static float scroll_y, scroll_target;
static float col_w; static int col_x, area_y, area_h;
static float prefer_x = -1;
static bool  selecting; static int sel_mode;  /* 0 char, 1 palavra, 2 parágrafo */
static Pos   sel_origin_a, sel_origin_b;
static char  *clip;
static char  find_needle[128];
static float caret_dx = -1, caret_dy, caret_dh;
static u64   caret_moved_ms;
static bool  free_scroll;                    /* rolagem manual no modo máquina de escrever */
static int   last_caret_para = -1;
static bool  layout_ok;

#define D (&app.doc)

void editor_invalidate(void) { lay_gen++; layout_ok = false; ui.redraw = true; }
const char *editor_clipboard(void) { return clip; }
static float base_px(void) { return prefs.font_size * ui.scale; }
static float base_lh(void) { return base_px() * prefs.line_height; }

/* ------------------------------------------------------------ estilo por byte */
static bool para_is_code(Para *p) { return p->kind == LK_CODE || p->kind == LK_FENCE; }
static Face *sty_face(Para *p, u8 st) {
    bool code = (st & ST_CODE) || para_is_code(p);
    Family *fam = code ? font_mono() : font_get(prefs.font);
    int s = 0;
    if ((st & ST_BOLD) || (p->kind == LK_H && !code)) s |= 1;
    if ((st & ST_ITALIC) || (p->kind == LK_QUOTE && !code)) s |= 2;
    return fam_face(fam, s);
}
static float sty_px(Para *p, u8 st) {
    if (para_is_code(p)) return p->size * 0.86f;
    if (st & ST_CODE) return p->size * 0.88f;
    return p->size;
}

typedef struct { Para *p; int i, end; float x; u16 prev; Face *pf; float ppx; int bullet; } GIt;
typedef struct { int b, n; float x, adv; Face *f; float px; u16 gid; u8 st; u32 cp; bool hid; } GOut;

extern u16 face_gid_fb(Face **f, u32 cp);

static void git_init(GIt *g, Para *p, int start, int end, float x0) {
    g->p = p; g->i = start; g->end = end; g->x = x0; g->prev = 0; g->pf = 0; g->ppx = 0;
    g->bullet = -1;
    if (prefs.md_live && p->kind == LK_ULIST) { int k = 0; while (k < p->len && (p->s[k] == ' ' || p->s[k] == '\t')) k++; g->bullet = k; }
}
static bool git_next(GIt *g, GOut *o) {
    Para *p = g->p;
    if (g->i >= g->end) return false;
    u32 cp; int n = utf8_decode(p->s, p->len, g->i, &cp);
    u8 st = p->sty ? p->sty[g->i] : 0;
    o->b = g->i; o->n = n; o->st = st; o->hid = false;
    g->i += n;
    if (st & ST_HIDE) { o->hid = true; o->x = g->x; o->adv = 0; o->cp = cp; o->f = 0; o->gid = 0; o->px = 0; return true; }
    if (o->b == g->bullet && (cp == '-' || cp == '*' || cp == '+')) cp = 0x2022;
    o->cp = cp;
    Face *f = sty_face(p, st); float px = sty_px(p, st);
    bool tab = cp == '\t';
    if (tab) cp = ' ';
    u16 gid = face_gid_fb(&f, cp);
    if (g->prev && g->pf == f && g->ppx == px) g->x += face_kern(f, g->prev, gid, px);
    o->x = g->x; o->adv = face_adv(f, gid, px) * (tab ? 4 : 1);
    o->f = f; o->px = px; o->gid = gid;
    g->x += o->adv; g->prev = gid; g->pf = f; g->ppx = px;
    return true;
}

/* ------------------------------------------------------------ layout */
static void add_line(Para *p, int s, int e, float w) {
    if (p->nlines >= p->lcap) { p->lcap = p->lcap ? p->lcap * 2 : 4; p->lines = mem_realloc(p->lines, sizeof(VLine) * (usize)p->lcap); }
    VLine *l = &p->lines[p->nlines++];
    l->start = s; l->end = e; l->w = w;
}
static float pad_top(Para *p) {
    if (p->kind == LK_H) return p->lh * (p->level <= 1 ? 0.95f : p->level == 2 ? 0.75f : 0.5f);
    return 0;
}
static float line_x(Para *p, int li) { return li == 0 ? p->hang_w : p->indent; }

static void layout_para(int pi, bool in_code, bool caret_here) {
    Para *p = &D->p[pi];
    u32 key = lay_gen * 4 + (in_code ? 2 : 0) + (caret_here ? 1 : 0);
    if (p->lay_key == key) return;
    p->lay_key = key;
    LineKind k = prefs.md_live ? md_line(p->s, p->len, in_code) : (LineKind){ p->len ? LK_PARA : LK_BLANK, 0, 0, false, 0, 0 };
    p->kind = k.kind; p->level = k.level; p->in_code = in_code;
    if (p->scap < p->len + 1) { p->scap = p->len + 16; p->sty = mem_realloc(p->sty, (usize)p->scap); }
    memset(p->sty, 0, (usize)p->len + 1);
    if (prefs.md_live) {
        if (k.kind == LK_FENCE) for (int i = 0; i < p->len; i++) p->sty[i] = ST_MARK | ST_CODE;
        else if (k.kind == LK_CODE) for (int i = 0; i < p->len; i++) p->sty[i] = ST_CODE;
        else if (k.kind == LK_HR) for (int i = 0; i < p->len; i++) p->sty[i] = ST_MARK;
        else {
            md_inline(p->s + k.prefix, p->len - k.prefix, p->sty + k.prefix);
            for (int i = 0; i < k.prefix; i++) p->sty[i] = ST_MARK;
            if (k.kind == LK_ULIST || k.kind == LK_OLIST) for (int i = 0; i < k.prefix; i++) p->sty[i] = 0;
        }
        bool hide = prefs.hide_markup && !caret_here;
        for (int i = 0; i < p->len; i++) {
            u8 s = p->sty[i] & (u8)~ST_HIDE;
            if (hide && (s & ST_MARK) && k.kind != LK_FENCE && k.kind != LK_HR && k.kind != LK_TASK && k.kind != LK_QUOTE) s |= ST_HIDE;
            p->sty[i] = s;
        }
    }
    float bpx = base_px();
    float mul = 1;
    if (k.kind == LK_H) mul = k.level == 1 ? 1.75f : k.level == 2 ? 1.4f : k.level == 3 ? 1.18f : 1.04f;
    p->size = bpx * mul;
    p->lh = k.kind == LK_H ? p->size * MAX(1.2f, prefs.line_height - 0.35f) : (para_is_code(p) ? base_lh() * 0.86f : base_lh());
    /* recuos */
    float pre_w = 0;
    if (k.prefix > 0) { GIt g; GOut o; git_init(&g, p, 0, k.prefix, 0); while (git_next(&g, &o)) {} pre_w = g.x; }
    float iw = bpx * 1.6f;
    p->hang_w = 0; p->indent = 0;
    if (k.kind == LK_H) { p->hang_w = -pre_w; p->indent = 0; }
    else if (k.kind == LK_ULIST || k.kind == LK_OLIST || k.kind == LK_TASK) {
        float lead = 0;
        { int i = 0; while (i < p->len && (p->s[i] == ' ' || p->s[i] == '\t')) i++;
          if (i) { GIt g; GOut o; git_init(&g, p, 0, i, 0); while (git_next(&g, &o)) {} lead = g.x; } }
        p->hang_w = MAX(0.0f, (float)k.indent * iw - lead);
        p->indent = p->hang_w + pre_w;
    } else if (k.kind == LK_QUOTE) { p->hang_w = 0; p->indent = pre_w; }
    else if (k.kind == LK_PARA && prefs.first_indent) { p->hang_w = bpx * 1.6f; p->indent = 0; }
    /* quebra de linha */
    p->nlines = 0;
    float maxw = col_w;
    int start = 0;
    float x0 = p->hang_w;
    while (true) {
        GIt g; GOut o; git_init(&g, p, start, p->len, x0);
        int brk = -1; float brk_w = 0; bool any = false, overflow = false; int forced = -1;
        float last_w = x0;
        while (git_next(&g, &o)) {
            bool space = o.cp == ' ' || o.cp == '\t';
            if (!space && !o.hid && o.x + o.adv > maxw && any) {
                overflow = true;
                if (brk <= start) forced = o.b;
                break;
            }
            if (!space && !o.hid) any = true;
            if (!space) last_w = o.x + o.adv;
            if (space) { brk = o.b + o.n; brk_w = o.x; }
            else if (o.cp == '-' || o.cp == 0x2014 || o.cp == 0x2013 || o.cp == '/') { if (any) { brk = o.b + o.n; brk_w = o.x + o.adv; } }
        }
        if (!overflow) { add_line(p, start, p->len, last_w); break; }
        int end = forced >= 0 ? forced : brk;
        if (end <= start) end = utf8_next(p->s, p->len, start);
        add_line(p, start, end, forced >= 0 ? last_w : brk_w);
        start = end;
        x0 = p->indent;
        if (start >= p->len) { break; }
    }
    if (p->nlines == 0) add_line(p, 0, 0, 0);
    float h = pad_top(p) + p->nlines * p->lh;
    if (k.kind == LK_BLANK) h = base_lh() * 0.6f;
    p->height = h;
}

static bool is_list(Para *p) { return p->kind == LK_ULIST || p->kind == LK_OLIST || p->kind == LK_TASK; }
static void ensure_layout(void) {
    if (D->n + 1 > pycap) { pycap = D->n + 256; py = mem_realloc(py, sizeof(float) * (usize)pycap); }
    bool in_code = false;
    /* 1) layout de cada parágrafo (estado de bloco de código é sequencial) */
    for (int i = 0; i < D->n; i++) {
        Para *p = &D->p[i];
        bool caret_here = prefs.hide_markup && i == app.caret.p;
        if (prefs.md_live) {
            LineKind k = md_line(p->s, p->len, in_code);
            layout_para(i, in_code, caret_here);
            if (k.kind == LK_FENCE) in_code = !in_code;
        } else layout_para(i, false, caret_here);
    }
    /* 2) posições verticais com espaçamento contextual */
    float y = 0, sp = prefs.para_space * base_lh();
    for (int i = 0; i < D->n; i++) {
        Para *p = &D->p[i];
        py[i] = y;
        y += p->height;
        if (i + 1 >= D->n) break;
        Para *q = &D->p[i + 1];
        if (p->kind == LK_BLANK || q->kind == LK_BLANK) continue;
        if (para_is_code(p) && para_is_code(q)) continue;
        if (is_list(p) && is_list(q)) { y += sp * 0.3f; continue; }
        if (p->kind == LK_QUOTE && q->kind == LK_QUOTE) { y += sp * 0.5f; continue; }
        y += sp;
    }
    py[D->n] = y;
    doc_h = y;
    layout_ok = true;
}

/* ------------------------------------------------------------ geometria */
static int line_of(Para *p, int o) {
    for (int i = 0; i < p->nlines; i++) {
        if (o < p->lines[i].end) return i;
        if (o == p->lines[i].end && (i + 1 == p->nlines || p->lines[i + 1].start > o)) return i;
    }
    return p->nlines - 1;
}
static float x_at(Para *p, int li, int o) {
    VLine *l = &p->lines[li];
    GIt g; GOut out; git_init(&g, p, l->start, l->end, line_x(p, li));
    while (git_next(&g, &out)) { if (out.b >= o) return out.x; }
    return g.x;
}
static void pos_xy(Pos ps, float *x, float *y, float *h) {
    Para *p = &D->p[ps.p];
    int li = line_of(p, ps.o);
    *x = x_at(p, li, ps.o);
    *y = py[ps.p] + pad_top(p) + li * p->lh;
    *h = p->kind == LK_BLANK ? p->height : p->lh;
}
static Pos hit_line(int pi, int li, float x) {
    Para *p = &D->p[pi];
    VLine *l = &p->lines[li];
    GIt g; GOut o; git_init(&g, p, l->start, l->end, line_x(p, li));
    Pos r = { pi, l->end };
    while (git_next(&g, &o)) {
        if (o.hid) continue;
        if (x < o.x + o.adv * 0.5f) { r.o = o.b; return r; }
    }
    if (li + 1 < p->nlines && l->end > l->start) {
        /* fim de linha quebrada: fica antes do espaço final */
        int e = l->end;
        if (e > l->start && p->s[e - 1] == ' ') e--;
        r.o = e;
    }
    return r;
}
static int para_at_y(float y) {
    int lo = 0, hi = D->n - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (py[mid] <= y) lo = mid; else hi = mid - 1; }
    return lo;
}
static Pos hit(float x, float y) {
    int pi = para_at_y(y);
    Para *p = &D->p[pi];
    float ly = y - py[pi] - pad_top(p);
    int li = CLAMP((int)(ly / p->lh), 0, p->nlines - 1);
    return hit_line(pi, li, x);
}

float editor_caret_screen_y(void) {
    if (!layout_ok) return 0;
    float x, y, h; pos_xy(app.caret, &x, &y, &h);
    return area_y + top_pad + y - scroll_y;
}

/* ------------------------------------------------------------ rolagem */
static float max_scroll(void) { return MAX(0.0f, top_pad + doc_h + bot_pad - area_h); }
void editor_scroll_to_caret(bool center) {
    if (!layout_ok) ensure_layout();
    float x, y, h; pos_xy(app.caret, &x, &y, &h);
    float cy = top_pad + y;
    if (prefs.typewriter || center) scroll_target = cy + h * 0.5f - area_h * 0.45f;
    else {
        float margin = MIN(base_lh() * 3, area_h * 0.25f);
        if (cy - margin < scroll_target) scroll_target = cy - margin;
        if (cy + h + margin > scroll_target + area_h) scroll_target = cy + h + margin - area_h;
    }
    scroll_target = CLAMP(scroll_target, 0.0f, max_scroll());
    free_scroll = false;
    ui.redraw = true;
}
static Pos   last_edit = { -1, 0 }, jump_back = { -1, 0 };   /* Ctrl+J: ir e voltar da última alteração */
void editor_reset_view(void) { scroll_y = scroll_target = 0; caret_dx = -1; layout_ok = false; prefer_x = -1; last_edit.p = jump_back.p = -1; }
float editor_get_scroll(void) { return scroll_target; }
void  editor_set_scroll(float y) { scroll_y = scroll_target = MAX(0.0f, y); }
bool editor_animating(void) { return f_abs(scroll_target - scroll_y) > 0.5f; }
float editor_blink_phase_ms(void) { return (float)(ui.in.now - caret_moved_ms); }

/* ------------------------------------------------------------ edição */
static bool has_sel(void) { return pos_cmp(app.caret, app.anchor) != 0; }
static void caret_changed(bool keep_x) {
    if (!keep_x) prefer_x = -1;
    caret_moved_ms = ui.in.now;
    if (prefs.hide_markup && app.caret.p != last_caret_para) layout_ok = false;
    last_caret_para = app.caret.p;
    editor_scroll_to_caret(false);
}
static void delete_sel(void) {
    if (!has_sel()) return;
    Pos a = pos_min(app.caret, app.anchor), b = pos_max(app.caret, app.anchor);
    doc_delete(D, a, b, true);
    app.caret = app.anchor = a;
}
static void touched(void) {
    app.last_edit_ms = ui.in.now;
    last_edit = app.caret;
    layout_ok = false;
    ui.in.typed = true;
}

void editor_insert_text(const char *s, int len) {
    if (has_sel()) { if (prefs.hemingway) return; delete_sel(); }
    app.caret = app.anchor = doc_insert(D, app.caret, s, len, true);
    touched();
    caret_changed(false);
}
/* alvo do método de entrada (coreano, japonês): troca os últimos caracteres compostos */
static void ed_ime_del(int ncp, void *ctx) {
    UNUSED(ctx);
    Pos a = app.caret;
    for (int i = 0; i < ncp; i++) a = doc_left(D, a);
    if (pos_cmp(a, app.caret) < 0) doc_delete(D, a, app.caret, true);
    app.caret = app.anchor = a;
}
static void ed_ime_ins(const char *s, int len, void *ctx) { UNUSED(ctx); editor_insert_text(s, len); }
static ImeTarget ed_ime = { ed_ime_del, ed_ime_ins, &ed_ime };
void editor_ime_commit(void) { ime_commit(&ed_ime); }
void editor_select_all(void) { app.anchor = (Pos){ 0, 0 }; app.caret = doc_end(D); caret_changed(false); }
void editor_copy(bool cut) {
    if (!has_sel()) return;
    int n; mem_free(clip); clip = doc_slice(D, app.caret, app.anchor, &n);
    if (cut && !prefs.hemingway) { doc_undo_break(D); delete_sel(); doc_undo_break(D); touched(); caret_changed(false); }
    ui_toast(cut ? L("Cut") : L("Copied"));
}
void editor_paste(void) {
    if (!clip) return;
    doc_undo_break(D);
    editor_insert_text(clip, str_len(clip));
    doc_undo_break(D);
}

static u32 char_before(Pos p) {
    if (p.o == 0) return 0;
    Para *q = &D->p[p.p]; u32 c; utf8_decode(q->s, q->len, utf8_prev(q->s, p.o), &c); return c;
}

static void wrap_sel(const char *mk);
/* com texto selecionado, ( [ " * _ ` envolvem a seleção em vez de substituí-la */
static bool wrap_pair(u32 cp) {
    u32 o = 0, c = 0;
    switch (cp) {
    case '*': wrap_sel("*"); return true;
    case '_': wrap_sel("_"); return true;
    case '`': wrap_sel("`"); return true;
    case '(': o = '('; c = ')'; break;
    case '[': o = '['; c = ']'; break;
    case '"': o = prefs.smart_typo ? i18n_quote(0) : '"'; c = prefs.smart_typo ? i18n_quote(1) : '"'; break;
    case 0x201C: o = 0x201C; c = 0x201D; break;
    case 0x201E: o = 0x201E; c = 0x201C; break;
    case 0xAB: o = 0xAB; c = 0xBB; break;
    default: return false;
    }
    Pos a = pos_min(app.caret, app.anchor), b = pos_max(app.caret, app.anchor);
    char to[4], tc[4]; int no = utf8_encode(o, to), nc = utf8_encode(c, tc);
    doc_undo_break(D);
    doc_insert(D, b, tc, nc, true);
    doc_insert(D, a, to, no, true);
    a.o += no; if (b.p == a.p) b.o += no;
    app.anchor = a; app.caret = b;
    doc_undo_break(D); touched(); caret_changed(false);
    return true;
}

static void type_char(u32 cp) {
    if (has_sel() && wrap_pair(cp)) return;
    Para *p = &D->p[app.caret.p];
    bool code = p->in_code || p->kind == LK_CODE || p->kind == LK_TABLE || (p->len > 0 && p->s[0] == '|') || (p->sty && app.caret.o > 0 && app.caret.o <= p->len && (p->sty[app.caret.o - 1] & ST_CODE));
    if (prefs.smart_typo && !code && !has_sel()) {
        u32 prev = char_before(app.caret);
        if (cp == '-' && prev == '-') {
            Pos a = app.caret; a.o--;
            doc_delete(D, a, app.caret, true); app.caret = app.anchor = a;
            char t[4]; int n = utf8_encode(0x2014, t); editor_insert_text(t, n); return;
        }
        if (cp == '-' && prev == 0x2014 && app.caret.o == 3 && p->len == 3) {
            Pos a = { app.caret.p, 0 };
            doc_delete(D, a, app.caret, true); app.caret = app.anchor = a;
            editor_insert_text("---", 3); return;
        }
        if (cp == '.' && app.caret.o >= 2 && p->s[app.caret.o - 1] == '.' && p->s[app.caret.o - 2] == '.') {
            Pos a = app.caret; a.o -= 2;
            doc_delete(D, a, app.caret, true); app.caret = app.anchor = a;
            char t[4]; int n = utf8_encode(0x2026, t); editor_insert_text(t, n); return;
        }
        if (cp == '"' || cp == '\'') {
            bool open = prev == 0 || cp_is_space(prev) || prev == '(' || prev == '[' || prev == 0x2014 || prev == 0x2013 || prev == i18n_quote(0) || prev == i18n_quote(2) || prev == 0xAB || prev == 0x201E;
            u32 r = i18n_quote((cp == '"' ? 0 : 2) + (open ? 0 : 1));   /* aspas do idioma (X-Quotes no .po) */
            char t[4]; int n = utf8_encode(r, t); editor_insert_text(t, n); return;
        }
    }
    char t[4]; int n = utf8_encode(cp, t);
    editor_insert_text(t, n);
}

static void do_enter(void) {
    doc_undo_break(D);
    Para *p = &D->p[app.caret.p];
    LineKind k = md_line(p->s, p->len, p->in_code);
    if (prefs.md_live && !has_sel() && (k.kind == LK_ULIST || k.kind == LK_OLIST || k.kind == LK_TASK || k.kind == LK_QUOTE) && app.caret.o >= k.prefix) {
        if (p->len <= k.prefix) {    /* item vazio encerra a lista */
            Pos a = { app.caret.p, 0 }, b = { app.caret.p, p->len };
            doc_delete(D, a, b, true); app.caret = app.anchor = a;
            touched(); caret_changed(false); doc_undo_break(D); return;
        }
        char pre[64]; int n = 0;
        int lead = 0; while (lead < p->len && (p->s[lead] == ' ' || p->s[lead] == '\t') && lead < 40) pre[n++] = p->s[lead++];
        if (k.kind == LK_ULIST) { pre[n++] = p->s[lead]; pre[n++] = ' '; }
        else if (k.kind == LK_TASK) { pre[n++] = p->s[lead]; memcpy(pre + n, " [ ] ", 5); n += 5; }
        else if (k.kind == LK_OLIST) {
            int j = lead; while (j < p->len && p->s[j] >= '0' && p->s[j] <= '9') j++;
            char delim = j < p->len ? p->s[j] : '.';
            n += fmt(pre + n, 20, "%d%c ", k.number + 1, delim);
        } else { int pl = MIN(k.prefix, 40); memcpy(pre, p->s, (usize)pl); n = pl; }
        char buf[80]; buf[0] = '\n'; memcpy(buf + 1, pre, (usize)n);
        editor_insert_text(buf, n + 1);
    } else editor_insert_text("\n", 1);
    doc_undo_break(D);
}

static void set_heading(int level) {
    doc_undo_break(D);
    Para *p = &D->p[app.caret.p];
    int i = 0; while (i < p->len && p->s[i] == '#') i++;
    if (i > 0 && i < p->len && p->s[i] == ' ') i++;
    else if (i > 0 && i != p->len) i = 0;
    Pos a = { app.caret.p, 0 }, b = { app.caret.p, i };
    int keep = app.caret.o - i;
    doc_delete(D, a, b, true);
    char pre[8]; int n = 0;
    for (int k = 0; k < level; k++) pre[n++] = '#';
    if (level) pre[n++] = ' ';
    if (n) doc_insert(D, a, pre, n, true);
    app.caret.o = MAX(0, keep) + n; app.anchor = app.caret;
    app.caret = doc_clamp(D, app.caret); app.anchor = app.caret;
    doc_undo_break(D); touched(); caret_changed(false);
}

static void wrap_sel(const char *mk) {
    int ml = str_len(mk);
    if (!has_sel()) {
        app.anchor = doc_word_left(D, doc_right(D, app.caret));
        Pos e = app.caret; Para *p = &D->p[e.p];
        while (e.o < p->len) { u32 c; utf8_decode(p->s, p->len, e.o, &c); if (!cp_is_word(c)) break; e.o = utf8_next(p->s, p->len, e.o); }
        app.caret = e;
        if (app.anchor.p != app.caret.p) app.anchor = app.caret;
        if (!has_sel()) { doc_undo_break(D); char t[16]; fmt(t, sizeof t, "%s%s", mk, mk); editor_insert_text(t, ml * 2); app.caret.o -= ml; app.anchor = app.caret; return; }
    }
    Pos a = pos_min(app.caret, app.anchor), b = pos_max(app.caret, app.anchor);
    doc_undo_break(D);
    Para *pa = &D->p[a.p], *pb = &D->p[b.p];
    bool wrapped = a.o >= ml && b.o + ml <= pb->len && memcmp(pa->s + a.o - ml, mk, (usize)ml) == 0 && memcmp(pb->s + b.o, mk, (usize)ml) == 0;
    if (wrapped) {
        Pos be = b; be.o += ml; doc_delete(D, b, be, true);
        Pos as = a; as.o -= ml; doc_delete(D, as, a, true);
        a.o -= ml; if (b.p == a.p) b.o -= 2 * ml; else b.o -= 0;
        if (b.p == a.p) b.o += ml;
    } else {
        doc_insert(D, b, mk, ml, true);
        doc_insert(D, a, mk, ml, true);
        a.o += ml; if (b.p == a.p) b.o += ml;
    }
    app.anchor = a; app.caret = b;
    doc_undo_break(D); touched(); caret_changed(false);
}

static void move_para(int dir) {
    int i = app.caret.p, j = i + dir;
    if (j < 0 || j >= D->n) return;
    doc_undo_break(D);
    int lo = MIN(i, j);
    int la, lb;
    char *A = doc_slice(D, (Pos){ lo, 0 }, (Pos){ lo, D->p[lo].len }, &la);
    char *B = doc_slice(D, (Pos){ lo + 1, 0 }, (Pos){ lo + 1, D->p[lo + 1].len }, &lb);
    int co = app.caret.o;
    doc_delete(D, (Pos){ lo, 0 }, (Pos){ lo + 1, D->p[lo + 1].len }, true);
    Buf t = {0}; buf_put(&t, B, (usize)lb); buf_put(&t, "\n", 1); buf_put(&t, A, (usize)la);
    doc_insert(D, (Pos){ lo, 0 }, t.s, (int)t.len, true);
    buf_free(&t); mem_free(A); mem_free(B);
    app.caret = app.anchor = doc_clamp(D, (Pos){ j, co });
    doc_undo_break(D); touched(); caret_changed(false);
}

static void indent_list(bool out) {
    Para *p = &D->p[app.caret.p];
    doc_undo_break(D);
    if (out) {
        int n = 0; while (n < 2 && n < p->len && p->s[n] == ' ') n++;
        if (!n) return;
        doc_delete(D, (Pos){ app.caret.p, 0 }, (Pos){ app.caret.p, n }, true);
        app.caret.o = MAX(0, app.caret.o - n);
    } else {
        doc_insert(D, (Pos){ app.caret.p, 0 }, "  ", 2, true);
        app.caret.o += 2;
    }
    app.anchor = app.caret;
    doc_undo_break(D); touched(); caret_changed(false);
}

/* ------------------------------------------------------------ capítulos, última alteração, parágrafos, maiúsculas */
bool editor_chapter(int dir) {
    if (!layout_ok) ensure_layout();
    int i = app.caret.p + dir;
    while (i >= 0 && i < D->n && D->p[i].kind != LK_H) i += dir;
    if (i < 0 || i >= D->n) {
        if (dir < 0 && (app.caret.p || app.caret.o)) { editor_goto((Pos){ 0, 0 }); return true; }
        return false;
    }
    app.caret = app.anchor = (Pos){ i, D->p[i].len };
    caret_changed(false);
    /* o título fica no alto da tela, com o capítulo à mostra embaixo */
    float x, y, h; pos_xy(app.caret, &x, &y, &h);
    scroll_target = CLAMP(top_pad + y - area_h * 0.18f, 0.0f, max_scroll());
    free_scroll = prefs.typewriter;
    return true;
}
void editor_jump_last_edit(void) {
    if (last_edit.p < 0) { ui_toast(L("There are no edits in this document yet")); return; }
    Pos to = doc_clamp(D, last_edit), here = app.caret;
    if (pos_cmp(here, to) == 0) { if (jump_back.p < 0) return; to = doc_clamp(D, jump_back); }
    jump_back = here;
    editor_goto(to);
}
static void sel_paras(int *a, int *b) {
    *a = *b = app.caret.p;
    if (has_sel()) {
        Pos s = pos_min(app.caret, app.anchor), e = pos_max(app.caret, app.anchor);
        *a = s.p; *b = (e.o == 0 && e.p > s.p) ? e.p - 1 : e.p;
    }
}
void editor_dup_para(void) {
    int a, b; sel_paras(&a, &b);
    int n; char *s = doc_slice(D, (Pos){ a, 0 }, (Pos){ b, D->p[b].len }, &n);
    doc_undo_break(D);
    Buf t = {0}; buf_put(&t, "\n", 1); buf_put(&t, s, (usize)n);
    doc_insert(D, (Pos){ b, D->p[b].len }, t.s, (int)t.len, true);
    buf_free(&t); mem_free(s);
    int off = b - a + 1;
    app.caret.p += off; app.anchor.p += off;
    app.caret = doc_clamp(D, app.caret); app.anchor = doc_clamp(D, app.anchor);
    doc_undo_break(D); touched(); caret_changed(false);
}
void editor_del_para(void) {
    if (prefs.hemingway) { ui_toast(L("Hemingway mode: keep going, no deleting")); return; }
    int a, b; sel_paras(&a, &b);
    Pos s, e;
    if (b + 1 < D->n) { s = (Pos){ a, 0 }; e = (Pos){ b + 1, 0 }; }
    else if (a > 0) { s = (Pos){ a - 1, D->p[a - 1].len }; e = (Pos){ b, D->p[b].len }; }
    else { s = (Pos){ 0, 0 }; e = (Pos){ b, D->p[b].len }; }
    doc_undo_break(D);
    doc_delete(D, s, e, true);
    app.caret = app.anchor = doc_clamp(D, (Pos){ MIN(a, D->n - 1), 0 });
    doc_undo_break(D); touched(); caret_changed(false);
    ui_toast(L("Paragraph deleted — Ctrl+Z undoes it"));
}
static u32 cp_up(u32 c) {
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c == 0x153) return 0x152;
    return c;
}
static u32 cp_low(u32 c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c == 0x178) return 0xFF;
    if (c == 0x152) return 0x153;
    return c;
}
/* Shift+F3: minúsculas -> Iniciais Maiúsculas -> MAIÚSCULAS -> minúsculas (seleção ou palavra) */
void editor_cycle_case(void) {
    if (!has_sel()) {
        Para *q = &D->p[app.caret.p];
        Pos a = app.caret, b = app.caret;
        while (a.o > 0) { u32 c; int pv = utf8_prev(q->s, a.o); utf8_decode(q->s, q->len, pv, &c); if (!cp_is_word(c)) break; a.o = pv; }
        while (b.o < q->len) { u32 c; utf8_decode(q->s, q->len, b.o, &c); if (!cp_is_word(c)) break; b.o = utf8_next(q->s, q->len, b.o); }
        if (pos_cmp(a, b) == 0) return;
        app.anchor = a; app.caret = b;
    }
    Pos a = pos_min(app.caret, app.anchor), b = pos_max(app.caret, app.anchor);
    int n; char *s = doc_slice(D, a, b, &n);
    bool any_low = false, title = true, start = true;
    for (int i = 0; i < n; ) {
        u32 c; i += utf8_decode(s, n, i, &c);
        bool low = cp_up(c) != c, up = cp_low(c) != c;
        if (low) any_low = true;
        if (cp_is_word(c)) { if ((start && low) || (!start && up)) title = false; start = false; } else start = true;
    }
    int mode = !any_low ? 0 : title ? 2 : 1;        /* 0 minúsculas, 1 iniciais, 2 maiúsculas */
    Buf t = {0}; start = true;
    for (int i = 0; i < n; ) {
        u32 c; i += utf8_decode(s, n, i, &c);
        bool w = cp_is_word(c);
        u32 r = mode == 0 ? cp_low(c) : mode == 2 ? cp_up(c) : (w ? (start ? cp_up(c) : cp_low(c)) : c);
        start = !w;
        char e[4]; int k = utf8_encode(r, e); buf_put(&t, e, (usize)k);
    }
    doc_undo_break(D);
    doc_delete(D, a, b, true);
    Pos end = doc_insert(D, a, t.s, (int)t.len, true);
    buf_free(&t); mem_free(s);
    app.anchor = a; app.caret = end;
    doc_undo_break(D); touched(); caret_changed(false);
}

static void vmove(int dir, bool extend) {
    if (!layout_ok) ensure_layout();
    float x, y, h; pos_xy(app.caret, &x, &y, &h);
    if (prefer_x < 0) prefer_x = x;
    Para *p = &D->p[app.caret.p];
    int li = line_of(p, app.caret.o) + dir;
    int pi = app.caret.p;
    if (li < 0) { if (pi == 0) { app.caret.o = 0; goto done; } pi--; li = D->p[pi].nlines - 1; }
    else if (li >= p->nlines) { if (pi + 1 >= D->n) { app.caret.o = p->len; goto done; } pi++; li = 0; }
    app.caret = hit_line(pi, li, prefer_x);
done:
    if (!extend) app.anchor = app.caret;
    caret_changed(true);
}

void editor_key(Event *e) {
    bool sh = (e->mods & MOD_SHIFT) != 0, ctrl = (e->mods & MOD_CTRL) != 0, alt = (e->mods & MOD_ALT) != 0;
    if (!layout_ok) ensure_layout();
    if (e->ch && ctrl) {
        switch (e->ch) {
        case 'a': editor_select_all(); return;
        case 'c': editor_copy(false); return;
        case 'x': editor_copy(true); return;
        case 'v': editor_paste(); return;
        case 'z': if (sh) goto redo; if (doc_undo(D, &app.caret)) { app.anchor = app.caret; touched(); caret_changed(false); } return;
        case 'y': redo: if (doc_redo(D, &app.caret)) { app.anchor = app.caret; touched(); caret_changed(false); } return;
        case 'b': wrap_sel("**"); return;
        case 'i': wrap_sel("*"); return;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': set_heading((int)(e->ch - '0')); return;
        case 'd': editor_dup_para(); return;
        case 'j': editor_jump_last_edit(); return;
        case 'k': if (sh) editor_del_para(); return;
        default: return;
        }
    }
    if (e->ch && alt && !ctrl && !(e->mods & MOD_ALTGR)) return;   /* Alt+letra sem atalho não digita (como Ctrl+letra) */
    /* método de entrada: qualquer tecla que não seja letra fecha a sílaba em composição */
    if (!(e->ch && !ctrl && e->ch >= 32) && e->key != KEY_BACKSPACE) ime_commit(&ed_ime);
    if (e->ch && !ctrl && e->ch >= 32 && ime_feed(e->ch, &ed_ime)) { if (app.last_edit_ms + 1200 < ui.in.now) doc_undo_break(D); return; }
    if (e->ch && !ctrl) {
        if (e->ch == 9) { Event t2 = *e; t2.ch = 0; t2.key = KEY_TAB; editor_key(&t2); return; }
        if (e->ch < 32) return;
        if (app.last_edit_ms + 1200 < ui.in.now) doc_undo_break(D);
        type_char(e->ch);
        if (e->ch == ' ' || e->ch == '.' || e->ch == ',') doc_undo_break(D);
        return;
    }
    Para *p = &D->p[app.caret.p];
    switch (e->key) {
    case KEY_ENTER: if (has_sel() && prefs.hemingway) return; do_enter(); return;
    case KEY_TAB: {
        LineKind k = md_line(p->s, p->len, p->in_code);
        if (k.kind == LK_ULIST || k.kind == LK_OLIST || k.kind == LK_TASK) indent_list(sh);
        else if (!sh) { doc_undo_break(D); editor_insert_text("    ", 4); }
        return; }
    case KEY_BACKSPACE:
        if (ime_backspace(&ed_ime)) return;      /* desfaz uma peça da sílaba em composição */
        if (prefs.hemingway) { ui_toast(L("Hemingway mode: keep going, no deleting")); return; }
        doc_undo_break(D);
        if (has_sel()) delete_sel();
        else {
            Pos a = ctrl ? doc_word_left(D, app.caret) : doc_left(D, app.caret);
            /* backspace no início de item de lista vazio remove o marcador */
            LineKind k = md_line(p->s, p->len, p->in_code);
            if (!ctrl && prefs.md_live && (k.kind == LK_ULIST || k.kind == LK_OLIST || k.kind == LK_TASK) && app.caret.o == k.prefix && k.prefix > 0)
                a = (Pos){ app.caret.p, 0 };
            doc_delete(D, a, app.caret, true); app.caret = app.anchor = a;
        }
        touched(); caret_changed(false); return;
    case KEY_DELETE:
        if (prefs.hemingway) { ui_toast(L("Hemingway mode: keep going, no deleting")); return; }
        doc_undo_break(D);
        if (has_sel()) delete_sel();
        else { Pos b = ctrl ? doc_word_right(D, app.caret) : doc_right(D, app.caret); doc_delete(D, app.caret, b, true); }
        app.anchor = app.caret;
        touched(); caret_changed(false); return;
    case KEY_LEFT:
        if (!sh && has_sel()) app.caret = pos_min(app.caret, app.anchor);
        else app.caret = ctrl ? doc_word_left(D, app.caret) : doc_left(D, app.caret);
        if (!sh) app.anchor = app.caret;
        doc_undo_break(D); caret_changed(false); return;
    case KEY_RIGHT:
        if (!sh && has_sel()) app.caret = pos_max(app.caret, app.anchor);
        else app.caret = ctrl ? doc_word_right(D, app.caret) : doc_right(D, app.caret);
        if (!sh) app.anchor = app.caret;
        doc_undo_break(D); caret_changed(false); return;
    case KEY_UP:
        if (alt) { move_para(-1); return; }
        if (ctrl) { app.caret.o = 0; if (app.caret.p > 0 && app.caret.o == 0) app.caret.p--; if (!sh) app.anchor = app.caret; caret_changed(false); return; }
        doc_undo_break(D); vmove(-1, sh); return;
    case KEY_DOWN:
        if (alt) { move_para(1); return; }
        if (ctrl) { if (app.caret.p + 1 < D->n) { app.caret.p++; app.caret.o = 0; } else app.caret.o = p->len; if (!sh) app.anchor = app.caret; caret_changed(false); return; }
        doc_undo_break(D); vmove(1, sh); return;
    case KEY_HOME:
        if (ctrl) app.caret = (Pos){ 0, 0 };
        else { int li = line_of(p, app.caret.o); app.caret.o = p->lines[li].start; }
        if (!sh) app.anchor = app.caret;
        caret_changed(false); return;
    case KEY_END:
        if (ctrl) app.caret = doc_end(D);
        else {
            int li = line_of(p, app.caret.o); int e2 = p->lines[li].end;
            if (li + 1 < p->nlines && e2 > p->lines[li].start && p->s[e2 - 1] == ' ') e2--;
            app.caret.o = e2;
        }
        if (!sh) app.anchor = app.caret;
        caret_changed(false); return;
    case KEY_PGUP: case KEY_PGDN: {
        if (ctrl) { if (!editor_chapter(e->key == KEY_PGUP ? -1 : 1) && e->key == KEY_PGDN) ui_toast(L("This is the last chapter")); return; }
        float x, y, h; pos_xy(app.caret, &x, &y, &h);
        float dy = (area_h - base_lh() * 2) * (e->key == KEY_PGUP ? -1 : 1);
        float ty = CLAMP(y + dy, 0.0f, doc_h - 1);
        app.caret = hit(prefer_x >= 0 ? prefer_x : x, ty);
        if (!sh) app.anchor = app.caret;
        scroll_target = CLAMP(scroll_target + dy, 0.0f, max_scroll());
        caret_changed(true); return; }
    default: return;
    }
}

/* ------------------------------------------------------------ busca */
void editor_find_highlight(const char *needle) { str_copy(find_needle, needle ? needle : "", sizeof find_needle); ui.redraw = true; }

bool editor_find_next(const char *needle, bool back) {
    int nl = str_len(needle);
    if (!nl) return false;
    Pos start = back ? pos_min(app.caret, app.anchor) : pos_max(app.caret, app.anchor);
    for (int pass = 0; pass <= D->n; pass++) {
        int pi = back ? (start.p - pass + D->n * 2) % D->n : (start.p + pass) % D->n;
        Para *p = &D->p[pi];
        if (!back) {
            int from = pass == 0 ? start.o : 0;
            int f = str_find_i(p->s, p->len, needle, nl, from);
            if (f >= 0) {
                int e = f, k = 0; while (k < nl) { u32 c; k += utf8_decode(needle, nl, k, &c); e = utf8_next(p->s, p->len, e); }
                app.anchor = (Pos){ pi, f }; app.caret = (Pos){ pi, e };
                editor_scroll_to_caret(true); return true;
            }
        } else {
            int last = -1, lastE = -1, from = 0;
            while (true) {
                int f = str_find_i(p->s, p->len, needle, nl, from);
                if (f < 0) break;
                int e = f, k = 0; while (k < nl) { u32 c; k += utf8_decode(needle, nl, k, &c); e = utf8_next(p->s, p->len, e); }
                if (pass == 0 && e > start.o) break;
                last = f; lastE = e; from = utf8_next(p->s, p->len, f);
            }
            if (last >= 0) { app.anchor = (Pos){ pi, last }; app.caret = (Pos){ pi, lastE }; editor_scroll_to_caret(true); return true; }
        }
        if (pass == D->n) break;
    }
    return false;
}
bool editor_replace_one(const char *needle, const char *rep) {
    if (has_sel()) {
        int n; char *s = doc_slice(D, app.caret, app.anchor, &n);
        bool match = str_find_i(s, n, needle, str_len(needle), 0) == 0 && n >= str_len(needle) - 3;
        mem_free(s);
        if (match) { doc_undo_break(D); editor_insert_text(rep, str_len(rep)); doc_undo_break(D); }
    }
    return editor_find_next(needle, false);
}
int editor_replace_all(const char *needle, const char *rep) {
    int nl = str_len(needle), rl = str_len(rep), count = 0;
    if (!nl) return 0;
    doc_undo_break(D);
    for (int pi = 0; pi < D->n; pi++) {
        int from = 0;
        while (true) {
            Para *p = &D->p[pi];
            int f = str_find_i(p->s, p->len, needle, nl, from);
            if (f < 0) break;
            int e = f, k = 0; while (k < nl) { u32 c; k += utf8_decode(needle, nl, k, &c); e = utf8_next(p->s, p->len, e); }
            doc_delete(D, (Pos){ pi, f }, (Pos){ pi, e }, true);
            doc_insert(D, (Pos){ pi, f }, rep, rl, true);
            from = f + rl; count++;
        }
    }
    doc_undo_break(D);
    app.caret = app.anchor = doc_clamp(D, app.caret);
    if (count) touched();
    return count;
}

void editor_goto(Pos p) { app.caret = app.anchor = doc_clamp(D, p); caret_changed(false); editor_scroll_to_caret(true); }

/* ------------------------------------------------------------ mouse */
void editor_mouse(Rect area) {
    Input *in = &ui.in;
    if (!ui_mouse_ok()) { if (!in->down) selecting = false; return; }
    Rect text_area = rect(col_x - SI(60), area.y, (int)col_w + SI(120), area.h);
    if (rect_has(area, in->mx, in->my) && rect_has(text_area, in->mx, in->my) && in->my >= ui.block_top && in->my < ui.H - ui.block_bottom) ui.cursor = 1;
    if (in->wheel && rect_has(area, in->mx, in->my)) {
        scroll_target = CLAMP(scroll_target - in->wheel * base_lh() * 3, 0.0f, max_scroll());
        free_scroll = true; ui.redraw = true;
    }
    if (!layout_ok) ensure_layout();
    float dx = (float)(in->mx - col_x), dy = (float)(in->my - area_y) + scroll_y - top_pad;
    bool in_chrome = in->my < ui.block_top || in->my >= ui.H - ui.block_bottom;
    if (in->pressed && rect_has(area, in->mx, in->my) && !in_chrome && ui.active != (void *)&scroll_target) {
        ime_commit(&ed_ime);
        Pos p = hit(dx, CLAMP(dy, 0.0f, doc_h - 1));
        selecting = true;
        sel_mode = in->triple ? 2 : in->dbl ? 1 : 0;
        if (sel_mode == 0) {
            app.caret = p;
            bool sh = false;
            for (int i = 0; i < in->nkeys; i++) {}
            extern int app_mods(void);
            sh = (app_mods() & MOD_SHIFT) != 0;
            if (!sh) app.anchor = p;
        } else if (sel_mode == 1) {
            Para *q = &D->p[p.p];
            Pos a = p, b = p;
            while (a.o > 0) { u32 c; int pv = utf8_prev(q->s, a.o); utf8_decode(q->s, q->len, pv, &c); if (!cp_is_word(c)) break; a.o = pv; }
            while (b.o < q->len) { u32 c; utf8_decode(q->s, q->len, b.o, &c); if (!cp_is_word(c)) break; b.o = utf8_next(q->s, q->len, b.o); }
            app.anchor = a; app.caret = b;
        } else {
            app.anchor = (Pos){ p.p, 0 }; app.caret = (Pos){ p.p, D->p[p.p].len };
        }
        sel_origin_a = pos_min(app.anchor, app.caret); sel_origin_b = pos_max(app.anchor, app.caret);
        doc_undo_break(D);
        prefer_x = -1; caret_moved_ms = in->now;
        if (prefs.hide_markup && app.caret.p != last_caret_para) { layout_ok = false; last_caret_para = app.caret.p; }
        ui.redraw = true;
    } else if (selecting && in->down && in->moved) {
        Pos p = hit(dx, CLAMP(dy, 0.0f, doc_h - 1));
        if (sel_mode == 0) app.caret = p;
        else {
            if (pos_cmp(p, sel_origin_a) < 0) { app.anchor = sel_origin_b; app.caret = sel_mode == 2 ? (Pos){ p.p, 0 } : doc_word_left(D, doc_right(D, p)); if (sel_mode==1 && pos_cmp(app.caret,p)>0) app.caret = p; }
            else { app.anchor = sel_origin_a; app.caret = sel_mode == 2 ? (Pos){ p.p, D->p[p.p].len } : p; }
        }
        if (in->my < area.y + SI(20)) scroll_target = MAX(0.0f, scroll_target - base_lh());
        if (in->my > area.y + area.h - SI(20)) scroll_target = MIN(max_scroll(), scroll_target + base_lh());
        ui.redraw = true;
    }
    if (!in->down) selecting = false;
}

/* ------------------------------------------------------------ desenho */
static void sentence_bounds(Para *p, int o, int *s0, int *s1) {
    int a = 0, b = p->len;
    for (int i = MIN(o, p->len) - 1; i > 0; i--) {
        char c = p->s[i - 1];
        if ((c == '.' || c == '!' || c == '?' || (u8)c == 0xA6) && p->s[i] == ' ') { a = i + 1; break; }
    }
    for (int i = o; i < p->len; i++) {
        char c = p->s[i];
        if ((c == '.' || c == '!' || c == '?' || (u8)c == 0xA6) && (i + 1 >= p->len || p->s[i + 1] == ' ')) { b = i + 1; break; }
    }
    *s0 = a; *s1 = b;
}

/* barra de rolagem fina à direita, com marcas dos capítulos; aparece junto com as barras */
static Rect  sb_track; static float sb_alpha, sb_content;
static void scrollbar_input(Rect area, float chrome_a) {
    Input *in = &ui.in;
    sb_content = top_pad + doc_h + bot_pad;
    sb_track = rect(area.x + area.w - SI(12), area.y + SI(64), SI(5), area.h - SI(128));
    bool dragging = ui.active == (void *)&scroll_target;
    sb_alpha = (sb_content > area.h * 1.3f && sb_track.h > SI(60)) ? chrome_a : 0;
    if (sb_alpha < 0.05f && !dragging) return;
    float maxs = max_scroll(), th = MAX(S(28), sb_track.h * area.h / sb_content);
    Rect hot = rect(sb_track.x - SI(8), sb_track.y, sb_track.w + SI(14), sb_track.h);
    if (ui_hover(hot)) ui.cursor = 0;
    if (in->pressed && ui_hover(hot)) { ui.active = (void *)&scroll_target; dragging = true; }
    if (dragging) {
        if (!in->down) { ui.active = 0; return; }
        float t = (in->my - sb_track.y - th / 2) / MAX(1.0f, sb_track.h - th);
        scroll_target = scroll_y = CLAMP(t, 0.0f, 1.0f) * maxs;
        free_scroll = true; ui.redraw = true;
    }
}
static void scrollbar_draw(void) {
    if (sb_alpha < 0.05f) return;
    Rect t = sb_track;
    float maxs = max_scroll(), th = MAX(S(28), t.h * area_h / sb_content);
    gfx_set_opacity(sb_alpha);
    for (int i = 0; i < D->n; i++) {
        Para *p = &D->p[i];
        if (p->kind != LK_H || p->level > 2) continue;
        int y = t.y + (int)((t.h - th) * CLAMP((top_pad + py[i] - area_h * 0.18f) / MAX(1.0f, maxs), 0.0f, 1.0f) + th * 0.5f);
        gfx_rect(rect(t.x - SI(5), y, p->level == 1 ? SI(14) : SI(10), MAX(1, SI(2))), col_alpha(T->accent, 0.55f));
    }
    float ty = t.y + (t.h - th) * (maxs > 0 ? scroll_y / maxs : 0);
    bool hv = ui_hover(rect(t.x - SI(8), t.y, t.w + SI(14), t.h)) || ui.active == (void *)&scroll_target;
    gfx_rrect(rect(t.x, (int)ty, t.w, (int)th), t.w * 0.5f, col_alpha(T->text, hv ? 0.35f : 0.18f));
    gfx_set_opacity(1);
}

void editor_draw(Rect area, float chrome_a) {
    Input *in = &ui.in;
    float new_w = app_text_w((float)area.w);
    if (new_w < S(200)) new_w = MIN(S(200), (float)area.w - S(16));
    if (new_w != col_w) { col_w = new_w; editor_invalidate(); }
    col_x = area.x + (int)((area.w - col_w) / 2);
    area_y = area.y; area_h = area.h;
    top_pad = prefs.typewriter ? area.h * 0.45f : MAX(S(48), area.h * 0.12f);
    bot_pad = prefs.typewriter ? area.h * 0.55f : area.h * 0.45f;
    if (!layout_ok) ensure_layout();
    if (app.caret.p >= D->n) { app.caret = app.anchor = doc_clamp(D, app.caret); }

    scrollbar_input(area, chrome_a);
    editor_mouse(area);

    /* rolagem suave */
    float diff = scroll_target - scroll_y;
    if (f_abs(diff) > 0.5f) { scroll_y += diff * MIN(1.0f, in->dt * 14.0f); ui.redraw = true; }
    else scroll_y = scroll_target;
    scroll_y = CLAMP(scroll_y, 0.0f, MAX(max_scroll(), scroll_y));

    /* folha de papel */
    if (app_sheet_on()) app_paper_sheet(rect(col_x - SI(72), area.y - SI(30), (int)col_w + SI(144), area.h + SI(60)));

    gfx_push_clip(area);
    Pos sa = pos_min(app.caret, app.anchor), sb = pos_max(app.caret, app.anchor);
    bool sel = pos_cmp(sa, sb) != 0;
    float y0 = area.y + top_pad - scroll_y;
    int first = para_at_y(scroll_y - top_pad - S(40));
    int cs0 = 0, cs1 = 0, cline = -1;
    Para *cp_ = &D->p[app.caret.p];
    if (prefs.focus_mode == FOCUS_SENTENCE) sentence_bounds(cp_, app.caret.o, &cs0, &cs1);
    if (prefs.focus_mode == FOCUS_LINE) cline = line_of(cp_, app.caret.o);
    int nl_needle = str_len(find_needle);

    for (int pi = first; pi < D->n; pi++) {
        Para *p = &D->p[pi];
        float ptop = y0 + py[pi];
        if (ptop > area.y + area.h + S(40)) break;
        float yy = ptop + pad_top(p);
        bool code = para_is_code(p);
        if (code && prefs.md_live) {
            float bottom = ptop + p->height;
            if (pi + 1 < D->n && para_is_code(&D->p[pi + 1]) && p->kind != LK_FENCE) bottom = y0 + py[pi + 1];
            if (p->kind == LK_FENCE && !p->in_code && pi + 1 < D->n) bottom = y0 + py[pi + 1];
            gfx_rect(rect(col_x - SI(14), (int)ptop, (int)col_w + SI(28), (int)(bottom - ptop + 0.99f)), T->code_bg);
        }
        if (p->kind == LK_QUOTE && prefs.md_live) {
            gfx_rrect(rect(col_x - SI(16), (int)yy + SI(4), SI(3), (int)(p->nlines * p->lh) - SI(8)), S(1.5f), col_alpha(T->accent, 0.55f));
        }
        if (p->kind == LK_HR && prefs.md_live) {
            float cy = yy + p->lh * 0.5f;
            gfx_rect(rect(col_x + (int)(col_w * 0.3f), (int)cy, (int)(col_w * 0.4f), MAX(1, SI(1))), col_alpha(T->muted, 0.6f));
        }
        float asc = face_ascent(fam_face(font_get(prefs.font), 0), p->size);
        float dsc = face_descent(fam_face(font_get(prefs.font), 0), p->size);
        /* foco: parágrafo inteiro apagado? */
        bool dim_para = false;
        if (prefs.focus_mode != FOCUS_OFF && pi != app.caret.p) dim_para = true;
        for (int li = 0; li < p->nlines; li++) {
            VLine *l = &p->lines[li];
            float ly = yy + li * p->lh;
            if (ly + p->lh < area.y) continue;
            if (ly > area.y + area.h) break;
            float base = ly + (p->lh - (asc + dsc)) * 0.5f + asc;
            /* seleção */
            if (sel && pos_cmp((Pos){ pi, l->end }, sa) >= 0 && pos_cmp((Pos){ pi, l->start }, sb) <= 0) {
                int s0 = (pi == sa.p) ? MAX(sa.o, l->start) : l->start;
                int s1 = (pi == sb.p) ? MIN(sb.o, l->end) : l->end;
                float x0 = x_at(p, li, s0), x1 = x_at(p, li, s1);
                if (pi != sb.p && li == p->nlines - 1) x1 += p->size * 0.3f;
                if (x1 > x0) gfx_rrect(rect(col_x + (int)x0, (int)(ly + p->lh * 0.08f), (int)(x1 - x0 + 0.5f), (int)(p->lh * 0.86f)), S(3), T->select);
            }
            /* destaques da busca */
            if (nl_needle) {
                int from = l->start;
                while (true) {
                    int f = str_find_i(p->s, l->end, find_needle, nl_needle, from);
                    if (f < 0) break;
                    int e = f, k = 0; while (k < nl_needle) { u32 c; k += utf8_decode(find_needle, nl_needle, k, &c); e = utf8_next(p->s, p->len, e); }
                    float x0 = x_at(p, li, f), x1 = x_at(p, li, MIN(e, l->end));
                    gfx_rrect(rect(col_x + (int)x0 - SI(1), (int)(ly + p->lh * 0.12f), (int)(x1 - x0) + SI(2), (int)(p->lh * 0.78f)), S(3), col_alpha(T->mark, 0.85f));
                    from = utf8_next(p->s, p->len, f);
                }
            }
            /* texto */
            GIt g; GOut o; git_init(&g, p, l->start, l->end, line_x(p, li));
            bool dim_line = dim_para || (prefs.focus_mode == FOCUS_LINE && pi == app.caret.p && li != cline);
            float code_x0 = -1;
            while (git_next(&g, &o)) {
                if (o.hid) continue;
                Color c = (p->kind == LK_H) ? T->heading : T->text;
                if (o.st & ST_MARK) c = T->muted;
                if (o.cp == 0x2022) c = T->accent;
                if (o.st & ST_LINK) c = T->accent;
                bool dim = dim_line;
                if (!dim && prefs.focus_mode == FOCUS_SENTENCE && pi == app.caret.p && (o.b < cs0 || o.b >= cs1)) dim = true;
                if (dim) c = col_mix(c, app_sheet_on() ? T->paper : T->desk, T->dark ? 0.72f : 0.68f);
                float gx = col_x + o.x;
                if ((o.st & ST_CODE) && !code && !(o.st & ST_MARK)) {
                    if (code_x0 < 0) code_x0 = gx;
                } else if (code_x0 >= 0 && !code) {
                    code_x0 = -1;
                }
                if ((o.st & ST_CODE) && !code) {
                    gfx_rect(rect((int)gx, (int)(base - asc * 0.82f), (int)(o.adv + 0.99f), (int)(asc * 0.82f + dsc * 0.9f)), col_alpha(T->code_bg, 1));
                }
                if (o.cp != ' ' && o.cp != '\t') {
                    float fx = f_floor(gx); int sub = (int)((gx - fx) * 4) & 3;
                    const Glyph *gl = face_glyph(o.f, o.gid, o.px, sub);
                    if (gl && gl->bmp) gfx_mask((int)fx + gl->ox, (int)f_round(base) + gl->oy, gl->bmp, gl->w, gl->h, gl->w, c);
                }
                if (o.st & ST_STRIKE) gfx_rect(rect((int)gx, (int)(base - face_xheight(o.f, o.px) * 0.5f), (int)(o.adv + 0.99f), MAX(1, SI(1.2f))), c);
                if ((o.st & ST_LINK) && !(o.st & ST_MARK)) gfx_rect(rect((int)gx, (int)(base + S(2.5f)), (int)(o.adv + 0.99f), MAX(1, SI(1))), col_alpha(c, 0.5f));
            }
        }
    }

    /* cursor */
    {
        float x, y, h; pos_xy(app.caret, &x, &y, &h);
        Para *p = &D->p[app.caret.p];
        float asc = face_ascent(fam_face(font_get(prefs.font), 0), p->size), dsc = face_descent(fam_face(font_get(prefs.font), 0), p->size);
        float ch = MIN(h, (asc + dsc) * 1.12f);
        float tx = col_x + x, ty = y0 + y + (h - ch) * 0.5f;
        if (caret_dx < 0 || !prefs.smooth_caret || f_abs(tx - caret_dx) > S(400) || f_abs(ty - caret_dy) > S(300)) { caret_dx = tx; caret_dy = ty; caret_dh = ch; }
        else {
            float k = MIN(1.0f, in->dt * 28.0f);
            caret_dx += (tx - caret_dx) * k; caret_dy += (ty - caret_dy) * k; caret_dh += (ch - caret_dh) * k;
            if (f_abs(tx - caret_dx) > 0.3f || f_abs(ty - caret_dy) > 0.3f) ui.redraw = true;
            else { caret_dx = tx; caret_dy = ty; caret_dh = ch; }
        }
        /* piscar suave: aceso 0,5 s, apaga em 0,1 s, apagado 0,3 s, acende em 0,1 s */
        float alpha = 1;
        if (prefs.caret_blink) {
            u64 since = in->now - caret_moved_ms;
            if (since > 700) {
                float ph = (float)((since - 700) % 1000) / 1000.0f;
                if (ph < 0.5f) alpha = 1;
                else if (ph < 0.6f) alpha = 1 - (ph - 0.5f) * 10 * 0.85f;
                else if (ph < 0.9f) alpha = 0.15f;
                else alpha = 0.15f + (ph - 0.9f) * 10 * 0.85f;
            }
        }
        if (!ui.key_win) gfx_rrect(rect((int)caret_dx, (int)caret_dy, MAX(2, SI(2.2f)), (int)caret_dh), S(1), col_alpha(T->caret, alpha));
    }
    gfx_pop_clip();
    scrollbar_draw();
}

/* área da folha de papel (para as barras se fundirem a ela); w = 0 sem folha */
Rect editor_sheet_rect(void) {
    if (!(app_sheet_on()) || col_w <= 0) return rect(0, 0, 0, 0);
    if (preview_active()) { float w = app_text_w((float)ui.W); int cx = (int)((ui.W - w) / 2); return rect(cx - SI(80), 0, (int)w + SI(160), ui.H); }
    return rect(col_x - SI(72), 0, (int)col_w + SI(144), ui.H);
}
