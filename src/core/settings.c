/* FolioOS — preferências (FOLIO/CONFIG.INI) */
#include "folio.h"

Prefs prefs;
#define CFG_DIR paths_dir(DIR_FOLIO)
#define CFG     paths_dir(FILE_CONFIG)

void prefs_defaults(void) {
    memset(&prefs, 0, sizeof prefs);
    prefs.theme = 0; prefs.font = -1;
    prefs.font_size = 21; prefs.line_height = 1.62f; prefs.column = 700; prefs.para_space = 0.45f;
    prefs.focus_mode = FOCUS_OFF;
    prefs.typewriter = false; prefs.smart_typo = true; prefs.hemingway = false;
    prefs.show_stats = true; prefs.autohide = true; prefs.paper = true; prefs.md_live = true;
    prefs.caret_blink = true; prefs.smooth_caret = true; prefs.hide_markup = false;
    prefs.volume = 60; prefs.goal = 1000; prefs.sprint_min = 25; prefs.read_wpm = 200;
    prefs.autosave_s = 20; prefs.ui_scale = 0; prefs.pointer_speed = 1.0f; prefs.touch_mode = 0; prefs.audio_gpio = true; prefs.video_fast = true;
    prefs.shuffle = false; prefs.repeat = true;
    prefs.tabs = false; prefs.splash = true; prefs.splash_secs = SPLASH_SECS_DEFAULT;
}
static char theme_name[48], font_name[64];
bool prefs_had_file;          /* havia um CONFIG.INI (instalação já em uso) */

/* chaves antigas (em português, de antes da tradução) → chaves atuais */
static const char *legacy_key(const char *k, char *buf, int cap) {
    static const char *alias[][2] = {
        { "tema", "theme" }, { "fonte", "font" }, { "tamanho", "size" }, { "entrelinha", "line_height" },
        { "coluna", "column" }, { "tela_cheia", "full_width" }, { "espaco_paragrafo", "paragraph_space" },
        { "foco", "focus" }, { "maquina", "typewriter" }, { "tipografia", "smart_typography" },
        { "ocultar_barras", "autohide_bars" }, { "papel", "paper" }, { "recuo", "first_line_indent" },
        { "markdown_vivo", "live_markdown" }, { "ocultar_marcacao", "hide_markup" }, { "cursor_pisca", "cursor_blink" },
        { "cursor_suave", "smooth_cursor" }, { "teclado", "keyboard" }, { "meta", "goal" },
        { "leitura_ppm", "reading_wpm" }, { "autosalvar", "autosave" }, { "escala", "scale" }, { "mouse", "pointer_speed" },
        { "toque", "touch" }, { "som_gpio", "audio_gpio" }, { "video_rapido", "fast_video" }, { "rotacao", "rotation" },
        { "resolucao", "resolution" }, { "aleatorio", "shuffle" }, { "repetir", "repeat" },
        { "ultimo_arquivo", "last_file" }, { "ultimo_volume", "last_volume" }, { "ultimo_paragrafo", "last_paragraph" },
        { "ultima_coluna", "last_column" }, { "hoje_data", "today_date" }, { "hoje_palavras", "today_words" },
        { "lendo", "reading" }, { "abas", "tabs" }, { "abertura", "splash" }, { "abertura_segundos", "splash_seconds" },
        { "aba_ativa", "active_tab" }, { "idioma", "language" },
    };
    for (int i = 0; i < COUNT(alias); i++) if (str_eq(k, alias[i][0])) return alias[i][1];
    /* numeradas: recenteN, posicaoN, abaposN, abaN */
    static const char *num[][2] = { { "recente", "recent" }, { "posicao", "position" }, { "abapos", "tabpos" }, { "aba", "tab" } };
    for (int i = 0; i < COUNT(num); i++) {
        int l = str_len(num[i][0]);
        if (str_starts(k, num[i][0]) && k[l] >= '1' && k[l] <= '9') { fmt(buf, cap, "%s%s", num[i][1], k + l); return buf; }
    }
    return k;
}

static bool numbered(const char *k, const char *prefix, int *idx, int max) {
    int l = str_len(prefix);
    if (!str_starts(k, prefix) || k[l] < '1' || k[l] > '9') return false;
    for (const char *c = k + l; *c; c++) if (*c < '0' || *c > '9') return false;
    *idx = str_to_int(k + l) - 1;
    return *idx >= 0 && *idx < max;
}

void prefs_load(void) {
    u8 *data; usize len;
    prefs_had_file = false;
    if (!pal_read(app_boot_vol(), CFG, &data, &len)) return;
    prefs_had_file = true;
    char *s = (char *)data;
    usize i = 0;
    while (i < len) {
        usize ls = i; while (i < len && s[i] != '\n') i++;
        usize le = i; i++;
        if (le > ls && s[le - 1] == '\r') le--;
        char line[600]; int n = (int)MIN(le - ls, sizeof line - 1);
        memcpy(line, s + ls, (usize)n); line[n] = 0;
        if (!line[0] || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        char *eq = 0; for (char *c = line; *c; c++) if (*c == '=') { eq = c; break; }
        if (!eq) continue;
        *eq = 0; char *k0 = line, *v = eq + 1;
        while (*v == ' ') v++;
        int kl = str_len(k0); while (kl > 0 && k0[kl - 1] == ' ') k0[--kl] = 0;
        char kb_[32]; const char *k = legacy_key(k0, kb_, sizeof kb_);
        int iv = str_to_int(v); float fv = (float)iv;
        { /* decimal com ponto ou vírgula */
            const char *c = v; int sg = 1; if (*c == '-') { sg = -1; c++; }
            float whole = 0, frac = 0, dv = 1; while (*c >= '0' && *c <= '9') whole = whole * 10 + (*c++ - '0');
            if (*c == '.' || *c == ',') { c++; while (*c >= '0' && *c <= '9') { frac = frac * 10 + (*c++ - '0'); dv *= 10; } }
            fv = sg * (whole + frac / dv);
        }
        bool bv = iv != 0 || str_ieq(v, "true") || str_ieq(v, "yes") || str_ieq(v, "on") || str_ieq(v, "sim");
        int r;
        if (str_eq(k, "theme")) str_copy(theme_name, v, sizeof theme_name);
        else if (str_eq(k, "font")) str_copy(font_name, v, sizeof font_name);
        else if (str_eq(k, "language")) str_copy(prefs.lang, v, sizeof prefs.lang);
        else if (str_eq(k, "keyboard")) str_copy(prefs.kb, v, sizeof prefs.kb);
        else if (str_eq(k, "size")) prefs.font_size = CLAMP(fv, 12.0f, 48.0f);
        else if (str_eq(k, "line_height")) prefs.line_height = CLAMP(fv, 1.1f, 2.4f);
        else if (str_eq(k, "column")) prefs.column = CLAMP(fv, 360.0f, 1400.0f);
        else if (str_eq(k, "full_width")) prefs.full_width = bv;
        else if (str_eq(k, "paragraph_space")) prefs.para_space = CLAMP(fv, 0.0f, 1.5f);
        else if (str_eq(k, "focus")) prefs.focus_mode = CLAMP(iv, 0, 3);
        else if (str_eq(k, "typewriter")) prefs.typewriter = bv;
        else if (str_eq(k, "smart_typography")) prefs.smart_typo = bv;
        else if (str_eq(k, "hemingway")) prefs.hemingway = bv;
        else if (str_eq(k, "autohide_bars")) prefs.autohide = bv;
        else if (str_eq(k, "paper")) prefs.paper = bv;
        else if (str_eq(k, "first_line_indent")) prefs.first_indent = bv;
        else if (str_eq(k, "live_markdown")) prefs.md_live = bv;
        else if (str_eq(k, "hide_markup")) prefs.hide_markup = bv;
        else if (str_eq(k, "cursor_blink")) prefs.caret_blink = bv;
        else if (str_eq(k, "smooth_cursor")) prefs.smooth_caret = bv;
        else if (str_eq(k, "volume")) prefs.volume = CLAMP(iv, 0, 100);
        else if (str_eq(k, "goal")) prefs.goal = CLAMP(iv, 0, 100000);
        else if (str_eq(k, "sprint")) prefs.sprint_min = CLAMP(iv, 5, 120);
        else if (str_eq(k, "reading_wpm")) prefs.read_wpm = CLAMP(iv, 60, 900);
        else if (str_eq(k, "autosave")) prefs.autosave_s = CLAMP(iv, 5, 600);
        else if (str_eq(k, "scale")) prefs.ui_scale = CLAMP(fv, 0.0f, 3.0f);
        else if (str_eq(k, "pointer_speed")) prefs.pointer_speed = CLAMP(fv, 0.2f, 4.0f);
        else if (str_eq(k, "touch")) prefs.touch_mode = CLAMP(iv, 0, 2);
        else if (str_eq(k, "use_mouse")) prefs.no_mouse = !bv;
        else if (str_eq(k, "debug_serial")) dbg_on = bv;
        else if (str_eq(k, "audio_gpio")) prefs.audio_gpio = bv;
        else if (str_eq(k, "fast_video")) prefs.video_fast = bv;
        else if (str_eq(k, "rotation")) prefs.rotation = str_ieq(v, "auto") ? 0 : iv == 0 && v[0] == '0' ? 1 : iv == 90 ? 2 : iv == 180 ? 3 : iv == 270 ? 4 : 0;
        else if (str_eq(k, "resolution")) str_copy(prefs.video_mode, str_ieq(v, "auto") ? "" : v, sizeof prefs.video_mode);
        else if (str_eq(k, "shuffle")) prefs.shuffle = bv;
        else if (str_eq(k, "repeat")) prefs.repeat = bv;
        else if (str_eq(k, "last_file")) str_copy(prefs.last_path, v, sizeof prefs.last_path);
        else if (str_eq(k, "last_volume")) prefs.last_vol = iv;
        else if (str_eq(k, "last_paragraph")) prefs.last_p = iv;
        else if (str_eq(k, "last_column")) prefs.last_o = iv;
        else if (str_eq(k, "today_date")) prefs.today_date = iv;
        else if (str_eq(k, "today_words")) prefs.today_words = iv;
        else if (str_eq(k, "reading")) prefs.last_reading = bv;
        else if (str_eq(k, "tabs")) prefs.tabs = bv;
        else if (str_eq(k, "splash")) prefs.splash = bv;
        else if (str_eq(k, "splash_seconds")) prefs.splash_secs = CLAMP(iv, 1, 60);
        else if (str_eq(k, "active_tab")) prefs.tab_active = MAX(0, iv - 1);
        else if (numbered(k, "recent", &r, 6)) str_copy(prefs.recent[r], v, sizeof prefs.recent[0]);
        else if (numbered(k, "position", &r, 6)) {
            const char *c = v; while (*c && *c != ',') c++;
            prefs.recent_pos[r][0] = MAX(0, iv); prefs.recent_pos[r][1] = *c ? MAX(0, str_to_int(c + 1)) : 0;
        }
        else if (numbered(k, "tabpos", &r, MAX_TABS)) {
            const char *c = v; while (*c && *c != ',') c++;
            prefs.tab_pos[r][0] = MAX(0, iv); prefs.tab_pos[r][1] = *c ? MAX(0, str_to_int(c + 1)) : 0;
        }
        else if (numbered(k, "tab", &r, MAX_TABS)) {
            str_copy(prefs.tab_list[r], v, sizeof prefs.tab_list[0]);
            if (r + 1 > prefs.ntab_saved) prefs.ntab_saved = r + 1;
        }
    }
    mem_free(data);
    int t = theme_find(theme_name);
    if (t >= 0) prefs.theme = t;
    if (font_name[0]) prefs.font = font_find(font_name);
}

static void put_dec(Buf *b, const char *k, float v) {
    buf_printf(b, "%s=%d.%02d\r\n", k, (int)v, (int)((v - (int)v) * 100 + 0.5f));
}

void prefs_save(void) {
    app_remember_pos();
    Buf b = {0};
    buf_puts(&b, "# FolioOS " FOLIO_VERSION " — preferences (you can edit this file by hand)\r\n");
    buf_printf(&b, "language=%s\r\nkeyboard=%s\r\n", prefs.lang[0] ? prefs.lang : "auto", prefs.kb);
    buf_printf(&b, "theme=%s\r\n", theme_get(prefs.theme)->name);
    buf_printf(&b, "font=%s\r\n", font_get(prefs.font)->name);
    buf_printf(&b, "size=%d\r\n", (int)(prefs.font_size + 0.5f));
    put_dec(&b, "line_height", prefs.line_height);
    buf_printf(&b, "column=%d\r\nfull_width=%d\r\n", (int)prefs.column, prefs.full_width);
    put_dec(&b, "paragraph_space", prefs.para_space);
    buf_printf(&b, "focus=%d\r\ntypewriter=%d\r\nsmart_typography=%d\r\nhemingway=%d\r\n", prefs.focus_mode, prefs.typewriter, prefs.smart_typo, prefs.hemingway);
    buf_printf(&b, "first_line_indent=%d\r\n", prefs.first_indent);
    buf_printf(&b, "autohide_bars=%d\r\npaper=%d\r\nlive_markdown=%d\r\nhide_markup=%d\r\n", prefs.autohide, prefs.paper, prefs.md_live, prefs.hide_markup);
    buf_printf(&b, "cursor_blink=%d\r\nsmooth_cursor=%d\r\n", prefs.caret_blink, prefs.smooth_caret);
    buf_printf(&b, "volume=%d\r\ngoal=%d\r\nsprint=%d\r\nautosave=%d\r\nreading_wpm=%d\r\n", prefs.volume, prefs.goal, prefs.sprint_min, prefs.autosave_s, prefs.read_wpm);
    put_dec(&b, "scale", prefs.ui_scale);
    put_dec(&b, "pointer_speed", prefs.pointer_speed);
    buf_printf(&b, "shuffle=%d\r\nrepeat=%d\r\n", prefs.shuffle, prefs.repeat);
    buf_printf(&b, "touch=%d\r\naudio_gpio=%d\r\nfast_video=%d\r\n", prefs.touch_mode, prefs.audio_gpio, prefs.video_fast);
    buf_printf(&b, "use_mouse=%d\r\n", !prefs.no_mouse);
    {
        static const char *rn[5] = { "auto", "0", "90", "180", "270" };
        buf_printf(&b, "rotation=%s\r\nresolution=%s\r\n", rn[CLAMP(prefs.rotation, 0, 4)], prefs.video_mode[0] ? prefs.video_mode : "auto");
    }
    if (app.has_path) {
        buf_printf(&b, "last_file=%s\r\nlast_volume=%d\r\nlast_paragraph=%d\r\nlast_column=%d\r\n", app.path, app.vol, preview_top_para(), preview_active() ? 0 : app.caret.o);
        buf_printf(&b, "reading=%d\r\n", preview_active());
    }
    buf_printf(&b, "today_date=%d\r\ntoday_words=%d\r\n", prefs.today_date, app_words_today());
    for (int i = 0; i < 6; i++)
        if (prefs.recent[i][0]) buf_printf(&b, "recent%d=%s\r\nposition%d=%d,%d\r\n", i + 1, prefs.recent[i], i + 1, prefs.recent_pos[i][0], prefs.recent_pos[i][1]);
    /* abas: com elas desligadas a lista anterior continua guardada, para voltar igual ao religar */
    if (prefs.tabs) app_tabs_to_prefs();
    buf_printf(&b, "splash=%d\r\nsplash_seconds=%d\r\ntabs=%d\r\nactive_tab=%d\r\n", prefs.splash, prefs.splash_secs, prefs.tabs, prefs.tab_active + 1);
    for (int i = 0, n = 0; i < prefs.ntab_saved; i++)
        if (prefs.tab_list[i][0]) { n++; buf_printf(&b, "tab%d=%s\r\ntabpos%d=%d,%d\r\n", n, prefs.tab_list[i], n, prefs.tab_pos[i][0], prefs.tab_pos[i][1]); }
    paths_ensure(app_boot_vol(), DIR_FOLIO);
    pal_write(app_boot_vol(), CFG, b.s, b.len);
    buf_free(&b);
}
