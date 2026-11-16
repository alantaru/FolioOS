/* FOLIO v4 — tela de abertura "FolioOS"
 *
 * Aparece logo depois que a fonte embutida fica pronta (antes de ler FOLIO\FONTES, que é a parte
 * lenta do boot), continua por ~1 s depois que o editor está pronto e se dissolve sobre ele.
 * Qualquer tecla ou clique pula; a tecla segue para o editor. Desliga em Ajustes > Sistema
 * ou com abertura=0 no CONFIG.INI. */
#include "folio.h"

#define SPLASH_FADE_MS 380
#define SPLASH_SKIP_MS 160

static bool  on;
static u32   hold_ms = SPLASH_SECS_DEFAULT * 1000;   /* desde o início do boot (abertura_segundos=) */
static u64   t0, fade_from;
static float fade_len;

static const Color BG_TOP = 0xFF1C1E24, BG_BOT = 0xFF0C0D10;
static const Color INK = 0xFFEFE8DC, MUTED = 0xFF8C877E, ACCENT = 0xFFE2A65A, PAPER = 0xFFF4EEE1;

bool splash_active(void) { return on; }

/* triângulo preenchido com anti-aliasing (rasterizador de contornos do gfx) */
static void tri(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
    if ((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0) < 0) { float tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; }
    int bx = (int)f_floor(MIN(x0, MIN(x1, x2))) - 1, by = (int)f_floor(MIN(y0, MIN(y1, y2))) - 1;
    int w = (int)f_ceil(MAX(x0, MAX(x1, x2))) - bx + 2, h = (int)f_ceil(MAX(y0, MAX(y1, y2))) - by + 2;
    Raster r; raster_begin(&r, w, h);
    raster_line(&r, x0 - bx, y0 - by, x1 - bx, y1 - by);
    raster_line(&r, x1 - bx, y1 - by, x2 - bx, y2 - by);
    raster_line(&r, x2 - bx, y2 - by, x0 - bx, y0 - by);
    u8 *m = raster_end(&r);
    gfx_mask(bx, by, m, w, h, w, c);
    mem_free(m);
}

/* lê "splash=" e "splash_seconds=" direto do CONFIG.INI (as preferências ainda não foram
 * carregadas a esta altura); aceita também as chaves antigas "abertura=" e "abertura_segundos=" */
static int key_at(const u8 *d, usize len, usize i, const char *k) {
    int l = str_len(k);
    return (i + (usize)l < len && memcmp(d + i, k, (usize)l) == 0) ? l : 0;
}
static bool wanted(void) {
    u8 *d; usize len;
    hold_ms = SPLASH_SECS_DEFAULT * 1000;
    if (!pal_read(app_boot_vol(), "/FOLIO/CONFIG.INI", &d, &len)) return true;
    bool want = true;
    for (usize i = 0; i < len; i++) {
        if (i && d[i - 1] != '\n') continue;
        int l;
        if ((l = key_at(d, len, i, "splash=")) || (l = key_at(d, len, i, "abertura="))) want = d[i + l] != '0';
        else if ((l = key_at(d, len, i, "splash_seconds=")) || (l = key_at(d, len, i, "abertura_segundos="))) {
            int s = 0;
            for (usize j = i + l; j < len && d[j] >= '0' && d[j] <= '9' && s < 1000; j++) s = s * 10 + (d[j] - '0');
            hold_ms = (u32)CLAMP(s, 1, 60) * 1000;
        }
    }
    mem_free(d);
    return want;
}

static void draw(Surface *s, float alpha, float t) {
    int W = s->w, H = s->h;
    float sc = MAX(1.0f, (float)MIN(W, H) / 820.0f);
    gfx_target(s);
    gfx_clip(rect(0, 0, W, H));
    gfx_set_opacity(alpha);
    gfx_gradient_v(rect(0, 0, W, H), BG_TOP, BG_BOT);

    /* entrada suave: o conjunto sobe 14 px e aparece nos primeiros 450 ms */
    float in = ease_out(CLAMP(t / 450.0f, 0.0f, 1.0f));
    float lift = (1 - in) * 14 * sc;
    float cx = W * 0.5f, cy = H * 0.44f + lift;
    gfx_set_opacity(alpha * (0.25f + 0.75f * in));

    /* marca: uma folha com o canto dobrado e três linhas de texto */
    float pw = 66 * sc, ph = 86 * sc;
    Rect page = rect((int)(cx - pw / 2), (int)(cy - ph - 34 * sc), (int)pw, (int)ph);
    gfx_shadow(rect(page.x, page.y + (int)(6 * sc), page.w, page.h), 6 * sc, 24 * sc, 0x70000000);
    gfx_rrect(page, 5 * sc, PAPER);
    /* canto dobrado: recorta com a cor do fundo e desenha a aba dobrada por cima */
    float fold = 18 * sc, rx = (float)(page.x + page.w), ty = (float)page.y;
    Color bgc = col_mix(BG_TOP, BG_BOT, (ty + fold * 0.5f) / H);
    tri(rx - fold - 0.5f, ty - 1, rx + 1, ty - 1, rx + 1, ty + fold + 0.5f, bgc);
    tri(rx - fold, ty, rx - fold, ty + fold, rx, ty + fold, col_mix(PAPER, 0xFF8C8273, 0.55f));
    for (int k = 0; k < 4; k++) {
        float lw = k == 0 ? pw * 0.42f : k == 3 ? pw * 0.38f : pw * 0.64f;
        Color c = k == 0 ? ACCENT : 0xFF5A5248;
        gfx_rrect(rect(page.x + (int)(11 * sc), page.y + (int)((24 + k * 13) * sc), (int)lw, (int)(4.5f * sc)), 2 * sc, c);
    }
    /* cursor piscando ao fim da última linha */
    if (((int)(t / 480)) % 2 == 0) gfx_rect(rect(page.x + (int)(13 * sc + pw * 0.38f), page.y + (int)((24 + 3 * 13 - 4) * sc), (int)MAX(1.0f, 2 * sc), (int)(12 * sc)), ACCENT);

    /* nome */
    Family *fam = font_ui();
    Face *fb = fam_face(fam, FS_BOLD), *fr = fam_face(fam, FS_REG);
    float px = 58 * sc;
    const char *a = "Folio", *b = "OS";
    float wa = text_width(fb, px, a, -1), wb = text_width(fr, px, b, -1);
    float tx = cx - (wa + wb) * 0.5f, base = cy + 34 * sc;
    draw_text(fb, px, tx, base, a, -1, INK);
    draw_text(fr, px, tx + wa, base, b, -1, ACCENT);
    draw_text_c(fr, 15 * sc, rect(0, (int)(base + 14 * sc), W, (int)(24 * sc)), L("distraction-free digital typewriter"), MUTED);

    /* barra de progresso fina (anda com o tempo do boot) */
    float bw = 180 * sc, bh = MAX(2.0f, 3 * sc);
    float p = CLAMP(t / (float)hold_ms, 0.04f, 1.0f);
    Rect track = rect((int)(cx - bw / 2), (int)(H * 0.70f), (int)bw, (int)bh);
    gfx_rrect(track, bh / 2, 0x26FFFFFF);
    gfx_rrect(rect(track.x, track.y, (int)(bw * ease_out(p)), track.h), bh / 2, ACCENT);

    char v[64]; fmt(v, sizeof v, L("FolioOS %s"), FOLIO_VERSION);
    draw_text_c(fr, 12.5f * sc, rect(0, H - (int)(46 * sc), W, (int)(20 * sc)), v, 0xFF5C5952);
    gfx_set_opacity(1);
}

void splash_start(void) {
    on = wanted();
    if (!on) return;
    t0 = pal_ms(); fade_from = 0; fade_len = SPLASH_FADE_MS;
    Surface *s = pal_screen();
    draw(s, 1, 0);
    pal_present(rect(0, 0, s->w, s->h));
}

void splash_skip(void) {
    if (!on) return;
    u64 now = pal_ms();
    if (!fade_from || now + SPLASH_SKIP_MS < fade_from + (u64)fade_len) { fade_from = now; fade_len = SPLASH_SKIP_MS; }
}

/* chamado ao fim de cada quadro do app, por cima de tudo, enquanto durar */
void splash_draw(void) {
    if (!on) return;
    u64 now = pal_ms();
    if (!fade_from && now >= t0 + hold_ms) fade_from = now;
    float a = 1;
    if (fade_from) {
        a = 1 - (float)(now - fade_from) / fade_len;
        if (a <= 0) { on = false; ui.redraw = true; return; }
        a = a * a * (3 - 2 * a);
    }
    draw(pal_screen(), a, (float)(now - t0));
}
