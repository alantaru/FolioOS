/* FolioOS — lê um arquivo de dentro de uma imagem de disco (MBR + FAT32 ou FAT16), sem montar nada.
 *   fatcat <imagem> <caminho>            imprime o arquivo (ex.: FOLIO/DIAGNOSTICS.TXT)
 *   fatcat <imagem> <caminho> <saida>    grava num arquivo
 *   fatcat <imagem> --ls <pasta>         lista uma pasta
 *   FATCAT_VOL=N: lê o N-ésimo volume FAT do disco (0 = o primeiro; ex.: a partição do FolioOS ao lado da EFI)
 * Usado pelo laboratório QEMU (tools/qemu-lab.ps1) para conferir o que o FolioOS gravou no disco
 * durante o boot emulado. Nomes longos (LFN) e 8.3; comparação sem diferenciar maiúsculas. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
static FILE *img;
static u64 part;                  /* início da partição em bytes */
static u32 bps, spc, rsv, nfats, fatsz, root_clus, root_ents, total;
static int fat16;
static u64 data_off, root_off;

static void die(const char *m) { fprintf(stderr, "fatcat: %s\n", m); exit(2); }
static void rd(u64 off, void *b, size_t n) { if (_fseeki64(img, (long long)off, SEEK_SET) || fread(b, 1, n, img) != n) die("falha de leitura"); }
static int rd_try(u64 off, void *b, size_t n) { return !_fseeki64(img, (long long)off, SEEK_SET) && fread(b, 1, n, img) == n; }   /* sem morrer: a outra cópia ainda pode servir */
static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static u32 crc32(const u8 *p, size_t n) {
    u32 c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
/* um cabeçalho GPT válido em hdr_lba (com os dois CRCs certos): *elba, *num, *esz; senão false */
static int gpt_at(u64 hdr_lba, u64 *elba, u32 *num, u32 *esz) {
    u8 h[512]; if (!rd_try(hdr_lba * 512, h, 512)) return 0;
    if (memcmp(h, "EFI PART", 8)) return 0;
    u32 hs = le32(h + 12); if (hs < 92 || hs > 512) return 0;
    u32 hc = le32(h + 16); h[16] = h[17] = h[18] = h[19] = 0;
    if (crc32(h, hs) != hc) return 0;
    *elba = le32(h + 72) | (u64)le32(h + 76) << 32; *num = le32(h + 80); *esz = le32(h + 84);
    if (*esz < 128 || *esz > 4096 || !*num || (u64)*num * *esz > (4u << 20)) return 0;
    u8 *ents = malloc((size_t)*num * *esz); if (!ents) die("sem memória");
    if (!rd_try(*elba * 512, ents, (size_t)*num * *esz)) { free(ents); return 0; }
    int ok = crc32(ents, (size_t)*num * *esz) == le32(h + 88);
    free(ents);
    return ok;
}
static void mount(void) {
    u8 s[512]; rd(0, s, 512);
    part = 0;
    int want = 0, seen = 0;                         /* FATCAT_VOL=N: o N-ésimo volume FAT (0 = o primeiro) */
    if (getenv("FATCAT_VOL") && getenv("FATCAT_VOL")[0]) {
        char *end; long v = strtol(getenv("FATCAT_VOL"), &end, 10);
        if (*end || v < 0 || v > 255) die("FATCAT_VOL inválido (um número de volume: 0, 1, 2…)");
        want = (int)v;
    }
    if (s[510] == 0x55 && s[511] == 0xAA && !(s[0] == 0xEB || s[0] == 0xE9)) {   /* MBR: primeira partição FAT */
        for (int i = 0; i < 4; i++) {
            const u8 *e = s + 446 + 16 * i;
            if ((e[4] == 0x0B || e[4] == 0x0C || e[4] == 0x0E || e[4] == 0x06 || e[4] == 0xEF) && seen++ == want) { part = (u64)le32(e + 8) * 512; break; }
            if (e[4] == 0xEE) {                       /* GPT: primeira entrada com sistema FAT */
                u64 elba; u32 num, esz;
                _fseeki64(img, 0, SEEK_END); u64 last = (u64)_ftelli64(img) / 512 - 1;
                if (!gpt_at(1, &elba, &num, &esz) && !gpt_at(last, &elba, &num, &esz)) die("GPT inválida (CRC do cabeçalho ou das entradas, nas duas cópias)");
                for (u32 k = 0; k < num && !part; k++) {
                    u8 en[128], b[512]; rd(elba * 512 + (u64)k * esz, en, 128);
                    u64 st = le32(en + 32) | (u64)le32(en + 36) << 32;
                    if (!st) continue;
                    rd(st * 512, b, 512);
                    if ((!memcmp(b + 82, "FAT32   ", 8) || !memcmp(b + 54, "FAT16   ", 8)) && seen++ == want) part = st * 512;
                }
                break;
            }
        }
        if (!part) die(want ? "não há esse volume FAT (FATCAT_VOL)" : "nenhuma partição FAT no MBR");
        rd(part, s, 512);
    } else if (want) die("disco sem tabela de partições: só há o volume 0 (FATCAT_VOL)");
    bps = le16(s + 11); spc = s[13]; rsv = le16(s + 14); nfats = s[16]; root_ents = le16(s + 17);
    total = le16(s + 19) ? le16(s + 19) : le32(s + 32);
    fatsz = le16(s + 22) ? le16(s + 22) : le32(s + 36);
    fat16 = le16(s + 22) != 0;
    if (!bps || !spc) die("setor de boot FAT inválido");
    root_clus = fat16 ? 0 : le32(s + 44);
    root_off = part + (u64)(rsv + nfats * fatsz) * bps;
    data_off = root_off + (u64)root_ents * 32;
    if (!fat16) data_off = root_off;
}
static u32 next_clus(u32 c) {
    if (fat16) { u8 b[2]; rd(part + (u64)rsv * bps + (u64)c * 2, b, 2); u32 v = le16(b); return v >= 0xFFF8 ? 0x0FFFFFFF : v; }
    u8 b[4]; rd(part + (u64)rsv * bps + (u64)c * 4, b, 4); return le32(b) & 0x0FFFFFFF;
}
static u64 clus_off(u32 c) { return data_off + (u64)(c - 2) * spc * bps; }

/* lê uma pasta inteira para a memória (raiz FAT16 tem tamanho fixo) */
static u8 *read_dir(u32 clus, size_t *len) {
    size_t cap = 0, n = 0; u8 *d = 0; u32 cb = spc * bps;
    if (!clus && fat16) { n = (size_t)root_ents * 32; d = malloc(n); rd(root_off, d, n); *len = n; return d; }
    if (!clus) clus = root_clus;
    for (int guard = 0; clus >= 2 && clus < 0x0FFFFFF8 && guard < 100000; guard++) {
        if (n + cb > cap) { cap = (n + cb) * 2; d = realloc(d, cap); }
        rd(clus_off(clus), d + n, cb); n += cb; clus = next_clus(clus);
    }
    *len = n; return d;
}
typedef struct { char name[512]; u32 clus, size; int dir; } Ent;
/* percorre as entradas montando os nomes longos; cb devolve 1 para parar */
static int each(u8 *d, size_t len, int (*cb)(Ent *, void *), void *ctx) {
    u16 lfn[260]; int lfn_ok = 0;
    for (size_t i = 0; i + 32 <= len; i += 32) {
        u8 *e = d + i;
        if (e[0] == 0) break;
        if (e[0] == 0xE5) { lfn_ok = 0; continue; }
        if (e[11] == 0x0F) {
            int seq = e[0] & 0x1F; if (e[0] & 0x40) { memset(lfn, 0, sizeof lfn); lfn_ok = 1; }
            static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int k = 0; k < 13; k++) { int at = (seq - 1) * 13 + k; if (at < 259) lfn[at] = le16(e + pos[k]); }
            continue;
        }
        if (e[11] & 0x08) { lfn_ok = 0; continue; }          /* rótulo do volume */
        Ent en; memset(&en, 0, sizeof en);
        if (lfn_ok && lfn[0]) {
            int o = 0;
            for (int k = 0; k < 259 && lfn[k] && lfn[k] != 0xFFFF && o < 500; k++) {
                u16 c = lfn[k];
                if (c < 0x80) en.name[o++] = (char)c;
                else if (c < 0x800) { en.name[o++] = (char)(0xC0 | c >> 6); en.name[o++] = (char)(0x80 | (c & 63)); }
                else { en.name[o++] = (char)(0xE0 | c >> 12); en.name[o++] = (char)(0x80 | ((c >> 6) & 63)); en.name[o++] = (char)(0x80 | (c & 63)); }
            }
            en.name[o] = 0;
        } else {
            int o = 0;
            for (int k = 0; k < 8 && e[k] != ' '; k++) en.name[o++] = (char)e[k];
            if (e[8] != ' ') { en.name[o++] = '.'; for (int k = 8; k < 11 && e[k] != ' '; k++) en.name[o++] = (char)e[k]; }
            en.name[o] = 0;
        }
        lfn_ok = 0;
        en.clus = (u32)le16(e + 26) | (fat16 ? 0 : (u32)le16(e + 20) << 16);
        en.size = le32(e + 28); en.dir = (e[11] & 0x10) != 0;
        if (cb(&en, ctx)) return 1;
    }
    return 0;
}
typedef struct { const char *want; Ent hit; int found; } Find;
static int find_cb(Ent *e, void *c) { Find *f = c; if (!_stricmp(e->name, f->want)) { f->hit = *e; f->found = 1; return 1; } return 0; }
static int ls_cb(Ent *e, void *c) { (void)c; if (strcmp(e->name, ".") && strcmp(e->name, "..")) printf("%s%s\t%u\n", e->name, e->dir ? "/" : "", e->size); return 0; }

/* resolve "A/B/C" a partir da raiz */
static int resolve(const char *path, Ent *out) {
    Ent cur; memset(&cur, 0, sizeof cur); cur.dir = 1; cur.clus = 0;
    char buf[1024]; snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';
    char *tok = strtok(buf, "/");
    while (tok) {
        if (!cur.dir) return 0;
        size_t len; u8 *d = read_dir(cur.clus, &len);
        Find f; memset(&f, 0, sizeof f); f.want = tok;
        each(d, len, find_cb, &f); free(d);
        if (!f.found) return 0;
        cur = f.hit;
        tok = strtok(0, "/");
    }
    *out = cur; return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: fatcat <imagem> <caminho> [saida] | fatcat <imagem> --ls <pasta>\n"); return 2; }
    img = fopen(argv[1], "rb"); if (!img) die("não abriu a imagem");
    mount();
    if (!strcmp(argv[2], "--ls")) {
        Ent d; if (!resolve(argc > 3 ? argv[3] : "", &d) || !d.dir) die("pasta não encontrada");
        size_t len; u8 *b = read_dir(d.clus, &len); each(b, len, ls_cb, 0); free(b); return 0;
    }
    Ent f; if (!resolve(argv[2], &f) || f.dir) { fprintf(stderr, "fatcat: não encontrado: %s\n", argv[2]); return 1; }
    FILE *out = argc > 3 ? fopen(argv[3], "wb") : stdout; if (!out) die("não abriu a saída");
    u32 left = f.size, c = f.clus, cb = spc * bps; u8 *b = malloc(cb);
    while (left && c >= 2 && c < 0x0FFFFFF8) { u32 n = left < cb ? left : cb; rd(clus_off(c), b, n); fwrite(b, 1, n, out); left -= n; c = next_clus(c); }
    free(b); if (out != stdout) fclose(out);
    return left ? 3 : 0;
}
