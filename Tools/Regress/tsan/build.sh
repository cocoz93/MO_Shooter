#!/bin/bash
# WSL: build Server with ThreadSanitizer. Output stays in /root on the WSL side (/tmp empties when the WSL VM restarts).
R=/mnt/c/Users/USER/Desktop/MyGit
B=/root/mo_tsan
cmake -S "$R/MO_Shooter" -B "$B" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DMO_SERVERCORE_DIR="$R/ServerCore" -DLOCKFREE_DIR="$R/MO_Shooter/LockFree/LockFree_Test" \
	"-DCMAKE_CXX_FLAGS=-fsanitize=thread -g -mcx16 -I$R/MO_Shooter/Tools/Regress/tsan/inc -include $R/MO_Shooter/Tools/Regress/tsan/win_shim.h" > /root/mo_tsan_cfg.log 2>&1 || { echo 설정 실패; tail -15 /root/mo_tsan_cfg.log | cut -c1-200; exit 1; }
cmake --build "$B" --target Server -j6 > /root/mo_tsan_build.log 2>&1
rc=$?
echo "build rc=$rc"
grep -E 'error|Error ' /root/mo_tsan_build.log | cut -c1-220 | sort -u | head -25
ls -la "$B/Server/Server" 2>/dev/null
