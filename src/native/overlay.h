// An ImGui-style overlay drawn over the presented frame: the triangles of the From Dust settings
// menu (fromdust_settings.cpp), which builds them with its own ImGui context on the UI thread.
// The overlay only draws what it is handed, textured by one RGBA font atlas, so the renderer does
// not touch ImGui (one context in rexruntime.dll that ReXGlue's UI thread also switches).
#pragma once

#include "d3d12_device.h"

#include <d3d12.h>

#include <cstdint>
#include <vector>

namespace pgr4::native {

class CommandStream;

// ImDrawVert's layout: position and atlas coordinates in pixels, RGBA8 color.
struct OverlayVertex {
  float x, y, u, v;
  uint32_t color;
};
// One draw: `count` 16-bit indices from `first_index`, added to `first_vertex`, cut by the
// clip rectangle (pixels: left, top, right, bottom).
struct OverlayDraw {
  int32_t clip[4];
  uint32_t count, first_index, first_vertex;
};
struct OverlayFrame {
  std::vector<OverlayVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<OverlayDraw> draws;
};
// The atlas the vertices' u and v (0..1) sample; `version` changes when it is rebuilt.
struct OverlayFont {
  std::vector<uint8_t> rgba;
  uint32_t width = 0, height = 0, version = 0;
};

// From the game (fromdust_settings.cpp), on the guest thread at each Swap: the overlay's latest
// triangles for a back buffer of width x height, true while it is up. It also asks the UI thread
// for the next ones.
bool OverlayFrameFor(uint32_t width, uint32_t height, OverlayFrame* frame);
// The atlas the frames sample (copied when its version changed); nullptr before one is built.
bool OverlayFontIfChanged(uint32_t version, OverlayFont* font);

class Overlay {
 public:
  bool Initialize(ID3D12Device* device);
  // Draws `frame` into the back buffer `rtv` (in RENDER_TARGET), on the frame's slot.
  void Record(ID3D12Device* device, CommandStream* list, uint32_t slot,
              D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height, bool hdr,
              const OverlayFrame& frame);

 private:
  bool UploadFont(ID3D12Device* device, CommandStream* list, const OverlayFont& font);

  ComPtr<ID3D12RootSignature> root_signature_;
  ComPtr<ID3D12PipelineState> pso_, hdr_pso_;
  ComPtr<ID3D12DescriptorHeap> heap_;
  ComPtr<ID3D12Resource> font_, font_upload_;
  uint32_t font_version_ = 0, font_view_ = 0, heap_stride_ = 0;
  // Per frame slot: vertices then indices, mapped while the overlay lives.
  struct Slot {
    ComPtr<ID3D12Resource> buffer;
    uint8_t* mapped = nullptr;
    uint64_t size = 0;
  } slots_[D3D12Device::kFramesInFlight];
};

}  // namespace pgr4::native
