; FolioOS — início do FOLIO\BIOS.SYS (nasm -f bin, carregado pelo VBR em 0x8000 com DL = disco)
;   modo real: A20, mapa de memória (E820), modo protegido de 32 bits sem paginação e chama a ponte C
;   (bridge.bin, ligada em 0x10000). Fica residente: rm_int volta ao modo real para chamar o BIOS
;   (INT 13h disco, INT 10h vídeo) e retorna ao modo protegido.
bits 16
org 0x8000
E820_BUF equ 0x1000                ; até 128 entradas de 24 bytes
RM_STACK equ 0x7C00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, RM_STACK
    sti
    cld
    mov [boot_drive], dl
    mov si, msg_hello
    call print
    ; --- A20: BIOS, depois a porta 0x92 ("fast A20"), depois o controlador de teclado
    call a20_ok
    jnc .a20done
    mov ax, 0x2401
    int 0x15
    call a20_ok
    jnc .a20done
    in al, 0x92
    test al, 2
    jnz .kbc
    or al, 2
    and al, 0xFE
    out 0x92, al
    call a20_ok
    jnc .a20done
.kbc:
    call kbc_wait
    mov al, 0xD1
    out 0x64, al
    call kbc_wait
    mov al, 0xDF
    out 0x60, al
    call kbc_wait
    call a20_ok
    jnc .a20done
    mov si, msg_a20
    call print
    jmp $
.a20done:
    ; --- mapa de memória (E820)
    xor ebx, ebx
    mov di, E820_BUF
    xor bp, bp
.e820:
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150            ; "SMAP"
    mov dword [di + 20], 1
    int 0x15
    jc .e820done
    cmp eax, 0x534D4150
    jne .e820done
    jcxz .skip
    inc bp
    add di, 24
    cmp bp, 128
    jae .e820done
.skip:
    test ebx, ebx
    jnz .e820
.e820done:
    mov [e820_count], bp
    ; --- modo protegido
    cli
    lgdt [gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword 0x08:pm_entry

a20_ok:                            ; CF = 0 se a A20 estiver ligada
    push ds
    push es
    xor ax, ax
    mov ds, ax
    not ax
    mov es, ax
    mov ax, [0x7DFE]
    mov bx, [es:0x7E0E]
    cmp ax, bx
    jne .on
    mov word [0x7DFE], 0x1234
    cmp word [es:0x7E0E], 0x1234
    mov [0x7DFE], ax
    jne .on
    stc
    jmp .out
.on:
    clc
.out:
    pop es
    pop ds
    ret
kbc_wait:
    in al, 0x64
    test al, 2
    jnz kbc_wait
    ret
print:
    lodsb
    test al, al
    jz .r
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp print
.r:
    ret

bits 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x70000               ; pilha provisória; a ponte troca por uma maior na memória alta
    fninit
    mov eax, cr0
    and eax, ~(1 << 2)             ; EM = 0 (FPU real)
    or eax, (1 << 1) | (1 << 5)    ; MP, NE
    mov cr0, eax
    push boot_info
    call 0x10000                   ; bridge_main(BootInfo *)
.dead:
    hlt
    jmp .dead

; ------------------------------------------------------------ volta ao modo real para chamar o BIOS
; void rm_int(int n, RmRegs *r)   (cdecl) — RmRegs: eax ebx ecx edx esi edi ebp (u32), ds es (u16), eflags (u32)
rm_int:
    pushad
    mov eax, [esp + 36]
    mov [.intno + 1], al           ; código automodificável: "int imm8"
    mov esi, [esp + 40]
    mov [regs_ptr], esi
    mov edi, rm_regs
    mov ecx, 9
    rep movsd                      ; 7 regs + ds/es + eflags = 36 bytes
    mov [saved_esp], esp
    sidt [pm_idt]
    jmp 0x18:.pm16
bits 16
.pm16:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    jmp 0:.rm
.rm:
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, RM_STACK
    lidt [rm_idtr]
    in al, 0xA1                    ; teclado e mouse ficam com a ponte: o BIOS não pode consumir os bytes
    mov [pic2], al
    or al, 0x10
    out 0xA1, al
    in al, 0x21
    mov [pic1], al
    or al, 0x02
    out 0x21, al
    mov ax, [rm_regs + 30]
    mov es, ax
    mov eax, [rm_regs + 0]
    mov ebx, [rm_regs + 4]
    mov ecx, [rm_regs + 8]
    mov edx, [rm_regs + 12]
    mov esi, [rm_regs + 16]
    mov edi, [rm_regs + 20]
    mov ebp, [rm_regs + 24]
    push word [rm_regs + 28]
    pop ds
    sti
.intno:
    int 0x00
    pushfd                         ; antes de qualquer outra instrução: o CF diz se o BIOS falhou
    cli
    push ds
    push ax
    xor ax, ax
    mov ds, ax
    pop ax
    mov [rm_regs + 0], eax
    mov [rm_regs + 4], ebx
    mov [rm_regs + 8], ecx
    mov [rm_regs + 12], edx
    mov [rm_regs + 16], esi
    mov [rm_regs + 20], edi
    mov [rm_regs + 24], ebp
    pop ax
    mov [rm_regs + 28], ax
    mov [rm_regs + 30], es
    pop eax
    mov [rm_regs + 32], eax
    mov al, [pic1]
    out 0x21, al
    mov al, [pic2]
    out 0xA1, al
    lgdt [gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword 0x08:.back
bits 32
.back:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, [saved_esp]
    lidt [pm_idt]
    mov esi, rm_regs
    mov edi, [regs_ptr]
    mov ecx, 9
    rep movsd
    popad
    ret

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF          ; 0x08 código 32 bits, base 0, 4 GB
    dq 0x00CF92000000FFFF          ; 0x10 dados 32 bits
    dq 0x00009A000000FFFF          ; 0x18 código 16 bits (64 KB)
    dq 0x000092000000FFFF          ; 0x20 dados 16 bits
gdtr:    dw gdtr - gdt - 1
         dd gdt
rm_idtr: dw 0x3FF
         dd 0
pm_idt:  dw 0
         dd 0
align 4
saved_esp: dd 0
regs_ptr:  dd 0
rm_regs:   times 9 dd 0
pic1: db 0
pic2: db 0
boot_drive: db 0
align 4
; estrutura passada à ponte (ver BootInfo em bridge.c)
boot_info:
    dd 0x464F4C49                  ; "ILOF"
    dd boot_drive
    dd e820_count
    dd E820_BUF
    dd rm_int
e820_count: dd 0
msg_hello: db "FolioOS BIOS bridge", 13, 10, 0
msg_a20:   db "FolioOS: could not enable the A20 line", 0

    times 0x8000 - ($ - $$) db 0   ; a ponte começa em 0x10000
incbin "bridge.bin"                ; nasm -i build/bios/ (procura na pasta do build)
