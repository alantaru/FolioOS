; FolioOS — um "outro sistema" de mentira para PCs só BIOS (o laboratório e os testes da fase 3)
;   nasm -f bin fakeother.asm -o fakeother.bin   -> 440 bytes de código de MBR
; Faz o papel do MBR do Windows ou do GRUB: quando roda, escreve FAKE OTHER STARTED na serial (COM1) e na tela e
; para. O laboratório confere a linha no registro da serial: o seletor do FolioOS iniciou o MBR original.
bits 16
org 0x7C00
start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov dx, 0x3F8 + 1              ; COM1: sem interrupções, 115200 8N1
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8
    mov al, 1
    out dx, al
    mov dx, 0x3F8 + 1
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 3
    out dx, al
    mov si, msg
.next:
    lodsb
    test al, al
    jz .done
    mov bl, al
    mov dx, 0x3F8 + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, 0x3F8
    mov al, bl
    out dx, al
    mov ah, 0x0E                   ; e na tela
    mov bh, 0
    int 0x10
    jmp .next
.done:
    cli
    hlt
    jmp .done
msg: db 13, 10, "FAKE OTHER STARTED", 13, 10, 0
    times 440 - ($ - $$) db 0
