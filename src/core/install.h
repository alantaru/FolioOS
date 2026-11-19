/* FolioOS — motor de instalação no disco do computador (fase 1: como único sistema).
 * Independente de plataforma: o disco de destino é um BlkDev (part.h) e os arquivos de origem (o
 * pendrive) chegam por funções do chamador. Assim o mesmo código roda na UEFI, na ponte BIOS e nos
 * testes no Windows. A entrada de boot do firmware (UEFI) fica com a plataforma, depois do sucesso. */
#ifndef FOLIO_INSTALL_H
#define FOLIO_INSTALL_H
#include "part.h"

typedef struct {
    void *ctx;
    /* lista uma pasta da origem ("" = raiz; separador '/'): chama cb para cada entrada */
    bool (*list)(void *ctx, const char *dir, void (*cb)(void *u, const char *name, bool is_dir, uint64_t size), void *u);
    /* lê n bytes do arquivo a partir de off; devolve quantos leu (<0 = erro) */
    int  (*read)(void *ctx, const char *path, uint64_t off, void *buf, uint32_t n);
    /* grava uma cópia de segurança na origem (pendrive), em FOLIO/BACKUP/<name> */
    bool (*backup)(void *ctx, const char *name, const void *data, uint32_t n);
    /* desinstalar: grava um arquivo na origem (pendrive) pelo caminho ("DOCUMENTS/computer-.../a.md"), criando as pastas */
    bool (*save)(void *ctx, const char *path, const void *data, uint32_t n);
    void (*progress)(void *ctx, int percent, const char *what);
    uint64_t seed;          /* aleatoriedade para GUIDs e números de série */
    void *buf; uint32_t bufsize;   /* buffer de trabalho (>= 64 KB) */
    char date[20];          /* "2026-11-18-1530", para o nome da cópia de segurança */
} InstallSrc;

enum { INST_DOCS_DISK, INST_DOCS_STICK, INST_DOCS_BOTH };
typedef struct {
    bool bios;              /* PC sem UEFI: MBR + código de boot BIOS; senão GPT com a partição EFI */
    int docs;               /* INST_DOCS_*: onde ficam DOCUMENTS e MUSIC */
    const char *label;      /* rótulo do volume ("FOLIO") */
    const char *efi_name;   /* o programa desta arquitetura em EFI/BOOT ("BOOTX64.EFI"); vai para \EFI\FolioOS ao lado de outro sistema */
    const char *version;    /* a versão do FolioOS que está sendo instalada (vai para FOLIO/INSTALLED.ID) */
} InstallOpts;

enum { INST_OK, INST_ERR_READ, INST_ERR_WRITE, INST_ERR_SMALL, INST_ERR_BACKUP, INST_ERR_FORMAT,
       INST_ERR_COPY, INST_ERR_VERIFY, INST_ERR_BOOTCODE, INST_ERR_SOURCE,
       INST_ERR_NOTGPT, INST_ERR_NOESP, INST_ERR_ESPFULL, INST_ERR_REGION, INST_ERR_SHRINK, INST_ERR_GROW, INST_ERR_RESIZE, INST_ERR_TABLE };
typedef struct {
    int code; char path[160];
    uint64_t part_lba, part_count; uint8_t part_guid[16]; uint8_t disk_guid[16];   /* a partição do FolioOS */
    /* de onde o firmware inicia o FolioOS (para a entrada de boot): a partição EFI e o caminho nela */
    int esp_num; uint64_t esp_lba, esp_count; uint8_t esp_guid[16]; char boot_path[64];
} InstallResult;

#define INSTALL_MIN_BYTES (512ull << 20)       /* disco mínimo: 512 MB */
#define ALONGSIDE_MIN_BYTES (512ull << 20)     /* partição do FolioOS ao lado de outro sistema: 512 MB */
void install_whole_disk(const BlkDev *dev, const InstallSrc *src, const InstallOpts *o, InstallResult *r);
/* Fase 2, PCs UEFI com GPT: o FolioOS ganha uma partição FAT32 no espaço livre `free` e a partição EFI que já
 * existe recebe só \EFI\FolioOS\ (o programa e FOLIO.ID, que diz qual é a partição de dados). Nenhuma outra
 * partição é tocada; a tabela só ganha uma entrada. */
void install_alongside(const BlkDev *dev, const InstallSrc *src, const InstallOpts *o, Region free, InstallResult *r);
/* Sem espaço livre: encolhe o NTFS que começa em part_start (sem mover dados, ver fscheck.h) para new_count
 * setores e instala no espaço que sobra logo depois dele. */
void install_shrink_alongside(const BlkDev *dev, const InstallSrc *src, const InstallOpts *o, uint64_t part_start, uint64_t new_count, InstallResult *r);
/* \EFI\FolioOS\FOLIO.ID: "data=<GUID>" da partição de dados; devolve false se o texto não tiver um GUID */
bool install_parse_id(const char *text, int len, uint8_t guid[16]);
/* Desinstalar o FolioOS instalado ao lado de outro sistema (achado pelo EFI/FolioOS/FOLIO.ID na partição EFI). */
typedef struct {
    bool found;
    int kind;                         /* FOUND_ALONGSIDE ou FOUND_WHOLE (o FolioOS é o único sistema do disco) */
    char version[16];                 /* do FOLIO/INSTALLED.ID ("" = instalação anterior a esse arquivo) */
    bool marked;                      /* tem o FOLIO/INSTALLED.ID (senão foi reconhecido pelo conteúdo) */
    bool bios;                        /* ao lado num PC só BIOS: o MBR original está em FOLIO/OTHERMBR.BIN (fase 3) */
    uint64_t part_start, part_count; uint8_t part_guid[16];   /* a partição de dados do FolioOS */
    uint64_t esp_start; uint8_t esp_guid[16];   /* a partição EFI (ao lado de outro sistema, UEFI): para achar a entrada de boot certa */
    int ntfs;                         /* a partição logo antes dela (NTFS, FAT32 ou exFAT), que pode crescer de volta (-1 = nenhuma) */
    int grow_fs;                      /* o sistema de arquivos dela (FS_*) */
    uint64_t ntfs_start, ntfs_count, ntfs_grow_to;   /* em setores; grow_to = o tamanho depois de crescer */
    int docs_files; uint64_t docs_bytes;               /* textos em DOCUMENTS (para a prévia) */
} InstallFound;
enum { FOUND_NONE, FOUND_ALONGSIDE, FOUND_WHOLE };
bool install_find(const BlkDev *dev, InstallFound *f);
/* copy_texts: DOCUMENTS vai antes para o pendrive (DOCUMENTS/computer-<data>/, conferido relendo); grow: o NTFS
 * de antes recebe o espaço de volta. A entrada de boot do firmware fica com a plataforma (pal_boot_entry_remove).
 * Em caso de sucesso, r->path diz onde os textos ficaram no pendrive. */
void install_remove(const BlkDev *dev, const InstallSrc *src, const InstallOpts *o, const InstallFound *f, bool copy_texts, bool grow, InstallResult *r);
/* Atualizar por um pendrive mais novo: só o programa, as fontes, os idiomas, os temas e os teclados; os textos, a
 * música e os ajustes ficam. Cada arquivo é trocado inteiro (grava "<nome>.~new", confere, renomeia). */
void install_update(const BlkDev *dev, const InstallSrc *src, const InstallOpts *o, const InstallFound *f, InstallResult *r);
/* Gerenciador de partições: cada operação guarda antes o começo do disco no pendrive (src->backup), e grava na
 * ordem que sobrevive a uma queda de energia. create: FAT32 novo num espaço livre (alinhado a 1 MiB); delete: tira
 * a entrada e zera o começo do volume; format: FAT32 (o tipo da entrada passa a ser de dados, a EFI fica EFI);
 * resize: NTFS, FAT32 ou exFAT sem mover dados (crescer só para o espaço livre logo depois). */
void part_op_create(const BlkDev *dev, const InstallSrc *src, uint64_t start, uint64_t count, const char *label, InstallResult *r);
void part_op_delete(const BlkDev *dev, const InstallSrc *src, uint64_t part_start, InstallResult *r);
void part_op_format(const BlkDev *dev, const InstallSrc *src, uint64_t part_start, const char *label, InstallResult *r);
void part_op_resize(const BlkDev *dev, const InstallSrc *src, uint64_t part_start, uint64_t new_count, InstallResult *r);
const char *install_error(int code);           /* texto em inglês (a interface traduz) */
#endif
