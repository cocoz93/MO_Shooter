#!/bin/bash
# 윈도 Git Bash 에서 실행. WSL 의 TSan 서버에 윈도 봇·EdgeProbe 를 붙여 경우별로 돌리고 보고 수를 센다.
#   Tools/Regress/tsan/suite.sh [--only "<이름들>"]
export MSYS_NO_PATHCONV=1
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
ONLY=""; [ "$1" = --only ] && ONLY="$2"
RUN=/mnt/c/Users/USER/Desktop/MyGit/MO_Shooter/Tools/Regress/tsan
MAPL="/mnt/c/Users/USER/Documents/Unreal Projects/LyraStarterGame/Saved/MOB/map_expanse.mocl"
MAPW="C:/Users/USER/Documents/Unreal Projects/LyraStarterGame/Saved/MOB/map_expanse.mocl"
BOT="$REPO/build/BotClient/Release/BotClient.exe"
EDGE="$REPO/build/Tools/EdgeProbe/Release/EdgeProbe.exe"
OUT="$REPO/build/regress/tsan"; mkdir -p "$OUT"
want() { [ -z "$ONLY" ] || [[ " $ONLY " == *" $1 "* ]]; }
W() { wsl -d Ubuntu-24.04 -u root -- "$@"; }

start_server() { # 태그 포트 인자...
	local tag=$1 port=$2; shift 2
	W bash $RUN/run_server.sh /root/$tag.log "--map=$MAPL" --port=$port "$@" > "$OUT/${tag}_rc.txt" 2>&1 &
	SPID=$!
	for i in $(seq 1 30); do sleep 1; W ss -ltn 2>/dev/null | grep -q ":$port " && return 0; done
	echo "$tag 서버가 안 뜸"; return 1
}
finish() { # 태그
	wait $SPID
	echo "$1: $(cat "$OUT/$1_rc.txt")"
	W bash $RUN/report.sh /root/$1.log 2>&1 | cut -c1-175
}
bots() { # 태그 포트 봇수 시드 서버 인자...
	local tag=$1 port=$2 n=$3 seed=$4; shift 4
	start_server $tag $port --exitWhenEmpty --seed=$seed "$@" || return
	timeout 120 "$BOT" "--map=$MAPW" --port=$port --bots=$n --seed=$seed 2>&1 | grep -E '^RESULT|BOT 서버' | tail -2 | cut -c1-110
	finish $tag
}
edge() { # 태그 포트 "서버 인자"
	local tag=$1 port=$2; shift 2
	start_server t_$tag $port "$@" || return
	timeout 150 "$EDGE" --port=$port --case=$tag 2>&1 | grep -E '^EDGE 실패 [^0]|^RESULT' | cut -c1-120
	finish t_$tag
}
want bots40 && bots t_bots40 7900 40 71 --warmup=1 --matchSeconds=20 --gameThreads=4
want leave_team && edge leave_team 7901 --duration=30 --warmup=1 --matchSeconds=100
want rejoin_all && edge rejoin_all 7902 --exitWhenEmpty --warmup=1 --matchSeconds=100
want reconnect_fast && edge reconnect_fast 7903 --exitWhenEmpty --warmup=1 --matchSeconds=100
want overflow && edge overflow 7904 --exitWhenEmpty --warmup=1 --matchSeconds=100 --gameThreads=2
want postgame && edge postgame 7905 --duration=14 --warmup=1 --matchSeconds=3 --gameThreads=2
want bad_packets && edge bad_packets 7906 --duration=40 --warmup=1 --matchSeconds=100
want values && edge values 7907 --exitWhenEmpty --warmup=0 --matchSeconds=100
want flood_10 && edge flood_10 7908 --exitWhenEmpty --warmup=0 --matchSeconds=100
