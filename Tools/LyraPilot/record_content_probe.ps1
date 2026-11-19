param(
    [string]$Tag = 'probe',
    [ValidateSet('gym', 'pads', 'bump', 'weapons', 'dash')][string]$Mode = 'gym',
    [int]$TimeoutSec = 420,
    [string]$ExtraArgs = ''
)
# Runs Lyra alone (no server) through a fixed script and records what Lyra itself does for aim (ADS), crouch, melee and health pads.
# Needs the MOBridge plugin mode -MOBContentProbe (MOBContentProbeSubsystem). gym = L_ShooterGym with one bot (aim, crouch, melee);
# pads = L_Expanse without bots (health pads); weapons = L_Expanse without bots (weapon pads, quick bar, reload tag - weapon-pickup-plan W-0);
# dash = L_ShooterGym with one bot, the bot hidden (dash directions, force, blocked and cancelled abilities, cooldown - dash-plan D-0). Results: Saved\MOB\cprobe_<Tag>.csv (one row per frame) and MOB_CPROBE lines in the log.
# The third mode (bridge = today's client attached to our server) runs through run_game.ps1 instead:
#   run_game.ps1 -Tag b1 -Bots 8 -ServerArgs '--warmup=2 --matchSeconds=120' -LyraArgs '-MOBContentProbe=b1 -MOBCPMode=bridge'
$proj = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue   = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$out  = "$proj\Saved\MOB"
New-Item -ItemType Directory -Force $out | Out-Null
$log  = "$out\cprobe_$Tag.log"
if ($Mode -eq 'pads' -or $Mode -eq 'weapons') { $map = '/ShooterMaps/Maps/L_Expanse?NumBots=0' } else { $map = '/ShooterCore/Maps/L_ShooterGym?NumBots=1' }

$a = @("`"$proj\Lyra.uproject`"", $map, '-game', '-RenderOffScreen', '-ResX=1280', '-ResY=720',
       '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"",
       "-MOBContentProbe=$Tag", "-MOBCPMode=$Mode", '-benchmark', '-fps=60', '-UseFixedTimeStep')
# Extra Lyra args, e.g. -ExtraArgs '-MOBCPQuinn' (force Quinn body on me and the bot, like the server clips - D33)
if ($ExtraArgs) { $a += $ExtraArgs.Split(' ', [StringSplitOptions]::RemoveEmptyEntries) }
$t0 = Get-Date
$p = Start-Process -FilePath $ue -ArgumentList $a -PassThru
if (-not $p.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing"; Stop-Process -Id $p.Id -Force }
"exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
Select-String -Path $log -Pattern 'MOB_CPROBE|Fatal error|Assertion failed' -Encoding UTF8 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
