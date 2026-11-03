# 방 수를 올려 가며 load.ps1 을 차례로 돌린다. 한계를 넘은 판이 StopAfter 번 이어지면 그 위는 안 돈다.
# 넘음 = 틱 p99 > 예산(16.7ms) 또는 밀린 틱이 전체의 1% 초과 — 여기서는 멈출지만 정한다. 판정은 csv 를 보고 한다.
# 예: powershell -File sweep.ps1 -GameThreads 1 -RoomsList 10,20,30   는 안 된다(-File 은 배열을 못 읽는다).
#     powershell -Command "& .\sweep.ps1 -GameThreads 1 -RoomsList 10,20,30"
param(
	[int]$GameThreads = 1,
	[int[]]$RoomsList = @(10, 20, 30),
	[int]$Repeat = 1,
	[int]$StopAfter = 2,
	[int]$SettleSeconds = 25,
	[int]$MeasureSeconds = 30,
	[int]$BasePort = 7700,
	[string]$Prefix = "s",
	[string]$OutDir = ""
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
if ($OutDir -eq "") { $OutDir = "$root\build\load" }
$csv = Join-Path $OutDir 'load.csv'
$budgetUs = 1000000.0 / 60.0
$over = 0
$port = $BasePort
foreach ($rooms in $RoomsList) {
	for ($r = 1; $r -le $Repeat; ++$r) {
		$tag = "{0}_t{1}_r{2}_n{3}" -f $Prefix, $GameThreads, $rooms, $r
		$port += 1
		& "$PSScriptRoot\load.ps1" -Rooms $rooms -GameThreads $GameThreads -SettleSeconds $SettleSeconds -MeasureSeconds $MeasureSeconds `
			-Seed (1000 + 7 * $r) -Port $port -Tag $tag -OutDir $OutDir | Out-Null
		$row = Import-Csv $csv | Where-Object { $_.tag -eq $tag } | Select-Object -Last 1
		"{0} rooms={1} sessions={2} p99={3}us worst={4}us late={5} gapP99={6}ms botCpuMax={7} other={8} bullets={9} pos={10} complete={11}" -f `
			$tag, $row.rooms, $row.sessions, $row.tickP99Us, $row.worstThreadP99Us, $row.lateTicks, $row.worstStateGapP99Ms, $row.botCpuMaxPercent, $row.otherCpuPercent, $row.bullets, $row.posDiff, $row.complete
		$ticks = 60.0 * [double]$row.seconds * $GameThreads
		$isOver = ([double]$row.worstThreadP99Us -gt $budgetUs) -or (([double]$row.lateTicks / $ticks) -gt 0.01)
		if ($isOver) { ++$over } else { $over = 0 }
		if ($over -ge $StopAfter) { "넘은 판이 $StopAfter 번 이어져 멈춤"; "SWEEP_DONE"; exit 0 }
	}
}
"SWEEP_DONE"
