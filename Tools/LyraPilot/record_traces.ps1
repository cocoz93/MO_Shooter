param(
    [string]$MapPath = '/ShooterCore/Maps/L_ShooterGym',
    [string]$Tag = 'gym',
    [int]$Rays = 150000,
    [int]$Sweeps = 45000,
    [int]$TimeoutSec = 540,
    [int]$DelaySec = 15
)
# Runs Lyra once (-game, offscreen): dumps map collision and records the same rays/capsules with Lyra physics.
# Output: Saved/MOB/map_<Tag>.mocl, trace_<Tag>.csv, tracerun_<Tag>.log
$proj = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue   = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$out  = "$proj\Saved\MOB"
New-Item -ItemType Directory -Force $out | Out-Null
$log   = "$out\tracerun_$Tag.log"
$file  = "$out\map_$Tag.mocl"
$trace = "$out\trace_$Tag.csv"

$a = @("`"$proj\Lyra.uproject`"", "$MapPath`?NumBots=0", '-game', '-RenderOffScreen', '-ResX=640', '-ResY=360',
       '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"", "-MOBDumpMap=`"$file`"", "-MOBDumpDelay=$DelaySec",
       "-MOBTraceRecord=`"$trace`"", "-MOBTraceRays=$Rays", "-MOBTraceSweeps=$Sweeps")
$t0 = Get-Date
$p = Start-Process -FilePath $ue -ArgumentList $a -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing"; Stop-Process -Id $p.Id -Force }
"exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
Get-Item $file, $trace -ErrorAction SilentlyContinue | ForEach-Object { '{0} {1} bytes' -f $_.Name, $_.Length }
Select-String -Path $log -Pattern 'MOB_MAP_COUNT|MOB_MAP_SKIP|MOB_MAP_LEAF|MOB_MAP_DONE|MOB_TRACE|Fatal error|Assertion failed' -Encoding UTF8 | Select-Object -First 40 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
