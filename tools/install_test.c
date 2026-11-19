/* FolioOS — teste do motor de instalação (src/core/install.c) no Windows.
 *   install_test.exe <pasta de origem (um pendrive FolioOS)> <imagem de destino> <MB> [--bios] [--docs-stick]
 * Cria a imagem (um "disco interno" vazio, ou usa a existente se já tiver o tamanho) e instala nela o
 * FolioOS da pasta, exatamente como o instalador faz no computador. A imagem resultante é iniciada no
 * laboratório QEMU como disco interno (OVMF para GPT/UEFI, SeaBIOS para --bios).
 *   --windows: antes, monta um disco GPT "com Windows" (EFI, MSR, NTFS de mentira, espaço livre);
 *              sem --alongside, só monta o disco e sai; --winefi <arquivo>: esse programa EFI no lugar do Windows
 *   --compare <antes.img>: confere que nada do que existia em antes.img mudou (depois de instalar ao lado)
 *   --alongside: instala ao lado (fase 2) e confere que nenhuma partição existente mudou
 *   --bitlocker: com --windows, o C: aparece cifrado com BitLocker (o instalador pede a chave de recuperação)
 *   --shrink-ntfs: o disco já existe (tests/fixtures/ntfs-big: EFI + NTFS do Windows, sem espaço livre); encolhe o
 *                  NTFS e instala depois dele
 *   --put-winefi <arquivo>: num disco que já existe, esse programa EFI vira o Windows Boot Manager (laboratório)
 *   --remove: desinstala o que foi instalado ao lado (textos para a origem, NTFS crescido de volta) e confere
 *   --update: atualiza o FolioOS instalado com a origem; texto e CONFIG.INI da pessoa intactos, programa igual
 *   --same-table <original.img>: MBR, GPT e setores de boot (NTFS, FAT32, exFAT) iguais aos do disco original */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include "../src/core/install.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
static FILE *img; static char root[MAX_PATH];
static bool dio(void *ctx, u64 lba, u32 n, void *buf, bool w) {
    (void)ctx;
    if (_fseeki64(img, (long long)(lba * 512), SEEK_SET)) return false;
    return w ? fwrite(buf, 512, n, img) == n : fread(buf, 512, n, img) == n;
}
static void full(const char *rel, wchar_t *out) {
    char p[1024]; snprintf(p, sizeof p, "%s\\%s", root, rel);
    for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
    MultiByteToWideChar(CP_UTF8, 0, p, -1, out, 1024);
}
static bool s_list(void *ctx, const char *dir, void (*cb)(void *, const char *, bool, u64), void *u) {
    (void)ctx; wchar_t w[1024]; full(dir, w); wcscat(w, L"\\*");
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        char name[512]; WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, 0, 0);
        cb(u, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}
static int s_read(void *ctx, const char *path, u64 off, void *buf, u32 n) {
    (void)ctx; wchar_t w[1024]; full(path, w);
    FILE *f = _wfopen(w, L"rb"); if (!f) return -1;
    _fseeki64(f, (long long)off, SEEK_SET);
    int got = (int)fread(buf, 1, n, f); fclose(f);
    return got;
}
/* desinstalar: grava na "origem" (a pasta que faz o papel do pendrive), criando as pastas do caminho */
static bool s_save(void *ctx, const char *path, const void *data, u32 n) {
    (void)ctx; char part[1024]; int k = 0;
    for (const char *p = path; *p; p++) {
        if (*p == '/') { part[k] = 0; wchar_t w[1024]; full(part, w); CreateDirectoryW(w, 0); }
        part[k++] = *p;
    }
    wchar_t w[1024]; full(path, w);
    FILE *f = _wfopen(w, L"wb"); if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n; fclose(f); return ok;
}
static bool s_backup(void *ctx, const char *name, const void *data, u32 n) {
    (void)ctx; wchar_t w[1024]; full("FOLIO/BACKUP", w); CreateDirectoryW(w, 0);
    char rel[256]; snprintf(rel, sizeof rel, "FOLIO/BACKUP/%s", name); full(rel, w);
    FILE *f = _wfopen(w, L"wb"); if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n; fclose(f); return ok;
}
static int last_pct = -1;
static void s_progress(void *ctx, int pct, const char *what) { (void)ctx; if (pct / 10 != last_pct / 10) { printf("  %3d%%  %s\n", pct, what); last_pct = pct; } }

/* ------------------------------------------------------------ disco "com Windows" (--windows) */
#include "../src/core/fat32.h"
#include "../src/core/fscheck.h"
static u64 rng_s = 0x1234567;
static bool bitlocker;   /* --bitlocker: o C: aparece cifrado com BitLocker */
static u32 rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return (u32)rng_s; }
static void fill_random(u64 lba, u64 count, bool ntfs) {
    static u8 b[1 << 16];
    for (u64 done = 0; done < count; ) {
        u32 n = count - done < sizeof b / 512 ? (u32)(count - done) : (u32)(sizeof b / 512);
        for (u32 i = 0; i < n * 512; i += 4) *(u32 *)(b + i) = rnd();
        if (done == 0 && ntfs) { memcpy(b + 3, bitlocker ? "-FVE-FS-" : "NTFS    ", 8); b[510] = 0x55; b[511] = 0xAA; }   /* parece NTFS (o FolioOS não mexe) */
        dio(0, lba + done, n, b, true); done += n;
    }
}
static void fat_put(Fat *f, FatNode *dir, const char *name, bool is_dir, u32 size, FatNode *out) {
    u16 w[64]; int i = 0; for (; name[i]; i++) w[i] = (u16)(unsigned char)name[i]; w[i] = 0;
    if (fat_create(f, dir, w, is_dir, out) != FAT_OK) { fprintf(stderr, "não criou %s\n", name); exit(2); }
    if (!is_dir && size) { u8 *b = malloc(size); for (u32 k = 0; k < size; k++) b[k] = (u8)rnd(); fat_write(f, out, 0, b, size); free(b); }
}
/* GPT como um Windows depois de encolher o C:: partição EFI 100 MB (FAT32 com o Windows Boot Manager e o
 * \EFI\BOOT de reserva), MSR 16 MB, um buraco na tabela (entrada 3 apagada), "Windows" 300 MB e
 * recuperação 200 MB no fim do disco, com dados aleatórios; o espaço livre fica entre o C: e a recuperação */
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static const char *winbios;                        /* --windows-bios <mbr.bin>: um PC só BIOS com "Windows" */
static const char *mbrfat;                         /* --mbr-fat32 <mbr.bin>: PC só BIOS, FAT32 no disco todo */
static const char *winefi;                         /* --winefi: um programa EFI de verdade no lugar do Windows (laboratório) */
static void put_efi(Fat *f, FatNode *dir, const char *name) {
    FatNode x;
    if (!winefi) { fat_put(f, dir, name, false, 1500000, &x); return; }
    FILE *e = fopen(winefi, "rb"); if (!e) { perror(winefi); exit(2); }
    static u8 b[4 << 20]; u32 n = (u32)fread(b, 1, sizeof b, e); fclose(e);
    fat_put(f, dir, name, false, 0, &x); fat_write(f, &x, 0, b, n);
}
/* --windows-bios <mbr.bin>: um PC só BIOS com "Windows": MBR com o código dado (build/fakeother.bin), a partição
 * ativa no começo (NTFS de mentira, 300 MB), outra no fim (100 MB) e espaço livre no meio */
static void make_windows_bios(const BlkDev *d, const char *code_path) {
    static PartTable t; part_new(&t, SCHEME_MBR, 99);
    int w = part_add(d, &t, 2048, 300 * 2048, GPT_MSDATA, 0x07, "", 1);
    u64 rs = (d->nsec - 100 * 2048) / 2048 * 2048;
    int rc = part_add(d, &t, rs, 100 * 2048, GPT_MSDATA, 0x27, "", 2);
    if (w < 0 || rc < 0) { fprintf(stderr, "tabela falhou\n"); exit(2); }
    t.p[w].active = true;
    if (!part_write(d, &t)) { fprintf(stderr, "tabela falhou\n"); exit(2); }
    fill_random(t.p[w].start, t.p[w].count, true); fill_random(t.p[rc].start, t.p[rc].count, false);
    static u8 s[512], code[440]; FILE *c = fopen(code_path, "rb");
    if (!c || fread(code, 1, 440, c) != 440) { fprintf(stderr, "código do MBR: %s\n", code_path); exit(2); }
    fclose(c);
    dio(0, 0, 1, s, false); memcpy(s, code, 440); dio(0, 0, 1, s, true);
}
/* --mbr-fat32 <mbr.bin>: um PC só BIOS sem espaço livre: MBR com o código dado e uma FAT32 ativa no disco todo
 * (com alguns arquivos), para instalar encolhendo-a (fase 3) */
static void make_mbr_fat32(const BlkDev *d, const char *code_path) {
    static PartTable t; part_new(&t, SCHEME_MBR, 98);
    Region fr[2]; int nf = part_free(d, &t, fr, 2);
    int k = nf ? part_add(d, &t, fr[0].start, fr[0].count, GPT_MSDATA, 0x0C, "", 3) : -1;
    if (k < 0) { fprintf(stderr, "tabela falhou\n"); exit(2); }
    t.p[k].active = true;
    if (!part_write(d, &t) || !fat32_format(dio, 0, t.p[k].start, t.p[k].count, "OTHER", 0x5151)) { fprintf(stderr, "formatação falhou\n"); exit(2); }
    static Fat f; FatNode root, x; fat_mount(&f, dio, 0, t.p[k].start); fat_root(&f, &root);
    for (int i = 0; i < 20; i++) { char n[16]; snprintf(n, sizeof n, "doc%02d.txt", i); fat_put(&f, &root, n, false, 4096 * (i + 1), &x); }
    fat_flush(&f);
    static u8 s[512], code[440]; FILE *c = fopen(code_path, "rb");
    if (!c || fread(code, 1, 440, c) != 440) { fprintf(stderr, "código do MBR: %s\n", code_path); exit(2); }
    fclose(c);
    dio(0, 0, 1, s, false); memcpy(s, code, 440); dio(0, 0, 1, s, true);
}
static void make_windows(const BlkDev *d) {
    static PartTable t; part_new(&t, SCHEME_GPT, 77);
    int e = part_add(d, &t, 2048, 100 * 2048, GPT_ESP, 0xEF, "EFI system partition", 1);
    int m = part_add(d, &t, 2048 + 100 * 2048, 16 * 2048, GPT_MSR, 0, "Microsoft reserved partition", 2);
    int gone = part_add(d, &t, 2048 + 116 * 2048, 8 * 2048, GPT_MSDATA, 0x07, "deleted", 3);
    int w = part_add(d, &t, 2048 + 124 * 2048, 300 * 2048, GPT_MSDATA, 0x07, "Basic data partition", 4);
    u64 rs = (d->nsec - 34 - 200 * 2048) / 2048 * 2048;
    int rc = part_add(d, &t, rs, 200 * 2048, GPT_WINRE, 0x27, "Recovery", 5);
    if (e < 0 || m < 0 || gone < 0 || w < 0 || rc < 0) { fprintf(stderr, "tabela do Windows falhou\n"); exit(2); }
    t.p[rc].attrs = 0x8000000000000001ull;             /* obrigatória + não montar, como a do Windows */
    part_del(&t, gone);                                /* buraco: a entrada 3 fica vazia */
    for (int i = 0; i < t.n; i++) {                    /* part_del desloca os índices da lista (não as entradas da tabela) */
        if (!memcmp(t.p[i].type, GPT_ESP, 16)) e = i;
        if (!memcmp(t.p[i].type, GPT_MSR, 16)) m = i;
        if (!memcmp(t.p[i].type, GPT_WINRE, 16)) rc = i;
        if (t.p[i].start == 2048 + 124 * 2048) w = i;
    }
    if (!part_write(d, &t)) { fprintf(stderr, "tabela do Windows falhou\n"); exit(2); }
    fill_random(t.p[rc].start, t.p[rc].count, true);
    if (!fat32_format(dio, 0, t.p[e].start, t.p[e].count, "SYSTEM", 0xABCD)) { fprintf(stderr, "formatação da EFI falhou\n"); exit(2); }
    static Fat f; fat_mount(&f, dio, 0, t.p[e].start);
    FatNode root, efi, ms, boot, bt, x; fat_root(&f, &root);
    fat_put(&f, &root, "EFI", true, 0, &efi); fat_put(&f, &efi, "Microsoft", true, 0, &ms); fat_put(&f, &ms, "Boot", true, 0, &boot);
    put_efi(&f, &boot, "bootmgfw.efi"); fat_put(&f, &boot, "BCD", false, 32768, &x);
    fat_put(&f, &efi, "BOOT", true, 0, &bt); put_efi(&f, &bt, "BOOTX64.EFI");
    fat_flush(&f);
    fill_random(t.p[m].start, t.p[m].count, false); fill_random(t.p[w].start, t.p[w].count, true);
}
/* --put-winefi <arquivo>: num disco que já existe (o feito pelo Windows, tests/fixtures/ntfs-big), esse programa EFI
 * passa a ser o "Windows Boot Manager" e o \EFI\BOOT de reserva (o laboratório inicia o fakewin.efi por ele) */
static void dir_get(Fat *f, FatNode *dir, const char *name, FatNode *out) {
    u16 w[64]; int i = 0; for (; name[i]; i++) w[i] = (u16)(unsigned char)name[i]; w[i] = 0;
    if (fat_lookup(f, dir, w, out) != FAT_OK && fat_create(f, dir, w, true, out) != FAT_OK) { fprintf(stderr, "pasta %s\n", name); exit(2); }
}
static void file_put(Fat *f, FatNode *dir, const char *name, const u8 *b, u32 n) {
    u16 w[64]; int i = 0; for (; name[i]; i++) w[i] = (u16)(unsigned char)name[i]; w[i] = 0;
    FatNode x;
    if (fat_lookup(f, dir, w, &x) == FAT_OK) fat_truncate(f, &x, 0);
    else if (fat_create(f, dir, w, false, &x) != FAT_OK) { fprintf(stderr, "arquivo %s\n", name); exit(2); }
    if (fat_write(f, &x, 0, b, n) != (int)n) { fprintf(stderr, "gravar %s\n", name); exit(2); }
}
static int put_winefi(const BlkDev *d, const char *path) {
    static PartTable t; static Fat f; static u8 b[4 << 20];
    FILE *e = fopen(path, "rb"); if (!e) { perror(path); return 2; }
    u32 n = (u32)fread(b, 1, sizeof b, e); fclose(e);
    if (!part_read(d, &t)) { fprintf(stderr, "sem tabela\n"); return 2; }
    int esp = -1; for (int i = 0; i < t.n && esp < 0; i++) if (!memcmp(t.p[i].type, GPT_ESP, 16)) esp = i;
    if (esp < 0 || !fat_mount(&f, dio, 0, t.p[esp].start)) { fprintf(stderr, "sem partição EFI\n"); return 2; }
    FatNode root, efi, ms, boot, bt; fat_root(&f, &root);
    dir_get(&f, &root, "EFI", &efi); dir_get(&f, &efi, "Microsoft", &ms); dir_get(&f, &ms, "Boot", &boot); dir_get(&f, &efi, "BOOT", &bt);
    file_put(&f, &boot, "bootmgfw.efi", b, n); file_put(&f, &bt, "BOOTX64.EFI", b, n);
    fat_flush(&f);
    printf("%s no lugar do Windows Boot Manager\n", path);
    return 0;
}
/* --same-table <original.img>: depois de instalar e desinstalar, o MBR, as duas cópias da GPT e o setor de boot de
 * cada NTFS são byte a byte os do disco original (o Windows não percebe que houve uma instalação) */
static int same_table(const BlkDev *d, const char *orig) {
    FILE *o = fopen(orig, "rb"); if (!o) { perror(orig); return 2; }
    static u8 a[512 * 34], b[512 * 34]; int fails = 0;
    u64 spots[3] = { 0, d->nsec - 33, 0 };
    for (int k = 0; k < 2; k++) {
        u32 n = k ? 33 : 34;
        _fseeki64(o, (long long)(spots[k] * 512), SEEK_SET);
        if (fread(b, 512, n, o) != n || !dio(0, spots[k], n, a, false)) { fprintf(stderr, "ler\n"); return 2; }
        for (u32 s = 0; s < n; s++) if (memcmp(a + s * 512, b + s * 512, 512)) { fprintf(stderr, "FALHA: setor %llu da tabela diferente do original\n", (unsigned long long)(spots[k] + s)); fails++; }
    }
    static PartTable t;
    if (!part_read(d, &t)) return 2;
    for (int i = 0; i < t.n; i++) {
        if (!fs_resizable(t.p[i].fs) || !memcmp(t.p[i].type, GPT_ESP, 16)) continue;
        _fseeki64(o, (long long)(t.p[i].start * 512), SEEK_SET);
        if (fread(b, 1, 512, o) != 512 || !dio(0, t.p[i].start, 1, a, false)) return 2;
        if (t.p[i].fs == FS_EXFAT) a[0x70] = b[0x70];   /* exFAT: PercentInUse fica "desconhecido" (0xFF), o que vale */
        if (memcmp(a, b, 512)) { fprintf(stderr, "FALHA: o setor de boot (%s) em %llu mudou (o tamanho do volume)\n", part_fs_name(t.p[i].fs), (unsigned long long)t.p[i].start); fails++; }
    }
    fclose(o);
    if (!fails) printf("tabela, MBR e setores de boot dos volumes iguais aos do original\n");
    return fails ? 1 : 0;
}
static u32 crc_range(u64 lba, u64 count) {
    static u8 b[1 << 16]; u32 c = 0;
    for (u64 done = 0; done < count; ) {
        u32 n = count - done < sizeof b / 512 ? (u32)(count - done) : (u32)(sizeof b / 512);
        if (!dio(0, lba + done, n, b, false)) { fprintf(stderr, "FALHA: não deu para ler o setor %llu\n", (unsigned long long)(lba + done)); exit(1); }   /* nunca um CRC de dados velhos */
        c = crc32_buf(c, b, n * 512); done += n;
    }
    return c;
}

/* --compare <antes.img>: tudo o que existia antes continua igual (entradas da tabela, conteúdo das partições
 * que não são a EFI e os arquivos do Windows na partição EFI), e a tabela ganhou só a partição do FolioOS */
static FILE *img_b;
static bool dio_b(void *ctx, u64 lba, u32 n, void *buf, bool w) {
    (void)ctx; if (w || _fseeki64(img_b, (long long)(lba * 512), SEEK_SET)) return false;
    return fread(buf, 512, n, img_b) == n;
}
static u32 crc_file_in(FatIo io, u64 part, const char *path, u32 *size) {
    static Fat f; if (!fat_mount(&f, io, 0, part)) return 0;
    FatNode cur; fat_root(&f, &cur);
    char buf[256]; snprintf(buf, sizeof buf, "%s", path);
    for (char *tok = strtok(buf, "/"); tok; tok = strtok(0, "/")) {
        u16 w[64]; int i = 0; for (; tok[i]; i++) w[i] = (u16)(unsigned char)tok[i]; w[i] = 0;
        FatNode nx; if (fat_lookup(&f, &cur, w, &nx) != FAT_OK) return 0;
        cur = nx;
    }
    static u8 b[1 << 16]; u32 c = 0;
    for (u32 off = 0; off < cur.size; ) { u32 n = cur.size - off < sizeof b ? cur.size - off : (u32)sizeof b; if (fat_read(&f, &cur, off, b, n) != (int)n) return 0; c = crc32_buf(c, b, n); off += n; }
    *size = cur.size; return c ? c : 1;
}
static int compare(const BlkDev *d, const char *before) {
    img_b = fopen(before, "rb"); if (!img_b) { perror(before); return 2; }
    _fseeki64(img_b, 0, SEEK_END); u64 nb = (u64)_ftelli64(img_b) / 512;
    BlkDev db = { dio_b, 0, nb, 512, 0 };
    static PartTable a, b; int fails = 0;
    if (!part_read(&db, &b) || !part_read(d, &a)) { fprintf(stderr, "FALHA: tabela ilegível\n"); return 1; }
    if (a.n != b.n + 1) { fprintf(stderr, "FALHA: %d entradas antes, %d depois (devia ser uma a mais)\n", b.n, a.n); fails++; }
    if (b.scheme == SCHEME_MBR) {                     /* cada entrada que existia: os 16 bytes iguais (ativa, tipo, CHS…) */
        static u8 m0[512], m1[512];
        if (!dio_b(0, 0, 1, m0, false) || !dio(0, 0, 1, m1, false)) { fprintf(stderr, "FALHA: ler o MBR\n"); fails++; }
        else for (int s = 0; s < 4; s++) {
            const u8 *e0 = m0 + 446 + 16 * s, *e1 = m1 + 446 + 16 * s;
            if (e0[4] && memcmp(e0, e1, 16)) { fprintf(stderr, "FALHA: a entrada %d do MBR mudou (ativa, tipo ou posição)\n", s + 1); fails++; }
        }
    }
    for (int i = 0; i < b.n; i++) {
        const Part *o = &b.p[i], *n = 0;
        for (int k = 0; k < a.n; k++) if (b.scheme == SCHEME_MBR ? a.p[k].start == o->start : !memcmp(a.p[k].guid, o->guid, 16)) n = &a.p[k];   /* MBR: sem GUID */
        if (!n || n->slot != o->slot || n->start != o->start || n->count != o->count || memcmp(n->type, o->type, 16) || memcmp(n->name, o->name, sizeof o->name) || n->attrs != o->attrs) {
            fprintf(stderr, "FALHA: a entrada %d da tabela mudou\n", o->slot + 1); fails++; continue;
        }
        if (!memcmp(o->type, GPT_ESP, 16)) {          /* a partição EFI: os arquivos do Windows continuam iguais */
            static const char *keep[] = { "EFI/Microsoft/Boot/bootmgfw.efi", "EFI/Microsoft/Boot/BCD", "EFI/BOOT/BOOTX64.EFI" };
            for (int k = 0; k < 3; k++) {
                u32 s1 = 0, s2 = 0, c1 = crc_file_in(dio_b, o->start, keep[k], &s1), c2 = crc_file_in(dio, n->start, keep[k], &s2);
                if (!c1 || c1 != c2 || s1 != s2) { fprintf(stderr, "FALHA: %s mudou na partição EFI\n", keep[k]); fails++; }
            }
            continue;
        }
        static u8 x[1 << 16], y[1 << 16];
        for (u64 done = 0; done < o->count; ) {
            u32 m = o->count - done < sizeof x / 512 ? (u32)(o->count - done) : (u32)(sizeof x / 512);
            if (!dio_b(0, o->start + done, m, x, false) || !dio(0, o->start + done, m, y, false)) { fprintf(stderr, "FALHA: não deu para ler a partição %d (setor %llu)\n", o->slot + 1, (unsigned long long)(o->start + done)); fails++; break; }
            if (memcmp(x, y, m * 512)) { fprintf(stderr, "FALHA: o conteúdo da partição %d mudou (setor %llu)\n", o->slot + 1, (unsigned long long)(o->start + done)); fails++; break; }
            done += m;
        }
    }
    fclose(img_b);
    if (!fails) printf("comparação: %d partições e entradas iguais, arquivos do Windows intactos\n", b.n);
    return fails ? 1 : 0;
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 4) { fprintf(stderr, "uso: install_test <origem> <imagem> <MB> [--bios] [--docs-stick] [--windows] [--alongside]\n"); return 2; }
    GetFullPathNameA(argv[1], MAX_PATH, root, 0);
    char *mend; u64 mb = strtoull(argv[3], &mend, 10);
    if (!argv[3][0] || *mend || !mb || mb > (16ull << 20)) { fprintf(stderr, "tamanho inválido: \"%s\" (MB)\n", argv[3]); return 2; }
    InstallOpts o = { 0 }; o.label = "FOLIO"; o.efi_name = "BOOTX64.EFI"; o.version = "test";
    bool windows = false, alongside = false, shrink = false, remove_mode = false, update_mode = false;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--bios")) o.bios = true;
        if (!strcmp(argv[i], "--docs-stick")) o.docs = INST_DOCS_STICK;
        if (!strcmp(argv[i], "--docs-both")) o.docs = INST_DOCS_BOTH;
        if (!strcmp(argv[i], "--windows")) windows = true;
        if (!strcmp(argv[i], "--bitlocker")) bitlocker = true;
        if (!strcmp(argv[i], "--alongside")) alongside = true;
        if (!strcmp(argv[i], "--shrink-ntfs")) alongside = shrink = true;
        if (!strcmp(argv[i], "--remove")) remove_mode = true;
        if (!strcmp(argv[i], "--update")) update_mode = true;
        if (!strcmp(argv[i], "--winefi") && i + 1 < argc) winefi = argv[++i];
        if (!strcmp(argv[i], "--windows-bios") && i + 1 < argc) winbios = argv[++i];
        if (!strcmp(argv[i], "--mbr-fat32") && i + 1 < argc) mbrfat = argv[++i];
    }
    /* só leitura (--list, --compare, --same-table): a imagem como está, nada muda de tamanho. Nos outros modos uma
     * imagem que já existe nunca encolhe (isso apagaria o fim dela): só uma nova (ou menor) ganha o tamanho pedido. */
    bool readonly = false;
    for (int i = 4; i < argc; i++) if (!strcmp(argv[i], "--list") || !strcmp(argv[i], "--compare") || !strcmp(argv[i], "--same-table")) readonly = true;
    img = fopen(argv[2], readonly ? "rb" : "r+b");
    if (!img && readonly) { perror(argv[2]); return 2; }
    if (!img) { img = fopen(argv[2], "w+b"); if (!img) { perror(argv[2]); return 2; } }
    _fseeki64(img, 0, SEEK_END); u64 have = (u64)_ftelli64(img);
    if (!readonly && have < (mb << 20) && _chsize_s(_fileno(img), (long long)(mb << 20))) { fprintf(stderr, "não deu para criar a imagem de %llu MB\n", (unsigned long long)mb); fclose(img); return 2; }
    u64 bytes = readonly || have > (mb << 20) ? have : (mb << 20);   /* o tamanho real do disco */
    mb = bytes >> 20;
    BlkDev d = { dio, 0, bytes / 512, 512, 0 };
    /* --set-version <v>: o FOLIO/INSTALLED.ID da instalação passa a dizer version=<v> (o teste da atualização pela tela
     * parte de uma versão antiga: se a atualização não rodar, a versão nova não aparece) */
    for (int i = 4; i + 1 < argc; i++) if (!strcmp(argv[i], "--set-version")) {
        static PartTable st; static Fat sv; static char id[1024], out[1100];
        if (!part_read(&d, &st)) { fprintf(stderr, "FALHA: tabela\n"); return 1; }
        for (int k = 0; k < st.n; k++) {
            if (st.p[k].fs != FS_FAT32 || !fat_mount(&sv, dio, 0, st.p[k].start)) continue;
            FatNode root, folio, idn; fat_root(&sv, &root);
            u16 w1[8] = { 'F', 'O', 'L', 'I', 'O', 0 }, w2[16] = { 'I', 'N', 'S', 'T', 'A', 'L', 'L', 'E', 'D', '.', 'I', 'D', 0 };
            if (fat_lookup(&sv, &root, w1, &folio) != FAT_OK || fat_lookup(&sv, &folio, w2, &idn) != FAT_OK || idn.size >= sizeof id) continue;
            int n = fat_read(&sv, &idn, 0, id, idn.size); if (n != (int)idn.size) { fprintf(stderr, "FALHA: ler INSTALLED.ID\n"); return 1; }
            id[n] = 0; int o = 0;
            for (char *line = id; *line; ) {               /* troca só a linha version= */
                char *e = line; while (*e && *e != '\n') e++;
                if (!strncmp(line, "version=", 8)) o += snprintf(out + o, sizeof out - o, "version=%s\r\n", argv[i + 1]);
                else { memcpy(out + o, line, (size_t)(e - line) + (*e ? 1 : 0)); o += (int)(e - line) + (*e ? 1 : 0); }
                line = *e ? e + 1 : e;
            }
            if (fat_truncate(&sv, &idn, 0) != FAT_OK || fat_write(&sv, &idn, 0, out, (u32)o) != o || fat_flush(&sv) != FAT_OK) { fprintf(stderr, "FALHA: gravar INSTALLED.ID\n"); return 1; }
            fclose(img); printf("versão instalada: %s\n", argv[i + 1]); return 0;
        }
        fprintf(stderr, "FALHA: nenhum FOLIO/INSTALLED.ID\n"); return 1;
    }
    for (int i = 4; i + 1 < argc; i++) if (!strcmp(argv[i], "--compare")) { int rc = compare(&d, argv[i + 1]); fclose(img); return rc; }
    for (int i = 4; i + 1 < argc; i++) if (!strcmp(argv[i], "--put-winefi")) { int rc = put_winefi(&d, argv[i + 1]); fclose(img); return rc; }
    for (int i = 4; i + 1 < argc; i++) if (!strcmp(argv[i], "--same-table")) { int rc = same_table(&d, argv[i + 1]); fclose(img); return rc; }
    if (windows) make_windows(&d);
    if (mbrfat) { make_mbr_fat32(&d, mbrfat); if (!alongside) { fclose(img); printf("disco MBR com FAT32 montado\n"); return 0; } }
    if (winbios) { make_windows_bios(&d, winbios); if (!alongside) { fclose(img); printf("disco com Windows (BIOS) de teste montado\n"); return 0; } }
    if (windows && !alongside) { fclose(img); printf("disco com Windows de teste montado\n"); return 0; }   /* só o disco (para testes em etapas) */
    static u8 buf[1 << 21];
    InstallSrc s = { 0 };
    s.list = s_list; s.read = s_read; s.backup = s_backup; s.progress = s_progress;
    s.seed = GetTickCount64() * 2654435761ull; s.buf = buf; s.bufsize = sizeof buf;
    snprintf(s.date, sizeof s.date, "t%llx-%lx", (unsigned long long)GetTickCount64(), (unsigned long)GetCurrentProcessId());   /* único mesmo com duas execuções no mesmo tique */
    InstallResult r;
    s.save = s_save;
    /* gerenciador de partições: --list (a tabela), --op-create <início MiB> <MiB> <rótulo>, --op-delete <n>,
     * --op-format <n> <rótulo>, --op-resize <n> <MiB> (n = a ordem do --list, a partir de 1) */
    for (int i = 4; i < argc; i++) {
        static PartTable lt; bool listing = !strcmp(argv[i], "--list");
        bool op = !strncmp(argv[i], "--op-", 5);
        if (!listing && !op) continue;
        if (op) {
            if (!part_read(&d, &lt)) lt.n = 0;
            int n = i + 1 < argc ? atoi(argv[i + 1]) : 0; Part *p = n >= 1 && n <= lt.n ? &lt.p[n - 1] : 0;
            if (!strcmp(argv[i], "--op-create") && i + 3 < argc) part_op_create(&d, &s, (u64)atoll(argv[i + 1]) * 2048, (u64)atoll(argv[i + 2]) * 2048, argv[i + 3], &r);
            else if (!strcmp(argv[i], "--op-delete") && p) part_op_delete(&d, &s, p->start, &r);
            else if (!strcmp(argv[i], "--op-format") && p && i + 2 < argc) part_op_format(&d, &s, p->start, argv[i + 2], &r);
            else if (!strcmp(argv[i], "--op-resize") && p && i + 2 < argc) part_op_resize(&d, &s, p->start, (u64)atoll(argv[i + 2]) * 2048, &r);
            else { fprintf(stderr, "uso: %s (partição %d de %d)\n", argv[i], n, lt.n); fclose(img); return 2; }
            if (r.code) { fprintf(stderr, "FALHA: %s: %s\n", install_error(r.code), r.path); fclose(img); return 1; }
            printf("%s: ok\n", argv[i]);
        }
        if (!part_read(&d, &lt)) { printf("sem tabela\n"); fclose(img); return 0; }
        for (int k = 0; k < lt.n; k++)
            printf("%d %llu %llu %s %s\n", k + 1, (unsigned long long)lt.p[k].start, (unsigned long long)lt.p[k].count, part_fs_name(lt.p[k].fs), lt.p[k].label[0] ? lt.p[k].label : "-");
        fclose(img); return 0;
    }
    if (update_mode) {
        /* atualizar: o que é da pessoa (um texto e o CONFIG.INI) não pode mudar; o programa tem de ficar igual ao da origem */
        static Fat v; InstallFound f, g; int fails = 0;
        if (!install_find(&d, &f)) { fprintf(stderr, "FALHA: nenhum FolioOS instalado neste disco\n"); fclose(img); return 1; }
        static const char mine[] = "# meu texto\nnão pode mudar\n", cfg[] = "theme=3\r\nuser_setting=1\r\n";
        if (!fat_mount(&v, dio, 0, f.part_start)) { fprintf(stderr, "montar\n"); return 2; }
        { FatNode root, docs, folio, x; fat_root(&v, &root); u16 w[64];
          #define W(s) do { int _i = 0; for (; (s)[_i]; _i++) w[_i] = (u16)(unsigned char)(s)[_i]; w[_i] = 0; } while (0)
          W("DOCUMENTS"); if (fat_lookup(&v, &root, w, &docs) != FAT_OK) fat_create(&v, &root, w, true, &docs);
          W("meu.md"); if (fat_lookup(&v, &docs, w, &x) == FAT_OK) fat_delete(&v, &x); fat_create(&v, &docs, w, false, &x); fat_write(&v, &x, 0, mine, (u32)strlen(mine));
          W("FOLIO"); fat_lookup(&v, &root, w, &folio);
          W("CONFIG.INI"); if (fat_lookup(&v, &folio, w, &x) == FAT_OK) fat_delete(&v, &x); fat_create(&v, &folio, w, false, &x); fat_write(&v, &x, 0, cfg, (u32)strlen(cfg));
          fat_flush(&v); }
        o.version = "update-test";
        install_update(&d, &s, &o, &f, &r);
        if (r.code) { fprintf(stderr, "FALHA: %s: %s\n", install_error(r.code), r.path); fclose(img); return 1; }
        if (!install_find(&d, &g) || strcmp(g.version, "update-test")) { fprintf(stderr, "FALHA: a versão instalada não mudou (%s)\n", g.version); fails++; }
        /* o que é da pessoa */
        if (!fat_mount(&v, dio, 0, f.part_start)) return 2;
        static char got[256]; FatNode root, dir, x; fat_root(&v, &root); u16 w[64];
        W("DOCUMENTS"); fat_lookup(&v, &root, w, &dir); W("meu.md"); memset(got, 0, sizeof got);
        if (fat_lookup(&v, &dir, w, &x) != FAT_OK || fat_read(&v, &x, 0, got, sizeof got - 1) != (int)strlen(mine) || strcmp(got, mine)) { fprintf(stderr, "FALHA: o texto da pessoa mudou\n"); fails++; }
        W("FOLIO"); fat_lookup(&v, &root, w, &dir); W("CONFIG.INI"); memset(got, 0, sizeof got);
        if (fat_lookup(&v, &dir, w, &x) != FAT_OK || fat_read(&v, &x, 0, got, sizeof got - 1) != (int)strlen(cfg) || strcmp(got, cfg)) { fprintf(stderr, "FALHA: o CONFIG.INI da pessoa mudou\n"); fails++; }
        /* o programa: igual ao da origem, no disco e (ao lado) na partição EFI; e nenhum "*.~new" esquecido */
        static u8 a[4 << 20], b[4 << 20];
        FILE *e = NULL; { wchar_t wp[1024]; full("EFI/BOOT/BOOTX64.EFI", wp); e = _wfopen(wp, L"rb"); }
        u32 n = e ? (u32)fread(a, 1, sizeof a, e) : 0; if (e) fclose(e);
        W("EFI"); fat_lookup(&v, &root, w, &dir); W("BOOT"); fat_lookup(&v, &dir, w, &dir); W("BOOTX64.EFI");
        if (!n || fat_lookup(&v, &dir, w, &x) != FAT_OK || x.size != n || fat_read(&v, &x, 0, b, n) != (int)n || memcmp(a, b, n)) { fprintf(stderr, "FALHA: EFI/BOOT/BOOTX64.EFI diferente da origem\n"); fails++; }
        W("BOOTX64.EFI.~new"); if (fat_lookup(&v, &dir, w, &x) == FAT_OK) { fprintf(stderr, "FALHA: sobrou um .~new\n"); fails++; }
        if (f.kind == FOUND_ALONGSIDE && !f.bios) {
            static Fat ev; if (!fat_mount(&ev, dio, 0, f.esp_start)) return 2;
            FatNode d1, d2; int lk[3];
            fat_root(&ev, &root); W("EFI"); lk[0] = fat_lookup(&ev, &root, w, &d1); W("FolioOS"); lk[1] = fat_lookup(&ev, &d1, w, &d2); W("BOOTX64.EFI");
            lk[2] = fat_lookup(&ev, &d2, w, &x);
            int rd = lk[2] == FAT_OK ? fat_read(&ev, &x, 0, b, n) : -9;
            if (lk[0] || lk[1] || lk[2] || x.size != n || rd != (int)n || memcmp(a, b, n)) {
                fprintf(stderr, "FALHA: \\EFI\\FolioOS\\BOOTX64.EFI diferente da origem (busca %d %d %d, tamanho %u/%u, lidos %d)\n", lk[0], lk[1], lk[2], x.size, n, rd); fails++;
            }
        }
        #undef W
        fclose(img);
        if (fails) return 1;
        printf("atualizado (%s): programa igual ao da origem, texto e CONFIG.INI da pessoa intactos\n", f.kind == FOUND_WHOLE ? "disco inteiro" : "ao lado");
        return 0;
    }
    if (remove_mode) {
        /* desinstalar o que foi instalado ao lado: textos para a "origem", \EFI\FolioOS e a entrada da tabela somem,
         * o NTFS de antes (se houver) cresce de volta */
        static PartTable t0, t1; InstallFound f, g; int fails = 0;
        if (!part_read(&d, &t0) || !install_find(&d, &f)) { fprintf(stderr, "FALHA: nenhum FolioOS instalado ao lado neste disco\n"); fclose(img); return 1; }
        install_remove(&d, &s, &o, &f, true, true, &r);
        if (r.code) { fprintf(stderr, "FALHA: %s: %s\n", install_error(r.code), r.path); fclose(img); return 1; }
        if (f.kind == FOUND_WHOLE) {
            /* disco inteiro: fica vazio (sem tabela), e nada mais o reconhece como FolioOS */
            if (!part_read(&d, &t1) || t1.scheme != SCHEME_NONE) { fprintf(stderr, "FALHA: o disco devia ficar sem tabela\n"); fails++; }
            if (install_find(&d, &g)) { fprintf(stderr, "FALHA: o FolioOS ainda é reconhecido no disco\n"); fails++; }
        } else
        if (!part_read(&d, &t1) || t1.n != t0.n - 1) { fprintf(stderr, "FALHA: a tabela devia ter uma entrada a menos\n"); fails++; }
        for (int i = 0; i < t1.n; i++) if (f.bios ? t1.p[i].start == f.part_start : !memcmp(t1.p[i].guid, f.part_guid, 16)) { fprintf(stderr, "FALHA: a partição do FolioOS continua na tabela\n"); fails++; }
        if (install_find(&d, &g)) { fprintf(stderr, "FALHA: \\EFI\\FolioOS continua na partição EFI\n"); fails++; }
        if (f.ntfs >= 0) {
            static Ntfs n; bool ok = false;
            for (int i = 0; i < t1.n; i++)
                if (t1.p[i].start == f.ntfs_start)
                    ok = t1.p[i].count == f.ntfs_grow_to && (f.grow_fs != FS_NTFS ? fs_grow_max(&d, f.grow_fs, f.ntfs_start) > 0
                         : ntfs_open(&n, &d, f.ntfs_start) && n.nclus == (f.ntfs_grow_to - 1) / (n.cs / 512) && ntfs_flags(&n) == NTFS_DIRTY);
            if (!ok) { fprintf(stderr, "FALHA: o NTFS não cresceu de volta\n"); fails++; }
        }
        /* os textos estão na origem, um por um */
        int copied = 0;
        if (f.docs_files) {
            wchar_t w[1024]; full(r.path, w);
            wchar_t stack[16][1024]; int sp = 0; wcscpy(stack[sp++], w);
            while (sp) {
                wchar_t dir[1024]; wcscpy(dir, stack[--sp]); wchar_t pat[1100]; swprintf(pat, 1100, L"%ls\\*", dir);
                WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
                if (h == INVALID_HANDLE_VALUE) continue;
                do {
                    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (sp < 16) swprintf(stack[sp++], 1024, L"%ls\\%ls", dir, fd.cFileName); }
                    else copied++;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            if (copied != f.docs_files) { fprintf(stderr, "FALHA: %d textos copiados, esperava %d\n", copied, f.docs_files); fails++; }
        }
        fclose(img);
        if (fails) return 1;
        printf("desinstalado: %d textos copiados para %s; %s\n", copied, f.docs_files ? r.path : "-",
               f.ntfs >= 0 ? "a partição de antes cresceu de volta" : "o espaço ficou livre");
        return 0;
    }
    if (!alongside) {
        install_whole_disk(&d, &s, &o, &r);
        if (r.code) { fclose(img); fprintf(stderr, "FALHA: %s: %s\n", install_error(r.code), r.path); return 1; }
        {   /* onde ficam os textos: docs_place= no CONFIG.INI instalado; no pendrive, DOCUMENTS não vai para o disco */
            static Fat v; static char cfg[16384]; int fails = 0;
            const char *want = o.docs == INST_DOCS_STICK ? "docs_place=usb" : o.docs == INST_DOCS_BOTH ? "docs_place=both" : "docs_place=computer";
            FatNode root, dir, x; u16 w[32]; memset(cfg, 0, sizeof cfg);
            #define W2(s) do { int _i = 0; for (; (s)[_i]; _i++) w[_i] = (u16)(unsigned char)(s)[_i]; w[_i] = 0; } while (0)
            fat_mount(&v, dio, 0, r.part_lba); fat_root(&v, &root);
            W2("FOLIO"); fat_lookup(&v, &root, w, &dir); W2("CONFIG.INI");
            if (fat_lookup(&v, &dir, w, &x) != FAT_OK || fat_read(&v, &x, 0, cfg, sizeof cfg - 1) < 0 || !strstr(cfg, want)) { fprintf(stderr, "FALHA: o CONFIG.INI instalado não tem %s\n", want); fails++; }
            W2("DOCUMENTS"); bool docs = fat_lookup(&v, &root, w, &dir) == FAT_OK;
            if ((o.docs == INST_DOCS_STICK) == docs) { fprintf(stderr, "FALHA: DOCUMENTS %s no disco\n", docs ? "foi" : "não foi"); fails++; }
            #undef W2
            if (fails) { fclose(img); return 1; }
        }
        fclose(img);
        printf("instalado: partição em LBA %llu, %llu MB, %s\n", (unsigned long long)r.part_lba, (unsigned long long)(r.part_count / 2048), o.bios ? "MBR + boot BIOS" : "GPT (UEFI)");
        return 0;
    }
    /* ao lado: antes, o retrato de tudo o que existe (entradas da tabela e o conteúdo de cada partição que não é a EFI) */
    static PartTable before, after;
    if (!part_read(&d, &before)) { fprintf(stderr, "sem tabela\n"); return 2; }
    if (shrink) {
        /* sem espaço livre: encolhe o NTFS (o disco feito pelo Windows, tests\fixtures\ntfs-big) e instala depois dele */
        /* (também FAT32 e exFAT feitos pelo Windows: tests/fixtures/fat32-pc, exfat-pc) */
        int k = -1, fails = 0;
        for (int i = 0; i < before.n; i++) if (fs_resizable(before.p[i].fs) && memcmp(before.p[i].type, GPT_ESP, 16)) k = i;
        if (k < 0) { fprintf(stderr, "sem partição de dados\n"); return 2; }
        bool isntfs = before.p[k].fs == FS_NTFS;
        static Ntfs n; u64 last0 = 0;
        if (isntfs && (!ntfs_open(&n, &d, before.p[k].start) || !ntfs_last_used(&n, &last0))) { fprintf(stderr, "NTFS ilegível\n"); return 2; }
        u64 want = before.p[k].count - (600ull << 11);              /* 600 MiB para o FolioOS */
        install_shrink_alongside(&d, &s, &o, before.p[k].start, want, &r);
        if (r.code) { fprintf(stderr, "FALHA: %s: %s\n", install_error(r.code), r.path); fclose(img); return 1; }
        if (!part_read(&d, &after) || after.n != before.n + 1) { fprintf(stderr, "FALHA: a tabela devia ter uma entrada a mais\n"); fails++; }
        const Part *a = &before.p[k], *b = 0;
        for (int i = 0; i < after.n; i++) if (before.scheme == SCHEME_MBR ? after.p[i].start == a->start : !memcmp(after.p[i].guid, a->guid, 16)) b = &after.p[i];   /* MBR: sem GUID */
        u64 end = (a->start + want) / 2048 * 2048;
        if (!b || b->start != a->start || b->slot != a->slot || b->count != end - a->start || memcmp(b->type, a->type, 16)) { fprintf(stderr, "FALHA: entrada do NTFS\n"); fails++; }
        if (r.part_lba != end) { fprintf(stderr, "FALHA: o FolioOS devia começar onde o NTFS termina (%llu, não %llu)\n", (unsigned long long)end, (unsigned long long)r.part_lba); fails++; }
        u64 last1 = 0;
        if (isntfs ? (!ntfs_open(&n, &d, a->start) || n.nclus != (end - a->start - 1) / (n.cs / 512) || ntfs_flags(&n) != NTFS_DIRTY || !ntfs_last_used(&n, &last1) || last1 != last0)
                   : fs_grow_max(&d, a->fs, a->start) == 0) {   /* FAT32/exFAT: o volume encolhido tem de ser legível (o fsck vem depois) */
            fprintf(stderr, "FALHA: o NTFS encolhido não confere\n"); fails++;
        }
        fclose(img);
        if (fails) return 1;
        printf("instalado encolhendo a partição %s: ", part_fs_name(a->fs)); printf("%llu -> %llu MB, FolioOS %llu MB em LBA %llu, %s\n", (unsigned long long)(a->count / 2048),
               (unsigned long long)((end - a->start) / 2048), (unsigned long long)(r.part_count / 2048), (unsigned long long)r.part_lba, r.boot_path);
        return 0;
    }
    if (o.bios) {
        /* PC só BIOS (fase 3): as entradas de antes ficam nas mesmas posições e com os mesmos bytes, os dados delas não
         * mudam, o MBR passa a ser o do FolioOS apontando a entrada nova, e o código original vai para OTHERMBR.BIN */
        static u8 s0[512], s1[512], om[600], mbrc[440]; int fails = 0;
        dio(0, 0, 1, s0, false);
        u32 crc0[4] = { 0 }; for (int i = 0; i < before.n; i++) if (before.p[i].slot >= 0 && before.p[i].slot < 4) crc0[before.p[i].slot] = crc_range(before.p[i].start, before.p[i].count);
        Region fr[8]; int nf = part_free(&d, &before, fr, 8), big = -1;
        for (int i = 0; i < nf; i++) if (big < 0 || fr[i].count > fr[big].count) big = i;
        if (big < 0) { fprintf(stderr, "sem espaço livre\n"); return 2; }
        install_alongside(&d, &s, &o, fr[big], &r);
        if (r.code) { fprintf(stderr, "FALHA: %s: %s\n", install_error(r.code), r.path); fclose(img); return 1; }
        dio(0, 0, 1, s1, false);
        if (!part_read(&d, &after) || after.n != before.n + 1) { fprintf(stderr, "FALHA: a tabela devia ter uma entrada a mais\n"); fails++; }
        int slot = -1;
        for (int i = 0; i < after.n; i++) if (after.p[i].start == r.part_lba) slot = after.p[i].slot;
        for (int sl = 0; sl < 4; sl++) {
            const u8 *a = s0 + 446 + 16 * sl, *b = s1 + 446 + 16 * sl;
            if (a[4] && memcmp(a, b, 16)) { fprintf(stderr, "FALHA: a entrada %d do MBR mudou\n", sl + 1); fails++; }
            if (a[4] && crc_range(rd32(a + 8), rd32(a + 12)) != crc0[sl]) { fprintf(stderr, "FALHA: o conteúdo da partição %d mudou\n", sl + 1); fails++; }
        }
        if (s_read(0, "FOLIO/TOOLS/bios/mbr.bin", 0, mbrc, 440) != 440 || memcmp(s1, mbrc, 437) || s1[437] != slot) { fprintf(stderr, "FALHA: o MBR não é o do FolioOS apontando a entrada %d\n", slot + 1); fails++; }
        static Fat v; FatNode root, dir, x; u16 w[32];
        #define W3(str) do { int _i = 0; for (; (str)[_i]; _i++) w[_i] = (u16)(unsigned char)(str)[_i]; w[_i] = 0; } while (0)
        fat_mount(&v, dio, 0, r.part_lba); fat_root(&v, &root); W3("FOLIO"); fat_lookup(&v, &root, w, &dir); W3("OTHERMBR.BIN");
        int on = fat_lookup(&v, &dir, w, &x) == FAT_OK ? fat_read(&v, &x, 0, om, sizeof om) : -1;
        #undef W3
        if (on < 440 || memcmp(om, s0, 440)) { fprintf(stderr, "FALHA: FOLIO/OTHERMBR.BIN não guarda o MBR original\n"); fails++; }
        fclose(img);
        if (fails) return 1;
        printf("instalado ao lado (BIOS): partição %llu MB em LBA %llu, entrada %d no MBR; o MBR original guardado (%d bytes)\n",
               (unsigned long long)(r.part_count / 2048), (unsigned long long)r.part_lba, slot + 1, on);
        return 0;
    }
    u32 crcs[PART_MAX];
    static bool has_crc[PART_MAX];                       /* 0 é um CRC válido: "tem" fica à parte */
    for (int i = 0; i < before.n; i++) { has_crc[i] = memcmp(before.p[i].type, GPT_ESP, 16) != 0; crcs[i] = has_crc[i] ? crc_range(before.p[i].start, before.p[i].count) : 0; }
    Region fr[8]; int nf = part_free(&d, &before, fr, 8), big = -1;
    for (int i = 0; i < nf; i++) if (big < 0 || fr[i].count > fr[big].count) big = i;
    if (big < 0) { fprintf(stderr, "sem espaço livre\n"); return 2; }
    install_alongside(&d, &s, &o, fr[big], &r);
    if (r.code) { fprintf(stderr, "FALHA: %s: %s\n", install_error(r.code), r.path); fclose(img); return 1; }
    int fails = 0;
    if (!part_read(&d, &after) || after.n != before.n + 1) { fprintf(stderr, "FALHA: a tabela devia ter uma entrada a mais\n"); fails++; }
    for (int i = 0; i < before.n; i++) {
        const Part *a = &before.p[i], *b = 0;
        for (int k = 0; k < after.n; k++) if (!memcmp(after.p[k].guid, a->guid, 16)) b = &after.p[k];
        if (!b || b->slot != a->slot || b->start != a->start || b->count != a->count || memcmp(b->type, a->type, 16) || memcmp(b->name, a->name, sizeof a->name) || b->attrs != a->attrs) {
            fprintf(stderr, "FALHA: a entrada %d da tabela mudou\n", a->slot + 1); fails++; continue;
        }
        if (has_crc[i] && crc_range(a->start, a->count) != crcs[i]) { fprintf(stderr, "FALHA: o conteúdo da partição %d mudou\n", a->slot + 1); fails++; }
    }
    fclose(img);
    if (fails) return 1;
    printf("instalado ao lado: partição %llu MB em LBA %llu, %s na partição EFI %d; %d partições existentes intactas\n",
           (unsigned long long)(r.part_count / 2048), (unsigned long long)r.part_lba, r.boot_path, r.esp_num, before.n);
    return 0;
}
