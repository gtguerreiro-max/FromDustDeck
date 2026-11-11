#!/usr/bin/env python3
"""Rewrites PGR4 guest addresses in the native renderer sources copied from PGR4Native into
From Dust's, from match_d3d.py's map (plus the matches checked by hand below).

  retarget_pgr4.py MAP SURFACE_INC FILE...
      SURFACE_INC (src/d3d_surface.inc) keeps only the functions that have a From Dust copy.
      In the other FILEs every mapped address is replaced (sub_/fn_/kFn_/0x forms and bare), and
      the unmapped ones still used outside comments are listed.
"""
import re, sys

MANUAL = {
    0x826CFF00: 0x8250D470,  # RestorePush: 3 instructions, identical, right before BeginVertices
    0x826D0798: 0x820EEAD8,  # DrawIndx2Inline: the internal draw (FromDustRecomp NATIVE_RENDERER)
    0x826DEDC8: 0x820EB9F0,  # draw flush (DrawIndexedVertices calls it in both)
    0x826CD380: 0x820D65A8,  # RestoreSegment from the saved cursor (13392 -> 13680)
    0x826CFF58: 0x8250D988,  # DrawVertices (0.80 against 0.76; FromDustRecomp's call counts)
}


def load_map(path):
    m = {}
    for line in open(path):
        g = re.match(r"([0-9A-F]{8}) ([0-9A-F]{8}) (exact|masked|fuzzy[\d.]+\(next [\d.]+\))", line)
        if not g:
            continue
        p = [g.group(1), g.group(2)]
        how = g.group(3)
        ok = how in ("exact", "masked")
        f = re.match(r"fuzzy([\d.]+)\(next ([\d.]+)\)", how)
        if f:
            r, nxt = float(f.group(1)), float(f.group(2))
            ok = r >= 0.70 and r - nxt >= 0.10
        if ok:
            m[int(p[0], 16)] = int(p[1], 16)
    m.update(MANUAL)
    return m


def main():
    m = load_map(sys.argv[1])
    surface = sys.argv[2]
    out = []
    kept = dropped = 0
    for line in open(surface):
        g = re.match(r"D3D_FN\(([0-9A-F]{8}),", line)
        if g:
            a = int(g.group(1), 16)
            if a not in m:
                dropped += 1
                continue
            line = line.replace(g.group(1), "%08X" % m[a], 1)
            kept += 1
        out.append(line)
    open(surface, "w", newline="\n").write("".join(out))
    print("surface: %d kept, %d dropped" % (kept, dropped))
    token = re.compile(r"(?<![0-9A-Fa-f])(826[0-9A-F]{5}|8285[0-9A-F]{4})(?![0-9A-Fa-f])")
    for path in sys.argv[3:]:
        text = open(path, encoding="utf-8").read()
        missing = set()

        def sub(mo):
            a = int(mo.group(1), 16)
            if a in m:
                return "%08X" % m[a]
            missing.add(mo.group(1))
            return mo.group(1)

        new = token.sub(sub, text)
        if new != text:
            open(path, "w", encoding="utf-8", newline="\n").write(new)
        code_missing = sorted(a for a in missing
                              if any(a in l.split("//")[0] for l in new.splitlines()))
        if code_missing:
            print("%s: unmapped in code: %s" % (path, " ".join(code_missing)))


main()
