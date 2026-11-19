# =============================================================================
# FolioOS - asks Windows itself whether a disk image is sound (run as administrator)
#   .\tools\vhd-chkdsk.ps1 -Image <disk.img>
# Mounts the raw image as a fixed VHD (a copy), runs a read-only chkdsk on every volume Windows can mount (FAT32,
# exFAT, NTFS; the EFI partition too) and prints what it saw. Exit code 0 only if every chkdsk found no problems.
# =============================================================================
param([Parameter(Mandatory = $true)][string]$Image)
$ErrorActionPreference = "Stop"
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw "run as administrator (virtual disks and chkdsk need it)" }
$Image = (Resolve-Path $Image).Path
# a copy at a path that belongs to this run only (an existing .vhd next to the image is never touched)
$vhd = Join-Path ([IO.Path]::GetDirectoryName($Image)) ("chk-" + [guid]::NewGuid().ToString("N") + ".vhd")
$mb = [math]::Ceiling((Get-Item $Image).Length / 1MB)
$bad = 0
try {
    # everything from here on is cleaned up by the finally below, even if creating or filling the VHD fails
    $dp = [IO.Path]::GetTempFileName()
    "create vdisk file=`"$vhd`" maximum=$mb type=fixed" | Set-Content $dp -Encoding Ascii
    diskpart /s $dp | Out-Null; $drc = $LASTEXITCODE; Remove-Item $dp
    if ($drc -ne 0 -or -not (Test-Path $vhd)) { throw "diskpart could not create $vhd (exit $drc)" }
    $src = $null; $dst = $null
    try { $src = [IO.File]::OpenRead($Image); $dst = [IO.File]::OpenWrite($vhd); $src.CopyTo($dst) } finally { if ($dst) { $dst.Dispose() }; if ($src) { $src.Dispose() } }
    $disk = Mount-DiskImage -ImagePath $vhd -PassThru | Get-Disk
    foreach ($p in Get-Partition -DiskNumber $disk.Number) {
        if ($p.Type -eq "Reserved") { continue }
        $esp = $p.GptType -eq "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}"
        if ($esp) { Set-Partition -DiskNumber $disk.Number -PartitionNumber $p.PartitionNumber -GptType "{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}" }   # an ESP gets no letter
        if (-not $p.DriveLetter -or $p.DriveLetter -eq [char]0) { $p | Add-PartitionAccessPath -AssignDriveLetter; Start-Sleep -Milliseconds 500; $p = Get-Partition -DiskNumber $disk.Number -PartitionNumber $p.PartitionNumber }
        $vol = Get-Volume -DriveLetter $p.DriveLetter -ErrorAction SilentlyContinue
        $out = chkdsk "$($p.DriveLetter):" 2>&1; $rc = $LASTEXITCODE
        $line = "  partition $($p.PartitionNumber): $($vol.FileSystem) '$($vol.FileSystemLabel)' $([int]($p.Size / 1MB)) MiB - chkdsk exit $rc"
        if ($rc -eq 0) { Write-Host $line -ForegroundColor Green } else { Write-Host $line -ForegroundColor Red; $out | Select-Object -Last 12 | ForEach-Object { "    $_" }; $bad++ }
    }
} finally { Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null; Remove-Item $vhd -ErrorAction SilentlyContinue }
exit $(if ($bad) { 1 } else { 0 })
