# =============================================================================
# FolioOS - build pipeline
#   .\build.ps1            builds everything, assembles the USB folder and runs the tests
#   .\build.ps1 -NoTests   build only
#   .\build.ps1 -Legacy    also builds the legacy FOLIO-86 v3 (Assembly, BIOS) into build\legacy
#   .\build.ps1 -Image [-ImageMB 4096] [-Vhd]
#                          writes build\folio.img (MBR + FAT32) for Rufus/Etcher/dd
#                          (-Vhd writes build\folio.vhd for VirtualBox/Hyper-V with EFI)
# Requirements: gcc (MinGW-w64) and perl on PATH (Git for Windows ships perl); nasm only for -Legacy.
# The Chinese/Japanese/Korean fonts are downloaded once by tools\fetch-cjk-fonts.ps1 (run automatically).
# =============================================================================
param([switch]$NoTests, [switch]$Legacy, [switch]$Image, [int]$ImageMB = 2048, [switch]$Vhd)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Step($n, $msg) { Write-Host "[$n] $msg" -ForegroundColor Cyan }
function Ok($msg) { Write-Host "      $msg" -ForegroundColor Green }
function Run($exe, [string[]]$argv) {
    $ErrorActionPreference = "Continue"
    & $exe @argv 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "failed: $exe $($argv -join ' ')" }
}

New-Item -ItemType Directory -Force build, build\efi, build\shots | Out-Null

$core = Get-ChildItem src\core\*.c | ForEach-Object { $_.FullName }
$warn = @("-Wall", "-Wextra", "-Wno-unused-parameter")
$free = @("-O2", "-ffreestanding", "-fno-stack-protector", "-fno-stack-check", "-mno-stack-arg-probe", "-fshort-wchar",
          "-fno-strict-aliasing", "-fno-tree-loop-distribute-patterns", "-fno-asynchronous-unwind-tables",
          "-DFOLIO_FREESTANDING", "-Isrc/libc", "-Isrc/core") + $warn
$uefi = @("src\platform\uefi\uefi_main.c", "src\platform\uefi\hda.c", "src\platform\uefi\usbhid.c", "src\platform\uefi\ps2mouse.c")
$link = @("-nostdlib", "-shared", "-Wl,--subsystem,10", "-Wl,--exclude-all-symbols")

Step 1 "UEFI x86_64  -> build\efi\BOOTX64.EFI"
Run gcc ($free + @("-mno-red-zone") + $core + $uefi + $link + @("-Wl,-e,efi_main", "-o", "build\efi\BOOTX64.EFI", "-lgcc"))
# the QEMU lab's stand-in for Windows (dual boot test): paints the screen blue, prints FAKE WINDOWS STARTED
Run gcc ($free + @("-mno-red-zone", "tools\fakewin.c") + $link + @("-Wl,-e,efi_main", "-o", "build\fakewin.efi"))

Step 2 "UEFI IA32    -> build\efi\BOOTIA32.EFI (tablets/netbooks with 32-bit firmware)"
Run gcc ($free + @("-m32", "-march=i686", "-mno-sse", "-mfpmath=387") + $core + $uefi + $link + @("-Wl,-e,_efi_main", "-o", "build\efi\BOOTIA32.EFI"))

Step 2b "UEFI ARM64   -> build\efi\BOOTAA64.EFI (Raspberry Pi 3/4/5 with UEFI firmware, ARM laptops)"
$clang = Get-Command clang -ErrorAction SilentlyContinue
if (-not $clang -and (Test-Path "$env:ProgramFiles\LLVM\bin\clang.exe")) { $clang = "$env:ProgramFiles\LLVM\bin\clang.exe" }
if ($clang) {
    $aa64 = @("--target=aarch64-unknown-windows", "-O2", "-ffreestanding", "-fno-stack-protector", "-mno-stack-arg-probe", "-fshort-wchar",
              "-fno-strict-aliasing", "-fno-math-errno", "-DFOLIO_FREESTANDING", "-Isrc/libc", "-Isrc/core") + $warn
    Run $clang ($aa64 + $core + $uefi + @("-nostdlib", "-fuse-ld=lld", "-Wl,/subsystem:efi_application", "-Wl,/entry:efi_main", "-o", "build\efi\BOOTAA64.EFI"))
    Remove-Item build\efi\BOOTAA64.lib -ErrorAction SilentlyContinue
} else {
    Write-Host "      clang (LLVM) not found: BOOTAA64.EFI not built (winget install LLVM.LLVM)" -ForegroundColor Yellow
}

Step 2c "BIOS-only PCs -> build\bios\BIOS.SYS (BIOS-to-UEFI bridge that runs BOOTIA32.EFI) + boot sectors"
$nasm = Get-Command nasm -ErrorAction SilentlyContinue
if ($clang -and $nasm) {
    New-Item -ItemType Directory -Force build\bios | Out-Null
    $b32 = @("--target=i686-unknown-none-elf", "-march=i686", "-O2", "-ffreestanding", "-fno-pic", "-fno-stack-protector", "-mno-sse", "-mno-mmx",
             "-malign-double", "-fshort-wchar", "-fno-strict-aliasing") + $warn
    # compile with clang, link with ld.lld itself: for a bare ELF target the clang driver may hand the link
    # to gcc, which runs lld in its MinGW (PE) mode and rejects -T (that broke the CI build)
    $lld = Join-Path (Split-Path ([string]$(if ($clang -is [string]) { $clang } else { $clang.Source }))) "ld.lld.exe"
    if (-not (Test-Path $lld)) { $lld = (Get-Command ld.lld -ErrorAction Stop).Source }
    Run $clang ($b32 + @("-c", "-o", "build\bios\bridge.o", "src\platform\bios\bridge.c"))
    Run $clang ($b32 + @("-c", "-o", "build\bios\fat32.o", "src\core\fat32.c"))
    Run $lld @("-T", "src/platform/bios/bridge.ld", "--oformat=binary", "-o", "build\bios\bridge.bin", "build\bios\bridge.o", "build\bios\fat32.o")
    Run $nasm @("-f", "bin", "-DMBR", "src\platform\bios\boot.asm", "-o", "build\bios\mbr.bin")
    Run $nasm @("-f", "bin", "-DVBR", "src\platform\bios\boot.asm", "-o", "build\bios\vbr.bin")
    Run $nasm @("-f", "bin", "-i", "build/bios/", "src\platform\bios\stub.asm", "-o", "build\bios\BIOS.SYS")
    Run $nasm @("-f", "bin", "tools\fakeother.asm", "-o", "build\fakeother.bin")   # the "other system" MBR of the phase 3 tests
    Run gcc @("-O2", "-Wall", "-Wextra", "tools\biosboot.c", "-o", "build\biosboot.exe")
    Run gcc @("-O2", "-Wall", "-Wextra", "tools\fat32_test.c", "src\core\fat32.c", "src\core\part.c", "-o", "build\fat32_test.exe")
    Run gcc @("-O2", "-Wall", "-Wextra", "tools\part_test.c", "src\core\part.c", "src\core\fat32.c", "-o", "build\part_test.exe")
    Run gcc @("-O2", "-Wall", "-Wextra", "tools\install_test.c", "src\core\install.c", "src\core\part.c", "src\core\fat32.c", "src\core\fscheck.c", "-o", "build\install_test.exe")
    Run gcc @("-O2", "-Wall", "-Wextra", "tools\fscheck_test.c", "src\core\fscheck.c", "src\core\part.c", "src\core\fat32.c", "-o", "build\fscheck_test.exe")
    Run gcc @("-O2", "-Wall", "tools\fatcat.c", "-o", "build\fatcat.exe")
} else {
    Write-Host "      clang (LLVM) and nasm are needed: BIOS boot not built" -ForegroundColor Yellow
}

Step 3 "Windows simulator -> build\folio_sim.exe"
Run gcc (@("-O2", "-g", "-fno-strict-aliasing", "-Isrc/core") + $warn + $core + @("src\platform\win32\win32_main.c", "-o", "build\folio_sim.exe", "-lgdi32", "-luser32", "-lwinmm"))

Step 4 "UEFI host (runs the real BOOTX64.EFI on Windows) -> build\efi_host.exe"
Run gcc @("-O2", "-Wall", "-Wno-cast-function-type", "tools\efi_host.c", "-o", "build\efi_host.exe", "-lgdi32", "-luser32")

Step 5 "Translations: template (assets\i18n\folio.pot) and check of every language"
Run gcc @("-O2", "-Wall", "tools\i18n.c", "-o", "build\i18n.exe")
$srcs = Get-ChildItem src\core\*.c | ForEach-Object { "src/core/" + $_.Name }
Run .\build\i18n.exe (@("extract", "build\folio.pot") + $srcs) | Out-Null
$oldPot = if (Test-Path assets\i18n\folio.pot) { (Get-Content -Raw assets\i18n\folio.pot) } else { "" }
if ($oldPot -ne (Get-Content -Raw build\folio.pot)) {
    Copy-Item -Force build\folio.pot assets\i18n\folio.pot
    Write-Host "      interface texts changed: assets\i18n\folio.pot updated (run: build\i18n.exe update assets\i18n\folio.pot assets\i18n\*.po)" -ForegroundColor Yellow
}
Run .\build\i18n.exe (@("check", "assets\i18n\folio.pot") + (Get-ChildItem assets\i18n\*.po | ForEach-Object { $_.FullName }))
# README in every language: same content as README.md (tools/readmes.pl); warn when a block is still in English
$perl = Get-Command perl -ErrorAction SilentlyContinue
if (-not $perl -and (Test-Path "$env:ProgramFiles\Git\usr\bin\perl.exe")) { $perl = "$env:ProgramFiles\Git\usr\bin\perl.exe" }
if ($perl) {
    $ErrorActionPreference = "Continue"
    $rd = & $perl tools\readmes.pl --check 2>&1 | ForEach-Object { "$_" }
    $rdok = $LASTEXITCODE -eq 0
    $ErrorActionPreference = "Stop"
    if ($rdok) { Write-Host "      README: 15 languages with the same content as README.md" }
    else { $rd | ForEach-Object { Write-Host "      $_" -ForegroundColor Yellow }; Write-Host "      README translations out of date: run perl tools\readmes.pl and translate the new blocks" -ForegroundColor Yellow }
}
# negative control: the checker must reject a translation whose %s/%d do not match the original
$bad = "build\i18n_bad.po"
(Get-Content -Raw -Encoding UTF8 assets\i18n\es.po) -replace 'msgstr "Tema: %s"', 'msgstr "Tema: %d"' | Set-Content -Encoding UTF8 $bad
$ErrorActionPreference = "Continue"
& .\build\i18n.exe check assets\i18n\folio.pot $bad 2>&1 | Out-Null
$rejected = $LASTEXITCODE -ne 0
$ErrorActionPreference = "Stop"
if (-not $rejected) { throw "i18n check accepted a broken format (negative control failed)" }
Remove-Item $bad
Ok "translations: formats valid in every language; the checker rejects broken formats"

Step 6 "USB drive folder -> build\usb_files"
$usb = "build\usb_files"
# staging folders from older builds used Portuguese names: move their content to the new folders
foreach ($p in @(@("TEXTOS", "DOCUMENTS"), @("MUSICA", "MUSIC"), @("FOLIO\TEMAS", "FOLIO\THEMES"), @("FOLIO\ABAS", "FOLIO\TABS"), @("FOLIO\VERSOES", "FOLIO\VERSIONS"))) {
    if (Test-Path "$usb\$($p[0])") {
        New-Item -ItemType Directory -Force "$usb\$($p[1])" | Out-Null
        Get-ChildItem "$usb\$($p[0])" | Where-Object { $_.Name -notin @("LEIA-ME.txt", "Bem-vindo.md", "Tinta azul.INI") -and -not (Test-Path -LiteralPath "$usb\$($p[1])\$($_.Name)") } | Move-Item -Destination "$usb\$($p[1])"
        Remove-Item -Recurse -Force "$usb\$($p[0])"
    }
}
foreach ($x in "$usb\FOLIO\FONTES", "$usb\LEIA-ME.txt", "$usb\COMO-INSTALAR.txt", "$usb\FOLIO\RASCUNHO.MD", "$usb\FOLIO\DIAGNOSTICO.TXT") { if (Test-Path $x) { Remove-Item -Recurse -Force $x } }
# only system files are refreshed: documents and music placed in DOCUMENTS and MUSIC are kept
foreach ($d in "$usb\EFI", "$usb\FOLIO\FONTS", "$usb\FOLIO\LANGUAGES", "$usb\FOLIO\KEYBOARDS", "$usb\FOLIO\TOOLS") { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
New-Item -ItemType Directory -Force "$usb\EFI\BOOT", "$usb\FOLIO\FONTS", "$usb\FOLIO\LANGUAGES", "$usb\FOLIO\KEYBOARDS",
    "$usb\FOLIO\THEMES", "$usb\DOCUMENTS", "$usb\MUSIC" | Out-Null
Copy-Item build\efi\BOOTX64.EFI, build\efi\BOOTIA32.EFI "$usb\EFI\BOOT\"
if (Test-Path build\efi\BOOTAA64.EFI) { Copy-Item build\efi\BOOTAA64.EFI "$usb\EFI\BOOT\" }
# BIOS-only PCs: the bridge, plus the Windows tool that writes the boot code (FOLIO\TOOLS\biosboot.exe E:)
if (Test-Path build\bios\BIOS.SYS) {
    Copy-Item build\bios\BIOS.SYS "$usb\FOLIO\BIOS.SYS"
    New-Item -ItemType Directory -Force "$usb\FOLIO\TOOLS\bios" | Out-Null
    Copy-Item build\biosboot.exe "$usb\FOLIO\TOOLS\"
    Copy-Item build\bios\mbr.bin, build\bios\vbr.bin "$usb\FOLIO\TOOLS\bios\"
}
Copy-Item assets\fonts\*.ttf, assets\fonts\OFL-*.txt "$usb\FOLIO\FONTS\"
# every language works out of the box: the writing fonts and the CJK fonts are part of the system
if (-not (Test-Path "assets\fonts-cjk\_LANGUAGE\NotoSansSC.ttf")) { Write-Host "      downloading the Chinese/Japanese/Korean fonts (once)"; & .\tools\fetch-cjk-fonts.ps1 }
foreach ($cjk in "NotoSansSC.ttf", "NotoSansJP.ttf", "NotoSansKR.ttf") { if (-not (Test-Path "assets\fonts-cjk\_LANGUAGE\$cjk")) { throw "missing assets\fonts-cjk\_LANGUAGE\$cjk (run .\tools\fetch-cjk-fonts.ps1)" } }
Copy-Item -Recurse assets\fonts-extras "$usb\FOLIO\FONTS\EXTRA"
Copy-Item -Recurse assets\fonts-cjk\_LANGUAGE "$usb\FOLIO\FONTS\_LANGUAGE"
Copy-Item assets\i18n\*.po "$usb\FOLIO\LANGUAGES\"
Copy-Item assets\keyboards\*.txt "$usb\FOLIO\KEYBOARDS\"
Copy-Item docs\README.txt, docs\LEIA-ME.txt, docs\INSTALL.txt "$usb\"
Copy-Item docs\MUSIC-README.txt "$usb\MUSIC\README.txt"
if (-not (Test-Path "$usb\DOCUMENTS\Welcome.md")) { Copy-Item docs\Welcome.md "$usb\DOCUMENTS\Welcome.md" }
# example theme: only when missing, so a theme edited by the user is never overwritten
foreach ($th in Get-ChildItem assets\themes\*.INI) { if (-not (Test-Path -LiteralPath "$usb\FOLIO\THEMES\$($th.Name)")) { Copy-Item -LiteralPath $th.FullName "$usb\FOLIO\THEMES\" } }

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# ZIP with "/" in names (PS 5.1 CreateFromDirectory writes "\"): opens correctly on Windows, macOS and Linux
function Write-Zip($srcRoot, $zipPath, $prefix) {
    if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
    $rootFull = (Resolve-Path $srcRoot).Path
    $za = [System.IO.Compression.ZipFile]::Open((Join-Path (Resolve-Path (Split-Path $zipPath)).Path (Split-Path $zipPath -Leaf)), [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($fi in Get-ChildItem -Recurse -File $rootFull) {
            $name = $prefix + $fi.FullName.Substring($rootFull.Length + 1).Replace('\', '/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $fi.FullName, $name, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $za.Dispose() }
}
# distribution package: the system only (no user documents/music), to unzip at the root of a FAT32 drive
$stage = "build\zip_stage"; $zip = "build\FolioOS.zip"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
Copy-Item -Recurse $usb $stage
Get-ChildItem "$stage\DOCUMENTS" -Exclude Welcome.md | Remove-Item -Recurse -Force
Get-ChildItem "$stage\MUSIC" -Exclude README.txt | Remove-Item -Recurse -Force
foreach ($x in "CONFIG.INI", "DIAGNOSTICS.TXT", "DRAFT.MD", "TABS", "VERSIONS") { if (Test-Path "$stage\FOLIO\$x") { Remove-Item -Recurse -Force "$stage\FOLIO\$x" } }
Write-Zip $stage $zip ""
# the disk image is built from this same clean copy: never from build\usb_files, which keeps the
# developer's own documents and music between builds
if (-not ($Image -or $Vhd)) { Remove-Item -Recurse -Force $stage }
Ok ("distribution package: $zip ({0:N1} MB) - unzip at the root of a FAT32 USB drive" -f ((Get-Item $zip).Length / 1MB))
# the separate font packs of 4.1 are now part of FolioOS.zip
foreach ($old in "build\FOLIO-FONTS-EXTRA.zip", "build\FOLIO-FONTS-CJK.zip") { if (Test-Path $old) { Remove-Item -Force $old } }
$size = (Get-Item build\efi\BOOTX64.EFI).Length / 1KB
Ok ("BOOTX64.EFI {0:N0} KB - fonts {1:N1} MB - {2} languages - {3} keyboards" -f $size, ((Get-ChildItem -Recurse "$usb\FOLIO\FONTS\*.ttf" | Measure-Object Length -Sum).Sum / 1MB), ((Get-ChildItem assets\i18n\*.po).Count + 1), (Get-ChildItem assets\keyboards\*.txt -Exclude _*).Count)

if ($Legacy) {
    Step "L" "Legacy FOLIO-86 v3 (BIOS) -> build\legacy"
    Push-Location
    New-Item -ItemType Directory -Force build\legacy | Out-Null
    Run gcc @("-O2", "tools\legacy\gen_fonts.c", "-o", "build\legacy\gen_fonts.exe")
    Run .\build\legacy\gen_fonts.exe @()
    Run nasm @("-f", "bin", "src\legacy\uefi_bootx64.asm", "-o", "build\bootx64.efi")
    Run nasm @("-f", "bin", "src\legacy\boot.asm", "-o", "build\boot.bin")
    Run nasm @("-f", "bin", "src\legacy\kernel.asm", "-o", "build\kernel.bin")
    Run nasm @("-f", "bin", "src\legacy\disk_image.asm", "-o", "build\legacy\folio86.img")
    Pop-Location
}

if (-not $NoTests) {
    Step 7 "Automated tests"
    $t = "build\test_root"
    function Fresh { if (Test-Path $t) { Remove-Item -Recurse -Force $t }; Copy-Item -Recurse $usb $t; Remove-Item -Recurse -Force "$t\DOCUMENTS\*", "$t\MUSIC\*.mp3", "$t\FOLIO\CONFIG.INI" -ErrorAction SilentlyContinue }
    # the original test scripts drive the Portuguese interface: the "firmware" says pt-BR, which also
    # exercises automatic language detection (and the ABNT2 keyboard that pt-BR.po asks for)
    $env:FOLIO_PLATFORM_LANG = "pt-BR"
    Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\smoke.txt", "--out", "build\shots")
    Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\full.txt", "--out", "build\shots")
    # exported files: ZIP structure + well-formed XML, language tag from the interface language
    $z = "build\test_root\_zip"
    foreach ($f in "docx", "odt", "epub") {
        Copy-Item "$t\DOCUMENTS\teste.$f" "$z.$f.zip" -Force
        Expand-Archive -Force "$z.$f.zip" "$z\$f"
        Get-ChildItem -Recurse "$z\$f" -Include *.xml, *.xhtml, *.opf | ForEach-Object { [xml](Get-Content -Raw -Encoding UTF8 -LiteralPath $_.FullName) | Out-Null }
    }
    if (-not (Select-String -Quiet -Path "$z\epub\OEBPS\*.opf", "$z\epub\*.opf" -Pattern "<dc:language>pt-BR</dc:language>" -ErrorAction SilentlyContinue)) {
        if (-not (Get-ChildItem -Recurse "$z\epub" -Include *.opf | Select-String -Quiet "<dc:language>pt-BR</dc:language>")) { throw "EPUB without the interface language tag" }
    }
    Ok "DOCX/ODT/EPUB exports: valid structure and XML, language tag pt-BR"
    Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\versoes.txt", "--out", "build\shots")
    if (-not (Get-ChildItem "$t\FOLIO\VERSIONS\*.md" -ErrorAction SilentlyContinue)) { throw "automatic versions were not created" }
    Ok "automatic versions (FOLIO\VERSIONS): OK"
    Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\leitura.txt", "--out", "build\shots") | Out-Null
    if (-not (Select-String -Quiet -Path "$t\FOLIO\CONFIG.INI" -Pattern "^position1=")) { throw "per-document position was not saved" }
    Ok "reading and writing: chapters, read/write sync, per-document position, editing - OK"
    Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\abas.txt", "--out", "build\shots") | Out-Null
    if (-not (Select-String -Quiet -Path "$t\FOLIO\CONFIG.INI" -Pattern "^tabs=1")) { throw "tabs were not saved in CONFIG.INI" }
    Ok "tabs: on/off, new, switch, close, reopen after restart (untitled too) - OK"
    Fresh
    Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\temas.txt", "--out", "build\shots") | Out-Null
    Ok "user themes (colors + fonts), theme editor, FolioOS splash - OK"
    # LaTeX sources: the fixture is CRLF (tests\*.txt are always CRLF, see .gitattributes); after the
    # script the autosaved file must match tests\latex_expected.txt byte for byte
    Fresh; Copy-Item tests\latex_doc.txt "$t\DOCUMENTS\artigo.tex"
    Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\latex.txt", "--out", "build\shots") | Out-Null
    $got = [IO.File]::ReadAllBytes((Resolve-Path "$t\DOCUMENTS\artigo.tex").Path)
    $want = [IO.File]::ReadAllBytes((Resolve-Path "tests\latex_expected.txt").Path)
    if ([Convert]::ToBase64String($got) -ne [Convert]::ToBase64String($want)) { throw "autosaved DOCUMENTS\artigo.tex differs from tests\latex_expected.txt" }
    if (-not (Test-Path "$t\DOCUMENTS\novo.tex") -or (Test-Path "$t\DOCUMENTS\novo.tex.md")) { throw "saving as novo.tex did not keep the extension" }
    if ([IO.File]::ReadAllBytes((Resolve-Path "$t\DOCUMENTS\novo.tex").Path) -contains 13) { throw "a new .tex must be written with LF line endings" }
    if (Test-Path "$t\DOCUMENTS\artigo.html") { throw "a LaTeX source was exported through the Markdown converter" }
    if (-not (Get-ChildItem "$t\FOLIO\VERSIONS\*.tex" -ErrorAction SilentlyContinue)) { throw "no automatic version of the .tex file" }
    Ok "LaTeX sources: raw text, highlighting, CRLF kept on autosave, save as .tex, no reading/export - OK"
    Fresh; Run .\build\efi_host.exe @("build\efi\BOOTX64.EFI", "--root", $t, "--script", "tests\efi_boot.txt", "--out", "build\shots")
    if (-not (Test-Path "$t\DOCUMENTS\uefi_teste.md")) { throw "BOOTX64.EFI did not write the test file" }
    Ok "real BOOTX64.EFI: boot, PlatformLang pt-BR, ABNT2 typing, saving to the volume - OK"
    Fresh; Run .\build\efi_host.exe @("build\efi\BOOTX64.EFI", "--root", $t, "--script", "tests\efi_roda.txt", "--out", "build\shots") | Out-Null
    Ok "real BOOTX64.EFI: fake wheel blocked, slow wheel free, left+right = middle, Esc stops - OK"
    # languages: English by default (no firmware language), switching, plurals, keyboards, CJK fonts,
    # old Portuguese installs keep Portuguese
    Remove-Item Env:\FOLIO_PLATFORM_LANG
    Fresh
    Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\i18n.txt", "--out", "build\shots") | Out-Null
    if (-not (Select-String -Quiet -Path "$t\FOLIO\CONFIG.INI" -Pattern "^language=")) { throw "language was not saved in CONFIG.INI" }
    Fresh; Remove-Item -Recurse -Force "$t\FOLIO\FONTS\_LANGUAGE" -ErrorAction SilentlyContinue
    Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\i18n_cjk_missing.txt", "--out", "build\shots") | Out-Null
    Fresh; Set-Content -Encoding ascii "$t\FOLIO\CONFIG.INI" "tema=Papiro`r`nteclado=abnt2`r`nabertura=0`r`n"
    Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\i18n_legacy.txt", "--out", "build\shots") | Out-Null
    if (-not (Select-String -Quiet -Path "$t\FOLIO\CONFIG.INI" -Pattern "^theme=Papyrus")) { throw "old Portuguese CONFIG.INI was not migrated to the new keys" }
    Ok "languages: English default, 14 translations, plurals (ru/pl), keyboards, CJK fonts, old installs stay in Portuguese - OK"
    # no mouse at all: the mouse is turned off and every window is used with Tab, arrows, Enter and Space
    Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\teclado.txt", "--out", "build\shots") | Out-Null
    if (-not (Select-String -Quiet -Path "$t\FOLIO\CONFIG.INI" -Pattern "^use_mouse=0")) { throw "turning the mouse off was not saved in CONFIG.INI" }
    if (-not (Select-String -Quiet -Path "$t\FOLIO\CONFIG.INI" -Pattern "^typewriter=1")) { throw "a setting changed with the keyboard was not saved" }
    Ok "keyboard only: mouse off, menu, files, settings, themes, outline, Ctrl+F6, dialogs, reading - OK"
    # input methods: Korean Hangul composition and Japanese romaji to kana (CJK fonts are on every drive)
    Fresh
    Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\ime.txt", "--out", "build\shots") | Out-Null
    Ok "input methods: Korean (Hangul syllables, backspace by jamo), Japanese (romaji to hiragana/katakana) - OK"
    # universal video: pixel formats, Blt only, padded stride, UGA (no GOP), portrait panel with EDID
    $vs = "build\test_video.txt"; Set-Content -Encoding ascii $vs "wait 1200`nshot v.png`nquit"
    foreach ($g in @(@("--gop", "rgb"), @("--gop", "mask16"), @("--gop", "blt"), @("--gop", "pad"), @("--gop", "uga"), @("--size", "800x1280", "--edid"))) {
        Fresh; Run .\build\efi_host.exe (@("build\efi\BOOTX64.EFI", "--root", $t, "--script", $vs, "--out", "build\shots") + $g) | Out-Null
        if (-not (Select-String -Quiet -Path "$t\FOLIO\DIAGNOSTICS.TXT" -Pattern "in use: ")) { throw "video $($g -join ' '): no output configured" }
    }
    # keep proportions: a 4:3 mode on a 16:9 panel is drawn at 1024x576 and stretched to the mode
    # (framebuffer, Blt only, rotated); "monitor" with an EDID that matches the mode changes nothing
    foreach ($g in @(@(), @("--gop", "blt"), @("--gop", "mask16"))) {
        foreach ($r in @("auto", "90")) {
            Fresh; Set-Content -Encoding ascii "$t\FOLIO\CONFIG.INI" "keep_aspect=16:9`r`nrotation=$r`r`n"
            Run .\build\efi_host.exe (@("build\efi\BOOTX64.EFI", "--root", $t, "--script", $vs, "--out", "build\shots", "--size", "1024x768") + $g) | Out-Null
            if (-not (Select-String -Quiet -Path "$t\FOLIO\DIAGNOSTICS.TXT" -Pattern "keep proportions: drawn at 1024x576, stretched to 1024x768")) { throw "keep proportions $($g -join ' ') rotation $r`: not applied" }
            $ls = if ($r -eq "90") { "screen 576x1024" } else { "screen 1024x576" }
            if (-not (Select-String -Quiet -Path "$t\FOLIO\DIAGNOSTICS.TXT" -Pattern "logical $ls")) { throw "keep proportions $($g -join ' ') rotation $r`: logical screen is not $ls" }
        }
    }
    Copy-Item build\shots\v.png build\shots\proporcao.png
    Fresh; Set-Content -Encoding ascii "$t\FOLIO\CONFIG.INI" "keep_aspect=monitor`r`n"
    Run .\build\efi_host.exe @("build\efi\BOOTX64.EFI", "--root", $t, "--script", $vs, "--out", "build\shots", "--size", "1024x768", "--edid") | Out-Null
    if (Select-String -Quiet -Path "$t\FOLIO\DIAGNOSTICS.TXT" -Pattern "keep proportions") { throw "keep proportions: changed the picture although the mode matches the monitor" }
    Remove-Item $vs
    Ok "video: RGB, 16-bit mask, Blt only, padded stride, UGA, rotated portrait; keep proportions (4:3 mode on 16:9) - OK"
    # FAT32 driver of the BIOS bridge: random create/write/truncate/rename/delete vs. a model, then a full volume check
    if (Test-Path build\fat32_test.exe) {
        Run gcc @("-O2", "-Wall", "tools\mkimg.c", "-o", "build\mkimg.exe")
        # disk images can take a few GB: FOLIO_WORK=<folder on another drive> keeps them off the system drive
        $ft = if ($env:FOLIO_WORK) { "$env:FOLIO_WORK\fat32t" } else { "build\fat32t" }; if (Test-Path $ft) { Remove-Item -Recurse -Force $ft }
        New-Item -ItemType Directory -Force "$ft\seed\Docs" | Out-Null
        Set-Content -Encoding ascii "$ft\seed\seed.txt" "seed"; Set-Content -Encoding ascii "$ft\seed\Docs\old notes.md" "old"
        foreach ($cfg in @(@(64, 7), @(64, 21), @(1200, 99))) {
            Run .\build\mkimg.exe @("$ft\seed", "$ft\t.img", "$($cfg[0])") | Out-Null
            Run .\build\fat32_test.exe @("$ft\t.img", "4000", "$($cfg[1])") | Out-Null
        }
        Remove-Item -Recurse -Force $ft
        Ok "FAT32 driver (BIOS bridge): 12000 random operations on 3 volumes, byte-exact, volume consistent - OK"
        # partition tables (GPT/MBR) and FAT32 format, then the installer on empty GPT and MBR disks
        New-Item -ItemType Directory -Force $ft | Out-Null
        Run .\build\part_test.exe @($ft) | Out-Null
        foreach ($i in "gpt", "mbr") { Run .\build\fat32_test.exe @("$ft\$i.img", "--fsck") | Out-Null }
        Copy-Item -Recurse build\usb_files "$ft\src"
        # a table FolioOS cannot read or would not rewrite exactly (unreadable GPT, MBR with an extended partition):
        # the partition manager refuses before writing anything, and the disk stays byte for byte the same
        foreach ($c in @(@("gpt256.img", "--op-create", "1", "20", "X"), @("mbrext.img", "--op-format", "1", "X"), @("mbrext.img", "--op-delete", "1"))) {
            $h0 = (Get-FileHash "$ft\$($c[0])").Hash
            $ErrorActionPreference = "Continue"; $null = & .\build\install_test.exe (@("$ft\src", "$ft\$($c[0])", "64") + $c[1..9]) 2>&1; $ec = $LASTEXITCODE; $ErrorActionPreference = "Stop"
            if ($ec -ne 1) { throw "partition manager on $($c[0]) $($c[1]): expected a refusal, exit $ec" }
            if ((Get-FileHash "$ft\$($c[0])").Hash -ne $h0) { throw "partition manager on $($c[0]) $($c[1]): the disk changed although the table is locked" }
        }
        foreach ($k in @(@("inst-uefi"), @("inst-bios", "--bios"))) {
            Run .\build\install_test.exe (@("$ft\src", "$ft\$($k[0]).img", "600") + $k[1..9]) | Out-Null
            Run .\build\fat32_test.exe @("$ft\$($k[0]).img", "--fsck") | Out-Null
        }
        if (@(Get-ChildItem "$ft\src\FOLIO\BACKUP\disk-*.bin").Count -ne 2) { throw "installer: safety copies missing from the USB drive" }
        # phase 2: next to Windows (EFI, MSR, a hole in the table, C:, recovery at the end): only one new entry,
        # every other entry and partition byte-for-byte the same, \EFI\FolioOS on the EFI partition, both volumes consistent
        Run .\build\install_test.exe @("$ft\src", "$ft\win.img", "1400", "--windows") | Out-Null
        Run .\build\install_test.exe @("$ft\src", "$ft\win.img", "1400", "--alongside") | Out-Null
        Run .\build\fat32_test.exe @("$ft\win.img", "--fsck") | Out-Null
        Remove-Item "$ft\win.img"
        # partitions of other systems: the NTFS reader against volumes made by Windows itself (tools\ntfs-fixtures.ps1),
        # with broken copies as negative controls, plus BitLocker/LUKS/LVM/Mac/ext4 verdicts on 512- and 4096-byte sectors
        # ext4 volumes made by Linux itself (tools\ext-fixtures.ps1: Alpine in QEMU): clean, 1 KiB blocks, errors, journal pending
        foreach ($n in "ntfs-clean", "ntfs-dirty", "ntfs-hiber", "ext-clean", "ext-small", "ext-errors", "ext-recover", "ext-linux") {
            $gi = [IO.File]::OpenRead("tests\fixtures\$n.img.gz"); $gz = New-Object IO.Compression.GZipStream($gi, [IO.Compression.CompressionMode]::Decompress)
            $go = [IO.File]::Create("$ft\$n.img"); $gz.CopyTo($go); $go.Dispose(); $gz.Dispose(); $gi.Dispose()
        }
        Run .\build\fscheck_test.exe @($ft, "tests\fixtures") | Out-Null
        # a PC with Linux and no free space (EFI + ext4 made by Linux): FolioOS does not shrink ext4, so the screen
        # offers the whole disk and explains how to make room from Linux; nothing is written
        $before = (Get-FileHash "$ft\ext-linux.img").Hash
        $ol = $env:FOLIO_PLATFORM_LANG; $env:FOLIO_PLATFORM_LANG = "pt-BR"; $env:FOLIO_DISKS = "$ft\ext-linux.img"
        try { Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\instalar_linux.txt", "--out", "build\shots") | Out-Null }
        finally { $env:FOLIO_DISKS = $null; $env:FOLIO_PLATFORM_LANG = $ol }
        if ((Get-FileHash "$ft\ext-linux.img").Hash -ne $before) { throw "installer: the Linux disk changed just by opening the installer" }
        Remove-Item "$ft\ntfs-*.img", "$ft\ext-*.img"
        Ok "other systems' partitions: NTFS read as Windows reports it, ext4 as Linux reports it; BitLocker/LUKS/LVM/Mac/FAT refusals - OK"
        # the real BOOTX64.EFI lists the computer's disks (Block I/O) and reads their partition tables
        $xf = [IO.File]::Create("$ft\empty.img"); $xf.SetLength(100MB); $xf.Close()
        Fresh; Run .\build\efi_host.exe @("build\efi\BOOTX64.EFI", "--root", $t, "--script", "tests\efi_boot.txt", "--out", "build\shots",
                                          "--disk", "$ft\inst-uefi.img", "--disk", "$ft\inst-bios.img", "--disk", "$ft\empty.img") | Out-Null
        $dg = Get-Content -Raw -Encoding UTF8 "$t\FOLIO\DIAGNOSTICS.TXT"
        foreach ($w in "== Disks \(3 found\)", "SATA . FolioOS test disk 1 . 629 MB", "partition table: GPT", "EFI system . FAT32 . 627 MB . `"FOLIO`"",
                       "partition table: MBR", "FolioOS test disk 3 . 104 MB", "partition table: none \(empty disk\)") {
            if ($dg -notmatch $w) { throw "disk list in DIAGNOSTICS.TXT: missing '$w'" }
        }
        if ($dg.Contains("started from here")) { throw "disk list: an image disk was taken for the boot disk" }
        # update a whole-disk install (UEFI and BIOS) from the USB drive: the person's text and CONFIG.INI stay, the program
        # becomes the USB drive's, every file swapped only after its new copy was checked
        foreach ($k in "inst-uefi", "inst-bios") { Run .\build\install_test.exe @("$ft\src", "$ft\$k.img", "600", "--update") | Out-Null; Run .\build\fat32_test.exe @("$ft\$k.img", "--fsck") | Out-Null }
        Ok "update an installation (UEFI and BIOS): program replaced and checked, the person's texts and settings untouched - OK"
        # phase 3, a PC with BIOS only: next to "Windows" on an MBR disk (the old entries keep their positions and bytes, the
        # original MBR code kept in FOLIO\OTHERMBR.BIN), update, uninstall: the table and the MBR back to the original
        $wb = "$ft\winbios.img"; Remove-Item $wb -ErrorAction SilentlyContinue
        Run .\build\install_test.exe @("$ft\src", $wb, "1400", "--windows-bios", "build\fakeother.bin") | Out-Null
        Copy-Item $wb "$ft\winbios-orig.img"
        Run .\build\install_test.exe @("$ft\src", $wb, "1400", "--alongside", "--bios") | Out-Null
        Run .\build\fat32_test.exe @($wb, "--fsck") | Out-Null
        Run .\build\install_test.exe @("$ft\src", $wb, "1400", "--update", "--bios") | Out-Null
        Run .\build\install_test.exe @("$ft\src", $wb, "1400", "--remove") | Out-Null
        Run .\build\install_test.exe @("$ft\src", $wb, "1400", "--same-table", "$ft\winbios-orig.img") | Out-Null
        Remove-Item $wb, "$ft\winbios-orig.img"
        # and with no free space: an MBR disk with one FAT32 filling it is shrunk, FolioOS goes after it, then uninstalled
        Run .\build\install_test.exe @("$ft\src", $wb, "1400", "--mbr-fat32", "build\fakeother.bin") | Out-Null
        Copy-Item $wb "$ft\winbios-orig.img"
        Run .\build\install_test.exe @("$ft\src", $wb, "1400", "--shrink-ntfs", "--bios") | Out-Null
        Run .\build\fat32_test.exe @($wb, "--fsck") | Out-Null
        Run .\build\install_test.exe @("$ft\src", $wb, "1400", "--remove") | Out-Null
        Run .\build\install_test.exe @("$ft\src", $wb, "1400", "--same-table", "$ft\winbios-orig.img") | Out-Null
        Run .\build\fat32_test.exe @($wb, "--fsck") | Out-Null
        Remove-Item $wb, "$ft\winbios-orig.img"
        Get-ChildItem "$ft\src\DOCUMENTS" -Directory -Filter "computer-*" | Remove-Item -Recurse -Force
        Ok "next to Windows on a PC with BIOS only: MBR entries untouched, original MBR kept, update, uninstall back to the original; also by shrinking - OK"
        # where the texts live, chosen at install time: docs_place= in the installed CONFIG.INI (on the USB drive: no DOCUMENTS on the disk)
        foreach ($m in "--docs-stick", "--docs-both") { Run .\build\install_test.exe @("$ft\src", "$ft\docs.img", "600", $m) | Out-Null; Remove-Item "$ft\docs.img" }
        # uninstall a whole-disk install (UEFI and BIOS): texts to the USB drive, the disk left empty (no table)
        Copy-Item -Recurse "$ft\src" "$ft\srcr"
        foreach ($k in "inst-uefi", "inst-bios") { Run .\build\install_test.exe @("$ft\srcr", "$ft\$k.img", "600", "--remove") | Out-Null }
        if (@(Get-ChildItem "$ft\srcr\DOCUMENTS" -Directory -Filter "computer-*").Count -ne 2) { throw "uninstall (whole disk): texts not copied to the USB drive" }
        Remove-Item -Recurse -Force "$ft\srcr"
        Ok "uninstall a whole-disk install (UEFI and BIOS): texts copied to the USB drive and checked, disk left empty - OK"
        # the installer screen, keyboard only, in the simulator (pt-BR: the word is APAGAR): a disk too small
        # (never written) and a 600 MB one. First run: wrong words only, nothing may be written. Second run:
        # the right word in lowercase installs.
        function Zero1MB($p) { $b = New-Object byte[] 1MB; $s = [IO.File]::OpenRead($p); [void]$s.Read($b, 0, $b.Length); $s.Close(); [Linq.Enumerable]::SequenceEqual($b, (New-Object byte[] 1MB)) }
        foreach ($d in @(@("small", 100MB), @("target", 600MB))) { $xf = [IO.File]::Create("$ft\$($d[0]).img"); $xf.SetLength($d[1]); $xf.Close() }
        $oldLang = $env:FOLIO_PLATFORM_LANG; $env:FOLIO_PLATFORM_LANG = "pt-BR"; $env:FOLIO_DISKS = "$ft\small.img;$ft\target.img"
        try {
            Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\instalar_errado.txt", "--out", "build\shots") | Out-Null
            if (-not (Zero1MB "$ft\target.img") -or (Test-Path "$t\FOLIO\BACKUP")) { throw "installer screen: a wrong confirmation word wrote to the disk" }
            Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\instalar.txt", "--out", "build\shots") | Out-Null
        } finally { $env:FOLIO_DISKS = $null; $env:FOLIO_PLATFORM_LANG = $oldLang }
        Run .\build\fat32_test.exe @("$ft\target.img", "--fsck") | Out-Null
        & .\build\fatcat.exe "$ft\target.img" "EFI/BOOT/BOOTX64.EFI" "$ft\copied.efi" 2>$null
        if (-not (Test-Path "$ft\copied.efi") -or (Get-FileHash "$ft\copied.efi").Hash -ne (Get-FileHash "$t\EFI\BOOT\BOOTX64.EFI").Hash) { throw "installer screen: EFI\BOOT\BOOTX64.EFI not copied to the disk" }
        if (@(Get-ChildItem "$t\FOLIO\BACKUP\disk-*.bin" -ErrorAction SilentlyContinue).Count -ne 1) { throw "installer screen: no safety copy of the disk on the USB drive" }
        if (-not (Zero1MB "$ft\small.img")) { throw "installer screen: the disk that is too small was written to" }
        # updating it through the screen (from this USB drive): the version in FOLIO\INSTALLED.ID becomes this one
        $env:FOLIO_PLATFORM_LANG = "pt-BR"; $env:FOLIO_DISKS = "$ft\target.img"
        try { Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\atualizar.txt", "--out", "build\shots") | Out-Null }
        finally { $env:FOLIO_DISKS = $null; $env:FOLIO_PLATFORM_LANG = $oldLang }
        $ver = [regex]::Match((Get-Content -Raw src\core\folio.h), '#define FOLIO_VERSION "([^"]+)"').Groups[1].Value
        Remove-Item "$ft\INSTALLED.ID" -ErrorAction SilentlyContinue; & .\build\fatcat.exe "$ft\target.img" "FOLIO/INSTALLED.ID" "$ft\INSTALLED.ID" 2>$null
        if (-not (Test-Path "$ft\INSTALLED.ID") -or -not (Select-String -Quiet -Path "$ft\INSTALLED.ID" -SimpleMatch "version=$ver")) { throw "update screen: FOLIO\INSTALLED.ID does not have version=$ver" }
        Run .\build\fat32_test.exe @("$ft\target.img", "--fsck") | Out-Null
        # where the texts live (docs_place=): an installation with the texts on the USB drive saves a new text there;
        # "both" exchanges them with the USB drive (newest wins, the replaced version kept in FOLIO\VERSIONS)
        $stick = "$ft\stick"
        Fresh; if (Test-Path $stick) { Remove-Item -Recurse -Force $stick }; New-Item -ItemType Directory -Force "$stick\DOCUMENTS" | Out-Null
        Set-Content -Encoding ascii "$t\FOLIO\INSTALLED.ID" "FolioOS installed on this disk`r`nkind=whole`r`nversion=$ver"
        Set-Content -Encoding ascii "$t\FOLIO\CONFIG.INI" "docs_place=usb"
        $env:FOLIO_PLATFORM_LANG = "pt-BR"
        try { Run .\build\folio_sim.exe @("--root", $t, "--vol2", $stick, "--script", "tests\textos_pendrive.txt", "--out", "build\shots") | Out-Null }
        finally { $env:FOLIO_PLATFORM_LANG = $oldLang }
        if (-not (Select-String -Quiet -Path "$stick\DOCUMENTS\*.md" -SimpleMatch "texto que mora no pendrive")) { throw "docs_place=usb: the new text was not saved on the USB drive" }
        Fresh; Remove-Item -Recurse -Force $stick; New-Item -ItemType Directory -Force "$stick\DOCUMENTS", "$t\DOCUMENTS" | Out-Null
        Set-Content -Encoding ascii "$t\FOLIO\INSTALLED.ID" "FolioOS installed on this disk`r`nkind=whole`r`nversion=$ver"
        Set-Content -Encoding ascii "$t\FOLIO\CONFIG.INI" "docs_place=both"
        Set-Content -Encoding utf8 "$t\DOCUMENTS\a.md" "a do computador"; Set-Content -Encoding utf8 "$stick\DOCUMENTS\b.md" "b do pendrive"
        Set-Content -Encoding utf8 "$t\DOCUMENTS\c.md" "c do computador"; Set-Content -Encoding utf8 "$stick\DOCUMENTS\c.md" "c do pendrive"
        (Get-Item "$t\DOCUMENTS\c.md").LastWriteTime = [datetime]"2026-01-01 10:00"; (Get-Item "$stick\DOCUMENTS\c.md").LastWriteTime = [datetime]"2026-06-01 10:00"
        $env:FOLIO_PLATFORM_LANG = "pt-BR"
        try { Run .\build\folio_sim.exe @("--root", $t, "--vol2", $stick, "--script", "tests\trocar.txt", "--out", "build\shots") | Out-Null }
        finally { $env:FOLIO_PLATFORM_LANG = $oldLang }
        if (-not (Test-Path "$t\DOCUMENTS\b.md") -or -not (Test-Path "$stick\DOCUMENTS\a.md")) { throw "exchange: a text that was only on one side was not copied to the other" }
        if ((Get-Content -Raw "$t\DOCUMENTS\c.md") -notmatch "c do pendrive" -or (Get-Content -Raw "$stick\DOCUMENTS\c.md") -notmatch "c do pendrive") { throw "exchange: the newest version of c.md did not win" }
        if (-not (Select-String -Quiet -Path "$t\FOLIO\VERSIONS\*" -SimpleMatch "c do computador")) { throw "exchange: the replaced version was not kept in FOLIO\VERSIONS" }
        Remove-Item -Recurse -Force $stick
        Ok "where the texts live: on the USB drive (a new text is saved there), or on both (exchange: newest wins, old version kept) - OK"
        # and removing it through the screen (the confirmation word is always asked: the disk is left empty)
        $env:FOLIO_PLATFORM_LANG = "pt-BR"; $env:FOLIO_DISKS = "$ft\target.img"
        try { Fresh; $out = & .\build\folio_sim.exe --root $t --script tests\remover_disco.txt --out build\shots 2>&1 | Out-String; $rc = $LASTEXITCODE }
        finally { $env:FOLIO_DISKS = $null; $env:FOLIO_PLATFORM_LANG = $oldLang }
        if ($rc -ne 0 -or $out -notmatch "boot entry removed" -or -not (Zero1MB "$ft\target.img")) { throw "uninstall screen (whole disk): the disk was not left empty: $out" }
        # phase 2 on the screen: next to "Windows" (the option comes first, nothing to type), then nothing else changed
        Run .\build\install_test.exe @("$ft\src", "$ft\winsim.img", "1400", "--windows") | Out-Null
        Copy-Item "$ft\winsim.img" "$ft\winsim-before.img"
        $env:FOLIO_PLATFORM_LANG = "pt-BR"; $env:FOLIO_DISKS = "$ft\winsim.img"
        try { Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\instalar_lado.txt", "--out", "build\shots") | Out-Null }
        finally { $env:FOLIO_DISKS = $null; $env:FOLIO_PLATFORM_LANG = $oldLang }
        Run .\build\install_test.exe @("$ft\src", "$ft\winsim.img", "1400", "--compare", "$ft\winsim-before.img") | Out-Null
        Run .\build\fat32_test.exe @("$ft\winsim.img", "--fsck") | Out-Null
        & .\build\fatcat.exe "$ft\winsim.img" "EFI/FolioOS/FOLIO.ID" "$ft\FOLIO.ID" 2>$null
        if (-not (Test-Path "$ft\FOLIO.ID") -or -not (Select-String -Quiet -Path "$ft\FOLIO.ID" -Pattern "^data=[0-9A-F-]{36}")) { throw "installer next to Windows: EFI\FolioOS\FOLIO.ID missing" }
        Remove-Item "$ft\winsim.img", "$ft\winsim-before.img"
        # BitLocker on the other system: without "I have the recovery key" nothing is written; with it, installs
        Run .\build\install_test.exe @("$ft\src", "$ft\winsim.img", "1400", "--windows", "--bitlocker") | Out-Null
        Copy-Item "$ft\winsim.img" "$ft\winsim-before.img"
        $env:FOLIO_PLATFORM_LANG = "pt-BR"; $env:FOLIO_DISKS = "$ft\winsim.img"
        try {
            Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\instalar_bitlocker_nao.txt", "--out", "build\shots") | Out-Null
            if ((Get-FileHash "$ft\winsim.img").Hash -ne (Get-FileHash "$ft\winsim-before.img").Hash) { throw "installer: wrote to a BitLocker disk without the recovery key confirmed" }
            Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\instalar_bitlocker.txt", "--out", "build\shots") | Out-Null
        } finally { $env:FOLIO_DISKS = $null; $env:FOLIO_PLATFORM_LANG = $oldLang }
        Run .\build\install_test.exe @("$ft\src", "$ft\winsim.img", "1400", "--compare", "$ft\winsim-before.img") | Out-Null
        & .\build\fatcat.exe "$ft\winsim.img" "EFI/FolioOS/FOLIO.ID" "$ft\FOLIO.ID2" 2>$null
        if (-not (Test-Path "$ft\FOLIO.ID2")) { throw "installer next to BitLocker: not installed after the recovery key was confirmed" }
        Remove-Item "$ft\winsim.img", "$ft\winsim-before.img"
        Ok "installer next to BitLocker: nothing written until the recovery key is confirmed - OK"
        # no free space: a disk made by Windows (EFI + NTFS, tests\fixtures\ntfs-big) - the engine shrinks the NTFS without
        # moving data and installs after it, then the same through the screen (preview, size with the arrow keys)
        function Expand-Big { $gi = [IO.File]::OpenRead("tests\fixtures\ntfs-big.img.gz"); $gz = New-Object IO.Compression.GZipStream($gi, [IO.Compression.CompressionMode]::Decompress)
                              $go = [IO.File]::Create("$ft\big.img"); $gz.CopyTo($go); $go.Dispose(); $gz.Dispose(); $gi.Dispose() }
        Expand-Big
        Run .\build\install_test.exe @("$ft\src", "$ft\big.img", "1100", "--shrink-ntfs") | Out-Null
        Run .\build\fat32_test.exe @("$ft\big.img", "--fsck") | Out-Null
        Expand-Big
        $env:FOLIO_PLATFORM_LANG = "pt-BR"; $env:FOLIO_DISKS = "$ft\big.img"
        try { Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\instalar_encolher.txt", "--out", "build\shots") | Out-Null }
        finally { $env:FOLIO_DISKS = $null; $env:FOLIO_PLATFORM_LANG = $oldLang }
        Run .\build\fscheck_test.exe @("--shrunk", "$ft\big.img", "983") | Out-Null
        Run .\build\fat32_test.exe @("$ft\big.img", "--fsck") | Out-Null
        Ok "installer with no free space: NTFS made by Windows shrunk without moving data, FolioOS installed after it (engine and screen) - OK"
        # FAT32 and exFAT made by Windows: shrink and grow without moving data (every file the same, the volume back byte
        # for byte), then the whole installer on a PC disk (EFI + FAT32/exFAT, no free space): shrink, install, uninstall
        foreach ($k in "fat32", "exfat") {
            foreach ($n in "data", "pc") {
                $gi = [IO.File]::OpenRead("tests\fixtures\$k-$n.img.gz"); $gz = New-Object IO.Compression.GZipStream($gi, [IO.Compression.CompressionMode]::Decompress)
                $go = [IO.File]::Create("$ft\$k-$n.img"); $gz.CopyTo($go); $go.Dispose(); $gz.Dispose(); $gi.Dispose()
            }
            Run .\build\fscheck_test.exe @("--$k", "$ft\$k-data.img") | Out-Null
            if ($k -eq "fat32") { Run .\build\fat32_test.exe @("$ft\$k-data.img", "--fsck") | Out-Null }
            Copy-Item "$ft\$k-pc.img" "$ft\$k-pc-orig.img"
            Run .\build\install_test.exe @("$ft\src", "$ft\$k-pc.img", "1100", "--shrink-ntfs") | Out-Null
            Run .\build\fat32_test.exe @("$ft\$k-pc.img", "--fsck") | Out-Null
            Run .\build\install_test.exe @("$ft\src", "$ft\$k-pc.img", "1100", "--remove") | Out-Null
            Run .\build\install_test.exe @("$ft\src", "$ft\$k-pc.img", "1100", "--same-table", "$ft\$k-pc-orig.img") | Out-Null
            Remove-Item "$ft\$k-data.img", "$ft\$k-pc.img", "$ft\$k-pc-orig.img"
            Get-ChildItem "$ft\src\DOCUMENTS" -Directory -Filter "computer-*" | Remove-Item -Recurse -Force   # the copied texts (keeps the test USB drive the same)
        }
        Ok "FAT32 and exFAT made by Windows: shrink and grow without moving data; install next to them and uninstall - OK"
        # partition manager, engine: create, grow, shrink, grow again, format, delete, grow into the freed space (FAT32 made
        # here gets a FAT with room to grow), every step consistent
        $pm = "$ft\pm.img"; Remove-Item $pm -ErrorAction SilentlyContinue
        foreach ($a in @(@("--op-create", "1", "300", "DADOS"), @("--op-create", "400", "200", "OUTRO"), @("--op-resize", "1", "380"), @("--op-resize", "2", "150"),
                         @("--op-resize", "2", "300"), @("--op-format", "2", "NOVO"), @("--op-delete", "1"), @("--op-resize", "1", "500"))) {
            Run .\build\install_test.exe (@("$ft\src", $pm, "1000") + $a) | Out-Null
            Run .\build\fat32_test.exe @($pm, "--fsck") | Out-Null
        }
        $l = (& .\build\install_test.exe "$ft\src" $pm 1000 --list) -join "`n"
        if ($l -notmatch "^1 819200 1024000 FAT32 NOVO$") { throw "partition manager (engine): unexpected table: $l" }
        # and the screen, keyboard only: create on an empty disk, shrink it with the slider, then delete it (word APAGAR)
        Remove-Item $pm; $xf = [IO.File]::Create($pm); $xf.SetLength(1000MB); $xf.Close()
        $ol = $env:FOLIO_PLATFORM_LANG; $env:FOLIO_PLATFORM_LANG = "pt-BR"; $env:FOLIO_DISKS = $pm
        try {
            Fresh; Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\particoes.txt", "--out", "build\shots") | Out-Null
            $l = @(& .\build\install_test.exe "$ft\src" $pm 1000 --list)
            if ($l.Count -ne 1 -or $l[0] -notmatch "^1 2048 (\d+) FAT32 DATA$" -or [int64]$Matches[1] -ge 2045952) { throw "partition manager (screen): expected one smaller FAT32 'DATA', got: $($l -join '; ')" }
            Run .\build\fat32_test.exe @($pm, "--fsck") | Out-Null
            Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\particoes_apagar.txt", "--out", "build\shots") | Out-Null
            $l = @(& .\build\install_test.exe "$ft\src" $pm 1000 --list | Where-Object { $_ -match "FAT32" })
            if ($l.Count) { throw "partition manager (screen): the partition was not deleted: $($l -join '; ')" }
        } finally { $env:FOLIO_DISKS = $null; $env:FOLIO_PLATFORM_LANG = $ol }
        Remove-Item $pm
        Ok "partition manager: create, resize, format, delete (engine), and create, shrink, delete on the screen - OK"
        # uninstall through the screen: texts to the USB drive, FolioOS's partition and \EFI\FolioOS gone, the NTFS back
        # to the size Windows made; the table and the NTFS boot sector must be byte for byte the original ones
        # (the disk installed by the engine from the full USB drive: it has texts in DOCUMENTS to copy back)
        Remove-Item "$ft\big.img"; Expand-Big; Copy-Item "$ft\big.img" "$ft\big-orig.img"
        Run .\build\install_test.exe @("$ft\src", "$ft\big.img", "1100", "--shrink-ntfs") | Out-Null
        Run .\build\install_test.exe @("$ft\src", "$ft\big.img", "1100", "--update") | Out-Null      # update next to Windows (\EFI\FolioOS too)
        $ol = $env:FOLIO_PLATFORM_LANG; $env:FOLIO_PLATFORM_LANG = "pt-BR"; $env:FOLIO_DISKS = "$ft\big.img"
        try { Fresh; $out = & .\build\folio_sim.exe --root $t --script tests\remover.txt --out build\shots 2>&1 | Out-String; $rc = $LASTEXITCODE }
        finally { $env:FOLIO_DISKS = $null; $env:FOLIO_PLATFORM_LANG = $ol }
        if ($rc -ne 0 -or $out -notmatch "boot entry removed") { throw "uninstall screen failed: $out" }
        Run .\build\install_test.exe @("$ft\src", "$ft\big.img", "1100", "--same-table", "$ft\big-orig.img") | Out-Null
        Run .\build\fat32_test.exe @("$ft\big.img", "--fsck") | Out-Null
        $copied = @(Get-ChildItem "$t\DOCUMENTS" -Directory -Filter "computer-*" -ErrorAction SilentlyContinue | Get-ChildItem -Recurse -File).Count
        if ($copied -lt 1) { throw "uninstall: the computer's texts were not copied to the USB drive" }
        # and the engine alone, on a fresh install (texts counted one by one, NTFS grown back, \EFI\FolioOS gone)
        Copy-Item "$ft\big-orig.img" "$ft\big.img" -Force
        Run .\build\install_test.exe @("$ft\src", "$ft\big.img", "1100", "--shrink-ntfs") | Out-Null
        Run .\build\install_test.exe @("$ft\src", "$ft\big.img", "1100", "--remove") | Out-Null
        Run .\build\install_test.exe @("$ft\src", "$ft\big.img", "1100", "--same-table", "$ft\big-orig.img") | Out-Null
        Remove-Item "$ft\big.img", "$ft\big-orig.img"
        Ok "uninstall: texts copied to the USB drive, NTFS back to the size Windows made, table byte for byte the original (engine and screen) - OK"
        # boot chooser: started from the EFI partition (FOLIO.ID names this volume), another system in the boot order
        $g = "3E540828-E7AD-48C4-B8AA-5F24B8C2B7E0"
        foreach ($wait in 3, 0) {
            Fresh; New-Item -ItemType Directory -Force "$t\EFI\FolioOS" | Out-Null
            Set-Content -Encoding ascii "$t\EFI\FolioOS\FOLIO.ID" "FolioOS data partition`r`ndata=$g"
            if ($wait -eq 0) { Set-Content -Encoding ascii "$t\FOLIO\CONFIG.INI" "boot_wait=0" }
            $env:FOLIO_VOL_GUID = $g; $env:FOLIO_OTHER_OS = "Windows Boot Manager"
            try { $out = & .\build\folio_sim.exe --root $t --script tests\seletor.txt --out build\shots 2>&1 | Out-String; $rc = $LASTEXITCODE }
            finally { $env:FOLIO_VOL_GUID = $null; $env:FOLIO_OTHER_OS = $null }
            $went = $out -match "boot other: Windows Boot Manager"
            if ($rc -ne 0 -or ($wait -gt 0) -ne $went) { throw "boot chooser (boot_wait=$wait): expected $(if ($wait) { 'the other system' } else { 'FolioOS' }); output: $out" }
            if ($wait -eq 0) {   # Settings → System: the chooser's wait, by keyboard, saved to CONFIG.INI
                $env:FOLIO_VOL_GUID = $g; $env:FOLIO_OTHER_OS = "Windows Boot Manager"
                try { Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\seletor_ajuste.txt", "--out", "build\shots") | Out-Null }
                finally { $env:FOLIO_VOL_GUID = $null; $env:FOLIO_OTHER_OS = $null }
                if (-not (Select-String -Quiet -Path "$t\FOLIO\CONFIG.INI" -Pattern "^boot_wait=[1-9]")) { throw "Settings: the boot chooser wait was not saved (CONFIG.INI: $((Select-String -Path "$t\FOLIO\CONFIG.INI" -Pattern 'boot_wait').Line))" }
            }
        }
        # the same screen in the real BOOTX64.EFI: reads the USB drive through SimpleFS, writes through Block I/O
        $xf = [IO.File]::Create("$ft\target2.img"); $xf.SetLength(600MB); $xf.Close()
        $env:FOLIO_PLATFORM_LANG = "pt-BR"
        try { Fresh; Run .\build\efi_host.exe @("build\efi\BOOTX64.EFI", "--root", $t, "--script", "tests\efi_instalar.txt", "--out", "build\shots", "--disk", "$ft\small.img", "--disk", "$ft\target2.img") | Out-Null }
        finally { $env:FOLIO_PLATFORM_LANG = $oldLang }
        Run .\build\fat32_test.exe @("$ft\target2.img", "--fsck") | Out-Null
        Remove-Item "$ft\copied.efi"; & .\build\fatcat.exe "$ft\target2.img" "EFI/BOOT/BOOTX64.EFI" "$ft\copied.efi" 2>$null
        if (-not (Test-Path "$ft\copied.efi") -or (Get-FileHash "$ft\copied.efi").Hash -ne (Get-FileHash "$t\EFI\BOOT\BOOTX64.EFI").Hash) { throw "installer in BOOTX64.EFI: EFI\BOOT\BOOTX64.EFI not copied to the disk" }
        if (-not (Zero1MB "$ft\small.img")) { throw "installer in BOOTX64.EFI: the disk that is too small was written to" }
        Remove-Item -Recurse -Force $ft
        Ok "partition tables + installer: GPT (UEFI) and MBR (BIOS) disks installed, every file CRC-checked, volumes consistent - OK"
        Ok "installer next to Windows: one new entry, other partitions and entry numbers untouched, \EFI\FolioOS added - OK"
        Ok "real BOOTX64.EFI: 3 disks listed through Block I/O (bus, model, size), GPT/MBR/empty tables read - OK"
        Ok "installer screen (keyboard only): small disk refused, wrong word refused, installed and verified - OK"
        Ok "installer in the real BOOTX64.EFI: USB drive read through SimpleFS, disk written through Block I/O - OK"
        Ok "next to Windows on the screen: nothing else changed; boot chooser: no key starts the other system, boot_wait=0 opens FolioOS - OK"
    }
    Remove-Item -Recurse -Force $t
}

if ($Image -or $Vhd) {
    Step 8 "Disk image (MBR + FAT32, $ImageMB MB)"
    Run gcc @("-O2", "-Wall", "tools\mkimg.c", "-o", "build\mkimg.exe")
    if ($Image) { Run .\build\mkimg.exe @("build\zip_stage", "build\folio.img", "$ImageMB") }
    if ($Vhd)   { Run .\build\mkimg.exe @("build\zip_stage", "build\folio.vhd", "$ImageMB", "--vhd") }
    Remove-Item -Recurse -Force build\zip_stage
    # hybrid: the same image also starts on PCs with BIOS only (boot code only, the FAT32 is unchanged)
    if ($Image -and (Test-Path build\biosboot.exe)) { Run .\build\biosboot.exe @("build\folio.img") | Out-Null; Ok "build\folio.img boots on UEFI and on BIOS-only PCs" }
}

Write-Host "`nDone. To install on a FAT32 USB drive:  .\flash_usb.ps1 -DriveLetter E:" -ForegroundColor Green
Write-Host "To try it on Windows:                    .\build\folio_sim.exe   (or .\build\efi_host.exe build\efi\BOOTX64.EFI)" -ForegroundColor Green
