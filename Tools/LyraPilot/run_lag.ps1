param(
    [string]$Tag = 'lag',
    [int]$DelayMs = 100,
    [int]$JitterMs = 33,
    [int]$Port = 7830,
    [int]$Bots = 9,
    [int]$QuitAt = 60,
    [int]$TimeoutSec = 200,
    [string]$ServerExtra = '',
    [string]$LyraExtra = ''
)
# One Lyra (no rendering) + bots, everyone through Tools/LagRelay. Judges only whether the match breaks; the sent-ack gap proves the delay is real.
$proj   = 'C:\Users\USER\Documents\Unreal Projects\LyraStarterGame'
$ue     = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$repo   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter'
$outDir = "$proj\Saved\MOB"
$map    = "$outDir\map_expanse.mocl"
$srvOut = "$outDir\game_${Tag}_server.txt"
$botOut = "$outDir\game_${Tag}_bots.txt"
$relayOut = "$outDir\game_${Tag}_relay.txt"
$log    = "$outDir\game_${Tag}_lyra.log"
$relayPort = $Port + 1

$srv = Start-Process -FilePath "$repo\build\Server\Release\Server.exe" -ArgumentList (@("--map=`"$map`"", "--port=$Port", '--exitWhenEmpty', '--warmup=2', '--matchSeconds=45') + ($ServerExtra -split ' ' | Where-Object { $_ })) -RedirectStandardOutput $srvOut -RedirectStandardError "$srvOut.err" -PassThru -WindowStyle Hidden
$null = $srv.Handle   # without this ExitCode stays empty after the process ends
Start-Sleep -Milliseconds 800
$relay = Start-Process -FilePath "$repo\build\Tools\LagRelay\Release\LagRelay.exe" -ArgumentList @("--listen=$relayPort", "--target=$Port", "--delayMs=$DelayMs", "--jitterMs=$JitterMs", "--seconds=$($TimeoutSec + 30)") -RedirectStandardOutput $relayOut -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 300

$lyraArgs = @("`"$proj\Lyra.uproject`"", '/ShooterMaps/Maps/L_Expanse?NumBots=0', '-game', '-nullrhi', '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"", "-MOBServer=127.0.0.1:$relayPort", "-MOBTag=$Tag", '-MOBAutoInput', "-MOBQuitAt=$QuitAt") + ($LyraExtra -split ' ' | Where-Object { $_ })
$t0 = Get-Date
$lyra = Start-Process -FilePath $ue -ArgumentList $lyraArgs -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline -and -not $lyra.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'MOB_WELCOME' -Quiet)) { break }
    Start-Sleep -Milliseconds 500
}
$bot = Start-Process -FilePath "$repo\build\BotClient\Release\BotClient.exe" -ArgumentList @("--map=`"$map`"", "--port=$relayPort", "--bots=$Bots") -RedirectStandardOutput $botOut -PassThru -WindowStyle Hidden
$null = $bot.Handle

$timedOut = $false
if (-not $lyra.WaitForExit($TimeoutSec * 1000)) { $timedOut = $true; "TIMEOUT - stopping Lyra"; Stop-Process -Id $lyra.Id -Force }
"lyra exit after {0:N1}s timedOut={1}" -f ((Get-Date) - $t0).TotalSeconds, $timedOut
if (-not $bot.WaitForExit(60000)) { Stop-Process -Id $bot.Id -Force; 'bots stopped' }
if (-not $srv.WaitForExit(20000)) { Stop-Process -Id $srv.Id -Force; 'server stopped' }
if (-not $relay.HasExited) { Stop-Process -Id $relay.Id -Force }
"server exit={0} bots exit={1}" -f $srv.ExitCode, $bot.ExitCode

Select-String -Path $srvOut -Pattern '^(SERVER|REJECT|ROOM)' -Encoding UTF8 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(170, $_.Line.Length)) }
Select-String -Path $botOut -Pattern 'BOT .*compared|^RESULT' -Encoding UTF8 | Select-Object -Last 2 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(120, $_.Line.Length)) }
# sent minus ack at 60 Hz is the round trip in ticks
$p = Select-String -Path $log -Pattern 'MOB_PROGRESS t=\S+ sent=(\d+) ack=(\d+)' -Encoding UTF8
$gaps = $p | ForEach-Object { [int]$_.Matches[0].Groups[1].Value - [int]$_.Matches[0].Groups[2].Value } | Where-Object { $_ -ge 0 }
if ($gaps) { "sent-ack gap samples={0} min={1} max={2}" -f @($gaps).Count, ($gaps | Measure-Object -Minimum).Minimum, ($gaps | Measure-Object -Maximum).Maximum }
Select-String -Path $log -Pattern 'MOB_SUMMARY |Fatal error|Assertion failed|Unhandled Exception|ACCESS_VIOLATION' -Encoding UTF8 | ForEach-Object { ($_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '').Substring(0, [Math]::Min(230, ($_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '').Length)) }
