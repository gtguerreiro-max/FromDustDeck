#!/usr/bin/env python3
"""Finds From Dust's copies of the guest D3D library functions PGR4Native hooks.

Both games link the same XDK D3D library (the draw functions flush the same device offsets to the
same registers), but at other addresses and in another order. For every PGR4 function in the
library range (and the hooked draw functions outside it), this looks for the From Dust function
whose instruction listing is the same: first exactly (branch targets relative, `lis` data
addresses masked), then with every number masked. Uses tu_address_map.py's listing parser.

  match_d3d.py PGR4_GEN FROMDUST_GEN SURFACE_INC > map.txt
      PGR4_GEN, FROMDUST_GEN: generated/default directories; SURFACE_INC: PGR4Native's
      src/d3d_surface.inc. Prints "pgr4 fromdust how name" per function, "-" when unmatched.
"""
import collections, importlib.util, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "tu", os.path.join(os.environ.get("PGR4_NATIVE", r"F:\PGR4Native"), "scripts", "analysis",
                       "tu_address_map.py"))
tu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tu)

NUM = re.compile(r'-?(?:0x[0-9a-fA-F]+|\d+)')


def keys(listing):
    """(exact, masked) keys of a function's [(address, instruction)] listing."""
    exact, masked = [], []
    for addr, insn in listing:
        def rel(m):
            v = int(m.group(0), 16)
            return "@%+d" % (v - addr) if 0x82000000 <= v < 0x8A800000 else m.group(0)
        e = re.sub(r'0x8[0-9a-f]{7}', rel, insn)
        if insn.startswith("lis "):
            e = NUM.sub("N", e)
        exact.append(e)
        masked.append(NUM.sub("N", insn))
    return "\n".join(exact), "\n".join(masked)


import difflib

FD_RANGES = ((0x820D0000, 0x820F8000), (0x82500000, 0x82520000))


def mnemonics(listing):
    return [insn.split(" ", 1)[0] for _, insn in listing]


def fuzzy(listing, fd):
    """(ratio, From Dust function, next best ratio) by mnemonic sequence similarity."""
    want = mnemonics(listing)
    scored = []
    for f, other in fd.items():
        if not any(lo <= f < hi for lo, hi in FD_RANGES):
            continue
        if not (0.6 * len(want) <= len(other) <= 1.6 * len(want)):
            continue
        sm = difflib.SequenceMatcher(None, want, mnemonics(other), autojunk=False)
        if sm.real_quick_ratio() < 0.6 or sm.quick_ratio() < 0.6:
            continue
        scored.append((sm.ratio(), f))
    scored.sort(reverse=True)
    if not scored or scored[0][0] < 0.6:
        return None
    return scored[0][0], scored[0][1], scored[1][0] if len(scored) > 1 else 0.0


def main():
    pgr4_gen, fd_gen, surface = sys.argv[1:4]
    pgr4 = tu.listings(pgr4_gen)
    fd = tu.listings(fd_gen)
    names = {}
    for m in re.finditer(r'D3D_FN\(([0-9A-F]{8}), "([^"]+)"', open(surface).read()):
        names[int(m.group(1), 16)] = m.group(2)
    # The hooked functions outside the scanned surface (hook_args.h).
    for a, n in ((0x826CFA60, "BeginVertices"), (0x826CFF10, "DrawVerticesUP"),
                 (0x826CFF58, "DrawVertices"), (0x826D0340, "DrawIndexedVertices"),
                 (0x826D0798, "DrawIndx2Inline"), (0x826DC888, "DrawIndx2Split"),
                 (0x826D18E0, "ClearF"), (0x826D1A08, "Clear"), (0x826CBEE0, "Resolve"),
                 (0x826CB148, "Swap"), (0x826D6610, "RunCommandBuffer"),
                 (0x826D2F00, "QueryIssue"), (0x826D3188, "QueryGetData"),
                 (0x826D1C38, "BeginTiling"), (0x826D20D0, "EndTiling"),
                 (0x826DEA78, "MakeSpace")):
        names.setdefault(a, n)
        if names[a] != n:
            names[a] = n + "/" + names[a]
    fd_exact, fd_masked = collections.defaultdict(list), collections.defaultdict(list)
    for f, listing in fd.items():
        e, m = keys(listing)
        fd_exact[e].append(f)
        fd_masked[m].append(f)
    matched = 0
    for a in sorted(names):
        if a not in pgr4:
            print("%08X -        missing-in-pgr4 %s" % (a, names[a]))
            continue
        e, m = keys(pgr4[a])
        if len(fd_exact.get(e, [])) == 1:
            how, f = "exact", fd_exact[e][0]
        elif len(fd_masked.get(m, [])) == 1:
            how, f = "masked", fd_masked[m][0]
        elif fd_exact.get(e) or fd_masked.get(m):
            cands = fd_exact.get(e) or fd_masked.get(m)
            print("%08X ambiguous(%d) %s %s" % (a, len(cands), names[a],
                                               " ".join("%08X" % c for c in cands[:6])))
            continue
        else:
            best = fuzzy(pgr4[a], fd)
            if best:
                ratio, f, second = best
                print("%08X %08X fuzzy%.2f(next %.2f) %s" % (a, f, ratio, second, names[a]))
            else:
                print("%08X -        none %s (%d insns)" % (a, names[a], len(pgr4[a])))
            continue
        matched += 1
        print("%08X %08X %s %s" % (a, f, how, names[a]))
    print("# %d of %d matched" % (matched, len(names)), file=sys.stderr)


main()
