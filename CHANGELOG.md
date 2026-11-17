# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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
  typography); `{`, `[` and `# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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
 close themselves and wrap a selection. CRLF files are written back
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
- A `.tex` file opened with "All files" was styled as Markdown, got smart quotes, and was never
  autosaved or saved on tab switch or shutdown; "Save as" `name.tex` produced `name.tex.md`.

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
