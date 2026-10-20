// Sengoku Basara 2 Heroes Recompiled - launcher.
//
// Library and game scanning, game information and compatibility, the local
// recompilation pipeline, process control (Play / Pause / Resume / Stop) and
// all settings (graphics, advanced graphics, audio, controls, system).
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "builder.hpp"
#include "game_session.hpp"
#include "imgui.h"
#include "library.hpp"
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
#endif

#ifndef SB2_VERSION
#define SB2_VERSION "0.0.0"
#endif

namespace sb2 {

using ui::T;

namespace {

enum class Page { Library, Settings, Logs, About };

std::string formatSize(uint64_t b) {
    char s[32];
    if (b >= (1ull << 30)) std::snprintf(s, sizeof s, "%.2f GB", b / double(1ull << 30));
    else std::snprintf(s, sizeof s, "%.1f MB", b / double(1ull << 20));
    return s;
}

std::string formatTime(int64_t t) {
    if (!t) return "-";
    char b[64];
    const std::time_t tt = (std::time_t)t;
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", std::localtime(&tt));
    return b;
}

class Launcher {
public:
    int run(int argc, char** argv);

private:
    void frame();
    void drawSidebar();
    void drawLibrary();
    void drawGameDetails(const GameEntry& g);
    void drawSettings();
    void drawLogs();
    void drawAbout();
    void drawStatusBar();
    void startBuild(const GameEntry& g, bool force);
    void play(const GameEntry& g);
    void pollProcesses();
    void saveSettings();
    void processDialogResults();
    std::string memcardDir() const;

    SDL_Window* window_ = nullptr;
    std::unique_ptr<Presenter> presenter_;
    std::unique_ptr<InputManager> input_;
    ui::Fonts fonts_;
    Settings settings_;
    std::string settingsPath_;
    Toolchain toolchain_;
    Library library_;
    ModuleBuilder builder_;
    std::unique_ptr<GameSession> session_;
    std::string sessionSerial_;
    std::vector<GameEntry> games_;
    int selected_ = 0;
    Page page_ = Page::Library;
    int settingsTab_ = 0;
    int controlsPort_ = 0;
    int logTab_ = 0;
    bool settingsDirty_ = false;
    bool playAfterBuild_ = false;
    std::string playAfterBuildSerial_;
    std::string notice_;
    ImVec4 noticeColor_ = ui::kMuted;
    std::string presenterLog_;
    bool quit_ = false;
    std::mutex dialogMutex_;
    std::vector<std::pair<int, std::string>> dialogResults_; // 0 = folder, 1 = file, 2 = memcard dir
    std::string lastGameLog_;
    std::string selfTestScreenshot_;
    int selfTestFrames_ = 0;
};

// ------------------------------------------------------------------ actions

std::string Launcher::memcardDir() const {
    return settings_.system.memcardDir.empty() ? joinPath(userDataDir(), "memcards") : settings_.system.memcardDir;
}

void Launcher::saveSettings() {
    settings_.clamp();
    settings_.save(settingsPath_);
    settingsDirty_ = false;
    if (session_ && session_->running()) session_->sendCommand("reload-settings");
}

void Launcher::startBuild(const GameEntry& g, bool force) {
    if (builder_.running()) return;
    if (!toolchain_.valid) {
        notice_ = std::string(T("Compiler bawaan tidak ditemukan: ", "Bundled compiler not found: ")) + toolchain_.error;
        noticeColor_ = ui::kBad;
        return;
    }
    builder_.start(g, toolchain_, settings_, force);
    notice_.clear();
}

void Launcher::play(const GameEntry& g) {
    if (session_ && session_->running()) return;
    const BuildInfo bi = inspectBuild(g, toolchain_);
    if (!bi.built || bi.stale) {
        playAfterBuild_ = true;
        playAfterBuildSerial_ = g.serial;
        startBuild(g, false);
        return;
    }
    saveSettings();
    makeDirs(memcardDir());
    const std::string logDir = joinPath(userDataDir(), "logs");
    makeDirs(logDir);
    char name[64];
    const std::time_t t = std::time(nullptr);
    std::strftime(name, sizeof name, "game_%Y%m%d_%H%M%S.log", std::localtime(&t));
    lastGameLog_ = joinPath(logDir, name);
    std::vector<std::string> args = {bi.exePath,        "--elf",    bi.elfPath,       "--disc",
                                     g.path,            "--boot",   g.bootPath,       "--saves",
                                     memcardDir(),      "--config", settingsPath_,    "--control-stdin",
                                     "--missing-entries", joinPath(bi.dir, "entries_found.txt"),
                                     "--title",         "Sengoku Basara 2 Heroes Recompiled",
                                     "--screenshot-dir", joinPath(userDataDir(), "screenshots")};
    std::string err;
    session_ = std::make_unique<GameSession>();
    if (!session_->start(args, lastGameLog_, err)) {
        notice_ = std::string(T("Gagal menjalankan game: ", "Failed to start the game: ")) + err;
        noticeColor_ = ui::kBad;
        session_.reset();
        return;
    }
    sessionSerial_ = g.serial;
    notice_ = T("Game berjalan.", "Game running.");
    noticeColor_ = ui::kGood;
}

void Launcher::pollProcesses() {
    // Build finished?
    static bool wasBuilding = false;
    const bool building = builder_.running();
    if (wasBuilding && !building) {
        const auto p = builder_.progress();
        if (p.stage == ModuleBuilder::Stage::Done) {
            notice_ = T("Recompile selesai. Executable native siap.", "Recompilation finished. Native executable ready.");
            noticeColor_ = ui::kGood;
            if (playAfterBuild_) {
                for (auto& g : games_)
                    if (g.serial == playAfterBuildSerial_) {
                        playAfterBuild_ = false;
                        play(g);
                        break;
                    }
            }
        } else {
            notice_ = std::string(T("Build gagal: ", "Build failed: ")) + p.message;
            noticeColor_ = ui::kBad;
            playAfterBuild_ = false;
        }
    }
    wasBuilding = building;
    // Game exited?
    int code = 0;
    if (session_ && session_->finished(code)) {
        // Merge newly discovered entry points into the build's list.
        for (auto& g : games_) {
            if (g.serial != sessionSerial_) continue;
            const BuildInfo bi = inspectBuild(g, toolchain_);
            const std::string found = joinPath(bi.dir, "entries_found.txt");
            std::string text;
            if (readTextFile(found, text) && !text.empty()) {
                std::string cur;
                readTextFile(joinPath(bi.dir, "entries.txt"), cur);
                writeTextFile(joinPath(bi.dir, "entries.txt"), cur + text);
                std::error_code ec;
                std::filesystem::remove(std::filesystem::path(std::u8string((const char8_t*)found.c_str())), ec);
                if (code == 6 && settings_.system.autoRebuildMissing) {
                    notice_ = T("Game mencapai kode yang belum direcompile. Recompile ulang otomatis dengan entry point baru...",
                                "The game reached code that was not recompiled. Rebuilding with the new entry points...");
                    noticeColor_ = ui::kWarn;
                    playAfterBuild_ = true;
                    playAfterBuildSerial_ = g.serial;
                    startBuild(g, false);
                }
            }
            break;
        }
        if (!builder_.running()) {
            char b[160];
            if (code == 0) std::snprintf(b, sizeof b, "%s", T("Game ditutup.", "Game closed."));
            else std::snprintf(b, sizeof b, T("Game berhenti dengan kode %d (lihat Log).", "Game stopped with code %d (see Logs)."), code);
            notice_ = b;
            noticeColor_ = code == 0 ? ui::kMuted : ui::kWarn;
        }
        session_.reset();
    }
}

void Launcher::processDialogResults() {
    std::vector<std::pair<int, std::string>> r;
    {
        std::lock_guard<std::mutex> lk(dialogMutex_);
        r.swap(dialogResults_);
    }
    bool rescan = false;
    for (auto& [kind, path] : r) {
        if (kind == 0) library_.addFolder(path), rescan = true;
        else if (kind == 1) library_.addFile(path), rescan = true;
        else if (kind == 2) {
            settings_.system.memcardDir = path;
            saveSettings();
        }
    }
    if (rescan) library_.startScan();
}

// ------------------------------------------------------------------ UI

void Launcher::drawSidebar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.063f, 0.073f, 1.0f));
    ImGui::BeginChild("sidebar", ImVec2(ImGui::GetFontSize() * 13.5f, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(fonts_.heading, fonts_.heading->LegacySize);
    ImGui::TextColored(ui::kAccent, "SB2 HEROES");
    ImGui::PopFont();
    ImGui::TextDisabled("Recompiled  v%s", SB2_VERSION);
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
    struct Item {
        const char* label;
        Page page;
    } items[] = {{T("Perpustakaan", "Library"), Page::Library},
                 {T("Pengaturan", "Settings"), Page::Settings},
                 {T("Log", "Logs"), Page::Logs},
                 {T("Tentang", "About"), Page::About}};
    for (auto& it : items) {
        const bool sel = page_ == it.page;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        if (ImGui::Selectable(it.label, sel, 0, ImVec2(0, ImGui::GetFontSize() * 2.0f))) page_ = it.page;
        if (sel) ImGui::PopStyleColor();
    }
    // Footer: toolchain state.
    const float footerH = ImGui::GetTextLineHeightWithSpacing() * 3 + ImGui::GetStyle().WindowPadding.y + ImGui::GetStyle().ItemSpacing.y * 2;
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - footerH));
    ImGui::Separator();
    if (toolchain_.valid) {
        ImGui::TextColored(ui::kGood, "%s", T("Compiler siap", "Compiler ready"));
        ImGui::TextDisabled("%s", hostCpuTarget().c_str());
    } else {
        ImGui::TextColored(ui::kBad, "%s", T("Compiler tidak ada", "Compiler missing"));
    }
    ImGui::TextDisabled("%s", presenter_->name());
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void Launcher::drawStatusBar() {
    if (session_) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.06f, 0.07f, 1.0f));
        ImGui::BeginChild("running", ImVec2(0, ImGui::GetFontSize() * 2.9f), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::AlignTextToFramePadding();
        ui::statusPill(session_->paused() ? T("DIJEDA", "PAUSED") : T("BERJALAN", "RUNNING"), session_->paused() ? ui::kWarn : ui::kGood);
        ImGui::SameLine();
        ImGui::Text("%s  |  %.1f VPS  %.0f%%  %.0f FPS", sessionSerial_.c_str(), session_->vps(), session_->speed(), session_->fps());
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetFontSize() * 15);
        if (session_->paused()) {
            if (ImGui::Button(T("Lanjutkan", "Resume"))) session_->resume();
        } else if (ImGui::Button(T("Jeda", "Pause"))) {
            session_->pause();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.15f, 1.0f));
        if (ImGui::Button(T("Hentikan", "Stop"))) session_->stop();
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if (!notice_.empty()) {
        ImGui::TextColored(noticeColor_, "%s", notice_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) notice_.clear();
    }
}

void Launcher::drawLibrary() {
    ImGui::PushFont(fonts_.title, fonts_.title->LegacySize);
    ImGui::TextUnformatted(T("Perpustakaan", "Library"));
    ImGui::PopFont();
    ImGui::SameLine();
    const float bw = ImGui::GetFontSize() * 8.5f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw * 3 - 40);
    if (ImGui::Button(T("Tambah folder", "Add folder"), ImVec2(bw, 0))) {
        SDL_ShowOpenFolderDialog(
            [](void* ud, const char* const* list, int) {
                auto* self = static_cast<Launcher*>(ud);
                if (!list) return;
                std::lock_guard<std::mutex> lk(self->dialogMutex_);
                for (; *list; ++list) self->dialogResults_.push_back({0, *list});
            },
            this, window_, nullptr, false);
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Tambah file", "Add file"), ImVec2(bw, 0))) {
        static const SDL_DialogFileFilter filters[] = {{"PS2 disc image", "iso;bin;cue;img"}};
        SDL_ShowOpenFileDialog(
            [](void* ud, const char* const* list, int) {
                auto* self = static_cast<Launcher*>(ud);
                if (!list) return;
                std::lock_guard<std::mutex> lk(self->dialogMutex_);
                for (; *list; ++list) self->dialogResults_.push_back({1, *list});
            },
            this, window_, filters, 1, nullptr, true);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(library_.scanning());
    if (ImGui::Button(library_.scanning() ? T("Memindai...", "Scanning...") : T("Pindai ulang", "Rescan"), ImVec2(bw, 0)))
        library_.startScan();
    ImGui::EndDisabled();
    if (library_.scanning()) ImGui::TextDisabled("%s", library_.scanStatus().c_str());

    // Folders
    const auto folders = library_.folders();
    if (ImGui::CollapsingHeader(T("Folder yang dipindai", "Scanned folders"))) {
        for (size_t i = 0; i < folders.size(); ++i) {
            ImGui::PushID((int)i);
            ImGui::TextUnformatted(folders[i].c_str());
            if (!isDirectory(folders[i])) {
                ImGui::SameLine();
                ImGui::TextColored(ui::kWarn, "%s", T("(tidak ada)", "(missing)"));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Hapus", "Remove"))) library_.removeFolder(i);
            ImGui::PopID();
        }
    }
    ImGui::Spacing();

    if (games_.empty()) {
        ImGui::BeginChild("empty", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushFont(fonts_.heading, fonts_.heading->LegacySize);
        ImGui::TextUnformatted(T("Belum ada game", "No games yet"));
        ImGui::PopFont();
        ImGui::TextWrapped("%s", T("Tambahkan folder yang berisi image disc PS2 milik Anda (ISO, BIN/CUE) atau pilih file langsung. "
                                   "Image Sengoku Basara 2 Heroes (SLPM-66848) akan dikenali secara otomatis. "
                                   "File game tidak disertakan dan tidak pernah diunggah ke mana pun.",
                                   "Add a folder containing your own PS2 disc images (ISO, BIN/CUE) or pick a file. "
                                   "Sengoku Basara 2 Heroes (SLPM-66848) images are recognised automatically. "
                                   "Game files are not included and are never uploaded anywhere."));
        ImGui::EndChild();
        return;
    }
    selected_ = std::clamp(selected_, 0, (int)games_.size() - 1);
    const float listW = std::max(ImGui::GetFontSize() * 18, ImGui::GetContentRegionAvail().x * 0.36f);
    ImGui::BeginChild("list", ImVec2(listW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    for (int i = 0; i < (int)games_.size(); ++i) {
        const GameEntry& g = games_[(size_t)i];
        ImGui::PushID(i);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFontSize() * 3.6f;
        if (ImGui::Selectable("##g", selected_ == i, 0, ImVec2(0, h))) selected_ = i;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bar = ImGui::GetColorU32(g.target ? ui::kAccent : (g.valid ? ui::kMuted : ui::kBad));
        dl->AddRectFilled(ImVec2(p0.x, p0.y + 4), ImVec2(p0.x + 4, p0.y + h - 4), bar, 2.0f);
        const float x = p0.x + 14;
        dl->AddText(fonts_.heading, ImGui::GetFontSize() * 1.1f, ImVec2(x, p0.y + 4), ImGui::GetColorU32(ImGuiCol_Text),
                    (g.title.empty() ? g.fileName : g.title).c_str());
        std::string sub = (g.serial.empty() ? std::string("?") : g.serial) + "  " + g.region + "  " + g.format + "  " + formatSize(g.fileSize);
        dl->AddText(ImVec2(x, p0.y + ImGui::GetFontSize() * 1.45f), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
        dl->AddText(ImVec2(x, p0.y + ImGui::GetFontSize() * 2.35f), ImGui::GetColorU32(ImGuiCol_TextDisabled), g.fileName.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    drawGameDetails(games_[(size_t)selected_]);
    ImGui::EndChild();
}

void Launcher::drawGameDetails(const GameEntry& g) {
    ImGui::PushFont(fonts_.title, fonts_.title->LegacySize);
    ImGui::TextWrapped("%s", (g.title.empty() ? g.fileName : g.title).c_str());
    ImGui::PopFont();
    if (g.target) ImGui::TextDisabled("%s", (const char*)u8"戦国BASARA 2 英雄外伝  -  Capcom, 2007");
    // Compatibility status
    if (!g.valid) ui::statusPill(T("Image tidak valid", "Invalid image"), ui::kBad);
    else if (g.target) ui::statusPill(T("Judul target", "Target title"), ui::kAccent);
    else if (g.known) ui::statusPill(T("Dikenal", "Known"), ui::kWarn);
    else ui::statusPill(T("Tidak didukung resmi", "Not officially supported"), ui::kMuted);
    if (g.valid) {
        ImGui::SameLine();
        if (g.exactVersion && g.known) ui::statusPill(T("Versi cocok", "Version match"), ui::kGood);
        else if (g.known) ui::statusPill(T("Versi berbeda", "Different version"), ui::kWarn);
    }
    const BuildInfo bi = inspectBuild(g, toolchain_);
    ImGui::SameLine();
    if (bi.built && !bi.stale) ui::statusPill(T("Siap dimainkan", "Ready to play"), ui::kGood);
    else if (bi.built) ui::statusPill(T("Perlu dibangun ulang", "Rebuild needed"), ui::kWarn);
    else if (bi.state.isObject() && !bi.state.get("success").asBool(true)) ui::statusPill(T("Build gagal", "Build failed"), ui::kBad);
    else ui::statusPill(T("Belum direcompile", "Not recompiled"), ui::kMuted);

    if (!g.valid) {
        ImGui::TextColored(ui::kBad, "%s", g.error.c_str());
        return;
    }
    for (auto& w : g.warnings) ImGui::TextColored(ui::kWarn, "! %s", w.c_str());

    // Action buttons
    ImGui::Spacing();
    const bool building = builder_.running();
    const bool running = session_ != nullptr;
    const float bh = ImGui::GetFontSize() * 2.2f;
    ImGui::BeginDisabled(building || running);
    ImGui::PushStyleColor(ImGuiCol_Button, ui::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.30f, 0.33f, 1.0f));
    if (ImGui::Button(bi.built && !bi.stale ? T("  MAIN  ", "  PLAY  ") : T("  RECOMPILE & MAIN  ", "  RECOMPILE & PLAY  "), ImVec2(0, bh)))
        play(g);
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button(T("Recompile", "Recompile"), ImVec2(0, bh))) startBuild(g, false);
    ImGui::SameLine();
    if (ImGui::Button(T("Bangun ulang penuh", "Full rebuild"), ImVec2(0, bh))) startBuild(g, true);
    ImGui::EndDisabled();
    if (building) {
        ImGui::SameLine();
        if (ImGui::Button(T("Batal", "Cancel"), ImVec2(0, bh))) builder_.cancel();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Folder cache", "Cache folder"), ImVec2(0, bh))) {
        makeDirs(bi.dir);
        openInShell(bi.dir);
    }

    if (building || builder_.progress().stage != ModuleBuilder::Stage::Idle) {
        const auto p = builder_.progress();
        ImGui::Spacing();
        char lbl[200];
        std::snprintf(lbl, sizeof lbl, "%s (pass %d): %s", ModuleBuilder::stageName(p.stage), p.pass, p.message.c_str());
        ImGui::ProgressBar((float)p.fraction, ImVec2(-FLT_MIN, 0), lbl);
        if (building) {
            ImGui::BeginChild("buildlog", ImVec2(0, ImGui::GetFontSize() * 7), ImGuiChildFlags_Borders);
            for (auto& l : builder_.logTail(8)) ImGui::TextUnformatted(l.c_str());
            ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
    }

    ui::sectionTitle(T("Informasi game", "Game information"));
    if (ImGui::BeginTable("info", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        auto row = [](const char* k, const std::string& v) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", k);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", v.c_str());
        };
        char hex[32];
        row(T("Serial", "Serial"), g.serial);
        row(T("Region", "Region"), g.region);
        row(T("Versi disc", "Disc version"), g.version.empty() ? "-" : g.version);
        row(T("Mode video", "Video mode"), g.vmode.empty() ? "-" : g.vmode);
        row(T("Executable", "Executable"), g.bootPath);
        std::snprintf(hex, sizeof hex, "%08X", g.elfCrc);
        row("ELF CRC", hex);
        std::snprintf(hex, sizeof hex, "%08X", g.elfEntry);
        row(T("Entry point", "Entry point"), hex);
        row(T("Ukuran ELF", "ELF size"), std::to_string(g.elfSize / 1024) + " KB");
        row("SHA-256", g.elfSha256);
        row(T("Format", "Format"), g.format + "  (" + std::to_string(g.sectorCount) + T(" sektor)", " sectors)"));
        row(T("Volume", "Volume"), g.volumeId);
        row(T("File", "File"), g.path);
        row(T("Kompatibilitas", "Compatibility"), g.compatibility);
        ImGui::EndTable();
    }

    ui::sectionTitle(T("Recompilation", "Recompilation"));
    if (bi.state.isObject()) {
        const Json& rs = bi.state.get("recompile");
        const Json& vf = bi.state.get("verify");
        if (ImGui::BeginTable("build", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            auto row = [](const char* k, const std::string& v) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", k);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextWrapped("%s", v.c_str());
            };
            row(T("Status", "Status"), bi.state.get("success").asBool() ? T("Berhasil", "Succeeded") : bi.state.get("error").asString());
            row(T("Dibangun", "Built"), formatTime(bi.state.get("builtAt").asInt()));
            row(T("Instruksi", "Instructions"), std::to_string(rs.get("instructions").asInt()));
            row(T("Fungsi", "Functions"), std::to_string(rs.get("functions").asInt()));
            row(T("File C++", "C++ files"), std::to_string(rs.get("files").asInt()));
            row(T("Entry point tambahan", "Extra entry points"), std::to_string(rs.get("extraEntries").asInt()));
            row(T("Konstruksi tak didukung", "Unsupported constructs"), std::to_string(rs.get("unsupported").asInt()));
            char v[200];
            std::snprintf(v, sizeof v, T("kode keluar %d, %.1f dtk, %d entry baru", "exit code %d, %.1f s, %d new entries"),
                          (int)vf.get("exitCode").asInt(), vf.get("seconds").asNumber(), (int)vf.get("newEntries").asInt());
            row(T("Uji boot headless", "Headless boot test"), vf.isObject() ? v : "-");
            std::snprintf(v, sizeof v, "%.0f s, -O%d, %s", bi.state.get("buildSeconds").asNumber(), (int)bi.state.get("optimization").asInt(),
                          bi.state.get("cpuTarget").asString().c_str());
            row(T("Build", "Build"), v);
            row(T("Compiler", "Compiler"), bi.state.get("compiler").asString());
            ImGui::EndTable();
        }
    } else {
        ImGui::TextWrapped("%s", T("Game ini belum direcompile. Tekan RECOMPILE untuk menerjemahkan kode MIPS R5900 game menjadi C++ lalu "
                                   "mengompilasinya menjadi executable Windows native di PC ini. Proses pertama dapat memakan waktu beberapa menit.",
                                   "This game has not been recompiled yet. Press RECOMPILE to translate the game's MIPS R5900 code into C++ and "
                                   "compile it into a native Windows executable on this PC. The first run can take several minutes."));
    }
}

void Launcher::drawSettings() {
    ImGui::PushFont(fonts_.title, fonts_.title->LegacySize);
    ImGui::TextUnformatted(T("Pengaturan", "Settings"));
    ImGui::PopFont();
    const char* tabs[] = {T("Grafis", "Graphics"), T("Grafis lanjutan", "Advanced graphics"), "Audio", T("Kontrol", "Controls"),
                          T("Sistem", "System")};
    if (ImGui::BeginTabBar("settabs")) {
        for (int i = 0; i < 5; ++i)
            if (ImGui::BeginTabItem(tabs[i])) {
                settingsTab_ = i;
                ImGui::EndTabItem();
            }
        ImGui::EndTabBar();
    }
    ImGui::BeginChild("setpage", ImVec2(0, -ImGui::GetFontSize() * 2.6f), ImGuiChildFlags_AlwaysUseWindowPadding);
    bool ch = false;
    const PresenterCaps caps{true, true}; // the game uses Direct3D 12 when available
    switch (settingsTab_) {
    case 0: ch = ui::editGraphics(settings_.graphics, caps, false); break;
    case 1: ch = ui::editAdvancedGraphics(settings_.graphics, caps, false); break;
    case 2: ch = ui::editAudio(settings_.audio); break;
    case 3: ch = ui::editControls(settings_, *input_, controlsPort_); break;
    case 4: {
        ch = ui::editSystem(settings_.system);
        ui::sectionTitle(T("Memory card", "Memory card"));
        ImGui::TextWrapped("%s", memcardDir().c_str());
        if (ImGui::Button(T("Pilih folder...", "Choose folder..."))) {
            SDL_ShowOpenFolderDialog(
                [](void* ud, const char* const* list, int) {
                    auto* self = static_cast<Launcher*>(ud);
                    if (!list || !*list) return;
                    std::lock_guard<std::mutex> lk(self->dialogMutex_);
                    self->dialogResults_.push_back({2, *list});
                },
                this, window_, nullptr, false);
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Buka", "Open"))) {
            makeDirs(memcardDir());
            openInShell(memcardDir());
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Default", "Default"))) {
            settings_.system.memcardDir.clear();
            ch = true;
        }
        ImGui::TextDisabled("%s", T("Save game disimpan oleh mekanisme memory card asli game (format .ps2 per slot).",
                                    "Saves are written by the game's own memory card mechanism."));
        ui::sectionTitle(T("Save state", "Save states"));
        ImGui::TextWrapped("%s", T("Tidak didukung: kode hasil recompile berjalan langsung di stack CPU host, sehingga status eksekusi tidak dapat "
                                   "dibekukan dan dipulihkan dengan aman. Gunakan save memory card di dalam game.",
                                   "Not supported: recompiled code runs directly on the host CPU stack, so execution state cannot be frozen and "
                                   "restored safely. Use the in-game memory card saves."));
        break;
    }
    }
    ImGui::EndChild();
    if (ch) settingsDirty_ = true;
    ImGui::BeginDisabled(!settingsDirty_);
    if (ImGui::Button(T("Simpan", "Save"), ImVec2(ImGui::GetFontSize() * 8, 0))) saveSettings();
    ImGui::SameLine();
    if (ImGui::Button(T("Batalkan perubahan", "Revert"), ImVec2(ImGui::GetFontSize() * 10, 0))) {
        settings_ = Settings::load(settingsPath_);
        ui::setLanguage(settings_.system.language);
        settingsDirty_ = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Kembalikan default", "Restore defaults"))) {
        settings_ = Settings();
        settingsDirty_ = true;
    }
    if (settingsDirty_) {
        ImGui::SameLine();
        ImGui::TextColored(ui::kWarn, "%s", T("Belum disimpan", "Unsaved"));
    }
}

void Launcher::drawLogs() {
    ImGui::PushFont(fonts_.title, fonts_.title->LegacySize);
    ImGui::TextUnformatted(T("Log", "Logs"));
    ImGui::PopFont();
    if (ImGui::BeginTabBar("logtabs")) {
        if (ImGui::BeginTabItem(T("Game", "Game"))) {
            logTab_ = 0;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("Recompile", "Recompile"))) {
            logTab_ = 1;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("Launcher", "Launcher"))) {
            logTab_ = 2;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    std::string text;
    if (logTab_ == 0) {
        if (session_) text = session_->logText();
        else if (!lastGameLog_.empty()) readTextFile(lastGameLog_, text);
    } else if (logTab_ == 1) {
        text = builder_.logText();
        if (text.empty() && !games_.empty()) {
            const BuildInfo bi = inspectBuild(games_[(size_t)std::clamp(selected_, 0, (int)games_.size() - 1)], toolchain_);
            readTextFile(joinPath(bi.dir, "build.log"), text);
        }
    } else {
        text = presenterLog_;
        text += "toolchain: " + (toolchain_.valid ? toolchain_.compilerVersion : toolchain_.error) + "\n";
        text += "sdk: " + toolchain_.root + " (" + toolchain_.sdkHash + ")\n";
        text += "user data: " + userDataDir() + "\n";
    }
    if (ImGui::Button(T("Salin", "Copy"))) ImGui::SetClipboardText(text.c_str());
    ImGui::SameLine();
    if (ImGui::Button(T("Buka folder log", "Open log folder"))) {
        makeDirs(joinPath(userDataDir(), "logs"));
        openInShell(joinPath(userDataDir(), "logs"));
    }
    ImGui::BeginChild("logtext", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void Launcher::drawAbout() {
    ImGui::PushFont(fonts_.title, fonts_.title->LegacySize);
    ImGui::TextUnformatted("Sengoku Basara 2 Heroes Recompiled");
    ImGui::PopFont();
    ImGui::TextDisabled("v%s", SB2_VERSION);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", T("Port PC native melalui static recompilation: kode MIPS R5900 dari executable PS2 diterjemahkan menjadi C++ dan "
                               "dikompilasi menjadi kode x86-64 di PC Anda. Tidak ada emulator PS2 (PCSX2 atau lainnya) yang dipakai. "
                               "Perangkat keras PS2 yang dibutuhkan game (kernel EE, SIF/IOP, DMA, VIF, VU, GS, SPU2, pad, memory card, CD/DVD) "
                               "diimplementasikan ulang di runtime.",
                               "A native PC port through static recompilation: the MIPS R5900 code of the PS2 executable is translated into C++ "
                               "and compiled to x86-64 code on your PC. No PS2 emulator (PCSX2 or otherwise) is used. The PS2 hardware the game "
                               "needs (EE kernel, SIF/IOP, DMA, VIF, VU, GS, SPU2, pad, memory card, CD/DVD) is reimplemented in the runtime."));
    ui::sectionTitle(T("Sistem", "System"));
    ImGui::Text("OS: %s", osDescription().c_str());
    ImGui::Text("CPU: %d threads, target %s", cpuThreads(), hostCpuTarget().c_str());
    ImGui::Text("RAM: %s", formatSize(totalMemoryBytes()).c_str());
    ImGui::Text("Video: %s - %s", presenter_->name(), presenter_->adapterName().c_str());
    ImGui::Text("Compiler: %s", toolchain_.valid ? toolchain_.compilerVersion.c_str() : toolchain_.error.c_str());
    ImGui::Text("SDK: %s (%s)", toolchain_.sdkVersion.c_str(), toolchain_.sdkHash.c_str());
    ImGui::Text("%s: %s", T("Data pengguna", "User data"), userDataDir().c_str());
    ui::sectionTitle(T("Legal", "Legal"));
    ImGui::TextWrapped("%s", T("Proyek ini tidak menyertakan file game, BIOS, atau aset berhak cipta apa pun. Anda membutuhkan disc asli "
                               "Sengoku Basara 2 Heroes milik Anda sendiri. Sengoku Basara adalah merek dagang Capcom. Proyek ini tidak "
                               "berafiliasi dengan Capcom atau Sony.",
                               "This project does not include any game files, BIOS or copyrighted assets. You need your own original disc of "
                               "Sengoku Basara 2 Heroes. Sengoku Basara is a trademark of Capcom. This project is not affiliated with Capcom or Sony."));
    ImGui::TextWrapped("%s", T("Komponen pihak ketiga: SDL3 (zlib), Dear ImGui (MIT), LLVM/Clang (Apache-2.0 dengan pengecualian LLVM), mingw-w64.",
                               "Third-party components: SDL3 (zlib), Dear ImGui (MIT), LLVM/Clang (Apache-2.0 with LLVM exception), mingw-w64."));
}

void Launcher::frame() {
    processDialogResults();
    pollProcesses();
    const auto lib = library_.games();
    if (!library_.scanning()) games_ = lib;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    // Background accent.
    ImDrawList* bg = ImGui::GetWindowDrawList();
    bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(io.DisplaySize.x, ImGui::GetFontSize() * 10),
                                ImGui::GetColorU32(ImVec4(0.30f, 0.05f, 0.08f, 0.35f)), ImGui::GetColorU32(ImVec4(0.10f, 0.05f, 0.06f, 0.0f)),
                                ImGui::GetColorU32(ImVec4(0, 0, 0, 0)), ImGui::GetColorU32(ImVec4(0, 0, 0, 0)));
    drawSidebar();
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("main", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    drawStatusBar();
    switch (page_) {
    case Page::Library: drawLibrary(); break;
    case Page::Settings: drawSettings(); break;
    case Page::Logs: drawLogs(); break;
    case Page::About: drawAbout(); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::End();
}

int Launcher::run(int argc, char** argv) {
    // Options for automated tests: --page N --select N --screenshot F --frames N
    int startPage = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) selfTestScreenshot_ = argv[++i];
        else if (a == "--frames" && i + 1 < argc) selfTestFrames_ = std::atoi(argv[++i]);
        else if (a == "--page" && i + 1 < argc) startPage = std::atoi(argv[++i]);
        else if (a == "--settings-tab" && i + 1 < argc) settingsTab_ = std::atoi(argv[++i]);
        else if (a == "--scan" && i + 1 < argc) library_.addFolder(argv[++i]);
    }
    page_ = (Page)std::clamp(startPage, 0, 3);
    settingsPath_ = joinPath(userDataDir(), "settings.json");
    settings_ = Settings::load(settingsPath_);
    if (!fileExists(settingsPath_)) settings_.save(settingsPath_);
    ui::setLanguage(settings_.system.language);

    SDL_SetHint(SDL_HINT_APP_NAME, "SB2 Heroes Recompiled");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window_ = createMainWindow("Sengoku Basara 2 Heroes Recompiled", 1280, 800, settings_.graphics.backend, true);
    if (!window_) {
        std::fprintf(stderr, "window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(window_, 960, 600);
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    const float scale = ui::displayScale(window_);
    ui::applyTheme(scale);
    fonts_ = ui::loadFonts(scale);
    presenter_ = createPresenter(settings_.graphics.backend, window_, settings_.graphics, presenterLog_);
    if (!presenter_) {
        std::fprintf(stderr, "%s", presenterLog_.c_str());
        return 1;
    }
    presenter_->setVsync(true, false);
    input_ = std::make_unique<InputManager>();
    toolchain_ = loadToolchain();
    library_.load();
    games_ = library_.games();
    library_.startScan();
    SDL_ShowWindow(window_);

    int frames = 0;
    while (!quit_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            input_->handleEvent(e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit_ = true;
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) presenter_->onResize(e.window.data1, e.window.data2);
        }
        input_->update(settings_, page_ == Page::Settings);
        presenter_->newImGuiFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        frame();
        ImGui::Render();
        if (!selfTestScreenshot_.empty() && selfTestFrames_ > 0 && frames == selfTestFrames_ - 1) presenter_->requestScreenshot();
        presenter_->render(RectF{}, PostParams{}, false);
        ++frames;
        if (!selfTestScreenshot_.empty() && selfTestFrames_ > 0 && frames >= selfTestFrames_) {
            std::vector<uint32_t> px;
            int w = 0, h = 0;
            if (presenter_->takeScreenshot(px, w, h)) {
                writePng(selfTestScreenshot_, px.data(), w, h);
                std::printf("screenshot: %s (%dx%d)\n", selfTestScreenshot_.c_str(), w, h);
                quit_ = true;
            } else if (frames > selfTestFrames_ + 10) {
                quit_ = true;
            } else {
                presenter_->requestScreenshot();
            }
        }
        // Idle politely when nothing changes.
        if (!builder_.running() && !library_.scanning() && !session_ && selfTestFrames_ == 0) SDL_Delay(4);
    }
    if (settingsDirty_) saveSettings();
    if (session_) session_->stop();
    session_.reset();
    input_.reset();
    presenter_.reset();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window_);
    SDL_Quit();
    return 0;
}

} // namespace
} // namespace sb2

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    sb2::Launcher l;
    return l.run(__argc, __argv);
}
#else
int main(int argc, char** argv) {
    sb2::Launcher l;
    return l.run(argc, argv);
}
#endif
