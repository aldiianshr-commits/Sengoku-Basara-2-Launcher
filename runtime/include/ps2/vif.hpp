// VIF0 / VIF1: command decoding, UNPACK to VU data memory, MPG uploads,
// micro program starts and the DIRECT path to the GIF.
#pragma once
#include <cstdint>
#include <vector>

namespace ps2 {

class Machine;

struct VifState {
    int index = 0;
    uint32_t stat = 0, fbrst = 0, err = 0, mark = 0, cycle = 0, mode = 0, num = 0, mask = 0, code = 0;
    uint32_t itops = 0, itop = 0, base = 0, ofst = 0, tops = 0, top = 0;
    uint32_t row[4] = {}, col[4] = {};
    bool dbf = false;
    bool maskPath3 = false;
    // Command in progress
    uint32_t cmd = 0;          // current VIFcode
    uint32_t wordsLeft = 0;    // data words still expected for the command
    std::vector<uint32_t> buf; // collected data words
    // UNPACK progress
    uint32_t unpackAddr = 0, unpackCount = 0, unpackDone = 0, cycleIndex = 0;
    uint32_t mpgAddr = 0;
    uint64_t commands = 0;
};

class Vif {
public:
    explicit Vif(Machine& m);
    uint32_t readReg(int vif, uint32_t off);
    void writeReg(int vif, uint32_t off, uint32_t v);
    // Feed a stream of 32-bit words (from DMA or FIFO).
    void feed(int vif, const uint32_t* words, size_t count);
    VifState& state(int i) { return v_[i & 1]; }

private:
    void startCommand(VifState& s, uint32_t code);
    void finishCommand(VifState& s);
    void unpackWords(VifState& s);
    void mscal(VifState& s, uint32_t addr);
    Machine& m_;
    VifState v_[2];
};

} // namespace ps2
