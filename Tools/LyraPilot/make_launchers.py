# Writes the jump pad file the server reads with --launchers= (jumppad-plan j-1).
# The file holds Epic map values, so it stays outside the repo (next to pads_expanse.txt in Client/Lyra/Saved/MOB, which git ignores).
#   python make_launchers.py --dump=<weapons_launchers.txt> --out=<launchers file> [--devices=<bridge log with MOB_DEVICE lines>]
# --dump    : a dump_weapons.ps1 -MapPath /ShooterMaps/Maps/L_Expanse -Launchers run. MOB_LAUNCHER* lines are live values
#             (after the construction scripts ran), the "=== GRAPH" sections are the launcher blueprint graphs.
# --devices : optional cross-check - every launcher actor location must match a MOB_DEVICE at=(x,y,z) line within 1 cm.
# Shared/JumpPads.cpp hard-codes the constants of the two launch rules (the graph nodes, see GRAPH_GUARDS below); if a guard line
# is missing from the graph text the rules changed and nothing is written.
# Line = "LAUNCHER kind name sphereX sphereY sphereZ sphereRadius upOverride multiplier useTarget targetX targetY targetZ minVerticalVelocity noFallEntry";
# kind = up | push.
import re
import sys
import pathlib

KINDS = {"B_Launcher_Up_C": "up", "B_Launcher_Push_C": "push"}

# Graph lines the Shared launch rules depend on (one block per graph, each must appear somewhere in that graph's text).
GRAPH_GUARDS = {
    "/ShooterCore/Blueprint/B_Launcher.B_Launcher": [
        "[K2Node_ComponentBoundEvent] On Component Begin Overlap (Sphere)",
        "> Is Not Valid -> Overlap.execute",          # only pawns without an AI controller are launched
        "< Condition = true -> Overlap.Return",       # the cue plays when Overlap returns true
    ],
    "/ShooterCore/Blueprint/B_Launcher_Up.B_Launcher_Up": [
        "< bXYOverride = false",
        "< bZOverride = true",
        "< Z = 0.0 -> Get UpOverride.UpOverride",
        "< A -> GetLastUpdateVelocity.ReturnValue_Z",
    ],
    "/ShooterCore/Blueprint/B_Launcher_Push.B_Launcher_Push": [
        "< bXYOverride = false",
        "< bZOverride = false",
        "< Condition = true -> Get Use Target.Use Target",
        "< B = 2.100000",
        "< B = 2.000000",
        "< Min = 250.000000",
        "< Max = 850.000000",
        "< Min = -12000.000000",
        "< Max = 12000.000000",
    ],
}


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
    return bad


def vec(s):
    return [float(x) for x in s.split(",")]


def main():
    lines = pathlib.Path(arg("dump")).read_text(encoding="utf-8", errors="replace").splitlines()
    out = pathlib.Path(arg("out"))
    bad = check_guards(graphs(lines))

    actors = {}
    head = re.compile(r"MOB_LAUNCHER name=(\S+) class=(\S+) loc=(\S+) rot=(\S+) scale=(\S+)$")
    var = re.compile(r"MOB_LAUNCHER_VAR name=(\S+) owner=\S+ var=(.+?) type=\S+ value=(.*)$")
    comp = re.compile(r"MOB_LAUNCHER_COMP name=(\S+) comp=(\S+) class=(\S+) parent=\S+ overlap=(\d) profile=\S+ collision=-?\d+ loc=(\S+) rot=\S+ scale=\S+ shape=(.*)$")
    for line in lines:
        m = head.match(line)
        if m:
            actors[m.group(1)] = {"class": m.group(2), "loc": vec(m.group(3)), "vars": {}, "spheres": []}
            continue
        m = var.match(line)
        if m:
            actors[m.group(1)]["vars"][m.group(2)] = m.group(3)
            continue
        m = comp.match(line)
        if m and m.group(3) == "SphereComponent" and m.group(4) == "1":
            r = re.search(r"scaledR=(\S+)", m.group(6))
            actors[m.group(1)]["spheres"].append((m.group(2), vec(m.group(5)), float(r.group(1))))

    rows = []
    for name, a in sorted(actors.items()):
        kind = KINDS.get(a["class"])
        if not kind:
            print(f"unknown launcher class {a['class']} {name}")
            bad += 1
            continue
        if len(a["spheres"]) != 1 or a["spheres"][0][0] != "Sphere":
            print(f"{name}: want one overlapping SphereComponent named Sphere, got {a['spheres']}")
            bad += 1
            continue
        v = a["vars"]
        _, c, r = a["spheres"][0]
        target = re.match(r"\(X=(\S+),Y=(\S+),Z=(\S+)\)", v.get("Target", "(X=0,Y=0,Z=0)"))
        rows.append((kind, name, c, r, float(v["UpOverride"]), float(v.get("Multiplier", "0")), 1 if v.get("Use Target") == "True" else 0,
                     [float(target.group(i)) for i in (1, 2, 3)], float(v["MinimumVerticalVelocityAllowed"]),
                     1 if v["Do Not Activate on Fall Entry"] == "True" else 0, a["loc"]))

    devices = arg("devices", required=False)
    if devices:
        at = [vec(m.group(1)) for m in re.finditer(r"MOB_DEVICE name=\S+ class=B_Launcher\S* shapes=\d+ at=\(([^)]*)\)",
                                                    pathlib.Path(devices).read_text(encoding="utf-8", errors="replace"))]
        for row in rows:
            if not any(max(abs(p - q) for p, q in zip(row[10], d)) <= 1.0 for d in at):
                print(f"{row[1]}: no MOB_DEVICE at {row[10]}")
                bad += 1
        print(f"devices checked: {len(rows)} launchers vs {len(at)} MOB_DEVICE launcher lines")

    if bad or not rows:
        sys.exit(f"not written: {bad} problem(s), {len(rows)} launcher(s)")
    text = ["# MO jump pads - read by Server --launchers= (Tools/LyraPilot/make_launchers.py). Epic map values: keep out of the repo.",
            f"# source: --dump={arg('dump')}",
            "# LAUNCHER kind name sphereX sphereY sphereZ sphereRadius upOverride multiplier useTarget targetX targetY targetZ minVerticalVelocity noFallEntry"]
    for kind, name, c, r, up, mul, use, t, minvz, nofall, _ in rows:
        text.append(f"LAUNCHER {kind} {name} {c[0]:.4f} {c[1]:.4f} {c[2]:.4f} {r:.4f} {up:.6f} {mul:.6f} {use} {t[0]:.6f} {t[1]:.6f} {t[2]:.6f} {minvz:.6f} {nofall}")
    out.write_text("\n".join(text) + "\n", encoding="utf-8")
    print(f"wrote {out} launchers={len(rows)}")


if __name__ == "__main__":
    main()
