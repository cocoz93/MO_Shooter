param(
    [string]$Tag = 'dedi',
    [string]$Script = 'startstop',
    [int]$LagMs = 0,
    [int]$JitterMs = 0,
    [int]$Port = 7790,
    [int]$TimeoutSec = 560
)
# Unreal dedicated server + one client on the same PC, for the baseline of candidate1-impl-plan.md step 0.
# The server moves its bot along the same script as the look test; server and client both write the bot's 22 hit shapes.
# -Script startstop | aimjump | run. -LagMs and -JitterMs are added to each direction by the engine's packet simulation.
# Results: Saved\MOB\dedi_<Tag>_server.csv and dedi_<Tag>_client.csv, compared by check_dedi_baseline.py.
$proj = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue   = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$out  = "$proj\Saved\MOB"
New-Item -ItemType Directory -Force $out | Out-Null
$serverLog = "$out\dedi_${Tag}_server.log"
$clientLog = "$out\dedi_${Tag}_client.log"

$lag = @()
if ($LagMs -gt 0) { $lag += "-PktLag=$LagMs" }
if ($JitterMs -gt 0) { $lag += "-PktLagVariance=$JitterMs" }

$serverArgs = @("`"$proj\Lyra.uproject`"", '/ShooterCore/Maps/L_ShooterGym?NumBots=1', '-server', '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$serverLog`"",
                "-port=$Port", "-MOBAnimCompare=$Tag", '-MOBACNoVideo', "-MOBDediBaseline=$Tag", '-ini:Engine:[/Script/OnlineSubsystemUtils.IpNetDriver]:NetServerMaxTickRate=60') + $lag
if ($Script -ne 'run') { $serverArgs += "-MOBACScript=$Script" }
$clientArgs = @("`"$proj\Lyra.uproject`"", "127.0.0.1:$Port", '-game', '-RenderOffScreen', '-ResX=640', '-ResY=360', '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$clientLog`"",
                "-MOBDediBaseline=$Tag", '-ExecCmds="t.MaxFPS 60"') + $lag

$t0 = Get-Date
$server = Start-Process -FilePath $ue -ArgumentList $serverArgs -PassThru
$up = $false
foreach ($i in 1..180) {
    Start-Sleep -Seconds 1
    if ($server.HasExited) { break }
    if ((Test-Path $serverLog) -and (Select-String -Path $serverLog -Pattern 'listening on port' -Quiet -Encoding UTF8)) { $up = $true; break }
}
"server up=$up after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
if (-not $up) { if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }; exit 1 }

$client = Start-Process -FilePath $ue -ArgumentList $clientArgs -PassThru
if (-not $server.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing server"; Stop-Process -Id $server.Id -Force }
"server exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
if (-not $client.WaitForExit(15 * 1000)) { 'client still running - killing (csv is already closed when the map unloads)'; Stop-Process -Id $client.Id -Force }
"client exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds

foreach ($name in "dedi_${Tag}_server.csv", "dedi_${Tag}_client.csv") {
    $file = "$out\$name"
    if (Test-Path $file) { "{0} {1} bytes" -f $name, (Get-Item $file).Length } else { "$name missing" }
}
foreach ($log in $serverLog, $clientLog) {
    "--- $(Split-Path -Leaf $log)"
    Select-String -Path $log -Pattern 'MOB_DEDI|MOB_ACOMP ready|MOB_ACOMP body|MOB_ACOMP_FAIL|MOB_ACOMP done|max tick rate|Fatal error|Assertion failed' -Encoding UTF8 | Where-Object { $_.Line -notmatch 'MOB_DEDI traffic' } | Select-Object -First 40 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
    Select-String -Path $log -Pattern 'MOB_DEDI traffic' -Encoding UTF8 | Select-Object -Last 1 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
}
