#!/bin/bash
# WSL: run the TSan Server. Usage: run_server.sh <log file under /root> <server args...>
LOG=$1; shift
export TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 log_path=$LOG.tsan suppressions=/mnt/c/Users/USER/Desktop/MyGit/MO_Shooter/Tools/Regress/tsan/known.supp print_suppressions=0"
rm -f "$LOG" "$LOG.tsan".*
setarch "$(uname -m)" -R /root/mo_tsan/Server/Server "$@" > "$LOG" 2>&1
echo "server rc=$?"
