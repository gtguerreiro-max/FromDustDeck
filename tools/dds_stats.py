"""Statistics of the 16-bit channels of DDS files saved by RenderDoc (R16G16 / R16G16B16A16
textures and targets): per channel, as signed and as unsigned 16-bit, over the whole image or
the top-left WxH region. For comparing the same pass between two captures.

    python -I tools/dds_stats.py <file.dds> [W H]
"""
import struct
import sys

path = sys.argv[1]
data = open(path, "rb").read()
assert data[:4] == b"DDS ", "not a DDS file"
height, width = struct.unpack_from("<II", data, 12)
fourcc = data[84:88]
offset = 128
dxgi = None
if fourcc == b"DX10":
    dxgi = struct.unpack_from("<I", data, 128)[0]
    offset += 20
channels = 4 if dxgi in (10, 11, 12, 13, 14, 9) else 2  # R16G16B16A16 formats, else R16G16
region_w = int(sys.argv[2]) if len(sys.argv) > 2 else width
region_h = int(sys.argv[3]) if len(sys.argv) > 3 else height
print(f"{path}: {width}x{height} dxgi {dxgi}, region {region_w}x{region_h}")
row = width * channels * 2
for signed in (True, False):
    fmt = "<%d%s" % (region_w * channels, "h" if signed else "H")
    values = [[] for _ in range(channels)]
    for y in range(region_h):
        r = struct.unpack_from(fmt, data, offset + y * row)
        for c in range(channels):
            values[c].extend(r[c::channels])
    for c in range(min(channels, 2)):
        v = values[c]
        print(f"  {'s16' if signed else 'u16'} ch{c}: min {min(v)} max {max(v)} "
              f"mean {sum(v) / len(v):.0f} distinct {len(set(v))}")
