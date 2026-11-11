#!/usr/bin/env python3
# 화면 ↔ 서버 자세 대조 (후보 1, 5단계) — Lyra 의 MOB_VIEWP(쏠 때 화면 22부위)와 서버 REWINDP(되감은 22부위)를 (seq, 상대, 부위) 로 잇는다
#   python check_view_pose.py <lyra.log> <server.txt> [--cm=0.1] [--deg=0.3] [--bullets=300] [--limit=18]
# ① 부위 차이: 부위 중심 cm, 축 각도(상자 = 세 축, 캡슐 = 긴 축, 구 = 없음). 기준 0.1cm · 0.3도는 제안값
# ② 같은 탄: 화면에서 맞은 탄이 서버에서도 맞는가. 소총 규칙(선 먼저 → 구 5.5)으로 머리 둘레 60cm · 몸 중심 둘레 100cm 공을 지나는 탄을 줄마다 쏜다.
#    머리 = 약점에 맞은 탄 기준, 몸 = 어디든 맞은 탄 기준. 100발당 오탈 = 화면에서 맞고 서버에서 안 맞은 발 수
# rc=0 기준 안 · rc=1 기준 밖이거나 이은 줄이 0 (0 은 「맞다」가 아니라 「못 읽었다」)
import math
import re
import sys

import numpy as np

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

opt = {}
pos = []
for a in sys.argv[1:]:
    if a.startswith('--'):
        k, _, v = a[2:].partition('=')
        opt[k] = v
    else:
        pos.append(a)
lyra_path, srv_path = pos[0], pos[1]
limit_cm = float(opt.get('cm', 0.1))
limit_deg = float(opt.get('deg', 0.3))
bullets = int(opt.get('bullets', 300))
rewind_limit = int(opt.get('limit', 18))    # 서버 --rewindLimitTicks 와 같게
RADIUS = 5.5
LIMIT_HEAD = 0.5    # 100발당, D33

NUM = r'(-?[\d.]+)'
TRI = NUM + ',' + NUM + ',' + NUM
VIEWP = re.compile(r'MOB_VIEWP seq=(\d+) target=(\d+) bone=(\S+) p=' + TRI + ' x=' + TRI + ' y=' + TRI + ' z=' + TRI)
SRVP = re.compile(r'REWINDP seq=(\d+) shooter=(\d+) target=(\d+) bone=(\S+) kind=(\d) weak=(\d) p=' + TRI + ' x=' + TRI + ' y=' + TRI + ' z=' + TRI + ' s=' + TRI)
VIEW = re.compile(r'MOB_VIEW seq=(\d+) target=(\d+) viewTick=(\d+) alpha=([\d.]+) x=' + NUM + ' y=' + NUM + ' z=' + NUM + ' yaw=' + NUM)
REW = re.compile(r'REWIND seq=(\d+) shooter=(\d+) target=(\d+) now=(\d+) viewTick=(\d+) alpha=([\d.]+) x=' + NUM + ' y=' + NUM + ' z=' + NUM + ' yaw=' + NUM)

me = None
screen = {}
screen_at = {}
view_lines = 0
for line in open(lyra_path, encoding='utf-8', errors='replace'):
    if me is None:
        m = re.search(r'MOB_WELCOME player=(\d+)', line)
        if m:
            me = int(m.group(1))
    m = VIEWP.search(line)
    if m:
        view_lines += 1
        g = m.groups()
        screen.setdefault((int(g[0]), int(g[1])), {})[g[2]] = np.array(g[3:], float)
        continue
    m = VIEW.search(line)
    if m:
        g = m.groups()
        screen_at[(int(g[0]), int(g[1]))] = (int(g[2]), float(g[3]), np.array(g[4:7], float), float(g[7]))

server = {}
server_at = {}
srv_lines = 0
for line in open(srv_path, encoding='utf-8', errors='replace'):
    m = SRVP.match(line)
    if m:
        g = m.groups()
        if int(g[1]) != me:
            continue
        srv_lines += 1
        server.setdefault((int(g[0]), int(g[2])), {})[g[3]] = (int(g[4]), int(g[5]), np.array(g[6:18], float), np.array(g[18:21], float))
        continue
    m = REW.match(line)
    if m and int(m.group(2)) == me:
        g = m.groups()
        server_at[(int(g[0]), int(g[2]))] = (int(g[4]), float(g[5]), np.array(g[6:9], float), float(g[9]), int(g[3]))


def angle(a, b):
    return math.degrees(math.acos(max(-1.0, min(1.0, float(np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b)))))))


keys = sorted(set(screen) & set(server))
# 되감기 한도에 걸린 쏨은 서버가 일부러 다른 시점으로 판정한다(Clamped) — 자세 식의 대조가 아니므로 따로 센다
clamped = [k for k in keys if k in server_at and k in screen_at and server_at[k][4] - screen_at[k][0] > rewind_limit]    # ClassifyRewind 의 Clamped 와 같은 조건
keys = [k for k in keys if k not in set(clamped)]
only_screen = len(set(screen) - set(server))
only_server = len(set(server) - set(screen))
partial = 0
rows = 0
cms = []
degs = []
worst_c = (0.0, None)
worst_a = (0.0, None)
for key in keys:
    s, v = server[key], screen[key]
    if len(s) != 22 or len(v) != 22 or set(s) != set(v):
        partial += 1
        continue
    for bone, (kind, _weak, sv, _size) in s.items():
        w = v[bone]
        rows += 1
        dc = float(np.linalg.norm(sv[:3] - w[:3]))
        if kind == 2:   # 상자
            da = max(angle(sv[3:6], w[3:6]), angle(sv[6:9], w[6:9]), angle(sv[9:12], w[9:12]))
        elif kind == 1: # 캡슐
            da = angle(sv[9:12], w[9:12])
        else:
            da = 0.0
        cms.append(dc)
        degs.append(da)
        if dc > worst_c[0]:
            worst_c = (dc, key + (bone,))
        if da > worst_a[0]:
            worst_a = (da, key + (bone,))

print('me=%s 화면줄=%d 서버줄=%d 이은쏨=%d 한도걸림=%d 화면만=%d 서버만=%d 부위수다름=%d 부위행=%d' % (me, view_lines, srv_lines, len(keys), len(clamped), only_screen, only_server, partial, rows))
ok = rows > 0
if rows:
    c = np.array(cms)
    d = np.array(degs)
    print('부위 차이: 중심 최대 %.4fcm p99 %.4f (%s) | 각도 최대 %.4f도 p99 %.4f (%s)' % (c.max(), np.percentile(c, 99), worst_c[1], d.max(), np.percentile(d, 99), worst_a[1]))
    ok = c.max() <= limit_cm and d.max() <= limit_deg
    print('기준 %.2fcm · %.2f도: %s' % (limit_cm, limit_deg, 'PASS' if ok else 'FAIL'))
else:
    print('이은 부위 행 없음')


def to_arrays(pose_rows, bones):
    return np.array([pose_rows[b] for b in bones])


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
        if kind == 2:
            h = np.array([a, b, c]) * .5
            with np.errstate(divide='ignore', invalid='ignore'):
                t1 = (-h - p) / d
                t2 = (h - p) / d
            tn = np.nanmax(np.minimum(t1, t2), 1)
            tf = np.nanmin(np.maximum(t1, t2), 1)
            t = np.where((tn <= tf) & (tf > 0), tn, np.inf)
        else:
            hl = (b * .5) if kind == 1 else 0.
            t = np.full(N, np.inf)
            A = d[:, 0] ** 2 + d[:, 1] ** 2
            B = p[:, 0] * d[:, 0] + p[:, 1] * d[:, 1]
            C = p[:, 0] ** 2 + p[:, 1] ** 2 - a * a
            disc = B * B - A * C
            good = (disc >= 0) & (A > 1e-12)
            tc = np.where(good, (-B - np.sqrt(np.maximum(disc, 0))) / np.where(A > 1e-12, A, 1), np.inf)
            z = p[:, 2] + np.where(good, tc, 0) * d[:, 2]
            t = np.where(good & (np.abs(z) <= hl), tc, t)
            for sign in (-1, 1):
                q = p - np.array([0, 0, sign * hl])
                B2 = (q * d).sum(1)
                C2 = (q * q).sum(1) - a * a
                A2 = (d * d).sum(1)
                disc2 = B2 * B2 - A2 * C2
                good2 = disc2 >= 0
                ts = np.where(good2, (-B2 - np.sqrt(np.maximum(disc2, 0))) / A2, np.inf)
                t = np.minimum(t, ts)
        upd = t < best
        best[upd] = t[upd]
        idx[upd] = i
    return idx


def inflate(m, r):
    return [(k, a + 2 * r, b + 2 * r, c + 2 * r) if k == 2 else (k, a + r, b, c) for k, a, b, c in m]


def resolve(T, meta, fat, O, D):
    idx = first_hit(T, meta, O, D)
    miss = idx < 0
    if miss.any():
        idx[miss] = first_hit(T, fat, O[miss], D[miss])
    return idx


def rot(v, yaw_deg):
    a = math.radians(yaw_deg)
    c, s = math.cos(a), math.sin(a)
    out = v.copy()
    out[:, 0] = c * v[:, 0] - s * v[:, 1]
    out[:, 1] = s * v[:, 0] + c * v[:, 1]
    return out


rng = np.random.default_rng(7)


def rays(c, n, ball):
    u = rng.normal(size=(n, 3))
    u /= np.linalg.norm(u, axis=1, keepdims=True)
    u *= ball * rng.random((n, 1)) ** (1 / 3)
    az = rng.uniform(0, 2 * math.pi, n)
    el = np.radians(rng.uniform(-5.0, 5.0, n))
    D = np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], 1)
    return c + u - D * 1500, D


head_hit = head_lost = head_gain = body_hit = body_lost = body_gain = 0
place_max = 0.0
bullet_rows = 0
for key in keys:
    s, v = server[key], screen[key]
    if len(s) != 22 or len(v) != 22 or set(s) != set(v) or key not in server_at or key not in screen_at:
        continue
    bones = sorted(s)
    meta = [(s[b][0], float(s[b][3][0]), float(s[b][3][1]), float(s[b][3][2])) for b in bones]
    fat = inflate(meta, RADIUS)
    weak = np.array([s[b][1] for b in bones], bool)
    head = int(np.argmax(weak))
    Ts = to_arrays({b: s[b][2] for b in bones}, bones)
    Tv = to_arrays(v, bones)
    _, _, spos, syaw, _ = server_at[key]
    _, _, vpos, vyaw = screen_at[key]
    bullet_rows += 1
    place_max = max(place_max, float(np.linalg.norm(vpos - spos)))
    for ball, center, is_head in ((60.0, Tv[head, :3], True), (100.0, np.zeros(3), False)):
        O, D = rays(center, bullets, ball)
        hv = resolve(Tv, meta, fat, O, D)
        # 같은 탄을 서버 몸 기준 좌표로 옮긴다
        Ow, Dw = rot(O, vyaw) + vpos, rot(D, vyaw)
        O2, D2 = rot(Ow - spos, -syaw), rot(Dw, -syaw)
        hs = resolve(Ts, meta, fat, O2, D2)
        if is_head:
            a, b = (hv >= 0) & weak[np.maximum(hv, 0)], (hs >= 0) & weak[np.maximum(hs, 0)]
            head_hit += int(a.sum())
            head_lost += int((a & ~b).sum())
            head_gain += int((~a & b).sum())
        else:
            a, b = hv >= 0, hs >= 0
            body_hit += int(a.sum())
            body_lost += int((a & ~b).sum())
            body_gain += int((~a & b).sum())

if bullet_rows:
    head_rate = 100.0 * head_lost / max(1, head_hit)
    body_rate = 100.0 * body_lost / max(1, body_hit)
    print('같은 탄 %d줄 x %d발: 머리 100발당 오탈 %.3f (덤 %.3f, 분모 %d) | 몸 100발당 오탈 %.3f (덤 %.3f, 분모 %d)' % (bullet_rows, bullets, head_rate, 100.0 * head_gain / max(1, head_hit), head_hit, body_rate, 100.0 * body_gain / max(1, body_hit), body_hit))
    print('캡슐 자리 차이 최대 %.3fcm' % place_max)
    good_head = head_rate <= LIMIT_HEAD
    print('머리 기준 %.1f발: %s' % (LIMIT_HEAD, 'PASS' if good_head else 'FAIL'))
    ok = ok and good_head
else:
    print('같은 탄을 쏠 줄 없음')
    ok = False

print('VIEWPOSE', 'PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
