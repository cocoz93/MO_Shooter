"""Decal fade-in from the fx probe screenshots (record_fx_probe.ps1 / MOBFxProbeSubsystem).

For each weapon: decal pixels = where the last shot (240 frames after the press) differs in brightness from the shot before
the press, inside a box around the aim point (screen centre); isolated noisy pixels are dropped (a pixel needs 4+ changed
neighbours). The hole can be darker (pink wall, fx2) or lighter (red wall, fx3) than the wall, so the sign is not assumed.
Visible = per-pixel change from 'before' with the ring's mean change taken off (the ring just outside the decal cancels
whole-screen brightness changes: muzzle flash light, exposure), projected on the final change; 0 = nothing, 1 = as at the last
shot. Dust or sparks over the decal pull it away from 1, as they do for the eye.
Control: the same number on a patch of plain wall away from the decal (moved 40 px) - should stay near 0.
Writes a strip image (crop around the decal, each frame) and prints the curve.
usage: python -I check_fx_probe.py <tag>   (reads Client/Lyra/Saved/MOB/fx_<tag>_*.png, writes fx_<tag>_strip.png there)
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

MOB = Path(__file__).resolve().parents[2] / 'Client' / 'Lyra' / 'Saved' / 'MOB'
OFFSETS = [1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 18, 21, 24, 27, 30, 36, 42, 48, 60, 75, 90, 105, 120, 150, 180, 240]
BOX = (600, 260, 800, 460)      # around the aim point (640,360); left edge kept off the character's gun (x < 590 in fx2)
CHANGED = 18                     # brightness change (0-255) at the last shot that counts as decal (TAA noise on the wall stays under ~10)
LAST = OFFSETS[-1]


def load(tag, w, name):
    return np.asarray(Image.open(MOB / f'fx_{tag}_{w}_{name}.png').convert('RGB'), dtype=np.float32)


def dilate(m, r):
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out


def main(tag):
    strips = []
    for w in ['pistol', 'rifle', 'shotgun']:
        x0, y0, x1, y1 = BOX
        lum = lambda a: a @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
        b = lum(load(tag, w, 'before')[y0:y1, x0:x1])
        e = lum(load(tag, w, f'{LAST:03d}')[y0:y1, x0:x1])
        chg = np.abs(e - b) > CHANGED
        n = sum(np.roll(np.roll(chg, dy, 0), dx, 1) for dy in (-1, 0, 1) for dx in (-1, 0, 1)) - chg
        decal = chg & (n >= 4)
        ring = dilate(decal, 7) & ~dilate(decal, 3)
        ctrl_d = np.roll(decal, 40, 0)
        ctrl_r = np.roll(ring, 40, 0)
        ys, xs = np.nonzero(decal)
        if len(xs) == 0:
            print(f'{w}: no decal pixels')
            continue

        def change(img, d, r):
            x = img - b
            return x[d] - x[r].mean()

        de = change(e, decal, ring)
        den = (de ** 2).sum()
        print(f'{w}: decal px={decal.sum()} bbox=({xs.min()+x0},{ys.min()+y0})-({xs.max()+x0},{ys.max()+y0}) mean |change| at last={np.abs(de).mean():.1f}')
        line, ctrl = [], []
        for o in OFFSETS:
            f = lum(load(tag, w, f'{o:03d}')[y0:y1, x0:x1])
            line.append(f'{o}:{(change(f, decal, ring) * de).sum() / den:.2f}')
            ctrl.append(f'{o}:{(change(f, ctrl_d, ctrl_r) * de).sum() / den:.2f}')
        print('  frame:visible  ' + ' '.join(line))
        print('  control patch  ' + ' '.join(ctrl))
        # strip: crop around the decal centre, 4x
        cx, cy = int(xs.mean()) + x0, int(ys.mean()) + y0
        half = 28 if w != 'shotgun' else 48
        names = ['before'] + [f'{o:03d}' for o in OFFSETS]
        tiles = []
        for n in names:
            im = Image.open(MOB / f'fx_{tag}_{w}_{n}.png').convert('RGB').crop((cx - half, cy - half, cx + half, cy + half))
            im = im.resize((half * 2 * 3, half * 2 * 3), Image.NEAREST)
            dr = ImageDraw.Draw(im)
            dr.rectangle((0, 0, 52, 14), fill=(0, 0, 0))
            dr.text((2, 1), n if n == 'before' else f'+{int(n)}f', fill=(255, 255, 0))
            tiles.append(im)
        tw = tiles[0].width
        cols = 11
        rows = (len(tiles) + cols - 1) // cols
        s = Image.new('RGB', (tw * cols, tw * rows + 18), (30, 30, 30))
        ImageDraw.Draw(s).text((4, 3), f'{w}  (60 fps fixed - +30f = 0.5 s)', fill=(255, 255, 255))
        for i, t in enumerate(tiles):
            s.paste(t, ((i % cols) * tw, (i // cols) * tw + 18))
        strips.append(s)
    W = max(s.width for s in strips)
    out = Image.new('RGB', (W, sum(s.height for s in strips)), (30, 30, 30))
    y = 0
    for s in strips:
        out.paste(s, (0, y))
        y += s.height
    p = MOB / f'fx_{tag}_strip.png'
    out.save(p)
    print('strip', p)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'fx2')
