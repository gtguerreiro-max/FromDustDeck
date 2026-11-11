# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_ps_textures.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture, output dir, first event, last event.
# For each indexed draw in the range that writes a color target, lists its pixel shader textures
# (size, mips, format) in ps-textures.txt and saves each distinct texture once as DDS
# (tex_<resource id>.dds), to look for textures whose data came out garbled.
import os
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n")]
    capture, out, first, last = lines[0], lines[1], int(lines[2]), int(lines[3])
    os.makedirs(out, exist_ok=True)
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    textures = {t.resourceId: t for t in controller.GetTextures()}
    sfile = controller.GetStructuredFile()
    draws = []

    def walk(items):
        for a in items:
            if (a.flags & rd.ActionFlags.Drawcall) and first <= a.eventId <= last and \
                    any(o != rd.ResourceId.Null() for o in a.outputs):
                draws.append(a)
            walk(a.children)

    walk(controller.GetRootActions())
    report = open(os.path.join(out, "ps-textures.txt"), "w")
    saved = set()
    for a in draws:
        controller.SetFrameEvent(a.eventId, True)
        state = controller.GetPipelineState()
        items = []
        for u in state.GetReadOnlyResources(rd.ShaderStage.Pixel, True):
            t = textures.get(u.descriptor.resource)
            if not t:
                continue
            rid = int(t.resourceId)
            items.append("%d:%dx%d/%dmips/%s" % (rid, t.width, t.height, t.mips, u.descriptor.format.Name()))
            if rid not in saved and len(saved) < 40 and t.width >= 64:
                saved.add(rid)
                s = rd.TextureSave()
                s.resourceId = t.resourceId
                s.destType = rd.FileType.DDS
                s.mip = -1
                controller.SaveTexture(s, os.path.join(out, "tex_%d.dds" % rid))
        report.write("%d %s %d idx: %s\n" % (a.eventId, a.GetName(sfile), a.numIndices, " ".join(items)))
    report.close()
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
os._exit(0)
