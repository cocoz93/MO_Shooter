#!/usr/bin/env python3
# 봇 화면 판정 <-> 서버 판정 대조 (candidate1-impl-plan.md 7단계, D11 · D33)
#   python check_bot_hits.py <봇 --judgeLog 파일> <서버 출력> [--run=500] [--limit=0.5]
# 서버는 --rewindBulletLog, 봇은 --judgeClips · --judgeLog 로 돌린 판이어야 한다.
# - 머리 = 봇이 약점에 맞힌 탄 기준, 서버에서 같은 상대의 약점이 아니면 오탈
# - 몸 = 봇이 어디든 맞힌 탄 기준, 서버에서 같은 상대를 안 맞혔으면 오탈(check_view_pose.py 와 같은 정의)
# - 덤 = 봇은 못 맞혔는데 서버가 맞힌 탄. 합격선에는 안 넣고 값만 낸다
# - 서버가 쏠 때 이미 죽어 있던 상대(DEADSKIP)를 봇이 맞힌 탄은 경기 규칙이라 빼고 따로 센다. 0단계 계산기에도 없는 사건이다
# - 서버가 쏠 때 이미 나간 상대(그 탄보다 앞선 LEAVE 줄)도 같은 까닭으로 빼고 따로 센다(C-5: 측정 판에서는 판 끝에 봇이 한꺼번에 끊을 때 생긴다)
# - 달리는 상대 = 봇이 본 수평 속도가 --run 이상(check_dedi_baseline.py RUN_SPEED 와 같은 500cm/s)
# - 상대 무기별 · 「상대가 몽타주 중」(9-6) = 봇이 맞힌 상대의 자세 묶음 · 몽타주 칸 수(BOTSHOT targetWeapon= montages=, 서버 REWIND montages= 와 같은 정의).
#   몽타주 중 쏨도 같은 기준으로 따로 판정한다(BOTHITS_MONTAGE). 옛 봇 기록에는 두 칸이 없어 이 줄들이 빈다
# - 「상대가 조준 중」(C-5) = 봇이 맞힌 상대 자세의 조준 묶음 비중 > 0(BOTSHOT targetAds=). 값만 낸다(BOTHITS_ADS). 옛 기록에는 칸이 없어 빈다
# - 「상대가 앉음」(D-6) = 같은 식의 앉기 묶음 비중 > 0(BOTSHOT targetCrouch=). 값만 낸다(BOTHITS_CROUCH). 봇에 --crouch 를 줘야 분모가 생긴다
# - 「상대가 반응 중」(E-3 17-d) = 맞힌 상대 자세에 맞은 쪽 반응 몽타주가 들었나(BOTSHOT targetReact=, 서버 REWIND react= 와 같은 정의). 값만 낸다(BOTHITS_REACT).
#   반응은 근접에 맞을 때만 켜져 봇에 --melee 를 줘야 분모가 생긴다
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

bot_re = re.compile(r'^BOTSHOT seq=(\d+) shooter=(\d+) i=(\d+) weapon=(\d+) kind=(\d+) target=(\d+) weak=(\d+) viewTick=(\d+) alpha=([\d.]+) speed=([\d.]+)(?: targetWeapon=(\d+) montages=(\d+))?(?: targetAds=([\d.]+))?(?: targetCrouch=([\d.]+))?(?: targetReact=(\d+))?')
srv_re = re.compile(r'^BULLET seq=(\d+) shooter=(\d+) i=(\d+) kind=(\d+) target=(\d+) weak=(\d+) rewind=(\d+)')
dead_re = re.compile(r'^DEADSKIP seq=(\d+) shooter=(\d+) target=(\d+)')
leave_re = re.compile(r'^LEAVE id=(\d+)')

bots = {}
for line in open(args[0], encoding='utf-8', errors='replace'):
    m = bot_re.match(line)
    if m:
        g = m.groups()
        bots[(int(g[1]), int(g[0]), int(g[2]))] = dict(weapon=int(g[3]), kind=int(g[4]), target=int(g[5]), weak=int(g[6]), speed=float(g[9]),
                                                       target_weapon=int(g[10]) if g[10] is not None else None, montages=int(g[11]) if g[11] is not None else None,
                                                       target_ads=float(g[12]) if g[12] is not None else None,
                                                       target_crouch=float(g[13]) if g[13] is not None else None,
                                                       target_react=int(g[14]) if g[14] is not None else None)

server, dead, left = {}, set(), set()
for line in open(args[1], encoding='utf-8', errors='replace'):
    m = srv_re.match(line)
    if m:
        g = m.groups()
        server[(int(g[1]), int(g[0]), int(g[2]))] = dict(kind=int(g[3]), target=int(g[4]), weak=int(g[5]), rewind=int(g[6]), left=frozenset(left))
        continue
    m = dead_re.match(line)
    if m:
        g = m.groups()
        dead.add((int(g[1]), int(g[0]), int(g[2])))
        continue
    m = leave_re.match(line)
    if m:
        left.add(int(m.group(1)))

both = [k for k in bots if k in server]
print(f'봇 탄 {len(bots)}, 서버 탄 {len(server)}, 이은 탄 {len(both)}, 봇만 {len(bots) - len(both)}, 서버만 {len(server) - len(both)}')
rewinds = collections.Counter(server[k]['rewind'] for k in both)
print('서버 시점 종류(0=없음 1=미래 2=한도 3=정상): ' + ' '.join(f'{k}:{v}' for k, v in sorted(rewinds.items())))

zero = lambda: dict(n=0, lost=0)
head, body, head_run, body_run = zero(), zero(), zero(), zero()
per_weapon = collections.defaultdict(lambda: [0, 0, 0, 0])
per_target_weapon = collections.defaultdict(lambda: [0, 0, 0, 0])		# 머리 분모 · 오탈 · 몸 분모 · 오탈
head_mont, body_mont, head_nomont, body_nomont = zero(), zero(), zero(), zero()
head_ads, body_ads, head_hip, body_hip = zero(), zero(), zero(), zero()
head_crouch, body_crouch, head_stand, body_stand = zero(), zero(), zero(), zero()
head_react, body_react, head_noreact, body_noreact = zero(), zero(), zero(), zero()
head_gain = body_gain = dead_excluded = left_excluded = 0
samples = []
for k in both:
    b, s = bots[k], server[k]
    b_hit = b['kind'] == KIND_BODY
    s_hit = s['kind'] == KIND_BODY
    if b_hit and (k[0], k[1], b['target']) in dead:
        dead_excluded += 1
        continue
    if b_hit and b['target'] in s['left']:
        left_excluded += 1
        continue
    if not b_hit:
        if s_hit:
            body_gain += 1
            if s['weak']:
                head_gain += 1
        continue
    running = b['speed'] >= RUN
    same_target = s_hit and s['target'] == b['target']
    has_tag = b['montages'] is not None
    mont_body, mont_head = (body_mont, head_mont) if has_tag and b['montages'] > 0 else (body_nomont, head_nomont)
    tw = per_target_weapon[b['target_weapon']] if has_tag else None
    has_ads = b['target_ads'] is not None
    ads_body, ads_head = (body_ads, head_ads) if has_ads and b['target_ads'] > 0 else (body_hip, head_hip)
    if has_ads:
        ads_body['n'] += 1
        if not same_target:
            ads_body['lost'] += 1
        if b['weak']:
            ads_head['n'] += 1
            if not (same_target and s['weak']):
                ads_head['lost'] += 1
    has_crouch = b['target_crouch'] is not None
    crouch_body, crouch_head = (body_crouch, head_crouch) if has_crouch and b['target_crouch'] > 0 else (body_stand, head_stand)
    if has_crouch:
        crouch_body['n'] += 1
        if not same_target:
            crouch_body['lost'] += 1
        if b['weak']:
            crouch_head['n'] += 1
            if not (same_target and s['weak']):
                crouch_head['lost'] += 1
    has_react = b['target_react'] is not None
    react_body, react_head = (body_react, head_react) if has_react and b['target_react'] > 0 else (body_noreact, head_noreact)
    if has_react:
        react_body['n'] += 1
        if not same_target:
            react_body['lost'] += 1
        if b['weak']:
            react_head['n'] += 1
            if not (same_target and s['weak']):
                react_head['lost'] += 1
    body['n'] += 1
    per_weapon[b['weapon']][2] += 1
    if running:
        body_run['n'] += 1
    if has_tag:
        mont_body['n'] += 1
        tw[2] += 1
    if not same_target:
        body['lost'] += 1
        per_weapon[b['weapon']][3] += 1
        if running:
            body_run['lost'] += 1
        if has_tag:
            mont_body['lost'] += 1
            tw[3] += 1
        if len(samples) < 5:
            samples.append((k, b, s))
    if b['weak']:
        head['n'] += 1
        per_weapon[b['weapon']][0] += 1
        if running:
            head_run['n'] += 1
        if has_tag:
            mont_head['n'] += 1
            tw[0] += 1
        if not (same_target and s['weak']):
            head['lost'] += 1
            per_weapon[b['weapon']][1] += 1
            if running:
                head_run['lost'] += 1
            if has_tag:
                mont_head['lost'] += 1
                tw[1] += 1


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
print(f'서버가 쏠 때 이미 나간 상대라 뺀 탄: {left_excluded}')
WEAPONS = {0: '권총', 1: '소총', 2: '샷건'}
for w, (hn, hl, bn, bl) in sorted(per_weapon.items()):
    print(f'  {WEAPONS.get(w, w)}: 머리 {hl}/{hn}, 몸 {bl}/{bn}')
show('몽타주 중 머리', head_mont)
show('몽타주 중 몸', body_mont)
show('몽타주 없음 머리', head_nomont)
show('몽타주 없음 몸', body_nomont)
for w, (hn, hl, bn, bl) in sorted(per_target_weapon.items()):
    print(f'  상대 {WEAPONS.get(w, w)}: 머리 {hl}/{hn}, 몸 {bl}/{bn}')
show('조준 중 상대 머리', head_ads)
show('조준 중 상대 몸', body_ads)
show('조준 안 한 상대 머리', head_hip)
show('조준 안 한 상대 몸', body_hip)
show('앉은 상대 머리', head_crouch)
show('앉은 상대 몸', body_crouch)
show('선 상대 머리', head_stand)
show('선 상대 몸', body_stand)
show('반응 중 상대 머리', head_react)
show('반응 중 상대 몸', body_react)
show('반응 없는 상대 머리', head_noreact)
show('반응 없는 상대 몸', body_noreact)
for k, b, s in samples:
    print(f'  오탈 보기 {k}: 봇 {b} 서버 {s}')

ok = head['n'] > 0 and body['n'] > 0 and rate(head) <= LIMIT and rate(body) <= LIMIT
print(f'BOTHITS {"PASS" if ok else "FAIL"} 머리={rate(head):.3f} 몸={rate(body):.3f} 달리는머리={rate(head_run):.3f} 달리는몸={rate(body_run):.3f} 기준={LIMIT}')
# 몽타주 중 판정은 값만 낸다(rc 에는 안 넣는다) — 분모가 600 에 차는지는 measure_pose.py 가 조건마다 합쳐 본다
mont_ok = head_mont['n'] > 0 and body_mont['n'] > 0 and rate(head_mont) <= LIMIT and rate(body_mont) <= LIMIT
print(f'BOTHITS_MONTAGE {"PASS" if mont_ok else "FAIL"} 머리={rate(head_mont):.3f}({head_mont["lost"]}/{head_mont["n"]}) 몸={rate(body_mont):.3f}({body_mont["lost"]}/{body_mont["n"]}) 기준={LIMIT}')
# 조준 중 상대(C-5)도 값만 — 봇에 --ads 를 줘야 분모가 생긴다
ads_ok = head_ads['n'] > 0 and body_ads['n'] > 0 and rate(head_ads) <= LIMIT and rate(body_ads) <= LIMIT
print(f'BOTHITS_ADS {"PASS" if ads_ok else "FAIL"} 머리={rate(head_ads):.3f}({head_ads["lost"]}/{head_ads["n"]}) 몸={rate(body_ads):.3f}({body_ads["lost"]}/{body_ads["n"]}) 기준={LIMIT}')
# 앉은 상대(D-6)도 값만 — 봇에 --crouch 를 줘야 분모가 생긴다
crouch_ok = head_crouch['n'] > 0 and body_crouch['n'] > 0 and rate(head_crouch) <= LIMIT and rate(body_crouch) <= LIMIT
print(f'BOTHITS_CROUCH {"PASS" if crouch_ok else "FAIL"} 머리={rate(head_crouch):.3f}({head_crouch["lost"]}/{head_crouch["n"]}) 몸={rate(body_crouch):.3f}({body_crouch["lost"]}/{body_crouch["n"]}) 기준={LIMIT}')
# 반응 중 상대(E-3 17-d)도 값만 — 봇에 --melee 를 줘야 분모가 생긴다
react_ok = head_react['n'] > 0 and body_react['n'] > 0 and rate(head_react) <= LIMIT and rate(body_react) <= LIMIT
print(f'BOTHITS_REACT {"PASS" if react_ok else "FAIL"} 머리={rate(head_react):.3f}({head_react["lost"]}/{head_react["n"]}) 몸={rate(body_react):.3f}({body_react["lost"]}/{body_react["n"]}) 기준={LIMIT}')
sys.exit(0 if ok else 1)
