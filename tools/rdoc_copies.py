# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_copies.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture, output dir, draw event.
# Writes copies.txt: every copy action's source and destination resource ids, and the vertex
# shader's read-only resources at the draw, to follow which upload fills the texture a draw reads.
import os
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n")]
    capture, out, draw = lines[0], lines[1], int(lines[2])
    os.makedirs(out, exist_ok=True)
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    sfile = controller.GetStructuredFile()
    report = open(os.path.join(out, "copies.txt"), "w")

    def walk(items):
        for a in items:
            if a.flags & rd.ActionFlags.Copy:
                report.write("%d %s src %d dst %d\n" % (a.eventId, a.GetName(sfile),
                                                        int(a.copySource), int(a.copyDestination)))
            walk(a.children)

    walk(controller.GetRootActions())
    controller.SetFrameEvent(draw, True)
    state = controller.GetPipelineState()
    for u in state.GetReadOnlyResources(rd.ShaderStage.Vertex, True):
        report.write("draw %d vs ro %d\n" % (draw, int(u.descriptor.resource)))
    report.close()
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
os._exit(0)
