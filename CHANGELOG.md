# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [4.4.1-beta] — 2026-11-19

**Beta.** Fixes from a full review of the 4.4.0 beta installer; please use this one instead. Like
4.4.0, it passes every test on emulated machines but has not run on real hardware yet: back up first.

### Added
- macOS simulator: app icon (the splash mark), drawn by `tools/mkicon_macos.m` at build time and
  packed with `iconutil`, so no image file is kept in the repository (#11, thanks to jagalindo).

### Fixed — firmware and boot
- A failure to read `BootOrder` could make the installer write a new boot order with FolioOS only,
  and a failed read of a `Boot####` variable could let it reuse a number that was in use. Only "not
  found" now counts as absent; any other read error stops before anything is written.
- Uninstalling removed every boot entry with "FolioOS" in its path, including another installation's.
  It now removes only the entries that point to the partition of the installation being removed.
- If the other system's boot entry failed to load, every unattended start tried it again. A failed
  attempt (FolioOS back in the very boot the entry was tried, by the firmware's boot counter) is no longer repeated automatically.
- Retrying a boot-entry registration no longer creates a duplicate entry.
- If the attempt record cannot be saved, FolioOS does not ask the firmware to start the other system
  automatically (it stays in FolioOS) instead of risking a loop.
- Disk writes are tied to the medium that was listed: if it changes (another stick in the same slot),
  nothing is read or written until the disks are scanned again. Buffers respect the firmware's I/O
  alignment. BIOS disks with sectors other than 512 bytes are no longer offered.
- An NVMe disk no longer shows garbage as its model.

### Fixed — disks and file systems
- NTFS: both `$LogFile` restart pages are read and the newest one decides; version 2.0 pages (Windows
  8+ with a pending journal) are refused. A restart page that is neither valid nor blank counts as
  "not clean". A damaged `$Volume` attribute (also one whose size would wrap around) can no longer make FolioOS
  read or write outside its record. The resize direction comes from the volume's own size.
- A failed flush is now an error in every resize, install, update, uninstall and partition-manager path
  (it used to report success).
- GPT: entries can no longer overlap the GPT itself; when only the backup GPT was valid, the primary
  is rewritten first; the backup reaches the disk before the primary is touched.
- exFAT: a shrink never frees the bitmap's own clusters (so the volume can always grow back), and the
  order of the writes keeps the bitmap at least as large as the volume after an interruption. A retry
  finishes a shrink interrupted before the bitmap's size was updated.
- FAT32: the free-cluster count is marked "unknown" instead of being adjusted, so a retried resize
  cannot apply the change twice.
- Deleting a partition shorter than 32 sectors no longer zeroes the start of the next one.
- Installing on a whole disk stops if the current partition table cannot be read (a blank disk or a
  damaged table still reads fine). Over an MBR with logical partitions, the safety copy also keeps
  their EBR sectors (with their positions), so the old table can be restored.
- Installing next to another system checks that the new partition fits in the table (MBR with four
  primaries) before shrinking anything.
- Uninstalling only grows back a partition the installer itself shrank (recorded at install time); a
  free-space install leaves the neighbour exactly as it was. Updating keeps that record, so an
  updated installation still gives the space back. Before anything is written, uninstalling
  checks that this partition can grow (a hibernated Windows or Fast Startup stops it with a message). `\EFI\FolioOS\FOLIO.ID` is removed only
  after the table changed, so an interrupted uninstall can be run again.
- The texts are copied before uninstalling also from the old `TEXTOS` folder, are always listed again,
  and any read error, long path or failed copy stops the uninstall before anything is deleted.
- Updating replaces each file recoverably (`<name>.~old` is restored if an update was interrupted); a
  failed final step is reported; an unreadable `CONFIG.INI` is never replaced with defaults; the source
  must contain the right boot program, not empty.
- Update and uninstall check that the disk still holds the installation shown on the screen.
- A retry after an interrupted grow (the table larger than the volume) finishes growing the volume, in
  the engine and in *Disks and partitions* (a *Finish resizing* button). A retry after a shrink
  whose table update failed only writes the table. This also works for an NTFS volume already marked for `chkdsk`,
  and exFAT finishes a pending bitmap update first. In *Disks and partitions*, *Finish resizing* is shown next to
  *Resize*, not instead of it. In a narrow window, *Format* and *Delete* move to a second row so all
  four buttons stay visible.
- The safety copy in `FOLIO/BACKUP` never overwrites an earlier one, and if the folder cannot be
  listed nothing is written. A directory read error is no longer taken for a missing `CONFIG.INI`.

### Fixed — your texts
- Exchanging texts with the USB drive skips a text with unsaved changes, never replaces a file whose
  previous version could not be saved first (nor one whose existence could not be checked), and reports
  the texts it could not copy (before the ones it skipped, so the warning is never cut off). If the
  previous version cannot be confirmed, the file is not replaced.
- Versions of documents with the same name in different folders no longer overwrite each other in
  `FOLIO/VERSIONS`, nor do two versions saved in the same minute. Pruning old versions never removes the
  one just saved.
- Tabs, recent documents and the last document reopen only from the volume they came from (by its
  identity), never from another volume that took the same number. A drive without a GPT partition ID
  (label and size only) is matched only at its recorded number; recent documents keep their place.
- With `docs_place=usb`, only a removable drive is taken for the USB drive.

### Fixed — screens
- The installer's options and the partition list scroll, so every option and partition can be seen and
  reached; the "Do not turn off" warning appears before the partition manager starts writing. A click
  on a button never selects a card hidden under it.
- Texts that promised more than the code does (a fully atomic update, a backup of every boot sector,
  zero-second wait) now say what really happens; messages are no longer cut off (long translations).
- The installer's shrink offer follows the same EFI partition rule as the engine; no false
  "firmware did not accept a boot entry" warning after a BIOS install; partition sizes use the disk's
  sector size; the LVM hint gives the whole safe order, partition included. On a BIOS PC, the 0-second
  wait text says how to reach the other system.

### Changed
- Documentation brought up to date with 4.4: the READMEs in all 15 languages (installer, keep
  proportions, 20 lab runs, platform badge with ARM64 and BIOS), the guides on the USB drive
  (`README.txt`, `LEIA-ME.txt`, `INSTALL.txt`, `COMO-INSTALAR.txt`), `ARCHITECTURE.md`,
  `ROADMAP.md`, `CONTRIBUTING.md`, `SECURITY.md` and the issue and pull request templates. They
  now say that the partition manager writes to disks too, that copying the texts on uninstall is
  optional, that only a partition the installer shrank grows back, where the safety copy goes when
  FolioOS started from a disk, and where to download the beta. Saving a text is named as the
  other way FolioOS writes to an internal volume. The installer and the guides say the safety copy is
  made before any partition changes (open texts are saved first).
- Tests and tools are stricter: refusals are checked on the whole disk, file inventories fail on any
  missing, extra or unreadable file, read errors fail comparisons, test tools are built without
  clang/nasm, the build uses a work folder of its own, the lab counts video failures and stops its
  VMs on errors, and the stand-in BIOS system checks the drive number it was started with.

### Fixed (documentation and tools)
- `COMO-INSTALAR.txt` and `ROADMAP.md` had paths garbled by lost backslashes, and
  `COMO-INSTALAR.txt` lacked the BIOS step. The guides said FolioOS takes 7 MB; it takes about 60 MB.
- `tools/readmes.pl` dropped the previous translation of a changed README block on its second
  run; it now keeps it as a `#~` entry, as `TRANSLATING.md` describes. A fuzzy translation stays fuzzy when
  it is archived and restored.
- `tools/linux-run.ps1` reported the exit code of an `echo` instead of the script's; `fatcat` reaches the
  backup GPT when the primary entries cannot be read; the file inventory keeps non-ASCII names apart;
  `fscheck_test` resize retries finish the table update; `--set-version` checks its buffer.
- `qemu-lab.ps1 -List` also lists the three dual-boot scenarios.
- macOS simulator: builds again (`pal_log` was missing from `src/platform/macos`) (#11).

## [4.4.0-beta] — 2026-11-18

**Beta.** The installer writes to the computer's own disk. It passes every test on emulated
machines (QEMU with OVMF and SeaBIOS, disks made by Windows and Linux, checked by Windows `chkdsk`
and Linux `fsck`), but it has not run on real hardware yet. Back up your disk before using it.

### Added
- **Install FolioOS on the computer** (command palette → *Install FolioOS on this computer*, or
  Settings → System). Keyboard-only screen, translated into every language:
  - **as the only system** on an empty or erased disk (GPT with UEFI, MBR with BIOS); the
    confirmation word is translated (*ERASE* in English);
  - **next to Windows or Linux** in free space, without touching the other partitions: a FAT32
    partition plus `\EFI\FolioOS` on the existing EFI partition and a firmware boot entry (UEFI),
    or the original MBR code kept in `FOLIO\OTHERMBR.BIN` (BIOS only);
  - **making room**: NTFS, FAT32 and exFAT are shrunk without moving data (only free space at the
    end is taken). The volume must be clean: Windows hibernated or with Fast Startup, a volume
    that needs `chkdsk`, or a dirty EFI partition is refused with an explanation. BitLocker disks
    ask you to confirm you have the recovery key. ext4 is never resized: the screen explains how to
    make room from Linux (GParted).
  - Every file is checked by CRC after copying; the partition table (the first 34 and last 33
    sectors of the disk, not each volume's boot sector) is backed up to the USB drive first
    (`FOLIO\BACKUP`).
  - A partition table FolioOS cannot read is never treated as an empty disk, and one it could not
    write back exactly (a GPT with a non-standard number or size of entries, an MBR with an extended
    partition) is shown but never changed. FAT32 volumes without mirrored FATs and exFAT volumes
    whose allocation bitmap is not contiguous are not resized.
- **Boot chooser** next to another system: FolioOS starts first and, without a key, hands over to
  the other system after `boot_wait` seconds (Settings → System; 0 = straight to FolioOS). With
  UEFI it uses `BootNext` and a restart, so BitLocker/TPM measurements stay as Windows expects.
- **Update** an installation from a newer USB drive: program, fonts, languages, themes and
  keyboards are replaced and checked; texts, music and settings stay.
- **Uninstall**: texts are copied to the USB drive and checked, then the neighbouring partition
  grows back to the size it had (the partition table ends up as before), the boot entry is
  removed and, with BIOS, the original MBR code is restored. A whole-disk install leaves an empty
  disk.
- **Where your texts live** on an installed FolioOS: on the computer, on the USB drive, or both
  (the newest copy wins; the replaced one is kept in `FOLIO\VERSIONS`). `docs_place=` in
  `CONFIG.INI`.
- **Disks and partitions** window (Settings → System, or the palette): create a FAT32 partition in
  free space, format, shrink/grow NTFS, FAT32 and exFAT, delete (with the confirmation word).
- **Keep proportions** (Settings → System → Display and pointer, `keep_aspect=` in `CONFIG.INI`): when the
  only video mode available has another shape than the screen (e.g. 1024×768 on a widescreen) and
  the screen stretches it, FolioOS draws at the screen's shape and the stretch evens out. Choose
  *Monitor* (the shape the monitor reports, 16:9 if it reports none), 16:9, 16:10 or 4:3.
  Works with UEFI and BIOS, also on rotated screens. Ctrl+Alt+Home turns it off and also resets
  rotation and resolution to automatic (and saves that); to turn off only the proportions, use the
  control in Settings.
- `DIAGNOSTICS.TXT` lists the computer's disks (bus, model, size, partitions and their state).
- Hardware lab: 17 QEMU machines (`-List`) plus 3 dual-boot scenarios (20 runs), including installed disks, dual boot next to a fake Windows
  (UEFI) and a fake other system (BIOS), shrinking an NTFS made by Windows, and keep proportions.

### Fixed
- `CONFIG.INI` lost `debug_serial` when the settings were saved.
- Video settings from `CONFIG.INI` (rotation, resolution) are applied before the boot diagnostics
  are written, so `DIAGNOSTICS.TXT` shows the screen actually in use.
- Build: the BIOS bridge is linked with `ld.lld` directly. On the CI runner the clang driver handed
  the link to gcc, whose lld ran in MinGW mode and rejected the linker script, so CI and the 4.3.0
  release workflow had failed since 4.3.0.

## [4.3.0] — 2026-11-17

### Added
- **PCs with BIOS only (Legacy/CSM):** `FOLIO\BIOS.SYS` is a small BIOS-to-UEFI bridge that runs
  the same `BOOTIA32.EFI` on PCs without UEFI — VESA/VBE video with the monitor's native mode
  (EDID), PS/2 keyboard and mouse/touchpad with wheel (USB ones through the BIOS's legacy
  emulation), its own FAT32 driver with writing over INT 13h, the CMOS clock, ACPI power-off and
  HD Audio/AC'97. `FOLIO\TOOLS\biosboot.exe E:` (or `.\flash_usb.ps1 -DriveLetter E: -Bios`)
  writes the boot code once; it only touches the MBR code and the FAT32 boot-sector code, never a
  file, and the UEFI boot keeps working.
- **ARM64:** `EFI\BOOT\BOOTAA64.EFI` for Raspberry Pi 3/4/5 with the community UEFI firmware and
  ARM laptops (built with LLVM `clang`).
- **Own pointer drivers** used when the firmware has none: USB HID (mouse, pen tablet, touch
  screen — absolute and relative, with wheel) and PS/2 mouse/touchpad (IntelliMouse wheel).
  Diagnostics mark them `[FolioOS USB HID driver]`.
- **Hardware lab** `tools/qemu-lab.ps1`: boots the real binaries on 13 QEMU machines (UEFI x64,
  IA32 and ARM64; BIOS-only from USB and IDE; 8 graphics cards; PS/2, USB mouse and tablet; HDA and
  AC'97) and checks picture, keyboard, mouse click, sound, saving a document and the integrity of
  the FAT32 volume on each. `tools/fatcat.c` reads files out of disk images and
  `tools/fat32_test.c` stress-tests the FAT32 driver (run by `build.ps1`).
- **Native macOS simulator** (`src/platform/macos`, `build_macos.sh`), contributed by
  @jagalindo in #4: an AppKit port of the Windows simulator with Retina support, Cmd as Ctrl for
  shortcuts, folders as volumes, audio and the same headless scripted tests
  (`./build_macos.sh --test`). Needs only the Xcode Command Line Tools; UEFI binaries, packages and
  disk images are still built on Windows. Option keeps typing characters; Option+M is Alt+M and
  Cmd+Option+1…9 is Alt+1…9. `src/core/embed.c` is portable to Mach-O (Apple clang).
- **LaTeX source editing** (`.tex`, `.bib`, `.sty`, `.cls`), contributed by @jagalindo in #5:
  listed, opened, saved, autosaved, versioned and reopened like Markdown documents, shown as plain
  source in the monospaced font with per-line highlighting (commands, braces, `%` comments, inline
  math) and real tab stops. Typing stays literal (no Markdown styling, list continuation or smart
  typography); `{`, `[` and `$` close themselves and wrap a selection. CRLF files are written back
  as CRLF. Reading mode and exports remain Markdown-only and say so.
- `debug_serial=1` in `CONFIG.INI` writes a debug log to the serial port.
- Command palette: **Sound test (chime)**.

### Fixed
- **32-bit UEFI (`BOOTIA32.EFI`):** the memory allocator's block header was 8 bytes on 32-bit,
  breaking the 16-byte alignment it relies on and corrupting memory.
- **HD Audio:** playback stalled after the first answer from the codec on controllers that need
  RINTCTL set and RIRBSTS cleared (QEMU's, among others).
- A very quick click could be lost between two reads of the mouse.
- `flash_usb.ps1` did not copy `FOLIO\FONTS\EXTRA` and `FOLIO\FONTS\_LANGUAGE` (Chinese, Japanese
  and Korean fonts) nor `BOOTAA64.EFI`.
- The plural index now also respects `nplurals` from the translation header.
- An English diagnostics line said "subsistema".

## [4.2.0] — 2026-11-17

### Added
- **Full keyboard operation:** every window can be used without a mouse. A window opened by its
  shortcut receives the keyboard; Tab/Shift+Tab and unused arrow keys move between controls,
  Enter/Space press them, Left/Right adjust sliders, segmented options, steppers and the music
  position. A focus ring (shown only after keyboard use) marks the active control, and tooltips
  appear for it too.
- **Ctrl+F6** moves the keyboard between the text and the open windows; **Alt+M** opens the menu
  (recent documents included), also reachable from the command palette.
- **Korean and Japanese input methods:** the new 한국어 (두벌식) keyboard composes Hangul syllables
  as you type (Backspace removes one jamo at a time) and 日本語 (ローマ字かな) turns romaji into
  hiragana or katakana; **Ctrl+Space** switches the input method. Keyboard files gain `ime` and
  `fonts` lines, and any Korean, Japanese or Chinese text now loads its font from the CJK pack on
  demand, whatever the interface language. Kanji conversion and pinyin still need dictionaries.
- **Alt+F7 / Alt+F8** move and resize windows with the arrow keys (Shift for small steps; Enter
  confirms, Esc puts the window back).
- **Option to switch the mouse and touchpad off** (Settings › System, Ctrl+K › Mouse, or
  `use_mouse=0` in `CONFIG.INI`).
- **Enter** toggles auto-scroll in reading mode; **Alt+↑** goes to the parent folder in the file
  manager (the shortcut was advertised but did not work).
- End-to-end test `tests/teclado.txt` that turns the mouse off and drives every window with keys.
- The full README in all 15 interface languages — same content as the English one, block by block.
  Translations live in `docs/i18n/README.<code>.po` (gettext, like the interface); `tools/readmes.pl`
  generates the pages, keeps the `#anchors` of the English headings working and flags blocks that
  changed in English. Every page has a language bar.

### Changed
- **All fonts are part of FolioOS:** `FolioOS.zip` (and the disk image) now include the 11 writing
  fonts and the Chinese, Japanese and Korean fonts. Every language works out of the box; the separate
  `FOLIO-FONTS-EXTRA.zip` and `FOLIO-FONTS-CJK.zip` packs are gone.

### Fixed
- Alt+letter shortcuts (Alt+1…9) now reach the app in the Windows simulator and ignore the active
  keyboard layout (they were translated, so Alt+1 failed on AZERTY).
- The music volume slider was labelled "Drive".
- F12 now behaves like F5, F6 and F10: it opens Settings, gives them the keyboard, and closes them.
- The menu showed "Ctrl+⇧+S" while the palette and help said "Ctrl+Shift+S".
- The Ctrl+W hint pointed only to a mouse button; it now also names the palette command.
- Unbound Alt+letter combinations no longer type the letter (they behave like unbound Ctrl+letter).
- Help: the reading-mode entries named only mouse actions (margin click, middle button, double click);
  they now list the keyboard equivalents.
- The danger style of confirm buttons depended on a Portuguese word.
- `tools/i18n update` dropped source references and translator comments from `.po` files.

## [4.1.0] — 2026-11-16

First public, open-source release as **FolioOS**.

### Added
- **Internationalization with nothing hard-coded:** interface translations are gettext `.po`
  files discovered at boot in `FOLIO/LANGUAGES`; plural rules (`Plural-Forms`), smart quotes,
  date format, decimal separator, default keyboard and script fonts come from each file's header.
- 15 interface languages: English, Spanish, French, German, Italian, Portuguese (Brazil),
  Portuguese (Portugal), Dutch, Polish, Turkish, Russian, Ukrainian, Chinese (Simplified),
  Japanese, Korean.
- Automatic language from the UEFI `PlatformLang` variable on first boot.
- Positional `printf` arguments (`%2$s`) so translations can reorder words.
- Keyboard layouts as plain-text files in `FOLIO/KEYBOARDS` (14 included: US, US-International,
  UK, ABNT2, Portugal, Spain, Latin America, AZERTY, QWERTZ, Italian, Polish, Turkish Q,
  Russian, Ukrainian) with shared dead-key compositions.
- On-demand fallback fonts for CJK interfaces (`FOLIO-FONTS-CJK.zip`); switching to a language
  whose fonts are missing is refused with an explanation.
- Translator tooling: `tools/i18n.c` (`extract`, `update`, `check`) and `docs/TRANSLATING.md`.
- Optional tabs, user themes with a theme editor, extra font pack, FolioOS splash screen
  (configurable duration).
- GitHub Actions build, issue forms, contribution and security policies.

### Changed
- Source language of the interface is now English; Portuguese became a translation.
- Folder and setting names are now neutral English (`DOCUMENTS`, `MUSIC`, `FOLIO/FONTS`,
  `FOLIO/THEMES`, `language=`, `theme=`…). Drives from older versions keep working with their
  Portuguese folders and settings, which are read transparently and never renamed.
- Hardware diagnostics (`FOLIO/DIAGNOSTICS.TXT`) are written in English for support.

## [4.0.x] — 2026

Private development of FOLIO 4: freestanding C rewrite for UEFI x64/IA32, TrueType
typography, live Markdown, reading mode, seven export formats, file manager, MP3 player,
Windows simulator and UEFI host for automated testing.
