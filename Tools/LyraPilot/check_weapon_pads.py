# Weapon pads (content W-2): our server against Lyra, frame by frame.
#   python check_weapon_pads.py --lyra=<cprobe_<tag>.log from record_content_probe.ps1 -Mode weapons> --probe=<ServerProbe --weapons output>
# Both runs follow the same script (MOBContentProbeSubsystem.cpp WEAPONS_SCRIPT = ServerProbe RunWeapons steps) at 60 frames/ticks a second.
# Frames are counted from the start of the first step on both sides (Lyra frame=, server tick - settle startTick).
#   Lyra   : MOB_CPROBE_INV (slots + active slot), MOB_CPROBE_WPAD avail=0/1 (weapon pads only), MOB_CPROBE_FAILMSG
#   server : INVEV (same slot text), PADEV take/takeRecheck -> off, ready -> on, kind=full on a weapon pad -> fail
# Checks, each printed with how many it compared (an empty list on both sides would also be "equal"):
#   1. step starts   — every server step started at the Lyra frame of that step (else the run itself drifted, not the rules)
#   2. bag at start  — the last bag before the script
#   3. events        — the lists above equal, frame and value
#   4. respawn       — Lyra clears the bag when the body goes (pawn=None) and fills a new body; the server keeps the bag while dead and
#                      resets it at respawn. Compared: bag kept while dead (no event between death and respawn on either side but the
#                      body removal) and the new bag. The respawn frame itself is a match-rules timing (5 s timer vs Lyra restart), printed only
#   5. client saw    — at every server bag change the probe's own state shows the held weapon and its ammo of that bag
#   7. pickup msgs   — packet 11 (W-3): every pad message names the pad kind; every server weapon take (PADEV take/takeRecheck) by the probe
#                      came as one message with the same result, weapon, slot (place in PADEV slots=), ammo after, and the input number the
#                      take followed: inside the inputs the server ran that tick (previous tick's lastSeq .. this tick's), and for a recheck
#                      (after all inputs of the tick, Room.cpp TickPads) this tick's lastSeq. Messages that are not a weapon take carry no take
#   8. state slots   — packet 11: at every server bag change the probe's state quickSlots (2 bits a slot) = the INVEV slots
#   6. rejections    — server-only (Lyra picks by slot, so it has no "weapon not owned"): the probe keeps sending the weapon it held
#                      before death until it sees the respawn state, so the first input after respawn may ask for a weapon the new bag
#                      lacks. Every INVEV reject must be that one (within a second after respawn, a weapon not in the new bag)
import re
import sys
import pathlib

# WEAPONS_SCRIPT: name, ticks
STEPS = [("settle", 60), ("rifle_r1", 840), ("away1", 60), ("slot0", 90), ("rifle_low", 30), ("rifle_r3", 120), ("away2", 60),
         ("pistol_p7", 120), ("away3", 60), ("pistol_low", 30), ("reload_press", 6), ("shotgun_s5", 180), ("away4", 60),
         ("pistol_low2", 30), ("reload_pistol", 180), ("slot1", 90), ("rifle_low2", 30), ("reload_rifle", 180), ("slot2", 90),
         ("shotgun_low", 30), ("reload_shotgun", 240), ("die", 900), ("end", 60)]
WEAPON_NAMES = {"pistol": 0, "rifle": 1, "shotgun": 2}
PAD_KINDS = {"heal": 0, "pistol": 1, "rifle": 2, "shotgun": 3}       # EPadKind (Shared/Pickups.h)
GIVE_CODES = {"new": 0, "newNoSwitch": 1, "ammo": 2, "full": 3, "noSlot": 4}   # EGiveResult (Shared/Loadout.h) = GiveResultName
GIVE_NONE = 0xFF                                                     # wire::kPickupGiveNone


def arg(name):
    for a in sys.argv[1:]:
        if a.startswith("--" + name + "="):
            return a.split("=", 1)[1]
    sys.exit(f"missing --{name}=")


def step_starts():
    out, at = {}, 0
    for name, ticks in STEPS:
        out[name] = at
        at += ticks
    return out


def norm_slots(text):
    # "[Pistol:12/48 Rifle:30/60 -]" -> "pistol:12/48 rifle:30/60 -"
    return " ".join(p.lower() for p in text.strip("[]").split())


def lyra_events(lines):
    events, initial, removed = [], None, []
    for line in lines:
        m = re.search(r"MOB_CPROBE_INV frame=(\d+) .*?step=(\S+) .*?pawn=(\S+) slots=(\[[^\]]*\]) active=(-?\d+)", line)
        if m:
            frame, step, pawn, slots, active = int(m.group(1)), m.group(2), m.group(3), norm_slots(m.group(4)), int(m.group(5))
            if step == "wait":
                if pawn != "None":
                    initial = (slots, active)
            elif pawn == "None":
                removed.append(frame)
            else:
                events.append((frame, "inv", f"{slots} active={active}"))
            continue
        m = re.search(r"MOB_CPROBE_WPAD frame=(\d+) .*?step=(\S+) .*?pad=(\S+) avail=(\d)", line)
        if m and m.group(2) != "wait" and m.group(3).startswith("B_WeaponSpawner"):
            events.append((int(m.group(1)), "on" if m.group(4) == "1" else "off", m.group(3)))
            continue
        m = re.search(r"MOB_CPROBE_FAILMSG frame=(\d+) .*?step=(\S+)", line)
        if m and m.group(2) != "wait":
            events.append((int(m.group(1)), "fail", ""))
    return events, initial, removed


def server_events(lines):
    starts, pads, inv, seen, rejects = {}, {}, [], [], []
    player, srv = None, []
    for line in lines:
        m = re.match(r"WPROBE step=(\S+) startTick=(\d+)", line)
        if m:
            starts[m.group(1)] = int(m.group(2))
        m = re.match(r"WPROBE player=(\d+) ", line)
        if m:
            player = int(m.group(1))
        m = re.match(r"PAD (\d+) kind=(\S+) name=(\S+) ", line)
        if m:
            pads[int(m.group(1))] = (m.group(2), m.group(3))
        m = re.match(r"WPROBE seen tick=(\d+) step=\S+ weapon=(\d+) mag=(-?\d+) spare=(-?\d+) dead=(\d)(?: slots=(\d+))?", line)
        if m:
            seen.append(tuple(int(m.group(i)) if m.group(i) is not None else -1 for i in range(1, 7)))
        m = re.match(r"INVEV tick=(\d+) room=\d+ player=(\d+) reject weapon=(\d+)", line)
        if m:
            rejects.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
        if line.startswith("PADEV ") or line.startswith("INVEV "):
            srv.append(line)
    t0 = starts.get(STEPS[0][0])
    events, initial = [], None
    for line in srv:
        m = re.match(r"INVEV tick=(\d+) room=\d+ player=(\d+) dead=(\d) slots=(\[[^\]]*\]) active=(\d+)", line)
        if m and int(m.group(2)) == player:
            tick, slots, active = int(m.group(1)), norm_slots(m.group(4)), int(m.group(5))
            inv.append((tick, slots, active, int(m.group(3))))
            if t0 is not None and tick < t0:
                initial = (slots, active)
            elif t0 is not None:
                events.append((tick - t0, "inv", f"{slots} active={active}"))
            continue
        m = re.match(r"PADEV tick=(\d+) room=\d+ pad=(\d+) kind=(\S+)", line)
        if not m or t0 is None:
            continue
        tick, pad, kind = int(m.group(1)), int(m.group(2)), m.group(3)
        if pad not in pads or pads[pad][0] == "heal" or tick < t0:
            continue
        name = pads[pad][1]
        if kind in ("take", "takeRecheck"):
            events.append((tick - t0, "off", name))
        elif kind == "ready":
            events.append((tick - t0, "on", name))
        elif kind == "full":
            events.append((tick - t0, "fail", ""))
    return starts, t0, events, initial, inv, seen, [r for r in rejects if r[1] == player]


def pickup_messages(lines):
    msgs, takes = [], []
    for line in lines:
        m = re.match(r"WPROBE padmsg " + " ".join(f"{k}=(-?\\d+)" for k in ("tick", "pad", "reason", "picker", "kind", "give", "weapon", "slot", "mag", "spare", "seq", "stateSeq", "prevSeq")), line)
        if m:
            msgs.append(dict(zip(("tick", "pad", "reason", "picker", "kind", "give", "weapon", "slot", "mag", "spare", "seq", "stateSeq", "prevSeq"), (int(g) for g in m.groups()))))
        m = re.match(r"PADEV tick=(\d+) room=\d+ pad=(\d+) kind=(take|takeRecheck) give=(\S+) player=(\d+) weapon=(\d+) mag=(-?\d+) spare=(-?\d+) held=\d+ slots=(-?\d+),(-?\d+),(-?\d+)", line)
        if m:
            takes.append(dict(tick=int(m.group(1)), pad=int(m.group(2)), reason=0 if m.group(3) == "take" else 1, give=m.group(4), player=int(m.group(5)),
                              weapon=int(m.group(6)), mag=int(m.group(7)), spare=int(m.group(8)), slots=[int(m.group(i)) for i in (9, 10, 11)]))
    return msgs, takes


def unpack_slots(packed):
    # CLoadout::PackSlots — 2 bits a slot, 0 = empty, else weapon + 1
    return [((packed >> (2 * i)) & 3) - 1 for i in range(3)]


def main():
    sys.stdout.reconfigure(encoding="utf-8")  # the Windows console default (cp949) has no em dash
    lyra, lyra_initial, lyra_removed = lyra_events(pathlib.Path(arg("lyra")).read_text(encoding="utf-8", errors="replace").splitlines())
    starts, t0, srv, srv_initial, srv_inv, seen, rejects = server_events(pathlib.Path(arg("probe")).read_text(encoding="utf-8", errors="replace").splitlines())
    fails = 0
    expect = step_starts()

    # 1. step starts
    drift = [(n, expect[n], starts.get(n) - t0 if starts.get(n) is not None and t0 is not None else None) for n, _ in STEPS]
    bad = [d for d in drift if d[1] != d[2]]
    print(f"{'PASS' if not bad else 'FAIL'} step starts: {len(drift) - len(bad)}/{len(drift)} at the Lyra frame")
    for d in bad:
        print(f"    !! {d[0]} lyra={d[1]} server={d[2]}")
    fails += bool(bad)

    # Lyra events fall inside the step the log names — the step table here is the script both sides ran
    ends = {n: expect[n] + t for n, t in STEPS}
    outside = 0
    for line in pathlib.Path(arg("lyra")).read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.search(r"MOB_CPROBE_(?:INV|WPAD|FAILMSG) frame=(\d+) .*?step=(\S+)", line)
        if m and m.group(2) in ends and not (expect[m.group(2)] <= int(m.group(1)) < ends[m.group(2)]):
            outside += 1
    print(f"{'PASS' if outside == 0 else 'FAIL'} Lyra events inside their named step: outside={outside}")
    fails += outside > 0

    # 2. bag at start
    ok = lyra_initial is not None and lyra_initial == srv_initial
    print(f"{'PASS' if ok else 'FAIL'} bag at start: lyra={lyra_initial} server={srv_initial}")
    fails += not ok

    # 3. events — the die step's bag events are compared in 4
    die_at = expect["die"]
    key = lambda e: (e[0], e[1], e[2])
    a = sorted([e for e in lyra if not (e[1] == "inv" and e[0] >= die_at)], key=key)
    b = sorted([e for e in srv if not (e[1] == "inv" and e[0] >= die_at)], key=key)
    ok = a == b and len(a) > 0
    print(f"{'PASS' if ok else 'FAIL'} events: lyra {len(a)} = server {len(b)} (inv {sum(e[1] == 'inv' for e in a)} · off {sum(e[1] == 'off' for e in a)} · on {sum(e[1] == 'on' for e in a)} · fail {sum(e[1] == 'fail' for e in a)})")
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else None
        y = b[i] if i < len(b) else None
        print(f"    {'  ' if x == y else '!!'} lyra={x} server={y}")
    fails += not ok

    # 4. respawn
    la = [e for e in lyra if e[1] == "inv" and e[0] >= die_at]
    sa = [e for e in srv if e[1] == "inv" and e[0] >= die_at]
    ok = len(la) == 1 and len(sa) == 1 and la[0][2] == sa[0][2] and len(lyra_removed) == 1 and lyra_removed[0] < la[0][0]
    if ok:
        print(f"PASS respawn: bag kept while dead (no change from death {die_at} on either side, Lyra body removed at {lyra_removed[0]}) · new bag {la[0][2]} "
              f"· frame lyra={la[0][0]} server={sa[0][0]} (+{la[0][0] - die_at} / +{sa[0][0] - die_at} after death — match-rules timing, not compared)")
    else:
        print(f"FAIL respawn: lyra={la} removed={lyra_removed} server={sa}")
    fails += not ok

    # 5. client saw — the state of the bag-change tick (last seen line at or before it)
    compared, wrong = 0, []
    for tick, slots, active, dead in srv_inv:
        if t0 is None or tick < t0 or dead:
            continue
        parts = slots.split()
        held = parts[active]
        name, ammo = held.split(":")
        mag, spare = (int(x) for x in ammo.split("/"))
        at = [s for s in seen if s[0] <= tick]
        if not at:
            wrong.append((tick, "no state"))
            continue
        s = at[-1]
        compared += 1
        if (s[1], s[2], s[3]) != (WEAPON_NAMES[name], mag, spare):
            wrong.append((tick, f"bag {held} state weapon={s[1]} {s[2]}/{s[3]}"))
    ok = compared > 0 and not wrong
    print(f"{'PASS' if ok else 'FAIL'} client saw: held weapon and ammo = server bag at {compared} bag changes")
    for w in wrong:
        print(f"    !! {w}")
    fails += not ok

    # 6. rejections
    respawn = sa[0][0] if len(sa) == 1 else None
    new_bag = [p.split(":")[0] for p in sa[0][2].split(" active=")[0].split() if p != "-"] if respawn is not None else []
    stray = [r for r in rejects if respawn is None or not (respawn <= r[0] - t0 < respawn + 60)
             or r[2] in [WEAPON_NAMES[n] for n in new_bag]]
    ok = not stray
    print(f"{'PASS' if ok else 'FAIL'} rejections: {len(rejects)} " + ", ".join(f"weapon={r[2]} at respawn+{r[0] - t0 - respawn}" if respawn is not None else str(r) for r in rejects)
          + (" (old weapon right after respawn)" if rejects and ok else ""))
    for r in stray:
        print(f"    !! reject tick={r[0]} weapon={r[2]}")
    fails += not ok

    # 7. pickup msgs
    probe_lines = pathlib.Path(arg("probe")).read_text(encoding="utf-8", errors="replace").splitlines()
    msgs, takes = pickup_messages(probe_lines)
    pads = {}
    player = None
    for line in probe_lines:
        m = re.match(r"PAD (\d+) kind=(\S+) ", line)
        if m:
            pads[int(m.group(1))] = m.group(2)
        m = re.match(r"WPROBE player=(\d+) ", line)
        if m:
            player = int(m.group(1))
    wrong = []
    kind_bad = [x for x in msgs if pads.get(x["pad"]) is None or PAD_KINDS[pads[x["pad"]]] != x["kind"]]
    wrong += [f"kind pad={x['pad']} msg={x['kind']} file={pads.get(x['pad'])}" for x in kind_bad]
    own_takes = [t for t in takes if t["player"] == player and pads.get(t["pad"]) not in (None, "heal")]
    take_msgs = [x for x in msgs if x["give"] != GIVE_NONE]
    matched = 0
    for t in own_takes:
        found = [x for x in take_msgs if x["tick"] == t["tick"] and x["pad"] == t["pad"] and x["reason"] == t["reason"] and x["picker"] == player]
        if len(found) != 1:
            wrong.append(f"take tick={t['tick']} pad={t['pad']} messages={len(found)}")
            continue
        x = found[0]
        slot = t["slots"].index(t["weapon"]) if t["weapon"] in t["slots"] else -1
        want = (GIVE_CODES[t["give"]], t["weapon"], slot, t["mag"], t["spare"])
        got = (x["give"], x["weapon"], x["slot"], x["mag"], x["spare"])
        seq_ok = x["stateSeq"] >= 0 and x["prevSeq"] >= 0 and x["prevSeq"] <= x["seq"] <= x["stateSeq"] and (t["reason"] == 0 or x["seq"] == x["stateSeq"])
        if want != got or not seq_ok:
            wrong.append(f"take tick={t['tick']} pad={t['pad']} server(give,weapon,slot,mag,spare)={want} msg={got} seq={x['seq']} state={x['prevSeq']}..{x['stateSeq']}")
            continue
        matched += 1
    if len(take_msgs) != len(own_takes):
        wrong.append(f"take messages {len(take_msgs)} != server weapon takes {len(own_takes)}")
    # not a weapon take: no take fields; the input number only with a picker (a heal take carries it)
    empty_bad = [x for x in msgs if x["give"] == GIVE_NONE and ((x["weapon"], x["slot"], x["mag"], x["spare"]) != (0, 0, 0, 0) or (x["picker"] == 0 and x["seq"] != 0))]
    wrong += [f"not a take but carries one: tick={x['tick']} pad={x['pad']} reason={x['reason']}" for x in empty_bad]
    ok = matched > 0 and not wrong
    print(f"{'PASS' if ok else 'FAIL'} pickup msgs: {len(msgs)} messages name the pad kind · weapon takes {matched}/{len(own_takes)} same result · weapon · slot · ammo · input number "
          f"(take {sum(t['reason'] == 0 for t in own_takes)} · recheck {sum(t['reason'] == 1 for t in own_takes)})")
    for w in wrong:
        print(f"    !! {w}")
    fails += not ok

    # 8. state slots — the state of the bag-change tick (last seen line at or before it)
    compared, wrong = 0, []
    for tick, slots, active, dead in srv_inv:
        if t0 is None or tick < t0 or dead:
            continue
        want = [WEAPON_NAMES[p.split(":")[0]] if p != "-" else -1 for p in slots.split()]
        at = [s for s in seen if s[0] <= tick]
        if not at or at[-1][5] < 0:
            wrong.append((tick, "no state slots"))
            continue
        compared += 1
        if unpack_slots(at[-1][5]) != want:
            wrong.append((tick, f"bag {want} state {unpack_slots(at[-1][5])}"))
    ok = compared > 0 and not wrong
    print(f"{'PASS' if ok else 'FAIL'} state slots: quickSlots = server bag at {compared} bag changes")
    for w in wrong:
        print(f"    !! {w}")
    fails += not ok

    print(f"WEAPONPADS {'PASS' if fails == 0 else 'FAIL'} events={len(a)} fails={fails}")
    sys.exit(0 if fails == 0 else 1)


if __name__ == "__main__":
    main()
