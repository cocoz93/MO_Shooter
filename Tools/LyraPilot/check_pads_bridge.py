# Health pads on screen (content B-3): what the bridge got and drew against what the server sent.
#   python check_pads_bridge.py --server=<padsb_<tag>_server.txt> --lyra=<padsb_<tag>.log>      (record_pads_bridge.ps1)
# 1. Messages: every server PADEV take / takeRecheck / ready, and every sync sent to all or to the bridge's own player, from the bridge's first one on,
#    up to the last state tick the bridge got (MOB_PADS lastStateTick), must arrive in the same order with the same tick, pad, kind, availability,
#    ready tick, picker, heal and health after (bridge MOB_PAD lines).
# 2. Heals: no MOB_PAD_HEAL_DIFF (the pad's own effect left a health other than the server's), every take healed through the pad (heals = takes),
#    no take without a body. MOB_PAD_HEAL_BEFORE lines (Lyra's health before the take was not the server's) are listed, not failed:
#    a debug place changes health without a damage message, so a take in that same tick starts from the old health.
# 3. Drawing: the bridge's once-a-second compare of Lyra's pad state with the server's ran (stateChecks > 0) and never differed, every pad was bound to a map actor.
# Weapon pads (content W-4): a weapon take also carries the server's give result, weapon, quick slot and ammo after the take; those are part of the
#   message compare (the slot is where the weapon sits in the server's PADEV slots list).
#   python check_pads_bridge.py ... --weapons      (record_pads_bridge.ps1 -Weapons: the bridge runs -MOBWeaponScript, the probe --weapons)
# 4. Bag (--weapons only, bridge MOB_WEAPONPADS line): every weapon take of mine went through the pad's own GiveWeapon (gives = my server takes > 0)
#    and left Lyra's quick bar, held slot and that weapon's ammo as the message says (giveDiff 0); the prediction got each take (received = my takes)
#    and never had to fix one (noSlot, resultDiffs, ammoFixes 0); prediction bag and ammo equal the server's on every compared state (invDiff, ammoDiff 0,
#    with states holding two or more weapons > 0); the once-a-second compare of Lyra's quick bar with the last server state ran and never differed;
#    every take by someone else played the pickup cue on that body (cues = their takes); the script found its four pads and ran to the end.
#    With --weapons the heal verdict may compare nothing (no health pad is taken in that run) and says so.
# Spare-full failures (L-9-a): the server sends PICKUP_FAIL_FULL to the picker only (PADEV kind=full give=full); the ones to the bridge's own player
#   are part of the message compare with give, weapon and ammo (the server line has no slot, so the slot is not compared).
# 5. Fail message (--weapons only, bridge MOB_WEAPONPADS line): every spare-full failure of mine called the pad's own GiveWeapon (failMine = failGives =
#    my server failures > 0), the graph went down its failure path and sent exactly one failure message to my player controller per call
#    (failMsgs = my failures, which Lyra's HUD shows as text), Lyra's ammo stayed the server's (failDiff 0) and no body was missing (failNoPawn 0).
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


# Shared/Loadout.h EGiveResult in wire order, named as the server's PADEV give= (Shared/Loadout.cpp GiveResultName)
GIVE_NUMBERS = {"new": 0, "newNoSwitch": 1, "ammo": 2}
GIVE_FULL = 3   # EGiveResult::Full — a spare-full failure (PICKUP_FAIL_FULL)
HEAL_KIND = 0   # Shared/Pickups.h EPadKind::Heal (bridge MOB_PAD kind=)
SCRIPT_STEPS = 23   # MOBridgeSubsystem.cpp kWsSteps


def main():
    weapons = "--weapons" in sys.argv[1:]
    srv = pathlib.Path(arg("server")).read_text(encoding="utf-8", errors="replace").splitlines()
    lyra = pathlib.Path(arg("lyra")).read_text(encoding="utf-8", errors="replace").splitlines()
    me = None
    got, heal_diff, heal_before, summary = [], [], [], None
    give_diff, inv_diff, bag_diff_1s, bag_diff, weapon_summary = [], [], [], [], None
    fail_diff = []
    for line in lyra:
        if me is None and "MOB_WELCOME player=" in line:
            me = int(num(line, "player"))
        i = line.find("MOB_PAD tick=")
        if i >= 0:
            t = line[i:]
            head = (int(num(t, "tick")), int(num(t, "pad")), re.search(r"reason=(\S+)", t).group(1), int(num(t, "avail")), int(num(t, "ready")), int(num(t, "picker")))
            kind = num(t, "kind")
            if head[2] in ("take", "takeRecheck") and kind is not None and int(kind) != HEAL_KIND:
                got.append(head + (int(num(t, "give")), int(num(t, "weapon")), int(num(t, "slot")), int(num(t, "mag")), int(num(t, "spare"))))
            elif head[2] == "failFull":
                got.append(head + (int(num(t, "give")), int(num(t, "weapon")), int(num(t, "mag")), int(num(t, "spare"))))
            else:
                got.append(head + (round(float(num(t, "heal")), 1), round(float(num(t, "hpAfter")), 1)))
        for key, bucket in (("MOB_PAD_HEAL_DIFF", heal_diff), ("MOB_PAD_HEAL_BEFORE", heal_before), ("MOB_WEAPON_GIVE_DIFF", give_diff),
                            ("MOB_INV_DIFF", inv_diff), ("MOB_BAG_DIFF_1S", bag_diff_1s), ("MOB_BAG_DIFF ", bag_diff)):
            if key in line:
                bucket.append(line[line.find(key):])
        if "MOB_PADS pads=" in line:
            summary = line[line.find("MOB_PADS"):]
        if "MOB_WEAPONPADS " in line:
            weapon_summary = line[line.find("MOB_WEAPONPADS"):]
        if "MOB_PAD_FAIL " in line and " ok=0" in line:
            fail_diff.append(line[line.find("MOB_PAD_FAIL "):])
    if me is None:
        sys.exit("FAIL no MOB_WELCOME in the bridge log")

    want = []
    for line in srv:
        m = re.match(r"PADEV tick=(\d+) room=\d+ pad=(\d+) kind=(\S+)(.*)", line)
        if not m:
            continue
        tick, pad, kind, rest = int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)
        if kind in ("take", "takeRecheck") and " give=" in rest:
            give = re.search(r"give=(\S+)", rest).group(1)
            weapon = int(num(rest, "weapon"))
            slots = [int(s) for s in re.search(r"slots=(\S+)", rest).group(1).split(",")]
            want.append((tick, pad, kind, 0, int(num(rest, "ready")), int(num(rest, "player")),
                         GIVE_NUMBERS.get(give, -1), weapon, slots.index(weapon) if weapon in slots else -1, int(num(rest, "mag")), int(num(rest, "spare"))))
        elif kind == "full" and " give=full " in rest and int(num(rest, "player")) == me:
            # spare-full failure sent to my player only (Room.cpp SendPad PICKUP_FAIL_FULL): pad still on, no ready tick
            want.append((tick, pad, "failFull", 1, 0, me, GIVE_FULL, int(num(rest, "weapon")), int(num(rest, "mag")), int(num(rest, "spare"))))
        elif kind == "full":
            continue   # health pad full, post-game, someone else's failure: no message to the bridge
        elif kind in ("take", "takeRecheck"):
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
    weapon_takes = [e for e in want if e[2] in ("take", "takeRecheck") and len(e) == 11]
    takes = kinds["take"] + kinds["takeRecheck"] - len(weapon_takes)   # health pad takes
    ok = summary is not None and not heal_diff and s_int("healMismatch") == 0 and s_int("healNoPawn") == 0 and s_int("heals") == takes and (takes > 0 or weapons)
    fails += not ok
    vacuous = " (none taken - weapons run, nothing compared)" if takes == 0 and weapons else ""
    print(f"{'PASS' if ok else 'FAIL'} heals: takes {takes} healed through the pad {s_int('heals')} health differences {s_int('healMismatch')} (lines {len(heal_diff)}) no body {s_int('healNoPawn')}{vacuous}")
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

    if weapons:
        w = weapon_summary or ""
        def w_int(key):
            v = num(w, key)
            return int(v) if v is not None else -1
        mine = [e for e in weapon_takes if e[5] == me]
        others = len(weapon_takes) - len(mine)
        by_give = {g: sum(1 for e in mine if e[6] == n) for g, n in GIVE_NUMBERS.items()}
        ok = (weapon_summary is not None and len(mine) > 0 and w_int("takesMine") == len(mine) and w_int("gives") == len(mine) and w_int("giveDiff") == 0
              and w_int("received") == len(mine) and w_int("noSlot") == 0 and w_int("resultDiffs") == 0 and w_int("ammoFixes") == 0
              and w_int("invCompared") > 0 and w_int("invMulti") > 0 and w_int("invDiff") == 0 and w_int("ammoCompared") > 0 and w_int("ammoDiff") == 0
              and w_int("bag1s") > 0 and w_int("bagDiff1s") == 0
              and w_int("takesOther") == others and w_int("cues") == others and w_int("noPawn") == 0 and w_int("bad") == 0
              and w_int("noPad") == 0 and w_int("steps") >= SCRIPT_STEPS)
        fails += not ok
        print(f"{'PASS' if ok else 'FAIL'} bag: my weapon takes {len(mine)} (new {by_give['new']} newNoSwitch {by_give['newNoSwitch']} ammo {by_give['ammo']})"
              f" bridge {w_int('takesMine')} gives {w_int('gives')} give differences {w_int('giveDiff')} (lines {len(give_diff)});"
              f" prediction received {w_int('received')} rerun {w_int('rerun')} noSlot {w_int('noSlot')} resultDiffs {w_int('resultDiffs')} ammoFixes {w_int('ammoFixes')};"
              f" compared states {w_int('invCompared')} (two+ weapons {w_int('invMulti')}) bag differences {w_int('invDiff')} ammo compared {w_int('ammoCompared')} differences {w_int('ammoDiff')} (lines {len(inv_diff)});"
              f" 1 s checks {w_int('bag1s')} differences {w_int('bagDiff1s')} (lines {len(bag_diff_1s)});"
              f" others' takes {others} bridge {w_int('takesOther')} cues {w_int('cues')} no body {w_int('noPawn')} bad {w_int('bad')};"
              f" script pads missing {w_int('noPad')} steps {w_int('steps')}/{SCRIPT_STEPS} places {w_int('places')}")
        for line in (give_diff + inv_diff + bag_diff_1s)[:8]:
            print("    " + line)
        print(f"     note: states where Lyra's bar was put back to the server's {w_int('bagDiffStates')} (adds {w_int('bagAdds')} removes {w_int('bagRemoves')} strays {w_int('bagStrays')}, lines {len(bag_diff)})")
        for line in bag_diff[:8]:
            print("    " + line)
        if weapon_summary is None:
            print("    no MOB_WEAPONPADS line (old bridge build or the bridge did not finish normally)")
        my_fails = sum(1 for e in want if e[2] == "failFull")
        ok = (weapon_summary is not None and my_fails > 0 and w_int("failMine") == my_fails and w_int("failGives") == my_fails
              and w_int("failMsgs") == my_fails and w_int("failDiff") == 0 and w_int("failNoPawn") == 0)
        fails += not ok
        print(f"{'PASS' if ok else 'FAIL'} fail message: my spare-full failures {my_fails} bridge {w_int('failMine')} graph called {w_int('failGives')}"
              f" failure-path messages {w_int('failMsgs')} differences {w_int('failDiff')} (lines {len(fail_diff)}) no body {w_int('failNoPawn')}")
        for line in fail_diff[:5]:
            print("    " + line)
        print(f"     note: other failure-channel messages (not during the call or not mine) {w_int('failMsgsElsewhere')}")
    print(f"PADS_BRIDGE {'PASS' if fails == 0 else 'FAIL'} fails={fails}")
    sys.exit(0 if fails == 0 else 1)


if __name__ == "__main__":
    main()
