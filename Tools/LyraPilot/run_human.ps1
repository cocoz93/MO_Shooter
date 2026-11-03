param(
    [string]$Tag = 'h1',
    [string]$ServerArgs = '--warmup=10 --matchSeconds=300',
    [int]$Bots = 9,
    [int]$Port = 7791,
    [string]$Res = '1280x720'
)
# One human game: match server + windowed Lyra (keyboard and mouse) + bots. Bots join once Lyra is welcomed.
# Results: Saved\MOB\game_<Tag>_*. Ends when the Lyra window is closed.
$resX, $resY = $Res -split 'x'
$proj   = 'C:\Users\USER\Documents\Unreal Projects\LyraStarterGame'
$ue     = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$repo   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter'
$outDir = "$proj\Saved\MOB"
$map    = "$outDir\map_expanse.mocl"
$base   = "$outDir\game_$Tag"
$log    = "${base}_lyra.log"

$srvArgList = @("--map=`"$map`"", "--port=$Port", '--exitWhenEmpty') + ($ServerArgs -split ' ' | Where-Object { $_ })
$srv = Start-Process -FilePath "$repo\build\Server\Release\Server.exe" -ArgumentList $srvArgList -RedirectStandardOutput "${base}_server.txt" -RedirectStandardError "${base}_server_err.txt" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800

$list = @("`"$proj\Lyra.uproject`"", '/ShooterMaps/Maps/L_Expanse?NumBots=0', '-game', '-windowed', "-ResX=$resX", "-ResY=$resY",
          '-nosplash', '-log', "-abslog=`"$log`"", "-MOBServer=127.0.0.1:$Port", "-MOBTag=$Tag")
$lyra = Start-Process -FilePath $ue -ArgumentList $list -PassThru
"server=$($srv.Id) lyra=$($lyra.Id)"

$bot = $null
while (-not $lyra.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'MOB_WELCOME' -Quiet)) {
        "welcomed at $(Get-Date -Format 'HH:mm:ss')"
        $bot = Start-Process -FilePath "$repo\build\BotClient\Release\BotClient.exe" -ArgumentList @("--map=`"$map`"", "--port=$Port", "--bots=$Bots") -RedirectStandardOutput "${base}_bots.txt" -PassThru -WindowStyle Hidden
        break
    }
    Start-Sleep -Milliseconds 500
}

$lyra.WaitForExit()
"lyra closed at $(Get-Date -Format 'HH:mm:ss') exit=$($lyra.ExitCode)"
if ($bot -and -not $bot.WaitForExit(5000)) { Stop-Process -Id $bot.Id -Force; 'bots stopped' }
if (-not $srv.WaitForExit(15000)) { Stop-Process -Id $srv.Id -Force; 'server stopped' }
'done'
