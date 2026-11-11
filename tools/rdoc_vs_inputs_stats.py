# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_vs_inputs_stats.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture path, output file, then either an event id
# or "match <numIndices>" (the first indexed draw with that many indices). Writes the draw's
# vertex shader textures with per-channel min / max / mean of their texels (read back as
# floats), and the first post-VS positions, to compare one draw between two captures.
import struct
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n") if l.strip()]
    capture, out, sel = lines[0], lines[1], lines[2].split()
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    report = open(out, "w")
    textures = {t.resourceId: t for t in controller.GetTextures()}

    draws = []

    def walk(items):
        for a in items:
            if a.flags & rd.ActionFlags.Drawcall:
                draws.append(a)
            walk(a.children)

    walk(controller.GetRootActions())
    if sel[0] == "match":
        want = [int(x) for x in sel[1:]]
        targets = [a for a in draws if a.numIndices in want]
    else:
        targets = [a for a in draws if a.eventId == int(sel[0])]
    for a in targets[:6]:
        controller.SetFrameEvent(a.eventId, True)
        state = controller.GetPipelineState()
        report.write("== event %d indices %d instances %d\n" % (a.eventId, a.numIndices, a.numInstances))
        for stage, name in ((rd.ShaderStage.Vertex, "vs"), (rd.ShaderStage.Pixel, "ps")):
            for used in state.GetReadOnlyResources(stage, True):
                rid = used.descriptor.resource
                t = textures.get(rid)
                if not t:
                    continue
                line = "%s %d %dx%d %s" % (name, int(rid), t.width, t.height, t.format.Name())
                if name == "vs" or t.format.Name().startswith("R16"):
                    try:
                        data = controller.GetTextureData(rid, rd.Subresource(0, 0, 0))
                        comp = t.format.compCount
                        size = t.format.compByteWidth
                        fmt = {2: "h", 1: "b", 4: "f"}[size]
                        typ = t.format.compType
                        n = len(data) // size
                        vals = struct.unpack("<%d%s" % (n, fmt), data[: n * size])
                        if size == 2 and typ == rd.CompType.UNorm:
                            vals = [v & 0xFFFF for v in vals]
                        stats = []
                        for c in range(comp):
                            ch = vals[c::comp]
                            stats.append("c%d min %s max %s mean %.1f" % (c, min(ch), max(ch), sum(ch) / len(ch)))
                        line += " | " + "; ".join(stats)
                    except Exception as e:
                        line += " | %s" % e
                report.write(line + "\n")
        try:
            mesh = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            data = controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, mesh.vertexByteStride * 8)
            for v in range(8):
                x, y, z, w = struct.unpack_from("<4f", data, v * mesh.vertexByteStride)
                report.write("pos %d: %.3f %.3f %.3f %.3f\n" % (v, x, y, z, w))
        except Exception as e:
            report.write("postvs: %s\n" % e)
    report.close()
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
import os
os._exit(0)
