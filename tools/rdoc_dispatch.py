# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_dispatch.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture, output dir, event ids (comma separated).
# For each dispatch: thread group counts, the compute shader's root constants / constant
# buffers as 32-bit words (hex), its bound read-only and writable resources, and the shader's
# disassembly, to see what a Xenia resolve dispatch was told to do (destination, format).
import os
import struct
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n")]
    capture, out = lines[0], lines[1]
    events = [int(e) for e in lines[2].split(",") if e]
    os.makedirs(out, exist_ok=True)
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    report = open(os.path.join(out, "dispatch.txt"), "w")
    by_event = {}

    def walk(items):
        for a in items:
            by_event[a.eventId] = a
            walk(a.children)

    walk(controller.GetRootActions())
    for event in events:
        controller.SetFrameEvent(event, True)
        state = controller.GetPipelineState()
        action = by_event.get(event)
        report.write("=== event %d\n" % event)
        if action is not None:
            report.write("  groups %s\n" % (tuple(action.dispatchDimension),))
        cs = state.GetShaderReflection(rd.ShaderStage.Compute)
        if cs is None:
            report.write("  no compute shader\n")
            continue
        for i, cb in enumerate(cs.constantBlocks):
            try:
                bind = state.GetConstantBlock(rd.ShaderStage.Compute, i, 0).descriptor
                size = min(bind.byteSize or 256, 256)
                data = controller.GetBufferData(bind.resource, bind.byteOffset, size)
                words = struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4])
                report.write("  cb %d %s (%d bytes): %s\n" % (i, cb.name, cb.byteSize,
                                                             " ".join("%08X" % w for w in words[:24])))
            except Exception as e:
                report.write("  cb %d read failed: %s\n" % (i, e))
        try:
            for used in state.GetReadWriteResources(rd.ShaderStage.Compute, True):
                d = used.descriptor
                report.write("  rw resource %d type %s fmt %s offset 0x%X size 0x%X elem %d\n" % (
                    int(d.resource), d.type, d.format.Name(), d.byteOffset, d.byteSize,
                    getattr(d, "elementByteSize", 0)))
            for used in state.GetReadOnlyResources(rd.ShaderStage.Compute, True):
                report.write("  ro resource %d\n" % int(used.descriptor.resource))
        except Exception as e:
            report.write("  resource listing failed: %s\n" % e)
        try:
            targets = controller.GetDisassemblyTargets(True)
            text = controller.DisassembleShader(state.GetComputePipelineObject(), cs, targets[0])
            open(os.path.join(out, "cs_%05d.txt" % event), "w").write(text)
        except Exception as e:
            report.write("  disassembly failed: %s\n" % e)
    report.close()
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
os._exit(0)
