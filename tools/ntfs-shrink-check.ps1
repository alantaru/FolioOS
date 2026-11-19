# =============================================================================
# FolioOS - checks the NTFS shrink (src\core\fscheck.c, ntfs_shrink) with Windows itself (run as administrator)
#   .\tools\ntfs-shrink-check.ps1 [-MiB 30]
# Takes the clean fixture made by Windows (tests\fixtures\ntfs-clean.img.gz), shrinks a copy with
# build\fscheck_test.exe --shrink (volume + partition entry, as the installer does), then mounts both as
# virtual disks and asks Windows: same files byte for byte? does chkdsk accept it (read-only, then /f)?
# is the volume the new size? can a new file be written and read back? A second chkdsk must be clean.
# Images go to $env:FOLIO_WORK (another drive) when set, otherwise %TEMP%.
# =============================================================================
param([int]$MiB = 30)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot)
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw "run as administrator (virtual disks and chkdsk need it)" }
if (-not (Test-Path build\fscheck_test.exe)) { throw "run .\build.ps1 first (build\fscheck_test.exe)" }
$base = if ($env:FOLIO_WORK) { $env:FOLIO_WORK } else { $env:TEMP }
$work = Join-Path $base "ntfs-shrink"
if (Test-Path $work) { Get-ChildItem "$work\*.vhd" -ErrorAction SilentlyContinue | ForEach-Object { Dismount-DiskImage -ImagePath $_.FullName -ErrorAction SilentlyContinue | Out-Null }; Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null
$fails = 0
function Fail([string]$m) { Write-Host "  FAIL: $m" -ForegroundColor Red; $script:fails++ }
function Pass([string]$m) { Write-Host "  ok:   $m" -ForegroundColor Green }

function Expand-Fixture([string]$name, [string]$to) {
    $i = [IO.File]::OpenRead("tests\fixtures\ntfs-$name.img.gz"); $g = New-Object IO.Compression.GZipStream($i, [IO.Compression.CompressionMode]::Decompress)
    $o = [IO.File]::Create($to); $g.CopyTo($o); $o.Dispose(); $g.Dispose(); $i.Dispose()
}
# a raw disk image as a fixed VHD: diskpart makes the VHD (raw data + 512-byte footer), the image goes over the data
function New-Vhd([string]$img) {
    $vhd = [IO.Path]::ChangeExtension($img, ".vhd")
    $mb = [int]((Get-Item $img).Length / 1MB)
    "create vdisk file=`"$vhd`" maximum=$mb type=fixed" | Set-Content "$work\dp.txt" -Encoding Ascii
    diskpart /s "$work\dp.txt" | Out-Null
    $src = [IO.File]::OpenRead($img); $dst = [IO.File]::OpenWrite($vhd)
    $src.CopyTo($dst); $dst.Dispose(); $src.Dispose()
    return $vhd
}
function Mount-Vhd([string]$vhd) {
    $img = Get-DiskImage -ImagePath $vhd
    if (-not $img.Attached) { $img = Mount-DiskImage -ImagePath $vhd -PassThru }   # chkdsk /x only takes the volume offline
    $disk = $img | Get-Disk
    $p = Get-Partition -DiskNumber $disk.Number | Where-Object { $_.Type -ne "Reserved" } | Select-Object -Last 1
    if (-not $p.DriveLetter -or $p.DriveLetter -eq [char]0) { $p | Add-PartitionAccessPath -AssignDriveLetter; Start-Sleep -Milliseconds 500; $p = Get-Partition -DiskNumber $disk.Number -PartitionNumber $p.PartitionNumber }
    return @{ letter = $p.DriveLetter; size = $p.Size }
}
function Get-Hashes([string]$root) {
    $h = @{}
    Get-ChildItem -Recurse -File -Force $root -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notmatch "System Volume Information" } |
        ForEach-Object { $h[$_.FullName.Substring(3)] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
    return $h
}

Write-Host "NTFS shrink checked by Windows ($MiB MiB)"
$orig = "$work\orig.img"; $shr = "$work\shrunk.img"
Expand-Fixture "clean" $orig
Copy-Item $orig $shr
& .\build\fscheck_test.exe --shrink $shr $MiB
if ($LASTEXITCODE -ne 0) { throw "fscheck_test --shrink failed" }

$v1 = New-Vhd $orig
$m = Mount-Vhd $v1
$before = Get-Hashes "$($m.letter):\"
Dismount-DiskImage -ImagePath $v1 | Out-Null
Write-Host "  original: $($before.Count) files"

$v2 = New-Vhd $shr
try {
    $m = Mount-Vhd $v2; $L = "$($m.letter):"
    if ($m.size -eq [int64]$MiB * 1MB) { Pass "partition is $MiB MiB" } else { Fail "partition size $($m.size)" }
    $vol = Get-Volume -DriveLetter $m.letter
    if ($vol.FileSystem -eq "NTFS" -and $vol.Size -le [int64]$MiB * 1MB -and $vol.Size -gt ([int64]$MiB - 1) * 1MB) { Pass "Windows mounts it as NTFS of $([math]::Round($vol.Size / 1MB, 2)) MiB" } else { Fail "volume: $($vol.FileSystem) $($vol.Size)" }
    $dq = fsutil dirty query $L
    if ($LASTEXITCODE -ne 0 -or "$dq" -notmatch "(está sujo|is dirty)" -or "$dq" -match "(NÃO|NOT)") { Fail "volume should be marked for chkdsk: $dq" } else { Pass "marked for chkdsk (dirty)" }
    $after = Get-Hashes "$L\"
    $diff = @($before.Keys | Where-Object { $after[$_] -ne $before[$_] })
    if ($after.Count -eq $before.Count -and $diff.Count -eq 0) { Pass "all $($after.Count) files byte for byte the same" } else { Fail "files differ: $($diff -join ', ') ($($after.Count) vs $($before.Count))" }
    $out = chkdsk $L 2>&1; $rc = $LASTEXITCODE
    $out | Set-Content "$work\chkdsk-readonly.txt"
    if ($rc -eq 0) { Pass "chkdsk (read-only): no problems" } else { Fail "chkdsk (read-only) exit $rc - see $work\chkdsk-readonly.txt" }
    $out = chkdsk $L /f /x 2>&1; $rc = $LASTEXITCODE
    $out | Set-Content "$work\chkdsk-fix.txt"
    if ($rc -eq 0) { Pass "chkdsk /f: nothing to fix" } else { Fail "chkdsk /f exit $rc - see $work\chkdsk-fix.txt" }
    $m = Mount-Vhd $v2; $L = "$($m.letter):"            # /x dismounts the volume
    $dq = fsutil dirty query $L
    if ("$dq" -match "(NÃO|NOT)") { Pass "chkdsk cleared the mark" } else { Fail "still dirty after chkdsk /f: $dq" }
    $free = (Get-Volume -DriveLetter $m.letter).SizeRemaining
    $b = New-Object byte[] ([int][math]::Min(2MB, $free - 256KB)); (New-Object Random 7).NextBytes($b)
    [IO.File]::WriteAllBytes("$L\after-shrink.bin", $b)
    Write-VolumeCache $m.letter
    $back = [IO.File]::ReadAllBytes("$L\after-shrink.bin")
    if ([Linq.Enumerable]::SequenceEqual($b, $back)) { Pass "new $([int]($b.Length / 1KB)) KB file written and read back" } else { Fail "new file differs" }
    $out = chkdsk $L 2>&1; $rc = $LASTEXITCODE
    $out | Set-Content "$work\chkdsk-final.txt"
    if ($rc -eq 0) { Pass "chkdsk after writing: no problems" } else { Fail "final chkdsk exit $rc - see $work\chkdsk-final.txt" }
} finally { Dismount-DiskImage -ImagePath $v2 -ErrorAction SilentlyContinue | Out-Null }

# ---- 2. the whole installer on a disk made by Windows with no free space (EFI + NTFS): shrink, then install after it
Write-Host "Installer next to Windows by shrinking NTFS (tests\fixtures\ntfs-big)"
$big = "$work\big.img"; $bigOrig = "$work\big-orig.img"
Expand-Fixture "big" $big
Copy-Item $big $bigOrig
if (-not (Test-Path "$work\src")) { Copy-Item -Recurse build\usb_files "$work\src" }
& .\build\install_test.exe "$work\src" $big 1100 --shrink-ntfs | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { throw "install_test --shrink-ntfs failed" }
function Get-Vols([string]$vhd) {    # letter for every partition Windows can mount (the EFI partition is left alone)
    $img = Get-DiskImage -ImagePath $vhd
    if (-not $img.Attached) { $img = Mount-DiskImage -ImagePath $vhd -PassThru }
    $disk = $img | Get-Disk
    $r = @()
    foreach ($p in Get-Partition -DiskNumber $disk.Number) {
        if ($p.GptType -eq "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}" -or $p.Type -eq "Reserved") { continue }
        if (-not $p.DriveLetter -or $p.DriveLetter -eq [char]0) { $p | Add-PartitionAccessPath -AssignDriveLetter; Start-Sleep -Milliseconds 500; $p = Get-Partition -DiskNumber $disk.Number -PartitionNumber $p.PartitionNumber }
        $r += @{ letter = $p.DriveLetter; size = $p.Size; fs = (Get-Volume -DriveLetter $p.DriveLetter).FileSystem; label = (Get-Volume -DriveLetter $p.DriveLetter).FileSystemLabel }
    }
    return $r
}
$vb = New-Vhd $bigOrig
$v = Get-Vols $vb; $ntfs = $v | Where-Object { $_.fs -eq "NTFS" }
$before = Get-Hashes "$($ntfs.letter):\"
Dismount-DiskImage -ImagePath $vb | Out-Null
$vi = New-Vhd $big
try {
    $v = Get-Vols $vi
    $ntfs = $v | Where-Object { $_.fs -eq "NTFS" }; $fol = $v | Where-Object { $_.fs -eq "FAT32" }
    if ($ntfs -and $fol) { Pass "Windows sees NTFS $([int]($ntfs.size / 1MB)) MiB and FAT32 '$($fol.label)' $([int]($fol.size / 1MB)) MiB" } else { Fail "volumes: $(($v | ForEach-Object { "$($_.fs) $($_.size)" }) -join ', ')" }
    $after = Get-Hashes "$($ntfs.letter):\"
    $diff = @($before.Keys | Where-Object { $after[$_] -ne $before[$_] })
    if ($after.Count -eq $before.Count -and $diff.Count -eq 0) { Pass "Windows files: all $($after.Count) the same" } else { Fail "Windows files differ: $($diff -join ', ')" }
    $out = chkdsk "$($ntfs.letter):" 2>&1; $rc = $LASTEXITCODE; $out | Set-Content "$work\chkdsk-installed-ntfs.txt"
    if ($rc -eq 0) { Pass "chkdsk on the shrunk NTFS: no problems" } else { Fail "chkdsk NTFS exit $rc - see $work\chkdsk-installed-ntfs.txt" }
    $out = chkdsk "$($fol.letter):" 2>&1; $rc = $LASTEXITCODE; $out | Set-Content "$work\chkdsk-installed-fat.txt"
    if ($rc -eq 0) { Pass "chkdsk on FolioOS's FAT32: no problems" } else { Fail "chkdsk FAT32 exit $rc - see $work\chkdsk-installed-fat.txt" }
    if (Test-Path "$($fol.letter):\FOLIO") { Pass "FolioOS files readable by Windows ($((Get-ChildItem -Recurse -File "$($fol.letter):\").Count) files)" } else { Fail "no FOLIO folder on the new partition" }
} finally { Dismount-DiskImage -ImagePath $vi -ErrorAction SilentlyContinue | Out-Null }

# ---- 3. uninstall: texts to the USB drive, FolioOS's partition and \EFI\FolioOS gone, NTFS back to its old size
Write-Host "Uninstall: the NTFS gets its space back"
& .\build\install_test.exe "$work\src" $big 1100 --remove | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { throw "install_test --remove failed" }
Remove-Item "$work\big.vhd" -ErrorAction SilentlyContinue
$vu = New-Vhd $big
try {
    $v = Get-Vols $vu
    $ntfs = @($v | Where-Object { $_.fs -eq "NTFS" }); $fat = @($v | Where-Object { $_.fs -eq "FAT32" })
    $origSize = (Get-Item $bigOrig).Length   # the partition size Windows made, from the original disk
    if ($ntfs.Count -eq 1 -and $fat.Count -eq 0) { Pass "Windows sees only the NTFS again ($([int]($ntfs[0].size / 1MB)) MiB), no FolioOS partition" } else { Fail "volumes after uninstall: $(($v | ForEach-Object { "$($_.fs) $($_.size)" }) -join ', ')" }
    $after = Get-Hashes "$($ntfs[0].letter):\"
    $diff = @($before.Keys | Where-Object { $after[$_] -ne $before[$_] })
    if ($after.Count -eq $before.Count -and $diff.Count -eq 0) { Pass "Windows files: all $($after.Count) the same" } else { Fail "Windows files differ after uninstall: $($diff -join ', ')" }
    $out = chkdsk "$($ntfs[0].letter):" 2>&1; $rc = $LASTEXITCODE; $out | Set-Content "$work\chkdsk-uninstalled.txt"
    if ($rc -eq 0) { Pass "chkdsk on the grown NTFS: no problems" } else { Fail "chkdsk after uninstall exit $rc - see $work\chkdsk-uninstalled.txt" }
    $out = chkdsk "$($ntfs[0].letter):" /f /x 2>&1; $rc = $LASTEXITCODE; $out | Set-Content "$work\chkdsk-uninstalled-fix.txt"
    if ($rc -eq 0) { Pass "chkdsk /f: nothing to fix" } else { Fail "chkdsk /f after uninstall exit $rc - see $work\chkdsk-uninstalled-fix.txt" }
} finally { Dismount-DiskImage -ImagePath $vu -ErrorAction SilentlyContinue | Out-Null }
# the partition table and the NTFS boot sector are exactly the ones Windows made
& .\build\install_test.exe "$work\src" $big 1100 --same-table $bigOrig | Select-Object -Last 1
if ($LASTEXITCODE -eq 0) { Pass "partition table and NTFS size exactly as Windows made them" } else { Fail "partition table differs from the original" }

# ---- 4. FAT32 and exFAT made by Windows (tests\fixtures\fat32-data, exfat-data): shrink, then grow, without moving data
foreach ($kind in "FAT32", "exFAT") {
Write-Host "$kind shrink and grow checked by Windows"
$tag = $kind.ToLower(); $fo = "$work\$tag-orig.img"; $fx = "$work\$tag.img"
$i = [IO.File]::OpenRead("tests\fixtures\$tag-data.img.gz"); $g = New-Object IO.Compression.GZipStream($i, [IO.Compression.CompressionMode]::Decompress)
$o = [IO.File]::Create($fo); $g.CopyTo($o); $o.Dispose(); $g.Dispose(); $i.Dispose()
Copy-Item $fo $fx
$vf = New-Vhd $fo; $v = Get-Vols $vf; $fb = Get-Hashes "$(($v | Where-Object { $_.fs -eq $kind }).letter):\"; Dismount-DiskImage -ImagePath $vf | Out-Null
foreach ($step in @(@{ mib = 400; what = "shrunk" }, @{ mib = 900; what = "grown" })) {
    & .\build\fscheck_test.exe --resize $fx $step.mib
    if ($LASTEXITCODE -ne 0) { Fail "fscheck_test --resize $($step.mib)"; break }
    Remove-Item "$work\$tag.vhd" -ErrorAction SilentlyContinue
    $vf = New-Vhd $fx
    try {
        $v = Get-Vols $vf; $fv = $v | Where-Object { $_.fs -eq $kind }
        if ($fv -and [math]::Abs($fv.size - [int64]$step.mib * 1MB) -lt 2MB) { Pass "$kind $($step.what) to $($step.mib) MiB: Windows mounts it" } else { Fail "$kind after $($step.what): $(($v | ForEach-Object { "$($_.fs) $($_.size)" }) -join ', ')"; continue }
        $fa = Get-Hashes "$($fv.letter):\"
        $diff = @($fb.Keys | Where-Object { $fa[$_] -ne $fb[$_] })
        if ($fa.Count -eq $fb.Count -and $diff.Count -eq 0) { Pass "all $($fa.Count) files the same" } else { Fail "files differ: $($diff -join ', ')" }
        $out = chkdsk "$($fv.letter):" 2>&1; $rc = $LASTEXITCODE; $out | Set-Content "$work\chkdsk-$tag-$($step.mib).txt"
        if ($rc -eq 0) { Pass "chkdsk: no problems" } else { Fail "chkdsk exit $rc - see $work\chkdsk-$tag-$($step.mib).txt" }
        if ($step.what -eq "grown") {   # the new space can be used
            $b = New-Object byte[] (64MB); (New-Object Random 3).NextBytes($b)
            [IO.File]::WriteAllBytes("$($fv.letter):\after-grow.bin", $b); Write-VolumeCache $fv.letter
            if ([Linq.Enumerable]::SequenceEqual($b, [IO.File]::ReadAllBytes("$($fv.letter):\after-grow.bin"))) { Pass "a 64 MB file written and read back" } else { Fail "new file differs" }
            $out = chkdsk "$($fv.letter):" 2>&1; if ($LASTEXITCODE -eq 0) { Pass "chkdsk after writing: no problems" } else { Fail "chkdsk after writing exit $LASTEXITCODE" }
        }
    } finally { Dismount-DiskImage -ImagePath $vf -ErrorAction SilentlyContinue | Out-Null }
}
}

if ($fails) { Write-Host "$fails problem(s); images and chkdsk logs kept in $work" -ForegroundColor Red; exit 1 }
Remove-Item -Recurse -Force $work
Write-Host "NTFS shrink: Windows agrees" -ForegroundColor Green
