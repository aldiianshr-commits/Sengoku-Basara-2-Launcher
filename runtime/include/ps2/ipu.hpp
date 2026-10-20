// IPU (Image Processing Unit): the EE's MPEG-1/MPEG-2 decoding coprocessor
// used by Sony's libmpeg (and ps2sdk's) to play PSS/MPEG movies.
//
// Implemented: bitstream FIFO fed by DMA channel 4 (or direct FIFO writes)
// with the hardware's bit pointer / FIFO counters, output FIFO drained by
// DMA channel 3, and the commands BCLR, IDEC, BDEC (VLC + inverse scan +
// inverse quantisation + IDCT), VDEC (MBAI / MBTYPE / MOTION / DMV), FDEC,
// SETIQ, SETVQ, CSC (YCbCr -> RGB32/RGB16 with dithering and alpha
// thresholds), PACK and SETTH.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace ps2 {

class Machine;
union Reg128;

class Ipu {
public:
    explicit Ipu(Machine& m);
    ~Ipu();
    uint64_t read64(uint32_t phys);
    void write64(uint32_t phys, uint64_t v);
    void readFifo(Reg128* out);        // 0x10007000 (output FIFO)
    void writeFifo(const Reg128* in);  // 0x10007010 (input FIFO)
    void dmaInKick();                  // channel 4 (to IPU) started
    void dmaOutKick();                 // channel 3 (from IPU) started

    // statistics
    uint64_t commands = 0, macroblocks = 0;

    struct Qw {
        uint8_t b[16];
    };

private:
    struct NeedData {};
    friend class IpuReader;
    void reset();
    void tryRun();
    bool pullInput();                  // fetch one qword from DMA ch4, false if none
    void pushOut(const uint8_t* data, size_t bytes);
    void drainOut();
    uint32_t bitsAvailable() const;
    uint32_t ctrlValue() const;
    // command implementations: return true when finished, throw NeedData to retry
    bool cmdIdec(class IpuReader& r);
    bool cmdBdec(class IpuReader& r);
    bool cmdVdec(class IpuReader& r);
    bool cmdFdec(class IpuReader& r);
    bool cmdSetIq(class IpuReader& r);
    bool cmdSetVq(class IpuReader& r);
    bool cmdCsc(class IpuReader& r);
    bool cmdPack(class IpuReader& r);
    bool decodeBlock(class IpuReader& r, int comp, bool intra, int16_t out[64]);
    void csc(const uint8_t* mb, uint8_t* out, bool rgb16, bool dither);

    Machine& m_;
    std::deque<Qw> in_;
    uint32_t bp_ = 0;      // bit pointer into in_.front()
    std::deque<Qw> spent_; // qwords consumed by the command in progress
    uint32_t cmdBp_ = 0;   // bit position where the command (re)starts
    std::deque<Qw> out_;
    uint32_t outPartialBytes_ = 0;
    std::array<uint8_t, 16> outPartial_{};
    // registers
    uint32_t cmd_ = 0;
    bool busy_ = false;
    uint32_t data_ = 0;    // IPU_CMD DATA
    uint32_t ctrl_ = 0;    // writable control bits (IDP, AS, IVF, QST, MP1, PCT)
    bool ecd_ = false, scd_ = false;
    uint32_t cbp_ = 0;
    // decoder state
    std::array<uint8_t, 64> iqIntra_{}, iqNonIntra_{}; // raster order
    std::array<uint16_t, 16> vqClut_{};
    uint32_t th0_ = 0, th1_ = 0;
    int dcPred_[3] = {128, 128, 128};
    // CSC / PACK progress (multi-macroblock commands)
    uint32_t mbRemaining_ = 0;
    bool chainEnd4_ = false; // channel 4 source chain reached its end tag
};

} // namespace ps2
