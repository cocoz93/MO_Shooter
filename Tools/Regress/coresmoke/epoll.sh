#!/bin/bash
# WSL 에서 epoll 빌드·실행. 산출물은 WSL 쪽 임시 폴더에 둔다.
H=/mnt/c/Users/USER/Desktop/MyGit/MO_Shooter/Tools/Regress/coresmoke
which cmake g++ || { echo "도구 없음"; exit 3; }
B=/root/coresmoke_epoll
cmake -S "$H" -B "$B" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-mcx16 -DCORE_DIR=/mnt/c/Users/USER/Desktop/MyGit/ServerCore -DLOCKFREE_DIR=/mnt/c/Users/USER/Desktop/MyGit/LockFree/LockFree_Test > /root/coresmoke_cfg.log 2>&1 || { echo 구성 실패; tail -8 /root/coresmoke_cfg.log; exit 1; }
cmake --build "$B" --target CoreSmoke -j8 > /root/coresmoke_build.log 2>&1 || { echo 빌드 실패; grep -E 'error' /root/coresmoke_build.log | head -10 | cut -c1-220; exit 1; }
echo "=== epoll"
"$B/CoreSmoke" 7974 2>&1 | grep -E '입출력|틀림|RESULT'
echo EPOLL_DONE
