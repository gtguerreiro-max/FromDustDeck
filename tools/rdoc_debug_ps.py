# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_debug_ps.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture, output dir, draw event, optional "x,y".
# Debugs one pixel of the draw (the viewport's centre by default) and writes each executed
# instruction with the registers it changed (debug.txt), plus the viewport and scissor.
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
    os.makedirs(out, exist_ok=True)
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    controller.SetFrameEvent(event, True)
    state = controller.GetPipelineState()
    report = open(os.path.join(out, "debug.txt"), "w")
    vp = state.GetViewport(0)
    sc = state.GetScissor(0)
    report.write("viewport %g %g %g %g scissor %d %d %d %d\n" % (vp.x, vp.y, vp.width, vp.height,
                                                               sc.x, sc.y, sc.width, sc.height))
    if len(lines) > 3 and lines[3]:
        x, y = (int(v) for v in lines[3].split(","))
    else:
        x, y = int(vp.x + vp.width / 2), int(vp.y + vp.height / 2)
    report.write("pixel %d %d\n" % (x, y))
    inputs = rd.DebugPixelInputs()
    trace = controller.DebugPixel(x, y, inputs)
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
                report.write("%4d: %s\n" % (s.nextInstruction,
                                            " ".join(fmt_var(c.after) for c in s.changes)))
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
