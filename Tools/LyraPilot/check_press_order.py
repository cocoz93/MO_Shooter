# Same-input press order (lyra-match-plan L-5-c): judges a bridge run that sent pressOrder.
#   python check_press_order.py --tag=<tag> --mode=auto|keys [--break=0|1|2]
# Reads Saved\MOB\game_<tag>_lyra.log + game_<tag>_server.txt (run_game.ps1).
#   auto  = -MOBAutoInput run: the auto script calls Lyra's press tags in code order (melee, dash, grenade, reload),
#           so every sent order must keep that order and no pressed button may be missing from Lyra's calls.
#   keys  = -MOBPressKeyTest run: key events go through Enhanced Input like a person's keys. Lyra calls same-frame
#           presses in its binding order (EnhancedPlayerInput.cpp:739), so both key orders of a pair must give the
#           binding order (MOB_PRESS bind=), and every cell must show up exactly once. The held-key cell (C held, Shift
#           3 frames later) depends on the frames the input covers: it holds Lyra's calls of the frames after the previous
#           input (frame - span + 1 .. frame). Lyra calls the Shift key one frame after it was sent (MOB_PRESSKEY_SECOND).
#           The held melee is called every frame, so if the covered frames start before that frame, the melee call comes
#           first; if they start at that frame, both are called in that frame = binding order (runs l5c_keys4 / keys5).
#   --break=1 expects the server to get 0 (unordered > 0), --break=2 expects the reverse of the above.
# One line per check: PASS/FAIL, the log name, the expectation, the value now. Counts go next to each verdict,
# because nothing compared would also be "equal".
import pathlib
import re
import sys

OUT = pathlib.Path(r"C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra\Saved\MOB")
CODE_ORDER = "MDGR"   # wire::EPressKey order = the auto script's call order (TickAutoInput)

args = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
tag, mode, brk = args.get("tag", ""), args.get("mode", "auto"), int(args.get("break", "0"))
lyra = (OUT / f"game_{tag}_lyra.log").read_text(encoding="utf-8", errors="replace")
server = (OUT / f"game_{tag}_server.txt").read_text(encoding="utf-8", errors="replace")
fails = 0


def check(ok, name, want, now):
    global fails
    fails += 0 if ok else 1
    print(f"{'PASS' if ok else 'FAIL'} {name} | want {want} | now {now}")


def fields(text, head):
    m = re.search(rf"{head} (.*)", text)
    return dict(re.findall(r"(\w+)=(\S+)", m.group(1))) if m else {}


press = fields(lyra, "MOB_PRESS")
sp = fields(server, "SERVER pressOrder")
si = fields(server, "SERVER inputs")
orders = re.findall(r"MOB_PRESS_ORDER seq=\d+ frame=(\d+) span=(\d+) order=(\S+) unlisted=(\d+)", lyra)
if not press or not sp or not si:
    print(f"FAIL logs | want MOB_PRESS + SERVER pressOrder + SERVER inputs | now {bool(press)} {bool(sp)} {bool(si)}")
    sys.exit(1)

multi, sent = int(press["multi"]), int(press["sent"])
smulti = int(sp["multi"])
left = int(si["received"]) - int(si["applied"]) - int(si["whileDead"])
check(int(press["break"]) == brk, "MOB_PRESS break", brk, press["break"])
check(multi > 0, "MOB_PRESS multi", "> 0", multi)
check(int(press["tagsDropped"]) == 0, "MOB_PRESS tagsDropped", 0, press["tagsDropped"])
check(0 <= multi - smulti <= max(left, 0), "bridge multi - SERVER multi", f"0..{left} (received - applied - whileDead)", multi - smulti)
if brk == 1:
    check(sent == 0, "MOB_PRESS sent", 0, sent)
    check(int(sp["unordered"]) == smulti and smulti > 0, "SERVER unordered", f"= multi {smulti}", sp["unordered"])
else:
    check(sent == multi, "MOB_PRESS sent", f"= multi {multi}", sent)
    check(int(sp["ordered"]) == smulti and int(sp["unordered"]) == 0, "SERVER ordered / unordered", f"{smulti} / 0", f"{sp['ordered']} / {sp['unordered']}")


def expect(keys, ref):
    """The order Lyra's calls give for these letters: ref order, reversed under --break=2."""
    want = "".join(k for k in ref if k in keys)
    return want[::-1] if brk == 2 else want


hist = {}
for _, _, order, _ in orders:
    hist[order] = hist.get(order, 0) + 1
print("orders " + " ".join(f"{k}:{v}" for k, v in sorted(hist.items())) + f" (first {len(orders)} lines, multi {multi})")

if mode == "auto":
    bad = [o for _, _, o, _ in orders if o.replace(">", "") != expect(o.replace(">", ""), CODE_ORDER)]
    check(len(orders) > 0 and not bad, "MOB_PRESS_ORDER order", f"code order {CODE_ORDER} on all {len(orders)}", f"{len(bad)} off {bad[:3]}")
    check(int(press["unlisted"]) == 0, "MOB_PRESS unlisted", 0, press["unlisted"])
else:
    bind = press["bind"]
    check(len(bind) == 4 and set(bind) == set(CODE_ORDER), "MOB_PRESS bind", "the 4 actions", bind)
    cells = re.findall(r"MOB_PRESSKEY cell=(\d+) name=(\S+) frame=(\d+) delay=(\d+)", lyra)
    check(len(cells) == 7 and "MOB_PRESSKEY_DONE cells=7" in lyra and "MOB_PRESSKEY_CUT" not in lyra,
          "MOB_PRESSKEY cells", "7, DONE, no CUT", f"{len(cells)} cut={lyra.count('MOB_PRESSKEY_CUT')}")
    letter = {"melee": "M", "dash": "D", "q": "G", "reload": "R"}
    seen = 0
    for i, (cell, name, frame, delay) in enumerate(cells):
        end = int(cells[i + 1][2]) if i + 1 < len(cells) else 1 << 62
        hits = [(o, u, int(sp)) for f, sp, o, u in orders if int(frame) <= int(f) < end]
        keys = "".join(letter[p] for p in name.replace("_hold", "").split("_"))
        note, held = "", False
        if int(delay) > 0:
            second = re.search(rf"MOB_PRESSKEY_SECOND cell={cell} frame=(\d+)", lyra)
            hit = [(int(f), int(sp)) for f, sp, o, u in orders if int(frame) <= int(f) < end]
            if second and len(hit) == 1:
                start, called = hit[0][0] - hit[0][1] + 1, int(second.group(1)) + 1
                held = start < called
                note = f" (covered frames from {start}, Shift called at {called}: {'held key first' if held else 'binding order'})"
            else:
                note = " (no MOB_PRESSKEY_SECOND or not one input)"
                hits = hits + [("missing", "", 0)]
        want = expect(keys, (keys[0] + bind.replace(keys[0], "")) if held else bind)
        ok = len(hits) == 1 and hits[0][0].replace(">", "") == want and hits[0][1] == "0"
        seen += len([h for h in hits if h[0] != "missing"])
        check(ok, f"cell {cell} {name}", f"one input, order {'>'.join(want)}, unlisted 0{note}", hits)
    check(seen == len(orders), "MOB_PRESS_ORDER lines outside the cells", 0, len(orders) - seen)

print(f"{'ALL PASS' if fails == 0 else f'FAIL {fails}'} tag={tag} mode={mode} break={brk}")
sys.exit(1 if fails else 0)
