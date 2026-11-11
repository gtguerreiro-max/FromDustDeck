// pgr4-native PGR4 native renderer (this port): NVIDIA DLSS 5 Neural Rendering.
//
// Two stages (--pgr4_native_dlss5_stage). "scene", the default: on the HDR scene right after
// DLAA, before the game's exposure, bloom, motion blur and tone mapping, as the community's
// OptiScaler forks run it on DLSS Super Resolution's output (neural_scene.cpp, called from
// Upscaler::Upscale). "front": on the finished image before the HUD, as below.
//
// DLSS 5 relights and restyles a finished frame from its color, depth and motion. PGR4 composites
// its frame (the scene after its exposure, bloom and motion blur) into the front buffer's surface
// in 2_10_10_10_FLOAT with an opaque draw, then draws the HUD over it with blending (td-265,
// frame_generation.h, which takes its HUD-less copy at the same place). At the HUD's first draw
// of a frame the upscaler upscaled, this runs DLSS 5 on the surface's host target and writes the
// result back over it, so the HUD lands on the new image and is not restyled. Depth and motion
// are the upscaler's (DLAA: the scene's own size). The image is display-referred: PGR4's gamma
// ramp is close to the identity (present.cpp's log: 0.25/0.5/0.75 -> 0.19/0.46/0.73).
//
// NVIDIA's signed nvngx_dlssnr.dll goes beside the executable (third_party/dlss_nr); NGX's
// answer for this GPU and driver is used as it is. Threading: the recording thread's
// (NativeRenderer::Execute*); the evaluation runs on the device's command stream.
#pragma once

#include "d3d12_device.h"
#include "dlss.h"
#include "render_targets.h"
#include "resolve.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace pgr4::native {

class CommandStream;
class Upscaler;

class NeuralPass {
 public:
  // Whether --pgr4_native_dlss5 asks for it.
  static bool Requested();
  // Whether the player's car stepped in the last half second (Pgr4CarStepHook): a race, not the
  // front end.
  static bool CarsSimulated();

  explicit NeuralPass(ID3D12Device* device) : device_(device) {}

  // The scene stage (neural_scene.cpp). The upscaler's inputs, as DLSS SR took them.
  struct SceneInputs {
    ID3D12Resource* depth = nullptr;   // NON_PIXEL_SHADER_RESOURCE, render size, reversed
    ID3D12Resource* motion = nullptr;  // NON_PIXEL_SHADER_RESOURCE, render size, UV-space
    DXGI_FORMAT depth_srv_format = DXGI_FORMAT_UNKNOWN;  // its depth plane's view
    uint32_t render_width = 0, render_height = 0;
    float frame_ms = 16.7f;
    bool reset = false;
  };
  static bool SceneStage();
  // Whether DLSS 5 runs on this frame's scene (the stage, the GPU, F8, the player driving or
  // --pgr4_native_dlss5_menus).
  bool SceneWanted(const Dlss& dlss);
  // Records DLSS 5 over `scene` (the upscaler's output: linear light in R16G16B16A16_FLOAT,
  // width x height host pixels, its state tracked) into `list`. False when it did not run.
  bool ProcessScene(CommandStream* list, uint64_t frame, Dlss& dlss, RenderTarget* scene,
                    uint32_t width, uint32_t height, const SceneInputs& in);

  // Every resolve, and each Swap with its front buffer's guest physical address: the front
  // buffer's resolve (the last one before the Swap) names the surface the frame is composited in.
  void OnResolve(const ResolveParams& p) { last_resolve_ = p; }
  void OnSwap(uint32_t front_base);
  // Before each draw of frame `frame` (`regs` its register file): at the HUD's first draw, records
  // DLSS 5 over the composited frame into `list` and returns true (the list's state changed).
  bool BeforeDraw(CommandStream* list, uint64_t frame, const uint32_t* regs,
                  RenderTargets& targets, Upscaler* upscaler, uint64_t ps_hash);

 private:
  bool Run(CommandStream* list, uint64_t frame, RenderTargets& targets, Upscaler& upscaler);
  // The model's settings and passes this frame (the cvars, PGR4_DLSS5_SWEEP).
  void ReadSettings(Dlss::NeuralSettings& s, uint32_t& passes);

  // The scene stage's passes and textures (neural_scene.cpp).
  bool CreateScenePasses();
  bool CreateSceneTargets(uint64_t frame, uint32_t width, uint32_t height, uint32_t work_width,
                          uint32_t work_height, uint32_t mask_width, uint32_t mask_height);
  // A pass's views: t0-t4 (a format of UNKNOWN takes the resource's own), u0 and the guides.
  struct SceneViews {
    ID3D12Resource* srv[5] = {};
    DXGI_FORMAT srv_format[5] = {};
    ID3D12Resource* dst = nullptr;
    ID3D12Resource* depth_out = nullptr;
    ID3D12Resource* motion_out = nullptr;
  };
  void SceneDispatch(CommandStream* list, uint64_t frame, uint32_t table, ID3D12PipelineState* pso,
                     const SceneViews& views, uint32_t groups_x, uint32_t groups_y,
                     const void* constants);
  // A replaced texture, released once the frames in flight that may use it are done.
  void Retire(ComPtr<ID3D12Resource>& r, uint64_t frame);
  std::vector<std::pair<uint64_t, ComPtr<ID3D12Resource>>> retired_;
  ComPtr<ID3D12Resource> scene_proxy_, scene_model_[2], scene_composed_, scene_mask_,
      scene_exposure_;
  // The model's depth and motion at its working size.
  ComPtr<ID3D12Resource> scene_depth_w_, scene_motion_w_;
  ComPtr<ID3D12Resource> scene_readback_;  // --pgr4_native_dlss5_log_white
  const float* scene_readback_data_ = nullptr;
  uint32_t scene_resets_ = 0;  // history resets (gaps, the upscaler's), for the log
  D3D12_RESOURCE_STATES scene_guides_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  D3D12_RESOURCE_STATES scene_proxy_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  D3D12_RESOURCE_STATES scene_model_state_[2] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
  D3D12_RESOURCE_STATES scene_composed_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ComPtr<ID3D12RootSignature> scene_signature_;
  ComPtr<ID3D12PipelineState> scene_meter_pso_, scene_exposure_pso_, scene_encode_pso_,
      scene_stabilize_pso_, scene_compose_pso_;
  // The stabilised change at the working size: two textures, this frame's and the last.
  ComPtr<ID3D12Resource> scene_history_[2];
  D3D12_RESOURCE_STATES scene_history_state_[2] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
  uint32_t scene_history_index_ = 0;
  bool scene_history_valid_ = false;
  ComPtr<ID3D12DescriptorHeap> scene_heap_, scene_mask_rtv_;
  uint32_t scene_stride_ = 0, scene_width_ = 0, scene_height_ = 0;
  uint32_t scene_work_width_ = 0, scene_work_height_ = 0;
  uint64_t scene_pass_frame_[Dlss::kMaxNeuralPasses] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
  uint32_t scene_mask_width_ = 0, scene_mask_height_ = 0;
  bool scene_mask_cleared_ = false, scene_racing_ = false, scene_logged_ = false;
  uint64_t scene_done_frame_ = UINT64_MAX, scene_last_frame_ = UINT64_MAX;
  // DLSS 5's textures at the working size (width x height), the composed frame at the full one.
  bool CreateTargets(uint32_t width, uint32_t height, uint32_t mask_width, uint32_t mask_height,
                     uint32_t full_width, uint32_t full_height);
  // Whether --pgr4_native_dlss5_skip_composites names the frame's composite shader (each one
  // seen is logged once).
  bool Skipped(uint64_t composite_ps);
  bool racing_ = false;
  uint64_t composite_ps_ = 0;  // the pixel shader of the frame's opaque composite
  bool skip_parsed_ = false;
  std::vector<uint64_t> skip_, seen_;

  ID3D12Device* device_;
  ResolveParams last_resolve_;
  std::optional<ResolveParams> front_;
  uint64_t composited_frame_ = UINT64_MAX;  // the frame of the last opaque draw into the front
  uint64_t done_frame_ = UINT64_MAX;        // the last frame BeforeDraw ran DLSS 5 in
  uint64_t last_run_frame_ = UINT64_MAX;    // its history: a gap resets it
  bool warned_ = false;
  // DLSS 5's output (the front target's format) and its control mask (white: everywhere).
  ComPtr<ID3D12Resource> output_, mask_;
  ComPtr<ID3D12Resource> original_;  // --pgr4_native_dlss5_split=2: the input's middle half
  // DLSS 5's input (kNeuralHlsl): the composite in [0, 1] with alpha 1 at the working size, and
  // the composition of its change into the full frame.
  bool CreateInputPass();
  void PrepareInput(CommandStream* list, uint64_t frame, ID3D12Resource* color,
                    uint32_t frame_width, uint32_t frame_height, uint32_t width, uint32_t height);
  void Dispatch(CommandStream* list, uint64_t frame, uint32_t table, ID3D12PipelineState* pso,
                ID3D12Resource* const srvs[3], ID3D12Resource* uav, DXGI_FORMAT uav_format,
                uint32_t width, uint32_t height, float uv_scale_x, float uv_scale_y);
  ComPtr<ID3D12Resource> work_, composed_;
  D3D12_RESOURCE_STATES work_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  D3D12_RESOURCE_STATES composed_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ComPtr<ID3D12PipelineState> compose_pso_;
  uint32_t full_width_ = 0, full_height_ = 0;
  ComPtr<ID3D12Resource> input_;
  D3D12_RESOURCE_STATES input_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ComPtr<ID3D12RootSignature> input_signature_;
  ComPtr<ID3D12PipelineState> input_pso_;
  ComPtr<ID3D12DescriptorHeap> input_heap_;
  uint32_t input_stride_ = 0;
  Dlss::NeuralSettings sweep_settings_;  // PGR4_DLSS5_SWEEP's last step, for its log
  uint32_t sweep_passes_ = 0;
  // The front buffer's resolve last logged.
  uint32_t logged_color_info_ = 0, logged_dest_format_ = 0;
  int32_t logged_exp_bias_ = 0;
  bool logged_swap_ = false;
  ComPtr<ID3D12DescriptorHeap> mask_rtv_;
  D3D12_RESOURCE_STATES output_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  bool mask_cleared_ = false;
  uint32_t width_ = 0, height_ = 0, mask_width_ = 0, mask_height_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

}  // namespace pgr4::native
