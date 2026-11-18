/* FolioOS — discos do computador para o núcleo (ver disks.c) */
#ifndef FOLIO_DISKS_H
#define FOLIO_DISKS_H
#include "part.h"
bool disk_dev(int disk, BlkDev *out);                  /* BlkDev sobre pal_disk_io */
const char *disk_kind_name(int kind);                  /* "SATA", "NVMe"… (nomes técnicos, iguais em todo idioma) */
void disk_size_text(u64 bytes, char *o, int cap);
void disks_diag(Buf *o);                               /* seção "Disks" do DIAGNOSTICS.TXT */
#endif
