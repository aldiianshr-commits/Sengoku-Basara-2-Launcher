// Windowed front-end for recompiled games.
//
// Threads:
//   main thread       SDL events, input, Dear ImGui (menu / OSD), presentation
//   emulation thread  Machine::run(): recompiled EE code, kernel, GS, ...
//   audio thread      (SDL) pulls mixed SPU2 samples
//   control thread    reads launcher commands from stdin (optional)
//
// The emulation thread hands every displayed GS frame to the main thread and
// paces itself at each vblank (precise timer with audio-buffer feedback, or
// locked to presentation), and blocks there while the game is paused.
#include "sb2/game.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"
#include "ps2/gs.hpp"
#include "ps2/host.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"
#include "ps2/pad.hpp"
#include "ps2/spu2.hpp"
#include "sb2/input.hpp"
#include "sb2/platform.hpp"
#include "sb2/presenter.hpp"
#include "sb2/settings.hpp"
#include "sb2/ui_common.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace sb2 {

using ui::T;
using Clock = std::chrono::steady_clock;

namespace {

struct LaunchExtras {
    std::string title = "Sengoku Basara 2 Heroes Recompiled";
    bool controlStdin = false;
    std::string screenshotDir;
    std::string exitScreenshot;
};

class GameFrontend final : public ps2::Frontend {
public:
    explicit GameFrontend(LaunchExtras x) : x_(std::move(x)) {}
    int run(ps2::Machine& m, const std::string& configFile) override;

private:
    // ---- emulation thread side
    void onFrame(const ps2::GsFrame& f);
    void onVsync();
    // ---- main thread side
    void applyWindowMode();
    void handleEvent(const SDL_Event& e);
    void drawOverlay();
    void drawMenu();
    void pollControl();
    void saveSettings();
    void takeScreenshot(const std::string& path);
    void notify(const std::string& text);
    void syncRuntimeSettings();
    PostParams postParams() const;

    LaunchExtras x_;
    ps2::Machine* m_ = nullptr;
    std::string configFile_;
    Settings settings_;
    bool settingsDirty_ = false;

    SDL_Window* window_ = nullptr;
    std::unique_ptr<Presenter> presenter_;
    std::unique_ptr<InputManager> input_;
    SDL_AudioStream* audio_ = nullptr;
    ui::Fonts fonts_;

    // Frame hand-off
    std::mutex frameMutex_;
    std::condition_variable frameCv_;
    std::vector<uint32_t> framePixels_;
    int frameW_ = 0, frameH_ = 0;
    bool frameInterlaced_ = false;
    uint64_t frameSeq_ = 0;
    uint64_t uploadedSeq_ = 0;
    std::atomic<uint64_t> presentedSeq_{0};

    // Pause / pacing (shared with the emulation thread)
    std::mutex pauseMutex_;
    std::condition_variable pauseCv_;
    std::atomic<bool> userPaused_{false};
    std::atomic<bool> menuOpen_{false};
    std::atomic<bool> focusPaused_{false};
    std::atomic<bool> emuPaused_{false};
    std::atomic<int> pacing_{0};
    std::atomic<int> fpsLimit_{0};
    std::atomic<int> textureFilter_{0};
    std::atomic<bool> audioEnabled_{true};
    std::atomic<bool> audioSync_{true};
    std::atomic<int> audioLatencyMs_{64};
    Clock::time_point nextVsync_{};
    bool pacingReset_ = true;

    // Statistics (emulation thread writes, main reads)
    std::atomic<uint64_t> vsyncCount_{0};
    std::atomic<double> vps_{0.0}, speed_{0.0};
    Clock::time_point statT0_{};
    uint64_t statV0_ = 0;
    double presentFps_ = 0.0;
    uint64_t presentCount_ = 0;
    Clock::time_point presentT0_{};

    // Control channel
    std::mutex ctlMutex_;
    std::vector<std::string> ctlQueue_;

    // UI state
    bool menuWanted_ = false;
    int menuTab_ = 0;
    int controlsPort_ = 0;
    std::string toast_;
    Clock::time_point toastUntil_{};
    bool screenshotPending_ = false;
    std::string screenshotPath_;
    bool quitRequested_ = false;
    std::string presenterLog_;
};

// ------------------------------------------------------------------ emulation thread

void GameFrontend::onFrame(const ps2::GsFrame& f) {
    std::lock_guard<std::mutex> lk(frameMutex_);
    framePixels_.assign(f.pixels.begin(), f.pixels.end());
    frameW_ = (int)f.width;
    frameH_ = (int)f.height;
    frameInterlaced_ = f.interlaced;
    ++frameSeq_;
    frameCv_.notify_all();
}

void GameFrontend::onVsync() {
    ps2::Machine& m = *m_;
    m.gs->textureFilterMode = textureFilter_.load(std::memory_order_relaxed);
    const double consoleRate = m.palMode ? 50.0 : 59.94;
    const int limit = fpsLimit_.load(std::memory_order_relaxed);
    const double rate = limit > 0 ? (double)limit : consoleRate;

    // Audio: the SPU2 renders each field itself; keep the device queue bounded.
    m.spu2->enabled = audioEnabled_.load(std::memory_order_relaxed);
    if (m.spu2->enabled) {
        const size_t maxQueued = (size_t)(48 * std::max(32, audioLatencyMs_.load()) * 3);
        m.spu2->trim(maxQueued);
    }

    // Pause.
    if (userPaused_ || menuOpen_ || focusPaused_) {
        std::unique_lock<std::mutex> lk(pauseMutex_);
        emuPaused_ = true;
        while ((userPaused_ || menuOpen_ || focusPaused_) && !m.exiting()) pauseCv_.wait_for(lk, std::chrono::milliseconds(50));
        emuPaused_ = false;
        pacingReset_ = true;
    }

    const uint64_t v = ++vsyncCount_;
    const auto now = Clock::now();
    if (statT0_ == Clock::time_point{}) {
        statT0_ = now;
        statV0_ = v;
    } else {
        const double dt = std::chrono::duration<double>(now - statT0_).count();
        if (dt >= 0.5) {
            const double vps = (double)(v - statV0_) / dt;
            vps_ = vps;
            speed_ = vps / consoleRate * 100.0;
            statT0_ = now;
            statV0_ = v;
        }
    }

    switch ((PacingMode)pacing_.load(std::memory_order_relaxed)) {
    case PacingMode::Unthrottled:
        pacingReset_ = true;
        break;
    case PacingMode::DisplayVsync: {
        // Wait until the main thread has presented the previous frame.
        std::unique_lock<std::mutex> lk(frameMutex_);
        const uint64_t want = frameSeq_ > 0 ? frameSeq_ - 1 : 0;
        frameCv_.wait_for(lk, std::chrono::milliseconds(100), [&] { return presentedSeq_.load() >= want || m.exiting(); });
        pacingReset_ = true;
        break;
    }
    case PacingMode::Timer:
    default: {
        double period = 1.0 / rate;
        if (audioSync_ && audioEnabled_) {
            // Audio as master clock: stretch or shrink the frame by up to 0.5%
            // to keep the device buffer near the target latency.
            const double target = 48.0 * audioLatencyMs_.load();
            const double level = (double)m.spu2->queuedFrames();
            const double err = std::clamp((level - target) / std::max(1.0, target), -1.0, 1.0);
            period *= 1.0 + 0.005 * err;
        }
        const auto p = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(period));
        if (pacingReset_ || now > nextVsync_ + p * 6) {
            nextVsync_ = now + p;
            pacingReset_ = false;
        } else {
            nextVsync_ += p;
            const auto wait = nextVsync_ - Clock::now();
            if (wait > std::chrono::microseconds(50)) {
                SDL_DelayPrecise((Uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count());
            }
        }
        break;
    }
    }
}

// ------------------------------------------------------------------ main thread

void GameFrontend::syncRuntimeSettings() {
    const auto& g = settings_.graphics;
    pacing_ = (int)g.pacing;
    fpsLimit_ = g.fpsLimit;
    textureFilter_ = (int)g.textureFiltering;
    audioEnabled_ = settings_.audio.enabled;
    audioSync_ = settings_.audio.syncToAudio;
    audioLatencyMs_ = settings_.audio.latencyMs;
    if (presenter_) {
        presenter_->setVsync(g.vsync, g.allowTearing);
        presenter_->setFrameLatency(g.maxFrameLatency);
    }
    if (audio_) SDL_SetAudioStreamGain(audio_, settings_.audio.enabled ? settings_.audio.volume / 100.0f : 0.0f);
    ui::setLanguage(settings_.system.language);
}

PostParams GameFrontend::postParams() const {
    const auto& g = settings_.graphics;
    PostParams p;
    p.filter = g.filter;
    p.fxaa = g.antiAliasing == AntiAliasing::Fxaa;
    p.blendFields = g.deinterlace == Deinterlace::Blend;
    p.sharpen = g.sharpen / 100.0f;
    p.scanlines = g.scanlines ? g.scanlineStrength / 100.0f : 0.0f;
    p.brightness = g.brightness;
    p.contrast = g.contrast;
    p.gamma = g.gamma;
    p.saturation = g.saturation;
    return p;
}

void GameFrontend::applyWindowMode() {
    const auto& g = settings_.graphics;
    if (g.fullscreen == FullscreenMode::Windowed) {
        SDL_SetWindowFullscreen(window_, false);
        return;
    }
    if (g.fullscreen == FullscreenMode::Exclusive) {
        int n = 0;
        SDL_DisplayID* ids = SDL_GetDisplays(&n);
        SDL_DisplayID d = (ids && n > 0) ? ids[std::min(g.display, n - 1)] : SDL_GetPrimaryDisplay();
        SDL_free(ids);
        const SDL_DisplayMode* desk = SDL_GetDesktopDisplayMode(d);
        const int w = g.exclusiveWidth ? g.exclusiveWidth : (desk ? desk->w : 1920);
        const int h = g.exclusiveHeight ? g.exclusiveHeight : (desk ? desk->h : 1080);
        const float hz = g.exclusiveRefresh ? (float)g.exclusiveRefresh : (desk ? desk->refresh_rate : 60.0f);
        SDL_DisplayMode mode{};
        if (SDL_GetClosestFullscreenDisplayMode(d, w, h, hz, true, &mode)) SDL_SetWindowFullscreenMode(window_, &mode);
        else SDL_SetWindowFullscreenMode(window_, nullptr);
    } else {
        SDL_SetWindowFullscreenMode(window_, nullptr); // borderless desktop
    }
    SDL_SetWindowFullscreen(window_, true);
}

void GameFrontend::notify(const std::string& text) {
    toast_ = text;
    toastUntil_ = Clock::now() + std::chrono::seconds(3);
}

void GameFrontend::saveSettings() {
    if (!settingsDirty_ || configFile_.empty()) return;
    settings_.clamp();
    settings_.save(configFile_);
    settingsDirty_ = false;
}

void GameFrontend::takeScreenshot(const std::string& path) {
    screenshotPath_ = path;
    screenshotPending_ = true;
    presenter_->requestScreenshot();
}

void GameFrontend::handleEvent(const SDL_Event& e) {
    ImGui_ImplSDL3_ProcessEvent(&e);
    input_->handleEvent(e);
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        quitRequested_ = true;
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        presenter_->onResize(e.window.data1, e.window.data2);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (settings_.graphics.pauseOnFocusLoss) focusPaused_ = true;
        input_->stopRumble();
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        focusPaused_ = false;
        pauseCv_.notify_all();
        break;
    case SDL_EVENT_KEY_DOWN: {
        if (input_->capturing() || e.key.repeat) break;
        const SDL_Keycode k = e.key.key;
        if (k == SDLK_ESCAPE || k == SDLK_F1) {
            menuWanted_ = !menuWanted_;
        } else if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
            auto& fs = settings_.graphics.fullscreen;
            fs = fs == FullscreenMode::Windowed ? FullscreenMode::Borderless : FullscreenMode::Windowed;
            applyWindowMode();
            settingsDirty_ = true;
        } else if (k == SDLK_F12) {
            char name[64];
            const std::time_t t = std::time(nullptr);
            std::strftime(name, sizeof name, "sb2h_%Y%m%d_%H%M%S.png", std::localtime(&t));
            std::string dir = x_.screenshotDir.empty() ? joinPath(userDataDir(), "screenshots") : x_.screenshotDir;
            makeDirs(dir);
            takeScreenshot(joinPath(dir, name));
        } else if (k == SDLK_F10) {
            settings_.graphics.showFps = !settings_.graphics.showFps;
            settingsDirty_ = true;
        } else if (k == SDLK_PAUSE || k == SDLK_P) {
            if (k == SDLK_PAUSE || (e.key.mod & SDL_KMOD_CTRL)) {
                userPaused_ = !userPaused_;
                pauseCv_.notify_all();
                notify(userPaused_ ? T("Dijeda", "Paused") : T("Dilanjutkan", "Resumed"));
            }
        }
        break;
    }
    default:
        break;
    }
}

void GameFrontend::pollControl() {
    std::vector<std::string> q;
    {
        std::lock_guard<std::mutex> lk(ctlMutex_);
        q.swap(ctlQueue_);
    }
    for (const std::string& c : q) {
        if (c == "pause") {
            userPaused_ = true;
        } else if (c == "resume") {
            userPaused_ = false;
            pauseCv_.notify_all();
        } else if (c == "stop") {
            quitRequested_ = true;
        } else if (c == "screenshot") {
            std::string dir = x_.screenshotDir.empty() ? joinPath(userDataDir(), "screenshots") : x_.screenshotDir;
            makeDirs(dir);
            takeScreenshot(joinPath(dir, "sb2h_remote.png"));
        } else if (c == "reload-settings") {
            settings_ = Settings::load(configFile_);
            syncRuntimeSettings();
        }
    }
}

void GameFrontend::drawOverlay() {
    const auto& g = settings_.graphics;
    ImGuiIO& io = ImGui::GetIO();
    if (g.showFps || g.showStats || userPaused_) {
        ImGui::SetNextWindowPos(ImVec2(12, 12));
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("##osd", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
        if (userPaused_) ImGui::TextColored(ui::kWarn, "%s", T("DIJEDA", "PAUSED"));
        if (g.showFps) ImGui::Text("%.1f VPS  %.0f%%  |  %.1f FPS", vps_.load(), speed_.load(), presentFps_);
        if (g.showStats) {
            ImGui::Text("%s %dx%d%s", T("Gambar", "Image"), frameW_, frameH_, frameInterlaced_ ? " (i)" : "");
            ImGui::Text("GS: %llu prims, %llu px", (unsigned long long)m_->gs->stats.primitives,
                        (unsigned long long)m_->gs->stats.pixels);
            ImGui::Text("Audio: %.0f ms", m_->spu2->queuedFrames() / 48.0);
            ImGui::Text("%s", presenter_->name());
        }
        ImGui::End();
    }
    if (!toast_.empty() && Clock::now() < toastUntil_) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 40), 0, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        ImGui::Begin("##toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted(toast_.c_str());
        ImGui::End();
    }
}

void GameFrontend::drawMenu() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size(std::min(io.DisplaySize.x - 40, ImGui::GetFontSize() * 46),
                      std::min(io.DisplaySize.y - 40, ImGui::GetFontSize() * 36));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::PushFont(fonts_.heading, fonts_.heading->LegacySize);
    ImGui::TextUnformatted(x_.title.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s  |  %s", presenter_->name(), presenter_->adapterName().c_str());
    ImGui::Separator();
    const float navW = ImGui::GetFontSize() * 10;
    ImGui::BeginChild("nav", ImVec2(navW, 0), 0, ImGuiWindowFlags_NoBackground);
    const char* tabs[] = {T("Lanjutkan", "Resume"), T("Grafis", "Graphics"), T("Grafis lanjutan", "Advanced graphics"), "Audio",
                          T("Kontrol", "Controls"), T("Keluar dari game", "Quit game")};
    for (int i = 0; i < 6; ++i) {
        if (i == 5) ImGui::Spacing();
        if (ImGui::Selectable(tabs[i], menuTab_ == i && i != 0 && i != 5, 0, ImVec2(0, ImGui::GetFontSize() * 1.8f))) {
            if (i == 0) menuWanted_ = false;
            else if (i == 5) quitRequested_ = true;
            else menuTab_ = i;
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("Esc: tutup menu\nF11: layar penuh\nF12: screenshot\nF10: FPS", "Esc: close menu\nF11: fullscreen\nF12: screenshot\nF10: FPS"));
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    bool ch = false;
    const PresenterCaps caps = presenter_->caps();
    const FullscreenMode oldFs = settings_.graphics.fullscreen;
    switch (menuTab_) {
    case 1: ch = ui::editGraphics(settings_.graphics, caps, true); break;
    case 2: ch = ui::editAdvancedGraphics(settings_.graphics, caps, true); break;
    case 3: ch = ui::editAudio(settings_.audio); break;
    case 4: ch = ui::editControls(settings_, *input_, controlsPort_); break;
    default: menuTab_ = 1; break;
    }
    ImGui::EndChild();
    ImGui::End();
    if (ch) {
        settingsDirty_ = true;
        settings_.clamp();
        syncRuntimeSettings();
        if (settings_.graphics.fullscreen != oldFs) applyWindowMode();
    }
}

int GameFrontend::run(ps2::Machine& m, const std::string& configFile) {
    m_ = &m;
    configFile_ = configFile;
    settings_ = configFile.empty() ? Settings() : Settings::load(configFile);
    ps2::logSetLevel(settings_.system.logLevel);
    ui::setLanguage(settings_.system.language);

    SDL_SetHint(SDL_HINT_APP_NAME, "Sengoku Basara 2 Heroes Recompiled");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        ps2::logError("frontend: SDL_Init failed: %s - running headless", SDL_GetError());
        return m.run();
    }
    window_ = createMainWindow(x_.title.c_str(), settings_.graphics.windowWidth, settings_.graphics.windowHeight,
                               settings_.graphics.backend, true);
    if (!window_) {
        ps2::logError("frontend: cannot create window: %s", SDL_GetError());
        SDL_Quit();
        return m.run();
    }
    SDL_SetWindowMinimumSize(window_, 320, 240);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    const float scale = ui::displayScale(window_);
    ui::applyTheme(scale);
    fonts_ = ui::loadFonts(scale);
    presenter_ = createPresenter(settings_.graphics.backend, window_, settings_.graphics, presenterLog_);
    std::fputs(presenterLog_.c_str(), stderr);
    if (!presenter_) {
        ImGui::DestroyContext();
        SDL_DestroyWindow(window_);
        SDL_Quit();
        return m.run();
    }
    input_ = std::make_unique<InputManager>();
    applyWindowMode();
    SDL_ShowWindow(window_);

    // Audio device (48 kHz stereo S16, pulled from the SPU2 mixer queue).
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
    audio_ = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
        [](void* ud, SDL_AudioStream* s, int additional, int) {
            auto* self = static_cast<GameFrontend*>(ud);
            if (additional <= 0) return;
            const size_t frames = (size_t)additional / 4;
            std::vector<int16_t> buf(frames * 2);
            self->m_->spu2->pull(buf.data(), frames);
            SDL_PutAudioStreamData(s, buf.data(), (int)(buf.size() * sizeof(int16_t)));
        },
        this);
    if (audio_) SDL_ResumeAudioStreamDevice(audio_);
    else ps2::logWarn("frontend: no audio device: %s", SDL_GetError());

    syncRuntimeSettings();
    m.gs->onFrame = [this](const ps2::GsFrame& f) { onFrame(f); };
    m.onVsync = [this]() { onVsync(); };

    if (x_.controlStdin) {
        std::thread([this]() {
            std::string line;
            char buf[256];
            for (;;) {
#if defined(_WIN32)
                DWORD n = 0;
                HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
                if (!h || h == INVALID_HANDLE_VALUE || !ReadFile(h, buf, sizeof buf, &n, nullptr) || n == 0) return;
#else
                const ssize_t n = ::read(0, buf, sizeof buf);
                if (n <= 0) return;
#endif
                for (size_t i = 0; i < (size_t)n; ++i) {
                    if (buf[i] == '\n' || buf[i] == '\r') {
                        if (!line.empty()) {
                            std::lock_guard<std::mutex> lk(ctlMutex_);
                            ctlQueue_.push_back(line);
                        }
                        line.clear();
                    } else {
                        line.push_back(buf[i]);
                    }
                }
            }
        }).detach();
    }

    std::atomic<bool> emuDone{false};
    int rc = 0;
    std::thread emu([&]() {
        rc = m.run();
        emuDone = true;
        frameCv_.notify_all();
    });

    std::printf("state: running\n");
    std::fflush(stdout);
    std::vector<uint32_t> local;
    auto lastStatus = Clock::now();
    presentT0_ = Clock::now();
    bool finalShotTaken = x_.exitScreenshot.empty();
    bool exiting = false;
    while (true) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handleEvent(e);
        pollControl();
        if (input_->menuRequested()) menuWanted_ = !menuWanted_;
        if (menuOpen_ != menuWanted_) {
            menuOpen_ = menuWanted_;
            pauseCv_.notify_all();
            if (!menuWanted_) {
                input_->cancelCapture();
                saveSettings();
            }
        }
        if (quitRequested_ && !exiting) {
            exiting = true;
            m.requestExit(0);
            pauseCv_.notify_all();
            frameCv_.notify_all();
        }

        // Input -> pads.
        const bool menuVisible = menuOpen_.load();
        input_->update(settings_, !menuVisible && !ImGui::GetIO().WantCaptureKeyboard);
        for (int p = 0; p < 2; ++p) {
            ps2::PadState st;
            const PadReport& r = input_->report(p);
            st.buttons = menuVisible ? 0 : r.buttons;
            st.lx = menuVisible ? 0x80 : r.lx;
            st.ly = menuVisible ? 0x80 : r.ly;
            st.rx = menuVisible ? 0x80 : r.rx;
            st.ry = menuVisible ? 0x80 : r.ry;
            st.pressure = r.pressure;
            st.connected = settings_.ports[(size_t)p].enabled;
            st.analog = settings_.ports[(size_t)p].analogMode;
            m.pad->setState(p, st);
            const ps2::PadState cur = m.pad->state(p);
            if (!menuVisible && !emuPaused_) input_->setRumble(p, cur.motorSmall, cur.motorLarge, settings_.ports[(size_t)p]);
        }

        // New frame from the emulation thread?
        {
            std::unique_lock<std::mutex> lk(frameMutex_);
            if (frameSeq_ == uploadedSeq_ && !settings_.graphics.vsync && !menuVisible && !emuDone && !emuPaused_)
                frameCv_.wait_for(lk, std::chrono::milliseconds(8), [&] { return frameSeq_ != uploadedSeq_ || emuDone.load(); });
            if (frameSeq_ != uploadedSeq_ && frameW_ > 0) {
                local.swap(framePixels_);
                const int w = frameW_, h = frameH_;
                const bool il = frameInterlaced_;
                uploadedSeq_ = frameSeq_;
                lk.unlock();
                presenter_->uploadFrame(local.data(), w, h, il);
                lk.lock();
                framePixels_.swap(local);
            }
        }

        presenter_->newImGuiFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        drawOverlay();
        if (menuVisible) drawMenu();
        const bool hideCursor = settings_.graphics.fullscreen != FullscreenMode::Windowed && !menuVisible;
        if (hideCursor) SDL_HideCursor();
        else SDL_ShowCursor();
        ImGui::Render();

        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(window_, &ww, &wh);
        const RectF dst = computeDestRect(ww, wh, frameW_ ? frameW_ : 640, frameH_ ? frameH_ : 448, settings_.graphics.aspect,
                                          settings_.graphics.integerScale);
        const bool lastFrame = emuDone && !finalShotTaken;
        if (lastFrame && !screenshotPending_) takeScreenshot(x_.exitScreenshot);
        presenter_->render(dst, postParams(), true);
        presentedSeq_ = uploadedSeq_;
        frameCv_.notify_all();
        ++presentCount_;
        if (screenshotPending_) {
            std::vector<uint32_t> px;
            int sw = 0, sh = 0;
            if (presenter_->takeScreenshot(px, sw, sh)) {
                screenshotPending_ = false;
                const bool bmp = screenshotPath_.size() > 4 && screenshotPath_.substr(screenshotPath_.size() - 4) == ".bmp";
                const bool ok = bmp ? writeBmp(screenshotPath_, px.data(), sw, sh) : writePng(screenshotPath_, px.data(), sw, sh);
                notify(ok ? std::string(T("Screenshot disimpan: ", "Screenshot saved: ")) + screenshotPath_
                          : T("Gagal menyimpan screenshot", "Failed to save screenshot"));
                std::printf("screenshot: %s %s\n", ok ? "ok" : "failed", screenshotPath_.c_str());
                std::fflush(stdout);
                if (lastFrame) finalShotTaken = true;
            }
        }

        const auto now = Clock::now();
        const double pdt = std::chrono::duration<double>(now - presentT0_).count();
        if (pdt >= 0.5) {
            presentFps_ = presentCount_ / pdt;
            presentCount_ = 0;
            presentT0_ = now;
        }
        if (now - lastStatus > std::chrono::seconds(1)) {
            lastStatus = now;
            std::printf("status: vps=%.2f speed=%.1f fps=%.1f paused=%d\n", vps_.load(), speed_.load(), presentFps_,
                        (int)(userPaused_ || menuOpen_ || focusPaused_));
            std::fflush(stdout);
        }
        if (emuDone && finalShotTaken) break;
    }

    saveSettings();
    input_->stopRumble();
    if (emu.joinable()) emu.join();
    if (audio_) SDL_DestroyAudioStream(audio_);
    audio_ = nullptr;
    m.gs->onFrame = nullptr;
    m.onVsync = nullptr;
    input_.reset();
    presenter_.reset();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window_);
    SDL_Quit();
    std::printf("state: exited %d\n", rc);
    std::fflush(stdout);
    return rc;
}

} // namespace

int gameMain(int argc, char** argv, const ps2::ModuleInfo* mod) {
    std::vector<std::string> args;
#if defined(_WIN32)
    // Use the UTF-16 command line so paths with any characters work.
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    for (int i = 0; i < wargc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s((size_t)std::max(0, n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
        args.push_back(s);
    }
    LocalFree(wargv);
    (void)argc;
    (void)argv;
#else
    for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
#endif
    LaunchExtras x;
    std::vector<std::string> rest;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--title") x.title = next();
        else if (a == "--control-stdin") x.controlStdin = true;
        else if (a == "--screenshot-dir") x.screenshotDir = next();
        else if (a == "--exit-screenshot") x.exitScreenshot = next();
        else rest.push_back(a);
    }
    std::vector<char*> cargv;
    for (auto& s : rest) cargv.push_back(s.data());
    cargv.push_back(nullptr);
    GameFrontend fe(x);
    ps2::setFrontend(&fe);
    const int rc = ps2::hostMain((int)rest.size(), cargv.data(), mod);
    ps2::setFrontend(nullptr);
    return rc;
}

} // namespace sb2
