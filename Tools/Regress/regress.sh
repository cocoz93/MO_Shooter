#!/bin/bash
# 회귀 시험 한 벌 — 대조 도구(맵·이동·무기) + 서버 판(봇·규칙·재생·권위·방 여럿).
# - 왜: 빌드 대상을 골라 돌리다 대조 도구가 옛 빌드로 남은 적이 있어, 전부 한 명령으로 묶는다.
#
#   Tools/Regress/regress.sh [--bin <빌드 폴더>] [--out <출력 폴더>] [--only checks|play|multi] [--break]
#     --break  일부러 틀림 — 대조 도구 입력을 잘라 기준선과 어긋나게 하고 봇에 --breakMirror 를 준다.
#              이때 FAIL 이 나와야 맞다(시험의 시험).
# 판정은 개수·바이트만 본다. 시간 값은 안 쓰므로 PC 가 바빠도 돌릴 수 있다.
# 기준선 = golden_checks.txt. 이동·충돌·무기 코드를 일부러 고쳤을 때만 갱신한다.

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BIN="$REPO/build"
OUT=""
ONLY=all
BREAK=0
while [ $# -gt 0 ]; do
	case "$1" in
		--bin) BIN="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--only) ONLY="$2"; shift 2 ;;
		--break) BREAK=1; shift ;;
		*) echo "모르는 인자 $1"; exit 2 ;;
	esac
done
[ -n "$OUT" ] || OUT="$BIN/regress/$(date +%H%M%S)"
mkdir -p "$OUT"

DATA="C:/Users/USER/Desktop/MyGit/MO_Shooter/Client/Lyra/Saved/MOB"
MAP="$DATA/map_expanse.mocl"
SRV="$BIN/Server/Release/Server.exe"
BOT="$BIN/BotClient/Release/BotClient.exe"
PROBE="$BIN/Tools/ServerProbe/Release/ServerProbe.exe"
OUT_W="$(cygpath -m "$OUT")"
FAILS=0

verdict() {	# 이름 참거짓 설명
	if [ "$2" = 1 ]; then echo "PASS $1 $3"; else echo "FAIL $1 $3"; FAILS=$((FAILS + 1)); fi
}

# 대조 도구는 판정이 출력 속 수치라 기준선과 줄 단위로 비교한다(TIME 줄은 뺀다)
run_checks() {
	local tools="$BIN/Tools" g="$OUT/checks.txt" limit=""
	[ "$BREAK" = 1 ] && limit="--limit 5000"
	: > "$g"
	for t in gym_fixed expanse_fixed expanse_free expanse_stress; do
		local m=map_expanse.mocl
		[ "$t" = gym_fixed ] && m=map_gym.mocl
		"$tools/MoveCheck/Release/MoveCheck.exe" "$DATA/$m" "$DATA/move_$t.csv" $limit > "$OUT/move_$t.log" 2>&1
		grep -E '^(ONE frames|RUN frames|RUN segments|COUNT )' "$OUT/move_$t.log" | sed "s/^/move_$t /" >> "$g"
	done
	"$tools/WeaponCheck/Release/WeaponCheck.exe" "$DATA/weaponrec_gym.csv" "$DATA/map_gym.mocl" > "$OUT/weapon.log" 2>&1
	grep -v '^TIME' "$OUT/weapon.log" | sed 's/^/weapon /' >> "$g"
	for t in expanse gym; do
		local m="map_$t.mocl"
		"$tools/MapCheck/Release/MapCheck.exe" "$DATA/$m" "$DATA/trace_$t.csv" > "$OUT/map_$t.log" 2>&1
		grep -E '^(CHECK|NORMAL) ' "$OUT/map_$t.log" | sed "s/^/map_$t /" >> "$g"
	done
	if cmp -s "$g" "$HERE/golden_checks.txt"; then
		verdict 대조도구 1 "$(wc -l < "$g")줄 기준선과 같음"
	else
		verdict 대조도구 0 "기준선과 다름 — diff $g $HERE/golden_checks.txt"
	fi
}

run_bots() {
	local break_arg=""
	[ "$BREAK" = 1 ] && break_arg="--breakMirror"
	"$SRV" "--map=$MAP" --port=7953 --exitWhenEmpty "--record=$OUT_W/bots_srv.csv" --warmup=2 --matchSeconds=40 --seed=21 > "$OUT/bots_srv.log" 2>&1 &
	local spid=$!
	"$BOT" "--map=$MAP" --port=7953 "--record=$OUT_W/bots_bot.csv" --seed=21 $break_arg > "$OUT/bots_bot.log" 2>&1
	local brc=$?
	wait $spid
	local src=$? ok=1
	[ $brc = 0 ] && [ $src = 0 ] || ok=0
	cmp -s "$OUT/bots_srv.csv" "$OUT/bots_bot.csv" || ok=0
	grep -q '^RESULT PASS' "$OUT/bots_bot.log" || ok=0
	verdict 봇한판 $ok "봇 rc=$brc 서버 rc=$src $(grep -E 'BOT 서버' "$OUT/bots_bot.log" | tail -1 | cut -c1-110)"

	# 시점을 안 보내는 봇은 전부 시점 없음 갈래여야 한다 — 다른 갈래가 늘면 기존 판정이 바뀐 것
	local rew shots noview
	rew="$(grep -a 'SERVER rewind' "$OUT/bots_srv.log")"
	shots="$(echo "$rew" | sed -E 's/.* shots=([0-9]+).*/\1/')"
	noview="$(echo "$rew" | sed -E 's/.*rewindNoView=([0-9]+).*/\1/')"
	ok=1
	[ -n "$shots" ] && [ "$shots" -gt 0 ] && [ "$shots" = "$noview" ] || ok=0
	verdict 시점없음갈래 $ok "shots=$shots noView=$noview"
}

run_rules() {
	"$SRV" "--map=$MAP" --port=7954 --debug --exitWhenEmpty --warmup=3 --targetScore=2 > "$OUT/rules_srv.log" 2>&1 &
	local spid=$!
	"$PROBE" "$MAP" "$DATA/move_expanse_fixed.csv" --port=7954 --rules > "$OUT/rules_probe.log" 2>&1
	local prc=$?
	wait $spid
	local src=$? ok=1
	[ $prc = 0 ] && [ $src = 0 ] || ok=0
	grep -q '^RESULT PASS' "$OUT/rules_probe.log" || ok=0
	verdict 경기규칙 $ok "프로브 rc=$prc 서버 rc=$src"
}

run_replay() {
	"$SRV" "--map=$MAP" --port=7955 --debug --exitWhenEmpty --warmup=0 --matchSeconds=100000 --fallKillZ=-1000000 > "$OUT/replay_srv.log" 2>&1 &
	local spid=$!
	"$PROBE" "$MAP" "$DATA/move_expanse_fixed.csv" --port=7955 > "$OUT/replay_probe.log" 2>&1
	local prc=$?
	wait $spid
	local src=$? ok=1
	[ $prc = 0 ] && [ $src = 0 ] || ok=0
	grep -q '^RESULT PASS' "$OUT/replay_probe.log" || ok=0
	verdict 재생 $ok "프로브 rc=$prc 서버 rc=$src"
}

run_rewind() {
	"$SRV" "--map=$MAP" --port=7957 --debug --rewindShadow --exitWhenEmpty --warmup=0 --matchSeconds=100000 --targetScore=1000 > "$OUT/rewind_srv.log" 2>&1 &
	local spid=$!
	"$PROBE" "$MAP" "$DATA/move_expanse_fixed.csv" --port=7957 --rewind "--serverLog=$OUT_W/rewind_srv.log" > "$OUT/rewind_probe.log" 2>&1
	local prc=$?
	wait $spid
	local src=$? ok=1
	[ $prc = 0 ] && [ $src = 0 ] || ok=0
	grep -q '^RESULT PASS' "$OUT/rewind_probe.log" || ok=0
	verdict 되감기 $ok "프로브 rc=$prc 서버 rc=$src $(grep -E 'REWIND 실패 [0-9]+건' "$OUT/rewind_probe.log" | cut -c1-40)"
}

run_authority() {
	"$SRV" "--map=$MAP" --port=7956 --exitWhenEmpty --warmup=0 --matchSeconds=100000 > "$OUT/auth_srv.log" 2>&1 &
	local spid=$!
	"$PROBE" "$MAP" "$DATA/move_expanse_fixed.csv" --port=7956 --authority "--serverLog=$OUT_W/auth_srv.log" > "$OUT/auth_probe.log" 2>&1
	local prc=$?
	wait $spid
	local src=$? ok=1
	[ $prc = 0 ] && [ $src = 0 ] || ok=0
	grep -q '^RESULT PASS' "$OUT/auth_probe.log" || ok=0
	verdict 권위 $ok "프로브 rc=$prc 서버 rc=$src $(grep -E 'AUTH 실패' "$OUT/auth_probe.log" | cut -c1-40)"
}

# 메모리 오류 검사 빌드는 서버 시작에만 16초쯤 걸린다
EXTRA=0
[[ "$BIN" == *asan* ]] && EXTRA=60

run_multi() {
	# 봇 30 · 스레드 2 → 방 셋, 방별 기록이 서버 ↔ 봇 바이트로 같아야 한다
	"$SRV" "--map=$MAP" --port=7961 --exitWhenEmpty --gameThreads=2 --warmup=2 --matchSeconds=30 --seed=31 "--record=$OUT_W/m1_srv.csv" > "$OUT/m1_srv.log" 2>&1 &
	local spid=$!
	"$BOT" "--map=$MAP" --port=7961 --bots=30 --seed=31 "--record=$OUT_W/m1_bot.csv" > "$OUT/m1_bot.log" 2>&1
	local brc=$?
	wait $spid
	local src=$? ok=1 n=0
	[ $brc = 0 ] && [ $src = 0 ] || ok=0
	for f in "$OUT"/m1_srv.csv*; do
		n=$((n + 1))
		cmp -s "$f" "${f/m1_srv/m1_bot}" || ok=0
	done
	[ $n = 3 ] || ok=0
	verdict 방셋 $ok "기록 $n벌 봇 rc=$brc 서버 rc=$src"

	# 판이 끝난 뒤 다음 봇은 새 방으로 간다
	"$SRV" "--map=$MAP" --port=7962 --duration=$((45 + EXTRA)) --gameThreads=2 --warmup=1 --matchSeconds=8 --seed=32 > "$OUT/m2_srv.log" 2>&1 &
	spid=$!
	"$BOT" "--map=$MAP" --port=7962 --bots=10 --seed=32 > "$OUT/m2_bot1.log" 2>&1
	local b1=$?
	sleep 1
	"$BOT" "--map=$MAP" --port=7962 --bots=10 --seed=33 > "$OUT/m2_bot2.log" 2>&1
	local b2=$?
	wait $spid
	src=$?
	ok=1
	[ $b1 = 0 ] && [ $b2 = 0 ] && [ $src = 0 ] || ok=0
	grep -q 'roomsCreated=2' "$OUT/m2_srv.log" || ok=0
	verdict 새방 $ok "봇 rc=$b1,$b2 서버 rc=$src"

	# 시험의 시험: 틀린 기록은 방마다 잡혀야 한다
	"$SRV" "--map=$MAP" --port=7963 --exitWhenEmpty --gameThreads=2 --warmup=1 --matchSeconds=10 --seed=34 "--record=$OUT_W/m3_srv.csv" > "$OUT/m3_srv.log" 2>&1 &
	spid=$!
	"$BOT" "--map=$MAP" --port=7963 --bots=20 --seed=34 --breakRecord "--record=$OUT_W/m3_bot.csv" > "$OUT/m3_bot.log" 2>&1
	wait $spid
	ok=1
	n=0
	for f in "$OUT"/m3_srv.csv*; do
		n=$((n + 1))
		cmp -s "$f" "${f/m3_srv/m3_bot}" && ok=0
	done
	[ $n = 2 ] || ok=0
	verdict 틀린기록잡음 $ok "기록 $n벌"
}

echo "출력 $OUT"
if [ "$ONLY" = multi ]; then
	run_multi
else
	if [ "$ONLY" != play ]; then run_checks; fi
	if [ "$ONLY" != checks ]; then
		run_bots
		run_rules
		run_replay
		run_authority
		run_rewind
		run_multi
	fi
fi
if [ $FAILS = 0 ]; then echo "REGRESS PASS"; else echo "REGRESS FAIL $FAILS"; exit 1; fi
