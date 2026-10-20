// Runtime entry points called from recompiled code.  These are the slow
// paths: hardware register access, kernel calls, dynamic dispatch, events.
#pragma once
#include "context.hpp"

namespace ps2 {

// Control flow
void rt_jump(Ctx* c);                       // run code at c->pc until it returns
void rt_badEntry(Ctx* c, uint32_t fnStart); // pc not a valid entry of this function
void rt_invalid(Ctx* c, uint32_t addr, uint32_t raw);
void rt_events(Ctx* c);                     // timers, interrupts, thread switches
void rt_syscall(Ctx* c, uint32_t addr);
void rt_break(Ctx* c, uint32_t addr, uint32_t raw);
void rt_trap(Ctx* c, uint32_t addr);
void rt_eret(Ctx* c);
void rt_unsupported(Ctx* c, uint32_t addr, const char* what);

// Memory slow paths (MMIO, unmapped)
uint8_t rt_read8(Ctx* c, uint32_t a);
uint16_t rt_read16(Ctx* c, uint32_t a);
uint32_t rt_read32(Ctx* c, uint32_t a);
uint64_t rt_read64(Ctx* c, uint32_t a);
void rt_read128(Ctx* c, uint32_t a, Reg128* out);
void rt_write8(Ctx* c, uint32_t a, uint8_t v);
void rt_write16(Ctx* c, uint32_t a, uint16_t v);
void rt_write32(Ctx* c, uint32_t a, uint32_t v);
void rt_write64(Ctx* c, uint32_t a, uint64_t v);
void rt_write128(Ctx* c, uint32_t a, const Reg128* v);

// COP0
uint32_t rt_mfc0(Ctx* c, uint32_t raw);
void rt_mtc0(Ctx* c, uint32_t raw, uint32_t value);
bool rt_cpcond0(Ctx* c);
void rt_ei(Ctx* c);
void rt_di(Ctx* c);

// COP2 / VU0 macro mode (all non-branch COP2 instructions)
void rt_cop2(Ctx* c, uint32_t raw);
bool rt_cop2cond(Ctx* c);

} // namespace ps2
