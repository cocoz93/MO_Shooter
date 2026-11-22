#!/usr/bin/env python3
# 클립 파일 대조 — Shared/AnimClips 가 읽은 값(AnimCheck clips --dump)을 파일을 만들 때 쓴 csv 와 전수 대조한다 (candidate1-impl-plan.md 1단계, 9단계 버전 2).
#   python check_clip_pack.py <dump.csv> --profiles=<blendprofiles.csv> <묶음 이름>=<clips120.csv> [<묶음 이름>=<clips120.csv> ...]
#   묶음 이름과 차례는 make_clip_pack.py 에 준 것과 같게 준다. 덤프의 BUNDLE 줄이 그 차례 · 이름과 다르면 diff 로 센다.
# 비교는 float 비트로 한다. csv 값을 float 로 줄인 것과 읽은 값이 한 비트라도 다르면 diff 로 센다.
# 뼈대 · 메시 자리 · 부위는 첫 묶음 csv 와 대조한다(make_clip_pack.py 가 묶음끼리 같은지 이미 막는다).
# 9단계 3-a — 뼈별 비중 표(PROFILE)는 비중 표 csv 와, 몽타주 표(MONTAGE · TRACK · MCURVE)와 덮기 자세(OVERRIDE)는 묶음 csv 와 대조한다.
#   끼움 자리(SLOT)는 csv 에서 온 값이 아니라(make_clip_pack.py 의 SLOTS 표) 여기선 이름이 겹치지 않는지 · 비중 표 이름이 있는지 · 줄이 가리키는 자리가 있는지만 본다
import struct
import sys

sys.stdout.reconfigure(encoding='utf-8')
PACK_CURVES = ('DisableLHandIK', 'DisableLegIK')    # make_clip_pack.py 와 같게 — 이 이름의 곡선만 파일에 들어간다


def bits(x):
    return struct.pack('<f', float(x))


def same(a, b):
    return bits(a) == bits(b)


args = sys.argv[2:]
profile_src = None
for a in list(args):
    if a.startswith('--profiles='):
        profile_src = a.split('=', 1)[1]
        args.remove(a)
if len(sys.argv) < 3 or profile_src is None or not args or any('=' not in a or a.startswith('--') for a in args):
    print('사용: check_clip_pack.py <dump.csv> --profiles=<blendprofiles.csv> <묶음 이름>=<clips120.csv> ...')
    sys.exit(2)
dump_path = sys.argv[1]
pairs = [a.split('=', 1) for a in args]
KINDS = {'sphere': 0, 'sphyl': 1, 'box': 2}

profiles_src = {}
for line in open(profile_src, encoding='utf-8-sig', errors='replace'):
    c = line.rstrip('\r\n').split(',')
    if c[0] == 'profile' or len(c) < 4:
        continue
    profiles_src.setdefault(c[0], dict(mode=c[1], scale={}))['scale'][c[2]] = c[3]


def parse(src_path):
    s = dict(names={}, parent={}, ref={}, clips={}, marks={}, curves={}, keys={}, aim=None, move=None, mesh=None, bodies={}, hz=None,
             montages={}, mprofile={}, tracks={}, mcurves={}, bcurves={}, override=None)
    for line in open(src_path, encoding='utf-8-sig', errors='replace'):
        c = line.rstrip('\r\n').split(',')
        if c[0] == 'INFO':
            s['hz'] = dict(zip(c[1::2], c[2::2])).get('hz')
        elif c[0] == 'MESH':
            s['mesh'] = c[1:8]
        elif c[0] == 'BODY':
            s['bodies'][c[1]] = (KINDS[c[2]], int(c[3]), c[4:14])
        elif c[0] == 'BONE':
            s['names'][int(c[1])] = c[2]
            s['parent'][int(c[1])] = int(c[3])
            s['ref'][c[2]] = c[4:11]
        elif c[0] == 'CLIP':
            s['clips'][int(c[1])] = c[2:9]
        elif c[0] == 'MARK':
            s['marks'].setdefault(int(c[1]), []).append((c[2], c[3]))
        elif c[0] == 'CURVE':
            s['curves'][int(c[1])] = (c[3], c[4], c[5:])
        elif c[0] == 'BCURVE':
            s['bcurves'][int(c[1])] = (c[3], c[4], c[5:])
        elif c[0] == 'AIMPITCH':
            s['aim'] = c[1:7]
        elif c[0] == 'MOVE':
            s['move'] = dict(zip(c[1::2], c[2::2]))
        elif c[0] == 'KEY':
            s['keys'][(int(c[1]), int(c[2]), int(c[3]))] = c[4:11]
        elif c[0] == 'MONTAGE':
            s['montages'][int(c[1])] = c[2:12]     # 이름 · 무리 · 길이 · 속도 · 넣기 · 넣기 곡선 · 빼기 · 빼기 곡선 · 빼기 시작 · 자동 빼기
            s['mprofile'][int(c[1])] = c[15]       # 빼기 비중 표 이름(버전 6, None = 없음)
        elif c[0] == 'TRACK':
            s['tracks'].setdefault(int(c[1]), []).append(c[2:8])     # 자리 · 클립 · 몽타주 시각 · 클립 시작 · 클립 끝 · 속도
        elif c[0] in ('MCURVE', 'SCURVE'):
            s['mcurves'].setdefault((c[0], int(c[1])), []).append((c[2], c[3], c[4], c[5:]))
        elif c[0] == 'OVERRIDE':
            s['override'] = c[1]
    # 몽타주 곡선을 덤프와 같은 (종류, 이름, 자리) 열쇠로 — 자리 -1 = 몽타주 시각, 0 이상 = 그 줄 클립
    s['packed'] = {}
    for k, tracks in s['tracks'].items():
        for name, hz, n, v in s['mcurves'].get(('MCURVE', k), []):
            if name in PACK_CURVES:
                s['packed'][(k, name, -1)] = (hz, n, v)
        for ti, t in enumerate(tracks):
            for name, hz, n, v in s['mcurves'].get(('SCURVE', int(t[1])), []):
                if name in PACK_CURVES:
                    s['packed'][(k, name, ti)] = (hz, n, v)
    return s


srcs = {name: parse(path) for name, path in pairs}
order = [name for name, _ in pairs]
first_src = srcs[order[0]]
src_names = first_src['names']
kept = sorted({b for (_, _, b) in first_src['keys']})
kept_names = {src_names[b] for b in kept}
name_of_key = {b: src_names[b] for b in kept}
index_of_name = {n: b for b, n in name_of_key.items()}

diff = missing = extra = 0
values = 0
counts = dict(bones=0, bodies=0, clips=0, marks=0, curves=0, curveValues=0, legCurves=0, keys=0)
first = []


def note(kind, where):
    if len(first) < 10:
        first.append(f'{kind} {where}')


# 묶음마다 본 것
seen = {name: dict(clips=set(), keys=set(), curves=set(), legCurves=set(), marks={}, aim=False, move=False, keyCount=0, montages=set(), tracks={}, packed=set(), override=False) for name in order}
seen_profiles = []
slot_names = []
slot_profiles = []
dump_bone_order = []
seen_bones = set()
seen_bodies = set()
mesh_ok = False
bundles_seen = []
cur = None      # 지금 읽는 묶음 이름(덤프의 BUNDLE 줄 뒤)
for line in open(dump_path, encoding='utf-8', errors='replace'):
    c = line.rstrip('\r\n').split(',')
    h = c[0]
    if h == 'MESH':
        mesh_ok = True
        for a, b in zip(first_src['mesh'], c[1:8]):
            values += 1
            if not same(a, b):
                diff += 1
                note('mesh', a)
    elif h == 'BODY':
        counts['bodies'] += 1
        if c[1] not in first_src['bodies']:
            extra += 1
            note('extra body', c[1])
            continue
        seen_bodies.add(c[1])
        kind, weak, v = first_src['bodies'][c[1]]
        if kind != int(c[2]) or weak != int(c[3]):
            diff += 1
            note('body kind', c[1])
        for a, b in zip(v, c[4:14]):
            values += 1
            if not same(a, b):
                diff += 1
                note('body', c[1])
    elif h == 'BONE':
        name = c[2]
        if name not in kept_names:
            extra += 1
            note('extra bone', name)
            continue
        seen_bones.add(name)
        dump_bone_order.append(name)
        counts['bones'] += 1
        p = first_src['parent'][index_of_name[name]]
        want_parent = '-' if p == -1 else src_names[p]
        if want_parent != c[3]:
            diff += 1
            note('parent', name)
        for a, b in zip(first_src['ref'][name], c[4:11]):
            values += 1
            if not same(a, b):
                diff += 1
                note('ref', name)
    elif h == 'BUNDLE':
        n = len(bundles_seen)
        bundles_seen.append(c[1])
        cur = c[1] if c[1] in srcs else None
        if n >= len(order) or order[n] != c[1]:
            diff += 1
            note('bundle order', (n, c[1]))
        if cur is None:
            extra += 1
            note('extra bundle', c[1])
            continue
        values += 1
        if srcs[cur]['hz'] != c[2]:
            diff += 1
            note('bundle hz', (cur, c[2]))
    elif h == 'PROFILE':
        # 덤프의 비중은 덤프 뼈 차례(BONE 줄 차례 = 남긴 뼈 차례)
        counts['profiles'] = counts.get('profiles', 0) + 1
        seen_profiles.append(c[2])
        want = profiles_src.get(c[2])
        if want is None:
            extra += 1
            note('extra profile', c[2])
            continue
        if want['mode'] != c[3]:
            diff += 1
            note('profile mode', c[2])
        default = '0' if want['mode'] == '2' else '1'
        got = c[4:]
        if len(got) != len(dump_bone_order):
            diff += 1
            note('profile size', (c[2], len(got)))
            continue
        for bone, value in zip(dump_bone_order, got):
            values += 1
            if not same(want['scale'].get(bone, default), value):
                diff += 1
                note('profile weight', (c[2], bone))
    elif h == 'SLOT':
        counts['slots'] = counts.get('slots', 0) + 1
        slot_names.append(c[2])
        slot_profiles.append(c[4])
    elif cur is None:
        if h in ('AIMPITCH', 'MOVE', 'CLIP', 'MARK', 'CURVE', 'KEY', 'MONTAGE', 'TRACK', 'MCURVE', 'OVERRIDE'):
            extra += 1
            note('row outside bundle', h)
    elif h == 'MONTAGE':
        k = int(c[1])
        counts['montages'] = counts.get('montages', 0) + 1
        want = srcs[cur]['montages'].get(k)
        if want is None:
            extra += 1
            note('extra montage', (cur, k))
            continue
        seen[cur]['montages'].add(k)
        if want[0] != c[2] or want[1] != c[3]:
            diff += 1
            note('montage name', (cur, k))
        for i in (2, 3, 4, 6, 8):       # 길이 · 속도 · 넣기 · 빼기 · 빼기 시작
            values += 1
            if not same(want[i], c[2 + i]):
                diff += 1
                note('montage value', (cur, k, i))
        for i in (5, 7, 9):             # 넣기 곡선 · 빼기 곡선 · 자동 빼기
            values += 1
            if int(want[i]) != int(c[2 + i]):
                diff += 1
                note('montage int', (cur, k, i))
        # 빼기 비중 표(버전 6) — 덤프는 번호, 원본은 이름. 번호는 덤프 PROFILE 줄 차례
        values += 1
        p = int(c[12]) if len(c) > 12 else -2
        got_profile = 'None' if p == -1 else (seen_profiles[p] if 0 <= p < len(seen_profiles) else f'?{p}')
        if got_profile != srcs[cur]['mprofile'].get(k):
            diff += 1
            note('montage blend out profile', (cur, k, got_profile, srcs[cur]['mprofile'].get(k)))
    elif h == 'TRACK':
        k = int(c[1])
        counts['tracks'] = counts.get('tracks', 0) + 1
        n = seen[cur]['tracks'].get(k, 0)
        seen[cur]['tracks'][k] = n + 1
        want = srcs[cur]['tracks'].get(k, [])
        if n >= len(want):
            extra += 1
            note('extra track', (cur, k))
            continue
        w = want[n]
        if w[0] != c[2] or int(w[1]) != int(c[3]) or c[2] not in slot_names:
            diff += 1
            note('track slot/clip', (cur, k, n))
        # 클립 끝은 묶는 도구가 클립 길이(float)로 자를 수 있다 — 자른 값과 같으면 맞다
        clip_len = srcs[cur]['clips'].get(int(w[1]), [None, '0'])[1]
        want_end = w[4] if struct.unpack('<f', bits(w[4]))[0] <= struct.unpack('<f', bits(clip_len))[0] else clip_len
        for a, b in ((w[2], c[4]), (w[3], c[5]), (want_end, c[6]), (w[5], c[7])):
            values += 1
            if not same(a, b):
                diff += 1
                note('track value', (cur, k, n))
    elif h == 'MCURVE':
        key = (int(c[1]), c[2], int(c[3]))
        counts['montageCurves'] = counts.get('montageCurves', 0) + 1
        want = srcs[cur]['packed'].get(key)
        if want is None:
            extra += 1
            note('extra montage curve', (cur, key))
            continue
        seen[cur]['packed'].add(key)
        hz, n, v = want
        if hz != c[4] or n != c[5] or len(v) != len(c[6:]):
            diff += 1
            note('montage curve shape', (cur, key))
            continue
        counts['montageCurveValues'] = counts.get('montageCurveValues', 0) + len(v)
        for a, b in zip(v, c[6:]):
            values += 1
            if not same(a, b):
                diff += 1
                note('montage curve', (cur, key))
    elif h == 'OVERRIDE':
        seen[cur]['override'] = True
        values += 1
        if srcs[cur]['override'] != c[1] or c[2] not in slot_names:
            diff += 1
            note('override', (cur, c[1], c[2]))
    elif h == 'AIMPITCH':
        seen[cur]['aim'] = True
        for a, b in zip(srcs[cur]['aim'], c[1:7]):
            values += 1
            if not same(a, b):
                diff += 1
                note('aim', (cur, a))
    elif h == 'MOVE':
        seen[cur]['move'] = True
        got = dict(zip(c[1::2], c[2::2]))
        for k, v in got.items():
            values += 1
            if not same(srcs[cur]['move'][k], v):
                diff += 1
                note('move', (cur, k))
    elif h == 'CLIP':
        i = int(c[1])
        src = srcs[cur]
        if i not in src['clips']:
            extra += 1
            note('extra clip', (cur, i))
            continue
        seen[cur]['clips'].add(i)
        counts['clips'] += 1
        s = src['clips'][i]
        if s[0] != c[2]:
            diff += 1
            note('clip name', (cur, i))
        for k in (3, 6, 8):     # 길이 · rootSpeed · 자산 재생 배율(판 10)
            values += 1
            if not same(s[k - 2], c[k]):
                diff += 1
                note('clip value', (cur, i, k))
        for k in (4, 5, 7):     # 표본 수 · 덧붙임 종류 · 반복
            values += 1
            if int(s[k - 2]) != int(c[k]):
                diff += 1
                note('clip int', (cur, i, k))
    elif h == 'MARK':
        i = int(c[1])
        marks_seen = seen[cur]['marks']
        n = marks_seen.get(i, 0)
        marks_seen[i] = n + 1
        want = srcs[cur]['marks'].get(i, [])
        counts['marks'] += 1
        if n >= len(want):
            extra += 1
            note('extra mark', (cur, i))
            continue
        values += 1
        if not same(want[n][0], c[2]) or want[n][1] != c[3]:
            diff += 1
            note('mark', (cur, i, n))
    elif h == 'CURVE':
        i = int(c[1])
        seen[cur]['curves'].add(i)
        counts['curves'] += 1
        if i not in srcs[cur]['curves']:
            extra += 1
            note('extra curve', (cur, i))
            continue
        hz, n, v = srcs[cur]['curves'][i]
        if hz != c[2] or n != c[3] or len(v) != len(c[4:]):
            diff += 1
            note('curve shape', (cur, i))
            continue
        counts['curveValues'] += len(v)
        for a, b in zip(v, c[4:]):
            values += 1
            if not same(a, b):
                diff += 1
                note('curve', (cur, i))
    elif h == 'LCURVE':
        i = int(c[1])
        seen[cur]['legCurves'].add(i)
        counts['legCurves'] += 1
        if i not in srcs[cur]['bcurves']:
            extra += 1
            note('extra leg curve', (cur, i))
            continue
        hz, cnt, v = srcs[cur]['bcurves'][i]
        if hz != c[2] or cnt != c[3] or len(v) != len(c[4:]):
            diff += 1
            note('leg curve shape', (cur, i))
            continue
        counts['curveValues'] += len(v)
        for a, b in zip(v, c[4:]):
            values += 1
            if not same(a, b):
                diff += 1
                note('leg curve', (cur, i))
    elif h == 'KEY':
        k = (int(c[1]), int(c[2]), c[3])
        seen[cur]['keys'].add(k)
        seen[cur]['keyCount'] += 1
        counts['keys'] += 1
        index = index_of_name.get(c[3])
        src = srcs[cur]['keys'].get((k[0], k[1], index)) if index is not None else None
        if src is None:
            extra += 1
            note('extra key', (cur, k))
            continue
        for a, b in zip(src, c[4:11]):
            values += 1
            if not same(a, b):
                diff += 1
                note('key', (cur, k))

src_key_total = 0
for name in order:
    src = srcs[name]
    got = seen[name]
    src_key_total += len(src['keys'])
    if name not in bundles_seen:
        missing += 1
        note('missing bundle', name)
        continue
    want_keys = {(cl, s, src['names'][b]) for (cl, s, b) in src['keys']}
    for k in want_keys - got['keys']:
        missing += 1
        note('missing key', (name, k))
    for i in set(src['clips']) - got['clips']:
        missing += 1
        note('missing clip', (name, i))
    for i in set(src['curves']) - got['curves']:
        missing += 1
        note('missing curve', (name, i))
    for i in set(src['bcurves']) - got['legCurves']:
        missing += 1
        note('missing leg curve', (name, i))
    for i, want in src['marks'].items():
        if got['marks'].get(i, 0) < len(want):
            missing += len(want) - got['marks'].get(i, 0)
            note('missing mark', (name, i))
    if not got['aim']:
        missing += 1
        note('missing AIMPITCH', name)
    if not got['move']:
        missing += 1
        note('missing MOVE', name)
    if got['keyCount'] != len(src['keys']):
        diff += 1
        note('key count', (name, got['keyCount'], len(src['keys'])))
    for k in set(src['montages']) - got['montages']:
        missing += 1
        note('missing montage', (name, k))
    for k, want in src['tracks'].items():
        if got['tracks'].get(k, 0) < len(want):
            missing += len(want) - got['tracks'].get(k, 0)
            note('missing track', (name, k))
    for key in set(src['packed']) - got['packed']:
        missing += 1
        note('missing montage curve', (name, key))
    if src['override'] is not None and not got['override']:
        missing += 1
        note('missing OVERRIDE', name)
for p in set(profiles_src) - set(seen_profiles):
    missing += 1
    note('missing profile', p)
if len(set(slot_names)) != len(slot_names) or any(p != '-' and p not in seen_profiles for p in slot_profiles):
    diff += 1
    note('slots', list(zip(slot_names, slot_profiles)))
for name in kept_names - seen_bones:
    missing += 1
    note('missing bone', name)
for name in set(first_src['bodies']) - seen_bodies:
    missing += 1
    note('missing body', name)
if not mesh_ok:
    missing += 1
    note('missing', 'MESH')

print(f'CLIPCHECK bundles={len(bundles_seen)}({" ".join(bundles_seen)}) bones={counts["bones"]} bodies={counts["bodies"]} clips={counts["clips"]} marks={counts["marks"]} curves={counts["curves"]} curveValues={counts["curveValues"]} keys={counts["keys"]} srcKeys={src_key_total} values={values} diff={diff} missing={missing} extra={extra}')
print(f'CLIPCHECK profiles={counts.get("profiles", 0)} slots={counts.get("slots", 0)} montages={counts.get("montages", 0)} tracks={counts.get("tracks", 0)} montageCurves={counts.get("montageCurves", 0)} montageCurveValues={counts.get("montageCurveValues", 0)}')
for x in first:
    print('  ' + str(x))
ok = diff == 0 and missing == 0 and extra == 0 and counts['keys'] == src_key_total and counts['keys'] > 0 and len(bundles_seen) == len(order)
print('CLIPCHECK ok' if ok else 'CLIPCHECK FAIL')
sys.exit(0 if ok else 1)
