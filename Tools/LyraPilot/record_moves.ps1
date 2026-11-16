param(
    [string]$MapPath = '/ShooterCore/Maps/L_ShooterGym',
    [string]$Tag = 'gym_fixed',
    [int]$Seed = 20261102,
    [int]$Segs = 40,
    [double]$SegSec = 10,
    [switch]$FreeDt,
    [switch]$Stress,
    [switch]$Ads,
    [int]$TimeoutSec = 580
)
# Replays a fixed random input on Lyra movement and writes Saved/MOB/move_<Tag>.csv (answer sheet for Tools/MoveCheck).
# Default = fixed 60 Hz frames. -FreeDt = let the frame time vary.
$proj = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue   = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$out  = "$proj\Saved\MOB"
New-Item -ItemType Directory -Force $out | Out-Null
$log  = "$out\move_$Tag.log"

$a = @("`"$proj\Lyra.uproject`"", "$MapPath`?NumBots=0", '-game', '-RenderOffScreen', '-ResX=640', '-ResY=360',
       '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"",
       "-MOBMoveRecord=$Tag", "-MOBMoveSeed=$Seed", "-MOBMoveSegs=$Segs", "-MOBMoveSegSec=$SegSec")
if (-not $FreeDt) { $a += @('-benchmark', '-fps=60', '-UseFixedTimeStep') }
if ($Stress) { $a += '-MOBMoveStress' }
# -Ads: presses/releases aim-down-sights at random (step C, walk speed x0.5). Separate random stream, so the movement input is the same as without it.
if ($Ads) { $a += '-MOBMoveAds' }
$t0 = Get-Date
$p = Start-Process -FilePath $ue -ArgumentList $a -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing"; Stop-Process -Id $p.Id -Force }
"exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
$file = "$out\move_$Tag.csv"
if (Test-Path $file) { "file {0} bytes" -f (Get-Item $file).Length } else { 'no file' }
Select-String -Path $log -Pattern 'MOB_MOVE|Fatal error|Assertion failed' -Encoding UTF8 | Select-Object -First 40 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
