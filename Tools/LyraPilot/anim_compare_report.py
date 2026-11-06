#!/usr/bin/env python3
"""Compare two Saved/MOB/animcmp_<tag>.csv recordings of the same scripted run, segment by segment.

usage: anim_compare_report.py A.csv B.csv

Runs can start from different spawn points and headings, so head and pelvis are compared in the body's own
frame (relative to the capsule, turned by the row's yaw), and the capsule is compared relative to where each
pass started. A capsule that drifted apart is reported on its own column, not counted as a pose difference.
"""
import csv
import math
import sys
from collections import OrderedDict


def load(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def vec(row, prefix):
    return tuple(float(row[prefix + axis]) for axis in "XYZ")


def body(row):
    return tuple(float(row[axis]) for axis in "xyz")


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def turned(v, yaw_degrees):
    c, s = math.cos(math.radians(-yaw_degrees)), math.sin(math.radians(-yaw_degrees))
    return (v[0] * c - v[1] * s, v[0] * s + v[1] * c, v[2])


def relative(row, prefix):
    delta = tuple(p - o for p, o in zip(vec(row, prefix), body(row)))
    return turned(delta, float(row["yaw"]))


def path(row, first):
    delta = tuple(p - o for p, o in zip(body(row), body(first)))
    return turned(delta, float(first["yaw"]))


def percentile(values, q):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(len(ordered) * q))]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a_rows, b_rows = load(sys.argv[1]), load(sys.argv[2])
    print(f"rows a={len(a_rows)} b={len(b_rows)}")
    if not a_rows or not b_rows:
        print("no rows: nothing was compared")
        return 1

    first_a, first_b = {}, {}
    groups = OrderedDict()
    mismatched = 0
    for a, b in zip(a_rows, b_rows):
        first_a.setdefault(a["pass"], a)
        first_b.setdefault(b["pass"], b)
        if (a["pass"], a["segment"]) != (b["pass"], b["segment"]):
            mismatched += 1
            continue
        groups.setdefault((a["pass"], a["segment"]), []).append((a, b))
    print(f"compared={sum(len(v) for v in groups.values())} segment_mismatch={mismatched}")

    print("pass segment rows | head mean p95 max | pelvis mean | path max | speed a b")
    all_head = []
    for (pass_no, name), pairs in groups.items():
        head = [dist(relative(a, "head"), relative(b, "head")) for a, b in pairs]
        pelvis = [dist(relative(a, "pelvis"), relative(b, "pelvis")) for a, b in pairs]
        drift = [dist(path(a, first_a[pass_no]), path(b, first_b[pass_no])) for a, b in pairs]
        speed_a = max(math.hypot(float(a["velX"]), float(a["velY"])) for a, _ in pairs)
        speed_b = max(math.hypot(float(b["velX"]), float(b["velY"])) for _, b in pairs)
        all_head += head
        print(f"{pass_no:>4} {name:<7} {len(pairs):>4} | {sum(head)/len(head):6.2f} {percentile(head, .95):6.2f} {max(head):6.2f}"
              f" | {sum(pelvis)/len(pelvis):6.2f} | {max(drift):7.2f} | {speed_a:5.0f} {speed_b:5.0f}")
    print(f"overall head rows={len(all_head)} mean={sum(all_head)/len(all_head):.3f} p95={percentile(all_head, .95):.3f} max={max(all_head):.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
