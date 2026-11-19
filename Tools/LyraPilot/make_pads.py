# Writes the pad file the server reads with --pads= (health pads = content B, weapon pads = weapon-pickup-plan W-1).
# The file holds Epic map values, so it stays outside the repo (next to map_expanse.mocl in Client/Lyra/Saved/MOB, which git ignores).
#   python make_pads.py --log=<cprobe_pads log> --assets=<asset dump> --out=<pads file>
# --log    : a record_content_probe.ps1 -Mode pads (or weapons) run. Its MOB_CPROBE_PADINFO lines are read from the running game,
#            so location (3 decimals), CoolDownTime (after BeginPlay copies SpawnCoolDownSeconds), pickup data, effect class and capsule are live values.
# --assets : the editor dump (content 0-3) with "## GE <path>" sections (BaseHeal magnitude) and "ACTOR" lines
#            (label, weapon_definition, check_existing_overlap_delay, location to 1 decimal).
# Every pad must be found in both inputs, the two locations must agree within 0.05 cm and a weapon pad's pickup data
# must be the same in both, or nothing is written. Line = "PAD kind name x y z radius halfHeight heal respawn recheck";
# kind = heal | pistol | rifle | shotgun, heal is 0 on weapon pads (Shared/Pickups.h).
import re
import sys
import pathlib

HEAL_CLASS = "B_AbilitySpawner_C"
WEAPON_CLASS = "B_WeaponSpawner_C"
# pickup data asset -> kind (WeaponPickupData_<Name>, B_WeaponSpawner's WeaponDefinition)
WEAPON_KINDS = {"WeaponPickupData_Pistol": "pistol", "WeaponPickupData_Rifle": "rifle", "WeaponPickupData_Shotgun": "shotgun"}


def arg(name):
    for a in sys.argv[1:]:
        if a.startswith("--" + name + "="):
            return a.split("=", 1)[1]
    sys.exit(f"missing --{name}=")


def number(s):
    return float(s)


def main():
    log = pathlib.Path(arg("log")).read_text(encoding="utf-8", errors="replace").splitlines()
    assets = pathlib.Path(arg("assets")).read_text(encoding="utf-8", errors="replace").splitlines()
    out = pathlib.Path(arg("out"))

    # Live values from the game.
    live = {}
    info = re.compile(r"MOB_CPROBE_PADINFO name=(\S+) class=(\S+) loc=X=(\S+) Y=(\S+) Z=(\S+) avail=\d+ coolDownTime=(\S+) def=(\S+) effect=(\S+) capsuleR=(\S+) capsuleHH=(\S+)")
    for line in log:
        m = info.search(line)
        if not m or m.group(2) not in (HEAL_CLASS, WEAPON_CLASS):
            continue
        pad = dict(x=number(m.group(3)), y=number(m.group(4)), z=number(m.group(5)), respawn=number(m.group(6)),
                   r=number(m.group(9)), hh=number(m.group(10)))
        if m.group(2) == HEAL_CLASS:
            effect = re.search(r"/([A-Za-z0-9_]+)\.\1_C'?$", m.group(8))
            if not effect:
                sys.exit(f"effect class not understood: {m.group(8)}")
            pad.update(kind="heal", effect=effect.group(1))
        else:
            data = re.search(r"/([A-Za-z0-9_]+)\.\1'?$", m.group(7))
            if not data or data.group(1) not in WEAPON_KINDS:
                sys.exit(f"weapon pickup data not understood: {m.group(7)}")
            pad.update(kind=WEAPON_KINDS[data.group(1)], data=data.group(1))
        live[m.group(1)] = pad

    # Asset values: BaseHeal per effect, and per actor label / recheck delay / rounded location.
    heal = {}
    current = None
    for line in assets:
        m = re.match(r"## GE (\S+)", line)
        if m:
            current = m.group(1).rsplit("/", 1)[1]
            continue
        if current and current not in heal:
            m = re.search(r'AttributeName="BaseHeal".*?ScalableFloatMagnitude=\(Value=([0-9.]+)', line)
            if m:
                heal[current] = number(m.group(1))
    actors = {}
    act = re.compile(r"^ACTOR (\S+) label=(\S+) class=(\S+) loc=\((\S+),(\S+),(\S+)\) .*weapon_definition=(\S+) .*check_existing_overlap_delay=(\S+)")
    for line in assets:
        m = act.match(line)
        if m and m.group(3) in (HEAL_CLASS, WEAPON_CLASS):
            actors[m.group(1)] = dict(label=m.group(2), x=number(m.group(4)), y=number(m.group(5)), z=number(m.group(6)),
                                      data=m.group(7).rsplit(".", 1)[-1], recheck=number(m.group(8)))

    if not live:
        sys.exit("no pads in --log")
    if sorted(live) != sorted(actors):
        sys.exit(f"pads differ between inputs: log={sorted(live)} assets={sorted(actors)}")
    rows = []
    for name in sorted(live):
        a, b = live[name], actors[name]
        far = max(abs(a[k] - b[k]) for k in ("x", "y", "z"))
        if far > 0.05 + 1e-9:
            sys.exit(f"{name}: location differs by {far:.3f} between inputs")
        if a["kind"] == "heal":
            if a["effect"] not in heal:
                sys.exit(f"{name}: no BaseHeal for {a['effect']} in --assets")
            rows.append((name, b["label"], a, heal[a["effect"]], b["recheck"], far, a["effect"]))
        else:
            if a["data"] != b["data"]:
                sys.exit(f"{name}: pickup data differs between inputs: log={a['data']} assets={b['data']}")
            rows.append((name, b["label"], a, 0.0, b["recheck"], far, a["data"]))

    text = ["# MO pads - read by Server --pads= (Tools/LyraPilot/make_pads.py). Epic map values: keep out of the repo.",
            f"# sources: --log={arg('log')} --assets={arg('assets')}",
            "# PAD kind name x y z radius halfHeight heal respawnSeconds recheckSeconds   (# label effect-or-pickup-data)"]
    for name, label, a, h, recheck, far, what in rows:
        text.append(f"PAD {a['kind']} {name} {a['x']:.3f} {a['y']:.3f} {a['z']:.3f} {a['r']:.1f} {a['hh']:.1f} {h:g} {a['respawn']:g} {recheck:g}   # {label} {what}")
    out.write_text("\n".join(text) + "\n", encoding="ascii", newline="\n")
    for name, label, a, h, recheck, far, what in rows:
        print(f"PAD {label} kind={a['kind']} heal={h:g} respawn={a['respawn']:g} recheck={recheck:g} at=({a['x']:.3f},{a['y']:.3f},{a['z']:.3f}) r={a['r']:g} hh={a['hh']:g} logVsAssets={far:.3f}")
    kinds = {}
    for row in rows:
        kinds[row[2]["kind"]] = kinds.get(row[2]["kind"], 0) + 1
    print(f"wrote {len(rows)} pads ({' '.join(f'{k}={kinds[k]}' for k in sorted(kinds))}) -> {out}")


if __name__ == "__main__":
    main()
