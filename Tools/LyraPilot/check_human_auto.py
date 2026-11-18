# Human-check automation (MyDocs/claude-plans/mo-shooter/human-check-auto-plan.md): judges the runs of record_human_auto.ps1.
#   python check_human_auto.py --tag=<tag> [--only=both|move|pads]
#   python check_human_auto.py --probe=<cprobe_<tag>.csv>     (prints the Lyra answer key for the crouch camera and feet - see probe_key)
# Reads Saved\MOB\hauto_<tag>.log + hauto_<tag>_server.txt (move run) and padsb_<tag>p.log + padsb_<tag>p_server.txt (pads run).
# One line per check: PASS/FAIL, the observed log name, the expectation, the value now. Counts go next to each verdict,
# because nothing compared would also be "equal". Writes Saved\MOB\hauto_<tag>_report.html (the same lines + every photo).
import html
import math
import pathlib
import re
import statistics
import subprocess
import sys

OUT = pathlib.Path(r"C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra\Saved\MOB")
HERE = pathlib.Path(__file__).resolve().parent

# Answer keys. Where each number comes from is in human-check-auto-progress.md (chunk 1 table and the pilot audit).
KEY = {
    "cam_diff": -47.36,      # crouched camera - standing camera (cm). Lyra alone, probe run cprobe_hck1 (gym, Quinn, --probe= below); the formula says -48
    "cam_tol": 2.0,          # my judgment: the third-person camera arm may bend a little against geometry (pilot ha1 was 0.16 off)
    "foot": (-0.94, -1.11),  # lowest foot bone above the capsule bottom, standing / crouched (cm) - same probe run
    "foot_tol": 3.0,         # my judgment: idle sway moves the feet by 1-2 cm (pilot ha1 was 0.90 / 0.04 off)
    "melee_gap": {"pistol": 34, "rifle": 35, "shotgun": 46},   # Lyra run, frames between melee activations while held (content-add-progress.md:92)
    "ads_ratio": 0.8,        # Shared/Weapon.cpp aimingMultiplier (shotgun), matched to Lyra in WeaponCheck
    "ads_ratio_tol": 0.02,
    "mult_diff": 0.001,      # bridge multiplier vs Lyra ability multiplier per pellet; C-1 record max 0.0002
    "corr_max": 5.0,         # record practice: just above f3push1 4.48 cm (not a feel threshold - feel stays a human check).
                             # D89: above this only while another body is near - pushing a moving player gives more (hc1-3: 15.85 / 8.55 / 8.60)
    "corr_near": 100.0,      # cm, D89 - my judgment: the widest seen for a correction over 5 cm was 67.7 cm (hc3, 30 of 30 touching)
    "crouch_drift": (20.0, 30.0),   # MOB_SELF_DRIFT vertical move of crouch size: Lyra crouching on its own lowers the capsule 90 - 65 = 25 cm
                                    # (hook off d5c3brk: 25.000 x 8, content-add-progress.md D-5-c table). Not selfDriftMax - other vertical snaps
                                    # of a few cm while standing are mixed into it (pilot ha1: 7.06 standing, nobody near)
    "drift_lines": 20,       # MOB_SELF_DRIFT prints the first 20 drifts over 1 cm (DriftLinesLeft) - more than that and the lines no longer cover all
    "force_diff": 1.0,       # my judgment: same kind and target -> same push formula, only float noise
    "cjump_frames": 2,       # my judgment: uncrouch and lift come from the same input (D-5); 2 frames for frame vs tick
}

results = []   # (ok, item, observed, expect, now)


def check(ok, item, observed, expect, now):
    results.append((bool(ok), item, observed, expect, str(now)))


def fields(line):
    return dict(re.findall(r"(\w+)=(\S+)", line))


def lines_of(text, name):
    return [l[l.index(name):] for l in text if name + " " in l]


def last(text, name):
    found = lines_of(text, name)
    return fields(found[-1]) if found else None


def rng(s):
    # "frames,avg,min,max"
    n, avg, lo, hi = s.split(",")
    return int(n), float(avg), float(lo), float(hi)


def read(path):
    p = pathlib.Path(path)
    return p.read_text(encoding="utf-8", errors="replace").splitlines() if p.exists() else None


def shots_of(text):
    out = []
    for l in lines_of(text, "MOB_SHOT"):
        f = fields(l)
        out.append((float(f.get("t", 0)), pathlib.Path(f.get("path", "")).name))
    return out


def judge_move(tag):
    log = read(OUT / f"hauto_{tag}.log")
    srv = read(OUT / f"hauto_{tag}_server.txt")
    if log is None:
        check(False, "move run", "hauto log", "exists", "missing")
        return []
    s = last(log, "MOB_HC_SUMMARY")
    done = bool(lines_of(log, "MOB_HC_SCRIPT_DONE"))
    check(s and done and s["steps"].startswith("16/"), "script", "MOB_HC_SUMMARY steps / MOB_HC_SCRIPT_DONE", "16/16 and done",   # 13 + approach before each melee step
          f"{s['steps'] if s else 'no summary'} done={done} restarts={s['restarts'] if s else '?'}")

    # step windows by input number: last begin -> end pair of each step
    begin, win = {}, {}
    for l in lines_of(log, "MOB_HC_STEP"):
        f = fields(l)
        kind = "begin" if "MOB_HC_STEP begin" in l else "end"
        if kind == "begin":
            begin[f["step"]] = int(f["seq"])
        elif f["step"] in begin:
            win[f["step"]] = (begin[f["step"]], int(f["seq"]))

    # C - shotgun aimed vs hip
    shot = {}
    for l in lines_of(log, "MOB_SHOTDIR"):
        f = fields(l)
        a = [float(x) for x in f["aim"].split(",")]
        d = [float(x) for x in f["dir"].split(",")]
        ang = math.degrees(math.acos(max(-1.0, min(1.0, sum(x * y for x, y in zip(a, d))))))
        shot.setdefault(int(f["seq"]), []).append((float(f["mult"]), float(f["lyraMult"]), ang))
    side = {}
    for name in ("ads_fire", "hip_fire"):
        lo, hi = win.get(name, (0, -1))
        side[name] = {q: v for q, v in shot.items() if lo <= q <= hi}
    ads, hip = side["ads_fire"], side["hip_fire"]
    pellets = sorted({len(v) for v in list(ads.values()) + list(hip.values())})
    check(len(ads) >= 6 and len(hip) >= 6 and pellets == [9], "C shots", "MOB_SHOTDIR in ads_fire / hip_fire windows",
          ">= 6 shots each side, 9 pellets per shot", f"aimed {len(ads)} hip {len(hip)} pellets/shot {pellets}")
    diffs = [abs(m - lm) for v in list(ads.values()) + list(hip.values()) for m, lm, _ in v]
    check(diffs and max(diffs) <= KEY["mult_diff"], "C bridge = Lyra", "MOB_SHOTDIR mult vs lyraMult", f"max diff <= {KEY['mult_diff']}",
          f"{max(diffs):.6f} over {len(diffs)} pellets" if diffs else "no pellets")
    if ads and hip:
        ma = statistics.median(v[0][0] for v in ads.values())
        mh = statistics.median(v[0][0] for v in hip.values())
        ratio = ma / mh if mh else 0
        check(abs(ratio - KEY["ads_ratio"]) <= KEY["ads_ratio_tol"], "C aimed spread", "MOB_SHOTDIR mult median aimed / hip",
              f"{KEY['ads_ratio']} +- {KEY['ads_ratio_tol']}", f"{ratio:.4f} (aimed {ma:.4f} hip {mh:.4f})")
        aa = statistics.mean(a for v in ads.values() for _, _, a in v)
        ah = statistics.mean(a for v in hip.values() for _, _, a in v)
        check(aa < ah, "C pellets drawn", "MOB_SHOTDIR mean pellet angle aimed / hip (deg)", "aimed narrower",
              f"{aa:.3f} / {ah:.3f} = {aa / ah:.3f}")
    else:
        check(False, "C aimed spread", "MOB_SHOTDIR", "both sides", "a side is empty")

    # D - crouch
    c = last(log, "MOB_CROUCH")
    s3 = last(log, "MOB_SUMMARY3")
    check(c and int(c["selfChanges"]) > 0 and int(c["flagDiff"]) == 0, "D crouch state", "MOB_CROUCH selfChanges / flagDiff", "> 0 / 0",
          f"{c['selfChanges']} / {c['flagDiff']}" if c else "no line")
    lo, hi = KEY["crouch_drift"]
    drifts = [abs(float(fields(l)["moved"].strip("()").split(",")[2])) for l in lines_of(log, "MOB_SELF_DRIFT")]
    sized = [d for d in drifts if lo <= d <= hi]
    over = int(s3["selfDriftOver1cm"]) if s3 else -1
    check(s3 and not sized and over <= KEY["drift_lines"], "D Lyra crouch blocked", "MOB_SELF_DRIFT vertical moves of crouch size",
          f"0 in [{lo:.0f},{hi:.0f}] cm, all drifts printed (selfDriftOver1cm <= {KEY['drift_lines']})",
          f"{len(sized)} of {len(drifts)} lines (selfDriftOver1cm {over}, selfDriftMax {s3['selfDriftMax'] if s3 else '?'})")
    check(c and int(c["remoteLiftFrames"]) == 0, "D others' body", "MOB_CROUCH remoteLiftFrames", "0", c["remoteLiftFrames"] if c else "no line")
    if s:
        cs, cc = rng(s["camStand"]), rng(s["camCrouch"])
        diff = float(s["camDiff"])
        check(cs[0] > 0 and cc[0] > 0 and abs(diff - KEY["cam_diff"]) <= KEY["cam_tol"], "D camera", "MOB_HC_SUMMARY camDiff (crouched - standing)",
              f"{KEY['cam_diff']} +- {KEY['cam_tol']}", f"{diff:.2f} (frames {cs[0]} / {cc[0]}, stand {cs[1]:.1f} [{cs[2]:.1f},{cs[3]:.1f}] crouch {cc[1]:.1f} [{cc[2]:.1f},{cc[3]:.1f}])")
        fs, fc = rng(s["footStand"]), rng(s["footCrouch"])
        ks, kc = KEY["foot"]
        check(fs[0] > 0 and fc[0] > 0 and int(s["footMissing"]) == 0 and abs(fs[1] - ks) <= KEY["foot_tol"] and abs(fc[1] - kc) <= KEY["foot_tol"],
              "D own feet", "MOB_HC_SUMMARY footStand / footCrouch avg", f"Lyra {ks} / {kc} +- {KEY['foot_tol']} cm, footMissing 0", f"{fs[1]:.2f} / {fc[1]:.2f} (ranges [{fs[2]:.1f},{fs[3]:.1f}] [{fc[2]:.1f},{fc[3]:.1f}]) missing {s['footMissing']}")
    else:
        check(False, "D camera / own feet", "MOB_HC_SUMMARY", "line exists", "no line")
    cj = [fields(l) for l in lines_of(log, "MOB_HC_CJUMP")]
    good = [f for f in cj if f["crouchedAtPress"] == "1" and int(f["uncrouchFrames"]) >= 0 and int(f["liftFrames"]) >= 0
            and abs(int(f["liftFrames"]) - int(f["uncrouchFrames"])) <= KEY["cjump_frames"]]
    check(cj and len(good) == len([f for f in cj if f["crouchedAtPress"] == "1"]) and good, "D crouch jump", "MOB_HC_CJUMP uncrouchFrames / liftFrames",
          f"crouched at press, both seen, within {KEY['cjump_frames']} frames", "; ".join(f"{f['crouchedAtPress']}:{f['uncrouchFrames']}/{f['liftFrames']}" for f in cj) or "no line")

    # E - melee
    melee_lines = lines_of(log, "MOB_HC_MELEE")
    check(len(melee_lines) == 3, "E lines", "MOB_HC_MELEE", "3 weapons", len(melee_lines))
    for l in melee_lines:
        f = fields(l)
        key = KEY["melee_gap"][f["weapon"]]
        gaps = [int(g) for g in re.search(r"gaps=\[([^\]]*)\]", l).group(1).split()]   # the list holds spaces
        held = [g for g in gaps if g < 2 * key]   # longer = released or died between two starts
        mode = statistics.mode(held) if held else -1
        check(int(f["starts"]) >= 3 and abs(mode - key) <= 1 and int(f["driven"]) == int(f["starts"]), f"E repeat {f['weapon']}",
              "MOB_HC_MELEE gaps mode / driven", f"{key} +- 1 / driven = starts", f"mode {mode} gaps {gaps} starts {f['starts']} driven {f['driven']}")
    mine = {fields(l)["name"]: int(fields(l)["count"]) for l in lines_of(log, "MOB_HC_MYMONTAGE")}
    melee_named = sum(n for k, n in mine.items() if "melee" in k.lower())
    check(melee_named > 0, "E my montage", "MOB_HC_MYMONTAGE (2 frames after my melee)", "a melee montage plays", mine or "no line")
    m = last(log, "MOB_MELEE")
    if m:
        same = int(m["kindDiff"]) == 0 and int(m["targetDiff"]) == 0
        hits = int(m["mine"].split("/")[3])   # my melee results none / same team / blocked / hit - a push is only judged on a hit (pilots ha1 / ha2 had 0 and still passed)
        check(hits > 0 and (not same or float(m["forceDiffMax"]) <= KEY["force_diff"]), "E push", "MOB_MELEE mine hit / forceDiffMax (kindDiff / targetDiff)",
              f"hit > 0, <= {KEY['force_diff']} when kind and target agree", f"hit {hits} / {m['forceDiffMax']} ({m['kindDiff']} / {m['targetDiff']}) judged {m['judged']}")
    else:
        check(False, "E push", "MOB_MELEE", "line exists", "no line")
    m3 = last(log, "MOB_MELEE3")
    check(m3 and int(m3["lyraSelf"]) == 0, "E Lyra blocked", "MOB_MELEE3 lyraSelf", "0", m3["lyraSelf"] if m3 else "no line")
    remote = {fields(l)["name"]: int(fields(l)["starts"]) for l in lines_of(log, "MOB_HC_REMOTEMONTAGE")}
    react = sum(n for k, n in remote.items() if "hit" in k.lower())
    mr = last(log, "MOB_HC_MELEEREACT")
    # the enemy my melee hit starts a hit reaction within 1 s - any hit reaction on anyone (gunfire) does not show this (pilots ha1 / ha2)
    check(mr and int(mr["hits"]) > 0 and int(mr["reacts"]) > 0, "E hit reaction", "MOB_HC_MELEEREACT hits / reacts (reaction of the enemy my melee hit)",
          "> 0 / > 0", f"{mr['hits']} / {mr['reacts']} (any Hit montage {react})" if mr else "no line")

    # F - bumping, own body
    p = last(log, "MOB_PAWN")
    check(p and int(p["predHits"]) > 0, "F bump", "MOB_PAWN predHits", "> 0", f"{p['predHits']} (startPen {p['predStartPen']})" if p else "no line")
    # D89: a correction over corr_max is fine only with another body within corr_near (the client cannot know where a pushing player goes next).
    # The bridge prints every correction of 3 cm or more (MOB_CORR), so when correctionMax >= 3 the largest line must equal it -
    # otherwise the lines do not cover all (old builds printed only the first 80: hc1 15.85 was not in them)
    corr = [fields(l) for l in lines_of(log, "MOB_CORR")]
    big = [c for c in corr if float(c["dist"]) >= KEY["corr_max"]]
    far = [c for c in big if not (0.0 <= float(c.get("nearestRemote", "-1")) <= KEY["corr_near"])]   # -1 = nobody, missing = old build
    line_max = max((float(c["dist"]) for c in corr), default=0.0)
    covered = bool(s3) and (float(s3["correctionMax"]) < 3.0 or abs(line_max - float(s3["correctionMax"])) < 0.006)
    check(s3 and int(s3["visualSnaps"]) == 0 and covered and not far, "F no screen jump",
          f"MOB_SUMMARY3 visualSnaps / MOB_CORR over {KEY['corr_max']} cm with nobody within {KEY['corr_near']:.0f} cm (D89)",
          "0 / 0 far, lines cover correctionMax",
          f"{s3['visualSnaps']} / {len(far)} far of {len(big)} over (correctionMax {s3['correctionMax']}, largest line {line_max:.2f}, corrections {s3['corrections']})"
          if s3 else "no line")
    b = last(log, "MOB_SELFBODY_SUMMARY")
    check(b and int(b["donePawns"]) == int(b["selfRespawns"]) + 1 and int(b["fails"]) == 0, "F own body", "MOB_SELFBODY_SUMMARY donePawns / selfRespawns / fails",
          "respawns + 1 / fails 0", f"{b['donePawns']} / {b['selfRespawns']} / {b['fails']}" if b else "no line")

    # A - scoreboard
    bd = last(log, "MOB_HC_BOARD")
    check(bd and bd["tag"] == "1" and bd["asc"] == "1", "A board opened", "MOB_HC_BOARD tag / asc / afterResult", "1 / 1",
          f"{bd['tag']} / {bd['asc']} / {bd['afterResult']}" if bd else "no line")
    server = {}
    for l in srv or []:
        if l.startswith("SCORE "):
            f = fields(l)
            server[f["id"]] = int(f["assists"])
    rows = [fields(l) for l in lines_of(log, "MOB_HC_BOARDROW")]
    pairs = [(r["id"], int(r["assists"]), server.get(r["id"])) for r in rows if "assists" in r]
    wrong = [x for x in pairs if x[2] is None or x[1] != x[2]]
    # nonzero > 0: an all-zero column would also match an all-zero server
    check(pairs and not wrong and any(x[1] > 0 for x in pairs), "A assists", "MOB_HC_BOARDROW assists vs server SCORE assists", "same for every row, some nonzero",
          f"rows {len(pairs)} (server {len(server)}) wrong {wrong} nonzero {sum(1 for x in pairs if x[1] > 0)}")
    return shots_of(log)


def judge_pads(tag):
    base = f"padsb_{tag}p"
    log = read(OUT / f"{base}.log")
    if log is None:
        check(False, "pads run", "padsb log", "exists", "missing")
        return []
    r = subprocess.run([sys.executable, str(HERE / "check_pads_bridge.py"), f"--server={OUT / (base + '_server.txt')}", f"--lyra={OUT / (base + '.log')}"],
                       capture_output=True, text=True)
    final = [l for l in r.stdout.splitlines() if l.startswith("PADS_BRIDGE")]
    check(final and "PASS" in final[-1], "B messages / heals / drawing", "check_pads_bridge.py", "PADS_BRIDGE PASS", final[-1] if final else r.stdout[-300:])
    pct = {}
    for f in (fields(l) for l in lines_of(log, "MOB_HC_PADPCT")):
        pct.setdefault(f["pad"], []).append((float(f["t"]), float(f["pct"]), int(f["shown"]), float(f["serverLeft"]), int(f["cool"])))
    # the bridge writes CoolDownPercentage and logs it in the same block of the same frame, so any difference means Lyra changed the value itself
    worst, falls, shown, cycles = 0.0, 0, 0, 0
    for rows in pct.values():
        for i, (t, v, sh, left, cool) in enumerate(rows):
            worst = max(worst, abs(v - max(0.0, min(1.0, 1 - left * 60 / cool))))   # wire::kTickRate 60
            shown += sh
            if i == 0 or left > rows[i - 1][3] + 0.5:   # server time left went up = a new cooldown (taken again)
                cycles += 1
            elif v + 1e-4 < rows[i - 1][1]:
                falls += 1
    check(pct and falls == 0 and shown == 0 and worst <= 0.001, "B cooldown display", "MOB_HC_PADPCT pct (Lyra CoolDownPercentage) vs 1 - serverLeft/cool",
          "rises within a cooldown, pad hidden, diff <= 0.001", f"pads {len(pct)} cycles {cycles} lines {sum(len(v) for v in pct.values())} falls {falls} shown {shown} worst {worst:.4f}")
    s = last(log, "MOB_HC_SUMMARY")
    check(s and int(s["padPickupFx"]) > 0 and int(s["padRespawnFx"]) > 0, "B effects", "MOB_HC_SUMMARY padPickupFx / padRespawnFx", "> 0 / > 0",
          f"{s['padPickupFx']} / {s['padRespawnFx']}" if s else "no line")
    return shots_of(log)


def probe_key(path):
    # Lyra answer key for the crouch camera and feet: record_content_probe.ps1 gym run (Lyra alone) with the camZ / footZ columns.
    # Same sampling as the bridge (SampleHumanCheck): on the ground, still, not aiming, 0.5 s after the last crouch change; heights above the capsule bottom.
    import csv
    rows = list(csv.DictReader(open(path, encoding="utf-8")))
    if not rows or "camZ" not in rows[0]:
        sys.exit(f"{path}: no camZ column (record it with the bridge build that has camZ / footZ)")
    cam, foot, changed, last = {0: [], 1: []}, {0: [], 1: []}, -1e9, None
    for r in rows:
        t, k = float(r["t"]), int(r["crouched"])
        if k != last:
            changed, last = t, k
        if r["mode"] != "1" or float(r["speed2d"]) > 1.0 or r["adsActive"] == "1" or t - changed < 0.5:
            continue
        floor = float(r["pz"]) - float(r["capHH"])
        cam[k].append(float(r["camZ"]) - floor)
        if float(r["footZ"]) > -1e5:
            foot[k].append(float(r["footZ"]) - floor)
    for name, d in (("cam", cam), ("foot", foot)):
        for k in (0, 1):
            v = d[k]
            print(f"{name} {'crouch' if k else 'stand'}: frames {len(v)}" + (f" avg {statistics.mean(v):.2f} min {min(v):.2f} max {max(v):.2f}" if v else ""))
    if cam[0] and cam[1]:
        print(f"PROBE_KEY camDiff={statistics.mean(cam[1]) - statistics.mean(cam[0]):.2f}"
              + (f" footStand={statistics.mean(foot[0]):.2f} footCrouch={statistics.mean(foot[1]):.2f}" if foot[0] and foot[1] else ""))


def main():
    probe = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--probe=")), None)
    if probe:
        probe_key(probe)
        return
    tag = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--tag=")), None) or sys.exit("missing --tag=")
    only = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--only=")), "both")
    shots = []
    if only != "pads":
        shots += judge_move(tag)
    if only != "move":
        shots += judge_pads(tag)
    for ok, item, observed, expect, now in results:
        print(f"{'PASS' if ok else 'FAIL'} {item}: {observed} | expect {expect} | now {now}")
    missing = [n for _, n in shots if not (OUT / n).exists()]
    print(f"photos {len(shots)} missing {len(missing)} {missing[:5]}")
    fails = sum(1 for r in results if not r[0])
    print(f"HUMAN_AUTO {'PASS' if fails == 0 else 'FAIL'} fails={fails} checks={len(results)}")

    rows = "".join(f"<tr class='{'ok' if ok else 'ng'}'><td>{'PASS' if ok else 'FAIL'}</td><td>{html.escape(i)}</td><td><code>{html.escape(o)}</code></td>"
                   f"<td>{html.escape(e)}</td><td>{html.escape(n)}</td></tr>" for ok, i, o, e, n in results)
    cards = "".join(f"<figure><a href='{html.escape(n)}'><img src='{html.escape(n)}' loading='lazy'></a><figcaption>{html.escape(n)} / t={t:.2f}"
                    f"{' / 파일 없음' if n in missing else ''}</figcaption></figure>" for t, n in shots)
    page = f"""<!doctype html><meta charset="utf-8"><title>사람 확인 자동화 {html.escape(tag)}</title>
<style>body{{font:14px sans-serif;margin:24px}}table{{border-collapse:collapse}}td,th{{border:1px solid #ccc;padding:4px 8px;vertical-align:top}}
tr.ok td:first-child{{color:#0a0;font-weight:bold}}tr.ng td:first-child{{color:#c00;font-weight:bold}}figure{{display:inline-block;margin:6px;width:420px}}
img{{width:420px;border:1px solid #999}}figcaption{{font-size:12px}}</style>
<h1>사람 확인 자동화 — 판 {html.escape(tag)} / {'PASS' if fails == 0 else f'FAIL {fails}건'}</h1>
<p>판정 줄 {len(results)}개 / 사진 {len(shots)}장(없음 {len(missing)}). 플랜 = MyDocs/claude-plans/mo-shooter/human-check-auto-plan.md</p>
<table><tr><th>판정</th><th>항목</th><th>관측 이름</th><th>기대</th><th>지금 값</th></tr>{rows}</table><h2>사진</h2>{cards}"""
    report = OUT / f"hauto_{tag}_report.html"
    report.write_text(page, encoding="utf-8")
    print(f"report {report}")
    sys.exit(0 if fails == 0 else 1)


if __name__ == "__main__":
    main()
