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
    bool enemies = true;
};

struct MenuActions {
    bool changed = false;
    bool passenger = false;
    bool teleport = false;
    bool lobby = false;  // host, or leave the current lobby
    bool invite = false;
    bool join = false;
    int joinStep = 0;    // -1 / +1 cycles the friend shown on the join row
};

// Extra rows shown when the experimental Steam transport is active.
struct MenuOnline {
    bool enabled = false;
    std::string lobby;  // value on the lobby row, e.g. "Host" or "Leave"
    std::string invite; // value on the invite row
    std::string join;   // value on the join row, e.g. "< Alice >"
};

class SettingsMenu {
public:
    MenuActions Update(ue::UObject* controller, bool focused, MenuSettings& settings, const MenuOnline& online = {});
    void Draw(int width, int height, const MenuSettings& settings,
              const std::string& status, const MenuOnline& online = {});
    bool IsOpen() const { return open_; }
    void Close(ue::UObject* controller) { SetOpen(false, controller); }

private:
    bool open_ = false;
    bool confirmDamage_ = false;
    bool keys_[256]{};
    uint16_t buttons_ = 0;
    int selected_ = 0;
    int rows_ = 11;
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
