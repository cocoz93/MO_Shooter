param(
    [string]$MapPath = '/ShooterCore/Maps/L_ShooterGym',
    [string]$Tag = 'gym',
    [int]$Seed = 20261102,
    [int]$Segs = 4,
    [double]$SegSec = 30,
    [double]$RaySec = 20,
    [switch]$FreeDt,
    [switch]$PoseOnly,
    [int]$TimeoutSec = 590
)
# Records weapon table, damage samples, scripted firing, real hits and body rays to Saved/MOB/weaponrec_<Tag>.csv (answer sheet for Tools/WeaponCheck).
$proj = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue   = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$out  = "$proj\Saved\MOB"
New-Item -ItemType Directory -Force $out | Out-Null
$log  = "$out\weaponrec_$Tag.log"

$a = @("`"$proj\Lyra.uproject`"", "$MapPath`?NumBots=2", '-game', '-RenderOffScreen', '-ResX=640', '-ResY=360',
       '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"",
       "-MOBWeaponRecord=$Tag", "-MOBWeaponSeed=$Seed", "-MOBWeaponSegs=$Segs", "-MOBWeaponSegSec=$SegSec", "-MOBWeaponRaySec=$RaySec")
if ($PoseOnly) { $a += '-MOBPoseOnly' }
if (-not $FreeDt) { $a += @('-benchmark', '-fps=60', '-UseFixedTimeStep') }
$t0 = Get-Date
$p = Start-Process -FilePath $ue -ArgumentList $a -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing"; Stop-Process -Id $p.Id -Force }
"exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
$file = "$out\weaponrec_$Tag.csv"
if (Test-Path $file) { "file {0} bytes" -f (Get-Item $file).Length } else { 'no file' }
Select-String -Path $log -Pattern 'MOB_WREC|Fatal error|Assertion failed' -Encoding UTF8 | Select-Object -First 40 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
