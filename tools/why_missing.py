"""Explains why a guest address the runtime called is not a recompiled function: the words
around it in the loaded image, which images hold a pointer to it, and its old bounds.

    python -I tools/why_missing.py 0x8811A818 [0x...]
"""
import pathlib
import re
import sys

DUMP = pathlib.Path("F:/FromDustRecomp/dump")
CONFIG = pathlib.Path("F:/FromDustRecomp/config")
BASES = {"Default": 0x82000000, "GalileoKernel": 0x88100000, "GalileoTexture": 0x88300000,
         "GalileoPathfind_9_9": 0x88480000, "GalileoSimulatorWater": 0x88500000,
         "GalileoSimulatorWater_9_9": 0x89180000, "dustai.x360.f": 0x8A000000}

images = {s: (DUMP / f"{s}.bin").read_bytes() for s in BASES}
for arg in sys.argv[1:]:
    t = int(arg, 16)
    owner = next((s for s, b in BASES.items() if b <= t < b + len(images[s])), None)
    print(f"0x{t:08X} in {owner}")
    if owner:
        img, b = images[owner], BASES[owner]
        for a in range(t - 12, t + 12, 4):
            print(f"  0x{a:08X} {img[a - b:a - b + 4].hex()}{'  <' if a == t else ''}")
        m = re.search(rf"address = 0x{t:08X}, size = (0x[0-9A-F]+)",
                      (CONFIG / f"{owner}.toml").read_text(encoding="utf-8-sig"), re.I)
        print("  old bounds size:", m.group(1) if m else "none")
    for s, img in images.items():
        n = img.count(t.to_bytes(4, "big"))
        if n:
            print(f"  pointed to {n}x from {s}")
