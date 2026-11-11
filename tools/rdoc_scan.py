# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_scan.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture path, output dir, first and last event id.
# For every draw / dispatch / copy in that range, writes the value of the middle pixel (and
# one at a quarter) of each color output after the event (scan.txt), to find the first pass
# whose output goes wrong without stepping through the frame by hand.
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
    report = open(os.path.join(out, "scan.txt"), "w")
    actions = []

    def walk(items):
        for a in items:
            if first <= a.eventId <= last and not a.children:
                outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
                if outs:
                    actions.append((a, outs))
            walk(a.children)

    walk(controller.GetRootActions())
    for a, outs in actions:
        controller.SetFrameEvent(a.eventId, True)
        parts = []
        for o in outs:
            t = textures.get(o)
            if not t:
                continue
            vals = []
            for fx, fy in ((0.5, 0.5), (0.25, 0.6)):
                v = controller.PickPixel(o, int(t.width * fx), int(t.height * fy),
                                         rd.Subresource(0, 0, 0), rd.CompType.Typeless)
                vals.append("(%.3g %.3g %.3g %.3g)" % tuple(v.floatValue[:4]))
            parts.append("%d %dx%d:%s %s" % (int(o), t.width, t.height, t.format.Name(), " ".join(vals)))
        report.write("%d %s | %s\n" % (a.eventId, a.GetName(sfile)[:40], " || ".join(parts)))
    report.close()
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
os._exit(0)
