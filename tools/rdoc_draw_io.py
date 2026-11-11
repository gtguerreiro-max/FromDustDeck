# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_draw_io.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture, output dir, draw event.
# Saves the draw's pixel shader input textures (ps_<slot>.dds), its color outputs after the draw
# (out_<n>.dds), its pixel shader disassembly and constants, to compare one pass between captures.
import os
import struct
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n")]
    capture, out, event = lines[0], lines[1], int(lines[2])
    os.makedirs(out, exist_ok=True)
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    textures = {t.resourceId: t for t in controller.GetTextures()}
    report = open(os.path.join(out, "draw.txt"), "w")
    controller.SetFrameEvent(event, True)
    state = controller.GetPipelineState()

    def save(rid, name):
        s = rd.TextureSave()
        s.resourceId = rid
        s.destType = rd.FileType.DDS
        controller.SaveTexture(s, os.path.join(out, name))

    for k, u in enumerate(state.GetReadOnlyResources(rd.ShaderStage.Pixel, True)):
        rid = u.descriptor.resource
        t = textures.get(rid)
        report.write("ps ro %d: %d %s fmt %s\n" % (k, int(rid),
                     "%dx%d %s" % (t.width, t.height, t.format.Name()) if t else "buffer",
                     u.descriptor.format.Name()))
        if t:
            save(rid, "ps_%d.dds" % k)
    for k, o in enumerate(state.GetOutputTargets()):
        rid = o.resource
        if rid == rd.ResourceId.Null():
            continue
        t = textures.get(rid)
        report.write("out %d: %d %dx%d %s view %s\n" % (k, int(rid), t.width, t.height,
                     t.format.Name(), o.format.Name()))
        save(rid, "out_%d.dds" % k)
    ps = state.GetShaderReflection(rd.ShaderStage.Pixel)
    if ps is not None:
        for i, cb in enumerate(ps.constantBlocks):
            try:
                bind = state.GetConstantBlock(rd.ShaderStage.Pixel, i, 0).descriptor
                data = controller.GetBufferData(bind.resource, bind.byteOffset, min(bind.byteSize or 256, 512))
                words = struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4])
                report.write("ps cb %d %s: %s\n" % (i, cb.name, " ".join("%08X" % w for w in words[:100])))
            except Exception as e:
                report.write("ps cb %d failed: %s\n" % (i, e))
        targets = controller.GetDisassemblyTargets(True)
        open(os.path.join(out, "ps.txt"), "w").write(
            controller.DisassembleShader(state.GetGraphicsPipelineObject(), ps, targets[0]))
    report.close()
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
os._exit(0)
