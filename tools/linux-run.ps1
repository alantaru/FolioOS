# =============================================================================
# FolioOS - runs shell commands in a real Linux (Alpine, in QEMU) against a disk image
#   .\tools\linux-run.ps1 -Disk <image> -Script <file.sh> [-Out <log>]
# Used to make ext4 test fixtures with Linux's own mkfs.ext4 and to judge FolioOS's changes with e2fsck,
# the way tools\ntfs-fixtures.ps1 and ntfs-shrink-check.ps1 use Windows for NTFS. Nothing is installed on
# this computer: the Alpine "virt" ISO (checked by SHA-256) is kept in $env:FOLIO_WORK\alpine (or %TEMP%).
# The disk image is the VM's /dev/vda. The script runs as root with e2fsprogs installed from the ISO; its
# output is printed between markers, and the exit code of the script is this script's exit code.
# =============================================================================
param([Parameter(Mandatory = $true)][string]$Disk, [Parameter(Mandatory = $true)][string]$Script, [string]$Out = "", [int]$Port = 47321)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot)
$ver = "3.24.4"; $sha = "157d353f8e54252cad2d9812a0d56bb7fa2231fc154a110e1d180d7480ef8b4f"
$base = if ($env:FOLIO_WORK) { $env:FOLIO_WORK } else { $env:TEMP }
$dir = Join-Path $base "alpine"; New-Item -ItemType Directory -Force $dir | Out-Null
$iso = Join-Path $dir "alpine-virt-$ver-x86_64.iso"
if (-not (Test-Path $iso) -or (Get-FileHash $iso -Algorithm SHA256).Hash -ne $sha.ToUpper()) {
    Write-Host "downloading Alpine $ver (virt, ~70 MB)"
    Invoke-WebRequest -UseBasicParsing "https://dl-cdn.alpinelinux.org/alpine/v$($ver.Substring(0, 4))/releases/x86_64/alpine-virt-$ver-x86_64.iso" -OutFile $iso
    if ((Get-FileHash $iso -Algorithm SHA256).Hash -ne $sha.ToUpper()) { Remove-Item $iso; throw "Alpine ISO: SHA-256 does not match" }
}
$QemuDir = "C:\Program Files\qemu"
if (-not (Test-Path "$QemuDir\qemu-system-x86_64.exe")) { $c = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue; if ($c) { $QemuDir = Split-Path $c.Source } else { throw "QEMU not found" } }
$Disk = (Resolve-Path $Disk).Path
$body = (Get-Content -Raw $Script) -replace "`r", ""

$a = @("-machine", "pc", "-m", "512", "-accel", "tcg", "-display", "none", "-no-reboot",
       "-cdrom", $iso, "-boot", "d", "-drive", "file=$($Disk -replace ',', ',,'),format=raw,if=virtio,cache=writeback",   # QEMU: ",," is a literal comma
       "-serial", "tcp:127.0.0.1:${Port},server=on,wait=on")
$argline = ($a | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join " "
$p = Start-Process -FilePath "$QemuDir\qemu-system-x86_64.exe" -ArgumentList $argline -PassThru -WindowStyle Hidden
$log = New-Object System.Text.StringBuilder
try {
    $tc = $null
    for ($i = 0; $i -lt 50 -and -not $tc; $i++) { try { $tc = New-Object Net.Sockets.TcpClient("127.0.0.1", $Port) } catch { Start-Sleep -Milliseconds 200 } }
    if (-not $tc) { throw "no serial connection to QEMU" }
    $s = $tc.GetStream(); $s.ReadTimeout = 500
    $buf = New-Object byte[] 65536
    $script:seen = ""
    function Read-Until([string]$pattern, [int]$secs) {
        $end = (Get-Date).AddSeconds($secs)
        while ((Get-Date) -lt $end) {
            try { $n = $s.Read($buf, 0, $buf.Length) } catch { $n = 0 }
            if ($n -gt 0) { $t = [Text.Encoding]::UTF8.GetString($buf, 0, $n); [void]$log.Append($t); $script:seen += $t }
            if ($script:seen -match $pattern) { $m = $script:seen; $script:seen = ""; return $m }
            if ($p.HasExited) { throw "QEMU exited" }
        }
        throw "timeout waiting for '$pattern' (last output: $($script:seen.Substring([Math]::Max(0, $script:seen.Length - 300))))"
    }
    function Send([string]$line) { $b = [Text.Encoding]::UTF8.GetBytes($line + "`n"); $s.Write($b, 0, $b.Length); $s.Flush() }
    $r = Read-Until "login:|boot: *$" 240
    if ($r -notmatch "login:") { Send ""; Read-Until "login:" 240 | Out-Null }   # ISOLINUX sometimes waits at "boot:": Enter = default
    Send "root"
    Read-Until "# $" 60 | Out-Null
    Send "stty -echo; export PS1='# '; apk add -q e2fsprogs e2fsprogs-extra >/dev/null 2>&1; echo APK=`$?"
    $r = Read-Until "APK=\d+" 120
    if ($r -notmatch "APK=0") { throw "apk add e2fsprogs failed" }
    # the script goes in base64 (no quoting problems over the serial line), then runs; markers around its output
    $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($body))
    Send "rm -f /tmp/s.b64"
    for ($i = 0; $i -lt $b64.Length; $i += 600) { Send ("echo " + $b64.Substring($i, [Math]::Min(600, $b64.Length - $i)) + " >> /tmp/s.b64") }
    # markers unique to this run, each on a line of its own: what the script prints can never be taken for them
    $tok = [guid]::NewGuid().ToString("N").Substring(0, 16)
    Send "base64 -d /tmp/s.b64 > /tmp/s.sh; echo; echo ===BEGIN-$tok===; sh /tmp/s.sh 2>&1; echo; echo ===END-$tok=`$?==="
    $r = Read-Until "(?m)^===END-$tok=\d+===\r?$" 900
    $mEnd = [regex]::Match($r, "(?m)^===END-$tok=(\d+)===\r?$"); $mBeg = [regex]::Match($r, "(?m)^===BEGIN-$tok===\r?$")
    if (-not $mEnd.Success -or -not $mBeg.Success -or $mBeg.Index -gt $mEnd.Index) { throw "guest output without the run's BEGIN/END markers" }
    $rc = [int]$mEnd.Groups[1].Value
    $outText = $r.Substring($mBeg.Index + $mBeg.Length, $mEnd.Index - $mBeg.Index - $mBeg.Length).TrimStart("`r", "`n") -replace "`r", ""
    Send "sync; poweroff"
    # the image is only good if the guest finished writing and powered off by itself
    if (-not $p.WaitForExit(60000)) { Stop-Process -Id $p.Id -Force; Write-Host "the guest did not power off: the image may be incomplete" -ForegroundColor Red; $rc = 99 }
} finally {
    if ($tc) { $tc.Close() }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    if ($Out) { Set-Content -Path $Out -Value $log.ToString() -Encoding utf8 }
}
Write-Output $outText
exit $rc
