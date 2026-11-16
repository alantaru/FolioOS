# =============================================================================
# FOLIO-86 — Teste em VM REAL (QEMU + Firmware UEFI OVMF/EDK2 oficial)
# Arquivo: tools/qemu_test.ps1
#
# Simula um PC x86_64 generico:
#   - Firmware UEFI real (edk2-x86_64-code.fd distribuido com o QEMU) ou SeaBIOS
#   - Controlador USB 3.0 xHCI + Pen-Drive USB (usb-storage) com build/folio86.img
#   - Mouse USB HID (usb-mouse) + Teclado USB HID (usb-kbd)
#   - Placa de video padrao (std VGA / GOP)
# Captura screenshots via monitor HMP (screendump) e injeta mouse/teclado.
#
# Uso:
#   .\tools\qemu_test.ps1                 # teste automatizado UEFI (headless)
#   .\tools\qemu_test.ps1 -Mode bios      # teste automatizado Legacy BIOS (SeaBIOS)
#   .\tools\qemu_test.ps1 -Interactive    # abre janela da VM para voce usar
# =============================================================================
param(
    [ValidateSet("uefi", "bios")]
    [string]$Mode = "uefi",
    [switch]$Interactive,
    [int]$BootWaitSec = 12
)

$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

# --- Localiza o QEMU ---------------------------------------------------------
$qemuDir = @("C:\Program Files\qemu", "$env:LOCALAPPDATA\Programs\qemu") |
    Where-Object { Test-Path (Join-Path $_ "qemu-system-x86_64.exe") } | Select-Object -First 1
if (-not $qemuDir) { throw "QEMU nao encontrado. Instale com: winget install SoftwareFreedomConservancy.QEMU" }
$qemu = Join-Path $qemuDir "qemu-system-x86_64.exe"

New-Item -ItemType Directory -Force -Path "build/qemu" | Out-Null

# Copia a imagem para nao alterar o build original durante o teste
$disk = "build/qemu/usb_stick_$Mode.img"
Copy-Item -Force "build/folio86.img" $disk

$qargs = @(
    "-machine", "q35",
    "-m", "512",
    "-vga", "std",
    "-device", "qemu-xhci,id=xhci",
    "-drive", "if=none,id=stick,format=raw,file=$disk",
    "-device", "usb-storage,bus=xhci.0,drive=stick,bootindex=0",
    "-device", "usb-mouse,bus=xhci.0",
    "-device", "usb-kbd,bus=xhci.0",
    "-monitor", "tcp:127.0.0.1:45454,server,nowait"
)

if ($Mode -eq "uefi") {
    $code = Join-Path $qemuDir "share\edk2-x86_64-code.fd"
    $varsSrc = Join-Path $qemuDir "share\edk2-i386-vars.fd"
    if (-not (Test-Path $code)) { throw "Firmware OVMF nao encontrado em $code" }
    $vars = "build/qemu/ovmf_vars.fd"
    Copy-Item -Force $varsSrc $vars
    $qargs += @(
        "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=$code",
        "-drive", "if=pflash,format=raw,unit=1,file=$vars"
    )
}

if (-not $Interactive) { $qargs += @("-display", "none") }

Write-Host "Iniciando QEMU ($Mode) ..." -ForegroundColor Cyan
if ($Interactive) {
    $proc = Start-Process -FilePath $qemu -ArgumentList $qargs -PassThru
    Write-Host "VM aberta. Feche a janela do QEMU para encerrar."
    $proc.WaitForExit()
    return
}
$proc = Start-Process -FilePath $qemu -ArgumentList $qargs -PassThru -WindowStyle Hidden `
    -RedirectStandardError "build/qemu/qemu_stderr_$Mode.log"

# --- Cliente do monitor HMP ---------------------------------------------------
function Send-Hmp([string]$cmd) {
    $client = New-Object System.Net.Sockets.TcpClient
    for ($i = 0; $i -lt 50; $i++) {
        try { $client.Connect("127.0.0.1", 45454); break } catch { Start-Sleep -Milliseconds 200 }
    }
    $stream = $client.GetStream()
    $writer = New-Object System.IO.StreamWriter($stream)
    $writer.AutoFlush = $true
    Start-Sleep -Milliseconds 250
    $writer.WriteLine($cmd)
    Start-Sleep -Milliseconds 400
    $buf = New-Object byte[] 65536
    $out = ""
    while ($stream.DataAvailable) { $n = $stream.Read($buf, 0, $buf.Length); $out += [Text.Encoding]::ASCII.GetString($buf, 0, $n) }
    $client.Close()
    return $out
}

function Shot([string]$name) {
    $path = ((Resolve-Path "build/qemu").Path + "\$name.png") -replace '\\', '/'
    $r = Send-Hmp "screendump $path -f png"
    Start-Sleep -Milliseconds 500
    Write-Host "  screenshot: build/qemu/$name.png"
}

function MouseMove([int]$dx, [int]$dy) {
    $steps = [Math]::Max([Math]::Ceiling([Math]::Max([Math]::Abs($dx), [Math]::Abs($dy)) / 20), 1)
    for ($i = 0; $i -lt $steps; $i++) {
        Send-Hmp ("mouse_move {0} {1}" -f [int]($dx / $steps), [int]($dy / $steps)) | Out-Null
    }
}

function Click() {
    Send-Hmp "mouse_button 1" | Out-Null
    Send-Hmp "mouse_button 0" | Out-Null
}

function TypeText([string]$text) {
    foreach ($ch in $text.ToCharArray()) {
        $s = [string]$ch
        $key = $null
        if ($s -cmatch '^[a-z0-9]$') { $key = $s }
        elseif ($s -cmatch '^[A-Z]$') { $key = "shift-" + $s.ToLower() }
        elseif ($s -eq ' ') { $key = "spc" }
        elseif ($s -eq '.') { $key = "dot" }
        elseif ($s -eq ',') { $key = "comma" }
        if ($key) { Send-Hmp "sendkey $key" | Out-Null }
    }
}

try {
    Write-Host "Aguardando boot ($BootWaitSec s)..."
    Start-Sleep -Seconds $BootWaitSec
    Shot "${Mode}_01_boot"

    Write-Host "Movendo mouse..."
    MouseMove -200 -340
    Start-Sleep -Milliseconds 800
    Shot "${Mode}_02_mouse_moved"

    Write-Host "Clicando..."
    Click
    Start-Sleep -Seconds 1
    Shot "${Mode}_03_after_click"

    Write-Host "Digitando texto..."
    Send-Hmp "sendkey end" | Out-Null
    TypeText "Teste em VM real QEMU."
    Start-Sleep -Seconds 1
    Shot "${Mode}_04_typed"

    Write-Host "Salvando (Ctrl+S)..."
    Send-Hmp "sendkey ctrl-s" | Out-Null
    Start-Sleep -Seconds 2
    Shot "${Mode}_05_saved"
}
finally {
    try { Send-Hmp "quit" | Out-Null } catch {}
    Start-Sleep -Seconds 1
    if (-not $proc.HasExited) { $proc.Kill() }
}

Write-Host "Concluido. Screenshots em build/qemu/" -ForegroundColor Green
