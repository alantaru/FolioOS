/* FOLIO v4 — música: MP3 da pasta \MUSICA do pen-drive (streaming + ID3 + player) */
#include "folio.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "../../third_party/minimp3.h"

typedef struct { char path[300]; char name[160]; } Track;
static Track *tracks; static int ntracks, cur = -1;
static int  mvol;
static bool playing, audio_on;
static PalFile *mf; static u64 fsize, data_start;
static u8  *inbuf; static int in_len, in_pos; static bool eof;
static mp3dec_t dec;
static i16  pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]; static int pcm_frames, pcm_pos, pcm_ch;
static int  src_rate, dev_rate;
static double rs_frac; static i16 rs_prev[2], rs_cur[2]; static bool rs_primed;
static u64  frames_played; static float duration;
static char title[200], artist[160], now_str[260];
static int  bitrate;
static int  order[1024];
static float list_scroll;
static int  chime_pos = -1;
static int  fail_count;

static void music_body(Window *w, Rect r);
Window win_music = { N_("Music"), IC_MUSIC, {0, 0, 440, 600}, false, false, false, true, false, 0, 360, 380, music_body, 0, 0, false };

bool music_available(void) { return ntracks > 0; }
bool music_playing(void) { return playing; }
const char *music_now(void) { return cur >= 0 ? now_str : ""; }

/* ------------------------------------------------------------ varredura */
static void add_dir(int vol, const char *dir, int depth) {
    FileEntry *l; int n = pal_list(vol, dir, &l);
    if (n <= 0) return;
    for (int i = 0; i < n && ntracks < 1024; i++) {
        if (l[i].name[0] == '.') continue;
        char p[300]; path_join(p, sizeof p, dir, l[i].name);
        if (l[i].dir) { if (depth < 2) add_dir(vol, p, depth + 1); continue; }
        if (!str_ends_i(l[i].name, ".mp3")) continue;
        Track *t = &tracks[ntracks++];
        str_copy(t->path, p, sizeof t->path);
        str_copy(t->name, l[i].name, sizeof t->name);
        int nl = str_len(t->name); if (nl > 4) t->name[nl - 4] = 0;
        for (char *c = t->name; *c; c++) if (*c == '_') *c = ' ';
    }
    mem_free(l);
}
static int music_vol;
void music_init(int vol) {
    music_vol = vol;
    tracks = mem_zalloc(sizeof(Track) * 1024);
    inbuf = mem_alloc(65536);
    ntracks = 0;
    const char *dirs[] = { paths_dir(DIR_MUSIC), "/MUSIC", "/MUSICA", "/MÚSICA" };
    bool isdir;
    for (int i = 0; i < COUNT(dirs) && !ntracks; i++) if (pal_exists(vol, dirs[i], &isdir) && isdir) add_dir(vol, dirs[i], 0);
    /* ordena por caminho (natural) */
    for (int i = 1; i < ntracks; i++) { Track t = tracks[i]; int j = i - 1; while (j >= 0 && str_cmp_i(tracks[j].path, t.path) > 0) { tracks[j + 1] = tracks[j]; j--; } tracks[j + 1] = t; }
    for (int i = 0; i < ntracks; i++) order[i] = i;
    mvol = prefs.volume;
    mp3dec_init(&dec);
}

/* ------------------------------------------------------------ ID3 */
static void id3_text(const u8 *p, int n, char *out, int cap) {
    out[0] = 0; if (n <= 1) return;
    int enc = p[0]; p++; n--;
    int o = 0; char t[4];
    if (enc == 1 || enc == 2) {
        bool le = enc == 1;
        int i = 0;
        if (enc == 1 && n >= 2) { le = p[0] == 0xFF; i = 2; }
        for (; i + 1 < n && o < cap - 4; i += 2) {
            u32 c = le ? (u32)(p[i] | (p[i + 1] << 8)) : (u32)((p[i] << 8) | p[i + 1]);
            if (!c) break;
            int k = utf8_encode(c, t); memcpy(out + o, t, (usize)k); o += k;
        }
    } else if (enc == 3) { for (int i = 0; i < n && p[i] && o < cap - 1; i++) out[o++] = (char)p[i]; }
    else { for (int i = 0; i < n && p[i] && o < cap - 4; i++) { int k = utf8_encode(p[i], t); memcpy(out + o, t, (usize)k); o += k; } }
    out[o] = 0;
}
static u64 parse_id3(const u8 *b, int n) {
    if (n < 10 || b[0] != 'I' || b[1] != 'D' || b[2] != '3') return 0;
    int ver = b[3];
    u32 size = ((u32)(b[6] & 0x7F) << 21) | ((u32)(b[7] & 0x7F) << 14) | ((u32)(b[8] & 0x7F) << 7) | (b[9] & 0x7F);
    u64 total = size + 10 + ((b[5] & 0x10) ? 10 : 0);
    int p = 10;
    int lim = (int)MIN((u64)n, size + 10);
    while (p + 10 <= lim) {
        if (ver == 2) {
            if (p + 6 > lim || !b[p]) break;
            u32 fs = ((u32)b[p + 3] << 16) | ((u32)b[p + 4] << 8) | b[p + 5];
            if (p + 6 + (int)fs > lim) break;
            if (!memcmp(b + p, "TT2", 3)) id3_text(b + p + 6, (int)fs, title, sizeof title);
            if (!memcmp(b + p, "TP1", 3)) id3_text(b + p + 6, (int)fs, artist, sizeof artist);
            p += 6 + (int)fs; continue;
        }
        if (!b[p]) break;
        u32 fs = ver >= 4 ? (((u32)(b[p + 4] & 0x7F) << 21) | ((u32)(b[p + 5] & 0x7F) << 14) | ((u32)(b[p + 6] & 0x7F) << 7) | (b[p + 7] & 0x7F))
                          : (((u32)b[p + 4] << 24) | ((u32)b[p + 5] << 16) | ((u32)b[p + 6] << 8) | b[p + 7]);
        if (fs == 0 || p + 10 + (int)fs > lim) break;
        if (!memcmp(b + p, "TIT2", 4)) id3_text(b + p + 10, (int)fs, title, sizeof title);
        if (!memcmp(b + p, "TPE1", 4)) id3_text(b + p + 10, (int)fs, artist, sizeof artist);
        p += 10 + (int)fs;
    }
    return total;
}

/* ------------------------------------------------------------ reprodução */
static void close_track(void) { if (mf) pal_fclose(mf); mf = 0; in_len = in_pos = 0; pcm_frames = pcm_pos = 0; eof = false; }

static void fill_input(void) {
    if (eof || !mf) return;
    if (in_pos > 0) { memmove(inbuf, inbuf + in_pos, (usize)(in_len - in_pos)); in_len -= in_pos; in_pos = 0; }
    while (in_len < 65536 - 4096) {
        int n = pal_fread(mf, inbuf + in_len, 65536 - in_len);
        if (n <= 0) { eof = true; break; }
        in_len += n;
    }
}

static void update_now(void) {
    if (title[0] && artist[0]) fmt(now_str, sizeof now_str, "%s — %s", title, artist);
    else if (title[0]) str_copy(now_str, title, sizeof now_str);
    else str_copy(now_str, cur >= 0 ? tracks[cur].name : "", sizeof now_str);
}

/* sem saída de áudio: diz o motivo e grava o diagnóstico completo no pen-drive */
static void audio_fail_toast(void) {
    char m[260]; fmt(m, sizeof m, L("No sound: %s. Details in %s"), pal_audio_info(), paths_show(FILE_DIAG));
    ui_toast(m);
    app_save_diag();
}

static bool start_track(int idx) {
    close_track();
    if (idx < 0 || idx >= ntracks) return false;
    cur = idx;
    title[0] = artist[0] = 0; duration = 0; bitrate = 0; frames_played = 0;
    mf = pal_fopen(music_vol, tracks[idx].path, &fsize);
    if (!mf) { ui_toast(L("Could not open the track")); return false; }
    fill_input();
    data_start = 0;
    if (in_len >= 10 && inbuf[0] == 'I' && inbuf[1] == 'D' && inbuf[2] == '3') {
        u64 sz = parse_id3(inbuf, in_len);
        data_start = sz;
        if (sz < (u64)in_len) in_pos = (int)sz;
        else { pal_fseek(mf, sz); in_len = in_pos = 0; eof = false; fill_input(); }
    }
    update_now();
    mp3dec_init(&dec);
    /* primeiro quadro: taxa, canais, bitrate, Xing */
    mp3dec_frame_info_t info;
    int guard = 0;
    pcm_frames = 0;
    while (guard++ < 64) {
        fill_input();
        int s = mp3dec_decode_frame(&dec, inbuf + in_pos, in_len - in_pos, pcm, &info);
        if (info.frame_bytes == 0) { if (eof) break; in_pos = in_len; continue; }
        const u8 *fr = inbuf + in_pos;
        in_pos += info.frame_bytes;
        if (s > 0) {
            src_rate = info.hz; pcm_ch = info.channels; pcm_frames = s; pcm_pos = 0; bitrate = info.bitrate_kbps;
            int xo = info.channels == 1 ? (src_rate >= 32000 ? 21 : 13) : (src_rate >= 32000 ? 36 : 21);
            if (info.frame_bytes > xo + 12 && (!memcmp(fr + xo, "Xing", 4) || !memcmp(fr + xo, "Info", 4))) {
                u32 flags = ((u32)fr[xo + 4] << 24) | ((u32)fr[xo + 5] << 16) | ((u32)fr[xo + 6] << 8) | fr[xo + 7];
                if (flags & 1) {
                    u32 nf = ((u32)fr[xo + 8] << 24) | ((u32)fr[xo + 9] << 16) | ((u32)fr[xo + 10] << 8) | fr[xo + 11];
                    duration = (float)nf * (float)s / (float)src_rate;
                }
                pcm_frames = 0;   /* quadro Xing é silêncio */
            }
            if (duration <= 0 && bitrate > 0) duration = (float)(fsize - data_start) * 8.0f / (bitrate * 1000.0f);
            break;
        }
    }
    if (src_rate <= 0) { close_track(); return false; }
    if (!audio_on || dev_rate != src_rate) {
        audio_on = pal_audio_start(src_rate);
        extern int pal_audio_rate(void);
        dev_rate = audio_on ? pal_audio_rate() : 0;
    }
    rs_frac = 0; rs_primed = false;
    if (!audio_on) { audio_fail_toast(); playing = false; return false; }
    playing = true;
    ui.redraw = true;
    return true;
}

static int pos_in_order(int idx) { for (int i = 0; i < ntracks; i++) if (order[i] == idx) return i; return 0; }
static void shuffle_order(void) {
    for (int i = 0; i < ntracks; i++) order[i] = i;
    if (!prefs.shuffle) return;
    for (int i = ntracks - 1; i > 0; i--) { int j = (int)(rng_next() % (u32)(i + 1)); int t = order[i]; order[i] = order[j]; order[j] = t; }
}

void music_next(int dir) {
    if (!ntracks) return;
    int p = cur < 0 ? -1 : pos_in_order(cur);
    int np = p + dir;
    if (np >= ntracks) { if (!prefs.repeat && dir > 0 && cur >= 0) { playing = false; close_track(); cur = -1; return; } np = 0; }
    if (np < 0) np = ntracks - 1;
    if (dir < 0 && frames_played > (u64)src_rate * 3) np = p;    /* "anterior" após 3 s reinicia a faixa */
    if (!start_track(order[np])) { if (++fail_count < ntracks) music_next(dir >= 0 ? 1 : -1); }
    else fail_count = 0;
}

void music_toggle(void) {
    if (!ntracks) { ui_toast(pathfmt(L("Put .mp3 files in the %s folder of the USB drive"), DIR_MUSIC)); return; }
    if (cur < 0) { shuffle_order(); music_next(1); return; }
    playing = !playing;
    if (playing && !mf) start_track(cur);
    ui.redraw = true;
}

static void seek_frac(float f) {
    if (!mf || duration <= 0) return;
    f = CLAMP(f, 0.0f, 0.995f);
    u64 pos = data_start + (u64)((double)(fsize - data_start) * f);
    pal_fseek(mf, pos);
    in_len = in_pos = 0; eof = false; pcm_frames = pcm_pos = 0;
    mp3dec_init(&dec);
    frames_played = (u64)(duration * f * src_rate);
}

void music_chime(void) {
    chime_pos = 0;
    if (!audio_on) { audio_on = pal_audio_start(44100); dev_rate = audio_on ? pal_audio_rate() : 0; if (!audio_on) { chime_pos = -1; audio_fail_toast(); } }
}

static bool decode_more(void) {
    mp3dec_frame_info_t info;
    for (int guard = 0; guard < 32; guard++) {
        if (in_len - in_pos < 8192) fill_input();
        if (in_pos >= in_len && eof) return false;
        int s = mp3dec_decode_frame(&dec, inbuf + in_pos, in_len - in_pos, pcm, &info);
        if (info.frame_bytes == 0) { if (eof) return false; in_pos = in_len; continue; }
        in_pos += info.frame_bytes;
        if (s > 0) { pcm_frames = s; pcm_pos = 0; pcm_ch = info.channels; return true; }
    }
    return false;
}

static float chime_sample(void) {
    if (chime_pos < 0 || dev_rate <= 0) return 0;
    float t = (float)chime_pos / dev_rate;
    if (t > 2.2f) { chime_pos = -1; return 0; }
    chime_pos++;
    float env = f_exp(-t * 2.4f) * MIN(1.0f, t * 200);
    float s = f_sin(6.2831853f * 880.0f * t) * 0.5f + f_sin(6.2831853f * 1318.5f * t) * 0.28f * f_exp(-t * 1.5f) + f_sin(6.2831853f * 1760.0f * t) * 0.12f;
    return s * env * 0.32f;
}

void music_update(void) {
    bool need = (playing && mf) || chime_pos >= 0;
    if (!need) {
        if (audio_on) { pal_audio_stop(); audio_on = false; dev_rate = 0; }
        return;
    }
    if (!audio_on) {
        extern int pal_audio_rate(void);
        audio_on = pal_audio_start((playing && src_rate > 0) ? src_rate : 44100);
        dev_rate = audio_on ? pal_audio_rate() : 0;
        rs_primed = false; rs_frac = 0;
        if (!audio_on) { playing = false; chime_pos = -1; return; }
    }
    int space = pal_audio_space();
    if (space <= 0) return;
    if (space > 4096) space = 4096;
    static i16 out[4096 * 2];
    float g = (float)prefs.volume / 100.0f; g = g * g;
    double step = (src_rate > 0 && dev_rate > 0) ? (double)src_rate / dev_rate : 1.0;
    int n = 0;
    while (n < space) {
        float l = 0, r = 0;
        if (playing && mf) {
            if (step == 1.0) {
                if (pcm_pos >= pcm_frames && !decode_more()) { close_track(); music_next(1); if (!playing) { /* fim */ } break; }
                const i16 *s = &pcm[pcm_pos * pcm_ch];
                l = s[0]; r = pcm_ch > 1 ? s[1] : s[0];
                pcm_pos++; frames_played++;
            } else {
                while (!rs_primed || rs_frac >= 1.0) {
                    if (pcm_pos >= pcm_frames && !decode_more()) { close_track(); music_next(1); goto done; }
                    const i16 *s = &pcm[pcm_pos * pcm_ch];
                    rs_prev[0] = rs_cur[0]; rs_prev[1] = rs_cur[1];
                    rs_cur[0] = s[0]; rs_cur[1] = pcm_ch > 1 ? s[1] : s[0];
                    pcm_pos++; frames_played++;
                    if (!rs_primed) { rs_primed = true; rs_prev[0] = rs_cur[0]; rs_prev[1] = rs_cur[1]; rs_frac = 0; break; }
                    rs_frac -= 1.0;
                }
                float t = (float)rs_frac;
                l = rs_prev[0] + (rs_cur[0] - rs_prev[0]) * t;
                r = rs_prev[1] + (rs_cur[1] - rs_prev[1]) * t;
                rs_frac += step;
            }
            l *= g; r *= g;
        }
        float c = chime_sample() * 32767.0f * MAX(0.35f, g + 0.2f);
        l += c; r += c;
        out[n * 2] = (i16)CLAMP(l, -32768.0f, 32767.0f);
        out[n * 2 + 1] = (i16)CLAMP(r, -32768.0f, 32767.0f);
        n++;
    }
done:
    if (n > 0) pal_audio_write(out, n);
}

/* ------------------------------------------------------------ janela */
static void fmt_time(float s, char *o, int cap) { int t = (int)s; fmt(o, cap, "%d:%02d", t / 60, t % 60); }

static void music_body(Window *w, Rect r) {
    UNUSED(w);
    Face *f = ui_font(false), *fb = ui_font(true);
    int pad = SI(22);
    int x = r.x + pad, wd = r.w - 2 * pad, y = r.y + SI(18);
    if (!ntracks) {
        icon_draw(IC_MUSIC, r.x + r.w * 0.5f, y + S(50), S(48), col_alpha(T->accent, 0.6f));
        draw_text_c(fb, S(16), rect(r.x, y + SI(96), r.w, SI(30)), L("No music found"), T->panel_text);
        const char *l1 = pathfmt(L("Create a %s folder at the root of the USB drive"), DIR_MUSIC), *l2 = L("and copy your .mp3 files into it.");
        draw_text_c(f, S(14), rect(r.x, y + SI(128), r.w, SI(24)), l1, col_alpha(T->panel_text, 0.6f));
        draw_text_c(f, S(14), rect(r.x, y + SI(150), r.w, SI(24)), l2, col_alpha(T->panel_text, 0.6f));
        draw_text_c(f, S(12), rect(r.x, r.y + r.h - SI(40), r.w, SI(24)), pal_audio_info(), col_alpha(T->panel_text, 0.4f));
        return;
    }
    /* capa abstrata */
    Rect art = rect(x, y, SI(64), SI(64));
    gfx_rrect(art, S(12), col_mix(T->accent, T->panel, 0.15f));
    if (playing) {
        for (int b = 0; b < 4; b++) {
            float ph = (float)(ui.in.now % 100000) / 1000.0f * (2.2f + b * 0.7f) + b;
            float hgt = S(10) + S(20) * (0.5f + 0.5f * f_sin(ph * 3.1f));
            gfx_rrect(rect(art.x + SI(14) + b * SI(10), (int)(art.y + art.h - S(14) - hgt), SI(6), (int)hgt), S(3), 0xE0FFFFFF);
        }
        ui.redraw = true;
    } else icon_draw(IC_MUSIC, art.x + art.w * 0.5f, art.y + art.h * 0.5f, S(30), 0xE0FFFFFF);
    float tx = (float)(art.x + art.w + SI(16));
    const char *t1 = cur >= 0 ? (title[0] ? title : tracks[cur].name) : L("Ready to play");
    const char *t2 = cur >= 0 ? (artist[0] ? artist : pathfmt(L("USB drive · %s"), DIR_MUSIC)) : L("Pick a track or press F8");
    draw_text_ellipsis(fb, S(16), tx, y + S(28), x + wd - tx, t1, T->panel_text);
    draw_text_ellipsis(f, S(13.5f), tx, y + S(50), x + wd - tx, t2, col_alpha(T->panel_text, 0.6f));
    y += SI(84);
    /* progresso */
    float el = src_rate > 0 ? (float)frames_played / src_rate : 0;
    float frac = duration > 0 ? CLAMP(el / duration, 0.0f, 1.0f) : 0;
    Rect pb = rect(x, y, wd, SI(16));
    float cy = pb.y + pb.h * 0.5f;
    gfx_rrect(rect(pb.x, (int)(cy - S(2)), pb.w, SI(4)), S(2), col_alpha(T->text, 0.12f));
    gfx_rrect(rect(pb.x, (int)(cy - S(2)), (int)(pb.w * frac), SI(4)), S(2), T->accent);
    if (ui_hover(pb)) { gfx_circle(pb.x + pb.w * frac, cy, S(6), T->accent); ui.cursor = 2; if (ui.in.pressed) seek_frac((float)(ui.in.mx - pb.x) / pb.w); }
    char a[16], b[16]; fmt_time(el, a, sizeof a); fmt_time(duration, b, sizeof b);
    draw_text(f, S(12), x, y + S(34), a, -1, col_alpha(T->panel_text, 0.55f));
    draw_text(f, S(12), x + wd - text_width(f, S(12), b, -1), y + S(34), b, -1, col_alpha(T->panel_text, 0.55f));
    y += SI(48);
    /* controles */
    int cxm = r.x + r.w / 2, bs = SI(40), big = SI(56);
    if (ui_icon_btn(rect(cxm - big / 2 - SI(14) - bs * 2 - SI(10), y + (big - bs) / 2, bs, bs), IC_SHUFFLE, prefs.shuffle, L("Shuffle"))) { prefs.shuffle = !prefs.shuffle; shuffle_order(); prefs_save(); }
    if (ui_icon_btn(rect(cxm - big / 2 - SI(14) - bs, y + (big - bs) / 2, bs, bs), IC_PREV, false, L("Previous (F7)"))) music_next(-1);
    Rect pp = rect(cxm - big / 2, y, big, big);
    bool hv = ui_hover(pp);
    gfx_shadow(rect(pp.x, pp.y + SI(3), pp.w, pp.h), big * 0.5f, S(10), col_alpha(T->accent, 0.35f));
    gfx_circle(cxm, y + big * 0.5f, big * 0.5f, hv ? col_lum_shift(T->accent, 0.08f) : T->accent);
    icon_draw(playing ? IC_PAUSE : IC_PLAY, cxm + (playing ? 0 : S(1.5f)), y + big * 0.5f, S(24), col_luma(T->accent) > 0.6f ? 0xFF1A1A1A : 0xFFFFFFFF);
    if (hv) ui.cursor = 2;
    if (ui.in.pressed && hv) music_toggle();
    if (ui_icon_btn(rect(cxm + big / 2 + SI(14), y + (big - bs) / 2, bs, bs), IC_NEXT, false, L("Next (F9)"))) music_next(1);
    if (ui_icon_btn(rect(cxm + big / 2 + SI(24) + bs, y + (big - bs) / 2, bs, bs), IC_REPEAT, prefs.repeat, L("Repeat list"))) { prefs.repeat = !prefs.repeat; prefs_save(); }
    y += big + SI(14);
    /* volume */
    float v = (float)prefs.volume;
    icon_draw(IC_VOLUME, x + S(10), y + S(39), S(18), col_alpha(T->panel_text, 0.6f));
    char vt[16]; fmt(vt, sizeof vt, "%d%%", prefs.volume);
    if (ui_slider(rect(x + SI(30), y, wd - SI(30), SI(50)), &v, 0, 100, L("Drive"), vt)) { prefs.volume = (int)(v + 0.5f); }
    if (ui.active == 0 && mvol != prefs.volume) { mvol = prefs.volume; prefs_save(); }
    y += SI(62);
    gfx_rect(rect(r.x + SI(1), y, r.w - SI(2), 1), col_alpha(T->border, 0.6f));
    /* lista */
    Rect lr = rect(r.x + SI(8), y + SI(6), r.w - SI(16), r.y + r.h - y - SI(30));
    int rh = SI(38);
    float content = (float)ntracks * rh;
    if (ui_hover(lr) && ui.in.wheel) list_scroll -= ui.in.wheel * rh * 3;
    list_scroll = CLAMP(list_scroll, 0.0f, MAX(0.0f, content - lr.h));
    gfx_push_clip(lr);
    for (int i = (int)(list_scroll / rh); i < ntracks; i++) {
        int ry = lr.y + i * rh - (int)list_scroll;
        if (ry > lr.y + lr.h) break;
        Rect it = rect(lr.x, ry, lr.w - SI(10), rh - SI(2));
        bool h2 = ui_hover(it) && ui_hover(lr), on = i == cur;
        if (on) gfx_rrect(it, S(8), col_alpha(T->accent, 0.14f)); else if (h2) gfx_rrect(it, S(8), col_alpha(T->text, 0.05f));
        char num[8]; fmt(num, sizeof num, "%d", i + 1);
        if (on && playing) icon_draw(IC_VOLUME, it.x + S(20), it.y + it.h * 0.5f, S(15), T->accent);
        else draw_text_c(f, S(12.5f), rect(it.x + SI(4), it.y, SI(32), it.h), num, col_alpha(T->panel_text, 0.45f));
        draw_text_ellipsis(on ? fb : f, S(14), it.x + S(42), it.y + it.h * 0.5f + face_ascent(f, S(14)) * 0.36f, it.w - S(50), tracks[i].name, on ? T->accent : T->panel_text);
        if (h2) ui.cursor = 2;
        if (ui.in.pressed && h2) { if (start_track(i)) fail_count = 0; }
    }
    gfx_pop_clip();
    ui_scrollbar(rect(lr.x + lr.w - SI(6), lr.y, SI(5), lr.h), &list_scroll, content, (float)lr.h);
    char info[160]; fmt(info, sizeof info, LN("%d track · %s", "%d tracks · %s", ntracks), ntracks, pal_audio_info());
    draw_text_ellipsis(f, S(11.5f), r.x + SI(16), r.y + r.h - SI(10), r.w - SI(32), info, col_alpha(T->panel_text, 0.4f));
}
