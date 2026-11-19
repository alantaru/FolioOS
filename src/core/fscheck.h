/* FolioOS — o que se pode fazer com uma partição de outro sistema, sem risco.
 * A verificação só lê; ntfs_shrink() encolhe um NTFS em ordem sem mover dados.
 * Decide se uma partição pode ser encolhida para abrir espaço e, se não, por quê, com uma explicação de
 * como resolver no próprio sistema dono dela. Inclui um leitor NTFS mínimo: MFT, $Volume (sujo),
 * hiberfil.sys (Windows hibernado ou com Inicialização Rápida) e $Bitmap (até onde há dados). */
#ifndef FOLIO_FSCHECK_H
#define FOLIO_FSCHECK_H
#include "part.h"

enum { SAFE_OK, SAFE_BITLOCKER, SAFE_LUKS, SAFE_LVM, SAFE_APPLE, SAFE_HIBERNATED, SAFE_DIRTY,
       SAFE_EXT_RECOVER, SAFE_EXT_ERRORS, SAFE_UNREADABLE, SAFE_UNSUPPORTED, SAFE_COUNT };

typedef struct {
    int verdict;            /* SAFE_* */
    int fs;                 /* FS_* */
    uint64_t min_bytes;     /* encolher até aqui não move dados (NTFS: até o último cluster usado), 0 = não se sabe */
} FsCheck;

void fs_check(const BlkDev *d, const Part *p, FsCheck *out);
const char *fs_safety_text(int verdict);      /* uma frase em inglês: o problema e como resolver (a interface traduz) */
const char *fs_safety_short(int verdict);     /* rótulo curto ("BitLocker", "hibernated"…) */

/* ---- NTFS (só leitura) */
typedef struct {
    const BlkDev *d; uint64_t base;           /* setor inicial da partição */
    uint32_t bps, cs, rec;                    /* bytes por setor, por cluster, por registro da MFT */
    uint64_t nclus, mft_lcn;
    struct { uint64_t vcn, lcn, len; } mft_runs[64]; int nmft;
} Ntfs;
enum { NTFS_DIRTY = 1, NTFS_HIBERNATED = 2 };
bool ntfs_open(Ntfs *n, const BlkDev *d, uint64_t part_lba);
int  ntfs_flags(Ntfs *n);                     /* NTFS_DIRTY | NTFS_HIBERNATED; <0 = não deu para ler */
bool ntfs_last_used(Ntfs *n, uint64_t *lcn);  /* último cluster em uso, pelo $Bitmap */
bool ntfs_log_clean(Ntfs *n);                 /* $LogFile fechado de forma limpa (nada para o Windows repetir) */

/* ---- encolher NTFS sem mover dados (a única operação que grava neste arquivo)
 * O volume passa a ter new_sectors setores (o último é a cópia do setor de boot, como o Windows faz).
 * Só aceita um volume em ordem (nem sujo, nem hibernado, diário fechado, sem clusters defeituosos) e um
 * tamanho que deixe todos os clusters em uso dentro. A ordem das gravações deixa o volume usável se a
 * energia cair no meio: cópia do boot no novo fim (espaço livre) → volume marcado para chkdsk → $BadClus →
 * $Bitmap → setor de boot. O volume fica marcado para o Windows verificar na próxima vez (chkdsk), e a
 * tabela de partições é de quem chama (encolher a entrada só depois que isto devolver OK). */
enum { NTFS_SHRINK_OK, NTFS_SHRINK_UNREADABLE, NTFS_SHRINK_NOT_CLEAN, NTFS_SHRINK_BAD_SIZE, NTFS_SHRINK_TOO_SMALL,
       NTFS_SHRINK_BAD_CLUSTERS, NTFS_SHRINK_WRITE };
int ntfs_shrink(const BlkDev *d, uint64_t part_lba, uint64_t part_sectors, uint64_t new_sectors);
/* O inverso, para devolver o espaço ao desinstalar: o volume passa a ter new_sectors (<= part_sectors: a entrada
 * da tabela cresce antes). O $Bitmap não ganha clusters, então o máximo é ntfs_grow_max() (o tamanho de antes de
 * encolher sempre cabe). Aceita volume marcado como sujo (o encolher deixa assim); recusa hibernado/diário aberto. */
uint64_t ntfs_grow_max(const BlkDev *d, uint64_t part_lba);
/* FAT32, sem mover dados (os mesmos códigos NTFS_SHRINK_*): o volume passa a ter new_sectors (menor ou maior,
 * dentro da partição). Encolher só se os clusters que saem estão livres; crescer até onde a FAT existente alcança.
 * Nunca abaixo de 65525 clusters (deixaria de ser FAT32). Atualiza o BPB (e a cópia) e o FSInfo. */
uint64_t fat32_min_sectors(const BlkDev *d, uint64_t part_lba);
uint64_t fat32_max_sectors(const BlkDev *d, uint64_t part_lba);
int fat32_resize(const BlkDev *d, uint64_t part_lba, uint64_t part_sectors, uint64_t new_sectors);
/* exFAT, do mesmo jeito: só os clusters livres no fim (pelo bitmap de alocação) saem; cresce até onde o bitmap e a
 * FAT já reservados alcançam. Atualiza as duas regiões de boot (com a soma de verificação) e o tamanho do bitmap. */
uint64_t exfat_min_sectors(const BlkDev *d, uint64_t part_lba);
uint64_t exfat_max_sectors(const BlkDev *d, uint64_t part_lba);
int exfat_resize(const BlkDev *d, uint64_t part_lba, uint64_t part_sectors, uint64_t new_sectors);
/* qualquer um dos três: new_sectors é o tamanho da partição (encolher: o volume antes da entrada da tabela; crescer:
 * a entrada da tabela antes, com part_sectors = o tamanho novo). fs_grow_max: até quanto a partição pode crescer. */
bool fs_resizable(int fs);
int fs_resize(const BlkDev *d, int fs, uint64_t part_lba, uint64_t part_sectors, uint64_t new_sectors);
uint64_t fs_size(const BlkDev *d, int fs, uint64_t part_lba);   /* setores do volume pelo setor de boot (0 = não sei) */
uint64_t fs_grow_max(const BlkDev *d, int fs, uint64_t part_lba);
int ntfs_grow(const BlkDev *d, uint64_t part_lba, uint64_t part_sectors, uint64_t new_sectors);
#endif
