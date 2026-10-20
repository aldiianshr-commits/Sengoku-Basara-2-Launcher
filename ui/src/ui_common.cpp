#include "sb2/ui_common.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "sb2/input.hpp"
#include "sb2/platform.hpp"

namespace sb2::ui {

namespace {
std::string g_lang = "id";
}

ImVec4 kAccent(0.86f, 0.20f, 0.24f, 1.0f);
ImVec4 kAccentSoft(0.86f, 0.20f, 0.24f, 0.25f);
ImVec4 kGood(0.30f, 0.78f, 0.47f, 1.0f);
ImVec4 kWarn(0.95f, 0.70f, 0.25f, 1.0f);
ImVec4 kBad(0.92f, 0.33f, 0.33f, 1.0f);
ImVec4 kMuted(0.60f, 0.62f, 0.66f, 1.0f);

void setLanguage(const std::string& lang) { g_lang = lang == "en" ? "en" : "id"; }
const std::string& language() { return g_lang; }
const char* T(const char* id, const char* en) { return g_lang == "en" ? en : id; }

float displayScale(SDL_Window* w) {
    float s = w ? SDL_GetWindowDisplayScale(w) : 1.0f;
    if (s <= 0.0f) s = 1.0f;
    return std::clamp(s, 1.0f, 3.0f);
}

void applyTheme(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 10.0f;
    st.ChildRounding = 10.0f;
    st.FrameRounding = 7.0f;
    st.PopupRounding = 8.0f;
    st.GrabRounding = 7.0f;
    st.TabRounding = 7.0f;
    st.ScrollbarRounding = 8.0f;
    st.WindowBorderSize = 0.0f;
    st.ChildBorderSize = 0.0f;
    st.FrameBorderSize = 0.0f;
    st.PopupBorderSize = 1.0f;
    st.WindowPadding = ImVec2(18, 16);
    st.FramePadding = ImVec2(10, 7);
    st.ItemSpacing = ImVec2(10, 9);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.ScrollbarSize = 12.0f;
    st.GrabMinSize = 12.0f;
    st.SeparatorTextBorderSize = 1.0f;
    ImVec4* c = st.Colors;
    const ImVec4 bg(0.075f, 0.078f, 0.090f, 1.0f);
    const ImVec4 panel(0.105f, 0.110f, 0.125f, 1.0f);
    const ImVec4 frame(0.150f, 0.157f, 0.176f, 1.0f);
    const ImVec4 frameHi(0.195f, 0.204f, 0.228f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.93f, 0.95f, 1.0f);
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.125f, 0.14f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = frameHi;
    c[ImGuiCol_FrameBgActive] = frameHi;
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = bg;
    c[ImGuiCol_MenuBarBg] = panel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.20f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.28f);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.95f, 0.30f, 0.33f, 1.0f);
    c[ImGuiCol_Button] = frame;
    c[ImGuiCol_ButtonHovered] = frameHi;
    c[ImGuiCol_ButtonActive] = kAccentSoft;
    c[ImGuiCol_Header] = ImVec4(0.86f, 0.20f, 0.24f, 0.22f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.86f, 0.20f, 0.24f, 0.32f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.86f, 0.20f, 0.24f, 0.42f);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabHovered] = frameHi;
    c[ImGuiCol_TabSelected] = frame;
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_TableHeaderBg] = panel;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    c[ImGuiCol_NavCursor] = kAccent;
    st.ScaleAllSizes(scale);
    st.FontScaleDpi = scale;
}

Fonts loadFonts(float) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    Fonts f;
    const float base = 17.0f;
#if defined(_WIN32)
    std::string winDir = "C:/Windows";
    if (const char* w = std::getenv("WINDIR")) winDir = w;
    const std::string fontDir = joinPath(winDir, "Fonts");
    const char* bodyFonts[] = {"segoeui.ttf", "arial.ttf"};
    const char* headFonts[] = {"seguisb.ttf", "segoeuib.ttf", "segoeui.ttf"};
    const char* cjkFonts[] = {"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc", "malgun.ttf"};
#else
    const std::string fontDir = "/usr/share/fonts/truetype";
    const char* bodyFonts[] = {"dejavu/DejaVuSans.ttf", "liberation/LiberationSans-Regular.ttf"};
    const char* headFonts[] = {"dejavu/DejaVuSans-Bold.ttf", "dejavu/DejaVuSans.ttf"};
    const char* cjkFonts[] = {"fonts-japanese-gothic.ttf", "wqy/wqy-microhei.ttc"};
#endif
    auto first = [&](const char* const* list, size_t n) -> std::string {
        for (size_t i = 0; i < n; ++i) {
            const std::string p = joinPath(fontDir, list[i]);
            if (fileExists(p)) return p;
        }
        return {};
    };
    const std::string body = first(bodyFonts, std::size(bodyFonts));
    const std::string head = first(headFonts, std::size(headFonts));
    const std::string cjk = first(cjkFonts, std::size(cjkFonts));
    auto add = [&](const std::string& path, float size) -> ImFont* {
        ImFont* font = nullptr;
        if (!path.empty()) font = io.Fonts->AddFontFromFileTTF(path.c_str(), size);
        if (!font) {
            ImFontConfig cfg;
            cfg.SizePixels = size;
            font = io.Fonts->AddFontDefault(&cfg);
        }
        if (!cjk.empty()) {
            ImFontConfig m;
            m.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(cjk.c_str(), size, &m);
        }
        return font;
    };
    f.body = add(body, base);
    f.heading = add(head, base * 1.25f);
    f.title = add(head, base * 1.9f);
    io.FontDefault = f.body;
    return f;
}

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool comboInt(const char* label, int* v, const char* const* items, int count) {
    const int cur = std::clamp(*v, 0, count - 1);
    bool changed = false;
    if (ImGui::BeginCombo(label, items[cur])) {
        for (int i = 0; i < count; ++i) {
            const bool sel = i == cur;
            if (ImGui::Selectable(items[i], sel)) {
                *v = i;
                changed = i != cur;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void sectionTitle(const char* text) {
    ImGui::Spacing();
    ImGui::SeparatorText(text);
}

void statusPill(const char* text, const ImVec4& color) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 pad(8, 3);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg = color;
    bg.w = 0.18f;
    dl->AddRectFilled(p, ImVec2(p.x + sz.x + pad.x * 2, p.y + sz.y + pad.y * 2), ImGui::GetColorU32(bg), 20.0f);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ImGui::GetColorU32(color), text);
    ImGui::Dummy(ImVec2(sz.x + pad.x * 2, sz.y + pad.y * 2));
}

// ------------------------------------------------------------------ editors

bool editGraphics(GraphicsSettings& g, const PresenterCaps& caps, bool inGame) {
    bool ch = false;
    ImGui::PushItemWidth(ImGui::GetFontSize() * 16);
    sectionTitle(T("Tampilan", "Display"));
    if (!inGame) {
        const char* backends[] = {T("Otomatis (Direct3D 12)", "Automatic (Direct3D 12)"), "Direct3D 12",
                                  T("SDL Renderer (kompatibilitas)", "SDL Renderer (compatibility)")};
        ch |= comboEnum(T("Renderer", "Renderer"), &g.backend, backends, 3);
        helpMarker(T("Direct3D 12 menjalankan semua filter dan efek di GPU. SDL Renderer adalah cadangan untuk driver lama.",
                     "Direct3D 12 runs all filters and effects on the GPU. SDL Renderer is a fallback for old drivers."));
        static const int kRes[][2] = {{960, 720}, {1280, 960}, {1440, 1080}, {1920, 1440}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
        char cur[32];
        std::snprintf(cur, sizeof cur, "%d x %d", g.windowWidth, g.windowHeight);
        if (ImGui::BeginCombo(T("Ukuran jendela", "Window size"), cur)) {
            for (auto& r : kRes) {
                char b[32];
                std::snprintf(b, sizeof b, "%d x %d", r[0], r[1]);
                if (ImGui::Selectable(b, g.windowWidth == r[0] && g.windowHeight == r[1])) {
                    g.windowWidth = r[0];
                    g.windowHeight = r[1];
                    ch = true;
                }
            }
            ImGui::EndCombo();
        }
    }
    const char* fs[] = {T("Jendela", "Windowed"), T("Layar penuh tanpa batas", "Borderless fullscreen"),
                        T("Layar penuh eksklusif", "Exclusive fullscreen")};
    ch |= comboEnum(T("Mode layar", "Screen mode"), &g.fullscreen, fs, 3);
    if (g.fullscreen == FullscreenMode::Exclusive && !inGame) {
        int n = 0;
        SDL_DisplayID* ids = SDL_GetDisplays(&n);
        if (ids && n > 0) {
            const SDL_DisplayID d = ids[std::min(g.display, n - 1)];
            int nm = 0;
            SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(d, &nm);
            char cur2[48];
            if (g.exclusiveWidth == 0) std::snprintf(cur2, sizeof cur2, "%s", T("Resolusi desktop", "Desktop resolution"));
            else std::snprintf(cur2, sizeof cur2, "%dx%d @ %d Hz", g.exclusiveWidth, g.exclusiveHeight, g.exclusiveRefresh);
            if (ImGui::BeginCombo(T("Resolusi layar penuh", "Fullscreen resolution"), cur2)) {
                if (ImGui::Selectable(T("Resolusi desktop", "Desktop resolution"), g.exclusiveWidth == 0)) {
                    g.exclusiveWidth = g.exclusiveHeight = g.exclusiveRefresh = 0;
                    ch = true;
                }
                for (int i = 0; modes && i < nm; ++i) {
                    char b[48];
                    const int hz = (int)(modes[i]->refresh_rate + 0.5f);
                    std::snprintf(b, sizeof b, "%dx%d @ %d Hz", modes[i]->w, modes[i]->h, hz);
                    if (ImGui::Selectable(b, g.exclusiveWidth == modes[i]->w && g.exclusiveHeight == modes[i]->h && g.exclusiveRefresh == hz)) {
                        g.exclusiveWidth = modes[i]->w;
                        g.exclusiveHeight = modes[i]->h;
                        g.exclusiveRefresh = hz;
                        ch = true;
                    }
                }
                ImGui::EndCombo();
            }
            SDL_free(modes);
        }
        SDL_free(ids);
    }
    if (!inGame) {
        int n = 0;
        SDL_DisplayID* ids = SDL_GetDisplays(&n);
        if (n > 1) {
            std::string cur3 = std::to_string(g.display + 1);
            if (ImGui::BeginCombo(T("Monitor", "Monitor"), cur3.c_str())) {
                for (int i = 0; i < n; ++i) {
                    const char* nm = SDL_GetDisplayName(ids[i]);
                    std::string label = std::to_string(i + 1) + ": " + (nm ? nm : "");
                    if (ImGui::Selectable(label.c_str(), g.display == i)) {
                        g.display = i;
                        ch = true;
                    }
                }
                ImGui::EndCombo();
            }
        }
        SDL_free(ids);
    }
    const char* aspects[] = {T("Otomatis (4:3)", "Auto (4:3)"), "4:3", "16:9", T("Regangkan", "Stretch")};
    ch |= comboEnum(T("Rasio aspek", "Aspect ratio"), &g.aspect, aspects, 4);
    ch |= ImGui::Checkbox(T("Skala bilangan bulat", "Integer scaling"), &g.integerScale);
    helpMarker(T("Memperbesar gambar dengan kelipatan bulat agar piksel tetap tajam.",
                 "Scales the image by whole multiples so pixels stay crisp."));

    sectionTitle(T("Sinkronisasi", "Synchronisation"));
    ch |= ImGui::Checkbox("VSync", &g.vsync);
    if (!g.vsync && caps.tearing) ch |= ImGui::Checkbox(T("Izinkan tearing (latensi terendah)", "Allow tearing (lowest latency)"), &g.allowTearing);
    const char* pacing[] = {T("Timer presisi (59,94 Hz)", "Precise timer (59.94 Hz)"), T("Ikuti VSync monitor", "Follow display VSync"),
                            T("Tanpa batas (turbo)", "Unthrottled (turbo)")};
    ch |= comboEnum(T("Frame pacing", "Frame pacing"), &g.pacing, pacing, 3);
    helpMarker(T("Timer presisi menjaga kecepatan asli PS2 dan menyinkronkan audio. 'Ikuti VSync' menghilangkan stutter di monitor 60 Hz (game sedikit lebih cepat 0,1%).",
                 "The precise timer keeps the original PS2 speed and syncs audio. 'Follow display VSync' removes stutter on 60 Hz monitors (game runs 0.1% fast)."));
    {
        static const int kFps[] = {0, 30, 50, 60, 75, 90, 120};
        const char* labels[] = {T("Asli konsol", "Console native"), "30", "50", "60", "75", "90", "120"};
        int idx = 0;
        for (int i = 0; i < 7; ++i)
            if (kFps[i] == g.fpsLimit) idx = i;
        if (comboInt(T("Batas FPS", "FPS limit"), &idx, labels, 7)) {
            g.fpsLimit = kFps[idx];
            ch = true;
        }
        helpMarker(T("Mengatur laju vblank yang dilihat game. Logika game PS2 terikat ke laju ini, jadi nilai di atas 60 juga mempercepat permainan.",
                     "Sets the vblank rate seen by the game. PS2 game logic is tied to it, so values above 60 also speed up gameplay."));
    }
    if (!inGame) {
        ch |= ImGui::SliderInt(T("Latensi frame maks.", "Max frame latency"), &g.maxFrameLatency, 1, 3);
    }

    sectionTitle(T("Kualitas gambar", "Image quality"));
    const char* filters[] = {T("Nearest (piksel tajam)", "Nearest (sharp pixels)"), "Bilinear", T("Sharp bilinear", "Sharp bilinear"),
                             T("Bicubic (Catmull-Rom)", "Bicubic (Catmull-Rom)"), "Lanczos"};
    const int nFilters = caps.shaderFilters ? 5 : 2;
    if (!caps.shaderFilters && (int)g.filter > 1) g.filter = PresentFilter::Bilinear;
    ch |= comboEnum(T("Filter skala layar", "Screen scaling filter"), &g.filter, filters, nFilters);
    const char* tf[] = {T("Sesuai game", "As the game requests"), T("Paksa bilinear", "Force bilinear"), T("Paksa nearest", "Force nearest")};
    ch |= comboEnum(T("Filter tekstur", "Texture filtering"), &g.textureFiltering, tf, 3);
    helpMarker(T("Diterapkan langsung di rasterizer GS saat menggambar tekstur 3D.",
                 "Applied directly in the GS rasterizer when sampling 3D textures."));
    if (caps.shaderFilters) {
        const char* aa[] = {T("Mati", "Off"), "FXAA"};
        ch |= comboEnum(T("Anti-aliasing", "Anti-aliasing"), &g.antiAliasing, aa, 2);
    }
    ImGui::BeginDisabled();
    int aniso = 0;
    const char* anisoItems[] = {T("Tidak tersedia", "Not available")};
    comboInt(T("Filter anisotropik", "Anisotropic filtering"), &aniso, anisoItems, 1);
    ImGui::EndDisabled();
    helpMarker(T("Membutuhkan renderer GS berbasis GPU. Rasterizer GS saat ini berbasis perangkat lunak (akurat seperti hardware asli), jadi anisotropik tidak dapat diterapkan.",
                 "Requires a GPU-based GS renderer. The current GS rasterizer is software based (hardware-accurate), so anisotropic filtering cannot be applied."));
    const char* deint[] = {T("Mati", "Off"), T("Campur field (anti-kedip)", "Blend fields (anti-flicker)")};
    ch |= comboEnum(T("Deinterlace", "Deinterlace"), &g.deinterlace, deint, caps.shaderFilters ? 2 : 1);

    sectionTitle(T("Informasi di layar", "On-screen display"));
    ch |= ImGui::Checkbox(T("Tampilkan FPS", "Show FPS"), &g.showFps);
    ImGui::SameLine();
    ch |= ImGui::Checkbox(T("Statistik", "Statistics"), &g.showStats);
    ch |= ImGui::Checkbox(T("Jeda saat jendela tidak aktif", "Pause when the window loses focus"), &g.pauseOnFocusLoss);
    ImGui::PopItemWidth();
    return ch;
}

bool editAdvancedGraphics(GraphicsSettings& g, const PresenterCaps& caps, bool inGame) {
    bool ch = false;
    ImGui::PushItemWidth(ImGui::GetFontSize() * 16);
    if (!caps.shaderFilters) {
        ImGui::TextColored(kWarn, "%s", T("Efek shader membutuhkan renderer Direct3D 12.", "Shader effects require the Direct3D 12 renderer."));
        ImGui::BeginDisabled();
    }
    sectionTitle(T("Efek shader", "Shader effects"));
    ch |= ImGui::SliderInt(T("Ketajaman (CAS)", "Sharpening (CAS)"), &g.sharpen, 0, 100, "%d%%");
    ch |= ImGui::Checkbox(T("Scanline CRT", "CRT scanlines"), &g.scanlines);
    if (g.scanlines) ch |= ImGui::SliderInt(T("Kekuatan scanline", "Scanline strength"), &g.scanlineStrength, 0, 100, "%d%%");
    sectionTitle(T("Warna", "Colour"));
    ch |= ImGui::SliderFloat(T("Kecerahan", "Brightness"), &g.brightness, 0.5f, 1.5f, "%.2f");
    ch |= ImGui::SliderFloat(T("Kontras", "Contrast"), &g.contrast, 0.5f, 1.5f, "%.2f");
    ch |= ImGui::SliderFloat("Gamma", &g.gamma, 0.5f, 2.0f, "%.2f");
    ch |= ImGui::SliderFloat(T("Saturasi", "Saturation"), &g.saturation, 0.0f, 2.0f, "%.2f");
    if (ImGui::Button(T("Reset warna", "Reset colour"))) {
        g.brightness = g.contrast = g.gamma = g.saturation = 1.0f;
        ch = true;
    }
    if (!caps.shaderFilters) ImGui::EndDisabled();
    sectionTitle(T("Rendering GS", "GS rendering"));
    if (!inGame) {
        const int maxT = std::min(32, cpuThreads());
        ch |= ImGui::SliderInt(T("Thread rasterizer", "Rasterizer threads"), &g.rasterThreads, 0, maxT,
                               g.rasterThreads == 0 ? T("Otomatis", "Automatic") : "%d");
        helpMarker(T("Jumlah thread CPU untuk rasterizer GS perangkat lunak. Otomatis = jumlah core dikurangi thread game.",
                     "CPU threads used by the software GS rasterizer. Automatic = core count minus the game thread."));
    } else {
        ImGui::TextDisabled("%s", T("Thread rasterizer dapat diubah dari launcher.", "Rasterizer threads can be changed from the launcher."));
    }
    ImGui::PopItemWidth();
    return ch;
}

bool editAudio(AudioSettings& a) {
    bool ch = false;
    ImGui::PushItemWidth(ImGui::GetFontSize() * 16);
    sectionTitle("Audio");
    ch |= ImGui::Checkbox(T("Aktifkan audio", "Enable audio"), &a.enabled);
    ch |= ImGui::SliderInt(T("Volume", "Volume"), &a.volume, 0, 100, "%d%%");
    ch |= ImGui::SliderInt(T("Buffer audio", "Audio buffer"), &a.latencyMs, 16, 250, "%d ms");
    ch |= ImGui::Checkbox(T("Sinkronkan kecepatan ke audio", "Sync speed to audio"), &a.syncToAudio);
    helpMarker(T("Menyesuaikan frame pacing ±0,5% berdasarkan isi buffer audio agar tidak ada suara putus-putus.",
                 "Nudges frame pacing by ±0.5% based on the audio buffer level to avoid crackling."));
    ImGui::PopItemWidth();
    return ch;
}

bool editControls(Settings& s, InputManager& input, int& port) {
    bool ch = false;
    static int capturing = -1;
    static int captureSlot = 0; // 0 = key, 1 = alternate key, 2 = gamepad
    if (ImGui::BeginTabBar("ports")) {
        for (int p = 0; p < 2; ++p) {
            char lbl[32];
            std::snprintf(lbl, sizeof lbl, "%s %d", T("Port", "Port"), p + 1);
            if (ImGui::BeginTabItem(lbl)) {
                if (port != p) capturing = -1;
                port = p;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    PortSettings& ps = s.ports[(size_t)port];
    ImGui::PushItemWidth(ImGui::GetFontSize() * 14);
    ch |= ImGui::Checkbox(T("Kontroler terhubung", "Controller connected"), &ps.enabled);
    const auto pads = input.gamepads();
    std::string cur;
    if (ps.device == -2) cur = T("Otomatis", "Automatic");
    else if (ps.device == -1) cur = T("Keyboard saja", "Keyboard only");
    else cur = std::string(T("Gamepad ", "Gamepad ")) + std::to_string(ps.device + 1);
    if (ImGui::BeginCombo(T("Perangkat", "Device"), cur.c_str())) {
        if (ImGui::Selectable(T("Otomatis", "Automatic"), ps.device == -2)) ps.device = -2, ch = true;
        if (ImGui::Selectable(T("Keyboard saja", "Keyboard only"), ps.device == -1)) ps.device = -1, ch = true;
        for (const auto& d : pads) {
            std::string l = std::to_string(d.slot + 1) + ": " + d.name;
            if (ImGui::Selectable(l.c_str(), ps.device == d.slot)) ps.device = d.slot, ch = true;
        }
        ImGui::EndCombo();
    }
    const int slot = input.padForPort(s, port);
    if (slot >= 0 && slot < (int)pads.size()) {
        ImGui::TextColored(kGood, "%s %s", T("Aktif:", "Active:"), pads[(size_t)slot].name.c_str());
    } else {
        ImGui::TextColored(kMuted, "%s", T("Tidak ada gamepad untuk port ini (keyboard dipakai bila ada binding).",
                                           "No gamepad for this port (keyboard used where bound)."));
    }
    int npres = 0;
    const char* const* presets = inputPresetNames(&npres);
    int pi = 0;
    for (int i = 0; i < npres; ++i)
        if (ps.preset == presets[i]) pi = i;
    const char* presetLabels[] = {T("Standar (WASD + JKLI)", "Default (WASD + JKLI)"), T("Southpaw (stik ditukar)", "Southpaw (sticks swapped)"),
                                  T("Panah + ZXCV", "Arrows + ZXCV")};
    if (comboInt(T("Preset", "Preset"), &pi, presetLabels, npres)) {
        applyInputPreset(ps, presets[pi], port);
        capturing = -1;
        ch = true;
    }
    ch |= ImGui::SliderFloat(T("Deadzone analog", "Analog deadzone"), &ps.deadzone, 0.0f, 0.6f, "%.2f");
    ch |= ImGui::SliderFloat(T("Sensitivitas analog", "Analog sensitivity"), &ps.sensitivity, 0.5f, 2.0f, "%.2fx");
    ch |= ImGui::SliderFloat(T("Ambang trigger", "Trigger threshold"), &ps.triggerThreshold, 0.05f, 0.9f, "%.2f");
    ch |= ImGui::Checkbox(T("Getaran", "Vibration"), &ps.vibration);
    if (ps.vibration) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ch |= ImGui::SliderFloat("##vib", &ps.vibrationStrength, 0.0f, 1.0f, "%.2f");
    }
    ch |= ImGui::Checkbox(T("Mode analog DualShock 2", "DualShock 2 analog mode"), &ps.analogMode);
    ImGui::PopItemWidth();

    // Live preview of the current report.
    const PadReport& r = input.report(port);
    ImGui::TextDisabled("%s LX %3u LY %3u  RX %3u RY %3u  %s %04X", T("Pratinjau:", "Preview:"), r.lx, r.ly, r.rx, r.ry,
                        T("tombol", "buttons"), r.buttons);

    sectionTitle(T("Pemetaan tombol", "Button mapping"));
    CapturedInput got;
    if (capturing >= 0 && input.pollCapture(got)) {
        Binding& b = ps.bind[(size_t)capturing];
        if (got.kind == CapturedInput::Key) {
            if (got.code == 41 /* Escape */) {
                // Escape cancels
            } else if (captureSlot == 1) b.key2 = got.code;
            else b.key = got.code;
        } else if (got.kind == CapturedInput::Button) {
            b.button = got.code;
            b.axis = -1;
        } else if (got.kind == CapturedInput::Axis) {
            b.axis = got.code;
            b.axisDir = got.dir;
            b.button = -1;
        }
        ps.preset = "custom";
        capturing = -1;
        ch = true;
    }
    if (ImGui::BeginTable("binds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetFontSize() * 18))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(T("Input PS2", "PS2 input"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn(T("Keyboard", "Keyboard"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(T("Alternatif", "Alternate"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Gamepad", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < IN_COUNT; ++i) {
            Binding& b = ps.bind[(size_t)i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(padInputName(i));
            for (int col = 0; col < 3; ++col) {
                ImGui::TableSetColumnIndex(col + 1);
                ImGui::PushID(i * 4 + col);
                std::string txt;
                if (capturing == i && captureSlot == col) txt = T("Tekan input...", "Press input...");
                else if (col == 0) txt = b.key ? InputManager::keyName(b.key) : "-";
                else if (col == 1) txt = b.key2 ? InputManager::keyName(b.key2) : "-";
                else txt = b.button >= 0 ? InputManager::buttonName(b.button) : (b.axis >= 0 ? InputManager::axisName(b.axis, b.axisDir) : "-");
                if (ImGui::Button(txt.c_str(), ImVec2(-FLT_MIN, 0))) {
                    capturing = i;
                    captureSlot = col;
                    input.beginCapture();
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem(T("Hapus", "Clear"))) {
                        if (col == 0) b.key = 0;
                        else if (col == 1) b.key2 = 0;
                        else b.button = -1, b.axis = -1;
                        ps.preset = "custom";
                        ch = true;
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%s", T("Klik untuk mengubah, klik kanan untuk menghapus. Esc membatalkan.",
                                "Click to change, right-click to clear. Esc cancels."));
    return ch;
}

bool editSystem(SystemSettings& sys) {
    bool ch = false;
    ImGui::PushItemWidth(ImGui::GetFontSize() * 16);
    sectionTitle(T("Bahasa", "Language"));
    int lang = sys.language == "en" ? 1 : 0;
    const char* langs[] = {"Bahasa Indonesia", "English"};
    if (comboInt(T("Bahasa antarmuka", "Interface language"), &lang, langs, 2)) {
        sys.language = lang == 1 ? "en" : "id";
        setLanguage(sys.language);
        ch = true;
    }
    sectionTitle(T("Recompilation", "Recompilation"));
    const int maxJ = cpuThreads();
    ch |= ImGui::SliderInt(T("Job kompilasi paralel", "Parallel compile jobs"), &sys.compileJobs, 0, maxJ,
                           sys.compileJobs == 0 ? T("Otomatis", "Automatic") : "%d");
    const char* opt[] = {"-O0", "-O1", T("-O2 (disarankan)", "-O2 (recommended)"), "-O3"};
    ch |= comboInt(T("Optimisasi kode hasil recompile", "Recompiled code optimisation"), &sys.optimization, opt, 4);
    ch |= ImGui::Checkbox(T("Recompile ulang otomatis bila ditemukan entry point baru", "Automatically recompile when new entry points are found"),
                          &sys.autoRebuildMissing);
    sectionTitle(T("Log", "Logging"));
    const char* lv[] = {T("Error", "Errors"), T("Peringatan", "Warnings"), "Info", "Debug"};
    ch |= comboInt(T("Tingkat log", "Log level"), &sys.logLevel, lv, 4);
    ImGui::PopItemWidth();
    return ch;
}

} // namespace sb2::ui
