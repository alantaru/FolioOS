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
                └───────────────────────────────────┬────────────────────────────────────────────────────┘
                                                    │  pal_* (platform abstraction, folio.h)
                       ┌────────────────────────────┴─────────────────────────────┐
                       │ src/platform/uefi        GOP/UGA video, SimpleTextInputEx, │
                       │   uefi_main.c, hda.c     Simple/Absolute Pointer, SimpleFS,│
                       │                          PCI I/O → HD Audio / AC'97        │
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

1. `efi_main` sets up memory, video (GOP, falling back to UGA), input and volumes.
2. `app_init`: resolve folder names (`paths.c`), load the embedded font, show the splash,
   scan fonts and themes, read `CONFIG.INI`, pick the language (setting, or UEFI
   `PlatformLang`, or English) and the keyboard layout, restore the last session.
3. Main loop: poll events → `app_frame` → present dirty regions.

## Testing

`build.ps1` compiles three targets from the same core — `BOOTX64.EFI`, `BOOTIA32.EFI` and
`folio_sim.exe` — plus `efi_host.exe`, a tiny UEFI implementation for Windows that loads and runs
the real `BOOTX64.EFI`. Scripts in `tests/` type, click, switch languages and keyboards,
export files and take screenshots; the build checks exports (valid ZIP/XML), translations
(placeholders, with a negative control) and several framebuffer formats. On macOS,
`build_macos.sh --test` builds the AppKit simulator and runs the same simulator scripts headless.

Detailed developer notes (in Portuguese) live in [dev/HANDOFF.pt-BR.md](dev/HANDOFF.pt-BR.md).
