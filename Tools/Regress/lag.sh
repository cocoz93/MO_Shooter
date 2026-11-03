#!/bin/bash
# 지연을 넣은 봇 10명 한 판씩 — 판이 깨지는지만 본다(rc·위치 대조). 시간 지표로 판정하지 않는다.
#   Tools/Regress/lag.sh [--bin <빌드 폴더>] [--out <출력 폴더>]
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BIN="$REPO/build"; OUT=""
while [ $# -gt 0 ]; do
	case "$1" in
		--bin) BIN="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		*) echo "모르는 인자 $1"; exit 2 ;;
	esac
done
[ -n "$OUT" ] || OUT="$BIN/regress/lag_$(date +%H%M%S)"
mkdir -p "$OUT"
OUT_W="$(cygpath -m "$OUT")"
MAP="C:/Users/USER/Documents/Unreal Projects/LyraStarterGame/Saved/MOB/map_expanse.mocl"
SRV="$BIN/Server/Release/Server.exe"; BOT="$BIN/BotClient/Release/BotClient.exe"; RELAY="$BIN/Tools/LagRelay/Release/LagRelay.exe"
FAILS=0; PORT=7810
for d in 0 50 100 150; do
	j=$((d / 3)); PORT=$((PORT + 2)); name=d$d
	"$SRV" "--map=$MAP" --port=$PORT --exitWhenEmpty --warmup=2 --matchSeconds=25 --seed=9$d "--record=$OUT_W/${name}_srv.csv" > "$OUT/${name}_srv.log" 2>&1 &
	spid=$!
	sleep 1
	"$RELAY" --listen=$((PORT + 1)) --target=$PORT --delayMs=$d --jitterMs=$j > "$OUT/${name}_relay.log" 2>&1 &
	rpid=$!
	sleep 0.5
	timeout 120 "$BOT" "--map=$MAP" --port=$((PORT + 1)) --bots=10 --seed=9$d "--record=$OUT_W/${name}_bot.csv" > "$OUT/${name}_bot.log" 2>&1
	brc=$?
	wait $spid; src=$?
	wait $rpid; rrc=$?
	ok=1
	[ $brc = 0 ] && [ $src = 0 ] && [ $rrc = 0 ] || ok=0
	cmp -s "$OUT/${name}_srv.csv" "$OUT/${name}_bot.csv" || ok=0
	[ $ok = 1 ] || FAILS=$((FAILS + 1))
	echo "$([ $ok = 1 ] && echo PASS || echo FAIL) 지연 ${d}ms 흔들림 ${j}ms 봇 rc=$brc 서버 rc=$src 중계 rc=$rrc $(grep -E 'BOT 서버' "$OUT/${name}_bot.log" | tail -1 | cut -c1-100)"
	cut -c1-110 "$OUT/${name}_relay.log" | sed 's/^/    /'
	grep -E '^REJECT' "$OUT/${name}_srv.log" | cut -c1-150 | sed 's/^/    /'
done
if [ $FAILS = 0 ]; then echo "LAG PASS"; else echo "LAG FAIL $FAILS"; exit 1; fi
