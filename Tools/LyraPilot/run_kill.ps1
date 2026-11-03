param(
    [ValidateSet('server','lyra')][string]$Kill = 'server',
    [string]$Tag = 'kill',
    [int]$Port = 7850,
    [int]$KillAfterSec = 25,
    [int]$Bots = 9
)
# Kills the server or Lyra mid-match on purpose and reports who is left. Judges only crashes and exit codes, never time.
$proj   = 'C:\Users\USER\Documents\Unreal Projects\LyraStarterGame'
$ue     = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$repo   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter'
$outDir = "$proj\Saved\MOB"
$map    = "$outDir\map_expanse.mocl"
$srvOut = "$outDir\game_${Tag}_server.txt"
$botOut = "$outDir\game_${Tag}_bots.txt"
$log    = "$outDir\game_${Tag}_lyra.log"
$crashes = "$proj\Saved\Crashes"
$crashBefore = @(Get-ChildItem $crashes -Directory -ErrorAction SilentlyContinue).Count

$srv = Start-Process -FilePath "$repo\build\Server\Release\Server.exe" -ArgumentList @("--map=`"$map`"", "--port=$Port", '--exitWhenEmpty', '--warmup=2', '--matchSeconds=60') -RedirectStandardOutput $srvOut -RedirectStandardError "$srvOut.err" -PassThru -WindowStyle Hidden
$null = $srv.Handle
Start-Sleep -Milliseconds 800
$lyraArgs = @("`"$proj\Lyra.uproject`"", '/ShooterMaps/Maps/L_Expanse?NumBots=0', '-game', '-nullrhi', '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"", "-MOBServer=127.0.0.1:$Port", "-MOBTag=$Tag", '-MOBAutoInput', '-MOBQuitAt=70')
$lyra = Start-Process -FilePath $ue -ArgumentList $lyraArgs -PassThru
$null = $lyra.Handle
$deadline = (Get-Date).AddSeconds(200)
while ((Get-Date) -lt $deadline -and -not $lyra.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'MOB_WELCOME' -Quiet)) { break }
    Start-Sleep -Milliseconds 500
}
$bot = Start-Process -FilePath "$repo\build\BotClient\Release\BotClient.exe" -ArgumentList @("--map=`"$map`"", "--port=$Port", "--bots=$Bots") -RedirectStandardOutput $botOut -PassThru -WindowStyle Hidden
$null = $bot.Handle
Start-Sleep -Seconds $KillAfterSec

if ($Kill -eq 'server') { "killing server pid=$($srv.Id)"; Stop-Process -Id $srv.Id -Force }
else { "killing lyra pid=$($lyra.Id)"; Stop-Process -Id $lyra.Id -Force }

$lyraDone = $lyra.WaitForExit(120000)
if (-not $lyraDone) { 'LYRA STILL RUNNING after 120s - stopping it'; Stop-Process -Id $lyra.Id -Force }
else { "lyra exit={0}" -f $lyra.ExitCode }
$botDone = $bot.WaitForExit(90000)
if (-not $botDone) { 'BOT STILL RUNNING after 90s - stopping it'; Stop-Process -Id $bot.Id -Force }
else { "bot exit={0}" -f $bot.ExitCode }
$srvDone = $srv.WaitForExit(60000)
if (-not $srvDone) { 'SERVER STILL RUNNING after 60s - stopping it'; Stop-Process -Id $srv.Id -Force }
else { "server exit={0}" -f $srv.ExitCode }

"crash folders before={0} after={1}" -f $crashBefore, @(Get-ChildItem $crashes -Directory -ErrorAction SilentlyContinue).Count
Select-String -Path $srvOut -Pattern '^(ROOM|SERVER connected|SERVER kills)' -Encoding UTF8 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(150, $_.Line.Length)) }
Select-String -Path $botOut -Pattern '^BOT |^RESULT' -Encoding UTF8 | Select-Object -Last 4 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(150, $_.Line.Length)) }
Select-String -Path $log -Pattern 'MOB_SUMMARY |MOB_DISCONNECT|MOB_LOST|MOB_CLOSE|Fatal error|Assertion failed|Unhandled Exception|ACCESS_VIOLATION' -Encoding UTF8 | Select-Object -Last 6 | ForEach-Object { $l = $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', ''; $l.Substring(0, [Math]::Min(200, $l.Length)) }
