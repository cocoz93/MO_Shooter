#!/usr/bin/env python3
"""Put a record_remote_shot.ps1 recording next to the passed animation comparison, same motion on the same frame.

usage: make_remote_compare.py <ffmpeg> <Saved/MOB dir> <remote tag> <reference tag> <out dir> [--check] [--left=<label>]

The reference ran on a fixed clock at 60 frames per second; the remote shot ran on real time and stamped each frame.
Matches the two routes by position, then picks the nearest remote frame for every reference frame at 30 frames per second.
--check only prints the route comparison and writes no video.
--left names the left picture. The events route reference (ev_<weapon>) is Lyra's own record, not the passed simple playback: pass --left="Lyra 원본".
"""
import csv
import math
import os
import subprocess
import sys

from make_anim_compare import FONT, SEGMENT_LABELS, caption

LEFT_LABEL = "합격본 (단순 재생)"
RIGHT_LABEL = "서버 구동"
RECORD_FPS = 60.0
VIDEO_FPS = 30
BYTES_PER_PIXEL = 4


def read_reference(path):
    rows = [r for r in csv.DictReader(open(path, newline="")) if r["pass"] == "0"]
    return [(i / RECORD_FPS, float(r["x"]), float(r["y"]), float(r["z"]), r["segment"]) for i, r in enumerate(rows)]


def read_remote(path):
    return [(float(r["seconds"]), float(r["x"]), float(r["y"]), float(r["z"])) for r in csv.DictReader(open(path, newline=""))]


def at_time(remote, t, start=0):
    """Remote position at time t by straight-line interpolation. Returns (x, y, z, index) with index for the next search."""
    i = start
    while i + 1 < len(remote) and remote[i + 1][0] < t:
        i += 1
    if i + 1 >= len(remote) or t <= remote[i][0]:
        return remote[i][1], remote[i][2], remote[i][3], i
    a, b = remote[i], remote[i + 1]
    k = (t - a[0]) / (b[0] - a[0])
    return a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k, a[3] + (b[3] - a[3]) * k, i


def route_error(reference, remote, offset):
    """Horizontal distances between the routes when reference time t is remote time t + offset."""
    out, i = [], 0
    for t, x, y, _z, _s in reference:
        rx, ry, _rz, i = at_time(remote, t + offset, i)
        out.append(math.hypot(rx - x, ry - y))
    return out


def best_offset(reference, remote):
    moved = next(r[0] for r in remote if math.hypot(r[1] - remote[0][1], r[2] - remote[0][2]) > 3.0)
    ref_moved = next(r[0] for r in reference if math.hypot(r[1] - reference[0][1], r[2] - reference[0][2]) > 3.0)
    guess = moved - ref_moved
    candidates = [guess + (k - 150) * 0.002 for k in range(301)]	# 0.3 seconds either way, 2 ms steps
    return min(candidates, key=lambda o: sum(d * d for d in route_error(reference, remote, o)))


def segments(reference):
    spans, start = [], 0
    for i in range(1, len(reference) + 1):
        if i == len(reference) or reference[i][4] != reference[start][4]:
            spans.append((SEGMENT_LABELS[reference[start][4]], reference[start][0], i / RECORD_FPS))
            start = i
    return spans


def main():
    args = [a for a in sys.argv[1:] if a != "--check" and not a.startswith("--left=")]
    check_only = "--check" in sys.argv
    left_label = next((a[len("--left="):] for a in sys.argv[1:] if a.startswith("--left=")), LEFT_LABEL)
    if len(args) != 5:
        print(__doc__)
        return 2
    ffmpeg, mob, remote_tag, reference_tag, out = args
    reference = read_reference(os.path.join(mob, f"animcmp_{reference_tag}.csv"))
    remote = read_remote(os.path.join(mob, f"remoteshot_{remote_tag}.csv"))

    gaps = sorted(b[0] - a[0] for a, b in zip(remote, remote[1:]))
    print(f"remote frames={len(remote)} seconds={remote[-1][0] - remote[0][0]:.2f} gap median={gaps[len(gaps) // 2] * 1000:.1f}ms p99={gaps[int(len(gaps) * 0.99)] * 1000:.1f}ms max={gaps[-1] * 1000:.1f}ms")
    print(f"remote z min={min(r[3] for r in remote):.2f} max={max(r[3] for r in remote):.2f}")
    offset = best_offset(reference, remote)
    errors = route_error(reference, remote, offset)
    print(f"offset={offset:.3f}s route error mean={sum(errors) / len(errors):.2f}cm max={max(errors):.2f}cm samples={len(errors)}")
    worst = {}
    for (_t, _x, _y, _z, name), d in zip(reference, errors):
        worst[name] = max(worst.get(name, 0.0), d)
    print("worst per segment:", " ".join(f"{k}={v:.1f}" for k, v in worst.items()))
    if offset < remote[0][0]:
        print(f"WARNING remote starts {remote[0][0] - offset:.2f}s late")
    if reference[-1][0] + offset > remote[-1][0]:
        print(f"WARNING remote ends {reference[-1][0] + offset - remote[-1][0]:.2f}s early")
    if check_only:
        return 0

    os.makedirs(out, exist_ok=True)
    size_line = next(l for l in open(os.path.join(mob, f"remoteshot_{remote_tag}.log"), encoding="utf-8", errors="replace") if "MOB_REMOTESHOT ready" in l)
    width, height = (int(v) for v in size_line.rsplit("size=", 1)[1].strip().split("x"))
    frame_bytes = width * height * BYTES_PER_PIXEL
    raw_path = os.path.join(mob, f"rec_remoteshot_{remote_tag}.bgra")
    available = os.path.getsize(raw_path) // frame_bytes
    if available != len(remote):
        print(f"WARNING raw frames={available} csv rows={len(remote)}")

    remote_mp4 = os.path.join(mob, f"remoteshot_{remote_tag}.mp4")
    encode = ["-c:v", "libx264", "-crf", "20", "-preset", "veryfast", "-pix_fmt", "yuv420p"]
    count = int(reference[-1][0] * VIDEO_FPS) + 1
    pipe = subprocess.Popen([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgra", "-s", f"{width}x{height}",
                             "-r", str(VIDEO_FPS), "-i", "-", *encode, remote_mp4], stdin=subprocess.PIPE)
    repeated, i = 0, 0
    last = -1
    with open(raw_path, "rb") as raw:
        for k in range(count):
            t = k / VIDEO_FPS + offset
            while i + 1 < min(len(remote), available) and abs(remote[i + 1][0] - t) <= abs(remote[i][0] - t):
                i += 1
            repeated += 1 if i == last else 0
            last = i
            raw.seek(i * frame_bytes)
            pipe.stdin.write(raw.read(frame_bytes))
    pipe.stdin.close()
    if pipe.wait() != 0:
        return 1
    print(f"output frames={count} repeated={repeated}")

    labels = ",".join(caption(t, "(w-text_w)/2", "h-60", 34, f":enable='between(t,{a:.3f},{b:.3f})'") for t, a, b in segments(reference))
    graph = (f"[0:v]scale=960:540,{caption(left_label, 20, 16, 30)}[a];"
             f"[1:v]scale=960:540,{caption(RIGHT_LABEL, 20, 16, 30)}[b];"
             f"[a][b]hstack=shortest=1,{labels}[v]")
    script = os.path.join(out, "_filter.txt")
    with open(script, "w", encoding="utf-8") as f:
        f.write(graph)
    near, slow = os.path.join(out, "near.mp4"), os.path.join(out, "near_half_speed.mp4")
    subprocess.check_call([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", os.path.join(mob, f"animcmp_{reference_tag}.mp4"), "-i", remote_mp4,
                           "-filter_complex_script", script, "-map", "[v]", *encode, near])
    subprocess.check_call([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", near, "-vf", "setpts=2*PTS", *encode, slow])
    os.remove(script)
    for path in (near, slow):
        print(path, os.path.getsize(path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
