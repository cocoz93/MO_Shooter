#!/bin/bash
# ① 지금 코어(IOCP) ② 사본 RIO 켬 ③ 사본에 일부러 틀림(자리 재사용 때 지정을 안 풀고, 지정 함수가 저장도 안 함) — 원본은 건드리지 않는다
H="$(cd "$(dirname "$0")" && pwd)"
W="$H/../../../build/coresmoke"; mkdir -p "$W"
CORE=/c/Users/USER/Desktop/MyGit/ServerCore

build_run() {
	local name=$1 core=$2 port=$3
	cmake -G "Visual Studio 17 2022" -A x64 -S "$H" -B "$W/b_$name" -DCORE_DIR="$core" > "$W/b_$name.cfg.log" 2>&1 || { echo "$name 구성 실패"; tail -5 "$W/b_$name.cfg.log"; return; }
	cmake --build "$W/b_$name" --config Release --target CoreSmoke > "$W/b_$name.log" 2>&1 || { echo "$name 빌드 실패"; grep -E 'error' "$W/b_$name.log" | head -8 | cut -c1-200; return; }
	echo "=== $name"
	"$W/b_$name/Release/CoreSmoke.exe" "$port" 2>&1 | grep -E '입출력|틀림|RESULT'
}

build_run iocp "C:/Users/USER/Desktop/MyGit/ServerCore" 7971

rm -rf "$W/sc_rio" "$W/sc_mut"
mkdir -p "$W/sc_rio" "$W/sc_mut"
(cd "$CORE" && git ls-files -z | xargs -0 -I{} cp --parents {} "$W/sc_rio/")
cp -r "$W/sc_rio/." "$W/sc_mut/"
python - "$H" <<'PY'
import sys
h = sys.argv[1]
p = h + '/sc_rio/Base/CoreConfig.h'
t = open(p, 'rb').read(); a = b'#define USE_RIO_TRANSPORT 0'; assert t.count(a) == 1
open(p, 'wb').write(t.replace(a, b'#define USE_RIO_TRANSPORT 1'))
p = h + '/sc_mut/Network/IOCPServer.cpp'
t = open(p, 'rb').read(); a = b'_recvEventQueue.store(nullptr, std::memory_order_release);'; assert t.count(a) == 1
open(p, 'wb').write(t.replace(a, b''))
PY
build_run rio "$(cygpath -m "$W/sc_rio")" 7972
build_run mut "$(cygpath -m "$W/sc_mut")" 7973
echo VARIANTS_DONE
