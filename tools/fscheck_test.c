/* FolioOS — teste da verificação de partições de outros sistemas (src/core/fscheck.c).
 *   fscheck_test.exe <pasta com ntfs-*.img> <pasta com ntfs-*.txt>
 * 1. NTFS feito pelo próprio Windows (tools/ntfs-fixtures.ps1): o leitor tem de concordar com o que o
 *    Windows disse (tamanho do cluster, total de clusters, início da MFT, último cluster em uso, sujo,
 *    hibernado). Controles negativos: as mesmas imagens com um setor da MFT estragado, ou com o tamanho de
 *    setor trocado, têm de dar "ilegível", nunca "pode mexer".
 * 2. Imagens sintéticas para os outros vereditos (BitLocker, LUKS, LVM, APFS, HFS+, ext4 limpo/diário
 *    pendente/com erros, também em disco de setor de 4 KiB, FAT32, XFS, NTFS sem estrutura). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core/part.h"
#include "../src/core/fscheck.h"
#include "../src/core/fat32.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALHA (linha %d): ", __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fails++; } } while (0)

typedef struct { u8 *p; u64 size; u32 bs; } Ram;
static bool ram_rw(void *ctx, u64 lba, u32 n, void *buf, bool w) {
    Ram *r = ctx;
    if ((lba + n) * r->bs > r->size) return false;
    if (w) memcpy(r->p + lba * r->bs, buf, (size_t)n * r->bs); else memcpy(buf, r->p + lba * r->bs, (size_t)n * r->bs);
    return true;
}
static BlkDev ram_dev(Ram *r) { BlkDev d = { ram_rw, r, r->size / r->bs, r->bs, 0 }; return d; }
static void wr16(u8 *p, uint16_t v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> 8 * i); }

/* a partição NTFS da imagem (a única) */
static int ntfs_part(const BlkDev *d, PartTable *t) {
    if (!part_read(d, t)) return -1;
    int k = -1;
    for (int i = 0; i < t->n; i++) if (t->p[i].fs == FS_NTFS) { if (k >= 0) return -1; k = i; }
    return k;
}

static void windows_fixture(const char *imgdir, const char *txtdir, const char *name) {
    char path[600];
    snprintf(path, sizeof path, "%s\\ntfs-%s.txt", txtdir, name);
    FILE *f = fopen(path, "r"); if (!f) { CHECK(0, "sem %s", path); return; }
    int flags; unsigned long long cs, nclus, last, mft, bytes;
    int got = fscanf(f, "flags=%d cs=%llu nclus=%llu last=%llu mft=%llu bytes=%llu", &flags, &cs, &nclus, &last, &mft, &bytes);
    fclose(f);
    if (got != 6) { CHECK(0, "%s: formato", path); return; }
    snprintf(path, sizeof path, "%s\\ntfs-%s.img", imgdir, name);
    f = fopen(path, "rb"); if (!f) { CHECK(0, "sem %s", path); return; }
    Ram r = { malloc(bytes), bytes, 512 };
    got = (int)fread(r.p, 1, bytes, f); fclose(f);
    if ((u64)got != bytes) { CHECK(0, "%s: %d bytes, esperava %llu", path, got, bytes); free(r.p); return; }
    BlkDev d = ram_dev(&r);
    static PartTable t;
    int k = ntfs_part(&d, &t);
    CHECK(k >= 0, "%s: partição NTFS não encontrada", name);
    if (k < 0) { free(r.p); return; }
    Part *p = &t.p[k];

    static Ntfs n;
    CHECK(ntfs_open(&n, &d, p->start), "%s: ntfs_open", name);
    CHECK(n.cs == cs && n.nclus == nclus && n.mft_lcn == mft, "%s: cluster %u/%llu, total %llu/%llu, MFT %llu/%llu",
          name, n.cs, cs, (unsigned long long)n.nclus, nclus, (unsigned long long)n.mft_lcn, mft);
    int fl = ntfs_flags(&n);
    CHECK(fl == flags, "%s: flags %d, Windows disse %d", name, fl, flags);
    u64 lu = 0;
    CHECK(ntfs_last_used(&n, &lu) && lu == last, "%s: último cluster usado %llu, Windows disse %llu", name, (unsigned long long)lu, last);

    FsCheck c; fs_check(&d, p, &c);
    int want = flags & NTFS_HIBERNATED ? SAFE_HIBERNATED : flags & NTFS_DIRTY ? SAFE_DIRTY : SAFE_OK;
    CHECK(c.verdict == want && c.fs == FS_NTFS, "%s: veredito %d, esperava %d", name, c.verdict, want);
    if (want == SAFE_OK) CHECK(c.min_bytes == (last + 1) * cs + 512, "%s: min_bytes %llu", name, (unsigned long long)c.min_bytes);
    printf("  %-6s Windows: flags=%d último=%llu de %llu  |  FolioOS: veredito \"%s\", mínimo %llu KiB\n", name, flags, last, nclus,
           c.verdict == SAFE_OK ? "ok" : fs_safety_short(c.verdict), (unsigned long long)c.min_bytes / 1024);

    /* controle negativo 1: o fim do 1º setor do registro 3 ($Volume) deixa de bater com o número da sequência */
    u64 at = (p->start * 512) + n.mft_lcn * n.cs + 3ull * n.rec + 510;
    u8 save = r.p[at]; r.p[at] ^= 0x5A;
    fs_check(&d, p, &c);
    CHECK(c.verdict == SAFE_UNREADABLE, "%s: $Volume estragado deu veredito %d", name, c.verdict);
    r.p[at] = save;
    /* controle negativo 2: o $Bitmap ($MFT registro 6) estragado: não pode inventar um mínimo */
    if (want == SAFE_OK) {
        at = (p->start * 512) + n.mft_lcn * n.cs + 6ull * n.rec + 1022;
        save = r.p[at]; r.p[at] ^= 0x5A;
        fs_check(&d, p, &c);
        CHECK(c.verdict == SAFE_UNREADABLE && c.min_bytes == 0, "%s: $Bitmap estragado deu veredito %d", name, c.verdict);
        r.p[at] = save;
    }
    /* controle negativo 3: setor de 4096 no cabeçalho, disco de 512 */
    u8 *bpb = r.p + p->start * 512;
    wr16(bpb + 0x0B, 4096);
    fs_check(&d, p, &c);
    CHECK(c.verdict == SAFE_UNREADABLE, "%s: setor trocado deu veredito %d", name, c.verdict);
    wr16(bpb + 0x0B, 512);
    fs_check(&d, p, &c);
    CHECK(c.verdict == want, "%s: depois de desfazer os estragos, veredito %d", name, c.verdict);

    /* encolher: recusado em volume que não está em ordem e abaixo do último cluster usado, sem mudar um
     * byte; aceito no limpo, e o resultado relido tem de bater */
    u8 *orig = malloc(bytes); memcpy(orig, r.p, bytes);
    u64 minsec = (last + 1) * (cs / 512) + 1;
    if (want != SAFE_OK) {
        int rc = ntfs_shrink(&d, p->start, p->count, minsec + 2048);
        CHECK(rc == NTFS_SHRINK_NOT_CLEAN, "%s: encolher deveria ser recusado (rc %d)", name, rc);
    } else {
        int rc = ntfs_shrink(&d, p->start, p->count, minsec - (cs / 512));
        CHECK(rc == NTFS_SHRINK_TOO_SMALL, "%s: encolher abaixo do último cluster usado (rc %d)", name, rc);
        rc = ntfs_shrink(&d, p->start, p->count, p->count);
        CHECK(rc == NTFS_SHRINK_BAD_SIZE, "%s: \"encolher\" para o mesmo tamanho (rc %d)", name, rc);
    }
    CHECK(!memcmp(orig, r.p, bytes), "%s: uma recusa mudou bytes do disco", name);
    if (want == SAFE_OK) {
        u64 ns = minsec + 1000;                                  /* não precisa ser múltiplo do cluster */
        int rc = ntfs_shrink(&d, p->start, p->count, ns);
        CHECK(rc == NTFS_SHRINK_OK, "%s: encolher para %llu setores (rc %d)", name, (unsigned long long)ns, rc);
        u64 ncl = (ns - 1) / (cs / 512);
        CHECK(ntfs_open(&n, &d, p->start) && n.nclus == ncl, "%s: depois de encolher, %llu clusters (esperava %llu)", name, (unsigned long long)n.nclus, (unsigned long long)ncl);
        CHECK(ntfs_flags(&n) == NTFS_DIRTY, "%s: depois de encolher, o volume tem de ficar marcado para o chkdsk", name);
        CHECK(ntfs_log_clean(&n), "%s: diário", name);
        CHECK(ntfs_last_used(&n, &lu) && lu == last, "%s: depois de encolher, último usado %llu", name, (unsigned long long)lu);
        CHECK(!memcmp(r.p + p->start * 512, r.p + (p->start + ns - 1) * 512, 512), "%s: cópia do setor de boot no novo fim", name);
        /* nada depois do novo fim mudou, fora o setor da cópia; e o $MFTMirr bate com a MFT nos registros 0-3 */
        u64 a = (p->start + ns) * 512;
        CHECK(!memcmp(orig + a, r.p + a, (size_t)(bytes - a)), "%s: bytes depois do novo fim mudaram", name);
        u8 *bpb2 = r.p + p->start * 512;
        u64 mirr = 0; for (int i = 0; i < 8; i++) mirr |= (u64)bpb2[0x38 + i] << 8 * i;
        CHECK(!memcmp(r.p + p->start * 512 + n.mft_lcn * cs, r.p + p->start * 512 + mirr * cs, 4 * n.rec), "%s: $MFTMirr diferente da MFT", name);
        printf("  %-6s encolhido de %llu para %llu setores (%llu clusters), marcado para chkdsk\n", name,
               (unsigned long long)p->count, (unsigned long long)ns, (unsigned long long)ncl);

        /* crescer de volta ao tamanho original: o volume tem de voltar byte a byte ao que era, fora os registros
         * 3, 6 e 8 da MFT (e o espelho do 3: número de sequência e a marca de "sujo") e a cópia do boot que ficou
         * no fim intermediário (espaço livre) */
        CHECK(ntfs_grow_max(&d, p->start) >= p->count, "%s: o $Bitmap não cobre o tamanho original", name);
        CHECK(ntfs_grow(&d, p->start, ns, p->count) == NTFS_SHRINK_BAD_SIZE, "%s: crescer além da partição", name);
        int rc2 = ntfs_grow(&d, p->start, p->count, p->count);
        CHECK(rc2 == NTFS_SHRINK_OK, "%s: crescer de volta (rc %d)", name, rc2);
        CHECK(ntfs_open(&n, &d, p->start) && n.nclus == nclus && ntfs_last_used(&n, &lu) && lu == last, "%s: depois de crescer, %llu clusters", name, (unsigned long long)n.nclus);
        u64 vol0 = p->start * 512, rs = n.rec;
        u64 skip[5][2] = { { vol0 + n.mft_lcn * cs + 3 * rs, rs }, { vol0 + n.mft_lcn * cs + 6 * rs, rs }, { vol0 + n.mft_lcn * cs + 8 * rs, rs },
                           { vol0 + mirr * cs + 3 * rs, rs }, { (p->start + ns - 1) * 512, 512 } };
        u64 diff = 0, first_diff = 0;
        for (u64 i = 0; i < bytes; i++) {
            if (orig[i] == r.p[i]) continue;
            bool ok = false; for (int k = 0; k < 5 && !ok; k++) ok = i >= skip[k][0] && i < skip[k][0] + skip[k][1];
            if (!ok) { if (!diff) first_diff = i; diff++; }
        }
        CHECK(!diff, "%s: depois de crescer, %llu bytes diferentes do original (o 1º em %llu)", name, (unsigned long long)diff, (unsigned long long)first_diff);
        if (!diff) printf("  %-6s crescido de volta: igual ao original, fora a marca para o chkdsk\n", name);
    }
    free(orig);
    free(r.p);
}

/* fscheck_test --shrink <imagem> <MiB>: encolhe a partição NTFS da imagem (o volume e a entrada da tabela),
 * para o tools\ntfs-shrink-check.ps1 conferir o resultado com o chkdsk do próprio Windows */
static int shrink_image(const char *path, unsigned long long mib) {
    FILE *f = fopen(path, "r+b"); if (!f) { perror(path); return 2; }
    _fseeki64(f, 0, SEEK_END); u64 bytes = (u64)_ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    Ram r = { malloc(bytes), bytes, 512 };
    if (fread(r.p, 1, bytes, f) != bytes) { fprintf(stderr, "ler %s\n", path); return 2; }
    BlkDev d = ram_dev(&r);
    static PartTable t;
    int k = ntfs_part(&d, &t);
    if (k < 0) { fprintf(stderr, "sem partição NTFS\n"); return 2; }
    u64 ns = (u64)mib * 2048;
    int rc = ntfs_shrink(&d, t.p[k].start, t.p[k].count, ns);
    if (rc != NTFS_SHRINK_OK) { fprintf(stderr, "ntfs_shrink: %d\n", rc); return 1; }
    t.p[k].count = ns;
    if (!part_write(&d, &t)) { fprintf(stderr, "part_write\n"); return 1; }
    _fseeki64(f, 0, SEEK_SET);
    if (fwrite(r.p, 1, bytes, f) != bytes) { fprintf(stderr, "gravar\n"); return 1; }
    fclose(f);
    printf("NTFS encolhido para %llu MiB\n", mib);
    return 0;
}

/* ext4 feito pelo próprio Linux (tools/ext-fixtures.ps1, Alpine no QEMU): o veredito tem de bater com o estado
 * que o Linux deixou (limpo, com erros, diário pendente), com blocos de 4 KiB e de 1 KiB */
static void linux_fixture(const char *imgdir, const char *txtdir, const char *name, int want) {
    char path[600];
    snprintf(path, sizeof path, "%s\\ext-%s.txt", txtdir, name);
    FILE *f = fopen(path, "r"); if (!f) { CHECK(0, "sem %s", path); return; }
    char line[1024] = ""; if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f);
    const char *pb = strstr(line, "bytes="), *pk = strstr(line, " bs=");
    if (!pb || !pk) { CHECK(0, "%s: formato", path); return; }
    u64 bytes = strtoull(pb + 6, 0, 10); unsigned bs = (unsigned)strtoul(pk + 4, 0, 10);
    snprintf(path, sizeof path, "%s\\ext-%s.img", imgdir, name);
    f = fopen(path, "rb"); if (!f) { CHECK(0, "sem %s", path); return; }
    Ram r = { malloc(bytes), bytes, 512 };
    size_t got = fread(r.p, 1, bytes, f); fclose(f);
    if (got != bytes) { CHECK(0, "%s: %zu bytes", path, got); free(r.p); return; }
    BlkDev d = ram_dev(&r);
    static PartTable t;
    int k = -1;
    if (part_read(&d, &t)) for (int i = 0; i < t.n; i++) if (t.p[i].fs == FS_EXT) k = i;
    CHECK(k >= 0, "ext-%s: partição ext4 não encontrada", name);
    if (k >= 0) {
        CHECK(!strcmp(t.p[k].label, "FOLIOTEST"), "ext-%s: rótulo \"%s\"", name, t.p[k].label);
        FsCheck c; fs_check(&d, &t.p[k], &c);
        CHECK(c.verdict == want, "ext-%s (blocos de %u): veredito %d, esperava %d", name, bs, c.verdict, want);
        printf("  ext-%-7s Linux: blocos de %u  |  FolioOS: veredito \"%s\"\n", name, bs, c.verdict == SAFE_OK ? "ok" : fs_safety_short(c.verdict));
    }
    free(r.p);
}

/* ---- sintéticas: um disco GPT com uma partição e só a assinatura (e o que o veredito lê) */
typedef void (*Fill)(u8 *part, u32 bs);
static void f_bitlocker(u8 *b, u32 bs) { (void)bs; memcpy(b + 3, "-FVE-FS-", 8); }
static void f_luks(u8 *b, u32 bs) { (void)bs; memcpy(b, "LUKS\xBA\xBE", 6); }
static void f_lvm(u8 *b, u32 bs) { (void)bs; memcpy(b + 512, "LABELONE", 8); }
static void f_apfs(u8 *b, u32 bs) { (void)bs; memcpy(b + 32, "NXSB", 4); }
static void f_hfsp(u8 *b, u32 bs) { (void)bs; b[1024] = 'H'; b[1025] = '+'; }
static void ext(u8 *b, uint16_t state, u32 incompat) { u8 *sb = b + 1024; wr16(sb + 0x38, 0xEF53); wr16(sb + 0x3A, state); wr32(sb + 0x60, incompat); memcpy(sb + 120, "linux", 5); }
static void f_ext_ok(u8 *b, u32 bs) { (void)bs; ext(b, 1, 0x2C2); }                 /* extents, flex_bg, 64bit, filetype */
static void f_ext_recover(u8 *b, u32 bs) { (void)bs; ext(b, 1, 0x2C2 | 4); }
static void f_ext_errors(u8 *b, u32 bs) { (void)bs; ext(b, 3, 0x2C2); }             /* limpo, mas com erros */
/* FAT32/FAT16: BPB com 32 setores reservados e a FAT logo depois; a entrada 1 diz se o volume foi desligado limpo */
static void fat(u8 *b, u32 bs, bool f32, u32 e1, u8 state) {
    wr16(b + 0x0B, (uint16_t)bs); b[0x0D] = 8; wr16(b + 0x0E, 32); b[0x10] = 2; b[510] = 0x55; b[511] = 0xAA;
    u8 *ft = b + 32 * bs;
    if (f32) { memcpy(b + 82, "FAT32   ", 8); b[0x41] = state; wr32(ft, 0x0FFFFFF8); wr32(ft + 4, e1); }
    else { memcpy(b + 54, "FAT16   ", 8); b[0x25] = state; wr16(ft, 0xFFF8); wr16(ft + 2, (uint16_t)e1); }
}
static void f_fat32(u8 *b, u32 bs) { fat(b, bs, true, 0x0FFFFFFF, 0); }
static void f_fat32_unclean(u8 *b, u32 bs) { fat(b, bs, true, 0x07FFFFFF, 0); }   /* sem "desligado limpo" */
static void f_fat32_errors(u8 *b, u32 bs) { fat(b, bs, true, 0x0BFFFFFF, 0); }    /* sem "sem erros" */
static void f_fat32_bpb(u8 *b, u32 bs) { fat(b, bs, true, 0x0FFFFFFF, 1); }       /* byte de estado do BPB */
static void f_fat16(u8 *b, u32 bs) { fat(b, bs, false, 0xFFFF, 0); }
static void f_fat16_unclean(u8 *b, u32 bs) { fat(b, bs, false, 0x7FFF, 0); }
static void f_exfat(u8 *b, u32 bs) { (void)bs; memcpy(b + 3, "EXFAT   ", 8); }
static void f_exfat_dirty(u8 *b, u32 bs) { (void)bs; memcpy(b + 3, "EXFAT   ", 8); wr16(b + 0x6A, 0x0002); }
static void f_xfs(u8 *b, u32 bs) { (void)bs; memcpy(b, "XFSB", 4); }
static void f_ntfs_empty(u8 *b, u32 bs) { memcpy(b + 3, "NTFS    ", 8); wr16(b + 0x0B, (uint16_t)bs); b[0x0D] = 8; }

static void synthetic(const char *name, u32 bs, Fill fill, int want_fs, int want) {
    Ram r = { calloc(1, 16 << 20), 16 << 20, bs };
    BlkDev d = ram_dev(&r);
    static PartTable t;
    part_new(&t, SCHEME_GPT, 777);
    u64 start = (1 << 20) / bs, count = (8 << 20) / bs;
    CHECK(part_add(&d, &t, start, count, GPT_MSDATA, 0x07, "test", 1) >= 0 && part_write(&d, &t), "%s: criar tabela", name);
    fill(r.p + start * bs, bs);
    CHECK(part_read(&d, &t) && t.n == 1, "%s: reler tabela", name);
    FsCheck c; fs_check(&d, &t.p[0], &c);
    CHECK(t.p[0].fs == want_fs && c.verdict == want, "%s (setor %u): sistema %s, veredito %d; esperava %s, %d", name, bs,
          part_fs_name(t.p[0].fs), c.verdict, part_fs_name(want_fs), want);
    CHECK((want == SAFE_OK) == !fs_safety_text(c.verdict)[0], "%s: texto do veredito", name);
    free(r.p);
}

/* fscheck_test --shrunk <imagem> <MiB antes>: depois do instalador encolher o NTFS (pela tela): a partição ficou
 * menor, o volume cabe nela (a cópia do setor de boot no último setor), está marcado para o chkdsk, e a
 * partição seguinte (a do FolioOS) começa logo depois */
static int check_shrunk(const char *path, unsigned long long mib_before) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return 2; }
    _fseeki64(f, 0, SEEK_END); u64 bytes = (u64)_ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    Ram r = { malloc(bytes), bytes, 512 };
    if (fread(r.p, 1, bytes, f) != bytes) { fprintf(stderr, "ler %s\n", path); return 2; }
    fclose(f);
    BlkDev d = ram_dev(&r);
    static PartTable t; static Ntfs n;
    int k = ntfs_part(&d, &t);
    CHECK(k >= 0, "sem partição NTFS");
    if (k < 0) return 1;
    Part *p = &t.p[k];
    u64 lu;
    CHECK(p->count < mib_before * 2048, "o NTFS não diminuiu (%llu setores)", (unsigned long long)p->count);
    CHECK(ntfs_open(&n, &d, p->start) && n.nclus == (p->count - 1) / (n.cs / 512), "volume e partição não batem");
    CHECK(!memcmp(r.p + p->start * 512, r.p + (p->start + p->count - 1) * 512, 512), "sem a cópia do setor de boot no fim");
    CHECK(ntfs_flags(&n) == NTFS_DIRTY && ntfs_last_used(&n, &lu), "o volume devia estar marcado para o chkdsk");
    bool next = false;
    for (int i = 0; i < t.n; i++) if (t.p[i].start == p->start + p->count && t.p[i].fs == FS_FAT32) next = true;
    CHECK(next, "a partição do FolioOS não começa onde o NTFS termina");
    if (fails) return 1;
    printf("NTFS encolhido pela tela: %llu -> %llu MiB, FolioOS logo depois\n", mib_before, (unsigned long long)(p->count / 2048));
    return 0;
}

/* fscheck_test --fat32 <imagem>: o FAT32 feito pelo Windows (tests/fixtures/fat32-data), direto no arquivo:
 * recusa abaixo do mínimo sem mudar nada; encolhe até perto do mínimo (todos os arquivos com o mesmo CRC); cresce de
 * volta ao tamanho original (o volume volta byte a byte, fora o FSInfo) */
static FILE *ff;
static bool file_rw(void *ctx, u64 lba, u32 n, void *buf, bool w) {
    (void)ctx;
    if (_fseeki64(ff, (long long)(lba * 512), SEEK_SET)) return false;
    return w ? fwrite(buf, 512, n, ff) == n : fread(buf, 512, n, ff) == n;
}
static bool ff_io(void *ctx, u64 lba, u32 n, void *buf, bool w) { return file_rw(ctx, lba, n, buf, w); }
typedef struct { char path[300]; u32 crc, size; } FEnt;
static FEnt fents[2000]; static int nfe;
static u8 fbuf[1 << 22];
static void fat_walk(Fat *v, FatNode *dir, const char *rel, bool record, int *bad) {
    u32 ck = 0; u16 nm[256]; FatNode n;
    while (fat_readdir(v, dir, &ck, nm, &n) == 1) {
        char name[256]; int k = 0; for (; nm[k] && k < 255; k++) name[k] = nm[k] < 128 ? (char)nm[k] : '?'; name[k] = 0;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char p[300]; snprintf(p, sizeof p, "%s/%s", rel, name);
        if (n.attr & 0x10) { fat_walk(v, &n, p, record, bad); continue; }
        u32 got = n.size <= sizeof fbuf && fat_read(v, &n, 0, fbuf, n.size) == (int)n.size ? crc32_buf(0, fbuf, n.size) : 0xFFFFFFFFu;
        if (record) { if (nfe < 2000) { snprintf(fents[nfe].path, sizeof fents[nfe].path, "%s", p); fents[nfe].crc = got; fents[nfe].size = n.size; nfe++; } }
        else { bool ok = false; for (int i = 0; i < nfe; i++) if (!strcmp(fents[i].path, p)) ok = fents[i].crc == got && fents[i].size == n.size; if (!ok) { fprintf(stderr, "  diferente: %s\n", p); (*bad)++; } }
    }
}
static int files_check(u64 lba, bool record) {
    static Fat v; int bad = 0;
    if (!fat_mount(&v, ff_io, 0, lba)) return -1;
    FatNode root; fat_root(&v, &root);
    if (record) nfe = 0;
    fat_walk(&v, &root, "", record, &bad);
    return bad;
}
static int fat32_cycle(const char *path) {
    ff = fopen(path, "r+b"); if (!ff) { perror(path); return 2; }
    _fseeki64(ff, 0, SEEK_END); u64 bytes = (u64)_ftelli64(ff);
    BlkDev d = { file_rw, 0, bytes / 512, 512, 0 };
    static PartTable t; int k = -1;
    if (part_read(&d, &t)) for (int i = 0; i < t.n; i++) if (t.p[i].fs == FS_FAT32) k = i;
    if (k < 0) { fprintf(stderr, "sem FAT32\n"); return 2; }
    Part *p = &t.p[k];
    static u8 b0[512]; d.rw(0, p->start, 1, b0, false);
    u32 total0 = b0[32] | b0[33] << 8 | b0[34] << 16 | (u32)b0[35] << 24;
    CHECK(files_check(p->start, true) == 0 && nfe > 0, "fat32: ler os arquivos (%d)", nfe);
    u64 mn = fat32_min_sectors(&d, p->start), mx = fat32_max_sectors(&d, p->start);
    CHECK(mn > 0 && mn < total0 && mx >= total0, "fat32: mínimo %llu, máximo %llu, total %u", (unsigned long long)mn, (unsigned long long)mx, total0);
    /* guarda o volume inteiro para comparar no fim (fora o FSInfo) */
    u64 vb = (u64)total0 * 512; u8 *orig = malloc(vb);
    _fseeki64(ff, (long long)(p->start * 512), SEEK_SET); if (fread(orig, 1, vb, ff) != vb) { fprintf(stderr, "ler\n"); return 2; }
    int rc = fat32_resize(&d, p->start, p->count, mn - 1);
    CHECK(rc == NTFS_SHRINK_TOO_SMALL || rc == NTFS_SHRINK_BAD_SIZE, "fat32: abaixo do mínimo devia ser recusado (rc %d)", rc);
    u64 ns = mn + 8 * 1024;                                    /* um pouco acima do mínimo */
    rc = fat32_resize(&d, p->start, p->count, ns);
    CHECK(rc == NTFS_SHRINK_OK, "fat32: encolher para %llu setores (rc %d)", (unsigned long long)ns, rc);
    static Fat v; CHECK(fat_mount(&v, ff_io, 0, p->start), "fat32: montar depois de encolher");
    int bad = files_check(p->start, false);
    CHECK(bad == 0, "fat32: %d arquivos diferentes depois de encolher", bad);
    printf("  fat32  encolhido de %u para %llu setores (%u clusters), %d arquivos iguais\n", total0, (unsigned long long)ns, v.nclus, nfe);
    rc = fat32_resize(&d, p->start, p->count, total0);
    CHECK(rc == NTFS_SHRINK_OK, "fat32: crescer de volta (rc %d)", rc);
    u8 *now = malloc(vb);
    _fseeki64(ff, (long long)(p->start * 512), SEEK_SET); if (fread(now, 1, vb, ff) != vb) return 2;
    u32 fsi = b0[48] | b0[49] << 8, bk = b0[50] | b0[51] << 8; u64 diffs = 0, first = 0;
    for (u64 s = 0; s < vb / 512; s++) {
        if (s == fsi || (bk && s == bk + fsi)) continue;
        if (memcmp(orig + s * 512, now + s * 512, 512)) { if (!diffs) first = s; diffs++; }
    }
    CHECK(!diffs, "fat32: depois de crescer, %llu setores diferentes do original (o 1º: %llu)", (unsigned long long)diffs, (unsigned long long)first);
    if (!diffs) printf("  fat32  crescido de volta: igual ao original, fora o FSInfo\n");
    free(orig); free(now); fclose(ff);
    return fails ? 1 : 0;
}

/* fscheck_test --exfat <imagem>: o exFAT feito pelo Windows (tests/fixtures/exfat-data): recusa abaixo do mínimo sem
 * mudar nada; encolhe; cresce de volta; o volume volta byte a byte fora as regiões de boot (PercentInUse e a soma) */
static int exfat_cycle(const char *path) {
    ff = fopen(path, "r+b"); if (!ff) { perror(path); return 2; }
    _fseeki64(ff, 0, SEEK_END); u64 bytes = (u64)_ftelli64(ff);
    BlkDev d = { file_rw, 0, bytes / 512, 512, 0 };
    static PartTable t; int k = -1;
    if (part_read(&d, &t)) for (int i = 0; i < t.n; i++) if (t.p[i].fs == FS_EXFAT) k = i;
    if (k < 0) { fprintf(stderr, "sem exFAT\n"); return 2; }
    Part *p = &t.p[k];
    static u8 b0[512]; d.rw(0, p->start, 1, b0, false);
    u64 len0 = 0; for (int i = 0; i < 8; i++) len0 |= (u64)b0[0x48 + i] << (8 * i);
    u64 mn = exfat_min_sectors(&d, p->start), mx = exfat_max_sectors(&d, p->start);
    CHECK(mn > 0 && mn < len0 && mx >= len0, "exfat: mínimo %llu, máximo %llu, total %llu", (unsigned long long)mn, (unsigned long long)mx, (unsigned long long)len0);
    u64 vb = len0 * 512; u8 *orig = malloc(vb), *now = malloc(vb);
    _fseeki64(ff, (long long)(p->start * 512), SEEK_SET); if (fread(orig, 1, vb, ff) != vb) return 2;
    int rc = exfat_resize(&d, p->start, p->count, mn - 8);
    CHECK(rc == NTFS_SHRINK_TOO_SMALL, "exfat: abaixo do mínimo devia ser recusado (rc %d)", rc);
    _fseeki64(ff, (long long)(p->start * 512), SEEK_SET); if (fread(now, 1, vb, ff) != vb) return 2;
    CHECK(!memcmp(orig, now, vb), "exfat: uma recusa mudou bytes");
    u64 ns = mn + 64 * 1024;
    rc = exfat_resize(&d, p->start, p->count, ns);
    CHECK(rc == NTFS_SHRINK_OK, "exfat: encolher para %llu setores (rc %d)", (unsigned long long)ns, rc);
    CHECK(exfat_min_sectors(&d, p->start) == mn, "exfat: o mínimo mudou depois de encolher");
    printf("  exfat  encolhido de %llu para %llu setores\n", (unsigned long long)len0, (unsigned long long)ns);
    rc = exfat_resize(&d, p->start, p->count, len0);
    CHECK(rc == NTFS_SHRINK_OK, "exfat: crescer de volta (rc %d)", rc);
    _fseeki64(ff, (long long)(p->start * 512), SEEK_SET); if (fread(now, 1, vb, ff) != vb) return 2;
    u64 diffs = 0, first = 0;
    for (u64 s = 24; s < vb / 512; s++) if (memcmp(orig + s * 512, now + s * 512, 512)) { if (!diffs) first = s; diffs++; }
    CHECK(!diffs, "exfat: depois de crescer, %llu setores diferentes (o 1º: %llu)", (unsigned long long)diffs, (unsigned long long)first);
    CHECK(!memcmp(orig + 0x48, now + 0x48, 0x70 - 0x48), "exfat: o setor de boot não voltou ao original");
    if (!diffs) printf("  exfat  crescido de volta: igual ao original, fora as regiões de boot\n");
    free(orig); free(now); fclose(ff);
    return fails ? 1 : 0;
}

/* fscheck_test --resize <imagem> <MiB>: a partição de dados (NTFS ou FAT32) passa a ter MiB, com o volume junto
 * (encolher: o volume e depois a entrada da tabela; crescer: a entrada e depois o volume), direto no arquivo; para o
 * tools\ntfs-shrink-check.ps1 conferir com o chkdsk do próprio Windows */
static int resize_image(const char *path, unsigned long long mib) {
    ff = fopen(path, "r+b"); if (!ff) { perror(path); return 2; }
    _fseeki64(ff, 0, SEEK_END); u64 bytes = (u64)_ftelli64(ff);
    BlkDev d = { file_rw, 0, bytes / 512, 512, 0 };
    static PartTable t; int k = -1;
    if (part_read(&d, &t)) for (int i = 0; i < t.n; i++) if ((t.p[i].fs == FS_NTFS || t.p[i].fs == FS_FAT32 || t.p[i].fs == FS_EXFAT) && memcmp(t.p[i].type, GPT_ESP, 16)) k = i;
    if (k < 0) { fprintf(stderr, "sem partição NTFS/FAT32\n"); return 2; }
    Part *p = &t.p[k]; u64 ns = (u64)mib * 2048; bool grow = ns > p->count; int rc;
    if (grow) {
        if (part_overlaps(&d, &t, p->start, ns, k)) { fprintf(stderr, "não há espaço livre depois da partição\n"); return 1; }
        p->count = ns; if (!part_write(&d, &t)) { fprintf(stderr, "part_write\n"); return 1; }
    }
    rc = p->fs == FS_NTFS ? (grow ? ntfs_grow(&d, p->start, ns, ns) : ntfs_shrink(&d, p->start, p->count, ns)) : p->fs == FS_EXFAT ? exfat_resize(&d, p->start, grow ? ns : p->count, ns) : fat32_resize(&d, p->start, grow ? ns : p->count, ns);
    if (rc != NTFS_SHRINK_OK) { fprintf(stderr, "redimensionar: %d\n", rc); return 1; }
    if (!grow) { p->count = ns; if (!part_write(&d, &t)) { fprintf(stderr, "part_write\n"); return 1; } }
    fclose(ff);
    printf("%s %s para %llu MiB\n", p->fs == FS_NTFS ? "NTFS" : p->fs == FS_EXFAT ? "exFAT" : "FAT32", grow ? "crescido" : "encolhido", mib);
    return 0;
}

/* fscheck_test --mkgpt <imagem> <MiB>: disco novo com GPT e uma partição Linux do 1 MiB ao fim (o Linux formata) */
static Ram *mk_ram;
static bool mk_io(void *ctx, u64 lba, u32 n, void *buf, bool w) { (void)ctx; return ram_rw(mk_ram, lba, n, buf, w); }
static int make_gpt(const char *path, unsigned long long mib, bool esp) {
    u64 bytes = (u64)mib << 20;
    Ram r = { calloc(1, bytes), bytes, 512 };
    BlkDev d = ram_dev(&r);
    static PartTable t;
    part_new(&t, SCHEME_GPT, 4242);
    if (esp) {   /* como um PC com Linux em UEFI: partição EFI de 100 MiB (FAT32) antes da do Linux */
        int e = part_add(&d, &t, 2048, 100 * 2048, GPT_ESP, 0xEF, "EFI system partition", 6);
        mk_ram = &r;
        if (e < 0 || !fat32_format(mk_io, 0, 2048, 100 * 2048, "EFI", 0x1234)) { fprintf(stderr, "EFI\n"); return 1; }
    }
    Region fr[4]; int nf = part_free(&d, &t, fr, 4);
    if (nf < 1 || part_add(&d, &t, fr[nf - 1].start, fr[nf - 1].count, GPT_LINUX, 0x83, "Linux", 7) < 0 || !part_write(&d, &t)) { fprintf(stderr, "GPT\n"); return 1; }
    FILE *f = fopen(path, "wb"); if (!f) { perror(path); return 2; }
    fwrite(r.p, 1, bytes, f); fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--fat32")) return fat32_cycle(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--exfat")) return exfat_cycle(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "--resize")) return resize_image(argv[2], strtoull(argv[3], 0, 10));
    if (argc >= 4 && !strcmp(argv[1], "--mkgpt")) return make_gpt(argv[2], strtoull(argv[3], 0, 10), argc > 4 && !strcmp(argv[4], "--esp"));
    if (argc == 4 && !strcmp(argv[1], "--shrunk")) return check_shrunk(argv[2], strtoull(argv[3], 0, 10));
    if (argc == 4 && !strcmp(argv[1], "--shrink")) return shrink_image(argv[2], strtoull(argv[3], 0, 10));
    if (argc < 3) { fprintf(stderr, "uso: fscheck_test <pasta das .img> <pasta das .txt>\n       fscheck_test --shrink <imagem> <MiB>\n"); return 2; }
    windows_fixture(argv[1], argv[2], "clean");
    windows_fixture(argv[1], argv[2], "dirty");
    windows_fixture(argv[1], argv[2], "hiber");
    linux_fixture(argv[1], argv[2], "clean", SAFE_OK);
    linux_fixture(argv[1], argv[2], "small", SAFE_OK);
    linux_fixture(argv[1], argv[2], "errors", SAFE_EXT_ERRORS);
    linux_fixture(argv[1], argv[2], "recover", SAFE_EXT_RECOVER);
    linux_fixture(argv[1], argv[2], "linux", SAFE_OK);

    for (u32 bs = 512; bs <= 4096; bs *= 8) {
        synthetic("BitLocker", bs, f_bitlocker, FS_BITLOCKER, SAFE_BITLOCKER);
        synthetic("LUKS", bs, f_luks, FS_LUKS, SAFE_LUKS);
        synthetic("LVM", bs, f_lvm, FS_LVM, SAFE_LVM);
        synthetic("APFS", bs, f_apfs, FS_APFS, SAFE_APPLE);
        synthetic("HFS+", bs, f_hfsp, FS_HFSP, SAFE_APPLE);
        synthetic("ext4", bs, f_ext_ok, FS_EXT, SAFE_OK);
        synthetic("ext4 diário", bs, f_ext_recover, FS_EXT, SAFE_EXT_RECOVER);
        synthetic("ext4 erros", bs, f_ext_errors, FS_EXT, SAFE_EXT_ERRORS);
        synthetic("FAT32", bs, f_fat32, FS_FAT32, SAFE_OK);
        synthetic("FAT32 desligado sem fechar", bs, f_fat32_unclean, FS_FAT32, SAFE_DIRTY);
        synthetic("FAT32 com erros", bs, f_fat32_errors, FS_FAT32, SAFE_DIRTY);
        synthetic("FAT32 sujo no BPB", bs, f_fat32_bpb, FS_FAT32, SAFE_DIRTY);
        synthetic("FAT16", bs, f_fat16, FS_FAT16, SAFE_OK);
        synthetic("FAT16 desligado sem fechar", bs, f_fat16_unclean, FS_FAT16, SAFE_DIRTY);
        synthetic("exFAT", bs, f_exfat, FS_EXFAT, SAFE_OK);
        synthetic("exFAT sujo", bs, f_exfat_dirty, FS_EXFAT, SAFE_DIRTY);
        synthetic("XFS", bs, f_xfs, FS_XFS, SAFE_UNSUPPORTED);
        synthetic("NTFS vazio", bs, f_ntfs_empty, FS_NTFS, SAFE_UNREADABLE);
    }
    if (fails) { fprintf(stderr, "fscheck_test: %d falha(s)\n", fails); return 1; }
    printf("fscheck_test: ok\n");
    return 0;
}
