param(
    [string]$Tag = 'fx',
    [int]$TimeoutSec = 300,
    [string]$ExtraArgs = ''
)
# Runs Lyra alone (no server, no bridge) on L_Expanse and records two things Lyra itself does (MOBFxProbeSubsystem, -MOBFxProbe):
#   1) own bullet decal on a wall - one shot per weapon, screenshots at fixed frames after the press (Saved\MOB\fx_<Tag>_<weapon>_<frame>.png)
#   2) weapon swap then fire - press fire N frames after the swap, log whether the new weapon's magazine dropped and the montages (MOB_FX_SWAP)
# Fixed 60 fps game time, so frame offsets are 1/60 s each. Analyse with check_fx_probe.py.
$proj = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue   = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$out  = "$proj\Saved\MOB"
New-Item -ItemType Directory -Force $out | Out-Null
$log  = "$out\fx_$Tag.log"
Get-ChildItem -Path $out -Filter "fx_${Tag}_*.png" -ErrorAction SilentlyContinue | ForEach-Object { [IO.File]::Delete($_.FullName) }

$a = @("`"$proj\Lyra.uproject`"", '/ShooterMaps/Maps/L_Expanse?NumBots=0', '-game', '-RenderOffScreen', '-ResX=1280', '-ResY=720',
       '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"",
       "-MOBFxProbe=$Tag", '-benchmark', '-fps=60', '-UseFixedTimeStep')
if ($ExtraArgs) { $a += $ExtraArgs.Split(' ', [StringSplitOptions]::RemoveEmptyEntries) }
$t0 = Get-Date
$p = Start-Process -FilePath $ue -ArgumentList $a -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing"; Stop-Process -Id $p.Id -Force }
"exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
Select-String -Path $log -Pattern 'MOB_FX_(INIT|SLOT|WALL|READY|AIM|DECAL|SWAP|FAIL|DONE)|Fatal error|Assertion failed' -Encoding UTF8 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
