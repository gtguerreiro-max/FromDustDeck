# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_actions.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture path, output dir, first and last event id.
# Lists every action (draws, dispatches, copies, clears, markers) in that range with its
# outputs, to compare the order of work (e.g. resolves) between two captures.
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
    report = open(os.path.join(out, "actions.txt"), "w")

    def walk(items, depth):
        for a in items:
            if first <= a.eventId <= last:
                outs = []
                for o in list(a.outputs) + [a.depthOut]:
                    if o != rd.ResourceId.Null():
                        t = textures.get(o)
                        outs.append("%dx%d:%s" % (t.width, t.height, t.format.Name()) if t else str(int(o)))
                report.write("%s%d %s %s\n" % ("  " * depth, a.eventId, a.GetName(sfile), " ".join(outs)))
            walk(a.children, depth + 1)

    walk(controller.GetRootActions(), 0)
    report.close()
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
os._exit(0)
