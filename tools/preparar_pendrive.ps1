# FOLIO 4 — apaga o pen-drive e cria UMA partição FAT32 ocupando o disco todo.
# Precisa ser executado como ADMINISTRADOR.
#   powershell -ExecutionPolicy Bypass -File tools\preparar_pendrive.ps1 -DriveLetter F
param([Parameter(Mandatory = $true)][string]$DriveLetter)
$ErrorActionPreference = "Stop"
$L = $DriveLetter.TrimEnd(':', '\')

$part = Get-Partition -DriveLetter $L
$disk = Get-Disk -Number $part.DiskNumber
Write-Host ("Disco {0}: {1} · {2:N1} GB · barramento {3}" -f $disk.Number, $disk.FriendlyName, ($disk.Size / 1GB), $disk.BusType) -ForegroundColor Cyan

# travas: só pen-drive USB, nunca disco de sistema, nunca acima de 64 GB
if ($disk.BusType -ne 'USB') { throw "O disco não é USB. Abortado." }
if ($disk.IsSystem -or $disk.IsBoot) { throw "Este é um disco de sistema. Abortado." }
if ($disk.Size -gt 64GB) { throw "Disco maior que 64 GB; confira a letra. Abortado." }

$ok = Read-Host "TUDO no disco $($disk.Number) ($($disk.FriendlyName)) será APAGADO. Digite SIM para continuar"
if ($ok -ne 'SIM') { Write-Host "Cancelado."; exit 1 }

Clear-Disk -Number $disk.Number -RemoveData -RemoveOEM -Confirm:$false
Initialize-Disk -Number $disk.Number -PartitionStyle MBR -ErrorAction SilentlyContinue
Set-Disk -Number $disk.Number -PartitionStyle MBR -ErrorAction SilentlyContinue
$p = New-Partition -DiskNumber $disk.Number -UseMaximumSize -IsActive -DriveLetter $L
Format-Volume -Partition $p -FileSystem FAT32 -NewFileSystemLabel FOLIO -Confirm:$false | Out-Null
$v = Get-Volume -DriveLetter $L
Write-Host ("Pronto: ${L}: FAT32 '{0}' com {1:N1} GB livres." -f $v.FileSystemLabel, ($v.SizeRemaining / 1GB)) -ForegroundColor Green
