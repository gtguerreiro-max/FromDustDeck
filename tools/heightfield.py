"""Shows a dumped height field (512x512 k_16_16: two big-endian 16-bit values per texel, the
terrain and water heights as the D3D12 port read them): statistics per channel and a PNG of
each, to tell real heights from zeros or garbage.

    python -I tools/heightfield.py build/FromDust/captures/mem-14F7B000-105s.bin [...]
"""
import struct
import sys

from PIL import Image

for path in sys.argv[1:]:
    data = open(path, "rb").read()
    n = 512 * 512
    values = struct.unpack(">%dH" % (2 * n), data[: 4 * n])
    for ch in (0, 1):
        v = values[ch::2]
        nonzero = sum(1 for x in v if x)
        print(f"{path} channel {ch}: min {min(v)} max {max(v)} mean {sum(v) / n:.0f} "
              f"nonzero {nonzero / n:.1%} distinct {len(set(v))}")
        lo, hi = min(v), max(v)
        scale = 255 / (hi - lo) if hi > lo else 0
        img = Image.new("L", (512, 512))
        img.putdata([int((x - lo) * scale) for x in v])
        img.save(f"{path[:-4]}-ch{ch}.png")
