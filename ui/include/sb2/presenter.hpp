// Video output: uploads the PS2 display image, runs the post-processing
// chain (scaling filter, FXAA, sharpening, scanlines, colour) on the GPU and
// draws the Dear ImGui layer on top.
//
//   D3D12Presenter  - Windows, Direct3D 12 (hardware adapter, WARP fallback)
//   SdlPresenter    - any SDL_Renderer driver (fallback / non-Windows)
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sb2/settings.hpp"

struct SDL_Window;
union SDL_Event;

namespace sb2 {

struct PostParams {
    PresentFilter filter = PresentFilter::Bilinear;
    bool fxaa = false;
    bool blendFields = false;   // deinterlace: average with the previous field
    float sharpen = 0.0f;       // 0..1
    float scanlines = 0.0f;     // 0..1 (0 = off)
    float brightness = 1.0f, contrast = 1.0f, gamma = 1.0f, saturation = 1.0f;
};

struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
};

struct PresenterCaps {
    bool shaderFilters = false; // sharp bilinear / bicubic / lanczos / FXAA / colour / scanlines
    bool tearing = false;       // VSync off without frame rate cap
};

class Presenter {
public:
    virtual ~Presenter() = default;
    virtual const char* name() const = 0;
    virtual std::string adapterName() const { return {}; }
    virtual PresenterCaps caps() const = 0;
    virtual void setVsync(bool vsync, bool allowTearing) = 0;
    virtual void setFrameLatency(int frames) { (void)frames; }
    // RGBA8 pixels (R in the low byte), row-major, `w` pixels per row.
    virtual void uploadFrame(const uint32_t* rgba, int w, int h, bool interlaced) = 0;
    virtual void clearFrame() = 0;
    // Start a Dear ImGui frame (renderer backend part; the SDL3 platform
    // backend and ImGui::NewFrame are called by the owner).
    virtual void newImGuiFrame() = 0;
    // Draw the frame into `dst` (window pixels) and the ImGui draw data.
    virtual void render(const RectF& dst, const PostParams& post, bool showFrame) = 0;
    virtual void onResize(int width, int height) = 0;
    // Screenshots: request a capture of the next rendered image, then poll
    // takeScreenshot() after render() until it returns true (RGBA8).
    virtual void requestScreenshot() = 0;
    virtual bool takeScreenshot(std::vector<uint32_t>& pixels, int& width, int& height) = 0;
};

// Creates the requested presenter (falls back to the SDL renderer when the
// preferred one cannot be created).  Also initialises the ImGui SDL3
// platform backend for the window.  An ImGui context must exist.
std::unique_ptr<Presenter> createPresenter(Backend preferred, SDL_Window* window, const GraphicsSettings& g,
                                           std::string& log);

// Window creation helper: the window flags depend on the presenter type.
SDL_Window* createMainWindow(const char* title, int w, int h, Backend preferred, bool resizable);

// Encoders for screenshots.
bool writePng(const std::string& path, const uint32_t* rgba, int w, int h);
bool writeBmp(const std::string& path, const uint32_t* rgba, int w, int h);

// Destination rectangle for the game image inside a window.
RectF computeDestRect(int winW, int winH, int srcW, int srcH, AspectMode aspect, bool integerScale);

} // namespace sb2
