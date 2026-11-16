#!/usr/bin/env python3
"""
FOLIO-86 — Suite de Auditoria e Verificação Binária (@qa)
Valida a imagem `build/folio86.img` quanto a:
  1. Assinatura de Boot MBR/VBR (0x55AA) e BPB FAT16
  2. Presença e integridade do Kernel Bare-Metal nos setores reservados (LBA 1..32)
  3. Consistência exata entre os cálculos de LBA do Assembly e os clusters FAT16
  4. Integridade das cadeias de clusters em FAT #1 e FAT #2
  5. Entradas do Diretório Raiz (MANUSCR1.MD a MANUSCR8.MD) e conteúdo inicial
"""

import struct
import sys
from pathlib import Path

SECTOR_SIZE = 512


def audit_image(img_path: Path) -> None:
    data = img_path.read_bytes()
    assert len(data) == 8192 * SECTOR_SIZE, f"Tamanho inválido: {len(data)}"

    # 1. Verifica assinatura de boot 0x55AA
    boot_sig = struct.unpack_from("<H", data, 510)[0]
    assert boot_sig == 0xAA55, f"Assinatura MBR inválida: {hex(boot_sig)}"

    # 2. Verifica campos do BPB FAT16
    oem = data[3:11]
    bytes_per_sec, sec_per_clus, res_secs, num_fats, root_entries, total_secs, media, secs_per_fat = (
        struct.unpack_from("<HBHBHHBH", data, 11)
    )
    assert oem == b"FOLIO86 ", f"OEM inválido: {oem}"
    assert bytes_per_sec == 512
    assert sec_per_clus == 4
    assert res_secs == 64
    assert num_fats == 2
    assert root_entries == 512
    assert total_secs == 8192
    assert media == 0xF8
    assert secs_per_fat == 8

    # 3. Verifica Kernel Bare-Metal em LBA 1..32
    kernel_start = 1 * SECTOR_SIZE
    assert data[kernel_start] == 0xFA, "Kernel deve iniciar com instrução CLI (0xFA)"
    kernel_slice = data[kernel_start : kernel_start + 32 * SECTOR_SIZE]
    assert b"FOLIO-86" in kernel_slice, "Assinatura de UI FOLIO-86 ausente no kernel"
    assert b"MODO HEMINGWAY ATIVO" in kernel_slice, "String do Modo Hemingway ausente"

    # 4. Verifica Config Sector em LBA 33
    cfg_magic = struct.unpack_from("<H", data, 33 * SECTOR_SIZE)[0]
    assert cfg_magic == 0x4638, f"Assinatura de configuração inválida: {hex(cfg_magic)}"

    # 5. Verifica igualdade entre FAT #1 (LBA 64..71) e FAT #2 (LBA 72..79)
    fat1 = data[64 * SECTOR_SIZE : 72 * SECTOR_SIZE]
    fat2 = data[72 * SECTOR_SIZE : 80 * SECTOR_SIZE]
    assert fat1 == fat2, "FAT #1 e FAT #2 divergem!"

    # 6. Verifica Diretório Raiz (LBA 80) e mapeamento LBA <-> Cluster FAT16 para os 8 Slots
    root_dir_off = 80 * SECTOR_SIZE
    data_start_lba = 64 + (2 * 8) + ((512 * 32) // 512)  # 112
    assert data_start_lba == 112

    for slot in range(8):
        entry_off = root_dir_off + slot * 32
        name = data[entry_off : entry_off + 8].decode("ascii")
        ext = data[entry_off + 8 : entry_off + 11].decode("ascii")
        first_clus = struct.unpack_from("<H", data, entry_off + 26)[0]
        file_size = struct.unpack_from("<I", data, entry_off + 28)[0]

        assert name == f"MANUSCR{slot + 1}", f"Nome incorreto no slot {slot}: {name}"
        assert ext == "MD ", f"Extensão incorreta no slot {slot}: {ext}"

        # Verifica que o LBA calculado pelo FAT16 coincide com get_slot_start_lba do Assembly
        fat_lba = data_start_lba + (first_clus - 2) * sec_per_clus
        asm_lba = 112 + (slot * 64)
        assert fat_lba == asm_lba, f"Divergência de LBA no slot {slot}: FAT={fat_lba} vs ASM={asm_lba}"

        # Verifica cadeia de 16 clusters na tabela FAT1
        cur = first_clus
        for step in range(15):
            nxt = struct.unpack_from("<H", fat1, cur * 2)[0]
            assert nxt == cur + 1, f"Cluster não-contíguo no slot {slot}: {cur} -> {nxt}"
            cur = nxt
        end_mark = struct.unpack_from("<H", fat1, cur * 2)[0]
        assert end_mark == 0xFFFF, f"Fim de cadeia inválido no slot {slot}: {hex(end_mark)}"

        # Verifica que o texto gravado no LBA do slot tem exatamente file_size bytes não-nulos
        slot_bytes = data[asm_lba * SECTOR_SIZE : (asm_lba + 64) * SECTOR_SIZE]
        actual_text = slot_bytes[:file_size]
        assert len(actual_text) > 0 and b"\x00" not in actual_text
        assert slot_bytes[file_size] == 0, "Byte após file_size deve ser NUL (0x00)"

    print("[QA PASS] Todos os 7 testes de auditoria binária, MBR, FAT16 e Kernel x86 passaram com 100% de precisão!")


if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    audit_image(root / "build" / "folio86.img")

