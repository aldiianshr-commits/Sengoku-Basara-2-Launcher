// Vector units (VU0 / VU1).
//
// VU0 macro mode (COP2 instructions issued by the EE) and micro mode
// (micro programs uploaded by the game and started with VCALLMS or VIF1
// MSCAL) share the same arithmetic implementation.  Micro programs are
// executed by an interpreter over the 64-bit instruction pairs.
#pragma once
#include <cstdint>

#include "context.hpp"

namespace ps2 {

class Machine;

struct VuState {
    Reg128 vf[32];
    uint16_t vi[16];
    Reg128 acc;
    uint32_t q = 0, p = 0, i = 0, r = 0x3F800000;
    uint32_t status = 0, mac = 0, clip = 0;
    uint32_t tpc = 0;
    uint8_t* mem = nullptr;  // data memory
    uint8_t* code = nullptr; // micro memory
    uint32_t memMask = 0, codeMask = 0;
    bool running = false;
    int index = 0;
    // micro-mode bookkeeping
    uint64_t microSteps = 0;
    // Pipeline model (micro mode): flags written by upper ops become visible
    // to lower-pipe flag instructions 4 instruction pairs later; Q (FDIV) and
    // P (EFU) results arrive after the unit's latency.
    bool pipelined = false;
    uint32_t macVis = 0, statusVis = 0, clipVis = 0;
    uint32_t qPending = 0, pPending = 0;
    int qDelay = 0, pDelay = 0; // non-zero while a result is in flight
    uint64_t cycle = 0, qReady = 0, pReady = 0;
};

class Vu {
public:
    explicit Vu(Machine& m);
    VuState& vu0() { return v_[0]; }
    VuState& vu1() { return v_[1]; }
    VuState& state(int i) { return v_[i & 1]; }

    // COP2 macro instruction (any non-branch COP2 op).
    void cop2(Ctx* c, uint32_t raw);
    bool vu0Running() const { return v_[0].running; }

    // Start a micro program; runs to completion (E bit) synchronously.
    void startMicro(int vu, uint32_t addrDoubleWords);
    // Hook for XGKICK (VU1 -> GIF path 1).
    void xgkick(uint32_t vuMemAddr);

    uint32_t fbrst = 0;
    uint32_t cmsar0 = 0, cmsar1 = 0;

private:
    void runMicro(VuState& v);
    void execUpper(VuState& v, uint32_t ins);
    bool execLower(VuState& v, uint32_t ins, uint32_t pc, uint32_t& nextPc, int& branchDelay);
    Machine& m_;
    VuState v_[2];
    uint32_t reg24_ = 0;
};

namespace vuops {
// Arithmetic primitives shared by macro and micro mode.
uint32_t add(uint32_t a, uint32_t b, uint32_t& macBits, int field);
uint32_t sub(uint32_t a, uint32_t b, uint32_t& macBits, int field);
uint32_t mul(uint32_t a, uint32_t b, uint32_t& macBits, int field);
// Upper-pipeline instruction on state `v` (shared by macro and micro mode).
void upper(VuState& v, uint32_t ins);
} // namespace vuops

} // namespace ps2
