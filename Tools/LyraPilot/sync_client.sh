#!/bin/bash
# Lyra 원본 수정분을 이 리포 Client/lyra_changes.diff 로 뜬다. Client/Lyra 는 깃이 무시하므로 거기만 두면 유일본이다.
#   Tools/LyraPilot/sync_client.sh
# 다리 플러그인은 Client/Lyra/Plugins/MOBridge 자리에서 바로 추적하므로 복사하지 않는다. diff = 백업(수정 전 원본) 대비 Source/ 차이
# 비공개 전제: diff 에 Lyra 원본 줄이 들어간다. 리포를 공개하기 전에 언리얼 라이선스 허용 범위를 먼저 볼 것

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
LYRA="/c/Users/USER/Desktop/MyGit/MO_Shooter/Client/Lyra"
BASE="/c/Users/USER/Documents/Unreal Projects/_backup/LyraStarterGame-src-20261025-2300"
OUT="$REPO/Client"

: > "$OUT/lyra_changes.diff"
(cd "$BASE/Source" && find . -type f) | sed 's#^\./##' | while IFS= read -r rel; do
	if ! cmp -s "$BASE/Source/$rel" "$LYRA/Source/$rel"; then
		git diff --no-index --ignore-cr-at-eol --src-prefix=a/Source/ --dst-prefix=b/Source/ -- "$BASE/Source/$rel" "$LYRA/Source/$rel" >> "$OUT/lyra_changes.diff"
	fi
done

# 경로를 Lyra 폴더 기준으로 — 이 파일만 봐도 어느 파일인지 알고, 백업 폴더에서 git apply --ignore-whitespace 로 재현된다
sed -i -e 's#a/Source/C:/Users/USER/Documents/Unreal Projects/_backup/LyraStarterGame-src-20261025-2300/#a/#g' -e 's#b/Source/C:/Users/USER/Desktop/MyGit/MO_Shooter/Client/Lyra/#b/#g' "$OUT/lyra_changes.diff"

echo "Lyra 수정 파일 $(grep -c '^diff --git' "$OUT/lyra_changes.diff") · diff $(wc -l < "$OUT/lyra_changes.diff")줄"
