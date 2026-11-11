// fromdust - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>

#include "crash_report.h"
#if defined(FROMDUST_NATIVE_RENDERER)
#include "fromdust_settings.h"
#endif

void InstallWaterFlowFix();  // water_flow.cpp
namespace rex {
class Runtime;
}
void StartGuestCaptures(rex::Runtime* runtime);  // guest_capture.cpp

class FromdustApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  void OnPostInitLogging() override {
    InstallCrashReport();
    InstallWaterFlowFix();
  }

  void OnPostSetup() override { StartGuestCaptures(runtime()); }

#if defined(FROMDUST_NATIVE_RENDERER)
  // The settings menu (F1 or Back+Start, fromdust_settings.h) writes the configuration file
  // ReXGlue loads.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    fromdust::settings::SetConfigPath(paths.config_path);
  }
  void OnCreateDialogs(rex::ui::ImGuiDrawer*) override { fromdust::settings::Initialize(window()); }
  void OnShutdown() override { fromdust::settings::Shutdown(); }
#endif

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<FromdustApp>(new FromdustApp(ctx, "fromdust",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
