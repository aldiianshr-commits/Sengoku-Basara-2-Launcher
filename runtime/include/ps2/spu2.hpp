// SPU2 sound processor: 2 cores x 24 voices, 2 MB sound RAM, ADPCM decoding
// with pitch interpolation, ADSR envelopes, volume sweeps, PCM input (auto
// DMA streaming), core 0 -> core 1 routing, master volumes and the libsd
// API used by the HLE IOP (sceSdRemote via SDRDRV).
//
// Register layout (IOP 0x1F900000 + offset, core 1 at +0x400 for the first
// 1 KB, general volumes at 0x760 + core * 0x28) follows the hardware so that
// register-level and libsd-level access share one state.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace ps2 {

class Machine;

struct SpuEnvelope {
    int32_t level = 0;      // 0..0x7FFF
    int phase = 0;          // 0 off, 1 attack, 2 decay, 3 sustain, 4 release
    int32_t counter = 0;    // sample counter for slow rates
};

struct SpuVoice {
    // registers
    uint16_t volL = 0, volR = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
    uint32_t ssa = 0, lsa = 0, nax = 0; // word addresses
    // current volumes (after sweep)
    int32_t curVolL = 0, curVolR = 0;
    int32_t volCntL = 0, volCntR = 0; // sweep counters
    // decoder
    int32_t hist1 = 0, hist2 = 0;
    int16_t block[28] = {};
    int blockPos = 28;
    uint8_t blockFlags = 0;
    int16_t prev[4] = {}; // interpolation history (oldest first)
    uint32_t counter = 0; // 12.12 sample position fraction
    bool customLoop = false;
    SpuEnvelope env;
    int32_t lastOut = 0; // for pitch modulation of the next voice
};

struct SpuCore {
    std::array<SpuVoice, 24> v;
    uint32_t pmon = 0, non = 0, vmixl = 0, vmixel = 0, vmixr = 0, vmixer = 0, endx = 0;
    uint16_t mmix = 0, attr = 0;
    uint32_t irqa = 0, tsa = 0, esa = 0, eea = 0;
    uint16_t mvolL = 0, mvolR = 0;
    int32_t curMvolL = 0, curMvolR = 0;
    int32_t mvolCntL = 0, mvolCntR = 0;
    int16_t evolL = 0, evolR = 0, avolL = 0, avolR = 0, bvolL = 0, bvolR = 0;
    uint16_t statx = 0, admas = 0;
    std::array<uint16_t, 0x40> reverb{}; // 0x2E4.. reverb registers
    // PCM input (auto DMA / block transfer)
    bool adma = false, admaLoop = false;
    uint32_t admaIop = 0, admaHalf = 0, admaBuff = 0, admaFrame = 0;
    bool irqFlag = false;
};

class Spu2 {
public:
    explicit Spu2(Machine& m);
    // Register interface (offset from 0x1F900000)
    uint16_t read16(uint32_t addr);
    void write16(uint32_t addr, uint16_t v);
    // Sound RAM (1M 16-bit words)
    uint16_t* ram() { return ram_.data(); }
    void dmaWrite(int core, const uint16_t* data, size_t count);
    // Mix `frames` stereo frames at 48 kHz.
    void mix(int16_t* out, size_t frames);
    size_t pull(int16_t* out, size_t frames);
    void generate(size_t frames);
    size_t queuedFrames();
    void trim(size_t maxFrames);
    float masterVolume = 1.0f;
    bool enabled = true; // queue output for the host device (the SPU2 always runs)
    // Optional tap receiving every generated block (tests, recording).
    std::function<void(const int16_t*, size_t)> onSamples;
    // Advance the SPU2 by one video field (called from the vblank).
    void advanceField(double fieldRate);

    // ---- libsd (IOP sound library) semantics
    int32_t sdRemote(uint32_t cmd, uint32_t sub, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                     const std::vector<uint8_t>& raw);
    int sdInit(int flag);
    void sdSetParam(uint16_t entry, uint16_t value);
    uint16_t sdGetParam(uint16_t entry);
    void sdSetSwitch(uint16_t entry, uint32_t value);
    uint32_t sdGetSwitch(uint16_t entry);
    void sdSetAddr(uint16_t entry, uint32_t byteAddr);
    uint32_t sdGetAddr(uint16_t entry);
    void sdSetCoreAttr(uint16_t entry, uint16_t value);
    uint16_t sdGetCoreAttr(uint16_t entry);
    int sdVoiceTrans(int chan, int mode, uint32_t iopAddr, uint32_t spuByteAddr, uint32_t size);
    int sdBlockTrans(int chan, int mode, uint32_t iopAddr, uint32_t size, uint32_t startAddr);
    uint32_t sdBlockTransStatus(int chan);
    int sdProcBatch(const uint8_t* batch, int count, std::vector<uint32_t>* returns);
    int sdSetEffectAttr(int core, const std::vector<uint8_t>& attr);
    static uint16_t note2Pitch(uint16_t centerNote, uint16_t centerFine, uint16_t note, int16_t fine);
    static uint16_t pitch2Note(uint16_t centerNote, uint16_t centerFine, uint16_t pitch);

    int activeVoices() const;
    const SpuCore& core(int c) const { return cores_[c & 1]; }

private:
    void keyOn(int core, int voice);
    void keyOff(int core, int voice);
    int16_t decodeNext(SpuCore& c, SpuVoice& v, int coreIdx, int voiceIdx);
    int32_t voiceSample(SpuCore& c, int coreIdx, int voiceIdx);
    void stepEnvelope(SpuVoice& v);
    void reverb(int coreIdx, int32_t inL, int32_t inR, int32_t& outL, int32_t& outR);
    bool readAdma(SpuCore& c, int32_t& l, int32_t& r);
    uint16_t& ramw(uint32_t wordAddr) { return ram_[wordAddr & 0xFFFFF]; }

    Machine& m_;
    std::vector<uint16_t> ram_;
    std::array<SpuCore, 2> cores_;
    std::array<uint16_t, 0x800> regs_{}; // raw register shadow
    std::array<int, 2> reverbCounter_{};
    std::array<uint32_t, 2> reverbPos_{};
    bool reverbHalf_ = false;
    uint16_t spdifOut_ = 0;
    double fieldFrac_ = 0.0;
    std::mutex queueMutex_;
    std::deque<int16_t> queue_;
};

} // namespace ps2
