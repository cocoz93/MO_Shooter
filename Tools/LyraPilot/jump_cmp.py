#!/usr/bin/env python3
"""Line up a jump, frame by frame: what Lyra played (state machine, asset players, DisableLegIK curve) next to what our
CAnimState stacked (clip, time, blend weight) and how far the hit-body pelvis ended up from Lyra's.

usage: jump_cmp.py <ours_dir> <tag_weapon> [--before=10] [--after=70] [--jump=N]

  ours_dir   AnimCheck state output with --stackDump and --plantDump: <tag_weapon>_0.csv / .stack.csv / .plant.csv
  tag_weapon e.g. crouch_rifle — Lyra side is Saved/MOB/animcmp_<tag>_<weapon>_probe.csv and _shapes.csv
  --jump=N   only the N-th jump of the recording (0-based); default all

A jump starts at the first frame Lyra reports IsOnGround=0 after a grounded frame. Our side keeps the last update of each
recorded frame (the dump has one row per update). Our weights use the same rule as CAnimState::JumpWeights: from the top
slot down, each slot takes smoothstep(fade) of what is left, the bottom slot takes the rest. Slot -1 is the walk/idle
pose. Pelvis numbers are hit-body pelvis z (POSEB vs POSE rows); 'pre' removes our foot-placement offset.
"""
import csv
import sys

MOB = 'C:/Users/USER/Desktop/MyGit/MO_Shooter/Client/Lyra/Saved/MOB'
JUMP = ['JumpStart', 'StartLoop', 'Apex', 'FallLoop', 'Land']
SETS = ['hip', 'ads', 'crouch']
KINDS = ['start', 'stop', 'pivot']
DIRS = ['F', 'B', 'L', 'R']


def clip_name(c):
    if c == -1:
        return 'walk'
    if 0 <= c < 5:
        return JUMP[c]
    if 5 <= c < 41:
        n = c - 5
        return f'{SETS[n // 12]}.{KINDS[(n % 12) // 4]}{DIRS[n % 4]}'
    if c == 41:
        return 'StanceEntry'
    if c == 42:
        return 'StanceExit'
    return f'?{c}'


def smooth(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3.0 - 2.0 * x)


def stack_weights(layers):
    out = [0.0] * len(layers)
    remain = 1.0
    for i in range(len(layers) - 1, -1, -1):
        alpha = 1.0 if i == 0 else smooth(layers[i][2])
        out[i] = remain * alpha
        remain -= out[i]
    return out


def load_probe(tw):
    csv.field_size_limit(10 ** 8)
    rows = {}
    with open(f'{MOB}/animcmp_{tw}_probe.csv', newline='', encoding='utf-8', errors='replace') as f:
        r = csv.reader(f)
        h = next(r)
        ix = {k: i for i, k in enumerate(h)}
        b = 'ABP_Mannequin_Base.'
        for row in r:
            key = (int(row[ix['pass']]), int(row[ix['frame']]))
            curve = ''
            for c in row[ix['curves']].split(';'):
                if c.startswith('DisableLegIK='):
                    curve = c.split('=')[1]
            players = [p for p in row[ix['players']].split(';') if p and not p.startswith('AO_')]
            rows[key] = {
                'ground': row[ix[b + 'IsOnGround']] == '1',
                'sm': row[ix['sm:' + b + 'LocomotionSM']],
                'apex': float(row[ix[b + 'TimeToJumpApex']]),
                'gd': float(row[ix[b + 'GroundDistance']]),
                'curve': curve,
                'players': players,
            }
    return rows


def load_pelvis(path, tag):
    out = {}
    with open(path, encoding='utf-8-sig') as f:
        for row in csv.reader(f):
            if row[0] != tag or row[3] != 'pelvis':
                continue
            out[(int(row[1]) // 100, int(row[2]))] = float(row[8])
    return out


def load_last(path):
    out = {}
    with open(path, encoding='utf-8-sig') as f:
        for row in csv.DictReader(f):
            out[(int(row['pass']), int(row['frame']))] = row
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
    if len(args) != 2:
        print(__doc__)
        return 2
    ours_dir, tw = args
    before = int(opts.get('before', 10))
    after = int(opts.get('after', 70))
    only = int(opts['jump']) if 'jump' in opts else None

    probe = load_probe(tw)
    lyra_pel = load_pelvis(f'{MOB}/animcmp_{tw}_shapes.csv', 'POSEB')
    ours_pel = load_pelvis(f'{ours_dir}/{tw}_0.csv', 'POSE')
    stack = load_last(f'{ours_dir}/{tw}_0.stack.csv')
    plant = load_last(f'{ours_dir}/{tw}_0.plant.csv')

    starts = [k for k in sorted(probe) if not probe[k]['ground'] and probe.get((k[0], k[1] - 1), {'ground': False})['ground']]
    print(f'{tw} jumps={len(starts)} starts={[k[1] for k in starts]}')
    for n, (p, f0) in enumerate(starts):
        if only is not None and n != only:
            continue
        print(f'--- jump {n} pass={p} takeoff frame={f0}')
        for f in range(f0 - before, f0 + after + 1):
            lr = probe.get((p, f))
            if lr is None:
                continue
            st = stack.get((p, f))
            ours = ''
            if st:
                cnt = int(st['count'])
                layers = [(int(st[f'clip{k}']), float(st[f'time{k}']), float(st[f'fade{k}'])) for k in range(cnt)]
                w = stack_weights(layers)
                ours = ' '.join(f'{clip_name(c)}@{t:.3f}*{wt:.2f}' for (c, t, _), wt in zip(layers, w))
                ours += f" | crouch={float(st['crouchBlend']):.2f} ads={float(st['adsBlend']):.2f} plantA={float(st['plantAlpha']):.3f}"
            pel = ''
            if (p, f) in lyra_pel and (p, f) in ours_pel:
                off = float(plant[(p, f)]['offsetZ']) if (p, f) in plant else 0.0
                d = ours_pel[(p, f)] - lyra_pel[(p, f)]
                pel = f'pel d={d:+6.2f} pre={ours_pel[(p, f)] - off:8.2f}'
            print(f"{f} {'G' if lr['ground'] else 'A'} {lr['sm']:<14} apexT={lr['apex']:.3f} gd={lr['gd']:6.1f} leg={lr['curve'] or '-':<6} {pel:<30} | lyra {' '.join(lr['players'])} || ours {ours}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
