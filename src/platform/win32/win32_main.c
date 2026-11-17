/* FOLIO v4 — simulador Win32 (desenvolvimento/testes). Mesmo núcleo do app UEFI.
 *   folio_sim.exe [--root DIR] [--vol2 DIR] [--size WxH] [--script FILE --out DIR]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#undef MOD_SHIFT
#undef MOD_ALT
#undef MOD_CONTROL
#undef RGB
#include "../../core/folio.h"

static Surface screen;
static HWND hwnd;
static BITMAPINFO bmi;
static char roots[4][MAX_PATH]; static int nroots = 1;
static Event q[2048]; static int qh, qt;
static bool headless; static u64 fake_ms;
static LARGE_INTEGER qpf, qp0;

static void push(Event *e) { int n = (qt + 1) % 2048; if (n == qh) return; q[qt] = *e; qt = n; }

/* ------------------------------------------------------------ PAL: tela/tempo */
Surface *pal_screen(void) { return &screen; }
void pal_present(Rect r) { (void)r; if (!headless) { InvalidateRect(hwnd, 0, FALSE); UpdateWindow(hwnd); } }
u64 pal_ms(void) {
    if (headless) return fake_ms;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (u64)((t.QuadPart - qp0.QuadPart) * 1000 / qpf.QuadPart);
}
void pal_now(DateTime *t) { SYSTEMTIME s; GetLocalTime(&s); t->year = s.wYear; t->month = s.wMonth; t->day = s.wDay; t->hour = s.wHour; t->min = s.wMinute; t->sec = s.wSecond; }
const char *pal_name(void) { return "Windows simulator"; }
void pal_power(int a) { (void)a; PostQuitMessage(0); }
void pal_set_pointer_speed(float s) { (void)s; }
void pal_set_touch_mode(int m) { (void)m; }
void pal_audio_set_gpio(int on) { (void)on; }
void pal_audio_probe(void) {}
void pal_video_fast(bool on) { (void)on; }
void pal_set_rotation(int pref) { (void)pref; }
int  pal_rotation(void) { return 0; }
void pal_set_video_mode(const char *pref) { (void)pref; }
void pal_video_mode(int *w, int *h) { *w = screen.w; *h = screen.h; }
int  pal_video_modes(int *ws, int *hs, int max) { if (max < 1) return 0; ws[0] = screen.w; hs[0] = screen.h; return 1; }
float pal_display_dpi(void) { return 0; }
void pal_log(const char *s) { fputs(s, stderr); }
void pal_diag(Buf *o) { buf_printf(o, "Windows simulator (no direct hardware access)\r\nAudio: %s\r\n", pal_audio_info()); }
int pal_mods(void) {
    int m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000) m |= MOD_ALT;
    return m;
}

bool pal_poll(Event *e) {
    if (!headless) {
        MSG m;
        while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { Event x = { 0 }; x.type = EV_QUIT; push(&x); break; }
            TranslateMessage(&m); DispatchMessageW(&m);
        }
    }
    if (qh == qt) return false;
    *e = q[qh]; qh = (qh + 1) % 2048;
    return true;
}
void pal_wait(int ms) { if (!headless) MsgWaitForMultipleObjects(0, 0, FALSE, (DWORD)ms, QS_ALLINPUT); }

/* ------------------------------------------------------------ PAL: arquivos */
static void to_w(int vol, const char *path, wchar_t *out, int cap) {
    char full[1024];
    snprintf(full, sizeof full, "%s%s", roots[vol], path);
    for (char *c = full; *c; c++) if (*c == '/') *c = '\\';
    int n = (int)strlen(full);
    while (n > 3 && full[n - 1] == '\\') full[--n] = 0;
    MultiByteToWideChar(CP_UTF8, 0, full, -1, out, cap);
}
int pal_vol_count(void) { return nroots; }
bool pal_vol_info(int v, VolInfo *o) {
    if (v < 0 || v >= nroots) return false;
    memset(o, 0, sizeof *o);
    o->boot = v == 0;
    snprintf(o->label, sizeof o->label, v == 0 ? "FOLIO" : "BACKUP");
    return true;
}
static void ft_to_dt(FILETIME ft, DateTime *d) {
    FILETIME lt; SYSTEMTIME s; FileTimeToLocalFileTime(&ft, &lt); FileTimeToSystemTime(&lt, &s);
    d->year = s.wYear; d->month = s.wMonth; d->day = s.wDay; d->hour = s.wHour; d->min = s.wMinute; d->sec = s.wSecond;
}
int pal_list(int vol, const char *path, FileEntry **out) {
    wchar_t w[1024]; to_w(vol, path, w, 1000);
    wcscat(w, L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    *out = 0;
    if (h == INVALID_HANDLE_VALUE) return -1;
    int n = 0, cap = 64;
    FileEntry *l = mem_alloc(sizeof(FileEntry) * (usize)cap);
    do {
        if (n >= cap) { cap *= 2; l = mem_realloc(l, sizeof(FileEntry) * (usize)cap); }
        FileEntry *e = &l[n];
        memset(e, 0, sizeof *e);
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, e->name, sizeof e->name, 0, 0);
        if (!strcmp(e->name, ".") || !strcmp(e->name, "..")) continue;
        e->dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e->size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        ft_to_dt(fd.ftLastWriteTime, &e->mtime);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    *out = l;
    return n;
}
bool pal_read(int vol, const char *path, u8 **data, usize *len) {
    wchar_t w[1024]; to_w(vol, path, w, 1000);
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, 0), rd = 0;
    u8 *b = mem_alloc(sz + 1);
    ReadFile(h, b, sz, &rd, 0);
    CloseHandle(h);
    b[rd] = 0; *data = b; *len = rd;
    return true;
}
bool pal_write(int vol, const char *path, const void *data, usize len) {
    wchar_t w[1024], t[1040]; to_w(vol, path, w, 1000);
    swprintf(t, 1040, L"%ls.tmp", w);
    HANDLE h = CreateFileW(t, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0; BOOL ok = WriteFile(h, data, (DWORD)len, &wr, 0);
    FlushFileBuffers(h); CloseHandle(h);
    if (!ok || wr != len) { DeleteFileW(t); return false; }
    return MoveFileExW(t, w, MOVEFILE_REPLACE_EXISTING) != 0;
}
bool pal_mkdir(int vol, const char *path) { wchar_t w[1024]; to_w(vol, path, w, 1000); return CreateDirectoryW(w, 0) != 0; }
bool pal_remove(int vol, const char *path) {
    wchar_t w[1024]; to_w(vol, path, w, 1000);
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(w) != 0 : DeleteFileW(w) != 0;
}
bool pal_rename(int vol, const char *path, const char *nn) {
    char np[1024]; snprintf(np, sizeof np, "%s", path);
    char *slash = strrchr(np, '/'); if (slash) slash[1] = 0; else np[0] = 0;
    strncat(np, nn, sizeof np - strlen(np) - 1);
    wchar_t a[1024], b[1024]; to_w(vol, path, a, 1000); to_w(vol, np, b, 1000);
    return MoveFileW(a, b) != 0;
}
bool pal_exists(int vol, const char *path, bool *is_dir) {
    wchar_t w[1024]; to_w(vol, path, w, 1000);
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    if (is_dir) *is_dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}
struct PalFile { HANDLE h; };
PalFile *pal_fopen(int vol, const char *path, u64 *size) {
    wchar_t w[1024]; to_w(vol, path, w, 1000);
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER s; GetFileSizeEx(h, &s); *size = (u64)s.QuadPart;
    PalFile *f = mem_alloc(sizeof *f); f->h = h; return f;
}
int pal_fread(PalFile *f, void *b, int n) { DWORD r = 0; if (!ReadFile(f->h, b, (DWORD)n, &r, 0)) return -1; return (int)r; }
bool pal_fseek(PalFile *f, u64 p) { LARGE_INTEGER li; li.QuadPart = (LONGLONG)p; return SetFilePointerEx(f->h, li, 0, FILE_BEGIN) != 0; }
void pal_fclose(PalFile *f) { if (f) { CloseHandle(f->h); mem_free(f); } }

/* ------------------------------------------------------------ PAL: áudio (waveOut) */
#define NBUF 8
#define BUFF 2048
static HWAVEOUT wo; static WAVEHDR hdr[NBUF]; static i16 *abuf[NBUF]; static int afill, acur, arate;
bool pal_audio_start(int rate) {
    if (headless) return false;
    pal_audio_stop();
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 2, (DWORD)rate, (DWORD)rate * 4, 4, 16, 0 };
    if (waveOutOpen(&wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wo = 0; return false; }
    for (int i = 0; i < NBUF; i++) {
        if (!abuf[i]) abuf[i] = malloc(BUFF * 4);
        memset(&hdr[i], 0, sizeof hdr[i]);
        hdr[i].lpData = (LPSTR)abuf[i]; hdr[i].dwBufferLength = BUFF * 4; hdr[i].dwFlags = WHDR_DONE;
    }
    afill = 0; acur = 0; arate = rate;
    return true;
}
int pal_audio_rate(void) { return arate; }
int pal_audio_space(void) {
    if (!wo) return 0;
    int n = 0;
    for (int k = 0; k < NBUF; k++) { int i = (acur + k) % NBUF; if (!(hdr[i].dwFlags & WHDR_DONE)) break; n += (k == 0) ? BUFF - afill : BUFF; }
    return n;
}
void pal_audio_write(const i16 *fr, int n) {
    while (n > 0 && wo) {
        WAVEHDR *h = &hdr[acur];
        if (!(h->dwFlags & WHDR_DONE)) return;
        int k = MIN(n, BUFF - afill);
        memcpy(abuf[acur] + afill * 2, fr, (size_t)k * 4);
        afill += k; fr += k * 2; n -= k;
        if (afill == BUFF) {
            if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, h, sizeof *h);
            h->dwFlags = 0; h->dwBufferLength = BUFF * 4;
            waveOutPrepareHeader(wo, h, sizeof *h);
            waveOutWrite(wo, h, sizeof *h);
            acur = (acur + 1) % NBUF; afill = 0;
        }
    }
}
void pal_audio_stop(void) {
    if (!wo) return;
    waveOutReset(wo);
    for (int i = 0; i < NBUF; i++) if (hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]);
    waveOutClose(wo); wo = 0;
}
const char *pal_audio_info(void) { return headless ? "no audio (test mode)" : "Windows waveOut (simulator)"; }

/* ------------------------------------------------------------ janela */
static int map_vk(WPARAM vk) {
    switch (vk) {
    case VK_ESCAPE: return KEY_ESC; case VK_RETURN: return KEY_ENTER; case VK_TAB: return KEY_TAB;
    case VK_BACK: return KEY_BACKSPACE; case VK_DELETE: return KEY_DELETE; case VK_INSERT: return KEY_INSERT;
    case VK_LEFT: return KEY_LEFT; case VK_RIGHT: return KEY_RIGHT; case VK_UP: return KEY_UP; case VK_DOWN: return KEY_DOWN;
    case VK_HOME: return KEY_HOME; case VK_END: return KEY_END; case VK_PRIOR: return KEY_PGUP; case VK_NEXT: return KEY_PGDN;
    default: if (vk >= VK_F1 && vk <= VK_F12) return KEY_F1 + (int)(vk - VK_F1); return 0;
    }
}
static void resize_surface(int w, int h) {
    free(screen.px);
    screen.w = w; screen.h = h; screen.stride = w;
    screen.px = calloc((size_t)w * h, 4);
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader; bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
}

static unsigned held_btns;          /* botões apertados (bit 0 esq, 1 dir, 2 meio) */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Event e = { 0 };
    switch (m) {
    case WM_CLOSE: e.type = EV_QUIT; push(&e); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SIZE: {
        int w = LOWORD(lp), hh = HIWORD(lp);
        if (w > 64 && hh > 64 && (w != screen.w || hh != screen.h)) { resize_surface(w, hh); app_request_redraw(); }
        return 0; }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        SetDIBitsToDevice(dc, 0, 0, (DWORD)screen.w, (DWORD)screen.h, 0, 0, 0, (UINT)screen.h, screen.px, &bmi, DIB_RGB_COLORS);
        EndPaint(h, &ps); return 0; }
    case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) { SetCursor(0); return TRUE; } break;
    case WM_MOUSEMOVE: e.type = EV_MOVE; e.x = (short)LOWORD(lp); e.y = (short)HIWORD(lp); push(&e); return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        e.button = m == WM_LBUTTONDOWN ? 0 : m == WM_RBUTTONDOWN ? 1 : 2;
        held_btns |= 1u << e.button;
        SetCapture(h); e.type = EV_DOWN; e.x = (short)LOWORD(lp); e.y = (short)HIWORD(lp); push(&e); return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        e.button = m == WM_LBUTTONUP ? 0 : m == WM_RBUTTONUP ? 1 : 2;
        held_btns &= ~(1u << e.button);
        /* a captura só é solta quando nenhum botão está apertado: soltá-la no primeiro "up" fazia o
         * "up" de outro botão ainda seguro (ex.: o do meio) se perder fora da janela */
        if (!held_btns) ReleaseCapture();
        e.type = EV_UP; e.x = (short)LOWORD(lp); e.y = (short)HIWORD(lp); push(&e); return 0;
    case WM_CAPTURECHANGED: case WM_KILLFOCUS:
        /* perdeu a captura/o foco (Alt+Tab, outra janela) com botões apertados: solta todos,
         * senão um arrasto do botão do meio continuaria rolando sozinho */
        if (m == WM_CAPTURECHANGED && (HWND)lp == h) break;
        for (int b = 0; b < 3; b++) if (held_btns & (1u << b)) { Event u = { 0 }; u.type = EV_UP; u.button = b; push(&u); }
        held_btns = 0;
        break;
    case WM_MOUSEWHEEL: {
        /* touchpads de precisão mandam frações de "dente" (delta < 120), inclusive a inércia depois que
         * o dedo sai; contar cada mensagem como um dente inteiro fazia a página seguir rolando sozinha.
         * A sobra de um gesto expira em 400 ms, para não virar um dente no gesto seguinte. */
        static int acc; static DWORD last_t;
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        DWORD t = GetMessageTime();
        if (t - last_t > 400) acc = 0;
        last_t = t;
        if ((d > 0) != (acc > 0)) acc = 0;
        acc += d;
        while (acc >= WHEEL_DELTA || acc <= -WHEEL_DELTA) {
            e.type = EV_WHEEL; e.wheel = acc > 0 ? 1 : -1; push(&e);
            acc -= e.wheel * WHEEL_DELTA;
        }
        return 0; }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        int mods = pal_mods();
        bool altgr = (mods & MOD_CTRL) && (mods & MOD_ALT);
        int k = map_vk(wp);
        if (k) { e.type = EV_KEY; e.key = k; e.mods = mods; push(&e); return 0; }
        /* Alt+letra/número (Alt+M menu, Alt+1…9 abas): o Windows manda WM_SYSCHAR, que é ignorado */
        if ((mods & MOD_ALT) && !(mods & MOD_CTRL) && ((wp >= 'A' && wp <= 'Z') || (wp >= '0' && wp <= '9'))) {
            e.type = EV_KEY; e.ch = (u32)(wp >= 'A' && wp <= 'Z' ? wp - 'A' + 'a' : wp); e.mods = mods; push(&e); return 0;
        }
        if ((mods & MOD_CTRL) && !altgr) {
            u32 ch = 0;
            if (wp >= 'A' && wp <= 'Z') ch = (u32)(wp - 'A' + 'a');
            else if (wp >= '0' && wp <= '9') ch = (u32)wp;
            else if (wp == VK_OEM_PLUS) ch = '=';
            else if (wp == VK_OEM_MINUS) ch = '-';
            else if (wp == VK_SPACE) ch = ' ';          /* Ctrl+Espaço: método de entrada */
            if (ch) { e.type = EV_KEY; e.ch = ch; e.mods = mods; push(&e); return 0; }
        }
        if (m == WM_SYSKEYDOWN && wp == VK_F4) { e.type = EV_QUIT; push(&e); return 0; }
        break; }
    case WM_CHAR: {
        int mods = pal_mods();
        bool altgr = (mods & MOD_CTRL) && (mods & MOD_ALT);
        if (wp < 32 || wp == 127) return 0;
        if ((mods & MOD_CTRL) && !altgr) return 0;
        e.type = EV_KEY; e.ch = (u32)wp; e.mods = mods & ~(MOD_CTRL | MOD_ALT); push(&e); return 0; }
    case WM_SYSCHAR: return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* ------------------------------------------------------------ PNG (para capturas) */
static u32 crc_t[256];
static u32 crc_up(u32 c, const u8 *b, size_t n) { for (size_t i = 0; i < n; i++) c = crc_t[(c ^ b[i]) & 255] ^ (c >> 8); return c; }
static void be32(FILE *f, u32 v) { u8 b[4] = { (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v }; fwrite(b, 1, 4, f); }
static void chunk(FILE *f, const char *t, const u8 *d, u32 n) {
    be32(f, n); fwrite(t, 1, 4, f); if (n) fwrite(d, 1, n, f);
    u32 c = crc_up(0xFFFFFFFFu, (const u8 *)t, 4); c = crc_up(c, d, n); be32(f, c ^ 0xFFFFFFFFu);
}
static void write_png(const char *path, Surface *s) {
    for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_t[i] = c; }
    size_t raw_n = (size_t)s->h * (s->w * 3 + 1);
    u8 *raw = malloc(raw_n);
    for (int y = 0; y < s->h; y++) {
        u8 *r = raw + (size_t)y * (s->w * 3 + 1); r[0] = 0;
        for (int x = 0; x < s->w; x++) { u32 p = s->px[y * s->stride + x]; r[1 + x * 3] = (u8)(p >> 16); r[2 + x * 3] = (u8)(p >> 8); r[3 + x * 3] = (u8)p; }
    }
    size_t nb = (raw_n + 65534) / 65535;
    u8 *z = malloc(raw_n + nb * 5 + 6); size_t zn = 0;
    z[zn++] = 0x78; z[zn++] = 0x01;
    u32 a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n; off += 65535) {
        u32 len = (u32)MIN((size_t)65535, raw_n - off);
        z[zn++] = off + len >= raw_n ? 1 : 0;
        z[zn++] = (u8)len; z[zn++] = (u8)(len >> 8); z[zn++] = (u8)~len; z[zn++] = (u8)(~len >> 8);
        memcpy(z + zn, raw + off, len); zn += len;
    }
    u32 ad = (b << 16) | a; z[zn++] = (u8)(ad >> 24); z[zn++] = (u8)(ad >> 16); z[zn++] = (u8)(ad >> 8); z[zn++] = (u8)ad;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    u8 ih[13] = { 0 };
    ih[0] = (u8)(s->w >> 24); ih[1] = (u8)(s->w >> 16); ih[2] = (u8)(s->w >> 8); ih[3] = (u8)s->w;
    ih[4] = (u8)(s->h >> 24); ih[5] = (u8)(s->h >> 16); ih[6] = (u8)(s->h >> 8); ih[7] = (u8)s->h;
    ih[8] = 8; ih[9] = 2;
    chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (u32)zn); chunk(f, "IEND", 0, 0);
    fclose(f); free(raw); free(z);
}

/* ------------------------------------------------------------ roteiro (testes headless) */
static void frame(void) { app_frame(); }
static void advance(int ms) { while (ms > 0) { int st = MIN(ms, 16); fake_ms += (u64)st; ms -= st; Event e; while (pal_poll(&e)) app_event(&e); music_update(); frame(); } }
static int parse_key(const char *s, int *mods) {
    *mods = 0;
    while (1) {
        if (!_strnicmp(s, "ctrl+", 5)) { *mods |= MOD_CTRL; s += 5; }
        else if (!_strnicmp(s, "shift+", 6)) { *mods |= MOD_SHIFT; s += 6; }
        else if (!_strnicmp(s, "alt+", 4)) { *mods |= MOD_ALT; s += 4; }
        else break;
    }
    static const struct { const char *n; int k; } t[] = {
        { "enter", KEY_ENTER }, { "esc", KEY_ESC }, { "tab", KEY_TAB }, { "backspace", KEY_BACKSPACE }, { "delete", KEY_DELETE },
        { "left", KEY_LEFT }, { "right", KEY_RIGHT }, { "up", KEY_UP }, { "down", KEY_DOWN }, { "home", KEY_HOME }, { "end", KEY_END },
        { "pgup", KEY_PGUP }, { "pgdn", KEY_PGDN },
        { "f1", KEY_F1 }, { "f2", KEY_F2 }, { "f3", KEY_F3 }, { "f4", KEY_F4 }, { "f5", KEY_F5 }, { "f6", KEY_F6 },
        { "f7", KEY_F7 }, { "f8", KEY_F8 }, { "f9", KEY_F9 }, { "f10", KEY_F10 }, { "f11", KEY_F11 }, { "f12", KEY_F12 },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!_stricmp(s, t[i].n)) return t[i].k;
    if (!_stricmp(s, "space")) return -(int)' ';
    return -(int)(unsigned char)s[0];   /* letra com modificador */
}
static Rect mark_r;
static int run_script(const char *file, const char *outdir) {
    FILE *f = fopen(file, "rb");
    if (!f) { fprintf(stderr, "roteiro não encontrado: %s\n", file); return 1; }
    char line[4096];
    app_init();
    advance(100);
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        char *arg = strchr(line, ' '); if (arg) *arg++ = 0; else arg = line + n;
        Event e = { 0 };
        if (!strcmp(line, "type") || !strcmp(line, "raw")) {
            int L = (int)strlen(arg);
            for (int i = 0; i < L; ) {
                u32 cp; int k = utf8_decode(arg, L, i, &cp); i += k;
                if (cp == 0xB6) { e.type = EV_KEY; e.key = KEY_ENTER; e.ch = 0; push(&e); advance(16); continue; }
                e.type = EV_KEY; e.key = 0; e.ch = cp; e.us_raw = !strcmp(line, "raw"); push(&e); advance(16);
            }
        } else if (!strcmp(line, "key")) {
            int mods; int k = parse_key(arg, &mods);
            e.type = EV_KEY; e.mods = mods;
            if (k > 0) e.key = k; else e.ch = (u32)(-k);
            push(&e); advance(30);
        } else if (!strcmp(line, "move")) { e.type = EV_MOVE; sscanf(arg, "%d %d", &e.x, &e.y); push(&e); advance(30); }
        else if (!strcmp(line, "mclick")) {        /* botão do meio: clique curto */
            int x, y; sscanf(arg, "%d %d", &x, &y);
            e.type = EV_MOVE; e.x = x; e.y = y; push(&e); advance(16);
            e.type = EV_DOWN; e.button = 2; push(&e); advance(50);
            e.type = EV_UP; push(&e); advance(60);
        } else if (!strcmp(line, "barclick")) {    /* clica num botão da barra de leitura pelo nome */
            static const char *nm[BAR_COUNT] = { "anterior", "play", "devagar", "velocidade", "depressa", "proxima" };
            int i = 0; while (i < BAR_COUNT && strcmp(arg, nm[i])) i++;
            if (i >= BAR_COUNT) { fprintf(stderr, "FALHA: botão da barra desconhecido: %s\n", arg); return 2; }
            Rect r = preview_bar_button(i);
            e.type = EV_MOVE; e.x = r.x + r.w / 2; e.y = r.y + r.h / 2; push(&e); advance(16);
            e.type = EV_DOWN; e.button = 0; push(&e); advance(16);
            e.type = EV_UP; push(&e); advance(60);
        } else if (!strcmp(line, "assert_still") || !strcmp(line, "assert_moving")) {
            /* a leitura fica parada (ou anda) durante N ms: mede a posição de rolagem, não o parágrafo */
            bool still = !strcmp(line, "assert_still");
            float a = preview_scroll_pos(); advance(atoi(arg)); float b = preview_scroll_pos();
            float d = b - a; if (d < 0) d = -d;
            if (still ? d > 0.01f : d < 1.0f) { fprintf(stderr, "FALHA: %s %s: a rolagem foi de %.2f a %.2f\n", line, arg, a, b); return 2; }
            printf("ok: %s (%.2f -> %.2f)\n", line, a, b);
        } else if (!strcmp(line, "assert_auto")) {
            bool want = atoi(arg) != 0;
            if (preview_auto_on() != want) { fprintf(stderr, "FALHA: rolagem automática %s\n", want ? "desligada" : "ligada"); return 2; }
            printf("ok: rolagem automática %s\n", want ? "ligada" : "desligada");
        } else if (!strcmp(line, "mdown") || !strcmp(line, "mup")) {
            e.type = !strcmp(line, "mdown") ? EV_DOWN : EV_UP; e.button = 2; e.x = ui.in.mx; e.y = ui.in.my; push(&e); advance(16);
        }
        else if (!strcmp(line, "click") || !strcmp(line, "dblclick")) {
            int reps = !strcmp(line, "click") ? 1 : 2;
            int x, y; sscanf(arg, "%d %d", &x, &y);
            e.type = EV_MOVE; e.x = x; e.y = y; push(&e); advance(16);
            for (int r = 0; r < reps; r++) {
                e.type = EV_DOWN; e.button = 0; push(&e); advance(16);
                e.type = EV_UP; push(&e); advance(16);
            }
            advance(60);
        } else if (!strcmp(line, "wheel")) { e.type = EV_WHEEL; e.wheel = atoi(arg); push(&e); advance(300); }
        else if (!strcmp(line, "wait")) advance(atoi(arg));
        else if (!strcmp(line, "bench")) {
            /* bench N modo: mede o tempo real de app_frame (modo: type = digitando, idle = só cursor, scroll) */
            int N = 200; char mode[32] = "type"; sscanf(arg, "%d %31s", &N, mode);
            LARGE_INTEGER q0, q1, fq; QueryPerformanceFrequency(&fq);
            double tot = 0, worst = 0;
            for (int i = 0; i < N; i++) {
                if (!strcmp(mode, "type")) { e.type = EV_KEY; e.key = 0; e.ch = "abcde fghij "[i % 12]; e.us_raw = false; app_event(&e); }
                else if (!strcmp(mode, "scroll")) { e.type = EV_WHEEL; e.wheel = (i / 40) % 2 ? 1 : -1; app_event(&e); }
                else if (!strcmp(mode, "win")) { if (i % 40 == 0 || i % 40 == 20) { e.type = EV_KEY; e.ch = 0; e.key = i % 40 ? KEY_ESC : KEY_F12; app_event(&e); } }
                else if (!strcmp(mode, "move")) { e.type = EV_MOVE; e.x = 200 + (i * 7) % 900; e.y = 300 + (i * 3) % 300; app_event(&e); }
                fake_ms += 16;
                QueryPerformanceCounter(&q0);
                app_frame();
                QueryPerformanceCounter(&q1);
                double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)fq.QuadPart;
                tot += ms; if (ms > worst) worst = ms;
            }
            printf("bench %s %dx%d: média %.2f ms/quadro (%.0f fps), pior %.2f ms\n", mode, screen.w, screen.h, tot / N, 1000.0 / (tot / N), worst);
        }
        else if (!strcmp(line, "shot")) {
            char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", outdir, arg);
            advance(16);
            write_png(p, &screen);
            printf("captura: %s\n", p);
        } else if (!strcmp(line, "theme")) { app_set_theme(atoi(arg)); advance(50); }
        else if (!strcmp(line, "font")) { prefs.font = atoi(arg); app_apply_font(); advance(50); }
        else if (!strcmp(line, "lang")) {
            /* lang <código>: troca o idioma como o usuário faria em Ajustes */
            int i = lang_find(arg);
            if (i < 0) { fprintf(stderr, "FALHA: idioma %s não encontrado em FOLIO/LANGUAGES\n", arg); return 2; }
            app_set_language(i, true); advance(50);
        } else if (!strcmp(line, "assert_lang")) {
            if (strcmp(i18n_code(), arg)) { fprintf(stderr, "FALHA: idioma %s (esperado %s)\n", i18n_code(), arg); return 2; }
            printf("ok: idioma %s (%d traduções)\n", arg, i18n_loaded());
        } else if (!strcmp(line, "assert_tr")) {
            /* assert_tr <original> => <tradução esperada> */
            char *sep = strstr(arg, " => ");
            if (!sep) { fprintf(stderr, "FALHA: assert_tr sem ' => '\n"); return 2; }
            *sep = 0;
            const char *got = L(arg);
            if (strcmp(got, sep + 4)) { fprintf(stderr, "FALHA: L(\"%s\") = \"%s\" (esperado \"%s\")\n", arg, got, sep + 4); return 2; }
            printf("ok: %s => %s\n", arg, got);
        } else if (!strcmp(line, "assert_plural")) {
            /* assert_plural <n> <singular> | <plural> => <esperado> */
            long n = strtol(arg, 0, 10); char *p = strchr(arg, ' ');
            char *bar = p ? strstr(p, " | ") : 0, *sep = bar ? strstr(bar, " => ") : 0;
            if (!p || !bar || !sep) { fprintf(stderr, "FALHA: assert_plural mal formado\n"); return 2; }
            *bar = 0; *sep = 0;
            const char *got = LN(p + 1, bar + 3, n);
            if (strcmp(got, sep + 4)) { fprintf(stderr, "FALHA: LN(%ld) = \"%s\" (esperado \"%s\")\n", n, got, sep + 4); return 2; }
            printf("ok: plural %ld => %s\n", n, got);
        } else if (!strcmp(line, "assert_fmt")) {
            /* assert_fmt <formato> | <int> <int> <int> => <esperado>   (testa %1$d, %2$d…) */
            char *bar = strstr(arg, " | "), *sep = bar ? strstr(bar, " => ") : 0;
            if (!bar || !sep) { fprintf(stderr, "FALHA: assert_fmt mal formado\n"); return 2; }
            *bar = 0; *sep = 0;
            int a = 0, b = 0, c = 0; sscanf(bar + 3, "%d %d %d", &a, &b, &c);
            char out[200]; fmt(out, sizeof out, arg, a, b, c);
            if (strcmp(out, sep + 4)) { fprintf(stderr, "FALHA: fmt(\"%s\") = \"%s\" (esperado \"%s\")\n", arg, out, sep + 4); return 2; }
            printf("ok: fmt %s => %s\n", arg, out);
        } else if (!strcmp(line, "kbd")) { snprintf(prefs.kb, sizeof prefs.kb, "%s", arg); kbd_select(prefs.kb); advance(16); }
        else if (!strcmp(line, "pref")) {
            char k[64]; int v; if (sscanf(arg, "%63s %d", k, &v) == 2) {
                if (!strcmp(k, "focus")) prefs.focus_mode = v;
                else if (!strcmp(k, "typewriter")) prefs.typewriter = v;
                else if (!strcmp(k, "hide_markup")) { prefs.hide_markup = v; editor_invalidate(); }
                else if (!strcmp(k, "paper")) prefs.paper = v;
                else if (!strcmp(k, "fullwidth")) { prefs.full_width = v; editor_invalidate(); }
                else if (!strcmp(k, "column")) { prefs.column = (float)v; editor_invalidate(); }
                else if (!strcmp(k, "indent")) { prefs.first_indent = v; editor_invalidate(); }
                else if (!strcmp(k, "splash_secs")) { prefs.splash_secs = v; prefs_save(); }
            }
            advance(50);
        } else if (!strcmp(line, "assert_text")) {
            usize L; char *t = doc_text(&app.doc, &L);
            if (!strstr(t, arg)) { fprintf(stderr, "FALHA: texto não contém \"%s\"\n--- texto ---\n%s\n", arg, t); return 2; }
            printf("ok: contém \"%s\"\n", arg);
            mem_free(t);
        } else if (!strcmp(line, "assert_no_text")) {
            usize L; char *t = doc_text(&app.doc, &L);
            if (strstr(t, arg)) { fprintf(stderr, "FALHA: texto ainda contém \"%s\"\n", arg); return 2; }
            printf("ok: não contém \"%s\"\n", arg);
            mem_free(t);
        } else if (!strcmp(line, "assert_nowin")) {
            Window *w = wm_top();
            if (w) { fprintf(stderr, "FALHA: janela ainda aberta: %s\n", w->title); return 2; }
            printf("ok: nenhuma janela aberta\n");
        } else if (!strcmp(line, "markwin")) {         /* guarda a posição da janela do topo */
            Window *w = wm_top(); if (!w) { fprintf(stderr, "FALHA: markwin sem janela\n"); return 2; }
            mark_r = w->r; printf("ok: janela marcada\n");
        } else if (!strcmp(line, "assert_winmoved")) { /* dx dy dw dh esperados desde o markwin (0 = igual, 1 = mudou) */
            Window *w = wm_top(); int a[4] = { 0 }; sscanf(arg, "%d %d %d %d", &a[0], &a[1], &a[2], &a[3]);
            int d[4] = { w ? w->r.x - mark_r.x : 0, w ? w->r.y - mark_r.y : 0, w ? w->r.w - mark_r.w : 0, w ? w->r.h - mark_r.h : 0 };
            for (int k = 0; k < 4; k++) if ((d[k] != 0) != (a[k] != 0)) { fprintf(stderr, "FALHA: janela %d,%d %dx%d (mudança %d %d %d %d)\n", w ? w->r.x : 0, w ? w->r.y : 0, w ? w->r.w : 0, w ? w->r.h : 0, d[0], d[1], d[2], d[3]); return 2; }
            printf("ok: janela mudou %d %d %d %d\n", d[0], d[1], d[2], d[3]);
        } else if (!strcmp(line, "assert_keywin")) {     /* quem recebe o teclado: "text" ou o título (msgid) da janela */
            const char *got = ui.key_win ? ui.key_win->title : "text";
            if (strcmp(got, arg)) { fprintf(stderr, "FALHA: teclado em \"%s\" (esperado \"%s\")\n", got, arg); return 2; }
            printf("ok: teclado em %s\n", arg);
        } else if (!strcmp(line, "assert_para")) {
            int a = 0, b = -1; if (sscanf(arg, "%d %d", &a, &b) < 2) b = a;
            int p = preview_active() ? preview_top_para() : app.caret.p;
            if (p < a || p > b) { fprintf(stderr, "FALHA: parágrafo %d fora de [%d, %d]%s\n", p, a, b, preview_active() ? " (leitura)" : ""); return 2; }
            printf("ok: parágrafo %d%s\n", p, preview_active() ? " (leitura)" : "");
        } else if (!strcmp(line, "assert_file")) {
            bool d; if (!pal_exists(0, arg, &d)) { fprintf(stderr, "FALHA: arquivo ausente %s\n", arg); return 2; }
            printf("ok: arquivo %s\n", arg);
        } else if (!strcmp(line, "export")) {
            char fmtname[16], path[512]; sscanf(arg, "%15s %511s", fmtname, path);
            int fm = -1; for (int i = 0; i < FMT_COUNT; i++) if (!_stricmp(format_info(i)->ext + 1, fmtname)) fm = i;
            if (fm < 0) { fprintf(stderr, "formato inválido\n"); return 2; }
            app_save_as_done(0, path, fm); advance(50);
        } else if (!strcmp(line, "open")) { app_open_path(0, arg); advance(100); }
        else if (!strcmp(line, "restart")) {
            /* simula desligar e ligar: grava tudo como no fim do app_run e inicia de novo */
            app_flush_all(); prefs_save();
            app_init(); advance(100);
        } else if (!strcmp(line, "assert_tabs")) {
            int want = atoi(arg);
            if (app_tab_count() != want) { fprintf(stderr, "FALHA: %d abas (esperado %d)\n", app_tab_count(), want); return 2; }
            printf("ok: %d abas\n", want);
        } else if (!strcmp(line, "assert_active")) {
            int want = atoi(arg);
            if (app_tab_current() != want) { fprintf(stderr, "FALHA: aba ativa %d (esperado %d)\n", app_tab_current(), want); return 2; }
            printf("ok: aba ativa %d\n", want);
        } else if (!strcmp(line, "themename")) {
            int i = theme_find(arg);
            if (i < 0) { fprintf(stderr, "FALHA: tema não encontrado: %s\n", arg); return 2; }
            app_set_theme(i); advance(50);
        } else if (!strcmp(line, "assert_theme")) {
            if (strcmp(T->name, arg)) { fprintf(stderr, "FALHA: tema ativo \"%s\" (esperado \"%s\")\n", T->name, arg); return 2; }
            printf("ok: tema %s\n", arg);
        } else if (!strcmp(line, "assert_font")) {
            if (strcmp(font_get(prefs.font)->name, arg)) { fprintf(stderr, "FALHA: fonte \"%s\" (esperado \"%s\")\n", font_get(prefs.font)->name, arg); return 2; }
            printf("ok: fonte %s\n", arg);
        } else if (!strcmp(line, "assert_uifont")) {
            if (strcmp(font_ui()->name, arg)) { fprintf(stderr, "FALHA: fonte da interface \"%s\" (esperado \"%s\")\n", font_ui()->name, arg); return 2; }
            printf("ok: fonte da interface %s\n", arg);
        } else if (!strcmp(line, "theme_save_copy")) {
            /* grava uma cópia do tema ativo com outro nome e papel trocado (testa o gravador de FOLIO\TEMAS) */
            Theme t = *T; t.paper = RGB(0x123456);
            int i = theme_save_custom(&t, arg, "Spectral", "", 0);
            if (i < 0) { fprintf(stderr, "FALHA: não gravou o tema %s\n", arg); return 2; }
            printf("ok: tema gravado (%d)\n", i);
        } else if (!strcmp(line, "assert_splash")) {
            if (splash_active() != (atoi(arg) != 0)) { fprintf(stderr, "FALHA: abertura %s\n", splash_active() ? "ainda visível" : "não visível"); return 2; }
            printf("ok: abertura %s\n", atoi(arg) ? "visível" : "encerrada");
        }
        else fprintf(stderr, "comando desconhecido: %s\n", line);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    QueryPerformanceFrequency(&qpf); QueryPerformanceCounter(&qp0);
    SetConsoleOutputCP(CP_UTF8);
    static u8 *heap; usize hs = 512u << 20;
    heap = VirtualAlloc(0, hs, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    heap_init(heap, hs);
    int W = 1600, H = 900;
    const char *script = 0, *out = ".";
    snprintf(roots[0], MAX_PATH, "build\\usb_files");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) snprintf(roots[0], MAX_PATH, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--vol2") && i + 1 < argc) snprintf(roots[nroots++], MAX_PATH, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    }
    resize_surface(W, H);
    if (script) { headless = true; fake_ms = 1000; return run_script(script, out); }
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(0); wc.lpszClassName = L"FolioSim";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r = { 0, 0, W, H }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowW(L"FolioSim", L"FOLIO 4 — simulador", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
    app_run();
    return 0;
}

/* idioma do Windows ("pt-BR"); nos testes roteirizados, FOLIO_PLATFORM_LANG (ou nenhum), para o
 * resultado não depender da máquina em que os testes rodam */
bool pal_lang(char *out, int cap) {
    if (headless) {
        const char *v = getenv("FOLIO_PLATFORM_LANG");
        if (!v || !v[0]) return false;
        snprintf(out, (size_t)cap, "%s", v);
        return true;
    }
    wchar_t w[LOCALE_NAME_MAX_LENGTH];
    if (!GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH)) return false;
    int n = 0;
    for (; w[n] && n < cap - 1; n++) out[n] = (char)w[n];
    out[n] = 0;
    return n >= 2;
}
