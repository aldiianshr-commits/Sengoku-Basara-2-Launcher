// sb2build: command-line front-end of the launcher's recompilation pipeline
// (used by automated tests and for scripting).
//
//   sb2build IMAGE [--force] [--jobs N] [--opt N] [--no-auto-entries]
//                  [--play] [--frames N] [--exit-screenshot FILE] [--headless]
//
// Exit code 0 when the build (and the optional run) succeeded.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "builder.hpp"
#include "game_session.hpp"
#include "sb2/platform.hpp"
#include "sb2/scanner.hpp"
#include "sb2/settings.hpp"

using namespace sb2;

int main(int argc, char** argv) {
    std::string image, shot;
    bool force = false, play = false, headless = false, autoEntries = true;
    int jobs = 0, opt = -1, frames = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--force") force = true;
        else if (a == "--jobs") jobs = std::atoi(next().c_str());
        else if (a == "--opt") opt = std::atoi(next().c_str());
        else if (a == "--no-auto-entries") autoEntries = false;
        else if (a == "--play") play = true;
        else if (a == "--frames") frames = std::atoi(next().c_str());
        else if (a == "--exit-screenshot") shot = next();
        else if (a == "--headless") headless = true;
        else if (a == "-h" || a == "--help") {
            std::printf("usage: sb2build IMAGE [--force] [--jobs N] [--opt N] [--no-auto-entries] [--play] [--frames N] "
                        "[--exit-screenshot FILE] [--headless]\n");
            return 0;
        } else image = a;
    }
    if (image.empty()) {
        std::fprintf(stderr, "sb2build: no disc image given\n");
        return 2;
    }
    const GameEntry g = analyzeImage(image);
    if (!g.valid) {
        std::fprintf(stderr, "sb2build: %s: %s\n", image.c_str(), g.error.c_str());
        return 3;
    }
    std::printf("game: %s [%s] %s, ELF %s (%u bytes)\n", g.title.c_str(), g.serial.c_str(), g.region.c_str(), g.bootPath.c_str(), g.elfSize);
    const Toolchain tc = loadToolchain();
    if (!tc.valid) {
        std::fprintf(stderr, "sb2build: toolchain: %s\n", tc.error.c_str());
        return 4;
    }
    std::printf("toolchain: %s, sdk %s\n", tc.compilerVersion.c_str(), tc.sdkHash.c_str());
    Settings s = Settings::load(joinPath(userDataDir(), "settings.json"));
    if (jobs > 0) s.system.compileJobs = jobs;
    if (opt >= 0) s.system.optimization = opt;
    s.system.autoRebuildMissing = autoEntries;
    ModuleBuilder b;
    BuildInfo bi = inspectBuild(g, tc);
    if (force || !bi.built || bi.stale) {
        b.start(g, tc, s, force);
        size_t shown = 0;
        while (b.running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const auto lines = b.logTail(100000);
            for (; shown < lines.size(); ++shown) std::printf("%s\n", lines[shown].c_str());
            std::fflush(stdout);
        }
        const auto lines = b.logTail(100000);
        for (; shown < lines.size(); ++shown) std::printf("%s\n", lines[shown].c_str());
        if (b.progress().stage != ModuleBuilder::Stage::Done) {
            std::fprintf(stderr, "sb2build: build failed: %s\n", b.progress().message.c_str());
            return 5;
        }
        bi = inspectBuild(g, tc);
    } else {
        std::printf("build: up to date (%s)\n", bi.exePath.c_str());
    }
    if (!play) return 0;
    std::vector<std::string> args = {bi.exePath, "--elf", bi.elfPath, "--disc", g.path, "--boot", g.bootPath,
                                     "--saves", joinPath(bi.dir, "memcards"), "--config", joinPath(userDataDir(), "settings.json")};
    if (frames > 0) args.insert(args.end(), {"--frames", std::to_string(frames)});
    if (!shot.empty()) args.insert(args.end(), {"--exit-screenshot", shot});
    if (headless) args.push_back("--headless");
    std::string out;
    const int rc = runProcess(args, out);
    std::printf("%s", out.c_str());
    std::printf("run: exit code %d\n", rc);
    return rc == 0 ? 0 : 6;
}
