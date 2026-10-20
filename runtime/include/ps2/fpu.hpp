// PS2 COP1 (FPU) arithmetic.
//
// The EE FPU is not IEEE-754: there are no NaNs or infinities (exponent 255
// is an ordinary exponent, so the largest value is 0x7FFFFFFF), denormal
// inputs are treated as zero, results that underflow flush to zero, results
// that overflow clamp to +/-0x7FFFFFFF, and rounding truncates toward zero.
// Everything here is done with integer arithmetic so results are identical
// on every host, independent of the host FPU rounding mode.
#pragma once
#include <cstdint>

#include "context.hpp"

namespace ps2::fpu {

constexpr uint32_t FLAG_C = 0x00800000;
constexpr uint32_t FLAG_I = 0x00020000;
constexpr uint32_t FLAG_D = 0x00010000;
constexpr uint32_t FLAG_O = 0x00008000;
constexpr uint32_t FLAG_U = 0x00004000;
constexpr uint32_t FLAG_SI = 0x00000040;
constexpr uint32_t FLAG_SD = 0x00000020;
constexpr uint32_t FLAG_SO = 0x00000010;
constexpr uint32_t FLAG_SU = 0x00000008;
constexpr uint32_t FCR31_WRITE_MASK = 0x0083C078;
constexpr uint32_t FCR0_VALUE = 0x00002E30;

constexpr uint32_t SIGN = 0x80000000u;
constexpr uint32_t MAXMAG = 0x7FFFFFFFu;

PS2_FORCEINLINE uint32_t expOf(uint32_t x) { return (x >> 23) & 0xFF; }
PS2_FORCEINLINE uint32_t mantOf(uint32_t x) { return (x & 0x7FFFFF) | 0x800000; }
PS2_FORCEINLINE bool isZero(uint32_t x) { return expOf(x) == 0; }

struct Result {
    uint32_t value;
    bool overflow;
    bool underflow;
};

// Pack sign/exponent/24-bit mantissa (hidden bit at 23), handling range.
PS2_FORCEINLINE Result pack(uint32_t sign, int32_t e, uint32_t m) {
    if (e > 255) return {sign | MAXMAG, true, false};
    if (e <= 0) return {sign, false, true};
    return {sign | ((uint32_t)e << 23) | (m & 0x7FFFFF), false, false};
}

inline Result add(uint32_t a, uint32_t b) {
    const bool az = isZero(a), bz = isZero(b);
    if (az && bz) return {(a & b) & SIGN, false, false};
    if (az) return {b, false, false};
    if (bz) return {a, false, false};
    // Order by magnitude.
    if ((a & MAXMAG) < (b & MAXMAG)) {
        uint32_t t = a;
        a = b;
        b = t;
    }
    const int32_t ea = (int32_t)expOf(a), eb = (int32_t)expOf(b);
    const uint32_t diff = (uint32_t)(ea - eb);
    const uint32_t sign = a & SIGN;
    // One guard bit: the smaller operand keeps bits down to half an ULP of
    // the larger one; anything below that is discarded before the add.
    uint64_t ma = (uint64_t)mantOf(a) << 1;
    uint64_t mb = diff >= 26 ? 0 : (((uint64_t)mantOf(b) << 1) >> diff);
    int32_t e = ea;
    if ((a ^ b) & SIGN) {
        uint64_t s = ma - mb;
        if (s == 0) return {0, false, false};
        while (s < (1ull << 24)) {
            s <<= 1;
            --e;
        }
        return pack(sign, e, (uint32_t)(s >> 1));
    }
    uint64_t s = ma + mb;
    if (s >= (1ull << 25)) {
        s >>= 1;
        ++e;
    }
    return pack(sign, e, (uint32_t)(s >> 1));
}

PS2_FORCEINLINE Result sub(uint32_t a, uint32_t b) { return add(a, b ^ SIGN); }

// The EE FPU multiplier is a radix-4 Booth multiplier whose partial
// products are not all summed exactly; bit 15 of the 48-bit mantissa product
// can differ from the exact product, which shows up as results one ULP below
// the truncated exact product (e.g. 1.0f * 0x3FFFFFFF == 0x3FFFFFFE on
// hardware).  This reproduces that circuit for the low 16 bits; validated
// against every multiply / multiply-add result in ps2autotests.
namespace detail {
struct Booth {
    uint32_t data, negate;
};
inline Booth booth(uint32_t a, uint32_t b, uint32_t bit) {
    const uint32_t test = (bit ? (b >> (bit * 2 - 1)) : (b << 1)) & 7;
    a <<= (bit * 2);
    a += (test == 3 || test == 4) ? a : 0;
    const uint32_t neg = (test >= 4 && test <= 6) ? ~0u : 0u;
    const uint32_t pos = 1u << (bit * 2);
    a ^= (neg & (0u - pos));
    a &= (test >= 1 && test <= 6) ? ~0u : 0u;
    return {a, neg & pos};
}
struct Sum {
    uint32_t lo, hi;
};
inline Sum add3(uint32_t a, uint32_t b, uint32_t c) {
    const uint32_t u = a ^ b;
    return {u ^ c, ((u & c) | (a & b)) << 1};
}
inline uint64_t mulMantissa(uint32_t a, uint32_t b) {
    const uint64_t full = (uint64_t)a * b;
    Booth bt[8];
    for (uint32_t i = 0; i < 8; ++i) bt[i] = booth(a, b, i);
    const Sum t0 = add3(bt[1].data, bt[2].data, bt[3].data);
    const Sum t1 = add3(bt[4].data & ~0x7FFu, bt[5].data & ~0xFFFu, bt[6].data);
    const Sum t2 = add3(bt[0].data, t0.hi, t0.lo);
    const Sum t3 = add3(bt[7].data, t1.hi, t1.lo);
    const Sum t4 = add3(t2.hi, t3.hi, t3.lo);
    Sum t5 = add3(t2.lo, t4.hi, t4.lo);
    t5.hi += bt[7].negate;
    t5.lo &= ~0x7FFFu;
    t5.hi &= ~0x7FFFu;
    const uint32_t ps2lo = t5.lo + t5.hi;
    return full - ((ps2lo ^ (uint32_t)full) & 0x8000u);
}
} // namespace detail

inline Result mul(uint32_t a, uint32_t b) {
    const uint32_t sign = (a ^ b) & SIGN;
    if (isZero(a) || isZero(b)) return {sign, false, false};
    uint64_t m = detail::mulMantissa(mantOf(a), mantOf(b));
    int32_t e = (int32_t)expOf(a) + (int32_t)expOf(b) - 127;
    if (m & (1ull << 47)) {
        m >>= 24;
        ++e;
    } else {
        m >>= 23;
    }
    return pack(sign, e, (uint32_t)m);
}

struct DivResult {
    uint32_t value;
    bool overflow, underflow, divByZero, invalid;
};

// Division: quotient of the mantissas rounded to nearest.  (The real divider
// is an iterative circuit that occasionally differs by one ULP from this;
// matches all division results in ps2autotests.)
inline DivResult div(uint32_t a, uint32_t b, bool truncate = false) {
    const uint32_t sign = (a ^ b) & SIGN;
    if (isZero(b)) {
        if (isZero(a)) return {sign | MAXMAG, false, false, false, true};
        return {sign | MAXMAG, false, false, true, false};
    }
    if (isZero(a)) return {sign, false, false, false, false};
    const uint64_t ma = mantOf(a), mb = mantOf(b);
    int32_t e = (int32_t)expOf(a) - (int32_t)expOf(b) + 127;
    uint64_t q, r;
    if (ma >= mb) {
        q = (ma << 23) / mb;
        r = (ma << 23) % mb;
    } else {
        q = (ma << 24) / mb;
        r = (ma << 24) % mb;
        --e;
    }
    if (!truncate && (r * 2 > mb || (r * 2 == mb && (q & 1)))) {
        ++q;
        if (q >= (1ull << 24)) {
            q >>= 1;
            ++e;
        }
    }
    Result res = pack(sign, e, (uint32_t)q);
    return {res.value, res.overflow, res.underflow, false, false};
}

// Square root of |x| rounded to nearest; the caller handles the sign flag.
inline uint32_t sqrtMag(uint32_t x) {
    if (isZero(x)) return 0;
    int32_t E = (int32_t)expOf(x) - 127;
    uint64_t M = mantOf(x);
    if (E & 1) {
        M <<= 1;
        E -= 1;
    }
    const uint64_t v = M << 23;
    // Integer square root (floor).
    uint64_t rem = v, r = 0, bit = 1ull << 62;
    while (bit > rem) bit >>= 2;
    while (bit) {
        if (rem >= r + bit) {
            rem -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    // Round to nearest: 4v > (2r+1)^2  <=>  v - r^2 > r  (exactly, integers)
    if (v - r * r > r) ++r;
    int32_t e = E / 2 + 127;
    if (r >= (1ull << 24)) {
        r >>= 1;
        ++e;
    }
    return ((uint32_t)e << 23) | ((uint32_t)r & 0x7FFFFF);
}

// Ordering used by MAX.S / MIN.S: plain sign-magnitude, so -0 < +0.
PS2_FORCEINLINE int64_t orderKey(uint32_t x) {
    const int64_t mag = (int64_t)(x & MAXMAG);
    return (x & SIGN) ? -mag - 1 : mag;
}

// Signed numeric compare treating denormals as zero and -0 == +0.
PS2_FORCEINLINE int64_t cmpKey(uint32_t x) {
    if (isZero(x)) return 0;
    const int64_t mag = (int64_t)(x & MAXMAG);
    return (x & SIGN) ? -mag : mag;
}

PS2_FORCEINLINE void setOU(Ctx* c, const Result& r) {
    c->fcr31 &= ~(FLAG_O | FLAG_U);
    if (r.overflow) c->fcr31 |= FLAG_O | FLAG_SO;
    if (r.underflow) c->fcr31 |= FLAG_U | FLAG_SU;
}

// Truncating float -> int32 conversion with saturation.
PS2_FORCEINLINE uint32_t toInt(uint32_t x) {
    const uint32_t e = expOf(x);
    if (e <= 126) return 0;
    if (e >= 158) return (x & SIGN) ? 0x80000000u : 0x7FFFFFFFu;
    const uint32_t m = mantOf(x);
    uint32_t v = e >= 150 ? (m << (e - 150)) : (m >> (150 - e));
    return (x & SIGN) ? (uint32_t)(-(int32_t)v) : v;
}

// int32 -> float (truncating).
PS2_FORCEINLINE uint32_t fromInt(uint32_t iv) {
    const int32_t s = (int32_t)iv;
    if (s == 0) return 0;
    const uint32_t sign = s < 0 ? SIGN : 0;
    uint64_t mag = s < 0 ? (uint64_t)(-(int64_t)s) : (uint64_t)s;
    int32_t e = 150;
    while (mag >= (1ull << 24)) {
        mag >>= 1;
        ++e;
    }
    while (mag < (1ull << 23)) {
        mag <<= 1;
        --e;
    }
    return sign | ((uint32_t)e << 23) | ((uint32_t)mag & 0x7FFFFF);
}

} // namespace ps2::fpu
