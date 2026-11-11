// pgr4-native PGR4 native renderer: the scene's upscaler. See upscaler.h. In this port it runs
// NVIDIA DLSS / DLAA (dlss.h) in place of FidelityFX Super Resolution.
#include "upscaler.h"

#include "clock.h"
#include "frame_generation.h"
#include "log.h"
#include "neural.h"

#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <string>

REXCVAR_DEFINE_BOOL(pgr4_native_upscaler, false, "PGR4 native",
                    "Render the 3D scene without MSAA and antialias it with NVIDIA DLSS "
                    "(--pgr4_native_dlss_mode; nvngx_dlss.dll beside the executable, an RTX GPU) "
                    "from the start; bind_upscaler (F7) switches it");
REXCVAR_DEFINE_INT32(pgr4_native_upscaler_width, 1280, "PGR4 native",
                     "Width the upscaler's scene renders at, upscaled to 1280 (320-1280; 1280 "
                     "with the height at the render height: antialiasing only)");
REXCVAR_DEFINE_INT32(pgr4_native_upscaler_height, 800, "PGR4 native",
                     "Height the upscaler's scene renders at, upscaled to the render height "
                     "(--pgr4_render_height, 800 or 720; from a quarter of it up to it)");
REXCVAR_DEFINE_INT32(pgr4_native_upscaler_target_fps, 0, "PGR4 native",
                     "Vary the upscaler's render size each frame, from the width and height "
                     "down to --pgr4_native_upscaler_min_height, so that the GPU keeps up with "
                     "this frame rate, or with the lower one the frames are shown at "
                     "(--pgr4_native_present_interval, frame generation; 0: a fixed size)");
REXCVAR_DEFINE_INT32(pgr4_native_upscaler_min_height, 360, "PGR4 native",
                     "With --pgr4_native_upscaler_target_fps: the least height the scene renders "
                     "at (from a quarter of the render height up to it)");

REXCVAR_DEFINE_STRING(pgr4_native_dlss_mode, "dlaa", "PGR4 native",
                      "With --pgr4_native_upscaler: dlaa (the game's resolution, anti-aliased by DLSS), "
                      "quality, balanced, performance or ultra (DLSS Super Resolution from a smaller "
                      "render size)");
REXCVAR_DEFINE_INT32(pgr4_native_dlss_preset, 0, "PGR4 native",
                     "DLSS render preset (NVSDK_NGX_DLSS_Hint_Render_Preset_*: 0 the DLL's default, "
                     "10 J, 11 K, ...)");

namespace pgr4::native {

namespace {

constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr D3D12_RESOURCE_STATES kInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// The scene's color for the upscaler in the range of the 7e3 EDRAM format, as the resolves read it: RGB in
// [0, 31.875], alpha in [0, 1], NaN 0 (resolve.cpp's Quantize7e3, Xenia's UnclampedFloat32To7e3).
// The host target is R16G16B16A16_FLOAT and keeps what the shaders write beyond that, such as
// the start lights' emissive colors above 31.875. Its root signature: the source's SRV and the
// destination's UAV in a table, then the size.
constexpr char kInputHlsl[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Constants : register(b0) { uint2 size; };
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
  float4 c = src.Load(int3(p, 0));
  dst[p] = float4(clamp(c.rgb, 0.0, 31.875), saturate(c.a));
}
)";
// Per frame in flight: the input pass's SRV and UAV.
constexpr uint32_t kViewsPerFrame = 2;

// The render size of one axis from its setting, at least a quarter of the scene's.
uint32_t RenderExtent(int32_t setting, uint32_t scene) {
  return uint32_t(std::clamp<int32_t>(setting, int32_t(scene / 4), int32_t(scene)));
}

}  // namespace

bool Upscaler::Requested() { return REXCVAR_GET(pgr4_native_upscaler); }

Upscaler::~Upscaler() {
  if (context_) api_->destroy(&context_, nullptr);
}

bool Upscaler::Initialize(ID3D12Device* device) {
  device_ = device;
  // This port: DLSS. The mode sets the render size (DLAA: the game's own); no dynamic resolution.
  mode_ = Dlss::ModeFromName(REXCVAR_GET(pgr4_native_dlss_mode));
  const double scale = Dlss::RenderScale(mode_);
  max_width_ = RenderExtent(int32_t(std::lround(OutputWidth() * scale)), OutputWidth());
  max_height_ = RenderExtent(int32_t(std::lround(OutputHeight() * scale)), OutputHeight());
  if (!dlss_.Initialize(device_) || !CreateContext(false)) return false;
  REXLOG_INFO("PGR4 native upscaler: {} from {}x{} to {}x{}", Dlss::Name(mode_), max_width_,
              max_height_, OutputWidth(), OutputHeight());
  return SetLevel(DynamicResolution::kSteps) && CreateTargets() && CreateInputPass();
}

bool Upscaler::CreateInputPass() {
  const D3D12_DESCRIPTOR_RANGE ranges[2] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1},
  };
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {2, ranges};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 2};
  const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, err;
  const D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                         kViewsPerFrame * D3D12Device::kFramesInFlight,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
             "upscaler input root signature serialize") ||
      Failed(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&input_signature_)),
             "upscaler input root signature") ||
      Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&input_heap_)), "upscaler input heap"))
    return false;
  input_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  const pD3DCompile compile = D3DCompiler();
  constexpr char kName[] = "pgr4_upscaler_input.hlsl";
  ComPtr<ID3DBlob> code, error;
  if (!compile ||
      FAILED(compile(kInputHlsl, sizeof(kInputHlsl) - 1, kName, nullptr, nullptr, "main", "cs_5_0",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) {
    REXLOG_ERROR("PGR4 native upscaler: {}: {}", kName,
                 error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = input_signature_.Get();
  pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  return !Failed(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&input_pso_)), kName);
}

void Upscaler::ClampInput(CommandStream* list, ID3D12Resource* color) {
  // In the frame's own views: those of a frame kFramesInFlight back, whose fence the device
  // waited for, are free.
  const uint32_t first = uint32_t(frame_ % D3D12Device::kFramesInFlight) * kViewsPerFrame;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = input_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(first) * input_stride_;
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = kOutputFormat;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(color, &sd, cpu);
  cpu.ptr += input_stride_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = kOutputFormat;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(input_.Get(), nullptr, &ud, cpu);
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = input_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(first) * input_stride_;

  const uint32_t size[2] = {render_width_, render_height_};
  ID3D12DescriptorHeap* heaps[] = {input_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(input_signature_.Get());
  list->SetPipelineState(input_pso_.Get());
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRoot32BitConstants(1, 2, size, 0);
  list->Dispatch((size[0] + 7) / 8, (size[1] + 7) / 8, 1);
}

void Upscaler::Time(CommandStream* list, GpuTimer::UpscalePhase phase) {
  if (timer_) timer_->Pass(list, GpuTimer::KeyUpscale(phase, render_width_, render_height_));
}

bool Upscaler::SetLevel(uint32_t level) {
  constexpr uint32_t n = DynamicResolution::kSteps;
  render_width_ = std::max(1u, (max_width_ * level + n / 2) / n);
  render_height_ = std::max(1u, (max_height_ * level + n / 2) / n);
  // AMD's recommended texture LOD bias for the render size (FSR docs, "Mipmap biasing"),
  // in the sampler key's quarter steps.
  const float ratio = std::min(float(render_width_) / float(OutputWidth()),
                               float(render_height_) / float(OutputHeight()));
  mip_bias_quarters_ = uint32_t(std::lround(-4.0f * (std::log2(ratio) - 1.0f)));
  const double display_ratio = double(OutputWidth()) / double(render_width_);
  phase_count_ = std::clamp(int32_t(std::ceil(8.0 * display_ratio * display_ratio)), 8, 64);
  return true;
}

bool Upscaler::CreateContext(bool debug) {
  // DLSS creates its feature at the first evaluation for a setup (dlss.h). FSR's debug view has
  // no DLSS counterpart; the flag stays for the F8 key.
  debug_ = debug;
  reset_ = true;
  return dlss_.available();
}

bool Upscaler::CreateTargets() {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = OutputWidth();
  d.Height = OutputHeight();
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = kOutputFormat;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&output_.resource)),
             "upscaler output"))
    return false;
  output_.resource_format = output_.view_format = output_.srv_format = kOutputFormat;
  output_.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  output_.width = kSceneWidth;  // guest pixels, at out_scale_ host pixels each
  output_.height = SceneHeight();
  output_.scale = output_.size_scale = {{out_scale_, out_scale_}, {1, 1}};

  d.Width = max_width_;
  d.Height = max_height_;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, input_state_, nullptr,
                                              IID_PPV_ARGS(&input_)),
             "upscaler input"))
    return false;

  d.Format = kMotionFormat;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE zero = {};
  zero.Format = kMotionFormat;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, motion_state_, &zero,
                                              IID_PPV_ARGS(&motion_)),
             "upscaler motion vectors"))
    return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = 1;
  if (Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&motion_rtv_)), "upscaler RTV heap"))
    return false;
  device_->CreateRenderTargetView(motion_.Get(), nullptr,
                                  motion_rtv_->GetCPUDescriptorHandleForHeapStart());
  return true;
}

bool Upscaler::BeginFrame(uint64_t frame, uint64_t gpu_busy_ns, uint64_t paced_ns) {
  frame_ = frame;
  bool resized = false;
  if (dynamic_) {
    const uint32_t level = dynamic_->level();
    if (dynamic_->Update(gpu_busy_ns, paced_ns) != level) {
      SetLevel(dynamic_->level());  // without a phase count, the jitter keeps the old size's
      resized = true;
    }
    if (dynamic_->out_of_reach())
      PGR4_WARN_ONCE(
          "PGR4 native upscaler: {:.1f} ms of GPU time a frame at the least height "
          "({} rows), over the budget of a frame shown for {:.1f} ms; a lower frame "
          "rate or frame generation gives each frame more",
          dynamic_->run_ns() * 1e-6, render_height_, double(paced_ns) * 1e-6);
  }
  const uint64_t now = NowNs();
  frame_ms_ = last_ns_ ? float(double(now - last_ns_) * 1e-6) : 0.0f;
  last_ns_ = now;
  DlssJitter(frame, double(render_width_) / double(OutputWidth()), jitter_[0], jitter_[1]);
  return resized;
}

D3D12_CPU_DESCRIPTOR_HANDLE Upscaler::MotionTarget(CommandStream* list) {
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = motion_rtv_->GetCPUDescriptorHandleForHeapStart();
  if (motion_state_ != D3D12_RESOURCE_STATE_RENDER_TARGET) {
    const D3D12_RESOURCE_BARRIER b =
        TransitionBarrier(motion_.Get(), motion_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &b);
    motion_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
  }
  if (motion_frame_ != frame_) {
    motion_frame_ = frame_;
    const float zero[4] = {};
    list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  }
  return rtv;
}

Upscaler::SceneInputs Upscaler::ReadSceneInputs(CommandStream* list) {
  SceneInputs in;
  if (!upscaled_this_frame() || !scene_depth_) return in;
  RenderTargets::Transition(list, scene_depth_, kInputState);
  if (motion_state_ != kInputState) {
    const D3D12_RESOURCE_BARRIER b = TransitionBarrier(motion_.Get(), motion_state_, kInputState);
    list->ResourceBarrier(1, &b);
    motion_state_ = kInputState;
  }
  in.depth = scene_depth_->resource.Get();
  in.motion = motion_.Get();
  in.width = render_width_;
  in.height = render_height_;
  in.jitter_x = jitter_[0];
  in.jitter_y = jitter_[1];
  in.reset = scene_reset_;
  return in;
}

RenderTarget* Upscaler::Upscale(CommandStream* list, RenderTargets& targets,
                                const SurfaceRegs& regs) {
  if (upscaled_frame_ == frame_) return upscaled_ ? &output_ : nullptr;
  upscaled_frame_ = frame_;
  upscaled_ = false;

  RenderTarget* color = targets.GetColor(regs, 0, SceneHeight());
  RenderTarget* depth = targets.GetDepth(regs, SceneHeight());
  if (!color || !depth || color->samples != 1 || depth->samples != 1 || color->scale != scale() ||
      depth->scale != scale() || color->srv_format != kOutputFormat) {
    PGR4_WARN_ONCE("PGR4 native upscaler: unexpected scene targets (surface {:08X} color {:08X})",
                   regs.surface_info, regs.color_info[0]);
    return nullptr;
  }
  Time(list, GpuTimer::kUpscaleInput);
  const D3D12_RECT all = {0, 0, LONG(kSceneWidth), LONG(SceneHeight())};
  targets.PrepareRead(list, color, all);
  targets.PrepareRead(list, depth, all);

  D3D12_RESOURCE_BARRIER bs[5];
  uint32_t nb = 0;
  auto to = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES* state, D3D12_RESOURCE_STATES after) {
    if (*state == after) return;
    bs[nb++] = TransitionBarrier(r, *state, after);
    *state = after;
  };
  MotionTarget(list);  // cleared if no draw wrote motion this frame
  to(color->resource.Get(), &color->state, kInputState);
  to(depth->resource.Get(), &depth->state, kInputState);
  to(motion_.Get(), &motion_state_, kInputState);
  to(input_.Get(), &input_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  to(output_.resource.Get(), &output_.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (nb) list->ResourceBarrier(nb, bs);
  ClampInput(list, color->resource.Get());
  nb = 0;
  to(input_.Get(), &input_state_, kInputState);
  list->ResourceBarrier(nb, bs);

  Time(list, GpuTimer::kUpscaleDispatch);
  // This port: DLSS. The motion target holds UV-space motion (current -> previous), so its scale
  // to render pixels is the render size, as FSR's motionVectorScale was.
  Dlss::Setup setup;
  setup.render_width = render_width_;
  setup.render_height = render_height_;
  setup.output_width = OutputWidth();
  setup.output_height = OutputHeight();
  setup.mode = mode_;
  setup.preset = REXCVAR_GET(pgr4_native_dlss_preset);
  Dlss::Frame f;
  f.color = input_.Get();
  f.depth = depth->resource.Get();
  f.motion = motion_.Get();
  f.output = output_.resource.Get();
  f.jitter_x = jitter_[0];
  f.jitter_y = jitter_[1];
  f.motion_scale_x = float(render_width_);
  f.motion_scale_y = float(render_height_);
  f.frame_ms = frame_ms_ > 0.0f ? frame_ms_ : 16.7f;
  f.reset = reset_;
  // On the stream's thread, which records the list: a failure shows from the next frame on.
  list->Call([this, setup, f](CommandStream::List* l) {
    if (!dlss_.Evaluate(l, setup, f)) PGR4_WARN_ONCE("PGR4 native upscaler: DLSS evaluation failed");
  });
  // This port: DLSS 5 on the HDR scene DLAA just made (neural.h's scene stage), before the
  // game's exposure, bloom, motion blur and tone mapping read it.
  if (neural_ && neural_->SceneWanted(dlss_)) {
    Time(list, GpuTimer::kUpscaleNeural);
    NeuralPass::SceneInputs in;
    in.depth = depth->resource.Get();
    in.depth_srv_format = depth->srv_format;
    in.motion = motion_.Get();
    in.render_width = render_width_;
    in.render_height = render_height_;
    in.frame_ms = frame_ms_ > 0.0f ? frame_ms_ : 16.7f;
    in.reset = reset_;
    neural_->ProcessScene(list, frame_, dlss_, &output_, OutputWidth(), OutputHeight(), in);
  }
  if (frame_generation_) {
    Time(list, GpuTimer::kUpscaleFrameGeneration);
    const FfxApiResource fg_depth =
        ffxApiGetResourceDX12(depth->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    const FfxApiResource fg_motion =
        ffxApiGetResourceDX12(motion_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    frame_generation_->Prepare(frame_, list, fg_depth, fg_motion, {render_width_, render_height_},
                               {jitter_[0], jitter_[1]},
                               {float(render_width_), float(render_height_)}, *camera_, frame_ms_,
                               reset_);
  }
  scene_depth_ = depth;
  scene_reset_ = reset_;
  reset_ = false;
  upscaled_ = true;
  output_.guest_format = color->guest_format;
  return &output_;
}

}  // namespace pgr4::native
