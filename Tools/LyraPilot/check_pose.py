#!/usr/bin/env python3
# 자세 대조 — 판정 모양 기록 둘을 (조각, 프레임, 부위)로 이어 부위 중심과 축 방향이 얼마나 다른지 낸다
#   python check_pose.py <기준.csv> <비교.csv> [--refTag=POSE] [--cmpTag=POSE] [--rows=animcmp_<Tag>.csv] [--limitCm=0.1] [--limitDeg=0.3]
# 기준 = 언리얼이 남긴 모양(animcmp_<Tag>_shapes.csv). 비교 = 언리얼 밖에서 계산한 모양(같은 줄 형식)
#   POSE  = 물리 몸체 자리로 남긴 줄,  POSEB = 뼈 자리로 남긴 줄(화면에 그려진 몸)
# 줄 형식 = <태그>,조각,프레임,부위,종류,약점,중심 xyz,X축 xyz,Y축 xyz,Z축 xyz,크기 a,b,c  (MOBAnimCompareSubsystem::WriteShapes)
#
# 기준값 근거 (candidate1-pilot-plan.md)
#  - 0.1cm = 전 부위를 0.87cm 옮기면 머리 오탈 3.0발(계산값)이라 0.1cm 는 약 0.3발. Lyra 원본도 같은 경로를 두 번 찍으면 머리가 최대 0.10cm 다르다(실측)
#  - 0.3도 = 가장 긴 부위(종아리, 가운데에서 끝까지 약 21cm)의 끝이 0.1cm 움직이는 각도
# 축 방향은 판정에 쓰이는 축만 본다 — 캡슐은 길이 축(Z) 하나, 상자는 세 축, 공은 보지 않는다
# rc=0 기준 안 · rc=1 기준 밖이거나 이은 줄이 0 (0 은 「맞다」가 아니라 「못 읽었다」)
import csv
import math
import sys

sys.stdout.reconfigure(encoding='utf-8')
args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
ref_path, cmp_path = args[0], args[1]
ref_tag = opt.get('refTag', 'POSE')
cmp_tag = opt.get('cmpTag', 'POSE')
limit_cm = float(opt.get('limitCm', 0.1))
limit_deg = float(opt.get('limitDeg', 0.3))


def load(path, tag):
    out = {}
    count = {}
    lines = 0
    head = tag + ','
    for line in open(path, encoding='utf-8', errors='replace'):
        if not line.startswith(head):
            continue
        lines += 1
        c = line.strip().split(',')
        base = (int(c[1]), int(c[2]), c[3])
        n = count.get(base, 0)      # 한 부위에 모양이 여럿이면 나온 차례로 잇는다
        count[base] = n + 1
        out[base + (n,)] = (c[4], [float(x) for x in c[6:21]])
    return out, lines


def angle(a, b):
    # acos 는 1 근처에서 자릿수를 잃는다(소수 5자리로 적힌 축은 길이가 정확히 1이 아니라 같은 축끼리도 0.3도가 나온다) — 외적 길이와 내적으로 구한다
    cx, cy, cz = a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]
    return math.degrees(math.atan2(math.sqrt(cx * cx + cy * cy + cz * cz), a[0] * b[0] + a[1] * b[1] + a[2] * b[2]))


ref, ref_lines = load(ref_path, ref_tag)
cmp_, cmp_lines = load(cmp_path, cmp_tag)

seg_names = {}
if 'rows' in opt:
    order = {}
    for r in csv.DictReader(open(opt['rows'], newline='', encoding='utf-8-sig')):
        order.setdefault(r['segment'], len(order))
    seg_names = {i: n for n, i in order.items()}

compared = 0
missing = 0
size_mismatch = 0
worst_c = (-1.0, None)
worst_a = (-1.0, None)
centers = []
by_seg = {}
by_bone = {}
for key, (kind, v) in ref.items():
    other = cmp_.get(key)
    if other is None:
        missing += 1
        continue
    compared += 1
    w = other[1]
    if other[0] != kind or any(abs(v[i] - w[i]) > 0.001 for i in (12, 13, 14)):
        size_mismatch += 1
    dc = math.sqrt(sum((v[i] - w[i]) ** 2 for i in range(3)))
    if kind == 'box':
        da = max(angle(v[3:6], w[3:6]), angle(v[6:9], w[6:9]), angle(v[9:12], w[9:12]))
    elif kind == 'sphyl':
        da = angle(v[9:12], w[9:12])
    else:
        da = 0.0
    centers.append(dc)
    if dc > worst_c[0]:
        worst_c = (dc, key)
    if da > worst_a[0]:
        worst_a = (da, key)
    seg = key[0] % 100
    s = by_seg.setdefault(seg, [0, 0.0, 0.0, 0.0])
    s[0] += 1
    s[1] = max(s[1], dc)
    s[2] = max(s[2], da)
    s[3] += dc
    b = by_bone.setdefault(key[2], [0, 0.0, 0.0])
    b[0] += 1
    b[1] = max(b[1], dc)
    b[2] = max(b[2], da)
extra = sum(1 for k in cmp_ if k not in ref)

print(f'POSECHECK refLines={ref_lines} cmpLines={cmp_lines} compared={compared} missing={missing} extra={extra} sizeMismatch={size_mismatch}')
if compared == 0:
    print('POSE FAIL 이은 줄 0')
    sys.exit(1)
centers.sort()
print(f'centerDiffMax={worst_c[0]:.4f} p99={centers[int(len(centers) * 0.99)]:.4f} p50={centers[len(centers) // 2]:.4f} mean={sum(centers) / len(centers):.4f} worst={worst_c[1]}')
print(f'axisDiffMax={worst_a[0]:.4f} worst={worst_a[1]}')
for seg in sorted(by_seg):
    n, c, a, total = by_seg[seg]
    print(f'  seg {seg:2d} {seg_names.get(seg, ""):7s} n={n:6d} centerMax={c:.4f} centerMean={total / n:.4f} axisMax={a:.4f}')
top = sorted(by_bone.items(), key=lambda kv: -kv[1][1])[:5]
print('  부위별 큰 순: ' + ' · '.join(f'{k} {v[1]:.4f}cm/{v[2]:.3f}도' for k, v in top))
ok = missing == 0 and size_mismatch == 0 and worst_c[0] <= limit_cm and worst_a[0] <= limit_deg
print('POSE PASS' if ok else f'POSE FAIL (기준 {limit_cm:g}cm · {limit_deg:g}도)')
sys.exit(0 if ok else 1)
