# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- **Full keyboard operation:** every window can be used without a mouse. A window opened by its
  shortcut receives the keyboard; Tab/Shift+Tab and unused arrow keys move between controls,
  Enter/Space press them, Left/Right adjust sliders, segmented options, steppers and the music
  position. A focus ring (shown only after keyboard use) marks the active control, and tooltips
  appear for it too.
- **Ctrl+F6** moves the keyboard between the text and the open windows; **Alt+M** opens the menu
  (recent documents included), also reachable from the command palette.
- **Option to switch the mouse and touchpad off** (Settings › System, Ctrl+K › Mouse, or
  `use_mouse=0` in `CONFIG.INI`).
- **Enter** toggles auto-scroll in reading mode; **Alt+↑** goes to the parent folder in the file
  manager (the shortcut was advertised but did not work).
- End-to-end test `tests/teclado.txt` that turns the mouse off and drives every window with keys.
- README in all 15 interface languages, generated from one template by `tools/readmes.pl`, with a
  language bar on every page.
- Native macOS simulator (`src/platform/macos`, `build_macos.sh`): an AppKit port of the
  Windows simulator with Retina support, Cmd as Ctrl for shortcuts, folders as volumes, audio
  and the same headless scripted tests (`./build_macos.sh --test`). Needs only the Xcode
  Command Line Tools; UEFI binaries, packages and disk images are still built on Windows.
  Option keeps typing characters; Option+M is Alt+M and Cmd+Option+1…9 is Alt+1…9.

### Changed
- `src/core/embed.c` is portable to Mach-O (Apple clang).

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
