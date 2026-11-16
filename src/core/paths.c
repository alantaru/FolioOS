/* FolioOS — nomes das pastas e arquivos no pen-drive
 *
 * Os nomes são neutros (inglês) para o projeto ser universal. Pen-drives antigos, com as pastas
 * em português (TEXTOS, MUSICA, FOLIO/FONTES…), continuam funcionando: se a pasta antiga existe e
 * a nova não, a antiga é usada como está. Nada é renomeado ou movido (os textos do usuário nunca
 * são tocados). */
#include "folio.h"

typedef struct { const char *path; const char *legacy[4]; } PathDef;

static const PathDef defs[PATH_COUNT] = {
    [DIR_FOLIO]     = { "/FOLIO",            { 0 } },
    [DIR_DOCS]      = { "/DOCUMENTS",        { "/TEXTOS", 0 } },
    [DIR_MUSIC]     = { "/MUSIC",            { "/MUSICA", "/MÚSICA", 0 } },
    [DIR_FONTS]     = { "/FOLIO/FONTS",      { "/FOLIO/FONTES", 0 } },
    [DIR_THEMES]    = { "/FOLIO/THEMES",     { "/FOLIO/TEMAS", 0 } },
    [DIR_TABS]      = { "/FOLIO/TABS",       { "/FOLIO/ABAS", 0 } },
    [DIR_VERSIONS]  = { "/FOLIO/VERSIONS",   { "/FOLIO/VERSOES", 0 } },
    [DIR_LANGUAGES] = { "/FOLIO/LANGUAGES",  { 0 } },
    [DIR_KEYBOARDS] = { "/FOLIO/KEYBOARDS",  { 0 } },
    [FILE_CONFIG]   = { "/FOLIO/CONFIG.INI", { 0 } },
    [FILE_DRAFT]    = { "/FOLIO/DRAFT.MD",   { "/FOLIO/RASCUNHO.MD", 0 } },
    [FILE_DIAG]     = { "/FOLIO/DIAGNOSTICS.TXT", { 0 } },
};

static const char *resolved[PATH_COUNT];

/* a pasta antiga, se existir, tem prioridade: num pen-drive em uso ela guarda os textos do
 * usuário, e uma atualização que crie a pasta nova ao lado não pode escondê-los */
void paths_init(int vol) {
    for (int i = 0; i < PATH_COUNT; i++) {
        resolved[i] = defs[i].path;
        bool d;
        for (int k = 0; k < 4 && defs[i].legacy[k]; k++)
            if (pal_exists(vol, defs[i].legacy[k], &d)) { resolved[i] = defs[i].legacy[k]; break; }
    }
}

const char *paths_dir(int id) {
    if (id < 0 || id >= PATH_COUNT) return "/";
    return resolved[id] ? resolved[id] : defs[id].path;
}

/* nome da pasta como o usuário a vê no pen-drive ("FOLIO\THEMES"), para mensagens */
const char *paths_show(int id) {
    static char out[4][64]; static int k;
    char *o = out[k = (k + 1) & 3];
    const char *p = paths_dir(id);
    if (*p == '/') p++;
    int n = 0;
    for (; *p && n < 63; p++) o[n++] = *p == '/' ? '\\' : *p;
    o[n] = 0;
    return o;
}

/* cria a pasta (e a FOLIO, se preciso) */
void paths_ensure(int vol, int id) {
    bool d;
    if (!pal_exists(vol, "/FOLIO", &d)) pal_mkdir(vol, "/FOLIO");
    if (!pal_exists(vol, paths_dir(id), &d)) pal_mkdir(vol, paths_dir(id));
}

/* texto (já traduzido) com um %s para o nome de uma pasta: pathfmt(L("Saved to %s"), DIR_THEMES) */
const char *pathfmt(const char *f, int id) {
    static char buf[4][320]; static int k;
    char *o = buf[k = (k + 1) & 3];
    fmt(o, 320, f, paths_show(id));
    return o;
}
