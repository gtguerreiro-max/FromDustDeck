// fromdust - ReXGlue port of From Dust (XBLA, title 58410A91), on ReXGlue v0.11.

#pragma once

#include <rex/app/title_app.h>

class FromdustApp : public rex::app::TitleApp {
 public:
  FromdustApp();

 protected:
  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override;
  rex::Result<void> OnInit() override;
  void OnShutdown() override;
};
