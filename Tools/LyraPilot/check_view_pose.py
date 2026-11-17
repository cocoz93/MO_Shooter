#!/usr/bin/env python3
# 화면 ↔ 서버 자세 대조 (후보 1, 5단계) — Lyra 의 MOB_VIEWP(쏠 때 화면 22부위)와 서버 REWINDP(되감은 22부위)를 (seq, 상대, 부위) 로 잇는다
#   python check_view_pose.py <lyra.log> <server.txt> [--cm=0.1] [--deg=0.3] [--bullets=300] [--limit=18]
# ① 부위 차이: 부위 중심 cm, 축 각도(상자 = 세 축, 캡슐 = 긴 축, 구 = 없음). 기준 0.1cm · 0.3도는 제안값
# ② 같은 탄: 화면에서 맞은 탄이 서버에서도 맞는가. 소총 규칙(선 먼저 → 구 5.5)으로 머리 둘레 60cm · 몸 중심 둘레 100cm 공을 지나는 탄을 줄마다 쏜다.
#    머리 = 약점에 맞은 탄 기준, 몸 = 어디든 맞은 탄 기준. 100발당 오탈 = 화면에서 맞고 서버에서 안 맞은 발 수
# ③ 상대가 몽타주 중인 쏨을 따로 센다 — 서버 REWIND 줄의 montages(그 자세에 든 몽타주 칸 수). 판정 줄 VIEWPOSE 는 전부를 본다
# ④ 상대가 조준 중인 쏨도 같은 식으로 따로 센다 — 서버 REWIND 줄의 ads(조준 묶음 비중 0~255, C-5). 0 이면 조준 자세를 안 본 것
# ⑥ 상대가 맞은 쪽 반응 몽타주 중인 쏨도 따로 센다 — 서버 REWIND 줄의 react(1 = 반응 몽타주 칸이 실림, E-3 17-d). 반응은 근접에 맞을 때만 켜져 근접 판에서만 분모가 생긴다
# ⑤ 상대가 앉은 쏨도 따로 센다 — 서버 REWIND 줄의 crouch(앉기 묶음 비중 0~255, D-6). 다 앉음(255) · 섞이는 중(1~254) · 서기(0) 셋으로 나눈다 — 앉으면 메시가 25 올라가 다 앉은 쪽이 따로 맞아야 한다
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
REW = re.compile(r'REWIND seq=(\d+) shooter=(\d+) target=(\d+) now=(\d+) viewTick=(\d+) alpha=([\d.]+) x=' + NUM + ' y=' + NUM + ' z=' + NUM + ' yaw=' + NUM + r'(?: animWeapon=(\d+))?(?: montages=(\d+))?(?: ads=(\d+))?(?: crouch=(\d+))?(?: react=(\d+))?')
WEAPON_NAMES = {0: '권총', 1: '소총', 2: '샷건', -1: '무기칸없음'}    # 서버 EWeapon 차례. -1 = 9단계 전 서버 기록(REWIND 에 animWeapon 이 없다)
MONTAGE_GROUPS = {1: '몽타주 중', 0: '몽타주 없음', -1: '몽타주칸없음'}    # -1 = 5-c 전 서버 기록(REWIND 에 montages 가 없다)
ADS_GROUPS = {1: '조준 중', 0: '조준 안 함', -1: '조준칸없음'}    # -1 = C-5 전 서버 기록(REWIND 에 ads 가 없다)
CROUCH_GROUPS = {2: '다 앉음', 1: '앉기 섞임', 0: '서기', -1: '앉기칸없음'}    # -1 = D-6 전 서버 기록(REWIND 에 crouch 가 없다)
REACT_GROUPS = {1: '반응 중', 0: '반응 없음', -1: '반응칸없음'}    # -1 = 17-d 전 서버 기록(REWIND 에 react 가 없다)


def crouch_group(raw):
    if raw is None:
        return -1
    v = int(raw)
    return 2 if v >= 255 else (1 if v > 0 else 0)

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
        server_at[(int(g[0]), int(g[2]))] = (int(g[4]), float(g[5]), np.array(g[6:9], float), float(g[9]), int(g[3]), int(g[10]) if g[10] is not None else -1,
                                             min(int(g[11]), 1) if g[11] is not None else -1, min(int(g[12]), 1) if g[12] is not None else -1, crouch_group(g[13]),
                                             min(int(g[14]), 1) if g[14] is not None else -1)


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
per_weapon = {}     # 서버가 그 자세를 푼 무기 묶음별 [쏨, 부위행, 중심 최대, 각도 최대] — 권총 · 샷건 상대를 실제로 대조했는지 센다
per_montage = {}    # 상대가 몽타주 중인가(MONTAGE_GROUPS)별 같은 칸
per_ads = {}        # 상대가 조준 중인가(ADS_GROUPS)별 같은 칸
per_crouch = {}     # 상대가 앉았나(CROUCH_GROUPS)별 같은 칸
per_react = {}      # 상대가 반응 중인가(REACT_GROUPS)별 같은 칸
for key in keys:
    s, v = server[key], screen[key]
    if len(s) != 22 or len(v) != 22 or set(s) != set(v):
        partial += 1
        continue
    pw = per_weapon.setdefault(server_at[key][5] if key in server_at else -1, [0, 0, 0.0, 0.0])
    pw[0] += 1
    pm = per_montage.setdefault(server_at[key][6] if key in server_at else -1, [0, 0, 0.0, 0.0])
    pm[0] += 1
    pa = per_ads.setdefault(server_at[key][7] if key in server_at else -1, [0, 0, 0.0, 0.0])
    pa[0] += 1
    pc = per_crouch.setdefault(server_at[key][8] if key in server_at else -1, [0, 0, 0.0, 0.0])
    pc[0] += 1
    pr = per_react.setdefault(server_at[key][9] if key in server_at else -1, [0, 0, 0.0, 0.0])
    pr[0] += 1
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
        pw[1] += 1
        pw[2] = max(pw[2], dc)
        pw[3] = max(pw[3], da)
        pm[1] += 1
        pm[2] = max(pm[2], dc)
        pm[3] = max(pm[3], da)
        pa[1] += 1
        pa[2] = max(pa[2], dc)
        pa[3] = max(pa[3], da)
        pc[1] += 1
        pc[2] = max(pc[2], dc)
        pc[3] = max(pc[3], da)
        pr[1] += 1
        pr[2] = max(pr[2], dc)
        pr[3] = max(pr[3], da)
        if dc > worst_c[0]:
            worst_c = (dc, key + (bone,))
        if da > worst_a[0]:
            worst_a = (da, key + (bone,))

print('me=%s 화면줄=%d 서버줄=%d 이은쏨=%d 한도걸림=%d 화면만=%d 서버만=%d 부위수다름=%d 부위행=%d' % (me, view_lines, srv_lines, len(keys), len(clamped), only_screen, only_server, partial, rows))
print('무기별(서버 묶음): ' + (' | '.join('%s 쏨=%d 부위행=%d 중심최대=%.4fcm 각도최대=%.4f도' % (WEAPON_NAMES.get(w, str(w)), p[0], p[1], p[2], p[3]) for w, p in sorted(per_weapon.items())) or '없음'))
print('몽타주별(서버 자세): ' + (' | '.join('%s 쏨=%d 부위행=%d 중심최대=%.4fcm 각도최대=%.4f도 %s' % (MONTAGE_GROUPS.get(g, str(g)), p[0], p[1], p[2], p[3], 'PASS' if p[2] <= limit_cm and p[3] <= limit_deg else 'FAIL')
                                    for g, p in sorted(per_montage.items(), reverse=True)) or '없음'))
print('조준별(서버 자세): ' + (' | '.join('%s 쏨=%d 부위행=%d 중심최대=%.4fcm 각도최대=%.4f도 %s' % (ADS_GROUPS.get(g, str(g)), p[0], p[1], p[2], p[3], 'PASS' if p[2] <= limit_cm and p[3] <= limit_deg else 'FAIL')
                                  for g, p in sorted(per_ads.items(), reverse=True)) or '없음'))
print('앉기별(서버 자세): ' + (' | '.join('%s 쏨=%d 부위행=%d 중심최대=%.4fcm 각도최대=%.4f도 %s' % (CROUCH_GROUPS.get(g, str(g)), p[0], p[1], p[2], p[3], 'PASS' if p[2] <= limit_cm and p[3] <= limit_deg else 'FAIL')
                                  for g, p in sorted(per_crouch.items(), reverse=True)) or '없음'))
print('반응별(서버 자세): ' + (' | '.join('%s 쏨=%d 부위행=%d 중심최대=%.4fcm 각도최대=%.4f도 %s' % (REACT_GROUPS.get(g, str(g)), p[0], p[1], p[2], p[3], 'PASS' if p[2] <= limit_cm and p[3] <= limit_deg else 'FAIL')
                                  for g, p in sorted(per_react.items(), reverse=True)) or '없음'))
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
group_bullets = {}  # 몽타주 묶음별 [머리 분모, 머리 오탈, 몸 분모, 몸 오탈]
ads_bullets = {}    # 조준 묶음별 같은 칸
crouch_bullets = {} # 앉기 묶음별 같은 칸
react_bullets = {}  # 반응 묶음별 같은 칸
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
    spos, syaw = server_at[key][2], server_at[key][3]
    gb = group_bullets.setdefault(server_at[key][6], [0, 0, 0, 0])
    ab = ads_bullets.setdefault(server_at[key][7], [0, 0, 0, 0])
    cb = crouch_bullets.setdefault(server_at[key][8], [0, 0, 0, 0])
    rb = react_bullets.setdefault(server_at[key][9], [0, 0, 0, 0])
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
            gb[0] += int(a.sum())
            gb[1] += int((a & ~b).sum())
            ab[0] += int(a.sum())
            ab[1] += int((a & ~b).sum())
            cb[0] += int(a.sum())
            cb[1] += int((a & ~b).sum())
            rb[0] += int(a.sum())
            rb[1] += int((a & ~b).sum())
            head_gain += int((~a & b).sum())
        else:
            a, b = hv >= 0, hs >= 0
            body_hit += int(a.sum())
            body_lost += int((a & ~b).sum())
            gb[2] += int(a.sum())
            gb[3] += int((a & ~b).sum())
            ab[2] += int(a.sum())
            ab[3] += int((a & ~b).sum())
            cb[2] += int(a.sum())
            cb[3] += int((a & ~b).sum())
            rb[2] += int(a.sum())
            rb[3] += int((a & ~b).sum())
            body_gain += int((~a & b).sum())

if bullet_rows:
    head_rate = 100.0 * head_lost / max(1, head_hit)
    body_rate = 100.0 * body_lost / max(1, body_hit)
    print('같은 탄 %d줄 x %d발: 머리 100발당 오탈 %.3f (덤 %.3f, 분모 %d) | 몸 100발당 오탈 %.3f (덤 %.3f, 분모 %d)' % (bullet_rows, bullets, head_rate, 100.0 * head_gain / max(1, head_hit), head_hit, body_rate, 100.0 * body_gain / max(1, body_hit), body_hit))
    print('캡슐 자리 차이 최대 %.3fcm' % place_max)
    print('몽타주별 오탈(100발당): ' + ' | '.join('%s 머리 %.3f (분모 %d) 몸 %.3f (분모 %d)' % (MONTAGE_GROUPS.get(g, str(g)), 100.0 * b[1] / max(1, b[0]), b[0], 100.0 * b[3] / max(1, b[2]), b[2])
                                       for g, b in sorted(group_bullets.items(), reverse=True)))
    print('조준별 오탈(100발당): ' + ' | '.join('%s 머리 %.3f (분모 %d) 몸 %.3f (분모 %d)' % (ADS_GROUPS.get(g, str(g)), 100.0 * b[1] / max(1, b[0]), b[0], 100.0 * b[3] / max(1, b[2]), b[2])
                                     for g, b in sorted(ads_bullets.items(), reverse=True)))
    print('앉기별 오탈(100발당): ' + ' | '.join('%s 머리 %.3f (분모 %d) 몸 %.3f (분모 %d)' % (CROUCH_GROUPS.get(g, str(g)), 100.0 * b[1] / max(1, b[0]), b[0], 100.0 * b[3] / max(1, b[2]), b[2])
                                     for g, b in sorted(crouch_bullets.items(), reverse=True)))
    print('반응별 오탈(100발당): ' + ' | '.join('%s 머리 %.3f (분모 %d) 몸 %.3f (분모 %d)' % (REACT_GROUPS.get(g, str(g)), 100.0 * b[1] / max(1, b[0]), b[0], 100.0 * b[3] / max(1, b[2]), b[2])
                                     for g, b in sorted(react_bullets.items(), reverse=True)))
    good_head = head_rate <= LIMIT_HEAD
    print('머리 기준 %.1f발: %s' % (LIMIT_HEAD, 'PASS' if good_head else 'FAIL'))
    ok = ok and good_head
else:
    print('같은 탄을 쏠 줄 없음')
    ok = False

print('VIEWPOSE', 'PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
