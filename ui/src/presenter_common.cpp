#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"
#include "sb2/hash.hpp"
#include "sb2/platform.hpp"
#include "sb2/presenter.hpp"

namespace sb2 {

#if defined(_WIN32)
std::unique_ptr<Presenter> createD3D12Presenter(SDL_Window* window, const GraphicsSettings& g, std::string& err);
#endif

namespace {

// ------------------------------------------------------------------ SDL_Renderer fallback

class SdlPresenter final : public Presenter {
public:
    ~SdlPresenter() override {
        if (imguiInit_) {
            ImGui_ImplSDLRenderer3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
        }
        if (tex_) SDL_DestroyTexture(tex_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
    }

    bool init(SDL_Window* w, const GraphicsSettings& g, std::string& err) {
        window_ = w;
        renderer_ = SDL_CreateRenderer(w, nullptr);
        if (!renderer_) {
            err = SDL_GetError();
            return false;
        }
        setVsync(g.vsync, false);
        ImGui_ImplSDL3_InitForSDLRenderer(w, renderer_);
        ImGui_ImplSDLRenderer3_Init(renderer_);
        imguiInit_ = true;
        return true;
    }

    const char* name() const override { return "SDL_Renderer"; }
    std::string adapterName() const override {
        const char* n = SDL_GetRendererName(renderer_);
        return n ? n : "";
    }
    PresenterCaps caps() const override { return PresenterCaps{false, false}; }

    void setVsync(bool vsync, bool) override { SDL_SetRenderVSync(renderer_, vsync ? 1 : 0); }

    void uploadFrame(const uint32_t* rgba, int w, int h, bool) override {
        if (!tex_ || texW_ != w || texH_ != h) {
            if (tex_) SDL_DestroyTexture(tex_);
            tex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
            texW_ = w;
            texH_ = h;
        }
        if (tex_) SDL_UpdateTexture(tex_, nullptr, rgba, w * 4);
        hasFrame_ = tex_ != nullptr;
    }
    void clearFrame() override { hasFrame_ = false; }

    void newImGuiFrame() override { ImGui_ImplSDLRenderer3_NewFrame(); }

    void render(const RectF& dst, const PostParams& post, bool showFrame) override {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        if (showFrame && hasFrame_ && tex_) {
            SDL_SetTextureScaleMode(tex_, post.filter == PresentFilter::Nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
            const Uint8 mod = (Uint8)std::clamp(post.brightness * 255.0f, 0.0f, 255.0f);
            SDL_SetTextureColorMod(tex_, mod, mod, mod);
            SDL_FRect r{dst.x, dst.y, dst.w, dst.h};
            SDL_RenderTexture(renderer_, tex_, nullptr, &r);
        }
        if (ImDrawData* dd = ImGui::GetDrawData()) ImGui_ImplSDLRenderer3_RenderDrawData(dd, renderer_);
        // Keep a copy for screenshots before presenting (back buffer contents
        // are undefined afterwards).
        if (wantShot_) {
            if (SDL_Surface* s = SDL_RenderReadPixels(renderer_, nullptr)) {
                SDL_Surface* c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);
                if (c) {
                    shotW_ = c->w;
                    shotH_ = c->h;
                    shot_.resize((size_t)c->w * c->h);
                    for (int y = 0; y < c->h; ++y)
                        std::memcpy(&shot_[(size_t)y * c->w], (uint8_t*)c->pixels + (size_t)y * c->pitch, (size_t)c->w * 4);
                    SDL_DestroySurface(c);
                }
                SDL_DestroySurface(s);
            }
            wantShot_ = false;
            shotReady_ = true;
        }
        SDL_RenderPresent(renderer_);
    }

    void onResize(int, int) override {}

    void requestScreenshot() override { wantShot_ = true; }
    bool takeScreenshot(std::vector<uint32_t>& px, int& w, int& h) override {
        if (!shotReady_) return false;
        px = shot_;
        w = shotW_;
        h = shotH_;
        shotReady_ = false;
        return true;
    }

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* tex_ = nullptr;
    int texW_ = 0, texH_ = 0;
    bool hasFrame_ = false;
    bool imguiInit_ = false;
    bool wantShot_ = false, shotReady_ = false;
    std::vector<uint32_t> shot_;
    int shotW_ = 0, shotH_ = 0;
};

// ------------------------------------------------------------------ image encoders

void put32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

void pngChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32be(out, (uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    put32be(out, sb2::crc32(td.data(), td.size()));
}

} // namespace

bool writePng(const std::string& path, const uint32_t* rgba, int w, int h) {
    // Uncompressed (stored) deflate stream: simple and dependency free.
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const uint32_t p = rgba[(size_t)y * w + x];
            raw.push_back((uint8_t)p);
            raw.push_back((uint8_t)(p >> 8));
            raw.push_back((uint8_t)(p >> 16));
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t off = 0; off < raw.size() || off == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)n);
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n);
        z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + (ptrdiff_t)off, raw.begin() + (ptrdiff_t)(off + n));
        off += n;
        if (last) break;
    }
    put32be(z, (b << 16) | a);
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put32be(ihdr, (uint32_t)w);
    put32be(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    pngChunk(out, "IHDR", ihdr);
    pngChunk(out, "IDAT", z);
    pngChunk(out, "IEND", {});
    return writeFile(path, out.data(), out.size());
}

bool writeBmp(const std::string& path, const uint32_t* rgba, int w, int h) {
    const uint32_t rowBytes = (uint32_t)((w * 3 + 3) & ~3);
    const uint32_t dataSize = rowBytes * (uint32_t)h;
    std::vector<uint8_t> out(54 + dataSize, 0);
    auto le32 = [&](size_t o, uint32_t v) { std::memcpy(&out[o], &v, 4); };
    auto le16 = [&](size_t o, uint16_t v) { std::memcpy(&out[o], &v, 2); };
    out[0] = 'B';
    out[1] = 'M';
    le32(2, (uint32_t)out.size());
    le32(10, 54);
    le32(14, 40);
    le32(18, (uint32_t)w);
    le32(22, (uint32_t)h);
    le16(26, 1);
    le16(28, 24);
    le32(34, dataSize);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = &out[54 + (size_t)(h - 1 - y) * rowBytes];
        for (int x = 0; x < w; ++x) {
            const uint32_t p = rgba[(size_t)y * w + x];
            row[x * 3 + 0] = (uint8_t)(p >> 16);
            row[x * 3 + 1] = (uint8_t)(p >> 8);
            row[x * 3 + 2] = (uint8_t)p;
        }
    }
    return writeFile(path, out.data(), out.size());
}

RectF computeDestRect(int winW, int winH, int srcW, int srcH, AspectMode aspect, bool integerScale) {
    RectF r;
    if (winW <= 0 || winH <= 0 || srcW <= 0 || srcH <= 0) return r;
    double ar;
    switch (aspect) {
    case AspectMode::Ratio16x9: ar = 16.0 / 9.0; break;
    case AspectMode::Stretch: ar = (double)winW / winH; break;
    case AspectMode::Ratio4x3:
    case AspectMode::Auto:
    default: ar = 4.0 / 3.0; break;
    }
    double w = winW, h = w / ar;
    if (h > winH) {
        h = winH;
        w = h * ar;
    }
    if (integerScale && aspect != AspectMode::Stretch) {
        // Integer multiple of the source height; width follows the aspect ratio.
        const int k = std::max(1, (int)std::floor(h / srcH));
        if (k * srcH <= winH) {
            h = (double)k * srcH;
            w = h * ar;
            if (w > winW) {
                w = winW;
                h = w / ar;
            }
        }
    }
    r.w = (float)std::round(w);
    r.h = (float)std::round(h);
    r.x = (float)std::floor((winW - r.w) * 0.5);
    r.y = (float)std::floor((winH - r.h) * 0.5);
    return r;
}

SDL_Window* createMainWindow(const char* title, int w, int h, Backend, bool resizable) {
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (resizable) flags |= SDL_WINDOW_RESIZABLE;
    return SDL_CreateWindow(title, w, h, flags);
}

std::unique_ptr<Presenter> createPresenter(Backend preferred, SDL_Window* window, const GraphicsSettings& g, std::string& log) {
#if defined(_WIN32)
    if (preferred == Backend::Auto || preferred == Backend::D3D12) {
        std::string err;
        if (auto p = createD3D12Presenter(window, g, err)) {
            log += "video: Direct3D 12 (" + p->adapterName() + ")\n";
            return p;
        }
        log += "video: Direct3D 12 unavailable: " + err + " - falling back to SDL_Renderer\n";
    }
#else
    (void)preferred;
#endif
    auto p = std::make_unique<SdlPresenter>();
    std::string err;
    if (!p->init(window, g, err)) {
        log += "video: SDL_Renderer failed: " + err + "\n";
        return nullptr;
    }
    log += "video: SDL_Renderer (" + p->adapterName() + ")\n";
    return p;
}

} // namespace sb2
