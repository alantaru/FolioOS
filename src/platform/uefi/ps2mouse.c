/* FolioOS — driver próprio de mouse e touchpad PS/2 (controlador i8042), só em PCs x86
 *
 * Muitos notebooks ligam o touchpad pela porta PS/2 auxiliar, e muitos firmwares UEFI não têm
 * driver para ela. Este driver só entra quando o firmware não ofereceu nenhum ponteiro de verdade.
 * O controlador é dividido com o driver de teclado do firmware: aqui só se leem os bytes marcados
 * como vindos da porta auxiliar (bit 5 do status); um byte de mouse lido pelo firmware só faz um
 * pacote se perder, e a sincronização (bit 3 do primeiro byte) se recupera no seguinte.
 * Com a roda (IntelliMouse, ID 3) os pacotes têm 4 bytes. Publica um EFI_SIMPLE_POINTER próprio. */
#include "folio.h"
#include "efi.h"

#if defined(__x86_64__) || defined(__i386__)
static inline u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
void pal_stall_us(u32 us);

static EFI_SIMPLE_POINTER_PROTOCOL sp; static EFI_SIMPLE_POINTER_MODE spm;
static bool on, wheel; static u8 pkt[4]; static int np;
static i32 dx, dy, dz; static u32 buttons, latch; static bool changed;
static char info[96] = "not started";

static bool wait_in(void) { for (int i = 0; i < 20000; i++) { if (!(inb(0x64) & 2)) return true; pal_stall_us(10); } return false; }
static int read_aux(int ms) {   /* próximo byte da porta auxiliar, ou -1 */
    for (int i = 0; i < ms * 100; i++) {
        u8 s = inb(0x64);
        if ((s & 0x21) == 0x21) return inb(0x60);
        pal_stall_us(10);
    }
    return -1;
}
static bool aux_cmd(u8 c) {      /* manda um comando ao mouse e espera o ACK (0xFA) */
    for (int tries = 0; tries < 3; tries++) {
        if (!wait_in()) return false;
        outb(0x64, 0xD4);
        if (!wait_in()) return false;
        outb(0x60, c);
        int r = read_aux(50);
        if (r == 0xFA) return true;
        if (r != 0xFE) return false;    /* 0xFE = repita */
    }
    return false;
}

static void pump(void) {
    for (int guard = 0; guard < 64; guard++) {
        u8 s = inb(0x64);
        if ((s & 0x21) != 0x21) return;           /* nada da porta auxiliar (byte de teclado fica para o firmware) */
        u8 b = inb(0x60);
        if (np == 0 && !(b & 0x08)) continue;      /* fora de sincronia: espera um primeiro byte válido */
        pkt[np++] = b;
        if (np < (wheel ? 4 : 3)) continue;
        np = 0;
        if (pkt[0] & 0xC0) continue;               /* estouro: descarta */
        i32 x = pkt[1], y = pkt[2];
        if (pkt[0] & 0x10) x -= 256;
        if (pkt[0] & 0x20) y -= 256;
        dx += x; dy -= y;                          /* PS/2: y para cima é positivo */
        if (wheel) { i32 z = (i8)(pkt[3] << 4) >> 4; dz += z; }
        buttons = pkt[0] & 7; latch |= buttons; changed = true;
    }
}
static EFI_STATUS EFIAPI ps2_reset(EFI_SIMPLE_POINTER_PROTOCOL *s, BOOLEAN e) { (void)s; (void)e; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI ps2_state(EFI_SIMPLE_POINTER_PROTOCOL *s, EFI_SIMPLE_POINTER_STATE *st) {
    (void)s;
    pump();
    if (!changed) return EFI_NOT_READY;
    changed = false;
    u32 b = buttons | latch; latch = 0;
    if (b != buttons) changed = true;              /* o soltar de um clique rápido aparece na leitura seguinte */
    st->RelativeMovementX = dx; st->RelativeMovementY = dy; st->RelativeMovementZ = dz;
    st->LeftButton = (b & 1) != 0; st->RightButton = (b & 2) != 0;
    dx = dy = dz = 0;
    return EFI_SUCCESS;
}

/* liga o mouse PS/2; devolve o ponteiro publicado, ou 0 se não houver controlador ou mouse */
void *ps2mouse_start(void) {
    if (on) return 0;
    u16 cs; __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    if (cs & 3) { str_copy(info, "no port access (not running in ring 0)", sizeof info); return 0; }   /* ex.: efi_host no Windows */
    u8 s = inb(0x64);
    if (s == 0xFF) { str_copy(info, "no i8042 controller", sizeof info); return 0; }
    if (!wait_in()) { str_copy(info, "controller busy", sizeof info); return 0; }
    outb(0x64, 0xA8);                              /* habilita a porta auxiliar */
    pal_stall_us(1000);
    while ((inb(0x64) & 0x21) == 0x21) inb(0x60);  /* restos antigos do mouse */
    if (!aux_cmd(0xF6)) { str_copy(info, "no PS/2 mouse answered", sizeof info); return 0; }
    /* IntelliMouse: taxa 200, 100, 80 e pergunta o ID; 3 = tem roda */
    if (aux_cmd(0xF3) && aux_cmd(200) && aux_cmd(0xF3) && aux_cmd(100) && aux_cmd(0xF3) && aux_cmd(80) && aux_cmd(0xF2)) {
        int id = read_aux(50); wheel = id == 3;
    }
    aux_cmd(0xF3); aux_cmd(100);                   /* 100 relatórios/s */
    if (!aux_cmd(0xF4)) { str_copy(info, "PS/2 mouse did not enable reporting", sizeof info); return 0; }
    sp.Reset = ps2_reset; sp.GetState = ps2_state; sp.Mode = &spm;
    spm.ResolutionX = spm.ResolutionY = 8; spm.LeftButton = spm.RightButton = 1;
    on = true;
    fmt(info, sizeof info, "PS/2 mouse (FolioOS driver)%s", wheel ? " with wheel" : "");
    return &sp;
}
const char *ps2mouse_info(void) { return info; }
#else
void *ps2mouse_start(void) { return 0; }
const char *ps2mouse_info(void) { return "not available on this architecture"; }
#endif
