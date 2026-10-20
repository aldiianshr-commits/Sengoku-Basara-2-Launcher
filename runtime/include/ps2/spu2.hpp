// SPU2 sound processor: 2 cores x 24 voices, ADPCM decoding, ADSR envelopes
// and mixing to a 48 kHz stereo stream consumed by the host audio device.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace ps2 {

class Machine;

struct SpuVoice {
    uint16_t volL = 0, volR = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
    uint32_t ssa = 0, lsax = 0, nax = 0;
    bool keyOn = false;
    // decoder state
    int32_t hist1 = 0, hist2 = 0;
    int16_t samples[28] = {};
    int sampleIndex = 28;
    uint32_t counter = 0;
    uint32_t loopAddr = 0;
    bool loopSet = false;
    // envelope
    int32_t envLevel = 0;
    int envPhase = 0; // 0 off, 1 attack, 2 decay, 3 sustain, 4 release
};

class Spu2 {
public:
    explicit Spu2(Machine& m);
    // Register interface (IOP addresses 0x1F900000..)
    uint16_t read16(uint32_t addr);
    void write16(uint32_t addr, uint16_t v);
    // Sound RAM (2 MB)
    uint16_t* ram() { return ram_.data(); }
    void dmaWrite(int core, const uint16_t* data, size_t count);
    // Mix `frames` stereo frames at 48 kHz.
    void mix(int16_t* out, size_t frames);
    // Pull rendered audio for the host device.
    size_t pull(int16_t* out, size_t frames);
    void generate(size_t frames); // advance emulation by frames (called from emulation thread)
    size_t queuedFrames();        // rendered stereo frames waiting for the host device
    void trim(size_t maxFrames);  // drop the oldest audio beyond maxFrames (latency bound)
    float masterVolume = 1.0f;
    bool enabled = true;

private:
    void keyOn(int core, int voice);
    void keyOff(int core, int voice);
    int16_t nextSample(SpuVoice& v);
    Machine& m_;
    std::vector<uint16_t> ram_;
    std::array<std::array<SpuVoice, 24>, 2> voices_;
    std::array<uint16_t, 0x800> regs_{};
    std::mutex queueMutex_;
    std::deque<int16_t> queue_;
    uint32_t transferAddr_[2] = {};
};

} // namespace ps2
