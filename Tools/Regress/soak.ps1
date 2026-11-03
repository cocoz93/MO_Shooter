param(
    [int]$Rounds = 20,
    [int]$Port = 7925,
    [string]$Bin = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\build',
    [string]$Out = 'C:\Users\USER\Desktop\MyGit\MO_Shooter\build\regress\soak'
)
# Runs short matches one after another on one server and samples memory, handles and threads after each. Counts only, no timing verdicts.
$map = 'C:\Users\USER\Documents\Unreal Projects\LyraStarterGame\Saved\MOB\map_expanse.mocl'
New-Item -ItemType Directory -Force $Out | Out-Null
$srv = Start-Process -FilePath "$Bin\Server\Release\Server.exe" -ArgumentList @("--map=`"$map`"", "--port=$Port", "--duration=$($Rounds * 28 + 30)", '--warmup=1', '--matchSeconds=5', '--seed=61') -RedirectStandardOutput "$Out\srv.log" -RedirectStandardError "$Out\srv.err" -PassThru -WindowStyle Hidden
$null = $srv.Handle   # without this ExitCode stays empty after the process ends
Start-Sleep -Seconds 3

$samples = @()
$botFails = 0
for ($i = 1; $i -le $Rounds; $i++) {
    $bot = Start-Process -FilePath "$Bin\BotClient\Release\BotClient.exe" -ArgumentList @("--map=`"$map`"", "--port=$Port", '--bots=10', "--seed=$(100 + $i)") -RedirectStandardOutput "$Out\bot$i.log" -PassThru -WindowStyle Hidden
    $null = $bot.Handle
    if (-not $bot.WaitForExit(60000)) { Stop-Process -Id $bot.Id -Force; $botFails++; "round $i bot timeout" }
    elseif ($bot.ExitCode -ne 0) { $botFails++ }
    Start-Sleep -Milliseconds 2500   # let the room close
    $srv.Refresh()
    if ($srv.HasExited) { "server gone at round $i"; break }
    $s = [pscustomobject]@{ Round = $i; PrivateMB = [math]::Round($srv.PrivateMemorySize64 / 1MB, 1); Handles = $srv.HandleCount; Threads = $srv.Threads.Count }
    $samples += $s
    "{0,3} private={1,8} handles={2,6} threads={3,4}" -f $s.Round, $s.PrivateMB, $s.Handles, $s.Threads
}
if (-not $srv.WaitForExit(($Rounds * 28 + 90) * 1000)) { Stop-Process -Id $srv.Id -Force; 'server stopped by script' }
"server exit={0} botFails={1}" -f $srv.ExitCode, $botFails
Select-String -Path "$Out\srv.log" -Pattern '^(ROUTER|SERVER connected)' -Encoding UTF8 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(120, $_.Line.Length)) }
if ($samples.Count -ge 11) {
    $mid = $samples[9]; $last = $samples[-1]
    "second half: privateMB {0} -> {1} ({2:+0.0;-0.0}) handles {3} -> {4} threads {5} -> {6}" -f $mid.PrivateMB, $last.PrivateMB, ($last.PrivateMB - $mid.PrivateMB), $mid.Handles, $last.Handles, $mid.Threads, $last.Threads
}
