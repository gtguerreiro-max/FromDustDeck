// pgr4-native PGR4 native renderer (this port): DLSS 5 before the HUD. See neural.h.
#include "neural.h"

#include "command_stream.h"
#include "guest_memory.h"
#include "log.h"
#include "upscaler.h"

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(pgr4_native_dlss5, false, "PGR4 native",
                    "NVIDIA DLSS 5 Neural Rendering of the finished image before the HUD "
                    "(nvngx_dlssnr.dll beside the executable, an RTX GPU and its driver), with "
                    "--pgr4_native_upscaler");
REXCVAR_DEFINE_DOUBLE(pgr4_native_dlss5_intensity, 1.0, "PGR4 native",
                      "DLSS 5: intensity (0-2; 1 the modding community's baseline)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_dlss5_structure, 1.0, "PGR4 native",
                      "DLSS 5: local structure strength, fine detail (0-2)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_dlss5_tone, 1.0, "PGR4 native",
                      "DLSS 5: local tone strength, light and contrast (0-2)");
REXCVAR_DEFINE_INT32(pgr4_native_dlss5_style, 2, "PGR4 native",
                     "DLSS 5: style (0 Default, the strongest; 1 Natural; 2 Cinematic)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_dlss5_scale, 1.0, "PGR4 native",
                      "DLSS 5's working scale, 0.5-1 (the community forks' WorkingScale): the model "
                      "runs on a frame this much smaller and its change is added to the full one; "
                      "its cost falls with the square");
REXCVAR_DEFINE_DOUBLE(pgr4_native_dlss5_max_ratio, 2.0, "PGR4 native",
                      "DLSS 5's change as a ratio to the game's colour, at most this much either "
                      "way per channel (the community forks' MaxRatio: 2); 0 adds the change as it "
                      "is");
REXCVAR_DEFINE_INT32(pgr4_native_dlss5_passes, 1, "PGR4 native",
                     "DLSS 5 passes, 1-3: each one reworks the previous one's result, with a "
                     "history of its own (the community's multi-pass forks); each costs as much as "
                     "the first");
REXCVAR_DEFINE_INT32(pgr4_native_dlss5_automask, 0, "PGR4 native",
                     "DLSS 5's skin auto mask: 0 off (the white control mask alone: the effect "
                     "everywhere), 1 combined with the control mask by min (RTX Remix's), 2 on "
                     "with the control mask taking precedence");
REXCVAR_DEFINE_INT32(pgr4_native_dlss5_preset, 0, "PGR4 native",
                     "DLSS 5: network preset (0 the DLL's default, 1 A, 2 B ...)");
REXCVAR_DEFINE_BOOL(pgr4_native_dlss5_compare, false, "PGR4 native",
                    "DLSS 5 comparison: evaluated every frame, its result shown in frames 2-3 of "
                    "every 4 (a capture burst takes every other frame: frames without and with "
                    "DLSS 5 at the same moment). At the scene stage the game's eye adaptation "
                    "follows the mix of both, so the frames without are not quite as the game "
                    "would expose them");
REXCVAR_DEFINE_BOOL(pgr4_native_dlss5_paused, false, "PGR4 native",
                    "DLSS 5 off for now (F8 cycles balanced, strong and off)");
REXCVAR_DEFINE_BOOL(pgr4_native_dlss5_menus, false, "PGR4 native",
                    "DLSS 5 on the front end's menus too (it greys their white backdrops); "
                    "false: only while cars are being simulated");
REXCVAR_DEFINE_STRING(pgr4_native_dlss5_skip_composites, "", "PGR4 native",
                      "DLSS 5 stays off in the frames composited by these pixel shaders "
                      "(comma-separated ucode hashes, as the log names them): the front end's "
                      "stylized menus, which it would turn grey. The front stage only "
                      "(--pgr4_native_dlss5_stage=front)");
REXCVAR_DEFINE_INT32(pgr4_native_dlss5_split, 0, "PGR4 native",
                     "DLSS 5 comparison: 0 the whole image, 1 the right half only (the left "
                     "half as the game draws it), 2 the middle half twice: as the game draws it "
                     "| with DLSS 5 (the same pixels of the same frame)");

// The time of the last car's gearbox step, steady clock ns (games/pgr4/src/pgr4_hooks.cpp).
uint64_t Pgr4LastCarStepNs();

namespace pgr4::native {

namespace {

namespace xenos = rex::graphics::xenos;

// A color target's EDRAM data: its base and storage format, which a format's _AS_16_16_16_16
// variant shares (frame_generation.cpp's, the same test).
uint32_t ColorStorage(uint32_t color_info) {
  const auto format = xenos::ColorRenderTargetFormat((color_info >> 16) & 0xF);
  return (color_info & 0xFFF) | uint32_t(xenos::GetStorageColorFormat(format)) << 16;
}

// The control mask: R the intensity, G the local tone, B the local structure (the DLSS 5
// header); with one channel G and B read 0 and cancelled the local tone and structure.
constexpr DXGI_FORMAT kMaskFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kInputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// DLSS 5's input: the display-referred composite in [0, 1] with alpha 1 (RTX Remix's input;
// the 7e3 front buffer holds up to 31.875, which the display clamps, and a 2-bit alpha), at the
// working size (bilinear: 2x2 texels at a scale of 1/2). Compose: the frame plus DLSS 5's change
// at the working size (its output - its input), upsampled. t0 the frame, t1 DLSS 5's output, t2
// its input, u0 the destination; size the destination's, uv_scale maps it onto t0's texels.
constexpr char kNeuralHlsl[] = R"(
Texture2D<float4> src : register(t0);
Texture2D<float4> nr : register(t1);
Texture2D<float4> base : register(t2);
RWTexture2D<float4> dst : register(u0);
SamplerState lin : register(s0);
cbuffer Constants : register(b0) { uint2 size; float2 uv_scale; float max_ratio; };
[numthreads(8, 8, 1)]
void input_main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
  float2 uv = (float2(p) + 0.5) / float2(size) * uv_scale;
  dst[p] = float4(saturate(src.SampleLevel(lin, uv, 0).rgb), 1.0);
}
[numthreads(8, 8, 1)]
void compose_main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
  float2 uv = (float2(p) + 0.5) / float2(size);
  float4 c = src.Load(int3(p, 0));
  float3 n = nr.SampleLevel(lin, uv, 0).rgb, b = base.SampleLevel(lin, uv, 0).rgb;
  float3 o;
  if (max_ratio > 0.0) {
    // Ratio composition (the Skynizz fork's): each channel takes DLSS 5's ratio to its input,
    // at most max_ratio either way, so a dark pixel cannot turn into a bright colour (the
    // flashes of blue seen on the player's car's dark rear window).
    float3 r = clamp((n + 0.02) / (b + 0.02), 1.0 / max_ratio, max_ratio);
    o = c.rgb * r;
  } else {
    o = c.rgb + (n - b);
  }
  dst[p] = float4(max(o, 0.0), c.a);
}
)";
// Per frame in flight: the input pass's table and the compose pass's (t0-t2, u0 each).
constexpr uint32_t kViewsPerTable = 4;
constexpr uint32_t kViewsPerFrame = 2 * kViewsPerTable;
constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// PGR4_DLSS5_SWEEP (diagnostic): "<s>:<style>:<intensity>:<structure>:<tone>,..." switches the
// settings at those seconds after the renderer started, for captures of each in one run.
struct SweepStep {
  double at;
  Dlss::NeuralSettings settings;
  int passes = 1;
  int preset = -1;    // -1: the cvar's
  int automask = -1;  // -1: the cvar's
};
// The sweep's clock: from the program's start (static initialization).
const std::chrono::steady_clock::time_point kStart = std::chrono::steady_clock::now();

std::vector<SweepStep> ParseSweep() {
  std::vector<SweepStep> steps;
  const char* env = std::getenv("PGR4_DLSS5_SWEEP");
  for (const char* p = env; p && *p;) {
    SweepStep s;
    double i = 1, st = 1, t = 1;
    int style = 2, passes = 1, preset = -1, automask = -1;
    if (std::sscanf(p, "%lf:%d:%lf:%lf:%lf:%d:%d:%d", &s.at, &style, &i, &st, &t, &passes, &preset,
                    &automask) >= 5) {
      s.preset = preset < 0 ? -1 : std::clamp(preset, 0, 7);
      s.automask = automask < 0 ? -1 : std::clamp(automask, 0, 2);
      s.passes = std::clamp(passes, 1, int(Dlss::kMaxNeuralPasses));
      s.settings.style = uint32_t(std::clamp(style, 0, 2));
      s.settings.intensity = float(std::clamp(i, 0.0, 2.0));
      s.settings.structure = float(std::clamp(st, 0.0, 2.0));
      s.settings.tone = float(std::clamp(t, 0.0, 2.0));
      steps.push_back(s);
    }
    p = std::strchr(p, ',');
    if (p) ++p;
  }
  return steps;
}

}  // namespace

bool NeuralPass::Requested() { return REXCVAR_GET(pgr4_native_dlss5); }

bool NeuralPass::CarsSimulated() {
  const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
  const uint64_t last = Pgr4LastCarStepNs();
  return last && now - last < 500'000'000ull;
}

void NeuralPass::OnSwap(uint32_t front_base) {
  if (last_resolve_.source < 4 && last_resolve_.dest_base == front_base)
    front_ = last_resolve_;
  else
    front_.reset();
}

bool NeuralPass::Skipped(uint64_t composite_ps) {
  if (!skip_parsed_) {
    skip_parsed_ = true;
    const std::string list = REXCVAR_GET(pgr4_native_dlss5_skip_composites);
    for (size_t i = 0; i < list.size();) {
      size_t j = list.find(',', i);
      if (j == std::string::npos) j = list.size();
      const std::string item = list.substr(i, j - i);
      if (!item.empty()) skip_.push_back(std::strtoull(item.c_str(), nullptr, 16));
      i = j + 1;
    }
  }
  const bool skip = std::find(skip_.begin(), skip_.end(), composite_ps) != skip_.end();
  if (std::find(seen_.begin(), seen_.end(), composite_ps) == seen_.end()) {
    seen_.push_back(composite_ps);
    REXLOG_INFO("PGR4 native DLSS 5: composite pixel shader {:016X} ({})", composite_ps,
                skip ? "skipped" : "DLSS 5 on");
  }
  return skip;
}

void NeuralPass::ReadSettings(Dlss::NeuralSettings& s, uint32_t& passes) {
  s.intensity = float(std::clamp(REXCVAR_GET(pgr4_native_dlss5_intensity), 0.0, 2.0));
  s.structure = float(std::clamp(REXCVAR_GET(pgr4_native_dlss5_structure), 0.0, 2.0));
  s.tone = float(std::clamp(REXCVAR_GET(pgr4_native_dlss5_tone), 0.0, 2.0));
  s.style = uint32_t(std::clamp(REXCVAR_GET(pgr4_native_dlss5_style), 0, 2));
  s.preset = std::clamp(REXCVAR_GET(pgr4_native_dlss5_preset), 0, 7);
  s.automask = std::clamp(REXCVAR_GET(pgr4_native_dlss5_automask), 0, 2);
  passes =
      uint32_t(std::clamp(REXCVAR_GET(pgr4_native_dlss5_passes), 1, int(Dlss::kMaxNeuralPasses)));
  static const std::vector<SweepStep> sweep = ParseSweep();
  if (sweep.empty()) return;
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - kStart).count();
  for (const SweepStep& st : sweep) {
    if (now < st.at) break;
    s.style = st.settings.style;
    s.intensity = st.settings.intensity;
    s.structure = st.settings.structure;
    s.tone = st.settings.tone;
    passes = uint32_t(st.passes);
    if (st.preset >= 0) s.preset = st.preset;
    if (st.automask >= 0) s.automask = st.automask;
  }
  if (!(s == sweep_settings_) || passes != sweep_passes_) {
    REXLOG_INFO("PGR4 native DLSS 5 sweep at {:.1f} s: style {} intensity {} structure {} tone {} "
                "passes {} preset {} automask {}", now, s.style, s.intensity, s.structure, s.tone,
                passes, s.preset, s.automask);
    sweep_settings_ = s;
    sweep_passes_ = passes;
  }
}

bool NeuralPass::BeforeDraw(CommandStream* list, uint64_t frame, const uint32_t* regs,
                            RenderTargets& targets, Upscaler* upscaler, uint64_t ps_hash) {
  if (SceneStage()) return false;  // the scene stage runs in Upscaler::Upscale
  if (!front_ || done_frame_ == frame || !upscaler || !upscaler->upscaled_this_frame())
    [[likely]]
    return false;
  using enum rex::graphics::Register;
  const SurfaceRegs& f = front_->regs;
  // RT 0 of the draw is the front buffer's surface (pitch, MSAA, EDRAM data), written.
  if (regs[XE_GPU_REG_RB_SURFACE_INFO] != f.surface_info ||
      ColorStorage(regs[XE_GPU_REG_RB_COLOR_INFO]) != ColorStorage(f.color_info[front_->source]) ||
      !(regs[XE_GPU_REG_RB_COLOR_MASK] & 0x7))
    return false;
  // RB_BLENDCONTROL0's color part: source ONE, ADD, destination ZERO is the opaque composite.
  if ((regs[XE_GPU_REG_RB_BLENDCONTROL0] & 0x1FFF) == 0x0001) {
    composited_frame_ = frame;
    composite_ps_ = ps_hash;
    return false;
  }
  if (composited_frame_ != frame) return false;
  done_frame_ = frame;
  if (Skipped(composite_ps_)) return false;
  // The front end's stylized menus (silhouettes on white) turn grey through DLSS 5: it runs
  // while cars are being simulated (a race, a replay), or everywhere with
  // --pgr4_native_dlss5_menus.
  const bool racing = CarsSimulated();
  if (racing != racing_) {
    racing_ = racing;
    REXLOG_INFO("PGR4 native DLSS 5: {} at frame {}", racing ? "the player drives, on" : "no player driving (front end), off",
                frame);
  }
  if (!racing && !REXCVAR_GET(pgr4_native_dlss5_menus)) return false;
  if (REXCVAR_GET(pgr4_native_dlss5_paused)) return false;
  return Run(list, frame, targets, *upscaler);
}

bool NeuralPass::CreateTargets(uint32_t width, uint32_t height, uint32_t mask_width,
                               uint32_t mask_height, uint32_t full_width, uint32_t full_height) {
  if (output_ && width == width_ && height == height_ && mask_width == mask_width_ &&
      mask_height == mask_height_ && full_width == full_width_ && full_height == full_height_)
    return true;
  output_.Reset();
  mask_.Reset();
  original_.Reset();
  input_.Reset();
  work_.Reset();
  composed_.Reset();
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = width;
  d.Height = height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = format_;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&output_)),
             "DLSS 5 output"))
    return false;
  output_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  d.Format = kInputFormat;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&input_)),
             "DLSS 5 input"))
    return false;
  input_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&work_)),
             "DLSS 5 second pass"))
    return false;
  work_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  {
    // The composed frame (compose_main), also at the full working scale.
    d.Width = full_width;
    d.Height = full_height;
    d.Format = format_;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&composed_)),
               "DLSS 5 composed frame"))
      return false;
    composed_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  d.Width = mask_width;
  d.Height = mask_height;
  d.Format = kMaskFormat;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE white = {};
  white.Format = kMaskFormat;
  white.Color[0] = white.Color[1] = white.Color[2] = white.Color[3] = 1.0f;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET, &white,
                                              IID_PPV_ARGS(&mask_)),
             "DLSS 5 control mask"))
    return false;
  if (!mask_rtv_) {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    if (Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&mask_rtv_)), "DLSS 5 RTV heap"))
      return false;
  }
  device_->CreateRenderTargetView(mask_.Get(), nullptr,
                                  mask_rtv_->GetCPUDescriptorHandleForHeapStart());
  mask_cleared_ = false;
  width_ = width;
  height_ = height;
  mask_width_ = mask_width;
  mask_height_ = mask_height;
  full_width_ = full_width;
  full_height_ = full_height;
  return true;
}

bool NeuralPass::CreateInputPass() {
  if (input_pso_) return true;
  const D3D12_DESCRIPTOR_RANGE ranges[2] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3},
  };
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {2, ranges};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 5};
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, err;
  const D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                         kViewsPerFrame * D3D12Device::kFramesInFlight,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
             "DLSS 5 root signature serialize") ||
      Failed(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&input_signature_)),
             "DLSS 5 root signature") ||
      Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&input_heap_)), "DLSS 5 heap"))
    return false;
  input_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const pD3DCompile compile = D3DCompiler();
  constexpr char kName[] = "pgr4_dlss5.hlsl";
  for (const char* entry : {"input_main", "compose_main"}) {
    ComPtr<ID3DBlob> code, error;
    if (!compile ||
        FAILED(compile(kNeuralHlsl, sizeof(kNeuralHlsl) - 1, kName, nullptr, nullptr, entry,
                       "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) {
      REXLOG_ERROR("PGR4 native DLSS 5: {} {}: {}", kName, entry,
                   error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
      return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = input_signature_.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ComPtr<ID3D12PipelineState>& pso = entry[0] == 'i' ? input_pso_ : compose_pso_;
    if (Failed(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), kName)) return false;
  }
  return true;
}

void NeuralPass::Dispatch(CommandStream* list, uint64_t frame, uint32_t table,
                          ID3D12PipelineState* pso, ID3D12Resource* const srvs[3],
                          ID3D12Resource* uav, DXGI_FORMAT uav_format, uint32_t width,
                          uint32_t height, float uv_scale_x, float uv_scale_y) {
  // In the frame's own views: those of a frame kFramesInFlight back are free.
  const uint32_t first =
      uint32_t(frame % D3D12Device::kFramesInFlight) * kViewsPerFrame + table * kViewsPerTable;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = input_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(first) * input_stride_;
  for (uint32_t i = 0; i < 3; ++i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = srvs[i] ? srvs[i]->GetDesc().Format : kInputFormat;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(srvs[i], &sd, cpu);
    cpu.ptr += input_stride_;
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = uav_format;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(uav, nullptr, &ud, cpu);
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = input_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(first) * input_stride_;
  struct {
    uint32_t size[2];
    float uv_scale[2];
    float max_ratio;
  } constants = {{width, height},
                 {uv_scale_x, uv_scale_y},
                 float(std::clamp(REXCVAR_GET(pgr4_native_dlss5_max_ratio), 0.0, 16.0))};
  ID3D12DescriptorHeap* heaps[] = {input_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(input_signature_.Get());
  list->SetPipelineState(pso);
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRoot32BitConstants(1, 5, &constants, 0);
  list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

void NeuralPass::PrepareInput(CommandStream* list, uint64_t frame, ID3D12Resource* color,
                              uint32_t frame_width, uint32_t frame_height, uint32_t width,
                              uint32_t height) {
  if (input_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    const D3D12_RESOURCE_BARRIER b =
        TransitionBarrier(input_.Get(), input_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &b);
  }
  const D3D12_RESOURCE_DESC cd = color->GetDesc();
  ID3D12Resource* const srvs[3] = {color, nullptr, nullptr};
  Dispatch(list, frame, 0, input_pso_.Get(), srvs, input_.Get(), kInputFormat, width, height,
           float(frame_width) / float(cd.Width), float(frame_height) / float(cd.Height));
  const D3D12_RESOURCE_BARRIER b =
      TransitionBarrier(input_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
  list->ResourceBarrier(1, &b);
  input_state_ = kRead;
}

bool NeuralPass::Run(CommandStream* list, uint64_t frame, RenderTargets& targets,
                     Upscaler& upscaler) {
  Dlss& dlss = upscaler.dlss();
  if (!dlss.neural_available()) {
    if (!warned_)
      REXLOG_WARN("PGR4 native DLSS 5: not available on this GPU, driver or DLL; the image "
                  "stays as the game draws it");
    warned_ = true;
    return false;
  }
  const ResolveParams& fp = *front_;
  if (fp.regs.color_info[fp.source & 3] != logged_color_info_ || fp.dest_swap != logged_swap_ ||
      fp.dest_format != logged_dest_format_ || fp.exp_bias != logged_exp_bias_) {
    logged_color_info_ = fp.regs.color_info[fp.source & 3];
    logged_swap_ = fp.dest_swap;
    logged_dest_format_ = fp.dest_format;
    logged_exp_bias_ = fp.exp_bias;
    REXLOG_INFO("PGR4 native DLSS 5: front buffer at frame {}: color info {:08X} (format {}), "
                "surface {:08X}, {}x{}, to texture format {} number {} swap {} exp bias {}",
                frame, logged_color_info_, (logged_color_info_ >> 16) & 0xF, fp.regs.surface_info,
                fp.x1 - fp.x0, fp.y1 - fp.y0, fp.dest_format, fp.dest_number, fp.dest_swap ? 1 : 0,
                fp.exp_bias);
  }
  uint32_t width = uint32_t(std::max(0, fp.x1 - fp.x0));
  uint32_t height = uint32_t(std::max(0, fp.y1 - fp.y0));
  RenderTarget* color = targets.GetColor(fp.regs, fp.source, uint32_t(fp.y1));
  if (!color || color->samples != 1 || fp.x0 != 0 || fp.y0 != 0 || !width || !height ||
      color->resource_format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
    PGR4_WARN_ONCE("PGR4 native DLSS 5: unexpected front buffer ({}x{} at {},{}, {} samples)",
                   width, height, fp.x0, fp.y0, color ? color->samples : 0);
    return false;
  }
  const Upscaler::SceneInputs in = upscaler.ReadSceneInputs(list);
  if (!in.depth || !in.motion) return false;
  format_ = color->resource_format;
  // The front buffer's host pixels (the resolution scale's); the rectangle read is in guest ones.
  const D3D12_RECT all = {0, 0, LONG(width), LONG(height)};
  const uint32_t guest_width = width, guest_height = height;
  width = color->scale.Extent(guest_width, 0);
  height = color->scale.Extent(guest_height, 1);
  // DLSS 5's working size (--pgr4_native_dlss5_scale); the comparison split needs the full one.
  double work_scale = std::clamp(REXCVAR_GET(pgr4_native_dlss5_scale), 0.5, 1.0);
  if (REXCVAR_GET(pgr4_native_dlss5_split) == 2) work_scale = 1.0;
  const uint32_t full_width = width, full_height = height;
  width = std::max(8u, uint32_t(std::lround(full_width * work_scale)));
  height = std::max(8u, uint32_t(std::lround(full_height * work_scale)));
  if (work_scale > 0.999) width = full_width, height = full_height;
  if (!CreateTargets(width, height, in.width, in.height, full_width, full_height) ||
      !CreateInputPass())
    return false;

  targets.PrepareRead(list, color, all);
  RenderTargets::Transition(list, color, kRead);
  D3D12_RESOURCE_BARRIER bs[2];
  uint32_t nb = 0;
  if (output_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    bs[nb++] = TransitionBarrier(output_.Get(), output_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    output_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  if (!mask_cleared_) {
    const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    list->ClearRenderTargetView(mask_rtv_->GetCPUDescriptorHandleForHeapStart(), one, 0, nullptr);
    bs[nb++] = TransitionBarrier(mask_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
    mask_cleared_ = true;
  }
  if (nb) list->ResourceBarrier(nb, bs);

  D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  const uint32_t quarter = width / 4;
  if (REXCVAR_GET(pgr4_native_dlss5_split) == 2) {
    // The input's middle half, as the game drew it, for the left half afterwards.
    if (!original_) {
      D3D12_HEAP_PROPERTIES hp = {};
      hp.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC d = output_->GetDesc();
      d.Flags = D3D12_RESOURCE_FLAG_NONE;
      if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&original_)),
                 "DLSS 5 comparison copy"))
        return false;
    }
    RenderTargets::Transition(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE);
    dst.pResource = original_.Get();
    src.pResource = color->resource.Get();
    const D3D12_BOX middle = {quarter, 0, 0, width - quarter, height, 1};
    list->CopyTextureRegion(&dst, quarter, 0, 0, &src, &middle);
    RenderTargets::Transition(list, color, kRead);
    const D3D12_RESOURCE_BARRIER b = TransitionBarrier(
        original_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b);
  }

  PrepareInput(list, frame, color->resource.Get(), full_width, full_height, width, height);

  Dlss::NeuralFrame nf;
  nf.color = input_.Get();
  nf.output = output_.Get();
  nf.depth = in.depth;
  nf.motion = in.motion;
  nf.mask = mask_.Get();
  nf.width = width;
  nf.height = height;
  nf.render_width = in.width;
  nf.render_height = in.height;
  nf.jitter_x = in.jitter_x;
  nf.jitter_y = in.jitter_y;
  nf.motion_scale_x = float(in.width);
  nf.motion_scale_y = float(in.height);
  nf.reset = in.reset || last_run_frame_ + 1 != frame;
  nf.settings.intensity = float(std::clamp(REXCVAR_GET(pgr4_native_dlss5_intensity), 0.0, 2.0));
  nf.settings.structure = float(std::clamp(REXCVAR_GET(pgr4_native_dlss5_structure), 0.0, 2.0));
  nf.settings.tone = float(std::clamp(REXCVAR_GET(pgr4_native_dlss5_tone), 0.0, 2.0));
  nf.settings.style = uint32_t(std::clamp(REXCVAR_GET(pgr4_native_dlss5_style), 0, 2));
  nf.settings.preset = std::clamp(REXCVAR_GET(pgr4_native_dlss5_preset), 0, 7);
  uint32_t passes =
      uint32_t(std::clamp(REXCVAR_GET(pgr4_native_dlss5_passes), 1, int(Dlss::kMaxNeuralPasses)));
  static const std::vector<SweepStep> sweep = ParseSweep();
  if (!sweep.empty()) {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - kStart).count();
    for (const SweepStep& s : sweep) {
      if (now < s.at) break;
      nf.settings.style = s.settings.style;
      nf.settings.intensity = s.settings.intensity;
      nf.settings.structure = s.settings.structure;
      nf.settings.tone = s.settings.tone;
      passes = uint32_t(s.passes);
    }
    if (!(nf.settings == sweep_settings_) || passes != sweep_passes_) {
      REXLOG_INFO("PGR4 native DLSS 5 sweep at {:.1f} s: style {} intensity {} structure {} tone {} "
                  "passes {}", now, nf.settings.style, nf.settings.intensity, nf.settings.structure,
                  nf.settings.tone, passes);
      sweep_settings_ = nf.settings;
      sweep_passes_ = passes;
    }
  }
  const int32_t split = REXCVAR_GET(pgr4_native_dlss5_split);
  last_run_frame_ = frame;
  // Pass 1 reads input_ into output_, pass 2 output_ into work_, pass 3 work_ into output_:
  // input_ stays the model's input for the composition. On the stream's thread, which records
  // the list: a failure leaves the output as it was, and is logged (Dlss::EvaluateNeural).
  ID3D12Resource* result = output_.Get();
  for (uint32_t pass = 0; pass < passes; ++pass) {
    ID3D12Resource* read = pass == 0 ? input_.Get() : pass == 1 ? output_.Get() : work_.Get();
    ID3D12Resource* write = pass == 1 ? work_.Get() : output_.Get();
    D3D12_RESOURCE_STATES& read_state =
        pass == 0 ? input_state_ : pass == 1 ? output_state_ : work_state_;
    D3D12_RESOURCE_STATES& write_state = pass == 1 ? work_state_ : output_state_;
    nb = 0;
    if (read_state != kRead) {
      bs[nb++] = TransitionBarrier(read, read_state, kRead);
      read_state = kRead;
    }
    if (write_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      bs[nb++] = TransitionBarrier(write, write_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      write_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (nb) list->ResourceBarrier(nb, bs);
    Dlss::NeuralFrame pf = nf;
    pf.color = read;
    pf.output = write;
    list->Call([&dlss, pf, pass](CommandStream::List* l) { dlss.EvaluateNeural(l, pf, pass); });
    result = write;
  }
  D3D12_RESOURCE_STATES* result_state = result == output_.Get() ? &output_state_ : &work_state_;
  if (composed_) {
    // The full frame plus the model's change at the working size.
    nb = 0;
    bs[nb++] = TransitionBarrier(result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    *result_state = kRead;
    if (composed_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
      bs[nb++] = TransitionBarrier(composed_.Get(), composed_state_,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    composed_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(nb, bs);
    ID3D12Resource* const srvs[3] = {color->resource.Get(), result, input_.Get()};
    Dispatch(list, frame, 1, compose_pso_.Get(), srvs, composed_.Get(), format_, full_width,
             full_height, 1.0f, 1.0f);
    result = composed_.Get();
    result_state = &composed_state_;
  }
  width = full_width;
  height = full_height;

  // The new image over the front target, where the HUD is drawn next (the left half kept as it
  // was with the comparison split).
  bs[0] = TransitionBarrier(result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
  *result_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, bs);
  RenderTargets::Transition(list, color, D3D12_RESOURCE_STATE_COPY_DEST);
  dst.pResource = color->resource.Get();
  src.pResource = result;
  if (REXCVAR_GET(pgr4_native_dlss5_compare) && !((frame >> 1) & 1)) {
    // Even frames as the game draws them (DLSS 5 evaluated, for its history).
    return true;
  }
  if (split == 2 && original_) {
    // Before | after: the middle half as the game drew it on the left, with DLSS 5 on the right.
    const D3D12_BOX middle = {quarter, 0, 0, width - quarter, height, 1};
    list->CopyTextureRegion(&dst, width / 2, 0, 0, &src, &middle);
    src.pResource = original_.Get();
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &middle);
    const D3D12_RESOURCE_BARRIER b = TransitionBarrier(
        original_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &b);
    return true;
  }
  const uint32_t left = split == 1 ? width / 2 : 0;
  const D3D12_BOX box = {left, 0, 0, width, height, 1};
  list->CopyTextureRegion(&dst, left, 0, 0, &src, &box);
  return true;
}

}  // namespace pgr4::native
