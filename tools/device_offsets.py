#!/usr/bin/env python3
"""Maps PGR4's guest D3D device field offsets to From Dust's.

The two games link versions of the XDK D3D library whose device keeps the register shadow at the
same offsets, but other fields (index buffer, shaders, surfaces, the tile list...) moved. For
every matched function pair (match_d3d.py's output), the two instruction listings are aligned
by mnemonic, and each aligned load or store with a displacement (`lwz r11,12688(r31)`) votes for
"PGR4 offset -> From Dust offset". Offsets that are equal everywhere are left out.

  device_offsets.py PGR4_GEN FROMDUST_GEN MAP [offset...]
      MAP: match_d3d.py output. With offsets, prints only those (PGR4 values).
"""
import collections, difflib, importlib.util, os, re, sys

spec = importlib.util.spec_from_file_location(
    "tu", os.path.join(os.environ.get("PGR4_NATIVE", r"F:\PGR4Native"), "scripts", "analysis",
                       "tu_address_map.py"))
tu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tu)

DFORM = re.compile(r'^(l[bhwd]z?u?x?|lhz|lha|lbz|lwz|lwzu|ld|st[bhwd]u?|stw|stwu|sth|stb|std|lfs|lfd|stfs|stfd)\s+([^,]+),(-?\d+)\((r\d+)\)$')


def main():
    pgr4_gen, fd_gen, mapfile = sys.argv[1:4]
    want = {int(x) for x in sys.argv[4:]}
    pgr4 = tu.listings(pgr4_gen)
    fd = tu.listings(fd_gen)
    pairs = []
    for line in open(mapfile):
        parts = line.split()
        if len(parts) >= 3 and re.fullmatch(r'[0-9A-F]{8}', parts[1]):
            pairs.append((int(parts[0], 16), int(parts[1], 16)))
    votes = collections.defaultdict(collections.Counter)
    for a, b in pairs:
        if a not in pgr4 or b not in fd:
            continue
        la, lb = pgr4[a], fd[b]
        ma = [i.split(" ", 1)[0] for _, i in la]
        mb = [i.split(" ", 1)[0] for _, i in lb]
        sm = difflib.SequenceMatcher(None, ma, mb, autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag != "equal":
                continue
            for k in range(i2 - i1):
                xa = DFORM.match(la[i1 + k][1])
                xb = DFORM.match(lb[j1 + k][1])
                if not xa or not xb or xa.group(1) != xb.group(1):
                    continue
                oa, ob = int(xa.group(3)), int(xb.group(3))
                if oa < 1024 and ob < 1024:
                    continue  # stack and small object fields
                votes[oa][ob] += 1
    for oa in sorted(votes):
        if want and oa not in want:
            continue
        c = votes[oa]
        (ob, n), = c.most_common(1)
        if ob == oa and len(c) == 1 and not want:
            continue
        print("%6d -> %6d  (%d votes; %s)" % (oa, ob, n, ", ".join("%d:%d" % kv for kv in c.most_common(4))))


main()
