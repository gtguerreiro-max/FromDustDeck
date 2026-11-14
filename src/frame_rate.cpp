// Frame rate above the console's 30 FPS without speeding up the world.
//
// From Dust presents every second vblank, and its per-frame component update (sub_825803A8: a
// manager calls each flagged component's Update, vtable slot 5) steps the world once per call:
// the world component (dustai sub_8A237C20) kicks the GalileoKernel water, terrain and AI jobs.
// Running the loop faster (a faster guest vblank, video_mode_refresh_rate) would run the world
// faster too, which the PC frame-rate unlockers and the D3D12 port (F:\FromDustRecomp
// runtime/game_loop.cpp, ported here) both ran into. So:
//
// - The world component runs from a real-time accumulator at fromdust_world_hz (30 = the
//   game's speed); the other components, the camera and the render run every frame. Frame
//   intervals within 15% of 1/k steps count as exactly that, so a steady 60 FPS steps the world
//   every second frame instead of after 1, 2 or 3 frames.
// - Even pacing: the loop sleeps at the start of the update to hold 120, 60 or 30 FPS, the
//   fastest that divides the world rate, fits under fromdust_fps_limit and that the frames
//   keep up with. A rate in between (45 FPS) shows some world steps for one frame and others for
//   two: water and people move in small jerks while the camera is smooth.
// - The world logic reads pad buttons and triggers as edges, so XInputGetState (sub_82512348)
//   lets them change only before an update that steps the world, with the packet number
//   advanced on every change (the game ignores a state whose packet did not change). The sticks
//   stay live.
// - The Breath cursor gets, on updates without a world step, the ground height the world's last
//   step gave it (RestoreCursorHeight): otherwise its height, and the camera that follows it,
//   alternated every frame.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmodule.h>

#if defined(FROMDUST_NATIVE_RENDERER)
#include "hud/hud.h"  // ReXNative's settings HUD
#endif

#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

REXCVAR_DEFINE_DOUBLE(fromdust_world_hz, 30.0, "FromDust",
                      "World simulation steps per second (30 = the game's speed). 0 steps it once "
                      "per frame, as the console does, so it speeds up with the frame rate")
    .range(0.0, 120.0);
REXCVAR_DEFINE_INT32(fromdust_fps_limit, 60, "FromDust",
                     "Highest frame rate even pacing holds: 30, 60 or 120 (also needs "
                     "video_mode_refresh_rate at twice that, the game presents every 2nd vblank)")
    .range(30, 240);
REXCVAR_DEFINE_BOOL(fromdust_even_pacing, true, "FromDust",
                    "Hold 120, 60 or 30 FPS, whichever the frames keep up with, so each world "
                    "step stays on screen for the same number of frames");
REXCVAR_DEFINE_BOOL(fromdust_trace_world_steps, false, "FromDust",
                    "Log every 10 s how many frames each world step stayed on screen");

REXCVAR_DEFINE_BOOL(fromdust_cursor_height_fix, true, "FromDust",
                    "On frames without a world step, give the Breath cursor the ground height the "
                    "world set (no 30 Hz camera shake at 60 FPS)");
REX_EXTERN(__imp__sub_825803A8);  // the per-frame component update
REX_EXTERN(__imp__sub_825801A8);  // its post-update
REX_EXTERN(__imp__sub_82512348);  // XInputGetState(user, state)

namespace {

using clock_type = std::chrono::steady_clock;

constexpr uint32_t kWorldUpdate = 0x8A237C20;  // dustai's world component Update

uint32_t Load32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

std::mutex g_lock;
uint64_t g_updates = 0, g_decided_for = UINT64_MAX;
bool g_decision = true;
double g_debt = 0, g_residual = 0;
clock_type::time_point g_last{};
std::atomic<int> g_even_rate{0};
std::atomic<bool> g_settling{true};  // the world steps on every update (see the hook)
std::atomic<double> g_step_share{1.0};  // world steps per frame, last frame (FromDustWorldStepShare)
std::atomic<bool> g_last_stepped{true};  // the last update stepped the world (RexNativeGameWorldStepped)

double Rate() { return std::max(REXCVAR_GET(fromdust_world_hz), 0.0); }

// Whether update number `index` steps the world (decided once per update, g_lock held).
bool Decide(uint64_t index) {
  if (g_decided_for == index) return g_decision;
  const double rate = Rate();
  const auto now = clock_type::now();
  if (rate <= 0) {
    g_decision = true;
    g_debt = 0;
  } else {
    if (g_last.time_since_epoch().count()) {
      const double measured = std::chrono::duration<double>(now - g_last).count() * rate;
      double steps = measured;
      for (int k = 1; k <= 4; ++k) {
        if (std::abs(measured * k - 1.0) < 0.15) {
          steps = 1.0 / k;
          break;
        }
      }
      // The snapped-off difference is paid back once it reaches a quarter step, so a steady
      // rate off the divisors (55 FPS) does not slow the world down.
      g_residual += measured - steps;
      if (std::abs(g_residual) >= 0.25) {
        steps += g_residual;
        g_residual = 0;
      }
      g_debt = std::min(g_debt + steps, 2.0);
    } else {
      g_debt = 1.0;
    }
    g_decision = g_debt >= 1.0 - 1e-6;
    if (g_decision) g_debt = std::max(g_debt - 1.0, 0.0);
  }
  if (REXCVAR_GET(fromdust_trace_world_steps) && rate > 0) {
    static uint64_t since_step = 0, held[5] = {};
    static auto since = now;
    ++since_step;
    if (g_decision) {
      ++held[std::min<uint64_t>(since_step, 5) - 1];
      since_step = 0;
    }
    if (now - since >= std::chrono::seconds(10)) {
      REXLOG_INFO("world steps: frames per step 1:{} 2:{} 3:{} 4:{} 5+:{}, even pacing {} FPS",
                  held[0], held[1], held[2], held[3], held[4], g_even_rate.load());
      std::fill(std::begin(held), std::end(held), 0);
      since = now;
    }
  }
  g_last = now;
  g_decided_for = index;
  return g_decision;
}

// Even pacing (see the file comment). Windows of 30 frames: 3 frames that start more than a
// quarter interval late -> the next slower rate; faster again after `calm_needed` windows in a
// row whose frames average under 95% of the faster interval with at most one over 130%.
void PaceLoop(double rate) {
  using namespace std::chrono;
  static clock_type::time_point next, woke;
  static int level = -1, frames = 0, missed = 0, long_frames = 0, calm_windows = 0;
  static int calm_needed = 3, windows_since_up = 1 << 20;
  static double work_sum = 0;
  const auto now = clock_type::now();
  if (rate <= 0 || !REXCVAR_GET(fromdust_even_pacing)) {
    g_even_rate = 0;
    woke = {};
    level = -1;
    return;
  }
  const auto period = duration_cast<nanoseconds>(duration<double>(1.0 / rate));
  const nanoseconds even[3] = {period / 4, period / 2, period};
  const int limit = REXCVAR_GET(fromdust_fps_limit);
  int fastest = 0;
  while (fastest < 2 && even[fastest] + microseconds(200) < nanoseconds(1000000000LL / limit))
    ++fastest;
  if (level < 0) level = fastest, frames = missed = long_frames = calm_windows = 0, work_sum = 0;
  level = std::clamp(level, fastest, 2);
  if (woke.time_since_epoch().count()) {
    const auto work = now - woke;
    const auto lateness = now > next ? now - next : clock_type::duration{};
    ++frames;
    work_sum += duration<double, std::milli>(work).count();
    if (lateness > even[level] / 4) ++missed;
    if (level > fastest && work > even[level - 1] * 13 / 10) ++long_frames;
    if (work > seconds(1)) frames = missed = long_frames = 0, work_sum = 0;  // a load, not a frame
    if (frames >= 30) {
      const int before = level;
      ++windows_since_up;
      if (missed >= 3 && level < 2) {
        ++level, calm_windows = 0;
        if (windows_since_up < 8) calm_needed = std::min(calm_needed * 2, 48);
      } else if (level > fastest && long_frames <= 1 &&
                 work_sum / frames < duration<double, std::milli>(even[level - 1]).count() * 0.95) {
        if (++calm_windows >= calm_needed) --level, calm_windows = 0, windows_since_up = 0;
      } else {
        calm_windows = 0;
      }
      if (windows_since_up == 40) calm_needed = 3;
      if (level != before) {
        REXLOG_INFO("even pacing: {:.0f} FPS (frames take {:.1f} ms without the wait, {} of {} late)",
                    1e9 / double(even[level].count()), work_sum / frames, missed, frames);
      }
      frames = missed = long_frames = 0;
      work_sum = 0;
    }
  }
  g_even_rate = int(1e9 / double(even[level].count()) + 0.5);
  if (now < next) {
    const auto spin_from = next - microseconds(500);
    if (spin_from > now) std::this_thread::sleep_until(spin_from);
    while (clock_type::now() < next) _mm_pause();
  }
  // A frame a little late keeps the cadence; one later than half an interval is dropped, not
  // caught up with a burst.
  next = (now <= next + even[level] / 2 ? next : now) + even[level];
  woke = clock_type::now();
}

bool InputMayChange() {
  std::lock_guard lock(g_lock);
  return Rate() <= 0 || g_settling.load() || Decide(g_updates);
}

}  // namespace

// The world component the manager currently holds, or 0.
uint32_t FindWorld(uint8_t* base, uint32_t manager) {
  for (uint32_t i = 0; i < Load32(base, manager + 12); ++i) {
    const uint32_t entry = Load32(base, manager + 8) + i * 8;
    const uint32_t object = Load32(base, entry);
    if (object && Load32(base, Load32(base, object) + 20) == kWorldUpdate) return object;
  }
  return 0;
}

// The Breath cursor's height on updates without a world step. Each world Update sets the cursor's
// height to the ground under it (dustai sub_8A191FC0, from the world's sub_8A196A20); the cursor
// behaviour (sub_8A1F4298), which runs every frame after the render, then lifts it to its hover
// height, about 3 units above. On a frame without a world step the behaviour got its own last
// result instead of the ground and gave the ground back (measured: 40.02 -> 36.91, 36.91 ->
// 40.02), so the cursor's height alternated every frame, and the camera, which follows the
// cursor (sub_8A1FF3B0), went up and down at 30 Hz. The height the world set is kept and put
// back on those frames; the cursor's x and y, which the stick moves, are left alone.
constexpr uint32_t kCursorObject = 0x8A295318;  // dustai's Breath cursor (pointer)
constexpr uint32_t kCursorHeight = 1752;        // its position at +1744 (x, y, height)
uint32_t g_cursor = 0, g_cursor_height = 0;
void SaveCursorHeight(uint8_t* base) {
  g_cursor = Load32(base, kCursorObject);
  if (g_cursor) g_cursor_height = Load32(base, g_cursor + kCursorHeight);
}
void RestoreCursorHeight(uint8_t* base) {
  if (!REXCVAR_GET(fromdust_cursor_height_fix) || !g_cursor ||
      Load32(base, kCursorObject) != g_cursor)
    return;
  Store32(base, g_cursor + kCursorHeight, g_cursor_height);
}

REX_HOOK_RAW(sub_825803A8) {
  PaceLoop(Rate());
  // While a world loads (start-up, a level), code that runs every frame reads objects the
  // world fills in during its updates: skipping those updates left it reading a null one
  // (dustai sub_8A1FE5F8 reads [+468]+12; most start-ups crashed within 2 s). So the world
  // steps on every update until it has run for kSettle with no frame longer than kLongFrame
  // (a load), counted again after every long frame and for every new world object.
  using namespace std::chrono;
  constexpr auto kSettle = seconds(5);
  constexpr auto kLongFrame = milliseconds(100);
  static uint32_t world = 0;
  static clock_type::time_point settled_from{}, last_update{};
  // The start-up load ends with the _9_9 modules (water and pathfinding), 1.2 s after the
  // start on a 5600X but 6.4 s on a Steam Deck: there, kSettle ran out while the start-up still
  // needed every update, and about one start in two read the null object (guest 0x0000000C)
  // in a loop at 0.5 FPS. So settling starts only once they are loaded.
  static bool started = false;
  if (!started) {
    auto* kernel = REX_KERNEL_STATE();
    started = kernel && kernel->GetModule(R"(game:\GalileoSimulatorWater_9_9.xex)", true) &&
              kernel->GetModule(R"(game:\GalileoPathfind_9_9.xex)", true);
  }
  const auto now = clock_type::now();
  const uint32_t current = FindWorld(base, ctx.r3.u32);
  if (!started || current != world || !current || now - last_update > kLongFrame) {
    world = current;
    settled_from = now;
  }
  // The share of a world step the last frame stood for (water_flow.cpp): its length times
  // the world rate, 1 while the world steps every update.
  const double interval = std::chrono::duration<double>(now - last_update).count();
  last_update = now;
  bool world_steps;
  {
    std::lock_guard lock(g_lock);
    world_steps = Decide(g_updates);
    ++g_updates;
  }
  g_settling = now - settled_from < kSettle;
  if (g_settling) world_steps = true;
  g_step_share = g_settling || Rate() <= 0 ? 1.0 : std::clamp(interval * Rate(), 0.0, 2.0);
  g_last_stepped = world_steps;
  const uint32_t manager = ctx.r3.u32;
  if (world_steps) {
    __imp__sub_825803A8(ctx, base);
    SaveCursorHeight(base);
    return;
  }
  RestoreCursorHeight(base);
  // The manager's loop with the world component left out (sub_825803A8 itself: the update
  // flag at +64, {component, flags} entries at +8, their count at +12, flag 0x10 = updated).
  Store32(base, manager + 64, 1);
  for (uint32_t i = 0; i < Load32(base, manager + 12); ++i) {
    const uint32_t entry = Load32(base, manager + 8) + i * 8;
    if (!(Load32(base, entry + 4) & 0x10)) continue;
    const uint32_t object = Load32(base, entry);
    const uint32_t update = Load32(base, Load32(base, object) + 20);
    if (update == kWorldUpdate) continue;
    ctx.r3.u64 = object;
    rex::runtime::ResolveIndirectFunction(update)(ctx, base);
  }
  Store32(base, manager + 64, 0);
  ctx.r3.u64 = manager;
  __imp__sub_825801A8(ctx, base);
}

// XINPUT_STATE: packet (u32), then buttons (u16), left and right trigger (u8), sticks.
REXCVAR_DEFINE_STRING(fromdust_autopress, "", "FromDust",
                      "Scripted pad 0 presses for tests without hands on the device: "
                      "<seconds>:<buttons>:<ms>,... with buttons joined by + (START, BACK, A, B, "
                      "X, Y, LB, RB, LT, RT, UP, DOWN, LEFT, RIGHT), e.g. 40:START:200,52:A:200");

namespace {

struct AutoPress {
  double at;        // seconds after the first pad read
  double duration;  // seconds held
  uint16_t buttons;
  bool lt, rt;
};

const std::vector<AutoPress>& AutoPresses() {
  static const std::vector<AutoPress> list = [] {
    std::vector<AutoPress> out;
    const std::string text = REXCVAR_GET(fromdust_autopress);
    static const std::pair<const char*, uint16_t> kNames[] = {
        {"UP", 0x0001}, {"DOWN", 0x0002}, {"LEFT", 0x0004},  {"RIGHT", 0x0008},
        {"START", 0x0010}, {"BACK", 0x0020}, {"LB", 0x0100}, {"RB", 0x0200},
        {"A", 0x1000},  {"B", 0x2000},  {"X", 0x4000},  {"Y", 0x8000}};
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find(',', at);
      if (end == std::string::npos) end = text.size();
      const std::string item = text.substr(at, end - at);
      at = end + 1;
      const size_t c1 = item.find(':'), c2 = item.find(':', c1 + 1);
      if (c1 == std::string::npos) continue;
      AutoPress press{std::atof(item.c_str()), 0.2, 0, false, false};
      const std::string names = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos
                                                                           : c2 - c1 - 1);
      if (c2 != std::string::npos) press.duration = std::atof(item.c_str() + c2 + 1) / 1000.0;
      size_t n = 0;
      while (n < names.size()) {
        size_t plus = names.find('+', n);
        if (plus == std::string::npos) plus = names.size();
        const std::string name = names.substr(n, plus - n);
        n = plus + 1;
        if (name == "LT") press.lt = true;
        if (name == "RT") press.rt = true;
        for (const auto& [key, bit] : kNames) {
          if (name == key) press.buttons |= bit;
        }
      }
      out.push_back(press);
    }
    return out;
  }();
  return list;
}

// The scripted presses active now, OR-ed into pad 0's guest state (big-endian buttons at +0,
// triggers at +2 and +3 of XINPUT_GAMEPAD). Returns whether any is active.
bool ApplyAutoPress(uint8_t* pad) {
  const auto& presses = AutoPresses();
  if (presses.empty()) return false;
  static const auto start = clock_type::now();
  const double now = std::chrono::duration<double>(clock_type::now() - start).count();
  bool any = false;
  for (const auto& p : presses) {
    if (now < p.at || now >= p.at + p.duration) continue;
    any = true;
    const uint16_t buttons = uint16_t((pad[0] << 8 | pad[1]) | p.buttons);
    pad[0] = uint8_t(buttons >> 8);
    pad[1] = uint8_t(buttons);
    if (p.lt) pad[2] = 255;
    if (p.rt) pad[3] = 255;
  }
  return any;
}

}  // namespace

REX_HOOK_RAW(sub_82512348) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_82512348(ctx, base);
  // Scripted presses also stand in for a missing pad 0 (a run over SSH may have none).
  if (ctx.r3.u32 != 0 && user == 0 && state && !AutoPresses().empty()) {
    std::memset(base + state, 0, 16);
    ctx.r3.u64 = 0;
  }
  if (ctx.r3.u32 != 0 || !state || user >= 4) return;
  if (user == 0) ApplyAutoPress(base + state + 4);
  struct Pad {
    uint8_t bytes[12];
  };
  static Pad held[4], shown[4];
  static uint32_t packets[4];
  static std::mutex lock;
  std::lock_guard guard(lock);
  Pad pad;
  std::memcpy(pad.bytes, base + state + 4, sizeof(pad.bytes));
#if defined(FROMDUST_NATIVE_RENDERER)
  // The settings HUD's chord (ReXNative), and a neutral pad while it is up.
  rexnative::hud::FilterPad(user, pad.bytes);
#endif
  if (InputMayChange()) {
    held[user] = pad;
  } else {
    std::memcpy(pad.bytes, held[user].bytes, 4);  // buttons and triggers as last held
  }
  if (std::memcmp(&shown[user], &pad, sizeof(pad))) {
    shown[user] = pad;
    ++packets[user];
  }
  std::memcpy(base + state + 4, pad.bytes, sizeof(pad.bytes));
  Store32(base, state, packets[user]);
}

// The share of a world step the last frame stood for (water_flow.cpp).
double FromDustWorldStepShare() { return g_step_share.load(); }
// Whether the last update stepped the world (the native renderer's camera trace).
bool RexNativeGameWorldStepped() { return g_last_stepped.load(); }
