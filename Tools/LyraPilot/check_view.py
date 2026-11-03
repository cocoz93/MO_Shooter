#!/usr/bin/env python3
# 되감기 화면 대조 — Lyra 의 MOB_VIEW(쏠 때 화면 자리)와 서버 REWIND(되돌린 자리)를 (seq, 상대) 로 잇는다
#   python check_view.py <lyra.log> <server.txt> [기준cm=1.0]
# 기준 1cm 는 제안값이다. 몸통 지름이 80cm 쯤이라 1cm 면 판정 결과를 거의 안 바꾸고, 그 넘게 벌어지면 시계·프레임 어긋남을 의심할 크기다.
# rc=0 기준 안 · rc=1 기준 밖이거나 이은 줄이 0 (0 은 「맞다」가 아니라 「못 읽었다」)
import re
import sys

lyra_path, srv_path = sys.argv[1], sys.argv[2]
limit = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0

me = None
views = {}
view_lines = 0
for line in open(lyra_path, encoding='utf-8', errors='replace'):
    if me is None:
        m = re.search(r'MOB_WELCOME player=(\d+)', line)
        if m:
            me = int(m.group(1))
    m = re.search(r'MOB_VIEW seq=(\d+) target=(\d+) viewTick=(\d+) alpha=([\d.]+) x=(-?[\d.]+) y=(-?[\d.]+) z=(-?[\d.]+) yaw=(-?[\d.]+)', line)
    if m:
        view_lines += 1
        views[(int(m.group(1)), int(m.group(2)))] = tuple(float(g) for g in m.groups()[2:])

rewinds = 0
compared = 0
missing = 0
mismatch_view = 0
diffs = []
yaws = []
last_at = {}
moved = 0
for line in open(srv_path, encoding='utf-8', errors='replace'):
    m = re.match(r'REWIND seq=(\d+) shooter=(\d+) target=(\d+) now=(\d+) viewTick=(\d+) alpha=([\d.]+) x=(-?[\d.]+) y=(-?[\d.]+) z=(-?[\d.]+) yaw=(-?[\d.]+)', line)
    if not m or int(m.group(2)) != me:
        continue
    rewinds += 1
    v = views.get((int(m.group(1)), int(m.group(3))))
    if v is None:
        missing += 1
        continue
    compared += 1
    if int(v[0]) != int(m.group(5)):
        mismatch_view += 1
    x, y, z, yaw = (float(m.group(i)) for i in (7, 8, 9, 10))
    # 움직이는 상대가 표본에 들었나 — 전부 서 있으면 대조가 시점 어긋남을 못 본다
    prev = last_at.get(int(m.group(3)))
    if prev is not None and ((prev[0] - x) ** 2 + (prev[1] - y) ** 2) ** 0.5 > 1.0:
        moved += 1
    last_at[int(m.group(3))] = (x, y)
    diffs.append((((v[2] - x) ** 2 + (v[3] - y) ** 2 + (v[4] - z) ** 2) ** 0.5, int(m.group(1)), int(m.group(3)), int(m.group(4)) - int(m.group(5))))
    yaws.append(abs((v[5] - yaw + 180.0) % 360.0 - 180.0))

print(f'me={me} viewLines={view_lines} serverRewinds={rewinds} compared={compared} moved={moved} missing={missing} viewTickMismatch={mismatch_view}')
if not diffs:
    print('VIEW FAIL 이은 줄 0')
    sys.exit(1)
diffs_sorted = sorted(d[0] for d in diffs)
worst = max(diffs)
over = sum(1 for d in diffs if d[0] > limit)
print(f'viewPosDiffMax={worst[0]:.3f} p50={diffs_sorted[len(diffs_sorted) // 2]:.3f} p99={diffs_sorted[int(len(diffs_sorted) * 0.99)]:.3f} over{limit:g}cm={over} worstSeq={worst[1]} worstTarget={worst[2]} worstAgeTicks={worst[3]} viewYawDiffMax={max(yaws):.3f}')
print('VIEW PASS' if worst[0] <= limit else 'VIEW FAIL')
sys.exit(0 if worst[0] <= limit else 1)
