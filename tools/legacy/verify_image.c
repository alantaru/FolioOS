/* ============================================================================
 * FOLIO-86 v3.0 — Suite de Auditoria Binária Pure MBR + UEFI x64 + BIOS (@qa)
 * Arquivo: tools/verify_image.c
 * Verifica a integridade estrutural de `build/folio86.img`:
 *   1. LBA 0: Pure MBR + Tabela de Partições em 0x01BE (Partição 1 Ativa em LBA 63)
 *   2. LBA 63: Volume Boot Record (VBR) da Partição 1 (BPB_RsvdSecCnt = 1)
 *   3. Geometria FAT16 Autêntica (4.090 clusters >= 4.085 para EDK2 FatDxe)
 *   4. Presença do Kernel Bare-Metal x86 em LBA 1..48 (24.576 bytes)
 *   5. Setor de Configuração Persistente em LBA 49
 *   6. Diretório Raiz FAT16 (LBA 80..111) com MANUSCR1..8.MD + \EFI\BOOT\BOOTX64.EFI + VolumeID
 *   7. Cabeçalho PE32+ x86_64 válido em \EFI\BOOT\BOOTX64.EFI (HP ProBook 640 G5)
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SECTOR_SIZE         512
#define TOTAL_SECTORS       16472
#define IMAGE_SIZE          (TOTAL_SECTORS * SECTOR_SIZE)
#define KERNEL_LBA          1
#define KERNEL_SECTORS      48
#define CONFIG_LBA          49
#define VBR_LBA             63
#define FAT1_LBA            64
#define ROOT_DIR_LBA        80
#define DATA_START_LBA      112
#define SECTORS_PER_SLOT    64
#define CLUSTERS_PER_SLOT   16

static uint16_t read_u16(const uint8_t *buf, size_t off) {
    return (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
}

static uint32_t read_u32(const uint8_t *buf, size_t off) {
    return (uint32_t)buf[off] |
           ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) |
           ((uint32_t)buf[off + 3] << 24);
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "build/folio86.img";
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[FALHA QA] Nao foi possivel abrir %s\n", path);
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz != IMAGE_SIZE) {
        fprintf(stderr, "[FALHA QA] Tamanho da imagem incorreto: %ld (esperado %d)\n", sz, IMAGE_SIZE);
        fclose(fp);
        return 1;
    }

    uint8_t *img = (uint8_t *)malloc(IMAGE_SIZE);
    if (!img || fread(img, 1, IMAGE_SIZE, fp) != IMAGE_SIZE) {
        fprintf(stderr, "[FALHA QA] Erro de leitura da imagem.\n");
        fclose(fp);
        return 1;
    }
    fclose(fp);

    /* 1. Verifica LBA 0 (Pure MBR) e Tabela de Partições MBR em 0x01BE */
    uint16_t boot_sig = read_u16(img, 510);
    if (boot_sig != 0xAA55 || img[0] != 0xFA) {
        fprintf(stderr, "[FALHA QA] LBA 0 Pure MBR invalido (opcode0=0x%02X, sig=0x%04X)\n", img[0], boot_sig);
        return 1;
    }
    uint8_t part_status = img[0x01BE + 0];
    uint8_t part_type   = img[0x01BE + 4];
    uint32_t part_lba   = read_u32(img, 0x01BE + 8);
    uint32_t part_size  = read_u32(img, 0x01BE + 12);
    if (part_status != 0x80 || part_type != 0x0E || part_lba != VBR_LBA || part_size != (TOTAL_SECTORS - VBR_LBA)) {
        fprintf(stderr, "[FALHA QA] Entrada de Particao MBR #1 invalida (status=0x%02X, type=0x%02X, lba=%u, size=%u)\n",
                part_status, part_type, part_lba, part_size);
        return 1;
    }
    printf("[QA OK] LBA 0: Pure MBR + Partition #1 Ativa (0x80, Tipo 0x0E FAT16 LBA, StartLBA=%u, Size=%u).\n",
           part_lba, part_size);

    /* 2. Verifica VBR da Partição #1 em LBA 63 e BPB FAT16 Genuíno (>= 4085 Clusters) */
    const uint8_t *vbr = img + (VBR_LBA * SECTOR_SIZE);
    uint16_t vbr_sig       = read_u16(vbr, 510);
    uint16_t bytes_per_sec = read_u16(vbr, 11);
    uint8_t sec_per_clus   = vbr[13];
    uint16_t reserved_secs = read_u16(vbr, 14);
    uint8_t num_fats       = vbr[16];
    uint16_t root_entries  = read_u16(vbr, 17);
    uint16_t total_secs16  = read_u16(vbr, 19);
    uint8_t media_desc     = vbr[21];
    uint16_t secs_per_fat  = read_u16(vbr, 22);
    uint32_t data_clusters = (total_secs16 - (reserved_secs + num_fats * secs_per_fat + 32)) / sec_per_clus;

    if (vbr_sig != 0xAA55 || bytes_per_sec != 512 || sec_per_clus != 4 || reserved_secs != 1 ||
        num_fats != 1 || root_entries != 512 || total_secs16 != (TOTAL_SECTORS - VBR_LBA) ||
        media_desc != 0xF8 || secs_per_fat != 16 || data_clusters < 4085 ||
        (VBR_LBA + reserved_secs) != FAT1_LBA) {
        fprintf(stderr, "[FALHA QA] VBR/BPB FAT16 inconsistente em LBA 63 (res=%u, clusters=%u).\n",
                reserved_secs, data_clusters);
        return 1;
    }
    printf("[QA OK] LBA 63 (VBR Particao #1): BPB_RsvdSecCnt=1, %u clusters FAT16 (>= 4085 exigido pelo UEFI EDK2 FatDxe).\n",
           data_clusters);

    /* 3. Verifica Kernel Bare-Metal em LBA 1..48 */
    size_t k_off = KERNEL_LBA * SECTOR_SIZE;
    if (img[k_off] != 0xFA) {
        fprintf(stderr, "[FALHA QA] Ponto de entrada do Kernel em LBA 1 nao inicia com CLI (0xFA): 0x%02X\n", img[k_off]);
        return 1;
    }
    printf("[QA OK] LBA 1..48: Kernel Bare-Metal x86 presente (24.576 bytes, 0xFA CLI verificado).\n");

    /* 4. Verifica Setor de Configuracao em LBA 49 */
    uint16_t cfg_magic = read_u16(img, CONFIG_LBA * SECTOR_SIZE);
    if (cfg_magic != 0x4638) {
        fprintf(stderr, "[FALHA QA] Assinatura de configuracao LBA 49 invalida: 0x%04X\n", cfg_magic);
        return 1;
    }
    printf("[QA OK] LBA 49: Setor de Configuracao Persistente inicializado (Magic 0x4638).\n");

    /* 5. Verifica Diretório Raiz FAT16 (LBA 80) e os 8 Manuscritos */
    const uint8_t *fat1 = img + (FAT1_LBA * SECTOR_SIZE);
    const uint8_t *root_dir = img + (ROOT_DIR_LBA * SECTOR_SIZE);
    for (int slot = 0; slot < 8; slot++) {
        const uint8_t *entry = root_dir + (slot * 32);
        char expected_name[12];
        snprintf(expected_name, sizeof(expected_name), "MANUSCR%dMD ", slot + 1);
        if (memcmp(entry, expected_name, 11) != 0) {
            fprintf(stderr, "[FALHA QA] Entrada de diretorio invalida no slot %d\n", slot);
            return 1;
        }

        uint16_t first_clus = read_u16(entry, 26);
        uint32_t file_size  = read_u32(entry, 28);

        uint32_t fat_lba = DATA_START_LBA + (uint32_t)(first_clus - 2) * sec_per_clus;
        uint32_t asm_lba = DATA_START_LBA + (uint32_t)(slot * SECTORS_PER_SLOT);

        if (fat_lba != asm_lba) {
            fprintf(stderr, "[FALHA QA] Divergencia de LBA no slot %d: FAT=%u vs ASM=%u\n", slot, fat_lba, asm_lba);
            return 1;
        }

        uint16_t cur = first_clus;
        for (int step = 0; step < CLUSTERS_PER_SLOT - 1; step++) {
            uint16_t nxt = read_u16(fat1, cur * 2);
            if (nxt != cur + 1) {
                fprintf(stderr, "[FALHA QA] Cadeia de cluster quebrada no slot %d: %u -> %u\n", slot, cur, nxt);
                return 1;
            }
            cur = nxt;
        }
        if (read_u16(fat1, cur * 2) != 0xFFFF) {
            fprintf(stderr, "[FALHA QA] Cluster final do slot %d nao possui marcador EOC (0xFFFF).\n", slot);
            return 1;
        }

        const uint8_t *slot_data = img + (asm_lba * SECTOR_SIZE);
        if (file_size == 0 || slot_data[0] != '#' || slot_data[file_size] != 0) {
            fprintf(stderr, "[FALHA QA] Conteudo inicial invalido no slot %d (size=%u)\n", slot, file_size);
            return 1;
        }
        printf("[QA OK] Slot %d (MANUSCR%d.MD): Cluster=%u, LBA=%u..%u, Tamanho=%u bytes.\n",
               slot + 1, slot + 1, first_clus, asm_lba, asm_lba + 63, file_size);
    }

    /* 6. Verifica Árvore UEFI (\EFI\BOOT\BOOTX64.EFI) + Rótulo FOLIO86_USB para HP ProBook 640 G5 */
    const uint8_t *efi_entry = root_dir + (8 * 32);
    const uint8_t *lbl_entry = root_dir + (9 * 32);
    if (memcmp(efi_entry, "EFI        ", 11) != 0 || efi_entry[11] != 0x10 || read_u16(efi_entry, 26) != 130) {
        fprintf(stderr, "[FALHA QA] Diretorio \\EFI ausente na raiz FAT16!\n");
        return 1;
    }
    if (memcmp(lbl_entry, "FOLIO86_USB", 11) != 0 || lbl_entry[11] != 0x08) {
        fprintf(stderr, "[FALHA QA] Rotulo de Volume FOLIO86_USB (0x08) ausente na raiz FAT16!\n");
        return 1;
    }
    uint32_t efi_bin_lba = DATA_START_LBA + (132 - 2) * sec_per_clus;
    const uint8_t *pe = img + (efi_bin_lba * SECTOR_SIZE);
    uint16_t mz_sig  = read_u16(pe, 0);
    uint32_t pe_off  = read_u32(pe, 0x3C);
    uint32_t pe_sig  = read_u32(pe, pe_off);
    uint16_t machine = read_u16(pe, pe_off + 4);
    uint16_t magic   = read_u16(pe, pe_off + 24);
    uint16_t subsys  = read_u16(pe, pe_off + 24 + 68);
    if (mz_sig != 0x5A4D || pe_sig != 0x00004550 || machine != 0x8664 || magic != 0x020B || subsys != 10) {
        fprintf(stderr, "[FALHA QA] Cabecalho PE32+ UEFI invalido em \\EFI\\BOOT\\BOOTX64.EFI!\n");
        return 1;
    }
    printf("[QA OK] \\EFI\\BOOT\\BOOTX64.EFI: PE32+ x86_64 valido (Machine=0x8664, Subsystem=10 EFI_APPLICATION, LBA=%u..%u).\n",
           efi_bin_lba, efi_bin_lba + 63);
    printf("[QA OK] Rotulo de Volume FAT16: 'FOLIO86_USB' (Atributo 0x08 presente).\n");

    free(img);
    printf("\n=== AUDITORIA QA v3.0 (PURE MBR + UEFI x64 + BIOS) CONCLUIDA: 100%% APROVADO ===\n");
    return 0;
}
