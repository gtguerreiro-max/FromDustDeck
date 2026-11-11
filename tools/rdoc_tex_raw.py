# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_tex_raw.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture path, output file, event, resource id.
# Writes the texture's mip 0 bytes as the replay holds them after the event.
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n") if l.strip()]
    capture, out, event, rid = lines[0], lines[1], int(lines[2]), int(lines[3])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    controller.SetFrameEvent(event, True)
    tex = next(t.resourceId for t in controller.GetTextures() if int(t.resourceId) == rid)
    open(out, "wb").write(bytes(controller.GetTextureData(tex, rd.Subresource(0, 0, 0))))
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
import os
os._exit(0)
