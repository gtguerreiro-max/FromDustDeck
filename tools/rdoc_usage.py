# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_usage.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture path, output file, then resource ids.
# Writes every event that uses each resource and how (read, write, copy destination...).
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n") if l.strip()]
    capture, out, rids = lines[0], lines[1], [int(x) for x in lines[2:]]
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    ids = {int(r.resourceId): r.resourceId for r in controller.GetResources()}
    names = {int(r.resourceId): r.name for r in controller.GetResources()}
    report = open(out, "w")
    for rid in rids:
        report.write("== %d %s\n" % (rid, names.get(rid, "")))
        for u in controller.GetUsage(ids[rid]):
            report.write("%d %s\n" % (u.eventId, str(u.usage).replace("ResourceUsage.", "")))
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
