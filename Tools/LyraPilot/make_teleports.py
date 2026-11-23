# Writes the teleport file the server reads with --teleports= (teleport-plan t-1).
# The file holds Epic map values, so it stays outside the repo (next to launchers_expanse.txt in Client/Lyra/Saved/MOB, which git ignores).
#   python make_teleports.py --dump=<weapons_teleports.txt> --out=<teleports file> [--devices=<bridge log with MOB_DEVICE lines>]
# --dump    : a dump_weapons.ps1 -MapPath /ShooterMaps/Maps/L_Expanse -Teleports run. MOB_TELEPORT* lines are live values
#             (after the construction scripts ran), the "=== GRAPH" sections are the B_Teleport / cue graphs.
# --devices : optional cross-check - every teleport actor location must match a MOB_DEVICE at=(x,y,z) line within 1 cm.
# Shared/Teleports.cpp hard-codes the rule read from the B_Teleport graph (GRAPH_GUARDS / GRAPH_BLOCKS below); if a guard is
# missing from the graph text the rule changed and nothing is written.
# The rule (B_Teleport EventGraph, authority path): Box begin overlap -> Cast To Character -> destination = Random Array Item of
# TeleportingTarget (must hold exactly one item here, so it is not random) -> Teleport(dest location, Rotation From X Vector(dest
# exit forward)) -> if Teleport succeeded and LaunchOnTeleport: LaunchCharacter(dest exit forward * own LaunchMultiplier, XY and Z
# override) -> if bForceDirection: SetControlRotation(same rotation).
# dest location = dest "Exit Direction" world location, plus (KeepReleativeEntryUponExit) the pawn mesh location projected onto
# the entered device's root plane (normal = root forward) minus the root location, its world Z clamped to [0, TeleporterHeight - 100].
# Lines:
#   TELEPORT_PAWN meshBaseZ standHalfHeight      (standing: mesh Z = capsule centre + meshBaseZ. Crouching raises the mesh by
#                                                  standHalfHeight - crouchedHalfHeight, so mesh Z = capsule bottom + standHalfHeight + meshBaseZ either way)
#   TELEPORT index name target boxC(3) boxX(3) boxY(3) boxZ(3) boxExt(3) root(3) rootFwd(3) exit(3) exitFwd(3)
#            keepRelative launchOnTeleport launchMultiplier forceDirection offsetZMax
#   box* / root* = the entered device, exit* = its target's Exit Direction arrow, target = the target's index in this file.
import math
import re
import sys
import pathlib

TELEPORT_GRAPH = "/ShooterCore/Blueprint/B_Teleport.B_Teleport"
GRAPH_GUARDS = {
    TELEPORT_GRAPH: [
        "[K2Node_ComponentBoundEvent] On Component Begin Overlap (Box)",
        "> then -> Cast To Character.execute",
        "> OutItem -> Set TeleporterDestination.TeleporterDestination",      # destination = Random Array Item(TeleportingTarget)
        "< DestRotation = 0, 0, 0 -> Rotation From X Vector.ReturnValue",
        "< A = false -> Teleport.ReturnValue",                               # launch only when Teleport succeeded ...
        "< B = false -> Get LaunchOnTeleport.LaunchOnTeleport",              # ... and LaunchOnTeleport
        "< bXYOverride = true",
        "< bZOverride = true",
        "< Condition = true -> Get bForceDirection.bForceDirection",
        "< bPickA = false -> NOT Boolean.ReturnValue",                       # SelectVector: A = exit location, B = offset location
        "< B = 0, 0, 0 -> GetOffsetLocation.Offset Exit Location",
        "< PlaneNormal = 0, 0, 0 -> GetForwardVector.ReturnValue",
        "< B = 100.000000",                                                  # offset Z max = TeleporterHeight - 100
        "< Min = 0.0",
        "> Mesh -> GetOffsetLocation.Mesh",
    ],
}
# Consecutive lines: the launch multiplier is read from self (the entered device) - the node has no self pin wired.
GRAPH_BLOCKS = {
    TELEPORT_GRAPH: [
        ["[K2Node_VariableGet] Get LaunchMultiplier", "> LaunchMultiplier = 0.0 -> vector * vector.B"],
        ["[K2Node_PromotableOperator] vector * vector", "< A -> 경유 노드.OutputPin", "< B -> Get LaunchMultiplier.LaunchMultiplier"],
    ],
}
NOTE_ONLY = ["[K2Node_DynamicCast] Cast To B_Grenade"]   # grenades are teleported too (Teleport Grenade) - not in this plan, reported


def arg(name, required=True):
    for a in sys.argv[1:]:
        if a.startswith("--" + name + "="):
            return a.split("=", 1)[1]
    if required:
        sys.exit(f"missing --{name}=")
    return None


def graphs(lines):
    out, cur = {}, None
    for line in lines:
        if line.startswith("=== "):
            cur = line[len("=== GRAPH "):].strip() if line.startswith("=== GRAPH ") else None
            if cur:
                out[cur] = []
            continue
        if cur and not line.startswith("MOB_"):
            out[cur].append(line.strip())
    return out


def check_guards(g):
    bad = 0
    for path, wants in GRAPH_GUARDS.items():
        body = g.get(path)
        if body is None:
            print(f"graph missing: {path}")
            bad += 1
            continue
        for want in wants:
            if want not in body:
                print(f"graph guard missing: {path}: {want}")
                bad += 1
        for block in GRAPH_BLOCKS.get(path, []):
            if not any(body[i:i + len(block)] == block for i in range(len(body))):
                print(f"graph block missing: {path}: {block}")
                bad += 1
        for note in NOTE_ONLY:
            if note in body:
                print(f"note: {path} also has '{note}' - grenade teleport is outside teleport-plan (open item)")
    return bad


def vec(s):
    return [float(x) for x in s.split(",")]


def main():
    lines = pathlib.Path(arg("dump")).read_text(encoding="utf-8", errors="replace").splitlines()
    out = pathlib.Path(arg("out"))
    bad = check_guards(graphs(lines))

    actors, pawn = {}, None
    head = re.compile(r"MOB_TELEPORT name=(\S+) class=(\S+) loc=(\S+) rot=(\S+) scale=(\S+)$")
    var = re.compile(r"MOB_TELEPORT_VAR name=(\S+) owner=\S+ var=(.+?) type=\S+ value=(.*)$")
    compx = re.compile(r"MOB_TELEPORT_COMPX name=(\S+) comp=(.+?) loc=(\S+) x=(\S+) y=(\S+) z=(\S+) boxExt=(\S+)$")
    comp = re.compile(r"MOB_TELEPORT_COMP name=(\S+) comp=(.+?) class=(\S+) parent=\S+ overlap=(\d) ")
    pawnre = re.compile(r"MOB_TELEPORT_PAWN name=\S+ meshRel=\S+ baseOffset=(\S+) capsuleR=\S+ capsuleHH=(\S+) meshMinusRoot=(\S+)$")
    for line in lines:
        m = head.match(line)
        if m:
            actors[m.group(1)] = {"class": m.group(2), "loc": vec(m.group(3)), "vars": {}, "comps": {}, "overlap": []}
            continue
        m = var.match(line)
        if m:
            actors[m.group(1)]["vars"][m.group(2)] = m.group(3)
            continue
        m = compx.match(line)
        if m:
            actors[m.group(1)]["comps"][m.group(2)] = {k: vec(m.group(i)) for k, i in (("loc", 3), ("x", 4), ("y", 5), ("z", 6), ("ext", 7))}
            continue
        m = comp.match(line)
        if m and m.group(4) == "1":
            actors[m.group(1)]["overlap"].append((m.group(2), m.group(3)))
            continue
        m = pawnre.match(line)
        if m:
            base, hh, mesh = vec(m.group(1)), float(m.group(2)), float(m.group(3))
            if abs(base[0]) > 1e-6 or abs(base[1]) > 1e-6 or abs(base[2] - mesh) > 1e-4:
                print(f"pawn mesh offset not straight below the capsule: base={base} meshMinusRoot={mesh}")
                bad += 1
            pawn = (base[2], hh)
    if pawn is None:
        print("no MOB_TELEPORT_PAWN line")
        bad += 1

    names = sorted(actors)
    index = {n: i for i, n in enumerate(names)}
    rows = []
    for name in names:
        a = actors[name]
        v, c = a["vars"], a["comps"]
        if a["class"] != "B_Teleport_C":
            print(f"{name}: unexpected class {a['class']}")
            bad += 1
            continue
        if a["overlap"] != [("Box", "BoxComponent")]:
            print(f"{name}: want one overlapping BoxComponent named Box, got {a['overlap']}")
            bad += 1
            continue
        if v.get("ReceiverOnly") != "False":
            print(f"{name}: ReceiverOnly is not False")
            bad += 1
            continue
        targets = re.findall(r"PersistentLevel\.([A-Za-z0-9_]+)", v.get("TeleportingTarget", ""))
        if len(targets) != 1 or targets[0] not in index:
            print(f"{name}: want exactly one known TeleportingTarget (Random Array Item is not reproducible otherwise), got {targets}")
            bad += 1
            continue
        tgt = actors[targets[0]]
        box, root, exitc = c.get("Box"), c.get("DefaultSceneRoot1"), tgt["comps"].get("Exit Direction")
        if not box or not root or not exitc:
            print(f"{name}: missing Box / DefaultSceneRoot1 / target Exit Direction component")
            bad += 1
            continue
        if abs(box["z"][2] - 1.0) > 1e-9 or abs(box["z"][0]) > 1e-9 or abs(box["z"][1]) > 1e-9:
            print(f"{name}: box is tilted (z axis {box['z']}) - Shared/Teleports only handles upright boxes")
            bad += 1
            continue
        if max(abs(p - q) for p, q in zip(root["loc"], a["loc"])) > 1e-3:
            print(f"{name}: root component is not at the actor location")
            bad += 1
            continue
        height = float(v["TeleporterHeight"])
        rows.append((index[name], name, index[targets[0]], box, root, exitc, v["KeepReleativeEntryUponExit"] == "True",
                     v["LaunchOnTeleport"] == "True", float(v["LaunchMultiplier"]), v["bForceDirection"] == "True", height - 100.0, a["loc"]))

    devices = arg("devices", required=False)
    if devices:
        at = [vec(m.group(1)) for m in re.finditer(r"MOB_DEVICE name=\S+ class=B_Teleport_C shapes=\d+ at=\(([^)]*)\)",
                                                    pathlib.Path(devices).read_text(encoding="utf-8", errors="replace"))]
        for row in rows:
            if not any(max(abs(p - q) for p, q in zip(row[11], d)) <= 1.0 for d in at):
                print(f"{row[1]}: no MOB_DEVICE at {row[11]}")
                bad += 1
        print(f"devices checked: {len(rows)} teleports vs {len(at)} MOB_DEVICE teleport lines")

    if bad or not rows or len(rows) != len(names):
        sys.exit(f"not written: {bad} problem(s), {len(rows)} of {len(names)} teleport(s)")

    def f3(p, digits):
        return " ".join(f"{x:.{digits}f}" for x in p)

    text = ["# MO teleports - read by Server --teleports= (Tools/LyraPilot/make_teleports.py). Epic map values: keep out of the repo.",
            f"# source: --dump={arg('dump')}",
            "# TELEPORT_PAWN meshBaseZ standHalfHeight",
            "# TELEPORT index name target boxC boxX boxY boxZ boxExt root rootFwd exit exitFwd keepRelative launchOnTeleport launchMultiplier forceDirection offsetZMax",
            f"TELEPORT_PAWN {pawn[0]:.6f} {pawn[1]:.6f}"]
    for i, name, t, box, root, ex, keep, launch, mul, force, zmax, _ in rows:
        text.append(f"TELEPORT {i} {name} {t} {f3(box['loc'], 9)} {f3(box['x'], 12)} {f3(box['y'], 12)} {f3(box['z'], 12)} {f3(box['ext'], 9)} "
                    f"{f3(root['loc'], 9)} {f3(root['x'], 12)} {f3(ex['loc'], 9)} {f3(ex['x'], 12)} {int(keep)} {int(launch)} {mul:.6f} {int(force)} {zmax:.6f}")
    out.write_text("\n".join(text) + "\n", encoding="utf-8")
    yaw = {i: math.degrees(math.atan2(ex['x'][1], ex['x'][0])) for i, _, _, _, _, ex, *_ in rows}
    for i, name, t, *_ in rows:
        print(f"  {i} {name[:40]} -> {t}  exitYaw={yaw[i]:.3f}")
    print(f"wrote {out} teleports={len(rows)}")


if __name__ == "__main__":
    main()
