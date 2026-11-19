#!/usr/bin/env bash
# =============================================================================
# FolioOS - macOS build (simulator only; the UEFI binaries need the MinGW build, build.ps1)
#   ./build_macos.sh          builds build/macos/folio_sim, build/macos/FolioOS Simulator.app
#                             and assembles the drive folder build/usb_files
#   ./build_macos.sh --test   same, then runs the scripted simulator tests headless
# Requirements: Xcode Command Line Tools (clang); xmllint and unzip for the export checks.
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")"

TEST=0
for a in "$@"; do
    case "$a" in
        --test) TEST=1 ;;
        *) echo "usage: $0 [--test]" >&2; exit 2 ;;
    esac
done

if [ -t 1 ]; then C_STEP=$'\033[36m'; C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_END=$'\033[0m'
else C_STEP=; C_OK=; C_WARN=; C_ERR=; C_END=; fi
step() { echo "${C_STEP}[$1] $2${C_END}"; }
ok()   { echo "${C_OK}      $1${C_END}"; }
note() { echo "${C_WARN}      $1${C_END}"; }
die()  { echo "${C_ERR}failed: $1${C_END}" >&2; exit 1; }

CC="${CC:-clang}"
OUT=build/macos
USB=build/usb_files
mkdir -p "$OUT"

step 1 "macOS simulator -> $OUT/folio_sim"
"$CC" -O2 -g -fno-strict-aliasing -Isrc/core -Wall -Wextra -Wno-unused-parameter \
    src/core/*.c src/platform/macos/macos_main.m -fobjc-arc \
    -framework Cocoa -framework QuartzCore -framework AudioToolbox \
    -o "$OUT/folio_sim" || die "compiling the simulator"
ok "folio_sim built"

step 2 "App bundle -> $OUT/FolioOS Simulator.app"
VERSION=$(sed -n 's/^#define FOLIO_VERSION "\(.*\)".*/\1/p' src/core/folio.h)
APP="$OUT/FolioOS Simulator.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$OUT/folio_sim" "$APP/Contents/MacOS/folio_sim"
# icon: drawn by tools/mkicon_macos.m (the splash mark), packed with the system's iconutil
"$CC" -O2 -Wall -fobjc-arc tools/mkicon_macos.m -framework Foundation -framework CoreGraphics -framework ImageIO \
    -o "$OUT/mkicon_macos" || die "compiling tools/mkicon_macos.m"
rm -rf "$OUT/FolioOS.iconset"
"$OUT/mkicon_macos" "$OUT/FolioOS.iconset" || die "drawing the icon"
iconutil -c icns "$OUT/FolioOS.iconset" -o "$OUT/FolioOS.icns" || die "packing FolioOS.icns"
cp "$OUT/FolioOS.icns" "$APP/Contents/Resources/FolioOS.icns"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>FolioOS Simulator</string>
    <key>CFBundleDisplayName</key><string>FolioOS Simulator</string>
    <key>CFBundleIdentifier</key><string>org.folioos.simulator</string>
    <key>CFBundleExecutable</key><string>folio_sim</string>
    <key>CFBundleIconFile</key><string>FolioOS</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>${VERSION}</string>
    <key>CFBundleVersion</key><string>${VERSION}</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
EOF
ok "FolioOS Simulator.app (version $VERSION)"

step 3 "USB drive folder -> $USB (no EFI binaries: those come from build.ps1)"
# only system files are refreshed: documents and music placed in DOCUMENTS and MUSIC are kept
rm -rf "$USB/FOLIO/FONTS" "$USB/FOLIO/LANGUAGES" "$USB/FOLIO/KEYBOARDS"
mkdir -p "$USB/FOLIO/FONTS" "$USB/FOLIO/LANGUAGES" "$USB/FOLIO/KEYBOARDS" "$USB/FOLIO/THEMES" "$USB/DOCUMENTS" "$USB/MUSIC"
cp assets/fonts/*.ttf assets/fonts/OFL-*.txt "$USB/FOLIO/FONTS/"
cp assets/i18n/*.po "$USB/FOLIO/LANGUAGES/"
cp assets/keyboards/*.txt "$USB/FOLIO/KEYBOARDS/"
cp docs/README.txt docs/LEIA-ME.txt docs/INSTALL.txt "$USB/"
cp docs/MUSIC-README.txt "$USB/MUSIC/README.txt"
[ -e "$USB/DOCUMENTS/Welcome.md" ] || cp docs/Welcome.md "$USB/DOCUMENTS/Welcome.md"
# example themes: only when missing, so a theme edited by the user is never overwritten
for th in assets/themes/*.INI; do
    [ -e "$USB/FOLIO/THEMES/$(basename "$th")" ] || cp "$th" "$USB/FOLIO/THEMES/"
done
nfonts=$(ls "$USB"/FOLIO/FONTS/*.ttf | wc -l | tr -d ' ')
nlangs=$(ls assets/i18n/*.po | wc -l | tr -d ' ')
nkbd=$(ls assets/keyboards/*.txt | grep -v '/_' | wc -l | tr -d ' ')
ok "$nfonts fonts - $((nlangs + 1)) languages - $nkbd keyboards"

step 4 "Translations: check of every language against assets/i18n/folio.pot"
if cc -O2 -Wall tools/i18n.c -o "$OUT/i18n" 2>"$OUT/i18n_build.log"; then
    "$OUT/i18n" check assets/i18n/folio.pot assets/i18n/*.po || die "translation check"
    # negative control: the checker must reject a translation whose %s/%d do not match the original
    bad="$OUT/i18n_bad.po"
    perl -pe 's/msgstr "Tema: %s"/msgstr "Tema: %d"/' assets/i18n/es.po > "$bad"
    if "$OUT/i18n" check assets/i18n/folio.pot "$bad" >/dev/null 2>&1; then
        die "i18n check accepted a broken format (negative control failed)"
    fi
    rm -f "$bad" "$OUT/i18n_build.log"
    ok "translations: formats valid in every language; the checker rejects broken formats"
else
    note "tools/i18n.c did not compile here: translation check skipped (see $OUT/i18n_build.log)"
fi

if [ "$TEST" = 1 ]; then
    step 5 "Automated tests (headless simulator)"
    SIM="$OUT/folio_sim"
    t="$OUT/test_root"
    shots="$OUT/shots"
    mkdir -p "$shots"
    passed=0; failed=0; skipped=0
    Fresh() {
        rm -rf "$t"
        cp -R "$USB" "$t"
        rm -rf "$t"/DOCUMENTS/* "$t"/MUSIC/*.mp3 "$t/FOLIO/CONFIG.INI"
    }
    # has_line FILE REGEX: CONFIG.INI may have CRLF line endings
    has_line() { [ -f "$1" ] && tr -d '\r' < "$1" | grep -q "$2"; }
    LOG="$OUT/test.log"
    # sim NAME: runs tests/NAME.txt; output kept in $LOG
    sim() { "$SIM" --root "$t" --script "tests/$1.txt" --out "$shots" >"$LOG" 2>&1; }
    pass() { passed=$((passed + 1)); echo "${C_OK}      PASS  $1${C_END}"; }
    fail() {
        failed=$((failed + 1)); echo "${C_ERR}      FAIL  $1: $2${C_END}"
        if [ -s "$LOG" ]; then sed 's/^/            /' "$LOG"; fi
    }
    skip() { skipped=$((skipped + 1)); echo "${C_WARN}      SKIP  $1: $2${C_END}"; }

    # the original test scripts drive the Portuguese interface: the "firmware" says pt-BR, which also
    # exercises automatic language detection (and the ABNT2 keyboard that pt-BR.po asks for)
    export FOLIO_PLATFORM_LANG=pt-BR

    Fresh; if sim smoke; then pass smoke; else fail smoke "folio_sim exited with an error"; fi

    Fresh
    if ! sim full; then fail full "folio_sim exited with an error"
    else
        # exported files: ZIP structure + well-formed XML, language tag from the interface language
        z="$t/_zip"; why=""
        for f in docx odt epub; do
            if [ ! -f "$t/DOCUMENTS/teste.$f" ]; then why="DOCUMENTS/teste.$f missing"; break; fi
            mkdir -p "$z/$f"
            if ! unzip -qo "$t/DOCUMENTS/teste.$f" -d "$z/$f" >>"$LOG" 2>&1; then why="teste.$f is not a valid ZIP"; break; fi
            while IFS= read -r -d '' x; do
                if ! xmllint --noout "$x" >>"$LOG" 2>&1; then why="malformed XML: ${x#$z/}"; break 2; fi
            done < <(find "$z/$f" -type f \( -name '*.xml' -o -name '*.xhtml' -o -name '*.opf' \) -print0)
        done
        if [ -z "$why" ] && ! find "$z/epub" -name '*.opf' -exec grep -l '<dc:language>pt-BR</dc:language>' {} + 2>/dev/null | grep -q .; then
            why="EPUB without the interface language tag"
        fi
        if [ -z "$why" ]; then pass "full (DOCX/ODT/EPUB exports: valid ZIP and XML, language tag pt-BR)"; else fail full "$why"; fi
    fi

    Fresh
    if ! sim versoes; then fail versoes "folio_sim exited with an error"
    elif ! ls "$t"/FOLIO/VERSIONS/*.md >/dev/null 2>&1; then fail versoes "automatic versions were not created"
    else pass "versoes (automatic versions in FOLIO/VERSIONS)"; fi

    Fresh
    if ! sim leitura; then fail leitura "folio_sim exited with an error"
    elif ! has_line "$t/FOLIO/CONFIG.INI" '^position1='; then fail leitura "per-document position was not saved"
    else pass "leitura (reading and writing, per-document position)"; fi

    Fresh
    if ! sim abas; then fail abas "folio_sim exited with an error"
    elif ! has_line "$t/FOLIO/CONFIG.INI" '^tabs=1'; then fail abas "tabs were not saved in CONFIG.INI"
    else pass "abas (tabs, reopened after restart)"; fi

    Fresh
    mkdir -p "$t/FOLIO/FONTS/EXTRA"
    cp -R "assets/fonts-extras/Lora" "assets/fonts-extras/iA Writer Quattro" "$t/FOLIO/FONTS/EXTRA/"
    if sim temas; then pass "temas (user themes, theme editor, splash)"; else fail temas "folio_sim exited with an error"; fi

    # LaTeX sources: the fixture is CRLF (tests/*.txt are always CRLF, see .gitattributes); after the
    # script the autosaved file must match tests/latex_expected.txt byte for byte
    Fresh; cp tests/latex_doc.txt "$t/DOCUMENTS/artigo.tex"
    if ! sim latex; then fail latex "folio_sim exited with an error"
    elif ! cmp -s "$t/DOCUMENTS/artigo.tex" tests/latex_expected.txt; then fail latex "autosaved DOCUMENTS/artigo.tex differs from tests/latex_expected.txt"
    elif [ ! -f "$t/DOCUMENTS/novo.tex" ] || [ -e "$t/DOCUMENTS/novo.tex.md" ]; then fail latex "saving as novo.tex did not keep the extension"
    elif grep -q $'\r' "$t/DOCUMENTS/novo.tex"; then fail latex "a new .tex must be written with LF line endings"
    elif [ -e "$t/DOCUMENTS/artigo.html" ]; then fail latex "a LaTeX source was exported through the Markdown converter"
    elif ! ls "$t"/FOLIO/VERSIONS/*.tex >/dev/null 2>&1; then fail latex "no automatic version of the .tex file"
    else pass "latex (LaTeX sources: raw text, highlighting, CRLF kept on autosave, save as .tex, no reading/export)"; fi

    for x in efi_boot efi_roda "video formats (rgb, mask16, blt, pad, uga, edid)"; do
        skip "$x" "needs build/efi/BOOTX64.EFI and efi_host.exe (Windows build only)"
    done

    # languages: English by default (no firmware language), switching, plurals, keyboards, CJK fonts,
    # old Portuguese installs keep Portuguese
    unset FOLIO_PLATFORM_LANG
    Fresh
    if [ -d "assets/fonts-cjk/_LANGUAGE" ]; then
        cp -R "assets/fonts-cjk/_LANGUAGE" "$t/FOLIO/FONTS/"
        if ! sim i18n; then fail i18n "folio_sim exited with an error"
        elif ! has_line "$t/FOLIO/CONFIG.INI" '^language='; then fail i18n "language was not saved in CONFIG.INI"
        else pass "i18n (English default, translations, plurals, keyboards, CJK fonts)"; fi
    else
        # tests/i18n.txt switches to Japanese and Chinese, which the core refuses without the fonts
        skip i18n "CJK fonts not downloaded (assets/fonts-cjk/_LANGUAGE; tools/fetch-cjk-fonts.ps1)"
    fi

    Fresh; rm -rf "$t/FOLIO/FONTS/_LANGUAGE"
    if sim i18n_cjk_missing; then pass "i18n_cjk_missing (language kept without the CJK pack)"
    else fail i18n_cjk_missing "folio_sim exited with an error"; fi

    Fresh; printf 'tema=Papiro\r\nteclado=abnt2\r\nabertura=0\r\n' > "$t/FOLIO/CONFIG.INI"
    if ! sim i18n_legacy; then fail i18n_legacy "folio_sim exited with an error"
    elif ! has_line "$t/FOLIO/CONFIG.INI" '^theme=Papyrus'; then fail i18n_legacy "old Portuguese CONFIG.INI was not migrated to the new keys"
    else pass "i18n_legacy (old Portuguese installs stay in Portuguese)"; fi

    # no mouse at all: the mouse is turned off and every window is used with Tab, arrows, Enter and Space
    Fresh
    if ! sim teclado; then fail teclado "folio_sim exited with an error"
    elif ! has_line "$t/FOLIO/CONFIG.INI" '^use_mouse=0'; then fail teclado "turning the mouse off was not saved in CONFIG.INI"
    elif ! has_line "$t/FOLIO/CONFIG.INI" '^typewriter=1'; then fail teclado "a setting changed with the keyboard was not saved"
    else pass "teclado (keyboard only: mouse off, menu, files, settings, themes, outline, Ctrl+F6, dialogs, reading)"; fi

    echo "tests: $passed passed, $failed failed, $skipped skipped"
    if [ "$failed" -gt 0 ]; then
        echo "${C_ERR}test root kept for inspection: $t${C_END}" >&2
        exit 1
    fi
    rm -rf "$t" "$LOG"
fi

echo
echo "${C_OK}Done. To try it:  ./$OUT/folio_sim   (or open \"$APP\")${C_END}"
