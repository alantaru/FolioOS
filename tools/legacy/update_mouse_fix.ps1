# tools/update_mouse_fix.ps1
# Corrige o despacho de cliques do mouse em tools/vm_uefi64.c e src/uefi_bootx64.asm

$enc = New-Object System.Text.UTF8Encoding($false)

# =============================================================================
# 1. ATUALIZA src/uefi_bootx64.asm
# =============================================================================
$uefi_path = "src/uefi_bootx64.asm"
$uefi_src = [System.IO.File]::ReadAllText($uefi_path, [System.Text.Encoding]::UTF8)

# 1.1 Adiciona needs_redraw = 1 no retorno de poll_abs
$old_poll_abs = @"
    mov eax, [abs_state + 0x18]         ; ActiveButtons
    and eax, 1
    movzx edx, byte [abs_prev_btn]
    mov [abs_prev_btn], al
    test eax, eax
    jz .done
    test edx, edx
    jnz .done
    call uefi_handle_mouse_click

.done:
"@
$new_poll_abs = @"
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
"@
$uefi_src = $uefi_src.Replace($old_poll_abs.Replace("`r`n", "`n").Replace("`n", "`r`n"), $new_poll_abs.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# 1.2 Corrige hitboxes dos botões e garante needs_redraw = 1 em uefi_handle_mouse_click
$old_click_handler = @"
uefi_handle_mouse_click:
    mov byte [dead_key], 0              ; Cancela dead key em cliques de mouse
    cmp byte [modal_active], 1
    jne .check_toolbar
"@
$new_click_handler = @"
uefi_handle_mouse_click:
    mov byte [needs_redraw], 1          ; Força redesenho visual imediato para todo clique!
    mov byte [dead_key], 0              ; Cancela dead key em cliques de mouse
    cmp byte [modal_active], 1
    jne .check_toolbar
"@
$uefi_src = $uefi_src.Replace($old_click_handler.Replace("`r`n", "`n").Replace("`n", "`r`n"), $new_click_handler.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# 1.3 Hitboxes precisas da barra de ferramentas
$old_toolbar_chk = @"
.check_toolbar:
    mov edx, [mouse_y]
    cmp edx, 34
    ja .check_sheet
    mov eax, [mouse_x]
    cmp eax, 155
    jb .open_modal
    cmp eax, 248
    jb .do_save
    cmp eax, 390
    jb .do_theme
    cmp eax, 530
    jb .do_font
    cmp eax, 670
    jb .do_focus
    cmp eax, 735
    jb .do_typewriter
    cmp eax, 798
    jb .do_hemingway
    cmp eax, 910
    jb .do_pomo
    xor byte [zen_mode], 1
    ret
"@
$new_toolbar_chk = @"
.check_toolbar:
    cmp byte [zen_mode], 1
    jne .check_tb_y
    ; No modo Ultra-Zen, clicar na barra superior (Y < 34) restaura a interface
    mov edx, [mouse_y]
    cmp edx, 34
    ja .check_sheet
    mov byte [zen_mode], 0
    ret

.check_tb_y:
    mov edx, [mouse_y]
    cmp edx, 34
    ja .check_sheet

    ; Hitboxes exatas calculadas sobre a largura CHAR_W = 10 px
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
"@
$uefi_src = $uefi_src.Replace($old_toolbar_chk.Replace("`r`n", "`n").Replace("`n", "`r`n"), $new_toolbar_chk.Replace("`r`n", "`n").Replace("`n", "`r`n"))

[System.IO.File]::WriteAllText($uefi_path, $uefi_src, $enc)
Write-Host "[OK] src/uefi_bootx64.asm atualizado com hitboxes e redraw imediato!" -ForegroundColor Green

# =============================================================================
# 2. ATUALIZA tools/vm_uefi64.c
# =============================================================================
$vm_path = "tools/vm_uefi64.c"
$vm_src = [System.IO.File]::ReadAllText($vm_path, [System.Text.Encoding]::UTF8)

# 2.1 Substitui filas estáticas lineares de 128 por Ring Buffers de 1024
$old_queues = @"
static SimMouseEvent g_mouse_queue[128];
static uint32_t g_mouse_q_head = 0;
static uint32_t g_mouse_q_tail = 0;

static SimAbsEvent g_abs_queue[128];
static uint32_t g_abs_q_head = 0;
static uint32_t g_abs_q_tail = 0;

static EFI_INPUT_KEY g_key_queue[512];
static uint32_t g_key_q_head = 0;
static uint32_t g_key_q_tail = 0;

static void push_mouse_event(int32_t dx, int32_t dy, int32_t dz, uint8_t left_btn) {
    if (g_mouse_q_tail < 128) {
        g_mouse_queue[g_mouse_q_tail].dx = dx;
        g_mouse_queue[g_mouse_q_tail].dy = dy;
        g_mouse_queue[g_mouse_q_tail].dz = dz;
        g_mouse_queue[g_mouse_q_tail].left_btn = left_btn;
        g_mouse_q_tail++;
    }
}

static void push_abs_event(uint64_t x, uint64_t y, uint32_t btn) {
    if (g_abs_q_tail < 128) {
        g_abs_queue[g_abs_q_tail].x = x;
        g_abs_queue[g_abs_q_tail].y = y;
        g_abs_queue[g_abs_q_tail].btn = btn;
        g_abs_q_tail++;
    }
}

static void push_key_event(uint16_t scan, uint16_t ch) {
    if (g_key_q_tail < 512) {
        g_key_queue[g_key_q_tail].ScanCode = scan;
        g_key_queue[g_key_q_tail].UnicodeChar = ch;
        g_key_q_tail++;
    }
}
"@
$new_queues = @"
#define EVENT_Q_SIZE 1024

static SimMouseEvent g_mouse_queue[EVENT_Q_SIZE];
static uint32_t g_mouse_q_head = 0;
static uint32_t g_mouse_q_tail = 0;

static SimAbsEvent g_abs_queue[EVENT_Q_SIZE];
static uint32_t g_abs_q_head = 0;
static uint32_t g_abs_q_tail = 0;

static EFI_INPUT_KEY g_key_queue[EVENT_Q_SIZE];
static uint32_t g_key_q_head = 0;
static uint32_t g_key_q_tail = 0;

static void push_mouse_event(int32_t dx, int32_t dy, int32_t dz, uint8_t left_btn) {
    uint32_t next = (g_mouse_q_tail + 1) % EVENT_Q_SIZE;
    if (next != g_mouse_q_head) {
        g_mouse_queue[g_mouse_q_tail].dx = dx;
        g_mouse_queue[g_mouse_q_tail].dy = dy;
        g_mouse_queue[g_mouse_q_tail].dz = dz;
        g_mouse_queue[g_mouse_q_tail].left_btn = left_btn;
        g_mouse_q_tail = next;
    }
}

static void push_abs_event(uint64_t x, uint64_t y, uint32_t btn) {
    uint32_t next = (g_abs_q_tail + 1) % EVENT_Q_SIZE;
    if (next != g_mouse_q_head) {
        g_abs_queue[g_abs_q_tail].x = x;
        g_abs_queue[g_abs_q_tail].y = y;
        g_abs_queue[g_abs_q_tail].btn = btn;
        g_abs_q_tail = next;
    }
}

static void push_key_event(uint16_t scan, uint16_t ch) {
    uint32_t next = (g_key_q_tail + 1) % EVENT_Q_SIZE;
    if (next != g_key_q_head) {
        g_key_queue[g_key_q_tail].ScanCode = scan;
        g_key_queue[g_key_q_tail].UnicodeChar = ch;
        g_key_q_tail = next;
    }
}
"@
$vm_src = $vm_src.Replace($old_queues.Replace("`r`n", "`n").Replace("`n", "`r`n"), $new_queues.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# 2.2 Atualiza UefiWndProc para Dual-Dispatch no clique
$old_wndproc_mouse = @"
        case WM_MOUSEMOVE: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            uint8_t btn = (wParam & MK_LBUTTON) ? 1 : 0;
            push_abs_event((uint64_t)x, (uint64_t)y, btn);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            push_abs_event((uint64_t)x, (uint64_t)y, 1);
            return 0;
        }
        case WM_LBUTTONUP: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            push_abs_event((uint64_t)x, (uint64_t)y, 0);
            return 0;
        }
"@
$new_wndproc_mouse = @"
        case WM_MOUSEMOVE: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            uint8_t btn = (wParam & MK_LBUTTON) ? 1 : 0;
            push_abs_event((uint64_t)x, (uint64_t)y, btn);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            push_abs_event((uint64_t)x, (uint64_t)y, 1);
            push_mouse_event(0, 0, 0, 1);
            return 0;
        }
        case WM_LBUTTONUP: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            push_abs_event((uint64_t)x, (uint64_t)y, 0);
            push_mouse_event(0, 0, 0, 0);
            return 0;
        }
"@
$vm_src = $vm_src.Replace($old_wndproc_mouse.Replace("`r`n", "`n").Replace("`n", "`r`n"), $new_wndproc_mouse.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# 2.3 Atualiza os consumidores de fila para Ring Buffer (% EVENT_Q_SIZE)
$old_active_ptr = @"
static EFI_STATUS __attribute__((ms_abi)) active_ptr_get_state(void *This, EFI_SIMPLE_POINTER_STATE *State) {
    (void)This;
    if (g_mouse_q_head >= g_mouse_q_tail) return EFI_NOT_READY;
    SimMouseEvent ev = g_mouse_queue[g_mouse_q_head++];
    State->RelativeMovementX = ev.dx;
    State->RelativeMovementY = ev.dy;
    State->RelativeMovementZ = ev.dz;
    State->LeftButton        = ev.left_btn;
    State->RightButton       = 0;
    return EFI_SUCCESS;
}
"@
$new_active_ptr = @"
static EFI_STATUS __attribute__((ms_abi)) active_ptr_get_state(void *This, EFI_SIMPLE_POINTER_STATE *State) {
    (void)This;
    if (g_mouse_q_head == g_mouse_q_tail) return EFI_NOT_READY;
    SimMouseEvent ev = g_mouse_queue[g_mouse_q_head];
    g_mouse_q_head = (g_mouse_q_head + 1) % EVENT_Q_SIZE;
    State->RelativeMovementX = ev.dx;
    State->RelativeMovementY = ev.dy;
    State->RelativeMovementZ = ev.dz;
    State->LeftButton        = ev.left_btn;
    State->RightButton       = 0;
    return EFI_SUCCESS;
}
"@
$vm_src = $vm_src.Replace($old_active_ptr.Replace("`r`n", "`n").Replace("`n", "`r`n"), $new_active_ptr.Replace("`r`n", "`n").Replace("`n", "`r`n"))

$old_abs_ptr = @"
static EFI_STATUS __attribute__((ms_abi)) abs_ptr_get_state(void *This, EFI_ABSOLUTE_POINTER_STATE *State) {
    (void)This;
    if (g_abs_q_head >= g_abs_q_tail) return EFI_NOT_READY;
    SimAbsEvent ev = g_abs_queue[g_abs_q_head++];
    State->CurrentX      = ev.x;
    State->CurrentY      = ev.y;
    State->CurrentZ      = 0;
    State->ActiveButtons = ev.btn;
    return EFI_SUCCESS;
}
"@
$new_abs_ptr = @"
static EFI_STATUS __attribute__((ms_abi)) abs_ptr_get_state(void *This, EFI_ABSOLUTE_POINTER_STATE *State) {
    (void)This;
    if (g_abs_q_head == g_abs_q_tail) return EFI_NOT_READY;
    SimAbsEvent ev = g_abs_queue[g_abs_q_head];
    g_abs_q_head = (g_abs_q_head + 1) % EVENT_Q_SIZE;
    State->CurrentX      = ev.x;
    State->CurrentY      = ev.y;
    State->CurrentZ      = 0;
    State->ActiveButtons = ev.btn;
    return EFI_SUCCESS;
}
"@
$vm_src = $vm_src.Replace($old_abs_ptr.Replace("`r`n", "`n").Replace("`n", "`r`n"), $new_abs_ptr.Replace("`r`n", "`n").Replace("`n", "`r`n"))

$old_con_in = @"
static EFI_STATUS __attribute__((ms_abi)) con_in_read_key(void *This, EFI_INPUT_KEY *Key) {
    (void)This;
    if (g_key_q_head >= g_key_q_tail) return EFI_NOT_READY;
    *Key = g_key_queue[g_key_q_head++];
    return EFI_SUCCESS;
}
"@
$new_con_in = @"
static EFI_STATUS __attribute__((ms_abi)) con_in_read_key(void *This, EFI_INPUT_KEY *Key) {
    (void)This;
    if (g_key_q_head == g_key_q_tail) return EFI_NOT_READY;
    *Key = g_key_queue[g_key_q_head];
    g_key_q_head = (g_key_q_head + 1) % EVENT_Q_SIZE;
    return EFI_SUCCESS;
}
"@
$vm_src = $vm_src.Replace($old_con_in.Replace("`r`n", "`n").Replace("`n", "`r`n"), $new_con_in.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# 2.4 Atualiza as checagens nos estágios da suíte de testes
$vm_src = $vm_src.Replace(
    "else if (g_sim_stage == 1 && g_mouse_q_head >= g_mouse_q_tail && g_abs_q_head >= g_abs_q_tail)",
    "else if (g_sim_stage == 1 && g_mouse_q_head == g_mouse_q_tail && g_abs_q_head == g_abs_q_tail)"
)
$vm_src = $vm_src.Replace(
    "else if (g_sim_stage == 2 && g_abs_q_head >= g_abs_q_tail)",
    "else if (g_sim_stage == 2 && g_abs_q_head == g_abs_q_tail)"
)

[System.IO.File]::WriteAllText($vm_path, $vm_src, $enc)
Write-Host "[OK] tools/vm_uefi64.c atualizado com Ring Buffers de 1024 e Dual Dispatch!" -ForegroundColor Green

