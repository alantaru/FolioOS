/* FOLIO v4 — font: leitor TrueType, kerning (GPOS/kern), cache de glifos, famílias */
#include "folio.h"

struct Face {
    const u8 *d; u32 len; bool owned;
    u32 cmap, loca, glyf, hmtx, kern, gpos, name, os2;
    int cmap_fmt, nhm, nglyphs, upem, loca_long;
    int asc, desc, gap, xh;
    int weight; bool italic, mono;
    bool synth_bold, synth_italic;
    struct Face *base;
    u32 *kern_subs; int nkern_subs;    /* subtabelas GPOS PairPos (offsets absolutos) */
    u32 *kcache;                        /* cache direto: chave (a<<16|b) -> valor */
    i16 *kval;
    u16 *cp_cache;                      /* cp < 0x2100 -> gid+1 */
};

static inline u32 rd8(const Face *f, u32 o) { return o < f->len ? f->d[o] : 0; }
static inline u32 rd16(const Face *f, u32 o) { return o + 1 < f->len ? ((u32)f->d[o] << 8) | f->d[o + 1] : 0; }
static inline i32 rds16(const Face *f, u32 o) { return (i16)rd16(f, o); }
static inline u32 rd32(const Face *f, u32 o) { return o + 3 < f->len ? ((u32)f->d[o] << 24) | ((u32)f->d[o + 1] << 16) | ((u32)f->d[o + 2] << 8) | f->d[o + 3] : 0; }

static u32 find_table(Face *f, const char *tag) {
    u32 n = rd16(f, 4);
    for (u32 i = 0; i < n; i++) {
        u32 r = 12 + 16 * i;
        if (f->d[r] == (u8)tag[0] && f->d[r + 1] == (u8)tag[1] && f->d[r + 2] == (u8)tag[2] && f->d[r + 3] == (u8)tag[3])
            return rd32(f, r + 8);
    }
    return 0;
}

/* ------------------------------------------------------------ GPOS kerning */
static int coverage_index(Face *f, u32 cov, u16 g) {
    u32 fmtc = rd16(f, cov);
    if (fmtc == 1) {
        int lo = 0, hi = (int)rd16(f, cov + 2) - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1; u32 v = rd16(f, cov + 4 + 2 * (u32)mid);
            if (v == g) return mid;
            if (v < g) lo = mid + 1; else hi = mid - 1;
        }
    } else if (fmtc == 2) {
        int lo = 0, hi = (int)rd16(f, cov + 2) - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1; u32 r = cov + 4 + 6 * (u32)mid;
            u32 s = rd16(f, r), e = rd16(f, r + 2);
            if (g < s) hi = mid - 1; else if (g > e) lo = mid + 1;
            else return (int)(rd16(f, r + 4) + (g - s));
        }
    }
    return -1;
}
static int class_of(Face *f, u32 cd, u16 g) {
    u32 fm = rd16(f, cd);
    if (fm == 1) {
        u32 start = rd16(f, cd + 2), cnt = rd16(f, cd + 4);
        if (g >= start && g < start + cnt) return (int)rd16(f, cd + 6 + 2 * (g - start));
    } else if (fm == 2) {
        int lo = 0, hi = (int)rd16(f, cd + 2) - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1; u32 r = cd + 4 + 6 * (u32)mid;
            u32 s = rd16(f, r), e = rd16(f, r + 2);
            if (g < s) hi = mid - 1; else if (g > e) lo = mid + 1; else return (int)rd16(f, r + 4);
        }
    }
    return 0;
}
static int popcnt(u32 v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

static void gpos_init(Face *f) {
    u32 g = f->gpos; if (!g) return;
    u32 flist = g + rd16(f, g + 6), llist = g + rd16(f, g + 8);
    u32 nfeat = rd16(f, flist);
    u16 lookups[64]; int nl = 0;
    for (u32 i = 0; i < nfeat && i < 512; i++) {
        u32 r = flist + 2 + 6 * i;
        if (f->d[r] != 'k' || f->d[r + 1] != 'e' || f->d[r + 2] != 'r' || f->d[r + 3] != 'n') continue;
        u32 ft = flist + rd16(f, r + 4);
        u32 cnt = rd16(f, ft + 2);
        for (u32 k = 0; k < cnt; k++) {
            u16 li = (u16)rd16(f, ft + 4 + 2 * k);
            bool dup = false;
            for (int q = 0; q < nl; q++) if (lookups[q] == li) dup = true;
            if (!dup && nl < 64) lookups[nl++] = li;
        }
    }
    u32 subs[256]; int ns = 0;
    for (int i = 0; i < nl; i++) {
        u32 lk = llist + rd16(f, llist + 2 + 2 * (u32)lookups[i]);
        u32 type = rd16(f, lk), cnt = rd16(f, lk + 4);
        for (u32 k = 0; k < cnt && ns < 256; k++) {
            u32 st = lk + rd16(f, lk + 6 + 2 * k);
            u32 t = type;
            if (t == 9) { t = rd16(f, st + 2); st = st + rd32(f, st + 4); }
            if (t == 2) subs[ns++] = st;
        }
    }
    if (ns) {
        f->kern_subs = mem_alloc(sizeof(u32) * (usize)ns);
        memcpy(f->kern_subs, subs, sizeof(u32) * (usize)ns);
        f->nkern_subs = ns;
    }
}

static int gpos_kern(Face *f, u16 a, u16 b) {
    for (int i = 0; i < f->nkern_subs; i++) {
        u32 st = f->kern_subs[i];
        u32 fm = rd16(f, st), cov = st + rd16(f, st + 2);
        u32 vf1 = rd16(f, st + 4), vf2 = rd16(f, st + 6);
        int ci = coverage_index(f, cov, a);
        if (ci < 0) continue;
        u32 xoff = (vf1 & 4) ? 2 * (u32)popcnt(vf1 & 3) : 0xFFFF;
        u32 s1 = 2 * (u32)popcnt(vf1 & 0xFF), s2 = 2 * (u32)popcnt(vf2 & 0xFF);
        if (fm == 1) {
            if ((u32)ci >= rd16(f, st + 8)) continue;
            u32 ps = st + rd16(f, st + 10 + 2 * (u32)ci);
            int lo = 0, hi = (int)rd16(f, ps) - 1;
            u32 rs = 2 + s1 + s2;
            while (lo <= hi) {
                int mid = (lo + hi) >> 1; u32 r = ps + 2 + rs * (u32)mid;
                u32 sg = rd16(f, r);
                if (sg == b) return xoff == 0xFFFF ? 0 : rds16(f, r + 2 + xoff);
                if (sg < b) lo = mid + 1; else hi = mid - 1;
            }
        } else if (fm == 2) {
            u32 cd1 = st + rd16(f, st + 8), cd2 = st + rd16(f, st + 10);
            u32 c1n = rd16(f, st + 12), c2n = rd16(f, st + 14);
            u32 c1 = (u32)class_of(f, cd1, a), c2 = (u32)class_of(f, cd2, b);
            if (c1 >= c1n || c2 >= c2n) continue;
            u32 r = st + 16 + (c1 * c2n + c2) * (s1 + s2);
            return xoff == 0xFFFF ? 0 : rds16(f, r + xoff);
        }
    }
    return 0;
}

static int kern_table(Face *f, u16 a, u16 b) {
    u32 k = f->kern; if (!k || rd16(f, k) != 0) return 0;
    u32 n = rd16(f, k + 2), off = k + 4;
    for (u32 t = 0; t < n; t++) {
        u32 len = rd16(f, off + 2), cov = rd16(f, off + 4);
        if ((cov >> 8) == 0 && (cov & 1)) {
            u32 np = rd16(f, off + 6);
            u32 key = ((u32)a << 16) | b;
            int lo = 0, hi = (int)np - 1;
            while (lo <= hi) {
                int mid = (lo + hi) >> 1; u32 r = off + 14 + 6 * (u32)mid;
                u32 v = rd32(f, r);
                if (v == key) return rds16(f, r + 4);
                if (v < key) lo = mid + 1; else hi = mid - 1;
            }
        }
        off += len;
    }
    return 0;
}

/* ------------------------------------------------------------ carregamento */
Face *face_load(const u8 *data, usize len, bool owned) {
    if (!data || len < 12) return 0;
    Face *f = mem_zalloc(sizeof(Face));
    f->d = data; f->len = (u32)len; f->owned = owned;
    u32 ver = rd32(f, 0);
    if (ver != 0x00010000 && ver != 0x74727565) { mem_free(f); return 0; }  /* sem CFF */
    u32 head = find_table(f, "head"), hhea = find_table(f, "hhea"), maxp = find_table(f, "maxp");
    f->cmap = find_table(f, "cmap"); f->loca = find_table(f, "loca"); f->glyf = find_table(f, "glyf");
    f->hmtx = find_table(f, "hmtx"); f->kern = find_table(f, "kern"); f->gpos = find_table(f, "GPOS");
    f->name = find_table(f, "name"); f->os2 = find_table(f, "OS/2");
    if (!head || !hhea || !maxp || !f->cmap || !f->loca || !f->glyf || !f->hmtx) { mem_free(f); return 0; }
    f->upem = (int)rd16(f, head + 18); if (f->upem <= 0) f->upem = 1000;
    f->loca_long = rds16(f, head + 50);
    f->nglyphs = (int)rd16(f, maxp + 4);
    f->asc = rds16(f, hhea + 4); f->desc = -rds16(f, hhea + 6); f->gap = rds16(f, hhea + 8);
    f->nhm = (int)rd16(f, hhea + 34);
    f->weight = 400; f->xh = f->upem / 2;
    if (f->os2) {
        f->weight = (int)rd16(f, f->os2 + 4);
        u32 sel = rd16(f, f->os2 + 62);
        f->italic = (sel & 1) != 0;
        if (rd16(f, f->os2) >= 2) { int xh = rds16(f, f->os2 + 86); if (xh > 0) f->xh = xh; }
        if ((sel & 0x80) || f->asc <= 0) {
            f->asc = rds16(f, f->os2 + 68); f->desc = -rds16(f, f->os2 + 70); f->gap = rds16(f, f->os2 + 72);
        }
    }
    u32 post = find_table(f, "post");
    if (post) f->mono = rd32(f, post + 12) != 0;
    /* subtabela cmap: prefere Unicode completo (formato 12) */
    u32 n = rd16(f, f->cmap + 2), best = 0; int bestfmt = 0, bestscore = -1;
    for (u32 i = 0; i < n; i++) {
        u32 r = f->cmap + 4 + 8 * i;
        u32 pid = rd16(f, r), eid = rd16(f, r + 2), off = f->cmap + rd32(f, r + 4);
        int fm = (int)rd16(f, off), sc = -1;
        if (fm == 12 && (pid == 0 || (pid == 3 && eid == 10))) sc = 3;
        else if (fm == 4 && (pid == 0 || (pid == 3 && eid == 1))) sc = 2;
        if (sc > bestscore) { bestscore = sc; best = off; bestfmt = fm; }
    }
    if (bestscore < 0) { mem_free(f); return 0; }
    f->cmap = best; f->cmap_fmt = bestfmt;
    gpos_init(f);
    f->kcache = mem_alloc(sizeof(u32) * 4096);
    f->kval = mem_alloc(sizeof(i16) * 4096);
    for (int i = 0; i < 4096; i++) f->kcache[i] = 0xFFFFFFFFu;
    f->cp_cache = mem_zalloc(sizeof(u16) * 0x2100);
    return f;
}

Face *face_variant(Face *base, bool sb, bool si) {
    Face *f = mem_alloc(sizeof(Face));
    *f = *base;
    f->owned = false; f->synth_bold = sb; f->synth_italic = si; f->base = base;
    if (sb) f->weight = 700;
    if (si) f->italic = true;
    return f;
}

int  face_weight(Face *f) { return f->weight; }
bool face_is_italic(Face *f) { return f->italic; }
bool face_is_mono(Face *f) { return f->mono; }

static void name_str(Face *f, u32 rec, char *out, int cap) {
    u32 base = f->name + rd16(f, f->name + 4);
    u32 pid = rd16(f, rec), len = rd16(f, rec + 8), off = base + rd16(f, rec + 10);
    int o = 0; char tmp[4];
    if (pid == 3 || pid == 0) {
        for (u32 i = 0; i + 1 < len && o < cap - 4; i += 2) {
            u32 cp = rd16(f, off + i);
            int k = utf8_encode(cp, tmp);
            for (int j = 0; j < k; j++) out[o++] = tmp[j];
        }
    } else {
        for (u32 i = 0; i < len && o < cap - 4; i++) {
            int k = utf8_encode(rd8(f, off + i), tmp);
            for (int j = 0; j < k; j++) out[o++] = tmp[j];
        }
    }
    out[o] = 0;
}
void face_names(Face *f, char *fam, int fcap, char *sty, int scap) {
    fam[0] = 0; sty[0] = 0;
    if (!f->name) return;
    u32 cnt = rd16(f, f->name + 2);
    int fscore = -1, sscore = -1;
    for (u32 i = 0; i < cnt; i++) {
        u32 r = f->name + 6 + 12 * i;
        u32 pid = rd16(f, r), lang = rd16(f, r + 4), nid = rd16(f, r + 6);
        int sc = (pid == 3 ? 4 : pid == 0 ? 3 : 1) + (lang == 0x409 ? 2 : 0);
        if (nid == 16) sc += 10;
        if (nid == 17) sc += 10;
        if ((nid == 1 || nid == 16) && sc > fscore) { fscore = sc; name_str(f, r, fam, fcap); }
        if ((nid == 2 || nid == 17) && sc > sscore) { sscore = sc; name_str(f, r, sty, scap); }
    }
}

/* ------------------------------------------------------------ cmap */
static u16 cmap_lookup(Face *f, u32 cp) {
    u32 t = f->cmap;
    if (f->cmap_fmt == 4) {
        if (cp > 0xFFFF) return 0;
        u32 segx2 = rd16(f, t + 6);
        u32 ends = t + 14, starts = ends + segx2 + 2, deltas = starts + segx2, ranges = deltas + segx2;
        int lo = 0, hi = (int)(segx2 / 2) - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            u32 e = rd16(f, ends + 2 * (u32)mid);
            if (e < cp) { lo = mid + 1; continue; }
            u32 s = rd16(f, starts + 2 * (u32)mid);
            if (s > cp) { if (mid == 0) return 0; hi = mid - 1; continue; }
            u32 ro = rd16(f, ranges + 2 * (u32)mid), dl = rd16(f, deltas + 2 * (u32)mid);
            if (ro == 0) return (u16)((cp + dl) & 0xFFFF);
            u32 g = rd16(f, ranges + 2 * (u32)mid + ro + 2 * (cp - s));
            return g ? (u16)((g + dl) & 0xFFFF) : 0;
        }
        return 0;
    } else if (f->cmap_fmt == 12) {
        u32 ng = rd32(f, t + 12);
        int lo = 0, hi = (int)ng - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1; u32 r = t + 16 + 12 * (u32)mid;
            u32 s = rd32(f, r), e = rd32(f, r + 4);
            if (cp < s) hi = mid - 1; else if (cp > e) lo = mid + 1;
            else return (u16)(rd32(f, r + 8) + (cp - s));
        }
    }
    return 0;
}
u16 face_gid(Face *f, u32 cp) {
    if (cp < 0x2100) {
        u16 v = f->cp_cache[cp];
        if (v) return (u16)(v - 1);
        u16 g = cmap_lookup(f, cp);
        f->cp_cache[cp] = (u16)(g + 1);
        return g;
    }
    return cmap_lookup(f, cp);
}

float face_adv(Face *f, u16 g, float px) {
    u32 i = (int)g < f->nhm ? g : (u32)(f->nhm - 1);
    float a = (float)rd16(f, f->hmtx + 4 * i) * px / f->upem;
    if (f->synth_bold) a += px * 0.03f;
    return a;
}
float face_kern(Face *f, u16 a, u16 b, float px) {
    u32 key = ((u32)a << 16) | b;
    u32 h = (key * 2654435761u) >> 20;
    if (f->kcache[h] != key) {
        int v = f->nkern_subs ? gpos_kern(f, a, b) : 0;
        if (!v && f->kern) v = kern_table(f, a, b);
        f->kcache[h] = key; f->kval[h] = (i16)v;
    }
    return (float)f->kval[h] * px / f->upem;
}
float face_ascent(Face *f, float px) { return (float)f->asc * px / f->upem; }
float face_descent(Face *f, float px) { return (float)f->desc * px / f->upem; }
float face_xheight(Face *f, float px) { return (float)f->xh * px / f->upem; }

/* ------------------------------------------------------------ contornos */
typedef struct { float a, b, c, d, e, g; } Xf;   /* X = a*x + c*y + e ; Y = b*x + d*y + g */

static u32 glyph_off(Face *f, u16 gid, u32 *len) {
    if ((int)gid >= f->nglyphs) { *len = 0; return 0; }
    u32 o0, o1;
    if (f->loca_long) { o0 = rd32(f, f->loca + 4 * (u32)gid); o1 = rd32(f, f->loca + 4 * (u32)gid + 4); }
    else { o0 = rd16(f, f->loca + 2 * (u32)gid) * 2; o1 = rd16(f, f->loca + 2 * (u32)gid + 2) * 2; }
    *len = o1 > o0 ? o1 - o0 : 0;
    return f->glyf + o0;
}

static void emit_contour(Raster *r, float *xs, float *ys, u8 *on, int n) {
    if (n < 2) return;
    int first = -1;
    for (int i = 0; i < n; i++) if (on[i]) { first = i; break; }
    float sx, sy; int k0, cnt;
    if (first >= 0) { sx = xs[first]; sy = ys[first]; k0 = first + 1; cnt = n - 1; }
    else { sx = (xs[n - 1] + xs[0]) * 0.5f; sy = (ys[n - 1] + ys[0]) * 0.5f; k0 = 0; cnt = n; }
    float px = sx, py = sy, cx = 0, cy = 0; bool hc = false;
    for (int k = 0; k < cnt; k++) {
        int i = (k0 + k) % n;
        float x = xs[i], y = ys[i];
        if (on[i]) {
            if (hc) raster_quad(r, px, py, cx, cy, x, y); else raster_line(r, px, py, x, y);
            px = x; py = y; hc = false;
        } else {
            if (hc) {
                float mx = (cx + x) * 0.5f, my = (cy + y) * 0.5f;
                raster_quad(r, px, py, cx, cy, mx, my);
                px = mx; py = my;
            }
            cx = x; cy = y; hc = true;
        }
    }
    if (hc) raster_quad(r, px, py, cx, cy, sx, sy); else raster_line(r, px, py, sx, sy);
}

static void emit_glyph(Face *f, u16 gid, Xf t, Raster *r, int depth) {
    if (depth > 6) return;
    u32 len; u32 g = glyph_off(f, gid, &len);
    if (len < 10) return;
    int nc = rds16(f, g);
    if (nc >= 0) {
        u32 p = g + 10;
        int npts = nc ? (int)rd16(f, p + 2 * (u32)(nc - 1)) + 1 : 0;
        if (npts <= 0 || npts > 20000) return;
        u16 *ends = mem_alloc(sizeof(u16) * (usize)nc);
        for (int i = 0; i < nc; i++) ends[i] = (u16)rd16(f, p + 2 * (u32)i);
        p += 2 * (u32)nc;
        p += 2 + rd16(f, p);
        u8 *fl = mem_alloc((usize)npts);
        float *xs = mem_alloc(sizeof(float) * (usize)npts * 2), *ys = xs + npts;
        for (int i = 0; i < npts; ) {
            u8 fg = (u8)rd8(f, p++); fl[i++] = fg;
            if (fg & 8) { int rep = (int)rd8(f, p++); while (rep-- > 0 && i < npts) fl[i++] = fg; }
        }
        int v = 0;
        for (int i = 0; i < npts; i++) {
            u8 fg = fl[i];
            if (fg & 2) { int dv = (int)rd8(f, p++); v += (fg & 16) ? dv : -dv; }
            else if (!(fg & 16)) { v += rds16(f, p); p += 2; }
            xs[i] = (float)v;
        }
        v = 0;
        for (int i = 0; i < npts; i++) {
            u8 fg = fl[i];
            if (fg & 4) { int dv = (int)rd8(f, p++); v += (fg & 32) ? dv : -dv; }
            else if (!(fg & 32)) { v += rds16(f, p); p += 2; }
            ys[i] = (float)v;
        }
        for (int i = 0; i < npts; i++) {
            float x = xs[i], y = ys[i];
            xs[i] = t.a * x + t.c * y + t.e;
            ys[i] = t.b * x + t.d * y + t.g;
            fl[i] &= 1;
        }
        int s = 0;
        for (int c = 0; c < nc; c++) {
            int e = ends[c];
            if (e >= npts || e < s) break;
            emit_contour(r, xs + s, ys + s, fl + s, e - s + 1);
            s = e + 1;
        }
        mem_free(ends); mem_free(fl); mem_free(xs);
    } else {
        u32 p = g + 10;
        for (int guard = 0; guard < 64; guard++) {
            u32 flags = rd16(f, p), cg = rd16(f, p + 2); p += 4;
            float dx, dy;
            if (flags & 1) { dx = (float)rds16(f, p); dy = (float)rds16(f, p + 2); p += 4; }
            else { dx = (float)(i8)rd8(f, p); dy = (float)(i8)rd8(f, p + 1); p += 2; }
            if (!(flags & 2)) { dx = dy = 0; }
            float a = 1, b = 0, c = 0, d = 1;
            if (flags & 8) { a = d = (float)rds16(f, p) / 16384.0f; p += 2; }
            else if (flags & 0x40) { a = (float)rds16(f, p) / 16384.0f; d = (float)rds16(f, p + 2) / 16384.0f; p += 4; }
            else if (flags & 0x80) {
                a = (float)rds16(f, p) / 16384.0f; b = (float)rds16(f, p + 2) / 16384.0f;
                c = (float)rds16(f, p + 4) / 16384.0f; d = (float)rds16(f, p + 6) / 16384.0f; p += 8;
            }
            Xf ct;
            ct.a = t.a * a + t.c * b; ct.b = t.b * a + t.d * b;
            ct.c = t.a * c + t.c * d; ct.d = t.b * c + t.d * d;
            ct.e = t.a * dx + t.c * dy + t.e; ct.g = t.b * dx + t.d * dy + t.g;
            emit_glyph(f, (u16)cg, ct, r, depth + 1);
            if (!(flags & 0x20)) break;
        }
    }
}

/* ------------------------------------------------------------ cache de glifos */
typedef struct { Face *f; u16 gid, sq; u8 sub, used; Glyph g; } GEntry;
#define GCACHE 8192
static GEntry *gcache;
static int gcount;
static usize gbytes;
static u8 gamma_lut[256];

void glyph_cache_trim(void) {
    if (!gcache) return;
    for (int i = 0; i < GCACHE; i++) if (gcache[i].used) { mem_free(gcache[i].g.bmp); gcache[i].used = 0; }
    gcount = 0; gbytes = 0;
}

static Glyph render_glyph(Face *f, u16 gid, float px, int sub) {
    Glyph out = {0, 0, 0, 0, 0};
    u32 len; u32 g = glyph_off(f, gid, &len);
    if (len < 10) return out;
    float sc = px / f->upem;
    float shear = f->synth_italic ? 0.2f : 0;
    float xmin = (float)rds16(f, g + 2), ymin = (float)rds16(f, g + 4), xmax = (float)rds16(f, g + 6), ymax = (float)rds16(f, g + 8);
    float sx = sub * 0.25f;
    float bold = f->synth_bold ? px * 0.03f : 0;
    float l = MIN(xmin + shear * ymin, xmin + shear * ymax) * sc + sx;
    float rr = MAX(xmax + shear * ymax, xmax + shear * ymin) * sc + sx + bold;
    int left = (int)f_floor(l) - 1, right = (int)f_ceil(rr) + 1;
    int top = (int)f_floor(-ymax * sc) - 1, bot = (int)f_ceil(-ymin * sc) + 1;
    int w = right - left, h = bot - top;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return out;
    Raster r; raster_begin(&r, w, h);
    Xf t = { sc, 0, shear * sc, -sc, sx - (float)left, -(float)top };
    emit_glyph(f, gid, t, &r, 0);
    if (bold > 0) { t.e += bold; emit_glyph(f, gid, t, &r, 0); }
    u8 *m = raster_end(&r);
    for (int i = 0; i < w * h; i++) m[i] = gamma_lut[m[i]];
    out.bmp = m; out.w = (i16)w; out.h = (i16)h; out.ox = (i16)left; out.oy = (i16)top;
    return out;
}

const Glyph *face_glyph(Face *f, u16 gid, float px, int sub) {
    if (!gcache) {
        gcache = mem_zalloc(sizeof(GEntry) * GCACHE);
        for (int i = 0; i < 256; i++) gamma_lut[i] = (u8)(f_pow(i / 255.0f, 0.82f) * 255 + 0.5f);
    }
    u16 sq = (u16)(px * 4 + 0.5f);
    u32 h = ((u32)(uintptr_t)f * 31u) ^ ((u32)gid * 2654435761u) ^ ((u32)sq * 40503u) ^ ((u32)sub * 977u);
    h = (h ^ (h >> 15)) & (GCACHE - 1);
    for (int probe = 0; probe < GCACHE; probe++) {
        GEntry *e = &gcache[(h + (u32)probe) & (GCACHE - 1)];
        if (!e->used) {
            if (gcount > GCACHE * 7 / 10 || gbytes > (24u << 20)) { glyph_cache_trim(); return face_glyph(f, gid, px, sub); }
            e->used = 1; e->f = f; e->gid = gid; e->sq = sq; e->sub = (u8)sub;
            e->g = render_glyph(f, gid, px, sub);
            gcount++; gbytes += (usize)e->g.w * e->g.h + 64;
            return &e->g;
        }
        if (e->f == f && e->gid == gid && e->sq == sq && e->sub == sub) return &e->g;
    }
    return 0;
}

/* ------------------------------------------------------------ famílias */
extern const u8 embed_font_regular[], embed_font_regular_end[];
extern const u8 embed_font_bold[], embed_font_bold_end[];

static Family fams[64];
static int nfams;
static int ui_fam, mono_fam, ui_override = -1;
static Face *fallback;
#define MAX_LANG_FONTS 6
static Face *lang_faces[MAX_LANG_FONTS];   /* fontes de reserva do idioma (X-Fonts) */
static int   nlang_faces;
static char  lang_files[160];

static void fam_complete(Family *fm) {
    if (!fm->f[FS_REG]) {
        for (int i = 1; i < 4; i++) if (fm->f[i]) { fm->f[FS_REG] = fm->f[i]; break; }
    }
    if (!fm->f[FS_REG]) return;
    if (!fm->f[FS_BOLD]) fm->f[FS_BOLD] = face_variant(fm->f[FS_REG], true, false);
    if (!fm->f[FS_ITALIC]) fm->f[FS_ITALIC] = face_variant(fm->f[FS_REG], false, true);
    if (!fm->f[FS_BOLDITALIC]) {
        Face *b = fm->f[FS_BOLD];
        fm->f[FS_BOLDITALIC] = b->synth_bold ? face_variant(fm->f[FS_REG], true, true) : face_variant(b, false, true);
    }
    fm->mono = face_is_mono(fm->f[FS_REG]);
}

static void fonts_pick_roles(void) {
    ui_fam = 0; mono_fam = -1;
    int i;
    if ((i = font_find("Fira Sans")) >= 0) ui_fam = i;
    if ((i = font_find("IBM Plex Mono")) >= 0) mono_fam = i;
    else if ((i = font_find("Courier Prime")) >= 0) mono_fam = i;
    if (mono_fam < 0) for (i = 0; i < nfams; i++) if (fams[i].mono) { mono_fam = i; break; }
    if (mono_fam < 0) mono_fam = ui_fam;
}

void fonts_init(void) {
    nfams = 0;
    Face *r = face_load(embed_font_regular, (usize)(embed_font_regular_end - embed_font_regular), false);
    Face *b = face_load(embed_font_bold, (usize)(embed_font_bold_end - embed_font_bold), false);
    Family *fm = &fams[nfams++];
    memset(fm, 0, sizeof *fm);
    str_copy(fm->name, "Atkinson Hyperlegible", sizeof fm->name);
    fm->f[FS_REG] = r; fm->f[FS_BOLD] = b; fm->embedded = true;
    fam_complete(fm);
    fallback = r;
    fonts_pick_roles();
}

static void add_face(Face *f) {
    char fam[48], sty[48];
    face_names(f, fam, sizeof fam, sty, sizeof sty);
    if (!fam[0]) return;
    int idx = font_find(fam);
    if (idx < 0) {
        if (nfams >= COUNT(fams)) return;
        idx = nfams++;
        memset(&fams[idx], 0, sizeof(Family));
        str_copy(fams[idx].name, fam, sizeof fams[idx].name);
    }
    Family *fm = &fams[idx];
    if (fm->embedded) return;
    int w = face_weight(f);
    bool bold = w >= 600;
    int slot = (bold ? 1 : 0) + (face_is_italic(f) ? 2 : 0);
    Face *cur = fm->f[slot];
    int target = bold ? 700 : 400;
    if (!cur || (w - target) * (w - target) < (face_weight(cur) - target) * (face_weight(cur) - target)) fm->f[slot] = f;
}

/* lê os .ttf de uma pasta; depth > 0 entra nas subpastas (ex.: FOLIO/FONTS/EXTRA/Lora). Pastas que
 * começam com _ (como _LANGUAGE, das fontes de idiomas) não entram: são carregadas só sob demanda */
static void scan_dir(int vol, const char *dir, int depth) {
    FileEntry *ents; int n = pal_list(vol, dir, &ents);
    if (n <= 0) return;
    for (int i = 0; i < n; i++) {
        if (ents[i].dir) {
            if (depth > 0 && ents[i].name[0] != '.' && ents[i].name[0] != '_') { char sub[512]; path_join(sub, sizeof sub, dir, ents[i].name); scan_dir(vol, sub, depth - 1); }
            continue;
        }
        if (!str_ends_i(ents[i].name, ".ttf")) continue;
        char path[512]; path_join(path, sizeof path, dir, ents[i].name);
        u8 *data; usize len;
        if (!pal_read(vol, path, &data, &len)) continue;
        Face *f = face_load(data, len, true);
        if (f) add_face(f); else mem_free(data);
    }
    mem_free(ents);
}

void fonts_scan(int vol, const char *dir) {
    char keep[48] = ""; if (ui_override >= 0) str_copy(keep, fams[ui_override].name, sizeof keep);
    scan_dir(vol, dir, 2);
    for (int i = 0; i < nfams; i++) if (!fams[i].embedded) fam_complete(&fams[i]);
    /* ordena: serifadas/sans/mono conforme nome, mantendo a embutida no fim */
    for (int i = 0; i < nfams; i++) if (!fams[i].f[FS_REG]) { fams[i] = fams[--nfams]; i--; }
    for (int i = 0; i < nfams; i++)
        for (int j = i + 1; j < nfams; j++) {
            bool swap = fams[i].embedded && !fams[j].embedded;
            if (!swap && fams[i].embedded == fams[j].embedded && str_cmp_i(fams[i].name, fams[j].name) > 0) swap = true;
            if (swap) { Family t = fams[i]; fams[i] = fams[j]; fams[j] = t; }
        }
    fonts_pick_roles();
    ui_override = keep[0] ? font_find(keep) : -1;
}

int font_count(void) { return nfams; }
Family *font_get(int i) { return &fams[CLAMP(i, 0, nfams - 1)]; }
int font_find(const char *name) { for (int i = 0; i < nfams; i++) if (str_ieq(fams[i].name, name)) return i; return -1; }
Family *font_ui(void) { return &fams[ui_override >= 0 && ui_override < nfams ? ui_override : ui_fam]; }
void font_set_ui(int i) { ui_override = (i >= 0 && i < nfams) ? i : -1; }
Family *font_mono(void) { return &fams[mono_fam]; }
Face *fam_face(Family *f, int style) { return f->f[style & 3] ? f->f[style & 3] : f->f[0]; }

/* ------------------------------------------------------------ texto */
static inline u16 resolve(Face **f, u32 cp) {
    u16 g = face_gid(*f, cp);
    if (!g && cp > 32 && fallback && *f != fallback) {
        u16 g2 = face_gid(fallback, cp);
        if (g2) { *f = fallback; return g2; }
    }
    if (!g && cp > 32) for (int i = 0; i < nlang_faces; i++) {
        if (*f == lang_faces[i]) continue;
        u16 g2 = face_gid(lang_faces[i], cp);
        if (g2) { *f = lang_faces[i]; return g2; }
    }
    return g;
}

float text_width(Face *f, float px, const char *s, int len) {
    if (len < 0) len = str_len(s);
    float w = 0; u16 prev = 0; Face *pf = 0;
    for (int i = 0; i < len; ) {
        u32 cp; i += utf8_decode(s, len, i, &cp);
        if (cp == '\t') cp = ' ';
        Face *ff = f; u16 g = resolve(&ff, cp);
        if (prev && pf == ff) w += face_kern(ff, prev, g, px);
        w += face_adv(ff, g, px);
        prev = g; pf = ff;
    }
    return w;
}

float draw_text(Face *f, float px, float x, float base, const char *s, int len, Color c) {
    if (len < 0) len = str_len(s);
    float pen = x; u16 prev = 0; Face *pf = 0;
    int by = (int)f_round(base);
    for (int i = 0; i < len; ) {
        u32 cp; i += utf8_decode(s, len, i, &cp);
        if (cp == '\t') cp = ' ';
        Face *ff = f; u16 g = resolve(&ff, cp);
        if (prev && pf == ff) pen += face_kern(ff, prev, g, px);
        if (cp != ' ') {
            float fx = f_floor(pen);
            int sub = (int)((pen - fx) * 4) & 3;
            const Glyph *gl = face_glyph(ff, g, px, sub);
            if (gl && gl->bmp) gfx_mask((int)fx + gl->ox, by + gl->oy, gl->bmp, gl->w, gl->h, gl->w, c);
        }
        pen += face_adv(ff, g, px);
        prev = g; pf = ff;
    }
    return pen;
}

float draw_text_c(Face *f, float px, Rect r, const char *s, Color c) {
    float w = text_width(f, px, s, -1);
    float asc = face_ascent(f, px), desc = face_descent(f, px);
    float base = r.y + (r.h - (asc + desc)) * 0.5f + asc;
    return draw_text(f, px, r.x + (r.w - w) * 0.5f, base, s, -1, c);
}

int text_fit(Face *f, float px, const char *s, int len, float maxw) {
    if (len < 0) len = str_len(s);
    float w = 0; u16 prev = 0;
    int i = 0;
    while (i < len) {
        u32 cp; int k = utf8_decode(s, len, i, &cp);
        Face *ff = f; u16 g = resolve(&ff, cp);
        float a = face_adv(ff, g, px) + (prev ? face_kern(ff, prev, g, px) : 0);
        if (w + a > maxw) break;
        w += a; prev = g; i += k;
    }
    return i;
}

void draw_text_ellipsis(Face *f, float px, float x, float base, float maxw, const char *s, Color c) {
    int len = str_len(s);
    if (text_width(f, px, s, len) <= maxw) { draw_text(f, px, x, base, s, len, c); return; }
    float ew = text_width(f, px, "…", -1);
    int n = text_fit(f, px, s, len, maxw - ew);
    float e = draw_text(f, px, x, base, s, n, c);
    draw_text(f, px, e, base, "…", -1, c);
}

u16 face_gid_fb(Face **f, u32 cp) { return resolve(f, cp); }

static void face_free(Face *f) {
    if (!f) return;
    if (f->kern_subs) mem_free(f->kern_subs);
    if (f->kcache) mem_free(f->kcache);
    if (f->kval) mem_free(f->kval);
    if (f->cp_cache) mem_free(f->cp_cache);
    if (f->owned) mem_free((void *)f->d);
    mem_free(f);
}

/* fontes de reserva do idioma: "CJK/NotoSansSC.ttf; CJK/Outra.ttf" (caminhos em FOLIO/FONTS).
 * Carregadas só quando o idioma é escolhido; as do idioma anterior são liberadas. */
void fonts_set_lang(const char *files) {
    if (!files) files = "";
    if (str_eq(files, lang_files)) return;
    if (nlang_faces) glyph_cache_trim();
    for (int i = 0; i < nlang_faces; i++) face_free(lang_faces[i]);
    nlang_faces = 0;
    str_copy(lang_files, files, sizeof lang_files);
    const char *p = files;
    while (*p && nlang_faces < MAX_LANG_FONTS) {
        while (*p == ' ' || *p == ';' || *p == ',') p++;
        const char *s = p; while (*p && *p != ';' && *p != ',') p++;
        int n = (int)(p - s); while (n > 0 && s[n - 1] == ' ') n--;
        if (n <= 0 || n > 120) continue;
        char rel[128]; memcpy(rel, s, (usize)n); rel[n] = 0;
        bool bad = rel[0] == '/' || rel[0] == '\\';
        for (char *c = rel; *c; c++) { if (*c == '\\') *c = '/'; if (c[0] == '.' && c[1] == '.') bad = true; }
        if (bad) continue;
        char path[300]; path_join(path, sizeof path, paths_dir(DIR_FONTS), rel);
        u8 *data; usize len;
        if (!pal_read(app_boot_vol(), path, &data, &len)) {
            path_join(path, sizeof path, "/FOLIO/FONTS", rel);              /* pacote descompactado num pen-drive antigo */
            if (!pal_read(app_boot_vol(), path, &data, &len)) continue;
        }
        Face *f = face_load(data, len, true);
        if (f) lang_faces[nlang_faces++] = f; else mem_free(data);
    }
}
bool fonts_lang_ok(void) { return lang_files[0] == 0 || nlang_faces > 0; }
