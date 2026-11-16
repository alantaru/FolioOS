; =============================================================================
; FOLIO-86 v2.1 — Bare-Metal Graphical & Native USB Mouse Edition
; Arquivo: src/kernel.asm (Carregado em 0x0800:0x0000, LBA 1..48)
; Recursos v2.1:
;   - Big Real Mode (Unreal Mode 4 GB Flat Addressing via FS:[EDI])
;   - Resolução Nativa do Monitor via VESA VBE 2.0/3.0 LFB 32-bit + EDID
;   - 3 Fontes Gráficas (Folio Serif, Clean Sans, Courier Slab) com Anti-Aliasing
;   - Negrito Tipográfico (# Título) + Transformação Itálica em Tempo Real (> Citação)
;   - Pilha Tripla de Mouse Bare-Metal:
;       1. Driver Nativo PCI + USB Host Controller (UHCI) + USB HID Boot Protocol
;          com suporte a Roda de Rolagem (Scroll Wheel) via DMA em 0x3000:0x0000
;       2. Callback BIOS INT 15h (AX=C207h) para SMM USB Legacy / Touchpads
;       3. Driver Clássico 8042 PS/2 (Portas 0x60/0x64)
; =============================================================================

BITS 16
ORG 0x0000

KERNEL_SEG          EQU 0x0800
DOC_SEG             EQU 0x1000      ; 32 KB para texto (0x10000, alinhado p/ DMA)
WORK_SEG            EQU 0x2000      ; Tabela de linhas visuais (LINE_TABLE_OFF)
LINE_TABLE_OFF      EQU 0x1000

USB_DMA_SEG         EQU 0x3000      ; 0x00030000 (Alinhado em 4 KB p/ UHCI Frame List)
USB_FL_PHYS         EQU 0x00030000  ; 1024 DWORDs Frame List (0x3000:0x0000)
USB_QH_OFF          EQU 0x1000      ; Queue Head em 0x3000:0x1000 (Físico 0x00031000)
USB_QH_PHYS         EQU 0x00031000
USB_TD_OFF          EQU 0x1020      ; Interrupt-IN TD em 0x3000:0x1020 (Físico 0x00031020)
USB_TD_PHYS         EQU 0x00031020
USB_BUF_OFF         EQU 0x1040      ; Buffer 4-byte HID Report (Físico 0x00031040)
USB_BUF_PHYS        EQU 0x00031040
USB_CTRL_TD_OFF     EQU 0x1100      ; TDs de Controle para Enumeração USB HID Boot
USB_CTRL_TD_PHYS    EQU 0x00031100
USB_SETUP_BUF_OFF   EQU 0x1160      ; Pacote Setup de 8 bytes (Físico 0x00031160)
USB_SETUP_BUF_PHYS  EQU 0x00031160

BACKBUF_PHYS        EQU 0x00200000  ; Backbuffer 32-bit TrueColor em 2 MB Físico
DEFAULT_LFB_PHYS    EQU 0xE0000000  ; Endereço LFB padrão VESA VBE

MAX_DOC_LEN         EQU 32700
MAX_VISUAL_LINES    EQU 3000

CONFIG_LBA          EQU 49          ; Setor reservado p/ configurações do usuário
ROOT_DIR_LBA        EQU 80          ; Primeiro setor do Diretório Raiz FAT16
DATA_START_LBA      EQU 112         ; Início do Slot 0 (MANUSCR1.MD)
SECTORS_PER_SLOT    EQU 64          ; 64 setores * 512B = 32 KB por manuscrito

WRAP_WIDTH          EQU 60          ; 60 colunas tipográficas por linha
CHAR_W              EQU 10          ; Largura de célula gráfica (10 px)
CHAR_H              EQU 16          ; Altura do glifo (16 px)
LINE_H              EQU 26          ; Altura de linha com entrelinha editorial (26 px)
TEXT_WIN_ROWS       EQU 22          ; 22 linhas visíveis na folha gráfica

; =============================================================================
; Ponto de Entrada do Kernel (CS = 0x0800, IP = 0x0000, DL = boot_drive)
; =============================================================================
kernel_start:
    cli
    mov ax, KERNEL_SEG
    mov ds, ax
    mov es, ax
    xor ax, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    mov [boot_drive], dl

    ; 1. Ativa Big Real Mode (Unreal Mode) com limite de 4 GB em FS e GS
    call enable_unreal_mode

    ; 2. Inicializa subsistema de disco USB (INT 13h LBA/CHS) e carrega Config
    call disk_init
    call disk_load_config

    ; 3. Detecta resolução nativa do monitor (EDID) e inicializa VESA VBE 32-bit LFB
    call vbe_init
    call load_theme_palette

    ; 4. Inicializa Pilha Tripla de Mouse (USB HID via PCI/UHCI + BIOS INT 15h + PS/2 8042)
    call mouse_init

    ; 5. Carrega o manuscrito ativo do Pen-Drive USB
    call disk_load_slot

    ; 6. Inicializa relógio de hardware e renderiza o primeiro quadro gráfico
    call read_bios_ticks
    mov [last_tick_lo], ax
    mov [autosave_tick_lo], ax

    call compute_stats
    call layout_document
    call render_screen

; =============================================================================
; Loop Principal de Eventos (Mouse USB HID + Mouse PS/2 + Teclado + Pomodoro)
; =============================================================================
main_loop:
    ; 1. Verifica pacotes DMA do Mouse USB HID e pacotes do Controlador 8042 PS/2
    call mouse_poll
    cmp byte [mouse_dirty], 0
    je .check_kbd
    mov byte [mouse_dirty], 0
    call compute_stats
    call layout_document
    call render_screen

.check_kbd:
    ; 2. Verifica tecla disponível no buffer da BIOS (INT 16h AH=01h)
    mov ah, 0x01
    int 0x16
    jnz .handle_key

    ; 3. Atualiza temporizadores (Pomodoro 18.2 Hz e Auto-Save)
    call check_timers

    ; 4. Baixo consumo de CPU até a próxima interrupção
    hlt
    jmp main_loop

.handle_key:
    mov ah, 0x00
    int 0x16

    mov word [toast_ptr], 0

    cmp byte [modal_mode], 0
    jne .handle_modal_key

    call process_key
    call compute_stats
    call layout_document

    ; Drena teclas pendentes no buffer da BIOS antes de redesenhar o quadro VESA
    mov ah, 0x01
    int 0x16
    jnz .handle_key

    call render_screen
    jmp main_loop

.handle_modal_key:
    cmp byte [modal_mode], 1
    je .modal_help
    cmp byte [modal_mode], 2
    je .modal_slots
    mov byte [modal_mode], 0
    call render_screen
    jmp main_loop

.modal_help:
    mov byte [modal_mode], 0
    call render_screen
    jmp main_loop

.modal_slots:
    cmp al, 27
    je .close_slot_modal
    cmp al, '1'
    jb .ignore_slot_key
    cmp al, '8'
    ja .ignore_slot_key

    sub al, '1'
    mov bl, al
    mov byte [modal_mode], 0
    call switch_to_slot
    call compute_stats
    call layout_document
    call render_screen
    jmp main_loop

.close_slot_modal:
    mov byte [modal_mode], 0
    call render_screen
.ignore_slot_key:
    jmp main_loop

; =============================================================================
; Big Real Mode (Unreal Mode) — Habilita Endereçamento Físico de 32 bits (4 GB)
; =============================================================================
enable_unreal_mode:
    ; Habilita linha A20 via BIOS e Fast A20 (Porta 0x92)
    mov ax, 0x2401
    int 0x15
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al

    ; Calcula endereço linear da tabela GDT: (KERNEL_SEG << 4) + gdt_start
    xor eax, eax
    mov ax, KERNEL_SEG
    shl eax, 4
    add eax, gdt_start
    mov [gdt_ptr + 2], eax

    cli
    lgdt [gdt_ptr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp short .pmode_flush
.pmode_flush:
    mov bx, 0x0008                  ; Seletor Flat 4 GB Data
    mov fs, bx
    mov gs, bx
    and al, 0xFE
    mov cr0, eax
    jmp short .rmode_flush
.rmode_flush:
    xor bx, bx
    mov fs, bx
    mov gs, bx
    mov ax, KERNEL_SEG
    mov ds, ax
    mov es, ax
    sti
    ret

align 8
gdt_start:
    dq 0x0000000000000000           ; 0x00: Descritor Nulo
    dq 0x00CF92000000FFFF           ; 0x08: Base=0, Limit=4GB, 32-bit Flat Data
gdt_end:

gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd 0x00000000

; =============================================================================
; Inicialização Gráfica VESA VBE 2.0/3.0 + Leitura de Resolução Nativa EDID
; =============================================================================
vbe_init:
    push es
    mov ax, KERNEL_SEG
    mov es, ax

    ; 1. Tenta ler EDID do Monitor via VBE/DDC (INT 10h AX=4F15h BL=01h)
    mov ax, 0x4F15
    mov bl, 0x01
    xor cx, cx
    xor dx, dx
    mov di, vbe_edid_buf
    int 0x10
    cmp ax, 0x004F
    jne .edid_fallback

    ; Verifica assinatura EDID (00 FF FF FF FF FF FF 00)
    cmp word [vbe_edid_buf], 0xFF00
    jne .edid_fallback
    cmp word [vbe_edid_buf + 2], 0xFFFF
    jne .edid_fallback

    ; Extrai resolução horizontal e vertical preferida do Detailed Timing Descriptor (Byte 54)
    movzx ax, byte [vbe_edid_buf + 54 + 2]
    movzx bx, byte [vbe_edid_buf + 54 + 4]
    shr bx, 4
    shl bx, 8
    or ax, bx
    cmp ax, 640
    jb .edid_fallback
    mov [target_w], ax

    movzx ax, byte [vbe_edid_buf + 54 + 5]
    movzx bx, byte [vbe_edid_buf + 54 + 7]
    shr bx, 4
    shl bx, 8
    or ax, bx
    cmp ax, 480
    jb .edid_fallback
    mov [target_h], ax
    jmp .scan_vbe_modes

.edid_fallback:
    mov word [target_w], 1024
    mov word [target_h], 768

.scan_vbe_modes:
    ; Procura na tabela de modos candidatos um modo 32-bit LFB que coincida com target_w x target_h
    mov si, candidate_vbe_modes
    mov word [best_mode], 0x0118    ; Fallback padrão 1024x768x32

.mode_loop:
    lodsw
    test ax, ax
    jz .activate_vbe
    mov cx, ax
    push si
    mov ax, 0x4F01
    mov di, vbe_mode_info
    int 0x10
    pop si
    cmp ax, 0x004F
    jne .mode_loop

    ; Verifica suporte a Linear Framebuffer (bit 7) + Graphics (bit 4) + 32 bpp
    mov ax, [vbe_mode_info]
    and ax, 0x0091
    cmp ax, 0x0091
    jne .mode_loop
    cmp byte [vbe_mode_info + 25], 32
    jne .mode_loop

    mov [best_mode], cx
    mov ax, [vbe_mode_info + 18]    ; XResolution
    cmp ax, [target_w]
    jne .mode_loop
    mov ax, [vbe_mode_info + 20]    ; YResolution
    cmp ax, [target_h]
    jne .mode_loop

    ; Encontrou resolução exata do monitor!
    jmp .activate_vbe

.activate_vbe:
    ; Consulta informações finais do modo vencedor
    mov cx, [best_mode]
    mov ax, 0x4F01
    mov di, vbe_mode_info
    int 0x10

    mov ax, [vbe_mode_info + 18]
    cmp ax, 640
    jae .w_ok
    mov ax, 1024
.w_ok:
    mov [screen_w], ax

    mov ax, [vbe_mode_info + 20]
    cmp ax, 480
    jae .h_ok
    mov ax, 768
.h_ok:
    mov [screen_h], ax

    mov eax, [vbe_mode_info + 40]   ; PhysBasePtr (32-bit LFB address)
    test eax, eax
    jnz .lfb_ok
    mov eax, DEFAULT_LFB_PHYS
.lfb_ok:
    mov [vbe_lfb_phys], eax

    ; Ativa o modo gráfico VESA VBE com Linear Framebuffer (bit 14 = 0x4000)
    mov bx, [best_mode]
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10

    ; Calcula layout responsivo centralizado de acordo com a resolução do monitor
    ; Folha de papel com 760px de largura centralizada horizontalmente
    mov ax, [screen_w]
    sub ax, 760
    jnc .center_ok
    xor ax, ax
.center_ok:
    shr ax, 1
    mov [paper_x], ax
    add ax, 80                      ; Margem interna esquerda de 80px (600px de texto)
    mov [text_x], ax

    mov word [paper_y], 44
    mov ax, [screen_h]
    sub ax, 88
    mov [paper_h], ax
    mov word [text_y], 70

    ; Posiciona o cursor do mouse inicialmente de forma discreta no canto direito da página
    mov ax, [paper_x]
    add ax, 700
    mov [mouse_x], ax
    mov ax, [paper_y]
    add ax, 60
    mov [mouse_y], ax

    pop es
    ret

candidate_vbe_modes:
    dw 0x0118, 0x011B, 0x0115, 0x0144, 0x0147, 0x0160, 0x017D, 0x0000

; =============================================================================
; Pilha Tripla de Mouse Bare-Metal:
;   1. Driver Nativo PCI + USB Host Controller (UHCI) + USB HID Boot Protocol
;   2. Callback BIOS INT 15h (AX=C207h) para SMM USB Legacy / Touchpads
;   3. Controlador Clássico 8042 PS/2 (Portas 0x60 e 0x64)
; =============================================================================
mouse_init:
    ; 1. Inicializa Controlador 8042 PS/2 Auxiliar
    cli
    mov al, 0xA8                    ; Enable Auxiliary PS/2 Device
    out 0x64, al

    ; Configura Command Byte do 8042 para habilitar Aux Port + Aux Clock
    mov al, 0x20
    out 0x64, al
    in al, 0x60
    and al, 0xDF                    ; Limpa bit 5 (Enable Mouse Clock)
    or al, 0x02                     ; Seta bit 1 (Enable Aux IRQ12 p/ SMM USB Legacy)
    mov bl, al
    mov al, 0x60
    out 0x64, al
    mov al, bl
    out 0x60, al

    ; Envia 0xD4 + 0xF4 (Enable Data Reporting) ao dispositivo PS/2
    mov al, 0xD4
    out 0x64, al
    mov al, 0xF4
    out 0x60, al
    in al, 0x60
    mov byte [mouse_pkt_idx], 0
    sti

    ; 2. Registra Callback BIOS INT 15h (AH=C2h) para USB Legacy SMM / Touchpads
    call bios_mouse_init

    ; 3. Inicializa Driver Nativo PCI + USB UHCI Host Controller (USB HID Boot Mouse)
    call usb_mouse_init
    ret

; -----------------------------------------------------------------------------
; Camada 2: BIOS Pointing Device Service (INT 15h AX=C2xx)
; -----------------------------------------------------------------------------
bios_mouse_init:
    push es
    mov ax, 0xC205                  ; Inicializa protocolo de 3 bytes
    mov bh, 0x03
    int 0x15
    jc .no_bios_mouse

    mov ax, 0xC203                  ; Resolução padrão
    mov bh, 0x02
    int 0x15

    mov ax, KERNEL_SEG
    mov es, ax
    mov bx, bios_mouse_callback
    mov ax, 0xC207                  ; Registra Handler ES:BX
    int 0x15
    jc .no_bios_mouse

    mov ax, 0xC200                  ; Habilita Pointing Device (BH=01h)
    mov bh, 0x01
    int 0x15
.no_bios_mouse:
    pop es
    ret

; Callback Far invocado pela BIOS SMM quando o mouse USB/Touchpad move ou clica
; Pilha na entrada (convenção BIOS INT 15h C207h):
;   [BP+12] = Status, [BP+10] = dX, [BP+8] = dY, [BP+6] = Z
bios_mouse_callback:
    push bp
    mov bp, sp
    push ax
    push bx
    push cx
    push dx
    push ds

    mov ax, KERNEL_SEG
    mov ds, ax

    mov al, [bp + 12]
    mov [bios_pkt_status], al
    mov al, [bp + 10]
    mov [bios_pkt_dx], al
    mov al, [bp + 8]
    mov [bios_pkt_dy], al
    mov byte [bios_pkt_ready], 1

    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    pop bp
    retf

; -----------------------------------------------------------------------------
; Camada 1: Driver Nativo PCI + USB UHCI Host Controller + USB HID Boot Mouse
; -----------------------------------------------------------------------------
; Lê DWORD da configuração PCI: BX = (bus << 8) | dev_func, CL = offset -> EAX
pci_read_dword:
    push dx
    xor eax, eax
    mov ax, bx
    shl eax, 8
    mov al, cl
    and al, 0xFC
    or eax, 0x80000000
    mov dx, 0x0CF8
    out dx, eax
    mov dx, 0x0CFC
    in eax, dx
    pop dx
    ret

; Escreve DWORD EAX na configuração PCI: BX = (bus << 8) | dev_func, CL = offset
pci_write_dword:
    push dx
    push eax
    xor eax, eax
    mov ax, bx
    shl eax, 8
    mov al, cl
    and al, 0xFC
    or eax, 0x80000000
    mov dx, 0x0CF8
    out dx, eax
    pop eax
    mov dx, 0x0CFC
    out dx, eax
    pop dx
    ret

usb_mouse_init:
    push es
    mov byte [usb_mouse_active], 0

    ; 1. Varre o Barramento PCI 0 (dev_func = 0..255) buscando USB UHCI (Class 0x0C0300)
    xor bx, bx
.pci_scan_loop:
    mov cl, 0x00                    ; Vendor / Device ID
    call pci_read_dword
    cmp ax, 0xFFFF
    je .next_pci_func

    mov cl, 0x08                    ; Revision / ProgIF / Subclass / Class
    call pci_read_dword
    shr eax, 8                      ; EAX[23:0] = Class(0C) Subclass(03) ProgIF(00)
    cmp eax, 0x000C0300
    je .found_uhci

.next_pci_func:
    inc bx
    cmp bx, 256
    jb .pci_scan_loop
    pop es
    ret

.found_uhci:
    ; 2. Lê BAR4 (Offset 0x20) para obter o endereço base de I/O do controlador USB
    mov cl, 0x20
    call pci_read_dword
    and ax, 0xFFE0
    jz .next_pci_func
    mov [usb_io_base], ax

    ; 3. Habilita I/O Space (Bit 0) + Bus Master DMA (Bit 2) no PCI Command Register (Offset 0x04)
    mov cl, 0x04
    call pci_read_dword
    or eax, 0x00000005
    call pci_write_dword

    ; 4. Reset Global e HCRESET do Controlador USB UHCI
    mov dx, [usb_io_base]           ; USBCMD (base + 0x00)
    mov ax, 0x0002                  ; HCRESET
    out dx, ax

    mov dx, [usb_io_base]
    add dx, 0x02                    ; USBSTS (base + 0x02)
    mov ax, 0x003F                  ; Limpa bits de status pendentes
    out dx, ax

    mov dx, [usb_io_base]
    add dx, 0x04                    ; USBINTR (base + 0x04)
    xor ax, ax                      ; Desabilita interrupções (usamos DMA Polling)
    out dx, ax

    mov dx, [usb_io_base]
    add dx, 0x06                    ; FRNUM (base + 0x06)
    xor ax, ax
    out dx, ax

    ; 5. Verifica e reseta as portas do Root Hub (PORTSC1 = base+0x10, PORTSC2 = base+0x12)
    mov dx, [usb_io_base]
    add dx, 0x10                    ; PORTSC1
    in ax, dx
    test ax, 0x0001                 ; Bit 0 = Current Connect Status
    jnz .reset_usb_port

    mov dx, [usb_io_base]
    add dx, 0x12                    ; PORTSC2
    in ax, dx
    test ax, 0x0001
    jz .no_usb_device

.reset_usb_port:
    ; Aplica Port Reset (Bit 9 = 0x0200) e depois habilita a porta (Bit 2 = Port Enable)
    mov ax, 0x0200
    out dx, ax
    mov cx, 1024
.rst_wait:
    nop
    loop .rst_wait
    mov ax, 0x000E                  ; Limpa CSC/PEC e ativa Port Enable (Bit 2)
    out dx, ax

    ; 6. Constrói as estruturas DMA do UHCI no segmento físico 0x3000 (0x00030000)
    mov ax, USB_DMA_SEG
    mov es, ax

    ; Limpa 8 KB da região DMA (0x3000:0x0000 .. 0x3000:0x1FFF)
    xor di, di
    mov cx, 4096
    xor ax, ax
    rep stosw

    ; Preenche as 1024 entradas da Frame List (0x3000:0x0000) apontando para o QH (0x00031002)
    xor di, di
    mov cx, 1024
    mov eax, USB_QH_PHYS | 0x00000002
.fill_fl:
    mov [es:di], eax
    add di, 4
    loop .fill_fl

    ; Configura o Queue Head (QH em 0x3000:0x1000)
    mov dword [es:USB_QH_OFF + 0], 0x00000001       ; Head Link Pointer = Terminate
    mov dword [es:USB_QH_OFF + 4], USB_TD_PHYS      ; Element Link Pointer -> Interrupt-IN TD

    ; Configura o endereço base da Frame List no registrador FRBASEADD (base + 0x08)
    mov dx, [usb_io_base]
    add dx, 0x08
    mov eax, USB_FL_PHYS
    out dx, eax

    ; 7. Executa Enumeração USB HID Boot (SET_ADDRESS=1, SET_CONFIGURATION=1, SET_PROTOCOL=0)
    ; Pacote 1: SET_ADDRESS (bmRequestType=0x00, bRequest=0x05, wValue=1, wIndex=0, wLength=0)
    mov dword [es:USB_SETUP_BUF_OFF + 0], 0x00010500
    mov dword [es:USB_SETUP_BUF_OFF + 4], 0x00000000
    xor al, al                      ; Dispositivo ainda no endereço 0
    call usb_exec_control_no_data

    ; Pacote 2: SET_CONFIGURATION (bmRequestType=0x00, bRequest=0x09, wValue=1, wIndex=0, wLength=0)
    mov dword [es:USB_SETUP_BUF_OFF + 0], 0x00010900
    mov dword [es:USB_SETUP_BUF_OFF + 4], 0x00000000
    mov al, 1                       ; Endereço 1
    call usb_exec_control_no_data

    ; Pacote 3: SET_PROTOCOL = Boot Protocol (bmRequestType=0x21, bRequest=0x0B, wValue=0, wIndex=0)
    mov dword [es:USB_SETUP_BUF_OFF + 0], 0x00000B21
    mov dword [es:USB_SETUP_BUF_OFF + 4], 0x00000000
    mov al, 1                       ; Endereço 1
    call usb_exec_control_no_data

    ; 8. Arma o Transfer Descriptor (TD) de Interrupção IN (Endpoint 1, 4 bytes HID Boot Report)
    mov dword [es:USB_TD_OFF + 0], 0x00000001       ; Link Pointer = Terminate
    mov dword [es:USB_TD_OFF + 4], 0x18800000       ; Control/Status: Active=1 (Bit 23), MaxErr=3
    ; Token: MaxLen=3 (4 bytes: 3<<21 = 0x00600000), EndPt=1 (1<<15 = 0x00008000), Addr=1 (1<<8), PID=0x69 (IN)
    mov dword [es:USB_TD_OFF + 8], 0x00608169
    mov dword [es:USB_TD_OFF + 12], USB_BUF_PHYS    ; Ponteiro do buffer DMA (0x00031040)

    ; Aponta o Queue Head para o TD de Interrupção e ativa o escalonador UHCI (Run/Stop = 1)
    mov dword [es:USB_QH_OFF + 4], USB_TD_PHYS
    mov dx, [usb_io_base]           ; USBCMD (base + 0x00)
    mov ax, 0x00C1                  ; Run/Stop (Bit 0) + Configure Flag (Bit 6) + MaxPacket 64 (Bit 7)
    out dx, ax

    mov byte [usb_mouse_active], 1
    pop es
    ret

.no_usb_device:
    pop es
    ret

; Executa uma transferência de controle USB sem estágio de dados (SETUP + STATUS IN)
; Entrada: AL = endereço do dispositivo (0 ou 1), ES = USB_DMA_SEG
usb_exec_control_no_data:
    push ax
    push cx
    push dx

    ; TD 1 (SETUP): 0x3000:0x1100 -> aponta para TD 2 (0x00031120)
    mov dword [es:USB_CTRL_TD_OFF + 0], (USB_CTRL_TD_PHYS + 0x20)
    mov dword [es:USB_CTRL_TD_OFF + 4], 0x18800000  ; Active=1, MaxErr=3
    movzx edx, al
    shl edx, 8
    or edx, 0x00E0002D              ; MaxLen=7 (8 bytes), DataToggle=0, PID=0x2D (SETUP)
    mov [es:USB_CTRL_TD_OFF + 8], edx
    mov dword [es:USB_CTRL_TD_OFF + 12], USB_SETUP_BUF_PHYS

    ; TD 2 (STATUS IN): 0x3000:0x1120 -> Terminate
    mov dword [es:USB_CTRL_TD_OFF + 0x20 + 0], 0x00000001
    mov dword [es:USB_CTRL_TD_OFF + 0x20 + 4], 0x18800000
    movzx edx, al
    shl edx, 8
    or edx, 0xFFE80069              ; MaxLen=0x7FF (Null Length), DataToggle=1, PID=0x69 (IN)
    mov [es:USB_CTRL_TD_OFF + 0x20 + 8], edx
    mov dword [es:USB_CTRL_TD_OFF + 0x20 + 12], 0x00000000

    ; Conecta ao Queue Head e dispara 1 quadro UHCI
    mov dword [es:USB_QH_OFF + 4], USB_CTRL_TD_PHYS
    mov dx, [usb_io_base]
    mov ax, 0x00C1
    out dx, ax

    ; Aguarda conclusão do TD 2 (ou timeout curto em hardware lento)
    mov cx, 2048
.wait_ctrl:
    test dword [es:USB_CTRL_TD_OFF + 0x20 + 4], 0x00800000
    jz .ctrl_done
    loop .wait_ctrl
.ctrl_done:
    pop dx
    pop cx
    pop ax
    ret

; -----------------------------------------------------------------------------
; Polling Unificado de Mouse (USB HID DMA + BIOS INT 15h + 8042 PS/2)
; -----------------------------------------------------------------------------
mouse_poll:
    ; 1. Polling DMA do Mouse USB HID (UHCI Transfer Descriptor em 0x3000:0x1020)
    cmp byte [usb_mouse_active], 0
    je .check_bios_cb
    call usb_mouse_poll

.check_bios_cb:
    ; 2. Verifica pacotes entregues via Callback BIOS INT 15h (SMM USB Legacy)
    cmp byte [bios_pkt_ready], 0
    je .check_ps2_port
    mov byte [bios_pkt_ready], 0
    mov bl, [bios_pkt_status]
    and bl, 0x07
    movsx cx, byte [bios_pkt_dx]
    movsx si, byte [bios_pkt_dy]
    neg si                          ; BIOS/PS2: +dY é para cima -> inverte para coordenada de tela
    xor di, di                      ; Sem scroll wheel no pacote de 3 bytes
    call apply_mouse_delta

.check_ps2_port:
    ; 3. Polling da Porta Auxiliar 8042 PS/2 (Portas 0x64 e 0x60)
    in al, 0x64
    and al, 0x21
    cmp al, 0x21
    jne .poll_done

    in al, 0x60
    movzx bx, byte [mouse_pkt_idx]
    mov [mouse_pkt + bx], al
    inc byte [mouse_pkt_idx]
    cmp byte [mouse_pkt_idx], 3
    jb .check_ps2_port

    mov byte [mouse_pkt_idx], 0

    mov bl, [mouse_pkt]
    and bl, 0x07
    movsx cx, byte [mouse_pkt + 1]  ; dX
    movsx si, byte [mouse_pkt + 2]  ; dY (PS/2: positivo = cima)
    neg si                          ; Converte para coordenada Y de tela (positivo = baixo)
    xor di, di                      ; Sem roda de rolagem no protocolo PS/2 padrão
    call apply_mouse_delta
    jmp .check_ps2_port

.poll_done:
    ret

; Verifica se o Host Controller USB UHCI gravou pacotes HID Boot Report via DMA
usb_mouse_poll:
    push es
    mov ax, USB_DMA_SEG
    mov es, ax

.usb_pkt_loop:
    ; Verifica se o Bit 23 (Active) do TD (0x3000:0x1024) foi limpo pelo controlador USB
    mov eax, [es:USB_TD_OFF + 4]
    test eax, 0x00800000
    jnz .usb_poll_done

    ; Lê o relatório de 4 bytes do protocolo USB HID Boot Mouse em 0x3000:0x1040:
    ;   Byte 0 = Botões (Bit 0 = Esq, Bit 1 = Dir, Bit 2 = Meio)
    ;   Byte 1 = dX (int8: positivo = Direita)
    ;   Byte 2 = dY (int8: no padrão USB HID, positivo = Baixo!)
    ;   Byte 3 = Wheel (int8: positivo = Rolar p/ Cima, negativo = Rolar p/ Baixo)
    mov bl, [es:USB_BUF_OFF + 0]
    and bl, 0x07
    movsx cx, byte [es:USB_BUF_OFF + 1]
    movsx si, byte [es:USB_BUF_OFF + 2]
    movsx di, byte [es:USB_BUF_OFF + 3]

    ; Limpa o buffer DMA e rearma o Transfer Descriptor com inversão de Data Toggle (Bit 19)
    mov dword [es:USB_BUF_OFF], 0
    xor dword [es:USB_TD_OFF + 8], 0x00080000
    mov dword [es:USB_TD_OFF + 4], 0x18800000
    mov dword [es:USB_QH_OFF + 4], USB_TD_PHYS

    call apply_mouse_delta

    ; Pede ao controlador UHCI para processar eventual pacote seguinte na fila
    mov dx, [usb_io_base]
    mov ax, 0x00C1
    out dx, ax
    jmp .usb_pkt_loop

.usb_poll_done:
    pop es
    ret

; Aplica movimento (CX = dX, SI = dY_tela), Roda de Rolagem (DI = dWheel) e Botões (BL)
apply_mouse_delta:
    ; 1. Atualiza X
    add cx, [mouse_x]
    cmp cx, 0
    jge .x_min_ok
    xor cx, cx
.x_min_ok:
    mov dx, [screen_w]
    dec dx
    cmp cx, dx
    jle .x_max_ok
    mov cx, dx
.x_max_ok:
    mov [mouse_x], cx

    ; 2. Atualiza Y
    add si, [mouse_y]
    cmp si, 0
    jge .y_min_ok
    xor si, si
.y_min_ok:
    mov dx, [screen_h]
    dec dx
    cmp si, dx
    jle .y_max_ok
    mov si, dx
.y_max_ok:
    mov [mouse_y], si

    ; 3. Aplica Roda de Rolagem USB (Scroll Wheel: DI > 0 sobe, DI < 0 desce)
    test di, di
    jz .check_btn_edge
    cmp di, 0
    jg .wheel_up
    ; Rolar para baixo (+2 linhas)
    mov ax, [scroll_line]
    add ax, 2
    mov dx, [total_lines]
    dec dx
    cmp ax, dx
    jbe .set_wheel_scroll
    mov ax, dx
    jmp .set_wheel_scroll
.wheel_up:
    ; Rolar para cima (-2 linhas)
    mov ax, [scroll_line]
    sub ax, 2
    jnc .set_wheel_scroll
    xor ax, ax
.set_wheel_scroll:
    mov [scroll_line], ax
    mov byte [typewriter_mode], 0   ; Desativa trava central ao rolar manualmente com a roda

.check_btn_edge:
    mov byte [mouse_dirty], 1

    ; 4. Verifica borda de subida do clique esquerdo (transição 0 -> 1)
    mov al, bl
    and al, 0x01
    mov ah, [mouse_prev_btn]
    mov [mouse_prev_btn], al
    cmp al, 1
    jne .delta_done
    cmp ah, 0
    jne .delta_done

    call handle_mouse_click
.delta_done:
    ret

; Trata clique esquerdo do mouse em (mouse_x, mouse_y)
handle_mouse_click:
    mov word [toast_ptr], 0

    ; Se modal de ajuda estiver aberto, fecha
    cmp byte [modal_mode], 1
    jne .check_slot_modal_click
    mov byte [modal_mode], 0
    ret

.check_slot_modal_click:
    cmp byte [modal_mode], 2
    jne .check_zen_restore
    ; Verifica clique nos 8 cartões de manuscrito (Y entre 180 e 180 + 8*34)
    mov ax, [mouse_y]
    sub ax, 180
    jb .close_modal
    xor dx, dx
    mov cx, 34
    div cx                          ; AX = índice do slot clicado (0..7)
    cmp ax, 8
    jae .close_modal
    mov bl, al
    mov byte [modal_mode], 0
    call switch_to_slot
    ret
.close_modal:
    mov byte [modal_mode], 0
    ret

.check_zen_restore:
    cmp byte [zen_mode], 0
    je .check_top_toolbar
    ; No modo Ultra-Zen, clicar nos 36px superiores restaura a barra
    cmp word [mouse_y], 36
    ja .check_paper_click
    mov byte [zen_mode], 0
    mov word [toast_ptr], msg_zen_off
    ret

.check_top_toolbar:
    ; Barra de ferramentas gráfica superior (Y < 36)
    cmp word [mouse_y], 36
    jae .check_paper_click

    mov ax, [mouse_x]
    cmp ax, 150
    jb .btn_slots
    cmp ax, 240
    jb .btn_save
    cmp ax, 390
    jb .btn_theme
    cmp ax, 530
    jb .btn_font
    cmp ax, 670
    jb .btn_focus
    cmp ax, 735
    jb .btn_tw
    cmp ax, 800
    jb .btn_hem
    cmp ax, 910
    jb .btn_pomo
    jmp .btn_zen

.btn_slots:
    mov byte [modal_mode], 2
    ret
.btn_save:
    call disk_save_slot
    call disk_save_config
    ret
.btn_theme:
    inc byte [current_theme]
    and byte [current_theme], 0x03
    call load_theme_palette
    call disk_save_config
    mov word [toast_ptr], msg_theme_changed
    ret
.btn_font:
    inc byte [current_font]
    cmp byte [current_font], 3
    jb .font_ok
    mov byte [current_font], 0
.font_ok:
    call disk_save_config
    mov word [toast_ptr], msg_font_changed
    ret
.btn_focus:
    inc byte [focus_mode]
    cmp byte [focus_mode], 3
    jb .f_ok
    mov byte [focus_mode], 0
.f_ok:
    mov word [toast_ptr], msg_focus_changed
    ret
.btn_tw:
    xor byte [typewriter_mode], 1
    mov word [toast_ptr], msg_typewriter_changed
    ret
.btn_hem:
    xor byte [hemingway_mode], 1
    mov word [toast_ptr], msg_hemingway_changed
    ret
.btn_pomo:
    call toggle_pomodoro
    ret
.btn_zen:
    xor byte [zen_mode], 1
    mov word [toast_ptr], msg_zen_on
    ret

.check_paper_click:
    ; Clique na folha de papel posiciona o cursor de edição diretamente no texto!
    mov ax, [mouse_y]
    sub ax, [text_y]
    jb .click_done
    xor dx, dx
    mov cx, LINE_H
    div cx                          ; AX = linha clicada na janela (0..TEXT_WIN_ROWS-1)
    cmp ax, TEXT_WIN_ROWS
    jae .click_done

    add ax, [scroll_line]
    cmp ax, [total_lines]
    jb .line_in_range
    mov ax, [total_lines]
    dec ax
.line_in_range:
    mov bx, ax                      ; BX = linha visual alvo

    ; Calcula coluna clicada: (mouse_x - text_x) / CHAR_W
    mov ax, [mouse_x]
    sub ax, [text_x]
    jnc .col_pos
    xor ax, ax
.col_pos:
    xor dx, dx
    mov cx, CHAR_W
    div cx                          ; AX = coluna clicada
    cmp ax, WRAP_WIDTH - 1
    jbe .col_clamped
    mov ax, WRAP_WIDTH - 1
.col_clamped:
    mov [cursor_col], ax
    call move_cursor_to_line

.click_done:
    ret

; =============================================================================
; Processador de Teclado e Comandos do Escritor
; =============================================================================
process_key:
    test al, al
    jz .extended_key

    cmp al, 0x13                    ; Ctrl+S -> Salvar Manuscrito no Pen-Drive
    je .cmd_save
    cmp al, 0x0E                    ; Ctrl+N -> Nova folha limpa
    je .cmd_new_doc
    cmp al, 0x06                    ; Ctrl+F -> Alternar Fonte Gráfica
    je .cmd_font

    cmp al, 0x08
    je .key_backspace
    cmp al, 0x0D
    je .key_enter
    cmp al, 0x09
    je .key_tab
    cmp al, 0x1B
    je .key_esc

    cmp al, 0x20
    jb .done
    cmp al, 0x7F
    je .done

    call insert_char
    call click_sound
    ret

.extended_key:
    cmp ah, 0x3B                    ; F1 -> Guia Rápido
    je .cmd_help
    cmp ah, 0x3C                    ; F2 -> Alternar Tema TrueColor
    je .cmd_theme
    cmp ah, 0x3D                    ; F3 -> Alternar Modo Foco
    je .cmd_focus
    cmp ah, 0x3E                    ; F4 -> Modo Máquina de Escrever
    je .cmd_typewriter
    cmp ah, 0x3F                    ; F5 -> Modo Hemingway
    je .cmd_hemingway
    cmp ah, 0x40                    ; F6 -> Sprint Pomodoro
    je .cmd_pomodoro
    cmp ah, 0x41                    ; F7 -> Meta de Palavras
    je .cmd_goal
    cmp ah, 0x42                    ; F8 -> Seletor de Manuscritos
    je .cmd_slots
    cmp ah, 0x43                    ; F9 -> Som Mecânico
    je .cmd_sound
    cmp ah, 0x44                    ; F10 -> Modo Ultra-Zen
    je .cmd_zen
    cmp ah, 0x85                    ; F11 -> Alternar Família de Fonte Gráfica
    je .cmd_font

    cmp ah, 0x4B
    je .nav_left
    cmp ah, 0x4D
    je .nav_right
    cmp ah, 0x48
    je .nav_up
    cmp ah, 0x50
    je .nav_down
    cmp ah, 0x47
    je .nav_home
    cmp ah, 0x4F
    je .nav_end
    cmp ah, 0x49
    je .nav_pgup
    cmp ah, 0x51
    je .nav_pgdn
    cmp ah, 0x53
    je .key_delete

.done:
    ret

.key_enter:
    mov al, 0x0A
    call insert_char
    call click_sound
    ret

.key_tab:
    mov cx, 4
.tab_loop:
    push cx
    mov al, ' '
    call insert_char
    pop cx
    loop .tab_loop
    call click_sound
    ret

.key_esc:
    cmp byte [zen_mode], 0
    je .show_help_on_esc
    mov byte [zen_mode], 0
    mov word [toast_ptr], msg_zen_off
    ret
.show_help_on_esc:
    mov byte [modal_mode], 1
    ret

.key_backspace:
    cmp byte [hemingway_mode], 0
    jne .hemingway_blocked
    mov bx, [cursor_pos]
    test bx, bx
    jz .done
    dec bx
    mov [cursor_pos], bx
    call delete_at_cursor
    call click_sound
    ret

.key_delete:
    cmp byte [hemingway_mode], 0
    jne .hemingway_blocked
    mov bx, [cursor_pos]
    cmp bx, [doc_len]
    jae .done
    call delete_at_cursor
    call click_sound
    ret

.hemingway_blocked:
    mov word [toast_ptr], msg_hemingway_lock
    ret

.cmd_save:
    call disk_save_slot
    call disk_save_config
    ret

.cmd_new_doc:
    cmp byte [is_dirty], 0
    je .clear_now
    call disk_save_slot
.clear_now:
    push es
    mov ax, DOC_SEG
    mov es, ax
    xor di, di
    mov cx, MAX_DOC_LEN / 2
    xor ax, ax
    rep stosw
    pop es
    mov word [doc_len], 0
    mov word [cursor_pos], 0
    mov word [scroll_line], 0
    mov byte [is_dirty], 1
    mov word [toast_ptr], msg_new_doc
    ret

.cmd_help:
    mov byte [modal_mode], 1
    ret

.cmd_theme:
    inc byte [current_theme]
    and byte [current_theme], 0x03
    call load_theme_palette
    call disk_save_config
    mov word [toast_ptr], msg_theme_changed
    ret

.cmd_font:
    inc byte [current_font]
    cmp byte [current_font], 3
    jb .font_set_ok
    mov byte [current_font], 0
.font_set_ok:
    call disk_save_config
    mov word [toast_ptr], msg_font_changed
    ret

.cmd_focus:
    inc byte [focus_mode]
    cmp byte [focus_mode], 3
    jb .focus_ok
    mov byte [focus_mode], 0
.focus_ok:
    mov word [toast_ptr], msg_focus_changed
    ret

.cmd_typewriter:
    xor byte [typewriter_mode], 1
    mov word [toast_ptr], msg_typewriter_changed
    ret

.cmd_hemingway:
    xor byte [hemingway_mode], 1
    mov word [toast_ptr], msg_hemingway_changed
    ret

.cmd_pomodoro:
    call toggle_pomodoro
    ret

.cmd_goal:
    inc byte [goal_index]
    and byte [goal_index], 0x03
    movzx bx, byte [goal_index]
    shl bx, 1
    mov ax, [goal_presets + bx]
    mov [word_goal], ax
    call disk_save_config
    mov word [toast_ptr], msg_goal_changed
    ret

.cmd_slots:
    mov byte [modal_mode], 2
    ret

.cmd_sound:
    xor byte [sound_enabled], 1
    call click_sound
    mov word [toast_ptr], msg_sound_changed
    ret

.cmd_zen:
    xor byte [zen_mode], 1
    cmp byte [zen_mode], 0
    jne .zen_on_msg
    mov word [toast_ptr], msg_zen_off
    ret
.zen_on_msg:
    mov word [toast_ptr], msg_zen_on
    ret

.nav_left:
    cmp word [cursor_pos], 0
    je .done
    dec word [cursor_pos]
    ret

.nav_right:
    mov ax, [cursor_pos]
    cmp ax, [doc_len]
    jae .done
    inc word [cursor_pos]
    ret

.nav_home:
    mov bx, [cursor_line]
    call get_line_entry
    mov [cursor_pos], ax
    ret

.nav_end:
    mov bx, [cursor_line]
    call get_line_entry
    mov [cursor_pos], dx
    ret

.nav_up:
    mov bx, [cursor_line]
    test bx, bx
    jz .done
    dec bx
    call move_cursor_to_line
    ret

.nav_down:
    mov bx, [cursor_line]
    inc bx
    cmp bx, [total_lines]
    jae .done
    call move_cursor_to_line
    ret

.nav_pgup:
    mov bx, [cursor_line]
    sub bx, 18
    jnc .pgup_go
    xor bx, bx
.pgup_go:
    call move_cursor_to_line
    ret

.nav_pgdn:
    mov bx, [cursor_line]
    add bx, 18
    mov ax, [total_lines]
    dec ax
    cmp bx, ax
    jbe .pgdn_go
    mov bx, ax
.pgdn_go:
    call move_cursor_to_line
    ret

toggle_pomodoro:
    cmp byte [pomo_active], 0
    jne .stop_pomo
    mov byte [pomo_active], 1
    mov word [pomo_seconds], 25 * 60
    call read_bios_ticks
    mov [last_tick_lo], ax
    mov word [toast_ptr], msg_pomo_start
    ret
.stop_pomo:
    mov byte [pomo_active], 0
    mov word [toast_ptr], msg_pomo_stop
    ret

move_cursor_to_line:
    call get_line_entry
    mov cx, dx
    sub cx, ax
    mov si, [cursor_col]
    cmp si, cx
    jbe .col_ok
    mov si, cx
.col_ok:
    add ax, si
    mov [cursor_pos], ax
    ret

get_line_entry:
    push es
    push di
    mov di, WORK_SEG
    mov es, di
    mov di, bx
    shl di, 3
    add di, LINE_TABLE_OFF
    mov ax, [es:di]
    mov dx, [es:di + 2]
    pop di
    pop es
    ret

; =============================================================================
; Operações no Buffer de Documento (DOC_SEG = 0x1000)
; =============================================================================
insert_char:
    mov bx, [doc_len]
    cmp bx, MAX_DOC_LEN
    jae .full

    push es
    push ds
    mov cx, bx
    sub cx, [cursor_pos]
    mov dx, DOC_SEG
    mov es, dx
    mov ds, dx

    mov si, bx
    dec si
    mov di, bx
    std
    rep movsb
    cld

    pop ds
    mov bx, [cursor_pos]
    mov [es:bx], al
    pop es

    inc word [doc_len]
    inc word [cursor_pos]
    mov byte [is_dirty], 1
.full:
    ret

delete_at_cursor:
    mov bx, [cursor_pos]
    mov cx, [doc_len]
    cmp bx, cx
    jae .del_done

    push es
    push ds
    sub cx, bx
    dec cx
    mov dx, DOC_SEG
    mov es, dx
    mov ds, dx

    mov si, bx
    inc si
    mov di, bx
    cld
    rep movsb
    pop ds

    mov bx, [doc_len]
    dec bx
    mov byte [es:bx], 0
    pop es

    mov [doc_len], bx
    mov byte [is_dirty], 1
.del_done:
    ret

; =============================================================================
; Motor de Word-Wrap e Layout de Página (60 Colunas)
; =============================================================================
layout_document:
    push es
    mov ax, WORK_SEG
    mov es, ax

    xor si, si
    xor bp, bp
    xor dx, dx
    mov byte [cur_line_style], 0
    mov byte [at_para_start], 1

    cmp word [doc_len], 0
    jne .wrap_loop

    xor ax, ax
    xor cx, cx
    call store_visual_line
    mov word [total_lines], 1
    mov word [cursor_line], 0
    mov word [cursor_col], 0
    mov word [cursor_para], 0
    jmp .adjust_scroll

.wrap_loop:
    cmp si, [doc_len]
    jae .check_trailing_newline
    cmp bp, MAX_VISUAL_LINES - 2
    jae .layout_done_lines

    cmp byte [at_para_start], 0
    je .scan_line
    call detect_markdown_style
    mov [cur_line_style], al
    mov byte [at_para_start], 0

.scan_line:
    mov [line_start_tmp], si
    mov word [last_space_tmp], 0xFFFF
    xor cx, cx

.scan_char:
    cmp si, [doc_len]
    jae .end_of_doc_line

    push es
    mov ax, DOC_SEG
    mov es, ax
    mov al, [es:si]
    pop es

    cmp al, 0x0A
    je .found_newline

    cmp al, ' '
    jne .not_space
    mov [last_space_tmp], si
.not_space:
    inc si
    inc cx
    cmp cx, WRAP_WIDTH
    jb .scan_char

    mov bx, [last_space_tmp]
    cmp bx, 0xFFFF
    je .hard_wrap
    cmp bx, [line_start_tmp]
    jb .hard_wrap

    mov ax, [line_start_tmp]
    mov cx, bx
    inc bx
    mov si, bx
    call store_visual_line
    call check_cursor_in_wrapped_line
    jmp .wrap_loop

.hard_wrap:
    mov ax, [line_start_tmp]
    mov cx, si
    call store_visual_line
    call check_cursor_in_wrapped_line
    jmp .wrap_loop

.found_newline:
    mov ax, [line_start_tmp]
    mov cx, si
    inc si
    call store_visual_line
    call check_cursor_in_newline_line
    inc dx
    mov byte [at_para_start], 1
    jmp .wrap_loop

.end_of_doc_line:
    mov ax, [line_start_tmp]
    mov cx, si
    call store_visual_line
    call check_cursor_in_newline_line
    jmp .layout_done_lines

.check_trailing_newline:
    mov bx, [doc_len]
    test bx, bx
    jz .layout_done_lines
    push es
    mov ax, DOC_SEG
    mov es, ax
    mov al, [es:bx - 1]
    pop es
    cmp al, 0x0A
    jne .layout_done_lines

    mov ax, bx
    mov cx, bx
    mov byte [cur_line_style], 0
    call store_visual_line
    mov ax, [cursor_pos]
    cmp ax, bx
    jne .layout_done_lines
    mov ax, bp
    dec ax
    mov [cursor_line], ax
    mov word [cursor_col], 0
    mov [cursor_para], dx

.layout_done_lines:
    mov [total_lines], bp

.adjust_scroll:
    cmp byte [typewriter_mode], 0
    je .normal_scroll

    mov ax, [cursor_line]
    sub ax, 10
    jnc .tw_set
    xor ax, ax
.tw_set:
    mov [scroll_line], ax
    pop es
    ret

.normal_scroll:
    mov ax, [cursor_line]
    cmp ax, [scroll_line]
    jb .scroll_up
    mov bx, [scroll_line]
    add bx, TEXT_WIN_ROWS - 1
    cmp ax, bx
    ja .scroll_down
    pop es
    ret

.scroll_up:
    mov [scroll_line], ax
    pop es
    ret

.scroll_down:
    sub ax, TEXT_WIN_ROWS - 1
    mov [scroll_line], ax
    pop es
    ret

detect_markdown_style:
    push es
    mov ax, DOC_SEG
    mov es, ax
    mov al, [es:si]
    pop es
    cmp al, '#'
    je .is_head
    cmp al, '>'
    je .is_quote
    xor al, al
    ret
.is_head:
    mov al, 1
    ret
.is_quote:
    mov al, 2
    ret

store_visual_line:
    push di
    mov di, bp
    shl di, 3
    add di, LINE_TABLE_OFF
    mov [es:di], ax
    mov [es:di + 2], cx
    mov [es:di + 4], dx
    mov al, [cur_line_style]
    mov [es:di + 6], al
    mov byte [es:di + 7], 0
    inc bp
    pop di
    ret

check_cursor_in_wrapped_line:
    mov ax, [cursor_pos]
    cmp ax, [line_start_tmp]
    jb .not_here
    cmp ax, si
    jb .in_wrap_line
    cmp si, [doc_len]
    jne .not_here
    cmp ax, si
    jne .not_here
.in_wrap_line:
    mov bx, bp
    dec bx
    mov [cursor_line], bx
    sub ax, [line_start_tmp]
    cmp ax, WRAP_WIDTH - 1
    jbe .col_ok
    mov ax, WRAP_WIDTH - 1
.col_ok:
    mov [cursor_col], ax
    mov [cursor_para], dx
.not_here:
    ret

check_cursor_in_newline_line:
    mov ax, [cursor_pos]
    cmp ax, [line_start_tmp]
    jb .not_here2
    cmp ax, cx
    ja .not_here2
    mov bx, bp
    dec bx
    mov [cursor_line], bx
    sub ax, [line_start_tmp]
    cmp ax, WRAP_WIDTH - 1
    jbe .col_ok2
    mov ax, WRAP_WIDTH - 1
.col_ok2:
    mov [cursor_col], ax
    mov [cursor_para], dx
.not_here2:
    ret

; =============================================================================
; Motor de Telemetria do Escritor
; =============================================================================
compute_stats:
    push es
    mov ax, DOC_SEG
    mov es, ax

    xor si, si
    mov cx, [doc_len]
    xor bx, bx
    xor dx, dx
    mov word [para_count], 1

    test cx, cx
    jz .stats_done

.stat_loop:
    mov al, [es:si]
    inc si

    cmp al, 0x0A
    jne .check_ws
    inc word [para_count]
    xor dx, dx
    jmp .next_char

.check_ws:
    cmp al, ' '
    je .is_ws
    cmp al, 0x09
    je .is_ws
    cmp al, 0x0D
    je .is_ws

    test dx, dx
    jnz .next_char
    inc bx
    mov dx, 1
    jmp .next_char

.is_ws:
    xor dx, dx

.next_char:
    loop .stat_loop

.stats_done:
    mov [word_count], bx
    mov ax, bx
    test ax, ax
    jz .zero_read
    add ax, 199
    xor dx, dx
    mov cx, 200
    div cx
.zero_read:
    mov [read_mins], ax
    pop es
    ret

; =============================================================================
; Motor Gráfico 32-Bit TrueColor (Linear Framebuffer + Fontes AA + Mouse)
; =============================================================================
load_theme_palette:
    movzx bx, byte [current_theme]
    shl bx, 5                       ; 32 bytes (8 cores de 32 bits) por tema
    lea si, [theme_palettes_32 + bx]
    mov di, col_desk
    mov cx, 8
.copy_pal:
    lodsd
    mov [di], eax
    add di, 4
    loop .copy_pal
    ret

; Preenche retângulo no Backbuffer 32-bit:
; [rect_x], [rect_y], [rect_w], [rect_h], EAX = cor 0x00RRGGBB
fill_rect_32:
    push eax
    push ebx
    push ecx
    push edx
    push edi

    mov [rect_color], eax
    cmp word [rect_w], 0
    jle .rect_done
    cmp word [rect_h], 0
    jle .rect_done

    ; Calcula endereço base: BACKBUF_PHYS + (rect_y * screen_w + rect_x) * 4
    movzx eax, word [rect_y]
    movzx ebx, word [screen_w]
    imul eax, ebx
    movzx edx, word [rect_x]
    add eax, edx
    shl eax, 2
    mov edi, eax
    add edi, BACKBUF_PHYS

    ; Passo entre linhas em bytes = screen_w * 4
    shl ebx, 2
    mov dx, [rect_h]
    mov eax, [rect_color]

.row_loop:
    push edi
    movzx ecx, word [rect_w]
.col_loop:
    mov [fs:edi], eax
    add edi, 4
    dec ecx
    jnz .col_loop
    pop edi
    add edi, ebx
    dec dx
    jnz .row_loop

.rect_done:
    pop edi
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; Renderiza um caractere gráfico 8x16 com Subpixel Anti-Aliasing, Negrito e Itálico
; Entrada: AL = ASCII, [draw_x], [draw_y], [cur_fg_rgb], [cur_bg_rgb], [glyph_style]
draw_glyph_gfx:
    pusha
    cmp al, 32
    jb .glyph_done
    cmp al, 126
    ja .glyph_done

    ; Calcula cor intermediária 50% para Anti-Aliasing subpixel:
    ; cur_aa_rgb = ((fg & 0xFEFEFE) >> 1) + ((bg & 0xFEFEFE) >> 1)
    mov ecx, [cur_fg_rgb]
    and ecx, 0x00FEFEFE
    shr ecx, 1
    mov edx, [cur_bg_rgb]
    and edx, 0x00FEFEFE
    shr edx, 1
    add ecx, edx
    mov [cur_aa_rgb], ecx

    ; Seleciona a tabela da família de fonte gráfica ativa (0=Serif, 1=Sans, 2=Slab)
    sub al, 32
    movzx bx, al
    shl bx, 4                       ; * 16 bytes por glifo
    movzx si, byte [current_font]
    shl si, 1
    mov si, [font_tables + si]
    add si, bx                      ; DS:SI aponta para os 16 bytes do glifo

    xor bp, bp                      ; Linha do glifo (0..15)
.glyph_row:
    mov al, [ds:si + bp]            ; Padrão de 8 bits da linha do glifo (DS explícito!)
    test al, al
    jz .next_glyph_row

    ; Calcula coordenada Y na tela: draw_y + bp
    mov bx, [draw_y]
    add bx, bp
    movzx edi, bx
    movzx edx, word [screen_w]
    imul edi, edx

    ; Calcula coordenada X inicial (+ cisalhamento itálico se glyph_style == 2)
    mov bx, [draw_x]
    cmp byte [glyph_style], 2
    jne .no_italic_shear
    mov cx, 15
    sub cx, bp
    shr cx, 2                       ; Deslocamento itálico (+3 no topo .. 0 na base)
    add bx, cx
.no_italic_shear:
    movzx edx, bx
    add edi, edx
    shl edi, 2
    add edi, BACKBUF_PHYS

    ; Varre os 8 bits horizontais da linha do glifo
    mov ch, 0x80                    ; Máscara de bit atual
.glyph_bit:
    test al, ch
    jz .skip_pixel

    ; Pixel principal aceso com a cor da Tinta
    mov edx, [cur_fg_rgb]
    mov [fs:edi], edx

    ; Se for Título em Negrito (glyph_style == 1), engrossa a haste e suaviza
    cmp byte [glyph_style], 1
    jne .check_subpixel_aa
    mov [fs:edi + 4], edx
    mov edx, [cur_aa_rgb]
    mov [fs:edi + 8], edx
    jmp .skip_pixel

.check_subpixel_aa:
    ; Anti-Aliasing Subpixel: se o próximo bit à direita estiver apagado, aplica 50% AA
    mov cl, ch
    shr cl, 1
    test al, cl
    jnz .skip_pixel
    mov edx, [cur_aa_rgb]
    mov [fs:edi + 4], edx

.skip_pixel:
    add edi, 4
    shr ch, 1
    jnz .glyph_bit

.next_glyph_row:
    inc bp
    cmp bp, 16
    jb .glyph_row

.glyph_done:
    popa
    ret

; Desenha string ASCIIZ apontada por DS:SI a partir de [draw_x], [draw_y]
draw_str_gfx:
    lodsb
    test al, al
    jz .done
    call draw_glyph_gfx
    add word [draw_x], CHAR_W
    jmp draw_str_gfx
.done:
    ret

; Desenha inteiro sem sinal AX em [draw_x], [draw_y]
draw_uint_gfx:
    push bx
    push cx
    push dx
    mov bx, 10
    xor cx, cx
.div_loop:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .div_loop
.print_loop:
    pop dx
    mov al, dl
    add al, '0'
    call draw_glyph_gfx
    add word [draw_x], CHAR_W
    loop .print_loop
    pop dx
    pop cx
    pop bx
    ret

; Desenha número de 2 dígitos (00..99) em AL
draw_uint_2dig_gfx:
    push bx
    mov bl, 10
    xor ah, ah
    div bl
    mov bh, ah
    add al, '0'
    call draw_glyph_gfx
    add word [draw_x], CHAR_W
    mov al, bh
    add al, '0'
    call draw_glyph_gfx
    add word [draw_x], CHAR_W
    pop bx
    ret

; =============================================================================
; Renderizador Principal da Página Gráfica "Folha sobre a Mesa"
; =============================================================================
render_screen:
    push es
    mov ax, WORK_SEG
    mov es, ax

    ; 1. Preenche o fundo da Mesa (Desk Background)
    mov word [rect_x], 0
    mov word [rect_y], 0
    mov ax, [screen_w]
    mov [rect_w], ax
    mov ax, [screen_h]
    mov [rect_h], ax
    mov eax, [col_desk]
    call fill_rect_32

    ; 2. Desenha a Sombra Projetada da Folha de Papel (Drop Shadow +8px, +8px)
    mov ax, [paper_x]
    add ax, 8
    mov [rect_x], ax
    mov ax, [paper_y]
    add ax, 8
    mov [rect_y], ax
    mov word [rect_w], 760
    mov ax, [paper_h]
    mov [rect_h], ax
    mov eax, [col_shadow]
    call fill_rect_32

    ; 3. Desenha a Folha de Papel Elevada (Paper Canvas)
    mov ax, [paper_x]
    mov [rect_x], ax
    mov ax, [paper_y]
    mov [rect_y], ax
    mov word [rect_w], 760
    mov ax, [paper_h]
    mov [rect_h], ax
    mov eax, [col_paper]
    call fill_rect_32

    ; 4. Renderiza as linhas visíveis do manuscrito com tipografia gráfica
    mov bx, [scroll_line]
    mov word [screen_row], 0
    mov ax, [text_y]
    mov [cur_row_y], ax

.row_loop:
    cmp word [screen_row], TEXT_WIN_ROWS
    jae .rows_done
    cmp bx, [total_lines]
    jae .rows_done

    mov di, bx
    shl di, 3
    add di, LINE_TABLE_OFF
    mov si, [es:di]                 ; line_start
    mov cx, [es:di + 2]             ; line_end
    mov dx, [es:di + 4]             ; para_id
    mov al, [es:di + 6]             ; style (0=Normal, 1=Heading, 2=Quote)
    mov [glyph_style], al

    ; Seleciona cor da tinta conforme estilo Markdown e Modo Foco (F3)
    mov eax, [col_ink]
    cmp byte [glyph_style], 1
    jne .check_quote_col
    mov eax, [col_accent]
    jmp .check_focus_dim
.check_quote_col:
    cmp byte [glyph_style], 2
    jne .check_focus_dim
    mov eax, [col_quote]

.check_focus_dim:
    cmp byte [focus_mode], 1
    je .focus_line_check
    cmp byte [focus_mode], 2
    je .focus_para_check
    jmp .set_line_colors

.focus_line_check:
    cmp bx, [cursor_line]
    je .set_line_colors
    mov eax, [col_dim]
    jmp .set_line_colors

.focus_para_check:
    cmp dx, [cursor_para]
    je .set_line_colors
    mov eax, [col_dim]

.set_line_colors:
    mov [cur_fg_rgb], eax
    mov eax, [col_paper]
    mov [cur_bg_rgb], eax

    ; Se for a linha ativa do cursor, desenha marcador lateral na margem da folha
    cmp bx, [cursor_line]
    jne .check_quote_bar
    mov ax, [text_x]
    sub ax, 22
    mov [rect_x], ax
    mov ax, [cur_row_y]
    add ax, 4
    mov [rect_y], ax
    mov word [rect_w], 6
    mov word [rect_h], 10
    mov eax, [col_accent]
    call fill_rect_32

.check_quote_bar:
    ; Se for citação (> ), desenha barra vertical estilizada à esquerda do parágrafo
    cmp byte [glyph_style], 2
    jne .draw_line_glyphs
    mov ax, [text_x]
    sub ax, 10
    mov [rect_x], ax
    mov ax, [cur_row_y]
    mov [rect_y], ax
    mov word [rect_w], 3
    mov word [rect_h], 20
    mov eax, [col_quote]
    call fill_rect_32

.draw_line_glyphs:
    mov ax, [text_x]
    mov [draw_x], ax
    mov ax, [cur_row_y]
    mov [draw_y], ax

    sub cx, si
    jbe .after_line_chars
    cmp cx, WRAP_WIDTH
    jbe .len_ok
    mov cx, WRAP_WIDTH
.len_ok:
    push bx
    mov bx, si
.char_loop:
    push es
    mov ax, DOC_SEG
    mov es, ax
    mov al, [es:bx]
    pop es
    cmp al, 0x0D
    je .skip_cr
    call draw_glyph_gfx
.skip_cr:
    add word [draw_x], CHAR_W
    inc bx
    loop .char_loop
    pop bx

.after_line_chars:
    ; Se for linha de Título (# ), desenha linha horizontal decorativa sutil abaixo
    cmp byte [glyph_style], 1
    jne .next_row
    mov ax, [text_x]
    mov [rect_x], ax
    mov ax, [cur_row_y]
    add ax, 20
    mov [rect_y], ax
    mov word [rect_w], 600
    mov word [rect_h], 1
    mov eax, [col_dim]
    call fill_rect_32

.next_row:
    inc bx
    inc word [screen_row]
    add word [cur_row_y], LINE_H
    jmp .row_loop

.rows_done:
    ; 5. Desenha o Cursor Tipográfico de Edição (Barra I-Beam de 2px em Destaque)
    mov ax, [cursor_line]
    sub ax, [scroll_line]
    cmp ax, TEXT_WIN_ROWS
    jae .draw_progress_bar
    imul ax, LINE_H
    add ax, [text_y]
    mov [rect_y], ax
    mov ax, [cursor_col]
    imul ax, CHAR_W
    add ax, [text_x]
    mov [rect_x], ax
    mov word [rect_w], 2
    mov word [rect_h], 18
    mov eax, [col_accent]
    call fill_rect_32

.draw_progress_bar:
    ; 6. Barra de Progresso Visual da Meta de Palavras na Base da Folha
    mov ax, [paper_x]
    mov [rect_x], ax
    mov ax, [paper_y]
    add ax, [paper_h]
    sub ax, 4
    mov [rect_y], ax
    mov ax, [word_count]
    cmp ax, [word_goal]
    jbe .calc_prog_w
    mov ax, [word_goal]
.calc_prog_w:
    movzx eax, ax
    imul eax, 760
    movzx ecx, word [word_goal]
    xor edx, edx
    div ecx
    mov [rect_w], ax
    mov word [rect_h], 4
    mov eax, [col_accent]
    call fill_rect_32

    ; 7. Barras de Ferramentas Superior e Status Inferior (se Ultra-Zen desativado)
    cmp byte [zen_mode], 0
    jne .check_modals

    call draw_top_toolbar_gfx
    call draw_bottom_statusbar_gfx

.check_modals:
    cmp byte [modal_mode], 1
    jne .check_slot_modal
    call draw_help_modal_gfx
    jmp .draw_mouse
.check_slot_modal:
    cmp byte [modal_mode], 2
    jne .draw_mouse
    call draw_slot_modal_gfx

.draw_mouse:
    ; 8. Desenha o Cursor Vetorial do Mouse sobre o Framebuffer
    call draw_mouse_cursor_gfx

    ; 9. Transfere o Backbuffer 32-bit (BACKBUF_PHYS) para o Linear Framebuffer VBE
    mov esi, BACKBUF_PHYS
    mov edi, [vbe_lfb_phys]
    movzx eax, word [screen_w]
    movzx ecx, word [screen_h]
    imul ecx, eax
.blit_lfb:
    mov eax, [fs:esi]
    mov [fs:edi], eax
    add esi, 4
    add edi, 4
    dec ecx
    jnz .blit_lfb

    pop es
    ret

; -----------------------------------------------------------------------------
; Barra de Ferramentas Gráfica Clicável (Topo: Y = 0..34)
; -----------------------------------------------------------------------------
draw_top_toolbar_gfx:
    mov word [rect_x], 0
    mov word [rect_y], 0
    mov ax, [screen_w]
    mov [rect_w], ax
    mov word [rect_h], 34
    mov eax, [col_bar]
    call fill_rect_32

    mov byte [glyph_style], 0
    mov eax, [col_bar]
    mov [cur_bg_rgb], eax
    mov word [draw_y], 9

    ; Botão 1: [MANUSCR1.MD] (X=16)
    mov word [draw_x], 16
    mov eax, [col_accent]
    mov [cur_fg_rgb], eax
    mov al, [current_slot]
    add al, '1'
    mov [str_slot_file + 7], al
    mov si, str_slot_file
    call draw_str_gfx

    ; Botão 2: [Salvar] (X=165)
    mov word [draw_x], 165
    mov eax, [col_ink]
    cmp byte [is_dirty], 0
    je .save_btn_col
    mov eax, [col_accent]
.save_btn_col:
    mov [cur_fg_rgb], eax
    mov si, str_btn_save
    call draw_str_gfx

    ; Botão 3: [Tema] (X=255)
    mov word [draw_x], 255
    mov eax, [col_ink]
    mov [cur_fg_rgb], eax
    movzx bx, byte [current_theme]
    shl bx, 1
    mov si, [theme_btn_labels + bx]
    call draw_str_gfx

    ; Botão 4: [Fonte] (X=395)
    mov word [draw_x], 395
    mov eax, [col_accent]
    mov [cur_fg_rgb], eax
    movzx bx, byte [current_font]
    shl bx, 1
    mov si, [font_btn_labels + bx]
    call draw_str_gfx

    ; Botão 5: [Foco] (X=535)
    mov word [draw_x], 535
    mov eax, [col_ink]
    mov [cur_fg_rgb], eax
    movzx bx, byte [focus_mode]
    shl bx, 1
    mov si, [focus_labels + bx]
    call draw_str_gfx

    ; Botão 6: [Maq] (X=675)
    mov word [draw_x], 675
    mov eax, [col_dim]
    cmp byte [typewriter_mode], 0
    je .draw_tw_btn
    mov eax, [col_accent]
.draw_tw_btn:
    mov [cur_fg_rgb], eax
    mov si, str_badge_tw
    call draw_str_gfx

    ; Botão 7: [Hem] (X=740)
    mov word [draw_x], 740
    mov eax, [col_dim]
    cmp byte [hemingway_mode], 0
    je .draw_hem_btn
    mov eax, [col_accent]
.draw_hem_btn:
    mov [cur_fg_rgb], eax
    mov si, str_badge_hem
    call draw_str_gfx

    ; Botão 8: [Pomodoro] (X=805)
    mov word [draw_x], 805
    cmp byte [pomo_active], 0
    jne .draw_pomo_active
    mov eax, [col_dim]
    mov [cur_fg_rgb], eax
    mov si, str_btn_pomo
    call draw_str_gfx
    jmp .draw_zen_btn

.draw_pomo_active:
    mov eax, [col_accent]
    mov [cur_fg_rgb], eax
    mov ax, [pomo_seconds]
    xor dx, dx
    mov cx, 60
    div cx
    push dx
    call draw_uint_2dig_gfx
    mov al, ':'
    call draw_glyph_gfx
    add word [draw_x], CHAR_W
    pop ax
    call draw_uint_2dig_gfx

.draw_zen_btn:
    ; Botão 9: [Zen] (X=915)
    mov word [draw_x], 915
    mov eax, [col_dim]
    mov [cur_fg_rgb], eax
    mov si, str_btn_zen
    call draw_str_gfx
    ret

; -----------------------------------------------------------------------------
; Barra de Status / Telemetria Inferior
; -----------------------------------------------------------------------------
draw_bottom_statusbar_gfx:
    mov word [rect_x], 0
    mov ax, [screen_h]
    sub ax, 32
    mov [rect_y], ax
    mov [draw_y], ax
    add word [draw_y], 8
    mov ax, [screen_w]
    mov [rect_w], ax
    mov word [rect_h], 32
    mov eax, [col_bar]
    call fill_rect_32

    mov byte [glyph_style], 0
    mov eax, [col_bar]
    mov [cur_bg_rgb], eax
    mov word [draw_x], 20

    cmp word [toast_ptr], 0
    je .normal_telemetry
    mov eax, [col_accent]
    mov [cur_fg_rgb], eax
    mov si, [toast_ptr]
    call draw_str_gfx
    ret

.normal_telemetry:
    mov eax, [col_accent]
    mov [cur_fg_rgb], eax
    mov ax, [word_count]
    call draw_uint_gfx

    mov eax, [col_ink]
    mov [cur_fg_rgb], eax
    mov si, str_words_lbl
    call draw_str_gfx

    mov eax, [col_dim]
    mov [cur_fg_rgb], eax
    mov ax, [word_goal]
    call draw_uint_gfx
    mov si, str_sep_pipe
    call draw_str_gfx

    mov eax, [col_ink]
    mov [cur_fg_rgb], eax
    mov ax, [doc_len]
    call draw_uint_gfx
    mov eax, [col_dim]
    mov [cur_fg_rgb], eax
    mov si, str_chars_lbl
    call draw_str_gfx

    mov eax, [col_ink]
    mov [cur_fg_rgb], eax
    mov ax, [read_mins]
    call draw_uint_gfx
    mov eax, [col_dim]
    mov [cur_fg_rgb], eax
    mov si, str_read_lbl
    call draw_str_gfx

    ; Exibe status do Mouse USB HID + Resolução ativa do monitor no canto inferior direito
    mov ax, [screen_w]
    sub ax, 260
    mov [draw_x], ax
    mov eax, [col_dim]
    mov [cur_fg_rgb], eax
    mov si, str_res_lbl
    cmp byte [usb_mouse_active], 0
    je .draw_hw_badge
    mov si, str_usb_res_lbl
.draw_hw_badge:
    call draw_str_gfx
    mov ax, [screen_w]
    call draw_uint_gfx
    mov al, 'x'
    call draw_glyph_gfx
    add word [draw_x], CHAR_W
    mov ax, [screen_h]
    call draw_uint_gfx
    ret

; -----------------------------------------------------------------------------
; Modais Gráficos (F1 Guia e F8 Seletor de Manuscritos Clicável)
; -----------------------------------------------------------------------------
draw_help_modal_gfx:
    mov ax, [paper_x]
    add ax, 60
    mov [rect_x], ax
    mov word [rect_y], 110
    mov word [rect_w], 640
    mov word [rect_h], 420
    mov eax, [col_bar]
    call fill_rect_32

    mov byte [glyph_style], 1
    mov eax, [col_bar]
    mov [cur_bg_rgb], eax
    mov eax, [col_accent]
    mov [cur_fg_rgb], eax
    mov ax, [paper_x]
    add ax, 90
    mov [draw_x], ax
    mov word [draw_y], 135
    mov si, help_title
    call draw_str_gfx

    mov byte [glyph_style], 0
    mov eax, [col_ink]
    mov [cur_fg_rgb], eax

    mov ax, [paper_x]
    add ax, 90
    mov [draw_x], ax
    mov word [draw_y], 175
    mov si, help_line1
    call draw_str_gfx

    mov ax, [paper_x]
    add ax, 90
    mov [draw_x], ax
    mov word [draw_y], 205
    mov si, help_line2
    call draw_str_gfx

    mov ax, [paper_x]
    add ax, 90
    mov [draw_x], ax
    mov word [draw_y], 235
    mov si, help_line3
    call draw_str_gfx

    mov ax, [paper_x]
    add ax, 90
    mov [draw_x], ax
    mov word [draw_y], 265
    mov si, help_line4
    call draw_str_gfx

    mov ax, [paper_x]
    add ax, 90
    mov [draw_x], ax
    mov word [draw_y], 295
    mov si, help_line5
    call draw_str_gfx

    mov ax, [paper_x]
    add ax, 90
    mov [draw_x], ax
    mov word [draw_y], 325
    mov si, help_line6
    call draw_str_gfx

    mov ax, [paper_x]
    add ax, 90
    mov [draw_x], ax
    mov word [draw_y], 355
    mov si, help_line7
    call draw_str_gfx

    mov ax, [paper_x]
    add ax, 90
    mov [draw_x], ax
    mov word [draw_y], 385
    mov si, help_line8
    call draw_str_gfx

    mov ax, [paper_x]
    add ax, 90
    mov [draw_x], ax
    mov word [draw_y], 415
    mov si, help_line9
    call draw_str_gfx

    mov eax, [col_accent]
    mov [cur_fg_rgb], eax
    mov ax, [paper_x]
    add ax, 90
    mov [draw_x], ax
    mov word [draw_y], 460
    mov si, help_footer
    call draw_str_gfx
    ret

draw_slot_modal_gfx:
    mov ax, [paper_x]
    add ax, 80
    mov [rect_x], ax
    mov word [rect_y], 120
    mov word [rect_w], 600
    mov word [rect_h], 390
    mov eax, [col_bar]
    call fill_rect_32

    mov byte [glyph_style], 1
    mov eax, [col_bar]
    mov [cur_bg_rgb], eax
    mov eax, [col_accent]
    mov [cur_fg_rgb], eax
    mov ax, [paper_x]
    add ax, 110
    mov [draw_x], ax
    mov word [draw_y], 140
    mov si, slot_modal_title
    call draw_str_gfx

    mov byte [glyph_style], 0
    xor cx, cx
.slot_card_loop:
    push cx
    mov ax, cx
    imul ax, 34
    add ax, 180
    mov [draw_y], ax

    mov ax, [paper_x]
    add ax, 120
    mov [draw_x], ax

    mov eax, [col_accent]
    mov [cur_fg_rgb], eax
    mov al, '['
    call draw_glyph_gfx
    add word [draw_x], CHAR_W
    pop cx
    push cx
    mov al, cl
    add al, '1'
    call draw_glyph_gfx
    add word [draw_x], CHAR_W
    mov al, ']'
    call draw_glyph_gfx
    add word [draw_x], CHAR_W * 2

    mov eax, [col_ink]
    mov [cur_fg_rgb], eax
    pop cx
    push cx
    mov al, cl
    add al, '1'
    mov [str_slot_file + 7], al
    mov si, str_slot_file
    call draw_str_gfx

    pop cx
    push cx
    cmp cl, [current_slot]
    jne .next_slot_card
    mov eax, [col_accent]
    mov [cur_fg_rgb], eax
    mov si, str_active_tag
    call draw_str_gfx
.next_slot_card:
    pop cx
    inc cx
    cmp cx, 8
    jb .slot_card_loop

    mov al, [current_slot]
    add al, '1'
    mov [str_slot_file + 7], al

    mov eax, [col_dim]
    mov [cur_fg_rgb], eax
    mov ax, [paper_x]
    add ax, 110
    mov [draw_x], ax
    mov word [draw_y], 468
    mov si, slot_modal_footer
    call draw_str_gfx
    ret

; -----------------------------------------------------------------------------
; Cursor Gráfico Vetorial do Mouse (Seta 12x16 com Contorno de Alto Contraste)
; -----------------------------------------------------------------------------
draw_mouse_cursor_gfx:
    xor bp, bp
.cursor_row_loop:
    mov bx, [mouse_y]
    add bx, bp
    cmp bx, [screen_h]
    jae .cursor_done

    movzx edi, bx
    movzx edx, word [screen_w]
    imul edi, edx
    movzx edx, word [mouse_x]
    add edi, edx
    shl edi, 2
    add edi, BACKBUF_PHYS

    mov si, bp
    shl si, 1
    mov ax, [cursor_mask_outline + si]
    mov dx, [cursor_mask_fill + si]
    mov cx, 12
    mov bx, 0x8000

.cursor_col_loop:
    test dx, bx
    jz .check_outline
    mov dword [fs:edi], 0x00C84B31  ; Núcleo do ponteiro em Destaque Terracota/Ouro
    jmp .next_cursor_px
.check_outline:
    test ax, bx
    jz .next_cursor_px
    mov dword [fs:edi], 0x00FFFFFF  ; Contorno branco nítido
.next_cursor_px:
    add edi, 4
    shr bx, 1
    loop .cursor_col_loop

    inc bp
    cmp bp, 16
    jb .cursor_row_loop

.cursor_done:
    ret

cursor_mask_outline:
    dw 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00, 0xFF80
    dw 0xFFC0, 0xFFE0, 0xFE00, 0xEF00, 0xCF00, 0x8780, 0x0780, 0x0380

cursor_mask_fill:
    dw 0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00
    dw 0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000

; =============================================================================
; Temporizador Hardware (PIT/BIOS 0040:006Ch) & Som Mecânico (Porta 0x61)
; =============================================================================
read_bios_ticks:
    push es
    xor ax, ax
    mov es, ax
    mov ax, [es:0x046C]
    pop es
    ret

check_timers:
    call read_bios_ticks
    mov bx, ax
    sub ax, [last_tick_lo]
    cmp ax, 18
    jb .check_autosave

    mov [last_tick_lo], bx
    cmp byte [pomo_active], 0
    je .check_autosave

    cmp word [pomo_seconds], 0
    je .pomo_finished
    dec word [pomo_seconds]
    call render_screen
    jmp .check_autosave

.pomo_finished:
    mov byte [pomo_active], 0
    mov word [toast_ptr], msg_pomo_done
    call click_sound
    call render_screen

.check_autosave:
    cmp byte [is_dirty], 0
    je .timers_done
    mov ax, bx
    sub ax, [autosave_tick_lo]
    cmp ax, 1092
    jb .timers_done
    mov [autosave_tick_lo], bx
    call disk_save_slot
    mov word [toast_ptr], msg_autosaved
    call render_screen
.timers_done:
    ret

click_sound:
    cmp byte [sound_enabled], 0
    je .no_sound
    push ax
    push cx
    mov al, 0xB6
    out 0x43, al
    mov ax, 1500
    out 0x42, al
    mov al, ah
    out 0x42, al

    in al, 0x61
    or al, 0x03
    out 0x61, al

    mov cx, 1800
.delay:
    nop
    loop .delay

    in al, 0x61
    and al, 0xFC
    out 0x61, al
    pop cx
    pop ax
.no_sound:
    ret

; =============================================================================
; Subsistema de Disco Bare-Metal (INT 13h LBA Estendido + Fallback CHS)
; =============================================================================
disk_init:
    mov byte [use_lba], 0
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .try_chs_geom
    cmp bx, 0xAA55
    jne .try_chs_geom
    mov byte [use_lba], 1
    ret

.try_chs_geom:
    mov dl, [boot_drive]
    cmp dl, 0x80
    jb .geom_done
    mov ah, 0x08
    int 0x13
    jc .geom_done
    and cl, 0x3F
    movzx ax, cl
    mov [chs_spt], ax
    movzx ax, dh
    inc ax
    mov [chs_heads], ax
    push ds
    pop es
.geom_done:
    ret

switch_to_slot:
    cmp bl, [current_slot]
    je .same_slot
    cmp byte [is_dirty], 0
    je .do_switch
    push bx
    call disk_save_slot
    pop bx
.do_switch:
    mov [current_slot], bl
    call disk_load_slot
    call disk_save_config
    mov word [toast_ptr], msg_slot_loaded
.same_slot:
    ret

get_slot_start_lba:
    movzx ax, byte [current_slot]
    shl ax, 6
    add ax, DATA_START_LBA
    ret

disk_load_slot:
    call get_slot_start_lba
    mov [io_lba], ax
    mov word [io_seg], DOC_SEG
    mov word [io_off], 0x0000
    mov cx, SECTORS_PER_SLOT

.read_loop:
    push cx
    call read_one_sector
    inc word [io_lba]
    add word [io_off], 512
    pop cx
    loop .read_loop

    call get_fat16_slot_filesize
    cmp ax, MAX_DOC_LEN
    jbe .check_scan
    xor ax, ax
.check_scan:
    push es
    mov bx, DOC_SEG
    mov es, bx
    xor si, si
.scan_len:
    cmp si, MAX_DOC_LEN
    jae .scan_done
    cmp byte [es:si], 0
    je .scan_done
    inc si
    jmp .scan_len
.scan_done:
    pop es
    mov [doc_len], si
    mov word [cursor_pos], 0
    mov word [scroll_line], 0
    mov byte [is_dirty], 0
    ret

disk_save_slot:
    push es
    mov ax, DOC_SEG
    mov es, ax
    mov di, [doc_len]
    mov cx, 32768
    sub cx, di
    xor al, al
    rep stosb
    pop es

    call get_slot_start_lba
    mov [io_lba], ax
    mov word [io_seg], DOC_SEG
    mov word [io_off], 0x0000
    mov cx, SECTORS_PER_SLOT

.write_loop:
    push cx
    call write_one_sector
    jc .save_error_loop
    inc word [io_lba]
    add word [io_off], 512
    pop cx
    loop .write_loop

    call update_fat16_dir_entry
    jc .save_error

    mov byte [is_dirty], 0
    mov word [toast_ptr], msg_saved_ok
    call read_bios_ticks
    mov [autosave_tick_lo], ax
    ret

.save_error_loop:
    pop cx
.save_error:
    mov word [toast_ptr], msg_save_err
    ret

get_fat16_slot_filesize:
    mov word [io_lba], ROOT_DIR_LBA
    mov word [io_seg], KERNEL_SEG
    mov word [io_off], sector_buf
    call read_one_sector
    jc .fallback_zero
    movzx bx, byte [current_slot]
    shl bx, 5
    mov ax, [sector_buf + bx + 28]
    ret
.fallback_zero:
    xor ax, ax
    ret

update_fat16_dir_entry:
    mov word [io_lba], ROOT_DIR_LBA
    mov word [io_seg], KERNEL_SEG
    mov word [io_off], sector_buf
    call read_one_sector
    jc .dir_err

    movzx bx, byte [current_slot]
    shl bx, 5
    mov ax, [doc_len]
    mov [sector_buf + bx + 28], ax
    mov word [sector_buf + bx + 30], 0

    mov word [io_lba], ROOT_DIR_LBA
    mov word [io_seg], KERNEL_SEG
    mov word [io_off], sector_buf
    call write_one_sector
.dir_err:
    ret

disk_load_config:
    mov word [io_lba], CONFIG_LBA
    mov word [io_seg], KERNEL_SEG
    mov word [io_off], sector_buf
    call read_one_sector
    jc .cfg_done
    cmp word [sector_buf], 0x4638
    jne .cfg_done
    mov al, [sector_buf + 2]
    and al, 0x07
    mov [current_slot], al
    mov al, [sector_buf + 3]
    and al, 0x03
    mov [current_theme], al
    mov al, [sector_buf + 4]
    and al, 0x03
    mov [goal_index], al
    movzx bx, al
    shl bx, 1
    mov ax, [goal_presets + bx]
    mov [word_goal], ax
    mov al, [sector_buf + 5]
    and al, 0x01
    mov [sound_enabled], al
    mov al, [sector_buf + 6]
    cmp al, 3
    jb .font_cfg_ok
    xor al, al
.font_cfg_ok:
    mov [current_font], al
.cfg_done:
    ret

disk_save_config:
    push es
    push ds
    pop es
    mov di, sector_buf
    mov cx, 256
    xor ax, ax
    rep stosw

    mov word [sector_buf], 0x4638
    mov al, [current_slot]
    mov [sector_buf + 2], al
    mov al, [current_theme]
    mov [sector_buf + 3], al
    mov al, [goal_index]
    mov [sector_buf + 4], al
    mov al, [sound_enabled]
    mov [sector_buf + 5], al
    mov al, [current_font]
    mov [sector_buf + 6], al

    mov word [io_lba], CONFIG_LBA
    mov word [io_seg], KERNEL_SEG
    mov word [io_off], sector_buf
    call write_one_sector
    pop es
    ret

read_one_sector:
    cmp byte [use_lba], 0
    je .chs_read
    mov word [dap_lba_lo], 0
    mov ax, [io_lba]
    mov [dap_lba_lo], ax
    mov ax, [io_off]
    mov [dap_off], ax
    mov ax, [io_seg]
    mov [dap_seg], ax
    mov si, dap_io
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jnc .ok
.chs_read:
    call lba_to_chs
    push es
    mov es, [io_seg]
    mov bx, [io_off]
    mov ax, 0x0201
    mov dl, [boot_drive]
    int 0x13
    pop es
.ok:
    ret

write_one_sector:
    cmp byte [use_lba], 0
    je .chs_write
    mov ax, [io_lba]
    mov [dap_lba_lo], ax
    mov ax, [io_off]
    mov [dap_off], ax
    mov ax, [io_seg]
    mov [dap_seg], ax
    mov si, dap_io
    mov ah, 0x43
    xor al, al
    mov dl, [boot_drive]
    int 0x13
    jnc .write_flush
.chs_write:
    call lba_to_chs
    push es
    mov es, [io_seg]
    mov bx, [io_off]
    mov ax, 0x0301
    mov dl, [boot_drive]
    int 0x13
    pop es
    jc .w_ret
.write_flush:
    clc
.w_ret:
    ret

lba_to_chs:
    mov ax, [io_lba]
    xor dx, dx
    div word [chs_spt]
    mov cl, dl
    inc cl
    xor dx, dx
    div word [chs_heads]
    mov dh, dl
    mov ch, al
    shl ah, 6
    or cl, ah
    ret

; =============================================================================
; Variáveis de Estado, Paletas TrueColor 32-bit e Fontes Gráficas
; =============================================================================
boot_drive:             db 0x80
use_lba:                db 1
chs_spt:                dw 63
chs_heads:              dw 16

align 4
dap_io:
    db 0x10
    db 0x00
    dw 1
dap_off:                dw 0x0000
dap_seg:                dw KERNEL_SEG
dap_lba_lo:             dw 0x0000
                        dw 0x0000
                        dd 0x00000000

io_lba:                 dw 0
io_seg:                 dw 0
io_off:                 dw 0

; Estado Gráfico VESA VBE & Geometria Responsiva
target_w:               dw 1024
target_h:               dw 768
best_mode:              dw 0x0118
screen_w:               dw 1024
screen_h:               dw 768
vbe_lfb_phys:           dd DEFAULT_LFB_PHYS

paper_x:                dw 132
paper_y:                dw 44
paper_h:                dw 680
text_x:                 dw 212
text_y:                 dw 70
cur_row_y:              dw 70

rect_x:                 dw 0
rect_y:                 dw 0
rect_w:                 dw 0
rect_h:                 dw 0
rect_color:             dd 0

draw_x:                 dw 0
draw_y:                 dw 0
cur_fg_rgb:             dd 0
cur_bg_rgb:             dd 0
cur_aa_rgb:             dd 0
glyph_style:            db 0

; Paleta ativa carregada (8 dwords)
col_desk:               dd 0x00DED6C6
col_shadow:             dd 0x00C4BAAA
col_paper:              dd 0x00FAF4E8
col_bar:                dd 0x00EFE7D8
col_ink:                dd 0x00222428
col_dim:                dd 0x00B4ACA0
col_accent:             dd 0x00C84B31
col_quote:              dd 0x004A6B7C

; Estado do Mouse Híbrido (USB HID + BIOS INT 15h + PS/2)
mouse_x:                dw 512
mouse_y:                dw 384
mouse_pkt:              db 0, 0, 0
mouse_pkt_idx:          db 0
mouse_prev_btn:         db 0
mouse_dirty:            db 0
usb_mouse_active:       db 0
usb_io_base:            dw 0xC000
bios_pkt_ready:         db 0
bios_pkt_status:        db 0
bios_pkt_dx:            db 0
bios_pkt_dy:            db 0

; Estado do Editor
current_slot:           db 0
current_theme:          db 0
current_font:           db 0        ; 0=Folio Serif, 1=Clean Sans, 2=Courier Slab
focus_mode:             db 0
typewriter_mode:        db 0
hemingway_mode:         db 0
zen_mode:               db 0
sound_enabled:          db 0
modal_mode:             db 0
is_dirty:               db 0
dead_key:               db 0

doc_len:                dw 0
cursor_pos:             dw 0
cursor_line:            dw 0
cursor_col:             dw 0
cursor_para:            dw 0
scroll_line:            dw 0
total_lines:            dw 1
screen_row:             dw 0

line_start_tmp:         dw 0
last_space_tmp:         dw 0
cur_line_style:         db 0
at_para_start:          db 1

word_count:             dw 0
para_count:             dw 1
read_mins:              dw 0
goal_index:             db 1
word_goal:              dw 500
goal_presets:           dw 250, 500, 1000, 2000

pomo_active:            db 0
pomo_seconds:           dw 1500
last_tick_lo:           dw 0
autosave_tick_lo:       dw 0
toast_ptr:              dw 0

; -----------------------------------------------------------------------------
; 4 Paletas TrueColor 32-Bit (Desk, Shadow, Paper, Bar, Ink, Dim, Accent, Quote)
; -----------------------------------------------------------------------------
align 4
theme_palettes_32:
    ; Tema 0: Papiro Quente (iA Writer Warm Cream)
    dd 0x00DED6C6, 0x00C4BAAA, 0x00FAF4E8, 0x00EFE7D8
    dd 0x00222428, 0x00B4ACA0, 0x00C84B31, 0x004A6B7C

    ; Tema 1: Noite Obsidiana (OLED Dark Mode)
    dd 0x000D0F14, 0x0007080B, 0x00161922, 0x001E222E
    dd 0x00E4E0D8, 0x004E5464, 0x00E8A838, 0x0078A8B8

    ; Tema 2: Fósforo Âmbar (IBM 5151 CRT Retro)
    dd 0x00100A02, 0x00080501, 0x001A1104, 0x00261906
    dd 0x00FFB018, 0x006E4808, 0x00FFE066, 0x00CC8414

    ; Tema 3: Floresta Sálvia (FocusWriter Calm)
    dd 0x00111C16, 0x000A120E, 0x00182820, 0x0022362B
    dd 0x00DCECE2, 0x004E6E5E, 0x0098E068, 0x0080B8A8

font_tables:
    dw font_serif, font_sans, font_slab

theme_btn_labels:       dw str_tbtn_0, str_tbtn_1, str_tbtn_2, str_tbtn_3
str_tbtn_0:             db '[Tema:Papiro]', 0
str_tbtn_1:             db '[Tema:Noite] ', 0
str_tbtn_2:             db '[Tema:Ambar] ', 0
str_tbtn_3:             db '[Tema:Salvia]', 0

font_btn_labels:        dw str_fbtn_0, str_fbtn_1, str_fbtn_2
str_fbtn_0:             db '[Fonte:Serif]', 0
str_fbtn_1:             db '[Fonte:Sans] ', 0
str_fbtn_2:             db '[Fonte:Slab] ', 0

focus_labels:           dw str_focus_0, str_focus_1, str_focus_2
str_focus_0:            db '[Foco:Livre] ', 0
str_focus_1:            db '[Foco:Linha] ', 0
str_focus_2:            db '[Foco:Parag.]', 0

str_slot_file:          db 'MANUSCR1.MD', 0
str_btn_save:           db '[Salvar]', 0
str_badge_tw:           db '[Maq]', 0
str_badge_hem:          db '[Hem]', 0
str_btn_pomo:           db '[Pomodoro]', 0
str_btn_zen:            db '[Zen]', 0

str_words_lbl:          db ' palavras / ', 0
str_sep_pipe:           db '  |  ', 0
str_chars_lbl:          db ' carac.  |  ~', 0
str_read_lbl:           db ' min leitura', 0
str_res_lbl:            db 'VBE TrueColor ', 0
str_usb_res_lbl:        db 'USB HID + VBE ', 0

; Mensagens Toast
msg_saved_ok:           db 'Manuscrito gravado diretamente no Pen-Drive USB (FAT16 sincronizado).', 0
msg_autosaved:          db 'Auto-Save: Manuscrito sincronizado no Pen-Drive USB.', 0
msg_save_err:           db 'Alerta: Falha de escrita INT 13h no Pen-Drive.', 0
msg_new_doc:            db 'Folha limpa iniciada. (O rascunho anterior foi salvo no Pen-Drive).', 0
msg_theme_changed:      db 'Paleta TrueColor 32-bit atualizada (F2 ou clique em [Tema]).', 0
msg_font_changed:       db 'Familia tipografica grafica alterada: Folio Serif / Clean Sans / Courier Slab.', 0
msg_focus_changed:      db 'Modo Foco alternado: Livre / Linha / Paragrafo (F3 ou clique em [Foco]).', 0
msg_typewriter_changed: db 'Modo Maquina de Escrever (linha centralizada) alternado (F4).', 0
msg_hemingway_changed:  db 'Modo Hemingway (bloqueio de edicao retroativa) alternado (F5).', 0
msg_hemingway_lock:     db 'MODO HEMINGWAY ATIVO: Proibido apagar! Continue escrevendo (F5 desativa).', 0
msg_pomo_start:         db 'Sprint Pomodoro de 25 minutos iniciado (F6). Boa escrita!', 0
msg_pomo_stop:          db 'Sprint Pomodoro encerrado (F6).', 0
msg_pomo_done:          db 'SPRINT CONCLUIDO! 25 minutos de foco completo.', 0
msg_goal_changed:       db 'Meta de palavras da sessao atualizada (F7).', 0
msg_slot_loaded:        db 'Manuscrito carregado diretamente do Pen-Drive USB.', 0
msg_sound_changed:      db 'Feedback acustico de maquina de escrever alternado (F9).', 0
msg_zen_on:             db 'Modo Ultra-Zen ativado (Clique no topo ou pressione ESC para restaurar).', 0
msg_zen_off:            db 'Barra de ferramentas grafica restaurada.', 0

; Strings do Modal de Ajuda (F1)
help_title:             db 'FOLIO-86 v2.1 : GUIA GRAFICO & MOUSE USB/PS2', 0
help_line1:             db 'Mouse USB   : Clique no texto/botoes + Roda de Rolagem vertical', 0
help_line2:             db 'Ctrl + S    : Salvar imediatamente no Pen-Drive USB', 0
help_line3:             db 'Ctrl + F    : Alternar Fonte (Folio Serif / Clean Sans / Slab)', 0
help_line4:             db 'F2          : Alternar Paleta TrueColor (Papiro/Noite/Ambar/Salvia)', 0
help_line5:             db 'F3          : Modo Foco (Desativado / Linha / Paragrafo)', 0
help_line6:             db 'F4 / F5     : Modo Maquina de Escrever / Modo Hemingway', 0
help_line7:             db 'F6 / F7     : Sprint Pomodoro (25m) / Meta de Palavras', 0
help_line8:             db 'F8          : Trocar Manuscrito no USB (MANUSCR1..8.MD)', 0
help_line9:             db 'F9 / F10    : Clique Mecanico / Modo Ultra-Zen (Tela Limpa)', 0
help_footer:            db '[ Clique com o mouse ou pressione qualquer tecla para voltar ]', 0

; Strings do Modal de Slots (F8)
slot_modal_title:       db 'SELECIONE O MANUSCRITO NO PEN-DRIVE (CLIQUE OU 1..8)', 0
str_active_tag:         db '  <-- ATIVO AGORA', 0
slot_modal_footer:      db 'Clique em um manuscrito ou pressione [1]..[8] ([ESC] fecha)', 0

; Inclui as 3 Famílias de Fontes Gráficas 8x16 (4.560 bytes)
%include "src/legacy/fonts.inc"

align 4
vbe_edid_buf:           times 128 db 0
vbe_mode_info:          times 256 db 0
sector_buf:             times 512 db 0

; Preenchimento exato para 48 setores (24.576 bytes)
times (48 * 512) - ($ - $$) db 0
