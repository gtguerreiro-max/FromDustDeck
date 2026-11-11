# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_vs_inputs.py
# Reads F:/FromDustDeck/build/rdoc-job.txt (capture path, output dir) and finds the draws whose
# VERTEX shader reads a texture (vertex texture fetch: in From Dust the terrain and water,
# displaced by the height maps). For the first few it writes vs-inputs.txt (event, texture
# sizes and formats) and saves each such texture as raw DDS (vs_<event>_<slot>.dds), to compare
# what the terrain actually samples between two captures.
import os
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = open(JOB).read().split("\n")
    capture, out = lines[0].strip(), lines[1].strip()
    os.makedirs(out, exist_ok=True)
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    textures = {t.resourceId: t for t in controller.GetTextures()}
    sfile = controller.GetStructuredFile()
    actions = []

    def walk(items):
        for a in items:
            if a.flags & rd.ActionFlags.Drawcall:
                actions.append(a)
            walk(a.children)

    walk(controller.GetRootActions())
    report = open(os.path.join(out, "vs-inputs.txt"), "w")
    found = 0
    for a in actions:
        controller.SetFrameEvent(a.eventId, True)
        state = controller.GetPipelineState()
        try:
            used = state.GetReadOnlyResources(rd.ShaderStage.Vertex, True)
        except Exception:
            continue
        texs = [u for u in used if u.descriptor.resource in textures and
                textures[u.descriptor.resource].width >= 256]
        if not texs:
            continue
        report.write("=== event %d %s, %d indices\n" % (a.eventId, a.GetName(sfile), a.numIndices))
        for k, u in enumerate(texs[:4]):
            t = textures[u.descriptor.resource]
            report.write("  vs texture %d: %dx%d %s\n" % (k, t.width, t.height, t.format.Name()))
            if found < 6:
                save = rd.TextureSave()
                save.resourceId = u.descriptor.resource
                save.destType = rd.FileType.DDS
                controller.SaveTexture(save, os.path.join(out, "vs_%05d_%d.dds" % (a.eventId, k)))
        found += 1
        if found >= 30:
            break
    report.write("draws with vertex textures (first 30 shown): %d\n" % found)
    report.close()
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
os._exit(0)
