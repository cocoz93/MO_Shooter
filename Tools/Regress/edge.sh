#!/bin/bash
# 비정상 상황 시험 한 벌 — 서버를 경우마다 띄우고 Tools/EdgeProbe 로 일을 일으킨 뒤, 서버 생존(rc)과 도구·봇 판정을 본다.
#   Tools/Regress/edge.sh [--bin <빌드 폴더>] [--out <출력 폴더>] [--only "<경우 이름들>"]
# ghost 는 75초, 나머지는 5~30초. 판정은 개수·rc 만 본다.

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BIN="$REPO/build"
OUT=""
ONLY=""
while [ $# -gt 0 ]; do
	case "$1" in
		--bin) BIN="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--only) ONLY="$2"; shift 2 ;;
		*) echo "모르는 인자 $1"; exit 2 ;;
	esac
done
[ -n "$OUT" ] || OUT="$BIN/regress/edge_$(date +%H%M%S)"
mkdir -p "$OUT"

MAP="C:/Users/USER/Desktop/MyGit/MO_Shooter/Client/Lyra/Saved/MOB/map_expanse.mocl"
SRV="$BIN/Server/Release/Server.exe"
BOT="$BIN/BotClient/Release/BotClient.exe"
EDGE="$BIN/Tools/EdgeProbe/Release/EdgeProbe.exe"
OUT_W="$(cygpath -m "$OUT")"
FAILS=0

want() { [ -z "$ONLY" ] || [[ " $ONLY " == *" $1 "* ]]; }
verdict() {
	if [ "$2" = 1 ]; then echo "PASS $1 $3"; else echo "FAIL $1 $3"; FAILS=$((FAILS + 1)); fi
}
# 메모리 오류 검사 빌드는 서버 시작에만 16초쯤 걸린다 — 고정 대기·제한 시간을 그만큼 늘린다
EXTRA=0
[[ "$BIN" == *asan* ]] && EXTRA=60
# 서버가 포트를 열 때까지 기다린다. 도구는 접속을 다시 시도하지 않으므로 먼저 붙으면 시험이 통째로 틀어진다
wait_ready() {
	for i in $(seq 1 120); do
		# 코어 로그와 줄이 섞이면 앞의 [ 가 저쪽 줄로 붙는다 — 뒤쪽만 본다
		grep -q '시작\] 포트' "$1" 2>/dev/null && return 0
		sleep 0.5
	done
	echo "    서버가 60초 안에 안 뜸: $1"
	return 1
}
# 도구가 실패하면 --exitWhenEmpty 서버는 영영 안 끝난다 — 기다리다 끊고 rc 를 124 로 돌려준다
wait_server() {
	for i in $(seq 1 $((60 + EXTRA + $2))); do
		kill -0 "$1" 2>/dev/null || { wait "$1"; return $?; }
		sleep 1
	done
	kill -9 "$1" 2>/dev/null
	wait "$1" 2>/dev/null
	return 124
}
srv_summary() { grep -E '^(ROUTER|SERVER connected|REJECT)' "$OUT/$1_srv.log" | cut -c1-200 | sed 's/^/    /'; }

# 이름 포트 "서버 인자" [EdgeProbe 추가 인자]
edge_only() {
	local name=$1 port=$2 sargs=$3 extra=$4
	"$SRV" "--map=$MAP" --port=$port $sargs > "$OUT/${name}_srv.log" 2>&1 &
	local spid=$!
	wait_ready "$OUT/${name}_srv.log"
	[ "$extra" = pid ] && extra="--serverPid=$(cat /proc/$spid/winpid)"
	"$EDGE" --port=$port --case=$name $extra > "$OUT/${name}_edge.log" 2>&1
	local erc=$?
	wait_server $spid 30
	local src=$? ok=1
	[ $erc = 0 ] && [ $src = 0 ] || ok=0
	verdict "$name" $ok "도구 rc=$erc 서버 rc=$src"
	grep -E '^EDGE (관측|실패|기대)' "$OUT/${name}_edge.log" | cut -c1-200 | sed 's/^/    /'
	srv_summary "$name"
}

if want leave_one; then
	# 봇 9명 곁에서 1명이 들어왔다 끊긴다 — 남은 봇의 위치 대조·기록이 멀쩡해야 한다
	"$SRV" "--map=$MAP" --port=7971 --exitWhenEmpty --warmup=3 --matchSeconds=25 --seed=41 "--record=$OUT_W/leave_one_srv.csv" > "$OUT/leave_one_srv.log" 2>&1 &
	spid=$!
	wait_ready "$OUT/leave_one_srv.log"
	if [ "$EXTRA" -gt 0 ]; then
		# asan 봇은 시작에 16초쯤 걸린다. 도구가 먼저 나가면 빈 방이 되어 서버가 먼저 끝나므로 봇을 앞세운다
		"$BOT" "--map=$MAP" --port=7971 --bots=9 --seed=41 "--record=$OUT_W/leave_one_bot.csv" > "$OUT/leave_one_bot.log" 2>&1 &
		bpid=$!
		sleep 25
		"$EDGE" --port=7971 --case=leave_one > "$OUT/leave_one_edge.log" 2>&1 &
		epid=$!
		wait $bpid; brc=$?
	else
		"$EDGE" --port=7971 --case=leave_one > "$OUT/leave_one_edge.log" 2>&1 &
		epid=$!
		sleep 0.5
		"$BOT" "--map=$MAP" --port=7971 --bots=9 --seed=41 "--record=$OUT_W/leave_one_bot.csv" > "$OUT/leave_one_bot.log" 2>&1
		brc=$?
	fi
	wait $epid; erc=$?
	wait_server $spid 30; src=$?
	ok=1
	[ $brc = 0 ] && [ $erc = 0 ] && [ $src = 0 ] || ok=0
	cmp -s "$OUT/leave_one_srv.csv" "$OUT/leave_one_bot.csv" || ok=0
	verdict leave_one $ok "봇 rc=$brc 도구 rc=$erc 서버 rc=$src $(grep -E 'BOT 서버' "$OUT/leave_one_bot.log" | tail -1 | cut -c1-100)"
	srv_summary leave_one
fi

want leave_team && edge_only leave_team 7972 "--duration=$((25 + EXTRA)) --warmup=2 --matchSeconds=100"
want rejoin_all && edge_only rejoin_all 7973 "--exitWhenEmpty --warmup=1 --matchSeconds=100"
want reconnect && edge_only reconnect 7974 "--exitWhenEmpty --warmup=1 --matchSeconds=100"
want reconnect_fast && edge_only reconnect_fast 7986 "--exitWhenEmpty --warmup=1 --matchSeconds=100"
want overflow && edge_only overflow 7975 "--exitWhenEmpty --warmup=1 --matchSeconds=100 --gameThreads=2"
want maxsessions && edge_only maxsessions 7976 "--duration=$((12 + EXTRA)) --maxSessions=5 --warmup=1 --matchSeconds=100"
want bad_packets && edge_only bad_packets 7977 "--duration=$((25 + EXTRA)) --warmup=1 --matchSeconds=100"
want ghost && edge_only ghost 7978 "--duration=$((85 + EXTRA)) --warmup=1 --matchSeconds=200"
want values && edge_only values 7979 "--exitWhenEmpty --warmup=0 --matchSeconds=100"
want flood && edge_only flood 7980 "--exitWhenEmpty --warmup=0 --matchSeconds=100" pid
want flood_10 && edge_only flood_10 7987 "--exitWhenEmpty --warmup=0 --matchSeconds=100" pid
want flood_100 && edge_only flood_100 7988 "--exitWhenEmpty --warmup=0 --matchSeconds=100" pid

if want postgame; then
	# 라우터가 닫힘을 알기 전의 틈은 1ms 안팎이라 여러 번 시도한다. 한 번도 못 맞히면 그 사실을 적는다
	hit=0
	for try in 1 2 3 4 5 6; do
		"$SRV" "--map=$MAP" --port=7981 --duration=$((9 + EXTRA)) --warmup=1 --matchSeconds=3 > "$OUT/postgame_srv.log" 2>&1 &
		spid=$!
		wait_ready "$OUT/postgame_srv.log"
		"$EDGE" --port=7981 --case=postgame > "$OUT/postgame_edge.log" 2>&1
		erc=$?
		wait_server $spid 30; src=$?
		rejected=$(grep -oE 'helloRejected=[0-9]+' "$OUT/postgame_srv.log" | cut -d= -f2)
		verdict "postgame#$try" $([ $erc = 0 ] && [ $src = 0 ] && echo 1 || echo 0) "도구 rc=$erc 서버 rc=$src helloRejected=$rejected"
		grep -E '^EDGE (관측|실패)' "$OUT/postgame_edge.log" | cut -c1-160 | sed 's/^/    /'
		if [ "${rejected:-0}" -gt 0 ]; then hit=1; break; fi
	done
	echo "    HelloRejected 경로 지남=$hit"
fi

if want threads; then
	for t in 3 4; do
		name=threads$t
		"$SRV" "--map=$MAP" --port=$((7982 + t)) --exitWhenEmpty --gameThreads=$t --warmup=2 --matchSeconds=20 --seed=5$t "--record=$OUT_W/${name}_srv.csv" > "$OUT/${name}_srv.log" 2>&1 &
		spid=$!
		"$BOT" "--map=$MAP" --port=$((7982 + t)) --bots=40 --seed=5$t "--record=$OUT_W/${name}_bot.csv" > "$OUT/${name}_bot.log" 2>&1
		brc=$?
		wait_server $spid 30; src=$?
		ok=1; n=0
		[ $brc = 0 ] && [ $src = 0 ] || ok=0
		for f in "$OUT/${name}"_srv.csv*; do
			n=$((n + 1))
			cmp -s "$f" "${f/_srv/_bot}" || ok=0
		done
		[ $n = 4 ] || ok=0
		verdict "$name" $ok "기록 $n벌 봇 rc=$brc 서버 rc=$src $(grep -E 'BOT 서버' "$OUT/${name}_bot.log" | tail -1 | cut -c1-90)"
		grep -E '^THREAD' "$OUT/${name}_srv.log" | cut -c1-40 | sed 's/^/    /'
	done
fi

if [ $FAILS = 0 ]; then echo "EDGE PASS"; else echo "EDGE FAIL $FAILS"; exit 1; fi
