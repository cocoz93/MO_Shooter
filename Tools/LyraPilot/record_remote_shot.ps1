param(
    [string]$Tag = 'r1',
    [int]$DelayMs = 100,
    [int]$JitterMs = 33,
    [int]$Port = 7850,
    [double]$HoldSec = 4.5,   # longer than the 4 s the shot waits before filming; route must start before the 7.35 s idle fidget, as the passed video did
    [string]$Res = '1280x720',
    [int]$TimeoutSec = 400
)
# Films one server-driven opponent with the same camera and the same route as the passed animation comparison (record_anim_compare.ps1 -Script startstop).
# Lyra goes through Tools/LagRelay with a real-match delay; the bot runs the fixed route and connects directly. Lyra sends no input, only a debug place every 20 s: the map has no spawn floor, and the server drops a session silent for 60 s.
# Results: Saved\MOB\remoteshot_<Tag>.csv and rec_remoteshot_<Tag>.bgra; make_remote_compare.py turns them into the side-by-side video.
$proj   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue     = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$repo   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter'
$outDir = "$proj\Saved\MOB"
$map    = "$outDir\map_gym.mocl"
$clips  = "$outDir\clips_rifle.bin"
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
$bot = Start-Process -FilePath "$repo\build\BotClient\Release\BotClient.exe" -ArgumentList @("--map=`"$map`"", "--port=$Port", '--bots=1', '--script=startstop', "--scriptHold=$HoldSec", "--seconds=$botSeconds") -RedirectStandardOutput $botOut -PassThru -WindowStyle Hidden
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
