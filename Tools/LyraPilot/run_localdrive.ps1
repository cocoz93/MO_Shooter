param(
    [string]$Drive = 'N',       # N engine | W PhysWalking hook | C PhysCustom hook
    [string]$Tag = 'localN',
    [int]$TimeoutSec = 240
)
# Runs Lyra once (-game, offscreen) with the same input script and prints MOB_LOCAL lines.
$proj = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue   = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$out  = "$proj\Saved\MOB"
New-Item -ItemType Directory -Force $out | Out-Null
$log  = "$out\local_$Tag.log"

$a = @("`"$proj\Lyra.uproject`"", '/ShooterCore/Maps/L_ShooterGym?NumBots=0', '-game', '-RenderOffScreen', '-ResX=640', '-ResY=360',
       '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"", "-MOBLocalDrive=$Drive", "-MOBTag=$Tag")
$t0 = Get-Date
$p = Start-Process -FilePath $ue -ArgumentList $a -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing"; Stop-Process -Id $p.Id -Force }
"exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
Select-String -Path $log -Pattern 'MOB_LOCAL|Fatal error|Assertion failed' -Encoding UTF8 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
