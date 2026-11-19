# FolioOS architecture

FolioOS is a single freestanding C program that owns the whole machine. There is no kernel,
no libc and no driver model: the UEFI firmware provides the hardware access it needs
(framebuffer, keyboard, pointer, FAT file system, PCI), and FolioOS never calls
`ExitBootServices`.

```
                ┌──────────────────────────── src/core (portable, no libc) ─────────────────────────────┐
  keyboard ───► │ app.c      main loop, bars, commands, tabs, settings        i18n.c     gettext .po, plurals │
  pointer  ───► │ editor.c   text layout, caret, selection, typewriter, focus  keyboard.c layouts from files  │
                │ doc.c      paragraphs, undo         md.c  Markdown           paths.c    drive folder names  │
  framebuffer ◄─│ ui.c       immediate-mode widgets + window manager           theme.c    themes + .INI files │
                │ gfx.c      2D raster (AA shapes, masks)  font.c TrueType + kerning + fallback fonts         │
  FAT volume ◄─►│ export.c   DOCX/ODT/EPUB/HTML/RTF/TXT    filedlg.c file manager    music.c MP3 (minimp3)  │
                │ settings.c CONFIG.INI                    preview.c reading mode   splash.c base.c         │
                │ disks.c    the PC's disks   part.c GPT/MBR tables   fat32.c own FAT32 (format too)        │
                │ install.c  install, update, uninstall (copy + CRC check, safe file swap, texts to USB)    │
                │ installer.c installer screen: next to another system (or shrinking it), whole disk        │
                │ fscheck.c  other systems' partitions: refusals, NTFS/FAT32/exFAT shrink/grow in place     │
                │ chooser.c  boot chooser: "Press any key to write", else BootNext or the original MBR      │
                │ partitions.c disks and partitions window: create, delete, format, resize                  │
                └───────────────────────────────────┬────────────────────────────────────────────────────┘
                                                    │  pal_* (platform abstraction, folio.h)
                       ┌────────────────────────────┴─────────────────────────────┐
                       │ src/platform/uefi        GOP/UGA video, SimpleTextInputEx, │
                       │   uefi_main.c, hda.c     Simple/Absolute Pointer, SimpleFS,│
                       │                          Block I/O + Disk Info (disks),    │
                       │   usbhid.c, ps2mouse.c   PCI I/O → HD Audio / AC'97; own   │
                       │                          USB HID and PS/2 pointer drivers  │
                       │   x64 · IA32 · ARM64     when the firmware has none        │
                       ├────────────────────────────────────────────────────────────┤
                       │ src/platform/bios        BIOS-only PCs: boot sectors, then │
                       │   boot.asm, stub.asm     a 32-bit BIOS→UEFI bridge (VBE,   │
                       │   bridge.c               i8042, INT 13h → Block I/O for    │
                       │                          every disk + core FAT32, CMOS,    │
                       │                          ACPI) that runs BOOTIA32.EFI      │
                       ├────────────────────────────────────────────────────────────┤
                       │ src/platform/win32       simulator: GDI window, folders as │
                       │   win32_main.c           volumes, scripted tests           │
                       ├────────────────────────────────────────────────────────────┤
                       │ src/platform/macos       simulator: AppKit window, folders │
                       │   macos_main.m           as volumes, scripted tests        │
                       └────────────────────────────────────────────────────────────┘
```

## Principles

- **The drive is plain FAT32.** Everything FolioOS stores is an ordinary file a person can
  open, copy or edit on another computer. Updates never rename, move or delete user files;
  folders created by older versions (Portuguese names) keep being used.
- **No language in the code.** English source strings are marked with `L()`, `N_()` and
  `LN()`; translations, plural rules, quotes, dates, keyboards and script fonts are data files
  ([TRANSLATING.md](TRANSLATING.md)).
- **Never lose text.** Atomic saves, a recovery draft for untitled text, automatic versions
  of saved files, and the session restored on every boot.
- **Immediate-mode UI.** Each frame redraws from state; widgets are function calls. This keeps
  the UI code small and the simulator deterministic for tests.
- **Never risk someone else's disk.** Nothing is written to a disk before its first sectors are
  backed up to the USB drive, and writes are ordered so a power cut never leaves a volume larger than
  its partition. `part.c` marks a table it cannot read as `SCHEME_UNREADABLE` (never "empty") and
  one it could not write back byte for byte as `locked` (`part_write` refuses it; every operation
  stops before writing). `fscheck.c` reads other systems' volumes only to decide: it resizes NTFS,
  FAT32 and exFAT without moving data, and refuses anything it cannot fully verify.

## Video

`uefi_main.c` picks the output (the one with an active EDID and a framebuffer; the console output
when there are several monitors) and the mode (the panel's native one, or the largest with the same
shape), then draws a logical screen that may differ from the mode:

- **Rotation** (portrait tablets, `rotation=`): the logical screen is the mode turned 90/180/270°.
- **Keep proportions** (`keep_aspect=`): when the only mode has another shape than the panel and
  the monitor stretches it, the logical screen takes the panel's shape (e.g. 1024×576 inside
  1024×768) and `emit_scaled` enlarges it to the mode with linear interpolation, so the monitor's
  stretch evens out. The picture still fills the screen, so absolute pointer coordinates (0..1)
  need no change.
- Only what changed since the last frame is sent (a shadow copy of the logical screen); the
  framebuffer is mapped write-combining when the CPU allows it. The BIOS bridge exposes VESA/VBE as
  a GOP, so all of this also runs on BIOS-only PCs.

## Boot sequence

0. On a BIOS-only PC: the MBR finds the FAT32 partition, its boot sector loads `FOLIO\BIOS.SYS`;
   `stub.asm` enables A20, reads the E820 memory map, enters 32-bit protected mode and calls
   `bridge.c`, which loads `EFI\BOOT\BOOTIA32.EFI` and gives it a small UEFI system table built
   on the BIOS (it drops back to real mode only for INT 13h disk and INT 10h VBE calls).
1. `efi_main` sets up memory, video (GOP, falling back to UGA), input and volumes.
2. `app_init`: resolve folder names (`paths.c`), load the embedded font, show the splash,
   scan fonts and themes, read `CONFIG.INI`, apply the video settings (rotation, resolution,
   keep proportions) before writing `DIAGNOSTICS.TXT`, pick the language (setting, or UEFI
   `PlatformLang`, or English) and the keyboard layout, restore the last session. When FolioOS
   runs from a disk where it was installed, it finds its own partition through `FOLIO.ID` (UEFI)
   or `FOLIO\OTHERMBR.BIN` (BIOS); next to another system, `chooser.c` first shows "Press any key
   to write" for `boot_wait` seconds and otherwise hands over: on UEFI through `BootNext` and a
   restart (Windows and BitLocker see the usual boot), on BIOS by running the original MBR code
   through the bridge (`FOLIO_BIOS_CHAIN`).
3. Main loop: poll events → `app_frame` → present dirty regions.

## Testing

`build.ps1` compiles every target from the same core — `BOOTX64.EFI`, `BOOTIA32.EFI`,
`BOOTAA64.EFI` (clang), `FOLIO\BIOS.SYS` (clang + nasm) and `folio_sim.exe` — plus `efi_host.exe`,
a tiny UEFI implementation for Windows that loads and runs the real `BOOTX64.EFI`. Scripts in
`tests/` type, click, switch languages and keyboards, export files and take screenshots; the
build checks exports (valid ZIP/XML), translations (placeholders, with a negative control) and
several framebuffer formats. `tools/fat32_test.c` hammers the FAT32 driver with
thousands of random operations against an in-memory model and then checks the whole volume.
`tools/part_test.c` covers the GPT/MBR tables and FAT32 formatting; `tools/install_test.c` runs the
installer onto empty GPT and MBR disk images, and `efi_host.exe --disk <image>` lets the real
`BOOTX64.EFI` see those images as SATA disks through Block I/O. The installer screen is driven from
the keyboard in the simulator (`FOLIO_DISKS=a.img;b.img`) and in the real `BOOTX64.EFI`: a wrong
confirmation word must leave the disk untouched, the right one must produce a verified volume.
`install_test --windows` builds a disk like a PC with Windows (EFI partition, MSR, a hole in the
table, C:, recovery at the end); after installing next to it, `--compare` checks that every entry,
every other partition and the Windows files on the EFI partition are unchanged. `--windows-bios`
and `--mbr-fat32` do the same on MBR disks, and `--op-*` drive the partition manager's engine.
Refusals are tested too: a locked or unreadable table, a dirty or non-mirrored FAT32, a scattered
exFAT bitmap, BitLocker without confirmation — each must leave the disk byte for byte the same.

The file-system code is judged by the systems that own the formats, not only by FolioOS:
`tests/fixtures` holds NTFS, FAT32 and exFAT volumes formatted by Windows and ext4 volumes made by
Linux (with the facts each system reports), `tools/fscheck_test.c` must agree with them, and
`tools/ntfs-shrink-check.ps1` / `tools/vhd-chkdsk.ps1` (Windows `chkdsk`, as administrator) and
`tools/linux-run.ps1` (Alpine Linux in QEMU: `fsck.vfat`, `fsck.exfat`, `e2fsck`) check what FolioOS
wrote. Regenerating the fixtures needs administrator rights; the build only reads them.

`tools/qemu-lab.ps1` is the hardware lab: it boots the real binaries in QEMU on 17 machines (`-List`)
(UEFI x64/IA32/ARM64 with OVMF/AAVMF, BIOS-only with SeaBIOS; 8 graphics cards; PS/2, USB mouse
and tablet; HDA and AC'97; USB, IDE, SATA and NVMe disks; two of them start FolioOS installed on
the internal disk, with no USB drive, and use the installer screen to install onto a second NVMe or
IDE disk; two start with `keep_aspect=16:9` in a 1024×768 mode, on UEFI and on BIOS; three dual-boot
runs install from the screen next to another system — a stand-in Windows, `build\fakewin.efi`
(`dual-uefi-sata`), an NTFS made by Windows with no free space, shrunk by the installer
(`dual-ntfs-shrink`), and a stand-in BIOS system, `tools\fakeother.asm` (`dual-bios-ide`) — then
start the disk without a key (the other system) and with one (the FolioOS editor), uninstall from
the screen and check that the firmware or the original MBR starts the other system directly) and
drives each one over QMP: boot, keyboard,
mouse click, sound (captured to WAV), typing and saving a document, and afterwards reads the
disk image back to check the saved text, `DIAGNOSTICS.TXT` (including the list of disks) and the
consistency of the volume.
`debug_serial=1` in `CONFIG.INI` sends a debug log to the serial port (captured by the lab).
On macOS, `build_macos.sh --test` builds the AppKit simulator and runs the same simulator scripts headless.

Detailed developer notes (in Portuguese) live in [dev/HANDOFF.pt-BR.md](dev/HANDOFF.pt-BR.md).
