/* FOLIO v4 — gfx: primitivas 2D anti-aliased sobre um framebuffer 32-bit */
#include "folio.h"

static Surface *tgt;
static Rect clip, clip_stack[16];
static int nclip;
static float opacity = 1.0f;

void gfx_target(Surface *s) { tgt = s; clip = rect(0, 0, s->w, s->h); nclip = 0; opacity = 1; }
Surface *gfx_surface(void) { return tgt; }
void gfx_clip(Rect r) { clip = rect_clip(r, rect(0, 0, tgt->w, tgt->h)); }
Rect gfx_get_clip(void) { return clip; }
void gfx_push_clip(Rect r) { if (nclip < 16) clip_stack[nclip++] = clip; clip = rect_clip(clip, r); }
void gfx_pop_clip(void) { if (nclip) clip = clip_stack[--nclip]; }
void gfx_set_opacity(float a) { opacity = CLAMP(a, 0.0f, 1.0f); }

static inline u32 blend(u32 d, u32 s, u32 a) {
    u32 rb = (s & 0xFF00FF) * a + (d & 0xFF00FF) * (255 - a) + 0x800080;
    rb = ((rb + ((rb >> 8) & 0xFF00FF)) >> 8) & 0xFF00FF;
    u32 g = (s & 0x00FF00) * a + (d & 0x00FF00) * (255 - a) + 0x008000;
    g = ((g + ((g >> 8) & 0x00FF00)) >> 8) & 0x00FF00;
    return rb | g;
}
static inline u32 eff_alpha(Color c) { return (u32)((float)(c >> 24) * opacity + 0.5f); }

Color col_mix(Color a, Color b, float t) {
    t = CLAMP(t, 0.0f, 1.0f);
    u32 k = (u32)(t * 255 + 0.5f);
    u32 rgb = blend(a, b, k);
    u32 aa = (u32)((float)(a >> 24) * (1 - t) + (float)(b >> 24) * t + 0.5f);
    return (aa << 24) | rgb;
}
Color col_alpha(Color c, float a) { u32 v = (u32)((float)(c >> 24) * CLAMP(a, 0.0f, 1.0f) + 0.5f); return (c & 0xFFFFFF) | (v << 24); }
float col_luma(Color c) { return (0.299f * ((c >> 16) & 255) + 0.587f * ((c >> 8) & 255) + 0.114f * (c & 255)) / 255.0f; }
Color col_lum_shift(Color c, float amt) {
    return amt >= 0 ? col_mix(c, (c & 0xFF000000) | 0xFFFFFF, amt) : col_mix(c, c & 0xFF000000, -amt);
}

static inline void put(int x, int y, u32 c, u32 a) {
    u32 *p = &tgt->px[y * tgt->stride + x];
    *p = a >= 255 ? (c & 0xFFFFFF) : blend(*p, c, a);
}

static void span(int x0, int x1, int y, u32 c, u32 a) {
    if (y < clip.y || y >= clip.y + clip.h) return;
    x0 = MAX(x0, clip.x); x1 = MIN(x1, clip.x + clip.w);
    if (x0 >= x1 || a == 0) return;
    u32 *p = &tgt->px[y * tgt->stride + x0];
    int n = x1 - x0;
    if (a >= 255) { u32 v = c & 0xFFFFFF; while (n--) *p++ = v; }
    else while (n--) { *p = blend(*p, c, a); p++; }
}

void gfx_fill(Color c) { gfx_rect(clip, c); }

void gfx_rect(Rect r, Color c) {
    u32 a = eff_alpha(c);
    if (!a) return;
    Rect k = rect_clip(r, clip);
    for (int y = k.y; y < k.y + k.h; y++) span(k.x, k.x + k.w, y, c, a);
}

/* distância com sinal a um retângulo arredondado centrado (cx,cy) meia-extensão (hw,hh) */
static inline float sd_rrect(float px, float py, float cx, float cy, float hw, float hh, float r) {
    float qx = f_abs(px - cx) - (hw - r), qy = f_abs(py - cy) - (hh - r);
    float mx = MAX(qx, 0.0f), my = MAX(qy, 0.0f);
    float outside = f_sqrt(mx * mx + my * my);
    float inside = MIN(MAX(qx, qy), 0.0f);
    return outside + inside - r;
}

void gfx_rrect(Rect r, float rad, Color c) {
    u32 A = eff_alpha(c);
    if (!A || r.w <= 0 || r.h <= 0) return;
    rad = MIN(rad, MIN(r.w, r.h) * 0.5f);
    if (rad < 0.5f) { gfx_rect(r, c); return; }
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, hw = r.w * 0.5f, hh = r.h * 0.5f;
    int ir = (int)f_ceil(rad);
    Rect k = rect_clip(r, clip);
    for (int y = k.y; y < k.y + k.h; y++) {
        bool corner_row = (y < r.y + ir) || (y >= r.y + r.h - ir);
        if (!corner_row) { span(k.x, k.x + k.w, y, c, A); continue; }
        int xl = MAX(k.x, r.x), xr = MIN(k.x + k.w, r.x + r.w);
        int inner0 = r.x + ir, inner1 = r.x + r.w - ir;
        for (int x = xl; x < xr; x++) {
            if (x >= inner0 && x < inner1) { span(MAX(x, xl), MIN(inner1, xr), y, c, A); x = inner1 - 1; continue; }
            float d = sd_rrect(x + 0.5f, y + 0.5f, cx, cy, hw, hh, rad);
            float cov = CLAMP(0.5f - d, 0.0f, 1.0f);
            if (cov > 0) put(x, y, c, (u32)(cov * A + 0.5f));
        }
    }
}

void gfx_rrect_line(Rect r, float rad, float th, Color c) {
    u32 A = eff_alpha(c);
    if (!A || r.w <= 0 || r.h <= 0) return;
    rad = MIN(rad, MIN(r.w, r.h) * 0.5f);
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, hw = r.w * 0.5f, hh = r.h * 0.5f;
    Rect k = rect_clip(r, clip);
    int band = (int)f_ceil(MAX(rad, th)) + 1;
    for (int y = k.y; y < k.y + k.h; y++) {
        bool edge_row = (y < r.y + band) || (y >= r.y + r.h - band);
        for (int x = k.x; x < k.x + k.w; x++) {
            if (!edge_row && x >= r.x + band && x < r.x + r.w - band) { x = r.x + r.w - band - 1; continue; }
            float d = sd_rrect(x + 0.5f, y + 0.5f, cx, cy, hw, hh, rad);
            float cov = CLAMP(0.5f - d, 0.0f, 1.0f) - CLAMP(0.5f - (d + th), 0.0f, 1.0f);
            if (cov > 0.003f) put(x, y, c, (u32)(cov * A + 0.5f));
        }
    }
}

static inline u32 shadow_alpha(float d, float blur, u32 A) {
    float t = d <= 0 ? 0 : d / blur;
    if (t >= 1) return 0;
    float a = 1 - t; a = a * a * (3 - 2 * a); a *= a;
    return a > 0.002f ? (u32)(a * A + 0.5f) : 0;
}
/* sombra suave: a distância só é calculada pixel a pixel nos cantos; nas faixas laterais o perfil é o
 * mesmo em todas as linhas (calculado uma vez) e nas faixas de cima/baixo é constante na linha */
void gfx_shadow(Rect r, float rad, float blur, Color c) {
    u32 A = eff_alpha(c);
    if (!A) return;
    int b = (int)f_ceil(blur);
    Rect o = rect(r.x - b, r.y - b, r.w + 2 * b, r.h + 2 * b);
    Rect k = rect_clip(o, clip);
    if (k.w <= 0 || k.h <= 0) return;
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, hw = r.w * 0.5f, hh = r.h * 0.5f;
    rad = MIN(rad, MIN(r.w, r.h) * 0.5f);
    int in0 = r.x + (int)rad + 1, in1 = r.x + r.w - (int)rad - 1;
    int c0 = CLAMP(in0, k.x, k.x + k.w), c1 = CLAMP(in1, c0, k.x + k.w);   /* colunas "do meio" visíveis */
    static u8 prof[8192];
    int pw = MIN(k.w, 8192);
    for (int i = 0; i < pw; i++) prof[i] = (u8)shadow_alpha(sd_rrect(k.x + i + 0.5f, cy, cx, cy, hw, hh, rad), blur, A);
    for (int y = k.y; y < k.y + k.h; y++) {
        bool mid = y > r.y + rad && y < r.y + r.h - rad;
        if (mid) {
            for (int x = k.x; x < c0; x++) { u32 a = x - k.x < pw ? prof[x - k.x] : 0; if (a) put(x, y, c, a); }
            span(c0, c1, y, c, A);
            for (int x = c1; x < k.x + k.w; x++) { u32 a = x - k.x < pw ? prof[x - k.x] : 0; if (a) put(x, y, c, a); }
            continue;
        }
        float py = y + 0.5f;
        if (c1 > c0) { u32 a = shadow_alpha(sd_rrect(cx, py, cx, cy, hw, hh, rad), blur, A); if (a) span(c0, c1, y, c, a); }
        for (int x = k.x; x < c0; x++) { u32 a = shadow_alpha(sd_rrect(x + 0.5f, py, cx, cy, hw, hh, rad), blur, A); if (a) put(x, y, c, a); }
        for (int x = c1; x < k.x + k.w; x++) { u32 a = shadow_alpha(sd_rrect(x + 0.5f, py, cx, cy, hw, hh, rad), blur, A); if (a) put(x, y, c, a); }
    }
}

void gfx_gradient_v(Rect r, Color top, Color bot) {
    Rect k = rect_clip(r, clip);
    for (int y = k.y; y < k.y + k.h; y++) {
        Color c = col_mix(top, bot, r.h > 1 ? (float)(y - r.y) / (r.h - 1) : 0);
        span(k.x, k.x + k.w, y, c, eff_alpha(c));
    }
}

void gfx_vignette(Rect r, Color c, float strength) {
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    float inv = 1.0f / (cx * cx + cy * cy);
    Rect k = rect_clip(r, clip);
    for (int y = k.y; y < k.y + k.h; y++) {
        float dy = (y - cy) * (y - cy);
        for (int x = k.x; x < k.x + k.w; x++) {
            float d = ((x - cx) * (x - cx) + dy) * inv;
            static const u8 bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
            float a = d * d * strength * 255.0f + (bayer[((y & 3) << 2) | (x & 3)] + 0.5f) / 16.0f;   /* dithering contra faixas */
            u32 ia = (u32)MIN(a, 255.0f);
            if (ia) put(x, y, c, ia);
        }
    }
}

void gfx_mask(int x, int y, const u8 *m, int w, int h, int pitch, Color c) {
    u32 A = eff_alpha(c);
    if (!A || !m) return;
    Rect k = rect_clip(rect(x, y, w, h), clip);
    for (int yy = k.y; yy < k.y + k.h; yy++) {
        const u8 *row = m + (yy - y) * pitch + (k.x - x);
        u32 *p = &tgt->px[yy * tgt->stride + k.x];
        for (int xx = 0; xx < k.w; xx++) {
            u32 cov = row[xx];
            if (!cov) continue;
            u32 a = (cov * A + 127) / 255;
            p[xx] = a >= 255 ? (c & 0xFFFFFF) : blend(p[xx], c, a);
        }
    }
}

void gfx_circle(float cx, float cy, float rad, Color c) {
    u32 A = eff_alpha(c);
    Rect k = rect_clip(rect((int)(cx - rad - 1), (int)(cy - rad - 1), (int)(rad * 2 + 3), (int)(rad * 2 + 3)), clip);
    for (int y = k.y; y < k.y + k.h; y++)
        for (int x = k.x; x < k.x + k.w; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float cov = CLAMP(rad - f_sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            if (cov > 0) put(x, y, c, (u32)(cov * A + 0.5f));
        }
}

void gfx_ring(float cx, float cy, float rad, float th, Color c) {
    u32 A = eff_alpha(c);
    Rect k = rect_clip(rect((int)(cx - rad - 1), (int)(cy - rad - 1), (int)(rad * 2 + 3), (int)(rad * 2 + 3)), clip);
    for (int y = k.y; y < k.y + k.h; y++)
        for (int x = k.x; x < k.x + k.w; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = f_sqrt(dx * dx + dy * dy);
            float cov = CLAMP(rad - d + 0.5f, 0.0f, 1.0f) - CLAMP(rad - th - d + 0.5f, 0.0f, 1.0f);
            if (cov > 0) put(x, y, c, (u32)(cov * A + 0.5f));
        }
}

void gfx_line(float x0, float y0, float x1, float y1, float th, Color c) {
    float dx = x1 - x0, dy = y1 - y0, len = f_sqrt(dx * dx + dy * dy);
    if (len < 0.01f) return;
    float nx = -dy / len * th * 0.5f, ny = dx / len * th * 0.5f;
    float minx = MIN(MIN(x0, x1) - f_abs(nx), MIN(x0, x1) + f_abs(nx)) - 1;
    float miny = MIN(MIN(y0, y1) - f_abs(ny), MIN(y0, y1) + f_abs(ny)) - 1;
    float maxx = MAX(x0, x1) + f_abs(nx) + 1, maxy = MAX(y0, y1) + f_abs(ny) + 1;
    int bx = (int)f_floor(minx), by = (int)f_floor(miny);
    Raster rs; raster_begin(&rs, (int)(maxx - bx) + 2, (int)(maxy - by) + 2);
    float ax = x0 - bx, ay = y0 - by, cx2 = x1 - bx, cy2 = y1 - by;
    raster_line(&rs, ax + nx, ay + ny, cx2 + nx, cy2 + ny);
    raster_line(&rs, cx2 + nx, cy2 + ny, cx2 - nx, cy2 - ny);
    raster_line(&rs, cx2 - nx, cy2 - ny, ax - nx, ay - ny);
    raster_line(&rs, ax - nx, ay - ny, ax + nx, ay + ny);
    u8 *m = raster_end(&rs);
    gfx_mask(bx, by, m, rs.w, rs.h, rs.w, c);
    mem_free(m);
}

/* ---------------------------------------------------------------------------
 * Rasterizador de cobertura exata por área (acumulação com soma corrida).
 * ------------------------------------------------------------------------- */
void raster_begin(Raster *r, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    r->w = w; r->h = h;
    r->acc = mem_zalloc(sizeof(float) * ((usize)w * h + 4));
}

void raster_line(Raster *r, float x0, float y0, float x1, float y1) {
    if (y0 == y1) return;
    float dir = 1;
    if (y0 > y1) { float t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dir = -1; }
    float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    if (y0 < 0) { x -= y0 * dxdy; y0 = 0; }
    if (y1 > r->h) y1 = (float)r->h;
    if (y0 >= y1) return;
    float wmax = (float)r->w - 0.001f;
    int ystart = (int)y0, yend = (int)f_ceil(y1);
    for (int y = ystart; y < yend; y++) {
        int ls = y * r->w;
        float dy = MIN((float)(y + 1), y1) - MAX((float)y, y0);
        float xnext = x + dxdy * dy;
        float d = dy * dir;
        float a0x = CLAMP(x, 0.0f, wmax), a1x = CLAMP(xnext, 0.0f, wmax);
        float xa = MIN(a0x, a1x), xb = MAX(a0x, a1x);
        float xaf = f_floor(xa);
        int xai = (int)xaf;
        float xbc = f_ceil(xb);
        int xbi = (int)xbc;
        float *acc = r->acc + ls;
        if (xbi <= xai + 1) {
            float xmf = 0.5f * (a0x + a1x) - xaf;
            acc[xai] += d - d * xmf;
            acc[xai + 1] += d * xmf;
        } else {
            float s = 1.0f / (xb - xa);
            float x0f = xa - xaf;
            float a0 = 0.5f * s * (1 - x0f) * (1 - x0f);
            float x1f = xb - xbc + 1;
            float am = 0.5f * s * x1f * x1f;
            acc[xai] += d * a0;
            if (xbi == xai + 2) acc[xai + 1] += d * (1 - a0 - am);
            else {
                float a1 = s * (1.5f - x0f);
                acc[xai + 1] += d * (a1 - a0);
                for (int xi = xai + 2; xi < xbi - 1; xi++) acc[xi] += d * s;
                float a2 = a1 + (float)(xbi - xai - 3) * s;
                acc[xbi - 1] += d * (1 - a2 - am);
            }
            acc[xbi] += d * am;
        }
        x = xnext;
    }
}

void raster_quad(Raster *r, float x0, float y0, float x1, float y1, float x2, float y2) {
    float ddx = x0 - 2 * x1 + x2, ddy = y0 - 2 * y1 + y2;
    float dev = ddx * ddx + ddy * ddy;
    if (dev < 0.333f) { raster_line(r, x0, y0, x2, y2); return; }
    int n = 1 + (int)f_floor(f_sqrt(f_sqrt(3.0f * dev)));
    if (n > 32) n = 32;
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n, u = 1 - t;
        float qx = u * u * x0 + 2 * u * t * x1 + t * t * x2;
        float qy = u * u * y0 + 2 * u * t * y1 + t * t * y2;
        raster_line(r, px, py, qx, qy);
        px = qx; py = qy;
    }
}

void raster_cubic(Raster *r, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {
    float len = f_abs(x1 - x0) + f_abs(y1 - y0) + f_abs(x2 - x1) + f_abs(y2 - y1) + f_abs(x3 - x2) + f_abs(y3 - y2);
    int n = 2 + (int)(len / 3);
    if (n > 48) n = 48;
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n, u = 1 - t;
        float qx = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
        float qy = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
        raster_line(r, px, py, qx, qy);
        px = qx; py = qy;
    }
}

u8 *raster_end(Raster *r) {
    int n = r->w * r->h;
    u8 *out = mem_alloc((usize)n);
    float sum = 0;
    for (int i = 0; i < n; i++) {
        sum += r->acc[i];
        float v = f_abs(sum);
        if (v > 1) v = 1;
        out[i] = (u8)(v * 255.0f + 0.5f);
    }
    mem_free(r->acc);
    r->acc = 0;
    return out;
}

/* arco de progresso suave (0 = topo, sentido horário), com pontas arredondadas */
static float f_atan2(float y, float x) {
    float ax = f_abs(x), ay = f_abs(y), mx = MAX(ax, ay), mn = MIN(ax, ay);
    if (mx < 1e-9f) return 0;
    float a = mn / mx, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    if (y < 0) r = -r;
    return r;
}
void gfx_arc(float cx, float cy, float rad, float th, float frac, Color c) {
    frac = CLAMP(frac, 0.0f, 1.0f);
    if (frac <= 0) return;
    if (frac >= 1) { gfx_ring(cx, cy, rad, th, c); return; }
    u32 A = eff_alpha(c);
    float end = frac * 6.2831853f, mid = rad - th * 0.5f;
    float ex = cx + mid * f_sin(end), ey = cy - mid * f_sin(end + 1.5707963f);
    Rect k = rect_clip(rect((int)(cx - rad - 1), (int)(cy - rad - 1), (int)(rad * 2 + 3), (int)(rad * 2 + 3)), clip);
    for (int y = k.y; y < k.y + k.h; y++)
        for (int x = k.x; x < k.x + k.w; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = f_sqrt(dx * dx + dy * dy);
            float cov = CLAMP(rad - d + 0.5f, 0.0f, 1.0f) - CLAMP(rad - th - d + 0.5f, 0.0f, 1.0f);
            if (cov <= 0) continue;
            float ang = f_atan2(dx, -dy); if (ang < 0) ang += 6.2831853f;
            if (ang > end) {
                /* pontas arredondadas */
                float sx = x + 0.5f - cx, sy = y + 0.5f - (cy - mid);
                float qx = x + 0.5f - ex, qy = y + 0.5f - ey;
                float dc = MIN(f_sqrt(sx * sx + sy * sy), f_sqrt(qx * qx + qy * qy));
                cov = MIN(cov, CLAMP(th * 0.5f - dc + 0.5f, 0.0f, 1.0f));
                if (cov <= 0) continue;
            }
            put(x, y, c, (u32)(cov * A + 0.5f));
        }
}
