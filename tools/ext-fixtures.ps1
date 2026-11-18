# =============================================================================
# FolioOS - ext4 test fixtures made by Linux itself (Alpine in QEMU, tools\linux-run.ps1); no administrator needed
#   .\tools\ext-fixtures.ps1 [-Only clean|small|errors|recover]
# Each disk gets a GPT with one Linux partition (fscheck_test --mkgpt); inside Alpine, mkfs.ext4 formats it,
# files are written and some deleted, and dumpe2fs reports the facts (block size, block count, last block in
# use). Saved as tests\fixtures\ext-<name>.img.gz + ext-<name>.txt; tools\fscheck_test.c compares FolioOS's
# reader with them, the way the NTFS fixtures are made by Windows.
#   clean    256 MB, 4 KiB blocks                         -> state ok
#   small     64 MB, 1 KiB blocks (mke2fs on small disks)  -> state ok
#   errors    64 MB, marked with errors (state 3)          -> "has errors (fsck)"
#   recover   64 MB, journal needs recovery                -> "not unmounted cleanly"
#   linux    600 MB: EFI partition + ext4 filling the rest (a PC with Linux, no free space: the installer explains)
# =============================================================================
param([string]$Only = "")
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot)
if (-not (Test-Path build\fscheck_test.exe)) { throw "run .\build.ps1 first (build\fscheck_test.exe)" }
$out = "tests\fixtures"; New-Item -ItemType Directory -Force $out | Out-Null
$base = if ($env:FOLIO_WORK) { $env:FOLIO_WORK } else { $env:TEMP }
$work = Join-Path $base "ext-fixtures"; New-Item -ItemType Directory -Force $work | Out-Null
$all = @(@{ name = "clean"; mb = 256 }, @{ name = "small"; mb = 64 }, @{ name = "errors"; mb = 64 }, @{ name = "recover"; mb = 64 },
         @{ name = "linux"; mb = 600; esp = $true })   # a PC with Linux: EFI partition + ext4 filling the rest (no free space)
foreach ($fx in $all) {
    if ($Only -and $fx.name -ne $Only) { continue }
    $img = "$work\ext-$($fx.name).img"
    if ($fx.esp) { & .\build\fscheck_test.exe --mkgpt $img $fx.mb --esp } else { & .\build\fscheck_test.exe --mkgpt $img $fx.mb }
    if ($LASTEXITCODE -ne 0) { throw "mkgpt failed" }
    $dev = if ($fx.esp) { "/dev/vda2" } else { "/dev/vda1" }
    (Get-Content -Raw tools\ext-fixture.sh) -replace "__NAME__", $fx.name -replace "__DEV__", $dev | Set-Content -Encoding ascii -NoNewline "$work\run.sh"
    $o = & .\tools\linux-run.ps1 -Disk $img -Script "$work\run.sh" -Out "$work\serial-$($fx.name).log"
    if ($LASTEXITCODE -ne 0) { throw "Linux script failed for $($fx.name): $o" }
    $o = @($o -split "`n" | ForEach-Object { $_.TrimEnd() })
    $facts = ($o | Select-String -Pattern "^facts: (.*)$").Matches | Select-Object -First 1
    if (-not $facts) { throw "no facts from Linux for $($fx.name): $o" }
    $line = $facts.Groups[1].Value.Trim()
    "$line bytes=$((Get-Item $img).Length)" | Set-Content -Encoding ascii "$out\ext-$($fx.name).txt"
    $src = [IO.File]::OpenRead($img); $dst = [IO.File]::Create("$out\ext-$($fx.name).img.gz")
    $gz = New-Object IO.Compression.GZipStream($dst, [IO.Compression.CompressionLevel]::Optimal); $src.CopyTo($gz)
    $gz.Dispose(); $dst.Dispose(); $src.Dispose(); Remove-Item $img
    Write-Host "  $($fx.name): $line ($([int]((Get-Item "$out\ext-$($fx.name).img.gz").Length / 1KB)) KB) | $(($o | Select-String 'e2fsck:').Line)"
}
Remove-Item -Recurse -Force $work
