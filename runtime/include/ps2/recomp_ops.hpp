// Semantics of every non-control-flow R5900 instruction.
//
// Recompiled code calls these as  NAME(c, 0xRAWWORD);  Because the raw word
// is a compile-time constant at every call site, the field extraction below
// folds away after inlining and each call becomes a handful of host
// instructions.  This header is the single source of truth for instruction
// behaviour; it is validated against results recorded on real PS2 hardware
// (ps2autotests).
#pragma once
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>

#include "context.hpp"
#include "fpu.hpp"
#include "rt_api.hpp"

namespace ps2::ops {

#define PS2_RS ((raw >> 21) & 31)
#define PS2_RT ((raw >> 16) & 31)
#define PS2_RD ((raw >> 11) & 31)
#define PS2_SA ((raw >> 6) & 31)
#define PS2_IMM ((int32_t)(int16_t)(raw & 0xFFFF))
#define PS2_UIMM (raw & 0xFFFFu)
#define PS2_OP(name) PS2_FORCEINLINE void name([[maybe_unused]] Ctx* c, [[maybe_unused]] uint32_t raw)

PS2_FORCEINLINE uint64_t sx32(uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; }
// Portable overflow checks (also usable with MSVC).
PS2_FORCEINLINE bool addOv32(int32_t a, int32_t b, int32_t* r) {
    const int64_t v = (int64_t)a + b;
    *r = (int32_t)v;
    return v != (int64_t)*r;
}
PS2_FORCEINLINE bool subOv32(int32_t a, int32_t b, int32_t* r) {
    const int64_t v = (int64_t)a - b;
    *r = (int32_t)v;
    return v != (int64_t)*r;
}
PS2_FORCEINLINE bool addOv64(int64_t a, int64_t b, int64_t* r) {
    const uint64_t v = (uint64_t)a + (uint64_t)b;
    *r = (int64_t)v;
    return ((a ^ *r) & (b ^ *r)) < 0;
}
PS2_FORCEINLINE bool subOv64(int64_t a, int64_t b, int64_t* r) {
    const uint64_t v = (uint64_t)a - (uint64_t)b;
    *r = (int64_t)v;
    return ((a ^ b) & (a ^ *r)) < 0;
}
PS2_FORCEINLINE uint32_t clz32(uint32_t v) {
    if (v == 0) return 32;
    uint32_t n = 0;
    while (!(v & 0x80000000u)) { v <<= 1; ++n; }
    return n;
}
// 64x64 -> 128 multiply helpers (portable).
PS2_FORCEINLINE void mulU64(uint64_t a, uint64_t b, uint64_t* lo, uint64_t* hi) {
    const uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    const uint64_t p0 = al * bl, p1 = al * bh, p2 = ah * bl, p3 = ah * bh;
    const uint64_t mid = (p0 >> 32) + (uint32_t)p1 + (uint32_t)p2;
    *lo = (mid << 32) | (uint32_t)p0;
    *hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}
PS2_FORCEINLINE void set64(Ctx* c, uint32_t r, uint64_t v) {
    if (r) c->r[r].u64[0] = v;
}
PS2_FORCEINLINE uint32_t addrOf(Ctx* c, uint32_t raw) { return c->r[PS2_RS].u32[0] + (uint32_t)PS2_IMM; }

// ---------------------------------------------------------------- memory
template <typename T>
PS2_FORCEINLINE T load(Ctx* c, uint32_t a) {
    uint8_t* p = c->rpage[a >> 12];
    if (PS2_LIKELY(p != nullptr)) {
        T v;
        std::memcpy(&v, p + (a & 0xFFF), sizeof(T));
        return v;
    }
    if constexpr (sizeof(T) == 1) return (T)rt_read8(c, a);
    else if constexpr (sizeof(T) == 2) return (T)rt_read16(c, a);
    else if constexpr (sizeof(T) == 4) return (T)rt_read32(c, a);
    else return (T)rt_read64(c, a);
}
template <typename T>
PS2_FORCEINLINE void store(Ctx* c, uint32_t a, T v) {
    uint8_t* p = c->wpage[a >> 12];
    if (PS2_LIKELY(p != nullptr)) {
        std::memcpy(p + (a & 0xFFF), &v, sizeof(T));
        return;
    }
    if constexpr (sizeof(T) == 1) rt_write8(c, a, (uint8_t)v);
    else if constexpr (sizeof(T) == 2) rt_write16(c, a, (uint16_t)v);
    else if constexpr (sizeof(T) == 4) rt_write32(c, a, (uint32_t)v);
    else rt_write64(c, a, (uint64_t)v);
}
PS2_FORCEINLINE void load128(Ctx* c, uint32_t a, Reg128* out) {
    a &= ~15u;
    uint8_t* p = c->rpage[a >> 12];
    if (PS2_LIKELY(p != nullptr)) std::memcpy(out, p + (a & 0xFFF), 16);
    else rt_read128(c, a, out);
}
PS2_FORCEINLINE void store128(Ctx* c, uint32_t a, const Reg128* v) {
    a &= ~15u;
    uint8_t* p = c->wpage[a >> 12];
    if (PS2_LIKELY(p != nullptr)) std::memcpy(p + (a & 0xFFF), v, 16);
    else rt_write128(c, a, v);
}

// ---------------------------------------------------------------- shifts
PS2_OP(SLL) { set64(c, PS2_RD, sx32(c->r[PS2_RT].u32[0] << PS2_SA)); }
PS2_OP(SRL) { set64(c, PS2_RD, sx32(c->r[PS2_RT].u32[0] >> PS2_SA)); }
PS2_OP(SRA) { set64(c, PS2_RD, sx32((uint32_t)(c->r[PS2_RT].s32[0] >> PS2_SA))); }
PS2_OP(SLLV) { set64(c, PS2_RD, sx32(c->r[PS2_RT].u32[0] << (c->r[PS2_RS].u32[0] & 31))); }
PS2_OP(SRLV) { set64(c, PS2_RD, sx32(c->r[PS2_RT].u32[0] >> (c->r[PS2_RS].u32[0] & 31))); }
PS2_OP(SRAV) { set64(c, PS2_RD, sx32((uint32_t)(c->r[PS2_RT].s32[0] >> (c->r[PS2_RS].u32[0] & 31)))); }
PS2_OP(DSLLV) { set64(c, PS2_RD, c->r[PS2_RT].u64[0] << (c->r[PS2_RS].u32[0] & 63)); }
PS2_OP(DSRLV) { set64(c, PS2_RD, c->r[PS2_RT].u64[0] >> (c->r[PS2_RS].u32[0] & 63)); }
PS2_OP(DSRAV) { set64(c, PS2_RD, (uint64_t)(c->r[PS2_RT].s64[0] >> (c->r[PS2_RS].u32[0] & 63))); }
PS2_OP(DSLL) { set64(c, PS2_RD, c->r[PS2_RT].u64[0] << PS2_SA); }
PS2_OP(DSRL) { set64(c, PS2_RD, c->r[PS2_RT].u64[0] >> PS2_SA); }
PS2_OP(DSRA) { set64(c, PS2_RD, (uint64_t)(c->r[PS2_RT].s64[0] >> PS2_SA)); }
PS2_OP(DSLL32) { set64(c, PS2_RD, c->r[PS2_RT].u64[0] << (PS2_SA + 32)); }
PS2_OP(DSRL32) { set64(c, PS2_RD, c->r[PS2_RT].u64[0] >> (PS2_SA + 32)); }
PS2_OP(DSRA32) { set64(c, PS2_RD, (uint64_t)(c->r[PS2_RT].s64[0] >> (PS2_SA + 32))); }

// ---------------------------------------------------------------- moves / misc
PS2_OP(MOVZ) {
    if (c->r[PS2_RT].u64[0] == 0) set64(c, PS2_RD, c->r[PS2_RS].u64[0]);
}
PS2_OP(MOVN) {
    if (c->r[PS2_RT].u64[0] != 0) set64(c, PS2_RD, c->r[PS2_RS].u64[0]);
}
PS2_OP(SYNC) {}
PS2_OP(CACHE) {}
PS2_OP(PREF) {}
PS2_OP(MFHI) { set64(c, PS2_RD, c->hi.u64[0]); }
PS2_OP(MTHI) { c->hi.u64[0] = c->r[PS2_RS].u64[0]; }
PS2_OP(MFLO) { set64(c, PS2_RD, c->lo.u64[0]); }
PS2_OP(MTLO) { c->lo.u64[0] = c->r[PS2_RS].u64[0]; }
PS2_OP(MFHI1) { set64(c, PS2_RD, c->hi.u64[1]); }
PS2_OP(MTHI1) { c->hi.u64[1] = c->r[PS2_RS].u64[0]; }
PS2_OP(MFLO1) { set64(c, PS2_RD, c->lo.u64[1]); }
PS2_OP(MTLO1) { c->lo.u64[1] = c->r[PS2_RS].u64[0]; }
PS2_OP(MFSA) { set64(c, PS2_RD, c->sa); }
PS2_OP(MTSA) { c->sa = c->r[PS2_RS].u32[0] & 15; }
PS2_OP(MTSAB) { c->sa = (c->r[PS2_RS].u32[0] ^ PS2_UIMM) & 15; }
PS2_OP(MTSAH) { c->sa = ((c->r[PS2_RS].u32[0] ^ PS2_UIMM) & 7) * 2; }

// ---------------------------------------------------------------- multiply / divide
PS2_FORCEINLINE void setHiLo(Ctx* c, int pipe, uint64_t prod) {
    c->lo.u64[pipe] = sx32((uint32_t)prod);
    c->hi.u64[pipe] = sx32((uint32_t)(prod >> 32));
}
PS2_FORCEINLINE void divS(Ctx* c, int pipe, int32_t n, int32_t d) {
    if (d == 0) {
        c->lo.u64[pipe] = n < 0 ? 1 : ~0ull;
        c->hi.u64[pipe] = sx32((uint32_t)n);
    } else if (n == INT_MIN && d == -1) {
        c->lo.u64[pipe] = sx32(0x80000000u);
        c->hi.u64[pipe] = 0;
    } else {
        c->lo.u64[pipe] = sx32((uint32_t)(n / d));
        c->hi.u64[pipe] = sx32((uint32_t)(n % d));
    }
}
PS2_FORCEINLINE void divU(Ctx* c, int pipe, uint32_t n, uint32_t d) {
    if (d == 0) {
        c->lo.u64[pipe] = ~0ull;
        c->hi.u64[pipe] = sx32(n);
    } else {
        c->lo.u64[pipe] = sx32(n / d);
        c->hi.u64[pipe] = sx32(n % d);
    }
}
PS2_OP(MULT) {
    setHiLo(c, 0, (uint64_t)((int64_t)c->r[PS2_RS].s32[0] * c->r[PS2_RT].s32[0]));
    set64(c, PS2_RD, c->lo.u64[0]);
}
PS2_OP(MULTU) {
    setHiLo(c, 0, (uint64_t)c->r[PS2_RS].u32[0] * c->r[PS2_RT].u32[0]);
    set64(c, PS2_RD, c->lo.u64[0]);
}
PS2_OP(MULT1) {
    setHiLo(c, 1, (uint64_t)((int64_t)c->r[PS2_RS].s32[0] * c->r[PS2_RT].s32[0]));
    set64(c, PS2_RD, c->lo.u64[1]);
}
PS2_OP(MULTU1) {
    setHiLo(c, 1, (uint64_t)c->r[PS2_RS].u32[0] * c->r[PS2_RT].u32[0]);
    set64(c, PS2_RD, c->lo.u64[1]);
}
PS2_OP(DIV) { divS(c, 0, c->r[PS2_RS].s32[0], c->r[PS2_RT].s32[0]); }
PS2_OP(DIVU) { divU(c, 0, c->r[PS2_RS].u32[0], c->r[PS2_RT].u32[0]); }
PS2_OP(DIV1) { divS(c, 1, c->r[PS2_RS].s32[0], c->r[PS2_RT].s32[0]); }
PS2_OP(DIVU1) { divU(c, 1, c->r[PS2_RS].u32[0], c->r[PS2_RT].u32[0]); }
PS2_FORCEINLINE uint64_t hilo32(Ctx* c, int pipe) {
    return ((uint64_t)c->hi.u32[pipe * 2] << 32) | c->lo.u32[pipe * 2];
}
PS2_OP(MADD) {
    uint64_t t = hilo32(c, 0) + (uint64_t)((int64_t)c->r[PS2_RS].s32[0] * c->r[PS2_RT].s32[0]);
    setHiLo(c, 0, t);
    set64(c, PS2_RD, c->lo.u64[0]);
}
PS2_OP(MADDU) {
    uint64_t t = hilo32(c, 0) + (uint64_t)c->r[PS2_RS].u32[0] * c->r[PS2_RT].u32[0];
    setHiLo(c, 0, t);
    set64(c, PS2_RD, c->lo.u64[0]);
}
PS2_OP(MADD1) {
    uint64_t t = hilo32(c, 1) + (uint64_t)((int64_t)c->r[PS2_RS].s32[0] * c->r[PS2_RT].s32[0]);
    setHiLo(c, 1, t);
    set64(c, PS2_RD, c->lo.u64[1]);
}
PS2_OP(MADDU1) {
    uint64_t t = hilo32(c, 1) + (uint64_t)c->r[PS2_RS].u32[0] * c->r[PS2_RT].u32[0];
    setHiLo(c, 1, t);
    set64(c, PS2_RD, c->lo.u64[1]);
}
// 64-bit multiply/divide (MIPS III; not implemented by the R5900 but harmless).
PS2_OP(DMULT) {
    const int64_t a = c->r[PS2_RS].s64[0], b = c->r[PS2_RT].s64[0];
    uint64_t lo, hi;
    mulU64((uint64_t)a, (uint64_t)b, &lo, &hi);
    if (a < 0) hi -= (uint64_t)b;
    if (b < 0) hi -= (uint64_t)a;
    c->lo.u64[0] = lo;
    c->hi.u64[0] = hi;
}
PS2_OP(DMULTU) { mulU64(c->r[PS2_RS].u64[0], c->r[PS2_RT].u64[0], &c->lo.u64[0], &c->hi.u64[0]); }
PS2_OP(DDIV) {
    int64_t n = c->r[PS2_RS].s64[0], d = c->r[PS2_RT].s64[0];
    if (d == 0 || (n == INT64_MIN && d == -1)) return;
    c->lo.s64[0] = n / d;
    c->hi.s64[0] = n % d;
}
PS2_OP(DDIVU) {
    uint64_t n = c->r[PS2_RS].u64[0], d = c->r[PS2_RT].u64[0];
    if (d == 0) return;
    c->lo.u64[0] = n / d;
    c->hi.u64[0] = n % d;
}

// ---------------------------------------------------------------- ALU
PS2_OP(ADD) {
    int32_t a = c->r[PS2_RS].s32[0], b = c->r[PS2_RT].s32[0];
    int32_t r;
    if (addOv32(a, b, &r)) return; // overflow exception: rd unchanged
    set64(c, PS2_RD, sx32((uint32_t)r));
}
PS2_OP(ADDU) { set64(c, PS2_RD, sx32(c->r[PS2_RS].u32[0] + c->r[PS2_RT].u32[0])); }
PS2_OP(SUB) {
    int32_t a = c->r[PS2_RS].s32[0], b = c->r[PS2_RT].s32[0];
    int32_t r;
    if (subOv32(a, b, &r)) return;
    set64(c, PS2_RD, sx32((uint32_t)r));
}
PS2_OP(SUBU) { set64(c, PS2_RD, sx32(c->r[PS2_RS].u32[0] - c->r[PS2_RT].u32[0])); }
PS2_OP(DADD) {
    int64_t r;
    if (addOv64(c->r[PS2_RS].s64[0], c->r[PS2_RT].s64[0], &r)) return;
    set64(c, PS2_RD, (uint64_t)r);
}
PS2_OP(DADDU) { set64(c, PS2_RD, c->r[PS2_RS].u64[0] + c->r[PS2_RT].u64[0]); }
PS2_OP(DSUB) {
    int64_t r;
    if (subOv64(c->r[PS2_RS].s64[0], c->r[PS2_RT].s64[0], &r)) return;
    set64(c, PS2_RD, (uint64_t)r);
}
PS2_OP(DSUBU) { set64(c, PS2_RD, c->r[PS2_RS].u64[0] - c->r[PS2_RT].u64[0]); }
PS2_OP(AND) { set64(c, PS2_RD, c->r[PS2_RS].u64[0] & c->r[PS2_RT].u64[0]); }
PS2_OP(OR) { set64(c, PS2_RD, c->r[PS2_RS].u64[0] | c->r[PS2_RT].u64[0]); }
PS2_OP(XOR) { set64(c, PS2_RD, c->r[PS2_RS].u64[0] ^ c->r[PS2_RT].u64[0]); }
PS2_OP(NOR) { set64(c, PS2_RD, ~(c->r[PS2_RS].u64[0] | c->r[PS2_RT].u64[0])); }
PS2_OP(SLT) { set64(c, PS2_RD, c->r[PS2_RS].s64[0] < c->r[PS2_RT].s64[0] ? 1 : 0); }
PS2_OP(SLTU) { set64(c, PS2_RD, c->r[PS2_RS].u64[0] < c->r[PS2_RT].u64[0] ? 1 : 0); }
PS2_OP(ADDI) {
    int32_t r;
    if (addOv32(c->r[PS2_RS].s32[0], PS2_IMM, &r)) return;
    set64(c, PS2_RT, sx32((uint32_t)r));
}
PS2_OP(ADDIU) { set64(c, PS2_RT, sx32(c->r[PS2_RS].u32[0] + (uint32_t)PS2_IMM)); }
PS2_OP(DADDI) {
    int64_t r;
    if (addOv64(c->r[PS2_RS].s64[0], (int64_t)PS2_IMM, &r)) return;
    set64(c, PS2_RT, (uint64_t)r);
}
PS2_OP(DADDIU) { set64(c, PS2_RT, c->r[PS2_RS].u64[0] + (uint64_t)(int64_t)PS2_IMM); }
PS2_OP(SLTI) { set64(c, PS2_RT, c->r[PS2_RS].s64[0] < (int64_t)PS2_IMM ? 1 : 0); }
PS2_OP(SLTIU) { set64(c, PS2_RT, c->r[PS2_RS].u64[0] < (uint64_t)(int64_t)PS2_IMM ? 1 : 0); }
PS2_OP(ANDI) { set64(c, PS2_RT, c->r[PS2_RS].u64[0] & PS2_UIMM); }
PS2_OP(ORI) { set64(c, PS2_RT, c->r[PS2_RS].u64[0] | PS2_UIMM); }
PS2_OP(XORI) { set64(c, PS2_RT, c->r[PS2_RS].u64[0] ^ PS2_UIMM); }
PS2_OP(LUI) { set64(c, PS2_RT, sx32(PS2_UIMM << 16)); }

// ---------------------------------------------------------------- traps
PS2_OP(TGE) { if (c->r[PS2_RS].s64[0] >= c->r[PS2_RT].s64[0]) rt_trap(c, 0); }
PS2_OP(TGEU) { if (c->r[PS2_RS].u64[0] >= c->r[PS2_RT].u64[0]) rt_trap(c, 0); }
PS2_OP(TLT) { if (c->r[PS2_RS].s64[0] < c->r[PS2_RT].s64[0]) rt_trap(c, 0); }
PS2_OP(TLTU) { if (c->r[PS2_RS].u64[0] < c->r[PS2_RT].u64[0]) rt_trap(c, 0); }
PS2_OP(TEQ) { if (c->r[PS2_RS].u64[0] == c->r[PS2_RT].u64[0]) rt_trap(c, 0); }
PS2_OP(TNE) { if (c->r[PS2_RS].u64[0] != c->r[PS2_RT].u64[0]) rt_trap(c, 0); }
PS2_OP(TGEI) { if (c->r[PS2_RS].s64[0] >= (int64_t)PS2_IMM) rt_trap(c, 0); }
PS2_OP(TGEIU) { if (c->r[PS2_RS].u64[0] >= (uint64_t)(int64_t)PS2_IMM) rt_trap(c, 0); }
PS2_OP(TLTI) { if (c->r[PS2_RS].s64[0] < (int64_t)PS2_IMM) rt_trap(c, 0); }
PS2_OP(TLTIU) { if (c->r[PS2_RS].u64[0] < (uint64_t)(int64_t)PS2_IMM) rt_trap(c, 0); }
PS2_OP(TEQI) { if (c->r[PS2_RS].s64[0] == (int64_t)PS2_IMM) rt_trap(c, 0); }
PS2_OP(TNEI) { if (c->r[PS2_RS].s64[0] != (int64_t)PS2_IMM) rt_trap(c, 0); }

// ---------------------------------------------------------------- loads / stores
PS2_OP(LB) { set64(c, PS2_RT, (uint64_t)(int64_t)load<int8_t>(c, addrOf(c, raw))); }
PS2_OP(LBU) { set64(c, PS2_RT, load<uint8_t>(c, addrOf(c, raw))); }
PS2_OP(LH) { set64(c, PS2_RT, (uint64_t)(int64_t)load<int16_t>(c, addrOf(c, raw))); }
PS2_OP(LHU) { set64(c, PS2_RT, load<uint16_t>(c, addrOf(c, raw))); }
PS2_OP(LW) { set64(c, PS2_RT, sx32(load<uint32_t>(c, addrOf(c, raw)))); }
PS2_OP(LWU) { set64(c, PS2_RT, load<uint32_t>(c, addrOf(c, raw))); }
PS2_OP(LD) { set64(c, PS2_RT, load<uint64_t>(c, addrOf(c, raw))); }
PS2_OP(LQ) {
    Reg128 v;
    load128(c, addrOf(c, raw), &v);
    if (PS2_RT) c->r[PS2_RT] = v;
}
PS2_OP(SB) { store<uint8_t>(c, addrOf(c, raw), c->r[PS2_RT].u8[0]); }
PS2_OP(SH) { store<uint16_t>(c, addrOf(c, raw), c->r[PS2_RT].u16[0]); }
PS2_OP(SW) { store<uint32_t>(c, addrOf(c, raw), c->r[PS2_RT].u32[0]); }
PS2_OP(SD) { store<uint64_t>(c, addrOf(c, raw), c->r[PS2_RT].u64[0]); }
PS2_OP(SQ) { store128(c, addrOf(c, raw), &c->r[PS2_RT]); }

PS2_OP(LWL) {
    static constexpr uint32_t MASK[4] = {0x00ffffff, 0x0000ffff, 0x000000ff, 0x00000000};
    static constexpr uint32_t SHIFT[4] = {24, 16, 8, 0};
    const uint32_t a = addrOf(c, raw), s = a & 3;
    const uint32_t mem = load<uint32_t>(c, a & ~3u);
    set64(c, PS2_RT, sx32((c->r[PS2_RT].u32[0] & MASK[s]) | (mem << SHIFT[s])));
}
PS2_OP(LWR) {
    static constexpr uint32_t MASK[4] = {0x00000000, 0xff000000, 0xffff0000, 0xffffff00};
    static constexpr uint32_t SHIFT[4] = {0, 8, 16, 24};
    const uint32_t a = addrOf(c, raw), s = a & 3;
    const uint32_t mem = load<uint32_t>(c, a & ~3u) >> SHIFT[s];
    if (!PS2_RT) return;
    if (s == 0) c->r[PS2_RT].u64[0] = sx32(mem);
    else c->r[PS2_RT].u32[0] = (c->r[PS2_RT].u32[0] & MASK[s]) | mem;
}
PS2_OP(LDL) {
    const uint32_t a = addrOf(c, raw), s = a & 7;
    const uint64_t mem = load<uint64_t>(c, a & ~7u);
    const uint32_t sh = 56 - s * 8;
    const uint64_t mask = s == 7 ? 0 : (~0ull >> (s * 8 + 8));
    set64(c, PS2_RT, (c->r[PS2_RT].u64[0] & mask) | (mem << sh));
}
PS2_OP(LDR) {
    const uint32_t a = addrOf(c, raw), s = a & 7;
    const uint64_t mem = load<uint64_t>(c, a & ~7u);
    const uint64_t mask = s == 0 ? 0 : (~0ull << (64 - s * 8));
    set64(c, PS2_RT, (c->r[PS2_RT].u64[0] & mask) | (mem >> (s * 8)));
}
PS2_OP(SWL) {
    static constexpr uint32_t MASK[4] = {0xffffff00, 0xffff0000, 0xff000000, 0x00000000};
    static constexpr uint32_t SHIFT[4] = {24, 16, 8, 0};
    const uint32_t a = addrOf(c, raw), s = a & 3;
    const uint32_t mem = load<uint32_t>(c, a & ~3u);
    store<uint32_t>(c, a & ~3u, (mem & MASK[s]) | (c->r[PS2_RT].u32[0] >> SHIFT[s]));
}
PS2_OP(SWR) {
    static constexpr uint32_t MASK[4] = {0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff};
    static constexpr uint32_t SHIFT[4] = {0, 8, 16, 24};
    const uint32_t a = addrOf(c, raw), s = a & 3;
    const uint32_t mem = load<uint32_t>(c, a & ~3u);
    store<uint32_t>(c, a & ~3u, (mem & MASK[s]) | (c->r[PS2_RT].u32[0] << SHIFT[s]));
}
PS2_OP(SDL) {
    const uint32_t a = addrOf(c, raw), s = a & 7;
    const uint64_t mem = load<uint64_t>(c, a & ~7u);
    const uint64_t mask = s == 7 ? 0 : (~0ull << (s * 8 + 8));
    store<uint64_t>(c, a & ~7u, (mem & mask) | (c->r[PS2_RT].u64[0] >> (56 - s * 8)));
}
PS2_OP(SDR) {
    const uint32_t a = addrOf(c, raw), s = a & 7;
    const uint64_t mem = load<uint64_t>(c, a & ~7u);
    const uint64_t mask = s == 0 ? 0 : (~0ull >> (64 - s * 8));
    store<uint64_t>(c, a & ~7u, (mem & mask) | (c->r[PS2_RT].u64[0] << (s * 8)));
}
PS2_OP(LWC1) { c->f[PS2_RT] = load<uint32_t>(c, addrOf(c, raw)); }
PS2_OP(SWC1) { store<uint32_t>(c, addrOf(c, raw), c->f[PS2_RT]); }
PS2_OP(LQC2) {
    Reg128 v;
    load128(c, addrOf(c, raw), &v);
    if (PS2_RT) c->vf[PS2_RT] = v;
}
PS2_OP(SQC2) { store128(c, addrOf(c, raw), &c->vf[PS2_RT]); }

// ---------------------------------------------------------------- MMI helpers
PS2_FORCEINLINE Reg128& RDr(Ctx* c, uint32_t raw) { return c->r[PS2_RD]; }
template <typename T>
PS2_FORCEINLINE T satS(int64_t v) {
    constexpr int64_t lo = (int64_t)std::numeric_limits<T>::min();
    constexpr int64_t hi = (int64_t)std::numeric_limits<T>::max();
    return (T)(v < lo ? lo : (v > hi ? hi : v));
}
template <typename T>
PS2_FORCEINLINE T satU(int64_t v) {
    constexpr int64_t hi = (int64_t)std::numeric_limits<T>::max();
    return (T)(v < 0 ? 0 : (v > hi ? hi : v));
}
#define PS2_MMI_WRITE(...)                                 \
    do {                                                   \
        if (!PS2_RD) return;                               \
        const Reg128 s = c->r[PS2_RS], t = c->r[PS2_RT];   \
        Reg128 d;                                          \
        __VA_ARGS__;                                       \
        c->r[PS2_RD] = d;                                  \
    } while (0)

// ---------------------------------------------------------------- MMI
PS2_OP(PLZCW) {
    if (!PS2_RD) return;
    for (int i = 0; i < 2; ++i) {
        uint32_t v = c->r[PS2_RS].u32[i];
        if (v & 0x80000000u) v = ~v;
        uint32_t n = v == 0 ? 32 : clz32(v);
        c->r[PS2_RD].u32[i] = n - 1;
    }
}
PS2_OP(PMFHL_LW) {
    if (!PS2_RD) return;
    Reg128 d;
    d.u32[0] = c->lo.u32[0]; d.u32[1] = c->hi.u32[0]; d.u32[2] = c->lo.u32[2]; d.u32[3] = c->hi.u32[2];
    c->r[PS2_RD] = d;
}
PS2_OP(PMFHL_UW) {
    if (!PS2_RD) return;
    Reg128 d;
    d.u32[0] = c->lo.u32[1]; d.u32[1] = c->hi.u32[1]; d.u32[2] = c->lo.u32[3]; d.u32[3] = c->hi.u32[3];
    c->r[PS2_RD] = d;
}
PS2_OP(PMFHL_SLW) {
    if (!PS2_RD) return;
    Reg128 d;
    for (int i = 0; i < 2; ++i) {
        int64_t v = (int64_t)(((uint64_t)c->hi.u32[i * 2] << 32) | c->lo.u32[i * 2]);
        d.s64[i] = satS<int32_t>(v);
    }
    c->r[PS2_RD] = d;
}
PS2_OP(PMFHL_LH) {
    if (!PS2_RD) return;
    Reg128 d;
    const Reg128 l = c->lo, h = c->hi;
    d.u16[0] = l.u16[0]; d.u16[1] = l.u16[2]; d.u16[2] = h.u16[0]; d.u16[3] = h.u16[2];
    d.u16[4] = l.u16[4]; d.u16[5] = l.u16[6]; d.u16[6] = h.u16[4]; d.u16[7] = h.u16[6];
    c->r[PS2_RD] = d;
}
PS2_OP(PMFHL_SH) {
    if (!PS2_RD) return;
    Reg128 d;
    const Reg128 l = c->lo, h = c->hi;
    d.s16[0] = satS<int16_t>(l.s32[0]); d.s16[1] = satS<int16_t>(l.s32[1]);
    d.s16[2] = satS<int16_t>(h.s32[0]); d.s16[3] = satS<int16_t>(h.s32[1]);
    d.s16[4] = satS<int16_t>(l.s32[2]); d.s16[5] = satS<int16_t>(l.s32[3]);
    d.s16[6] = satS<int16_t>(h.s32[2]); d.s16[7] = satS<int16_t>(h.s32[3]);
    c->r[PS2_RD] = d;
}
PS2_OP(PMTHL_LW) {
    const Reg128 s = c->r[PS2_RS];
    c->lo.u32[0] = s.u32[0]; c->hi.u32[0] = s.u32[1]; c->lo.u32[2] = s.u32[2]; c->hi.u32[2] = s.u32[3];
}
PS2_OP(PSLLH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.u16[i] = (uint16_t)(t.u16[i] << (PS2_SA & 15))); }
PS2_OP(PSRLH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.u16[i] = (uint16_t)(t.u16[i] >> (PS2_SA & 15))); }
PS2_OP(PSRAH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.s16[i] = (int16_t)(t.s16[i] >> (PS2_SA & 15))); }
PS2_OP(PSLLW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.u32[i] = t.u32[i] << PS2_SA); }
PS2_OP(PSRLW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.u32[i] = t.u32[i] >> PS2_SA); }
PS2_OP(PSRAW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.s32[i] = t.s32[i] >> PS2_SA); }

// MMI0
PS2_OP(PADDW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.u32[i] = s.u32[i] + t.u32[i]); }
PS2_OP(PSUBW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.u32[i] = s.u32[i] - t.u32[i]); }
PS2_OP(PCGTW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.u32[i] = s.s32[i] > t.s32[i] ? ~0u : 0u); }
PS2_OP(PMAXW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.s32[i] = s.s32[i] > t.s32[i] ? s.s32[i] : t.s32[i]); }
PS2_OP(PADDH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.u16[i] = (uint16_t)(s.u16[i] + t.u16[i])); }
PS2_OP(PSUBH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.u16[i] = (uint16_t)(s.u16[i] - t.u16[i])); }
PS2_OP(PCGTH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.u16[i] = s.s16[i] > t.s16[i] ? 0xFFFF : 0); }
PS2_OP(PMAXH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.s16[i] = s.s16[i] > t.s16[i] ? s.s16[i] : t.s16[i]); }
PS2_OP(PADDB) { PS2_MMI_WRITE(for (int i = 0; i < 16; ++i) d.u8[i] = (uint8_t)(s.u8[i] + t.u8[i])); }
PS2_OP(PSUBB) { PS2_MMI_WRITE(for (int i = 0; i < 16; ++i) d.u8[i] = (uint8_t)(s.u8[i] - t.u8[i])); }
PS2_OP(PCGTB) { PS2_MMI_WRITE(for (int i = 0; i < 16; ++i) d.u8[i] = s.s8[i] > t.s8[i] ? 0xFF : 0); }
PS2_OP(PADDSW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.s32[i] = satS<int32_t>((int64_t)s.s32[i] + t.s32[i])); }
PS2_OP(PSUBSW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.s32[i] = satS<int32_t>((int64_t)s.s32[i] - t.s32[i])); }
PS2_OP(PEXTLW) { PS2_MMI_WRITE(d.u32[0] = t.u32[0]; d.u32[1] = s.u32[0]; d.u32[2] = t.u32[1]; d.u32[3] = s.u32[1]); }
PS2_OP(PPACW) { PS2_MMI_WRITE(d.u32[0] = t.u32[0]; d.u32[1] = t.u32[2]; d.u32[2] = s.u32[0]; d.u32[3] = s.u32[2]); }
PS2_OP(PADDSH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.s16[i] = satS<int16_t>((int32_t)s.s16[i] + t.s16[i])); }
PS2_OP(PSUBSH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.s16[i] = satS<int16_t>((int32_t)s.s16[i] - t.s16[i])); }
PS2_OP(PEXTLH) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) { d.u16[2 * i] = t.u16[i]; d.u16[2 * i + 1] = s.u16[i]; }); }
PS2_OP(PPACH) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) { d.u16[i] = t.u16[2 * i]; d.u16[4 + i] = s.u16[2 * i]; }); }
PS2_OP(PADDSB) { PS2_MMI_WRITE(for (int i = 0; i < 16; ++i) d.s8[i] = satS<int8_t>((int32_t)s.s8[i] + t.s8[i])); }
PS2_OP(PSUBSB) { PS2_MMI_WRITE(for (int i = 0; i < 16; ++i) d.s8[i] = satS<int8_t>((int32_t)s.s8[i] - t.s8[i])); }
PS2_OP(PEXTLB) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) { d.u8[2 * i] = t.u8[i]; d.u8[2 * i + 1] = s.u8[i]; }); }
PS2_OP(PPACB) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) { d.u8[i] = t.u8[2 * i]; d.u8[8 + i] = s.u8[2 * i]; }); }
PS2_OP(PEXT5) {
    PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) {
        const uint32_t x = t.u32[i];
        d.u32[i] = ((x & 0x1F) << 3) | ((x & 0x3E0) << 6) | ((x & 0x7C00) << 9) | ((x & 0x8000) << 16);
    });
}
PS2_OP(PPAC5) {
    PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) {
        const uint32_t x = t.u32[i];
        d.u32[i] = ((x >> 3) & 0x1F) | ((x >> 6) & 0x3E0) | ((x >> 9) & 0x7C00) | ((x >> 16) & 0x8000);
    });
}

// MMI1
PS2_OP(PABSW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.s32[i] = t.s32[i] == INT_MIN ? INT_MAX : (t.s32[i] < 0 ? -t.s32[i] : t.s32[i])); }
PS2_OP(PABSH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.s16[i] = t.s16[i] == -32768 ? 32767 : (int16_t)(t.s16[i] < 0 ? -t.s16[i] : t.s16[i])); }
PS2_OP(PCEQW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.u32[i] = s.u32[i] == t.u32[i] ? ~0u : 0u); }
PS2_OP(PCEQH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.u16[i] = s.u16[i] == t.u16[i] ? 0xFFFF : 0); }
PS2_OP(PCEQB) { PS2_MMI_WRITE(for (int i = 0; i < 16; ++i) d.u8[i] = s.u8[i] == t.u8[i] ? 0xFF : 0); }
PS2_OP(PMINW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.s32[i] = s.s32[i] < t.s32[i] ? s.s32[i] : t.s32[i]); }
PS2_OP(PMINH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.s16[i] = s.s16[i] < t.s16[i] ? s.s16[i] : t.s16[i]); }
PS2_OP(PADSBH) {
    PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.u16[i] = (uint16_t)(s.u16[i] - t.u16[i]);
                  for (int i = 4; i < 8; ++i) d.u16[i] = (uint16_t)(s.u16[i] + t.u16[i]));
}
PS2_OP(PADDUW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) { uint64_t v = (uint64_t)s.u32[i] + t.u32[i]; d.u32[i] = v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }); }
PS2_OP(PSUBUW) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) d.u32[i] = s.u32[i] > t.u32[i] ? s.u32[i] - t.u32[i] : 0); }
PS2_OP(PADDUH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.u16[i] = satU<uint16_t>((int32_t)s.u16[i] + t.u16[i])); }
PS2_OP(PSUBUH) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) d.u16[i] = satU<uint16_t>((int32_t)s.u16[i] - t.u16[i])); }
PS2_OP(PADDUB) { PS2_MMI_WRITE(for (int i = 0; i < 16; ++i) d.u8[i] = satU<uint8_t>((int32_t)s.u8[i] + t.u8[i])); }
PS2_OP(PSUBUB) { PS2_MMI_WRITE(for (int i = 0; i < 16; ++i) d.u8[i] = satU<uint8_t>((int32_t)s.u8[i] - t.u8[i])); }
PS2_OP(PEXTUW) { PS2_MMI_WRITE(d.u32[0] = t.u32[2]; d.u32[1] = s.u32[2]; d.u32[2] = t.u32[3]; d.u32[3] = s.u32[3]); }
PS2_OP(PEXTUH) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) { d.u16[2 * i] = t.u16[4 + i]; d.u16[2 * i + 1] = s.u16[4 + i]; }); }
PS2_OP(PEXTUB) { PS2_MMI_WRITE(for (int i = 0; i < 8; ++i) { d.u8[2 * i] = t.u8[8 + i]; d.u8[2 * i + 1] = s.u8[8 + i]; }); }
PS2_OP(QFSRV) {
    PS2_MMI_WRITE({
        uint8_t buf[32];
        std::memcpy(buf, &t, 16);
        std::memcpy(buf + 16, &s, 16);
        std::memcpy(&d, buf + (c->sa & 15), 16);
    });
}

// MMI2
PS2_OP(PMADDW) {
    const Reg128 s = c->r[PS2_RS], t = c->r[PS2_RT];
    Reg128 d = c->r[PS2_RD];
    for (int i = 0; i < 2; ++i) {
        int64_t acc = (int64_t)(((uint64_t)c->hi.u32[i * 2] << 32) | c->lo.u32[i * 2]);
        int64_t v = acc + (int64_t)s.s32[i * 2] * t.s32[i * 2];
        c->lo.u64[i] = sx32((uint32_t)v);
        c->hi.u64[i] = sx32((uint32_t)((uint64_t)v >> 32));
        d.s64[i] = v;
    }
    if (PS2_RD) c->r[PS2_RD] = d;
}
PS2_OP(PMSUBW) {
    const Reg128 s = c->r[PS2_RS], t = c->r[PS2_RT];
    Reg128 d = c->r[PS2_RD];
    for (int i = 0; i < 2; ++i) {
        int64_t acc = (int64_t)(((uint64_t)c->hi.u32[i * 2] << 32) | c->lo.u32[i * 2]);
        int64_t v = acc - (int64_t)s.s32[i * 2] * t.s32[i * 2];
        c->lo.u64[i] = sx32((uint32_t)v);
        c->hi.u64[i] = sx32((uint32_t)((uint64_t)v >> 32));
        d.s64[i] = v;
    }
    if (PS2_RD) c->r[PS2_RD] = d;
}
PS2_OP(PSLLVW) { PS2_MMI_WRITE(d.u64[0] = sx32(t.u32[0] << (s.u32[0] & 31)); d.u64[1] = sx32(t.u32[2] << (s.u32[2] & 31))); }
PS2_OP(PSRLVW) { PS2_MMI_WRITE(d.u64[0] = sx32(t.u32[0] >> (s.u32[0] & 31)); d.u64[1] = sx32(t.u32[2] >> (s.u32[2] & 31))); }
PS2_OP(PSRAVW) { PS2_MMI_WRITE(d.u64[0] = sx32((uint32_t)(t.s32[0] >> (s.u32[0] & 31))); d.u64[1] = sx32((uint32_t)(t.s32[2] >> (s.u32[2] & 31)))); }
PS2_OP(PMFHI) { if (PS2_RD) c->r[PS2_RD] = c->hi; }
PS2_OP(PMFLO) { if (PS2_RD) c->r[PS2_RD] = c->lo; }
PS2_OP(PMTHI) { c->hi = c->r[PS2_RS]; }
PS2_OP(PMTLO) { c->lo = c->r[PS2_RS]; }
PS2_OP(PINTH) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) { d.u16[2 * i] = t.u16[i]; d.u16[2 * i + 1] = s.u16[4 + i]; }); }
PS2_OP(PINTEH) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) { d.u16[2 * i] = t.u16[2 * i]; d.u16[2 * i + 1] = s.u16[2 * i]; }); }
PS2_OP(PMULTW) {
    const Reg128 s = c->r[PS2_RS], t = c->r[PS2_RT];
    Reg128 d;
    for (int i = 0; i < 2; ++i) {
        int64_t v = (int64_t)s.s32[i * 2] * t.s32[i * 2];
        c->lo.u64[i] = sx32((uint32_t)v);
        c->hi.u64[i] = sx32((uint32_t)((uint64_t)v >> 32));
        d.s64[i] = v;
    }
    if (PS2_RD) c->r[PS2_RD] = d;
}
PS2_OP(PMULTUW) {
    const Reg128 s = c->r[PS2_RS], t = c->r[PS2_RT];
    Reg128 d;
    for (int i = 0; i < 2; ++i) {
        uint64_t v = (uint64_t)s.u32[i * 2] * t.u32[i * 2];
        c->lo.u64[i] = sx32((uint32_t)v);
        c->hi.u64[i] = sx32((uint32_t)(v >> 32));
        d.u64[i] = v;
    }
    if (PS2_RD) c->r[PS2_RD] = d;
}
PS2_OP(PMADDUW) {
    const Reg128 s = c->r[PS2_RS], t = c->r[PS2_RT];
    Reg128 d;
    for (int i = 0; i < 2; ++i) {
        uint64_t acc = ((uint64_t)c->hi.u32[i * 2] << 32) | c->lo.u32[i * 2];
        uint64_t v = acc + (uint64_t)s.u32[i * 2] * t.u32[i * 2];
        c->lo.u64[i] = sx32((uint32_t)v);
        c->hi.u64[i] = sx32((uint32_t)(v >> 32));
        d.u64[i] = v;
    }
    if (PS2_RD) c->r[PS2_RD] = d;
}
PS2_OP(PDIVW) {
    const Reg128 s = c->r[PS2_RS], t = c->r[PS2_RT];
    divS(c, 0, s.s32[0], t.s32[0]);
    divS(c, 1, s.s32[2], t.s32[2]);
}
PS2_OP(PDIVUW) {
    const Reg128 s = c->r[PS2_RS], t = c->r[PS2_RT];
    divU(c, 0, s.u32[0], t.u32[0]);
    divU(c, 1, s.u32[2], t.u32[2]);
}
PS2_OP(PDIVBW) {
    const Reg128 s = c->r[PS2_RS];
    const int32_t dv = c->r[PS2_RT].s16[0];
    for (int i = 0; i < 4; ++i) {
        const int32_t n = s.s32[i];
        if (dv == 0) {
            c->lo.s32[i] = n < 0 ? 1 : -1;
            c->hi.s32[i] = n;
        } else if (n == INT_MIN && dv == -1) {
            c->lo.s32[i] = INT_MIN;
            c->hi.s32[i] = 0;
        } else {
            c->lo.s32[i] = n / dv;
            c->hi.s32[i] = n % dv;
        }
    }
}
PS2_OP(PCPYLD) { PS2_MMI_WRITE(d.u64[0] = t.u64[0]; d.u64[1] = s.u64[0]); }
PS2_OP(PCPYUD) { PS2_MMI_WRITE(d.u64[0] = s.u64[1]; d.u64[1] = t.u64[1]); }
PS2_FORCEINLINE int32_t mh(const Reg128& s, const Reg128& t, int i) { return (int32_t)s.s16[i] * t.s16[i]; }
PS2_OP(PMADDH) {
    const Reg128 s = c->r[PS2_RS], t = c->r[PS2_RT];
    c->lo.u32[0] += (uint32_t)mh(s, t, 0); c->lo.u32[1] += (uint32_t)mh(s, t, 1);
    c->hi.u32[0] += (uint32_t)mh(s, t, 2); c->hi.u32[1] += (uint32_t)mh(s, t, 3);
    c->lo.u32[2] += (uint32_t)mh(s, t, 4); c->lo.u32[3] += (uint32_t)mh(s, t, 5);
    c->hi.u32[2] += (uint32_t)mh(s, t, 6); c->hi.u32[3] += (uint32_t)mh(s, t, 7);
    if (PS2_RD) {
        Reg128 d;
        d.u32[0] = c->lo.u32[0]; d.u32[1] = c->hi.u32[0]; d.u32[2] = c->lo.u32[2]; d.u32[3] = c->hi.u32[2];
        c->r[PS2_RD] = d;
    }
}
PS2_OP(PMSUBH) {
    const Reg128 s = c->r[PS2_RS], t = c->r[PS2_RT];
    c->lo.u32[0] -= (uint32_t)mh(s, t, 0); c->lo.u32[1] -= (uint32_t)mh(s, t, 1);
    c->hi.u32[0] -= (uint32_t)mh(s, t, 2); c->hi.u32[1] -= (uint32_t)mh(s, t, 3);
    c->lo.u32[2] -= (uint32_t)mh(s, t, 4); c->lo.u32[3] -= (uint32_t)mh(s, t, 5);
    c->hi.u32[2] -= (uint32_t)mh(s, t, 6); c->hi.u32[3] -= (uint32_t)mh(s, t, 7);
    if (PS2_RD) {
        Reg128 d;
        d.u32[0] = c->lo.u32[0]; d.u32[1] = c->hi.u32[0]; d.u32[2] = c->lo.u32[2]; d.u32[3] = c->hi.u32[2];
        c->r[PS2_RD] = d;
    }
}
PS2_OP(PMULTH) {
    const Reg128 s = c->r[PS2_RS], t = c->r[PS2_RT];
    c->lo.s32[0] = mh(s, t, 0); c->lo.s32[1] = mh(s, t, 1);
    c->hi.s32[0] = mh(s, t, 2); c->hi.s32[1] = mh(s, t, 3);
    c->lo.s32[2] = mh(s, t, 4); c->lo.s32[3] = mh(s, t, 5);
    c->hi.s32[2] = mh(s, t, 6); c->hi.s32[3] = mh(s, t, 7);
    if (PS2_RD) {
        Reg128 d;
        d.u32[0] = c->lo.u32[0]; d.u32[1] = c->hi.u32[0]; d.u32[2] = c->lo.u32[2]; d.u32[3] = c->hi.u32[2];
        c->r[PS2_RD] = d;
    }
}
PS2_OP(PHMADH) {
    const Reg128 s = c->r[PS2_RS], t = c->r[PS2_RT];
    c->lo.s32[0] = mh(s, t, 1) + mh(s, t, 0);
    c->lo.s32[1] = mh(s, t, 1);
    c->hi.s32[0] = mh(s, t, 3) + mh(s, t, 2);
    c->hi.s32[1] = mh(s, t, 3);
    c->lo.s32[2] = mh(s, t, 5) + mh(s, t, 4);
    c->lo.s32[3] = mh(s, t, 5);
    c->hi.s32[2] = mh(s, t, 7) + mh(s, t, 6);
    c->hi.s32[3] = mh(s, t, 7);
    if (PS2_RD) {
        Reg128 d;
        d.u32[0] = c->lo.u32[0]; d.u32[1] = c->hi.u32[0]; d.u32[2] = c->lo.u32[2]; d.u32[3] = c->hi.u32[2];
        c->r[PS2_RD] = d;
    }
}
PS2_OP(PHMSBH) {
    const Reg128 s = c->r[PS2_RS], t = c->r[PS2_RT];
    c->lo.s32[0] = mh(s, t, 1) - mh(s, t, 0);
    c->lo.s32[1] = ~mh(s, t, 1);
    c->hi.s32[0] = mh(s, t, 3) - mh(s, t, 2);
    c->hi.s32[1] = ~mh(s, t, 3);
    c->lo.s32[2] = mh(s, t, 5) - mh(s, t, 4);
    c->lo.s32[3] = ~mh(s, t, 5);
    c->hi.s32[2] = mh(s, t, 7) - mh(s, t, 6);
    c->hi.s32[3] = ~mh(s, t, 7);
    if (PS2_RD) {
        Reg128 d;
        d.u32[0] = c->lo.u32[0]; d.u32[1] = c->hi.u32[0]; d.u32[2] = c->lo.u32[2]; d.u32[3] = c->hi.u32[2];
        c->r[PS2_RD] = d;
    }
}
PS2_OP(PAND) { PS2_MMI_WRITE(d.u64[0] = s.u64[0] & t.u64[0]; d.u64[1] = s.u64[1] & t.u64[1]); }
PS2_OP(PXOR) { PS2_MMI_WRITE(d.u64[0] = s.u64[0] ^ t.u64[0]; d.u64[1] = s.u64[1] ^ t.u64[1]); }
PS2_OP(POR) { PS2_MMI_WRITE(d.u64[0] = s.u64[0] | t.u64[0]; d.u64[1] = s.u64[1] | t.u64[1]); }
PS2_OP(PNOR) { PS2_MMI_WRITE(d.u64[0] = ~(s.u64[0] | t.u64[0]); d.u64[1] = ~(s.u64[1] | t.u64[1])); }
PS2_OP(PEXEH) { PS2_MMI_WRITE(static constexpr int o[8] = {2, 1, 0, 3, 6, 5, 4, 7}; for (int i = 0; i < 8; ++i) d.u16[i] = t.u16[o[i]]); }
PS2_OP(PREVH) { PS2_MMI_WRITE(static constexpr int o[8] = {3, 2, 1, 0, 7, 6, 5, 4}; for (int i = 0; i < 8; ++i) d.u16[i] = t.u16[o[i]]); }
PS2_OP(PEXCH) { PS2_MMI_WRITE(static constexpr int o[8] = {0, 2, 1, 3, 4, 6, 5, 7}; for (int i = 0; i < 8; ++i) d.u16[i] = t.u16[o[i]]); }
PS2_OP(PEXEW) { PS2_MMI_WRITE(d.u32[0] = t.u32[2]; d.u32[1] = t.u32[1]; d.u32[2] = t.u32[0]; d.u32[3] = t.u32[3]); }
PS2_OP(PROT3W) { PS2_MMI_WRITE(d.u32[0] = t.u32[1]; d.u32[1] = t.u32[2]; d.u32[2] = t.u32[0]; d.u32[3] = t.u32[3]); }
PS2_OP(PEXCW) { PS2_MMI_WRITE(d.u32[0] = t.u32[0]; d.u32[1] = t.u32[2]; d.u32[2] = t.u32[1]; d.u32[3] = t.u32[3]); }
PS2_OP(PCPYH) { PS2_MMI_WRITE(for (int i = 0; i < 4; ++i) { d.u16[i] = t.u16[0]; d.u16[4 + i] = t.u16[4]; }); }

// ---------------------------------------------------------------- COP0
PS2_OP(MFC0) { set64(c, PS2_RT, sx32(rt_mfc0(c, raw))); }
PS2_OP(MTC0) { rt_mtc0(c, raw, c->r[PS2_RT].u32[0]); }
PS2_OP(EI) { rt_ei(c); }
PS2_OP(DI) { rt_di(c); }
PS2_OP(TLBR) {}
PS2_OP(TLBWI) {}
PS2_OP(TLBWR) {}
PS2_OP(TLBP) {}

// ---------------------------------------------------------------- COP1 (FPU)
#define PS2_FS ((raw >> 11) & 31)
#define PS2_FT ((raw >> 16) & 31)
#define PS2_FD ((raw >> 6) & 31)
PS2_OP(MFC1) { set64(c, PS2_RT, sx32(c->f[PS2_FS])); }
PS2_OP(MTC1) { c->f[PS2_FS] = c->r[PS2_RT].u32[0]; }
PS2_OP(CFC1) { set64(c, PS2_RT, sx32(PS2_FS < 16 ? fpu::FCR0_VALUE : c->fcr31)); }
PS2_OP(CTC1) {
    if (PS2_FS >= 16) c->fcr31 = (c->r[PS2_RT].u32[0] & fpu::FCR31_WRITE_MASK) | 0x01000001u;
}
PS2_OP(ADD_S) { auto r = fpu::add(c->f[PS2_FS], c->f[PS2_FT]); fpu::setOU(c, r); c->f[PS2_FD] = r.value; }
PS2_OP(SUB_S) { auto r = fpu::sub(c->f[PS2_FS], c->f[PS2_FT]); fpu::setOU(c, r); c->f[PS2_FD] = r.value; }
PS2_OP(MUL_S) { auto r = fpu::mul(c->f[PS2_FS], c->f[PS2_FT]); fpu::setOU(c, r); c->f[PS2_FD] = r.value; }
PS2_OP(DIV_S) {
    auto r = fpu::div(c->f[PS2_FS], c->f[PS2_FT]);
    c->fcr31 &= ~(fpu::FLAG_D | fpu::FLAG_I);
    if (r.divByZero) c->fcr31 |= fpu::FLAG_D | fpu::FLAG_SD;
    if (r.invalid) c->fcr31 |= fpu::FLAG_I | fpu::FLAG_SI;
    c->f[PS2_FD] = r.value;
}
PS2_OP(SQRT_S) {
    const uint32_t t = c->f[PS2_FT];
    c->fcr31 &= ~(fpu::FLAG_D | fpu::FLAG_I);
    uint32_t r = fpu::sqrtMag(t);
    if ((t & fpu::SIGN) && !fpu::isZero(t)) c->fcr31 |= fpu::FLAG_I | fpu::FLAG_SI;
    c->f[PS2_FD] = r;
}
PS2_OP(RSQRT_S) {
    const uint32_t s = c->f[PS2_FS], t = c->f[PS2_FT];
    c->fcr31 &= ~(fpu::FLAG_D | fpu::FLAG_I);
    if (fpu::isZero(t)) {
        c->fcr31 |= fpu::FLAG_D | fpu::FLAG_SD;
        c->f[PS2_FD] = (s & fpu::SIGN) | fpu::MAXMAG;
        return;
    }
    if (t & fpu::SIGN) c->fcr31 |= fpu::FLAG_I | fpu::FLAG_SI;
    const uint32_t root = fpu::sqrtMag(t);
    auto r = fpu::div(s, root, true);
    c->f[PS2_FD] = r.value;
}
PS2_OP(ABS_S) { c->f[PS2_FD] = c->f[PS2_FS] & fpu::MAXMAG; c->fcr31 &= ~(fpu::FLAG_O | fpu::FLAG_U); }
PS2_OP(NEG_S) { c->f[PS2_FD] = c->f[PS2_FS] ^ fpu::SIGN; c->fcr31 &= ~(fpu::FLAG_O | fpu::FLAG_U); }
PS2_OP(MOV_S) { c->f[PS2_FD] = c->f[PS2_FS]; }
PS2_OP(ADDA_S) { auto r = fpu::add(c->f[PS2_FS], c->f[PS2_FT]); fpu::setOU(c, r); c->facc = r.value; }
PS2_OP(SUBA_S) { auto r = fpu::sub(c->f[PS2_FS], c->f[PS2_FT]); fpu::setOU(c, r); c->facc = r.value; }
PS2_OP(MULA_S) { auto r = fpu::mul(c->f[PS2_FS], c->f[PS2_FT]); fpu::setOU(c, r); c->facc = r.value; }
PS2_FORCEINLINE fpu::Result maddCore(Ctx* c, uint32_t fs, uint32_t ft, bool subtract) {
    auto p = fpu::mul(fs, ft);
    if (p.overflow) return {(subtract ? (p.value ^ fpu::SIGN) : p.value), true, false};
    return subtract ? fpu::sub(c->facc, p.value) : fpu::add(c->facc, p.value);
}
PS2_OP(MADD_S) { auto r = maddCore(c, c->f[PS2_FS], c->f[PS2_FT], false); fpu::setOU(c, r); c->f[PS2_FD] = r.value; }
PS2_OP(MSUB_S) { auto r = maddCore(c, c->f[PS2_FS], c->f[PS2_FT], true); fpu::setOU(c, r); c->f[PS2_FD] = r.value; }
PS2_OP(MADDA_S) { auto r = maddCore(c, c->f[PS2_FS], c->f[PS2_FT], false); fpu::setOU(c, r); c->facc = r.value; }
PS2_OP(MSUBA_S) { auto r = maddCore(c, c->f[PS2_FS], c->f[PS2_FT], true); fpu::setOU(c, r); c->facc = r.value; }
PS2_OP(CVT_W_S) { c->f[PS2_FD] = fpu::toInt(c->f[PS2_FS]); }
PS2_OP(CVT_S_W) { c->f[PS2_FD] = fpu::fromInt(c->f[PS2_FS]); }
PS2_OP(MAX_S) {
    const uint32_t s = c->f[PS2_FS], t = c->f[PS2_FT];
    c->f[PS2_FD] = fpu::orderKey(s) >= fpu::orderKey(t) ? s : t;
    c->fcr31 &= ~(fpu::FLAG_O | fpu::FLAG_U);
}
PS2_OP(MIN_S) {
    const uint32_t s = c->f[PS2_FS], t = c->f[PS2_FT];
    c->f[PS2_FD] = fpu::orderKey(s) <= fpu::orderKey(t) ? s : t;
    c->fcr31 &= ~(fpu::FLAG_O | fpu::FLAG_U);
}
PS2_FORCEINLINE void setC(Ctx* c, bool v) {
    if (v) c->fcr31 |= fpu::FLAG_C;
    else c->fcr31 &= ~fpu::FLAG_C;
}
PS2_OP(C_F_S) { setC(c, false); }
PS2_OP(C_EQ_S) { setC(c, fpu::cmpKey(c->f[PS2_FS]) == fpu::cmpKey(c->f[PS2_FT])); }
PS2_OP(C_LT_S) { setC(c, fpu::cmpKey(c->f[PS2_FS]) < fpu::cmpKey(c->f[PS2_FT])); }
PS2_OP(C_LE_S) { setC(c, fpu::cmpKey(c->f[PS2_FS]) <= fpu::cmpKey(c->f[PS2_FT])); }

// ---------------------------------------------------------------- COP2 (VU0 macro) -> runtime
#define PS2_COP2(name) PS2_OP(name) { rt_cop2(c, raw); }
PS2_COP2(QMFC2) PS2_COP2(CFC2) PS2_COP2(QMTC2) PS2_COP2(CTC2)
PS2_COP2(VADDbc) PS2_COP2(VSUBbc) PS2_COP2(VMADDbc) PS2_COP2(VMSUBbc) PS2_COP2(VMAXbc) PS2_COP2(VMINIbc)
PS2_COP2(VMULbc) PS2_COP2(VMULq) PS2_COP2(VMAXi) PS2_COP2(VMULi) PS2_COP2(VMINIi) PS2_COP2(VADDq)
PS2_COP2(VMADDq) PS2_COP2(VADDi) PS2_COP2(VMADDi) PS2_COP2(VSUBq) PS2_COP2(VMSUBq) PS2_COP2(VSUBi)
PS2_COP2(VMSUBi) PS2_COP2(VADD) PS2_COP2(VMADD) PS2_COP2(VMUL) PS2_COP2(VMAX) PS2_COP2(VSUB)
PS2_COP2(VMSUB) PS2_COP2(VOPMSUB) PS2_COP2(VMINI) PS2_COP2(VIADD) PS2_COP2(VISUB) PS2_COP2(VIADDI)
PS2_COP2(VIAND) PS2_COP2(VIOR) PS2_COP2(VCALLMS) PS2_COP2(VCALLMSR) PS2_COP2(VADDAbc) PS2_COP2(VSUBAbc)
PS2_COP2(VMADDAbc) PS2_COP2(VMSUBAbc) PS2_COP2(VITOF0) PS2_COP2(VITOF4) PS2_COP2(VITOF12) PS2_COP2(VITOF15)
PS2_COP2(VFTOI0) PS2_COP2(VFTOI4) PS2_COP2(VFTOI12) PS2_COP2(VFTOI15) PS2_COP2(VMULAbc) PS2_COP2(VMULAq)
PS2_COP2(VABS) PS2_COP2(VMULAi) PS2_COP2(VCLIPw) PS2_COP2(VADDAq) PS2_COP2(VMADDAq) PS2_COP2(VADDAi)
PS2_COP2(VMADDAi) PS2_COP2(VSUBAq) PS2_COP2(VMSUBAq) PS2_COP2(VSUBAi) PS2_COP2(VMSUBAi) PS2_COP2(VADDA)
PS2_COP2(VMADDA) PS2_COP2(VMULA) PS2_COP2(VSUBA) PS2_COP2(VMSUBA) PS2_COP2(VOPMULA) PS2_COP2(VNOP)
PS2_COP2(VMOVE) PS2_COP2(VMR32) PS2_COP2(VLQI) PS2_COP2(VSQI) PS2_COP2(VLQD) PS2_COP2(VSQD)
PS2_COP2(VDIV) PS2_COP2(VSQRT) PS2_COP2(VRSQRT) PS2_COP2(VWAITQ) PS2_COP2(VMTIR) PS2_COP2(VMFIR)
PS2_COP2(VILWR) PS2_COP2(VISWR) PS2_COP2(VRNEXT) PS2_COP2(VRGET) PS2_COP2(VRINIT) PS2_COP2(VRXOR)
#undef PS2_COP2

// ---------------------------------------------------------------- branch conditions
PS2_FORCEINLINE bool BC_BEQ(Ctx* c, uint32_t raw) { return c->r[PS2_RS].u64[0] == c->r[PS2_RT].u64[0]; }
PS2_FORCEINLINE bool BC_BNE(Ctx* c, uint32_t raw) { return c->r[PS2_RS].u64[0] != c->r[PS2_RT].u64[0]; }
PS2_FORCEINLINE bool BC_BLEZ(Ctx* c, uint32_t raw) { return c->r[PS2_RS].s64[0] <= 0; }
PS2_FORCEINLINE bool BC_BGTZ(Ctx* c, uint32_t raw) { return c->r[PS2_RS].s64[0] > 0; }
PS2_FORCEINLINE bool BC_BLTZ(Ctx* c, uint32_t raw) { return c->r[PS2_RS].s64[0] < 0; }
PS2_FORCEINLINE bool BC_BGEZ(Ctx* c, uint32_t raw) { return c->r[PS2_RS].s64[0] >= 0; }
PS2_FORCEINLINE bool BC_BC1F(Ctx* c, uint32_t) { return !(c->fcr31 & fpu::FLAG_C); }
PS2_FORCEINLINE bool BC_BC1T(Ctx* c, uint32_t) { return (c->fcr31 & fpu::FLAG_C) != 0; }
PS2_FORCEINLINE bool BC_BC0F(Ctx* c, uint32_t) { return !rt_cpcond0(c); }
PS2_FORCEINLINE bool BC_BC0T(Ctx* c, uint32_t) { return rt_cpcond0(c); }
PS2_FORCEINLINE bool BC_BC2F(Ctx* c, uint32_t) { return !rt_cop2cond(c); }
PS2_FORCEINLINE bool BC_BC2T(Ctx* c, uint32_t) { return rt_cop2cond(c); }

} // namespace ps2::ops

// Events: called at loop back-edges and dynamic jumps.
#define PS2_CYCLES(n) (c->cycles += (n))
#define PS2_CHECK_EVENTS()                                         \
    do {                                                           \
        if (PS2_UNLIKELY(c->cycles >= *c->nextEvent)) ps2::rt_events(c); \
    } while (0)
