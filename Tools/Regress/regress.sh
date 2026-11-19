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
PADS="$DATA/pads_expanse.txt"	# 받침 자료(컨텐츠 B) — Epic 맵 값이라 맵처럼 리포 밖
# 세 무기로 부활(시험 전용 --testStartAllWeapons, 무기 줍기 W-2)은 프로브 넷(경기규칙 · 재생 · 권위 · 되감기)의 서버에만 준다.
# 옵션 없이 회귀 한 벌을 돌려(W-5) 그 넷만 갈렸다 — 각본이 무기를 바꾸는데 권총 하나면 서버가 거절한다(switchRejected 2 · 39 · 94 · 1).
# 나머지 판은 Lyra 규칙(권총 하나로 부활)대로 돈다
SRV="$BIN/Server/Release/Server.exe"
BOT="$BIN/BotClient/Release/BotClient.exe"
PROBE="$BIN/Tools/ServerProbe/Release/ServerProbe.exe"
OUT_W="$(cygpath -m "$OUT")"
FAILS=0

verdict() {	# 이름 참거짓 설명
	if [ "$2" = 1 ]; then echo "PASS $1 $3"; else echo "FAIL $1 $3"; FAILS=$((FAILS + 1)); fi
}

num() {	# 줄 이름 — 줄 속 「 이름=숫자」의 숫자(없으면 빈 값)
	echo "$1" | sed -nE "s/.* $2=([0-9]+).*/\1/p"
}

# 대조 도구는 판정이 출력 속 수치라 기준선과 줄 단위로 비교한다(TIME 줄은 뺀다)
run_checks() {
	local tools="$BIN/Tools" g="$OUT/checks.txt" limit=""
	[ "$BREAK" = 1 ] && limit="--limit 5000"
	: > "$g"
	# expanse_crouch = 앉기 기록(컨텐츠 D). 앉기 줄(ONE crouch · RUN crouch · CROUCH_COUNT · PARAM crouch)은 앉기 기록에만 나온다
	for t in gym_fixed expanse_fixed expanse_free expanse_stress expanse_crouch; do
		local m=map_expanse.mocl
		[ "$t" = gym_fixed ] && m=map_gym.mocl
		"$tools/MoveCheck/Release/MoveCheck.exe" "$DATA/$m" "$DATA/move_$t.csv" $limit > "$OUT/move_$t.log" 2>&1
		grep -E '^(ONE frames|RUN frames|RUN segments|COUNT |ONE crouch|RUN crouch|CROUCH_COUNT|PARAM crouch)' "$OUT/move_$t.log" | sed "s/^/move_$t /" >> "$g"
	done
	"$tools/WeaponCheck/Release/WeaponCheck.exe" "$DATA/weaponrec_gym.csv" "$DATA/map_gym.mocl" > "$OUT/weapon.log" 2>&1
	grep -v '^TIME' "$OUT/weapon.log" | sed 's/^/weapon /' >> "$g"
	# 근접(컨텐츠 E) — Lyra 연습장 근접 기록(cprobe_e1g3)과 Shared ResolveMelee 대조. 어긋난 줄(DIFF)도 그대로 기준선에 들어가 다르면 잡힌다
	"$tools/MeleeCheck/Release/MeleeCheck.exe" "$DATA/cprobe_e1g3.log" "$DATA/cprobe_e1g3.csv" "$DATA/clips.bin" "$DATA/map_gym.mocl" > "$OUT/melee.log" 2>&1
	sed 's/^/melee /' "$OUT/melee.log" >> "$g"
	# Quinn 몸 근접 기록(17-d) — 걸으며 · 뛰며 칸(밀기 중 입력 · 낙하 재생)은 이 기록에만 있다. 판 시험(run_melee)도 같은 기록을 쓴다
	"$tools/MeleeCheck/Release/MeleeCheck.exe" "$DATA/cprobe_d3f2.log" "$DATA/cprobe_d3f2.csv" "$DATA/clips.bin" "$DATA/map_gym.mocl" > "$OUT/melee_q.log" 2>&1
	sed 's/^/melee_q /' "$OUT/melee_q.log" >> "$g"
	# 대시(D-1 · D-2) — Lyra 연습장 대시 기록(cprobe_dash6 = D-0 각본 46칸 + 누르는 프레임에 입력 바꿈 두 칸)과 Shared 이동 힘(설정 셋 · 두 칸 · 뜨기)을 칸마다 프레임 대조,
	# 능력(CDashAbility — 켠 입력 · 방향 · 막힘 · 쿨다운)을 입력마다 대조. 힘 없는 대조군 줄도 기준선에 든다
	"$tools/DashCheck/Release/DashCheck.exe" "$DATA/cprobe_dash6.log" "$DATA/cprobe_dash6.csv" "$DATA/map_gym.mocl" > "$OUT/dash.log" 2>&1
	sed 's/^/dash /' "$OUT/dash.log" >> "$g"
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
	# 받침을 켠다(컨텐츠 B) — 기록은 처치 · 결과 줄뿐이라 받침이 있어도 서버 ↔ 봇 바이트 비교는 그대로다
	# 사람끼리 막기는 끈다(시험 전용 --testNoPawnBlock, D83) — 봇끼리 부딪히면 남 자리를 늦게 알아 위치가 갈려, 서버 ↔ 같은 코드 위치 차이 0 을 못 본다. 막기 판은 run_block
	"$SRV" "--map=$MAP" --port=7953 --exitWhenEmpty "--record=$OUT_W/bots_srv.csv" --warmup=2 --matchSeconds=40 --seed=21 "--pads=$PADS" --testNoPawnBlock > "$OUT/bots_srv.log" 2>&1 &
	local spid=$!
	"$BOT" "--map=$MAP" --port=7953 "--record=$OUT_W/bots_bot.csv" --seed=21 --testNoPawnBlock $break_arg > "$OUT/bots_bot.log" 2>&1
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

	# 받침 갈래(컨텐츠 B-4) — 봇이 센 자기 회복 합 = 서버 회복 수, 찾아가던 중에 받은 회복이 한 번 이상(갈래가 실제로 받침에 닿았다)
	local pk mine sought
	pk="$(grep -a '^SERVER pickups' "$OUT/bots_srv.log" | sed -E 's/^SERVER pickups=([0-9]+).*/\1/')"
	mine="$(grep -a '^BOT pads' "$OUT/bots_bot.log" | sed -E 's/.* healsMine=([0-9]+).*/\1/')"
	sought="$(grep -a '^BOT pads' "$OUT/bots_bot.log" | sed -E 's/.* healsSought=([0-9]+).*/\1/')"
	ok=1
	[ -n "$pk" ] && [ "$pk" = "$mine" ] && [ -n "$sought" ] && [ "$sought" -gt 0 ] || ok=0
	verdict 받침갈래 $ok "서버 회복=$pk 봇 자기 회복 합=$mine 찾아가 닿음=$sought"

	# 무기 받침 갈래(컨텐츠 W-5) — 권총 하나로 부활한 봇이 안 가진 무기의 받침을 찾아가 집는다.
	# 서버가 보낸 내 줍기 소식 합(new · noSwitch · ammo — full · 칸 없음 · 판 끝은 소식이 없다) = 봇이 센 내 집음 = 예측이 받은 소식 = 그 입력부터 다시 돌린 수,
	# 칸 없음 · 결과 어긋남 · 탄 고침 · 가방 어긋남 0, 무기 둘 이상 든 비교 줄이 있고(가방 비교가 돌았다) · 서버 new 1 이상 · 찾아가던 중 집음 1 이상(갈래가 받침에 닿았다).
	# ammo · full 은 값만 본다 — 이 판은 ammo 0 · full 1 이라 사건 하나에 판정이 걸린다. 서버 ammo 갈래는 무기받침 프로브가 Lyra 기록과 견준다
	local wp wb snew snosw sammo bmine brecv brerun sent k
	wp="$(grep -a '^SERVER weaponPickups' "$OUT/bots_srv.log")"
	wb="$(grep -a '^BOT weapons' "$OUT/bots_bot.log")"
	snew="$(num "$wp" new)"; snosw="$(num "$wp" noSwitch)"; sammo="$(num "$wp" ammo)"
	bmine="$(num "$wb" pickupsMine)"; brecv="$(num "$wb" received)"; brerun="$(num "$wb" rerun)"
	ok=1
	if [ -n "$snew" ] && [ -n "$snosw" ] && [ -n "$sammo" ]; then
		sent=$((snew + snosw + sammo))
		[ "$bmine" = "$sent" ] && [ "$brecv" = "$sent" ] && [ "$brerun" = "$sent" ] || ok=0
	else
		ok=0
	fi
	for k in noSlot resultDiffs ammoFixes invDiff; do [ "$(num "$wb" $k)" = 0 ] || ok=0; done
	[ "$(num "$wb" invMulti)" -gt 0 ] 2>/dev/null && [ "$(num "$wb" sought)" -gt 0 ] 2>/dev/null && [ "$snew" -gt 0 ] 2>/dev/null || ok=0
	verdict 무기갈래 $ok "서버 new=$snew noSwitch=$snosw ammo=$sammo full=$(num "$wp" full) 봇 집음=$bmine 받음=$brecv 다시돌림=$brerun$(echo "$wb" | grep -oE ' (noSlot|resultDiffs|ammoFixes|invDiff|invMulti|sought)=[0-9]+' | tr -d '\n')"
}

# 사람끼리 막기(컨텐츠 F, D83 늘 켬) — 봇이 안 쏘고 가장 가까운 사람 쪽으로 걸어(--push) 계속 부딪힌다. 예측도 남 캡슐을 보고 서버와 다르면 고친다.
# 봇 RESULT PASS = 고친 수를 본 비교가 있고 · 예측이 한 번 이상 닿고 · 예측 단체 이상 0 · 탄 · 깃발 차이 0(떨어짐은 따로 센다)
# 같은 판에서 부활 직후 40틱 쉰다(--testPauseAfterSpawn, D84) — 서버가 빈 입력으로 움직여도 이동 막음이 클라와 같은 입력 수로 풀려야 한다(BOT moveLock diff=0)
run_block() {
	"$SRV" "--map=$MAP" --port=7964 --exitWhenEmpty "--record=$OUT_W/block_srv.csv" --warmup=2 --matchSeconds=40 --seed=21 > "$OUT/block_srv.log" 2>&1 &
	local spid=$!
	"$BOT" "--map=$MAP" --port=7964 --bots=10 --seed=21 --push --testPauseAfterSpawn=40 "--record=$OUT_W/block_bot.csv" > "$OUT/block_bot.log" 2>&1
	local brc=$?
	wait $spid
	local src=$? ok=1
	[ $brc = 0 ] && [ $src = 0 ] || ok=0
	cmp -s "$OUT/block_srv.csv" "$OUT/block_bot.csv" || ok=0
	grep -q '^RESULT PASS' "$OUT/block_bot.log" || ok=0
	local pawn moves odd lock idle
	pawn="$(grep -a '^SERVER pawn' "$OUT/block_srv.log")"
	moves="$(echo "$pawn" | sed -E 's/.* blockMoves=([0-9]+).*/\1/')"
	odd="$(echo "$pawn" | sed -E 's/.* oddSimplex=([0-9]+).*/\1/')"
	lock="$(grep -a '^BOT moveLock' "$OUT/block_bot.log" | sed -E 's/.* diff=([0-9]+).*/\1/')"
	idle="$(grep -a '^SERVER inputs' "$OUT/block_srv.log" | sed -E 's/.* idleMoves=([0-9]+).*/\1/')"
	[ -n "$moves" ] && [ "$moves" -gt 0 ] && [ "$odd" = 0 ] || ok=0
	[ -n "$idle" ] && [ "$idle" -gt 0 ] && [ "$lock" = 0 ] || ok=0
	verdict 막기판 $ok "봇 rc=$brc 서버 rc=$src 서버 막기입력=$moves 단체이상=$odd 빈입력이동=$idle 막음어긋남=$lock $(grep -a '^BOT pawn' "$OUT/block_bot.log" | grep -oE '(corrected|correctMax|predHits)=[0-9.]+' | tr '\n' ' ')"
}

# 렉 때 서버가 만든 빈 입력(D87) — 봇이 쏘며 돌아다니다 부활 직후 40틱 쉰다(--testPauseAfterSpawn). 서버는 빈 입력으로 걸음만 굴리고
# 무기 · 근접 · 조준 시계는 안 돌려야 한다 — 돌리면 서버가 한 발 더 쏜다(고치기 전 같은 판 ammoDiff=1358).
# 막기는 켠 채(봇이 서버 상태로 고친다) — 막기를 끄면 렉 동안 서버가 캐릭터를 떨어뜨린 만큼 봇이 못 따라가 위치 · 떨어짐 차이가 남는다(빈 입력의 몫, 고칠 것 아님)
run_pause() {
	"$SRV" "--map=$MAP" --port=7965 --exitWhenEmpty "--record=$OUT_W/pause_srv.csv" --warmup=2 --matchSeconds=40 --seed=21 > "$OUT/pause_srv.log" 2>&1 &
	local spid=$!
	"$BOT" "--map=$MAP" --port=7965 --bots=10 --seed=21 --testPauseAfterSpawn=40 "--record=$OUT_W/pause_bot.csv" > "$OUT/pause_bot.log" 2>&1
	local brc=$?
	wait $spid
	local src=$? ok=1
	[ $brc = 0 ] && [ $src = 0 ] || ok=0
	cmp -s "$OUT/pause_srv.csv" "$OUT/pause_bot.csv" || ok=0
	grep -q '^RESULT PASS' "$OUT/pause_bot.log" || ok=0
	local same ammo flag lock idle
	same="$(grep -a '^BOT 서버 ↔' "$OUT/pause_bot.log")"
	ammo="$(echo "$same" | sed -E 's/.* ammoDiff=([0-9]+).*/\1/')"
	flag="$(echo "$same" | sed -E 's/.* flagDiff=([0-9]+).*/\1/')"
	lock="$(grep -a '^BOT moveLock' "$OUT/pause_bot.log" | sed -E 's/.* diff=([0-9]+).*/\1/')"
	idle="$(grep -a '^SERVER inputs' "$OUT/pause_srv.log" | sed -E 's/.* idleMoves=([0-9]+).*/\1/')"
	[ -n "$idle" ] && [ "$idle" -gt 0 ] || ok=0
	[ "$ammo" = 0 ] && [ "$flag" = 0 ] && [ "$lock" = 0 ] || ok=0
	verdict 쉬기판 $ok "봇 rc=$brc 서버 rc=$src 빈입력이동=$idle 탄어긋남=$ammo 깃발어긋남=$flag 막음어긋남=$lock"
}

# 사람끼리 막기 대조(컨텐츠 F-1) — Lyra 연습장에서 두 사람이 부딪힌 기록을 Shared 이동으로 다시 돌려 위치 · 속도가 0.01 안이어야 한다.
# 기록 = F-1 에서 PASS(cells=40)를 낸 네 벌 그대로(f1a · f1b · f1c · f1e)
run_bump() {
	"$BIN/Tools/BumpCheck/Release/BumpCheck.exe" "$DATA/map_gym.mocl" "$DATA/cprobe_f1a.csv" "$DATA/cprobe_f1b.csv" "$DATA/cprobe_f1c.csv" "$DATA/cprobe_f1e.csv" > "$OUT/bump.log" 2>&1
	local rc=$? ok=1
	[ $rc = 0 ] || ok=0
	grep -q '^BUMPCHECK PASS' "$OUT/bump.log" || ok=0
	verdict 부딪힘대조 $ok "rc=$rc $(grep -a '^BUMPCHECK' "$OUT/bump.log" | cut -c1-90)"
}

# 받침 상태 · 겹침 · 대기(컨텐츠 B-2) — 프로브가 받침 각본을 걷고, 서버 받침 사건을 Lyra 단독 기록과 프레임 단위로 견준다
run_pads() {
	"$SRV" "--map=$MAP" --port=7958 --debug --warmup=0 --exitWhenEmpty "--pads=$PADS" > "$OUT/pads_srv.log" 2>&1 &
	local spid=$!
	"$PROBE" "$MAP" "$DATA/move_expanse_fixed.csv" --pads --port=7958 "--serverLog=$OUT_W/pads_srv.log" > "$OUT/pads_probe.log" 2>&1
	local prc=$?
	wait $spid
	local src=$? ok=1
	python "$REPO/Tools/LyraPilot/check_pads.py" "--lyra=$DATA/cprobe_pads3.log" "--probe=$OUT_W/pads_probe.log" > "$OUT/pads_check.log" 2>&1
	local crc=$?
	[ $prc = 0 ] && [ $src = 0 ] && [ $crc = 0 ] || ok=0
	grep -q '^PADS PASS' "$OUT/pads_check.log" || ok=0
	verdict 받침 $ok "프로브 rc=$prc 서버 rc=$src $(tail -1 "$OUT/pads_check.log" | cut -c1-40)"
}

# 무기 받침(컨텐츠 W-2) — 프로브가 Lyra 무기 판 각본(WEAPONS_SCRIPT)을 걷고, 서버 가방 · 받침 사건을 Lyra 기록 cprobe_w0a 와 프레임 단위로 견준다.
# 무기 줍기를 보는 판이라 Lyra 처럼 권총 하나로 부활한다(--testStartAllWeapons 안 줌)
run_weapons() {
	"$SRV" "--map=$MAP" --port=7960 --debug --warmup=0 --exitWhenEmpty "--pads=$PADS" > "$OUT/weapons_srv.log" 2>&1 &
	local spid=$!
	"$PROBE" "$MAP" "$DATA/move_expanse_fixed.csv" --weapons --port=7960 "--serverLog=$OUT_W/weapons_srv.log" > "$OUT/weapons_probe.log" 2>&1
	local prc=$?
	wait $spid
	local src=$? ok=1
	python "$REPO/Tools/LyraPilot/check_weapon_pads.py" "--lyra=$DATA/cprobe_w0a.log" "--probe=$OUT_W/weapons_probe.log" > "$OUT/weapons_check.log" 2>&1
	local crc=$?
	[ $prc = 0 ] && [ $src = 0 ] && [ $crc = 0 ] || ok=0
	grep -q '^WEAPONPADS PASS' "$OUT/weapons_check.log" || ok=0
	verdict 무기받침 $ok "프로브 rc=$prc 서버 rc=$src $(tail -1 "$OUT/weapons_check.log" | cut -c1-40)"
}

# 근접 시험판(컨텐츠 E-3) — Lyra 연습장 근접 칸(cprobe_d3f2, Quinn 몸)을 클립 실은 서버에 다시 세워 결과 · 대상 · 피해 · 따라간 자리를 견준다.
# 클립 없는 서버는 몸 판정이 상자라 칸 몇 개가 Lyra 와 갈린다(17-d d2) — 사람 판과 같은 클립 실은 서버로 본다
run_melee() {
	"$SRV" "--map=$DATA/map_gym.mocl" --port=7959 --debug --warmup=0 --exitWhenEmpty "--clips=$DATA/clips.bin" > "$OUT/melee_srv.log" 2>&1 &
	local spid=$!
	"$PROBE" "$DATA/map_gym.mocl" "$DATA/move_expanse_fixed.csv" "--melee=$DATA/cprobe_d3f2.log" --port=7959 "--serverLog=$OUT_W/melee_srv.log" > "$OUT/melee_probe.log" 2>&1
	local prc=$?
	wait $spid
	local src=$? ok=1
	[ $prc = 0 ] && [ $src = 0 ] || ok=0
	grep -q '^RESULT PASS' "$OUT/melee_probe.log" || ok=0
	verdict 근접 $ok "프로브 rc=$prc 서버 rc=$src $(grep -a '^MELEEPROBE cells=' "$OUT/melee_probe.log" | grep -oE '(cells|kindDiff|targetDiff|damageDiff|runMeasured|runFails)=[0-9]+' | tr '\n' ' ')"
}

run_rules() {
	"$SRV" "--map=$MAP" --port=7954 --debug --exitWhenEmpty --warmup=3 --targetScore=2 --testStartAllWeapons > "$OUT/rules_srv.log" 2>&1 &
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
	"$SRV" "--map=$MAP" --port=7955 --debug --exitWhenEmpty --warmup=0 --matchSeconds=100000 --fallKillZ=-1000000 --testStartAllWeapons > "$OUT/replay_srv.log" 2>&1 &
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
	"$SRV" "--map=$MAP" --port=7957 --debug --rewindShadow --exitWhenEmpty --warmup=0 --matchSeconds=100000 --targetScore=1000 --testStartAllWeapons > "$OUT/rewind_srv.log" 2>&1 &
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
	"$SRV" "--map=$MAP" --port=7956 --exitWhenEmpty --warmup=0 --matchSeconds=100000 --testStartAllWeapons > "$OUT/auth_srv.log" 2>&1 &
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
	if [ "$ONLY" != play ]; then run_checks; run_bump; fi
	if [ "$ONLY" != checks ]; then
		run_bots
		run_block
		run_pause
		run_rules
		run_replay
		run_authority
		run_rewind
		run_pads
		run_weapons
		run_melee
		run_multi
	fi
fi
if [ $FAILS = 0 ]; then echo "REGRESS PASS"; else echo "REGRESS FAIL $FAILS"; exit 1; fi
