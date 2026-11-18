# 측정 구간 동안 내 프로세스(서버·더미)가 아닌 쪽이 쓴 CPU — PC 가 조용했는지의 근거.
# PC 전체 사용 시간(GetSystemTimes)에서 내 프로세스 몫을 뺀다.
#   예전 방식(남의 프로세스마다 누적 CPU 시간을 더해 구간 앞뒤 차이)은 두 곳이 샜다
#   ① 구간 중 꺼진 프로세스는 끝 합계에서 통째로 빠져 음수까지 내려갔다(11-02 q1_t1_r50_n2 -25.2)
#   ② 관리자 권한 없이 못 읽는 프로세스(svchost · MsMpEng 등, 11-18 실측 324개 중 120개)는 아예 안 셌다
# 내 프로세스가 구간 중 꺼지면 그 몫이 남 쪽으로 넘어간다 — 「안 조용했다」 쪽으로 틀리니 판정을 후하게 만들지는 않는다
if (-not ('MOLoad.SystemTimes' -as [type])) {
	Add-Type -Namespace MOLoad -Name SystemTimes -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
'@
}

# 지금 시각의 PC 전체 CPU 시간 · 쉰 시간 · 내 프로세스 CPU 시간(초, 논리 코어 전부 합)
function Get-CpuMark {
	param([int[]]$MineIds)
	$idle = 0L; $kernel = 0L; $user = 0L
	if (-not [MOLoad.SystemTimes]::GetSystemTimes([ref]$idle, [ref]$kernel, [ref]$user)) { throw "GetSystemTimes 실패" }
	$mine = 0.0
	foreach ($id in $MineIds) {
		$p = Get-Process -Id $id -ErrorAction SilentlyContinue
		if ($p -and $p.TotalProcessorTime) { $mine += $p.TotalProcessorTime.TotalSeconds }
	}
	# kernel 에는 쉰 시간(idle)이 들어 있다 — 단위 100ns
	return @{ total = ($kernel + $user) / 1e7; busy = ($kernel + $user - $idle) / 1e7; mine = $mine }
}

# 두 시점 사이, PC 전체 CPU 대비 백분율 — other = 남이 쓴 몫, mine = 내 프로세스 몫
function Measure-CpuWindow {
	param($Begin, $End)
	$total = $End.total - $Begin.total
	$busy = $End.busy - $Begin.busy
	$mine = $End.mine - $Begin.mine
	return @{ otherPercent = ($busy - $mine) / $total * 100.0; minePercent = $mine / $total * 100.0 }
}
