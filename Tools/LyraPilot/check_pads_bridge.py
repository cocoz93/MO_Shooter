# Health pads on screen (content B-3): what the bridge got and drew against what the server sent.
#   python check_pads_bridge.py --server=<padsb_<tag>_server.txt> --lyra=<padsb_<tag>.log>      (record_pads_bridge.ps1)
# 1. Messages: every server PADEV take / takeRecheck / ready, and every sync sent to all or to the bridge's own player, from the bridge's first one on,
#    up to the last state tick the bridge got (MOB_PADS lastStateTick), must arrive in the same order with the same tick, pad, kind, availability,
#    ready tick, picker, heal and health after (bridge MOB_PAD lines).
# 2. Heals: no MOB_PAD_HEAL_DIFF (the pad's own effect left a health other than the server's), every take healed through the pad (heals = takes),
#    no take without a body. MOB_PAD_HEAL_BEFORE lines (Lyra's health before the take was not the server's) are listed, not failed:
#    a debug place changes health without a damage message, so a take in that same tick starts from the old health.
# 3. Drawing: the bridge's once-a-second compare of Lyra's pad state with the server's ran (stateChecks > 0) and never differed, every pad was bound to a map actor.
# Counts are printed next to each verdict, because nothing compared would also be "equal".
import re
import sys
import pathlib


def arg(name):
    for a in sys.argv[1:]:
        if a.startswith("--" + name + "="):
            return a.split("=", 1)[1]
    sys.exit(f"missing --{name}=")


def num(text, key):
    m = re.search(r"\b" + key + r"=(-?[\d.]+)", text)
    return m.group(1) if m else None


def main():
    srv = pathlib.Path(arg("server")).read_text(encoding="utf-8", errors="replace").splitlines()
    lyra = pathlib.Path(arg("lyra")).read_text(encoding="utf-8", errors="replace").splitlines()
    me = None
    got, heal_diff, heal_before, summary = [], [], [], None
    for line in lyra:
        if me is None and "MOB_WELCOME player=" in line:
            me = int(num(line, "player"))
        i = line.find("MOB_PAD tick=")
        if i >= 0:
            t = line[i:]
            got.append((int(num(t, "tick")), int(num(t, "pad")), re.search(r"reason=(\S+)", t).group(1), int(num(t, "avail")), int(num(t, "ready")),
                        int(num(t, "picker")), round(float(num(t, "heal")), 1), round(float(num(t, "hpAfter")), 1)))
        if "MOB_PAD_HEAL_DIFF" in line:
            heal_diff.append(line[line.find("MOB_PAD_HEAL_DIFF"):])
        if "MOB_PAD_HEAL_BEFORE" in line:
            heal_before.append(line[line.find("MOB_PAD_HEAL_BEFORE"):])
        if "MOB_PADS pads=" in line:
            summary = line[line.find("MOB_PADS"):]
    if me is None:
        sys.exit("FAIL no MOB_WELCOME in the bridge log")

    want = []
    for line in srv:
        m = re.match(r"PADEV tick=(\d+) room=\d+ pad=(\d+) kind=(\S+)(.*)", line)
        if not m:
            continue
        tick, pad, kind, rest = int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)
        if kind in ("take", "takeRecheck"):
            hp = re.search(r"hp=(\S+)>(\S+)", rest)
            before, after = float(hp.group(1)), float(hp.group(2))
            want.append((tick, pad, kind, 0, int(num(rest, "ready")), int(num(rest, "player")), round(after - before, 1), round(after, 1)))
        elif kind == "ready":
            want.append((tick, pad, kind, 1, 0, 0, 0.0, 0.0))
        elif kind == "sync":
            to = int(num(rest, "to"))
            if to in (0, me):
                want.append((tick, pad, kind, int(num(rest, "available")), int(num(rest, "ready")), 0, 0.0, 0.0))
    first = next((i for i, e in enumerate(want) if e[2] == "sync"), None)
    want = want[first:] if first is not None else []
    # The bridge leaves before the server stops: pad timers go on after the last step. A pad message goes out before the state of its tick,
    # so everything up to the last state tick the bridge got must have arrived; later events are dropped here and counted.
    last = num(summary or "", "lastStateTick")
    after = [e for e in want if last is not None and e[0] > int(last)]
    want = [e for e in want if last is None or e[0] <= int(last)]
    print(f"     note: bridge's last state tick {last}; server events after it (bridge gone) {len(after)}")

    fails = 0
    same = sum(1 for a, b in zip(got, want) if a == b)
    ok = got == want and len(want) > 0
    fails += not ok
    kinds = {k: sum(1 for e in want if e[2] == k) for k in ("sync", "take", "takeRecheck", "ready")}
    print(f"{'PASS' if ok else 'FAIL'} messages: bridge {len(got)} = server {len(want)} (same in order {same}; server sync {kinds['sync']} take {kinds['take']} takeRecheck {kinds['takeRecheck']} ready {kinds['ready']})")
    if not ok:
        for i in range(max(len(got), len(want))):
            a = got[i] if i < len(got) else None
            b = want[i] if i < len(want) else None
            if a != b:
                print(f"    first difference at {i}: bridge={a} server={b}")
                break

    s = summary or ""
    def s_int(key):
        v = num(s, key)
        return int(v) if v is not None else -1
    takes = kinds["take"] + kinds["takeRecheck"]
    ok = summary is not None and not heal_diff and s_int("healMismatch") == 0 and s_int("healNoPawn") == 0 and s_int("heals") == takes and takes > 0
    fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} heals: takes {takes} healed through the pad {s_int('heals')} health differences {s_int('healMismatch')} (lines {len(heal_diff)}) no body {s_int('healNoPawn')}")
    for line in heal_diff[:5]:
        print("    " + line)
    print(f"     note: health before a take was not the server's {s_int('healBeforeFix')} times (lines {len(heal_before)})")
    for line in heal_before[:8]:
        print("    " + line)

    ok = summary is not None and s_int("stateChecks") > 0 and s_int("stateDiff") == 0 and s_int("noActor") == 0 and s_int("badIndex") == 0 and s_int("bound") >= s_int("pads") > 0
    fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} drawing: pads {s_int('pads')} bound {s_int('bound')} state checks {s_int('stateChecks')} differences {s_int('stateDiff')} messages without a map pad {s_int('noActor')} bad index {s_int('badIndex')}")
    if summary is None:
        print("    no MOB_PADS line (the bridge did not finish normally)")
    print(f"PADS_BRIDGE {'PASS' if fails == 0 else 'FAIL'} fails={fails}")
    sys.exit(0 if fails == 0 else 1)


if __name__ == "__main__":
    main()
