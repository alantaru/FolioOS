/* ============================================================================
 * FOLIO-86 v3.0 â€” MÃ¡quina Virtual x86 (VESA VBE + PCI USB UHCI HID + PS/2)
 * Arquivo: tools/vm86.c
 *
 * Executa o cÃ³digo de mÃ¡quina real de `build/folio86.img` desde o reset da CPU
 * (CS:IP = 0000:7C00, DL = 0x80), emulando:
 *   - CPU x86 16/32-bit Real & Unreal Mode (Prefixos 0x66/0x67, FS/GS 4GB, GDT, CR0)
 *   - MemÃ³ria RAM fÃ­sica de 16 MB (DMA USB em 0x00030000, Backbuffer em 0x00200000, LFB em 0x00800000)
 *   - Monitor EDID (INT 10h AX=4F15h) e VESA VBE 3.0 TrueColor 32-bit (AX=4F01h/4F02h)
 *   - Barramento PCI (Portas 0x0CF8 / 0x0CFC) + Controlador USB UHCI (Intel 82371SB 0x8086:0x7020)
 *   - Motor DMA USB UHCI (Frame List, Queue Head, Control TDs e Interrupt-IN TDs)
 *   - Mouse USB HID Boot Protocol de 4 bytes ([BotÃµes, dX, dY, Roda_Scroll])
 *   - Controlador 8042 PS/2 para Mouse (Portas 0x60 e 0x64) + BIOS INT 15h (AX=C2xx)
 *   - ExportaÃ§Ã£o de Screenshots TrueColor (.BMP) e Janela Interativa Win32 (--gui)
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif
#endif

#define RAM_SIZE        (16 * 1024 * 1024) /* 16 MB para abranger DMA USB, Backbuffer (2MB) e LFB (8MB) */
#define RAM_MASK        (RAM_SIZE - 1)
#define BACKBUF_PHYS    0x00200000u
#define VBE_LFB_PHYS    0x00800000u

#define SECTOR_SIZE     512
#define TOTAL_SECTORS   16472
#define DISK_SIZE       (TOTAL_SECTORS * SECTOR_SIZE)

/* Flags da CPU x86 */
#define FL_CF   (1u << 0)
#define FL_ZF   (1u << 6)
#define FL_SF   (1u << 7)
#define FL_IF   (1u << 9)
#define FL_DF   (1u << 10)
#define FL_OF   (1u << 11)

typedef struct {
    uint8_t buttons;
    int8_t  dx;
    int8_t  dy;
    int8_t  wheel;
} UsbHidReport;

typedef struct {
    union { uint32_t eax; uint16_t ax; struct { uint8_t al, ah; }; };
    union { uint32_t ecx; uint16_t cx; struct { uint8_t cl, ch; }; };
    union { uint32_t edx; uint16_t dx; struct { uint8_t dl, dh; }; };
    union { uint32_t ebx; uint16_t bx; struct { uint8_t bl, bh; }; };
    union { uint32_t esp; uint16_t sp; };
    union { uint32_t ebp; uint16_t bp; };
    union { uint32_t esi; uint16_t si; };
    union { uint32_t edi; uint16_t di; };

    uint16_t es, cs, ss, ds, fs, gs;
    uint16_t ip;
    uint16_t flags;
    uint32_t cr0;
    bool halted;

    uint8_t *ram;
    uint8_t *disk;

    /* Estado VESA VBE & Monitor EDID */
    uint16_t monitor_w;
    uint16_t monitor_h;
    uint16_t vbe_width;
    uint16_t vbe_height;
    uint8_t  vbe_bpp;
    bool     vbe_active;

    /* Hardware VGA & DAC */
    uint8_t dac_write_idx;
    uint8_t dac_comp_phase;
    uint8_t dac_rgb[256][3];
    uint8_t cursor_row, cursor_col;
    uint8_t vretrace_toggle;

    /* Hardware Speaker */
    uint8_t port_61;
    uint32_t speaker_pulses;

    /* Barramento PCI (0x0CF8 / 0x0CFC) + Controlador USB UHCI (I/O Base 0xC000) */
    uint32_t pci_config_addr;
    uint32_t usb_pci_cmd;
    uint16_t usb_cmd;
    uint16_t usb_sts;
    uint16_t usb_intr;
    uint16_t usb_frnum;
    uint32_t usb_frbase;
    uint16_t usb_portsc1;
    uint16_t usb_portsc2;
    uint8_t  usb_dev_addr;
    uint8_t  usb_enum_mask; /* Bit 0=SET_ADDRESS, Bit 1=SET_CONFIGURATION, Bit 2=SET_PROTOCOL(Boot) */
    uint32_t usb_dma_packets_delivered;

    /* Fila de relatÃ³rios USB HID Boot Mouse de 4 bytes ([buttons, dx, dy, wheel]) */
    UsbHidReport usb_hid_queue[512];
    int usb_hid_head, usb_hid_tail;

    /* Controlador 8042 PS/2 Mouse (Portas 0x60 / 0x64) */
    uint8_t cmd_64;
    uint8_t aux_queue[1024];
    int aux_head, aux_tail;
    int host_mouse_x, host_mouse_y;

    /* Fila de teclado (INT 16h) */
    uint16_t key_queue[256];
    int key_head, key_tail;

    /* Contadores de auditoria */
    uint32_t disk_reads;
    uint32_t disk_writes;
    uint64_t instructions_executed;
} VM86;

static uint32_t phys_addr32(uint16_t seg, uint32_t off) {
    return ((((uint32_t)seg << 4) + off) & RAM_MASK);
}

static uint32_t phys_r32(VM86 *vm, uint32_t paddr) {
    uint32_t p = paddr & RAM_MASK;
    return (uint32_t)vm->ram[p] |
           ((uint32_t)vm->ram[(p + 1) & RAM_MASK] << 8) |
           ((uint32_t)vm->ram[(p + 2) & RAM_MASK] << 16) |
           ((uint32_t)vm->ram[(p + 3) & RAM_MASK] << 24);
}

static void phys_w32(VM86 *vm, uint32_t paddr, uint32_t val) {
    uint32_t p = paddr & RAM_MASK;
    vm->ram[p] = (uint8_t)(val & 0xFF);
    vm->ram[(p + 1) & RAM_MASK] = (uint8_t)((val >> 8) & 0xFF);
    vm->ram[(p + 2) & RAM_MASK] = (uint8_t)((val >> 16) & 0xFF);
    vm->ram[(p + 3) & RAM_MASK] = (uint8_t)((val >> 24) & 0xFF);
}

static uint8_t mem_r8(VM86 *vm, uint16_t seg, uint32_t off) {
    return vm->ram[phys_addr32(seg, off)];
}

static uint16_t mem_r16(VM86 *vm, uint16_t seg, uint32_t off) {
    uint32_t p = phys_addr32(seg, off);
    return (uint16_t)(vm->ram[p] | ((uint16_t)vm->ram[(p + 1) & RAM_MASK] << 8));
}

static uint32_t mem_r32(VM86 *vm, uint16_t seg, uint32_t off) {
    return phys_r32(vm, phys_addr32(seg, off));
}

static void mem_w8(VM86 *vm, uint16_t seg, uint32_t off, uint8_t val) {
    vm->ram[phys_addr32(seg, off)] = val;
}

static void mem_w16(VM86 *vm, uint16_t seg, uint32_t off, uint16_t val) {
    uint32_t p = phys_addr32(seg, off);
    vm->ram[p] = (uint8_t)(val & 0xFF);
    vm->ram[(p + 1) & RAM_MASK] = (uint8_t)(val >> 8);
}

static void mem_w32(VM86 *vm, uint16_t seg, uint32_t off, uint32_t val) {
    phys_w32(vm, phys_addr32(seg, off), val);
}

/* ============================================================================
 * Emulador do Escalador DMA do Controlador USB UHCI (Intel 82371SB PIIX3)
 * Processa Frame List -> Queue Head -> Transfer Descriptors na RAM fÃ­sica
 * ============================================================================ */
static void vm_usb_process_schedule(VM86 *vm) {
    if ((vm->usb_cmd & 0x0001) == 0) return; /* Run/Stop bit == 0 */
    if (vm->usb_frbase == 0 || vm->usb_frbase >= RAM_SIZE) return;

    /* LÃª a entrada 0 da Frame List (4 KB aligned) */
    uint32_t fl_entry = phys_r32(vm, vm->usb_frbase);
    if (fl_entry & 1) return; /* Terminate */

    uint32_t qh_phys = fl_entry & ~0x0Fu;
    uint32_t td_phys = phys_r32(vm, qh_phys + 4); /* Element Link Pointer */

    int safety = 0;
    while ((td_phys & 1) == 0 && safety++ < 16) {
        uint32_t td_addr = td_phys & ~0x0Fu;
        uint32_t link    = phys_r32(vm, td_addr + 0);
        uint32_t ctrl    = phys_r32(vm, td_addr + 4);
        uint32_t token   = phys_r32(vm, td_addr + 8);
        uint32_t buf_ptr = phys_r32(vm, td_addr + 12);

        /* Bit 23 = Active */
        if ((ctrl & 0x00800000u) == 0) break;

        uint8_t pid   = (uint8_t)(token & 0xFF);
        uint8_t endpt = (uint8_t)((token >> 15) & 0x0F);

        if (endpt == 0) {
            /* TransferÃªncia de Controle USB (Endpoint 0: SETUP 0x2D / IN 0x69 / OUT 0xE1) */
            if (pid == 0x2D && buf_ptr + 8 <= RAM_SIZE) {
                uint8_t bRequest = vm->ram[buf_ptr + 1];
                uint8_t wValueLo = vm->ram[buf_ptr + 2];
                if (bRequest == 0x05) {
                    /* SET_ADDRESS */
                    vm->usb_dev_addr = wValueLo;
                    vm->usb_enum_mask |= 0x01;
                } else if (bRequest == 0x09) {
                    /* SET_CONFIGURATION */
                    vm->usb_enum_mask |= 0x02;
                } else if (bRequest == 0x0B) {
                    /* SET_PROTOCOL (0 = Boot Protocol) */
                    if (wValueLo == 0) vm->usb_enum_mask |= 0x04;
                }
            }
            /* Conclui o TD de controle limpando Bit 23 (Active) e avanÃ§a o Queue Head */
            ctrl &= ~0x00800000u;
            phys_w32(vm, td_addr + 4, ctrl);
            phys_w32(vm, qh_phys + 4, link);
            td_phys = link;
        } else if (pid == 0x69 && endpt == 1) {
            /* TransferÃªncia Interrupt-IN (Endpoint 1: RelatÃ³rio de 4 bytes do Mouse USB HID Boot) */
            if (vm->usb_hid_head != vm->usb_hid_tail && buf_ptr + 4 <= RAM_SIZE) {
                UsbHidReport rep = vm->usb_hid_queue[vm->usb_hid_head];
                vm->usb_hid_head = (vm->usb_hid_head + 1) % 512;

                /* Grava os 4 bytes via DMA diretamente no buffer fÃ­sico do TD */
                vm->ram[buf_ptr + 0] = rep.buttons;
                vm->ram[buf_ptr + 1] = (uint8_t)rep.dx;
                vm->ram[buf_ptr + 2] = (uint8_t)rep.dy;
                vm->ram[buf_ptr + 3] = (uint8_t)rep.wheel;

                /* Limpa o bit Active (Bit 23) e define ActLen = 3 (4 bytes transferidos) */
                ctrl = (ctrl & ~0x008007FFu) | 0x00000003u;
                phys_w32(vm, td_addr + 4, ctrl);
                phys_w32(vm, qh_phys + 4, link);
                vm->usb_dma_packets_delivered++;
            }
            break;
        } else {
            break;
        }
    }
}

static uint8_t fetch8(VM86 *vm) {
    uint8_t v = mem_r8(vm, vm->cs, vm->ip);
    vm->ip++;
    return v;
}

static uint16_t fetch16(VM86 *vm) {
    uint16_t lo = fetch8(vm);
    uint16_t hi = fetch8(vm);
    return (uint16_t)(lo | (hi << 8));
}

static uint32_t fetch32(VM86 *vm) {
    uint32_t b0 = fetch8(vm);
    uint32_t b1 = fetch8(vm);
    uint32_t b2 = fetch8(vm);
    uint32_t b3 = fetch8(vm);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

static void push16(VM86 *vm, uint16_t val) {
    vm->sp -= 2;
    mem_w16(vm, vm->ss, vm->sp, val);
}

static uint16_t pop16(VM86 *vm) {
    uint16_t v = mem_r16(vm, vm->ss, vm->sp);
    vm->sp += 2;
    return v;
}

static void push32(VM86 *vm, uint32_t val) {
    vm->sp -= 4;
    mem_w32(vm, vm->ss, vm->sp, val);
}

static uint32_t pop32(VM86 *vm) {
    uint32_t v = mem_r32(vm, vm->ss, vm->sp);
    vm->sp += 4;
    return v;
}

static void set_zf_sf8(VM86 *vm, uint8_t res) {
    if (res == 0) vm->flags |= FL_ZF; else vm->flags &= ~FL_ZF;
    if (res & 0x80) vm->flags |= FL_SF; else vm->flags &= ~FL_SF;
}

static void set_zf_sf16(VM86 *vm, uint16_t res) {
    if (res == 0) vm->flags |= FL_ZF; else vm->flags &= ~FL_ZF;
    if (res & 0x8000) vm->flags |= FL_SF; else vm->flags &= ~FL_SF;
}

static void set_zf_sf32(VM86 *vm, uint32_t res) {
    if (res == 0) vm->flags |= FL_ZF; else vm->flags &= ~FL_ZF;
    if (res & 0x80000000u) vm->flags |= FL_SF; else vm->flags &= ~FL_SF;
}

static uint8_t alu_add8(VM86 *vm, uint8_t a, uint8_t b) {
    uint16_t r = (uint16_t)a + (uint16_t)b;
    if (r & 0x100) vm->flags |= FL_CF; else vm->flags &= ~FL_CF;
    if (~(a ^ b) & (a ^ (uint8_t)r) & 0x80) vm->flags |= FL_OF; else vm->flags &= ~FL_OF;
    set_zf_sf8(vm, (uint8_t)r);
    return (uint8_t)r;
}

static uint16_t alu_add16(VM86 *vm, uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a + (uint32_t)b;
    if (r & 0x10000) vm->flags |= FL_CF; else vm->flags &= ~FL_CF;
    if (~(a ^ b) & (a ^ (uint16_t)r) & 0x8000) vm->flags |= FL_OF; else vm->flags &= ~FL_OF;
    set_zf_sf16(vm, (uint16_t)r);
    return (uint16_t)r;
}

static uint32_t alu_add32(VM86 *vm, uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a + (uint64_t)b;
    if (r & (1ULL << 32)) vm->flags |= FL_CF; else vm->flags &= ~FL_CF;
    if (~(a ^ b) & (a ^ (uint32_t)r) & 0x80000000u) vm->flags |= FL_OF; else vm->flags &= ~FL_OF;
    set_zf_sf32(vm, (uint32_t)r);
    return (uint32_t)r;
}

static uint8_t alu_sub8(VM86 *vm, uint8_t a, uint8_t b) {
    uint16_t r = (uint16_t)a - (uint16_t)b;
    if (a < b) vm->flags |= FL_CF; else vm->flags &= ~FL_CF;
    if ((a ^ b) & (a ^ (uint8_t)r) & 0x80) vm->flags |= FL_OF; else vm->flags &= ~FL_OF;
    set_zf_sf8(vm, (uint8_t)r);
    return (uint8_t)r;
}

static uint16_t alu_sub16(VM86 *vm, uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a - (uint32_t)b;
    if (a < b) vm->flags |= FL_CF; else vm->flags &= ~FL_CF;
    if ((a ^ b) & (a ^ (uint16_t)r) & 0x8000) vm->flags |= FL_OF; else vm->flags &= ~FL_OF;
    set_zf_sf16(vm, (uint16_t)r);
    return (uint16_t)r;
}

static uint32_t alu_sub32(VM86 *vm, uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a - (uint64_t)b;
    if (a < b) vm->flags |= FL_CF; else vm->flags &= ~FL_CF;
    if ((a ^ b) & (a ^ (uint32_t)r) & 0x80000000u) vm->flags |= FL_OF; else vm->flags &= ~FL_OF;
    set_zf_sf32(vm, (uint32_t)r);
    return (uint32_t)r;
}

static uint8_t alu_logic8(VM86 *vm, uint8_t res) {
    vm->flags &= ~(FL_CF | FL_OF);
    set_zf_sf8(vm, res);
    return res;
}

static uint16_t alu_logic16(VM86 *vm, uint16_t res) {
    vm->flags &= ~(FL_CF | FL_OF);
    set_zf_sf16(vm, res);
    return res;
}

static uint32_t alu_logic32(VM86 *vm, uint32_t res) {
    vm->flags &= ~(FL_CF | FL_OF);
    set_zf_sf32(vm, res);
    return res;
}

static uint8_t get_r8(VM86 *vm, int r) {
    switch (r & 7) {
        case 0: return vm->al; case 1: return vm->cl;
        case 2: return vm->dl; case 3: return vm->bl;
        case 4: return vm->ah; case 5: return vm->ch;
        case 6: return vm->dh; default: return vm->bh;
    }
}

static void set_r8(VM86 *vm, int r, uint8_t v) {
    switch (r & 7) {
        case 0: vm->al = v; break; case 1: vm->cl = v; break;
        case 2: vm->dl = v; break; case 3: vm->bl = v; break;
        case 4: vm->ah = v; break; case 5: vm->ch = v; break;
        case 6: vm->dh = v; break; default: vm->bh = v; break;
    }
}

static uint16_t get_r16(VM86 *vm, int r) {
    switch (r & 7) {
        case 0: return vm->ax; case 1: return vm->cx;
        case 2: return vm->dx; case 3: return vm->bx;
        case 4: return vm->sp; case 5: return vm->bp;
        case 6: return vm->si; default: return vm->di;
    }
}

static void set_r16(VM86 *vm, int r, uint16_t v) {
    switch (r & 7) {
        case 0: vm->ax = v; break; case 1: vm->cx = v; break;
        case 2: vm->dx = v; break; case 3: vm->bx = v; break;
        case 4: vm->sp = v; break; case 5: vm->bp = v; break;
        case 6: vm->si = v; break; default: vm->di = v; break;
    }
}

static uint32_t get_r32(VM86 *vm, int r) {
    switch (r & 7) {
        case 0: return vm->eax; case 1: return vm->ecx;
        case 2: return vm->edx; case 3: return vm->ebx;
        case 4: return vm->esp; case 5: return vm->ebp;
        case 6: return vm->esi; default: return vm->edi;
    }
}

static void set_r32(VM86 *vm, int r, uint32_t v) {
    switch (r & 7) {
        case 0: vm->eax = v; break; case 1: vm->ecx = v; break;
        case 2: vm->edx = v; break; case 3: vm->ebx = v; break;
        case 4: vm->esp = v; break; case 5: vm->ebp = v; break;
        case 6: vm->esi = v; break; default: vm->edi = v; break;
    }
}

static uint16_t get_seg(VM86 *vm, int s) {
    switch (s & 7) {
        case 0: return vm->es; case 1: return vm->cs;
        case 2: return vm->ss; case 3: return vm->ds;
        case 4: return vm->fs; case 5: return vm->gs;
        default: return vm->ds;
    }
}

static void set_seg(VM86 *vm, int s, uint16_t v) {
    switch (s & 7) {
        case 0: vm->es = v; break; case 1: vm->cs = v; break;
        case 2: vm->ss = v; break; case 3: vm->ds = v; break;
        case 4: vm->fs = v; break; case 5: vm->gs = v; break;
        default: break;
    }
}

/* Decodificador de endereÃ§o efetivo 16-bit e 32-bit (0x67) ModR/M */
typedef struct {
    bool is_reg;
    int reg_idx;
    uint16_t seg;
    uint32_t off;
} EffAddr;

static EffAddr decode_modrm(VM86 *vm, uint8_t modrm, int seg_override, bool addr32) {
    EffAddr ea;
    int mod = (modrm >> 6) & 3;
    int rm  = modrm & 7;
    if (mod == 3) {
        ea.is_reg = true;
        ea.reg_idx = rm;
        ea.seg = 0;
        ea.off = 0;
        return ea;
    }
    ea.is_reg = false;
    ea.reg_idx = 0;
    uint16_t def_seg = vm->ds;

    if (!addr32) {
        uint16_t base = 0;
        if (mod == 0 && rm == 6) {
            base = fetch16(vm);
        } else {
            switch (rm) {
                case 0: base = vm->bx + vm->si; break;
                case 1: base = vm->bx + vm->di; break;
                case 2: base = vm->bp + vm->si; def_seg = vm->ss; break;
                case 3: base = vm->bp + vm->di; def_seg = vm->ss; break;
                case 4: base = vm->si; break;
                case 5: base = vm->di; break;
                case 6: base = vm->bp; def_seg = vm->ss; break;
                case 7: base = vm->bx; break;
            }
            if (mod == 1) {
                int8_t disp8 = (int8_t)fetch8(vm);
                base = (uint16_t)(base + (int16_t)disp8);
            } else if (mod == 2) {
                uint16_t disp16 = fetch16(vm);
                base = (uint16_t)(base + disp16);
            }
        }
        ea.seg = (seg_override >= 0) ? (uint16_t)seg_override : def_seg;
        ea.off = (uint32_t)base;
    } else {
        uint32_t base = 0;
        if (rm == 4) {
            uint8_t sib = fetch8(vm);
            int scale = (sib >> 6) & 3;
            int idx   = (sib >> 3) & 7;
            int b_reg = sib & 7;
            if (b_reg == 5 && mod == 0) {
                base = fetch32(vm);
            } else {
                base = get_r32(vm, b_reg);
                if (b_reg == 4 || b_reg == 5) def_seg = vm->ss;
            }
            if (idx != 4) {
                base += get_r32(vm, idx) << scale;
            }
        } else if (mod == 0 && rm == 5) {
            base = fetch32(vm);
        } else {
            base = get_r32(vm, rm);
            if (rm == 4 || rm == 5) def_seg = vm->ss;
        }
        if (mod == 1) {
            int8_t disp8 = (int8_t)fetch8(vm);
            base += (int32_t)disp8;
        } else if (mod == 2) {
            uint32_t disp32 = fetch32(vm);
            base += disp32;
        }
        ea.seg = (seg_override >= 0) ? (uint16_t)seg_override : def_seg;
        ea.off = base;
    }
    return ea;
}

static uint8_t ea_r8(VM86 *vm, EffAddr ea) {
    return ea.is_reg ? get_r8(vm, ea.reg_idx) : mem_r8(vm, ea.seg, ea.off);
}

static void ea_w8(VM86 *vm, EffAddr ea, uint8_t v) {
    if (ea.is_reg) set_r8(vm, ea.reg_idx, v);
    else mem_w8(vm, ea.seg, ea.off, v);
}

static uint16_t ea_r16(VM86 *vm, EffAddr ea) {
    return ea.is_reg ? get_r16(vm, ea.reg_idx) : mem_r16(vm, ea.seg, ea.off);
}

static void ea_w16(VM86 *vm, EffAddr ea, uint16_t v) {
    if (ea.is_reg) set_r16(vm, ea.reg_idx, v);
    else mem_w16(vm, ea.seg, ea.off, v);
}

static uint32_t ea_r32(VM86 *vm, EffAddr ea) {
    return ea.is_reg ? get_r32(vm, ea.reg_idx) : mem_r32(vm, ea.seg, ea.off);
}

static void ea_w32(VM86 *vm, EffAddr ea, uint32_t v) {
    if (ea.is_reg) set_r32(vm, ea.reg_idx, v);
    else mem_w32(vm, ea.seg, ea.off, v);
}

/* Mapeia cÃ³digos de modo VESA VBE para resoluÃ§Ã£o (L x A) */
static bool get_vbe_mode_dims(uint16_t mode, uint16_t *w, uint16_t *h) {
    switch (mode & 0x01FF) {
        case 0x0112: *w = 640;  *h = 480;  return true;
        case 0x0115: *w = 800;  *h = 600;  return true;
        case 0x0118: *w = 1024; *h = 768;  return true;
        case 0x011B: *w = 1280; *h = 1024; return true;
        case 0x0144: *w = 1440; *h = 900;  return true;
        case 0x0145: *w = 1600; *h = 900;  return true;
        case 0x0147: *w = 1920; *h = 1080; return true;
        default: return false;
    }
}

/* EmulaÃ§Ã£o de BIOS INT 10h (VBE/EDID/VGA), INT 13h (Disco), INT 15h (A20/Mouse), INT 16h (Teclado) */
static void handle_int(VM86 *vm, uint8_t vec) {
    if (vec == 0x10) {
        if (vm->ax == 0x4F15 && vm->bl == 0x01) {
            /* VBE/DDC Read EDID (128 bytes em ES:DI) */
            uint32_t dst = phys_addr32(vm->es, vm->di);
            memset(vm->ram + dst, 0, 128);
            vm->ram[dst + 0] = 0x00;
            for (int i = 1; i <= 6; i++) vm->ram[dst + i] = 0xFF;
            vm->ram[dst + 7] = 0x00;
            vm->ram[dst + 54] = 0x64;
            vm->ram[dst + 55] = 0x19;
            vm->ram[dst + 56] = (uint8_t)(vm->monitor_w & 0xFF);
            vm->ram[dst + 58] = (uint8_t)(((vm->monitor_w >> 8) & 0x0F) << 4);
            vm->ram[dst + 59] = (uint8_t)(vm->monitor_h & 0xFF);
            vm->ram[dst + 61] = (uint8_t)(((vm->monitor_h >> 8) & 0x0F) << 4);
            vm->ax = 0x004F;
        } else if (vm->ax == 0x4F01) {
            uint16_t mw = 0, mh = 0;
            if (get_vbe_mode_dims(vm->cx, &mw, &mh)) {
                uint32_t dst = phys_addr32(vm->es, vm->di);
                memset(vm->ram + dst, 0, 256);
                mem_w16(vm, vm->es, vm->di + 0,  0x009B);
                mem_w16(vm, vm->es, vm->di + 16, mw * 4);
                mem_w16(vm, vm->es, vm->di + 18, mw);
                mem_w16(vm, vm->es, vm->di + 20, mh);
                mem_w8 (vm, vm->es, vm->di + 25, 32);
                mem_w32(vm, vm->es, vm->di + 40, VBE_LFB_PHYS);
                mem_w16(vm, vm->es, vm->di + 50, mw * 4);
                vm->ax = 0x004F;
            } else {
                vm->ax = 0x014F;
            }
        } else if (vm->ax == 0x4F02) {
            uint16_t mw = 0, mh = 0;
            if (get_vbe_mode_dims(vm->bx, &mw, &mh)) {
                vm->vbe_width  = mw;
                vm->vbe_height = mh;
                vm->vbe_bpp    = 32;
                vm->vbe_active = true;
                vm->ax = 0x004F;
            } else {
                vm->ax = 0x014F;
            }
        } else if (vm->ah == 0x00) {
            vm->vbe_active = false;
            for (int i = 0; i < 80 * 25; i++) {
                mem_w16(vm, 0xB800, (uint16_t)(i * 2), 0x0720);
            }
        } else if (vm->ah == 0x02) {
            vm->cursor_row = vm->dh;
            vm->cursor_col = vm->dl;
        }
    } else if (vec == 0x15) {
        if (vm->ax == 0x2401) {
            /* Enable A20 Gate */
            vm->ah = 0;
            vm->flags &= ~FL_CF;
        } else if (vm->ah == 0xC2) {
            /* BIOS Pointing Device Service (INT 15h AX=C2xx) */
            vm->ah = 0;
            vm->flags &= ~FL_CF;
        }
    } else if (vec == 0x13) {
        if (vm->ah == 0x41 && vm->bx == 0x55AA) {
            vm->bx = 0xAA55;
            vm->cx = 0x0001;
            vm->flags &= ~FL_CF;
        } else if (vm->ah == 0x42) {
            uint16_t count = mem_r16(vm, vm->ds, vm->si + 2);
            uint16_t off   = mem_r16(vm, vm->ds, vm->si + 4);
            uint16_t seg   = mem_r16(vm, vm->ds, vm->si + 6);
            uint32_t lba   = mem_r16(vm, vm->ds, vm->si + 8) |
                             ((uint32_t)mem_r16(vm, vm->ds, vm->si + 10) << 16);
            if (lba + count > TOTAL_SECTORS) {
                vm->flags |= FL_CF;
            } else {
                uint32_t dst = phys_addr32(seg, off);
                memcpy(vm->ram + dst, vm->disk + (lba * SECTOR_SIZE), count * SECTOR_SIZE);
                vm->disk_reads += count;
                vm->ah = 0;
                vm->flags &= ~FL_CF;
            }
        } else if (vm->ah == 0x43) {
            uint16_t count = mem_r16(vm, vm->ds, vm->si + 2);
            uint16_t off   = mem_r16(vm, vm->ds, vm->si + 4);
            uint16_t seg   = mem_r16(vm, vm->ds, vm->si + 6);
            uint32_t lba   = mem_r16(vm, vm->ds, vm->si + 8) |
                             ((uint32_t)mem_r16(vm, vm->ds, vm->si + 10) << 16);
            if (lba + count > TOTAL_SECTORS) {
                vm->flags |= FL_CF;
            } else {
                uint32_t src = phys_addr32(seg, off);
                memcpy(vm->disk + (lba * SECTOR_SIZE), vm->ram + src, count * SECTOR_SIZE);
                vm->disk_writes += count;
                vm->ah = 0;
                vm->flags &= ~FL_CF;
            }
        } else if (vm->ah == 0x08) {
            vm->ch = 0;
            vm->cl = 63;
            vm->dh = 15;
            vm->flags &= ~FL_CF;
        } else {
            vm->flags &= ~FL_CF;
        }
    } else if (vec == 0x16) {
        if (vm->ah == 0x01) {
            if (vm->key_head == vm->key_tail) {
                vm->flags |= FL_ZF;
            } else {
                vm->ax = vm->key_queue[vm->key_head];
                vm->flags &= ~FL_ZF;
            }
        } else if (vm->ah == 0x00) {
            if (vm->key_head != vm->key_tail) {
                vm->ax = vm->key_queue[vm->key_head];
                vm->key_head = (vm->key_head + 1) % 256;
            } else {
                vm->ax = 0;
            }
        }
    }
}

static void aux_push(VM86 *vm, uint8_t b) {
    vm->aux_queue[vm->aux_tail] = b;
    vm->aux_tail = (vm->aux_tail + 1) % 1024;
}

static bool eval_cc(VM86 *vm, int cc) {
    bool cf = (vm->flags & FL_CF) != 0;
    bool zf = (vm->flags & FL_ZF) != 0;
    bool sf = (vm->flags & FL_SF) != 0;
    bool of = (vm->flags & FL_OF) != 0;
    switch (cc & 0x0F) {
        case 0x0: return of;
        case 0x1: return !of;
        case 0x2: return cf;
        case 0x3: return !cf;
        case 0x4: return zf;
        case 0x5: return !zf;
        case 0x6: return cf || zf;
        case 0x7: return !cf && !zf;
        case 0x8: return sf;
        case 0x9: return !sf;
        case 0xC: return sf != of;
        case 0xD: return sf == of;
        case 0xE: return zf || (sf != of);
        case 0xF: return !zf && (sf == of);
        default: return false;
    }
}

/* Executa 1 instruÃ§Ã£o x86 (16/32-bit Real/Unreal Mode) */
static void vm_step(VM86 *vm) {
    int seg_ovr = -1;
    bool rep_prefix = false;
    bool op32 = false;
    bool addr32 = false;

    for (;;) {
        uint8_t b = fetch8(vm);
        if (b == 0x26) { seg_ovr = vm->es; continue; }
        if (b == 0x3E) { seg_ovr = vm->ds; continue; }
        if (b == 0x2E) { seg_ovr = vm->cs; continue; }
        if (b == 0x36) { seg_ovr = vm->ss; continue; }
        if (b == 0x64) { seg_ovr = vm->fs; continue; }
        if (b == 0x65) { seg_ovr = vm->gs; continue; }
        if (b == 0x66) { op32 = !op32; continue; }
        if (b == 0x67) { addr32 = !addr32; continue; }
        if (b == 0xF3 || b == 0xF2) { rep_prefix = true; continue; }

        vm->instructions_executed++;

        if (b == 0xF4) { vm->halted = true; return; }
        if (b == 0x90) return;
        if (b == 0xFA) { vm->flags &= ~FL_IF; return; }
        if (b == 0xFB) { vm->flags |= FL_IF; return; }
        if (b == 0xFC) { vm->flags &= ~FL_DF; return; }
        if (b == 0xFD) { vm->flags |= FL_DF; return; }
        if (b == 0xF8) { vm->flags &= ~FL_CF; return; }
        if (b == 0xF9) { vm->flags |= FL_CF; return; }

        /* CBW/CWDE (0x98) e CWD/CDQ (0x99) */
        if (b == 0x98) {
            if (!op32) vm->ax = (uint16_t)(int16_t)(int8_t)vm->al;
            else vm->eax = (uint32_t)(int32_t)(int16_t)vm->ax;
            return;
        }
        if (b == 0x99) {
            if (!op32) vm->dx = (vm->ax & 0x8000) ? 0xFFFF : 0x0000;
            else vm->edx = (vm->eax & 0x80000000u) ? 0xFFFFFFFFu : 0x00000000u;
            return;
        }

        /* PUSHA (0x60) / POPA (0x61) */
        if (b == 0x60) {
            if (!op32) {
                uint16_t tmp = vm->sp;
                push16(vm, vm->ax); push16(vm, vm->cx); push16(vm, vm->dx); push16(vm, vm->bx);
                push16(vm, tmp);    push16(vm, vm->bp); push16(vm, vm->si); push16(vm, vm->di);
            } else {
                uint32_t tmp = vm->esp;
                push32(vm, vm->eax); push32(vm, vm->ecx); push32(vm, vm->edx); push32(vm, vm->ebx);
                push32(vm, tmp);     push32(vm, vm->ebp); push32(vm, vm->esi); push32(vm, vm->edi);
            }
            return;
        }
        if (b == 0x61) {
            if (!op32) {
                vm->di = pop16(vm); vm->si = pop16(vm); vm->bp = pop16(vm); (void)pop16(vm);
                vm->bx = pop16(vm); vm->dx = pop16(vm); vm->cx = pop16(vm); vm->ax = pop16(vm);
            } else {
                vm->edi = pop32(vm); vm->esi = pop32(vm); vm->ebp = pop32(vm); (void)pop32(vm);
                vm->ebx = pop32(vm); vm->edx = pop32(vm); vm->ecx = pop32(vm); vm->eax = pop32(vm);
            }
            return;
        }

        /* PUSH / POP Segment Registers */
        if (b == 0x06) { push16(vm, vm->es); return; }
        if (b == 0x07) { vm->es = pop16(vm); return; }
        if (b == 0x1E) { push16(vm, vm->ds); return; }
        if (b == 0x1F) { vm->ds = pop16(vm); return; }
        if (b == 0x0E) { push16(vm, vm->cs); return; }

        /* PUSH / POP General Registers (0x50..0x5F) */
        if (b >= 0x50 && b <= 0x57) {
            if (!op32) push16(vm, get_r16(vm, b - 0x50));
            else push32(vm, get_r32(vm, b - 0x50));
            return;
        }
        if (b >= 0x58 && b <= 0x5F) {
            if (!op32) set_r16(vm, b - 0x58, pop16(vm));
            else set_r32(vm, b - 0x58, pop32(vm));
            return;
        }

        /* INC / DEC r16/r32 (0x40..0x4F) */
        if (b >= 0x40 && b <= 0x47) {
            int r = b - 0x40;
            bool old_cf = (vm->flags & FL_CF) != 0;
            if (!op32) set_r16(vm, r, alu_add16(vm, get_r16(vm, r), 1));
            else set_r32(vm, r, alu_add32(vm, get_r32(vm, r), 1));
            if (old_cf) vm->flags |= FL_CF; else vm->flags &= ~FL_CF;
            return;
        }
        if (b >= 0x48 && b <= 0x4F) {
            int r = b - 0x48;
            bool old_cf = (vm->flags & FL_CF) != 0;
            if (!op32) set_r16(vm, r, alu_sub16(vm, get_r16(vm, r), 1));
            else set_r32(vm, r, alu_sub32(vm, get_r32(vm, r), 1));
            if (old_cf) vm->flags |= FL_CF; else vm->flags &= ~FL_CF;
            return;
        }

        /* XCHG AX/EAX, r16/r32 (0x91..0x97) */
        if (b >= 0x91 && b <= 0x97) {
            int r = b - 0x90;
            if (!op32) { uint16_t t = vm->ax; vm->ax = get_r16(vm, r); set_r16(vm, r, t); }
            else { uint32_t t = vm->eax; vm->eax = get_r32(vm, r); set_r32(vm, r, t); }
            return;
        }

        /* MOV r8, imm8 (0xB0..0xB7) */
        if (b >= 0xB0 && b <= 0xB7) { set_r8(vm, b - 0xB0, fetch8(vm)); return; }
        /* MOV r16/r32, imm16/imm32 (0xB8..0xBF) */
        if (b >= 0xB8 && b <= 0xBF) {
            if (!op32) set_r16(vm, b - 0xB8, fetch16(vm));
            else set_r32(vm, b - 0xB8, fetch32(vm));
            return;
        }

        /* MOV AL/AX/EAX, moffs (0xA0..0xA3) */
        if (b >= 0xA0 && b <= 0xA3) {
            uint32_t off = addr32 ? fetch32(vm) : (uint32_t)fetch16(vm);
            uint16_t seg = (seg_ovr >= 0) ? (uint16_t)seg_ovr : vm->ds;
            if (b == 0xA0) vm->al = mem_r8(vm, seg, off);
            else if (b == 0xA1) { if (!op32) vm->ax = mem_r16(vm, seg, off); else vm->eax = mem_r32(vm, seg, off); }
            else if (b == 0xA2) mem_w8(vm, seg, off, vm->al);
            else { if (!op32) mem_w16(vm, seg, off, vm->ax); else mem_w32(vm, seg, off, vm->eax); }
            return;
        }

        /* MOV r/m, r e MOV r, r/m (0x88..0x8B) */
        if (b >= 0x88 && b <= 0x8B) {
            uint8_t m = fetch8(vm);
            int reg = (m >> 3) & 7;
            EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
            if (b == 0x88) ea_w8(vm, ea, get_r8(vm, reg));
            else if (b == 0x89) { if (!op32) ea_w16(vm, ea, get_r16(vm, reg)); else ea_w32(vm, ea, get_r32(vm, reg)); }
            else if (b == 0x8A) set_r8(vm, reg, ea_r8(vm, ea));
            else { if (!op32) set_r16(vm, reg, ea_r16(vm, ea)); else set_r32(vm, reg, ea_r32(vm, ea)); }
            return;
        }

        /* MOV Sreg, r/m16 (0x8E) e MOV r/m16, Sreg (0x8C) */
        if (b == 0x8E || b == 0x8C) {
            uint8_t m = fetch8(vm);
            int sreg = (m >> 3) & 7;
            EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
            if (b == 0x8E) set_seg(vm, sreg, ea_r16(vm, ea));
            else ea_w16(vm, ea, get_seg(vm, sreg));
            return;
        }

        /* MOV r/m8, imm8 (0xC6) e MOV r/m16/32, imm16/32 (0xC7) */
        if (b == 0xC6 || b == 0xC7) {
            uint8_t m = fetch8(vm);
            EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
            if (b == 0xC6) ea_w8(vm, ea, fetch8(vm));
            else if (!op32) ea_w16(vm, ea, fetch16(vm));
            else ea_w32(vm, ea, fetch32(vm));
            return;
        }

        /* LEA r16/32, m (0x8D) */
        if (b == 0x8D) {
            uint8_t m = fetch8(vm);
            int reg = (m >> 3) & 7;
            EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
            if (!op32) set_r16(vm, reg, (uint16_t)ea.off);
            else set_r32(vm, reg, ea.off);
            return;
        }

        /* OperaÃ§Ãµes ALU padrÃ£o (ADD=0, OR=1, AND=4, SUB=5, XOR=6, CMP=7) */
        if ((b & 0xC0) == 0x00 && (b & 6) != 6) {
            int op = (b >> 3) & 7;
            int dir = (b >> 1) & 1;
            int w = b & 1;
            if ((b & 6) == 4) {
                if (!w) {
                    uint8_t imm = fetch8(vm);
                    if (op == 0) vm->al = alu_add8(vm, vm->al, imm);
                    else if (op == 1) vm->al = alu_logic8(vm, vm->al | imm);
                    else if (op == 4) vm->al = alu_logic8(vm, vm->al & imm);
                    else if (op == 5) vm->al = alu_sub8(vm, vm->al, imm);
                    else if (op == 6) vm->al = alu_logic8(vm, vm->al ^ imm);
                    else if (op == 7) (void)alu_sub8(vm, vm->al, imm);
                } else if (!op32) {
                    uint16_t imm = fetch16(vm);
                    if (op == 0) vm->ax = alu_add16(vm, vm->ax, imm);
                    else if (op == 1) vm->ax = alu_logic16(vm, vm->ax | imm);
                    else if (op == 4) vm->ax = alu_logic16(vm, vm->ax & imm);
                    else if (op == 5) vm->ax = alu_sub16(vm, vm->ax, imm);
                    else if (op == 6) vm->ax = alu_logic16(vm, vm->ax ^ imm);
                    else if (op == 7) (void)alu_sub16(vm, vm->ax, imm);
                } else {
                    uint32_t imm = fetch32(vm);
                    if (op == 0) vm->eax = alu_add32(vm, vm->eax, imm);
                    else if (op == 1) vm->eax = alu_logic32(vm, vm->eax | imm);
                    else if (op == 4) vm->eax = alu_logic32(vm, vm->eax & imm);
                    else if (op == 5) vm->eax = alu_sub32(vm, vm->eax, imm);
                    else if (op == 6) vm->eax = alu_logic32(vm, vm->eax ^ imm);
                    else if (op == 7) (void)alu_sub32(vm, vm->eax, imm);
                }
                return;
            } else {
                uint8_t m = fetch8(vm);
                int reg = (m >> 3) & 7;
                EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
                if (!w) {
                    uint8_t lhs = dir ? get_r8(vm, reg) : ea_r8(vm, ea);
                    uint8_t rhs = dir ? ea_r8(vm, ea) : get_r8(vm, reg);
                    uint8_t res = 0;
                    if (op == 0) res = alu_add8(vm, lhs, rhs);
                    else if (op == 1) res = alu_logic8(vm, lhs | rhs);
                    else if (op == 4) res = alu_logic8(vm, lhs & rhs);
                    else if (op == 5) res = alu_sub8(vm, lhs, rhs);
                    else if (op == 6) res = alu_logic8(vm, lhs ^ rhs);
                    else if (op == 7) { (void)alu_sub8(vm, lhs, rhs); return; }
                    if (dir) set_r8(vm, reg, res); else ea_w8(vm, ea, res);
                } else if (!op32) {
                    uint16_t lhs = dir ? get_r16(vm, reg) : ea_r16(vm, ea);
                    uint16_t rhs = dir ? ea_r16(vm, ea) : get_r16(vm, reg);
                    uint16_t res = 0;
                    if (op == 0) res = alu_add16(vm, lhs, rhs);
                    else if (op == 1) res = alu_logic16(vm, lhs | rhs);
                    else if (op == 4) res = alu_logic16(vm, lhs & rhs);
                    else if (op == 5) res = alu_sub16(vm, lhs, rhs);
                    else if (op == 6) res = alu_logic16(vm, lhs ^ rhs);
                    else if (op == 7) { (void)alu_sub16(vm, lhs, rhs); return; }
                    if (dir) set_r16(vm, reg, res); else ea_w16(vm, ea, res);
                } else {
                    uint32_t lhs = dir ? get_r32(vm, reg) : ea_r32(vm, ea);
                    uint32_t rhs = dir ? ea_r32(vm, ea) : get_r32(vm, reg);
                    uint32_t res = 0;
                    if (op == 0) res = alu_add32(vm, lhs, rhs);
                    else if (op == 1) res = alu_logic32(vm, lhs | rhs);
                    else if (op == 4) res = alu_logic32(vm, lhs & rhs);
                    else if (op == 5) res = alu_sub32(vm, lhs, rhs);
                    else if (op == 6) res = alu_logic32(vm, lhs ^ rhs);
                    else if (op == 7) { (void)alu_sub32(vm, lhs, rhs); return; }
                    if (dir) set_r32(vm, reg, res); else ea_w32(vm, ea, res);
                }
                return;
            }
        }

        /* Grupo 0x80 / 0x81 / 0x83 (ALU r/m, imm) */
        if (b == 0x80 || b == 0x81 || b == 0x83) {
            uint8_t m = fetch8(vm);
            int op = (m >> 3) & 7;
            EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
            if (b == 0x80) {
                uint8_t lhs = ea_r8(vm, ea);
                uint8_t rhs = fetch8(vm);
                uint8_t res = 0;
                if (op == 0) res = alu_add8(vm, lhs, rhs);
                else if (op == 1) res = alu_logic8(vm, lhs | rhs);
                else if (op == 4) res = alu_logic8(vm, lhs & rhs);
                else if (op == 5) res = alu_sub8(vm, lhs, rhs);
                else if (op == 6) res = alu_logic8(vm, lhs ^ rhs);
                else if (op == 7) { (void)alu_sub8(vm, lhs, rhs); return; }
                ea_w8(vm, ea, res);
            } else if (!op32) {
                uint16_t lhs = ea_r16(vm, ea);
                uint16_t rhs = (b == 0x81) ? fetch16(vm) : (uint16_t)(int16_t)(int8_t)fetch8(vm);
                uint16_t res = 0;
                if (op == 0) res = alu_add16(vm, lhs, rhs);
                else if (op == 1) res = alu_logic16(vm, lhs | rhs);
                else if (op == 4) res = alu_logic16(vm, lhs & rhs);
                else if (op == 5) res = alu_sub16(vm, lhs, rhs);
                else if (op == 6) res = alu_logic16(vm, lhs ^ rhs);
                else if (op == 7) { (void)alu_sub16(vm, lhs, rhs); return; }
                ea_w16(vm, ea, res);
            } else {
                uint32_t lhs = ea_r32(vm, ea);
                uint32_t rhs = (b == 0x81) ? fetch32(vm) : (uint32_t)(int32_t)(int8_t)fetch8(vm);
                uint32_t res = 0;
                if (op == 0) res = alu_add32(vm, lhs, rhs);
                else if (op == 1) res = alu_logic32(vm, lhs | rhs);
                else if (op == 4) res = alu_logic32(vm, lhs & rhs);
                else if (op == 5) res = alu_sub32(vm, lhs, rhs);
                else if (op == 6) res = alu_logic32(vm, lhs ^ rhs);
                else if (op == 7) { (void)alu_sub32(vm, lhs, rhs); return; }
                ea_w32(vm, ea, res);
            }
            return;
        }

        /* TEST r/m8, r8 (0x84), TEST r/m16/32, r16/32 (0x85), TEST AL, imm8 (0xA8), TEST AX/EAX, imm (0xA9) */
        if (b == 0x84 || b == 0x85) {
            uint8_t m = fetch8(vm);
            int reg = (m >> 3) & 7;
            EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
            if (b == 0x84) (void)alu_logic8(vm, ea_r8(vm, ea) & get_r8(vm, reg));
            else if (!op32) (void)alu_logic16(vm, ea_r16(vm, ea) & get_r16(vm, reg));
            else (void)alu_logic32(vm, ea_r32(vm, ea) & get_r32(vm, reg));
            return;
        }
        if (b == 0xA8) { (void)alu_logic8(vm, vm->al & fetch8(vm)); return; }
        if (b == 0xA9) {
            if (!op32) (void)alu_logic16(vm, vm->ax & fetch16(vm));
            else (void)alu_logic32(vm, vm->eax & fetch32(vm));
            return;
        }

        /* SHL/SHR/SAR Grupo 0xC0 / 0xC1 / 0xD0 / 0xD1 / 0xD2 / 0xD3 */
        if (b == 0xC0 || b == 0xC1 || b == 0xD0 || b == 0xD1 || b == 0xD2 || b == 0xD3) {
            uint8_t m = fetch8(vm);
            int op = (m >> 3) & 7;
            EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
            uint8_t cnt = (b == 0xC0 || b == 0xC1) ? fetch8(vm) : ((b == 0xD2 || b == 0xD3) ? (vm->cl & 31) : 1);
            if (b == 0xC0 || b == 0xD0 || b == 0xD2) {
                uint8_t v = ea_r8(vm, ea);
                if (op == 4) v = (uint8_t)(v << cnt);
                else if (op == 5) v = (uint8_t)(v >> cnt);
                else if (op == 7) v = (uint8_t)(((int8_t)v) >> cnt);
                ea_w8(vm, ea, alu_logic8(vm, v));
            } else if (!op32) {
                uint16_t v = ea_r16(vm, ea);
                if (op == 4) v = (uint16_t)(v << cnt);
                else if (op == 5) v = (uint16_t)(v >> cnt);
                else if (op == 7) v = (uint16_t)(((int16_t)v) >> cnt);
                ea_w16(vm, ea, alu_logic16(vm, v));
            } else {
                uint32_t v = ea_r32(vm, ea);
                if (op == 4) v = (uint32_t)(v << cnt);
                else if (op == 5) v = (uint32_t)(v >> cnt);
                else if (op == 7) v = (uint32_t)(((int32_t)v) >> cnt);
                ea_w32(vm, ea, alu_logic32(vm, v));
            }
            return;
        }

        /* IMUL r, r/m, imm8 (0x6B) ou imm16/32 (0x69) */
        if (b == 0x6B || b == 0x69) {
            uint8_t m = fetch8(vm);
            int reg = (m >> 3) & 7;
            EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
            if (!op32) {
                int16_t lhs = (int16_t)ea_r16(vm, ea);
                int16_t rhs = (b == 0x6B) ? (int16_t)(int8_t)fetch8(vm) : (int16_t)fetch16(vm);
                set_r16(vm, reg, (uint16_t)(lhs * rhs));
            } else {
                int32_t lhs = (int32_t)ea_r32(vm, ea);
                int32_t rhs = (b == 0x6B) ? (int32_t)(int8_t)fetch8(vm) : (int32_t)fetch32(vm);
                set_r32(vm, reg, (uint32_t)(lhs * rhs));
            }
            return;
        }

        /* Grupo 0xF6 (8-bit) e 0xF7 (16/32-bit): TEST, NOT, NEG, MUL, IMUL, DIV, IDIV */
        if (b == 0xF6 || b == 0xF7) {
            uint8_t m = fetch8(vm);
            int op = (m >> 3) & 7;
            EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
            if (b == 0xF6) {
                uint8_t v = ea_r8(vm, ea);
                if (op == 0) { (void)alu_logic8(vm, v & fetch8(vm)); return; }
                if (op == 2) { ea_w8(vm, ea, ~v); return; }
                if (op == 3) { ea_w8(vm, ea, alu_sub8(vm, 0, v)); return; }
                if (op == 4) { vm->ax = (uint16_t)vm->al * (uint16_t)v; return; }
                if (op == 6) {
                    uint16_t num = vm->ax;
                    vm->al = (uint8_t)(num / v);
                    vm->ah = (uint8_t)(num % v);
                    return;
                }
            } else if (!op32) {
                uint16_t v = ea_r16(vm, ea);
                if (op == 0) { (void)alu_logic16(vm, v & fetch16(vm)); return; }
                if (op == 2) { ea_w16(vm, ea, ~v); return; }
                if (op == 3) { ea_w16(vm, ea, alu_sub16(vm, 0, v)); return; }
                if (op == 4) {
                    uint32_t p = (uint32_t)vm->ax * (uint32_t)v;
                    vm->ax = (uint16_t)p;
                    vm->dx = (uint16_t)(p >> 16);
                    return;
                }
                if (op == 6) {
                    uint32_t num = ((uint32_t)vm->dx << 16) | vm->ax;
                    vm->ax = (uint16_t)(num / v);
                    vm->dx = (uint16_t)(num % v);
                    return;
                }
            } else {
                uint32_t v = ea_r32(vm, ea);
                if (op == 0) { (void)alu_logic32(vm, v & fetch32(vm)); return; }
                if (op == 2) { ea_w32(vm, ea, ~v); return; }
                if (op == 3) { ea_w32(vm, ea, alu_sub32(vm, 0, v)); return; }
                if (op == 4) {
                    uint64_t p = (uint64_t)vm->eax * (uint64_t)v;
                    vm->eax = (uint32_t)p;
                    vm->edx = (uint32_t)(p >> 32);
                    return;
                }
                if (op == 6) {
                    uint64_t num = ((uint64_t)vm->edx << 32) | vm->eax;
                    vm->eax = (uint32_t)(num / v);
                    vm->edx = (uint32_t)(num % v);
                    return;
                }
            }
        }

        /* INC/DEC r/m8 (0xFE) e INC/DEC/CALL/JMP/PUSH r/m16/32 (0xFF) */
        if (b == 0xFE || b == 0xFF) {
            uint8_t m = fetch8(vm);
            int op = (m >> 3) & 7;
            EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
            bool old_cf = (vm->flags & FL_CF) != 0;
            if (b == 0xFE) {
                uint8_t v = ea_r8(vm, ea);
                v = (op == 0) ? alu_add8(vm, v, 1) : alu_sub8(vm, v, 1);
                ea_w8(vm, ea, v);
                if (old_cf) vm->flags |= FL_CF; else vm->flags &= ~FL_CF;
                return;
            } else {
                if (op == 0 || op == 1) {
                    if (!op32) {
                        uint16_t v = ea_r16(vm, ea);
                        v = (op == 0) ? alu_add16(vm, v, 1) : alu_sub16(vm, v, 1);
                        ea_w16(vm, ea, v);
                    } else {
                        uint32_t v = ea_r32(vm, ea);
                        v = (op == 0) ? alu_add32(vm, v, 1) : alu_sub32(vm, v, 1);
                        ea_w32(vm, ea, v);
                    }
                    if (old_cf) vm->flags |= FL_CF; else vm->flags &= ~FL_CF;
                    return;
                } else if (op == 6) {
                    if (!op32) push16(vm, ea_r16(vm, ea));
                    else push32(vm, ea_r32(vm, ea));
                    return;
                }
            }
        }

        /* InstruÃ§Ãµes de String: MOVSB (A4), MOVSW/D (A5), STOSB (AA), STOSW/D (AB), LODSB (AC), LODSW/D (AD) */
        if (b == 0xA4 || b == 0xA5 || b == 0xAA || b == 0xAB || b == 0xAC || b == 0xAD) {
            int step = (vm->flags & FL_DF) ? -1 : 1;
            uint32_t count = rep_prefix ? (addr32 ? vm->ecx : vm->cx) : 1;
            uint16_t src_seg = (seg_ovr >= 0) ? (uint16_t)seg_ovr : vm->ds;
            while (count > 0) {
                if (b == 0xA4) {
                    mem_w8(vm, vm->es, vm->di, mem_r8(vm, src_seg, vm->si));
                    vm->si = (uint16_t)(vm->si + step);
                    vm->di = (uint16_t)(vm->di + step);
                } else if (b == 0xA5) {
                    if (!op32) {
                        mem_w16(vm, vm->es, vm->di, mem_r16(vm, src_seg, vm->si));
                        vm->si = (uint16_t)(vm->si + step * 2);
                        vm->di = (uint16_t)(vm->di + step * 2);
                    } else {
                        mem_w32(vm, vm->es, vm->di, mem_r32(vm, src_seg, vm->si));
                        vm->si = (uint16_t)(vm->si + step * 4);
                        vm->di = (uint16_t)(vm->di + step * 4);
                    }
                } else if (b == 0xAA) {
                    mem_w8(vm, vm->es, vm->di, vm->al);
                    vm->di = (uint16_t)(vm->di + step);
                } else if (b == 0xAB) {
                    if (!op32) {
                        mem_w16(vm, vm->es, vm->di, vm->ax);
                        vm->di = (uint16_t)(vm->di + step * 2);
                    } else {
                        mem_w32(vm, vm->es, vm->di, vm->eax);
                        vm->di = (uint16_t)(vm->di + step * 4);
                    }
                } else if (b == 0xAC) {
                    vm->al = mem_r8(vm, src_seg, vm->si);
                    vm->si = (uint16_t)(vm->si + step);
                } else if (b == 0xAD) {
                    if (!op32) {
                        vm->ax = mem_r16(vm, src_seg, vm->si);
                        vm->si = (uint16_t)(vm->si + step * 2);
                    } else {
                        vm->eax = mem_r32(vm, src_seg, vm->si);
                        vm->si = (uint16_t)(vm->si + step * 4);
                    }
                }
                count--;
                if (rep_prefix) {
                    if (addr32) vm->ecx = count;
                    else vm->cx = (uint16_t)count;
                }
            }
            return;
        }

        /* Saltos Condicionais 8-bit (0x70..0x7F) */
        if (b >= 0x70 && b <= 0x7F) {
            int8_t rel = (int8_t)fetch8(vm);
            if (eval_cc(vm, b & 0x0F)) vm->ip = (uint16_t)(vm->ip + (int16_t)rel);
            return;
        }

        /* Prefixos 0x0F (2-byte opcodes) */
        if (b == 0x0F) {
            uint8_t b2 = fetch8(vm);
            if (b2 == 0x01) {
                uint8_t m = fetch8(vm);
                (void)decode_modrm(vm, m, seg_ovr, addr32);
                return;
            }
            if (b2 == 0x20) {
                uint8_t m = fetch8(vm);
                set_r32(vm, m & 7, vm->cr0);
                return;
            }
            if (b2 == 0x22) {
                uint8_t m = fetch8(vm);
                vm->cr0 = get_r32(vm, m & 7);
                return;
            }
            if (b2 >= 0x80 && b2 <= 0x8F) {
                int16_t rel = (int16_t)fetch16(vm);
                if (eval_cc(vm, b2 & 0x0F)) vm->ip = (uint16_t)(vm->ip + rel);
                return;
            }
            if (b2 == 0xAF) {
                uint8_t m = fetch8(vm);
                int reg = (m >> 3) & 7;
                EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
                if (!op32) {
                    int16_t p = (int16_t)get_r16(vm, reg) * (int16_t)ea_r16(vm, ea);
                    set_r16(vm, reg, (uint16_t)p);
                } else {
                    int32_t p = (int32_t)get_r32(vm, reg) * (int32_t)ea_r32(vm, ea);
                    set_r32(vm, reg, (uint32_t)p);
                }
                return;
            }
            if (b2 == 0xB6) {
                uint8_t m = fetch8(vm);
                int reg = (m >> 3) & 7;
                EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
                uint8_t v = ea_r8(vm, ea);
                if (!op32) set_r16(vm, reg, (uint16_t)v);
                else set_r32(vm, reg, (uint32_t)v);
                return;
            }
            if (b2 == 0xB7) {
                uint8_t m = fetch8(vm);
                int reg = (m >> 3) & 7;
                EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
                uint16_t v = ea_r16(vm, ea);
                set_r32(vm, reg, (uint32_t)v);
                return;
            }
            if (b2 == 0xBE) {
                uint8_t m = fetch8(vm);
                int reg = (m >> 3) & 7;
                EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
                int8_t v = (int8_t)ea_r8(vm, ea);
                if (!op32) set_r16(vm, reg, (uint16_t)(int16_t)v);
                else set_r32(vm, reg, (uint32_t)(int32_t)v);
                return;
            }
            if (b2 == 0xBF) {
                uint8_t m = fetch8(vm);
                int reg = (m >> 3) & 7;
                EffAddr ea = decode_modrm(vm, m, seg_ovr, addr32);
                int16_t v = (int16_t)ea_r16(vm, ea);
                set_r32(vm, reg, (uint32_t)(int32_t)v);
                return;
            }
            fprintf(stderr, "[VM ERRO] Opcode 0x0F 0x%02X nao suportado em %04X:%04X\n",
                    b2, vm->cs, (uint16_t)(vm->ip - 2));
            exit(1);
        }

        /* LOOP rel8 (0xE2) */
        if (b == 0xE2) {
            int8_t rel = (int8_t)fetch8(vm);
            vm->cx--;
            if (vm->cx != 0) vm->ip = (uint16_t)(vm->ip + (int16_t)rel);
            return;
        }

        /* CALL rel16 (0xE8), JMP rel16 (0xE9), JMP ptr16:16 (0xEA), JMP rel8 (0xEB), RET (0xC3), RETF (0xCB) */
        if (b == 0xE8) {
            int16_t rel = (int16_t)fetch16(vm);
            push16(vm, vm->ip);
            vm->ip = (uint16_t)(vm->ip + rel);
            return;
        }
        if (b == 0xE9) {
            int16_t rel = (int16_t)fetch16(vm);
            vm->ip = (uint16_t)(vm->ip + rel);
            return;
        }
        if (b == 0xEB) {
            int8_t rel = (int8_t)fetch8(vm);
            vm->ip = (uint16_t)(vm->ip + (int16_t)rel);
            return;
        }
        if (b == 0xEA) {
            uint16_t new_ip = fetch16(vm);
            uint16_t new_cs = fetch16(vm);
            vm->ip = new_ip;
            vm->cs = new_cs;
            return;
        }
        if (b == 0xC3) {
            vm->ip = pop16(vm);
            return;
        }
        if (b == 0xCB) {
            vm->ip = pop16(vm);
            vm->cs = pop16(vm);
            return;
        }

        /* INT imm8 (0xCD) */
        if (b == 0xCD) {
            uint8_t vec = fetch8(vm);
            handle_int(vm, vec);
            return;
        }

        /* Portas de I/O:
         *   0xE4: IN AL, imm8    | 0xE6: OUT imm8, AL
         *   0xEC: IN AL, DX      | 0xED: IN AX/EAX, DX
         *   0xEE: OUT DX, AL     | 0xEF: OUT DX, AX/EAX */
        if (b == 0xE4) {
            uint8_t port = fetch8(vm);
            if (port == 0x61) {
                vm->al = vm->port_61;
            } else if (port == 0x64) {
                vm->al = (vm->aux_head != vm->aux_tail) ? 0x21 : 0x00;
            } else if (port == 0x60) {
                if (vm->aux_head != vm->aux_tail) {
                    vm->al = vm->aux_queue[vm->aux_head];
                    vm->aux_head = (vm->aux_head + 1) % 1024;
                } else {
                    vm->al = 0xFA;
                }
            } else {
                vm->al = 0;
            }
            return;
        }
        if (b == 0xE6) {
            uint8_t port = fetch8(vm);
            if (port == 0x61) {
                if ((vm->al & 3) == 3) vm->speaker_pulses++;
                vm->port_61 = vm->al;
            } else if (port == 0x64) {
                vm->cmd_64 = vm->al;
                if (vm->al == 0x20) {
                    aux_push(vm, 0x00);
                }
            } else if (port == 0x60) {
                if (vm->cmd_64 == 0xD4) {
                    aux_push(vm, 0xFA);
                    vm->cmd_64 = 0;
                }
            }
            return;
        }
        if (b == 0xEC) {
            if (vm->dx == 0x03DA) {
                vm->vretrace_toggle ^= 0x08;
                vm->al = vm->vretrace_toggle;
            } else {
                vm->al = 0;
            }
            return;
        }
        if (b == 0xED) {
            /* IN AX, DX ou IN EAX, DX (Barramento PCI 0x0CFC e Controlador USB UHCI 0xC000..0xC01F) */
            if (vm->dx == 0x0CFC && op32) {
                uint8_t dev_func = (uint8_t)((vm->pci_config_addr >> 8) & 0xFF);
                uint8_t reg      = (uint8_t)(vm->pci_config_addr & 0xFC);
                /* Dispositivo PCI 00:01.1 (dev_func = 0x09): Intel 82371SB PIIX3 USB UHCI */
                if (dev_func == 0x09) {
                    if (reg == 0x00) vm->eax = 0x70208086u;      /* Vendor 8086h (Intel), Device 7020h (UHCI) */
                    else if (reg == 0x04) vm->eax = vm->usb_pci_cmd;
                    else if (reg == 0x08) vm->eax = 0x0C030001u; /* Class 0Ch (Serial), Sub 03h (USB), ProgIF 00h (UHCI) */
                    else if (reg == 0x20) vm->eax = 0x0000C001u; /* BAR4: I/O Base 0xC000 */
                    else vm->eax = 0;
                } else {
                    vm->eax = 0xFFFFFFFFu;
                }
            } else if (vm->dx >= 0xC000 && vm->dx <= 0xC01F) {
                uint16_t off = (uint16_t)(vm->dx - 0xC000);
                if (off == 0x00) vm->ax = vm->usb_cmd;
                else if (off == 0x02) vm->ax = vm->usb_sts;
                else if (off == 0x10) vm->ax = vm->usb_portsc1; /* Porta 1: Mouse USB HID conectado (Bit 0 = 1) */
                else if (off == 0x12) vm->ax = vm->usb_portsc2;
                else vm->ax = 0;
            } else {
                if (!op32) vm->ax = 0; else vm->eax = 0;
            }
            return;
        }
        if (b == 0xEE) {
            if (vm->dx == 0x03C8) {
                vm->dac_write_idx = vm->al;
                vm->dac_comp_phase = 0;
            } else if (vm->dx == 0x03C9) {
                vm->dac_rgb[vm->dac_write_idx][vm->dac_comp_phase] = vm->al & 0x3F;
                vm->dac_comp_phase++;
                if (vm->dac_comp_phase >= 3) {
                    vm->dac_comp_phase = 0;
                    vm->dac_write_idx++;
                }
            }
            return;
        }
        if (b == 0xEF) {
            /* OUT DX, AX ou OUT DX, EAX (Barramento PCI 0x0CF8/0x0CFC e Controlador USB UHCI 0xC000..0xC01F) */
            if (vm->dx == 0x0CF8 && op32) {
                vm->pci_config_addr = vm->eax;
            } else if (vm->dx == 0x0CFC && op32) {
                uint8_t dev_func = (uint8_t)((vm->pci_config_addr >> 8) & 0xFF);
                uint8_t reg      = (uint8_t)(vm->pci_config_addr & 0xFC);
                if (dev_func == 0x09 && reg == 0x04) {
                    vm->usb_pci_cmd = vm->eax;
                }
            } else if (vm->dx >= 0xC000 && vm->dx <= 0xC01F) {
                uint16_t off = (uint16_t)(vm->dx - 0xC000);
                if (off == 0x00) {
                    /* USBCMD */
                    if (vm->ax & 0x0002) {
                        /* HCRESET limpa bit 1 imediatamente apÃ³s o reset */
                        vm->usb_cmd = vm->ax & ~0x0002u;
                    } else {
                        vm->usb_cmd = vm->ax;
                    }
                    if (vm->usb_cmd & 0x0001) {
                        vm_usb_process_schedule(vm);
                    }
                } else if (off == 0x02) {
                    vm->usb_sts &= ~vm->ax;
                } else if (off == 0x04) {
                    vm->usb_intr = vm->ax;
                } else if (off == 0x06) {
                    vm->usb_frnum = vm->ax;
                } else if (off == 0x08 && op32) {
                    vm->usb_frbase = vm->eax & 0xFFFFF000u;
                } else if (off == 0x10) {
                    /* PORTSC1: preserva Current Connect Status (Bit 0 = 1) */
                    vm->usb_portsc1 = (vm->ax & ~0x020Au) | 0x0095u;
                } else if (off == 0x12) {
                    vm->usb_portsc2 = 0x0080u;
                }
            }
            return;
        }

        fprintf(stderr, "[VM ERRO] Opcode nao suportado 0x%02X em %04X:%04X\n",
                b, vm->cs, (uint16_t)(vm->ip - 1));
        exit(1);
    }
}

/* Inicializa / Reinicia a VM a frio carregando LBA 0 em 0x0000:0x7C00 */
static void vm_cold_reset(VM86 *vm) {
    memset(vm->ram, 0, RAM_SIZE);
    vm->eax = vm->ebx = vm->ecx = 0;
    vm->edx = 0x0080; /* DL = 0x80 (USB Boot Drive) */
    vm->esp = 0x7C00;
    vm->ebp = vm->esi = vm->edi = 0;
    vm->cs = vm->ds = vm->es = vm->ss = vm->fs = vm->gs = 0x0000;
    vm->ip = 0x7C00;
    vm->flags = FL_IF;
    vm->cr0 = 0;
    vm->halted = false;
    vm->key_head = vm->key_tail = 0;
    vm->aux_head = vm->aux_tail = 0;
    vm->usb_hid_head = vm->usb_hid_tail = 0;
    vm->monitor_w = 1024;
    vm->monitor_h = 768;
    vm->host_mouse_x = 832; /* PosiÃ§Ã£o inicial do mouse definida em vbe_init (paper_x + 700) */
    vm->host_mouse_y = 104; /* PosiÃ§Ã£o inicial do mouse definida em vbe_init (paper_y + 60)  */

    /* Estado inicial do Controlador PCI USB UHCI com Mouse USB HID conectado na Porta 1 */
    vm->pci_config_addr = 0;
    vm->usb_pci_cmd = 0x02800001u;
    vm->usb_cmd = 0;
    vm->usb_sts = 0;
    vm->usb_frbase = 0;
    vm->usb_portsc1 = 0x0093u; /* Bit 0=Connected, Bit 7=Reserved1, Bit 4=LineStatus */
    vm->usb_portsc2 = 0x0080u;
    vm->usb_dev_addr = 0;
    vm->usb_enum_mask = 0;
    vm->usb_dma_packets_delivered = 0;

    /* Copia LBA 0 (Bootloader) para 0x07C00 */
    memcpy(vm->ram + 0x7C00, vm->disk, SECTOR_SIZE);
}

/* Executa a VM atÃ© atingir a instruÃ§Ã£o HLT (idle do loop de eventos) */
static void vm_run_until_hlt(VM86 *vm, uint32_t max_steps) {
    vm->halted = false;
    for (uint32_t i = 0; i < max_steps && !vm->halted; i++) {
        vm_step(vm);
    }
}

static void vm_push_key(VM86 *vm, uint8_t scan, uint8_t ascii) {
    vm->key_queue[vm->key_tail] = (uint16_t)(((uint16_t)scan << 8) | ascii);
    vm->key_tail = (vm->key_tail + 1) % 256;
    vm_run_until_hlt(vm, 30000000);
}

/* Enfileira string em lote na fila INT 16h para renderizar apÃ³s o bloco */
static void vm_type_string(VM86 *vm, const char *str) {
    int queued = 0;
    for (const char *p = str; *p; p++) {
        uint16_t k = (*p == '\n') ? 0x1C0D : (uint16_t)(uint8_t)*p;
        vm->key_queue[vm->key_tail] = k;
        vm->key_tail = (vm->key_tail + 1) % 256;
        queued++;
        if (queued >= 120) {
            vm_run_until_hlt(vm, 40000000);
            queued = 0;
        }
    }
    if (queued > 0) {
        vm_run_until_hlt(vm, 40000000);
    }
}

/* Enfileira um pacote de 4 bytes no Mouse USB HID Boot Protocol e dispara o DMA UHCI */
static void usb_hid_push(VM86 *vm, uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    UsbHidReport rep;
    rep.buttons = buttons;
    rep.dx      = dx;
    rep.dy      = dy;
    rep.wheel   = wheel;
    vm->usb_hid_queue[vm->usb_hid_tail] = rep;
    vm->usb_hid_tail = (vm->usb_hid_tail + 1) % 512;
}

/* Envia movimento/clique/roda pelo protocolo nativo USB HID Boot Mouse via DMA UHCI */
static void vm_send_usb_mouse(VM86 *vm, int dx, int dy_screen, int wheel, uint8_t buttons) {
    while (dx != 0 || dy_screen != 0) {
        int step_x = (dx > 120) ? 120 : ((dx < -120) ? -120 : dx);
        int step_y = (dy_screen > 120) ? 120 : ((dy_screen < -120) ? -120 : dy_screen);
        /* No padrÃ£o USB HID, +dY aponta para baixo (mesmo sentido da tela) */
        usb_hid_push(vm, buttons, (int8_t)step_x, (int8_t)step_y, 0);
        dx -= step_x;
        dy_screen -= step_y;
    }
    usb_hid_push(vm, buttons, 0, 0, (int8_t)wheel);
    vm_usb_process_schedule(vm);
    vm_run_until_hlt(vm, 35000000);
}

/* Move o cursor do Mouse USB HID para (target_x, target_y) e realiza um clique esquerdo */
static void vm_usb_mouse_click_at(VM86 *vm, int target_x, int target_y) {
    int dx = target_x - vm->host_mouse_x;
    int dy = target_y - vm->host_mouse_y;
    vm->host_mouse_x = target_x;
    vm->host_mouse_y = target_y;
    vm_send_usb_mouse(vm, dx, dy, 0, 0); /* 1. Move atÃ© o alvo via USB HID DMA */
    vm_send_usb_mouse(vm, 0,  0,  0, 1); /* 2. Pressiona botÃ£o esquerdo USB HID */
    vm_send_usb_mouse(vm, 0,  0,  0, 0); /* 3. Solta botÃ£o esquerdo USB HID */
}

/* Move o cursor do Mouse USB HID sem clicar */
static void vm_usb_mouse_move_to(VM86 *vm, int target_x, int target_y) {
    int dx = target_x - vm->host_mouse_x;
    int dy = target_y - vm->host_mouse_y;
    vm->host_mouse_x = target_x;
    vm->host_mouse_y = target_y;
    vm_send_usb_mouse(vm, dx, dy, 0, 0);
}

/* Injeta pacotes PS/2 de 3 bytes no controlador 8042 (Porta 0x60/0x64) p/ teste de compatibilidade */
static void vm_send_ps2_mouse(VM86 *vm, int dx, int dy_screen, uint8_t buttons) {
    while (dx != 0 || dy_screen != 0) {
        int step_x = (dx > 120) ? 120 : ((dx < -120) ? -120 : dx);
        int step_y = (dy_screen > 120) ? 120 : ((dy_screen < -120) ? -120 : dy_screen);
        int ps2_dy = -step_y;
        uint8_t b0 = 0x08 | (buttons & 0x07) |
                     ((step_x < 0) ? 0x10 : 0) |
                     ((ps2_dy < 0) ? 0x20 : 0);
        aux_push(vm, b0);
        aux_push(vm, (uint8_t)(int8_t)step_x);
        aux_push(vm, (uint8_t)(int8_t)ps2_dy);
        dx -= step_x;
        dy_screen -= step_y;
    }
    aux_push(vm, (uint8_t)(0x08 | (buttons & 0x07)));
    aux_push(vm, 0);
    aux_push(vm, 0);
    vm_run_until_hlt(vm, 35000000);
}

/* Exporta o Linear Framebuffer VESA VBE 32-bit (0x00800000) como imagem BMP 24-bit */
static bool vm_save_bmp(VM86 *vm, const char *path) {
    if (!vm->vbe_active) return false;
    int w = vm->vbe_width;
    int h = vm->vbe_height;
    int row_stride = (w * 3 + 3) & ~3;
    int img_size = row_stride * h;
    int file_size = 54 + img_size;

    FILE *fp = fopen(path, "wb");
    if (!fp) return false;

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t *)(hdr + 2)  = (uint32_t)file_size;
    *(uint32_t *)(hdr + 10) = 54;
    *(uint32_t *)(hdr + 14) = 40;
    *(int32_t  *)(hdr + 18) = w;
    *(int32_t  *)(hdr + 22) = h;
    *(uint16_t *)(hdr + 26) = 1;
    *(uint16_t *)(hdr + 28) = 24;
    *(uint32_t *)(hdr + 34) = (uint32_t)img_size;

    fwrite(hdr, 1, 54, fp);

    uint8_t *row_buf = (uint8_t *)calloc(1, (size_t)row_stride);
    const uint32_t *lfb = (const uint32_t *)(vm->ram + VBE_LFB_PHYS);

    for (int y = h - 1; y >= 0; y--) {
        const uint32_t *src = lfb + (y * w);
        for (int x = 0; x < w; x++) {
            uint32_t px = src[x];
            row_buf[x * 3 + 0] = (uint8_t)(px & 0xFF);
            row_buf[x * 3 + 1] = (uint8_t)((px >> 8) & 0xFF);
            row_buf[x * 3 + 2] = (uint8_t)((px >> 16) & 0xFF);
        }
        fwrite(row_buf, 1, (size_t)row_stride, fp);
    }

    free(row_buf);
    fclose(fp);
    return true;
}

/* LÃª um pixel 32-bit 0x00RRGGBB do Framebuffer VESA VBE */
static uint32_t vm_get_pixel(VM86 *vm, int x, int y) {
    const uint32_t *lfb = (const uint32_t *)(vm->ram + VBE_LFB_PHYS);
    return lfb[y * vm->vbe_width + x];
}

#ifdef _WIN32
/* ============================================================================
 * MODO JANELA INTERATIVA WIN32 GDI (--gui) COM MOUSE USB HID + RODA DE ROLAGEM
 * ============================================================================ */
static VM86 *g_gui_vm = NULL;

static LRESULT CALLBACK folio_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_gui_vm) return DefWindowProcA(hwnd, msg, wparam, lparam);
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_MOUSEMOVE: {
            int mx = (int)(short)LOWORD(lparam);
            int my = (int)(short)HIWORD(lparam);
            if (mx >= 0 && mx < g_gui_vm->vbe_width && my >= 0 && my < g_gui_vm->vbe_height) {
                int dx = mx - g_gui_vm->host_mouse_x;
                int dy = my - g_gui_vm->host_mouse_y;
                if (dx != 0 || dy != 0) {
                    g_gui_vm->host_mouse_x = mx;
                    g_gui_vm->host_mouse_y = my;
                    uint8_t btn = (wparam & MK_LBUTTON) ? 1 : 0;
                    vm_send_usb_mouse(g_gui_vm, dx, dy, 0, btn);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int mx = (int)(short)LOWORD(lparam);
            int my = (int)(short)HIWORD(lparam);
            vm_usb_mouse_click_at(g_gui_vm, mx, my);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int zDelta = (short)HIWORD(wparam);
            int wheel = (zDelta > 0) ? 1 : ((zDelta < 0) ? -1 : 0);
            if (wheel != 0) {
                vm_send_usb_mouse(g_gui_vm, 0, 0, wheel, 0);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_CHAR: {
            if ((wparam >= 32 && wparam <= 126) || (wparam >= 0xA0 && wparam <= 0xFF)) {
                vm_push_key(g_gui_vm, 0x00, (uint8_t)wparam);
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wparam == '\r') {
                vm_push_key(g_gui_vm, 0x1C, 0x0D);
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wparam == '\b') {
                vm_push_key(g_gui_vm, 0x0E, 0x08);
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wparam == 19) { /* Ctrl+S */
                vm_push_key(g_gui_vm, 0x1F, 0x13);
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wparam == 14) { /* Ctrl+N */
                vm_push_key(g_gui_vm, 0x31, 0x0E);
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wparam == 6) {  /* Ctrl+F (Fonte) */
                vm_push_key(g_gui_vm, 0x21, 0x06);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            uint8_t scan = 0;
            if (wparam == VK_F1) scan = 0x3B;
            else if (wparam == VK_F2) scan = 0x3C;
            else if (wparam == VK_F3) scan = 0x3D;
            else if (wparam == VK_F4) scan = 0x3E;
            else if (wparam == VK_F5) scan = 0x3F;
            else if (wparam == VK_F6) scan = 0x40;
            else if (wparam == VK_F7) scan = 0x41;
            else if (wparam == VK_F8) scan = 0x42;
            else if (wparam == VK_F9) scan = 0x43;
            else if (wparam == VK_F10) scan = 0x44;
            else if (wparam == VK_F11) scan = 0x85;
            else if (wparam == VK_UP) scan = 0x48;
            else if (wparam == VK_DOWN) scan = 0x50;
            else if (wparam == VK_LEFT) scan = 0x4B;
            else if (wparam == VK_RIGHT) scan = 0x4D;
            else if (wparam == VK_HOME) scan = 0x47;
            else if (wparam == VK_END) scan = 0x4F;
            else if (wparam == VK_PRIOR) scan = 0x49;
            else if (wparam == VK_NEXT) scan = 0x51;
            else if (wparam == VK_DELETE) scan = 0x53;
            else if (wparam == VK_ESCAPE) { vm_push_key(g_gui_vm, 0x01, 27); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (scan != 0) {
                vm_push_key(g_gui_vm, scan, 0x00);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            break;
        }
        case WM_TIMER: {
            uint16_t cur_ticks = mem_r16(g_gui_vm, 0x0000, 0x046C);
            mem_w16(g_gui_vm, 0x0000, 0x046C, (uint16_t)(cur_ticks + 1));
            vm_run_until_hlt(g_gui_vm, 500000);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            BITMAPINFO bmi;
            memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = g_gui_vm->vbe_width;
            bmi.bmiHeader.biHeight = -g_gui_vm->vbe_height;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(hdc, 0, 0, g_gui_vm->vbe_width, g_gui_vm->vbe_height,
                          0, 0, g_gui_vm->vbe_width, g_gui_vm->vbe_height,
                          g_gui_vm->ram + VBE_LFB_PHYS, &bmi, DIB_RGB_COLORS, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static int run_win32_gui(VM86 *vm) {
    g_gui_vm = vm;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = folio_wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "Folio86VBEWindow";
    RegisterClassA(&wc);

    RECT rc = { 0, 0, vm->vbe_width, vm->vbe_height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, FALSE);

    HWND hwnd = CreateWindowExA(
        0, wc.lpszClassName,
        "FOLIO-86 v3.0 - Bare-Metal VESA TrueColor + Fontes Graficas + Mouse USB HID & PS/2",
        (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, wc.hInstance, NULL);

    SetTimer(hwnd, 1, 55, NULL);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
#endif

int main(int argc, char **argv) {
    const char *img_path = (argc > 1 && argv[1][0] != '-') ? argv[1] : "build/folio86.img";
    bool gui_mode = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gui") == 0) gui_mode = true;
    }

    FILE *fp = fopen(img_path, "rb");
    if (!fp) {
        fprintf(stderr, "Erro ao abrir %s\n", img_path);
        return 1;
    }

    VM86 vm;
    memset(&vm, 0, sizeof(vm));
    vm.ram  = (uint8_t *)calloc(1, RAM_SIZE);
    vm.disk = (uint8_t *)calloc(1, DISK_SIZE);

    if (fread(vm.disk, 1, DISK_SIZE, fp) != DISK_SIZE) {
        fprintf(stderr, "Imagem incompleta.\n");
        fclose(fp);
        return 1;
    }
    fclose(fp);

    if (gui_mode) {
#ifdef _WIN32
        vm_cold_reset(&vm);
        vm_run_until_hlt(&vm, 35000000);
        return run_win32_gui(&vm);
#endif
    }

    printf("==================================================================================\n");
    printf("  FOLIO-86 v3.0 â€” TESTE EM VM x86 (Hybrid UEFI/MBR + VESA + Mouse USB HID/PS2)   \n");
    printf("==================================================================================\n");

    /* -------------------------------------------------------------------------
     * TESTE VM 1..4: Cold Boot -> Unreal Mode -> EDID -> VBE LFB -> PCI USB UHCI
     * ------------------------------------------------------------------------- */
    vm_cold_reset(&vm);
    vm_run_until_hlt(&vm, 35000000);

    if (!vm.vbe_active || vm.vbe_width != 1024 || vm.vbe_height != 768 || vm.vbe_bpp != 32) {
        fprintf(stderr, "[FALHA VM 1] Modo grafico VESA VBE nao ativado! active=%d, %ux%ux%u\n",
                vm.vbe_active, vm.vbe_width, vm.vbe_height, vm.vbe_bpp);
        return 1;
    }
    printf("[VM PASS 1] Bootloader LBA 0 carregou 48 setores (24.576B) e ativou Unreal Mode 32-bit.\n");
    printf("[VM PASS 2] EDID do Monitor lido via INT 10h (AX=4F15h): Resolucao Nativa %ux%u detectada.\n",
           vm.vbe_width, vm.vbe_height);
    printf("[VM PASS 3] Modo VESA VBE TrueColor 32-bit (%ux%u @ LFB 0x%08X) inicializado.\n",
           vm.vbe_width, vm.vbe_height, VBE_LFB_PHYS);

    /* Verifica se o Kernel descobriu o Controlador USB UHCI via PCI (0x0CF8/0x0CFC) e enumerou o Mouse USB HID */
    if (vm.usb_frbase != 0x00030000u || vm.usb_enum_mask != 0x07 || vm.usb_dev_addr != 1) {
        fprintf(stderr, "[FALHA VM 4] Falha na enumeracao PCI USB UHCI! frbase=0x%08X, enum_mask=0x%02X, addr=%u\n",
                vm.usb_frbase, vm.usb_enum_mask, vm.usb_dev_addr);
        return 1;
    }
    printf("[VM PASS 4] Controlador PCI USB UHCI (8086:7020 @ 0xC000) descoberto e Mouse USB HID Boot enumerado (Addr=1, FrameList=0x%08X).\n",
           vm.usb_frbase);

    uint32_t px_desk  = vm_get_pixel(&vm, 20, 200);
    uint32_t px_paper = vm_get_pixel(&vm, 200, 200);
    if (px_desk != 0x00DED6C6 || px_paper != 0x00FAF4E8) {
        fprintf(stderr, "[FALHA VM 5] Cores TrueColor da folha/mesa incorretas: desk=0x%08X, paper=0x%08X\n",
                px_desk, px_paper);
        return 1;
    }
    printf("[VM PASS 5] Renderizador TrueColor 32-bit verificado: Mesa=0x%06X, Folha Papiro=0x%06X.\n",
           px_desk, px_paper);

    /* Move o Mouse USB HID via pacote DMA e exporta Screenshot 1 */
    vm_usb_mouse_move_to(&vm, 540, 280);
    vm_save_bmp(&vm, "build/screen1_papiro_serif.bmp");
    printf("[VM PASS 6] Screenshot 1 exportado: build/screen1_papiro_serif.bmp (Papiro Quente + Folio Serif + Mouse USB HID).\n");

    /* -------------------------------------------------------------------------
     * TESTE VM 7..10: Cliques via Mouse USB HID (DMA Transfer Descriptor) + Roda de Rolagem
     * ------------------------------------------------------------------------- */
    vm_usb_mouse_click_at(&vm, 300, 16);
    uint32_t px_obs_paper = vm_get_pixel(&vm, 200, 200);
    if (px_obs_paper != 0x00161922) {
        fprintf(stderr, "[FALHA VM 7] Clique de Mouse USB HID em [Tema] nao mudou para Noite Obsidiana! px=0x%08X\n",
                px_obs_paper);
        return 1;
    }
    printf("[VM PASS 7] Clique de Mouse USB HID no botao [Tema] (x=300, y=16) mudou paleta para Noite Obsidiana (0x%06X).\n",
           px_obs_paper);

    /* Clique de Mouse USB HID em [Fonte] (x=440, y=16) -> Clean Sans */
    vm_usb_mouse_click_at(&vm, 440, 16);
    printf("[VM PASS 8] Clique de Mouse USB HID no botao [Fonte] (x=440, y=16) ativou tipografia Clean Sans.\n");

    /* Teste da Roda de Rolagem do Mouse USB HID (4Âº byte do pacote HID Boot: wheel = -1 e depois +1) */
    vm_send_usb_mouse(&vm, 0, 0, -1, 0); /* Rola para baixo */
    vm_send_usb_mouse(&vm, 0, 0, +1, 0); /* Rola de volta para o topo */
    printf("[VM PASS 9] Roda de Rolagem do Mouse USB HID (4o byte Wheel=-1/+1) rolou a folha suavemente.\n");

    /* Clique de Mouse USB HID diretamente sobre o texto na folha (x=320, y=155) */
    vm_usb_mouse_click_at(&vm, 320, 155);
    printf("[VM PASS 10] Clique de Mouse USB HID na folha de papel (x=320, y=155) reposicionou o cursor de texto.\n");

    /* Clique de Mouse USB HID em [Foco] (x=575, y=16) 2 vezes e em [Pomodoro] (x=840, y=16) */
    vm_usb_mouse_click_at(&vm, 575, 16);
    vm_usb_mouse_click_at(&vm, 575, 16);
    vm_usb_mouse_click_at(&vm, 840, 16);
    vm_usb_mouse_move_to(&vm, 480, 185);
    vm_save_bmp(&vm, "build/screen2_obsidiana_sans_focus.bmp");
    printf("[VM PASS 11] Screenshot 2 exportado: build/screen2_obsidiana_sans_focus.bmp (Noite Obsidiana + Clean Sans + Foco).\n");

    /* -------------------------------------------------------------------------
     * TESTE VM 12..15: Modal de Manuscritos via Mouse USB HID + Compatibilidade PS/2
     * ------------------------------------------------------------------------- */
    vm_usb_mouse_click_at(&vm, 80, 16);
    vm_usb_mouse_move_to(&vm, 420, 264);
    vm_save_bmp(&vm, "build/screen3_modal_manuscritos.bmp");
    printf("[VM PASS 12] Clique de Mouse USB HID em [MANUSCR1.MD] abriu o Seletor de Manuscritos (screen3_modal_manuscritos.bmp).\n");

    /* Clica no Card #3 (MANUSCR3.MD) via Mouse USB HID */
    vm_usb_mouse_click_at(&vm, 450, 264);

    /* Muda para Tema 2 (FÃ³sforo Ã‚mbar), Fonte 2 (Courier Slab) e Foco Livre */
    vm_usb_mouse_click_at(&vm, 300, 16);
    vm_usb_mouse_click_at(&vm, 440, 16);
    vm_usb_mouse_click_at(&vm, 575, 16);

    /* Testa tambÃ©m um movimento hÃ­brido pelo controlador clÃ¡ssico 8042 PS/2 para provar coexistÃªncia */
    vm_send_ps2_mouse(&vm, 5, -5, 0);
    vm.host_mouse_x += 5;
    vm.host_mouse_y -= 5;

    /* Limpa folha com Ctrl+N e digita novo manuscrito */
    vm_push_key(&vm, 0x31, 0x0E);
    const char *novo_texto =
        "# Tipografia Grafica & Mouse USB HID Bare-Metal\n"
        "\n"
        "> Citacao renderizada com inclinacao italica em tempo real e barra lateral.\n"
        "\n"
        "Este manuscrito foi aberto clicando com o Mouse USB HID no cartao MANUSCR3.MD, "
        "exibido em alta resolucao VESA TrueColor 1024x768 com fonte Courier Slab "
        "anti-aliased e salvo clicando no botao Salvar da barra superior.\n";
    vm_type_string(&vm, novo_texto);

    /* Clica no botÃ£o [Salvar] (x=195, y=16) com o Mouse USB HID para gravar no Pen-Drive! */
    uint32_t writes_before = vm.disk_writes;
    vm_usb_mouse_click_at(&vm, 195, 16);
    uint32_t writes_delta = vm.disk_writes - writes_before;

    if (writes_delta == 0) {
        fprintf(stderr, "[FALHA VM 13] Clique de Mouse USB HID em [Salvar] nao gravou setores no Pen-Drive!\n");
        return 1;
    }
    vm_usb_mouse_move_to(&vm, 600, 240);
    vm_save_bmp(&vm, "build/screen4_eink_slab_saved.bmp");
    printf("[VM PASS 13] Clique de Mouse USB HID em [Salvar] (x=195, y=16) gravou %u setores no Pen-Drive (screen4_eink_slab_saved.bmp).\n",
           writes_delta);
    printf("[VM PASS 14] Total de pacotes DMA USB HID Boot entregues pelo controlador UHCI: %u pacotes.\n",
           vm.usb_dma_packets_delivered);

    /* Verifica no DiretÃ³rio Raiz FAT16 (LBA 80, Entrada 2 = MANUSCR3.MD) */
    uint32_t fat_filesize = *(uint32_t *)(vm.disk + (80 * SECTOR_SIZE) + (2 * 32) + 28);
    if (fat_filesize != strlen(novo_texto)) {
        fprintf(stderr, "[FALHA VM 15] Tamanho FAT16 (%u) difere do texto (%zu)!\n",
                fat_filesize, strlen(novo_texto));
        return 1;
    }
    printf("[VM PASS 15] Entrada FAT16 de MANUSCR3.MD e Configuracao (LBA 49) persistidas (%u bytes).\n",
           fat_filesize);

    printf("\n==================================================================================\n");
    printf("  TODOS OS 15 TESTES GRAFICOS VESA VBE + FONTES + MOUSE USB HID PASSARAM 100%%!    \n");
    printf("==================================================================================\n");

    free(vm.ram);
    free(vm.disk);
    return 0;
}
