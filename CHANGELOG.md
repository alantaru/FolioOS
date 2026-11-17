# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

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
