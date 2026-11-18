/* FolioOS — motor de instalação (ver install.h).
 * Ordem: lista a origem e confere que tem o sistema -> cópia de segurança do início e do fim do disco no
 * pendrive -> tabela nova (GPT ou MBR) com uma partição FAT32 -> formatação -> cópia dos arquivos (com
 * CRC) -> código de boot BIOS, se for o caso -> remonta o volume do zero e confere cada arquivo pelo CRC. */
#include "install.h"
#include "fat32.h"
#include "fscheck.h"
#include <stddef.h>
void *memcpy(void *, const void *, size_t); void *memset(void *, int, size_t); int memcmp(const void *, const void *, size_t);

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

typedef struct { char path[160]; bool dir; u64 size; u32 crc; } Ent;
#define MAXENT 4000
static Ent ents[MAXENT]; static int nent;
static const InstallSrc *S;
static const BlkDev *D;

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static bool seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static bool sieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) { char x = *a, y = *b; if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; if (x != y) return false; }
    return *a == *b;
}
static void scopy(char *d, const char *s, int cap) { int i = 0; for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; }
static void scat(char *d, const char *s, int cap) { int n = slen(d); scopy(d + n, s, cap - n); }
static void set_err(InstallResult *r, int code, const char *path) { r->code = code; scopy(r->path, path ? path : "", sizeof r->path); }

/* UTF-8 -> UTF-16 (plano básico) */
static void to_w(const char *s, u16 *w, int cap) {
    const u8 *p = (const u8 *)s; int k = 0;
    while (*p && k < cap - 1) {
        u32 c = *p++;
        if (c >= 0xF0) { c = (c & 7) << 18; c |= (u32)(*p++ & 63) << 12; c |= (u32)(*p++ & 63) << 6; c |= *p++ & 63; c = '?'; }
        else if (c >= 0xE0) { c = (c & 15) << 12; c |= (u32)(*p++ & 63) << 6; c |= *p++ & 63; }
        else if (c >= 0xC0) { c = (c & 31) << 6; c |= *p++ & 63; }
        w[k++] = (u16)c;
    }
    w[k] = 0;
}

/* ------------------------------------------------------------ o que copiar */
static const InstallOpts *O;
static char cur_dir[160];
static bool updating;                             /* install_update: só o programa (nada do que é da pessoa) */
static bool skip(const char *path, bool dir) {
    /* não vão para o disco: cópias de segurança, o diagnóstico e (com os textos no pendrive) os textos */
    if (sieq(path, "FOLIO/BACKUP") || sieq(path, "FOLIO/DIAGNOSTICS.TXT") || sieq(path, "System Volume Information")) return true;
    bool user = sieq(path, "DOCUMENTS") || sieq(path, "MUSIC") || sieq(path, "FOLIO/VERSIONS") || sieq(path, "FOLIO/TABS") || sieq(path, "FOLIO/DRAFT.MD");
    if (O->docs == INST_DOCS_STICK && user) return true;
    /* atualizar: os textos, a música, os ajustes e o que o próprio computador gravou ficam como estão */
    if (updating && (user || sieq(path, "FOLIO/CONFIG.INI") || sieq(path, "FOLIO/INSTALLED.ID"))) return true;
    (void)dir;
    return false;
}
static void on_entry(void *u, const char *name, bool is_dir, u64 size) {
    (void)u;
    if (nent >= MAXENT || seq(name, ".") || seq(name, "..")) return;
    char p[160]; scopy(p, cur_dir, sizeof p);
    if (p[0]) scat(p, "/", sizeof p);
    scat(p, name, sizeof p);
    if (skip(p, is_dir)) return;
    Ent *e = &ents[nent++]; scopy(e->path, p, sizeof e->path); e->dir = is_dir; e->size = size; e->crc = 0;
}
static bool walk(void) {
    nent = 0;
    for (int i = -1; i < nent; i++) {                  /* em largura: cada pasta achada é listada depois */
        if (i >= 0 && !ents[i].dir) continue;
        scopy(cur_dir, i < 0 ? "" : ents[i].path, sizeof cur_dir);
        if (!S->list(S->ctx, cur_dir, on_entry, 0)) return false;
        if (nent >= MAXENT) return false;
    }
    return true;
}
static bool has(const char *path) { for (int i = 0; i < nent; i++) if (sieq(ents[i].path, path)) return true; return false; }

/* ------------------------------------------------------------ disco de destino */
static bool dio(void *ctx, u64 lba, u32 n, void *buf, bool w) { (void)ctx; return D->rw(D->ctx, lba, n, buf, w); }
static FatNode dir_for(Fat *f, const char *path, bool *ok) {
    /* pasta-mãe de path (as pastas já foram criadas antes, na ordem da lista) */
    FatNode cur; fat_root(f, &cur); *ok = true;
    char part[128]; const char *s = path;
    for (;;) {
        int k = 0; while (*s && *s != '/' && k < 127) part[k++] = *s++; part[k] = 0;
        if (!*s) return cur;
        s++;
        u16 w[128]; to_w(part, w, 128);
        FatNode nx; if (fat_lookup(f, &cur, w, &nx) != FAT_OK) { *ok = false; return cur; }
        cur = nx;
    }
}
static const char *base_name(const char *p) { const char *b = p; for (; *p; p++) if (*p == '/') b = p + 1; return b; }

/* ------------------------------------------------------------ etapas comuns */
static u8 *B; static u32 BSZ;
static u64 total, done;
static void prog(int pct, const char *what) { if (S->progress) S->progress(S->ctx, pct, what); }

/* 1 e 2: a origem tem o sistema? cópia de segurança do começo (MBR + GPT) e do fim (GPT de reserva) do disco */
/* a cópia de segurança do começo (MBR + GPT) e do fim (GPT de reserva) do disco, em FOLIO/BACKUP/disk-<data>.bin
 * no pendrive: um cabeçalho de 512 bytes ("FOLIOBAK" + setores do disco), os 34 primeiros e os 33 últimos setores */
static bool save_disk_start(const BlkDev *dev, const InstallSrc *src, InstallResult *r) {
    u32 n = 34; u8 *b = B;
    memcpy(b, "FOLIOBAK", 8); for (int i = 0; i < 8; i++) b[8 + i] = (u8)(dev->nsec >> (8 * i));
    memset(b + 16, 0, 512 - 16);
    if (!dev->rw(dev->ctx, 0, n, b + 512, false) || !dev->rw(dev->ctx, dev->nsec - 33, 33, b + 512 + n * 512, false)) { set_err(r, INST_ERR_READ, "disk"); return false; }
    char name[64] = "disk-"; scat(name, src->date, sizeof name); scat(name, ".bin", sizeof name);
    if (!src->backup || !src->backup(src->ctx, name, b, 512 + (n + 33) * 512)) { set_err(r, INST_ERR_BACKUP, name); return false; }
    return true;
}
static bool prepare(const BlkDev *dev, const InstallSrc *src, const InstallOpts *o, InstallResult *r) {
    memset(r, 0, sizeof *r);
    S = src; D = dev; O = o;
    B = src->buf; BSZ = src->bufsize & ~511u;
    if (!B || BSZ < 65536) { set_err(r, INST_ERR_SOURCE, "buffer"); return false; }
    if (dev->bs != 512) { set_err(r, INST_ERR_WRITE, "4K sectors"); return false; }
    prog(0, "source");
    if (!walk()) { set_err(r, INST_ERR_READ, cur_dir); return false; }
    /* o mínimo para iniciar; pastas de fontes/idiomas podem ter nomes antigos (FOLIO/FONTES…) e vão como estão */
    const char *need[] = { "EFI/BOOT", "FOLIO", o->bios ? "FOLIO/BIOS.SYS" : "EFI/BOOT", o->bios ? "EFI/BOOT/BOOTIA32.EFI" : "EFI/BOOT" };
    for (int i = 0; i < 4; i++) if (!has(need[i])) { set_err(r, INST_ERR_SOURCE, need[i]); return false; }
    total = 0; done = 0;
    for (int i = 0; i < nent; i++) if (!ents[i].dir) total += ents[i].size;
    prog(1, "backup");
    return save_disk_start(dev, src, r);
}

/* 4 e 5: formata a partição do FolioOS e copia tudo, guardando o CRC de cada arquivo */
static Fat f;
static bool format_and_copy(InstallResult *r, int pct_from, int pct_to) {
    prog(pct_from, "format");
    if (!fat32_format(dio, 0, r->part_lba, r->part_count, O->label ? O->label : "FOLIO", (u32)(S->seed >> 7))) { set_err(r, INST_ERR_FORMAT, ""); return false; }
    if (!fat_mount(&f, dio, 0, r->part_lba)) { set_err(r, INST_ERR_FORMAT, "mount"); return false; }
    for (int i = 0; i < nent; i++) {                     /* pastas primeiro: a mãe vem antes da filha na lista */
        if (!ents[i].dir) continue;
        bool ok; FatNode parent = dir_for(&f, ents[i].path, &ok), nd;
        u16 w[128]; to_w(base_name(ents[i].path), w, 128);
        if (!ok || (fat_lookup(&f, &parent, w, &nd) != FAT_OK && fat_create(&f, &parent, w, true, &nd) != FAT_OK)) { set_err(r, INST_ERR_COPY, ents[i].path); return false; }
    }
    for (int i = 0; i < nent; i++) {
        Ent *e = &ents[i]; if (e->dir) continue;
        bool ok; FatNode parent = dir_for(&f, e->path, &ok), nf;
        u16 w[128]; to_w(base_name(e->path), w, 128);
        if (!ok || fat_create(&f, &parent, w, false, &nf) != FAT_OK) { set_err(r, INST_ERR_COPY, e->path); return false; }
        u32 crc = 0;
        for (u64 off = 0; off < e->size; ) {
            u32 want = e->size - off < BSZ ? (u32)(e->size - off) : BSZ;
            int got = S->read(S->ctx, e->path, off, B, want);
            if (got != (int)want) { set_err(r, INST_ERR_READ, e->path); return false; }
            crc = crc32_buf(crc, B, want);
            if (fat_write(&f, &nf, (u32)off, B, want) != (int)want) { set_err(r, INST_ERR_WRITE, e->path); return false; }
            off += want; done += want;
            if (total) prog(pct_from + 1 + (int)(done * (u64)(pct_to - pct_from - 1) / total), e->path);
        }
        e->crc = crc;
    }
    if (fat_flush(&f)) { set_err(r, INST_ERR_WRITE, "FSInfo"); return false; }
    return true;
}

/* 7: monta de novo, do zero, e relê cada arquivo comparando o CRC */
static bool verify(InstallResult *r, int pct) {
    if (D->flush && !D->flush(D->ctx)) { set_err(r, INST_ERR_WRITE, "flush"); return false; }   /* tudo no disco antes de conferir */
    prog(pct, "verify");
    if (!fat_mount(&f, dio, 0, r->part_lba)) { set_err(r, INST_ERR_VERIFY, "mount"); return false; }
    for (int i = 0; i < nent; i++) {
        Ent *e = &ents[i]; if (e->dir) continue;
        bool ok; FatNode parent = dir_for(&f, e->path, &ok), nf;
        u16 w[128]; to_w(base_name(e->path), w, 128);
        if (!ok || fat_lookup(&f, &parent, w, &nf) != FAT_OK || nf.size != e->size) { set_err(r, INST_ERR_VERIFY, e->path); return false; }
        u32 crc = 0;
        for (u64 off = 0; off < e->size; ) {
            u32 want = e->size - off < BSZ ? (u32)(e->size - off) : BSZ;
            if (fat_read(&f, &nf, (u32)off, B, want) != (int)want) { set_err(r, INST_ERR_VERIFY, e->path); return false; }
            crc = crc32_buf(crc, B, want); off += want;
        }
        if (crc != e->crc) { set_err(r, INST_ERR_VERIFY, e->path); return false; }
    }
    return true;
}
static const char *efi_name(void) { return O->efi_name && O->efi_name[0] ? O->efi_name : "BOOTX64.EFI"; }

/* ------------------------------------------------------------ fase 1: o disco inteiro */
static u64 shrunk_start, shrunk_count;           /* install_shrink_alongside: o NTFS antes de encolher (vai para o FOLIO.ID) */
static void u64_str(u64 v, char *o) { char t[24]; int k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); for (int i = 0; i < k; i++) o[i] = t[k - 1 - i]; o[k] = 0; }
/* FOLIO/INSTALLED.ID na partição do FolioOS: o que foi instalado (para remover e atualizar com segurança depois) */
static bool get_dir(Fat *v, FatNode *parent, const char *name, FatNode *out);
static bool put_whole(Fat *v, FatNode *dir, const char *name, const u8 *data, u32 n);
static int fat_get(Fat *v, const char *path, u8 *buf, u32 cap);
static bool write_marker(InstallResult *r, const char *kind) {
    FatNode root, folio; fat_root(&f, &root);
    char t[200] = "FolioOS installed on this disk\r\nkind=";
    scat(t, kind, sizeof t); scat(t, "\r\nversion=", sizeof t); scat(t, O->version && O->version[0] ? O->version : "?", sizeof t); scat(t, "\r\n", sizeof t);
    if (shrunk_count) {   /* a partição que encolheu, como era (no BIOS não há FOLIO.ID na partição EFI) */
        char num[24]; scat(t, "shrunk=", sizeof t); u64_str(shrunk_start, num); scat(t, num, sizeof t);
        scat(t, ",", sizeof t); u64_str(shrunk_count, num); scat(t, num, sizeof t); scat(t, "\r\n", sizeof t);
    }
    if (!get_dir(&f, &root, "FOLIO", &folio) || !put_whole(&f, &folio, "INSTALLED.ID", (const u8 *)t, (u32)slen(t))) { set_err(r, INST_ERR_WRITE, "FOLIO/INSTALLED.ID"); return false; }
    return true;
}

/* FOLIO/CONFIG.INI da instalação ganha docs_place= (onde ficam os textos); o resto dos ajustes, copiado do pendrive, fica */
static bool write_docs_place(InstallResult *r) {
    static u8 cfg[16384]; static char out[16384 + 64];
    int n = fat_get(&f, "FOLIO/CONFIG.INI", cfg, sizeof cfg - 1);
    if (n < 0) n = 0;
    cfg[n] = 0;
    int k = 0;
    for (int i = 0; i < n; ) {                         /* tira um docs_place= que já exista (copiado do pendrive) */
        int e = i; while (e < n && cfg[e] != '\n') e++;
        if (e < n) e++;
        bool skip_line = e - i >= 11 && !memcmp(cfg + i, "docs_place=", 11);
        if (!skip_line) { memcpy(out + k, cfg + i, (u32)(e - i)); k += e - i; }
        i = e;
    }
    if (k && out[k - 1] != '\n') { out[k++] = '\r'; out[k++] = '\n'; }
    const char *v = O->docs == INST_DOCS_STICK ? "usb" : O->docs == INST_DOCS_BOTH ? "both" : "computer";
    char line[40] = "docs_place="; scat(line, v, sizeof line); scat(line, "\r\n", sizeof line);
    memcpy(out + k, line, (u32)slen(line)); k += slen(line);
    FatNode root, folio; fat_root(&f, &root);
    if (!get_dir(&f, &root, "FOLIO", &folio) || !put_whole(&f, &folio, "CONFIG.INI", (const u8 *)out, (u32)k)) { set_err(r, INST_ERR_WRITE, "FOLIO/CONFIG.INI"); return false; }
    /* a conferência final relê cada arquivo pela lista da origem: o CONFIG.INI agora é este */
    bool listed = false;
    for (int i = 0; i < nent; i++) if (!ents[i].dir && sieq(ents[i].path, "FOLIO/CONFIG.INI")) { ents[i].size = (u64)k; ents[i].crc = crc32_buf(0, out, (u32)k); listed = true; }
    if (!listed && nent < MAXENT) { Ent *e = &ents[nent++]; scopy(e->path, "FOLIO/CONFIG.INI", sizeof e->path); e->dir = false; e->size = (u64)k; e->crc = crc32_buf(0, out, (u32)k); }
    return true;
}

void install_whole_disk(const BlkDev *dev, const InstallSrc *src, const InstallOpts *o, InstallResult *r) {
    if (dev->nsec * 512 < INSTALL_MIN_BYTES) { memset(r, 0, sizeof *r); set_err(r, INST_ERR_SMALL, ""); return; }
    if (!prepare(dev, src, o, r)) return;

    /* 3. tabela nova com uma partição FAT32 do tamanho do disco (FAT32 vai até 2 TB) */
    prog(2, "partition");
    static PartTable t;
    if (!part_read(dev, &t)) memset(&t, 0, sizeof t);
    part_new(&t, o->bios ? SCHEME_MBR : SCHEME_GPT, src->seed);
    memset(t.mbr_code, 0, sizeof t.mbr_code);
    Region rg[4];
    if (part_free(dev, &t, rg, 4) < 1) { set_err(r, INST_ERR_SMALL, ""); return; }
    u64 count = rg[0].count; if (count > 0xFFFFFFFFull) count = 0xFFFFFFFFull & ~2047ull;
    int pi = part_add(dev, &t, rg[0].start, count, GPT_ESP, 0x0C, "FolioOS", src->seed ^ 0x5A5A);
    if (pi < 0) { set_err(r, INST_ERR_WRITE, "partition"); return; }
    t.p[pi].active = true;
    if (!part_write(dev, &t)) { set_err(r, INST_ERR_WRITE, "partition table"); return; }
    r->part_lba = t.p[pi].start; r->part_count = t.p[pi].count;
    memcpy(r->part_guid, t.p[pi].guid, 16); memcpy(r->disk_guid, t.disk_guid, 16);
    /* a própria partição é a partição EFI: o firmware acha \EFI\BOOT\BOOTX64.EFI sozinho; a entrada de boot é um reforço */
    r->esp_num = pi + 1; r->esp_lba = r->part_lba; r->esp_count = r->part_count; memcpy(r->esp_guid, r->part_guid, 16);
    scopy(r->boot_path, "\\EFI\\BOOT\\", sizeof r->boot_path); scat(r->boot_path, efi_name(), sizeof r->boot_path);

    if (!format_and_copy(r, 4, 90) || !write_marker(r, "whole") || !write_docs_place(r)) return;

    /* 6. PC sem UEFI: código de boot do MBR e do setor de boot do volume (como o biosboot.exe) */
    if (o->bios) {
        prog(91, "boot code");
        u8 mbrc[512], vbrc[512], s[512];
        if (src->read(src->ctx, "FOLIO/TOOLS/bios/mbr.bin", 0, mbrc, 440) != 440 || src->read(src->ctx, "FOLIO/TOOLS/bios/vbr.bin", 0, vbrc, 512) != 512) { set_err(r, INST_ERR_BOOTCODE, "FOLIO/TOOLS/bios"); return; }
        if (!dev->rw(dev->ctx, 0, 1, s, false)) { set_err(r, INST_ERR_READ, "MBR"); return; }
        memcpy(s, mbrc, 440);
        if (!dev->rw(dev->ctx, 0, 1, s, true)) { set_err(r, INST_ERR_WRITE, "MBR"); return; }
        u64 vb[2] = { r->part_lba, r->part_lba + 6 };
        for (int k = 0; k < 2; k++) {
            if (!dev->rw(dev->ctx, vb[k], 1, s, false)) { set_err(r, INST_ERR_READ, "boot sector"); return; }
            memcpy(s, vbrc, 3); memcpy(s + 90, vbrc + 90, 420);
            if (!dev->rw(dev->ctx, vb[k], 1, s, true)) { set_err(r, INST_ERR_WRITE, "boot sector"); return; }
        }
    }
    if (!verify(r, 92)) return;
    prog(100, "done");
    r->code = INST_OK;
}

/* ------------------------------------------------------------ fase 2: ao lado de outro sistema (UEFI/GPT) */
/* grava um arquivo inteiro numa pasta do volume f (substitui se existir) e confere pelo CRC */
static bool put_whole(Fat *v, FatNode *dir, const char *name, const u8 *data, u32 n) {
    u16 w[64]; to_w(name, w, 64);
    FatNode nf;
    if (fat_lookup(v, dir, w, &nf) == FAT_OK && fat_delete(v, &nf) != FAT_OK) return false;
    if (fat_create(v, dir, w, false, &nf) != FAT_OK) return false;
    if (n && fat_write(v, &nf, 0, data, n) != (int)n) return false;
    return fat_flush(v) == FAT_OK;
}
static bool get_dir(Fat *v, FatNode *parent, const char *name, FatNode *out) {
    u16 w[64]; to_w(name, w, 64);
    return fat_lookup(v, parent, w, out) == FAT_OK || fat_create(v, parent, w, true, out) == FAT_OK;
}
bool install_parse_id(const char *text, int len, uint8_t guid[16]) {
    for (int i = 0; i + 5 < len; i++) {
        if (text[i] != 'd' || text[i + 1] != 'a' || text[i + 2] != 't' || text[i + 3] != 'a' || text[i + 4] != '=') continue;
        char g[40]; int k = 0;
        for (int j = i + 5; j < len && k < 39 && text[j] != '\r' && text[j] != '\n'; j++) g[k++] = text[j];
        g[k] = 0;
        return part_guid_parse(g, guid);
    }
    return false;
}

/* a partição EFI recebe \EFI\FolioOS: não se grava num FAT que o Windows marcou para verificar */
static bool esp_clean(const BlkDev *dev, const Part *esp, InstallResult *r) {
    static FsCheck c;
    fs_check(dev, esp, &c);
    if (c.verdict != SAFE_OK) { set_err(r, INST_ERR_NOESP, fs_safety_short(c.verdict)); return false; }
    return true;
}
static bool parse_u64(const char *s, int len, int *i, u64 *v) {
    int k = *i; u64 x = 0; bool any = false;
    while (k < len && s[k] >= '0' && s[k] <= '9') { x = x * 10 + (u64)(s[k] - '0'); k++; any = true; }
    *i = k; *v = x; return any;
}
static void alongside(const BlkDev *dev, const InstallSrc *src, Region fr, InstallResult *r) {
    static PartTable t;
    prog(2, "partition");
    if (!part_read(dev, &t) || t.scheme != SCHEME_GPT) { set_err(r, INST_ERR_NOTGPT, ""); return; }
    /* a partição EFI do disco (a primeira, como o firmware faz) tem de ser FAT32 */
    int esp = -1;
    for (int i = 0; i < t.n && esp < 0; i++) if (!memcmp(t.p[i].type, GPT_ESP, 16)) esp = i;
    if (esp < 0 || t.p[esp].fs != FS_FAT32) { set_err(r, INST_ERR_NOESP, ""); return; }
    if (!esp_clean(dev, &t.p[esp], r)) return;
    /* o espaço pedido tem de estar livre, alinhado e dentro do disco utilizável */
    u64 al = 2048, start = (fr.start + al - 1) / al * al, count = fr.count > start - fr.start ? (fr.count - (start - fr.start)) / al * al : 0;
    if (count > 0xFFFFFFFFull) count = 0xFFFFFFFFull & ~(al - 1);
    if (count * 512 < ALONGSIDE_MIN_BYTES) { set_err(r, INST_ERR_SMALL, ""); return; }
    if (part_overlaps(dev, &t, start, count, -1)) { set_err(r, INST_ERR_REGION, ""); return; }
    /* o programa cabe na partição EFI? (com folga de 1 MB) */
    static Fat ef;
    if (!fat_mount(&ef, dio, 0, t.p[esp].start)) { set_err(r, INST_ERR_NOESP, "mount"); return; }
    char pth[64] = "EFI/BOOT/"; scat(pth, efi_name(), sizeof pth);
    u64 prog_size = 0; for (int i = 0; i < nent; i++) if (sieq(ents[i].path, pth)) prog_size = ents[i].size;
    if (!prog_size) { set_err(r, INST_ERR_SOURCE, pth); return; }
    if ((u64)fat_free_clusters(&ef) * ef.bpc < prog_size + (1u << 20)) { set_err(r, INST_ERR_ESPFULL, ""); return; }

    /* 3. só uma entrada nova na tabela: "dados básicos" (o Windows e o Linux enxergam o FAT32 do FolioOS) */
    int pi = part_add(dev, &t, start, count, GPT_MSDATA, 0x0C, "FolioOS", src->seed ^ 0xA5A5);
    if (pi < 0) { set_err(r, INST_ERR_REGION, "partition"); return; }
    if (!part_write(dev, &t)) { set_err(r, INST_ERR_WRITE, "partition table"); return; }
    r->part_lba = t.p[pi].start; r->part_count = t.p[pi].count;
    memcpy(r->part_guid, t.p[pi].guid, 16); memcpy(r->disk_guid, t.disk_guid, 16);
    for (int i = 0; i < t.n; i++) if (!memcmp(t.p[i].type, GPT_ESP, 16)) { esp = i; break; }   /* part_add reordena a tabela */
    r->esp_num = esp + 1; r->esp_lba = t.p[esp].start; r->esp_count = t.p[esp].count; memcpy(r->esp_guid, t.p[esp].guid, 16);
    scopy(r->boot_path, "\\EFI\\FolioOS\\", sizeof r->boot_path); scat(r->boot_path, efi_name(), sizeof r->boot_path);

    if (!format_and_copy(r, 4, 88) || !write_marker(r, "alongside") || !write_docs_place(r)) return;

    /* 6. partição EFI: só \EFI\FolioOS\<programa> e FOLIO.ID (qual partição tem os dados) */
    prog(89, "EFI system partition");
    if (prog_size > BSZ) { set_err(r, INST_ERR_SOURCE, "buffer"); return; }
    if (src->read(src->ctx, pth, 0, B, (u32)prog_size) != (int)prog_size) { set_err(r, INST_ERR_READ, pth); return; }
    u32 pcrc = crc32_buf(0, B, (u32)prog_size);
    if (!fat_mount(&ef, dio, 0, r->esp_lba)) { set_err(r, INST_ERR_NOESP, "mount"); return; }
    FatNode root, d_efi, d_folio; fat_root(&ef, &root);
    if (!get_dir(&ef, &root, "EFI", &d_efi) || !get_dir(&ef, &d_efi, "FolioOS", &d_folio)) { set_err(r, INST_ERR_WRITE, "EFI/FolioOS"); return; }
    if (!put_whole(&ef, &d_folio, efi_name(), B, (u32)prog_size)) { set_err(r, INST_ERR_WRITE, r->boot_path); return; }
    char id[192] = "FolioOS data partition\r\ndata=", gs[37];
    part_guid_str(r->part_guid, gs); scat(id, gs, sizeof id); scat(id, "\r\n", sizeof id);
    if (shrunk_count) {   /* encolhendo: o NTFS como era, para a desinstalação devolver exatamente o mesmo tamanho */
        char num[48]; scat(id, "shrunk=", sizeof id); u64_str(shrunk_start, num); scat(id, num, sizeof id);
        scat(id, ",", sizeof id); u64_str(shrunk_count, num); scat(id, num, sizeof id); scat(id, "\r\n", sizeof id);
    }
    if (!put_whole(&ef, &d_folio, "FOLIO.ID", (const u8 *)id, (u32)slen(id))) { set_err(r, INST_ERR_WRITE, "FOLIO.ID"); return; }

    if (!verify(r, 92)) return;
    /* a cópia na partição EFI, relida do zero */
    if (!fat_mount(&ef, dio, 0, r->esp_lba)) { set_err(r, INST_ERR_VERIFY, "EFI"); return; }
    { bool ok; char p2[64] = "EFI/FolioOS/"; scat(p2, efi_name(), sizeof p2);
      FatNode parent = dir_for(&ef, p2, &ok), nf; u16 w[64]; to_w(efi_name(), w, 64);
      if (!ok || fat_lookup(&ef, &parent, w, &nf) != FAT_OK || nf.size != prog_size || fat_read(&ef, &nf, 0, B, (u32)prog_size) != (int)prog_size || crc32_buf(0, B, (u32)prog_size) != pcrc) { set_err(r, INST_ERR_VERIFY, r->boot_path); return; } }
    prog(100, "done");
    r->code = INST_OK;
}
/* Fase 3, PC só BIOS (disco MBR): o FolioOS ganha uma partição FAT32 primária no espaço livre (a do outro sistema
 * continua ativa), o setor de boot dela é o do FolioOS, e o código do MBR passa a ser o do FolioOS com o byte 437
 * apontando a entrada dele. O código original do MBR vai para FOLIO/OTHERMBR.BIN (440 bytes + o nome do sistema):
 * o seletor o inicia pela ponte BIOS, e a desinstalação o devolve. O MBR é a última coisa gravada. */
static void alongside_mbr(const BlkDev *dev, const InstallSrc *src, Region fr, InstallResult *r) {
    static PartTable t; static u8 mbrc[512], vbrc[512], s[512], orig[512];
    prog(2, "partition");
    if (!part_read(dev, &t) || t.scheme != SCHEME_MBR) { set_err(r, INST_ERR_REGION, "MBR"); return; }
    if (src->read(src->ctx, "FOLIO/TOOLS/bios/mbr.bin", 0, mbrc, 440) != 440 || src->read(src->ctx, "FOLIO/TOOLS/bios/vbr.bin", 0, vbrc, 512) != 512) { set_err(r, INST_ERR_BOOTCODE, "FOLIO/TOOLS/bios"); return; }
    if (!dev->rw(dev->ctx, 0, 1, orig, false)) { set_err(r, INST_ERR_READ, "MBR"); return; }
    if (!memcmp(orig, mbrc, 400)) { set_err(r, INST_ERR_REGION, "FolioOS MBR already"); return; }   /* reinstalar: desinstale antes */
    u64 al = 2048, start = (fr.start + al - 1) / al * al, count = fr.count > start - fr.start ? (fr.count - (start - fr.start)) / al * al : 0;
    if (count > 0xFFFFFFFFull) count = 0xFFFFFFFFull & ~(al - 1);
    if (count * 512 < ALONGSIDE_MIN_BYTES) { set_err(r, INST_ERR_SMALL, ""); return; }
    if (part_overlaps(dev, &t, start, count, -1)) { set_err(r, INST_ERR_REGION, ""); return; }
    /* o nome do outro sistema, pelo que está na partição ativa (o seletor mostra; sem nome, "o outro sistema") */
    const char *name = "";
    for (int i = 0; i < t.n; i++) if (t.p[i].active) name = t.p[i].fs == FS_NTFS ? "Windows" : t.p[i].fs == FS_EXT ? "Linux" : "";
    int pi = part_add(dev, &t, start, count, GPT_MSDATA, 0x0C, "FolioOS", src->seed ^ 0xB105);
    if (pi < 0) { set_err(r, INST_ERR_REGION, "4 primary partitions"); return; }
    if (!part_write(dev, &t)) { set_err(r, INST_ERR_WRITE, "partition table"); return; }
    if (!part_read(dev, &t)) { set_err(r, INST_ERR_READ, "partition table"); return; }
    int slot = -1;
    for (int i = 0; i < t.n; i++) if (t.p[i].start == start) slot = t.p[i].slot;
    if (slot < 0 || slot > 3) { set_err(r, INST_ERR_REGION, "slot"); return; }
    r->part_lba = start; r->part_count = count;
    if (!format_and_copy(r, 4, 86) || !write_marker(r, "alongside") || !write_docs_place(r)) return;
    /* o MBR original, guardado na partição do FolioOS (antes de mudar qualquer coisa no MBR) */
    {
        static u8 om[440 + 64]; int n = 440;
        memcpy(om, orig, 440); for (int i = 0; name[i] && n < (int)sizeof om; i++) om[n++] = (u8)name[i];
        FatNode root, folio; fat_root(&f, &root);
        if (!get_dir(&f, &root, "FOLIO", &folio) || !put_whole(&f, &folio, "OTHERMBR.BIN", om, (u32)n)) { set_err(r, INST_ERR_WRITE, "FOLIO/OTHERMBR.BIN"); return; }
    }
    /* o setor de boot da partição nova (e a cópia no setor 6): o salto e o código do FolioOS, o BPB fica */
    prog(88, "boot code");
    u64 vb[2] = { start, start + 6 };
    for (int k = 0; k < 2; k++) {
        if (!dev->rw(dev->ctx, vb[k], 1, s, false)) { set_err(r, INST_ERR_READ, "boot sector"); return; }
        memcpy(s, vbrc, 3); memcpy(s + 90, vbrc + 90, 420);
        if (!dev->rw(dev->ctx, vb[k], 1, s, true)) { set_err(r, INST_ERR_WRITE, "boot sector"); return; }
    }
    if (!verify(r, 90)) return;
    /* por último, o MBR: o código do FolioOS, com a entrada dele no byte 437 (a tabela e a assinatura ficam) */
    prog(98, "MBR");
    if (!dev->rw(dev->ctx, 0, 1, s, false)) { set_err(r, INST_ERR_READ, "MBR"); return; }
    memcpy(s, mbrc, 440); s[437] = (u8)slot;
    if (!dev->rw(dev->ctx, 0, 1, s, true)) { set_err(r, INST_ERR_WRITE, "MBR"); return; }
    if (dev->flush) dev->flush(dev->ctx);
    memcpy(r->part_guid, t.disk_guid, 16);
    prog(100, "done");
    r->code = INST_OK;
}

void install_alongside(const BlkDev *dev, const InstallSrc *src, const InstallOpts *o, Region fr, InstallResult *r) {
    shrunk_start = shrunk_count = 0;
    if (!prepare(dev, src, o, r)) return;
    if (o->bios) alongside_mbr(dev, src, fr, r); else alongside(dev, src, fr, r);
}

/* Fase 2, sem espaço livre: encolhe o NTFS que começa em part_start para new_count setores (o fim fica
 * alinhado a 1 MiB) e instala no espaço que sobrou logo depois dele. Ordem: cópia de segurança do início
 * do disco → volume NTFS encolhido (ele continua válido dentro da partição antiga, se a energia cair aqui) →
 * entrada da tabela encolhida → a instalação de sempre ao lado. */
void install_shrink_alongside(const BlkDev *dev, const InstallSrc *src, const InstallOpts *o, uint64_t part_start, uint64_t new_count, InstallResult *r) {
    static PartTable t; static FsCheck c;
    if (!prepare(dev, src, o, r)) return;
    prog(2, "shrink");
    bool mbr = o->bios;                                          /* PC só BIOS: disco MBR (fase 3) */
    if (!part_read(dev, &t) || t.scheme != (mbr ? SCHEME_MBR : SCHEME_GPT)) { set_err(r, mbr ? INST_ERR_REGION : INST_ERR_NOTGPT, mbr ? "MBR" : ""); return; }
    int k = -1;
    for (int i = 0; i < t.n; i++) if (t.p[i].start == part_start) k = i;
    if (k < 0 || !fs_resizable(t.p[k].fs) || (!mbr && !memcmp(t.p[k].type, GPT_ESP, 16))) { set_err(r, INST_ERR_SHRINK, "partition"); return; }
    if (!mbr) {   /* a partição EFI tem de estar em ordem antes de mexer na outra (senão encolheria à toa) */
        int esp = -1;
        for (int i = 0; i < t.n && esp < 0; i++) if (!memcmp(t.p[i].type, GPT_ESP, 16)) esp = i;
        if (esp < 0 || t.p[esp].fs != FS_FAT32) { set_err(r, INST_ERR_NOESP, ""); return; }
        if (!esp_clean(dev, &t.p[esp], r)) return;
    }
    u64 end = (part_start + new_count) / 2048 * 2048;            /* o espaço novo começa alinhado */
    if (end <= part_start || end >= part_start + t.p[k].count) { set_err(r, INST_ERR_SHRINK, "size"); return; }
    fs_check(dev, &t.p[k], &c);
    if (c.verdict != SAFE_OK) { set_err(r, INST_ERR_SHRINK, fs_safety_short(c.verdict)); return; }
    if ((end - part_start) * dev->bs < c.min_bytes) { set_err(r, INST_ERR_SHRINK, "size"); return; }
    int rc = fs_resize(dev, t.p[k].fs, part_start, t.p[k].count, end - part_start);   /* NTFS, FAT32 ou exFAT */
    if (rc != NTFS_SHRINK_OK) {
        static const char *why[] = { "", "unreadable", "not clean", "size", "too small", "bad clusters", "write" };
        set_err(r, INST_ERR_SHRINK, rc > 0 && rc < 7 ? why[rc] : "?"); return;
    }
    shrunk_start = part_start; shrunk_count = t.p[k].count;
    t.p[k].count = end - part_start;
    if (!part_write(dev, &t)) { set_err(r, INST_ERR_WRITE, "partition table"); return; }
    if (dev->flush) dev->flush(dev->ctx);
    /* o espaço livre que começa onde a outra partição agora termina */
    Region fr[16]; int nf = part_free(dev, &t, fr, 16), pick = -1;
    for (int i = 0; i < nf; i++) if (fr[i].start == end) pick = i;
    if (pick < 0) { set_err(r, INST_ERR_REGION, ""); return; }
    if (mbr) alongside_mbr(dev, src, fr[pick], r); else alongside(dev, src, fr[pick], r);
    shrunk_start = shrunk_count = 0;
}

/* ------------------------------------------------------------ desinstalar (ao lado de outro sistema) */
static void from_w(const u16 *w, char *o, int cap) {
    int k = 0;
    for (; *w && k < cap - 4; w++) {
        u32 c = *w;
        if (c < 0x80) o[k++] = (char)c;
        else if (c < 0x800) { o[k++] = (char)(0xC0 | c >> 6); o[k++] = (char)(0x80 | (c & 63)); }
        else { o[k++] = (char)(0xE0 | c >> 12); o[k++] = (char)(0x80 | ((c >> 6) & 63)); o[k++] = (char)(0x80 | (c & 63)); }
    }
    o[k] = 0;
}
static Fat fd;                                    /* a partição de dados do FolioOS instalado */
/* lê um arquivo inteiro de um volume FAT32 pelo caminho ("EFI/FolioOS/FOLIO.ID"); -1 = não existe ou não coube */
static int fat_get(Fat *v, const char *path, u8 *buf, u32 cap) {
    bool ok; FatNode dir = dir_for(v, path, &ok), nf; u16 w[128]; to_w(base_name(path), w, 128);
    if (!ok || fat_lookup(v, &dir, w, &nf) != FAT_OK || nf.size > cap) return -1;
    return fat_read(v, &nf, 0, buf, nf.size);
}
/* percorre uma pasta da partição de dados (recursivo): cada arquivo vai para cb com o caminho relativo */
typedef bool (*FileCb)(const char *rel, FatNode *n, void *u);
static bool tree(FatNode *dir, const char *rel, int depth, FileCb cb, void *u) {
    if (depth > 16) return false;
    u32 cookie = 0; u16 nm[256]; FatNode n;
    while (fat_readdir(&fd, dir, &cookie, nm, &n) == 1) {
        char name[200], p[400];
        from_w(nm, name, sizeof name);
        if (seq(name, ".") || seq(name, "..")) continue;
        scopy(p, rel, sizeof p); if (p[0]) scat(p, "/", sizeof p); scat(p, name, sizeof p);
        if (n.attr & 0x10) { if (!tree(&n, p, depth + 1, cb, u)) return false; }
        else if (!cb(p, &n, u)) return false;
    }
    return true;
}
static bool count_cb(const char *rel, FatNode *n, void *u) { InstallFound *f = u; (void)rel; f->docs_files++; f->docs_bytes += n->size; return true; }

/* procura um texto dentro de outro (os arquivos .ID são pequenos e em ASCII) */
static const char *sfind(const char *h, const char *n) {
    for (; *h; h++) { int k = 0; while (n[k] && h[k] == n[k]) k++; if (!n[k]) return h; }
    return 0;
}
static bool dir_exists(Fat *v, const char *path) {
    char p[96]; scopy(p, path, sizeof p); scat(p, "/x", sizeof p);   /* dir_for acha a pasta-mãe do último nome */
    bool ok; (void)dir_for(v, p, &ok); return ok;
}
/* o FolioOS como único sistema: uma partição só, FAT32, com FOLIO/ e o programa (EFI/BOOT ou a ponte BIOS) */
static bool find_whole(const PartTable *t, InstallFound *f) {
    static Fat v; static u8 m[512];
    if (t->n != 1 || t->p[0].fs != FS_FAT32 || !fat_mount(&v, dio, 0, t->p[0].start)) return false;
    int n = fat_get(&v, "FOLIO/INSTALLED.ID", m, sizeof m - 1);
    if (n > 0) { m[n] = 0; if (!sfind((const char *)m, "kind=whole")) return false; }
    else if (!sieq(t->p[0].label, "FOLIO") || !dir_exists(&v, "FOLIO") || !(dir_exists(&v, "EFI/BOOT") || dir_exists(&v, "FOLIO/TOOLS/bios")))
        return false;   /* instalação anterior ao INSTALLED.ID: reconhecida pelo rótulo, pela pasta FOLIO e pelo programa */
    f->kind = FOUND_WHOLE; f->found = true;
    f->part_start = t->p[0].start; f->part_count = t->p[0].count; memcpy(f->part_guid, t->p[0].guid, 16);
    return true;
}
/* ao lado de outro sistema: a partição EFI tem \EFI\FolioOS\FOLIO.ID, que diz qual é a partição de dados */
/* a partição logo antes da do FolioOS (sem outra no meio), NTFS, FAT32 ou exFAT, que pode receber o espaço de volta;
 * id: o texto do FOLIO.ID (UEFI) ou do FOLIO/INSTALLED.ID (BIOS), com "shrunk=" (ou o antigo "ntfs=") */
static void find_neighbor(const BlkDev *dev, const PartTable *t, InstallFound *f, const u8 *id, int n) {
    /* o NTFS logo antes (sem outra partição no meio) pode receber o espaço de volta */
    int q = -1;
    for (int i = 0; i < t->n; i++)
        if (t->p[i].start + t->p[i].count <= f->part_start && (q < 0 || t->p[i].start > t->p[q].start)) q = i;
    if (q >= 0 && fs_resizable(t->p[q].fs) && memcmp(t->p[q].type, GPT_ESP, 16)) {   /* NTFS, FAT32 ou exFAT */
        /* até onde começa a próxima partição (ou o fim utilizável do disco): a original ia até lá, e a partição
         * do FolioOS, alinhada a 1 MiB, pode ter deixado um resto livre depois dela */
        u64 end = t->scheme == SCHEME_GPT ? dev->nsec - 33 : dev->nsec;   /* GPT: antes da cópia de reserva */
        for (int i = 0; i < t->n; i++) if (t->p[i].start >= f->part_start + f->part_count && t->p[i].start < end) end = t->p[i].start;
        u64 want = end - t->p[q].start, max = fs_grow_max(dev, t->p[q].fs, t->p[q].start);
        for (int i = 0; i + 5 < n; i++) {                       /* "shrunk=<início>,<tamanho>" (antes: "ntfs="): a outra antes de encolher */
            int kl = !memcmp(id + i, "shrunk=", 7) ? 7 : !memcmp(id + i, "ntfs=", 5) ? 5 : 0;
            if (!kl) continue;
            int j = i + kl; u64 s0, c0;
            if (parse_u64((const char *)id, n, &j, &s0) && j < n && id[j] == ',' && (j++, parse_u64((const char *)id, n, &j, &c0))
                && s0 == t->p[q].start && c0 > t->p[q].count && c0 <= want) want = c0;
            break;
        }
        if (max > want) max = want;
        if (max > t->p[q].count) { f->ntfs = q; f->grow_fs = t->p[q].fs; f->ntfs_start = t->p[q].start; f->ntfs_count = t->p[q].count; f->ntfs_grow_to = max; }
    }
}
static bool find_alongside(const BlkDev *dev, const PartTable *t, InstallFound *f) {
    static Fat ef; static u8 id[512];
    if (t->scheme != SCHEME_GPT) return false;
    int esp = -1;
    for (int i = 0; i < t->n && esp < 0; i++) if (!memcmp(t->p[i].type, GPT_ESP, 16) && t->p[i].fs == FS_FAT32) esp = i;
    if (esp < 0 || !fat_mount(&ef, dio, 0, t->p[esp].start)) return false;
    int n = fat_get(&ef, "EFI/FolioOS/FOLIO.ID", id, sizeof id - 1);
    u8 g[16];
    if (n <= 0 || !install_parse_id((const char *)id, n, g)) return false;
    int k = -1;
    for (int i = 0; i < t->n; i++) if (!memcmp(t->p[i].guid, g, 16)) k = i;
    if (k < 0) return false;
    f->found = true; f->kind = FOUND_ALONGSIDE; f->esp_start = t->p[esp].start;
    f->part_start = t->p[k].start; f->part_count = t->p[k].count; memcpy(f->part_guid, g, 16);
    find_neighbor(dev, t, f, id, n);
    return true;
}

/* ao lado de outro sistema num PC só BIOS (fase 3): uma primária FAT32 com FOLIO/OTHERMBR.BIN (o MBR original) */
static bool find_alongside_mbr(const BlkDev *dev, const PartTable *t, InstallFound *f) {
    static Fat v; static u8 om[600], id[512];
    if (t->scheme != SCHEME_MBR) return false;
    for (int i = 0; i < t->n; i++) {
        if (t->p[i].fs != FS_FAT32 || t->p[i].logical || !fat_mount(&v, dio, 0, t->p[i].start)) continue;
        if (fat_get(&v, "FOLIO/OTHERMBR.BIN", om, sizeof om) < 440) continue;
        f->found = true; f->kind = FOUND_ALONGSIDE; f->bios = true; f->esp_start = 0;
        f->part_start = t->p[i].start; f->part_count = t->p[i].count;
        int n = fat_get(&v, "FOLIO/INSTALLED.ID", id, sizeof id - 1);
        find_neighbor(dev, t, f, id, n > 0 ? n : 0);
        return true;
    }
    return false;
}

bool install_find(const BlkDev *dev, InstallFound *f) {
    static PartTable t; static u8 id[512];
    memset(f, 0, sizeof *f); f->ntfs = -1;
    D = dev;
    if (dev->bs != 512 || !part_read(dev, &t)) return false;
    if (!find_alongside(dev, &t, f) && !find_alongside_mbr(dev, &t, f) && !find_whole(&t, f)) return false;
    /* a versão instalada (FOLIO/INSTALLED.ID) e os textos que estão lá (para a prévia e a cópia) */
    if (fat_mount(&fd, dio, 0, f->part_start)) {
        int m = fat_get(&fd, "FOLIO/INSTALLED.ID", id, sizeof id - 1);
        if (m > 0) {
            id[m] = 0; f->marked = true;
            const char *v = sfind((const char *)id, "version=");
            if (v) { int k = 0; v += 8; while (*v && *v != '\r' && *v != '\n' && k < 15) f->version[k++] = *v++; f->version[k] = 0; }
        }
        FatNode root, docs; fat_root(&fd, &root); u16 w[16]; to_w("DOCUMENTS", w, 16);
        if (fat_lookup(&fd, &root, w, &docs) == FAT_OK) tree(&docs, "", 0, count_cb, f);
    }
    return true;
}

/* copia um arquivo da partição de dados para o pendrive (inteiro: textos são pequenos) e confere relendo */
typedef struct { const char *dest; InstallResult *r; int done, total; } CopyCtx;
static bool copy_cb(const char *rel, FatNode *n, void *u) {
    CopyCtx *c = u;
    char p[400]; scopy(p, c->dest, sizeof p); scat(p, "/", sizeof p); scat(p, rel, sizeof p);
    if (n->size > BSZ / 2) { set_err(c->r, INST_ERR_COPY, rel); return false; }         /* não cabe: não apaga nada */
    if (n->size && fat_read(&fd, n, 0, B, n->size) != (int)n->size) { set_err(c->r, INST_ERR_READ, rel); return false; }
    u32 crc = crc32_buf(0, B, n->size);
    if (!S->save || !S->save(S->ctx, p, B, n->size)) { set_err(c->r, INST_ERR_COPY, p); return false; }
    u8 *back = B + BSZ / 2;
    if (S->read(S->ctx, p, 0, back, n->size) != (int)n->size || crc32_buf(0, back, n->size) != crc) { set_err(c->r, INST_ERR_VERIFY, p); return false; }
    c->done++;
    if (c->total) prog(4 + 50 * c->done / c->total, rel);
    return true;
}

/* ------------------------------------------------------------ atualizar por um pendrive mais novo */
/* a pasta-mãe de path num volume, criando as que faltam */
static bool parent_make(Fat *v, const char *path, FatNode *out) {
    FatNode cur; fat_root(v, &cur);
    char part[128]; const char *s = path;
    for (;;) {
        int k = 0; while (*s && *s != '/' && k < 127) part[k++] = *s++; part[k] = 0;
        if (!*s) { *out = cur; return true; }
        s++;
        FatNode nx; if (!get_dir(v, &cur, part, &nx)) return false;
        cur = nx;
    }
}
/* grava src_path da origem em dest no volume v sem nunca deixar um arquivo pela metade: primeiro "<nome>.~new",
 * conferido relendo (CRC); só então o antigo sai e o novo ganha o nome. Uma queda no meio deixa o antigo ou o novo. */
static bool copy_safe(Fat *v, const char *dest, const char *src_path, u64 size, InstallResult *r) {
    FatNode dir, nf, old; u16 w[128], wt[136];
    if (!parent_make(v, dest, &dir)) { set_err(r, INST_ERR_WRITE, dest); return false; }
    char tmp[140]; scopy(tmp, base_name(dest), sizeof tmp); scat(tmp, ".~new", sizeof tmp);
    to_w(base_name(dest), w, 128); to_w(tmp, wt, 136);
    if (fat_lookup(v, &dir, wt, &nf) == FAT_OK && fat_delete(v, &nf) != FAT_OK) { set_err(r, INST_ERR_WRITE, dest); return false; }
    if (fat_create(v, &dir, wt, false, &nf) != FAT_OK) { set_err(r, INST_ERR_WRITE, dest); return false; }
    u32 crc = 0;
    for (u64 off = 0; off < size; ) {
        u32 n = size - off < BSZ ? (u32)(size - off) : BSZ;
        if (S->read(S->ctx, src_path, off, B, n) != (int)n) { set_err(r, INST_ERR_READ, src_path); return false; }
        if (fat_write(v, &nf, (u32)off, B, n) != (int)n) { set_err(r, INST_ERR_COPY, dest); return false; }
        crc = crc32_buf(crc, B, n); off += n; done += n;
    }
    if (fat_flush(v) != FAT_OK) { set_err(r, INST_ERR_WRITE, dest); return false; }
    u32 back = 0;
    if (fat_lookup(v, &dir, wt, &nf) != FAT_OK || nf.size != size) { set_err(r, INST_ERR_VERIFY, dest); return false; }
    for (u64 off = 0; off < size; ) {
        u32 n = size - off < BSZ ? (u32)(size - off) : BSZ;
        if (fat_read(v, &nf, (u32)off, B, n) != (int)n) { set_err(r, INST_ERR_VERIFY, dest); return false; }
        back = crc32_buf(back, B, n); off += n;
    }
    if (back != crc) { set_err(r, INST_ERR_VERIFY, dest); return false; }
    if (fat_lookup(v, &dir, w, &old) == FAT_OK && fat_delete(v, &old) != FAT_OK) { set_err(r, INST_ERR_WRITE, dest); return false; }
    if (fat_lookup(v, &dir, wt, &nf) != FAT_OK || fat_rename(v, &nf, w) != FAT_OK || fat_flush(v) != FAT_OK) { set_err(r, INST_ERR_WRITE, dest); return false; }
    return true;
}

void install_update(const BlkDev *dev, const InstallSrc *src, const InstallOpts *o, const InstallFound *fo, InstallResult *r) {
    static Fat ef;
    updating = true;
    bool ok = prepare(dev, src, o, r);                        /* lista a origem (sem o que é da pessoa) e guarda o início do disco */
    updating = false;
    if (!ok) return;
    if (!fo->found) { set_err(r, INST_ERR_REGION, ""); return; }
    if (!fat_mount(&fd, dio, 0, fo->part_start)) { set_err(r, INST_ERR_READ, "FolioOS partition"); return; }
    /* 1. pastas, depois os arquivos (cada um trocado inteiro, ver copy_safe) */
    prog(2, "update");
    for (int i = 0; i < nent; i++) {
        Ent *e = &ents[i]; if (e->dir) continue;
        if (!copy_safe(&fd, e->path, e->path, e->size, r)) return;
        prog(2 + (int)(85 * (total ? done * 100 / total : 100) / 100), e->path);
    }
    /* 2. ao lado de outro sistema: o programa na partição EFI também */
    if (fo->kind == FOUND_ALONGSIDE && !fo->bios) {   /* no BIOS não há partição EFI: o programa está só na partição do FolioOS */
        prog(90, "EFI system partition");
        static PartTable t; int esp = -1;
        if (!part_read(dev, &t)) { set_err(r, INST_ERR_READ, "partition table"); return; }
        for (int i = 0; i < t.n && esp < 0; i++) if (t.p[i].start == fo->esp_start) esp = i;
        if (esp < 0 || !esp_clean(dev, &t.p[esp], r) || !fat_mount(&ef, dio, 0, fo->esp_start)) { if (!r->code) set_err(r, INST_ERR_NOESP, ""); return; }
        char sp[64] = "EFI/BOOT/", dp[64] = "EFI/FolioOS/"; scat(sp, efi_name(), sizeof sp); scat(dp, efi_name(), sizeof dp);
        u64 sz = 0; for (int i = 0; i < nent; i++) if (sieq(ents[i].path, sp)) sz = ents[i].size;
        if (!sz) { set_err(r, INST_ERR_SOURCE, sp); return; }
        if (!copy_safe(&ef, dp, sp, sz, r)) return;
    }
    /* 3. a versão nova no FOLIO/INSTALLED.ID */
    prog(97, "FOLIO/INSTALLED.ID");
    if (!fat_mount(&f, dio, 0, fo->part_start) || !write_marker(r, fo->kind == FOUND_WHOLE ? "whole" : "alongside")) return;
    if (dev->flush) dev->flush(dev->ctx);
    r->part_lba = fo->part_start; r->part_count = fo->part_count;
    prog(100, "done");
    r->code = INST_OK;
}

void install_remove(const BlkDev *dev, const InstallSrc *src, const InstallOpts *o, const InstallFound *f, bool copy_texts, bool grow, InstallResult *r) {
    static PartTable t; static Fat ef;
    if (!prepare(dev, src, o, r)) return;                      /* inclui a cópia de segurança do início do disco */
    if (!f->found) { set_err(r, INST_ERR_REGION, ""); return; }
    /* 1. os textos para o pendrive, antes de apagar qualquer coisa: DOCUMENTS/computer-<data>/ */
    if (copy_texts && f->docs_files) {
        char dest[64] = "DOCUMENTS/computer-"; scat(dest, src->date, sizeof dest);
        CopyCtx c = { dest, r, 0, f->docs_files };
        if (!fat_mount(&fd, dio, 0, f->part_start)) { set_err(r, INST_ERR_READ, "FolioOS partition"); return; }
        FatNode root, docs; fat_root(&fd, &root); u16 w[16]; to_w("DOCUMENTS", w, 16);
        if (fat_lookup(&fd, &root, w, &docs) == FAT_OK && !tree(&docs, "", 0, copy_cb, &c)) return;
        scopy(r->path, dest, sizeof r->path);                  /* onde os textos ficaram (para a tela) */
    }
    if (f->kind == FOUND_WHOLE) {
        /* o FolioOS era o único sistema: o disco fica vazio, sem tabela (pronto para instalar outro sistema). Zera a
         * tabela (MBR/GPT e a GPT de reserva) e o começo do volume, para nada mais reconhecê-lo */
        prog(70, "partition table");
        memset(B, 0, 64 * 512);
        u64 z[3][2] = { { 0, 34 }, { dev->nsec - 33, 33 }, { f->part_start, 32 } };
        for (int k = 0; k < 3; k++) if (!dev->rw(dev->ctx, z[k][0], (u32)z[k][1], B, true)) { set_err(r, INST_ERR_WRITE, "partition table"); return; }
        if (dev->flush) dev->flush(dev->ctx);
        r->part_lba = f->part_start; r->part_count = f->part_count;
        prog(100, "done");
        r->code = INST_OK;
        return;
    }
    if (f->bios) {
        /* 2 (BIOS). o MBR volta a ser o do outro sistema: o código guardado em FOLIO/OTHERMBR.BIN, com a tabela de agora
         * (que a seguir perde a entrada do FolioOS); a partição do outro sistema nunca deixou de ser a ativa */
        static u8 om[600];
        prog(60, "MBR");
        if (!fat_mount(&fd, dio, 0, f->part_start) || fat_get(&fd, "FOLIO/OTHERMBR.BIN", om, sizeof om) < 440) { set_err(r, INST_ERR_READ, "FOLIO/OTHERMBR.BIN"); return; }
        if (!part_read(dev, &t) || t.scheme != SCHEME_MBR) { set_err(r, INST_ERR_REGION, "MBR"); return; }
        memcpy(t.mbr_code, om, 440);
    } else {
    /* 2. a partição EFI perde só \EFI\FolioOS (o programa e o FOLIO.ID) */
    prog(60, "EFI system partition");
    if (!part_read(dev, &t) || t.scheme != SCHEME_GPT) { set_err(r, INST_ERR_NOTGPT, ""); return; }
    int esp = -1;
    for (int i = 0; i < t.n && esp < 0; i++) if (!memcmp(t.p[i].type, GPT_ESP, 16)) esp = i;
    if (esp < 0 || t.p[esp].start != f->esp_start || !fat_mount(&ef, dio, 0, f->esp_start)) { set_err(r, INST_ERR_NOESP, ""); return; }
    if (!esp_clean(dev, &t.p[esp], r)) return;
    {
        FatNode root, efi, fol, n; fat_root(&ef, &root); u16 w[256];
        to_w("EFI", w, 256);
        if (fat_lookup(&ef, &root, w, &efi) == FAT_OK && (to_w("FolioOS", w, 256), fat_lookup(&ef, &efi, w, &fol) == FAT_OK)) {
            u32 cookie;
            for (;;) {                                         /* apaga um por vez e recomeça: a pasta muda */
                cookie = 0; bool any = false;
                while (fat_readdir(&ef, &fol, &cookie, w, &n) == 1) {
                    char nm[200]; from_w(w, nm, sizeof nm);
                    if (seq(nm, ".") || seq(nm, "..")) continue;
                    if (n.attr & 0x10 || fat_delete(&ef, &n) != FAT_OK) { set_err(r, INST_ERR_WRITE, "EFI/FolioOS"); return; }
                    any = true; break;
                }
                if (!any) break;
            }
            if (fat_delete(&ef, &fol) != FAT_OK || fat_flush(&ef) != FAT_OK) { set_err(r, INST_ERR_WRITE, "EFI/FolioOS"); return; }
        }
    }
    }
    /* 3. a tabela: sai a entrada do FolioOS; o NTFS de antes, se for o caso, cresce (a entrada primeiro: um volume
     * menor que a partição é normal, então uma queda de energia entre 3 e 4 não estraga nada) */
    prog(70, "partition");
    int k = -1, q = -1;
    for (int i = 0; i < t.n; i++) {
        if (f->bios ? t.p[i].start == f->part_start : !memcmp(t.p[i].guid, f->part_guid, 16)) k = i;
        if (grow && f->ntfs >= 0 && t.p[i].start == f->ntfs_start && t.p[i].fs == f->grow_fs) q = i;
    }
    if (k < 0 || t.p[k].start != f->part_start) { set_err(r, INST_ERR_REGION, "partition"); return; }
    if (q >= 0) t.p[q].count = f->ntfs_grow_to;
    if (!part_del(&t, k) || !part_write(dev, &t)) { set_err(r, INST_ERR_WRITE, "partition table"); return; }
    if (dev->flush) dev->flush(dev->ctx);
    /* 4. o NTFS volta a ocupar o espaço (o Windows roda o chkdsk uma vez na próxima vez que iniciar) */
    if (q >= 0) {
        prog(80, "NTFS");
        int rc = fs_resize(dev, f->grow_fs, f->ntfs_start, f->ntfs_grow_to, f->ntfs_grow_to);
        if (rc != NTFS_SHRINK_OK) { set_err(r, INST_ERR_GROW, ""); return; }
    }
    r->part_lba = f->part_start; r->part_count = f->part_count;
    prog(100, "done");
    r->code = INST_OK;
}

/* ------------------------------------------------------------ gerenciador de partições (fase 1) */
/* Cada operação: cópia de segurança do começo do disco no pendrive, depois a mudança, na ordem que sobrevive a
 * uma queda de energia. A tela mostra o antes → depois e pede a confirmação antes de chamar. */
static bool op_begin(const BlkDev *dev, const InstallSrc *src, InstallResult *r, PartTable *t, uint64_t part_start, int *k) {
    memset(r, 0, sizeof *r);
    S = src; D = dev; B = src->buf; BSZ = src->bufsize & ~511u;
    if (!B || BSZ < 65536) { set_err(r, INST_ERR_SOURCE, "buffer"); return false; }
    if (dev->bs != 512) { set_err(r, INST_ERR_WRITE, "4K sectors"); return false; }
    if (!part_read(dev, t)) { set_err(r, INST_ERR_READ, "partition table"); return false; }
    if (t->has_extended) { set_err(r, INST_ERR_REGION, "extended partition"); return false; }   /* lógicas: só leitura nesta fase */
    *k = -1;
    for (int i = 0; i < t->n; i++) if (t->p[i].start == part_start) *k = i;
    if (!save_disk_start(dev, src, r)) return false;
    return true;
}
/* quanto um FAT32 criado aqui pode crescer depois sem mover dados: até 4 vezes, sem passar do fim do disco */
static u64 grow_room(const BlkDev *dev, u64 start, u64 count) { u64 m = count * 4, end = dev->nsec - 34 - start; return m < end ? m : end; }
void part_op_create(const BlkDev *dev, const InstallSrc *src, uint64_t start, uint64_t count, const char *label, InstallResult *r) {
    static PartTable t; int k;
    if (!op_begin(dev, src, r, &t, ~0ull, &k)) return;
    if (t.scheme == SCHEME_NONE) part_new(&t, SCHEME_GPT, src->seed);                 /* disco vazio: GPT */
    u64 s = (start + 2047) / 2048 * 2048, c = count > s - start ? (count - (s - start)) / 2048 * 2048 : 0;
    if (c * 512 < (64ull << 20)) { set_err(r, INST_ERR_SMALL, ""); return; }          /* FAT32: 64 MB no mínimo */
    if (c > 0xFFFFFFFFull) c = 0xFFFFFFFFull & ~2047ull;
    {   /* dentro do espaço livre de verdade da tabela que vai ser gravada (uma GPT nova reserva o fim do disco) */
        Region fr[32]; int nf = part_free(dev, &t, fr, 32); bool in = false;
        for (int i = 0; i < nf && !in; i++)
            if (s >= fr[i].start && s < fr[i].start + fr[i].count) { u64 room = (fr[i].start + fr[i].count - s) / 2048 * 2048; if (c > room) c = room; in = true; }
        if (!in || c * 512 < (64ull << 20)) { set_err(r, INST_ERR_REGION, ""); return; }
    }
    if (part_overlaps(dev, &t, s, c, -1)) { set_err(r, INST_ERR_REGION, ""); return; }
    int pi = part_add(dev, &t, s, c, GPT_MSDATA, 0x0C, label && label[0] ? label : "Data", src->seed ^ 0xC0DE);
    if (pi < 0) { set_err(r, INST_ERR_REGION, t.scheme == SCHEME_MBR ? "4 primary partitions" : ""); return; }
    /* o volume primeiro (a tabela ainda não aponta para ele), depois a tabela */
    if (!fat32_format2(dio, 0, s, c, grow_room(dev, s, c), label && label[0] ? label : "DATA", (u32)(src->seed >> 9))) { set_err(r, INST_ERR_FORMAT, ""); return; }
    if (!part_write(dev, &t)) { set_err(r, INST_ERR_WRITE, "partition table"); return; }
    if (dev->flush) dev->flush(dev->ctx);
    r->part_lba = s; r->part_count = c; r->code = INST_OK;
}
void part_op_delete(const BlkDev *dev, const InstallSrc *src, uint64_t part_start, InstallResult *r) {
    static PartTable t; int k;
    if (!op_begin(dev, src, r, &t, part_start, &k)) return;
    if (k < 0) { set_err(r, INST_ERR_REGION, "partition"); return; }
    u64 s = t.p[k].start;
    if (!part_del(&t, k) || !part_write(dev, &t)) { set_err(r, INST_ERR_WRITE, "partition table"); return; }
    /* o começo do volume zerado: nada mais o reconhece (o espaço fica realmente livre) */
    memset(B, 0, 32 * 512);
    if (!dev->rw(dev->ctx, s, 32, B, true)) { set_err(r, INST_ERR_WRITE, "volume"); return; }
    if (dev->flush) dev->flush(dev->ctx);
    r->part_lba = s; r->code = INST_OK;
}
void part_op_format(const BlkDev *dev, const InstallSrc *src, uint64_t part_start, const char *label, InstallResult *r) {
    static PartTable t; int k;
    if (!op_begin(dev, src, r, &t, part_start, &k)) return;
    if (k < 0) { set_err(r, INST_ERR_REGION, "partition"); return; }
    Part *p = &t.p[k];
    if (p->count * 512 < (64ull << 20) || p->count > 0xFFFFFFFFull) { set_err(r, INST_ERR_SMALL, ""); return; }
    if (!fat32_format2(dio, 0, p->start, p->count, grow_room(dev, p->start, p->count), label && label[0] ? label : "DATA", (u32)(src->seed >> 9))) { set_err(r, INST_ERR_FORMAT, ""); return; }
    /* o tipo passa a dizer FAT32 (a partição EFI continua EFI) */
    bool changed = false;
    if (t.scheme == SCHEME_GPT && memcmp(p->type, GPT_ESP, 16) && memcmp(p->type, GPT_MSDATA, 16)) { memcpy(p->type, GPT_MSDATA, 16); changed = true; }
    if (t.scheme == SCHEME_MBR && p->mbr_type != 0x0C && p->mbr_type != 0x0B && p->mbr_type != 0xEF) { p->mbr_type = 0x0C; changed = true; }
    if (changed && !part_write(dev, &t)) { set_err(r, INST_ERR_WRITE, "partition table"); return; }
    if (dev->flush) dev->flush(dev->ctx);
    r->part_lba = p->start; r->part_count = p->count; r->code = INST_OK;
}
void part_op_resize(const BlkDev *dev, const InstallSrc *src, uint64_t part_start, uint64_t new_count, InstallResult *r) {
    static PartTable t; static FsCheck c; int k;
    if (!op_begin(dev, src, r, &t, part_start, &k)) return;
    if (k < 0 || !fs_resizable(t.p[k].fs)) { set_err(r, INST_ERR_SHRINK, "partition"); return; }
    Part *p = &t.p[k];
    fs_check(dev, p, &c);
    if (c.verdict != SAFE_OK && !(c.verdict == SAFE_DIRTY && p->fs == FS_NTFS && new_count > p->count)) { set_err(r, INST_ERR_SHRINK, fs_safety_short(c.verdict)); return; }
    if (new_count == p->count || !new_count) { set_err(r, INST_ERR_RESIZE, ""); return; }
    if (new_count < p->count) {
        if (new_count * dev->bs < c.min_bytes) { set_err(r, INST_ERR_RESIZE, ""); return; }
        int rc = fs_resize(dev, p->fs, p->start, p->count, new_count);       /* o volume, depois a tabela */
        if (rc != NTFS_SHRINK_OK) { set_err(r, INST_ERR_RESIZE, "volume"); return; }
        p->count = new_count;
        if (!part_write(dev, &t)) { set_err(r, INST_ERR_WRITE, "partition table"); return; }
    } else {
        if (part_overlaps(dev, &t, p->start, new_count, k) || new_count > fs_grow_max(dev, p->fs, p->start)) { set_err(r, INST_ERR_RESIZE, ""); return; }
        p->count = new_count;                                                  /* a tabela, depois o volume */
        if (!part_write(dev, &t)) { set_err(r, INST_ERR_WRITE, "partition table"); return; }
        int rc = fs_resize(dev, p->fs, p->start, new_count, new_count);
        if (rc != NTFS_SHRINK_OK) { set_err(r, INST_ERR_RESIZE, "volume"); return; }
    }
    if (dev->flush) dev->flush(dev->ctx);
    r->part_lba = p->start; r->part_count = new_count; r->code = INST_OK;
}

#ifndef N_
#define N_(s) (s)                 /* marca para a tradução (a interface chama L() no texto devolvido) */
#endif
const char *install_error(int code) {
    switch (code) {
    case INST_OK: return N_("Done");
    case INST_ERR_READ: return N_("Could not read");
    case INST_ERR_WRITE: return N_("Could not write to the disk");
    case INST_ERR_SMALL: return N_("The disk is too small (at least 512 MB)");
    case INST_ERR_BACKUP: return N_("Could not save the safety copy to the USB drive");
    case INST_ERR_FORMAT: return N_("Could not format the new partition");
    case INST_ERR_COPY: return N_("Could not copy");
    case INST_ERR_VERIFY: return N_("A copied file did not match the original");
    case INST_ERR_BOOTCODE: return N_("The BIOS boot code is missing from the USB drive");
    case INST_ERR_SOURCE: return N_("The USB drive does not have a complete FolioOS");
    case INST_ERR_NOTGPT: return N_("This disk does not use a GPT partition table");
    case INST_ERR_NOESP: return N_("No FAT32 EFI system partition was found on this disk");
    case INST_ERR_ESPFULL: return N_("The EFI system partition is full");
    case INST_ERR_REGION: return N_("The chosen space is not free");
    case INST_ERR_SHRINK: return N_("Could not shrink the other system's partition");
    case INST_ERR_GROW: return N_("Could not give the space back to the other system's partition");
    case INST_ERR_RESIZE: return N_("This partition cannot change to that size without moving data");
    default: return N_("Error");
    }
}
