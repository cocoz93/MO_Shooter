param(
    [string]$MapPath = '/ShooterMaps/Maps/L_Expanse',
    [string]$Tag = 'expanse',
    [int]$TimeoutSec = 540,
    [int]$DelaySec = 30,
    [int]$Pairs = 300,
    [string]$ExtraArgs = ''
)
# Runs Lyra once (-game, offscreen) and dumps the walkable-floor data to Saved/MOB/nav_<Tag>.monv, with Unreal's own path answers in nav_<Tag>.monv.pairs.csv
$proj = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue   = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$out  = "$proj\Saved\MOB"
New-Item -ItemType Directory -Force $out | Out-Null
$log  = "$out\navdump_$Tag.log"
$file = "$out\nav_$Tag.monv"

$a = @("`"$proj\Lyra.uproject`"", "$MapPath`?NumBots=0", '-game', '-RenderOffScreen', '-ResX=640', '-ResY=360',
       '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"", "-MOBDumpNav=`"$file`"", "-MOBDumpDelay=$DelaySec", "-MOBNavPairs=$Pairs") + ($ExtraArgs -split ' ' | Where-Object { $_ })
$t0 = Get-Date
$p = Start-Process -FilePath $ue -ArgumentList $a -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing"; Stop-Process -Id $p.Id -Force }
"exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
if (Test-Path $file) { "file {0} bytes" -f (Get-Item $file).Length } else { 'no file' }
Select-String -Path $log -Pattern 'MOB_NAV|Fatal error|Assertion failed|Unable to find RecastNavMesh' -Encoding UTF8 | Select-Object -First 60 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
