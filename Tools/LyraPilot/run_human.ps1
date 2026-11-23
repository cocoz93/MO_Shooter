param(
    [string]$Tag = 'h1',
    # Human play only: win at 50 kills (user asked 11-22; Lyra's 30 ended games too soon). At the 11-22 pace (30 kills in 252 s, game_jp3)
    # 50 takes about 420 s, so the 300 s clock would end it first - 600 s lets the score decide. Shared/MatchRules.h keeps Lyra's values.
    [string]$ServerArgs = '--warmup=10 --matchSeconds=600 --targetScore=50',
    [int]$Bots = 9,
    [string]$BotArgs = '',   # extra BotClient arguments, e.g. '--ads' so remote bodies aim on screen (content C-5)
    [int]$Port = 7791,
    [string]$Res = '1280x720',
    [string]$LyraExtra = ''
)
# One human game: match server + windowed Lyra (keyboard and mouse) + bots. Bots join once Lyra is welcomed.
# Results: Saved\MOB\game_<Tag>_*. Ends when the Lyra window is closed.
$resX, $resY = $Res -split 'x'
$proj   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue     = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$repo   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter'
$outDir = "$proj\Saved\MOB"
$map    = "$outDir\map_expanse.mocl"
$pads   = "$outDir\pads_expanse.txt"   # health pads (content B, make_pads.py) - outside the repo like the map; no file = server without pads
$launchers = "$outDir\launchers_expanse.txt"   # jump pads (jumppad-plan j-1, make_launchers.py) - outside the repo like the map; no file = server without jump pads
$teleports = "$outDir\teleports_expanse.txt"   # teleports (teleport-plan t-1, make_teleports.py) - outside the repo like the map; no file = server without teleports
$base   = "$outDir\game_$Tag"
$log    = "${base}_lyra.log"

# An old log of the same tag already holds MOB_WELCOME, so the wait below would start bots before this Lyra joins (M-10). Move it aside first.
if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log -Destination "${base}_lyra-old-$(Get-Date -Format 'yyyyMMdd-HHmmss').log" -Force }

$srvArgList = @("--map=`"$map`"", "--port=$Port", '--exitWhenEmpty') + ($ServerArgs -split ' ' | Where-Object { $_ })
if (Test-Path -LiteralPath $pads) { $srvArgList += "--pads=`"$pads`"" }
if (Test-Path -LiteralPath $launchers) { $srvArgList += "--launchers=`"$launchers`"" }
if (Test-Path -LiteralPath $teleports) { $srvArgList += "--teleports=`"$teleports`"" }
$srv = Start-Process -FilePath "$repo\build\Server\Release\Server.exe" -ArgumentList $srvArgList -RedirectStandardOutput "${base}_server.txt" -RedirectStandardError "${base}_server_err.txt" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800

$list = @("`"$proj\Lyra.uproject`"", '/ShooterMaps/Maps/L_Expanse?NumBots=0', '-game', '-windowed', "-ResX=$resX", "-ResY=$resY",
          '-nosplash', '-log', "-abslog=`"$log`"", "-MOBServer=127.0.0.1:$Port", "-MOBTag=$Tag") + ($LyraExtra -split ' ' | Where-Object { $_ })
$lyra = Start-Process -FilePath $ue -ArgumentList $list -PassThru
"server=$($srv.Id) lyra=$($lyra.Id)"

$bot = $null
while (-not $lyra.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'MOB_WELCOME' -Quiet)) {
        "welcomed at $(Get-Date -Format 'HH:mm:ss')"
        $bot = Start-Process -FilePath "$repo\build\BotClient\Release\BotClient.exe" -ArgumentList (@("--map=`"$map`"", "--port=$Port", "--bots=$Bots") + ($BotArgs -split ' ' | Where-Object { $_ })) -RedirectStandardOutput "${base}_bots.txt" -PassThru -WindowStyle Hidden
        break
    }
    Start-Sleep -Milliseconds 500
}

$lyra.WaitForExit()
"lyra closed at $(Get-Date -Format 'HH:mm:ss') exit=$($lyra.ExitCode)"
if ($bot -and -not $bot.WaitForExit(5000)) { Stop-Process -Id $bot.Id -Force; 'bots stopped' }
if (-not $srv.WaitForExit(15000)) { Stop-Process -Id $srv.Id -Force; 'server stopped' }
'done'
