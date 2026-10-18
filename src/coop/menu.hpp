#pragma once

#include "ue.hpp"

#include <string>

namespace coop {

struct MenuSettings {
    int jacket = 0;
    int trousers = 0;
    int capColor = 0;
    bool cap = true;
    bool motorcycles = true;
    bool passengers = true;
    bool friendlyFire = false;
};

struct MenuActions {
    bool changed = false;
    bool passenger = false;
    bool teleport = false;
};

class SettingsMenu {
public:
    MenuActions Update(ue::UObject* controller, bool focused, MenuSettings& settings);
    void Draw(int width, int height, const MenuSettings& settings,
              const std::string& status);
    bool IsOpen() const { return open_; }
    void Close(ue::UObject* controller) { SetOpen(false, controller); }

private:
    bool open_ = false;
    bool confirmDamage_ = false;
    bool keys_[256]{};
    uint16_t buttons_ = 0;
    int selected_ = 0;
    float panelX_ = 0, panelY_ = 0, panelWidth_ = 0, rowHeight_ = 0;
    int renderWidth_ = 0, renderHeight_ = 0;
    ue::UObject* blockedController_ = nullptr;
    bool Pressed(int key);
    bool InputHeld() const;
    void RestoreInput();
    void SetOpen(bool open, ue::UObject* controller);
};

const char* PaletteName(int index);
constexpr int kPaletteCount = 7;

} // namespace coop
