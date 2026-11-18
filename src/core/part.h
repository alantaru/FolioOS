/* FolioOS — tabelas de partição (GPT e MBR), detecção de sistemas de arquivos e espaço livre.
 * Independente de plataforma, como o fat32.c: o acesso ao disco vem por uma função de E/S de setores,
 * então roda igual na UEFI, na ponte BIOS e nos testes no Windows (arquivos de imagem).
 * Nada aqui grava sem que part_write() seja chamado; as operações mexem só na tabela em memória. */
#ifndef FOLIO_PART_H
#define FOLIO_PART_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool (*rw)(void *ctx, uint64_t lba, uint32_t count, void *buf, bool write);
    void *ctx;
    uint64_t nsec;          /* setores do disco */
    uint32_t bs;            /* bytes por setor (512 ou 4096) */
    bool (*flush)(void *ctx);   /* opcional: esvazia o cache de escrita do disco (pontos seguros, não a cada gravação) */
} BlkDev;

enum { SCHEME_NONE, SCHEME_MBR, SCHEME_GPT, SCHEME_SUPERFLOPPY };
enum { FS_UNKNOWN, FS_EMPTY, FS_FAT12, FS_FAT16, FS_FAT32, FS_EXFAT, FS_NTFS, FS_EXT, FS_BTRFS, FS_XFS,
       FS_BITLOCKER, FS_LUKS, FS_APFS, FS_HFSP, FS_SWAP, FS_LVM, FS_ISO, FS_REFS };

#define PART_MAX 128
typedef struct {
    uint64_t start, count;  /* em setores */
    uint8_t  type[16];      /* GPT: GUID do tipo (bytes como no disco) */
    uint8_t  guid[16];      /* GPT: GUID único */
    uint64_t attrs;
    uint16_t name[37];      /* GPT: nome (UTF-16), MBR: vazio */
    uint8_t  mbr_type;      /* MBR: tipo (0x07 NTFS, 0x0C FAT32…) */
    bool     active;        /* MBR: partição ativa */
    bool     logical;       /* MBR: dentro da estendida (só leitura nesta fase) */
    int      fs;            /* FS_* detectado */
    int      slot;          /* GPT: posição da entrada na tabela (preservada ao regravar: o número da partição não muda) */
    char     label[40];     /* rótulo do sistema de arquivos, se houver */
} Part;

typedef struct {
    int scheme;
    uint8_t disk_guid[16];
    uint32_t mbr_sig;
    uint8_t mbr_code[440];  /* código de boot do MBR (preservado ao regravar) */
    int n;
    Part p[PART_MAX];
    bool gpt_backup_used;   /* o cabeçalho principal estava estragado: lido do reserva */
    bool has_extended;      /* MBR com partição estendida */
} PartTable;

typedef struct { uint64_t start, count; } Region;

/* tipos de partição GPT conhecidos */
extern const uint8_t GPT_ESP[16], GPT_MSDATA[16], GPT_MSR[16], GPT_WINRE[16], GPT_LINUX[16], GPT_LINUX_SWAP[16],
                     GPT_BIOSBOOT[16], GPT_APFS[16], GPT_HFSP[16], GPT_LVM[16];

uint32_t crc32_buf(uint32_t crc, const void *p, uint32_t n);
bool part_read(const BlkDev *d, PartTable *t);
int  part_probe(const BlkDev *d, uint64_t start, uint64_t count, char *label, int cap);   /* FS_* */
const char *part_fs_name(int fs);
const char *part_type_name(const Part *p, int scheme);    /* "EFI", "Windows", "Linux"… (inglês; a interface traduz) */
void part_new(PartTable *t, int scheme, uint64_t seed);  /* tabela vazia nova (GUID/assinatura a partir da semente) */
int  part_free(const BlkDev *d, const PartTable *t, Region *out, int cap);   /* espaços livres alinhados a 1 MiB */
int  part_add(const BlkDev *d, PartTable *t, uint64_t start, uint64_t count, const uint8_t type[16], uint8_t mbr_type, const char *name, uint64_t seed);
bool part_del(PartTable *t, int idx);
bool part_write(const BlkDev *d, const PartTable *t);     /* grava a tabela inteira (GPT: principal e reserva) */
bool part_overlaps(const BlkDev *d, const PartTable *t, uint64_t start, uint64_t count, int ignore);
void part_guid_str(const uint8_t g[16], char out[37]);        /* forma canônica "12345678-9ABC-…" (GUID do disco) */
bool part_guid_parse(const char *s, uint8_t g[16]);           /* o inverso; aceita maiúsculas e minúsculas */
#endif
