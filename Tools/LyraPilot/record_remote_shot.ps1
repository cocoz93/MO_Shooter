param(
    [string]$Tag = 'r1',
    [int]$DelayMs = 100,
    [int]$JitterMs = 33,
    [int]$Port = 7850,
    [string]$Weapon = 'rifle',   # bot weapon (BotClient --scriptWeapon: pistol, rifle, shotgun); the screen poses it with that weapon's clips
    [double]$HoldSec = -1,   # longer than the 4 s the shot waits before filming; route must start before the idle fidget, as the passed video did. -1 = by weapon (below)
    [string]$Res = '1280x720',
    [string]$Script = 'startstop',   # BotClient --script: startstop (passed start/stop video) or events (fire, reload, swap; record_anim_compare.ps1 -Script events)
    [int]$TimeoutSec = 400
)
# The route starts at hold + 2.02 s (first script piece = stand 121 ticks). The fidget comes at stop clip seconds + 6 s after the bot is placed:
# rifle and shotgun 1.35 + 6 = 7.35 s, pistol 0.35 + 6 = 6.35 s (Shared/AnimState.cpp kBundleTimings). Lyra's own pistol and shotgun records show no fidget before the route either.
if ($HoldSec -lt 0) { $HoldSec = if ($Weapon -eq 'pistol') { 4.25 } else { 4.5 } }
# Films one server-driven opponent with the same camera and the same route as the passed animation comparison (record_anim_compare.ps1 -Script startstop or events).
# The shot stops 47.3 s + 1 s after the first move (MOBRemoteShotSubsystem SCRIPT_AFTER_MOVE, the start/stop route). The events route first moves at tick 333 and ends at 2470,
# so its film carries about 13 s of standing after the route (333/60 + 48.28 - 2470/60 = 12.7 s); make_remote_compare.py stops at the reference end. The bot stays connected for the same time either way.
# Lyra goes through Tools/LagRelay with a real-match delay; the bot runs the fixed route and connects directly. Lyra sends no input, only a debug place every 20 s: the map has no spawn floor, and the server drops a session silent for 60 s.
# Results: Saved\MOB\remoteshot_<Tag>.csv and rec_remoteshot_<Tag>.bgra; make_remote_compare.py turns them into the side-by-side video.
$proj   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue     = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$repo   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter'
$outDir = "$proj\Saved\MOB"
$map    = "$outDir\map_gym.mocl"
$clips  = "$outDir\clips.bin"
$srvOut = "$outDir\remoteshot_${Tag}_server.txt"
$botOut = "$outDir\remoteshot_${Tag}_bot.txt"
$relayOut = "$outDir\remoteshot_${Tag}_relay.txt"
$log    = "$outDir\remoteshot_$Tag.log"
$relayPort = $Port + 1
$resX, $resY = $Res -split 'x'
$botSeconds = [int]($HoldSec + 48.9 + 25)

$srv = Start-Process -FilePath "$repo\build\Server\Release\Server.exe" -ArgumentList @("--map=`"$map`"", "--port=$Port", "--clips=`"$clips`"", '--debug', '--exitWhenEmpty', '--warmup=1', '--matchSeconds=500') -RedirectStandardOutput $srvOut -PassThru -WindowStyle Hidden
$null = $srv.Handle   # without this ExitCode stays empty after the process ends
Start-Sleep -Milliseconds 800
$relay = Start-Process -FilePath "$repo\build\Tools\LagRelay\Release\LagRelay.exe" -ArgumentList @("--listen=$relayPort", "--target=$Port", "--delayMs=$DelayMs", "--jitterMs=$JitterMs", "--seconds=$TimeoutSec") -RedirectStandardOutput $relayOut -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 300

$lyraArgs = @("`"$proj\Lyra.uproject`"", '/ShooterCore/Maps/L_ShooterGym?NumBots=0', '-game', '-RenderOffScreen', "-ResX=$resX", "-ResY=$resY",
              '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"",
              "-MOBServer=127.0.0.1:$relayPort", '-MOBServerAnim', '-MOBStandAt=-1020,-1070,142', "-MOBRemoteShot=$Tag", "-MOBTag=remoteshot_$Tag", '-ExecCmds="t.MaxFPS 60"')
$t0 = Get-Date
$lyra = Start-Process -FilePath $ue -ArgumentList $lyraArgs -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline -and -not $lyra.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'MOB_WELCOME' -Quiet)) { break }
    Start-Sleep -Milliseconds 500
}
Start-Sleep -Seconds 3   # the 1 s warmup ends and the match starts before the bot joins
$bot = Start-Process -FilePath "$repo\build\BotClient\Release\BotClient.exe" -ArgumentList @("--map=`"$map`"", "--port=$Port", '--bots=1', "--script=$Script", "--scriptHold=$HoldSec", "--scriptWeapon=$Weapon", "--seconds=$botSeconds") -RedirectStandardOutput $botOut -PassThru -WindowStyle Hidden
$null = $bot.Handle

$timedOut = $false
if (-not $lyra.WaitForExit($TimeoutSec * 1000)) { $timedOut = $true; "TIMEOUT - stopping Lyra"; Stop-Process -Id $lyra.Id -Force }
"lyra exit after {0:N1}s timedOut={1}" -f ((Get-Date) - $t0).TotalSeconds, $timedOut
if (-not $bot.WaitForExit(60000)) { Stop-Process -Id $bot.Id -Force; 'bot stopped' }
if (-not $srv.WaitForExit(20000)) { Stop-Process -Id $srv.Id -Force; 'server stopped' }
if (-not $relay.HasExited) { Stop-Process -Id $relay.Id -Force }
"server exit={0} bot exit={1}" -f $srv.ExitCode, $bot.ExitCode

foreach ($name in "remoteshot_$Tag.csv", "rec_remoteshot_$Tag.bgra") {
    $file = "$outDir\$name"
    if (Test-Path $file) { "{0} {1} bytes" -f $name, (Get-Item $file).Length } else { "$name missing" }
}
Select-String -Path $botOut -Pattern 'SCRIPT (placed|end)|step=(idle|fwd) ' -Encoding UTF8 | ForEach-Object { $_.Line }
Select-String -Path $log -Pattern 'MOB_REMOTESHOT|MOB_WELCOME|MOB_SUMMARY |Fatal error|Assertion failed|Unhandled Exception|ACCESS_VIOLATION' -Encoding UTF8 | Select-Object -First 40 | ForEach-Object { ($_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '').Substring(0, [Math]::Min(220, ($_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '').Length)) }
