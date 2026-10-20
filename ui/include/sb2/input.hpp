// Keyboard and gamepad input (SDL3: XInput, DualShock/DualSense, Switch and
// other controllers known to SDL's gamepad database) mapped to DualShock 2
// reports according to the user's bindings.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sb2/settings.hpp"

union SDL_Event;
struct SDL_Gamepad;

namespace sb2 {

// Button bits match ps2::PadButton (1 = pressed).
struct PadReport {
    uint16_t buttons = 0;
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    std::array<uint8_t, 12> pressure{}; // right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2
};

struct CapturedInput {
    enum Kind { None, Key, Button, Axis } kind = None;
    int code = 0;
    int dir = 1;
};

class InputManager {
public:
    InputManager();
    ~InputManager();
    void handleEvent(const SDL_Event& e);
    // Computes both port reports from the current device state.
    void update(const Settings& s, bool allowKeyboard);
    const PadReport& report(int port) const { return reports_[port & 1]; }
    // Motor values from the game (small: 0/1, large: 0..255).
    void setRumble(int port, uint8_t small, uint8_t large, const PortSettings& p);
    void stopRumble();

    struct DeviceInfo {
        int slot;
        std::string name;
        std::string type;
        bool rumble;
    };
    std::vector<DeviceInfo> gamepads() const;
    // Gamepad slot driving a port (-1 = none).
    int padForPort(const Settings& s, int port) const;

    // Remapping: start capturing, then poll until a key/button/axis moves.
    void beginCapture();
    void cancelCapture();
    bool capturing() const { return capturing_; }
    bool pollCapture(CapturedInput& out);

    // UI helpers.
    static std::string keyName(int scancode);
    static std::string buttonName(int button);
    static std::string axisName(int axis, int dir);
    static std::string bindingText(const Binding& b);

    // True while the gamepad "guide"/home button or Escape requests the menu.
    bool menuRequested();

private:
    void openAll();
    void closeAll();
    std::vector<SDL_Gamepad*> pads_;
    std::array<PadReport, 2> reports_{};
    bool capturing_ = false;
    CapturedInput captured_;
    std::vector<int16_t> captureBaseline_;
    bool menuRequest_ = false;
    std::array<int, 2> rumbleSlot_{-1, -1};
};

} // namespace sb2
