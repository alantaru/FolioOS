# =============================================================================
# FolioOS - downloads the fonts needed by the Chinese, Japanese and Korean interface
# (Noto Sans SC/JP/KR, SIL Open Font License) into assets\fonts-cjk\_LANGUAGE.
# build.ps1 runs this when they are missing and ships them in FolioOS.zip (FOLIO\FONTS\_LANGUAGE).
# The files are large (~35 MB in total), so they are not stored in the repository.
# =============================================================================
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot)
$dst = "assets\fonts-cjk\_LANGUAGE"
New-Item -ItemType Directory -Force $dst | Out-Null
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$base = "https://raw.githubusercontent.com/google/fonts/main/ofl"
$fonts = @(
    @{ dir = "notosanssc"; file = "NotoSansSC%5Bwght%5D.ttf"; out = "NotoSansSC.ttf" },
    @{ dir = "notosansjp"; file = "NotoSansJP%5Bwght%5D.ttf"; out = "NotoSansJP.ttf" },
    @{ dir = "notosanskr"; file = "NotoSansKR%5Bwght%5D.ttf"; out = "NotoSansKR.ttf" }
)
foreach ($f in $fonts) {
    $target = Join-Path $dst $f.out
    if (Test-Path $target) { Write-Host "already here: $($f.out)"; continue }
    Write-Host "downloading $($f.out) ..."
    Invoke-WebRequest -UseBasicParsing "$base/$($f.dir)/$($f.file)" -OutFile $target
}
Invoke-WebRequest -UseBasicParsing "$base/notosanssc/OFL.txt" -OutFile (Join-Path $dst "OFL-NotoSansCJK.txt")
Write-Host "CJK fonts ready in $dst" -ForegroundColor Green
