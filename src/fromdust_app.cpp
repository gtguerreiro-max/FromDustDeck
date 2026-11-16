// fromdust - ReXGlue port of From Dust (XBLA, title 58410A91), on ReXGlue v0.11.

#include "fromdust_app.h"

#include <filesystem>
#include <string>

#include <rex/setting.h>
#include <rex/settings_registry.h>

#include "generated/default/fromdust_init.h"
#if defined(FROMDUST_NATIVE_RENDERER)
#include "host.h"     // ReXNative's connection to the app
#include "hud/hud.h"  // ReXNative's settings HUD
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void InstallWaterFlowFix();  // water_flow.cpp

namespace {

// The folder of the executable: the game's files (game\) and the player's data (userdata\) are
// beside it.
std::filesystem::path ExeDir() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(std::wstring(path, n)).parent_path();
}

}  // namespace

FromdustApp::FromdustApp() : TitleApp("fromdust", kPPCImageConfig) {}

rex::Result<void> FromdustApp::OnConfigure(rex::SettingsRegistry& settings) {
#if defined(FROMDUST_NATIVE_RENDERER)
  rexnative::host::Configure();
#endif
  const std::filesystem::path dir = ExeDir();
  std::error_code ec;
  if (std::filesystem::is_directory(dir / "game", ec))
    (void)settings.SetText(rex::SettingLayer::kTitle, "title.game_root", (dir / "game").string());
  (void)settings.SetText(rex::SettingLayer::kTitle, "app.user_root", (dir / "userdata").string());
  InstallWaterFlowFix();
  return TitleApp::OnConfigure(settings);
}

rex::Result<void> FromdustApp::OnInit() {
  if (auto r = TitleApp::OnInit(); !r) return r;
#if defined(FROMDUST_NATIVE_RENDERER)
  rexnative::host::Attach(*this);
  rexnative::hud::Initialize();
#endif
  return {};
}

void FromdustApp::OnShutdown() {
#if defined(FROMDUST_NATIVE_RENDERER)
  rexnative::hud::Shutdown();
  rexnative::host::Detach();
#endif
  TitleApp::OnShutdown();
}
