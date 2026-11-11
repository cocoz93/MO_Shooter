#!/usr/bin/env python3
# 탄 방향 대조 (candidate1-impl-plan.md 6단계, D15) — 서버 SPREAD 줄 <-> 화면 쪽 로그를 (입력 번호, 탄 번호) 로 잇는다.
#   python check_spread.py <lyra.log> <server.txt> [--deg=0.001]
# 서버는 --rewindLog, Lyra 는 -MOBViewLog 로 돌린 판이어야 한다.
# - 예측(MOB_PREDSHOT) = 클라가 입력을 돌려 쏜다고 본 발. 입력 번호 · 퍼짐 · 조준이 서버와 같으면 방향도 같아야 한다
# - 능력(MOB_SHOTDIR) = Lyra 능력이 실제로 쏜 탄. 화면에 그려지는 쪽
import math
import re
import sys

sys.stdout.reconfigure(encoding='utf-8')
args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict((a[2:].split('=', 1) + [''])[:2] for a in sys.argv[1:] if a.startswith('--'))
LIMIT = float(opt.get('deg', 0.001))

ROW = r'seq=(\d+) i=(\d+) frame=(\d+) spread=([-\d.]+) mult=([-\d.]+) aim=([-\d.]+),([-\d.]+),([-\d.]+) dir=([-\d.]+),([-\d.]+),([-\d.]+)'
pred_re = re.compile('MOB_PREDSHOT ' + ROW)
shot_re = re.compile('MOB_SHOTDIR ' + ROW)
summary_re = re.compile(r'MOB_SUMMARY .*?player=(\d+)')
server_re = re.compile(r'^SPREAD seq=(\d+) shooter=(\d+) i=(\d+) spread=([-\d.]+) mult=([-\d.]+) aim=([-\d.]+),([-\d.]+),([-\d.]+) dir=([-\d.]+),([-\d.]+),([-\d.]+)')


def row(g):
    return dict(seq=int(g[0]), i=int(g[1]), frame=int(g[2]), spread=float(g[3]), mult=float(g[4]), aim=tuple(float(x) for x in g[5:8]), dir=tuple(float(x) for x in g[8:11]))


def angle(a, b):
    cross = (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    return math.degrees(math.atan2(math.sqrt(sum(c * c for c in cross)), sum(x * y for x, y in zip(a, b))))


def local(aim, d):
    # 조준을 앞으로 둔 좌표에서 (가운데서 벌어진 각, 도는 각) — 조준이 달라도 퍼짐이 같은지 본다
    f = aim
    flat = math.hypot(f[0], f[1]) or 1e-9
    r = (-f[1] / flat, f[0] / flat, 0.0)
    u = (f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0])
    x = sum(a * b for a, b in zip(d, f))
    y = sum(a * b for a, b in zip(d, r))
    z = sum(a * b for a, b in zip(d, u))
    return (y, z, x)


def pct(values, p):
    return sorted(values)[min(len(values) - 1, int(len(values) * p))] if values else 0.0


preds, shots, me, spawn_last = {}, [], None, []
for line in open(args[0], encoding='utf-8', errors='replace'):
    if 'MOB_SPAWN me' in line:
        m = re.search(r'lastSeq=(\d+)', line)
        if m:
            spawn_last.append(int(m.group(1)))
    elif 'MOB_PREDSHOT' in line:
        m = pred_re.search(line)
        if m:
            r = row(m.groups())
            preds[(r['seq'], r['i'])] = r
    elif 'MOB_SHOTDIR' in line:
        m = shot_re.search(line)
        if m:
            shots.append(row(m.groups()))
    elif 'MOB_SUMMARY' in line:
        m = summary_re.search(line)
        if m:
            me = int(m.group(1))

server, server_all, weapon_of = {}, 0, {}
aim_re = re.compile(r'^AIM seq=(\d+) shooter=(\d+) weapon=(\d+)')
for line in open(args[1], encoding='utf-8', errors='replace'):
    if line.startswith('AIM'):
        m = aim_re.match(line)
        if m and me is not None and int(m.group(2)) == me:
            weapon_of[int(m.group(1))] = int(m.group(3))
        continue
    if not line.startswith('SPREAD'):
        continue
    m = server_re.match(line)
    if not m:
        continue
    server_all += 1
    g = m.groups()
    if me is not None and int(g[1]) != me:
        continue
    server[(int(g[0]), int(g[2]))] = dict(spread=float(g[3]), mult=float(g[4]), aim=tuple(float(x) for x in g[5:8]), dir=tuple(float(x) for x in g[8:11]))

print(f'me={me} 서버줄(전원)={server_all} 서버줄(나)={len(server)} 예측줄={len(preds)} 능력줄={len(shots)}')
if me is None or not server or not preds:
    print('SPREADCHECK FAIL 잇기 전에 줄이 모자란다')
    sys.exit(1)

# 1) 예측 <-> 서버
both = [k for k in preds if k in server]
diffs = [angle(preds[k]['dir'], server[k]['dir']) for k in both]
aim_diffs = [angle(preds[k]['aim'], server[k]['aim']) for k in both]
spread_max = max((abs(preds[k]['spread'] * preds[k]['mult'] - server[k]['spread'] * server[k]['mult']) for k in both), default=0.0)
pred_only = len(preds) - len(both)
# 부활 경계 — 클라가 부활을 알기 전에 보낸 입력의 사격을 서버는 쏘고 클라는 대기 중이라 안 쏜다. 알림이 오기까지의 입력(0.5초 = 30입력) 안이면 경계로 센다
RESPAWN_WINDOW = 30
server_only_keys = [k for k in server if k not in preds]
at_respawn = sum(1 for k in server_only_keys if any(0 < k[0] - s <= RESPAWN_WINDOW for s in spawn_last))
server_only = len(server_only_keys) - at_respawn
print(f'예측 <-> 서버: 이은 탄 {len(both)}, 예측만 {pred_only}, 서버만 {server_only}(부활 경계 {at_respawn} 뺌) | 방향 차이 최대 {max(diffs, default=0):.6f}도 p99 {pct(diffs, 0.99):.6f} | 조준 차이 최대 {max(aim_diffs, default=0):.6f}도 | 퍼짐 차이 최대 {spread_max:.6f}도')
WEAPONS = {0: '권총', 1: '소총', 2: '샷건'}
for w in sorted(set(weapon_of.values())):
    ks = [k for k in both if weapon_of.get(k[0]) == w]
    ds = [angle(preds[k]['dir'], server[k]['dir']) for k in ks]
    wrong = sum(1 for d in ds if d > LIMIT)
    print(f'  {WEAPONS.get(w, w)}: 탄 {len(ks)}, 기준 넘음 {wrong}, 방향 차이 최대 {max(ds, default=0):.6f}도')
pred_ok = both and max(diffs) <= LIMIT and server_only == 0
print(f'기준 {LIMIT}도, 서버만 0: {"PASS" if pred_ok else "FAIL"}')

# 2) 능력 <-> 서버 (능력은 입력을 만들기 전에 돌아서 번호를 다음 입력으로 짐작한다)
shot_keys = {(s['seq'], s['i']) for s in shots}
matched = [s for s in shots if (s['seq'], s['i']) in server]
missed = len(shots) - len(matched)
m_diffs = [angle(s['dir'], server[(s['seq'], s['i'])]['dir']) for s in matched]
m_aim = [angle(s['aim'], server[(s['seq'], s['i'])]['aim']) for s in matched]
m_spread = [abs(s['spread'] * s['mult'] - server[(s['seq'], s['i'])]['spread'] * server[(s['seq'], s['i'])]['mult']) for s in matched]
m_local = [angle(local(s['aim'], s['dir']), local(server[(s['seq'], s['i'])]['aim'], server[(s['seq'], s['i'])]['dir'])) for s in matched]
if not shots:
    print('능력 줄 없음(-MOBAbilitySpread 를 안 켠 판) — 예측만 판정')
    print('SPREADCHECK ' + ('PASS' if pred_ok else 'FAIL') + f' 예측={"PASS" if pred_ok else "FAIL"} 능력=안 봄')
    sys.exit(0 if pred_ok else 1)
print(f'능력 <-> 서버: 능력 탄 {len(shots)}, 서버에 같은 번호 있음 {len(matched)}, 없음 {missed}, 능력이 안 쏜 서버 탄 {len(server.keys() - shot_keys)}')
if matched:
    print(f'  방향 차이 최대 {max(m_diffs):.6f}도 p50 {pct(m_diffs, 0.5):.6f} p99 {pct(m_diffs, 0.99):.6f} | 조준 차이 최대 {max(m_aim):.6f}도 p50 {pct(m_aim, 0.5):.6f} | 퍼짐 차이 최대 {max(m_spread):.6f}도 p50 {pct(m_spread, 0.5):.6f}')
    print(f'  조준을 뺀 탄 방향 차이 최대 {max(m_local):.6f}도 p50 {pct(m_local, 0.5):.6f} p99 {pct(m_local, 0.99):.6f}')

# 번호가 맞는 정도 — 능력 탄마다 프레임이 가장 가까운 예측 발과의 번호 차이
first_preds = sorted((r['frame'], r['seq']) for k, r in preds.items() if k[1] == 0)
hist_seq, hist_frame = {}, {}
for s in shots:
    if s['i'] != 0 or not first_preds:
        continue
    best = min(first_preds, key=lambda fs: (abs(fs[0] - s['frame']), fs[0]))
    ds = s['seq'] - best[1]
    df = s['frame'] - best[0]
    hist_seq[ds] = hist_seq.get(ds, 0) + 1
    hist_frame[df] = hist_frame.get(df, 0) + 1
print('  가장 가까운 예측 발과 번호 차이(능력 - 예측): ' + ' '.join(f'{k}:{v}' for k, v in sorted(hist_seq.items())))
print('  가장 가까운 예측 발과 프레임 차이(능력 - 예측): ' + ' '.join(f'{k}:{v}' for k, v in sorted(hist_frame.items())))
shot_ok = matched and missed == 0 and max(m_diffs) <= LIMIT
print(f'기준 {LIMIT}도, 없음 0: {"PASS" if shot_ok else "FAIL"}')
print('SPREADCHECK ' + ('PASS' if pred_ok and shot_ok else 'FAIL') + f' 예측={"PASS" if pred_ok else "FAIL"} 능력={"PASS" if shot_ok else "FAIL"}')
sys.exit(0 if pred_ok and shot_ok else 1)
