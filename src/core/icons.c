/* FOLIO v4 — ícones vetoriais (grade 24x24) e cursores, rasterizados e em cache */
#include "folio.h"

typedef struct { Raster *r; float s, w, pad; } IC;
#define PT(x) ((x) * c->s + c->pad)

static void ic_poly(IC *c, const float *p, int n, bool hole) {
    float area = 0;
    for (int i = 0; i < n; i++) { int j = (i + 1) % n; area += p[2 * i] * p[2 * j + 1] - p[2 * j] * p[2 * i + 1]; }
    bool rev = hole ? area > 0 : area < 0;
    for (int k = 0; k < n; k++) {
        int i = rev ? n - 1 - k : k, j = rev ? (i - 1 + n) % n : (i + 1) % n;
        raster_line(c->r, PT(p[2 * i]), PT(p[2 * i + 1]), PT(p[2 * j]), PT(p[2 * j + 1]));
    }
}
static void ic_circle_r(IC *c, float cx, float cy, float rad, bool hole) {
    float p[64];
    for (int i = 0; i < 32; i++) {
        float a = i * 6.2831853f / 32;
        p[2 * i] = cx + rad * f_sin(a + 1.5707963f); p[2 * i + 1] = cy + rad * f_sin(a);
    }
    ic_poly(c, p, 32, hole);
}
static void ic_circle(IC *c, float cx, float cy, float r) { ic_circle_r(c, cx, cy, r, false); }
static void ic_ring(IC *c, float cx, float cy, float r) {
    ic_circle_r(c, cx, cy, r + c->w * 0.5f, false);
    ic_circle_r(c, cx, cy, r - c->w * 0.5f, true);
}
static void ic_seg(IC *c, float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0, l = f_sqrt(dx * dx + dy * dy);
    if (l < 0.001f) return;
    float nx = -dy / l * c->w * 0.5f, ny = dx / l * c->w * 0.5f;
    float q[8] = { x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny };
    ic_poly(c, q, 4, false);
}
static void ic_stroke(IC *c, const float *p, int n, bool closed) {
    for (int i = 0; i + 1 < n; i++) ic_seg(c, p[2 * i], p[2 * i + 1], p[2 * i + 2], p[2 * i + 3]);
    if (closed) ic_seg(c, p[2 * n - 2], p[2 * n - 1], p[0], p[1]);
    for (int i = 0; i < n; i++) ic_circle(c, p[2 * i], p[2 * i + 1], c->w * 0.5f);
}
static void ic_line(IC *c, float x0, float y0, float x1, float y1) {
    float p[4] = { x0, y0, x1, y1 }; ic_stroke(c, p, 2, false);
}
static void ic_arc(IC *c, float cx, float cy, float r, float a0, float a1) {
    float p[80]; int n = 0;
    int steps = 24;
    for (int i = 0; i <= steps && n < 40; i++) {
        float a = (a0 + (a1 - a0) * i / steps) * 0.01745329f;
        p[2 * n] = cx + r * f_sin(a + 1.5707963f); p[2 * n + 1] = cy + r * f_sin(a); n++;
    }
    ic_stroke(c, p, n, false);
}
static void ic_rrect(IC *c, float x0, float y0, float x1, float y1) {
    float p[8] = { x0, y0, x1, y0, x1, y1, x0, y1 }; ic_stroke(c, p, 4, true);
}

static void build_icon(IC *c, int id) {
    switch (id) {
    case IC_MENU: ic_line(c, 4, 7, 20, 7); ic_line(c, 4, 12, 20, 12); ic_line(c, 4, 17, 20, 17); break;
    case IC_FILE: { float p[] = { 6, 3, 14, 3, 19, 8, 19, 21, 6, 21 }; ic_stroke(c, p, 5, true);
        float q[] = { 14, 3, 14, 8, 19, 8 }; ic_stroke(c, q, 3, false); } break;
    case IC_DOC: { float p[] = { 6, 3, 14, 3, 19, 8, 19, 21, 6, 21 }; ic_stroke(c, p, 5, true);
        ic_line(c, 9, 12, 16, 12); ic_line(c, 9, 16, 16, 16); } break;
    case IC_FOLDER: { float p[] = { 3, 6, 9, 6, 11, 8.5f, 21, 8.5f, 21, 19, 3, 19 }; ic_stroke(c, p, 6, true); } break;
    case IC_SAVE: { ic_rrect(c, 4, 4, 20, 20); float p[] = { 8, 4, 8, 9, 15, 9, 15, 4 }; ic_stroke(c, p, 4, false);
        float q[] = { 7.5f, 20, 7.5f, 14, 16.5f, 14, 16.5f, 20 }; ic_stroke(c, q, 4, false); } break;
    case IC_EXPORT: { float p[] = { 4, 14, 4, 20, 20, 20, 20, 14 }; ic_stroke(c, p, 4, false);
        ic_line(c, 12, 3.5f, 12, 15); float q[] = { 7, 8.5f, 12, 3.5f, 17, 8.5f }; ic_stroke(c, q, 3, false); } break;
    case IC_EYE: { float p[34]; int n = 0;
        for (int i = 0; i <= 8; i++) { float t = i / 8.0f; p[2*n] = 2.5f + 19 * t; p[2*n+1] = 12 - 7.5f * 4 * t * (1 - t); n++; }
        for (int i = 7; i >= 1; i--) { float t = i / 8.0f; p[2*n] = 2.5f + 19 * t; p[2*n+1] = 12 + 7.5f * 4 * t * (1 - t); n++; }
        ic_stroke(c, p, n, true); ic_circle(c, 12, 12, 3); } break;
    case IC_FOCUS: {
        float a[] = { 4, 9, 4, 4, 9, 4 }, b[] = { 15, 4, 20, 4, 20, 9 }, d[] = { 20, 15, 20, 20, 15, 20 }, e[] = { 9, 20, 4, 20, 4, 15 };
        ic_stroke(c, a, 3, false); ic_stroke(c, b, 3, false); ic_stroke(c, d, 3, false); ic_stroke(c, e, 3, false);
        ic_circle(c, 12, 12, 2.6f); } break;
    case IC_PALETTE: ic_ring(c, 12, 12, 8.5f); ic_circle(c, 8.5f, 9.5f, 1.6f); ic_circle(c, 13, 7.5f, 1.6f);
        ic_circle(c, 16.5f, 11, 1.6f); ic_circle(c, 9.5f, 15, 1.6f); break;
    case IC_MUSIC: { float p[] = { 9, 18, 9, 6, 19, 4, 19, 16 }; ic_stroke(c, p, 4, false);
        ic_circle(c, 6.5f, 18, 2.8f); ic_circle(c, 16.5f, 16, 2.8f); ic_line(c, 9, 9.5f, 19, 7.5f); } break;
    case IC_SETTINGS: ic_line(c, 4, 7, 20, 7); ic_line(c, 4, 12, 20, 12); ic_line(c, 4, 17, 20, 17);
        ic_circle(c, 9, 7, 2.6f); ic_circle(c, 15.5f, 12, 2.6f); ic_circle(c, 7.5f, 17, 2.6f); break;
    case IC_CLOSE: ic_line(c, 6.5f, 6.5f, 17.5f, 17.5f); ic_line(c, 17.5f, 6.5f, 6.5f, 17.5f); break;
    case IC_PLAY: { float p[] = { 7.5f, 4.5f, 19.5f, 12, 7.5f, 19.5f }; ic_poly(c, p, 3, false); ic_stroke(c, p, 3, true); } break;
    case IC_PAUSE: { float a[] = { 7, 5, 10, 5, 10, 19, 7, 19 }, b[] = { 14, 5, 17, 5, 17, 19, 14, 19 };
        ic_poly(c, a, 4, false); ic_poly(c, b, 4, false); } break;
    case IC_NEXT: { float p[] = { 6, 6, 15, 12, 6, 18 }; ic_poly(c, p, 3, false); ic_stroke(c, p, 3, true); ic_line(c, 18, 6, 18, 18); } break;
    case IC_PREV: { float p[] = { 18, 6, 9, 12, 18, 18 }; ic_poly(c, p, 3, false); ic_stroke(c, p, 3, true); ic_line(c, 6, 6, 6, 18); } break;
    case IC_SEARCH: ic_ring(c, 10.5f, 10.5f, 6); ic_line(c, 15, 15, 20, 20); break;
    case IC_LIST: ic_circle(c, 5, 7, 1.5f); ic_circle(c, 5, 12, 1.5f); ic_circle(c, 5, 17, 1.5f);
        ic_line(c, 9, 7, 20, 7); ic_line(c, 9, 12, 20, 12); ic_line(c, 9, 17, 17, 17); break;
    case IC_TIMER: ic_ring(c, 12, 13.5f, 7.5f); ic_line(c, 12, 13.5f, 12, 9.5f); ic_line(c, 10, 3, 14, 3); ic_line(c, 12, 3, 12, 6); break;
    case IC_UP: ic_line(c, 12, 19, 12, 5); { float p[] = { 6, 11, 12, 5, 18, 11 }; ic_stroke(c, p, 3, false); } break;
    case IC_BACK: { float p[] = { 15, 5, 8, 12, 15, 19 }; ic_stroke(c, p, 3, false); } break;
    case IC_FWD: { float p[] = { 9, 5, 16, 12, 9, 19 }; ic_stroke(c, p, 3, false); } break;
    case IC_CHEVRON: { float p[] = { 6, 9, 12, 15, 18, 9 }; ic_stroke(c, p, 3, false); } break;
    case IC_PLUS: ic_line(c, 12, 5, 12, 19); ic_line(c, 5, 12, 19, 12); break;
    case IC_TRASH: ic_line(c, 4, 6.5f, 20, 6.5f); { float h[] = { 9, 6.5f, 9, 3.5f, 15, 3.5f, 15, 6.5f }; ic_stroke(c, h, 4, false);
        float b[] = { 6, 6.5f, 7, 20.5f, 17, 20.5f, 18, 6.5f }; ic_stroke(c, b, 4, false); ic_line(c, 10, 10, 10, 17); ic_line(c, 14, 10, 14, 17); } break;
    case IC_EDIT: { float p[] = { 5, 19, 5.5f, 15, 15.5f, 5, 19, 8.5f, 9, 18.5f }; ic_stroke(c, p, 5, true); ic_line(c, 13, 7.5f, 16.5f, 11); } break;
    case IC_DRIVE: ic_rrect(c, 3, 8, 21, 17); ic_circle(c, 17, 12.5f, 1.5f); ic_line(c, 6, 12.5f, 12, 12.5f); break;
    case IC_CHECK: { float p[] = { 5, 12.5f, 10, 17.5f, 19.5f, 7 }; ic_stroke(c, p, 3, false); } break;
    case IC_SHUFFLE: { float a[] = { 4, 7, 8.5f, 7, 15.5f, 17, 20, 17 }, b[] = { 4, 17, 8.5f, 17, 15.5f, 7, 20, 7 };
        ic_stroke(c, a, 4, false); ic_stroke(c, b, 4, false);
        float h1[] = { 17, 4, 20, 7, 17, 10 }, h2[] = { 17, 14, 20, 17, 17, 20 }; ic_stroke(c, h1, 3, false); ic_stroke(c, h2, 3, false); } break;
    case IC_REPEAT: { float a[] = { 4, 12, 4, 7, 19, 7 }, b[] = { 20, 12, 20, 17, 5, 17 };
        ic_stroke(c, a, 3, false); ic_stroke(c, b, 3, false);
        float h1[] = { 16, 4, 19, 7, 16, 10 }, h2[] = { 8, 14, 5, 17, 8, 20 }; ic_stroke(c, h1, 3, false); ic_stroke(c, h2, 3, false); } break;
    case IC_VOLUME: { float p[] = { 3.5f, 9, 7.5f, 9, 12.5f, 4.5f, 12.5f, 19.5f, 7.5f, 15, 3.5f, 15 }; ic_poly(c, p, 6, false);
        ic_arc(c, 13, 12, 4, -45, 45); ic_arc(c, 13, 12, 7.5f, -50, 50); } break;
    case IC_HELP: ic_ring(c, 12, 12, 9); ic_arc(c, 12, 9.5f, 3, 180, 405); ic_line(c, 12.0f + 2.1f, 11.6f, 12, 14); ic_circle(c, 12, 17.2f, 1.3f); break;
    case IC_POWER: ic_arc(c, 12, 13, 7.5f, -50, 230); ic_line(c, 12, 3.5f, 12, 11); break;
    case IC_TYPEWRITER: ic_line(c, 4, 6, 20, 6); ic_line(c, 7, 10, 17, 10); ic_line(c, 4, 14, 20, 14); ic_line(c, 7, 18, 17, 18); break;
    case IC_CHART: ic_line(c, 5, 20, 5, 12); ic_line(c, 10, 20, 10, 6); ic_line(c, 15, 20, 15, 10); ic_line(c, 20, 20, 20, 4); break;
    case IC_COMMAND: { float p[] = { 5, 7, 10, 12, 5, 17 }; ic_stroke(c, p, 3, false); ic_line(c, 12.5f, 17, 19, 17); } break;
    case IC_HOME: { float p[] = { 4, 11, 12, 4, 20, 11 }; ic_stroke(c, p, 3, false);
        float q[] = { 6.5f, 9.5f, 6.5f, 20, 17.5f, 20, 17.5f, 9.5f }; ic_stroke(c, q, 4, false); } break;
    case IC_MOON: { float p[96]; int n = 0;
        for (int i = 0; i <= 24; i++) { float a = (40 + 280.0f * i / 24) * 0.01745329f; p[2*n] = 12 + 8 * f_sin(a + 1.5707963f); p[2*n+1] = 12 + 8 * f_sin(a); n++; }
        for (int i = 0; i <= 20; i++) { float a = (300 - 240.0f * i / 20) * 0.01745329f; p[2*n] = 16 + 6.2f * f_sin(a + 1.5707963f); p[2*n+1] = 10 + 6.2f * f_sin(a); n++; }
        ic_poly(c, p, n, false); } break;
    case IC_STAR: { float p[20];
        for (int i = 0; i < 10; i++) { float r = (i & 1) ? 3.8f : 9; float a = (-90 + 36.0f * i) * 0.01745329f;
            p[2*i] = 12 + r * f_sin(a + 1.5707963f); p[2*i+1] = 12.5f + r * f_sin(a); }
        ic_poly(c, p, 10, false); } break;
    case IC_TABS: { /* duas abas sobre uma página */
        float a[] = { 3, 20, 3, 6, 10, 6, 10, 9.5f, 21, 9.5f, 21, 20 }; ic_stroke(c, a, 6, true);
        ic_line(c, 12.5f, 6, 18.5f, 6); ic_line(c, 18.5f, 6, 18.5f, 9.5f); } break;
    default: ic_ring(c, 12, 12, 6); break;
    }
}

typedef struct { int id, size; u8 *m; int w; } IconCache;
static IconCache icache[160];
static int nicache;

static const u8 *icon_mask(int id, int size, int *w) {
    for (int i = 0; i < nicache; i++) if (icache[i].id == id && icache[i].size == size) { *w = icache[i].w; return icache[i].m; }
    int pad = 2, W = size + pad * 2;
    Raster r; raster_begin(&r, W, W);
    IC c = { &r, size / 24.0f, 1.75f, (float)pad };
    if (size <= 16) c.w = 2.0f;
    build_icon(&c, id);
    u8 *m = raster_end(&r);
    if (nicache >= COUNT(icache)) { for (int i = 0; i < nicache; i++) mem_free(icache[i].m); nicache = 0; }
    icache[nicache].id = id; icache[nicache].size = size; icache[nicache].m = m; icache[nicache].w = W; nicache++;
    *w = W;
    return m;
}

void icon_draw(int id, float cx, float cy, float size, Color col) {
    int s = (int)(size + 0.5f), w;
    const u8 *m = icon_mask(id, s, &w);
    int x = (int)f_round(cx - w * 0.5f), y = (int)f_round(cy - w * 0.5f);
    gfx_mask(x, y, m, w, w, w, col);
}

/* ------------------------------------------------------------ cursores */
static u8 *cur_fill[3], *cur_line[3]; static int cur_w[3]; static float cur_scale[3];

static void build_cursor(int kind, float sc) {
    int W = (int)(26 * sc) + 4;
    for (int pass = 0; pass < 2; pass++) {
        Raster r; raster_begin(&r, W, W);
        IC c = { &r, sc, pass == 0 ? 0 : 1.6f, 2 };
        if (kind == 0) {
            float p[] = { 1, 1, 1, 17.5f, 5.2f, 13.6f, 8, 19.8f, 10.8f, 18.6f, 8.1f, 12.5f, 13.6f, 12.5f };
            if (pass == 0) ic_poly(&c, p, 7, false); else ic_stroke(&c, p, 7, true);
        } else if (kind == 1) {
            float p[] = { 7, 2, 9, 3.2f, 11, 2 }, q[] = { 7, 20, 9, 18.8f, 11, 20 };
            if (pass == 0) { c.w = 1.6f; ic_line(&c, 9, 3, 9, 19); ic_stroke(&c, p, 3, false); ic_stroke(&c, q, 3, false); }
            else { c.w = 4.2f; ic_line(&c, 9, 3, 9, 19); ic_stroke(&c, p, 3, false); ic_stroke(&c, q, 3, false); }
        } else {
            float p[] = { 6, 9, 6, 2.5f, 8.8f, 2.5f, 8.8f, 9, 15.5f, 10.5f, 15.5f, 16, 13, 20.5f, 7.5f, 20.5f, 3.5f, 14, 3.5f, 10.5f };
            if (pass == 0) ic_poly(&c, p, 10, false); else ic_stroke(&c, p, 10, true);
        }
        u8 *m = raster_end(&r);
        if (pass == 0) { mem_free(cur_fill[kind]); cur_fill[kind] = m; } else { mem_free(cur_line[kind]); cur_line[kind] = m; }
    }
    cur_w[kind] = W; cur_scale[kind] = sc;
}

void cursor_draw(int kind, int x, int y, float sc) {
    kind = CLAMP(kind, 0, 2);
    if (!cur_fill[kind] || cur_scale[kind] != sc) build_cursor(kind, sc);
    int W = cur_w[kind];
    int ox = kind == 1 ? (int)(9 * sc) + 2 : kind == 2 ? (int)(7.4f * sc) + 2 : 3;
    int oy = kind == 1 ? (int)(11 * sc) + 2 : kind == 2 ? 4 : 3;
    if (kind == 1) {
        gfx_mask(x - ox, y - oy, cur_line[kind], W, W, W, 0xE0FFFFFF);
        gfx_mask(x - ox, y - oy, cur_fill[kind], W, W, W, 0xFF101010);
    } else {
        gfx_mask(x - ox, y - oy, cur_line[kind], W, W, W, 0xFFFFFFFF);
        gfx_mask(x - ox, y - oy, cur_fill[kind], W, W, W, 0xFF111111);
        gfx_mask(x - ox, y - oy, cur_line[kind], W, W, W, 0xF0FFFFFF);
    }
}
