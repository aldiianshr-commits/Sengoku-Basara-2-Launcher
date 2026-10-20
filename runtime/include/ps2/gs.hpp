// Graphics Synthesizer: local memory, GIF packet processing, rasterizer and
// display output.  Rendering is done by a multithreaded software rasterizer
// into GS local memory (exactly like the real GS), and the display circuits
// are read back each vsync and handed to the host presenter (Direct3D 12 /
// Vulkan via SDL3) for scaling, filtering and presentation.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace ps2 {

class Machine;

struct GsVertex {
    int32_t x = 0, y = 0; // 12.4 fixed point (after XYOFFSET)
    uint32_t z = 0;
    uint8_t r = 0, g = 0, b = 0, a = 0;
    float q = 1.0f;
    float s = 0, t = 0;
    uint16_t u = 0, v = 0; // 10.4 fixed
    uint8_t fog = 0;
};

struct GsContextRegs {
    uint64_t xyoffset = 0, tex0 = 0, tex1 = 0, tex2 = 0, clamp = 0, miptbp1 = 0, miptbp2 = 0;
    uint64_t scissor = 0, alpha = 0, test = 0, fba = 0, frame = 0, zbuf = 0;
};

// A presented frame (RGBA8, row-major).
struct GsFrame {
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> pixels;
    uint64_t frameNumber = 0;
    bool interlaced = false;
};

struct GsStats {
    uint64_t primitives = 0, pixels = 0, vertices = 0, transfers = 0, gifPackets = 0;
};

class Gs {
public:
    explicit Gs(Machine& m);
    ~Gs();

    // Privileged registers (0x12000000..)
    uint64_t readPriv64(uint32_t phys);
    void writePriv64(uint32_t phys, uint64_t v);
    uint32_t readPriv32(uint32_t phys) { return (uint32_t)(readPriv64(phys & ~7u) >> ((phys & 4) * 8)); }
    void writePriv32(uint32_t phys, uint32_t v);

    // General register write (A+D, PACKED register, REGLIST)
    void writeReg(uint32_t reg, uint64_t value);
    // Packed-format register write (GIF PACKED mode, 128-bit data)
    void writePacked(uint32_t reg, uint64_t lo, uint64_t hi);
    // Image data (GIF IMAGE mode / HWREG)
    void writeImage(const uint8_t* data, size_t qwords);
    // Local -> host transfer (TRXDIR=1), returns qwords produced.
    size_t readImage(uint8_t* out, size_t maxQwords);

    // GIF paths.
    void gifPath1(const uint8_t* vuMem, uint32_t addr); // XGKICK from VU1 memory (qword address)
    size_t gifTransfer(const uint8_t* data, size_t qwords, int path); // returns qwords consumed

    // Video timing
    void startVideo();
    bool interlaced() const;
    uint32_t field() const { return field_; }

    // Display output: snapshot of the current displayed image.
    bool latestFrame(GsFrame& out);
    std::function<void(const GsFrame&)> onFrame; // called at each vsync (emulation thread)

    // Local memory (4 MB) and access helpers used by tests / dumping.
    uint8_t* vram() { return vram_.data(); }
    uint32_t readPixel(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y) const;
    void writePixel(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, uint32_t c);

    // Settings
    int rasterThreads = 0; // 0 = auto
    bool bilinearOverride = false;
    GsStats stats;

    // GIF tag state (exposed for VIF DIRECT/DIRECTHL partial packets)
    struct GifTagState {
        bool active = false;
        uint32_t nloop = 0, nreg = 0, flg = 0, eop = 0;
        uint64_t regs = 0;
        uint32_t regIndex = 0;
        bool prePrim = false;
    };

private:
    friend class GsRasterizer;
    void vblankStart();
    void vblankEnd();
    void kick(bool drawing);
    void drawPrimitive();
    void transferBegin();
    void buildFrame(GsFrame& f);
    void flushPending();
    size_t processTag(const uint8_t* data, size_t qwords, GifTagState& st, int path);

    Machine& m_;
    std::vector<uint8_t> vram_;
    // General registers
    uint64_t prim_ = 0, prmode_ = 0, prmodecont_ = 1, rgbaq_ = 0, st_ = 0, uv_ = 0, fog_ = 0;
    uint64_t texclut_ = 0, scanmsk_ = 0, texa_ = 0, fogcol_ = 0, dimx_ = 0, dthe_ = 0, colclamp_ = 1, pabe_ = 0;
    uint64_t bitbltbuf_ = 0, trxpos_ = 0, trxreg_ = 0, trxdir_ = 0, signal_ = 0, label_ = 0;
    GsContextRegs ctx_[2];
    float q_ = 1.0f;
    // Vertex queue
    GsVertex vq_[3];
    int vcount_ = 0;
    int stripIndex_ = 0;
    GsVertex fanFirst_;
    // Privileged registers
    uint64_t pmode_ = 0, smode1_ = 0, smode2_ = 0, srfsh_ = 0, synch1_ = 0, synch2_ = 0, syncv_ = 0;
    uint64_t dispfb_[2] = {}, display_[2] = {}, extbuf_ = 0, extdata_ = 0, extwrite_ = 0, bgcolor_ = 0;
    uint64_t csr_ = 0, imr_ = 0x7F00, busdir_ = 0, siglblid_ = 0;
    uint32_t field_ = 0;
    // CLUT
    std::array<uint16_t, 512> clut_{};
    uint64_t lastClutTex0_ = ~0ull;
    void loadClut(uint64_t tex0);
    // Transfer state
    struct Xfer {
        bool active = false;
        int dir = 0;
        uint32_t x = 0, y = 0, w = 0, h = 0, sx = 0, sy = 0, dx = 0, dy = 0;
        uint32_t psm = 0, bp = 0, bw = 0;
        std::vector<uint8_t> partial;
    } xfer_;
    void localToLocal();
    // GIF path state
    GifTagState path_[4];
    // Display
    std::mutex frameMutex_;
    GsFrame frame_;
    bool frameValid_ = false;
    std::unique_ptr<class GsRasterizer> raster_;
};

class Gif {
public:
    explicit Gif(Machine& m) : m_(m) {}
    uint32_t read(uint32_t phys);
    void write(uint32_t phys, uint32_t v);
    void writeFifo(const uint8_t* qword); // 0x10006000
    uint32_t ctrl = 0, mode = 0, stat = 0;

private:
    Machine& m_;
};

} // namespace ps2
