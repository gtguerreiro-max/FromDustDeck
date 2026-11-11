# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_copy_params.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture path, output file, then event ids. Writes the
# API call's parameters (structured data) for each event: for copies, the buffers and regions.
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def dump(obj, depth, out):
    if depth > 6:
        return
    name = obj.name
    if obj.NumChildren():
        out.append("  " * depth + name)
        for i in range(obj.NumChildren()):
            dump(obj.GetChild(i), depth + 1, out)
    else:
        try:
            v = obj.AsString() or str(obj.data.basic.u) or ""
            if obj.type.basetype == 9 or "Buffer" in name or "Image" in name:
                v = "%s id=%d" % (v, int(obj.data.basic.id))
        except Exception:
            v = str(obj.data.basic.u)
        out.append("  " * depth + "%s = %s" % (name, v))


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n") if l.strip()]
    capture, out, eids = lines[0], lines[1], [int(x) for x in lines[2:]]
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    sfile = controller.GetStructuredFile()
    resources = {int(r.resourceId): r.name for r in controller.GetResources()}
    buffers = {int(b.resourceId): b.length for b in controller.GetBuffers()}
    acts = {}

    def walk(items):
        for a in items:
            acts[a.eventId] = a
            walk(a.children)

    walk(controller.GetRootActions())
    report = []
    for e in eids:
        a = acts[e]
        report.append("== %d %s" % (e, a.GetName(sfile)))
        chunk = sfile.chunks[a.events[-1].chunkIndex]
        lines_out = []
        dump(chunk, 0, lines_out)
        report.extend(lines_out[:60])
    report.append("buffers: " + ", ".join("%d:%s:%d" % (k, resources.get(k, ""), v) for k, v in buffers.items() if v >= 1 << 20))
    open(out, "w").write("\n".join(report))
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
import os
os._exit(0)
