// The port's player settings: a menu over the cvars that give each setting, after PGR4Native's
// in-game Options page (games/pgr4/src/pgr4_settings.cpp). F1 or the pad's Back+Start opens it
// over the running game; the native renderer draws it (native/overlay.h) with an ImGui context of
// its own. A setting the renderer can change while it runs is applied at once; one it takes at
// its start is kept until the next one. Both are saved into the configuration file,
// fromdust.toml, keeping its other lines and comments.
#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::ui {
class Window;
}

namespace fromdust::settings {

// From FromdustApp: the configuration file (OnConfigurePaths) and the window (OnCreateDialogs,
// on the UI thread; registers the key and the input listener).
void SetConfigPath(const std::filesystem::path& config);
void Initialize(rex::ui::Window* window);
void Shutdown();

// Whether the menu is up. Any thread.
bool MenuOpen();

// From the game's XInputGetState hook (frame_rate.cpp), for each pad read: `pad` is the
// XINPUT_GAMEPAD the guest gets (big-endian: buttons, triggers, sticks; 12 bytes). Watches for
// the Back+Start chord (a lone Back or Start reaches the game a moment late) and feeds the menu;
// while the menu is up the pad is cleared and true is returned.
bool FilterPad(uint32_t user, uint8_t* pad);

}  // namespace fromdust::settings
