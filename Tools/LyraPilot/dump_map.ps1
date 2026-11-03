param(
    [string]$MapPath = '/ShooterCore/Maps/L_ShooterGym',
    [string]$Tag = 'gym',
    [int]$TimeoutSec = 540,
    [int]$DelaySec = 15,
    [string]$ExtraArgs = ''
)
# Runs Lyra once (-game, offscreen) and dumps the map collision to Saved/MOB/map_<Tag>.mocl
$proj = 'C:\Users\USER\Documents\Unreal Projects\LyraStarterGame'
$ue   = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$out  = "$proj\Saved\MOB"
New-Item -ItemType Directory -Force $out | Out-Null
$log  = "$out\mapdump_$Tag.log"
$file = "$out\map_$Tag.mocl"

$a = @("`"$proj\Lyra.uproject`"", "$MapPath`?NumBots=0", '-game', '-RenderOffScreen', '-ResX=640', '-ResY=360',
       '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"", "-MOBDumpMap=`"$file`"", "-MOBDumpDelay=$DelaySec") + ($ExtraArgs -split ' ' | Where-Object { $_ })
$t0 = Get-Date
$p = Start-Process -FilePath $ue -ArgumentList $a -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing"; Stop-Process -Id $p.Id -Force }
"exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
if (Test-Path $file) { "file {0} bytes" -f (Get-Item $file).Length } else { 'no file' }
Select-String -Path $log -Pattern 'MOB_MAP|Fatal error|Assertion failed' -Encoding UTF8 | Select-Object -First 60 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
