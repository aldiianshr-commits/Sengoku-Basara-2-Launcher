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

inline Result mul(uint32_t a, uint32_t b) {
    const uint32_t sign = (a ^ b) & SIGN;
    if (isZero(a) || isZero(b)) return {sign, false, false};
    uint64_t m = (uint64_t)mantOf(a) * mantOf(b);
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

inline DivResult div(uint32_t a, uint32_t b) {
    const uint32_t sign = (a ^ b) & SIGN;
    if (isZero(b)) {
        if (isZero(a)) return {sign | MAXMAG, false, false, false, true};
        return {sign | MAXMAG, false, false, true, false};
    }
    if (isZero(a)) return {sign, false, false, false, false};
    const uint64_t ma = mantOf(a), mb = mantOf(b);
    uint64_t q = (ma << 24) / mb;
    int32_t e = (int32_t)expOf(a) - (int32_t)expOf(b) + 127;
    if (q & (1ull << 24)) q >>= 1;
    else --e;
    Result r = pack(sign, e, (uint32_t)q);
    return {r.value, r.overflow, r.underflow, false, false};
}

// Square root of |x| (truncated); the caller handles the sign flag.
inline uint32_t sqrtMag(uint32_t x) {
    if (isZero(x)) return 0;
    int32_t E = (int32_t)expOf(x) - 127;
    uint64_t M = mantOf(x);
    if (E & 1) {
        M <<= 1;
        E -= 1;
    }
    uint64_t v = M << 23;
    // Integer square root (floor).
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    const int32_t e = E / 2 + 127;
    return ((uint32_t)e << 23) | ((uint32_t)r & 0x7FFFFF);
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
