#include "ps2/spu2.hpp"

#include <algorithm>
#include <cstring>

#include "ps2/log.hpp"
#include "ps2/machine.hpp"

namespace ps2 {

namespace {
constexpr int32_t kAdpcmF0[5] = {0, 60, 115, 98, 122};
constexpr int32_t kAdpcmF1[5] = {0, 0, -52, -55, -60};
constexpr uint32_t kRamWords = 1024 * 1024; // 2 MB of 16-bit words
} // namespace

Spu2::Spu2(Machine& m) : m_(m), ram_(kRamWords, 0) {}

uint16_t Spu2::read16(uint32_t addr) { return regs_[(addr >> 1) & 0x7FF]; }

// Register map (per core, core 1 at +0x400):
//   voice params: 0x000 + v*0x10: VOLL, VOLR, PITCH, ADSR1, ADSR2, ENVX, VOLXL, VOLXR
//   voice addresses: 0x1C0 + v*0xC: SSA hi/lo, LSAX hi/lo, NAX hi/lo
//   0x1A0 KON0, 0x1A2 KON1, 0x1A4 KOFF0, 0x1A6 KOFF1
void Spu2::write16(uint32_t addr, uint16_t v) {
    const uint32_t a = addr & 0xFFF;
    regs_[(a >> 1) & 0x7FF] = v;
    const int core = a >= 0x400 ? 1 : 0;
    const uint32_t ca = a & 0x3FF;
    if (ca < 0x180) {
        const int voice = (int)(ca >> 4);
        if (voice >= 24) return;
        SpuVoice& vo = voices_[core][voice];
        switch (ca & 0xF) {
        case 0x0: vo.volL = v; break;
        case 0x2: vo.volR = v; break;
        case 0x4: vo.pitch = v; break;
        case 0x6: vo.adsr1 = v; break;
        case 0x8: vo.adsr2 = v; break;
        default: break;
        }
        return;
    }
    if (ca >= 0x1C0 && ca < 0x1C0 + 24 * 0xC) {
        const int voice = (int)((ca - 0x1C0) / 0xC);
        const uint32_t off = (ca - 0x1C0) % 0xC;
        SpuVoice& vo = voices_[core][voice];
        auto setHi = [&](uint32_t& r) { r = (r & 0xFFFF) | ((uint32_t)(v & 0xF) << 16); };
        auto setLo = [&](uint32_t& r) { r = (r & 0xF0000) | v; };
        switch (off) {
        case 0x0: setHi(vo.ssa); break;
        case 0x2: setLo(vo.ssa); break;
        case 0x4: setHi(vo.lsax); vo.loopSet = true; break;
        case 0x6: setLo(vo.lsax); vo.loopSet = true; break;
        default: break;
        }
        return;
    }
    switch (ca) {
    case 0x1A0: for (int i = 0; i < 16; ++i) if (v & (1 << i)) keyOn(core, i); break;
    case 0x1A2: for (int i = 0; i < 8; ++i) if (v & (1 << i)) keyOn(core, 16 + i); break;
    case 0x1A4: for (int i = 0; i < 16; ++i) if (v & (1 << i)) keyOff(core, i); break;
    case 0x1A6: for (int i = 0; i < 8; ++i) if (v & (1 << i)) keyOff(core, 16 + i); break;
    case 0x1A8: transferAddr_[core] = (transferAddr_[core] & 0xFFFF) | ((uint32_t)(v & 0xF) << 16); break;
    case 0x1AA: transferAddr_[core] = (transferAddr_[core] & 0xF0000) | v; break;
    case 0x1AC: ram_[transferAddr_[core] & (kRamWords - 1)] = v; transferAddr_[core]++; break;
    default: break;
    }
}

void Spu2::dmaWrite(int core, const uint16_t* data, size_t count) {
    for (size_t i = 0; i < count; ++i) ram_[(transferAddr_[core & 1] + i) & (kRamWords - 1)] = data[i];
    transferAddr_[core & 1] += (uint32_t)count;
}

void Spu2::keyOn(int core, int voice) {
    SpuVoice& v = voices_[core][voice];
    v.nax = v.ssa;
    v.sampleIndex = 28;
    v.hist1 = v.hist2 = 0;
    v.counter = 0;
    v.envPhase = 1;
    v.envLevel = 0;
    v.keyOn = true;
    if (!v.loopSet) v.lsax = v.ssa;
}

void Spu2::keyOff(int core, int voice) {
    SpuVoice& v = voices_[core][voice];
    if (v.envPhase) v.envPhase = 4;
}

int16_t Spu2::nextSample(SpuVoice& v) {
    if (v.sampleIndex >= 28) {
        // Decode one 16-byte ADPCM block at NAX (in 16-bit words).
        const uint16_t* blk = &ram_[v.nax & (kRamWords - 1) & ~7u];
        const uint8_t header = (uint8_t)(blk[0] & 0xFF);
        const uint8_t flags = (uint8_t)(blk[0] >> 8);
        const int shift = header & 0xF;
        const int filter = std::min(4, (header >> 4) & 0xF);
        if (flags & 4) v.lsax = v.nax & ~7u; // loop start
        for (int i = 0; i < 28; ++i) {
            const uint16_t w = blk[1 + i / 4];
            int32_t s = (int16_t)(((w >> ((i & 3) * 4)) & 0xF) << 12) >> (shift > 12 ? 12 : shift);
            s += (v.hist1 * kAdpcmF0[filter] + v.hist2 * kAdpcmF1[filter]) >> 6;
            s = std::clamp(s, -32768, 32767);
            v.hist2 = v.hist1;
            v.hist1 = s;
            v.samples[i] = (int16_t)s;
        }
        v.sampleIndex = 0;
        if (flags & 1) { // loop end
            v.nax = v.lsax;
            if (!(flags & 2)) v.envPhase = 0; // end without repeat: voice stops
        } else {
            v.nax += 8;
        }
    }
    return v.samples[v.sampleIndex];
}

void Spu2::mix(int16_t* out, size_t frames) {
    for (size_t f = 0; f < frames; ++f) {
        int32_t l = 0, r = 0;
        for (auto& core : voices_) {
            for (auto& v : core) {
                if (!v.envPhase) continue;
                const int16_t s = nextSample(v);
                // Simplified envelope: linear attack, hold, linear release.
                switch (v.envPhase) {
                case 1: v.envLevel = std::min(0x7FFF, v.envLevel + 0x400); if (v.envLevel >= 0x7FFF) v.envPhase = 3; break;
                case 4: v.envLevel = std::max(0, v.envLevel - 0x80); if (v.envLevel == 0) v.envPhase = 0; break;
                default: break;
                }
                const int32_t val = (s * v.envLevel) >> 15;
                const int32_t vl = (v.volL & 0x8000) ? 0x3FFF : (v.volL & 0x7FFF);
                const int32_t vr = (v.volR & 0x8000) ? 0x3FFF : (v.volR & 0x7FFF);
                l += (val * vl) >> 15;
                r += (val * vr) >> 15;
                v.counter += v.pitch ? v.pitch : 0x1000;
                while (v.counter >= 0x1000) {
                    v.counter -= 0x1000;
                    v.sampleIndex++;
                    if (v.sampleIndex >= 28) break;
                }
            }
        }
        out[f * 2] = (int16_t)std::clamp((int32_t)(l * masterVolume), -32768, 32767);
        out[f * 2 + 1] = (int16_t)std::clamp((int32_t)(r * masterVolume), -32768, 32767);
    }
}

void Spu2::generate(size_t frames) {
    if (!enabled) return;
    std::vector<int16_t> buf(frames * 2);
    mix(buf.data(), frames);
    std::lock_guard<std::mutex> lk(queueMutex_);
    queue_.insert(queue_.end(), buf.begin(), buf.end());
    const size_t maxQueued = 48000 * 2 / 2; // 0.5 s
    while (queue_.size() > maxQueued) queue_.pop_front();
}

size_t Spu2::pull(int16_t* out, size_t frames) {
    std::lock_guard<std::mutex> lk(queueMutex_);
    const size_t n = std::min(frames * 2, queue_.size());
    std::copy(queue_.begin(), queue_.begin() + (ptrdiff_t)n, out);
    queue_.erase(queue_.begin(), queue_.begin() + (ptrdiff_t)n);
    if (n < frames * 2) std::memset(out + n, 0, (frames * 2 - n) * sizeof(int16_t));
    return n / 2;
}

} // namespace ps2

namespace ps2 {

size_t Spu2::queuedFrames() {
    std::lock_guard<std::mutex> lk(queueMutex_);
    return queue_.size() / 2;
}

void Spu2::trim(size_t maxFrames) {
    std::lock_guard<std::mutex> lk(queueMutex_);
    while (queue_.size() > maxFrames * 2) queue_.pop_front();
}

} // namespace ps2
