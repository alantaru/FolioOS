/* FolioOS — seletor de boot (fase 2: o FolioOS ao lado de outro sistema).
 * O firmware inicia o FolioOS primeiro. Por alguns segundos (boot_wait= no CONFIG.INI, 3 por padrão) a tela
 * diz "aperte qualquer tecla para escrever"; sem tecla, o outro sistema inicia. Esc inicia o outro na hora.
 * O outro sistema é iniciado pelo firmware (BootNext + reinício), nunca por dentro do FolioOS: o Windows
 * inicia do jeito de sempre, e o BitLocker não vê nada diferente. */
#include "folio.h"

static void draw(const char *other, int secs) {
    Surface *s = pal_screen();
    int W = s->w, H = s->h;
    float sc = MAX(1.0f, (float)MIN(W, H) / 820.0f);
    gfx_target(s);
    gfx_clip(rect(0, 0, W, H));
    gfx_set_opacity(1);
    gfx_rect(rect(0, 0, W, H), T->desk);
    Family *fam = font_ui();
    Face *fb = fam_face(fam, FS_BOLD), *fr = fam_face(fam, FS_REG);
    float cy = H * 0.42f;
    draw_text_c(fb, 40 * sc, rect(0, (int)(cy - 60 * sc), W, (int)(50 * sc)), "FolioOS", T->text);
    draw_text_c(fb, 22 * sc, rect(0, (int)(cy + 4 * sc), W, (int)(32 * sc)), L("Press any key to write"), T->accent);
    char line[200];
    fmt(line, sizeof line, LN("Starting %2$s in %1$d second…", "Starting %2$s in %1$d seconds…", secs), secs, other);
    draw_text_c(fr, 16 * sc, rect(0, (int)(cy + 52 * sc), W, (int)(26 * sc)), line, col_alpha(T->text, 0.75f));
    fmt(line, sizeof line, L("Esc: start %s now"), other);
    draw_text_c(fr, 13 * sc, rect(0, H - (int)(56 * sc), W, (int)(22 * sc)), line, col_alpha(T->text, 0.5f));
    pal_present(rect(0, 0, W, H));
}

/* devolve quando o usuário quer escrever (ou se o outro sistema não puder ser iniciado) */
void boot_chooser(void) {
    char other[96];
    if (prefs.boot_wait <= 0 || !pal_boot_other(other, sizeof other)) return;
    dbg("boot chooser: %s in %d s", other, prefs.boot_wait);
    u64 end = pal_ms() + (u64)prefs.boot_wait * 1000;
    int shown = -1;
    for (;;) {
        u64 now = pal_ms();
        if (now >= end) break;
        int secs = (int)((end - now + 999) / 1000);
        if (secs != shown) { draw(other, secs); shown = secs; }
        Event e;
        while (pal_poll(&e)) {
            if (e.type == EV_KEY && e.key == KEY_ESC) { dbg("boot chooser: Esc, starting the other system"); pal_boot_other_start(); return; }
            if (e.type == EV_KEY || e.type == EV_DOWN) { dbg("boot chooser: key, writing"); return; }   /* qualquer tecla ou clique: escrever */
        }
        pal_wait(20);
    }
    dbg("boot chooser: no key, starting the other system");
    pal_boot_other_start();                                    /* só volta se o firmware não deixar: fica no FolioOS */
}
