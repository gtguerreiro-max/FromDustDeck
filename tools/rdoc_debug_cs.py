# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_debug_cs.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture, output dir, event, group "x,y,z",
# thread "x,y,z". Runs RenderDoc's shader debugger on that compute thread and writes every
# executed instruction with the registers it changed (debug.txt), to see where a dispatch's
# thread goes (early return, branch, final store address and value).
import os
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def fmt_var(v):
    vals = v.value.u32v
    n = max(1, v.columns) * max(1, v.rows)
    return "%s=[%s]" % (v.name, " ".join("%08X" % vals[i] for i in range(min(n, 4))))


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n")]
    capture, out, event = lines[0], lines[1], int(lines[2])
    group = tuple(int(x) for x in lines[3].split(","))
    thread = tuple(int(x) for x in lines[4].split(","))
    os.makedirs(out, exist_ok=True)
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    controller.SetFrameEvent(event, True)
    report = open(os.path.join(out, "debug.txt"), "w")
    trace = controller.DebugThread(group, thread)
    if trace is None or trace.debugger is None:
        report.write("debugging not available\n")
    else:
        steps = 0
        while steps < 20000:
            states = controller.ContinueDebug(trace.debugger)
            if not states:
                break
            for s in states:
                steps += 1
                changes = " ".join(fmt_var(c.after) for c in s.changes)
                report.write("%4d: %s\n" % (s.nextInstruction, changes))
        controller.FreeDebugger(trace.debugger)
        report.write("steps %d\n" % steps)
    report.close()
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
os._exit(0)
