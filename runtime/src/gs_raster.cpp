#include "gs_raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ps2/gs_mem.hpp"

namespace ps2 {

using namespace gsmem;

namespace {
inline uint32_t bits(uint64_t v, int lo, int n) { return (uint32_t)((v >> lo) & ((1ull << n) - 1)); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

struct GsRasterizer::Setup {
    uint64_t prim;
    bool iip, tme, fge, abe, fst;
    // frame / z
    uint32_t fbp, fbw, fpsm, fbmsk;
    uint32_t zbp, zpsm;
    bool zmsk;
    // test
    bool ate, date, datm, zte;
    uint32_t atst, aref, afail, ztst;
    // alpha
    uint32_t aA, aB, aC, aD, fix;
    bool pabe, fba, colclamp, dthe;
    int dimx[4][4];
    // scissor
    int scx0, scx1, scy0, scy1;
    // texture
    uint32_t tbp[7], tbw[7];
    uint32_t tpsm, tw, th, tcc, tfx;
    uint32_t cpsm, csa;
    uint32_t wms, wmt, minu, maxu, minv, maxv;
    bool magLinear, minLinear, mipNearest;
    uint32_t mxl, lcm, lK, lL;
    float kVal;
    uint32_t ta0, ta1;
    bool aem;
    uint32_t fogR, fogG, fogB;
    uint32_t scanmsk;
};

void GsRasterizer::setup(uint64_t prim, Setup& s) {
    const GsContextRegs& c = gs_.ctx_[(prim >> 9) & 1];
    s.prim = prim;
    s.iip = (prim >> 3) & 1;
    s.tme = (prim >> 4) & 1;
    s.fge = (prim >> 5) & 1;
    s.abe = (prim >> 6) & 1;
    s.fst = (prim >> 8) & 1;
    s.fbp = bits(c.frame, 0, 9) * 32;
    s.fbw = bits(c.frame, 16, 6);
    s.fpsm = bits(c.frame, 24, 6);
    s.fbmsk = (uint32_t)(c.frame >> 32);
    s.zbp = bits(c.zbuf, 0, 9) * 32;
    s.zpsm = 0x30 | bits(c.zbuf, 24, 4);
    s.zmsk = (c.zbuf >> 32) & 1;
    s.ate = c.test & 1;
    s.atst = bits(c.test, 1, 3);
    s.aref = bits(c.test, 4, 8);
    s.afail = bits(c.test, 12, 2);
    s.date = (c.test >> 14) & 1;
    s.datm = (c.test >> 15) & 1;
    s.zte = (c.test >> 16) & 1;
    s.ztst = bits(c.test, 17, 2);
    s.aA = bits(c.alpha, 0, 2);
    s.aB = bits(c.alpha, 2, 2);
    s.aC = bits(c.alpha, 4, 2);
    s.aD = bits(c.alpha, 6, 2);
    s.fix = bits(c.alpha, 32, 8);
    s.pabe = gs_.pabe_ & 1;
    s.fba = c.fba & 1;
    s.colclamp = gs_.colclamp_ & 1;
    s.dthe = gs_.dthe_ & 1;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int v = (int)bits(gs_.dimx_, y * 16 + x * 4, 3);
            if (v & 4) v -= 8;
            s.dimx[y][x] = v;
        }
    s.scx0 = (int)bits(c.scissor, 0, 11);
    s.scx1 = (int)bits(c.scissor, 16, 11);
    s.scy0 = (int)bits(c.scissor, 32, 11);
    s.scy1 = (int)bits(c.scissor, 48, 11);
    // Never draw outside the frame buffer width.
    if (s.fbw) s.scx1 = std::min(s.scx1, (int)s.fbw * 64 - 1);
    s.tbp[0] = bits(c.tex0, 0, 14);
    s.tbw[0] = bits(c.tex0, 14, 6);
    s.tpsm = bits(c.tex0, 20, 6);
    s.tw = std::min<uint32_t>(bits(c.tex0, 26, 4), 10);
    s.th = std::min<uint32_t>(bits(c.tex0, 30, 4), 10);
    s.tcc = bits(c.tex0, 34, 1);
    s.tfx = bits(c.tex0, 35, 2);
    s.cpsm = bits(c.tex0, 51, 4);
    s.csa = bits(c.tex0, 56, 5);
    s.tbp[1] = bits(c.miptbp1, 0, 14);
    s.tbw[1] = bits(c.miptbp1, 14, 6);
    s.tbp[2] = bits(c.miptbp1, 20, 14);
    s.tbw[2] = bits(c.miptbp1, 34, 6);
    s.tbp[3] = bits(c.miptbp1, 40, 14);
    s.tbw[3] = bits(c.miptbp1, 54, 6);
    s.tbp[4] = bits(c.miptbp2, 0, 14);
    s.tbw[4] = bits(c.miptbp2, 14, 6);
    s.tbp[5] = bits(c.miptbp2, 20, 14);
    s.tbw[5] = bits(c.miptbp2, 34, 6);
    s.tbp[6] = bits(c.miptbp2, 40, 14);
    s.tbw[6] = bits(c.miptbp2, 54, 6);
    s.wms = bits(c.clamp, 0, 2);
    s.wmt = bits(c.clamp, 2, 2);
    s.minu = bits(c.clamp, 4, 10);
    s.maxu = bits(c.clamp, 14, 10);
    s.minv = bits(c.clamp, 24, 10);
    s.maxv = bits(c.clamp, 34, 10);
    s.lcm = bits(c.tex1, 0, 1);
    s.mxl = std::min<uint32_t>(bits(c.tex1, 2, 3), 6);
    s.magLinear = bits(c.tex1, 5, 1) || gs_.bilinearOverride;
    const uint32_t mmin = bits(c.tex1, 6, 3);
    s.minLinear = mmin == 1 || mmin == 3 || mmin == 5 || gs_.bilinearOverride;
    s.mipNearest = mmin >= 2;
    s.lL = bits(c.tex1, 19, 2);
    s.lK = bits(c.tex1, 32, 12);
    int k = (int)s.lK;
    if (k & 0x800) k -= 0x1000;
    s.kVal = k / 16.0f;
    s.ta0 = bits(gs_.texa_, 0, 8);
    s.aem = (gs_.texa_ >> 15) & 1;
    s.ta1 = bits(gs_.texa_, 32, 8);
    s.fogR = bits(gs_.fogcol_, 0, 8);
    s.fogG = bits(gs_.fogcol_, 8, 8);
    s.fogB = bits(gs_.fogcol_, 16, 8);
    s.scanmsk = bits(gs_.scanmsk_, 0, 2);
}

void GsRasterizer::draw(uint64_t prim, const GsVertex* v, int count) {
    Setup s;
    setup(prim, s);
    gs_.stats.primitives++;
    switch (prim & 7) {
    case 0: point(s, v[0]); break;
    case 1:
    case 2: line(s, v[0], v[1]); break;
    case 3:
    case 4:
    case 5: triangle(s, v[0], v[1], v[2]); break;
    case 6: sprite(s, v[0], v[1]); break;
    default: break;
    }
    (void)count;
}

// ------------------------------------------------------------------ texturing

uint32_t GsRasterizer::fetchTexel(const Setup& s, int level, int x, int y) {
    const uint32_t bp = s.tbp[level], bw = s.tbw[level];
    const uint32_t raw = gs_.readPixel(bp, bw, s.tpsm, x, y);
    auto expand16 = [&](uint32_t c) {
        const uint32_t r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
        uint32_t a = (c & 0x8000) ? s.ta1 : s.ta0;
        if (s.aem && (c & 0x7FFF) == 0) a = 0;
        return r | (g << 8) | (b << 16) | (a << 24);
    };
    auto fromClut = [&](uint32_t idx, bool four) {
        if (s.cpsm == PSMCT32 || s.cpsm == PSMCT24) {
            const uint32_t i = four ? (((s.csa & 15) * 16 + idx) & 255) : (idx & 255);
            return (uint32_t)gs_.clut_[i] | ((uint32_t)gs_.clut_[i + 256] << 16);
        }
        const uint32_t i = four ? ((s.csa * 16 + idx) & 511) : (idx & 255);
        return expand16(gs_.clut_[i]);
    };
    switch (s.tpsm) {
    case PSMCT32:
    case PSMZ32: return raw;
    case PSMCT24:
    case PSMZ24: {
        uint32_t a = s.ta0;
        if (s.aem && (raw & 0xFFFFFF) == 0) a = 0;
        return (raw & 0xFFFFFF) | (a << 24);
    }
    case PSMCT16:
    case PSMCT16S:
    case PSMZ16:
    case PSMZ16S: return expand16(raw);
    case PSMT8:
    case PSMT8H: return fromClut(raw, false);
    case PSMT4:
    case PSMT4HL:
    case PSMT4HH: return fromClut(raw, true);
    default: return raw;
    }
}

static inline int wrapCoord(int c, int size, uint32_t mode, uint32_t mn, uint32_t mx) {
    switch (mode) {
    case 0: return c & (size - 1);
    case 1: return clampi(c, 0, size - 1);
    case 2: return clampi(c, (int)mn, (int)mx);
    case 3: return (int)(((uint32_t)c & mn) | mx);
    default: return c & (size - 1);
    }
}

uint32_t GsRasterizer::sampleTexture(const Setup& s, float u, float v, float lod) {
    int level = 0;
    bool linear = s.magLinear;
    if (lod > 0.0f) {
        linear = s.minLinear;
        if (s.mipNearest && s.mxl > 0) {
            level = std::min<int>((int)(lod + 0.5f), (int)s.mxl);
            if (level > 0 && s.tbw[level] == 0) level = 0;
        }
    }
    int w = 1 << s.tw, h = 1 << s.th;
    if (level) {
        w = std::max(1, w >> level);
        h = std::max(1, h >> level);
        u /= (float)(1 << level);
        v /= (float)(1 << level);
    }
    const uint32_t minu = level ? s.minu >> level : s.minu, maxu = level ? s.maxu >> level : s.maxu;
    const uint32_t minv = level ? s.minv >> level : s.minv, maxv = level ? s.maxv >> level : s.maxv;
    if (!linear) {
        const int x = wrapCoord((int)std::floor(u), w, s.wms, minu, maxu);
        const int y = wrapCoord((int)std::floor(v), h, s.wmt, minv, maxv);
        return fetchTexel(s, level, x, y);
    }
    const float fu = u - 0.5f, fv = v - 0.5f;
    const int x0 = (int)std::floor(fu), y0 = (int)std::floor(fv);
    const int fx = (int)((fu - (float)x0) * 16.0f), fy = (int)((fv - (float)y0) * 16.0f);
    const int xa = wrapCoord(x0, w, s.wms, minu, maxu), xb = wrapCoord(x0 + 1, w, s.wms, minu, maxu);
    const int ya = wrapCoord(y0, h, s.wmt, minv, maxv), yb = wrapCoord(y0 + 1, h, s.wmt, minv, maxv);
    const uint32_t c00 = fetchTexel(s, level, xa, ya), c10 = fetchTexel(s, level, xb, ya);
    const uint32_t c01 = fetchTexel(s, level, xa, yb), c11 = fetchTexel(s, level, xb, yb);
    uint32_t out = 0;
    for (int k = 0; k < 4; ++k) {
        const int a = (c00 >> (k * 8)) & 0xFF, b = (c10 >> (k * 8)) & 0xFF;
        const int c = (c01 >> (k * 8)) & 0xFF, d = (c11 >> (k * 8)) & 0xFF;
        const int top = a * (16 - fx) + b * fx, bot = c * (16 - fx) + d * fx;
        const int val = (top * (16 - fy) + bot * fy) >> 8;
        out |= (uint32_t)clampi(val, 0, 255) << (k * 8);
    }
    return out;
}

// ------------------------------------------------------------------ pixel pipeline

void GsRasterizer::shade(const Setup& s, Frag& f) {
    if (f.x < s.scx0 || f.x > s.scx1 || f.y < s.scy0 || f.y > s.scy1) return;
    if (s.scanmsk == 2 && !(f.y & 1)) return;
    if (s.scanmsk == 3 && (f.y & 1)) return;
    // Depth test
    uint32_t zmax = 0xFFFFFFFFu;
    if (s.zpsm == PSMZ24) zmax = 0xFFFFFF;
    else if (s.zpsm == PSMZ16 || s.zpsm == PSMZ16S) zmax = 0xFFFF;
    const uint32_t z = std::min(f.z, zmax);
    if (s.zte) {
        const uint32_t zd = gs_.readPixel(s.zbp, s.fbw, s.zpsm, f.x, f.y);
        switch (s.ztst) {
        case 0: return;
        case 1: break;
        case 2: if (z < zd) return; break;
        case 3: if (z <= zd) return; break;
        }
    }
    int r = f.r, g = f.g, b = f.b, a = f.a;
    if (s.tme) {
        const uint32_t t = sampleTexture(s, f.u, f.v, f.lod);
        const int tr = t & 0xFF, tg = (t >> 8) & 0xFF, tb = (t >> 16) & 0xFF, ta = (t >> 24) & 0xFF;
        switch (s.tfx) {
        case 0: // MODULATE
            r = std::min(255, (tr * r) >> 7);
            g = std::min(255, (tg * g) >> 7);
            b = std::min(255, (tb * b) >> 7);
            a = s.tcc ? std::min(255, (ta * a) >> 7) : a;
            break;
        case 1: // DECAL
            r = tr; g = tg; b = tb;
            a = s.tcc ? ta : a;
            break;
        case 2: // HIGHLIGHT
            r = std::min(255, ((tr * r) >> 7) + a);
            g = std::min(255, ((tg * g) >> 7) + a);
            b = std::min(255, ((tb * b) >> 7) + a);
            a = s.tcc ? std::min(255, ta + a) : a;
            break;
        case 3: // HIGHLIGHT2
            r = std::min(255, ((tr * r) >> 7) + a);
            g = std::min(255, ((tg * g) >> 7) + a);
            b = std::min(255, ((tb * b) >> 7) + a);
            a = s.tcc ? ta : a;
            break;
        }
    }
    if (s.fge) {
        const int fo = f.fog;
        r = (fo * r + (255 - fo) * (int)s.fogR) >> 8;
        g = (fo * g + (255 - fo) * (int)s.fogG) >> 8;
        b = (fo * b + (255 - fo) * (int)s.fogB) >> 8;
    }
    // Alpha test
    bool writeRgb = true, writeA = true, writeZ = s.zte ? !s.zmsk : false;
    if (!s.zte) writeZ = false;
    if (s.zte && !s.zmsk) writeZ = true;
    if (s.ate) {
        bool pass = true;
        switch (s.atst) {
        case 0: pass = false; break;
        case 1: pass = true; break;
        case 2: pass = (uint32_t)a < s.aref; break;
        case 3: pass = (uint32_t)a <= s.aref; break;
        case 4: pass = (uint32_t)a == s.aref; break;
        case 5: pass = (uint32_t)a >= s.aref; break;
        case 6: pass = (uint32_t)a > s.aref; break;
        case 7: pass = (uint32_t)a != s.aref; break;
        }
        if (!pass) {
            switch (s.afail) {
            case 0: return;
            case 1: writeZ = false; break;
            case 2: writeRgb = writeA = false; break;
            case 3: writeA = false; writeZ = false; break;
            }
        }
    }
    // Destination color
    const bool fb16 = s.fpsm == PSMCT16 || s.fpsm == PSMCT16S;
    const uint32_t draw = gs_.readPixel(s.fbp, s.fbw, s.fpsm, f.x, f.y);
    int dr, dg, db, da;
    if (fb16) {
        dr = (draw & 0x1F) << 3;
        dg = ((draw >> 5) & 0x1F) << 3;
        db = ((draw >> 10) & 0x1F) << 3;
        da = (draw & 0x8000) ? 0x80 : 0;
    } else {
        dr = draw & 0xFF;
        dg = (draw >> 8) & 0xFF;
        db = (draw >> 16) & 0xFF;
        da = s.fpsm == PSMCT24 ? 0x80 : (int)((draw >> 24) & 0xFF);
    }
    if (s.date) {
        const int bit = fb16 ? ((draw >> 15) & 1) : ((da >> 7) & 1);
        if (bit != (int)s.datm) return;
    }
    if (s.abe && !(s.pabe && !(a & 0x80))) {
        auto sel = [](uint32_t m, int cs, int cd) { return m == 0 ? cs : (m == 1 ? cd : 0); };
        const int cv = s.aC == 0 ? a : (s.aC == 1 ? da : (int)s.fix);
        r = ((sel(s.aA, r, dr) - sel(s.aB, r, dr)) * cv >> 7) + sel(s.aD, r, dr);
        g = ((sel(s.aA, g, dg) - sel(s.aB, g, dg)) * cv >> 7) + sel(s.aD, g, dg);
        b = ((sel(s.aA, b, db) - sel(s.aB, b, db)) * cv >> 7) + sel(s.aD, b, db);
    }
    if (s.dthe && fb16) {
        const int d = s.dimx[f.y & 3][f.x & 3];
        r += d;
        g += d;
        b += d;
    }
    if (s.colclamp) {
        r = clampi(r, 0, 255);
        g = clampi(g, 0, 255);
        b = clampi(b, 0, 255);
    } else {
        r &= 0xFF;
        g &= 0xFF;
        b &= 0xFF;
    }
    if (s.fba) a |= 0x80;
    gs_.stats.pixels++;
    if (writeRgb || writeA) {
        uint32_t out;
        uint32_t mask = s.fbmsk;
        if (fb16) {
            out = (uint32_t)(r >> 3) | ((uint32_t)(g >> 3) << 5) | ((uint32_t)(b >> 3) << 10) | ((a & 0x80) ? 0x8000u : 0u);
            uint32_t m16 = ((mask >> 3) & 0x1F) | (((mask >> 11) & 0x1F) << 5) | (((mask >> 19) & 0x1F) << 10) |
                           ((mask >> 31) << 15);
            if (!writeA) m16 |= 0x8000;
            out = (draw & m16) | (out & ~m16);
        } else {
            out = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)(a & 0xFF) << 24);
            if (!writeA) mask |= 0xFF000000u;
            if (s.fpsm == PSMCT24) mask |= 0xFF000000u;
            out = (draw & mask) | (out & ~mask);
        }
        gs_.writePixel(s.fbp, s.fbw, s.fpsm, f.x, f.y, out);
    }
    if (writeZ) gs_.writePixel(s.zbp, s.fbw, s.zpsm, f.x, f.y, z);
}

// ------------------------------------------------------------------ primitives

static inline float lodFromQ(const GsRasterizer*, float q, uint32_t lcm, uint32_t L, float k) {
    if (lcm) return k;
    const float aq = std::fabs(q);
    if (aq <= 0.0f) return k;
    return (float)(-std::log2(aq) * (float)(1 << L)) + k;
}

void GsRasterizer::triangle(const Setup& s, const GsVertex& v0, const GsVertex& v1, const GsVertex& v2) {
    // Edge setup in 12.4 fixed point; samples at integer pixel positions.
    const int64_t x0 = v0.x, y0 = v0.y, x1 = v1.x, y1 = v1.y, x2 = v2.x, y2 = v2.y;
    int64_t area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0) return;
    const GsVertex* a = &v0;
    const GsVertex* b = &v1;
    const GsVertex* c = &v2;
    if (area < 0) {
        std::swap(b, c);
        area = -area;
    }
    const int64_t ax = a->x, ay = a->y, bx = b->x, by = b->y, cx = c->x, cy = c->y;
    int minx = (int)((std::min({ax, bx, cx}) + 15) >> 4), maxx = (int)((std::max({ax, bx, cx}) - 1) >> 4);
    int miny = (int)((std::min({ay, by, cy}) + 15) >> 4), maxy = (int)((std::max({ay, by, cy}) - 1) >> 4);
    minx = std::max(minx, s.scx0);
    maxx = std::min(maxx, s.scx1);
    miny = std::max(miny, s.scy0);
    maxy = std::min(maxy, s.scy1);
    if (minx > maxx || miny > maxy) return;
    auto edge = [](int64_t px, int64_t py, int64_t qx, int64_t qy, int64_t x, int64_t y) {
        return (qx - px) * (y - py) - (qy - py) * (x - px);
    };
    // Top-left rule bias.
    auto isTopLeft = [](int64_t px, int64_t py, int64_t qx, int64_t qy) {
        const int64_t dy = qy - py, dx = qx - px;
        return (dy < 0) || (dy == 0 && dx > 0);
    };
    const int64_t bias0 = isTopLeft(bx, by, cx, cy) ? 0 : -1;
    const int64_t bias1 = isTopLeft(cx, cy, ax, ay) ? 0 : -1;
    const int64_t bias2 = isTopLeft(ax, ay, bx, by) ? 0 : -1;
    const double inv = 1.0 / (double)area;
    const bool persp = s.tme && !s.fst;
    const float tw = (float)(1 << s.tw), th = (float)(1 << s.th);
    for (int y = miny; y <= maxy; ++y) {
        const int64_t py = (int64_t)y * 16;
        for (int x = minx; x <= maxx; ++x) {
            const int64_t px = (int64_t)x * 16;
            const int64_t w0 = edge(bx, by, cx, cy, px, py) + bias0;
            const int64_t w1 = edge(cx, cy, ax, ay, px, py) + bias1;
            const int64_t w2 = edge(ax, ay, bx, by, px, py) + bias2;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            const double l0 = (double)(w0 - bias0) * inv, l1 = (double)(w1 - bias1) * inv;
            const double l2 = 1.0 - l0 - l1;
            Frag f;
            f.x = x;
            f.y = y;
            const double zf = l0 * a->z + l1 * b->z + l2 * c->z;
            f.z = (uint32_t)std::clamp(zf, 0.0, 4294967295.0);
            if (s.iip) {
                f.r = clampi((int)(l0 * a->r + l1 * b->r + l2 * c->r), 0, 255);
                f.g = clampi((int)(l0 * a->g + l1 * b->g + l2 * c->g), 0, 255);
                f.b = clampi((int)(l0 * a->b + l1 * b->b + l2 * c->b), 0, 255);
                f.a = clampi((int)(l0 * a->a + l1 * b->a + l2 * c->a), 0, 255);
            } else {
                f.r = v2.r;
                f.g = v2.g;
                f.b = v2.b;
                f.a = v2.a;
            }
            f.fog = clampi((int)(l0 * a->fog + l1 * b->fog + l2 * c->fog), 0, 255);
            f.lod = 0;
            if (s.tme) {
                if (persp) {
                    const double sq = l0 * a->s + l1 * b->s + l2 * c->s;
                    const double tq = l0 * a->t + l1 * b->t + l2 * c->t;
                    const double q = l0 * a->q + l1 * b->q + l2 * c->q;
                    const double iq = q != 0.0 ? 1.0 / q : 0.0;
                    f.u = (float)(sq * iq) * tw;
                    f.v = (float)(tq * iq) * th;
                    f.lod = lodFromQ(this, (float)q, s.lcm, s.lL, s.kVal);
                } else {
                    f.u = (float)((l0 * a->u + l1 * b->u + l2 * c->u) / 16.0);
                    f.v = (float)((l0 * a->v + l1 * b->v + l2 * c->v) / 16.0);
                    f.lod = s.lcm ? s.kVal : 0.0f;
                }
            }
            shade(s, f);
        }
    }
}

void GsRasterizer::sprite(const Setup& s, const GsVertex& v0, const GsVertex& v1) {
    const GsVertex& l = v0.x <= v1.x ? v0 : v1;
    const GsVertex& r = v0.x <= v1.x ? v1 : v0;
    const GsVertex& t = v0.y <= v1.y ? v0 : v1;
    const GsVertex& bt = v0.y <= v1.y ? v1 : v0;
    int x0 = (l.x + 15) >> 4, x1 = ((r.x + 15) >> 4) - 1;
    int y0 = (t.y + 15) >> 4, y1 = ((bt.y + 15) >> 4) - 1;
    const int sx0 = x0, sy0 = y0;
    x0 = std::max(x0, s.scx0);
    x1 = std::min(x1, s.scx1);
    y0 = std::max(y0, s.scy0);
    y1 = std::min(y1, s.scy1);
    if (x0 > x1 || y0 > y1) return;
    const float spanX = (float)(r.x - l.x) / 16.0f, spanY = (float)(bt.y - t.y) / 16.0f;
    float u0, u1, vv0, vv1;
    const float tw = (float)(1 << s.tw), th = (float)(1 << s.th);
    if (s.fst) {
        u0 = l.u / 16.0f;
        u1 = r.u / 16.0f;
        vv0 = t.v / 16.0f;
        vv1 = bt.v / 16.0f;
    } else {
        const float lq = l.q != 0 ? l.q : 1.0f, rq = r.q != 0 ? r.q : 1.0f;
        const float tq = t.q != 0 ? t.q : 1.0f, bq = bt.q != 0 ? bt.q : 1.0f;
        u0 = l.s / lq * tw;
        u1 = r.s / rq * tw;
        vv0 = t.t / tq * th;
        vv1 = bt.t / bq * th;
    }
    const float du = spanX > 0 ? (u1 - u0) / spanX : 0, dv = spanY > 0 ? (vv1 - vv0) / spanY : 0;
    // Offset of first pixel centre relative to the left/top vertex.
    const float offX = (float)sx0 - (float)l.x / 16.0f, offY = (float)sy0 - (float)t.y / 16.0f;
    const float lod = s.lcm ? s.kVal : (s.fst ? 0.0f : lodFromQ(this, v1.q, 0, s.lL, s.kVal));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            Frag f;
            f.x = x;
            f.y = y;
            f.z = v1.z;
            f.r = v1.r;
            f.g = v1.g;
            f.b = v1.b;
            f.a = v1.a;
            f.fog = v1.fog;
            f.u = u0 + du * (offX + (float)(x - sx0));
            f.v = vv0 + dv * (offY + (float)(y - sy0));
            f.lod = lod;
            shade(s, f);
        }
    }
}

void GsRasterizer::line(const Setup& s, const GsVertex& a, const GsVertex& b) {
    const float ax = a.x / 16.0f, ay = a.y / 16.0f, bx = b.x / 16.0f, by = b.y / 16.0f;
    const float dx = bx - ax, dy = by - ay;
    const int steps = (int)std::max(std::fabs(dx), std::fabs(dy));
    if (steps <= 0) return;
    for (int i = 0; i < steps; ++i) {
        const float t = (float)i / (float)steps;
        Frag f;
        f.x = (int)std::floor(ax + dx * t + 0.5f);
        f.y = (int)std::floor(ay + dy * t + 0.5f);
        f.z = (uint32_t)((double)a.z + ((double)b.z - (double)a.z) * t);
        if (s.iip) {
            f.r = (int)(a.r + (b.r - a.r) * t);
            f.g = (int)(a.g + (b.g - a.g) * t);
            f.b = (int)(a.b + (b.b - a.b) * t);
            f.a = (int)(a.a + (b.a - a.a) * t);
        } else {
            f.r = b.r; f.g = b.g; f.b = b.b; f.a = b.a;
        }
        f.fog = (int)(a.fog + (b.fog - a.fog) * t);
        if (s.fst) {
            f.u = (a.u + (b.u - a.u) * t) / 16.0f;
            f.v = (a.v + (b.v - a.v) * t) / 16.0f;
        } else {
            const float q = a.q + (b.q - a.q) * t;
            f.u = (a.s + (b.s - a.s) * t) / (q ? q : 1.0f) * (float)(1 << s.tw);
            f.v = (a.t + (b.t - a.t) * t) / (q ? q : 1.0f) * (float)(1 << s.th);
        }
        f.lod = s.lcm ? s.kVal : 0.0f;
        shade(s, f);
    }
}

void GsRasterizer::point(const Setup& s, const GsVertex& a) {
    Frag f;
    f.x = (a.x + 8) >> 4;
    f.y = (a.y + 8) >> 4;
    f.z = a.z;
    f.r = a.r; f.g = a.g; f.b = a.b; f.a = a.a;
    f.fog = a.fog;
    if (s.fst) {
        f.u = a.u / 16.0f;
        f.v = a.v / 16.0f;
    } else {
        f.u = a.s / (a.q ? a.q : 1.0f) * (float)(1 << s.tw);
        f.v = a.t / (a.q ? a.q : 1.0f) * (float)(1 << s.th);
    }
    f.lod = s.lcm ? s.kVal : 0.0f;
    shade(s, f);
}

} // namespace ps2
