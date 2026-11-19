# FolioOS roadmap

FolioOS splits into two editions that share the same editor (`src/core`): the same themes, the
15 languages, the keyboards and the exports. Both keep texts as plain files in `DOCUMENTS\` on FAT32.

| | **FolioOS** | **FolioOS Sync** |
|---|---|---|
| Philosophy | **Zero internet, ever.** Instant and minimal. | Writing first; the network is off until you ask for it, and is used only to sync your texts |
| What it is | Today's freestanding program (~600 KB) on UEFI or BIOS | A minimal Alpine Linux that starts straight into the FolioOS editor |
| Hardware | Universal, down to very old PCs (BIOS-only, 32-bit UEFI) | Modern laptops (Wi-Fi, I2C touchpads, DSP audio), Raspberry Pi |
| Runs from | USB drive, or installed as the only system or next to any other | USB drive, or installed as the only system or next to any other |
| Secure Boot | Supported (phase 7) | Supported from the start |

Status: ✅ done · 🔨 in progress · ⏳ planned

---

## FolioOS (offline edition)

### Phase 1 — Disk tools and installing as the only system ✅
- ✅ Disk access through UEFI (`BlockIo`) and through the BIOS bridge (INT 13h). SATA, NVMe, USB and IDE
  disks are listed with their model and size.
- ✅ GPT and MBR partition tables, read and written in full (CRC32, backup GPT header, protective MBR).
  Rewriting a GPT keeps every entry in its slot, so partition numbers never change.
- ✅ **Partition manager** (Settings → System → "Disks and partitions…", or the command palette). It can
  create, delete, format and resize partitions, with the safety rules below: it creates and formats FAT32,
  grows and shrinks NTFS, FAT32 and exFAT without moving data, and deletes any partition after the
  confirmation word.
- ✅ **Install on this computer as the only system.** The installer erases the disk only after the
  user types a confirmation word. It then creates GPT with one FAT32 partition on UEFI PCs (the
  partition is also the EFI system partition) or MBR with FAT32 and the BIOS bridge on BIOS-only
  PCs, copies FolioOS and adds a firmware boot entry.
- ✅ **Where your texts live:** chosen when installing, changeable in Settings → System: on this
  computer, on the USB drive (they go with you; without the drive, new texts are saved on the computer),
  or on both, with a button that exchanges them with the USB drive (the newest version of each text wins,
  the replaced one is kept in `FOLIO\VERSIONS`, nothing is deleted).
- ✅ **Backup and uninstall.** Before anything is written, the partition table and the boot sectors
  are saved to the USB drive (`FOLIO\BACKUP\`). Uninstalling copies the texts to the USB drive first
  (checked file by file):
  - next to another system: `\EFI\FolioOS` and FolioOS's boot entry are removed and the space goes
    back to Windows; tested on a real UEFI firmware in the QEMU lab (afterwards the computer starts
    Windows directly);
  - as the only system: the disk is left empty, ready for another system (always asks for the
    confirmation word).
- ✅ **Update this computer from a newer USB drive.** Only the program, fonts, languages, themes and
  keyboards are replaced; texts, music and settings stay. Each file is replaced only after its new copy
  has been checked, so a power cut leaves either the old or the new file. `FOLIO\INSTALLED.ID` records
  the installed version.

### Phase 2 — Dual boot on UEFI PCs ✅
- ✅ FolioOS gets a FAT32 partition of its own in the free space. The existing EFI system partition
  receives only `\EFI\FolioOS\` (the program and `FOLIO.ID`, which names FolioOS's partition); fonts,
  settings and texts stay on FolioOS's partition. Nothing else on the disk changes: the table gains one
  entry, and every other partition stays byte-for-byte the same (checked by the build and the lab).
- ✅ **Boot chooser.** Every start shows "press a key to write" for 3 seconds (set in Settings → System,
  saved as `boot_wait=` in `CONFIG.INI`; 0 always starts FolioOS), then hands over to the other system: the next entry of the firmware's boot order
  (Windows Boot Manager, GRUB, systemd-boot, rEFInd…). The other system is started by the firmware
  (`BootNext` and a restart), never from inside FolioOS, so Windows starts exactly as before and
  BitLocker sees nothing different.
- **Resizing the other system to make room:**
  - ✅ NTFS: shrink without moving any data. When there is no free space, the installer offers to take
    space from the end of an NTFS partition in good order. The other system keeps what it uses plus 10%,
    and the size for FolioOS is chosen with the arrow keys, with a before/after preview. Checked by
    Windows itself: `chkdsk` finds nothing, every file stays byte-for-byte the same, and the new
    FolioOS partition is readable from Windows (`tools\ntfs-shrink-check.ps1`).
  - ✅ NTFS: grow back on uninstall, to exactly the size Windows had made (recorded at install time);
    Windows' `chkdsk` finds nothing, and the partition table, protective MBR and NTFS boot sector come
    back byte for byte the originals;
  - ✅ ext4: **not resized by FolioOS (decided).** Linux spreads folders over the whole volume and
    `mkfs.ext4` puts the journal in the middle, so shrinking without moving data frees almost nothing, and
    moving blocks is the risky part the rules forbid. When there is no free space, the installer explains
    how to shrink the partition from Linux (GParted) and then uses the free space. The ext4 checks are
    tested against volumes made by Linux itself (Alpine in QEMU, `tools\ext-fixtures.ps1`);
  - ✅ FAT32 and exFAT: shrink and grow back the same way (only the end moves; the FAT or the allocation
    bitmap is sized so the volume can grow back). Checked by Windows' `chkdsk` and by Linux's `fsck.vfat` and
    `fsck.exfat` against volumes formatted by Windows (`testsixtures`).
- ✅ **Refused with a clear explanation** of how to fix it in the owning system. Each partition shows
  its state in the installer:
  - volumes encrypted with BitLocker or LUKS;
  - LVM;
  - APFS/HFS+;
  - Windows hibernated or with Fast Startup;
  - NTFS marked dirty or with an unfinished `$LogFile`, FAT16/FAT32/exFAT marked dirty, ext4 with errors or
    a pending journal. Nothing is written to an EFI system partition that is marked dirty.

  The NTFS reader is tested against volumes formatted by Windows (`tests\fixtures`). The QEMU lab
  installs next to a disk made by Windows (EFI + NTFS, no free space) by shrinking it, then starts both
  systems (`dual-ntfs-shrink`).
- ✅ **BitLocker warning.** Changing the startup settings (and, later, turning off Secure Boot) can make
  Windows ask once for the recovery key. On a disk with BitLocker, the installer does not write anything
  until the user confirms they have the key. Phase 7 removes the Secure Boot part.

### Phase 3 — Dual boot on BIOS-only PCs ✅
- ✅ FolioOS gets a primary FAT32 partition in the free space (or by shrinking NTFS, FAT32 or exFAT, as in
  phase 2). The other entries of the MBR keep their positions and bytes, and the other system's partition
  stays the active one.
- ✅ Our MBR starts FolioOS's partition (its entry number is kept in the MBR), and the BIOS bridge shows the
  same chooser. To start the other system it runs the original MBR code, saved at install time in
  `FOLIOOTHERMBR.BIN`, exactly as the BIOS would have.
- ✅ Uninstalling puts the original MBR code back, removes FolioOS's entry and grows the other partition
  back: the table, the MBR and the boot sectors come back byte for byte the originals. The QEMU lab checks it
  on SeaBIOS with a stand-in for the other system (`dual-bios-ide`, `toolsakeother.asm`).

### Phase 7 — Secure Boot ⏳
- FolioOS is signed with the project's key and started through a shim signed by Microsoft.
- **Goal: no key confirmation at all.** The project's own shim goes through the public
  [shim-review](https://github.com/rhboot/shim-review) and is signed by Microsoft. This needs an EV
  code-signing certificate and a Microsoft hardware developer account, held by the project
  maintainer. Until then, the user approves the project's key once in MokManager.

### Rules of the partition manager (both editions)
1. **Nothing happens without a preview.** Every change is shown as before/after, in plain words
   ("Windows (C:) 237 GB → 180 GB, new FolioOS 57 GB"), and applied together with one confirmation.
2. **Never lose data.**
   - The partition table and boot sectors are backed up first.
   - Every operation is ordered so a power cut leaves a usable disk: a volume is shrunk before its table
     entry, and the entry grows before its volume, so a volume is never larger than its partition.
     (This replaces the journal first planned: with this order there is nothing to resume.)
   - Each file system is checked before it is resized and verified afterwards.
3. **Only what is safe.** Moving the start of a partition copies data and stays off until it is as
   well tested as everything else. File systems that cannot be resized safely are refused, with
   instructions instead of a risk.
4. **The same FolioOS.** Fully usable from the keyboard, translated into every language, and with
   the same calm interface.
5. **Tested on every build.** In the QEMU lab, virtual disks with Windows-like (NTFS), Linux
   (ext4/GRUB) and empty layouts are installed into, booted both ways and uninstalled. Partitions
   that were not part of the operation must stay byte-for-byte identical.

---

## FolioOS Sync (connected edition)

### Phase 4 — The Linux edition boots ⏳
- **Alpine Linux, minimal.** No desktop: the kernel starts the FolioOS editor directly. It draws
  through DRM/KMS and takes input through libinput.
- A Linux platform layer (`src/platform/linux`) for:
  - video;
  - input with FolioOS's own keyboard layouts;
  - ALSA sound;
  - files;
  - power off and suspend;
  - screen brightness;
  - the battery level.
- **The USB drive stays plain FAT32.** The kernel lives in `EFI\`, the read-only system is a single
  file (`FOLIO\SYSTEM.SQFS`), and settings and texts stay where they are today.
- Targets: ≤ ~150 MB besides the texts, start in ≤ ~6 s.
- x86-64 for PCs and laptops; arm64 for Raspberry Pi 3/4/5.

### Phase 5 — Network and the first sync services ⏳
- **Network.**
  - Wi-Fi (`iwd`, WPA2/WPA3), Ethernet and USB tethering from a phone, set up from a FolioOS screen.
  - **"Airplane mode" is the default:** the network turns on only when you ask, or to sync.
  - No browser, no notifications, nothing listening for incoming connections.
- **Sync, wave 1:**
  - WebDAV (Nextcloud, ownCloud, NAS);
  - SFTP;
  - FTP/FTPS;
  - SMB;
  - Git (commit, push, pull).
- **Never overwrite.** When two edits collide, both versions are kept
  (`chapter (conflict 2026-11-18).md`). Triggers are "Sync now", at shut-down, or every N minutes.
- **Credentials.** Passwords and tokens are stored encrypted on the USB drive with a passphrase.

### Phase 6 — Cloud accounts, P2P, Secure Boot and installation ⏳
- **Cloud accounts:**
  - Dropbox (app folder);
  - Google Drive (`drive.file` scope);
  - OneDrive (Microsoft Graph).

  Each user signs in with **their own account**. The login page opens on the phone through a code
  shown on screen, and FolioOS never sees the password. The project registers the app with each
  service once.
- **Syncthing** for peer-to-peer sync on the local network.
- **Secure Boot.** Shared with phase 7: one signed shim for both editions.
- **Installation** as the only system or next to any other, with the same partition manager and rules.
- **Signed updates.** "Update now" downloads a new system file, verifies its signature and replaces
  it. Security updates follow Alpine's.

### Only if there is demand
- CouchDB / Obsidian LiveSync, Joplin (through its WebDAV/Dropbox formats).
- **Not feasible:**
  - Obsidian Sync (proprietary, no public API);
  - Standard Notes (proprietary encrypted protocol).

---

## Writing features (both editions)
- [x] PCs with BIOS only (Legacy/CSM) and ARM64 (Raspberry Pi with UEFI firmware, ARM laptops) — 4.3.0
- [x] Korean input (2-set Hangul) and Japanese kana input (romaji → hiragana/katakana) — 4.2.0
- [x] LaTeX source editing — 4.3.0
- [ ] Japanese kanji conversion and Chinese pinyin input (need dictionaries)
- [ ] Text shaping and right-to-left scripts (Arabic, Hebrew, Devanagari…)
- [ ] Spell checking with Hunspell-compatible dictionaries
- [ ] Linux build of the simulator and tools (macOS: done in 4.3.0)

Ideas and votes are welcome in [Discussions](https://github.com/alantaru/FolioOS/discussions).
