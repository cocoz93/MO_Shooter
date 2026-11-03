#!/usr/bin/env python3
# 되감기 측정 — 서버 + 지연 중계 + 측정 봇(--measureView)을 조합마다 돌려 명중률을 표로 남긴다. 플랜 rewind-plan.md R5
#   python measure_rewind.py <출력폴더> pilot   = 왕복 100 1회
#   python measure_rewind.py <출력폴더> full    = 왕복 0·50·100·150·200·300 각 3회 + 대조군 + 흔들림
#
# 수치 근거
#  - 중계 --delayMs 는 방향마다 걸리므로 왕복의 절반을 준다
#  - 봇 10명 60초 3회 = 플랜 값. 한 판 탄 수가 수천 발이라 3%포인트 차이를 가를 만큼 분모가 나오는지는 파일럿으로 먼저 본다
#  - 시간 지표는 안 쓴다(다른 세션이 녹스를 돌리는 중). 봇이 멈추면 어긋나므로 idleMoves 를 같이 적어 튄 판을 표시한다
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
MAP = r'C:\Users\USER\Documents\Unreal Projects\LyraStarterGame\Saved\MOB\map_expanse.mocl'
BOTS = 10
SECONDS = 60
LEAD = os.environ.get('MEASURE_LEAD', '3')


def num(text, key):
    m = re.search(r'\b' + key + r'=(-?[\d.]+)', text)
    return float(m.group(1)) if m else None


def run_one(out_dir, label, port, rtt, limit=None, jitter=0):
    srv_path = os.path.join(out_dir, f'{label}_server.txt')
    bot_path = os.path.join(out_dir, f'{label}_bot.txt')
    srv_args = [SERVER, f'--map={MAP}', f'--port={port}', '--exitWhenEmpty', '--warmup=0', '--matchSeconds=100000', '--targetScore=100000', '--rewindShadow']
    if limit is not None:
        srv_args.append(f'--rewindLimitTicks={limit}')
    if os.environ.get('MEASURE_NOSPREAD') == '1':
        srv_args.append('--rewindNoSpread')  # 진단 전용 — 파일럿의 켬·끔이 같게 나온 까닭이 무기 퍼짐인지 가른다
    with open(srv_path, 'wb') as srv_out, open(bot_path, 'wb') as bot_out, open(os.devnull, 'wb') as null:
        srv = subprocess.Popen(srv_args, stdout=srv_out, stderr=subprocess.STDOUT)
        time.sleep(0.5)
        relay = subprocess.Popen([RELAY, f'--listen={port + 1}', f'--target={port}', f'--delayMs={rtt // 2}', f'--jitterMs={jitter}'], stdout=null, stderr=subprocess.STDOUT)
        time.sleep(0.3)
        bot = subprocess.Popen([BOT, f'--map={MAP}', f'--port={port + 1}', f'--bots={BOTS}', f'--seconds={SECONDS}', f'--lead={LEAD}', '--measureView'], stdout=bot_out, stderr=subprocess.STDOUT)
        bot.wait(timeout=SECONDS + 120)
        try:
            srv.wait(timeout=30)
        except subprocess.TimeoutExpired:
            srv.kill()
            srv.wait()
        relay.kill()
        relay.wait()
    srv_text = open(srv_path, encoding='utf-8', errors='replace').read()
    bot_text = open(bot_path, encoding='utf-8', errors='replace').read()
    row = {'label': label, 'rtt': rtt, 'limit': 18 if limit is None else limit, 'jitter': jitter, 'botRc': bot.returncode, 'serverRc': srv.returncode}
    for key in ('rewindNoView', 'rewindFuture', 'rewindClamped', 'rewindOk', 'rewindTicksSum', 'hitsBoth', 'hitsRewindOnly', 'hitsNowOnly', 'hitsNeither', 'rewindOnlyBehindWall', 'shots', 'bullets', 'idleMoves', 'starvedTicks'):
        row[key] = num(srv_text, key)
    row['ackLagAvg'] = num(bot_text, 'ackLagAvg')
    row['maxAckLag'] = num(bot_text, 'maxAckLag')
    row['botPass'] = 'RESULT PASS' in bot_text
    bullets = row['bullets'] or 0
    both = row['hitsBoth'] or 0
    rewind_only = row['hitsRewindOnly'] or 0
    now_only = row['hitsNowOnly'] or 0
    row['hitOn'] = (both + rewind_only) / bullets if bullets else None
    row['hitOff'] = (both + now_only) / bullets if bullets else None
    on = both + rewind_only
    row['wallRate'] = (row['rewindOnlyBehindWall'] or 0) / on if on else None
    shots = row['shots'] or 0
    total = sum(row[k] or 0 for k in ('rewindNoView', 'rewindFuture', 'rewindClamped', 'rewindOk'))
    row['kindSumOk'] = total == shots
    history = (row['rewindClamped'] or 0) + (row['rewindOk'] or 0)
    row['ageAvg'] = (row['rewindTicksSum'] or 0) / history if history else None
    return row


def fmt(v):
    if v is None:
        return '-'
    if isinstance(v, float):
        return f'{v:.4f}' if abs(v) < 1 else f'{v:.0f}' if v == int(v) else f'{v:.2f}'
    return str(v)


def main():
    out_dir = sys.argv[1]
    mode = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    if mode == 'pilot':
        plan = [('p100', 100, None, 0)]
    else:
        plan = []
        for rtt in (0, 50, 100, 150, 200, 300):
            for i in range(3):
                plan.append((f'r{rtt}_{i + 1}', rtt, None, 0))
        plan.append(('r100_limit0', 100, 0, 0))
        plan.append(('r100_jitter33', 100, None, 33))
    columns = ['label', 'rtt', 'limit', 'jitter', 'bullets', 'hitOn', 'hitOff', 'wallRate', 'hitsBoth', 'hitsRewindOnly', 'hitsNowOnly', 'hitsNeither', 'rewindOnlyBehindWall', 'shots', 'rewindNoView', 'rewindFuture', 'rewindClamped', 'rewindOk', 'ageAvg', 'kindSumOk', 'idleMoves', 'starvedTicks', 'ackLagAvg', 'maxAckLag', 'botPass', 'botRc', 'serverRc']
    csv_path = os.path.join(out_dir, f'rewind_{mode}.csv')
    with open(csv_path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, columns, extrasaction='ignore')
        w.writeheader()
        f.flush()
        for n, (label, rtt, limit, jitter) in enumerate(plan):
            row = run_one(out_dir, label, 7960 + (n % 10) * 2, rtt, limit, jitter)
            w.writerow({k: fmt(row.get(k)) for k in columns})
            f.flush()
            print(' '.join(f'{k}={fmt(row.get(k))}' for k in columns), flush=True)
    print('MEASURE DONE', csv_path, flush=True)


if __name__ == '__main__':
    main()
