// pgr4-native PGR4 native renderer (this port): DLSS 5 on the HDR scene, right after DLAA. See
// neural.h.
//
// The community's OptiScaler DLSS 5 forks (Dagherbou's module and the Skynizz, janblade and
// ShyVortex forks built on it) run the model on DLSS Super Resolution's output: the game's
// linear HDR scene before its exposure, bloom, motion blur and tone mapping, which then work on
// the model's lighting. The model takes display-referred SDR (RTX Remix feeds sRGB-encoded [0, 1]
// with alpha 1; the DLSS 5 API has no HDR or exposure input), so the scene is encoded the forks'
// way and their composition takes the model's change back into linear light:
//
//   meter    the white point W: the geometric mean of 64x64 tile means maps to 0.20861 (the
//            Skynizz meter, measured on the DLAA output before the model writes anything),
//            smoothed with a 0.4-stop dead band, 0.8 s up, 1.6 s down, at most 3 stops a second
//   encode   n = max(c, 0) / W; a soft knee on the luminance above 0.75, the peak channel to 1;
//            the sRGB curve; alpha 1
//   model    DLSS 5 on that proxy (1-3 passes), the scene's depth and motion at the render size
//   compose  in linear light over W, on the luminance (one ratio a pixel, the model's hue): below
//            the proxy the original's ratio to it, above it the knee's headroom handed back;
//            TransferStrength, ColourStrength and a two-sided MaxRatio clamp; times W, clamped to
//            the 7e3 range the scene's resolve keeps
//
// The scene target keeps its tracked state (RenderTarget::state) for the resolve after it.
#include "neural.h"

#include "command_stream.h"
#include "log.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <memory>

REXCVAR_DECLARE(bool, pgr4_native_dlss5_paused);
REXCVAR_DECLARE(bool, pgr4_native_dlss5_menus);
REXCVAR_DECLARE(bool, pgr4_native_dlss5_compare);
REXCVAR_DECLARE(int32_t, pgr4_native_dlss5_split);
REXCVAR_DECLARE(double, pgr4_native_dlss5_max_ratio);
REXCVAR_DECLARE(double, pgr4_native_dlss5_scale);

REXCVAR_DEFINE_STRING(pgr4_native_dlss5_stage, "scene", "PGR4 native",
                      "Where DLSS 5 runs: scene (the HDR scene right after DLAA, before the game's "
                      "exposure, bloom, motion blur and tone mapping, as the community's "
                      "OptiScaler forks run it) or front (the finished image before the HUD)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_dlss5_white, 0.0, "PGR4 native",
                      "DLSS 5 at the scene: the scene's white point for its SDR encode (0: metered "
                      "from the scene, the Skynizz fork's automatic white point)");
REXCVAR_DEFINE_INT32(pgr4_native_dlss5_view, 0, "PGR4 native",
                     "DLSS 5 at the scene, diagnostic: 0 the composition, 1 the model's raw output "
                     "(not stabilised), 2 its input (the encode alone)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_dlss5_stability, 0.5, "PGR4 native",
                      "DLSS 5 at the scene: how much of the previous frame's change each frame "
                      "keeps, reprojected and rejected on disocclusions (0-0.95; the model's own "
                      "output flickers from frame to frame)");
REXCVAR_DEFINE_BOOL(pgr4_native_dlss5_log_white, false, "PGR4 native",
                    "DLSS 5 at the scene, diagnostic: log the metered white point every 120 frames");
REXCVAR_DEFINE_DOUBLE(pgr4_native_dlss5_meter_up, 0.8, "PGR4 native",
                      "DLSS 5 at the scene: the white point meter's time constant (seconds) when "
                      "the scene brightens (the Skynizz meter: 0.8)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_dlss5_meter_down, 1.6, "PGR4 native",
                      "DLSS 5 at the scene: the meter's time constant (seconds) when it darkens");
REXCVAR_DEFINE_DOUBLE(pgr4_native_dlss5_meter_band, 0.4, "PGR4 native",
                      "DLSS 5 at the scene: the meter's dead band (stops): changes smaller than "
                      "this leave the white point as it is");
REXCVAR_DEFINE_DOUBLE(pgr4_native_dlss5_transfer, 1.0, "PGR4 native",
                      "DLSS 5 at the scene: how much of the model's lighting is taken (the forks' "
                      "TransferStrength, 0-2)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_dlss5_colour, 1.0, "PGR4 native",
                      "DLSS 5 at the scene: 0 keeps the game's hue, 1 takes the model's (the "
                      "forks' ColourStrength)");

namespace pgr4::native {

namespace {

constexpr DXGI_FORMAT kSceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kMaskFormat4 = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr uint32_t kExposureBytes = 256;

// t0 the scene, t1 the model's output, t2 its input (the proxy); u0 the destination, u1 the
// exposure buffer: [0] log2 W smoothed, [4] set, [8] tile sum, [12] tile count, [16] W.
constexpr char kSceneHlsl[] = R"(
Texture2D<float4> scene : register(t0);
Texture2D<float4> model : register(t1);
Texture2D<float4> proxy : register(t2);
Texture2D<float> depth_in : register(t3);    // the scene's depth, render size
Texture2D<float2> motion_in : register(t4);  // its motion, render size, UV-space
RWTexture2D<float4> dst : register(u0);
RWByteAddressBuffer expo : register(u1);
RWTexture2D<float> depth_out : register(u2);    // at the working size
RWTexture2D<float2> motion_out : register(u3);  // at the working size
SamplerState lin : register(s0);
cbuffer Constants : register(b0) {
  uint2 size;         // the scene's host pixels
  float max_ratio;    // the forks' MaxRatio; 0: no clamp
  float transfer;     // TransferStrength
  float colour;       // ColourStrength
  float white_fixed;  // > 0: a fixed white point; 0: the meter's
  float dt;           // seconds since the last frame, for the meter's smoothing
  uint flags;         // 1: left half as the game draws it (comparison), 2: the meter starts over,
                      // 4: the model's raw output shown (diagnostic), 8: the proxy shown,
                      // 16: the middle half twice, as the game draws it | with DLSS 5,
                      // 32: no history for the stabilisation (a reset)
  uint2 work;         // the model's working size (proxy, model, depth and motion)
  uint2 render;       // the scene's depth and motion size
  float tau_up;       // the meter's time constants (seconds) and dead band (stops)
  float tau_down;
  float dead_band;
  float stability;    // how much of the previous frame's change the stabilisation keeps
};
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
float Luma(float3 c) { return dot(c, kLuma); }
float3 LinToSrgb(float3 c) {
  c = saturate(c);
  return lerp(c * 12.92, 1.055 * pow(max(c, 1e-8), 1.0 / 2.4) - 0.055, step(0.0031308, c));
}
float3 SrgbToLin(float3 c) {
  c = saturate(c);
  return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c));
}
float White() { return white_fixed > 0.0 ? white_fixed : asfloat(expo.Load(16)); }
// n over W: the forks' soft knee on the luminance above 0.75, then the peak channel to 1.
float3 Knee(float3 n) {
  float L = Luma(n);
  if (L > 0.75) n *= (0.75 + 0.25 * (1.0 - exp(-(L - 0.75) / 0.25))) / L;
  float peak = max(n.r, max(n.g, n.b));
  if (peak > 1.0) n /= peak;
  return n;
}

groupshared float gs_sum[64];
groupshared uint gs_n[64];
// One group a 64x64 tile (each thread 8x8 texels): log2 of the tile's mean luminance into the sum.
[numthreads(8, 8, 1)]
void meter_main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
  uint2 base = gid.xy * 64 + gtid.xy * 8;
  float s = 0.0;
  uint n = 0;
  for (uint y = 0; y < 8; ++y)
    for (uint x = 0; x < 8; ++x) {
      uint2 p = base + uint2(x, y);
      if (all(p < size)) {
        s += Luma(max(scene.Load(int3(p, 0)).rgb, 0.0));
        ++n;
      }
    }
  gs_sum[gi] = s;
  gs_n[gi] = n;
  GroupMemoryBarrierWithGroupSync();
  if (gi == 0) {
    float t = 0.0;
    uint c = 0;
    for (uint i = 0; i < 64; ++i) {
      t += gs_sum[i];
      c += gs_n[i];
    }
    if (c > 0 && t / float(c) > 1e-5) {
      float lg = clamp(log2(t / float(c)), -20.0, 12.0);
      uint ignored;
      expo.InterlockedAdd(8, (uint)round((lg + 20.0) * 256.0), ignored);
      expo.InterlockedAdd(12, 1, ignored);
    }
  }
}

// The white point from this frame's tiles, smoothed; the sums cleared for the next frame.
[numthreads(1, 1, 1)]
void exposure_main() {
  uint sum = expo.Load(8), cnt = expo.Load(12);
  expo.Store(8, 0);
  expo.Store(12, 0);
  float cur = asfloat(expo.Load(0));
  bool set = expo.Load(4) != 0;
  if (cnt > 0) {
    float target = float(sum) / float(cnt) / 256.0 - 20.0 - log2(0.20861);
    target = clamp(target, log2(0.01), log2(65536.0));
    if (!set || (flags & 2u) != 0u || !(cur == cur)) {
      cur = target;
    } else {
      float d = target - cur;
      if (abs(d) > dead_band) {
        float tau = d > 0.0 ? tau_up : tau_down;
        float stp = d * (1.0 - exp(-max(dt, 0.0) / tau));
        cur += clamp(stp, -3.0 * dt, 3.0 * dt);
      }
    }
    expo.Store(0, asuint(cur));
    expo.Store(4, 1);
  }
  expo.Store(16, asuint(set || cnt > 0 ? exp2(cur) : 1.0));
}

// The model's input at its working size: the scene (bilinear in linear light: 2x2 texels a texel
// at a scale of 1/2) over W, the soft knee, sRGB, alpha 1.
[numthreads(8, 8, 1)]
void encode_main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= work)) return;
  float2 uv = (float2(p) + 0.5) / float2(work);
  float3 n = max(scene.SampleLevel(lin, uv, 0).rgb, 0.0) / White();
  dst[p] = float4(LinToSrgb(Knee(n)), 1.0);
  // The model's guides at its own size: the depth and motion texel under the texel's centre
  // (point samples: no depth or motion blended across edges).
  int2 rp = int2(min(uv * float2(render), float2(render) - 1.0));
  depth_out[p] = depth_in.Load(int3(rp, 0));
  motion_out[p] = motion_in.Load(int3(rp, 0));
}

// The model's change (model - proxy, at the working size) stabilised: blended with the previous
// frame's at the reprojected position (t0 the history: rgb the change, a its depth; t3/t4 this
// frame's depth and motion at the working size), unless the depth there disagrees.
[numthreads(8, 8, 1)]
void stabilize_main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= work)) return;
  // This frame's change and its 3x3 range (TAA's neighbourhood clamp: the history may smooth
  // the model's flicker but never carry a change this neighbourhood does not have, which is what
  // trails would be).
  float3 d = 0.0, dmin = 1e9, dmax = -1e9;
  float zmin = 1e9, zmax = -1e9;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      int3 q = int3(clamp(int2(p) + int2(x, y), int2(0, 0), int2(work) - 1), 0);
      float3 v = model.Load(q).rgb - proxy.Load(q).rgb;
      dmin = min(dmin, v);
      dmax = max(dmax, v);
      if (x == 0 && y == 0) d = v;
      float zq = depth_in.Load(q);
      zmin = min(zmin, zq);
      zmax = max(zmax, zq);
    }
  float z = depth_in.Load(int3(p, 0));
  float3 o = d;
  // 32: no history (a reset); 4: the diagnostic view of the model's raw output.
  if ((flags & 36u) == 0u && stability > 0.0) {
    float2 uv = (float2(p) + 0.5) / float2(work);
    float2 puv = uv + motion_in.Load(int3(p, 0));  // UV-space, current -> previous
    if (all(puv >= 0.0) && all(puv <= 1.0)) {
      // The history's depth at the nearest texel, unfiltered, against this frame's 3x3 depth
      // range (the DLAA jitter moves silhouettes by a texel from frame to frame) widened by
      // 25 % (far more than a surface moves between frames at racing speed): outside it, a
      // disocclusion.
      int2 hp = clamp(int2(puv * float2(work)), int2(0, 0), int2(work) - 1);
      float zh = scene.Load(int3(hp, 0)).a;
      if (zh >= zmin * 0.75 && zh <= zmax * 1.25 + 1e-5) {
        float3 h = clamp(scene.SampleLevel(lin, puv, 0).rgb, dmin, dmax);
        o = lerp(d, h, saturate(stability));
      }
    }
  }
  if (any(isnan(o)) || any(isinf(o))) o = 0.0;
  dst[p] = float4(clamp(o, -1.0, 1.0), z);
}

// The model's change back into the scene's linear light, at the full size. The change at the
// working size goes onto the full-size proxy as a residual (the forks' matched residual: the
// same as the model's output at a working scale of 1), then the composition on the luminance.
[numthreads(8, 8, 1)]
void compose_main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
  uint2 q = p;  // the scene texel shown at p
  bool raw = (flags & 1u) != 0u && p.x < size.x / 2;
  if ((flags & 16u) != 0u) {
    raw = p.x < size.x / 2;
    q.x = raw ? p.x + size.x / 4 : p.x - size.x / 4;
  }
  float4 c = scene.Load(int3(q, 0));
  if (raw) {
    dst[p] = c;
    return;
  }
  float W = White();
  float3 o = max(c.rgb, 0.0) / W;
  float2 uv = (float2(q) + 0.5) / float2(size);
  float3 pr = Knee(o);
  // t1: the stabilised change (stabilize_main) at the working size.
  float3 delta = model.SampleLevel(lin, uv, 0).rgb;
  float3 m = SrgbToLin(LinToSrgb(pr) + delta);
  if ((flags & 12u) != 0u) {
    // Diagnostics: the model's output or its input as they are, back in the scene's light.
    dst[p] = float4(((flags & 4u) != 0u ? m : pr) * W, c.a);
    return;
  }
  float Lo = Luma(o), Lp = Luma(pr), Lm = Luma(m);
  float3 up = o;
  if (Lm > 1e-5) {
    float ratio = Lo < Lp ? Lo / max(Lp, 1e-6) : (Lm + max(0.0, Lo - Lp)) / Lm;
    up = lerp(o, m * ratio, saturate(transfer));
  }
  // The clamp on a target luminance Lt, reached by both ends of the colour lerp (scaling by
  // b / r with an offset in r let dark output through far past 1 / MaxRatio).
  float Lu = Luma(up);
  float r = (Lu + 1.0 / 512.0) / (Lo + 1.0 / 512.0);
  float g = pow(max(r, 1e-6), 1.0 + max(transfer - 1.0, 0.0));
  float b = g;
  if (max_ratio > 0.0) {
    float mr = max(max_ratio, 1.0);
    b = clamp(g, 1.0 / mr, mr);
  }
  float Lt = b * Lo;
  float3 keep = o;
  if (Lo > 1e-6) keep = o * (Lt / Lo);
  float3 took = keep;
  if (Lu > 1e-6) took = up * (Lt / Lu);
  float3 res = lerp(keep, took, saturate(colour));
  float3 outc = min(max(res, 0.0) * W, 31.875);
  if (any(isnan(outc)) || any(isinf(outc))) outc = c.rgb;
  dst[p] = float4(outc, c.a);
}
)";

// A table a pass: t0-t4, u0-u3.
constexpr uint32_t kSceneSrvs = 5, kSceneUavs = 4;
constexpr uint32_t kSceneViews = kSceneSrvs + kSceneUavs;
constexpr DXGI_FORMAT kSceneDepthFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kSceneMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
enum SceneTable : uint32_t {
  kMeterTable,
  kExposureTable,
  kEncodeTable,
  kStabilizeTable,
  kComposeTable,
  kTables
};

struct SceneConstants {
  uint32_t size[2];
  float max_ratio, transfer, colour, white_fixed, dt;
  uint32_t flags;
  uint32_t work[2];
  uint32_t render[2];
  float tau_up, tau_down, dead_band, stability;
};
static_assert(sizeof(SceneConstants) == 64);

}  // namespace

bool NeuralPass::SceneStage() { return REXCVAR_GET(pgr4_native_dlss5_stage) != "front"; }

bool NeuralPass::CreateScenePasses() {
  if (scene_compose_pso_) return true;
  // The exposure buffer: zeros (W unset).
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC bd = {};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = kExposureBytes;
  bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (!scene_exposure_ &&
      Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, kUav, nullptr,
                                              IID_PPV_ARGS(&scene_exposure_)),
             "DLSS 5 exposure buffer"))
    return false;
  const D3D12_DESCRIPTOR_RANGE ranges[2] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSceneSrvs, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kSceneUavs, 0, 0, kSceneSrvs},
  };
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {2, ranges};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, sizeof(SceneConstants) / 4};
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, err;
  const D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                         kSceneViews * kTables * D3D12Device::kFramesInFlight,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
             "DLSS 5 scene root signature serialize") ||
      Failed(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&scene_signature_)),
             "DLSS 5 scene root signature") ||
      Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&scene_heap_)), "DLSS 5 scene heap"))
    return false;
  scene_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const pD3DCompile compile = D3DCompiler();
  constexpr char kName[] = "pgr4_dlss5_scene.hlsl";
  // std::addressof: ComPtr's operator& is its own.
  ComPtr<ID3D12PipelineState>* psos[] = {std::addressof(scene_meter_pso_),
                                         std::addressof(scene_exposure_pso_),
                                         std::addressof(scene_encode_pso_),
                                         std::addressof(scene_stabilize_pso_),
                                         std::addressof(scene_compose_pso_)};
  const char* entries[] = {"meter_main", "exposure_main", "encode_main", "stabilize_main",
                           "compose_main"};
  for (int i = 0; i < 5; ++i) {
    ComPtr<ID3DBlob> code, error;
    if (!compile ||
        FAILED(compile(kSceneHlsl, sizeof(kSceneHlsl) - 1, kName, nullptr, nullptr, entries[i],
                       "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) {
      REXLOG_ERROR("PGR4 native DLSS 5: {} {}: {}", kName, entries[i],
                   error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
      return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = scene_signature_.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (Failed(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&*psos[i])), kName))
      return false;
  }
  return true;
}

void NeuralPass::Retire(ComPtr<ID3D12Resource>& r, uint64_t frame) {
  if (r) retired_.push_back({frame, std::move(r)});
  r.Reset();
}

bool NeuralPass::CreateSceneTargets(uint64_t frame, uint32_t width, uint32_t height,
                                    uint32_t work_width, uint32_t work_height,
                                    uint32_t mask_width, uint32_t mask_height) {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  if (!scene_proxy_ || width != scene_width_ || height != scene_height_ ||
      work_width != scene_work_width_ || work_height != scene_work_height_) {
    // The frames in flight may still read the old ones: they go when those are done.
    Retire(scene_proxy_, frame);
    Retire(scene_model_[0], frame);
    Retire(scene_model_[1], frame);
    Retire(scene_composed_, frame);
    Retire(scene_depth_w_, frame);
    Retire(scene_motion_w_, frame);
    Retire(scene_history_[0], frame);
    Retire(scene_history_[1], frame);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    d.Width = work_width;
    d.Height = work_height;
    d.Format = kSceneDepthFormat;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kUav, nullptr,
                                                IID_PPV_ARGS(&scene_depth_w_)),
               "DLSS 5 depth at the working size"))
      return false;
    d.Format = kSceneMotionFormat;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kUav, nullptr,
                                                IID_PPV_ARGS(&scene_motion_w_)),
               "DLSS 5 motion at the working size"))
      return false;
    scene_guides_state_ = kUav;
    d.Format = kSceneFormat;
    for (ComPtr<ID3D12Resource>* r :
         {std::addressof(scene_history_[0]), std::addressof(scene_history_[1])})
      if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kUav, nullptr,
                                                  IID_PPV_ARGS(&*r)),
                 "DLSS 5 change history"))
        return false;
    scene_history_state_[0] = scene_history_state_[1] = kUav;
    scene_history_valid_ = false;
    for (ComPtr<ID3D12Resource>* r :
         {std::addressof(scene_proxy_), std::addressof(scene_model_[0]),
          std::addressof(scene_model_[1])})
      if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kUav, nullptr,
                                                  IID_PPV_ARGS(&*r)),
                 "DLSS 5 scene texture"))
        return false;
    d.Width = width;
    d.Height = height;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kUav, nullptr,
                                                IID_PPV_ARGS(&scene_composed_)),
               "DLSS 5 scene composed frame"))
      return false;
    scene_proxy_state_ = scene_model_state_[0] = scene_model_state_[1] = scene_composed_state_ =
        kUav;
    scene_width_ = width;
    scene_height_ = height;
    scene_work_width_ = work_width;
    scene_work_height_ = work_height;
  }
  if (!scene_mask_ || mask_width != scene_mask_width_ || mask_height != scene_mask_height_) {
    Retire(scene_mask_, frame);
    // The control mask: R the intensity, G the local tone, B the local structure (the DLSS 5
    // header); white, the full effect everywhere.
    d.Width = mask_width;
    d.Height = mask_height;
    d.Format = kMaskFormat4;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE white = {};
    white.Format = kMaskFormat4;
    white.Color[0] = white.Color[1] = white.Color[2] = white.Color[3] = 1.0f;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET, &white,
                                                IID_PPV_ARGS(&scene_mask_)),
               "DLSS 5 scene control mask"))
      return false;
    if (!scene_mask_rtv_) {
      D3D12_DESCRIPTOR_HEAP_DESC hd = {};
      hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      hd.NumDescriptors = 1;
      if (Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&scene_mask_rtv_)),
                 "DLSS 5 scene RTV heap"))
        return false;
    }
    device_->CreateRenderTargetView(scene_mask_.Get(), nullptr,
                                    scene_mask_rtv_->GetCPUDescriptorHandleForHeapStart());
    scene_mask_cleared_ = false;
    scene_mask_width_ = mask_width;
    scene_mask_height_ = mask_height;
  }
  return true;
}

void NeuralPass::SceneDispatch(CommandStream* list, uint64_t frame, uint32_t table,
                               ID3D12PipelineState* pso, const SceneViews& v, uint32_t groups_x,
                               uint32_t groups_y, const void* constants) {
  const uint32_t first =
      (uint32_t(frame % D3D12Device::kFramesInFlight) * kTables + table) * kSceneViews;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = scene_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(first) * scene_stride_;
  static constexpr DXGI_FORMAT kNullSrv[kSceneSrvs] = {kSceneFormat, kSceneFormat, kSceneFormat,
                                                       kSceneDepthFormat, kSceneMotionFormat};
  for (uint32_t i = 0; i < kSceneSrvs; ++i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = v.srv_format[i] != DXGI_FORMAT_UNKNOWN ? v.srv_format[i]
                : v.srv[i]                             ? v.srv[i]->GetDesc().Format
                                                       : kNullSrv[i];
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(v.srv[i], &sd, cpu);
    cpu.ptr += scene_stride_;
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = kSceneFormat;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(v.dst, nullptr, &ud, cpu);
  cpu.ptr += scene_stride_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC bd = {};
  bd.Format = DXGI_FORMAT_R32_TYPELESS;
  bd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  bd.Buffer.NumElements = kExposureBytes / 4;
  bd.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device_->CreateUnorderedAccessView(scene_exposure_.Get(), nullptr, &bd, cpu);
  cpu.ptr += scene_stride_;
  ud.Format = kSceneDepthFormat;
  device_->CreateUnorderedAccessView(v.depth_out, nullptr, &ud, cpu);
  cpu.ptr += scene_stride_;
  ud.Format = kSceneMotionFormat;
  device_->CreateUnorderedAccessView(v.motion_out, nullptr, &ud, cpu);
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = scene_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(first) * scene_stride_;
  ID3D12DescriptorHeap* heaps[] = {scene_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(scene_signature_.Get());
  list->SetPipelineState(pso);
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRoot32BitConstants(1, sizeof(SceneConstants) / 4, constants, 0);
  list->Dispatch(groups_x, groups_y, 1);
}

bool NeuralPass::SceneWanted(const Dlss& dlss) {
  if (!SceneStage() || !dlss.neural_available() || REXCVAR_GET(pgr4_native_dlss5_paused))
    return false;
  const bool racing = CarsSimulated();
  if (racing != scene_racing_) {
    scene_racing_ = racing;
    REXLOG_INFO("PGR4 native DLSS 5 (scene): {}", racing ? "the player drives" : "the front end");
  }
  return racing || REXCVAR_GET(pgr4_native_dlss5_menus);
}

bool NeuralPass::ProcessScene(CommandStream* list, uint64_t frame, Dlss& dlss, RenderTarget* scene,
                              uint32_t width, uint32_t height, const SceneInputs& in) {
  if (!scene || !in.depth || !in.motion || !width || !height ||
      scene->resource_format != kSceneFormat || scene_done_frame_ == frame)
    return false;
  scene_done_frame_ = frame;
  // Resources replaced kFramesInFlight + 2 frames ago are no longer in use.
  std::erase_if(retired_, [frame](const auto& r) {
    return r.first + D3D12Device::kFramesInFlight + 2 < frame;
  });
  // The model's working size (--pgr4_native_dlss5_scale, the forks' WorkingScale; its cost falls
  // with the square); the comparison split needs the full one.
  const int32_t split = REXCVAR_GET(pgr4_native_dlss5_split);
  double work_scale = std::clamp(REXCVAR_GET(pgr4_native_dlss5_scale), 0.5, 1.0);
  if (split == 2) work_scale = 1.0;
  uint32_t work_width = width, work_height = height;
  if (work_scale < 0.999) {
    work_width = std::max(8u, uint32_t(std::lround(width * work_scale)));
    work_height = std::max(8u, uint32_t(std::lround(height * work_scale)));
  }
  if (!CreateScenePasses() || !CreateSceneTargets(frame, width, height, work_width, work_height,
                                                  work_width, work_height))
    return false;
  if (!scene_logged_) {
    scene_logged_ = true;
    REXLOG_INFO("PGR4 native DLSS 5 (scene): on the HDR scene after DLAA, {}x{}, the model at "
                "{}x{} (depth and motion {}x{}), white point {}", width, height, work_width,
                work_height, in.render_width, in.render_height,
                REXCVAR_GET(pgr4_native_dlss5_white) > 0.0 ? "fixed" : "metered");
  }

  Dlss::NeuralFrame nf;
  uint32_t passes = 1;
  ReadSettings(nf.settings, passes);
  const bool gap = scene_last_frame_ + 1 != frame;
  scene_last_frame_ = frame;
  if (gap || in.reset) ++scene_resets_;

  SceneConstants c = {};
  c.size[0] = width;
  c.size[1] = height;
  // 0 (or less): no clamp, the change as the model made it.
  const double max_ratio = REXCVAR_GET(pgr4_native_dlss5_max_ratio);
  c.max_ratio = max_ratio <= 0.0 ? 0.0f : float(std::clamp(max_ratio, 1.0, 16.0));
  c.transfer = float(std::clamp(REXCVAR_GET(pgr4_native_dlss5_transfer), 0.0, 2.0));
  c.colour = float(std::clamp(REXCVAR_GET(pgr4_native_dlss5_colour), 0.0, 1.0));
  c.white_fixed = float(std::max(REXCVAR_GET(pgr4_native_dlss5_white), 0.0));
  c.dt = std::clamp(in.frame_ms * 0.001f, 0.0f, 0.25f);
  c.flags = (split == 1 ? 1u : 0u) | (gap ? 2u : 0u) |
            (REXCVAR_GET(pgr4_native_dlss5_view) == 1 ? 4u : 0u) |
            (REXCVAR_GET(pgr4_native_dlss5_view) == 2 ? 8u : 0u) | (split == 2 ? 16u : 0u);
  c.work[0] = work_width;
  c.work[1] = work_height;
  c.render[0] = in.render_width;
  c.render[1] = in.render_height;
  c.tau_up = float(std::clamp(REXCVAR_GET(pgr4_native_dlss5_meter_up), 0.05, 60.0));
  c.tau_down = float(std::clamp(REXCVAR_GET(pgr4_native_dlss5_meter_down), 0.05, 60.0));
  c.dead_band = float(std::clamp(REXCVAR_GET(pgr4_native_dlss5_meter_band), 0.0, 4.0));
  c.stability = float(std::clamp(REXCVAR_GET(pgr4_native_dlss5_stability), 0.0, 0.95));

  D3D12_RESOURCE_BARRIER bs[4];
  uint32_t nb = 0;
  auto to = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES* state, D3D12_RESOURCE_STATES after) {
    if (*state == after) return;
    bs[nb++] = TransitionBarrier(r, *state, after);
    *state = after;
  };
  auto flush = [&] {
    if (nb) list->ResourceBarrier(nb, bs);
    nb = 0;
  };
  auto uav_barrier = [&](ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    list->ResourceBarrier(1, &b);
  };

  // The scene readable (DLSS SR's writes done), the mask white once.
  to(scene->resource.Get(), &scene->state, kRead);
  if (!scene_mask_cleared_) {
    const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    flush();
    list->ClearRenderTargetView(scene_mask_rtv_->GetCPUDescriptorHandleForHeapStart(), one, 0,
                                nullptr);
    bs[nb++] = TransitionBarrier(scene_mask_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
    scene_mask_cleared_ = true;
  }
  to(scene_proxy_.Get(), &scene_proxy_state_, kUav);
  flush();

  // Meter and white point (the DLAA output, before the model writes anything), then the proxy
  // and the model's guides at its working size.
  SceneViews meter;
  meter.srv[0] = scene->resource.Get();
  if (c.white_fixed <= 0.0f) {
    SceneDispatch(list, frame, kMeterTable, scene_meter_pso_.Get(), meter, (width + 63) / 64,
                  (height + 63) / 64, &c);
    uav_barrier(scene_exposure_.Get());
    SceneDispatch(list, frame, kExposureTable, scene_exposure_pso_.Get(), SceneViews{}, 1, 1, &c);
    uav_barrier(scene_exposure_.Get());
  }
  if (scene_guides_state_ != kUav) {
    bs[nb++] = TransitionBarrier(scene_depth_w_.Get(), scene_guides_state_, kUav);
    bs[nb++] = TransitionBarrier(scene_motion_w_.Get(), scene_guides_state_, kUav);
    scene_guides_state_ = kUav;
    flush();
  }
  SceneViews encode;
  encode.srv[0] = scene->resource.Get();
  encode.srv[3] = in.depth;
  encode.srv_format[3] = in.depth_srv_format;
  encode.srv[4] = in.motion;
  encode.dst = scene_proxy_.Get();
  encode.depth_out = scene_depth_w_.Get();
  encode.motion_out = scene_motion_w_.Get();
  SceneDispatch(list, frame, kEncodeTable, scene_encode_pso_.Get(), encode,
                (work_width + 7) / 8, (work_height + 7) / 8, &c);
  to(scene_proxy_.Get(), &scene_proxy_state_, kRead);
  bs[nb++] = TransitionBarrier(scene_depth_w_.Get(), kUav, kRead);
  bs[nb++] = TransitionBarrier(scene_motion_w_.Get(), kUav, kRead);
  scene_guides_state_ = kRead;
  flush();

  // The model: pass 1 the proxy into model 0, pass 2 model 0 into model 1, pass 3 model 1 into 0.
  // Every input at the working size: the motion's UV-space vectors scaled to its pixels.
  nf.depth = scene_depth_w_.Get();
  nf.motion = scene_motion_w_.Get();
  nf.mask = scene_mask_.Get();
  nf.width = work_width;
  nf.height = work_height;
  nf.render_width = work_width;
  nf.render_height = work_height;
  nf.motion_scale_x = float(work_width);
  nf.motion_scale_y = float(work_height);
  nf.reset = in.reset || gap;
  ID3D12Resource* result = scene_proxy_.Get();
  uint32_t result_index = 0;
  for (uint32_t pass = 0; pass < passes; ++pass) {
    const uint32_t w = pass == 1 ? 1u : 0u;
    ID3D12Resource* read = result;
    to(scene_model_[w].Get(), &scene_model_state_[w], kUav);
    flush();
    Dlss::NeuralFrame pf = nf;
    pf.color = read;
    pf.output = scene_model_[w].Get();
    // Each pass its own history: one that rejoins (more passes again) starts over.
    pf.reset = nf.reset || scene_pass_frame_[pass] + 1 != frame;
    scene_pass_frame_[pass] = frame;
    list->Call([&dlss, pf, pass](CommandStream::List* l) { dlss.EvaluateNeural(l, pf, pass); });
    to(scene_model_[w].Get(), &scene_model_state_[w], kRead);
    flush();
    result = scene_model_[w].Get();
    result_index = w;
  }
  (void)result_index;

  // The metered white point in the log every 120 frames (a copy 120 frames old: long done).
  if (c.white_fixed <= 0.0f && frame % 120 == 0) {
    if (!scene_readback_) {
      D3D12_HEAP_PROPERTIES rp = {};
      rp.Type = D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC rd = {};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      rd.Width = kExposureBytes;
      rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
      rd.SampleDesc.Count = 1;
      rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      void* p = nullptr;
      if (!Failed(device_->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &rd,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&scene_readback_)),
                  "DLSS 5 exposure readback") &&
          SUCCEEDED(scene_readback_->Map(0, nullptr, &p)))
        scene_readback_data_ = static_cast<const float*>(p);
    } else if (scene_readback_data_ && REXCVAR_GET(pgr4_native_dlss5_log_white)) {
      REXLOG_INFO("PGR4 native DLSS 5 (scene): white point {:.4f} (log2 {:.2f}) at frame {}, {} "
                  "history resets since the last line", scene_readback_data_[4],
                  scene_readback_data_[0], frame - 120, scene_resets_);
      scene_resets_ = 0;
    }
    if (scene_readback_) {
      D3D12_RESOURCE_BARRIER b =
          TransitionBarrier(scene_exposure_.Get(), kUav, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->ResourceBarrier(1, &b);
      list->CopyBufferRegion(scene_readback_.Get(), 0, scene_exposure_.Get(), 0, 32);
      b = TransitionBarrier(scene_exposure_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kUav);
      list->ResourceBarrier(1, &b);
    }
  }

  // The change stabilised over time (history flip-flops between two textures).
  {
    const uint32_t cur = scene_history_index_ ^= 1, prev = cur ^ 1;
    SceneConstants sc = c;
    if (!scene_history_valid_ || nf.reset) sc.flags |= 32u;
    to(scene_history_[prev].Get(), &scene_history_state_[prev], kRead);
    to(scene_history_[cur].Get(), &scene_history_state_[cur], kUav);
    flush();
    SceneViews st;
    st.srv[0] = scene_history_[prev].Get();
    st.srv[1] = result;
    st.srv[2] = scene_proxy_.Get();
    st.srv[3] = scene_depth_w_.Get();
    st.srv[4] = scene_motion_w_.Get();
    st.dst = scene_history_[cur].Get();
    SceneDispatch(list, frame, kStabilizeTable, scene_stabilize_pso_.Get(), st,
                  (work_width + 7) / 8, (work_height + 7) / 8, &sc);
    to(scene_history_[cur].Get(), &scene_history_state_[cur], kRead);
    flush();
    scene_history_valid_ = true;
    result = scene_history_[cur].Get();
  }

  // Compare mode: the frames not shown keep the scene as DLAA made it (the model still ran, for
  // its history).
  if (REXCVAR_GET(pgr4_native_dlss5_compare) && !((frame >> 1) & 1)) return true;

  to(scene_composed_.Get(), &scene_composed_state_, kUav);
  flush();
  SceneViews compose;
  compose.srv[0] = scene->resource.Get();
  compose.srv[1] = result;
  compose.srv[2] = scene_proxy_.Get();
  compose.dst = scene_composed_.Get();
  SceneDispatch(list, frame, kComposeTable, scene_compose_pso_.Get(), compose, (width + 7) / 8,
                (height + 7) / 8, &c);
  to(scene_composed_.Get(), &scene_composed_state_, D3D12_RESOURCE_STATE_COPY_SOURCE);
  to(scene->resource.Get(), &scene->state, D3D12_RESOURCE_STATE_COPY_DEST);
  flush();
  D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
  dst.pResource = scene->resource.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.pResource = scene_composed_.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  const D3D12_BOX box = {0, 0, 0, width, height, 1};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  return true;
}

}  // namespace pgr4::native
