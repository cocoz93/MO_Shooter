# Judges one run_game2.ps1 run: each Lyra against the server, and the two Lyras against each other.
#   python check_game2.py <Tag> [--dir=<folder>] [--players=10]
# --players=2 means no bots: every damage event is then one Lyra hitting the other.
# Prints every count next to its verdict, because a 0 with nothing compared means nothing.
import re
import sys
import pathlib

DEFAULT_DIR = r"C:\Users\USER\Desktop\MyGit\MO_Shooter\Client\Lyra\Saved\MOB"


def read(path):
    p = pathlib.Path(path)
    return p.read_text(encoding="utf-8", errors="replace").splitlines() if p.exists() else None


def field(line, name):
    m = re.search(r"\b" + name + r"=(\S+)", line)
    return m.group(1) if m else None


def strip(line):
    return re.sub(r"^\[[^\]]*\]\[[^\]]*\]", "", line)


fails = []


def check(label, ok, detail=""):
    print(f"  {'OK ' if ok else 'BAD'} {label} {detail}")
    if not ok:
        fails.append(label)


def load_lyra(lines):
    out = {"lines": [strip(x) for x in lines if "MOB_" in x or "Fatal error" in x or "Assertion failed" in x]}
    for key in ["MOB_SUMMARY ", "MOB_SUMMARY2", "MOB_SUMMARY3", "MOB_SUMMARY4", "MOB_SUMMARY5", "MOB_SUMMARY6", "MOB_WELCOME", "MOB_RESULT", "MOB_PICKUPS", "MOB_PAWN "]:
        out[key.strip()] = next((x for x in out["lines"] if key in x), "")
    # Kill identity = everything the server sent. Own kill and death counts differ per player only by target, so they stay in.
    out["kills"] = [
        tuple(field(x, n) for n in ["tick", "attacker", "target", "weapon", "weak", "score", "kills", "deaths"])
        for x in out["lines"] if "MOB_KILL " in x
    ]
    out["fatal"] = [x for x in out["lines"] if "Fatal error" in x or "Assertion failed" in x]
    return out


def main():
    tag = sys.argv[1]
    folder = next((a[6:] for a in sys.argv[2:] if a.startswith("--dir=")), DEFAULT_DIR)
    players = int(next((a[10:] for a in sys.argv[2:] if a.startswith("--players=")), "10"))
    base = pathlib.Path(folder) / f"game2_{tag}"

    server = read(f"{base}_server.txt") or []
    bots = read(f"{base}_bots.txt") or []
    lyra = {}
    for name in "AB":
        raw = read(f"{base}_lyra{name}.log")
        if raw is None:
            print(f"Lyra {name} log missing")
            fails.append(f"log {name}")
            continue
        lyra[name] = load_lyra(raw)

    print("[server]")
    room = next((x for x in server if x.startswith("ROOM ")), "")
    router = next((x for x in server if x.startswith("ROUTER ")), "")
    reject = next((x for x in server if x.startswith("REJECT ")), "")
    shots = next((x for x in server if x.startswith("SERVER shots")), "")
    team = next((x for x in server if x.startswith("TEAMSCORE")), "")
    for line in [room, router, reject, shots, team]:
        print("  " + line[:220])
    check(f"{players} players in one room", field(room, "playersPeak") == str(players) and field(router, "roomsCreated") == "1")
    check("no seat refused or packet without a room", field(router, "helloRejected") == "0" and field(router, "packetsNoRoom") == "0")
    for name in ["badValue", "moveOver", "weaponId", "aimFar", "swap", "badPackets"]:
        check(f"server reject {name}=0", field(reject, name) == "0", f"(shots={field(shots, 'shots')})")
    check("server saw shots", int(field(shots, "shots") or 0) > 0, f"shots={field(shots, 'shots')}")
    check("match finished", field(room, "finished") == "1")

    if players > 2:
        print("[bots]")
        print("  " + " | ".join(x[:120] for x in bots[-2:]))
        check("bots RESULT PASS", any(x.strip() == "RESULT PASS" for x in bots))
    else:
        print("[no bots]")

    srec = pathlib.Path(f"{base}_server_record.csv")
    brec = pathlib.Path(f"{base}_bot_record.csv")
    if players == 2:
        print(f"  info server record {srec.stat().st_size if srec.exists() else 'missing'}B (no bot record to compare)")
    elif srec.exists() and brec.exists():
        check("server record bytes = bot record bytes", srec.read_bytes() == brec.read_bytes(), f"({srec.stat().st_size}B)")
    else:
        check("record files exist", False)

    ids = {}
    for name, l in lyra.items():
        print(f"[Lyra {name}]")
        for key in ["MOB_WELCOME", "MOB_SUMMARY", "MOB_SUMMARY2", "MOB_SUMMARY3", "MOB_SUMMARY4", "MOB_SUMMARY5", "MOB_SUMMARY6"]:
            print("  " + l[key][:300])
        ids[name] = field(l["MOB_WELCOME"], "player")
        s2, s3, s4, s5 = l["MOB_SUMMARY2"], l["MOB_SUMMARY3"], l["MOB_SUMMARY4"], l["MOB_SUMMARY5"]
        check("no crash line", not l["fatal"], str(l["fatal"][:1]))
        check("reached result", field(l["MOB_SUMMARY"], "why") == "result", f"why={field(l['MOB_SUMMARY'], 'why')}")
        check(f"{players - 1} others on screen", field(s4, "remotes") == str(players - 1) and field(s4, "remoteFail") == "0", f"remotes={field(s4, 'remotes')} fail={field(s4, 'remoteFail')}")
        # Pawn blocking (D83) is always on for the bridge (it has no switch to turn it off), so this is the regress / load rule for a blocking run
        # (BotClient pawnBlock branch, c290178): reconciled states exist and the prediction saw no odd pawn overlap. Corrections then come from
        # knowing the other pawn's place one state late, so they are printed, not judged. A strict corrections=0 run needs blocking off.
        m = re.search(r"\bposDiff n=(\d+)", s2)
        reconciled = int(m.group(1)) if m else 0
        pawn = l["MOB_PAWN"]
        check("pawn blocking: reconciled and predOdd=0", reconciled > 0 and field(pawn, "predOdd") == "0",
              f"reconciled={reconciled} predOdd={field(pawn, 'predOdd')} corrections={field(s3, 'corrections')} correctionMax={field(s3, 'correctionMax')} of states={field(s2, 'states')}")
        hp_n = int(field(s5, "hpCompared") or 0)
        check("hp compared and equal", hp_n > 0 and field(s5, "hpDiffCount") == "0", f"hpCompared={hp_n} hpDiffCount={field(s5, 'hpDiffCount')} hpFixes={field(l['MOB_PICKUPS'], 'hpFixes')}")
        check("damage equal", int(field(s5, "damageApplied") or 0) > 0 and field(s5, "damageMismatch") == "0", f"damageApplied={field(s5, 'damageApplied')} mismatch={field(s5, 'damageMismatch')} noTarget={field(s5, 'damageNoTarget')}")
        check("score equal", field(s5, "scoreFixes") == "0" and field(s5, "scoreServer") == field(s5, "scoreLyra") and field(s5, "scoreServer") == f"{field(team, 'a')}:{field(team, 'b')}", f"server={field(s5, 'scoreServer')} lyra={field(s5, 'scoreLyra')} fixes={field(s5, 'scoreFixes')}")
        check("kills = server kills", len(l["kills"]) == int(field(next((x for x in server if x.startswith('SERVER kills')), ''), "kills") or -1), f"lyra={len(l['kills'])}")

    if len(lyra) == 2:
        print("[Lyra A vs B]")
        check("different players", ids["A"] != ids["B"] and None not in ids.values(), f"A={ids['A']} B={ids['B']}")
        a, b = lyra["A"]["kills"], lyra["B"]["kills"]
        same = sum(1 for x, y in zip(a, b) if x == y)
        check("same kill list", a == b and len(a) > 0, f"compared={len(a)}/{len(b)} same={same}")
        check("same result line", field(lyra["A"]["MOB_RESULT"], "score") == field(lyra["B"]["MOB_RESULT"], "score") and lyra["A"]["MOB_RESULT"] != "", lyra["A"]["MOB_RESULT"][:80])
        pair = {ids["A"], ids["B"]}
        between = [k for k in a if k[1] in pair and k[2] in pair]
        if players == 2:
            da, db = field(lyra["A"]["MOB_SUMMARY2"], "damages"), field(lyra["B"]["MOB_SUMMARY2"], "damages")
            check("Lyras hit each other, both saw the same count", int(da or 0) > 0 and da == db, f"damages A={da} B={db} server={field(shots, 'damageEvents')}")
            check("kill between the Lyras", len(between) > 0, f"count={len(between)}")
        print(f"  info kills between the two Lyras = {len(between)} {between[:4]}")
        print(f"  info kills by Lyra A={sum(1 for k in a if k[1] == ids['A'])} by Lyra B={sum(1 for k in a if k[1] == ids['B'])}")

    print(f"RESULT {'PASS' if not fails else 'FAIL ' + str(len(fails))}")
    for f in fails:
        print("  - " + f)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
