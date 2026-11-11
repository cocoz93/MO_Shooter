param(
    [string]$Tag = 'bodies',
    [string]$MapPath = '/ShooterCore/Maps/L_ShooterGym',
    [string]$Res = '1600x1200',
    [int]$TimeoutSec = 300,
    [switch]$NoOpen
)
# Stands Lyra's two stock bodies (Manny, Quinn) side by side and takes four stills: front, front + hit shapes, side, side + hit shapes.
# Needs the MOBridge plugin mode -MOBBodyShot (MOBBodyShotSubsystem). Borrows the bodies of two bots, so the map URL carries NumBots=2.
# Results: Saved\MOB\bodyshot_<Tag>_<n>_<name>.bgra (raw stills) -> make_body_shot.py -> Saved\MOB\bodyshot_<Tag>.png
$proj = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue   = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$out  = "$proj\Saved\MOB"
New-Item -ItemType Directory -Force $out | Out-Null
$log  = "$out\bodyshot_$Tag.log"
$resX, $resY = $Res -split 'x'

$a = @("`"$proj\Lyra.uproject`"", "$MapPath`?NumBots=2", '-game', '-RenderOffScreen', "-ResX=$resX", "-ResY=$resY",
       '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"",
       "-MOBBodyShot=$Tag", '-benchmark', '-fps=60', '-UseFixedTimeStep')
$t0 = Get-Date
$p = Start-Process -FilePath $ue -ArgumentList $a -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing"; Stop-Process -Id $p.Id -Force }
"exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
Select-String -Path $log -Pattern 'MOB_BODYSHOT|Fatal error|Assertion failed' -Encoding UTF8 | Select-Object -First 30 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }

$png = "$out\bodyshot_$Tag.png"
& python "$PSScriptRoot\make_body_shot.py" $out $Tag $resX $resY $png
if ((Test-Path $png) -and -not $NoOpen) { Invoke-Item $png }
