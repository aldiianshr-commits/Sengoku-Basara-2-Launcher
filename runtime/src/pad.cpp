#include "ps2/pad.hpp"

#include <cstring>

namespace ps2 {

void Pad::setState(int port, const PadState& s) {
    std::lock_guard<std::mutex> lk(mutex_);
    const uint8_t ms = pads_[port & 1].motorSmall, ml = pads_[port & 1].motorLarge;
    pads_[port & 1] = s;
    pads_[port & 1].motorSmall = ms;
    pads_[port & 1].motorLarge = ml;
}

PadState Pad::state(int port) {
    std::lock_guard<std::mutex> lk(mutex_);
    return pads_[port & 1];
}

void Pad::setMotors(int port, uint8_t small, uint8_t large) {
    std::lock_guard<std::mutex> lk(mutex_);
    pads_[port & 1].motorSmall = small;
    pads_[port & 1].motorLarge = large;
}

int Pad::buildReport(int port, uint8_t* out, bool analogMode, bool pressureMode) {
    const PadState s = state(port);
    std::memset(out, 0, 32);
    const uint16_t inv = (uint16_t)~s.buttons;
    out[0] = 0x00;
    if (!analogMode) {
        out[1] = 0x41; // digital
        out[2] = 0x5A;
        out[3] = (uint8_t)(inv & 0xFF);
        out[4] = (uint8_t)(inv >> 8);
        return 5;
    }
    out[1] = pressureMode ? 0x79 : 0x73;
    out[2] = 0x5A;
    out[3] = (uint8_t)(inv & 0xFF);
    out[4] = (uint8_t)(inv >> 8);
    out[5] = s.rx;
    out[6] = s.ry;
    out[7] = s.lx;
    out[8] = s.ly;
    if (!pressureMode) return 9;
    // Pressure order: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2
    const uint16_t order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
    for (int i = 0; i < 12; ++i) out[9 + i] = (s.buttons & order[i]) ? (s.pressure[i] ? s.pressure[i] : 0xFF) : 0;
    return 21;
}

} // namespace ps2
