#!/bin/bash
# 안전성 판(D172 · MyDocs soak-plan.md) — 서버 하나 + 봇 프로세스 둘을 정한 분 동안 돌리며 접속 · 끊김과 방 생성 · 삭제를 계속 반복시키고,
# 서버 SOAK 줄과 봇 끝 줄로 판정한다.
#   - 남는 쪽(--rejoin) — 판 끝까지 있다가 서버가 방을 닫으면 다시 접속한다(방 생성 · 삭제 반복)
#   - 끊는 쪽(--rejoin --churn) — 접속마다 T~5T초 뒤 스스로 끊고 다시 접속한다(MMO_Zone 에코 더미 규칙)
#
#   Tools/Soak/soak.sh --out <폴더> --minutes <분> [--bin <빌드 폴더>] [--port 7978] [--bots 20] [--churn 6] [--match 60] [--tail 20]
#                      [--break leave|memory] [--memLimitKB N] [--handleLimit N] [--infoOnly]
#     --break  일부러 틀림(서버 --testSoakBreak) — leave 는 「다 나간 뒤 0」 이, memory 는 「메모리가 계속 늚」 이 FAIL 이어야 맞다
#
# 판정은 개수와 메모리 · 핸들 양만 본다(시간 지표 안 씀) — PC 가 바빠도 돌릴 수 있다.
# 160분을 넘는 판은 거절한다 — 게임 스레드 틱 기록 칸이 60만 칸(Server/GameThread.cpp reserve · 60Hz 로 약 2.8시간)이라 그 뒤엔 칸이 커져 메모리 판정에 섞인다.

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BIN="$REPO/build"
OUT=""
MINUTES=""
PORT=7978
BOTS=20
CHURN=6
MATCH=60
TAIL=20
BREAK=""
# 문턱 — 기준선(soak-progress.md)에서 정했다
#   메모리 = 힙 남은 증가(시작 ↔ 다 나간 뒤 heapKB 에서 늘어나도 되는 몫 둘 — 닫힌 방 요약 · 버퍼 풀 청크 — 을 뺀 값), KB. 값은 아래 MEM_LIMIT 주석
#   핸들 = 끝(다 나간 뒤) − 시작(접속 전). 기준선 51~59(만든 세션 264 와 무관한 처음 한 번 몫) → 80. 접속마다 하나씩 새면 +264 라 잡힌다
MEM_LIMIT=2048		# 11-24 힙 기준선 k1~k3(3분) · k8(8분): 남은 증가 16~49KB(청크당 버퍼 몫 셈의 작은 어긋남 — 청크 수에 비례, 8분 판 17청크에 49KB) ·
			# 인사마다 64KB 를 안 돌려준 km 약 17MB. 2,048 = 긴 판에서 청크가 몇 배 늘어도 어긋남이 못 닿는 높이 · 3분 판에서도 인사당 8KB 넘게 새면 잡힌다
HANDLE_LIMIT=80
INFO_ONLY=0		# --infoOnly — 메모리 · 핸들을 판정하지 않고 적기만(메모리 오류 검사 빌드는 격리 칸이 메모리를 붙잡는다)
while [ $# -gt 0 ]; do
	case "$1" in
		--bin) BIN="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--minutes) MINUTES="$2"; shift 2 ;;
		--port) PORT="$2"; shift 2 ;;
		--bots) BOTS="$2"; shift 2 ;;
		--churn) CHURN="$2"; shift 2 ;;
		--match) MATCH="$2"; shift 2 ;;
		--tail) TAIL="$2"; shift 2 ;;
		--break) BREAK="$2"; shift 2 ;;
		--memLimitKB) MEM_LIMIT="$2"; shift 2 ;;
		--handleLimit) HANDLE_LIMIT="$2"; shift 2 ;;
		--infoOnly) INFO_ONLY=1; shift ;;
		*) echo "모르는 인자 $1"; exit 2 ;;
	esac
done
if [ -z "$OUT" ] || [ -z "$MINUTES" ]; then echo "--out · --minutes 는 꼭"; exit 2; fi
if [ "$MINUTES" -gt 160 ] 2>/dev/null; then echo "160분 넘는 판은 거절(틱 기록 칸)"; exit 2; fi
mkdir -p "$OUT"
OUT_W="$(cygpath -m "$OUT")"

DATA="C:/Users/USER/Desktop/MyGit/MO_Shooter/Client/Lyra/Saved/MOB"	# Epic 맵 값이라 리포 밖(regress.sh 와 같은 자리)
MAP="$DATA/map_expanse.mocl"
SRV="$BIN/Server/Release/Server.exe"
BOT="$BIN/BotClient/Release/BotClient.exe"
SECS=$((MINUTES * 60))
STEP=60
[ "$MINUTES" -le 10 ] && STEP=10	# SOAK 줄 간격 — 짧은 판은 메모리 흐름을 볼 줄 수가 모자라 촘촘히

# 서버 — 사람용 판과 같은 컨텐츠(받침 · 점프대 · 순간이동 · 클립). 경기는 짧게 잘라 방이 자주 닫히게 한다
srvArgs=("--map=$MAP" "--port=$PORT" "--duration=$((SECS + TAIL + 15))" "--gameThreads=2" "--maxSessions=$((BOTS * 2 + 32))"
	"--warmup=2" "--matchSeconds=$MATCH" "--seed=72" "--soakSeconds=$STEP"
	"--pads=$DATA/pads_expanse.txt" "--launchers=$DATA/launchers_expanse.txt" "--teleports=$DATA/teleports_expanse.txt" "--clips=$DATA/clips.bin")
[ -n "$BREAK" ] && srvArgs+=("--testSoakBreak=$BREAK")
# 봇 — 부하 판(--seconds)과 같은 행동(대시 · 수류탄 · 조준 · 근접 · 앉기 · 길찾기)
botArgs=("--map=$MAP" "--port=$PORT" "--bots=$BOTS" "--seconds=$SECS" "--rejoin" "--dash" "--grenade" "--ads" "--melee" "--crouch" "--nav=$DATA/nav_expanse.monv")

echo "SOAK 시작 $(date +%H:%M:%S) 분=$MINUTES 봇=${BOTS}x2 끊기=$CHURN 경기=$MATCH 틀림=${BREAK:-없음} 출력=$OUT_W"
"$SRV" "${srvArgs[@]}" > "$OUT/srv.log" 2>&1 &
spid=$!
sleep 1
"$BOT" "${botArgs[@]}" --seed=2000 > "$OUT/bot_stay.log" 2>&1 &
apid=$!
"$BOT" "${botArgs[@]}" --seed=3000 "--churn=$CHURN" > "$OUT/bot_churn.log" 2>&1 &
bpid=$!
wait $apid; arc=$?
wait $bpid; brc=$?
wait $spid; src=$?
echo "SOAK 끝 $(date +%H:%M:%S) 서버 rc=$src 남는쪽 rc=$arc 끊는쪽 rc=$brc"

FAILS=0
verdict() {	# 이름 참거짓 설명
	if [ "$2" = 1 ]; then echo "PASS $1 $3"; else echo "FAIL $1 $3"; FAILS=$((FAILS + 1)); fi
}
num() {	# 줄 이름 — 줄 속 「 이름=숫자」의 숫자(없으면 빈 값)
	echo "$1" | sed -nE "s/.* $2=(-?[0-9]+).*/\1/p"
}

S_START="$(grep -a '^SOAK start ' "$OUT/srv.log" | head -1)"
S_END="$(grep -a '^SOAK end ' "$OUT/srv.log" | tail -1)"
ticks="$(grep -a -c '^SOAK tick ' "$OUT/srv.log")"
A_LOAD="$(grep -a '^LOADBOT ' "$OUT/bot_stay.log" | tail -1)"
B_LOAD="$(grep -a '^LOADBOT ' "$OUT/bot_churn.log" | tail -1)"
A_RJ="$(grep -a '^BOT rejoin ' "$OUT/bot_stay.log" | tail -1)"
B_RJ="$(grep -a '^BOT rejoin ' "$OUT/bot_churn.log" | tail -1)"

# 1. 서버가 끝까지 돌았다
ok=1; [ "$src" = 0 ] && [ -n "$S_START" ] && [ -n "$S_END" ] && [ "$ticks" -gt 0 ] || ok=0
verdict 서버생존 $ok "rc=$src SOAK줄 tick=$ticks"

# 2. 다 나간 뒤 0 으로 돌아왔다 — 코어 접속 수 · 만든 = 없앤 · 라우터 접속 = 끊김 · 앉은 수 · 방 · 인사 마감 줄
ok=1
for k in coreSessions seated rooms helloWaiting; do [ "$(num "$S_END" $k)" = 0 ] || ok=0; done
[ "$(num "$S_END" coreCreated)" = "$(num "$S_END" coreDestroyed)" ] || ok=0
[ "$(num "$S_END" connected)" = "$(num "$S_END" disconnected)" ] || ok=0
verdict 다나간뒤0 $ok "$(echo "$S_END" | grep -oE ' (coreSessions|coreCreated|coreDestroyed|connected|disconnected|seated|rooms|helloWaiting)=[0-9-]+' | tr -d '\n')"

# 3. 오류 수 0 — 접속 거부 · 패킷 오류 · 송신 넘침 · 부분 송신 · 수신 넘침 · 나쁜 패킷
ok=1
for k in acceptFailed packetErrors sendOverflow partialSend recvOverflow badPackets; do [ "$(num "$S_END" $k)" = 0 ] || ok=0; done
verdict 오류0 $ok "$(echo "$S_END" | grep -oE ' (acceptFailed|timedOut|packetErrors|sendOverflow|partialSend|recvOverflow|badPackets|helloRejected|helloTimeouts)=[0-9-]+' | tr -d '\n')"

# 4. 봇 판정 그대로 — 부하 판 끝 줄(예측 ↔ 서버 탄창 · 깃발 · 인벤 0 · 다시 접속 실패 0)
ok=1
[ "$arc" = 0 ] && [ "$brc" = 0 ] || ok=0
grep -aq '^RESULT PASS' "$OUT/bot_stay.log" && grep -aq '^RESULT PASS' "$OUT/bot_churn.log" || ok=0
# 뒤 셋(crouchDiff · fallDiff · launchDiff)은 막기 켠 판이라 판정 밖 — 따로 센 몫(D82 · D100 · D174)을 같이 적는다
LOAD_KEYS='compared|ammoDiff|flagDiff|invDiff|pawnReconciles|predOdd|crouchDiff|fallDiff|launchDiff'
verdict 봇판정 $ok "남는쪽$(echo "$A_LOAD" | grep -oE " ($LOAD_KEYS)=[0-9]+" | tr -d '\n') | 끊는쪽$(echo "$B_LOAD" | grep -oE " ($LOAD_KEYS)=[0-9]+" | tr -d '\n')"

# 5. 실제로 반복했다 — 0 이면 못 돌린 것과 결함 없음을 못 가른다
ok=1
[ "$(num "$A_RJ" afterResult)" -gt 0 ] 2>/dev/null || ok=0		# 판 끝에 방이 닫혀 다시 접속
[ "$(num "$B_RJ" selfClosed)" -gt 0 ] 2>/dev/null || ok=0		# 스스로 끊고 다시 접속
[ "$(num "$A_RJ" rejoins)" -gt 0 ] 2>/dev/null && [ "$(num "$B_RJ" rejoins)" -gt 0 ] 2>/dev/null || ok=0
[ "$(num "$S_END" roomsClosed)" -gt 1 ] 2>/dev/null || ok=0
verdict 반복함 $ok "남는쪽$(echo "$A_RJ" | grep -oE ' (rejoins|serverClosed|afterResult|selfClosed|connectFails)=[0-9]+' | tr -d '\n') | 끊는쪽$(echo "$B_RJ" | grep -oE ' (rejoins|serverClosed|afterResult|selfClosed|connectFails)=[0-9]+' | tr -d '\n') | 방 만듦=$(num "$S_END" roomsCreated) 닫힘=$(num "$S_END" roomsClosed) 인사=$(num "$S_END" hellos)"

# 6. 메모리 — 시작(접속 전) ↔ 끝(다 나간 뒤)의 힙 사용량(heapKB = 지금 잡힌 양, 풀면 돌아온다 — 11-24 시험 프로그램: 100B × 20만 잡으면 79→21,234KB · 풀면 83KB).
#    늘어나도 되는 몫 둘을 뺀다 — 닫힌 방 요약(summaryBytes) · 패킷 버퍼 풀이 늘린 청크(bufChunks × bufChunkKB. 풀은 받은 청크를 끝까지 쥐고 다시 쓴다).
#    개인 메모리(privateKB)는 판정에 안 쓴다 — 힙이 늘린 칸을 돌려주지 않아 계단으로 뛴다(11-23 결함 없는 8분 판 넷 중 하나가 t=350 에 +8MB, 그 전 판정이 FAIL). 참고로 적는다
#    버퍼 풀 자체가 새는지는 「쥔 청크」(끝 bufChunks − bufChunksIdle — 스레드마다 지금 쓰는 청크 하나씩)로 적는다. 판정은 긴 판 흐름을 본 뒤
hS="$(num "$S_START" heapKB)"; hE="$(num "$S_END" heapKB)"
cS="$(num "$S_START" bufChunks)"; cE="$(num "$S_END" bufChunks)"; cI="$(num "$S_END" bufChunksIdle)"; cK="$(num "$S_END" bufChunkKB)"
if [ -z "$hS" ] || [ -z "$hE" ] || [ -z "$cS" ] || [ -z "$cE" ] || [ -z "$cI" ] || [ -z "$cK" ] || [ "$hS" -lt 0 ] || [ "$hE" -lt 0 ] || [ "$cS" -lt 0 ] || [ "$cE" -lt 0 ]; then
	verdict 메모리 0 "힙 · 버퍼 풀 값을 못 읽음 heapKB=$hS→$hE bufChunks=$cS→$cE bufChunksIdle=$cI bufChunkKB=$cK"
else
	sumKB=$(( ($(num "$S_END" summaryBytes) - $(num "$S_START" summaryBytes)) / 1024 ))
	poolKB=$(( (cE - cS) * cK ))
	residual=$(( hE - hS - sumKB - poolKB ))
	cPeak="$(grep -a '^SOAK ' "$OUT/srv.log" | grep -oE ' bufChunks=[0-9]+' | cut -d= -f2 | sort -n | tail -1)"
	desc="힙 $hS→${hE}KB(+$((hE - hS))) 요약몫 +${sumKB}KB 버퍼풀 청크 $cS→$cE(최고 $cPeak · 쥔 $((cE - cI)) · 청크당 ${cK}KB) +${poolKB}KB 남은증가=${residual}KB 인사=$(num "$S_END" hellos) 틀림몫=$(num "$S_END" leakKB)KB 개인메모리 $(num "$S_START" privateKB)→$(num "$S_END" privateKB)KB"
	if [ "$INFO_ONLY" = 0 ]; then
		ok=1; [ "$residual" -le "$MEM_LIMIT" ] || ok=0
		verdict 메모리 $ok "$desc 문턱=${MEM_LIMIT}KB"
	else
		echo "INFO 메모리 $desc (--infoOnly — 판정 안 함)"
	fi
fi

# 7. 핸들 — 다 나간 뒤 ↔ 시작(접속 전)
hs="$(num "$S_START" handles)"; he="$(num "$S_END" handles)"
hdesc="시작=$hs 끝=$he 차=$((he - hs)) 봇도는중최대=$(grep -a '^SOAK tick ' "$OUT/srv.log" | grep -oE ' handles=[0-9]+' | cut -d= -f2 | sort -n | tail -1)"
if [ "$INFO_ONLY" = 0 ]; then
	ok=1; [ $((he - hs)) -le "$HANDLE_LIMIT" ] || ok=0
	verdict 핸들 $ok "$hdesc 문턱=$HANDLE_LIMIT"
else
	echo "INFO 핸들 $hdesc (--infoOnly — 판정 안 함)"
fi

# 적기만 — 서버 빈 입력 이동과 그 바로 뒤 대시(「예측만 대시」 가르기, soak-progress 「긴 판」 갈래 ②). 사건 줄은 srv.log DASHAFTERIDLE
echo "INFO 빈입력$(grep -a '^SERVER inputs ' "$OUT/srv.log" | tr -d '\r' | grep -oE ' (starvedTicks|idleMoves|idleMovesPostGame|dashAfterIdle|dashAfterIdleNoDirection)=[0-9]+' | tr -d '\n') 사건줄=$(grep -a -c '^DASHAFTERIDLE ' "$OUT/srv.log")"

if [ $FAILS = 0 ]; then echo "SOAK RESULT PASS"; else echo "SOAK RESULT FAIL ($FAILS)"; fi
[ $FAILS = 0 ]
