# 방 수 대 틱 시간 측정 한 판 — 서버 하나 + 더미 여러 프로세스.
# 서버와 더미를 다른 물리 코어에 묶는다: 서버 = 논리 0~9, 더미 = 논리 10~19.
#   (i9-10900 은 논리 코어 짝이 이웃이다 — 0x3, 0xC, … GetLogicalProcessorInformation 로 확인)
# 결과는 <OutDir>\load.csv 에 한 줄씩 쌓이고, 서버·더미 원본 로그는 <OutDir>\<Tag>\ 에 남는다.
param(
	[Parameter(Mandatory = $true)][int]$Rooms,
	[int]$GameThreads = 1,
	[int]$BotsPerProcess = 40,
	[int]$SettleSeconds = 25,		# 접속이 다 붙고 싸움이 시작될 때까지 — 이 구간 수치는 안 쓴다
	[int]$MeasureSeconds = 30,
	[int]$Seed = 1000,
	[int]$Port = 7951,
	[string]$Tag = "",
	[string]$OutDir = "",
	[string]$Map = "$env:USERPROFILE\Desktop\MyGit\MO_Shooter\Client\Lyra\Saved\MOB\map_expanse.mocl",
	[switch]$NoPawnBlock,		# 시험 전용 — 사람끼리 막기를 서버 · 더미 둘 다 끈다(--testNoPawnBlock, D83). 위치 어긋남 0 을 엄격히 볼 때
	[switch]$FireAlways		# 더미 전원 연사(--judgeFireAlways) — 적이 보이든 말든 계속 쏜다. 한계 측정 조건(D95)
)
$ErrorActionPreference = 'Stop'

$PlayersInRoom = 10		# wire::kMaxPlayersInRoom
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$serverExe = "$root\build\Server\Release\Server.exe"
$botExe = "$root\build\BotClient\Release\BotClient.exe"
if ($OutDir -eq "") { $OutDir = "$root\build\load" }
if ($Tag -eq "") { $Tag = "r{0}_t{1}_{2}" -f $Rooms, $GameThreads, (Get-Date -Format 'HHmmss') }
$runDir = Join-Path $OutDir $Tag
New-Item -ItemType Directory -Force $runDir | Out-Null

$serverMask = [IntPtr]0x3FF
$botMask = [IntPtr]0xFFC00
$botTotal = $Rooms * $PlayersInRoom
$processCount = [int][math]::Ceiling($botTotal / [double]$BotsPerProcess)

# 내 것이 아닌 프로세스가 쓴 CPU — 조용했는지의 근거(Get-CpuMark · Measure-CpuWindow)
. "$PSScriptRoot\cpu_window.ps1"

function Read-KeyValues {
	param([string]$Line)
	$map = [ordered]@{}
	foreach ($token in ($Line -split '\s+')) {
		$pair = $token -split '=', 2
		if ($pair.Count -eq 2) { $map[$pair[0]] = $pair[1] }
	}
	return $map
}

# 서버
$serverArgs = @(
	"`"--map=$Map`"", "--port=$Port", "--seed=$Seed", "--exitWhenEmpty",
	"--gameThreads=$GameThreads", "--maxSessions=$($botTotal + 16)",
	"--warmup=2", "--matchSeconds=36000", "--targetScore=1000000",
	"--measureFrom=$SettleSeconds", "--measureSeconds=$MeasureSeconds"
)
if ($NoPawnBlock) { $serverArgs += "--testNoPawnBlock" }
$server = Start-Process -FilePath $serverExe -ArgumentList $serverArgs -PassThru -WindowStyle Hidden `
	-RedirectStandardOutput "$runDir\server.txt" -RedirectStandardError "$runDir\server_err.txt"
$null = $server.Handle
$server.ProcessorAffinity = $serverMask

# 포트가 열리면 측정 시계가 돌기 시작한 것이다
$listenDeadline = (Get-Date).AddSeconds(60)
while (-not (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue)) {
	if ($server.HasExited) { throw "서버가 먼저 끝남 — $runDir\server.txt" }
	if ((Get-Date) -gt $listenDeadline) { $server.Kill(); throw "포트가 안 열림" }
	Start-Sleep -Milliseconds 100
}

# 더미 — 각자 자기 시작 시각부터 잰다. 지도 읽는 시간만큼 늦으니 서버 구간 안쪽으로 들어온다
$botProcesses = @()
$botMeasureFrom = [math]::Max(1, $SettleSeconds - 3)
$botSeconds = $SettleSeconds + $MeasureSeconds + 3
$left = $botTotal
for ($i = 0; $i -lt $processCount; ++$i) {
	$count = [math]::Min($BotsPerProcess, $left)
	$left -= $count
	$botArgs = @(
		"`"--map=$Map`"", "--port=$Port", "--bots=$count", "--seed=$($Seed + 1000 * ($i + 1))",
		"--seconds=$botSeconds", "--measureFrom=$botMeasureFrom", "--measureSeconds=$MeasureSeconds"
	)
	if ($NoPawnBlock) { $botArgs += "--testNoPawnBlock" }
	if ($FireAlways) { $botArgs += "--judgeFireAlways" }
	$bot = Start-Process -FilePath $botExe -ArgumentList $botArgs -PassThru -WindowStyle Hidden `
		-RedirectStandardOutput "$runDir\bot$i.txt" -RedirectStandardError "$runDir\bot${i}_err.txt"
	$null = $bot.Handle
	$bot.ProcessorAffinity = $botMask
	$botProcesses += $bot
}

$mine = @($server.Id) + @($botProcesses | ForEach-Object { $_.Id })
Start-Sleep -Seconds ($SettleSeconds - 2)
$cpuBegin = Get-CpuMark $mine
Start-Sleep -Seconds $MeasureSeconds
$cpuEnd = Get-CpuMark $mine
$cpu = Measure-CpuWindow $cpuBegin $cpuEnd

foreach ($bot in $botProcesses) {
	if (-not $bot.WaitForExit(60000)) { $bot.Kill(); Write-Output "더미가 안 끝나 강제 종료 pid=$($bot.Id)" }
}
if (-not $server.WaitForExit(60000)) { $server.Kill(); Write-Output "서버가 안 끝나 강제 종료" }

# 합치기
$serverLine = Select-String -Path "$runDir\server.txt" -Pattern '^LOAD ' | Select-Object -First 1
if (-not $serverLine) { throw "서버 LOAD 줄 없음 — $runDir\server.txt" }
$row = [ordered]@{ tag = $Tag; requestedRooms = $Rooms; otherCpuPercent = [math]::Round($cpu.otherPercent, 2); mineCpuPercent = [math]::Round($cpu.minePercent, 2); botProcesses = $processCount }
$load = Read-KeyValues $serverLine.Line
foreach ($key in $load.Keys) { $row[$key] = $load[$key] }

$botsOpen = 0; $compared = [uint64]0; $posDiff = [uint64]0; $velDiff = [uint64]0; $ammoDiff = [uint64]0; $flagDiff = [uint64]0
$gapP50 = 0.0; $gapP99 = 0.0; $gapMax = 0.0; $loopP99 = 0.0; $loopMax = 0.0; $botCpuSum = 0.0; $botCpuMax = 0.0; $botsFailed = 0; $gaps = [uint64]0
for ($i = 0; $i -lt $processCount; ++$i) {
	$line = Select-String -Path "$runDir\bot$i.txt" -Pattern '^LOADBOT ' | Select-Object -First 1
	if (-not $line) { ++$botsFailed; continue }
	$v = Read-KeyValues $line.Line
	$botsOpen += [int]$v.open; $compared += [uint64]$v.compared; $posDiff += [uint64]$v.posDiff; $velDiff += [uint64]$v.velDiff
	$ammoDiff += [uint64]$v.ammoDiff; $flagDiff += [uint64]$v.flagDiff; $gaps += [uint64]$v.stateGaps
	$gapP50 = [math]::Max($gapP50, [double]$v.stateGapP50Ms); $gapP99 = [math]::Max($gapP99, [double]$v.stateGapP99Ms); $gapMax = [math]::Max($gapMax, [double]$v.stateGapMaxMs)
	$loopP99 = [math]::Max($loopP99, [double]$v.loopP99Ms); $loopMax = [math]::Max($loopMax, [double]$v.loopMaxMs)
	$botCpuSum += [double]$v.cpuPercent; $botCpuMax = [math]::Max($botCpuMax, [double]$v.cpuPercent)
	if (-not (Select-String -Path "$runDir\bot$i.txt" -Pattern '^RESULT PASS' -Quiet)) { ++$botsFailed }
}
# 더미의 위치 대조는 판 전체를 보므로, 구간 밖에서 버린 입력도 같이 적어야 어긋난 까닭이 갈린다
$rejectLine = Select-String -Path "$runDir\server.txt" -Pattern '^REJECT ' | Select-Object -First 1
$row['droppedFullWholeRun'] = if ($rejectLine) { (Read-KeyValues $rejectLine.Line)['droppedFull'] } else { '' }
$row['botsOpen'] = $botsOpen
$row['botsFailedProcesses'] = $botsFailed
$row['compared'] = $compared; $row['posDiff'] = $posDiff; $row['velDiff'] = $velDiff; $row['ammoDiff'] = $ammoDiff; $row['flagDiff'] = $flagDiff
$row['stateGaps'] = $gaps
# 프로세스별 분위수는 합칠 수 없다 — 가장 나쁜 프로세스 값을 적는다
$row['worstStateGapP50Ms'] = $gapP50; $row['worstStateGapP99Ms'] = $gapP99; $row['worstStateGapMaxMs'] = $gapMax
$row['worstBotLoopP99Ms'] = $loopP99; $row['worstBotLoopMaxMs'] = $loopMax
$row['botCpuSumPercent'] = [math]::Round($botCpuSum, 1); $row['botCpuMaxPercent'] = [math]::Round($botCpuMax, 1)

$csv = Join-Path $OutDir 'load.csv'
# 열이 다른 옛 csv 에 덧붙이면 sweep.ps1 의 Import-Csv 가 칸을 엇갈려 읽는다 — 옛 파일은 이름을 바꿔 두고 새로 시작
if ((Test-Path $csv) -and ((Get-Content -Path $csv -TotalCount 1 -Encoding UTF8) -ne ($row.Keys -join ','))) {
	$old = Join-Path $OutDir ('load-cols-{0}.csv' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
	Move-Item -Path $csv -Destination $old
	Write-Output "열이 달라 옛 csv 를 옮김 → $old"
}
if (-not (Test-Path $csv)) { Set-Content -Path $csv -Value ($row.Keys -join ',') -Encoding UTF8 }
Add-Content -Path $csv -Value (($row.Values | ForEach-Object { "$_" }) -join ',') -Encoding UTF8

$row.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" } | Out-String -Width 4000 | Write-Output
