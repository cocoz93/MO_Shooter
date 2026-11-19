#!/usr/bin/env python3
# 클립 자료 묶기 — Lyra 에서 무기마다 뽑은 csv(animcmp_<tag>_clips120.csv)를 서버 · 화면 · 봇이 함께 읽는 이진 파일 하나로 만든다 (candidate1-impl-plan.md 1단계, 9단계 버전 2).
#   python make_clip_pack.py <out.bin> --profiles=<blendprofiles.csv> <묶음 이름>=<clips120.csv> [<묶음 이름>=<clips120.csv> ...]
#   예) make_clip_pack.py clips.bin --profiles=animcmp_ev_rifle_blendprofiles.csv rifle=animcmp_clips3_rifle_clips120.csv pistol=animcmp_clips3_pistol_clips120.csv shotgun=animcmp_clips3_shotgun_clips120.csv
# 묶음 이름 = 무기 이름(record_anim_compare.ps1 -Weapon 값). csv 의 WEAPON 줄이 묶음 이름과 다르면 멈춘다 — 다른 무기 자료를 잘못 넣지 않게.
# 뼈대 · 메시 자리 · 부위 · 남긴 뼈는 묶음 전부가 같아야 한다(한 몸이다). 하나라도 다르면 멈춘다.
# 클립 자료는 Epic 샘플에서 나온 것이라 리포에 넣지 않는다. 이 도구와 읽는 코드만 리포에 있고, 파일은 Saved/MOB/ 에 둔다.
# 형식은 Shared/AnimClips.h 의 주석과 짝이다. 고치면 kClipPackVersion 을 같이 올린다.
# 버전 2 = 뼈별 비중 표 · 끼움 자리 표(부위 뒤)와 묶음마다 몽타주 표 · 쏜 뒤 윗몸 덮기 자세(클립 뒤). 몽타주 클립과 덮기 자세 클립은 묶음 클립 30개 뒤에 붙는다(9단계 3-a).
# 버전 3 = 묶음 클립이 47개 — 30~46 이 조준 서기 · 걸음 넷 · 전환 12(컨텐츠 C-3, D60). 몽타주 · 덮기 자세 클립은 그 뒤.
# 버전 4 = 묶음 클립이 67개 — 47~66 이 앉은 서기 · 걸음 넷 · 전환 12 · 앉기 · 일어서기 전환 · 앉아 쏜 뒤 덮기 자세(컨텐츠 D-3).
# 버전 5 = 몽타주 종류가 8개 — 근접 · 맞은 쪽 반응 뒤 · 앞(컨텐츠 E-2). 칸 꼴은 버전 4 와 같다.
#   뼈별 비중 표 = 골격의 블렌드 프로파일(1-a 기록 animcmp_ev_<무기>_blendprofiles.csv, 세 무기가 같은 골격). 표에 없는 뼈는 엔진 기본값(뼈 가림 0, 그 밖 1)으로 채운다.
#   몽타주 곡선은 PACK_CURVES 이름만 넣는다 — 나머지 이름은 화면이 엔진 몽타주로 직접 트는 쪽 몫이라 버리고, 버린 이름을 찍는다.
#
# 크기 칸(scale)은 파일에 넣지 않는다. 일반 클립은 1, 덧붙임 클립은 0 이어야 하고, 아니면 여기서 멈춘다(엔진의 덧붙임 크기는 1 에서의 차이다).
import math
import struct
import sys

sys.stdout.reconfigure(encoding='utf-8')
VERSION = 6                                     # 6 = 몽타주 종류 12(대시 넷, D-4) · 몽타주마다 빼기 비중 표 칸 i16(형식이 바뀐 판). 5 = 몽타주 종류 8(근접 · 반응 둘, E-2). 4 = 조준 클립 뒤에 앉기 클립 20개(컨텐츠 D-3). 3 = 묶음 클립 30개 뒤에 조준 클립 17개(C-3, D60). 형식 칸은 그대로고 앞 클립 수만 는다
KINDS = {'sphere': 0, 'sphyl': 1, 'box': 2}     # Shared/Weapon.h EBodyShape 값
SCALE_LIMIT = 1e-4
CLIPS_PER_BUNDLE = 67                           # Shared/AnimClips.h ACLIP_COUNT — 앞 67개(조준 17개 · 앉기 20개 포함). 그 뒤는 몽타주 · 덮기 자세 클립
ADS_IDLE_INDEX = 30                             # Shared/AnimClips.h ACLIP_ADS_IDLE
CROUCH_IDLE_INDEX = 47                          # Shared/AnimClips.h ACLIP_CROUCH_IDLE
MONTAGE_KINDS = ('fire', 'reload', 'equip', 'unequip', 'spawn', 'melee', 'hit_back', 'hit_front', 'dash_forward', 'dash_backward', 'dash_left', 'dash_right')    # Shared/AnimClips.h EMontageKind 차례 = 뽑는 쪽 WEAPON_MONTAGES 차례
SLOT_MODES = {'replace': 0, 'additive': 1, 'perbone': 2}           # Shared/AnimClips.h ESlotMode
BLEND_OPTION_MAX = 2                            # Shared/AnimClips.h kBlendOptionMax — 몽타주 넣기 · 빼기 곡선은 선형 · Cubic · HermiteCubic 만 옮긴다
# 끼움 자리 표. 방식은 슬롯 이름에서 읽은 것이고(이름 짐작) 아래에서 줄 클립의 덧붙임 종류와 맞는지 막는다 — 덧붙임 자리엔 덧붙임 클립만, 나머지엔 일반 클립만.
# 뼈별 비중 표는 플랜 9-1 의 짐작(윗몸/아랫몸 나눔 표)이다. 어느 표가 Lyra 와 맞는지는 3-b 대조로 고른다(1-c — 비중 표 5개 중 어느 것인지 기록 칸에 없다).
# 3-b 대조로 고른 표 = 세 자리 모두 UpperBodyMask(D50) — 뼈 가림 넷을 자리마다 바꿔 Lyra 원본 기록과의 22부위 차이를 쟀고, 나눔 표보다 세 무기 모두 꺼내기 · 재장전 · 교체에서 작다.
# 그래프를 읽은 값이 아니라 가장 가까운 후보다(제 판단)
# 컨텐츠 E-2-b 에서 그래프를 읽었다(D72, 원본 기준) — 윗몸 자리는 LayeredBoneBlend_2 의 가림 UpperBodyLowerBodySplitMask 로 바꿨다.
# 윗몸 덧붙임 자리는 그래프에서 가림 없이 더해지고(ApplyAdditive_1) Shared 실제 경로도 이 표를 안 쓴다 — 옛 차례(D50) 시험 판을 다시 만들 때만 쓰여 그대로 둔다
# MOB_ 로 시작하는 자리는 엔진 슬롯이 아니다 — 쏜 뒤 윗몸 덮기(D45)가 덮는 범위
SLOTS = (
    ('FullBody', 'replace', None),
    ('FullBodyAdditivePreAim', 'additive', None),
    ('UpperBody', 'perbone', 'UpperBodyLowerBodySplitMask'),
    ('UpperBodyAdditive', 'additive', 'UpperBodyMask'),
    ('MOB_HipfireOverride', 'perbone', 'UpperBodyMask'),
    ('AdditiveHitReact', 'additive', None),     # 맞은 쪽 반응(E-2). 그래프의 그냥 끼움 노드라 비중 표가 없다(ABP_Mannequin_Base 덤프, 원본 기준)
)
OVERRIDE_SLOT = 'MOB_HipfireOverride'
PACK_CURVES = ('DisableLHandIK',)               # 판정 몸에 닿는 곡선 = 손 붙이기 끄기(1-c). ScaleDownWeaponR 은 무기 메시 크기라 화면 몫(이름 짐작)


def fail(text):
    print('CLIPPACK FAIL ' + text)
    sys.exit(1)


args = sys.argv[2:]
profile_src = None
for a in list(args):
    if a.startswith('--profiles='):
        profile_src = a.split('=', 1)[1]
        args.remove(a)
if len(sys.argv) < 3 or profile_src is None or not args or any('=' not in a or a.startswith('--') for a in args):
    fail('사용: make_clip_pack.py <out.bin> --profiles=<blendprofiles.csv> <묶음 이름>=<clips120.csv> ...')
dst = sys.argv[1]
pairs = [a.split('=', 1) for a in args]
if len({name for name, _ in pairs}) != len(pairs):
    fail('묶음 이름이 겹친다 ' + ' '.join(name for name, _ in pairs))


def parse(name, src):
    d = dict(name=name, src=src, bones={}, keyed=set(), clips={}, marks={}, curves={}, keys={}, aim=None, move=None, info={}, mesh=None, bodies=[], fails=[], weapon=None,
             montages={}, tracks={}, mcurves={}, scurves={}, override=None, mfails=[])
    for line in open(src, encoding='utf-8-sig', errors='replace'):
        c = line.rstrip('\r\n').split(',')
        h = c[0]
        if h == 'INFO':
            d['info'] = dict(zip(c[1::2], c[2::2]))
        elif h == 'WEAPON':
            d['weapon'] = c[1]
        elif h == 'MESH':
            d['mesh'] = [float(x) for x in c[1:11]]
        elif h == 'BODY':
            d['bodies'].append(dict(bone=c[1], kind=c[2], weak=int(c[3]), v=[float(x) for x in c[4:14]]))
        elif h == 'BONE':
            d['bones'][int(c[1])] = dict(name=c[2], parent=int(c[3]), ref=[float(x) for x in c[4:14]])
        elif h == 'CLIP':
            d['clips'][int(c[1])] = dict(name=c[2], length=float(c[3]), samples=int(c[4]), additive=int(c[5]), root=float(c[6]), loop=int(c[7]))
        elif h == 'CLIPFAIL':
            d['fails'].append(int(c[1]))
        elif h == 'MARK':
            d['marks'].setdefault(int(c[1]), []).append((float(c[2]), c[3]))
        elif h == 'CURVE':
            d['curves'][int(c[1])] = dict(name=c[2], hz=int(c[3]), n=int(c[4]), v=[float(x) for x in c[5:]])
        elif h == 'AIMPITCH':
            d['aim'] = [float(x) for x in c[1:7]]
        elif h == 'MOVE':
            d['move'] = dict(zip(c[1::2], c[2::2]))
        elif h == 'KEY':
            b = int(c[3])
            d['keyed'].add(b)
            d['keys'].setdefault(int(c[1]), {})[(int(c[2]), b)] = [float(x) for x in c[4:14]]
        elif h == 'MONTAGE':
            # 칸 = 차례, 이름, 무리, 길이, 재생 속도, 넣기 시간 · 곡선, 빼기 시간 · 곡선, 빼기 시작, 자동 빼기, 넣기 · 빼기 방식, 넣기 · 빼기 비중 표, 구역
            d['montages'][int(c[1])] = dict(name=c[2], group=c[3], length=float(c[4]), rate=float(c[5]), blendIn=float(c[6]), blendInOption=int(c[7]), blendOut=float(c[8]),
                                            blendOutOption=int(c[9]), trigger=float(c[10]), auto=int(c[11]), modeIn=int(c[12]), modeOut=int(c[13]), profileIn=c[14], profileOut=c[15], sections=c[16])
        elif h == 'TRACK':
            d['tracks'].setdefault(int(c[1]), []).append(dict(slot=c[2], clip=int(c[3]), startPos=float(c[4]), clipStart=float(c[5]), clipEnd=float(c[6]), rate=float(c[7]), loops=int(c[8])))
        elif h == 'MCURVE':
            d['mcurves'].setdefault(int(c[1]), []).append(dict(name=c[2], hz=int(c[3]), n=int(c[4]), v=[float(x) for x in c[5:]]))
        elif h == 'SCURVE':
            d['scurves'].setdefault(int(c[1]), []).append(dict(name=c[2], hz=int(c[3]), n=int(c[4]), v=[float(x) for x in c[5:]]))
        elif h == 'OVERRIDE':
            d['override'] = int(c[1])
        elif h in ('MONTAGEFAIL', 'TRACKFAIL', 'OVERRIDEFAIL'):
            d['mfails'].append(line.strip())

    # 약점 부위가 하나도 없으면 머리 판정이 영영 안 난다 — 뽑을 때 약점 재질을 못 읽은 것이다
    weak = sum(1 for b in d['bodies'] if b['weak'])
    clip_count = len(d['clips'])
    if d['fails'] or clip_count < CLIPS_PER_BUNDLE or sorted(d['clips']) != list(range(clip_count)) or d['aim'] is None or d['move'] is None or d['mesh'] is None or not d['bodies'] or weak == 0 or 'hz' not in d['info']:
        fail(f'bundle={name} clips={clip_count} fails={d["fails"]} aim={d["aim"] is not None} move={d["move"] is not None} mesh={d["mesh"] is not None} bodies={len(d["bodies"])} weak={weak} info={d["info"]}')
    if d['mfails'] or sorted(d['montages']) != list(range(len(MONTAGE_KINDS))) or d['override'] is None:
        fail(f'bundle={name} montages={sorted(d["montages"])} override={d["override"]} fails={d["mfails"]} — 몽타주 종류 {len(MONTAGE_KINDS)}개와 덮기 자세가 다 있어야 한다')
    if d['weapon'] != name:
        fail(f'bundle={name} 인데 csv 의 WEAPON 줄 = {d["weapon"]} ({src})')
    # 버전 2 때 뽑은 자료(조준 클립 없음)는 30번부터 몽타주 클립이라 클립 수만으로는 못 가른다 — 조준 서기 자리 이름으로 본다
    if not d['clips'][ADS_IDLE_INDEX]['name'].endswith('_Idle_ADS'):
        fail(f'bundle={name} 클립 {ADS_IDLE_INDEX} = {d["clips"][ADS_IDLE_INDEX]["name"]} — 조준 서기(*_Idle_ADS)가 아니다. 조준 클립 전에 뽑은 자료다({src})')
    # 버전 3 때 뽑은 자료(앉기 클립 없음)도 47번부터 몽타주 클립이다 — 앉은 서기 자리 이름으로 가른다
    if not d['clips'][CROUCH_IDLE_INDEX]['name'].endswith('_Crouch_Idle'):
        fail(f'bundle={name} 클립 {CROUCH_IDLE_INDEX} = {d["clips"][CROUCH_IDLE_INDEX]["name"]} — 앉은 서기(*_Crouch_Idle)가 아니다. 앉기 클립 전에 뽑은 자료다({src})')
    d['hz'] = int(d['info']['hz'])
    return d


bundles = [parse(name, src) for name, src in pairs]
body = bundles[0]

# 한 몸 검사 — 뼈대 · 남긴 뼈 · 메시 자리 · 부위가 묶음끼리 값까지 같아야 한다
for d in bundles[1:]:
    for part in ('bones', 'keyed', 'mesh', 'bodies'):
        if d[part] != body[part]:
            fail(f'bundle={d["name"]} 의 {part} 가 bundle={body["name"]} 와 다르다 — 한 몸이 아니다')

# 남긴 뼈만 쓴다. 부모는 항상 남긴 뼈 안에 있어야 하고(조상까지 남겼으므로), 번호는 부모가 먼저 오는 차례를 지킨다
bones = body['bones']
order = sorted(body['keyed'])
remap = {b: i for i, b in enumerate(order)}
for b in order:
    p = bones[b]['parent']
    if p != -1 and (p not in remap or remap[p] >= remap[b]):
        fail(f'bone={bones[b]["name"]} parent={p} 가 남긴 뼈에 없거나 차례가 어긋남')

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
        fail(f'ref scale bone={bones[b]["name"]} {s}')

mesh = body['mesh']
if max(abs(x - 1.0) for x in mesh[7:10]) > SCALE_LIMIT:
    fail(f'mesh scale {mesh[7:10]}')
out += struct.pack('<7f', *mesh[:7])
out += struct.pack('<I', len(body['bodies']))
kept_names = {bones[b]['name'] for b in order}
for part in body['bodies']:
    if part['bone'] not in kept_names or part['kind'] not in KINDS:
        fail(f'body={part["bone"]} kind={part["kind"]}')
    n = part['bone'].encode('ascii')
    out += struct.pack('<B', len(n)) + n
    out += struct.pack('<BB', KINDS[part['kind']], part['weak'])
    out += struct.pack('<10f', *part['v'])

# 뼈별 비중 표 — 남긴 뼈 차례로. 표에 없는 뼈는 엔진 기본값(BlendProfile.h GetDefaultBlendScale: 뼈 가림 방식 0, 그 밖 1)
profiles = {}
for line in open(profile_src, encoding='utf-8-sig', errors='replace'):
    c = line.rstrip('\r\n').split(',')
    if c[0] == 'profile' or len(c) < 4:
        continue
    p = profiles.setdefault(c[0], dict(mode=int(c[1]), scale={}))
    if p['mode'] != int(c[1]):
        fail(f'profile={c[0]} 방식이 줄마다 다르다')
    p['scale'][c[2]] = float(c[3])
if not profiles:
    fail(f'뼈별 비중 표가 비었다 {profile_src}')
all_bone_names = {bb['name'] for bb in bones.values()}
profile_names = list(profiles)
profile_summary = []
out += struct.pack('<I', len(profile_names))
for pname in profile_names:
    p = profiles[pname]
    unknown = [n for n in p['scale'] if n not in all_bone_names]
    if unknown:
        fail(f'profile={pname} 골격에 없는 뼈 {unknown[:5]}')
    default = 0.0 if p['mode'] == 2 else 1.0
    weights = [p['scale'].get(bones[b]['name'], default) for b in order]
    n = pname.encode('ascii')
    out += struct.pack('<B', len(n)) + n + struct.pack('<B', p['mode'])
    out += struct.pack(f'<{len(weights)}f', *weights)
    listed = sum(1 for b in order if bones[b]['name'] in p['scale'])
    profile_summary.append(f'{pname}(mode={p["mode"]} rows={len(p["scale"])} keptListed={listed}/{len(order)} nonzero={sum(1 for w in weights if w != 0)})')

slot_index = {}
out += struct.pack('<I', len(SLOTS))
for i, (sname, mode, pname) in enumerate(SLOTS):
    if pname is not None and pname not in profiles:
        fail(f'slot={sname} 의 비중 표 {pname} 가 {profile_src} 에 없다')
    slot_index[sname] = i
    n = sname.encode('ascii')
    out += struct.pack('<B', len(n)) + n + struct.pack('<Bh', SLOT_MODES[mode], -1 if pname is None else profile_names.index(pname))

out += struct.pack('<I', len(bundles))
scale_max = 0.0
summary = []
for d in bundles:
    n = d['name'].encode('ascii')
    move = d['move']
    out += struct.pack('<B', len(n)) + n
    out += struct.pack('<I', d['hz'])
    out += struct.pack('<6f', *d['aim'])
    out += struct.pack('<5f', float(move['groundFriction']), float(move['brakingFriction']), float(move['brakingFrictionFactor']), float(move['brakingDecelerationWalking']), float(move['gravityZ']))
    out += struct.pack('<B', int(move['separateBraking']))
    out += struct.pack('<I', len(d['clips']))
    total_samples = 0
    total_seconds = 0.0
    extra_seconds = 0.0
    for i in range(len(d['clips'])):
        c = d['clips'][i]
        # 표본 수 = 뽑는 쪽과 같은 float 곱의 올림 + 1. 다르면 뽑은 Hz 와 INFO 의 hz 가 다른 것이다
        want_samples = math.ceil(struct.unpack('<f', struct.pack('<f', c['length'] * d['hz']))[0]) + 1
        if c['samples'] != want_samples:
            fail(f'bundle={d["name"]} clip={c["name"]} samples={c["samples"]} want={want_samples} hz={d["hz"]}')
        n = c['name'].encode('ascii')
        out += struct.pack('<B', len(n)) + n
        out += struct.pack('<fBBfI', c['length'], c['additive'], c['loop'], c['root'], c['samples'])
        m = d['marks'].get(i, [])
        out += struct.pack('<H', len(m))
        for t, name in m:
            if name not in ('L', 'R'):
                fail(f'bundle={d["name"]} clip={c["name"]} marker={name}')
            out += struct.pack('<fB', t, 1 if name == 'L' else 0)
        cv = d['curves'].get(i)
        out += struct.pack('<B', 0 if cv is None else 1)
        if cv is not None:
            if cv['n'] != len(cv['v']):
                fail(f'curve bundle={d["name"]} clip={c["name"]} n={cv["n"]} got={len(cv["v"])}')
            out += struct.pack('<II', cv['hz'], cv['n'])
            out += struct.pack(f'<{cv["n"]}f', *cv['v'])
        want_scale = 0.0 if c['additive'] != 0 else 1.0
        k = d['keys'][i]
        if len(k) != c['samples'] * len(order):
            fail(f'bundle={d["name"]} clip={c["name"]} keys={len(k)} want={c["samples"] * len(order)}')
        for s in range(c['samples']):
            for b in order:
                v = k[(s, b)]
                out += struct.pack('<7f', *v[:7])
                for x in v[7:10]:
                    scale_max = max(scale_max, abs(x - want_scale))
        total_samples += c['samples']
        total_seconds += c['length']
        if i >= CLIPS_PER_BUNDLE:
            extra_seconds += c['length']

    # 몽타주 표 — 종류 차례. 엔진 기능 중 옮기지 않는 것(관성 섞기 방식 · 넣기/빼기 비중 표 · 구역 여럿 · 반복 줄 · 한 자리 여러 토막)이 쓰이면 멈춘다
    out += struct.pack('<I', len(MONTAGE_KINDS))
    dropped = set()
    mline = []
    for k in range(len(MONTAGE_KINDS)):
        m = d['montages'][k]
        tracks = d['tracks'].get(k, [])
        where = f'bundle={d["name"]} montage={m["name"]}'
        # 빼기 비중 표(버전 6, 대시 넷 = FastFeet)는 시간 배율 방식만 싣는다. 넣기 비중 표 · 관성 섞기는 안 옮긴다
        out_profile = m['profileOut'] != 'None'
        if m['modeIn'] != 0 or m['modeOut'] != 0 or m['profileIn'] != 'None' or (out_profile and (m['profileOut'] not in profiles or profiles[m['profileOut']]['mode'] != 0)):
            fail(f'{where} 넣기 · 빼기 방식 {m["modeIn"]}/{m["modeOut"]} 비중 표 {m["profileIn"]}/{m["profileOut"]} — 보통 섞기 · 시간 배율 빼기 비중 표만 옮긴다')
        if m['blendInOption'] > BLEND_OPTION_MAX or m['blendOutOption'] > BLEND_OPTION_MAX:
            fail(f'{where} 섞기 곡선 {m["blendInOption"]}/{m["blendOutOption"]} — 엔진 EAlphaBlendOption 0 · 1 · 2(선형 · Cubic · HermiteCubic)만 옮긴다(Shared/AnimState.cpp BlendCurve)')
        sections = [s for s in m['sections'].split(';') if s]
        if len(sections) != 1 or not sections[0].split('@')[1].startswith('0.0000'):
            fail(f'{where} 구역 {sections} — 0초에서 시작하는 구역 하나만 옮긴다')
        if not tracks or len({t['slot'] for t in tracks}) != len(tracks):
            fail(f'{where} 줄 {[(t["slot"], t["clip"]) for t in tracks]} — 자리마다 토막 하나만 옮긴다')
        n = m['name'].encode('ascii')
        g = m['group'].encode('ascii')
        out += struct.pack('<B', len(n)) + n + struct.pack('<B', k) + struct.pack('<B', len(g)) + g
        out += struct.pack('<ff', m['length'], m['rate'])
        out += struct.pack('<fB', m['blendIn'], m['blendInOption'])
        out += struct.pack('<fB', m['blendOut'], m['blendOutOption'])
        out += struct.pack('<fB', m['trigger'], m['auto'])
        out += struct.pack('<h', profile_names.index(m['profileOut']) if out_profile else -1)
        out += struct.pack('<B', len(tracks))
        for t in tracks:
            if t['slot'] not in slot_index:
                fail(f'{where} 자리 {t["slot"]} 가 끼움 자리 표(SLOTS)에 없다')
            if t['loops'] != 1:
                fail(f'{where} 자리 {t["slot"]} 반복 {t["loops"]}')
            clip = d['clips'][t['clip']]
            additive_slot = SLOTS[slot_index[t['slot']]][1] == 'additive'
            if additive_slot != (clip['additive'] != 0):
                fail(f'{where} 자리 {t["slot"]}(방식 {SLOTS[slot_index[t["slot"]]][1]}) 에 클립 {clip["name"]} 덧붙임={clip["additive"]} — 자리 방식 짐작이 틀렸다')
            if not (0 <= t['clipStart'] < t['clipEnd'] <= clip['length'] + 1e-6):
                fail(f'{where} 클립 {clip["name"]} 구간 {t["clipStart"]}~{t["clipEnd"]} 이 길이 {clip["length"]} 밖')
            # 클립 끝은 파일의 클립 길이(float)를 넘지 않게 — 몽타주가 클립 끝을 그대로 가리키는 경우 반올림 차이만
            clip_end = min(t['clipEnd'], struct.unpack('<f', struct.pack('<f', clip['length']))[0])
            out += struct.pack('<BHffff', slot_index[t['slot']], t['clip'], t['startPos'], t['clipStart'], clip_end, t['rate'])
        # 곡선 — 몽타주 자체(-1)와 줄 클립(줄 번호) 것 중 PACK_CURVES 이름만
        curves = []
        for cv in d['mcurves'].get(k, []):
            (curves.append((cv, -1)) if cv['name'] in PACK_CURVES else dropped.add(cv['name']))
        for ti, t in enumerate(tracks):
            for cv in d['scurves'].get(t['clip'], []):
                (curves.append((cv, ti)) if cv['name'] in PACK_CURVES else dropped.add(cv['name']))
        out += struct.pack('<B', len(curves))
        for cv, owner in curves:
            if cv['n'] != len(cv['v']) or cv['n'] == 0:
                fail(f'{where} 곡선 {cv["name"]} n={cv["n"]} got={len(cv["v"])}')
            cn = cv['name'].encode('ascii')
            out += struct.pack('<B', len(cn)) + cn + struct.pack('<bII', owner, cv['hz'], cv['n'])
            out += struct.pack(f'<{cv["n"]}f', *cv['v'])
        mline.append(f'{MONTAGE_KINDS[k]}={m["name"]}[' + ' '.join(f'{t["slot"]}:{d["clips"][t["clip"]]["name"]}#{t["clip"]}' for t in tracks) + ']'
                     + ('' if not curves else '{' + ' '.join(f'{cv["name"]}@{owner}max={max(cv["v"]):.3f}' for cv, owner in curves) + '}'))
    ov = d['override']
    if not (0 <= ov < len(d['clips'])):
        fail(f'bundle={d["name"]} 덮기 자세 클립 번호 {ov}')
    if d['clips'][ov]['additive'] != 0:
        fail(f'bundle={d["name"]} 덮기 자세 {d["clips"][ov]["name"]} 가 덧붙임 클립이다')
    out += struct.pack('<hB', ov, slot_index[OVERRIDE_SLOT])
    summary.append((d, total_samples, total_seconds, extra_seconds, mline, sorted(dropped)))
if scale_max > SCALE_LIMIT:
    fail(f'크기가 움직이는 클립이 있다 scaleDiffMax={scale_max}')
out += b'MOAE'
open(dst, 'wb').write(out)
print(f'CLIPPACK ok version={VERSION} bundles={len(bundles)} bones={len(order)} bodies={len(body["bodies"])} bytes={len(out)} scaleDiffMax={scale_max:.6f}')
print(f'CLIPPACK profiles ' + ' '.join(profile_summary))
print(f'CLIPPACK slots ' + ' '.join(f'{s}={m}/{p or "-"}' for s, m, p in SLOTS))
for d, samples, seconds, extra, mline, dropped in summary:
    print(f'CLIPPACK bundle={d["name"]} clips={len(d["clips"])} hz={d["hz"]} samples={samples} seconds={seconds:.2f} extraSeconds={extra:.2f} src={d["src"]}')
    print(f'CLIPPACK bundle={d["name"]} additive ' + ' '.join(f'{c["name"]}={c["additive"]}' for c in d['clips'].values() if c['additive'] != 0))
    print(f'CLIPPACK bundle={d["name"]} move ' + ' '.join(f'{k}={v}' for k, v in d['move'].items()))
    print(f'CLIPPACK bundle={d["name"]} montages ' + ' '.join(mline) + f' override={d["clips"][d["override"]]["name"]}#{d["override"]}')
    print(f'CLIPPACK bundle={d["name"]} droppedCurves ' + (' '.join(dropped) if dropped else '-'))
