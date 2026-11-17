#!/usr/bin/env python3
"""Join two record_anim_compare.ps1 recordings side by side, one video per camera distance plus a half-speed close one.

usage: make_anim_compare.py <ffmpeg> <Saved/MOB dir> <tagA> <tagB> <out dir>

Reads animcmp_<tag>.mp4 and the segment timing from animcmp_<tagA>.csv. Captions name the motion on screen.
"""
import csv
import os
import subprocess
import sys

FONT = "C\\:/Windows/Fonts/malgun.ttf"
LEFT_LABEL = "Lyra 원본"
RIGHT_LABEL = "단순 재생"
SEGMENT_LABELS = {
    "idle": "서 있기", "fwd": "앞으로", "stop1": "멈춤", "left": "왼쪽으로", "right": "오른쪽으로", "stop2": "멈춤",
    "bwd": "뒤로", "flip1": "앞뒤 급전환", "flip2": "앞뒤 급전환", "flip3": "앞뒤 급전환", "stop3": "멈춤",
    "circle": "원 달리기 (몸 방향 고정)", "stop4": "멈춤", "turn": "제자리 회전", "idle2": "서 있기",
    # -Script aimjump
    "look_up": "위로 겨눔 (0 → 60도)", "hold_up": "위 60도 유지", "look_down": "아래로 겨눔 (60 → -60도)",
    "hold_down": "아래 60도 유지", "look_mid": "가운데로 (-60 → 0도)", "run_up": "앞으로 달리며 위로 (0 → 45도)",
    "run_down": "뒤로 달리며 아래로 (45 → -45도)", "stop_mid": "멈추며 가운데로", "jump": "제자리 점프",
    "run_pre": "앞으로 달리기", "run_jump": "달리며 점프", "stop_end": "멈춤",
    # -Script startstop
    "stop_fwd": "앞 달리기 멈춤", "stop_bwd": "뒤 달리기 멈춤", "stop_left": "왼쪽 달리기 멈춤", "stop_right": "오른쪽 달리기 멈춤",
    "diag": "대각선 45도로", "stop_diag": "대각선 멈춤",
    "piv_bf_a": "뒤로 달리기", "piv_bf_b": "뒤 → 앞 돌아서기", "stop_bf": "멈춤",
    "piv_fb_a": "앞으로 달리기", "piv_fb_b": "앞 → 뒤 돌아서기", "stop_fb": "멈춤",
    "piv_lr_a": "왼쪽으로 달리기", "piv_lr_b": "왼쪽 → 오른쪽 돌아서기", "stop_lr": "멈춤",
    "piv_rl_a": "오른쪽으로 달리기", "piv_rl_b": "오른쪽 → 왼쪽 돌아서기", "stop_rl": "멈춤",
    "ad1": "좌우 번갈아 (0.4초씩)", "ad2": "좌우 번갈아 (0.4초씩)", "ad3": "좌우 번갈아 (0.4초씩)",
    "ad4": "좌우 번갈아 (0.4초씩)", "ad5": "좌우 번갈아 (0.4초씩)", "ad6": "좌우 번갈아 (0.4초씩)", "stop_ad": "멈춤",
    # -Script events (9단계 사건 경로, MOBAnimCompareSubsystem EVENTS_SCRIPT · BotClient kEventsScript — idle 은 위와 같다)
    "fire_stand": "서서 한 발", "fire_hold": "당긴 채 연사 (0.5초)", "run_fire": "앞으로 달리며 두 발", "stop_f": "멈춤",
    "up_fire": "위로 겨누며 한 발 (0 → 45도)", "down_fire": "아래로 겨누며 한 발 (45 → -45도)", "mid": "가운데로 (-45 → 0도)",
    "reload": "재장전", "fire_b": "한 발", "reload_cancel": "재장전 중에 쏘기", "settle_c": "서 있기",
    "run_reload": "오른쪽으로 달리며 재장전", "stop_r": "멈춤", "swap_out": "무기 바꾸기", "swap_back": "원래 무기로",
    "swap_fire": "바꾸자마자 쏘기", "swap_back2": "원래 무기로", "run_swap": "왼쪽으로 달리며 바꾸고 돌아오기", "stop_s": "멈춤",
    "spawn": "부활 동작", "idle_end": "서 있기",
    # -Script crouch (컨텐츠 D-3 · D-6 앉기 경로, MOBAnimCompareSubsystem CROUCH_SCRIPT · BotClient kCrouchScript — idle · stop_end · idle_end 는 위와 같다).
    # 조준 경로(-Script ads)도 run_on · run_off · stop_a · start_toggle · stop_toggle · piv_toggle_a/b · ret_left 이름을 쓰지만 뜻이 조준이다 — 그 영상을 만들 땐 따로 나눠야 한다
    "c_on": "서서 앉기", "c_up": "앉아 위로 (0 → 45도)", "c_down": "앉아 아래로 (45 → -45도)", "c_mid": "앉아 가운데로", "c_off": "일어서기", "c_on2": "다시 앉기",
    "c_fwd": "앉아 앞으로", "c_stop_fwd": "앉은 걸음 멈춤", "c_bwd": "앉아 뒤로", "c_stop_bwd": "앉은 걸음 멈춤",
    "c_left": "앉아 왼쪽으로", "c_stop_left": "앉은 걸음 멈춤", "c_right": "앉아 오른쪽으로", "c_stop_right": "앉은 걸음 멈춤",
    "c_piv_bf_a": "앉아 뒤로", "c_piv_bf_b": "앉아 뒤 → 앞 돌아서기", "c_stop_bf": "멈춤",
    "c_piv_fb_a": "앉아 앞으로", "c_piv_fb_b": "앉아 앞 → 뒤 돌아서기", "c_stop_fb": "멈춤",
    "c_piv_lr_a": "앉아 왼쪽으로", "c_piv_lr_b": "앉아 왼쪽 → 오른쪽 돌아서기", "c_stop_lr": "멈춤",
    "c_piv_rl_a": "앉아 오른쪽으로", "c_piv_rl_b": "앉아 오른쪽 → 왼쪽 돌아서기", "c_stop_rl": "멈춤",
    "c_fire": "앉아 한 발", "c_ads_on": "앉아 조준", "c_ads_fwd": "앉아 조준하고 앞으로", "c_ads_stop": "멈춤", "c_ads_bwd": "앉아 조준하고 뒤로",
    "c_ads_off": "조준 풀기", "c_jump": "앉은 데서 점프 (일어서며 뜀)", "c_settle": "서 있기",
    "run_on": "앞으로 달리다 앉기", "run_off": "앉아 뒤로 가다 일어서기", "stop_a": "멈춤",
    "start_toggle": "출발하며 앉기", "stop_toggle": "멈추며 일어서기", "piv_toggle_a": "앉아 왼쪽으로", "piv_toggle_b": "돌아서며 앉기",
    "ret_left": "제자리로",
}
RECORD_FPS = 60.0
VIDEO_FPS = 30


def segments(csv_path):
    """Returns {pass: [(label, start, end)]} in seconds from the start of that pass."""
    passes = {}
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            passes.setdefault(row["pass"], []).append(row["segment"])
    result = {}
    for pass_no, names in passes.items():
        spans, start = [], 0
        for i in range(1, len(names) + 1):
            if i == len(names) or names[i] != names[start]:
                spans.append((SEGMENT_LABELS[names[start]], start / RECORD_FPS, i / RECORD_FPS))
                start = i
        result[pass_no] = spans
    return result


def caption(text, x, y, size, extra=""):
    return f"drawtext=fontfile='{FONT}':text='{text}':x={x}:y={y}:fontsize={size}:fontcolor=white:box=1:boxcolor=black@0.55:boxborderw=8{extra}"


def main():
    if len(sys.argv) != 6:
        print(__doc__)
        return 2
    ffmpeg, mob, tag_a, tag_b, out = sys.argv[1:]
    os.makedirs(out, exist_ok=True)
    spans = segments(os.path.join(mob, f"animcmp_{tag_a}.csv"))
    near, far = spans["0"], spans["1"]
    near_seconds = near[-1][2]
    # Each pass records every 2nd frame, counted across passes, so the far pass starts at this video time.
    cut = (round(near_seconds * RECORD_FPS) + 1) // 2 / VIDEO_FPS

    def labels(items):
        return ",".join(caption(t, "(w-text_w)/2", "h-60", 34, f":enable='between(t,{a:.3f},{b:.3f})'") for t, a, b in items)

    graph = (
        f"[0:v]scale=960:540,{caption(LEFT_LABEL, 20, 16, 30)}[a];"
        f"[1:v]scale=960:540,{caption(RIGHT_LABEL, 20, 16, 30)}[b];"
        f"[a][b]hstack,split=2[n][f];"
        f"[n]trim=0:{cut:.4f},setpts=PTS-STARTPTS,{labels(near)}[near];"
        f"[f]trim=start={cut:.4f},setpts=PTS-STARTPTS,{labels(far)}[far]"
    )
    script = os.path.join(out, "_filter.txt")
    with open(script, "w", encoding="utf-8") as f:
        f.write(graph)

    near_mp4, far_mp4, slow_mp4 = (os.path.join(out, n) for n in ("near.mp4", "far.mp4", "near_half_speed.mp4"))
    encode = ["-c:v", "libx264", "-crf", "20", "-preset", "veryfast", "-pix_fmt", "yuv420p"]
    subprocess.check_call([ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                           "-i", os.path.join(mob, f"animcmp_{tag_a}.mp4"), "-i", os.path.join(mob, f"animcmp_{tag_b}.mp4"),
                           "-filter_complex_script", script,
                           "-map", "[near]", *encode, near_mp4, "-map", "[far]", *encode, far_mp4])
    subprocess.check_call([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", near_mp4,
                           "-vf", "setpts=2*PTS", *encode, slow_mp4])
    os.remove(script)
    for path in (near_mp4, far_mp4, slow_mp4):
        print(path, os.path.getsize(path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
