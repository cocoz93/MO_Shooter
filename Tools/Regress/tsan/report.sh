#!/bin/bash
# WSL: summarize TSan reports for <log file under /root>. The zero line always has the amount that passed beside it.
LOG=$1
grep -E '^(ROUTER|SERVER connected|THREAD)' "$LOG" | cut -c1-150
n=$(cat "$LOG".tsan.* 2>/dev/null | grep -c "WARNING: ThreadSanitizer")
echo "tsan reports=$n"
cat "$LOG".tsan.* 2>/dev/null | grep -E '^SUMMARY' | sed -E 's#/mnt/c/Users/USER/Desktop/MyGit/##g' | cut -c1-170 | sort | uniq -c | sort -rn | head -30
