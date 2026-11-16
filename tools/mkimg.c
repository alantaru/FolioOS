/* FOLIO v4 — gerador de imagem de disco (MBR + partição FAT32) a partir de uma pasta.
 *   mkimg.exe <pasta> <saida.img> [tamanho_MB] [--vhd]
 * A imagem resultante pode ser gravada com Rufus (modo DD), balenaEtcher ou dd.
 * Com --vhd, acrescenta um rodapé VHD fixo (para VirtualBox/Hyper-V com EFI).
 * Ao final, relê a imagem com um leitor FAT32 independente e confere cada arquivo. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <io.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

#define SEC 512u
#define PART_LBA 2048u
#define RSVD 32u

typedef struct Node {
    wchar_t name[260];
    wchar_t path[1024];
    int dir;
    u64 size;
    FILETIME mtime;
    struct Node **kids; int nkids;
    u8 sname[11];
    int lfn;               /* número de entradas LFN */
    u32 first, nclus;
} Node;

static FILE *img;
static u32 spc, clus_bytes, fatsz, total_secs, part_secs, data_start, next_clus = 2, nclusters;
static u32 *fat;

static void die(const char *m) { fprintf(stderr, "ERRO: %s\n", m); exit(1); }
static void wr_at(u64 off, const void *b, size_t n) { _fseeki64(img, (long long)off, SEEK_SET); if (fwrite(b, 1, n, img) != n) die("falha ao escrever a imagem"); }
static void rd_at(u64 off, void *b, size_t n) { _fseeki64(img, (long long)off, SEEK_SET); if (fread(b, 1, n, img) != n) die("falha ao ler a imagem"); }
static u64 part_off(u32 sec) { return ((u64)PART_LBA + sec) * SEC; }
static u64 clus_off(u32 c) { return part_off(data_start + (c - 2) * spc); }

/* ------------------------------------------------------------ varredura */
static Node *scan(const wchar_t *path, const wchar_t *name) {
    Node *n = calloc(1, sizeof(Node));
    wcsncpy(n->path, path, 1023); wcsncpy(n->name, name, 259);
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &ad)) die("não consegui ler um arquivo de origem");
    n->dir = (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    n->size = ((u64)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
    n->mtime = ad.ftLastWriteTime;
    if (n->dir) {
        wchar_t pat[1100]; swprintf(pat, 1100, L"%ls\\*", path);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                wchar_t cp[1024]; swprintf(cp, 1024, L"%ls\\%ls", path, fd.cFileName);
                n->kids = realloc(n->kids, sizeof(Node *) * (size_t)(n->nkids + 1));
                n->kids[n->nkids++] = scan(cp, fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    return n;
}

/* ------------------------------------------------------------ nomes 8.3 + LFN */
static int valid83(const wchar_t *s) {
    const wchar_t *dot = wcsrchr(s, L'.');
    int bl = dot ? (int)(dot - s) : (int)wcslen(s), el = dot ? (int)wcslen(dot + 1) : 0;
    if (bl < 1 || bl > 8 || el > 3) return 0;
    for (const wchar_t *c = s; *c; c++) {
        if (c == dot) continue;
        if (*c == L'.') return 0;
        if (!((*c >= L'A' && *c <= L'Z') || (*c >= L'0' && *c <= L'9') || wcschr(L"_-~!#$%&'()@^`{}", *c))) return 0;
    }
    return 1;
}
static void make_short(Node *dir) {
    for (int i = 0; i < dir->nkids; i++) {
        Node *k = dir->kids[i];
        memset(k->sname, ' ', 11);
        const wchar_t *dot = wcsrchr(k->name, L'.');
        if (valid83(k->name)) {
            int j = 0;
            for (const wchar_t *c = k->name; *c && c != dot; c++) k->sname[j++] = (u8)*c;
            if (dot) for (int e = 0; dot[1 + e] && e < 3; e++) k->sname[8 + e] = (u8)dot[1 + e];
            k->lfn = 0;
        } else {
            char base[9] = { 0 }; int bl = 0;
            for (const wchar_t *c = k->name; *c && c != dot && bl < 6; c++) {
                wchar_t w = towupper(*c);
                if ((w >= L'A' && w <= L'Z') || (w >= L'0' && w <= L'9')) base[bl++] = (char)w;
            }
            if (!bl) base[bl++] = 'F';
            char ext[4] = { 0 }; int xl = 0;
            if (dot) for (const wchar_t *c = dot + 1; *c && xl < 3; c++) { wchar_t w = towupper(*c); if ((w >= L'A' && w <= L'Z') || (w >= L'0' && w <= L'9')) ext[xl++] = (char)w; }
            for (int t = 1; t < 1000000; t++) {
                char tail[12]; int tl = sprintf(tail, "~%d", t);
                int keep = bl; if (keep + tl > 8) keep = 8 - tl;
                u8 cand[11]; memset(cand, ' ', 11);
                memcpy(cand, base, (size_t)keep); memcpy(cand + keep, tail, (size_t)tl); memcpy(cand + 8, ext, (size_t)xl);
                int clash = 0;
                for (int q = 0; q < i; q++) if (!memcmp(dir->kids[q]->sname, cand, 11)) clash = 1;
                if (!clash) { memcpy(k->sname, cand, 11); break; }
            }
            k->lfn = (int)((wcslen(k->name) + 12) / 13);
        }
        if (k->dir) make_short(k);
    }
}
static u8 lfn_sum(const u8 *s) { u8 c = 0; for (int i = 0; i < 11; i++) c = (u8)(((c & 1) << 7) + (c >> 1) + s[i]); return c; }

/* ------------------------------------------------------------ clusters */
static u32 dir_bytes(Node *d, int is_root) {
    u32 e = is_root ? 1 : 2;                        /* rótulo do volume  /  "." e ".." */
    for (int i = 0; i < d->nkids; i++) e += 1 + (u32)d->kids[i]->lfn;
    return e * 32;
}
static void alloc(Node *n, int is_root) {
    u64 bytes = n->dir ? dir_bytes(n, is_root) : n->size;
    n->nclus = (u32)((bytes + clus_bytes - 1) / clus_bytes);
    if (n->dir && n->nclus == 0) n->nclus = 1;
    n->first = n->nclus ? next_clus : 0;
    if (next_clus + n->nclus > nclusters + 2) die("os arquivos não cabem no tamanho de imagem escolhido");
    for (u32 i = 0; i < n->nclus; i++) fat[next_clus + i] = (i + 1 == n->nclus) ? 0x0FFFFFFF : next_clus + i + 1;
    next_clus += n->nclus;
    if (n->dir) for (int i = 0; i < n->nkids; i++) alloc(n->kids[i], 0);
}

static void dos_time(FILETIME ft, u16 *date, u16 *time) {
    FILETIME lt; SYSTEMTIME s; FileTimeToLocalFileTime(&ft, &lt); FileTimeToSystemTime(&lt, &s);
    if (s.wYear < 1980) s.wYear = 1980;
    *date = (u16)(((s.wYear - 1980) << 9) | (s.wMonth << 5) | s.wDay);
    *time = (u16)((s.wHour << 11) | (s.wMinute << 5) | (s.wSecond / 2));
}
static void put_entry(u8 *e, const u8 *sname, u8 attr, u32 clus, u32 size, FILETIME ft) {
    memset(e, 0, 32);
    memcpy(e, sname, 11); e[11] = attr;
    u16 d, t; dos_time(ft, &d, &t);
    memcpy(e + 14, &t, 2); memcpy(e + 16, &d, 2); memcpy(e + 18, &d, 2); memcpy(e + 22, &t, 2); memcpy(e + 24, &d, 2);
    u16 hi = (u16)(clus >> 16), lo = (u16)clus; memcpy(e + 20, &hi, 2); memcpy(e + 26, &lo, 2);
    memcpy(e + 28, &size, 4);
}

static void write_node(Node *n, Node *parent, int is_root) {
    if (!n->dir) {
        if (!n->nclus) return;
        HANDLE h = CreateFileW(n->path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        if (h == INVALID_HANDLE_VALUE) die("não abriu arquivo de origem");
        u8 *buf = malloc(1 << 20); u64 off = clus_off(n->first), left = n->size;
        while (left) { DWORD r = 0; ReadFile(h, buf, (DWORD)(left < (1 << 20) ? left : (1 << 20)), &r, 0); if (!r) die("leitura curta"); wr_at(off, buf, r); off += r; left -= r; }
        CloseHandle(h); free(buf);
        return;
    }
    u32 bytes = n->nclus * clus_bytes;
    u8 *d = calloc(1, bytes); u32 p = 0;
    if (is_root) { u8 lab[11]; memcpy(lab, "FOLIO      ", 11); put_entry(d + p, lab, 0x08, 0, 0, n->mtime); p += 32; }
    else {
        put_entry(d + p, (const u8 *)".          ", 0x10, n->first, 0, n->mtime); p += 32;
        put_entry(d + p, (const u8 *)"..         ", 0x10, parent && parent->first != 2 ? parent->first : 0, 0, n->mtime); p += 32;
    }
    for (int i = 0; i < n->nkids; i++) {
        Node *k = n->kids[i];
        u8 sum = lfn_sum(k->sname);
        size_t L = wcslen(k->name);
        for (int s = k->lfn; s >= 1; s--) {
            u8 *e = d + p; memset(e, 0, 32);
            e[0] = (u8)(s | (s == k->lfn ? 0x40 : 0)); e[11] = 0x0F; e[13] = sum;
            static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int c = 0; c < 13; c++) {
                size_t idx = (size_t)(s - 1) * 13 + (size_t)c;
                u16 ch = idx < L ? (u16)k->name[idx] : idx == L ? 0 : 0xFFFF;
                memcpy(e + pos[c], &ch, 2);
            }
            p += 32;
        }
        put_entry(d + p, k->sname, k->dir ? 0x10 : 0x20, k->first, k->dir ? 0 : (u32)k->size, k->mtime); p += 32;
    }
    wr_at(clus_off(n->first), d, bytes);
    free(d);
    for (int i = 0; i < n->nkids; i++) write_node(n->kids[i], n, 0);
}

/* ------------------------------------------------------------ verificação (leitor FAT32 independente) */
static u32 v_spc, v_data, v_fat, v_root;
static u32 v_next(u32 c) { u32 v; rd_at(part_off(v_fat) + (u64)c * 4, &v, 4); return v & 0x0FFFFFFF; }
static int checked, errors;
static void verify_dir(u32 clus, Node *src) {
    for (int i = 0; i < src->nkids; i++) {
        Node *k = src->kids[i];
        /* procura a entrada curta k->sname nos clusters do diretório */
        u32 c = clus; int found = 0; u32 fc = 0, fsz = 0; u8 attr = 0;
        while (c >= 2 && c < 0x0FFFFFF8 && !found) {
            u8 *b = malloc(v_spc * SEC); rd_at(part_off(v_data + (c - 2) * v_spc), b, v_spc * SEC);
            for (u32 o = 0; o < v_spc * SEC; o += 32) if (b[o + 11] != 0x0F && !(b[o + 11] & 0x08) && !memcmp(b + o, k->sname, 11)) {
                found = 1; attr = b[o + 11]; fc = (u32)(b[o + 20] | b[o + 21] << 8) << 16 | (u32)(b[o + 26] | b[o + 27] << 8); memcpy(&fsz, b + o + 28, 4); break; }
            free(b); c = v_next(c);
        }
        if (!found) { fwprintf(stderr, L"  falta: %ls\n", k->path); errors++; continue; }
        if (k->dir) { if (!(attr & 0x10)) errors++; verify_dir(fc, k); continue; }
        if (fsz != k->size) { fwprintf(stderr, L"  tamanho errado: %ls\n", k->path); errors++; continue; }
        HANDLE h = CreateFileW(k->path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        u8 *a = malloc(v_spc * SEC), *s = malloc(v_spc * SEC); u32 left = fsz, cc = fc;
        while (left) {
            u32 n = left < v_spc * SEC ? left : v_spc * SEC; DWORD r;
            rd_at(part_off(v_data + (cc - 2) * v_spc), a, n); ReadFile(h, s, n, &r, 0);
            if (memcmp(a, s, n)) { fwprintf(stderr, L"  conteúdo divergente: %ls\n", k->path); errors++; break; }
            left -= n; cc = v_next(cc);
        }
        CloseHandle(h); free(a); free(s); checked++;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: mkimg <pasta> <saida.img> [MB] [--vhd]\n"); return 1; }
    u32 mb = 2048; int vhd = 0;
    for (int i = 3; i < argc; i++) { if (!strcmp(argv[i], "--vhd")) vhd = 1; else mb = (u32)atoi(argv[i]); }
    if (mb < 64) mb = 64;
    if (mb > 32768) die("tamanho máximo: 32768 MB");
    wchar_t src[1024]; MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, src, 1024);
    wchar_t full[1024]; GetFullPathNameW(src, 1024, full, 0);
    Node *root = scan(full, L"");
    make_short(root);

    total_secs = (u32)((u64)mb * 1024 * 1024 / SEC);
    part_secs = total_secs - PART_LBA;
    /* cluster: o maior (até 4 KB) que ainda dê >= 65.525 clusters (exigência do FAT32) */
    u32 cands[4] = { 8, 4, 2, 1 };
    for (int i = 0; i < 4; i++) {
        spc = cands[i];
        u32 t2 = (256 * spc + 2) / 2;
        fatsz = (part_secs - RSVD + t2 - 1) / t2;
        nclusters = (part_secs - RSVD - 2 * fatsz) / spc;
        if (nclusters >= 66000) break;
    }
    if (nclusters < 65525) die("imagem pequena demais para FAT32");
    clus_bytes = spc * SEC;
    data_start = RSVD + 2 * fatsz;
    fat = calloc((size_t)fatsz * SEC / 4, 4);
    fat[0] = 0x0FFFFFF8; fat[1] = 0x0FFFFFFF;
    alloc(root, 1);

    img = fopen(argv[2], "w+b");
    if (!img) die("não consegui criar o arquivo de saída");
    /* define o tamanho final (o NTFS preenche com zeros) */
    _chsize_s(_fileno(img), (long long)total_secs * SEC);

    /* MBR */
    u8 mbr[512] = { 0 };
    u32 sig = GetTickCount() ^ 0x464F4C49; memcpy(mbr + 440, &sig, 4);
    u8 *pe = mbr + 446;
    pe[0] = 0x80; pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF; pe[4] = 0x0C; pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
    u32 lba = PART_LBA; memcpy(pe + 8, &lba, 4); memcpy(pe + 12, &part_secs, 4);

    /* setor de boot FAT32 */
    u8 bs[512] = { 0 };
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90; memcpy(bs + 3, "MSWIN4.1", 8);
    u16 v16; u32 v32;
    v16 = SEC; memcpy(bs + 11, &v16, 2); bs[13] = (u8)spc; v16 = RSVD; memcpy(bs + 14, &v16, 2); bs[16] = 2;
    bs[21] = 0xF8; v16 = 63; memcpy(bs + 24, &v16, 2); v16 = 255; memcpy(bs + 26, &v16, 2);
    v32 = PART_LBA; memcpy(bs + 28, &v32, 4); memcpy(bs + 32, &part_secs, 4); memcpy(bs + 36, &fatsz, 4);
    v32 = 2; memcpy(bs + 44, &v32, 4); v16 = 1; memcpy(bs + 48, &v16, 2); v16 = 6; memcpy(bs + 50, &v16, 2);
    bs[64] = 0x80; bs[66] = 0x29; memcpy(bs + 67, &sig, 4); memcpy(bs + 71, "FOLIO      ", 11); memcpy(bs + 82, "FAT32   ", 8);
    /* código de boot BIOS: apenas avisa que é preciso iniciar em modo UEFI */
    static const u8 code[] = { 0xFB, 0x31, 0xC0, 0x8E, 0xD8, 0xBE, 0x77, 0x7C, 0xFC, 0xAC, 0x84, 0xC0, 0x74, 0x06, 0xB4, 0x0E, 0xCD, 0x10, 0xEB, 0xF5, 0xF4, 0xEB, 0xFD };
    memcpy(bs + 90, code, sizeof code);
    const char *msg = "FOLIO 4: inicie este pen-drive em modo UEFI (desative o CSM/Legacy).\r\n";
    memcpy(bs + 0x77, msg, strlen(msg) + 1);
    bs[510] = 0x55; bs[511] = 0xAA;
    memcpy(mbr, code, sizeof code); memcpy(mbr + 0x77, msg, strlen(msg) + 1);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    wr_at(0, mbr, 512);
    wr_at(part_off(0), bs, 512); wr_at(part_off(6), bs, 512);
    u8 fsi[512] = { 0 };
    v32 = 0x41615252; memcpy(fsi, &v32, 4); v32 = 0x61417272; memcpy(fsi + 484, &v32, 4);
    v32 = nclusters - (next_clus - 2); memcpy(fsi + 488, &v32, 4); memcpy(fsi + 492, &next_clus, 4);
    v32 = 0xAA550000; memcpy(fsi + 508, &v32, 4);
    wr_at(part_off(1), fsi, 512); wr_at(part_off(7), fsi, 512);
    wr_at(part_off(RSVD), fat, (size_t)fatsz * SEC);
    wr_at(part_off(RSVD + fatsz), fat, (size_t)fatsz * SEC);
    write_node(root, 0, 1);
    fflush(img);

    /* verificação */
    u8 rb[512]; rd_at(part_off(0), rb, 512);
    v_spc = rb[13]; v_fat = (u32)(rb[14] | rb[15] << 8); u32 vf; memcpy(&vf, rb + 36, 4); v_data = v_fat + 2 * vf; memcpy(&v_root, rb + 44, 4);
    verify_dir(v_root, root);
    if (vhd) {
        /* rodapé VHD fixo (formato Microsoft Virtual Hard Disk) */
        u8 f[512] = { 0 }; u64 sz = (u64)total_secs * SEC;
        memcpy(f, "conectix", 8); f[11] = 2; f[12] = 0; f[13] = 1; f[14] = 0; f[15] = 0;
        for (int i = 16; i < 24; i++) f[i] = 0xFF;
        memcpy(f + 28, "flio", 4); f[32] = 0; f[33] = 1; memcpy(f + 36, "Wi2k", 4);
        for (int i = 0; i < 8; i++) { f[40 + i] = (u8)(sz >> (56 - 8 * i)); f[48 + i] = (u8)(sz >> (56 - 8 * i)); }
        u32 cyl = total_secs / (16 * 63); if (cyl > 65535) cyl = 65535;
        f[56] = (u8)(cyl >> 8); f[57] = (u8)cyl; f[58] = 16; f[59] = 63;
        f[63] = 2;   /* disco fixo */
        for (int i = 0; i < 16; i++) f[68 + i] = (u8)(sig * (i + 7));
        u32 cs = 0; for (int i = 0; i < 512; i++) cs += f[i]; cs = ~cs;
        f[64] = (u8)(cs >> 24); f[65] = (u8)(cs >> 16); f[66] = (u8)(cs >> 8); f[67] = (u8)cs;
        wr_at(sz, f, 512);
    }
    fclose(img);
    printf("imagem: %s · %u MB · FAT32 com clusters de %u bytes · %u clusters (%u usados)\n", argv[2], mb, clus_bytes, nclusters, next_clus - 2);
    printf("verificação: %d arquivos relidos da imagem e conferidos byte a byte, %d erro(s)\n", checked, errors);
    return errors ? 2 : 0;
}
