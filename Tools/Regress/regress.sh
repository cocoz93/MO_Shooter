#!/bin/bash
# 회귀 시험 한 벌 — 대조 도구(맵·이동·무기) + 서버 판(봇·규칙·재생·권위·방 여럿).
# - 왜: 빌드 대상을 골라 돌리다 대조 도구가 옛 빌드로 남은 적이 있어, 전부 한 명령으로 묶는다.
#
#   Tools/Regress/regress.sh [--bin <빌드 폴더>] [--out <출력 폴더>] [--only checks|play|multi] [--break]
#     --break  일부러 틀림 — 대조 도구 입력을 잘라 기준선과 어긋나게 하고 봇에 --breakMirror 를 준다(대시 판 봇엔 --breakDash=predict, 수류탄 판 봇엔 --breakGrenade=predict).
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
	# 점프대(jumppad-plan j-3) — Lyra 점프대 기록(move_expanse_pads, 64구간)과 make_launchers.py 표로 겹침 비트 · 발사 프레임 · 발사 속도를 프레임마다 대조하고,
	# 이어 달리기는 기록 발사 칸 대신 우리 셈(TickWithLaunchers)으로 쏜다. 순간이동 구간 둘은 범위 밖이라 뺀다(D157)
	"$tools/MoveCheck/Release/MoveCheck.exe" "$DATA/map_expanse.mocl" "$DATA/move_expanse_pads.csv" $limit --launchers "$DATA/launchers_expanse.txt" > "$OUT/move_expanse_pads.log" 2>&1
	grep -E '^(ONE frames|RUN frames|RUN segments|COUNT |PAD |PAD_COVER|PADCHECK|RUNPAD )' "$OUT/move_expanse_pads.log" | sed "s/^/move_expanse_pads /" >> "$g"
	"$tools/WeaponCheck/Release/WeaponCheck.exe" "$DATA/weaponrec_gym.csv" "$DATA/map_gym.mocl" > "$OUT/weapon.log" 2>&1
	grep -v '^TIME' "$OUT/weapon.log" | sed 's/^/weapon /' >> "$g"
	# 근접(컨텐츠 E) — Lyra 연습장 근접 기록(cprobe_e1g3)과 Shared ResolveMelee 대조. 어긋난 줄(DIFF)도 그대로 기준선에 들어가 다르면 잡힌다
	"$tools/MeleeCheck/Release/MeleeCheck.exe" "$DATA/cprobe_e1g3.log" "$DATA/cprobe_e1g3.csv" "$DATA/clips.bin" "$DATA/map_gym.mocl" > "$OUT/melee.log" 2>&1
	sed 's/^/melee /' "$OUT/melee.log" >> "$g"
	# Quinn 몸 근접 기록(17-d) — 걸으며 · 뛰며 칸(밀기 중 입력 · 낙하 재생)은 이 기록에만 있다. 판 시험(run_melee)도 같은 기록을 쓴다
	"$tools/MeleeCheck/Release/MeleeCheck.exe" "$DATA/cprobe_d3f2.log" "$DATA/cprobe_d3f2.csv" "$DATA/clips.bin" "$DATA/map_gym.mocl" > "$OUT/melee_q.log" 2>&1
	sed 's/^/melee_q /' "$OUT/melee_q.log" >> "$g"
	# 대시(D-1 · D-2) — Lyra 연습장 대시 기록(cprobe_dash6 = D-0 각본 46칸 + 누르는 프레임에 입력 바꿈 두 칸)과 Shared 이동 힘(설정 셋 · 두 칸 · 뜨기)을 칸마다 프레임 대조,
	# 능력(CDashAbility — 켠 입력 · 방향 · 막힘 · 쿨다운)을 입력마다 대조, 막기 · 끊기(사격 · 재장전 · 근접 · 점프 · 조준 켜짐 · 탄창)를 줄마다 대조(D-2-b). 힘 없는 대조군 줄도 기준선에 든다
	"$tools/DashCheck/Release/DashCheck.exe" "$DATA/cprobe_dash6.log" "$DATA/cprobe_dash6.csv" "$DATA/map_gym.mocl" > "$OUT/dash.log" 2>&1
	sed 's/^/dash /' "$OUT/dash.log" >> "$g"
	# 수류탄(G-1 · G-2) — Lyra 연습장 수류탄 기록 15벌(G-0, 태그마다 cprobe_<태그>.log · .csv · _gren.csv)과 Shared 탄체 걸음(날아감 · 튕김 · 멈춤) · 능력 · 던짐 계산 · 겨눔 선 · 폭발 선 · 피해 · 처치 · 신관을
	# 프레임 · 수류탄 · 사람마다 대조. 태그별 줄 · 합계 줄 · GRENADECHECK PASS 까지 기준선에 든다. --break 이면 기록 한 벌(g0t2)을 빼 합계 줄이 어긋나야 한다
	local gtags="g0a1 g0a2 g0a3 g0a4 g0b1 g0b2 g0c1 g0c2 g0c3 g0end1 g0end2 g0life1 g0life2 g0t1 g0t2"
	[ "$BREAK" = 1 ] && gtags="${gtags% g0t2}"
	"$tools/GrenadeCheck/Release/GrenadeCheck.exe" "$DATA/map_gym.mocl" "$DATA" $gtags > "$OUT/grenade.log" 2>&1
	sed 's/^/grenade /' "$OUT/grenade.log" >> "$g"
	# 수류탄 정한 장면(L-1-a) — 같은 각본 두 판(cprobe_l1a1 · l1a2, 26칸 = 정한 자리 16 · 옮겨 놓기 10). 옛 15벌에 없던 갈래(2550 자름 · 구석 · 바닥+벽 멈춤 ·
	# 미끄러짐 마찰 · 겹침 둘)를 Lyra 와 대조한다. 옛 줄을 안 건드리려 따로 부른다. --break 이면 l1a2 를 빼 합계 줄이 어긋나야 한다
	local ltags="l1a1 l1a2"
	[ "$BREAK" = 1 ] && ltags="l1a1"
	"$tools/GrenadeCheck/Release/GrenadeCheck.exe" "$DATA/map_gym.mocl" "$DATA" $ltags > "$OUT/grenade_l1a.log" 2>&1
	sed 's/^/grenade_l1a /' "$OUT/grenade_l1a.log" >> "$g"
	# 수류탄 L_Expanse 장면(L-1-b) — 같은 각본 두 판(cprobe_l1b1 · l1b2, 5칸 = 이동은 지나고 수류탄은 막는 창틀 띠 셋 · 무기 광선은 지나고 폭발 선은 막는 문틀 뒤 · 막힘 없는 대조).
	# 맵은 $MAP(map_expanse.mocl). 지금 FAIL 줄도 그대로 기준선에 든다 — 1cm 띠 맞은 점(Lyra 쪽 맞은 점이
	# 면 위에서 0.003~0.004cm 비껴 법선 x 가 생김, lyra-match-progress L-1-b). --break 이면 l1b2 를 빼 합계 줄이 어긋나야 한다. 겨눔 선 갈림은 맵 파일 판 4 겨눔 선 깃발로 닫힘(aim traceDiff 6 → 0, L-7)
	local btags="l1b1 l1b2"
	[ "$BREAK" = 1 ] && btags="l1b1"
	"$tools/GrenadeCheck/Release/GrenadeCheck.exe" "$MAP" "$DATA" $btags > "$OUT/grenade_l1b.log" 2>&1
	sed 's/^/grenade_l1b /' "$OUT/grenade_l1b.log" >> "$g"
	# 수류탄 대 사람 몸 요소(L-8-a) — 캡슐 대 상자 쓸기 자가 시험(--sweep-test: 무차별 기준 · TraceBody 와 맞춤)과 l2c2 메시 맞음 대조(그 프레임 Lyra 몸 22 줄 · 맞음 6).
	# l2c2 는 상대 가까이 던져 생긴 프레임에 팔에 맞고 터지는 판뿐이라 날아감 · 튕김 프레임이 없어 GRENADECHECK FAIL 줄도 그대로 기준선에 든다. --break 이면 l2c2 를 안 돌려 줄이 어긋나야 한다
	"$tools/GrenadeCheck/Release/GrenadeCheck.exe" --sweep-test > "$OUT/grenade_sweep.log" 2>&1
	sed 's/^/grenade_sweep /' "$OUT/grenade_sweep.log" >> "$g"
	if [ "$BREAK" != 1 ]; then
		"$tools/GrenadeCheck/Release/GrenadeCheck.exe" "$DATA/map_gym.mocl" "$DATA" l2c2 > "$OUT/grenade_l2c.log" 2>&1
		sed 's/^/grenade_l2c /' "$OUT/grenade_l2c.log" >> "$g"
	fi
	# 수류탄 위 걸음(L-8-b) — g0t1 · g0t2 걸음 칸 넷(멈춘 수류탄 위를 지남 · 수류탄 없음)을 산 수류탄 몸을 올라설 맵 몸으로 보는 이동으로 다시 굴림.
	# 칸마다 대조군(지나는 칸은 수류탄을 뺌 · 없는 칸은 넣음)이 어긋나는지까지 도구가 판정한다. --break 이면 g0t2 를 빼 줄이 어긋나야 한다
	local wtags="g0t1 g0t2"
	[ "$BREAK" = 1 ] && wtags="g0t1"
	"$tools/GrenadeWalkCheck/Release/GrenadeWalkCheck.exe" "$DATA/map_gym.mocl" "$DATA" $wtags > "$OUT/grenade_walk.log" 2>&1
	sed 's/^/grenade_walk /' "$OUT/grenade_walk.log" >> "$g"
	# 같은 입력 두 누름(L-5-b) — L-1-c 기록 넷(수류탄 l1c1 · l1c2 · 대시 l1cd1 · l1cd2)의 누름 차례를 예측(CSelfPrediction)에 넣어 Lyra 근접 켬 · 끊김 · 막힘 · 대시 · 던짐 ·
	# 재장전 · 칸 끝 탄창과 대조. 차례 없이 넣은 대조군이 Lyra 와 다른 칸 수(fixedOrderDiff)까지 기준선에 든다. --break 이면 l1cd2 를 빼 합계 줄이 어긋나야 한다
	# L-12-b 기록 l12b1 = 대시 · 수류탄 짝 둘 · 셋 누름 여섯 · 넷 누름 · 근접 → Q + 쥔 사격(사격 켬 · 탄창까지 대조)
	# l12b2 = 같은 각본을 다시 기록 + 사격을 쥔 채 Q 둘(s_fhq2 · s_fhq5 — 수류탄이 사격을 끊고 다음 프레임에 다시 쏨, D156). 사격 켬 프레임 목록(fires=)까지 대조
	local ptags="l1c1 l1c2 l1cd1 l1cd2 l12b1 l12b2"
	[ "$BREAK" = 1 ] && ptags="l1c1 l1c2 l1cd1 l12b1 l12b2"
	"$tools/PressCheck/Release/PressCheck.exe" "$DATA/map_gym.mocl" "$DATA" $ptags > "$OUT/press.log" 2>&1
	sed 's/^/press /' "$OUT/press.log" >> "$g"
	for t in expanse gym; do
		local m="map_$t.mocl"
		"$tools/MapCheck/Release/MapCheck.exe" "$DATA/$m" "$DATA/trace_$t.csv" > "$OUT/map_$t.log" 2>&1
		grep -E '^(CHECK|NORMAL) ' "$OUT/map_$t.log" | sed "s/^/map_$t /" >> "$g"
	done
	# 애니 자세(L-11-a) — Lyra 애니 대조 기록 중 수류탄(쏜 뒤 덮기 자리, L-6-a) · 대시(빠지는 몽타주 뼈 비중, L-6-b) 세 무기를 AnimCheck state 로 다시 세운다.
	# 요약 줄(시각 줄 STATETIME · STATEPOSETIME 은 뺀다) · 몽타주 종류별 22부위 차(pose_kind.py) · 자세 출력 해시가 기준선에 든다. 이 단계 전엔 쏜 뒤 덮기를
	# 옛 자리로 되돌린 빌드도 기준선이 같았다(L-6-a). 옛 길 둘(--poseTest=128 덮기 자리 · 256 빼기 비중)은 이 단계 해시가 어긋난다(L-11-a 확인).
	# 기록 하나 약 11초. --break 이면 대시 샷건을 빼 줄이 어긋나야 한다
	local atags="gren_rifle gren_pistol gren_shotgun dash_rifle dash_pistol dash_shotgun"
	[ "$BREAK" = 1 ] && atags="${atags% dash_shotgun}"
	for a in $atags; do
		local at="${a%_*}" aw="${a#*_}"
		"$tools/AnimCheck/Release/AnimCheck.exe" state "$DATA/clips.bin" "$DATA/animcmp_${at}s_$aw.csv" "$DATA/animcmp_${at}s_${aw}_moves.csv" "$OUT_W/anim_$a.csv" \
			"--probe=$DATA/animcmp_${a}_probe.csv" > "$OUT/anim_$a.log" 2>&1
		echo "rc=$?" >> "$OUT/anim_$a.log"
		grep -vE '^STATE(POSE)?TIME ' "$OUT/anim_$a.log" | sed "s/^/anim_$a /" >> "$g"
		python "$REPO/Tools/LyraPilot/pose_kind.py" "$a" "$OUT_W/anim_$a.csv" "$DATA" 2>&1 | sed "s/^/anim_$a kind /" >> "$g"
		echo "anim_$a sha $(sha1sum < "$OUT/anim_$a.csv" | cut -c1-40)" >> "$g"
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
	# 기다림 판(시험 전용 --testLockstep, L-0-c · D121) — 서버는 봇 10명의 다음 입력이 다 와야 틱을 넘기고, 봇은 직전 입력을 적용한 상태를 받은 뒤 다음 입력을 만든다.
	# 벽시계로 돌면 상태가 막 도착하는 중에 만든 입력 · 접속 차례가 판마다 달라 같은 빌드도 판이 갈렸다(받침갈래가 가끔 회복 0 으로 FAIL — 미결 6). 이 판은 같은 빌드면 기록이 바이트째 같다
	"$SRV" "--map=$MAP" --port=7953 --exitWhenEmpty "--record=$OUT_W/bots_srv.csv" --warmup=2 --matchSeconds=40 --seed=21 "--pads=$PADS" --testNoPawnBlock --testLockstep=10 > "$OUT/bots_srv.log" 2>&1 &
	local spid=$!
	"$BOT" "--map=$MAP" --port=7953 "--record=$OUT_W/bots_bot.csv" --seed=21 --testNoPawnBlock --testLockstep $break_arg > "$OUT/bots_bot.log" 2>&1
	local brc=$?
	wait $spid
	local src=$? ok=1
	[ $brc = 0 ] && [ $src = 0 ] || ok=0
	cmp -s "$OUT/bots_srv.csv" "$OUT/bots_bot.csv" || ok=0
	grep -q '^RESULT PASS' "$OUT/bots_bot.log" || ok=0
	verdict 봇한판 $ok "봇 rc=$brc 서버 rc=$src $(grep -E 'BOT 서버' "$OUT/bots_bot.log" | tail -1 | cut -c1-110)"

	# 기다림 판이 실제로 기다렸나 — 서버 · 봇 모두 한도에 안 걸렸고(걸리면 덜 온 채로 돈 틱이 있다) 서버에 입력이 늦은 틱이 없다
	local sl bl si
	sl="$(grep -a '^SERVER lockstep' "$OUT/bots_srv.log")"
	bl="$(grep -a '^BOT lockstep' "$OUT/bots_bot.log")"
	si="$(grep -a '^SERVER inputs' "$OUT/bots_srv.log")"
	ok=1
	[ "$(num "$sl" players)" = 10 ] && [ "$(num "$sl" timeouts)" = 0 ] && [ "$(num "$bl" on)" = 1 ] && [ "$(num "$bl" timeouts)" = 0 ] && [ "$(num "$si" starvedTicks)" = 0 ] || ok=0
	verdict 기다림판 $ok "서버 기다림=$(num "$sl" waits) 한도=$(num "$sl" timeouts) 봇 보냄=$(num "$bl" sends) 한도=$(num "$bl" timeouts) 입력 늦은 틱=$(num "$si" starvedTicks)"

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
	# 서버가 보낸 내 줍기 소식 합(new · noSwitch · ammo — 칸 없음 · 판 끝은 소식이 없고 full 은 집으려던 사람에게 실패 소식, 판 14 · L-4) = 봇이 센 내 집음 = 예측이 받은 소식 = 그 입력부터 다시 돌린 수,
	# 서버 full = 보낸 실패 소식(failMsgs) = 봇이 받은 내 실패 소식(failMine), 모양 틀린 실패 소식(failBad) 0,
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
	for k in noSlot resultDiffs ammoFixes invDiff failBad; do [ "$(num "$wb" $k)" = 0 ] || ok=0; done
	[ "$(num "$wb" invMulti)" -gt 0 ] 2>/dev/null && [ "$(num "$wb" sought)" -gt 0 ] 2>/dev/null && [ "$snew" -gt 0 ] 2>/dev/null || ok=0
	[ -n "$(num "$wp" full)" ] && [ "$(num "$wp" failMsgs)" = "$(num "$wp" full)" ] && [ "$(num "$wb" failMine)" = "$(num "$wp" full)" ] || ok=0
	verdict 무기갈래 $ok "서버 new=$snew noSwitch=$snosw ammo=$sammo full=$(num "$wp" full) failMsgs=$(num "$wp" failMsgs) 봇 집음=$bmine 받음=$brecv 다시돌림=$brerun$(echo "$wb" | grep -oE ' (noSlot|resultDiffs|ammoFixes|invDiff|invMulti|sought|failMine|failBad)=[0-9]+' | tr -d '\n')"
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

# 대시(컨텐츠 D-3 · D-4) — 봇이 대시하며(--dash, 조준 · 근접 · 앉기와 섞어 막기 · 끊기 갈래까지) 싸운다. 서버엔 클립을 줘 대시 몽타주를 튼다.
# 봇 RESULT PASS = 위치 · 속도 · 탄 · 깃발 차이 0 · 대시 소식이 예측과 같음(dashDiff 0). 서버 켬 = 보낸 소식 = 봇이 받은 내 소식(mine = results),
# 켠 방향 수(montage F/B/L/R) = 쌓은 몽타주 사건 = 튼 수(버림 뺌) · 네 방향 다 1 이상(세는 자리마다 한 번은 지나갔다).
# 막기는 끈다(--testNoPawnBlock) — 위치 차이 0 을 엄격히 본다(run_bots 와 같은 까닭). --break 이면 예측에 대시를 안 넣어(--breakDash=predict) 어긋나야 한다
run_dash() {
	local break_arg=""
	[ "$BREAK" = 1 ] && break_arg="--breakDash=predict"
	"$SRV" "--map=$MAP" --port=7966 --exitWhenEmpty "--record=$OUT_W/dash_srv.csv" --warmup=2 --matchSeconds=40 --seed=21 "--pads=$PADS" "--clips=$DATA/clips.bin" --testNoPawnBlock > "$OUT/dash_srv.log" 2>&1 &
	local spid=$!
	"$BOT" "--map=$MAP" --port=7966 "--record=$OUT_W/dash_bot.csv" --seed=21 --testNoPawnBlock --dash --ads --melee --crouch $break_arg > "$OUT/dash_bot.log" 2>&1
	local brc=$?
	wait $spid
	local src=$? ok=1
	[ $brc = 0 ] && [ $src = 0 ] || ok=0
	cmp -s "$OUT/dash_srv.csv" "$OUT/dash_bot.csv" || ok=0
	grep -q '^RESULT PASS' "$OUT/dash_bot.log" || ok=0
	local sd ad bd started msgs mine results f b l r
	sd="$(grep -a '^SERVER dash ' "$OUT/dash_srv.log")"
	ad="$(grep -a '^SERVER animDash ' "$OUT/dash_srv.log")"
	bd="$(grep -a '^BOT dash ' "$OUT/dash_bot.log")"
	started="$(num "$sd" started)"; msgs="$(num "$sd" msgs)"
	mine="$(num "$bd" mine)"; results="$(num "$bd" results)"
	f="$(num "$sd" F)"; b="$(num "$sd" B)"; l="$(num "$sd" L)"; r="$(num "$sd" R)"
	[ -n "$started" ] && [ "$started" -gt 0 ] && [ "$msgs" = "$started" ] && [ "$mine" = "$started" ] && [ "$results" = "$started" ] || ok=0
	[ "$(num "$bd" dashDiff)" = 0 ] && [ "$(num "$bd" noSlot)" = 0 ] || ok=0
	# 판 끝 확인 안 된 켬(BOT dash unacked, L-11-c) — 방을 닫을 때 서버가 받고 안 돌린 대시 누름(봇 pressed − 서버 presses, 서버는 돌릴 때 센다)에만 있을 수 있다.
	# 그보다 크면 예측이 서버 없이 켠 것을 이 몫이 덮는다. 11-21 남겨 둔 대시 판 56개 중 누름이 어긋난 판은 하나(서버 364 · 봇 365)뿐이고 그 판만 봇 판정 FAIL 이었다
	local un bpr spr pdiff="?"
	un="$(num "$bd" unacked)"; bpr="$(num "$bd" pressed)"; spr="$(num "$sd" presses)"
	if [ -n "$un" ] && [ -n "$bpr" ] && [ -n "$spr" ]; then pdiff=$((bpr - spr)); [ "$pdiff" -ge 0 ] && [ "$un" -le "$pdiff" ] || ok=0; else ok=0; fi
	if [ -n "$f" ] && [ -n "$b" ] && [ -n "$l" ] && [ -n "$r" ]; then
		[ $((f + b + l + r)) = "$started" ] && [ "$f" -gt 0 ] && [ "$b" -gt 0 ] && [ "$l" -gt 0 ] && [ "$r" -gt 0 ] || ok=0
	else
		ok=0
	fi
	# animDash 줄 = events 방향 넷 · dropped 방향 넷 · starts 방향 넷(Room.cpp) — 사건 = 켠 방향 수, 튼 수 = 사건 − 버림
	local ev dr st i
	ev=($(echo "$ad" | sed -nE 's/.* events forward=([0-9]+) backward=([0-9]+) left=([0-9]+) right=([0-9]+) dropped .*/\1 \2 \3 \4/p'))
	dr=($(echo "$ad" | sed -nE 's/.* dropped forward=([0-9]+) backward=([0-9]+) left=([0-9]+) right=([0-9]+) starts .*/\1 \2 \3 \4/p'))
	st=($(echo "$ad" | sed -nE 's/.* starts forward=([0-9]+) backward=([0-9]+) left=([0-9]+) right=([0-9]+).*/\1 \2 \3 \4/p'))
	if [ ${#ev[@]} = 4 ] && [ ${#dr[@]} = 4 ] && [ ${#st[@]} = 4 ]; then
		[ "${ev[*]}" = "$f $b $l $r" ] || ok=0
		for i in 0 1 2 3; do [ "${st[$i]}" = $((ev[i] - dr[i])) ] || ok=0; done
	else
		ok=0
	fi
	# 몽타주 칸 빼기 진행(판 14 · L-4) — 서버가 쓴 칸(blendOutCells, 사람 × 틱) · 봇이 받은 칸 둘 다 있고, 빼기 비중 표가 없는 몽타주(대시 넷 밖)에 실린 칸 0.
	# 봇 칸은 봇마다 모든 사람 꼬리를 받아 서버 칸의 봇 수 배쯤(11-21 판 2842 · 28420 — 같은 수는 접속 · 나감 틱에 기대므로 값만 본다)
	local aw sbo
	aw="$(grep -a '^BOT animWire ' "$OUT/dash_bot.log")"
	sbo="$(num "$ad" blendOutCells)"
	[ -n "$sbo" ] && [ "$sbo" -gt 0 ] && [ "$(num "$aw" blendOut)" -gt 0 ] 2>/dev/null && [ "$(num "$aw" blendOutBad)" = 0 ] || ok=0
	press_check "$OUT/dash_srv.log" "$OUT/dash_bot.log" 1
	[ $press_ok = 1 ] || ok=0
	verdict 봇대시 $ok "봇 rc=$brc 서버 rc=$src 켬=$started 소식=$msgs 봇받음=$mine 방향=$f/$b/$l/$r 사건=${ev[*]} 버림=${dr[*]} 튼수=${st[*]}$(echo "$bd" | grep -oE ' (dashDiff|startMismatches|unconfirmed|unconfirmedPost|unacked)=[0-9]+' | tr -d '\n') 누름차=$pdiff 빼기칸 서버=$sbo 봇=$(num "$aw" blendOut) 대시밖=$(num "$aw" blendOutBad)$press_note"
}

# 같은 입력 누름 차례(L-5-c) — 봇대시 · 봇수류탄 판 공통. 봇은 넷(근접 · 대시 · 수류탄 · 재장전) 중 둘 이상 누른 입력마다 섞은 차례를 싣는다.
# 서버가 그런 입력을 다 믿고(unordered 0 · ordered = multi > 0), 봇이 센 수 − 서버 수는 0 이상 · 서버가 받고 안 돌린 입력(received − applied − whileDead —
# 방을 닫을 때 봇마다 마지막 하나쯤, 11-21 판 10) 이하. $3 = 1 이면 근접 · 대시 짝(MD)이 두 방향 다 1 이상이어야 한다(봇대시 판 — 봇이 대시 누름 넷 중 하나에
# 근접을 같이 실어(L-11-b, BOT dash pairMelee) 11-21 네 판 50/37 · 52/33 · 63/46 · 52/33. 전엔 우연히 겹친 입력뿐이라 0/0 ~ 18/11 로 흔들렸다).
# 결과는 press_ok(1 · 0) · press_note(판정 줄에 붙일 값)
press_check() {
	local sp bp si sm bm rc ap wd left="?" md
	press_ok=1
	sp="$(grep -a '^SERVER pressOrder ' "$1")"
	si="$(grep -a '^SERVER inputs ' "$1")"
	bp="$(grep -a '^BOT pressOrder ' "$2")"
	sm="$(num "$sp" multi)"; bm="$(num "$bp" multi)"
	rc="$(num "$si" received)"; ap="$(num "$si" applied)"; wd="$(num "$si" whileDead)"
	md=($(echo "$bp" | sed -nE 's/.* MD=([0-9]+)\/([0-9]+) .*/\1 \2/p'))
	# 빈 값으로 $(( )) 를 셈하면 bash 가 스크립트를 끝낸다 — 다 있는지 먼저 본다
	if [ -n "$sm" ] && [ -n "$bm" ] && [ -n "$rc" ] && [ -n "$ap" ] && [ -n "$wd" ] && [ ${#md[@]} = 2 ]; then
		left=$((rc - ap - wd))
		[ "$sm" -gt 0 ] && [ "$(num "$sp" ordered)" = "$sm" ] && [ "$(num "$sp" unordered)" = 0 ] || press_ok=0
		[ $((bm - sm)) -ge 0 ] && [ $((bm - sm)) -le "$left" ] || press_ok=0
		[ "$3" != 1 ] || { [ "${md[0]}" -gt 0 ] && [ "${md[1]}" -gt 0 ]; } || press_ok=0
	else
		press_ok=0
	fi
	press_note=" 누름 서버=$sm 봇=$bm 안돌림=$left unordered=$(num "$sp" unordered) reordered=$(num "$sp" reordered) MD=${md[0]}/${md[1]}"
}

# 수류탄(컨텐츠 G-3 · G-4) — 봇이 수류탄을 던지며(--grenade, 대시 · 조준 · 근접 · 앉기와 섞어 대시 중 · 근접 끊기 · 재장전 끊기 갈래까지) 싸운다. 서버엔 클립을 줘 수류탄 몽타주를 튼다.
# 봇 RESULT PASS = 위치 · 속도 · 탄 · 깃발 차이 0 · 던짐 소식이 예측과 같음(grenadeDiff 0) · 소식 · 피해 규칙 어긋남 0.
# 여기서 더 보는 것 = 서버 던짐 = 던짐 소식 = 봇이 받은 내 소식(mine) = 터짐(신관 + 맞음, = 터짐 소식) + 방을 닫을 때 남은 수(alive) · 봇이 센 내 피해 · 자기 피해 · 처치 = 서버 ·
# (alive — 서버는 끝 단계에도 던지고(L-3-b) 경기를 닫으면 틱을 안 돌려, 신관 2초(kGrenadeFuseFrames) 안의 막판 던짐이 안 터진 채 남는다. L-3-b 전엔 0.
#  던진 사람이 나가도 지우지 않으므로(L-3-d) 나갈 때 센 ownerLeft 로는 셈이 안 맞는다 — 나간 뒤 남은 틱에 터지면 두 번 센다) ·
# 봇이 받은 던짐 · 신관 터짐 · 맞음 터짐 · 상태 소식 · 상태 칸 · 튕김 = 서버 × 봇 수(봇마다 같은 소식을 받는다) · 몽타주 사건 = 던짐 · 튼 수 = 사건 − 버림.
# 한 번 이상이어야 하는 것 = 던짐 · 신관 터짐 · 하늘 던짐(noHit — 각본 갈래) · 피해(같음 비교가 0 = 0 이 아니게).
# 맞아 터짐 · 근접 끊기는 판마다 0 이 나온다(G-3-c · G-4-b 판 0~3 · 0~4). 대시 중 · 벽 · 재장전 끊기도 판마다 흔들린다 — 다섯 다 값만 적는다.
# 막기는 끈다(--testNoPawnBlock, run_bots 와 같은 까닭). --break 이면 예측에 수류탄을 안 넣어(--breakGrenade=predict) 어긋나야 한다
run_grenade() {
	local break_arg=""
	[ "$BREAK" = 1 ] && break_arg="--breakGrenade=predict"
	"$SRV" "--map=$MAP" --port=7967 --exitWhenEmpty "--record=$OUT_W/grenade_srv.csv" --warmup=2 --matchSeconds=40 --seed=21 "--pads=$PADS" "--clips=$DATA/clips.bin" --testNoPawnBlock > "$OUT/grenade_srv.log" 2>&1 &
	local spid=$!
	"$BOT" "--map=$MAP" --port=7967 "--record=$OUT_W/grenade_bot.csv" --seed=21 --testNoPawnBlock --grenade --dash --ads --melee --crouch $break_arg > "$OUT/grenade_bot.log" 2>&1
	local brc=$?
	wait $spid
	local src=$? ok=1
	[ $brc = 0 ] && [ $src = 0 ] || ok=0
	cmp -s "$OUT/grenade_srv.csv" "$OUT/grenade_bot.csv" || ok=0
	grep -q '^RESULT PASS' "$OUT/grenade_bot.log" || ok=0
	local sg sw sa sb bg bw nb thrown timer hit alive smsgs sents sbnc hurt bhit bwire ev dr st bc bna bfar bnp bpo bpa nbody
	sg="$(grep -a '^SERVER grenade ' "$OUT/grenade_srv.log")"
	sw="$(grep -a '^SERVER grenadeWire ' "$OUT/grenade_srv.log")"
	sa="$(grep -a '^SERVER animGrenade ' "$OUT/grenade_srv.log")"
	sb="$(grep -a '^SERVER grenadeBody ' "$OUT/grenade_srv.log")"
	bg="$(grep -a '^BOT grenade ' "$OUT/grenade_bot.log")"
	bw="$(grep -a '^BOT grenadeWire ' "$OUT/grenade_bot.log")"
	nb="$(grep -a '^BOT 방 ' "$OUT/grenade_bot.log" | head -1 | sed -nE 's/.*: 봇 ([0-9]+) .*/\1/p')"
	thrown="$(num "$sg" thrown)"; timer="$(num "$sg" timer)"; hit="$(num "$sg" hit)"; alive="$(num "$sg" alive)"
	smsgs="$(num "$sw" stateMsgs)"; sents="$(num "$sw" stateEntries)"; sbnc="$(num "$sw" stateBounced)"
	# 빈 값으로 $(( )) 를 셈하면 bash 가 스크립트를 끝낸다 — 아래 if 가 먼저 다 있는지 본다
	# grenadeHits 줄엔 「 damage=」가 둘(mine · bad)이라 num 대신 자리로 읽는다
	hurt=($(echo "$sg" | sed -nE 's/.* damage=([0-9]+) self=([0-9]+) kills=([0-9]+).*/\1 \2 \3/p'))
	bhit=($(grep -a '^BOT grenadeHits ' "$OUT/grenade_bot.log" | sed -nE 's/.* mine damage=([0-9]+) selfDamage=([0-9]+) kills=([0-9]+) .*/\1 \2 \3/p'))
	# grenadeWire 줄엔 「 msgs=」가 둘(전체 · 상태)이라 상태 몫도 자리로 읽는다 — throws · timer · hit · 상태 소식 · 칸 · 튕김
	bwire=($(echo "$bw" | sed -nE 's/.* throws=([0-9]+) .* boom timer=([0-9]+) hit=([0-9]+) state msgs=([0-9]+) entries=([0-9]+) bounced=([0-9]+) .*/\1 \2 \3 \4 \5 \6/p'))
	if [ -n "$thrown" ] && [ -n "$timer" ] && [ -n "$hit" ] && [ -n "$alive" ] && [ -n "$nb" ] && [ -n "$smsgs" ] && [ -n "$sents" ] && [ -n "$sbnc" ] && [ ${#hurt[@]} = 3 ] && [ ${#bhit[@]} = 3 ] && [ ${#bwire[@]} = 6 ]; then
		[ "$thrown" -gt 0 ] && [ "$timer" -gt 0 ] && [ "$(num "$sg" noHit)" -gt 0 ] 2>/dev/null && [ "${hurt[0]}" -gt 0 ] || ok=0
		[ "$(num "$sw" throwMsgs)" = "$thrown" ] && [ "$(num "$sw" boomMsgs)" = $((timer + hit)) ] && [ $((timer + hit + alive)) = "$thrown" ] || ok=0
		[ "$(num "$bg" mine)" = "$thrown" ] && [ "$(num "$bg" grenadeDiff)" = 0 ] || ok=0
		[ "${bhit[*]}" = "${hurt[*]}" ] || ok=0
		[ "${bwire[0]}" = $((thrown * nb)) ] && [ "${bwire[1]}" = $((timer * nb)) ] && [ "${bwire[2]}" = $((hit * nb)) ] || ok=0
		[ "${bwire[3]}" = $((smsgs * nb)) ] && [ "${bwire[4]}" = $((sents * nb)) ] && [ "${bwire[5]}" = $((sbnc * nb)) ] || ok=0
	else
		ok=0
	fi
	# animGrenade 줄 = events · dropped · starts · afterFire · reloadCancels(Server/main.cpp 가 찍음) — 사건 = 던짐, 튼 수 = 사건 − 버림
	ev="$(num "$sa" events)"; dr="$(num "$sa" dropped)"; st="$(num "$sa" starts)"
	if [ -n "$ev" ] && [ -n "$dr" ] && [ -n "$st" ]; then
		[ "$ev" = "$thrown" ] && [ "$st" = $((ev - dr)) ] || ok=0
	else
		ok=0
	fi
	# 몸 부위(L-8-a-2) — 클립 실은 판: 갈래 합 = checks · noAnim 0 · 몸 부위를 넣은 걸음 > 0 · 넣은 부위 수 = poses × 몸 수(ANIMBOUND bodies).
	# 몸 부위 맞음 bodyHits 는 판마다 달라(같은 빌드 두 판 4 · 8) 보이기만 한다
	bc="$(num "$sb" checks)"; bna="$(num "$sb" noAnim)"; bfar="$(num "$sb" far)"; bnp="$(num "$sb" noPose)"; bpo="$(num "$sb" poses)"; bpa="$(num "$sb" parts)"
	nbody="$(num "$(grep -a '^ANIMBOUND ' "$OUT/grenade_srv.log")" bodies)"
	if [ -n "$bc" ] && [ -n "$bna" ] && [ -n "$bfar" ] && [ -n "$bnp" ] && [ -n "$bpo" ] && [ -n "$bpa" ] && [ -n "$nbody" ]; then
		[ "$bc" = $((bna + bfar + bnp + bpo)) ] && [ "$bna" = 0 ] && [ "$bpo" -gt 0 ] && [ "$bpa" = $((bpo * nbody)) ] || ok=0
	else
		ok=0
	fi
	# 걸음 몸(L-8-b-2) — 서버 사람 걸음이 산 수류탄 몸을 넣었나(moves · bodies > 0) · 봇 · 다리의 던진 사람 무시 셈 = 서버 판정(ignoreMismatch 0) ·
	# 판이 수류탄 몸을 실제로 밟았나(hits + floors > 0 — 0 이면 봇이 몸을 안 넣어도 위치 차가 안 나 이 판이 그 빠뜨림을 못 본다). 봇 예측이 서버와 같은지는 위 RESULT PASS(posDiff 0)가 본다
	local swk bwk wmv wbd wim whit wfl
	swk="$(grep -a '^SERVER grenadeWalk ' "$OUT/grenade_srv.log")"
	bwk="$(grep -a '^BOT grenadeWalk ' "$OUT/grenade_bot.log")"
	wmv="$(num "$swk" moves)"; wbd="$(num "$swk" bodies)"; wim="$(num "$swk" ignoreMismatch)"; whit="$(num "$swk" hits)"; wfl="$(num "$swk" floors)"
	if [ -n "$wmv" ] && [ -n "$wbd" ] && [ -n "$wim" ] && [ -n "$whit" ] && [ -n "$wfl" ] && [ -n "$bwk" ]; then
		[ "$wmv" -gt 0 ] && [ "$wbd" -gt 0 ] && [ "$wim" = 0 ] && [ $((whit + wfl)) -gt 0 ] || ok=0
	else
		ok=0
	fi
	press_check "$OUT/grenade_srv.log" "$OUT/grenade_bot.log" 0
	[ $press_ok = 1 ] || ok=0
	verdict 봇수류탄 $ok "봇 rc=$brc 서버 rc=$src 봇 $nb 던짐=$thrown 신관=$timer 맞음=$hit 피해·자기·처치=${hurt[*]} 봇=${bhit[*]} 사건=$ev 버림=$dr 튼수=$st$(echo "$sg" | grep -oE ' (noHit|whileDash|melee|reload|wall|alive|ownerLeft)=[0-9]+' | tr -d '\n')$(echo "$bg" | grep -oE ' (grenadeDiff|startMismatches|unconfirmed|unconfirmedPost)=[0-9]+' | tr -d '\n') 몸부위$(echo "$sb" | grep -oE ' (poses|parts|bodyHits|capsuleHits)=[0-9]+' | tr -d '\n') 걸음몸$(echo "$swk" | grep -oE ' (moves|ownIgnored|ignoreMismatch|hits|floors)=[0-9]+' | tr -d '\n') 봇$(echo "$bwk" | grep -oE ' (stale|standableHits)=[0-9]+' | tr -d '\n')$press_note"
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

# 던진 사람 나감(L-3-d) — 프로브 --grenadeLeave 가 칸 셋(① 안 나감 대조 ② 던지고 나감 · 상대 몸에 닿음 ③ 던지고 나감 · 곁에서 터짐)을 돈다.
# 프로브 RESULT PASS = ① 맞아 터지고 피해 · ② 신관 터짐 · 피해 0 · ③ 같은 코드 곡선 피해 > 0 인데 피해 0. 여기서 더 보는 것 = 서버가 센 나간 사람 몫
# ownerLeft 2(② · ③) · 나간 뒤 몸 맞음 ownerGoneBody ≥ 1(② 가 몸에 닿았다 — 안 닿았으면 ② 가 아무것도 안 본다) · 반지름 안 피해 0 ownerGone ≥ 1(③) · 남은 수 alive 0
# 클립 없는 서버라 몸 부위가 없다 — 몸 부위를 본 수 전부 noAnim(L-8-a-2)
run_grenade_leave() {
	"$SRV" "--map=$MAP" --port=7968 --debug --exitWhenEmpty --warmup=0 --matchSeconds=100000 --targetScore=1000 > "$OUT/gleave_srv.log" 2>&1 &
	local spid=$!
	"$PROBE" "$MAP" "$DATA/move_expanse_fixed.csv" --port=7968 --grenadeLeave > "$OUT/gleave_probe.log" 2>&1
	local prc=$?
	wait $spid
	local src=$? ok=1 sg sb
	[ $prc = 0 ] && [ $src = 0 ] || ok=0
	grep -q '^RESULT PASS' "$OUT/gleave_probe.log" || ok=0
	sg="$(grep -a '^SERVER grenade ' "$OUT/gleave_srv.log")"
	sb="$(grep -a '^SERVER grenadeBody ' "$OUT/gleave_srv.log")"
	[ "$(num "$sb" checks)" -gt 0 ] 2>/dev/null && [ "$(num "$sb" noAnim)" = "$(num "$sb" checks)" ] || ok=0
	[ "$(num "$sg" ownerLeft)" = 2 ] && [ "$(num "$sg" ownerGoneBody)" -ge 1 ] 2>/dev/null && [ "$(num "$sg" ownerGone)" -ge 1 ] 2>/dev/null && [ "$(num "$sg" alive)" = 0 ] || ok=0
	verdict 던진사람나감 $ok "프로브 rc=$prc 서버 rc=$src$(echo "$sg" | grep -oE ' (timer|hit|alive|ownerLeft|ownerGoneBody|ownerGone|damage)=[0-9]+' | tr -d '\n') 몸부위$(echo "$sb" | grep -oE ' (checks|noAnim)=[0-9]+' | tr -d '\n')"
}

# 한 입력 안 누름 차례(판 14 · L-4) — 서버 시험이 쓰고 읽기 왕복(모음 11 · 차례 60)을 보고, 차례 실은 둘 · 0 을 실은 둘 켬 하나 · 하나만 켠 하나를 보내
# 서버가 multi 3 · ordered 2 · unordered 1 로 세나 본다(서버는 L-5 전까지 세기만). 서버 줄은 프로브가 서버 출력에서 읽는다
run_press_order() {
	"$SRV" "--map=$MAP" --port=7969 --debug --exitWhenEmpty --warmup=0 > "$OUT/press_srv.log" 2>&1 &
	local spid=$!
	"$PROBE" "$MAP" "$DATA/move_expanse_fixed.csv" --port=7969 --pressOrder "--serverLog=$OUT_W/press_srv.log" > "$OUT/press_probe.log" 2>&1
	local prc=$?
	wait $spid
	local src=$? ok=1 sp
	[ $prc = 0 ] && [ $src = 0 ] || ok=0
	grep -q '^RESULT PASS' "$OUT/press_probe.log" || ok=0
	sp="$(grep -a '^SERVER pressOrder ' "$OUT/press_srv.log")"
	[ "$(num "$sp" multi)" = 3 ] && [ "$(num "$sp" ordered)" = 2 ] && [ "$(num "$sp" unordered)" = 1 ] || ok=0
	verdict 누름차례 $ok "프로브 rc=$prc 서버 rc=$src$(echo "$sp" | grep -oE ' (multi|ordered|unordered)=[0-9]+' | tr -d '\n') $(grep -a '^PRESSORDER 왕복' "$OUT/press_probe.log" | sed -E 's/^PRESSORDER //')"
}

# 예비 가득 실패 소식(판 14 · L-4) — 둘이 들어와 A 만 권총 받침에 선다. 예비 가득이면 A 에게만 실패 소식 하나(B 0), 예비를 줄이면 실패 없이 탄 채움.
# 무기받침 판은 한 사람이라 「방 전원에게」를 못 가르고, 봇 판들은 예비 가득이 드물어(11-21 판 0) 이 판이 실패 소식의 받는 사람을 본다
run_fail_full() {
	"$SRV" "--map=$MAP" --port=7974 --debug --exitWhenEmpty --warmup=0 "--pads=$PADS" > "$OUT/failfull_srv.log" 2>&1 &
	local spid=$!
	"$PROBE" "$MAP" "$DATA/move_expanse_fixed.csv" --port=7974 --failFull > "$OUT/failfull_probe.log" 2>&1
	local prc=$?
	wait $spid
	local src=$? ok=1 wp
	[ $prc = 0 ] && [ $src = 0 ] || ok=0
	grep -q '^RESULT PASS' "$OUT/failfull_probe.log" || ok=0
	wp="$(grep -a '^SERVER weaponPickups' "$OUT/failfull_srv.log")"
	[ "$(num "$wp" full)" = 1 ] && [ "$(num "$wp" failMsgs)" = 1 ] && [ "$(num "$wp" ammo)" = 1 ] || ok=0
	verdict 가득실패 $ok "프로브 rc=$prc 서버 rc=$src$(echo "$wp" | grep -oE ' (ammo|full|failMsgs)=[0-9]+' | tr -d '\n') $(grep -a '^FAILFULL ② ' "$OUT/failfull_probe.log" | sed -E 's/^FAILFULL //')"
}

# 같은 입력 누름 차례(L-5-a · D127 · L-12-b) — 연습장 칸 열아홉 = L-1-c 짝 셋의 두 차례 여섯 · L-12-b 기록 아홉(대시 · 수류탄 짝 둘 · 셋 누름 여섯 · 넷 누름) · 쥔 사격 넷
# (근접 → Q · 대시 · 근접 + 쥔 사격, 앞 입력 근접 뒤 쥔 사격). 서버가 실린 차례대로 켜는지 근접 맞음 · 대시 · 던짐 · 탄창 · 25틱 뒤 몽타주로 본다.
# 서버 셈 = 능력 둘 이상 열넷 · 고정 차례와 다르게 켬 열(근접→대시 · 근접→Q 둘 · Q→대시 · 셋 누름 다섯 · 넷 누름) · 재장전 앞 둘 · 그 누름을 던짐이 끊음 하나(넷 누름은 근접이 먼저 막는다 — D127 ④)
# · 능력 사건 앞에 끼운 쏘기 셋(쥔 사격 칸 — L-12-b) · 근접이 막은 쏘기 하나(앞 입력 근접 칸)
run_press_cells() {
	"$SRV" "--map=$DATA/map_gym.mocl" --port=7975 --debug --warmup=0 --exitWhenEmpty "--clips=$DATA/clips.bin" > "$OUT/presscell_srv.log" 2>&1 &
	local spid=$!
	"$PROBE" "$DATA/map_gym.mocl" "$DATA/move_expanse_fixed.csv" --port=7975 --pressCells > "$OUT/presscell_probe.log" 2>&1
	local prc=$?
	wait $spid
	local src=$? ok=1 sp pc sm
	[ $prc = 0 ] && [ $src = 0 ] || ok=0
	grep -q '^RESULT PASS' "$OUT/presscell_probe.log" || ok=0
	pc="$(grep -a '^PRESSCELLS ' "$OUT/presscell_probe.log")"
	[ "$(num "$pc" cells)" = 19 ] && [ "$(num "$pc" fails)" = 0 ] && [ "$(num "$pc" noAnim)" = 0 ] || ok=0
	sp="$(grep -a '^SERVER pressOrder ' "$OUT/presscell_srv.log")"
	[ "$(num "$sp" abilityMulti)" = 14 ] && [ "$(num "$sp" reordered)" = 10 ] && [ "$(num "$sp" reloadFirst)" = 2 ] && [ "$(num "$sp" reloadPressCut)" = 1 ] && [ "$(num "$sp" fireAnimFirst)" = 3 ] || ok=0
	sm="$(grep -a '^SERVER melee ' "$OUT/presscell_srv.log")"
	[ "$(num "$sm" firesBlocked)" = 1 ] || ok=0
	verdict 누름칸 $ok "프로브 rc=$prc 서버 rc=$src $(echo "$pc" | sed -E 's/^PRESSCELLS //')$(echo "$sp" | grep -oE ' (abilityMulti|reordered|reloadFirst|reloadPressCut|fireAnimFirst)=[0-9]+' | tr -d '\n') 근접막은쏘기=$(num "$sm" firesBlocked)"
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
		run_dash
		run_grenade
		run_rules
		run_grenade_leave
		run_press_order
		run_fail_full
		run_press_cells
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
