# Checks a walkable-floor dump (MOBNavDumpSubsystem): are the floors connected, and does the dumped graph give the same answers as Unreal's own path search?
#   check_nav.py <nav.monv> [--png=out.png] [--spawns=Shared/MatchRules.cpp] [--break=cut|join]
# --break corrupts the graph on purpose; the comparison must then report mismatches.
# Exit code 0 = graph and Unreal agree on every asked pair.
import collections
import csv
import re
import statistics
import struct
import sys

SUCCESS = 3  # ENavigationQueryResult::Success
STACKED_MIN_DZ = 250.0


def load(path):
    data = open(path, 'rb').read()
    if data[:4] != b'MONV':
        sys.exit('NAV_CHECK bad magic')
    version, node_count, link_count = struct.unpack_from('<III', data, 4)
    pos = 16 + 24
    radius, height = struct.unpack_from('<ff', data, pos)
    pos += 8
    nodes = []
    for _ in range(node_count):
        kind, area = struct.unpack_from('<II', data, pos)
        pos += 8
        center = struct.unpack_from('<fff', data, pos)
        pos += 12
        (vert_count,) = struct.unpack_from('<I', data, pos)
        pos += 4
        verts = [struct.unpack_from('<fff', data, pos + i * 12) for i in range(vert_count)]
        pos += vert_count * 12
        (neighbor_count,) = struct.unpack_from('<I', data, pos)
        pos += 4
        neighbors = list(struct.unpack_from('<%dI' % neighbor_count, data, pos))
        pos += neighbor_count * 4
        nodes.append({'kind': kind, 'area': area, 'center': center, 'verts': verts, 'next': neighbors})
    if pos != len(data):
        sys.exit('NAV_CHECK file size mismatch: read %d of %d' % (pos, len(data)))
    seen = sum(len(n['next']) for n in nodes)
    if seen != link_count or any(i >= node_count for n in nodes for i in n['next']):
        sys.exit('NAV_CHECK link table broken: %d links, header %d' % (seen, link_count))
    print('NAV_CHECK loaded version=%d nodes=%d links=%d agentRadius=%.1f agentHeight=%.1f' % (version, node_count, link_count, radius, height))
    return nodes


def components(nodes):
    parent = list(range(len(nodes)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for i, node in enumerate(nodes):
        for j in node['next']:
            parent[find(i)] = find(j)
    groups = collections.defaultdict(list)
    for i in range(len(nodes)):
        groups[find(i)].append(i)
    return sorted(groups.values(), key=len, reverse=True)


def reachable(nodes, start):
    seen = {start}
    queue = collections.deque([start])
    while queue:
        for j in nodes[queue.popleft()]['next']:
            if j not in seen:
                seen.add(j)
                queue.append(j)
    return seen


def inside_xy(point, verts):
    x, y = point[0], point[1]
    hit = False
    for i in range(len(verts)):
        ax, ay = verts[i][0], verts[i][1]
        bx, by = verts[i - 1][0], verts[i - 1][1]
        if (ay > y) != (by > y) and x < (bx - ax) * (y - ay) / (by - ay) + ax:
            hit = not hit
    return hit


def corrupt(nodes, mode, groups):
    if mode == 'cut':
        # cut the largest group in two by height: floors that were connected no longer are
        members = groups[0]
        middle = statistics.median(nodes[i]['center'][2] for i in members)
        cut = 0
        for i in members:
            keep = [j for j in nodes[i]['next'] if (nodes[i]['center'][2] > middle) == (nodes[j]['center'][2] > middle)]
            cut += len(nodes[i]['next']) - len(keep)
            nodes[i]['next'] = keep
        print('NAV_CHECK break=cut removed=%d' % cut)
    elif mode == 'join':
        # join every group: floors that had no route now seem to have one
        for a, b in zip(groups, groups[1:]):
            nodes[a[0]]['next'].append(b[0])
            nodes[b[0]]['next'].append(a[0])
        print('NAV_CHECK break=join added=%d' % (2 * (len(groups) - 1)))


def compare(nodes, pairs):
    table = collections.Counter()
    cache = {}
    for row in pairs:
        a, b = int(row['a']), int(row['b'])
        if a not in cache:
            cache[a] = reachable(nodes, a)
        table[(row['kind'], b in cache[a], int(row['result']) == SUCCESS)] += 1
    mismatch = 0
    for kind in sorted({k[0] for k in table}):
        yes_yes, yes_no = table[(kind, True, True)], table[(kind, True, False)]
        no_yes, no_no = table[(kind, False, True)], table[(kind, False, False)]
        mismatch += yes_no + no_yes
        print('NAV_CHECK pairs kind=%s bothYes=%d bothNo=%d graphYesUnrealNo=%d graphNoUnrealYes=%d' % (kind, yes_yes, no_no, yes_no, no_yes))
    return mismatch


def report_floors(nodes, groups):
    ground = [i for i, n in enumerate(nodes) if n['kind'] == 0]
    print('NAV_CHECK groups=%d sizes=%s' % (len(groups), [len(g) for g in groups[:10]]))
    for rank, members in enumerate(groups[:4]):
        zs = [nodes[i]['center'][2] for i in members]
        print('NAV_CHECK group rank=%d nodes=%d zMin=%.0f zMax=%.0f' % (rank, len(members), min(zs), max(zs)))
    main = set(groups[0])
    same, other = 0, 0
    examples = []
    for i in ground:
        for j in ground:
            if nodes[j]['center'][2] - nodes[i]['center'][2] < STACKED_MIN_DZ:
                continue
            if inside_xy(nodes[i]['center'], nodes[j]['verts']) or inside_xy(nodes[j]['center'], nodes[i]['verts']):
                if i in main and j in main:
                    same += 1
                    examples.append((i, j))
                else:
                    other += 1
    print('NAV_CHECK overlappedFloors sameGroup=%d otherGroup=%d' % (same, other))
    return examples


def report_route(nodes, pairs, examples):
    if not examples:
        return
    # shortest hop route between the most separated overlapped floors: does it climb step by step?
    a, b = max(examples, key=lambda p: nodes[p[1]]['center'][2] - nodes[p[0]]['center'][2])
    previous = {a: None}
    queue = collections.deque([a])
    while queue and b not in previous:
        i = queue.popleft()
        for j in nodes[i]['next']:
            if j not in previous:
                previous[j] = i
                queue.append(j)
    route = []
    at = b if b in previous else None
    while at is not None:
        route.append(at)
        at = previous[at]
    route.reverse()
    print('NAV_CHECK route from=%d to=%d hops=%d offMeshHops=%d z=%s' % (a, b, len(route) - 1, sum(nodes[i]['kind'] for i in route), [round(nodes[i]['center'][2]) for i in route]))


def report_walk(nodes, spawns_path):
    # The server has no jump pads yet, so a bot can only use plain floor links. Repeat the floor report without the off-mesh links.
    walk = [dict(n, next=[] if n['kind'] else [j for j in n['next'] if nodes[j]['kind'] == 0]) for n in nodes]
    groups = [g for g in components(walk) if walk[g[0]]['kind'] == 0]
    main = set(groups[0])
    ground = [i for i, n in enumerate(walk) if n['kind'] == 0]
    zs = [walk[i]['center'][2] for i in groups[0]]
    print('NAV_CHECK walk groups=%d mainNodes=%d of %d mainZ=%.0f..%.0f' % (len(groups), len(groups[0]), len(ground), min(zs), max(zs)))
    same = sum(1 for i in main for j in main if walk[j]['center'][2] - walk[i]['center'][2] >= STACKED_MIN_DZ
               and (inside_xy(walk[i]['center'], walk[j]['verts']) or inside_xy(walk[j]['center'], walk[i]['verts'])))
    print('NAV_CHECK walk overlappedFloorsInMain=%d' % same)
    if not spawns_path:
        return
    points = [tuple(float(v) for v in p) for p in re.findall(r'Vec3\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)', open(spawns_path, encoding='utf-8-sig').read())]
    in_main, other, off = 0, 0, []
    for p in points:
        under = [i for i in ground if inside_xy(p, walk[i]['verts']) and abs(walk[i]['center'][2] - p[2]) <= 200]
        if not under:
            off.append(p)
        elif min(under, key=lambda i: abs(walk[i]['center'][2] - p[2])) in main:
            in_main += 1
        else:
            other += 1
    print('NAV_CHECK spawns total=%d inWalkMain=%d otherGroup=%d notOnFloor=%d %s' % (len(points), in_main, other, len(off), off[:4]))


def report_floor_distance(nodes, path):
    try:
        rows = list(csv.DictReader(open(path, encoding='utf-8')))
    except FileNotFoundError:
        print('NAV_CHECK floor file missing')
        return
    far, near = [], []
    for row in rows:
        node = nodes[int(row['node'])]
        spread = max(v[2] for v in node['verts']) - min(v[2] for v in node['verts'])
        distance = float(row['distance'])
        (far if distance < 0 or distance > 35 else near).append(spread)
    print('NAV_CHECK floorDistance near=%d nearFlat=%d far=%d farFlat=%d (flat = polygon height spread under 20)' % (len(near), sum(s < 20 for s in near), len(far), sum(s < 20 for s in far)))


def draw(nodes, groups, out):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection
    plt.rcParams['font.family'] = 'Malgun Gothic'
    plt.rcParams['axes.unicode_minus'] = False  # this font has no minus glyph
    group_of = {i: rank for rank, members in enumerate(groups) for i in members}
    ground = [i for i, n in enumerate(nodes) if n['kind'] == 0]
    polys = [[(v[0], v[1]) for v in nodes[i]['verts']] for i in ground]
    figure, axes = plt.subplots(2, 1, figsize=(14, 10))
    high = PolyCollection(polys, array=[nodes[i]['center'][2] for i in ground], cmap='viridis', edgecolors='k', linewidths=0.2)
    axes[0].add_collection(high)
    figure.colorbar(high, ax=axes[0], label='높이(cm)')
    axes[0].set_title('걸을 수 있는 바닥 — 색 = 높이')
    colors = ['#2a9d8f' if group_of[i] == 0 else '#e76f51' for i in ground]
    axes[1].add_collection(PolyCollection(polys, facecolors=colors, edgecolors='k', linewidths=0.2))
    axes[1].set_title('이어진 덩어리 — 초록 = 가장 큰 덩어리(%d개) · 주황 = 떨어진 덩어리' % len(groups[0]))
    for axis in axes:
        axis.autoscale()
        axis.set_aspect('equal')
    figure.tight_layout()
    figure.savefig(out, dpi=130)
    print('NAV_CHECK png=%s' % out)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    options = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--'))
    nodes = load(args[0])
    pairs = list(csv.DictReader(open(args[0] + '.pairs.csv', encoding='utf-8')))
    groups = components(nodes)
    if 'break' in options:
        corrupt(nodes, options['break'], groups)
        groups = components(nodes)
    examples = report_floors(nodes, groups)
    report_route(nodes, pairs, examples)
    report_walk(nodes, options.get('spawns'))
    report_floor_distance(nodes, args[0] + '.floor.csv')
    mismatch = compare(nodes, pairs)
    print('NAV_CHECK mismatch=%d asked=%d %s' % (mismatch, len(pairs), 'PASS' if mismatch == 0 and pairs else 'FAIL'))
    if 'png' in options:
        draw(nodes, groups, options['png'])
    return 0 if mismatch == 0 and pairs else 1


if __name__ == '__main__':
    sys.exit(main())
