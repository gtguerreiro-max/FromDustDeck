# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_events.py
# Reads F:/FromDustDeck/build/rdoc-job.txt (capture path, output dir, optional render target
# format filter, default R16G16) and writes, for every draw that renders to a target of that
# format: events.txt with each draw's pixel shader constant buffer contents (as floats), its
# bound textures (size, format) and the pixel shader's disassembly in ps_<event>.txt. For
# comparing the same pass between two captures (the D3D12 port's and this port's).
import os
import struct
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = open(JOB).read().split("\n")
    capture, out = lines[0].strip(), lines[1].strip()
    fmt_filter = lines[2].strip() if len(lines) > 2 and lines[2].strip() else "R16G16_"
    os.makedirs(out, exist_ok=True)
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    textures = {t.resourceId: t for t in controller.GetTextures()}
    sfile = controller.GetStructuredFile()
    actions = []

    def walk(items):
        for a in items:
            if a.flags & rd.ActionFlags.Drawcall:
                actions.append(a)
            walk(a.children)

    walk(controller.GetRootActions())
    report = open(os.path.join(out, "events.txt"), "w")
    done = 0
    for a in actions:
        targets = [o for o in a.outputs if o != rd.ResourceId.Null()]
        names = [str(textures[o].format.Name()) if o in textures else "?" for o in targets]
        if not any(fmt_filter in n for n in names):
            continue
        controller.SetFrameEvent(a.eventId, True)
        state = controller.GetPipelineState()
        report.write("=== event %d %s -> %s\n" % (a.eventId, a.GetName(sfile), " ".join(names)))
        ps = state.GetShaderReflection(rd.ShaderStage.Pixel)
        # Blending of the first target.
        try:
            for i, b in enumerate(state.GetColorBlends()[:1]):
                report.write("  blend %d enabled=%s color=%s %s %s alpha=%s %s %s mask=%x\n" % (
                    i, b.enabled, b.colorBlend.source, b.colorBlend.operation,
                    b.colorBlend.destination, b.alphaBlend.source, b.alphaBlend.operation,
                    b.alphaBlend.destination, b.writeMask))
        except Exception as e:
            report.write("  blend read failed: %s\n" % e)
        # Raw DDS of the first bound texture and of the target after this draw, for numbers.
        if lines and len(lines) > 3 and str(a.eventId) in lines[3].split(","):
            try:
                for used in state.GetReadOnlyResources(rd.ShaderStage.Pixel, True)[:1]:
                    save = rd.TextureSave()
                    save.resourceId = used.descriptor.resource
                    save.destType = rd.FileType.DDS
                    controller.SaveTexture(save, os.path.join(out, "srv_%05d.dds" % a.eventId))
                save = rd.TextureSave()
                save.resourceId = targets[0]
                save.destType = rd.FileType.DDS
                controller.SaveTexture(save, os.path.join(out, "rt_%05d.dds" % a.eventId))
                report.write("  saved srv_%05d.dds, rt_%05d.dds\n" % (a.eventId, a.eventId))
            except Exception as e:
                report.write("  dds save failed: %s\n" % e)
        # Bound read-only resources of the pixel stage.
        try:
            for used in state.GetReadOnlyResources(rd.ShaderStage.Pixel, True):
                rid = used.descriptor.resource
                if rid in textures:
                    t = textures[rid]
                    report.write("  srv %s %dx%d %s\n" % (used.access.index if hasattr(used, "access") else "",
                                                          t.width, t.height, t.format.Name()))
        except Exception as e:  # API differences between versions
            report.write("  srv listing failed: %s\n" % e)
        # Constant buffers of the pixel stage, as floats.
        if ps is not None:
            for i, cb in enumerate(ps.constantBlocks):
                try:
                    bind = state.GetConstantBlock(rd.ShaderStage.Pixel, i, 0).descriptor
                    data = controller.GetBufferData(bind.resource, bind.byteOffset,
                                                    min(bind.byteSize or 4096, 4096))
                    floats = struct.unpack("<%df" % (len(data) // 4), data[: len(data) // 4 * 4])
                    nonzero = [(k, v) for k, v in enumerate(floats) if v != 0.0][:64]
                    report.write("  cb %d %s: %s\n" % (i, cb.name, " ".join(
                        "%d=%.6g" % kv for kv in nonzero)))
                except Exception as e:
                    report.write("  cb %d read failed: %s\n" % (i, e))
            try:
                targets_d = controller.GetDisassemblyTargets(True)
                text = controller.DisassembleShader(state.GetGraphicsPipelineObject(), ps, targets_d[0])
                open(os.path.join(out, "ps_%05d.txt" % a.eventId), "w").write(text)
            except Exception as e:
                report.write("  disassembly failed: %s\n" % e)
        done += 1
        if done >= 12:
            break
    report.close()
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write("ok\n")
except Exception:
    open(r"F:\FromDustDeck\build\rdoc-script.log", "w").write(traceback.format_exc())
os._exit(0)
