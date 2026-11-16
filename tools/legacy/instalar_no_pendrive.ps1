# tools/instalar_no_pendrive.ps1
# Prepara e copia automaticamente os arquivos UEFI 64-bit do Folio-86 para o Pen-Drive USB

param(
    [string]$Drive = "F:"
)

$ErrorActionPreference = "Stop"

# Normaliza a letra da unidade
$driveLetter = $Drive.TrimEnd('\', '/').ToUpper()
if (-not $driveLetter.EndsWith(':')) {
    $driveLetter = "$driveLetter`:"
}

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "  FOLIO-86 — PREPARACAO DO PEN-DRIVE UEFI (HP ProBook 640 G5)    " -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan

# 1. Verifica se o diretório fonte existe
$sourceDir = Join-Path $PSScriptRoot "..\build\usb_files"
$efiSource = Join-Path $sourceDir "EFI\BOOT\BOOTX64.EFI"

if (-not (Test-Path $efiSource)) {
    Write-Host "[ERRO] Arquivo BOOTX64.EFI nao encontrado em: $efiSource" -ForegroundColor Red
    Write-Host "Recompile o projeto executando build.bat ou nasm antes de continuar." -ForegroundColor Yellow
    exit 1
}

# 2. Verifica se a unidade de destino existe
if (-not (Test-Path "$driveLetter\")) {
    Write-Host "[ERRO] A unidade $driveLetter nao foi encontrada ou o Pen-Drive nao esta conectado!" -ForegroundColor Red
    Write-Host "Unidades disponiveis no sistema:" -ForegroundColor Yellow
    Get-Volume | Where-Object { $_.DriveLetter } | Format-Table DriveLetter, FriendlyName, FileSystemType, SizeRemaining
    exit 1
}

$vol = Get-Volume -DriveLetter ($driveLetter.TrimEnd(':')) -ErrorAction SilentlyContinue
if ($vol) {
    Write-Host "[INFO] Pen-Drive detectado: $driveLetter ($($vol.FriendlyName)) - Sistema de Arquivos: $($vol.FileSystemType)" -ForegroundColor Green
    if ($vol.FileSystemType -ne "FAT32" -and $vol.FileSystemType -ne "FAT") {
        Write-Host "[AVISO] O sistema de arquivos atual e $($vol.FileSystemType)." -ForegroundColor Yellow
        Write-Host "        O padrao UEFI oficial exige FAT32 para dar boot em notebooks HP/Dell/Lenovo." -ForegroundColor Yellow
        Write-Host "        Recomendado: Formatar $driveLetter como FAT32 antes de usar." -ForegroundColor Yellow
    }
}

# 3. Cria a estrutura EFI\BOOT\ no Pen-Drive
$destEfiBoot = Join-Path "$driveLetter\" "EFI\BOOT"
if (-not (Test-Path $destEfiBoot)) {
    New-Item -ItemType Directory -Path $destEfiBoot -Force | Out-Null
    Write-Host "[OK] Diretorio $destEfiBoot criado." -ForegroundColor Green
}

# 4. Copia o bootloader UEFI 64-bit
$destEfi = Join-Path $destEfiBoot "BOOTX64.EFI"
Copy-Item -Path $efiSource -Destination $destEfi -Force
Write-Host "[OK] BOOTX64.EFI copiado com sucesso para: $destEfi" -ForegroundColor Green

# 5. Copia os manuscritos iniciais para a raiz do Pen-Drive
$manuscripts = Get-ChildItem -Path $sourceDir -Filter "MANUSCR*.MD"
foreach ($m in $manuscripts) {
    $destFile = Join-Path "$driveLetter\" $m.Name
    Copy-Item -Path $m.FullName -Destination $destFile -Force
    Write-Host "[OK] $($m.Name) copiado para raiz ($driveLetter\)" -ForegroundColor DarkGreen
}

Write-Host "`n=================================================================" -ForegroundColor Cyan
Write-Host "  PEN-DRIVE PREPARADO COM SUCESSO! ESTRUTURA FINAL:              " -ForegroundColor Green
Write-Host "=================================================================" -ForegroundColor Cyan
Get-ChildItem -Path "$driveLetter\" -Recurse | Select-Object FullName, Length | Format-Table -AutoSize

Write-Host @"
COMO DAR BOOT NO HP PROBOOK 640 G5:
1. No HP ProBook, reinicie e aperte F10 para entrar no BIOS Setup.
2. Va em 'Security' -> 'Secure Boot Configuration' e DESATIVE o Secure Boot (Disable). Salve (F10) e reinicie.
3. Ao ligar, aperte F9 repetidamente para abrir o menu de inicializacao da HP.
4. Selecione 'USB Hard Drive (UEFI)' ou 'Boot From EFI File' -> selecione EFI\BOOT\BOOTX64.EFI.
5. O Folio-86 abrira com resolucao nativa, fontes graficas e suporte a cliques de mouse/touchpad!
"@ -ForegroundColor Yellow

