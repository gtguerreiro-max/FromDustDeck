// AMD FidelityFX Super Resolution 1 for the present: EASU scales the front buffer's display
// values up to the window, RCAS sharpens them into the back buffer. The shaders are the ones
// ReXGlue's D3D12 presenter ships (Xenia's guest_output_ffx_fsr_easu / _rcas and its
// triangle-strip rectangle vertex shader, prebuilt DXBC; FidelityFX FSR 1.0 is MIT-licensed),
// bound the way that presenter binds them.
#pragma once

#include "d3d12_device.h"

#include <d3d12.h>

#include <cstdint>
#include <vector>

namespace pgr4::native {

class CommandStream;

class Fsr1 {
 public:
  bool Initialize(ID3D12Device* device);
  // The input target (in_width x in_height, display-encoded RGBA8) in RENDER_TARGET state, for
  // the caller to draw the image into, for an output of out_width x out_height. The targets are
  // made again when a size changes.
  bool BeginInput(ID3D12Device* device, CommandStream* list, uint32_t in_width, uint32_t in_height,
                  uint32_t out_width, uint32_t out_height, D3D12_CPU_DESCRIPTOR_HANDLE* rtv);
  // EASU from the input, then RCAS into `rtv` (in RENDER_TARGET) at (x, y). `sharpness_stops`:
  // the RCAS sharpness reduction, 0 the sharpest (Xenia's default 0.2).
  void Upscale(CommandStream* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, int32_t x, int32_t y,
               float sharpness_stops);

 private:
  static constexpr uint32_t kGenerations = 4;  // descriptor sets, so a resize never rewrites one
                                               // a recorded frame still reads
  ComPtr<ID3D12RootSignature> root_signature_;
  ComPtr<ID3D12PipelineState> easu_, rcas_;
  ComPtr<ID3D12DescriptorHeap> srv_heap_, rtv_heap_;
  uint32_t srv_stride_ = 0, rtv_stride_ = 0;
  ComPtr<ID3D12Resource> input_, output_;
  uint32_t in_width_ = 0, in_height_ = 0, out_width_ = 0, out_height_ = 0, generation_ = 0;
  struct Retired {
    ComPtr<ID3D12Resource> resource;
    uint32_t frames_left;
  };
  std::vector<Retired> retired_;

  D3D12_CPU_DESCRIPTOR_HANDLE Rtv(uint32_t i) const;
  D3D12_GPU_DESCRIPTOR_HANDLE Srv(uint32_t i) const;
};

}  // namespace pgr4::native
