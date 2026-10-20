// DualShock 2 controller state shared between the host input layer and the
// IOP pad service.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace ps2 {

class Machine;

// Button bits in DualShock 2 report order (active high here; inverted when
// written into the PS2 report, where 0 = pressed).
enum PadButton : uint16_t {
    PAD_SELECT = 1 << 0,
    PAD_L3 = 1 << 1,
    PAD_R3 = 1 << 2,
    PAD_START = 1 << 3,
    PAD_UP = 1 << 4,
    PAD_RIGHT = 1 << 5,
    PAD_DOWN = 1 << 6,
    PAD_LEFT = 1 << 7,
    PAD_L2 = 1 << 8,
    PAD_R2 = 1 << 9,
    PAD_L1 = 1 << 10,
    PAD_R1 = 1 << 11,
    PAD_TRIANGLE = 1 << 12,
    PAD_CIRCLE = 1 << 13,
    PAD_CROSS = 1 << 14,
    PAD_SQUARE = 1 << 15,
};

struct PadState {
    uint16_t buttons = 0; // PadButton bits, 1 = pressed
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    std::array<uint8_t, 12> pressure{}; // analog pressure for buttons
    bool connected = true;
    bool analog = true;
    uint8_t motorSmall = 0, motorLarge = 0; // set by the game
};

class Pad {
public:
    explicit Pad(Machine& m) : m_(m) {}
    void setState(int port, const PadState& s);
    PadState state(int port);
    void setMotors(int port, uint8_t small, uint8_t large);
    // Builds the 32-byte DualShock 2 report (as returned by the pad over SIO2).
    int buildReport(int port, uint8_t* out, bool analogMode, bool pressureMode);

private:
    Machine& m_;
    std::mutex mutex_;
    PadState pads_[2];
};

} // namespace ps2
