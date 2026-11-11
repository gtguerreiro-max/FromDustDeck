# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_save_tex.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture, output PNG path, event, resource id.
# Saves that texture as it is after the event (e.g. the back buffer at Present) as PNG.
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n")]
    capture, out, event, rid = lines[0], lines[1], int(lines[2]), int(lines[3])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    controller.SetFrameEvent(event, True)
    for t in controller.GetTextures():
        if int(t.resourceId) == rid:
            s = rd.TextureSave()
            s.resourceId = t.resourceId
            s.destType = rd.FileType.PNG
            s.alpha = rd.AlphaMapping.Discard
            open(out + ".txt", "w").write("%dx%d samples %d arraysize %d fmt %s\n" % (t.width, t.height, t.msSamp, t.arraysize, t.format.Name()))
            controller.SaveTexture(s, out)
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
import os
os._exit(0)
