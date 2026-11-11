#!/usr/bin/env python3
# 언리얼 서버-클라 기준값 — 서버의 몸과 화면의 몸을 위치로 짝지어 22부위 차이를 내고 오탈로 환산한다 (candidate1-impl-plan.md 0단계)
#   python check_dedi_baseline.py <서버.csv> <클라.csv> [--self] [--shift=N] [--limit=프레임수]
#   --self    = 클라 기록을 자기 자신과 대조한다(도구 확인, 전부 0.00 이어야 한다)
#   --shift=N = 화면 프레임을 N 만큼 밀어 대조한다(도구 확인, 달리는 구간 값이 커져야 한다)
# 입력 형식 = FRAME · POSEB 줄 (MOBDediBaselineSubsystem)
#
# 짝짓기 = 「위치 되감기는 완벽하다」고 가정하고 자세 차이만 본다. 화면 프레임마다 서버 경로에서 몸 자리가 가장 가까운 지점을 찾아 앞뒤 틱의 자세를 그 비율로 섞는다.
# 서 있는 구간은 자리로 시점을 못 고르므로 달리는 구간에서 구한 시간 차이의 중앙값에 가장 가까운 지점을 쓴다.
# 언리얼에 유리한 쪽으로 잡은 것이다 — 위치 오차가 없고, 틱 사이도 섞고, 서버가 직접 움직이는 봇이라 입력 흔들림도 없다.
#
# 오탈 환산은 후보 1과 같은 계산기다: 소총 반지름 5.5 · 머리 둘레 60cm 공 · 몸은 몸 중심 둘레 100cm 공 · 사방 · 높이각 ±5도 · 프레임당 300발 (body-pose-calc/pose_miss.py 와 같은 조건)
import math
import sys

import numpy as np

sys.stdout.reconfigure(encoding='utf-8')
args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict((a[2:].split('=', 1) + [''])[:2] for a in sys.argv[1:] if a.startswith('--'))
SELF = 'self' in opt
SHIFT = int(opt.get('shift', 0) or 0)
LIMIT = int(opt.get('limit', 0) or 0)
N = 300
RUN_SPEED = 500
RADIUS = 5.5
WINDOW_WIDE = 0.8       # 시간 차이를 처음 구할 때 앞뒤로 볼 폭(초). 왕복 지연 + 부드럽게 따라가는 시간을 넉넉히 덮는 값
WINDOW_NEAR = 0.15      # 시간 차이의 중앙값 둘레로 볼 폭(초). 판 중간에 서버 시각 맞춤이 몇 ms 움직여도 덮는 값
TIME_WEIGHT = 1.0       # 시각이 1초 멀면 1cm 더 먼 것으로 친다. 서 있는 구간의 동점을 가르는 용도라 달리는 구간(틱당 10cm)에는 영향이 없다
FAR_CM = 30.0           # 이보다 먼 짝은 못 이은 줄로 센다
GAP_SECONDS = 0.05      # 서버 기록이 이만큼 끊긴 두 프레임은 잇지 않는다(판과 판 사이에 기록이 멈춘다). 60틱 3개 분량


def load(path):
    frames = []
    index = {}
    bones = []
    meta = {}
    weak = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('FRAME,'):
            c = line.strip().split(',')
            index[int(c[1])] = len(frames)
            frames.append(dict(t=float(c[2]), p=(float(c[3]), float(c[4]), float(c[5])), yaw=float(c[6]), v=(float(c[7]), float(c[8]), float(c[9])), pas=int(c[10]), seg=c[11], parts={}))
        elif line.startswith('POSEB,'):
            c = line.strip().split(',')
            f = index.get(int(c[2]))
            if f is None:
                continue
            b = c[3]
            if b not in meta:
                bones.append(b)
                meta[b] = (c[4], float(c[18]), float(c[19]), float(c[20]))
                weak[b] = int(c[5])
            frames[f]['parts'][b] = [float(x) for x in c[6:18]]
    return frames, bones, meta, weak


def arrays(frames, bones):
    good = [f for f in frames if len(f['parts']) == len(bones)]
    T = np.array([[f['parts'][b] for b in bones] for f in good], float)
    t = np.array([f['t'] for f in good])
    p = np.array([f['p'] for f in good], float)
    v = np.array([f['v'] for f in good], float)
    return good, T, t, p, v


def ortho(A):
    for i in range(len(A)):
        z = A[i, 9:12].copy()
        z /= np.linalg.norm(z)
        x = A[i, 3:6].copy()
        x -= x.dot(z) * z
        x /= np.linalg.norm(x)
        A[i, 3:6] = x
        A[i, 6:9] = np.cross(z, x)
        A[i, 9:12] = z
    return A


# ---- 아래 first_hit · inflate · resolve · rays 는 body-pose-calc/sim2.py · pose_miss.py 와 같은 식이다(후보 1 환산과 같은 계산기)
def first_hit(T, meta, O, D):
    N = len(O)
    best = np.full(N, np.inf)
    idx = np.full(N, -1)
    for i in range(len(T)):
        kind, a, b, c = meta[i]
        cen = T[i, :3]
        X, Y, Z = T[i, 3:6], T[i, 6:9], T[i, 9:12]
        r = O - cen
        p = np.stack([r @ X, r @ Y, r @ Z], 1)
        d = np.stack([D @ X, D @ Y, D @ Z], 1)
        if kind == 'box':
            h = np.array([a, b, c]) * .5
            with np.errstate(divide='ignore', invalid='ignore'):
                t1 = (-h - p) / d
                t2 = (h - p) / d
            tn = np.nanmax(np.minimum(t1, t2), 1)
            tf = np.nanmin(np.maximum(t1, t2), 1)
            t = np.where((tn <= tf) & (tf > 0), tn, np.inf)
        else:
            hl = (b * .5) if kind == 'sphyl' else 0.
            t = np.full(N, np.inf)
            A = d[:, 0] ** 2 + d[:, 1] ** 2
            B = p[:, 0] * d[:, 0] + p[:, 1] * d[:, 1]
            C = p[:, 0] ** 2 + p[:, 1] ** 2 - a * a
            disc = B * B - A * C
            ok = (disc >= 0) & (A > 1e-12)
            tc = np.where(ok, (-B - np.sqrt(np.maximum(disc, 0))) / np.where(A > 1e-12, A, 1), np.inf)
            z = p[:, 2] + np.where(ok, tc, 0) * d[:, 2]
            t = np.where(ok & (np.abs(z) <= hl), tc, t)
            for sign in (-1, 1):
                q = p - np.array([0, 0, sign * hl])
                B2 = (q * d).sum(1)
                C2 = (q * q).sum(1) - a * a
                A2 = (d * d).sum(1)
                disc2 = B2 * B2 - A2 * C2
                ok2 = disc2 >= 0
                ts = np.where(ok2, (-B2 - np.sqrt(np.maximum(disc2, 0))) / A2, np.inf)
                t = np.minimum(t, ts)
        upd = t < best
        best[upd] = t[upd]
        idx[upd] = i
    return idx


def inflate(m, r):
    return [(k, a + 2 * r, b + 2 * r, c + 2 * r) if k == 'box' else (k, a + r, b, c) for k, a, b, c in m]


def resolve(T, O, D, r):
    idx = first_hit(T, META, O, D)
    if r > 0:
        miss = idx < 0
        if miss.any():
            idx[miss] = first_hit(T, inflate(META, r), O[miss], D[miss])
    return idx


def rays(c, n, ball, el_deg):
    u = rng.normal(size=(n, 3))
    u /= np.linalg.norm(u, axis=1, keepdims=True)
    u *= ball * rng.random((n, 1)) ** (1 / 3)
    pts = c + u
    az = rng.uniform(0, 2 * math.pi, n)
    el = np.radians(rng.uniform(-el_deg, el_deg, n))
    Dr = np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], 1)
    return pts - Dr * 1500, Dr
# ---- 여기까지


def nearest_on_path(pc, ps, lo, hi, target_t, ts):
    # 서버 경로 ps[lo..hi] 의 선분 중 pc 에 가장 가까운 점. 동점이면 target_t 에 가까운 쪽
    a = ps[lo:hi]
    b = ps[lo + 1:hi + 1]
    ab = b - a
    denom = (ab * ab).sum(1)
    alpha = np.clip(np.where(denom > 1e-9, ((pc - a) * ab).sum(1) / np.where(denom > 1e-9, denom, 1), 0.0), 0.0, 1.0)
    pt = a + ab * alpha[:, None]
    dist = np.linalg.norm(pc - pt, axis=1)
    when = ts[lo:hi] + (ts[lo + 1:hi + 1] - ts[lo:hi]) * alpha
    score = dist + TIME_WEIGHT * np.abs(when - target_t)
    score = np.where(ts[lo + 1:hi + 1] - ts[lo:hi] > GAP_SECONDS, np.inf, score)
    if not np.isfinite(score).any():
        return lo, 0.0, np.inf, target_t
    k = int(np.argmin(score))
    return lo + k, float(alpha[k]), float(dist[k]), float(when[k])


s_path, c_path = args[0], args[1]
s_frames, s_bones, s_meta, s_weak = load(s_path)
c_frames, c_bones, c_meta, c_weak = load(c_path)
if SELF:
    s_frames, s_bones, s_meta, s_weak = c_frames, c_bones, c_meta, c_weak
print(f'BASELINE serverFrames={len(s_frames)} clientFrames={len(c_frames)} serverParts={len(s_bones)} clientParts={len(c_bones)}')
if not s_frames or not c_frames or not s_bones or not c_bones:
    print('BASELINE FAIL 기록이 비었다')
    sys.exit(1)

bones = [b for b in c_bones if b in s_meta]
size_diff = [b for b in bones if s_meta[b] != c_meta[b] or s_weak[b] != c_weak[b]]
print(f'이은 부위={len(bones)} 크기·약점이 다른 부위={len(size_diff)} {size_diff[:6]}')
if len(bones) == 0:
    print('BASELINE FAIL 이을 부위가 없다')
    sys.exit(1)
META = [c_meta[b] for b in bones]
WEAK = np.array([c_weak[b] for b in bones], bool)
HEAD = bones.index('head')

sf, sT, st, sp, sv = arrays(s_frames, bones)
cf, cT, ct, cp, cv = arrays(c_frames, bones)
s_dt = np.diff(st)
print(f'서버 프레임 간격 평균 {s_dt.mean() * 1000:.2f}ms 최대 {s_dt.max() * 1000:.1f}ms · 클라 프레임 간격 평균 {np.diff(ct).mean() * 1000:.2f}ms 최대 {np.diff(ct).max() * 1000:.1f}ms')

# 시간 차이(화면이 서버보다 얼마나 뒤인가)를 달리는 구간에서 구한다
c_speed = np.hypot(cv[:, 0], cv[:, 1])
offsets = []
for i in np.where(c_speed > RUN_SPEED)[0][::3]:
    lo = np.searchsorted(st, ct[i] - WINDOW_WIDE)
    hi = min(np.searchsorted(st, ct[i] + 0.2), len(st) - 1)
    if hi <= lo:
        continue
    k, al, d, when = nearest_on_path(cp[i], sp, lo, hi, ct[i], st)
    if d < 5.0:
        offsets.append(ct[i] - when)
if SELF:
    offset = 0.0
elif offsets:
    offset = float(np.median(offsets))
else:
    print('BASELINE FAIL 달리는 구간에서 시간 차이를 못 구했다')
    sys.exit(1)
print(f'시간 차이(화면이 서버보다 뒤) 중앙값 {offset * 1000:.1f}ms · p10 {np.percentile(offsets, 10) * 1000 if offsets else 0:.1f} · p90 {np.percentile(offsets, 90) * 1000 if offsets else 0:.1f} · 표본 {len(offsets)}')

# 화면 프레임마다 서버 경로의 짝을 찾는다
pairs = []
out_of_range = far = 0
dists = []
for i in range(len(ct)):
    target = ct[i] - offset
    if target < st[0] or target > st[-1]:
        out_of_range += 1
        pairs.append(None)
        continue
    lo = max(np.searchsorted(st, target - WINDOW_NEAR) - 1, 0)
    hi = min(np.searchsorted(st, target + WINDOW_NEAR) + 1, len(st) - 1)
    k, al, d, when = nearest_on_path(cp[i], sp, lo, hi, target, st)
    if d > FAR_CM:
        far += 1
        pairs.append(None)
        continue
    pairs.append((k, al, d))
    dists.append(d)
matched = sum(1 for x in pairs if x is not None)
print(f'BASELINE compared={matched} unmatched={far} outOfRange={out_of_range} posDiffMean={np.mean(dists) if dists else 0:.3f}cm posDiffMax={np.max(dists) if dists else 0:.3f}cm')
if matched == 0:
    print('BASELINE FAIL 이은 프레임 0')
    sys.exit(1)

# 짝지은 서버 자세(앞뒤 틱을 비율대로 섞음)
ref_idx = []
srv = []
seg = []
speed = []
for i, x in enumerate(pairs):
    j = i + SHIFT
    if x is None or j < 0 or j >= len(ct):
        continue
    k, al, _ = x
    ref_idx.append(j)
    srv.append(ortho(sT[k] * (1 - al) + sT[k + 1] * al))
    near = k if al < 0.5 else k + 1
    seg.append(sf[near]['seg'])
    speed.append(math.hypot(sv[near, 0], sv[near, 1]))
if LIMIT:
    ref_idx, srv, seg, speed = ref_idx[:LIMIT], srv[:LIMIT], seg[:LIMIT], speed[:LIMIT]
srv = np.array(srv)
ref = cT[np.array(ref_idx)]

diff = np.linalg.norm(ref[:, :, :3] - srv[:, :, :3], axis=2)
print(f'22부위 중심 차이 전체: 평균 {diff.mean():.2f}cm · 프레임별 최대의 평균 {diff.max(1).mean():.2f}cm · 최대 {diff.max():.2f}cm  (프레임 {len(diff)})')
head = diff[:, HEAD]
print(f'머리: 평균 {head.mean():.2f}cm · p90 {np.percentile(head, 90):.2f} · 최대 {head.max():.2f}')
worst = sorted(range(len(bones)), key=lambda b: -diff[:, b].mean())[:4]
print('부위별 평균이 큰 순서: ' + ' · '.join(f'{bones[b]} {diff[:, b].mean():.2f}(최대 {diff[:, b].max():.1f})' for b in worst))
order = []
for s in seg:
    if s not in order:
        order.append(s)
print('조각별 (프레임 수 · 22부위 평균 · 최대 · 머리 평균):')
seg_arr = np.array(seg)
for s in order:
    m = seg_arr == s
    print(f'  {s:12s} n={int(m.sum()):5d} 평균 {diff[m].mean():6.2f} 최대 {diff[m].max():6.2f} 머리 {head[m].mean():6.2f}')


def measure(frames, seed):
    global rng
    rng = np.random.default_rng(seed)
    hit = lost = gain = bhit = blost = 0
    for f in frames:
        Ts, Tv = ref[f], srv[f]
        O, D = rays(Ts[HEAD, :3], N, 60.0, 5.0)
        hs, hv = resolve(Ts, O, D, RADIUS), resolve(Tv, O, D, RADIUS)
        s, v = (hs >= 0) & WEAK[np.maximum(hs, 0)], (hv >= 0) & WEAK[np.maximum(hv, 0)]
        hit += int(s.sum())
        lost += int((s & ~v).sum())
        gain += int((~s & v).sum())
        O, D = rays(np.zeros(3), N, 100.0, 5.0)
        a, b = resolve(Ts, O, D, RADIUS) >= 0, resolve(Tv, O, D, RADIUS) >= 0
        bhit += int(a.sum())
        blost += int((a & ~b).sum())
    return 100 * lost / max(1, hit), 100 * gain / max(1, hit), 100 * blost / max(1, bhit), hit, bhit


speed = np.array(speed)
run = list(np.where(speed > RUN_SPEED)[0][::2])
rest = list(np.where(speed <= RUN_SPEED)[0][::2])
print(f'오탈 환산(화면 = 클라 몸, 서버 = 짝지은 서버 몸, 100발당): 달리는 프레임 {len(run) * 2} · 그 밖 {len(rest) * 2}, 2프레임마다 하나, 프레임당 {N}발')
for name, frames in (('달리는 상대', run), ('그 밖', rest)):
    for sd in (7, 8):
        h, g, b, nh, nb = measure(frames, sd)
        print(f'MISS {name} 난수 {sd}: 머리 오탈 {h:.2f} (덤 {g:.2f}) · 몸 오탈 {b:.2f}   (분모 머리 {nh} · 몸 {nb})')
