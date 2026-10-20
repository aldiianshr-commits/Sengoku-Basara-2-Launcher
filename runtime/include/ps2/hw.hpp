// EE-side hardware blocks: interrupt controller, DMA controller, timers.
#pragma once
#include <array>
#include <cstdint>
#include <functional>

#include "context.hpp"

namespace ps2 {

class Machine;

enum IntcCause : int {
    INTC_GS = 0,
    INTC_SBUS = 1,
    INTC_VBLANK_START = 2,
    INTC_VBLANK_END = 3,
    INTC_VIF0 = 4,
    INTC_VIF1 = 5,
    INTC_VU0 = 6,
    INTC_VU1 = 7,
    INTC_IPU = 8,
    INTC_TIM0 = 9,
    INTC_TIM1 = 10,
    INTC_TIM2 = 11,
    INTC_TIM3 = 12,
    INTC_SFIFO = 13,
    INTC_VU0WD = 14,
};

class Intc {
public:
    explicit Intc(Machine& m) : m_(m) {}
    void raise(int cause) { stat |= 1u << cause; }
    uint32_t pending() const { return stat & mask & 0x7FFF; }
    uint32_t stat = 0, mask = 0;

private:
    Machine& m_;
};

enum DmaChannel : int {
    DMA_VIF0 = 0,
    DMA_VIF1 = 1,
    DMA_GIF = 2,
    DMA_FROM_IPU = 3,
    DMA_TO_IPU = 4,
    DMA_SIF0 = 5,
    DMA_SIF1 = 6,
    DMA_SIF2 = 7,
    DMA_FROM_SPR = 8,
    DMA_TO_SPR = 9,
};

struct DmaRegs {
    uint32_t chcr = 0, madr = 0, qwc = 0, tadr = 0, asr0 = 0, asr1 = 0, sadr = 0;
};

class Dmac {
public:
    explicit Dmac(Machine& m) : m_(m) {}
    std::array<DmaRegs, 10> ch;
    uint32_t ctrl = 0, stat = 0, pcr = 0, sqwc = 0, rbsr = 0, rbor = 0, stadr = 0;
    uint32_t enable = 0x1201; // D_ENABLER/W (bit 16 = hold)

    uint32_t read(uint32_t phys);
    void write(uint32_t phys, uint32_t v);
    uint32_t pending() const { return (stat & 0x3FF) & ((stat >> 16) & 0x3FF); }
    void complete(int channel); // set CIS, clear STR
    // Kick a channel (after CHCR.STR set or when its peer becomes ready).
    void kick(int channel);
    // SIF0 helper: the IOP side delivers data into EE memory.
    bool sif0Armed() const { return (ch[DMA_SIF0].chcr & 0x100) != 0; }

private:
    void runNormalOrChain(int channel);
    void transferToSpr(int channel);
    void transferFromSpr(int channel);
    void transferGifVif(int channel);
    void transferSif1();
    Machine& m_;
};

class Timers {
public:
    explicit Timers(Machine& m) : m_(m) {}
    struct T {
        uint32_t mode = 0, comp = 0, hold = 0;
        uint32_t countBase = 0;
        int64_t baseCycle = 0;
        uint64_t evt = 0;
    };
    std::array<T, 4> t;
    uint32_t read(uint32_t phys);
    void write(uint32_t phys, uint32_t v);
    void onHblank();
    uint32_t count(int i) const;

private:
    int64_t divider(int i) const;
    void reschedule(int i);
    Machine& m_;
};

} // namespace ps2
