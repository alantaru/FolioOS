/* FOLIO v4 — gerenciador de janelas próprio + widgets de modo imediato */
#include "folio.h"

UI ui;

Face *ui_font(bool bold) { return fam_face(font_ui(), bold ? FS_BOLD : FS_REG); }
float ui_px(void) { return S(14.5f); }

/* ------------------------------------------------------------ janelas */
void wm_add(Window *w) {
    for (int i = 0; i < ui.nwins; i++) if (ui.wins[i] == w) return;
    if (ui.nwins < COUNT(ui.wins)) ui.wins[ui.nwins++] = w;
}
static int zcounter;
void wm_raise(Window *w) { w->z = ++zcounter; }
void wm_open(Window *w) {
    wm_add(w);
    if (!w->open) { w->open = true; w->nav = -1; w->nav_kind = NAV_ITEM; if (w->anim <= 0.01f) w->anim = 0.0f; }
    if (!w->centered_once || w->modal) {
        if (w->r.w > ui.W - SI(24)) w->r.w = ui.W - SI(24);
        if (w->r.h > ui.H - SI(24)) w->r.h = ui.H - SI(24);
        w->r.x = (ui.W - w->r.w) / 2;
        w->r.y = MAX(SI(56), (ui.H - w->r.h) / 2 - SI(20));
        if (w->r.y + w->r.h > ui.H - SI(8)) w->r.y = MAX(SI(8), ui.H - w->r.h - SI(8));
        w->centered_once = true;
    }
    wm_raise(w);
    if (w->wants_keys) ui.key_win = w;
    ui.redraw = true;
}
void wm_close(Window *w) {
    if (!w->open) return;
    w->open = false;
    if (ui.key_win == w) ui.key_win = 0;
    if (ui.drag_win == w) ui.drag_win = 0;
    ui.focus = 0;
    if (w->on_close) w->on_close(w);
    /* devolve o foco de teclado à janela modal/teclado mais alta ainda aberta */
    Window *best = 0;
    for (int i = 0; i < ui.nwins; i++) {
        Window *o = ui.wins[i];
        if (o->open && o->wants_keys && (!best || o->z > best->z)) best = o;
    }
    if (best && best->modal) ui.key_win = best;
    ui.redraw = true;
}
void wm_toggle(Window *w) { if (w->open) wm_close(w); else wm_open(w); }

/* foco de teclado: uma janela (painéis como Ajustes também, para quem não usa mouse) ou o texto (0) */
void wm_focus(Window *w) {
    if (w && !w->open) return;
    if (ui.key_win != w) { ui.key_win = w; ui.focus = 0; }
    if (w) wm_raise(w);
    ui.nav_show = true; ui.redraw = true;
}
/* Ctrl+F6: texto → janelas abertas (na ordem em que foram criadas, estável) → texto */
void wm_cycle_focus(void) {
    if (wm_any_modal()) return;
    Window *seq[33]; int m = 0, cur = 0;
    seq[m++] = 0;
    for (int i = 0; i < ui.nwins; i++) if (ui.wins[i]->open) seq[m++] = ui.wins[i];
    for (int k = 0; k < m; k++) if (seq[k] == ui.key_win) cur = k;
    Window *nx = seq[(cur + 1) % m];
    wm_focus(nx);
    char t[160];
    if (nx) fmt(t, sizeof t, L("Keyboard: %s (Tab moves, Esc closes)"), L(nx->title));
    else str_copy(t, L("Keyboard: text"), sizeof t);
    ui_toast(t);
}

static void sorted(Window **out, int *n) {
    *n = 0;
    for (int i = 0; i < ui.nwins; i++) if (ui.wins[i]->open || ui.wins[i]->anim > 0.01f) out[(*n)++] = ui.wins[i];
    for (int i = 0; i < *n; i++) for (int j = i + 1; j < *n; j++) if (out[j]->z < out[i]->z) { Window *t = out[i]; out[i] = out[j]; out[j] = t; }
}
Window *wm_top(void) {
    Window *list[32]; int n; sorted(list, &n);
    for (int i = n - 1; i >= 0; i--) if (list[i]->open) return list[i];
    return 0;
}
bool wm_any_modal(void) {
    for (int i = 0; i < ui.nwins; i++) if (ui.wins[i]->open && ui.wins[i]->modal) return true;
    return false;
}

void wm_hit_test(void) {
    Window *list[32]; int n; sorted(list, &n);
    ui.hot_win = 0; ui.modal_block = false;
    for (int i = n - 1; i >= 0; i--) {
        Window *w = list[i];
        if (!w->open) continue;
        if (rect_has(w->r, ui.in.mx, ui.in.my)) { ui.hot_win = w; break; }
        if (w->modal) { ui.modal_block = true; break; }
    }
    if (!ui.hot_win) for (int i = 0; i < n; i++) if (list[i]->open && list[i]->modal) ui.modal_block = true;
    if (ui.in.pressed) ui.nav_show = false;          /* clicou: o contorno de foco do teclado some */
    if (ui.in.pressed && !ui.drag_win) {
        if (ui.hot_win) {
            wm_raise(ui.hot_win);
            if (ui.hot_win->wants_keys) ui.key_win = ui.hot_win;
        } else if (!ui.modal_block) {
            ui.key_win = 0; ui.focus = 0;
        }
    }
    /* clicar fora fecha: o diálogo modal do topo, ou todas as janelas comuns abertas.
       As barras ficam de fora (seus botões já abrem e fecham as janelas). O clique só fecha. */
    bool in_bars = ui.in.my < ui.block_top || ui.in.my >= ui.H - ui.block_bottom;
    if (ui.in.pressed && !ui.drag_win && !ui.hot_win) {
        Window *top = 0;
        for (int i = n - 1; i >= 0; i--) if (list[i]->open && list[i]->modal) { top = list[i]; break; }
        if (top) { wm_close(top); ui.modal_block = true; }
        else if (!in_bars) {
            bool any = false;
            for (int i = 0; i < n; i++) if (list[i]->open && list[i] != ui.sticky) { wm_close(list[i]); any = true; }
            if (any) ui.modal_block = true;
        }
    }
}

bool ui_mouse_ok(void) {
    if (ui.drag_win) return false;
    if (ui.cur_win) return ui.cur_win == ui.hot_win;
    return !ui.hot_win && !ui.modal_block;
}
bool ui_hover(Rect r) { return ui_mouse_ok() && rect_has(r, ui.in.mx, ui.in.my); }
bool ui_clicked(Rect r) { return ui.in.pressed && ui_hover(r); }

/* ------------------------------------------------------------ teclado */
static bool keys_allowed(void) { return ui.cur_win ? ui.cur_win == ui.key_win : ui.key_win == 0; }
bool ui_take_key(Event *e) {
    if (!keys_allowed()) return false;
    for (int i = 0; i < ui.in.nkeys; i++) {
        if (ui.in.keys[i].type != EV_KEY) continue;
        *e = ui.in.keys[i];
        ui.in.keys[i].type = EV_NONE;
        return true;
    }
    return false;
}
static bool take_matching(int key, int mods, Event *out) {
    if (!keys_allowed()) return false;
    for (int i = 0; i < ui.in.nkeys; i++) {
        Event *e = &ui.in.keys[i];
        if (e->type != EV_KEY) continue;
        if (e->key == key && (mods < 0 || (e->mods & (MOD_CTRL | MOD_SHIFT | MOD_ALT)) == mods)) {
            if (out) *out = *e;
            e->type = EV_NONE; return true;
        }
    }
    return false;
}
bool ui_key(int key) { return take_matching(key, -1, 0); }

/* ------------------------------------------------------------ navegação sem mouse
 * Cada widget de uma janela se registra como item (ui_nav), na ordem em que é desenhado.
 * Na janela com foco de teclado: Tab/Shift+Tab e as setas que a janela não usou andam entre os
 * itens; Enter/Espaço acionam o item; ←/→ ajustam deslizantes e opções. O contorno de foco só
 * aparece depois de usar o teclado, e nada muda no desenho para quem usa mouse. */
static bool plain_key(const Event *e) { return e->type == EV_KEY && !(e->mods & (MOD_CTRL | MOD_ALT)); }
static bool is_tab(const Event *e) { return e->key == KEY_TAB || (e->ch == 9 && !e->key); }
static void nav_move(Window *w, int d) {
    int n = w->nav_n;
    if (n <= 0) return;
    w->nav = w->nav < 0 ? (d > 0 ? 0 : n - 1) : (w->nav + d + n) % n;
    w->nav_kind = NAV_ITEM;          /* até o item se registrar de novo no próximo quadro */
    ui.nav_show = true; ui.nav_reveal = true; ui.focus = 0; ui.redraw = true;
}
/* antes do corpo da janela: Tab, e Enter/Espaço/←/→ quando são do item focado */
static void nav_pre(Window *w) {
    for (int i = 0; i < ui.in.nkeys; i++) {
        Event *e = &ui.in.keys[i];
        if (!plain_key(e)) continue;
        if (is_tab(e)) { e->type = EV_NONE; nav_move(w, (e->mods & MOD_SHIFT) ? -1 : 1); continue; }
        if (w->nav < 0 || !ui.nav_show) continue;
        int k = w->nav_kind;
        if ((k == NAV_ITEM || k == NAV_ADJ) && !ui.nav_fire && (e->key == KEY_ENTER || (e->ch == ' ' && !e->key))) { e->type = EV_NONE; ui.nav_fire = true; continue; }
        if (k == NAV_ADJ && !ui.nav_adj && (e->key == KEY_LEFT || e->key == KEY_RIGHT)) { e->type = EV_NONE; ui.nav_adj = e->key == KEY_LEFT ? -1 : 1; continue; }
    }
}
/* depois do corpo: as setas que a janela não usou (listas usam ↑/↓) passam de um item ao outro */
static void nav_post(Window *w) {
    w->nav_n = w->nav_cnt;
    if (w->nav >= w->nav_n) w->nav = w->nav_n - 1;
    ui.nav_reveal = false;
    for (int i = 0; i < ui.in.nkeys; i++) {
        Event *e = &ui.in.keys[i];
        if (!plain_key(e)) continue;
        int d = e->key == KEY_UP || e->key == KEY_LEFT ? -1 : e->key == KEY_DOWN || e->key == KEY_RIGHT ? 1 : 0;
        if (!d) continue;
        e->type = EV_NONE;
        nav_move(w, d);
    }
}
bool ui_nav(Rect r, int kind) {
    Window *w = ui.cur_win;
    if (!w || ui.nav_skip) return false;
    int idx = w->nav_cnt++;
    if (ui.in.pressed && ui_hover(r)) { w->nav = idx; w->nav_kind = kind; }   /* o clique leva o foco junto */
    if (w != ui.key_win || idx != w->nav) return false;
    w->nav_kind = kind;
    ui.nav_found = true; ui.nav_r = r;
    if (ui.nav_show) gfx_rrect_line(rect(r.x - SI(7), r.y - SI(3), r.w + SI(14), r.h + SI(6)), S(10), S(2), T->accent);
    return true;
}
bool ui_item(Rect r) {
    bool f = ui_nav(r, NAV_ITEM), hv = ui_hover(r);
    if (hv) ui.cursor = 2;
    return (ui.in.pressed && hv) || (f && ui.nav_fire);
}
/* o mesmo, numa lista que rola: o clique só conta na parte visível (vis); o teclado alcança o item inteiro */
bool ui_item_in(Rect r, Rect vis) {
    bool f = ui_nav(r, NAV_ITEM), hv = ui_hover(rect_clip(r, vis));
    if (hv) ui.cursor = 2;
    return (ui.in.pressed && hv) || (f && ui.nav_fire);
}
void ui_reveal(float *scroll, Rect view) {
    if (!ui.nav_reveal || !ui.nav_found || !ui.cur_win || ui.cur_win != ui.key_win) return;
    Rect r = ui.nav_r; int pad = SI(14);
    float d = 0;
    if (r.y < view.y + pad || r.h > view.h - 2 * pad) d = (float)(r.y - view.y - pad);
    else if (r.y + r.h > view.y + view.h - pad) d = (float)(r.y + r.h - (view.y + view.h - pad));
    if (d != 0) { *scroll = MAX(0.0f, *scroll + d); ui.redraw = true; }
    ui.nav_reveal = false;
}

/* ------------------------------------------------------------ mover/redimensionar pelo teclado
 * Alt+F7 move, Alt+F8 redimensiona (como no Linux): setas andam 32 px (com Shift, 4 px),
 * Enter confirma, Esc volta ao lugar de antes; outra tecla confirma e segue adiante. */
static int kb_mode; static Window *kb_w; static Rect kb_orig;
bool wm_kb_key(Event *e) {
    bool alt = (e->mods & MOD_ALT) != 0, ctrl = (e->mods & MOD_CTRL) != 0, sh = (e->mods & MOD_SHIFT) != 0;
    if (!kb_mode) {
        if (!alt || ctrl || (e->key != KEY_F7 && e->key != KEY_F8)) return false;
        Window *w = ui.key_win && ui.key_win->open ? ui.key_win : wm_top();
        if (!w) { ui_toast(L("No window open")); return true; }
        if (e->key == KEY_F8 && !w->resizable) { ui_toast(L("This window cannot be resized")); return true; }
        kb_mode = e->key == KEY_F7 ? 1 : 2; kb_w = w; kb_orig = w->r;
        wm_raise(w);
        ui_toast(kb_mode == 1 ? L("Move the window with the arrows · Enter confirms · Esc cancels")
                              : L("Resize the window with the arrows · Enter confirms · Esc cancels"));
        ui.redraw = true;
        return true;
    }
    if (!kb_w->open) { kb_mode = 0; return false; }
    if (e->key == KEY_ENTER) { kb_mode = 0; ui.redraw = true; return true; }
    if (e->key == KEY_ESC) { kb_w->r = kb_orig; kb_mode = 0; ui.redraw = true; return true; }
    int dx = e->key == KEY_LEFT ? -1 : e->key == KEY_RIGHT ? 1 : 0, dy = e->key == KEY_UP ? -1 : e->key == KEY_DOWN ? 1 : 0;
    if (!dx && !dy) { kb_mode = 0; ui.redraw = true; return false; }
    int st = sh ? SI(4) : SI(32);
    Window *w = kb_w;
    if (kb_mode == 1) {
        w->r.x = CLAMP(w->r.x + dx * st, -w->r.w + SI(80), ui.W - SI(80));
        w->r.y = CLAMP(w->r.y + dy * st, 0, ui.H - SI(40));
    } else {
        w->r.w = MIN(MAX(w->min_w, w->r.w + dx * st), ui.W - w->r.x);
        w->r.h = MIN(MAX(w->min_h, w->r.h + dy * st), ui.H - w->r.y);
    }
    ui.redraw = true;
    return true;
}

/* ------------------------------------------------------------ desenho de janela */
static void draw_window(Window *w) {
    float a = ease_out(w->anim);
    int dy = (int)((1 - a) * S(14));
    Rect r = w->r; r.y += dy;
    ui.cur_win = w;
    if (w->modal) { gfx_set_opacity(a); gfx_rect(rect(0, 0, ui.W, ui.H), T->dark ? 0x70000000 : 0x40202020); }
    gfx_set_opacity(a);
    float rad = S(12);
    gfx_shadow(rect(r.x, r.y + SI(10), r.w, r.h), rad, S(34), T->dark ? 0x80000000 : 0x2A3A2A1A);
    gfx_shadow(rect(r.x, r.y + SI(1), r.w, r.h), rad, S(4), T->dark ? 0x50000000 : 0x14000000);
    gfx_rrect(r, rad, T->panel);
    /* janela com o teclado (ou sendo movida pelo teclado): borda na cor de destaque */
    bool kf = (w == ui.key_win && ui.nav_show && !w->modal) || (kb_mode && kb_w == w);
    gfx_rrect_line(r, rad, kf ? S(1.5f) : S(1), kf ? T->accent : col_alpha(T->border, T->dark ? 0.9f : 0.8f));
    int th = w->no_titlebar ? 0 : SI(46);
    if (!w->no_titlebar) {
        Face *fb = ui_font(true);
        float px = S(14.5f);
        float tx = r.x + S(18);
        if (w->icon >= 0) { icon_draw(w->icon, tx + S(9), r.y + th * 0.5f, S(18), T->accent); tx += S(28); }
        float base = r.y + th * 0.5f + face_ascent(fb, px) * 0.36f;
        draw_text(fb, px, tx, base, L(w->title), -1, T->panel_text);
        Rect cb = rect(r.x + r.w - SI(40), r.y + (th - SI(28)) / 2, SI(28), SI(28));
        bool hv = ui_hover(cb);
        if (hv) gfx_rrect(cb, S(8), col_alpha(T->text, 0.08f));
        icon_draw(IC_CLOSE, cb.x + cb.w * 0.5f, cb.y + cb.h * 0.5f, S(16), hv ? T->panel_text : col_alpha(T->panel_text, 0.55f));
        if (hv) { ui.cursor = 2; ui_tooltip(w, cb, L("Close (Esc)")); }
        if (ui.in.pressed && hv) { wm_close(w); }
        Rect tb = rect(r.x, r.y, r.w - SI(48), th);
        if (ui.in.pressed && ui_hover(tb) && ui.cur_win == ui.hot_win) {
            ui.drag_win = w; ui.drag_dx = ui.in.mx - w->r.x; ui.drag_dy = ui.in.my - w->r.y; ui.resizing = false;
        }
        gfx_rect(rect(r.x + SI(1), r.y + th - 1, r.w - SI(2), SI(1) > 0 ? 1 : 1), col_alpha(T->border, 0.6f));
    }
    if (w->resizable) {
        Rect grip = rect(r.x + r.w - SI(18), r.y + r.h - SI(18), SI(18), SI(18));
        if (ui_hover(grip)) ui.cursor = 2;
        if (ui.in.pressed && ui_hover(grip)) { ui.drag_win = w; ui.resizing = true; ui.drag_dx = ui.in.mx - (w->r.x + w->r.w); ui.drag_dy = ui.in.my - (w->r.y + w->r.h); }
        for (int k = 0; k < 3; k++) gfx_line(grip.x + S(6 + 4 * k), grip.y + grip.h - S(4), grip.x + grip.w - S(4), grip.y + S(6 + 4 * k), S(1.2f), col_alpha(T->panel_text, 0.25f));
    }
    Rect body = rect(r.x, r.y + th, r.w, r.h - th);
    gfx_push_clip(rect_inset(body, 1));
    w->nav_cnt = 0; ui.nav_fire = false; ui.nav_adj = 0; ui.nav_found = false;
    bool keys = w == ui.key_win && w->open;
    if (keys) nav_pre(w);
    if (w->body) w->body(w, body);
    if (keys && ui.key_win == w) nav_post(w); else w->nav_n = w->nav_cnt;
    ui.nav_fire = false; ui.nav_adj = 0;
    gfx_pop_clip();
    gfx_set_opacity(1);
    ui.cur_win = 0;
}

void wm_draw_all(void) {
    /* arrasto/redimensionamento */
    if (ui.drag_win) {
        Window *w = ui.drag_win;
        if (!ui.in.down) ui.drag_win = 0;
        else if (ui.resizing) {
            w->r.w = MAX(w->min_w, ui.in.mx - ui.drag_dx - w->r.x);
            w->r.h = MAX(w->min_h, ui.in.my - ui.drag_dy - w->r.y);
            w->r.w = MIN(w->r.w, ui.W - w->r.x); w->r.h = MIN(w->r.h, ui.H - w->r.y);
        } else {
            w->r.x = CLAMP(ui.in.mx - ui.drag_dx, -w->r.w + SI(80), ui.W - SI(80));
            w->r.y = CLAMP(ui.in.my - ui.drag_dy, 0, ui.H - SI(40));
        }
        ui.redraw = true;
    }
    Window *list[32]; int n; sorted(list, &n);
    for (int i = 0; i < n; i++) {
        Window *w = list[i];
        float target = w->open ? 1.0f : 0.0f;
        if (w->anim != target) {
            float sp = ui.in.dt * (w->open ? 7.5f : 10.0f);
            w->anim = w->open ? MIN(1.0f, w->anim + sp) : MAX(0.0f, w->anim - sp);
            ui.redraw = true;
        }
        if (w->anim > 0.01f) draw_window(w);
    }
}

/* ------------------------------------------------------------ tooltips / toasts */
void ui_tooltip(const void *id, Rect r, const char *text) {
    /* também no item com o foco do teclado, para quem não passa o mouse por cima */
    bool nf = ui.nav_show && ui.nav_found && ui.cur_win && ui.cur_win == ui.key_win && r.x == ui.nav_r.x && r.y == ui.nav_r.y && r.w == ui.nav_r.w && r.h == ui.nav_r.h;
    if (!ui_hover(r) && !nf) return;
    if (ui.tip_id != id) { ui.tip_id = id; ui.tip_since = ui.in.now; }
    ui.tip = text; ui.tip_r = r;
}

typedef struct { char msg[1024]; u64 t; } Toast;   /* cabe uma mensagem longa traduzida; desenhada em várias linhas */
static float toast_secs(const char *m) { float s = 3.2f + (float)str_len(m) / 40.0f; return s > 12.0f ? 12.0f : s; }
static Toast toasts[4]; static int ntoasts;
void ui_toast(const char *msg) {
    /* um aviso por vez: o novo substitui o anterior (menos ruído visual) */
    if (ntoasts > 0 && str_eq(toasts[ntoasts - 1].msg, msg)) { toasts[ntoasts - 1].t = ui.in.now - 250; ui.redraw = true; return; }
    ntoasts = 0;
    str_copy(toasts[ntoasts].msg, msg, sizeof toasts[0].msg);
    toasts[ntoasts].t = ui.in.now;
    ntoasts++;
    ui.redraw = true;
}
void ui_draw_toasts(void) {
    Face *f = ui_font(false); float px = S(14);
    float y = ui.H - S(84);
    for (int i = ntoasts - 1; i >= 0; i--) {
        float age = (float)(ui.in.now - toasts[i].t) / 1000.0f, life = toast_secs(toasts[i].msg);
        if (age > life) { memmove(toasts + i, toasts + i + 1, sizeof(Toast) * (usize)(ntoasts - i - 1)); ntoasts--; continue; }
        float a = age < 0.25f ? age / 0.25f : age > life - 0.5f ? (life - age) / 0.5f : 1.0f;
        /* em linhas que cabem na tela (no máximo 12) */
        const char *m = toasts[i].msg; int len = str_len(m), st[12], ln[12], nl = 0; float maxw = (float)ui.W - S(120), w = 0;
        for (int p = 0; p < len && nl < 12; ) {
            int n = text_fit(f, px, m + p, len - p, maxw);
            if (n <= 0) n = utf8_next(m + p, len - p, 0);
            if (p + n < len) { int k = n; while (k > 0 && m[p + k] != ' ') k--; if (k > 0) n = k; }
            st[nl] = p; ln[nl] = n; nl++;
            float lw = text_width(f, px, m + p, n); if (lw > w) w = lw;
            p += n; while (p < len && m[p] == ' ') p++;
        }
        if (!nl) { st[0] = 0; ln[0] = 0; nl = 1; }
        w += S(44);
        float lh = px * 1.45f; int h = (int)(SI(38) + (nl - 1) * lh);
        Rect r = rect((int)((ui.W - w) / 2), (int)(y - (nl - 1) * lh + (1 - ease_out(MIN(1.0f, age * 4))) * S(10)), (int)w, h);
        gfx_set_opacity(a);
        gfx_shadow(rect(r.x, r.y + SI(4), r.w, r.h), S(19), S(16), 0x40000000);
        Color bg = T->dark ? col_lum_shift(T->panel, 0.08f) : 0xFF2A2724;
        Color fg = T->dark ? T->panel_text : 0xFFF5F2EC;
        gfx_rrect(r, S(19), bg);
        gfx_circle(r.x + S(18), r.y + SI(38) * 0.5f, S(3.5f), T->accent);
        for (int k = 0; k < nl; k++)
            draw_text(f, px, r.x + S(30), r.y + SI(38) * 0.5f + face_ascent(f, px) * 0.36f + k * lh, m + st[k], ln[k], fg);
        gfx_set_opacity(1);
        y -= (float)h + S(8);
        ui.redraw = true;
    }
    /* tooltip */
    if (ui.tip && ui.in.now - ui.tip_since > 550) {
        float tw = text_width(f, S(13), ui.tip, -1) + S(20);
        Rect r = rect(ui.tip_r.x + ui.tip_r.w / 2 - (int)(tw / 2), ui.tip_r.y + ui.tip_r.h + SI(8), (int)tw, SI(28));
        if (r.y + r.h > ui.H) r.y = ui.tip_r.y - r.h - SI(8);
        r.x = CLAMP(r.x, SI(6), ui.W - r.w - SI(6));
        gfx_shadow(rect(r.x, r.y + SI(2), r.w, r.h), S(7), S(8), 0x30000000);
        gfx_rrect(r, S(7), T->dark ? col_lum_shift(T->panel, 0.12f) : 0xF0222222);
        draw_text_c(f, S(13), r, ui.tip, T->dark ? T->panel_text : 0xFFF2F2F2);
    } else if (ui.tip) ui.redraw = true;
}

/* ------------------------------------------------------------ widgets */
void ui_label(float x, float y, const char *s, Color c, float px, bool bold) {
    draw_text(ui_font(bold), px, x, y, s, -1, c);
}

void ui_panel(Rect r) { gfx_rrect(r, S(10), col_alpha(T->text, T->dark ? 0.05f : 0.035f)); }

bool ui_button(Rect r, const char *label, int style) {
    bool nf = ui_nav(r, NAV_ITEM);
    bool hv = ui_hover(r), dn = hv && ui.in.down;
    Color bg, fg;
    switch (style) {
    case BTN_PRIMARY: bg = dn ? col_lum_shift(T->accent, -0.12f) : hv ? col_lum_shift(T->accent, 0.08f) : T->accent;
        fg = col_luma(T->accent) > 0.6f ? 0xFF1A1A1A : 0xFFFFFFFF; break;
    case BTN_DANGER: bg = hv ? 0xFFD14A3C : 0xFFC0392B; fg = 0xFFFFFFFF; break;
    case BTN_SUBTLE: bg = hv ? col_alpha(T->text, 0.10f) : 0; fg = T->panel_text; break;
    default: bg = hv ? col_alpha(T->text, 0.12f) : col_alpha(T->text, 0.06f); fg = T->panel_text; break;
    }
    if (bg) gfx_rrect(r, S(8), bg);
    draw_text_c(ui_font(style == BTN_PRIMARY), S(14), r, label, fg);
    if (hv) ui.cursor = 2;
    return (ui.in.pressed && hv) || (nf && ui.nav_fire);
}

bool ui_icon_btn(Rect r, int icon, bool active, const char *tip) {
    bool nf = ui_nav(r, NAV_ITEM);
    bool hv = ui_hover(r);
    if (active) gfx_rrect(r, S(8), col_alpha(T->accent, 0.16f));
    else if (hv) gfx_rrect(r, S(8), col_alpha(T->chrome_text, 0.10f));
    Color c = active ? T->accent : hv ? T->chrome_text : col_alpha(T->chrome_text, 0.72f);
    icon_draw(icon, r.x + r.w * 0.5f, r.y + r.h * 0.5f, S(19), c);
    if (hv) ui.cursor = 2;
    if (tip) ui_tooltip(tip, r, tip);
    return (ui.in.pressed && hv) || (nf && ui.nav_fire);
}

bool ui_toggle(Rect r, const char *label, bool *v) {
    bool nf = ui_nav(r, NAV_ITEM);
    bool hv = ui_hover(r);
    Face *f = ui_font(false); float px = S(14);
    draw_text(f, px, r.x, r.y + r.h * 0.5f + face_ascent(f, px) * 0.36f, label, -1, T->panel_text);
    float sw = S(40), sh = S(22);
    Rect s = rect(r.x + r.w - (int)sw, r.y + (int)((r.h - sh) / 2), (int)sw, (int)sh);
    gfx_rrect(s, sh / 2, *v ? T->accent : col_alpha(T->text, hv ? 0.24f : 0.18f));
    float kx = *v ? s.x + sw - sh / 2 : s.x + sh / 2;
    gfx_circle(kx, s.y + sh / 2, sh / 2 - S(3), 0xFFFFFFFF);
    if (hv) ui.cursor = 2;
    if ((ui.in.pressed && hv) || (nf && ui.nav_fire)) { *v = !*v; ui.redraw = true; return true; }
    return false;
}

bool ui_slider(Rect r, float *v, float lo, float hi, const char *label, const char *valtxt) {
    Face *f = ui_font(false); float px = S(14);
    float base = r.y + S(16);
    if (label) draw_text(f, px, r.x, base, label, -1, T->panel_text);
    if (valtxt) { float w = text_width(f, px, valtxt, -1); draw_text(f, px, r.x + r.w - w, base, valtxt, -1, col_alpha(T->panel_text, 0.6f)); }
    Rect tr = rect(r.x, r.y + SI(30), r.w, SI(18));
    bool changed = false;
    if (ui_nav(r, NAV_ADJ) && ui.nav_adj) {          /* ←/→: um vigésimo da faixa por toque */
        float nv = CLAMP(*v + ui.nav_adj * (hi - lo) / 20.0f, lo, hi);
        if (nv != *v) { *v = nv; changed = true; ui.redraw = true; }
    }
    float t = (*v - lo) / (hi - lo); t = CLAMP(t, 0.0f, 1.0f);
    float ty = tr.y + tr.h * 0.5f;
    gfx_rrect(rect(tr.x, (int)(ty - S(2)), tr.w, SI(4)), S(2), col_alpha(T->text, 0.14f));
    gfx_rrect(rect(tr.x, (int)(ty - S(2)), (int)(tr.w * t), SI(4)), S(2), T->accent);
    bool hv = ui_hover(tr);
    if (ui.in.pressed && hv) ui.active = v;
    if (ui.active == v) {
        if (!ui.in.down) ui.active = 0;
        else {
            float nt = CLAMP((float)(ui.in.mx - tr.x) / (float)tr.w, 0.0f, 1.0f);
            float nv = lo + nt * (hi - lo);
            if (nv != *v) { *v = nv; changed = true; ui.redraw = true; }
        }
    }
    float kx = tr.x + tr.w * t;
    gfx_circle(kx, ty, S(8.5f), col_alpha(0xFF000000, 0.15f));
    gfx_circle(kx, ty - S(0.5f), S(8), 0xFFFFFFFF);
    gfx_ring(kx, ty - S(0.5f), S(8), S(1), col_alpha(T->border, 1));
    if (hv) ui.cursor = 2;
    return changed;
}

bool ui_segmented(Rect r, const char **items, int n, int *sel) {
    bool changed = false;
    if (ui_nav(r, NAV_ADJ) && (ui.nav_adj || ui.nav_fire)) {   /* ←/→ escolhem; Enter/Espaço avançam */
        int ns = ui.nav_adj ? CLAMP(*sel + ui.nav_adj, 0, n - 1) : (*sel + 1) % n;
        if (ns != *sel) { *sel = ns; changed = true; ui.redraw = true; }
    }
    gfx_rrect(r, S(9), col_alpha(T->text, T->dark ? 0.08f : 0.06f));
    float w = (float)r.w / n;
    Face *f = ui_font(false);
    for (int i = 0; i < n; i++) {
        Rect c = rect(r.x + (int)(w * i) + SI(3), r.y + SI(3), (int)w - SI(6), r.h - SI(6));
        bool hv = ui_hover(c);
        if (i == *sel) {
            gfx_shadow(c, S(7), S(4), 0x18000000);
            gfx_rrect(c, S(7), T->dark ? col_lum_shift(T->panel, 0.12f) : 0xFFFFFFFF);
        } else if (hv) gfx_rrect(c, S(7), col_alpha(T->text, 0.05f));
        draw_text_c(i == *sel ? ui_font(true) : f, S(13.5f), c, L(items[i]), i == *sel ? T->panel_text : col_alpha(T->panel_text, 0.7f));
        if (hv) ui.cursor = 2;
        if (ui.in.pressed && hv && *sel != i) { *sel = i; changed = true; ui.redraw = true; }
    }
    return changed;
}

void ui_section(Rect *cur, const char *title) {
    Face *f = ui_font(true);
    draw_text(f, S(12), cur->x, cur->y + S(14), title, -1, col_alpha(T->panel_text, 0.5f));
    cur->y += SI(26);
}

void ui_scrollbar(Rect track, float *scroll, float content, float view) {
    if (content <= view + 1) { *scroll = 0; return; }
    float maxs = content - view;
    *scroll = CLAMP(*scroll, 0.0f, maxs);
    float th = MAX(S(28), track.h * view / content);
    float ty = track.y + (track.h - th) * (*scroll / maxs);
    Rect thumb = rect(track.x, (int)ty, track.w, (int)th);
    bool hv = ui_hover(rect(track.x - SI(4), track.y, track.w + SI(8), track.h));
    if (ui.in.pressed && hv) { ui.active = scroll; }
    if (ui.active == scroll) {
        if (!ui.in.down) ui.active = 0;
        else { float t = (ui.in.my - track.y - th / 2) / (track.h - th); *scroll = CLAMP(t, 0.0f, 1.0f) * maxs; ui.redraw = true; }
    }
    gfx_rrect(thumb, track.w * 0.5f, col_alpha(T->text, hv || ui.active == scroll ? 0.35f : 0.18f));
}

/* alvo do método de entrada nos campos: o campo focado (buf/cap guardados em tf_buf/tf_cap) */
static char *tf_buf; static int tf_cap;
static void tf_ime_del(int ncp, void *ctx) {
    UNUSED(ctx);
    int len = str_len(tf_buf), p = ui.focus_caret;
    for (int i = 0; i < ncp && p > 0; i++) p = utf8_prev(tf_buf, p);
    memmove(tf_buf + p, tf_buf + ui.focus_caret, (usize)(len - ui.focus_caret + 1));
    ui.focus_caret = p;
}
static void tf_ime_ins(const char *s, int n, void *ctx) {
    UNUSED(ctx);
    int len = str_len(tf_buf);
    if (len + n >= tf_cap - 1) return;
    memmove(tf_buf + ui.focus_caret + n, tf_buf + ui.focus_caret, (usize)(len - ui.focus_caret + 1));
    memcpy(tf_buf + ui.focus_caret, s, (usize)n); ui.focus_caret += n;
}
/* campo de texto de uma linha */
static int prev_cp(const char *s, int i) { return utf8_prev(s, i); }
bool ui_textfield(Rect r, char *buf, int cap, const char *ph, bool autofocus) {
    bool hv = ui_hover(r);
    if (ui_nav(r, NAV_TEXT) && ui.nav_show && ui.focus != buf) { ui.focus = buf; ui.focus_caret = str_len(buf); ui.focus_sel = buf[0] ? 1 : 0; }
    bool focused = ui.focus == buf;
    if (autofocus && !ui.focus && keys_allowed() && (!ui.cur_win || ui.cur_win->nav < 0)) { ui.focus = buf; ui.focus_caret = str_len(buf); ui.focus_sel = buf[0] ? 1 : 0; focused = true; }
    Face *f = ui_font(false); float px = S(15);
    int len = str_len(buf);
    if (ui.in.pressed && hv) {
        ui.focus = buf; focused = true; ui.focus_sel = -1;
        float pad = S(12);
        int n = text_fit(f, px, buf, len, ui.in.mx - r.x - pad + S(3));
        ui.focus_caret = n;
    }
    bool changed = false;
    if (focused && keys_allowed()) {
        if (ui.focus_caret > len) ui.focus_caret = len;
        for (int i = 0; i < ui.in.nkeys; i++) {
            Event *e = &ui.in.keys[i];
            if (e->type != EV_KEY) continue;
            bool sel_all = ui.focus_sel == 1;
            bool used = true;
            ImeTarget tg = { tf_ime_del, tf_ime_ins, buf }; tf_buf = buf; tf_cap = cap;
            if (e->ch && !(e->mods & MOD_CTRL) && e->ch >= 32) {
                if (sel_all && ime_on()) { buf[0] = 0; len = 0; ui.focus_caret = 0; ui.focus_sel = 0; sel_all = false; }
                if (ime_feed(e->ch, &tg)) { len = str_len(buf); changed = true; e->type = EV_NONE; continue; }
            } else if (e->key == KEY_BACKSPACE && !sel_all && ime_backspace(&tg)) { len = str_len(buf); changed = true; e->type = EV_NONE; continue; }
            else ime_commit(&tg);
            if (e->ch && (e->mods & MOD_CTRL) && (e->ch == 'a')) { ui.focus_sel = 1; ui.focus_caret = len; }
            else if (e->ch && (e->mods & MOD_CTRL) && e->ch == 'v') {
                extern const char *editor_clipboard(void);
                const char *cl = editor_clipboard();
                if (cl) {
                    if (sel_all) { buf[0] = 0; len = 0; ui.focus_caret = 0; }
                    int cl_len = 0; while (cl[cl_len] && cl[cl_len] != '\n') cl_len++;
                    if (len + cl_len < cap - 1) {
                        memmove(buf + ui.focus_caret + cl_len, buf + ui.focus_caret, (usize)(len - ui.focus_caret + 1));
                        memcpy(buf + ui.focus_caret, cl, (usize)cl_len); ui.focus_caret += cl_len; len += cl_len; changed = true;
                    }
                }
                ui.focus_sel = 0;
            }
            else if (e->ch && !(e->mods & MOD_CTRL) && e->ch >= 32) {
                if (sel_all) { buf[0] = 0; len = 0; ui.focus_caret = 0; ui.focus_sel = 0; }
                char t[4]; int k = utf8_encode(e->ch, t);
                if (len + k < cap - 1) {
                    memmove(buf + ui.focus_caret + k, buf + ui.focus_caret, (usize)(len - ui.focus_caret + 1));
                    memcpy(buf + ui.focus_caret, t, (usize)k); ui.focus_caret += k; len += k; changed = true;
                }
            }
            else if (e->key == KEY_BACKSPACE) {
                if (sel_all) { buf[0] = 0; len = 0; ui.focus_caret = 0; ui.focus_sel = 0; changed = true; }
                else if (ui.focus_caret > 0) {
                    int p = prev_cp(buf, ui.focus_caret);
                    if (e->mods & MOD_CTRL) { while (p > 0 && buf[p - 1] != ' ') p = prev_cp(buf, p); }
                    memmove(buf + p, buf + ui.focus_caret, (usize)(len - ui.focus_caret + 1));
                    len -= ui.focus_caret - p; ui.focus_caret = p; changed = true;
                }
            }
            else if (e->key == KEY_DELETE) {
                if (sel_all) { buf[0] = 0; len = 0; ui.focus_caret = 0; ui.focus_sel = 0; changed = true; }
                else if (ui.focus_caret < len) {
                    int nx = utf8_next(buf, len, ui.focus_caret);
                    memmove(buf + ui.focus_caret, buf + nx, (usize)(len - nx + 1)); len -= nx - ui.focus_caret; changed = true;
                }
            }
            else if (e->key == KEY_LEFT) { ui.focus_sel = 0; ui.focus_caret = prev_cp(buf, ui.focus_caret); }
            else if (e->key == KEY_RIGHT) { ui.focus_sel = 0; ui.focus_caret = utf8_next(buf, len, ui.focus_caret); }
            else if (e->key == KEY_HOME) { ui.focus_sel = 0; ui.focus_caret = 0; }
            else if (e->key == KEY_END) { ui.focus_sel = 0; ui.focus_caret = len; }
            else used = false;
            if (used) e->type = EV_NONE;
        }
    }
    gfx_rrect(r, S(9), T->dark ? col_lum_shift(T->panel, -0.15f) : col_lum_shift(T->panel, -0.025f));
    gfx_rrect_line(r, S(9), focused ? S(1.5f) : S(1), focused ? T->accent : col_alpha(T->border, hv ? 1.0f : 0.8f));
    float pad = S(12);
    float base = r.y + r.h * 0.5f + face_ascent(f, px) * 0.36f;
    gfx_push_clip(rect_inset(r, 2));
    if (!buf[0] && ph) draw_text(f, px, r.x + pad, base, ph, -1, col_alpha(T->panel_text, 0.4f));
    if (focused && ui.focus_sel == 1 && buf[0]) {
        float w = text_width(f, px, buf, len);
        gfx_rrect(rect((int)(r.x + pad - S(2)), r.y + SI(6), (int)(w + S(4)), r.h - SI(12)), S(4), T->select);
    }
    draw_text(f, px, r.x + pad, base, buf, len, T->panel_text);
    if (focused) {
        float cx = r.x + pad + text_width(f, px, buf, ui.focus_caret);
        bool on = ((ui.in.now / 530) & 1) == 0;
        if (on) gfx_rect(rect((int)cx, r.y + SI(9), MAX(1, SI(1.6f)), r.h - SI(18)), T->accent);
        ui.redraw = true;
    }
    gfx_pop_clip();
    if (hv) ui.cursor = 1;
    return changed;
}

/* ------------------------------------------------------------ quadro */
void ui_begin_frame(void) {
    ui.nav_found = false; ui.nav_r = rect(0, 0, 0, 0);
    ui.cursor = 0;
    ui.wake_ms = 0;
    ui.tip = 0;
    wm_hit_test();
}
void ui_end_frame(void) {
    if (!ui.tip) ui.tip_id = 0;
    ui.in.pressed = ui.in.released = ui.in.rpressed = ui.in.mpressed = ui.in.mreleased = ui.in.dbl = ui.in.triple = false;
    ui.in.wheel = 0; ui.in.nkeys = 0; ui.in.moved = false; ui.in.typed = false;
}
