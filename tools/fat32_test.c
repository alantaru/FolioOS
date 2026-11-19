/* FolioOS — teste do driver FAT32 da ponte BIOS (src/platform/bios/fat32.c) sobre uma imagem de disco.
 *   fat32_test.exe <imagem.img> [operações] [semente]
 * Aplica operações aleatórias (criar, gravar, truncar, renomear, apagar, criar pasta) comparando cada
 * byte com um modelo em memória; no fim monta de novo do zero e confere tudo, mais uma verificação
 * completa do volume: nenhum cluster em duas cadeias, cadeias do tamanho certo, cópias da FAT iguais,
 * nenhum cluster perdido e a contagem de livres do FSInfo correta. Sai com 1 se algo estiver errado. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core/fat32.h"
#include "../src/core/part.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
static FILE *img;
static bool io(void *ctx, u64 lba, u32 n, void *buf, bool w) {
    (void)ctx;
    if (_fseeki64(img, (long long)(lba * 512), SEEK_SET)) return false;
    return w ? fwrite(buf, 512, n, img) == n : fread(buf, 512, n, img) == n;
}
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALHA (linha %d): ", __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fails++; if (fails > 10) exit(1); } } while (0)

static u32 rng = 12345;
static u32 rnd(u32 n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % (n ? n : 1); }

static void w(u16 *o, const char *utf8) {   /* UTF-8 -> UCS-2 */
    const u8 *s = (const u8 *)utf8; int k = 0;
    while (*s) {
        u32 c = *s++;
        if (c >= 0xE0) { c = (c & 15) << 12; c |= (u32)(*s++ & 63) << 6; c |= *s++ & 63; }
        else if (c >= 0xC0) { c = (c & 31) << 6; c |= *s++ & 63; }
        o[k++] = (u16)c;
    }
    o[k] = 0;
}
static const char *pool[] = {
    "lab.md", "Capítulo 1 — início.md", "UPPER.TXT", "a.b.c.d", "notas", "README", "x.md", "Résumé final.odt",
    "日本語のファイル.txt", "Ação.docx", "one two three four five six seven eight nine ten eleven twelve thirteen.md",
    "MiXeD.Md", "e", "file+plus;semi.txt", "lower.txt", "LOWER2.TXT", "draft (1).md", "draft (2).md", "Чехов.txt", "ü.md"
};
#define NPOOL (int)(sizeof pool / sizeof pool[0])
typedef struct { bool live; int dir; int name; u8 *data; u32 size; } MF;
static MF mf[64];
static const char *dirs[] = { "", "Docs", "Pasta com nome longo", "Pasta com nome longo/Sub" };
static FatNode dirn[4];
static Fat fat;

static bool same_name(const u16 *a, const char *b) { u16 t[260]; w(t, b); for (int i = 0; ; i++) { u16 x = a[i], y = t[i]; if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; if (x >= 0xE0 && x <= 0xFE && x != 0xF7) x -= 32; if (y >= 0xE0 && y <= 0xFE && y != 0xF7) y -= 32; if (x != y) return false; if (!x) return true; } }
static int find_model(int d, int name) { for (int i = 0; i < 64; i++) if (mf[i].live && mf[i].dir == d) { u16 a[260]; w(a, pool[mf[i].name]); if (same_name(a, pool[name])) return i; } return -1; }

static bool open_dirs(void) {
    fat_root(&fat, &dirn[0]);
    for (int d = 1; d < 4; d++) {
        const char *p = dirs[d]; FatNode cur = dirn[0];
        char part[128]; const char *s = p;
        while (*s) {
            int k = 0; while (*s && *s != '/') part[k++] = *s++; part[k] = 0; if (*s) s++;
            u16 nm[128]; w(nm, part);
            FatNode nx;
            if (fat_lookup(&fat, &cur, nm, &nx) != FAT_OK) { int r = fat_create(&fat, &cur, nm, true, &nx); if (r) { fprintf(stderr, "mkdir %s: %d\n", part, r); return false; } }
            cur = nx;
        }
        dirn[d] = cur;
    }
    return true;
}
static void verify_file(int i, const char *when) {
    u16 nm[260]; w(nm, pool[mf[i].name]);
    FatNode n; int r = fat_lookup(&fat, &dirn[mf[i].dir], nm, &n);
    CHECK(r == FAT_OK, "%s: %s/%s sumiu", when, dirs[mf[i].dir], pool[mf[i].name]);
    if (r) return;
    CHECK(n.size == mf[i].size, "%s: %s tamanho %u, esperado %u", when, pool[mf[i].name], n.size, mf[i].size);
    u8 *b = malloc(mf[i].size + 1);
    int got = fat_read(&fat, &n, 0, b, mf[i].size);
    CHECK(got == (int)mf[i].size, "%s: leu %d de %u", when, got, mf[i].size);
    if (got == (int)mf[i].size && mf[i].size) CHECK(!memcmp(b, mf[i].data, mf[i].size), "%s: conteúdo diferente em %s", when, pool[mf[i].name]);
    /* leitura em pedaços desalinhados também */
    if (mf[i].size > 1000) {
        u32 pos = rnd(mf[i].size), len = rnd(mf[i].size - pos) + 1;
        got = fat_read(&fat, &n, pos, b, len);
        CHECK(got == (int)len && !memcmp(b, mf[i].data + pos, len), "%s: leitura parcial %u+%u falhou", when, pos, len);
    }
    free(b);
}

/* ------------------------------------------------------------ verificação do volume */
static u8 *seen;
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u32 fatv(u32 c) { u8 s[512]; io(0, fat.part + fat.fat_lba + c / 128, 1, s, false); return rd32(s + (c % 128) * 4) & 0x0FFFFFFF; }
static u32 walk(u32 c, const char *who) {
    u32 n = 0;
    while (c >= 2 && c < 0x0FFFFFF8) {
        CHECK(c < fat.nclus + 2, "%s: cluster %u fora do volume", who, c);
        if (c >= fat.nclus + 2) break;
        CHECK(!seen[c], "%s: cluster %u em duas cadeias", who, c);
        if (seen[c]) break;
        seen[c] = 1; n++;
        c = fatv(c);
        CHECK(c != 0, "%s: cadeia aponta para cluster livre", who);
    }
    return n;
}
static void fsck_dir(FatNode *d, const char *path, int depth) {
    u16 nm[256]; FatNode ch; u32 ck = 0;
    while (fat_readdir(&fat, d, &ck, nm, &ch) == 1) {
        char who[300]; snprintf(who, sizeof who, "%s/%c...", path, (char)nm[0]);
        u32 n = walk(ch.clus, who);
        if (ch.attr & 0x10) { CHECK(n >= 1, "%s: pasta sem cluster", who); if (depth < 8) fsck_dir(&ch, who, depth + 1); }
        else CHECK(n == (ch.size + fat.bpc - 1) / fat.bpc, "%s: cadeia de %u clusters para %u bytes", who, n, ch.size);
    }
}
static void fsck(void) {
    seen = calloc(fat.nclus + 2, 1);
    FatNode root; fat_root(&fat, &root);
    walk(root.clus, "raiz");
    fsck_dir(&root, "", 0);
    u32 used = 0, marked = 0;
    for (u32 c = 2; c < fat.nclus + 2; c++) { used += seen[c]; if (fatv(c)) marked++; }
    CHECK(used == marked, "clusters marcados na FAT %u, usados por arquivos %u (perdidos %d)", marked, used, (int)(marked - used));
    /* cópias da FAT */
    u8 a[512], b[512];
    for (u32 s = 0; s < fat.fat_secs && fat.nfats > 1; s++) {
        io(0, fat.part + fat.fat_lba + s, 1, a, false); io(0, fat.part + fat.fat_lba + fat.fat_secs + s, 1, b, false);
        if (memcmp(a, b, 512)) { CHECK(0, "cópias da FAT diferem no setor %u", s); break; }
    }
    if (fat.fsinfo) {
        u8 s[512]; io(0, fat.part + fat.fsinfo, 1, s, false);
        u32 fc = rd32(s + 488);
        CHECK(fc == 0xFFFFFFFFu || fc == fat.nclus - used, "FSInfo diz %u livres, de fato %u", fc, fat.nclus - used);
    }
    printf("verificação: %u clusters usados de %u, FAT %u cópias\n", used, fat.nclus, fat.nfats);
    free(seen);
}

/* volumes FAT32 da imagem (MBR, GPT ou sem tabela), pela mesma leitura de tabelas do instalador */
static int fat_volumes(u64 *lba, int cap) {
    _fseeki64(img, 0, SEEK_END); u64 size = (u64)_ftelli64(img);
    BlkDev d = { io, 0, size / 512, 512, 0 };
    static PartTable t; int n = 0;
    if (!part_read(&d, &t)) return 0;
    for (int i = 0; i < t.n; i++) if (t.p[i].fs == FS_FAT32) { if (n >= cap) return -1; lba[n++] = t.p[i].start; }   /* -1: não cabe (nunca conferir só uma parte) */
    /* sem tabela e sem "FAT32   " no campo de tipo (que é só informativo): um FAT32 que monta no setor 0 também vale */
    if (!n && t.scheme != SCHEME_GPT && t.scheme != SCHEME_MBR) { static Fat probe; if (fat_mount(&probe, io, 0, 0)) lba[n++] = 0; }
    /* sem tabela e sem "FAT32   " no tipo (o campo é só informativo): um FAT32 que monta no setor 0 também vale */
    if (!n && t.scheme != SCHEME_GPT && t.scheme != SCHEME_MBR) { static Fat probe; if (fat_mount(&probe, io, 0, 0)) lba[n++] = 0; }
    return n;
}
static bool mount(void) {
    u64 lba[8];
    if (fat_volumes(lba, 8) < 1 || !fat_mount(&fat, io, 0, lba[0])) { fprintf(stderr, "não montou\n"); return false; }
    return open_dirs();
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: fat32_test <imagem> [operações] [semente]\n"); return 2; }
    int ops = argc > 2 ? atoi(argv[2]) : 3000;
    if (argc > 3) rng = (u32)atoi(argv[3]);
    if (argc > 2 && !strcmp(argv[2], "--fsck")) {          /* só confere o volume (depois de uma sessão no laboratório) */
        img = fopen(argv[1], "rb"); if (!img) { perror(argv[1]); return 2; }
        static u64 lba[PART_MAX + 1]; int nv = fat_volumes(lba, PART_MAX + 1);
        if (nv < 0) { fprintf(stderr, "volumes FAT32 demais para conferir\n"); return 1; }
        if (!nv) { fprintf(stderr, "nenhum volume FAT32\n"); return 1; }
        for (int v = 0; v < nv; v++) {                       /* confere todos os volumes FAT32 do disco */
            if (!fat_mount(&fat, io, 0, lba[v])) { fprintf(stderr, "não montou o volume em %llu\n", (unsigned long long)lba[v]); return 1; }
            fsck();
        }
        return fails ? 1 : 0;
    }
    img = fopen(argv[1], "r+b"); if (!img) { perror(argv[1]); return 2; }
    if (!mount()) return 1;
    printf("montado: %u clusters de %u bytes, %u livres\n", fat.nclus, fat.bpc, fat_free_clusters(&fat));
    int counts[8] = { 0 };
    for (int op = 0; op < ops; op++) {
        int k = (int)rnd(100), i = (int)rnd(64);
        if (k < 25) {                                   /* criar */
            if (mf[i].live) continue;
            int d = (int)rnd(4), nm = (int)rnd(NPOOL);
            u16 name[260]; w(name, pool[nm]);
            FatNode n; int r = fat_create(&fat, &dirn[d], name, false, &n);
            int exist = find_model(d, nm);
            if (exist >= 0) { CHECK(r == FAT_EXISTS, "criar %s duplicado deu %d", pool[nm], r); continue; }
            CHECK(r == FAT_OK, "criar %s/%s deu %d", dirs[d], pool[nm], r);
            if (r) continue;
            mf[i].live = true; mf[i].dir = d; mf[i].name = nm; mf[i].data = 0; mf[i].size = 0; counts[0]++;
        } else if (!mf[i].live) continue;
        else if (k < 60) {                              /* gravar */
            u32 pos = rnd(3) == 0 ? mf[i].size + rnd(5000) : rnd(mf[i].size + 1), len = rnd(4) == 0 ? rnd(70000) + 1 : rnd(3000) + 1;
            if (pos + len > 400000) continue;
            u8 *buf = malloc(len); for (u32 j = 0; j < len; j++) buf[j] = (u8)rnd(256);
            u16 name[260]; w(name, pool[mf[i].name]); FatNode n;
            if (fat_lookup(&fat, &dirn[mf[i].dir], name, &n)) { CHECK(0, "gravar: não achou %s", pool[mf[i].name]); free(buf); continue; }
            int r = fat_write(&fat, &n, pos, buf, len);
            CHECK(r == (int)len, "gravar %u em %u deu %d", len, pos, r);
            u32 ns = pos + len > mf[i].size ? pos + len : mf[i].size;
            mf[i].data = realloc(mf[i].data, ns);
            if (pos > mf[i].size) memset(mf[i].data + mf[i].size, 0, pos - mf[i].size);
            memcpy(mf[i].data + pos, buf, len); mf[i].size = ns; free(buf); counts[1]++;
        } else if (k < 70) {                            /* truncar */
            u32 ns = rnd(2) ? rnd(mf[i].size + 1) : mf[i].size + rnd(9000);
            u16 name[260]; w(name, pool[mf[i].name]); FatNode n;
            if (fat_lookup(&fat, &dirn[mf[i].dir], name, &n)) continue;
            int r = fat_truncate(&fat, &n, ns); CHECK(r == FAT_OK, "truncar deu %d", r);
            mf[i].data = realloc(mf[i].data, ns ? ns : 1);
            if (ns > mf[i].size) memset(mf[i].data + mf[i].size, 0, ns - mf[i].size);
            mf[i].size = ns; counts[2]++;
        } else if (k < 82) {                            /* renomear */
            int nm = (int)rnd(NPOOL);
            u16 name[260], nn[260]; w(name, pool[mf[i].name]); w(nn, pool[nm]); FatNode n;
            if (fat_lookup(&fat, &dirn[mf[i].dir], name, &n)) continue;
            int r = fat_rename(&fat, &n, nn);
            int other = find_model(mf[i].dir, nm);
            if (other >= 0 && other != i) { CHECK(r == FAT_EXISTS, "renomear sobre existente deu %d", r); continue; }
            CHECK(r == FAT_OK, "renomear %s -> %s deu %d", pool[mf[i].name], pool[nm], r);
            if (!r) { mf[i].name = nm; counts[3]++; }
        } else if (k < 92) {                            /* apagar */
            u16 name[260]; w(name, pool[mf[i].name]); FatNode n;
            if (fat_lookup(&fat, &dirn[mf[i].dir], name, &n)) continue;
            int r = fat_delete(&fat, &n); CHECK(r == FAT_OK, "apagar deu %d", r);
            mf[i].live = false; free(mf[i].data); mf[i].data = 0; counts[4]++;
        } else verify_file(i, "durante");
        if (op % 500 == 499) for (int j = 0; j < 64; j++) if (mf[j].live) verify_file(j, "checagem");
    }
    fat_flush(&fat);
    printf("operações: %d criados, %d gravações, %d truncamentos, %d renomeados, %d apagados\n", counts[0], counts[1], counts[2], counts[3], counts[4]);
    /* do zero: nova montagem, sem nada em cache */
    if (!mount()) return 1;
    int live = 0;
    for (int j = 0; j < 64; j++) if (mf[j].live) { verify_file(j, "remontado"); live++; }
    /* a listagem de cada pasta tem exatamente os arquivos do modelo */
    for (int d = 0; d < 4; d++) {
        u16 nm[256]; FatNode ch; u32 ck = 0; int files = 0, want = 0;
        while (fat_readdir(&fat, &dirn[d], &ck, nm, &ch) == 1) if (!(ch.attr & 0x10)) {
            bool known = false;
            for (int j = 0; j < 64; j++) if (mf[j].live && mf[j].dir == d) { u16 a[260]; w(a, pool[mf[j].name]); if (!memcmp(a, nm, (size_t)(1 + (int)wcslen((wchar_t *)a)) * 2)) known = true; }
            if (d == 0 || d == 1) { if (known) files++; } else { CHECK(known, "pasta %s tem arquivo inesperado", dirs[d]); files++; }
        }
        for (int j = 0; j < 64; j++) if (mf[j].live && mf[j].dir == d) want++;
        CHECK(files == want, "pasta '%s': %d arquivos, esperado %d", dirs[d], files, want);
    }
    fsck();
    printf("%d arquivos vivos conferidos byte a byte; %s\n", live, fails ? "COM FALHAS" : "tudo certo");
    fclose(img);
    return fails ? 1 : 0;
}
