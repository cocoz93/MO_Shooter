#!/usr/bin/env python3
# 부딪힘 기록 요약 (컨텐츠 F-1) — Lyra 연습장 부딪힘 각본(record_content_probe.ps1 -Mode bump)의 칸마다
# 두 사람이 막히는지 · 미끄러지는지 · 미는지를 숫자로 낸다.
#   python check_bump.py <cprobe_<tag>.csv> [--other=<cprobe_<tag2>.csv>]
# - 거리 = 나 ↔ 상대 수평 중심 거리. 닿음 = 두 반지름 + 1cm 안(반지름 35 — Lyra 캡슐 기록 MOB_CPROBE_PARAMS capsuleR)
# - 닿은 뒤 = 처음 닿은 줄부터 칸 끝까지. 내 X 속도 · 옆(Y) 움직임 · 상대 움직임은 그 구간에서 잰다
# - --other 를 주면 같은 각본 두 번(다른 기록)의 칸 값을 줄마다 견줘 가장 큰 차이를 낸다(고정 프레임이면 0 이어야 한다)
# - 둘 다 걷는 칸은 실행마다 「누가 먼저 움직이나」 차례가 갈린다(f1a · f1b 실측 — 닿는 프레임에 먼저 쪽은 다 가고 나중 쪽이 막힌다).
#   first = 닿는 줄에서 속도가 큰 쪽(me · foe). 두 기록의 차례가 다르면 한쪽을 두 사람 시작 자리 가운데 점으로 뒤집어(나 ↔ 상대) 견준다
# rc=0 = 부딪힘 칸을 하나 이상 읽음(--other 면 두 기록 차이 0.01cm 안까지) · rc=1 = 못 읽었거나 두 기록이 다름
import csv
import math
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict((a[2:].split('=', 1) + [''])[:2] for a in sys.argv[1:] if a.startswith('--'))
RADIUS = 35.0
TOUCH = 2 * RADIUS + 1.0


def read(path):
    steps = {}
    for r in csv.DictReader(open(path, encoding='utf-8')):
        if r['step'].startswith('bump_'):
            steps.setdefault(r['step'], []).append(r)
    return steps


def f(r, k):
    return float(r[k])


def summarize(rows):
    gap = [math.hypot(f(r, 'px') - f(r, 'foeX'), f(r, 'py') - f(r, 'foeY')) for r in rows]
    touch = next((i for i, g in enumerate(gap) if g <= TOUCH), None)
    first, last = rows[0], rows[-1]
    out = dict(frames=len(rows), foeWalk=int(first['foeWalk']), crouched=int(first['crouched']), foeHH=f(last, 'foeHH'),
               start=gap[0], min=min(gap), end=gap[-1], touch=touch, first='-')
    if touch is not None:
        t = rows[touch]
        tail = rows[touch:]
        out.update(meMoveX=f(last, 'px') - f(t, 'px'), meMoveY=f(last, 'py') - f(t, 'py'),
                   foeMoveX=f(last, 'foeX') - f(t, 'foeX'), foeMoveY=f(last, 'foeY') - f(t, 'foeY'),
                   meVxEnd=f(last, 'vx'), meVyEnd=f(last, 'vy'), foeVxEnd=f(last, 'foeVx'), foeVyEnd=f(last, 'foeVy'),
                   meVxMin=min(f(r, 'vx') for r in tail), touchedFrames=sum(1 for g in gap[touch:] if g <= TOUCH))
        if out['foeWalk']:
            vMe = math.hypot(f(t, 'vx'), f(t, 'vy'))
            vFoe = math.hypot(f(t, 'foeVx'), f(t, 'foeVy'))
            out['first'] = 'me' if vMe > vFoe + 0.01 else ('foe' if vFoe > vMe + 0.01 else 'same')
    return out


steps = read(args[0])
if not steps:
    print('BUMP 부딪힘 칸 없음')
    sys.exit(1)
for name, rows in steps.items():
    s = summarize(rows)
    line = f'BUMP step={name} frames={s["frames"]} foeWalk={s["foeWalk"]} first={s["first"]} crouched={s["crouched"]} foeHH={s["foeHH"]:.1f} gapStart={s["start"]:.2f} gapMin={s["min"]:.3f} gapEnd={s["end"]:.3f}'
    if s['touch'] is None:
        line += ' touch=없음'
    else:
        line += (f' touchRow={s["touch"]} touchedFrames={s["touchedFrames"]} meMove={s["meMoveX"]:.2f},{s["meMoveY"]:.2f} foeMove={s["foeMoveX"]:.2f},{s["foeMoveY"]:.2f}'
                 f' meVxMin={s["meVxMin"]:.2f} meVEnd={s["meVxEnd"]:.2f},{s["meVyEnd"]:.2f} foeVEnd={s["foeVxEnd"]:.2f},{s["foeVyEnd"]:.2f}')
    print(line)

ok = True
if 'other' in opt:
    other = read(opt['other'])
    worst = 0.0
    flips = 0
    missing = sorted(set(steps) ^ set(other))
    for name in sorted(set(steps) & set(other)):
        a, b = steps[name], other[name]
        if len(a) != len(b):
            missing.append(name + '(줄 수 다름)')
            continue
        flip = summarize(a)['first'] != summarize(b)['first']
        cx = (f(b[0], 'px') + f(b[0], 'foeX')) / 2
        cy = (f(b[0], 'py') + f(b[0], 'foeY')) / 2
        flips += flip
        for ra, rb in zip(a, b):
            if flip:
                mb = {'px': 2 * cx - f(rb, 'foeX'), 'py': 2 * cy - f(rb, 'foeY'), 'foeX': 2 * cx - f(rb, 'px'), 'foeY': 2 * cy - f(rb, 'py')}
            else:
                mb = {k: f(rb, k) for k in ('px', 'py', 'foeX', 'foeY')}
            for k in ('px', 'py', 'foeX', 'foeY'):
                worst = max(worst, abs(f(ra, k) - mb[k]))
    ok = not missing and worst <= 0.01
    print(f'BUMP_SAME {"PASS" if ok else "FAIL"} steps={len(set(steps) & set(other))} flipped={flips} maxPosDiff={worst:.4f} missing={missing}')
print(f'BUMP steps={len(steps)}')
sys.exit(0 if ok else 1)
