/* FOLIO v4 — plataforma UEFI (BOOTX64.EFI / BOOTIA32.EFI): GOP, teclado, ponteiros, arquivos, tempo */
#include "efi.h"
#include "../../core/folio.h"

EFI_SYSTEM_TABLE *efi_st; EFI_BOOT_SERVICES *efi_bs;
static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_HANDLE image;

static EFI_GUID gop_guid = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
static EFI_GUID edid_guid = { 0xbd8c1056, 0x9f36, 0x44ec, { 0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86 } };
static EFI_GUID tiex_guid = { 0xdd9e7534, 0x7762, 0x4698, { 0x8c, 0x14, 0xf5, 0x85, 0x17, 0xa6, 0x25, 0xaa } };
static EFI_GUID sptr_guid = { 0x31878c87, 0x0b75, 0x11d5, { 0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };
static EFI_GUID aptr_guid = { 0x8d59d32b, 0xc655, 0x4ae9, { 0x9b, 0x15, 0xf2, 0x59, 0x04, 0x99, 0x2a, 0x43 } };
static EFI_GUID limg_guid = { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID sfs_guid  = { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID finfo_guid = { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID fsinfo_guid = { 0x09576e93, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

/* ------------------------------------------------------------ tempo */
static u64 tsc_per_ms = 1000000, tsc0;
#if FOLIO_X86
static inline u64 rdtsc(void) { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }
#elif defined(__aarch64__)
static inline u64 rdtsc(void) { u64 v; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v)); return v; }   /* contador genérico do ARM */
#else
#error "arquitetura sem contador de tempo"
#endif
u64 pal_ms(void) { return (rdtsc() - tsc0) / tsc_per_ms; }
/* porta serial do firmware (EFI_SERIAL_IO_PROTOCOL): só para o registro de depuração */
typedef struct { UINT32 Revision; void *Reset, *SetAttributes, *SetControl, *GetControl;
                 EFI_STATUS (EFIAPI *Write)(void *self, UINTN *size, void *buf); void *Read, *Mode; } SERIAL_IO;
void pal_log(const char *s) {
    static EFI_GUID guid = { 0xBB25CF6F, 0xF1D4, 0x11D2, { 0x9A, 0x0C, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0xFD } };
    static SERIAL_IO *sio; static int state;
    if (!state) state = EFI_ERROR(BS->LocateProtocol(&guid, 0, (void **)&sio)) || !sio ? -1 : 1;
    if (state < 0) return;
    UINTN n = (UINTN)str_len(s);
    sio->Write(sio, &n, (void *)s);
}
void pal_now(DateTime *t) {
    EFI_TIME e; memset(&e, 0, sizeof e);
    if (EFI_ERROR(ST->RuntimeServices->GetTime(&e, 0))) { t->year = 2026; t->month = 1; t->day = 1; t->hour = t->min = t->sec = 0; return; }
    t->year = e.Year; t->month = e.Month; t->day = e.Day; t->hour = e.Hour; t->min = e.Minute; t->sec = e.Second;
}
#if defined(__x86_64__)
const char *pal_name(void) { return "UEFI x64"; }
#elif defined(__aarch64__)
const char *pal_name(void) { return "UEFI ARM64"; }
#else
/* o mesmo BOOTIA32.EFI roda também nos PCs só com BIOS, pela ponte do FOLIO\BIOS.SYS */
const char *pal_name(void) {
    static const CHAR16 br[] = { 'F','o','l','i','o','O','S',' ','B','I','O','S' };
    const CHAR16 *v = ST ? ST->FirmwareVendor : 0;
    if (v) { int i = 0; while (i < 12 && v[i] == br[i]) i++; if (i == 12) return "BIOS (FolioOS bridge)"; }
    return "UEFI IA32";
}
#endif
void pal_power(int a) {
    pal_audio_stop();
    ST->RuntimeServices->ResetSystem(a == 0 ? EfiResetShutdown : EfiResetCold, EFI_SUCCESS, 0, 0);
}

/* ------------------------------------------------------------ vídeo
 * Universal:
 *  - escolhe a saída certa entre várias (placa integrada + dedicada, notebook + monitor): a que tem
 *    monitor ligado (EDID ativo) e framebuffer; com dois ou mais monitores, a saída do console (espelha);
 *  - resolução nativa do painel (EDID: primeiro descritor de temporização válido); sem o modo nativo,
 *    o maior modo com a mesma proporção; "resolucao=" no CONFIG.INI escolhe outro;
 *  - qualquer formato de pixel: BGR, RGB, máscara de bits (16, 24 ou 32 bits) e "só Blt";
 *  - sem GOP: protocolo UGA (Macs Intel antigos e firmwares EFI 1.1);
 *  - framebuffer inválido (endereço, tamanho, linha) ou acima de 4 GB no modo 32 bits: Blt;
 *  - tela girada (tablets com painel em retrato): rotação automática, 0/90/180/270 nos ajustes
 *    ou Ctrl+Alt+setas; toque é convertido junto;
 *  - escala automática pelo tamanho físico (DPI) informado pelo EDID. */
static EFI_GUID uga_guid   = { 0x982c298b, 0xf4fa, 0x41cb, { 0xb8, 0x38, 0x77, 0xaa, 0x68, 0x8f, 0xb8, 0x39 } };
static EFI_GUID edidd_guid = { 0x1c0c34f6, 0xd380, 0x41fa, { 0xa0, 0x49, 0x8a, 0xd0, 0x6c, 0x1a, 0x66, 0xaa } };
static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
static EFI_UGA_DRAW_PROTOCOL *uga;
static Surface screen;
static int pw, ph;                    /* tamanho físico do modo */
static int rot, rot_pref;             /* rotação em uso (0..3 × 90° horário); preferência 0 = automática, 1..4 */
static int fb_format; static u32 *fb; static u32 fb_stride; static int fb_bpp = 4;
static u32 *shadow, *tmp;
static int mk_sh[3], mk_w[3];         /* máscara de bits: deslocamento e largura de R, G, B */
static int edid_w, edid_h, edid_mmw, edid_mmh; static bool edid_ok;
static char mode_pref[16];            /* "" automático, "max" ou "LxA" */
static int pending_rot = -1; static bool pending_mode;
static Buf vlog;
static void VL(const char *f, ...) {
    char t[256]; va_list ap; va_start(ap, f); int n = vfmt(t, sizeof t, f, ap); va_end(ap);
    buf_put(&vlog, t, (usize)MIN(n, (int)sizeof t - 1));
}

static bool parse_edid(const u8 *d, u32 n, int *w, int *h, int *mmw, int *mmh) {
    static const u8 hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (!d || n < 128 || memcmp(d, hdr, 8)) return false;
    for (int o = 54; o <= 108; o += 18) {
        if (!(d[o] | d[o + 1])) continue;                 /* não é temporização (nome, limites…) */
        *w = d[o + 2] | ((d[o + 4] >> 4) << 8); *h = d[o + 5] | ((d[o + 7] >> 4) << 8);
        *mmw = d[o + 12] | ((d[o + 14] >> 4) << 8); *mmh = d[o + 13] | ((d[o + 14] & 0xF) << 8);
        if (*mmw < 50 || *mmh < 30 || *mmw > 3000) { *mmw = d[21] * 10; *mmh = d[22] * 10; }
        return *w >= 320 && *h >= 200;
    }
    return false;
}

static void pick_output(void) {
    gop = 0; uga = 0; edid_ok = false;
    EFI_HANDLE *hs = 0; UINTN n = 0;
    if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &gop_guid, 0, &n, &hs))) n = 0;
    int best = -1, nact = 0; EFI_GRAPHICS_OUTPUT_PROTOCOL *con_gop = 0;
    VL("  GOP outputs: %d\r\n", (int)n);
    for (UINTN i = 0; i < n; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *g = 0;
        if (EFI_ERROR(BS->HandleProtocol(hs[i], &gop_guid, (void **)&g)) || !g || !g->Mode || !g->Mode->Info) continue;
        EFI_EDID_ACTIVE_PROTOCOL *ea = 0, *ed = 0;
        if (EFI_ERROR(BS->HandleProtocol(hs[i], &edid_guid, (void **)&ea))) ea = 0;
        if (EFI_ERROR(BS->HandleProtocol(hs[i], &edidd_guid, (void **)&ed))) ed = 0;
        int w = 0, h = 0, mw = 0, mh = 0;
        bool act = ea && parse_edid(ea->Edid, ea->SizeOfEdid, &w, &h, &mw, &mh);
        bool dis = !act && ed && parse_edid(ed->Edid, ed->SizeOfEdid, &w, &h, &mw, &mh);
        bool hasfb = g->Mode->FrameBufferBase && g->Mode->Info->PixelFormat != PixelBltOnly;
        bool con = hs[i] == ST->ConsoleOutHandle;
        int sc = (act ? 8 : 0) + (dis ? 2 : 0) + (hasfb ? 4 : 0) + (con ? 1 : 0);
        if (act) nact++;
        if (con) con_gop = g;
        VL("   #%d: %dx%d formato %d · %d modos · EDID %s%s%s\r\n", (int)i, (int)g->Mode->Info->HorizontalResolution,
           (int)g->Mode->Info->VerticalResolution, (int)g->Mode->Info->PixelFormat, (int)g->Mode->MaxMode,
           act ? "active" : dis ? "detected" : "none", hasfb ? " · framebuffer" : " · Blt only", con ? " · console" : "");
        if (act || dis) VL("       native panel %dx%d, %dx%d mm\r\n", w, h, mw, mh);
        if (sc > best) { best = sc; gop = g; edid_ok = act || dis; edid_w = w; edid_h = h; edid_mmw = mw; edid_mmh = mh; }
    }
    if (hs) BS->FreePool(hs);
    if (nact >= 2 && con_gop) { gop = con_gop; edid_ok = false; VL("  %d monitors: using the console output (picture on all of them)\r\n", nact); }
    if (!gop && EFI_ERROR(BS->HandleProtocol(ST->ConsoleOutHandle, &gop_guid, (void **)&gop))) gop = 0;
    if (!gop && EFI_ERROR(BS->LocateProtocol(&gop_guid, 0, (void **)&gop))) gop = 0;
    if (gop && (!gop->Mode || !gop->Mode->Info)) gop = 0;
    if (gop) return;
    if (EFI_ERROR(BS->HandleProtocol(ST->ConsoleOutHandle, &uga_guid, (void **)&uga)) &&
        EFI_ERROR(BS->LocateProtocol(&uga_guid, 0, (void **)&uga))) uga = 0;
    if (uga) VL("  no GOP: using UGA Draw (EFI 1.1)\r\n");
}

static bool mode_usable(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *i) {
    return i->HorizontalResolution >= 640 && i->VerticalResolution >= 400 && i->HorizontalResolution <= 8192 && i->VerticalResolution <= 8192;
}
static void pick_mode(void) {
    if (!gop) return;
    u32 cw = gop->Mode->Info->HorizontalResolution, ch = gop->Mode->Info->VerticalResolution, cur = gop->Mode->Mode;
    int ww = 0, wh = 0; bool want_max = str_eq(mode_pref, "max");
    if (mode_pref[0] && !want_max) {
        const char *s = mode_pref; while (*s >= '0' && *s <= '9') ww = ww * 10 + (*s++ - '0');
        if (*s == 'x' || *s == 'X') { s++; while (*s >= '0' && *s <= '9') wh = wh * 10 + (*s++ - '0'); }
    }
    bool manual = want_max || (ww && wh);
    /* automático: sem EDID, só sai do modo atual se ele for pequeno demais */
    if (!manual && !edid_ok && cw >= 1024 && ch >= 720) return;
    if (!manual && edid_ok && (int)cw == edid_w && (int)ch == edid_h) return;
    int best = -1; u64 bestv = 0;
    for (u32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info; UINTN sz;
        if (EFI_ERROR(gop->QueryMode(gop, m, &sz, &info))) continue;
        u64 w = info->HorizontalResolution, h = info->VerticalResolution, v = 0;
        if (mode_usable(info)) {
            if (ww && wh) v = (w == (u64)ww && h == (u64)wh) ? 1 : 0;
            else if (want_max) v = w * h;
            else if (edid_ok) {
                if (w == (u64)edid_w && h == (u64)edid_h) v = 1ull << 62;
                else if (w <= (u64)edid_w && h <= (u64)edid_h) v = (w * (u64)edid_h == h * (u64)edid_w ? 1ull << 40 : 0) + w * h;
            } else if (w <= 2560 && h <= 1600) v = w * h;
        }
        if (v > bestv) { bestv = v; best = (int)m; }
        BS->FreePool(info);
    }
    if (best < 0 || (u32)best == cur) return;
    EFI_STATUS st = gop->SetMode(gop, (u32)best);
    VL("  modo %d -> %dx%d%s\r\n", best, (int)gop->Mode->Info->HorizontalResolution, (int)gop->Mode->Info->VerticalResolution,
       EFI_ERROR(st) ? " (o firmware recusou)" : "");
    if (EFI_ERROR(st)) gop->SetMode(gop, cur);
}

static int lowbit(u32 m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int nbits(u32 m) { int c = 0; while (m) { c += (int)(m & 1); m >>= 1; } return c; }

/* (re)configura o quadro lógico a partir do modo atual; devolve false se não houver vídeo */
static bool video_setup(void) {
    u64 base = 0, size = 0; u32 stride = 0; int format = PixelBltOnly;
    if (gop) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *i = gop->Mode->Info;
        pw = (int)i->HorizontalResolution; ph = (int)i->VerticalResolution;
        format = i->PixelFormat; stride = i->PixelsPerScanLine;
        base = gop->Mode->FrameBufferBase; size = gop->Mode->FrameBufferSize;
        fb_bpp = 4;
        if (format == PixelBitMask) {
            EFI_PIXEL_BITMASK *m = &i->PixelInformation;
            u32 all = m->RedMask | m->GreenMask | m->BlueMask | m->ReservedMask; int hb = 0;
            for (u32 t = all; t; t >>= 1) hb++;
            fb_bpp = (hb + 7) / 8;
            u32 ms[3] = { m->RedMask, m->GreenMask, m->BlueMask };
            for (int k = 0; k < 3; k++) { mk_sh[k] = lowbit(ms[k]); mk_w[k] = MIN(16, nbits(ms[k])); }
            if (fb_bpp < 2 || !m->RedMask || !m->GreenMask || !m->BlueMask) format = PixelBltOnly;
        } else if (format > PixelBitMask) format = PixelBltOnly;
    } else if (uga) {
        UINT32 hr = 0, vr = 0, depth = 0, refresh = 0;
        if (EFI_ERROR(uga->GetMode(uga, &hr, &vr, &depth, &refresh))) return false;
        pw = (int)hr; ph = (int)vr;
    } else return false;
    if (pw < 320 || ph < 200 || pw > 8192 || ph > 8192) return false;
    /* framebuffer utilizável? */
    if (format != PixelBltOnly) {
        u64 need = (u64)stride * (u64)ph * (u64)fb_bpp;
        if (!base || stride < (u32)pw || (size && size < need)) format = PixelBltOnly;
#if __SIZEOF_POINTER__ == 4
        if (base + need > 0x100000000ull) format = PixelBltOnly;
#endif
    }
    fb_format = format;
    fb = format == PixelBltOnly ? 0 : (u32 *)(uintptr_t)base;
    fb_stride = stride;
    rot = rot_pref ? rot_pref - 1 : (ph > pw ? 1 : 0);    /* painel em retrato (tablet): deita a imagem */
    int lw = (rot & 1) ? ph : pw, lh = (rot & 1) ? pw : ph;
    if (screen.w != lw || screen.h != lh || !screen.px) {
        mem_free(screen.px); mem_free(shadow); mem_free(tmp);
        screen.w = lw; screen.h = lh; screen.stride = lw;
        screen.px = mem_alloc(sizeof(u32) * (usize)lw * lh);
        shadow = mem_alloc(sizeof(u32) * (usize)lw * lh);       /* cópia do que está na tela: só o que mudou é enviado */
        tmp = mem_alloc(sizeof(u32) * 64 * (usize)MAX(pw, ph));
        if (!screen.px || !tmp) return false;
        memset(screen.px, 0, sizeof(u32) * (usize)lw * lh);
    }
    if (shadow) for (usize k = 0, n = (usize)lw * lh; k < n; k++) shadow[k] = 0x5A5A5A5Au;
    static const char *fn[] = { "RGB", "BGR", "bit mask", "Blt only" };
    VL("  in use: %dx%d · %s%s%s · %d bytes/pixel · stride %d px · rotation %d°\r\n", pw, ph, uga ? "UGA " : "", fn[fb_format & 3],
       "", fb_bpp, (int)fb_stride, rot * 90);
    return true;
}

static void video_init(void) {
    VL("== Video\r\n");
    pick_output();
    pick_mode();
    if (!video_setup()) { gop = 0; uga = 0; }
}
Surface *pal_screen(void) { return &screen; }
float pal_display_dpi(void) {
    if (!edid_ok || edid_mmw <= 0 || edid_mmh <= 0) return 0;
    bool same = (pw >= ph) == (edid_w >= edid_h);
    float mm = (float)(same ? edid_mmw : edid_mmh);
    return (float)pw / (mm / 25.4f);
}

/* ------------------------------------------------------------ memória de vídeo em write-combining
 * O firmware deixa o framebuffer como "sem cache" (UC): cada pixel vira uma escrita isolada no
 * barramento, e 8 MB por quadro levam dezenas de ms. Com um MTRR em write-combining (o que Linux e
 * Windows fazem) as escritas são agrupadas e ficam 10–50× mais rápidas. */
static char wc_state[96] = "not attempted";
#if FOLIO_X86
static inline u64 rdmsr(u32 m) { u32 lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return ((u64)hi << 32) | lo; }
static inline void wrmsr(u32 m, u64 v) { __asm__ volatile("wrmsr" : : "c"(m), "a"((u32)v), "d"((u32)(v >> 32)) : "memory"); }
static inline void cpuid4(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) { __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0)); }
static inline uintptr_t rd_cr0(void) { uintptr_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline void wr_cr0(uintptr_t v) { __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline uintptr_t rd_cr4(void) { uintptr_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void wr_cr4(uintptr_t v) { __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory"); }
static inline uintptr_t rd_cr3(void) { uintptr_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void wr_cr3(uintptr_t v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }
#endif

#if defined(__x86_64__)
/* Write-combining pela tabela PAT (o método do Linux e do Windows): vale mesmo quando o firmware cobriu
 * a região de dispositivos com um MTRR "sem cache" — pela regra da Intel, PAT=WC sobre MTRR=UC dá WC,
 * enquanto um MTRR WC sobreposto a um MTRR UC fica UC (sem efeito). A entrada PA4 da PAT (bit PAT=1,
 * PCD=PWT=0), que o firmware não usa, vira WC, e só as páginas do framebuffer passam a apontar para ela;
 * páginas grandes que passam das bordas do framebuffer são divididas. */
#define PTE_ADDR 0x000FFFFFFFFFF000ull
static u64 *pt_pool[16]; static int pt_npool, pt_split; static bool pat_fail;
static void pat_mark(u64 *t, int level, u64 vbase, u64 s, u64 e) {
    u64 size = 1ull << (12 + 9 * (level - 1));
    u64 first = s > vbase ? (s - vbase) / size : 0;
    for (u64 i = first; i < 512; i++) {
        u64 eb = vbase + i * size, ee = eb + size;
        if (eb >= e) break;
        u64 v = t[i];
        if (!(v & 1)) continue;
        if (level == 1) { t[i] = (v & ~0x18ull) | 0x80; continue; }                    /* PTE: bit 7 = PAT */
        if (level <= 3 && (v & 0x80)) {                                                 /* página de 1 GB / 2 MB */
            if (eb >= s && ee <= e) { t[i] = (v & ~0x18ull) | 0x1000; continue; }       /* bit 12 = PAT */
            u64 *n = pt_npool ? pt_pool[--pt_npool] : 0;
            if (!n) { pat_fail = true; continue; }
            u64 cs = size >> 9, pa = v & PTE_ADDR & ~(size - 1), patb = v & 0x1000, attr = v & ~PTE_ADDR;
            for (int k = 0; k < 512; k++) {
                u64 ne = pa + (u64)k * cs;
                if (level == 2) ne |= (attr & ~0x80ull) | (patb ? 0x80 : 0);           /* vira PTEs de 4 KB */
                else ne |= attr | patb;                                                 /* vira páginas de 2 MB */
                n[k] = ne;
            }
            pt_split++;
            t[i] = (u64)(uintptr_t)n | (v & 0x27) | (v & (1ull << 63));
            pat_mark(n, level - 1, eb, s, e);
            continue;
        }
        pat_mark((u64 *)(uintptr_t)(v & PTE_ADDR), level - 1, eb, s, e);
    }
}
static bool fb_pat(u64 base, u64 size) {
    u32 a, b, c, d; cpuid4(1, &a, &b, &c, &d);
    if (!(d & (1u << 16))) return false;                         /* CPU sem PAT */
    if (!(rd_cr0() & 0x80000000u)) return false;                 /* sem paginação */
    for (pt_npool = 0; pt_npool < 16; pt_npool++) {
        EFI_PHYSICAL_ADDRESS p = 0;
        if (EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiBootServicesData, 1, &p))) break;
        pt_pool[pt_npool] = (u64 *)(uintptr_t)p;
    }
    int levels = (rd_cr4() & (1u << 12)) ? 5 : 4;
    u64 pat = rdmsr(0x277), npat = (pat & ~(0xFFull << 32)) | (0x01ull << 32);
    uintptr_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    uintptr_t cr0 = rd_cr0(), cr4 = rd_cr4();
    wr_cr0(((cr0 | (1u << 30)) & ~(uintptr_t)(1u << 29)) & ~(uintptr_t)(1u << 16));   /* sem cache; WP desligado (tabelas só-leitura) */
    __asm__ volatile("wbinvd" ::: "memory");
    wrmsr(0x277, npat);
    pat_mark((u64 *)(uintptr_t)(rd_cr3() & PTE_ADDR), levels, 0, base, base + size);
    __asm__ volatile("wbinvd" ::: "memory");
    if (cr4 & (1u << 7)) { wr_cr4(cr4 & ~(uintptr_t)(1u << 7)); wr_cr4(cr4); } else wr_cr3(rd_cr3());
    wr_cr0(cr0);
    __asm__ volatile("push %0; popf" : : "r"(flags) : "memory", "cc");
    for (int i = 0; i < pt_npool; i++) BS->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)pt_pool[i], 1);
    pt_npool = 0;
    fmt(wc_state, sizeof wc_state, "on via PAT (%d KB, %d large pages split%s)", (int)(size >> 10), pt_split,
        pat_fail ? ", out of memory for some" : "");
    return true;
}
#endif

/* cópia para a memória de vídeo em blocos de 16 bytes com escrita não temporal: rápida em WC e, se o
 * WC não puder ser ligado, 4–16× menos transações no barramento do que cópias byte a byte */
static void fb_copy(u32 *d, const u32 *s, usize n) {
#if defined(__x86_64__)
    while (n && ((uintptr_t)d & 15)) { *(volatile u32 *)d = *s; d++; s++; n--; }
    usize m = n / 4;
    if (m) __asm__ volatile(
        "1:\n\tmovdqu (%1), %%xmm0\n\tmovntdq %%xmm0, (%0)\n\tadd $16, %0\n\tadd $16, %1\n\tdec %2\n\tjnz 1b"
        : "+r"(d), "+r"(s), "+r"(m) : : "xmm0", "memory", "cc");
    n &= 3;
    while (n--) { *(volatile u32 *)d = *s; d++; s++; }
#elif defined(__i386__)
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
#else
    while (n--) { *(volatile u32 *)d = *s; d++; s++; }
#endif
}

static bool wc_wanted; static u64 wc_b, wc_s;
static void wc_apply(void);
void pal_video_fast(bool on) {
    wc_wanted = on;
    if (!on) { str_copy(wc_state, "off in settings", sizeof wc_state); return; }
    wc_apply();
}
/* aplicado de novo se uma troca de modo mover/aumentar o framebuffer */
static void wc_apply(void) {
    if (!wc_wanted || !fb || !gop || fb_format == PixelBltOnly) return;
    {
        u64 b0 = gop->Mode->FrameBufferBase, s0 = MAX((u64)gop->Mode->FrameBufferSize, (u64)fb_stride * (u64)ph * (u64)fb_bpp);
        if (wc_s && b0 >= wc_b && b0 + s0 <= wc_b + wc_s) return;
        wc_b = b0; wc_s = s0;
    }
#if !FOLIO_X86
    str_copy(wc_state, "not applicable on this architecture (the firmware maps the framebuffer)", sizeof wc_state);
#else
    u16 cs; __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    if (cs & 3) { str_copy(wc_state, "not in ring 0 (emulator): unchanged", sizeof wc_state); return; }
    u32 a, b, c, d;
    cpuid4(1, &a, &b, &c, &d);
    if (c & (1u << 31)) { str_copy(wc_state, "virtual machine: unchanged", sizeof wc_state); return; }
#if defined(__x86_64__)
    {
        u64 fbase = gop->Mode->FrameBufferBase, fsize = gop->Mode->FrameBufferSize, fneed = (u64)fb_stride * (u64)ph * (u64)fb_bpp;
        if (fsize < fneed) fsize = fneed;
        u64 s0 = fbase & ~0xFFFull, s1 = (fbase + fsize + 0xFFF) & ~0xFFFull;
        if (fb_pat(s0, s1 - s0)) return;
    }
#endif
    if (!(d & (1u << 12))) { str_copy(wc_state, "CPU without MTRR", sizeof wc_state); return; }
    u64 cap = rdmsr(0xFE);
    if (!(cap & (1u << 10))) { str_copy(wc_state, "CPU without write-combining", sizeof wc_state); return; }
    int vcnt = (int)(cap & 0xFF);
    u32 phys = 36;
    cpuid4(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000008u) { cpuid4(0x80000008u, &a, &b, &c, &d); phys = a & 0xFF; if (phys < 32 || phys > 52) phys = 36; }
    u64 pmask = ((u64)1 << phys) - 1;
    u64 base = gop->Mode->FrameBufferBase, size = gop->Mode->FrameBufferSize;
    u64 need = (u64)fb_stride * (u64)ph * (u64)fb_bpp;
    if (size < need) size = need;
    /* já está em WC? */
    for (int i = 0; i < vcnt; i++) {
        u64 pb = rdmsr(0x200 + 2 * (u32)i), pm = rdmsr(0x201 + 2 * (u32)i);
        if ((pm & 0x800) && (pb & 0xFF) == 1 && ((base ^ pb) & pm & pmask & ~0xFFFull) == 0) { str_copy(wc_state, "already write-combining", sizeof wc_state); return; }
    }
    /* blocos alinhados (potência de 2) cobrindo o framebuffer */
    u64 blk_b[4], blk_s[4]; int nb = 0;
    u64 pos = base, end = base + size;
    while (pos < end && nb < 4) {
        u64 s = 4096;
        while ((pos & (s * 2 - 1)) == 0 && s * 2 <= ((u64)1 << 34) && pos + s < end) s *= 2;   /* o último bloco pode passar um pouco do fim (ainda é VRAM) */
        blk_b[nb] = pos; blk_s[nb] = s; nb++;
        pos += s;
    }
    if (pos < end) { str_copy(wc_state, "misaligned framebuffer: unchanged", sizeof wc_state); return; }
    int freei[4], nf = 0;
    for (int i = 0; i < vcnt && nf < nb; i++) if (!(rdmsr(0x201 + 2 * (u32)i) & 0x800)) freei[nf++] = i;
    if (nf < nb) { str_copy(wc_state, "sem MTRR livre", sizeof wc_state); return; }
    /* sequência do manual Intel (SDM 11.11.7.2) */
    uintptr_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    uintptr_t cr0 = rd_cr0(), cr4 = rd_cr4();
    wr_cr0((cr0 | (1u << 30)) & ~(uintptr_t)(1u << 29));
    __asm__ volatile("wbinvd" ::: "memory");
    if (cr4 & (1u << 7)) wr_cr4(cr4 & ~(uintptr_t)(1u << 7)); else wr_cr3(rd_cr3());
    u64 def = rdmsr(0x2FF);
    wrmsr(0x2FF, def & ~(1ull << 11));
    for (int k = 0; k < nb; k++) {
        wrmsr(0x200 + 2 * (u32)freei[k], (blk_b[k] & pmask & ~0xFFFull) | 1);                 /* tipo 1 = WC */
        wrmsr(0x201 + 2 * (u32)freei[k], (~(blk_s[k] - 1) & pmask & ~0xFFFull) | 0x800);
    }
    __asm__ volatile("wbinvd" ::: "memory");
    wr_cr3(rd_cr3());
    wrmsr(0x2FF, def);
    wr_cr0(cr0);
    wr_cr4(cr4);
    __asm__ volatile("push %0; popf" : : "r"(flags) : "memory", "cc");
    fmt(wc_state, sizeof wc_state, "on (%d MTRR, %d KB)", nb, (int)((pos - base) >> 10));
#endif
}

/* envia ao vídeo apenas os trechos de cada linha que mudaram desde o último quadro */
static u64 present_us_tot, present_n, present_px;
static inline void put_span(int y, int x0, int x1) {          /* caminho rápido: BGR de 32 bits, sem rotação */
    u32 *s = screen.px + (usize)y * screen.stride, *sh = shadow + (usize)y * screen.w;
    memcpy(sh + x0, s + x0, (usize)(x1 - x0) * 4);
    fb_copy(fb + (usize)y * fb_stride + x0, s + x0, (usize)(x1 - x0));
}
static inline u32 conv_px(u32 p) {                             /* 0x00RRGGBB -> formato do framebuffer */
    if (fb_format == PixelRedGreenBlueReserved8BitPerColor) return (p & 0xFF00) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
    u32 c[3] = { (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF }, v = 0;
    for (int k = 0; k < 3; k++) {
        int w = mk_w[k];
        u32 x = w >= 8 ? (c[k] << (w - 8)) | (w > 8 ? c[k] >> (16 - w) : 0) : c[k] >> (8 - w);
        v |= x << mk_sh[k];
    }
    return v;
}
static void fb_write_row(int X, int Y, u32 *row, int n) {
    u8 *dst = (u8 *)fb + ((usize)Y * fb_stride + (usize)X) * (usize)fb_bpp;
    if (fb_format != PixelBlueGreenRedReserved8BitPerColor) for (int i = 0; i < n; i++) row[i] = conv_px(row[i]);
    if (fb_bpp == 4) fb_copy((u32 *)dst, row, (usize)n);
    else if (fb_bpp == 2) { volatile u16 *d = (volatile u16 *)dst; for (int i = 0; i < n; i++) d[i] = (u16)row[i]; }
    else { volatile u8 *d = dst; for (int i = 0; i < n; i++) { d[3 * i] = (u8)row[i]; d[3 * i + 1] = (u8)(row[i] >> 8); d[3 * i + 2] = (u8)(row[i] >> 16); } }
}
static void blt_out(u32 *buf, int x, int y, int w, int h) {
    if (gop) gop->Blt(gop, buf, EfiBltBufferToVideo, 0, 0, (UINTN)x, (UINTN)y, (UINTN)w, (UINTN)h, (UINTN)w * 4);
    else if (uga) uga->Blt(uga, buf, EfiUgaBltBufferToVideo, 0, 0, (UINTN)x, (UINTN)y, (UINTN)w, (UINTN)h, (UINTN)w * 4);
}
/* envia um retângulo lógico (no máximo 64 linhas) girando e convertendo o formato conforme preciso */
static void emit_rect(int lx0, int ly0, int lx1, int ly1) {
    if (lx1 <= lx0 || ly1 <= ly0) return;
    if (shadow) for (int y = ly0; y < ly1; y++) memcpy(shadow + (usize)y * screen.w + lx0, screen.px + (usize)y * screen.stride + lx0, (usize)(lx1 - lx0) * 4);
    int X0, Y0, w, h;
    switch (rot) {
    case 0:  X0 = lx0;      Y0 = ly0;      w = lx1 - lx0; h = ly1 - ly0; break;
    case 1:  X0 = pw - ly1; Y0 = lx0;      w = ly1 - ly0; h = lx1 - lx0; break;
    case 2:  X0 = pw - lx1; Y0 = ph - ly1; w = lx1 - lx0; h = ly1 - ly0; break;
    default: X0 = ly0;      Y0 = ph - lx1; w = ly1 - ly0; h = lx1 - lx0; break;
    }
    bool direct = fb_format != PixelBltOnly;
    const u32 *S = screen.px; usize ss = (usize)screen.stride;
    for (int j = 0; j < h; j++) {
        u32 *row = direct ? tmp : tmp + (usize)j * (usize)w;
        int Y = Y0 + j;
        switch (rot) {
        case 0: memcpy(row, S + (usize)Y * ss + X0, (usize)w * 4); break;
        case 1: for (int i = 0; i < w; i++) row[i] = S[(usize)(pw - 1 - (X0 + i)) * ss + (usize)Y]; break;
        case 2: { const u32 *src = S + (usize)(ph - 1 - Y) * ss; for (int i = 0; i < w; i++) row[i] = src[pw - 1 - (X0 + i)]; } break;
        default: for (int i = 0; i < w; i++) row[i] = S[(usize)(X0 + i) * ss + (usize)(ph - 1 - Y)]; break;
        }
        if (direct) fb_write_row(X0, Y, row, w);
    }
    if (!direct) blt_out(tmp, X0, Y0, w, h);
    present_px += (u64)w * (u64)h;
}
void pal_present(Rect r) {
    r = rect_clip(r, rect(0, 0, screen.w, screen.h));
    if (r.w <= 0 || r.h <= 0 || (!gop && !uga) || !tmp) return;
    u64 t0 = rdtsc();
    bool fast = rot == 0 && fb_format == PixelBlueGreenRedReserved8BitPerColor && shadow;
    if (!shadow) {
        for (int y = r.y; y < r.y + r.h; y += 64) emit_rect(r.x, y, r.x + r.w, MIN(y + 64, r.y + r.h));
    } else {
        /* linhas sujas consecutivas viram um retângulo (até 64 linhas) */
        int by0 = -1, bx0 = 0, bx1 = 0;
        for (int y = r.y; y <= r.y + r.h; y++) {
            int x0 = -1, x1 = -1;
            if (y < r.y + r.h) {
                const u32 *s = screen.px + (usize)y * screen.stride, *sh = shadow + (usize)y * screen.w;
                int x = r.x, xe = r.x + r.w;
                const u64 *s8 = (const u64 *)(s + x), *h8 = (const u64 *)(sh + x);
                int n8 = (xe - x) / 2, i = 0;
                while (i < n8 && s8[i] == h8[i]) i++;
                if (i < n8 || (((xe - x) & 1) && s[xe - 1] != sh[xe - 1])) {
                    x0 = x + i * 2;
                    while (x0 < xe && s[x0] == sh[x0]) x0++;
                    int j = xe;
                    while (j > x0 && s[j - 1] == sh[j - 1]) j--;
                    x1 = j;
                    if (x0 >= x1) x0 = x1 = -1;
                }
            }
            if (fast) {
                if (x0 >= 0) { put_span(y, x0, x1); present_px += (u64)(x1 - x0); }
                continue;
            }
            if (x0 >= 0 && (by0 < 0 || y - by0 < 64)) {
                if (by0 < 0) { by0 = y; bx0 = x0; bx1 = x1; } else { bx0 = MIN(bx0, x0); bx1 = MAX(bx1, x1); }
                continue;
            }
            if (by0 >= 0) { emit_rect(bx0, by0, bx1, y); by0 = -1; }
            if (x0 >= 0) { by0 = y; bx0 = x0; bx1 = x1; }
        }
    }
#if defined(__x86_64__)
    __asm__ volatile("sfence" ::: "memory");
#endif
    present_us_tot += (rdtsc() - t0) * 1000 / tsc_per_ms; present_n++;
}
void pal_diag_video(Buf *o) {
    if (vlog.s) buf_put(o, vlog.s, vlog.len);
    float dpi = pal_display_dpi();
    buf_printf(o, "  logical screen %dx%d · DPI %d · write-combining: %s · video upload: %d frames, average %d µs, %d Mpx sent\r\n",
               screen.w, screen.h, (int)dpi, wc_state, (int)present_n, present_n ? (int)(present_us_tot / present_n) : 0, (int)(present_px >> 20));
}

/* ---- mudanças em tempo de execução: aplicadas entre quadros (o app nunca desenha num quadro trocado) */
static void apply_pending(void);
/* antes do primeiro quadro (ajustes lidos no início) aplica na hora; depois, entre dois quadros */
void pal_set_rotation(int pref) { pref = CLAMP(pref, 0, 4); if (pref != rot_pref) pending_rot = pref; if (!present_n) apply_pending(); }
int  pal_rotation(void) { return rot; }
void pal_set_video_mode(const char *pref) { str_copy(mode_pref, pref ? pref : "", sizeof mode_pref); pending_mode = true; if (!present_n) apply_pending(); }
void pal_video_mode(int *w, int *h) { *w = pw; *h = ph; }
/* modos utilizáveis, do maior para o menor, sem repetições */
int pal_video_modes(int *ws, int *hs, int max) {
    int n = 0;
    if (!gop) return 0;
    for (u32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info; UINTN sz;
        if (EFI_ERROR(gop->QueryMode(gop, m, &sz, &info))) continue;
        int w = (int)info->HorizontalResolution, h = (int)info->VerticalResolution;
        bool ok = mode_usable(info);
        BS->FreePool(info);
        if (!ok) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) if (ws[k] == w && hs[k] == h) dup = true;
        if (dup || n >= max) continue;
        int k = n++;
        while (k > 0 && (u64)ws[k - 1] * hs[k - 1] < (u64)w * h) { ws[k] = ws[k - 1]; hs[k] = hs[k - 1]; k--; }
        ws[k] = w; hs[k] = h;
    }
    return n;
}

/* ------------------------------------------------------------ eventos */
static Event q[512]; static int qh, qt;
static void push(Event *e) { int n = (qt + 1) % 512; if (n == qh) return; q[qt] = *e; qt = n; }

static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *kbx;
static EFI_GUID dp_guid = { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

/* Cada dispositivo apontador é lido separadamente, com estado de botões próprio.
 * O "ConSplitter" do firmware (no ConsoleInHandle) agrega todos os dispositivos; lê-lo junto com
 * os dispositivos reais faz um roubar o movimento do outro e os botões "piscarem". Por isso ele só
 * é usado quando não existe nenhum dispositivo real. */
typedef struct {
    EFI_HANDLE h; bool virt;
    const char *own;               /* driver próprio do FolioOS (USB HID), não do firmware */
    EFI_SIMPLE_POINTER_PROTOCOL *sp; EFI_ABSOLUTE_POINTER_PROTOCOL *ap;
    u32 btn;
    bool have_last, touch, touch_seen; u64 lx, ly, t_down, t_last; float travel; int hover_moves; bool abs_now;
    u32 events; i32 last_dx, last_dy; u32 last_raw_btn;
    i32 z_last, z_raw; u64 z_since, z_t; u32 z_used, z_dropped, z_n, z_xy, z_blk_rate, z_blk_xy; int z_block;   /* roda (eixo Z) */
} Ptr;
static Ptr ptrs[16]; static int nptrs;
static float mx, my, speed = 1.0f;
static int touch_mode;              /* 0 automático, 1 touchpad (relativo), 2 tela de toque/tablet (absoluto) */
static u32 btn_state;
static u64 last_scan;
static int last_mods;
static EFI_EVENT timer_ev;

void pal_set_pointer_speed(float s) { speed = s; }
void pal_set_touch_mode(int m) { touch_mode = m; }
int pal_mods(void) { return last_mods; }

int usbhid_scan(void **out_ifc, bool *out_abs, const char **out_kind, int cap);
void *ps2mouse_start(void); const char *ps2mouse_info(void);
static bool known_ptr(void *ifc) { for (int i = 0; i < nptrs; i++) if ((void *)ptrs[i].sp == ifc || (void *)ptrs[i].ap == ifc) return true; return false; }
static void scan_pointers(void) {
    for (int kind = 0; kind < 2; kind++) {
        EFI_HANDLE *hs; UINTN n;
        if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, kind ? &aptr_guid : &sptr_guid, 0, &n, &hs))) continue;
        for (UINTN i = 0; i < n && nptrs < 16; i++) {
            void *ifc = 0, *other, *dp;
            if (EFI_ERROR(BS->HandleProtocol(hs[i], kind ? &aptr_guid : &sptr_guid, &ifc)) || !ifc || known_ptr(ifc)) continue;
            /* o mesmo dispositivo com os dois protocolos: usa só o relativo, senão o movimento dobra */
            if (kind == 1 && !EFI_ERROR(BS->HandleProtocol(hs[i], &sptr_guid, &other))) continue;
            Ptr *p = &ptrs[nptrs++]; memset(p, 0, sizeof *p);
            p->h = hs[i];
            p->virt = hs[i] == ST->ConsoleInHandle || EFI_ERROR(BS->HandleProtocol(hs[i], &dp_guid, &dp));
            if (kind) { p->ap = ifc; p->ap->Reset(p->ap, 0); } else { p->sp = ifc; p->sp->Reset(p->sp, 0); }
        }
        BS->FreePool(hs);
    }
    /* mouses, tablets e telas de toque USB que o firmware não transformou em ponteiro: driver próprio */
    void *ifc[8]; bool abs_[8]; const char *kind[8];
    int n = usbhid_scan(ifc, abs_, kind, 8);
    for (int i = 0; i < n && nptrs < 16; i++) {
        Ptr *p = &ptrs[nptrs++]; memset(p, 0, sizeof *p);
        p->own = kind[i];
        if (abs_[i]) p->ap = ifc[i]; else p->sp = ifc[i];
    }
    /* nenhum ponteiro de verdade: tenta o mouse/touchpad PS/2 (i8042) com o driver próprio, uma vez */
    static bool ps2_tried;
    bool real = false;
    for (int i = 0; i < nptrs; i++) if (!ptrs[i].virt || ptrs[i].own) real = true;
    if (!real && !ps2_tried && nptrs < 16) {
        ps2_tried = true;
        void *sp = ps2mouse_start();
        if (sp) { Ptr *p = &ptrs[nptrs++]; memset(p, 0, sizeof *p); p->own = ps2mouse_info(); p->sp = sp; }
    }
}

/* liga todos os drivers do firmware (em "inicialização rápida" o mouse USB e o touchpad
 * muitas vezes ficam desconectados até alguém pedir) */
static void connect_all(void) {
    if (!BS->ConnectController) return;
    EFI_HANDLE *hs; UINTN n;
    if (EFI_ERROR(BS->LocateHandleBuffer(AllHandles, 0, 0, &n, &hs))) return;
    for (UINTN i = 0; i < n; i++) BS->ConnectController(hs[i], 0, 0, 1);
    BS->FreePool(hs);
}

static void input_init(void) {
    if (EFI_ERROR(BS->HandleProtocol(ST->ConsoleInHandle, &tiex_guid, (void **)&kbx))) kbx = 0;
    scan_pointers();
    mx = screen.w / 2.0f; my = screen.h / 2.0f;
    BS->CreateEvent(EVT_TIMER, 0, 0, 0, &timer_ev);
}

/* toque/caneta: coordenadas do painel físico -> tela lógica (girada) */
static void abs_to_screen(float u, float v) {
    switch (rot) {
    case 1:  mx = v * screen.w;       my = (1 - u) * screen.h; break;
    case 2:  mx = (1 - u) * screen.w; my = (1 - v) * screen.h; break;
    case 3:  mx = (1 - v) * screen.w; my = u * screen.h;       break;
    default: mx = u * screen.w;       my = v * screen.h;       break;
    }
}
static void emit_move(void) {
    mx = CLAMP(mx, 0.0f, (float)(screen.w - 1)); my = CLAMP(my, 0.0f, (float)(screen.h - 1));
    Event e = { 0 }; e.type = EV_MOVE; e.x = (int)mx; e.y = (int)my; push(&e);
}
static void emit_btn(int b, bool down) { Event e = { 0 }; e.type = down ? EV_DOWN : EV_UP; e.button = b; e.x = (int)mx; e.y = (int)my; push(&e); }

static float accel(float mag) { return mag > 12 ? 1.8f : mag > 6 ? 1.45f : mag > 2.5f ? 1.15f : 1.0f; }

static void poll_simple(Ptr *p) {
    EFI_SIMPLE_POINTER_STATE s;
    if (p->sp->GetState(p->sp, &s) != EFI_SUCCESS) return;
    p->events++; p->last_dx = s.RelativeMovementX; p->last_dy = s.RelativeMovementY;
    /* resolução em contagens/mm; valores absurdos são ignorados (8 = 1 contagem por pixel) */
    u64 rx = p->sp->Mode->ResolutionX, ry = p->sp->Mode->ResolutionY;
    float kx = 8.0f / (float)(rx >= 2 && rx <= 64 ? rx : 8), ky = 8.0f / (float)(ry >= 2 && ry <= 64 ? ry : 8);
    kx = CLAMP(kx, 0.25f, 4.0f); ky = CLAMP(ky, 0.25f, 4.0f);
    float dx = (float)s.RelativeMovementX * kx * speed, dy = (float)s.RelativeMovementY * ky * speed;
    float a = accel(f_abs(dx) + f_abs(dy)); dx *= a; dy *= a;
    if (dx != 0 || dy != 0) { mx += dx; my += dy; emit_move(); }
    /* Roda: defesa contra Z falso, a causa PROVÁVEL (não confirmada no hardware do usuário) da
     * "página rolando sozinha". Hipótese: drivers USB de firmware (EDK2) põem o mouse no protocolo
     * "boot", em que só 3 bytes do relatório têm significado, e ainda assim leem o 4º como roda;
     * mouses que põem outra coisa ali geram Z contínuo. ResolutionZ não é critério: firmwares
     * informam 0 também para mouses com roda de verdade. Regras (por dispositivo):
     *  - |Z| > 24 numa leitura: lixo (a roda real dá poucos dentes entre duas leituras);
     *  - o mesmo Z em relatórios densos: 16 em 250 ms ou 40 em 1,5 s (relatório periódico a
     *    ~125 Hz). Uma roda real girada, mesmo sem parar, raramente passa de ~25 dentes/s, e
     *    rolar devagar e contínuo (um dente a cada 200 ms) nunca bloqueia;
     *  - o mesmo Z em 8 relatórios seguidos que também trazem movimento: o byte não é roda.
     * Bloqueio por densidade sai depois de 0,6 s sem relatório; por movimento, no primeiro
     * relatório com Z = 0 (mouse de verdade manda um ao se mexer; o defeituoso, nunca). */
    {
        int z = s.RelativeMovementZ;
        u64 now = pal_ms();
        bool same = z && z == p->z_last && now - p->z_t < 300;
        if (!same) { p->z_since = now; p->z_n = 0; p->z_xy = 0; }
        p->z_n++;
        if (same && (s.RelativeMovementX || s.RelativeMovementY)) p->z_xy++;
        if (p->z_block == 1 && now - p->z_t > 600) p->z_block = 0;
        p->z_last = z; p->z_t = now;
        if (z) p->z_raw = z;
        if (!z) { if (p->z_block == 2) p->z_block = 0; }
        else {
            u64 span = now - p->z_since;
            if (!p->z_block && ((p->z_n >= 16 && span <= 250) || (p->z_n >= 40 && span <= 1500))) { p->z_block = 1; p->z_blk_rate++; }
            if (p->z_xy >= 8 && p->z_block != 2) { p->z_block = 2; p->z_blk_xy++; }
            if (p->z_block || iabs(z) > 24) p->z_dropped++;
            else {
                p->z_used++;
                int k = CLAMP(iabs(z), 1, 6);
                for (int i = 0; i < k; i++) { Event e = { 0 }; e.type = EV_WHEEL; e.wheel = z > 0 ? 1 : -1; push(&e); }
            }
        }
    }
    p->btn = (s.LeftButton ? 1u : 0u) | (s.RightButton ? 2u : 0u);
}

static void poll_absolute(Ptr *p, u64 now) {
    EFI_ABSOLUTE_POINTER_STATE s;
    if (p->ap->GetState(p->ap, &s) != EFI_SUCCESS) return;
    p->events++; p->last_raw_btn = s.ActiveButtons;
    EFI_ABSOLUTE_POINTER_MODE *m = p->ap->Mode;
    u64 spanx = m->AbsoluteMaxX > m->AbsoluteMinX ? m->AbsoluteMaxX - m->AbsoluteMinX : 1;
    u64 spany = m->AbsoluteMaxY > m->AbsoluteMinY ? m->AbsoluteMaxY - m->AbsoluteMinY : 1;
    bool touch = (s.ActiveButtons & 1) != 0, alt = (s.ActiveButtons & 2) != 0;
    if (touch) p->touch_seen = true;
    bool moved = p->have_last && (s.CurrentX != p->lx || s.CurrentY != p->ly);
    /* movimento sem toque (e sem ser a amostra de quando o dedo sai) = caneta/tablet/máquina virtual */
    if (moved && !touch && !p->touch) p->hover_moves++;
    p->abs_now = touch_mode == 2 || (touch_mode == 0 && p->hover_moves >= 12);
    if (p->abs_now) {
        if (moved || !p->have_last) {
            u64 cx = CLAMP(s.CurrentX, m->AbsoluteMinX, m->AbsoluteMaxX), cy = CLAMP(s.CurrentY, m->AbsoluteMinY, m->AbsoluteMaxY);
            abs_to_screen((float)(cx - m->AbsoluteMinX) / (float)spanx, (float)(cy - m->AbsoluteMinY) / (float)spany);
            emit_move();
        }
        p->btn = (touch ? 1u : 0u) | (alt ? 2u : 0u);
    } else {
        /* touchpad: movimento relativo; um novo traço começa ao encostar o dedo ou após uma pausa */
        bool active = p->touch_seen ? touch : true;
        bool new_stroke = !p->have_last || (touch && !p->touch) || now - p->t_last > 150;
        if (active && moved && !new_stroke) {
            float kx = 1.5f * speed * (float)screen.w / (float)spanx, ky = 1.5f * speed * (float)screen.h / (float)spany;
            float dx = ((float)s.CurrentX - (float)p->lx) * kx, dy = ((float)s.CurrentY - (float)p->ly) * ky;
            float a = accel(f_abs(dx) + f_abs(dy)); dx *= a; dy *= a;
            mx += dx; my += dy; p->travel += f_abs(dx) + f_abs(dy); emit_move();
        }
        if (touch && !p->touch) { p->t_down = now; p->travel = 0; }
        /* toque rápido = clique */
        if (!touch && p->touch && now - p->t_down < 220 && p->travel < 8) { emit_btn(0, true); emit_btn(0, false); }
        p->btn = alt ? 1u : 0u;      /* botão físico do touchpad */
    }
    p->touch = touch;
    p->t_last = now;
    p->have_last = true; p->lx = s.CurrentX; p->ly = s.CurrentY;
}

static void poll_pointers(void) {
    u64 now = pal_ms();
    if (now - last_scan > 2000) { last_scan = now; scan_pointers(); }     /* mouse ligado depois do boot */
    bool real = false;
    for (int i = 0; i < nptrs; i++) if (!ptrs[i].virt) real = true;
    u32 b = 0;
    for (int i = 0; i < nptrs; i++) {
        Ptr *p = &ptrs[i];
        if (real && p->virt) continue;
        if (p->sp) poll_simple(p); else poll_absolute(p, now);
        b |= p->btn;
    }
    if ((b & 1) != (btn_state & 1)) emit_btn(0, (b & 1) != 0);
    if ((b & 2) != (btn_state & 2)) emit_btn(1, (b & 2) != 0);
    /* o protocolo Simple Pointer do UEFI só conhece os botões esquerdo e direito; os dois juntos
     * fazem as vezes do botão do meio (como o "Emulate3Buttons" do X11) */
    bool mid = (b & 3) == 3, was = (btn_state & 4) != 0;
    if (mid && !was) { emit_btn(2, true); b |= 4; }
    else if (was && (b & 3)) b |= 4;           /* segue "no meio" até soltar os dois */
    else if (was) emit_btn(2, false);
    btn_state = b;
}

void pal_diag_input(Buf *o) {
    buf_printf(o, "== Pointers (%d found; touch mode %s; speed %d%%)\r\n", nptrs,
               touch_mode == 1 ? "touchpad" : touch_mode == 2 ? "absolute" : "automatic", (int)(speed * 100));
    bool real = false; for (int i = 0; i < nptrs; i++) if (!ptrs[i].virt) real = true;
    for (int i = 0; i < nptrs; i++) {
        Ptr *p = &ptrs[i];
        const char *use = real && p->virt ? "ignorado (agregador do firmware)" : "em uso";
        if (p->sp) buf_printf(o, "  %d. relative (SimplePointer)%s · resolution %d/%d/%d · buttons L%d R%d · %d reads · last dx=%d dy=%d · "
                                 "wheel: %d reads used, %d discarded, last raw Z %d, blocked by density %d, by motion %d%s · %s\r\n",
                               i + 1, p->own ? " [FolioOS USB HID driver]" : p->virt ? " [virtual]" : "", (int)p->sp->Mode->ResolutionX, (int)p->sp->Mode->ResolutionY, (int)p->sp->Mode->ResolutionZ,
                               (int)p->sp->Mode->LeftButton, (int)p->sp->Mode->RightButton, (int)p->events, (int)p->last_dx, (int)p->last_dy,
                               (int)p->z_used, (int)p->z_dropped, (int)p->z_raw, (int)p->z_blk_rate, (int)p->z_blk_xy,
                               p->z_block ? " (blocked now)" : "", use);
        else {
            EFI_ABSOLUTE_POINTER_MODE *m = p->ap->Mode;
            buf_printf(o, "  %d. absolute (AbsolutePointer)%s · X %d..%d · Y %d..%d · attributes %x · %d reads · touch seen %d · hover moves %d · buttons %x · treated as %s · %s\r\n",
                       i + 1, p->own ? " [FolioOS USB HID driver]" : p->virt ? " [virtual]" : "", (int)m->AbsoluteMinX, (int)m->AbsoluteMaxX, (int)m->AbsoluteMinY, (int)m->AbsoluteMaxY, (int)m->Attributes,
                       (int)p->events, (int)p->touch_seen, p->hover_moves, (int)p->last_raw_btn, p->abs_now ? "screen/tablet" : "touchpad", use);
        }
    }
    if (!nptrs) buf_puts(o, "  none: this PC's firmware offers no mouse/touchpad driver (only the keyboard works)\r\n");
}

static void key_event(u16 scan, u16 uc, u32 shift) {
    int mods = 0;
    if (shift & EFI_SHIFT_STATE_VALID) {
        if (shift & (EFI_LEFT_SHIFT | EFI_RIGHT_SHIFT)) mods |= MOD_SHIFT;
        if (shift & (EFI_LEFT_CONTROL | EFI_RIGHT_CONTROL)) mods |= MOD_CTRL;
        if (shift & EFI_LEFT_ALT) mods |= MOD_ALT;
        if (shift & EFI_RIGHT_ALT) mods |= MOD_ALTGR;
    }
    Event e = { 0 }; e.type = EV_KEY;
    static const int scmap[] = { 0, KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_HOME, KEY_END, KEY_INSERT, KEY_DELETE, KEY_PGUP, KEY_PGDN,
        KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_ESC };
    if (scan && scan < (u16)COUNT(scmap)) { e.key = scmap[scan]; }
    else if (uc == 8) e.key = KEY_BACKSPACE;                 /* Ctrl+Backspace (apagar palavra) vale mais que Ctrl+H */
    else if ((uc == 9 || uc == 10) && (mods & MOD_CTRL)) e.ch = (u32)('a' + uc - 1);   /* Ctrl+I itálico, Ctrl+J */
    else if (uc == 9) e.key = KEY_TAB;
    else if (uc == 13 || uc == 10) e.key = KEY_ENTER;
    else if (uc == 27) e.key = KEY_ESC;
    else if (uc >= 1 && uc <= 26) { e.ch = (u32)('a' + uc - 1); mods |= MOD_CTRL; }
    else if (uc) {
        e.ch = uc;
        if (mods & MOD_CTRL) { if (e.ch >= 'A' && e.ch <= 'Z') e.ch += 32; }
        else e.us_raw = uc < 128;
        if (!(mods & MOD_CTRL)) mods &= ~MOD_SHIFT;   /* o caractere já vem com shift aplicado */
        if ((shift & EFI_SHIFT_STATE_VALID) && (shift & (EFI_LEFT_SHIFT | EFI_RIGHT_SHIFT)) && (mods & MOD_CTRL)) mods |= MOD_SHIFT;
    }
    else return;
    if ((mods & MOD_ALTGR) && (mods & MOD_CTRL) && e.ch) mods &= ~MOD_CTRL;
    e.mods = mods;
    last_mods = mods;
    e.x = (int)mx; e.y = (int)my;
    push(&e);
}

static void poll_keys(void) {
    for (int guard = 0; guard < 32; guard++) {
        if (kbx) {
            EFI_KEY_DATA k;
            if (kbx->ReadKeyStrokeEx(kbx, &k) != EFI_SUCCESS) break;
            key_event(k.Key.ScanCode, k.Key.UnicodeChar, k.KeyState.KeyShiftState);
        } else {
            EFI_INPUT_KEY k;
            if (ST->ConIn->ReadKeyStroke(ST->ConIn, &k) != EFI_SUCCESS) break;
            key_event(k.ScanCode, k.UnicodeChar, 0);
        }
    }
}

/* rotação/resolução pedidas nos ajustes: aplicadas aqui, entre um quadro e outro */
static void apply_pending(void) {
    if (pending_rot < 0 && !pending_mode) return;
    if (pending_rot >= 0) { rot_pref = pending_rot; pending_rot = -1; }
    if (pending_mode) { pending_mode = false; pick_mode(); }
    if (!video_setup()) return;
    wc_apply();
    mx = CLAMP(mx, 0.0f, (float)(screen.w - 1)); my = CLAMP(my, 0.0f, (float)(screen.h - 1));
    emit_move();                               /* acorda o laço principal para redesenhar */
}
bool pal_poll(Event *e) {
    apply_pending();
    if (qh == qt) { poll_keys(); poll_pointers(); }
    if (qh == qt) return false;
    *e = q[qh]; qh = (qh + 1) % 512;
    return true;
}

void pal_wait(int ms) {
    if (ms <= 0) return;
    EFI_EVENT evs[20]; UINTN n = 0, idx;
    BS->SetTimer(timer_ev, TimerRelative, (u64)ms * 10000);
    evs[n++] = timer_ev;
    evs[n++] = kbx ? kbx->WaitForKeyEx : ST->ConIn->WaitForKey;
    bool real = false; for (int i = 0; i < nptrs; i++) if (!ptrs[i].virt) real = true;
    for (int i = 0; i < nptrs && n < 20; i++) {
        if (real && ptrs[i].virt) continue;
        EFI_EVENT ev = ptrs[i].sp ? ptrs[i].sp->WaitForInput : ptrs[i].ap->WaitForInput;
        if (ev) evs[n++] = ev;
    }
    BS->WaitForEvent(n, evs, &idx);
}

/* ------------------------------------------------------------ arquivos */
typedef struct { EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs; EFI_FILE_PROTOCOL *root; bool boot; char label[64]; bool has_guid; u8 guid[16]; } Vol;
static Vol vols[16]; static int nvols;

static void w_to_utf8(const CHAR16 *w, char *o, int cap) {
    int n = 0; char t[4];
    for (; *w && n < cap - 4; w++) { int k = utf8_encode(*w, t); for (int i = 0; i < k; i++) o[n++] = t[i]; }
    o[n] = 0;
}
static void path_w(const char *p, CHAR16 *o, int cap) {
    int n = 0, len = str_len(p);
    for (int i = 0; i < len && n < cap - 1; ) {
        u32 c; i += utf8_decode(p, len, i, &c);
        o[n++] = c == '/' ? '\\' : (CHAR16)c;
    }
    while (n > 1 && o[n - 1] == '\\') n--;
    if (n == 0) o[n++] = '\\';
    o[n] = 0;
}

static void fs_init(void) {
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    BS->HandleProtocol(image, &limg_guid, (void **)&li);
    EFI_HANDLE *hs; UINTN n;
    if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &sfs_guid, 0, &n, &hs))) return;
    /* volume de boot primeiro */
    for (int pass = 0; pass < 2; pass++)
        for (UINTN i = 0; i < n && nvols < 16; i++) {
            bool isboot = li && hs[i] == li->DeviceHandle;
            if ((pass == 0) != isboot) continue;
            Vol *v = &vols[nvols];
            if (EFI_ERROR(BS->HandleProtocol(hs[i], &sfs_guid, (void **)&v->fs))) continue;
            if (EFI_ERROR(v->fs->OpenVolume(v->fs, &v->root))) continue;
            v->boot = isboot; v->has_guid = false;
            {   /* partição GPT do volume: nó de disco rígido do caminho, com assinatura GUID */
                u8 *dp = 0;
                if (!EFI_ERROR(BS->HandleProtocol(hs[i], &dp_guid, (void **)&dp)) && dp)
                    for (int k = 0; k < 64 && dp[0] != 0x7F; k++) {
                        u32 l = dp[2] | dp[3] << 8; if (l < 4) break;
                        if (dp[0] == 4 && dp[1] == 1 && l >= 42 && dp[41] == 2) { memcpy(v->guid, dp + 24, 16); v->has_guid = true; }
                        dp += l;
                    }
            }
            u8 buf[512]; UINTN sz = sizeof buf;
            v->label[0] = 0;
            if (!EFI_ERROR(v->root->GetInfo(v->root, &fsinfo_guid, &sz, buf))) w_to_utf8(((EFI_FILE_SYSTEM_INFO *)buf)->VolumeLabel, v->label, 64);
            nvols++;
        }
    BS->FreePool(hs);
}

/* ------------------------------------------------------------ discos inteiros (Block I/O) */
static EFI_GUID bio_guid   = { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID dinfo_guid = { 0xd432a67f, 0x14dc, 0x484b, { 0xb3, 0xbb, 0x3f, 0x02, 0x91, 0x84, 0x93, 0x27 } };
static EFI_GUID dpath_guid = { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
typedef struct { EFI_BLOCK_IO_PROTOCOL *bio; DiskInfo info; } Disk;
static Disk disks[16]; static int ndisks; static bool disks_done;
void *pal_dma_alloc(usize bytes);
static u8 *disk_bounce;            /* 64 KB alinhados a 4 KB, para firmwares que exigem IoAlign */
#define DISK_CHUNK 128             /* setores por chamada (64 KB com 512) */

static u32 dp_size(EFI_DEVICE_PATH *p) {   /* bytes até o nó final, sem ele */
    u32 n = 0;
    for (int k = 0; k < 64 && p->Type != 0x7F; k++) { u32 l = DP_LEN(p); if (l < 4) break; n += l; p = (EFI_DEVICE_PATH *)((u8 *)p + l); }
    return n;
}
static int dp_kind(EFI_DEVICE_PATH *p) {
    int kind = DISK_OTHER;
    for (int k = 0; k < 64 && p->Type != 0x7F; k++) {
        u32 l = DP_LEN(p); if (l < 4) break;
        if (p->Type == 3) switch (p->SubType) {       /* nós de mensagem: o barramento do disco */
            case 0x01: kind = DISK_IDE; break;   case 0x02: kind = DISK_SCSI; break; case 0x05: kind = DISK_USB; break;
            case 0x12: kind = DISK_SATA; break;  case 0x17: kind = DISK_NVME; break; case 0x1A: kind = DISK_SD; break;
            case 0x1D: kind = DISK_MMC; break;   case 0x0F: kind = DISK_USB; break;   /* USB por classe */
        }
        if (p->Type == 4 && p->SubType == 3 && kind == DISK_OTHER) kind = DISK_VIRTUAL;   /* vendor de mídia (RAM disk etc.) */
        p = (EFI_DEVICE_PATH *)((u8 *)p + l);
    }
    return kind;
}
/* texto ASCII de um campo de identificação, sem espaços nas pontas; swap = palavras ATA (bytes trocados) */
static void id_str(char *o, int cap, int *n, const u8 *s, int len, bool swap) {
    if (*n && *n < cap - 1) o[(*n)++] = ' ';
    int st = *n;
    for (int i = 0; i < len && *n < cap - 1; i++) {
        u8 c = s[swap ? (i ^ 1) : i];
        if (c < 32 || c > 126) c = ' ';
        if (c == ' ' && (*n == st || o[*n - 1] == ' ')) continue;
        o[(*n)++] = (char)c;
    }
    while (*n > 0 && o[*n - 1] == ' ') (*n)--;
    o[*n] = 0;
}
static void disk_model(EFI_HANDLE h, Disk *d) {
    EFI_DISK_INFO_PROTOCOL *di;
    if (EFI_ERROR(BS->HandleProtocol(h, &dinfo_guid, (void **)&di)) || !di) return;
    static u8 b[4096]; UINT32 sz = sizeof b; int n = 0;
    memset(b, 0, sizeof b);
    if ((d->info.kind == DISK_SATA || d->info.kind == DISK_IDE) && di->Identify && !EFI_ERROR(di->Identify(di, b, &sz)) && sz >= 94)
        id_str(d->info.model, sizeof d->info.model, &n, b + 54, 40, true);           /* ATA IDENTIFY, palavras 27..46 */
    else if (d->info.kind == DISK_NVME && di->Identify && !EFI_ERROR(di->Identify(di, b, &sz)) && sz >= 64 && b[4] && b[24] >= 32)
        id_str(d->info.model, sizeof d->info.model, &n, b + 24, 40, false);          /* NVMe: dados do controlador, se vierem */
    else { sz = sizeof b; if (di->Inquiry && !EFI_ERROR(di->Inquiry(di, b, &sz)) && sz >= 32) {
        id_str(d->info.model, sizeof d->info.model, &n, b + 8, 8, false);            /* SCSI/USB: fabricante + produto */
        id_str(d->info.model, sizeof d->info.model, &n, b + 16, 16, false); } }
}
static void disks_init(void) {
    if (disks_done) return;
    disks_done = true;
    EFI_HANDLE *hs; UINTN n;
    if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &bio_guid, 0, &n, &hs))) return;
    /* caminho do volume de boot: o disco cujo caminho é o começo dele é o disco de boot */
    EFI_LOADED_IMAGE_PROTOCOL *li = 0; EFI_DEVICE_PATH *bootdp = 0;
    if (!EFI_ERROR(BS->HandleProtocol(image, &limg_guid, (void **)&li)) && li) BS->HandleProtocol(li->DeviceHandle, &dpath_guid, (void **)&bootdp);
    u32 bootlen = bootdp ? dp_size(bootdp) : 0;
    for (UINTN i = 0; i < n && ndisks < 16; i++) {
        EFI_BLOCK_IO_PROTOCOL *b;
        if (EFI_ERROR(BS->HandleProtocol(hs[i], &bio_guid, (void **)&b)) || !b || !b->Media) continue;
        EFI_BLOCK_IO_MEDIA *m = b->Media;
        if (m->LogicalPartition || !m->MediaPresent || !m->BlockSize || m->BlockSize > 4096) continue;
        Disk *d = &disks[ndisks]; memset(d, 0, sizeof *d);
        d->bio = b;
        d->info.nsec = m->LastBlock + 1; d->info.bs = m->BlockSize;
        d->info.removable = m->RemovableMedia; d->info.readonly = m->ReadOnly;
        EFI_DEVICE_PATH *dp = 0;
        if (!EFI_ERROR(BS->HandleProtocol(hs[i], &dpath_guid, (void **)&dp)) && dp) {
            d->info.kind = dp_kind(dp);
            u32 l = dp_size(dp);
            d->info.boot = bootdp && l && l <= bootlen && !memcmp(dp, bootdp, l);
        }
        disk_model(hs[i], d);
        ndisks++;
    }
    BS->FreePool(hs);
}
int pal_disk_count(void) { disks_init(); return ndisks; }
bool pal_disk_info(int k, DiskInfo *o) { disks_init(); if (k < 0 || k >= ndisks) return false; *o = disks[k].info; return true; }
bool pal_disk_io(int k, u64 lba, u32 count, void *buf, bool write) {
    disks_init();
    if (k < 0 || k >= ndisks) return false;
    Disk *d = &disks[k]; EFI_BLOCK_IO_PROTOCOL *b = d->bio; u32 bs = d->info.bs;
    if (lba + count > d->info.nsec || (write && d->info.readonly)) return false;
    if (!disk_bounce) disk_bounce = pal_dma_alloc(DISK_CHUNK * 512);
    u32 align = b->Media->IoAlign;
    u8 *p = buf;
    while (count) {
        u32 k2 = count > DISK_CHUNK ? DISK_CHUNK : count;
        if (k2 * bs > DISK_CHUNK * 512 && bs > 512) k2 = DISK_CHUNK * 512 / bs;
        bool bounce = align > 1 && ((uintptr_t)p & (align - 1)) && disk_bounce;
        u8 *io = bounce ? disk_bounce : p;
        if (bounce && write) memcpy(io, p, (usize)k2 * bs);
        EFI_STATUS s = write ? b->WriteBlocks(b, b->Media->MediaId, lba, (UINTN)k2 * bs, io) : b->ReadBlocks(b, b->Media->MediaId, lba, (UINTN)k2 * bs, io);
        if (EFI_ERROR(s)) return false;
        if (bounce && !write) memcpy(p, io, (usize)k2 * bs);
        lba += k2; count -= k2; p += (usize)k2 * bs;
    }
    return true;   /* o flush fica para pal_disk_flush, nos pontos seguros (no QEMU cada flush é um fsync no hospedeiro) */
}
bool pal_disk_flush(int k) {
    disks_init();
    if (k < 0 || k >= ndisks || !disks[k].bio->FlushBlocks) return k >= 0 && k < ndisks;
    return !EFI_ERROR(disks[k].bio->FlushBlocks(disks[k].bio));
}

int pal_vol_count(void) { return nvols; }
bool pal_vol_info(int v, VolInfo *o) {
    if (v < 0 || v >= nvols) return false;
    memset(o, 0, sizeof *o);
    o->boot = vols[v].boot;
    str_copy(o->label, vols[v].label[0] ? vols[v].label : (vols[v].boot ? "FOLIO" : "Disco"), sizeof o->label);
    o->has_guid = vols[v].has_guid; memcpy(o->part_guid, vols[v].guid, 16);
    return true;
}

static EFI_FILE_PROTOCOL *open_path(int vol, const char *path, u64 mode, u64 attr) {
    if (vol < 0 || vol >= nvols) return 0;
    CHAR16 w[520]; path_w(path, w, 520);
    if (w[0] == '\\' && w[1] == 0) {
        EFI_FILE_PROTOCOL *f;
        if (EFI_ERROR(vols[vol].fs->OpenVolume(vols[vol].fs, &f))) return 0;
        return f;
    }
    EFI_FILE_PROTOCOL *f;
    if (EFI_ERROR(vols[vol].root->Open(vols[vol].root, &f, w, mode, attr))) return 0;
    return f;
}
static EFI_FILE_INFO *get_info(EFI_FILE_PROTOCOL *f) {
    UINTN sz = 0; EFI_FILE_INFO *i = 0;
    EFI_STATUS s = f->GetInfo(f, &finfo_guid, &sz, 0);
    if (s != EFI_BUFFER_TOO_SMALL) return 0;
    i = mem_alloc(sz + 16);
    if (EFI_ERROR(f->GetInfo(f, &finfo_guid, &sz, i))) { mem_free(i); return 0; }
    return i;
}
static void efi_dt(EFI_TIME *e, DateTime *d) { d->year = e->Year; d->month = e->Month; d->day = e->Day; d->hour = e->Hour; d->min = e->Minute; d->sec = e->Second; }

int pal_list(int vol, const char *path, FileEntry **out) {
    *out = 0;
    EFI_FILE_PROTOCOL *d = open_path(vol, path, EFI_FILE_MODE_READ, 0);
    if (!d) return -1;
    int n = 0, cap = 64;
    FileEntry *l = mem_alloc(sizeof(FileEntry) * (usize)cap);
    UINTN bcap = 1024; u8 *buf = mem_alloc(bcap);
    while (true) {
        UINTN sz = bcap;
        EFI_STATUS s = d->Read(d, &sz, buf);
        if (s == EFI_BUFFER_TOO_SMALL) { bcap = sz + 64; mem_free(buf); buf = mem_alloc(bcap); continue; }
        if (EFI_ERROR(s) || sz == 0) break;
        EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
        if (n >= cap) { cap *= 2; l = mem_realloc(l, sizeof(FileEntry) * (usize)cap); }
        FileEntry *e = &l[n];
        memset(e, 0, sizeof *e);
        w_to_utf8(fi->FileName, e->name, sizeof e->name);
        if (str_eq(e->name, ".") || str_eq(e->name, "..")) continue;
        e->dir = (fi->Attribute & EFI_FILE_DIRECTORY) != 0;
        e->size = fi->FileSize;
        efi_dt(&fi->ModificationTime, &e->mtime);
        n++;
    }
    mem_free(buf);
    d->Close(d);
    *out = l;
    return n;
}

bool pal_read(int vol, const char *path, u8 **data, usize *len) {
    EFI_FILE_PROTOCOL *f = open_path(vol, path, EFI_FILE_MODE_READ, 0);
    if (!f) return false;
    EFI_FILE_INFO *i = get_info(f);
    if (!i || (i->Attribute & EFI_FILE_DIRECTORY)) { mem_free(i); f->Close(f); return false; }
    usize sz = (usize)i->FileSize; mem_free(i);
    u8 *b = mem_alloc(sz + 1);
    if (!b) { f->Close(f); return false; }
    usize got = 0;
    while (got < sz) {
        UINTN chunk = MIN((usize)(1 << 20), sz - got);
        if (EFI_ERROR(f->Read(f, &chunk, b + got)) || chunk == 0) break;
        got += chunk;
    }
    f->Close(f);
    b[got] = 0; *data = b; *len = got;
    return true;
}

static bool write_raw(int vol, const char *path, const void *data, usize len) {
    EFI_FILE_PROTOCOL *f = open_path(vol, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (!f) return false;
    /* trunca se já existia */
    EFI_FILE_INFO *i = get_info(f);
    if (i && i->FileSize > 0) { i->FileSize = 0; f->SetInfo(f, &finfo_guid, (UINTN)i->Size, i); }
    mem_free(i);
    f->SetPosition(f, 0);
    usize done = 0; bool ok = true;
    while (done < len) {
        UINTN chunk = MIN((usize)(256 << 10), len - done);
        if (EFI_ERROR(f->Write(f, &chunk, (u8 *)data + done)) || chunk == 0) { ok = false; break; }
        done += chunk;
    }
    if (EFI_ERROR(f->Flush(f))) ok = false;
    f->Close(f);
    return ok;
}

bool pal_rename(int vol, const char *path, const char *nn) {
    EFI_FILE_PROTOCOL *f = open_path(vol, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!f) return false;
    EFI_FILE_INFO *i = get_info(f);
    if (!i) { f->Close(f); return false; }
    UINTN base = (UINTN)((u8 *)i->FileName - (u8 *)i);
    usize sz = base + 2 * 260;
    EFI_FILE_INFO *ni = mem_zalloc(sz);
    memcpy(ni, i, base);
    path_w(nn, ni->FileName, 259);
    if (ni->FileName[0] == '\\') memmove(ni->FileName, ni->FileName + 1, 2 * 258);
    UINTN nl = 0; while (ni->FileName[nl]) nl++;
    ni->Size = base + (nl + 1) * 2;
    EFI_STATUS s = f->SetInfo(f, &finfo_guid, (UINTN)ni->Size, ni);
    mem_free(i); mem_free(ni);
    f->Close(f);
    return !EFI_ERROR(s);
}

bool pal_remove(int vol, const char *path) {
    EFI_FILE_PROTOCOL *f = open_path(vol, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!f) return false;
    return !EFI_ERROR(f->Delete(f));   /* Delete fecha o handle */
}

bool pal_exists(int vol, const char *path, bool *is_dir) {
    EFI_FILE_PROTOCOL *f = open_path(vol, path, EFI_FILE_MODE_READ, 0);
    if (!f) return false;
    EFI_FILE_INFO *i = get_info(f);
    if (is_dir) *is_dir = i && (i->Attribute & EFI_FILE_DIRECTORY);
    mem_free(i); f->Close(f);
    return true;
}

/* gravação segura: escreve em .tmp, confere, depois troca pelo original */
bool pal_write(int vol, const char *path, const void *data, usize len) {
    char tmp[560]; fmt(tmp, sizeof tmp, "%s.tmp", path);
    if (!write_raw(vol, tmp, data, len)) { pal_remove(vol, tmp); return write_raw(vol, path, data, len); }
    bool d;
    if (pal_exists(vol, path, &d)) pal_remove(vol, path);
    if (pal_rename(vol, tmp, path_basename(path))) return true;
    bool ok = write_raw(vol, path, data, len);
    if (ok) pal_remove(vol, tmp);
    return ok;
}

bool pal_mkdir(int vol, const char *path) {
    EFI_FILE_PROTOCOL *f = open_path(vol, path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY);
    if (!f) return false;
    f->Close(f);
    return true;
}

struct PalFile { EFI_FILE_PROTOCOL *f; };
PalFile *pal_fopen(int vol, const char *path, u64 *size) {
    EFI_FILE_PROTOCOL *f = open_path(vol, path, EFI_FILE_MODE_READ, 0);
    if (!f) return 0;
    EFI_FILE_INFO *i = get_info(f);
    *size = i ? i->FileSize : 0; mem_free(i);
    PalFile *p = mem_alloc(sizeof *p); p->f = f;
    return p;
}
int pal_fread(PalFile *p, void *b, int n) { UINTN sz = (UINTN)n; if (EFI_ERROR(p->f->Read(p->f, &sz, b))) return -1; return (int)sz; }
bool pal_fseek(PalFile *p, u64 pos) { return !EFI_ERROR(p->f->SetPosition(p->f, pos)); }
void pal_fclose(PalFile *p) { if (p) { p->f->Close(p->f); mem_free(p); } }

/* ------------------------------------------------------------ memória DMA (abaixo de 4 GB) para o áudio */
void *pal_dma_alloc(usize bytes) {
    EFI_PHYSICAL_ADDRESS a = 0xFFFFFFFFull;
    UINTN pages = (bytes + 4095) / 4096;
    if (EFI_ERROR(BS->AllocatePages(AllocateMaxAddress, EfiBootServicesData, pages, &a))) return 0;
    memset((void *)(uintptr_t)a, 0, pages * 4096);
    return (void *)(uintptr_t)a;
}
void pal_stall_us(u32 us) { BS->Stall(us); }


void pal_diag_audio(Buf *o);
void pal_diag(Buf *o) {
    buf_printf(o, "FolioOS %s · %s · screen %dx%d\r\n", FOLIO_VERSION, pal_name(), screen.w, screen.h);
    char fw[128] = "?";
    if (ST->FirmwareVendor) w_to_utf8(ST->FirmwareVendor, fw, sizeof fw);
    buf_printf(o, "Firmware: %s rev %x · UEFI %d.%d\r\n\r\n", fw, (int)ST->FirmwareRevision, (int)(ST->Hdr.Revision >> 16), (int)((ST->Hdr.Revision & 0xFFFF) / 10));
    pal_diag_video(o); buf_puts(o, "\r\n"); pal_diag_input(o); buf_puts(o, "\r\n"); pal_diag_audio(o);
}

/* ------------------------------------------------------------ ponto de entrada */
static void say(const char *s) {
    CHAR16 w[256]; int n = 0;
    for (int i = 0; s[i] && n < 254; ) { u32 c; i += utf8_decode(s, str_len(s), i, &c); w[n++] = (CHAR16)c; }
    w[n++] = '\r'; w[n] = 0;
    CHAR16 nl[2] = { '\n', 0 };
    ST->ConOut->OutputString(ST->ConOut, w);
    ST->ConOut->OutputString(ST->ConOut, nl);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE img, EFI_SYSTEM_TABLE *st) {
    image = img; ST = st; BS = st->BootServices; efi_st = st; efi_bs = BS;
    BS->SetWatchdogTimer(0, 0, 0, 0);          /* sem isso o firmware reinicia após 5 minutos */
    /* heap: tenta o máximo razoável */
    static const UINTN sizes_mb[] = { 768, 512, 256, 128, 64, 32 };
    EFI_PHYSICAL_ADDRESS hb = 0; UINTN hsz = 0;
    for (int i = 0; i < 6; i++) {
        UINTN pages = sizes_mb[i] * 256;
#if __SIZEOF_POINTER__ == 4
        if (sizes_mb[i] > 512) continue;
#endif
        if (!EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &hb))) { hsz = pages * 4096; break; }
    }
    if (!hsz) { say("FolioOS: not enough memory."); BS->Stall(5000000); return 1; }
    heap_init((void *)(uintptr_t)hb, hsz);
    /* calibra o TSC */
    u64 a = rdtsc(); BS->Stall(20000); u64 b = rdtsc();
    tsc_per_ms = (b - a) / 20; if (tsc_per_ms == 0) tsc_per_ms = 1;
    tsc0 = rdtsc();
    video_init();
    bool connected = false;
    if (!gop && !uga) {                         /* "fast boot": a placa de vídeo ainda não foi ligada pelo firmware */
        connect_all(); connected = true;
        VL("  no video output before connecting devices; trying again\r\n");
        video_init();
    }
    if (!gop && !uga) {
        say("FolioOS: this computer offers no UEFI graphics (neither GOP nor UGA).");
        say("Very old graphics cards only work in BIOS/CSM mode; see README.txt.");
        BS->Stall(10000000); return 1;
    }
    st->ConOut->EnableCursor(st->ConOut, 0);
    if (!connected) {
        connect_all();
        if (gop && !edid_ok) {                   /* um monitor pode ter aparecido só depois de conectar tudo */
            EFI_GRAPHICS_OUTPUT_PROTOCOL *old = gop;
            VL("  after connecting devices:\r\n");
            pick_output();
            if (gop && gop != old && edid_ok) { pick_mode(); if (!video_setup()) { gop = old; edid_ok = false; video_setup(); } }
            else { gop = old; uga = 0; edid_ok = false; }
        }
    }
    input_init();
    fs_init();

    app_run();
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------ entradas de boot (Boot####, BootOrder, BootNext)
 * O outro sistema é iniciado com BootNext + reinício, nunca carregado por dentro do FolioOS: assim o firmware
 * inicia o Windows do jeito de sempre e o TPM não registra o FolioOS no caminho (o BitLocker não pede a chave). */
static EFI_GUID gvar = { 0x8BE4DF61, 0x93CA, 0x11d2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };
static void boot_var_name(CHAR16 *o, const char *pfx, int num) {
    int k = 0; while (*pfx) o[k++] = (CHAR16)*pfx++;
    if (num >= 0) for (int s = 12; s >= 0; s -= 4) o[k++] = (CHAR16)"0123456789ABCDEF"[(num >> s) & 15];
    o[k] = 0;
}
static u8 *var_get(CHAR16 *name, UINTN *size) {
    EFI_RUNTIME_SERVICES *rt = ST->RuntimeServices;
    if (!rt || !rt->GetVariable) return 0;
    UINT32 attr; UINTN n = 0;
    if (rt->GetVariable(name, &gvar, &attr, &n, 0) != EFI_BUFFER_TOO_SMALL || !n) return 0;
    u8 *b = mem_alloc(n + 2);
    if (!b || EFI_ERROR(rt->GetVariable(name, &gvar, &attr, &n, b))) { mem_free(b); return 0; }
    *size = n; return b;
}
static bool var_set(CHAR16 *name, const void *d, UINTN n) {
    EFI_RUNTIME_SERVICES *rt = ST->RuntimeServices;
    return rt && rt->SetVariable && !EFI_ERROR(rt->SetVariable(name, &gvar, 7 /* NV | BS | RT */, n, (void *)d));
}
/* opção de carga: UINT32 atributos, UINT16 tamanho do caminho, descrição (UTF-16 com 0), caminho */
static bool opt_parse(const u8 *v, UINTN n, u32 *attrs, const CHAR16 **desc, const u8 **dp, u32 *dplen) {
    if (n < 8) return false;
    *attrs = (u32)v[0] | (u32)v[1] << 8 | (u32)v[2] << 16 | (u32)v[3] << 24;
    u32 fl = (u32)v[4] | (u32)v[5] << 8;
    UINTN k = 6; *desc = (const CHAR16 *)(v + 6);
    while (k + 1 < n && (v[k] | v[k + 1])) k += 2;
    k += 2;
    if (k + fl > n) return false;
    *dp = v + k; *dplen = fl;
    return true;
}
/* o caminho tem um nó de arquivo com "FolioOS"? (a nossa entrada, em qualquer partição) */
static bool dp_is_folio(const u8 *dp, u32 len) {
    for (u32 k = 0; k + 4 <= len; ) {
        u32 l = dp[k + 2] | dp[k + 3] << 8; if (l < 4 || k + l > len) break;
        if (dp[k] == 4 && dp[k + 1] == 4) {
            const u8 *s = dp + k + 4; u32 nch = (l - 4) / 2;
            for (u32 i = 0; i + 7 <= nch; i++) {
                static const char w[] = "folioos"; bool m = true;
                for (int j = 0; j < 7 && m; j++) { u16 c = (u16)(s[2 * (i + j)] | s[2 * (i + j) + 1] << 8); if (c >= 'A' && c <= 'Z') c += 32; m = c == (u16)w[j]; }
                if (m) return true;
            }
        }
        if (dp[k] == 0x7F) break;
        k += l;
    }
    return false;
}
static bool dp_has(const u8 *dp, u32 len, u8 type, u8 sub) {
    for (u32 k = 0; k + 4 <= len; ) {
        u32 l = dp[k + 2] | dp[k + 3] << 8; if (l < 4 || k + l > len) break;
        if (dp[k] == type && dp[k + 1] == sub) return true;
        if (dp[k] == 0x7F) break;
        k += l;
    }
    return false;
}
bool pal_boot_entry_add(const char *desc, int part_num, u64 part_lba, u64 part_count, const u8 guid[16], const char *path) {
    EFI_RUNTIME_SERVICES *rt = ST->RuntimeServices;
    if (!rt || !rt->SetVariable || !rt->GetVariable) return false;
    static u8 opt[600]; u32 k = 0;
    #define P8(v) (opt[k++] = (u8)(v))
    #define P16(v) do { u32 _v = (u32)(v); P8(_v); P8(_v >> 8); } while (0)
    #define P32(v) do { u32 _w = (u32)(v); P16(_w); P16(_w >> 16); } while (0)
    #define P64(v) do { u64 _q = (u64)(v); P32(_q); P32(_q >> 32); } while (0)
    P32(1);                                           /* LOAD_OPTION_ACTIVE */
    u32 fl_at = k; P16(0);
    for (int i = 0; desc[i] && k < 200; i++) P16((u8)desc[i]);
    P16(0);
    u32 dp_at = k;
    P8(4); P8(1); P16(42); P32(part_num); P64(part_lba); P64(part_count);   /* nó de disco rígido (GPT) */
    for (int i = 0; i < 16; i++) P8(guid[i]);
    P8(2); P8(2);                                     /* MBRType = GPT, SignatureType = GUID */
    int plen = str_len(path);
    P8(4); P8(4); P16(4 + 2 * (plen + 1));            /* nó de arquivo */
    for (int i = 0; i < plen; i++) P16((u8)path[i]);
    P16(0);
    P8(0x7F); P8(0xFF); P16(4);
    u32 dplen = k - dp_at;
    opt[fl_at] = (u8)dplen; opt[fl_at + 1] = (u8)(dplen >> 8);
    #undef P8
    #undef P16
    #undef P32
    #undef P64
    /* uma entrada com o mesmo caminho já existe? (reinstalação) */
    CHAR16 nm[16]; UINTN osz = 0; boot_var_name(nm, "BootOrder", -1);
    u16 *order = (u16 *)var_get(nm, &osz); int on = order ? (int)(osz / 2) : 0;
    int num = -1;
    for (int i = 0; i < on && num < 0; i++) {
        boot_var_name(nm, "Boot", order[i]); UINTN n; u8 *v = var_get(nm, &n);
        u32 at; const CHAR16 *d; const u8 *dp; u32 dl;
        if (v && opt_parse(v, n, &at, &d, &dp, &dl) && dl == dplen && !memcmp(dp, opt + dp_at, dplen)) num = order[i];
        mem_free(v);
    }
    for (int c = 0; num < 0 && c < 0x1000; c++) {    /* número livre */
        boot_var_name(nm, "Boot", c); UINTN n; u8 *v = var_get(nm, &n);
        if (!v) num = c; else mem_free(v);
    }
    bool ok = num >= 0;
    if (ok) { boot_var_name(nm, "Boot", num); ok = var_set(nm, opt, k); }
    if (ok) {                                         /* o FolioOS vai para o começo da ordem; o resto fica como estava */
        u16 *no = mem_alloc((usize)(on + 1) * 2); int nn = 0;
        no[nn++] = (u16)num;
        for (int i = 0; i < on; i++) if (order[i] != num) no[nn++] = order[i];
        boot_var_name(nm, "BootOrder", -1);
        ok = var_set(nm, no, (UINTN)nn * 2);
        mem_free(no);
    }
    mem_free(order);
    return ok;
}
/* a entrada é do FolioOS: o caminho passa por \EFI\FolioOS (ao lado) ou a descrição é "FolioOS" (disco inteiro) */
static bool entry_is_folio(const CHAR16 *d, const u8 *dp, u32 dl) {
    static const char nm[] = "FolioOS"; int i = 0;
    for (; nm[i] && d[i] == (CHAR16)nm[i]; i++) {}
    return dp_is_folio(dp, dl) || (!nm[i] && !d[i]);
}
/* desinstalar: as entradas Boot#### do FolioOS somem, e a BootOrder fica com o resto, na mesma ordem; o firmware
 * volta a iniciar o outro sistema direto */
bool pal_boot_entry_remove(void) {
    EFI_RUNTIME_SERVICES *rt = ST->RuntimeServices;
    if (!rt || !rt->SetVariable || !rt->GetVariable) return false;
    CHAR16 nm[16]; UINTN osz = 0; boot_var_name(nm, "BootOrder", -1);
    u16 *order = (u16 *)var_get(nm, &osz); int on = order ? (int)(osz / 2) : 0;
    u16 *keep = mem_alloc((usize)on * 2 + 2); int nk = 0; bool ok = true;
    for (int i = 0; i < on; i++) {
        boot_var_name(nm, "Boot", order[i]); UINTN n; u8 *v = var_get(nm, &n);
        u32 at; const CHAR16 *d; const u8 *dp; u32 dl;
        char dn[64] = "?";
        bool parsed = v && opt_parse(v, n, &at, &d, &dp, &dl);
        if (parsed) w_to_utf8(d, dn, sizeof dn);
        if (parsed && entry_is_folio(d, dp, dl)) {
            dbg("boot entry remove: Boot%04X \"%s\" (FolioOS)", order[i], dn);
            if (EFI_ERROR(rt->SetVariable(nm, &gvar, 7, 0, 0))) ok = false;   /* tamanho 0 = apagar a variável */
        } else { dbg("boot entry keep: Boot%04X \"%s\"", order[i], dn); keep[nk++] = order[i]; }
        mem_free(v);
    }
    if (nk != on) { boot_var_name(nm, "BootOrder", -1); ok = var_set(nm, keep, (UINTN)nk * 2) && ok; }
    mem_free(keep); mem_free(order);
    /* o firmware pode ter tirado a entrada da ordem sem apagá-la (o OVMF reescreve a BootOrder a cada boot):
     * as variáveis Boot#### do FolioOS fora da ordem também somem */
    for (int c = 0; c < 0x1000; c++) {
        boot_var_name(nm, "Boot", c); UINTN n; u8 *v = var_get(nm, &n);
        if (!v) continue;
        u32 at; const CHAR16 *d; const u8 *dp; u32 dl;
        if (opt_parse(v, n, &at, &d, &dp, &dl) && entry_is_folio(d, dp, dl)) {
            dbg("boot entry remove: Boot%04X (outside BootOrder)", c);
            if (EFI_ERROR(rt->SetVariable(nm, &gvar, 7, 0, 0))) ok = false;
        }
        mem_free(v);
    }
    return ok;
}
static int other_num = -1;
bool pal_boot_other(char *name, int cap) {
    CHAR16 nm[16]; UINTN osz = 0; boot_var_name(nm, "BootOrder", -1);
    u16 *order = (u16 *)var_get(nm, &osz); int on = order ? (int)(osz / 2) : 0;
    other_num = -1;
    for (int i = 0; i < on && other_num < 0; i++) {
        boot_var_name(nm, "Boot", order[i]); UINTN n; u8 *v = var_get(nm, &n);
        u32 at; const CHAR16 *d; const u8 *dp; u32 dl;
        if (v && opt_parse(v, n, &at, &d, &dp, &dl) && (at & 1) && !dp_is_folio(dp, dl)
            && !dp_has(dp, dl, 4, 6) && !dp_has(dp, dl, 4, 7)            /* aplicativos do firmware (shell, setup) */
            && !dp_has(dp, dl, 3, 11) && !dp_has(dp, dl, 3, 12) && !dp_has(dp, dl, 3, 13)   /* rede (PXE, HTTP) */
            && !dp_has(dp, dl, 3, 24)) {
            other_num = order[i];
            w_to_utf8(d, name, cap);
        }
        mem_free(v);
    }
    mem_free(order);
    return other_num >= 0;
}
void pal_boot_other_start(void) {
    if (other_num < 0) return;
    CHAR16 nm[16]; boot_var_name(nm, "BootNext", -1);
    u16 n = (u16)other_num;
    if (!var_set(nm, &n, 2)) return;
    ST->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, 0);
}

/* ------------------------------------------------------------ idioma do firmware
 * PlatformLang (variável global da UEFI) traz o idioma do setup, no formato RFC 4646 ("en-US").
 * Serve só de sugestão no primeiro boot (language=auto); o .po correspondente precisa existir. */
bool pal_lang(char *out, int cap) {
    EFI_RUNTIME_SERVICES *rt = ST->RuntimeServices;
    if (!rt || !rt->GetVariable || cap < 2) return false;
    static EFI_GUID global = { 0x8BE4DF61, 0x93CA, 0x11d2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };
    CHAR16 name[] = { 'P', 'l', 'a', 't', 'f', 'o', 'r', 'm', 'L', 'a', 'n', 'g', 0 };
    char buf[32]; UINTN size = sizeof buf - 1; UINT32 attr = 0;
    if (EFI_ERROR(rt->GetVariable(name, &global, &attr, &size, buf)) || size == 0 || size >= sizeof buf) return false;
    buf[size] = 0;
    int n = 0;
    for (UINTN i = 0; i < size && buf[i] && n < cap - 1; i++) {
        char c = buf[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) break;
        out[n++] = c;
    }
    out[n] = 0;
    return n >= 2;
}
