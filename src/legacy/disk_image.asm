; =============================================================================
; FOLIO-86 v3.0 — Pure MBR + Partition #1 FAT16 + UEFI x86_64 Disk Assembler
; Arquivo: src/disk_image.asm
; Estrutura de Setores (LBA):
;   - LBA 0       : Pure Master Boot Record (MBR) + Stage-1 Loader (Partição 1 = LBA 63)
;   - LBA 1..48   : Kernel Bare-Metal x86 (VESA VBE + Mouse USB UHCI HID + 3 Fontes)
;   - LBA 49      : Setor de Configuração Persistente (Magic 0x4638)
;   - LBA 50..62  : Padding pré-partição até CHS (0,1,1) = LBA 63 (13 setores)
;   - LBA 63      : Volume Boot Record (VBR) da Partição #1 (BPB_RsvdSecCnt = 1)
;   - LBA 64..79  : Tabela FAT16 (16 setores = 4.096 entradas FAT16)
;   - LBA 80..111 : Diretório Raiz FAT16 (32 setores = 512 entradas: MANUSCR1..8.MD + \EFI + Label)
;   - LBA 112..623: Área de Dados dos 8 Manuscritos (Clusters 2..129, 32 KB cada)
;   - LBA 624..627: Cluster 130 (\EFI)
;   - LBA 628..631: Cluster 131 (\EFI\BOOT)
;   - LBA 632..695: Clusters 132..147 (\EFI\BOOT\BOOTX64.EFI, 32 KB PE32+ x86_64)
;   - LBA 696..16471: Clusters livres até completar 4.090 clusters (>= 4.085 -> FAT16 Real)
; =============================================================================

BITS 16
ORG 0x0000

SECTOR_SIZE         EQU 512
TOTAL_SECTORS       EQU 16472       ; 8.433.664 bytes (~8,1 MB)
PART1_START_LBA     EQU 63          ; CHS (0, 1, 1) padrão MBR
PART1_SECTORS       EQU (TOTAL_SECTORS - PART1_START_LBA) ; 16.409 setores (4.090 clusters)
SECTORS_PER_SLOT    EQU 64          ; 32 KB por manuscrito
CLUSTERS_PER_SLOT   EQU 16          ; 16 clusters de 2 KB por manuscrito

; -----------------------------------------------------------------------------
; LBA 0: Pure Master Boot Record (512 bytes)
; -----------------------------------------------------------------------------
incbin "build/boot.bin"

; -----------------------------------------------------------------------------
; LBA 1..48: Kernel Bare-Metal FOLIO-86 (48 setores = 24.576 bytes)
; -----------------------------------------------------------------------------
incbin "build/kernel.bin"

; -----------------------------------------------------------------------------
; LBA 49: Setor de Configuração Persistente (512 bytes)
; -----------------------------------------------------------------------------
config_sector_start:
    dw 0x4638                       ; Assinatura '8F'
    db 0                            ; current_slot = 0 (MANUSCR1.MD)
    db 0                            ; current_theme = 0 (Papiro Quente)
    db 1                            ; goal_index = 1 (500 palavras)
    db 0                            ; sound_enabled = 0 (Silencioso)
    db 0                            ; current_font = 0 (Folio Serif)
times 512 - ($ - config_sector_start) db 0

; -----------------------------------------------------------------------------
; LBA 50..62: Setores de alinhamento até LBA 63 (13 setores)
; -----------------------------------------------------------------------------
times (13 * SECTOR_SIZE) db 0

; -----------------------------------------------------------------------------
; LBA 63: Volume Boot Record (VBR) da Partição #1 (CHS 0/1/1)
; Segue estritamente fatgen103.doc: BPB_RsvdSecCnt = 1 -> FAT16 em LBA 64!
; -----------------------------------------------------------------------------
vbr_sector_start:
    jmp short vbr_boot_code
    nop
    db 'FOLIO86 '                   ; OEM Name (8 bytes)
    dw 512                          ; Bytes por setor
    db 4                            ; Setores por cluster (2 KB)
    dw 1                            ; BPB_RsvdSecCnt = 1 (Apenas LBA 63 -> FAT em LBA 64)
    db 1                            ; Número de FATs = 1 (LBA 64..79)
    dw 512                          ; Entradas no Diretório Raiz (32 setores: LBA 80..111)
    dw PART1_SECTORS                ; Total de setores na Partição 1 (16.409 -> 4.090 clusters)
    db 0xF8                         ; Media Descriptor (Fixed/USB Disk)
    dw 16                           ; Setores por FAT = 16 (4.096 entradas FAT16)
    dw 63                           ; Setores por trilha
    dw 16                           ; Cabeças
    dd PART1_START_LBA              ; Hidden Sectors = 63 (Início da Partição 1)
    dd 0                            ; Total Sectors 32
    db 0x80                         ; Drive Number
    db 0x00                         ; Reserved
    db 0x29                         ; Extended Boot Signature
    dd 0x20261112                   ; Volume Serial Number
    db 'FOLIO86_USB'                ; Volume Label (11 bytes)
    db 'FAT16   '                   ; File System Type (8 bytes)

vbr_boot_code:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov si, (0x7C00 + (vbr_dap - vbr_sector_start))
    mov ah, 0x42
    int 0x13
    jmp 0x0800:0x0000

align 4
vbr_dap:
    db 0x10, 0x00
    dw 48
    dw 0x0000, 0x0800
    dq 1

times 510 - ($ - vbr_sector_start) db 0
dw 0xAA55

; -----------------------------------------------------------------------------
; LBA 64..79: Tabela de Alocação de Arquivos FAT16 (16 setores = 4.096 entradas)
; -----------------------------------------------------------------------------
fat1_start:
    dw 0xFFF8                       ; Cluster 0: Media Descriptor (0xF8)
    dw 0xFFFF                       ; Cluster 1: Reservado / EOC

    ; Clusters 2..129: 8 Manuscritos de 16 clusters (32 KB) cada
    %assign slot_idx 0
    %rep 8
        %assign base_clus (2 + slot_idx * CLUSTERS_PER_SLOT)
        %assign step 0
        %rep (CLUSTERS_PER_SLOT - 1)
            dw (base_clus + step + 1)
            %assign step (step + 1)
        %endrep
        dw 0xFFFF                   ; Fim da cadeia do manuscrito (EOC)
        %assign slot_idx (slot_idx + 1)
    %endrep

    ; Cluster 130: Subdiretório \EFI (1 cluster)
    dw 0xFFFF

    ; Cluster 131: Subdiretório \EFI\BOOT (1 cluster)
    dw 0xFFFF

    ; Clusters 132..147: Executável \EFI\BOOT\BOOTX64.EFI (16 clusters = 32 KB)
    %assign efi_step 0
    %rep 15
        dw (132 + efi_step + 1)
        %assign efi_step (efi_step + 1)
    %endrep
    dw 0xFFFF                       ; Fim do executável BOOTX64.EFI

times (16 * SECTOR_SIZE) - ($ - fat1_start) db 0

; -----------------------------------------------------------------------------
; LBA 80..111: Diretório Raiz FAT16 (512 entradas de 32 bytes = 32 setores)
; -----------------------------------------------------------------------------
%macro FAT16_ENTRY_RAW 4
    db %1                           ; 00..07: Nome (8 bytes)
    db %2                           ; 08..10: Extensão (3 bytes)
    db %3                           ; 11: Atributo (0x20=Arquivo, 0x10=Diretório, 0x08=VolumeID)
    db 0x00                         ; 12: Reservado NT
    db 0x00                         ; 13: Décimos de segundo
    dw ((21 << 11) | (30 << 5))     ; 14..15: Hora
    dw (((2026 - 1980) << 9) | (11 << 5) | 12) ; 16..17: Data
    dw (((2026 - 1980) << 9) | (11 << 5) | 12) ; 18..19: Último acesso
    dw 0x0000                       ; 20..21: Cluster alto
    dw ((21 << 11) | (30 << 5))     ; 22..23: Hora modificação
    dw (((2026 - 1980) << 9) | (11 << 5) | 12) ; 24..25: Data modificação
    dw %4                           ; 26..27: Primeiro Cluster (Low)
%endmacro

%macro FAT16_DIR_ENTRY 3
    FAT16_ENTRY_RAW %1, 'MD ', 0x20, %2
    dd %3                           ; 28..31: Tamanho em bytes
%endmacro

root_dir_start:
    FAT16_DIR_ENTRY 'MANUSCR1', (2 + 0 * 16), (slot1_text_end - slot1_text_start)
    FAT16_DIR_ENTRY 'MANUSCR2', (2 + 1 * 16), (slot2_text_end - slot2_text_start)
    FAT16_DIR_ENTRY 'MANUSCR3', (2 + 2 * 16), (slot3_text_end - slot3_text_start)
    FAT16_DIR_ENTRY 'MANUSCR4', (2 + 3 * 16), (slot4_text_end - slot4_text_start)
    FAT16_DIR_ENTRY 'MANUSCR5', (2 + 4 * 16), (slot5_text_end - slot5_text_start)
    FAT16_DIR_ENTRY 'MANUSCR6', (2 + 5 * 16), (slot6_text_end - slot6_text_start)
    FAT16_DIR_ENTRY 'MANUSCR7', (2 + 6 * 16), (slot7_text_end - slot7_text_start)
    FAT16_DIR_ENTRY 'MANUSCR8', (2 + 7 * 16), (slot8_text_end - slot8_text_start)
    ; Entrada 8: Diretório \EFI no Cluster 130 (Boot UEFI x86_64 p/ HP ProBook 640 G5)
    FAT16_ENTRY_RAW 'EFI     ', '   ', 0x10, 130
    dd 0
    ; Entrada 9: Rótulo de Volume (Volume ID = 0x08) para exibir FOLIO86_USB no Windows/Rufus
    FAT16_ENTRY_RAW 'FOLIO86_', 'USB', 0x08, 0
    dd 0

times (32 * SECTOR_SIZE) - ($ - root_dir_start) db 0

; -----------------------------------------------------------------------------
; LBA 112..623: Área de Dados Contígua dos 8 Manuscritos (64 setores = 32 KB cada)
; -----------------------------------------------------------------------------

; --- Slot 1 (LBA 112..175): MANUSCR1.MD ---
slot1_start:
slot1_text_start:
    db '# FOLIO-86 v2.1 : Edicao Grafica & Mouse USB', 10
    db 10
    db '> Tipografia serifada real, resolucao nativa VBE TrueColor e Mouse USB HID.', 10
    db 10
    db 'Bem-vindo ao FOLIO-86 v2.1. Agora voce escreve sobre uma folha de papel grafica '
    db 'em 32 bits de cor diretamente na resolucao do seu monitor, com suporte nativo '
    db 'a Mouse USB HID (incluindo Roda de Rolagem) e PS/2, sem sistema operacional.', 10
    db 10
    db 'Destaques da interface grafica e Mouse USB:', 10
    db 10
    db '- Clique com o Mouse USB sobre qualquer palavra para posicionar o cursor ou '
    db 'use a Roda de Rolagem (Scroll Wheel) para percorrer o manuscrito.', 10
    db '- Clique nos botoes da barra superior ([Tema], [Fonte], [Foco], [Salvar]).', 10
    db '- Pressione Ctrl+F (ou clique em [Fonte]) para alternar entre as 3 fontes graficas '
    db 'embutidas: Folio Serif (Literaria), Clean Sans (Moderna) e Courier Slab (Maquina).', 10
    db '- Titulos iniciados com "# " sao renderizados em Negrito com linha divisoria.', 10
    db '- Citacoes iniciadas com "> " sao renderizados em Italico real com barra lateral.', 10
slot1_text_end:
times (SECTORS_PER_SLOT * SECTOR_SIZE) - ($ - slot1_start) db 0

; --- Slot 2 (LBA 176..239): MANUSCR2.MD ---
slot2_start:
slot2_text_start:
    db '# Capitulo I : O Manuscrito', 10
    db 10
    db '> A simplicidade e o ultimo grau de sofisticacao.', 10
    db 10
    db 'Escreva seu primeiro paragrafo aqui. Quando voce conectar este pen-drive em '
    db 'qualquer computador comum, este arquivo estara disponivel diretamente na raiz '
    db 'como MANUSCR2.MD.', 10
slot2_text_end:
times (SECTORS_PER_SLOT * SECTOR_SIZE) - ($ - slot2_start) db 0

; --- Slots 3..8 ---
slot3_start:
slot3_text_start:
    db '# Manuscrito 3', 10, 10
slot3_text_end:
times (SECTORS_PER_SLOT * SECTOR_SIZE) - ($ - slot3_start) db 0

slot4_start:
slot4_text_start:
    db '# Manuscrito 4', 10, 10
slot4_text_end:
times (SECTORS_PER_SLOT * SECTOR_SIZE) - ($ - slot4_start) db 0

slot5_start:
slot5_text_start:
    db '# Manuscrito 5', 10, 10
slot5_text_end:
times (SECTORS_PER_SLOT * SECTOR_SIZE) - ($ - slot5_start) db 0

slot6_start:
slot6_text_start:
    db '# Manuscrito 6', 10, 10
slot6_text_end:
times (SECTORS_PER_SLOT * SECTOR_SIZE) - ($ - slot6_start) db 0

slot7_start:
slot7_text_start:
    db '# Manuscrito 7', 10, 10
slot7_text_end:
times (SECTORS_PER_SLOT * SECTOR_SIZE) - ($ - slot7_start) db 0

slot8_start:
slot8_text_start:
    db '# Manuscrito 8', 10, 10
slot8_text_end:
times (SECTORS_PER_SLOT * SECTOR_SIZE) - ($ - slot8_start) db 0

; -----------------------------------------------------------------------------
; Cluster 130 (LBA 624..627): Subdiretório \EFI (4 setores = 2.048 bytes)
; -----------------------------------------------------------------------------
efi_dir_cluster_start:
    FAT16_ENTRY_RAW '.       ', '   ', 0x10, 130
    dd 0
    FAT16_ENTRY_RAW '..      ', '   ', 0x10, 0
    dd 0
    FAT16_ENTRY_RAW 'BOOT    ', '   ', 0x10, 131
    dd 0
times (4 * SECTOR_SIZE) - ($ - efi_dir_cluster_start) db 0

; -----------------------------------------------------------------------------
; Cluster 131 (LBA 628..631): Subdiretório \EFI\BOOT (4 setores = 2.048 bytes)
; -----------------------------------------------------------------------------
efi_boot_dir_cluster_start:
    FAT16_ENTRY_RAW '.       ', '   ', 0x10, 131
    dd 0
    FAT16_ENTRY_RAW '..      ', '   ', 0x10, 130
    dd 0
    FAT16_ENTRY_RAW 'BOOTX64 ', 'EFI', 0x20, 132
    dd 32768
times (4 * SECTOR_SIZE) - ($ - efi_boot_dir_cluster_start) db 0

; -----------------------------------------------------------------------------
; Clusters 132..147 (LBA 632..695): \EFI\BOOT\BOOTX64.EFI (64 setores = 32 KB)
; -----------------------------------------------------------------------------
incbin "build/bootx64.efi"

; -----------------------------------------------------------------------------
; Restante da Área de Dados FAT16 até completar 16.472 setores (4.090 clusters)
; -----------------------------------------------------------------------------
times (TOTAL_SECTORS * SECTOR_SIZE) - ($ - $$) db 0
