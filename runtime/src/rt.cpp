// Implementation of the rt_* entry points used by recompiled code.
#include <cstdio>
#include <cstdlib>

#include "ps2/hw.hpp"
#include "ps2/kernel.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"
#include "ps2/rt_api.hpp"
#include "ps2/vu.hpp"

namespace ps2 {

void rt_jump(Ctx* c) {
    if (isMagic(c->pc)) return; // returning to the runtime
    Machine* m = c->m;
    if (m->exiting()) throw GuestExit{0};
    RecompFn fn = m->lookup(c->pc);
    if (!fn) {
        m->reportMissingEntry(c->pc);
        m->requestExit(6);
        throw GuestExit{6};
    }
    fn(c);
}

void rt_badEntry(Ctx* c, uint32_t fnStart) {
    Machine* m = c->m;
    logError("runtime: jump to %08x inside function %08x is not a known entry point", c->pc, fnStart);
    m->reportMissingEntry(c->pc);
    m->requestExit(6);
    throw GuestExit{6};
}

void rt_invalid(Ctx* c, uint32_t addr, uint32_t raw) {
    logError("runtime: invalid instruction %08x at %08x", raw, addr);
    c->m->requestExit(8);
    throw GuestExit{8};
}

void rt_unsupported(Ctx* c, uint32_t addr, const char* what) {
    logError("runtime: unsupported construct at %08x: %s", addr, what);
    c->m->requestExit(9);
    throw GuestExit{9};
}

void rt_events(Ctx* c) {
    Machine* m = c->m;
    if (m->exiting()) throw GuestExit{0}; // unwind to the fiber / scheduler
    m->syncFrom(c);
    m->processDueEvents();
    m->syncTo(c);
    m->checkInterrupts(c);
}

void rt_syscall(Ctx* c, uint32_t addr) {
    c->m->kernel->syscall(c, addr);
    if (c->m->exiting()) throw GuestExit{0};
}

void rt_break(Ctx* c, uint32_t addr, uint32_t raw) {
    logWarn("runtime: BREAK %u at %08x", (raw >> 6) & 0xFFFFF, addr);
}

void rt_trap(Ctx* c, uint32_t addr) {
    (void)c;
    logWarn("runtime: trap at %08x", addr);
}

void rt_eret(Ctx* c) { c->pc = MAGIC_HANDLER_RETURN; }

// ------------------------------------------------------------------ memory slow paths

uint8_t rt_read8(Ctx* c, uint32_t a) { return c->m->read8(a); }
uint16_t rt_read16(Ctx* c, uint32_t a) { return c->m->read16(a); }
uint32_t rt_read32(Ctx* c, uint32_t a) {
    const uint32_t v = c->m->read32(a);
    // Busy-wait loops polling hardware registers: let time advance.
    c->cycles += 8;
    return v;
}
uint64_t rt_read64(Ctx* c, uint32_t a) { return c->m->read64(a); }
void rt_read128(Ctx* c, uint32_t a, Reg128* out) { c->m->read128(a, out); }
void rt_write8(Ctx* c, uint32_t a, uint8_t v) {
    c->m->syncFrom(c);
    c->m->write8(a, v);
}
void rt_write16(Ctx* c, uint32_t a, uint16_t v) {
    c->m->syncFrom(c);
    c->m->write16(a, v);
}
void rt_write32(Ctx* c, uint32_t a, uint32_t v) {
    c->m->syncFrom(c);
    c->m->write32(a, v);
    c->m->syncTo(c);
}
void rt_write64(Ctx* c, uint32_t a, uint64_t v) {
    c->m->syncFrom(c);
    c->m->write64(a, v);
    c->m->syncTo(c);
}
void rt_write128(Ctx* c, uint32_t a, const Reg128* v) {
    c->m->syncFrom(c);
    c->m->write128(a, v);
    c->m->syncTo(c);
}

// ------------------------------------------------------------------ COP0

namespace {
// Performance counters (MFPS/MTPS/MFPC/MTPC: COP0 register 25).
struct PerfCounters {
    uint32_t pccr = 0;
    uint32_t pcr[2] = {0, 0};
    int64_t since[2] = {0, 0};
    bool counting(int i) const {
        if (!(pccr & 0x80000000u)) return false;
        const uint32_t ev = (pccr >> (i ? 15 : 5)) & 31;
        const uint32_t modes = (pccr >> (i ? 11 : 1)) & 15;
        return ev == 1 && modes != 0;
    }
    uint32_t read(int i, int64_t now) const {
        return counting(i) ? pcr[i] + (uint32_t)(now - since[i]) : pcr[i];
    }
    void latch(int64_t now) {
        for (int i = 0; i < 2; ++i) {
            pcr[i] = read(i, now);
            since[i] = now;
        }
    }
} g_perf;
} // namespace

uint32_t rt_mfc0(Ctx* c, uint32_t raw) {
    if (((raw >> 11) & 31) == 25) {
        if (raw & 1) return g_perf.read((raw >> 1) & 1, c->cycles); // MFPC
        return g_perf.pccr;                                          // MFPS
    }
    const uint32_t rd = (raw >> 11) & 31;
    Machine* m = c->m;
    switch (rd) {
    case 9: return (uint32_t)c->cycles; // Count
    case 12: { // Status
        uint32_t st = 0x70030C13u & ~0x10000u;
        if (c->intEnabled) st |= 0x10000; // EIE
        return st;
    }
    case 15: return 0x00002E20; // PRId: R5900 rev 2.0
    case 16: return 0x00000440; // Config
    case 25: return 0;          // performance counters
    default: return m->cop0[rd];
    }
}

void rt_mtc0(Ctx* c, uint32_t raw, uint32_t v) {
    const uint32_t rd = (raw >> 11) & 31;
    if (rd == 25) {
        g_perf.latch(c->cycles);
        if (raw & 1) g_perf.pcr[(raw >> 1) & 1] = v;              // MTPC
        else if (((raw >> 1) & 31) == 0) g_perf.pccr = v & 0x800FFBFEu; // MTPS (only specifier 0)
        return;
    }
    if (rd == 12) {
        c->intEnabled = (v & 0x10000) != 0;
        if (c->intEnabled) rt_ei(c);
        return;
    }
    if (rd == 9) return; // Count is derived from emulated time
    c->m->cop0[rd] = v;
}

bool rt_cpcond0(Ctx* c) {
    // CPCOND0 is set when every DMA channel selected in D_PCR.CPC has its
    // D_STAT.CIS bit set.
    const Dmac& d = *c->m->dmac;
    return ((d.pcr & 0x3FF) & ~(d.stat & 0x3FF)) == 0;
}

void rt_ei(Ctx* c) {
    c->intEnabled = true;
    if (!c->inInterrupt) {
        c->m->syncFrom(c);
        c->m->checkInterrupts(c);
    }
}

void rt_di(Ctx* c) { c->intEnabled = false; }

// ------------------------------------------------------------------ COP2

void rt_cop2(Ctx* c, uint32_t raw) { c->m->vu->cop2(c, raw); }
bool rt_cop2cond(Ctx* c) { return c->m->vu->vu0Running(); }

} // namespace ps2
