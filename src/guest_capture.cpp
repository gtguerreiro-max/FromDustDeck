// Saves the game's own image (the guest output, before the presenter's FSR / scaling) at given
// seconds after start: fromdust_capture_at = "40,100" writes captures/guest-40s.bmp and
// captures/guest-100s.bmp beside the executable. Window captures cannot always see the game: on
// Windows with Vulkan the swapchain is presented in a way GDI reads as black, and on the Deck
// the game runs under gamescope. Presenter::CaptureGuestOutput reads the image itself.

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <fmt/format.h>
#include <rex/ui/presenter.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../third_party/renderdoc/renderdoc_app.h"
#endif

REXCVAR_DEFINE_STRING(fromdust_capture_at, "", "FromDust",
                      "Seconds after start at which to save the game's image to captures/ "
                      "(comma separated, e.g. 40,100)");
REXCVAR_DEFINE_STRING(fromdust_dump_memory, "", "FromDust",
                      "Guest physical ranges to save at the fromdust_capture_at times, as "
                      "<hex address>:<hex size>,... (captures/mem-<address>-<s>s.bin), e.g. the "
                      "height field 14F7B000:100000");

namespace {

// Launched under RenderDoc (renderdoccmd capture -w fromdust.exe ...): capture the next frame
// too, with the GPU's whole work for it (the passes, their inputs, outputs and constants).
void TriggerRenderDoc(int s) {
#if defined(_WIN32)
  HMODULE rdoc = GetModuleHandleA("renderdoc.dll");
  if (!rdoc) return;
  auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(rdoc, "RENDERDOC_GetAPI"));
  RENDERDOC_API_1_1_2* api = nullptr;
  if (get_api && get_api(eRENDERDOC_API_Version_1_1_2, reinterpret_cast<void**>(&api)) == 1 &&
      api) {
    api->TriggerCapture();
    REXLOG_INFO("RenderDoc capture triggered at {} s", s);
  }
#else
  (void)s;
#endif
}

// 24-bit bottom-up BMP from the presenter's R8 G8 B8 X8 rows.
bool WriteBmp(const std::filesystem::path& path, const rex::ui::RawImage& image) {
  const uint32_t row = (image.width * 3 + 3) & ~3u;
  const uint32_t size = 54 + row * image.height;
  std::vector<uint8_t> out(size, 0);
  auto put32 = [&](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + i] = uint8_t(v >> (8 * i));
  };
  out[0] = 'B';
  out[1] = 'M';
  put32(2, size);
  put32(10, 54);
  put32(14, 40);
  put32(18, image.width);
  put32(22, image.height);
  out[26] = 1;
  out[28] = 24;
  put32(34, row * image.height);
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* src = image.data.data() + size_t(image.height - 1 - y) * image.stride;
    uint8_t* dst = out.data() + 54 + size_t(y) * row;
    for (uint32_t x = 0; x < image.width; ++x) {
      dst[x * 3 + 0] = src[x * 4 + 2];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 0];
    }
  }
  FILE* f = nullptr;
#if defined(_WIN32)
  _wfopen_s(&f, path.c_str(), L"wb");
#else
  f = std::fopen(path.c_str(), "wb");
#endif
  if (!f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

// Saves the fromdust_dump_memory ranges (guest physical addresses, as the GPU sees them).
void DumpMemory(const std::filesystem::path& dir, int s) {
  const std::string list = REXCVAR_GET(fromdust_dump_memory);
  std::stringstream text(list);
  for (std::string item; std::getline(text, item, ',');) {
    const size_t colon = item.find(':');
    if (colon == std::string::npos) continue;
    const uint32_t address = uint32_t(std::stoul(item.substr(0, colon), nullptr, 16));
    const uint32_t size = uint32_t(std::stoul(item.substr(colon + 1), nullptr, 16));
    const auto* data = rex::system::kernel_memory()->TranslatePhysical<const uint8_t*>(address);
    const auto path = dir / fmt::format("mem-{:08X}-{}s.bin", address, s);
    FILE* f = nullptr;
#if defined(_WIN32)
    _wfopen_s(&f, path.c_str(), L"wb");
#else
    f = std::fopen(path.c_str(), "wb");
#endif
    if (!f || !data) continue;
    std::fwrite(data, 1, size, f);
    std::fclose(f);
    REXLOG_INFO("memory dump at {} s: {:08X}+{:X} -> {}", s, address, size, path.string());
  }
}

}  // namespace

void StartGuestCaptures(rex::Runtime* runtime) {
  const std::string list = REXCVAR_GET(fromdust_capture_at);
  if (list.empty() || !runtime) return;
  std::vector<int> seconds;
  std::stringstream text(list);
  for (std::string item; std::getline(text, item, ',');) {
    if (!item.empty()) seconds.push_back(std::stoi(item));
  }
  const auto dir = rex::filesystem::GetExecutableFolder() / "captures";
  std::thread([runtime, seconds, dir] {
    const auto start = std::chrono::steady_clock::now();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (int s : seconds) {
      std::this_thread::sleep_until(start + std::chrono::seconds(s));
      TriggerRenderDoc(s);
      auto* graphics = runtime->graphics_system();
      auto* presenter = graphics ? graphics->presenter() : nullptr;
      rex::ui::RawImage image;
      if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width) {
        REXLOG_WARN("guest capture at {} s: no image", s);
        continue;
      }
      const auto path = dir / ("guest-" + std::to_string(s) + "s.bmp");
      REXLOG_INFO("guest capture at {} s: {}x{} -> {}", s, image.width, image.height,
                  WriteBmp(path, image) ? path.string() : std::string("write failed"));
      DumpMemory(dir, s);
    }
  }).detach();
}
