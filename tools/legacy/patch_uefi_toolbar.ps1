# tools/patch_uefi_toolbar.ps1
$enc = New-Object System.Text.UTF8Encoding($false)
$path = "src/uefi_bootx64.asm"

$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
$content = $content.Replace("`r`n", "`n")

# 1. Entrada de uefi_handle_mouse_click: garante needs_redraw = 1
$target_entry = @'
uefi_handle_mouse_click:
    mov byte [dead_key], 0              ; Cancela dead key em cliques de mouse
    cmp byte [modal_active], 1
'@.Replace("`r`n", "`n")

$new_entry = @'
uefi_handle_mouse_click:
    mov byte [needs_redraw], 1          ; Forca redesenho visual imediato para todo clique!
    mov byte [dead_key], 0              ; Cancela dead key em cliques de mouse
    cmp byte [modal_active], 1
'@.Replace("`r`n", "`n")

# 2. Hitboxes exatas da barra de ferramentas
$target_tb = @'
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
'@.Replace("`r`n", "`n")

$new_tb = @'
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
'@.Replace("`r`n", "`n")

# 3. .poll_abs: adiciona needs_redraw = 1
$target_poll_abs = @'
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
'@.Replace("`r`n", "`n")

$new_poll_abs = @'
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
'@.Replace("`r`n", "`n")

$content = $content.Replace($target_entry, $new_entry)
$content = $content.Replace($target_tb, $new_tb)
$content = $content.Replace($target_poll_abs, $new_poll_abs)

[System.IO.File]::WriteAllText($path, $content, $enc)
Write-Host "[OK] uefi_bootx64.asm hitboxes atualizadas com sucesso!" -ForegroundColor Green

