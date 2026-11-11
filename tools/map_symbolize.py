"""Resolves module+offset frames (crash.txt) to symbols with an lld-link /MAP file.

    python -I tools/map_symbolize.py build/game/fromdust.map 0x1e28aa2 0xe8093b ...
"""
import bisect
import re
import sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
base = int(re.search(r"Preferred load address is ([0-9a-fA-F]+)", text).group(1), 16) \
    if "Preferred load address" in text else 0x140000000
syms = []
# lld-link /MAP writes MSVC's format: " 0001:00002490  name  0000000180003490  obj".
for m in re.finditer(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})", text, re.M):
    syms.append((int(m.group(2), 16), m.group(1)))
syms.sort()
addrs = [a for a, _ in syms]
for arg in sys.argv[2:]:
    off = int(arg, 16)
    for va in (base + off, off):
        i = bisect.bisect_right(addrs, va) - 1
        if i >= 0 and va - addrs[i] < 0x100000:
            print(f"{arg}: {syms[i][1]}+0x{va - addrs[i]:x}")
            break
    else:
        print(f"{arg}: ?")
