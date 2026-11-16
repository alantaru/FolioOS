# =============================================================================
# FolioOS - regenerates the README screenshots (docs\screenshots) from tools\showcase\showcase.txt
# Needs a previous .\build.ps1 (uses build\usb_files and build\folio_sim.exe).
# =============================================================================
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot)
$t = "build\showcase_root"; $out = "build\showcase_shots"
foreach ($d in $t, $out) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
Copy-Item -Recurse build\usb_files $t
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem "$t\DOCUMENTS" | Remove-Item -Recurse -Force
Copy-Item tools\showcase\*.md "$t\DOCUMENTS\"
Remove-Item Env:\FOLIO_PLATFORM_LANG -ErrorAction SilentlyContinue
& .\build\folio_sim.exe --root $t --script tools\showcase\showcase.txt --out $out
if ($LASTEXITCODE -ne 0) { throw "showcase script failed" }
# the simulator writes uncompressed PNG: re-encode them (about 20x smaller)
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force docs\screenshots | Out-Null
foreach ($f in Get-ChildItem "$out\*.png") {
    $img = [System.Drawing.Image]::FromFile($f.FullName)
    $bmp = New-Object System.Drawing.Bitmap $img
    $img.Dispose()
    $bmp.Save((Join-Path (Resolve-Path docs\screenshots) $f.Name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
Remove-Item -Recurse -Force $t, $out
Write-Host "screenshots written to docs\screenshots" -ForegroundColor Green
