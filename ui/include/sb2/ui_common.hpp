// Shared Dear ImGui look & feel and settings editors used by both the
// launcher and the in-game menu.
#pragma once
#include <string>

#include "imgui.h"
#include "sb2/presenter.hpp"
#include "sb2/settings.hpp"

struct SDL_Window;

namespace sb2 {
class InputManager;

namespace ui {

// Tiny i18n: Indonesian is the primary language, English the alternative.
void setLanguage(const std::string& lang); // "id" or "en"
const std::string& language();
const char* T(const char* id, const char* en);

float displayScale(SDL_Window* w);
void applyTheme(float scale);
// Loads UI fonts (system fonts with CJK fallback). Returns the heading font.
struct Fonts {
    ImFont* body = nullptr;
    ImFont* heading = nullptr;
    ImFont* title = nullptr;
};
Fonts loadFonts(float scale);

extern ImVec4 kAccent, kAccentSoft, kGood, kWarn, kBad, kMuted;

void helpMarker(const char* text);
bool comboInt(const char* label, int* v, const char* const* items, int count);
template <typename E>
bool comboEnum(const char* label, E* v, const char* const* items, int count) {
    int i = (int)*v;
    if (!comboInt(label, &i, items, count)) return false;
    *v = (E)i;
    return true;
}
void sectionTitle(const char* text);
void statusPill(const char* text, const ImVec4& color);

// Settings editors. Return true when something changed.
// `inGame` hides options that need a restart.
bool editGraphics(GraphicsSettings& g, const PresenterCaps& caps, bool inGame);
bool editAdvancedGraphics(GraphicsSettings& g, const PresenterCaps& caps, bool inGame);
bool editAudio(AudioSettings& a);
bool editControls(Settings& s, InputManager& input, int& port);
bool editSystem(SystemSettings& sys);

} // namespace ui
} // namespace sb2
