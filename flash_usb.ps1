# =============================================================================
# FolioOS - install or update on a USB drive (FAT32) without deleting anything
#   .\flash_usb.ps1                  lists removable drives
#   .\flash_usb.ps1 -DriveLetter E:  installs/updates FolioOS on E:\
#   .\flash_usb.ps1 -DriveLetter E: -Bios
#                                    also makes the drive boot on PCs with BIOS only (Legacy/CSM).
#                                    Needs administrator; writes only the boot code, no file is
#                                    touched and the UEFI boot stays the same.
#
# The drive stays an ordinary FAT32 USB drive: DOCUMENTS\, MUSIC\ and FOLIO\ remain
# readable on Windows, Linux and macOS. Drives set up by older versions keep their
# folder names (TEXTOS\, MUSICA\, FOLIO\FONTES\ ...): nothing is renamed or moved.
# =============================================================================
param([string]$DriveLetter = "", [switch]$Bios)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if (-not (Test-Path "build\usb_files\EFI\BOOT\BOOTX64.EFI")) {
    Write-Host "Building first..." -ForegroundColor Yellow
    .\build.ps1 -NoTests
}

if ([string]::IsNullOrWhiteSpace($DriveLetter)) {
    Write-Host "`nRemovable drives:" -ForegroundColor Cyan
    Get-Volume | Where-Object { $_.DriveType -eq 'Removable' -and $_.DriveLetter } |
        Select-Object DriveLetter, FileSystemLabel, FileSystem, @{ Name = "GB"; Expression = { [math]::Round($_.Size / 1GB, 1) } } |
        Format-Table -AutoSize
    Write-Host "Usage: .\flash_usb.ps1 -DriveLetter X:" -ForegroundColor White
    exit 0
}

$letter = $DriveLetter.TrimEnd('\').TrimEnd(':')
$drive = "${letter}:"
if (-not (Test-Path "$drive\")) { throw "Drive $drive not found." }

$vol = Get-Volume -DriveLetter $letter -ErrorAction SilentlyContinue
if ($vol -and $vol.FileSystem -notin @('FAT32', 'FAT')) {
    Write-Host "WARNING: $drive is formatted as $($vol.FileSystem)." -ForegroundColor Red
    Write-Host "UEFI firmware only boots from FAT32. Format it as FAT32 (this ERASES the drive):" -ForegroundColor Yellow
    Write-Host "  Format-Volume -DriveLetter $letter -FileSystem FAT32 -NewFileSystemLabel FOLIO" -ForegroundColor White
    Write-Host "(For drives larger than 32 GB, use Rufus in 'Non bootable' mode + FAT32.)" -ForegroundColor Yellow
    exit 1
}

# existing folder (older Portuguese name) or the current one
function Pick($new, $old) { if (Test-Path "$drive\$old") { return $old } return $new }
$docs   = Pick "DOCUMENTS" "TEXTOS"
$music  = Pick "MUSIC" "MUSICA"
$fonts  = Pick "FOLIO\FONTS" "FOLIO\FONTES"
$themes = Pick "FOLIO\THEMES" "FOLIO\TEMAS"

$src = "build\usb_files"
Write-Host "Installing FolioOS on $drive\ ..." -ForegroundColor Cyan

# system files (always updated)
New-Item -ItemType Directory -Force "$drive\EFI\BOOT", "$drive\$fonts", "$drive\$docs", "$drive\$music", "$drive\$themes",
    "$drive\FOLIO\LANGUAGES", "$drive\FOLIO\KEYBOARDS" | Out-Null
Copy-Item -Force "$src\EFI\BOOT\*.EFI" "$drive\EFI\BOOT\"
Copy-Item -Force "$src\FOLIO\FONTS\*.ttf", "$src\FOLIO\FONTS\*.txt" "$drive\$fonts\"
# every language works out of the box: the extra fonts and the Chinese/Japanese/Korean fonts too
foreach ($sub in "EXTRA", "_LANGUAGE") {
    New-Item -ItemType Directory -Force "$drive\$fonts\$sub" | Out-Null
    Copy-Item -Force -Recurse "$src\FOLIO\FONTS\$sub\*" "$drive\$fonts\$sub\"
}
# PCs with BIOS only: the bridge and the tool that writes the boot code
if (Test-Path "$src\FOLIO\BIOS.SYS") {
    Copy-Item -Force "$src\FOLIO\BIOS.SYS" "$drive\FOLIO\"
    New-Item -ItemType Directory -Force "$drive\FOLIO\TOOLS\bios" | Out-Null
    Copy-Item -Force "$src\FOLIO\TOOLS\biosboot.exe" "$drive\FOLIO\TOOLS\"
    Copy-Item -Force "$src\FOLIO\TOOLS\bios\*" "$drive\FOLIO\TOOLS\bios\"
}
Copy-Item -Force "$src\FOLIO\LANGUAGES\*" "$drive\FOLIO\LANGUAGES\"
Copy-Item -Force "$src\FOLIO\KEYBOARDS\*" "$drive\FOLIO\KEYBOARDS\"
Copy-Item -Force "$src\README.txt", "$src\LEIA-ME.txt", "$src\INSTALL.txt" "$drive\"

# user content: only created when missing
if (-not (Test-Path "$drive\$music\README.txt")) { Copy-Item "$src\MUSIC\README.txt" "$drive\$music\" }
if (-not (Get-ChildItem "$drive\$docs" -File -ErrorAction SilentlyContinue)) { Copy-Item "$src\DOCUMENTS\Welcome.md" "$drive\$docs\" }
foreach ($t in Get-ChildItem "$src\FOLIO\THEMES\*.INI") { if (-not (Test-Path -LiteralPath "$drive\$themes\$($t.Name)")) { Copy-Item -LiteralPath $t.FullName "$drive\$themes\" } }
if ($vol -and -not $vol.FileSystemLabel) { try { Set-Volume -DriveLetter $letter -NewFileSystemLabel "FOLIO" } catch {} }

if ($Bios) {
    Write-Host "Adding the BIOS boot code..." -ForegroundColor Cyan
    & .\build\biosboot.exe $drive
    if ($LASTEXITCODE -ne 0) { throw "biosboot failed (run PowerShell as administrator)" }
}
Write-Host "`n[OK] FolioOS installed." -ForegroundColor Green
Write-Host "To use it: start the PC with the drive plugged in, open the boot menu (F9 HP, F12 Dell/Lenovo, F8 Asus, Esc/F11 others)"
Write-Host "and pick the UEFI entry of the drive. If it does not show up, disable Secure Boot in the firmware setup."
if (-not $Bios) { Write-Host "PC with BIOS only (no UEFI)? Run once, as administrator: .\flash_usb.ps1 -DriveLetter $drive -Bios" }
Write-Host "Your documents live in $drive\$docs and your music in $drive\$music - readable on any computer."
