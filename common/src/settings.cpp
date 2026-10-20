#include "sb2/settings.hpp"

#include <algorithm>
#include <cmath>

namespace sb2 {

namespace {

// SDL3 enum values (stable ABI: USB HID usage ids for scancodes).  The
// front-end static_asserts these against the SDL headers.
enum : int {
    SC_A = 4, SC_B, SC_C, SC_D, SC_E, SC_F, SC_G, SC_H, SC_I, SC_J, SC_K, SC_L, SC_M,
    SC_N, SC_O, SC_P, SC_Q, SC_R, SC_S, SC_T, SC_U, SC_V, SC_W, SC_X, SC_Y, SC_Z,
    SC_RETURN = 40, SC_ESCAPE = 41, SC_BACKSPACE = 42, SC_TAB = 43, SC_SPACE = 44,
    SC_RIGHT = 79, SC_LEFT = 80, SC_DOWN = 81, SC_UP = 82,
    SC_KP_2 = 90, SC_KP_4 = 92, SC_KP_5 = 93, SC_KP_6 = 94, SC_KP_8 = 96,
    SC_LCTRL = 224, SC_LSHIFT = 225, SC_LALT = 226, SC_RCTRL = 228, SC_RSHIFT = 229,
};
enum : int {
    GB_SOUTH = 0, GB_EAST, GB_WEST, GB_NORTH, GB_BACK, GB_GUIDE, GB_START, GB_LSTICK, GB_RSTICK,
    GB_LSHOULDER, GB_RSHOULDER, GB_DPAD_UP, GB_DPAD_DOWN, GB_DPAD_LEFT, GB_DPAD_RIGHT,
};
enum : int { GA_LEFTX = 0, GA_LEFTY, GA_RIGHTX, GA_RIGHTY, GA_LTRIGGER, GA_RTRIGGER };

struct InputInfo {
    const char* id;
    const char* name;
};
constexpr InputInfo kInputs[IN_COUNT] = {
    {"up", "D-Pad Up"}, {"down", "D-Pad Down"}, {"left", "D-Pad Left"}, {"right", "D-Pad Right"},
    {"cross", "Cross"}, {"circle", "Circle"}, {"square", "Square"}, {"triangle", "Triangle"},
    {"l1", "L1"}, {"r1", "R1"}, {"l2", "L2"}, {"r2", "R2"}, {"l3", "L3"}, {"r3", "R3"},
    {"select", "Select"}, {"start", "Start"},
    {"lx_neg", "Left Stick Left"}, {"lx_pos", "Left Stick Right"}, {"ly_neg", "Left Stick Up"}, {"ly_pos", "Left Stick Down"},
    {"rx_neg", "Right Stick Left"}, {"rx_pos", "Right Stick Right"}, {"ry_neg", "Right Stick Up"}, {"ry_pos", "Right Stick Down"},
    {"analog_mod", "Walk Modifier (keyboard)"},
};

const char* const kPresetNames[] = {"default", "southpaw", "arrows"};

Binding B(int key, int button, int axis = -1, int dir = 1, int key2 = 0) {
    Binding b;
    b.key = key;
    b.key2 = key2;
    b.button = button;
    b.axis = axis;
    b.axisDir = dir;
    return b;
}

template <typename T>
T clampv(T v, T lo, T hi) {
    return std::min(hi, std::max(lo, v));
}

template <typename E>
E enumFrom(const Json& j, E def, int maxv) {
    if (!j.isNumber()) return def;
    const int v = (int)j.asInt();
    return (v < 0 || v > maxv) ? def : (E)v;
}

} // namespace

const char* padInputName(int i) { return (i >= 0 && i < IN_COUNT) ? kInputs[i].name : "?"; }
const char* padInputId(int i) { return (i >= 0 && i < IN_COUNT) ? kInputs[i].id : "?"; }

const char* const* inputPresetNames(int* count) {
    *count = (int)(sizeof kPresetNames / sizeof kPresetNames[0]);
    return kPresetNames;
}

bool applyInputPreset(PortSettings& p, const std::string& preset, int port) {
    std::array<Binding, IN_COUNT> b{};
    // Gamepad part (positional face buttons, like a DualShock).
    b[IN_UP] = B(0, GB_DPAD_UP);
    b[IN_DOWN] = B(0, GB_DPAD_DOWN);
    b[IN_LEFT] = B(0, GB_DPAD_LEFT);
    b[IN_RIGHT] = B(0, GB_DPAD_RIGHT);
    b[IN_CROSS] = B(0, GB_SOUTH);
    b[IN_CIRCLE] = B(0, GB_EAST);
    b[IN_SQUARE] = B(0, GB_WEST);
    b[IN_TRIANGLE] = B(0, GB_NORTH);
    b[IN_L1] = B(0, GB_LSHOULDER);
    b[IN_R1] = B(0, GB_RSHOULDER);
    b[IN_L2] = B(0, -1, GA_LTRIGGER, 1);
    b[IN_R2] = B(0, -1, GA_RTRIGGER, 1);
    b[IN_L3] = B(0, GB_LSTICK);
    b[IN_R3] = B(0, GB_RSTICK);
    b[IN_SELECT] = B(0, GB_BACK);
    b[IN_START] = B(0, GB_START);
    int lx = GA_LEFTX, ly = GA_LEFTY, rx = GA_RIGHTX, ry = GA_RIGHTY;
    if (preset == "southpaw") {
        std::swap(lx, rx);
        std::swap(ly, ry);
    } else if (preset != "default" && preset != "arrows") {
        return false;
    }
    b[IN_LX_NEG] = B(0, -1, lx, -1);
    b[IN_LX_POS] = B(0, -1, lx, 1);
    b[IN_LY_NEG] = B(0, -1, ly, -1);
    b[IN_LY_POS] = B(0, -1, ly, 1);
    b[IN_RX_NEG] = B(0, -1, rx, -1);
    b[IN_RX_POS] = B(0, -1, rx, 1);
    b[IN_RY_NEG] = B(0, -1, ry, -1);
    b[IN_RY_POS] = B(0, -1, ry, 1);
    b[IN_ANALOG_MOD] = B(0, -1);

    // Keyboard part: only the first port gets keys by default.
    if (port == 0) {
        auto key = [&](int in, int k, int k2 = 0) {
            b[in].key = k;
            b[in].key2 = k2;
        };
        if (preset == "arrows") {
            key(IN_LX_NEG, SC_LEFT);
            key(IN_LX_POS, SC_RIGHT);
            key(IN_LY_NEG, SC_UP);
            key(IN_LY_POS, SC_DOWN);
            key(IN_SQUARE, SC_Z);
            key(IN_CROSS, SC_X, SC_SPACE);
            key(IN_CIRCLE, SC_C);
            key(IN_TRIANGLE, SC_V);
            key(IN_L1, SC_A);
            key(IN_R1, SC_S);
            key(IN_L2, SC_Q);
            key(IN_R2, SC_W);
            key(IN_UP, SC_I);
            key(IN_DOWN, SC_K);
            key(IN_LEFT, SC_J);
            key(IN_RIGHT, SC_L);
            key(IN_L3, SC_D);
            key(IN_R3, SC_F);
        } else {
            key(IN_LX_NEG, SC_A);
            key(IN_LX_POS, SC_D);
            key(IN_LY_NEG, SC_W);
            key(IN_LY_POS, SC_S);
            key(IN_SQUARE, SC_J);
            key(IN_CROSS, SC_K, SC_SPACE);
            key(IN_CIRCLE, SC_L);
            key(IN_TRIANGLE, SC_I);
            key(IN_L1, SC_U);
            key(IN_R1, SC_O);
            key(IN_L2, SC_Q);
            key(IN_R2, SC_E);
            key(IN_UP, SC_UP);
            key(IN_DOWN, SC_DOWN);
            key(IN_LEFT, SC_LEFT);
            key(IN_RIGHT, SC_RIGHT);
            key(IN_L3, SC_Z);
            key(IN_R3, SC_X);
        }
        key(IN_RX_NEG, SC_KP_4);
        key(IN_RX_POS, SC_KP_6);
        key(IN_RY_NEG, SC_KP_8);
        key(IN_RY_POS, SC_KP_5, SC_KP_2);
        key(IN_SELECT, SC_BACKSPACE, SC_TAB);
        key(IN_START, SC_RETURN);
        key(IN_ANALOG_MOD, SC_LSHIFT);
    }
    p.bind = b;
    p.preset = preset;
    return true;
}

Settings::Settings() {
    for (int i = 0; i < 2; ++i) {
        applyInputPreset(ports[i], "default", i);
        ports[i].device = i == 0 ? -2 : 1;
        ports[i].enabled = i == 0;
    }
}

void Settings::clamp() {
    auto& g = graphics;
    g.windowWidth = clampv(g.windowWidth, 320, 7680);
    g.windowHeight = clampv(g.windowHeight, 240, 4320);
    g.display = clampv(g.display, 0, 15);
    g.fpsLimit = clampv(g.fpsLimit, 0, 240);
    g.maxFrameLatency = clampv(g.maxFrameLatency, 1, 3);
    g.anisotropy = clampv(g.anisotropy, 0, 16);
    g.sharpen = clampv(g.sharpen, 0, 100);
    g.scanlineStrength = clampv(g.scanlineStrength, 0, 100);
    g.brightness = clampv(g.brightness, 0.5f, 1.5f);
    g.contrast = clampv(g.contrast, 0.5f, 1.5f);
    g.gamma = clampv(g.gamma, 0.5f, 2.0f);
    g.saturation = clampv(g.saturation, 0.0f, 2.0f);
    g.rasterThreads = clampv(g.rasterThreads, 0, 64);
    audio.volume = clampv(audio.volume, 0, 100);
    audio.latencyMs = clampv(audio.latencyMs, 16, 500);
    for (auto& p : ports) {
        p.device = clampv(p.device, -2, 15);
        p.deadzone = clampv(p.deadzone, 0.0f, 0.9f);
        p.sensitivity = clampv(p.sensitivity, 0.25f, 3.0f);
        p.triggerThreshold = clampv(p.triggerThreshold, 0.05f, 0.95f);
        p.vibrationStrength = clampv(p.vibrationStrength, 0.0f, 1.0f);
        for (auto& b : p.bind) {
            if (b.key < 0 || b.key >= 512) b.key = 0;
            if (b.key2 < 0 || b.key2 >= 512) b.key2 = 0;
            if (b.button < -1 || b.button > 25) b.button = -1;
            if (b.axis < -1 || b.axis > 5) b.axis = -1;
            b.axisDir = b.axisDir < 0 ? -1 : 1;
        }
    }
    system.logLevel = clampv(system.logLevel, 0, 3);
    system.compileJobs = clampv(system.compileJobs, 0, 256);
    system.optimization = clampv(system.optimization, 0, 3);
}

Json Settings::toJson() const {
    Json j;
    const auto& g = graphics;
    Json& jg = j["graphics"];
    jg["backend"] = (int)g.backend;
    jg["windowWidth"] = g.windowWidth;
    jg["windowHeight"] = g.windowHeight;
    jg["fullscreen"] = (int)g.fullscreen;
    jg["display"] = g.display;
    jg["exclusiveWidth"] = g.exclusiveWidth;
    jg["exclusiveHeight"] = g.exclusiveHeight;
    jg["exclusiveRefresh"] = g.exclusiveRefresh;
    jg["vsync"] = g.vsync;
    jg["allowTearing"] = g.allowTearing;
    jg["pacing"] = (int)g.pacing;
    jg["fpsLimit"] = g.fpsLimit;
    jg["maxFrameLatency"] = g.maxFrameLatency;
    jg["filter"] = (int)g.filter;
    jg["aspect"] = (int)g.aspect;
    jg["integerScale"] = g.integerScale;
    jg["textureFiltering"] = (int)g.textureFiltering;
    jg["anisotropy"] = g.anisotropy;
    jg["antiAliasing"] = (int)g.antiAliasing;
    jg["deinterlace"] = (int)g.deinterlace;
    jg["sharpen"] = g.sharpen;
    jg["scanlines"] = g.scanlines;
    jg["scanlineStrength"] = g.scanlineStrength;
    jg["brightness"] = (double)g.brightness;
    jg["contrast"] = (double)g.contrast;
    jg["gamma"] = (double)g.gamma;
    jg["saturation"] = (double)g.saturation;
    jg["rasterThreads"] = g.rasterThreads;
    jg["showFps"] = g.showFps;
    jg["showStats"] = g.showStats;
    jg["pauseOnFocusLoss"] = g.pauseOnFocusLoss;

    Json& ja = j["audio"];
    ja["enabled"] = audio.enabled;
    ja["volume"] = audio.volume;
    ja["latencyMs"] = audio.latencyMs;
    ja["syncToAudio"] = audio.syncToAudio;

    Json& jp = j["ports"];
    jp.arr();
    for (const auto& p : ports) {
        Json o;
        o["enabled"] = p.enabled;
        o["device"] = p.device;
        o["preset"] = p.preset;
        o["deadzone"] = (double)p.deadzone;
        o["sensitivity"] = (double)p.sensitivity;
        o["triggerThreshold"] = (double)p.triggerThreshold;
        o["vibration"] = p.vibration;
        o["vibrationStrength"] = (double)p.vibrationStrength;
        o["analogMode"] = p.analogMode;
        Json& jb = o["bindings"];
        for (int i = 0; i < IN_COUNT; ++i) {
            const Binding& b = p.bind[(size_t)i];
            Json e;
            e["key"] = b.key;
            e["key2"] = b.key2;
            e["button"] = b.button;
            e["axis"] = b.axis;
            e["axisDir"] = b.axisDir;
            jb[padInputId(i)] = e;
        }
        jp.push(o);
    }

    Json& js = j["system"];
    js["memcardDir"] = system.memcardDir;
    js["logLevel"] = system.logLevel;
    js["autoRebuildMissing"] = system.autoRebuildMissing;
    js["compileJobs"] = system.compileJobs;
    js["optimization"] = system.optimization;
    js["language"] = system.language;
    j["version"] = 1;
    return j;
}

Settings Settings::fromJson(const Json& j) {
    Settings s;
    if (!j.isObject()) return s;
    const Json& jg = j.get("graphics");
    auto& g = s.graphics;
    auto num = [](const Json& o, const char* k, double def) { return o.get(k).asNumber(def); };
    auto boo = [](const Json& o, const char* k, bool def) { return o.get(k).asBool(def); };
    g.backend = enumFrom(jg.get("backend"), g.backend, 2);
    g.windowWidth = (int)num(jg, "windowWidth", g.windowWidth);
    g.windowHeight = (int)num(jg, "windowHeight", g.windowHeight);
    g.fullscreen = enumFrom(jg.get("fullscreen"), g.fullscreen, 2);
    g.display = (int)num(jg, "display", g.display);
    g.exclusiveWidth = (int)num(jg, "exclusiveWidth", 0);
    g.exclusiveHeight = (int)num(jg, "exclusiveHeight", 0);
    g.exclusiveRefresh = (int)num(jg, "exclusiveRefresh", 0);
    g.vsync = boo(jg, "vsync", g.vsync);
    g.allowTearing = boo(jg, "allowTearing", g.allowTearing);
    g.pacing = enumFrom(jg.get("pacing"), g.pacing, 2);
    g.fpsLimit = (int)num(jg, "fpsLimit", g.fpsLimit);
    g.maxFrameLatency = (int)num(jg, "maxFrameLatency", g.maxFrameLatency);
    g.filter = enumFrom(jg.get("filter"), g.filter, 4);
    g.aspect = enumFrom(jg.get("aspect"), g.aspect, 3);
    g.integerScale = boo(jg, "integerScale", g.integerScale);
    g.textureFiltering = enumFrom(jg.get("textureFiltering"), g.textureFiltering, 2);
    g.anisotropy = (int)num(jg, "anisotropy", 0);
    g.antiAliasing = enumFrom(jg.get("antiAliasing"), g.antiAliasing, 1);
    g.deinterlace = enumFrom(jg.get("deinterlace"), g.deinterlace, 1);
    g.sharpen = (int)num(jg, "sharpen", g.sharpen);
    g.scanlines = boo(jg, "scanlines", g.scanlines);
    g.scanlineStrength = (int)num(jg, "scanlineStrength", g.scanlineStrength);
    g.brightness = (float)num(jg, "brightness", g.brightness);
    g.contrast = (float)num(jg, "contrast", g.contrast);
    g.gamma = (float)num(jg, "gamma", g.gamma);
    g.saturation = (float)num(jg, "saturation", g.saturation);
    g.rasterThreads = (int)num(jg, "rasterThreads", g.rasterThreads);
    g.showFps = boo(jg, "showFps", g.showFps);
    g.showStats = boo(jg, "showStats", g.showStats);
    g.pauseOnFocusLoss = boo(jg, "pauseOnFocusLoss", g.pauseOnFocusLoss);

    const Json& ja = j.get("audio");
    s.audio.enabled = boo(ja, "enabled", s.audio.enabled);
    s.audio.volume = (int)num(ja, "volume", s.audio.volume);
    s.audio.latencyMs = (int)num(ja, "latencyMs", s.audio.latencyMs);
    s.audio.syncToAudio = boo(ja, "syncToAudio", s.audio.syncToAudio);

    const Json& jp = j.get("ports");
    if (jp.isArray()) {
        for (size_t i = 0; i < jp.asArray().size() && i < 2; ++i) {
            const Json& o = jp.asArray()[i];
            PortSettings& p = s.ports[i];
            p.enabled = boo(o, "enabled", p.enabled);
            p.device = (int)num(o, "device", p.device);
            p.preset = o.get("preset").asString(p.preset);
            p.deadzone = (float)num(o, "deadzone", p.deadzone);
            p.sensitivity = (float)num(o, "sensitivity", p.sensitivity);
            p.triggerThreshold = (float)num(o, "triggerThreshold", p.triggerThreshold);
            p.vibration = boo(o, "vibration", p.vibration);
            p.vibrationStrength = (float)num(o, "vibrationStrength", p.vibrationStrength);
            p.analogMode = boo(o, "analogMode", p.analogMode);
            const Json& jb = o.get("bindings");
            for (int k = 0; k < IN_COUNT; ++k) {
                const Json& e = jb.get(padInputId(k));
                if (!e.isObject()) continue;
                Binding& b = p.bind[(size_t)k];
                b.key = (int)num(e, "key", b.key);
                b.key2 = (int)num(e, "key2", b.key2);
                b.button = (int)num(e, "button", b.button);
                b.axis = (int)num(e, "axis", b.axis);
                b.axisDir = (int)num(e, "axisDir", b.axisDir);
            }
        }
    }

    const Json& js = j.get("system");
    s.system.memcardDir = js.get("memcardDir").asString();
    s.system.logLevel = (int)num(js, "logLevel", s.system.logLevel);
    s.system.autoRebuildMissing = boo(js, "autoRebuildMissing", s.system.autoRebuildMissing);
    s.system.compileJobs = (int)num(js, "compileJobs", s.system.compileJobs);
    s.system.optimization = (int)num(js, "optimization", s.system.optimization);
    s.system.language = js.get("language").asString(s.system.language);
    s.clamp();
    return s;
}

Settings Settings::load(const std::string& path) {
    std::string err;
    Json j = Json::load(path, &err);
    if (!err.empty() || !j.isObject()) return Settings();
    return fromJson(j);
}

bool Settings::save(const std::string& path) const { return toJson().save(path); }

} // namespace sb2
