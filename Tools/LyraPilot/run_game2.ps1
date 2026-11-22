param(
    [string]$Tag = 'g1',
    [string]$ServerArgs = '--warmup=30 --matchSeconds=60',
    [int]$Bots = 8,
    [string]$ArgsA = '-MOBAutoInput -MOBAutoAim -nullrhi',
    [string]$ArgsB = '-MOBAutoInput -nullrhi',
    [int]$Port = 7792,
    [int]$TimeoutSec = 400,
    [string]$Res = '640x360'
)
# Runs match server + two Lyra clients (-game) + bots once. Lyras go first so both get a seat before bots fill the room.
# Results: Saved\MOB\game2_<Tag>_*. Compare them with check_game2.py.
$resX, $resY = $Res -split 'x'
$proj   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue     = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$repo   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter'
$srvExe = "$repo\build\Server\Release\Server.exe"
$botExe = "$repo\build\BotClient\Release\BotClient.exe"
$outDir = "$proj\Saved\MOB"
$map    = "$outDir\map_expanse.mocl"
$pads   = "$outDir\pads_expanse.txt"   # health pads (content B, make_pads.py) - outside the repo like the map; no file = server without pads
$launchers = "$outDir\launchers_expanse.txt"   # jump pads (jumppad-plan j-1, make_launchers.py) - outside the repo like the map; no file = server without jump pads
$base   = "$outDir\game2_$Tag"

$srvArgList = @("--map=`"$map`"", "--port=$Port", '--exitWhenEmpty', "--record=`"${base}_server_record.csv`"") + ($ServerArgs -split ' ' | Where-Object { $_ })
if (Test-Path -LiteralPath $pads) { $srvArgList += "--pads=`"$pads`"" }
if (Test-Path -LiteralPath $launchers) { $srvArgList += "--launchers=`"$launchers`"" }
$srv = Start-Process -FilePath $srvExe -ArgumentList $srvArgList -RedirectStandardOutput "${base}_server.txt" -RedirectStandardError "${base}_server_err.txt" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800

function Start-Lyra([string]$Name, [string]$Extra) {
    $list = @("`"$proj\Lyra.uproject`"", '/ShooterMaps/Maps/L_Expanse?NumBots=0', '-game', '-RenderOffScreen', "-ResX=$resX", "-ResY=$resY",
              '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"${base}_lyra$Name.log`"", "-MOBServer=127.0.0.1:$Port", "-MOBTag=$Tag$Name") + ($Extra -split ' ' | Where-Object { $_ })
    Start-Process -FilePath $ue -ArgumentList $list -PassThru
}

$t0 = Get-Date
$a = Start-Lyra 'A' $ArgsA
$b = Start-Lyra 'B' $ArgsB
"lyra pids A=$($a.Id) B=$($b.Id)"

# Bots join after both Lyras are welcomed. The first Welcome starts the warmup clock, so warmup must cover the gap.
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$welcomed = $false
while ((Get-Date) -lt $deadline -and -not $a.HasExited -and -not $b.HasExited) {
    $okA = (Test-Path "${base}_lyraA.log") -and (Select-String -Path "${base}_lyraA.log" -Pattern 'MOB_WELCOME' -Quiet)
    $okB = (Test-Path "${base}_lyraB.log") -and (Select-String -Path "${base}_lyraB.log" -Pattern 'MOB_WELCOME' -Quiet)
    if ($okA -and $okB) { $welcomed = $true; break }
    Start-Sleep -Milliseconds 500
}
"both welcomed=$welcomed after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds

$bot = $null
if ($welcomed -and $Bots -gt 0) {
    $bot = Start-Process -FilePath $botExe -ArgumentList @("--map=`"$map`"", "--port=$Port", "--bots=$Bots", "--record=`"${base}_bot_record.csv`"") -RedirectStandardOutput "${base}_bots.txt" -PassThru -WindowStyle Hidden
}

# Sample memory while waiting. Another session may use the PC, so free memory goes in the report.
[long]$peakA = 0; [long]$peakB = 0; $minFree = [double]::MaxValue
while ((Get-Date) -lt $deadline -and -not ($a.HasExited -and $b.HasExited)) {
    foreach ($pair in @(@('A', $a), @('B', $b))) {
        if (-not $pair[1].HasExited) {
            $pair[1].Refresh()
            if ($pair[0] -eq 'A') { $peakA = [Math]::Max($peakA, $pair[1].WorkingSet64) } else { $peakB = [Math]::Max($peakB, $pair[1].WorkingSet64) }
        }
    }
    $minFree = [Math]::Min($minFree, (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB)
    Start-Sleep -Seconds 3
}
foreach ($p in @($a, $b)) { if (-not $p.HasExited) { "TIMEOUT - killing Lyra $($p.Id)"; Stop-Process -Id $p.Id -Force } }
"lyras done after {0:N1}s exitA={1} exitB={2}" -f ((Get-Date) - $t0).TotalSeconds, $a.ExitCode, $b.ExitCode
"memory peakA={0:N1}GB peakB={1:N1}GB minFree={2:N1}GB" -f ($peakA / 1GB), ($peakB / 1GB), $minFree
if ($bot -and -not $bot.WaitForExit(30000)) { Stop-Process -Id $bot.Id -Force; 'bots killed' }
if (-not $srv.WaitForExit(15000)) { Stop-Process -Id $srv.Id -Force; 'server killed' }
"finished at $(Get-Date -Format 'HH:mm:ss')"
