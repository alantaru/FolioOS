/* FOLIO v4 — aplicação: laço principal, barras, janelas, comandos e atalhos */
#include "folio.h"
#include "disks.h"
#include "install.h"

AppDoc app;
static bool  quit_flag;
static u64   last_frame_ms, last_mouse_ms, sprint_end, sprint_start_words;
static float chrome_a = 1;
static bool  zen;
static int   boot_vol;
static bool  alongside;                    /* instalado ao lado de outro sistema (iniciado por \EFI\FolioOS + FOLIO.ID) */
static int   last_W, last_H, last_theme = -1;
static Surface desk;
static int   cur_mods;
static u64   last_autosave;
static float last_scale;

extern int  kbd_translate(Event *e, u32 out[2]);
extern u32  kbd_pending(void);
extern void dialog_confirm(const char *title, const char *msg, const char *yes, const char *alt, ConfirmFn fn);
extern void dialog_prompt(const char *title, const char *initial, PromptFn fn);
extern int  pal_mods(void);

int  app_boot_vol(void) { return boot_vol; }
bool app_should_quit(void) { return quit_flag; }
void app_request_redraw(void) { ui.redraw = true; }
int  app_mods(void) { return pal_mods() | cur_mods; }

/* ======================================================================
 * documento
 * ====================================================================== */
static void set_title_from_path(void) {
    if (!app.has_path) { str_copy(app.title, L("Untitled"), sizeof app.title); return; }
    str_copy(app.title, path_basename(app.path), sizeof app.title);
}

/* título mostrado na barra: nome do arquivo sem extensão, ou a 1ª linha do texto */
static const char *display_title(AppDoc *ad) {
    static char t[128];
    if (ad->has_path) {
        str_copy(t, path_basename(ad->path), sizeof t);
        char *dot = 0; for (char *c = t; *c; c++) if (*c == '.') dot = c;
        if (dot && dot != t) *dot = 0;
        return t;
    }
    for (int i = 0; i < ad->doc.n && i < 50; i++) {
        Para *p = &ad->doc.p[i];
        int k = 0; while (k < p->len && (p->s[k] == '#' || p->s[k] == ' ' || p->s[k] == '>')) k++;
        if (k >= p->len) continue;
        int pl; char *pt = md_plain(p->s + k, p->len - k, &pl);
        str_copy(t, pt, MIN((int)sizeof t, pl + 1)); mem_free(pt);
        if (str_len(t) > 48) { int n = text_fit(ui_font(false), 100, t, -1, 100 * 26); t[n] = 0; str_cat(t, "…", sizeof t); }
        return t;
    }
    return L("Untitled");
}

/* ------------------------------------------------------------ palavras de hoje (persistem entre sessões) */
static int today_key(void) { DateTime d; pal_now(&d); return d.year * 10000 + d.month * 100 + d.day; }
static int session_words(void) { return MAX(0, doc_words(&app.doc) - app.session_start_words); }
int app_words_today(void) {
    if (prefs.today_date != today_key()) { prefs.today_date = today_key(); prefs.today_words = 0; }
    return prefs.today_words + session_words();
}
static void bank_session(void) {          /* ao trocar de documento, guarda o que foi escrito */
    app_words_today();
    prefs.today_words += session_words();
    app.session_start_words = doc_words(&app.doc);
}

/* ------------------------------------------------------------ volumes lembrados pela identidade, não só pelo índice
 * O índice de um volume é a posição dele na lista do início: sem o pendrive, outro volume pode ocupar o mesmo
 * número. Cada entrada lembrada ("vol@identidade|caminho") só volta no volume com a mesma identidade — o GUID da
 * partição, ou (pendrive sem GPT) o rótulo e o tamanho; se ele estiver em outro índice, vai para lá. */
static void vol_ident(int v, char *o, int cap) {
    VolInfo vi; o[0] = 0;
    if (!pal_vol_info(v, &vi)) return;
    if (vi.has_guid) { int k = 0; for (int i = 0; i < 16 && k + 2 < cap; i++) { o[k++] = "0123456789abcdef"[vi.part_guid[i] >> 4]; o[k++] = "0123456789abcdef"[vi.part_guid[i] & 15]; } o[k] = 0; return; }
    char lab[64]; int k = 0;
    for (int i = 0; vi.label[i] && k < 40; i++) { char c = vi.label[i]; if (c == '|' || c == '@' || c == '#') c = '_'; lab[k++] = c; }
    lab[k] = 0;
    fmt(o, cap, "%s#%d", lab, (int)(vi.size >> 20));
}
static void entry_make(char *o, int cap, int vol, const char *path) {
    char id[80]; vol_ident(vol, id, sizeof id);
    fmt(o, cap, "%d@%s|%s", vol, id, path);
}
/* o volume de uma entrada lembrada (-1 = não está aqui agora); *path aponta para o caminho dentro de e */
static int entry_vol(const char *e, const char **path) {
    const char *bar = e; while (*bar && *bar != '|') bar++;
    *path = *bar ? bar + 1 : bar;
    if (!*bar) return -1;
    int vol = str_to_int(e);
    const char *at = e; while (at < bar && *at != '@') at++;
    if (at == bar) return vol == boot_vol ? vol : -1;   /* formato antigo, sem identidade: só o volume de onde o FolioOS iniciou */
    char want[80]; int n = (int)(bar - at - 1); if (n < 0 || n >= (int)sizeof want) return -1;
    memcpy(want, at + 1, (usize)n); want[n] = 0;
    char id[80];
    if (vol >= 0 && vol < pal_vol_count()) { vol_ident(vol, id, sizeof id); if (str_eq(id, want)) return vol; }
    /* em outro índice, só pelo GUID da partição (único); rótulo + tamanho não bastam para dizer que é o mesmo
     * pendrive (dois iguais existem), então sem GUID só vale o índice gravado, como antes */
    bool guid = n == 32; for (int i = 0; guid && i < n; i++) { char c = want[i]; guid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
    if (guid) for (int v = 0; v < pal_vol_count(); v++) { vol_ident(v, id, sizeof id); if (str_eq(id, want)) return v; }
    return -1;
}

/* o volume do último documento: pela identidade gravada (last_volume_id=); sem ela (CONFIG.INI antigo), só o de boot */
static int last_volume(void) {
    char e[sizeof prefs.last_vol_id + 32];
    if (!prefs.last_vol_id[0]) return prefs.last_vol == boot_vol ? boot_vol : -1;
    fmt(e, sizeof e, "%d@%s|x", prefs.last_vol, prefs.last_vol_id);
    const char *p; return entry_vol(e, &p);
}
void app_last_vol_id(char *o, int cap) { if (app.has_path) vol_ident(app.vol, o, cap); else o[0] = 0; }

/* ------------------------------------------------------------ recentes (cada um lembra onde você parou) */
static int recent_find(int vol, const char *path) {
    /* pelo volume que a entrada indica hoje (que pode ter mudado de índice) e pelo caminho */
    for (int i = 0; i < 6; i++) { const char *p; if (prefs.recent[i][0] && entry_vol(prefs.recent[i], &p) == vol && str_eq(p, path)) return i; }
    return -1;
}
static void add_recent(int vol, const char *path) {
    char key[sizeof prefs.recent[0]]; entry_make(key, sizeof key, vol, path);
    int i = recent_find(vol, path), pp = 0, po = 0;
    if (i >= 0) { pp = prefs.recent_pos[i][0]; po = prefs.recent_pos[i][1]; } else i = 5;
    for (; i > 0; i--) {
        str_copy(prefs.recent[i], prefs.recent[i - 1], sizeof prefs.recent[0]);
        prefs.recent_pos[i][0] = prefs.recent_pos[i - 1][0]; prefs.recent_pos[i][1] = prefs.recent_pos[i - 1][1];
    }
    str_copy(prefs.recent[0], key, sizeof prefs.recent[0]);
    prefs.recent_pos[0][0] = pp; prefs.recent_pos[0][1] = po;
}
/* guarda a posição do documento aberto (no modo leitura, o trecho que está sendo lido) */
void app_remember_pos(void) {
    if (!app.has_path) return;
    int i = recent_find(app.vol, app.path);
    if (i < 0) return;
    prefs.recent_pos[i][0] = preview_top_para();
    prefs.recent_pos[i][1] = preview_active() ? 0 : app.caret.o;
}
static Pos recent_pos_of(int vol, const char *path) {
    int i = recent_find(vol, path);
    return i >= 0 ? (Pos){ prefs.recent_pos[i][0], prefs.recent_pos[i][1] } : (Pos){ 0, 0 };
}

/* ------------------------------------------------------------ versões: cópia do arquivo anterior antes de sobrescrever */
static u64 last_snap_ms; static char last_snap_path[512];
static const char *docs_dir_on(int vol);
/* true = não havia versão anterior, ou ela ficou guardada; false = havia e não deu para guardar (quem vai
 * substituir o arquivo sem poder voltar atrás, como a troca com o pendrive, tem de parar) */
static bool snapshot(int vol, const char *path, bool force) {
    if (!force && str_eq(last_snap_path, path) && ui.in.now - last_snap_ms < 10 * 60000) return true;
    bool d;
    if (!pal_exists(vol, path, &d) || d) return true;
    u8 *old; usize len;
    if (!pal_read(vol, path, &old, &len)) return false;
    paths_ensure(boot_vol, DIR_VERSIONS);
    /* o nome leva a pasta do documento quando ele não está direto em DOCUMENTS: "rascunho (contos)" e
     * "rascunho (cartas)" são documentos diferentes e não podem dividir as versões */
    char base[200]; str_copy(base, path_basename(path), sizeof base);
    char *dot = 0; for (char *c = base; *c; c++) if (*c == '.') dot = c;
    const char *ext = dot ? path_ext(path) : ".md";
    if (dot) *dot = 0;
    {
        char par[300]; str_copy(par, path, sizeof par);
        int k = str_len(par); while (k > 0 && par[k - 1] != '/') k--;
        if (k > 0) par[k - 1] = 0; else par[0] = 0;
        const char *pn = path_basename(par);
        if (par[0] && !str_ieq(par, docs_dir_on(vol)) && pn[0]) { str_cat(base, " (", sizeof base); str_cat(base, pn, sizeof base); str_cat(base, ")", sizeof base); }
    }
    DateTime t; pal_now(&t);
    char vp[400];
    for (int n = 1; ; n++) {                               /* nunca sobrescreve uma versão que já existe */
        if (n == 1) fmt(vp, sizeof vp, "%s/%s · %04d-%02d-%02d %02dh%02d%s", paths_dir(DIR_VERSIONS), base, t.year, t.month, t.day, t.hour, t.min, ext);
        else fmt(vp, sizeof vp, "%s/%s · %04d-%02d-%02d %02dh%02d (%d)%s", paths_dir(DIR_VERSIONS), base, t.year, t.month, t.day, t.hour, t.min, n, ext);
        bool dd; if (!pal_exists(boot_vol, vp, &dd)) break;
        if (n >= 99) { mem_free(old); return false; }
    }
    bool saved = pal_write(boot_vol, vp, old, len);
    mem_free(old);
    if (!saved) return false;
    last_snap_ms = ui.in.now; str_copy(last_snap_path, path, sizeof last_snap_path);
    /* mantém as 30 versões mais recentes deste documento */
    FileEntry *l; int n = pal_list(boot_vol, paths_dir(DIR_VERSIONS), &l);
    if (n <= 30) { if (n >= 0) mem_free(l); return true; }
    char pre[240]; fmt(pre, sizeof pre, "%s · ", base);
    int cnt = 0; for (int i = 0; i < n; i++) if (str_starts(l[i].name, pre)) cnt++;
    const char *mine = path_basename(vp);                  /* a versão que acabou de ser gravada nunca é a que sai */
    #define VKEY(e) (((((i64)(e).mtime.year * 12 + (e).mtime.month) * 31 + (e).mtime.day) * 24 + (e).mtime.hour) * 3600 + (i64)(e).mtime.min * 60 + (e).mtime.sec)
    while (cnt > 30) {
        int oldest = -1;
        for (int i = 0; i < n; i++) {
            if (!l[i].name[0] || !str_starts(l[i].name, pre) || str_ieq(l[i].name, mine)) continue;
            if (oldest < 0 || VKEY(l[i]) < VKEY(l[oldest]) || (VKEY(l[i]) == VKEY(l[oldest]) && str_cmp_i(l[i].name, l[oldest].name) < 0)) oldest = i;
        }
    #undef VKEY
        if (oldest < 0) break;
        char p[400]; path_join(p, sizeof p, paths_dir(DIR_VERSIONS), l[oldest].name);
        pal_remove(boot_vol, p); l[oldest].name[0] = 0; cnt--;
    }
    mem_free(l);
    return true;
}

/* sintaxe do documento ativo pelo nome do arquivo (sem nome: Markdown); chamar depois de trocar app.path */
void app_sync_syntax(void) {
    u8 syn = app.has_path ? (u8)doc_syntax_from_path(app.path) : (u8)SYN_MD;
    if (syn == app.syntax) return;
    app.syntax = syn;
    if (syn == SYN_TEX && preview_active()) preview_close();
    editor_invalidate();
}
/* fim de linha do arquivo, pela primeira linha (\r\n ou \n) */
static bool text_crlf(const u8 *d, usize n) {
    for (usize i = 0; i < n; i++) if (d[i] == '\n') return i > 0 && d[i - 1] == '\r';
    return false;
}

void app_new_doc(void) {
    app_remember_pos();
    bank_session();
    doc_free(&app.doc); doc_init(&app.doc);
    app.has_path = false; app.path[0] = 0; app.vol = boot_vol;
    app.crlf = false; app_sync_syntax();
    app.caret = app.anchor = (Pos){ 0, 0 };
    set_title_from_path();
    app.session_start_words = 0;
    editor_reset_view(); editor_invalidate();
    if (preview_active()) preview_close();
}

static bool load_into(int vol, const char *path, Pos caret) {
    u8 *data; usize len;
    if (!pal_read(vol, path, &data, &len)) { ui_toast(L("Could not open the file")); return false; }
    app_remember_pos();
    bank_session();
    add_recent(vol, path);
    doc_set(&app.doc, (const char *)data, len);
    app.crlf = text_crlf(data, len);
    mem_free(data);
    app.vol = vol; str_copy(app.path, path, sizeof app.path); app.has_path = true;
    app_sync_syntax();
    set_title_from_path();
    app.caret = app.anchor = doc_clamp(&app.doc, caret);
    app.session_start_words = doc_words(&app.doc);
    editor_reset_view(); editor_invalidate();
    editor_scroll_to_caret(true);
    if (preview_active()) preview_close();
    return true;
}

/* ======================================================================
 * abas (opcional, estilo Sublime Text): o documento ativo vive em `app`;
 * os outros ficam guardados em tab_doc[]. Documentos sem nome ganham um
 * rascunho próprio em FOLIO\ABAS\ABA-N.MD, para voltarem após reiniciar.
 * ====================================================================== */
static AppDoc tab_doc[MAX_TABS];
static float  tab_scroll[MAX_TABS];
static int    tab_draft[MAX_TABS];
static u32    tab_dver[MAX_TABS];
static int    ntabs, cur_tab;
static int    cur_draft; static u32 cur_dver = 0xFFFFFFFFu;   /* rascunho do documento ativo (0 = nenhum) */

static void draft_path(int id, char *out, int cap) {
    fmt(out, cap, "%s/TAB-%d.MD", paths_dir(DIR_TABS), id);
    /* pen-drives antigos: o rascunho da aba se chamava ABA-N.MD */
    bool d; char old[96]; fmt(old, sizeof old, "%s/ABA-%d.MD", paths_dir(DIR_TABS), id);
    if (!pal_exists(boot_vol, out, &d) && pal_exists(boot_vol, old, &d)) str_copy(out, old, cap);
}
static AppDoc *tab_at(int i) { return i == cur_tab ? &app : &tab_doc[i]; }
int  app_tab_count(void) { return prefs.tabs ? ntabs : 0; }
int  app_tab_current(void) { return cur_tab; }

static int new_draft_id(void) {
    for (int id = 1; id < 1000; id++) {
        bool used = id == cur_draft;
        for (int i = 0; i < ntabs && !used; i++) if (i != cur_tab && tab_draft[i] == id) used = true;
        for (int i = 0; i < prefs.ntab_saved && !used; i++) if (prefs.tab_list[i][0] == '*' && str_to_int(prefs.tab_list[i] + 2) == id) used = true;
        char p[64]; bool d; draft_path(id, p, sizeof p);
        if (!used && !pal_exists(boot_vol, p, &d)) return id;
    }
    return 999;
}

static bool write_doc_of(AppDoc *ad, int vol, const char *path, int fmtx, bool quiet);

/* grava um documento de aba: o próprio arquivo (.md/.txt/.tex…) ou o rascunho da aba */
static void flush_doc(AppDoc *ad, int *draft, u32 *dver) {
    if (ad->has_path) {
        if (!ad->doc.modified) return;
        int f = format_from_path(ad->path);
        if (path_editable(ad->path) && write_doc_of(ad, ad->vol, ad->path, f >= 0 ? f : FMT_MD, true)) { ad->doc.modified = false; ad->last_save_ms = ui.in.now; }
        return;
    }
    if (doc_chars(&ad->doc) == 0) return;
    if (!*draft) *draft = new_draft_id();
    if (*dver == ad->doc.version) return;
    bool d;
    (void)d; paths_ensure(boot_vol, DIR_TABS);
    char p[64]; draft_path(*draft, p, sizeof p);
    if (write_doc_of(ad, boot_vol, p, FMT_MD, true)) *dver = ad->doc.version;
}
static void drop_draft(int *draft) {
    if (!*draft) return;
    char p[64]; draft_path(*draft, p, sizeof p);
    pal_remove(boot_vol, p);
    *draft = 0;
}

void app_flush_all(void) {
    if (!prefs.tabs) return;
    for (int i = 0; i < ntabs; i++) {
        if (i == cur_tab) flush_doc(&app, &cur_draft, &cur_dver);
        else flush_doc(&tab_doc[i], &tab_draft[i], &tab_dver[i]);
    }
}

/* guarda o documento ativo na sua vaga */
static void stash_current(void) {
    if (preview_active()) preview_close();
    flush_doc(&app, &cur_draft, &cur_dver);
    app_remember_pos();
    bank_session();
    tab_scroll[cur_tab] = editor_get_scroll();
    tab_doc[cur_tab] = app;
    tab_draft[cur_tab] = cur_draft; tab_dver[cur_tab] = cur_dver;
}
/* traz a vaga i para `app` */
static void load_slot(int i) {
    u64 sms = app.session_start_ms;
    app = tab_doc[i];
    memset(&tab_doc[i], 0, sizeof tab_doc[i]);
    cur_draft = tab_draft[i]; cur_dver = tab_dver[i];
    app.session_start_ms = sms ? sms : ui.in.now;
    app.session_start_words = doc_words(&app.doc);
    cur_tab = i;
    editor_reset_view(); editor_invalidate();
    editor_set_scroll(tab_scroll[i]);
    ui.redraw = true;
}
static void fresh_slot(int i) {
    AppDoc *d = &tab_doc[i];
    memset(d, 0, sizeof *d);
    doc_init(&d->doc);
    d->vol = boot_vol;
    str_copy(d->title, L("Untitled"), sizeof d->title);
    tab_draft[i] = 0; tab_dver[i] = 0xFFFFFFFFu; tab_scroll[i] = 0;
}

void app_tab_switch(int i) {
    if (!prefs.tabs || i < 0 || i >= ntabs || i == cur_tab) return;
    stash_current();
    load_slot(i);
    prefs_save();
}
void app_tab_new(void) {
    if (!prefs.tabs) return;
    if (ntabs >= MAX_TABS) { ui_toast(L("Limit of 16 tabs — close one (Ctrl+W)")); return; }
    stash_current();
    int i = ntabs++;
    fresh_slot(i);
    load_slot(i);
    prefs_save();
}

/* fecha a aba ativa sem perguntar (a pergunta, se preciso, já foi feita) */
static void close_current_tab(void) {
    int k = cur_tab;
    if (preview_active()) preview_close();
    app_remember_pos();
    bank_session();
    drop_draft(&cur_draft);
    doc_free(&app.doc);
    for (int j = k; j + 1 < ntabs; j++) {
        tab_doc[j] = tab_doc[j + 1]; tab_draft[j] = tab_draft[j + 1]; tab_dver[j] = tab_dver[j + 1]; tab_scroll[j] = tab_scroll[j + 1];
    }
    ntabs--;
    if (ntabs <= 0) { ntabs = 1; fresh_slot(0); }
    cur_tab = -1;
    load_slot(MIN(k, ntabs - 1));
    prefs_save();
}
static void guard_then(int action);
void app_tab_close(int i) {
    if (!prefs.tabs || i < 0 || i >= ntabs) return;
    if (i != cur_tab) { stash_current(); load_slot(i); }
    guard_then(5);
}

/* lê uma entrada da lista ("vol|caminho" ou "*|N") para uma vaga */
static bool slot_from_entry(int i, const char *e, int pp, int po) {
    fresh_slot(i);
    AppDoc *d = &tab_doc[i];
    u8 *data; usize len;
    if (e[0] == '*') {
        int id = str_to_int(e + 2);
        char p[64]; draft_path(id, p, sizeof p);
        if (!id || !pal_read(boot_vol, p, &data, &len)) { doc_free(&d->doc); return false; }
        doc_set(&d->doc, (const char *)data, len); mem_free(data);
        d->doc.modified = true;
        tab_draft[i] = id; tab_dver[i] = d->doc.version;
    } else {
        const char *path; int vol = entry_vol(e, &path);      /* -1: o volume de onde ela veio não está aqui */
        bool dir;
        if (vol < 0 || !pal_exists(vol, path, &dir) || dir || !pal_read(vol, path, &data, &len)) { doc_free(&d->doc); return false; }
        doc_set(&d->doc, (const char *)data, len); d->crlf = text_crlf(data, len); mem_free(data);
        d->vol = vol; str_copy(d->path, path, sizeof d->path); d->has_path = true;
        d->syntax = (u8)doc_syntax_from_path(d->path);
        str_copy(d->title, path_basename(d->path), sizeof d->title);
    }
    d->caret = d->anchor = doc_clamp(&d->doc, (Pos){ pp, po });
    return true;
}
static bool entry_is_current(const char *e) {
    if (e[0] == '*') return !app.has_path && cur_draft && str_to_int(e + 2) == cur_draft;
    const char *p; int v = entry_vol(e, &p);
    return app.has_path && v >= 0 && v == app.vol && str_eq(p, app.path);
}

void app_tabs_to_prefs(void) {
    if (!prefs.tabs) return;
    int n = 0, act = 0;
    for (int i = 0; i < ntabs && n < MAX_TABS; i++) {
        AppDoc *d = tab_at(i);
        char *e = prefs.tab_list[n];
        if (d->has_path) entry_make(e, sizeof prefs.tab_list[0], d->vol, d->path);
        else {
            int *dr = i == cur_tab ? &cur_draft : &tab_draft[i];
            u32 *dv = i == cur_tab ? &cur_dver : &tab_dver[i];
            if (doc_chars(&d->doc) == 0) continue;          /* aba vazia não é lembrada */
            flush_doc(d, dr, dv);
            if (!*dr) continue;
            fmt(e, sizeof prefs.tab_list[0], "*|%d", *dr);
        }
        if (i == cur_tab) { prefs.tab_pos[n][0] = preview_top_para(); prefs.tab_pos[n][1] = preview_active() ? 0 : app.caret.o; act = n; }
        else { prefs.tab_pos[n][0] = d->caret.p; prefs.tab_pos[n][1] = d->caret.o; }
        n++;
    }
    for (int i = n; i < MAX_TABS; i++) prefs.tab_list[i][0] = 0;
    prefs.ntab_saved = n; prefs.tab_active = act;
}

/* liga: reabre a lista lembrada (o documento atual entra nela); desliga: grava tudo e fica só o atual */
void app_tabs_enable(bool on) {
    if (on == prefs.tabs && (!on || ntabs > 0)) return;
    if (on) {
        prefs.tabs = true;
        ntabs = 0; cur_tab = 0;
        int match = -1;
        for (int k = 0; k < prefs.ntab_saved && ntabs < MAX_TABS; k++) {
            const char *e = prefs.tab_list[k];
            if (!e[0]) continue;
            if (match < 0 && entry_is_current(e)) { match = ntabs; memset(&tab_doc[ntabs], 0, sizeof tab_doc[0]); tab_scroll[ntabs] = 0; ntabs++; }
            else if (slot_from_entry(ntabs, e, prefs.tab_pos[k][0], prefs.tab_pos[k][1])) {
                if (k == prefs.tab_active) prefs.tab_active = ntabs | 0x10000;   /* marca a ativa lembrada */
                ntabs++;
            }
        }
        int remembered = (prefs.tab_active & 0x10000) ? (prefs.tab_active & 0xFFFF) : -1;
        prefs.tab_active &= 0xFFFF;
        bool cur_empty = !app.has_path && doc_chars(&app.doc) == 0;
        if (match >= 0) cur_tab = match;
        else if (cur_empty && ntabs > 0) {
            doc_free(&app.doc);
            cur_tab = -1;
            load_slot(remembered >= 0 ? remembered : 0);
        } else if (ntabs < MAX_TABS) { cur_tab = ntabs++; tab_scroll[cur_tab] = 0; }
        else { /* lista cheia: o atual toma o lugar da última */
            doc_free(&tab_doc[ntabs - 1].doc); cur_tab = ntabs - 1;
        }
        if (cur_tab < 0) cur_tab = 0;
    } else {
        app_tabs_to_prefs();                 /* lembra a lista para quando religar */
        for (int i = 0; i < ntabs; i++) {
            if (i == cur_tab) continue;
            flush_doc(&tab_doc[i], &tab_draft[i], &tab_dver[i]);
            doc_free(&tab_doc[i].doc);
        }
        ntabs = 0; cur_tab = 0;
        prefs.tabs = false;
    }
    ui.redraw = true;
}

static int  pend_vol; static char pend_path[512]; static int pend_action;   /* 1 abrir, 2 novo, 3 desligar, 4 reiniciar, 5 fechar aba */
static void run_pending(void) {
    switch (pend_action) {
    case 1: {
        Pos rp = recent_pos_of(pend_vol, pend_path);
        if (load_into(pend_vol, pend_path, rp)) { cur_draft = 0; cur_dver = 0xFFFFFFFFu; }
        prefs_save();
        if (rp.p || rp.o) ui_toast(L("Picking up where you left off"));
        break; }
    case 2: app_new_doc(); cur_draft = 0; break;
    case 3: case 4: app_flush_all(); prefs_save(); pal_power(pend_action == 3 ? 0 : 1); quit_flag = true; break;
    case 5: pend_action = 0; close_current_tab(); break;
    }
    pend_action = 0;
}
static void unsaved_choice(int c) {
    if (c == 0) { pend_action = 0; return; }
    if (c == 1) { if (!app_save()) { pend_action = 0; return; } }
    run_pending();
}
static void guard_then(int action) {
    pend_action = action;
    /* com abas, desligar/reiniciar não pergunta: tudo vai para o disco e volta no próximo boot */
    if (prefs.tabs && (action == 3 || action == 4)) { run_pending(); return; }
    if (app.doc.modified && (app.has_path || doc_chars(&app.doc) > 0)) {
        dialog_confirm(action == 5 ? L("Close tab without saving?") : L("Save changes?"),
                       L("The current document has changes that have not been saved yet."), L("Save"), L("Don't save"), unsaved_choice);
        return;
    }
    run_pending();
}
/* ------------------------------------------------------------ onde ficam os textos (docs_place=) */
bool app_installed(void) { bool d; return pal_exists(boot_vol, "/FOLIO/INSTALLED.ID", &d) && !d; }
int app_texts_stick(void) {
    /* um outro volume, gravável, removível (pendrive, cartão), com a pasta de textos: o pendrive (o primeiro achado).
     * Uma partição interna com uma pasta DOCUMENTS nunca faz esse papel (os textos ficariam no computador) */
    for (int v = 0; v < pal_vol_count(); v++) {
        VolInfo vi; bool d;
        if (v == boot_vol || !pal_vol_info(v, &vi) || vi.readonly || !vi.removable) continue;
        if ((pal_exists(v, "/DOCUMENTS", &d) && d) || (pal_exists(v, "/TEXTOS", &d) && d)) return v;
    }
    return -1;
}
int app_docs_vol(void) {
    if (prefs.docs_place == DOCS_USB) { int s = app_texts_stick(); if (s >= 0) return s; }
    return boot_vol;
}
static const char *docs_dir_on(int vol) {
    bool d;
    if (vol == boot_vol) return paths_dir(DIR_DOCS);
    return pal_exists(vol, "/TEXTOS", &d) && d && !(pal_exists(vol, "/DOCUMENTS", &d) && d) ? "/TEXTOS" : "/DOCUMENTS";
}
const char *app_docs_dir(void) { return docs_dir_on(app_docs_vol()); }

/* troca dos textos com o pendrive (docs_place=both): o que só existe de um lado vai para o outro; o que existe nos
 * dois e é diferente fica com a versão mais nova (a mais velha vai antes para FOLIO/VERSIONS). Nada é apagado.
 * Um texto aberto em outra aba e diferente fica de fora (não pode haver duas versões dele na memória). */
typedef struct { int a, b, to_b, to_a, skipped, failed; } Xch;
static i64 dt_key(DateTime t) { return ((((i64)t.year * 12 + t.month) * 31 + t.day) * 24 + t.hour) * 3600 + (i64)t.min * 60 + t.sec; }
/* um texto que não pode entrar na troca agora: aberto em outra aba, ou aberto com alterações ainda não salvas (o
 * arquivo no disco não é o que a pessoa está vendo; trocá-lo e recarregar perderia o que ela escreveu) */
static bool open_elsewhere(int vol, const char *path) {
    if (app.has_path && app.vol == vol && str_eq(app.path, path) && app.doc.modified) return true;
    for (int i = 0; prefs.tabs && i < ntabs; i++) {
        if (i == cur_tab) continue;
        AppDoc *d = tab_at(i);
        if (d->has_path && d->vol == vol && str_eq(d->path, path)) return true;
    }
    return false;
}
static bool xch_copy(int sv, const char *sp, int dv, const char *dp) {
    u8 *data; usize len; bool d;
    /* o destino existe? pela lista da pasta (pal_exists diz "não" também quando não consegue ler): se a lista falhar,
     * não há como saber, e nada é substituído */
    bool there = false;
    {
        char par[512]; str_copy(par, dp, sizeof par);
        int k = str_len(par); while (k > 0 && par[k - 1] != '/') k--;
        const char *nm = dp + k; par[k > 1 ? k - 1 : k] = 0; if (!par[0]) str_copy(par, "/", sizeof par);
        FileEntry *l; int n = pal_list(dv, par, &l);
        if (n < 0) return false;
        for (int i = 0; i < n; i++) if (!l[i].dir && str_ieq(l[i].name, nm)) there = true;
        mem_free(l);
    }
    if (!pal_read(sv, sp, &data, &len)) return false;
    /* a versão que vai ser substituída fica guardada; sem conseguir guardá-la, não substitui */
    if (there && (!pal_exists(dv, dp, &d) || d || !snapshot(dv, dp, true))) { mem_free(data); return false; }
    bool ok = pal_write(dv, dp, data, len);
    mem_free(data);
    if (ok && app.has_path && app.vol == dv && str_eq(app.path, dp)) load_into(dv, dp, app.caret);   /* o texto aberto */
    return ok;
}
static void xch_dir(Xch *x, const char *pa, const char *pb, int depth) {
    if (depth > 12) return;
    FileEntry *la = 0, *lb = 0;
    int na = pal_list(x->a, pa, &la), nb = pal_list(x->b, pb, &lb);
    if (na < 0 || nb < 0) {                              /* uma pasta que não deu para ler não é "igual": conta como falha */
        if (na < 0) x->failed++;
        if (nb < 0) x->failed++;
        if (na < 0) na = 0;
        if (nb < 0) nb = 0;
        if (!na || !nb) { if (la) mem_free(la); if (lb) mem_free(lb); return; }
    }
    for (int pass = 0; pass < 2; pass++) {               /* 0: o que está em A (e talvez em B); 1: o que só está em B */
        FileEntry *l = pass ? lb : la; int n = pass ? nb : na;
        for (int i = 0; i < n; i++) {
            FileEntry *e = &l[i], *o = 0;
            if (e->name[0] == '.') continue;
            FileEntry *other = pass ? la : lb; int no = pass ? na : nb;
            for (int k = 0; k < no; k++) if (str_ieq(other[k].name, e->name)) o = &other[k];
            if (pass && o) continue;                       /* já tratado na passada 0 */
            char a[512], b[512];
            path_join(a, sizeof a, pa, e->name); path_join(b, sizeof b, pb, o ? o->name : e->name);
            if (e->dir) {
                if (pass) pal_mkdir(x->a, a); else if (!o) pal_mkdir(x->b, b);
                xch_dir(x, a, b, depth + 1);
                continue;
            }
            if (!o) {                                      /* só de um lado: copia */
                if (open_elsewhere(pass ? x->b : x->a, pass ? b : a)) { x->skipped++; continue; }
                if (pass ? xch_copy(x->b, b, x->a, a) : xch_copy(x->a, a, x->b, b)) { if (pass) x->to_a++; else x->to_b++; } else x->failed++;
                continue;
            }
            u8 *da, *db; usize ln_a, ln_b;
            if (!pal_read(x->a, a, &da, &ln_a)) { x->failed++; continue; }
            if (!pal_read(x->b, b, &db, &ln_b)) { mem_free(da); x->failed++; continue; }
            bool same = ln_a == ln_b && !memcmp(da, db, ln_a);
            mem_free(da); mem_free(db);
            if (same) continue;
            bool a_newer = dt_key(e->mtime) > dt_key(o->mtime);
            if (dt_key(e->mtime) == dt_key(o->mtime)) { x->skipped++; continue; }   /* mesma hora, conteúdo diferente: não adivinha */
            if (open_elsewhere(x->a, a) || open_elsewhere(x->b, b)) { x->skipped++; continue; }
            if (a_newer ? xch_copy(x->a, a, x->b, b) : xch_copy(x->b, b, x->a, a)) { if (a_newer) x->to_b++; else x->to_a++; } else x->failed++;
        }
    }
    if (la) mem_free(la);
    if (lb) mem_free(lb);
}
void app_exchange_texts(void) {
    int s = app_texts_stick();
    if (s < 0) { ui_toast(L("Connect the USB drive with your texts (a DOCUMENTS folder) first.")); return; }
    app_flush_all();                                       /* os textos abertos vão para o disco antes de comparar */
    Xch x = { boot_vol, s, 0, 0, 0, 0 };
    xch_dir(&x, docs_dir_on(boot_vol), docs_dir_on(s), 0);
    char m[1024];                                          /* traduções longas (japonês, russo…) cabem inteiras */
    if (!x.to_a && !x.to_b && !x.skipped && !x.failed) str_copy(m, L("The texts on this computer and on the USB drive are already the same."), sizeof m);
    else fmt(m, sizeof m, L("Texts exchanged: %d to the USB drive, %d to this computer."), x.to_b, x.to_a);
    if (x.failed) { char k[400]; fmt(k, sizeof k, LN(" %d could not be copied (nothing was replaced without its previous version saved).", " %d could not be copied (nothing was replaced without its previous version saved).", x.failed), x.failed); str_cat(m, k, sizeof m); }
    if (x.skipped) { char k[400]; fmt(k, sizeof k, LN(" %d left as it is (open in another tab or with unsaved changes, or changed on both sides at the same minute).", " %d left as they are (open in another tab or with unsaved changes, or changed on both sides at the same minute).", x.skipped), x.skipped); str_cat(m, k, sizeof m); }
    ui_toast(m);
    dbg("texts exchanged: to stick %d, to computer %d, skipped %d, failed %d", x.to_b, x.to_a, x.skipped, x.failed);
}

void app_open_path(int vol, const char *path) {
    int f = format_from_path(path);
    if (f >= 0 && !path_editable(path)) { ui_toast(L("This format is export-only. Open the matching .md file.")); return; }
    pend_vol = vol; str_copy(pend_path, path, sizeof pend_path);
    if (prefs.tabs) {
        /* já aberto: vai para a aba; aba atual vazia: abre nela; senão, aba nova */
        for (int i = 0; i < ntabs; i++) {
            AppDoc *d = tab_at(i);
            if (d->has_path && d->vol == vol && str_eq(d->path, path)) { app_tab_switch(i); return; }
        }
        bool empty = !app.has_path && doc_chars(&app.doc) == 0;
        if (!empty) {
            if (ntabs >= MAX_TABS) { ui_toast(L("Limit of 16 tabs — close one (Ctrl+W)")); return; }
            app_tab_new();
        }
        pend_action = 1; run_pending();
        return;
    }
    guard_then(1);
}

static bool write_doc_of(AppDoc *ad, int vol, const char *path, int fmtx, bool quiet) {
    u8 *data; usize len;
    if (!export_doc(&ad->doc, fmtx, ad->title, &data, &len)) { ui_toast(L("Failed to create the file")); return false; }
    if (ad->crlf && format_info(fmtx)->editable && doc_syntax_from_path(path) == SYN_TEX) {
        /* fontes LaTeX voltam ao disco com o fim de linha que tinham (\r\n) */
        usize n = 0; for (usize i = 0; i < len; i++) if (data[i] == '\n') n++;
        u8 *o = mem_alloc(len + n + 1); usize k = 0;
        for (usize i = 0; i < len; i++) { if (data[i] == '\n') o[k++] = '\r'; o[k++] = data[i]; }
        mem_free(data); data = o; len = k;
    }
    if (format_info(fmtx)->editable && !(vol == boot_vol && str_starts(path, "/FOLIO/"))) snapshot(vol, path, false);
    bool ok = pal_write(vol, path, data, len);
    mem_free(data);
    if (!ok && !quiet) ui_toast(L("Write error — is the USB drive write-protected or full?"));
    return ok;
}
static bool write_doc(int vol, const char *path, int fmtx, bool quiet) { return write_doc_of(&app, vol, path, fmtx, quiet); }

bool app_save(void) {
    if (!app.has_path) { filedlg_show(FD_SAVE); return false; }
    int f = format_from_path(app.path);
    if (f < 0) f = FMT_MD;
    if (!write_doc(app.vol, app.path, f, false)) return false;
    app.doc.modified = false;
    app.last_save_ms = ui.in.now;
    prefs_save();
    char m[200]; fmt(m, sizeof m, L("Saved · %s"), app.title); ui_toast(m);
    return true;
}

void app_save_as_done(int vol, const char *path, int f) {
    if (app.syntax == SYN_TEX && !format_info(f)->editable) { ui_toast(L("Export is not available for LaTeX files")); return; }
    if (!write_doc(vol, path, f, false)) return;
    char m[300];
    if (format_info(f)->editable) {
        drop_draft(&cur_draft);              /* deixou de ser "sem título": o rascunho da aba não serve mais */
        app.vol = vol; str_copy(app.path, path, sizeof app.path); app.has_path = true;
        app_sync_syntax();
        set_title_from_path();
        add_recent(vol, path);
        app.doc.modified = false; app.last_save_ms = ui.in.now;
        prefs_save();
        fmt(m, sizeof m, L("Saved · %s"), app.title);
        if (pend_action) run_pending();
    } else fmt(m, sizeof m, L("Exported · %s"), path_basename(path));
    ui_toast(m);
}

static void autosave(void) {
    if (!app.doc.modified || ui.in.now - app.last_edit_ms < 1500) return;
    if (ui.in.now - last_autosave < (u64)prefs.autosave_s * 1000) return;
    last_autosave = ui.in.now;
    if (app.has_path) {
        int f = format_from_path(app.path);
        if (path_editable(app.path)) { if (write_doc(app.vol, app.path, f >= 0 ? f : FMT_MD, true)) { app.doc.modified = false; app.last_save_ms = ui.in.now; } }
    } else if (prefs.tabs) {
        flush_doc(&app, &cur_draft, &cur_dver);   /* cada aba sem nome tem o seu rascunho */
    } else if (doc_chars(&app.doc) > 0) {
        paths_ensure(boot_vol, DIR_FOLIO);
        write_doc(boot_vol, paths_dir(FILE_DRAFT), FMT_MD, true);
    }
}

/* ======================================================================
 * aparência
 * ====================================================================== */
/* temas do usuário trazem fontes: a do texto vira a preferência; a da interface vale enquanto o tema estiver ativo */
static void apply_theme_fonts(bool text) {
    Family *before = font_ui();
    font_set_ui(T->font_ui ? font_find(T->font_ui) : -1);
    bool changed = before != font_ui();
    if (text && T->font_text) { int f = font_find(T->font_text); if (f >= 0 && f != prefs.font) { prefs.font = f; changed = true; } }
    if (changed) app_apply_font();
}
void app_set_theme(int i) {
    prefs.theme = (i + theme_count()) % theme_count();
    T = theme_get(prefs.theme);
    apply_theme_fonts(true);
    last_theme = -1;
    ui.redraw = true;
}
void app_apply_font(void) { glyph_cache_trim(); editor_invalidate(); }

/* a folha de papel (com sua sombra) fica pronta no fundo: não é redesenhada a cada quadro */
static Rect sheet_r; static bool sheet_baked;
/* modo página (coluna com folha) ou tela cheia (texto em toda a largura) */
bool app_sheet_on(void) { return T->paper_sheet && prefs.paper && !prefs.full_width; }
float app_text_w(float aw) {
    if (prefs.full_width) return MAX(S(200), aw - 2 * MAX(S(56), aw * 0.045f));
    return MIN(prefs.column * ui.scale, aw - S(96));
}
static const int page_sizes[] = { 560, 700, 860, 1040 };
static const char *page_names[] = { N_("Narrow"), N_("Medium"), N_("Wide"), N_("Extra") };
static int page_size_idx(void) { for (int i = 0; i < 4; i++) if ((int)prefs.column == page_sizes[i]) return i; return -1; }
static void set_full_width(bool on) {
    prefs.full_width = on; last_theme = -1; editor_invalidate();
    ui_toast(on ? L("Full screen: the text uses the full width") : L("Page: text in a column, like on a sheet of paper"));
    prefs_save();
}
static void draw_sheet(Rect sheet) {
    gfx_shadow(rect(sheet.x, sheet.y + SI(6), sheet.w, sheet.h), S(6), S(28), T->dark ? 0x60000000 : 0x22302010);
    gfx_rrect(sheet, S(4), T->paper);
}
static void build_desk(void);
void app_paper_sheet(Rect sheet) {
    if (sheet_baked && sheet.x == sheet_r.x && sheet.y == sheet_r.y && sheet.w == sheet_r.w && sheet.h == sheet_r.h) return;   /* já veio com o fundo */
    draw_sheet(sheet);
    sheet_r = sheet; sheet_baked = true;
    build_desk();
}
static void build_desk(void) {
    Surface *s = pal_screen();
    if (desk.w != s->w || desk.h != s->h) {
        mem_free(desk.px);
        desk.w = s->w; desk.h = s->h; desk.stride = s->w;
        desk.px = mem_alloc(sizeof(u32) * (usize)desk.w * desk.h);
    }
    gfx_target(&desk);
    Color top = col_lum_shift(T->desk, T->dark ? 0.025f : 0.02f), bot = col_lum_shift(T->desk, T->dark ? -0.03f : -0.025f);
    if (!(app_sheet_on())) { top = col_mix(T->paper, top, 0.35f); bot = col_mix(T->paper, bot, 0.5f); }
    gfx_gradient_v(rect(0, 0, desk.w, desk.h), top, bot);
    if (T->grain > 0) gfx_vignette(rect(0, 0, desk.w, desk.h), T->dark ? 0xFF000000 : col_lum_shift(T->desk, -0.35f), T->grain * (T->dark ? 0.55f : 0.32f));
    if (sheet_baked) draw_sheet(sheet_r);
    gfx_target(s);
    last_theme = prefs.theme; last_W = s->w; last_H = s->h;
}

/* ======================================================================
 * janelas
 * ====================================================================== */
static void settings_body(Window *w, Rect r);
static void find_body(Window *w, Rect r);
static void outline_body(Window *w, Rect r);
static void stats_body(Window *w, Rect r);
static void help_body(Window *w, Rect r);
static void palette_body(Window *w, Rect r);
static void menu_body(Window *w, Rect r);

static Window win_settings = { N_("Settings"), IC_SETTINGS, {0, 0, 640, 640}, false, false, false, true, false, 0, 520, 420, settings_body, 0, 0, false, -1, 0, 0, 0 };
static Window win_find     = { N_("Find and replace"), IC_SEARCH, {0, 0, 520, 236}, false, false, true, false, false, 0, 420, 200, find_body, 0, 0, false, -1, 0, 0, 0 };
static Window win_outline  = { N_("Outline"), IC_LIST, {0, 0, 340, 520}, false, false, false, true, false, 0, 260, 240, outline_body, 0, 0, false, -1, 0, 0, 0 };
static Window win_stats    = { N_("Session and goals"), IC_CHART, {0, 0, 440, 540}, false, false, false, false, false, 0, 400, 400, stats_body, 0, 0, false, -1, 0, 0, 0 };
static Window win_help     = { N_("Keyboard shortcuts"), IC_HELP, {0, 0, 760, 600}, false, false, false, true, false, 0, 560, 400, help_body, 0, 0, false, -1, 0, 0, 0 };
static Window win_palette  = { N_("Commands"), IC_COMMAND, {0, 0, 600, 440}, false, true, true, false, true, 0, 400, 300, palette_body, 0, 0, false, -1, 0, 0, 0 };
static Window win_menu     = { N_("Menu"), -1, {0, 0, 300, 400}, false, false, false, false, true, 0, 200, 200, menu_body, 0, 0, false, -1, 0, 0, 0 };
static void theme_ed_body(Window *w, Rect r);
static Window win_theme_ed = { N_("Theme editor"), IC_PALETTE, {0, 0, 700, 680}, false, false, true, true, false, 0, 520, 420, theme_ed_body, 0, 0, false, -1, 0, 0, 0 };

static int settings_tab;
static char find_text[128], repl_text[128];
static void find_closed(Window *w) { UNUSED(w); editor_find_highlight(0); }

static void open_settings(int tab) {
    settings_tab = tab;
    win_settings.r.w = MIN(SI(660), ui.W - SI(40)); win_settings.r.h = MIN(SI(680), ui.H - SI(80));
    win_settings.min_w = SI(520); win_settings.min_h = SI(420);
    if (!win_settings.open) win_settings.centered_once = false;
    wm_open(&win_settings);
}

/* ------------------------------------------------------------ comandos */
static void c_new(void) { if (prefs.tabs) app_tab_new(); else guard_then(2); }
static void c_tabs(void) {
    app_tabs_enable(!prefs.tabs);
    char m[120];
    if (prefs.tabs) fmt(m, sizeof m, ntabs > 1 ? L("Tabs on · %d documents open (Ctrl+N new, Ctrl+W close)") : L("Tabs on (Ctrl+N new tab, Ctrl+W close)"), ntabs);
    else str_copy(m, L("Tabs off — your tabs are kept for when you turn them back on"), sizeof m);
    ui_toast(m); prefs_save();
}
static void c_tab_close(void) { if (prefs.tabs) app_tab_close(cur_tab); else ui_toast(L("Turn tabs on to use Ctrl+W (Ctrl+K → Tabs, or the button at the top)")); }
static void c_tab_next(void) { if (prefs.tabs && ntabs > 1) app_tab_switch((cur_tab + 1) % ntabs); }
static void c_tab_prev(void) { if (prefs.tabs && ntabs > 1) app_tab_switch((cur_tab + ntabs - 1) % ntabs); }
static void open_theme_editor(bool edit);
static void c_theme_new(void) { open_theme_editor(false); }
static void c_theme_edit(void) { if (T->custom) open_theme_editor(true); else open_theme_editor(false); }
static void c_open(void) { filedlg_show(FD_OPEN); }
static void c_save(void) { app_save(); }
static void c_saveas(void) { filedlg_show(FD_SAVE); }
static void c_preview(void) { if (preview_active()) preview_leave(); else preview_open(); }
static void c_focus(void) {
    prefs.focus_mode = (prefs.focus_mode + 1) % 4;
    const char *n[] = { L("Focus off"), L("Focus on sentence"), L("Focus on paragraph"), L("Focus on line") };
    ui_toast(n[prefs.focus_mode]); prefs_save();
}
static void c_typewriter(void) { prefs.typewriter = !prefs.typewriter; editor_scroll_to_caret(true); ui_toast(prefs.typewriter ? L("Typewriter: current line always centered") : L("Typewriter off")); prefs_save(); }
static void c_zen(void) { zen = !zen; ui_toast(zen ? L("Zen mode — Esc or move the mouse to the top to leave") : L("Zen mode off")); }
static void c_find(void) {
    if (preview_active()) preview_leave();          /* a busca acontece no texto editável */
    win_find.r.w = SI(540); win_find.r.h = SI(240);
    if (!win_find.centered_once) { win_find.centered_once = true; win_find.r.x = ui.W - win_find.r.w - SI(24); win_find.r.y = SI(64); }
    win_find.on_close = find_closed;
    ui.focus = 0;
    wm_open(&win_find);
    if (pos_cmp(app.caret, app.anchor) != 0 && app.caret.p == app.anchor.p) {
        int n; char *s = doc_slice(&app.doc, app.caret, app.anchor, &n);
        str_copy(find_text, s, sizeof find_text); mem_free(s);
    }
    ui.focus = find_text;
}
static void c_outline(void) {
    win_outline.r.w = SI(340); win_outline.r.h = MIN(SI(560), ui.H - SI(120));
    if (!win_outline.centered_once) { win_outline.centered_once = true; win_outline.r.x = SI(24); win_outline.r.y = SI(70); }
    wm_toggle(&win_outline);
}
static void c_appearance(void) { open_settings(0); }
static void c_writing(void) { open_settings(1); }
static void c_next_theme(void) { app_set_theme(prefs.theme + 1); char m[80]; fmt(m, sizeof m, L("Theme: %s"), L(T->name)); ui_toast(m); prefs_save(); }
static void c_next_font(void) { prefs.font = (prefs.font + 1) % font_count(); app_apply_font(); char m[96]; fmt(m, sizeof m, L("Font: %s"), font_get(prefs.font)->name); ui_toast(m); prefs_save(); }
static void c_bigger(void) { prefs.font_size = MIN(48.0f, prefs.font_size + 1); editor_invalidate(); prefs_save(); }
static void c_smaller(void) { prefs.font_size = MAX(12.0f, prefs.font_size - 1); editor_invalidate(); prefs_save(); }
static void c_music(void) {
    win_music.r.w = SI(440); win_music.r.h = MIN(SI(640), ui.H - SI(100));
    win_music.min_w = SI(360); win_music.min_h = SI(380);
    if (!win_music.centered_once) { win_music.centered_once = true; win_music.r.x = ui.W - win_music.r.w - SI(24); win_music.r.y = SI(70); }
    wm_toggle(&win_music);
}
static void c_play(void) { music_toggle(); }
static void c_next(void) { music_next(1); }
static void c_prev(void) { music_next(-1); }
static void c_sprint(void) {
    if (sprint_end) { sprint_end = 0; ui_toast(L("Sprint stopped")); return; }
    sprint_end = ui.in.now + (u64)prefs.sprint_min * 60000;
    sprint_start_words = (u64)doc_words(&app.doc);
    char m[80]; fmt(m, sizeof m, L("%d-minute sprint started. Happy writing!"), prefs.sprint_min); ui_toast(m);
}
static void c_stats(void) { win_stats.r.w = SI(440); win_stats.r.h = SI(560); wm_toggle(&win_stats); }
static void c_hemingway(void) { prefs.hemingway = !prefs.hemingway; ui_toast(prefs.hemingway ? L("Hemingway mode: deleting is disabled") : L("Hemingway mode off")); prefs_save(); }
static void c_markup(void) { prefs.hide_markup = !prefs.hide_markup; editor_invalidate(); ui_toast(prefs.hide_markup ? L("Markup hidden outside the current paragraph") : L("Markup always visible")); prefs_save(); }
static void c_smart(void) { prefs.smart_typo = !prefs.smart_typo; ui_toast(prefs.smart_typo ? L("Smart typography on (— “ ” …)") : L("Smart typography off")); prefs_save(); }
static void c_help(void) { win_help.r.w = MIN(SI(780), ui.W - SI(40)); win_help.r.h = MIN(SI(700), ui.H - SI(60)); win_help.centered_once = false; wm_toggle(&win_help); }
static void c_shutdown(void) { guard_then(3); }
static void c_reboot(void) { guard_then(4); }
static void c_palette(void);
static void c_menu(void);
static void c_paper(void) { prefs.paper = !prefs.paper; last_theme = -1; prefs_save(); }
static void c_full_width(void) { set_full_width(!prefs.full_width); }
static void c_page_size(void) {
    int i = (page_size_idx() + 1) % 4;
    prefs.column = (float)page_sizes[i]; if (prefs.full_width) prefs.full_width = false, last_theme = -1;
    editor_invalidate(); prefs_save();
    char m[64]; fmt(m, sizeof m, L("Page %s"), L(page_names[i])); ui_toast(m);
}
/* rolagem automática pela paleta (Ctrl+K): o caminho para quem não tem mouse no firmware.
 * Fora da leitura, abre a leitura no trecho do cursor e já liga. */
static void c_autoscroll(void) {
    if (!preview_active()) preview_open();
    preview_toggle_auto();
}
static void c_chapter_next(void) { bool ok = preview_active() ? preview_chapter(1) : editor_chapter(1); if (!ok) ui_toast(L("This is the last chapter")); }
static void c_chapter_prev(void) { if (preview_active()) preview_chapter(-1); else editor_chapter(-1); }
static void c_last_edit(void) { if (preview_active()) preview_leave(); editor_jump_last_edit(); }
static void c_dup(void) { if (!preview_active()) editor_dup_para(); }
static void c_del(void) { if (!preview_active()) editor_del_para(); }
static void c_case(void) { if (!preview_active()) editor_cycle_case(); }
static void c_date(void) {
    if (preview_active()) preview_leave();
    DateTime t; pal_now(&t);
    char s[96]; i18n_date(s, sizeof s, t.day, t.month, t.year);   /* ordem e nomes vêm do idioma (X-Date-Format) */
    editor_insert_text(s, str_len(s));
}
static int ed_stepper(Rect r, const char *label, const char *value, Face *vf);
/* mouse e touchpad desligados: tudo pelo teclado (Ajustes → Sistema, Ctrl+K → Mouse, CONFIG.INI use_mouse=0) */
static void c_mouse(void) {
    prefs.no_mouse = !prefs.no_mouse;
    prefs_save();
    ui_toast(prefs.no_mouse ? L("Mouse and touchpad off: keyboard only (Ctrl+K → Mouse turns them back on)") : L("Mouse and touchpad on"));
}
static void c_focus_cycle(void) { wm_cycle_focus(); }
/* abriu uma janela pelo teclado (atalho, paleta, menu): o teclado vai junto para ela */
static int top_z(void) { Window *t = wm_top(); return t ? t->z : 0; }
static void focus_new_top(int z0) { Window *t = wm_top(); if (t && t->z > z0) wm_focus(t); }

/* troca o idioma da interface; se o idioma pede fontes que não estão no pen-drive (ex.: chinês
 * sem o pacote de fontes), volta ao anterior e avisa, para a tela nunca ficar ilegível */
void app_set_language(int idx, bool save) {
    int prev = i18n_current();
    if (!i18n_set(idx)) { ui_toast(L("Could not read the language file")); return; }
    if (!fonts_lang_ok()) {
        char need[160]; str_copy(need, lang_fonts(idx), sizeof need);
        i18n_set(prev);
        char m[300]; fmt(m, sizeof m, L("This language needs fonts (%s) that are missing from %s. Copy FolioOS.zip to the USB drive again."), need, paths_show(DIR_FONTS));
        ui_toast(m);
        return;
    }
    str_copy(prefs.lang, lang_code(idx), sizeof prefs.lang);
    editor_invalidate();
    ui.redraw = true;
    if (save) prefs_save();
}
static void c_language(void) { open_settings(2); }

typedef struct { const char *name, *keys; void (*fn)(void); int icon; } Cmd;
static const Cmd cmds[] = {
    { N_("New document"), N_("Ctrl+N"), c_new, IC_FILE },
    { N_("Tabs: several documents (on/off)"), "", c_tabs, IC_TABS },
    { N_("Close tab"), N_("Ctrl+W"), c_tab_close, IC_CLOSE },
    { N_("Next tab"), N_("Ctrl+Tab"), c_tab_next, IC_FWD },
    { N_("Previous tab"), N_("Ctrl+Shift+Tab"), c_tab_prev, IC_BACK },
    { N_("Create a custom theme (colors and fonts)"), "", c_theme_new, IC_PALETTE },
    { N_("Edit the current theme"), "", c_theme_edit, IC_EDIT },
    { N_("Open…"), N_("Ctrl+O"), c_open, IC_FOLDER },
    { N_("Save"), N_("Ctrl+S"), c_save, IC_SAVE },
    { N_("Save as / Export…"), N_("Ctrl+Shift+S"), c_saveas, IC_EXPORT },
    { N_("Reading mode (formatted Markdown)"), N_("Ctrl+P"), c_preview, IC_EYE },
    { N_("Reading auto-scroll (on/off)"), N_("Enter (reading)"), c_autoscroll, IC_PLAY },
    { N_("Next chapter"), N_("Ctrl+PgDn"), c_chapter_next, IC_LIST },
    { N_("Previous chapter"), N_("Ctrl+PgUp"), c_chapter_prev, IC_LIST },
    { N_("Go to the last edit"), N_("Ctrl+J"), c_last_edit, IC_EDIT },
    { N_("Duplicate paragraph"), N_("Ctrl+D"), c_dup, IC_DOC },
    { N_("Delete paragraph"), N_("Ctrl+Shift+K"), c_del, IC_TRASH },
    { N_("Change case"), N_("Shift+F3"), c_case, IC_EDIT },
    { N_("Insert today's date"), "", c_date, IC_TIMER },
    { N_("Focus mode (sentence / paragraph / line)"), "F2", c_focus, IC_FOCUS },
    { N_("Typewriter (centered line)"), "F3", c_typewriter, IC_TYPEWRITER },
    { N_("Zen mode (no bars)"), "F11", c_zen, IC_MOON },
    { N_("Find and replace"), N_("Ctrl+F"), c_find, IC_SEARCH },
    { N_("Document outline"), "F5", c_outline, IC_LIST },
    { N_("Appearance: themes and typography"), "F12", c_appearance, IC_PALETTE },
    { N_("Next theme"), N_("Ctrl+T"), c_next_theme, IC_PALETTE },
    { N_("Next font"), N_("Ctrl+Shift+T"), c_next_font, IC_EDIT },
    { N_("Larger text"), N_("Ctrl+="), c_bigger, IC_PLUS },
    { N_("Smaller text"), N_("Ctrl+-"), c_smaller, IC_PLUS },
    { N_("Page or full screen (text width)"), "", c_full_width, IC_DOC },
    { N_("Page size: narrow · medium · wide · extra"), "", c_page_size, IC_DOC },
    { N_("Paper sheet (show/hide)"), "", c_paper, IC_DOC },
    { N_("Writing settings"), "", c_writing, IC_SETTINGS },
    { N_("Music: play / pause"), "F8", c_play, IC_PLAY },
    { N_("Music: next track"), "F9", c_next, IC_NEXT },
    { N_("Music: previous track"), "F7", c_prev, IC_PREV },
    { N_("Music player"), "F6", c_music, IC_MUSIC },
    { N_("Sound test (chime)"), "", music_chime, IC_VOLUME },
    { N_("Start / stop writing sprint"), "F4", c_sprint, IC_TIMER },
    { N_("Session and goals"), "F10", c_stats, IC_CHART },
    { N_("Hemingway mode (no deleting)"), "", c_hemingway, IC_TYPEWRITER },
    { N_("Hide markup outside the paragraph"), "", c_markup, IC_EYE },
    { N_("Smart typography (— “ ” …)"), "", c_smart, IC_EDIT },
    { N_("Keyboard shortcuts"), "F1", c_help, IC_HELP },
    { N_("Menu and recent documents"), N_("Alt+M"), 0, IC_MENU },
    { N_("Keyboard focus: text or the next window"), N_("Ctrl+F6"), c_focus_cycle, IC_COMMAND },
    { N_("Mouse and touchpad (on/off)"), "", c_mouse, IC_SETTINGS },
    { N_("Input method on/off (Korean, Japanese)"), N_("Ctrl+Space"), ime_toggle, IC_EDIT },
    { N_("Language and keyboard"), "", c_language, IC_SETTINGS },
    { N_("Install FolioOS on this computer"), "", installer_open, IC_DRIVE },
    { N_("Update FolioOS on this computer"), "", installer_open_update, IC_DRIVE },
    { N_("Exchange texts with the USB drive"), "", app_exchange_texts, IC_DOC },
    { N_("Remove FolioOS from this computer"), "", installer_open_remove, IC_DRIVE },
    { N_("Disks and partitions"), "", partitions_open, IC_DRIVE },
    { N_("Restart the computer"), "", c_reboot, IC_POWER },
    { N_("Shut down the computer"), N_("Ctrl+Q"), c_shutdown, IC_POWER },
};

/* ------------------------------------------------------------ paleta */
static char pal_text[96]; static int pal_sel; static int pal_idx[64], pal_n;
static bool fuzzy(const char *hay, const char *needle) {
    int hl = str_len(hay), nl = str_len(needle), j = 0;
    for (int i = 0; i < hl && j < nl; ) {
        u32 a, b; int k = utf8_decode(hay, hl, i, &a); int m = utf8_decode(needle, nl, j, &b);
        if (cp_fold(a) == cp_fold(b)) j += m;
        i += k;
    }
    return j >= nl;
}
static void c_palette(void) {
    pal_text[0] = 0; pal_sel = 0; ui.focus = 0;
    win_palette.r.w = MIN(SI(620), ui.W - SI(40)); win_palette.r.h = MIN(SI(470), ui.H - SI(100));
    win_palette.centered_once = false;
    wm_open(&win_palette);
    win_palette.r.y = SI(90);
}
static void palette_body(Window *w, Rect r) {
    ui_textfield(rect(r.x + SI(16), r.y + SI(16), r.w - SI(32), SI(46)), pal_text, sizeof pal_text, L("Type a command…"), true);
    pal_n = 0;
    for (int i = 0; i < COUNT(cmds) && pal_n < 64; i++) if (fuzzy(L(cmds[i].name), pal_text)) pal_idx[pal_n++] = i;
    if (ui_key(KEY_DOWN)) pal_sel++;
    if (ui_key(KEY_UP)) pal_sel--;
    pal_sel = CLAMP(pal_sel, 0, MAX(0, pal_n - 1));
    bool run = ui_key(KEY_ENTER);
    int y = r.y + SI(74), rh = SI(40);
    int first = MAX(0, pal_sel - (r.h - SI(90)) / rh + 1);
    Face *f = ui_font(false);
    gfx_push_clip(rect(r.x, y, r.w, r.y + r.h - y - SI(8)));
    for (int k = first; k < pal_n; k++) {
        const Cmd *c = &cmds[pal_idx[k]];
        Rect it = rect(r.x + SI(10), y, r.w - SI(20), rh - SI(2));
        if (it.y > r.y + r.h) break;
        bool hv = ui_hover(it);
        if (hv && ui.in.moved) pal_sel = k;
        if (k == pal_sel) gfx_rrect(it, S(8), col_alpha(T->accent, 0.15f));
        icon_draw(c->icon, it.x + S(20), it.y + it.h * 0.5f, S(17), k == pal_sel ? T->accent : col_alpha(T->panel_text, 0.6f));
        draw_text(f, S(14.5f), it.x + S(42), it.y + it.h * 0.5f + face_ascent(f, S(14.5f)) * 0.36f, L(c->name), -1, T->panel_text);
        if (c->keys[0]) {
            float kw = text_width(f, S(12.5f), L(c->keys), -1);
            Rect kr = rect((int)(it.x + it.w - kw - S(24)), it.y + SI(9), (int)(kw + S(14)), it.h - SI(18));
            gfx_rrect(kr, S(5), col_alpha(T->text, 0.07f));
            draw_text_c(f, S(12.5f), kr, L(c->keys), col_alpha(T->panel_text, 0.6f));
        }
        if (hv) ui.cursor = 2;
        if (ui.in.pressed && hv) { pal_sel = k; run = true; }
        y += rh;
    }
    gfx_pop_clip();
    if (!pal_n) draw_text_c(f, S(14), rect(r.x, r.y + SI(80), r.w, SI(30)), L("No command found"), col_alpha(T->panel_text, 0.45f));
    int want = SI(74) + MAX(1, MIN(pal_n, 9)) * rh + SI(14);
    if (w->r.h != want + SI(2)) { w->r.h = want + SI(2); ui.redraw = true; }
    if (run && pal_n) {
        bool kb = !ui.in.pressed;
        wm_close(w);
        int z0 = top_z();
        void (*fn)(void) = cmds[pal_idx[pal_sel]].fn;
        if (fn) fn(); else c_menu();
        if (kb) focus_new_top(z0);
    }
}

/* ------------------------------------------------------------ menu */
static void c_menu(void) {
    win_menu.r = rect(SI(12), SI(52), SI(320), SI(200));
    win_menu.centered_once = true;
    wm_toggle(&win_menu);
    win_menu.nav = 0;            /* pelo teclado (Alt+M): ↑/↓ e Enter já funcionam */
}
static void open_recent(int i) {
    const char *path; int vol = entry_vol(prefs.recent[i], &path);
    if (!path[0]) return;
    bool d;
    if (vol < 0 || !pal_exists(vol, path, &d)) { ui_toast(L("File not found (a different USB drive?)")); return; }
    app_open_path(vol, path);
}
static void menu_body(Window *w, Rect r) {
    typedef struct { const char *n, *k; void (*fn)(void); int ic; int recent; } It;
    It items[40]; int n = 0;
#define SEC(t) items[n++] = (It){ t, 0, 0, -1, -1 }
#define ITM(t, k, f, ic) items[n++] = (It){ t, k, f, ic, -1 }
    SEC(L("DOCUMENT"));
    ITM(L("New"), L("Ctrl+N"), c_new, IC_FILE); ITM(L("Open…"), L("Ctrl+O"), c_open, IC_FOLDER);
    ITM(L("Save"), L("Ctrl+S"), c_save, IC_SAVE); ITM(L("Save as / Export…"), L("Ctrl+Shift+S"), c_saveas, IC_EXPORT);
    int nrec = 0;
    for (int i = 0; i < 6 && nrec < 4; i++) {
        if (!prefs.recent[i][0]) continue;
        const char *bar = prefs.recent[i]; while (*bar && *bar != '|') bar++;
        if (!*bar || (app.has_path && str_eq(bar + 1, app.path))) continue;
        if (!nrec) SEC(L("RECENT"));
        items[n++] = (It){ path_basename(bar + 1), "", 0, IC_DOC, i };
        nrec++;
    }
    SEC(L("WRITING"));
    ITM(L("Find and replace"), L("Ctrl+F"), c_find, IC_SEARCH); ITM(L("Outline"), "F5", c_outline, IC_LIST);
    ITM(L("Session, goals and sprint"), "F10", c_stats, IC_CHART);
    SEC(L("PREFERENCES"));
    ITM(L("Appearance"), "F12", c_appearance, IC_PALETTE); ITM(L("Writing settings"), "", c_writing, IC_SETTINGS);
    ITM(L("Keyboard shortcuts"), "F1", c_help, IC_HELP);
    items[n++] = (It){ 0, 0, 0, 0, -1 };
    ITM(L("Shut down"), L("Ctrl+Q"), c_shutdown, IC_POWER);
#undef SEC
#undef ITM
    int y = r.y + SI(6);
    Face *f = ui_font(false), *fb = ui_font(true);
    for (int i = 0; i < n; i++) {
        It *t = &items[i];
        if (!t->n) { gfx_rect(rect(r.x + SI(14), y + SI(5), r.w - SI(28), 1), col_alpha(T->border, 0.7f)); y += SI(11); continue; }
        if (t->ic < 0) {
            draw_text(fb, S(11), r.x + S(18), y + S(21), t->n, -1, col_alpha(T->panel_text, 0.42f));
            y += SI(28); continue;
        }
        Rect it = rect(r.x + SI(6), y, r.w - SI(12), SI(34));
        bool fire = ui_item(it), hv = ui_hover(it);
        if (hv) gfx_rrect(it, S(8), col_alpha(T->accent, 0.12f));
        icon_draw(t->ic, it.x + S(18), it.y + it.h * 0.5f, S(16), hv ? T->accent : col_alpha(T->panel_text, 0.55f));
        draw_text_ellipsis(f, S(14), it.x + S(38), it.y + it.h * 0.5f + face_ascent(f, S(14)) * 0.36f, it.w - S(110), t->n, T->panel_text);
        if (t->k && t->k[0]) { float kw = text_width(f, S(12), t->k, -1); draw_text(f, S(12), it.x + it.w - kw - S(12), it.y + it.h * 0.5f + S(4.3f), t->k, -1, col_alpha(T->panel_text, 0.4f)); }
        if (fire) {
            bool kb = !ui.in.pressed;
            wm_close(w);
            int z0 = top_z();
            if (t->recent >= 0) open_recent(t->recent); else t->fn();
            if (kb) focus_new_top(z0);
            return;
        }
        y += SI(35);
    }
    w->r.h = y - r.y + SI(8);
}

/* ------------------------------------------------------------ ajustes */
static void theme_card(Rect c, int ti) {
    const Theme *t = theme_get(ti);
    bool fire = ui_item(c);
    bool on = ti == prefs.theme, hv = ui_hover(c);
    if (on) gfx_rrect(rect(c.x - SI(3), c.y - SI(3), c.w + SI(6), c.h + SI(6)), S(12), T->accent);
    else if (hv) gfx_rrect(rect(c.x - SI(2), c.y - SI(2), c.w + SI(4), c.h + SI(4)), S(11), col_alpha(T->text, 0.18f));
    gfx_rrect(c, S(9), t->desk);
    Rect sheet = rect(c.x + c.w / 6, c.y + SI(8), c.w * 2 / 3, c.h - SI(30));
    gfx_rrect(sheet, S(4), t->paper);
    for (int k = 0; k < 4; k++) {
        int lw = (k == 0) ? sheet.w / 2 : (k == 3 ? sheet.w / 3 : sheet.w - SI(16));
        gfx_rrect(rect(sheet.x + SI(8), sheet.y + SI(9) + k * SI(9), lw, SI(4)), S(2), k == 0 ? t->heading : col_alpha(t->text, 0.7f));
    }
    gfx_circle(sheet.x + sheet.w - S(12), sheet.y + S(12), S(4), t->accent);
    if (t->custom) icon_draw(IC_STAR, c.x + S(12), c.y + S(12), S(12), t->accent);   /* tema do usuário */
    Face *f = ui_font(on);
    draw_text_c(f, S(12), rect(c.x, c.y + c.h - SI(22), c.w, SI(20)), L(t->name), t->dark ? 0xFFEDEDED : 0xFF222222);
    if (fire && !on) { app_set_theme(ti); prefs_save(); }
}

/* FOLIO\DIAGNOSTICO.TXT: o que o firmware e o hardware informaram (mouse, touchpad, som).
 * Gravado a cada boot e pelo botão em Ajustes → Sistema; legível em qualquer computador. */
static void app_write_diag(bool notify) {
    Buf b = {0};
    DateTime t; pal_now(&t);
    buf_printf(&b, "FolioOS — hardware diagnostics · %04d-%02d-%02d %02d:%02d\r\n", t.year, t.month, t.day, t.hour, t.min);
    buf_puts(&b, "Send this file to the FolioOS maintainers if the mouse, touchpad or sound does not work.\r\n\r\n");
    pal_diag(&b);
    buf_puts(&b, "\r\n"); disks_diag(&b);
    bool d;
    (void)d; paths_ensure(boot_vol, DIR_FOLIO);
    bool ok = pal_write(boot_vol, paths_dir(FILE_DIAG), b.s, b.len);
    buf_free(&b);
    if (notify) ui_toast(ok ? pathfmt(L("Diagnostics saved to %s"), FILE_DIAG) : L("Could not save the diagnostics"));
}

void app_save_diag(void) { app_write_diag(false); }

/* manter proporção: o que o ajuste pede à PAL */
static void app_apply_aspect(void) {
    static const int aw[5] = { 0, -1, 16, 16, 4 }, ah[5] = { 0, -1, 9, 10, 3 };
    int a = CLAMP(prefs.aspect, 0, 4);
    pal_set_aspect(aw[a], ah[a]);
}

/* volta à resolução e rotação automáticas (atalho Ctrl+Alt+Home, útil se a imagem sumir) */
static void app_video_reset(void) {
    prefs.rotation = 0; prefs.video_mode[0] = 0; prefs.aspect = 0;
    pal_set_rotation(0); pal_set_video_mode(""); pal_set_aspect(0, 0);
    prefs_save();
    ui_toast(L("Display restored: automatic resolution and rotation"));
}

static float set_scroll;
static void settings_body(Window *w, Rect r) {
    UNUSED(w);
    const char *tabs[] = { L("Appearance"), L("Writing"), L("System") };
    ui_segmented(rect(r.x + SI(20), r.y + SI(14), r.w - SI(40), SI(38)), tabs, 3, &settings_tab);
    Rect area = rect(r.x, r.y + SI(62), r.w, r.h - SI(62));
    if (ui_hover(area) && ui.in.wheel) set_scroll -= ui.in.wheel * SI(60);
    gfx_push_clip(area);
    int x = r.x + SI(24), wd = r.w - SI(48);
    Rect cur = rect(x, area.y + SI(8) - (int)set_scroll, wd, 0);
    Face *f = ui_font(false);
    char vt[48];
    if (settings_tab == 0) {
        ui_section(&cur, L("THEMES"));
        int cols = MAX(3, wd / SI(118)), cw = (wd - (cols - 1) * SI(12)) / cols, ch = SI(92);
        for (int i = 0; i < theme_count(); i++) {
            int cx = cur.x + (i % cols) * (cw + SI(12)), cy = cur.y + (i / cols) * (ch + SI(14));
            theme_card(rect(cx, cy, cw, ch), i);
        }
        cur.y += ((theme_count() + cols - 1) / cols) * (ch + SI(14)) + SI(4);
        if (ui_button(rect(cur.x, cur.y, SI(210), SI(38)), L("+  Create my theme"), BTN_PRIMARY)) c_theme_new();
        if (T->custom && ui_button(rect(cur.x + SI(220), cur.y, SI(170), SI(38)), L("Edit this theme"), BTN_GHOST)) c_theme_edit();
        cur.y += SI(46);
        draw_text(f, S(12), cur.x, cur.y + S(12), pathfmt(L("Mix colors and fonts; themes are saved in %s (editable .INI files)."), DIR_THEMES), -1, col_alpha(T->panel_text, 0.45f));
        cur.y += SI(34);
        ui_section(&cur, L("TEXT FONT"));
        for (int i = 0; i < font_count(); i++) {
            Family *fam = font_get(i);
            Rect it = rect(cur.x, cur.y, wd, SI(44));
            bool fire = ui_item(it);
            bool on = i == prefs.font, hv = ui_hover(it);
            if (on) gfx_rrect(it, S(9), col_alpha(T->accent, 0.13f)); else if (hv) gfx_rrect(it, S(9), col_alpha(T->text, 0.05f));
            Face *ff = fam_face(fam, 0);
            draw_text(ff, S(19), it.x + S(14), it.y + it.h * 0.5f + face_ascent(ff, S(19)) * 0.34f, fam->name, -1, T->panel_text);
            const char *sample = L("The quick brown fox jumps — “Hello!”");
            float sw = text_width(ff, S(15), sample, -1);
            draw_text(ff, S(15), it.x + it.w - sw - S(14), it.y + it.h * 0.5f + face_ascent(ff, S(15)) * 0.34f, sample, -1, col_alpha(T->panel_text, 0.55f));
            if (fire && !on) { prefs.font = i; app_apply_font(); prefs_save(); }
            cur.y += SI(48);
        }
        draw_text(f, S(12), cur.x, cur.y + S(14), pathfmt(L("Add .ttf fonts to %s (subfolders work too) and restart."), DIR_FONTS), -1, col_alpha(T->panel_text, 0.45f));
        draw_text(f, S(12), cur.x, cur.y + S(32), L("Included writing fonts: Literata, Lora, EB Garamond, iA Writer, Special Elite and more."), -1, col_alpha(T->panel_text, 0.45f));
        cur.y += SI(48);
        ui_section(&cur, L("TYPOGRAPHY"));
        fmt(vt, sizeof vt, L("%d px"), (int)prefs.font_size);
        if (ui_slider(rect(cur.x, cur.y, wd, SI(52)), &prefs.font_size, 12, 40, L("Text size"), vt)) { prefs.font_size = f_round(prefs.font_size); editor_invalidate(); }
        cur.y += SI(58);
        fmt(vt, sizeof vt, "%.2f", (double)prefs.line_height);
        if (ui_slider(rect(cur.x, cur.y, wd, SI(52)), &prefs.line_height, 1.2f, 2.2f, L("Line spacing"), vt)) editor_invalidate();
        cur.y += SI(58);
        fmt(vt, sizeof vt, L("%.2f lines"), (double)prefs.para_space);
        if (ui_slider(rect(cur.x, cur.y, wd, SI(52)), &prefs.para_space, 0, 1.2f, L("Space between paragraphs"), vt)) editor_invalidate();
        cur.y += SI(62);
        if (ui_toggle(rect(cur.x, cur.y, wd, SI(36)), L("First-line indent for paragraphs (book style)"), &prefs.first_indent)) { editor_invalidate(); prefs_save(); }
        cur.y += SI(48);
        ui_section(&cur, L("PAGE"));
        {
            const char *lm[] = { L("Page"), L("Full screen") };
            int mode = prefs.full_width ? 1 : 0;
            if (ui_segmented(rect(cur.x, cur.y, wd, SI(38)), lm, 2, &mode)) set_full_width(mode == 1);
            cur.y += SI(48);
        }
        if (!prefs.full_width) {
            int sz = page_size_idx();
            if (ui_segmented(rect(cur.x, cur.y, wd, SI(38)), page_names, 4, &sz)) { prefs.column = (float)page_sizes[sz]; editor_invalidate(); prefs_save(); }
            cur.y += SI(46);
            fmt(vt, sizeof vt, L("%d px (~%d characters per line)"), (int)prefs.column, (int)(prefs.column / (prefs.font_size * 0.5f)));
            if (ui_slider(rect(cur.x, cur.y, wd, SI(52)), &prefs.column, 420, 1200, L("Page width"), vt)) { prefs.column = f_round(prefs.column / 10) * 10; editor_invalidate(); }
            cur.y += SI(58);
            if (ui_toggle(rect(cur.x, cur.y, wd, SI(36)), L("Paper sheet on the desk (light themes)"), &prefs.paper)) last_theme = -1;
            cur.y += SI(44);
        } else {
            draw_text(f, S(12.5f), cur.x, cur.y + S(14), L("The text fills the whole screen width, with no sheet."), -1, col_alpha(T->panel_text, 0.55f));
            cur.y += SI(30);
        }
    } else if (settings_tab == 1) {
        ui_section(&cur, L("FOCUS MODE"));
        const char *fm[] = { L("Off"), L("Sentence"), L("Paragraph"), L("Line") };
        ui_segmented(rect(cur.x, cur.y, wd, SI(38)), fm, 4, &prefs.focus_mode);
        cur.y += SI(52);
        ui_section(&cur, L("EDITOR"));
        struct { const char *l; bool *v; bool relayout; } tg[] = {
            { L("Typewriter — current line in the center"), &prefs.typewriter, false },
            { L("Live Markdown (headings, bold, lists)"), &prefs.md_live, true },
            { L("Hide markup outside the current paragraph"), &prefs.hide_markup, true },
            { L("Smart typography:  --  →  —   \"\"  →  “”   ...  →  …"), &prefs.smart_typo, false },
            { L("Hemingway mode — deleting is not allowed"), &prefs.hemingway, false },
            { L("Hide the bars while writing"), &prefs.autohide, false },
            { L("Blinking cursor"), &prefs.caret_blink, false },
            { L("Smooth cursor movement"), &prefs.smooth_caret, false },
        };
        for (int i = 0; i < COUNT(tg); i++) {
            if (ui_toggle(rect(cur.x, cur.y, wd, SI(36)), tg[i].l, tg[i].v)) { if (tg[i].relayout) editor_invalidate(); prefs_save(); }
            cur.y += SI(42);
        }
        { bool tb = prefs.tabs;
          if (ui_toggle(rect(cur.x, cur.y, wd, SI(36)), L("Tabs — several open documents, remembered after a restart"), &tb)) c_tabs();
          cur.y += SI(42); }
        cur.y += SI(8);
        ui_section(&cur, L("GOALS AND SAVING"));
        float g = (float)prefs.goal; fmt(vt, sizeof vt, LN("%d word", "%d words", prefs.goal), prefs.goal);
        if (ui_slider(rect(cur.x, cur.y, wd, SI(52)), &g, 0, 5000, L("Session goal"), vt)) prefs.goal = ((int)g / 50) * 50;
        cur.y += SI(58);
        float sp = (float)prefs.sprint_min; fmt(vt, sizeof vt, L("%d min"), prefs.sprint_min);
        if (ui_slider(rect(cur.x, cur.y, wd, SI(52)), &sp, 5, 90, L("Sprint length"), vt)) prefs.sprint_min = ((int)(sp + 2.5f) / 5) * 5;
        cur.y += SI(58);
        float as = (float)prefs.autosave_s; fmt(vt, sizeof vt, L("every %d s"), prefs.autosave_s);
        if (ui_slider(rect(cur.x, cur.y, wd, SI(52)), &as, 5, 120, L("Autosave"), vt)) prefs.autosave_s = (int)as;
        cur.y += SI(62);
    } else {
        ui_section(&cur, L("LANGUAGE AND KEYBOARD"));
        {
            int li = i18n_current(), n = lang_count();
            int d = ed_stepper(rect(cur.x, cur.y, wd, SI(40)), L("Language"), lang_name(li), f);
            if (d && n > 1) app_set_language((li + d + n) % n, true);
            cur.y += SI(48);
            int kn = kbd_count(), ki = kbd_find(prefs.kb);
            char kname[80]; str_copy(kname, ki >= 0 ? kbd_name(ki) : prefs.kb, sizeof kname);
            d = ed_stepper(rect(cur.x, cur.y, wd, SI(40)), L("Keyboard"), kname, f);
            if (d && kn > 0) {
                ki = ki < 0 ? 0 : (ki + d + kn) % kn;
                str_copy(prefs.kb, kbd_code(ki), sizeof prefs.kb);
                kbd_select(prefs.kb);
                prefs_save();
            }
            cur.y += SI(46);
            char hint[200];
            fmt(hint, sizeof hint, L("Languages: %s · keyboards: %s (text files anyone can add)"), paths_show(DIR_LANGUAGES), paths_show(DIR_KEYBOARDS));
            draw_text_ellipsis(f, S(12.5f), cur.x, cur.y + S(12), wd, hint, col_alpha(T->panel_text, 0.5f));
            cur.y += SI(34);
        }
        ui_section(&cur, L("DISPLAY AND POINTER"));
        const char *sc[] = { L("Auto"), "100%", "125%", "150%", "200%" };
        float scales[] = { 0, 1.0f, 1.25f, 1.5f, 2.0f };
        int si = 0; for (int i = 0; i < 5; i++) if (f_abs(prefs.ui_scale - scales[i]) < 0.01f) si = i;
        if (ui_segmented(rect(cur.x, cur.y, wd, SI(38)), sc, 5, &si)) { prefs.ui_scale = scales[si]; prefs_save(); }
        cur.y += SI(52);
        const char *rt[] = { L("Rotate: auto"), "0°", "90°", "180°", "270°" };
        if (ui_segmented(rect(cur.x, cur.y, wd, SI(38)), rt, 5, &prefs.rotation)) { pal_set_rotation(prefs.rotation); prefs_save(); }
        cur.y += SI(46);
        {
            /* resolução: o botão percorre os modos que o monitor oferece (automática = nativa) */
            static int mws[48], mhs[48]; int nm = pal_video_modes(mws, mhs, 48);
            int cw = 0, chh = 0; pal_video_mode(&cw, &chh);
            char lab[96];
            if (prefs.video_mode[0]) fmt(lab, sizeof lab, L("Resolution: %dx%d"), cw, chh);
            else fmt(lab, sizeof lab, L("Resolution: automatic (%dx%d)"), cw, chh);
            if (ui_button(rect(cur.x, cur.y, wd - SI(160), SI(38)), lab, BTN_GHOST) && nm > 0) {
                int idx = -1;
                for (int i = 0; i < nm; i++) if (prefs.video_mode[0] && mws[i] == cw && mhs[i] == chh) idx = i;
                idx++;
                if (idx >= nm) prefs.video_mode[0] = 0;
                else fmt(prefs.video_mode, sizeof prefs.video_mode, "%dx%d", mws[idx], mhs[idx]);
                pal_set_video_mode(prefs.video_mode);
                prefs_save();
            }
            if (ui_button(rect(cur.x + wd - SI(150), cur.y, SI(150), SI(38)), L("Restore display"), BTN_GHOST)) app_video_reset();
            cur.y += SI(46);
            /* manter proporção: quando o modo de vídeo tem outra proporção que o painel e o monitor estica a imagem */
            const char *as[] = { L("Proportions: off"), L("Monitor"), "16:9", "16:10", "4:3" };
            if (ui_segmented(rect(cur.x, cur.y, wd, SI(38)), as, 5, &prefs.aspect)) { app_apply_aspect(); prefs_save(); }
            cur.y += SI(46);
            draw_text(f, S(12.5f), cur.x, cur.y + S(12), L("If circles look like ovals, choose the shape of your screen."), -1, col_alpha(T->panel_text, 0.5f));
            cur.y += SI(30);
            draw_text(f, S(12.5f), cur.x, cur.y + S(12), L("If the picture looks wrong or goes black: Ctrl+Alt+Home restores it; Ctrl+Alt+arrows rotate the screen."), -1, col_alpha(T->panel_text, 0.5f));
            cur.y += SI(34);
        }
        fmt(vt, sizeof vt, "%.1fx", (double)prefs.pointer_speed);
        if (ui_slider(rect(cur.x, cur.y, wd, SI(52)), &prefs.pointer_speed, 0.3f, 3.0f, L("Mouse / touchpad speed"), vt)) pal_set_pointer_speed(prefs.pointer_speed);
        cur.y += SI(64);
        const char *tm[] = { L("Automatic"), L("Touchpad"), L("Touch screen") };
        if (ui_segmented(rect(cur.x, cur.y, wd, SI(38)), tm, 3, &prefs.touch_mode)) { pal_set_touch_mode(prefs.touch_mode); prefs_save(); }
        cur.y += SI(46);
        draw_text(f, S(12.5f), cur.x, cur.y + S(12), L("If the pointer jumps to the corners when you use the touchpad, choose Touchpad."), -1, col_alpha(T->panel_text, 0.5f));
        cur.y += SI(34);
        { bool um = !prefs.no_mouse;
          if (ui_toggle(rect(cur.x, cur.y, wd, SI(36)), L("Use the mouse and touchpad (off = keyboard only)"), &um)) c_mouse();
          cur.y += SI(46); }
        ui_section(&cur, L("SOUND"));
        if (ui_toggle(rect(cur.x, cur.y, wd, SI(36)), L("Turn on the amplifier through GPIO (some laptops; applies after a restart)"), &prefs.audio_gpio)) prefs_save();
        cur.y += SI(46);
        if (ui_button(rect(cur.x, cur.y, SI(150), SI(38)), L("Test sound"), BTN_GHOST)) music_chime();
        if (ui_button(rect(cur.x + SI(160), cur.y, SI(200), SI(38)), L("Save diagnostics"), BTN_GHOST)) app_write_diag(true);
        cur.y += SI(46);
        draw_text(f, S(12.5f), cur.x, cur.y + S(12), pathfmt(L("The diagnostics go to %s on the USB drive (it opens on any computer)."), FILE_DIAG), -1, col_alpha(T->panel_text, 0.5f));
        cur.y += SI(34);
        ui_section(&cur, L("STARTUP"));
        if (ui_toggle(rect(cur.x, cur.y, wd, SI(36)), L("FolioOS splash screen"), &prefs.splash)) prefs_save();
        cur.y += SI(48);
        if (prefs.splash) {
            char vt[24]; fmt(vt, sizeof vt, "%d s", prefs.splash_secs);
            float sv = (float)prefs.splash_secs;
            if (ui_slider(rect(cur.x, cur.y, wd, SI(52)), &sv, 1, 60, L("Splash duration (any key skips it)"), vt)) {
                int ns = CLAMP((int)f_round(sv), 1, 60);
                if (ns != prefs.splash_secs) { prefs.splash_secs = ns; prefs_save(); }   /* grava só quando muda o segundo */
            }
            cur.y += SI(62);
        }
        if (app_installed()) {   /* instalado: onde ficam os textos (no computador, no pendrive, ou nos dois com a troca) */
            ui_section(&cur, L("TEXTS"));
            const char *dp[] = { L("This computer"), L("USB drive"), L("Both") };
            if (ui_segmented(rect(cur.x, cur.y, wd, SI(38)), dp, 3, &prefs.docs_place)) prefs_save();
            cur.y += SI(46);
            const char *hint = prefs.docs_place == DOCS_USB ? L("Your texts stay on the USB drive and go with you; without it, new texts are saved on this computer.")
                             : prefs.docs_place == DOCS_BOTH ? L("Your texts are on this computer; the button copies the newest version of each text in both directions. Nothing is deleted.")
                             : L("Your texts are on this computer.");
            draw_text_ellipsis(f, S(12.5f), cur.x, cur.y + S(12), wd, hint, col_alpha(T->panel_text, 0.5f));
            cur.y += SI(34);
            if (prefs.docs_place == DOCS_BOTH) {
                if (ui_button(rect(cur.x, cur.y, wd, SI(38)), L("Exchange texts with the USB drive"), BTN_GHOST)) app_exchange_texts();
                cur.y += SI(48);
            }
        }
        if (alongside) {   /* seletor de boot: quanto tempo o FolioOS espera uma tecla antes de iniciar o outro sistema */
            char vt[24]; fmt(vt, sizeof vt, "%d s", prefs.boot_wait);
            float bv = (float)prefs.boot_wait;
            if (ui_slider(rect(cur.x, cur.y, wd, SI(52)), &bv, 0, 30, L("Wait for a key before starting the other system"), vt)) {
                int nb = CLAMP((int)f_round(bv), 0, 30);
                if (nb != prefs.boot_wait) { prefs.boot_wait = nb; prefs_save(); }
            }
            cur.y += SI(56);
            draw_text(f, S(12.5f), cur.x, cur.y + S(12), prefs.boot_wait ? L("Without a key, the other system starts as usual.") : L("0 s: the computer always starts FolioOS."), -1, col_alpha(T->panel_text, 0.5f));
            cur.y += SI(34);
        }
        ui_section(&cur, L("ABOUT"));
        Surface *s = pal_screen();
        char l1[160], l2[160], l3[200];
        fmt(l1, sizeof l1, L("FolioOS %s · %s · %dx%d"), FOLIO_VERSION, pal_name(), s->w, s->h);
        fmt(l2, sizeof l2, L("Memory: %d MB used of %d MB · %d fonts · %d themes"), (int)(heap_used() >> 20), (int)(heap_total() >> 20), font_count(), theme_count());
        fmt(l3, sizeof l3, L("Audio: %s"), pal_audio_info());
        draw_text(f, S(13.5f), cur.x, cur.y + S(14), l1, -1, T->panel_text);
        draw_text(f, S(13.5f), cur.x, cur.y + S(36), l2, -1, col_alpha(T->panel_text, 0.7f));
        draw_text_ellipsis(f, S(13.5f), cur.x, cur.y + S(58), wd, l3, col_alpha(T->panel_text, 0.7f));
        cur.y += SI(80);
        if (ui_button(rect(cur.x, cur.y, wd, SI(38)), L("Install FolioOS on this computer…"), BTN_GHOST)) installer_open();
        cur.y += SI(48);
        if (ui_button(rect(cur.x, cur.y, wd, SI(38)), L("Disks and partitions…"), BTN_GHOST)) partitions_open();
        cur.y += SI(48);
        if (ui_button(rect(cur.x, cur.y, SI(150), SI(38)), L("Restart"), BTN_GHOST)) c_reboot();
        if (ui_button(rect(cur.x + SI(160), cur.y, SI(150), SI(38)), L("Shut down"), BTN_DANGER)) c_shutdown();
        cur.y += SI(56);
    }
    float content = cur.y + set_scroll - area.y;
    ui_reveal(&set_scroll, area);
    gfx_pop_clip();
    ui_scrollbar(rect(r.x + r.w - SI(8), area.y + SI(4), SI(5), area.h - SI(8)), &set_scroll, content, (float)area.h);
    if (ui.active == 0 && (ui.in.released || ui.nav_adj)) prefs_save();
}

/* ------------------------------------------------------------ editor de temas
 * Trabalha numa cópia (ed_th) que vira o tema ativo enquanto a janela está aberta:
 * cada mudança aparece na hora no texto, nas barras e na própria janela. */
static Theme ed_th;
static char  ed_name[48], ed_old[48], ed_hex[12];
static int   ed_ftext, ed_fui, ed_sel = 1, ed_copy_from;
static float ed_h, ed_s, ed_l, ed_scroll;
static bool  ed_saved;
static int   ed_prev_theme, ed_prev_font;
static const char *ed_labels[THEME_NCOLORS] = { N_("Desk (background)"), N_("Paper"), N_("Text"), N_("Faded text"), N_("Subtle lines"), N_("Accent"), N_("Selection"), N_("Cursor"),
    N_("Bars"), N_("Bar text"), N_("Borders"), N_("Panels"), N_("Panel text"), N_("Headings"), N_("Code background"), N_("Highlighter") };

static void ed_hex_from(Color c) { fmt(ed_hex, sizeof ed_hex, "#%02X%02X%02X", (int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255)); }
static void ed_sync_picker(void) {
    Color c = *theme_color(&ed_th, ed_sel);
    color_to_hsl(c, &ed_h, &ed_s, &ed_l);
    ed_hex_from(c);
}
static void ed_set_color(Color c, bool sync_hsl, bool sync_hex) {
    *theme_color(&ed_th, ed_sel) = c | 0xFF000000u;
    ed_th.dark = col_luma(ed_th.paper) < 0.5f;
    if (sync_hsl) color_to_hsl(c, &ed_h, &ed_s, &ed_l);
    if (sync_hex) ed_hex_from(c);
    last_theme = -1; ui.redraw = true;
}
static void theme_ed_closed(Window *w) {
    UNUSED(w);
    if (ui.sticky == &win_theme_ed) ui.sticky = 0;
    if (ed_saved) return;
    /* cancelado: volta tudo como estava */
    T = theme_get(prefs.theme);
    prefs.font = ed_prev_font;
    apply_theme_fonts(false);
    app_apply_font();
    last_theme = -1; ui.redraw = true;
}
static void open_theme_editor(bool edit) {
    if (win_theme_ed.open) { wm_raise(&win_theme_ed); return; }
    ed_prev_theme = prefs.theme; ed_prev_font = prefs.font;
    ed_th = *T;
    ed_th.custom = false; ed_th.font_text = ed_th.font_ui = 0;
    if (edit && T->custom) { str_copy(ed_name, T->name, sizeof ed_name); str_copy(ed_old, T->name, sizeof ed_old); }
    else {
        ed_old[0] = 0;
        str_copy(ed_name, L("My theme"), sizeof ed_name);
        for (int k = 2; theme_find(ed_name) >= 0 && k < 100; k++) fmt(ed_name, sizeof ed_name, L("My theme %d"), k);
    }
    ed_th.name = ed_name;
    ed_ftext = prefs.font;
    ed_fui = T->font_ui ? font_find(T->font_ui) : -1;
    ed_copy_from = MIN(prefs.theme, theme_builtin_count() - 1);
    ed_sel = 1; ed_sync_picker();
    ed_saved = false; ed_scroll = 0;
    T = &ed_th;
    wm_close(&win_settings);
    win_theme_ed.r.w = MIN(SI(720), ui.W - SI(40)); win_theme_ed.r.h = MIN(SI(760), ui.H - SI(80));
    win_theme_ed.min_w = MIN(SI(520), ui.W - SI(40)); win_theme_ed.min_h = MIN(SI(420), ui.H - SI(80));
    win_theme_ed.centered_once = false;
    win_theme_ed.on_close = theme_ed_closed;
    ui.focus = 0;
    wm_open(&win_theme_ed);
    ui.sticky = &win_theme_ed;
    last_theme = -1;
}
static void theme_ed_save(void) {
    int n = str_len(ed_name); while (n > 0 && ed_name[n - 1] == ' ') ed_name[--n] = 0;
    char *s = ed_name; while (*s == ' ') s++;
    if (s != ed_name) memmove(ed_name, s, (usize)str_len(s) + 1);
    if (!ed_name[0]) { ui_toast(L("Give the theme a name")); return; }
    int ex = theme_find(ed_name);
    if (ex >= 0 && ex < theme_builtin_count()) { ui_toast(L("That name belongs to a built-in theme — choose another")); return; }
    if (ex >= 0 && !str_ieq(ed_name, ed_old)) { ui_toast(L("You already have a theme with that name")); return; }
    ed_th.dark = col_luma(ed_th.paper) < 0.5f;
    int idx = theme_save_custom(&ed_th, ed_name, font_get(ed_ftext)->name, ed_fui >= 0 ? font_get(ed_fui)->name : "", ed_old);
    if (idx < 0) { ui_toast(pathfmt(L("Could not write to %s (is the USB drive write-protected or full?)"), DIR_THEMES)); return; }
    ed_saved = true;
    wm_close(&win_theme_ed);
    app_set_theme(idx);
    prefs_save();
    char m[120]; fmt(m, sizeof m, L("Theme “%s” saved to %s"), theme_get(idx)->name, paths_show(DIR_THEMES)); ui_toast(m);
}
static void theme_ed_delete_choice(int c) {
    if (c != 1) return;
    int i = theme_find(ed_old);
    if (i >= theme_builtin_count()) theme_delete_custom(i);
    ed_saved = true;
    wm_close(&win_theme_ed);
    prefs.font = ed_prev_font;
    app_set_theme(ed_prev_theme < theme_builtin_count() ? ed_prev_theme : 0);
    app_apply_font();
    prefs_save();
    ui_toast(L("Theme deleted"));
}
/* < nome > : percorre uma lista; devolve -1/+1 quando uma seta é clicada */
static int ed_stepper(Rect r, const char *label, const char *value, Face *vf) {
    Face *f = ui_font(false);
    draw_text(f, S(14), r.x, r.y + r.h * 0.5f + face_ascent(f, S(14)) * 0.36f, label, -1, T->panel_text);
    int bw = SI(36), vx = r.x + SI(150), vw = r.w - SI(150) - 2 * bw - SI(8);
    int d = 0;
    if (ui_nav(r, NAV_ADJ)) d = ui.nav_adj ? ui.nav_adj : ui.nav_fire ? 1 : 0;   /* teclado: ←/→, Enter avança */
    ui.nav_skip = true;
    if (ui_icon_btn(rect(vx, r.y, bw, r.h), IC_BACK, false, 0)) d = -1;
    Rect vr = rect(vx + bw + SI(4), r.y, vw, r.h);
    gfx_rrect(vr, S(8), col_alpha(T->text, 0.06f));
    gfx_push_clip(vr);
    float tw = text_width(vf, S(16), value, -1);
    draw_text(vf, S(16), vr.x + MAX(S(10), (vr.w - tw) * 0.5f), vr.y + vr.h * 0.5f + face_ascent(vf, S(16)) * 0.34f, value, -1, T->panel_text);
    gfx_pop_clip();
    if (ui_icon_btn(rect(vr.x + vr.w + SI(4), r.y, bw, r.h), IC_FWD, false, 0)) d = 1;
    ui.nav_skip = false;
    return d;
}
static void theme_ed_body(Window *w, Rect r) {
    UNUSED(w);
    Face *f = ui_font(false), *fb = ui_font(true);
    int pad = SI(22), x = r.x + pad, wd = r.w - 2 * pad;
    int foot = SI(64);
    Rect area = rect(r.x, r.y, r.w, r.h - foot);
    if (ui_hover(area) && ui.in.wheel) ed_scroll -= ui.in.wheel * SI(60);
    gfx_push_clip(area);
    Rect cur = rect(x, r.y + SI(12) - (int)ed_scroll, wd, 0);

    ui_section(&cur, L("THEME NAME"));
    ui_textfield(rect(x, cur.y, wd, SI(42)), ed_name, sizeof ed_name, L("Theme name"), false);
    cur.y += SI(54);

    ui_section(&cur, L("STARTING POINT"));
    {
        const Theme *src = theme_get(ed_copy_from);
        int d = ed_stepper(rect(x, cur.y, wd - SI(190), SI(38)), L("Colors from"), L(src->name), f);
        if (d) ed_copy_from = (ed_copy_from + d + theme_builtin_count()) % theme_builtin_count();
        if (ui_button(rect(x + wd - SI(180), cur.y, SI(180), SI(38)), L("Use all"), BTN_GHOST)) {
            const Theme *s2 = theme_get(ed_copy_from);
            for (int k = 0; k < THEME_NCOLORS; k++) *theme_color(&ed_th, k) = *theme_color((Theme *)s2, k);
            ed_th.paper_sheet = s2->paper_sheet; ed_th.grain = s2->grain; ed_th.dark = s2->dark;
            ed_sync_picker(); last_theme = -1;
        }
        cur.y += SI(46);
        draw_text(f, S(12), x, cur.y + S(12), L("Or mix: pick a color below and take it from any built-in theme."), -1, col_alpha(T->panel_text, 0.5f));
        cur.y += SI(30);
    }

    ui_section(&cur, L("COLORS"));
    {
        int cols = wd > SI(500) ? 2 : 1, rh = SI(36), gap = SI(12);
        int cw = (wd - (cols - 1) * gap) / cols, rows = (THEME_NCOLORS + cols - 1) / cols;
        for (int k = 0; k < THEME_NCOLORS; k++) {
            int c = k / rows, rr = k % rows;
            Rect it = rect(x + c * (cw + gap), cur.y + rr * rh, cw, rh - SI(4));
            bool fire = ui_item(it);
            bool hv = ui_hover(it), on = k == ed_sel;
            if (on) gfx_rrect(it, S(8), col_alpha(T->accent, 0.16f)); else if (hv) gfx_rrect(it, S(8), col_alpha(T->text, 0.05f));
            Rect sw = rect(it.x + SI(8), it.y + (it.h - SI(20)) / 2, SI(28), SI(20));
            gfx_rrect(sw, S(5), *theme_color(&ed_th, k));
            gfx_rrect_line(sw, S(5), S(1), col_alpha(T->panel_text, 0.25f));
            draw_text((on ? fb : f), S(13.5f), it.x + SI(46), it.y + it.h * 0.5f + face_ascent(f, S(13.5f)) * 0.36f, L(ed_labels[k]), -1, T->panel_text);
            if (fire && !on) { ed_sel = k; ed_sync_picker(); if (ui.in.pressed) ui.focus = 0; }
        }
        cur.y += rows * rh + SI(12);
    }

    /* seletor da cor escolhida */
    {
        Color cc = *theme_color(&ed_th, ed_sel);
        Rect big = rect(x, cur.y, SI(70), SI(70));
        gfx_rrect(big, S(10), cc);
        gfx_rrect_line(big, S(10), S(1), col_alpha(T->panel_text, 0.25f));
        draw_text(fb, S(15), x + SI(86), cur.y + S(22), L(ed_labels[ed_sel]), -1, T->panel_text);
        if (ui_textfield(rect(x + SI(86), cur.y + SI(32), SI(140), SI(38)), ed_hex, sizeof ed_hex, "#RRGGBB", false)) {
            Color nc; if (color_parse(ed_hex, &nc)) ed_set_color(nc, true, false);
        }
        draw_text(f, S(12), x + SI(240), cur.y + SI(56), L("hex code"), -1, col_alpha(T->panel_text, 0.45f));
        cur.y += SI(84);
        char vt[32];
        fmt(vt, sizeof vt, "%d°", (int)ed_h);
        bool ch = ui_slider(rect(x, cur.y, wd, SI(52)), &ed_h, 0, 359, L("Hue"), vt);
        cur.y += SI(56);
        fmt(vt, sizeof vt, "%d%%", (int)(ed_s * 100 + 0.5f));
        ch |= ui_slider(rect(x, cur.y, wd, SI(52)), &ed_s, 0, 1, L("Saturation"), vt);
        cur.y += SI(56);
        fmt(vt, sizeof vt, "%d%%", (int)(ed_l * 100 + 0.5f));
        ch |= ui_slider(rect(x, cur.y, wd, SI(52)), &ed_l, 0, 1, L("Lightness"), vt);
        cur.y += SI(60);
        if (ch) { ui.focus = 0; ed_set_color(color_from_hsl(ed_h, ed_s, ed_l), false, true); }
        /* a mesma cor em cada tema pronto: um clique e ela vem para o seu */
        draw_text(f, S(12.5f), x, cur.y + S(12), L("Take from a built-in theme:"), -1, col_alpha(T->panel_text, 0.6f));
        cur.y += SI(22);
        int d = SI(26), gx = x, n = 0;
        Color seen[64];
        for (int t = 0; t < theme_count(); t++) {
            const Theme *th = theme_get(t);
            Color c = *theme_color((Theme *)th, ed_sel);
            bool dup = false; for (int q = 0; q < n; q++) if (seen[q] == c) dup = true;
            if (dup || n >= 64) continue;
            seen[n++] = c;
            if (gx + d > x + wd) { gx = x; cur.y += d + SI(8); }
            Rect sr = rect(gx, cur.y, d, d);
            bool fire = ui_item(sr);
            bool hv = ui_hover(sr);
            gfx_circle(sr.x + d * 0.5f, sr.y + d * 0.5f, d * 0.5f, col_alpha(T->panel_text, hv ? 0.6f : 0.2f));
            gfx_circle(sr.x + d * 0.5f, sr.y + d * 0.5f, d * 0.5f - S(1.5f), c);
            ui_tooltip(th, sr, L(th->name));
            if (fire) { if (ui.in.pressed) ui.focus = 0; ed_set_color(c, true, true); }
            gx += d + SI(8);
        }
        cur.y += d + SI(18);
    }

    ui_section(&cur, L("THEME FONTS"));
    {
        int d = ed_stepper(rect(x, cur.y, wd, SI(40)), L("Text"), font_get(ed_ftext)->name, fam_face(font_get(ed_ftext), 0));
        if (d) { ed_ftext = (ed_ftext + d + font_count()) % font_count(); prefs.font = ed_ftext; app_apply_font(); }
        cur.y += SI(48);
        char dn[80];
        if (ed_fui >= 0) str_copy(dn, font_get(ed_fui)->name, sizeof dn);
        else { font_set_ui(-1); fmt(dn, sizeof dn, L("Default (%s)"), font_ui()->name); }
        d = ed_stepper(rect(x, cur.y, wd, SI(40)), L("Interface"), dn, ed_fui >= 0 ? fam_face(font_get(ed_fui), 0) : f);
        if (d) {
            ed_fui += d;
            if (ed_fui < -1) ed_fui = font_count() - 1;
            if (ed_fui >= font_count()) ed_fui = -1;
            font_set_ui(ed_fui); app_apply_font();
        }
        cur.y += SI(48);
        draw_text(f, S(12), x, cur.y + S(12), L("Interface = bars, menus and windows."), -1, col_alpha(T->panel_text, 0.45f));
        cur.y += SI(32);
    }

    ui_section(&cur, L("DESK AND PAPER"));
    if (ui_toggle(rect(x, cur.y, wd, SI(36)), L("Paper sheet on the desk"), &ed_th.paper_sheet)) last_theme = -1;
    cur.y += SI(44);
    {
        char vt[24]; fmt(vt, sizeof vt, "%d%%", (int)(ed_th.grain * 200 + 0.5f));
        if (ui_slider(rect(x, cur.y, wd, SI(52)), &ed_th.grain, 0, 0.5f, L("Vignette at the desk edges"), vt)) last_theme = -1;
        cur.y += SI(64);
    }
    float content = cur.y + ed_scroll - r.y;
    ui_reveal(&ed_scroll, area);
    gfx_pop_clip();
    ui_scrollbar(rect(r.x + r.w - SI(8), r.y + SI(4), SI(5), area.h - SI(8)), &ed_scroll, content, (float)area.h);

    /* rodapé fixo */
    int by = r.y + r.h - foot + SI(12);
    gfx_rect(rect(r.x, by - SI(12), r.w, 1), col_alpha(T->border, 0.7f));
    if (ui_button(rect(x + wd - SI(170), by, SI(170), SI(40)), L("Save theme"), BTN_PRIMARY)) { theme_ed_save(); return; }
    if (ui_button(rect(x + wd - SI(300), by, SI(120), SI(40)), L("Cancel"), BTN_GHOST)) { wm_close(&win_theme_ed); return; }
    if (ed_old[0] && ui_button(rect(x, by, SI(120), SI(40)), L("Delete"), BTN_DANGER))
        dialog_confirm(L("Delete theme?"), pathfmt(L("The theme file in %s will be deleted."), DIR_THEMES), L("Delete"), 0, theme_ed_delete_choice);
}

/* ------------------------------------------------------------ localizar */
static void find_body(Window *w, Rect r) {
    UNUSED(w);
    int x = r.x + SI(18), wd = r.w - SI(36);
    if (ui_textfield(rect(x, r.y + SI(16), wd - SI(96), SI(40)), find_text, sizeof find_text, L("Find"), false)) editor_find_highlight(find_text);
    if (ui_icon_btn(rect(x + wd - SI(88), r.y + SI(16), SI(40), SI(40)), IC_UP, false, L("Previous (Shift+Enter)"))) editor_find_next(find_text, true);
    if (ui_icon_btn(rect(x + wd - SI(42), r.y + SI(16), SI(40), SI(40)), IC_CHEVRON, false, L("Next (Enter)"))) editor_find_next(find_text, false);
    ui_textfield(rect(x, r.y + SI(66), wd, SI(40)), repl_text, sizeof repl_text, L("Replace with"), false);
    int by = r.y + SI(120);
    if (ui_button(rect(x, by, SI(150), SI(38)), L("Replace"), BTN_GHOST)) editor_replace_one(find_text, repl_text);
    if (ui_button(rect(x + SI(160), by, SI(170), SI(38)), L("Replace all"), BTN_GHOST)) {
        int n = editor_replace_all(find_text, repl_text);
        char m[80]; fmt(m, sizeof m, LN("%d replacement", "%d replacements", n), n); ui_toast(m);
    }
    Event e;
    for (int i = 0; i < ui.in.nkeys; i++) {
        Event *k = &ui.in.keys[i];
        if (k->type == EV_KEY && k->key == KEY_ENTER && ui.key_win == &win_find) {
            e = *k; k->type = EV_NONE;
            if (!editor_find_next(find_text, (e.mods & MOD_SHIFT) != 0)) ui_toast(L("Nothing found"));
            editor_find_highlight(find_text);
        }
    }
    if (find_text[0]) {
        int total = 0;
        for (int p = 0; p < app.doc.n; p++) { int from = 0; Para *q = &app.doc.p[p]; while (true) { int fnd = str_find_i(q->s, q->len, find_text, str_len(find_text), from); if (fnd < 0) break; total++; from = utf8_next(q->s, q->len, fnd); } }
        char m[64]; fmt(m, sizeof m, LN("%d match", "%d matches", total), total);
        Face *f = ui_font(false);
        draw_text(f, S(13), x + wd - text_width(f, S(13), m, -1), by + SI(25), m, -1, col_alpha(T->panel_text, 0.5f));
    }
}

/* ------------------------------------------------------------ sumário */
static float out_scroll;
static void outline_body(Window *w, Rect r) {
    UNUSED(w);
    Face *f = ui_font(false), *fb = ui_font(true);
    int y = r.y + SI(8) - (int)out_scroll, rh = SI(34), count = 0;
    bool in_code = false, tex = app.syntax == SYN_TEX;   /* fontes LaTeX: sem capítulos por # */
    int cur_h = -1, here = preview_active() ? preview_top_para() : app.caret.p;
    for (int i = 0; i < app.doc.n && i <= here && !tex; i++) {
        LineKind k = md_line(app.doc.p[i].s, app.doc.p[i].len, in_code);
        if (k.kind == LK_FENCE) in_code = !in_code;
        if (k.kind == LK_H) cur_h = i;
    }
    in_code = false;
    if (ui_hover(r) && ui.in.wheel) out_scroll -= ui.in.wheel * rh * 3;
    for (int i = 0; i < app.doc.n && !tex; i++) {
        Para *p = &app.doc.p[i];
        LineKind k = md_line(p->s, p->len, in_code);
        if (k.kind == LK_FENCE) in_code = !in_code;
        if (k.kind != LK_H) continue;
        count++;
        Rect it = rect(r.x + SI(8), y, r.w - SI(16), rh);
        bool vis = y + rh > r.y && y < r.y + r.h;   /* fora da vista: só o teclado chega nele */
        bool fire = vis ? ui_item(it) : (ui_nav(it, NAV_ITEM) && ui.nav_fire), kb = fire && !ui.in.pressed;
        if (fire) { if (preview_active()) preview_goto_para(i, true); else editor_goto((Pos){ i, p->len }); }
        if (kb) wm_focus(0);
        if (y + rh > r.y && y < r.y + r.h) {
            bool hv = ui_hover(it), on = i == cur_h;
            if (on) gfx_rrect(it, S(7), col_alpha(T->accent, 0.14f)); else if (hv) gfx_rrect(it, S(7), col_alpha(T->text, 0.05f));
            float ix = it.x + S(12) + (k.level - 1) * S(16);
            char t[200]; int pl; char *pt = md_plain(p->s + k.prefix, p->len - k.prefix, &pl);
            str_copy(t, pt, MIN((int)sizeof t, pl + 1)); mem_free(pt);
            if (!t[0]) str_copy(t, L("(untitled)"), sizeof t);
            draw_text_ellipsis(k.level == 1 ? fb : f, S(k.level == 1 ? 14.5f : 13.5f), ix, it.y + it.h * 0.5f + S(5), it.x + it.w - ix - S(6), t, on ? T->accent : T->panel_text);
        }
        y += rh;
    }
    float content = (float)count * rh + SI(16);
    ui_reveal(&out_scroll, r);
    out_scroll = CLAMP(out_scroll, 0.0f, MAX(0.0f, content - r.h));
    if (!count) {
        draw_text_c(f, S(14), rect(r.x, r.y + SI(30), r.w, SI(24)), L("No headings yet."), col_alpha(T->panel_text, 0.55f));
        if (!tex) draw_text_c(f, S(13), rect(r.x, r.y + SI(56), r.w, SI(24)), L("Start a line with # to create chapters."), col_alpha(T->panel_text, 0.45f));
    }
}

/* ------------------------------------------------------------ sessão e metas */
static void ring(float cx, float cy, float rad, float th, float frac, Color bg, Color fg) {
    gfx_ring(cx, cy, rad, th, bg);
    gfx_arc(cx, cy, rad, th, frac, fg);
}

static int sel_words(void) {
    if (pos_cmp(app.caret, app.anchor) == 0 || iabs(app.caret.p - app.anchor.p) > 400) return 0;
    int n; char *s = doc_slice(&app.doc, app.caret, app.anchor, &n);
    int w = 0; bool in = false;
    for (int i = 0; i < n; i++) { bool sp = s[i] == ' ' || s[i] == '\n' || s[i] == '\t'; if (!sp && !in) w++; in = !sp; }
    mem_free(s);
    return w;
}

static void stats_body(Window *w, Rect r) {
    Face *f = ui_font(false), *fb = ui_font(true);
    int today = app_words_today(), words = doc_words(&app.doc), sw = session_words();
    float cx = r.x + r.w * 0.5f, cy = r.y + S(104);
    float frac = prefs.goal ? (float)today / prefs.goal : 0;
    Color ok = 0xFF3FAE6A;
    ring(cx, cy, S(74), S(9), frac, col_alpha(T->text, 0.07f), frac >= 1 ? ok : T->accent);
    char big[32]; fmt(big, sizeof big, "%d", today);
    draw_text_c(fb, S(36), rect(r.x, (int)(cy - S(34)), r.w, SI(46)), big, T->panel_text);
    char g[64];
    if (prefs.goal) fmt(g, sizeof g, frac >= 1 ? L("goal of %d reached") : L("of %d words today"), prefs.goal);
    else str_copy(g, L("words today"), sizeof g);
    draw_text_c(f, S(13), rect(r.x, (int)(cy + S(12)), r.w, SI(20)), g, frac >= 1 ? ok : col_alpha(T->panel_text, 0.55f));
    int y = (int)(cy + S(98));
    int mins = (int)((ui.in.now - app.session_start_ms) / 60000);
    struct { const char *k; char v[48]; } rows[4];
    rows[0].k = L("This document"); fmt(rows[0].v, 48, LN("%d word", "%d words", words), words);
    rows[1].k = L("This session"); fmt(rows[1].v, 48, L("+%d in %dh%02d"), sw, mins / 60, mins % 60);
    rows[2].k = L("Estimated reading"); fmt(rows[2].v, 48, L("%d min"), MAX(1, (words + 199) / 200));
    rows[3].k = L("Pace"); fmt(rows[3].v, 48, L("%d words/h"), mins > 0 ? sw * 60 / mins : 0);
    for (int i = 0; i < 4; i++) {
        draw_text(f, S(14), r.x + S(28), y + S(17), rows[i].k, -1, col_alpha(T->panel_text, 0.6f));
        float vw = text_width(fb, S(14), rows[i].v, -1);
        draw_text(fb, S(14), r.x + r.w - S(28) - vw, y + S(17), rows[i].v, -1, T->panel_text);
        y += SI(31);
    }
    y += SI(14);
    gfx_rect(rect(r.x + SI(24), y, r.w - SI(48), 1), col_alpha(T->border, 0.7f));
    y += SI(16);
    if (!sprint_end) {
        const char *dur[] = { L("15 min"), L("25 min"), L("45 min"), L("60 min") };
        int vals[] = { 15, 25, 45, 60 };
        int si = 1; for (int i = 0; i < 4; i++) if (prefs.sprint_min == vals[i]) si = i;
        if (ui_segmented(rect(r.x + SI(24), y, r.w - SI(48), SI(36)), dur, 4, &si)) { prefs.sprint_min = vals[si]; prefs_save(); }
        y += SI(46);
        if (ui_button(rect(r.x + SI(24), y, r.w - SI(48), SI(42)), L("Start sprint"), BTN_PRIMARY)) c_sprint();
    } else {
        int rem = (int)((sprint_end - MIN(sprint_end, ui.in.now)) / 1000);
        char t[32]; fmt(t, sizeof t, "%d:%02d", rem / 60, rem % 60);
        draw_text_c(fb, S(30), rect(r.x, y, r.w, SI(40)), t, T->accent);
        int done = MAX(0, doc_words(&app.doc) - (int)sprint_start_words);
        char d[48]; fmt(d, sizeof d, LN("+%d word in this sprint", "+%d words in this sprint", done), done);
        draw_text_c(f, S(13), rect(r.x, y + SI(36), r.w, SI(20)), d, col_alpha(T->panel_text, 0.55f));
        y += SI(64);
        if (ui_button(rect(r.x + SI(24), y, r.w - SI(48), SI(42)), L("Stop sprint"), BTN_GHOST)) c_sprint();
        ui.redraw = true;
    }
    y += SI(42) + SI(20);
    int want_h = (y - r.y) + SI(46);
    if (w->r.h != want_h) { w->r.h = want_h; ui.redraw = true; }
}

/* ------------------------------------------------------------ ajuda */
static void help_body(Window *w, Rect r) {
    UNUSED(w);
    static const char *keys[][2] = {
        { N_("Ctrl+N / O / S"), N_("New · Open · Save") }, { N_("Ctrl+Shift+S"), N_("Save as / Export") },
        { N_("Ctrl+W · Ctrl+Tab"), N_("Tabs: close · next") }, { N_("Alt+1…9"), N_("Go to tab 1…9") },
        { N_("Ctrl+K"), N_("Command palette") }, { N_("Ctrl+P"), N_("Reading (formatted Markdown)") },
        { N_("Ctrl+F"), N_("Find and replace") }, { N_("Ctrl+Z / Y"), N_("Undo · Redo") },
        { N_("Ctrl+B / I"), N_("Bold · Italic") }, { N_("Ctrl+1…6 / 0"), N_("Heading level 1–6 · Text") },
        { N_("Alt+↑ / ↓"), N_("Move paragraph") }, { N_("Tab / Shift+Tab"), N_("Indent list item") },
        { N_("Ctrl+D"), N_("Duplicate paragraph") }, { N_("Ctrl+Shift+K"), N_("Delete paragraph") },
        { N_("Ctrl+PgUp / PgDn"), N_("Previous · next chapter") }, { N_("Ctrl+J"), N_("Jump to and back from the last edit") },
        { N_("Shift+F3"), N_("lowercase → Title Case → UPPERCASE") }, { N_("Select and type ( \" *"), N_("Wraps the selection") },
        { N_("← / → · Space (reading)"), N_("Previous · next page") }, { N_("Enter (reading)"), N_("Auto-scroll on/off") },
        { N_("+ / − (reading)"), N_("Auto-scroll speed") }, { N_("Alt+M"), N_("Menu and recent documents") },
        { N_("Ctrl+← / →"), N_("Previous / next word") }, { N_("Ctrl+Home / End"), N_("Start · end of the text") },
        { N_("Ctrl+= / -"), N_("Larger · smaller text") }, { N_("Ctrl+T"), N_("Next theme") },
        { "F1", N_("This help") }, { "F2", N_("Focus mode (sentence/paragraph/line)") },
        { "F3", N_("Typewriter") }, { "F4", N_("Writing sprint") },
        { "F5", N_("Outline") }, { "F6", N_("Music player") },
        { N_("F7 / F8 / F9"), N_("Previous track · Play/pause · Next") }, { "F10", N_("Session and goals") },
        { "F11", N_("Zen mode") }, { "F12", N_("Appearance") },
        { N_("Esc"), N_("Close window · leave zen/reading") }, { N_("Ctrl+Q"), N_("Shut down") },
        { N_("Tab · ↑ ↓ (windows)"), N_("Move between buttons and options") }, { N_("Enter · Space · ← →"), N_("Press · adjust the focused option") },
        { N_("Ctrl+F6"), N_("Keyboard between the text and the windows") }, { N_("Ctrl+Shift+← / →"), N_("Select word by word") },
        { "-- ...", N_("Become — and … (smart typography)") }, { N_("Alt+F7 / Alt+F8"), N_("Move · resize the window (arrows, Enter)") },
        { N_("Ctrl+Space"), N_("Input method on/off (Korean, Japanese)") },
    };
    Face *f = ui_font(false), *fb = ui_font(true);
    int cols = r.w > SI(640) ? 2 : 1;
    int per = (COUNT(keys) + cols - 1) / cols;
    int cw = (r.w - SI(40)) / cols, rh = MIN(SI(32), MAX(SI(24), (r.h - SI(20)) / per));
    for (int i = 0; i < COUNT(keys); i++) {
        int c = i / per, row = i % per;
        int x = r.x + SI(20) + c * cw, y = r.y + SI(14) + row * rh;
        float kw = text_width(fb, S(12.5f), L(keys[i][0]), -1);
        Rect kr = rect(x, y + SI(4), (int)(kw + S(16)), rh - SI(8));
        gfx_rrect(kr, S(5), col_alpha(T->text, 0.08f));
        draw_text_c(fb, S(12.5f), kr, L(keys[i][0]), T->panel_text);
        draw_text_ellipsis(f, S(13.5f), x + kr.w + S(12), y + rh * 0.5f + S(5), cw - kr.w - S(20), L(keys[i][1]), col_alpha(T->panel_text, 0.75f));
    }
}

/* ======================================================================
 * barras (dissolvem-se sobre a página; só o essencial)
 * ====================================================================== */
static Color chrome_bg(void) { return (app_sheet_on()) ? T->desk : T->paper; }
extern Rect editor_sheet_rect(void);
/* degradê das barras: cor da mesa fora da folha, cor do papel sobre a folha */
static void chrome_fade(Rect band, bool top) {
    Color bg = chrome_bg();
    float a0 = top ? 0.97f : 0.0f, a1 = top ? 0.0f : 0.97f;
    Rect sh = editor_sheet_rect();
    if (sh.w <= 0) { gfx_gradient_v(band, col_alpha(bg, a0), col_alpha(bg, a1)); return; }
    gfx_push_clip(rect(band.x, band.y, sh.x - band.x, band.h)); gfx_gradient_v(band, col_alpha(bg, a0), col_alpha(bg, a1)); gfx_pop_clip();
    gfx_push_clip(rect(sh.x + sh.w, band.y, band.x + band.w - sh.x - sh.w, band.h)); gfx_gradient_v(band, col_alpha(bg, a0), col_alpha(bg, a1)); gfx_pop_clip();
    {
        Rect s = rect_clip(band, rect(sh.x, band.y, sh.w, band.h));
        gfx_push_clip(s);
        gfx_gradient_v(band, col_alpha(T->paper, a0), col_alpha(T->paper, a1));
        gfx_pop_clip();
    }
}


/* abas (estilo Sublime): ocupam o lugar do título na barra superior, entre o menu e os ícones
 * da direita, e somem junto com ela. Muitas abas encolhem (o título vira reticências). */
static void draw_tabbar(int x0, int right, int y, int h) {
    int plus = SI(32), avail = right - x0 - plus - SI(4);
    if (avail < SI(40)) return;
    int tw = CLAMP(avail / MAX(1, ntabs), SI(44), SI(220));
    Face *f = ui_font(false), *fb = ui_font(true);
    float px = S(13);
    int click = -1, close = -1, shown = 0;
    for (int i = 0; i < ntabs; i++) {
        AppDoc *d = tab_at(i);
        bool on = i == cur_tab;
        Rect r = rect(x0 + i * tw, y, tw - SI(4), h);
        if (r.x + r.w > x0 + avail) {
            if (!on) continue;
            r.x = x0 + avail - r.w;   /* a aba atual nunca some: ocupa o último lugar visível */
        }
        shown = i + 1;
        bool hv = ui_hover(r);
        if (on) {
            gfx_rrect(r, S(7), col_alpha(T->chrome_text, 0.10f));
            gfx_rrect(rect(r.x + SI(10), r.y + r.h - SI(3), r.w - SI(20), SI(2)), S(1), T->accent);
        } else if (hv) gfx_rrect(r, S(7), col_alpha(T->chrome_text, 0.06f));
        if (i > 0 && !on && i - 1 != cur_tab && !hv)   /* separador fino entre abas inativas */
            gfx_rect(rect(r.x - SI(2), r.y + SI(10), 1, r.h - SI(20)), col_alpha(T->chrome_text, 0.15f));
        Rect xr = rect(r.x + r.w - SI(26), r.y + (r.h - SI(20)) / 2, SI(20), SI(20));
        const char *t = display_title(d);
        Color tc = on ? T->chrome_text : col_alpha(T->chrome_text, 0.6f);
        draw_text_ellipsis(on ? fb : f, px, r.x + S(12), r.y + r.h * 0.5f + face_ascent(f, px) * 0.36f, r.w - S(42), t, tc);
        bool xh = ui_hover(xr);
        if (hv) {
            if (xh) gfx_rrect(xr, S(5), col_alpha(T->chrome_text, 0.14f));
            icon_draw(IC_CLOSE, xr.x + xr.w * 0.5f, xr.y + xr.h * 0.5f, S(12), col_alpha(T->chrome_text, xh ? 0.9f : 0.5f));
        } else if (d->doc.modified) gfx_circle(xr.x + xr.w * 0.5f, xr.y + xr.h * 0.5f, S(3.5f), on ? col_alpha(T->accent, 0.9f) : col_alpha(T->chrome_text, 0.45f));
        else if (on && d->has_path) icon_draw(IC_CHECK, xr.x + xr.w * 0.5f, xr.y + xr.h * 0.5f, S(12), col_alpha(T->chrome_text, 0.35f));
        if (hv) {
            ui.cursor = 2;
            ui_tooltip(d, r, d->has_path ? d->path : pathfmt(L("Untitled — kept in %s until you save it"), DIR_TABS));
            if (ui.in.pressed) { if (xh) close = i; else click = i; }
            if (ui.in.mpressed) { close = i; ui.in.mpressed = false; }   /* botão do meio fecha, como nos navegadores */
        }
    }
    (void)shown;
    int px0 = x0 + MIN(ntabs * tw, avail) + SI(2);
    if (ui_icon_btn(rect(px0, y + (h - plus) / 2, plus, plus), IC_PLUS, false, L("New tab (Ctrl+N)"))) app_tab_new();
    if (close >= 0) app_tab_close(close);
    else if (click >= 0) app_tab_switch(click);
}

/* título centralizado + estado de salvamento (sem abas) */
static void draw_title(int y, int h, int bs) {
    Face *f = ui_font(false);
    float px = S(14);
    const char *title = display_title(&app);
    float tw = MIN(text_width(f, px, title, -1), ui.W * 0.4f);
    float tx = (ui.W - tw) * 0.5f, base = h * 0.5f + face_ascent(f, px) * 0.36f;
    draw_text_ellipsis(f, px, tx, base, ui.W * 0.4f, title, col_alpha(T->chrome_text, 0.8f));
    float sx = tx + tw + S(10);
    if (app.doc.modified) gfx_circle(sx + S(3), h * 0.5f, S(3), col_alpha(T->accent, 0.9f));
    else if (app.has_path) icon_draw(IC_CHECK, sx + S(5), h * 0.5f, S(13), col_alpha(T->chrome_text, 0.35f));
    Rect th = rect((int)tx - SI(8), y, (int)tw + SI(30), bs);
    if (ui_hover(th)) {
        ui.cursor = 2;
        ui_tooltip(&app, th, app.has_path ? app.path : L("Not saved yet (Ctrl+S)"));
        if (ui.in.pressed) c_open();
    }
}

static void draw_topbar(float a) {
    if (a <= 0.01f) return;
    int h = SI(56);
    gfx_set_opacity(a);
    chrome_fade(rect(0, 0, ui.W, h + SI(18)), true);
    int bs = SI(36), y = (h - bs) / 2;
    if (ui_icon_btn(rect(SI(12), y, bs, bs), IC_MENU, win_menu.open, L("Menu"))) c_menu();
    /* direita: modos de escrita, música e aparência (o foco por frase/parágrafo fica no F2 e na paleta) */
    char mtip[200];
    const char *playing_now = music_now();
    if (music_playing() && playing_now[0]) fmt(mtip, sizeof mtip, L("Playing: %s (F6)"), playing_now);
    else str_copy(mtip, music_available() ? L("Music (F6)") : pathfmt(L("Music (F6) — put MP3 files in the %s folder"), DIR_MUSIC), sizeof mtip);
    struct { int ic; const char *tip; void (*fn)(void); bool on; } btns[] = {
        { IC_TABS, prefs.tabs ? L("Tabs on — click to turn off") : L("Tabs: open several documents (like in Sublime Text)"), c_tabs, prefs.tabs },
        { IC_MOON, L("Zen mode (F11)"), c_zen, zen },
        { IC_TYPEWRITER, L("Typewriter (F3)"), c_typewriter, prefs.typewriter },
        { IC_EYE, L("Reading (Ctrl+P)"), c_preview, preview_active() },
        { IC_MUSIC, mtip, c_music, win_music.open || music_playing() },
        { IC_PALETTE, L("Appearance (F12)"), c_appearance, win_settings.open },
        { IC_COMMAND, L("Commands (Ctrl+K)"), c_palette, win_palette.open },
    };
    int x = ui.W - SI(12) - bs;
    for (int i = COUNT(btns) - 1; i >= 0; i--) {
        Rect br = rect(x, y, bs, bs);
        if (ui_icon_btn(br, btns[i].ic, btns[i].on, btns[i].tip)) btns[i].fn();
        if (btns[i].ic == IC_MUSIC && music_playing()) gfx_circle(br.x + br.w - S(8), br.y + S(8), S(2.5f), T->accent);   /* tocando */
        x -= bs + SI(2);
        /* com uma faixa carregada (tocando ou pausada): anterior · tocar/pausar · próxima, ao lado da clave */
        if (btns[i].ic == IC_MUSIC && music_now()[0]) {
            int cw = (int)(bs * 0.86f);
            x += bs - cw;
            if (ui_icon_btn(rect(x, y, cw, bs), IC_NEXT, false, L("Next track (F9)"))) c_next();
            x -= cw;
            if (ui_icon_btn(rect(x, y, cw, bs), music_playing() ? IC_PAUSE : IC_PLAY, false, music_playing() ? L("Pause (F8)") : L("Play (F8)"))) c_play();
            x -= cw;
            if (ui_icon_btn(rect(x, y, cw, bs), IC_PREV, false, L("Previous track (F7)"))) c_prev();
            x -= bs + SI(8);
        }
    }
    /* centro: as abas (quando ligadas) ocupam o espaço do título, do menu até o primeiro ícone */
    if (prefs.tabs && ntabs > 0) draw_tabbar(SI(12) + bs + SI(12), x + bs + SI(2) - SI(12), y, bs);
    else draw_title(y, h, bs);
    gfx_set_opacity(1);
}

static void read_time(char *o, int cap, int words) {
    /* na leitura, no ritmo escolhido na barra (o mesmo da rolagem automática); fora dela, 200 pal/min */
    int m = MAX(1, (words + prefs.read_wpm - 1) / prefs.read_wpm);
    if (m < 60) fmt(o, cap, L("%d min"), m); else fmt(o, cap, L("%d h %02d min"), m / 60, m % 60);
}
static void draw_statusbar(float a) {
    if (a <= 0.01f) return;
    int h = SI(44), y0 = ui.H - h;
    gfx_set_opacity(a);
    chrome_fade(rect(0, y0 - SI(18), ui.W, h + SI(18)), false);
    Face *f = ui_font(false), *fb = ui_font(true);
    float px = S(13);
    float cy = y0 + h * 0.5f + SI(2);
    float base = cy + face_ascent(f, px) * 0.36f;
    Color c = col_alpha(T->chrome_text, 0.62f);
    int words = doc_words(&app.doc), today = app_words_today(), sel = sel_words();
    char s[160];
    float x = S(20);
    if (prefs.goal) {
        float frac = (float)today / prefs.goal;
        ring(x + S(7), cy, S(7.5f), S(2.2f), frac, col_alpha(T->chrome_text, 0.14f), frac >= 1 ? 0xFF3FAE6A : T->accent);
        x += S(22);
    }
    char chap[120]; int pct = 0, chap_left = 0, left = 0;
    if (preview_active() && preview_info(chap, sizeof chap, &pct, &chap_left, &left)) {
        /* leitura: quanto já foi lido e quanto falta (no ritmo escolhido na barra de leitura) */
        char ta[32], tb[32];
        read_time(ta, sizeof ta, chap_left); read_time(tb, sizeof tb, left);
        fmt(s, sizeof s, "%d%%", pct);
        float x3 = draw_text(fb, px, x, base, s, -1, col_alpha(T->chrome_text, 0.85f));
        char rest[300];
        if (!left) fmt(rest, sizeof rest, L(" · end of the text"));
        else if (chap[0] && chap_left < left) fmt(rest, sizeof rest, L(" · %s · %s to the end of the chapter · %s in total"), chap, ta, tb);
        else if (chap[0]) fmt(rest, sizeof rest, L(" · %s · %s of reading left"), chap, tb);
        else fmt(rest, sizeof rest, L(" · %s of reading left"), tb);
        draw_text_ellipsis(f, px, x3, base, MIN(ui.W * 0.62f, preview_bar_rect().x - S(16)) - x3, rest, c);
    } else {
        if (sel) fmt(s, sizeof s, LN("%d of %d word", "%d of %d words", words), sel, words);
        else fmt(s, sizeof s, LN("%d word", "%d words", words), words);
        float x2 = draw_text(fb, px, x, base, s, -1, col_alpha(T->chrome_text, 0.8f));
        Rect wr = rect(0, y0, (int)x2 + SI(8), h);
        if (ui_hover(wr)) {
            static char tip[120];
            fmt(tip, sizeof tip, L("Today: %d words · %d min reading"), today, MAX(1, (words + 199) / 200));
            ui_tooltip(&prefs, wr, tip);
            ui.cursor = 2;
            if (ui.in.pressed) c_stats();
        }
    }
    /* direita: sprint, tecla morta pendente, relógio */
    DateTime t; pal_now(&t);
    fmt(s, sizeof s, "%02d:%02d", t.hour, t.min);
    float rx = ui.W - S(20) - text_width(f, px, s, -1);
    draw_text(f, px, rx, base, s, -1, c);
    if (sprint_end) {
        int rem = (int)((sprint_end - MIN(sprint_end, ui.in.now)) / 1000);
        fmt(s, sizeof s, "%d:%02d", rem / 60, rem % 60);
        float w = text_width(fb, px, s, -1);
        rx -= w + S(40);
        icon_draw(IC_TIMER, rx - S(2), cy, S(14), T->accent);
        draw_text(fb, px, rx + S(10), base, s, -1, T->accent);
    }
    const char *ib = ime_badge();                       /* método de entrada: 가 / あ / ア / A */
    if (ib[0]) { rx -= S(30); draw_text(fb, S(14), rx, base, ib, -1, str_eq(ib, "A") ? c : T->accent); }
    u32 dk = kbd_pending();
    if (dk) { char d[8]; int n = utf8_encode(dk, d); d[n] = 0; rx -= S(28); draw_text(fb, S(15), rx, base, d, -1, T->accent); }
    gfx_set_opacity(1);
}

/* ======================================================================
 * entrada
 * ====================================================================== */
void app_event(Event *e) {
    Input *in = &ui.in;
    /* mouse desligado (só teclado): movimentos, cliques e roda são ignorados */
    if (prefs.no_mouse && (e->type == EV_MOVE || e->type == EV_DOWN || e->type == EV_UP || e->type == EV_WHEEL)) return;
    switch (e->type) {
    case EV_MOVE:
        if (e->x != in->mx || e->y != in->my) { in->moved = true; last_mouse_ms = pal_ms(); }
        in->mx = e->x; in->my = e->y; break;
    case EV_DOWN:
        in->mx = e->x; in->my = e->y; last_mouse_ms = pal_ms();
        if (e->button == 0) {
            u64 now = pal_ms();
            if (now - in->last_click_ms < 420 && iabs(e->x - in->last_click_x) < 6 && iabs(e->y - in->last_click_y) < 6) in->click_count++;
            else in->click_count = 1;
            in->last_click_ms = now; in->last_click_x = e->x; in->last_click_y = e->y;
            in->pressed = true; in->down = true;
            in->dbl = in->click_count == 2; in->triple = in->click_count >= 3;
        } else if (e->button == 1) in->rpressed = true;
        else if (e->button == 2) { in->mpressed = true; in->mdown = true; }
        break;
    case EV_UP:
        if (e->button == 0) { in->down = false; in->released = true; }
        else if (e->button == 2) { in->mdown = false; in->mreleased = true; }
        break;
    case EV_WHEEL:
        last_mouse_ms = pal_ms();
        if (app_mods() & MOD_CTRL) { if (e->wheel > 0) c_bigger(); else c_smaller(); }
        else in->wheel += e->wheel;
        break;
    case EV_KEY: {
        cur_mods = e->mods;
        Event k = *e;
        /* Alt+letra/número são atalhos (Alt+M, Alt+1…9): a tecla física, sem passar pelo layout */
        if (e->us_raw && e->ch && !e->key && !((e->mods & MOD_ALT) && !(e->mods & (MOD_CTRL | MOD_ALTGR)))) {
            u32 out[2]; int n = kbd_translate(e, out);
            for (int i = 0; i < n && in->nkeys < 64; i++) { k.ch = out[i]; k.us_raw = false; in->keys[in->nkeys++] = k; }
            ui.redraw = true;
            break;
        }
        if (in->nkeys < 64) in->keys[in->nkeys++] = k;
        break; }
    case EV_QUIT: quit_flag = true; break;
    }
    ui.redraw = true;
}

/* atalho de um painel (F5, F6, F10, F12): aberto sem o teclado, recebe o teclado; senão, abre/fecha */
static bool panel_key(Window *w) {
    if (w->open && ui.key_win != w && !wm_any_modal()) { wm_focus(w); return true; }
    return false;
}
static bool global_key(Event *e) {
    bool ctrl = (e->mods & MOD_CTRL) != 0, sh = (e->mods & MOD_SHIFT) != 0;
    /* janela que digita (campos de texto) ou modal: as teclas são dela; painéis (Ajustes, Sumário…) deixam passar os atalhos */
    bool win_keys = ui.key_win != 0 && (ui.key_win->wants_keys || ui.key_win->modal);
    if (e->key == KEY_ESC) {
        Window *top = ui.key_win && ui.key_win->open ? ui.key_win : wm_top();
        if (top) { wm_close(top); return true; }
        if (preview_stop_motion()) return true;      /* Esc primeiro só para o que está rolando (nunca liga) */
        if (preview_active()) { preview_leave(); return true; }
        if (zen) { zen = false; return true; }
        last_mouse_ms = ui.in.now; return true;
    }
    if (wm_kb_key(e)) return true;                         /* Alt+F7/Alt+F8 e as setas enquanto move */
    if (ctrl && !(e->mods & MOD_ALT) && e->ch == ' ') { ime_toggle(); return true; }   /* método de entrada (coreano, japonês) */
    if (ctrl && !(e->mods & MOD_ALT) && e->key == KEY_F6) { wm_cycle_focus(); return true; }   /* teclado: texto ↔ janelas */
    if ((e->mods & MOD_ALT) && !ctrl && (e->ch == 'm' || e->ch == 'M') && !wm_any_modal()) { if (!win_menu.open || ui.key_win == &win_menu) c_menu(); else wm_focus(&win_menu); return true; }
    if (ctrl && (e->mods & MOD_ALT) && !e->ch) {          /* Ctrl+Alt+setas: girar a tela; Ctrl+Alt+Home: restaurar */
        int r = e->key == KEY_UP ? 1 : e->key == KEY_RIGHT ? 2 : e->key == KEY_DOWN ? 3 : e->key == KEY_LEFT ? 4 : 0;
        if (r) { prefs.rotation = r; pal_set_rotation(r); prefs_save(); return true; }
        if (e->key == KEY_HOME) { app_video_reset(); return true; }
    }
    /* abas: Ctrl+Tab / Ctrl+Shift+Tab; Alt+1…9 vai direto (9 = última). No UEFI, Ctrl+Tab chega igual a Ctrl+I: lá vale Alt+número */
    if (prefs.tabs && ctrl && (e->key == KEY_TAB || (e->ch == 9 && !e->key))) { if (sh) c_tab_prev(); else c_tab_next(); return true; }
    if (prefs.tabs && !win_keys && (e->mods & MOD_ALT) && !ctrl && e->ch >= '1' && e->ch <= '9') {
        int i = e->ch == '9' ? ntabs - 1 : (int)(e->ch - '1');
        if (i < ntabs) app_tab_switch(i);
        return true;
    }
    if (ctrl && e->ch) {
        switch (e->ch) {
        case 'w': if (!prefs.tabs) return false; c_tab_close(); return true;
        case 's': if (sh) c_saveas(); else c_save(); return true;
        case 'k': if (sh && !win_keys && !preview_active()) return false; c_palette(); return true;   /* Ctrl+Shift+K apaga o parágrafo */
        case 'q': c_shutdown(); return true;
        default: break;
        }
        if (win_keys) return false;
        switch (e->ch) {
        case 'n': c_new(); return true;
        case 'o': c_open(); return true;
        case 'e': c_saveas(); return true;
        case 'p': if (sh) { c_palette(); return true; } c_preview(); return true;
        case 'f': case 'h': c_find(); return true;
        case 't': if (sh) c_next_font(); else c_next_theme(); return true;
        case '=': case '+': c_bigger(); return true;
        case '-': c_smaller(); return true;
        default: return false;
        }
    }
    if (win_keys && e->key != KEY_F1) return false;
    switch (e->key) {
    case KEY_F1: if (!panel_key(&win_help)) c_help(); return true;
    case KEY_F2: c_focus(); return true;
    case KEY_F3: if (sh) { if (!preview_active()) editor_cycle_case(); } else c_typewriter(); return true;
    case KEY_F4: c_sprint(); return true;
    case KEY_F5: if (!panel_key(&win_outline)) c_outline(); return true;
    case KEY_F6: if (!panel_key(&win_music)) c_music(); return true;
    case KEY_F7: c_prev(); return true;
    case KEY_F8: c_play(); return true;
    case KEY_F9: c_next(); return true;
    case KEY_F10: if (!panel_key(&win_stats)) c_stats(); return true;
    case KEY_F11: c_zen(); return true;
    case KEY_F12: if (!panel_key(&win_settings)) { if (win_settings.open) wm_close(&win_settings); else c_appearance(); } return true;   /* como F5/F6/F10: abre, dá o teclado, fecha */
    default: return false;
    }
}

/* ======================================================================
 * inicialização e quadro
 * ====================================================================== */
/* idioma e teclado do boot. language=auto (ou ausente numa instalação nova): o idioma do firmware
 * (PlatformLang) ou do sistema, se houver um .po para ele; senão, o original (inglês).
 * Instalações de antes da tradução (CONFIG.INI sem "language") eram só em português: continuam. */
extern bool prefs_had_file;
static void setup_language(void) {
    kbd_scan();
    i18n_scan();
    bool legacy = prefs_had_file && !prefs.lang[0];
    bool is_auto = !prefs.lang[0] || str_ieq(prefs.lang, "auto");
    int li = -1;
    if (legacy) li = lang_find("pt-BR");
    else if (!is_auto) li = lang_find(prefs.lang);
    if (li < 0) { char sys[32]; if (pal_lang(sys, sizeof sys)) li = lang_find(sys); }
    if (li < 0) li = 0;
    if (!i18n_set(li) || !fonts_lang_ok()) { li = 0; i18n_set(0); }
    str_copy(prefs.lang, (is_auto && !legacy) ? "auto" : lang_code(li), sizeof prefs.lang);
    if (!prefs.kb[0]) str_copy(prefs.kb, legacy ? "abnt2" : lang_keyboard(li)[0] ? lang_keyboard(li) : "us", sizeof prefs.kb);
    kbd_select(prefs.kb);
}

void app_init(void) {
    Surface *s = pal_screen();
    ui.W = s->w; ui.H = s->h;
    prefs_defaults();
    memset(&app, 0, sizeof app);                 /* estado limpo (o simulador reinicia sem recarregar o programa) */
    ntabs = 0; cur_tab = 0; cur_draft = 0; cur_dver = 0xFFFFFFFFu;
    boot_vol = 0;
    for (int v = 0; v < pal_vol_count(); v++) { VolInfo vi; if (pal_vol_info(v, &vi) && vi.boot) { boot_vol = v; break; } }
    alongside = false;
    {   /* ao lado de outro sistema: o firmware iniciou o FolioOS pela partição EFI, que só tem \EFI\FolioOS;
         * FOLIO.ID diz qual partição tem as fontes, os ajustes e os textos */
        u8 *id; usize n; u8 g[16];
        if (pal_read(boot_vol, "/EFI/FolioOS/FOLIO.ID", &id, &n)) {
            if (install_parse_id((const char *)id, (int)n, g))
                for (int v = 0; v < pal_vol_count(); v++) {
                    VolInfo vi;
                    if (pal_vol_info(v, &vi) && vi.has_guid && !memcmp(vi.part_guid, g, 16)) { boot_vol = v; alongside = true; break; }
                }
            mem_free(id);
        }
    }
    if (!alongside) {   /* PC só BIOS (fase 3): instalado ao lado, o MBR original do outro sistema está guardado aqui */
        bool d; if (pal_exists(boot_vol, "/FOLIO/OTHERMBR.BIN", &d) && !d) alongside = true;
    }
    paths_init(boot_vol);
    fonts_init();
    splash_start();                         /* FolioOS: aparece já, enquanto as fontes carregam */
    fonts_scan(boot_vol, paths_dir(DIR_FONTS));
    { bool d; if (!str_eq(paths_dir(DIR_FONTS), "/FOLIO/FONTS") && pal_exists(boot_vol, "/FOLIO/FONTS", &d)) fonts_scan(boot_vol, "/FOLIO/FONTS"); }   /* pen-drive antigo atualizado: as duas pastas */
    themes_scan(boot_vol);
    prefs_load();
    setup_language();
    if (prefs.font < 0 || prefs.font >= font_count()) {
        int i = font_find("Spectral"); if (i < 0) i = font_find("Literata"); if (i < 0) i = 0;
        prefs.font = i;
    }
    T = theme_get(prefs.theme);
    apply_theme_fonts(false);
    if (alongside) { boot_chooser(); splash_skip(); }   /* "aperte uma tecla para escrever"; sem tecla, o outro sistema inicia */
    if (prefs.docs_place == DOCS_USB && app_installed() && app_texts_stick() < 0)   /* os textos estão no pendrive, que não veio */
        ui_toast(L("The USB drive with your texts is not connected: new texts are saved on this computer for now."));
    pal_set_pointer_speed(prefs.pointer_speed);
    pal_set_touch_mode(prefs.touch_mode);
    pal_audio_set_gpio(prefs.audio_gpio);
    if (prefs.rotation) pal_set_rotation(prefs.rotation);
    if (prefs.video_mode[0]) pal_set_video_mode(prefs.video_mode);
    if (prefs.aspect) app_apply_aspect();
    pal_video_apply();                   /* antes do DIAGNOSTICS.TXT, que mostra a tela em uso */
    pal_video_fast(prefs.video_fast);
    pal_audio_probe();
    doc_init(&app.doc);
    app.vol = boot_vol;
    app.syntax = SYN_MD; app.crlf = false;
    set_title_from_path();
    music_init(boot_vol);
    app_write_diag(false);
    wm_add(&win_settings); wm_add(&win_find); wm_add(&win_outline); wm_add(&win_stats);
    wm_add(&win_help); wm_add(&win_palette); wm_add(&win_menu); wm_add(&win_music); wm_add(&win_files); wm_add(&win_theme_ed);
    wm_add(installer_window());
    wm_add(partitions_window());
    ui.in.now = pal_ms();
    app.session_start_ms = ui.in.now;
    last_frame_ms = ui.in.now; last_mouse_ms = ui.in.now;
    ui.in.mx = s->w / 2; ui.in.my = s->h / 2;
    /* restaura a sessão anterior */
    bool d; int lv;
    if (prefs.tabs && prefs.ntab_saved > 0) {
        /* abas: reabre todas (arquivos e rascunhos de FOLIO\ABAS) na aba em que estava */
        prefs.tabs = false;
        app_tabs_enable(true);
        if (app.has_path && prefs.last_reading && str_eq(app.path, prefs.last_path) && app.syntax != SYN_TEX) preview_open();
        char m[120]; fmt(m, sizeof m, ntabs == 1 ? L("Welcome back. Picking up where you left off.") : LN("Welcome back: %d tab reopened.", "Welcome back: %d tabs reopened.", ntabs), ntabs);
        ui_toast(m);
    } else if (prefs.last_path[0] && (lv = last_volume()) >= 0 && pal_exists(lv, prefs.last_path, &d) && !d) {
        load_into(lv, prefs.last_path, (Pos){ prefs.last_p, prefs.last_o });
        if (prefs.last_reading && app.syntax != SYN_TEX) { preview_open(); ui_toast(L("Welcome back. Reading continues where you left off.")); }
        else ui_toast(L("Welcome back. Picking up where you left off."));
    } else if (pal_exists(boot_vol, paths_dir(FILE_DRAFT), &d) && !d) {
        u8 *data; usize len;
        if (pal_read(boot_vol, paths_dir(FILE_DRAFT), &data, &len)) {
            if (len > 1) { doc_set(&app.doc, (const char *)data, len); app.doc.modified = true; app.caret = app.anchor = doc_end(&app.doc); ui_toast(L("Unsaved draft recovered")); }
            mem_free(data);
        }
    } else {
        /* primeiro uso: texto de boas-vindas no idioma escolhido (uma entrada só no .po) */
        const char *welcome = L("# Welcome to FolioOS\n\n"
            "A distraction-free digital typewriter. No notifications, no internet — just you and the text.\n\n"
            "## First steps\n\n"
            "- Start typing: the bars fade away while you write and come back when you move the mouse.\n"
            "- **Ctrl+S** saves, **Ctrl+Shift+S** saves as or exports to *DOCX, ODT, EPUB, HTML, RTF* and *TXT*.\n"
            "- **Ctrl+K** opens the palette with every command. **F1** shows the shortcuts.\n"
            "- **F2** toggles focus mode and **F3** the typewriter.\n"
            "- Put `.mp3` files in the MUSIC folder of the USB drive and play them with **F8**.\n\n"
            "> Write with your heart. Edit with your head.\n\n"
            "Delete this text (Ctrl+A) and start your story.\n");
        doc_set(&app.doc, welcome, (usize)str_len(welcome));
        app.caret = app.anchor = doc_end(&app.doc);
    }
    if (prefs.tabs && ntabs == 0) { prefs.tabs = false; app_tabs_enable(true); }   /* abas ligadas, lista vazia: o documento atual vira a 1ª aba */
    editor_invalidate();
}

static float auto_scale(int W, int H) {
    float byh = H <= 820 ? 1.0f : H <= 1100 ? 1.08f : H <= 1300 ? 1.25f : H <= 1700 ? 1.5f : 2.0f;
    /* com o tamanho físico do monitor (EDID): mesmo tamanho aparente em qualquer tela, ~110 DPI = 100% */
    float dpi = pal_display_dpi();
    if (dpi < 80 || dpi > 600) return byh;                /* TV, projetor ou EDID sem tamanho */
    float s = dpi / 110.0f;
    s = CLAMP(s, 1.0f, 3.0f);
    s = MIN(s, MAX(1.0f, (float)MIN(W, H) / 700.0f));     /* telas baixas: ao menos ~700 px lógicos de altura */
    return (float)(int)(s * 20.0f + 0.5f) / 20.0f;
}

bool app_frame(void) {
    installer_poll();                    /* instalação pedida no quadro anterior (desenha o progresso por conta própria) */
    partitions_poll();
    Surface *s = pal_screen();
    Input *in = &ui.in;
    u64 now = pal_ms();
    in->dt = CLAMP((float)(now - last_frame_ms) / 1000.0f, 0.0f, 0.1f);
    last_frame_ms = now;
    in->now = now;
    ui.W = s->w; ui.H = s->h;
    float sc = prefs.ui_scale > 0 ? prefs.ui_scale : auto_scale(s->w, s->h);
    if (sc != last_scale) { ui.scale = sc; last_scale = sc; editor_invalidate(); glyph_cache_trim(); }
    ui.redraw = false;
    gfx_target(s);
    if (sheet_baked && !(app_sheet_on())) { sheet_baked = false; build_desk(); }
    if (last_theme != prefs.theme || last_W != s->w || last_H != s->h) build_desk();

    /* abertura: tecla ou clique pulam; a tecla segue para o editor (dá para começar a digitar já) */
    if (splash_active() && (in->nkeys || in->pressed)) {
        splash_skip();
        if (in->pressed) { in->pressed = in->dbl = in->triple = false; }
    }

    if (prefs.no_mouse) { in->mx = in->my = -100000; in->pressed = in->down = in->dbl = in->triple = false; in->wheel = 0; }
    ui_begin_frame();

    /* atalhos globais e teclado do editor; a janela aberta por um atalho recebe o teclado */
    for (int i = 0; i < in->nkeys; i++) {
        Event *e = &in->keys[i];
        if (e->type != EV_KEY) continue;
        int z0 = top_z();
        if (global_key(e)) { e->type = EV_NONE; focus_new_top(z0); }
    }
    if (!ui.key_win && !wm_any_modal()) {
        for (int i = 0; i < in->nkeys; i++) {
            Event *e = &in->keys[i];
            if (e->type != EV_KEY) continue;
            if (preview_active()) preview_key(e); else editor_key(e);
            e->type = EV_NONE;
        }
    }

    /* barras: aparecem com o mouse, somem ao digitar */
    /* digitou depois do último movimento do mouse: as barras só voltam quando o mouse se mexe */
    bool typing_recent = app.last_edit_ms > last_mouse_ms;
    bool near_edge = !prefs.no_mouse && (in->my < SI(70) || in->my > ui.H - SI(52));
    /* rolagem automática com o mouse parado: some tudo, como se estivesse digitando
     * (só se o usuário deixou "ocultar barras" ligado; desligado, as barras ficam) */
    bool auto_idle = prefs.autohide && preview_auto_idle(now - last_mouse_ms);
    bool want = !zen && (!prefs.autohide || !(typing_recent || auto_idle) || near_edge || wm_top() != 0);
    if (zen) {
        /* zen: as barras aparecem ao tocar a borda de cima (ou a de baixo, na leitura, onde fica a
         * barra de leitura) e continuam enquanto o mouse estiver sobre elas; antes, descer o mouse
         * até a barra de baixo a fazia sumir e ela ficava inalcançável */
        bool edge = !prefs.no_mouse && (in->my < SI(8) || (preview_active() && in->my >= ui.H - SI(8)));
        bool on_bars = chrome_a > 0.3f && (in->my < SI(56) || in->my >= ui.H - SI(44));
        want = (edge && now - last_mouse_ms < 1500) || on_bars;
    }
    float tgt = want ? 1.0f : 0.0f;
    if (chrome_a != tgt) {
        float sp = in->dt * (want ? 6.0f : 2.2f);
        chrome_a = want ? MIN(1.0f, chrome_a + sp) : MAX(0.0f, chrome_a - sp);
        ui.redraw = true;
    }

    /* desenho */
    ui.block_top = chrome_a > 0.3f ? SI(56) : 0;
    ui.block_bottom = chrome_a > 0.3f ? SI(44) : 0;
    memcpy(s->px, desk.px, sizeof(u32) * (usize)s->w * s->h);
    Rect area = rect(0, 0, s->w, s->h);
    if (preview_active()) preview_draw(area, chrome_a); else editor_draw(area, chrome_a);
    draw_topbar(chrome_a);
    draw_statusbar(chrome_a);
    preview_draw_bar(chrome_a);
    wm_draw_all();
    /* painel com o teclado (Ajustes, Sumário, Música…) e a pessoa voltou a digitar: o texto vai
     * para o documento, como antes de o painel ter o foco */
    if (ui.key_win && !ui.key_win->modal && !ui.key_win->wants_keys) {
        int first = -1;
        for (int i = 0; i < in->nkeys && first < 0; i++) {
            Event *e = &in->keys[i];
            if (e->type == EV_KEY && e->ch >= 32 && !(e->mods & (MOD_CTRL | MOD_ALT))) first = i;
        }
        if (first >= 0) {
            if (ui.key_win == &win_menu) wm_close(&win_menu);
            wm_focus(0);
            for (int i = first; i < in->nkeys; i++) {
                Event *e = &in->keys[i];
                if (e->type != EV_KEY) continue;
                if (preview_active()) preview_key(e); else editor_key(e);
                e->type = EV_NONE;
            }
        }
    }
    if (win_menu.open && in->pressed && !rect_has(win_menu.r, in->mx, in->my) && !(in->mx < SI(50) && in->my < SI(52))) wm_close(&win_menu);
    ui_draw_toasts();

    /* sprint */
    if (sprint_end && now >= sprint_end) {
        sprint_end = 0;
        int done = MAX(0, doc_words(&app.doc) - (int)sprint_start_words);
        char m[120]; fmt(m, sizeof m, LN("Sprint finished: %d word. Breathe and stretch.", "Sprint finished: %d words. Breathe and stretch.", done), done);
        ui_toast(m); music_chime();
    }
    if (sprint_end && (now / 1000) != ((now - (u64)(in->dt * 1000)) / 1000)) ui.redraw = true;

    autosave();

    /* ponteiro: escondido enquanto se digita */
    bool show_ptr = (now - last_mouse_ms < 3000 || !typing_recent || wm_top()) && !auto_idle && !prefs.no_mouse;
    if (splash_active()) { splash_draw(); ui.redraw = true; }
    if (show_ptr) cursor_draw(ui.cursor, in->mx, in->my, ui.scale);

    ui_end_frame();
    return ui.redraw || editor_animating();
}

/* laço principal compartilhado pelas plataformas */
void app_run(void) {
    app_init();
    u64 next_tick = 0, last_present = 0;
    bool need = true;
    while (!quit_flag) {
        Event e; bool got = false;
        while (ui.in.nkeys < 48 && pal_poll(&e)) {
            app_event(&e); got = true;
            if (quit_flag) break;
            if (e.type == EV_DOWN || e.type == EV_UP) break;   /* um clique por quadro: toque rápido no touchpad não se perde */
        }
        music_update();
        u64 now = pal_ms();
        if (got) need = true;
        if (now >= next_tick) need = true;
        if (need && now - last_present >= 7) {          /* até ~140 quadros por segundo */
            need = app_frame();
            pal_present(rect(0, 0, pal_screen()->w, pal_screen()->h));   /* a PAL só envia o que mudou */
            last_present = now;
            u64 interval = prefs.caret_blink ? 80 : 1000;   /* piscar suave sem gastar CPU à toa */
            next_tick = now + interval;
            if (!need && ui.wake_ms > 0 && now + (u64)ui.wake_ms < next_tick) next_tick = now + (u64)ui.wake_ms;   /* quadro agendado (rolagem automática) */
        } else if (!need) {
            int wait = (int)MIN((u64)20, next_tick > now ? next_tick - now : 0);
            pal_wait(MAX(1, wait));
        }
        /* com quadro pendente, não dorme: o relógio do firmware anda em passos de 10 ms ou mais */
    }
    app_flush_all();
    prefs_save();
}
