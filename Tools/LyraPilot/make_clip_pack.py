#!/usr/bin/env python3
# 클립 자료 묶기 — Lyra 에서 뽑은 csv(animcmp_<tag>_clips120.csv)를 서버 · 화면 · 봇이 함께 읽는 이진 파일 하나로 만든다 (candidate1-impl-plan.md 1단계).
#   python make_clip_pack.py <clips120.csv> <out.bin> [--bundle=rifle]
# 클립 자료는 Epic 샘플에서 나온 것이라 리포에 넣지 않는다. 이 도구와 읽는 코드만 리포에 있고, 파일은 Saved/MOB/ 에 둔다.
# 형식은 Shared/AnimClips.h 의 주석과 짝이다. 고치면 kClipPackVersion 을 같이 올린다.
#
# 크기 칸(scale)은 파일에 넣지 않는다. 일반 클립은 1, 덧붙임 클립은 0 이어야 하고, 아니면 여기서 멈춘다(엔진의 덧붙임 크기는 1 에서의 차이다).
import math
import struct
import sys

sys.stdout.reconfigure(encoding='utf-8')
args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict((a[2:].split('=', 1) + [''])[:2] for a in sys.argv[1:] if a.startswith('--'))
BUNDLE = opt.get('bundle', 'rifle')
VERSION = 1
KINDS = {'sphere': 0, 'sphyl': 1, 'box': 2}     # Shared/Weapon.h EBodyShape 값
SCALE_LIMIT = 1e-4

src, dst = args[0], args[1]
bones = {}
keyed = set()
clips = {}
marks = {}
curves = {}
keys = {}
aim = None
move = None
info = {}
mesh = None
bodies = []
fails = []
for line in open(src, encoding='utf-8-sig', errors='replace'):
    c = line.rstrip('\r\n').split(',')
    h = c[0]
    if h == 'INFO':
        info = dict(zip(c[1::2], c[2::2]))
    elif h == 'MESH':
        mesh = [float(x) for x in c[1:11]]
    elif h == 'BODY':
        bodies.append(dict(bone=c[1], kind=c[2], weak=int(c[3]), v=[float(x) for x in c[4:14]]))
    elif h == 'BONE':
        bones[int(c[1])] = dict(name=c[2], parent=int(c[3]), ref=[float(x) for x in c[4:14]])
    elif h == 'CLIP':
        clips[int(c[1])] = dict(name=c[2], length=float(c[3]), samples=int(c[4]), additive=int(c[5]), root=float(c[6]), loop=int(c[7]))
    elif h == 'CLIPFAIL':
        fails.append(int(c[1]))
    elif h == 'MARK':
        marks.setdefault(int(c[1]), []).append((float(c[2]), c[3]))
    elif h == 'CURVE':
        curves[int(c[1])] = dict(name=c[2], hz=int(c[3]), n=int(c[4]), v=[float(x) for x in c[5:]])
    elif h == 'AIMPITCH':
        aim = [float(x) for x in c[1:7]]
    elif h == 'MOVE':
        move = dict(zip(c[1::2], c[2::2]))
    elif h == 'KEY':
        b = int(c[3])
        keyed.add(b)
        keys.setdefault(int(c[1]), {})[(int(c[2]), b)] = [float(x) for x in c[4:14]]

# 약점 부위가 하나도 없으면 머리 판정이 영영 안 난다 — 뽑을 때 약점 재질을 못 읽은 것이다
weak_bodies = sum(1 for b in bodies if b['weak'])
if fails or len(clips) != 30 or aim is None or move is None or mesh is None or not bodies or weak_bodies == 0 or 'hz' not in info:
    print(f'CLIPPACK FAIL clips={len(clips)} fails={fails} aim={aim is not None} move={move is not None} mesh={mesh is not None} bodies={len(bodies)} weak={weak_bodies} info={info}')
    sys.exit(1)
HZ = int(info['hz'])

# 남긴 뼈만 쓴다. 부모는 항상 남긴 뼈 안에 있어야 하고(조상까지 남겼으므로), 번호는 부모가 먼저 오는 차례를 지킨다
order = sorted(keyed)
remap = {b: i for i, b in enumerate(order)}
for b in order:
    p = bones[b]['parent']
    if p != -1 and (p not in remap or remap[p] >= remap[b]):
        print(f'CLIPPACK FAIL bone={bones[b]["name"]} parent={p} 가 남긴 뼈에 없거나 차례가 어긋남')
        sys.exit(1)

out = bytearray()
out += b'MOAC' + struct.pack('<I', VERSION)
out += struct.pack('<I', len(order))
for b in order:
    n = bones[b]['name'].encode('ascii')
    out += struct.pack('<B', len(n)) + n
    p = bones[b]['parent']
    out += struct.pack('<h', -1 if p == -1 else remap[p])
    out += struct.pack('<7f', *bones[b]['ref'][:7])
    s = bones[b]['ref'][7:10]
    if max(abs(x - 1.0) for x in s) > SCALE_LIMIT:
        print(f'CLIPPACK FAIL ref scale bone={bones[b]["name"]} {s}')
        sys.exit(1)

if max(abs(x - 1.0) for x in mesh[7:10]) > SCALE_LIMIT:
    print(f'CLIPPACK FAIL mesh scale {mesh[7:10]}')
    sys.exit(1)
out += struct.pack('<7f', *mesh[:7])
out += struct.pack('<I', len(bodies))
kept_names = {bones[b]['name'] for b in order}
for body in bodies:
    if body['bone'] not in kept_names or body['kind'] not in KINDS:
        print(f'CLIPPACK FAIL body={body["bone"]} kind={body["kind"]}')
        sys.exit(1)
    n = body['bone'].encode('ascii')
    out += struct.pack('<B', len(n)) + n
    out += struct.pack('<BB', KINDS[body['kind']], body['weak'])
    out += struct.pack('<10f', *body['v'])

out += struct.pack('<I', 1)     # 무기별 묶음 수. 1차 자료는 소총 한 벌이다
n = BUNDLE.encode('ascii')
out += struct.pack('<B', len(n)) + n
out += struct.pack('<I', HZ)
out += struct.pack('<6f', *aim)
out += struct.pack('<5f', float(move['groundFriction']), float(move['brakingFriction']), float(move['brakingFrictionFactor']), float(move['brakingDecelerationWalking']), float(move['gravityZ']))
out += struct.pack('<B', int(move['separateBraking']))
out += struct.pack('<I', len(clips))
total_samples = 0
total_seconds = 0.0
scale_max = 0.0
for i in range(30):
    c = clips[i]
    # 표본 수 = 뽑는 쪽과 같은 float 곱의 올림 + 1. 다르면 --hz 가 뽑은 Hz 와 다른 것이다
    want_samples = math.ceil(struct.unpack('<f', struct.pack('<f', c['length'] * HZ))[0]) + 1
    if c['samples'] != want_samples:
        print(f'CLIPPACK FAIL clip={c["name"]} samples={c["samples"]} want={want_samples} hz={HZ}')
        sys.exit(1)
    n = c['name'].encode('ascii')
    out += struct.pack('<B', len(n)) + n
    out += struct.pack('<fBBfI', c['length'], c['additive'], c['loop'], c['root'], c['samples'])
    m = marks.get(i, [])
    out += struct.pack('<H', len(m))
    for t, name in m:
        if name not in ('L', 'R'):
            print(f'CLIPPACK FAIL clip={c["name"]} marker={name}')
            sys.exit(1)
        out += struct.pack('<fB', t, 1 if name == 'L' else 0)
    cv = curves.get(i)
    out += struct.pack('<B', 0 if cv is None else 1)
    if cv is not None:
        if cv['n'] != len(cv['v']):
            print(f'CLIPPACK FAIL curve clip={c["name"]} n={cv["n"]} got={len(cv["v"])}')
            sys.exit(1)
        out += struct.pack('<II', cv['hz'], cv['n'])
        out += struct.pack(f'<{cv["n"]}f', *cv['v'])
    want_scale = 0.0 if c['additive'] != 0 else 1.0
    k = keys[i]
    if len(k) != c['samples'] * len(order):
        print(f'CLIPPACK FAIL clip={c["name"]} keys={len(k)} want={c["samples"] * len(order)}')
        sys.exit(1)
    for s in range(c['samples']):
        for b in order:
            v = k[(s, b)]
            out += struct.pack('<7f', *v[:7])
            for x in v[7:10]:
                scale_max = max(scale_max, abs(x - want_scale))
    total_samples += c['samples']
    total_seconds += c['length']
if scale_max > SCALE_LIMIT:
    print(f'CLIPPACK FAIL 크기가 움직이는 클립이 있다 scaleDiffMax={scale_max}')
    sys.exit(1)
out += b'MOAE'
open(dst, 'wb').write(out)
print(f'CLIPPACK ok bones={len(order)} clips={len(clips)} samples={total_samples} seconds={total_seconds:.2f} bytes={len(out)} scaleDiffMax={scale_max:.6f}')
print('CLIPPACK additive ' + ' '.join(f'{c["name"]}={c["additive"]}' for c in clips.values() if c['additive'] != 0))
print('CLIPPACK move ' + ' '.join(f'{k}={v}' for k, v in move.items()))
