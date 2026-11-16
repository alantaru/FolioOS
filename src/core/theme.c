/* FolioOS — temas (paletas calibradas para leitura prolongada); nomes traduzíveis com L() */
#include "folio.h"

/*  nome, escuro, mesa, papel, texto, apagado, sutil, destaque, seleção, cursor,
    barra, texto-barra, borda, painel, texto-painel, título, fundo-código, marca-texto, folha, vinheta */
#define TH(n, dk, desk, paper, text, muted, faint, acc, sel, caret, chrome, ctext, border, panel, ptext, head, code, mark, sheet, grain) \
    { n, dk, RGB(desk), RGB(paper), RGB(text), RGB(muted), RGB(faint), RGB(acc), RGB(sel), RGB(caret), RGB(chrome), RGB(ctext), \
      RGB(border), RGB(panel), RGB(ptext), RGB(head), RGB(code), RGB(mark), sheet, grain, 0, 0, false }

static const Theme themes[] = {
    TH(N_("Papyrus"), false, 0xE6DECD, 0xF8F2E4, 0x3A3227, 0x9C907B, 0xE7DDCB, 0xB4561F, 0xEAD7B2, 0xB4561F,
       0xF3ECDD, 0x5A4F40, 0xD8CCB6, 0xFBF7EE, 0x3A3227, 0x29221A, 0xEEE4D1, 0xF2D78C, true, 0.16f),
    TH(N_("Snow"), false, 0xEDEDEA, 0xFFFFFF, 0x24262A, 0x9B9DA3, 0xEFEFF1, 0x2F6FEB, 0xD5E3FF, 0x2F6FEB,
       0xFAFAFA, 0x4A4D55, 0xE1E2E5, 0xFFFFFF, 0x24262A, 0x111317, 0xF2F3F5, 0xFFE68A, true, 0.08f),
    TH(N_("Obsidian"), true,  0x111214, 0x18191C, 0xD9D4CB, 0x6F6B65, 0x26272B, 0xE2A65A, 0x3B3326, 0xE2A65A,
       0x1D1E22, 0xB9B3A8, 0x2E2F34, 0x202125, 0xD9D4CB, 0xF0EBE2, 0x232428, 0x5C4A22, false, 0.25f),
    TH(N_("E-Ink"), false, 0xE9E9E4, 0xF1F1EC, 0x141414, 0x7C7C77, 0xE0E0DA, 0x141414, 0xCFCFC7, 0x141414,
       0xF1F1EC, 0x2C2C2C, 0xCDCDC6, 0xF5F5F1, 0x141414, 0x000000, 0xE6E6E0, 0xD9D9D0, false, 0.0f),
    TH(N_("Sepia"), false, 0xDCCAA8, 0xF0E2C4, 0x4B3A27, 0x9A8466, 0xE6D5B3, 0x8E4A2C, 0xE2C79A, 0x8E4A2C,
       0xEADBBB, 0x5C4831, 0xCDB892, 0xF4E8CE, 0x4B3A27, 0x3A2B1B, 0xE7D6B4, 0xE8C46A, true, 0.22f),
    TH(N_("Ocean"), true,  0x272C36, 0x2E3440, 0xD8DEE9, 0x6F7A8F, 0x3B4252, 0x88C0D0, 0x434C5E, 0x88C0D0,
       0x2B303B, 0xC4CCD9, 0x3B4252, 0x323946, 0xD8DEE9, 0xECEFF4, 0x353C4A, 0x5E5530, false, 0.18f),
    TH(N_("Forest"), true,  0x141B17, 0x1A231E, 0xD3DCCD, 0x6D7B70, 0x26312A, 0x93C47D, 0x33452F, 0x93C47D,
       0x18201B, 0xB4C0AF, 0x2A352E, 0x1F2923, 0xD3DCCD, 0xE7EFE2, 0x212C25, 0x4C5A2A, false, 0.22f),
    TH(N_("Midnight"), true,  0x0C1120, 0x111829, 0xCDD5EB, 0x5F6A8A, 0x1C2440, 0x7AA2F7, 0x283456, 0x7AA2F7,
       0x10162A, 0xA9B3D1, 0x222B47, 0x151C31, 0xCDD5EB, 0xE6EBFA, 0x182038, 0x4A4A20, false, 0.25f),
    TH(N_("Lavender"), false, 0xE7E2EF, 0xF8F5FC, 0x2E2939, 0x968FA6, 0xEAE4F2, 0x7756C9, 0xDDD2F4, 0x7756C9,
       0xF3EFF8, 0x4B4458, 0xD9D1E6, 0xFBF9FD, 0x2E2939, 0x1F1A2A, 0xEEE8F5, 0xF2DE8A, true, 0.10f),
    TH(N_("Rosé"), false, 0xF0E1DE, 0xFCF3F1, 0x3D2A2B, 0xA28A8B, 0xF2E3E0, 0xC0506F, 0xF1D0D6, 0xC0506F,
       0xF8EDEA, 0x5B4446, 0xE6CFCB, 0xFDF7F5, 0x3D2A2B, 0x2C1B1C, 0xF5E6E3, 0xF5D98C, true, 0.10f),
    TH(N_("Graphite"), true,  0x1A1A1B, 0x222224, 0xD6D6D6, 0x737376, 0x2E2E31, 0x58B6F0, 0x2D4152, 0x58B6F0,
       0x1F1F21, 0xB8B8BA, 0x323235, 0x27272A, 0xD6D6D6, 0xF0F0F0, 0x2A2A2D, 0x4F4A25, false, 0.20f),
    TH(N_("Phosphor"), true,  0x030703, 0x061006, 0x52E86A, 0x2A7A36, 0x0C1E0F, 0xA6FFB2, 0x124A1C, 0xA6FFB2,
       0x051005, 0x3FC157, 0x10301A, 0x081608, 0x52E86A, 0x8CFF9E, 0x0A1C0C, 0x1E5A1E, false, 0.35f),
    TH(N_("Amber"), true,  0x0B0700, 0x120C02, 0xFFB43C, 0x8A6420, 0x22180A, 0xFFD27F, 0x4A3410, 0xFFD27F,
       0x100A01, 0xE0A040, 0x302208, 0x171004, 0xFFB43C, 0xFFD27F, 0x1E1606, 0x5A4010, false, 0.35f),
    TH(N_("Solar Light"), false, 0xEEE8D5, 0xFDF6E3, 0x586E75, 0x93A1A1, 0xEEE8D5, 0x268BD2, 0xE4DCC1, 0xCB4B16,
       0xF6EFDB, 0x586E75, 0xE0D8C0, 0xFDF6E3, 0x586E75, 0x073642, 0xF1EAD5, 0xF0DE9A, true, 0.10f),
    TH(N_("Solar Dark"), true,  0x00232C, 0x002B36, 0x93A1A1, 0x586E75, 0x073642, 0xB58900, 0x0C4554, 0xCB4B16,
       0x00262F, 0x839496, 0x0A3D4A, 0x01313D, 0x93A1A1, 0xEEE8D5, 0x073642, 0x4E4A10, false, 0.2f),
    TH(N_("High Contrast"), true,  0x000000, 0x000000, 0xFFFFFF, 0xB0B0B0, 0x1C1C1C, 0xFFD400, 0x3C3C00, 0xFFD400,
       0x000000, 0xFFFFFF, 0x808080, 0x0A0A0A, 0xFFFFFF, 0xFFFFFF, 0x1A1A1A, 0x5A5A00, false, 0.0f),
    TH(N_("Coffee"), true,  0x241C18, 0x2C221D, 0xE5D6C5, 0x8C7766, 0x3A2E27, 0xD8925F, 0x4C3628, 0xD8925F,
       0x281F1A, 0xC9B6A3, 0x3E3129, 0x31261F, 0xE5D6C5, 0xF5E8DA, 0x352A23, 0x5C4220, false, 0.28f),
    TH(N_("Dune"), false, 0xECE2D3, 0xFAF6EF, 0x3B3530, 0x9C9186, 0xF0E8DC, 0xC26D35, 0xF1DCC4, 0xC26D35,
       0xF5EEE3, 0x5A5148, 0xE2D6C4, 0xFCF9F4, 0x3B3530, 0x28231F, 0xF2EBDF, 0xF3D48A, true, 0.12f),
    TH(N_("Mist"), false, 0xE3E8EE, 0xF6F8FB, 0x29323D, 0x8A95A3, 0xE9EDF2, 0x3A7BD5, 0xD3E2F6, 0x3A7BD5,
       0xF0F3F7, 0x47505C, 0xD6DDE6, 0xF9FAFC, 0x29323D, 0x18202A, 0xECF0F5, 0xF5E28E, true, 0.08f),
    TH(N_("Wine"), true,  0x1B1012, 0x231518, 0xE8D5D8, 0x86686D, 0x332125, 0xE57A90, 0x4A2731, 0xE57A90,
       0x1F1315, 0xC9AEB2, 0x3A262A, 0x29191C, 0xE8D5D8, 0xF7E9EB, 0x2E1C20, 0x5A3020, false, 0.28f),
};

const Theme *T = &themes[0];

/* ------------------------------------------------------------ temas do usuário (FOLIO\TEMAS\*.INI)
 * Cada tema é um arquivo de texto comum: cores em #RRGGBB, fontes pelo nome da família.
 * Pode ser criado no editor de temas (Ajustes > Aparência) ou à mão em qualquer computador. */
typedef struct { Theme t; char name[48], ftext[48], fui[48]; } CustomTheme;
static CustomTheme customs[MAX_CUSTOM_THEMES];
static int ncustom;
#define TDIR paths_dir(DIR_THEMES)

int theme_count(void) { return COUNT(themes) + ncustom; }
int theme_builtin_count(void) { return COUNT(themes); }
const Theme *theme_get(int i) {
    i = CLAMP(i, 0, theme_count() - 1);
    return i < COUNT(themes) ? &themes[i] : &customs[i - COUNT(themes)].t;
}
int theme_find(const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < theme_count(); i++) if (str_ieq(theme_get(i)->name, name)) return i;
    /* nomes antigos dos temas prontos (antes da tradução) */
    static const char *legacy[][2] = { { "Papiro", "Papyrus" }, { "Neve", "Snow" }, { "Obsidiana", "Obsidian" }, { "Sépia", "Sepia" }, { "Oceano", "Ocean" }, { "Floresta", "Forest" }, { "Meia-noite", "Midnight" }, { "Lavanda", "Lavender" }, { "Grafite", "Graphite" }, { "Fósforo", "Phosphor" }, { "Âmbar", "Amber" }, { "Solar claro", "Solar Light" }, { "Solar escuro", "Solar Dark" }, { "Alto contraste", "High Contrast" }, { "Café", "Coffee" }, { "Duna", "Dune" }, { "Névoa", "Mist" }, { "Vinho", "Wine" } };
    for (int i = 0; i < COUNT(legacy); i++) if (str_ieq(legacy[i][0], name)) return theme_find(legacy[i][1]);
    return -1;
}

/* nomes das cores, na ordem dos campos de Theme (desk..mark) */
static const char *ckeys[THEME_NCOLORS] = { "desk", "paper", "text", "muted", "subtle", "accent", "selection", "cursor",
    "bars", "bar_text", "borders", "panels", "panel_text", "headings", "code_background", "highlight" };
/* chaves antigas (em português), aceitas ao ler */
static const char *ckeys_old[THEME_NCOLORS] = { "mesa", "papel", "texto", "apagado", "sutil", "destaque", "selecao", "cursor",
    "barras", "texto_barras", "bordas", "paineis", "texto_paineis", "titulos", "fundo_codigo", "marca_texto" };
Color *theme_color(Theme *t, int k) { return &(&t->desk)[CLAMP(k, 0, THEME_NCOLORS - 1)]; }
const char *theme_color_key(int k) { return ckeys[CLAMP(k, 0, THEME_NCOLORS - 1)]; }

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
bool color_parse(const char *s, Color *out) {
    while (*s == ' ') s++;
    if (*s == '#') s++;
    u32 v = 0;
    for (int i = 0; i < 6; i++) { int h = hexv(s[i]); if (h < 0) return false; v = v * 16 + (u32)h; }
    *out = RGB(v); return true;
}

static void custom_fix(CustomTheme *c) {
    c->t.name = c->name;
    c->t.font_text = c->ftext[0] ? c->ftext : 0;
    c->t.font_ui = c->fui[0] ? c->fui : 0;
    c->t.custom = true;
}

static bool parse_theme(const char *s, usize len, CustomTheme *c) {
    memset(c, 0, sizeof *c);
    c->t = themes[0];
    bool dark_set = false;
    usize i = 0;
    while (i < len) {
        usize ls = i; while (i < len && s[i] != '\n') i++;
        usize le = i; i++;
        if (le > ls && s[le - 1] == '\r') le--;
        char line[200]; int n = (int)MIN(le - ls, sizeof line - 1);
        memcpy(line, s + ls, (usize)n); line[n] = 0;
        if (!line[0] || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        char *eq = 0; for (char *p = line; *p; p++) if (*p == '=') { eq = p; break; }
        if (!eq) continue;
        *eq = 0; char *k = line, *v = eq + 1;
        int kl = str_len(k); while (kl > 0 && k[kl - 1] == ' ') k[--kl] = 0;
        while (*v == ' ') v++;
        int vl = str_len(v); while (vl > 0 && v[vl - 1] == ' ') v[--vl] = 0;
        if (str_eq(k, "name") || str_eq(k, "nome")) str_copy(c->name, v, sizeof c->name);
        else if (str_eq(k, "font") || str_eq(k, "fonte")) str_copy(c->ftext, v, sizeof c->ftext);
        else if (str_eq(k, "ui_font") || str_eq(k, "fonte_interface")) str_copy(c->fui, v, sizeof c->fui);
        else if (str_eq(k, "dark") || str_eq(k, "escuro")) { c->t.dark = str_to_int(v) != 0 || str_ieq(v, "yes") || str_ieq(v, "true"); dark_set = true; }
        else if (str_eq(k, "sheet") || str_eq(k, "folha")) c->t.paper_sheet = str_to_int(v) != 0 || str_ieq(v, "yes") || str_ieq(v, "true");
        else if (str_eq(k, "vignette") || str_eq(k, "vinheta")) c->t.grain = CLAMP((float)str_to_int(v) / 100.0f, 0.0f, 0.6f);
        else for (int q = 0; q < THEME_NCOLORS; q++) if (str_eq(k, ckeys[q]) || str_eq(k, ckeys_old[q])) color_parse(v, theme_color(&c->t, q));
    }
    if (!c->name[0]) return false;
    if (!dark_set) c->t.dark = col_luma(c->t.paper) < 0.5f;
    custom_fix(c);
    return true;
}

void themes_scan(int vol) {
    ncustom = 0;
    FileEntry *l; int n = pal_list(vol, TDIR, &l);
    if (n <= 0) return;
    for (int i = 0; i < n && ncustom < MAX_CUSTOM_THEMES; i++) {
        if (l[i].dir || !str_ends_i(l[i].name, ".ini")) continue;
        char p[400]; path_join(p, sizeof p, TDIR, l[i].name);
        u8 *d; usize len;
        if (!pal_read(vol, p, &d, &len)) continue;
        CustomTheme *c = &customs[ncustom];
        if (parse_theme((const char *)d, len, c)) {
            bool dup = false;
            for (int k = 0; k < theme_count(); k++) if (str_ieq(theme_get(k)->name, c->name)) dup = true;
            if (!dup) ncustom++;
        }
        mem_free(d);
    }
    mem_free(l);
}

/* nome de arquivo seguro para FAT32 a partir do nome do tema */
static void theme_file(const char *name, char *out, int cap) {
    char b[64]; int n = 0;
    for (const char *p = name; *p && n < 40; p++) {
        u8 ch = (u8)*p;
        if (ch < 32 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') ch = '_';
        b[n++] = (char)ch;
    }
    while (n > 0 && (b[n - 1] == ' ' || b[n - 1] == '.')) n--;
    b[n] = 0;
    if (!n) str_copy(b, "theme", sizeof b);
    fmt(out, cap, "%s/%s.INI", TDIR, b);
}

static void put_color(Buf *b, const char *k, Color c) { buf_printf(b, "%s=#%02X%02X%02X\r\n", k, (int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255)); }

/* grava (ou regrava) um tema do usuário; old_name = nome anterior ao editar (renomear). Devolve o índice ou -1. */
int theme_save_custom(const Theme *t, const char *name, const char *ftext, const char *fui, const char *old_name) {
    int vol = app_boot_vol();
    bool d;
    (void)d; paths_ensure(vol, DIR_THEMES);
    Buf b = {0};
    buf_puts(&b, "# FolioOS theme — you can edit it by hand. Colors in #RRGGBB; fonts by family name.\r\n");
    buf_printf(&b, "name=%s\r\ndark=%d\r\nsheet=%d\r\nvignette=%d\r\n", name, t->dark, t->paper_sheet, (int)(t->grain * 100 + 0.5f));
    buf_printf(&b, "font=%s\r\nui_font=%s\r\n", ftext ? ftext : "", fui ? fui : "");
    for (int q = 0; q < THEME_NCOLORS; q++) put_color(&b, ckeys[q], *theme_color((Theme *)t, q));
    char path[400]; theme_file(name, path, sizeof path);
    if (old_name && old_name[0] && !str_ieq(old_name, name)) { char op[400]; theme_file(old_name, op, sizeof op); pal_remove(vol, op); }
    bool ok = pal_write(vol, path, b.s, b.len);
    buf_free(&b);
    if (!ok) return -1;
    /* atualiza a lista em memória sem mover os temas já carregados (T pode apontar para um deles) */
    int slot = -1;
    for (int i = 0; i < ncustom; i++) if (str_ieq(customs[i].name, old_name && old_name[0] ? old_name : name)) slot = i;
    if (slot < 0) { if (ncustom >= MAX_CUSTOM_THEMES) return -1; slot = ncustom++; }
    CustomTheme *c = &customs[slot];
    c->t = *t;
    str_copy(c->name, name, sizeof c->name);
    str_copy(c->ftext, ftext ? ftext : "", sizeof c->ftext);
    str_copy(c->fui, fui ? fui : "", sizeof c->fui);
    custom_fix(c);
    return COUNT(themes) + slot;
}

bool theme_delete_custom(int i) {
    int k = i - COUNT(themes);
    if (k < 0 || k >= ncustom) return false;
    char path[400]; theme_file(customs[k].name, path, sizeof path);
    pal_remove(app_boot_vol(), path);
    for (int j = k; j + 1 < ncustom; j++) { customs[j] = customs[j + 1]; custom_fix(&customs[j]); }
    ncustom--;
    return true;
}

/* ------------------------------------------------------------ HSL (seletor de cores) */
void color_to_hsl(Color c, float *h, float *s, float *l) {
    float r = ((c >> 16) & 255) / 255.0f, g = ((c >> 8) & 255) / 255.0f, b = (c & 255) / 255.0f;
    float mx = MAX(r, MAX(g, b)), mn = MIN(r, MIN(g, b)), d = mx - mn;
    *l = (mx + mn) * 0.5f;
    if (d < 1e-5f) { *h = 0; *s = 0; return; }
    *s = *l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
    float hh;
    if (mx == r) hh = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) hh = (b - r) / d + 2;
    else hh = (r - g) / d + 4;
    *h = hh * 60.0f;
}
static float hue2(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}
Color color_from_hsl(float h, float s, float l) {
    float r, g, b;
    h = h / 360.0f; s = CLAMP(s, 0.0f, 1.0f); l = CLAMP(l, 0.0f, 1.0f);
    if (s < 1e-5f) r = g = b = l;
    else {
        float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        r = hue2(p, q, h + 1.0f / 3); g = hue2(p, q, h); b = hue2(p, q, h - 1.0f / 3);
    }
    return RGB(((u32)(r * 255 + 0.5f) << 16) | ((u32)(g * 255 + 0.5f) << 8) | (u32)(b * 255 + 0.5f));
}
