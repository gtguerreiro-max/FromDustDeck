"""Names the samples of tools/sample_thread.ps1 with lld-link /MAP files and sums them per function.

    python -I tools/symbolize_samples.py build/FromDust/samples.txt <dir with .map files>...
"""
import bisect
import collections
import glob
import os
import re
import sys

maps = {}
for d in sys.argv[2:]:
    for path in glob.glob(os.path.join(d, "*.map")):
        maps[os.path.splitext(os.path.basename(path))[0].lower()] = path
cache = {}


def table(module):
    if module not in cache:
        path = maps.get(module)
        syms = []
        base = 0x180000000
        if path:
            text = open(path, encoding="utf-8", errors="replace").read()
            m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", text)
            if m:
                base = int(m.group(1), 16)
            for m in re.finditer(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})", text, re.M):
                syms.append((int(m.group(2), 16) - base, m.group(1)))
        syms.sort()
        cache[module] = ([a for a, _ in syms], syms)
    return cache[module]


totals = collections.Counter()
all_samples = 0
for line in open(sys.argv[1], encoding="utf-8-sig"):
    parts = line.split()
    if len(parts) != 3:
        continue
    count, module, off = int(parts[0]), parts[1], int(parts[2], 16)
    all_samples += count
    addrs, syms = table(os.path.splitext(module)[0].lower())
    i = bisect.bisect_right(addrs, off) - 1
    name = syms[i][1] if i >= 0 else "?"
    totals["%s!%s" % (module, name)] += count
for name, count in totals.most_common(40):
    print("%5.1f%%  %s" % (100.0 * count / all_samples, name))
