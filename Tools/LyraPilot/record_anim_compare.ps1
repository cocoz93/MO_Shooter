param(
    [string]$Tag = 'lyra',
    [switch]$Simple,
    [switch]$NoVideo,
    [switch]$Shapes,
    [switch]$DumpClips,
    [string]$Script = '',
    [switch]$Probe,
    [switch]$Transitions,
    [string]$Weapon = '',
    [int]$VideoPass = -1,
    [double]$EventDelay = 0,
    [string]$MapPath = '/ShooterCore/Maps/L_ShooterGym',
    [string]$Res = '1280x720',
    [int]$TimeoutSec = 590
)
# Records one opponent running a fixed script (Lyra animation, or -Simple for the plain clip blend) for a side-by-side look.
# Results: Saved\MOB\animcmp_<Tag>.csv and rec_animcmp_<Tag>.bgra (raw video, turned into mp4 afterwards with ffmpeg).
# -Shapes also writes animcmp_<Tag>_shapes.csv: every hit shape per frame, the input of the mis-hit calculation.
# -DumpClips (with -Simple) also writes animcmp_<Tag>_clips60.csv and _clips120.csv: the clip data a pose calculation outside Unreal needs.
# -Script aimjump records aiming up/down and jumping instead of the running script; with -Simple the plain blend also does aim and jump.
# -Script startstop records starts, stops and direction reversals in four directions (third look, start-stop-pivot-test.md).
# -Transitions (with -Simple) also puts the start, stop and reversal clips into the plain blend.
# -Weapon pistol|rifle|shotgun gives the opponent that weapon (default rifle). -Script events fires, reloads, swaps and replays the spawn effect along the path (step 9-1);
#   -EventDelay <s> shifts every event of that script (tool check). -VideoPass <n> records only that pass to video.
#   with -Probe it also writes animcmp_<Tag>_montages.csv (montage asset values) and _blendprofiles.csv (per-bone blend tables of the skeleton).
# -Script ads presses/releases aim-down-sights along the path: standing toggles, aimed starts/stops/pivots in four directions, an aimed jump, and toggles mid-run/start/stop/pivot (content-add C-3).
# -Probe also writes animcmp_<Tag>_probe.csv: per frame anim blueprint variables, state machine states, curves and body-space bone positions.
$proj = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue   = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$out  = "$proj\Saved\MOB"
New-Item -ItemType Directory -Force $out | Out-Null
$log  = "$out\animcmp_$Tag.log"
$resX, $resY = $Res -split 'x'

$a = @("`"$proj\Lyra.uproject`"", "$MapPath`?NumBots=1", '-game', '-RenderOffScreen', "-ResX=$resX", "-ResY=$resY",
       '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"",
       "-MOBAnimCompare=$Tag", '-benchmark', '-fps=60', '-UseFixedTimeStep')
if ($Simple)  { $a += '-MOBSimpleAnim' }
if ($NoVideo) { $a += '-MOBACNoVideo' }
if ($Shapes)  { $a += '-MOBACShapes' }
if ($DumpClips) { $a += '-MOBACDumpClips' }
if ($Script)  { $a += "-MOBACScript=$Script" }
if ($Probe)   { $a += '-MOBACProbe' }
if ($Weapon)  { $a += "-MOBACWeapon=$Weapon" }
if ($VideoPass -ge 0) { $a += "-MOBACVideoPass=$VideoPass" }  # only that pass goes to video (0 = near), when the disk is short
if ($EventDelay -gt 0) { $a += "-MOBACEventDelay=$EventDelay" }  # tool check: every event of the events script shifts by this many seconds
if ($Simple -and ($Transitions -or $Script -eq 'aimjump' -or $Script -eq 'startstop')) { $a += '-MOBSimpleAimJump' }  # -Transitions needs the aim/jump clip set too
if ($Simple -and $Transitions) { $a += '-MOBSimpleTransitions' }
$t0 = Get-Date
$p = Start-Process -FilePath $ue -ArgumentList $a -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing"; Stop-Process -Id $p.Id -Force }
"exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
foreach ($name in "animcmp_$Tag.csv", "animcmp_${Tag}_shapes.csv", "animcmp_${Tag}_clips60.csv", "animcmp_${Tag}_clips120.csv", "animcmp_${Tag}_probe.csv", "animcmp_${Tag}_montages.csv", "animcmp_${Tag}_blendprofiles.csv", "rec_animcmp_$Tag.bgra") {
    $file = "$out\$name"
    if (Test-Path $file) { "{0} {1} bytes" -f $name, (Get-Item $file).Length } else { "$name missing" }
}
Select-String -Path $log -Pattern 'MOB_ACOMP|MOB_SIMPLE|Fatal error|Assertion failed' -Encoding UTF8 | Select-Object -First 60 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
