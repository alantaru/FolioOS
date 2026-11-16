# tools/update_kernel_ptbr.ps1
# Atualiza src/kernel.asm para suportar integralmente PT-BR, Dead Keys, Cedilha e UTF-8

$path = "src/kernel.asm"
$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)

# 1. Adiciona dead_key no bloco de dados
$target_data = "is_dirty:               db 0"
$replacement_data = "is_dirty:               db 0`r`ndead_key:               db 0"
$content = $content.Replace($target_data, $replacement_data)

# 2. handle_mouse_click: zera dead_key
$target_click = "handle_mouse_click:`r`n    mov word [toast_ptr], 0"
$replacement_click = "handle_mouse_click:`r`n    mov word [toast_ptr], 0`r`n    mov byte [dead_key], 0"
$content = $content.Replace($target_click, $replacement_click)

# 3. draw_glyph_gfx: aceita codepoints 32..255
$target_glyph = "draw_glyph_gfx:`r`n    pusha`r`n    cmp al, 32`r`n    jb .glyph_done`r`n    cmp al, 126`r`n    ja .glyph_done"
$replacement_glyph = "draw_glyph_gfx:`r`n    pusha`r`n    cmp al, 32`r`n    jb .glyph_done"
$content = $content.Replace($target_glyph, $replacement_glyph)

# 4. draw_str_gfx: decodifica sequencias UTF-8 de 2 bytes
$target_str = @"
draw_str_gfx:
    lodsb
    test al, al
    jz .done
    call draw_glyph_gfx
    add word [draw_x], CHAR_W
    jmp draw_str_gfx
.done:
    ret
"@
$replacement_str = @"
draw_str_gfx:
    lodsb
    test al, al
    jz .done
    cmp al, 0xC2
    jb .str_single
    cmp al, 0xC3
    ja .str_single
    mov ah, al
    lodsb
    test al, al
    jz .done
    and ah, 0x1F
    shl ah, 6
    and al, 0x3F
    or al, ah
.str_single:
    call draw_glyph_gfx
    add word [draw_x], CHAR_W
    jmp draw_str_gfx
.done:
    ret
"@
$content = $content.Replace($target_str.Replace("`r`n", "`n").Replace("`n", "`r`n"), $replacement_str.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# 5. render_visual_line: decodifica sequencias UTF-8 de 2 bytes ao ler do DOC_SEG
$target_render_char = @"
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
"@
$replacement_render_char = @"
.char_loop:
    push es
    mov ax, DOC_SEG
    mov es, ax
    mov al, [es:bx]
    pop es
    cmp al, 0x0D
    je .skip_cr
    cmp al, 0x0A
    je .skip_cr
    cmp al, 0xC2
    jb .char_single
    cmp al, 0xC3
    ja .char_single
    cmp cx, 1
    jbe .char_single
    push es
    mov dx, DOC_SEG
    mov es, dx
    mov dl, [es:bx + 1]
    pop es
    inc bx
    dec cx
    and al, 0x1F
    shl al, 6
    and dl, 0x3F
    or al, dl
.char_single:
    call draw_glyph_gfx
.skip_cr:
    add word [draw_x], CHAR_W
    inc bx
    loop .char_loop
"@
$content = $content.Replace($target_render_char.Replace("`r`n", "`n").Replace("`n", "`r`n"), $replacement_render_char.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# 6. layout_document: no scan_char, nao incrementa cx para bytes de continuacao UTF-8 (0x80..0xBF)
$target_scan_char = @"
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
"@
$replacement_scan_char = @"
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

    ; Nao incrementa coluna visual cx se for byte de continuacao UTF-8 (0x80..0xBF)
    mov ah, al
    and ah, 0xC0
    cmp ah, 0x80
    je .utf8_cont_scan

    cmp al, ' '
    jne .not_space
    mov [last_space_tmp], si
.not_space:
    inc cx
.utf8_cont_scan:
    inc si
    cmp cx, WRAP_WIDTH
    jb .scan_char
"@
$content = $content.Replace($target_scan_char.Replace("`r`n", "`n").Replace("`n", "`r`n"), $replacement_scan_char.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# 7. check_cursor_in_wrapped_line e check_cursor_in_newline_line: calcula coluna visual contando codepoints
$target_check_cols = @"
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
"@
$replacement_check_cols = @"
calc_cursor_visual_col:
    push si
    push es
    mov ax, DOC_SEG
    mov es, ax
    mov si, [line_start_tmp]
    xor di, di
.col_calc_loop:
    cmp si, [cursor_pos]
    jae .col_calc_done
    mov al, [es:si]
    inc si
    and al, 0xC0
    cmp al, 0x80
    je .col_calc_loop
    inc di
    jmp .col_calc_loop
.col_calc_done:
    pop es
    pop si
    mov ax, di
    cmp ax, WRAP_WIDTH - 1
    jbe .col_clamped_ret
    mov ax, WRAP_WIDTH - 1
.col_clamped_ret:
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
    call calc_cursor_visual_col
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
    call calc_cursor_visual_col
    mov [cursor_col], ax
    mov [cursor_para], dx
.not_here2:
    ret
"@
$content = $content.Replace($target_check_cols.Replace("`r`n", "`n").Replace("`n", "`r`n"), $replacement_check_cols.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# 8. move_cursor_to_line: avanca em colunas visuais considerando UTF-8
$target_move_cursor = @"
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
"@
$replacement_move_cursor = @"
move_cursor_to_line:
    call get_line_entry
    push es
    mov di, DOC_SEG
    mov es, di
    mov si, ax
    mov cx, [cursor_col]
.step_visual_cols:
    test cx, cx
    jz .target_pos_found
    cmp si, dx
    jae .target_pos_found
    mov bl, [es:si]
    inc si
    and bl, 0xC0
    cmp bl, 0x80
    je .step_visual_cols
    dec cx
    jmp .step_visual_cols
.target_pos_found:
    pop es
    mov [cursor_pos], si
    ret
"@
$content = $content.Replace($target_move_cursor.Replace("`r`n", "`n").Replace("`n", "`r`n"), $replacement_move_cursor.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# 9. .nav_left e .nav_right: navegam saltando caracteres UTF-8 de 2 bytes
$target_nav = @"
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
"@
$replacement_nav = @"
.nav_left:
    cmp word [cursor_pos], 0
    je .done
    dec word [cursor_pos]
    mov bx, [cursor_pos]
    push es
    mov ax, DOC_SEG
    mov es, ax
    mov al, [es:bx]
    pop es
    and al, 0xC0
    cmp al, 0x80
    jne .left_ok
    cmp word [cursor_pos], 0
    je .left_ok
    dec word [cursor_pos]
.left_ok:
    ret

.nav_right:
    mov ax, [cursor_pos]
    cmp ax, [doc_len]
    jae .done
    inc word [cursor_pos]
    mov bx, ax
    push es
    mov ax, DOC_SEG
    mov es, ax
    mov al, [es:bx]
    pop es
    and al, 0xE0
    cmp al, 0xC0
    jne .right_ok
    mov ax, [cursor_pos]
    cmp ax, [doc_len]
    jae .right_ok
    inc word [cursor_pos]
.right_ok:
    ret
"@
$content = $content.Replace($target_nav.Replace("`r`n", "`n").Replace("`n", "`r`n"), $replacement_nav.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# 10. process_key: Maquina de Dead Keys PT-BR ('c -> ç, ~a -> ã, 'o -> ó), Latin-1 direto (0xA0..0xFF) e UTF-8
$target_process_key = @"
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
"@
$replacement_process_key = @"
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

    ; Teclas diretas Latin-1 (0xA0..0xFF) de teclados ABNT2 (ex: ç = 0xE7, Ç = 0xC7)
    cmp al, 0xA0
    jae .direct_latin1

    cmp al, 0x20
    jb .done
    cmp al, 0x7F
    jae .done

    ; Maquina de estados Dead Key PT-BR
    movzx bx, byte [dead_key]
    test bx, bx
    jnz .process_dead_key

    ; Verifica se esta tecla inicia uma dead key:
    cmp al, 0x27                    ; ' (Agudo / Cedilha)
    je .set_dead_key
    cmp al, 0x7E                    ; ~ (Til)
    je .set_dead_key
    cmp al, 0x5E                    ; ^ (Circunflexo)
    je .set_dead_key
    cmp al, 0x60                    ; ` (Crase)
    je .set_dead_key
    cmp al, 0x22                    ; " (Trema)
    je .set_dead_key

    ; ASCII comum:
    call insert_char_utf8
    call click_sound
    ret

.set_dead_key:
    mov [dead_key], al
    ret

.direct_latin1:
    mov byte [dead_key], 0
    call insert_char_utf8
    call click_sound
    ret

.process_dead_key:
    mov byte [dead_key], 0
    mov dl, bl                      ; dl = dead key anterior
    mov dh, al                      ; dh = tecla atual

    cmp dh, 0x20                    ; Espaco apos dead key -> emite simbolo da dead key
    jne .chk_same_dk
    mov al, dl
    call insert_char_utf8
    call click_sound
    ret

.chk_same_dk:
    cmp dh, dl                      ; Mesma dead key repetida ('' ou ~~) -> emite ambas
    jne .chk_dk_acute
    mov al, dl
    call insert_char_utf8
    mov al, dl
    call insert_char_utf8
    call click_sound
    ret

.chk_dk_acute:
    cmp dl, 0x27                    ; ' (Agudo / Cedilha)
    jne .chk_dk_tilde
    cmp dh, 'c'
    jne .dk_c_up
    mov al, 0xE7                    ; ç
    jmp .emit_dk_char
.dk_c_up:
    cmp dh, 'C'
    jne .dk_a_ac
    mov al, 0xC7                    ; Ç
    jmp .emit_dk_char
.dk_a_ac:
    cmp dh, 'a'
    jne .dk_e_ac
    mov al, 0xE1                    ; á
    jmp .emit_dk_char
.dk_e_ac:
    cmp dh, 'e'
    jne .dk_i_ac
    mov al, 0xE9                    ; é
    jmp .emit_dk_char
.dk_i_ac:
    cmp dh, 'i'
    jne .dk_o_ac
    mov al, 0xED                    ; í
    jmp .emit_dk_char
.dk_o_ac:
    cmp dh, 'o'
    jne .dk_u_ac
    mov al, 0xF3                    ; ó
    jmp .emit_dk_char
.dk_u_ac:
    cmp dh, 'u'
    jne .dk_A_ac
    mov al, 0xFA                    ; ú
    jmp .emit_dk_char
.dk_A_ac:
    cmp dh, 'A'
    jne .dk_E_ac
    mov al, 0xC1                    ; Á
    jmp .emit_dk_char
.dk_E_ac:
    cmp dh, 'E'
    jne .dk_I_ac
    mov al, 0xC9                    ; É
    jmp .emit_dk_char
.dk_I_ac:
    cmp dh, 'I'
    jne .dk_O_ac
    mov al, 0xCD                    ; Í
    jmp .emit_dk_char
.dk_O_ac:
    cmp dh, 'O'
    jne .dk_U_ac
    mov al, 0xD3                    ; Ó
    jmp .emit_dk_char
.dk_U_ac:
    cmp dh, 'U'
    jne .dk_fallback
    mov al, 0xDA                    ; Ú
    jmp .emit_dk_char

.chk_dk_tilde:
    cmp dl, 0x7E                    ; ~ (Til)
    jne .chk_dk_circ
    cmp dh, 'a'
    jne .dk_o_ti
    mov al, 0xE3                    ; ã
    jmp .emit_dk_char
.dk_o_ti:
    cmp dh, 'o'
    jne .dk_A_ti
    mov al, 0xF5                    ; õ
    jmp .emit_dk_char
.dk_A_ti:
    cmp dh, 'A'
    jne .dk_O_ti
    mov al, 0xC3                    ; Ã
    jmp .emit_dk_char
.dk_O_ti:
    cmp dh, 'O'
    jne .dk_fallback
    mov al, 0xD5                    ; Õ
    jmp .emit_dk_char

.chk_dk_circ:
    cmp dl, 0x5E                    ; ^ (Circunflexo)
    jne .chk_dk_grave
    cmp dh, 'a'
    jne .dk_e_ci
    mov al, 0xE2                    ; â
    jmp .emit_dk_char
.dk_e_ci:
    cmp dh, 'e'
    jne .dk_i_ci
    mov al, 0xEA                    ; ê
    jmp .emit_dk_char
.dk_i_ci:
    cmp dh, 'i'
    jne .dk_o_ci
    mov al, 0xEE                    ; î
    jmp .emit_dk_char
.dk_o_ci:
    cmp dh, 'o'
    jne .dk_u_ci
    mov al, 0xF4                    ; ô
    jmp .emit_dk_char
.dk_u_ci:
    cmp dh, 'u'
    jne .dk_A_ci
    mov al, 0xFB                    ; û
    jmp .emit_dk_char
.dk_A_ci:
    cmp dh, 'A'
    jne .dk_E_ci
    mov al, 0xC2                    ; Â
    jmp .emit_dk_char
.dk_E_ci:
    cmp dh, 'E'
    jne .dk_I_ci
    mov al, 0xCA                    ; Ê
    jmp .emit_dk_char
.dk_I_ci:
    cmp dh, 'I'
    jne .dk_O_ci
    mov al, 0xCE                    ; Î
    jmp .emit_dk_char
.dk_O_ci:
    cmp dh, 'O'
    jne .dk_U_ci
    mov al, 0xD4                    ; Ô
    jmp .emit_dk_char
.dk_U_ci:
    cmp dh, 'U'
    jne .dk_fallback
    mov al, 0xDB                    ; Û
    jmp .emit_dk_char

.chk_dk_grave:
    cmp dl, 0x60                    ; ` (Crase)
    jne .chk_dk_trema
    cmp dh, 'a'
    jne .dk_A_gr
    mov al, 0xE0                    ; à
    jmp .emit_dk_char
.dk_A_gr:
    cmp dh, 'A'
    jne .dk_fallback
    mov al, 0xC0                    ; À
    jmp .emit_dk_char

.chk_dk_trema:
    cmp dl, 0x22                    ; " (Trema)
    jne .dk_fallback
    cmp dh, 'u'
    jne .dk_U_tr
    mov al, 0xFC                    ; ü
    jmp .emit_dk_char
.dk_U_tr:
    cmp dh, 'U'
    jne .dk_fallback
    mov al, 0xDC                    ; Ü
    jmp .emit_dk_char

.dk_fallback:
    mov al, dl
    call insert_char_utf8
    mov al, dh
    call insert_char_utf8
    call click_sound
    ret

.emit_dk_char:
    call insert_char_utf8
    call click_sound
    ret

; Insere caractere AL (0x20..0xFF) em DOC_SEG codificando em UTF-8 se >= 0x80
insert_char_utf8:
    cmp al, 0x80
    jae .utf8_2bytes
    call insert_char
    ret
.utf8_2bytes:
    push ax
    mov ah, al
    shr ah, 6
    or ah, 0xC0
    mov al, ah
    call insert_char
    pop ax
    and al, 0x3F
    or al, 0x80
    call insert_char
    ret
"@
$content = $content.Replace($target_process_key.Replace("`r`n", "`n").Replace("`n", "`r`n"), $replacement_process_key.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# 11. key_backspace e key_delete: apagam caracteres UTF-8 de 2 bytes atomicamente
$target_del = @"
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
"@
$replacement_del = @"
.key_backspace:
    cmp byte [dead_key], 0
    je .bs_exec
    mov byte [dead_key], 0
    ret
.bs_exec:
    cmp byte [hemingway_mode], 0
    jne .hemingway_blocked
    mov bx, [cursor_pos]
    test bx, bx
    jz .done
    push es
    mov ax, DOC_SEG
    mov es, ax
    mov al, [es:bx - 1]
    pop es
    and al, 0xC0
    cmp al, 0x80
    jne .bs_single
    cmp bx, 1
    jbe .bs_single
    dec word [cursor_pos]
    call delete_at_cursor
.bs_single:
    dec word [cursor_pos]
    call delete_at_cursor
    call click_sound
    ret

.key_delete:
    mov byte [dead_key], 0
    cmp byte [hemingway_mode], 0
    jne .hemingway_blocked
    mov bx, [cursor_pos]
    cmp bx, [doc_len]
    jae .done
    push es
    mov ax, DOC_SEG
    mov es, ax
    mov al, [es:bx]
    pop es
    and al, 0xE0
    cmp al, 0xC0
    jne .del_single
    call delete_at_cursor
.del_single:
    call delete_at_cursor
    call click_sound
    ret
"@
$content = $content.Replace($target_del.Replace("`r`n", "`n").Replace("`n", "`r`n"), $replacement_del.Replace("`r`n", "`n").Replace("`n", "`r`n"))

# 12. .extended_key, .key_enter, .key_tab, .key_esc, .cmd_save, .cmd_new_doc, .cmd_font: zera dead_key
$content = $content.Replace(".extended_key:`r`n    cmp ah, 0x3B", ".extended_key:`r`n    mov byte [dead_key], 0`r`n    cmp ah, 0x3B")
$content = $content.Replace(".key_enter:`r`n    mov al, 0x0A", ".key_enter:`r`n    mov byte [dead_key], 0`r`n    mov al, 0x0A")
$content = $content.Replace(".key_tab:`r`n    mov cx, 4", ".key_tab:`r`n    mov byte [dead_key], 0`r`n    mov cx, 4")
$content = $content.Replace(".key_esc:`r`n    cmp byte [zen_mode], 0", ".key_esc:`r`n    mov byte [dead_key], 0`r`n    cmp byte [zen_mode], 0")
$content = $content.Replace(".cmd_save:`r`n    call disk_save_slot", ".cmd_save:`r`n    mov byte [dead_key], 0`r`n    call disk_save_slot")
$content = $content.Replace(".cmd_new_doc:`r`n    cmp byte [is_dirty], 0", ".cmd_new_doc:`r`n    mov byte [dead_key], 0`r`n    cmp byte [is_dirty], 0")
$content = $content.Replace(".cmd_font:`r`n    inc byte [current_font]", ".cmd_font:`r`n    mov byte [dead_key], 0`r`n    inc byte [current_font]")

[System.IO.File]::WriteAllText($path, $content, [System.Text.Encoding]::UTF8)
Write-Host "[OK] src/kernel.asm atualizado com suporte integral a PT-BR!" -ForegroundColor Green

