// pgr4-native PGR4 native renderer: NVIDIA DLSS through the NGX SDK (dlss.h).
#include "dlss.h"

#include "log.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#undef DrawState

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_defs_dlssnr.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

REXCVAR_DEFINE_INT32(pgr4_native_ngx_log, 1, "PGR4 native",
                     "NGX's log in the game's: 0 off, 1 on, 2 verbose (every evaluation)");

namespace pgr4::native {

namespace {

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature feature) {
  std::string m = message ? message : "";
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
  if (!m.empty()) REXLOG_INFO("NGX [{}{}]: {}", int(feature), level == NVSDK_NGX_LOGGING_LEVEL_VERBOSE ? " v" : "", m);
}

// This project's NGX application ID (a custom engine, NVSDK_NGX_ENGINE_TYPE_CUSTOM).
constexpr char kProjectId[] = "5b8f2d1e-3c4a-4e7b-9f60-1d2c3b4a5e6f";

float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  while (index > 0) {
    f /= float(base);
    r += f * float(index % base);
    index /= base;
  }
  return r;
}

}  // namespace

Dlss::Mode Dlss::ModeFromName(std::string_view name) {
  if (name == "quality") return Mode::kQuality;
  if (name == "balanced") return Mode::kBalanced;
  if (name == "performance") return Mode::kPerformance;
  if (name == "ultra" || name == "ultra_performance") return Mode::kUltraPerformance;
  return Mode::kDlaa;
}

const char* Dlss::Name(Mode mode) {
  switch (mode) {
    case Mode::kQuality: return "DLSS Quality";
    case Mode::kBalanced: return "DLSS Balanced";
    case Mode::kPerformance: return "DLSS Performance";
    case Mode::kUltraPerformance: return "DLSS Ultra Performance";
    default: return "DLAA";
  }
}

double Dlss::RenderScale(Mode mode) {
  switch (mode) {
    case Mode::kQuality: return 2.0 / 3.0;
    case Mode::kBalanced: return 0.58;
    case Mode::kPerformance: return 0.5;
    case Mode::kUltraPerformance: return 1.0 / 3.0;
    default: return 1.0;
  }
}

void DlssJitter(uint64_t index, double render_scale, float& x, float& y) {
  const double ratio = 1.0 / std::max(render_scale, 0.01);
  const uint32_t phases = std::clamp(uint32_t(std::ceil(8.0 * ratio * ratio)), 8u, 64u);
  const uint32_t i = uint32_t(index % phases) + 1;
  x = Halton(i, 2) - 0.5f;
  y = Halton(i, 3) - 0.5f;
}

Dlss::~Dlss() {
  Release();
  for (NeuralFeature& n : neural_)
    if (n.handle) NVSDK_NGX_D3D12_ReleaseFeature(n.handle);
  for (const Retired& r : retired_) NVSDK_NGX_D3D12_ReleaseFeature(r.handle);
  if (available_) NVSDK_NGX_D3D12_Shutdown1(device_);
}

void Dlss::Retire(NVSDK_NGX_Handle* handle) {
  if (handle) retired_.push_back({handle, evaluations_ + kRetireEvaluations});
}

void Dlss::ReleaseRetired() {
  ++evaluations_;
  // The frames in flight that recorded a retired feature have finished by now (each frame
  // evaluates once or twice, and at most D3D12Device::kFramesInFlight are in flight).
  std::erase_if(retired_, [this](const Retired& r) {
    if (evaluations_ < r.release_at) return false;
    NVSDK_NGX_D3D12_ReleaseFeature(r.handle);
    return true;
  });
}

void Dlss::Release() {
  // Destruction only: the device is idle.
  if (feature_) NVSDK_NGX_D3D12_ReleaseFeature(feature_);
  feature_ = nullptr;
  current_ = {};
}

bool Dlss::Initialize(ID3D12Device* device) {
  if (tried_) return available_;
  tried_ = true;
  device_ = device;
  wchar_t path[MAX_PATH] = L".";
  GetTempPathW(MAX_PATH, path);
  // NGX's own log in ours (--pgr4_native_ngx_log): DLSS 5's feature creation, its network size and
  // the evaluations it skips are only told there.
  NVSDK_NGX_FeatureCommonInfo info = {};
  const int32_t log_level = std::clamp(REXCVAR_GET(pgr4_native_ngx_log), 0, 2);
  info.LoggingInfo.LoggingCallback = NgxLog;
  info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_Logging_Level(log_level);
  info.LoggingInfo.DisableOtherLoggingSinks = false;
  NVSDK_NGX_Result r =
      NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", path, device,
                                          log_level > 0 ? &info : nullptr);
  if (NVSDK_NGX_FAILED(r)) {
    REXLOG_WARN("PGR4 native DLSS: NGX initialization failed ({:08X}): needs an NVIDIA RTX GPU, its driver "
                "and nvngx_dlss.dll beside the executable", uint32_t(r));
    return false;
  }
  int supported = 0, needs_driver = 0;
  if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&params_))) {
    NVSDK_NGX_Parameter_GetI(params_, NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
    NVSDK_NGX_Parameter_GetI(params_, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
  }
  if (!supported) {
    REXLOG_WARN("PGR4 native DLSS: not available{}", needs_driver ? " (the driver needs an update)" : "");
    NVSDK_NGX_D3D12_Shutdown1(device_);
    return false;
  }
  available_ = true;
  REXLOG_INFO("PGR4 native DLSS: ready");
  // DLSS 5 Neural Rendering: what NGX answers for this application, used as it is.
  int nr_available = 0, nr_needs_driver = 0, nr_init = 0;
  unsigned int nr_major = 0, nr_minor = 0;
  NVSDK_NGX_Parameter_GetI(params_, NVSDK_NGX_Parameter_DLSSNR_Available, &nr_available);
  NVSDK_NGX_Parameter_GetI(params_, NVSDK_NGX_Parameter_DLSSNR_NeedsUpdatedDriver, &nr_needs_driver);
  NVSDK_NGX_Parameter_GetUI(params_, NVSDK_NGX_Parameter_DLSSNR_MinDriverVersionMajor, &nr_major);
  NVSDK_NGX_Parameter_GetUI(params_, NVSDK_NGX_Parameter_DLSSNR_MinDriverVersionMinor, &nr_minor);
  NVSDK_NGX_Parameter_GetI(params_, NVSDK_NGX_Parameter_DLSSNR_FeatureInitResult, &nr_init);
  neural_available_ = nr_available != 0;
  REXLOG_INFO("PGR4 native DLSS 5: available {}, needs driver {} (min {}.{}), init result {:08X}",
              nr_available, nr_needs_driver, nr_major, nr_minor, uint32_t(nr_init));
  return true;
}

bool Dlss::EvaluateNeural(ID3D12GraphicsCommandList* list, const NeuralFrame& f, uint32_t pass) {
  if (!neural_available() || !f.color || !f.output || !f.depth || !f.motion ||
      pass >= kMaxNeuralPasses)
    return false;
  if (pass == 0) ReleaseRetired();
  NeuralFeature& n = neural_[pass];
  // The model's tuning (style, strengths, preset) is latched when the feature is created (the
  // OptiScaler DLSS 5 forks' finding): it is set before the creation too, and the feature is
  // created again when it changes.
  auto tune = [&f](NVSDK_NGX_Parameter* p) {
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength, f.settings.tone);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength,
                             f.settings.structure);
    NVSDK_NGX_Parameter_SetUI(p, NVSDK_NGX_Parameter_DLSSNR_Style, f.settings.style);
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_DLSSNR_Intensity, f.settings.intensity);
    // The skin auto mask combined with the control mask by "min", as RTX Remix does (the
    // FromDust port's NGX code, the same); skin structure -1 follows the local structure.
    NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_DLSSNR_UseAutoMask, f.settings.automask ? 1 : 0);
    NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_DLSSNR_MaskBlendMode,
                             int(f.settings.automask == 1 ? NVSDK_NGX_DLSSNR_Mask_Blend_Mode_Min
                                                          : NVSDK_NGX_DLSSNR_Mask_Blend_Mode_None));
    NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength, -1.0f);
  };
  if (n.failed && n.width == f.width && n.height == f.height && n.settings == f.settings)
    return false;
  if (!n.handle || n.width != f.width || n.height != f.height || !(n.settings == f.settings)) {
    Retire(n.handle);
    n.handle = nullptr;
    tune(params_);
    NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_DLSSNR_Width, f.width);
    NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_DLSSNR_Height, f.height);
    NVSDK_NGX_Parameter_SetI(params_, NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset, f.settings.preset);
    const NVSDK_NGX_Result r =
        NVSDK_NGX_D3D12_CreateFeature(list, NVSDK_NGX_Feature_DLSSNR, params_, &n.handle);
    if (NVSDK_NGX_FAILED(r)) {
      // These settings only (an unsupported preset, say): tried again when they change.
      REXLOG_ERROR("PGR4 native DLSS 5: create failed ({:08X}) for {}x{}, preset {}", uint32_t(r),
                   f.width, f.height, f.settings.preset);
      n.handle = nullptr;
      n.failed = true;
      n.width = f.width;
      n.height = f.height;
      n.settings = f.settings;
      return false;
    }
    n.failed = false;
    n.width = f.width;
    n.height = f.height;
    n.settings = f.settings;
    REXLOG_INFO("PGR4 native DLSS 5 pass {}: {}x{} (depth and motion {}x{}), style {}, intensity {}, "
                "structure {}, tone {}, preset {}", pass + 1, f.width, f.height, f.render_width,
                f.render_height, f.settings.style, f.settings.intensity, f.settings.structure,
                f.settings.tone, f.settings.preset);
    REXLOG_INFO("PGR4 native DLSS 5 pass {}: skin auto mask {}", pass + 1,
                f.settings.automask == 0 ? "off" : f.settings.automask == 1 ? "min" : "none");
  }
  NVSDK_NGX_Parameter* p = params_;
  NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_Parameter_DLSSNR_Color, f.color);
  NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_Parameter_DLSSNR_Output, f.output);
  NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_Parameter_DLSSNR_Backbuffer, nullptr);
  NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_Parameter_DLSSNR_MVec, f.motion);
  NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_Parameter_DLSSNR_Depth, f.depth);
  NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_Parameter_DLSSNR_BidirectionalDistortionField,
                                       nullptr);
  NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_Parameter_DLSSNR_ControlMask, f.mask);
  NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_Parameter_DLSSNR_UI, nullptr);
  NVSDK_NGX_Parameter_SetD3d12Resource(p, NVSDK_NGX_Parameter_DLSSNR_UIAlpha, nullptr);
  tune(p);
  NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_DLSSNR_Reset, f.reset ? 1 : 0);
  NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_DLSSNR_DepthInverted, f.depth_inverted ? 1 : 0);
  NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_DLSSNR_Enabled, 1);
  NVSDK_NGX_Parameter_SetI(p, NVSDK_NGX_Parameter_DLSSNR_UICorrection, 0);
  NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_DLSSNR_MVecScaleX, f.motion_scale_x);
  NVSDK_NGX_Parameter_SetF(p, NVSDK_NGX_Parameter_DLSSNR_MVecScaleY, f.motion_scale_y);
  auto rect = [p](const char* x, const char* y, const char* w, const char* h, uint32_t width,
                  uint32_t height) {
    NVSDK_NGX_Parameter_SetUI(p, x, 0);
    NVSDK_NGX_Parameter_SetUI(p, y, 0);
    NVSDK_NGX_Parameter_SetUI(p, w, width);
    NVSDK_NGX_Parameter_SetUI(p, h, height);
  };
  rect(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX, NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY,
       NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth, NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight,
       f.width, f.height);
  rect(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX, NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY,
       NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth, NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight,
       f.width, f.height);
  rect(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX, NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY,
       NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth, NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight,
       f.render_width, f.render_height);
  rect(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX, NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY,
       NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth, NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight,
       f.render_width, f.render_height);
  rect(NVSDK_NGX_Parameter_DLSSNR_ControlMaskSubrectBaseX,
       NVSDK_NGX_Parameter_DLSSNR_ControlMaskSubrectBaseY,
       NVSDK_NGX_Parameter_DLSSNR_ControlMaskSubrectWidth,
       NVSDK_NGX_Parameter_DLSSNR_ControlMaskSubrectHeight, f.render_width, f.render_height);
  // No jitter: DLSS 5 has no such parameter (its header, and the DLL has no such string), and its
  // colour (DLAA's output, or the finished image) is not jittered.
  const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_EvaluateFeature(list, n.handle, p, nullptr);
  if (NVSDK_NGX_FAILED(r)) {
    PGR4_LOG_FIRST(3, ERROR, "PGR4 native DLSS 5: evaluate failed ({:08X})", uint32_t(r));
    return false;
  }
  return true;
}

bool Dlss::Evaluate(ID3D12GraphicsCommandList* list, const Setup& setup, const Frame& frame) {
  if (!available_) return false;
  ReleaseRetired();
  if (!feature_ || !(setup == current_)) {
    Retire(feature_);
    feature_ = nullptr;
    current_ = {};
    NVSDK_NGX_PerfQuality_Value quality = NVSDK_NGX_PerfQuality_Value_DLAA;
    switch (setup.mode) {
      case Mode::kQuality: quality = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
      case Mode::kBalanced: quality = NVSDK_NGX_PerfQuality_Value_Balanced; break;
      case Mode::kPerformance: quality = NVSDK_NGX_PerfQuality_Value_MaxPerf; break;
      case Mode::kUltraPerformance: quality = NVSDK_NGX_PerfQuality_Value_UltraPerformance; break;
      default: break;
    }
    if (setup.preset > 0) {
      for (const char* hint :
           {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance})
        NVSDK_NGX_Parameter_SetUI(params_, hint, uint32_t(setup.preset));
    }
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = setup.render_width;
    create.Feature.InHeight = setup.render_height;
    create.Feature.InTargetWidth = setup.output_width;
    create.Feature.InTargetHeight = setup.output_height;
    create.Feature.InPerfQualityValue = quality;
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                                  (setup.depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) |
                                  (setup.hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0);
    const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &feature_, params_, &create);
    if (NVSDK_NGX_FAILED(r)) {
      REXLOG_ERROR("PGR4 native DLSS: create failed ({:08X}) for {}x{} -> {}x{}", uint32_t(r), setup.render_width,
                   setup.render_height, setup.output_width, setup.output_height);
      feature_ = nullptr;
      available_ = false;
      return false;
    }
    current_ = setup;
    REXLOG_INFO("PGR4 native DLSS: {} {}x{} -> {}x{}", Name(setup.mode), setup.render_width, setup.render_height,
                setup.output_width, setup.output_height);
  }
  NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
  eval.Feature.pInColor = frame.color;
  eval.Feature.pInOutput = frame.output;
  eval.pInDepth = frame.depth;
  eval.pInMotionVectors = frame.motion;
  eval.InJitterOffsetX = frame.jitter_x;
  eval.InJitterOffsetY = frame.jitter_y;
  eval.InRenderSubrectDimensions = {setup.render_width, setup.render_height};
  eval.InReset = frame.reset ? 1 : 0;
  eval.InMVScaleX = frame.motion_scale_x;
  eval.InMVScaleY = frame.motion_scale_y;
  eval.InFrameTimeDeltaInMsec = frame.frame_ms;
  const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(list, feature_, params_, &eval);
  if (NVSDK_NGX_FAILED(r)) {
    PGR4_LOG_FIRST(3, ERROR, "PGR4 native DLSS: evaluate failed ({:08X})", uint32_t(r));
    return false;
  }
  return true;
}

}  // namespace pgr4::native
