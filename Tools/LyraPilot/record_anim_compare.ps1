param(
    [string]$Tag = 'lyra',
    [switch]$Simple,
    [switch]$NoVideo,
    [switch]$Shapes,
    [switch]$DumpClips,
    [string]$Script = '',
    [switch]$Probe,
    [string]$MapPath = '/ShooterCore/Maps/L_ShooterGym',
    [string]$Res = '1280x720',
    [int]$TimeoutSec = 590
)
# Records one opponent running a fixed script (Lyra animation, or -Simple for the plain clip blend) for a side-by-side look.
# Results: Saved\MOB\animcmp_<Tag>.csv and rec_animcmp_<Tag>.bgra (raw video, turned into mp4 afterwards with ffmpeg).
# -Shapes also writes animcmp_<Tag>_shapes.csv: every hit shape per frame, the input of the mis-hit calculation.
# -DumpClips (with -Simple) also writes animcmp_<Tag>_clips60.csv and _clips120.csv: the clip data a pose calculation outside Unreal needs.
# -Script aimjump records aiming up/down and jumping instead of the running script; with -Simple the plain blend also does aim and jump.
# -Probe also writes animcmp_<Tag>_probe.csv: per frame anim blueprint variables, state machine states, curves and body-space bone positions.
$proj = 'C:\Users\USER\Documents\Unreal Projects\LyraStarterGame'
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
if ($Simple -and $Script -eq 'aimjump') { $a += '-MOBSimpleAimJump' }
$t0 = Get-Date
$p = Start-Process -FilePath $ue -ArgumentList $a -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing"; Stop-Process -Id $p.Id -Force }
"exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
foreach ($name in "animcmp_$Tag.csv", "animcmp_${Tag}_shapes.csv", "animcmp_${Tag}_clips60.csv", "animcmp_${Tag}_clips120.csv", "animcmp_${Tag}_probe.csv", "rec_animcmp_$Tag.bgra") {
    $file = "$out\$name"
    if (Test-Path $file) { "{0} {1} bytes" -f $name, (Get-Item $file).Length } else { "$name missing" }
}
Select-String -Path $log -Pattern 'MOB_ACOMP|MOB_SIMPLE|Fatal error|Assertion failed' -Encoding UTF8 | Select-Object -First 60 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
