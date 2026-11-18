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

### Phase 1 — Disk tools and installing as the only system 🔨
- Disk access through UEFI (`BlockIo`) and through the BIOS bridge (INT 13h). SATA, NVMe, USB and IDE
  disks are listed with their model and size.
- GPT and MBR partition tables, read and written in full (CRC32, backup GPT header, protective MBR).
- **Partition manager.** It can create, delete, format and resize partitions, with the safety rules
  below. In this phase it handles FAT32 (create, format, grow, shrink) and deletes any partition.
- **Install on this computer as the only system.** The installer erases the disk only after the
  user types a confirmation word. It then creates GPT with one FAT32 partition on UEFI PCs (the
  partition is also the EFI system partition) or MBR with FAT32 and the BIOS bridge on BIOS-only
  PCs, copies FolioOS and adds a firmware boot entry.
- **Where your texts live:** on the disk, on the USB drive, or on both, with a button that
  exchanges the texts with the USB drive locally.
- **Backup, restore and uninstall.** Before anything is written, the partition table and the boot
  sectors are saved to the USB drive (`FOLIO\BACKUP\`). One action restores them. Uninstalling
  offers to copy the texts to the USB drive first.
- **Update this computer from a newer USB drive.**

### Phase 2 — Dual boot on UEFI PCs ⏳
- FolioOS gets a FAT32 partition of its own. The existing EFI system partition receives only a
  ~600 KB file (`\EFI\FolioOS\`); fonts, settings and texts stay on FolioOS's partition.
- **Boot chooser.** Every start shows "press a key to write" for 3 seconds, then hands over to the
  other system: Windows Boot Manager, GRUB, systemd-boot, rEFInd or the macOS boot on Intel Macs.
- **Resizing the other system to make room:**
  - NTFS: shrink and grow;
  - ext4: shrink and grow;
  - FAT32 and exFAT.
- **Refused with a clear explanation** of how to fix it in the owning system:
  - volumes encrypted with BitLocker or LUKS;
  - LVM;
  - APFS/HFS+;
  - Windows hibernated or with Fast Startup;
  - any file system marked as needing a check.
- **BitLocker warning.** Turning off Secure Boot makes Windows ask for the recovery key, so the
  installer asks the user to confirm they have it. That prompt goes away with phase 7.

### Phase 3 — Dual boot on BIOS-only PCs ⏳
- Our MBR starts FolioOS, and the BIOS bridge shows the same chooser. To start the other system
  it runs the original MBR, which was saved at install time and is restored on uninstall.

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
   - Every operation writes a journal to the USB drive, so a power cut can be resumed or rolled back.
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
