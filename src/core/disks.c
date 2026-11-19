/* FolioOS — discos do computador vistos pelo núcleo: liga a PAL (pal_disk_*) às tabelas de partição
 * (part.c). Só leitura aqui; quem grava é o instalador, depois da confirmação do usuário.
 * A lista vai para o DIAGNOSTICS.TXT, então cada boot já confere a leitura das tabelas no firmware real. */
#include "folio.h"
#include "part.h"
#include "disks.h"

static bool blk_rw(void *ctx, u64 lba, u32 n, void *buf, bool w) { return pal_disk_io((int)(intptr_t)ctx, lba, n, buf, w); }
static bool blk_flush(void *ctx) { return pal_disk_flush((int)(intptr_t)ctx); }
bool disk_dev(int disk, BlkDev *out) {
    DiskInfo di;
    if (!pal_disk_info(disk, &di)) return false;
    out->rw = blk_rw; out->ctx = (void *)(intptr_t)disk; out->nsec = di.nsec; out->bs = di.bs; out->flush = blk_flush;
    return true;
}
const char *disk_kind_name(int kind) {
    switch (kind) {
    case DISK_SATA: return "SATA"; case DISK_NVME: return "NVMe"; case DISK_USB: return "USB"; case DISK_IDE: return "IDE";
    case DISK_SD: return "SD"; case DISK_MMC: return "eMMC"; case DISK_SCSI: return "SCSI"; case DISK_VIRTUAL: return "virtual";
    default: return "disk";
    }
}
/* "238 GB", "512 MB" (potências de 1000, como na etiqueta do disco) */
void disk_size_text(u64 bytes, char *o, int cap) {
    u64 mb = bytes / 1000000;
    if (mb >= 10000) fmt(o, cap, "%u GB", (unsigned)(mb / 1000));
    else if (mb >= 1000) fmt(o, cap, "%u.%u GB", (unsigned)(mb / 1000), (unsigned)(mb % 1000 / 100));
    else fmt(o, cap, "%u MB", (unsigned)mb);
}

void disks_diag(Buf *o) {
    int n = pal_disk_count();
    buf_printf(o, "== Disks (%d found)\r\n", n);
    PartTable *t = mem_alloc(sizeof *t);
    if (!t) return;
    for (int k = 0; k < n; k++) {
        DiskInfo di; BlkDev d; char sz[24];
        if (!pal_disk_info(k, &di) || !disk_dev(k, &d)) continue;
        disk_size_text(di.nsec * di.bs, sz, sizeof sz);
        buf_printf(o, "  %d. %s · %s · %s · %u-byte sectors%s%s%s\r\n", k + 1, disk_kind_name(di.kind), di.model[0] ? di.model : "(no model)", sz,
                   di.bs, di.removable ? " · removable" : "", di.readonly ? " · read-only" : "", di.boot ? " · FolioOS started from here" : "");
        if (di.bs != 512 && di.bs != 4096) continue;
        if (!part_read(&d, t)) { buf_puts(o, "     partition table: unreadable\r\n"); continue; }
        static const char *sch[] = { "none (empty disk)", "MBR", "GPT", "none (whole-disk volume)", "unreadable" };
        buf_printf(o, "     partition table: %s%s%s%s\r\n", sch[t->scheme], t->gpt_backup_used ? " (main header damaged, backup used)" : "",
                   t->has_extended ? " · extended partition" : "", t->locked && t->scheme != SCHEME_UNREADABLE ? " · FolioOS will not change it" : "");
        for (int i = 0; i < t->n; i++) {
            Part *p = &t->p[i];
            disk_size_text(p->count * di.bs, sz, sizeof sz);
            const char *ty = part_type_name(p, t->scheme), *fs = part_fs_name(p->fs);
            buf_printf(o, "     %c %s%s%s · %s%s%s%s\r\n", p->logical ? '-' : '*', ty, str_eq(ty, fs) ? "" : " · ", str_eq(ty, fs) ? "" : fs, sz,
                       p->label[0] ? " · \"" : "", p->label, p->label[0] ? "\"" : "");
        }
    }
    mem_free(t);
}
