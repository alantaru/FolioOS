/* FOLIO v4 — hospedeiro UEFI para Windows x64.
 * Carrega o BOOTX64.EFI real (PE32+, aplica relocações) e o executa com uma
 * System Table emulada: GOP (janela), teclado "US cru" por scancode como um firmware,
 * mouse relativo (Simple Pointer), Simple File System sobre uma pasta, relógio e timers.
 *
 *   efi_host.exe BOOTX64.EFI --root build\usb_files [--size 1600x900] [--script roteiro.txt --out pasta]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../src/platform/uefi/efi.h"

#define NOT_FOUND EFI_ERR(14)
#define UNSUPPORTED EFI_ERR(3)
#define INVALID EFI_ERR(2)

static EFI_GUID G_GOP = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
static EFI_GUID G_TIEX = { 0xdd9e7534, 0x7762, 0x4698, { 0x8c, 0x14, 0xf5, 0x85, 0x17, 0xa6, 0x25, 0xaa } };
static EFI_GUID G_SPTR = { 0x31878c87, 0x0b75, 0x11d5, { 0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };
static EFI_GUID G_LIMG = { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID G_SFS = { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID G_FINFO = { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID G_FSINFO = { 0x09576e93, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static int geq(EFI_GUID *a, EFI_GUID *b) { return !memcmp(a, b, sizeof *a); }

static int W = 1600, H = 900;
static HWND hwnd; static BITMAPINFO bmi; static UINT32 *fbmem;
/* --gop: 0 bgr, 1 rgb, 2 mask16 (RGB565), 3 blt (sem framebuffer), 4 pad (linha maior que a tela), 5 uga (sem GOP) */
static int gopmode, SP; static int edid_on;
static UINT32 get_px(int x, int y) {
    if (gopmode == 2) { unsigned v = ((unsigned short *)fbmem)[y * SP + x]; unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31; return ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31); }
    UINT32 p = fbmem[y * SP + x];
    if (gopmode == 1) return ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
    return p & 0xFFFFFF;
}
static char root[MAX_PATH] = "build\\usb_files";
static int headless;
static LARGE_INTEGER qpf;
static FILE *logf;

/* ------------------------------------------------------------ handles e protocolos */
typedef struct { void *handle; EFI_GUID *guid; void *iface; } Reg;
static Reg regs[32]; static int nregs;
static int h_image, h_con, h_gop, h_mouse, h_fs;
static void reg(void *h, EFI_GUID *g, void *i) { regs[nregs].handle = h; regs[nregs].guid = g; regs[nregs].iface = i; nregs++; }

/* ------------------------------------------------------------ eventos de entrada */
static EFI_KEY_DATA keyq[512]; static int kh, kt;
static int mdx, mdy, mwheel, mleft, mright, mouse_dirty;
static void pushkey(UINT16 scan, CHAR16 ch, UINT32 shift) { int n = (kt + 1) % 512; if (n == kh) return; keyq[kt].Key.ScanCode = scan; keyq[kt].Key.UnicodeChar = ch; keyq[kt].KeyState.KeyShiftState = shift | EFI_SHIFT_STATE_VALID; keyq[kt].KeyState.KeyToggleState = 0; kt = n; }

/* tabela US por scancode (conjunto 1): sem shift / com shift */
static const char *us_lo = "\0\0331234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./";
static const char *us_hi = "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";

static UINT32 shift_state(void) {
    UINT32 s = 0;
    if (GetKeyState(VK_LSHIFT) & 0x8000) s |= EFI_LEFT_SHIFT;
    if (GetKeyState(VK_RSHIFT) & 0x8000) s |= EFI_RIGHT_SHIFT;
    if (GetKeyState(VK_LCONTROL) & 0x8000) s |= EFI_LEFT_CONTROL;
    if (GetKeyState(VK_RCONTROL) & 0x8000) s |= EFI_RIGHT_CONTROL;
    if (GetKeyState(VK_LMENU) & 0x8000) s |= EFI_LEFT_ALT;
    if (GetKeyState(VK_RMENU) & 0x8000) s |= EFI_RIGHT_ALT;
    return s;
}

static void key_from_scancode(int sc, int ext, UINT32 shift) {
    static const struct { int sc; UINT16 efi; } ex[] = {
        { 0x48, 1 }, { 0x50, 2 }, { 0x4D, 3 }, { 0x4B, 4 }, { 0x47, 5 }, { 0x4F, 6 }, { 0x52, 7 }, { 0x53, 8 }, { 0x49, 9 }, { 0x51, 10 } };
    for (int i = 0; i < 10; i++) if (ex[i].sc == sc) { pushkey(ex[i].efi, 0, shift); return; }
    if (sc >= 0x3B && sc <= 0x44) { pushkey((UINT16)(0x0B + sc - 0x3B), 0, shift); return; }
    if (sc == 0x57) { pushkey(0x15, 0, shift); return; }
    if (sc == 0x58) { pushkey(0x16, 0, shift); return; }
    if (sc == 0x01) { pushkey(0x17, 0, shift); return; }
    if (ext && sc == 0x1C) { pushkey(0, '\r', shift); return; }
    if (sc == 0x39) { pushkey(0, ' ', shift); return; }
    if (sc == 0x56) { pushkey(0, (shift & 3) ? '|' : '\\', shift); return; }      /* tecla extra ABNT2/ISO */
    if (sc == 0x73) return;                                                       /* /? do ABNT2: firmware US não mapeia */
    if (sc > 0 && sc < 0x36) {
        int sh = (shift & (EFI_LEFT_SHIFT | EFI_RIGHT_SHIFT)) != 0;
        char c = sh ? us_hi[sc] : us_lo[sc];
        if (!c) return;
        if (shift & (EFI_LEFT_CONTROL | EFI_RIGHT_CONTROL)) {
            char l = us_lo[sc];
            if (l >= 'a' && l <= 'z' && !(shift & EFI_RIGHT_ALT)) { pushkey(0, (CHAR16)(l - 'a' + 1), shift); return; }   /* como o EDK2: Ctrl+letra = controle */
            c = us_lo[sc];
        }
        pushkey(0, (CHAR16)(unsigned char)c, shift);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    static int lx = -1, ly = -1;
    switch (m) {
    case WM_CLOSE: ExitProcess(0);
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); SetDIBitsToDevice(dc, 0, 0, W, H, 0, 0, 0, H, fbmem, &bmi, DIB_RGB_COLORS); EndPaint(h, &ps); return 0; }
    case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) { SetCursor(0); return TRUE; } break;
    case WM_MOUSEMOVE: { int x = (short)LOWORD(lp), y = (short)HIWORD(lp); if (lx >= 0) { mdx += x - lx; mdy += y - ly; mouse_dirty = 1; } lx = x; ly = y; return 0; }
    case WM_LBUTTONDOWN: mleft = 1; mouse_dirty = 1; SetCapture(h); return 0;
    case WM_LBUTTONUP: mleft = 0; mouse_dirty = 1; ReleaseCapture(); return 0;
    case WM_RBUTTONDOWN: mright = 1; mouse_dirty = 1; return 0;
    case WM_RBUTTONUP: mright = 0; mouse_dirty = 1; return 0;
    case WM_MOUSEWHEEL: mwheel += GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1; mouse_dirty = 1; return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: key_from_scancode((int)((lp >> 16) & 0xFF), (int)((lp >> 24) & 1), shift_state()); return 0;
    case WM_CHAR: case WM_SYSCHAR: return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static void pump(void) {
    if (headless) return;
    MSG msg;
    while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    InvalidateRect(hwnd, 0, FALSE);
}

/* ------------------------------------------------------------ roteiro */
static FILE *script; static char outdir[MAX_PATH] = "."; static double script_due; static double now_ms(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / qpf.QuadPart; }
static int junk_z, wheel_left; static double junk_period = 8, junk_last, wheel_last;
static int files_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb"); int eq = fa && fb;
    while (eq) { int ca = fgetc(fa), cb = fgetc(fb); if (ca != cb) eq = 0; if (ca == EOF || cb == EOF) break; }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return eq;
}
static void write_png(const char *path);
static void script_step(void) {
    if (!script || now_ms() < script_due) return;
    char line[2048];
    while (fgets(line, sizeof line, script)) {
        size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        char *arg = strchr(line, ' '); if (arg) *arg++ = 0; else arg = line + n;
        if (!strcmp(line, "wait")) { script_due = now_ms() + atoi(arg); return; }
        if (!strcmp(line, "type")) {
            for (unsigned char *c = (unsigned char *)arg; *c; c++) {
                if (*c == 0xC2 && c[1] == 0xB6) { pushkey(0, '\r', 0); c++; continue; }   /* ¶ = Enter */
                pushkey(0, *c, (*c >= 'A' && *c <= 'Z') ? EFI_LEFT_SHIFT : 0);
            }
            script_due = now_ms() + 300; return;
        }
        if (!strcmp(line, "ctrl")) { pushkey(0, (CHAR16)(arg[0] - 'a' + 1), EFI_LEFT_CONTROL); script_due = now_ms() + 200; return; }
        if (!strcmp(line, "ctrlshift")) { pushkey(0, (CHAR16)(arg[0] - 'a' + 1), EFI_LEFT_CONTROL | EFI_LEFT_SHIFT); script_due = now_ms() + 200; return; }
        if (!strcmp(line, "key")) {                    /* teclas de navegação: tab, shift+tab, enter, esc */
            if (!strcmp(arg, "tab")) pushkey(0, 9, 0);
            else if (!strcmp(arg, "shift+tab")) pushkey(0, 9, EFI_LEFT_SHIFT);
            else if (!strcmp(arg, "enter")) pushkey(0, '\r', 0);
            else if (!strcmp(arg, "esc")) pushkey(0x17, 0, 0);
            else { fprintf(stderr, "tecla desconhecida: %s\n", arg); ExitProcess(2); }
            script_due = now_ms() + 150; return;
        }
        if (!strcmp(line, "scan")) { pushkey((UINT16)strtol(arg, 0, 0), 0, 0); script_due = now_ms() + 200; return; }
        if (!strcmp(line, "mouse")) { int x, y; sscanf(arg, "%d %d", &x, &y); mdx += x; mdy += y; mouse_dirty = 1; script_due = now_ms() + 150; return; }
        if (!strcmp(line, "click")) { mleft = 1; mouse_dirty = 1; script_due = now_ms() + 60; char *x = 0; (void)x; return; }
        if (!strcmp(line, "release")) { mleft = 0; mouse_dirty = 1; script_due = now_ms() + 100; return; }
        if (!strcmp(line, "rclick")) { mright = 1; mouse_dirty = 1; script_due = now_ms() + 60; return; }
        if (!strcmp(line, "rrelease")) { mright = 0; mouse_dirty = 1; script_due = now_ms() + 100; return; }
        /* roda de verdade: N dentes, um relatório por dente, 40 ms entre eles */
        if (!strcmp(line, "wheel")) { int n = atoi(arg); wheel_left = n; script_due = now_ms() + 40.0 * (n < 0 ? -n : n) + 100; return; }
        /* mouse defeituoso: relatório periódico (Hz) com o mesmo Z falso; "zjunk 0" desliga */
        if (!strcmp(line, "zjunk")) { int z = 0, hz = 125; sscanf(arg, "%d %d", &z, &hz); junk_z = z; junk_period = 1000.0 / (hz > 0 ? hz : 125); continue; }
        if (!strcmp(line, "assert_same") || !strcmp(line, "assert_diff")) {
            char a[MAX_PATH], b[MAX_PATH], pa[MAX_PATH * 2], pb[MAX_PATH * 2]; sscanf(arg, "%259s %259s", a, b);
            snprintf(pa, sizeof pa, "%s\\%s", outdir, a); snprintf(pb, sizeof pb, "%s\\%s", outdir, b);
            int same = files_equal(pa, pb), want = !strcmp(line, "assert_same");
            if (same != want) { fprintf(stderr, "FALHA: %s %s %s\n", line, a, b); fflush(stderr); ExitProcess(2); }
            printf("ok: %s %s %s\n", line, a, b); fflush(stdout); continue;
        }
        if (!strcmp(line, "shot")) { char p[MAX_PATH * 2]; snprintf(p, sizeof p, "%s\\%s", outdir, arg); write_png(p); printf("captura: %s\n", p); fflush(stdout); continue; }
        if (!strcmp(line, "quit")) { printf("roteiro concluído\n"); fflush(stdout); ExitProcess(0); }
    }
    fclose(script); script = 0;
    printf("roteiro concluído\n"); fflush(stdout);
    ExitProcess(0);
}

/* ------------------------------------------------------------ Boot Services */
static EFI_STATUS EFIAPI bs_allocpages(EFI_ALLOCATE_TYPE t, EFI_MEMORY_TYPE m, UINTN pages, EFI_PHYSICAL_ADDRESS *a) {
    (void)t; (void)m;
    void *p = VirtualAlloc(0, pages * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return EFI_ERR(9);
    *a = (EFI_PHYSICAL_ADDRESS)(UINTN)p; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI bs_freepages(EFI_PHYSICAL_ADDRESS a, UINTN n) { (void)n; VirtualFree((void *)(UINTN)a, 0, MEM_RELEASE); return 0; }
static EFI_STATUS EFIAPI bs_allocpool(EFI_MEMORY_TYPE t, UINTN n, void **p) { (void)t; *p = malloc(n ? n : 1); return *p ? 0 : EFI_ERR(9); }
static EFI_STATUS EFIAPI bs_freepool(void *p) { free(p); return 0; }

typedef struct { int kind; double due; } Ev;   /* 1 timer, 2 teclado, 3 mouse */
static Ev ev_key = { 2, 0 }, ev_mouse = { 3, 0 };
static EFI_STATUS EFIAPI bs_createevent(UINT32 t, UINTN tpl, void *f, void *c, EFI_EVENT *e) { (void)t; (void)tpl; (void)f; (void)c; Ev *x = malloc(sizeof(Ev)); x->kind = 1; x->due = 0; *e = x; return 0; }
static EFI_STATUS EFIAPI bs_settimer(EFI_EVENT e, EFI_TIMER_DELAY t, UINT64 ns100) { (void)t; ((Ev *)e)->due = now_ms() + ns100 / 10000.0; return 0; }
static int ev_ready(Ev *e) {
    if (e->kind == 1) return now_ms() >= e->due;
    if (e->kind == 2) return kh != kt;
    return mouse_dirty;
}
static EFI_STATUS EFIAPI bs_waitforevent(UINTN n, EFI_EVENT *evs, UINTN *idx) {
    for (;;) {
        pump(); script_step();
        for (UINTN i = 0; i < n; i++) if (ev_ready((Ev *)evs[i])) { *idx = i; return 0; }
        if (!headless) MsgWaitForMultipleObjects(0, 0, FALSE, 2, QS_ALLINPUT); else Sleep(1);
    }
}
static EFI_STATUS EFIAPI bs_closeevent(EFI_EVENT e) { (void)e; return 0; }
static EFI_STATUS EFIAPI bs_checkevent(EFI_EVENT e) { return ev_ready((Ev *)e) ? 0 : EFI_NOT_READY; }
static EFI_STATUS EFIAPI bs_handleprotocol(EFI_HANDLE h, EFI_GUID *g, void **i) {
    for (int k = 0; k < nregs; k++) if (regs[k].handle == h && geq(regs[k].guid, g)) { *i = regs[k].iface; return 0; }
    return UNSUPPORTED;
}
static EFI_STATUS EFIAPI bs_locatehandlebuffer(EFI_LOCATE_SEARCH_TYPE t, EFI_GUID *g, void *k, UINTN *n, EFI_HANDLE **out) {
    (void)t; (void)k;
    EFI_HANDLE *b = malloc(sizeof(EFI_HANDLE) * 32); UINTN c = 0;
    for (int i = 0; i < nregs; i++) if (geq(regs[i].guid, g)) b[c++] = regs[i].handle;
    if (!c) { free(b); return NOT_FOUND; }
    *n = c; *out = b; return 0;
}
static EFI_STATUS EFIAPI bs_locateprotocol(EFI_GUID *g, void *r, void **i) { (void)r; for (int k = 0; k < nregs; k++) if (geq(regs[k].guid, g)) { *i = regs[k].iface; return 0; } return NOT_FOUND; }
static EFI_STATUS EFIAPI bs_stall(UINTN us) { double end = now_ms() + us / 1000.0; while (now_ms() < end) { if (us > 2000) Sleep(1); } return 0; }
static EFI_STATUS EFIAPI bs_watchdog(UINTN a, UINT64 b, UINTN c, CHAR16 *d) { (void)a; (void)b; (void)c; (void)d; if (logf) fprintf(logf, "watchdog desativado pelo app\n"); return 0; }
static EFI_STATUS EFIAPI rt_gettime(EFI_TIME *t, void *c) { (void)c; SYSTEMTIME s; GetLocalTime(&s); memset(t, 0, sizeof *t); t->Year = s.wYear; t->Month = (UINT8)s.wMonth; t->Day = (UINT8)s.wDay; t->Hour = (UINT8)s.wHour; t->Minute = (UINT8)s.wMinute; t->Second = (UINT8)s.wSecond; return 0; }
/* PlatformLang: o idioma do "firmware" (FOLIO_PLATFORM_LANG no ambiente; padrão en-US) */
static EFI_STATUS EFIAPI rt_getvariable(CHAR16 *name, EFI_GUID *g, UINT32 *attr, UINTN *size, void *data) {
    (void)g; (void)attr;
    const wchar_t pl[] = L"PlatformLang"; int i = 0;
    for (; pl[i]; i++) if (name[i] != (CHAR16)pl[i]) return 0x800000000000000EULL;   /* EFI_NOT_FOUND */
    if (name[i]) return 0x800000000000000EULL;
    const char *v = getenv("FOLIO_PLATFORM_LANG"); if (!v) v = "en-US";
    UINTN n = strlen(v) + 1;
    if (*size < n) { *size = n; return 0x8000000000000005ULL; }   /* EFI_BUFFER_TOO_SMALL */
    memcpy(data, v, n); *size = n; return 0;
}
static void EFIAPI rt_reset(EFI_RESET_TYPE t, EFI_STATUS s, UINTN n, void *d) { (void)s; (void)n; (void)d; printf("ResetSystem(%s) chamado pelo app\n", t == EfiResetShutdown ? "desligar" : "reiniciar"); ExitProcess(0); }

/* ------------------------------------------------------------ console */
static EFI_STATUS EFIAPI con_out(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t, CHAR16 *s) { (void)t; while (*s) { putchar(*s < 128 ? (char)*s : '?'); s++; } return 0; }
static EFI_STATUS EFIAPI con_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t, BOOLEAN b) { (void)t; (void)b; return 0; }
static EFI_STATUS EFIAPI con_clear(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t) { (void)t; return 0; }
static EFI_STATUS EFIAPI kx_reset(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *t, BOOLEAN b) { (void)t; (void)b; return 0; }
static EFI_STATUS EFIAPI kx_read(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *t, EFI_KEY_DATA *k) {
    (void)t; pump(); script_step();
    if (kh == kt) return EFI_NOT_READY;
    *k = keyq[kh]; kh = (kh + 1) % 512; return 0;
}
static EFI_STATUS EFIAPI ki_read(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *t, EFI_INPUT_KEY *k) { (void)t; if (kh == kt) return EFI_NOT_READY; *k = keyq[kh].Key; kh = (kh + 1) % 512; return 0; }
static EFI_STATUS EFIAPI ki_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *t, BOOLEAN b) { (void)t; (void)b; return 0; }

/* ------------------------------------------------------------ GOP */
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION ginfo;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE gmode;
static EFI_STATUS EFIAPI gop_query(EFI_GRAPHICS_OUTPUT_PROTOCOL *g, UINT32 m, UINTN *sz, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **i) {
    (void)g; if (m != 0) return INVALID;
    *i = malloc(sizeof ginfo); **i = ginfo; *sz = sizeof ginfo; return 0;
}
static EFI_STATUS EFIAPI gop_set(EFI_GRAPHICS_OUTPUT_PROTOCOL *g, UINT32 m) { (void)g; return m == 0 ? 0 : INVALID; }
static EFI_STATUS EFIAPI gop_blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *g, void *b, EFI_GRAPHICS_OUTPUT_BLT_OPERATION op, UINTN sx, UINTN sy, UINTN dx, UINTN dy, UINTN w, UINTN h, UINTN delta) {
    (void)g;
    if (op != EfiBltBufferToVideo) return UNSUPPORTED;
    for (UINTN y = 0; y < h; y++) memcpy(fbmem + (dy + y) * SP + dx, (UINT8 *)b + (sy + y) * delta + sx * 4, w * 4);
    return 0;
}

static EFI_GUID G_UGA = { 0x982c298b, 0xf4fa, 0x41cb, { 0xb8, 0x38, 0x77, 0xaa, 0x68, 0x8f, 0xb8, 0x39 } };
static EFI_GUID G_EDIDA = { 0xbd8c1056, 0x9f36, 0x44ec, { 0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86 } };
static EFI_STATUS EFIAPI uga_getmode(EFI_UGA_DRAW_PROTOCOL *u, UINT32 *h, UINT32 *v, UINT32 *d, UINT32 *r) { (void)u; *h = (UINT32)W; *v = (UINT32)H; *d = 32; *r = 60; return 0; }
static EFI_STATUS EFIAPI uga_setmode(EFI_UGA_DRAW_PROTOCOL *u, UINT32 h, UINT32 v, UINT32 d, UINT32 r) { (void)u; (void)h; (void)v; (void)d; (void)r; return 0; }
static EFI_STATUS EFIAPI uga_blt(EFI_UGA_DRAW_PROTOCOL *u, void *b, EFI_UGA_BLT_OPERATION op, UINTN sx, UINTN sy, UINTN dx, UINTN dy, UINTN w, UINTN h, UINTN delta) {
    (void)u; if (op != EfiUgaBltBufferToVideo) return UNSUPPORTED;
    for (UINTN y = 0; y < h; y++) memcpy(fbmem + (dy + y) * SP + dx, (UINT8 *)b + (sy + y) * delta + sx * 4, w * 4);
    return 0;
}
/* EDID de um painel de 15,6" (344 x 194 mm) com a resolução da janela */
static UINT8 edid_bytes[128]; static EFI_EDID_ACTIVE_PROTOCOL edid_p;
static void make_edid(void) {
    static const UINT8 hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    memcpy(edid_bytes, hdr, 8); edid_bytes[21] = 34; edid_bytes[22] = 19;
    UINT8 *d = edid_bytes + 54; d[0] = 0x3A; d[1] = 0x02;
    d[2] = (UINT8)W; d[4] = (UINT8)((W >> 8) << 4); d[5] = (UINT8)H; d[7] = (UINT8)((H >> 8) << 4);
    d[12] = 344 & 0xFF; d[13] = 194; d[14] = (UINT8)(((344 >> 8) << 4) | (194 >> 8));
    edid_p.SizeOfEdid = 128; edid_p.Edid = edid_bytes;
}

/* ------------------------------------------------------------ ponteiro */
static EFI_SIMPLE_POINTER_MODE pmode = { 8, 8, 8, 1, 1 };
static EFI_STATUS EFIAPI ptr_reset(EFI_SIMPLE_POINTER_PROTOCOL *p, BOOLEAN b) { (void)p; (void)b; return 0; }
static EFI_STATUS EFIAPI ptr_state(EFI_SIMPLE_POINTER_PROTOCOL *p, EFI_SIMPLE_POINTER_STATE *s) {
    (void)p; pump(); script_step();
    double t = now_ms();
    if (junk_z && t - junk_last >= junk_period) { junk_last = t; mwheel += junk_z; mouse_dirty = 1; }
    if (wheel_left && t - wheel_last >= 40) { wheel_last = t; mwheel += wheel_left > 0 ? 1 : -1; wheel_left -= wheel_left > 0 ? 1 : -1; mouse_dirty = 1; }
    if (!mouse_dirty) return EFI_NOT_READY;
    s->RelativeMovementX = mdx; s->RelativeMovementY = mdy; s->RelativeMovementZ = mwheel;
    s->LeftButton = (BOOLEAN)mleft; s->RightButton = (BOOLEAN)mright;
    mdx = mdy = mwheel = 0; mouse_dirty = 0;
    return 0;
}

/* ------------------------------------------------------------ sistema de arquivos */
typedef struct { EFI_FILE_PROTOCOL p; wchar_t path[1024]; HANDLE h; int dir; HANDLE find; WIN32_FIND_DATAW fd; int fd_valid; } HFile;
static EFI_FILE_PROTOCOL file_proto;
static HFile *new_file(const wchar_t *path) { HFile *f = calloc(1, sizeof(HFile)); f->p = file_proto; wcsncpy(f->path, path, 1023); f->h = INVALID_HANDLE_VALUE; f->find = INVALID_HANDLE_VALUE; return f; }
static void full_path(HFile *base, CHAR16 *name, wchar_t *out) {
    wchar_t rootw[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, root, -1, rootw, MAX_PATH);
    if (name[0] == '\\') swprintf(out, 1024, L"%ls%ls", rootw, (wchar_t *)name);
    else swprintf(out, 1024, L"%ls\\%ls", base->path, (wchar_t *)name);
    size_t n = wcslen(out); while (n > 3 && out[n - 1] == '\\') out[--n] = 0;
}
static EFI_STATUS EFIAPI f_open(EFI_FILE_PROTOCOL *t, EFI_FILE_PROTOCOL **nh, CHAR16 *name, UINT64 mode, UINT64 attr) {
    HFile *b = (HFile *)t; wchar_t p[1024]; full_path(b, name, p);
    DWORD a = GetFileAttributesW(p);
    if (a == INVALID_FILE_ATTRIBUTES) {
        if (!(mode & EFI_FILE_MODE_CREATE)) return NOT_FOUND;
        if (attr & EFI_FILE_DIRECTORY) { if (!CreateDirectoryW(p, 0)) return EFI_ERR(7); a = FILE_ATTRIBUTE_DIRECTORY; }
        else { HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, 0, CREATE_NEW, 0, 0); if (h == INVALID_HANDLE_VALUE) return EFI_ERR(7); CloseHandle(h); a = 0; }
    }
    HFile *f = new_file(p);
    if (a & FILE_ATTRIBUTE_DIRECTORY) f->dir = 1;
    else {
        f->h = CreateFileW(p, GENERIC_READ | ((mode & EFI_FILE_MODE_WRITE) ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
        if (f->h == INVALID_HANDLE_VALUE) { free(f); return EFI_ERR(8); }
    }
    *nh = &f->p; return 0;
}
static EFI_STATUS EFIAPI f_close(EFI_FILE_PROTOCOL *t) { HFile *f = (HFile *)t; if (f->h != INVALID_HANDLE_VALUE) CloseHandle(f->h); if (f->find != INVALID_HANDLE_VALUE) FindClose(f->find); free(f); return 0; }
static EFI_STATUS EFIAPI f_delete(EFI_FILE_PROTOCOL *t) {
    HFile *f = (HFile *)t; wchar_t p[1024]; wcscpy(p, f->path); int d = f->dir;
    f_close(t);
    BOOL ok = d ? RemoveDirectoryW(p) : DeleteFileW(p);
    return ok ? 0 : EFI_ERR(8);
}
static UINTN fill_info(EFI_FILE_INFO *i, UINTN cap, const wchar_t *name, DWORD attr, UINT64 size, FILETIME mt) {
    UINTN need = sizeof(EFI_FILE_INFO) + wcslen(name) * 2 + 2;
    if (cap < need) return need;
    memset(i, 0, need);
    i->Size = need; i->FileSize = size; i->PhysicalSize = size;
    i->Attribute = (attr & FILE_ATTRIBUTE_DIRECTORY) ? EFI_FILE_DIRECTORY : 0;
    FILETIME lt; SYSTEMTIME s; FileTimeToLocalFileTime(&mt, &lt); FileTimeToSystemTime(&lt, &s);
    i->ModificationTime.Year = s.wYear; i->ModificationTime.Month = (UINT8)s.wMonth; i->ModificationTime.Day = (UINT8)s.wDay;
    i->ModificationTime.Hour = (UINT8)s.wHour; i->ModificationTime.Minute = (UINT8)s.wMinute; i->ModificationTime.Second = (UINT8)s.wSecond;
    wcscpy((wchar_t *)i->FileName, name);
    return 0;
}
static EFI_STATUS EFIAPI f_read(EFI_FILE_PROTOCOL *t, UINTN *sz, void *buf) {
    HFile *f = (HFile *)t;
    if (!f->dir) { DWORD r = 0; if (!ReadFile(f->h, buf, (DWORD)*sz, &r, 0)) return EFI_ERR(7); *sz = r; return 0; }
    if (f->find == INVALID_HANDLE_VALUE && !f->fd_valid) {
        wchar_t p[1030]; swprintf(p, 1030, L"%ls\\*", f->path);
        f->find = FindFirstFileW(p, &f->fd);
        if (f->find == INVALID_HANDLE_VALUE) { *sz = 0; return 0; }
        f->fd_valid = 1;
    }
    if (!f->fd_valid) { *sz = 0; return 0; }
    UINTN need = fill_info(buf, *sz, f->fd.cFileName, f->fd.dwFileAttributes, ((UINT64)f->fd.nFileSizeHigh << 32) | f->fd.nFileSizeLow, f->fd.ftLastWriteTime);
    if (need) { *sz = need; return EFI_BUFFER_TOO_SMALL; }
    *sz = ((EFI_FILE_INFO *)buf)->Size;
    if (!FindNextFileW(f->find, &f->fd)) f->fd_valid = 0;
    return 0;
}
static EFI_STATUS EFIAPI f_write(EFI_FILE_PROTOCOL *t, UINTN *sz, void *buf) { HFile *f = (HFile *)t; DWORD w = 0; if (f->dir || !WriteFile(f->h, buf, (DWORD)*sz, &w, 0)) return EFI_ERR(7); *sz = w; return 0; }
static EFI_STATUS EFIAPI f_getpos(EFI_FILE_PROTOCOL *t, UINT64 *p) { HFile *f = (HFile *)t; LARGE_INTEGER z = { 0 }, o; SetFilePointerEx(f->h, z, &o, FILE_CURRENT); *p = (UINT64)o.QuadPart; return 0; }
static EFI_STATUS EFIAPI f_setpos(EFI_FILE_PROTOCOL *t, UINT64 p) { HFile *f = (HFile *)t; if (f->dir) { if (f->find != INVALID_HANDLE_VALUE) FindClose(f->find); f->find = INVALID_HANDLE_VALUE; f->fd_valid = 0; return 0; } LARGE_INTEGER l; l.QuadPart = (LONGLONG)p; SetFilePointerEx(f->h, l, 0, FILE_BEGIN); return 0; }
static EFI_STATUS EFIAPI f_getinfo(EFI_FILE_PROTOCOL *t, EFI_GUID *g, UINTN *sz, void *buf) {
    HFile *f = (HFile *)t;
    if (geq(g, &G_FSINFO)) {
        UINTN need = sizeof(EFI_FILE_SYSTEM_INFO) + 16;
        if (*sz < need || !buf) { *sz = need; return EFI_BUFFER_TOO_SMALL; }
        EFI_FILE_SYSTEM_INFO *i = buf; memset(i, 0, need); i->Size = need; wcscpy((wchar_t *)i->VolumeLabel, L"FOLIO"); return 0;
    }
    if (!geq(g, &G_FINFO)) return UNSUPPORTED;
    WIN32_FILE_ATTRIBUTE_DATA ad; GetFileAttributesExW(f->path, GetFileExInfoStandard, &ad);
    const wchar_t *base = wcsrchr(f->path, '\\'); base = base ? base + 1 : f->path;
    UINTN need = fill_info(buf, buf ? *sz : 0, base, ad.dwFileAttributes, ((UINT64)ad.nFileSizeHigh << 32) | ad.nFileSizeLow, ad.ftLastWriteTime);
    if (need) { *sz = need; return EFI_BUFFER_TOO_SMALL; }
    *sz = ((EFI_FILE_INFO *)buf)->Size;
    return 0;
}
static EFI_STATUS EFIAPI f_setinfo(EFI_FILE_PROTOCOL *t, EFI_GUID *g, UINTN sz, void *buf) {
    (void)sz; HFile *f = (HFile *)t;
    if (!geq(g, &G_FINFO)) return UNSUPPORTED;
    EFI_FILE_INFO *i = buf;
    if (!f->dir) {
        LARGE_INTEGER cur; GetFileSizeEx(f->h, &cur);
        if ((UINT64)cur.QuadPart != i->FileSize) { LARGE_INTEGER l; l.QuadPart = (LONGLONG)i->FileSize; SetFilePointerEx(f->h, l, 0, FILE_BEGIN); SetEndOfFile(f->h); }
    }
    const wchar_t *base = wcsrchr(f->path, '\\'); base = base ? base + 1 : f->path;
    if (wcscmp(base, (wchar_t *)i->FileName)) {
        wchar_t np[1024]; wcsncpy(np, f->path, (size_t)(base - f->path)); np[base - f->path] = 0; wcscat(np, (wchar_t *)i->FileName);
        if (!MoveFileExW(f->path, np, 0)) return EFI_ERR(8);
        wcscpy(f->path, np);
    }
    return 0;
}
static EFI_STATUS EFIAPI f_flush(EFI_FILE_PROTOCOL *t) { HFile *f = (HFile *)t; if (!f->dir) FlushFileBuffers(f->h); return 0; }
static EFI_STATUS EFIAPI sfs_open(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *s, EFI_FILE_PROTOCOL **r) {
    (void)s; wchar_t rootw[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, root, -1, rootw, MAX_PATH);
    HFile *f = new_file(rootw); f->dir = 1; *r = &f->p; return 0;
}

/* ------------------------------------------------------------ PNG */
static unsigned crc_t[256];
static unsigned crc_up(unsigned c, const unsigned char *b, size_t n) { for (size_t i = 0; i < n; i++) c = crc_t[(c ^ b[i]) & 255] ^ (c >> 8); return c; }
static void be32(FILE *f, unsigned v) { unsigned char b[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v }; fwrite(b, 1, 4, f); }
static void chunk(FILE *f, const char *t, const unsigned char *d, unsigned n) { be32(f, n); fwrite(t, 1, 4, f); if (n) fwrite(d, 1, n, f); unsigned c = crc_up(0xFFFFFFFFu, (const unsigned char *)t, 4); c = crc_up(c, d, n); be32(f, c ^ 0xFFFFFFFFu); }
static void write_png(const char *path) {
    for (unsigned i = 0; i < 256; i++) { unsigned c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_t[i] = c; }
    size_t rn = (size_t)H * (W * 3 + 1); unsigned char *raw = malloc(rn);
    for (int y = 0; y < H; y++) { unsigned char *r = raw + (size_t)y * (W * 3 + 1); r[0] = 0; for (int x = 0; x < W; x++) { UINT32 p = get_px(x, y); r[1 + x * 3] = (unsigned char)(p >> 16); r[2 + x * 3] = (unsigned char)(p >> 8); r[3 + x * 3] = (unsigned char)p; } }
    size_t nb = (rn + 65534) / 65535; unsigned char *z = malloc(rn + nb * 5 + 6); size_t zn = 0;
    z[zn++] = 0x78; z[zn++] = 1; unsigned a = 1, b = 0;
    for (size_t i = 0; i < rn; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t o = 0; o < rn; o += 65535) { unsigned l = (unsigned)(rn - o < 65535 ? rn - o : 65535); z[zn++] = o + l >= rn; z[zn++] = (unsigned char)l; z[zn++] = (unsigned char)(l >> 8); z[zn++] = (unsigned char)~l; z[zn++] = (unsigned char)(~l >> 8); memcpy(z + zn, raw + o, l); zn += l; }
    unsigned ad = (b << 16) | a; z[zn++] = (unsigned char)(ad >> 24); z[zn++] = (unsigned char)(ad >> 16); z[zn++] = (unsigned char)(ad >> 8); z[zn++] = (unsigned char)ad;
    FILE *f = fopen(path, "wb"); if (!f) return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    unsigned char ih[13] = { (unsigned char)(W >> 24), (unsigned char)(W >> 16), (unsigned char)(W >> 8), (unsigned char)W, (unsigned char)(H >> 24), (unsigned char)(H >> 16), (unsigned char)(H >> 8), (unsigned char)H, 8, 2, 0, 0, 0 };
    chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (unsigned)zn); chunk(f, "IEND", 0, 0); fclose(f); free(raw); free(z);
}

/* ------------------------------------------------------------ carregador PE32+ */
typedef EFI_STATUS (*EntryFn)(EFI_HANDLE, EFI_SYSTEM_TABLE *);
static EntryFn load_pe(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "não abriu %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n); fread(d, 1, (size_t)n, f); fclose(f);
    if (d[0] != 'M' || d[1] != 'Z') { fprintf(stderr, "não é PE\n"); return 0; }
    UINT32 pe = *(UINT32 *)(d + 0x3C);
    if (memcmp(d + pe, "PE\0\0", 4)) { fprintf(stderr, "assinatura PE inválida\n"); return 0; }
    UINT16 machine = *(UINT16 *)(d + pe + 4), nsec = *(UINT16 *)(d + pe + 6), optsz = *(UINT16 *)(d + pe + 20);
    unsigned char *opt = d + pe + 24;
    if (machine != 0x8664 || *(UINT16 *)opt != 0x20B) { fprintf(stderr, "esperado PE32+ x64 (máquina %04x)\n", machine); return 0; }
    UINT16 subsys = *(UINT16 *)(opt + 68);
    if (subsys != 10) { fprintf(stderr, "subsistema %d não é EFI application\n", subsys); return 0; }
    UINT32 entry = *(UINT32 *)(opt + 16); UINT64 ibase = *(UINT64 *)(opt + 24);
    UINT32 imgsz = *(UINT32 *)(opt + 56), hdrsz = *(UINT32 *)(opt + 60);
    UINT32 reloc_rva = *(UINT32 *)(opt + 112 + 5 * 8), reloc_sz = *(UINT32 *)(opt + 112 + 5 * 8 + 4);
    unsigned char *img = VirtualAlloc(0, imgsz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(img, d, hdrsz);
    unsigned char *sec = opt + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        UINT32 va = *(UINT32 *)(sec + 12), rsz = *(UINT32 *)(sec + 16), rptr = *(UINT32 *)(sec + 20);
        if (rsz) memcpy(img + va, d + rptr, rsz);
    }
    INT64 delta = (INT64)((UINT64)(UINTN)img - ibase);
    int nrel = 0;
    for (UINT32 o = 0; o < reloc_sz; ) {
        UINT32 page = *(UINT32 *)(img + reloc_rva + o), bsz = *(UINT32 *)(img + reloc_rva + o + 4);
        if (!bsz) break;
        for (UINT32 k = 8; k < bsz; k += 2) {
            UINT16 e = *(UINT16 *)(img + reloc_rva + o + k);
            int type = e >> 12, off = e & 0xFFF;
            if (type == 10) { *(UINT64 *)(img + page + off) += (UINT64)delta; nrel++; }
            else if (type == 3) { *(UINT32 *)(img + page + off) += (UINT32)delta; nrel++; }
            else if (type != 0) fprintf(stderr, "relocação tipo %d não suportada\n", type);
        }
        o += bsz;
    }
    printf("PE carregado: %u bytes de imagem, %d relocações aplicadas, base 0x%p, entrada RVA 0x%x\n", imgsz, nrel, img, entry);
    return (EntryFn)(img + entry);
}

/* ------------------------------------------------------------ discos (--disk imagem): Block I/O sobre arquivos
 * Cada imagem vira um disco SATA com Disk Info (IDENTIFY com o modelo "FolioOS test disk N"). O volume da
 * pasta --root fica num caminho USB, então nenhum desses discos é o de boot. */
static EFI_GUID G_BIO   = { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID G_DPATH = { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID G_DINFO = { 0xd432a67f, 0x14dc, 0x484b, { 0xb3, 0xbb, 0x3f, 0x02, 0x91, 0x84, 0x93, 0x27 } };
typedef struct { EFI_BLOCK_IO_PROTOCOL bio; EFI_DISK_INFO_PROTOCOL di; EFI_BLOCK_IO_MEDIA media; FILE *f; UINT8 dp[14]; int handle; } HDisk;
static HDisk hdisks[4]; static int nhdisks;
static UINT8 fs_dp[] = { 3, 5, 6, 0, 1, 0,   4, 1, 42, 0, 1, 0, 0, 0,  0, 8, 0, 0, 0, 0, 0, 0,  0, 0, 1, 0, 0, 0, 0, 0,
                         0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,   0x7F, 0xFF, 4, 0 };   /* USB / partição 1 / fim */
static EFI_STATUS hd_rw(EFI_BLOCK_IO_PROTOCOL *b, UINT32 m, UINT64 lba, UINTN sz, void *buf, int w) {
    HDisk *d = (HDisk *)b;
    if (m != d->media.MediaId) return EFI_ERR(13);                       /* MEDIA_CHANGED */
    if (sz % 512 || lba + sz / 512 > d->media.LastBlock + 1) return EFI_ERR(2);   /* INVALID_PARAMETER */
    if (w && d->media.ReadOnly) return EFI_ERR(8);
    if (_fseeki64(d->f, (long long)(lba * 512), SEEK_SET)) return EFI_ERR(7);
    size_t got = w ? fwrite(buf, 1, sz, d->f) : fread(buf, 1, sz, d->f);
    return got == sz ? 0 : EFI_ERR(7);                                   /* DEVICE_ERROR */
}
static EFI_STATUS EFIAPI hd_read(EFI_BLOCK_IO_PROTOCOL *b, UINT32 m, UINT64 lba, UINTN sz, void *buf) { return hd_rw(b, m, lba, sz, buf, 0); }
static EFI_STATUS EFIAPI hd_write(EFI_BLOCK_IO_PROTOCOL *b, UINT32 m, UINT64 lba, UINTN sz, void *buf) { return hd_rw(b, m, lba, sz, buf, 1); }
static EFI_STATUS EFIAPI hd_flush(EFI_BLOCK_IO_PROTOCOL *b) { return fflush(((HDisk *)b)->f) ? EFI_ERR(7) : 0; }   /* DEVICE_ERROR se não foi para o arquivo */
static EFI_STATUS EFIAPI hd_reset(EFI_BLOCK_IO_PROTOCOL *b, BOOLEAN x) { (void)b; (void)x; return 0; }
static EFI_STATUS EFIAPI hd_identify(EFI_DISK_INFO_PROTOCOL *p, void *buf, UINT32 *sz) {
    HDisk *d = (HDisk *)((char *)p - offsetof(HDisk, di));
    if (*sz < 512) { *sz = 512; return EFI_BUFFER_TOO_SMALL; }
    char m[48]; snprintf(m, sizeof m, "%-40s", "");
    int n = snprintf(m, sizeof m, "FolioOS test disk %d", (int)(d - hdisks) + 1); m[n] = ' ';
    memset(buf, 0, 512);
    for (int i = 0; i < 40; i++) ((UINT8 *)buf)[54 + (i ^ 1)] = (UINT8)m[i];   /* palavras ATA: bytes trocados */
    *sz = 512; return 0;
}
static EFI_STATUS EFIAPI hd_inquiry(EFI_DISK_INFO_PROTOCOL *p, void *buf, UINT32 *sz) { (void)p; (void)buf; (void)sz; return UNSUPPORTED; }
static bool add_disk(const char *path) {
    if (nhdisks >= 4) return false;
    HDisk *d = &hdisks[nhdisks];
    d->f = fopen(path, "r+b"); if (!d->f) { perror(path); return false; }
    _fseeki64(d->f, 0, SEEK_END); UINT64 size = (UINT64)_ftelli64(d->f);
    d->media.MediaId = 1; d->media.MediaPresent = 1; d->media.BlockSize = 512; d->media.LastBlock = size / 512 - 1;
    d->bio.Revision = 0x10000; d->bio.Media = &d->media; d->bio.Reset = hd_reset; d->bio.ReadBlocks = hd_read; d->bio.WriteBlocks = hd_write; d->bio.FlushBlocks = hd_flush;
    d->di.Identify = hd_identify; d->di.Inquiry = hd_inquiry;
    UINT8 dp[14] = { 3, 0x12, 10, 0, (UINT8)nhdisks, 0, 0xFF, 0xFF, 0, 0, 0x7F, 0xFF, 4, 0 };   /* SATA porta N / fim */
    memcpy(d->dp, dp, sizeof dp);
    reg(&d->handle, &G_BIO, &d->bio); reg(&d->handle, &G_DPATH, d->dp); reg(&d->handle, &G_DINFO, &d->di);
    nhdisks++;
    return true;
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    QueryPerformanceFrequency(&qpf);
    const char *efi = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) snprintf(root, sizeof root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) { script = fopen(argv[++i], "rb"); headless = 1; }
        else if (!strcmp(argv[i], "--gop") && i + 1 < argc) { const char *m = argv[++i]; gopmode = !strcmp(m, "rgb") ? 1 : !strcmp(m, "mask16") ? 2 : !strcmp(m, "blt") ? 3 : !strcmp(m, "pad") ? 4 : !strcmp(m, "uga") ? 5 : 0; }
        else if (!strcmp(argv[i], "--edid")) edid_on = 1;
        else if (!strcmp(argv[i], "--disk") && i + 1 < argc) { if (!add_disk(argv[++i])) return 1; }
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) snprintf(outdir, sizeof outdir, "%s", argv[++i]);
        else efi = argv[i];
    }
    if (!efi) { fprintf(stderr, "uso: efi_host BOOTX64.EFI [--root pasta] [--size LxA] [--script f --out pasta]\n"); return 1; }
    char full[MAX_PATH]; GetFullPathNameA(root, MAX_PATH, full, 0); snprintf(root, sizeof root, "%s", full);
    SP = gopmode == 4 ? W + 40 : W;
    fbmem = calloc((size_t)SP * H, 4);
    memset(&bmi, 0, sizeof bmi); bmi.bmiHeader.biSize = sizeof bmi.bmiHeader; bmi.bmiHeader.biWidth = W; bmi.bmiHeader.biHeight = -H; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    if (!headless) {
        WNDCLASSW wc = { 0 }; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(0); wc.lpszClassName = L"EfiHost"; RegisterClassW(&wc);
        RECT r = { 0, 0, W, H }; AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
        hwnd = CreateWindowW(L"EfiHost", L"FOLIO — BOOTX64.EFI no hospedeiro UEFI", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
    }
    EntryFn entry = load_pe(efi);
    if (!entry) return 1;

    static EFI_BOOT_SERVICES bs; static EFI_RUNTIME_SERVICES rt; static EFI_SYSTEM_TABLE st;
    static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL co; static EFI_SIMPLE_TEXT_INPUT_PROTOCOL ci; static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL cix;
    static EFI_GRAPHICS_OUTPUT_PROTOCOL gop; static EFI_SIMPLE_POINTER_PROTOCOL ptr; static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL sfs; static EFI_LOADED_IMAGE_PROTOCOL li;
    bs.AllocatePages = bs_allocpages; bs.FreePages = bs_freepages; bs.AllocatePool = bs_allocpool; bs.FreePool = bs_freepool;
    bs.CreateEvent = bs_createevent; bs.SetTimer = bs_settimer; bs.WaitForEvent = bs_waitforevent; bs.CloseEvent = bs_closeevent; bs.CheckEvent = bs_checkevent;
    bs.HandleProtocol = bs_handleprotocol; bs.LocateHandleBuffer = bs_locatehandlebuffer; bs.LocateProtocol = bs_locateprotocol;
    bs.Stall = bs_stall; bs.SetWatchdogTimer = bs_watchdog;
    rt.GetTime = rt_gettime; rt.ResetSystem = rt_reset; rt.GetVariable = rt_getvariable;
    co.OutputString = con_out; co.EnableCursor = con_cursor; co.ClearScreen = con_clear;
    ci.Reset = ki_reset; ci.ReadKeyStroke = ki_read; ci.WaitForKey = &ev_key;
    cix.Reset = kx_reset; cix.ReadKeyStrokeEx = kx_read; cix.WaitForKeyEx = &ev_key;
    ginfo.HorizontalResolution = (UINT32)W; ginfo.VerticalResolution = (UINT32)H; ginfo.PixelFormat = gopmode == 1 ? PixelRedGreenBlueReserved8BitPerColor : gopmode == 2 ? PixelBitMask : gopmode == 3 ? PixelBltOnly : PixelBlueGreenRedReserved8BitPerColor; ginfo.PixelsPerScanLine = (UINT32)SP;
    if (gopmode == 2) { ginfo.PixelInformation.RedMask = 0xF800; ginfo.PixelInformation.GreenMask = 0x07E0; ginfo.PixelInformation.BlueMask = 0x001F; }
    gmode.MaxMode = 1; gmode.Mode = 0; gmode.Info = &ginfo; gmode.SizeOfInfo = sizeof ginfo; gmode.FrameBufferBase = gopmode == 3 ? 0 : (EFI_PHYSICAL_ADDRESS)(UINTN)fbmem; gmode.FrameBufferSize = gopmode == 3 ? 0 : (UINTN)SP * H * 4;
    gop.QueryMode = gop_query; gop.SetMode = gop_set; gop.Blt = gop_blt; gop.Mode = &gmode;
    ptr.Reset = ptr_reset; ptr.GetState = ptr_state; ptr.WaitForInput = &ev_mouse; ptr.Mode = &pmode;
    file_proto.Revision = 0x10000; file_proto.Open = f_open; file_proto.Close = f_close; file_proto.Delete = f_delete; file_proto.Read = f_read;
    file_proto.Write = f_write; file_proto.GetPosition = f_getpos; file_proto.SetPosition = f_setpos; file_proto.GetInfo = f_getinfo; file_proto.SetInfo = f_setinfo; file_proto.Flush = f_flush;
    sfs.Revision = 0x10000; sfs.OpenVolume = sfs_open;
    li.DeviceHandle = &h_fs;
    st.ConIn = &ci; st.ConOut = &co; st.ConsoleInHandle = &h_con; st.RuntimeServices = &rt; st.BootServices = &bs;
    reg(&h_con, &G_TIEX, &cix); if (gopmode == 5) { static EFI_UGA_DRAW_PROTOCOL uga; uga.GetMode = uga_getmode; uga.SetMode = uga_setmode; uga.Blt = uga_blt; reg(&h_gop, &G_UGA, &uga); }
    else reg(&h_gop, &G_GOP, &gop);
    if (edid_on) { make_edid(); reg(&h_gop, &G_EDIDA, &edid_p); } reg(&h_mouse, &G_SPTR, &ptr); reg(&h_fs, &G_SFS, &sfs); reg(&h_fs, &G_DPATH, fs_dp); reg(&h_image, &G_LIMG, &li);
    printf("executando efi_main…\n"); fflush(stdout);
    EFI_STATUS s = entry(&h_image, &st);
    printf("efi_main retornou 0x%llx\n", (unsigned long long)s);
    return (int)s;
}
