FolioOS — a distraction-free digital typewriter that boots from this USB drive
==============================================================================

This drive works in two ways:

1) BOOT FROM IT (no Windows, no internet, no distractions)
   - Turn on the PC and open the boot menu (F9 on HP, F12 on Dell/Lenovo, F8 on
     Asus, Esc/F11 on others). Pick the "UEFI" entry of this USB drive.
   - If it does not show up: enter the firmware setup (F10/F2/Del) and DISABLE
     "Secure Boot".
   - PC without UEFI (BIOS/Legacy only, mostly from before ~2012): on Windows,
     run FOLIO\TOOLS\biosboot.exe E: once, as administrator (E: = this drive).
     It writes only the boot code - no file is touched - and from then on the
     drive also starts on those PCs, as an ordinary USB disk in the boot menu.
   - Raspberry Pi 3/4/5 and ARM laptops start EFI\BOOT\BOOTAA64.EFI through
     their UEFI firmware (on a Pi: the community UEFI firmware).

2) AS AN ORDINARY USB DRIVE, on any Windows, Linux or macOS computer
   Every folder is open, plain FAT32 — no proprietary format:

   DOCUMENTS\          your texts (.md, .txt) and exports (.docx .odt .epub
                       .html .rtf). Copy freely in both directions.
   MUSIC\              put your .mp3 files here; FolioOS plays them (F8) while
                       you write. Subfolders work too.
   FOLIO\FONTS\        .ttf fonts used by the editor. Add your own (subfolders
                       work). 11 writing fonts are already in FONTS\EXTRA.
   FOLIO\THEMES\       your themes (plain-text .INI: #RRGGBB colors and fonts).
   FOLIO\LANGUAGES\    interface translations (.po files — add a language by
                       dropping a file here). See "LANGUAGES" below.
   FOLIO\KEYBOARDS\    keyboard layouts (plain text). Copy one to make yours.
   FOLIO\TABS\         untitled texts open in tabs (they come back after a restart).
   FOLIO\CONFIG.INI    preferences (language, keyboard, theme, font, goals…).
                       Editable by hand.
   FOLIO\DRAFT.MD      automatic recovery of texts that have no name yet.
   EFI\BOOT\           the system itself. Do not delete.

   Drives set up by older versions keep their Portuguese folder names (TEXTOS,
   MUSICA, FOLIO\FONTES, FOLIO\TEMAS…). FolioOS keeps using them; nothing is
   renamed or moved.

.md files open in any text editor (Notepad, TextEdit, VS Code, Obsidian, Typora…).
Inside FolioOS, Ctrl+Shift+S exports to Word (.docx), LibreOffice (.odt), e-book
(.epub), web page (.html) and RTF.

Main shortcuts: F1 help · Ctrl+K commands · F2 focus · F3 typewriter ·
F11 zen mode · F12 appearance · Ctrl+Q shut down.

WITHOUT A MOUSE
   Everything works from the keyboard. A window opened by its shortcut gets the
   keyboard: Tab / Shift+Tab and the arrows move between its controls, Enter or
   Space press them, Left/Right adjust sliders and options, Esc closes. Ctrl+F6
   moves the keyboard between the text and the open windows; Alt+M opens the menu
   with the recent documents; Alt+F7 / Alt+F8 move / resize a window with the
   arrows (Enter confirms, Esc undoes). To switch the mouse and touchpad off completely:
   Settings > System > "Use the mouse and touchpad", Ctrl+K > "Mouse", or
   use_mouse=0 in FOLIO\CONFIG.INI.

LANGUAGES
   On first boot FolioOS follows the language of the firmware setup, when a
   translation exists; otherwise it starts in English. Change it any time in
   Settings > System > LANGUAGE AND KEYBOARD (or Ctrl+K > "Language").
   Included: English, Español, Français, Deutsch, Italiano, Português (Brasil),
   Português (Portugal), Nederlands, Polski, Türkçe, Русский, Українська,
   简体中文, 日本語, 한국어.
   The fonts for Chinese, Japanese and Korean are included (FOLIO\FONTS\_LANGUAGE),
   so every language works out of the box. Korean and Japanese can be typed with the keyboards
   "한국어 (두벌식)" (Hangul syllables) and "日本語 (ローマ字かな)" (romaji to kana);
   Ctrl+Space switches the input method on and off. Kanji conversion and Chinese
   pinyin are not available yet.
   To add or fix a language, edit or create FOLIO\LANGUAGES\<code>.po with Poedit
   or any text editor, and restart. Guide: docs/TRANSLATING.md in the project.

KEYBOARD
   Settings > System > Keyboard. Layouts are text files in FOLIO\KEYBOARDS:
   US, US-International, UK, ABNT2 (Brazil), Portugal, Spain, Latin America,
   French AZERTY, German QWERTZ, Italian, Polish, Turkish Q, Russian, Ukrainian,
   Korean (2-set Hangul input) and Japanese (romaji to kana).

DISPLAY
   FolioOS uses the native resolution of the screen and rotates the picture on
   tablets with a portrait screen. Settings > System lets you pick another
   resolution and rotate the screen. Ctrl+Alt+arrows rotate; Ctrl+Alt+Home goes
   back to automatic. In CONFIG.INI: resolution=auto (or 1920x1080, or max),
   rotation=auto (or 0, 90, 180, 270), fast_video=0 if the picture glitches.
   If circles look like ovals (the computer only offers, say, 1024x768 and the
   screen stretches it to widescreen), choose the shape of your screen in
   Settings > System (Monitor, 16:9, 16:10 or 4:3): FolioOS then draws at that
   shape and the stretching evens out. CONFIG.INI: keep_aspect=off (or monitor,
   16:9, 16:10, 4:3). Ctrl+Alt+Home turns it off again.
   If the PC shows nothing: very old graphics cards (before ~2012) may only work
   in BIOS mode - make the drive start in BIOS mode as explained in 1).

IF SOMETHING DOES NOT WORK (sound, display, mouse)
   FolioOS writes FOLIO\DIAGNOSTICS.TXT on every start (and in Settings > System >
   Save diagnostics). Open it on any computer: it says which sound card, display
   and mouse were found and what failed. Please attach it to bug reports.
