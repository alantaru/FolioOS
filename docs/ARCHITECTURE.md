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
                │ chooser.c  boot chooser: "press a key to write", else BootNext or the original MBR        │
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

## Boot sequence

0. On a BIOS-only PC: the MBR finds the FAT32 partition, its boot sector loads `FOLIO\BIOS.SYS`;
   `stub.asm` enables A20, reads the E820 memory map, enters 32-bit protected mode and calls
   `bridge.c`, which loads `EFI\BOOT\BOOTIA32.EFI` and gives it a small UEFI system table built
   on the BIOS (it drops back to real mode only for INT 13h disk and INT 10h VBE calls).
1. `efi_main` sets up memory, video (GOP, falling back to UGA), input and volumes.
2. `app_init`: resolve folder names (`paths.c`), load the embedded font, show the splash,
   scan fonts and themes, read `CONFIG.INI`, pick the language (setting, or UEFI
   `PlatformLang`, or English) and the keyboard layout, restore the last session.
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
every other partition and the Windows files on the EFI partition are unchanged.

`tools/qemu-lab.ps1` is the hardware lab: it boots the real binaries in QEMU on 15 machines
(UEFI x64/IA32/ARM64 with OVMF/AAVMF, BIOS-only with SeaBIOS; 8 graphics cards; PS/2, USB mouse
and tablet; HDA and AC'97; USB, IDE, SATA and NVMe disks; two of them start FolioOS installed on
the internal disk, with no USB drive, and use the installer screen to install onto a second NVMe or
IDE disk; a dual-boot run installs next to a stand-in Windows, `build\fakewin.efi`, then starts the
disk twice: without a key the firmware ends up in "Windows", with a key in the FolioOS editor) and
drives each one over QMP: boot, keyboard,
mouse click, sound (captured to WAV), typing and saving a document, and afterwards reads the
disk image back to check the saved text, `DIAGNOSTICS.TXT` (including the list of disks) and the
consistency of the volume.
`debug_serial=1` in `CONFIG.INI` sends a debug log to the serial port (captured by the lab).
On macOS, `build_macos.sh --test` builds the AppKit simulator and runs the same simulator scripts headless.

Detailed developer notes (in Portuguese) live in [dev/HANDOFF.pt-BR.md](dev/HANDOFF.pt-BR.md).
