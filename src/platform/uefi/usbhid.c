/* FolioOS — driver próprio de mouse, tablet e tela de toque USB (classe HID)
 *
 * Muitos firmwares não trazem driver de mouse (o OVMF, vários PCs com "fast boot"), mas todo
 * firmware UEFI expõe cada dispositivo USB pelo EFI_USB_IO_PROTOCOL. Este driver:
 *   1. procura interfaces HID (classe 3) que o firmware não transformou em ponteiro;
 *   2. lê o "report descriptor" e descobre onde ficam X, Y, roda e botões (relativos ou absolutos);
 *   3. recebe os relatórios por transferência de interrupção assíncrona;
 *   4. publica cada dispositivo como um EFI_SIMPLE_POINTER (relativo) ou EFI_ABSOLUTE_POINTER
 *      (tablet, tela de toque) só nosso, que a camada de entrada lê como qualquer outro ponteiro.
 * Teclados HID ficam com o firmware (que sempre tem driver de teclado). */
#include "folio.h"
#include "efi.h"

extern EFI_BOOT_SERVICES *efi_bs;

static EFI_GUID usbio_guid = { 0x2B2F68D6, 0x0CD2, 0x44CF, { 0x8E, 0x8B, 0xBB, 0xA2, 0x0B, 0x1B, 0x5B, 0x75 } };
static EFI_GUID sptr_g = { 0x31878c87, 0x0b75, 0x11d5, { 0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };
static EFI_GUID aptr_g = { 0x8d59d32b, 0xc655, 0x4ae9, { 0x9b, 0x15, 0xf2, 0x59, 0x04, 0x99, 0x2a, 0x43 } };

#pragma pack(push, 1)
typedef struct { UINT8 RequestType, Request; UINT16 Value, Index, Length; } USB_REQ;
typedef struct { UINT8 Length, DescriptorType, InterfaceNumber, AlternateSetting, NumEndpoints, InterfaceClass, InterfaceSubClass, InterfaceProtocol, Interface; } USB_IFACE;
typedef struct { UINT8 Length, DescriptorType, EndpointAddress, Attributes; UINT16 MaxPacketSize; UINT8 Interval; } USB_EP;
#pragma pack(pop)
typedef EFI_STATUS (EFIAPI *USB_ASYNC_CB)(void *data, UINTN len, void *ctx, UINT32 status);
typedef struct USB_IO {
    EFI_STATUS (EFIAPI *ControlTransfer)(struct USB_IO *, USB_REQ *, UINT32 dir, UINT32 timeout, void *data, UINTN len, UINT32 *status);
    void *BulkTransfer;
    EFI_STATUS (EFIAPI *AsyncInterruptTransfer)(struct USB_IO *, UINT8 ep, BOOLEAN is_new, UINTN interval, UINTN len, USB_ASYNC_CB cb, void *ctx);
    void *SyncInterruptTransfer, *IsochronousTransfer, *AsyncIsochronousTransfer, *GetDeviceDescriptor, *GetConfigDescriptor;
    EFI_STATUS (EFIAPI *GetInterfaceDescriptor)(struct USB_IO *, USB_IFACE *);
    EFI_STATUS (EFIAPI *GetEndpointDescriptor)(struct USB_IO *, UINT8 index, USB_EP *);
    void *GetStringDescriptor, *GetSupportedLanguages, *PortReset;
} USB_IO;

/* um campo de um relatório HID: posição em bits, tamanho e faixa lógica */
typedef struct { int report_id, bit, size; i32 lmin, lmax; bool rel, used; } Field;
typedef struct UsbPtr {
    EFI_SIMPLE_POINTER_PROTOCOL sp; EFI_SIMPLE_POINTER_MODE spm;     /* o primeiro campo: o ponteiro é o próprio dispositivo */
    EFI_ABSOLUTE_POINTER_PROTOCOL ap; EFI_ABSOLUTE_POINTER_MODE apm;
    USB_IO *io; EFI_HANDLE h; u8 ep; int maxp;
    Field x, y, wheel, btn[3];
    bool absolute, has_ids;
    volatile i32 dx, dy, dz; volatile u32 ax, ay, buttons, latch;   /* latch: botões apertados desde a última leitura (um clique rápido não se perde) */ volatile u32 reports, changed;
    char kind[48];
} UsbPtr;
static UsbPtr devs[8]; static int ndevs;
static EFI_HANDLE seen[32]; static int nseen;

/* ------------------------------------------------------------ report descriptor (HID 1.11, seção 6.2.2) */
static void parse_report_desc(UsbPtr *d, const u8 *r, int len) {
    u32 page = 0, usages[16], umin = 0, umax = 0; int nus = 0;
    i32 lmin = 0, lmax = 0; int rsize = 0, rcount = 0, rid = 0;
    int bitpos[256]; memset(bitpos, 0, sizeof bitpos);
    for (int i = 0; i < len; ) {
        u8 pfx = r[i++];
        if (pfx == 0xFE) { if (i + 1 < len) i += 2 + r[i]; continue; }          /* item longo: ignora */
        int sz = (pfx & 3) == 3 ? 4 : (pfx & 3), type = (pfx >> 2) & 3, tag = pfx >> 4;
        if (i + sz > len) break;
        u32 v = 0; for (int k = 0; k < sz; k++) v |= (u32)r[i + k] << (8 * k);
        i32 sv = sz == 1 ? (i8)v : sz == 2 ? (i16)v : (i32)v;
        i += sz;
        if (type == 1) {                       /* global */
            if (tag == 0) page = v;
            else if (tag == 1) lmin = sv;
            else if (tag == 2) lmax = (lmin >= 0 && sz < 4) ? (i32)v : sv;
            else if (tag == 7) rsize = (int)v;
            else if (tag == 8) { rid = (int)v & 0xFF; d->has_ids = true; }
            else if (tag == 9) rcount = (int)v;
        } else if (type == 2) {                /* local */
            u32 u = sz == 4 ? v : (page << 16) | v;
            if (tag == 0 && nus < 16) usages[nus++] = u;
            else if (tag == 1) umin = u;
            else if (tag == 2) umax = u;
        } else if (type == 0) {                /* main */
            if (tag == 8) {                    /* Input */
                bool is_const = v & 1, rel = (v & 4) != 0;
                for (int k = 0; k < rcount; k++) {
                    u32 u = k < nus ? usages[k] : nus ? usages[nus - 1] : (umin && umin + (u32)k <= umax ? umin + (u32)k : 0);
                    if (!is_const) {
                        Field f = { rid, bitpos[rid], rsize, lmin, lmax, rel, true };
                        if (u == 0x00010030 && !d->x.used) d->x = f;
                        else if (u == 0x00010031 && !d->y.used) d->y = f;
                        else if (u == 0x00010038 && !d->wheel.used) d->wheel = f;
                        else if ((u >> 16) == 9 && (u & 0xFFFF) >= 1 && (u & 0xFFFF) <= 3 && !d->btn[(u & 0xFFFF) - 1].used) d->btn[(u & 0xFFFF) - 1] = f;
                    }
                    bitpos[rid] += rsize;
                }
            }
            nus = 0; umin = umax = 0;            /* todo item "main" (Input, Output, Feature, Collection, End Collection) limpa os locais */
        }
    }
}
static i32 get_field(const Field *f, const u8 *data, int len) {
    if (!f->used || f->size <= 0 || f->size > 32) return 0;
    u32 v = 0;
    for (int b = 0; b < f->size; b++) {
        int bit = f->bit + b, byte = bit >> 3;
        if (byte >= len) break;
        if (data[byte] >> (bit & 7) & 1) v |= 1u << b;
    }
    if (f->lmin < 0 && f->size < 32 && (v >> (f->size - 1)) & 1) return (i32)(v | (~0u << f->size));   /* com sinal */
    return (i32)v;
}

static void hid_log(const char *f, int a, int b, int c, int d, int e);
/* chamada pelo firmware (TPL de callback) a cada relatório */
static EFI_STATUS EFIAPI on_report(void *data, UINTN len, void *ctx, UINT32 status) {
    UsbPtr *d = ctx; const u8 *p = data; int n = (int)len;
    if (status || !p || n <= 0) return EFI_SUCCESS;
    int rid = 0;
    if (d->has_ids) { rid = p[0]; p++; n--; }
    if (d->x.used && d->x.report_id != rid) return EFI_SUCCESS;
    u32 b = 0;
    for (int k = 0; k < 3; k++) if (d->btn[k].used && d->btn[k].report_id == rid && get_field(&d->btn[k], p, n)) b |= 1u << k;
    if (d->absolute) { d->ax = (u32)get_field(&d->x, p, n); d->ay = (u32)get_field(&d->y, p, n); }
    else { d->dx += get_field(&d->x, p, n); d->dy += get_field(&d->y, p, n); }
    if (d->wheel.used && d->wheel.report_id == rid) d->dz += get_field(&d->wheel, p, n);
    d->buttons = b; d->latch |= b; d->reports++; d->changed = 1;
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------ protocolos publicados */
static EFI_STATUS EFIAPI sp_reset(EFI_SIMPLE_POINTER_PROTOCOL *s, BOOLEAN e) { (void)s; (void)e; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI sp_state(EFI_SIMPLE_POINTER_PROTOCOL *s, EFI_SIMPLE_POINTER_STATE *st) {
    UsbPtr *d = (UsbPtr *)s;
    if (!d->changed) return EFI_NOT_READY;
    d->changed = 0;
    st->RelativeMovementX = d->dx; st->RelativeMovementY = d->dy; st->RelativeMovementZ = -d->dz;   /* roda para cima = Z negativo na UEFI */
    u32 b = d->buttons | d->latch; d->latch = 0;
    if (b != d->buttons) d->changed = 1;   /* o soltar aparece na leitura seguinte */
    st->LeftButton = (b & 1) != 0; st->RightButton = (b & 2) != 0;
    d->dx = d->dy = d->dz = 0;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ap_reset(EFI_ABSOLUTE_POINTER_PROTOCOL *a, BOOLEAN e) { (void)a; (void)e; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI ap_state(EFI_ABSOLUTE_POINTER_PROTOCOL *a, EFI_ABSOLUTE_POINTER_STATE *st) {
    UsbPtr *d = (UsbPtr *)((u8 *)a - __builtin_offsetof(UsbPtr, ap));
    if (!d->changed) return EFI_NOT_READY;
    d->changed = 0;
    u32 b = d->buttons | d->latch; d->latch = 0;
    if (b != d->buttons) d->changed = 1;
    st->CurrentX = d->ax; st->CurrentY = d->ay; st->CurrentZ = 0; st->ActiveButtons = b;
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------ descoberta */
static bool already(EFI_HANDLE h) { for (int i = 0; i < nseen; i++) if (seen[i] == h) return true; return false; }
static void hid_log(const char *f, int a, int b, int c, int d, int e) {
    char s[160]; fmt(s, sizeof s, f, a, b, c, d, e); pal_log(s);
}
static void try_device(EFI_HANDLE h, USB_IO *io) {
    USB_IFACE ifc;
    EFI_STATUS s0 = io->GetInterfaceDescriptor(io, &ifc);
    if (dbg_on) hid_log("DBGHID iface st=%d class=%d sub=%d proto=%d eps=%d\r\n", (int)s0, ifc.InterfaceClass, ifc.InterfaceSubClass, ifc.InterfaceProtocol, ifc.NumEndpoints);
    if (s0 != EFI_SUCCESS || ifc.InterfaceClass != 3) return;
    if (ifc.InterfaceSubClass == 1 && ifc.InterfaceProtocol == 1) return;          /* teclado "boot": é do firmware */
    void *other;
    if (!EFI_ERROR(efi_bs->HandleProtocol(h, &sptr_g, &other)) || !EFI_ERROR(efi_bs->HandleProtocol(h, &aptr_g, &other))) return;   /* o firmware já cuida */
    USB_EP ep; u8 epaddr = 0; int maxp = 8, interval = 10;
    for (int k = 0; k < ifc.NumEndpoints && k < 8; k++) {
        if (io->GetEndpointDescriptor(io, (UINT8)k, &ep) != EFI_SUCCESS) continue;
        if ((ep.EndpointAddress & 0x80) && (ep.Attributes & 3) == 3) { epaddr = ep.EndpointAddress; maxp = ep.MaxPacketSize & 0x7FF; interval = ep.Interval ? ep.Interval : 10; break; }
    }
    if (!epaddr || ndevs >= COUNT(devs)) return;
    UsbPtr *d = &devs[ndevs]; memset(d, 0, sizeof *d);
    u8 desc[1024]; UINT32 st = 0;
    memset(desc, 0, sizeof desc);
    USB_REQ rq = { 0x81, 6, 0x2200, ifc.InterfaceNumber, (UINT16)sizeof desc };      /* GET_DESCRIPTOR (relatório) */
    EFI_STATUS s1 = io->ControlTransfer(io, &rq, 0, 1000, desc, sizeof desc, &st);
    int dlen = 0;
    for (int k = (int)sizeof desc - 1; k >= 0; k--) if (desc[k]) { dlen = k + 1; break; }   /* o firmware não diz quantos bytes vieram */
    /* o dispositivo manda menos bytes do que o pedido (normal); alguns firmwares acusam erro mesmo assim */
    if (dlen > 0) parse_report_desc(d, desc, dlen);
    if (dbg_on) hid_log("DBGHID desc st=%d len=%d x=%d y=%d rel=%d\r\n", (int)s1, dlen, d->x.used, d->y.used, d->x.rel);
    if ((!d->x.used || !d->y.used) && ifc.InterfaceSubClass == 1 && ifc.InterfaceProtocol == 2) {
        /* mouse que não se descreveu direito: protocolo "boot" (botões, dx, dy, roda), que todo mouse USB tem */
        USB_REQ proto = { 0x21, 0x0B, 0, ifc.InterfaceNumber, 0 };                  /* SET_PROTOCOL boot */
        io->ControlTransfer(io, &proto, 2, 200, 0, 0, &st);
        memset(d, 0, sizeof *d);
        for (int k = 0; k < 3; k++) d->btn[k] = (Field){ 0, k, 1, 0, 1, false, true };
        d->x = (Field){ 0, 8, 8, -127, 127, true, true };
        d->y = (Field){ 0, 16, 8, -127, 127, true, true };
        if (maxp >= 4) d->wheel = (Field){ 0, 24, 8, -127, 127, true, true };
    }
    if (!d->x.used || !d->y.used) return;                                            /* não é ponteiro */
    USB_REQ idle = { 0x21, 0x0A, 0, ifc.InterfaceNumber, 0 };                        /* SET_IDLE 0: só relata mudanças */
    io->ControlTransfer(io, &idle, 2, 200, 0, 0, &st);
    d->io = io; d->h = h; d->ep = epaddr; d->maxp = maxp;
    d->absolute = !d->x.rel;
    d->sp.Reset = sp_reset; d->sp.GetState = sp_state; d->sp.Mode = &d->spm;
    d->spm.ResolutionX = d->spm.ResolutionY = 8; d->spm.LeftButton = d->spm.RightButton = 1;
    d->ap.Reset = ap_reset; d->ap.GetState = ap_state; d->ap.Mode = &d->apm;
    d->apm.AbsoluteMinX = (u64)(d->x.lmin > 0 ? d->x.lmin : 0); d->apm.AbsoluteMaxX = (u64)(d->x.lmax > 0 ? d->x.lmax : 32767);
    d->apm.AbsoluteMinY = (u64)(d->y.lmin > 0 ? d->y.lmin : 0); d->apm.AbsoluteMaxY = (u64)(d->y.lmax > 0 ? d->y.lmax : 32767);
    EFI_STATUS s2 = io->AsyncInterruptTransfer(io, epaddr, 1, (UINTN)interval, (UINTN)maxp, on_report, d);
    if (dbg_on) hid_log("DBGHID async st=%d ep=%x maxp=%d interval=%d abs=%d\r\n", (int)s2, epaddr, maxp, interval, d->absolute);
    if (s2 != EFI_SUCCESS) return;
    fmt(d->kind, sizeof d->kind, "%s USB HID (FolioOS driver)", d->absolute ? "absolute" : "relative");
    ndevs++;
}

/* chamada pela camada de entrada (no boot e a cada 2 s); devolve os ponteiros novos */
int usbhid_scan(void **out_ifc, bool *out_abs, const char **out_kind, int cap) {
    EFI_HANDLE *hs; UINTN n; int added = 0, first = ndevs;
    if (EFI_ERROR(efi_bs->LocateHandleBuffer(ByProtocol, &usbio_guid, 0, &n, &hs))) return 0;
    for (UINTN i = 0; i < n; i++) {
        if (already(hs[i])) continue;
        if (nseen < COUNT(seen)) seen[nseen++] = hs[i];
        USB_IO *io;
        if (EFI_ERROR(efi_bs->HandleProtocol(hs[i], &usbio_guid, (void **)&io)) || !io) continue;
        try_device(hs[i], io);
    }
    efi_bs->FreePool(hs);
    for (int i = first; i < ndevs && added < cap; i++, added++) {
        out_abs[added] = devs[i].absolute;
        out_ifc[added] = devs[i].absolute ? (void *)&devs[i].ap : (void *)&devs[i].sp;
        out_kind[added] = devs[i].kind;
    }
    return added;
}
