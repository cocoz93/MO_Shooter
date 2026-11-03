#!/usr/bin/env python3
# 자세 측정 정리 — 고정 자세(Shared/BodyPose.inl)와 Lyra 실제 자세의 어긋남(cm)·겹침을 무기 × 동작으로 낸다
#   python pose_report.py <weaponrec_태그.csv> <맵.mocl> [출력.md]
# 기록은 Tools/LyraPilot/record_weapons.ps1 -PoseOnly 로 뜬다. 상태 번호 = 무기*10 + 동작
#
# 어긋남 = 표본 프레임마다 몸체 22개 중심의 거리(고정 자세 ↔ 실제 자세, 캡슐 중심 · 요 0 기준)
# 겹침   = Lyra 광선과 고정 자세 광선이 「둘 중 하나라도 몸에 맞은」 것 중 「둘 다 맞은」 비율. WeaponCheck 의 RAYS 줄을 그대로 쓴다
#          WeaponCheck 는 표·발사 기록이 없어 RESULT FAIL 로 끝나지만 광선 대조는 그 앞에서 정상으로 돈다. 여기서는 RAYS 줄만 읽는다
#          반지름 0(선)을 앞에 두는 까닭 = 탄이 먼저 선으로 판정되고, 서버 판정의 큰 몫이 선 결과라서
import math
import os
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
WEAPONS = {0: '권총', 1: '소총', 2: '샷건'}
MOTIONS = {0: '서 있기', 1: '걷기', 2: '점프', 3: '제자리 회전', 4: '위아래 조준(실제 설정)', 5: '위아래 조준(컨트롤러 멈춤)'}


def load_fixed():
    out = {}
    pattern = re.compile(r'\{\s*"(\w+)",\s*EBodyShape::\w+,\s*\w+,\s*\{\s*(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*(-?[\d.]+)f')
    for line in open(os.path.join(REPO, 'Shared', 'BodyPose.inl'), encoding='utf-8-sig'):
        m = pattern.search(line)
        if m:
            out[m.group(1)] = tuple(float(m.group(i)) for i in (2, 3, 4))
    return out


def group_of(bone):
    if bone.startswith('head') or bone.startswith('neck'):
        return '머리'
    if any(k in bone for k in ('pelvis', 'spine')):
        return '몸통'
    if any(k in bone for k in ('arm', 'hand', 'clavicle')):
        return '팔'
    return '다리'


def percentile(sorted_values, p):
    if not sorted_values:
        return 0.0
    return sorted_values[min(len(sorted_values) - 1, int(len(sorted_values) * p))]


def main():
    csv_path, map_path = sys.argv[1], sys.argv[2]
    out_path = sys.argv[3] if len(sys.argv) > 3 else None
    fixed = load_fixed()

    falling = {}
    samples = {}	# 상태 → 프레임 → 몸체 → 거리
    weapons_ok = {}
    unknown_bones = set()
    meshes = set()
    for line in open(csv_path, encoding='utf-8', errors='replace'):
        if line.startswith('POSE,'):
            c = line.split(',')
            state, frame, bone = int(c[1]), int(c[2]), c[3]
            base = fixed.get(bone)
            if base is None:
                unknown_bones.add(bone)
                continue
            x, y, z = float(c[6]), float(c[7]), float(c[8])
            d = math.dist((x, y, z), base)
            samples.setdefault(state, {}).setdefault(frame, {})[bone] = d
        elif line.startswith('BOT,'):
            c = line.split(',')
            falling[(int(c[1]), int(c[2]))] = c[10].strip() == '1'
        elif line.startswith('POSEMESH,'):
            c = line.strip().split(',')
            meshes.add(f'{c[2]}={c[3]}')
        elif line.startswith('POSEWEAPON,'):
            c = line.strip().split(',')
            weapons_ok[int(c[1])] = c[3] == '1'

    tool = os.path.join(REPO, 'build', 'Tools', 'WeaponCheck', 'Release', 'WeaponCheck.exe')
    check = subprocess.run([tool, csv_path, map_path], capture_output=True)
    text = check.stdout.decode('utf-8', errors='replace')
    rays = {}
    for m in re.finditer(r'RAYS\s+state=(\d+) r=([\d.]+) n=(\d+) .*?body either=(\d+) both=(\d+)', text):
        rays.setdefault(int(m.group(1)), {})[float(m.group(2))] = (int(m.group(3)), int(m.group(4)), int(m.group(5)))

    lines = []
    lines.append('| 무기 | 동작 | 표본 프레임 | 공중 비율 | 어긋남 평균 | 어긋남 p95 | 가장 큰 몸체 값 | 머리 | 몸통 | 팔 | 다리 | 겹침(선) | 겹침(전체) | 광선 n · 몸 맞음 |')
    lines.append('|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|')
    for state in sorted(set(samples) | set(rays)):
        frames = samples.get(state, {})
        per_frame = []
        groups = {}
        peak = 0.0
        for bones in frames.values():
            per_frame.append(sum(bones.values()) / len(bones))
            for bone, d in bones.items():
                groups.setdefault(group_of(bone), []).append(d)
                peak = max(peak, d)
        per_frame.sort()
        mean = sum(per_frame) / len(per_frame) if per_frame else 0.0
        air = [falling.get((state, f), False) for f in frames]
        air_rate = sum(air) / len(air) if air else 0.0
        r = rays.get(state, {})
        line_n, line_either, line_both = r.get(0.0, (0, 0, 0))
        all_n = sum(v[0] for v in r.values())
        all_either = sum(v[1] for v in r.values())
        all_both = sum(v[2] for v in r.values())
        overlap_line = f'{100.0 * line_both / line_either:.1f}%' if line_either else '-'
        overlap_all = f'{100.0 * all_both / all_either:.1f}%' if all_either else '-'
        group_text = [f'{sum(groups[g]) / len(groups[g]):.1f}' if g in groups else '-' for g in ('머리', '몸통', '팔', '다리')]
        lines.append(f'| {WEAPONS.get(state // 10, state // 10)} | {MOTIONS.get(state % 10, state % 10)} | {len(frames)} | {air_rate * 100:.0f}% | {mean:.1f} | {percentile(per_frame, 0.95):.1f} | {peak:.1f} | ' + ' | '.join(group_text) + f' | {overlap_line} | {overlap_all} | {all_n} · {all_either} |')
    lines.append('')
    lines.append(f'몸 종류: {sorted(meshes) or "기록 없음"}')
    lines.append(f'무기 장착 기록: {weapons_ok} · 표에 없는 몸체: {sorted(unknown_bones) or "없음"} · WeaponCheck rc={check.returncode}(자세 기록은 표 줄이 없어 1 이 정상)')
    report = '\n'.join(lines) + '\n'
    sys.stdout.reconfigure(encoding='utf-8')
    print(report)
    if out_path:
        with open(out_path, 'w', encoding='utf-8') as f:
            f.write(report)
    return 0 if samples and rays else 1


if __name__ == '__main__':
    sys.exit(main())
