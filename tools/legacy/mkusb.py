#!/usr/bin/env python3
"""
FOLIO-86 — Bare-Metal USB Image Builder & FAT16 Formatter
Constrói a imagem inicializável `build/folio86.img` (4 MB) contendo:
  - LBA 0      : Bootloader FAT16 (build/boot.bin)
  - LBA 1..32  : Motor Bare-Metal FOLIO-86 (build/kernel.bin)
  - LBA 33     : Setor de Configuração Persistente
  - LBA 64..71 : FAT #1 (Clusters pré-alocados para MANUSCR1.MD .. MANUSCR8.MD)
  - LBA 72..79 : FAT #2 (Espelho de segurança)
  - LBA 80..111: Diretório Raiz FAT16
  - LBA 112+   : Área de Dados Contígua dos 8 Manuscritos (32 KB cada)
"""

import os
import struct
import subprocess
import sys
from pathlib import Path

SECTOR_SIZE = 512
TOTAL_SECTORS = 8192  # 4 MB (8192 * 512 = 4.194.304 bytes)
RESERVED_SECTORS = 64
SECTORS_PER_FAT = 8
NUM_FATS = 2
ROOT_DIR_ENTRIES = 512
ROOT_DIR_SECTORS = (ROOT_DIR_ENTRIES * 32) // SECTOR_SIZE  # 32 setores
SECTORS_PER_CLUSTER = 4  # 2 KB por cluster

KERNEL_LBA = 1
KERNEL_SECTORS = 32
CONFIG_LBA = 33
FAT1_LBA = RESERVED_SECTORS  # 64
FAT2_LBA = FAT1_LBA + SECTORS_PER_FAT  # 72
ROOT_DIR_LBA = FAT2_LBA + SECTORS_PER_FAT  # 80
DATA_START_LBA = ROOT_DIR_LBA + ROOT_DIR_SECTORS  # 112

NUM_SLOTS = 8
SECTORS_PER_SLOT = 64  # 32 KB por manuscrito
CLUSTERS_PER_SLOT = SECTORS_PER_SLOT // SECTORS_PER_CLUSTER  # 16 clusters

WELCOME_MANUSCRIPT = (
    "# FOLIO-86 : A Maquina de Escrever Bare-Metal\n"
    "\n"
    "> Silencio absoluto. Nenhum sistema operacional. Apenas voce e as palavras.\n"
    "\n"
    "Bem-vindo ao FOLIO-86. Este ambiente roda diretamente no processador x86 a partir "
    "do seu pen-drive USB, sem sistema operacional, sem rede e sem distracoes.\n"
    "\n"
    "Experimente agora os recursos de qualidade de vida criados para escritores:\n"
    "\n"
    "- Pressione F2 para alternar entre as 4 paletas tipograficas suaves (Papiro Quente, "
    "Noite Obsidiana, Fosforo Ambar e Floresta Salvia).\n"
    "- Pressione F3 para ativar o Modo Foco (destaca apenas a Linha Atual ou o Paragrafo "
    "Atual, esmaecendo o restante da pagina).\n"
    "- Pressione F4 para ativar o Modo Maquina de Escrever (mantem a linha ativa sempre "
    "centralizada no meio da tela).\n"
    "- Pressione F5 para o Modo Hemingway (bloqueia Backspace e Delete para forcar o fluxo "
    "do primeiro rascunho).\n"
    "- Pressione F6 para iniciar um Sprint Pomodoro de 25 minutos.\n"
    "- Pressione F8 para alternar entre os 8 manuscritos (MANUSCR1.MD a MANUSCR8.MD) "
    "gravados diretamente no sistema de arquivos FAT16 deste pen-drive.\n"
    "- Pressione Ctrl+S a qualquer momento para salvar no pen-drive.\n"
    "\n"
    "Continue escrevendo abaixo ou pressione Ctrl+N para iniciar uma folha em branco.\n"
)

CHAPTER_TEMPLATE = (
    "# Capitulo I\n"
    "\n"
    "> Epigrafe ou nota de abertura do capitulo.\n"
    "\n"
    "Escreva seu primeiro paragrafo aqui. Quando voce conectar este pen-drive em "
    "qualquer computador comum, este arquivo estara disponivel diretamente na raiz "
    "como MANUSCR2.MD.\n"
)


def run_nasm(root: Path) -> tuple[bytes, bytes]:
    build_dir = root / "build"
    build_dir.mkdir(parents=True, exist_ok=True)

    boot_src = root / "src" / "boot.asm"
    kernel_src = root / "src" / "kernel.asm"
    boot_bin = build_dir / "boot.bin"
    kernel_bin = build_dir / "kernel.bin"

    subprocess.run(
        ["nasm", "-f", "bin", str(boot_src), "-o", str(boot_bin)],
        check=True,
    )
    subprocess.run(
        ["nasm", "-f", "bin", str(kernel_src), "-o", str(kernel_bin)],
        check=True,
    )

    boot_bytes = boot_bin.read_bytes()
    kernel_bytes = kernel_bin.read_bytes()

    if len(boot_bytes) != SECTOR_SIZE:
        raise ValueError(f"boot.bin deve ter 512 bytes, mas tem {len(boot_bytes)}")
    if len(kernel_bytes) != KERNEL_SECTORS * SECTOR_SIZE:
        raise ValueError(
            f"kernel.bin deve ter {KERNEL_SECTORS * SECTOR_SIZE} bytes, mas tem {len(kernel_bytes)}"
        )

    return boot_bytes, kernel_bytes


def build_fat_table() -> bytes:
    """Constrói uma tabela FAT16 com cadeias de 16 clusters pré-alocadas para cada um dos 8 slots."""
    fat_entries = [0x0000] * ((SECTORS_PER_FAT * SECTOR_SIZE) // 2)
    fat_entries[0] = 0xFFF8  # Media descriptor 0xF8
    fat_entries[1] = 0xFFFF  # End of chain

    for slot in range(NUM_SLOTS):
        first_cluster = 2 + slot * CLUSTERS_PER_SLOT
        for c in range(CLUSTERS_PER_SLOT - 1):
            fat_entries[first_cluster + c] = first_cluster + c + 1
        fat_entries[first_cluster + CLUSTERS_PER_SLOT - 1] = 0xFFFF

    return struct.pack(f"<{len(fat_entries)}H", *fat_entries)


def make_dir_entry(slot_idx: int, file_size: int) -> bytes:
    """Cria uma entrada de 32 bytes padrão FAT16 para MANUSCR<1..8>.MD."""
    name_8 = f"MANUSCR{slot_idx + 1}".encode("ascii")
    ext_3 = b"MD "
    attr = 0x20  # Archive
    nt_res = 0
    crt_time_tenth = 0
    # Hora: 21:30:00 -> (21 << 11) | (30 << 5) | 0
    fat_time = (21 << 11) | (30 << 5)
    # Data: 2026-11-11 -> ((2026 - 1980) << 9) | (11 << 5) | 11
    fat_date = ((2026 - 1980) << 9) | (11 << 5) | 11
    first_cluster_hi = 0
    first_cluster_lo = 2 + slot_idx * CLUSTERS_PER_SLOT

    return struct.pack(
        "<8s3sBBBHHHHHHHI",
        name_8,
        ext_3,
        attr,
        nt_res,
        crt_time_tenth,
        fat_time,
        fat_date,
        fat_date,
        first_cluster_hi,
        fat_time,
        fat_date,
        first_cluster_lo,
        file_size,
    )


def build_image(root: Path) -> Path:
    boot_bytes, kernel_bytes = run_nasm(root)
    img = bytearray(TOTAL_SECTORS * SECTOR_SIZE)

    # 1. LBA 0: Boot Sector
    img[0:SECTOR_SIZE] = boot_bytes

    # 2. LBA 1..32: Kernel Bare-Metal FOLIO-86
    k_start = KERNEL_LBA * SECTOR_SIZE
    img[k_start : k_start + len(kernel_bytes)] = kernel_bytes

    # 3. LBA 33: Setor de Configuração Inicial ('8F', slot=0, theme=0, goal_idx=1 (500), sound=0)
    cfg_offset = CONFIG_LBA * SECTOR_SIZE
    struct.pack_into("<HBBBB", img, cfg_offset, 0x4638, 0, 0, 1, 0)

    # 4. LBA 64..71 (FAT #1) e LBA 72..79 (FAT #2)
    fat_bytes = build_fat_table()
    f1_start = FAT1_LBA * SECTOR_SIZE
    f2_start = FAT2_LBA * SECTOR_SIZE
    img[f1_start : f1_start + len(fat_bytes)] = fat_bytes
    img[f2_start : f2_start + len(fat_bytes)] = fat_bytes

    # 5. Conteúdos Iniciais dos Manuscritos (Slots 0..7)
    initial_texts = [b""] * NUM_SLOTS
    initial_texts[0] = WELCOME_MANUSCRIPT.encode("ascii")
    initial_texts[1] = CHAPTER_TEMPLATE.encode("ascii")
    for s in range(2, NUM_SLOTS):
        initial_texts[s] = f"# Manuscrito {s + 1}\n\n".encode("ascii")

    # 6. LBA 80..111: Diretório Raiz FAT16 + LBA 112+: Área de Dados
    root_offset = ROOT_DIR_LBA * SECTOR_SIZE
    for slot in range(NUM_SLOTS):
        content = initial_texts[slot]
        entry = make_dir_entry(slot, len(content))
        img[root_offset + slot * 32 : root_offset + (slot + 1) * 32] = entry

        slot_lba = DATA_START_LBA + slot * SECTORS_PER_SLOT
        slot_offset = slot_lba * SECTOR_SIZE
        img[slot_offset : slot_offset + len(content)] = content

    out_path = root / "build" / "folio86.img"
    out_path.write_bytes(img)
    return out_path


if __name__ == "__main__":
    project_root = Path(__file__).resolve().parent.parent
    img_path = build_image(project_root)
    print(f"[OK] Imagem Bare-Metal USB gerada com sucesso: {img_path} ({img_path.stat().st_size} bytes)")

