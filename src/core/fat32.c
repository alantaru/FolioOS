/* FolioOS — driver FAT32 (ver fat32.h). Gravação direta (write-through): cada setor alterado vai
 * ao disco na hora, então tirar o pendrive depois de salvar nunca deixa o sistema de arquivos pela metade
 * por causa de um cache. A contagem de livres do FSInfo é mantida em memória e gravada em fat_flush. */
#include "fat32.h"
#include <stddef.h>
void *memcpy(void *, const void *, size_t); void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
static bool in_set(const char *set, int c) { for (; *set; set++) if (*set == c) return true; return false; }

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define EOC 0x0FFFFFFFu
static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }


/* ------------------------------------------------------------ cache de setores (write-through) */
static u8 *cache_get(Fat *f, u64 lba) {
    int victim = 0; u32 oldest = 0xFFFFFFFFu;
    for (int i = 0; i < FAT_CACHE; i++) {
        if (f->c[i].used && f->c[i].lba == lba) { f->c[i].age = ++f->tick; return f->c[i].d; }
        u32 a = f->c[i].used ? f->c[i].age : 0;
        if (a < oldest) { oldest = a; victim = i; }
    }
    if (!f->io(f->ctx, lba, 1, f->c[victim].d, false)) { f->c[victim].used = false; return 0; }
    f->c[victim].used = true; f->c[victim].lba = lba; f->c[victim].age = ++f->tick;
    return f->c[victim].d;
}
static bool cache_write(Fat *f, u64 lba) {
    for (int i = 0; i < FAT_CACHE; i++) if (f->c[i].used && f->c[i].lba == lba) return f->io(f->ctx, lba, 1, f->c[i].d, true);
    return false;
}
static void cache_drop(Fat *f, u64 lba, u32 n) {
    for (int i = 0; i < FAT_CACHE; i++) if (f->c[i].used && f->c[i].lba >= lba && f->c[i].lba < lba + n) f->c[i].used = false;
}

/* ------------------------------------------------------------ tabela FAT */
static u32 fat_get(Fat *f, u32 c) {
    if (c < 2 || c >= f->nclus + 2) return EOC;
    u8 *s = cache_get(f, f->part + f->fat_lba + c / 128);
    return s ? rd32(s + (c % 128) * 4) & 0x0FFFFFFFu : EOC;
}
static bool fat_set(Fat *f, u32 c, u32 v) {
    if (c < 2 || c >= f->nclus + 2) return false;
    u32 copies = f->mirror ? f->nfats : 1;
    for (u32 k = 0; k < copies; k++) {
        u64 lba = f->part + f->fat_lba + (u64)k * f->fat_secs + c / 128;
        u8 *s = cache_get(f, lba); if (!s) return false;
        u8 *p = s + (c % 128) * 4;
        wr32(p, (rd32(p) & 0xF0000000u) | (v & 0x0FFFFFFFu));
        if (!cache_write(f, lba)) return false;
    }
    return true;
}
static u64 clus_lba(Fat *f, u32 c) { return f->part + f->data_lba + (u64)(c - 2) * f->spc; }
static bool is_eoc(u32 v) { return v >= 0x0FFFFFF8u || v < 2; }

static bool zero_cluster(Fat *f, u32 c) {
    static u8 z[512];
    u64 lba = clus_lba(f, c);
    cache_drop(f, lba, f->spc);
    for (u32 i = 0; i < f->spc; i++) if (!f->io(f->ctx, lba + i, 1, z, true)) return false;
    return true;
}
/* novo cluster no fim da cadeia de prev (0 = cadeia nova); devolve 0 se o disco encheu */
static u32 alloc_cluster(Fat *f, u32 prev, bool zero) {
    u32 start = f->hint >= 2 && f->hint < f->nclus + 2 ? f->hint : 2;
    for (u32 k = 0; k < f->nclus; k++) {
        u32 c = start + k; if (c >= f->nclus + 2) c -= f->nclus;
        if (fat_get(f, c) != 0) continue;
        if (!fat_set(f, c, EOC)) return 0;
        if (prev && !fat_set(f, prev, c)) return 0;
        if (zero && !zero_cluster(f, c)) return 0;
        f->hint = c + 1;
        if (f->free_count != 0xFFFFFFFFu && f->free_count) f->free_count--;
        f->dirty_info = true;
        return c;
    }
    return 0;
}
static void free_chain(Fat *f, u32 c) {
    f->memo_d = 0; f->memo_c = 0;
    for (u32 guard = 0; !is_eoc(c) && guard < f->nclus; guard++) {
        u32 n = fat_get(f, c);
        fat_set(f, c, 0);
        if (f->free_count != 0xFFFFFFFFu) f->free_count++;
        if (c < f->hint) f->hint = c;
        c = n;
    }
    f->dirty_info = true;
}
/* cluster de número idx da cadeia do nó (0 se a cadeia acaba antes) */
static u32 nth(Fat *f, FatNode *n, u32 idx) {
    u32 c = n->clus, i = 0;
    if (n->cur_clus >= 2 && n->cur_idx <= idx) { c = n->cur_clus; i = n->cur_idx; }
    if (c < 2) return 0;
    while (i < idx) { u32 nx = fat_get(f, c); if (is_eoc(nx)) return 0; c = nx; i++; }
    n->cur_idx = i; n->cur_clus = c;
    return c;
}

/* ------------------------------------------------------------ montagem */
bool fat_mount(Fat *f, FatIo io, void *ctx, u64 part_lba) {
    memset(f, 0, sizeof *f);
    f->memo_d = 0; f->memo_c = 0;
    f->io = io; f->ctx = ctx; f->part = part_lba;
    u8 *b = cache_get(f, part_lba);
    if (!b || b[510] != 0x55 || b[511] != 0xAA) return false;
    if (rd16(b + 11) != 512 || !b[13] || rd16(b + 17) != 0 || rd16(b + 22) != 0 || !rd32(b + 36)) return false;   /* só FAT32 */
    f->spc = b[13]; f->bpc = f->spc * 512;
    f->fat_lba = rd16(b + 14); f->nfats = b[16] ? b[16] : 1; f->fat_secs = rd32(b + 36);
    u32 total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    f->data_lba = f->fat_lba + f->nfats * f->fat_secs;
    if (total <= f->data_lba) return false;
    f->nclus = (total - f->data_lba) / f->spc;
    if (f->nclus > f->fat_secs * 128 - 2) f->nclus = f->fat_secs * 128 - 2;
    f->root = rd32(b + 44);
    f->mirror = !(rd16(b + 40) & 0x80);
    f->fsinfo = rd16(b + 48);
    memcpy(f->label, b + 71, 11); f->label[11] = 0;
    for (int i = 10; i >= 0 && f->label[i] == ' '; i--) f->label[i] = 0;
    f->free_count = 0xFFFFFFFFu; f->hint = 2;
    if (f->fsinfo && f->fsinfo < f->fat_lba) {
        u8 *s = cache_get(f, part_lba + f->fsinfo);
        if (s && rd32(s) == 0x41615252u && rd32(s + 484) == 0x61417272u) {
            u32 fc = rd32(s + 488), nx = rd32(s + 492);
            if (fc <= f->nclus) f->free_count = fc;
            if (nx >= 2 && nx < f->nclus + 2) f->hint = nx;
        } else f->fsinfo = 0;
    } else f->fsinfo = 0;
    f->ok = true;
    return true;
}
void fat_root(Fat *f, FatNode *n) { memset(n, 0, sizeof *n); n->clus = f->root; n->attr = 0x10; n->ent = -1; n->first = -1; }

uint32_t fat_free_clusters(Fat *f) {
    if (f->free_count != 0xFFFFFFFFu) return f->free_count;
    static u8 buf[32 * 512];
    u32 cnt = 0;
    for (u32 s = 0; s < f->fat_secs; s += 32) {
        u32 k = f->fat_secs - s < 32 ? f->fat_secs - s : 32;
        if (!f->io(f->ctx, f->part + f->fat_lba + s, k, buf, false)) return 0;
        for (u32 i = 0; i < k * 128; i++) {
            u32 c = s * 128 + i;
            if (c >= 2 && c < f->nclus + 2 && (rd32(buf + i * 4) & 0x0FFFFFFFu) == 0) cnt++;
        }
    }
    f->free_count = cnt;
    return cnt;
}
int fat_flush(Fat *f) {
    if (!f->dirty_info || !f->fsinfo) return FAT_OK;
    u64 lba = f->part + f->fsinfo;
    u8 *s = cache_get(f, lba); if (!s) return FAT_IO;
    wr32(s + 488, f->free_count); wr32(s + 492, f->hint);
    f->dirty_info = false;
    return cache_write(f, lba) ? FAT_OK : FAT_IO;
}

/* ------------------------------------------------------------ entradas de pasta */
/* ponteiro para a entrada idx da pasta (cadeia que começa em dclus); grow = cria cluster novo se faltar */
static u8 *dirent(Fat *f, u32 dclus, u32 idx, u64 *lba, bool grow) {
    u32 per = f->bpc / 32, ci = idx / per;
    u32 c = dclus, i = 0;
    if (f->memo_d == dclus && f->memo_c >= 2 && f->memo_i <= ci) { c = f->memo_c; i = f->memo_i; }
    while (i < ci) {
        u32 nx = fat_get(f, c);
        if (is_eoc(nx)) {
            if (!grow || ci - i > 1 || idx >= 65536) return 0;
            nx = alloc_cluster(f, c, true); if (!nx) return 0;
        }
        c = nx; i++;
    }
    f->memo_d = dclus; f->memo_i = ci; f->memo_c = c;
    u32 off = (idx % per) * 32;
    *lba = clus_lba(f, c) + off / 512;
    u8 *s = cache_get(f, *lba);
    return s ? s + off % 512 : 0;
}
static void invalidate_memo(Fat *f) { f->memo_d = 0; f->memo_c = 0; }
static u8 sfn_sum(const u8 *n) { u8 s = 0; for (int i = 0; i < 11; i++) s = (u8)(((s & 1) << 7) + (s >> 1) + n[i]); return s; }
static u16 fold(u16 c) {
    if (c >= 'a' && c <= 'z') return (u16)(c - 32);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (u16)(c - 32);
    return c;
}
static bool name_eq(const u16 *a, const u16 *b) {
    for (; *a && *b; a++, b++) if (fold(*a) != fold(*b)) return false;
    return *a == *b;
}
static int wlen(const u16 *s) { int n = 0; while (s[n]) n++; return n; }
static void fill_node(Fat *f, const u8 *e, u32 dclus, u32 idx, u32 first, FatNode *n) {
    memset(n, 0, sizeof *n);
    n->clus = (u32)rd16(e + 20) << 16 | rd16(e + 26);
    n->size = rd32(e + 28); n->attr = e[11];
    n->dir_clus = dclus; n->ent = (int32_t)idx; n->first = (int32_t)first;
    n->wtime = rd16(e + 22); n->wdate = rd16(e + 24);
    if ((n->attr & 0x10) && n->clus == 0) n->clus = f->root;    /* ".." que aponta para a raiz */
}

int fat_readdir(Fat *f, const FatNode *dir, uint32_t *cookie, uint16_t name[256], FatNode *out) {
    u16 lfn[260]; int lfn_n = 0; u8 lfn_sum = 0; u32 lfn_first = 0; int lfn_next = 0;
    for (u32 idx = *cookie; ; idx++) {
        u64 lba; u8 *e = dirent(f, dir->clus, idx, &lba, false);
        if (!e || e[0] == 0) { *cookie = idx; return 0; }
        if (e[0] == 0xE5) { lfn_n = 0; continue; }
        if (e[11] == 0x0F) {
            int ord = e[0] & 0x1F;
            if (e[0] & 0x40) { lfn_n = ord; lfn_sum = e[13]; lfn_first = idx; lfn_next = ord; memset(lfn, 0, sizeof lfn); }
            if (!lfn_n || ord != lfn_next || e[13] != lfn_sum || ord < 1 || ord > 20) { lfn_n = 0; continue; }
            static const u8 at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int k = 0; k < 13; k++) lfn[(ord - 1) * 13 + k] = rd16(e + at[k]);
            lfn_next = ord - 1;
            continue;
        }
        if (e[11] & 0x08) { lfn_n = 0; continue; }                         /* rótulo do volume */
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) { lfn_n = 0; continue; }
        u32 first = idx;
        if (lfn_n && lfn_next == 0 && sfn_sum(e) == lfn_sum) {
            int k = 0; while (k < 255 && lfn[k] && lfn[k] != 0xFFFF) { name[k] = lfn[k]; k++; }
            name[k] = 0; first = lfn_first;
        } else {
            int k = 0; bool lb = e[12] & 0x08, le = e[12] & 0x10;
            for (int i = 0; i < 8 && e[i] != ' '; i++) { u8 c = (u8)(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]); name[k++] = (u16)(lb && c >= 'A' && c <= 'Z' ? c + 32 : c); }
            if (e[8] != ' ') { name[k++] = '.'; for (int i = 8; i < 11 && e[i] != ' '; i++) name[k++] = (u16)(le && e[i] >= 'A' && e[i] <= 'Z' ? e[i] + 32 : e[i]); }
            name[k] = 0;
        }
        lfn_n = 0;
        fill_node(f, e, dir->clus, idx, first, out);
        *cookie = idx + 1;
        return 1;
    }
}

int fat_lookup(Fat *f, const FatNode *dir, const uint16_t *name, FatNode *out) {
    if (!(dir->attr & 0x10)) return FAT_NOTFOUND;
    FatNode d = *dir;                                  /* out pode ser a própria pasta (fat_lookup(f, &n, w, &n)) */
    u16 nm[256]; u32 ck = 0;
    while (fat_readdir(f, &d, &ck, nm, out) == 1) if (name_eq(nm, name)) return FAT_OK;
    return FAT_NOTFOUND;
}

/* ------------------------------------------------------------ criação de entradas */
static bool sfn_char(u16 c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c < 128 && c > 32 && in_set("!#$%&'()-@^_`{}~", (int)c)); }
static bool sfn_taken(Fat *f, u32 dclus, const u8 *sfn) {
    for (u32 idx = 0; ; idx++) {
        u64 lba; u8 *e = dirent(f, dclus, idx, &lba, false);
        if (!e || e[0] == 0) return false;
        /* o rótulo do volume não é um nome: a pasta FOLIO convive com o rótulo "FOLIO" (como no Windows) */
        if (e[0] != 0xE5 && e[11] != 0x0F && !(e[11] & 0x08) && !memcmp(e, sfn, 11)) return true;
    }
}
/* nome curto 8.3; devolve true se o nome inteiro cabe nele (então não precisa de nome longo) */
static bool make_sfn(Fat *f, u32 dclus, const u16 *name, u8 sfn[11]) {
    int n = wlen(name), dot = -1;
    for (int i = n - 1; i > 0; i--) if (name[i] == '.') { dot = i; break; }
    int blen = dot < 0 ? n : dot, elen = dot < 0 ? 0 : n - dot - 1;
    bool exact = blen >= 1 && blen <= 8 && elen <= 3;
    for (int i = 0; i < n && exact; i++) if (i != dot && !sfn_char(name[i])) exact = false;
    memset(sfn, ' ', 11);
    if (exact) {
        for (int i = 0; i < blen; i++) sfn[i] = (u8)name[i];
        for (int i = 0; i < elen; i++) sfn[8 + i] = (u8)name[dot + 1 + i];
        if (sfn[0] == 0xE5) sfn[0] = 0x05;
        if (!sfn_taken(f, dclus, sfn)) return true;
    }
    u8 base[8]; int bl = 0;
    for (int i = 0; i < blen && bl < 8; i++) {
        u16 c = fold(name[i]);
        if (c == ' ' || c == '.') continue;
        base[bl++] = (u8)(sfn_char(c) ? c : '_');
    }
    if (!bl) base[bl++] = '_';
    memset(sfn, ' ', 11);
    for (int i = 0, k = 0; i < elen && k < 3; i++) { u16 c = fold(name[dot + 1 + i]); if (c == ' ') continue; sfn[8 + k++] = (u8)(sfn_char(c) ? c : '_'); }
    for (u32 num = 1; num < 1000000; num++) {
        char tail[8]; int tl = 0; u32 v = num; char rev[8]; int rl = 0;
        while (v) { rev[rl++] = (char)('0' + v % 10); v /= 10; }
        tail[tl++] = '~'; while (rl) tail[tl++] = rev[--rl];
        int keep = bl < 8 - tl ? bl : 8 - tl;
        memset(sfn, ' ', 8);
        for (int i = 0; i < keep; i++) sfn[i] = base[i];
        for (int i = 0; i < tl; i++) sfn[keep + i] = (u8)tail[i];
        if (!sfn_taken(f, dclus, sfn)) return false;
    }
    return false;
}
static int check_name(const u16 *name) {
    int n = wlen(name);
    if (n == 0 || n > 255) return FAT_BADNAME;
    if (name[0] == '.' && (n == 1 || (n == 2 && name[1] == '.'))) return FAT_BADNAME;
    if (name[n - 1] == '.' || name[n - 1] == ' ') return FAT_BADNAME;
    for (int i = 0; i < n; i++) { u16 c = name[i]; if (c < 32 || (c < 128 && in_set("\"*/:<>?\\|", (int)c))) return FAT_BADNAME; }
    return FAT_OK;
}
/* grava a sequência (nome longo + entrada curta) numa pasta; tmpl traz os campos da entrada curta */
static int put_entries(Fat *f, u32 dclus, const u16 *name, const u8 tmpl[32], FatNode *out) {
    u8 sfn[11];
    bool plain = make_sfn(f, dclus, name, sfn);
    int len = wlen(name), nl = plain ? 0 : (len + 12) / 13, need = nl + 1;
    u32 run = 0, start = 0;
    u32 idx;
    for (idx = 0; ; idx++) {
        u64 lba; u8 *e = dirent(f, dclus, idx, &lba, true);
        if (!e) return FAT_FULL;
        if (e[0] == 0 || e[0] == 0xE5) { if (!run) start = idx; if (++run == (u32)need) break; }
        else run = 0;
    }
    u8 sum = sfn_sum(sfn);
    for (int k = nl; k >= 1; k--) {
        u64 lba; u8 *e = dirent(f, dclus, start + (u32)(nl - k), &lba, true); if (!e) return FAT_IO;
        memset(e, 0, 32);
        e[0] = (u8)(k | (k == nl ? 0x40 : 0)); e[11] = 0x0F; e[13] = sum;
        static const u8 at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        for (int j = 0; j < 13; j++) {
            int p = (k - 1) * 13 + j;
            wr16(e + at[j], p < len ? name[p] : p == len ? 0 : 0xFFFF);
        }
        if (!cache_write(f, lba)) return FAT_IO;
    }
    u64 lba; u8 *e = dirent(f, dclus, start + (u32)nl, &lba, true); if (!e) return FAT_IO;
    memcpy(e, tmpl, 32); memcpy(e, sfn, 11);
    if (!cache_write(f, lba)) return FAT_IO;
    fill_node(f, e, dclus, start + (u32)nl, start, out);
    return FAT_OK;
}
static void stamp(Fat *f, u8 *e, bool created) {
    u16 d = (u16)((46 << 9) | (1 << 5) | 1), t = 0;     /* 2026-01-01 sem relógio */
    if (f->now) f->now(&d, &t);
    if (created) { e[13] = 0; wr16(e + 14, t); wr16(e + 16, d); }
    wr16(e + 18, d); wr16(e + 22, t); wr16(e + 24, d);
}
static int mark_deleted(Fat *f, u32 dclus, u32 from, u32 to) {
    for (u32 i = from; i <= to; i++) {
        u64 lba; u8 *e = dirent(f, dclus, i, &lba, false); if (!e) return FAT_IO;
        e[0] = 0xE5;
        if (!cache_write(f, lba)) return FAT_IO;
    }
    return FAT_OK;
}

int fat_create(Fat *f, const FatNode *dir, const uint16_t *name, bool is_dir, FatNode *out) {
    int r = check_name(name); if (r) return r;
    if (!(dir->attr & 0x10)) return FAT_NOTFOUND;
    FatNode tmp;
    if (fat_lookup(f, dir, name, &tmp) == FAT_OK) return FAT_EXISTS;
    u8 t[32]; memset(t, 0, 32);
    t[11] = is_dir ? 0x10 : 0x20;
    stamp(f, t, true);
    u32 c = 0;
    if (is_dir) {
        c = alloc_cluster(f, 0, true); if (!c) return FAT_FULL;
        wr16(t + 20, (u16)(c >> 16)); wr16(t + 26, (u16)c);
    }
    r = put_entries(f, dir->clus, name, t, out);
    if (r) { if (c) free_chain(f, c); return r; }
    if (is_dir) {
        u64 lba = clus_lba(f, c);
        u8 *s = cache_get(f, lba); if (!s) return FAT_IO;
        memcpy(s, t, 32); memcpy(s, ".          ", 11);
        memcpy(s + 32, t, 32); memcpy(s + 32, "..         ", 11);
        u32 pc = dir->clus == f->root ? 0 : dir->clus;
        wr16(s + 32 + 20, (u16)(pc >> 16)); wr16(s + 32 + 26, (u16)pc);
        if (!cache_write(f, lba)) return FAT_IO;
    }
    return FAT_OK;
}

/* atualiza cluster, tamanho e data de modificação na entrada do nó */
static int update_entry(Fat *f, FatNode *n) {
    if (n->ent < 0) return FAT_OK;
    u64 lba; u8 *e = dirent(f, n->dir_clus, (u32)n->ent, &lba, false); if (!e) return FAT_IO;
    u32 c = (n->attr & 0x10) && n->clus == f->root ? 0 : n->clus;
    wr16(e + 20, (u16)(c >> 16)); wr16(e + 26, (u16)c);
    if (!(n->attr & 0x10)) wr32(e + 28, n->size);
    stamp(f, e, false);
    n->wtime = rd16(e + 22); n->wdate = rd16(e + 24);
    return cache_write(f, lba) ? FAT_OK : FAT_IO;
}

/* ------------------------------------------------------------ dados */
int fat_read(Fat *f, FatNode *n, uint32_t pos, void *buf, uint32_t len) {
    if (n->attr & 0x10) return FAT_BADNAME;
    if (pos >= n->size) return 0;
    if (len > n->size - pos) len = n->size - pos;
    u8 *out = buf; u32 done = 0;
    while (done < len) {
        u32 p = pos + done, ci = p / f->bpc, co = p % f->bpc;
        u32 c = nth(f, n, ci); if (c < 2) break;
        u64 lba = clus_lba(f, c) + co / 512; u32 so = co % 512;
        if (so || len - done < 512) {
            u8 *s = cache_get(f, lba); if (!s) return FAT_IO;
            u32 k = 512 - so; if (k > len - done) k = len - done;
            memcpy(out + done, s + so, k); done += k;
        } else {
            u32 want = (len - done) / 512, secs = (f->bpc - co) / 512, cc = c, idx = ci;
            while (secs < want) { u32 nx = fat_get(f, cc); if (nx != cc + 1) break; cc = nx; idx++; secs += f->spc; }
            if (secs > want) secs = want;
            for (u32 s = 0; s < secs; ) {
                u32 k = secs - s > 128 ? 128 : secs - s;
                if (!f->io(f->ctx, lba + s, k, out + done + s * 512, false)) return FAT_IO;
                s += k;
            }
            done += secs * 512;
            n->cur_idx = idx; n->cur_clus = cc;
        }
    }
    return (int)done;
}

/* garante clusters suficientes para `bytes` */
static int ensure_size(Fat *f, FatNode *n, u32 bytes) {
    u32 need = (bytes + f->bpc - 1) / f->bpc; if (!need) return FAT_OK;
    if (n->clus < 2) {
        u32 c = alloc_cluster(f, 0, false); if (!c) return FAT_FULL;
        n->clus = c; n->cur_idx = 0; n->cur_clus = c;
    }
    u32 have = 1, c = n->clus;
    if (n->cur_clus >= 2) { have = n->cur_idx + 1; c = n->cur_clus; }
    for (;;) { u32 nx = fat_get(f, c); if (is_eoc(nx)) break; c = nx; have++; }
    n->cur_idx = have - 1; n->cur_clus = c;
    while (have < need) {
        u32 nc = alloc_cluster(f, c, false); if (!nc) return FAT_FULL;
        c = nc; have++; n->cur_idx = have - 1; n->cur_clus = c;
    }
    return FAT_OK;
}
static int write_raw(Fat *f, FatNode *n, u32 pos, const u8 *in, u32 len) {
    u32 done = 0;
    while (done < len) {
        u32 p = pos + done, ci = p / f->bpc, co = p % f->bpc;
        u32 c = nth(f, n, ci); if (c < 2) return FAT_IO;
        u64 lba = clus_lba(f, c) + co / 512; u32 so = co % 512;
        if (so || len - done < 512) {
            u8 *s = cache_get(f, lba); if (!s) return FAT_IO;
            u32 k = 512 - so; if (k > len - done) k = len - done;
            if (in) memcpy(s + so, in + done, k); else memset(s + so, 0, k);
            if (!cache_write(f, lba)) return FAT_IO;
            done += k;
        } else {
            u32 secs = (f->bpc - co) / 512, want = (len - done) / 512;
            if (secs > want) secs = want;
            cache_drop(f, lba, secs);
            if (in) { if (!f->io(f->ctx, lba, secs, (void *)(in + done), true)) return FAT_IO; }
            else { static u8 z[512]; for (u32 s = 0; s < secs; s++) if (!f->io(f->ctx, lba + s, 1, z, true)) return FAT_IO; }
            done += secs * 512;
        }
    }
    return FAT_OK;
}
int fat_write(Fat *f, FatNode *n, uint32_t pos, const void *buf, uint32_t len) {
    if (n->attr & 0x10) return FAT_BADNAME;
    if (!len) return 0;
    if ((u64)pos + len > 0xFFFFFFFFull) return FAT_FULL;
    u32 end = pos + len;
    int r = ensure_size(f, n, end > n->size ? end : n->size); if (r) return r;
    if (pos > n->size) { r = write_raw(f, n, n->size, 0, pos - n->size); if (r) return r; }
    r = write_raw(f, n, pos, buf, len); if (r) return r;
    if (end > n->size) n->size = end;
    r = update_entry(f, n); if (r) return r;
    return (int)len;
}
int fat_truncate(Fat *f, FatNode *n, uint32_t size) {
    if (n->attr & 0x10) return FAT_BADNAME;
    if (size == n->size) return FAT_OK;
    if (size > n->size) {
        int r = ensure_size(f, n, size); if (r) return r;
        r = write_raw(f, n, n->size, 0, size - n->size); if (r) return r;
        n->size = size;
        return update_entry(f, n);
    }
    u32 keep = (size + f->bpc - 1) / f->bpc;
    n->cur_idx = 0; n->cur_clus = 0;
    if (keep == 0) { if (n->clus >= 2) free_chain(f, n->clus); n->clus = 0; }
    else {
        u32 c = nth(f, n, keep - 1);
        if (c >= 2) { u32 nx = fat_get(f, c); if (!is_eoc(nx)) { fat_set(f, c, EOC); free_chain(f, nx); } }
        n->cur_idx = 0; n->cur_clus = 0;
    }
    n->size = size;
    return update_entry(f, n);
}

/* ------------------------------------------------------------ apagar e renomear */
int fat_delete(Fat *f, FatNode *n) {
    if (n->ent < 0) return FAT_BADNAME;
    if (n->attr & 0x10) {
        u16 nm[256]; FatNode ch; u32 ck = 0;
        if (fat_readdir(f, n, &ck, nm, &ch) == 1) return FAT_NOTEMPTY;
    }
    int r = mark_deleted(f, n->dir_clus, (u32)n->first, (u32)n->ent); if (r) return r;
    if (n->clus >= 2 && n->clus != f->root) free_chain(f, n->clus);
    invalidate_memo(f);
    return FAT_OK;
}
int fat_rename(Fat *f, FatNode *n, const uint16_t *newname) {
    if (n->ent < 0) return FAT_BADNAME;
    int r = check_name(newname); if (r) return r;
    FatNode dir; memset(&dir, 0, sizeof dir); dir.clus = n->dir_clus; dir.attr = 0x10;
    FatNode other;
    if (fat_lookup(f, &dir, newname, &other) == FAT_OK && other.ent != n->ent) return FAT_EXISTS;
    u64 lba; u8 *e = dirent(f, n->dir_clus, (u32)n->ent, &lba, false); if (!e) return FAT_IO;
    u8 t[32]; memcpy(t, e, 32); t[12] = 0;
    u32 oldf = (u32)n->first, olde = (u32)n->ent;
    if (olde - oldf > 20) return FAT_IO;
    u8 first_bytes[21];
    for (u32 i = oldf; i <= olde; i++) { u8 *x = dirent(f, n->dir_clus, i, &lba, false); if (!x) return FAT_IO; first_bytes[i - oldf] = x[0]; }
    r = mark_deleted(f, n->dir_clus, oldf, olde); if (r) return r;        /* libera antes: o novo nome pode ocupar o mesmo lugar */
    FatNode nn;
    r = put_entries(f, n->dir_clus, newname, t, &nn);
    if (r) {   /* sem espaço: devolve a entrada antiga como estava */
        for (u32 i = oldf; i <= olde; i++) { u8 *x = dirent(f, n->dir_clus, i, &lba, false); if (x) { x[0] = first_bytes[i - oldf]; cache_write(f, lba); } }
        return r;
    }
    n->ent = nn.ent; n->first = nn.first;
    return FAT_OK;
}

/* ------------------------------------------------------------ formatação */
/* Formata a região [part_lba, part_lba + nsec) como FAT32 (setores de 512 bytes). Grava o setor de boot
 * (e a cópia no setor 6), o FSInfo, as duas FATs zeradas e a pasta raiz vazia com o rótulo. */
bool fat32_format(FatIo io, void *ctx, uint64_t part_lba, uint64_t nsec, const char *label, uint32_t serial) {
    static u8 z[64 * 512];
    if (nsec > 0xFFFFFFFFull) nsec = 0xFFFFFFFFull;
    u64 bytes = nsec * 512;
    u32 spc = bytes <= 8ull << 30 ? 8 : bytes <= 16ull << 30 ? 16 : bytes <= 32ull << 30 ? 32 : 64;
    const u32 rsvd = 32;
    u32 fatsz = 0, nclus = 0;
    for (;; spc /= 2) {
        u32 t2 = (256 * spc + 2) / 2;
        fatsz = (u32)((nsec - rsvd + t2 - 1) / t2);
        nclus = (u32)((nsec - rsvd - 2ull * fatsz) / spc);
        if (nclus >= 65525 || spc == 1) break;
    }
    if (nclus < 65525 || nclus > 0x0FFFFFF5) return false;
    memset(z, 0, sizeof z);
    for (u64 s = 0; s < rsvd + 2ull * fatsz; s += 64) {            /* reservados e FATs zerados */
        u32 k = rsvd + 2ull * fatsz - s < 64 ? (u32)(rsvd + 2ull * fatsz - s) : 64;
        if (!io(ctx, part_lba + s, k, z, true)) return false;
    }
    u64 data = rsvd + 2ull * fatsz;
    for (u32 s = 0; s < spc; s++) if (!io(ctx, part_lba + data + s, 1, z, true)) return false;   /* raiz (cluster 2) */
    u8 bs[512]; memset(bs, 0, 512);
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90; memcpy(bs + 3, "MSWIN4.1", 8);
    wr16(bs + 11, 512); bs[13] = (u8)spc; wr16(bs + 14, (u16)rsvd); bs[16] = 2; bs[21] = 0xF8;
    wr16(bs + 24, 63); wr16(bs + 26, 255); wr32(bs + 28, part_lba > 0xFFFFFFFFull ? 0 : (u32)part_lba); wr32(bs + 32, (u32)nsec);
    wr32(bs + 36, fatsz); wr32(bs + 44, 2); wr16(bs + 48, 1); wr16(bs + 50, 6);
    bs[64] = 0x80; bs[66] = 0x29; wr32(bs + 67, serial);
    u8 lab[11]; memset(lab, ' ', 11);
    for (int i = 0; label && label[i] && i < 11; i++) { char c = label[i]; lab[i] = (u8)(c >= 'a' && c <= 'z' ? c - 32 : c); }
    memcpy(bs + 71, lab, 11); memcpy(bs + 82, "FAT32   ", 8);
    /* sem sistema: o código só avisa (o instalador grava o código de boot BIOS depois, se for o caso) */
    static const u8 code[] = { 0xFB, 0x31, 0xC0, 0x8E, 0xD8, 0xBE, 0x77, 0x7C, 0xFC, 0xAC, 0x84, 0xC0, 0x74, 0x06, 0xB4, 0x0E, 0xCD, 0x10, 0xEB, 0xF5, 0xF4, 0xEB, 0xFD };
    memcpy(bs + 90, code, sizeof code);
    static const char msg[] = "This volume has no system to start.\r\n";
    memcpy(bs + 0x77, msg, sizeof msg);
    bs[510] = 0x55; bs[511] = 0xAA;
    u8 fsi[512]; memset(fsi, 0, 512);
    wr32(fsi, 0x41615252); wr32(fsi + 484, 0x61417272); wr32(fsi + 488, nclus - 1); wr32(fsi + 492, 3); wr32(fsi + 508, 0xAA550000);
    u8 fat0[512]; memset(fat0, 0, 512);
    wr32(fat0, 0x0FFFFFF8); wr32(fat0 + 4, 0x0FFFFFFF); wr32(fat0 + 8, 0x0FFFFFFF);
    u8 root[512]; memset(root, 0, 512);
    if (label && label[0]) { memcpy(root, lab, 11); root[11] = 0x08; }
    return io(ctx, part_lba + 1, 1, fsi, true) && io(ctx, part_lba + 7, 1, fsi, true) &&
           io(ctx, part_lba + rsvd, 1, fat0, true) && io(ctx, part_lba + rsvd + fatsz, 1, fat0, true) &&
           io(ctx, part_lba + data, 1, root, true) &&
           io(ctx, part_lba + 6, 1, bs, true) && io(ctx, part_lba, 1, bs, true);   /* o setor de boot por último */
}
