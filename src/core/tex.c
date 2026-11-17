/* FOLIO v4 — LaTeX: fontes .tex/.bib/.sty/.cls editadas como texto puro, com realce por linha */
#include "folio.h"

int doc_syntax_from_path(const char *p) {
    const char *e = path_ext(p);
    return (str_ieq(e, ".tex") || str_ieq(e, ".bib") || str_ieq(e, ".sty") || str_ieq(e, ".cls")) ? SYN_TEX : SYN_MD;
}
bool path_editable(const char *p) {
    int f = format_from_path(p);
    return f == FMT_MD || f == FMT_TXT || doc_syntax_from_path(p) == SYN_TEX;
}

static bool tx_letter(u8 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '@'; }   /* @: \makeatletter (.sty/.cls) */
static void tx_math(u8 *sty, int a, int b) { for (int k = a; k < b; k++) sty[k] |= ST_TEX | TX_MATH; }

/* \comando (letras, * opcional) ou \símbolo (\% \\ \{ \$); { } [ ]; % comentário até o fim da linha;
 * matemática $..$, $$..$$, \(..\) e \[..\] fechada na mesma linha, delimitadores inclusive (aberta: sem marca);
 * @tipo no início da linha (.bib). Delimitadores são ASCII: nunca parte um caractere UTF-8. */
void tex_line(const char *s, int len, u8 *sty) {
    memset(sty, 0, (usize)len);
    int i = 0, m0 = -1; char close = 0;      /* matemática aberta: início do delimitador e fecho esperado ('$', 'D' = $$, ')' ou ']') */
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < len && s[i] == '@') {
        int j = i + 1; while (j < len && tx_letter((u8)s[j])) j++;
        if (j > i + 1) { memset(sty + i, ST_TEX | TX_CMD, (usize)(j - i)); i = j; }
    }
    while (i < len) {
        u8 c = (u8)s[i], st = 0;
        int n = 1;
        if (c == '\\') {
            st = ST_TEX | TX_CMD;
            if (i + 1 < len && tx_letter((u8)s[i + 1])) { n = 2; while (i + n < len && tx_letter((u8)s[i + n])) n++; if (i + n < len && s[i + n] == '*') n++; }
            else if (i + 1 < len && (u8)s[i + 1] < 0x80) {
                n = 2;
                char d = s[i + 1];
                if (m0 < 0 && (d == '(' || d == '[')) { m0 = i; close = d == '(' ? ')' : ']'; }
                else if (m0 >= 0 && d == close) { memset(sty + i, st, 2); tx_math(sty, m0, i + 2); m0 = -1; st = 0; }
            }
        } else if (c == '%') { memset(sty + i, ST_TEX | TX_COMMENT, (usize)(len - i)); break; }
        else if (c == '{' || c == '}' || c == '[' || c == ']') st = ST_TEX | TX_BRACE;
        else if (c == '$') {
            bool dbl = i + 1 < len && s[i + 1] == '$';
            if (m0 < 0) { if (dbl) n = 2; m0 = i; close = dbl ? 'D' : '$'; }
            else if (close == '$') { tx_math(sty, m0, i + 1); m0 = -1; }
            else if (close == 'D' && dbl) { n = 2; tx_math(sty, m0, i + 2); m0 = -1; }
        }
        if (st) memset(sty + i, st, (usize)n);
        i += n;
    }
}
