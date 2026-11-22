param(
    [string]$MapPath = '/ShooterCore/Maps/L_ShooterGym',
    [string]$Tag = 'gym',
    [int]$TimeoutSec = 180,
    [switch]$Launchers   # jump pads (j-1): launcher actors' blueprint vars + shapes, and the launcher graphs (deep)
)
# Dumps runtime weapon numbers, blueprint graphs and character collision bodies to Saved/MOB/weapons_<Tag>.txt
$proj = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue   = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$out  = "$proj\Saved\MOB"
New-Item -ItemType Directory -Force $out | Out-Null
$log  = "$out\weapons_$Tag.log"

$a = @("`"$proj\Lyra.uproject`"", "$MapPath`?NumBots=1", '-game', '-RenderOffScreen', '-ResX=640', '-ResY=360',
       '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"", "-MOBWeaponDump=$Tag")
if ($Launchers) {
    $a += '-MOBWDLaunchers'
    $a += '-MOBWDGraph=/ShooterCore/Blueprint/B_Launcher.B_Launcher;/ShooterCore/Blueprint/B_Launcher_Up.B_Launcher_Up;/ShooterCore/Blueprint/B_Launcher_Push.B_Launcher_Push;/ShooterCore/GameplayCues/GCNL_Launcher_Activate.GCNL_Launcher_Activate'
}
$t0 = Get-Date
$p = Start-Process -FilePath $ue -ArgumentList $a -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing"; Stop-Process -Id $p.Id -Force }
"exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
$file = "$out\weapons_$Tag.txt"
if (Test-Path $file) { "file {0} bytes" -f (Get-Item $file).Length } else { 'no file' }
Select-String -Path $log -Pattern 'MOB_WDUMP|Fatal error|Assertion failed' -Encoding UTF8 | Select-Object -First 40 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
