# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

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
