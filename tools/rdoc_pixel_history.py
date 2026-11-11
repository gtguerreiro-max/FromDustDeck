# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_pixel_history.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture path, output file, resource id, x, y,
# last event. Writes the pixel history of that texel up to the event: every draw that touched
# it, with its tests (depth, stencil, discard...) and the value before and after.
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n") if l.strip()]
    capture, out, rid, x, y, last = lines[0], lines[1], int(lines[2]), int(lines[3]), int(lines[4]), int(lines[5])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    tex = next(t.resourceId for t in controller.GetTextures() if int(t.resourceId) == rid)
    controller.SetFrameEvent(last, True)
    history = controller.PixelHistory(tex, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
    report = open(out, "w")
    flags = ["sampleMasked", "backfaceCulled", "depthClipped", "depthBoundsFailed", "viewClipped",
             "scissorClipped", "shaderDiscarded", "depthTestFailed", "stencilTestFailed",
             "predicationSkipped", "unboundPS"]
    for m in history:
        failed = [f for f in flags if getattr(m, f, False)]
        pre = tuple(m.preMod.col.floatValue[:4])
        post = tuple(m.postMod.col.floatValue[:4])
        report.write("%d prim %d pass=%s %s pre=(%.3g %.3g %.3g %.3g) post=(%.3g %.3g %.3g %.3g) depth %.6g->%.6g\n" % (
            m.eventId, m.primitiveID, m.Passed(), ",".join(failed), *pre, *post,
            m.preMod.depth, m.postMod.depth))
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
