#include "sb2/input.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sb2 {

// The settings module stores raw SDL3 enum values; make sure they match.
static_assert(SDL_SCANCODE_A == 4 && SDL_SCANCODE_RETURN == 40 && SDL_SCANCODE_UP == 82 && SDL_SCANCODE_KP_8 == 96 &&
              SDL_SCANCODE_LSHIFT == 225);
static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0 && SDL_GAMEPAD_BUTTON_START == 6 && SDL_GAMEPAD_BUTTON_LEFT_SHOULDER == 9 &&
              SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14);
static_assert(SDL_GAMEPAD_AXIS_LEFTX == 0 && SDL_GAMEPAD_AXIS_RIGHT_TRIGGER == 5);

namespace {
// ps2::PadButton bit for each digital PadInput (IN_UP..IN_START).
constexpr uint16_t kButtonBit[IN_START + 1] = {
    1 << 4,  1 << 6,  1 << 7,  1 << 5,  // up down left right
    1 << 14, 1 << 13, 1 << 15, 1 << 12, // cross circle square triangle
    1 << 10, 1 << 11, 1 << 8,  1 << 9,  // L1 R1 L2 R2
    1 << 1,  1 << 2,                    // L3 R3
    1 << 0,  1 << 3,                    // select start
};
// Pressure byte index for each digital input (-1 = no pressure sensor).
constexpr int kPressureIndex[IN_START + 1] = {2, 3, 1, 0, 6, 5, 7, 4, 8, 9, 10, 11, -1, -1, -1, -1};

float axisValue(SDL_Gamepad* g, int axis) {
    if (!g || axis < 0) return 0.0f;
    const Sint16 v = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)axis);
    return v >= 0 ? v / 32767.0f : v / 32768.0f;
}

uint8_t toStick(float v) { return (uint8_t)std::clamp((int)std::lround(128.0f + v * 127.5f), 0, 255); }
} // namespace

InputManager::InputManager() {
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    openAll();
}

InputManager::~InputManager() {
    stopRumble();
    closeAll();
}

void InputManager::openAll() {
    closeAll();
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n; ++i)
        if (SDL_Gamepad* g = SDL_OpenGamepad(ids[i])) pads_.push_back(g);
    SDL_free(ids);
}

void InputManager::closeAll() {
    for (SDL_Gamepad* g : pads_) SDL_CloseGamepad(g);
    pads_.clear();
}

void InputManager::handleEvent(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        openAll();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (capturing_ && captured_.kind == CapturedInput::None) {
            captured_.kind = CapturedInput::Button;
            captured_.code = e.gbutton.button;
        } else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
            menuRequest_ = true;
        }
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (capturing_ && captured_.kind == CapturedInput::None && std::abs((int)e.gaxis.value) > 24000) {
            captured_.kind = CapturedInput::Axis;
            captured_.code = e.gaxis.axis;
            captured_.dir = e.gaxis.value < 0 ? -1 : 1;
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (capturing_ && captured_.kind == CapturedInput::None && !e.key.repeat) {
            captured_.kind = CapturedInput::Key;
            captured_.code = e.key.scancode;
        }
        break;
    default:
        break;
    }
}

std::vector<InputManager::DeviceInfo> InputManager::gamepads() const {
    std::vector<DeviceInfo> v;
    for (size_t i = 0; i < pads_.size(); ++i) {
        SDL_Gamepad* g = pads_[i];
        const char* nm = SDL_GetGamepadName(g);
        const char* ty = SDL_GetGamepadStringForType(SDL_GetGamepadType(g));
        const SDL_PropertiesID props = SDL_GetGamepadProperties(g);
        const bool rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        v.push_back({(int)i, nm ? nm : "Gamepad", ty ? ty : "", rumble});
    }
    return v;
}

int InputManager::padForPort(const Settings& s, int port) const {
    const PortSettings& p = s.ports[(size_t)(port & 1)];
    if (!p.enabled || p.device == -1) return -1;
    int slot = p.device;
    if (slot == -2) {
        // Auto: first port takes the first pad, second port the next pad not
        // explicitly used by the first.
        slot = 0;
        if (port == 1) {
            const int other = padForPort(s, 0);
            slot = other == 0 ? 1 : 0;
        }
    }
    return slot >= 0 && slot < (int)pads_.size() ? slot : -1;
}

void InputManager::update(const Settings& s, bool allowKeyboard) {
    int nkeys = 0;
    const bool* keys = SDL_GetKeyboardState(&nkeys);
    auto keyDown = [&](int sc) { return allowKeyboard && sc > 0 && sc < nkeys && keys[sc]; };
    for (int port = 0; port < 2; ++port) {
        const PortSettings& p = s.ports[(size_t)port];
        PadReport r;
        if (!p.enabled) {
            reports_[(size_t)port] = r;
            continue;
        }
        const int slot = padForPort(s, port);
        rumbleSlot_[(size_t)port] = slot;
        SDL_Gamepad* g = slot >= 0 ? pads_[(size_t)slot] : nullptr;
        const bool useKeys = allowKeyboard && (port == 0 || p.device == -1 || p.device == -2);
        auto bound = [&](int in) -> float {
            const Binding& b = p.bind[(size_t)in];
            float v = 0.0f;
            if (useKeys && (keyDown(b.key) || keyDown(b.key2))) v = 1.0f;
            if (g) {
                if (b.button >= 0 && SDL_GetGamepadButton(g, (SDL_GamepadButton)b.button)) v = 1.0f;
                if (b.axis >= 0) v = std::max(v, std::max(0.0f, axisValue(g, b.axis) * (float)b.axisDir));
            }
            return v;
        };
        // Digital buttons (axis-bound buttons such as triggers use a threshold).
        for (int in = IN_UP; in <= IN_START; ++in) {
            const float v = bound(in);
            const Binding& b = p.bind[(size_t)in];
            const bool fromAxis = g && b.axis >= 0 && v > 0.0f && v < 1.0f;
            const bool pressed = fromAxis ? v >= p.triggerThreshold : v >= 0.5f;
            if (pressed) {
                r.buttons |= kButtonBit[in];
                if (kPressureIndex[in] >= 0)
                    r.pressure[(size_t)kPressureIndex[in]] = (uint8_t)std::clamp((int)(v * 255.0f), 1, 255);
            }
        }
        // Analog sticks: gamepad axes with radial deadzone, keys as full
        // deflection (half with the walk modifier).
        auto stick = [&](int negX, int posX, int negY, int posY, uint8_t& ox, uint8_t& oy) {
            float x = 0, y = 0;
            const Binding& bx = p.bind[(size_t)posX];
            const Binding& by = p.bind[(size_t)posY];
            if (g && bx.axis >= 0 && bx.axis == p.bind[(size_t)negX].axis && by.axis >= 0 &&
                by.axis == p.bind[(size_t)negY].axis) {
                x = axisValue(g, bx.axis) * (float)bx.axisDir;
                y = axisValue(g, by.axis) * (float)by.axisDir;
                const float mag = std::sqrt(x * x + y * y);
                if (mag <= p.deadzone) {
                    x = y = 0;
                } else {
                    const float scaled = std::min(1.0f, (mag - p.deadzone) / (1.0f - p.deadzone) * p.sensitivity);
                    x = x / mag * scaled;
                    y = y / mag * scaled;
                }
            } else {
                x = bound(posX) - bound(negX);
                y = bound(posY) - bound(negY);
            }
            // Keyboard / button overrides.
            float kx = 0, ky = 0;
            auto digital = [&](int in) {
                const Binding& b = p.bind[(size_t)in];
                return (useKeys && (keyDown(b.key) || keyDown(b.key2))) ||
                       (g && b.button >= 0 && SDL_GetGamepadButton(g, (SDL_GamepadButton)b.button));
            };
            if (digital(negX)) kx -= 1;
            if (digital(posX)) kx += 1;
            if (digital(negY)) ky -= 1;
            if (digital(posY)) ky += 1;
            if (kx != 0 || ky != 0) {
                const float scale = digital(IN_ANALOG_MOD) ? 0.5f : 1.0f;
                x = kx * scale;
                y = ky * scale;
            }
            ox = toStick(std::clamp(x, -1.0f, 1.0f));
            oy = toStick(std::clamp(y, -1.0f, 1.0f));
        };
        stick(IN_LX_NEG, IN_LX_POS, IN_LY_NEG, IN_LY_POS, r.lx, r.ly);
        stick(IN_RX_NEG, IN_RX_POS, IN_RY_NEG, IN_RY_POS, r.rx, r.ry);
        reports_[(size_t)port] = r;
    }
}

void InputManager::setRumble(int port, uint8_t small, uint8_t large, const PortSettings& p) {
    (void)port;
    if (!p.vibration) return;
    // Port -> pad resolution happens in the caller via padForPort; here we
    // rumble every pad assigned to the port (cheap and robust).
    const float k = p.vibrationStrength;
    const Uint16 lo = (Uint16)std::clamp((int)(large * 257 * k), 0, 65535);
    const Uint16 hi = small ? (Uint16)std::clamp((int)(65535 * k), 0, 65535) : 0;
    const int slot = rumbleSlot_[port & 1];
    if (slot >= 0 && slot < (int)pads_.size()) SDL_RumbleGamepad(pads_[(size_t)slot], lo, hi, 150);
}

void InputManager::stopRumble() {
    for (SDL_Gamepad* g : pads_) SDL_RumbleGamepad(g, 0, 0, 0);
}

void InputManager::beginCapture() {
    capturing_ = true;
    captured_ = CapturedInput{};
}

void InputManager::cancelCapture() {
    capturing_ = false;
    captured_ = CapturedInput{};
}

bool InputManager::pollCapture(CapturedInput& out) {
    if (!capturing_ || captured_.kind == CapturedInput::None) return false;
    out = captured_;
    capturing_ = false;
    captured_ = CapturedInput{};
    return true;
}

bool InputManager::menuRequested() {
    const bool r = menuRequest_;
    menuRequest_ = false;
    return r;
}

std::string InputManager::keyName(int sc) {
    if (sc <= 0) return {};
    const char* n = SDL_GetScancodeName((SDL_Scancode)sc);
    return (n && *n) ? n : ("Key " + std::to_string(sc));
}

std::string InputManager::buttonName(int b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "A/Cross";
    case SDL_GAMEPAD_BUTTON_EAST: return "B/Circle";
    case SDL_GAMEPAD_BUTTON_WEST: return "X/Square";
    case SDL_GAMEPAD_BUTTON_NORTH: return "Y/Triangle";
    case SDL_GAMEPAD_BUTTON_BACK: return "Back";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
    case SDL_GAMEPAD_BUTTON_START: return "Start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "LS";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "RS";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "DPad Up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "DPad Down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "DPad Left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "DPad Right";
    default: return b >= 0 ? "Button " + std::to_string(b) : std::string();
    }
}

std::string InputManager::axisName(int a, int dir) {
    const char* s = dir < 0 ? "-" : "+";
    switch (a) {
    case SDL_GAMEPAD_AXIS_LEFTX: return std::string("LS X") + s;
    case SDL_GAMEPAD_AXIS_LEFTY: return std::string("LS Y") + s;
    case SDL_GAMEPAD_AXIS_RIGHTX: return std::string("RS X") + s;
    case SDL_GAMEPAD_AXIS_RIGHTY: return std::string("RS Y") + s;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "LT";
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "RT";
    default: return a >= 0 ? "Axis " + std::to_string(a) : std::string();
    }
}

std::string InputManager::bindingText(const Binding& b) {
    std::string s;
    auto add = [&](const std::string& t) {
        if (t.empty()) return;
        if (!s.empty()) s += " / ";
        s += t;
    };
    add(keyName(b.key));
    add(keyName(b.key2));
    add(buttonName(b.button));
    if (b.axis >= 0) add(axisName(b.axis, b.axisDir));
    return s.empty() ? "-" : s;
}

} // namespace sb2
