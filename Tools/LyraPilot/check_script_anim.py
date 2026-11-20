#!/usr/bin/env python3
# 경로 봇 몽타주 ↔ Lyra 사건 경로 기록 (후보 1, 9단계 조각 5-c)
#   python check_script_anim.py <bot.txt> <animcmp_ev_<무기>_probe.csv> [--pass=0] [--time=0.0002] [--weight=0.0021] [--except=fire_hold]
# 봇(BotClient --script=events)이 받은 자기 몽타주 칸(SCRIPTANIM 줄)을 Lyra 기록 같은 프레임의 몽타주 목록과 칸 차례대로 견준다.
#   경로 몇 번째 틱 = SCRIPTANIM seq - 마지막 SCRIPT start seq. Lyra 기록 프레임(pass 안에서 0부터)과 같은 번호로 잇는다
#   Lyra 쪽은 비중이 와이어에서 0 으로 반올림되는 칸(갓 튼 칸 · 비중 0 인 채 빠지는 빼기)을 뺀다 — 서버도 그런 칸은 안 싣는다
#   기준: 시각 0.0002초(와이어 1/6000초 칸 + 기록의 다섯 자리) · 비중 0.0021(와이어 1/255 칸의 반 + 기록의 네 자리) — 제안값
# 부활 조각(spawn)은 봇이 부활을 낼 길이 없어 따로 센다(판정에서 뺀다). --except 로 준 조각도 따로 센다 —
#   fire_hold = 당긴 채 다시 쏘는 간격이 이 기록(비교 도구가 상대 능력 입력을 직접 돌림) 8프레임 ↔ 서버 9틱(1-d 에서 Lyra 실행값으로 잰 간격)이라 둘째 발부터 1틱씩 밀린다
# Lyra 사건 프레임에서 서버에만 있는 칸은 따로 센다 — Lyra 줄은 그 프레임 사건 뒤 목록(사건이 바로 끈 칸이 빠짐)이고, 그 프레임 자세는 사건 앞에 계산된다.
#   서버는 사건을 다음 틱에 넣으므로(BotClient ThinkScript 주석) 그 줄엔 아직 있다. Lyra 칸이 서버 칸 사이에 차례대로 다 있으면 맞다로 본다
# 입력이 한 틱 굶고 뒤에 몰리면(틱당 입력 하나 아님) 그 사이 사건 입력이 한 틱 늦게 들어가 몽타주도 한 틱 늦다. 다른 줄이 Lyra 한 프레임 앞 줄과 같고
#   그 줄 서버 칸 중 가장 먼저 선 칸의 첫 틱(칸이 없으면 마지막 사건 프레임) 세 틱 앞부터 그 줄 사이에 몰림이 있었으면
#   「1틱 늦음(입력 몰림)」으로 따로 센다(판정에서 뺀다). 몰림 없이 늦으면 다름이다
#   (마지막 사건 프레임만 보면, 몰림으로 늦게 선 재장전 위에 새 사건이 난 줄을 못 가른다 — 샷건 2판 틱 1053)
# rc=0 다름 0 · rc=1 다름이 있거나 견준 줄이 0
import csv
import re
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

opt = {}
pos = []
for a in sys.argv[1:]:
    if a.startswith('--'):
        k, _, v = a[2:].partition('=')
        opt[k] = v
    else:
        pos.append(a)
bot_path, probe_path = pos[0], pos[1]
want_pass = opt.get('pass', '0')
tol_t = float(opt.get('time', 0.0002))
tol_w = float(opt.get('weight', 0.0021))
excepted = set(s for s in opt.get('except', '').split(',') if s)

KINDS = ['fire', 'reload', 'equip', 'unequip', 'spawn', 'melee', 'hit_back', 'hit_front', 'dash_forward', 'dash_backward', 'dash_left', 'dash_right', 'grenade']    # Shared/AnimClips.h EMontageKind 차례


def kind_of(name):
    if '_Fire' in name:
        return 0
    if '_Reload' in name:
        return 1
    if 'Unequip' in name:
        return 3
    if '_Equip' in name:
        return 2
    if '_Spawn' in name:
        return 4
    return -1


# ─── 봇 ───
start_seq = None
rows = {}
irregular = 0       # 틱이 하나 늘 때 입력 번호가 하나가 아니게 는 상태 — 그 틱의 애니 갱신 수와 입력 수가 다르다
irr_at = []         # 그 상태가 난 경로 틱(seq - 시작 seq)
last = None
for line in open(bot_path, encoding='utf-8', errors='replace'):
    m = re.match(r'SCRIPT start seq=(\d+)', line)
    if m:   # 경로를 다시 시작하면(경기 시작 · 부활로 자리를 다시 옮김) 마지막 시작부터 본다
        start_seq = int(m.group(1))
        rows = {}
        irregular = 0
        irr_at = []
        last = None
        continue
    m = re.match(r'SCRIPTANIM tick=(\d+) seq=(\d+) weapon=(\d+) n=(\d+)(.*)', line)
    if not m or start_seq is None:
        continue
    tick, seq = int(m.group(1)), int(m.group(2))
    if last is not None and tick == last[0] + 1 and seq != last[1] + 1:
        irregular += 1
        irr_at.append(seq - start_seq)
    last = (tick, seq)
    g = seq - start_seq
    if g < 0 or g in rows:
        continue
    slots = []
    for part in m.group(5).split():
        b, t, w = (int(x) for x in part.split(':'))
        slots.append((b & 15, b >> 4, t / 6000.0, w / 255.0))
    rows[g] = (int(m.group(3)), slots)

# ─── Lyra ───
lyra = {}
seg_of = {}
event_at = set()    # Lyra 가 사건을 일으킨 프레임
with open(probe_path, encoding='utf-8', errors='replace') as f:
    r = csv.reader(f)
    h = next(r)
    iP, iF, iS, iM, iE = h.index('pass'), h.index('frame'), h.index('segment'), h.index('montages'), h.index('events')
    first = None
    for row in r:
        if row[iP] != want_pass:
            continue
        frame = int(row[iF])
        if first is None:
            first = frame
        k = frame - first
        seg_of[k] = row[iS]
        if row[iE]:
            event_at.add(k)
        out = []
        for part in row[iM].split(';'):
            part = part.strip()
            mm = re.match(r'(\S+?)#\d+@([\d.]+)\*([\d.]+)', part)
            if not mm:
                continue
            w = float(mm.group(3))
            if round(w * 255) < 1:
                continue
            out.append((kind_of(mm.group(1)), mm.group(1), float(mm.group(2)), w))
        lyra[k] = out

# ─── 견주기 ───
def oldest_birth(k):
    # 서버 줄 k 에 든 칸이 처음 선 경로 틱 중 가장 이른 것 — 같은 종류 · 묶음 칸이 시각이 줄며 이어지는 동안 거슬러 간다.
    #   몰림으로 빠진 줄(봇에 없음)은 하나까지 건너뛴다
    best = None
    for sk, sb, st, _sw in rows[k][1]:
        j, t = k, st
        while True:
            pj = j - 1 if j - 1 in rows else (j - 2 if j - 2 in rows else None)
            if pj is None:
                break
            prev = [s[2] for s in rows[pj][1] if s[0] == sk and s[1] == sb and s[2] < t]
            if not prev:
                break
            j, t = pj, max(prev)
        best = j if best is None else min(best, j)
    return best


compared = count_diff = kind_diff = time_bad = weight_bad = 0
spawn_rows = spawn_diff = 0
except_rows = {}    # 조각 → [줄, 다른 줄]
event_extra = 0     # 사건 프레임에서 서버에만 남은 칸이 있던 줄(나머지는 같음)
late_bunch = 0      # 1틱 늦음(입력 몰림) 줄
late_seg = {}
missing = 0
max_t = max_w = 0.0
first_bad = []
seg_bad = {}
seg_rows = {}
for k in sorted(lyra):
    seg = seg_of[k]
    if k not in rows:
        missing += 1
        continue
    srv = rows[k][1]
    ly = lyra[k]
    if seg == 'spawn':
        spawn_rows += 1
        if len(srv) != len(ly):
            spawn_diff += 1
        continue
    def row_diff(srv, ly):
        # (칸 수 다름, 종류 다른 칸, 시각 넘은 칸, 비중 넘은 칸, 시각 최대, 비중 최대)
        c = 1 if len(srv) != len(ly) else 0
        kd = tb = wb = 0
        mt = mw = 0.0
        for (sk, _sb, st, sw), (lk, ln, lt, lw) in zip(srv, ly):
            if sk != lk:
                kd += 1
                continue
            dt, dw = abs(st - lt), abs(sw - lw)
            mt, mw = max(mt, dt), max(mw, dw)
            tb += dt > tol_t
            wb += dw > tol_w
        return c, kd, tb, wb, mt, mw
    d = row_diff(srv, ly)
    if seg in excepted:
        er = except_rows.setdefault(seg, [0, 0])
        er[0] += 1
        er[1] += 1 if any(d[:4]) else 0
        continue
    def lyra_in_server(srv, ly):
        # Lyra 칸이 서버 칸 사이에 차례대로 다 있나(서버에만 있는 칸은 건너뛴다)
        i = 0
        for sk, _sb, st, sw in srv:
            if i < len(ly) and sk == ly[i][0] and abs(st - ly[i][2]) <= tol_t and abs(sw - ly[i][3]) <= tol_w:
                i += 1
        return i == len(ly)
    if k in event_at and len(srv) > len(ly) and lyra_in_server(srv, ly):
        event_extra += 1
        continue
    if any(d[:4]) and k - 1 in lyra and not any(row_diff(srv, lyra[k - 1])[:4]):
        b = oldest_birth(k)
        e = b if b is not None else max((x for x in event_at if x <= k), default=None)
        if e is not None and any(e - 3 <= p <= k for p in irr_at):
            late_bunch += 1
            late_seg[seg] = late_seg.get(seg, 0) + 1
            continue
    compared += 1
    seg_rows[seg] = seg_rows.get(seg, 0) + 1
    count_diff += d[0]
    kind_diff += d[1]
    time_bad += d[2]
    weight_bad += d[3]
    max_t, max_w = max(max_t, d[4]), max(max_w, d[5])
    bad = any(d[:4])
    if bad:
        seg_bad[seg] = seg_bad.get(seg, 0) + 1
        if len(first_bad) < 12:
            first_bad.append((k, seg, [(KINDS[s[0]], round(s[2], 4), round(s[3], 4)) for s in srv], [(KINDS[l[0]] if l[0] >= 0 else l[1], round(l[2], 4), round(l[3], 4)) for l in ly]))

print('경로 시작 seq=%s 봇 줄=%d Lyra 프레임=%d 견준 줄=%d 봇에 없음=%d 틱당 입력 하나 아님=%d' % (start_seq, len(rows), len(lyra), compared, missing, irregular))
print('다름: 칸 수 %d줄 · 종류 %d칸 · 시각 %d칸(최대 %.5f초) · 비중 %d칸(최대 %.4f) | 부활 조각 %d줄 중 칸 수 다름 %d(봇은 부활을 안 낸다)' % (count_diff, kind_diff, time_bad, max_t, weight_bad, max_w, spawn_rows, spawn_diff))
print('사건 프레임에서 서버에만 남은 칸(나머지 같음) %d줄 | 따로 센 조각: %s' % (event_extra, ' '.join('%s 다름 %d/%d줄' % (s, e[1], e[0]) for s, e in except_rows.items()) or '없음'))
print('1틱 늦음(입력 몰림, 판정에서 뺌) %d줄: %s | 몰림 자리: %s' % (late_bunch, ' '.join('%s=%d' % (s, n) for s, n in late_seg.items()) or '없음', ' '.join(str(p) for p in irr_at) or '없음'))
print('조각별 다른 줄: ' + (' '.join('%s=%d/%d' % (s, seg_bad[s], seg_rows[s]) for s in seg_rows if s in seg_bad) or '없음'))
for k, seg, s, l in first_bad:
    print('  틱 %d %s 서버 %s | Lyra %s' % (k, seg, s, l))
ok = compared > 0 and count_diff == 0 and kind_diff == 0 and time_bad == 0 and weight_bad == 0
print('SCRIPTANIM', 'PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
