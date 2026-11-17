; FolioOS — setores de boot para PCs só com BIOS (nasm -f bin)
;   nasm -f bin -DMBR boot.asm -o mbr.bin   -> 440 bytes de código do MBR (a tabela de partições fica intacta)
;   nasm -f bin -DVBR boot.asm -o vbr.bin   -> 512 bytes; o instalador copia só o salto (0..2) e o código (90..509),
;                                              o BPB do FAT32 (3..89) do pendrive nunca é tocado
; Nada aqui grava no disco: só lê. Nenhum arquivo do usuário é tocado.

%ifdef MBR
; ------------------------------------------------------------------------------------------- MBR
; Copia a si mesmo para 0x0600, acha a partição FAT32 (a ativa; se nenhuma estiver marcada — comum em
; pendrives formatados pelo Windows — a primeira FAT32) e passa o controle ao setor de boot dela.
bits 16
org 0x0600
mbr:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov si, 0x7C00
    mov di, 0x0600
    mov cx, 256
    rep movsw
    jmp 0:.moved
.moved:
    sti
    mov [drive], dl
    mov si, 0x07BE                 ; 1ª passada: partição ativa
.act:
    test byte [si], 0x80
    jnz .go
    add si, 16
    cmp si, 0x07FE
    jb .act
    mov si, 0x07BE                 ; 2ª passada: primeira FAT32 (0x0B/0x0C) ou EFI/FAT (0xEF)
.fat:
    mov al, [si + 4]
    cmp al, 0x0B
    je .go
    cmp al, 0x0C
    je .go
    cmp al, 0xEF
    je .go
    add si, 16
    cmp si, 0x07FE
    jb .fat
    mov si, msg_nopart
    jmp fail
.go:
    mov eax, [si + 8]              ; LBA do início da partição
    push si
    push dword 0
    push eax
    push word 0                    ; segmento
    push word 0x7C00               ; deslocamento
    push word 1
    push word 0x10
    mov si, sp
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    lea sp, [si + 16]
    pop si
    jc .ioerr
    cmp word [0x7DFE], 0xAA55
    jne .ioerr
    mov dl, [drive]
    jmp 0:0x7C00                   ; DS:SI aponta para a entrada da partição (convenção)
.ioerr:
    mov si, msg_io
fail:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp fail
.halt:
    int 0x18                       ; o BIOS tenta o próximo dispositivo
    hlt
    jmp .halt
drive:      db 0x80
msg_nopart: db "FolioOS: no FAT32 partition", 0
msg_io:     db "FolioOS: disk read error", 0
    times 440 - ($ - $$) db 0
%endif

%ifdef VBR
; ------------------------------------------------------------------------------------------- VBR
; Carrega \FOLIO\BIOS.SYS (cadeia de clusters, um setor por vez) em 0x8000 e salta para lá com DL = disco.
bits 16
org 0x7C00
BPB_SPC   equ 0x7C00 + 13
BPB_RSVD  equ 0x7C00 + 14
BPB_NFATS equ 0x7C00 + 16
BPB_HID   equ 0x7C00 + 28
BPB_FATSZ equ 0x7C00 + 36
BPB_ROOT  equ 0x7C00 + 44
DIRBUF    equ 0x0600
FATBUF    equ 0x7E00
    jmp short vbr
    nop
    times 90 - ($ - $$) db 0       ; BPB do pendrive (preservado pelo instalador)
vbr:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [vdrive], dl
    mov eax, [BPB_HID]
    movzx ebx, word [BPB_RSVD]
    add eax, ebx
    mov [fat_lba], eax
    movzx cx, byte [BPB_NFATS]
.nf:
    add eax, [BPB_FATSZ]
    loop .nf
    mov [data_lba], eax
    mov eax, [BPB_ROOT]
    mov si, name_dir
    call find
    mov si, name_sys
    call find
    mov word [dseg], 0x0800        ; destino: 0x0800:0000 = 0x8000
.load:
    push eax
    call clus_lba
    movzx cx, byte [BPB_SPC]
.lsec:
    push cx
    mov es, [dseg]
    xor bx, bx
    call read1
    add word [dseg], 32            ; 512 bytes
    inc eax
    pop cx
    loop .lsec
    pop eax
    call next
    cmp eax, 0x0FFFFFF8
    jb .load
    xor ax, ax
    mov es, ax
    mov dl, [vdrive]
    jmp 0:0x8000

; eax = cluster da pasta, si = nome 8.3 (11 bytes) -> eax = primeiro cluster da entrada
find:
.clus:
    mov [cur], eax
    call clus_lba
    movzx cx, byte [BPB_SPC]
.sec:
    push cx
    push eax
    xor bx, bx
    mov es, bx
    mov bx, DIRBUF
    call read1
    mov di, DIRBUF
    mov cx, 16
.ent:
    cmp byte [di], 0
    je notfound
    pusha
    mov cx, 11
    repe cmpsb
    popa
    jne .nxt
    test byte [di + 11], 0x08      ; o rótulo do volume costuma ter o mesmo nome ("FOLIO")
    jz .found
.nxt:
    add di, 32
    loop .ent
    pop eax
    pop cx
    inc eax
    loop .sec
    mov eax, [cur]
    call next
    cmp eax, 0x0FFFFFF8
    jb .clus
    jmp notfound
.found:
    add sp, 6
    mov ax, [di + 20]
    shl eax, 16
    mov ax, [di + 26]
    ret
notfound:
    mov si, msg_missing
.p:
    lodsb
    test al, al
    jz .h
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp .p
.h:
    int 0x18
    hlt
    jmp .h

; eax = cluster -> eax = LBA do primeiro setor
clus_lba:
    sub eax, 2
    movzx ecx, byte [BPB_SPC]
    mul ecx
    add eax, [data_lba]
    ret

; eax = cluster -> eax = próximo cluster da cadeia
next:
    push es
    mov edi, eax
    shr eax, 7
    add eax, [fat_lba]
    xor bx, bx
    mov es, bx
    mov bx, FATBUF
    call read1
    and di, 127
    shl di, 2
    mov eax, [FATBUF + di]
    and eax, 0x0FFFFFFF
    pop es
    ret

; lê 1 setor (LBA eax) para es:bx (INT 13h estendida)
read1:
    pushad
    push dword 0
    push eax
    push es
    push bx
    push word 1
    push word 0x10
    mov si, sp
    mov ah, 0x42
    mov dl, [vdrive]
    int 0x13
    jc .err
    add sp, 16
    popad
    ret
.err:
    mov si, msg_io2
    jmp notfound.p

name_dir:    db "FOLIO      "
name_sys:    db "BIOS    SYS"
msg_missing: db "FolioOS: FOLIO\BIOS.SYS missing", 0
msg_io2:     db "FolioOS: read error", 0
vdrive: db 0
    times 510 - ($ - $$) db 0
    dw 0xAA55
; variáveis fora do setor (0x7E00+ fica para o buffer da FAT: usam 0x0500)
fat_lba  equ 0x0500
data_lba equ 0x0504
cur      equ 0x0508
dseg     equ 0x050C
%endif
