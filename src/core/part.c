/* FolioOS — tabelas de partição (ver part.h).
 * GPT: lê o cabeçalho principal e, se ele ou a lista estiverem estragados (CRC), o de reserva no fim do
 * disco. Ao gravar, escreve primeiro a cópia de reserva e por último a principal: se a energia cair no
 * meio, sempre sobra uma cópia íntegra (a antiga ou a nova).
 * MBR: as 4 entradas primárias e as lógicas da partição estendida (estas só para leitura nesta fase;
 * a estendida e as lógicas são preservadas byte a byte ao regravar). */
#include "part.h"
#include <stddef.h>
void *memcpy(void *, const void *, size_t); void *memset(void *, int, size_t); int memcmp(const void *, const void *, size_t);

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64(const u8 *p) { return rd32(p) | (u64)rd32(p + 4) << 32; }
static void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i)); }
static void wr64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i)); }

const u8 GPT_ESP[16]        = { 0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
const u8 GPT_MSDATA[16]     = { 0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7 };
const u8 GPT_MSR[16]        = { 0x16,0xE3,0xC9,0xE3,0x5C,0x0B,0xB8,0x4D,0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE };
const u8 GPT_WINRE[16]      = { 0xA4,0xBB,0x94,0xDE,0xD1,0x06,0x40,0x4D,0xA1,0x6A,0xBF,0xD5,0x01,0x79,0xD6,0xAC };
const u8 GPT_LINUX[16]      = { 0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4 };
const u8 GPT_LINUX_SWAP[16] = { 0x6D,0xFD,0x57,0x06,0xAB,0xA4,0xC4,0x43,0x84,0xE5,0x09,0x33,0xC8,0x4B,0x4F,0x4F };
const u8 GPT_BIOSBOOT[16]   = { 0x48,0x61,0x68,0x21,0x49,0x64,0x6F,0x6E,0x74,0x4E,0x65,0x65,0x64,0x45,0x46,0x49 };
const u8 GPT_APFS[16]       = { 0xEF,0x57,0x34,0x7C,0x00,0x00,0xAA,0x11,0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC };
const u8 GPT_HFSP[16]       = { 0x00,0x53,0x46,0x48,0x00,0x00,0xAA,0x11,0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC };
const u8 GPT_LVM[16]        = { 0x79,0xD3,0xD6,0xE6,0x07,0xF5,0xC2,0x44,0xA2,0x3C,0x23,0x8F,0x2A,0x3D,0xF9,0x28 };
static const u8 ZERO16[16];

uint32_t crc32_buf(uint32_t crc, const void *p, uint32_t n) {
    const u8 *b = p; crc = ~crc;
    while (n--) { crc ^= *b++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1))); }
    return ~crc;
}
static u64 rng(u64 *s) { u64 x = *s ? *s : 0x9E3779B97F4A7C15ull; x ^= x << 13; x ^= x >> 7; x ^= x << 17; *s = x; return x; }
static void new_guid(u8 g[16], u64 *s) {
    u64 a = rng(s), b = rng(s);
    for (int i = 0; i < 8; i++) { g[i] = (u8)(a >> (8 * i)); g[8 + i] = (u8)(b >> (8 * i)); }
    g[7] = (u8)((g[7] & 0x0F) | 0x40);        /* versão 4 (aleatório) */
    g[8] = (u8)((g[8] & 0x3F) | 0x80);        /* variante RFC 4122 */
}

/* ------------------------------------------------------------ leitura em bytes (setores de qualquer tamanho) */
static bool rd_bytes(const BlkDev *d, u64 lba, u32 off, void *out, u32 n, u8 *tmp) {
    /* tmp: buffer de pelo menos 8 KB + bs */
    u64 first = lba + off / d->bs; u32 skip = off % d->bs;
    u32 secs = (skip + n + d->bs - 1) / d->bs;
    if (secs * d->bs > 8192 + d->bs || first + secs > d->nsec) return false;
    if (!d->rw(d->ctx, first, secs, tmp, false)) return false;
    memcpy(out, tmp + skip, n);
    return true;
}

/* ------------------------------------------------------------ detecção do sistema de arquivos */
static void copy_label(char *dst, int cap, const u8 *src, int n) {
    int k = 0;
    for (int i = 0; i < n && k < cap - 1; i++) { u8 c = src[i]; if (!c) break; dst[k++] = (char)(c < 32 || c > 126 ? '?' : c); }
    while (k > 0 && dst[k - 1] == ' ') k--;
    dst[k] = 0;
}
int part_probe(const BlkDev *d, u64 start, u64 count, char *label, int cap) {
    static u8 tmp[8192 + 4096], b[4096];
    if (label && cap) label[0] = 0;
    if (count == 0) return FS_UNKNOWN;
    if (!rd_bytes(d, start, 0, b, 4096 < count * d->bs ? 4096 : (u32)(count * d->bs), tmp)) return FS_UNKNOWN;
    if (!memcmp(b + 3, "-FVE-FS-", 8)) return FS_BITLOCKER;
    if (!memcmp(b, "LUKS\xBA\xBE", 6)) return FS_LUKS;
    if (!memcmp(b + 3, "NTFS    ", 8)) return FS_NTFS;
    if (!memcmp(b + 3, "EXFAT   ", 8)) return FS_EXFAT;
    if (!memcmp(b + 3, "ReFS", 4)) return FS_REFS;
    if (!memcmp(b, "XFSB", 4)) return FS_XFS;
    if (!memcmp(b + 32, "NXSB", 4)) return FS_APFS;
    if (b[510] == 0x55 && b[511] == 0xAA) {
        if (!memcmp(b + 82, "FAT32   ", 8)) { if (label) copy_label(label, cap, b + 71, 11); if (label && !memcmp(label, "NO NAME", 7)) label[0] = 0; return FS_FAT32; }
        if (!memcmp(b + 54, "FAT16   ", 8)) { if (label) copy_label(label, cap, b + 43, 11); return FS_FAT16; }
        if (!memcmp(b + 54, "FAT12   ", 8)) { if (label) copy_label(label, cap, b + 43, 11); return FS_FAT12; }
    }
    if (count * d->bs > 4096 && !memcmp(b + 4086, "SWAPSPACE2", 10)) return FS_SWAP;
    for (int s = 0; s < 4; s++) if (!memcmp(b + 512 * s, "LABELONE", 8)) return FS_LVM;
    u8 sb[256];
    if (count * d->bs >= 2048 && rd_bytes(d, start, 1024, sb, 256, tmp)) {
        if (rd16(sb + 56) == 0xEF53) { if (label) copy_label(label, cap, sb + 120, 16); return FS_EXT; }
        if (sb[0] == 'H' && (sb[1] == '+' || sb[1] == 'X')) return FS_HFSP;
    }
    if (count * d->bs >= 65536 + 4096 && rd_bytes(d, start, 65536 + 64, sb, 8, tmp) && !memcmp(sb, "_BHRfS_M", 8)) return FS_BTRFS;
    bool zero = true; for (int i = 0; i < 4096 && i < (int)(count * d->bs); i++) if (b[i]) { zero = false; break; }
    return zero ? FS_EMPTY : FS_UNKNOWN;
}
const char *part_fs_name(int fs) {
    static const char *n[] = { "unknown", "empty", "FAT12", "FAT16", "FAT32", "exFAT", "NTFS", "ext2/3/4", "Btrfs", "XFS",
                               "BitLocker", "LUKS", "APFS", "HFS+", "swap", "LVM", "ISO 9660", "ReFS" };
    return fs >= 0 && fs < (int)(sizeof n / sizeof n[0]) ? n[fs] : "unknown";
}
const char *part_type_name(const Part *p, int scheme) {
    if (scheme == SCHEME_GPT) {
        if (!memcmp(p->type, GPT_ESP, 16)) return "EFI system";
        if (!memcmp(p->type, GPT_MSR, 16)) return "Microsoft reserved";
        if (!memcmp(p->type, GPT_WINRE, 16)) return "Windows recovery";
        if (!memcmp(p->type, GPT_MSDATA, 16)) return "Basic data";
        if (!memcmp(p->type, GPT_LINUX, 16)) return "Linux";
        if (!memcmp(p->type, GPT_LINUX_SWAP, 16)) return "Linux swap";
        if (!memcmp(p->type, GPT_LVM, 16)) return "Linux LVM";
        if (!memcmp(p->type, GPT_BIOSBOOT, 16)) return "BIOS boot";
        if (!memcmp(p->type, GPT_APFS, 16)) return "Apple APFS";
        if (!memcmp(p->type, GPT_HFSP, 16)) return "Apple HFS+";
        return "Other";
    }
    switch (p->mbr_type) {
    case 0x01: case 0x04: case 0x06: case 0x0E: return "FAT";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x07: return "NTFS/exFAT";
    case 0x27: return "Windows recovery";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0x8E: return "Linux LVM";
    case 0xEF: return "EFI system";
    case 0xAF: return "Apple HFS+";
    default: return "Other";
    }
}

/* ------------------------------------------------------------ leitura das tabelas */
static u32 entry_secs(const BlkDev *d) { return (128 * 128 + d->bs - 1) / d->bs; }
static bool read_gpt_at(const BlkDev *d, PartTable *t, u64 hdr_lba) {
    static u8 h[4096], ents[128 * 128 + 4096];
    if (!d->rw(d->ctx, hdr_lba, 1, h, false) || memcmp(h, "EFI PART", 8)) return false;
    u32 hs = rd32(h + 12);
    if (hs < 92 || hs > d->bs) return false;
    u32 crc = rd32(h + 16); wr32(h + 16, 0);
    if (crc32_buf(0, h, hs) != crc) return false;
    if (rd64(h + 24) != hdr_lba) return false;
    u64 elba = rd64(h + 72); u32 num = rd32(h + 80), esz = rd32(h + 84), ecrc = rd32(h + 88);
    if (esz < 128 || esz > 1024 || (esz & 7) || num == 0 || (u64)num * esz > sizeof ents - 4096) return false;
    u32 secs = (u32)(((u64)num * esz + d->bs - 1) / d->bs);
    if (elba + secs > d->nsec || !d->rw(d->ctx, elba, secs, ents, false)) return false;
    if (crc32_buf(0, ents, num * esz) != ecrc) return false;
    memcpy(t->disk_guid, h + 56, 16);
    t->n = 0;
    for (u32 i = 0; i < num && t->n < PART_MAX; i++) {
        const u8 *e = ents + (u64)i * esz;
        if (!memcmp(e, ZERO16, 16)) continue;
        Part *p = &t->p[t->n++]; memset(p, 0, sizeof *p);
        memcpy(p->type, e, 16); memcpy(p->guid, e + 16, 16); p->slot = (int)i;
        u64 a = rd64(e + 32), b = rd64(e + 40);
        p->start = a; p->count = b >= a ? b - a + 1 : 0; p->attrs = rd64(e + 48);
        for (int k = 0; k < 36; k++) p->name[k] = rd16(e + 56 + 2 * k);
    }
    return true;
}
static bool is_vbr(const u8 *b) {
    return b[510] == 0x55 && b[511] == 0xAA && (!memcmp(b + 3, "NTFS    ", 8) || !memcmp(b + 3, "EXFAT   ", 8) || !memcmp(b + 82, "FAT32   ", 8) ||
           !memcmp(b + 54, "FAT16   ", 8) || !memcmp(b + 54, "FAT12   ", 8) || !memcmp(b + 3, "-FVE-FS-", 8));
}
static bool is_ext(u8 t) { return t == 0x05 || t == 0x0F || t == 0x85; }
bool part_read(const BlkDev *d, PartTable *t) {
    static u8 m[4096];
    memset(t, 0, sizeof *t);
    if (d->bs != 512 && d->bs != 4096) return false;
    if (!d->rw(d->ctx, 0, 1, m, false)) return false;
    memcpy(t->mbr_code, m, 440); t->mbr_sig = rd32(m + 440);
    bool sig = m[510] == 0x55 && m[511] == 0xAA, prot = false;
    if (sig) for (int i = 0; i < 4; i++) if (m[446 + i * 16 + 4] == 0xEE) prot = true;
    if (prot || !sig) {
        if (read_gpt_at(d, t, 1)) t->scheme = SCHEME_GPT;
        else if (read_gpt_at(d, t, d->nsec - 1)) { t->scheme = SCHEME_GPT; t->gpt_backup_used = true; }
    }
    if (!t->scheme && sig && is_vbr(m)) {
        t->scheme = SCHEME_SUPERFLOPPY; t->n = 1;
        memset(&t->p[0], 0, sizeof t->p[0]); t->p[0].start = 0; t->p[0].count = d->nsec;
    }
    if (!t->scheme && sig && !prot) {
        t->scheme = SCHEME_MBR;
        for (int i = 0; i < 4; i++) {
            const u8 *e = m + 446 + i * 16;
            u8 ty = e[4]; u32 s = rd32(e + 8), c = rd32(e + 12);
            if (!ty || !c) continue;
            if (is_ext(ty)) {
                t->has_extended = true;
                u64 ebr = s; static u8 eb[4096];
                for (int guard = 0; guard < 64 && t->n < PART_MAX; guard++) {
                    if (ebr >= d->nsec || !d->rw(d->ctx, ebr, 1, eb, false) || eb[510] != 0x55 || eb[511] != 0xAA) break;
                    const u8 *l = eb + 446;
                    if (l[4] && rd32(l + 12)) {
                        Part *p = &t->p[t->n++]; memset(p, 0, sizeof *p);
                        p->start = ebr + rd32(l + 8); p->count = rd32(l + 12); p->mbr_type = l[4]; p->logical = true;
                    }
                    const u8 *nx = eb + 446 + 16;
                    if (!is_ext(nx[4]) || !rd32(nx + 8)) break;
                    ebr = s + rd32(nx + 8);
                }
                continue;
            }
            if (t->n >= PART_MAX) break;
            Part *p = &t->p[t->n++]; memset(p, 0, sizeof *p);
            p->start = s; p->count = c; p->mbr_type = ty; p->active = e[0] == 0x80;
        }
    }
    for (int i = 0; i < t->n; i++) {
        Part *p = &t->p[i];
        if (p->start + p->count > d->nsec) p->count = p->start < d->nsec ? d->nsec - p->start : 0;
        p->fs = part_probe(d, p->start, p->count, p->label, sizeof p->label);
    }
    return true;
}

/* ------------------------------------------------------------ espaço livre e edição */
static void usable(const BlkDev *d, const PartTable *t, u64 *lo, u64 *hi) {
    if (t->scheme == SCHEME_GPT) { *lo = 2 + entry_secs(d); *hi = d->nsec - 1 - entry_secs(d); }   /* hi: exclusivo */
    else { *lo = 1; *hi = d->nsec; }
}
static void ext_region(const BlkDev *d, const PartTable *t, u64 *s, u64 *c) {
    *s = *c = 0;
    if (t->scheme != SCHEME_MBR || !t->has_extended) return;
    static u8 m[4096];
    if (!d->rw(d->ctx, 0, 1, m, false)) return;
    for (int i = 0; i < 4; i++) { const u8 *e = m + 446 + i * 16; if (is_ext(e[4])) { *s = rd32(e + 8); *c = rd32(e + 12); } }
}
bool part_overlaps(const BlkDev *d, const PartTable *t, u64 start, u64 count, int ignore) {
    for (int i = 0; i < t->n; i++) {
        if (i == ignore) continue;
        const Part *p = &t->p[i];
        if (start < p->start + p->count && p->start < start + count) return true;
    }
    u64 es, ec; ext_region(d, t, &es, &ec);
    return ec && start < es + ec && es < start + count;
}
int part_free(const BlkDev *d, const PartTable *t, Region *out, int cap) {
    u64 lo, hi; usable(d, t, &lo, &hi);
    u64 al = 1048576 / d->bs;                               /* 1 MiB */
    u64 used_s[PART_MAX + 1], used_e[PART_MAX + 1]; int nu = 0;
    for (int i = 0; i < t->n; i++) { used_s[nu] = t->p[i].start; used_e[nu] = t->p[i].start + t->p[i].count; nu++; }
    u64 es, ec; ext_region(d, t, &es, &ec);
    if (ec) { used_s[nu] = es; used_e[nu] = es + ec; nu++; }
    for (int i = 1; i < nu; i++) for (int j = i; j > 0 && used_s[j] < used_s[j - 1]; j--) {   /* ordena por início */
        u64 a = used_s[j]; used_s[j] = used_s[j - 1]; used_s[j - 1] = a;
        a = used_e[j]; used_e[j] = used_e[j - 1]; used_e[j - 1] = a;
    }
    int n = 0; u64 cur = lo;
    for (int i = 0; i <= nu; i++) {
        u64 next = i < nu ? used_s[i] : hi;
        if (next > hi) next = hi;
        u64 a = (cur + al - 1) / al * al, b = next / al * al;
        if (b > a && b - a >= al && n < cap) { out[n].start = a; out[n].count = b - a; n++; }
        if (i < nu && used_e[i] > cur) cur = used_e[i];
    }
    return n;
}
void part_new(PartTable *t, int scheme, u64 seed) {
    u8 code[440]; memcpy(code, t->mbr_code, 440);
    memset(t, 0, sizeof *t);
    memcpy(t->mbr_code, code, 440);
    t->scheme = scheme;
    new_guid(t->disk_guid, &seed);
    t->mbr_sig = (u32)rng(&seed);
}
int part_add(const BlkDev *d, PartTable *t, u64 start, u64 count, const u8 type[16], u8 mbr_type, const char *name, u64 seed) {
    u64 lo, hi; usable(d, t, &lo, &hi);
    if (t->n >= PART_MAX || count == 0 || start < lo || start + count > hi || part_overlaps(d, t, start, count, -1)) return -1;
    if (t->scheme == SCHEME_MBR) {
        int prim = 0; for (int i = 0; i < t->n; i++) if (!t->p[i].logical) prim++;
        if (t->has_extended) prim++;
        if (prim >= 4 || start > 0xFFFFFFFFull || count > 0xFFFFFFFFull) return -1;
    } else if (t->scheme != SCHEME_GPT) return -1;
    int slot = 0;                                      /* GPT: a primeira posição livre; as outras entradas não se mexem */
    for (bool used = true; used && slot < 128; ) { used = false; for (int i = 0; i < t->n; i++) if (t->p[i].slot == slot) { used = true; slot++; break; } }
    if (t->scheme == SCHEME_GPT && slot >= 128) return -1;
    Part *p = &t->p[t->n]; memset(p, 0, sizeof *p);
    p->start = start; p->count = count; p->mbr_type = mbr_type; p->slot = slot;
    if (type) memcpy(p->type, type, 16);
    new_guid(p->guid, &seed);
    for (int k = 0; name && name[k] && k < 36; k++) p->name[k] = (u8)name[k];
    p->fs = FS_EMPTY;
    return t->n++;
}
bool part_del(PartTable *t, int idx) {
    if (idx < 0 || idx >= t->n || t->p[idx].logical) return false;
    for (int i = idx; i < t->n - 1; i++) t->p[i] = t->p[i + 1];
    t->n--;
    return true;
}

/* ------------------------------------------------------------ gravação */
static bool write_gpt(const BlkDev *d, const PartTable *t) {
    static u8 ents[128 * 128 + 4096], h[4096], m[4096];
    u32 es = entry_secs(d);
    memset(ents, 0, sizeof ents);
    for (int i = 0; i < t->n; i++) {
        const Part *p = &t->p[i];
        if (p->slot < 0 || p->slot >= 128) return false;
        u8 *e = ents + 128 * p->slot;                  /* cada entrada volta à sua posição: os números das partições não mudam */
        memcpy(e, p->type, 16); memcpy(e + 16, p->guid, 16);
        wr64(e + 32, p->start); wr64(e + 40, p->start + p->count - 1); wr64(e + 48, p->attrs);
        for (int c = 0; c < 36; c++) wr16(e + 56 + 2 * c, p->name[c]);
    }
    u32 ecrc = crc32_buf(0, ents, 128 * 128);
    u64 last = d->nsec - 1;
    for (int pass = 0; pass < 2; pass++) {            /* 0: cópia de reserva (fim do disco), 1: principal */
        u64 my = pass ? 1 : last, alt = pass ? last : 1, elba = pass ? 2 : last - es;
        memset(h, 0, d->bs);
        memcpy(h, "EFI PART", 8); wr32(h + 8, 0x00010000); wr32(h + 12, 92);
        wr64(h + 24, my); wr64(h + 32, alt); wr64(h + 40, 2 + es); wr64(h + 48, last - es - 1);
        memcpy(h + 56, t->disk_guid, 16); wr64(h + 72, elba); wr32(h + 80, 128); wr32(h + 84, 128); wr32(h + 88, ecrc);
        wr32(h + 16, crc32_buf(0, h, 92));
        if (!d->rw(d->ctx, elba, es, ents, true) || !d->rw(d->ctx, my, 1, h, true)) return false;
    }
    /* MBR protetor: se já existe um válido (o do Windows, com os CHS que ele calculou), fica como está, só com o
     * código de boot e a assinatura da tabela (que vieram dele mesmo); senão, um novo */
    if (!d->rw(d->ctx, 0, 1, m, false)) return false;
    int prot = 0;
    if (m[510] == 0x55 && m[511] == 0xAA)
        for (int i = 0; i < 4; i++) { u8 *e = m + 446 + i * 16; if (e[4] == 0xEE) prot += rd32(e + 8) == 1 ? 1 : 100; else if (e[4]) prot += 100; }
    if (prot != 1) {
        memset(m, 0, d->bs);
        u8 *e = m + 446; e[1] = 0x00; e[2] = 0x02; e[3] = 0x00; e[4] = 0xEE; e[5] = 0xFF; e[6] = 0xFF; e[7] = 0xFF;
        wr32(e + 8, 1); wr32(e + 12, d->nsec - 1 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)(d->nsec - 1));
        m[510] = 0x55; m[511] = 0xAA;
    }
    memcpy(m, t->mbr_code, 440); wr32(m + 440, t->mbr_sig);
    return d->rw(d->ctx, 0, 1, m, true);
}
static bool write_mbr(const BlkDev *d, const PartTable *t) {
    static u8 old[4096], m[4096];
    if (!d->rw(d->ctx, 0, 1, old, false)) return false;
    memset(m, 0, d->bs);
    memcpy(m, t->mbr_code, 440); wr32(m + 440, t->mbr_sig);
    int ext_slot = -1;
    if (t->has_extended && old[510] == 0x55 && old[511] == 0xAA)
        for (int i = 0; i < 4; i++) if (is_ext(old[446 + i * 16 + 4])) { ext_slot = i; memcpy(m + 446 + i * 16, old + 446 + i * 16, 16); }
    const Part *pr[4]; int np = 0;
    for (int i = 0; i < t->n; i++) if (!t->p[i].logical) { if (np >= 4) return false; pr[np++] = &t->p[i]; }
    for (int i = 1; i < np; i++) for (int j = i; j > 0 && pr[j]->start < pr[j - 1]->start; j--) { const Part *x = pr[j]; pr[j] = pr[j - 1]; pr[j - 1] = x; }
    int slot = 0;
    for (int i = 0; i < np; i++) {
        if (slot == ext_slot) slot++;
        if (slot >= 4 || pr[i]->start > 0xFFFFFFFFull || pr[i]->count > 0xFFFFFFFFull) return false;
        u8 *e = m + 446 + 16 * slot++;
        e[0] = pr[i]->active ? 0x80 : 0; e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF; e[4] = pr[i]->mbr_type; e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
        wr32(e + 8, (u32)pr[i]->start); wr32(e + 12, (u32)pr[i]->count);
    }
    m[510] = 0x55; m[511] = 0xAA;
    /* restos de uma GPT antiga (início e fim do disco) confundiriam o firmware: apaga as assinaturas */
    static u8 g[4096];
    u64 spots[2] = { 1, d->nsec - 1 };
    for (int i = 0; i < 2; i++)
        if (d->rw(d->ctx, spots[i], 1, g, false) && !memcmp(g, "EFI PART", 8)) { memset(g, 0, d->bs); if (!d->rw(d->ctx, spots[i], 1, g, true)) return false; }
    return d->rw(d->ctx, 0, 1, m, true);
}
/* GUID: 4 bytes, 2 e 2 em little-endian, depois 8 bytes na ordem */
static const u8 guid_order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
void part_guid_str(const u8 g[16], char out[37]) {
    static const char hx[] = "0123456789ABCDEF";
    int k = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[k++] = '-';
        u8 b = g[guid_order[i]]; out[k++] = hx[b >> 4]; out[k++] = hx[b & 15];
    }
    out[k] = 0;
}
bool part_guid_parse(const char *s, u8 g[16]) {
    int i = 0, nib = -1;
    for (; *s && i < 16; s++) {
        int v = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (v < 0) { if (*s == '-' && nib < 0) continue; return false; }
        if (nib < 0) nib = v; else { g[guid_order[i++]] = (u8)(nib << 4 | v); nib = -1; }
    }
    return i == 16;
}

bool part_write(const BlkDev *d, const PartTable *t) {
    bool ok = t->scheme == SCHEME_GPT ? write_gpt(d, t) : t->scheme == SCHEME_MBR ? write_mbr(d, t) : false;
    if (ok && d->flush && !d->flush(d->ctx)) ok = false;     /* a tabela nova chega ao disco antes de qualquer outra coisa */
    return ok;
}
