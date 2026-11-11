// pgr4-native PGR4 native renderer: NVIDIA DLSS through the NGX SDK, in place of FSR.
//
// DLAA (render size = output size) and DLSS Super Resolution (a smaller render size) for the
// 3D scene, at the Upscaler's hook: the scene's first resolve (upscaler.h). The NGX code comes
// from this PC's FromDustRecomp port (runtime/gpu/native/upscaler.cpp) and uses NVIDIA's own
// signed DLLs (nvngx_dlss.dll beside the executable, third_party/dlss_sdk). NGX is initialised
// with this project's ID and its answer is used as it is: on a GPU or driver without DLSS the
// scene stays on MSAA.
#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace pgr4::native {

class Dlss {
 public:
  enum class Mode : uint32_t { kDlaa, kQuality, kBalanced, kPerformance, kUltraPerformance };
  // --pgr4_native_dlss_mode: dlaa, quality, balanced, performance or ultra (dlaa otherwise).
  static Mode ModeFromName(std::string_view name);
  static const char* Name(Mode mode);
  // The render size per output size of a mode (NVIDIA's ratios; 1 for DLAA).
  static double RenderScale(Mode mode);

  struct Setup {
    uint32_t render_width = 0, render_height = 0, output_width = 0, output_height = 0;
    Mode mode = Mode::kDlaa;
    bool hdr = true;             // linear scene light, before the game's exposure
    bool depth_inverted = true;  // reversed depth (cleared to 0, tested GEQUAL)
    int preset = 0;              // NVSDK_NGX_DLSS_Hint_Render_Preset_*; 0 the DLL's default
    bool operator==(const Setup&) const = default;
  };
  struct Frame {
    ID3D12Resource* color = nullptr;   // NON_PIXEL_SHADER_RESOURCE, render size
    ID3D12Resource* depth = nullptr;   // NON_PIXEL_SHADER_RESOURCE, render size
    ID3D12Resource* motion = nullptr;  // NON_PIXEL_SHADER_RESOURCE, render size, current -> previous
    ID3D12Resource* output = nullptr;  // UNORDERED_ACCESS, output size
    float jitter_x = 0, jitter_y = 0;      // render pixels
    float motion_scale_x = 1, motion_scale_y = 1;  // motion * scale = render pixels
    float frame_ms = 16.7f;
    bool reset = false;
  };

  Dlss() = default;
  ~Dlss();
  Dlss(const Dlss&) = delete;
  Dlss& operator=(const Dlss&) = delete;

  // DLSS 5 Neural Rendering (NGX feature 18, nvngx_dlssnr.dll beside the executable): it
  // relights and restyles the finished image (neural.h runs it before the HUD).
  struct NeuralSettings {
    uint32_t style = 2;  // DLSSNR.Style: 0 Default (the strongest), 1 Natural, 2 Cinematic
    float intensity = 1.0f, structure = 1.0f, tone = 1.0f;
    int preset = 0;  // NVSDK_NGX_DLSSNR_Hint_Render_Preset_*: 0 the DLL's default, 1 A ...
    // The skin auto mask: 0 off (the control mask alone), 1 on and combined with the control
    // mask by "min" (RTX Remix's, FromDust's), 2 on with the control mask taking precedence
    // (NVSDK_NGX_DLSSNR_Mask_Blend_Mode_None, the header's default).
    int automask = 0;
    bool operator==(const NeuralSettings&) const = default;
  };
  struct NeuralFrame {
    ID3D12Resource* color = nullptr;   // NON_PIXEL_SHADER_RESOURCE, the image before the HUD
    ID3D12Resource* output = nullptr;  // UNORDERED_ACCESS, at least width x height
    ID3D12Resource* depth = nullptr;   // NON_PIXEL_SHADER_RESOURCE, render size
    ID3D12Resource* motion = nullptr;  // NON_PIXEL_SHADER_RESOURCE, render size
    ID3D12Resource* mask = nullptr;    // NON_PIXEL_SHADER_RESOURCE, render size: 1 = apply
    uint32_t width = 0, height = 0;                // color and output, from (0, 0)
    uint32_t render_width = 0, render_height = 0;  // depth, motion and mask, from (0, 0)
    float jitter_x = 0, jitter_y = 0;
    float motion_scale_x = 1, motion_scale_y = 1;
    bool reset = false, depth_inverted = true;
    NeuralSettings settings;
  };

  // Initializes NGX once; false (logged once) without an RTX GPU, driver or nvngx_dlss.dll.
  bool Initialize(ID3D12Device* device);
  bool available() const { return available_; }
  bool neural_available() const { return available_ && neural_available_ && !neural_failed_; }
  // Records DLSS 5 into `list`, (re)creating its feature when the size or preset changes; the
  // caller sets its own state again afterwards. False when it cannot (logged).
  // `pass` (0 to kMaxNeuralPasses - 1) has a feature of its own.
  static constexpr uint32_t kMaxNeuralPasses = 3;
  bool EvaluateNeural(ID3D12GraphicsCommandList* list, const NeuralFrame& frame, uint32_t pass = 0);
  // Records DLSS into `list`, (re)creating the feature when `setup` changes. The caller sets its
  // own pipeline, root signature and descriptor heaps again afterwards.
  bool Evaluate(ID3D12GraphicsCommandList* list, const Setup& setup, const Frame& frame);

 private:
  void Release();
  // A replaced feature, released once the frames that may use it have finished.
  void Retire(NVSDK_NGX_Handle* handle);
  void ReleaseRetired();
  struct Retired {
    NVSDK_NGX_Handle* handle;
    uint64_t release_at;
  };
  static constexpr uint64_t kRetireEvaluations = 16;
  std::vector<Retired> retired_;
  uint64_t evaluations_ = 0;
  // Written on the command stream's thread (Evaluate*), read on the recording one.
  bool tried_ = false;
  std::atomic<bool> available_{false};
  bool neural_available_ = false;
  std::atomic<bool> neural_failed_{false};
  struct NeuralFeature {
    NVSDK_NGX_Handle* handle = nullptr;
    uint32_t width = 0, height = 0;
    NeuralSettings settings{};
    bool failed = false;  // its creation failed for these settings
  };
  NeuralFeature neural_[kMaxNeuralPasses];
  ID3D12Device* device_ = nullptr;
  NVSDK_NGX_Parameter* params_ = nullptr;
  NVSDK_NGX_Handle* feature_ = nullptr;
  Setup current_{};
};

// Halton(2,3) sub-pixel jitter in [-0.5, 0.5) for frame `index`, with 8 * (output / render)^2
// phases (8 to 64), as NVIDIA's DLSS guide recommends.
void DlssJitter(uint64_t index, double render_scale, float& x, float& y);

}  // namespace pgr4::native
