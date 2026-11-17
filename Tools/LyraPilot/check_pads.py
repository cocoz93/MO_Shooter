# Health pads (content B): our server against Lyra, frame by frame.
#   python check_pads.py --lyra=<cprobe_<tag>.log from record_content_probe.ps1 -Mode pads> --probe=<ServerProbe --pads output>
# Both runs follow the same script (MOBContentProbeSubsystem.cpp PADS_SCRIPT = ServerProbe RunPads steps) at 60 frames/ticks a second.
# For every step that stands on a pad, events are listed as frames after the step started:
#   Lyra   : MOB_CPROBE_PAD avail=0/1 -> off/on, MOB_CPROBE_HP from->to -> hp
#   server : PADEV take/takeRecheck -> off + hp, ready -> on (the pad of that step only); PADPROBE hpTick (health the client saw) -> seen
# Lyra's list must equal the server's, and the health the client saw must equal the server's own hp events.
# Prints how many events were compared, because an empty list on both sides would also be "equal".
import re
import sys
import pathlib

PAD_STEPS = {"big_full": ("big", 0), "big_hurt": ("big", 0), "small_hurt": ("small", 1)}


def arg(name):
    for a in sys.argv[1:]:
        if a.startswith("--" + name + "="):
            return a.split("=", 1)[1]
    sys.exit(f"missing --{name}=")


def lyra_events(lines):
    steps = {}
    for line in lines:
        m = re.search(r"MOB_CPROBE_PAD t=\S+ step=(\S+) stepT=(\S+) pad=(\S+) avail=(\d)", line)
        if m and m.group(1) in PAD_STEPS and m.group(3) == PAD_STEPS[m.group(1)][0]:
            steps.setdefault(m.group(1), []).append((round(float(m.group(2)) * 60), "on" if m.group(4) == "1" else "off", None))
        m = re.search(r"MOB_CPROBE_HP t=\S+ step=(\S+) stepT=(\S+) from=\S+ to=(\S+)", line)
        if m and m.group(1) in PAD_STEPS:
            steps.setdefault(m.group(1), []).append((round(float(m.group(2)) * 60), "hp", float(m.group(3))))
    return steps


def server_events(lines):
    places = []   # (tick, step) in order
    for line in lines:
        m = re.match(r"PADPROBE step=(\S+) placeTick=(\d+)", line)
        if m:
            places.append((int(m.group(2)), m.group(1)))
    def window(step):
        for i, (tick, name) in enumerate(places):
            if name == step:
                return tick, places[i + 1][0] if i + 1 < len(places) else 1 << 32
        return None
    steps, seen, full = {}, {}, {}
    for step, (_, pad) in PAD_STEPS.items():
        w = window(step)
        if not w:
            continue
        start, end = w
        steps[step], seen[step], full[step] = [], [], 0
        for line in lines:
            m = re.match(r"PADEV tick=(\d+) room=\d+ pad=(\d+) kind=(\S+)(.*)", line)
            if m and int(m.group(2)) == pad and start <= int(m.group(1)) < end:
                f = int(m.group(1)) - start
                kind, rest = m.group(3), m.group(4)
                if kind in ("take", "takeRecheck"):
                    steps[step].append((f, "off", None))
                    steps[step].append((f, "hp", float(re.search(r"hp=\S+>(\S+)", rest).group(1))))
                elif kind == "ready":
                    steps[step].append((f, "on", None))
                elif kind == "full":
                    full[step] += 1
            m = re.match(r"PADPROBE step=(\S+) hpTick=(\d+) hp=\S+>(\S+)", line)
            if m and m.group(1) == step:
                seen[step].append((int(m.group(2)) - start, "hp", float(m.group(3))))
    return steps, seen, full


def main():
    lyra = lyra_events(pathlib.Path(arg("lyra")).read_text(encoding="utf-8", errors="replace").splitlines())
    srv, seen, full = server_events(pathlib.Path(arg("probe")).read_text(encoding="utf-8", errors="replace").splitlines())
    fails, compared = 0, 0
    key = lambda e: (e[0], {"off": 0, "hp": 1, "on": 2}[e[1]])
    for step in PAD_STEPS:
        a = sorted(lyra.get(step, []), key=key)
        b = sorted(srv.get(step, []), key=key)
        s = [e for e in b if e[1] == "hp"]
        ok = a == b
        okSeen = s == sorted(seen.get(step, []), key=key)
        compared += len(a)
        print(f"{'PASS' if ok else 'FAIL'} {step}: lyra {len(a)} events = server {len(b)} events (server full-health refusals {full.get(step, 0)})")
        for i in range(max(len(a), len(b))):
            x = a[i] if i < len(a) else None
            y = b[i] if i < len(b) else None
            print(f"    {'  ' if x == y else '!!'} lyra={x} server={y}")
        print(f"{'PASS' if okSeen else 'FAIL'} {step}: health the client saw = server hp events ({len(s)})")
        fails += (not ok) + (not okSeen)
    if compared == 0:
        print("FAIL nothing compared")
        fails += 1
    print(f"PADS {'PASS' if fails == 0 else 'FAIL'} compared={compared} fails={fails}")
    sys.exit(0 if fails == 0 else 1)


if __name__ == "__main__":
    main()
