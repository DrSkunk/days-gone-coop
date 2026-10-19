#include "menu.hpp"
#include "log.hpp"
#include "overlay.hpp"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

using namespace ue;

namespace coop {
namespace {
constexpr int kRows = 11;
using Color = OverlayColor;
OverlayFrame pendingFrame;

void Rect(Color color, float x, float y, float width, float height) {
    pendingFrame.primitives.push_back({x, y, width, height, 0, color, {}});
}

void Text(const std::string& text, Color color, float x, float y, float scale) {
    pendingFrame.primitives.push_back({x, y, 0, 0, 18 * scale, color, text});
}

WORD GamepadButtons() {
    static unsigned previousConnections = ~0u;
    static ULONGLONG nextConnectionPoll = 0;
    ULONGLONG now = GetTickCount64();
    if (!previousConnections && now < nextConnectionPoll) return 0;
    using GetState = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
    static auto providers = [] {
        const wchar_t* names[] = {L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll"};
        std::array<GetState, 3> result{};
        for (size_t i = 0; i < result.size(); ++i) {
            auto name = names[i];
            HMODULE module = LoadLibraryW(name);
            if (module) result[i] = std::bit_cast<GetState>(GetProcAddress(module, "XInputGetState"));
        }
        return result;
    }();
    WORD buttons = 0;
    unsigned connected = 0;
    for (size_t provider = 0; provider < providers.size(); ++provider) {
        auto get = providers[provider];
        if (!get) continue;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE state{};
            if (get(i, &state) == ERROR_SUCCESS) {
                connected |= 1u << (provider * XUSER_MAX_COUNT + i);
                buttons |= state.Gamepad.wButtons;
            }
        }
    }
    nextConnectionPoll = now + 250;
    if (connected != previousConnections) {
        previousConnections = connected;
        Log("menu: XInput controller mask=0x%x (providers 1_3, 1_4, 9_1_0; four slots each)", connected);
        if (!connected) Log("menu: no XInput controller is exposed; native Steam Input layouts may require gamepad emulation");
    }
    return buttons;
}
} // namespace

const char* PaletteName(int index) {
    static constexpr const char* names[] = {"Original", "Crimson", "Ocean", "Forest", "Sand", "Charcoal", "Violet"};
    return index >= 0 && index < kPaletteCount ? names[index] : "Invalid";
}

bool SettingsMenu::Pressed(int key) {
    bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    bool pressed = down && !keys_[key];
    keys_[key] = down;
    return pressed;
}

bool SettingsMenu::InputHeld() const {
    for (int key : {VK_F8, VK_ESCAPE, VK_RETURN, VK_LBUTTON, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT})
        if (GetAsyncKeyState(key) & 0x8000) return true;
    return buttons_ != 0;
}

void SettingsMenu::RestoreInput() {
    if (IsLive(blockedController_)) {
        if (auto library = FindDefaultObject("WidgetBlueprintLibrary"))
            Call(library, "SetInputMode_GameOnly").set("Target", blockedController_).invoke();
        Call(blockedController_, "EnableInput").set("PlayerController", blockedController_).invoke();
        Call(blockedController_, "SetIgnoreMoveInput").setBool("bNewMoveInput", false).invoke();
        Call(blockedController_, "SetIgnoreLookInput").setBool("bNewLookInput", false).invoke();
    }
    blockedController_ = nullptr;
}

void SettingsMenu::SetOpen(bool open, UObject* controller) {
    if (open && !OverlayReady()) {
        Log("menu: cannot open because the renderer is not ready");
        return;
    }
    if (open_ == open) return;
    // Do not leak the closing Escape/A/click into vanilla pause, jump or attack input.
    if (open || !InputHeld()) RestoreInput();
    open_ = open;
    if (!open_) PublishOverlay({});
    confirmDamage_ = false;
    if (open_ && IsLive(controller)) {
        blockedController_ = controller;
        Call(controller, "DisableInput").set("PlayerController", controller).invoke();
        Call(controller, "SetIgnoreMoveInput").setBool("bNewMoveInput", true).invoke();
        Call(controller, "SetIgnoreLookInput").setBool("bNewLookInput", true).invoke();
        if (auto library = FindDefaultObject("WidgetBlueprintLibrary"))
            Call(library, "SetInputMode_UIOnly").set("Target", controller)
                .set("InWidgetToFocus", static_cast<UObject*>(nullptr)).setBool("bLockMouseToViewport", false).invoke();
        else Log("menu: WidgetBlueprintLibrary unavailable; mouse capture could not be released");
    }
    Log("menu: %s", open_ ? "opened" : "closed");
}

MenuActions SettingsMenu::Update(UObject* controller, bool focused, MenuSettings& settings) {
    MenuActions result;
    bool toggle = Pressed(VK_F8);
    bool passenger = Pressed(VK_F9);
    bool back = Pressed(VK_ESCAPE);
    bool up = Pressed(VK_UP), down = Pressed(VK_DOWN);
    bool left = Pressed(VK_LEFT), right = Pressed(VK_RIGHT);
    bool accept = Pressed(VK_RETURN), click = Pressed(VK_LBUTTON);
    WORD buttons = GamepadButtons(), pressed = buttons & ~buttons_;
    constexpr WORD chord = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_Y;
    toggle |= (buttons & chord) == chord && (buttons_ & chord) != chord;
    constexpr WORD passengerChord = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_X;
    passenger |= (buttons & passengerChord) == passengerChord && (buttons_ & passengerChord) != passengerChord;
    buttons_ = buttons;
    if (!open_ && blockedController_ && (!InputHeld() || blockedController_ != controller))
        RestoreInput();
    if (!focused || (open_ && controller != blockedController_)) {
        SetOpen(false, controller);
        return result;
    }
    if (toggle) SetOpen(!open_, controller);
    result.passenger = passenger;
    if (!open_) return result;
    if (back || (pressed & XINPUT_GAMEPAD_B)) {
        if (confirmDamage_) confirmDamage_ = false;
        else SetOpen(false, controller);
        return result;
    }
    up |= (pressed & XINPUT_GAMEPAD_DPAD_UP) != 0;
    down |= (pressed & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    left |= (pressed & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    right |= (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
    accept |= (pressed & XINPUT_GAMEPAD_A) != 0;
    if (up || down) {
        selected_ = (selected_ + (down ? 1 : kRows - 1)) % kRows;
        confirmDamage_ = false;
    }
    if (click && panelWidth_ > 0) {
        POINT cursor{};
        HWND window = GetForegroundWindow();
        if (GetCursorPos(&cursor) && ScreenToClient(window, &cursor)) {
            RECT client{};
            if (GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
                // HUD coordinates follow the render target, not the Windows desktop size.
                float x = static_cast<float>(cursor.x) * renderWidth_ / client.right;
                float y = static_cast<float>(cursor.y) * renderHeight_ / client.bottom;
                float row = (y - panelY_) / rowHeight_;
                if (x >= panelX_ && x < panelX_ + panelWidth_ && row >= 0 && row < kRows) {
                    selected_ = static_cast<int>(row);
                    accept = true;
                }
            }
        }
    }
    if (!accept && !left && !right) return result;
    int direction = left ? -1 : 1;
    int* palette = selected_ == 0 ? &settings.jacket : selected_ == 1 ? &settings.trousers :
                   selected_ == 2 ? &settings.capColor : nullptr;
    if (palette) {
        *palette = (*palette + direction + kPaletteCount) % kPaletteCount;
        result.changed = true;
    } else if (selected_ == 3) {
        settings.cap = !settings.cap;
        result.changed = true;
    } else if (selected_ == 4) {
        settings.motorcycles = !settings.motorcycles;
        result.changed = true;
    } else if (selected_ == 5) {
        settings.passengers = !settings.passengers;
        result.changed = true;
    } else if (selected_ == 6) {
        if (settings.friendlyFire || confirmDamage_) {
            settings.friendlyFire = !settings.friendlyFire;
            confirmDamage_ = false;
            result.changed = true;
        } else {
            confirmDamage_ = true;
        }
    } else if (selected_ == 7) {
        settings.enemies = !settings.enemies;
        result.changed = true;
    } else if (selected_ == 8 && accept) result.passenger = true;
    else if (selected_ == 9 && accept) result.teleport = true;
    else if (selected_ == 10 && accept) SetOpen(false, controller);
    return result;
}

void SettingsMenu::Draw(int width, int height, const MenuSettings& settings,
                        const std::string& status) {
    if (!open_ || width < 320 || height < 240) return;
    pendingFrame = {width, height, {}};
    renderWidth_ = width;
    renderHeight_ = height;
    float scale = std::min(width / 1280.f, height / 800.f);
    float w = 700 * scale, h = 690 * scale, x = (width - w) * 0.5f, y = (height - h) * 0.5f;
    Color white{0.92f, 0.95f, 1.f, 1.f}, muted{0.55f, 0.64f, 0.72f, 1.f};
    Rect({0.01f, 0.02f, 0.03f, 0.75f}, 0, 0, static_cast<float>(width), static_cast<float>(height));
    Rect({0.025f, 0.045f, 0.065f, 0.98f}, x, y, w, h);
    Rect({0.12f, 0.7f, 0.65f, 1.f}, x, y, w, 4 * scale);
    Text("DAYS GONE / CO-OP", white, x + 28 * scale, y + 22 * scale, 1.8f * scale);
    Text(status, muted, x + 28 * scale, y + 65 * scale, 0.9f * scale);
    std::array<std::string, kRows> labels = {"Jacket color", "Trousers color", "Cap color", "Cap",
        "Sync motorcycles", "Allow a passenger", "Friendly fire", "Sync host's enemies", "Join / leave partner's bike", "Teleport to partner", "Close"};
    std::array<std::string, kRows> values = {PaletteName(settings.jacket), PaletteName(settings.trousers),
        PaletteName(settings.capColor), settings.cap ? "On" : "Off", settings.motorcycles ? "On" : "Off",
        settings.passengers ? "On" : "Off", settings.friendlyFire ? "ON - LETHAL" : "Off", settings.enemies ? "On" : "Off", "", "", ""};
    float rowY = y + 108 * scale, rowH = 40 * scale;
    panelX_ = x; panelY_ = rowY; panelWidth_ = w; rowHeight_ = rowH;
    for (int i = 0; i < kRows; ++i) {
        if (i == selected_) Rect({0.06f, 0.22f, 0.26f, 1.f}, x + 12 * scale, rowY + i * rowH, w - 24 * scale, rowH);
        Text(labels[i], white, x + 28 * scale, rowY + (i * 40 + 10) * scale, scale);
        Text(values[i], i == 6 && settings.friendlyFire ? Color{1.f, 0.35f, 0.25f, 1.f} : muted,
             x + 470 * scale, rowY + (i * 40 + 10) * scale, scale);
    }
    Text(confirmDamage_ ? "Enable real damage and death? Press A / Enter again to confirm." :
         "Friendly fire requires BOTH players to opt in. The world keeps running.",
         confirmDamage_ ? Color{1.f, 0.6f, 0.25f, 1.f} : muted, x + 28 * scale, y + 575 * scale, 0.86f * scale);
    Text("D-pad / arrows: select     Left / right / A: change     B / Esc: close", muted,
         x + 28 * scale, y + 615 * scale, 0.86f * scale);
    Text("Open: F8 or View + Y     Appearance saves automatically", muted,
         x + 28 * scale, y + 644 * scale, 0.86f * scale);
    POINT cursor{};
    RECT client{};
    HWND window = GetForegroundWindow();
    if (GetCursorPos(&cursor) && ScreenToClient(window, &cursor) && GetClientRect(window, &client) &&
        client.right > 0 && client.bottom > 0)
        Text("+", white, static_cast<float>(cursor.x) * width / client.right,
             static_cast<float>(cursor.y) * height / client.bottom, 1.4f * scale);
    PublishOverlay(std::move(pendingFrame));
}

} // namespace coop
