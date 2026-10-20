#include "ps2/vu.hpp"

#include <cmath>
#include <cstring>

#include "ps2/fpu.hpp"
#include "ps2/gs.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"
#include "ps2/vif.hpp"

namespace ps2 {

namespace {

constexpr uint32_t ST_Z = 1, ST_S = 2, ST_U = 4, ST_O = 8, ST_I = 16, ST_D = 32;

inline bool destBit(uint32_t ins, int f) { return (ins >> (24 - f)) & 1; }
inline uint32_t FD(uint32_t ins) { return (ins >> 6) & 31; }
inline uint32_t FS(uint32_t ins) { return (ins >> 11) & 31; }
inline uint32_t FT(uint32_t ins) { return (ins >> 16) & 31; }

// MAC bit layout: Z = bits 0-3 (w..x), S = 4-7, U = 8-11, O = 12-15
inline uint32_t macZ(int f) { return 1u << (3 - f); }
inline uint32_t macS(int f) { return 1u << (7 - f); }
inline uint32_t macU(int f) { return 1u << (11 - f); }
inline uint32_t macO(int f) { return 1u << (15 - f); }

uint32_t flagsFor(const fpu::Result& r, int f) {
    uint32_t m = 0;
    if (fpu::isZero(r.value)) m |= macZ(f);
    if (r.value & fpu::SIGN) m |= macS(f);
    if (r.underflow) m |= macU(f) | macZ(f);
    if (r.overflow) m |= macO(f);
    return m;
}

void commitStatus(VuState& v, uint32_t mac) {
    v.mac = mac;
    uint32_t st = v.status & ~0xFu;
    if (mac & 0x000F) st |= ST_Z;
    if (mac & 0x00F0) st |= ST_S;
    if (mac & 0x0F00) st |= ST_U;
    if (mac & 0xF000) st |= ST_O;
    st |= (st & 0xF) << 6; // sticky
    v.status = st;
}

inline uint32_t clampIn(uint32_t x) { return fpu::isZero(x) ? (x & fpu::SIGN) : x; }

enum class Arith { Add, Sub, Mul, MAdd, MSub };

// Generic vector arithmetic: dst[f] = op(a[f], b[f] or broadcast)
void arith(VuState& v, uint32_t ins, Arith op, Reg128* dst, const Reg128& a, const Reg128& b, int bc, uint32_t scalar,
           bool useScalar) {
    uint32_t mac = 0;
    Reg128 out = *dst;
    for (int f = 0; f < 4; ++f) {
        if (!destBit(ins, f)) continue;
        const uint32_t x = clampIn(a.u32[f]);
        const uint32_t y = clampIn(useScalar ? scalar : (bc >= 0 ? b.u32[bc] : b.u32[f]));
        fpu::Result r{};
        switch (op) {
        case Arith::Add: r = fpu::add(x, y); break;
        case Arith::Sub: r = fpu::sub(x, y); break;
        case Arith::Mul: r = fpu::mul(x, y); break;
        case Arith::MAdd: {
            auto p = fpu::mul(x, y);
            r = fpu::add(clampIn(v.acc.u32[f]), p.value);
            r.overflow |= p.overflow;
            break;
        }
        case Arith::MSub: {
            auto p = fpu::mul(x, y);
            r = fpu::sub(clampIn(v.acc.u32[f]), p.value);
            r.overflow |= p.overflow;
            break;
        }
        }
        out.u32[f] = r.value;
        mac |= flagsFor(r, f);
    }
    *dst = out;
    commitStatus(v, mac);
}

void minmax(uint32_t ins, Reg128* dst, const Reg128& a, const Reg128& b, int bc, uint32_t scalar, bool useScalar,
            bool isMax) {
    Reg128 out = *dst;
    for (int f = 0; f < 4; ++f) {
        if (!destBit(ins, f)) continue;
        const uint32_t x = a.u32[f];
        const uint32_t y = useScalar ? scalar : (bc >= 0 ? b.u32[bc] : b.u32[f]);
        const int64_t kx = fpu::cmpKey(x), ky = fpu::cmpKey(y);
        out.u32[f] = isMax ? (kx >= ky ? x : y) : (kx <= ky ? x : y);
    }
    *dst = out;
}

uint32_t ftoi(uint32_t x, int shift) {
    if (fpu::isZero(x)) return 0;
    uint32_t e = fpu::expOf(x) + (uint32_t)shift;
    if (e > 255) return (x & fpu::SIGN) ? 0x80000000u : 0x7FFFFFFFu;
    return fpu::toInt((x & ~0x7F800000u) | (e << 23));
}

uint32_t itof(uint32_t i, int shift) {
    uint32_t f = fpu::fromInt(i);
    if (fpu::isZero(f)) return 0;
    int32_t e = (int32_t)fpu::expOf(f) - shift;
    if (e <= 0) return f & fpu::SIGN;
    return (f & ~0x7F800000u) | ((uint32_t)e << 23);
}

void writeVf(VuState& v, uint32_t reg, const Reg128& val) {
    if (reg != 0) v.vf[reg] = val;
}

uint32_t fieldOf(const Reg128& r, uint32_t f) { return r.u32[f & 3]; }

void vuDiv(VuState& v, uint32_t s, uint32_t t) {
    v.status &= ~(ST_I | ST_D);
    if (fpu::isZero(t)) {
        if (fpu::isZero(s)) v.status |= ST_I | (ST_I << 6);
        else v.status |= ST_D | (ST_D << 6);
        v.q = ((s ^ t) & fpu::SIGN) | fpu::MAXMAG;
        return;
    }
    v.q = fpu::div(s, t).value;
}

void vuSqrt(VuState& v, uint32_t t) {
    v.status &= ~(ST_I | ST_D);
    if ((t & fpu::SIGN) && !fpu::isZero(t)) v.status |= ST_I | (ST_I << 6);
    v.q = fpu::sqrtMag(t);
}

void vuRsqrt(VuState& v, uint32_t s, uint32_t t) {
    v.status &= ~(ST_I | ST_D);
    if (fpu::isZero(t)) {
        v.status |= ST_D | (ST_D << 6);
        v.q = (s & fpu::SIGN) | fpu::MAXMAG;
        return;
    }
    if (t & fpu::SIGN) v.status |= ST_I | (ST_I << 6);
    v.q = fpu::div(s, fpu::sqrtMag(t)).value;
}

void rnext(VuState& v) {
    // 23-bit LFSR as documented for the VU random unit.
    uint32_t x = v.r & 0x7FFFFF;
    const uint32_t bit = ((x >> 4) ^ (x >> 22)) & 1;
    x = ((x << 1) | bit) & 0x7FFFFF;
    v.r = 0x3F800000 | x;
}

} // namespace

namespace vuops {

uint32_t add(uint32_t a, uint32_t b, uint32_t& macBits, int field) {
    auto r = fpu::add(clampIn(a), clampIn(b));
    macBits |= flagsFor(r, field);
    return r.value;
}
uint32_t sub(uint32_t a, uint32_t b, uint32_t& macBits, int field) {
    auto r = fpu::sub(clampIn(a), clampIn(b));
    macBits |= flagsFor(r, field);
    return r.value;
}
uint32_t mul(uint32_t a, uint32_t b, uint32_t& macBits, int field) {
    auto r = fpu::mul(clampIn(a), clampIn(b));
    macBits |= flagsFor(r, field);
    return r.value;
}

void upper(VuState& v, uint32_t ins) {
    const uint32_t funct = ins & 0x3F;
    const Reg128 fs = v.vf[FS(ins)];
    const Reg128 ft = v.vf[FT(ins)];
    Reg128* fd = FD(ins) ? &v.vf[FD(ins)] : nullptr;
    Reg128 scratch;
    if (!fd) {
        scratch = v.vf[0];
        fd = &scratch; // writes to vf0 are discarded (flags still update)
    }
    const int bc = (int)(ins & 3);
    if (funct < 60) {
        switch (funct) {
        case 0: case 1: case 2: case 3: arith(v, ins, Arith::Add, fd, fs, ft, bc, 0, false); break;
        case 4: case 5: case 6: case 7: arith(v, ins, Arith::Sub, fd, fs, ft, bc, 0, false); break;
        case 8: case 9: case 10: case 11: arith(v, ins, Arith::MAdd, fd, fs, ft, bc, 0, false); break;
        case 12: case 13: case 14: case 15: arith(v, ins, Arith::MSub, fd, fs, ft, bc, 0, false); break;
        case 16: case 17: case 18: case 19: minmax(ins, fd, fs, ft, bc, 0, false, true); break;
        case 20: case 21: case 22: case 23: minmax(ins, fd, fs, ft, bc, 0, false, false); break;
        case 24: case 25: case 26: case 27: arith(v, ins, Arith::Mul, fd, fs, ft, bc, 0, false); break;
        case 28: arith(v, ins, Arith::Mul, fd, fs, ft, -1, v.q, true); break;  // MULq
        case 29: minmax(ins, fd, fs, ft, -1, v.i, true, true); break;          // MAXi
        case 30: arith(v, ins, Arith::Mul, fd, fs, ft, -1, v.i, true); break;  // MULi
        case 31: minmax(ins, fd, fs, ft, -1, v.i, true, false); break;         // MINIi
        case 32: arith(v, ins, Arith::Add, fd, fs, ft, -1, v.q, true); break;  // ADDq
        case 33: arith(v, ins, Arith::MAdd, fd, fs, ft, -1, v.q, true); break; // MADDq
        case 34: arith(v, ins, Arith::Add, fd, fs, ft, -1, v.i, true); break;  // ADDi
        case 35: arith(v, ins, Arith::MAdd, fd, fs, ft, -1, v.i, true); break; // MADDi
        case 36: arith(v, ins, Arith::Sub, fd, fs, ft, -1, v.q, true); break;  // SUBq
        case 37: arith(v, ins, Arith::MSub, fd, fs, ft, -1, v.q, true); break; // MSUBq
        case 38: arith(v, ins, Arith::Sub, fd, fs, ft, -1, v.i, true); break;  // SUBi
        case 39: arith(v, ins, Arith::MSub, fd, fs, ft, -1, v.i, true); break; // MSUBi
        case 40: arith(v, ins, Arith::Add, fd, fs, ft, -1, 0, false); break;
        case 41: arith(v, ins, Arith::MAdd, fd, fs, ft, -1, 0, false); break;
        case 42: arith(v, ins, Arith::Mul, fd, fs, ft, -1, 0, false); break;
        case 43: minmax(ins, fd, fs, ft, -1, 0, false, true); break;
        case 44: arith(v, ins, Arith::Sub, fd, fs, ft, -1, 0, false); break;
        case 45: arith(v, ins, Arith::MSub, fd, fs, ft, -1, 0, false); break;
        case 46: { // OPMSUB: fd = ACC - fs x ft (cross terms)
            Reg128 a, b;
            a = fs;
            b = ft;
            Reg128 out = *fd;
            uint32_t mac = 0;
            const int sy[3] = {1, 2, 0}, tz[3] = {2, 0, 1};
            for (int f = 0; f < 3; ++f) {
                if (!destBit(ins, f)) continue;
                auto p = fpu::mul(clampIn(a.u32[sy[f]]), clampIn(b.u32[tz[f]]));
                auto r = fpu::sub(clampIn(v.acc.u32[f]), p.value);
                out.u32[f] = r.value;
                mac |= flagsFor(r, f);
            }
            *fd = out;
            commitStatus(v, mac);
            break;
        }
        case 47: minmax(ins, fd, fs, ft, -1, 0, false, false); break;
        default:
            break; // lower-only (macro) or invalid
        }
        return;
    }
    // special2: ACC forms and single-operand ops
    const uint32_t idx = (((ins >> 6) & 0x1F) << 2) | (ins & 3);
    Reg128* acc = &v.acc;
    Reg128 tmp;
    auto destFt = [&](const Reg128& val) {
        Reg128 o = v.vf[FT(ins)];
        for (int f = 0; f < 4; ++f)
            if (destBit(ins, f)) o.u32[f] = val.u32[f];
        writeVf(v, FT(ins), o);
    };
    switch (idx) {
    case 0: case 1: case 2: case 3: arith(v, ins, Arith::Add, acc, fs, ft, bc, 0, false); break;
    case 4: case 5: case 6: case 7: arith(v, ins, Arith::Sub, acc, fs, ft, bc, 0, false); break;
    case 8: case 9: case 10: case 11: arith(v, ins, Arith::MAdd, acc, fs, ft, bc, 0, false); break;
    case 12: case 13: case 14: case 15: arith(v, ins, Arith::MSub, acc, fs, ft, bc, 0, false); break;
    case 16: case 17: case 18: case 19: { // ITOF0/4/12/15
        static const int sh[4] = {0, 4, 12, 15};
        for (int f = 0; f < 4; ++f) tmp.u32[f] = itof(fs.u32[f], sh[idx - 16]);
        destFt(tmp);
        break;
    }
    case 20: case 21: case 22: case 23: { // FTOI0/4/12/15
        static const int sh[4] = {0, 4, 12, 15};
        for (int f = 0; f < 4; ++f) tmp.u32[f] = ftoi(fs.u32[f], sh[idx - 20]);
        destFt(tmp);
        break;
    }
    case 24: case 25: case 26: case 27: arith(v, ins, Arith::Mul, acc, fs, ft, bc, 0, false); break;
    case 28: arith(v, ins, Arith::Mul, acc, fs, ft, -1, v.q, true); break; // MULAq
    case 29: // ABS ft, fs
        for (int f = 0; f < 4; ++f) tmp.u32[f] = fs.u32[f] & 0x7FFFFFFF;
        destFt(tmp);
        break;
    case 30: arith(v, ins, Arith::Mul, acc, fs, ft, -1, v.i, true); break; // MULAi
    case 31: { // CLIPw fs.xyz, ft.w
        const int64_t w = fpu::cmpKey(ft.u32[3] & 0x7FFFFFFF);
        uint32_t cf = 0;
        for (int f = 0; f < 3; ++f) {
            const int64_t x = fpu::cmpKey(fs.u32[f]);
            if (x > w) cf |= 1u << (f * 2);
            if (x < -w) cf |= 2u << (f * 2);
        }
        v.clip = ((v.clip << 6) | cf) & 0xFFFFFF;
        break;
    }
    case 32: arith(v, ins, Arith::Add, acc, fs, ft, -1, v.q, true); break;
    case 33: arith(v, ins, Arith::MAdd, acc, fs, ft, -1, v.q, true); break;
    case 34: arith(v, ins, Arith::Add, acc, fs, ft, -1, v.i, true); break;
    case 35: arith(v, ins, Arith::MAdd, acc, fs, ft, -1, v.i, true); break;
    case 36: arith(v, ins, Arith::Sub, acc, fs, ft, -1, v.q, true); break;
    case 37: arith(v, ins, Arith::MSub, acc, fs, ft, -1, v.q, true); break;
    case 38: arith(v, ins, Arith::Sub, acc, fs, ft, -1, v.i, true); break;
    case 39: arith(v, ins, Arith::MSub, acc, fs, ft, -1, v.i, true); break;
    case 40: arith(v, ins, Arith::Add, acc, fs, ft, -1, 0, false); break;
    case 41: arith(v, ins, Arith::MAdd, acc, fs, ft, -1, 0, false); break;
    case 42: arith(v, ins, Arith::Mul, acc, fs, ft, -1, 0, false); break;
    case 44: arith(v, ins, Arith::Sub, acc, fs, ft, -1, 0, false); break;
    case 45: arith(v, ins, Arith::MSub, acc, fs, ft, -1, 0, false); break;
    case 46: { // OPMULA: ACC = fs x ft cross terms
        uint32_t mac = 0;
        const int sy[3] = {1, 2, 0}, tz[3] = {2, 0, 1};
        Reg128 out = v.acc;
        for (int f = 0; f < 3; ++f) {
            if (!destBit(ins, f)) continue;
            auto r = fpu::mul(clampIn(fs.u32[sy[f]]), clampIn(ft.u32[tz[f]]));
            out.u32[f] = r.value;
            mac |= flagsFor(r, f);
        }
        v.acc = out;
        commitStatus(v, mac);
        break;
    }
    case 47: break; // NOP
    default: break; // lower ops handled elsewhere
    }
}

} // namespace vuops

// ------------------------------------------------------------------ Vu

Vu::Vu(Machine& m) : m_(m) {
    for (int i = 0; i < 2; ++i) {
        std::memset(v_[i].vf, 0, sizeof v_[i].vf);
        std::memset(v_[i].vi, 0, sizeof v_[i].vi);
        std::memset(&v_[i].acc, 0, sizeof v_[i].acc);
        v_[i].vf[0].u32[3] = 0x3F800000;
        v_[i].index = i;
    }
    v_[0].mem = m.vu0Data;
    v_[0].code = m.vu0Code;
    v_[0].memMask = 0xFFF;
    v_[0].codeMask = 0xFFF;
    v_[1].mem = m.vu1Data;
    v_[1].code = m.vu1Code;
    v_[1].memMask = 0x3FFF;
    v_[1].codeMask = 0x3FFF;
}

static void loadQ(VuState& v, uint32_t addr16, uint32_t ins, uint32_t reg) {
    const uint32_t a = (addr16 * 16) & v.memMask;
    Reg128 val = v.vf[reg];
    for (int f = 0; f < 4; ++f)
        if (destBit(ins, f)) std::memcpy(&val.u32[f], v.mem + a + f * 4, 4);
    writeVf(v, reg, val);
}
static void storeQ(VuState& v, uint32_t addr16, uint32_t ins, uint32_t reg) {
    const uint32_t a = (addr16 * 16) & v.memMask;
    for (int f = 0; f < 4; ++f)
        if (destBit(ins, f)) std::memcpy(v.mem + a + f * 4, &v.vf[reg].u32[f], 4);
}

void Vu::cop2(Ctx* c, uint32_t raw) {
    VuState& v = v_[0];
    const uint32_t rs = (raw >> 21) & 31;
    const uint32_t rt = (raw >> 16) & 31;
    if (!(rs & 16)) {
        const uint32_t id = (raw >> 11) & 31;
        switch (rs) {
        case 1: // QMFC2
            if (rt) c->r[rt] = v.vf[id];
            return;
        case 5: // QMTC2
            if (id) v.vf[id] = c->r[rt];
            return;
        case 2: { // CFC2
            uint32_t val = 0;
            if (id < 16) val = v.vi[id];
            else switch (id) {
                case 16: val = v.status; break;
                case 17: val = v.mac; break;
                case 18: val = v.clip; break;
                case 20: val = v.r; break;
                case 21: val = v.i; break;
                case 22: val = v.q; break;
                case 26: val = v.tpc; break;
                case 27: val = cmsar0; break;
                case 28: val = fbrst; break;
                case 29: val = (v_[0].running ? 1u : 0u) | (v_[1].running ? 0x100u : 0u); break;
                case 31: val = v_[1].tpc; break;
                default: val = 0;
                }
            if (rt) c->r[rt].u64[0] = (uint64_t)(int64_t)(int32_t)val;
            return;
        }
        case 6: { // CTC2
            const uint32_t val = c->r[rt].u32[0];
            if (id < 16) {
                if (id) v.vi[id] = (uint16_t)val;
            } else switch (id) {
                case 16: v.status = (v.status & 0x3F) | (val & 0xFC0); break;
                case 17: break; // MAC read-only
                case 18: v.clip = val & 0xFFFFFF; break;
                case 20: v.r = 0x3F800000 | (val & 0x7FFFFF); break;
                case 21: v.i = val; break;
                case 22: v.q = val; break;
                case 27: cmsar0 = val & 0xFFFF; break;
                case 28:
                    fbrst = val & 0x0C0C;
                    if (val & 0x2) v_[0].running = false;
                    if (val & 0x200) v_[1].running = false;
                    break;
                case 31: startMicro(1, val & 0xFFFF); break;
                default: break;
                }
            return;
        }
        default:
            return;
        }
    }
    const uint32_t funct = raw & 0x3F;
    if (funct < 60) {
        switch (funct) {
        case 48: v.vi[(raw >> 6) & 15] = (uint16_t)(v.vi[(raw >> 11) & 15] + v.vi[(raw >> 16) & 15]); break;
        case 49: v.vi[(raw >> 6) & 15] = (uint16_t)(v.vi[(raw >> 11) & 15] - v.vi[(raw >> 16) & 15]); break;
        case 50: { // VIADDI it, is, imm5
            int32_t imm = (int32_t)((raw >> 6) & 0x1F);
            if (imm & 0x10) imm -= 0x20;
            v.vi[(raw >> 16) & 15] = (uint16_t)(v.vi[(raw >> 11) & 15] + imm);
            break;
        }
        case 52: v.vi[(raw >> 6) & 15] = v.vi[(raw >> 11) & 15] & v.vi[(raw >> 16) & 15]; break;
        case 53: v.vi[(raw >> 6) & 15] = v.vi[(raw >> 11) & 15] | v.vi[(raw >> 16) & 15]; break;
        case 56: startMicro(0, (raw >> 6) & 0x7FFF); break;
        case 57: startMicro(0, cmsar0); break;
        default: vuops::upper(v, raw); break;
        }
        v.vi[0] = 0;
        return;
    }
    const uint32_t idx = (((raw >> 6) & 0x1F) << 2) | (raw & 3);
    const uint32_t ft = FT(raw), fs = FS(raw);
    switch (idx) {
    case 48: { // VMOVE
        Reg128 o = v.vf[ft];
        for (int f = 0; f < 4; ++f)
            if (destBit(raw, f)) o.u32[f] = v.vf[fs].u32[f];
        writeVf(v, ft, o);
        break;
    }
    case 49: { // VMR32
        Reg128 o = v.vf[ft], s = v.vf[fs];
        const uint32_t rot[4] = {s.u32[1], s.u32[2], s.u32[3], s.u32[0]};
        for (int f = 0; f < 4; ++f)
            if (destBit(raw, f)) o.u32[f] = rot[f];
        writeVf(v, ft, o);
        break;
    }
    case 52: { // VLQI ft, (is++)
        const uint32_t is = fs & 15;
        loadQ(v, v.vi[is], raw, ft);
        if (is) v.vi[is]++;
        break;
    }
    case 53: { // VSQI fs, (it++)
        const uint32_t it = ft & 15;
        storeQ(v, v.vi[it], raw, fs);
        if (it) v.vi[it]++;
        break;
    }
    case 54: { // VLQD
        const uint32_t is = fs & 15;
        if (is) v.vi[is]--;
        loadQ(v, v.vi[is], raw, ft);
        break;
    }
    case 55: { // VSQD
        const uint32_t it = ft & 15;
        if (it) v.vi[it]--;
        storeQ(v, v.vi[it], raw, fs);
        break;
    }
    case 56: vuDiv(v, fieldOf(v.vf[fs], (raw >> 21) & 3), fieldOf(v.vf[ft], (raw >> 23) & 3)); break;
    case 57: vuSqrt(v, fieldOf(v.vf[ft], (raw >> 23) & 3)); break;
    case 58: vuRsqrt(v, fieldOf(v.vf[fs], (raw >> 21) & 3), fieldOf(v.vf[ft], (raw >> 23) & 3)); break;
    case 59: break; // VWAITQ
    case 60: // VMTIR it, fs.fsf
        if (ft & 15) v.vi[ft & 15] = (uint16_t)fieldOf(v.vf[fs], (raw >> 21) & 3);
        break;
    case 61: { // VMFIR ft, is
        Reg128 o = v.vf[ft];
        const uint32_t val = (uint32_t)(int32_t)(int16_t)v.vi[fs & 15];
        for (int f = 0; f < 4; ++f)
            if (destBit(raw, f)) o.u32[f] = val;
        writeVf(v, ft, o);
        break;
    }
    case 62: { // VILWR it, (is)
        const uint32_t a = (v.vi[fs & 15] * 16u) & v.memMask;
        for (int f = 0; f < 4; ++f)
            if (destBit(raw, f)) {
                uint16_t x;
                std::memcpy(&x, v.mem + a + f * 4, 2);
                if (ft & 15) v.vi[ft & 15] = x;
                break;
            }
        break;
    }
    case 63: { // VISWR it, (is)
        const uint32_t a = (v.vi[fs & 15] * 16u) & v.memMask;
        const uint32_t val = v.vi[ft & 15];
        for (int f = 0; f < 4; ++f)
            if (destBit(raw, f)) std::memcpy(v.mem + a + f * 4, &val, 4);
        break;
    }
    case 64: { // VRNEXT ft
        rnext(v);
        Reg128 o = v.vf[ft];
        for (int f = 0; f < 4; ++f)
            if (destBit(raw, f)) o.u32[f] = v.r;
        writeVf(v, ft, o);
        break;
    }
    case 65: { // VRGET
        Reg128 o = v.vf[ft];
        for (int f = 0; f < 4; ++f)
            if (destBit(raw, f)) o.u32[f] = v.r;
        writeVf(v, ft, o);
        break;
    }
    case 66: v.r = 0x3F800000 | (fieldOf(v.vf[fs], (raw >> 21) & 3) & 0x7FFFFF); break;
    case 67: v.r = 0x3F800000 | ((v.r ^ fieldOf(v.vf[fs], (raw >> 21) & 3)) & 0x7FFFFF); break;
    default: vuops::upper(v, raw); break;
    }
    v.vi[0] = 0;
}

// ------------------------------------------------------------------ micro mode

void Vu::startMicro(int vu, uint32_t addr) {
    VuState& v = v_[vu & 1];
    v.tpc = addr & (v.codeMask >> 3);
    v.running = true;
    runMicro(v);
}

void Vu::xgkick(uint32_t vuMemAddr) { m_.gs->gifPath1(m_.vu1Data, vuMemAddr); }

bool Vu::execLower(VuState& v, uint32_t ins, uint32_t pc, uint32_t& nextPc, int& branchDelay) {
    const uint32_t op = ins >> 25;
    const uint32_t it = (ins >> 16) & 15, is = (ins >> 11) & 15;
    const uint32_t ft = (ins >> 16) & 31, fs = (ins >> 11) & 31;
    auto imm11 = [&]() {
        int32_t i = (int32_t)(ins & 0x7FF);
        if (i & 0x400) i -= 0x800;
        return i;
    };
    auto setVi = [&](uint32_t r, uint32_t val) {
        if (r) v.vi[r] = (uint16_t)val;
    };
    auto branchTo = [&](int32_t off) {
        nextPc = (pc + 1 + (uint32_t)off) & (v.codeMask >> 3);
        branchDelay = 1;
    };
    switch (op) {
    case 0x00: loadQ(v, (uint32_t)(v.vi[is] + imm11()), ins, ft); break;               // LQ
    case 0x01: storeQ(v, (uint32_t)(v.vi[it] + imm11()), ins, fs); break;              // SQ
    case 0x04: {                                                                       // ILW
        const uint32_t a = ((uint32_t)(v.vi[is] + imm11()) * 16) & v.memMask;
        for (int f = 0; f < 4; ++f)
            if (destBit(ins, f)) {
                uint16_t x;
                std::memcpy(&x, v.mem + a + f * 4, 2);
                setVi(it, x);
                break;
            }
        break;
    }
    case 0x05: { // ISW
        const uint32_t a = ((uint32_t)(v.vi[is] + imm11()) * 16) & v.memMask;
        const uint32_t val = v.vi[it];
        for (int f = 0; f < 4; ++f)
            if (destBit(ins, f)) std::memcpy(v.mem + a + f * 4, &val, 4);
        break;
    }
    case 0x08: setVi(it, v.vi[is] + (((ins >> 10) & 0x7800) | (ins & 0x7FF))); break; // IADDIU
    case 0x09: setVi(it, v.vi[is] - (((ins >> 10) & 0x7800) | (ins & 0x7FF))); break; // ISUBIU
    case 0x10: v.vi[1] = ((v.clip & 0xFFFFFF) == (ins & 0xFFFFFF)) ? 1 : 0; break;     // FCEQ
    case 0x11: v.clip = ins & 0xFFFFFF; break;                                         // FCSET
    case 0x12: v.vi[1] = (v.clip & ins & 0xFFFFFF) ? 1 : 0; break;                     // FCAND
    case 0x13: v.vi[1] = (((v.clip | ins) & 0xFFFFFF) == 0xFFFFFF) ? 1 : 0; break;     // FCOR
    case 0x14: { const uint32_t i12 = ((ins >> 10) & 0x800) | (ins & 0x7FF); setVi(it, (v.status & 0xFFF) == i12); break; }
    case 0x15: { const uint32_t i12 = ((ins >> 10) & 0x800) | (ins & 0x7FF); v.status = (v.status & 0x3F) | (i12 & 0xFC0); break; }
    case 0x16: { const uint32_t i12 = ((ins >> 10) & 0x800) | (ins & 0x7FF); setVi(it, v.status & i12); break; }
    case 0x17: { const uint32_t i12 = ((ins >> 10) & 0x800) | (ins & 0x7FF); setVi(it, ((v.status | i12) & 0xFFF) == 0xFFF); break; }
    case 0x18: setVi(it, (v.mac & 0xFFFF) == v.vi[is]); break;          // FMEQ
    case 0x1A: setVi(it, v.mac & v.vi[is]); break;                     // FMAND
    case 0x1B: setVi(it, ((v.mac | v.vi[is]) & 0xFFFF) == 0xFFFF); break; // FMOR
    case 0x1C: setVi(it, v.clip & 0xFFF); break;                        // FCGET
    case 0x20: branchTo(imm11()); break;                                // B
    case 0x21: setVi(it, pc + 2); branchTo(imm11()); break;             // BAL
    case 0x24: nextPc = ((uint32_t)v.vi[is] * 1) & (v.codeMask >> 3); branchDelay = 1; break; // JR
    case 0x25: {
        const uint32_t target = v.vi[is];
        setVi(it, pc + 2);
        nextPc = target & (v.codeMask >> 3);
        branchDelay = 1;
        break;
    }
    case 0x28: if (v.vi[it] == v.vi[is]) branchTo(imm11()); break;
    case 0x29: if (v.vi[it] != v.vi[is]) branchTo(imm11()); break;
    case 0x2C: if ((int16_t)v.vi[is] < 0) branchTo(imm11()); break;
    case 0x2D: if ((int16_t)v.vi[is] > 0) branchTo(imm11()); break;
    case 0x2E: if ((int16_t)v.vi[is] <= 0) branchTo(imm11()); break;
    case 0x2F: if ((int16_t)v.vi[is] >= 0) branchTo(imm11()); break;
    case 0x40: {
        const uint32_t funct = ins & 0x3F;
        const uint32_t id = (ins >> 6) & 15;
        if (funct < 0x3C) {
            switch (funct) {
            case 0x30: setVi(id, v.vi[is] + v.vi[it]); break;
            case 0x31: setVi(id, v.vi[is] - v.vi[it]); break;
            case 0x32: {
                int32_t imm = (int32_t)((ins >> 6) & 0x1F);
                if (imm & 0x10) imm -= 0x20;
                setVi(it, v.vi[is] + imm);
                break;
            }
            case 0x34: setVi(id, v.vi[is] & v.vi[it]); break;
            case 0x35: setVi(id, v.vi[is] | v.vi[it]); break;
            default: break;
            }
            break;
        }
        const uint32_t idx = (((ins >> 6) & 0x1F) << 2) | (ins & 3);
        switch (idx) {
        case 48: { Reg128 o = v.vf[ft]; for (int f = 0; f < 4; ++f) if (destBit(ins, f)) o.u32[f] = v.vf[fs].u32[f]; writeVf(v, ft, o); break; }
        case 49: { Reg128 o = v.vf[ft], s = v.vf[fs]; const uint32_t rot[4] = {s.u32[1], s.u32[2], s.u32[3], s.u32[0]}; for (int f = 0; f < 4; ++f) if (destBit(ins, f)) o.u32[f] = rot[f]; writeVf(v, ft, o); break; }
        case 52: loadQ(v, v.vi[is], ins, ft); if (is) v.vi[is]++; break;
        case 53: storeQ(v, v.vi[it], ins, fs); if (it) v.vi[it]++; break;
        case 54: if (is) v.vi[is]--; loadQ(v, v.vi[is], ins, ft); break;
        case 55: if (it) v.vi[it]--; storeQ(v, v.vi[it], ins, fs); break;
        case 56: vuDiv(v, fieldOf(v.vf[fs], (ins >> 21) & 3), fieldOf(v.vf[ft], (ins >> 23) & 3)); break;
        case 57: vuSqrt(v, fieldOf(v.vf[ft], (ins >> 23) & 3)); break;
        case 58: vuRsqrt(v, fieldOf(v.vf[fs], (ins >> 21) & 3), fieldOf(v.vf[ft], (ins >> 23) & 3)); break;
        case 59: break; // WAITQ
        case 60: setVi(it, fieldOf(v.vf[fs], (ins >> 21) & 3)); break;
        case 61: { Reg128 o = v.vf[ft]; const uint32_t val = (uint32_t)(int32_t)(int16_t)v.vi[is]; for (int f = 0; f < 4; ++f) if (destBit(ins, f)) o.u32[f] = val; writeVf(v, ft, o); break; }
        case 62: { const uint32_t a = (v.vi[is] * 16u) & v.memMask; for (int f = 0; f < 4; ++f) if (destBit(ins, f)) { uint16_t x; std::memcpy(&x, v.mem + a + f * 4, 2); setVi(it, x); break; } break; }
        case 63: { const uint32_t a = (v.vi[is] * 16u) & v.memMask; const uint32_t val = v.vi[it]; for (int f = 0; f < 4; ++f) if (destBit(ins, f)) std::memcpy(v.mem + a + f * 4, &val, 4); break; }
        case 64: { rnext(v); Reg128 o = v.vf[ft]; for (int f = 0; f < 4; ++f) if (destBit(ins, f)) o.u32[f] = v.r; writeVf(v, ft, o); break; }
        case 65: { Reg128 o = v.vf[ft]; for (int f = 0; f < 4; ++f) if (destBit(ins, f)) o.u32[f] = v.r; writeVf(v, ft, o); break; }
        case 66: v.r = 0x3F800000 | (fieldOf(v.vf[fs], (ins >> 21) & 3) & 0x7FFFFF); break;
        case 67: v.r = 0x3F800000 | ((v.r ^ fieldOf(v.vf[fs], (ins >> 21) & 3)) & 0x7FFFFF); break;
        case 100: { Reg128 o = v.vf[ft]; for (int f = 0; f < 4; ++f) if (destBit(ins, f)) o.u32[f] = v.p; writeVf(v, ft, o); break; } // MFP
        case 104: setVi(it, m_.vif->state(1).top); break;              // XTOP
        case 105: setVi(it, m_.vif->state(v.index).itop); break;       // XITOP
        case 108: if (v.index == 1) xgkick(v.vi[is]); break;
        case 112: case 113: case 114: case 115: case 116: case 117: case 118: case 120: case 121: case 122:
        case 124: case 125: case 126: { // EFU
            const Reg128 s = v.vf[fs];
            auto f2 = [](uint32_t x) { float f; std::memcpy(&f, &x, 4); return f; };
            auto u2 = [](float f) { uint32_t x; std::memcpy(&x, &f, 4); return x; };
            const float x = f2(s.u32[0]), y = f2(s.u32[1]), z = f2(s.u32[2]), w = f2(s.u32[3]);
            const float fsf = f2(s.u32[(ins >> 21) & 3]);
            float r = 0;
            switch (idx) {
            case 112: r = x * x + y * y + z * z; break;                // ESADD
            case 113: r = 1.0f / (x * x + y * y + z * z); break;       // ERSADD
            case 114: r = std::sqrt(x * x + y * y + z * z); break; // ELENG
            case 115: r = 1.0f / std::sqrt(x * x + y * y + z * z); break;
            case 116: r = std::atan2(y, x); break;              // EATANxy
            case 117: r = std::atan2(z, x); break;              // EATANxz
            case 118: r = x + y + z + w; break;                       // ESUM
            case 120: r = std::sqrt(fsf < 0 ? -fsf : fsf); break;
            case 121: r = 1.0f / std::sqrt(fsf < 0 ? -fsf : fsf); break;
            case 122: r = 1.0f / fsf; break;
            case 124: r = std::sin(fsf); break;
            case 125: r = std::atan(fsf); break;
            case 126: r = std::exp(-fsf); break;
            }
            v.p = u2(r);
            break;
        }
        case 123: break; // WAITP
        default: break;
        }
        break;
    }
    default:
        break;
    }
    v.vi[0] = 0;
    return true;
}

void Vu::runMicro(VuState& v) {
    uint32_t pc = v.tpc;
    int branchPending = 0;
    uint32_t branchTarget = 0;
    bool endPending = false;
    const uint32_t pcMask = v.codeMask >> 3;
    uint64_t steps = 0;
    while (v.running) {
        uint32_t lower, upperW;
        std::memcpy(&lower, v.code + (pc & pcMask) * 8, 4);
        std::memcpy(&upperW, v.code + (pc & pcMask) * 8 + 4, 4);
        const bool iBit = (upperW >> 31) & 1;
        const bool eBit = (upperW >> 30) & 1;
        uint32_t nextPc = (pc + 1) & pcMask;
        int bd = 0;
        if (iBit) {
            vuops::upper(v, upperW);
            v.i = lower;
        } else {
            // Upper reads happen before lower writes: if lower writes a vf
            // register the upper op reads, run upper first.
            const uint32_t uFs = (upperW >> 11) & 31, uFt = (upperW >> 16) & 31;
            const uint32_t lOp = lower >> 25;
            const uint32_t lFt = (lower >> 16) & 31;
            const bool lowerWritesFt = lOp == 0x00 || (lOp == 0x40 && (lower & 0x3C) == 0x3C &&
                                                        ((((lower >> 6) & 0x1F) << 2 | (lower & 3)) == 48 ||
                                                         (((lower >> 6) & 0x1F) << 2 | (lower & 3)) == 49 ||
                                                         (((lower >> 6) & 0x1F) << 2 | (lower & 3)) == 52 ||
                                                         (((lower >> 6) & 0x1F) << 2 | (lower & 3)) == 54));
            if (lowerWritesFt && lFt != 0 && (lFt == uFs || lFt == uFt)) {
                vuops::upper(v, upperW);
                execLower(v, lower, pc, nextPc, bd);
            } else {
                execLower(v, lower, pc, nextPc, bd);
                vuops::upper(v, upperW);
            }
        }
        v.vf[0].u32[0] = 0;
        v.vf[0].u32[1] = 0;
        v.vf[0].u32[2] = 0;
        v.vf[0].u32[3] = 0x3F800000;
        ++steps;
        if (endPending) {
            v.running = false;
            pc = nextPc;
            break;
        }
        if (eBit) endPending = true;
        if (branchPending) {
            pc = branchTarget;
            branchPending = 0;
        } else {
            pc = (pc + 1) & pcMask;
        }
        if (bd) {
            branchPending = 1;
            branchTarget = nextPc;
        }
        if (steps > 20000000) {
            logError("vu%d: micro program did not terminate (pc=%04x)", v.index, pc);
            v.running = false;
            break;
        }
    }
    v.tpc = pc;
    v.microSteps += steps;
}

void Vu::execUpper(VuState& v, uint32_t ins) { vuops::upper(v, ins); }

} // namespace ps2
