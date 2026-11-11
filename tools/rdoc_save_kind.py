# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_save_kind.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture path, output dir, event, then
# "<width>x<height>:<format>" lines. Saves every texture of those kinds as it is after the
# event (<dir>/<kind>-<resource>.png), to compare the same resources between two captures.
import os
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n") if l.strip()]
    capture, out, event, kinds = lines[0], lines[1], int(lines[2]), set(lines[3:])
    os.makedirs(out, exist_ok=True)
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    controller.SetFrameEvent(event, True)
    for t in controller.GetTextures():
        kind = "%dx%d:%s" % (t.width, t.height, t.format.Name())
        if kind in kinds:
            s = rd.TextureSave()
            s.resourceId = t.resourceId
            s.destType = rd.FileType.PNG
            s.alpha = rd.AlphaMapping.Discard
            controller.SaveTexture(s, os.path.join(out, "%s-%d.png" % (kind.replace(":", "_"), int(t.resourceId))))
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
os._exit(0)
