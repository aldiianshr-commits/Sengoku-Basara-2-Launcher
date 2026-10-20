#include "ps2/spu2.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ps2/log.hpp"
#include "ps2/machine.hpp"

namespace ps2 {

namespace {
constexpr int32_t kAdpcmF0[5] = {0, 60, 115, 98, 122};
constexpr int32_t kAdpcmF1[5] = {0, 0, -52, -55, -60};
constexpr uint32_t kRamWords = 1024 * 1024; // 2 MB of 16-bit words

inline int32_t clamp16(int32_t v) { return std::clamp(v, -32768, 32767); }
inline int32_t applyVol(int32_t s, int32_t vol) { return (s * vol) >> 15; }

// Envelope step (PS1/PS2 SPU formula): rate 0..127.
void envStep(int32_t& level, int32_t& counter, int rate, bool decrease, bool exponential) {
    const int shift = rate >> 2;
    int32_t step = decrease ? (-8 + (rate & 3)) : (7 - (rate & 3));
    int32_t cycles = 1 << std::max(0, shift - 11);
    step <<= std::max(0, 11 - shift);
    if (exponential && !decrease && level > 0x6000) cycles <<= 2;
    if (exponential && decrease) step = (step * level) >> 15;
    if (++counter < cycles) return;
    counter = 0;
    level = std::clamp(level + step, 0, 0x7FFF);
}

// Fixed or sweeping volume register -> current volume.
void volStep(uint16_t reg, int32_t& cur, int32_t& counter) {
    if (!(reg & 0x8000)) {
        cur = (int16_t)(reg << 1);
        return;
    }
    const bool exponential = reg & 0x4000, decrease = reg & 0x2000, inverted = reg & 0x1000;
    int32_t mag = std::abs(cur);
    envStep(mag, counter, reg & 0x7F, decrease, exponential);
    cur = inverted ? -mag : mag;
}

inline uint32_t join24(uint16_t hi, uint16_t lo) { return ((uint32_t)(hi & 0xFF) << 16) | lo; }
inline uint32_t joinAddr(uint16_t hi, uint16_t lo) { return (((uint32_t)hi << 16) | lo) & 0xFFFFF; }
} // namespace

Spu2::Spu2(Machine& m) : m_(m), ram_(kRamWords, 0) {
    for (auto& c : cores_) {
        c.eea = 0xFFFFF;
        c.mmix = 0xFF0;
    }
}

// ------------------------------------------------------------------ registers

uint16_t Spu2::read16(uint32_t addr) {
    const uint32_t a = addr & 0x7FF;
    if (a < 0x760) {
        const int ci = a >= 0x400 ? 1 : 0;
        const uint32_t ca = a & 0x3FF;
        SpuCore& c = cores_[ci];
        if (ca < 0x180) {
            const int vi = (int)(ca >> 4);
            if (vi < 24) {
                SpuVoice& v = c.v[(size_t)vi];
                switch (ca & 0xF) {
                case 0xA: return (uint16_t)v.env.level;
                case 0xC: return (uint16_t)(v.curVolL >> 1);
                case 0xE: return (uint16_t)(v.curVolR >> 1);
                default: break;
                }
            }
        } else if (ca >= 0x1C0 && ca < 0x1C0 + 24 * 0xC) {
            const int vi = (int)((ca - 0x1C0) / 0xC);
            const uint32_t off = (ca - 0x1C0) % 0xC;
            const SpuVoice& v = c.v[(size_t)vi];
            if (off == 0x8) return (uint16_t)(v.nax >> 16);
            if (off == 0xA) return (uint16_t)v.nax;
        } else {
            switch (ca) {
            case 0x340: return (uint16_t)c.endx;
            case 0x342: return (uint16_t)(c.endx >> 16);
            case 0x344: return c.statx;
            case 0x1B0: return c.admas;
            case 0x19A: return c.attr;
            default: break;
            }
        }
    }
    return regs_[a >> 1];
}

void Spu2::write16(uint32_t addr, uint16_t val) {
    const uint32_t a = addr & 0x7FF;
    regs_[a >> 1] = val;
    if (a >= 0x760) {
        // General volume registers: core 0 at 0x760, core 1 at 0x788.
        const int ci = a >= 0x788 ? 1 : 0;
        const uint32_t ra = a - (ci ? 0x788 : 0x760);
        SpuCore& c = cores_[ci];
        switch (ra) {
        case 0x00: c.mvolL = val; break;
        case 0x02: c.mvolR = val; break;
        case 0x04: c.evolL = (int16_t)val; break;
        case 0x06: c.evolR = (int16_t)val; break;
        case 0x08: c.avolL = (int16_t)val; break;
        case 0x0A: c.avolR = (int16_t)val; break;
        case 0x0C: c.bvolL = (int16_t)val; break;
        case 0x0E: c.bvolR = (int16_t)val; break;
        default:
            if (ra >= 0x14 && ra < 0x28) c.reverb[0x30 + (ra - 0x14) / 2] = val; // IIR_ALPHA.. IN_COEF_R
            break;
        }
        if (a == 0x7C0) spdifOut_ = val;
        return;
    }
    const int ci = a >= 0x400 ? 1 : 0;
    const uint32_t ca = a & 0x3FF;
    SpuCore& c = cores_[ci];
    if (ca < 0x180) {
        const int vi = (int)(ca >> 4);
        if (vi >= 24) return;
        SpuVoice& v = c.v[(size_t)vi];
        switch (ca & 0xF) {
        case 0x0: v.volL = val; if (!(val & 0x8000)) v.curVolL = (int16_t)(val << 1); break;
        case 0x2: v.volR = val; if (!(val & 0x8000)) v.curVolR = (int16_t)(val << 1); break;
        case 0x4: v.pitch = val; break;
        case 0x6: v.adsr1 = val; break;
        case 0x8: v.adsr2 = val; break;
        case 0xA: v.env.level = val & 0x7FFF; break;
        default: break;
        }
        return;
    }
    if (ca >= 0x1C0 && ca < 0x1C0 + 24 * 0xC) {
        const int vi = (int)((ca - 0x1C0) / 0xC);
        const uint32_t off = (ca - 0x1C0) % 0xC;
        SpuVoice& v = c.v[(size_t)vi];
        auto hi = [&](uint32_t& r) { r = ((uint32_t)(val & 0xF) << 16) | (r & 0xFFFF); };
        auto lo = [&](uint32_t& r) { r = (r & 0xF0000) | val; };
        switch (off) {
        case 0x0: hi(v.ssa); break;
        case 0x2: lo(v.ssa); break;
        case 0x4: hi(v.lsa); v.customLoop = true; break;
        case 0x6: lo(v.lsa); v.customLoop = true; break;
        case 0x8: hi(v.nax); break;
        case 0xA: lo(v.nax); break;
        default: break;
        }
        return;
    }
    auto mask24 = [&](uint32_t& r, bool high) {
        r = high ? ((r & 0xFFFF) | ((uint32_t)(val & 0xFF) << 16)) : ((r & 0xFF0000) | val);
    };
    switch (ca) {
    case 0x180: mask24(c.pmon, false); break;
    case 0x182: mask24(c.pmon, true); break;
    case 0x184: mask24(c.non, false); break;
    case 0x186: mask24(c.non, true); break;
    case 0x188: mask24(c.vmixl, false); break;
    case 0x18A: mask24(c.vmixl, true); break;
    case 0x18C: mask24(c.vmixel, false); break;
    case 0x18E: mask24(c.vmixel, true); break;
    case 0x190: mask24(c.vmixr, false); break;
    case 0x192: mask24(c.vmixr, true); break;
    case 0x194: mask24(c.vmixer, false); break;
    case 0x196: mask24(c.vmixer, true); break;
    case 0x198: c.mmix = ci == 0 ? (val & 0xFF0) : val; break;
    case 0x19A:
        c.attr = val;
        if (!(val & 0x40)) c.irqFlag = false;
        break;
    case 0x19C: c.irqa = (c.irqa & 0xFFFF) | ((uint32_t)(val & 0xF) << 16); break;
    case 0x19E: c.irqa = (c.irqa & 0xF0000) | val; break;
    case 0x1A0: for (int i = 0; i < 16; ++i) if (val & (1 << i)) keyOn(ci, i); break;
    case 0x1A2: for (int i = 0; i < 8; ++i) if (val & (1 << i)) keyOn(ci, 16 + i); break;
    case 0x1A4: for (int i = 0; i < 16; ++i) if (val & (1 << i)) keyOff(ci, i); break;
    case 0x1A6: for (int i = 0; i < 8; ++i) if (val & (1 << i)) keyOff(ci, 16 + i); break;
    case 0x1A8: c.tsa = (c.tsa & 0xFFFF) | ((uint32_t)(val & 0xF) << 16); break;
    case 0x1AA: c.tsa = (c.tsa & 0xF0000) | val; break;
    case 0x1AC: ramw(c.tsa) = val; c.tsa = (c.tsa + 1) & 0xFFFFF; break; // manual transfer port
    case 0x1B0: c.admas = val; break;
    case 0x2E0: c.esa = (c.esa & 0xFFFF) | ((uint32_t)(val & 0xF) << 16); break;
    case 0x2E2: c.esa = (c.esa & 0xF0000) | val; break;
    case 0x33C: c.eea = ((uint32_t)(val & 0xF) << 16) | 0xFFFF; break;
    case 0x340: c.endx &= ~(uint32_t)val; break;
    case 0x342: c.endx &= ~((uint32_t)(val & 0xFF) << 16); break;
    default:
        if (ca >= 0x2E4 && ca < 0x33C) c.reverb[(ca - 0x2E4) / 2] = val;
        break;
    }
}

void Spu2::dmaWrite(int core, const uint16_t* data, size_t count) {
    SpuCore& c = cores_[core & 1];
    for (size_t i = 0; i < count; ++i) ramw(c.tsa + (uint32_t)i) = data[i];
    c.tsa = (c.tsa + (uint32_t)count) & 0xFFFFF;
}

// ------------------------------------------------------------------ voices

void Spu2::keyOn(int ci, int vi) {
    SpuCore& c = cores_[ci];
    SpuVoice& v = c.v[(size_t)vi];
    v.nax = v.ssa;
    if (!v.customLoop) v.lsa = v.ssa;
    v.customLoop = false;
    v.blockPos = 28;
    v.hist1 = v.hist2 = 0;
    std::memset(v.prev, 0, sizeof v.prev);
    v.counter = 0;
    v.env.phase = 1;
    v.env.level = 0;
    v.env.counter = 0;
    c.endx &= ~(1u << vi);
    PS2_DEBUG(Spu, "KON core%d v%d ssa=%05x pitch=%04x adsr=%04x/%04x vol=%04x/%04x", ci, vi, v.ssa, v.pitch, v.adsr1, v.adsr2,
              v.volL, v.volR);
}

void Spu2::keyOff(int ci, int vi) {
    SpuVoice& v = cores_[ci].v[(size_t)vi];
    if (v.env.phase) {
        v.env.phase = 4;
        v.env.counter = 0;
    }
}

int16_t Spu2::decodeNext(SpuCore& c, SpuVoice& v, int ci, int vi) {
    if (v.blockPos >= 28) {
        if (v.blockPos != 99) {
            // Finished the previous block: handle its loop flags.
        }
        const uint32_t base = v.nax & ~7u;
        const uint16_t h = ramw(base);
        const uint8_t header = (uint8_t)(h & 0xFF);
        v.blockFlags = (uint8_t)(h >> 8);
        const int shiftRaw = header & 0xF;
        const int shift = shiftRaw > 12 ? 9 : shiftRaw;
        const int filter = std::min(4, (header >> 4) & 0x7);
        if (v.blockFlags & 4) v.lsa = base;
        for (int i = 0; i < 28; ++i) {
            const uint16_t w = ramw(base + 1 + (uint32_t)(i >> 2));
            int32_t s = (int16_t)(((w >> ((i & 3) * 4)) & 0xF) << 12) >> shift;
            s += (v.hist1 * kAdpcmF0[filter] + v.hist2 * kAdpcmF1[filter] + 32) >> 6;
            s = clamp16(s);
            v.hist2 = v.hist1;
            v.hist1 = s;
            v.block[i] = (int16_t)s;
        }
        // IRQ when the block containing IRQA is read.
        for (int k = 0; k < 2; ++k) {
            SpuCore& ck = cores_[k];
            if ((ck.attr & 0x40) && (ck.irqa & ~7u) == base) {
                ck.irqFlag = true;
                ck.statx |= 0x40;
            }
        }
        v.blockPos = 0;
        // Advance NAX now; loop end jumps take effect after this block.
        if (v.blockFlags & 1) {
            c.endx |= 1u << vi;
            v.nax = v.lsa;
            if (!(v.blockFlags & 2)) {
                // Loop end without repeat: the voice is released and silenced
                // once this block has been played.
                v.blockFlags |= 0x80;
            }
        } else {
            v.nax = (base + 8) & 0xFFFFF;
        }
    }
    const int16_t s = v.block[v.blockPos++];
    if (v.blockPos >= 28 && (v.blockFlags & 0x80)) {
        v.env.phase = 0;
        v.env.level = 0;
    }
    (void)ci;
    return s;
}

void Spu2::stepEnvelope(SpuVoice& v) {
    SpuEnvelope& e = v.env;
    switch (e.phase) {
    case 1: { // attack
        envStep(e.level, e.counter, (v.adsr1 >> 8) & 0x7F, false, (v.adsr1 & 0x8000) != 0);
        if (e.level >= 0x7FFF) {
            e.level = 0x7FFF;
            e.phase = 2;
            e.counter = 0;
        }
        break;
    }
    case 2: { // decay
        const int32_t sustain = std::min(0x7FFF, (((v.adsr1 & 0xF) + 1) * 0x800));
        envStep(e.level, e.counter, ((v.adsr1 >> 4) & 0xF) << 2, true, true);
        if (e.level <= sustain) {
            e.phase = 3;
            e.counter = 0;
        }
        break;
    }
    case 3: // sustain
        envStep(e.level, e.counter, (v.adsr2 >> 6) & 0x7F, (v.adsr2 & 0x4000) != 0, (v.adsr2 & 0x8000) != 0);
        break;
    case 4: // release
        envStep(e.level, e.counter, (v.adsr2 & 0x1F) << 2, true, (v.adsr2 & 0x20) != 0);
        if (e.level <= 0) {
            e.level = 0;
            e.phase = 0;
        }
        break;
    default: break;
    }
}

int32_t Spu2::voiceSample(SpuCore& c, int ci, int vi) {
    SpuVoice& v = c.v[(size_t)vi];
    // Catmull-Rom interpolation between prev[1] and prev[2].
    const float t = (float)(v.counter & 0xFFF) / 4096.0f;
    const float p0 = v.prev[0], p1 = v.prev[1], p2 = v.prev[2], p3 = v.prev[3];
    const float out = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
    int32_t sample = clamp16((int32_t)out);
    // Pitch (with optional modulation by the previous voice's output).
    uint32_t pitch = std::min<uint32_t>(v.pitch, 0x3FFF);
    if (vi > 0 && (c.pmon & (1u << vi))) {
        const int32_t mod = c.v[(size_t)vi - 1].lastOut;
        pitch = (uint32_t)std::clamp<int64_t>((int64_t)pitch * (32768 + mod) >> 15, 0, 0x3FFF);
    }
    v.counter += pitch;
    while (v.counter >= 0x1000) {
        v.counter -= 0x1000;
        v.prev[0] = v.prev[1];
        v.prev[1] = v.prev[2];
        v.prev[2] = v.prev[3];
        v.prev[3] = decodeNext(c, v, ci, vi);
        if (!v.env.phase) break;
    }
    stepEnvelope(v);
    const int32_t s = applyVol(sample, v.env.level);
    v.lastOut = s;
    return s;
}

// ------------------------------------------------------------------ reverb (SPU algorithm, 24 kHz)

void Spu2::reverb(int ci, int32_t inL, int32_t inR, int32_t& outL, int32_t& outR) {
    SpuCore& c = cores_[ci];
    const auto& R = c.reverb;
    auto addr32 = [&](int idx) -> uint32_t { return (((uint32_t)R[(size_t)idx] << 16) | R[(size_t)idx + 1]) & 0xFFFFF; };
    const uint32_t start = c.esa, end = std::max(c.eea, start + 1);
    const uint32_t size = end - start + 1;
    auto at = [&](uint32_t off) -> uint32_t { return start + ((reverbPos_[ci] + off) % size); };
    auto rd = [&](uint32_t off) -> int32_t { return (int16_t)ramw(at(off)); };
    auto wr = [&](uint32_t off, int32_t v) { ramw(at(off)) = (uint16_t)clamp16(v); };
    const uint32_t dAPF1 = addr32(0), dAPF2 = addr32(2);
    const uint32_t mLSAME = addr32(4), mRSAME = addr32(6);
    const uint32_t mLCOMB1 = addr32(8), mRCOMB1 = addr32(10), mLCOMB2 = addr32(12), mRCOMB2 = addr32(14);
    const uint32_t dLSAME = addr32(16), dRSAME = addr32(18);
    const uint32_t mLDIFF = addr32(20), mRDIFF = addr32(22);
    const uint32_t mLCOMB3 = addr32(24), mRCOMB3 = addr32(26), mLCOMB4 = addr32(28), mRCOMB4 = addr32(30);
    const uint32_t dLDIFF = addr32(32), dRDIFF = addr32(34);
    const uint32_t mLAPF1 = addr32(36), mRAPF1 = addr32(38), mLAPF2 = addr32(40), mRAPF2 = addr32(42);
    const int32_t vIIR = (int16_t)R[0x30], vCOMB1 = (int16_t)R[0x31], vCOMB2 = (int16_t)R[0x32], vCOMB3 = (int16_t)R[0x33],
                  vCOMB4 = (int16_t)R[0x34], vWALL = (int16_t)R[0x35], vAPF1 = (int16_t)R[0x36], vAPF2 = (int16_t)R[0x37],
                  vLIN = (int16_t)R[0x38], vRIN = (int16_t)R[0x39];
    const int32_t Lin = applyVol(inL, vLIN), Rin = applyVol(inR, vRIN);
    auto iir = [&](uint32_t dst, uint32_t src, int32_t input) {
        const int32_t prevv = rd(dst - 1);
        wr(dst, prevv + applyVol(input + applyVol(rd(src), vWALL) - prevv, vIIR));
    };
    iir(mLSAME, dLSAME, Lin);
    iir(mRSAME, dRSAME, Rin);
    iir(mLDIFF, dRDIFF, Lin);
    iir(mRDIFF, dLDIFF, Rin);
    int32_t L = applyVol(rd(mLCOMB1), vCOMB1) + applyVol(rd(mLCOMB2), vCOMB2) + applyVol(rd(mLCOMB3), vCOMB3) + applyVol(rd(mLCOMB4), vCOMB4);
    int32_t Rr = applyVol(rd(mRCOMB1), vCOMB1) + applyVol(rd(mRCOMB2), vCOMB2) + applyVol(rd(mRCOMB3), vCOMB3) + applyVol(rd(mRCOMB4), vCOMB4);
    auto apf = [&](int32_t x, uint32_t m, uint32_t d, int32_t k) {
        const int32_t delayed = rd(m - d);
        const int32_t y = x - applyVol(delayed, k);
        wr(m, y);
        return applyVol(y, k) + delayed;
    };
    L = apf(L, mLAPF1, dAPF1, vAPF1);
    Rr = apf(Rr, mRAPF1, dAPF1, vAPF1);
    L = apf(L, mLAPF2, dAPF2, vAPF2);
    Rr = apf(Rr, mRAPF2, dAPF2, vAPF2);
    outL = clamp16(L);
    outR = clamp16(Rr);
    reverbPos_[ci] = (reverbPos_[ci] + 1) % size;
}

// ------------------------------------------------------------------ PCM input (ADMA)

bool Spu2::readAdma(SpuCore& c, int32_t& l, int32_t& r) {
    if (!c.adma || c.admaHalf == 0) return false;
    const uint32_t framesPerHalf = c.admaHalf / 4;
    const uint32_t chunk = c.admaFrame / 256, j = c.admaFrame % 256;
    const uint32_t base = c.admaIop + c.admaBuff * c.admaHalf + chunk * 1024;
    int16_t sl, sr;
    std::memcpy(&sl, m_.iopRam + ((base + j * 2) & (IOP_RAM_SIZE - 1)), 2);
    std::memcpy(&sr, m_.iopRam + ((base + 512 + j * 2) & (IOP_RAM_SIZE - 1)), 2);
    l = sl;
    r = sr;
    if (++c.admaFrame >= framesPerHalf) {
        c.admaFrame = 0;
        if (c.admaLoop) {
            c.admaBuff ^= 1;
        } else {
            c.adma = false;
            c.admas = 0;
        }
    }
    return true;
}

// ------------------------------------------------------------------ mixer

void Spu2::mix(int16_t* out, size_t frames) {
    for (size_t f = 0; f < frames; ++f) {
        int32_t ext[2] = {0, 0};
        int32_t finalOut[2] = {0, 0};
        reverbHalf_ = !reverbHalf_;
        for (int ci = 0; ci < 2; ++ci) {
            SpuCore& c = cores_[(size_t)ci];
            int32_t dryL = 0, dryR = 0, wetL = 0, wetR = 0;
            for (int vi = 0; vi < 24; ++vi) {
                SpuVoice& v = c.v[(size_t)vi];
                if (!v.env.phase) {
                    v.lastOut = 0;
                    continue;
                }
                const int32_t s = voiceSample(c, ci, vi);
                volStep(v.volL, v.curVolL, v.volCntL);
                volStep(v.volR, v.curVolR, v.volCntR);
                const int32_t l = applyVol(s, v.curVolL), r = applyVol(s, v.curVolR);
                const uint32_t bit = 1u << vi;
                if (c.vmixl & bit) dryL += l;
                if (c.vmixr & bit) dryR += r;
                if (c.vmixel & bit) wetL += l;
                if (c.vmixer & bit) wetR += r;
            }
            dryL = clamp16(dryL);
            dryR = clamp16(dryR);
            wetL = clamp16(wetL);
            wetR = clamp16(wetR);
            const uint16_t mm = c.mmix;
            int32_t tdL = 0, tdR = 0, twL = 0, twR = 0;
            // voices
            if (mm & 0x800) tdL += dryL;
            if (mm & 0x400) tdR += dryR;
            if (mm & 0x200) twL += wetL;
            if (mm & 0x100) twR += wetR;
            // PCM input (sound data in), scaled by AVOL
            int32_t inL = 0, inR = 0;
            if (readAdma(c, inL, inR)) {
                inL = applyVol(inL, c.avolL);
                inR = applyVol(inR, c.avolR);
                if (mm & 0x80) tdL += inL;
                if (mm & 0x40) tdR += inR;
                if (mm & 0x20) twL += inL;
                if (mm & 0x10) twR += inR;
            }
            // external input (core 0 output into core 1), scaled by BVOL
            if (ci == 1) {
                const int32_t eL = applyVol(ext[0], c.bvolL), eR = applyVol(ext[1], c.bvolR);
                if (mm & 0x8) tdL += eL;
                if (mm & 0x4) tdR += eR;
                if (mm & 0x2) twL += eL;
                if (mm & 0x1) twR += eR;
            }
            int32_t rvL = 0, rvR = 0;
            if (c.attr & 0x80) {
                static int32_t lastL[2], lastR[2];
                if (reverbHalf_) reverb(ci, twL, twR, lastL[ci], lastR[ci]);
                rvL = applyVol(lastL[ci], c.evolL);
                rvR = applyVol(lastR[ci], c.evolR);
            }
            int32_t oL = clamp16(tdL + rvL), oR = clamp16(tdR + rvR);
            volStep(c.mvolL, c.curMvolL, c.mvolCntL);
            volStep(c.mvolR, c.curMvolR, c.mvolCntR);
            oL = applyVol(oL, c.curMvolL);
            oR = applyVol(oR, c.curMvolR);
            if (c.attr & 0x4000) oL = oR = 0; // core muted
            if (!(c.attr & 0x8000)) oL = oR = 0; // core disabled
            if (ci == 0) {
                ext[0] = oL;
                ext[1] = oR;
            } else {
                finalOut[0] = oL;
                finalOut[1] = oR;
            }
        }
        out[f * 2] = (int16_t)clamp16((int32_t)(finalOut[0] * masterVolume));
        out[f * 2 + 1] = (int16_t)clamp16((int32_t)(finalOut[1] * masterVolume));
    }
}

void Spu2::advanceField(double fieldRate) {
    fieldFrac_ += 48000.0 / fieldRate;
    const size_t n = (size_t)fieldFrac_;
    fieldFrac_ -= (double)n;
    generate(n);
}

void Spu2::generate(size_t frames) {
    std::vector<int16_t> buf(frames * 2);
    mix(buf.data(), frames);
    if (onSamples) onSamples(buf.data(), frames);
    if (!enabled) return;
    std::lock_guard<std::mutex> lk(queueMutex_);
    queue_.insert(queue_.end(), buf.begin(), buf.end());
    const size_t maxQueued = 48000; // 0.5 s of stereo samples
    while (queue_.size() > maxQueued * 2) queue_.pop_front();
}

size_t Spu2::pull(int16_t* out, size_t frames) {
    std::lock_guard<std::mutex> lk(queueMutex_);
    const size_t n = std::min(frames * 2, queue_.size());
    std::copy(queue_.begin(), queue_.begin() + (ptrdiff_t)n, out);
    queue_.erase(queue_.begin(), queue_.begin() + (ptrdiff_t)n);
    if (n < frames * 2) std::memset(out + n, 0, (frames * 2 - n) * sizeof(int16_t));
    return n / 2;
}

size_t Spu2::queuedFrames() {
    std::lock_guard<std::mutex> lk(queueMutex_);
    return queue_.size() / 2;
}

void Spu2::trim(size_t maxFrames) {
    std::lock_guard<std::mutex> lk(queueMutex_);
    while (queue_.size() > maxFrames * 2) queue_.pop_front();
}

int Spu2::activeVoices() const {
    int n = 0;
    for (auto& c : cores_)
        for (auto& v : c.v) n += v.env.phase ? 1 : 0;
    return n;
}

// ------------------------------------------------------------------ libsd

namespace {
uint32_t voiceReg(int core, int voice, int param) { return (uint32_t)core * 0x400 + (uint32_t)voice * 0x10 + (uint32_t)param * 2; }
uint32_t voiceAddrReg(int core, int voice, int which) { return (uint32_t)core * 0x400 + 0x1C0 + (uint32_t)voice * 0xC + (uint32_t)which * 4; }
uint32_t generalReg(int core, int idx) { return 0x760 + (uint32_t)core * 0x28 + (uint32_t)idx * 2; }
} // namespace

int Spu2::sdInit(int flag) {
    for (int c = 0; c < 2; ++c) {
        for (int v = 0; v < 24; ++v) {
            cores_[(size_t)c].v[(size_t)v] = SpuVoice{};
        }
        SpuCore& cc = cores_[(size_t)c];
        cc.attr = (uint16_t)(0x8000 | 0x4000 | (flag ? 0 : 0) | (c == 1 ? 1 : 0)); // on + muted (libsd default)
        cc.vmixl = cc.vmixr = cc.vmixel = cc.vmixer = 0xFFFFFF;
        cc.mmix = c == 0 ? 0xFF0 : 0xFFC;
        cc.mvolL = cc.mvolR = 0;
        cc.curMvolL = cc.curMvolR = 0;
        cc.evolL = cc.evolR = 0;
        cc.avolL = cc.avolR = c == 1 ? 0x7FFF : 0;
        cc.bvolL = cc.bvolR = 0;
        cc.endx = 0;
        cc.adma = false;
        cc.admas = 0;
    }
    logInfo("spu2: sceSdInit(%d)", flag);
    return 0;
}

void Spu2::sdSetParam(uint16_t entry, uint16_t value) {
    const int core = entry & 1;
    const int id = (entry >> 8) & 0xFF;
    if (entry & 0x80) {
        static const int kIdx[] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        if (id >= 9 && id <= 0x12) write16(generalReg(core, kIdx[id]), value);
        return;
    }
    if (id == 0x08) {
        write16((uint32_t)core * 0x400 + 0x198, value); // MMIX
        return;
    }
    const int voice = (entry >> 1) & 0x1F;
    if (id <= 7 && voice < 24) write16(voiceReg(core, voice, id), value);
}

uint16_t Spu2::sdGetParam(uint16_t entry) {
    const int core = entry & 1;
    const int id = (entry >> 8) & 0xFF;
    if (entry & 0x80) {
        if (id >= 9 && id <= 0x12) return read16(generalReg(core, id - 9));
        return 0;
    }
    if (id == 0x08) return read16((uint32_t)core * 0x400 + 0x198);
    const int voice = (entry >> 1) & 0x1F;
    if (id <= 7 && voice < 24) return read16(voiceReg(core, voice, id));
    return 0;
}

void Spu2::sdSetSwitch(uint16_t entry, uint32_t value) {
    const int core = entry & 1;
    const int id = (entry >> 8) & 0xFF;
    static const uint16_t kReg[] = {0x180, 0x184, 0x1A0, 0x1A4, 0x340, 0x188, 0x18C, 0x190, 0x194};
    if (id < 0x13 || id > 0x1B) return;
    const uint32_t base = (uint32_t)core * 0x400 + kReg[id - 0x13];
    write16(base, (uint16_t)value);
    write16(base + 2, (uint16_t)((value >> 16) & 0xFF));
}

uint32_t Spu2::sdGetSwitch(uint16_t entry) {
    const int core = entry & 1;
    const int id = (entry >> 8) & 0xFF;
    const SpuCore& c = cores_[(size_t)core];
    switch (id) {
    case 0x13: return c.pmon;
    case 0x14: return c.non;
    case 0x17: return c.endx;
    case 0x18: return c.vmixl;
    case 0x19: return c.vmixel;
    case 0x1A: return c.vmixr;
    case 0x1B: return c.vmixer;
    default: return 0;
    }
}

void Spu2::sdSetAddr(uint16_t entry, uint32_t byteAddr) {
    const int core = entry & 1;
    const int id = (entry >> 8) & 0xFF;
    const uint32_t w = (byteAddr >> 1) & 0xFFFFF;
    SpuCore& c = cores_[(size_t)core];
    switch (id) {
    case 0x1C: c.esa = w; break;                 // ESA
    case 0x1D: c.eea = (w | 0x1FFFF) & 0xFFFFF; break; // EEA (top bits)
    case 0x1E: c.tsa = w; break;                 // TSA
    case 0x1F: c.irqa = w; break;                // IRQA
    case 0x20: case 0x21: case 0x22: {
        const int voice = (entry >> 1) & 0x1F;
        if (voice >= 24) return;
        const uint32_t reg = voiceAddrReg(core, voice, id - 0x20);
        write16(reg, (uint16_t)(w >> 16));
        write16(reg + 2, (uint16_t)w);
        break;
    }
    default: break;
    }
}

uint32_t Spu2::sdGetAddr(uint16_t entry) {
    const int core = entry & 1;
    const int id = (entry >> 8) & 0xFF;
    const SpuCore& c = cores_[(size_t)core];
    const int voice = (entry >> 1) & 0x1F;
    switch (id) {
    case 0x1C: return c.esa << 1;
    case 0x1D: return c.eea << 1;
    case 0x1E: return c.tsa << 1;
    case 0x1F: return c.irqa << 1;
    case 0x20: return voice < 24 ? c.v[(size_t)voice].ssa << 1 : 0;
    case 0x21: return voice < 24 ? c.v[(size_t)voice].lsa << 1 : 0;
    case 0x22: return voice < 24 ? c.v[(size_t)voice].nax << 1 : 0;
    default: return 0;
    }
}

void Spu2::sdSetCoreAttr(uint16_t entry, uint16_t value) {
    SpuCore& c = cores_[(size_t)(entry & 1)];
    switch (entry & ~1) {
    case 0x2: c.attr = (uint16_t)((c.attr & ~0x80) | (value ? 0x80 : 0)); break; // effect enable
    case 0x4: c.attr = (uint16_t)((c.attr & ~0x40) | (value ? 0x40 : 0)); break; // IRQ enable
    case 0x6: c.attr = (uint16_t)((c.attr & ~0x4000) | (value ? 0x4000 : 0)); break; // mute
    case 0x8: c.attr = (uint16_t)((c.attr & ~0x3F00) | ((value & 0x3F) << 8)); break; // noise clock
    case 0xA: spdifOut_ = value; break;
    default: break;
    }
}

uint16_t Spu2::sdGetCoreAttr(uint16_t entry) {
    const SpuCore& c = cores_[(size_t)(entry & 1)];
    switch (entry & ~1) {
    case 0x2: return (c.attr >> 7) & 1;
    case 0x4: return (c.attr >> 6) & 1;
    case 0x6: return (c.attr >> 14) & 1;
    case 0x8: return (c.attr >> 8) & 0x3F;
    case 0xA: return spdifOut_;
    default: return 0;
    }
}

int Spu2::sdVoiceTrans(int chan, int mode, uint32_t iopAddr, uint32_t spuByteAddr, uint32_t size) {
    (void)chan;
    const uint32_t ia = iopAddr & (IOP_RAM_SIZE - 1);
    const uint32_t words = size / 2;
    const uint32_t start = (spuByteAddr >> 1) & 0xFFFFF;
    if ((mode & 3) == 1) { // read SPU -> IOP
        for (uint32_t i = 0; i < words; ++i) {
            const uint16_t w = ramw(start + i);
            std::memcpy(m_.iopRam + ((ia + i * 2) & (IOP_RAM_SIZE - 1)), &w, 2);
        }
    } else {
        for (uint32_t i = 0; i < words; ++i) {
            uint16_t w;
            std::memcpy(&w, m_.iopRam + ((ia + i * 2) & (IOP_RAM_SIZE - 1)), 2);
            ramw(start + i) = w;
        }
    }
    PS2_DEBUG(Spu, "VoiceTrans ch%d mode=%x iop=%06x spu=%06x size=%u", chan, mode, iopAddr, spuByteAddr, size);
    return (int)size;
}

int Spu2::sdBlockTrans(int chan, int mode, uint32_t iopAddr, uint32_t size, uint32_t startAddr) {
    SpuCore& c = cores_[(size_t)(chan & 1)];
    const int dir = mode & 3;
    if (dir == 2) { // stop
        const uint32_t pos = c.admaIop + c.admaBuff * c.admaHalf + c.admaFrame * 4;
        c.adma = false;
        c.admas = 0;
        return (int)pos;
    }
    if (dir == 1) return 0; // reading the core output back is not supported
    c.admaLoop = (mode & 0x10) != 0;
    c.admaIop = iopAddr & (IOP_RAM_SIZE - 1);
    c.admaHalf = c.admaLoop ? size / 2 : size;
    c.admaBuff = 0;
    c.admaFrame = 0;
    if (dir == 3 && startAddr) { // write from a start address
        uint32_t off = (startAddr & (IOP_RAM_SIZE - 1)) - c.admaIop;
        if (off >= c.admaHalf && c.admaLoop) {
            off -= c.admaHalf;
            c.admaBuff = 1;
        }
        c.admaFrame = std::min(off / 4, c.admaHalf / 4);
    }
    c.adma = c.admaHalf >= 1024;
    c.admas = (uint16_t)(1 << (chan & 1));
    PS2_DEBUG(Spu, "BlockTrans core%d mode=%x iop=%06x size=%u", chan & 1, mode, iopAddr, size);
    return 0;
}

uint32_t Spu2::sdBlockTransStatus(int chan) {
    const SpuCore& c = cores_[(size_t)(chan & 1)];
    const uint32_t addr = c.adma ? (c.admaIop + c.admaBuff * c.admaHalf + c.admaFrame * 4) : 0;
    return (c.admaBuff << 24) | (addr & 0xFFFFFF);
}

int Spu2::sdProcBatch(const uint8_t* batch, int count, std::vector<uint32_t>* returns) {
    int done = 0;
    for (int i = 0; i < count; ++i) {
        uint16_t func, entry;
        uint32_t value;
        std::memcpy(&func, batch + i * 8, 2);
        std::memcpy(&entry, batch + i * 8 + 2, 2);
        std::memcpy(&value, batch + i * 8 + 4, 4);
        uint32_t ret = 0;
        switch (func) {
        case 0x01: sdSetParam(entry, (uint16_t)value); break;
        case 0x02: sdSetSwitch(entry, value); break;
        case 0x03: sdSetAddr(entry, value); break;
        case 0x04: sdSetCoreAttr(entry, (uint16_t)value); break;
        case 0x05: { // write IOP memory
            const uint32_t a = value & (IOP_RAM_SIZE - 1);
            std::memcpy(m_.iopRam + (a & ~3u), &entry, 2);
            break;
        }
        case 0x06: m_.write32(value, entry); break; // write EE memory
        case 0x10: ret = sdGetParam(entry); break;
        case 0x12: ret = sdGetSwitch(entry); break;
        case 0x13: ret = sdGetAddr(entry); break;
        case 0x14: ret = sdGetCoreAttr(entry); break;
        default: break;
        }
        if (returns) returns->push_back(ret);
        ++done;
    }
    return done;
}

int Spu2::sdSetEffectAttr(int core, const std::vector<uint8_t>& attr) {
    // sceSdEffectAttr {int core; int mode; short depth_L, depth_R; int delay; int feedback;}
    static const uint16_t kSizes[10] = {0x1, 0x26C, 0x1F4, 0x484, 0x6FE, 0xADE, 0xF6C, 0x1804, 0x1804, 0x3C0};
    // Preset register values (same as the PS1 SPU presets, as used by libsd).
    static const uint32_t kParams[10][16] = {
        {0, 0, 0, 0, 0, 0x00010001, 0, 0, 0, 0x00010001, 0, 0, 0, 0, 0, 0},
        {0x005B007D, 0x54B86D80, 0x0000BED0, 0xBA800000, 0x53005800, 0x033304D6, 0x022703F0, 0x01EF0374, 0x01B70336, 0x01B60335,
         0x01B50334, 0x01B50334, 0x01B50334, 0x013601B4, 0x005C00B8, 0x80008000},
        {0x00250033, 0x4FA870F0, 0x4410BCE0, 0x9C00C0F0, 0x4EC05280, 0x031B03E4, 0x02AF03A4, 0x02660372, 0x025D031C, 0x018E025C,
         0x0135022F, 0x00B701D2, 0x00B5018F, 0x008000B4, 0x0026004C, 0x80008000},
        {0x007F00B1, 0x4FA870F0, 0x4510BCE0, 0xB4C0BEF0, 0x4EC05280, 0x076B0904, 0x065F0824, 0x061607A2, 0x05ED076C, 0x042E05EC,
         0x0305050F, 0x02B70462, 0x0265042F, 0x01B20264, 0x00800100, 0x80008000},
        {0x00A900E3, 0x4FA86F60, 0x4510BCE0, 0xA680BEF0, 0x52C05680, 0x0B580DFB, 0x0A3C0D09, 0x09730BD9, 0x08DA0B59, 0x05E908D9,
         0x04B007EC, 0x03D206EF, 0x031D05EA, 0x0238031C, 0x00AA0154, 0x80008000},
        {0x013901A5, 0x50006000, 0xB8004C00, 0xC000BC00, 0x5C006000, 0x11BB15BA, 0x10BD14C2, 0x0DC111BC, 0x0DC311C0, 0x09C10DC0,
         0x07C10BC4, 0x06CD0A00, 0x05C109C2, 0x041A05C0, 0x013A0274, 0x80008000},
        {0x0231033D, 0x50007E00, 0xB000B400, 0xB0004C00, 0x54006000, 0x1A311ED6, 0x183B1D14, 0x16B21BC2, 0x15EF1A32, 0x105515EE,
         0x0F2D1334, 0x0C5D11F6, 0x0AE11056, 0x07A20AE0, 0x02320464, 0x80008000},
        {0x00030003, 0x7FFF7FFF, 0x00000000, 0x81000000, 0x00000000, 0x0FFD1FFD, 0x00091009, 0x00000000, 0x00091009, 0x1FFF1FFF,
         0x1FFE1FFE, 0x1FFE1FFE, 0x1FFE1FFE, 0x10041008, 0x00040008, 0x80008000},
        {0x00030003, 0x7FFF7FFF, 0x00000000, 0x00000000, 0x00000000, 0x0FFD1FFD, 0x00091009, 0x00000000, 0x00091009, 0x1FFF1FFF,
         0x1FFE1FFE, 0x1FFE1FFE, 0x1FFE1FFE, 0x10041008, 0x00040008, 0x80008000},
        {0x00130017, 0x4FA870F0, 0x4510BCE0, 0x8500BEF0, 0x54C05F80, 0x02AF0371, 0x01DF02E5, 0x01D702B0, 0x026A0358, 0x011E01D6,
         0x00B1012D, 0x0059011F, 0x00E301A0, 0x00400058, 0x00140028, 0x80008000},
    };
    auto r32 = [&](size_t o) -> int32_t {
        int32_t v = 0;
        if (o + 4 <= attr.size()) std::memcpy(&v, attr.data() + o, 4);
        return v;
    };
    auto r16 = [&](size_t o) -> int16_t {
        int16_t v = 0;
        if (o + 2 <= attr.size()) std::memcpy(&v, attr.data() + o, 2);
        return v;
    };
    core &= 1;
    uint32_t mode = (uint32_t)r32(4) & 0xFF;
    if (mode > 9) return -1;
    SpuCore& c = cores_[(size_t)core];
    uint16_t d[32];
    std::memcpy(d, kParams[mode], 64);
    auto set32 = [&](int idx, uint32_t v) {
        const uint32_t w = v << 2; // 8-byte units -> words
        c.reverb[(size_t)idx] = (uint16_t)(w >> 16);
        c.reverb[(size_t)idx + 1] = (uint16_t)w;
    };
    set32(0, d[0]);
    set32(2, d[1]);
    for (int i = 0; i < 8; ++i) c.reverb[(size_t)(0x30 + i)] = d[2 + i];
    for (int i = 0; i < 20; ++i) set32(4 + i * 2, d[10 + i]);
    c.reverb[0x38] = d[30];
    c.reverb[0x39] = d[31];
    if (mode == 7 || mode == 8) {
        const int32_t delay = r32(12), feedback = r32(16);
        (void)feedback;
        if (delay > 0) {
            const uint32_t dly = (uint32_t)std::min(127, delay) << 7;
            set32(16, dly);
            set32(18, dly);
        }
    }
    const uint32_t size = (uint32_t)kSizes[mode] << 3; // words
    c.esa = (c.eea - size + 1) & 0xFFFFF;
    c.evolL = r16(8);
    c.evolR = r16(10);
    reverbPos_[(size_t)core] = 0;
    return 0;
}

uint16_t Spu2::note2Pitch(uint16_t centerNote, uint16_t centerFine, uint16_t note, int16_t fine) {
    const double semis = ((double)note - centerNote) + ((double)fine - centerFine) / 128.0;
    const double p = 4096.0 * std::pow(2.0, semis / 12.0);
    return (uint16_t)std::clamp(p, 0.0, 16383.0);
}

uint16_t Spu2::pitch2Note(uint16_t centerNote, uint16_t centerFine, uint16_t pitch) {
    if (!pitch) return 0;
    const double semis = 12.0 * std::log2(pitch / 4096.0);
    const double total = centerNote + centerFine / 128.0 + semis;
    const int n = (int)std::floor(total);
    const int f = (int)std::lround((total - n) * 128.0);
    return (uint16_t)((n << 8) | (f & 0x7F));
}

int32_t Spu2::sdRemote(uint32_t cmd, uint32_t sub, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, const std::vector<uint8_t>& raw) {
    auto w = [&](size_t i) -> uint32_t {
        uint32_t v = 0;
        if (i * 4 + 4 <= raw.size()) std::memcpy(&v, raw.data() + i * 4, 4);
        return v;
    };
    switch (cmd) {
    case 0x8000: return sdInit((int)a1);
    case 0x8010: sdSetParam((uint16_t)a1, (uint16_t)a2); return 0;
    case 0x8020: return sdGetParam((uint16_t)a1);
    case 0x8030: sdSetSwitch((uint16_t)a1, a2); return 0;
    case 0x8040: return (int32_t)sdGetSwitch((uint16_t)a1);
    case 0x8050: sdSetAddr((uint16_t)a1, a2); return 0;
    case 0x8060: return (int32_t)sdGetAddr((uint16_t)a1);
    case 0x8070: sdSetCoreAttr((uint16_t)a1, (uint16_t)a2); return 0;
    case 0x8080: return sdGetCoreAttr((uint16_t)a1);
    case 0x8090: return note2Pitch((uint16_t)a1, (uint16_t)a2, (uint16_t)a3, (int16_t)a4);
    case 0x80A0: return pitch2Note((uint16_t)a1, (uint16_t)a2, (uint16_t)a3);
    case 0x80B0:   // ProcBatch(batch in IOP memory, returns in IOP memory, count)
    case 0x80C0: { // ProcBatchEx
        const uint32_t b = a1 & (IOP_RAM_SIZE - 1), rets = a2 & (IOP_RAM_SIZE - 1);
        const int count = (int)std::min<uint32_t>(a3, 512);
        std::vector<uint32_t> out;
        const int n = sdProcBatch(m_.iopRam + b, count, a2 ? &out : nullptr);
        for (size_t i = 0; i < out.size(); ++i) std::memcpy(m_.iopRam + ((rets + i * 4) & (IOP_RAM_SIZE - 1)), &out[i], 4);
        return n;
    }
    case 0x81C0:   // ProcBatch2: batch inside the packet (entry 0 holds the count)
    case 0x81D0: {
        const int count = (int)(raw.size() >= 4 ? (uint16_t)(raw[2] | (raw[3] << 8)) : 0);
        if (raw.size() < 8 + (size_t)count * 8) return 0;
        return sdProcBatch(raw.data() + 8, count, nullptr);
    }
    case 0x80D0: return sdVoiceTrans((int)(a1 & 0xFFFF), (int)(a2 & 0xFFFF), a3, a4, w(5));
    case 0x80E0: return sdBlockTrans((int)(a1 & 0xFFFF), (int)(a2 & 0xFFFF), a3, a4, w(5));
    case 0x80F0: return 1; // VoiceTransStatus: transfers complete immediately
    case 0x8100: return (int32_t)sdBlockTransStatus((int)(a1 & 0xFFFF));
    case 0x8110: case 0x8120: case 0x8160: case 0x8170: return 0; // callbacks / handlers
    case 0x8130: case 0x81A0: case 0x81B0: return sdSetEffectAttr((int)sub, raw);
    case 0x8140: return 0;
    case 0x8150: case 0x8190: { // Clear/CleanEffectWorkArea(core, channel, mode)
        const SpuCore& c = cores_[(size_t)(a1 & 1)];
        for (uint32_t i = c.esa; i <= c.eea && i < kRamWords; ++i) ramw(i) = 0;
        return 0;
    }
    case 0x8180: return sdBlockTrans((int)a1, 2, 0, 0, 0); // StopTrans
    case 0x8F10: return 0;                                 // ChangeThreadPriority
    default:
        if (cmd >= 0x9000 && cmd <= 0x90F0) return 0; // user commands (game specific IOP code)
        logWarn("spu2: unsupported libsd remote command %04x", cmd);
        return -1;
    }
}

} // namespace ps2
