/* FolioOS — verificação das partições de outros sistemas (ver fscheck.h). A verificação só lê; o que grava é
 * ntfs_shrink() e ntfs_grow(), no fim do arquivo.
 * NTFS: o suficiente para responder três perguntas com segurança: o volume está marcado como sujo
 * ($Volume)? o Windows está hibernado ou com Inicialização Rápida (hiberfil.sys começando por "hibr")?
 * até onde há dados ($Bitmap)? Qualquer coisa estranha (registro sem "FILE", correção de setor que não
 * bate, runlist inválida) vira "não deu para ler", e a partição não é tocada. */
#include "fscheck.h"
#include <stddef.h>
void *memcpy(void *, const void *, size_t); void *memset(void *, int, size_t); int memcmp(const void *, const void *, size_t);

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int64_t i64;
static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64(const u8 *p) { return rd32(p) | (u64)rd32(p + 4) << 32; }

/* ------------------------------------------------------------ NTFS */
#define BUF (64 * 1024)
static u8 secbuf[BUF];
/* bytes [off, off+len) do volume; off e len múltiplos do setor */
static bool vol_read(Ntfs *n, u64 off, void *buf, u32 len) {
    if (off % n->bps || len % n->bps || off / n->bps + len / n->bps > n->nclus * (n->cs / n->bps)) return false;
    return n->d->rw(n->d->ctx, n->base + off / n->bps, len / n->bps, buf, false);
}
/* correção das sequências de atualização (a cada 512 bytes os 2 últimos foram trocados pelo número da sequência) */
static bool fixup(u8 *b, u32 len, const char *magic) {
    if (memcmp(b, magic, 4)) return false;
    u32 uo = rd16(b + 4), uc = rd16(b + 6);
    if (uc < 2 || uo + 2 * uc > len || (uc - 1) * 512 > len) return false;
    for (u32 i = 1; i < uc; i++) {
        u8 *end = b + i * 512 - 2;
        if (end[0] != b[uo] || end[1] != b[uo + 1]) return false;
        end[0] = b[uo + 2 * i]; end[1] = b[uo + 2 * i + 1];
    }
    return true;
}
typedef struct { u64 vcn, lcn, len; } Run;
static int runs_decode(const u8 *p, const u8 *end, Run *out, int cap) {
    int n = 0; u64 vcn = 0; i64 lcn = 0;
    while (p < end && *p && n < cap) {
        int ls = *p & 15, os = *p >> 4; p++;
        if (!ls || ls > 8 || os > 8 || p + ls + os > end) return -1;
        u64 len = 0; for (int i = 0; i < ls; i++) len |= (u64)p[i] << (8 * i);
        p += ls;
        if (os) {
            i64 d = 0; for (int i = 0; i < os; i++) d |= (i64)p[i] << (8 * i);
            if (p[os - 1] & 0x80) d -= (i64)1 << (8 * os);        /* deslocamento com sinal */
            lcn += d; p += os;
            out[n].lcn = (u64)lcn;
        } else out[n].lcn = ~0ull;                                 /* esparso */
        out[n].vcn = vcn; out[n].len = len; vcn += len; n++;
    }
    return n;
}
/* atributo do registro: tipo e (opcional) só os sem nome */
static const u8 *attr_find(const u8 *rec, u32 rlen, u32 type, bool unnamed) {
    u32 off = rd16(rec + 0x14), used = rd32(rec + 0x18);
    if (used > rlen) used = rlen;
    while (off + 16 <= used) {
        const u8 *a = rec + off; u32 t = rd32(a), l = rd32(a + 4);
        if (t == 0xFFFFFFFFu || l < 16 || off + l > used) return 0;
        if (t == type && (!unnamed || a[9] == 0)) return a;
        off += l;
    }
    return 0;
}
static bool mft_record(Ntfs *n, u64 no, u8 *buf) {
    u64 off = no * n->rec, vcn = off / n->cs, in = off % n->cs;
    for (int i = 0; i < n->nmft; i++) {
        Run *r = (Run *)&n->mft_runs[i];
        if (vcn < r->vcn || vcn >= r->vcn + r->len || r->lcn == ~0ull) continue;
        u64 at = (r->lcn + (vcn - r->vcn)) * n->cs + in;
        if (!vol_read(n, at, buf, n->rec)) return false;
        return fixup(buf, n->rec, "FILE") && (rd16(buf + 0x16) & 1);   /* em uso */
    }
    return false;
}
/* atributo de um arquivo pelo número do registro, seguindo o $ATTRIBUTE_LIST (0x20) quando o arquivo não
 * cabe num registro só (uma pasta com muitas entradas, por exemplo: o índice vai para um registro de
 * extensão). Só a primeira parte (VCN 0); se o atributo continuar em outro registro, *split = true e quem
 * chama trata como ilegível. buf recebe o registro onde o atributo está. 0 = não tem; (u8*)1 = erro. */
#define ATTR_ERR ((const u8 *)1)
static u64 attr_rec;                       /* o registro onde o último attr_get achou o atributo (para regravar) */
static const u8 *attr_get(Ntfs *n, u64 no, u32 type, bool unnamed, u8 *buf, bool *split) {
    *split = false; attr_rec = no;
    if (!mft_record(n, no, buf)) return ATTR_ERR;
    const u8 *a = attr_find(buf, n->rec, type, unnamed);
    const u8 *al = attr_find(buf, n->rec, 0x20, true);
    if (!al) return a;
    if (al[8]) return ATTR_ERR;                                /* lista não residente: não acontece em volumes pequenos */
    static u8 list[1024];
    u32 ll = rd32(al + 0x10);
    if (ll > sizeof list || rd16(al + 0x14) + ll > rd32(al + 4)) return ATTR_ERR;
    memcpy(list, al + rd16(al + 0x14), ll);
    u64 where = ~0ull; int parts = 0;
    for (u32 o = 0; o + 26 <= ll; ) {
        u32 t = rd32(list + o); u16 el = rd16(list + o + 4);
        if (el < 26 || o + el > ll) return ATTR_ERR;
        if (t == type && (!unnamed || list[o + 6] == 0)) {
            parts++;
            if (rd64(list + o + 8) == 0) where = rd64(list + o + 0x10) & 0xFFFFFFFFFFFFull;
        }
        o += el;
    }
    *split = parts > 1;
    if (a) return a;
    if (where == ~0ull) return 0;
    attr_rec = where;
    if (!mft_record(n, where, buf)) return ATTR_ERR;
    a = attr_find(buf, n->rec, type, unnamed);
    return a ? a : ATTR_ERR;
}

/* lê [off, off+len) de um atributo não residente (off e len múltiplos do setor; esparso = zeros) */
static bool nonres_read(Ntfs *n, const u8 *a, u64 off, u8 *buf, u32 len) {
    static Run rl[256];
    const u8 *end = a + rd32(a + 4);
    int nr = runs_decode(a + rd16(a + 0x20), end, rl, 256);
    if (nr < 0) return false;
    while (len) {
        u64 vcn = off / n->cs, in = off % n->cs; bool hit = false;
        for (int i = 0; i < nr && !hit; i++) {
            if (vcn < rl[i].vcn || vcn >= rl[i].vcn + rl[i].len) continue;
            u64 avail = (rl[i].vcn + rl[i].len - vcn) * n->cs - in;
            u32 k = len < avail ? len : (u32)avail;
            if (rl[i].lcn == ~0ull) memset(buf, 0, k);
            else if (!vol_read(n, (rl[i].lcn + (vcn - rl[i].vcn)) * n->cs + in, buf, k)) return false;
            buf += k; off += k; len -= k; hit = true;
        }
        if (!hit) return false;
    }
    return true;
}

bool ntfs_open(Ntfs *n, const BlkDev *d, u64 part_lba) {
    memset(n, 0, sizeof *n); n->d = d; n->base = part_lba;
    u8 *b = secbuf;
    if (!d->rw(d->ctx, part_lba, 1, b, false) || memcmp(b + 3, "NTFS    ", 8)) return false;
    n->bps = rd16(b + 0x0B);
    u32 spc = b[0x0D]; if (spc > 0x80) spc = 1u << (256 - spc);
    if (n->bps != d->bs || !spc || spc > 4096) return false;
    n->cs = n->bps * spc;
    u64 total = rd64(b + 0x28);
    n->nclus = total / spc; n->mft_lcn = rd64(b + 0x30);
    int8_t cpr = (int8_t)b[0x40];
    n->rec = cpr > 0 ? (u32)cpr * n->cs : 1u << (-cpr);
    if (n->rec < 512 || n->rec > 4096 || n->mft_lcn >= n->nclus || n->cs > BUF) return false;
    /* registro 0 ($MFT): as runs da própria MFT */
    static u8 rec[4096];
    if (!vol_read(n, n->mft_lcn * n->cs, rec, n->rec) || !fixup(rec, n->rec, "FILE")) return false;
    const u8 *a = attr_find(rec, n->rec, 0x80, true);
    if (!a || !a[8]) return false;
    static Run rl[64];
    int nr = runs_decode(a + rd16(a + 0x20), a + rd32(a + 4), rl, 64);
    if (nr <= 0) return false;
    for (int i = 0; i < nr; i++) { n->mft_runs[i].vcn = rl[i].vcn; n->mft_runs[i].lcn = rl[i].lcn; n->mft_runs[i].len = rl[i].len; }
    n->nmft = nr;
    return true;
}

/* procura um nome nas entradas de um nó de índice (chave FILE_NAME): 1 = achou (*rec), 0 = não, -1 = estragado */
static int index_entries(const u8 *p, const u8 *end, const char *want, u64 *rec) {
    while (p + 16 <= end) {
        u16 el = rd16(p + 8), sl = rd16(p + 10), fl = rd16(p + 12);
        if (el < 16 || p + el > end || 16 + sl > el) return -1;
        if (fl & 2) return 0;                                  /* última entrada */
        if (sl >= 0x42) {
            const u8 *fn = p + 16; int nl = fn[0x40]; bool eq = true; int k = 0;
            if (0x42 + 2 * nl > sl) return -1;
            for (; k < nl && want[k] && eq; k++) {
                u16 c = rd16(fn + 0x42 + 2 * k); char w = want[k];
                if (c >= 'A' && c <= 'Z') c += 32;
                if (w >= 'A' && w <= 'Z') w += 32;
                eq = c == (u16)w;
            }
            if (eq && k == nl && !want[k]) { *rec = rd64(p) & 0xFFFFFFFFFFFFull; return 1; }
        }
        p += el;
    }
    return -1;                                                 /* acabou sem a entrada final */
}
/* um arquivo da raiz, por nome: 1 = achou (*rec), 0 = não existe, -1 = não deu para ler o índice.
 * Lê a raiz do índice e todos os blocos INDX em uso (pelo $BITMAP do índice); a ordem da árvore não
 * importa. Na dúvida, -1: "não achei" só vale quando o índice inteiro foi lido. */
static int root_lookup(Ntfs *n, const char *name, u64 *rec) {
    static u8 r1[4096], r2[4096], r3[4096];
    bool split;
    const u8 *ir = attr_get(n, 5, 0x90, false, r1, &split);
    if (!ir || ir == ATTR_ERR || ir[8] || split) return -1;
    const u8 *v = ir + rd16(ir + 0x14); u32 vl = rd32(ir + 0x10);
    if (vl < 0x20 || rd16(ir + 0x14) + vl > rd32(ir + 4)) return -1;
    u32 ibs = rd32(v + 8);
    const u8 *h = v + 0x10;
    if (0x10 + rd32(h) > vl) return -1;
    int r = index_entries(h + rd32(h), v + vl, name, rec);
    if (r) return r;
    if (!(h[0x0C] & 1)) return 0;                              /* índice pequeno: sem blocos INDX */
    const u8 *ia = attr_get(n, 5, 0xA0, false, r2, &split);
    if (!ia || ia == ATTR_ERR || !ia[8] || split || ibs < 512 || ibs > BUF || ibs % n->bps) return -1;
    const u8 *bm = attr_get(n, 5, 0xB0, false, r3, &split);   /* quais blocos estão em uso */
    const u8 *bits = 0; u32 nbits = 0;
    if (bm && bm != ATTR_ERR && !bm[8] && !split) { bits = bm + rd16(bm + 0x14); nbits = rd32(bm + 0x10) * 8; }
    u64 size = rd64(ia + 0x30);
    static u8 blk[BUF];
    for (u64 off = 0, i = 0; off + ibs <= size; off += ibs, i++) {
        if (bits && (i >= nbits || !(bits[i / 8] >> (i % 8) & 1))) continue;   /* livre: entradas velhas */
        if (!nonres_read(n, ia, off, blk, ibs) || !fixup(blk, ibs, "INDX")) return -1;
        const u8 *hh = blk + 0x18;
        u32 eo = rd32(hh), tot = rd32(hh + 4);
        if (0x18 + tot > ibs || eo > tot) return -1;
        r = index_entries(hh + eo, hh + tot, name, rec);
        if (r) return r;
    }
    return 0;
}

int ntfs_flags(Ntfs *n) {
    static u8 rec[4096];
    int fl = 0;
    if (!mft_record(n, 3, rec)) return -1;                     /* $Volume */
    const u8 *vi = attr_find(rec, n->rec, 0x70, true);
    if (!vi || vi[8] || rd32(vi + 0x10) < 12) return -1;
    if (rd16(vi + rd16(vi + 0x14) + 0x0A) & 0x0001) fl |= NTFS_DIRTY;
    u64 h;
    int found = root_lookup(n, "hiberfil.sys", &h);
    if (found < 0) return -1;
    if (found) {
        bool split;
        const u8 *da = attr_get(n, h, 0x80, true, rec, &split);
        if (da == ATTR_ERR) return -1;
        if (da) {
            u8 *b = secbuf; bool got = false;
            if (!da[8]) { u32 l = rd32(da + 0x10); if (l >= 4) { memcpy(b, da + rd16(da + 0x14), 4); got = true; } }
            else if (rd64(da + 0x30) >= 4) { if (!nonres_read(n, da, 0, b, n->bps)) return -1; got = true; }
            if (got) {
                char m[4]; for (int i = 0; i < 4; i++) m[i] = (char)(b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i]);
                if (!memcmp(m, "hibr", 4)) fl |= NTFS_HIBERNATED;
            }
        }
    }
    return fl;
}

bool ntfs_last_used(Ntfs *n, u64 *lcn) {
    static u8 rec[4096];
    bool split;
    const u8 *da = attr_get(n, 6, 0x80, true, rec, &split);    /* $Bitmap */
    if (!da || da == ATTR_ERR || split) return false;
    /* só os bits dos clusters que existem: o último byte vem com os bits de sobra ligados (12255 clusters
     * num volume do Windows deram o bit 12255 aceso), e eles não são dados */
    u64 bytes = (n->nclus + 7) / 8, last = 0; bool any = false;
    u32 chunk = BUF / 2 / n->bps * n->bps; static u8 b[BUF];
    #define SCAN(byte, base) for (u32 bit = 0; bit < 8; bit++) { u64 c = (u64)(base) * 8 + bit; \
        if (c < n->nclus && ((byte) >> bit & 1)) { last = c; any = true; } }
    if (!da[8]) {                                             /* residente (volume minúsculo) */
        const u8 *v = da + rd16(da + 0x14); u32 vl = rd32(da + 0x10);
        if (vl < bytes) return false;
        for (u32 i = 0; i < bytes; i++) if (v[i]) SCAN(v[i], i)
    } else {
        if (rd64(da + 0x30) < bytes) return false;
        for (u64 off = 0; off < bytes; off += chunk) {
            u32 k = (u32)((bytes - off + n->bps - 1) / n->bps * n->bps); if (k > chunk) k = chunk;
            if (!nonres_read(n, da, off, b, k)) return false;
            for (u32 i = 0; i < k && off + i < bytes; i++) if (b[i]) SCAN(b[i], off + i)
        }
    }
    #undef SCAN
    if (!any) return false;
    *lcn = last;
    return true;
}

/* $LogFile fechado de forma limpa? (área de reinício com RESTART_VOLUME_IS_CLEAN, ou sem clientes)
 * Se não, o Windows vai repetir o diário na próxima montagem, por cima do que se mudar aqui. */
bool ntfs_log_clean(Ntfs *n) {
    static u8 rec[4096], pg[8192]; bool split;
    const u8 *a = attr_get(n, 2, 0x80, true, rec, &split);
    if (!a || a == ATTR_ERR || !a[8] || rd64(a + 0x30) < 4096) return false;
    if (!nonres_read(n, a, 0, pg, 4096) || memcmp(pg, "RSTR", 4)) return false;
    u32 ps = rd32(pg + 0x10);                                    /* tamanho da página do sistema */
    if (ps < 512 || ps > sizeof pg || ps % n->bps) return false;
    if (ps > 4096 && !nonres_read(n, a, 0, pg, ps)) return false;
    if (!fixup(pg, ps, "RSTR")) return false;
    u32 ro = rd16(pg + 0x18);
    if (ro + 0x10 > ps) return false;
    const u8 *ra = pg + ro;
    return (rd16(ra + 0x0E) & 0x0002) || rd16(ra + 0x0C) == 0xFFFF;
}

/* ------------------------------------------------------------ encolher NTFS (sem mover dados) */
static bool vol_write(Ntfs *n, u64 off, const void *buf, u32 len) {
    if (off % n->bps || len % n->bps) return false;
    return n->d->rw(n->d->ctx, n->base + off / n->bps, len / n->bps, (void *)buf, true);
}
/* o inverso do fixup: novo número de sequência, guarda os 2 últimos bytes de cada setor e grava */
static bool mft_write(Ntfs *n, u64 no, u8 *rec) {
    static u8 out[4096];
    memcpy(out, rec, n->rec);
    u32 uo = rd16(out + 4), uc = rd16(out + 6);
    if (uc < 2 || uo + 2 * uc > n->rec) return false;
    u16 usn = (u16)(rd16(out + uo) + 1); if (usn == 0 || usn == 0xFFFF) usn = 1;
    out[uo] = (u8)usn; out[uo + 1] = (u8)(usn >> 8);
    for (u32 i = 1; i < uc; i++) {
        u8 *end = out + i * 512 - 2;
        out[uo + 2 * i] = end[0]; out[uo + 2 * i + 1] = end[1];
        end[0] = (u8)usn; end[1] = (u8)(usn >> 8);
    }
    u64 off = no * n->rec, vcn = off / n->cs, in = off % n->cs;
    for (int i = 0; i < n->nmft; i++) {
        if (vcn < n->mft_runs[i].vcn || vcn >= n->mft_runs[i].vcn + n->mft_runs[i].len || n->mft_runs[i].lcn == ~0ull) continue;
        return vol_write(n, (n->mft_runs[i].lcn + (vcn - n->mft_runs[i].vcn)) * n->cs + in, out, n->rec);
    }
    return false;
}
static void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> 8 * i); }
/* atributo com nome (ASCII) num registro já lido */
static u8 *attr_named(u8 *rec, u32 rlen, u32 type, const char *name) {
    u32 off = rd16(rec + 0x14), used = rd32(rec + 0x18), nl = 0;
    while (name[nl]) nl++;
    if (used > rlen) used = rlen;
    while (off + 16 <= used) {
        u8 *a = rec + off; u32 t = rd32(a), l = rd32(a + 4);
        if (t == 0xFFFFFFFFu || l < 16 || off + l > used) return 0;
        if (t == type && a[9] == nl && rd16(a + 10) + 2 * nl <= l) {
            bool eq = true;
            for (u32 k = 0; k < nl && eq; k++) eq = rd16(a + rd16(a + 10) + 2 * k) == (u16)name[k];
            if (eq) return a;
        }
        off += l;
    }
    return 0;
}

int ntfs_shrink(const BlkDev *d, u64 part_lba, u64 part_sectors, u64 new_sectors) {
    static Ntfs n; static u8 boot[4096], rec[4096], bm[4096];
    if (d->bs > sizeof boot || !ntfs_open(&n, d, part_lba)) return NTFS_SHRINK_UNREADABLE;
    if (!d->rw(d->ctx, part_lba, 1, boot, false)) return NTFS_SHRINK_UNREADABLE;
    u32 spc = n.cs / n.bps;
    u64 total = rd64(boot + 0x28);
    if (total + 1 > part_sectors || new_sectors >= part_sectors || new_sectors < 2) return NTFS_SHRINK_BAD_SIZE;
    u64 ntotal = new_sectors - 1, nclus = ntotal / spc;          /* a cópia do setor de boot fica no último setor */
    /* 1. só um volume em ordem: nem sujo, nem hibernado, diário fechado, sem clusters defeituosos */
    int fl = ntfs_flags(&n);
    if (fl < 0) return NTFS_SHRINK_UNREADABLE;
    if (fl) return NTFS_SHRINK_NOT_CLEAN;
    if (!ntfs_log_clean(&n)) return NTFS_SHRINK_NOT_CLEAN;
    u64 last;
    if (!ntfs_last_used(&n, &last)) return NTFS_SHRINK_UNREADABLE;
    if (last >= nclus || n.mft_lcn >= nclus || rd64(boot + 0x38) >= nclus) return NTFS_SHRINK_TOO_SMALL;
    /* $BadClus:$Bad tem de ser uma run esparsa só (nenhum cluster defeituoso marcado) */
    if (!mft_record(&n, 8, rec)) return NTFS_SHRINK_UNREADABLE;
    u8 *bad = attr_named(rec, n.rec, 0x80, "$Bad");
    if (!bad || !bad[8] || rd64(bad + 0x10) != 0) return NTFS_SHRINK_UNREADABLE;
    {
        Run rl[4]; int nr = runs_decode(bad + rd16(bad + 0x20), bad + rd32(bad + 4), rl, 4);
        if (nr != 1 || rl[0].lcn != ~0ull) return NTFS_SHRINK_BAD_CLUSTERS;
    }
    bool split;
    const u8 *bma = attr_get(&n, 6, 0x80, true, bm, &split);
    u64 bm_rec = attr_rec;
    if (!bma || bma == ATTR_ERR || split || !bma[8]) return NTFS_SHRINK_UNREADABLE;

    /* 2. a cópia do setor de boot no novo fim (espaço livre: depois do último cluster) */
    wr64(boot + 0x28, ntotal);
    static u8 sec[4096]; memcpy(sec, boot, n.bps);
    if (!d->rw(d->ctx, part_lba + ntotal, 1, sec, true)) return NTFS_SHRINK_WRITE;
    /* 3. marca o volume para verificação: se algo parar no meio, o Windows roda o chkdsk antes de usar */
    static u8 vol[4096];
    if (!mft_record(&n, 3, vol)) return NTFS_SHRINK_UNREADABLE;
    u8 *vi = (u8 *)attr_find(vol, n.rec, 0x70, true);
    if (!vi || vi[8]) return NTFS_SHRINK_UNREADABLE;
    u8 *vf = vi + rd16(vi + 0x14) + 0x0A; wr16(vf, rd16(vf) | 0x0001);
    if (!mft_write(&n, 3, vol)) return NTFS_SHRINK_WRITE;
    {   /* $MFTMirr guarda os registros 0 a 3: o mesmo registro 3, byte a byte (com o mesmo número de sequência) */
        u64 mirr = rd64(boot + 0x38), off = 3ull * n.rec; static u8 raw[4096];
        if (n.mft_runs[0].vcn != 0 || n.mft_runs[0].len * n.cs < 4ull * n.rec) return NTFS_SHRINK_UNREADABLE;
        if (!vol_read(&n, n.mft_runs[0].lcn * n.cs + off, raw, n.rec) || !vol_write(&n, mirr * n.cs + off, raw, n.rec)) return NTFS_SHRINK_WRITE;
    }
    /* 4. $BadClus:$Bad: uma run esparsa do tamanho novo */
    {
        u8 *r = bad + rd16(bad + 0x20), *end = bad + rd32(bad + 4);
        int lb = 1; while (lb < 8 && (nclus >> (8 * lb))) lb++;
        if (r + 1 + lb + 1 > end) return NTFS_SHRINK_UNREADABLE;
        u8 *z = r; while (z < end) *z++ = 0;
        r[0] = (u8)lb; for (int i = 0; i < lb; i++) r[1 + i] = (u8)(nclus >> (8 * i));
        wr64(bad + 0x18, nclus - 1);
        wr64(bad + 0x28, nclus * n.cs); wr64(bad + 0x30, nclus * n.cs); wr64(bad + 0x38, nclus * n.cs);
        if (!mft_write(&n, 8, rec)) return NTFS_SHRINK_WRITE;
    }
    /* 5. $Bitmap: tamanho novo (múltiplo de 8 bytes, como o Windows faz) e os bits de sobra ligados */
    {
        u64 bytes = (nclus + 63) / 64 * 8;
        if (bytes > rd64(bma + 0x28)) return NTFS_SHRINK_UNREADABLE;
        u64 first = nclus / 8 / n.bps * n.bps;                   /* setor onde começa a sobra */
        static u8 b[BUF];
        u32 k = (u32)((bytes - first + n.bps - 1) / n.bps * n.bps);
        if (k > sizeof b || !nonres_read(&n, bma, first, b, k)) return NTFS_SHRINK_UNREADABLE;
        for (u64 c = nclus; c < bytes * 8; c++) b[c / 8 - first] |= (u8)(1u << (c % 8));
        /* grava esses setores de volta, pelas runs do $Bitmap */
        Run rl[64]; int nr = runs_decode(bma + rd16(bma + 0x20), bma + rd32(bma + 4), rl, 64);
        if (nr <= 0) return NTFS_SHRINK_UNREADABLE;
        for (u32 o = 0; o < k; o += n.bps) {
            u64 off = first + o, vcn = off / n.cs; bool done = false;
            for (int i = 0; i < nr && !done; i++) {
                if (vcn < rl[i].vcn || vcn >= rl[i].vcn + rl[i].len || rl[i].lcn == ~0ull) continue;
                if (!vol_write(&n, (rl[i].lcn + (vcn - rl[i].vcn)) * n.cs + off % n.cs, b + o, n.bps)) return NTFS_SHRINK_WRITE;
                done = true;
            }
            if (!done) return NTFS_SHRINK_UNREADABLE;
        }
        u8 *wa = bm + (bma - bm);
        wr64(wa + 0x30, bytes); wr64(wa + 0x38, bytes);
        if (!mft_write(&n, bm_rec, bm)) return NTFS_SHRINK_WRITE;
    }
    /* 6. por último, o setor de boot principal com o total novo */
    if (!d->rw(d->ctx, part_lba, 1, boot, true)) return NTFS_SHRINK_WRITE;
    if (d->flush) d->flush(d->ctx);
    return NTFS_SHRINK_OK;
}

/* ------------------------------------------------------------ crescer NTFS (devolver o espaço) */
/* bits [from, to) do $Bitmap = val, um setor por vez (qualquer tamanho), pelas runs do atributo */
static int bm_set(Ntfs *n, const u8 *bma, u64 from, u64 to, int val) {
    Run rl[64]; int nr = runs_decode(bma + rd16(bma + 0x20), bma + rd32(bma + 4), rl, 64);
    if (nr <= 0) return NTFS_SHRINK_UNREADABLE;
    static u8 s[4096];
    u64 bits = (u64)n->bps * 8;
    for (u64 sec = from / bits; sec * bits < to; sec++) {
        u64 off = sec * n->bps, vcn = off / n->cs, at = ~0ull;
        for (int i = 0; i < nr && at == ~0ull; i++)
            if (vcn >= rl[i].vcn && vcn < rl[i].vcn + rl[i].len && rl[i].lcn != ~0ull) at = (rl[i].lcn + (vcn - rl[i].vcn)) * n->cs + off % n->cs;
        if (at == ~0ull) return NTFS_SHRINK_UNREADABLE;
        if (!n->d->rw(n->d->ctx, n->base + at / n->bps, 1, s, false)) return NTFS_SHRINK_UNREADABLE;
        for (u64 c = sec * bits; c < (sec + 1) * bits; c++) {
            if (c < from || c >= to) continue;
            u64 i = c - sec * bits;
            if (val) s[i / 8] |= (u8)(1u << (i % 8)); else s[i / 8] &= (u8)~(1u << (i % 8));
        }
        if (!vol_write(n, at, s, n->bps)) return NTFS_SHRINK_WRITE;
    }
    return NTFS_SHRINK_OK;
}
/* marca o volume para o chkdsk ($Volume e a cópia no $MFTMirr) */
static int mark_dirty(Ntfs *n, const u8 *boot) {
    static u8 vol[4096], raw[4096];
    if (!mft_record(n, 3, vol)) return NTFS_SHRINK_UNREADABLE;
    u8 *vi = (u8 *)attr_find(vol, n->rec, 0x70, true);
    if (!vi || vi[8]) return NTFS_SHRINK_UNREADABLE;
    u8 *vf = vi + rd16(vi + 0x14) + 0x0A; wr16(vf, rd16(vf) | 0x0001);
    if (!mft_write(n, 3, vol)) return NTFS_SHRINK_WRITE;
    u64 mirr = rd64(boot + 0x38), off = 3ull * n->rec;
    if (n->mft_runs[0].vcn != 0 || n->mft_runs[0].len * n->cs < 4ull * n->rec) return NTFS_SHRINK_UNREADABLE;
    if (!vol_read(n, n->mft_runs[0].lcn * n->cs + off, raw, n->rec) || !vol_write(n, mirr * n->cs + off, raw, n->rec)) return NTFS_SHRINK_WRITE;
    return NTFS_SHRINK_OK;
}

u64 ntfs_grow_max(const BlkDev *d, u64 part_lba) {
    static Ntfs n; static u8 bm[4096]; bool split;
    if (!ntfs_open(&n, d, part_lba)) return 0;
    const u8 *bma = attr_get(&n, 6, 0x80, true, bm, &split);
    if (!bma || bma == ATTR_ERR || split || !bma[8]) return 0;
    /* o $Bitmap não ganha clusters novos: o volume cresce até onde o espaço já reservado para ele cobre */
    return rd64(bma + 0x28) * 8 * (n.cs / n.bps) + 1;
}

int ntfs_grow(const BlkDev *d, u64 part_lba, u64 part_sectors, u64 new_sectors) {
    static Ntfs n; static u8 boot[4096], rec[4096], bm[4096];
    if (d->bs > sizeof boot || !ntfs_open(&n, d, part_lba)) return NTFS_SHRINK_UNREADABLE;
    if (!d->rw(d->ctx, part_lba, 1, boot, false)) return NTFS_SHRINK_UNREADABLE;
    u32 spc = n.cs / n.bps;
    u64 total = rd64(boot + 0x28), old = n.nclus;
    u64 ntotal = new_sectors - 1, nclus = ntotal / spc;
    if (new_sectors > part_sectors || nclus <= old || total + 1 > part_sectors) return NTFS_SHRINK_BAD_SIZE;
    /* sujo é aceito (o próprio encolher deixa assim até o Windows rodar o chkdsk); hibernado e diário aberto, não */
    int fl = ntfs_flags(&n);
    if (fl < 0) return NTFS_SHRINK_UNREADABLE;
    if ((fl & NTFS_HIBERNATED) || !ntfs_log_clean(&n)) return NTFS_SHRINK_NOT_CLEAN;
    if (!mft_record(&n, 8, rec)) return NTFS_SHRINK_UNREADABLE;
    u8 *bad = attr_named(rec, n.rec, 0x80, "$Bad");
    if (!bad || !bad[8] || rd64(bad + 0x10) != 0) return NTFS_SHRINK_UNREADABLE;
    {
        Run rl[4]; int nr = runs_decode(bad + rd16(bad + 0x20), bad + rd32(bad + 4), rl, 4);
        if (nr != 1 || rl[0].lcn != ~0ull) return NTFS_SHRINK_BAD_CLUSTERS;
    }
    bool split;
    const u8 *bma = attr_get(&n, 6, 0x80, true, bm, &split);
    u64 bm_rec = attr_rec;
    if (!bma || bma == ATTR_ERR || split || !bma[8]) return NTFS_SHRINK_UNREADABLE;
    u64 bytes = (nclus + 63) / 64 * 8;
    if (bytes > rd64(bma + 0x28)) return NTFS_SHRINK_TOO_SMALL;   /* além do que o $Bitmap cobre (ver ntfs_grow_max) */

    /* ordem: cópia do boot no novo fim (fora do volume atual) → marcado para o chkdsk → bits do $Bitmap (os
     * clusters novos livres, a sobra ligada; além do tamanho atual, o NTFS ainda não os vê) → $BadClus →
     * tamanho do $Bitmap → setor de boot. Até o último passo, o volume continua o antigo, só marcado. */
    wr64(boot + 0x28, ntotal);
    static u8 sec[4096]; memcpy(sec, boot, n.bps);
    if (!d->rw(d->ctx, part_lba + ntotal, 1, sec, true)) return NTFS_SHRINK_WRITE;
    int rc = mark_dirty(&n, boot);
    if (rc) return rc;
    if ((rc = bm_set(&n, bma, old, nclus, 0)) || (rc = bm_set(&n, bma, nclus, bytes * 8, 1))) return rc;
    {
        u8 *r = bad + rd16(bad + 0x20), *end = bad + rd32(bad + 4);
        int lb = 1; while (lb < 8 && (nclus >> (8 * lb))) lb++;
        if (r + 1 + lb + 1 > end) return NTFS_SHRINK_UNREADABLE;
        u8 *z = r; while (z < end) *z++ = 0;
        r[0] = (u8)lb; for (int i = 0; i < lb; i++) r[1 + i] = (u8)(nclus >> (8 * i));
        wr64(bad + 0x18, nclus - 1);
        wr64(bad + 0x28, nclus * n.cs); wr64(bad + 0x30, nclus * n.cs); wr64(bad + 0x38, nclus * n.cs);
        if (!mft_write(&n, 8, rec)) return NTFS_SHRINK_WRITE;
    }
    u8 *wa = bm + (bma - bm);
    wr64(wa + 0x30, bytes); wr64(wa + 0x38, bytes);
    if (!mft_write(&n, bm_rec, bm)) return NTFS_SHRINK_WRITE;
    if (!d->rw(d->ctx, part_lba, 1, boot, true)) return NTFS_SHRINK_WRITE;
    if (d->flush) d->flush(d->ctx);
    return NTFS_SHRINK_OK;
}

/* ------------------------------------------------------------ veredito por partição */
void fs_check(const BlkDev *d, const Part *p, FsCheck *o) {
    memset(o, 0, sizeof *o);
    o->fs = p->fs;
    switch (p->fs) {
    case FS_BITLOCKER: o->verdict = SAFE_BITLOCKER; return;
    case FS_LUKS: o->verdict = SAFE_LUKS; return;
    case FS_LVM: o->verdict = SAFE_LVM; return;
    case FS_APFS: case FS_HFSP: o->verdict = SAFE_APPLE; return;
    case FS_FAT12: o->verdict = SAFE_OK; return;                 /* FAT12 não tem marca de "sujo" */
    case FS_FAT16: case FS_FAT32: case FS_EXFAT: {
        /* "sujo": o Windows desliga o bit "desligado limpo" (e o "sem erros", quando acha erro) na entrada 1 da
         * FAT, e liga o bit 0 do byte de estado no BPB; o exFAT tem VolumeFlags (bit 1 sujo, bit 2 falha de mídia) */
        static u8 b[4096], fat[4096];
        if (d->bs > sizeof b || !d->rw(d->ctx, p->start, 1, b, false)) { o->verdict = SAFE_UNREADABLE; return; }
        if (p->fs == FS_EXFAT) { o->verdict = rd16(b + 0x6A) & 0x0006 ? SAFE_DIRTY : SAFE_OK; return; }
        u32 rsvd = rd16(b + 0x0E), bps = rd16(b + 0x0B);
        if (bps != d->bs || !rsvd || !d->rw(d->ctx, p->start + rsvd, 1, fat, false)) { o->verdict = SAFE_UNREADABLE; return; }
        bool dirty;
        if (p->fs == FS_FAT32) { u32 e1 = rd32(fat + 4); dirty = !(e1 & 0x08000000u) || !(e1 & 0x04000000u) || (b[0x41] & 1); }
        else { u16 e1 = rd16(fat + 2); dirty = !(e1 & 0x8000) || !(e1 & 0x4000) || (b[0x25] & 1); }
        o->verdict = dirty ? SAFE_DIRTY : SAFE_OK; return;
    }
    case FS_EXT: {
        static u8 b[4096];                                       /* um setor, de 512 ou de 4096 bytes */
        if (d->bs > sizeof b || !d->rw(d->ctx, p->start + 1024 / d->bs, 1, b, false)) { o->verdict = SAFE_UNREADABLE; return; }
        const u8 *sb = b + (1024 % d->bs);
        if (rd16(sb + 0x38) != 0xEF53) { o->verdict = SAFE_UNREADABLE; return; }
        if (rd16(sb + 0x3A) & 2) { o->verdict = SAFE_EXT_ERRORS; return; }               /* s_state: erros */
        if (rd32(sb + 0x60) & 0x4) { o->verdict = SAFE_EXT_RECOVER; return; }            /* diário pendente */
        o->verdict = SAFE_OK; return;
    }
    case FS_NTFS: {
        static Ntfs n;
        if (!ntfs_open(&n, d, p->start)) { o->verdict = SAFE_UNREADABLE; return; }
        int fl = ntfs_flags(&n);
        if (fl < 0) { o->verdict = SAFE_UNREADABLE; return; }
        if (fl & NTFS_HIBERNATED) { o->verdict = SAFE_HIBERNATED; return; }
        if (fl & NTFS_DIRTY) { o->verdict = SAFE_DIRTY; return; }
        if (!ntfs_log_clean(&n)) { o->verdict = SAFE_DIRTY; return; }    /* desligado sem fechar o diário */
        u64 last;
        if (!ntfs_last_used(&n, &last)) { o->verdict = SAFE_UNREADABLE; return; }
        o->min_bytes = (last + 1) * n.cs + n.bps;                 /* + a cópia do setor de boot no fim */
        o->verdict = SAFE_OK; return;
    }
    default: o->verdict = SAFE_UNSUPPORTED; return;
    }
}

#ifndef N_
#define N_(s) (s)
#endif
const char *fs_safety_text(int v) {
    switch (v) {
    case SAFE_OK: return "";
    case SAFE_BITLOCKER: return N_("This partition is encrypted with BitLocker. To make room, shrink it in Windows (Disk Management, Shrink Volume) or turn BitLocker off first.");
    case SAFE_LUKS: return N_("This partition is encrypted (LUKS). To make room, shrink it from Linux with the encryption tools.");
    case SAFE_LVM: return N_("This partition holds LVM volumes. To make room, shrink them from Linux (lvreduce, then pvresize).");
    case SAFE_APPLE: return N_("This is a Mac partition (APFS or HFS+). To make room, use Disk Utility on the Mac.");
    case SAFE_HIBERNATED: return N_("Windows is hibernated or uses Fast Startup. Start Windows, then shut it down while holding Shift (or turn Fast Startup off in Power Options), and try again.");
    case SAFE_DIRTY: return N_("Windows marked this partition as needing a check. Start Windows, run chkdsk /f on it, and try again.");
    case SAFE_EXT_RECOVER: return N_("This Linux partition was not unmounted cleanly. Start Linux and shut it down normally, then try again.");
    case SAFE_EXT_ERRORS: return N_("Linux found errors on this partition. Check it with fsck from Linux, then try again.");
    case SAFE_UNREADABLE: return N_("This partition could not be read safely, so FolioOS will not change it.");
    default: return N_("FolioOS cannot resize this file system yet.");
    }
}
const char *fs_safety_short(int v) {
    switch (v) {
    case SAFE_OK: return "";
    case SAFE_BITLOCKER: return N_("encrypted (BitLocker)");
    case SAFE_LUKS: return N_("encrypted (LUKS)");
    case SAFE_LVM: return "LVM";
    case SAFE_APPLE: return N_("Mac (APFS/HFS+)");
    case SAFE_HIBERNATED: return N_("Windows hibernated or Fast Startup");
    case SAFE_DIRTY: return N_("needs a check (chkdsk)");
    case SAFE_EXT_RECOVER: return N_("not unmounted cleanly");
    case SAFE_EXT_ERRORS: return N_("has errors (fsck)");
    case SAFE_UNREADABLE: return N_("unreadable");
    default: return N_("cannot be resized yet");
    }
}
