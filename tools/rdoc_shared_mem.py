# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_shared_mem.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture, output dir, event id, guest address (hex),
# size (hex). After that event, reads the compute stage's writable buffers (Xenia's shared
# memory: the guest's 512 MB of physical memory) at the guest address and saves the bytes, so a
# resolve's output can be compared between captures.
import os
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n")]
    capture, out, event = lines[0], lines[1], int(lines[2])
    address, size = int(lines[3], 16), int(lines[4], 16)
    # Optional 6th line: read only this buffer (resource id), e.g. Xenia's EDRAM buffer.
    only = int(lines[5]) if len(lines) > 5 and lines[5] else None
    os.makedirs(out, exist_ok=True)
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    buffers = {b.resourceId: b for b in controller.GetBuffers()}
    report = open(os.path.join(out, "shared-mem.txt"), "w")
    controller.SetFrameEvent(event, True)
    state = controller.GetPipelineState()
    candidates = []
    for stage in (rd.ShaderStage.Compute, rd.ShaderStage.Pixel):
        try:
            for used in state.GetReadWriteResources(stage, True):
                rid = used.descriptor.resource
                if rid in buffers:
                    candidates.append(rid)
        except Exception as e:
            report.write("rw listing failed: %s\n" % e)
    # Also every buffer of at least 512 MB (the shared memory), in case it is bound otherwise.
    for rid, b in buffers.items():
        if b.length >= 0x20000000 and rid not in candidates:
            candidates.append(rid)
    if only is not None:
        candidates = [rid for rid in buffers if int(rid) == only]
    for rid in candidates:
        b = buffers[rid]
        report.write("buffer %d length 0x%X\n" % (int(rid), b.length))
        if b.length >= address + size:
            data = controller.GetBufferData(rid, address, size)
            path = os.path.join(out, "mem_%d_%08X.bin" % (int(rid), address))
            open(path, "wb").write(data)
            nonzero = sum(1 for i in range(0, len(data), 4) if data[i:i + 4] != b"\0\0\0\0")
            report.write("  saved %s, %d of %d dwords nonzero\n" % (path, nonzero, len(data) // 4))
    report.close()
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
os._exit(0)
