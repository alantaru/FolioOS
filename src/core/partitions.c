/* FolioOS — gerenciador de partições (fase 1 do ROADMAP): os discos do computador, com as partições e os espaços
 * livres; criar FAT32 num espaço livre, redimensionar NTFS/FAT32/exFAT sem mover dados, formatar como FAT32 e apagar.
 * Regras (ROADMAP, "Rules of the partition manager"): nada acontece sem a prévia antes → depois; o começo do disco vai
 * antes para FOLIO/BACKUP no pendrive; apagar e formatar pedem a palavra de confirmação; tudo pelo teclado.
 * Quem grava é o motor (install.c, part_op_*); o disco de onde o FolioOS iniciou nunca aparece. */
#include "folio.h"
#include "disks.h"
#include "install.h"
#include "fscheck.h"

enum { PS_LIST, PS_CREATE, PS_RESIZE, PS_FORMAT, PS_DELETE, PS_WORKING };
#define PD 16
#define PR 48
typedef struct { int d; bool free, none; int part; u64 start, count; } Row;        /* uma linha: partição ou espaço livre */
static struct { int k; DiskInfo di; char why[96]; } dsk[PD];
static int ndsk;
static PartTable tab[PD];                                                     /* a tabela de cada disco, como foi lida */
static FsCheck cv[PD][PART_MAX];
static Row rows[PD * PR]; static int nrows, cur = -1, st;
static float size_mb, size_lo, size_hi;                                       /* criar/redimensionar: o tamanho escolhido */
static char label[12] = "DATA", word[48];
static bool pending;
static InstallResult res;
static char backup_name[64];

static void parts_body(Window *w, Rect r);
static Window win_parts = { N_("Disks and partitions"), IC_DRIVE, {0, 0, 680, 620}, false, true, true, false, false, 0, 520, 440,
                            parts_body, 0, 0, false, -1, 0, 0, 0 };
Window *partitions_window(void) { return &win_parts; }

/* ------------------------------------------------------------ leitura */
static void scan(void) {
    ndsk = 0; nrows = 0;
    for (int k = 0; k < pal_disk_count() && ndsk < PD; k++) {
        DiskInfo di; BlkDev b;
        if (!pal_disk_info(k, &di) || di.boot || !disk_dev(k, &b)) continue;   /* o pendrive de onde o FolioOS iniciou: nunca */
        int d = ndsk++;
        dsk[d].k = k; dsk[d].di = di; dsk[d].why[0] = 0;
        if (di.readonly) str_copy(dsk[d].why, L("This disk is read-only."), sizeof dsk[d].why);
        else if (di.bs != 512) str_copy(dsk[d].why, L("Disks with 4096-byte sectors are not supported yet."), sizeof dsk[d].why);
        PartTable *t = &tab[d];
        /* uma tabela ilegível nunca vira "disco vazio"; uma que o FolioOS não regravaria exatamente fica só para ver */
        if (!part_read(&b, t)) { t->n = 0; t->scheme = SCHEME_UNREADABLE; t->locked = true; }
        if (!dsk[d].why[0] && t->scheme == SCHEME_UNREADABLE) str_copy(dsk[d].why, L("Partition table unreadable"), sizeof dsk[d].why);
        else if (!dsk[d].why[0] && t->scheme != SCHEME_NONE && !part_editable(t))
            str_copy(dsk[d].why, L("FolioOS cannot change this disk's partition table safely: it is left as it is"), sizeof dsk[d].why);
        for (int p = 0; p < t->n; p++) {
            cv[d][p].verdict = SAFE_OK; cv[d][p].min_bytes = 0;
            if (t->p[p].fs == FS_NTFS || t->p[p].fs == FS_EXT || t->p[p].fs == FS_FAT16 || t->p[p].fs == FS_FAT32 || t->p[p].fs == FS_EXFAT) fs_check(&b, &t->p[p], &cv[d][p]);
        }
        /* partições e espaços livres na ordem do disco */
        Region fr[32]; int nf = di.bs == 512 && !t->locked ? part_free(&b, t, fr, 32) : 0, fi = 0, pi = 0;
        int order[PART_MAX]; for (int i = 0; i < t->n; i++) order[i] = i;
        for (int i = 1; i < t->n; i++) for (int j = i; j > 0 && t->p[order[j]].start < t->p[order[j - 1]].start; j--) { int x = order[j]; order[j] = order[j - 1]; order[j - 1] = x; }
        int count = 0;
        while ((pi < t->n || fi < nf) && count < PR && nrows < PD * PR) {
            bool take_free = fi < nf && (pi >= t->n || fr[fi].start < t->p[order[pi]].start);
            Row *rw = &rows[nrows++]; rw->d = d;
            rw->none = false;
            if (take_free) { rw->free = true; rw->part = -1; rw->start = fr[fi].start; rw->count = fr[fi].count; fi++; }
            else { rw->free = false; rw->part = order[pi]; rw->start = t->p[order[pi]].start; rw->count = t->p[order[pi]].count; pi++; }
            count++;
        }
        /* um disco sem nenhuma linha (tabela ilegível ou travada sem partições) continua na lista, com o motivo */
        if (!count && nrows < PD * PR) { Row *rw = &rows[nrows++]; rw->d = d; rw->none = true; rw->free = false; rw->part = -1; rw->start = rw->count = 0; }
    }
    if (cur >= nrows) cur = nrows - 1;
    if (cur < 0 && nrows) cur = 0;
}
void partitions_open(void) {
    if (pending) return;
    cur = -1; st = PS_LIST; scan();
    win_parts.r.w = MIN(SI(700), ui.W - SI(40)); win_parts.r.h = MIN(SI(640), ui.H - SI(60));
    win_parts.min_w = SI(520); win_parts.min_h = SI(440);
    win_parts.centered_once = false; win_parts.nav = -1;
    wm_open(&win_parts);
}

/* ------------------------------------------------------------ gravação (no começo do quadro seguinte) */
static bool s_backup(void *ctx, const char *name, const void *data, u32 n) {
    UNUSED(ctx);
    paths_ensure(app_boot_vol(), DIR_FOLIO);
    char d[200], p[260]; fmt(d, sizeof d, "%s/BACKUP", paths_dir(DIR_FOLIO));
    pal_mkdir(app_boot_vol(), d);
    fmt(p, sizeof p, "%s/%s", d, name);
    str_copy(backup_name, p, sizeof backup_name);
    return pal_write(app_boot_vol(), p, data, n);
}
void partitions_poll(void) {
    if (!pending) return;
    pending = false;
    Row rw = rows[cur]; BlkDev b; InstallSrc s; memset(&s, 0, sizeof s);
    DateTime t; pal_now(&t);
    fmt(s.date, sizeof s.date, "%04d-%02d-%02d-%02d%02d%02d", t.year, t.month, t.day, t.hour, t.min, t.sec);
    s.backup = s_backup; s.seed = pal_ms() * 0x9E3779B97F4A7C15ull ^ (u64)t.sec << 40;
    s.bufsize = 1u << 20; s.buf = mem_alloc(s.bufsize);
    backup_name[0] = 0;
    memset(&res, 0, sizeof res);
    if (!s.buf || !disk_dev(dsk[rw.d].k, &b)) res.code = INST_ERR_WRITE;
    else {
        u64 ns = (u64)size_mb * 2048;
        int op = st;
        if (op == PS_CREATE) part_op_create(&b, &s, rw.start, MIN(ns, rw.count), label, &res);
        else if (op == PS_RESIZE) part_op_resize(&b, &s, rw.start, ns, &res);
        else if (op == PS_FORMAT) part_op_format(&b, &s, rw.start, label, &res);
        else if (op == PS_DELETE) part_op_delete(&b, &s, rw.start, &res);
    }
    mem_free(s.buf);
    char m[240];
    if (res.code == INST_OK) str_copy(m, L("Done. The previous start of the disk is saved on this USB drive."), sizeof m);
    else fmt(m, sizeof m, res.path[0] ? "%s: %s" : "%s", L(install_error(res.code)), res.path);
    ui_toast(m);
    dbg("partitions: op %d -> %d (%s)", st, res.code, res.path);
    st = PS_LIST; word[0] = 0; win_parts.nav = -1; ui.focus = 0;
    scan();
    ui.redraw = true;
}

/* ------------------------------------------------------------ desenho */
static float wrap(Face *f, float px, float x, float y, float w, const char *s, Color c) {
    int len = str_len(s), i = 0; float lh = px * 1.45f;
    while (i < len) {
        int n = text_fit(f, px, s + i, len - i, w);
        if (n <= 0) n = utf8_next(s + i, len - i, 0);
        if (i + n < len) { int k = n; while (k > 0 && s[i + k] != ' ') k--; if (k > 0) n = k; }
        draw_text(f, px, x, y + px, s + i, n, c);
        i += n; while (i < len && s[i] == ' ') i++;
        y += lh;
    }
    return y;
}
static void disk_title(int d, char *o, int cap) {
    char sz[24]; disk_size_text(dsk[d].di.nsec * dsk[d].di.bs, sz, sizeof sz);
    fmt(o, cap, "%s · %s · %s", disk_kind_name(dsk[d].di.kind), dsk[d].di.model[0] ? dsk[d].di.model : L("disk"), sz);
}
static void row_text(const Row *rw, char *o, int cap) {
    char sz[24]; disk_size_text(rw->count * 512, sz, sizeof sz);
    if (rw->none) { str_copy(o, dsk[rw->d].why[0] ? dsk[rw->d].why : L("Empty: no partitions"), cap); return; }
    if (rw->free) { fmt(o, cap, L("Free space · %s"), sz); return; }
    PartTable *t = &tab[rw->d]; Part *p = &t->p[rw->part];
    const char *ty = part_type_name(p, t->scheme), *fs = part_fs_name(p->fs);
    const char *state = cv[rw->d][rw->part].verdict != SAFE_OK ? L(fs_safety_short(cv[rw->d][rw->part].verdict)) : "";
    fmt(o, cap, "%s%s%s · %s%s%s%s%s%s", ty, str_eq(ty, fs) ? "" : " · ", str_eq(ty, fs) ? "" : fs, sz,
        p->label[0] ? " · \"" : "", p->label, p->label[0] ? "\"" : "", state[0] ? " · " : "", state);
}
/* até onde a partição pode crescer: o espaço livre logo depois dela (ou o fim do disco) e o que o volume aguenta */
static u64 grow_limit(const Row *rw) {
    PartTable *t = &tab[rw->d]; Part *p = &t->p[rw->part];
    u64 end = dsk[rw->d].di.nsec - (t->scheme == SCHEME_GPT ? 34 : 0);
    for (int i = 0; i < t->n; i++) if (t->p[i].start > p->start && t->p[i].start < end) end = t->p[i].start;
    BlkDev b; u64 fsmax = disk_dev(dsk[rw->d].k, &b) ? fs_grow_max(&b, p->fs, p->start) : 0;
    u64 lim = end - p->start;
    return fsmax && fsmax < lim ? fsmax : lim;
}
static bool word_ok(const char *typed, const char *w) {
    while (*typed == ' ') typed++;
    int n = str_len(typed); while (n > 0 && typed[n - 1] == ' ') n--;
    int m = str_len(w);
    return n && m && str_find_i(typed, n, w, m, 0) == 0 && str_find_i(w, m, typed, n, 0) == 0;
}

static void parts_body(Window *w, Rect r) {
    Face *f = ui_font(false), *fb = ui_font(true);
    int x = r.x + SI(22), wd = r.w - SI(44);
    float y = r.y + S(16);
    Color dim = col_alpha(T->panel_text, 0.62f), red = 0xFFC0392B;
    int by = r.y + r.h - SI(56);
    char line[240], a[24], b2[24];
    if (st == PS_WORKING) { wrap(fb, S(15), x, y + S(20), wd, L("Working… Do not turn off the computer or remove the USB drive."), T->panel_text); return; }

    if (st == PS_LIST) {
        y = wrap(f, S(13.5f), x, y, wd, L("Choose a partition or a free space, then an action. Before anything is written, the start of the disk is saved to FOLIO/BACKUP on this USB drive."), dim) + S(6);
        if (!ndsk) wrap(f, S(14), x, y + S(10), wd, L("No disk was found besides the USB drive FolioOS started from."), T->panel_text);
        int last_d = -1;
        for (int i = 0; i < nrows && y < by - S(60); i++) {
            Row *rw = &rows[i];
            if (rw->d != last_d) {
                last_d = rw->d; disk_title(rw->d, line, sizeof line);
                y = wrap(fb, S(14), x, y + S(6), wd, line, T->panel_text);
                if (dsk[rw->d].why[0]) y = wrap(f, S(12.5f), x + S(12), y, wd - S(12), dsk[rw->d].why, red);
            }
            Rect it = rect(x, (int)y + SI(2), wd, SI(32));
            bool fire = ui_item(it), on = i == cur;
            if (on) gfx_rrect(it, S(8), col_alpha(T->accent, 0.14f)); else if (ui_hover(it)) gfx_rrect(it, S(8), col_alpha(T->text, 0.06f));
            row_text(rw, line, sizeof line);
            draw_text_ellipsis(f, S(13.5f), it.x + S(14), it.y + it.h * 0.5f + S(5), it.w - S(24), line, rw->free ? dim : T->panel_text);
            if (fire) cur = i;
            y += S(36);
        }
        /* as ações do item escolhido */
        if (cur >= 0 && cur < nrows && !dsk[rows[cur].d].why[0] && !rows[cur].none) {
            Row *rw = &rows[cur]; int bx = x;
            if (rw->free) {
                if (rw->count * 512 >= (64ull << 20) && ui_button(rect(bx, by, SI(240), SI(40)), L("Create a FAT32 partition"), BTN_PRIMARY)) {
                    st = PS_CREATE; size_lo = 64; size_hi = (float)(rw->count >> 11); size_mb = size_hi; str_copy(label, "DATA", sizeof label); w->nav = -1;
                }
            } else {
                Part *p = &tab[rw->d].p[rw->part]; FsCheck *c = &cv[rw->d][rw->part];
                if (fs_resizable(p->fs) && c->verdict == SAFE_OK && c->min_bytes) {
                    if (ui_button(rect(bx, by, SI(150), SI(40)), L("Resize"), BTN_PRIMARY)) {
                        u64 lim = grow_limit(rw);
                        st = PS_RESIZE; size_lo = (float)((c->min_bytes + (1 << 20) - 1) >> 20); size_hi = (float)(lim >> 11); size_mb = (float)(rw->count >> 11); w->nav = -1;
                    }
                    bx += SI(160);
                }
                if (ui_button(rect(bx, by, SI(180), SI(40)), L("Format as FAT32"), BTN_GHOST)) { st = PS_FORMAT; word[0] = 0; str_copy(label, "DATA", sizeof label); w->nav = -1; }
                bx += SI(190);
                if (ui_button(rect(bx, by, SI(140), SI(40)), L("Delete"), BTN_DANGER)) { st = PS_DELETE; word[0] = 0; w->nav = -1; }
            }
        }
        return;
    }

    /* telas de confirmação: o que vai acontecer, em palavras, antes → depois */
    Row *rw = &rows[cur]; PartTable *t = &tab[rw->d];
    disk_title(rw->d, line, sizeof line);
    y = wrap(fb, S(14), x, y, wd, line, T->panel_text);
    row_text(rw, line, sizeof line);
    y = wrap(f, S(13.5f), x + S(12), y, wd - S(12), line, T->panel_text) + S(8);
    const char *cw = L("ERASE");
    bool ok = true, need_word = st == PS_FORMAT || st == PS_DELETE;
    if (st == PS_CREATE || st == PS_RESIZE) {
        size_mb = CLAMP((float)(int)size_mb, size_lo, size_hi);
        disk_size_text((u64)size_mb << 20, a, sizeof a);
        if (st == PS_CREATE) fmt(line, sizeof line, L("+ FAT32 · %s, in this free space"), a);
        else { disk_size_text(rw->count * 512, b2, sizeof b2); fmt(line, sizeof line, L("%s%s: %s → %s"), part_fs_name(t->p[rw->part].fs), "", b2, a); }
        y = wrap(fb, S(13.5f), x + S(12), y, wd - S(12), line, T->accent) + S(6);
        ui_slider(rect(x, (int)y, wd, SI(52)), &size_mb, size_lo, size_hi, L("Size"), a);
        y += S(58);
        if (st == PS_RESIZE) y = wrap(f, S(13), x, y, wd, t->p[rw->part].fs == FS_NTFS ? L("No file is moved: FolioOS only takes free space from the end of that partition. The next time Windows starts, it checks the partition (chkdsk) once; this is expected.") : L("No file is moved: FolioOS only takes free space from the end of that partition."), dim);
        ok = (u64)size_mb != (rw->count >> 11) || st == PS_CREATE;
    }
    if (st == PS_CREATE || st == PS_FORMAT) {
        y = wrap(f, S(13.5f), x, y + S(4), wd, L("Name of the new volume:"), T->panel_text) + S(2);
        ui_textfield(rect(x, (int)y, SI(220), SI(38)), label, sizeof label, "DATA", false);
        y += S(46);
    }
    if (st == PS_FORMAT) y = wrap(fb, S(13.5f), x, y, wd, L("Everything on this partition will be erased; it becomes an empty FAT32 volume."), red) + S(4);
    if (st == PS_DELETE) y = wrap(fb, S(13.5f), x, y, wd, L("This partition and everything on it will be erased; its space becomes free."), red) + S(4);
    if (need_word) {
        fmt(line, sizeof line, L("To confirm, type %s:"), cw);
        y = wrap(fb, S(14), x, y, wd, line, T->panel_text) + S(4);
        ui_textfield(rect(x, (int)y, SI(260), SI(40)), word, sizeof word, cw, false);
        ok = word_ok(word, cw);
    }
    if (ui_button(rect(x, by, SI(140), SI(40)), L("Back"), BTN_GHOST)) { st = PS_LIST; ui.focus = 0; w->nav = -1; return; }
    const char *go = st == PS_CREATE ? L("Create") : st == PS_RESIZE ? L("Apply") : st == PS_FORMAT ? L("Format") : L("Delete");
    if (ui_button(rect(x + wd - SI(200), by, SI(200), SI(40)), go, ok ? (need_word ? BTN_DANGER : BTN_PRIMARY) : BTN_SUBTLE)) {
        if (ok) { pending = true; ui.focus = 0; w->nav = -1; }
        else if (need_word) { fmt(line, sizeof line, L("To confirm, type %s:"), cw); ui_toast(line); }
    }
}
