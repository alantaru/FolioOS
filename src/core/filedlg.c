/* FOLIO v4 — gerenciador de arquivos (abrir/salvar/exportar) + diálogos genéricos */
#include "folio.h"

static void files_body(Window *w, Rect r);
Window win_files = { N_("Files"), IC_FOLDER, {0, 0, 900, 580}, false, true, true, true, false, 0, 640, 420, files_body, 0, 0, false, -1, 0, 0, 0 };

static int   mode, vol, sel = -1, sort_key, fmt_sel;
static bool  sort_desc, show_all;
static char  cwd[512], fname[160];
static FileEntry *ents; static int nents;
static float scroll;
static u64   last_click; static int last_click_idx = -1;

/* ------------------------------------------------------------ diálogos genéricos */
static char  dlg_title[96], dlg_msg[320], dlg_yes[48], dlg_alt[48], dlg_text[160];
static ConfirmFn dlg_cfn; static PromptFn dlg_pfn;
static void confirm_body(Window *w, Rect r);
static void prompt_body(Window *w, Rect r);
static void confirm_closed(Window *w);
static Window win_confirm = { dlg_title, IC_HELP, {0, 0, 480, 210}, false, true, true, false, false, 0, 300, 160, confirm_body, confirm_closed, 0, false, -1, 0, 0, 0 };
static Window win_prompt = { dlg_title, IC_EDIT, {0, 0, 460, 200}, false, true, true, false, false, 0, 300, 160, prompt_body, 0, 0, false, -1, 0, 0, 0 };

void dialog_confirm(const char *title, const char *msg, const char *yes, const char *alt, ConfirmFn fn) {
    str_copy(dlg_title, title, sizeof dlg_title); str_copy(dlg_msg, msg, sizeof dlg_msg);
    str_copy(dlg_yes, yes ? yes : L("OK"), sizeof dlg_yes); str_copy(dlg_alt, alt ? alt : "", sizeof dlg_alt);
    dlg_cfn = fn;
    win_confirm.r.w = SI(500); win_confirm.r.h = SI(214);
    wm_open(&win_confirm);
}
void dialog_prompt(const char *title, const char *initial, PromptFn fn) {
    str_copy(dlg_title, title, sizeof dlg_title); str_copy(dlg_text, initial ? initial : "", sizeof dlg_text);
    dlg_pfn = fn; ui.focus = 0;
    win_prompt.r.w = SI(480); win_prompt.r.h = SI(196);
    wm_open(&win_prompt);
}

static void wrap_text(Face *f, float px, float x, float y, float w, float lh, const char *s, Color c) {
    int len = str_len(s), i = 0;
    while (i < len) {
        int n = text_fit(f, px, s + i, len - i, w);
        if (n <= 0) n = utf8_next(s + i, len - i, 0);
        if (i + n < len) { int k = n; while (k > 0 && s[i + k] != ' ') k--; if (k > 0) n = k; }
        draw_text(f, px, x, y, s + i, n, c);
        i += n; while (i < len && s[i] == ' ') i++;
        y += lh;
    }
}

static void confirm_body(Window *w, Rect r) {
    Face *f = ui_font(false);
    wrap_text(f, S(15), r.x + S(24), r.y + S(36), r.w - S(48), S(22), dlg_msg, T->panel_text);
    int bw = SI(140), bh = SI(38), by = r.y + r.h - bh - SI(20);
    int x = r.x + r.w - SI(24) - bw;
    ConfirmFn fn = dlg_cfn;
    /* resposta dada: o fechamento não conta como "Cancelar" */
#define ANSWER(c) do { dlg_cfn = 0; wm_close(w); if (fn) fn(c); } while (0)
    if (ui_button(rect(x, by, bw, bh), dlg_yes, str_eq(dlg_yes, L("Delete")) ? BTN_DANGER : BTN_PRIMARY) || ui_key(KEY_ENTER)) { ANSWER(1); return; }
    x -= bw + SI(10);
    if (dlg_alt[0]) { if (ui_button(rect(x, by, bw, bh), dlg_alt, BTN_GHOST)) { ANSWER(2); return; } x -= bw + SI(10); }
    if (ui_button(rect(x, by, bw, bh), L("Cancel"), BTN_SUBTLE)) { ANSWER(0); }
#undef ANSWER
}
/* fechar sem responder (Esc, clique fora, X) = Cancelar */
static void confirm_closed(Window *w) { UNUSED(w); ConfirmFn fn = dlg_cfn; dlg_cfn = 0; if (fn) fn(0); }

static void prompt_body(Window *w, Rect r) {
    ui_textfield(rect(r.x + SI(24), r.y + SI(26), r.w - SI(48), SI(42)), dlg_text, sizeof dlg_text, L("Name"), true);
    int bw = SI(130), bh = SI(38), by = r.y + r.h - bh - SI(20);
    PromptFn fn = dlg_pfn;
    if (ui_button(rect(r.x + r.w - SI(24) - bw, by, bw, bh), L("OK"), BTN_PRIMARY) || ui_key(KEY_ENTER)) {
        wm_close(w); if (fn && dlg_text[0]) fn(dlg_text); return;
    }
    if (ui_button(rect(r.x + r.w - SI(34) - 2 * bw, by, bw, bh), L("Cancel"), BTN_SUBTLE)) wm_close(w);
}

/* ------------------------------------------------------------ listagem */
static bool hidden_name(const char *n) {
    return str_eq(n, ".") || str_eq(n, "..") || str_ieq(n, "System Volume Information") || str_ieq(n, "$RECYCLE.BIN") || n[0] == '.';
}
static bool visible(FileEntry *e) {
    if (hidden_name(e->name)) return false;
    if (e->dir || show_all || mode == FD_SAVE) return true;
    int f = format_from_path(e->name);
    return f == FMT_MD || f == FMT_TXT;
}
static int cmp_ent(FileEntry *a, FileEntry *b) {
    if (a->dir != b->dir) return a->dir ? -1 : 1;
    int c = 0;
    if (sort_key == 1) c = a->size < b->size ? -1 : a->size > b->size ? 1 : 0;
    else if (sort_key == 2) {
        DateTime *x = &a->mtime, *y = &b->mtime;
        long long va = ((((long long)x->year * 13 + x->month) * 32 + x->day) * 24 + x->hour) * 3600 + x->min * 60 + x->sec;
        long long vb = ((((long long)y->year * 13 + y->month) * 32 + y->day) * 24 + y->hour) * 3600 + y->min * 60 + y->sec;
        c = va < vb ? -1 : va > vb ? 1 : 0;
    }
    if (c == 0) c = str_cmp_i(a->name, b->name);
    return sort_desc ? -c : c;
}
static void refresh(void) {
    mem_free(ents); ents = 0; nents = 0;
    FileEntry *all; int n = pal_list(vol, cwd, &all);
    if (n < 0) { ui_toast(L("Could not read this folder")); n = 0; all = 0; }
    ents = mem_alloc(sizeof(FileEntry) * (usize)(n + 1));
    for (int i = 0; i < n; i++) if (visible(&all[i])) ents[nents++] = all[i];
    mem_free(all);
    for (int i = 1; i < nents; i++) {           /* inserção (listas pequenas) */
        FileEntry t = ents[i]; int j = i - 1;
        while (j >= 0 && cmp_ent(&ents[j], &t) > 0) { ents[j + 1] = ents[j]; j--; }
        ents[j + 1] = t;
    }
    if (sel >= nents) sel = nents - 1;
    ui.redraw = true;
}
static void go(int v, const char *path) {
    vol = v; str_copy(cwd, path, sizeof cwd);
    if (!cwd[0]) str_copy(cwd, "/", sizeof cwd);
    sel = -1; scroll = 0;
    refresh();
}

void filedlg_show(int m) {
    mode = m;
    win_files.title = m == FD_SAVE ? L("Save as / Export") : L("Open document");
    win_files.icon = m == FD_SAVE ? IC_SAVE : IC_FOLDER;
    win_files.r.w = MIN(SI(940), ui.W - SI(40)); win_files.r.h = MIN(SI(620), ui.H - SI(60));
    win_files.min_w = SI(640); win_files.min_h = SI(420);
    win_files.centered_once = false;
    if (app.has_path) {
        char dir[512]; str_copy(dir, app.path, sizeof dir); path_parent(dir);
        go(app.vol, dir);
        str_copy(fname, path_basename(app.path), sizeof fname);
        char *dot = 0; for (char *c = fname; *c; c++) if (*c == '.') dot = c;
        if (dot) *dot = 0;
    } else {
        bool isdir;
        (void)isdir; paths_ensure(app_boot_vol(), DIR_DOCS);
        go(app_boot_vol(), paths_dir(DIR_DOCS));
        /* sugere o nome a partir do primeiro título do texto */
        MdDoc m; md_parse(&app.doc, &m);
        str_copy(fname, m.title && m.title[0] ? m.title : L("Untitled"), sizeof fname);
        md_free(&m);
        for (char *c = fname; *c; c++) if (*c == '/' || *c == '\\' || *c == ':' || *c == '*' || *c == '?' || *c == '"' || *c == '<' || *c == '>' || *c == '|') *c = '-';
    }
    if (m == FD_SAVE && fmt_sel < 0) fmt_sel = 0;
    ui.focus = 0;
    wm_open(&win_files);
}

/* ------------------------------------------------------------ ações */
static void do_mkdir(const char *name) {
    char p[512]; path_join(p, sizeof p, cwd, name);
    if (pal_mkdir(vol, p)) { ui_toast(L("Folder created")); refresh(); } else ui_toast(L("Could not create the folder"));
}
static void do_rename(const char *name) {
    if (sel < 0 || sel >= nents) return;
    char p[512]; path_join(p, sizeof p, cwd, ents[sel].name);
    if (pal_rename(vol, p, name)) {
        if (app.has_path && app.vol == vol && str_ieq(app.path, p)) path_join(app.path, sizeof app.path, cwd, name);
        ui_toast(L("Renamed")); refresh();
    } else ui_toast(L("Could not rename"));
}
static bool rm_tree(int v, const char *path) {
    FileEntry *l; int n = pal_list(v, path, &l);
    for (int i = 0; i < n; i++) {
        if (str_eq(l[i].name, ".") || str_eq(l[i].name, "..")) continue;
        char p[512]; path_join(p, sizeof p, path, l[i].name);
        if (l[i].dir) rm_tree(v, p); else pal_remove(v, p);
    }
    if (n >= 0) mem_free(l);
    return pal_remove(v, path);
}
static void do_delete(int choice) {
    if (choice != 1 || sel < 0 || sel >= nents) return;
    char p[512]; path_join(p, sizeof p, cwd, ents[sel].name);
    bool ok = ents[sel].dir ? rm_tree(vol, p) : pal_remove(vol, p);
    ui_toast(ok ? L("Deleted") : L("Could not delete"));
    refresh();
}

static char pending_path[512]; static int pending_fmt;
static void save_confirmed(int c) { if (c == 1) { app_save_as_done(vol, pending_path, pending_fmt); wm_close(&win_files); } }

static void commit(void) {
    if (mode == FD_OPEN) {
        if (sel < 0 || sel >= nents) return;
        char p[512]; path_join(p, sizeof p, cwd, ents[sel].name);
        if (ents[sel].dir) { go(vol, p); return; }
        wm_close(&win_files);
        app_open_path(vol, p);
        return;
    }
    if (!fname[0]) { ui_toast(L("Type a name for the file")); return; }
    char name[200]; str_copy(name, fname, sizeof name);
    for (char *c = name; *c; c++) if (*c == '/' || *c == '\\' || *c == ':' || *c == '*' || *c == '?' || *c == '"' || *c == '<' || *c == '>' || *c == '|') *c = '-';
    int f = format_from_path(name);
    if (f < 0) { f = fmt_sel; str_cat(name, format_info(f)->ext, sizeof name); }
    path_join(pending_path, sizeof pending_path, cwd, name);
    pending_fmt = f;
    bool isdir;
    if (pal_exists(vol, pending_path, &isdir)) {
        if (isdir) { ui_toast(L("A folder with that name already exists")); return; }
        char msg[320]; fmt(msg, sizeof msg, L("\"%s\" already exists in this folder. Do you want to replace the file?"), name);
        dialog_confirm(L("Replace file?"), msg, L("Replace"), 0, save_confirmed);
        return;
    }
    save_confirmed(1);
}

/* ------------------------------------------------------------ desenho */
static void fmt_size(u64 s, char *o, int cap) {
    if (s < 1024) fmt(o, cap, "%d B", (int)s);
    else if (s < 1024 * 1024) fmt(o, cap, L("%.1f KB"), (double)s / 1024.0);
    else if (s < 1024ull * 1024 * 1024) fmt(o, cap, L("%.1f MB"), (double)s / (1024.0 * 1024.0));
    else fmt(o, cap, L("%.1f GB"), (double)s / (1024.0 * 1024.0 * 1024.0));
}

static void files_body(Window *w, Rect r) {
    UNUSED(w);
    Face *f = ui_font(false), *fb = ui_font(true);
    float px = S(14);
    int side = SI(206);
    /* --- barra lateral: volumes e locais --- */
    Rect sb = rect(r.x, r.y, side, r.h);
    gfx_rect(sb, col_alpha(T->text, T->dark ? 0.03f : 0.025f));
    gfx_rect(rect(sb.x + sb.w - 1, sb.y, 1, sb.h), col_alpha(T->border, 0.6f));
    Rect cur = rect(r.x + SI(14), r.y + SI(12), side - SI(28), 0);
    ui_section(&cur, L("DRIVES"));
    int nv = pal_vol_count();
    for (int v = 0; v < nv && v < 8; v++) {
        VolInfo vi; pal_vol_info(v, &vi);
        char lab[80];
        if (vi.boot) fmt(lab, sizeof lab, "%s", vi.label[0] ? vi.label : L("FolioOS USB drive"));
        else fmt(lab, sizeof lab, "%s", vi.label[0] ? vi.label : L("Drive"));
        Rect it = rect(cur.x - SI(6), cur.y, cur.w + SI(12), SI(34));
        bool fire = ui_item(it);
        bool on = v == vol, hv = ui_hover(it);
        if (on) gfx_rrect(it, S(8), col_alpha(T->accent, 0.14f)); else if (hv) gfx_rrect(it, S(8), col_alpha(T->text, 0.06f));
        icon_draw(IC_DRIVE, it.x + S(18), it.y + it.h * 0.5f, S(18), on ? T->accent : col_alpha(T->panel_text, 0.7f));
        draw_text_ellipsis(f, px, it.x + S(36), it.y + it.h * 0.5f + face_ascent(f, px) * 0.36f, it.w - S(42), lab, T->panel_text);
        if (fire) go(v, "/");
        cur.y += SI(36);
    }
    cur.y += SI(10);
    ui_section(&cur, L("PLACES"));
    struct { const char *n, *p; int ic; } places[] = { { L("Documents"), paths_dir(DIR_DOCS), IC_DOC }, { L("Music"), paths_dir(DIR_MUSIC), IC_MUSIC }, { L("Root"), "/", IC_HOME } };
    for (int i = 0; i < 3; i++) {
        Rect it = rect(cur.x - SI(6), cur.y, cur.w + SI(12), SI(34));
        bool fire = ui_item(it);
        bool on = vol == app_boot_vol() && str_ieq(cwd, places[i].p), hv = ui_hover(it);
        if (on) gfx_rrect(it, S(8), col_alpha(T->accent, 0.14f)); else if (hv) gfx_rrect(it, S(8), col_alpha(T->text, 0.06f));
        icon_draw(places[i].ic, it.x + S(18), it.y + it.h * 0.5f, S(18), on ? T->accent : col_alpha(T->panel_text, 0.7f));
        draw_text(f, px, it.x + S(36), it.y + it.h * 0.5f + face_ascent(f, px) * 0.36f, places[i].n, -1, T->panel_text);
        if (fire) {
            bool d; if (!pal_exists(app_boot_vol(), places[i].p, &d) && i < 2) pal_mkdir(app_boot_vol(), places[i].p);
            go(app_boot_vol(), places[i].p);
        }
        cur.y += SI(36);
    }
    if (mode == FD_OPEN) {
        cur.y += SI(10);
        Rect t = rect(cur.x, cur.y, cur.w, SI(30));
        if (ui_toggle(t, L("All files"), &show_all)) refresh();
    }

    /* --- barra de navegação --- */
    int mx = r.x + side + SI(16), mw = r.w - side - SI(32);
    int ty = r.y + SI(12), th = SI(36);
    bool alt_up = false;
    for (int i = 0; i < ui.in.nkeys && ui.key_win == &win_files; i++) {
        Event *k = &ui.in.keys[i];
        if (k->type == EV_KEY && k->key == KEY_UP && (k->mods & MOD_ALT)) { k->type = EV_NONE; alt_up = true; }
    }
    if (ui_icon_btn(rect(mx, ty, th, th), IC_UP, false, L("Parent folder (Alt+↑)")) || alt_up) {
        char p[512]; str_copy(p, cwd, sizeof p); path_parent(p); go(vol, p);
    }
    /* breadcrumb */
    float bx = mx + th + S(10);
    float base = ty + th * 0.5f + face_ascent(f, px) * 0.36f;
    {
        char segs[16][128]; int ns = 0; int i = 0, plen = str_len(cwd);
        while (i < plen && ns < 16) {
            while (i < plen && cwd[i] == '/') i++;
            int s = i; while (i < plen && cwd[i] != '/') i++;
            if (i > s) { int n = MIN(i - s, 127); memcpy(segs[ns], cwd + s, (usize)n); segs[ns][n] = 0; ns++; }
        }
        VolInfo vi; pal_vol_info(vol, &vi);
        const char *root = vi.label[0] ? vi.label : L("Drive");
        float limit = mx + mw - S(136);
        for (int k = -1; k < ns; k++) {
            const char *lab = k < 0 ? root : segs[k];
            float wv = text_width(k == ns - 1 ? fb : f, px, lab, -1);
            if (bx + wv > limit) { draw_text(f, px, bx, base, "…", -1, T->muted); break; }
            Rect hr = rect((int)bx - SI(6), ty + SI(4), (int)wv + SI(12), th - SI(8));
            bool fire = k < ns - 1 && ui_item(hr);
            bool hv = ui_hover(hr);
            if (hv && k < ns - 1) { gfx_rrect(hr, S(6), col_alpha(T->text, 0.07f)); ui.cursor = 2; }
            draw_text(k == ns - 1 ? fb : f, px, bx, base, lab, -1, k == ns - 1 ? T->panel_text : col_alpha(T->panel_text, 0.65f));
            if (fire) {
                char p[512] = "/";
                for (int q = 0; q <= k; q++) { str_cat(p, segs[q], sizeof p); if (q < k) str_cat(p, "/", sizeof p); }
                go(vol, p); return;
            }
            bx += wv + S(8);
            if (k < ns - 1) { icon_draw(IC_CHEVRON, bx + S(4), ty + th * 0.5f, S(12), col_alpha(T->panel_text, 0.4f)); bx += S(16); }
        }
    }
    int ax = mx + mw - th * 3 - SI(8);
    if (ui_icon_btn(rect(ax, ty, th, th), IC_PLUS, false, L("New folder"))) dialog_prompt(L("New folder"), L("New folder"), do_mkdir);
    bool has = sel >= 0 && sel < nents;
    if (ui_icon_btn(rect(ax + th + SI(4), ty, th, th), IC_EDIT, false, L("Rename (F2)")) || ui_key(KEY_F2)) {
        if (has) dialog_prompt(L("Rename"), ents[sel].name, do_rename); else ui_toast(L("Select an item"));
    }
    if (ui_icon_btn(rect(ax + 2 * (th + SI(4)), ty, th, th), IC_TRASH, false, L("Delete"))) {
        if (has) {
            char msg[320]; fmt(msg, sizeof msg, ents[sel].dir ? L("Delete the folder \"%s\" and everything in it? This cannot be undone.") : L("Delete \"%s\"? This cannot be undone."), ents[sel].name);
            dialog_confirm(L("Delete"), msg, L("Delete"), 0, do_delete);
        } else ui_toast(L("Select an item"));
    }

    /* --- lista --- */
    int bottom_h = mode == FD_SAVE ? SI(150) : SI(70);
    Rect list = rect(mx, ty + th + SI(12), mw, r.y + r.h - bottom_h - (ty + th + SI(12)));
    int hh = SI(30);
    int c1 = list.x + SI(40), c3 = list.x + list.w - SI(170), c2 = c3 - SI(110);
    const char *heads[3] = { L("Name"), L("Size"), L("Modified") };
    int hx[3] = { c1, c2, c3 };
    for (int k = 0; k < 3; k++) {
        Rect hr = rect(hx[k] - SI(6), list.y, (k == 0 ? c2 - c1 : SI(104)), hh);
        bool fire = ui_item(hr);
        bool hv = ui_hover(hr);
        Color hc = sort_key == k ? T->accent : col_alpha(T->panel_text, hv ? 0.85f : 0.55f);
        float tw = draw_text(ui_font(true), S(12.5f), hx[k], list.y + hh * 0.62f, heads[k], -1, hc);
        if (sort_key == k) icon_draw(sort_desc ? IC_CHEVRON : IC_UP, tw + S(10), list.y + hh * 0.5f, S(11), hc);
        if (fire) { if (sort_key == k) sort_desc = !sort_desc; else { sort_key = k; sort_desc = k == 2; } refresh(); }
    }
    gfx_rect(rect(list.x, list.y + hh, list.w, 1), col_alpha(T->border, 0.7f));
    Rect rows = rect(list.x, list.y + hh + SI(2), list.w, list.h - hh - SI(2));
    int rh = SI(36);
    float content = (float)nents * rh;
    if (ui_hover(rows) && ui.in.wheel) scroll -= ui.in.wheel * rh * 3;
    scroll = CLAMP(scroll, 0.0f, MAX(0.0f, content - rows.h));
    ui_nav(rows, NAV_LIST);
    /* teclado */
    Event e;
    while (true) {
        bool any = false;
        if (ui_key(KEY_DOWN)) { sel = MIN(nents - 1, sel + 1); any = true; }
        else if (ui_key(KEY_UP)) { sel = MAX(0, sel - 1); any = true; }
        else if (ui_key(KEY_ENTER)) {
            if (mode == FD_SAVE && sel >= 0 && sel < nents && ents[sel].dir && ui.focus != fname) { char p[512]; path_join(p, sizeof p, cwd, ents[sel].name); go(vol, p); }
            else commit();
            return;
        }
        else if (mode == FD_OPEN && ui_key(KEY_BACKSPACE)) { char p[512]; str_copy(p, cwd, sizeof p); path_parent(p); go(vol, p); return; }
        if (!any) break;
        if (sel >= 0) {
            if (sel * rh < scroll) scroll = (float)(sel * rh);
            if ((sel + 1) * rh > scroll + rows.h) scroll = (float)((sel + 1) * rh - rows.h);
            if (mode == FD_SAVE && !ents[sel].dir) {
                str_copy(fname, ents[sel].name, sizeof fname);
            }
        }
    }
    UNUSED(e);
    gfx_push_clip(rows);
    if (!nents) draw_text_c(f, px, rect(rows.x, rows.y, rows.w, SI(80)), mode == FD_OPEN && !show_all ? L("No .md or .txt documents here") : L("Empty folder"), col_alpha(T->panel_text, 0.45f));
    int i0 = (int)(scroll / rh);
    for (int i = i0; i < nents; i++) {
        int y = rows.y + i * rh - (int)scroll;
        if (y > rows.y + rows.h) break;
        Rect rr = rect(rows.x, y, rows.w - SI(10), rh - SI(2));
        FileEntry *fe = &ents[i];
        bool hv = ui_hover(rr) && ui_hover(rows);
        if (i == sel) gfx_rrect(rr, S(8), col_alpha(T->accent, 0.16f));
        else if (hv) gfx_rrect(rr, S(8), col_alpha(T->text, 0.05f));
        int fm = format_from_path(fe->name);
        int ic = fe->dir ? IC_FOLDER : str_ends_i(fe->name, ".mp3") ? IC_MUSIC : (fm >= 0 ? IC_DOC : IC_FILE);
        Color icc = fe->dir ? T->accent : col_alpha(T->panel_text, fm >= 0 ? 0.75f : 0.45f);
        icon_draw(ic, rr.x + S(20), rr.y + rr.h * 0.5f, S(19), icc);
        float bl = rr.y + rr.h * 0.5f + face_ascent(f, px) * 0.36f;
        draw_text_ellipsis(f, px, c1, bl, c2 - c1 - S(14), fe->name, T->panel_text);
        if (!fe->dir) { char sz[32]; fmt_size(fe->size, sz, sizeof sz); draw_text(f, S(13), c2, bl, sz, -1, col_alpha(T->panel_text, 0.55f)); }
        if (fe->mtime.year > 1980) {
            char dt[40]; fmt(dt, sizeof dt, "%02d/%02d/%04d  %02d:%02d", fe->mtime.day, fe->mtime.month, fe->mtime.year, fe->mtime.hour, fe->mtime.min);
            draw_text(f, S(13), c3, bl, dt, -1, col_alpha(T->panel_text, 0.55f));
        }
        if (hv) ui.cursor = 2;
        if (ui.in.pressed && hv) {
            bool dbl = last_click_idx == i && ui.in.now - last_click < 450;
            sel = i; last_click = ui.in.now; last_click_idx = i;
            if (mode == FD_SAVE && !fe->dir) str_copy(fname, fe->name, sizeof fname);
            if (dbl) {
                if (fe->dir) { char p[512]; path_join(p, sizeof p, cwd, fe->name); go(vol, p); gfx_pop_clip(); return; }
                commit(); gfx_pop_clip(); return;
            }
        }
    }
    gfx_pop_clip();
    ui_scrollbar(rect(rows.x + rows.w - SI(6), rows.y, SI(5), rows.h), &scroll, content, (float)rows.h);

    /* --- rodapé --- */
    int fy = r.y + r.h - bottom_h + SI(10);
    gfx_rect(rect(mx - SI(16), fy - SI(10), mw + SI(32), 1), col_alpha(T->border, 0.6f));
    int bw = SI(132), bh = SI(40);
    if (mode == FD_SAVE) {
        ui_textfield(rect(mx, fy + SI(4), mw - 2 * bw - SI(20), SI(40)), fname, sizeof fname, L("File name"), true);
        if (ui_button(rect(mx + mw - bw, fy + SI(4), bw, bh), format_info(fmt_sel)->editable ? L("Save") : L("Export"), BTN_PRIMARY)) { commit(); return; }
        if (ui_button(rect(mx + mw - 2 * bw - SI(10), fy + SI(4), bw, bh), L("Cancel"), BTN_SUBTLE)) { wm_close(&win_files); return; }
        /* formatos */
        int cy = fy + SI(56);
        float x = (float)mx;
        for (int k = 0; k < FMT_COUNT; k++) {
            const FormatInfo *fi = format_info(k);
            char lab[32]; fmt(lab, sizeof lab, "%s", fi->ext);
            float wv = text_width(fb, S(13), lab, -1) + S(24);
            Rect cr = rect((int)x, cy, (int)wv, SI(30));
            bool fire = ui_item(cr);
            bool on = fmt_sel == k, hv = ui_hover(cr);
            gfx_rrect(cr, S(15), on ? T->accent : col_alpha(T->text, hv ? 0.12f : 0.06f));
            draw_text_c(fb, S(13), cr, lab, on ? (col_luma(T->accent) > 0.6f ? 0xFF1A1A1A : 0xFFFFFFFF) : T->panel_text);
            ui_tooltip(fi, cr, L(fi->name));
            if (fire) {
                fmt_sel = k;
                /* troca a extensão digitada, se houver */
                int ff = format_from_path(fname);
                if (ff >= 0) { char *dot = 0; for (char *c = fname; *c; c++) if (*c == '.') dot = c; if (dot) *dot = 0; }
            }
            x += wv + S(8);
        }
        const FormatInfo *fi = format_info(fmt_sel);
        char info[200];
        fmt(info, sizeof info, "%s — %s%s", L(fi->name), L(fi->desc), fi->editable ? "" : L(". The document stays open as .md."));
        draw_text_ellipsis(f, S(13), mx, cy + SI(52), mw, info, col_alpha(T->panel_text, 0.6f));
    } else {
        char info[200];
        if (sel >= 0 && sel < nents) fmt(info, sizeof info, "%s", ents[sel].name); else fmt(info, sizeof info, LN("%d item", "%d items", nents), nents);
        draw_text_ellipsis(f, px, mx, fy + SI(4) + bh * 0.5f + face_ascent(f, px) * 0.36f, mw - 2 * bw - SI(30), info, col_alpha(T->panel_text, 0.6f));
        if (ui_button(rect(mx + mw - bw, fy + SI(4), bw, bh), L("Open"), BTN_PRIMARY)) { commit(); return; }
        if (ui_button(rect(mx + mw - 2 * bw - SI(10), fy + SI(4), bw, bh), L("Cancel"), BTN_SUBTLE)) { wm_close(&win_files); return; }
    }
}
