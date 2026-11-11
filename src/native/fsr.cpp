// FSR 1 for the present. See fsr.h. The root signature and the draws follow ReXGlue's
// src/ui/d3d12/d3d12_presenter.cpp (guest output painting): the source texture's table, the
// rectangle's constants for the vertex shader and the effect's constants for the pixel shader,
// both at b0, a bilinear clamp sampler at s0, a 4-vertex triangle strip.
#include "fsr.h"

#include "command_stream.h"

#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace pgr4::native {

namespace shaders {
#include "d3d12_5_1/guest_output_ffx_fsr_easu_ps.h"
#include "d3d12_5_1/guest_output_ffx_fsr_rcas_ps.h"
#include "d3d12_5_1/guest_output_triangle_strip_rect_vs.h"
}  // namespace shaders

namespace {

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
static_assert(D3D12Device::kBackBufferFormat == kFormat);

// The rectangle filling the viewport, in Xenia's convention (+Y is -V).
constexpr float kFullRect[4] = {-1.0f, 1.0f, 2.0f, -2.0f};

}  // namespace

bool Fsr1::Initialize(ID3D12Device* device) {
  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[3] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 4};  // the rectangle: offset, size
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[2].Constants = {0, 0, 4};  // EASU's or RCAS's
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rs = {3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, err;
  if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
             "FSR root signature serialize") ||
      Failed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature_)),
             "FSR root signature"))
    return false;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = root_signature_.Get();
  d.VS = {shaders::guest_output_triangle_strip_rect_vs,
          sizeof(shaders::guest_output_triangle_strip_rect_vs)};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = kFormat;
  d.SampleDesc.Count = 1;
  d.PS = {shaders::guest_output_ffx_fsr_easu_ps, sizeof(shaders::guest_output_ffx_fsr_easu_ps)};
  if (Failed(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&easu_)), "FSR EASU pipeline"))
    return false;
  d.PS = {shaders::guest_output_ffx_fsr_rcas_ps, sizeof(shaders::guest_output_ffx_fsr_rcas_ps)};
  if (Failed(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&rcas_)), "FSR RCAS pipeline"))
    return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2 * kGenerations,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap_)), "FSR SRV heap"))
    return false;
  hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 * kGenerations, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)), "FSR RTV heap"))
    return false;
  srv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  rtv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE Fsr1::Rtv(uint32_t i) const {
  D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  h.ptr += size_t((generation_ % kGenerations) * 2 + i) * rtv_stride_;
  return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Fsr1::Srv(uint32_t i) const {
  D3D12_GPU_DESCRIPTOR_HANDLE h = srv_heap_->GetGPUDescriptorHandleForHeapStart();
  h.ptr += uint64_t((generation_ % kGenerations) * 2 + i) * srv_stride_;
  return h;
}

bool Fsr1::BeginInput(ID3D12Device* device, CommandStream* list, uint32_t in_width,
                      uint32_t in_height, uint32_t out_width, uint32_t out_height,
                      D3D12_CPU_DESCRIPTOR_HANDLE* rtv) {
  // Resources replaced by a resize live on until no recorded frame can use them.
  std::erase_if(retired_, [](Retired& r) { return r.frames_left-- == 0; });
  if (!input_ || in_width != in_width_ || in_height != in_height_ || out_width != out_width_ ||
      out_height != out_height_) {
    if (input_) retired_.push_back({std::move(input_), 8});
    if (output_) retired_.push_back({std::move(output_), 8});
    ++generation_;
    const auto make = [&](uint32_t w, uint32_t h, uint32_t index, ComPtr<ID3D12Resource>* out) {
      D3D12_RESOURCE_DESC desc = {};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = w;
      desc.Height = h;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = kFormat;
      desc.SampleDesc.Count = 1;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
      if (Failed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 nullptr, IID_PPV_ARGS(out->ReleaseAndGetAddressOf())),
                 "FSR target"))
        return false;
      device->CreateRenderTargetView(out->Get(), nullptr, Rtv(index));
      D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = kFormat;
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      sd.Texture2D.MipLevels = 1;
      D3D12_CPU_DESCRIPTOR_HANDLE cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
      cpu.ptr += size_t((generation_ % kGenerations) * 2 + index) * srv_stride_;
      device->CreateShaderResourceView(out->Get(), &sd, cpu);
      return true;
    };
    if (!make(in_width, in_height, 0, &input_) || !make(out_width, out_height, 1, &output_)) {
      input_.Reset();
      output_.Reset();
      in_width_ = 0;
      return false;
    }
    in_width_ = in_width;
    in_height_ = in_height;
    out_width_ = out_width;
    out_height_ = out_height;
    REXLOG_INFO("PGR4 native present: FSR 1 from {}x{} to {}x{}", in_width, in_height, out_width,
                out_height);
  }
  D3D12_RESOURCE_BARRIER b = TransitionBarrier(input_.Get(),
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET);
  list->ResourceBarrier(1, &b);
  *rtv = Rtv(0);
  return true;
}

void Fsr1::Upscale(CommandStream* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, int32_t x, int32_t y,
                   float sharpness_stops) {
  D3D12_RESOURCE_BARRIER b[2] = {
      TransitionBarrier(input_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
      TransitionBarrier(output_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_RENDER_TARGET)};
  list->ResourceBarrier(2, b);
  ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetGraphicsRootSignature(root_signature_.Get());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  list->SetGraphicsRoot32BitConstants(1, 4, kFullRect, 0);

  // EASU: the input to the output's size (FsrEasuConstants: the size ratio, 1 / input size).
  const float easu[4] = {float(in_width_) / float(out_width_), float(in_height_) / float(out_height_),
                         1.0f / float(in_width_), 1.0f / float(in_height_)};
  const D3D12_VIEWPORT easu_vp = {0.0f, 0.0f, float(out_width_), float(out_height_), 0.0f, 1.0f};
  const D3D12_RECT easu_sc = {0, 0, LONG(out_width_), LONG(out_height_)};
  const D3D12_CPU_DESCRIPTOR_HANDLE output_rtv = Rtv(1);
  list->SetPipelineState(easu_.Get());
  list->SetGraphicsRootDescriptorTable(0, Srv(0));
  list->SetGraphicsRoot32BitConstants(2, 4, easu, 0);
  list->RSSetViewports(1, &easu_vp);
  list->RSSetScissorRects(1, &easu_sc);
  list->OMSetRenderTargets(1, &output_rtv, FALSE, nullptr);
  list->DrawInstanced(4, 1, 0, 0);

  b[1] = TransitionBarrier(output_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  list->ResourceBarrier(1, &b[1]);

  // RCAS into the target at (x, y) (FsrRcasConstants: the output offset, exp2(-stops)).
  struct {
    int32_t offset[2];
    float sharpness;
    float unused;
  } rcas = {{x, y}, std::exp2(-std::max(sharpness_stops, 0.0f)), 0.0f};
  const D3D12_VIEWPORT rcas_vp = {float(x), float(y), float(out_width_), float(out_height_),
                                  0.0f, 1.0f};
  const D3D12_RECT rcas_sc = {LONG(x), LONG(y), LONG(x + int32_t(out_width_)),
                              LONG(y + int32_t(out_height_))};
  list->SetPipelineState(rcas_.Get());
  list->SetGraphicsRootDescriptorTable(0, Srv(1));
  list->SetGraphicsRoot32BitConstants(2, 4, &rcas, 0);
  list->RSSetViewports(1, &rcas_vp);
  list->RSSetScissorRects(1, &rcas_sc);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->DrawInstanced(4, 1, 0, 0);
}

}  // namespace pgr4::native
