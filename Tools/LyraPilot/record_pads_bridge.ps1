param(
    [string]$Tag = 'pb1',
    [int]$Port = 7860,
    [string]$Stand = '0,1086,378',   # on small pad 2 (pads_expanse.txt line 3): the probe walks pads 0 and 1, so the two never share a pad
    [double]$StandHp = 40,
    [double]$BreakPadsAt = -1,       # > 0: bridge breaks itself at that second (MOBridgeSubsystem.h -MOBBreakPadsAt); check_pads_bridge.py must then fail
    [int]$QuitAt = 95,               # after the probe's last step (3 s wait + 81 s script) and before its 20 s wait for the server log runs out
    [string]$ShotAt = '5,40',        # pad 2 off (taken at the first place) and on again (full health refuses the 30.75 s recheck)
    [int]$TimeoutSec = 400,
    [string]$Res = '640x360',
    [string]$LyraExtra = ''          # extra bridge options, e.g. '-MOBHumanCheck -MOBEventShots' (record_human_auto.ps1 pads run)
)
# Content B-3: one Lyra (the bridge) stands hurt on a health pad while ServerProbe --pads, a second player, walks the pad script on pads 0 and 1.
# The bridge must draw every pad as the server says (S2C_PICKUP), heal itself and the probe's body with the pad's own effect up to the server's health,
# and log each message. check_pads_bridge.py compares that log with the server's PADEV lines; check_pads.py still compares the probe with Lyra alone.
# Results: Saved\MOB\padsb_<Tag>_server.txt, padsb_<Tag>_probe.txt, padsb_<Tag>.log, padsb_<Tag>_<N>.png
$proj   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue     = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$repo   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter'
$outDir = "$proj\Saved\MOB"
$map    = "$outDir\map_expanse.mocl"
$pads   = "$outDir\pads_expanse.txt"
$moves  = "$outDir\move_expanse_fixed.csv"
$base   = "$outDir\padsb_$Tag"
$log    = "$base.log"
$resX, $resY = $Res -split 'x'

$srv = Start-Process -FilePath "$repo\build\Server\Release\Server.exe" -ArgumentList @("--map=`"$map`"", "--port=$Port", '--debug', '--exitWhenEmpty', '--warmup=1', '--matchSeconds=500', "--pads=`"$pads`"") -RedirectStandardOutput "${base}_server.txt" -RedirectStandardError "${base}_server_err.txt" -PassThru -WindowStyle Hidden
$null = $srv.Handle   # without this ExitCode stays empty after the process ends
Start-Sleep -Milliseconds 800

$lyraArgs = @("`"$proj\Lyra.uproject`"", '/ShooterMaps/Maps/L_Expanse?NumBots=0', '-game', '-RenderOffScreen', "-ResX=$resX", "-ResY=$resY",
              '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"", "-MOBServer=127.0.0.1:$Port", "-MOBTag=padsb_$Tag",
              "-MOBStandAt=$Stand", "-MOBStandHp=$StandHp", "-MOBQuitAt=$QuitAt", "-MOBShotAt=$ShotAt")
if ($BreakPadsAt -gt 0) { $lyraArgs += "-MOBBreakPadsAt=$BreakPadsAt" }
$lyraArgs += ($LyraExtra -split ' ' | Where-Object { $_ })
$t0 = Get-Date
$lyra = Start-Process -FilePath $ue -ArgumentList $lyraArgs -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$welcomed = $false
while ((Get-Date) -lt $deadline -and -not $lyra.HasExited) {
    # only a log this run has written counts (an older log of the same tag already holds MOB_WELCOME)
    if ((Test-Path $log) -and (Get-Item $log).LastWriteTime -ge $t0 -and (Select-String -Path $log -Pattern 'MOB_WELCOME' -Quiet)) { $welcomed = $true; break }
    Start-Sleep -Milliseconds 500
}
"lyra welcomed=$welcomed after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
Start-Sleep -Seconds 3   # the 1 s warmup ends and the match starts before the probe joins

$probe = $null
if ($welcomed) {
    $probe = Start-Process -FilePath "$repo\build\Tools\ServerProbe\Release\ServerProbe.exe" -ArgumentList @("`"$map`"", "`"$moves`"", '--pads', "--port=$Port", "--serverLog=`"${base}_server.txt`"") -RedirectStandardOutput "${base}_probe.txt" -RedirectStandardError "${base}_probe_err.txt" -PassThru -WindowStyle Hidden
}
while ((Get-Date) -lt $deadline -and -not $lyra.HasExited) { Start-Sleep -Seconds 1 }
if (-not $lyra.HasExited) { "TIMEOUT - killing Lyra $($lyra.Id)"; Stop-Process -Id $lyra.Id -Force }
"lyra done after {0:N1}s exit={1}" -f ((Get-Date) - $t0).TotalSeconds, $lyra.ExitCode
if ($probe -and -not $probe.WaitForExit(60000)) { Stop-Process -Id $probe.Id -Force; 'probe killed' }
if (-not $srv.WaitForExit(15000)) { Stop-Process -Id $srv.Id -Force; 'server killed' }
"server exit=$($srv.ExitCode) finished at $(Get-Date -Format 'HH:mm:ss')"
