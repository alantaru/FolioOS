# =============================================================================
# FolioOS - build pipeline
#   .\build.ps1            builds everything, assembles the USB folder and runs the tests
#   .\build.ps1 -NoTests   build only
#   .\build.ps1 -Legacy    also builds the legacy FOLIO-86 v3 (Assembly, BIOS) into build\legacy
#   .\build.ps1 -Image [-ImageMB 4096] [-Vhd]
#                          writes build\folio.img (MBR + FAT32) for Rufus/Etcher/dd
#                          (-Vhd writes build\folio.vhd for VirtualBox/Hyper-V with EFI)
# Requirements: gcc (MinGW-w64) and perl on PATH (Git for Windows ships perl); nasm only for -Legacy.
# Optional: .\tools\fetch-cjk-fonts.ps1 once, to also build FOLIO-FONTS-CJK.zip.
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
$uefi = @("src\platform\uefi\uefi_main.c", "src\platform\uefi\hda.c")
$link = @("-nostdlib", "-shared", "-Wl,--subsystem,10", "-Wl,--exclude-all-symbols")

Step 1 "UEFI x86_64  -> build\efi\BOOTX64.EFI"
Run gcc ($free + @("-mno-red-zone") + $core + $uefi + $link + @("-Wl,-e,efi_main", "-o", "build\efi\BOOTX64.EFI", "-lgcc"))

Step 2 "UEFI IA32    -> build\efi\BOOTIA32.EFI (tablets/netbooks with 32-bit firmware)"
Run gcc ($free + @("-m32", "-march=i686", "-mno-sse", "-mfpmath=387") + $core + $uefi + $link + @("-Wl,-e,_efi_main", "-o", "build\efi\BOOTIA32.EFI"))

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
foreach ($d in "$usb\EFI", "$usb\FOLIO\FONTS", "$usb\FOLIO\LANGUAGES", "$usb\FOLIO\KEYBOARDS") { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
New-Item -ItemType Directory -Force "$usb\EFI\BOOT", "$usb\FOLIO\FONTS", "$usb\FOLIO\LANGUAGES", "$usb\FOLIO\KEYBOARDS",
    "$usb\FOLIO\THEMES", "$usb\DOCUMENTS", "$usb\MUSIC" | Out-Null
Copy-Item build\efi\BOOTX64.EFI, build\efi\BOOTIA32.EFI "$usb\EFI\BOOT\"
Copy-Item assets\fonts\*.ttf, assets\fonts\OFL-*.txt "$usb\FOLIO\FONTS\"
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
Remove-Item -Recurse -Force $stage
Ok ("distribution package: $zip ({0:N1} MB) - unzip at the root of a FAT32 USB drive" -f ((Get-Item $zip).Length / 1MB))
# optional font pack: unzip at the drive root -> FOLIO\FONTS\EXTRA\<family>
$fz = "build\FOLIO-FONTS-EXTRA.zip"
Write-Zip "assets\fonts-extras" $fz "FOLIO/FONTS/EXTRA/"
Ok ("extra font pack: $fz ({0:N1} MB, {1} families)" -f ((Get-Item $fz).Length / 1MB), (Get-ChildItem -Directory assets\fonts-extras).Count)
# CJK pack (Chinese/Japanese/Korean interface): only when the fonts were downloaded
if (Test-Path "assets\fonts-cjk\_LANGUAGE\NotoSansSC.ttf") {
    $cz = "build\FOLIO-FONTS-CJK.zip"
    Write-Zip "assets\fonts-cjk" $cz "FOLIO/FONTS/"
    Ok ("CJK font pack: $cz ({0:N1} MB)" -f ((Get-Item $cz).Length / 1MB))
} else {
    Write-Host "      CJK font pack skipped (run .\tools\fetch-cjk-fonts.ps1 once to build it)" -ForegroundColor Yellow
}
$size = (Get-Item build\efi\BOOTX64.EFI).Length / 1KB
Ok ("BOOTX64.EFI {0:N0} KB - fonts {1:N1} MB - {2} languages - {3} keyboards" -f $size, ((Get-ChildItem "$usb\FOLIO\FONTS\*.ttf" | Measure-Object Length -Sum).Sum / 1MB), ((Get-ChildItem assets\i18n\*.po).Count + 1), (Get-ChildItem assets\keyboards\*.txt -Exclude _*).Count)

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
    New-Item -ItemType Directory -Force "$t\FOLIO\FONTS\EXTRA" | Out-Null
    Copy-Item -Recurse "assets\fonts-extras\Lora", "assets\fonts-extras\iA Writer Quattro" "$t\FOLIO\FONTS\EXTRA\"
    Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\temas.txt", "--out", "build\shots") | Out-Null
    Ok "user themes (colors + fonts), theme editor, FolioOS splash - OK"
    Fresh; Run .\build\efi_host.exe @("build\efi\BOOTX64.EFI", "--root", $t, "--script", "tests\efi_boot.txt", "--out", "build\shots")
    if (-not (Test-Path "$t\DOCUMENTS\uefi_teste.md")) { throw "BOOTX64.EFI did not write the test file" }
    Ok "real BOOTX64.EFI: boot, PlatformLang pt-BR, ABNT2 typing, saving to the volume - OK"
    Fresh; Run .\build\efi_host.exe @("build\efi\BOOTX64.EFI", "--root", $t, "--script", "tests\efi_roda.txt", "--out", "build\shots") | Out-Null
    Ok "real BOOTX64.EFI: fake wheel blocked, slow wheel free, left+right = middle, Esc stops - OK"
    # languages: English by default (no firmware language), switching, plurals, keyboards, CJK fonts,
    # old Portuguese installs keep Portuguese
    Remove-Item Env:\FOLIO_PLATFORM_LANG
    Fresh
    if (Test-Path "assets\fonts-cjk\_LANGUAGE") { Copy-Item -Recurse "assets\fonts-cjk\_LANGUAGE" "$t\FOLIO\FONTS\" }
    Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\i18n.txt", "--out", "build\shots") | Out-Null
    if (-not (Select-String -Quiet -Path "$t\FOLIO\CONFIG.INI" -Pattern "^language=")) { throw "language was not saved in CONFIG.INI" }
    Fresh; Remove-Item -Recurse -Force "$t\FOLIO\FONTS\_LANGUAGE" -ErrorAction SilentlyContinue
    Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\i18n_cjk_missing.txt", "--out", "build\shots") | Out-Null
    Fresh; Set-Content -Encoding ascii "$t\FOLIO\CONFIG.INI" "tema=Papiro`r`nteclado=abnt2`r`nabertura=0`r`n"
    Run .\build\folio_sim.exe @("--root", $t, "--script", "tests\i18n_legacy.txt", "--out", "build\shots") | Out-Null
    if (-not (Select-String -Quiet -Path "$t\FOLIO\CONFIG.INI" -Pattern "^theme=Papyrus")) { throw "old Portuguese CONFIG.INI was not migrated to the new keys" }
    Ok "languages: English default, 14 translations, plurals (ru/pl), keyboards, CJK fonts, old installs stay in Portuguese - OK"
    # universal video: pixel formats, Blt only, padded stride, UGA (no GOP), portrait panel with EDID
    $vs = "build\test_video.txt"; Set-Content -Encoding ascii $vs "wait 1200`nshot v.png`nquit"
    foreach ($g in @(@("--gop", "rgb"), @("--gop", "mask16"), @("--gop", "blt"), @("--gop", "pad"), @("--gop", "uga"), @("--size", "800x1280", "--edid"))) {
        Fresh; Run .\build\efi_host.exe (@("build\efi\BOOTX64.EFI", "--root", $t, "--script", $vs, "--out", "build\shots") + $g) | Out-Null
        if (-not (Select-String -Quiet -Path "$t\FOLIO\DIAGNOSTICS.TXT" -Pattern "in use: ")) { throw "video $($g -join ' '): no output configured" }
    }
    Remove-Item $vs
    Ok "video: RGB, 16-bit mask, Blt only, padded stride, UGA, rotated portrait - OK"
    Remove-Item -Recurse -Force $t
}

if ($Image -or $Vhd) {
    Step 8 "Disk image (MBR + FAT32, $ImageMB MB)"
    Run gcc @("-O2", "-Wall", "tools\mkimg.c", "-o", "build\mkimg.exe")
    if ($Image) { Run .\build\mkimg.exe @("build\usb_files", "build\folio.img", "$ImageMB") }
    if ($Vhd)   { Run .\build\mkimg.exe @("build\usb_files", "build\folio.vhd", "$ImageMB", "--vhd") }
}

Write-Host "`nDone. To install on a FAT32 USB drive:  .\flash_usb.ps1 -DriveLetter E:" -ForegroundColor Green
Write-Host "To try it on Windows:                    .\build\folio_sim.exe   (or .\build\efi_host.exe build\efi\BOOTX64.EFI)" -ForegroundColor Green
