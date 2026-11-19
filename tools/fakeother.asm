; FolioOS — um "outro sistema" de mentira para PCs só BIOS (o laboratório e os testes da fase 3)
;   nasm -f bin fakeother.asm -o fakeother.bin   -> 440 bytes de código de MBR
; Faz o papel do MBR do Windows ou do GRUB: quando roda, lê o setor 0 do disco que recebeu em DL (como um MBR de
; verdade faz para achar a partição ativa) e confere que a tabela de lá é a mesma que está em 0x7C00; depois
; escreve FAKE OTHER STARTED (DL OK) — ou (DL WRONG) — na serial (COM1) e na tela, e para. O laboratório confere a
; linha no registro da serial: o seletor do FolioOS iniciou o MBR original, com o número do disco certo.
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
    mov [drive], dl                ; antes de qualquer uso de DX
    mov si, bad                    ; até prova em contrário
    mov ax, 0x0201                 ; ler 1 setor: cilindro 0, cabeça 0, setor 1, do disco recebido
    mov cx, 0x0001
    xor dh, dh
    mov dl, [drive]
    mov bx, 0x8000
    int 0x13
    jc .say
    mov si, 0x7C00 + 446           ; a tabela (e a assinatura) que a ponte pôs junto do código
    mov di, 0x8000 + 446
    mov cx, 66
    repe cmpsb
    mov si, bad
    jne .say
    mov si, good
.say:
    push si
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
    call puts
    pop si
    call puts
.done:
    cli
    hlt
    jmp .done
puts:
    lodsb
    test al, al
    jz .r
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
    jmp puts
.r: ret
drive: db 0
msg:  db 13, 10, "FAKE OTHER STARTED", 0
good: db " (DL OK)", 13, 10, 0
bad:  db " (DL WRONG)", 13, 10, 0
    times 440 - ($ - $$) db 0
