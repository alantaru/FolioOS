/* FolioOS — ponte BIOS→UEFI: deixa o mesmo BOOTIA32.EFI rodar em PCs que só têm BIOS (sem UEFI).
 *
 * Roda em modo protegido de 32 bits, sem paginação, com interrupções desligadas, e oferece ao FolioOS
 * a pequena parte da UEFI que ele usa:
 *   - Boot Services: páginas e pool (mapa E820), eventos de tempo e de teclado, Stall (TSC calibrado no PIT)
 *   - GOP: modos VBE com framebuffer linear (INT 10h pelo modo real), EDID do monitor
 *   - teclado e mouse/touchpad PS/2 (controlador i8042; teclados e mouses USB entram pela emulação
 *     "legacy USB" do próprio BIOS)
 *   - Simple File System: driver FAT32 próprio (fat32.c) sobre INT 13h, com escrita
 *   - relógio do CMOS, desligamento por ACPI (S5), reinício, porta serial COM1 para o registro de depuração
 *   - marca de "barramento PCI acessível": o áudio (HDA/AC'97) usa as portas 0xCF8 diretamente
 * O código C é ligado em 0x10000; stub.asm (o começo do FOLIO\BIOS.SYS) faz a transição e oferece rm_int. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../uefi/efi.h"
#include "../../core/fat32.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t i32;
#define NOT_FOUND   EFI_ERR(14)
#define UNSUPPORTED EFI_ERR(3)
#define INVALID     EFI_ERR(2)
#define OUT_OF_RES  EFI_ERR(9)
#define DEV_ERROR   EFI_ERR(7)
#define WRITE_PROT  EFI_ERR(8)
#define ACCESS_DEN  EFI_ERR(15)
#define VOL_FULL    EFI_ERR(11)
#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

_Static_assert(sizeof(void *) == 4, "a ponte é 32 bits");
_Static_assert(offsetof(EFI_FILE_SYSTEM_INFO, VolumeSize) == 16, "alinhamento de UINT64 igual ao da UEFI (use -malign-double)");

/* ============================================================ libc mínima */
void *memcpy(void *d, const void *s, size_t n) { u8 *a = d; const u8 *b = s; while (n--) { *a++ = *b++; __asm__ volatile("" ::: "memory"); } return d; }
void *memset(void *d, int c, size_t n) { u8 *a = d; while (n--) { *a++ = (u8)c; __asm__ volatile("" ::: "memory"); } return d; }
void *memmove(void *d, const void *s, size_t n) { u8 *a = d; const u8 *b = s; if (a < b) return memcpy(d, s, n); while (n--) { a[n] = b[n]; __asm__ volatile("" ::: "memory"); } return d; }
int memcmp(const void *x, const void *y, size_t n) { const u8 *a = x, *b = y; for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] - b[i]; return 0; }
char *strchr(const char *s, int c) { for (; *s; s++) if (*s == (char)c) return (char *)s; return c ? 0 : (char *)s; }
/* divisão de 64 bits (o compilador chama estas em i386) */
u64 __udivmoddi4(u64 n, u64 d, u64 *rem) {
    u64 q = 0, r = 0;
    for (int i = 63; i >= 0; i--) { r = (r << 1) | ((n >> i) & 1); if (r >= d) { r -= d; q |= 1ull << i; } }
    if (rem) *rem = r;
    return q;
}
u64 __udivdi3(u64 n, u64 d) { return __udivmoddi4(n, d, 0); }
u64 __umoddi3(u64 n, u64 d) { u64 r; __udivmoddi4(n, d, &r); return r; }
int64_t __divdi3(int64_t a, int64_t b) { bool neg = (a < 0) != (b < 0); u64 q = __udivmoddi4(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, 0); return neg ? -(int64_t)q : (int64_t)q; }
int64_t __moddi3(int64_t a, int64_t b) { u64 r; __udivmoddi4(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, &r); return a < 0 ? -(int64_t)r : (int64_t)r; }

static inline u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline u16 inw(u16 p) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline u64 rdtsc(void) { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }
static int geq(const EFI_GUID *a, const EFI_GUID *b) { return !memcmp(a, b, sizeof *a); }

/* ============================================================ serial (COM1) e texto */
static bool com1;
static void ser_init(void) {
    outb(0x3FF, 0x5A); if (inb(0x3FF) != 0x5A) return;
    outb(0x3F9, 0); outb(0x3FB, 0x80); outb(0x3F8, 1); outb(0x3F9, 0); outb(0x3FB, 3); outb(0x3FA, 0xC7); outb(0x3FC, 0x0B);
    com1 = true;
}
static void ser_putc(char c) { if (!com1) return; for (int i = 0; i < 100000 && !(inb(0x3FD) & 0x20); i++) {} outb(0x3F8, (u8)c); }
static int tx, ty; static bool text_mode = true;
static void putc_all(char c) {
    ser_putc(c);
    if (!text_mode) return;
    volatile u16 *v = (u16 *)0xB8000;
    if (c == '\n') { tx = 0; ty++; }
    else if (c != '\r') { v[ty * 80 + tx] = (u16)(0x0700 | (u8)c); if (++tx == 80) { tx = 0; ty++; } }
    if (ty >= 25) { for (int i = 0; i < 24 * 80; i++) v[i] = v[i + 80]; for (int i = 0; i < 80; i++) v[24 * 80 + i] = 0x0720; ty = 24; }
}
static void puts_(const char *s) { while (*s) putc_all(*s++); }
static void putn(u32 v, int base) { char b[12]; int n = 0; do { b[n++] = "0123456789ABCDEF"[v % (u32)base]; v /= (u32)base; } while (v); while (n) putc_all(b[--n]); }
static void fatal(const char *m) { puts_("\r\nFolioOS: "); puts_(m); puts_("\r\n"); for (;;) __asm__ volatile("hlt"); }

/* ============================================================ modo real (stub.asm) */
typedef struct { u32 eax, ebx, ecx, edx, esi, edi, ebp; u16 ds, es; u32 eflags; } RmRegs;
typedef struct { u32 magic; u8 *drive; u32 *e820_count; u8 *e820; void (*rm_int)(int, RmRegs *); } BootInfo;
static BootInfo *bi;
static void rm(int n, RmRegs *r) { bi->rm_int(n, r); }
#define LOW_VBE  0x5000     /* buffers abaixo de 1 MB para o BIOS */
#define LOW_MODE 0x5200
#define LOW_EDID 0x5400
#define LOW_DAP  0x5600
#define BOUNCE   0x70000
#define BOUNCE_SECS 64

/* ============================================================ tempo */
static u64 tsc_per_us = 1000;
static void calibrate(void) {
    outb(0x61, (u8)((inb(0x61) & ~0x02) | 0x01));
    outb(0x43, 0xB0);
    u16 cnt = 59659;                                  /* 50 ms a 1,193182 MHz */
    outb(0x42, (u8)cnt); outb(0x42, (u8)(cnt >> 8));
    u64 a = rdtsc();
    for (u32 guard = 0; !(inb(0x61) & 0x20) && guard < 100000000u; guard++) {}
    u64 b = rdtsc();
    tsc_per_us = (b - a) / 50000; if (!tsc_per_us) tsc_per_us = 1;
}
static u64 now_us(void) { return rdtsc() / tsc_per_us; }

/* ============================================================ memória */
typedef struct { u32 base, pages; } Range;
static Range ranges[64]; static int nranges;
static void add_range(u32 base, u32 pages) { if (pages && nranges < COUNT(ranges)) { ranges[nranges].base = base; ranges[nranges].pages = pages; nranges++; } }
static void mem_init(void) {
    u32 n = *bi->e820_count; u8 *e = bi->e820;
    for (u32 i = 0; i < n; i++, e += 24) {
        u64 base = *(u64 *)e, len = *(u64 *)(e + 8); u32 type = *(u32 *)(e + 16);
        if (type != 1 || base >= 0x100000000ull) continue;
        u64 end = base + len; if (end > 0xFFFFF000ull) end = 0xFFFFF000ull;
        if (base < 0x100000) base = 0x100000;             /* abaixo de 1 MB fica para o BIOS e para nós */
        base = (base + 4095) & ~4095ull; end &= ~4095ull;
        if (end > base) add_range((u32)base, (u32)((end - base) / 4096));
    }
    if (!nranges) fatal("no usable memory above 1 MB (E820)");
}
/* páginas: do topo da faixa com espaço (a mais alta primeiro); os pedaços liberados são reaproveitados */
static u32 page_alloc(u32 pages) {
    int best = -1;
    for (int i = 0; i < nranges; i++) if (ranges[i].pages >= pages && (best < 0 || ranges[i].base > ranges[best].base)) best = i;
    if (best < 0) return 0;
    ranges[best].pages -= pages;
    return ranges[best].base + ranges[best].pages * 4096;
}
static EFI_STATUS EFIAPI bs_allocpages(EFI_ALLOCATE_TYPE t, EFI_MEMORY_TYPE m, UINTN pages, EFI_PHYSICAL_ADDRESS *a) {
    (void)m;
    if (t == AllocateAddress) return OUT_OF_RES;
    u32 p = page_alloc((u32)pages);
    if (!p) return OUT_OF_RES;
    *a = p; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI bs_freepages(EFI_PHYSICAL_ADDRESS a, UINTN pages) { add_range((u32)a, (u32)pages); return EFI_SUCCESS; }
/* pool: primeiro encaixe com cabeçalho de 16 bytes, junta vizinhos livres ao liberar */
typedef struct PB { u32 size; u32 free; struct PB *next; u32 pad; } PB;
static PB *pool;
static void pool_init(void) {
    u32 pages = 2048;                                  /* 8 MB */
    u32 a = page_alloc(pages); if (!a) fatal("out of memory for the pool");
    pool = (PB *)a; pool->size = pages * 4096 - sizeof(PB); pool->free = 1; pool->next = 0;
}
static void *pool_alloc(u32 n) {
    n = (n + 15) & ~15u;
    for (PB *b = pool; b; b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + sizeof(PB) + 64) {
            PB *r = (PB *)((u8 *)(b + 1) + n);
            r->size = b->size - n - sizeof(PB); r->free = 1; r->next = b->next;
            b->size = n; b->next = r;
        }
        b->free = 0;
        return b + 1;
    }
    return 0;
}
static void pool_free(void *p) {
    if (!p) return;
    PB *b = (PB *)p - 1; b->free = 1;
    for (PB *x = pool; x; x = x->next) while (x->free && x->next && x->next->free) { x->size += x->next->size + sizeof(PB); x->next = x->next->next; }
}
static EFI_STATUS EFIAPI bs_allocpool(EFI_MEMORY_TYPE t, UINTN n, void **p) { (void)t; *p = pool_alloc((u32)(n ? n : 1)); return *p ? EFI_SUCCESS : OUT_OF_RES; }
static EFI_STATUS EFIAPI bs_freepool(void *p) { pool_free(p); return EFI_SUCCESS; }

/* ============================================================ teclado e mouse PS/2 (i8042) */
static EFI_KEY_DATA keyq[64]; static int kh, kt;
static u32 shift; static bool caps, numlock = true, e0;
static void pushkey(u16 scan, CHAR16 ch) {
    int n = (kt + 1) % COUNT(keyq); if (n == kh) return;
    keyq[kt].Key.ScanCode = scan; keyq[kt].Key.UnicodeChar = ch;
    keyq[kt].KeyState.KeyShiftState = shift | EFI_SHIFT_STATE_VALID;
    keyq[kt].KeyState.KeyToggleState = (u8)(0x40 | (caps ? 4 : 0) | (numlock ? 2 : 0));   /* VALID, CAPS, NUM */
    kt = n;
}
static const char us_lo[] = "\0\0331234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./";
static const char us_hi[] = "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";
static void kbd_byte(u8 b) {
    if (b == 0xE0) { e0 = true; return; }
    if (b == 0xE1 || b == 0xFA || b == 0xFE || b == 0x00 || b == 0xFF) { e0 = false; return; }
    bool ext = e0; e0 = false;
    bool up = b & 0x80; u8 sc = b & 0x7F;
    if (ext && (sc == 0x2A || sc == 0x36)) return;                    /* shifts falsos do PrintScreen */
    u32 bit = 0;
    if (sc == 0x2A) bit = EFI_LEFT_SHIFT; else if (sc == 0x36) bit = EFI_RIGHT_SHIFT;
    else if (sc == 0x1D) bit = ext ? EFI_RIGHT_CONTROL : EFI_LEFT_CONTROL;
    else if (sc == 0x38) bit = ext ? EFI_RIGHT_ALT : EFI_LEFT_ALT;
    if (bit) { if (up) shift &= ~bit; else shift |= bit; return; }
    if (up) return;
    if (sc == 0x3A) { caps = !caps; return; }
    if (sc == 0x45 && !ext) { numlock = !numlock; return; }
    static const u8 nav_sc[] = { 0x48, 0x50, 0x4D, 0x4B, 0x47, 0x4F, 0x52, 0x53, 0x49, 0x51 };
    bool kp = !ext && sc >= 0x47 && sc <= 0x53 && sc != 0x4A && sc != 0x4E;
    if (kp && numlock) { pushkey(0, (CHAR16)"789-456+1230."[sc - 0x47]); return; }
    if (ext || kp) for (int i = 0; i < 10; i++) if (nav_sc[i] == sc) { pushkey((u16)(i + 1), 0); return; }
    if (ext && sc == 0x1C) { pushkey(0, '\r'); return; }
    if (ext && sc == 0x35) { pushkey(0, '/'); return; }
    if (ext) return;
    if (sc == 0x37) { pushkey(0, '*'); return; }
    if (sc == 0x4A) { pushkey(0, '-'); return; }
    if (sc == 0x4E) { pushkey(0, '+'); return; }
    if (sc >= 0x3B && sc <= 0x44) { pushkey((u16)(0x0B + sc - 0x3B), 0); return; }
    if (sc == 0x57) { pushkey(0x15, 0); return; }
    if (sc == 0x58) { pushkey(0x16, 0); return; }
    if (sc == 0x01) { pushkey(0x17, 0); return; }
    if (sc == 0x39) { pushkey(0, ' '); return; }
    if (sc == 0x56) { pushkey(0, (shift & 3) ? '|' : '\\'); return; }    /* tecla extra ISO/ABNT2 */
    if (sc > 0 && sc < 0x36) {
        bool sh = (shift & (EFI_LEFT_SHIFT | EFI_RIGHT_SHIFT)) != 0;
        char l = us_lo[sc]; if (!l) return;
        if (caps && l >= 'a' && l <= 'z') sh = !sh;
        char c = sh ? us_hi[sc] : l;
        if ((shift & (EFI_LEFT_CONTROL | EFI_RIGHT_CONTROL)) && l >= 'a' && l <= 'z' && !(shift & EFI_RIGHT_ALT)) { pushkey(0, (CHAR16)(l - 'a' + 1)); return; }
        pushkey(0, (CHAR16)(u8)c);
    }
}
/* mouse */
static bool mouse_on, wheel; static u8 pkt[4]; static int np;
static i32 mdx, mdy, mdz; static u32 mbtn, mlatch; static bool mchanged;
static void aux_byte(u8 b) {
    if (!mouse_on) return;
    if (np == 0 && !(b & 0x08)) return;
    pkt[np++] = b;
    if (np < (wheel ? 4 : 3)) return;
    np = 0;
    if (pkt[0] & 0xC0) return;
    i32 x = pkt[1], y = pkt[2];
    if (pkt[0] & 0x10) x -= 256;
    if (pkt[0] & 0x20) y -= 256;
    mdx += x; mdy -= y;
    if (wheel) mdz += (int8_t)(u8)(pkt[3] << 4) >> 4;
    mbtn = pkt[0] & 7; mlatch |= mbtn; mchanged = true;
}
static void ps2_poll(void) {
    for (int g = 0; g < 64; g++) {
        u8 s = inb(0x64);
        if (!(s & 1)) return;
        u8 b = inb(0x60);
        if (s & 0x20) aux_byte(b); else kbd_byte(b);
    }
}
static bool wait_in(void) { for (int i = 0; i < 100000; i++) { if (!(inb(0x64) & 2)) return true; } return false; }
static int read_aux(u32 ms) { u64 end = now_us() + ms * 1000; while (now_us() < end) { u8 s = inb(0x64); if ((s & 0x21) == 0x21) return inb(0x60); if (s & 1) kbd_byte(inb(0x60)); } return -1; }
static bool aux_cmd(u8 c) {
    for (int t = 0; t < 3; t++) {
        if (!wait_in()) return false; outb(0x64, 0xD4);
        if (!wait_in()) return false; outb(0x60, c);
        int r = read_aux(60);
        if (r == 0xFA) return true;
        if (r != 0xFE) return false;
    }
    return false;
}
static char mouse_info[64] = "no PS/2 mouse";
static void mouse_init(void) {
    if (inb(0x64) == 0xFF) return;
    if (!wait_in()) return;
    outb(0x64, 0xA8);
    if (!aux_cmd(0xF6)) return;
    if (aux_cmd(0xF3) && aux_cmd(200) && aux_cmd(0xF3) && aux_cmd(100) && aux_cmd(0xF3) && aux_cmd(80) && aux_cmd(0xF2)) wheel = read_aux(60) == 3;
    aux_cmd(0xF3); aux_cmd(100);
    if (!aux_cmd(0xF4)) return;
    mouse_on = true;
    const char *m = wheel ? "PS/2 mouse with wheel" : "PS/2 mouse"; int i = 0; while (m[i]) { mouse_info[i] = m[i]; i++; } mouse_info[i] = 0;
}

/* ============================================================ eventos */
typedef struct { int kind; u64 due; } Ev;          /* 1 tempo, 2 teclado, 3 mouse */
static Ev ev_key = { 2, 0 }, ev_mouse = { 3, 0 };
static bool ev_ready(Ev *e) {
    ps2_poll();
    if (e->kind == 1) return e->due && now_us() >= e->due;
    if (e->kind == 2) return kh != kt;
    return mchanged;
}
static EFI_STATUS EFIAPI bs_createevent(UINT32 t, UINTN tpl, void *f, void *c, EFI_EVENT *e) {
    (void)t; (void)tpl; (void)f; (void)c;
    Ev *x = pool_alloc(sizeof(Ev)); if (!x) return OUT_OF_RES;
    x->kind = 1; x->due = 0; *e = x; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI bs_settimer(EFI_EVENT e, EFI_TIMER_DELAY t, UINT64 ns100) {
    Ev *x = e; x->due = t == TimerCancel ? 0 : now_us() + ns100 / 10 + 1; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI bs_waitforevent(UINTN n, EFI_EVENT *evs, UINTN *idx) {
    for (;;) {
        for (UINTN i = 0; i < n; i++) if (ev_ready((Ev *)evs[i])) { if (((Ev *)evs[i])->kind == 1) ((Ev *)evs[i])->due = 0; *idx = i; return EFI_SUCCESS; }
        __asm__ volatile("pause");
    }
}
static EFI_STATUS EFIAPI bs_closeevent(EFI_EVENT e) { if (e != &ev_key && e != &ev_mouse) pool_free(e); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI bs_checkevent(EFI_EVENT e) { return ev_ready((Ev *)e) ? EFI_SUCCESS : EFI_NOT_READY; }
static EFI_STATUS EFIAPI bs_stall(UINTN us) { u64 end = now_us() + us; while (now_us() < end) { ps2_poll(); __asm__ volatile("pause"); } return EFI_SUCCESS; }
static EFI_STATUS EFIAPI bs_watchdog(UINTN a, UINT64 b, UINTN c, CHAR16 *d) { (void)a; (void)b; (void)c; (void)d; return EFI_SUCCESS; }

/* ============================================================ protocolos e handles */
typedef struct { void *handle; const EFI_GUID *guid; void *iface; } Reg;
static Reg regs[48]; static int nregs;
static int h_image, h_con, h_gop, h_mouse, h_fs, h_pci, h_serial;
static void reg(void *h, const EFI_GUID *g, void *i) { if (nregs < COUNT(regs)) { regs[nregs].handle = h; regs[nregs].guid = g; regs[nregs].iface = i; nregs++; } }
static EFI_STATUS EFIAPI bs_handleprotocol(EFI_HANDLE h, EFI_GUID *g, void **i) {
    for (int k = 0; k < nregs; k++) if (regs[k].handle == h && geq(regs[k].guid, g)) { *i = regs[k].iface; return EFI_SUCCESS; }
    return UNSUPPORTED;
}
static EFI_STATUS EFIAPI bs_locatehandlebuffer(EFI_LOCATE_SEARCH_TYPE t, EFI_GUID *g, void *k, UINTN *n, EFI_HANDLE **out) {
    (void)k;
    EFI_HANDLE *b = pool_alloc(sizeof(EFI_HANDLE) * COUNT(regs)); UINTN c = 0;
    if (!b) return OUT_OF_RES;
    for (int i = 0; i < nregs; i++) {
        if (t == ByProtocol && !geq(regs[i].guid, g)) continue;
        bool dup = false; for (UINTN j = 0; j < c; j++) if (b[j] == regs[i].handle) dup = true;
        if (!dup) b[c++] = regs[i].handle;
    }
    if (!c) { pool_free(b); return NOT_FOUND; }
    *n = c; *out = b; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI bs_locateprotocol(EFI_GUID *g, void *r, void **i) { (void)r; for (int k = 0; k < nregs; k++) if (geq(regs[k].guid, g)) { *i = regs[k].iface; return EFI_SUCCESS; } return NOT_FOUND; }

static const EFI_GUID G_GOP    = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
static const EFI_GUID G_EDIDA  = { 0xbd8c1056, 0x9f36, 0x44ec, { 0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86 } };
static const EFI_GUID G_TIEX   = { 0xdd9e7534, 0x7762, 0x4698, { 0x8c, 0x14, 0xf5, 0x85, 0x17, 0xa6, 0x25, 0xaa } };
static const EFI_GUID G_SPTR   = { 0x31878c87, 0x0b75, 0x11d5, { 0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };
static const EFI_GUID G_LIMG   = { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const EFI_GUID G_SFS    = { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const EFI_GUID G_FINFO  = { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const EFI_GUID G_FSINFO = { 0x09576e93, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const EFI_GUID G_DP     = { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const EFI_GUID G_BIO    = { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const EFI_GUID G_RBIO   = { 0x2f707ebb, 0x4a1a, 0x11d4, { 0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };
static const EFI_GUID G_SERIAL = { 0xBB25CF6F, 0xF1D4, 0x11D2, { 0x9A, 0x0C, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0xFD } };

/* ============================================================ console de texto */
static EFI_STATUS EFIAPI con_out(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t, CHAR16 *s) { (void)t; while (*s) { putc_all(*s < 128 ? (char)*s : '?'); s++; } return EFI_SUCCESS; }
static EFI_STATUS EFIAPI con_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t, BOOLEAN b) { (void)t; (void)b; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI con_clear(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t) { (void)t; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI con_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *t, BOOLEAN b) { (void)t; (void)b; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI kx_reset(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *t, BOOLEAN b) { (void)t; (void)b; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI kx_read(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *t, EFI_KEY_DATA *k) {
    (void)t; ps2_poll();
    if (kh == kt) return EFI_NOT_READY;
    *k = keyq[kh]; kh = (kh + 1) % COUNT(keyq); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ki_read(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *t, EFI_INPUT_KEY *k) {
    (void)t; ps2_poll();
    if (kh == kt) return EFI_NOT_READY;
    *k = keyq[kh].Key; kh = (kh + 1) % COUNT(keyq); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ki_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *t, BOOLEAN b) { (void)t; (void)b; return EFI_SUCCESS; }

/* ============================================================ mouse como protocolo */
static EFI_SIMPLE_POINTER_MODE pmode = { 8, 8, 0, 1, 1 };
static EFI_STATUS EFIAPI ptr_reset(EFI_SIMPLE_POINTER_PROTOCOL *p, BOOLEAN b) { (void)p; (void)b; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI ptr_state(EFI_SIMPLE_POINTER_PROTOCOL *p, EFI_SIMPLE_POINTER_STATE *s) {
    (void)p; ps2_poll();
    if (!mchanged) return EFI_NOT_READY;
    mchanged = false;
    u32 b = mbtn | mlatch; mlatch = 0;
    if (b != mbtn) mchanged = true;                 /* o soltar de um clique rápido aparece na leitura seguinte */
    s->RelativeMovementX = mdx; s->RelativeMovementY = mdy; s->RelativeMovementZ = mdz;
    s->LeftButton = (b & 1) != 0; s->RightButton = (b & 2) != 0;
    mdx = mdy = mdz = 0;
    return EFI_SUCCESS;
}
static const u8 dp_end[4] = { 0x7F, 0xFF, 4, 0 };    /* caminho de dispositivo mínimo: "é um dispositivo de verdade" */

/* ============================================================ serial como protocolo (registro de depuração) */
typedef struct { UINT32 Revision; void *Reset, *SetAttributes, *SetControl, *GetControl;
                 EFI_STATUS (EFIAPI *Write)(void *self, UINTN *size, void *buf); void *Read, *Mode; } SERIAL_IO;
static EFI_STATUS EFIAPI sio_write(void *self, UINTN *n, void *buf) { (void)self; for (UINTN i = 0; i < *n; i++) ser_putc(((char *)buf)[i]); return EFI_SUCCESS; }

/* ============================================================ relógio (CMOS) e energia */
static u8 cmos(u8 r) { outb(0x70, r); return inb(0x71); }
static EFI_STATUS EFIAPI rt_gettime(EFI_TIME *t, void *c) {
    (void)c;
    for (int i = 0; i < 100000 && (cmos(0x0A) & 0x80); i++) {}
    u8 s = cmos(0), m = cmos(2), h = cmos(4), d = cmos(7), mo = cmos(8), y = cmos(9), cen = cmos(0x32), b = cmos(0x0B);
    #define BCD(x) (u8)(((x) & 0x0F) + ((x) >> 4) * 10)
    bool pm = h & 0x80; h &= 0x7F;
    if (!(b & 4)) { s = BCD(s); m = BCD(m); h = BCD(h); d = BCD(d); mo = BCD(mo); y = BCD(y); cen = BCD(cen); }
    if (!(b & 2)) { if (h == 12) h = 0; if (pm) h += 12; }
    memset(t, 0, sizeof *t);
    t->Year = (u16)((cen >= 19 && cen <= 21 ? cen : 20) * 100 + y); t->Month = mo; t->Day = d; t->Hour = h; t->Minute = m; t->Second = s;
    t->TimeZone = 0x07FF;                               /* EFI_UNSPECIFIED_TIMEZONE */
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI rt_getvariable(CHAR16 *n, EFI_GUID *g, UINT32 *a, UINTN *s, void *d) { (void)n; (void)g; (void)a; (void)s; (void)d; return NOT_FOUND; }
/* ACPI: desligamento pelo estado S5 (o mesmo que um sistema operacional faz) */
static bool acpi_off(void) {
    u8 *rsdp = 0;
    u32 ebda = (u32)(*(u16 *)0x40E) << 4;
    for (u32 a = ebda; ebda && a < ebda + 1024 && !rsdp; a += 16) if (!memcmp((void *)a, "RSD PTR ", 8)) rsdp = (u8 *)a;
    for (u32 a = 0xE0000; a < 0x100000 && !rsdp; a += 16) if (!memcmp((void *)a, "RSD PTR ", 8)) rsdp = (u8 *)a;
    if (!rsdp) return false;
    u8 *rsdt = (u8 *)*(u32 *)(rsdp + 16);
    if (!rsdt || memcmp(rsdt, "RSDT", 4)) return false;
    u32 n = (*(u32 *)(rsdt + 4) - 36) / 4; u8 *facp = 0;
    for (u32 i = 0; i < n && !facp; i++) { u8 *t = (u8 *)((u32 *)(rsdt + 36))[i]; if (t && !memcmp(t, "FACP", 4)) facp = t; }
    if (!facp) return false;
    u8 *dsdt = (u8 *)*(u32 *)(facp + 40);
    u32 smi = *(u32 *)(facp + 48); u8 en = facp[52];
    u32 pm1a = *(u32 *)(facp + 64), pm1b = *(u32 *)(facp + 68);
    if (!dsdt || memcmp(dsdt, "DSDT", 4) || !pm1a) return false;
    u32 len = *(u32 *)(dsdt + 4); u8 *p = 0;
    for (u32 i = 36; i + 4 < len; i++) if (!memcmp(dsdt + i, "_S5_", 4) && (dsdt[i - 1] == 0x08 || (dsdt[i - 2] == 0x08 && dsdt[i - 1] == '\\'))) { p = dsdt + i + 4; break; }
    if (!p || *p != 0x12) return false;
    p++; p += ((*p & 0xC0) >> 6) + 1; p++;               /* PkgLength e NumElements */
    if (*p == 0x0A) p++;
    u16 a = (u16)(*p << 10); p++;
    if (*p == 0x0A) p++;
    u16 b = (u16)(*p << 10);
    if (smi && en && !(inw((u16)pm1a) & 1)) { outb((u16)smi, en); for (int i = 0; i < 300 && !(inw((u16)pm1a) & 1); i++) bs_stall(10000); }
    outw((u16)pm1a, (u16)(a | 0x2000));
    if (pm1b) outw((u16)pm1b, (u16)(b | 0x2000));
    bs_stall(200000);
    return false;
}
static void EFIAPI rt_reset(EFI_RESET_TYPE t, EFI_STATUS s, UINTN n, void *d) {
    (void)s; (void)n; (void)d;
    if (t == EfiResetShutdown) {
        acpi_off();
        outw(0x604, 0x2000); outw(0xB004, 0x2000); outw(0x4004, 0x3400);   /* QEMU, Bochs, VirtualBox */
        bs_stall(200000);
        puts_("\r\nFolioOS: you can switch the computer off now.\r\n");
        for (;;) __asm__ volatile("hlt");
    }
    outb(0xCF9, 0x02); bs_stall(50); outb(0xCF9, 0x06); bs_stall(200000);
    wait_in(); outb(0x64, 0xFE); bs_stall(200000);
    struct { u16 l; u32 b; } __attribute__((packed)) idt0 = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(idt0));
    for (;;) __asm__ volatile("hlt");
}

/* ============================================================ disco (INT 13h) */
/* Cada disco do BIOS (o de boot e os discos rígidos 80h…) vira um EFI_BLOCK_IO, para o instalador.
 * Caminho de dispositivo: nó de fabricante com o número do drive + nó do barramento (EDD 3.0: ATA, SATA,
 * USB, SCSI), assim o FolioOS sabe o tipo do disco e qual é o de boot (o volume tem o mesmo começo). */
#define LOW_EDD 0x5700
typedef struct {
    EFI_BLOCK_IO_PROTOCOL bio;             /* primeiro campo: o ponteiro do protocolo é o do disco */
    EFI_BLOCK_IO_MEDIA media;
    u8 drive; bool lba_ok; u32 heads, spt;
    u8 dp[21 + 10 + 4]; int handle;
} BDisk;
static BDisk bdisks[8]; static int nbdisks;
static BDisk *bootd;
static bool bdisk_init(BDisk *d, u8 drv) {
    memset(d, 0, sizeof *d); d->drive = drv;
    RmRegs r; memset(&r, 0, sizeof r);
    r.eax = 0x4100; r.ebx = 0x55AA; r.edx = drv;
    rm(0x13, &r);
    d->lba_ok = !(r.eflags & 1) && (r.ebx & 0xFFFF) == 0xAA55 && (r.ecx & 1);
    u64 nsec = 0;
    memset(&r, 0, sizeof r);
    r.eax = 0x0800; r.edx = drv;
    rm(0x13, &r);
    if (!(r.eflags & 1)) {
        d->heads = ((r.edx >> 8) & 0xFF) + 1; d->spt = r.ecx & 0x3F;
        u32 cyl = (((r.ecx & 0xC0) << 2) | ((r.ecx >> 8) & 0xFF)) + 1;
        nsec = (u64)cyl * d->heads * d->spt;
    }
    if (!d->lba_ok && !d->spt) return false;
    /* parâmetros estendidos: tamanho real, removível e (EDD 3.0) o barramento */
    u8 *p = (u8 *)LOW_EDD; memset(p, 0, 0x4A); *(u16 *)p = 0x4A;
    char bus[9] = "";
    if (d->lba_ok) {
        memset(&r, 0, sizeof r); r.eax = 0x4800; r.edx = drv; r.esi = LOW_EDD; r.ds = 0;
        rm(0x13, &r);
        if (!(r.eflags & 1)) {
            u64 tot = *(u64 *)(p + 0x10);
            if (tot && tot < (1ull << 40)) nsec = tot;
            d->media.RemovableMedia = (*(u16 *)(p + 2) & 4) != 0;
            if (*(u16 *)p >= 0x42 && *(u16 *)(p + 0x1E) == 0xBEDD) { memcpy(bus, p + 0x28, 8); bus[8] = 0; }
            puts_("  drive "); putn(drv, 16); puts_("h: EDD size "); putn(*(u16 *)p, 16); puts_(" key "); putn(*(u16 *)(p + 0x1E), 16); puts_(" bus \""); puts_(bus); puts_("\"\r\n");
        }
    }
    if (!nsec) return false;
    d->media.MediaId = drv; d->media.MediaPresent = 1; d->media.BlockSize = 512; d->media.LastBlock = nsec - 1;
    /* caminho: fabricante (GUID do FolioOS + drive), barramento, fim */
    static const u8 vguid[16] = { 0x46, 0x4F, 0x4C, 0x49, 0x4F, 0x42, 0x49, 0x4F, 0x53, 0x44, 0x52, 0x49, 0x56, 0x45, 0x00, 0x01 };
    u8 *q = d->dp;
    q[0] = 1; q[1] = 4; q[2] = 21; q[3] = 0; memcpy(q + 4, vguid, 16); q[20] = drv; q += 21;
    u8 sub = !memcmp(bus, "SATA", 4) ? 0x12 : !memcmp(bus, "ATAPI", 5) || !memcmp(bus, "ATA", 3) ? 0x01 : !memcmp(bus, "USB", 3) ? 0x05 : !memcmp(bus, "SCSI", 4) ? 0x02 : 0;
    if (sub) { u8 len = sub == 0x12 ? 10 : sub == 0x05 ? 6 : 8; q[0] = 3; q[1] = sub; q[2] = len; q[3] = 0; memset(q + 4, 0, len - 4u); q += len; }
    q[0] = 0x7F; q[1] = 0xFF; q[2] = 4; q[3] = 0;
    return true;
}
static bool drive_rw(BDisk *d, u64 lba, u32 n, u8 *buf, bool w) {
    u8 drive = d->drive; bool lba_ok = d->lba_ok; u32 heads = d->heads, spt = d->spt;
    while (n) {
        u32 k = n > BOUNCE_SECS ? BOUNCE_SECS : n;
        if (!lba_ok) k = 1;
        if (w) memcpy((void *)BOUNCE, buf, k * 512);
        bool ok = false;
        for (int tries = 0; tries < 3 && !ok; tries++) {
            RmRegs r; memset(&r, 0, sizeof r);
            if (lba_ok) {
                u8 *dap = (u8 *)LOW_DAP; memset(dap, 0, 16);
                dap[0] = 16; *(u16 *)(dap + 2) = (u16)k; *(u16 *)(dap + 4) = 0; *(u16 *)(dap + 6) = BOUNCE >> 4; *(u64 *)(dap + 8) = lba;
                r.eax = w ? 0x4300 : 0x4200; r.edx = drive; r.esi = LOW_DAP; r.ds = 0;
            } else {
                u32 c = (u32)(lba / (spt * heads)), t = (u32)(lba % (spt * heads)), h = t / spt, s = t % spt + 1;
                if (c > 1023) return false;
                r.eax = (w ? 0x0300 : 0x0200) | 1; r.ecx = ((c & 0xFF) << 8) | ((c >> 2) & 0xC0) | s; r.edx = (h << 8) | drive;
                r.es = BOUNCE >> 4; r.ebx = 0;
            }
            rm(0x13, &r);
            ok = !(r.eflags & 1) && !(r.eax & 0xFF00);
            if (!ok) { RmRegs z; memset(&z, 0, sizeof z); z.edx = drive; rm(0x13, &z); }
        }
        if (!ok) return false;
        if (!w) memcpy(buf, (void *)BOUNCE, k * 512);
        lba += k; n -= k; buf += k * 512;
    }
    return true;
}
static bool disk_rw(u64 lba, u32 n, u8 *buf, bool w) { return drive_rw(bootd, lba, n, buf, w); }
static bool fat_io(void *ctx, u64 lba, u32 n, void *buf, bool w) { (void)ctx; return disk_rw(lba, n, buf, w); }

static Fat fat;
static EFI_STATUS bio_rw(EFI_BLOCK_IO_PROTOCOL *b, UINT32 m, UINT64 lba, UINTN sz, void *buf, bool w) {
    BDisk *d = (BDisk *)b;
    if (m != d->media.MediaId) return EFI_ERR(13);
    if (sz % 512 || lba + sz / 512 > d->media.LastBlock + 1) return EFI_ERR(2);
    if (w && d == bootd) fat_flush(&fat);           /* o cache do FAT32 do volume de boot vai antes */
    return drive_rw(d, lba, (u32)(sz / 512), buf, w) ? EFI_SUCCESS : DEV_ERROR;
}
static EFI_STATUS EFIAPI bio_read(EFI_BLOCK_IO_PROTOCOL *b, UINT32 m, UINT64 lba, UINTN sz, void *buf) { return bio_rw(b, m, lba, sz, buf, false); }
static EFI_STATUS EFIAPI bio_write(EFI_BLOCK_IO_PROTOCOL *b, UINT32 m, UINT64 lba, UINTN sz, void *buf) { return bio_rw(b, m, lba, sz, buf, true); }
static EFI_STATUS EFIAPI bio_flush(EFI_BLOCK_IO_PROTOCOL *b) { (void)b; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI bio_reset(EFI_BLOCK_IO_PROTOCOL *b, BOOLEAN x) { (void)b; (void)x; return EFI_SUCCESS; }
/* disco de boot + discos rígidos que o BIOS informa (BDA 0x475) */
static bool disks_scan(u8 boot) {
    if (!bdisk_init(&bdisks[0], boot)) return false;
    bootd = &bdisks[0]; nbdisks = 1;
    u8 nhd = *(u8 *)0x475; if (nhd > 8) nhd = 8;
    for (u8 k = 0; k < nhd && nbdisks < COUNT(bdisks); k++) {
        u8 drv = (u8)(0x80 + k);
        if (drv == boot) continue;
        if (bdisk_init(&bdisks[nbdisks], drv)) nbdisks++;
    }
    for (int i = 0; i < nbdisks; i++) {
        BDisk *d = &bdisks[i];
        d->bio.Revision = 0x10000; d->bio.Media = &d->media;
        d->bio.Reset = bio_reset; d->bio.ReadBlocks = bio_read; d->bio.WriteBlocks = bio_write; d->bio.FlushBlocks = bio_flush;
    }
    return true;
}

/* ============================================================ sistema de arquivos (FAT32 -> EFI_FILE_PROTOCOL) */
static void fat_now(u16 *d, u16 *t) {
    EFI_TIME e; rt_gettime(&e, 0);
    if (e.Year < 1980) return;
    *d = (u16)(((e.Year - 1980) << 9) | (e.Month << 5) | e.Day); *t = (u16)((e.Hour << 11) | (e.Minute << 5) | (e.Second / 2));
}
typedef struct { EFI_FILE_PROTOCOL p; FatNode n; u64 pos; u32 cookie; u16 name[256]; bool write; } FH;
static EFI_FILE_PROTOCOL file_proto;
static FH *fh_new(const FatNode *n, const u16 *name) {
    FH *h = pool_alloc(sizeof(FH)); if (!h) return 0;
    memset(h, 0, sizeof *h); h->p = file_proto; h->n = *n;
    int i = 0; if (name) for (; name[i] && i < 255; i++) h->name[i] = name[i];
    h->name[i] = 0;
    return h;
}
static EFI_STATUS fat_err(int r) {
    switch (r) { case FAT_NOTFOUND: return NOT_FOUND; case FAT_EXISTS: return ACCESS_DEN; case FAT_FULL: return VOL_FULL;
                 case FAT_BADNAME: return INVALID; case FAT_NOTEMPTY: return ACCESS_DEN; default: return DEV_ERROR; }
}
/* volta à pasta-mãe: a entrada ".." da pasta */
static bool parent_of(const FatNode *d, FatNode *out) {
    if (d->ent < 0) { fat_root(&fat, out); return true; }
    FatNode root; fat_root(&fat, &root);
    if (d->dir_clus == fat.root) { *out = root; return true; }
    /* acha a entrada da pasta-mãe dentro da avó: a avó é o ".." da mãe */
    u8 sec[512]; u64 lba = fat.part + fat.data_lba + (u64)(d->dir_clus - 2) * fat.spc;
    if (!disk_rw(lba, 1, sec, false)) return false;
    u32 gp = (u32)(*(u16 *)(sec + 32 + 20)) << 16 | *(u16 *)(sec + 32 + 26);
    FatNode g; if (gp == 0) fat_root(&fat, &g); else { memset(&g, 0, sizeof g); g.clus = gp; g.attr = 0x10; }
    u16 nm[256]; u32 ck = 0; FatNode ch;
    while (fat_readdir(&fat, &g, &ck, nm, &ch) == 1) if ((ch.attr & 0x10) && ch.clus == d->dir_clus) { *out = ch; return true; }
    return false;
}
static EFI_STATUS EFIAPI f_open(EFI_FILE_PROTOCOL *t, EFI_FILE_PROTOCOL **nh, CHAR16 *path, UINT64 mode, UINT64 attr) {
    FH *base = (FH *)t; FatNode cur = base->n; u16 last[256]; last[0] = 0;
    const CHAR16 *s = path;
    if (*s == '\\') { fat_root(&fat, &cur); while (*s == '\\') s++; }
    if (!(cur.attr & 0x10)) return NOT_FOUND;
    while (*s) {
        u16 part[256]; int k = 0;
        while (*s && *s != '\\') { if (k < 255) part[k++] = *s; s++; }
        part[k] = 0; while (*s == '\\') s++;
        if (k == 0 || (k == 1 && part[0] == '.')) continue;
        if (k == 2 && part[0] == '.' && part[1] == '.') { FatNode p; if (!parent_of(&cur, &p)) return NOT_FOUND; cur = p; last[0] = 0; continue; }
        FatNode nx; int r = fat_lookup(&fat, &cur, part, &nx);
        if (r == FAT_NOTFOUND && !*s && (mode & EFI_FILE_MODE_CREATE)) {
            r = fat_create(&fat, &cur, part, (attr & EFI_FILE_DIRECTORY) != 0, &nx);
            fat_flush(&fat);
        }
        if (r) return fat_err(r);
        if (*s && !(nx.attr & 0x10)) return NOT_FOUND;
        cur = nx; for (int i = 0; i <= k; i++) last[i] = part[i];
    }
    FH *h = fh_new(&cur, last); if (!h) return OUT_OF_RES;
    h->write = (mode & EFI_FILE_MODE_WRITE) != 0;
    *nh = &h->p; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI f_close(EFI_FILE_PROTOCOL *t) { fat_flush(&fat); pool_free(t); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI f_delete(EFI_FILE_PROTOCOL *t) {
    FH *h = (FH *)t; int r = h->n.ent < 0 ? FAT_BADNAME : fat_delete(&fat, &h->n);
    fat_flush(&fat); pool_free(t);
    return r ? (EFI_STATUS)2 : EFI_SUCCESS;            /* EFI_WARN_DELETE_FAILURE */
}
static void to_time(u16 d, u16 t, EFI_TIME *e) {
    memset(e, 0, sizeof *e);
    e->Year = (u16)(1980 + (d >> 9)); e->Month = (u8)((d >> 5) & 15); e->Day = (u8)(d & 31);
    e->Hour = (u8)(t >> 11); e->Minute = (u8)((t >> 5) & 63); e->Second = (u8)((t & 31) * 2);
    e->TimeZone = 0x07FF;
}
static UINTN info_size(const u16 *name) { int n = 0; while (name[n]) n++; return sizeof(EFI_FILE_INFO) + (UINTN)n * 2; }
static void fill_info(EFI_FILE_INFO *i, const FatNode *n, const u16 *name) {
    UINTN sz = info_size(name);
    memset(i, 0, sz);
    i->Size = sz; i->FileSize = n->attr & 0x10 ? 0 : n->size;
    i->PhysicalSize = (i->FileSize + fat.bpc - 1) / fat.bpc * fat.bpc;
    to_time(n->wdate, n->wtime, &i->ModificationTime); i->CreateTime = i->ModificationTime; i->LastAccessTime = i->ModificationTime;
    i->Attribute = (n->attr & 0x10 ? EFI_FILE_DIRECTORY : 0) | (n->attr & 0x01 ? 1 : 0) | (n->attr & 0x02 ? 2 : 0) | (n->attr & 0x04 ? 4 : 0) | (n->attr & 0x20 ? 0x20 : 0);
    int k = 0; while (name[k]) { i->FileName[k] = name[k]; k++; } i->FileName[k] = 0;
}
static EFI_STATUS EFIAPI f_read(EFI_FILE_PROTOCOL *t, UINTN *sz, void *buf) {
    FH *h = (FH *)t;
    if (h->n.attr & 0x10) {
        u16 nm[256]; FatNode ch; u32 ck = h->cookie;
        if (fat_readdir(&fat, &h->n, &ck, nm, &ch) != 1) { *sz = 0; return EFI_SUCCESS; }
        UINTN need = info_size(nm);
        if (*sz < need) { *sz = need; return EFI_BUFFER_TOO_SMALL; }
        fill_info(buf, &ch, nm); *sz = need; h->cookie = ck;
        return EFI_SUCCESS;
    }
    if (h->pos >= h->n.size) { *sz = 0; return EFI_SUCCESS; }
    int r = fat_read(&fat, &h->n, (u32)h->pos, buf, (u32)*sz);
    if (r < 0) return DEV_ERROR;
    *sz = (UINTN)r; h->pos += (u32)r; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI f_write(EFI_FILE_PROTOCOL *t, UINTN *sz, void *buf) {
    FH *h = (FH *)t;
    if (h->n.attr & 0x10) return UNSUPPORTED;
    if (!h->write) return ACCESS_DEN;
    int r = fat_write(&fat, &h->n, (u32)h->pos, buf, (u32)*sz);
    if (r < 0) { *sz = 0; return fat_err(r); }
    h->pos += (u32)r; *sz = (UINTN)r; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI f_getpos(EFI_FILE_PROTOCOL *t, UINT64 *p) { FH *h = (FH *)t; if (h->n.attr & 0x10) return UNSUPPORTED; *p = h->pos; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI f_setpos(EFI_FILE_PROTOCOL *t, UINT64 p) {
    FH *h = (FH *)t;
    if (h->n.attr & 0x10) { if (p) return UNSUPPORTED; h->cookie = 0; return EFI_SUCCESS; }
    h->pos = p == ~0ull ? h->n.size : p; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI f_getinfo(EFI_FILE_PROTOCOL *t, EFI_GUID *g, UINTN *sz, void *buf) {
    FH *h = (FH *)t;
    if (geq(g, &G_FSINFO)) {
        int ln = 0; while (fat.label[ln]) ln++;
        UINTN need = sizeof(EFI_FILE_SYSTEM_INFO) + (UINTN)ln * 2;
        if (*sz < need || !buf) { *sz = need; return EFI_BUFFER_TOO_SMALL; }
        EFI_FILE_SYSTEM_INFO *i = buf; memset(i, 0, need);
        i->Size = need; i->ReadOnly = 0; i->BlockSize = fat.bpc;
        i->VolumeSize = (u64)fat.nclus * fat.bpc; i->FreeSpace = (u64)fat_free_clusters(&fat) * fat.bpc;
        for (int k = 0; k < ln; k++) i->VolumeLabel[k] = (u8)fat.label[k];
        *sz = need; return EFI_SUCCESS;
    }
    if (!geq(g, &G_FINFO)) return UNSUPPORTED;
    UINTN need = info_size(h->name);
    if (*sz < need || !buf) { *sz = need; return EFI_BUFFER_TOO_SMALL; }
    fill_info(buf, &h->n, h->name); *sz = need; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI f_setinfo(EFI_FILE_PROTOCOL *t, EFI_GUID *g, UINTN sz, void *buf) {
    (void)sz; FH *h = (FH *)t;
    if (!geq(g, &G_FINFO)) return UNSUPPORTED;
    EFI_FILE_INFO *i = buf;
    if (!(h->n.attr & 0x10) && i->FileSize != h->n.size) {
        int r = fat_truncate(&fat, &h->n, (u32)i->FileSize); if (r) return fat_err(r);
        if (h->pos > h->n.size) h->pos = h->n.size;
    }
    const CHAR16 *nn = i->FileName;
    for (const CHAR16 *p = i->FileName; *p; p++) if (*p == '\\') nn = p + 1;     /* só o nome: renomear na mesma pasta */
    bool same = true; int k = 0;
    for (; nn[k] || h->name[k]; k++) if (nn[k] != h->name[k]) { same = false; break; }
    if (!same && nn[0] && h->n.ent >= 0) {
        int r = fat_rename(&fat, &h->n, nn); if (r) return fat_err(r);
        int j = 0; for (; nn[j] && j < 255; j++) h->name[j] = nn[j];
        h->name[j] = 0;
    }
    fat_flush(&fat);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI f_flush(EFI_FILE_PROTOCOL *t) { (void)t; return fat_flush(&fat) ? DEV_ERROR : EFI_SUCCESS; }
static EFI_STATUS EFIAPI sfs_open(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *s, EFI_FILE_PROTOCOL **r) {
    (void)s; FatNode root; fat_root(&fat, &root);
    FH *h = fh_new(&root, 0); if (!h) return OUT_OF_RES;
    *r = &h->p; return EFI_SUCCESS;
}
static bool mount_boot_volume(void) {
    u8 sec[512];
    if (!disk_rw(0, 1, sec, false)) return false;
    static const u16 n_folio[] = { 'F', 'O', 'L', 'I', 'O', 0 }, n_sys[] = { 'B', 'I', 'O', 'S', '.', 'S', 'Y', 'S', 0 };
    u32 cand[5]; int nc = 0;
    cand[nc++] = 0;                                            /* pendrive sem tabela de partições */
    if (sec[510] == 0x55 && sec[511] == 0xAA) for (int i = 0; i < 4; i++) { u8 *e = sec + 446 + i * 16; if (e[4]) cand[nc++] = *(u32 *)(e + 8); }
    for (int i = 0; i < nc; i++) {
        if (!fat_mount(&fat, fat_io, 0, cand[i])) continue;
        FatNode root, d, f; fat_root(&fat, &root);
        if (fat_lookup(&fat, &root, n_folio, &d) == FAT_OK && fat_lookup(&fat, &d, n_sys, &f) == FAT_OK) return true;
    }
    return false;
}

/* ============================================================ vídeo (VBE) */
typedef struct { u16 mode; u16 w, h, pitch; u8 bpp, rpos, gpos, bpos; u32 phys; } VMode;
static VMode vmodes[48]; static int nvmodes;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION ginfo;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE gmode;
static EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
static u8 edid[128]; static EFI_EDID_ACTIVE_PROTOCOL edid_p;
static void vbe_scan(void) {
    u8 *vi = (u8 *)LOW_VBE; memset(vi, 0, 512); memcpy(vi, "VBE2", 4);
    RmRegs r; memset(&r, 0, sizeof r); r.eax = 0x4F00; r.edi = LOW_VBE; r.es = 0;
    rm(0x10, &r);
    if ((r.eax & 0xFFFF) != 0x004F || memcmp(vi, "VESA", 4)) { puts_("VBE not available\r\n"); return; }
    u32 lp = *(u32 *)(vi + 14); u16 *list = (u16 *)(((lp >> 16) << 4) + (lp & 0xFFFF));
    u16 modes[128]; int nm = 0;
    for (int i = 0; i < 128 && list[i] != 0xFFFF; i++) modes[nm++] = list[i];
    for (int i = 0; i < nm && nvmodes < COUNT(vmodes); i++) {
        u8 *mi = (u8 *)LOW_MODE; memset(mi, 0, 256);
        memset(&r, 0, sizeof r); r.eax = 0x4F01; r.ecx = modes[i]; r.edi = LOW_MODE; r.es = 0;
        rm(0x10, &r);
        if ((r.eax & 0xFFFF) != 0x004F) continue;
        u16 attr = *(u16 *)mi; u8 bpp = mi[25], model = mi[27];
        if ((attr & 0x91) != 0x91 || model != 6 || (bpp != 32 && bpp != 24)) continue;     /* suportado, gráfico, linear */
        VMode *v = &vmodes[nvmodes];
        v->mode = modes[i]; v->w = *(u16 *)(mi + 18); v->h = *(u16 *)(mi + 20); v->bpp = bpp;
        v->pitch = *(u16 *)(mi + 16); if (*(u16 *)(mi + 50)) v->pitch = *(u16 *)(mi + 50);
        v->rpos = mi[32]; v->gpos = mi[34]; v->bpos = mi[36]; v->phys = *(u32 *)(mi + 40);
        if (v->w < 640 || v->h < 480 || !v->phys || v->pitch % (bpp / 8)) continue;
        nvmodes++;
    }
    memset(&r, 0, sizeof r); r.eax = 0x4F15; r.ebx = 1; r.edi = LOW_EDID; r.es = 0;
    memset((void *)LOW_EDID, 0, 128);
    rm(0x10, &r);
    if ((r.eax & 0xFFFF) == 0x004F && !memcmp((void *)LOW_EDID, "\0\xFF\xFF\xFF\xFF\xFF\xFF\0", 8)) {
        memcpy(edid, (void *)LOW_EDID, 128); edid_p.SizeOfEdid = 128; edid_p.Edid = edid;
    }
}
static void mode_info(const VMode *v, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *i) {
    memset(i, 0, sizeof *i);
    i->HorizontalResolution = v->w; i->VerticalResolution = v->h; i->PixelsPerScanLine = v->pitch / (v->bpp / 8u);
    if (v->bpp == 32 && v->rpos == 16 && v->gpos == 8 && v->bpos == 0) i->PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
    else if (v->bpp == 32 && v->rpos == 0 && v->gpos == 8 && v->bpos == 16) i->PixelFormat = PixelRedGreenBlueReserved8BitPerColor;
    else {
        i->PixelFormat = PixelBitMask;
        i->PixelInformation.RedMask = 0xFFu << v->rpos; i->PixelInformation.GreenMask = 0xFFu << v->gpos; i->PixelInformation.BlueMask = 0xFFu << v->bpos;
    }
}
static EFI_STATUS EFIAPI gop_query(EFI_GRAPHICS_OUTPUT_PROTOCOL *g, UINT32 m, UINTN *sz, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **i) {
    (void)g; if (m >= (u32)nvmodes) return INVALID;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *x = pool_alloc(sizeof *x); if (!x) return OUT_OF_RES;
    mode_info(&vmodes[m], x); *i = x; *sz = sizeof *x; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI gop_set(EFI_GRAPHICS_OUTPUT_PROTOCOL *g, UINT32 m) {
    (void)g; if (m >= (u32)nvmodes) return INVALID;
    RmRegs r; memset(&r, 0, sizeof r); r.eax = 0x4F02; r.ebx = 0x4000u | vmodes[m].mode;
    rm(0x10, &r);
    if ((r.eax & 0xFFFF) != 0x004F) return DEV_ERROR;
    text_mode = false;
    gmode.Mode = m; mode_info(&vmodes[m], &ginfo);
    gmode.FrameBufferBase = vmodes[m].phys; gmode.FrameBufferSize = (UINTN)vmodes[m].pitch * vmodes[m].h;
    memset((void *)vmodes[m].phys, 0, gmode.FrameBufferSize);
    return EFI_SUCCESS;
}
static void px_put(u32 x, u32 y, u32 c) {
    const VMode *v = &vmodes[gmode.Mode]; u8 *p = (u8 *)v->phys + y * v->pitch + x * (v->bpp / 8u);
    u8 r = (u8)(c >> 16), gg = (u8)(c >> 8), b = (u8)c;
    if (v->bpp == 32) { u32 o = ((u32)r << v->rpos) | ((u32)gg << v->gpos) | ((u32)b << v->bpos); *(u32 *)p = o; }
    else { p[v->rpos / 8] = r; p[v->gpos / 8] = gg; p[v->bpos / 8] = b; }
}
static u32 px_get(u32 x, u32 y) {
    const VMode *v = &vmodes[gmode.Mode]; u8 *p = (u8 *)v->phys + y * v->pitch + x * (v->bpp / 8u);
    if (v->bpp == 32) { u32 o = *(u32 *)p; return (((o >> v->rpos) & 255) << 16) | (((o >> v->gpos) & 255) << 8) | ((o >> v->bpos) & 255); }
    return ((u32)p[v->rpos / 8] << 16) | ((u32)p[v->gpos / 8] << 8) | p[v->bpos / 8];
}
static EFI_STATUS EFIAPI gop_blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *g, void *b, EFI_GRAPHICS_OUTPUT_BLT_OPERATION op, UINTN sx, UINTN sy, UINTN dx, UINTN dy, UINTN w, UINTN h, UINTN delta) {
    (void)g; u32 *buf = b; if (!delta) delta = w * 4;
    for (UINTN y = 0; y < h; y++) for (UINTN x = 0; x < w; x++) {
        if (op == EfiBltVideoFill) px_put((u32)(dx + x), (u32)(dy + y), buf[0]);
        else if (op == EfiBltBufferToVideo) px_put((u32)(dx + x), (u32)(dy + y), *(u32 *)((u8 *)buf + (sy + y) * delta + (sx + x) * 4));
        else if (op == EfiBltVideoToBltBuffer) *(u32 *)((u8 *)buf + (dy + y) * delta + (dx + x) * 4) = px_get((u32)(sx + x), (u32)(sy + y));
        else if (op == EfiBltVideoToVideo) px_put((u32)(dx + x), (u32)(dy + y), px_get((u32)(sx + x), (u32)(sy + y)));
    }
    return EFI_SUCCESS;
}
/* modo inicial: o nativo do monitor (EDID) se existir; senão 1024x768; senão o maior até 1920x1200 */
static int pick_mode(void) {
    int best = -1;
    if (edid_p.Edid) {
        u32 w = edid[56] | ((u32)(edid[58] & 0xF0) << 4), h = edid[59] | ((u32)(edid[61] & 0xF0) << 4);
        for (int i = 0; i < nvmodes; i++) if (vmodes[i].w == w && vmodes[i].h == h && (best < 0 || vmodes[i].bpp > vmodes[best].bpp)) best = i;
        if (best >= 0) return best;
    }
    for (int i = 0; i < nvmodes; i++) if (vmodes[i].w == 1024 && vmodes[i].h == 768 && (best < 0 || vmodes[i].bpp > vmodes[best].bpp)) best = i;
    if (best >= 0) return best;
    for (int i = 0; i < nvmodes; i++) {
        if (vmodes[i].w > 1920 || vmodes[i].h > 1200) continue;
        if (best < 0 || (u32)vmodes[i].w * vmodes[i].h > (u32)vmodes[best].w * vmodes[best].h) best = i;
    }
    return best;
}

/* ============================================================ carregador PE32 (BOOTIA32.EFI) */
typedef EFI_STATUS (*EntryFn)(EFI_HANDLE, EFI_SYSTEM_TABLE *);
static EntryFn load_pe(u8 *d, u32 n, u32 *base_out, u32 *size_out) {
    if (n < 512 || d[0] != 'M' || d[1] != 'Z') return 0;
    u32 pe = *(u32 *)(d + 0x3C);
    if (pe + 248 > n || memcmp(d + pe, "PE\0\0", 4)) return 0;
    u16 machine = *(u16 *)(d + pe + 4), nsec = *(u16 *)(d + pe + 6), optsz = *(u16 *)(d + pe + 20);
    u8 *opt = d + pe + 24;
    if (machine != 0x14C || *(u16 *)opt != 0x10B || *(u16 *)(opt + 68) != 10) return 0;
    u32 entry = *(u32 *)(opt + 16), ibase = *(u32 *)(opt + 28), imgsz = *(u32 *)(opt + 56), hdrsz = *(u32 *)(opt + 60);
    u32 reloc_rva = *(u32 *)(opt + 96 + 5 * 8), reloc_sz = *(u32 *)(opt + 96 + 5 * 8 + 4);
    u32 img = page_alloc((imgsz + 4095) / 4096); if (!img) return 0;
    memset((void *)img, 0, imgsz);
    memcpy((void *)img, d, hdrsz);
    u8 *sec = opt + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        u32 va = *(u32 *)(sec + 12), rsz = *(u32 *)(sec + 16), rptr = *(u32 *)(sec + 20), vsz = *(u32 *)(sec + 8);
        if (rsz > vsz && vsz) rsz = vsz;
        if (rsz && rptr + rsz <= n && va + rsz <= imgsz) memcpy((void *)(img + va), d + rptr, rsz);
    }
    u32 delta = img - ibase;
    for (u32 o = 0; o + 8 <= reloc_sz; ) {
        u32 page = *(u32 *)(img + reloc_rva + o), bsz = *(u32 *)(img + reloc_rva + o + 4);
        if (bsz < 8) break;
        for (u32 k = 8; k < bsz; k += 2) {
            u16 e = *(u16 *)(img + reloc_rva + o + k);
            if ((e >> 12) == 3) *(u32 *)(img + page + (e & 0xFFF)) += delta;
        }
        o += bsz;
    }
    *base_out = img; *size_out = imgsz;
    return (EntryFn)(img + entry);
}
/* troca a pilha (a do BIOS.SYS é pequena) e chama a aplicação */
__attribute__((used)) static volatile u32 cs_top, cs_st, cs_img, cs_fn, cs_save, cs_ret;   /* lidas pelo asm abaixo */
static EFI_STATUS call_on_stack(EntryFn fn, EFI_HANDLE img, EFI_SYSTEM_TABLE *st, u32 top) {
    cs_top = top; cs_st = (u32)st; cs_img = (u32)img; cs_fn = (u32)fn;
    __asm__ volatile(                                  /* só endereços absolutos: a pilha muda no meio */
        "mov %%esp, cs_save\n\t"
        "mov cs_top, %%esp\n\t"
        "pushl cs_st\n\t"
        "pushl cs_img\n\t"
        "call *cs_fn\n\t"
        "mov %%eax, cs_ret\n\t"
        "mov cs_save, %%esp\n\t"
        : : : "eax", "ebx", "ecx", "edx", "esi", "edi", "memory", "cc");
    return cs_ret;
}

/* ============================================================ início */
static CHAR16 vendor[] = { 'F','o','l','i','o','O','S',' ','B','I','O','S',' ','b','r','i','d','g','e',0 };
__attribute__((section(".text.entry"))) void bridge_main(BootInfo *info) {
    extern u8 __bss_start[], __bss_end[];
    for (u8 *p = __bss_start; p < __bss_end; p++) *p = 0;
    bi = info;
    ty = 2;
    ser_init();
    puts_("FolioOS BIOS bridge: protected mode\r\n");
    if (bi->magic != 0x464F4C49) fatal("bad BootInfo");
    u8 boot_drive = *bi->drive;
    calibrate();
    mem_init(); pool_init();
    { u32 total = 0; for (int i = 0; i < nranges; i++) total += ranges[i].pages / 256; puts_("memory: "); putn(total, 10); puts_(" MB, CPU "); putn((u32)(tsc_per_us), 10); puts_(" MHz\r\n"); }
    numlock = (*(u8 *)0x417 & 0x20) != 0;
    if (!disks_scan(boot_drive)) fatal("the BIOS gives no access to the boot disk (INT 13h)");
    puts_(bootd->lba_ok ? "disk: LBA, " : "disk: CHS, "); putn((u32)nbdisks, 10); puts_(" disk(s) found\r\n");
    if (!mount_boot_volume()) fatal("FAT32 volume with FOLIO\\BIOS.SYS not found");
    fat.now = fat_now;
    puts_("volume: "); puts_(fat.label[0] ? fat.label : "(no label)"); puts_("\r\n");
    static const u16 p_efi[] = { 'E','F','I',0 }, p_boot[] = { 'B','O','O','T',0 }, p_app[] = { 'B','O','O','T','I','A','3','2','.','E','F','I',0 };
    FatNode root, a, b, app; fat_root(&fat, &root);
    if (fat_lookup(&fat, &root, p_efi, &a) || fat_lookup(&fat, &a, p_boot, &b) || fat_lookup(&fat, &b, p_app, &app)) fatal("EFI\\BOOT\\BOOTIA32.EFI not found");
    u32 fpages = (app.size + 4095) / 4096; u8 *file = (u8 *)page_alloc(fpages);
    if (!file || fat_read(&fat, &app, 0, file, app.size) != (int)app.size) fatal("could not read BOOTIA32.EFI");
    u32 ibase, isize; EntryFn entry = load_pe(file, app.size, &ibase, &isize);
    if (!entry) fatal("BOOTIA32.EFI is not a valid 32-bit EFI application");
    bs_freepages((u32)file, fpages);
    puts_("BOOTIA32.EFI loaded\r\n");
    mouse_init(); puts_(mouse_info); puts_("\r\n");
    vbe_scan();
    puts_("video: "); putn((u32)nvmodes, 10); puts_(" VBE modes"); puts_(edid_p.Edid ? ", EDID\r\n" : "\r\n");

    static EFI_BOOT_SERVICES bs; static EFI_RUNTIME_SERVICES rt; static EFI_SYSTEM_TABLE st;
    static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL co; static EFI_SIMPLE_TEXT_INPUT_PROTOCOL ci; static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL cix;
    static EFI_SIMPLE_POINTER_PROTOCOL ptr; static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL sfs; static EFI_LOADED_IMAGE_PROTOCOL li;
    static SERIAL_IO sio; static u32 rbio_marker[16];
    bs.Hdr.Signature = 0x56524553544f4f42ull; bs.Hdr.Revision = 0x00020000;
    bs.AllocatePages = bs_allocpages; bs.FreePages = bs_freepages; bs.AllocatePool = bs_allocpool; bs.FreePool = bs_freepool;
    bs.CreateEvent = bs_createevent; bs.SetTimer = bs_settimer; bs.WaitForEvent = bs_waitforevent; bs.CloseEvent = bs_closeevent; bs.CheckEvent = bs_checkevent;
    bs.HandleProtocol = bs_handleprotocol; bs.LocateHandleBuffer = bs_locatehandlebuffer; bs.LocateProtocol = bs_locateprotocol;
    bs.Stall = bs_stall; bs.SetWatchdogTimer = bs_watchdog;
    rt.Hdr.Signature = 0x56524553544e5552ull; rt.GetTime = rt_gettime; rt.ResetSystem = rt_reset; rt.GetVariable = rt_getvariable;
    co.Reset = con_reset; co.OutputString = con_out; co.EnableCursor = con_cursor; co.ClearScreen = con_clear;
    ci.Reset = ki_reset; ci.ReadKeyStroke = ki_read; ci.WaitForKey = &ev_key;
    cix.Reset = kx_reset; cix.ReadKeyStrokeEx = kx_read; cix.WaitForKeyEx = &ev_key;
    ptr.Reset = ptr_reset; ptr.GetState = ptr_state; ptr.WaitForInput = &ev_mouse; ptr.Mode = &pmode;
    pmode.ResolutionZ = wheel ? 1 : 0;
    file_proto.Revision = 0x10000; file_proto.Open = f_open; file_proto.Close = f_close; file_proto.Delete = f_delete; file_proto.Read = f_read;
    file_proto.Write = f_write; file_proto.GetPosition = f_getpos; file_proto.SetPosition = f_setpos; file_proto.GetInfo = f_getinfo; file_proto.SetInfo = f_setinfo; file_proto.Flush = f_flush;
    sfs.Revision = 0x10000; sfs.OpenVolume = sfs_open;
    sio.Revision = 0x10000; sio.Write = sio_write;
    li.Revision = 0x1000; li.SystemTable = &st; li.DeviceHandle = &h_fs; li.ImageBase = (void *)ibase; li.ImageSize = isize;
    li.ImageCodeType = EfiLoaderCode; li.ImageDataType = EfiLoaderData;
    st.Hdr.Signature = 0x5453595320494249ull; st.Hdr.Revision = 0x00020000;
    st.FirmwareVendor = vendor; st.FirmwareRevision = 0x00010000;
    st.ConIn = &ci; st.ConOut = &co; st.StdErr = &co; st.ConsoleInHandle = &h_con; st.ConsoleOutHandle = &h_gop; st.StandardErrorHandle = &h_gop;
    st.RuntimeServices = &rt; st.BootServices = &bs;
    reg(&h_con, &G_TIEX, &cix);
    reg(&h_fs, &G_SFS, &sfs); reg(&h_fs, &G_DP, bootd->dp);   /* o volume fica "dentro" do disco de boot */
    for (int i = 0; i < nbdisks; i++) { reg(&bdisks[i].handle, &G_BIO, &bdisks[i].bio); reg(&bdisks[i].handle, &G_DP, bdisks[i].dp); }
    reg(&h_image, &G_LIMG, &li);
    reg(&h_pci, &G_RBIO, rbio_marker);
    if (com1) reg(&h_serial, &G_SERIAL, &sio);
    if (mouse_on) { reg(&h_mouse, &G_SPTR, &ptr); reg(&h_mouse, &G_DP, (void *)dp_end); }
    int m = pick_mode();
    if (m >= 0) {
        gop.QueryMode = gop_query; gop.SetMode = gop_set; gop.Blt = gop_blt; gop.Mode = &gmode;
        gmode.MaxMode = (u32)nvmodes; gmode.Info = &ginfo; gmode.SizeOfInfo = sizeof ginfo;
        reg(&h_gop, &G_GOP, &gop); reg(&h_gop, &G_DP, (void *)dp_end);
        if (edid_p.Edid) reg(&h_gop, &G_EDIDA, &edid_p);
        puts_("starting FolioOS\r\n");
        if (gop_set(&gop, (u32)m)) { gop.Mode = &gmode; puts_("could not set the video mode\r\n"); }
    } else puts_("no usable VBE mode: FolioOS will report that it has no graphics\r\n");
    u32 stack = page_alloc(256);                               /* 1 MB de pilha, como uma UEFI */
    if (!stack) fatal("out of memory for the stack");
    EFI_STATUS s = call_on_stack(entry, &h_image, &st, stack + 256 * 4096 - 16);
    text_mode = false;
    puts_("FolioOS returned "); putn((u32)s, 16); puts_("\r\n");
    rt_reset(EfiResetShutdown, 0, 0, 0);
}
