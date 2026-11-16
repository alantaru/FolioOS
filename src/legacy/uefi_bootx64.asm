; =============================================================================
; FOLIO-86 v3.0 — Native 64-bit UEFI Application (\EFI\BOOT\BOOTX64.EFI)
; Arquivo: src/uefi_bootx64.asm
; Compilacao direta via NASM (nasm -f bin) — Zero Linker Externo
; Suporta: HP ProBook 640 G5 & PCs UEFI x86_64
;   - UEFI Graphics Output Protocol (GOP + Linear Framebuffer Fallback)
;   - Multi-Handle EFI_SIMPLE_POINTER_PROTOCOL (Mouses USB HID + Touchpads PS/2)
;   - Multi-Handle EFI_ABSOLUTE_POINTER_PROTOCOL (Touchpads I2C Precision HP)
;   - EFI_LOADED_IMAGE_PROTOCOL + EFI_SIMPLE_FILE_SYSTEM_PROTOCOL (USB FAT16/32)
;   - Suporte Integral PT-BR: Acentos ('~^`"), Cedilha (ç, Ç) e UTF-8 Nativo
; =============================================================================

BITS 64
DEFAULT REL
ORG 0x00000000

IMAGE_BASE          EQU 0x00400000
TEXT_RVA            EQU 0x00001000
TEXT_SIZE           EQU 0x00006000      ; 24 KB (.text + dados + fontes)
RELOC_RVA           EQU 0x00007000
RELOC_SIZE          EQU 0x00001000      ; 4 KB (.reloc)
IMAGE_TOTAL_SIZE    EQU 0x00008000      ; 32 KB (32.768 bytes = 64 setores)

SCREEN_W            EQU 1024
SCREEN_H            EQU 768
SCREEN_PITCH        EQU 4096
SHEET_X1            EQU 132
SHEET_Y1            EQU 44
SHEET_X2            EQU 892
SHEET_Y2            EQU 728
TEXT_ORIGIN_X       EQU 212
TEXT_ORIGIN_Y       EQU 72
TEXT_COLS           EQU 60
TEXT_ROWS           EQU 26
CHAR_W              EQU 10
CHAR_H              EQU 24
MAX_DOC_SIZE        EQU 32760
MAX_POINTERS        EQU 8

; =============================================================================
; Cabeçalho PE32+ (4096 bytes, RVA == FileOffset)
; =============================================================================
dos_header:
    dw 0x5A4D                           ; e_magic = 'MZ'
    times 29 dw 0
    dd pe_header                        ; e_lfanew = 0x0040

pe_header:
    dd 0x00004550                       ; Assinatura 'PE\0\0'
    dw 0x8664                           ; Machine = IMAGE_FILE_MACHINE_AMD64
    dw 2                                ; NumberOfSections = 2 (.text, .reloc)
    dd 0x6732A000                       ; TimeDateStamp
    dd 0                                ; PointerToSymbolTable
    dd 0                                ; NumberOfSymbols
    dw (opt_header_end - opt_header_start) ; SizeOfOptionalHeader = 240
    dw 0x020E                           ; Characteristics (Executable, LargeAddressAware)

opt_header_start:
    dw 0x020B                           ; Magic = PE32+ (64-bit)
    db 14                               ; MajorLinkerVersion
    db 0                                ; MinorLinkerVersion
    dd TEXT_SIZE                        ; SizeOfCode
    dd RELOC_SIZE                       ; SizeOfInitializedData
    dd 0                                ; SizeOfUninitializedData
    dd TEXT_RVA                         ; AddressOfEntryPoint = 0x1000
    dd TEXT_RVA                         ; BaseOfCode = 0x1000
    dq IMAGE_BASE                       ; ImageBase
    dd 0x00001000                       ; SectionAlignment = 4096
    dd 0x00001000                       ; FileAlignment = 4096
    dw 0                                ; MajorOperatingSystemVersion
    dw 0                                ; MinorOperatingSystemVersion
    dw 3                                ; MajorImageVersion
    dw 0                                ; MinorImageVersion
    dw 0                                ; MajorSubsystemVersion
    dw 0                                ; MinorSubsystemVersion
    dd 0                                ; Win32VersionValue
    dd IMAGE_TOTAL_SIZE                 ; SizeOfImage = 0x8000
    dd 0x00001000                       ; SizeOfHeaders = 4096
    dd 0                                ; CheckSum
    dw 10                               ; Subsystem = IMAGE_SUBSYSTEM_EFI_APPLICATION
    dw 0                                ; DllCharacteristics
    dq 0x00100000                       ; SizeOfStackReserve (1 MB)
    dq 0x00010000                       ; SizeOfStackCommit (64 KB)
    dq 0x00100000                       ; SizeOfHeapReserve (1 MB)
    dq 0x00010000                       ; SizeOfHeapCommit (64 KB)
    dd 0                                ; LoaderFlags
    dd 16                               ; NumberOfRvaAndSizes

    ; Data Directories (16 entries)
    dq 0                                ; 0: Export
    dq 0                                ; 1: Import
    dq 0                                ; 2: Resource
    dq 0                                ; 3: Exception
    dq 0                                ; 4: Certificate
    dd RELOC_RVA                        ; 5: Base Relocation Table RVA
    dd 12                               ;    Base Relocation Table Size
    times 10 dq 0                       ; 6..15: Unused
opt_header_end:

section_headers:
    ; Section 1: .text (Code + Data + Fonts)
    db '.text', 0, 0, 0
    dd TEXT_SIZE                        ; VirtualSize
    dd TEXT_RVA                         ; VirtualAddress
    dd TEXT_SIZE                        ; SizeOfRawData
    dd TEXT_RVA                         ; PointerToRawData
    dd 0                                ; PointerToRelocations
    dd 0                                ; PointerToLinenumbers
    dw 0                                ; NumberOfRelocations
    dw 0                                ; NumberOfLinenumbers
    dd 0xE0000060                       ; Characteristics (R/W/X Code + Data)

    ; Section 2: .reloc (Base Relocations)
    db '.reloc', 0, 0
    dd 12                               ; VirtualSize
    dd RELOC_RVA                        ; VirtualAddress
    dd RELOC_SIZE                       ; SizeOfRawData
    dd RELOC_RVA                        ; PointerToRawData
    dd 0                                ; PointerToRelocations
    dd 0                                ; PointerToLinenumbers
    dw 0                                ; NumberOfRelocations
    dw 0                                ; NumberOfLinenumbers
    dd 0x42000040                       ; Characteristics (Discardable Read-Only Data)

times TEXT_RVA - ($ - $$) db 0

; =============================================================================
; Seção .text (RVA 0x1000) — Ponto de Entrada UEFI x86_64
; RCX = EFI_HANDLE ImageHandle
; RDX = EFI_SYSTEM_TABLE *SystemTable
; =============================================================================
efi_main:
    push rbp
    push rbx
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 0x68                       ; Alinhamento de 16 bytes + espaço para 10 args (Blt)

    mov [efi_image_handle], rcx
    mov [efi_system_table], rdx
    mov rax, [rdx + 0x28]               ; SystemTable->ConsoleInHandle
    mov [efi_con_in_handle], rax
    mov rax, [rdx + 0x30]               ; SystemTable->ConIn
    mov [efi_con_in], rax
    mov rax, [rdx + 0x60]               ; SystemTable->BootServices
    mov [efi_boot_services], rax

    ; 1. Desativa o Watchdog Timer de 5 minutos do UEFI (SetWatchdogTimer = BS + 0x100)
    xor rcx, rcx
    xor rdx, rdx
    xor r8, r8
    xor r9, r9
    mov rax, [efi_boot_services]
    call [rax + 0x100]

    ; 2. Aloca 4 MB para o Backbuffer Gráfico + 64 KB para o Manuscrito (AllocatePool = BS + 0x40)
    mov ecx, 2                          ; EfiLoaderData
    mov edx, 0x00400000                 ; 4 MB
    lea r8, [backbuffer_ptr]
    mov rax, [efi_boot_services]
    call [rax + 0x40]
    test rax, rax
    jnz .fatal_exit

    mov ecx, 2                          ; EfiLoaderData
    mov edx, 0x00010000                 ; 64 KB
    lea r8, [doc_buffer_ptr]
    mov rax, [efi_boot_services]
    call [rax + 0x40]
    test rax, rax
    jnz .fatal_exit

    ; 3. Inicializa o UEFI Graphics Output Protocol (GOP)
    call uefi_init_gop
    test eax, eax
    jnz .fatal_exit

    ; 4. Inicializa todos os dispositivos SimplePointer e AbsolutePointer (Mouse USB + Touchpad HP)
    call uefi_init_pointer

    ; 5. Abre o Volume FAT do Pen-Drive USB via LoadedImageProtocol + SimpleFileSystemProtocol
    call uefi_init_filesystem

    ; 6. Carrega o Manuscrito inicial (MANUSCR1.MD) e renderiza o primeiro quadro completo
    mov byte [current_slot], 0
    call uefi_load_slot
    call uefi_fill_native_border
    mov byte [needs_redraw], 1

.main_loop:
    cmp byte [needs_redraw], 1
    jne .skip_render
    call uefi_render_frame
    mov byte [needs_redraw], 0

.skip_render:
    ; Se max_frames_limit > 0 (usado pela VM de teste automatizado), verifica encerramento
    mov eax, [max_frames_limit]
    test eax, eax
    jz .do_stall
    inc dword [frames_executed]
    mov edx, [frames_executed]
    cmp edx, eax
    jae .fatal_exit

.do_stall:
    ; Pausa curta de 4 ms (Stall = BS + 0xF8) para ~240 Hz de amostragem suave
    mov ecx, 4000                       ; 4000 microssegundos = 4 ms
    mov rax, [efi_boot_services]
    call [rax + 0xF8]

    ; Atualiza o Pomodoro Timer a cada ~1 segundo (250 iterações de 4 ms)
    inc dword [tick_accum]
    cmp dword [tick_accum], 250
    jb .poll_inputs
    mov dword [tick_accum], 0
    cmp byte [pomo_running], 1
    jne .poll_inputs
    cmp dword [pomo_seconds], 0
    je .pomo_finished
    dec dword [pomo_seconds]
    mov byte [needs_redraw], 1
    jmp .poll_inputs

.pomo_finished:
    mov byte [pomo_running], 0
    lea rax, [msg_pomo_done]
    mov [status_msg_ptr], rax
    mov byte [needs_redraw], 1

.poll_inputs:
    call uefi_poll_pointer
    call uefi_poll_keyboard
    jmp .main_loop

.fatal_exit:
    add rsp, 0x68
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbx
    pop rbp
    xor eax, eax
    ret

; =============================================================================
; Inicialização do UEFI Graphics Output Protocol (GOP)
; =============================================================================
uefi_init_gop:
    sub rsp, 0x28
    lea rcx, [guid_gop]
    xor rdx, rdx
    lea r8, [gop_ptr]
    mov rax, [efi_boot_services]
    call [rax + 0x140]                  ; LocateProtocol
    test rax, rax
    jnz .fail

    mov rcx, [gop_ptr]
    mov rdx, [rcx + 0x18]               ; Gop->Mode
    mov rax, [rdx + 0x18]               ; Gop->Mode->FrameBufferBase
    mov [gop_fb_base], rax
    mov r8, [rdx + 0x08]                ; Gop->Mode->Info
    mov eax, [r8 + 0x04]                ; HorizontalResolution
    mov [native_w], eax
    mov eax, [r8 + 0x08]                ; VerticalResolution
    mov [native_h], eax
    mov eax, [r8 + 0x20]                ; PixelsPerScanLine
    test eax, eax
    jnz .ppsl_ok
    mov eax, [native_w]
.ppsl_ok:
    mov [native_ppsl], eax

    ; Calcula offset centralizado para telas maiores que 1024x768 (ex: 1920x1080 no HP ProBook 640 G5)
    mov eax, [native_w]
    cmp eax, SCREEN_W
    jbe .zero_x
    sub eax, SCREEN_W
    shr eax, 1
    mov [screen_off_x], eax
    jmp .calc_y
.zero_x:
    mov dword [screen_off_x], 0

.calc_y:
    mov eax, [native_h]
    cmp eax, SCREEN_H
    jbe .zero_y
    sub eax, SCREEN_H
    shr eax, 1
    mov [screen_off_y], eax
    jmp .ok
.zero_y:
    mov dword [screen_off_y], 0

.ok:
    xor eax, eax
    add rsp, 0x28
    ret
.fail:
    mov eax, 1
    add rsp, 0x28
    ret

; Preenche a moldura nativa ao redor da área de 1024x768 com a cor do tema atual
uefi_fill_native_border:
    sub rsp, 0x58
    mov rcx, [gop_ptr]
    test rcx, rcx
    jz .done

    movzx eax, byte [current_theme]
    shl eax, 5
    lea rdx, [theme_table]
    mov eax, [rdx + rax + 0]            ; bg_desk
    mov [blt_fill_pixel], eax

    mov rcx, [gop_ptr]
    lea rdx, [blt_fill_pixel]
    xor r8d, r8d                        ; EfiBltVideoFill = 0
    xor r9d, r9d                        ; SourceX = 0
    mov qword [rsp + 0x20], 0           ; SourceY = 0
    mov qword [rsp + 0x28], 0           ; DestinationX = 0
    mov qword [rsp + 0x30], 0           ; DestinationY = 0
    mov eax, [native_w]
    mov [rsp + 0x38], rax               ; Width
    mov eax, [native_h]
    mov [rsp + 0x40], rax               ; Height
    mov qword [rsp + 0x48], 0           ; Delta = 0
    call [rcx + 0x10]                   ; Gop->Blt
.done:
    add rsp, 0x58
    ret

; =============================================================================
; Inicialização Multi-Handle de Mouse USB / Touchpad (SimplePointer + AbsolutePointer)
; =============================================================================
uefi_init_pointer:
    push rbx
    push rsi
    push rdi
    sub rsp, 0x30

    mov dword [pointer_count], 0

    ; 1. Tenta o agregador ConSplitter em SystemTable->ConsoleInHandle (HandleProtocol = BS + 0x98)
    mov rcx, [efi_con_in_handle]
    test rcx, rcx
    jz .try_locate_buf
    lea rdx, [guid_simple_pointer]
    lea r8, [temp_ptr_iface]
    mov rax, [efi_boot_services]
    call [rax + 0x98]                   ; HandleProtocol
    test rax, rax
    jnz .try_locate_buf
    mov rax, [temp_ptr_iface]
    call uefi_register_pointer

.try_locate_buf:
    ; 2. Enumera todos os handles SimplePointer no sistema (LocateHandleBuffer = BS + 0x138)
    mov ecx, 2                          ; ByProtocol = 2
    lea rdx, [guid_simple_pointer]
    xor r8, r8                          ; SearchKey = NULL
    lea r9, [ptr_handle_count]
    lea rax, [ptr_handle_buf]
    mov [rsp + 0x20], rax
    mov rax, [efi_boot_services]
    call [rax + 0x138]                  ; LocateHandleBuffer
    test rax, rax
    jnz .try_single

    xor ebx, ebx
.handle_loop:
    cmp rbx, [ptr_handle_count]
    jae .try_single
    cmp dword [pointer_count], MAX_POINTERS
    jae .try_single

    mov rsi, [ptr_handle_buf]
    mov rcx, [rsi + rbx * 8]
    lea rdx, [guid_simple_pointer]
    lea r8, [temp_ptr_iface]
    mov rax, [efi_boot_services]
    call [rax + 0x98]                   ; HandleProtocol
    test rax, rax
    jnz .next_handle
    mov rax, [temp_ptr_iface]
    call uefi_register_pointer

.next_handle:
    inc rbx
    jmp .handle_loop

.try_single:
    ; 3. Fallback: LocateProtocol direto caso nenhum handle tenha sido listado ainda
    cmp dword [pointer_count], 0
    jne .try_abs
    lea rcx, [guid_simple_pointer]
    xor rdx, rdx
    lea r8, [temp_ptr_iface]
    mov rax, [efi_boot_services]
    call [rax + 0x140]                  ; LocateProtocol
    test rax, rax
    jnz .try_abs
    mov rax, [temp_ptr_iface]
    call uefi_register_pointer

.try_abs:
    ; 4. Procura também EFI_ABSOLUTE_POINTER_PROTOCOL (Touchpads I2C Precision HP ProBook)
    lea rcx, [guid_abs_pointer]
    xor rdx, rdx
    lea r8, [abs_ptr_iface]
    mov rax, [efi_boot_services]
    call [rax + 0x140]                  ; LocateProtocol
    test rax, rax
    jnz .done
    mov rcx, [abs_ptr_iface]
    test rcx, rcx
    jz .done
    xor edx, edx
    call [rcx + 0x00]                   ; AbsPointer->Reset

.done:
    add rsp, 0x30
    pop rdi
    pop rsi
    pop rbx
    ret

; Registra ponteiro RAX em pointer_list (evitando duplicatas) e chama Reset(FALSE)
uefi_register_pointer:
    push rbx
    push rsi
    sub rsp, 0x28
    test rax, rax
    jz .reg_done
    mov ecx, [pointer_count]
    cmp ecx, MAX_POINTERS
    jae .reg_done

    lea rsi, [pointer_list]
    xor ebx, ebx
.chk_dup:
    cmp ebx, ecx
    jae .add_it
    cmp [rsi + rbx * 8], rax
    je .reg_done
    inc ebx
    jmp .chk_dup

.add_it:
    mov [rsi + rcx * 8], rax
    inc ecx
    mov [pointer_count], ecx

    ; Chama SimplePointer->Reset(This, FALSE)
    mov rcx, rax
    xor edx, edx
    call [rcx + 0x00]

.reg_done:
    add rsp, 0x28
    pop rsi
    pop rbx
    ret

; Lê estado de TODOS os mouses USB e Touchpads registrados
uefi_poll_pointer:
    push rbx
    push rsi
    sub rsp, 0x28

    mov ecx, [pointer_count]
    test ecx, ecx
    jz .poll_abs

    xor ebx, ebx
.poll_loop:
    cmp ebx, [pointer_count]
    jae .poll_abs

    lea rsi, [pointer_list]
    mov rcx, [rsi + rbx * 8]
    test rcx, rcx
    jz .next_dev

    lea rdx, [ptr_state]
    call [rcx + 0x08]                   ; SimplePointer->GetState
    test rax, rax
    jnz .next_dev

    ; Escala inteligente de RelativeMovementX para Mouses USB (counts 1..30) e Touchpads I2C/PS2
    mov eax, [ptr_state + 0x00]         ; RelativeMovementX (int32)
    test eax, eax
    jz .do_y
    cmp eax, 256
    jg .scale_x_hi
    cmp eax, -256
    jl .scale_x_hi
    cmp eax, 32
    jg .scale_x_mid
    cmp eax, -32
    jl .scale_x_mid
    shl eax, 1                          ; Movimentos finos de Mouse USB (* 2 pixels)
    jmp .apply_x
.scale_x_mid:
    sar eax, 2
    jmp .apply_x
.scale_x_hi:
    sar eax, 6
.apply_x:
    add eax, [mouse_x]
    cmp eax, 0
    jge .chk_x_max
    xor eax, eax
.chk_x_max:
    cmp eax, SCREEN_W - 1
    jle .set_x
    mov eax, SCREEN_W - 1
.set_x:
    cmp eax, [mouse_x]
    je .do_y
    mov [mouse_x], eax
    mov byte [needs_redraw], 1

.do_y:
    mov eax, [ptr_state + 0x04]         ; RelativeMovementY (int32)
    test eax, eax
    jz .do_wheel
    cmp eax, 256
    jg .scale_y_hi
    cmp eax, -256
    jl .scale_y_hi
    cmp eax, 32
    jg .scale_y_mid
    cmp eax, -32
    jl .scale_y_mid
    shl eax, 1
    jmp .apply_y
.scale_y_mid:
    sar eax, 2
    jmp .apply_y
.scale_y_hi:
    sar eax, 6
.apply_y:
    add eax, [mouse_y]
    cmp eax, 0
    jge .chk_y_max
    xor eax, eax
.chk_y_max:
    cmp eax, SCREEN_H - 1
    jle .set_y
    mov eax, SCREEN_H - 1
.set_y:
    cmp eax, [mouse_y]
    je .do_wheel
    mov [mouse_y], eax
    mov byte [needs_redraw], 1

.do_wheel:
    mov eax, [ptr_state + 0x08]         ; RelativeMovementZ (Scroll Wheel)
    test eax, eax
    jz .do_buttons
    js .wheel_down
    cmp dword [scroll_line], 2
    jb .wheel_zero
    sub dword [scroll_line], 2
    jmp .wheel_updated
.wheel_zero:
    mov dword [scroll_line], 0
    jmp .wheel_updated
.wheel_down:
    add dword [scroll_line], 2
.wheel_updated:
    mov byte [needs_redraw], 1

.do_buttons:
    movzx eax, byte [ptr_state + 0x0C]  ; LeftButton
    movzx edx, byte [mouse_prev_btn]
    mov [mouse_prev_btn], al
    test eax, eax
    jz .next_dev
    test edx, edx
    jnz .next_dev
    call uefi_handle_mouse_click
    mov byte [needs_redraw], 1

.next_dev:
    inc ebx
    jmp .poll_loop

.poll_abs:
    ; Verifica também Touchpad I2C Precision via EFI_ABSOLUTE_POINTER_PROTOCOL
    mov rcx, [abs_ptr_iface]
    test rcx, rcx
    jz .done
    lea rdx, [abs_state]
    call [rcx + 0x08]                   ; AbsPointer->GetState
    test rax, rax
    jnz .done

    mov rcx, [abs_ptr_iface]
    mov rsi, [rcx + 0x18]               ; AbsPointer->Mode
    test rsi, rsi
    jz .done
    mov r8, [rsi + 0x18]                ; AbsoluteMaxX
    test r8, r8
    jz .done
    mov r9, [rsi + 0x20]                ; AbsoluteMaxY
    test r9, r9
    jz .done

    mov rax, [abs_state + 0x00]         ; CurrentX
    imul rax, SCREEN_W
    xor edx, edx
    div r8
    cmp eax, SCREEN_W - 1
    jbe .abs_x_ok
    mov eax, SCREEN_W - 1
.abs_x_ok:
    mov [mouse_x], eax

    mov rax, [abs_state + 0x08]         ; CurrentY
    imul rax, SCREEN_H
    xor edx, edx
    div r9
    cmp eax, SCREEN_H - 1
    jbe .abs_y_ok
    mov eax, SCREEN_H - 1
.abs_y_ok:
    mov [mouse_y], eax
    mov byte [needs_redraw], 1

    mov eax, [abs_state + 0x18]         ; ActiveButtons
    and eax, 1
    movzx edx, byte [abs_prev_btn]
    mov [abs_prev_btn], al
    test eax, eax
    jz .done
    test edx, edx
    jnz .done
    call uefi_handle_mouse_click
    mov byte [needs_redraw], 1

.done:
    add rsp, 0x28
    pop rsi
    pop rbx
    ret

; =============================================================================
; Clique do Mouse na Barra Superior, Modal de Manuscritos ou Folha de Papel
; =============================================================================
uefi_handle_mouse_click:
    mov byte [needs_redraw], 1          ; Forca redesenho visual imediato para todo clique!
    mov byte [dead_key], 0              ; Cancela dead key em cliques de mouse
    cmp byte [modal_active], 1
    jne .check_toolbar

    ; Cliques nos 8 cartões do Modal (2 colunas x 4 linhas)
    mov eax, [mouse_x]
    mov edx, [mouse_y]
    cmp edx, 200
    jb .close_modal
    cmp edx, 550
    ja .close_modal
    sub edx, 200
    mov eax, edx
    xor edx, edx
    mov ebx, 86
    div ebx
    cmp eax, 3
    ja .close_modal
    mov ecx, eax                        ; Row 0..3

    mov eax, [mouse_x]
    cmp eax, 250
    jb .close_modal
    cmp eax, 505
    jbe .col_left
    cmp eax, 520
    jb .close_modal
    cmp eax, 775
    ja .close_modal
    lea eax, [rcx * 2 + 1]
    jmp .select_slot
.col_left:
    lea eax, [rcx * 2]
.select_slot:
    mov [current_slot], al
    mov byte [modal_active], 0
    call uefi_load_slot
    ret
.close_modal:
    mov byte [modal_active], 0
    ret

.check_toolbar:
    cmp byte [zen_mode], 1
    jne .check_tb_y
    ; No modo Ultra-Zen, clicar no topo (Y < 34) restaura a barra de ferramentas
    mov edx, [mouse_y]
    cmp edx, 34
    ja .check_sheet
    mov byte [zen_mode], 0
    ret

.check_tb_y:
    mov edx, [mouse_y]
    cmp edx, 34
    ja .check_sheet

    ; Hitboxes exatas calculadas sobre CHAR_W = 10 px
    mov eax, [mouse_x]
    cmp eax, 150                        ; MANUSCR1.MD (16..126)
    jb .open_modal
    cmp eax, 255                        ; [Salvar] (165..245)
    jb .do_save
    cmp eax, 335                        ; [Tema] (265..325)
    jb .do_theme
    cmp eax, 425                        ; [Fonte] (345..415)
    jb .do_font
    cmp eax, 505                        ; [Foco] (435..495)
    jb .do_focus
    cmp eax, 575                        ; [Maq] (515..565)
    jb .do_typewriter
    cmp eax, 645                        ; [Hem] (585..635)
    jb .do_hemingway
    cmp eax, 765                        ; [Pomodoro] (655..755)
    jb .do_pomo
    cmp eax, 850                        ; [Zen] (775..825)
    jb .do_zen
    ret

.do_zen:
    mov byte [zen_mode], 1
    ret
.open_modal:
    mov byte [modal_active], 1
    ret
.do_save:
    call uefi_save_slot
    ret
.do_theme:
    inc byte [current_theme]
    and byte [current_theme], 3
    call uefi_fill_native_border
    ret
.do_font:
    inc byte [current_font]
    cmp byte [current_font], 3
    jb .font_ok
    mov byte [current_font], 0
.font_ok:
    ret
.do_focus:
    inc byte [focus_mode]
    cmp byte [focus_mode], 3
    jb .focus_ok
    mov byte [focus_mode], 0
.focus_ok:
    ret
.do_typewriter:
    xor byte [typewriter_mode], 1
    ret
.do_hemingway:
    xor byte [hemingway_mode], 1
    ret
.do_pomo:
    xor byte [pomo_running], 1
    ret

.check_sheet:
    mov eax, [mouse_x]
    mov edx, [mouse_y]
    cmp eax, TEXT_ORIGIN_X
    jb .ret
    cmp edx, TEXT_ORIGIN_Y
    jb .ret
    sub eax, TEXT_ORIGIN_X
    xor edx, edx
    mov ebx, CHAR_W
    div ebx
    mov r8d, eax                        ; target_col

    mov eax, [mouse_y]
    sub eax, TEXT_ORIGIN_Y
    xor edx, edx
    mov ebx, CHAR_H
    div ebx
    add eax, [scroll_line]              ; target_line
    mov r9d, eax

    ; Percorre o documento até (r9d, r8d)
    mov rsi, [doc_buffer_ptr]
    mov ecx, [doc_len]
    xor ebx, ebx                        ; idx = 0
    xor edx, edx                        ; cur_line = 0
    xor edi, edi                        ; cur_col = 0
.find_loop:
    cmp edx, r9d
    jb .advance
    ja .found
    cmp edi, r8d
    jae .found
.advance:
    cmp ebx, ecx
    jae .found
    mov al, [rsi + rbx]
    inc ebx
    cmp al, 10
    je .newline
    ; Se for lead byte UTF-8 (0xC2..0xDF), consome trail byte sem avançar coluna gráfica
    cmp al, 0xC2
    jb .mouse_adv_col
    cmp al, 0xDF
    ja .mouse_adv_col
    cmp ebx, ecx
    jae .mouse_adv_col
    inc ebx
.mouse_adv_col:
    inc edi
    cmp edi, TEXT_COLS
    jb .find_loop
.newline:
    inc edx
    xor edi, edi
    jmp .find_loop
.found:
    mov [cursor_idx], ebx
.ret:
    ret

; =============================================================================
; Leitura de Teclado UEFI (Drena todas as teclas com Suporte Completo PT-BR)
; =============================================================================
uefi_poll_keyboard:
    sub rsp, 0x28
.read_next_key:
    mov rcx, [efi_con_in]
    test rcx, rcx
    jz .done
    lea rdx, [efi_key]
    call [rcx + 0x08]                   ; ReadKeyStroke
    test rax, rax
    jnz .done

    mov byte [needs_redraw], 1
    movzx eax, word [efi_key + 0]       ; ScanCode
    movzx edx, word [efi_key + 2]       ; UnicodeChar

    cmp byte [modal_active], 1
    jne .normal_keys
    cmp eax, 0x17                       ; ESC
    je .close_modal_k
    cmp edx, '1'
    jb .read_next_key
    cmp edx, '8'
    ja .read_next_key
    sub edx, '1'
    mov [current_slot], dl
    mov byte [modal_active], 0
    mov byte [dead_key], 0
    call uefi_load_slot
    jmp .read_next_key
.close_modal_k:
    mov byte [modal_active], 0
    mov byte [dead_key], 0
    jmp .read_next_key

.normal_keys:
    cmp eax, 0x01                       ; Up
    je .k_up
    cmp eax, 0x02                       ; Down
    je .k_down
    cmp eax, 0x03                       ; Right
    je .k_right
    cmp eax, 0x04                       ; Left
    je .k_left
    cmp eax, 0x05                       ; Home
    je .k_home
    cmp eax, 0x06                       ; End
    je .k_end
    cmp eax, 0x08                       ; Delete
    je .k_del
    cmp eax, 0x09                       ; PageUp
    je .k_pgup
    cmp eax, 0x0A                       ; PageDown
    je .k_pgdn
    cmp eax, 0x0C                       ; F2 (Tema / Salvar)
    je .k_theme
    cmp eax, 0x0D                       ; F3 (Foco)
    je .k_focus
    cmp eax, 0x0E                       ; F4 (Máquina)
    je .k_typewriter
    cmp eax, 0x0F                       ; F5 (Hemingway)
    je .k_hem
    cmp eax, 0x10                       ; F6 (Pomodoro)
    je .k_pomo
    cmp eax, 0x12                       ; F8 (Manuscritos)
    je .k_modal
    cmp eax, 0x14                       ; F10 (Zen)
    je .k_zen

    ; Atalhos Ctrl+Letra via UnicodeChar
    cmp edx, 0x13                       ; Ctrl+S
    je .k_save
    cmp edx, 0x14                       ; Ctrl+T
    je .k_theme
    cmp edx, 0x06                       ; Ctrl+F
    je .k_font
    cmp edx, 0x0E                       ; Ctrl+N (Novo Documento Limpo)
    je .k_new_doc
    cmp edx, 0x10                       ; Ctrl+P
    je .k_pomo
    cmp edx, 0x1A                       ; Ctrl+Z
    je .k_zen
    cmp edx, 0x0F                       ; Ctrl+O
    je .k_modal
    cmp edx, 0x08                       ; Backspace
    je .k_bs
    cmp edx, 0x0D                       ; Enter
    je .k_enter

    ; 1. Suporte direto a Latin-1 (0x00A0..0x00FF) de teclados ABNT2 (ex: 'ç' = 0x00E7, 'Ç' = 0x00C7)
    cmp edx, 0x00A0
    jb .chk_ascii
    cmp edx, 0x00FF
    ja .read_next_key
    mov byte [dead_key], 0
    mov eax, edx
    call uefi_insert_codepoint
    jmp .read_next_key

.chk_ascii:
    cmp edx, 0x20
    jb .read_next_key
    cmp edx, 0x7E
    ja .read_next_key

    ; Verifica máquina de estados Dead Key PT-BR
    movzx eax, byte [dead_key]
    test eax, eax
    jnz .process_dead_key

    ; Verifica se esta tecla é o início de uma dead key:
    cmp edx, 0x27                       ; ' (Acento Agudo / Cedilha)
    je .set_dead_key
    cmp edx, 0x7E                       ; ~ (Til)
    je .set_dead_key
    cmp edx, 0x5E                       ; ^ (Circunflexo)
    je .set_dead_key
    cmp edx, 0x60                       ; ` (Crase)
    je .set_dead_key
    cmp edx, 0x22                       ; " (Trema)
    je .set_dead_key

    ; Caractere ASCII comum
    mov eax, edx
    call uefi_insert_codepoint
    jmp .read_next_key

.set_dead_key:
    mov [dead_key], dl
    jmp .read_next_key

.process_dead_key:
    mov byte [dead_key], 0              ; Consome a dead key
    mov r8d, eax                        ; r8d = dead_key anterior
    mov r9d, edx                        ; r9d = tecla atual

    cmp r9d, 0x20                       ; Espaço após dead key -> emite o símbolo da dead key
    jne .chk_same_dk
    mov eax, r8d
    call uefi_insert_codepoint
    jmp .read_next_key

.chk_same_dk:
    cmp r9d, r8d                        ; Mesma dead key repetida ('' ou ~~) -> emite ambas
    jne .chk_acute
    mov eax, r8d
    call uefi_insert_codepoint
    mov eax, r8d
    call uefi_insert_codepoint
    jmp .read_next_key

.chk_acute:
    cmp r8d, 0x27                       ; '
    jne .chk_tilde
    cmp r9d, 'c'
    je .c_cedil
    cmp r9d, 'C'
    je .c_cedil_cap
    cmp r9d, 'a'
    je .a_acute
    cmp r9d, 'e'
    je .e_acute
    cmp r9d, 'i'
    je .i_acute
    cmp r9d, 'o'
    je .o_acute
    cmp r9d, 'u'
    je .u_acute
    cmp r9d, 'A'
    je .a_acute_cap
    cmp r9d, 'E'
    je .e_acute_cap
    cmp r9d, 'I'
    je .i_acute_cap
    cmp r9d, 'O'
    je .o_acute_cap
    cmp r9d, 'U'
    je .u_acute_cap
    jmp .dk_fallback

.c_cedil:
    mov eax, 0x00E7                     ; ç
    call uefi_insert_codepoint
    jmp .read_next_key
.c_cedil_cap:
    mov eax, 0x00C7                     ; Ç
    call uefi_insert_codepoint
    jmp .read_next_key
.a_acute:
    mov eax, 0x00E1                     ; á
    call uefi_insert_codepoint
    jmp .read_next_key
.a_acute_cap:
    mov eax, 0x00C1                     ; Á
    call uefi_insert_codepoint
    jmp .read_next_key
.e_acute:
    mov eax, 0x00E9                     ; é
    call uefi_insert_codepoint
    jmp .read_next_key
.e_acute_cap:
    mov eax, 0x00C9                     ; É
    call uefi_insert_codepoint
    jmp .read_next_key
.i_acute:
    mov eax, 0x00ED                     ; í
    call uefi_insert_codepoint
    jmp .read_next_key
.i_acute_cap:
    mov eax, 0x00CD                     ; Í
    call uefi_insert_codepoint
    jmp .read_next_key
.o_acute:
    mov eax, 0x00F3                     ; ó
    call uefi_insert_codepoint
    jmp .read_next_key
.o_acute_cap:
    mov eax, 0x00D3                     ; Ó
    call uefi_insert_codepoint
    jmp .read_next_key
.u_acute:
    mov eax, 0x00FA                     ; ú
    call uefi_insert_codepoint
    jmp .read_next_key
.u_acute_cap:
    mov eax, 0x00DA                     ; Ú
    call uefi_insert_codepoint
    jmp .read_next_key

.chk_tilde:
    cmp r8d, 0x7E                       ; ~
    jne .chk_circ
    cmp r9d, 'a'
    je .a_tilde
    cmp r9d, 'o'
    je .o_tilde
    cmp r9d, 'A'
    je .a_tilde_cap
    cmp r9d, 'O'
    je .o_tilde_cap
    jmp .dk_fallback
.a_tilde:
    mov eax, 0x00E3                     ; ã
    call uefi_insert_codepoint
    jmp .read_next_key
.a_tilde_cap:
    mov eax, 0x00C3                     ; Ã
    call uefi_insert_codepoint
    jmp .read_next_key
.o_tilde:
    mov eax, 0x00F5                     ; õ
    call uefi_insert_codepoint
    jmp .read_next_key
.o_tilde_cap:
    mov eax, 0x00D5                     ; Õ
    call uefi_insert_codepoint
    jmp .read_next_key

.chk_circ:
    cmp r8d, 0x5E                       ; ^
    jne .chk_grave
    cmp r9d, 'a'
    je .a_circ
    cmp r9d, 'e'
    je .e_circ
    cmp r9d, 'o'
    je .o_circ
    cmp r9d, 'A'
    je .a_circ_cap
    cmp r9d, 'E'
    je .e_circ_cap
    cmp r9d, 'O'
    je .o_circ_cap
    jmp .dk_fallback
.a_circ:
    mov eax, 0x00E2                     ; â
    call uefi_insert_codepoint
    jmp .read_next_key
.a_circ_cap:
    mov eax, 0x00C2                     ; Â
    call uefi_insert_codepoint
    jmp .read_next_key
.e_circ:
    mov eax, 0x00EA                     ; ê
    call uefi_insert_codepoint
    jmp .read_next_key
.e_circ_cap:
    mov eax, 0x00CA                     ; Ê
    call uefi_insert_codepoint
    jmp .read_next_key
.o_circ:
    mov eax, 0x00F4                     ; ô
    call uefi_insert_codepoint
    jmp .read_next_key
.o_circ_cap:
    mov eax, 0x00D4                     ; Ô
    call uefi_insert_codepoint
    jmp .read_next_key

.chk_grave:
    cmp r8d, 0x60                       ; `
    jne .chk_trema
    cmp r9d, 'a'
    je .a_grave
    cmp r9d, 'A'
    je .a_grave_cap
    jmp .dk_fallback
.a_grave:
    mov eax, 0x00E0                     ; à
    call uefi_insert_codepoint
    jmp .read_next_key
.a_grave_cap:
    mov eax, 0x00C0                     ; À
    call uefi_insert_codepoint
    jmp .read_next_key

.chk_trema:
    cmp r8d, 0x22                       ; "
    jne .dk_fallback
    cmp r9d, 'u'
    je .u_trema
    cmp r9d, 'U'
    je .u_trema_cap
    jmp .dk_fallback
.u_trema:
    mov eax, 0x00FC                     ; ü
    call uefi_insert_codepoint
    jmp .read_next_key
.u_trema_cap:
    mov eax, 0x00DC                     ; Ü
    call uefi_insert_codepoint
    jmp .read_next_key

.dk_fallback:
    ; Combinação não acentuada (ex: 's ou ~b): emite ambos os caracteres
    mov eax, r8d
    call uefi_insert_codepoint
    mov eax, r9d
    call uefi_insert_codepoint
    jmp .read_next_key

.k_new_doc:
    mov byte [dead_key], 0
    mov dword [doc_len], 0
    mov dword [cursor_idx], 0
    mov dword [scroll_line], 0
    mov byte [doc_dirty], 1
    jmp .read_next_key
.k_save:
    mov byte [dead_key], 0
    call uefi_save_slot
    jmp .read_next_key
.k_theme:
    mov byte [dead_key], 0
    inc byte [current_theme]
    and byte [current_theme], 3
    call uefi_fill_native_border
    jmp .read_next_key
.k_font:
    mov byte [dead_key], 0
    inc byte [current_font]
    cmp byte [current_font], 3
    jb .read_next_key
    mov byte [current_font], 0
    jmp .read_next_key
.k_focus:
    mov byte [dead_key], 0
    inc byte [focus_mode]
    cmp byte [focus_mode], 3
    jb .read_next_key
    mov byte [focus_mode], 0
    jmp .read_next_key
.k_typewriter:
    mov byte [dead_key], 0
    xor byte [typewriter_mode], 1
    jmp .read_next_key
.k_hem:
    mov byte [dead_key], 0
    xor byte [hemingway_mode], 1
    jmp .read_next_key
.k_pomo:
    mov byte [dead_key], 0
    xor byte [pomo_running], 1
    jmp .read_next_key
.k_modal:
    mov byte [dead_key], 0
    mov byte [modal_active], 1
    jmp .read_next_key
.k_zen:
    mov byte [dead_key], 0
    xor byte [zen_mode], 1
    jmp .read_next_key

.k_left:
    mov byte [dead_key], 0
    mov eax, [cursor_idx]
    test eax, eax
    jz .read_next_key
    dec eax
    mov rdi, [doc_buffer_ptr]
    mov cl, [rdi + rax]
    ; Se for byte de continuação UTF-8 (0x80..0xBF) e eax > 0, pula mais 1 byte para trás!
    cmp cl, 0x80
    jb .left_done
    cmp cl, 0xBF
    ja .left_done
    test eax, eax
    jz .left_done
    dec eax
.left_done:
    mov [cursor_idx], eax
    jmp .read_next_key

.k_right:
    mov byte [dead_key], 0
    mov eax, [cursor_idx]
    cmp eax, [doc_len]
    jae .read_next_key
    mov rdi, [doc_buffer_ptr]
    mov cl, [rdi + rax]
    inc eax
    ; Se for lead byte UTF-8 (0xC2..0xDF) e houver continuação, pula o próximo byte também
    cmp cl, 0xC2
    jb .right_done
    cmp cl, 0xDF
    ja .right_done
    cmp eax, [doc_len]
    jae .right_done
    inc eax
.right_done:
    mov [cursor_idx], eax
    jmp .read_next_key

.k_up:
    mov byte [dead_key], 0
    cmp dword [cursor_idx], TEXT_COLS
    jb .k_home
    sub dword [cursor_idx], TEXT_COLS
    jmp .read_next_key
.k_down:
    mov byte [dead_key], 0
    mov eax, [cursor_idx]
    add eax, TEXT_COLS
    cmp eax, [doc_len]
    ja .k_end
    mov [cursor_idx], eax
    jmp .read_next_key
.k_home:
    mov byte [dead_key], 0
    mov dword [cursor_idx], 0
    jmp .read_next_key
.k_end:
    mov byte [dead_key], 0
    mov eax, [doc_len]
    mov [cursor_idx], eax
    jmp .read_next_key
.k_pgup:
    mov byte [dead_key], 0
    cmp dword [scroll_line], 10
    jb .pg0
    sub dword [scroll_line], 10
    jmp .read_next_key
.pg0:
    mov dword [scroll_line], 0
    jmp .read_next_key
.k_pgdn:
    mov byte [dead_key], 0
    add dword [scroll_line], 10
    jmp .read_next_key

.k_enter:
    mov byte [dead_key], 0
    mov al, 10
    call uefi_insert_byte
    jmp .read_next_key

.k_bs:
    ; Se havia dead key pendente, Backspace apenas a cancela
    cmp byte [dead_key], 0
    je .bs_exec_chk
    mov byte [dead_key], 0
    jmp .read_next_key

.bs_exec_chk:
    cmp byte [hemingway_mode], 1
    je .read_next_key
    mov eax, [cursor_idx]
    test eax, eax
    jz .read_next_key
    mov rdi, [doc_buffer_ptr]
    mov edx, 1                          ; deleta 1 byte por padrão
    mov cl, [rdi + rax - 1]
    ; Se o byte anterior for continuação UTF-8 (0x80..0xBF) e eax >= 2, deleta 2 bytes!
    cmp cl, 0x80
    jb .bs_run
    cmp cl, 0xBF
    ja .bs_run
    cmp eax, 2
    jb .bs_run
    mov edx, 2
.bs_run:
    mov ecx, [doc_len]
    sub ecx, eax                        ; bytes após o cursor
    lea rsi, [rdi + rax]
    sub eax, edx                        ; novo cursor_idx
    mov [cursor_idx], eax
    mov rdi, rsi
    sub rdi, rdx
    cld
    rep movsb
    sub [doc_len], edx
    mov byte [doc_dirty], 1
    jmp .read_next_key

.k_del:
    mov byte [dead_key], 0
    cmp byte [hemingway_mode], 1
    je .read_next_key
    mov eax, [cursor_idx]
    cmp eax, [doc_len]
    jae .read_next_key
    mov rdi, [doc_buffer_ptr]
    mov edx, 1                          ; deleta 1 byte por padrão
    mov cl, [rdi + rax]
    ; Se o byte atual for lead byte UTF-8 (0xC2..0xDF) e houver >= 2 bytes restantes, deleta 2!
    cmp cl, 0xC2
    jb .del_run
    cmp cl, 0xDF
    ja .del_run
    mov ecx, [doc_len]
    sub ecx, eax
    cmp ecx, 2
    jb .del_run
    mov edx, 2
.del_run:
    mov ecx, [doc_len]
    sub ecx, eax
    sub ecx, edx
    lea rdi, [rdi + rax]
    lea rsi, [rdi + rdx]
    cld
    rep movsb
    sub [doc_len], edx
    mov byte [doc_dirty], 1
    jmp .read_next_key

.done:
    add rsp, 0x28
    ret

; Insere 1 byte isolado (AL) em doc_buffer[cursor_idx]
uefi_insert_byte:
    push rsi
    push rdi
    push rbx
    mov edx, [doc_len]
    cmp edx, MAX_DOC_SIZE - 2
    jae .ret_b
    mov ecx, edx
    sub ecx, [cursor_idx]
    mov rbx, [doc_buffer_ptr]
    test ecx, ecx
    jz .write_b
    lea rsi, [rbx + rdx - 1]
    lea rdi, [rbx + rdx]
    std
    rep movsb
    cld
.write_b:
    mov edx, [cursor_idx]
    mov [rbx + rdx], al
    inc dword [cursor_idx]
    inc dword [doc_len]
    mov byte [doc_dirty], 1
.ret_b:
    pop rbx
    pop rdi
    pop rsi
    ret

; Insere codepoint Unicode/Latin-1 EAX (0x20..0xFF)
; Se < 0x80: insere 1 byte ASCII
; Se >= 0x80: codifica em 2 bytes UTF-8 (0xC0 | (cp>>6), 0x80 | (cp&0x3F))
uefi_insert_codepoint:
    push rsi
    push rdi
    push rbx
    push r12
    push r13
    cmp eax, 0x80
    jb .single_cp

    ; Multi-byte UTF-8 de 2 bytes
    mov edx, [doc_len]
    cmp edx, MAX_DOC_SIZE - 4
    jae .done_cp

    mov r12d, eax
    shr r12d, 6
    or r12b, 0xC0                       ; r12b = lead byte
    mov r13d, eax
    and r13d, 0x3F
    or r13b, 0x80                       ; r13b = trail byte

    mov edx, [doc_len]
    mov ecx, edx
    sub ecx, [cursor_idx]
    mov rbx, [doc_buffer_ptr]
    test ecx, ecx
    jz .write_2cp
    lea rsi, [rbx + rdx - 1]
    lea rdi, [rbx + rdx + 1]           ; abre espaço de +2 bytes
    std
    rep movsb
    cld
.write_2cp:
    mov edx, [cursor_idx]
    mov [rbx + rdx], r12b
    mov [rbx + rdx + 1], r13b
    add dword [cursor_idx], 2
    add dword [doc_len], 2
    mov byte [doc_dirty], 1
    jmp .done_cp

.single_cp:
    call uefi_insert_byte

.done_cp:
    pop r13
    pop r12
    pop rbx
    pop rdi
    pop rsi
    ret

; =============================================================================
; Sistema de Arquivos UEFI (LoadedImage->DeviceHandle + SimpleFileSystem)
; Garante que abrimos o Pen-Drive USB exato de onde BOOTX64.EFI deu boot!
; =============================================================================
uefi_init_filesystem:
    sub rsp, 0x38

    ; 1. Obtém LoadedImageProtocol do próprio ImageHandle (HandleProtocol = BS + 0x98)
    mov rcx, [efi_image_handle]
    lea rdx, [guid_loaded_image]
    lea r8, [loaded_image_ptr]
    mov rax, [efi_boot_services]
    call [rax + 0x98]
    test rax, rax
    jnz .fallback_locate

    ; 2. Extrai LoadedImage->DeviceHandle (offset 0x18) e abre SimpleFileSystem nele
    mov rax, [loaded_image_ptr]
    mov rcx, [rax + 0x18]               ; DeviceHandle do Pen-Drive USB
    test rcx, rcx
    jz .fallback_locate
    lea rdx, [guid_sfs]
    lea r8, [sfs_ptr]
    mov rax, [efi_boot_services]
    call [rax + 0x98]                   ; HandleProtocol(DeviceHandle, &guid_sfs, &sfs_ptr)
    test rax, rax
    jz .open_vol

.fallback_locate:
    ; 3. Fallback: LocateProtocol(&guid_sfs)
    lea rcx, [guid_sfs]
    xor rdx, rdx
    lea r8, [sfs_ptr]
    mov rax, [efi_boot_services]
    call [rax + 0x140]                  ; LocateProtocol
    test rax, rax
    jnz .done

.open_vol:
    mov rcx, [sfs_ptr]
    lea rdx, [root_dir_handle]
    call [rcx + 0x08]                   ; OpenVolume
.done:
    add rsp, 0x38
    ret

uefi_update_filename:
    movzx eax, byte [current_slot]
    add eax, '1'
    mov [fname_manuscr + 14], ax        ; Atualiza '1'..'8' em u"MANUSCR1.MD"
    ret

uefi_load_slot:
    sub rsp, 0x48
    call uefi_update_filename
    mov rcx, [root_dir_handle]
    test rcx, rcx
    jz .fallback

    lea rdx, [file_handle]
    lea r8, [fname_manuscr]
    mov r9, 1                           ; EFI_FILE_MODE_READ
    mov qword [rsp + 0x20], 0
    call [rcx + 0x08]                   ; RootDir->Open
    test rax, rax
    jnz .fallback

    mov qword [io_byte_size], MAX_DOC_SIZE
    mov rcx, [file_handle]
    lea rdx, [io_byte_size]
    mov r8, [doc_buffer_ptr]
    call [rcx + 0x20]                   ; File->Read

    mov rcx, [file_handle]
    call [rcx + 0x10]                   ; File->Close

    mov eax, [io_byte_size]
    mov rdi, [doc_buffer_ptr]
    xor ecx, ecx
.trim_nulls:
    cmp ecx, eax
    jae .len_ready
    cmp byte [rdi + rcx], 0
    je .len_ready
    inc ecx
    jmp .trim_nulls
.len_ready:
    mov [doc_len], ecx
    mov [cursor_idx], ecx
    mov dword [scroll_line], 0
    mov byte [doc_dirty], 0
    add rsp, 0x48
    ret

.fallback:
    mov rdi, [doc_buffer_ptr]
    lea rsi, [default_uefi_doc]
    mov ecx, default_uefi_doc_end - default_uefi_doc
    mov [doc_len], ecx
    mov [cursor_idx], ecx
    cld
    rep movsb
    mov byte [doc_dirty], 0
    add rsp, 0x48
    ret

uefi_save_slot:
    sub rsp, 0x48
    call uefi_update_filename
    mov rcx, [root_dir_handle]
    test rcx, rcx
    jz .done

    lea rdx, [file_handle]
    lea r8, [fname_manuscr]
    mov r9, 0x8000000000000003          ; READ | WRITE | CREATE
    mov qword [rsp + 0x20], 0
    call [rcx + 0x08]                   ; RootDir->Open
    test rax, rax
    jnz .done

    mov eax, [doc_len]
    mov [io_byte_size], rax
    mov rcx, [file_handle]
    lea rdx, [io_byte_size]
    mov r8, [doc_buffer_ptr]
    call [rcx + 0x28]                   ; File->Write

    mov rcx, [file_handle]
    call [rcx + 0x10]                   ; File->Close

    mov byte [doc_dirty], 0
    lea rax, [msg_saved_ok]
    mov [status_msg_ptr], rax
.done:
    add rsp, 0x48
    ret

; =============================================================================
; Motor Gráfico 64-bit TrueColor + Subpixel Anti-Aliased Typography
; Preserva estritamente TODOS os registradores dos loops chamadores!
; =============================================================================
uefi_fill_rect:
    ; r8d=x, r9d=y, r10d=w, r11d=h, eax=color
    push rax
    push rbx
    push rcx
    push rdx
    push rdi
    test r10d, r10d
    jle .done
    test r11d, r11d
    jle .done
    cmp r8d, 0
    jge .x_ok
    xor r8d, r8d
.x_ok:
    cmp r9d, 0
    jge .y_ok
    xor r9d, r9d
.y_ok:
    mov edx, r8d
    add edx, r10d
    cmp edx, SCREEN_W
    jle .w_ok
    mov r10d, SCREEN_W
    sub r10d, r8d
    jle .done
.w_ok:
    mov edx, r9d
    add edx, r11d
    cmp edx, SCREEN_H
    jle .h_ok
    mov r11d, SCREEN_H
    sub r11d, r9d
    jle .done
.h_ok:
    mov rdi, [backbuffer_ptr]
    mov ebx, r9d
    shl ebx, 12                         ; y * 4096
    lea rdi, [rdi + rbx]
    mov ebx, r8d
    shl ebx, 2
    add rdi, rbx
    mov edx, r11d
.row:
    push rdi
    mov ecx, r10d
    cld
    rep stosd
    pop rdi
    add rdi, SCREEN_PITCH
    dec edx
    jnz .row
.done:
    pop rdi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; Desenha um glifo 8x16 com Anti-Aliasing subpixel em (r8d=x, r9d=y), al=char, r10d=fg, r11d=style
; Preserva RAX, RBX, RCX, RDX, RSI, RDI, R12..R15!
uefi_draw_glyph:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15

    cmp al, 32
    jb .space
    ; Suporta toda a tabela de 224 glifos (32..255) com acentos PT-BR e cedilha!
    jmp .valid
.space:
    mov al, 32
.valid:
    movzx eax, al
    sub eax, 32
    shl eax, 4                          ; * 16 bytes/glifo

    movzx ebx, byte [current_font]
    cmp ebx, 1
    je .f_sans
    ja .f_slab
    lea rsi, [font_serif]
    jmp .f_ok
.f_sans:
    lea rsi, [font_sans]
    jmp .f_ok
.f_slab:
    lea rsi, [font_slab]
.f_ok:
    add rsi, rax

    ; Calcula cor de Anti-Aliasing 50% entre r10d (fg) e col_sheet (bg)
    mov r15d, r10d
    and r15d, 0x00FEFEFE
    shr r15d, 1
    mov eax, [col_sheet]
    and eax, 0x00FEFEFE
    shr eax, 1
    add r15d, eax                       ; r15d = cor AA 50%

    mov rdi, [backbuffer_ptr]
    mov eax, r9d
    add eax, 4
    shl eax, 12
    add rdi, rax
    mov eax, r8d
    inc eax
    shl eax, 2
    add rdi, rax

    xor r12d, r12d                      ; row = 0..15
.row_loop:
    movzx r13d, byte [rsi + r12]        ; bitmask 8 bits
    test r13d, r13d
    jz .next_row

    xor r14d, r14d                      ; shift itálico
    test r11d, 2
    jz .col_init
    mov eax, 15
    sub eax, r12d
    shr eax, 2
    mov r14d, eax
.col_init:
    mov ecx, 7
.col_loop:
    bt r13d, ecx
    jnc .skip_px
    mov eax, 7
    sub eax, ecx
    add eax, r14d
    mov [rdi + rax * 4], r10d

    ; Negrito sintético (bit 0 de r11d) ou Suavização Subpixel AA à direita
    test r11d, 1
    jz .aa_px
    mov [rdi + rax * 4 + 4], r10d
    jmp .skip_px
.aa_px:
    test ecx, ecx
    jz .skip_px
    mov edx, ecx
    dec edx
    bt r13d, edx
    jc .skip_px
    mov [rdi + rax * 4 + 4], r15d

.skip_px:
    dec ecx
    jns .col_loop

.next_row:
    add rdi, SCREEN_PITCH
    inc r12d
    cmp r12d, 16
    jb .row_loop

    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; Desenha string UTF-8 / ASCII terminada em zero: rsi=str, r8d=x, r9d=y, r10d=fg, r11d=style
uefi_draw_str:
    push rax
    push rbx
    push rsi
    push r8
.loop:
    lodsb
    test al, al
    jz .done
    ; Decodificador UTF-8 para sequências Latin-1 de 2 bytes (0xC2..0xDF)
    cmp al, 0xC2
    jb .draw_it
    cmp al, 0xDF
    ja .draw_it
    movzx ebx, byte [rsi]
    cmp bl, 0x80
    jb .draw_it
    cmp bl, 0xBF
    ja .draw_it
    inc rsi
    and al, 0x1F
    shl al, 6
    and bl, 0x3F
    or al, bl
.draw_it:
    call uefi_draw_glyph
    add r8d, CHAR_W
    jmp .loop
.done:
    pop r8
    pop rsi
    pop rbx
    pop rax
    ret

; Desenha inteiro decimal em (r8d=x, r9d=y), eax=valor, r10d=fg
uefi_draw_uint:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    lea rsi, [num_tmp_buf + 15]
    mov byte [rsi], 0
    mov ebx, 10
.div_loop:
    xor edx, edx
    div ebx
    add dl, '0'
    dec rsi
    mov [rsi], dl
    test eax, eax
    jnz .div_loop
    xor r11d, r11d
    call uefi_draw_str
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

uefi_render_frame:
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 0x50                       ; 7 pushes (56) + 0x50 (80) + ret (8) = 144 (múltiplo de 16!)

    ; Carrega paleta do tema atual usando RIP-relative puro
    movzx eax, byte [current_theme]
    shl eax, 5
    lea rbx, [theme_table]
    add rbx, rax
    mov eax, [rbx + 0]
    mov [col_desk], eax
    mov eax, [rbx + 4]
    mov [col_sheet], eax
    mov eax, [rbx + 8]
    mov [col_bar], eax
    mov eax, [rbx + 12]
    mov [col_ink], eax
    mov eax, [rbx + 16]
    mov [col_dim], eax
    mov eax, [rbx + 20]
    mov [col_accent], eax
    mov eax, [rbx + 24]
    mov [col_quote], eax
    mov eax, [rbx + 28]
    mov [col_shadow], eax

    ; 1. Fundo da Mesa + Sombra + Folha de Papel
    xor r8d, r8d
    xor r9d, r9d
    mov r10d, SCREEN_W
    mov r11d, SCREEN_H
    mov eax, [col_desk]
    call uefi_fill_rect

    mov r8d, SHEET_X1 + 8
    mov r9d, SHEET_Y1 + 8
    mov r10d, SHEET_X2 - SHEET_X1
    mov r11d, SHEET_Y2 - SHEET_Y1
    mov eax, [col_shadow]
    call uefi_fill_rect

    mov r8d, SHEET_X1
    mov r9d, SHEET_Y1
    mov r10d, SHEET_X2 - SHEET_X1
    mov r11d, SHEET_Y2 - SHEET_Y1
    mov eax, [col_sheet]
    call uefi_fill_rect

    ; 2. Barra Superior e Inferior (se não estiver em Zen Mode)
    cmp byte [zen_mode], 1
    je .render_text

    xor r8d, r8d
    xor r9d, r9d
    mov r10d, SCREEN_W
    mov r11d, 32
    mov eax, [col_bar]
    call uefi_fill_rect

    call uefi_update_filename
    lea rsi, [ui_slot_label]
    movzx eax, byte [current_slot]
    add al, '1'
    mov [rsi + 7], al
    mov r8d, 16
    mov r9d, 4
    mov r10d, [col_accent]
    mov r11d, 1
    call uefi_draw_str

    lea rsi, [ui_top_buttons]
    mov r8d, 165
    mov r9d, 4
    mov r10d, [col_ink]
    xor r11d, r11d
    call uefi_draw_str

    ; Barra Inferior
    xor r8d, r8d
    mov r9d, 740
    mov r10d, SCREEN_W
    mov r11d, 28
    mov eax, [col_bar]
    call uefi_fill_rect

    mov r8d, 20
    mov r9d, 742
    mov eax, [doc_len]
    mov r10d, [col_accent]
    call uefi_draw_uint

    lea rsi, [ui_chars_lbl]
    mov r8d, 68
    mov r9d, 742
    mov r10d, [col_dim]
    xor r11d, r11d
    call uefi_draw_str

    lea rsi, [ui_uefi_badge]
    mov r8d, 780
    mov r9d, 742
    mov r10d, [col_accent]
    xor r11d, r11d
    call uefi_draw_str

.render_text:
    ; 3. Renderiza as linhas visíveis do manuscrito
    mov rsi, [doc_buffer_ptr]
    mov ecx, [doc_len]
    xor ebx, ebx                        ; idx = 0
    xor edx, edx                        ; line = 0
    xor edi, edi                        ; col = 0
    xor r14d, r14d                      ; line_style (1=Heading, 2=Quote)

.text_loop:
    cmp ebx, ecx
    jae .draw_cursor_check

    ; No início de cada linha, verifica "# " ou "> "
    test edi, edi
    jnz .draw_char_vis
    xor r14d, r14d
    mov al, [rsi + rbx]
    cmp al, '#'
    jne .chk_q
    mov r14d, 1
    jmp .draw_char_vis
.chk_q:
    cmp al, '>'
    jne .draw_char_vis
    mov r14d, 2

.draw_char_vis:
    ; Desenha o cursor se ebx == cursor_idx
    cmp ebx, [cursor_idx]
    jne .no_cur_here
    mov eax, edx
    sub eax, [scroll_line]
    js .no_cur_here
    cmp eax, TEXT_ROWS
    jae .no_cur_here
    imul eax, CHAR_H
    add eax, TEXT_ORIGIN_Y + 2
    mov r9d, eax
    mov eax, edi
    imul eax, CHAR_W
    add eax, TEXT_ORIGIN_X
    mov r8d, eax
    mov r10d, 3
    mov r11d, 20
    mov eax, [col_accent]
    call uefi_fill_rect

.no_cur_here:
    movzx eax, byte [rsi + rbx]
    inc ebx
    cmp al, 10
    je .next_line

    ; Decodificador UTF-8 para sequências Latin-1 de 2 bytes (0xC2..0xDF)
    cmp al, 0xC2
    jb .single_char
    cmp al, 0xDF
    ja .single_char
    cmp ebx, ecx
    jae .single_char
    movzx r12d, byte [rsi + rbx]
    cmp r12b, 0x80
    jb .single_char
    cmp r12b, 0xBF
    ja .single_char
    ; Sequência válida de 2 bytes:
    inc ebx                             ; consome o trail byte do buffer
    and al, 0x1F
    shl al, 6
    and r12b, 0x3F
    or al, r12b                         ; al = codepoint Latin-1 (0x80..0xFF)
.single_char:
    mov r13b, al                        ; guarda o caractere decodificado em r13b

    mov r15d, edx
    sub r15d, [scroll_line]
    js .adv_col
    cmp r15d, TEXT_ROWS
    jae .adv_col

    ; Se for citação (r14d == 2) e coluna 0, desenha barra vertical estilizada à esquerda
    test edi, edi
    jnz .calc_glyph_xy
    cmp r14d, 2
    jne .calc_glyph_xy
    mov eax, r15d
    imul eax, CHAR_H
    add eax, TEXT_ORIGIN_Y + 2
    mov r9d, eax
    mov r8d, TEXT_ORIGIN_X - 12
    mov r10d, 3
    mov r11d, 20
    mov eax, [col_quote]
    call uefi_fill_rect

.calc_glyph_xy:
    mov eax, edi
    imul eax, CHAR_W
    add eax, TEXT_ORIGIN_X
    mov r8d, eax
    mov eax, r15d
    imul eax, CHAR_H
    add eax, TEXT_ORIGIN_Y
    mov r9d, eax

    mov r10d, [col_ink]
    xor r11d, r11d
    cmp r14d, 1
    jne .chk_q_col
    mov r10d, [col_accent]
    mov r11d, 1
    jmp .do_glyph
.chk_q_col:
    cmp r14d, 2
    jne .chk_focus_col
    mov r10d, [col_quote]
    mov r11d, 2
.chk_focus_col:
    cmp byte [focus_mode], 0
    je .do_glyph
    mov eax, [cursor_idx]
    sub eax, ebx
    cmp eax, -60
    jl .dim_it
    cmp eax, 60
    jg .dim_it
    jmp .do_glyph
.dim_it:
    mov r10d, [col_dim]
.do_glyph:
    mov al, r13b
    call uefi_draw_glyph

.adv_col:
    inc edi
    cmp edi, TEXT_COLS
    jb .text_loop
.next_line:
    inc edx
    xor edi, edi
    jmp .text_loop

.draw_cursor_check:
    cmp ebx, [cursor_idx]
    jne .chk_modal
    mov eax, edx
    sub eax, [scroll_line]
    js .chk_modal
    cmp eax, TEXT_ROWS
    jae .chk_modal
    imul eax, CHAR_H
    add eax, TEXT_ORIGIN_Y + 2
    mov r9d, eax
    mov eax, edi
    imul eax, CHAR_W
    add eax, TEXT_ORIGIN_X
    mov r8d, eax
    mov r10d, 3
    mov r11d, 20
    mov eax, [col_accent]
    call uefi_fill_rect

.chk_modal:
    cmp byte [modal_active], 1
    jne .draw_mouse
    mov r8d, 220
    mov r9d, 140
    mov r10d, 584
    mov r11d, 440
    mov eax, [col_bar]
    call uefi_fill_rect

    lea rsi, [ui_modal_title]
    mov r8d, 260
    mov r9d, 160
    mov r10d, [col_accent]
    mov r11d, 1
    call uefi_draw_str

    ; Desenha os 8 cartões
    xor ecx, ecx
.card_loop:
    push rcx
    mov eax, ecx
    shr eax, 1
    imul eax, 86
    add eax, 200
    mov r9d, eax
    mov eax, ecx
    and eax, 1
    imul eax, 270
    add eax, 250
    mov r8d, eax
    mov r10d, 244
    mov r11d, 72
    mov eax, [col_sheet]
    call uefi_fill_rect

    pop rcx
    push rcx
    lea rsi, [ui_slot_label]
    mov al, cl
    add al, '1'
    mov [rsi + 7], al
    add r8d, 16
    add r9d, 22
    mov r10d, [col_ink]
    mov r11d, 1
    call uefi_draw_str
    pop rcx
    inc ecx
    cmp ecx, 8
    jb .card_loop

.draw_mouse:
    ; Desenha ponteiro gráfico de seta (10x14)
    call uefi_draw_mouse_cursor

    ; Envia o Backbuffer (1024x768) para a tela via Gop->Blt (EfiBltBufferToVideo = 2)
    mov rcx, [gop_ptr]
    mov rdx, [backbuffer_ptr]
    mov r8d, 2                          ; EfiBltBufferToVideo
    xor r9d, r9d                        ; SourceX = 0
    mov qword [rsp + 0x20], 0           ; SourceY = 0
    mov eax, [screen_off_x]
    mov [rsp + 0x28], rax               ; DestinationX
    mov eax, [screen_off_y]
    mov [rsp + 0x30], rax               ; DestinationY
    mov qword [rsp + 0x38], SCREEN_W    ; Width = 1024
    mov qword [rsp + 0x40], SCREEN_H    ; Height = 768
    mov qword [rsp + 0x48], SCREEN_PITCH ; Delta = 4096
    call [rcx + 0x10]                   ; Gop->Blt
    test rax, rax
    jz .blt_ok

    ; Fallback direto via Linear Framebuffer (Gop->Mode->FrameBufferBase) se Gop->Blt falhar
    mov rdi, [gop_fb_base]
    test rdi, rdi
    jz .blt_ok
    mov rsi, [backbuffer_ptr]
    mov eax, [screen_off_y]
    imul eax, [native_ppsl]
    add eax, [screen_off_x]
    shl rax, 2
    add rdi, rax
    mov edx, SCREEN_H
    mov ebx, [native_ppsl]
    shl rbx, 2
.fb_row:
    push rdi
    mov ecx, SCREEN_W
    cld
    rep movsd
    pop rdi
    add rdi, rbx
    dec edx
    jnz .fb_row

.blt_ok:
    add rsp, 0x50
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    ret

; Desenha uma seta gráfica de mouse nítida em (mouse_x, mouse_y)
uefi_draw_mouse_cursor:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi

    lea rsi, [cursor_arrow_mask]
    xor edx, edx                        ; row 0..13
.cur_row:
    movzx ebx, word [rsi + rdx * 2]     ; 10 bits de máscara de seta
    mov ecx, 9
.cur_col:
    bt ebx, ecx
    jnc .cur_skip
    mov r8d, [mouse_x]
    mov eax, 9
    sub eax, ecx
    add r8d, eax
    mov r9d, [mouse_y]
    add r9d, edx
    mov r10d, 1
    mov r11d, 1
    mov eax, [col_accent]
    cmp edx, 0
    je .cur_draw
    cmp eax, ecx
    je .cur_draw
    mov eax, 0x00181818
.cur_draw:
    call uefi_fill_rect
.cur_skip:
    dec ecx
    jns .cur_col
    inc edx
    cmp edx, 14
    jb .cur_row

    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret

; =============================================================================
; Dados, GUIDs UEFI, Paletas e Fontes Gráficas Embutidas
; =============================================================================
align 8
efi_image_handle:   dq 0
efi_system_table:   dq 0
efi_boot_services:  dq 0
efi_con_in_handle:  dq 0
efi_con_in:         dq 0
gop_ptr:            dq 0
gop_fb_base:        dq 0
loaded_image_ptr:   dq 0
sfs_ptr:            dq 0
root_dir_handle:    dq 0
file_handle:        dq 0
backbuffer_ptr:     dq 0
doc_buffer_ptr:     dq 0
io_byte_size:       dq 0
status_msg_ptr:     dq 0

temp_ptr_iface:     dq 0
ptr_handle_count:   dq 0
ptr_handle_buf:     dq 0
pointer_list:       times MAX_POINTERS dq 0
pointer_count:      dd 0
                    dd 0
abs_ptr_iface:      dq 0
abs_state:          times 32 db 0

native_w:           dd 1024
native_h:           dd 768
native_ppsl:        dd 1024
screen_off_x:       dd 0
screen_off_y:       dd 0
blt_fill_pixel:     dd 0

max_frames_limit:   dd 0
frames_executed:    dd 0

mouse_x:            dd 512
mouse_y:            dd 384
mouse_prev_btn:     db 0
abs_prev_btn:       db 0
current_slot:       db 0
current_theme:      db 0
current_font:       db 0
focus_mode:         db 0
typewriter_mode:    db 0
hemingway_mode:     db 0
zen_mode:           db 0
modal_active:       db 0
pomo_running:       db 0
doc_dirty:          db 0
needs_redraw:       db 1
dead_key:           db 0

align 4
doc_len:            dd 0
cursor_idx:         dd 0
scroll_line:        dd 0
pomo_seconds:       dd 1500
tick_accum:         dd 0

col_desk:           dd 0
col_sheet:          dd 0
col_bar:            dd 0
col_ink:            dd 0
col_dim:            dd 0
col_accent:         dd 0
col_quote:          dd 0
col_shadow:         dd 0

align 8
ptr_state:          times 16 db 0
efi_key:            times 8 db 0
num_tmp_buf:        times 16 db 0

; Máscara bitmap 10x14 da Seta do Mouse
align 2
cursor_arrow_mask:
    dw 0b1000000000
    dw 0b1100000000
    dw 0b1110000000
    dw 0b1111000000
    dw 0b1111100000
    dw 0b1111110000
    dw 0b1111111000
    dw 0b1111111100
    dw 0b1111111110
    dw 0b1111110000
    dw 0b1101110000
    dw 0b1000111000
    dw 0b0000111000
    dw 0b0000011000

; GUIDs oficiais UEFI 2.x
align 8
guid_gop:
    dd 0x9042A9DE
    dw 0x23DC, 0x4A38
    db 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A

guid_simple_pointer:
    dd 0x31878C87
    dw 0x0B75, 0x11D5
    db 0x9A, 0x4F, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D

guid_abs_pointer:
    dd 0x8D59D32B
    dw 0xC655, 0x4AE9
    db 0x9B, 0x15, 0xF2, 0x59, 0x04, 0x99, 0x2A, 0x43

guid_loaded_image:
    dd 0x5B1B31A1
    dw 0x9562, 0x11D2
    db 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B

guid_sfs:
    dd 0x0964E5B22
    dw 0x6459, 0x11D2
    db 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B

; Nome do arquivo UTF-16LE (u"MANUSCR1.MD")
align 2
fname_manuscr:
    dw 'M','A','N','U','S','C','R','1','.','M','D', 0

; 4 Paletas TrueColor (32 bytes cada)
align 4
theme_table:
    ; 0: Papiro Quente
    dd 0x00D8D0C0, 0x00F6F1E3, 0x00EAE3D2, 0x0026221E, 0x009E9689, 0x00B84228, 0x003D5A6C, 0x00BEB6A6
    ; 1: Obsidiana
    dd 0x000D0E11, 0x0016181D, 0x00121317, 0x00DCDFE4, 0x00525660, 0x00E09F3E, 0x006CA0DC, 0x0007080A
    ; 2: E-Ink
    dd 0x00D0D3D6, 0x00F4F5F6, 0x00E2E4E6, 0x00111213, 0x00888C90, 0x001A5FB4, 0x002E6B4E, 0x00B4B7BA
    ; 3: Âmbar Noturno
    dd 0x00120D05, 0x001B1307, 0x00160F05, 0x00FFB000, 0x007A5400, 0x00FFD166, 0x00E07A5F, 0x00090602

ui_slot_label:      db 'MANUSCR1.MD', 0
ui_top_buttons:     db '[Salvar]  [Tema]  [Fonte]  [Foco]  [Maq]  [Hem]  [Pomodoro]  [Zen]', 0
ui_chars_lbl:       db 'carac.  |  Ctrl+S Salvar  |  Ctrl+T Tema  |  Ctrl+F Fonte  |  Ctrl+O Manuscritos', 0
ui_uefi_badge:      db 'UEFI x64 GOP + USB HID', 0
ui_modal_title:     db 'SELECIONE O MANUSCRITO (1..8 OU CLIQUE COM O MOUSE):', 0
msg_saved_ok:       db 'Salvo no Pen-Drive USB!', 0
msg_pomo_done:      db 'Pomodoro concluído!', 0

default_uefi_doc:
    db '# FOLIO-86 v3.0 : Edição Gráfica PT-BR', 10, 10
    db '> Rodando nativamente em 64-bit UEFI GOP no seu HP ProBook 640 G5!', 10, 10
    db 'Bem-vindo ao FOLIO-86 v3.0. Suporte nativo a acentuação e cedilha:', 10
    db 'á, é, í, ó, ú, â, ê, ô, ã, õ, à, ü, ç, Á, É, Í, Ó, Ú, Â, Ê, Ô, Ã, Õ, À, Ü, Ç.', 10, 10
    db 'Seus manuscritos são lidos e gravados em UTF-8 no Pen-Drive USB.', 10
default_uefi_doc_end:

align 4
%include "src/legacy/fonts.inc"

times (TEXT_RVA + TEXT_SIZE) - ($ - $$) db 0

; =============================================================================
; Seção .reloc (RVA 0x7000) — Bloco Dummy para o Loader PE32+ do UEFI (EDK2)
; =============================================================================
reloc_section:
    dd TEXT_RVA                         ; Page RVA
    dd 12                               ; Block Size (8 header + 4 bytes de entradas)
    dw 0                                ; IMAGE_REL_BASED_ABSOLUTE (no-op)
    dw 0                                ; IMAGE_REL_BASED_ABSOLUTE (no-op)

times IMAGE_TOTAL_SIZE - ($ - $$) db 0
