/* FolioOS — driver FAT32 próprio (leitura e escrita, nomes longos), usado pela ponte BIOS→UEFI.
 * Independente de plataforma: o acesso ao disco vem por uma função de E/S de setores, então o mesmo
 * código roda no BIOS (INT 13h) e nos testes no Windows (arquivo de imagem). Só FAT32, setores de 512. */
#ifndef FOLIO_FAT32_H
#define FOLIO_FAT32_H
#include <stdint.h>
#include <stdbool.h>

typedef bool (*FatIo)(void *ctx, uint64_t lba, uint32_t count, void *buf, bool write);

typedef struct {
    uint32_t clus;           /* primeiro cluster (0 = arquivo vazio) */
    uint32_t size;
    uint8_t  attr;           /* 0x10 = pasta */
    uint32_t dir_clus;       /* pasta que contém a entrada (0 = é a raiz do volume) */
    int32_t  ent;            /* índice da entrada curta na pasta (-1 = raiz) */
    int32_t  first;          /* índice da primeira entrada da sequência (nome longo), = ent sem nome longo */
    uint16_t wdate, wtime;   /* modificação */
    uint32_t cur_idx, cur_clus;   /* cursor: cluster número cur_idx da cadeia é cur_clus (acelera leitura sequencial) */
} FatNode;

#define FAT_CACHE 64
typedef struct {
    FatIo io; void *ctx;
    uint64_t part;           /* LBA do início da partição */
    uint32_t spc, bpc;       /* setores e bytes por cluster */
    uint32_t fat_lba, fat_secs, nfats, data_lba, root, nclus;
    uint32_t fsinfo;         /* setor do FSInfo (relativo à partição), 0 = nenhum */
    bool mirror, dirty_info, ok;
    uint32_t hint;           /* próximo cluster livre provável */
    uint32_t free_count;     /* livres (0xFFFFFFFF = desconhecido) */
    uint32_t memo_d, memo_i, memo_c;   /* última posição achada numa pasta */
    struct { uint64_t lba; uint32_t age; bool used; uint8_t d[512]; } c[FAT_CACHE];
    uint32_t tick;
    void (*now)(uint16_t *date, uint16_t *time);
    char label[12];
} Fat;

enum { FAT_OK = 0, FAT_NOTFOUND = -1, FAT_EXISTS = -2, FAT_FULL = -3, FAT_IO = -4, FAT_BADNAME = -5, FAT_NOTEMPTY = -6 };

bool fat_mount(Fat *f, FatIo io, void *ctx, uint64_t part_lba);
void fat_root(Fat *f, FatNode *n);
int  fat_lookup(Fat *f, const FatNode *dir, const uint16_t *name, FatNode *out);
int  fat_create(Fat *f, const FatNode *dir, const uint16_t *name, bool is_dir, FatNode *out);
int  fat_read(Fat *f, FatNode *n, uint32_t pos, void *buf, uint32_t len);          /* bytes lidos, <0 erro */
int  fat_write(Fat *f, FatNode *n, uint32_t pos, const void *buf, uint32_t len);   /* bytes gravados, <0 erro */
int  fat_truncate(Fat *f, FatNode *n, uint32_t size);
int  fat_readdir(Fat *f, const FatNode *dir, uint32_t *cookie, uint16_t name[256], FatNode *out);   /* 1 = entrada, 0 = fim */
int  fat_delete(Fat *f, FatNode *n);
int  fat_rename(Fat *f, FatNode *n, const uint16_t *newname);
int  fat_flush(Fat *f);
uint32_t fat_free_clusters(Fat *f);
bool fat32_format(FatIo io, void *ctx, uint64_t part_lba, uint64_t nsec, const char *label, uint32_t serial);
bool fat32_format2(FatIo io, void *ctx, uint64_t part_lba, uint64_t nsec, uint64_t plan, const char *label, uint32_t serial);   /* FAT com folga para crescer até plan */
#endif
