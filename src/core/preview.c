/* FOLIO v4 — pré-visualização Markdown (modo leitura) */
#include "folio.h"

typedef struct { u32 cp; u8 st; } SC;
typedef struct { int s, e; float w; } PL;
typedef struct { MdBlock *b; float y, h, size, lh, indent, top; int sc0, nsc, l0, nl, words_before; } PB;

static bool   active;
static MdDoc  md;
static SC    *scs; static int nscs, cscs;
static PL    *pls; static int npls, cpls;
static PB    *pbs; static int npbs;
static float  total_h, scroll, target, built_w; static u32 built_ver; static int built_font;
static float  built_scale, built_size;
static int    total_words;
static int    pend_src = -1; static bool pend_top;   /* posição a aplicar quando o layout ficar pronto */
static float  view_h = 600, view_top = 64, max_s;    /* última área desenhada */

/* Rolagem automática (estudo em docs/ESTUDO_ROLAGEM.md). Só liga por ação explícita: o botão da
 * barra de leitura ou um clique do botão do meio. Nunca é restaurada ao abrir a leitura. */
static bool   built(void);
static bool   auto_on;                 /* pedida pelo usuário */
static float  auto_v;                  /* velocidade atual em px/s (com rampa de partida e parada) */
static bool   pan_on, pan_drag;        /* botão do meio segurado / arrastado */
static float  pan_x, pan_y, pan_v; static u64 pan_t;
static Rect   bar_r, bar_btn[BAR_COUNT];   /* barra de controles da leitura e seus botões */
static bool   wpm_dirty; static u64 wpm_t;

bool preview_active(void) { return active; }
bool preview_auto_on(void) { return active && auto_on; }
float preview_scroll_pos(void) { return scroll; }
Rect preview_bar_button(int i) { return bar_btn[CLAMP(i, 0, BAR_COUNT - 1)]; }
/* barras e ponteiro podem sumir: rolando sozinho, mouse parado e longe dos controles */
bool preview_auto_idle(u64 idle_ms) {
    return active && auto_on && !pan_on && idle_ms > 2500 && !rect_has(bar_r, ui.in.mx, ui.in.my);
}
static void wpm_save(void) { if (wpm_dirty) { wpm_dirty = false; prefs_save(); } }
static void auto_set(bool on) {
    auto_on = on;
    wpm_save();
    ui.redraw = true;
}
void preview_toggle_auto(void) {
    if (!active) return;
    if (!auto_on && built()) {
        if (max_s < 1) { ui_toast(L("The whole text already fits on the screen")); return; }
        if (target >= max_s - 1) { ui_toast(L("Already at the end of the text")); return; }
    }
    auto_set(!auto_on);
}
/* Esc: interrompe o que estiver em movimento (arrasto do meio ou rolagem automática).
 * Nunca liga nada: devolve false se não havia o que parar. */
bool preview_stop_motion(void) {
    if (!active) return false;
    if (pan_on) { pan_on = pan_drag = false; pan_v = 0; ui.redraw = true; return true; }
    if (auto_on) { auto_set(false); return true; }
    return false;
}
static void wpm_step(int d) {
    int v = CLAMP(prefs.read_wpm + d * 20, 60, 900);
    if (v == prefs.read_wpm) return;
    prefs.read_wpm = v; wpm_dirty = true; wpm_t = ui.in.now; ui.redraw = true;
}

static void sc_push(u32 cp, u8 st) {
    if (nscs >= cscs) { cscs = cscs ? cscs * 2 : 4096; scs = mem_realloc(scs, sizeof(SC) * (usize)cscs); }
    scs[nscs].cp = cp; scs[nscs].st = st; nscs++;
}
static void pl_push(int s, int e, float w) {
    if (npls >= cpls) { cpls = cpls ? cpls * 2 : 1024; pls = mem_realloc(pls, sizeof(PL) * (usize)cpls); }
    pls[npls].s = s; pls[npls].e = e; pls[npls].w = w; npls++;
}

#define PST_HEAD 0x80
static Face *pface(PB *b, u8 st) {
    bool code = (st & ST_CODE) || b->b->kind == MB_CODE;
    Family *fam = code ? font_mono() : font_get(prefs.font);
    int s = 0;
    if ((st & ST_BOLD) || b->b->kind == MB_H) s |= 1;
    if ((st & ST_ITALIC) || b->b->kind == MB_QUOTE) s |= 2;
    return fam_face(fam, s);
}
static float ppx(PB *b, u8 st) { return (b->b->kind == MB_CODE || (st & ST_CODE)) ? b->size * 0.87f : b->size; }

static void free_all(void) { md_free(&md); npbs = 0; total_words = 0; mem_free(pbs); pbs = 0; nscs = 0; npls = 0; }

static void build(float w) {
    free_all();
    md_parse(&app.doc, &md);
    pbs = mem_zalloc(sizeof(PB) * (usize)(md.n + 1));
    npbs = md.n;
    float base = prefs.font_size * ui.scale * 1.02f;
    float y = 0;
    for (int i = 0; i < md.n; i++) {
        PB *b = &pbs[i]; MdBlock *m = &md.b[i];
        b->b = m;
        b->size = base;
        if (m->kind == MB_H) b->size = base * (m->level == 1 ? 1.9f : m->level == 2 ? 1.45f : m->level == 3 ? 1.2f : 1.05f);
        b->lh = m->kind == MB_H ? b->size * 1.28f : m->kind == MB_CODE ? base * 1.35f : base * prefs.line_height;
        b->indent = 0;
        if (m->kind == MB_ULIST || m->kind == MB_OLIST || m->kind == MB_TASK) b->indent = base * (1.7f + 1.5f * m->indent);
        if (m->kind == MB_QUOTE) b->indent = base * 1.4f;
        if (m->kind == MB_CODE) b->indent = base * 1.0f;
        b->top = m->kind == MB_H ? b->size * (m->level <= 2 ? 1.0f : 0.7f) : 0;
        if (i == 0) b->top = 0;
        b->sc0 = nscs;
        if (m->kind == MB_CODE) {
            for (int k = 0; k < m->len; ) { u32 c; k += utf8_decode(m->text, m->len, k, &c); sc_push(c, ST_CODE); }
        } else if (m->kind != MB_HR && m->kind != MB_TABLE) {
            MdRun runs[512]; int n = md_runs(m->text, m->len, runs, 512);
            for (int r = 0; r < n; r++)
                for (int k = 0; k < runs[r].len; ) { u32 c; k += utf8_decode(runs[r].s, runs[r].len, k, &c); sc_push(c, runs[r].sty); }
        }
        b->nsc = nscs - b->sc0;
        b->words_before = total_words;
        { bool inw = false; for (int k = b->sc0; k < b->sc0 + b->nsc; k++) { bool wd = cp_is_word(scs[k].cp); if (wd && !inw) total_words++; inw = wd; } }
        if (m->kind == MB_TABLE) {          /* tabela também se lê: conta as palavras das células */
            bool inw = false;
            for (int k = 0; k < m->len; ) { u32 c; k += utf8_decode(m->text, m->len, k, &c); bool wd = cp_is_word(c); if (wd && !inw) total_words++; inw = wd; }
        }
        /* quebra de linhas */
        b->l0 = npls;
        float maxw0 = w - b->indent - (m->kind == MB_CODE ? base : 0);
        float ind0 = (m->kind == MB_PARA && prefs.first_indent && i > 0 && md.b[i - 1].kind == MB_PARA) ? base * 1.6f : 0;
        int s = b->sc0, end = b->sc0 + b->nsc;
        while (s < end) {
            float x = 0; int brk = -1; float brk_w = 0; int i2 = s; u16 prev = 0; Face *pf = 0; float lastw = 0;
            bool hard = false;
            while (i2 < end) {
                SC *c = &scs[i2];
                if (c->cp == '\n') { hard = true; break; }
                Face *f = pface(b, c->st); float px = ppx(b, c->st);
                u16 g = face_gid_fb(&f, c->cp == '\t' ? ' ' : c->cp);
                float adv = face_adv(f, g, px) * (c->cp == '\t' ? 4 : 1);
                if (prev && pf == f) x += face_kern(f, prev, g, px);
                float maxw = maxw0 - (npls == b->l0 ? ind0 : 0);
                if (c->cp != ' ' && x + adv > maxw && i2 > s && m->kind != MB_CODE) break;
                if (c->cp == ' ') { brk = i2 + 1; brk_w = x; }
                x += adv; prev = g; pf = f; i2++;
                if (c->cp != ' ') lastw = x;
            }
            if (hard) { pl_push(s, i2, lastw); s = i2 + 1; continue; }
            if (i2 >= end) { pl_push(s, end, lastw); break; }
            if (brk > s) { pl_push(s, brk, brk_w); s = brk; }
            else { pl_push(s, i2, lastw); s = i2; }
        }
        if (npls == b->l0) pl_push(b->sc0, b->sc0, 0);
        b->nl = npls - b->l0;
        float h = b->nl * b->lh;
        if (m->kind == MB_HR) h = base * 2.2f;
        if (m->kind == MB_TABLE) { int rows = 0; for (int k = 0; k < m->len; k++) if (m->text[k] == '\n') rows++; h = (rows + 1) * base * 1.9f; }
        if (m->kind == MB_CODE) h += base * 1.0f;
        b->h = h;
        y += b->top;
        b->y = y;
        y += h;
        bool next_list = i + 1 < md.n && (md.b[i + 1].kind == MB_ULIST || md.b[i + 1].kind == MB_OLIST || md.b[i + 1].kind == MB_TASK);
        bool list = m->kind == MB_ULIST || m->kind == MB_OLIST || m->kind == MB_TASK;
        y += (list && next_list) ? base * 0.25f : base * 0.85f;
    }
    total_h = y;
    built_w = w; built_ver = app.doc.version; built_font = prefs.font; built_scale = ui.scale; built_size = prefs.font_size;
}

/* a "linha de leitura" fica a 30% da altura: é por ela que leitura e escrita trocam de lugar */
static float read_off(void) { return view_h * 0.3f - view_top; }
static float head_y(PB *b) { return b->y - b->top * 0.6f; }
static int block_at(float Y) {
    int i = 0;
    for (int k = 0; k < npbs; k++) { if (pbs[k].y - pbs[k].top <= Y) i = k; else break; }
    return i;
}
static int block_for_src(int p) {
    int i = 0;
    for (int k = 0; k < npbs; k++) { if (pbs[k].b->src <= p) i = k; else break; }
    return i;
}
static bool built(void) { return npbs > 0 && pend_src < 0; }

/* abre no ponto onde o cursor estava no editor */
void preview_open(void) {
    if (app.syntax == SYN_TEX) { ui_toast(L("Reading mode is not available for LaTeX files")); return; }   /* a leitura formata Markdown */
    active = true; scroll = target = 0; built_ver = 0xFFFFFFFF; built_w = 0;
    pend_src = app.caret.p; pend_top = false;
    auto_on = pan_on = pan_drag = false; auto_v = pan_v = 0;
    ui.redraw = true;
}
void preview_close(void) {
    wpm_save();
    active = false; auto_on = pan_on = pan_drag = false; auto_v = pan_v = 0;
    free_all(); pend_src = -1; ui.redraw = true;
}

/* parágrafo do texto-fonte que está na linha de leitura */
int preview_top_para(void) {
    if (!active) return app.caret.p;
    if (pend_src >= 0 || !npbs) return pend_src >= 0 ? pend_src : app.caret.p;
    return pbs[block_at(scroll + read_off())].b->src;
}
/* volta a escrever exatamente no trecho que estava sendo lido */
void preview_leave(void) {
    if (!active) return;
    int p = preview_top_para();
    preview_close();
    app.caret = app.anchor = doc_clamp(&app.doc, (Pos){ p, 0 });
    editor_scroll_to_caret(true);
}
void preview_goto_para(int p, bool top) {
    if (!built()) { pend_src = p; pend_top = top; ui.redraw = true; return; }
    PB *b = &pbs[block_for_src(p)];
    target = CLAMP(top ? head_y(b) : b->y - read_off(), 0.0f, max_s);
    ui.redraw = true;
}
/* capítulo (título) anterior/seguinte; devolve false se não há */
bool preview_chapter(int dir) {
    if (!built()) return false;
    int pick = -1;
    if (dir > 0) { for (int k = 0; k < npbs; k++) if (pbs[k].b->kind == MB_H && head_y(&pbs[k]) > target + 4) { pick = k; break; } }
    else { for (int k = npbs - 1; k >= 0; k--) if (pbs[k].b->kind == MB_H && head_y(&pbs[k]) < target - 4) { pick = k; break; } }
    if (pick < 0) { if (dir < 0 && target > 0) { target = 0; ui.redraw = true; return true; } return false; }
    target = CLAMP(head_y(&pbs[pick]), 0.0f, max_s);
    ui.redraw = true;
    return true;
}
/* progresso: % lido, palavras até o fim do capítulo e do texto, título do capítulo */
bool preview_info(char *chap, int cap, int *pct, int *chap_left, int *left) {
    if (!built() || !total_words) return false;
    int cur = block_at(scroll + read_off());
    int done = pbs[cur].words_before;
    bool at_end = scroll >= max_s - 1;
    int h = -1, nx = -1;
    for (int k = cur; k >= 0; k--) if (pbs[k].b->kind == MB_H) { h = k; break; }
    for (int k = cur + 1; k < npbs; k++) if (pbs[k].b->kind == MB_H) { nx = k; break; }
    *left = at_end ? 0 : total_words - done;
    *chap_left = at_end ? 0 : (nx >= 0 ? pbs[nx].words_before : total_words) - done;
    *pct = at_end ? 100 : (int)((u64)done * 100 / (u64)total_words);
    chap[0] = 0;
    if (h >= 0) { int pl; char *pt = md_plain(pbs[h].b->text, pbs[h].b->len, &pl); str_copy(chap, pt, MIN(cap, pl + 1)); mem_free(pt); }
    return true;
}

void preview_key(Event *e) {
    float step = prefs.font_size * ui.scale * 3;
    float page = view_h * 0.85f;
    bool ctrl = (e->mods & MOD_CTRL) != 0;
    switch (e->key) {
    case KEY_UP: target -= step; break;
    case KEY_DOWN: target += step; break;
    case KEY_LEFT: target -= page; break;
    case KEY_RIGHT: target += page; break;
    case KEY_PGUP: if (ctrl) { preview_chapter(-1); return; } target -= page; break;
    case KEY_PGDN: if (ctrl) { if (!preview_chapter(1)) ui_toast(L("This is the last chapter")); return; } target += page; break;
    case KEY_HOME: target = 0; break;
    case KEY_END: target = total_h; break;
    case KEY_ENTER: preview_toggle_auto(); return;     /* rolagem automática sem o botão do meio */
    default:
        if (e->ch == ' ') target += (e->mods & MOD_SHIFT) ? -page : page;
        else if (e->ch == '+' || e->ch == '=' || e->ch == '-') {     /* velocidade da rolagem automática */
            wpm_step(e->ch == '-' ? -1 : 1);
            char m[64]; fmt(m, sizeof m, L("Reading: %d words per minute"), prefs.read_wpm); ui_toast(m);
        }
        break;
    }
    ui.redraw = true;
}

/* ------------------------------------------------------------ rolagem automática */
/* Velocidade em palavras por minuto, convertida para px/s pela densidade de palavras da janela de
 * leitura (60% da tela a partir da linha de leitura). Títulos, linhas em branco e separadores passam
 * mais depressa, parágrafos densos mais devagar, e o ritmo de leitura fica constante. Como a janela
 * é larga e desliza continuamente, a velocidade muda aos poucos, sem trancos. */
static float auto_speed(void) {
    float Y0 = scroll + read_off() - view_h * 0.1f, Y1 = Y0 + view_h * 0.6f;
    float words = 0;
    for (int k = 0; k < npbs; k++) {
        float top = pbs[k].y - pbs[k].top;
        float bot = k + 1 < npbs ? pbs[k + 1].y - pbs[k + 1].top : total_h;
        if (bot <= Y0) continue;
        if (top >= Y1) break;
        int wk = (k + 1 < npbs ? pbs[k + 1].words_before : total_words) - pbs[k].words_before;
        words += wk * (MIN(bot, Y1) - MAX(top, Y0)) / MAX(1.0f, bot - top);
    }
    float dens = words / (Y1 - Y0);                              /* palavras por pixel */
    float avg = total_h > 0 ? total_words / total_h : 0;
    dens = MAX(dens, avg * 0.35f);                               /* trecho vazio não vira um salto */
    if (dens <= 0) return S(40);
    return CLAMP(prefs.read_wpm / 60.0f / dens, S(6), view_h * 0.5f);
}

/* botão do meio: clique curto liga/desliga a rolagem automática; segurar e arrastar rola na
 * direção e na velocidade da distância ao ponto de partida (como nos navegadores), e soltar para */
static void middle_input(Rect area) {
    Input *in = &ui.in;
    /* só na faixa do texto (fora das barras e da barra de leitura) */
    bool in_text = rect_has(area, in->mx, in->my) && in->my >= ui.block_top && in->my < ui.H - ui.block_bottom
                   && !rect_has(bar_r, in->mx, in->my);
    if (in->mpressed && ui_mouse_ok() && in_text) {
        pan_on = true; pan_drag = false; pan_x = (float)in->mx; pan_y = (float)in->my; pan_t = in->now;
    }
    pan_v = 0;
    if (!pan_on) return;
    /* rede de segurança: se o "soltar" do botão do meio se perder (foco, captura do mouse), qualquer
     * tecla ou clique esquerdo encerra o arrasto. Nada fica rolando sem alguém segurando o botão. */
    if (in->nkeys > 0 || in->pressed) { pan_on = pan_drag = false; return; }
    float dx = in->mx - pan_x, dy = in->my - pan_y;
    if (!pan_drag && (f_abs(dy) > S(10) || f_abs(dx) > S(10))) pan_drag = true;
    if (!in->mdown) {
        bool click = !pan_drag && in->now - pan_t < 450;
        pan_on = pan_drag = false;
        if (click) preview_toggle_auto();
        return;
    }
    if (pan_drag) {
        float u = MAX(0.0f, f_abs(dy) - S(14)) / ui.scale;      /* zona morta em volta do ponto */
        float v = (u * 2.4f + u * u * 0.035f) * ui.scale;        /* fino perto, rápido longe */
        pan_v = (dy < 0 ? -v : v);
        pan_v = CLAMP(pan_v, -view_h * 6, view_h * 6);
        ui.redraw = true;
    }
}
static void pan_anchor_draw(void) {
    if (!pan_on || !pan_drag) return;
    float r = S(15), x = pan_x, y = pan_y;
    gfx_circle(x, y, r, col_alpha(T->panel, 0.92f));
    gfx_ring(x, y, r, S(1.3f), col_alpha(T->text, 0.35f));
    gfx_circle(x, y, S(2.2f), col_alpha(T->text, 0.6f));
    Color up = col_alpha(T->text, pan_v < 0 ? 0.9f : 0.35f), dn = col_alpha(T->text, pan_v > 0 ? 0.9f : 0.35f);
    float a = S(4.5f), t = S(1.6f);
    gfx_line(x - a, y - S(6), x, y - S(10.5f), t, up); gfx_line(x, y - S(10.5f), x + a, y - S(6), t, up);
    gfx_line(x - a, y + S(6), x, y + S(10.5f), t, dn); gfx_line(x, y + S(10.5f), x + a, y + S(6), t, dn);
}

/* barra de controles da leitura: página anterior · rolagem automática · velocidade · próxima página */
/* fica no centro da barra de status (não cobre o texto); um só lugar calcula os botões, usado
 * pelo desenho, pelo clique e pelos testes */
static void bar_layout(Rect area) {
    int bs = SI(36), sb = SI(30), lw = SI(86), pad = SI(3);
    int w = SI(10) + bs + SI(4) + bs + SI(2) + sb + lw + sb + SI(4) + bs + SI(10);
    int h = bs + pad * 2;
    bar_r = rect(area.x + (area.w - w) / 2, area.y + area.h - SI(44) + (SI(44) - h) / 2, w, h);
    int x = bar_r.x + SI(10), y = bar_r.y + pad, sy = y + (bs - sb) / 2;
    bar_btn[BAR_PREV]   = rect(x, y, bs, bs);   x += bs + SI(4);
    bar_btn[BAR_PLAY]   = rect(x, y, bs, bs);   x += bs + SI(2);
    bar_btn[BAR_SLOWER] = rect(x, sy, sb, sb);  x += sb;
    bar_btn[BAR_SPEED]  = rect(x, y, lw, bs);   x += lw;
    bar_btn[BAR_FASTER] = rect(x, sy, sb, sb);  x += sb + SI(4);
    bar_btn[BAR_NEXT]   = rect(x, y, bs, bs);
}
Rect preview_bar_rect(void) { return bar_r; }
static bool small_btn(Rect r, const char *label, const char *tip) {
    bool hv = ui_hover(r);
    if (hv) { gfx_rrect(r, S(8), col_alpha(T->chrome_text, 0.10f)); ui.cursor = 2; ui_tooltip(tip, r, tip); }
    draw_text_c(ui_font(true), S(18), r, label, hv ? T->chrome_text : col_alpha(T->chrome_text, 0.72f));
    return ui.in.pressed && hv;
}
/* chamada pelo app depois da barra de status, para ficar por cima dela */
void preview_draw_bar(float a) {
    if (!active) return;
    if (a > 0.05f) {
        Rect r = bar_r;
        gfx_set_opacity(a);
        gfx_shadow(r, r.h * 0.5f, S(10), 0x1C000000);
        gfx_rrect(r, r.h * 0.5f, T->panel | 0xFF000000);
        if (ui_icon_btn(bar_btn[BAR_PREV], IC_BACK, false, L("Previous page (\xE2\x86\x90)"))) { target -= view_h * 0.85f; ui.redraw = true; }
        if (ui_icon_btn(bar_btn[BAR_PLAY], auto_on ? IC_PAUSE : IC_PLAY, auto_on,
                        auto_on ? L("Stop auto-scroll (Enter or middle button)") : L("Auto-scroll (Enter or middle button)"))) preview_toggle_auto();
        if (small_btn(bar_btn[BAR_SLOWER], "\xE2\x88\x92", L("Slower (\xE2\x88\x92)"))) wpm_step(-1);
        Rect lr = bar_btn[BAR_SPEED];
        char s[32]; fmt(s, sizeof s, L("%d wpm"), prefs.read_wpm);
        draw_text_c(ui_font(auto_on), S(13.5f), lr, s, auto_on ? T->accent : col_alpha(T->chrome_text, 0.8f));
        if (ui_hover(lr)) ui_tooltip(&prefs.read_wpm, lr, L("Reading speed in words per minute (the mouse wheel adjusts it here)"));
        if (small_btn(bar_btn[BAR_FASTER], "+", L("Faster (+)"))) wpm_step(1);
        if (ui_icon_btn(bar_btn[BAR_NEXT], IC_FWD, false, L("Next page (\xE2\x86\x92 or space)"))) { target += view_h * 0.85f; ui.redraw = true; }
        gfx_set_opacity(1);
    }
    pan_anchor_draw();              /* por cima das barras, mesmo perto da borda */
}

static void draw_line(PB *b, PL *l, float x, float base, Color col) {
    u16 prev = 0; Face *pf = 0; float pen = x;
    for (int i = l->s; i < l->e; i++) {
        SC *c = &scs[i];
        Face *f = pface(b, c->st); float px = ppx(b, c->st);
        u16 g = face_gid_fb(&f, c->cp == '\t' ? ' ' : c->cp);
        if (prev && pf == f) pen += face_kern(f, prev, g, px);
        float adv = face_adv(f, g, px) * (c->cp == '\t' ? 4 : 1);
        Color cc = (c->st & ST_LINK) ? T->accent : col;
        if ((c->st & ST_CODE) && b->b->kind != MB_CODE)
            gfx_rect(rect((int)pen, (int)(base - px * 0.82f), (int)(adv + 1), (int)(px * 1.08f)), T->code_bg);
        if (c->cp != ' ' && c->cp != '\t') {
            float fx = f_floor(pen); int sub = (int)((pen - fx) * 4) & 3;
            const Glyph *gl = face_glyph(f, g, px, sub);
            if (gl && gl->bmp) gfx_mask((int)fx + gl->ox, (int)f_round(base) + gl->oy, gl->bmp, gl->w, gl->h, gl->w, cc);
        }
        if (c->st & ST_STRIKE) gfx_rect(rect((int)pen, (int)(base - face_xheight(f, px) * 0.5f), (int)(adv + 1), MAX(1, SI(1.2f))), cc);
        if (c->st & ST_LINK) gfx_rect(rect((int)pen, (int)(base + S(2.5f)), (int)(adv + 1), MAX(1, SI(1))), col_alpha(cc, 0.5f));
        pen += adv; prev = g; pf = f;
    }
}

static void draw_table(PB *b, float x, float y, float w) {
    MdBlock *m = b->b;
    float rh = prefs.font_size * ui.scale * 1.9f;
    int row = 0, i = 0;
    Face *fr = fam_face(font_get(prefs.font), 0), *fb = fam_face(font_get(prefs.font), 1);
    float px = prefs.font_size * ui.scale * 0.92f;
    while (i < m->len) {
        int s = i; while (i < m->len && m->text[i] != '\n') i++;
        const char *r = m->text + s; int rl = i - s; i++;
        bool sep = true; for (int k = 0; k < rl; k++) if (r[k] != '|' && r[k] != '-' && r[k] != ':' && r[k] != ' ') sep = false;
        if (sep) continue;
        const char *cells[24]; int lens[24]; int n = 0, k = 0;
        while (k < rl && r[k] == ' ') k++;
        if (k < rl && r[k] == '|') k++;
        while (k < rl && n < 24) {
            int cs = k; while (k < rl && r[k] != '|') k++;
            int ce = k; while (cs < ce && r[cs] == ' ') cs++; while (ce > cs && r[ce - 1] == ' ') ce--;
            if (k >= rl && cs == ce) break;
            cells[n] = r + cs; lens[n] = ce - cs; n++; k++;
        }
        if (!n) continue;
        float cw = w / n, ry = y + row * rh;
        if (row == 0) gfx_rect(rect((int)x, (int)ry, (int)w, (int)rh), col_alpha(T->code_bg, 1));
        gfx_rect(rect((int)x, (int)(ry + rh - 1), (int)w, 1), col_alpha(T->muted, 0.35f));
        for (int c = 0; c < n; c++) {
            int pl; char *pt = md_plain(cells[c], lens[c], &pl);
            char tmp[256]; str_copy(tmp, pt, MIN((int)sizeof tmp, pl + 1));
            draw_text_ellipsis(row == 0 ? fb : fr, px, x + c * cw + S(10), ry + rh * 0.66f, cw - S(16), tmp, T->text);
            mem_free(pt);
        }
        row++;
    }
}

void preview_draw(Rect area, float chrome_a) {
    float w = app_text_w((float)area.w);
    view_h = (float)area.h; view_top = MAX(S(64), area.h * 0.1f);
    if (w != built_w || built_ver != app.doc.version || built_font != prefs.font || built_scale != ui.scale || built_size != prefs.font_size) {
        /* mudou fonte, tamanho ou largura: continua no mesmo trecho */
        if (npbs && pend_src < 0) { pend_src = preview_top_para(); pend_top = false; }
        build(w);
    }
    Input *in = &ui.in;
    bar_layout(area);
    bool bar_on = chrome_a > 0.05f;
    if (bar_on && ui_hover(bar_r) && in->wheel) { wpm_step(in->wheel > 0 ? 1 : -1); in->wheel = 0; }   /* roda sobre a barra: velocidade */
    if (ui_mouse_ok() && in->wheel) target -= in->wheel * prefs.font_size * ui.scale * 4;
    float maxs = MAX(0.0f, total_h - area.h * 0.6f);
    max_s = maxs;
    if (pend_src >= 0) {                     /* posição pedida antes do layout: aplica sem animar */
        int p = pend_src; pend_src = -1;
        preview_goto_para(p, pend_top);
        scroll = target;
    }
    float cx = area.x + (area.w - w) / 2;

    /* rolagem automática + arrasto do botão do meio. Avança posição e alvo juntos (a suavização
     * abaixo continua cuidando só dos saltos de teclado/roda) e, com rampa de ~0,3 s, parte e
     * para sem tranco. Pausa só enquanto algo pede atenção: um diálogo/paleta (modal), uma janela
     * com o teclado (busca) ou o arrasto da barra de rolagem. Janelas comuns (música, sumário,
     * ajustes) não pausam: ler com o player aberto é um uso normal. */
    middle_input(area);
    bool hold = wm_any_modal() || (ui.key_win && ui.key_win->wants_keys) || ui.active == (void *)&target || pend_src >= 0;
    float want = auto_on && !hold ? auto_speed() : 0;
    /* partida em ~0,3 s (suave); parada em ~0,1 s (para quase onde foi pedido) */
    auto_v += (want - auto_v) * MIN(1.0f, in->dt * (want > auto_v ? 3.5f : 10.0f));
    if (want == 0 && auto_v < S(2)) auto_v = 0;
    if (!hold && pan_v != 0) ui.redraw = true;
    float v = auto_v + (hold ? 0 : pan_v);
    if (v != 0) { float dy = v * in->dt; scroll += dy; target += dy; }
    /* (ligada antes do layout existir, pela paleta: o texto pode já caber inteiro na tela) */
    if (auto_on && pend_src < 0 && maxs < 1) { auto_set(false); auto_v = 0; ui_toast(L("The whole text already fits on the screen")); }
    else if (auto_on && target >= maxs) { target = maxs; auto_set(false); auto_v = 0; ui_toast(L("End of the text")); }
    if (wpm_dirty && in->now - wpm_t > 2000) wpm_save();     /* velocidade: grava 2 s depois do último ajuste */

    /* barra de rolagem (aparece com as barras) e virar página clicando nas margens */
    Rect track = rect(area.x + area.w - SI(12), area.y + SI(64), SI(5), area.h - SI(128));
    bool in_band = in->my >= ui.block_top && in->my < ui.H - ui.block_bottom && in->mx < track.x - SI(10)
                   && !(bar_on && rect_has(bar_r, in->mx, in->my));
    int side = 0;
    if (ui_mouse_ok() && in_band && ui.active != &target) {
        if (in->mx < cx - S(28)) side = -1;
        else if (in->mx > cx + w + S(28)) side = 1;
    }
    if (side) {
        ui.cursor = 2;
        if (in->pressed) { target += side * area.h * 0.85f; ui.redraw = true; }
    }
    target = CLAMP(target, 0.0f, maxs);
    scroll = CLAMP(scroll, 0.0f, maxs);
    float d = target - scroll;
    if (f_abs(d) > 0.5f) { scroll += d * MIN(1.0f, in->dt * 14); ui.redraw = true; } else scroll = target;

    /* Quadros da rolagem automática: não adianta desenhar 140 vezes por segundo se o texto só muda
     * de pixel ~10 vezes (200 pal/min). Pede o próximo quadro para quando o deslocamento arredondado
     * mudar (no mínimo a cada 7 ms, no máximo a cada 50 ms, para a rampa e o relógio seguirem
     * precisos). Economiza CPU e bateria; no UEFI o laço dorme no WaitForEvent entre os quadros. */
    if (auto_v > 0) {
        float dist = f_round(scroll) + 0.5f - scroll;
        if (dist <= 0) dist += 1;
        int ms = (int)(dist / auto_v * 1000.0f) + 1;
        ms = CLAMP(ms, 7, 50);
        if (!ui.wake_ms || ms < ui.wake_ms) ui.wake_ms = ms;
    }
    if (wpm_dirty && !ui.wake_ms) ui.wake_ms = 500;

    if (app_sheet_on()) app_paper_sheet(rect((int)cx - SI(80), area.y - SI(30), (int)w + SI(160), area.h + SI(60)));
    if (side) {
        float mx = side < 0 ? (area.x + cx - S(80)) * 0.5f : (cx + w + S(80) + area.x + area.w) * 0.5f;
        mx = side < 0 ? MAX(mx, (float)area.x + S(30)) : MIN(mx, (float)(area.x + area.w) - S(40));
        icon_draw(side < 0 ? IC_BACK : IC_FWD, mx, area.y + area.h * 0.5f, S(30), col_alpha(T->muted, 0.55f));
    }
    gfx_push_clip(area);
    /* a página inteira anda em pixels inteiros: arredondar cada linha por conta própria fazia as
     * linhas (e fundos de código, barras de citação) pularem de pixel em momentos diferentes, e o
     * texto "tremia" na rolagem lenta */
    float y0 = area.y + view_top - f_round(scroll);
    for (int i = 0; i < npbs; i++) {
        PB *b = &pbs[i];
        float by = y0 + b->y;
        if (by > area.y + area.h) break;
        if (by + b->h < area.y) continue;
        MdBlock *m = b->b;
        Face *f0 = pface(b, 0);
        float asc = face_ascent(f0, b->size), dsc = face_descent(f0, b->size);
        Color col = m->kind == MB_H ? T->heading : m->kind == MB_QUOTE ? col_mix(T->text, T->muted, 0.35f) : T->text;
        if (m->kind == MB_HR) {
            float cy = by + b->h * 0.5f;
            for (int k = -1; k <= 1; k++) gfx_circle(cx + w / 2 + k * S(18), cy, S(2.2f), col_alpha(T->muted, 0.8f));
            continue;
        }
        if (m->kind == MB_TABLE) { draw_table(b, cx, by, w); continue; }
        if (m->kind == MB_CODE) gfx_rrect(rect((int)cx, (int)by, (int)w, (int)b->h), S(8), T->code_bg);
        if (m->kind == MB_QUOTE) gfx_rrect(rect((int)(cx + S(4)), (int)by + SI(4), SI(3), (int)(b->nl * b->lh) - SI(8)), S(1.5f), col_alpha(T->accent, 0.6f));
        float ty = by + (m->kind == MB_CODE ? prefs.font_size * ui.scale * 0.5f : 0);
        for (int li = 0; li < b->nl; li++) {
            PL *l = &pls[b->l0 + li];
            float base = ty + li * b->lh + (b->lh - (asc + dsc)) * 0.5f + asc;
            float x = cx + b->indent;
            if (li == 0 && m->kind == MB_PARA && prefs.first_indent && i > 0 && md.b[i - 1].kind == MB_PARA) x += prefs.font_size * ui.scale * 1.02f * 1.6f;
            if (m->kind == MB_H && m->level == 1 && i == 0) x = cx + (w - l->w) / 2;
            if (li == 0 && (m->kind == MB_ULIST || m->kind == MB_OLIST || m->kind == MB_TASK)) {
                float mx = x - prefs.font_size * ui.scale * 1.1f;
                if (m->kind == MB_ULIST) gfx_circle(mx + S(4), base - face_xheight(f0, b->size) * 0.5f, S(2.6f), T->accent);
                else if (m->kind == MB_OLIST) { char nb[16]; fmt(nb, sizeof nb, "%d.", m->number); draw_text(f0, b->size, mx - S(4), base, nb, -1, T->muted); }
                else {
                    float bs = b->size * 0.8f; Rect cb = rect((int)(mx - S(2)), (int)(base - bs * 0.85f), (int)bs, (int)bs);
                    if (m->checked) { gfx_rrect(cb, S(3), T->accent); icon_draw(IC_CHECK, cb.x + cb.w * 0.5f, cb.y + cb.h * 0.5f, bs * 0.95f, 0xFFFFFFFF); }
                    else gfx_rrect_line(cb, S(3), S(1.5f), T->muted);
                }
            }
            Color lc = (m->kind == MB_TASK && m->checked) ? T->muted : col;
            draw_line(b, l, x, base, lc);
        }
    }
    /* barra de progresso de leitura */
    float prog = total_h > area.h * 0.6f ? scroll / MAX(1.0f, total_h - area.h * 0.6f) : 1;
    gfx_rect(rect(area.x, area.y, (int)(area.w * CLAMP(prog, 0.0f, 1.0f)), MAX(2, SI(2))), col_alpha(T->accent, 0.8f));
    gfx_pop_clip();
    if (chrome_a > 0.05f) { gfx_set_opacity(chrome_a); ui_scrollbar(track, &target, maxs + area.h, (float)area.h); gfx_set_opacity(1); }
    if (pan_on && pan_drag) ui.cursor = 0;
}
