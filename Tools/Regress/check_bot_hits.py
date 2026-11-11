#!/usr/bin/env python3
# 봇 화면 판정 <-> 서버 판정 대조 (candidate1-impl-plan.md 7단계, D11 · D33)
#   python check_bot_hits.py <봇 --judgeLog 파일> <서버 출력> [--run=500] [--limit=0.5]
# 서버는 --rewindBulletLog, 봇은 --judgeClips · --judgeLog 로 돌린 판이어야 한다.
# - 머리 = 봇이 약점에 맞힌 탄 기준, 서버에서 같은 상대의 약점이 아니면 오탈
# - 몸 = 봇이 어디든 맞힌 탄 기준, 서버에서 같은 상대를 안 맞혔으면 오탈(check_view_pose.py 와 같은 정의)
# - 덤 = 봇은 못 맞혔는데 서버가 맞힌 탄. 합격선에는 안 넣고 값만 낸다
# - 서버가 쏠 때 이미 죽어 있던 상대(DEADSKIP)를 봇이 맞힌 탄은 경기 규칙이라 빼고 따로 센다. 0단계 계산기에도 없는 사건이다
# - 달리는 상대 = 봇이 본 수평 속도가 --run 이상(check_dedi_baseline.py RUN_SPEED 와 같은 500cm/s)
# rc=0 기준 안 · rc=1 기준 밖이거나 분모가 0 (0 은 「맞다」가 아니라 「못 읽었다」)
import collections
import re
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict((a[2:].split('=', 1) + [''])[:2] for a in sys.argv[1:] if a.startswith('--'))
RUN = float(opt.get('run', 500))
LIMIT = float(opt.get('limit', 0.5))
KIND_BODY = 2   # Shared/Weapon.h EBulletHit::Body

bot_re = re.compile(r'^BOTSHOT seq=(\d+) shooter=(\d+) i=(\d+) weapon=(\d+) kind=(\d+) target=(\d+) weak=(\d+) viewTick=(\d+) alpha=([\d.]+) speed=([\d.]+)')
srv_re = re.compile(r'^BULLET seq=(\d+) shooter=(\d+) i=(\d+) kind=(\d+) target=(\d+) weak=(\d+) rewind=(\d+)')
dead_re = re.compile(r'^DEADSKIP seq=(\d+) shooter=(\d+) target=(\d+)')

bots = {}
for line in open(args[0], encoding='utf-8', errors='replace'):
    m = bot_re.match(line)
    if m:
        g = m.groups()
        bots[(int(g[1]), int(g[0]), int(g[2]))] = dict(weapon=int(g[3]), kind=int(g[4]), target=int(g[5]), weak=int(g[6]), speed=float(g[9]))

server, dead = {}, set()
for line in open(args[1], encoding='utf-8', errors='replace'):
    m = srv_re.match(line)
    if m:
        g = m.groups()
        server[(int(g[1]), int(g[0]), int(g[2]))] = dict(kind=int(g[3]), target=int(g[4]), weak=int(g[5]), rewind=int(g[6]))
        continue
    m = dead_re.match(line)
    if m:
        g = m.groups()
        dead.add((int(g[1]), int(g[0]), int(g[2])))

both = [k for k in bots if k in server]
print(f'봇 탄 {len(bots)}, 서버 탄 {len(server)}, 이은 탄 {len(both)}, 봇만 {len(bots) - len(both)}, 서버만 {len(server) - len(both)}')
rewinds = collections.Counter(server[k]['rewind'] for k in both)
print('서버 시점 종류(0=없음 1=미래 2=한도 3=정상): ' + ' '.join(f'{k}:{v}' for k, v in sorted(rewinds.items())))

zero = lambda: dict(n=0, lost=0)
head, body, head_run, body_run = zero(), zero(), zero(), zero()
per_weapon = collections.defaultdict(lambda: [0, 0, 0, 0])
head_gain = body_gain = dead_excluded = 0
samples = []
for k in both:
    b, s = bots[k], server[k]
    b_hit = b['kind'] == KIND_BODY
    s_hit = s['kind'] == KIND_BODY
    if b_hit and (k[0], k[1], b['target']) in dead:
        dead_excluded += 1
        continue
    if not b_hit:
        if s_hit:
            body_gain += 1
            if s['weak']:
                head_gain += 1
        continue
    running = b['speed'] >= RUN
    same_target = s_hit and s['target'] == b['target']
    body['n'] += 1
    per_weapon[b['weapon']][2] += 1
    if running:
        body_run['n'] += 1
    if not same_target:
        body['lost'] += 1
        per_weapon[b['weapon']][3] += 1
        if running:
            body_run['lost'] += 1
        if len(samples) < 5:
            samples.append((k, b, s))
    if b['weak']:
        head['n'] += 1
        per_weapon[b['weapon']][0] += 1
        if running:
            head_run['n'] += 1
        if not (same_target and s['weak']):
            head['lost'] += 1
            per_weapon[b['weapon']][1] += 1
            if running:
                head_run['lost'] += 1


def rate(c):
    return 100.0 * c['lost'] / c['n'] if c['n'] else 0.0


def show(name, c):
    print(f'{name}: 분모 {c["n"]}, 오탈 {c["lost"]}, 100발당 {rate(c):.3f}')


show('머리', head)
show('몸', body)
show('달리는 상대 머리', head_run)
show('달리는 상대 몸', body_run)
print(f'덤(봇 못 맞힘 · 서버 맞힘): 몸 {body_gain}, 그중 머리 {head_gain}')
print(f'서버가 쏠 때 이미 죽은 상대라 뺀 탄: {dead_excluded}')
WEAPONS = {0: '권총', 1: '소총', 2: '샷건'}
for w, (hn, hl, bn, bl) in sorted(per_weapon.items()):
    print(f'  {WEAPONS.get(w, w)}: 머리 {hl}/{hn}, 몸 {bl}/{bn}')
for k, b, s in samples:
    print(f'  오탈 보기 {k}: 봇 {b} 서버 {s}')

ok = head['n'] > 0 and body['n'] > 0 and rate(head) <= LIMIT and rate(body) <= LIMIT
print(f'BOTHITS {"PASS" if ok else "FAIL"} 머리={rate(head):.3f} 몸={rate(body):.3f} 달리는머리={rate(head_run):.3f} 달리는몸={rate(body_run):.3f} 기준={LIMIT}')
sys.exit(0 if ok else 1)
