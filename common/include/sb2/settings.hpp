// User settings shared by the launcher (which edits them) and the game
// runtime front-end (which applies them).  Stored as JSON.
#pragma once
#include <array>
#include <cstdint>
#include <string>

#include "sb2/json.hpp"

namespace sb2 {

// ---------------------------------------------------------------- graphics

enum class Backend : int { Auto = 0, D3D12 = 1, SdlRenderer = 2 };
enum class FullscreenMode : int { Windowed = 0, Borderless = 1, Exclusive = 2 };
enum class PacingMode : int { Timer = 0, DisplayVsync = 1, Unthrottled = 2 };
enum class PresentFilter : int { Nearest = 0, Bilinear = 1, SharpBilinear = 2, Bicubic = 3, Lanczos = 4 };
enum class AspectMode : int { Auto = 0, Ratio4x3 = 1, Ratio16x9 = 2, Stretch = 3 };
enum class TextureFiltering : int { AsGame = 0, ForceBilinear = 1, ForceNearest = 2 };
enum class AntiAliasing : int { Off = 0, Fxaa = 1 };
enum class Deinterlace : int { Off = 0, Blend = 1 };

struct GraphicsSettings {
    Backend backend = Backend::Auto;
    int windowWidth = 1280;
    int windowHeight = 960;
    FullscreenMode fullscreen = FullscreenMode::Windowed;
    int display = 0;                 // monitor index for fullscreen
    int exclusiveWidth = 0;          // 0 = desktop resolution
    int exclusiveHeight = 0;
    int exclusiveRefresh = 0;
    bool vsync = true;
    bool allowTearing = true;        // use DXGI tearing when VSync is off
    PacingMode pacing = PacingMode::Timer;
    int fpsLimit = 0;                // 0 = console rate (59.94 / 50), otherwise target vblank rate
    int maxFrameLatency = 2;         // swap chain frame latency (1..3)
    PresentFilter filter = PresentFilter::SharpBilinear;
    AspectMode aspect = AspectMode::Auto;
    bool integerScale = false;
    TextureFiltering textureFiltering = TextureFiltering::AsGame;
    int anisotropy = 0;              // requires a hardware GS renderer; kept for config compatibility
    AntiAliasing antiAliasing = AntiAliasing::Off;
    Deinterlace deinterlace = Deinterlace::Blend;
    int sharpen = 0;                 // 0..100 (contrast adaptive sharpening)
    bool scanlines = false;
    int scanlineStrength = 25;       // 0..100
    float brightness = 1.0f;         // 0.5..1.5
    float contrast = 1.0f;           // 0.5..1.5
    float gamma = 1.0f;              // 0.5..2.0
    float saturation = 1.0f;         // 0..2
    int rasterThreads = 0;           // 0 = automatic
    bool showFps = false;
    bool showStats = false;
    bool pauseOnFocusLoss = false;
};

// ---------------------------------------------------------------- audio

struct AudioSettings {
    bool enabled = true;
    int volume = 100;    // 0..100
    int latencyMs = 64;  // target buffered audio
    bool syncToAudio = true; // nudge frame pacing using the audio buffer level
};

// ---------------------------------------------------------------- input

// Logical DualShock 2 inputs, in binding-table order.
enum PadInput : int {
    IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT,
    IN_CROSS, IN_CIRCLE, IN_SQUARE, IN_TRIANGLE,
    IN_L1, IN_R1, IN_L2, IN_R2, IN_L3, IN_R3,
    IN_SELECT, IN_START,
    IN_LX_NEG, IN_LX_POS, IN_LY_NEG, IN_LY_POS,
    IN_RX_NEG, IN_RX_POS, IN_RY_NEG, IN_RY_POS,
    IN_ANALOG_MOD, // keyboard: hold to walk (half analog deflection)
    IN_COUNT
};
const char* padInputName(int input);   // "Cross", "Left Stick Up", ...
const char* padInputId(int input);     // stable JSON key

// One binding: a keyboard key and/or a gamepad element.  Values are SDL3
// enum values (SDL_Scancode, SDL_GamepadButton, SDL_GamepadAxis); -1/0 = none.
struct Binding {
    int key = 0;          // SDL_Scancode (0 = none)
    int key2 = 0;         // alternate key
    int button = -1;      // SDL_GamepadButton
    int axis = -1;        // SDL_GamepadAxis
    int axisDir = 1;      // +1 / -1 half-axis
};

struct PortSettings {
    bool enabled = true;
    int device = -2;            // -2 = auto (first free gamepad + keyboard), -1 = keyboard only, >=0 gamepad slot
    std::string preset = "default";
    std::array<Binding, IN_COUNT> bind{};
    float deadzone = 0.15f;     // 0..0.9 (radial, left & right stick)
    float sensitivity = 1.0f;   // 0.5..2.0 response scale
    float triggerThreshold = 0.30f;
    bool vibration = true;
    float vibrationStrength = 1.0f;
    bool analogMode = true;     // report as DualShock 2 analog
};

// Built-in presets. Returns false for an unknown name.
bool applyInputPreset(PortSettings& p, const std::string& preset, int port);
const char* const* inputPresetNames(int* count);

// ---------------------------------------------------------------- system

struct SystemSettings {
    std::string memcardDir;     // empty = <user data>/memcards
    int logLevel = 1;           // 0 errors, 1 warnings, 2 info, 3 debug
    bool autoRebuildMissing = true; // re-run recompilation with newly discovered entry points
    int compileJobs = 0;        // 0 = number of CPU threads
    int optimization = 2;       // -O level for generated code (1..3)
    std::string language = "id";
};

struct Settings {
    GraphicsSettings graphics;
    AudioSettings audio;
    std::array<PortSettings, 2> ports{};
    SystemSettings system;

    Settings();
    Json toJson() const;
    static Settings fromJson(const Json& j);
    static Settings load(const std::string& path); // defaults when missing / invalid
    bool save(const std::string& path) const;
    void clamp(); // force all values into their valid ranges
};

} // namespace sb2
