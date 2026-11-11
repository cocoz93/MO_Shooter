#!/usr/bin/env python3
# 클립 파일 대조 — Shared/AnimClips 가 읽은 값(AnimCheck clips --dump)을 파일을 만들 때 쓴 csv 와 전수 대조한다 (candidate1-impl-plan.md 1단계).
#   python check_clip_pack.py <clips120.csv> <dump.csv>
# 비교는 float 비트로 한다. csv 값을 float 로 줄인 것과 읽은 값이 한 비트라도 다르면 diff 로 센다.
import struct
import sys

sys.stdout.reconfigure(encoding='utf-8')


def bits(x):
    return struct.pack('<f', float(x))


def same(a, b):
    return bits(a) == bits(b)


src_path, dump_path = sys.argv[1], sys.argv[2]

src_names = {}
src_parent = {}
src_ref = {}
src_clips = {}
src_marks = {}
src_curves = {}
src_keys = {}
src_aim = None
src_move = None
src_mesh = None
src_bodies = {}
KINDS = {'sphere': 0, 'sphyl': 1, 'box': 2}
for line in open(src_path, encoding='utf-8-sig', errors='replace'):
    c = line.rstrip('\r\n').split(',')
    if c[0] == 'MESH':
        src_mesh = c[1:8]
    elif c[0] == 'BODY':
        src_bodies[c[1]] = (KINDS[c[2]], int(c[3]), c[4:14])
    elif c[0] == 'BONE':
        src_names[int(c[1])] = c[2]
        src_parent[int(c[1])] = int(c[3])
        src_ref[c[2]] = c[4:11]
    elif c[0] == 'CLIP':
        src_clips[int(c[1])] = c[2:8]
    elif c[0] == 'MARK':
        src_marks.setdefault(int(c[1]), []).append((c[2], c[3]))
    elif c[0] == 'CURVE':
        src_curves[int(c[1])] = (c[3], c[4], c[5:])
    elif c[0] == 'AIMPITCH':
        src_aim = c[1:7]
    elif c[0] == 'MOVE':
        src_move = dict(zip(c[1::2], c[2::2]))
    elif c[0] == 'KEY':
        src_keys[(int(c[1]), int(c[2]), int(c[3]))] = c[4:11]

kept = sorted({b for (_, _, b) in src_keys})
kept_names = {src_names[b] for b in kept}
name_of_key = {b: src_names[b] for b in kept}

diff = missing = extra = 0
values = 0
counts = dict(bones=0, bodies=0, clips=0, marks=0, curves=0, curveValues=0, keys=0)
first = []


def note(kind, where):
    if len(first) < 10:
        first.append(f'{kind} {where}')


seen_bones = set()
seen_clips = set()
seen_keys = set()
seen_curves = set()
marks_seen = {}
seen_bodies = set()
aim_ok = move_ok = mesh_ok = False
bundles = 0
for line in open(dump_path, encoding='utf-8', errors='replace'):
    c = line.rstrip('\r\n').split(',')
    h = c[0]
    if h == 'MESH':
        mesh_ok = True
        for a, b in zip(src_mesh, c[1:8]):
            values += 1
            if not same(a, b):
                diff += 1
                note('mesh', a)
    elif h == 'BODY':
        counts['bodies'] += 1
        if c[1] not in src_bodies:
            extra += 1
            note('extra body', c[1])
            continue
        seen_bodies.add(c[1])
        kind, weak, v = src_bodies[c[1]]
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
        counts['bones'] += 1
        index = next(b for b in kept if src_names[b] == name)
        p = src_parent[index]
        want_parent = '-' if p == -1 else src_names[p]
        if want_parent != c[3]:
            diff += 1
            note('parent', name)
        for a, b in zip(src_ref[name], c[4:11]):
            values += 1
            if not same(a, b):
                diff += 1
                note('ref', name)
    elif h == 'BUNDLE':
        bundles += 1
    elif h == 'AIMPITCH':
        aim_ok = True
        for a, b in zip(src_aim, c[1:7]):
            values += 1
            if not same(a, b):
                diff += 1
                note('aim', a)
    elif h == 'MOVE':
        move_ok = True
        got = dict(zip(c[1::2], c[2::2]))
        for k, v in got.items():
            values += 1
            if not same(src_move[k], v):
                diff += 1
                note('move', k)
    elif h == 'CLIP':
        i = int(c[1])
        if i not in src_clips:
            extra += 1
            note('extra clip', i)
            continue
        seen_clips.add(i)
        counts['clips'] += 1
        s = src_clips[i]
        if s[0] != c[2]:
            diff += 1
            note('clip name', i)
        for k in (3, 6):        # 길이 · rootSpeed
            values += 1
            if not same(s[k - 2], c[k]):
                diff += 1
                note('clip value', (i, k))
        for k in (4, 5, 7):     # 표본 수 · 덧붙임 종류 · 반복
            values += 1
            if int(s[k - 2]) != int(c[k]):
                diff += 1
                note('clip int', (i, k))
    elif h == 'MARK':
        i = int(c[1])
        n = marks_seen.get(i, 0)
        marks_seen[i] = n + 1
        want = src_marks.get(i, [])
        counts['marks'] += 1
        if n >= len(want):
            extra += 1
            note('extra mark', i)
            continue
        values += 1
        if not same(want[n][0], c[2]) or want[n][1] != c[3]:
            diff += 1
            note('mark', (i, n))
    elif h == 'CURVE':
        i = int(c[1])
        seen_curves.add(i)
        counts['curves'] += 1
        if i not in src_curves:
            extra += 1
            note('extra curve', i)
            continue
        hz, n, v = src_curves[i]
        if hz != c[2] or n != c[3] or len(v) != len(c[4:]):
            diff += 1
            note('curve shape', i)
            continue
        counts['curveValues'] += len(v)
        for a, b in zip(v, c[4:]):
            values += 1
            if not same(a, b):
                diff += 1
                note('curve', i)
    elif h == 'KEY':
        k = (int(c[1]), int(c[2]), c[3])
        seen_keys.add(k)
        counts['keys'] += 1
        index = next((b for b, n in name_of_key.items() if n == c[3]), None)
        src = src_keys.get((k[0], k[1], index)) if index is not None else None
        if src is None:
            extra += 1
            note('extra key', k)
            continue
        for a, b in zip(src, c[4:11]):
            values += 1
            if not same(a, b):
                diff += 1
                note('key', k)

want_keys = {(cl, s, src_names[b]) for (cl, s, b) in src_keys}
for k in want_keys - seen_keys:
    missing += 1
    note('missing key', k)
for name in kept_names - seen_bones:
    missing += 1
    note('missing bone', name)
for i in set(src_clips) - seen_clips:
    missing += 1
    note('missing clip', i)
for i in set(src_curves) - seen_curves:
    missing += 1
    note('missing curve', i)
for i, want in src_marks.items():
    if marks_seen.get(i, 0) < len(want):
        missing += len(want) - marks_seen.get(i, 0)
        note('missing mark', i)
for name in set(src_bodies) - seen_bodies:
    missing += 1
    note('missing body', name)
if not mesh_ok:
    missing += 1
    note('missing', 'MESH')
if not aim_ok:
    missing += 1
    note('missing', 'AIMPITCH')
if not move_ok:
    missing += 1
    note('missing', 'MOVE')

print(f'CLIPCHECK bundles={bundles} bones={counts["bones"]} bodies={counts["bodies"]} clips={counts["clips"]} marks={counts["marks"]} curves={counts["curves"]} curveValues={counts["curveValues"]} keys={counts["keys"]} srcKeys={len(src_keys)} values={values} diff={diff} missing={missing} extra={extra}')
for x in first:
    print('  ' + str(x))
ok = diff == 0 and missing == 0 and extra == 0 and counts['keys'] == len(src_keys) and counts['keys'] > 0
print('CLIPCHECK ok' if ok else 'CLIPCHECK FAIL')
sys.exit(0 if ok else 1)
