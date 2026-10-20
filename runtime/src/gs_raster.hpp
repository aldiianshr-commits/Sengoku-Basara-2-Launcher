// Software rasterizer implementing the GS pixel pipeline.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "ps2/gs.hpp"

namespace ps2 {

class GsRasterizer {
public:
    explicit GsRasterizer(Gs& gs) : gs_(gs) {}
    void draw(uint64_t prim, const GsVertex* v, int count);

private:
    struct Setup;
    struct Tex;
    void setup(uint64_t prim, Setup& s);
    void triangle(const Setup& s, const GsVertex& a, const GsVertex& b, const GsVertex& c);
    void sprite(const Setup& s, const GsVertex& a, const GsVertex& b);
    void line(const Setup& s, const GsVertex& a, const GsVertex& b);
    void point(const Setup& s, const GsVertex& a);
    struct Frag {
        int x, y;
        uint32_t z;
        int r, g, b, a;
        float u, v; // texel coordinates (already divided by q)
        int fog;
        float lod;
    };
    void shade(const Setup& s, Frag& f);
    uint32_t sampleTexture(const Setup& s, float u, float v, float lod);
    uint32_t fetchTexel(const Setup& s, int level, int x, int y);
    Gs& gs_;
};

} // namespace ps2
