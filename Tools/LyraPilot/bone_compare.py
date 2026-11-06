#!/usr/bin/env python3
"""Per-segment bone distance (cm) between two runs, pass 0, frames matched by index.
usage:
  bone_compare.py probe <a_probe.csv> <b_probe.csv>      body-local bones from -MOBACProbe files
  bone_compare.py head  <a.csv> <b.csv> [<c.csv> <d.csv>]  head offset from actor (main csv), optionally a second pair
"""
import csv
import math
import sys

BONES = ["head", "hand_r", "hand_l", "foot_l", "foot_r", "pelvis"]


def load(path):
    with open(path, newline="", encoding="utf-8") as f:
        return [r for r in csv.DictReader(f) if r["pass"] == "0"]


def seg_order(rows):
    out = []
    for r in rows:
        if not out or out[-1] != r["segment"]:
            out.append(r["segment"])
    return out


def head_local(r):
    yaw = math.radians(float(r["yaw"]))
    dx = float(r["headX"]) - float(r["x"])
    dy = float(r["headY"]) - float(r["y"])
    dz = float(r["headZ"]) - float(r["z"])
    c, s = math.cos(-yaw), math.sin(-yaw)
    return (dx * c - dy * s, dx * s + dy * c, dz)


def stats(pairs):
    if not pairs:
        return "-"
    ds = [math.dist(a, b) for a, b in pairs]
    return f"{sum(ds) / len(ds):5.1f}/{max(ds):5.1f}"


mode = sys.argv[1]
if mode == "probe":
    a, b = load(sys.argv[2]), load(sys.argv[3])
    n = min(len(a), len(b))
    print(f"frames a={len(a)} b={len(b)} used={n}   cells = mean/max cm")
    print("segment".ljust(10) + "".join(x.rjust(13) for x in BONES))
    for seg in seg_order(a):
        idx = [i for i in range(n) if a[i]["segment"] == seg and b[i]["segment"] == seg]
        cells = []
        for bone in BONES:
            pairs = [(tuple(float(a[i][f"{bone}.{k}"]) for k in "xyz"), tuple(float(b[i][f"{bone}.{k}"]) for k in "xyz")) for i in idx]
            cells.append(stats(pairs).rjust(13))
        print(seg.ljust(10) + "".join(cells) + f"  n={len(idx)}")
else:
    pairs_list = [(load(sys.argv[i]), load(sys.argv[i + 1])) for i in range(2, len(sys.argv), 2)]
    print("head offset from body, mean/max cm per segment: " + " | ".join(f"pair{k}" for k in range(len(pairs_list))))
    for seg in seg_order(pairs_list[0][0]):
        cells = []
        for a, b in pairs_list:
            n = min(len(a), len(b))
            idx = [i for i in range(n) if a[i]["segment"] == seg and b[i]["segment"] == seg]
            cells.append(stats([(head_local(a[i]), head_local(b[i])) for i in idx]).rjust(13))
        print(seg.ljust(10) + "".join(cells))
