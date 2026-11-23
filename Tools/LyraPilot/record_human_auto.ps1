param(
    [string]$Tag = 'ha1',
    [ValidateSet('both', 'move', 'pads')]
    [string]$Only = 'both',
    [int]$Port = 7870,
    [int]$Bots = 5,                  # bots that play normally (they shoot, so kills and assists happen)
    [int]$PushBots = 1,              # bots with --push: no shooting, they walk into the nearest human. One, because six push only each other (f3push)
    [int]$MatchSeconds = 240,      # script up to ~171 s (51 s + three approaches of up to 40 s, each ends at body contact), then about a minute of the five bots fighting (kills, assists)
    [string]$LyraExtra = '',         # extra bridge options for the move run, e.g. -MOBBreakCrouchBlock / -MOBBreakMeleeBlock (break runs)
    [double]$BreakPadsAt = -1,       # > 0: passed to the pads run (record_pads_bridge.ps1 -BreakPadsAt)
    [int]$TimeoutSec = 400,
    [string]$Res = '1280x720'
)
# Human-check automation (MyDocs/claude-plans/mo-shooter/human-check-auto-plan.md): two runs that replace hand checks with logs and photos.
#  move run - match server + normal bots + push bots + one Lyra (the bridge) with -MOBHumanCheck: the bridge script faces a wall,
#             fires the shotgun aimed then from the hip, crouches / crouch-walks / crouch-jumps / stands, holds melee with each weapon,
#             then plays on with the auto input; at the match result it opens the Lyra scoreboard and takes a photo.
#  pads run - record_pads_bridge.ps1 with -MOBHumanCheck -MOBEventShots: the bridge stands hurt on a pad, photos at take / cooldown / ready.
# Then check_human_auto.py judges both. Results: Saved\MOB\hauto_<Tag>* (move), padsb_<Tag>p* (pads), hauto_<Tag>_report.html
$proj   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra'
$ue     = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$repo   = 'C:\Users\USER\Desktop\MyGit\MO_Shooter'
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = "$proj\Saved\MOB"
$map    = "$outDir\map_expanse.mocl"
$pads   = "$outDir\pads_expanse.txt"
$launchers = "$outDir\launchers_expanse.txt"   # jump pads (jumppad-plan j-1, make_launchers.py) - outside the repo like the map; no file = server without jump pads
$teleports = "$outDir\teleports_expanse.txt"   # teleports (teleport-plan t-1, make_teleports.py) - outside the repo like the map; no file = server without teleports
$base   = "$outDir\hauto_$Tag"
$log    = "$base.log"
$resX, $resY = $Res -split 'x'

if ($Only -ne 'pads') {
    # an old log of the same tag already holds MOB_WELCOME - move it aside so the wait below sees only this run
    if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log -Destination "${base}-old-$(Get-Date -Format 'yyyyMMdd-HHmmss').log" -Force }
    # --testStartAllWeapons (server and both bot groups): the bridge script fires the shotgun and holds melee with each of the three weapons.
    # Respawning with the pistol only (Lyra rule, weapon pickup W-2) the server rejects those switches (W-5 runs w5ha / w5hb: switchRejected 12 / 15,
    # C shots and E repeat rifle / shotgun FAIL). The bots get it too so their predicted bag matches the server
    $srvArgs = @("--map=`"$map`"", "--port=$Port", '--exitWhenEmpty', '--warmup=5', "--matchSeconds=$MatchSeconds", '--testStartAllWeapons')
    if (Test-Path -LiteralPath $pads) { $srvArgs += "--pads=`"$pads`"" }
    if (Test-Path -LiteralPath $launchers) { $srvArgs += "--launchers=`"$launchers`"" }
    if (Test-Path -LiteralPath $teleports) { $srvArgs += "--teleports=`"$teleports`"" }
    $srv = Start-Process -FilePath "$repo\build\Server\Release\Server.exe" -ArgumentList $srvArgs -RedirectStandardOutput "${base}_server.txt" -RedirectStandardError "${base}_server_err.txt" -PassThru -WindowStyle Hidden
    $null = $srv.Handle
    Start-Sleep -Milliseconds 800

    # -MOBQuitAt only as a fallback: the bridge quits 3 s after the match result (-MOBHumanCheck) and opens the scoreboard at the result
    $quitAt = $MatchSeconds + 60
    $lyraArgs = @("`"$proj\Lyra.uproject`"", '/ShooterMaps/Maps/L_Expanse?NumBots=0', '-game', '-RenderOffScreen', "-ResX=$resX", "-ResY=$resY",
                  '-unattended', '-nosplash', '-nosound', '-log', "-abslog=`"$log`"", "-MOBServer=127.0.0.1:$Port", "-MOBTag=hauto_$Tag",
                  '-MOBAutoInput', '-MOBAutoAim', '-MOBAutoMelee', '-MOBEventShots', '-MOBViewLog', '-MOBHumanCheck', "-MOBQuitAt=$quitAt") + ($LyraExtra -split ' ' | Where-Object { $_ })
    $t0 = Get-Date
    $lyra = Start-Process -FilePath $ue -ArgumentList $lyraArgs -PassThru
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $welcomed = $false
    while ((Get-Date) -lt $deadline -and -not $lyra.HasExited) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'MOB_WELCOME' -Quiet)) { $welcomed = $true; break }
        Start-Sleep -Milliseconds 500
    }
    "move: lyra welcomed=$welcomed after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
    $botProcs = @()
    if ($welcomed) {
        # push bots first: the server puts a new player on the smaller team (Room.cpp ChooseTeam), so the one joining right after me is on the other team
        # and my scripted melee hits it (a teammate gives MELEE_SAME_TEAM: no damage, no hit reaction - pilot ha1)
        if ($PushBots -gt 0) {
            $botProcs += Start-Process -FilePath "$repo\build\BotClient\Release\BotClient.exe" -ArgumentList @("--map=`"$map`"", "--port=$Port", "--bots=$PushBots", '--push', '--seed=7', '--testStartAllWeapons') -RedirectStandardOutput "${base}_pushbots.txt" -RedirectStandardError "${base}_pushbots_err.txt" -PassThru -WindowStyle Hidden
            Start-Sleep -Seconds 2
            # the other bots join only after the script: the push bot walks to the nearest player, so alone with me it sticks to me (bump, my melee hits an enemy),
            # and my teammates are not there to shoot it (pilot ha2: it spawned with its own team, stuck to them and died 3 times; my melee hit nobody)
            $scriptDeadline = (Get-Date).AddSeconds(220)   # script up to ~171 s after the bridge is ready
            while ((Get-Date) -lt $scriptDeadline -and -not $lyra.HasExited) {
                if (Select-String -Path $log -Pattern 'MOB_HC_SCRIPT_DONE' -Quiet) { break }
                Start-Sleep -Milliseconds 500
            }
            "move: script done after {0:N1}s - other bots join" -f ((Get-Date) - $t0).TotalSeconds
        }
        if ($Bots -gt 0) {
            $botProcs += Start-Process -FilePath "$repo\build\BotClient\Release\BotClient.exe" -ArgumentList @("--map=`"$map`"", "--port=$Port", "--bots=$Bots", '--testStartAllWeapons') -RedirectStandardOutput "${base}_bots.txt" -RedirectStandardError "${base}_bots_err.txt" -PassThru -WindowStyle Hidden
        }
    }
    while ((Get-Date) -lt $deadline -and -not $lyra.HasExited) { Start-Sleep -Seconds 1 }
    if (-not $lyra.HasExited) { "move: TIMEOUT - killing Lyra $($lyra.Id)"; Stop-Process -Id $lyra.Id -Force }
    "move: lyra done after {0:N1}s exit={1}" -f ((Get-Date) - $t0).TotalSeconds, $lyra.ExitCode
    foreach ($b in $botProcs) { if (-not $b.WaitForExit(10000)) { Stop-Process -Id $b.Id -Force; "move: bots $($b.Id) stopped" } }
    if (-not $srv.WaitForExit(15000)) { Stop-Process -Id $srv.Id -Force; 'move: server stopped' }
    "move: server exit=$($srv.ExitCode)"
}

if ($Only -ne 'move') {
    $padArgs = @{ Tag = "${Tag}p"; Res = $Res; LyraExtra = '-MOBHumanCheck -MOBEventShots' }
    if ($BreakPadsAt -gt 0) { $padArgs.BreakPadsAt = $BreakPadsAt }
    & "$here\record_pads_bridge.ps1" @padArgs
}

$checkArgs = @("$here\check_human_auto.py", "--tag=$Tag", "--only=$Only")
& python @checkArgs
