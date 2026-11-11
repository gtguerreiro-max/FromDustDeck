# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_save_many.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture path, output dir, then "event resource"
# lines. Saves each texture as it is after its event, as <dir>/<event>-<resource>.png.
import os
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n") if l.strip()]
    capture, out = lines[0], lines[1]
    os.makedirs(out, exist_ok=True)
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    ids = {int(t.resourceId): t.resourceId for t in controller.GetTextures()}
    for line in lines[2:]:
        event, rid = (int(x) for x in line.split())
        controller.SetFrameEvent(event, True)
        s = rd.TextureSave()
        s.resourceId = ids[rid]
        s.destType = rd.FileType.PNG
        s.alpha = rd.AlphaMapping.Discard
        controller.SaveTexture(s, os.path.join(out, "%d-%d.png" % (event, rid)))
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
os._exit(0)
