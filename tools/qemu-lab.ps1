# =============================================================================
# FolioOS - QEMU lab: boots the real FolioOS on emulated machines and checks it end to end
#   .\tools\qemu-lab.ps1                 every configuration in the matrix
#   .\tools\qemu-lab.ps1 -Only q35-std   configurations whose name matches
#   .\tools\qemu-lab.ps1 -List           list the matrix
# Needs QEMU (winget install SoftwareFreedomConservancy.QEMU) and a previous .\build.ps1.
# For each machine: boots a copy of the FAT32 drive image, then through QMP (QEMU's control
# protocol) takes screenshots, presses keys, moves and clicks the mouse, plays the sound test
# into a .wav file, and finally reads FOLIO\DIAGNOSTICS.TXT from the disk image.
# Results: build\lab\report.md and build\lab\<config>\*.png
# =============================================================================
param([string]$Only = "", [switch]$List, [int]$BootWait = 0)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot)
$QemuDir = "C:\Program Files\qemu"
if (-not (Test-Path "$QemuDir\qemu-system-x86_64.exe")) { $c = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue; if ($c) { $QemuDir = Split-Path $c.Source } else { throw "QEMU not found (winget install SoftwareFreedomConservancy.QEMU)" } }
$lab = "build\lab"
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
    @{ name = "bios-pc-cirrus-ide-ich9";  sys = "x86_64";  fw = "bios"; machine = "pc";   vga = "-vga cirrus";            input = "ps2";    audio = "ich9"; disk = "ide" }
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
foreach ($x in "CONFIG.INI", "DIAGNOSTICS.TXT", "DRAFT.MD", "TABS", "VERSIONS") { if (Test-Path "$stage\FOLIO\$x") { Remove-Item -Recurse -Force "$stage\FOLIO\$x" } }
Set-Content -Encoding ascii "$stage\FOLIO\CONFIG.INI" "language=en`r`nsplash=1`r`nsplash_seconds=1`r`nvolume=100`r`ndebug_serial=1`r`n"
Set-Content -Encoding ascii "$stage\DOCUMENTS\Lab.md" "# QEMU lab`r`n`r`nThe quick brown fox jumps over the lazy dog.`r`n"
& .\build\mkimg.exe $stage "$lab\base.img" 256 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "mkimg failed" }
# boot by BIOS too (MBR/VBR code only; the FAT32 and the UEFI boot stay the same)
if (Test-Path build\biosboot.exe) { & .\build\biosboot.exe "$lab\base.img" | Out-Null; if ($LASTEXITCODE -ne 0) { throw "biosboot failed" } }

# ------------------------------------------------------------ run one configuration
$port = 4460
$results = @()
foreach ($m in $matrix) {
    if ($Only -and $m.name -notmatch $Only) { continue }
    $dir = "$lab\$($m.name)"; if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }; New-Item -ItemType Directory $dir | Out-Null
    $img = "$dir\disk.img"; Copy-Item "$lab\base.img" $img
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
    else { $a += @("-device", "qemu-xhci,id=xhci", "-drive", "if=none,id=stick,format=raw,file=$img", "-device", "usb-storage,bus=xhci.0,drive=stick,bootindex=0") }
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
    $r = [ordered]@{ config = $m.name; boot = "FAIL"; keyboard = "-"; mouse = "-"; sound = "-"; save = "-"; fs = "-"; video = "-"; diag = "-"; seconds = 0 }
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
    if (Test-Path "$dir\DIAGNOSTICS.TXT") {
        $d = Get-Content -Raw -Encoding UTF8 "$dir\DIAGNOSTICS.TXT"
        $r.diag = "OK"
        $v = [regex]::Match($d, "in use:[^\r\n]*"); if ($v.Success) { $r.video = $v.Value.Substring(7).Trim() }
    }
    $ErrorActionPreference = "Stop"
    $results += [pscustomobject]$r
    "  boot {0} ({1}s) | keyboard {2} | mouse {3} | sound {4} | save {5} | volume {6} | diagnostics {7} | video {8}" -f $r.boot, $r.seconds, $r.keyboard, $r.mouse, $r.sound, $r.save, $r.fs, $r.diag, $r.video
}

# ------------------------------------------------------------ report
$md = "# FolioOS QEMU lab - $(Get-Date -Format 'yyyy-MM-dd HH:mm')`n`n| Config | Boot | s | Keyboard | Mouse | Sound | Save | Volume | Diagnostics | Video |`n|---|---|---|---|---|---|---|---|---|---|`n"
foreach ($r in $results) { $md += "| $($r.config) | $($r.boot) | $($r.seconds) | $($r.keyboard) | $($r.mouse) | $($r.sound) | $($r.save) | $($r.fs) | $($r.diag) | $($r.video) |`n" }
Set-Content -Encoding utf8 "$lab\report.md" $md
$bad = @($results | Where-Object { $_.boot -ne "OK" -or $_.keyboard -ne "OK" -or $_.mouse -ne "OK" -or $_.sound -like "FAIL*" -or $_.save -ne "OK" -or $_.fs -ne "OK" -or $_.diag -ne "OK" }).Count
Write-Host ""; Write-Host "report: $lab\report.md - $($results.Count) configurations, $bad with problems" -ForegroundColor $(if ($bad) { "Yellow" } else { "Green" })
exit $(if ($bad) { 1 } else { 0 })


