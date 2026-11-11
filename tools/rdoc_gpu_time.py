# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc_gpu_time.py
# Reads F:/FromDustDeck/build/rdoc-job.txt: capture path, output file. Measures the GPU duration
# of every action (RenderDoc's GPUDuration counter) and sums it by kind: the game's draws, the
# render target cache's ownership transfers (draws whose pixel shader is a transfer shader),
# dispatches (EDRAM dumps, resolves, texture loads), copies and clears, and the top actions.
import traceback

JOB = r"F:\FromDustDeck\build\rdoc-job.txt"


def main():
    import renderdoc as rd
    lines = [l.strip() for l in open(JOB).read().split("\n") if l.strip()]
    capture, out = lines[0], lines[1]
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    sfile = controller.GetStructuredFile()
    textures = {t.resourceId: t for t in controller.GetTextures()}
    actions = {}

    def walk(items):
        for a in items:
            if not a.children:
                actions[a.eventId] = a
            walk(a.children)

    walk(controller.GetRootActions())
    results = controller.FetchCounters([rd.GPUCounter.EventGPUDuration])
    durations = {r.eventId: r.value.d for r in results}
    # Classify draws by their pixel shader's entry/debug name: transfer shaders are generated
    # with "xe_transfer" resources.
    kinds = {}
    top = []
    shader_kind = {}
    for eid, dur in durations.items():
        a = actions.get(eid)
        if not a:
            continue
        name = a.GetName(sfile)
        kind = "other"
        if a.flags & rd.ActionFlags.Drawcall:
            controller.SetFrameEvent(eid, False)
            ps = controller.GetPipelineState().GetShaderReflection(rd.ShaderStage.Pixel)
            key = ps.resourceId if ps else None
            if key not in shader_kind:
                names = " ".join(r.name for r in (ps.readOnlyResources if ps else [])) + " " + \
                    " ".join(c.name for c in (ps.constantBlocks if ps else []))
                shader_kind[key] = "transfer" if "xe_transfer" in names else "game draw"
            kind = shader_kind[key]
            outs = [o for o in a.outputs if o != rd.ResourceId.Null()] + \
                ([a.depthOut] if a.depthOut != rd.ResourceId.Null() else [])
            if outs and outs[0] in textures:
                t = textures[outs[0]]
                kind += " %dx%d" % (t.width, t.height)
        elif a.flags & rd.ActionFlags.Dispatch:
            kind = "dispatch"
        elif a.flags & rd.ActionFlags.Copy:
            kind = "copy"
        elif a.flags & rd.ActionFlags.Clear:
            kind = "clear"
        k = kinds.setdefault(kind, [0, 0.0])
        k[0] += 1
        k[1] += dur
        top.append((dur, eid, name, kind))
    total = sum(v[1] for v in kinds.values())
    report = open(out, "w")
    report.write("total GPU %.3f ms over %d actions\n" % (total * 1000, len(top)))
    for kind, (n, d) in sorted(kinds.items(), key=lambda kv: -kv[1][1]):
        report.write("%-40s %5d actions %8.3f ms %5.1f %%\n" % (kind, n, d * 1000, 100 * d / total))
    report.write("\ntop actions:\n")
    for dur, eid, name, kind in sorted(top, reverse=True)[:40]:
        report.write("%8.3f ms  %6d  %-30s %s\n" % (dur * 1000, eid, name[:30], kind))
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
