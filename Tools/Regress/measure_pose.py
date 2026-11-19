#!/usr/bin/env python3
# 봇 판정 측정 (candidate1-impl-plan.md 7단계) — 서버 + 지연 중계 + 화면 역할 봇을 조건마다 돌려 오탈 · 비용 · 바이트를 표로 남긴다
#   python measure_pose.py <출력폴더> <클립 파일> pilot   = 왕복 100 + 흔들림 33, 퍼짐 끔 1판
#   python measure_pose.py <출력폴더> <클립 파일> full    = 합격 5조건 + 값만 2조건, 퍼짐 끔 · 켬 각 1판
#   python measure_pose.py <출력폴더> <클립 파일> extra <조건,조건,...> = 분모가 모자란 조건(퍼짐 켬)만 더 (MEASURE_SEED_BASE 를 바꿔 준다)
#     조건 이름에 _nospread 를 붙이면 퍼짐 끔 판을 더한다(C-5). 그냥 이름 · _spread 는 퍼짐 켬
#   python measure_pose.py <출력폴더> <클립 파일> cost    = 로그 없이 지연 없이 10명 연사 1판 (workUs · 바이트). PC 가 조용할 때
#
# 수치 근거
#  - 중계 --delayMs 는 방향마다 걸리므로 왕복의 절반을 준다(measure_rewind.py 와 같다)
#  - 조건은 플랜 7단계 그대로: 합격 = 왕복 20 · 100 · 150 고정, 100 + 흔들림 33, 20 + 흔들림 100. 값만 = 100 + 흔들림 66 · 100
#  - 조건당 600발 이상이 플랜 값이다. 봇이 서로 자주 못 봐서 한 판 분당 머리 30~79발뿐이라(파일럿 실측) 같은 조건을 MEASURE_GAMES 판 동시에 돌려 합산한다
#  - 비용(cost)은 시간을 재므로 한 판만, --judgeFireAlways 로 10명이 쏠 수 있을 때 계속 쏜다. 무기 셋의 탄이 약 35초치라 MEASURE_SECONDS 는 30 이하로 준다
#  - 17-d: 「상대가 반응 중」 머리 · 몸(react*) 열 — 반응은 근접에 맞을 때만 켜져 MEASURE_BOT_EXTRA 에 --melee 를 줘야 분모가 생긴다
#  - 컨텐츠 D-6: 「상대가 대시 중」 머리 · 몸(dash*) 열 — MEASURE_BOT_EXTRA 에 --dash 를 줘야 분모가 생긴다. 서버 --clips(이 도구가 늘 준다)라야 대시 몽타주가 실린다
#  - 9-6: 「상대가 몽타주 중」 머리 · 몸(mont*) 열을 더 낸다. MEASURE_SERVER_EXTRA = 서버에 더 줄 인자(일부러 틀림 --rewindMontageShift=1 확인용)
import concurrent.futures
import csv
import os
import re
import subprocess
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SERVER = os.path.join(REPO, 'build', 'Server', 'Release', 'Server.exe')
RELAY = os.path.join(REPO, 'build', 'Tools', 'LagRelay', 'Release', 'LagRelay.exe')
BOT = os.path.join(REPO, 'build', 'BotClient', 'Release', 'BotClient.exe')
CHECK = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'check_bot_hits.py')
MAP = r'C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra\Saved\MOB\map_expanse.mocl'
BOTS = 10
SECONDS = int(os.environ.get('MEASURE_SECONDS', '90'))
SECONDS_NOSPREAD = int(os.environ.get('MEASURE_SECONDS_NOSPREAD', str(SECONDS)))	# 퍼짐을 끄면 머리 분모가 빨리 찬다(파일럿 5판 60초 머리 266~549 vs 켬 144~203, 실측)
LEAD = os.environ.get('MEASURE_LEAD', '3')
GAMES = int(os.environ.get('MEASURE_GAMES', '5'))
SEED_BASE = int(os.environ.get('MEASURE_SEED_BASE', '20261102'))		# 분모가 모자란 조건을 더 돌릴 때 앞 판과 다른 시드를 준다
SERVER_EXTRA = os.environ.get('MEASURE_SERVER_EXTRA', '').split()
BOT_EXTRA = os.environ.get('MEASURE_BOT_EXTRA', '').split()		# 봇에 더 줄 인자 — C-5 조준 측정은 --ads


def num(text, key):
    m = re.search(r'\b' + key + r'=(-?[\d.]+)', text)
    return float(m.group(1)) if m else None


def run_one(out_dir, clips, label, port, rtt, jitter, no_spread, logs=True, seed=20261102, fire_always=False):
    seconds = SECONDS_NOSPREAD if no_spread else SECONDS
    srv_path = os.path.join(out_dir, f'{label}_server.txt')
    bot_path = os.path.join(out_dir, f'{label}_bot.txt')
    judge_path = os.path.join(out_dir, f'{label}_judge.txt')
    srv_args = [SERVER, f'--map={MAP}', f'--port={port}', '--exitWhenEmpty', '--warmup=0', '--matchSeconds=100000', '--targetScore=100000', f'--clips={clips}', f'--seed={seed}'] + SERVER_EXTRA
    bot_args = [BOT, f'--map={MAP}', f'--port={port + 1 if rtt or jitter else port}', f'--bots={BOTS}', f'--seconds={seconds}', f'--lead={LEAD}', f'--judgeClips={clips}', f'--seed={seed}'] + BOT_EXTRA
    if fire_always:
        bot_args.append('--judgeFireAlways')
    if logs:
        srv_args.append('--rewindBulletLog')
        bot_args.append(f'--judgeLog={judge_path}')
    if no_spread:
        srv_args.append('--rewindNoSpread')
        bot_args.append('--judgeNoSpread')
    relay = None
    with open(srv_path, 'wb') as srv_out, open(bot_path, 'wb') as bot_out, open(os.devnull, 'wb') as null:
        srv = subprocess.Popen(srv_args, stdout=srv_out, stderr=subprocess.STDOUT)
        # 서버가 듣기 시작할 때까지 기다린다 — 클립 묶음 v3(29.6MB)를 읽느라 5판 동시면 7.75초 걸렸다(C-5 실측). 듣기 전에 중계가 서버로 이으려 하면
        # 중계는 그 봇 연결을 말없이 닫고(LagRelay main.cpp connect 실패 갈래) 봇은 붙었다고 믿어 한 명이 빠진다(9단계 M-9, C-5 파일럿 10판 모두)
        deadline = time.time() + 60
        while time.time() < deadline and srv.poll() is None:
            if 'Server started' in open(srv_path, encoding='utf-8', errors='replace').read():
                break
            time.sleep(0.1)
        time.sleep(0.5)
        if rtt or jitter:
            relay = subprocess.Popen([RELAY, f'--listen={port + 1}', f'--target={port}', f'--delayMs={rtt // 2}', f'--jitterMs={jitter}'], stdout=null, stderr=subprocess.STDOUT)
            time.sleep(0.3)
        bot = subprocess.Popen(bot_args, stdout=bot_out, stderr=subprocess.STDOUT)
        bot.wait(timeout=seconds + 120)
        try:
            srv.wait(timeout=30)
        except subprocess.TimeoutExpired:
            srv.kill()
            srv.wait()
        if relay:
            relay.kill()
            relay.wait()
    srv_text = open(srv_path, encoding='utf-8', errors='replace').read()
    bot_text = open(bot_path, encoding='utf-8', errors='replace').read()
    row = dict(label=label, rtt=rtt, jitter=jitter, spread=0 if no_spread else 1, botRc=bot.returncode, serverRc=srv.returncode)
    judge_line = next((line for line in bot_text.splitlines() if line.startswith('JUDGE ')), '')		# PROGRESS 줄에도 shots= 가 있어 끝줄만 읽는다
    for key in ('shots', 'bullets', 'noView', 'noSnap', 'noAnim', 'clampedThinks', 'recvBytesPerBotPerSec'):
        row[key] = num(judge_line, key)
    for key in ('rewindClamped', 'rewindOk', 'rewindFuture', 'rewindNoView', 'idleMoves', 'kills', 'stateBytesMax', 'poseEvals', 'poseReused', 'poseSkipped'):
        row[key] = num(srv_text, key)
    m = re.search(r'workUs p50=([\d.]+) p99=([\d.]+) max=([\d.]+)', srv_text)
    if m:
        row['workP50'], row['workP99'], row['workMax'] = (float(x) for x in m.groups())
    row['botPass'] = 'RESULT PASS' in bot_text
    if logs:
        res = subprocess.run([sys.executable, CHECK, judge_path, srv_path], capture_output=True, encoding='utf-8', errors='replace')
        open(os.path.join(out_dir, f'{label}_hits.txt'), 'w', encoding='utf-8').write(res.stdout + res.stderr)
        row['hitsPass'] = res.returncode == 0
        for name, rx in (('head', r'^머리: 분모 (\d+), 오탈 (\d+)'), ('body', r'^몸: 분모 (\d+), 오탈 (\d+)'), ('headRun', r'^달리는 상대 머리: 분모 (\d+), 오탈 (\d+)'), ('bodyRun', r'^달리는 상대 몸: 분모 (\d+), 오탈 (\d+)'),
                         ('montHead', r'^몽타주 중 머리: 분모 (\d+), 오탈 (\d+)'), ('montBody', r'^몽타주 중 몸: 분모 (\d+), 오탈 (\d+)'),
                         ('adsHead', r'^조준 중 상대 머리: 분모 (\d+), 오탈 (\d+)'), ('adsBody', r'^조준 중 상대 몸: 분모 (\d+), 오탈 (\d+)'),
                         ('crouchHead', r'^앉은 상대 머리: 분모 (\d+), 오탈 (\d+)'), ('crouchBody', r'^앉은 상대 몸: 분모 (\d+), 오탈 (\d+)'),
                         ('reactHead', r'^반응 중 상대 머리: 분모 (\d+), 오탈 (\d+)'), ('reactBody', r'^반응 중 상대 몸: 분모 (\d+), 오탈 (\d+)'),
                         ('dashHead', r'^대시 중 상대 머리: 분모 (\d+), 오탈 (\d+)'), ('dashBody', r'^대시 중 상대 몸: 분모 (\d+), 오탈 (\d+)')):
            mm = re.search(rx, res.stdout, re.M)
            row[name + 'N'], row[name + 'Lost'] = (int(mm.group(1)), int(mm.group(2))) if mm else (None, None)
            n = row[name + 'N']
            row[name + 'Rate'] = 100.0 * row[name + 'Lost'] / n if n else None
        mm = re.search(r'봇만 (\d+), 서버만 (\d+)', res.stdout)
        if mm:
            row['botOnly'], row['serverOnly'] = int(mm.group(1)), int(mm.group(2))
        mm = re.search(r'이미 죽은 상대라 뺀 탄: (\d+)', res.stdout)
        if mm:
            row['deadExcluded'] = int(mm.group(1))
        mm = re.search(r'이미 나간 상대라 뺀 탄: (\d+)', res.stdout)
        if mm:
            row['leftExcluded'] = int(mm.group(1))
        mm = re.search(r'줍기 소식 전 옛 무기로 쏜 탄이라 뺀 탄: (\d+)', res.stdout)
        if mm:
            row['pickupExcluded'] = int(mm.group(1))
    return row


def fmt(v):
    if v is None:
        return '-'
    if isinstance(v, bool):
        return '1' if v else '0'
    if isinstance(v, float):
        return f'{v:.3f}' if abs(v) < 100 else f'{v:.0f}'
    return str(v)


COLUMNS = ['label', 'rtt', 'jitter', 'spread', 'hitsPass', 'headN', 'headLost', 'headRate', 'bodyN', 'bodyLost', 'bodyRate', 'headRunN', 'headRunLost', 'headRunRate', 'bodyRunN', 'bodyRunLost', 'bodyRunRate',
           'montHeadN', 'montHeadLost', 'montHeadRate', 'montBodyN', 'montBodyLost', 'montBodyRate',
           'adsHeadN', 'adsHeadLost', 'adsHeadRate', 'adsBodyN', 'adsBodyLost', 'adsBodyRate',
           'crouchHeadN', 'crouchHeadLost', 'crouchHeadRate', 'crouchBodyN', 'crouchBodyLost', 'crouchBodyRate',
           'reactHeadN', 'reactHeadLost', 'reactHeadRate', 'reactBodyN', 'reactBodyLost', 'reactBodyRate',
           'dashHeadN', 'dashHeadLost', 'dashHeadRate', 'dashBodyN', 'dashBodyLost', 'dashBodyRate',
           'botOnly', 'serverOnly', 'deadExcluded', 'leftExcluded', 'pickupExcluded', 'shots', 'bullets', 'noView', 'noSnap', 'noAnim', 'clampedThinks', 'rewindClamped', 'rewindOk', 'rewindFuture', 'rewindNoView', 'idleMoves', 'kills',
           'workP50', 'workP99', 'workMax', 'recvBytesPerBotPerSec', 'stateBytesMax', 'poseEvals', 'poseReused', 'poseSkipped', 'botPass', 'botRc', 'serverRc']

SUMMED = ['headN', 'headLost', 'bodyN', 'bodyLost', 'headRunN', 'headRunLost', 'bodyRunN', 'bodyRunLost', 'montHeadN', 'montHeadLost', 'montBodyN', 'montBodyLost', 'adsHeadN', 'adsHeadLost', 'adsBodyN', 'adsBodyLost', 'crouchHeadN', 'crouchHeadLost', 'crouchBodyN', 'crouchBodyLost', 'reactHeadN', 'reactHeadLost', 'reactBodyN', 'reactBodyLost', 'dashHeadN', 'dashHeadLost', 'dashBodyN', 'dashBodyLost', 'botOnly', 'serverOnly', 'deadExcluded', 'leftExcluded', 'pickupExcluded', 'shots', 'bullets', 'noView', 'noSnap', 'noAnim', 'clampedThinks',
          'rewindClamped', 'rewindOk', 'rewindFuture', 'rewindNoView', 'idleMoves', 'kills', 'poseEvals', 'poseReused', 'poseSkipped']


def combine(rows, label, limit=0.5):
    """같은 조건의 여러 판을 하나로 — 분모와 오탈은 더하고, 시간 · 바이트의 최대는 최대로, 판정은 합친 값으로 다시 한다."""
    out = dict(rows[0])
    out['label'] = label
    for key in SUMMED:
        values = [r.get(key) for r in rows]
        out[key] = sum(v for v in values if v is not None) if any(v is not None for v in values) else None
    for key in ('workMax', 'workP99', 'stateBytesMax', 'recvBytesPerBotPerSec'):
        values = [r[key] for r in rows if r.get(key) is not None]
        out[key] = max(values) if values else None
    values = [r['workP50'] for r in rows if r.get('workP50') is not None]
    out['workP50'] = sum(values) / len(values) if values else None
    for name in ('head', 'body', 'headRun', 'bodyRun', 'montHead', 'montBody', 'adsHead', 'adsBody', 'crouchHead', 'crouchBody', 'reactHead', 'reactBody', 'dashHead', 'dashBody'):
        n, lost = out.get(name + 'N'), out.get(name + 'Lost')
        out[name + 'Rate'] = 100.0 * lost / n if n else None
    out['hitsPass'] = bool(out.get('headN')) and bool(out.get('bodyN')) and out['headRate'] <= limit and out['bodyRate'] <= limit
    out['botPass'] = all(r.get('botPass') for r in rows)
    out['botRc'] = max(r['botRc'] for r in rows)
    out['serverRc'] = max(r['serverRc'] for r in rows)
    return out


def run_cond(out_dir, clips, label, base_port, rtt, jitter, no_spread, logs, games, fire_always=False):
    if games == 1:
        return run_one(out_dir, clips, label, base_port, rtt, jitter, no_spread, logs, fire_always=fire_always)
    with concurrent.futures.ThreadPoolExecutor(max_workers=games) as pool:
        futures = [pool.submit(run_one, out_dir, clips, f'{label}_g{k}', base_port + k * 2, rtt, jitter, no_spread, logs, SEED_BASE + k * 1000) for k in range(games)]
        rows = [f.result() for f in futures]
    return combine(rows, label)


COND_PASS = [('r20', 20, 0), ('r100', 100, 0), ('r150', 150, 0), ('r100j33', 100, 33), ('r20j100', 20, 100)]
COND_VALUE = [('r100j66', 100, 66), ('r100j100', 100, 100)]


def main():
    out_dir, clips, mode = sys.argv[1], sys.argv[2], sys.argv[3]
    games = GAMES
    os.makedirs(out_dir, exist_ok=True)
    if mode == 'pilot':
        plan = [('pilot_nospread', 100, 33, True, True), ('pilot_spread', 100, 33, False, True)]
    elif mode == 'extra':
        names = sys.argv[4].split(',')
        plan = []
        for name, rtt, jitter in COND_PASS + COND_VALUE:
            if name + '_nospread' in names:
                plan.append((name + '_nospread', rtt, jitter, True, True))
            if name in names or name + '_spread' in names:
                plan.append((name + '_spread', rtt, jitter, False, True))
    elif mode == 'cost':
        plan = [('cost', 0, 0, False, False)]
        games = 1
    else:
        plan = []
        for name, rtt, jitter in COND_PASS + COND_VALUE:
            plan.append((name + '_nospread', rtt, jitter, True, True))
            plan.append((name + '_spread', rtt, jitter, False, True))
    csv_path = os.path.join(out_dir, f'pose_{mode}.csv')
    with open(csv_path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, COLUMNS, extrasaction='ignore')
        w.writeheader()
        f.flush()
        for n, (label, rtt, jitter, no_spread, logs) in enumerate(plan):
            row = run_cond(out_dir, clips, label, 8100 + n * 20, rtt, jitter, no_spread, logs, games, fire_always=mode == 'cost')
            w.writerow({k: fmt(row.get(k)) for k in COLUMNS})
            f.flush()
            print(' '.join(f'{k}={fmt(row.get(k))}' for k in COLUMNS), flush=True)
    print('MEASURE DONE', csv_path, flush=True)


if __name__ == '__main__':
    main()
