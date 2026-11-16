/* FOLIO v4 — exportação: MD, TXT, HTML, RTF, DOCX, ODT, EPUB (+ ZIP "stored") */
#include "folio.h"

static const FormatInfo formats[FMT_COUNT] = {
    { ".md",   N_("Markdown"),      N_("Lightly marked-up text — the native format"), true },
    { ".txt",  N_("Plain text"),    N_("Simple text, opens in any editor"), true },
    { ".html", N_("Web page"),      N_("HTML with typography ready for the browser"), false },
    { ".rtf",  N_("Rich Text"),     N_("Opens in WordPad, Word, Pages, LibreOffice"), false },
    { ".docx", N_("Word"),          N_("Microsoft Word / Google Docs"), false },
    { ".odt",  N_("OpenDocument"),  N_("LibreOffice Writer"), false },
    { ".epub", N_("E-book"),        N_("E-readers (EPUB 3, chapters by #)"), false },
};
const FormatInfo *format_info(int f) { return &formats[CLAMP(f, 0, FMT_COUNT - 1)]; }
int format_from_path(const char *p) {
    const char *e = path_ext(p);
    for (int i = 0; i < FMT_COUNT; i++) if (str_ieq(e, formats[i].ext)) return i;
    if (str_ieq(e, ".markdown") || str_ieq(e, ".mdown")) return FMT_MD;
    if (str_ieq(e, ".text") || str_ieq(e, ".fountain")) return FMT_TXT;
    return -1;
}

/* ---------------------------------------------------------------- ZIP */
static u32 crc_tab[256];
u32 crc32(const void *data, usize len) {
    if (!crc_tab[1]) for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; }
    u32 c = 0xFFFFFFFFu; const u8 *p = data;
    for (usize i = 0; i < len; i++) c = crc_tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void w16(Buf *b, u32 v) { u8 t[2] = { (u8)v, (u8)(v >> 8) }; buf_put(b, t, 2); }
static void w32(Buf *b, u32 v) { u8 t[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) }; buf_put(b, t, 4); }

/* marcas de idioma nos arquivos exportados: @LANG@ (pt-BR), @L1@ (pt), @C1@ (BR ou none), @TOC@ (sumário) */
static void lang_fill(Buf *o, const char *s, usize len) {
    const char *code = i18n_code();
    char l1[8] = "", c1[8] = "none"; int k = 0;
    while (code[k] && code[k] != '-' && k < 7) { l1[k] = code[k]; k++; } l1[k] = 0;
    if (code[k] == '-') str_copy(c1, code + k + 1, sizeof c1);
    for (usize i = 0; i < len; ) {
        if (s[i] == '@') {
            const char *rep = 0; usize n = 0;
            if (i + 6 <= len && memcmp(s + i, "@LANG@", 6) == 0) { rep = code; n = 6; }
            else if (i + 4 <= len && memcmp(s + i, "@L1@", 4) == 0) { rep = l1; n = 4; }
            else if (i + 4 <= len && memcmp(s + i, "@C1@", 4) == 0) { rep = c1; n = 4; }
            else if (i + 5 <= len && memcmp(s + i, "@TOC@", 5) == 0) { n = 5; Buf x = {0}; buf_xml(&x, L("Table of contents"), -1); buf_put(o, x.s, x.len); buf_free(&x); i += n; continue; }
            if (rep) { buf_puts(o, rep); i += n; continue; }
        }
        buf_put(o, s + i, 1); i++;
    }
}

void zip_begin(Zip *z) { memset(z, 0, sizeof *z); }
void zip_add(Zip *z, const char *name, const void *data, usize len) {
    Buf filled = {0}; lang_fill(&filled, (const char *)data, len); data = filled.s ? filled.s : ""; len = filled.len;
    Buf b = { (char *)z->buf, z->len, z->cap }, c = { (char *)z->cdir, z->clen, z->ccap };
    DateTime t; pal_now(&t);
    u32 dtime = (u32)((t.hour << 11) | (t.min << 5) | (t.sec / 2));
    u32 ddate = (u32)(((t.year - 1980) << 9) | (t.month << 5) | t.day);
    u32 crc = crc32(data, len), off = (u32)b.len, nl = (u32)str_len(name);
    w32(&b, 0x04034b50); w16(&b, 10); w16(&b, 0x0800); w16(&b, 0); w16(&b, dtime); w16(&b, ddate);
    w32(&b, crc); w32(&b, (u32)len); w32(&b, (u32)len); w16(&b, nl); w16(&b, 0);
    buf_put(&b, name, nl); buf_put(&b, data, len);
    w32(&c, 0x02014b50); w16(&c, 20); w16(&c, 10); w16(&c, 0x0800); w16(&c, 0); w16(&c, dtime); w16(&c, ddate);
    w32(&c, crc); w32(&c, (u32)len); w32(&c, (u32)len); w16(&c, nl); w16(&c, 0); w16(&c, 0); w16(&c, 0); w16(&c, 0);
    w32(&c, 0); w32(&c, off); buf_put(&c, name, nl);
    z->buf = (u8 *)b.s; z->len = b.len; z->cap = b.cap; z->cdir = (u8 *)c.s; z->clen = c.len; z->ccap = c.cap;
    z->count++;
    buf_free(&filled);
}
void zip_end(Zip *z, u8 **out, usize *len) {
    Buf b = { (char *)z->buf, z->len, z->cap };
    u32 cdoff = (u32)b.len;
    buf_put(&b, z->cdir, z->clen);
    w32(&b, 0x06054b50); w16(&b, 0); w16(&b, 0); w16(&b, (u32)z->count); w16(&b, (u32)z->count);
    w32(&b, (u32)z->clen); w32(&b, cdoff); w16(&b, 0);
    mem_free(z->cdir);
    *out = (u8 *)b.s; *len = b.len;
}

/* ---------------------------------------------------------------- tabela */
static int table_cells(const char *row, int len, const char **cells, int *lens, int max) {
    int i = 0, n = 0;
    while (i < len && row[i] == ' ') i++;
    if (i < len && row[i] == '|') i++;
    while (i < len && n < max) {
        int s = i;
        while (i < len && row[i] != '|') { if (row[i] == '\\' && i + 1 < len) i++; i++; }
        int e = i;
        while (s < e && row[s] == ' ') s++;
        while (e > s && row[e - 1] == ' ') e--;
        if (i >= len && s == e) break;
        cells[n] = row + s; lens[n] = e - s; n++;
        if (i < len) i++;
    }
    return n;
}
static bool table_sep(const char *row, int len) {
    bool dash = false;
    for (int i = 0; i < len; i++) { char c = row[i]; if (c == '-') dash = true; else if (c != '|' && c != ':' && c != ' ') return false; }
    return dash;
}
typedef void (*RowFn)(void *ctx, const char **cells, int *lens, int n, bool header);
static void table_each(MdBlock *b, void *ctx, RowFn fn) {
    int i = 0, row = 0;
    while (i < b->len) {
        int s = i; while (i < b->len && b->text[i] != '\n') i++;
        const char *r = b->text + s; int rl = i - s; i++;
        if (table_sep(r, rl)) continue;
        const char *cells[32]; int lens[32];
        int n = table_cells(r, rl, cells, lens, 32);
        fn(ctx, cells, lens, n, row == 0);
        row++;
    }
}

/* ---------------------------------------------------------------- HTML / XHTML */
static void html_inline(Buf *o, const char *s, int len) {
    MdRun runs[512]; int n = md_runs(s, len, runs, 512);
    for (int i = 0; i < n; i++) {
        MdRun *r = &runs[i];
        if (r->sty & ST_LINK) { buf_puts(o, "<a href=\""); buf_xml(o, r->url ? r->url : "#", r->url ? r->url_len : 1); buf_puts(o, "\">"); }
        if (r->sty & ST_CODE) buf_puts(o, "<code>");
        if (r->sty & ST_BOLD) buf_puts(o, "<strong>");
        if (r->sty & ST_ITALIC) buf_puts(o, "<em>");
        if (r->sty & ST_STRIKE) buf_puts(o, "<del>");
        for (int k = 0; k < r->len; k++) {
            if (r->s[k] == '\n') buf_puts(o, "<br/>");
            else buf_xml(o, r->s + k, 1);
        }
        if (r->sty & ST_STRIKE) buf_puts(o, "</del>");
        if (r->sty & ST_ITALIC) buf_puts(o, "</em>");
        if (r->sty & ST_BOLD) buf_puts(o, "</strong>");
        if (r->sty & ST_CODE) buf_puts(o, "</code>");
        if (r->sty & ST_LINK) buf_puts(o, "</a>");
    }
}
static void html_row(void *ctx, const char **c, int *l, int n, bool header) {
    Buf *o = ctx;
    buf_puts(o, "<tr>");
    for (int i = 0; i < n; i++) { buf_puts(o, header ? "<th>" : "<td>"); html_inline(o, c[i], l[i]); buf_puts(o, header ? "</th>" : "</td>"); }
    buf_puts(o, "</tr>\n");
}

static void html_blocks(Buf *o, MdDoc *m, int from, int to) {
    int lstack[8]; int ldepth = 0;   /* tipos de lista abertos (1 ul, 2 ol) */
    for (int i = from; i < to; i++) {
        MdBlock *b = &m->b[i];
        bool is_list = b->kind == MB_ULIST || b->kind == MB_OLIST || b->kind == MB_TASK;
        int want = is_list ? MIN(b->indent + 1, 8) : 0;
        int type = b->kind == MB_OLIST ? 2 : 1;
        while (ldepth > want || (ldepth == want && ldepth > 0 && lstack[ldepth - 1] != type)) {
            buf_puts(o, lstack[--ldepth] == 2 ? "</ol>\n" : "</ul>\n");
        }
        while (ldepth < want) {
            lstack[ldepth++] = type;
            buf_puts(o, type == 2 ? "<ol>\n" : (b->kind == MB_TASK ? "<ul class=\"tasks\">\n" : "<ul>\n"));
        }
        switch (b->kind) {
        case MB_H: buf_printf(o, "<h%d>", b->level); html_inline(o, b->text, b->len); buf_printf(o, "</h%d>\n", b->level); break;
        case MB_PARA: buf_puts(o, "<p>"); html_inline(o, b->text, b->len); buf_puts(o, "</p>\n"); break;
        case MB_QUOTE: buf_puts(o, "<blockquote><p>"); html_inline(o, b->text, b->len); buf_puts(o, "</p></blockquote>\n"); break;
        case MB_HR: buf_puts(o, "<hr/>\n"); break;
        case MB_CODE: buf_puts(o, "<pre><code>"); buf_xml(o, b->text, b->len); buf_puts(o, "</code></pre>\n"); break;
        case MB_ULIST: case MB_OLIST: buf_puts(o, "<li>"); html_inline(o, b->text, b->len); buf_puts(o, "</li>\n"); break;
        case MB_TASK: buf_puts(o, b->checked ? "<li>&#9745; " : "<li>&#9744; "); html_inline(o, b->text, b->len); buf_puts(o, "</li>\n"); break;
        case MB_TABLE: buf_puts(o, "<table>\n"); table_each(b, o, html_row); buf_puts(o, "</table>\n"); break;
        }
    }
    while (ldepth > 0) buf_puts(o, lstack[--ldepth] == 2 ? "</ol>\n" : "</ul>\n");
}

static const char *CSS =
    "body{max-width:40em;margin:4em auto;padding:0 1.25em;font:1.13em/1.7 'Iowan Old Style','Palatino Linotype',Georgia,serif;"
    "color:#2a2620;background:#fbf9f4;text-rendering:optimizeLegibility;hyphens:auto}"
    "h1,h2,h3,h4{font-family:'Avenir Next','Segoe UI',sans-serif;line-height:1.25;margin:1.8em 0 .6em}"
    "h1{font-size:2em}h2{font-size:1.5em}h3{font-size:1.2em}p{margin:0 0 1em}"
    "blockquote{margin:1.5em 0;padding:.2em 1.2em;border-left:3px solid #c9a96e;color:#5c5548;font-style:italic}"
    "code{font-family:'SF Mono',Consolas,monospace;font-size:.88em;background:#f0ece2;padding:.1em .3em;border-radius:3px}"
    "pre{background:#f0ece2;padding:1em 1.2em;border-radius:6px;overflow:auto}pre code{background:none;padding:0}"
    "hr{border:0;text-align:center;margin:2.5em 0}hr:after{content:'\\2042';color:#a99}"
    "a{color:#9a4a1c}table{border-collapse:collapse;margin:1.5em 0}td,th{border:1px solid #ddd5c5;padding:.35em .7em}"
    "th{background:#f0ece2}ul.tasks{list-style:none;padding-left:1.2em}\n";

static void export_html(MdDoc *m, Buf *o) {
    buf_puts(o, "<!DOCTYPE html>\n<html lang=\"@LANG@\">\n<head>\n<meta charset=\"utf-8\">\n"
                "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                "<meta name=\"generator\" content=\"FOLIO " FOLIO_VERSION "\">\n<title>");
    buf_xml(o, m->title, -1);
    buf_puts(o, "</title>\n<style>"); buf_puts(o, CSS); buf_puts(o, "</style>\n</head>\n<body>\n<article>\n");
    html_blocks(o, m, 0, m->n);
    buf_puts(o, "</article>\n</body>\n</html>\n");
}

/* ---------------------------------------------------------------- TXT */
static void export_txt(MdDoc *m, Buf *o) {
    for (int i = 0; i < m->n; i++) {
        MdBlock *b = &m->b[i];
        int pl; char *p = b->kind == MB_CODE ? str_ndup(b->text, b->len) : md_plain(b->text, b->len, &pl);
        if (b->kind == MB_CODE) pl = b->len;
        for (int k = 0; k < b->indent; k++) buf_puts(o, "   ");
        switch (b->kind) {
        case MB_HR: buf_puts(o, "                    * * *\n\n"); break;
        case MB_ULIST: buf_puts(o, "• "); buf_put(o, p, (usize)pl); buf_puts(o, "\n"); break;
        case MB_TASK: buf_puts(o, b->checked ? "[x] " : "[ ] "); buf_put(o, p, (usize)pl); buf_puts(o, "\n"); break;
        case MB_OLIST: buf_printf(o, "%d. ", b->number); buf_put(o, p, (usize)pl); buf_puts(o, "\n"); break;
        case MB_QUOTE: buf_puts(o, "    "); buf_put(o, p, (usize)pl); buf_puts(o, "\n\n"); break;
        case MB_H:
            buf_put(o, p, (usize)pl); buf_puts(o, "\n");
            if (b->level <= 2) { int n = 0; for (int k = 0; k < pl; k++) if (((u8)p[k] & 0xC0) != 0x80) n++; for (int k = 0; k < n; k++) buf_puts(o, b->level == 1 ? "=" : "-"); buf_puts(o, "\n"); }
            buf_puts(o, "\n"); break;
        default: buf_put(o, p, (usize)pl); buf_puts(o, "\n\n"); break;
        }
        bool next_list = i + 1 < m->n && (m->b[i + 1].kind == MB_ULIST || m->b[i + 1].kind == MB_OLIST || m->b[i + 1].kind == MB_TASK);
        if ((b->kind == MB_ULIST || b->kind == MB_OLIST || b->kind == MB_TASK) && !next_list) buf_puts(o, "\n");
        mem_free(p);
    }
}

/* ---------------------------------------------------------------- RTF */
static void rtf_text(Buf *o, const char *s, int len) {
    for (int i = 0; i < len; ) {
        u32 c; i += utf8_decode(s, len, i, &c);
        if (c == '\\' || c == '{' || c == '}') { char t[2] = { '\\', (char)c }; buf_put(o, t, 2); }
        else if (c == '\n') buf_puts(o, "\\line ");
        else if (c == '\t') buf_puts(o, "\\tab ");
        else if (c < 128) { char t = (char)c; buf_put(o, &t, 1); }
        else if (c < 0x10000) buf_printf(o, "\\u%d?", (int)(i16)c);
        else { c -= 0x10000; buf_printf(o, "\\u%d?\\u%d?", (int)(i16)(0xD800 + (c >> 10)), (int)(i16)(0xDC00 + (c & 0x3FF))); }
    }
}
static void rtf_inline(Buf *o, const char *s, int len) {
    MdRun runs[512]; int n = md_runs(s, len, runs, 512);
    for (int i = 0; i < n; i++) {
        MdRun *r = &runs[i];
        buf_puts(o, "{");
        if (r->sty & ST_BOLD) buf_puts(o, "\\b");
        if (r->sty & ST_ITALIC) buf_puts(o, "\\i");
        if (r->sty & ST_STRIKE) buf_puts(o, "\\strike");
        if (r->sty & ST_CODE) buf_puts(o, "\\f1\\fs20");
        if (r->sty & ST_LINK) buf_puts(o, "\\ul\\cf2");
        buf_puts(o, " ");
        rtf_text(o, r->s, r->len);
        buf_puts(o, "}");
    }
}
static void rtf_row(void *ctx, const char **c, int *l, int n, bool header) {
    Buf *o = ctx;
    buf_puts(o, "\\trowd\\trgaph108");
    for (int i = 0; i < n; i++) buf_printf(o, "\\clbrdrt\\brdrs\\clbrdrl\\brdrs\\clbrdrb\\brdrs\\clbrdrr\\brdrs\\cellx%d", 2200 * (i + 1));
    buf_puts(o, "\n");
    for (int i = 0; i < n; i++) { buf_puts(o, "\\pard\\intbl"); if (header) buf_puts(o, "\\b"); buf_puts(o, " "); rtf_inline(o, c[i], l[i]); buf_puts(o, header ? "\\b0\\cell\n" : "\\cell\n"); }
    buf_puts(o, "\\row\n");
}
static void export_rtf(MdDoc *m, Buf *o) {
    buf_puts(o, "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\froman Georgia;}{\\f1\\fmodern Courier New;}{\\f2\\fswiss Arial;}}\n"
                "{\\colortbl;\\red90\\green84\\blue74;\\red26\\green95\\blue180;}\n"
                "{\\info{\\title ");
    rtf_text(o, m->title, str_len(m->title));
    buf_puts(o, "}{\\doccomm FOLIO " FOLIO_VERSION "}}\n\\paperw11906\\paperh16838\\margl1701\\margr1701\\margt1417\\margb1417\n\\f0\\fs24\n");
    for (int i = 0; i < m->n; i++) {
        MdBlock *b = &m->b[i];
        switch (b->kind) {
        case MB_H: { int fs = b->level == 1 ? 40 : b->level == 2 ? 32 : b->level == 3 ? 28 : 24;
            buf_printf(o, "\\pard\\keepn\\sb360\\sa180\\outlinelevel%d\\f2\\b\\fs%d ", b->level - 1, fs); rtf_inline(o, b->text, b->len); buf_puts(o, "\\b0\\f0\\fs24\\par\n"); break; }
        case MB_PARA: buf_puts(o, "\\pard\\sa200\\sl360\\slmult1\\fi0 "); rtf_inline(o, b->text, b->len); buf_puts(o, "\\par\n"); break;
        case MB_QUOTE: buf_puts(o, "\\pard\\li720\\ri720\\sa200\\sl360\\slmult1\\i\\cf1 "); rtf_inline(o, b->text, b->len); buf_puts(o, "\\i0\\cf0\\par\n"); break;
        case MB_HR: buf_puts(o, "\\pard\\qc\\sb240\\sa240 *\\~\\~*\\~\\~*\\par\n"); break;
        case MB_CODE: buf_puts(o, "\\pard\\li360\\sa200\\f1\\fs20 "); rtf_text(o, b->text, b->len); buf_puts(o, "\\f0\\fs24\\par\n"); break;
        case MB_ULIST: case MB_TASK: case MB_OLIST: {
            int li = 360 + 360 * b->indent;
            buf_printf(o, "\\pard\\li%d\\fi-280\\sa80 ", li);
            if (b->kind == MB_OLIST) buf_printf(o, "%d.\\tab ", b->number);
            else if (b->kind == MB_TASK) buf_puts(o, b->checked ? "\\u9745?\\tab " : "\\u9744?\\tab ");
            else buf_puts(o, "\\bullet\\tab ");
            rtf_inline(o, b->text, b->len); buf_puts(o, "\\par\n"); break; }
        case MB_TABLE: table_each(b, o, rtf_row); buf_puts(o, "\\pard\\sa200\\par\n"); break;
        }
    }
    buf_puts(o, "}\n");
}

/* ---------------------------------------------------------------- DOCX */
static void docx_inline(Buf *o, const char *s, int len) {
    MdRun runs[512]; int n = md_runs(s, len, runs, 512);
    for (int i = 0; i < n; i++) {
        MdRun *r = &runs[i];
        buf_puts(o, "<w:r>");
        if (r->sty) {
            buf_puts(o, "<w:rPr>");
            if (r->sty & ST_CODE) buf_puts(o, "<w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" w:cs=\"Consolas\"/>");
            if (r->sty & ST_BOLD) buf_puts(o, "<w:b/>");
            if (r->sty & ST_ITALIC) buf_puts(o, "<w:i/>");
            if (r->sty & ST_STRIKE) buf_puts(o, "<w:strike/>");
            if (r->sty & ST_LINK) buf_puts(o, "<w:color w:val=\"1A5FB4\"/><w:u w:val=\"single\"/>");
            buf_puts(o, "</w:rPr>");
        }
        int st = 0;
        for (int k = 0; k <= r->len; k++) {
            if (k == r->len || r->s[k] == '\n') {
                buf_puts(o, "<w:t xml:space=\"preserve\">"); buf_xml(o, r->s + st, k - st); buf_puts(o, "</w:t>");
                if (k < r->len) buf_puts(o, "<w:br/>");
                st = k + 1;
            }
        }
        buf_puts(o, "</w:r>");
    }
}
static void docx_para(Buf *o, const char *style, const char *extra, const char *s, int len, const char *prefix) {
    buf_puts(o, "<w:p><w:pPr>");
    if (style) buf_printf(o, "<w:pStyle w:val=\"%s\"/>", style);
    if (extra) buf_puts(o, extra);
    buf_puts(o, "</w:pPr>");
    if (prefix) { buf_puts(o, "<w:r><w:t xml:space=\"preserve\">"); buf_puts(o, prefix); buf_puts(o, "</w:t></w:r>"); }
    docx_inline(o, s, len);
    buf_puts(o, "</w:p>\n");
}
typedef struct { Buf *o; int cols; } DocxTab;
static void docx_count(void *ctx, const char **c, int *l, int n, bool h) { UNUSED(c); UNUSED(l); UNUSED(h); DocxTab *t = ctx; if (n > t->cols) t->cols = n; }
static void docx_row(void *ctx, const char **c, int *l, int n, bool header) {
    DocxTab *t = ctx; Buf *o = t->o;
    buf_puts(o, "<w:tr>");
    for (int i = 0; i < t->cols; i++) {
        buf_puts(o, "<w:tc><w:tcPr><w:tcW w:w=\"2400\" w:type=\"dxa\"/></w:tcPr><w:p>");
        if (header) buf_puts(o, "<w:pPr><w:rPr><w:b/></w:rPr></w:pPr>");
        if (i < n) docx_inline(o, c[i], l[i]);
        buf_puts(o, "</w:p></w:tc>");
    }
    buf_puts(o, "</w:tr>\n");
}

static void export_docx(MdDoc *m, u8 **out, usize *len) {
    Buf d = {0};
    buf_puts(&d, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                 "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>\n");
    for (int i = 0; i < m->n; i++) {
        MdBlock *b = &m->b[i];
        char ind[96];
        switch (b->kind) {
        case MB_H: { char st[16]; fmt(st, sizeof st, "Heading%d", MIN(b->level, 4)); docx_para(&d, st, 0, b->text, b->len, 0); break; }
        case MB_PARA: docx_para(&d, 0, 0, b->text, b->len, 0); break;
        case MB_QUOTE: docx_para(&d, "Quote", 0, b->text, b->len, 0); break;
        case MB_HR: buf_puts(&d, "<w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:r><w:t>*   *   *</w:t></w:r></w:p>\n"); break;
        case MB_CODE: {
            int s = 0;
            for (int k = 0; k <= b->len; k++) if (k == b->len || b->text[k] == '\n') {
                buf_puts(&d, "<w:p><w:pPr><w:pStyle w:val=\"Code\"/></w:pPr><w:r><w:t xml:space=\"preserve\">");
                buf_xml(&d, b->text + s, k - s); buf_puts(&d, "</w:t></w:r></w:p>\n"); s = k + 1;
            }
            break; }
        case MB_ULIST: case MB_OLIST: case MB_TASK: {
            fmt(ind, sizeof ind, "<w:ind w:left=\"%d\" w:hanging=\"360\"/><w:spacing w:after=\"60\"/>", 720 + 360 * b->indent);
            char pre[24];
            if (b->kind == MB_OLIST) fmt(pre, sizeof pre, "%d.\t", b->number);
            else if (b->kind == MB_TASK) str_copy(pre, b->checked ? "\xE2\x98\x91\t" : "\xE2\x98\x90\t", sizeof pre);
            else str_copy(pre, "\xE2\x80\xA2\t", sizeof pre);
            docx_para(&d, 0, ind, b->text, b->len, pre); break; }
        case MB_TABLE: {
            DocxTab t = { &d, 0 }; table_each(b, &t, docx_count);
            buf_puts(&d, "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/><w:tblBorders>"
                         "<w:top w:val=\"single\" w:sz=\"4\" w:color=\"BBBBBB\"/><w:left w:val=\"single\" w:sz=\"4\" w:color=\"BBBBBB\"/>"
                         "<w:bottom w:val=\"single\" w:sz=\"4\" w:color=\"BBBBBB\"/><w:right w:val=\"single\" w:sz=\"4\" w:color=\"BBBBBB\"/>"
                         "<w:insideH w:val=\"single\" w:sz=\"4\" w:color=\"BBBBBB\"/><w:insideV w:val=\"single\" w:sz=\"4\" w:color=\"BBBBBB\"/>"
                         "</w:tblBorders></w:tblPr><w:tblGrid>");
            for (int c = 0; c < t.cols; c++) buf_puts(&d, "<w:gridCol w:w=\"2400\"/>");
            buf_puts(&d, "</w:tblGrid>\n");
            table_each(b, &t, docx_row);
            buf_puts(&d, "</w:tbl><w:p/>\n"); break; }
        }
    }
    buf_puts(&d, "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1417\" w:right=\"1701\" w:bottom=\"1417\" w:left=\"1701\" "
                 "w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>\n");

    static const char *styles =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Georgia\" w:hAnsi=\"Georgia\" w:cs=\"Georgia\"/><w:sz w:val=\"24\"/>"
        "<w:lang w:val=\"@LANG@\"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after=\"160\" w:line=\"360\" w:lineRule=\"auto\"/>"
        "</w:pPr></w:pPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/>"
        "<w:pPr><w:keepNext/><w:spacing w:before=\"480\" w:after=\"240\"/><w:outlineLvl w:val=\"0\"/></w:pPr><w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\"/><w:b/><w:sz w:val=\"40\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\"><w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/>"
        "<w:pPr><w:keepNext/><w:spacing w:before=\"360\" w:after=\"180\"/><w:outlineLvl w:val=\"1\"/></w:pPr><w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\"/><w:b/><w:sz w:val=\"32\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading3\"><w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/>"
        "<w:pPr><w:keepNext/><w:spacing w:before=\"280\" w:after=\"120\"/><w:outlineLvl w:val=\"2\"/></w:pPr><w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\"/><w:b/><w:sz w:val=\"28\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading4\"><w:name w:val=\"heading 4\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/>"
        "<w:pPr><w:keepNext/><w:outlineLvl w:val=\"3\"/></w:pPr><w:rPr><w:b/><w:i/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Quote\"><w:name w:val=\"Quote\"/><w:basedOn w:val=\"Normal\"/><w:qFormat/>"
        "<w:pPr><w:ind w:left=\"720\" w:right=\"720\"/></w:pPr><w:rPr><w:i/><w:color w:val=\"5A544A\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Code\"><w:name w:val=\"Code\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/><w:ind w:left=\"360\"/></w:pPr><w:rPr><w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\"/><w:sz w:val=\"20\"/></w:rPr></w:style>"
        "</w:styles>\n";
    Buf core = {0};
    buf_puts(&core, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
                    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><dc:title>");
    buf_xml(&core, m->title, -1);
    buf_puts(&core, "</dc:title><dc:creator>FolioOS</dc:creator><dc:language>@LANG@</dc:language></cp:coreProperties>\n");

    Zip z; zip_begin(&z);
    const char *ct = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
        "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/></Types>\n";
    const char *rels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/></Relationships>\n";
    const char *drels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/></Relationships>\n";
    zip_add(&z, "[Content_Types].xml", ct, (usize)str_len(ct));
    zip_add(&z, "_rels/.rels", rels, (usize)str_len(rels));
    zip_add(&z, "word/document.xml", d.s, d.len);
    zip_add(&z, "word/styles.xml", styles, (usize)str_len(styles));
    zip_add(&z, "word/_rels/document.xml.rels", drels, (usize)str_len(drels));
    zip_add(&z, "docProps/core.xml", core.s, core.len);
    zip_end(&z, out, len);
    buf_free(&d); buf_free(&core);
}

/* ---------------------------------------------------------------- ODT */
static void odt_inline(Buf *o, const char *s, int len) {
    MdRun runs[512]; int n = md_runs(s, len, runs, 512);
    for (int i = 0; i < n; i++) {
        MdRun *r = &runs[i];
        const char *st = 0;
        if (r->sty & ST_CODE) st = "Tcode";
        else if ((r->sty & ST_BOLD) && (r->sty & ST_ITALIC)) st = "Tbi";
        else if (r->sty & ST_BOLD) st = "Tb";
        else if (r->sty & ST_ITALIC) st = "Ti";
        else if (r->sty & ST_STRIKE) st = "Ts";
        else if (r->sty & ST_LINK) st = "Tl";
        if (st) buf_printf(o, "<text:span text:style-name=\"%s\">", st);
        int a = 0;
        for (int k = 0; k <= r->len; k++) if (k == r->len || r->s[k] == '\n') {
            buf_xml(o, r->s + a, k - a);
            if (k < r->len) buf_puts(o, "<text:line-break/>");
            a = k + 1;
        }
        if (st) buf_puts(o, "</text:span>");
    }
}
typedef struct { Buf *o; int cols; } OdtTab;
static void odt_count(void *ctx, const char **c, int *l, int n, bool h) { UNUSED(c); UNUSED(l); UNUSED(h); OdtTab *t = ctx; if (n > t->cols) t->cols = n; }
static void odt_row(void *ctx, const char **c, int *l, int n, bool header) {
    OdtTab *t = ctx; Buf *o = t->o;
    buf_puts(o, "<table:table-row>");
    for (int i = 0; i < t->cols; i++) {
        buf_puts(o, "<table:table-cell table:style-name=\"Cell\" office:value-type=\"string\"><text:p");
        buf_puts(o, header ? " text:style-name=\"Th\">" : ">");
        if (i < n) odt_inline(o, c[i], l[i]);
        buf_puts(o, "</text:p></table:table-cell>");
    }
    buf_puts(o, "</table:table-row>\n");
}

static void export_odt(MdDoc *m, u8 **out, usize *len) {
    Buf c = {0};
    buf_puts(&c, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content "
        "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
        "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
        "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" office:version=\"1.2\">\n"
        "<office:automatic-styles>"
        "<style:style style:name=\"Body\" style:family=\"paragraph\"><style:paragraph-properties fo:margin-bottom=\"0.25cm\" fo:line-height=\"150%\"/>"
        "<style:text-properties fo:font-family=\"Georgia\" fo:font-size=\"12pt\" fo:language=\"@L1@\" fo:country=\"@C1@\"/></style:style>"
        "<style:style style:name=\"H1\" style:family=\"paragraph\"><style:paragraph-properties fo:margin-top=\"0.8cm\" fo:margin-bottom=\"0.4cm\" fo:keep-with-next=\"always\"/>"
        "<style:text-properties fo:font-family=\"Arial\" fo:font-size=\"20pt\" fo:font-weight=\"bold\"/></style:style>"
        "<style:style style:name=\"H2\" style:family=\"paragraph\"><style:paragraph-properties fo:margin-top=\"0.6cm\" fo:margin-bottom=\"0.3cm\" fo:keep-with-next=\"always\"/>"
        "<style:text-properties fo:font-family=\"Arial\" fo:font-size=\"16pt\" fo:font-weight=\"bold\"/></style:style>"
        "<style:style style:name=\"H3\" style:family=\"paragraph\"><style:paragraph-properties fo:margin-top=\"0.5cm\" fo:margin-bottom=\"0.2cm\" fo:keep-with-next=\"always\"/>"
        "<style:text-properties fo:font-family=\"Arial\" fo:font-size=\"14pt\" fo:font-weight=\"bold\"/></style:style>"
        "<style:style style:name=\"Quote\" style:family=\"paragraph\"><style:paragraph-properties fo:margin-left=\"1.2cm\" fo:margin-right=\"1.2cm\" fo:margin-bottom=\"0.25cm\" fo:line-height=\"150%\"/>"
        "<style:text-properties fo:font-family=\"Georgia\" fo:font-size=\"12pt\" fo:font-style=\"italic\" fo:color=\"#5a544a\"/></style:style>"
        "<style:style style:name=\"Code\" style:family=\"paragraph\"><style:paragraph-properties fo:margin-left=\"0.6cm\" fo:background-color=\"#f0ece2\"/>"
        "<style:text-properties fo:font-family=\"'Courier New'\" fo:font-size=\"10pt\"/></style:style>"
        "<style:style style:name=\"Center\" style:family=\"paragraph\"><style:paragraph-properties fo:text-align=\"center\" fo:margin-top=\"0.4cm\" fo:margin-bottom=\"0.4cm\"/></style:style>"
        "<style:style style:name=\"Th\" style:family=\"paragraph\"><style:text-properties fo:font-weight=\"bold\"/></style:style>"
        "<style:style style:name=\"Cell\" style:family=\"table-cell\"><style:table-cell-properties fo:border=\"0.5pt solid #bbbbbb\" fo:padding=\"0.1cm\"/></style:style>");
    for (int lv = 0; lv < 6; lv++)
        buf_printf(&c, "<style:style style:name=\"L%d\" style:family=\"paragraph\"><style:paragraph-properties fo:margin-left=\"%d.%dcm\" fo:text-indent=\"-0.5cm\" fo:margin-bottom=\"0.1cm\"/>"
                       "<style:text-properties fo:font-family=\"Georgia\" fo:font-size=\"12pt\"/></style:style>", lv, 1 + (lv * 6) / 10, (lv * 6) % 10);
    buf_puts(&c, "<style:style style:name=\"Tb\" style:family=\"text\"><style:text-properties fo:font-weight=\"bold\"/></style:style>"
        "<style:style style:name=\"Ti\" style:family=\"text\"><style:text-properties fo:font-style=\"italic\"/></style:style>"
        "<style:style style:name=\"Tbi\" style:family=\"text\"><style:text-properties fo:font-weight=\"bold\" fo:font-style=\"italic\"/></style:style>"
        "<style:style style:name=\"Ts\" style:family=\"text\"><style:text-properties style:text-line-through-style=\"solid\"/></style:style>"
        "<style:style style:name=\"Tl\" style:family=\"text\"><style:text-properties style:text-underline-style=\"solid\" fo:color=\"#1a5fb4\"/></style:style>"
        "<style:style style:name=\"Tcode\" style:family=\"text\"><style:text-properties fo:font-family=\"'Courier New'\"/></style:style>"
        "</office:automatic-styles>\n<office:body><office:text>\n");
    for (int i = 0; i < m->n; i++) {
        MdBlock *b = &m->b[i];
        switch (b->kind) {
        case MB_H: buf_printf(&c, "<text:h text:style-name=\"H%d\" text:outline-level=\"%d\">", MIN(b->level, 3), b->level); odt_inline(&c, b->text, b->len); buf_puts(&c, "</text:h>\n"); break;
        case MB_PARA: buf_puts(&c, "<text:p text:style-name=\"Body\">"); odt_inline(&c, b->text, b->len); buf_puts(&c, "</text:p>\n"); break;
        case MB_QUOTE: buf_puts(&c, "<text:p text:style-name=\"Quote\">"); odt_inline(&c, b->text, b->len); buf_puts(&c, "</text:p>\n"); break;
        case MB_HR: buf_puts(&c, "<text:p text:style-name=\"Center\">*   *   *</text:p>\n"); break;
        case MB_CODE: {
            int s = 0;
            for (int k = 0; k <= b->len; k++) if (k == b->len || b->text[k] == '\n') {
                buf_puts(&c, "<text:p text:style-name=\"Code\">");
                for (int q = s; q < k; q++) { if (b->text[q] == ' ') buf_puts(&c, "<text:s/>"); else buf_xml(&c, b->text + q, 1); }
                buf_puts(&c, "</text:p>\n"); s = k + 1;
            }
            break; }
        case MB_ULIST: case MB_OLIST: case MB_TASK:
            buf_printf(&c, "<text:p text:style-name=\"L%d\">", MIN(b->indent, 5));
            if (b->kind == MB_OLIST) buf_printf(&c, "%d.<text:tab/>", b->number);
            else if (b->kind == MB_TASK) buf_puts(&c, b->checked ? "\xE2\x98\x91<text:tab/>" : "\xE2\x98\x90<text:tab/>");
            else buf_puts(&c, "\xE2\x80\xA2<text:tab/>");
            odt_inline(&c, b->text, b->len); buf_puts(&c, "</text:p>\n"); break;
        case MB_TABLE: {
            OdtTab t = { &c, 0 }; table_each(b, &t, odt_count);
            buf_printf(&c, "<table:table table:name=\"Tabela%d\"><table:table-column table:number-columns-repeated=\"%d\"/>\n", i + 1, MAX(t.cols, 1));
            table_each(b, &t, odt_row);
            buf_puts(&c, "</table:table>\n"); break; }
        }
    }
    buf_puts(&c, "</office:text></office:body></office:document-content>\n");
    const char *manifest = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
        "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" manifest:media-type=\"application/vnd.oasis.opendocument.text\"/>"
        "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
        "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/></manifest:manifest>\n";
    Buf meta = {0};
    buf_puts(&meta, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
                    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" office:version=\"1.2\">"
                    "<office:meta><meta:generator>FOLIO " FOLIO_VERSION "</meta:generator><dc:title>");
    buf_xml(&meta, m->title, -1);
    buf_puts(&meta, "</dc:title><dc:language>@LANG@</dc:language></office:meta></office:document-meta>\n");
    Zip z; zip_begin(&z);
    const char *mt = "application/vnd.oasis.opendocument.text";
    zip_add(&z, "mimetype", mt, (usize)str_len(mt));
    zip_add(&z, "META-INF/manifest.xml", manifest, (usize)str_len(manifest));
    zip_add(&z, "content.xml", c.s, c.len);
    zip_add(&z, "meta.xml", meta.s, meta.len);
    zip_end(&z, out, len);
    buf_free(&c); buf_free(&meta);
}

/* ---------------------------------------------------------------- EPUB 3 */
static void export_epub(MdDoc *m, u8 **out, usize *len) {
    int starts[256]; int nch = 0;
    starts[nch++] = 0;
    for (int i = 1; i < m->n && nch < 255; i++) if (m->b[i].kind == MB_H && m->b[i].level == 1) starts[nch++] = i;
    if (nch > 1 && starts[1] == 0) { for (int i = 1; i < nch; i++) starts[i - 1] = starts[i]; nch--; }
    starts[nch] = m->n;
    /* se o primeiro "capítulo" está vazio (doc começa com # ), remove */
    Zip z; zip_begin(&z);
    const char *mt = "application/epub+zip";
    zip_add(&z, "mimetype", mt, (usize)str_len(mt));
    const char *cont = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                       "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n";
    zip_add(&z, "META-INF/container.xml", cont, (usize)str_len(cont));
    zip_add(&z, "OEBPS/style.css", CSS, (usize)str_len(CSS));
    Buf nav = {0}, opf = {0};
    DateTime t; pal_now(&t);
    buf_puts(&opf, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"uid\" xml:lang=\"@LANG@\">\n"
                   "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">");
    buf_printf(&opf, "<dc:identifier id=\"uid\">urn:folio:%04d%02d%02d%02d%02d%02d-%x</dc:identifier><dc:title>", t.year, t.month, t.day, t.hour, t.min, t.sec, rng_next());
    buf_xml(&opf, m->title, -1);
    buf_printf(&opf, "</dc:title><dc:language>@LANG@</dc:language><dc:creator>FolioOS</dc:creator>"
                     "<meta property=\"dcterms:modified\">%04d-%02d-%02dT%02d:%02d:%02dZ</meta></metadata>\n<manifest>\n"
                     "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
                     "<item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n", t.year, t.month, t.day, t.hour, t.min, t.sec);
    buf_puts(&nav, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" xml:lang=\"@LANG@\">"
                   "<head><meta charset=\"utf-8\"/><title>@TOC@</title></head><body><nav epub:type=\"toc\" id=\"toc\"><h1>@TOC@</h1><ol>\n");
    for (int c = 0; c < nch; c++) {
        Buf ch = {0};
        const char *title = m->title; int tl = -1;
        for (int i = starts[c]; i < starts[c + 1]; i++) if (m->b[i].kind == MB_H) { title = m->b[i].text; tl = m->b[i].len; break; }
        int pl; char *pt = md_plain(title, tl < 0 ? str_len(title) : tl, &pl);
        buf_puts(&ch, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"@LANG@\"><head><meta charset=\"utf-8\"/><title>");
        buf_xml(&ch, pt, pl);
        buf_puts(&ch, "</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n<section>\n");
        html_blocks(&ch, m, starts[c], starts[c + 1]);
        buf_puts(&ch, "</section>\n</body></html>\n");
        char name[48]; fmt(name, sizeof name, "OEBPS/cap%03d.xhtml", c + 1);
        zip_add(&z, name, ch.s, ch.len);
        buf_printf(&opf, "<item id=\"c%d\" href=\"cap%03d.xhtml\" media-type=\"application/xhtml+xml\"/>\n", c + 1, c + 1);
        buf_printf(&nav, "<li><a href=\"cap%03d.xhtml\">", c + 1); buf_xml(&nav, pt, pl); buf_puts(&nav, "</a></li>\n");
        mem_free(pt); buf_free(&ch);
    }
    buf_puts(&opf, "</manifest>\n<spine>\n");
    for (int c = 0; c < nch; c++) buf_printf(&opf, "<itemref idref=\"c%d\"/>\n", c + 1);
    buf_puts(&opf, "</spine>\n</package>\n");
    buf_puts(&nav, "</ol></nav></body></html>\n");
    zip_add(&z, "OEBPS/content.opf", opf.s, opf.len);
    zip_add(&z, "OEBPS/nav.xhtml", nav.s, nav.len);
    zip_end(&z, out, len);
    buf_free(&nav); buf_free(&opf);
}

/* ---------------------------------------------------------------- entrada */
bool export_doc(Doc *d, int f, const char *title, u8 **out, usize *len) {
    if (f == FMT_MD || f == FMT_TXT) { *out = (u8 *)doc_text(d, len); return true; }
    MdDoc m; md_parse(d, &m);
    if (title && title[0] && !m.title[0]) { mem_free(m.title); m.title = str_dup(title); }
    Buf o = {0};
    bool ok = true;
    switch (f) {
    case FMT_TXT: export_txt(&m, &o); break;
    case FMT_HTML: { Buf h = {0}; export_html(&m, &h); lang_fill(&o, h.s ? h.s : "", h.len); buf_free(&h); } break;
    case FMT_RTF: export_rtf(&m, &o); break;
    case FMT_DOCX: export_docx(&m, out, len); md_free(&m); return true;
    case FMT_ODT: export_odt(&m, out, len); md_free(&m); return true;
    case FMT_EPUB: export_epub(&m, out, len); md_free(&m); return true;
    default: ok = false;
    }
    md_free(&m);
    if (!o.s) buf_put(&o, "", 0);
    *out = (u8 *)o.s; *len = o.len;
    return ok;
}
