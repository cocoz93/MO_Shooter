#!/bin/bash
# 다리 플러그인과 Lyra 원본 수정분의 사본을 이 리포 Client/ 로 뜬다. Lyra 폴더는 깃이 아니라서 거기만 두면 유일본이다.
#   Tools/LyraPilot/sync_client.sh
# 받는 쪽 — Client/MOBridge = Plugins/MOBridge 의 소스, Client/lyra_changes.diff = 백업(수정 전 원본) 대비 Source/ 차이
# 비공개 전제: diff 에 Lyra 원본 줄이 들어간다. 리포를 공개하기 전에 언리얼 라이선스 허용 범위를 먼저 볼 것

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
LYRA="/c/Users/USER/Documents/Unreal Projects/LyraStarterGame"
BASE="/c/Users/USER/Documents/Unreal Projects/_backup/LyraStarterGame-src-20261025-2300"
OUT="$REPO/Client"

mkdir -p "$OUT/MOBridge"
cp "$LYRA/Plugins/MOBridge/MOBridge.uplugin" "$OUT/MOBridge/"
(cd "$LYRA/Plugins/MOBridge" && find Source -type f -not -name '*.bak-*') | while IFS= read -r rel; do
	mkdir -p "$OUT/MOBridge/$(dirname "$rel")"
	cp -p "$LYRA/Plugins/MOBridge/$rel" "$OUT/MOBridge/$rel"
done

: > "$OUT/lyra_changes.diff"
(cd "$BASE/Source" && find . -type f) | sed 's#^\./##' | while IFS= read -r rel; do
	if ! cmp -s "$BASE/Source/$rel" "$LYRA/Source/$rel"; then
		git diff --no-index --ignore-cr-at-eol --src-prefix=a/Source/ --dst-prefix=b/Source/ -- "$BASE/Source/$rel" "$LYRA/Source/$rel" >> "$OUT/lyra_changes.diff"
	fi
done

# 경로를 Lyra 폴더 기준으로 — 이 파일만 봐도 어느 파일인지 알고, 백업 폴더에서 git apply --ignore-whitespace 로 재현된다
sed -i -e 's#a/Source/C:/Users/USER/Documents/Unreal Projects/_backup/LyraStarterGame-src-20261025-2300/#a/#g' -e 's#b/Source/C:/Users/USER/Documents/Unreal Projects/LyraStarterGame/#b/#g' "$OUT/lyra_changes.diff"

src=$(cd "$LYRA/Plugins/MOBridge" && find Source -type f -not -name '*.bak-*' | wc -l)
dst=$(cd "$OUT/MOBridge" && find Source -type f | wc -l)
bad=0
while IFS= read -r rel; do
	cmp -s "$LYRA/Plugins/MOBridge/$rel" "$OUT/MOBridge/$rel" || bad=$((bad + 1))
done < <(cd "$LYRA/Plugins/MOBridge" && find Source -type f -not -name '*.bak-*')
echo "다리 파일 원본 $src · 사본 $dst · 내용 다름 $bad"
echo "Lyra 수정 파일 $(grep -c '^diff --git' "$OUT/lyra_changes.diff") · diff $(wc -l < "$OUT/lyra_changes.diff")줄"
