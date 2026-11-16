param(
    [string]$Tag = 'game',
    [string]$ServerArgs = '--warmup=2 --matchSeconds=40',
    [int]$Bots = 0,
    [string]$BotArgs = '',   # extra BotClient arguments, e.g. '--ads' so remote bodies aim on screen (content C-5)
    [string]$LyraArgs = '-MOBAutoInput -MOBQuitAt=60',
    [int]$Port = 7791,
    [int]$TimeoutSec = 300,
    [string]$Res = '640x360'
)
# Runs match server + optional bots + Lyra (-game, offscreen) once, then prints MOB_ lines and the server result lines.
$resX, $resY = $Res -split 'x'
$proj   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue     = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$repo   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter'
$srvExe = "$repo\build\Server\Release\Server.exe"
$botExe = "$repo\build\BotClient\Release\BotClient.exe"
$outDir = "$proj\Saved\MOB"
$map    = "$outDir\map_expanse.mocl"
$srvOut = "$outDir\game_${Tag}_server.txt"
$srvErr = "$outDir\game_${Tag}_server_err.txt"
$botOut = "$outDir\game_${Tag}_bots.txt"
$log    = "$outDir\game_${Tag}_lyra.log"

$srvArgList = @("--map=`"$map`"", "--port=$Port", '--exitWhenEmpty') + ($ServerArgs -split ' ' | Where-Object { $_ })
$srv = Start-Process -FilePath $srvExe -ArgumentList $srvArgList -RedirectStandardOutput $srvOut -RedirectStandardError $srvErr -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800

$lyraArgList = @("`"$proj\Lyra.uproject`"", '/ShooterMaps/Maps/L_Expanse?NumBots=0', '-game', '-RenderOffScreen', "-ResX=$resX", "-ResY=$resY",
                 '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"", "-MOBServer=127.0.0.1:$Port", "-MOBTag=$Tag") + ($LyraArgs -split ' ' | Where-Object { $_ })
$t0 = Get-Date
$lyra = Start-Process -FilePath $ue -ArgumentList $lyraArgList -PassThru

$bot = $null
if ($Bots -gt 0) {
    # Bots join once Lyra has connected so the human player always gets a place.
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline -and -not $lyra.HasExited) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'MOB_WELCOME' -Quiet)) { break }
        Start-Sleep -Milliseconds 500
    }
    $bot = Start-Process -FilePath $botExe -ArgumentList (@("--map=`"$map`"", "--port=$Port", "--bots=$Bots") + ($BotArgs -split ' ' | Where-Object { $_ })) -RedirectStandardOutput $botOut -PassThru -WindowStyle Hidden
}

if (-not $lyra.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing Lyra"; Stop-Process -Id $lyra.Id -Force }
"lyra exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
if ($bot -and -not $bot.WaitForExit(30000)) { Stop-Process -Id $bot.Id -Force; 'bots killed' }
if (-not $srv.WaitForExit(15000)) { Stop-Process -Id $srv.Id -Force; 'server killed' }

'--- server ---'
Select-String -Path $srvOut -Pattern '^(SERVER|REJECT|RESULT|ROOM|MATCH)' -Encoding UTF8 | ForEach-Object { $_.Line }
'--- lyra ---'
Select-String -Path $log -Pattern 'MOB_|Fatal error|Assertion failed' -Encoding UTF8 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
