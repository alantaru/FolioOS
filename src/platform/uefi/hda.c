/* FOLIO v4 — áudio bare-metal (por polling, sem interrupções)
 *
 * Backends, tentados nesta ordem:
 *   1. Intel High Definition Audio (HDA) — praticamente todo PC/notebook de 2005 em diante:
 *      controladoras Intel (inclusive Smart Sound/SST em modo HDA), AMD, VIA, SiS, NVIDIA, VMware, QEMU,
 *      VirtualBox; codecs Realtek, Conexant, IDT/Sigmatel, Cirrus (Macs), VIA, Analog Devices…
 *   2. AC'97 — PCs de ~1998–2007 (Intel ICH, VIA, nForce, SiS) e o VirtualBox no modo AC'97.
 *   3. Placas de vídeo (só HDMI/DisplayPort) por último.
 *
 * Robustez (cada item corresponde a um caso real em que o som fica mudo):
 *  - acesso PCI pelo protocolo PCI I/O do firmware (portas 0xCF8 só como último recurso);
 *  - toda memória de DMA alocada e MAPEADA pelo firmware abaixo de 4 GB (funciona com a "proteção de DMA
 *    pré-boot"/IOMMU dos notebooks recentes);
 *  - coerência de cache: snoop ligado (Intel/AMD/NVIDIA) e clflush nos buffers;
 *  - Intel Skylake+: liga o link HDA e corrige o clock (multi-link), desvia o DMA do DSP (GPROCEN);
 *  - codec que não se anuncia (STATESTS 0): tenta os endereços 0..3 mesmo assim;
 *  - CORB/RIRB sem resposta: comandos imediatos, por codec;
 *  - posição de reprodução por LPIB ou pelo buffer de posição (o que andar), como o "position_fix" do Linux;
 *  - tenta todos os streams de saída até achar um cujo DMA ande;
 *  - liga alto-falante, fone e saída de linha ao mesmo tempo, EAPD e GPIO (amplificadores externos);
 *  - se a configuração de pinos da BIOS estiver errada, tenta qualquer pino de saída analógico;
 *  - registra tudo em FOLIO\DIAGNOSTICO.TXT. */
#include "efi.h"
#include "../../core/folio.h"

void *pal_dma_alloc(usize bytes);
void  pal_stall_us(u32 us);
void  hda_probe(void);

static EFI_GUID pciio_guid = { 0x4cf5b200, 0x68b8, 0x4ca5, { 0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a } };
static EFI_GUID rbio_guid  = { 0x2f707ebb, 0x4a1a, 0x11d4, { 0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };

/* ------------------------------------------------------------ registro de diagnóstico */
static Buf dlog;
static void HLOG(const char *f, ...) {
    char t[512]; va_list ap; va_start(ap, f); int n = vfmt(t, sizeof t, f, ap); va_end(ap);
    buf_put(&dlog, t, (usize)MIN(n, (int)sizeof t - 1));
}

/* ------------------------------------------------------------ portas */
#if FOLIO_X86
static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline u8  inb(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline u16 inw(u16 p) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline u32 inl(u16 p) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
#else   /* sem portas de E/S: tudo passa pelo EFI_PCI_IO_PROTOCOL do firmware */
static inline void outb(u16 p, u8 v) { (void)p; (void)v; }
static inline void outw(u16 p, u16 v) { (void)p; (void)v; }
static inline void outl(u16 p, u32 v) { (void)p; (void)v; }
static inline u8  inb(u16 p) { (void)p; return 0xFF; }
static inline u16 inw(u16 p) { (void)p; return 0xFFFF; }
static inline u32 inl(u16 p) { (void)p; return 0xFFFFFFFF; }
#endif

/* ------------------------------------------------------------ PCI */
enum { K_NONE, K_HDA, K_AC97 };
typedef struct { EFI_PCI_IO_PROTOCOL *io; int bus, dev, fn; u16 ven, did, sven, sdid; u32 cls; int rank, kind; } Pci;
static Pci pcis[12]; static int npcis;
static Pci *pc;                       /* controladora em uso */

static u32 cfg32(Pci *p, int off) {
    off &= ~3;
    if (p->io) { u32 v = 0xFFFFFFFF; p->io->Pci.Read(p->io, EfiPciIoWidthUint32, (u32)off, 1, &v); return v; }
    outl(0xCF8, 0x80000000u | ((u32)p->bus << 16) | ((u32)p->dev << 11) | ((u32)p->fn << 8) | (u32)off);
    return inl(0xCFC);
}
static void cfgw32(Pci *p, int off, u32 v) {
    off &= ~3;
    if (p->io) { p->io->Pci.Write(p->io, EfiPciIoWidthUint32, (u32)off, 1, &v); return; }
    outl(0xCF8, 0x80000000u | ((u32)p->bus << 16) | ((u32)p->dev << 11) | ((u32)p->fn << 8) | (u32)off);
    outl(0xCFC, v);
}
static u8 cfg8(Pci *p, int off) { return (u8)(cfg32(p, off) >> ((off & 3) * 8)); }
static void cfgw8(Pci *p, int off, u8 v) {
    if (p->io) { p->io->Pci.Write(p->io, EfiPciIoWidthUint8, (u32)off, 1, &v); return; }
    int sh = (off & 3) * 8; u32 d = cfg32(p, off); d = (d & ~(0xFFu << sh)) | ((u32)v << sh); cfgw32(p, off, d);
}
static u16 cfg16(Pci *p, int off) { return (u16)(cfg32(p, off) >> ((off & 2) * 8)); }
static void cfgw16(Pci *p, int off, u16 v) {
    if (p->io) { p->io->Pci.Write(p->io, EfiPciIoWidthUint16, (u32)off, 1, &v); return; }
    int sh = (off & 2) * 8; u32 d = cfg32(p, off); d = (d & ~(0xFFFFu << sh)) | ((u32)v << sh); cfgw32(p, off, d);
}

/* O registrador 0x08 do PCI é  classe<<24 | subclasse<<16 | prog-if<<8 | revisão.
 * Multimídia (classe 04): subclasse 01 = áudio (AC'97 ou Intel SST), 03 = HD Audio, 80 = outros (Intel SST). */
static int audio_kind(Pci *p) {
    u32 c = p->cls >> 24, s = (p->cls >> 16) & 0xFF;
    if (c != 0x04 || (s != 0x01 && s != 0x03 && s != 0x80)) return K_NONE;
    u32 bar0 = cfg32(p, 0x10);
    if (bar0 & 1) return s == 0x01 ? K_AC97 : K_NONE;      /* BAR0 de portas: AC'97 */
    return K_HDA;                                          /* BAR0 de memória: HDA (confirmado pela versão 1.0) */
}
static int rank_of(Pci *p) {
    if (p->ven == 0x10DE) return 3;          /* placas de vídeo (só HDMI) por último */
    if (p->ven == 0x1002) return 2;
    if (p->kind == K_AC97) return 1;
    if (((p->cls >> 16) & 0xFF) != 0x03) return 1;   /* SST/"outros": depois das HDA declaradas */
    return 0;
}
static void add_pci(Pci *p) {
    if (npcis >= 12) return;
    p->rank = rank_of(p);
    u32 ss = cfg32(p, 0x2C); p->sven = (u16)ss; p->sdid = (u16)(ss >> 16);
    int i = npcis++;
    while (i > 0 && pcis[i - 1].rank > p->rank) { pcis[i] = pcis[i - 1]; i--; }
    pcis[i] = *p;
}
static void consider(Pci *p) {
    if (p->ven == 0xFFFF || (p->cls >> 24) != 0x04) return;
    p->kind = audio_kind(p);
    static const char *kn[3] = { "unsupported", "HDA", "AC'97" };
    HLOG("  %02x:%02x.%d %04x:%04x classe %06x -> %s\r\n", p->bus, p->dev, p->fn, p->ven, p->did, (int)(p->cls >> 8), kn[p->kind]);
    if (p->kind) add_pci(p);
}

static void enum_pci(void) {
    EFI_HANDLE *hs; UINTN n = 0;
    bool have_io = !EFI_ERROR(efi_bs->LocateHandleBuffer(ByProtocol, &pciio_guid, 0, &n, &hs));
    if (have_io) {
        HLOG("PCI: %d devices via firmware PCI I/O; multimedia:\r\n", (int)n);
        for (UINTN i = 0; i < n; i++) {
            Pci p; memset(&p, 0, sizeof p);
            if (EFI_ERROR(efi_bs->HandleProtocol(hs[i], &pciio_guid, (void **)&p.io)) || !p.io) continue;
            UINTN seg, b, d, f;
            if (!EFI_ERROR(p.io->GetLocation(p.io, &seg, &b, &d, &f))) { p.bus = (int)b; p.dev = (int)d; p.fn = (int)f; }
            u32 id = cfg32(&p, 0); p.ven = (u16)id; p.did = (u16)(id >> 16);
            p.cls = cfg32(&p, 8);
            consider(&p);
        }
        efi_bs->FreePool(hs);
        if (npcis) return;
    }
    /* sem PCI I/O (ou nada achado): portas 0xCF8/0xCFC, mas só se houver um barramento PCI de verdade */
    void *rb;
    if (EFI_ERROR(efi_bs->LocateProtocol(&rbio_guid, 0, &rb))) { HLOG("PCI: firmware has no accessible PCI bus\r\n"); return; }
    HLOG("PCI: direct scan through ports 0xCF8; multimedia:\r\n");
    for (int b = 0; b < 256; b++)
        for (int d = 0; d < 32; d++)
            for (int f = 0; f < 8; f++) {
                Pci p; memset(&p, 0, sizeof p); p.bus = b; p.dev = d; p.fn = f;
                u32 id = cfg32(&p, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
                p.ven = (u16)id; p.did = (u16)(id >> 16); p.cls = cfg32(&p, 8);
                bool dup = false;
                for (int k = 0; k < npcis; k++) if (pcis[k].bus == b && pcis[k].dev == d && pcis[k].fn == f) dup = true;
                if (!dup) consider(&p);
                if (f == 0 && !(cfg32(&p, 0x0C) & 0x00800000)) break;
            }
}

/* liga memória/portas + bus master e põe o dispositivo em D0 */
static void pci_enable(Pci *p, bool io_space) {
    if (p->io) {
        UINT64 sup = 0;
        p->io->Attributes(p->io, EfiPciIoAttributeOperationSupported, 0, &sup);
        UINT64 want = EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER | (io_space ? EFI_PCI_IO_ATTRIBUTE_IO : 0);
        p->io->Attributes(p->io, EfiPciIoAttributeOperationEnable, sup & want, 0);
    }
    cfgw16(p, 0x04, (u16)(cfg16(p, 0x04) | 0x06 | (io_space ? 1 : 0)));
    u32 cap = (cfg16(p, 0x06) & 0x10) ? cfg8(p, 0x34) : 0;
    for (int g = 0; cap && g < 24; g++) {
        u32 h = cfg32(p, (int)cap);
        if ((h & 0xFF) == 1) {
            u16 pm = cfg16(p, (int)cap + 4);
            if (pm & 3) { HLOG("  leaving power state D%d\r\n", pm & 3); cfgw16(p, (int)cap + 4, (u16)(pm & ~3u)); pal_stall_us(10000); }
        }
        cap = (h >> 8) & 0xFC;
    }
}

/* ------------------------------------------------------------ DMA */
typedef struct { u8 *cpu; u64 dev; } Dma;
static bool dma_mapped;
static bool dma_alloc(usize bytes, Dma *d) {
    UINTN pages = (bytes + 4095) / 4096;
    if (pc->io) {
        void *host = 0;
        if (!EFI_ERROR(pc->io->AllocateBuffer(pc->io, AllocateAnyPages, EfiBootServicesData, pages, &host, 0)) && host) {
            UINTN nb = pages * 4096; EFI_PHYSICAL_ADDRESS da = 0; void *map = 0;
            if (!EFI_ERROR(pc->io->Map(pc->io, EfiPciIoOperationBusMasterCommonBuffer, host, &nb, &da, &map)) && nb >= pages * 4096) {
                memset(host, 0, pages * 4096);
                d->cpu = host; d->dev = da; dma_mapped = true;
                return true;
            }
            pc->io->FreeBuffer(pc->io, pages, host);
        }
    }
    void *a = pal_dma_alloc(bytes);
    if (!a) return false;
    d->cpu = a; d->dev = (u64)(uintptr_t)a;
    return true;
}

/* coerência de cache: alguns chipsets leem a RAM sem consultar o cache da CPU */
static bool has_clflush;
static void cpu_features(void) {
#if FOLIO_X86
    u32 a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    has_clflush = (d >> 19) & 1;
#else
    has_clflush = true;           /* ARM: limpeza por linha de cache (dc civac) sempre existe */
#endif
}
static void flush(const void *p, usize n) {
    if (!has_clflush || !n) return;
    uintptr_t s = (uintptr_t)p & ~(uintptr_t)63, e = (uintptr_t)p + n;
#if FOLIO_X86
    for (; s < e; s += 64) __asm__ volatile("clflush (%0)" : : "r"(s) : "memory");
    __asm__ volatile("mfence" ::: "memory");
#elif defined(__aarch64__)
    for (; s < e; s += 64) __asm__ volatile("dc civac, %0" : : "r"(s) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

/* ------------------------------------------------------------ estado comum */
static char info[200] = "audio not initialized yet";
static int be;                         /* backend em uso: K_HDA / K_AC97 */
static bool ready, tried, running;
static Dma ring_d, bdl_d;
static u32 ring_size = 128 * 1024;
static u32 wpos, last_pos;
static int ahead;
static int cur_rate;
static int gpio_mode = 1;            /* 0 não mexe, 1 todos em nível alto (liga amplificadores externos) */
static u32 pos_moves, stalls;

/* =============================================================================================== HDA */
static volatile u8 *mm;
static inline u8  r8(u32 o) { return *(volatile u8 *)(mm + o); }
static inline u16 r16(u32 o) { return *(volatile u16 *)(mm + o); }
static inline u32 r32(u32 o) { return *(volatile u32 *)(mm + o); }
static inline void w8(u32 o, u8 v) { *(volatile u8 *)(mm + o) = v; }
static inline void w16(u32 o, u16 v) { *(volatile u16 *)(mm + o) = v; }
static inline void w32(u32 o, u32 v) { *(volatile u32 *)(mm + o) = v; }
static bool wait_bits8(u32 o, u8 mask, u8 want, int ms) {
    for (int i = 0; i < ms * 10; i++) { if ((r8(o) & mask) == want) return true; pal_stall_us(100); }
    return false;
}

static Dma corb_d, rirb_d, posb_d;
static u32 *corb, *rirb; static int rirb_rp; static bool use_imm;
static int codec = -1, afg, conv = -1;
static u32 conv_rates;
static int sd, sd_index; static bool use_posbuf;
static int dacs[8], ndacs;

/* ------------------------------------------------------------ comandos ao codec */
static u32 cmd_imm(u32 verb) {
    for (int i = 0; i < 1000 && (r16(0x68) & 1); i++) pal_stall_us(10);
    w16(0x68, 2);
    w32(0x60, verb);
    w16(0x68, 1);
    for (int i = 0; i < 1000; i++) { if (r16(0x68) & 2) { u32 r = r32(0x64); w16(0x68, 2); return r; } pal_stall_us(10); }
    return 0xFFFFFFFF;
}
static u32 cmd(int cad, int nid, u32 verb, u32 payload) {
    u32 v = ((u32)cad << 28) | ((u32)nid << 20);
    if (verb >= 0x100) v |= (verb << 8) | (payload & 0xFF);     /* verbo de 12 bits + 8 bits */
    else v |= (verb << 16) | (payload & 0xFFFF);                 /* verbo de 4 bits + 16 bits */
    if (use_imm) return cmd_imm(v);
    w8(0x5D, 0x05);                 /* limpa RINTFL/RIRBOIS: algumas controladoras (e o QEMU) param de responder até isso */
    u16 wp = (u16)((r16(0x48) + 1) & 0xFF);
    corb[wp] = v;
    flush(&corb[wp], 4);
    w16(0x48, wp);
    for (int i = 0; i < 4000; i++) {
        int hw = r16(0x58) & 0xFF;
        if (hw != rirb_rp) {
            rirb_rp = (rirb_rp + 1) & 0xFF;
            flush(&rirb[rirb_rp * 2], 8);
            w8(0x5D, 0x05);
            return rirb[rirb_rp * 2];
        }
        pal_stall_us(5);
    }
    return 0xFFFFFFFF;
}
static u32 param(int nid, int id) { return cmd(codec, nid, 0xF00, (u32)id); }

/* ------------------------------------------------------------ caminho de saída */
static int wtype(int nid) { return (int)((param(nid, 9) >> 20) & 0xF); }
static int conn_list(int nid, int *out, int max) {
    u32 l = param(nid, 0x0E);
    int n = (int)(l & 0x7F); bool lng = (l & 0x80) != 0;
    int c = 0;
    for (int i = 0; i < n && c < max; ) {
        u32 r = cmd(codec, nid, 0xF02, (u32)i);
        int per = lng ? 2 : 4;
        for (int k = 0; k < per && i < n && c < max; k++, i++) {
            u32 e = lng ? (r >> (16 * k)) & 0xFFFF : (r >> (8 * k)) & 0xFF;
            u32 id = lng ? e & 0x7FFF : e & 0x7F;
            out[c++] = (int)id;
        }
    }
    return c;
}
static void set_amp_out(int nid) {
    u32 caps = param(nid, 9);
    if (!(caps & 4)) return;
    u32 ac = (caps & 8) ? param(nid, 0x12) : param(afg, 0x12);
    u32 off = ac & 0x7F, steps = (ac >> 8) & 0x7F;
    u32 g = off ? off : steps;
    cmd(codec, nid, 0x3, 0xB000 | g);           /* saída, esq+dir, sem mudo, 0 dB */
}
static void set_amp_in(int nid, int idx) {
    u32 caps = param(nid, 9);
    if (!(caps & 2)) return;
    u32 ac = (caps & 8) ? param(nid, 0x0D) : param(afg, 0x0D);
    u32 off = ac & 0x7F;
    cmd(codec, nid, 0x3, 0x7000 | ((u32)idx << 8) | off);
}
static void power_up(int nid) {
    if (param(nid, 9) & (1 << 10)) cmd(codec, nid, 0x705, 0);    /* D0 se o widget tem controle de energia */
}
static void add_dac(int d) { for (int i = 0; i < ndacs; i++) if (dacs[i] == d) return; if (ndacs < 8) dacs[ndacs++] = d; }
/* busca em profundidade até um conversor de saída; liga e desmuta tudo no caminho */
static int find_dac(int nid, int depth) {
    if (depth > 7) return -1;
    int list[32]; int n = conn_list(nid, list, 32);
    int nt = wtype(nid);
    for (int pass = 0; pass < 2; pass++)            /* prefere DAC direto a passar por mixers */
        for (int i = 0; i < n; i++) {
            int c = list[i], t = wtype(c);
            int dac = -1;
            if (t == 0 && pass == 0) dac = c;
            else if ((t == 2 || t == 3) && pass == 1) dac = find_dac(c, depth + 1);
            if (dac < 0) continue;
            if ((nt == 4 || nt == 3) && n > 1) cmd(codec, nid, 0x701, (u32)i);     /* seleciona a entrada */
            if (nt == 2) set_amp_in(nid, i);
            power_up(c);
            set_amp_out(c);
            if (t == 0) { cmd(codec, c, 0x70C, 0x02); }                         /* EAPD em DACs que o tenham (IDT) */
            HLOG(" %d", c);
            return dac;
        }
    return -1;
}

static const char *devname(int devt) {
    static const char *n[16] = { "line out", "speaker", "headphones", "CD", "S/PDIF", "digital", "modem", "handset",
                                 "line in", "aux", "microphone", "telephony", "S/PDIF in", "digital in", "reserved", "other" };
    return n[devt & 15];
}
static const char *connname(int c) { static const char *n[4] = { "jack", "none", "fixed/internal", "both" }; return n[c & 3]; }

static bool setup_codec(int cad) {
    codec = cad; conv = -1; ndacs = 0;
    u32 vid = param(0, 0), rev = param(0, 2);
    HLOG("  codec %d: %04x:%04x rev %x", cad, (int)(vid >> 16), (int)(vid & 0xFFFF), (int)rev);
    u32 sub = param(0, 4);
    int start = (int)((sub >> 16) & 0xFF), cnt = (int)(sub & 0xFF);
    afg = -1;
    for (int i = 0; i < cnt && i < 16; i++) if ((param(start + i, 5) & 0xFF) == 1) { afg = start + i; break; }
    if (afg < 0) { HLOG(" · no audio function group\r\n"); return false; }
    cmd(codec, afg, 0x7FF, 0);                       /* reset do grupo de funções (volta aos padrões) */
    pal_stall_us(1000);
    cmd(codec, afg, 0x705, 0);
    for (int i = 0; i < 50 && ((cmd(codec, afg, 0xF05, 0) >> 4) & 0xF) != 0; i++) pal_stall_us(2000);
    u32 ssid = cmd(codec, afg, 0xF20, 0);
    u32 gp = param(afg, 0x11); int ngpio = (int)(gp & 0xFF);
    HLOG(" · AFG %d · subsystem %08x · %d GPIO\r\n", afg, (int)ssid, ngpio);
    if (ngpio > 0 && gpio_mode) {
        u32 mask = ngpio >= 8 ? 0xFF : (1u << ngpio) - 1;
        cmd(codec, afg, 0x716, mask); cmd(codec, afg, 0x717, mask); cmd(codec, afg, 0x715, mask);
        HLOG("    GPIO 0..%d on (high level)\r\n", ngpio - 1);
    }
    u32 ws = param(afg, 4);
    int ws0 = (int)((ws >> 16) & 0xFF), wn = (int)(ws & 0xFF);
    /* inventário dos pinos */
    for (int i = 0; i < wn; i++) {
        int nid = ws0 + i; u32 wc = param(nid, 9); int t = (int)((wc >> 20) & 0xF);
        if (t != 4) continue;
        u32 pcap = param(nid, 0x0C), cfg = cmd(codec, nid, 0xF1C, 0);
        int list[32]; int n = conn_list(nid, list, 32);
        HLOG("    pin %d: config %08x (%s, %s) · caps %08x%s%s · connections", nid, (int)cfg, devname((int)(cfg >> 20) & 0xF),
          connname((int)(cfg >> 30)), (int)pcap, (pcap & 0x10) ? " out" : "", (wc & (1 << 9)) ? " digital" : "");
        for (int k = 0; k < n; k++) HLOG(" %d", list[k]);
        HLOG("\r\n");
    }
    int found = 0; char kinds[96] = "";
    /* passada 0: alto-falante e fone; 1: saída de linha (caixas no PC de mesa); 2: qualquer pino de saída analógico (BIOS mal configurada) */
    for (int pass = 0; pass < 3 && !(pass == 2 && found); pass++) {
        for (int i = 0; i < wn; i++) {
            int nid = ws0 + i;
            u32 wc = param(nid, 9);
            if (((wc >> 20) & 0xF) != 4 || (wc & (1 << 9))) continue;         /* só pinos analógicos */
            u32 pcap = param(nid, 0x0C);
            if (!(pcap & 0x10)) continue;                                     /* não é saída */
            u32 cfg = cmd(codec, nid, 0xF1C, 0);
            int connty = (int)(cfg >> 30), devt = (int)((cfg >> 20) & 0xF);
            bool want;
            if (pass == 0) want = connty != 1 && (devt == 1 || devt == 2);
            else if (pass == 1) want = connty != 1 && devt == 0;
            else want = devt <= 2 || connty == 1;
            if (!want) continue;
            HLOG("    caminho %d ->", nid);
            int dac = find_dac(nid, 0);
            if (dac < 0) { HLOG(" (no DAC)\r\n"); continue; }
            HLOG("\r\n");
            add_dac(dac);
            if (conv < 0) conv = dac;
            power_up(nid);
            cmd(codec, nid, 0x705, 0);
            cmd(codec, nid, 0x707, 0x40 | (devt == 2 ? 0x80 : 0));
            if (pcap & 0x10000) cmd(codec, nid, 0x70C, 0x02);   /* EAPD (amplificador externo) */
            set_amp_out(nid);
            found++;
            str_cat(kinds, " ", sizeof kinds); str_cat(kinds, pass == 2 ? "out" : devname(devt), sizeof kinds);
        }
    }
    /* EAPD em todos os pinos que o suportam (alguns codecs ligam o amplificador por pinos não usados) */
    for (int i = 0; i < wn; i++) { int nid = ws0 + i; if (wtype(nid) == 4 && (param(nid, 0x0C) & 0x10000)) cmd(codec, nid, 0x70C, 0x02); }
    if (conv < 0) { HLOG("    no usable analog output\r\n"); return false; }
    conv_rates = param(conv, 0x0A);
    fmt(info, sizeof info, "HDA %04x:%04x · codec %04x:%04x ·%s%s", pc->ven, pc->did, (int)(vid >> 16), (int)(vid & 0xFFFF), kinds,
        dma_mapped ? "" : " · DMA direto");
    return true;
}

/* Capacidades estendidas (Intel Skylake e posteriores, inclusive Smart Sound em modo HDA):
 *  - multi-link: o firmware pode deixar o link HDA desligado ou com o clock errado (6 MHz) — sem isso
 *    nenhum codec aparece. Mesma correção do Linux (intel_init_lctl).
 *  - processing pipe: se o DMA estiver desviado para o DSP (GPROCEN), a controladora toca silêncio. */
static void hda_ext_caps(bool after_reset) {
    u32 off = r16(0x14);
    for (int g = 0; off && off < 0x4000 && g < 16; g++) {
        u32 h = r32(off);
        if (h == 0xFFFFFFFF) break;
        int id = (int)((h >> 16) & 0xFFF);
        if (id == 1 && !after_reset) {                                    /* multi-link */
            u32 lcap = r32(off + 0x40), lctl = r32(off + 0x44);
            HLOG("  multi-link: LCAP %08x LCTL %08x\r\n", (int)lcap, (int)lctl);
            bool up = (lctl & (1u << 23)) != 0; u32 scf = lctl & 0xF;
            if (!up || !scf) {
                static const int pref[] = { 2, 3, 1, 4, 5 };
                if (!scf) for (int i = 0; i < 5; i++) if (lcap & (1u << pref[i])) { scf = (u32)pref[i]; break; }
                u32 v = lctl & ~(1u << 16); w32(off + 0x44, v);
                for (int i = 0; i < 100 && (r32(off + 0x44) & (1u << 23)); i++) pal_stall_us(20);
                v = (v & ~0xFu) | scf; w32(off + 0x44, v);
                v |= 1u << 16; w32(off + 0x44, v);
                for (int i = 0; i < 100 && !(r32(off + 0x44) & (1u << 23)); i++) pal_stall_us(20);
                HLOG("  HDA link powered on again (clock %d) · LCTL %08x\r\n", (int)scf, (int)r32(off + 0x44));
            }
        } else if (id == 3 && after_reset) {                              /* processing pipe */
            u32 ppctl = r32(off + 0x04);
            if (ppctl & (1u << 30)) { w32(off + 0x04, ppctl & ~(1u << 30)); HLOG("  DMA routed from the DSP to the controller (GPROCEN)\r\n"); }
        }
        off = h & 0xFFFF;
    }
}

static bool bad(u32 v) { return v == 0xFFFFFFFF || v == 0; }

static bool init_controller(void) {
    Pci *p = pc;
    HLOG("HDA controller %02x:%02x.%d %04x:%04x class %06x subsystem %04x:%04x\r\n", p->bus, p->dev, p->fn, p->ven, p->did,
      (int)(p->cls >> 8), p->sven, p->sdid);
    pci_enable(p, false);
    u32 bar = cfg32(p, 0x10);
    u64 base = bar & ~0xFull;
    if ((bar & 6) == 4) base |= (u64)cfg32(p, 0x14) << 32;
    if (!base || (bar & 1)) { HLOG("  invalid BAR0 (%08x)\r\n", (int)bar); return false; }
#if !defined(__x86_64__)
    if (base >> 32) { HLOG("  BAR0 above 4 GB (not reachable in 32-bit mode)\r\n"); return false; }
#endif
    /* ajustes por fabricante (os mesmos do Linux) */
    if (p->ven == 0x8086) {
        cfgw8(p, 0x44, (u8)(cfg8(p, 0x44) & ~7));                   /* TCSEL = TC0: evita chiado/silêncio */
        u16 devc = cfg16(p, 0x78);
        if (devc & 0x0800) cfgw16(p, 0x78, (u16)(devc & ~0x0800));   /* snoop ligado */
    } else if (p->ven == 0x1002 || p->ven == 0x1022) {
        cfgw8(p, 0x42, (u8)((cfg8(p, 0x42) & 0xF8) | 0x02));         /* ATI/AMD: snoop */
    } else if (p->ven == 0x10DE) {
        cfgw8(p, 0x4E, (u8)((cfg8(p, 0x4E) & 0xF0) | 0x0F));         /* NVIDIA: coerência */
    } else if (p->ven == 0x1106) {
        cfgw8(p, 0x41, (u8)(cfg8(p, 0x41) | 0x40));                  /* VIA: snoop */
    }
    mm = (volatile u8 *)(uintptr_t)base;
    if (r32(0x08) == 0xFFFFFFFF) { HLOG("  MMIO not responding\r\n"); return false; }
    if (r8(0x03) != 1) {
        HLOG("  not an HDA controller (version %d.%d)%s\r\n", r8(0x03), r8(0x02),
          p->ven == 0x8086 ? " — probably an Intel DSP without an HDA codec (SoundWire/I2S)" : "");
        return false;
    }
    if (p->ven == 0x8086) hda_ext_caps(false);
    /* para DMA de CORB/RIRB e streams antigos */
    w8(0x4C, 0); w8(0x5C, 0);
    u16 gcap = r16(0x00);
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF, bss = (gcap >> 3) & 0x1F;
    for (int s = 0; s < iss + oss + bss && s < 30; s++) w8(0x80 + 0x20 * (u32)s, 0);
    w32(0x70, 0); w32(0x74, 0);
    w16(0x0E, 0xFFFF);
    /* reset da controladora */
    w32(0x08, r32(0x08) & ~1u);
    if (!wait_bits8(0x08, 1, 0, 100)) HLOG("  reset did not assert (continuing anyway)\r\n");
    pal_stall_us(1000);
    w32(0x08, r32(0x08) | 1);
    if (!wait_bits8(0x08, 1, 1, 200)) { HLOG("  reset did not deassert\r\n"); return false; }
    pal_stall_us(2000);
    u16 states = 0;
    for (int i = 0; i < 600 && !states; i++) { states = r16(0x0E); pal_stall_us(1000); }
    if (p->ven == 0x8086) hda_ext_caps(true);
    gcap = r16(0x00);
    HLOG("  GCAP %04x (outputs %d, inputs %d, bidirectional %d, 64-bit %d) · version %d.%d · codecs %04x\r\n", gcap, (gcap >> 12) & 0xF,
      (gcap >> 8) & 0xF, (gcap >> 3) & 0x1F, gcap & 1, r8(0x03), r8(0x02), states);
    bool guessed = false;
    if (!states) { HLOG("  no codec announced itself: trying addresses 0 to 3 anyway\r\n"); states = 0x000F; guessed = true; }
    w32(0x20, 0);                                   /* sem interrupções */
    /* CORB / RIRB */
    if (!corb_d.cpu && (!dma_alloc(4096, &corb_d) || !dma_alloc(4096, &rirb_d) || !dma_alloc(4096, &posb_d))) { HLOG("  no DMA memory\r\n"); return false; }
    corb = (u32 *)corb_d.cpu; rirb = (u32 *)rirb_d.cpu;
    HLOG("  DMA %s (CORB at %x:%08x)\r\n", dma_mapped ? "mapped by the firmware" : "direct", (int)(corb_d.dev >> 32), (int)corb_d.dev);
    w32(0x40, (u32)corb_d.dev); w32(0x44, (u32)(corb_d.dev >> 32));
    u8 cs = r8(0x4E); w8(0x4E, (u8)((cs & 0xFC) | ((cs & 0x40) ? 2 : (cs & 0x20) ? 1 : 0)));
    w16(0x4A, 0x8000); for (int i = 0; i < 100 && !(r16(0x4A) & 0x8000); i++) pal_stall_us(10);
    w16(0x4A, 0); for (int i = 0; i < 100 && (r16(0x4A) & 0x8000); i++) pal_stall_us(10);
    w16(0x48, 0);
    w32(0x50, (u32)rirb_d.dev); w32(0x54, (u32)(rirb_d.dev >> 32));
    u8 rs = r8(0x5E); w8(0x5E, (u8)((rs & 0xFC) | ((rs & 0x40) ? 2 : (rs & 0x20) ? 1 : 0)));
    w16(0x58, 0x8000); pal_stall_us(100);
    w16(0x5A, 1);
    /* RIRB: DMA + RINTCTL. Com INTCTL=0 nenhuma interrupção chega à CPU, mas o contador de respostas
     * (RINTCNT) só recomeça quando RINTFL é sinalizado e limpo; sem RINTCTL, algumas controladoras
     * (e o QEMU) respondem ao primeiro comando e param */
    w8(0x4C, 2); w8(0x5C, 3);
    pal_stall_us(1000);
    rirb_rp = r16(0x58) & 0xFF;
    use_imm = false;
    /* buffer de posição: a controladora escreve ali onde cada stream está (alternativa ao LPIB) */
    memset(posb_d.cpu, 0, 4096); flush(posb_d.cpu, 4096);
    w32(0x74, (u32)(posb_d.dev >> 32)); w32(0x70, (u32)posb_d.dev | 1);
    /* codecs: analógicos primeiro; HDMI (Intel 8086, NVIDIA 10de, AMD 1002) só se não houver outro */
    for (int pass = 0; pass < 2; pass++)
        for (int c = 0; c < 15; c++) {
            if (!(states & (1 << c))) continue;
            codec = c;
            u32 vid = param(0, 0);
            if (bad(vid) && !use_imm) {                     /* CORB não respondeu: tenta comandos imediatos */
                w8(0x4C, 0); w8(0x5C, 0); use_imm = true;
                vid = param(0, 0);
                if (bad(vid)) { use_imm = false; w8(0x4C, 2); w8(0x5C, 3); pal_stall_us(500); rirb_rp = r16(0x58) & 0xFF; }
                else if (pass == 0) HLOG("  CORB/RIRB not responding: using immediate commands\r\n");
            }
            if (bad(vid)) { if (pass == 0 && !guessed) HLOG("  codec %d not responding\r\n", c); continue; }
            u16 vv = (u16)(vid >> 16);
            bool hdmi = vv == 0x8086 || vv == 0x10DE || vv == 0x1002;
            if ((pass == 0) == hdmi) continue;
            if (setup_codec(c)) return true;
        }
    str_copy(info, "no analog output found on the codec", sizeof info);
    return false;
}

static u16 fmt_word(int rate) {
    u16 base = rate == 44100 ? 0x4000 : 0;
    return (u16)(base | (1 << 4) | 1);   /* 16 bits, 2 canais */
}
static u32 hda_lpib(void) { return r32((u32)sd + 0x04); }
static u32 hda_posbuf(void) {
    volatile u32 *pb = (volatile u32 *)(posb_d.cpu + sd_index * 8);
    flush((const void *)pb, 8);
    return *pb;
}

/* programa e liga um stream; devolve true se o DMA andar */
static bool hda_try_stream(int index, bool bidir, int r) {
    sd_index = index; sd = 0x80 + 0x20 * index;
    u8 *ring = ring_d.cpu; u32 *bdl = (u32 *)bdl_d.cpu;
    memset(ring, 0, ring_size); flush(ring, ring_size);
    w8((u32)sd, r8((u32)sd) & ~2);
    w8((u32)sd, r8((u32)sd) | 1); wait_bits8((u32)sd, 1, 1, 10);
    w8((u32)sd, r8((u32)sd) & ~1); wait_bits8((u32)sd, 1, 0, 10);
    int nent = 8; u32 chunk = ring_size / (u32)nent;
    for (int i = 0; i < nent; i++) {
        u64 a = ring_d.dev + chunk * (u32)i;
        bdl[i * 4 + 0] = (u32)a; bdl[i * 4 + 1] = (u32)(a >> 32); bdl[i * 4 + 2] = chunk; bdl[i * 4 + 3] = 0;
    }
    flush(bdl, 4096);
    w32((u32)sd + 0x18, (u32)bdl_d.dev); w32((u32)sd + 0x1C, (u32)(bdl_d.dev >> 32));
    w32((u32)sd + 0x08, ring_size);
    w16((u32)sd + 0x0C, (u16)(nent - 1));
    w16((u32)sd + 0x12, fmt_word(r));
    w8((u32)sd + 2, (u8)((r8((u32)sd + 2) & 0x07) | (1 << 4) | (bidir ? 0x08 : 0)));   /* tag 1; DIR=saída se bidirecional */
    for (int i = 0; i < ndacs; i++) {
        power_up(dacs[i]);
        cmd(codec, dacs[i], 0x2, fmt_word(r));
        cmd(codec, dacs[i], 0x706, 1 << 4);
    }
    w8((u32)sd + 3, 0x1C);                                           /* limpa status */
    w8((u32)sd, r8((u32)sd) | 2);                                    /* RUN */
    u32 l0 = hda_lpib(), p0 = hda_posbuf();
    pal_stall_us(40000);
    u32 l1 = hda_lpib(), p1 = hda_posbuf();
    use_posbuf = false;
    if (l0 != l1) { HLOG("stream %d at %d Hz: DMA ok (%d bytes in 40 ms)\r\n", index, r, (int)((l1 - l0 + ring_size) % ring_size)); return true; }
    if (p0 != p1) { use_posbuf = true; HLOG("stream %d at %d Hz: DMA ok through the position buffer (LPIB stuck)\r\n", index, r); return true; }
    stalls++;
    HLOG("stream %d: read position does not advance · status %02x\r\n", index, r8((u32)sd + 3));
    w8((u32)sd, r8((u32)sd) & ~2);
    return false;
}

static bool hda_start(int rate) {
    int r = 48000;
    if (rate == 44100 && (conv_rates & (1 << 5))) r = 44100;
    else if (!(conv_rates & (1 << 6)) && (conv_rates & (1 << 5))) r = 44100;
    cur_rate = r;
    u16 gcap = r16(0x00);
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF, bss = (gcap >> 3) & 0x1F;
    if (!oss && !bss) { str_copy(info, "controller without an output stream", sizeof info); return false; }
    static int good = -1;                    /* stream que funcionou da última vez */
    if (good >= 0) { bool bd = good >= iss + oss; if (hda_try_stream(good, bd, r)) return true; }
    for (int i = 0; i < oss + bss && i < 16; i++) {
        int idx = i < oss ? iss + i : iss + oss + (i - oss);
        if (idx == good) continue;
        if (hda_try_stream(idx, i >= oss, r)) { good = idx; return true; }
    }
    str_copy(info, "the HDA controller does not read audio from memory (DMA blocked)", sizeof info);
    return false;
}
static u32 hda_pos(void) { return (use_posbuf ? hda_posbuf() : hda_lpib()) % ring_size; }
static void hda_stop(void) { w8((u32)sd, r8((u32)sd) & ~2); wait_bits8((u32)sd, 2, 0, 10); }

/* ============================================================================================ AC'97 */
static u32 nam, nabm; static bool ac_sis, ac_vra;
#define AC_CHUNKS 32
static u32 ac_io(int bar, u32 off, int w, bool wr, u32 v) {
    if (pc->io) {
        EFI_PCI_IO_PROTOCOL_WIDTH wd = w == 1 ? EfiPciIoWidthUint8 : w == 2 ? EfiPciIoWidthUint16 : EfiPciIoWidthUint32;
        if (wr) { pc->io->Io.Write(pc->io, wd, (UINT8)bar, off, 1, &v); return 0; }
        u32 r = 0; pc->io->Io.Read(pc->io, wd, (UINT8)bar, off, 1, &r); return r;
    }
    u16 port = (u16)((bar ? nabm : nam) + off);
    if (wr) { if (w == 1) outb(port, (u8)v); else if (w == 2) outw(port, (u16)v); else outl(port, v); return 0; }
    return w == 1 ? inb(port) : w == 2 ? inw(port) : inl(port);
}
#define MIX_R(o)     ((u16)ac_io(0, o, 2, false, 0))
#define MIX_W(o, v)  ac_io(0, o, 2, true, v)
#define BM_R8(o)     ((u8)ac_io(1, o, 1, false, 0))
#define BM_W8(o, v)  ac_io(1, o, 1, true, v)
#define BM_R16(o)    ((u16)ac_io(1, o, 2, false, 0))
#define BM_W16(o, v) ac_io(1, o, 2, true, v)
#define BM_R32(o)    ac_io(1, o, 4, false, 0)
#define BM_W32(o, v) ac_io(1, o, 4, true, v)
#define PO_SR   (ac_sis ? 0x18u : 0x16u)
#define PO_PICB (ac_sis ? 0x16u : 0x18u)

static bool ac97_init(void) {
    Pci *p = pc;
    HLOG("AC'97 controller %02x:%02x.%d %04x:%04x subsystem %04x:%04x\r\n", p->bus, p->dev, p->fn, p->ven, p->did, p->sven, p->sdid);
    pci_enable(p, true);
    if (p->ven == 0x8086) cfgw8(p, 0x41, (u8)(cfg8(p, 0x41) | 1));     /* ICH4+: liga as portas legadas */
    nam = cfg32(p, 0x10) & ~3u; nabm = cfg32(p, 0x14) & ~3u;
    if (!nam || !nabm || !(cfg32(p, 0x14) & 1)) { HLOG("  invalid ports\r\n"); return false; }
    ac_sis = p->ven == 0x1039 && p->did == 0x7012;
    if (p->ven == 0x10B9) { HLOG("  ALi M5455 not supported\r\n"); return false; }
    /* reset do link: frio se ainda em reset, morno caso contrário (como o Linux) */
    u32 gc = BM_R32(0x2C);
    gc &= ~((3u << 20) | (3u << 22) | 8u);               /* 2 canais, 16 bits, link ligado */
    if (gc & 2) { BM_W32(0x2C, gc | 4); for (int i = 0; i < 100 && (BM_R32(0x2C) & 4); i++) pal_stall_us(1000); }
    else BM_W32(0x2C, gc | 2);
    int t = 0;
    for (; t < 600 && !(BM_R32(0x30) & 0x100); t++) pal_stall_us(1000);
    if (!(BM_R32(0x30) & 0x100)) {                       /* tenta um reset frio completo */
        BM_W32(0x2C, gc & ~2u); pal_stall_us(1000); BM_W32(0x2C, gc | 2);
        for (t = 0; t < 1000 && !(BM_R32(0x30) & 0x100); t++) pal_stall_us(1000);
    }
    HLOG("  GLOB_STA %08x\r\n", (int)BM_R32(0x30));
    MIX_W(0x00, 0);                                      /* reset dos registradores do codec */
    for (t = 0; t < 1000 && (MIX_R(0x26) & 0xF) != 0xF; t++) pal_stall_us(1000);
    u16 v1 = MIX_R(0x7C), v2 = MIX_R(0x7E);
    if (v1 == 0xFFFF && v2 == 0xFFFF) { HLOG("  AC'97 codec not responding\r\n"); str_copy(info, "AC'97 without codec", sizeof info); return false; }
    HLOG("  codec %04x:%04x · energia %04x · recursos %04x\r\n", v1, v2, MIX_R(0x26), MIX_R(0x28));
    MIX_W(0x26, 0);                                      /* liga tudo, inclusive o amplificador externo (EAPD) */
    MIX_W(0x02, 0x0000);                                 /* volume geral: máximo, sem mudo */
    MIX_W(0x04, 0x0000);                                 /* fone */
    MIX_W(0x06, 0x0000);                                 /* mono */
    MIX_W(0x18, 0x0808);                                 /* PCM: 0 dB */
    MIX_W(0x38, 0x0000);                                 /* surround (alguns codecs ligam a saída por aqui) */
    ac_vra = (MIX_R(0x28) & 1) != 0;
    char vn[5] = { (char)(v1 >> 8), (char)v1, (char)(v2 >> 8), 0, 0 };
    for (int i = 0; i < 3; i++) if (vn[i] < 32 || vn[i] > 126) vn[i] = '?';
    fmt(info, sizeof info, "AC'97 %04x:%04x · codec %s%d%s%s", p->ven, p->did, vn, v2 & 0xFF, ac_vra ? " · variable rate" : "",
        dma_mapped ? "" : " · DMA direto");
    return true;
}

static bool ac97_start(int rate) {
    int r = 48000;
    if (ac_vra) {
        MIX_W(0x2A, MIX_R(0x2A) | 1);
        MIX_W(0x2C, (u16)rate);
        pal_stall_us(1000);
        int got = MIX_R(0x2C);
        r = got >= 8000 ? got : 48000;
    }
    cur_rate = r;
    BM_W8(0x1B, 0); BM_W8(0x1B, 2);
    for (int i = 0; i < 100 && (BM_R8(0x1B) & 2); i++) pal_stall_us(100);
    u8 *ring = ring_d.cpu; u32 *bdl = (u32 *)bdl_d.cpu;
    memset(ring, 0, ring_size); flush(ring, ring_size);
    u32 chunk = ring_size / AC_CHUNKS;
    for (int i = 0; i < AC_CHUNKS; i++) {
        bdl[i * 2] = (u32)(ring_d.dev + chunk * (u32)i);
        bdl[i * 2 + 1] = ac_sis ? chunk : chunk / 2;    /* amostras de 16 bits (SiS: bytes); sem interrupção */
    }
    flush(bdl, 4096);
    BM_W32(0x10, (u32)bdl_d.dev);
    BM_W8(0x15, AC_CHUNKS - 1);
    BM_W16(PO_SR, 0x1C);
    BM_W8(0x1B, 1);
    u32 a = BM_R8(0x14) * 4096u + BM_R16(PO_PICB); pal_stall_us(40000); u32 b = BM_R8(0x14) * 4096u + BM_R16(PO_PICB);
    if (a == b) { stalls++; HLOG("AC'97: DMA is stuck (status %04x)\r\n", BM_R16(PO_SR)); str_copy(info, "AC'97: DMA blocked", sizeof info); return false; }
    HLOG("AC'97 at %d Hz: DMA ok\r\n", r);
    return true;
}
static u32 ac97_pos(void) {
    u32 chunk = ring_size / AC_CHUNKS;
    u8 civ; u32 picb;
    for (int k = 0; k < 3; k++) {
        civ = BM_R8(0x14) & (AC_CHUNKS - 1); picb = BM_R16(PO_PICB);
        if ((BM_R8(0x14) & (AC_CHUNKS - 1)) == civ) break;
    }
    u32 rem = ac_sis ? picb : picb * 2; if (rem > chunk) rem = chunk;
    BM_W8(0x15, (u8)((civ + AC_CHUNKS - 1) & (AC_CHUNKS - 1)));       /* nunca chega ao fim da lista */
    if (BM_R16(PO_SR) & 1) { BM_W16(PO_SR, 0x1C); BM_W8(0x1B, 1); }     /* parou mesmo assim: religa */
    return (civ * chunk + (chunk - rem)) % ring_size;
}
static void ac97_stop(void) { BM_W8(0x1B, 0); }

/* ======================================================================================= interface */
void pal_audio_probe(void) { hda_probe(); }
void hda_probe(void) {
    if (tried) return;
    tried = true;
    cpu_features();
    HLOG("== Audio\r\n");
    str_copy(info, "no sound card (HDA or AC'97) found", sizeof info);
    enum_pci();
    if (!npcis) {
        HLOG("no HDA/AC'97 sound card in this computer (USB, Bluetooth and pure SoundWire/I2S audio are not supported)\r\n");
        return;
    }
    for (int i = 0; i < npcis; i++) {
        pc = &pcis[i];
        corb_d.cpu = 0; ring_d.cpu = 0; dma_mapped = false;   /* cada controladora tem seu próprio mapeamento de DMA */
        bool ok = pc->kind == K_AC97 ? ac97_init() : init_controller();
        if (!ok) continue;
        be = pc->kind;
        if (!dma_alloc(ring_size, &ring_d) || !dma_alloc(4096, &bdl_d)) { HLOG("  no memory for the sound buffer\r\n"); continue; }
        ready = true; HLOG("=> in use: %s\r\n", info);
        return;
    }
    pc = 0;
    HLOG("=> no usable controller: %s\r\n", info);
}

bool pal_audio_start(int rate) {
    if (!tried) hda_probe();
    if (!ready) return false;
    pal_audio_stop();
    bool ok = be == K_AC97 ? ac97_start(rate) : hda_start(rate);
    if (!ok) return false;
    ahead = 4096;
    last_pos = be == K_AC97 ? ac97_pos() : hda_pos();
    wpos = (last_pos + 4096) % ring_size;
    running = true;
    return true;
}
int pal_audio_rate(void) { return cur_rate; }

int pal_audio_space(void) {
    if (!running) return 0;
    u8 *ring = ring_d.cpu;
    u32 pos = be == K_AC97 ? ac97_pos() : hda_pos();
    /* zera o que já tocou: num atraso, toca silêncio em vez de repetir áudio velho */
    u32 p = last_pos;
    while (p != pos) {
        u32 end = pos > p ? pos : ring_size;
        memset(ring + p, 0, end - p); flush(ring + p, end - p);
        p = end % ring_size;
    }
    int consumed = (int)((pos - last_pos + ring_size) % ring_size);
    if (consumed) pos_moves++;
    last_pos = pos;
    ahead -= consumed;
    if (ahead < 0) {            /* atraso: o hardware passou do ponto de escrita — recomeça logo à frente */
        wpos = (pos + 2048) % ring_size;
        ahead = 2048;
    }
    int free = (int)ring_size - ahead - 8192;
    return free > 0 ? free / 4 : 0;
}
void pal_audio_write(const i16 *fr, int n) {
    if (!running) return;
    u8 *ring = ring_d.cpu;
    const u8 *s = (const u8 *)fr; u32 bytes = (u32)n * 4;
    ahead += (int)bytes;
    while (bytes) {
        u32 k = MIN(bytes, ring_size - wpos);
        memcpy(ring + wpos, s, k); flush(ring + wpos, k);
        wpos = (wpos + k) % ring_size; s += k; bytes -= k;
    }
}
void pal_audio_stop(void) {
    if (!running) return;
    if (be == K_AC97) ac97_stop(); else hda_stop();
    running = false;
}
const char *pal_audio_info(void) { return info; }
void pal_audio_set_gpio(int on) { gpio_mode = on; }

void pal_diag_audio(Buf *o) {
    if (dlog.s) buf_put(o, dlog.s, dlog.len);
    else buf_puts(o, "== Audio\r\nnot initialized yet\r\n");
    buf_printf(o, "state: %s · playing %d · DMA advances %d · stalls %d · position via %s · clflush %d\r\n",
               info, running, (int)pos_moves, (int)stalls, be == K_AC97 ? "CIV/PICB" : use_posbuf ? "buffer" : "LPIB", has_clflush);
}
