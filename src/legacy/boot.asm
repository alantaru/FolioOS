; =============================================================================
; FOLIO-86 v3.0 — Bare-Metal Distraction-Free Writer (Pure MBR + UEFI/BIOS)
; Arquivo: src/boot.asm (LBA 0 — Pure Master Boot Record + Stage-1 Loader)
; Por que LBA 0 é um MBR Puro e a Partição #1 inicia em LBA 63:
;   1. Sem BPB em LBA 0, o Windows (partmgr.sys), Rufus e a BIOS do HP ProBook
;      tratam o pen-drive como um disco particionado real (e não Superfloppy).
;   2. LBA 63 corresponde exatamente a CHS = (Cilindro 0, Cabeça 1, Setor 1)
;      com 63 setores/trilha, e permite que o VBR em LBA 63 tenha
;      BPB_RsvdSecCnt = 1 (exigido pela especificação Microsoft fatgen103),
;      alinhando a FAT16 em LBA 64, Diretório Raiz em LBA 80 e Dados em LBA 112.
; =============================================================================

BITS 16
ORG 0x7C00

KERNEL_SEG      EQU 0x0800
KERNEL_LBA      EQU 1
KERNEL_SECTORS  EQU 48          ; 48 setores = 24 KB (Motor Gráfico VBE + 3 Fontes + Mouse USB)
TOTAL_SECTORS   EQU 16472       ; 8.433.664 bytes (~8,1 MB -> 4.090 clusters FAT16 >= 4.085)
PART1_START_LBA EQU 63          ; CHS (0, 1, 1) -> VBR em LBA 63 + 1 reservado = FAT16 em LBA 64

boot_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    ; Salva o drive físico de boot fornecido pela BIOS (ex: 0x80 para USB HDD)
    mov [boot_drive], dl

    ; Tenta carregar o Kernel usando INT 13h Extended Read (LBA)
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .fallback_chs
    cmp bx, 0xAA55
    jne .fallback_chs

    ; Leitura via LBA (Disk Address Packet)
    mov si, dap_packet
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jnc .boot_kernel

.fallback_chs:
    mov dl, [boot_drive]
    cmp dl, 0x80
    jb .load_loop_init
    mov ah, 0x08
    int 0x13
    jc .load_loop_init
    and cl, 0x3F
    movzx ax, cl
    mov [chs_secs_per_track], ax
    movzx ax, dh
    inc ax
    mov [chs_num_heads], ax
    xor ax, ax
    mov es, ax

.load_loop_init:
    mov cx, KERNEL_SECTORS
    mov word [cur_lba], KERNEL_LBA
    mov word [cur_seg], KERNEL_SEG

.load_loop:
    push cx
    mov ax, [cur_lba]
    xor dx, dx
    div word [chs_secs_per_track]
    mov cl, dl
    inc cl
    xor dx, dx
    div word [chs_num_heads]
    mov dh, dl
    mov ch, al
    shl ah, 6
    or cl, ah

    mov es, [cur_seg]
    xor bx, bx
    mov ax, 0x0201
    mov dl, [boot_drive]
    int 0x13
    jc .disk_error

    inc word [cur_lba]
    add word [cur_seg], 0x0020
    pop cx
    loop .load_loop

.boot_kernel:
    mov dl, [boot_drive]
    jmp KERNEL_SEG:0x0000

.disk_error:
    mov si, msg_err
    call print_str
    cli
    hlt

print_str:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print_str
.done:
    ret

boot_drive:         db 0x80
cur_lba:            dw KERNEL_LBA
cur_seg:            dw KERNEL_SEG
chs_secs_per_track: dw 63
chs_num_heads:      dw 16

align 4
dap_packet:
    db 0x10
    db 0x00
    dw KERNEL_SECTORS
    dw 0x0000
    dw KERNEL_SEG
    dq KERNEL_LBA

msg_err:            db 'Erro de leitura no Pen-Drive.', 0

; -----------------------------------------------------------------------------
; Tabela de Partições MBR Padrão (Offset 0x01B8..0x01FD)
; Partição #1 Ativa (0x80), Tipo 0x0E (FAT16 LBA), StartingLBA = 63 (CHS 0/1/1)
; -----------------------------------------------------------------------------
times 440 - ($ - $$) db 0
mbr_disk_sig:       dd 0x2026F086
mbr_reserved:       dw 0x0000

; Partição #1 (Offset 0x01BE — 16 bytes)
mbr_part1:
    db 0x80                             ; Status: 0x80 (Ativa / Bootável -> IsActive=True)
    db 0x01, 0x01, 0x00                 ; CHS inicial: Head 1, Sector 1, Cyl 0 (exato LBA 63!)
    db 0x0E                             ; Tipo: 0x0E (FAT16 LBA — Windows + UEFI EDK2)
    db 0x0F, 0xFF, 0xFF                 ; CHS final
    dd PART1_START_LBA                  ; StartingLBA = 63 (Offset = 32.256 bytes)
    dd (TOTAL_SECTORS - PART1_START_LBA); SizeInLBA = 16.409 setores (4.090 clusters FAT16)

; Partições #2, #3 e #4 vazias (48 bytes)
times 48 db 0

dw 0xAA55
