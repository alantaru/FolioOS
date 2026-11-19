# =============================================================================
# FolioOS - QEMU lab: boots the real FolioOS on emulated machines and checks it end to end
#   .\tools\qemu-lab.ps1                 every configuration in the matrix
#   .\tools\qemu-lab.ps1 -Only q35-std   configurations whose name matches
#   .\tools\qemu-lab.ps1 -List           list the matrix
# Needs QEMU (winget install SoftwareFreedomConservancy.QEMU) and a previous .\build.ps1.
# For each machine: boots a copy of the FAT32 drive image, then through QMP (QEMU's control
# protocol) takes screenshots, presses keys, moves and clicks the mouse, plays the sound test
# into a .wav file, and finally reads FOLIO\DIAGNOSTICS.TXT from the disk image.
# Results: build\lab\report.md and build\lab\<config>\*.png ($env:FOLIO_WORK\lab when FOLIO_WORK is set)
# =============================================================================
param([string]$Only = "", [switch]$List, [int]$BootWait = 0, [switch]$Keep)   # -Keep: keep the disk images
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot)
$QemuDir = "C:\Program Files\qemu"
if (-not (Test-Path "$QemuDir\qemu-system-x86_64.exe")) { $c = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue; if ($c) { $QemuDir = Split-Path $c.Source } else { throw "QEMU not found (winget install SoftwareFreedomConservancy.QEMU)" } }
$lab = if ($env:FOLIO_WORK) { "$env:FOLIO_WORK\lab" } else { "build\lab" }   # FOLIO_WORK: disk images on another drive
New-Item -ItemType Directory -Force $lab | Out-Null
Add-Type -AssemblyName System.Drawing

# ------------------------------------------------------------ the matrix
# fw: uefi64 / uefi32 / bios / aa64 ; vga: QEMU display device ; input: ps2 / usb / tablet ; audio: hda / ich9 / ac97 / none
$matrix = @(
    @{ name = "q35-std-ps2-ich9";      sys = "x86_64"; fw = "uefi64"; machine = "q35"; vga = "-vga std";                         input = "ps2";    audio = "ich9" },
    @{ name = "pc-cirrus-usb-ac97";    sys = "x86_64"; fw = "uefi64"; machine = "pc";  vga = "-vga cirrus";                      input = "usb";    audio = "ac97" },
    @{ name = "q35-virtio-tablet-hda"; sys = "x86_64"; fw = "uefi64"; machine = "q35"; vga = "-vga virtio";                      input = "tablet"; audio = "hda" },
    @{ name = "q35-vmware-ps2-ich9";   sys = "x86_64"; fw = "uefi64"; machine = "q35"; vga = "-vga vmware";                      input = "ps2";    audio = "ich9" },
    @{ name = "q35-qxl-usb-ich9";      sys = "x86_64"; fw = "uefi64"; machine = "q35"; vga = "-vga qxl";                         input = "usb";    audio = "ich9" },
    @{ name = "q35-ramfb-usb-none";    sys = "x86_64"; fw = "uefi64"; machine = "q35"; vga = "-vga none -device ramfb";          input = "usb";    audio = "none" },
    @{ name = "q35-bochs-tablet-hda";  sys = "x86_64"; fw = "uefi64"; machine = "q35"; vga = "-vga none -device bochs-display";  input = "tablet"; audio = "hda" },
    @{ name = "ia32-pc-std-ps2-ac97";  sys = "i386";   fw = "uefi32"; machine = "pc";  vga = "-vga std";                         input = "ps2";    audio = "ac97" },
    # ARM64: QEMU "virt" with a Cortex-A72 (the Raspberry Pi 4 CPU); no PS/2 and no legacy VGA on ARM
    @{ name = "aa64-virtio-gpu-usb-hda";  sys = "aarch64"; fw = "aa64"; machine = "virt"; vga = "-device virtio-gpu-pci"; input = "usb";    audio = "hda" },
    @{ name = "aa64-ramfb-tablet-none";   sys = "aarch64"; fw = "aa64"; machine = "virt"; vga = "-device ramfb";          input = "tablet"; audio = "none" },
    # BIOS only (SeaBIOS, no UEFI): FOLIO\BIOS.SYS bridge + BOOTIA32.EFI; stick on USB like a real PC, and an IDE disk
    @{ name = "bios-pc-std-ps2-ac97";     sys = "i386";    fw = "bios"; machine = "pc";   vga = "-vga std";               input = "ps2";    audio = "ac97" },
    @{ name = "bios-q35-std-ps2-hda";     sys = "x86_64";  fw = "bios"; machine = "q35";  vga = "-vga std";               input = "ps2";    audio = "hda" },
    @{ name = "bios-pc-cirrus-ide-ich9";  sys = "x86_64";  fw = "bios"; machine = "pc";   vga = "-vga cirrus";            input = "ps2";    audio = "ich9"; disk = "ide" },
    # installed on the computer's own disk (no USB drive): GPT on a SATA disk with UEFI, MBR on an IDE disk with BIOS
    @{ name = "inst-uefi-q35-sata";       sys = "x86_64";  fw = "uefi64"; machine = "q35"; vga = "-vga std";              input = "ps2";    audio = "hda";  disk = "sata"; src = "inst-uefi"; extra = "nvme" },
    @{ name = "inst-bios-pc-ide";         sys = "x86_64";  fw = "bios"; machine = "pc";   vga = "-vga std";               input = "ps2";    audio = "ac97"; disk = "ide";  src = "inst-bios"; extra = "ide" },
    # keep proportions: a 4:3 mode (1024x768) on a 16:9 panel, drawn at 1024x576 and stretched to the mode (UEFI GOP and BIOS VBE)
    @{ name = "aspect-uefi-q35-std";      sys = "x86_64";  fw = "uefi64"; machine = "q35"; vga = "-vga std";              input = "ps2";    audio = "none"; ini = "resolution=1024x768`r`nkeep_aspect=16:9"; aspect = "1024x576" },
    @{ name = "aspect-bios-pc-std";       sys = "i386";    fw = "bios"; machine = "pc";   vga = "-vga std";               input = "ps2";    audio = "none"; ini = "resolution=1024x768`r`nkeep_aspect=16:9"; aspect = "1024x576" }
)
if ($List) { $matrix | ForEach-Object { "{0,-26} {1,-7} {2,-6} {3}  input={4} audio={5}" -f $_.name, $_.fw, $_.machine, $_.vga, $_.input, $_.audio }; return }

# ------------------------------------------------------------ QMP (QEMU Machine Protocol) client
class Qmp {
    [System.Net.Sockets.TcpClient]$tcp; [System.IO.StreamReader]$r; [System.IO.StreamWriter]$w
    Qmp([int]$port) {
        $deadline = (Get-Date).AddSeconds(20)
        while ($true) {
            try { $this.tcp = [System.Net.Sockets.TcpClient]::new("127.0.0.1", $port); break }
            catch { if ((Get-Date) -gt $deadline) { throw "QMP did not answer on port $port" }; Start-Sleep -Milliseconds 300 }
        }
        $s = $this.tcp.GetStream()
        $this.r = [System.IO.StreamReader]::new($s); $this.w = [System.IO.StreamWriter]::new($s); $this.w.AutoFlush = $true
        $this.r.ReadLine() | Out-Null
        $this.Cmd('{"execute":"qmp_capabilities"}') | Out-Null
    }
    [string] Cmd([string]$json) {
        $this.w.WriteLine($json)
        while ($true) { $l = $this.r.ReadLine(); if ($null -eq $l) { return "" }; if ($l -match '"return"|"error"') { return $l } }
        return ""
    }
    [void] Key([string[]]$qcodes) {
        $k = ($qcodes | ForEach-Object { '{"type":"qcode","data":"' + $_ + '"}' }) -join ","
        $this.Cmd('{"execute":"send-key","arguments":{"keys":[' + $k + '],"hold-time":60}}') | Out-Null
        Start-Sleep -Milliseconds 250
    }
    [void] Write([string]$text) {
        foreach ($ch in $text.ToCharArray()) {
            $s = [string]$ch; $q = ""
            if ($s -match '^[a-z0-9]$') { $q = $s } elseif ($s -eq ' ') { $q = "spc" } elseif ($s -eq '.') { $q = "dot" }
            if ($q) { $this.Key(@($q)) }
        }
    }
    [void] MouseRel([int]$dx, [int]$dy) {
        $this.Cmd('{"execute":"input-send-event","arguments":{"events":[{"type":"rel","data":{"axis":"x","value":' + $dx + '}},{"type":"rel","data":{"axis":"y","value":' + $dy + '}}]}}') | Out-Null
        Start-Sleep -Milliseconds 120
    }
    [void] MouseAbs([int]$x, [int]$y) {   # 0..32767 in both axes (tablet)
        $this.Cmd('{"execute":"input-send-event","arguments":{"events":[{"type":"abs","data":{"axis":"x","value":' + $x + '}},{"type":"abs","data":{"axis":"y","value":' + $y + '}}]}}') | Out-Null
        Start-Sleep -Milliseconds 120
    }
    [void] Click() {
        $this.Cmd('{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":true,"button":"left"}}]}}') | Out-Null
        Start-Sleep -Milliseconds 120
        $this.Cmd('{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":false,"button":"left"}}]}}') | Out-Null
        Start-Sleep -Milliseconds 300
    }
    [void] Shot([string]$path) {
        $full = [System.IO.Path]::GetFullPath($path).Replace('\', '/')
        $this.Cmd('{"execute":"screendump","arguments":{"filename":"' + $full + '","format":"png"}}') | Out-Null
        for ($i = 0; $i -lt 40 -and -not (Test-Path $path); $i++) { Start-Sleep -Milliseconds 100 }
        Start-Sleep -Milliseconds 200
    }
    [void] Quit() { try { $this.Cmd('{"execute":"quit"}') | Out-Null } catch {} ; $this.tcp.Close() }
}

# ------------------------------------------------------------ image checks
function Read-Shot($path) {
    $bmp = [System.Drawing.Bitmap]::FromFile((Resolve-Path $path).Path)
    $w = $bmp.Width; $h = $bmp.Height; $paper = 0; $dark = 0; $n = 0
    $px = @{}
    for ($y = 0; $y -lt $h; $y += 8) { for ($x = 0; $x -lt $w; $x += 8) {
        $c = $bmp.GetPixel($x, $y); $n++
        if ($c.R + $c.G + $c.B -lt 40) { $dark++ }
        if ([math]::Abs($c.R - 0xF8) -lt 14 -and [math]::Abs($c.G - 0xF2) -lt 14 -and [math]::Abs($c.B - 0xE4) -lt 18) { $paper++ }
        $px["$x,$y"] = $c.ToArgb()
    } }
    $bmp.Dispose()
    return @{ w = $w; h = $h; paper = $paper / $n; dark = $dark / $n; px = $px }
}
function Compare-Shot($a, $b) {
    if ($a.w -ne $b.w -or $a.h -ne $b.h) { return 1.0 }
    $d = 0; foreach ($k in $a.px.Keys) { if ($a.px[$k] -ne $b.px[$k]) { $d++ } }
    return $d / $a.px.Count
}
function Wav-Peak($path) {
    if (-not (Test-Path $path)) { return 0 }
    $b = [System.IO.File]::ReadAllBytes((Resolve-Path $path).Path)
    $peak = 0
    for ($i = 44; $i + 1 -lt $b.Length; $i += 2) { $v = [Math]::Abs([int][BitConverter]::ToInt16($b, $i)); if ($v -gt $peak) { $peak = $v } }
    return $peak
}

# ------------------------------------------------------------ base image (drive with a test CONFIG.INI)
if (-not (Test-Path build\usb_files\EFI\BOOT\BOOTX64.EFI)) { throw "run .\build.ps1 first" }
& gcc -O2 tools\mkimg.c -o build\mkimg.exe; & gcc -O2 tools\fatcat.c -o build\fatcat.exe
$stage = "$lab\drive"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
Copy-Item -Recurse build\usb_files $stage
Get-ChildItem "$stage\DOCUMENTS" | Remove-Item -Recurse -Force
Get-ChildItem "$stage\MUSIC" -Exclude README.txt | Remove-Item -Recurse -Force    # the developer's own music stays out
foreach ($x in "CONFIG.INI", "DIAGNOSTICS.TXT", "DRAFT.MD", "TABS", "VERSIONS") { if (Test-Path "$stage\FOLIO\$x") { Remove-Item -Recurse -Force "$stage\FOLIO\$x" } }
Set-Content -Encoding ascii "$stage\FOLIO\CONFIG.INI" "language=en`r`nsplash=1`r`nsplash_seconds=1`r`nvolume=100`r`ndebug_serial=1`r`n"
Set-Content -Encoding ascii "$stage\DOCUMENTS\Lab.md" "# QEMU lab`r`n`r`nThe quick brown fox jumps over the lazy dog.`r`n"
& .\build\mkimg.exe $stage "$lab\base.img" 256 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "mkimg failed" }
# boot by BIOS too (MBR/VBR code only; the FAT32 and the UEFI boot stay the same)
if (Test-Path build\biosboot.exe) { & .\build\biosboot.exe "$lab\base.img" | Out-Null; if ($LASTEXITCODE -ne 0) { throw "biosboot failed" } }
# the installer: FolioOS installed by the real install engine on an empty "internal disk" (GPT for UEFI,
# MBR for BIOS), then started like a PC with no USB drive attached
if (Test-Path build\install_test.exe) {
    $instMatch = -not $Only -or ($matrix | Where-Object { $_.src -and $_.name -match $Only })
    if ($instMatch) {
        & .\build\install_test.exe $stage "$lab\inst-uefi.img" 1024 | Out-Null; if ($LASTEXITCODE -ne 0) { throw "install on a GPT disk failed" }
        & .\build\install_test.exe $stage "$lab\inst-bios.img" 1024 --bios | Out-Null; if ($LASTEXITCODE -ne 0) { throw "install on an MBR disk failed" }
    }
}

# ------------------------------------------------------------ run one configuration
$port = 4460
$results = @()
foreach ($m in $matrix) {
    if ($Only -and $m.name -notmatch $Only) { continue }
    $dir = "$lab\$($m.name)"; if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }; New-Item -ItemType Directory $dir | Out-Null
    if ($m.src -and -not (Test-Path "$lab\$($m.src).img")) { continue }     # installer test images need build\install_test.exe
    $img = "$dir\disk.img"; Copy-Item $(if ($m.src) { "$lab\$($m.src).img" } else { "$lab\base.img" }) $img
    if ($m.ini) {                                 # its own drive: the test CONFIG.INI plus this configuration's settings
        $st2 = "$dir\drive"; Copy-Item -Recurse $stage $st2
        Add-Content -Encoding ascii "$st2\FOLIO\CONFIG.INI" $m.ini
        & .\build\mkimg.exe $st2 $img 256 | Out-Null; if ($LASTEXITCODE -ne 0) { throw "mkimg failed" }
        if (Test-Path build\biosboot.exe) { & .\build\biosboot.exe $img | Out-Null }
        Remove-Item -Recurse -Force $st2
    }
    $wav = (Resolve-Path $dir).Path + "\audio.wav"
    $port++
    $exe = "$QemuDir\qemu-system-$($m.sys).exe"
    $a = @("-machine", $m.machine, "-m", "1024", "-smp", "2", "-accel", "tcg", "-display", "none", "-no-shutdown",
           "-qmp", "tcp:127.0.0.1:$port,server=on,wait=off", "-serial", "file:$dir\serial.txt")
    switch ($m.fw) {
        "uefi64" { Copy-Item "$QemuDir\share\edk2-i386-vars.fd" "$dir\vars.fd"; $a += @("-drive", "if=pflash,format=raw,readonly=on,file=$QemuDir\share\edk2-x86_64-code.fd", "-drive", "if=pflash,format=raw,file=$dir\vars.fd") }
        "aa64"   { Copy-Item "$QemuDir\share\edk2-arm-vars.fd" "$dir\vars.fd"; $a += @("-cpu", "cortex-a72", "-drive", "if=pflash,format=raw,readonly=on,file=$QemuDir\share\edk2-aarch64-code.fd", "-drive", "if=pflash,format=raw,file=$dir\vars.fd") }
        "uefi32" { Copy-Item "$QemuDir\share\edk2-i386-vars.fd" "$dir\vars.fd"; $a += @("-drive", "if=pflash,format=raw,readonly=on,file=$QemuDir\share\edk2-i386-code.fd", "-drive", "if=pflash,format=raw,file=$dir\vars.fd") }
    }
    $a += $m.vga.Split(" ")
    if ($m.disk -eq "ide") { $a += @("-drive", "if=ide,format=raw,file=$img") }
    elseif ($m.disk -eq "sata") { $a += @("-drive", "if=none,id=hd,format=raw,file=$img", "-device", "ide-hd,drive=hd,bus=ide.0,bootindex=0") }   # q35: SATA (AHCI)
    else { $a += @("-device", "qemu-xhci,id=xhci", "-drive", "if=none,id=stick,format=raw,file=$img", "-device", "usb-storage,bus=xhci.0,drive=stick,bootindex=0") }
    if ($m.extra) {                               # an empty second disk: the disk list must show both, with the right bus; the installer screen installs onto it
        $x = "$dir\extra.img"; $xf = [IO.File]::Create($x); $xf.SetLength(1GB); $xf.Close()
        if ($m.extra -eq "nvme") { $a += @("-drive", "if=none,id=nv,format=raw,file=$x", "-device", "nvme,serial=folio2,drive=nv") }
        else { $a += @("-drive", "if=ide,index=1,format=raw,file=$x") }
    }
    switch ($m.input) { "usb" { $a += @("-device", "usb-kbd,bus=xhci.0", "-device", "usb-mouse,bus=xhci.0") } "tablet" { $a += @("-device", "usb-kbd,bus=xhci.0", "-device", "usb-tablet,bus=xhci.0") } }
    if ($m.audio -ne "none") { $a += @("-audiodev", "wav,id=snd,path=$wav") }
    switch ($m.audio) {
        "hda"  { $a += @("-device", "intel-hda", "-device", "hda-output,audiodev=snd") }
        "ich9" { $a += @("-device", "ich9-intel-hda", "-device", "hda-output,audiodev=snd") }
        "ac97" { $a += @("-device", "AC97,audiodev=snd") }
    }
    Write-Host ("[{0}]" -f $m.name) -ForegroundColor Cyan
    $argline = ($a | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join " "
    $p = Start-Process -FilePath $exe -ArgumentList $argline -PassThru -WindowStyle Hidden -RedirectStandardError "$dir\qemu-stderr.txt"
    $r = [ordered]@{ config = $m.name; boot = "FAIL"; keyboard = "-"; mouse = "-"; sound = "-"; save = "-"; fs = "-"; video = "-"; diag = "-"; disks = "-"; install = "-"; seconds = 0 }
    $t0 = Get-Date
    try {
        $q = [Qmp]::new($port)
        # boot: wait until the cream paper of the editor fills a good part of the screen
        $limit = if ($BootWait) { $BootWait } else { 150 }
        $booted = $false
        for ($i = 0; $i -lt $limit / 3; $i++) {
            Start-Sleep 3
            $q.Shot("$dir\boot.png")
            if (Test-Path "$dir\boot.png") { $s = Read-Shot "$dir\boot.png"; Remove-Item "$dir\boot.png" -ErrorAction SilentlyContinue; if ($s.paper -gt 0.25) { $booted = $true; break } }
            if ($p.HasExited) { break }
        }
        $r.seconds = [int]((Get-Date) - $t0).TotalSeconds
        Start-Sleep 2
        $q.Shot("$dir\01-editor.png")
        if ($booted) {
            $r.boot = "OK"
            $base = Read-Shot "$dir\01-editor.png"
            # keyboard: F12 opens Settings (a big window over the page)
            $q.Key(@("f12")); Start-Sleep 2; $q.Shot("$dir\02-f12-settings.png")
            $r.keyboard = if ((Compare-Shot $base (Read-Shot "$dir\02-f12-settings.png")) -gt 0.05) { "OK" } else { "FAIL" }
            $q.Key(@("esc")); Start-Sleep 1; $q.Key(@("esc")); Start-Sleep 2
            # mouse: point at the menu button (top left) and click; the menu window opens
            $before = Read-Shot ($(Start-Sleep 1; $q.Shot("$dir\03-before-click.png"); "$dir\03-before-click.png"))
            if ($m.input -eq "tablet") { for ($k = 15; $k -ge 0; $k--) { $q.MouseAbs([int]((30 + 20 * $k) / $base.w * 32767), [int]((28 + 12 * $k) / $base.h * 32767)) } }   # pen/tablet: hovering moves first
            else { for ($k = 0; $k -lt 12; $k++) { $q.MouseRel(-200, -200) }; for ($k = 0; $k -lt 28; $k++) { $q.MouseRel(1, 1) } }   # small steps: no pointer acceleration
            $q.Click(); Start-Sleep 2; $q.Shot("$dir\04-menu-click.png")
            $r.mouse = if ((Compare-Shot $before (Read-Shot "$dir\04-menu-click.png")) -gt 0.02) { "OK" } else { "FAIL" }
            $q.Key(@("esc")); Start-Sleep 1
            # sound: command palette -> "Sound test"
            if ($m.audio -ne "none") {
                $q.Key(@("ctrl", "k")); Start-Sleep 1; $q.Write("sound test"); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 4
            }
            # saving: type a phrase at the end of the open document and save (the drive is checked after QEMU closes)
            $q.Key(@("esc")); Start-Sleep 1
            $q.Key(@("ctrl", "end")); $q.Write(" folio lab saved"); Start-Sleep 1
            $q.Key(@("ctrl", "s")); Start-Sleep 2; $q.Key(@("ret")); Start-Sleep 3; $q.Shot("$dir\05-saved.png")   # untitled: "Save as" with the name filled in
            # installer: FolioOS installed from the screen onto the empty second disk (English: the word is ERASE)
            if ($m.extra) {
                $q.Key(@("esc")); Start-Sleep 1
                $q.Key(@("ctrl", "k")); Start-Sleep 1; $q.Write("install folioos"); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 3
                $q.Shot("$dir\06-installer.png")
                $q.Key(@("shift", "tab")); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 2
                $q.Write("erase"); Start-Sleep 1; $q.Key(@("tab")); $q.Key(@("tab")); Start-Sleep 1; $q.Shot("$dir\07-confirm.png"); $q.Key(@("ret"))
                Start-Sleep $(if ($m.fw -eq "bios") { 150 } else { 60 })
                $q.Shot("$dir\08-installed.png")
            }
        }
        $q.Quit()
    } catch { Write-Host "  $_" -ForegroundColor Red }
    if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
    Start-Sleep 1
    if ($m.audio -ne "none") { $peak = Wav-Peak $wav; $r.sound = if ($peak -gt 500) { "OK ($peak)" } else { "FAIL ($peak)" } }
    $ErrorActionPreference = "Continue"            # a missing file is a test result, not a script error
    & .\build\fatcat.exe $img "FOLIO/DIAGNOSTICS.TXT" "$dir\DIAGNOSTICS.TXT" 2>$null
    if ($r.boot -eq "OK") {
        & .\build\fatcat.exe $img "DOCUMENTS/Welcome to FolioOS.md" "$dir\saved.md" 2>$null
        $r.save = if ((Test-Path "$dir\saved.md") -and ((Get-Content -Raw -Encoding UTF8 "$dir\saved.md") -match "folio lab saved")) { "OK" } else { "FAIL" }
    }
    # the whole FAT32 volume is still consistent (no cluster in two files, no lost clusters, FAT copies equal)
    if (Test-Path build\fat32_test.exe) { $fsout = & .\build\fat32_test.exe $img --fsck 2>&1; $r.fs = if ($LASTEXITCODE -eq 0) { "OK" } else { "FAIL" }; $fsout | Set-Content "$dir\fsck.txt" }
    if ($m.extra -and $r.boot -eq "OK") {          # the disk written by the installer screen
        $null = & .\build\fat32_test.exe "$dir\extra.img" --fsck 2>&1; $iok = $LASTEXITCODE -eq 0
        & .\build\fatcat.exe "$dir\extra.img" "EFI/BOOT/BOOTX64.EFI" "$dir\installed.efi" 2>$null
        $r.install = if ($iok -and (Test-Path "$dir\installed.efi") -and (Get-Item "$dir\installed.efi").Length -gt 100000) { "OK" } else { "FAIL" }
    }
    if (Test-Path "$dir\DIAGNOSTICS.TXT") {
        $d = Get-Content -Raw -Encoding UTF8 "$dir\DIAGNOSTICS.TXT"
        $r.diag = "OK"
        $v = [regex]::Match($d, "in use:[^\r\n]*"); if ($v.Success) { $r.video = $v.Value.Substring(7).Trim() }
        if ($m.aspect) {                         # keep proportions: drawn at the panel's shape, the picture still fills the 1024x768 mode
            $sh = if (Test-Path "$dir\01-editor.png") { Read-Shot "$dir\01-editor.png" } else { $null }
            $ok = $d -match "keep proportions: drawn at $($m.aspect), stretched to 1024x768" -and $d -match "logical screen $($m.aspect)" -and $sh -and $sh.w -eq 1024 -and $sh.h -eq 768 -and $sh.dark -lt 0.05
            $r.video = $(if ($ok) { "OK keep proportions $($m.aspect) -> 1024x768" } else { "FAIL keep proportions: $($r.video)" })
        }
        # disks: the one FolioOS started from must be recognised, with its FAT32 volume
        $dk = [regex]::Match($d, "== Disks \((\d+) found\)")
        $want = if ($m.extra) { 2 } else { 1 }
        $r.disks = if ($dk.Success -and [int]$dk.Groups[1].Value -eq $want -and $d -match "FolioOS started from here" -and $d -match "FAT32" -and ($m.disk -ne "sata" -or $d -match "SATA . QEMU HARDDISK")) { "OK ($($dk.Groups[1].Value))" } else { "FAIL" }
    }
    $ErrorActionPreference = "Stop"
    $results += [pscustomobject]$r
    # the disk (256 MB) and the recording were only needed for the checks above
    if (-not $Keep) { Remove-Item $img, $wav, "$dir\vars.fd", "$dir\extra.img" -ErrorAction SilentlyContinue }
    "  boot {0} ({1}s) | keyboard {2} | mouse {3} | sound {4} | save {5} | volume {6} | diagnostics {7} | disks {9} | install {10} | video {8}" -f $r.boot, $r.seconds, $r.keyboard, $r.mouse, $r.sound, $r.save, $r.fs, $r.diag, $r.video, $r.disks, $r.install
}

# ------------------------------------------------------------ report
# ------------------------------------------------------------ dual boot next to "Windows" (ROADMAP phase 2)
# A disk like a PC with Windows: EFI partition whose Windows Boot Manager is build\fakewin.efi (it paints the
# screen blue and prints FAKE WINDOWS STARTED), MSR, an NTFS-like C:, free space and a recovery partition.
#   A. started from the USB drive, the installer screen installs FolioOS next to it (no word to type);
#   B. only the disk, no key: FolioOS starts first (its firmware boot entry), waits, then "Windows" starts;
#   C. only the disk, a key during the wait: the FolioOS editor, running from its own partition.
# dual-ntfs-shrink: the same with a disk made by Windows itself (tests\fixtures\ntfs-big: EFI + NTFS, no free space),
# so the installer shrinks the NTFS (same keys: the shrink option comes first); afterwards the NTFS is re-read.
$dualAll = @()
$dualCfgs = @(@{ name = "dual-uefi-sata"; mb = 1400 }, @{ name = "dual-ntfs-shrink"; mb = 1100 })
foreach ($dc in $dualCfgs) {
if ((-not $Only -or $dc.name -match $Only) -and (Test-Path build\install_test.exe) -and (Test-Path build\fakewin.efi) -and (Test-Path build\fscheck_test.exe)) {
    $dd = "$lab\$($dc.name)"; if (Test-Path $dd) { Remove-Item -Recurse -Force $dd }; New-Item -ItemType Directory $dd | Out-Null
    $dualRes = [ordered]@{ name = $dc.name; install = "FAIL"; other = "FAIL"; writer = "FAIL"; untouched = "FAIL" }
    if ($dc.name -eq "dual-uefi-sata") {
        & .\build\install_test.exe $stage "$dd\win.img" 1400 --windows --winefi build\fakewin.efi | Out-Null
        & .\build\install_test.exe $stage "$dd\before.img" 1400 --windows --winefi build\fakewin.efi | Out-Null   # the same disk, never booted: to compare
    } else {
        $gi = [IO.File]::OpenRead("tests\fixtures\ntfs-big.img.gz"); $gz = New-Object IO.Compression.GZipStream($gi, [IO.Compression.CompressionMode]::Decompress)
        $go = [IO.File]::Create("$dd\win.img"); $gz.CopyTo($go); $go.Dispose(); $gz.Dispose(); $gi.Dispose()
        & .\build\install_test.exe $stage "$dd\win.img" 1100 --put-winefi build\fakewin.efi | Out-Null
    }
    Copy-Item "$lab\base.img" "$dd\stick.img"
    Copy-Item "$QemuDir\share\edk2-i386-vars.fd" "$dd\vars.fd"
    function Start-Dual([bool]$stick, [string]$tag) {
        $script:port++
        $a = @("-machine", "q35", "-m", "1024", "-smp", "2", "-accel", "tcg", "-display", "none", "-no-shutdown",
               "-qmp", "tcp:127.0.0.1:$script:port,server=on,wait=off", "-serial", "file:$dd\serial-$tag.txt",
               "-drive", "if=pflash,format=raw,readonly=on,file=$QemuDir\share\edk2-x86_64-code.fd", "-drive", "if=pflash,format=raw,file=$dd\vars.fd",
               "-vga", "std", "-drive", "if=none,id=hd,format=raw,file=$dd\win.img", "-device", "ide-hd,drive=hd,bus=ide.0$(if ($stick) { ',bootindex=1' })")
        # with the stick, the internal disk keeps a place in the boot order too (OVMF rebuilds BootOrder from the
        # bootindex list on every start and would drop the disk's entries, as no real PC does)
        if ($stick) { $a += @("-device", "qemu-xhci,id=xhci", "-drive", "if=none,id=stick,format=raw,file=$dd\stick.img", "-device", "usb-storage,bus=xhci.0,drive=stick,bootindex=0") }
        $argline = ($a | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join " "
        $pp = Start-Process -FilePath "$QemuDir\qemu-system-x86_64.exe" -ArgumentList $argline -PassThru -WindowStyle Hidden -RedirectStandardError "$dd\qemu-$tag.txt"
        return @($pp, [Qmp]::new($script:port))
    }
    function Wait-Paper($q, [string]$tag, [int]$secs, [scriptblock]$each) {
        for ($i = 0; $i -lt $secs / 3; $i++) {
            Start-Sleep 3; if ($each) { & $each }
            $q.Shot("$dd\$tag.png"); if (Test-Path "$dd\$tag.png") { $s = Read-Shot "$dd\$tag.png"; if ($s.paper -gt 0.25) { return $true }; Remove-Item "$dd\$tag.png" }
        }
        return $false
    }
    Write-Host "[$($dc.name)]" -ForegroundColor Cyan
    try {
        # A: install from the USB drive
        $pq = Start-Dual $true "A"; $p = $pq[0]; $q = $pq[1]
        if (Wait-Paper $q "A1-boot" 150 $null) {
            $q.Key(@("ctrl", "k")); Start-Sleep 1; $q.Write("install folioos"); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 3
            $q.Shot("$dd\A2-options.png")
            $q.Key(@("shift", "tab")); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 2; $q.Shot("$dd\A3-confirm.png")
            $q.Key(@("shift", "tab")); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 75
            $q.Shot("$dd\A4-installed.png")
        }
        $q.Quit(); if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
        $ErrorActionPreference = "Continue"
        $null = & .\build\fat32_test.exe "$dd\win.img" --fsck 2>&1; $fsok = $LASTEXITCODE -eq 0
        & .\build\fatcat.exe "$dd\win.img" "EFI/FolioOS/FOLIO.ID" "$dd\FOLIO.ID" 2>$null
        $env:FATCAT_VOL = "1"; & .\build\fatcat.exe "$dd\win.img" "EFI/BOOT/BOOTX64.EFI" "$dd\data-efi.bin" 2>$null; $env:FATCAT_VOL = $null
        $ErrorActionPreference = "Stop"
        if ($fsok -and (Test-Path "$dd\FOLIO.ID") -and (Test-Path "$dd\data-efi.bin")) { $dualRes.install = "OK" }
        if ($dc.name -eq "dual-uefi-sata") {
            # nothing outside the new partition and EFI\FolioOS changed: compare every other sector range with the untouched copy
            $null = & .\build\install_test.exe $stage "$dd\win.img" 1400 --compare "$dd\before.img" 2>&1
        } else {
            # the NTFS made by Windows: smaller, consistent, marked for chkdsk, FolioOS right after it
            $null = & .\build\fscheck_test.exe --shrunk "$dd\win.img" 983 2>&1
        }
        if ($LASTEXITCODE -eq 0) { $dualRes.untouched = "OK" }
        # B: only the disk, no key: FolioOS first (boot chooser in the serial log), then "Windows"
        $pq = Start-Dual $false "B"; $p = $pq[0]; $q = $pq[1]
        for ($i = 0; $i -lt 60; $i++) { Start-Sleep 3; if ((Test-Path "$dd\serial-B.txt") -and (Select-String -Quiet -Path "$dd\serial-B.txt" -Pattern "FAKE WINDOWS STARTED")) { break } }
        $q.Shot("$dd\B-other.png"); $q.Quit(); if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
        $ser = Get-Content -Raw "$dd\serial-B.txt" -ErrorAction SilentlyContinue
        if ($ser -and $ser.IndexOf("boot chooser: no key") -ge 0 -and $ser.IndexOf("FAKE WINDOWS STARTED") -gt $ser.IndexOf("boot chooser: no key")) { $dualRes.other = "OK" }
        # C: only the disk, a key during the wait: the editor
        $pq = Start-Dual $false "C"; $p = $pq[0]; $q = $pq[1]
        $ok = Wait-Paper $q "C-writer" 150 { $q.Key(@("spc")) }
        $q.Quit(); if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
        $ser = Get-Content -Raw "$dd\serial-C.txt" -ErrorAction SilentlyContinue
        if ($ok -and $ser -and $ser.IndexOf("boot chooser: key, writing") -ge 0 -and $ser.IndexOf("FAKE WINDOWS") -lt 0) { $dualRes.writer = "OK" }
        if ($dc.name -eq "dual-ntfs-shrink") {
            # D: from the USB drive, remove FolioOS through the screen (the first option); E: only the disk: the firmware
            # starts "Windows" directly (FolioOS's boot entry is gone), and the table is the one Windows made
            $dualRes.removed = "FAIL"; $dualRes.after = "FAIL"
            $pq = Start-Dual $true "D"; $p = $pq[0]; $q = $pq[1]
            if (Wait-Paper $q "D1-boot" 150 $null) {
                $q.Key(@("ctrl", "k")); Start-Sleep 1; $q.Write("remove folioos"); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 3
                $q.Shot("$dd\D2-options.png")
                $q.Key(@("shift", "tab")); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 2; $q.Shot("$dd\D3-confirm.png")
                $q.Key(@("shift", "tab")); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 60
                $q.Shot("$dd\D4-removed.png")
            }
            $q.Quit(); if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
            $gi = [IO.File]::OpenRead("tests\fixtures\ntfs-big.img.gz"); $gz = New-Object IO.Compression.GZipStream($gi, [IO.Compression.CompressionMode]::Decompress)
            $go = [IO.File]::Create("$dd\orig.img"); $gz.CopyTo($go); $go.Dispose(); $gz.Dispose(); $gi.Dispose()
            $ErrorActionPreference = "Continue"
            $null = & .\build\install_test.exe $stage "$dd\win.img" 1100 --same-table "$dd\orig.img" 2>&1; $same = $LASTEXITCODE -eq 0
            $ErrorActionPreference = "Stop"
            if ($same) { $dualRes.removed = "OK" }
            $pq = Start-Dual $false "E"; $p = $pq[0]; $q = $pq[1]
            for ($i = 0; $i -lt 40; $i++) { Start-Sleep 3; if ((Test-Path "$dd\serial-E.txt") -and (Select-String -Quiet -Path "$dd\serial-E.txt" -Pattern "FAKE WINDOWS STARTED")) { break } }
            $q.Shot("$dd\E-windows.png"); $q.Quit(); if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
            $ser = Get-Content -Raw "$dd\serial-E.txt" -ErrorAction SilentlyContinue
            if ($ser -and $ser.IndexOf("FAKE WINDOWS STARTED") -ge 0 -and $ser.IndexOf("boot chooser") -lt 0) { $dualRes.after = "OK" }
            Remove-Item "$dd\orig.img" -ErrorAction SilentlyContinue
        }
    } catch { Write-Host "  $_" -ForegroundColor Red }
    "  install {0} | other partitions untouched {1} | no key: other system {2} | key: FolioOS {3}" -f $dualRes.install, $dualRes.untouched, $dualRes.other, $dualRes.writer
    if ($dualRes.Contains("removed")) { "  removed: table as Windows made it {0} | then the firmware starts Windows directly {1}" -f $dualRes.removed, $dualRes.after }
    if (-not $Keep) { Remove-Item "$dd\win.img", "$dd\before.img", "$dd\stick.img", "$dd\vars.fd" -ErrorAction SilentlyContinue }
    $dualAll += $dualRes
}
}

# ------------------------------------------------------------ dual boot on a PC with BIOS only (ROADMAP phase 3)
# A disk like an old PC with Windows (MBR, the active partition at the start, free space): its MBR code is
# build\fakeother.bin, which prints FAKE OTHER STARTED on the serial port. A: from the USB drive, install next to it
# (FolioOS's MBR boots FolioOS's partition, the original code is kept in FOLIO\OTHERMBR.BIN); B: only the disk, no
# key: FolioOS's chooser starts the original MBR; C: a key: the editor; D: remove from the USB drive; E: only the disk:
# the original MBR runs directly (no chooser) and the table is the one before.
if ((-not $Only -or "dual-bios-ide" -match $Only) -and (Test-Path build\install_test.exe) -and (Test-Path build\fakeother.bin)) {
    $dd = "$lab\dual-bios-ide"; if (Test-Path $dd) { Remove-Item -Recurse -Force $dd }; New-Item -ItemType Directory $dd | Out-Null
    $dualRes = [ordered]@{ name = "dual-bios-ide"; install = "FAIL"; other = "FAIL"; writer = "FAIL"; untouched = "FAIL"; removed = "FAIL"; after = "FAIL" }
    & .\build\install_test.exe $stage "$dd\win.img" 1400 --windows-bios build\fakeother.bin | Out-Null
    Copy-Item "$dd\win.img" "$dd\before.img"
    Copy-Item "$lab\base.img" "$dd\stick.img"
    function Start-Bios([bool]$stick, [string]$tag) {
        $script:port++
        $a = @("-machine", "pc", "-m", "1024", "-accel", "tcg", "-display", "none", "-no-shutdown",
               "-qmp", "tcp:127.0.0.1:$script:port,server=on,wait=off", "-serial", "file:$dd\serial-$tag.txt", "-vga", "std")
        if ($stick) { $a += @("-drive", "if=none,id=stick,format=raw,file=$dd\stick.img", "-device", "ide-hd,drive=stick,bus=ide.0,bootindex=0",
                              "-drive", "if=none,id=hd,format=raw,file=$dd\win.img", "-device", "ide-hd,drive=hd,bus=ide.1,bootindex=1") }
        else { $a += @("-drive", "if=none,id=hd,format=raw,file=$dd\win.img", "-device", "ide-hd,drive=hd,bus=ide.0,bootindex=0") }
        $argline = ($a | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join " "
        $pp = Start-Process -FilePath "$QemuDir\qemu-system-x86_64.exe" -ArgumentList $argline -PassThru -WindowStyle Hidden -RedirectStandardError "$dd\qemu-$tag.txt"
        return @($pp, [Qmp]::new($script:port))
    }
    function Wait-Paper2($q, [string]$tag, [int]$secs, [scriptblock]$each) {
        for ($i = 0; $i -lt $secs / 3; $i++) {
            Start-Sleep 3; if ($each) { & $each }
            $q.Shot("$dd\$tag.png"); if (Test-Path "$dd\$tag.png") { $s = Read-Shot "$dd\$tag.png"; if ($s.paper -gt 0.25) { return $true }; Remove-Item "$dd\$tag.png" }
        }
        return $false
    }
    function Wait-Serial([string]$tag, [string]$pattern, [int]$secs) {
        for ($i = 0; $i -lt $secs / 3; $i++) { Start-Sleep 3; if ((Test-Path "$dd\serial-$tag.txt") -and (Select-String -Quiet -Path "$dd\serial-$tag.txt" -Pattern $pattern)) { return $true } }
        return $false
    }
    Write-Host "[dual-bios-ide]" -ForegroundColor Cyan
    try {
        # A: install next to it from the USB drive
        $pq = Start-Bios $true "A"; $p = $pq[0]; $q = $pq[1]
        if (Wait-Paper2 $q "A1-boot" 180 $null) {
            $q.Key(@("ctrl", "k")); Start-Sleep 1; $q.Write("install folioos"); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 3
            $q.Shot("$dd\A2-options.png")
            $q.Key(@("shift", "tab")); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 2; $q.Shot("$dd\A3-confirm.png")
            $q.Key(@("shift", "tab")); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 180
            $q.Shot("$dd\A4-installed.png")
        }
        $q.Quit(); if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
        $ErrorActionPreference = "Continue"
        $null = & .\build\fat32_test.exe "$dd\win.img" --fsck 2>&1; $fsok = $LASTEXITCODE -eq 0
        & .\build\fatcat.exe "$dd\win.img" "FOLIO/OTHERMBR.BIN" "$dd\OTHERMBR.BIN" 2>$null
        $ErrorActionPreference = "Stop"
        if ($fsok -and (Test-Path "$dd\OTHERMBR.BIN") -and (Get-Item "$dd\OTHERMBR.BIN").Length -ge 440) { $dualRes.install = "OK" }
        $b0 = [IO.File]::ReadAllBytes("$dd\before.img")[446..509]; $b1 = New-Object byte[] 512; $fs = [IO.File]::OpenRead("$dd\win.img"); [void]$fs.Read($b1, 0, 512); $fs.Close()
        $same = $true; for ($k = 0; $k -lt 64; $k++) { if ($b0[$k] -ne 0 -and $b0[$k] -ne $b1[446 + $k]) { $same = $false } }   # the old entries, byte for byte
        if ($same) { $dualRes.untouched = "OK" }
        # B: only the disk, no key: FolioOS's chooser, then the original MBR
        $pq = Start-Bios $false "B"; $p = $pq[0]; $q = $pq[1]
        $null = Wait-Serial "B" "FAKE OTHER STARTED" 180
        $q.Shot("$dd\B-other.png"); $q.Quit(); if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
        $ser = Get-Content -Raw "$dd\serial-B.txt" -ErrorAction SilentlyContinue
        if ($ser -and $ser.IndexOf("boot chooser: no key") -ge 0 -and $ser.IndexOf("FAKE OTHER STARTED") -gt $ser.IndexOf("boot chooser: no key")) { $dualRes.other = "OK" }
        # C: only the disk, a key during the wait: the editor
        $pq = Start-Bios $false "C"; $p = $pq[0]; $q = $pq[1]
        $ok = Wait-Paper2 $q "C-writer" 180 { $q.Key(@("spc")) }
        $q.Quit(); if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
        $ser = Get-Content -Raw "$dd\serial-C.txt" -ErrorAction SilentlyContinue
        if ($ok -and $ser -and $ser.IndexOf("boot chooser: key, writing") -ge 0 -and $ser.IndexOf("FAKE OTHER") -lt 0) { $dualRes.writer = "OK" }
        # D: remove from the USB drive (the first option); E: only the disk: the original MBR directly, the table as before
        $pq = Start-Bios $true "D"; $p = $pq[0]; $q = $pq[1]
        if (Wait-Paper2 $q "D1-boot" 180 $null) {
            $q.Key(@("ctrl", "k")); Start-Sleep 1; $q.Write("remove folioos"); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 3
            $q.Shot("$dd\D2-options.png")
            $q.Key(@("shift", "tab")); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 2; $q.Shot("$dd\D3-confirm.png")
            $q.Key(@("shift", "tab")); Start-Sleep 1; $q.Key(@("ret")); Start-Sleep 90
            $q.Shot("$dd\D4-removed.png")
        }
        $q.Quit(); if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
        $ErrorActionPreference = "Continue"
        $null = & .\build\install_test.exe $stage "$dd\win.img" 1400 --same-table "$dd\before.img" 2>&1; if ($LASTEXITCODE -eq 0) { $dualRes.removed = "OK" }
        $ErrorActionPreference = "Stop"
        $pq = Start-Bios $false "E"; $p = $pq[0]; $q = $pq[1]
        $null = Wait-Serial "E" "FAKE OTHER STARTED" 60
        $q.Shot("$dd\E-other.png"); $q.Quit(); if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
        $ser = Get-Content -Raw "$dd\serial-E.txt" -ErrorAction SilentlyContinue
        if ($ser -and $ser.IndexOf("FAKE OTHER STARTED") -ge 0 -and $ser.IndexOf("boot chooser") -lt 0) { $dualRes.after = "OK" }
    } catch { Write-Host "  $_" -ForegroundColor Red }
    "  install {0} | other entries untouched {1} | no key: other system {2} | key: FolioOS {3}" -f $dualRes.install, $dualRes.untouched, $dualRes.other, $dualRes.writer
    "  removed: table and MBR as before {0} | then the original MBR starts directly {1}" -f $dualRes.removed, $dualRes.after
    if (-not $Keep) { Remove-Item "$dd\win.img", "$dd\before.img", "$dd\stick.img" -ErrorAction SilentlyContinue }
    $dualAll += $dualRes
}

$md = "# FolioOS QEMU lab - $(Get-Date -Format 'yyyy-MM-dd HH:mm')`n`n| Config | Boot | s | Keyboard | Mouse | Sound | Save | Volume | Diagnostics | Disks | Install | Video |`n|---|---|---|---|---|---|---|---|---|---|---|---|`n"
foreach ($r in $results) { $md += "| $($r.config) | $($r.boot) | $($r.seconds) | $($r.keyboard) | $($r.mouse) | $($r.sound) | $($r.save) | $($r.fs) | $($r.diag) | $($r.disks) | $($r.install) | $($r.video) |`n" }
if ($dualAll.Count) {
    $md += "`n## Dual boot next to Windows (phase 2)`n`n| Config | Install | Other partitions untouched (shrink: NTFS consistent) | No key: other system | Key: FolioOS |`n|---|---|---|---|---|`n"
    foreach ($d in $dualAll) { $md += "| $($d.name) | $($d.install) | $($d.untouched) | $($d.other) | $($d.writer) |`n" }
    foreach ($d in $dualAll) { if ($d.Contains("removed")) { $md += "`nUninstall ($($d.name)): table as Windows made it **$($d.removed)**, then Windows starts directly **$($d.after)**`n" } }
}
Set-Content -Encoding utf8 "$lab\report.md" $md
if (-not $Keep) { Remove-Item -Recurse -Force "$lab\base.img", "$lab\inst-uefi.img", "$lab\inst-bios.img", $stage -ErrorAction SilentlyContinue }
$bad = @($results | Where-Object { $_.boot -ne "OK" -or $_.keyboard -ne "OK" -or $_.mouse -ne "OK" -or $_.sound -like "FAIL*" -or $_.save -ne "OK" -or $_.fs -ne "OK" -or $_.diag -ne "OK" -or $_.disks -like "FAIL*" -or $_.install -eq "FAIL" }).Count
foreach ($d in $dualAll) { if (@($d.Keys | Where-Object { $_ -ne "name" -and $d[$_] -ne "OK" }).Count) { $bad++ } }
Write-Host ""; Write-Host "report: $lab\report.md - $($results.Count + $dualAll.Count) configurations, $bad with problems" -ForegroundColor $(if ($bad) { "Yellow" } else { "Green" })
exit $(if ($bad) { 1 } else { 0 })


