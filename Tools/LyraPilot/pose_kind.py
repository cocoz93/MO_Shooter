# 애니 대조 기록 하나: AnimCheck state 출력(POSE 줄)과 Lyra 몸 부위(animcmp_<태그>_shapes.csv POSEB 줄)의 22부위 차를 그 줄 몽타주 종류(이름 끝)로 나눈다.
#   처음 = G-4-b 쏜 뒤 덮기 자리 시험(C:\tmp\mob\g4\b). 회귀 애니 단계(Tools/Regress/regress.sh run_checks, L-11-a)가 이 줄을 기준선에 넣는다
#   pose_kind.py <태그> <AnimCheck 출력 csv> [<기록 폴더>]
#     → 전체 rows · mean · max 와 덮기 비중(HipFireUpperBodyOverrideWeight) > 0 부위만 따로, 그리고 종류별 rows · mean · max(3줄 이상인 종류만)
import csv, sys, math, re
from collections import defaultdict
sys.stdout.reconfigure(encoding='utf-8')
tag, cmpf = sys.argv[1:3]
D = sys.argv[3] if len(sys.argv) > 3 else 'C:/Users/USER/Desktop/MyGit/MO_Shooter/Client/Lyra/Saved/MOB'
kind = {}; ovr = {}
with open(f'{D}/animcmp_{tag}_probe.csv', newline='', encoding='utf-8', errors='replace') as f:
    r = csv.reader(f); h = next(r); ix = {k: i for i, k in enumerate(h)}
    oc = next((i for k, i in ix.items() if k.endswith('HipFireUpperBodyOverrideWeight')), None)
    for row in r:
        key = (int(row[ix['pass']]), int(row[ix['frame']]))
        names = sorted({re.sub(r'^AM_MM_(Rifle|Pistol|Shotgun)_', '', e.split('#')[0]) for e in row[ix['montages']].split(';') if e.strip()})
        kind[key] = '+'.join(names) or 'none'
        ovr[key] = oc is not None and float(row[oc] or 0) > 0.001
def load(path, t):
    d = {}
    for row in csv.reader(open(path, encoding='utf-8-sig')):
        if row[0] != t: continue
        d[(int(row[1]), int(row[2]), row[3])] = tuple(map(float, row[6:9]))
    return d
R = load(f'{D}/animcmp_{tag}_shapes.csv', 'POSEB'); C = load(cmpf, 'POSE')
acc = defaultdict(list); rows = defaultdict(set); allv = []; ov = []
for k, v in R.items():
    if k not in C: continue
    key = (k[0] // 100, k[1]); d = math.dist(v, C[k])
    kd = kind.get(key, '?'); acc[kd].append(d); rows[kd].add((k[0], k[1])); allv.append(d)
    if ovr.get(key): ov.append(d)
print(f'ALL rows={len(allv)//22} mean={sum(allv)/max(len(allv),1):.3f} max={max(allv) if allv else 0:.1f} | OVR>0 parts={len(ov)} mean={(sum(ov)/len(ov)) if ov else 0:.3f} max={max(ov) if ov else 0:.1f}')
for kd in sorted(acc, key=lambda x: (-len(acc[x]), x)):
    v = acc[kd]
    if len(rows[kd]) < 3: continue
    print(f'  {kd[:40]:40s} rows={len(rows[kd]):5d} mean={sum(v)/len(v):.3f} max={max(v):.1f}')
