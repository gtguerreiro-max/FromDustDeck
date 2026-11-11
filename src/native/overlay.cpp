// The settings menu's overlay. See overlay.h. The pipeline is Dear ImGui's D3D12 backend's
// (imgui_impl_dx12.cpp: alpha blending, no depth, scissor per draw), recorded on the command
// stream instead of a list of its own.
#include "overlay.h"

#include "command_stream.h"

#include <rex/logging.h>

#include <algorithm>
#include <cstring>

namespace pgr4::native {

namespace {

const char kOverlayHlsl[] = R"(
cbuffer view : register(b0) { float2 scale; float2 offset; };
Texture2D<float4> atlas : register(t0);
SamplerState linear_clamp : register(s0);
struct VsOut { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; };
VsOut vs(float2 pos : POSITION, float2 uv : TEXCOORD0, float4 col : COLOR0) {
  VsOut o;
  o.pos = float4(pos * scale + offset, 0.0, 1.0);
  o.col = col;
  o.uv = uv;
  return o;
}
float4 ps(VsOut i) : SV_Target { return i.col * atlas.Sample(linear_clamp, i.uv); }
)";

constexpr uint64_t Align(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

}  // namespace

bool Overlay::Initialize(ID3D12Device* device) {
  const pD3DCompile compile = D3DCompiler();
  if (!compile) return false;
  ComPtr<ID3DBlob> vs, ps, err;
  if (FAILED(compile(kOverlayHlsl, sizeof(kOverlayHlsl) - 1, "overlay", nullptr, nullptr, "vs",
                     "vs_5_1", 0, 0, &vs, &err)) ||
      FAILED(compile(kOverlayHlsl, sizeof(kOverlayHlsl) - 1, "overlay", nullptr, nullptr, "ps",
                     "ps_5_1", 0, 0, &ps, &err))) {
    REXLOG_ERROR("Native overlay: shader: {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return false;
  }
  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants = {0, 0, 4};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable = {1, &range};
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 1, &sampler,
                                  D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ComPtr<ID3DBlob> blob;
  if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
             "overlay root signature serialize") ||
      Failed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature_)),
             "overlay root signature"))
    return false;
  static const D3D12_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(OverlayVertex, x),
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(OverlayVertex, u),
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(OverlayVertex, color),
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = root_signature_.Get();
  d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  D3D12_RENDER_TARGET_BLEND_DESC& blend = d.BlendState.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
  blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  blend.BlendOp = D3D12_BLEND_OP_ADD;
  blend.SrcBlendAlpha = D3D12_BLEND_ONE;
  blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.InputLayout = {layout, UINT(std::size(layout))};
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = D3D12Device::kBackBufferFormat;
  d.SampleDesc.Count = 1;
  if (Failed(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso_)), "overlay pipeline"))
    return false;
  d.RTVFormats[0] = D3D12Device::kHdrBackBufferFormat;
  if (Failed(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&hdr_pso_)),
             "overlay HDR pipeline"))
    return false;
  // Two atlas views, used in turn, so a new atlas never rewrites the view a frame in flight reads.
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)), "overlay heap")) return false;
  heap_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return true;
}

bool Overlay::UploadFont(ID3D12Device* device, CommandStream* list, const OverlayFont& font) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = font.width;
  desc.Height = font.height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> texture, upload;
  if (Failed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&texture)),
             "overlay font texture"))
    return false;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 total = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
  void* p = nullptr;
  const D3D12_RANGE none = {0, 0};
  if (!CreateBuffer(device, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ,
                    "overlay font upload", &upload) ||
      Failed(upload->Map(0, &none, &p), "overlay font upload map"))
    return false;
  for (uint32_t y = 0; y < font.height; ++y)
    std::memcpy(static_cast<uint8_t*>(p) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                font.rgba.data() + size_t(y) * font.width * 4, size_t(font.width) * 4);
  upload->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
  dst.pResource = texture.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.pResource = upload.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = footprint;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  D3D12_RESOURCE_BARRIER b = TransitionBarrier(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  list->ResourceBarrier(1, &b);
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = desc.Format;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  // The old atlas may still be read by a frame in flight: it is only replaced when ImGui
  // rebuilds the atlas, which the menu does once, so the old one is simply kept until then.
  static std::vector<ComPtr<ID3D12Resource>> retired;
  if (font_) retired.push_back(font_);
  if (font_upload_) retired.push_back(font_upload_);
  font_view_ ^= 1;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(font_view_) * heap_stride_;
  device->CreateShaderResourceView(texture.Get(), &sd, cpu);
  font_ = std::move(texture);
  font_upload_ = std::move(upload);
  font_version_ = font.version;
  return true;
}

void Overlay::Record(ID3D12Device* device, CommandStream* list, uint32_t slot,
                     D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height, bool hdr,
                     const OverlayFrame& frame) {
  if (!pso_ || frame.draws.empty() || !width || !height) return;
  {
    OverlayFont font;
    if (OverlayFontIfChanged(font_version_, &font) && font.width && font.height &&
        !UploadFont(device, list, font))
      return;
  }
  if (!font_) return;
  // Vertices and indices into this slot's upload buffer (its previous frame has completed).
  const uint64_t vertex_bytes = frame.vertices.size() * sizeof(OverlayVertex);
  const uint64_t index_offset = Align(vertex_bytes, 256);
  const uint64_t bytes = index_offset + frame.indices.size() * sizeof(uint16_t);
  Slot& s = slots_[slot % D3D12Device::kFramesInFlight];
  if (s.size < bytes) {
    if (s.buffer) s.buffer->Unmap(0, nullptr);
    s = {};
    const uint64_t size = Align(bytes + bytes / 2, 64 * 1024);
    void* p = nullptr;
    const D3D12_RANGE none = {0, 0};
    if (!CreateBuffer(device, D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ,
                      "overlay vertices", &s.buffer) ||
        Failed(s.buffer->Map(0, &none, &p), "overlay vertices map")) {
      s = {};
      return;
    }
    s.mapped = static_cast<uint8_t*>(p);
    s.size = size;
  }
  std::memcpy(s.mapped, frame.vertices.data(), vertex_bytes);
  std::memcpy(s.mapped + index_offset, frame.indices.data(),
              frame.indices.size() * sizeof(uint16_t));
  const D3D12_GPU_VIRTUAL_ADDRESS gpu = s.buffer->GetGPUVirtualAddress();
  const D3D12_VERTEX_BUFFER_VIEW vbv = {gpu, UINT(vertex_bytes), sizeof(OverlayVertex)};
  const D3D12_INDEX_BUFFER_VIEW ibv = {gpu + index_offset,
                                       UINT(frame.indices.size() * sizeof(uint16_t)),
                                       DXGI_FORMAT_R16_UINT};

  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetGraphicsRootSignature(root_signature_.Get());
  list->SetPipelineState(hdr ? hdr_pso_.Get() : pso_.Get());
  // Pixels to clip space.
  const float view[4] = {2.0f / float(width), -2.0f / float(height), -1.0f, 1.0f};
  list->SetGraphicsRoot32BitConstants(0, 4, view, 0);
  D3D12_GPU_DESCRIPTOR_HANDLE atlas = heap_->GetGPUDescriptorHandleForHeapStart();
  atlas.ptr += uint64_t(font_view_) * heap_stride_;
  list->SetGraphicsRootDescriptorTable(1, atlas);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT vp = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  list->RSSetViewports(1, &vp);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->IASetIndexBuffer(&ibv);
  list->Call([vbv](ID3D12GraphicsCommandList* l) { l->IASetVertexBuffers(0, 1, &vbv); });
  for (const OverlayDraw& draw : frame.draws) {
    const D3D12_RECT clip = {std::max(draw.clip[0], 0), std::max(draw.clip[1], 0),
                             std::min(draw.clip[2], int32_t(width)),
                             std::min(draw.clip[3], int32_t(height))};
    if (clip.right <= clip.left || clip.bottom <= clip.top || !draw.count) continue;
    list->RSSetScissorRects(1, &clip);
    list->DrawIndexedInstanced(draw.count, 1, draw.first_index, INT(draw.first_vertex), 0);
  }
}

}  // namespace pgr4::native
