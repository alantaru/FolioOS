/* FolioOS — tela do instalador: instala o FolioOS no disco do próprio computador, ao lado do sistema que já existe
 * (fase 2: no espaço livre de um disco GPT, nada é apagado) ou como único sistema
 * (fase 1). Passos: escolher onde → ver o que muda (no disco inteiro, digitar também a palavra de
 * confirmação → progresso → resultado. Tudo funciona só pelo teclado (ui_item/ui_textfield/ui_button).
 * Quem grava é o motor (install.c); aqui só se monta a origem (o pendrive, pela PAL) e o destino. */
#include "folio.h"
#include "disks.h"
#include "install.h"
#include "fscheck.h"

enum { ST_CHOOSE, ST_CONFIRM, ST_RUN, ST_DONE, ST_FAIL };
enum { MODE_WHOLE, MODE_ALONGSIDE, MODE_SHRINK, MODE_REMOVE, MODE_UPDATE };
#define MAXD 16
static int   st, sel = -1, ndisk, nopt;
static int   dk[MAXD];                     /* índice na PAL */
static DiskInfo di[MAXD];
static char  why[MAXD][96];                /* "" = pode instalar; senão o motivo */
static char  what_on[MAXD][6][96];         /* o que há no disco hoje, uma linha por partição */
static int   n_on[MAXD];
static bool  bitlocker[MAXD];              /* há partição com BitLocker: ao lado, pedir a confirmação da chave */
static char  hint[MAXD][200];             /* sem opção "ao lado": como abrir espaço no próprio sistema */
static bool  has_key;                      /* "tenho a chave de recuperação do BitLocker" */
/* opções da lista: ao lado de outro sistema (no espaço livre, ou encolhendo o NTFS dele: nada é apagado) ou o
 * disco inteiro. Encolhendo: a partição (início, tamanho, rótulo) e o máximo que o FolioOS pode ganhar */
static struct { int d, mode; Region fr; u64 pstart, pcount, maxfol; char plabel[40]; } opt[MAXD * 4];
static float fol_mb;                       /* encolhendo: o tamanho escolhido para o FolioOS, em MiB */
static InstallFound found[MAXD];            /* o FolioOS já instalado ao lado, em cada disco (para remover) */
static bool  copy_texts = true;            /* remover: copiar os textos para o pendrive antes */
static int   docs_choice;                  /* instalar: onde ficam os textos (0 computador, 1 pendrive, 2 os dois) */
#define SHRINK_DEFAULT_MB 4096.0f          /* o FolioOS guarda textos: 4 GB é muito, e o resto continua do outro sistema */
static char  confirm[48];
static bool  run_pending, running, entry_ok;
static int   pct; static char step[160];
static InstallResult res;
static char  backup_name[64];

static void installer_body(Window *w, Rect r);
static Window win_install = { N_("Install FolioOS on this computer"), IC_DRIVE, {0, 0, 640, 600}, false, true, true, false, false, 0, 480, 420,
                              installer_body, 0, 0, false, -1, 0, 0, 0 };
Window *installer_window(void) { return &win_install; }

/* ------------------------------------------------------------ discos */
static bool is_bios(void) { return str_find_i(pal_name(), str_len(pal_name()), "BIOS", 4, 0) >= 0; }
static void scan(void) {
    ndisk = 0; nopt = 0; sel = -1;
    static PartTable t;
    int n = pal_disk_count();
    for (int k = 0; k < n && ndisk < MAXD; k++) {
        DiskInfo d; BlkDev b;
        if (!pal_disk_info(k, &d) || d.boot || !disk_dev(k, &b)) continue;   /* o pendrive de onde o FolioOS iniciou nunca aparece */
        int i = ndisk++;
        dk[i] = k; di[i] = d; why[i][0] = 0; n_on[i] = 0; bitlocker[i] = false; hint[i][0] = 0;
        if (d.readonly) str_copy(why[i], L("This disk is read-only."), sizeof why[i]);
        else if (d.bs != 512) str_copy(why[i], L("Disks with 4096-byte sectors are not supported yet."), sizeof why[i]);
        else if (d.nsec * d.bs < INSTALL_MIN_BYTES) str_copy(why[i], L("This disk is too small (at least 512 MB)."), sizeof why[i]);
        bool table = (d.bs == 512 || d.bs == 4096) && part_read(&b, &t);
        /* o estado de cada NTFS/ext (só leitura), uma vez: serve para a lista e para a opção de encolher */
        static FsCheck cv[PART_MAX];
        for (int p = 0; table && p < t.n; p++) {
            cv[p].verdict = SAFE_OK; cv[p].min_bytes = 0;
            int fs = t.p[p].fs;
            if (fs == FS_NTFS || fs == FS_EXT || fs == FS_FAT16 || fs == FS_FAT32 || fs == FS_EXFAT) fs_check(&b, &t.p[p], &cv[p]);
        }
        /* o FolioOS já está aqui, ao lado de outro sistema: a 1ª opção é removê-lo */
        found[i].found = false;
        if (table && !why[i][0] && install_find(&b, &found[i]) && nopt < MAXD * 4) {
            opt[nopt].d = i; opt[nopt].mode = MODE_UPDATE; opt[nopt].fr.start = opt[nopt].fr.count = 0; nopt++;   /* atualizar vem antes de remover */
            if (nopt < MAXD * 4) { opt[nopt].d = i; opt[nopt].mode = MODE_REMOVE; opt[nopt].fr.start = opt[nopt].fr.count = 0; nopt++; }
        }
        /* ao lado: GPT, partição EFI FAT32 e espaço livre que chegue (só em PCs UEFI) */
        if (table && t.scheme == SCHEME_GPT && !is_bios() && !why[i][0] && !found[i].found) {
            bool esp = false; for (int p = 0; p < t.n; p++) if (!memcmp(t.p[p].type, GPT_ESP, 16) && t.p[p].fs == FS_FAT32 && cv[p].verdict == SAFE_OK) esp = true;   /* a EFI recebe a pasta EFI/FolioOS: só se estiver em ordem */
            Region fr[16]; int nf = part_free(&b, &t, fr, 16), big = -1;
            for (int f = 0; f < nf; f++) if (big < 0 || fr[f].count > fr[big].count) big = f;
            if (esp && big >= 0 && fr[big].count * d.bs >= ALONGSIDE_MIN_BYTES && t.n > 0 && nopt < MAXD * 4) {
                opt[nopt].d = i; opt[nopt].mode = MODE_ALONGSIDE; opt[nopt].fr = fr[big]; nopt++;
            } else if (esp && nopt < MAXD * 4) {
                /* sem espaço livre: encolher um NTFS em ordem. O outro sistema fica com o que usa + 10% da partição,
                 * e o FolioOS com o resto (até o que a pessoa escolher); escolhe o NTFS que mais pode ceder */
                int bp = -1; u64 bmax = 0;
                for (int p = 0; p < t.n; p++) {
                    if (t.p[p].fs != FS_NTFS || cv[p].verdict != SAFE_OK || !cv[p].min_bytes) continue;
                    u64 size = t.p[p].count * d.bs, keep = cv[p].min_bytes + size / 10;
                    u64 room = size > keep ? (size - keep) & ~((1ull << 20) - 1) : 0;
                    if (room >= ALONGSIDE_MIN_BYTES && room > bmax) { bmax = room; bp = p; }
                }
                if (bp >= 0) {
                    opt[nopt].d = i; opt[nopt].mode = MODE_SHRINK; opt[nopt].fr.start = opt[nopt].fr.count = 0;
                    opt[nopt].pstart = t.p[bp].start; opt[nopt].pcount = t.p[bp].count; opt[nopt].maxfol = bmax;
                    str_copy(opt[nopt].plabel, t.p[bp].label, sizeof opt[nopt].plabel); nopt++;
                } else {
                    /* não dá para abrir espaço daqui: diz como fazer no próprio sistema (o ext4 o FolioOS não encolhe:
                     * o Linux espalha os dados pelo volume inteiro, e encolher exigiria mover blocos) */
                    bool lin = false, win = false;
                    for (int p = 0; p < t.n; p++) { if (t.p[p].fs == FS_EXT) lin = true; if (t.p[p].fs == FS_NTFS) win = true; }
                    if (lin) str_copy(hint[i], L("To keep Linux, shrink its partition from Linux (with GParted, for example) and run the installer again: FolioOS will use the free space."), sizeof hint[i]);
                    else if (win) str_copy(hint[i], L("To keep Windows, shrink drive C: in Windows (Disk Management, Shrink Volume) and run the installer again: FolioOS will use the free space."), sizeof hint[i]);
                }
            }
        }
        if (nopt < MAXD * 4) { opt[nopt].d = i; opt[nopt].mode = MODE_WHOLE; opt[nopt].fr.start = opt[nopt].fr.count = 0; nopt++; }
        if (d.bs != 512 && d.bs != 4096) continue;
        if (!table) { str_copy(what_on[i][n_on[i]++], L("Partition table unreadable"), 96); continue; }
        if (t.scheme == SCHEME_NONE) { str_copy(what_on[i][n_on[i]++], L("Empty: no partitions"), 96); continue; }
        for (int p = 0; p < t.n; p++) if (t.p[p].fs == FS_BITLOCKER) bitlocker[i] = true;
        for (int p = 0; p < t.n && n_on[i] < 6; p++) {
            Part *pp = &t.p[p]; char sz[24];
            disk_size_text(pp->count * d.bs, sz, sizeof sz);
            const char *ty = part_type_name(pp, t.scheme), *fs = part_fs_name(pp->fs);
            if (n_on[i] == 5 && t.n > 6) { fmt(what_on[i][n_on[i]++], 96, LN("…and %d more partition", "…and %d more partitions", t.n - 5), t.n - 5); break; }
            /* o estado do sistema de arquivos, quando pede uma ação no sistema dono dele (só leitura) */
            const char *state = cv[p].verdict != SAFE_OK ? L(fs_safety_short(cv[p].verdict)) : "";
            fmt(what_on[i][n_on[i]++], 96, "%s%s%s · %s%s%s%s%s%s", ty, str_eq(ty, fs) ? "" : " · ", str_eq(ty, fs) ? "" : fs, sz,
                pp->label[0] ? " · \"" : "", pp->label, pp->label[0] ? "\"" : "", state[0] ? " · " : "", state);
        }
    }
    for (int i = 0; i < nopt; i++) if (!why[opt[i].d][0]) { sel = i; break; }   /* a 1ª possível (remover ou ao lado vêm antes) */
}
static void disk_title(int i, char *o, int cap) {
    char sz[24]; disk_size_text(di[i].nsec * di[i].bs, sz, sizeof sz);
    fmt(o, cap, "%s · %s · %s", disk_kind_name(di[i].kind), di[i].model[0] ? di[i].model : L("disk"), sz);
}

static void open_with(int mode) {
    if (running) return;
    scan();
    /* pela paleta "Remover…"/"Atualizar…": essa opção já vem marcada (se existir) */
    if (mode >= 0) for (int i = 0; i < nopt; i++) if (opt[i].mode == mode && !why[opt[i].d][0]) { sel = i; break; }
    st = ST_CHOOSE; confirm[0] = 0;
    win_install.r.w = MIN(SI(660), ui.W - SI(40)); win_install.r.h = MIN(SI(680), ui.H - SI(60));
    win_install.min_w = SI(480); win_install.min_h = SI(420);
    win_install.centered_once = false; win_install.nav = -1;
    wm_open(&win_install);
}
void installer_open(void) { open_with(-1); }
void installer_open_remove(void) { open_with(MODE_REMOVE); }
void installer_open_update(void) { open_with(MODE_UPDATE); }

/* ------------------------------------------------------------ origem: o pendrive, pela PAL */
static PalFile *rf; static char rf_path[200];
static void abs_path(const char *rel, char *o, int cap) { fmt(o, cap, "/%s", rel); }
static bool s_list(void *ctx, const char *dir, void (*cb)(void *, const char *, bool, u64), void *u) {
    UNUSED(ctx); char p[200]; abs_path(dir, p, sizeof p);
    FileEntry *l; int n = pal_list(app_boot_vol(), p, &l);
    if (n < 0) return false;
    for (int i = 0; i < n; i++) cb(u, l[i].name, l[i].dir, l[i].size);
    mem_free(l);
    return true;
}
static int s_read(void *ctx, const char *path, u64 off, void *buf, u32 n) {
    UNUSED(ctx);
    if (!rf || !str_eq(rf_path, path)) {
        if (rf) pal_fclose(rf);
        char p[200]; abs_path(path, p, sizeof p); u64 sz;
        rf = pal_fopen(app_boot_vol(), p, &sz);
        if (!rf) return -1;
        str_copy(rf_path, path, sizeof rf_path);
    }
    if (!pal_fseek(rf, off)) return -1;
    u32 got = 0;
    while (got < n) { int k = pal_fread(rf, (u8 *)buf + got, (int)(n - got)); if (k <= 0) break; got += (u32)k; }
    return (int)got;
}
/* remover: grava no pendrive pelo caminho relativo ("DOCUMENTS/computer-.../a.md"), criando as pastas */
static bool s_save(void *ctx, const char *path, const void *data, u32 n) {
    UNUSED(ctx);
    char p[400]; abs_path(path, p, sizeof p);
    for (int i = 1; p[i]; i++) if (p[i] == '/') { p[i] = 0; pal_mkdir(app_boot_vol(), p); p[i] = '/'; }
    return pal_write(app_boot_vol(), p, data, n);
}
static bool s_backup(void *ctx, const char *name, const void *data, u32 n) {
    UNUSED(ctx);
    paths_ensure(app_boot_vol(), DIR_FOLIO);
    char d[200], p[260]; fmt(d, sizeof d, "%s/BACKUP", paths_dir(DIR_FOLIO));
    pal_mkdir(app_boot_vol(), d);
    fmt(p, sizeof p, "%s/%s", d, name);
    str_copy(backup_name, p, sizeof backup_name);
    return pal_write(app_boot_vol(), p, data, n);
}
static u64 last_draw;
static void s_progress(void *ctx, int percent, const char *what) {
    UNUSED(ctx);
    pct = percent; str_copy(step, what, sizeof step);
    u64 now = pal_ms();
    if (now - last_draw < 120 && percent < 100) return;
    last_draw = now;
    app_frame();                                     /* a janela mostra o progresso; nenhuma tecla é lida até o fim */
    pal_present(rect(0, 0, pal_screen()->w, pal_screen()->h));
}

/* o programa desta arquitetura em EFI/BOOT (vai para \EFI\FolioOS ao lado de outro sistema) */
static const char *efi_name(void) {
    const char *n = pal_name();
    if (str_find_i(n, str_len(n), "ARM64", 5, 0) >= 0) return "BOOTAA64.EFI";
    if (str_find_i(n, str_len(n), "IA32", 4, 0) >= 0) return "BOOTIA32.EFI";
    return "BOOTX64.EFI";
}

/* chamado no começo de cada quadro: a instalação roda fora do desenho da janela */
void installer_poll(void) {
    if (!run_pending || running) return;
    run_pending = false; running = true;
    app_flush_all(); prefs_save();                  /* os textos abertos vão para o pendrive antes da cópia */
    BlkDev b; InstallSrc s; InstallOpts o;
    memset(&s, 0, sizeof s); memset(&o, 0, sizeof o);
    int d = opt[sel].d;
    if (!disk_dev(dk[d], &b)) { res.code = INST_ERR_WRITE; str_copy(res.path, "", sizeof res.path); st = ST_FAIL; running = false; return; }
    DateTime t; pal_now(&t);
    fmt(s.date, sizeof s.date, "%04d-%02d-%02d-%02d%02d", t.year, t.month, t.day, t.hour, t.min);
    s.list = s_list; s.read = s_read; s.backup = s_backup; s.progress = s_progress; s.save = s_save;
    s.seed = pal_ms() * 0x9E3779B97F4A7C15ull ^ ((u64)t.sec << 32 | (u64)t.min << 40 | (u64)t.day << 48);
    s.bufsize = 2u << 20; s.buf = mem_alloc(s.bufsize);   /* cabe o programa inteiro (a cópia para a partição EFI vai de uma vez) */
    o.bios = is_bios();                              /* a ponte BIOS: MBR + código de boot; senão GPT/UEFI */
    o.docs = INST_DOCS_DISK; o.label = "FOLIO"; o.efi_name = efi_name(); o.version = FOLIO_VERSION;
    o.docs = docs_choice == 1 ? INST_DOCS_STICK : docs_choice == 2 ? INST_DOCS_BOTH : INST_DOCS_DISK;
    backup_name[0] = 0; entry_ok = false;
    if (!s.buf) { res.code = INST_ERR_SOURCE; str_copy(res.path, "memory", sizeof res.path); }
    else if (opt[sel].mode == MODE_ALONGSIDE) install_alongside(&b, &s, &o, opt[sel].fr, &res);
    else if (opt[sel].mode == MODE_SHRINK) {
        u64 fol = (u64)fol_mb << 20, size = opt[sel].pcount * b.bs;
        install_shrink_alongside(&b, &s, &o, opt[sel].pstart, (size - fol) / b.bs, &res);
    }
    else if (opt[sel].mode == MODE_REMOVE) install_remove(&b, &s, &o, &found[d], copy_texts, true, &res);
    else if (opt[sel].mode == MODE_UPDATE) install_update(&b, &s, &o, &found[d], &res);
    else install_whole_disk(&b, &s, &o, &res);
    if (rf) { pal_fclose(rf); rf = 0; }
    mem_free(s.buf);
    /* entrada de boot no firmware (UEFI): ao lado é ela que faz o FolioOS iniciar primeiro; ao remover, ela sai */
    if (res.code == INST_OK && opt[sel].mode == MODE_REMOVE) entry_ok = pal_boot_entry_remove() || o.bios;   /* PC só BIOS: não há entrada */
    else if (res.code == INST_OK && opt[sel].mode == MODE_UPDATE) entry_ok = true;   /* a entrada de boot continua a mesma */
    else if (res.code == INST_OK && !o.bios)
        entry_ok = pal_boot_entry_add("FolioOS", res.esp_num, res.esp_lba, res.esp_count, res.esp_guid, res.boot_path);
    running = false;
    st = res.code == INST_OK ? ST_DONE : ST_FAIL;
    ui.redraw = true;
}

/* ------------------------------------------------------------ janela */
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
/* a palavra digitada confere? (maiúsculas/minúsculas não importam, em qualquer escrita; espaços nas pontas também não) */
static bool word_ok(const char *typed, const char *word) {
    while (*typed == ' ') typed++;
    int n = str_len(typed); while (n > 0 && typed[n - 1] == ' ') n--;
    int m = str_len(word);
    if (!n || !m) return false;
    return str_find_i(typed, n, word, m, 0) == 0 && str_find_i(word, m, typed, n, 0) == 0;   /* uma contém a outra no início = iguais */
}
static void free_text(int i, char *o, int cap) { char sz[24]; disk_size_text(opt[i].fr.count * di[opt[i].d].bs, sz, sizeof sz); str_copy(o, sz, cap); }

/* "Seus textos:" — o rótulo e uma linha de explicação; devolve onde vai o seletor (chamado à parte: a ordem do
 * teclado segue a ordem das chamadas, e no disco inteiro o seletor vem depois dos botões) */
static Rect docs_row(Face *f, int x, float *y, int wd) {
    *y += S(10);
    draw_text(f, S(13.5f), x, *y + S(14), L("Your texts:"), -1, T->panel_text);
    Rect sr = rect(x, (int)(*y + S(22)), wd, SI(36));
    const char *h = docs_choice == 1 ? L("They stay on this USB drive and go with you; FolioOS on the computer uses them when the drive is connected.")
                  : docs_choice == 2 ? L("They are copied to the computer; a button in Settings exchanges them with this USB drive.")
                  : L("They are copied to the computer.");
    draw_text_ellipsis(f, S(12), x, *y + S(22) + S(36) + S(16), wd, h, col_alpha(T->panel_text, 0.55f));
    *y += S(22) + S(36) + S(24);
    return sr;
}
static void installer_body(Window *w, Rect r) {
    Face *f = ui_font(false), *fb = ui_font(true);
    int x = r.x + SI(22), wd = r.w - SI(44);
    float y = r.y + S(18);
    Color dim = col_alpha(T->panel_text, 0.62f);
    int by = r.y + r.h - SI(56);                          /* linha dos botões */
    char line[240], sz[24];
    bool along = sel >= 0 && (opt[sel].mode == MODE_ALONGSIDE || opt[sel].mode == MODE_SHRINK);
    bool removing = sel >= 0 && opt[sel].mode == MODE_REMOVE, updating = sel >= 0 && opt[sel].mode == MODE_UPDATE;

    if (st == ST_CHOOSE) {
        y = wrap(f, S(14), x, y, wd, L("Install FolioOS next to the system you already have, in the free space, or as the only system on a disk."), T->panel_text);
        y = wrap(f, S(13), x, y + S(4), wd, L("Choose where:"), dim) + S(4);
        if (!nopt) wrap(f, S(14), x, y + S(10), wd, L("No disk was found besides the USB drive FolioOS started from."), T->panel_text);
        for (int i = 0; i < nopt; i++) {
            int d = opt[i].d; bool al = opt[i].mode != MODE_WHOLE;
            float hgt = S(64) + S(19) * (float)n_on[d] + (why[d][0] ? S(20) : 0);
            Rect it = rect(x, (int)y, wd, (int)hgt);
            bool fire = ui_item(it), on = i == sel;
            if (on) gfx_rrect(it, S(10), col_alpha(T->accent, 0.14f));
            else if (ui_hover(it)) gfx_rrect(it, S(10), col_alpha(T->text, 0.06f));
            gfx_rrect_line(it, S(10), on ? S(2) : S(1), on ? T->accent : col_alpha(T->border, 0.9f));
            icon_draw(on ? IC_CHECK : IC_DRIVE, it.x + S(22), it.y + S(22), S(16), on ? T->accent : dim);
            if (opt[i].mode == MODE_UPDATE) {   /* de qual versão para qual (a do pendrive); igual = reinstalar o programa */
                const char *iv = found[d].version[0] ? found[d].version : "?";
                if (str_eq(iv, FOLIO_VERSION)) str_copy(line, L("Reinstall the FolioOS program on this disk (your texts and settings stay)"), sizeof line);
                else fmt(line, sizeof line, L("Update FolioOS on this disk (%s → %s)"), iv, FOLIO_VERSION);
            }
            else             if (opt[i].mode == MODE_REMOVE) str_copy(line, found[d].kind == FOUND_WHOLE ? L("Remove FolioOS from this disk (the disk is left empty)") : found[d].ntfs >= 0 ? L("Remove FolioOS from this disk (Windows gets the space back)") : L("Remove FolioOS from this disk (its space becomes free)"), sizeof line);
            else if (opt[i].mode == MODE_SHRINK) { disk_size_text(opt[i].maxfol, sz, sizeof sz); fmt(line, sizeof line, L("Next to the other system, making room in its NTFS partition (up to %s)"), sz); }
            else if (al) { free_text(i, sz, sizeof sz); fmt(line, sizeof line, L("Next to the other system, in %s of free space"), sz); }
            else str_copy(line, L("The whole disk (everything on it is erased)"), sizeof line);
            draw_text_ellipsis(fb, S(14.5f), it.x + S(44), it.y + S(27), it.w - S(56), line, why[d][0] ? dim : (al ? T->panel_text : 0xFFC0392B));
            disk_title(d, line, sizeof line);
            draw_text_ellipsis(f, S(13), it.x + S(44), it.y + S(48), it.w - S(56), line, T->panel_text);
            float ly = it.y + S(66);
            for (int k = 0; k < n_on[d]; k++) { draw_text_ellipsis(f, S(12.5f), it.x + S(56), ly + S(10), it.w - S(68), what_on[d][k], dim); ly += S(19); }
            if (why[d][0]) draw_text_ellipsis(f, S(12.5f), it.x + S(44), ly + S(12), it.w - S(56), why[d], 0xFFC0392B);
            if (fire) { if (why[d][0]) ui_toast(why[d]); else sel = i; }
            y += hgt + S(10);
            if (!al && hint[d][0]) y = wrap(f, S(13), x + S(12), y - S(2), wd - S(24), hint[d], T->accent) + S(10);   /* como manter o outro sistema */
        }
        if (ui_button(rect(x, by, SI(140), SI(40)), L("Cancel"), BTN_GHOST)) wm_close(w);
        if (ui_button(rect(x + wd - SI(180), by, SI(180), SI(40)), L("Next"), sel >= 0 ? BTN_PRIMARY : BTN_SUBTLE) && sel >= 0) {
            st = ST_CONFIRM; confirm[0] = 0; has_key = false; copy_texts = true; docs_choice = 0;
            if (opt[sel].mode == MODE_SHRINK) fol_mb = MIN(SHRINK_DEFAULT_MB, (float)(opt[sel].maxfol >> 20));
            if (opt[sel].mode == MODE_WHOLE) {
                w->nav = 0; w->nav_kind = NAV_TEXT;           /* o campo da palavra é o 1º item: recebe o teclado já */
                ui.focus = confirm; ui.focus_caret = 0; ui.focus_sel = 0;
            } else { w->nav = -1; ui.focus = 0; }
        }
        return;
    }
    if (st == ST_CONFIRM && updating) {
        int d = opt[sel].d; InstallFound *fo = &found[d];
        y = wrap(fb, S(15), x, y, wd, L("FolioOS on this disk will be updated:"), T->panel_text) + S(2);
        disk_title(d, line, sizeof line);
        y = wrap(fb, S(14), x + S(12), y, wd - S(12), line, T->panel_text);
        fmt(line, sizeof line, L("Version %s → %s"), fo->version[0] ? fo->version : "?", FOLIO_VERSION);
        y = wrap(fb, S(13), x + S(24), y, wd - S(24), line, T->accent) + S(8);
        y = wrap(f, S(13.5f), x, y, wd, L("Only the program, fonts, languages, themes and keyboards are replaced. Your texts, music and settings stay as they are, and fonts or themes you added are kept."), T->panel_text);
        y = wrap(f, S(13.5f), x, y + S(4), wd, L("Each file is replaced only after the new copy has been checked, so a power cut never leaves FolioOS half updated."), T->panel_text);
        fmt(line, sizeof line, L("Before anything is written, the start of the disk (partition table and boot sectors) is saved to %s on this USB drive."), "FOLIO/BACKUP");
        wrap(f, S(13.5f), x, y + S(4), wd, line, dim);
        if (ui_button(rect(x, by, SI(140), SI(40)), L("Back"), BTN_GHOST)) { st = ST_CHOOSE; ui.focus = 0; w->nav = -1; }
        if (ui_button(rect(x + wd - SI(220), by, SI(220), SI(40)), L("Update"), BTN_PRIMARY)) { st = ST_RUN; pct = 0; step[0] = 0; run_pending = true; ui.focus = 0; w->nav = -1; }
        return;
    }
    if (st == ST_CONFIRM && removing) {
        /* remover: o antes → depois, os textos para o pendrive (ligado; desligado pede a palavra, eles se perderiam) */
        int d = opt[sel].d; InstallFound *fo = &found[d];
        y = wrap(fb, S(15), x, y, wd, L("FolioOS will be removed from this disk:"), T->panel_text) + S(2);
        disk_title(d, line, sizeof line);
        y = wrap(fb, S(14), x + S(12), y, wd - S(12), line, T->panel_text);
        disk_size_text(fo->part_count * di[d].bs, sz, sizeof sz);
        fmt(line, sizeof line, L("− FolioOS · FAT32 · %s"), sz);
        y = wrap(fb, S(13), x + S(24), y, wd - S(24), line, 0xFFC0392B);
        if (fo->ntfs >= 0) {
            char a[24], b2[24];
            disk_size_text(fo->ntfs_count * di[d].bs, a, sizeof a); disk_size_text(fo->ntfs_grow_to * di[d].bs, b2, sizeof b2);
            fmt(line, sizeof line, L("NTFS%s: %s → %s"), "", a, b2);
            y = wrap(fb, S(13), x + S(24), y, wd - S(24), line, T->accent);
        }
        y += S(8);
        if (fo->docs_files) {
            disk_size_text(fo->docs_bytes, sz, sizeof sz);
            fmt(line, sizeof line, LN("Copy the %d text from this computer to the USB drive first (%s)", "Copy the %d texts from this computer to the USB drive first (%s)", fo->docs_files), fo->docs_files, sz);
            ui_toggle(rect(x, (int)y, wd, SI(36)), line, &copy_texts);
            y += S(44);
        } else y = wrap(f, S(13.5f), x, y, wd, L("There are no texts on FolioOS's partition."), dim) + S(4);
        if (fo->kind == FOUND_WHOLE) y = wrap(fb, S(13.5f), x, y, wd, L("The disk will be left empty, with no partitions, ready for another system."), 0xFFC0392B);
        else y = wrap(f, S(13.5f), x, y, wd, L("The EFI system partition loses only the folder EFI/FolioOS, and the computer starts the other system directly again."), T->panel_text);
        if (fo->ntfs >= 0) y = wrap(f, S(13.5f), x, y + S(4), wd, L("The next time Windows starts, it checks its partition (chkdsk) once; this is expected."), T->panel_text);
        fmt(line, sizeof line, L("Before anything is written, the start of the disk (partition table and boot sectors) is saved to %s on this USB drive."), "FOLIO/BACKUP");
        y = wrap(f, S(13.5f), x, y + S(4), wd, line, dim) + S(8);
        const char *word = L("ERASE");
        bool whole = fo->kind == FOUND_WHOLE;                   /* o disco inteiro fica vazio: sempre a palavra */
        bool need_word = whole || (fo->docs_files && !copy_texts), ok = !need_word;
        if (need_word) {   /* sem a cópia, os textos do computador se perdem; ou o disco inteiro fica vazio: a palavra */
            fmt(line, sizeof line, fo->docs_files && !copy_texts ? L("Your texts on this computer will be lost. To confirm, type %s:") : L("To confirm, type %s:"), word);
            y = wrap(fb, S(14), x, y, wd, line, 0xFFC0392B) + S(4);
            ui_textfield(rect(x, (int)y, SI(260), SI(40)), confirm, sizeof confirm, word, false);
            ok = word_ok(confirm, word);
        }
        if (ui_button(rect(x, by, SI(140), SI(40)), L("Back"), BTN_GHOST)) { st = ST_CHOOSE; ui.focus = 0; w->nav = -1; }
        if (ui_button(rect(x + wd - SI(220), by, SI(220), SI(40)), L("Remove FolioOS"), ok ? BTN_DANGER : BTN_SUBTLE)) {
            if (ok) { st = ST_RUN; pct = 0; step[0] = 0; run_pending = true; ui.focus = 0; w->nav = -1; }
            else { fmt(line, sizeof line, L("To confirm, type %s:"), word); ui_toast(line); }
        }
        return;
    }
    if (st == ST_CONFIRM && along) {
        /* antes e depois, em palavras: nada é apagado */
        int d = opt[sel].d;
        y = wrap(fb, S(15), x, y, wd, L("Nothing on this disk will be erased."), T->panel_text) + S(2);
        disk_title(d, line, sizeof line);
        y = wrap(fb, S(14), x + S(12), y, wd - S(12), line, T->panel_text);
        for (int k = 0; k < n_on[d]; k++) y = wrap(f, S(13), x + S(24), y, wd - S(24), what_on[d][k], dim);
        bool shr = opt[sel].mode == MODE_SHRINK;
        if (shr) {
            /* o antes → depois do outro sistema, que muda junto com o tamanho escolhido (←/→) */
            float hi = (float)(opt[sel].maxfol >> 20), lo = (float)(ALONGSIDE_MIN_BYTES >> 20);
            fol_mb = CLAMP((float)(int)fol_mb, lo, hi);
            u64 fol = (u64)fol_mb << 20, size = opt[sel].pcount * di[d].bs;
            char a[24], b2[24], lab[48] = "";
            disk_size_text(size, a, sizeof a); disk_size_text((size - fol) / (1u << 20) * (1u << 20), b2, sizeof b2);
            if (opt[sel].plabel[0]) fmt(lab, sizeof lab, " \"%s\"", opt[sel].plabel);
            fmt(line, sizeof line, L("NTFS%s: %s → %s"), lab, a, b2);
            y = wrap(fb, S(13), x + S(24), y, wd - S(24), line, T->accent);
            disk_size_text(fol, sz, sizeof sz);
            fmt(line, sizeof line, L("+ FolioOS · FAT32 · %s, in the space this frees"), sz);
            y = wrap(fb, S(13), x + S(24), y, wd - S(24), line, T->accent) + S(6);
            ui_slider(rect(x, (int)y, wd, SI(52)), &fol_mb, lo, hi, L("Size for FolioOS"), sz);
            y += S(56);
            y = wrap(f, S(13.5f), x, y, wd, L("No file is moved: FolioOS only takes free space from the end of that partition. The next time Windows starts, it checks the partition (chkdsk) once; this is expected."), T->panel_text);
        } else {
            free_text(sel, sz, sizeof sz);
            fmt(line, sizeof line, L("+ FolioOS · FAT32 · %s, in the free space"), sz);
            y = wrap(fb, S(13), x + S(24), y, wd - S(24), line, T->accent) + S(8);
            y = wrap(f, S(13.5f), x, y, wd, L("The EFI system partition only receives the folder EFI/FolioOS. Your texts, fonts and settings are copied from this USB drive to the new partition."), T->panel_text);
        }
        fmt(line, sizeof line, LN("When the computer starts, FolioOS waits %d second for a key; without one, the other system starts as usual.",
                                  "When the computer starts, FolioOS waits %d seconds for a key; without one, the other system starts as usual.", prefs.boot_wait), prefs.boot_wait);
        y = wrap(f, S(13.5f), x, y + S(4), wd, line, T->panel_text);
        fmt(line, sizeof line, L("Before anything is written, the start of the disk (partition table and boot sectors) is saved to %s on this USB drive."), "FOLIO/BACKUP");
        y = wrap(f, S(13.5f), x, y + S(4), wd, line, dim);
        /* BitLocker: o FolioOS não toca na partição, mas a mudança na ordem de boot pode fazer o Windows pedir a
         * chave de recuperação uma vez; sem a chave a pessoa perde o acesso aos próprios arquivos */
        bool need_key = bitlocker[d];
        if (need_key) {
            y = wrap(fb, S(13.5f), x, y + S(10), wd, L("This disk has a partition encrypted with BitLocker. FolioOS does not touch it, but after the startup settings change, Windows may ask once for the BitLocker recovery key. Make sure you have it (it is in your Microsoft account at aka.ms/myrecoverykey, or where you saved or printed it) before you continue."), 0xFFC0392B);
            ui_toggle(rect(x, (int)y + SI(6), MIN(wd, SI(460)), SI(34)), L("I have the BitLocker recovery key"), &has_key);
            y += SI(6) + SI(34);
        }
        { Rect sr = docs_row(f, x, &y, wd); const char *it3[] = { L("On this computer"), L("On the USB drive"), L("On both") }; ui_segmented(sr, it3, 3, &docs_choice); }
        bool go = !need_key || has_key;
        if (ui_button(rect(x, by, SI(140), SI(40)), L("Back"), BTN_GHOST)) { st = ST_CHOOSE; ui.focus = 0; w->nav = -1; }
        if (ui_button(rect(x + wd - SI(220), by, SI(220), SI(40)), shr ? L("Shrink and install") : L("Install next to it"), go ? BTN_PRIMARY : BTN_SUBTLE)) {
            if (go) { st = ST_RUN; pct = 0; step[0] = 0; run_pending = true; ui.focus = 0; w->nav = -1; }
            else ui_toast(L("I have the BitLocker recovery key"));
        }
        return;
    }
    if (st == ST_CONFIRM) {
        const char *word = L("ERASE");                    /* a palavra de confirmação (cada idioma escolhe a sua) */
        int d = opt[sel].d;
        y = wrap(fb, S(15), x, y, wd, L("Everything on this disk will be erased:"), T->panel_text) + S(2);
        disk_title(d, line, sizeof line);
        y = wrap(fb, S(14), x + S(12), y, wd - S(12), line, 0xFFC0392B);
        for (int k = 0; k < n_on[d]; k++) y = wrap(f, S(13), x + S(24), y, wd - S(24), what_on[d][k], dim);
        y += S(8);
        y = wrap(f, S(13.5f), x, y, wd, L("FolioOS gets one partition that fills the disk, with your texts, fonts and settings copied from this USB drive."), T->panel_text);
        fmt(line, sizeof line, L("Before anything is written, the start of the disk (partition table and boot sectors) is saved to %s on this USB drive."), "FOLIO/BACKUP");
        y = wrap(f, S(13.5f), x, y + S(4), wd, line, T->panel_text) + S(10);
        fmt(line, sizeof line, L("To confirm, type %s:"), word);
        y = wrap(fb, S(14), x, y, wd, line, T->panel_text) + S(4);
        ui_textfield(rect(x, (int)y, SI(260), SI(40)), confirm, sizeof confirm, word, false);
        bool ok = word_ok(confirm, word);
        y += S(52); Rect sr = docs_row(f, x, &y, wd);
        if (ui_button(rect(x, by, SI(140), SI(40)), L("Back"), BTN_GHOST)) { st = ST_CHOOSE; ui.focus = 0; w->nav = -1; }
        if (ui_button(rect(x + wd - SI(220), by, SI(220), SI(40)), L("Erase and install"), ok ? BTN_DANGER : BTN_SUBTLE)) {
            if (ok) { st = ST_RUN; pct = 0; step[0] = 0; run_pending = true; ui.focus = 0; w->nav = -1; }
            else { fmt(line, sizeof line, L("To confirm, type %s:"), word); ui_toast(line); }
        }
        { const char *it3[] = { L("On this computer"), L("On the USB drive"), L("On both") }; ui_segmented(sr, it3, 3, &docs_choice); }   /* depois dos botões no teclado */
        return;
    }
    if (st == ST_RUN) {
        y = wrap(fb, S(15), x, y + S(20), wd, removing ? L("Removing FolioOS… Do not turn off the computer or remove the USB drive.") : updating ? L("Updating FolioOS… Do not turn off the computer or remove the USB drive.") : L("Installing FolioOS… Do not turn off the computer or remove the USB drive."), T->panel_text) + S(16);
        Rect bar = rect(x, (int)y, wd, SI(14));
        gfx_rrect(bar, S(7), col_alpha(T->text, 0.10f));
        gfx_rrect(rect(bar.x, bar.y, MAX(SI(14), bar.w * CLAMP(pct, 0, 100) / 100), bar.h), S(7), T->accent);
        fmt(line, sizeof line, "%d%%", pct);
        draw_text(fb, S(13), x, y + S(36), line, -1, T->panel_text);
        draw_text_ellipsis(f, S(12.5f), x + S(56), y + S(36), wd - S(56), step, dim);
        return;
    }
    if (st == ST_DONE) {
        icon_draw(IC_CHECK, x + S(16), y + S(30), S(26), T->accent);
        if (updating) {
            y = wrap(fb, S(16), x + S(44), y + S(18), wd - S(44), L("FolioOS on this disk is up to date."), T->panel_text) + S(10);
            y = wrap(f, S(14), x, y, wd, L("Remove the USB drive and restart. Your texts and settings are where they were."), T->panel_text);
        } else         if (removing) {
            y = wrap(fb, S(16), x + S(44), y + S(18), wd - S(44), L("FolioOS was removed from this computer."), T->panel_text) + S(10);
            if (copy_texts && found[opt[sel].d].docs_files) { fmt(line, sizeof line, L("The texts from the computer are on this USB drive, in %s."), res.path); y = wrap(f, S(14), x, y, wd, line, T->panel_text) + S(4); }
            y = wrap(f, S(14), x, y, wd, found[opt[sel].d].kind == FOUND_WHOLE ? L("The disk is empty now: install another system on it, or FolioOS again from this USB drive.") : L("Remove the USB drive and restart: the computer starts the other system as before."), T->panel_text);
            if (!entry_ok) y = wrap(fb, S(13.5f), x, y + S(8), wd, L("The firmware did not let FolioOS remove its boot entry; if the computer still lists FolioOS at startup, remove it in the firmware settings."), 0xFFC0392B);
        } else if (along) {
            y = wrap(fb, S(16), x + S(44), y + S(18), wd - S(44), L("FolioOS is installed next to the other system."), T->panel_text) + S(10);
            y = wrap(f, S(14), x, y, wd, L("Remove the USB drive and restart. FolioOS starts first and waits a few seconds for a key; without one, the other system starts as usual."), T->panel_text);
        } else {
            y = wrap(fb, S(16), x + S(44), y + S(18), wd - S(44), L("FolioOS is installed on this computer."), T->panel_text) + S(10);
            y = wrap(f, S(14), x, y, wd, L("Remove the USB drive and restart. From now on the computer starts straight into FolioOS; your texts are in DOCUMENTS on the disk."), T->panel_text);
        }
        if (along && !entry_ok) y = wrap(fb, S(13.5f), x, y + S(8), wd, L("The firmware did not accept a new boot entry: choose FolioOS in the computer's boot menu (often F12, F8 or Esc right after power-on)."), 0xFFC0392B);
        if (backup_name[0]) { fmt(line, sizeof line, L("The previous start of the disk is saved in %s."), backup_name); wrap(f, S(12.5f), x, y + S(8), wd, line, dim); }
        if (ui_button(rect(x, by, SI(140), SI(40)), L("Close"), BTN_GHOST)) wm_close(w);
        if (ui_button(rect(x + wd - SI(180), by, SI(180), SI(40)), L("Restart"), BTN_PRIMARY)) { app_flush_all(); prefs_save(); pal_power(1); }
        return;
    }
    /* ST_FAIL */
    y = wrap(fb, S(16), x, y + S(18), wd, removing ? L("The removal did not finish.") : updating ? L("The update did not finish. FolioOS on the disk still works: every file is either the old or the new one.") : L("The installation did not finish."), 0xFFC0392B) + S(8);
    fmt(line, sizeof line, res.path[0] ? "%s: %s" : "%s", L(install_error(res.code)), res.path);
    y = wrap(f, S(14), x, y, wd, line, T->panel_text) + S(8);
    if (backup_name[0]) { fmt(line, sizeof line, L("The previous start of the disk is saved in %s."), backup_name); y = wrap(f, S(13), x, y, wd, line, dim); }
    wrap(f, S(13), x, y + S(6), wd, L("Your texts on this USB drive were not touched. You can try again, or keep using FolioOS from the USB drive."), dim);
    if (ui_button(rect(x, by, SI(140), SI(40)), L("Close"), BTN_GHOST)) wm_close(w);
    if (ui_button(rect(x + wd - SI(180), by, SI(180), SI(40)), L("Try again"), BTN_GHOST)) installer_open();
}
