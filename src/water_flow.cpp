// The water's texture flow at the world's speed above 30 FPS.
//
// The water UV advection pass (the pixel shader that samples g_SamplerPreviousUV) moves the
// water's texture coordinates along the flow on every draw by PS_f4_WaterTime.x (c36, 0.0032)
// and PS_f4_UVScrollParams.y (c38, 0.0029), steps made for one draw per 30 Hz frame. The world
// itself keeps its 30 steps per second (frame_rate.cpp), but this pass runs every rendered
// frame, so at 60 FPS the water's texture flowed twice as fast. Found and fixed the same way in
// the D3D12 port (F:\FromDustRecomp docs/NATIVE_RENDERER.md, "Water texture flow"): those
// steps are scaled on that draw by the frame's share of a world step. Measured here: in play
// c36.x is a real-time phase (see Hook), so only the components that are steps are scaled.
//
// fromdust_find_water_shader logs every pixel shader that reads c36 and c38 (with their values)
// and, every 600 draws of the pass, the scale applied.

#include <rex/cvar.h>
#include <rex/graphics/draw_hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <set>

REXCVAR_DEFINE_BOOL(fromdust_water_flow_fix, true, "FromDust",
                    "Keep the water's texture flow at the world's speed above 30 FPS");
REXCVAR_DEFINE_BOOL(fromdust_find_water_shader, false, "FromDust",
                    "Log the shaders that read c36 and c38 and the water flow scale applied");
REXCVAR_DEFINE_STRING(fromdust_water_shader_hash, "", "FromDust",
                      "Ucode hash (hex) of the water UV advection pixel shader; empty: found by "
                      "the constants it reads");

double FromDustWorldStepShare();  // frame_rate.cpp

namespace {

uint64_t WaterHash() {
  static const uint64_t hash = [] {
    const std::string text = REXCVAR_GET(fromdust_water_shader_hash);
    return text.empty() ? 0 : std::stoull(text, nullptr, 16);
  }();
  return hash;
}

float GetF(const uint32_t* c, int reg, int comp) { return std::bit_cast<float>(c[reg * 4 + comp]); }
void SetF(uint32_t* c, int reg, int comp, float v) { c[reg * 4 + comp] = std::bit_cast<uint32_t>(v); }

bool Reads(const uint64_t* used, int reg) { return (used[reg / 64] >> (reg % 64)) & 1; }

// The advection pass, told apart by the constants it reads rather than by their values (c36 is
// shared state that most draws inherit): it reads c36 (WaterTime) and c38 (UVScrollParams) and
// few others. In this build three shaders match: 1481C0FA6A4885D3 (the title's sea, 4
// constants) and DDDE27E5D5243460 / E7D99DD63D212F01 in play (11 and 12, c38.y = 0.00293); a
// water surface shader would read lighting and matrices too. fromdust_water_shader_hash, if
// set, picks one shader by hash instead.
bool IsAdvectionPass(uint64_t hash, const uint64_t* used) {
  if (WaterHash()) return hash == WaterHash();
  if (!Reads(used, 36) || !Reads(used, 38)) return false;
  int count = 0;
  for (int w = 0; w < 4; ++w) count += std::popcount(used[w]);
  return count <= 16;
}

bool Hook(uint64_t hash, const uint64_t* used, uint32_t* constants) {
  const bool find = REXCVAR_GET(fromdust_find_water_shader);
  const bool water = REXCVAR_GET(fromdust_water_flow_fix) && IsAdvectionPass(hash, used);
  const bool candidate = find && Reads(used, 36) && Reads(used, 38);
  if (!constants) return water || candidate;
  if (candidate) {
    static std::mutex lock;
    static std::set<uint64_t> seen;
    std::lock_guard guard(lock);
    if (seen.insert(hash).second) {
      int count = 0;
      for (int w = 0; w < 4; ++w) count += std::popcount(used[w]);
      REXLOG_INFO("water shader candidate: hash {:016X} reads {} constants, c36 = {:.5f} {:.5f}, "
                  "c38 = {:.5f} {:.5f}",
                  hash, count, GetF(constants, 36, 0), GetF(constants, 36, 1),
                  GetF(constants, 38, 0), GetF(constants, 38, 1));
    }
  }
  if (!water) return false;
  const float share = float(std::clamp(FromDustWorldStepShare(), 0.0, 2.0));
  if (find) {
    static std::atomic<uint64_t> scaled{0};
    if (scaled.fetch_add(1) % 600 == 0) {
      REXLOG_INFO("water flow: shader {:016X}, step share {:.2f}, c36.x {:.5f}, c38.y {:.5f}", hash,
                  share, GetF(constants, 36, 0), GetF(constants, 38, 1));
    }
  }
  // Only the per-draw steps are scaled. In play c36.x is a real-time phase (0..1, moving
  // ~0.75/s at 30 and at 60 FPS alike: measured), not a step, and scaling it would make the
  // water jump; c38.y (0.00293) and the title's c36.x (0.0032) are the steps. A shader whose
  // c36.x has ever been above kStepMax keeps it untouched for good.
  constexpr float kStepMax = 0.02f;
  static std::mutex lock;
  static std::set<uint64_t> phase_shaders;
  const float c36x = GetF(constants, 36, 0);
  bool c36x_is_step;
  {
    std::lock_guard guard(lock);
    if (std::abs(c36x) > kStepMax) phase_shaders.insert(hash);
    c36x_is_step = !phase_shaders.contains(hash);
  }
  if (share == 1.0f) return false;
  bool changed = false;
  if (c36x_is_step && c36x != 0.0f) {
    SetF(constants, 36, 0, c36x * share);
    changed = true;
  }
  if (const float c38y = GetF(constants, 38, 1); c38y != 0.0f && std::abs(c38y) <= kStepMax) {
    SetF(constants, 38, 1, c38y * share);
    changed = true;
  }
  return changed;
}

}  // namespace

void InstallWaterFlowFix() { rex::graphics::SetPixelConstantHook(Hook); }
