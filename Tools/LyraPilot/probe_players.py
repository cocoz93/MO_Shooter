#!/usr/bin/env python3
"""Per-frame weight (and time) of each playing clip, pass 0, for a segment range.
usage: probe_players.py <probe.csv> <seg>[:from:to],... [--time]
"""
import csv
import sys

path, segs = sys.argv[1], sys.argv[2].split(",")
show_time = "--time" in sys.argv
with open(path, newline="", encoding="utf-8") as f:
    rows = [r for r in csv.DictReader(f) if r["pass"] == "0"]
picked = []
for s in segs:
    name, *rng = s.split(":")
    lo, hi = (float(rng[0]), float(rng[1])) if rng else (-1, 99)
    picked += [r for r in rows if r["segment"] == name and lo <= float(r["segTime"]) <= hi]
clips = []
parsed = []
for r in picked:
    d = {}
    for item in r["players"].split(";"):
        if "@" not in item:
            continue
        clip, rest = item.split("@")
        t, w = rest.split("*")
        if clip.startswith("AO_") or clip.startswith("BS_"):
            continue
        key = clip
        n = 2
        while key in d:
            key = f"{clip}#{n}"
            n += 1
        d[key] = (float(t), float(w))
        if key not in clips:
            clips.append(key)
    parsed.append((r, d))
short = [c.replace("MM_Rifle_", "").replace("MF_Rifle_", "F_").replace("Mf_Rifle_", "F_") for c in clips]
print("frame,seg,t," + ",".join(short))
for r, d in parsed:
    cells = []
    for c in clips:
        if c in d:
            t, w = d[c]
            cells.append(f"{w:.2f}" + (f"@{t:.3f}" if show_time else ""))
        else:
            cells.append("")
    print(f"{r['frame']},{r['segment']},{float(r['segTime']):.3f}," + ",".join(cells))
