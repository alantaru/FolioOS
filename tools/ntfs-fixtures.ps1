# =============================================================================
# FolioOS - NTFS test fixtures made by Windows itself (run once, as administrator)
#   .\tools\ntfs-fixtures.ps1
# Creates small virtual disks, lets Windows partition and format them as NTFS, fills them with
# files, and saves each disk as tests\fixtures\ntfs-<name>.img.gz plus the facts Windows reports
# about it (tests\fixtures\ntfs-<name>.txt). tools\fscheck_test.c checks src\core\fscheck.c
# against those facts, so the NTFS reader is validated against Windows, not against itself.
#   clean  GPT, scattered files, a hiberfil.sys full of zeros (not hibernated)   -> flags=0
#   dirty  MBR, same files, "fsutil dirty set"                                   -> flags=1
#   hiber  GPT, same files, hiberfil.sys starting with "HIBR" (hibernated/Fast Startup) -> flags=2
#   big    GPT, 1100 MB: EFI system partition (FAT32) + NTFS filling the rest, no free space (install by shrinking) -> flags=0
# The fixtures are committed; the build only reads them (no administrator needed there).
# =============================================================================
param([string]$Only = "")   # -Only clean|dirty|hiber|big: just that one
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot)
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw "run as administrator (diskpart, volume handles and fsutil need it)" }
$out = "tests\fixtures"
New-Item -ItemType Directory -Force $out | Out-Null
$work = Join-Path $(if ($env:FOLIO_WORK) { $env:FOLIO_WORK } else { $env:TEMP }) "folio-ntfs"
New-Item -ItemType Directory -Force $work | Out-Null

Add-Type @"
using System; using System.Runtime.InteropServices; using Microsoft.Win32.SafeHandles;
public static class NtfsFacts {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inb, int inl, byte[] outb, int outl, out int got, IntPtr ov);
    // returns { cluster size, total clusters, last used cluster (from the volume bitmap), MFT start }
    public static long[] Query(char letter) {
        using (var h = CreateFile(@"\\.\" + letter + ":", 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (h.IsInvalid) throw new Exception("open volume: " + Marshal.GetLastWin32Error());
            byte[] vd = new byte[256]; int got;
            if (!DeviceIoControl(h, 0x90064, null, 0, vd, vd.Length, out got, IntPtr.Zero)) throw new Exception("NTFS volume data: " + Marshal.GetLastWin32Error());
            long nclus = BitConverter.ToInt64(vd, 16), cs = BitConverter.ToUInt32(vd, 44), mft = BitConverter.ToInt64(vd, 64);
            byte[] bm = new byte[16 + (nclus + 7) / 8 + 64];
            if (!DeviceIoControl(h, 0x9006F, new byte[8], 8, bm, bm.Length, out got, IntPtr.Zero)) throw new Exception("volume bitmap: " + Marshal.GetLastWin32Error());
            long start = BitConverter.ToInt64(bm, 0), bits = BitConverter.ToInt64(bm, 8), last = -1;
            for (long c = 0; c < bits; c++) if ((bm[16 + c / 8] >> (int)(c % 8) & 1) != 0) last = start + c;
            return new long[] { cs, nclus, last, mft };
        }
    }
}
"@

function Add-Files([string]$root) {
    # many entries in the root so its index spills out of the MFT record (INDX blocks), and
    # files created then deleted so the used clusters are scattered, not one block at the start
    $rnd = New-Object System.Random 4242
    # non-zero but compressible content (random bytes would make each fixture ~10 MB)
    function Fill([int]$n, [int]$seed) { $t = "FolioOS fixture $seed`n"; ,[Text.Encoding]::ASCII.GetBytes(($t * ([Math]::Ceiling($n / $t.Length))).Substring(0, $n)) }
    for ($i = 0; $i -lt 80; $i++) {
        $b = Fill ($rnd.Next(1, 40) * 1024) $i
        [IO.File]::WriteAllBytes("$root\file$('{0:D3}' -f $i).bin", $b)
    }
    New-Item -ItemType Directory "$root\Documents\Chapters" -Force | Out-Null
    for ($i = 0; $i -lt 30; $i++) { Set-Content "$root\Documents\Chapters\ch$i.md" ("# Chapter $i`n" + ("text " * (200 * ($i + 1)))) -Encoding Ascii }
    $big = Fill 6MB 1000
    [IO.File]::WriteAllBytes("$root\big.bin", $big)
    for ($i = 0; $i -lt 80; $i += 2) { Remove-Item "$root\file$('{0:D3}' -f $i).bin" }
    $tail = Fill 3MB 2000
    [IO.File]::WriteAllBytes("$root\Documents\tail.bin", $tail)
}

function New-Fixture([string]$name, [string]$style, [scriptblock]$extra, [int]$mb = 48, [switch]$esp, [string]$fs = "NTFS") {
    $pre = if ($fs -eq "NTFS") { "ntfs" } else { $fs.ToLower() }   # ntfs-<name>, fat32-<name>, exfat-<name>
    $vhd = Join-Path $work "$name.vhd"
    if (Test-Path $vhd) { Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null; Remove-Item $vhd }
    "create vdisk file=`"$vhd`" maximum=$mb type=fixed" | Set-Content "$work\dp.txt" -Encoding Ascii
    diskpart /s "$work\dp.txt" | Out-Null
    if (-not (Test-Path $vhd)) { throw "diskpart did not create $vhd" }
    try {
        $disk = Mount-DiskImage -ImagePath $vhd -PassThru | Get-Disk
        Initialize-Disk -Number $disk.Number -PartitionStyle $style
        if ($esp) {   # an EFI system partition first, as on a PC with Windows (FAT32, then the ESP type)
            $e = New-Partition -DiskNumber $disk.Number -Size 100MB -AssignDriveLetter
            Format-Volume -Partition $e -FileSystem FAT32 -NewFileSystemLabel "SYSTEM" -Confirm:$false -Force | Out-Null
            $eL = (Get-Partition -DiskNumber $disk.Number -PartitionNumber $e.PartitionNumber).DriveLetter
            New-Item -ItemType Directory -Force "${eL}:\EFI\Microsoft\Boot", "${eL}:\EFI\Boot" | Out-Null
            Set-Content "${eL}:\EFI\Microsoft\Boot\bootmgfw.efi" "stand-in for Windows Boot Manager" -Encoding Ascii
            Remove-PartitionAccessPath -DiskNumber $disk.Number -PartitionNumber $e.PartitionNumber -AccessPath "${eL}:\"
            Set-Partition -DiskNumber $disk.Number -PartitionNumber $e.PartitionNumber -GptType "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}"
        }
        $p = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter
        Format-Volume -Partition $p -FileSystem $fs -NewFileSystemLabel "FOLIOTEST" -Confirm:$false -Force | Out-Null
        $L = (Get-Partition -DiskNumber $disk.Number -PartitionNumber $p.PartitionNumber).DriveLetter
        Add-Files "${L}:"
        & $extra "${L}:"
        Write-VolumeCache $L
        $f = if ($fs -eq "NTFS") { [NtfsFacts]::Query($L) } else { @(0, 0, 0, 0) }
        $dq = fsutil dirty query "${L}:"
        Write-Host "  $name : $dq"
    } finally { Dismount-DiskImage -ImagePath $vhd | Out-Null }
    # a fixed VHD is the raw disk followed by a 512-byte footer
    $len = (Get-Item $vhd).Length - 512
    $src = [IO.File]::OpenRead($vhd)
    $dst = [IO.File]::Create("$out\$pre-$name.img.gz")
    $gz = New-Object IO.Compression.GZipStream($dst, [IO.Compression.CompressionLevel]::Optimal)
    $buf = New-Object byte[] (1MB); $left = $len
    while ($left -gt 0) { $k = $src.Read($buf, 0, [Math]::Min($buf.Length, $left)); $gz.Write($buf, 0, $k); $left -= $k }
    $gz.Dispose(); $dst.Dispose(); $src.Dispose()
    Remove-Item $vhd
    if ($fs -ne "NTFS") {
        "fs=$($fs.ToLower()) bytes=$len" | Set-Content "$out\$pre-$name.txt" -Encoding Ascii
        Write-Host "  $pre-$name -> $out\$pre-$name.img.gz ($([int]((Get-Item "$out\$pre-$name.img.gz").Length / 1KB)) KB)"
        return
    }
    $flags = @{ clean = 0; dirty = 1; hiber = 2; big = 0 }[$name]
    "flags=$flags cs=$($f[0]) nclus=$($f[1]) last=$($f[2]) mft=$($f[3]) bytes=$len" | Set-Content "$out\ntfs-$name.txt" -Encoding Ascii
    Write-Host "  $name : cs=$($f[0]) nclus=$($f[1]) last=$($f[2]) -> $out\ntfs-$name.img.gz ($([int]((Get-Item "$out\ntfs-$name.img.gz").Length / 1KB)) KB)"
}

function Set-Hiberfil([string]$root, [string]$magic) {
    $b = New-Object byte[] (256KB)
    if ($magic) { [Text.Encoding]::ASCII.GetBytes($magic).CopyTo($b, 0) }
    [IO.File]::WriteAllBytes("$root\hiberfil.sys", $b)
}

if ($Only -eq "" -or $Only -eq "clean") { New-Fixture "clean" "GPT" { param($r) Set-Hiberfil $r "" } }
if ($Only -eq "" -or $Only -eq "dirty") { New-Fixture "dirty" "MBR" { param($r) fsutil dirty set $r | Out-Null } }
if ($Only -eq "" -or $Only -eq "hiber") { New-Fixture "hiber" "GPT" { param($r) Set-Hiberfil $r "HIBR" } }
# a whole "Windows" disk with no free space: EFI system partition + NTFS filling the rest (the installer has to shrink it)
if ($Only -eq "" -or $Only -eq "big") { New-Fixture "big" "GPT" { param($r) Set-Hiberfil $r "" } -mb 1100 -esp }
# data disks formatted by Windows as FAT32 and exFAT (1100 MB: room to shrink above the FAT32 minimum of 65525 clusters)
if ($Only -eq "" -or $Only -eq "fat32") { New-Fixture "data" "GPT" { param($r) } -mb 1100 -fs FAT32 }
if ($Only -eq "" -or $Only -eq "exfat") { New-Fixture "data" "GPT" { param($r) } -mb 1100 -fs exFAT }
# a PC disk with an EFI system partition and a FAT32 / exFAT data partition filling the rest (install by shrinking it)
if ($Only -eq "" -or $Only -eq "fat32pc") { New-Fixture "pc" "GPT" { param($r) } -mb 1100 -esp -fs FAT32 }
if ($Only -eq "" -or $Only -eq "exfatpc") { New-Fixture "pc" "GPT" { param($r) } -mb 1100 -esp -fs exFAT }
Remove-Item -Recurse -Force $work
Write-Host "fixtures in $out"
