// The game-side functions the native renderer (src/native, from PGR4Native) calls, which
// PGR4Native's games/pgr4/src/pgr4_hooks.cpp defines for PGR4. From Dust's own frame pacing is
// src/frame_rate.cpp; the renderer's FramePacer is the clock only while it presents
// (NativeRenderer::PacesFrames).

#include <cstdint>

// The 60 FPS option of PGR4's settings (its guest presentation interval ONE). From Dust has none:
// its frame rate comes from frame_rate.cpp's world stepping and the guest vblank.
bool Pgr4Enable60Fps() { return false; }

// The time of PGR4's last car physics step, for DLSS 5's scene timing (neural.cpp). From Dust has
// no such step: 0 makes the renderer use the frame's own time.
uint64_t Pgr4LastCarStepNs() { return 0; }

// PGR4Native's SDK patch 0025 (the guest's pad reads per frame, for the renderer's frame CSV) is
// not applied here: no pad summary.
extern "C" uint64_t rex_xam_take_frame_pad() { return 0; }
