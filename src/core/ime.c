/* FolioOS — métodos de entrada para escritas que não cabem num teclado
 *
 * Coreano (hangul, teclado de 2 conjuntos / dubeolsik): cada tecla é um jamo; o IME monta a
 * sílaba (inicial + vogal + final) e a troca no texto a cada tecla. Backspace desfaz a última
 * peça da sílaba, como nos sistemas coreanos.
 *
 * Japonês (romaji → kana): as letras ficam no texto até formarem uma sílaba da tabela (ka → か,
 * kya → きゃ, kk → っk, nn → ん); Ctrl+Espaço alterna hiragana → katakana → letras latinas.
 * A conversão para kanji precisa de dicionário (ainda não incluído).
 *
 * O IME não conhece o editor: escreve por um ImeTarget (apagar N caracteres antes do cursor e
 * inserir texto), usado pelo editor e pelos campos de texto. O layout ativo liga o IME com a
 * linha "ime hangul" ou "ime kana" (arquivos em FOLIO/KEYBOARDS). */
#include "folio.h"

static int  kind;          /* IME_* do layout */
static int  mode;          /* 0 = latino; hangul: 1; kana: 1 hiragana, 2 katakana */
static int  shown;         /* caracteres da composição atual no texto */
static ImeTarget last_tg;   /* último alvo: para fechar a composição ao trocar de modo (Ctrl+Espaço) */
static void *last_ctx;     /* o alvo mudou (editor ↔ campo): a composição anterior fica como está */

/* ------------------------------------------------------------ comum */
static void put(ImeTarget *t, int del, const u32 *cps, int n) {
    char buf[64]; int len = 0;
    for (int i = 0; i < n && len < (int)sizeof buf - 4; i++) len += utf8_encode(cps[i], buf + len);
    if (del > 0) t->del_back(del, t->ctx);
    if (len > 0) t->insert(buf, len, t->ctx);
}

/* ------------------------------------------------------------ hangul */
/* tecla US → jamo de compatibilidade (U+3131…); maiúsculas dão as consoantes duplas */
static u32 hangul_key(u32 c) {
    static const char *lower = "qwertyuiopasdfghjklzxcvbnm";
    static const u16 jl[26] = { 0x3142, 0x3148, 0x3137, 0x3131, 0x3145, 0x315B, 0x3155, 0x3151, 0x3150, 0x3154,
                                0x3141, 0x3134, 0x3147, 0x3139, 0x314E, 0x3157, 0x3153, 0x314F, 0x3163,
                                0x314B, 0x314C, 0x314A, 0x314D, 0x3160, 0x315C, 0x3161 };
    switch (c) { case 'Q': return 0x3143; case 'W': return 0x3149; case 'E': return 0x3138; case 'R': return 0x3132;
                 case 'T': return 0x3146; case 'O': return 0x3152; case 'P': return 0x3156; default: break; }
    if (c >= 'A' && c <= 'Z') c += 32;
    for (int i = 0; i < 26; i++) if ((u32)lower[i] == c) return jl[i];
    return 0;
}
/* consoante de compatibilidade (0x3131..0x314E) → índice inicial (L) e final (T) */
static const i8 L_of[30] = { 0, 1, -1, 2, -1, -1, 3, 4, 5, -1, -1, -1, -1, -1, -1, -1, 6, 7, 8, -1, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18 };
static const i8 T_of[30] = { 1, 2, 3, 4, 5, 6, 7, 0, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 0, 18, 19, 20, 21, 22, 0, 23, 24, 25, 26, 27 };
static const u16 L_cp[19] = { 0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143, 0x3145, 0x3146,
                              0x3147, 0x3148, 0x3149, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E };
static int vowel_join(int a, int b) {
    static const i8 t[][3] = { { 8, 0, 9 }, { 8, 1, 10 }, { 8, 20, 11 }, { 13, 4, 14 }, { 13, 5, 15 }, { 13, 20, 16 }, { 18, 20, 19 } };
    for (int i = 0; i < COUNT(t); i++) if (t[i][0] == a && t[i][1] == b) return t[i][2];
    return -1;
}
/* final composta: (final, consoante seguinte como final) → final composta; e a volta */
static const i8 T_join[][3] = { { 1, 19, 3 }, { 4, 22, 5 }, { 4, 27, 6 }, { 8, 1, 9 }, { 8, 16, 10 }, { 8, 17, 11 }, { 8, 19, 12 },
                                 { 8, 25, 13 }, { 8, 26, 14 }, { 8, 27, 15 }, { 17, 19, 18 } };
static int final_join(int t, int c) { for (int i = 0; i < COUNT(T_join); i++) if (T_join[i][0] == t && T_join[i][1] == c) return T_join[i][2]; return -1; }
static bool final_split(int t, int *a, int *b) { for (int i = 0; i < COUNT(T_join); i++) if (T_join[i][2] == t) { *a = T_join[i][0]; *b = T_join[i][1]; return true; } return false; }
static int L_of_T(int t) {    /* final simples → mesma consoante como inicial */
    for (int i = 0; i < 30; i++) if (T_of[i] == t && L_of[i] >= 0) return L_of[i];
    return -1;
}

typedef struct { i8 l, v, t; } Syl;      /* -1 = vazio; t = 0 sem final */
static Syl syl, hist[8]; static int nhist;

static int syl_render(Syl s, u32 *out) {
    if (s.l >= 0 && s.v >= 0) { out[0] = 0xAC00 + ((u32)(s.l * 21 + s.v) * 28) + (u32)s.t; return 1; }
    if (s.l >= 0) { out[0] = L_cp[s.l]; return 1; }
    if (s.v >= 0) { out[0] = 0x314F + (u32)s.v; return 1; }
    return 0;
}
static void syl_clear(void) { syl.l = syl.v = -1; syl.t = 0; nhist = 0; }
/* mostra a sílaba nova no lugar da que estava; com "fecha", a anterior fica e começa outra */
static void hangul_show(ImeTarget *t, const Syl *done) {
    u32 out[4]; int n = 0;
    if (done) n += syl_render(*done, out);
    int k = syl_render(syl, out + n); n += k;
    put(t, shown, out, n);
    shown = k;
}
static void hangul_key_jamo(u32 j, ImeTarget *tg) {
    Syl before = syl;
    if (nhist < COUNT(hist)) hist[nhist++] = syl;
    if (j >= 0x314F) {                                        /* vogal */
        int v = (int)(j - 0x314F);
        if (syl.l < 0 && syl.v < 0) { syl.v = (i8)v; hangul_show(tg, 0); return; }
        if (syl.v < 0) { syl.v = (i8)v; hangul_show(tg, 0); return; }
        if (syl.t == 0) {
            int c = vowel_join(syl.v, v);
            if (c >= 0) { syl.v = (i8)c; hangul_show(tg, 0); return; }
            Syl done = syl; syl_clear(); syl.v = (i8)v; hist[nhist++] = (Syl){ -1, -1, 0 };
            put(tg, shown, 0, 0); shown = 0; hangul_show(tg, &done); return;
        }
        /* a final passa para a próxima sílaba: 각 + ㅏ = 가가 */
        int a, b, nl;
        Syl done = syl;
        if (final_split(syl.t, &a, &b)) { done.t = (i8)a; nl = L_of_T(b); }
        else { done.t = 0; nl = L_of_T(syl.t); }
        syl_clear(); syl.l = (i8)nl; syl.v = (i8)v;
        hist[nhist++] = (Syl){ (i8)nl, -1, 0 };
        put(tg, shown, 0, 0); shown = 0; hangul_show(tg, &done);
        return;
    }
    int ci = (int)(j - 0x3131), L = L_of[ci], T = T_of[ci];   /* consoante */
    if (syl.l >= 0 && syl.v >= 0) {
        if (syl.t == 0 && T > 0) { syl.t = (i8)T; hangul_show(tg, 0); return; }
        if (syl.t > 0) { int c = final_join(syl.t, T); if (c > 0) { syl.t = (i8)c; hangul_show(tg, 0); return; } }
    }
    if (syl.l < 0 && syl.v < 0 && L >= 0) { syl.l = (i8)L; hangul_show(tg, 0); return; }
    /* fecha a sílaba e começa outra com esta consoante */
    Syl done = before; syl_clear(); syl.l = (i8)L; hist[nhist++] = (Syl){ -1, -1, 0 };
    put(tg, shown, 0, 0); shown = 0; hangul_show(tg, &done);
}

/* ------------------------------------------------------------ kana */
static const struct { const char *r, *k; } KANA[] = {
    { "a", "あ" }, { "i", "い" }, { "u", "う" }, { "e", "え" }, { "o", "お" },
    { "ka", "か" }, { "ki", "き" }, { "ku", "く" }, { "ke", "け" }, { "ko", "こ" },
    { "sa", "さ" }, { "shi", "し" }, { "si", "し" }, { "su", "す" }, { "se", "せ" }, { "so", "そ" },
    { "ta", "た" }, { "chi", "ち" }, { "ti", "ち" }, { "tsu", "つ" }, { "tu", "つ" }, { "te", "て" }, { "to", "と" },
    { "na", "な" }, { "ni", "に" }, { "nu", "ぬ" }, { "ne", "ね" }, { "no", "の" },
    { "ha", "は" }, { "hi", "ひ" }, { "fu", "ふ" }, { "hu", "ふ" }, { "he", "へ" }, { "ho", "ほ" },
    { "ma", "ま" }, { "mi", "み" }, { "mu", "む" }, { "me", "め" }, { "mo", "も" },
    { "ya", "や" }, { "yu", "ゆ" }, { "yo", "よ" },
    { "ra", "ら" }, { "ri", "り" }, { "ru", "る" }, { "re", "れ" }, { "ro", "ろ" },
    { "wa", "わ" }, { "wo", "を" }, { "nn", "ん" }, { "n'", "ん" },
    { "ga", "が" }, { "gi", "ぎ" }, { "gu", "ぐ" }, { "ge", "げ" }, { "go", "ご" },
    { "za", "ざ" }, { "ji", "じ" }, { "zi", "じ" }, { "zu", "ず" }, { "ze", "ぜ" }, { "zo", "ぞ" },
    { "da", "だ" }, { "di", "ぢ" }, { "du", "づ" }, { "de", "で" }, { "do", "ど" },
    { "ba", "ば" }, { "bi", "び" }, { "bu", "ぶ" }, { "be", "べ" }, { "bo", "ぼ" },
    { "pa", "ぱ" }, { "pi", "ぴ" }, { "pu", "ぷ" }, { "pe", "ぺ" }, { "po", "ぽ" },
    { "kya", "きゃ" }, { "kyu", "きゅ" }, { "kyo", "きょ" },
    { "sha", "しゃ" }, { "shu", "しゅ" }, { "sho", "しょ" }, { "she", "しぇ" }, { "sya", "しゃ" }, { "syu", "しゅ" }, { "syo", "しょ" },
    { "cha", "ちゃ" }, { "chu", "ちゅ" }, { "cho", "ちょ" }, { "che", "ちぇ" }, { "tya", "ちゃ" }, { "tyu", "ちゅ" }, { "tyo", "ちょ" },
    { "nya", "にゃ" }, { "nyu", "にゅ" }, { "nyo", "にょ" },
    { "hya", "ひゃ" }, { "hyu", "ひゅ" }, { "hyo", "ひょ" },
    { "mya", "みゃ" }, { "myu", "みゅ" }, { "myo", "みょ" },
    { "rya", "りゃ" }, { "ryu", "りゅ" }, { "ryo", "りょ" },
    { "gya", "ぎゃ" }, { "gyu", "ぎゅ" }, { "gyo", "ぎょ" },
    { "ja", "じゃ" }, { "ju", "じゅ" }, { "jo", "じょ" }, { "je", "じぇ" }, { "jya", "じゃ" }, { "jyu", "じゅ" }, { "jyo", "じょ" },
    { "zya", "じゃ" }, { "zyu", "じゅ" }, { "zyo", "じょ" },
    { "bya", "びゃ" }, { "byu", "びゅ" }, { "byo", "びょ" },
    { "pya", "ぴゃ" }, { "pyu", "ぴゅ" }, { "pyo", "ぴょ" },
    { "fa", "ふぁ" }, { "fi", "ふぃ" }, { "fe", "ふぇ" }, { "fo", "ふぉ" },
    { "xa", "ぁ" }, { "xi", "ぃ" }, { "xu", "ぅ" }, { "xe", "ぇ" }, { "xo", "ぉ" },
    { "la", "ぁ" }, { "li", "ぃ" }, { "lu", "ぅ" }, { "le", "ぇ" }, { "lo", "ぉ" },
    { "xya", "ゃ" }, { "xyu", "ゅ" }, { "xyo", "ょ" }, { "xtu", "っ" }, { "ltu", "っ" },
};
static char rbuf[8]; static int rlen;          /* letras ainda sem sílaba (estão no texto) */
static int kana_find(const char *s, int n, bool *prefix) {
    *prefix = false; int hit = -1;
    for (int i = 0; i < COUNT(KANA); i++) {
        const char *r = KANA[i].r; int rl = str_len(r);
        if (rl == n && !memcmp(r, s, (usize)n)) hit = i;
        else if (rl > n && !memcmp(r, s, (usize)n)) *prefix = true;
    }
    return hit;
}
/* hiragana → katakana: mesmo desenho, bloco 0x60 adiante */
static int kana_out(const char *k, u32 *out, int cap) {
    int n = 0, len = str_len(k);
    for (int i = 0; i < len && n < cap; ) {
        u32 cp; i += utf8_decode(k, len, i, &cp);
        if (mode == 2 && cp >= 0x3041 && cp <= 0x3096) cp += 0x60;
        out[n++] = cp;
    }
    return n;
}
static bool is_vowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }
/* resolve o que der no buffer; o resto continua como letras */
static void kana_run(ImeTarget *t, bool flush) {
    u32 out[24]; int n = 0;
    while (rlen > 0) {
        bool pre; int h = kana_find(rbuf, rlen, &pre);
        if (h >= 0 && !(pre && !flush && str_eq(KANA[h].r, "n"))) {
            n += kana_out(KANA[h].k, out + n, 24 - n);
            rlen = 0; break;
        }
        if (rlen >= 2 && rbuf[0] == rbuf[1] && rbuf[0] != 'n' && !is_vowel(rbuf[0])) {
            n += kana_out("っ", out + n, 24 - n); memmove(rbuf, rbuf + 1, (usize)--rlen); continue;
        }
        if (rbuf[0] == 'n' && (rlen >= 2 ? (!is_vowel(rbuf[1]) && rbuf[1] != 'y' && rbuf[1] != 'n' && rbuf[1] != '\'') : flush)) {
            n += kana_out("ん", out + n, 24 - n); memmove(rbuf, rbuf + 1, (usize)--rlen); continue;
        }
        if (pre && !flush) break;                                   /* ainda pode virar sílaba */
        out[n++] = (u32)(u8)rbuf[0]; memmove(rbuf, rbuf + 1, (usize)--rlen);   /* não forma nada: fica a letra */
    }
    for (int i = 0; i < rlen && n < 24; i++) out[n++] = (u32)(u8)rbuf[i];
    put(t, shown, out, n);
    shown = rlen;
}

/* ------------------------------------------------------------ interface */
void ime_setup(void) {
    kind = kbd_ime(); mode = kind ? 1 : 0; shown = 0; rlen = 0; syl_clear();
}
bool ime_on(void) { return kind != IME_NONE && mode != 0; }
bool ime_composing(void) { return ime_on() && shown > 0; }
void ime_toggle(void) {
    if (!kind) { ui_toast(L("This keyboard has no input method (choose one in Settings › System)")); return; }
    if (ime_on() && last_tg.ctx) ime_commit(&last_tg);   /* o "n" pendente vira ん antes de trocar */
    shown = 0; rlen = 0; syl_clear();
    mode = kind == IME_KANA ? (mode + 1) % 3 : !mode;
    const char *m = !mode ? L("Input: Latin letters") : kind == IME_HANGUL ? L("Input: Hangul") : mode == 1 ? L("Input: Hiragana") : L("Input: Katakana");
    ui_toast(m);
    ui.redraw = true;
}
const char *ime_badge(void) {
    if (!kind) return "";
    if (!mode) return "A";
    return kind == IME_HANGUL ? "\xEA\xB0\x80" : mode == 1 ? "\xE3\x81\x82" : "\xE3\x82\xA2";   /* 가 あ ア */
}

static void same_target(ImeTarget *t) { if (t->ctx != last_ctx) { shown = 0; rlen = 0; syl_clear(); last_ctx = t->ctx; } last_tg = *t; }
bool ime_feed(u32 ch, ImeTarget *t) {
    if (!ime_on()) return false;
    same_target(t);
    if (kind == IME_HANGUL) {
        u32 j = ch < 128 ? hangul_key(ch) : 0;
        if (!j) { ime_commit(t); return false; }
        hangul_key_jamo(j, t);
        return true;
    }
    /* kana */
    char c = ch < 128 ? (char)ch : 0;
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if ((c >= 'a' && c <= 'z') || (c == '\'' && rlen > 0)) {
        if (rlen >= (int)sizeof rbuf - 1) kana_run(t, true);
        rbuf[rlen++] = c;
        kana_run(t, false);
        return true;
    }
    const char *p = c == '-' ? "ー" : c == ',' ? "、" : c == '.' ? "。" : c == '[' ? "「" : c == ']' ? "」" : 0;
    ime_commit(t);
    if (!p) return false;
    u32 out[2]; int n = kana_out(p, out, 2);
    put(t, 0, out, n);
    return true;
}
bool ime_backspace(ImeTarget *t) {
    if (!ime_on()) return false;
    same_target(t);
    if (!shown) return false;
    if (kind == IME_HANGUL) {
        if (nhist > 0) syl = hist[--nhist]; else syl_clear();
        hangul_show(t, 0);
        if (!shown) syl_clear();
        return true;
    }
    if (rlen > 0) { rlen--; put(t, shown, 0, 0); u32 out[8]; for (int i = 0; i < rlen; i++) out[i] = (u32)(u8)rbuf[i]; put(t, 0, out, rlen); shown = rlen; }
    return true;
}
void ime_commit(ImeTarget *t) {
    if (!ime_on()) return;
    same_target(t);
    if (kind == IME_KANA && rlen > 0) kana_run(t, true);
    shown = 0; rlen = 0; syl_clear();
}
