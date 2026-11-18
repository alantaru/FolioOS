/* FolioOS — teste das tabelas de partição (src/core/part.c) e da formatação FAT32 (fat32_format).
 *   part_test.exe <pasta de trabalho>
 * Cria imagens de disco e confere: disco vazio, GPT nova com 2 partições, formatação, arquivos gravados e
 * relidos, cabeçalho GPT principal estragado (lido pela cópia de reserva), apagar partição, MBR com
 * primárias, conversão GPT -> MBR sem restos, pendrive sem tabela, espaço livre alinhado a 1 MiB.
 * Deixa as imagens finais em <pasta>\gpt.img e <pasta>\mbr.img para o fat32_test --fsck. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include "../src/core/part.h"
#include "../src/core/fat32.h"

typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
static FILE *img; static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALHA (linha %d): ", __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fails++; } } while (0)
static bool io(void *ctx, u64 lba, u32 n, void *buf, bool w) {
    (void)ctx;
    if (_fseeki64(img, (long long)(lba * 512), SEEK_SET)) return false;
    bool ok = w ? fwrite(buf, 512, n, img) == n : fread(buf, 512, n, img) == n;
    if (w) fflush(img);
    return ok;
}
static BlkDev open_img(const char *path, u64 mb) {
    if (img) fclose(img);
    img = fopen(path, "w+b"); if (!img) { perror(path); exit(2); }
    _chsize_s(_fileno(img), (long long)(mb << 20));
    BlkDev d = { io, 0, (mb << 20) / 512, 512, 0 };
    return d;
}
static void put_file(Fat *f, const char *name, const char *text) {
    FatNode root, n; fat_root(f, &root);
    uint16_t w[64]; int i = 0; for (; name[i]; i++) w[i] = (uint16_t)(unsigned char)name[i]; w[i] = 0;
    CHECK(fat_create(f, &root, w, false, &n) == FAT_OK, "criar %s", name);
    CHECK(fat_write(f, &n, 0, text, (u32)strlen(text)) == (int)strlen(text), "gravar %s", name);
    fat_flush(f);
}
static void check_file(const BlkDev *d, u64 lba, const char *name, const char *text) {
    Fat f; CHECK(fat_mount(&f, io, 0, lba), "montar em %llu", (unsigned long long)lba);
    FatNode root, n; fat_root(&f, &root);
    uint16_t w[64]; int i = 0; for (; name[i]; i++) w[i] = (uint16_t)(unsigned char)name[i]; w[i] = 0;
    char buf[256] = { 0 };
    CHECK(fat_lookup(&f, &root, w, &n) == FAT_OK && fat_read(&f, &n, 0, buf, sizeof buf - 1) == (int)strlen(text) && !strcmp(buf, text), "reler %s", name);
    (void)d;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: part_test <pasta>\n"); return 2; }
    char p[512];
    PartTable t; Region r[16];

    /* 1. disco vazio */
    snprintf(p, sizeof p, "%s\\gpt.img", argv[1]);
    BlkDev d = open_img(p, 600);
    CHECK(part_read(&d, &t) && t.scheme == SCHEME_NONE, "disco vazio: esquema %d", t.scheme);

    /* 2. GPT nova: EFI de 100 MiB + dados no resto */
    part_new(&t, SCHEME_GPT, 12345);
    int nf = part_free(&d, &t, r, 16);
    CHECK(nf == 1 && r[0].start == 2048, "livre inicial: %d regiões, início %llu", nf, nf ? (unsigned long long)r[0].start : 0);
    int a = part_add(&d, &t, r[0].start, 100 * 2048, GPT_ESP, 0, "EFI system partition", 1);
    nf = part_free(&d, &t, r, 16);
    int b = part_add(&d, &t, r[0].start, r[0].count, GPT_MSDATA, 0, "FolioOS", 2);
    CHECK(a == 0 && b == 1, "adicionar: %d %d", a, b);
    CHECK(part_add(&d, &t, 4096, 2048, GPT_LINUX, 0, "x", 3) < 0, "sobreposição deveria falhar");
    CHECK(part_write(&d, &t), "gravar GPT");
    CHECK(fat32_format(io, 0, t.p[0].start, t.p[0].count, "EFI", 0x1111), "formatar EFI");
    CHECK(fat32_format(io, 0, t.p[1].start, t.p[1].count, "FOLIO", 0x2222), "formatar dados");
    PartTable u;
    CHECK(part_read(&d, &u) && u.scheme == SCHEME_GPT && u.n == 2 && !u.gpt_backup_used, "reler GPT: esquema %d, %d partições", u.scheme, u.n);
    CHECK(u.n == 2 && u.p[0].start == t.p[0].start && u.p[0].count == t.p[0].count && !memcmp(u.p[0].type, GPT_ESP, 16) && !memcmp(u.p[0].guid, t.p[0].guid, 16), "partição 1 diferente");
    CHECK(u.n == 2 && u.p[1].fs == FS_FAT32 && !strcmp(u.p[1].label, "FOLIO"), "partição 2: fs %s rótulo '%s'", part_fs_name(u.p[1].fs), u.p[1].label);
    CHECK(u.n == 2 && u.p[1].name[0] == 'F' && u.p[1].name[6] == 'S', "nome GPT");
    CHECK(part_free(&d, &u, r, 16) == 0, "não deveria sobrar espaço");
    Fat f;
    CHECK(fat_mount(&f, io, 0, u.p[1].start), "montar dados");
    put_file(&f, "hello.md", "# Hello from the partition test");
    check_file(&d, u.p[1].start, "hello.md", "# Hello from the partition test");
    {   /* a pasta FOLIO num volume com rótulo "FOLIO": nome curto "FOLIO", sem ~1 (o boot BIOS procura FOLIO\BIOS.SYS em 8.3) */
        FatNode root, n; fat_root(&f, &root);
        const uint16_t w[] = { 'F', 'O', 'L', 'I', 'O', 0 };
        CHECK(fat_create(&f, &root, w, true, &n) == FAT_OK && n.first == n.ent, "pasta FOLIO ganhou nome curto diferente (colidiu com o rótulo)");
        fat_flush(&f);
    }

    /* 3. cabeçalho principal estragado: vale a cópia de reserva */
    u8 s[512]; io(0, 1, 1, s, false); s[100] ^= 0xFF; s[16] ^= 0x55; io(0, 1, 1, s, true);
    CHECK(part_read(&d, &u) && u.scheme == SCHEME_GPT && u.gpt_backup_used && u.n == 2, "reserva: esquema %d usado %d n %d", u.scheme, u.gpt_backup_used, u.n);
    CHECK(part_write(&d, &u), "regravar a partir da reserva");
    CHECK(part_read(&d, &u) && !u.gpt_backup_used && u.n == 2, "principal restaurado");

    /* 4. apagar a partição EFI e criar outra no lugar */
    CHECK(part_del(&u, 0) && part_write(&d, &u), "apagar");
    CHECK(part_read(&d, &u) && u.n == 1 && u.p[0].fs == FS_FAT32, "depois de apagar: %d", u.n);
    nf = part_free(&d, &u, r, 16);
    CHECK(nf == 1 && r[0].start == 2048 && r[0].count == 100 * 2048, "espaço devolvido: %d %llu", nf, nf ? (unsigned long long)r[0].count : 0);
    CHECK(part_add(&d, &u, r[0].start, r[0].count, GPT_ESP, 0, "EFI", 9) == 1 && part_write(&d, &u), "recriar");
    CHECK(fat32_format(io, 0, u.p[1].start, u.p[1].count, "EFI", 0x3333), "reformatar");
    check_file(&d, u.p[0].start, "hello.md", "# Hello from the partition test");    /* a outra partição ficou intacta */

    /* 5. MBR: duas primárias, e conversão de GPT para MBR sem restos da GPT */
    snprintf(p, sizeof p, "%s\\mbr.img", argv[1]);
    d = open_img(p, 300);
    part_new(&t, SCHEME_GPT, 7); part_free(&d, &t, r, 16); part_add(&d, &t, r[0].start, r[0].count, GPT_MSDATA, 0, "old", 8); part_write(&d, &t);
    part_new(&t, SCHEME_MBR, 77);
    nf = part_free(&d, &t, r, 16);
    CHECK(nf == 1 && r[0].start == 2048, "MBR livre");
    a = part_add(&d, &t, 2048, 150 * 2048, 0, 0x0C, 0, 1);
    nf = part_free(&d, &t, r, 16);
    b = part_add(&d, &t, r[0].start, r[0].count, 0, 0x0C, 0, 2);
    t.p[0].active = true;
    CHECK(a == 0 && b == 1 && part_write(&d, &t), "gravar MBR");
    CHECK(fat32_format(io, 0, t.p[0].start, t.p[0].count, "ONE", 1) && fat32_format(io, 0, t.p[1].start, t.p[1].count, "TWO", 2), "formatar MBR");
    CHECK(part_read(&d, &u) && u.scheme == SCHEME_MBR && u.n == 2 && u.p[0].active && u.p[0].mbr_type == 0x0C, "reler MBR: esquema %d n %d", u.scheme, u.n);
    CHECK(u.n == 2 && !strcmp(u.p[0].label, "ONE") && !strcmp(u.p[1].label, "TWO"), "rótulos MBR");
    io(0, 1, 1, s, false); CHECK(memcmp(s, "EFI PART", 8) != 0, "restos da GPT no setor 1");
    io(0, d.nsec - 1, 1, s, false); CHECK(memcmp(s, "EFI PART", 8) != 0, "restos da GPT no fim");
    CHECK(fat_mount(&f, io, 0, u.p[1].start), "montar TWO"); put_file(&f, "two.txt", "second");

    /* 6. pendrive sem tabela (FAT32 desde o setor 0) */
    snprintf(p, sizeof p, "%s\\floppy.img", argv[1]);
    d = open_img(p, 100);
    CHECK(fat32_format(io, 0, 0, d.nsec, "STICK", 9), "formatar sem tabela");
    CHECK(part_read(&d, &u) && u.scheme == SCHEME_SUPERFLOPPY && u.n == 1 && u.p[0].fs == FS_FAT32, "sem tabela: esquema %d", u.scheme);
    CHECK(part_probe(&d, 0, d.nsec, 0, 0) == FS_FAT32, "probe");

    /* 7. detecção de outros sistemas (assinaturas sintéticas, numa imagem só para isso) */
    snprintf(p, sizeof p, "%s\\probe.img", argv[1]);
    d = open_img(p, 8);
    u8 z[4096]; memset(z, 0, sizeof z);
    memcpy(z + 3, "NTFS    ", 8); io(0, 1000, 1, z, true); CHECK(part_probe(&d, 1000, 4096, 0, 0) == FS_NTFS, "NTFS");
    memset(z, 0, sizeof z); memcpy(z + 3, "-FVE-FS-", 8); io(0, 2000, 1, z, true); CHECK(part_probe(&d, 2000, 4096, 0, 0) == FS_BITLOCKER, "BitLocker");
    memset(z, 0, sizeof z); z[1024 + 56] = 0x53; z[1024 + 57] = 0xEF; memcpy(z + 1024 + 120, "rootfs", 6); io(0, 3000, 8, z, true);
    char lab[40]; CHECK(part_probe(&d, 3000, 4096, lab, sizeof lab) == FS_EXT && !strcmp(lab, "rootfs"), "ext4 '%s'", lab);
    memset(z, 0, sizeof z); memcpy(z, "LUKS\xBA\xBE", 6); io(0, 4000, 1, z, true); CHECK(part_probe(&d, 4000, 4096, 0, 0) == FS_LUKS, "LUKS");
    memset(z, 0, sizeof z); memcpy(z + 32, "NXSB", 4); io(0, 5000, 1, z, true); CHECK(part_probe(&d, 5000, 4096, 0, 0) == FS_APFS, "APFS");
    memset(z, 0, sizeof z); io(0, 6000, 8, z, true); CHECK(part_probe(&d, 6000, 4096, 0, 0) == FS_EMPTY, "vazio");

    fclose(img);
    printf("tabelas de partição e formatação: %s\n", fails ? "COM FALHAS" : "tudo certo");
    return fails ? 1 : 0;
}
