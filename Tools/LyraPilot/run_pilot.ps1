param(
    [string]$Mode = 'move',          # server script: move | damage | weapon | idle
    [string]$ServerArgs = '',        # extra server args, e.g. "--rifle=50"
    [string]$Tag = 'moveA',
    [string]$LyraArgs = '',          # extra MOB args, e.g. "-MOBFollow=A -MOBShotAt=6,10"
    [int]$TimeoutSec = 240,
    [string]$Res = '1280x720'
)
$resX, $resY = $Res -split 'x'
# Runs pilot server + Lyra (-game, offscreen) once, then collects MOB_ lines.
$proj   = 'C:\Users\USER\Documents\Unreal Projects\LyraStarterGame'
$ue     = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$srvExe = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\Tools\LyraPilot\build\Release\LyraPilot.exe'
$outDir = "$proj\Saved\MOB"
New-Item -ItemType Directory -Force $outDir | Out-Null
$srvOut = "$outDir\server_$Tag.txt"

$srvArgList = @($Mode) + ($ServerArgs -split ' ' | Where-Object { $_ })
$srv = Start-Process -FilePath $srvExe -ArgumentList $srvArgList -RedirectStandardOutput $srvOut -PassThru -WindowStyle Hidden -WorkingDirectory (Split-Path $srvExe)
Start-Sleep -Milliseconds 800

$lyraArgList = @("`"$proj\Lyra.uproject`"", '/ShooterCore/Maps/L_ShooterGym?NumBots=0', '-game', '-RenderOffScreen', "-ResX=$resX", "-ResY=$resY",
                 '-unattended', '-nosplash', '-nosound', '-log', '-MOBServer=127.0.0.1:7790', "-MOBTag=$Tag") + ($LyraArgs -split ' ' | Where-Object { $_ })
$t0 = Get-Date
$lyra = Start-Process -FilePath $ue -ArgumentList $lyraArgList -PassThru
if (-not $lyra.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT after $TimeoutSec s - killing"; Stop-Process -Id $lyra.Id -Force }
"lyra exit after {0:N1}s" -f ((Get-Date) - $t0).TotalSeconds
Start-Sleep -Milliseconds 1500
if (-not $srv.HasExited) { Stop-Process -Id $srv.Id -Force; "server killed" }

Copy-Item "$proj\Saved\Logs\Lyra.log" "$outDir\lyra_$Tag.log" -Force
"--- server ---"
Get-Content $srvOut -Encoding UTF8 | Where-Object { $_ -notmatch '\[info\]' }
"--- lyra MOB/Bridge lines ---"
Select-String -Path "$outDir\lyra_$Tag.log" -Pattern 'MOB_|Bridge:|Error:|Fatal' -Encoding UTF8 | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[[^\]]*\]', '' }
