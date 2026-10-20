// Entry point for a recompiled program.  Parses the command line, creates
// the Machine and runs it.  The windowed front-end hooks in through
// ps2::setFrontend(); without one the program runs headless.
#include "ps2/host.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "ps2/gs.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"
#include "ps2/pad.hpp"
#include "ps2/spu2.hpp"

namespace ps2 {

namespace {
Frontend* g_frontend = nullptr;

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --elf FILE [options]\n"
                 "  --disc IMAGE          ISO/BIN/CUE used as cdrom0:\n"
                 "  --boot PATH           argv[0] passed to the game (default cdrom0:\\<elf>;1)\n"
                 "  --host DIR            directory backing host0:\n"
                 "  --saves DIR           memory card directory\n"
                 "  --headless            run without a window\n"
                 "  --frames N            exit after N video frames\n"
                 "  --max-cycles N        exit after N EE cycles\n"
                 "  --missing-entries F   append undiscovered code addresses to F\n"
                 "  --dump-frame F.ppm    write the last displayed frame to F\n"
                 "  --log MASK            debug log category mask (hex)\n"
                 "  --log-file F          also write log output to F\n"
                 "  --config F            settings file (written by the launcher)\n"
                 "  --test-pad HEX        hold these DualShock buttons on port 1 (tests)\n"
                 "  --dump-audio F.wav    write the SPU2 output to a WAV file (headless)\n",
                 argv0);
}

bool writePpm(const std::string& path, const GsFrame& f) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    o << "P6\n" << f.width << " " << f.height << "\n255\n";
    for (uint32_t p : f.pixels) {
        const char rgb[3] = {(char)(p & 0xFF), (char)((p >> 8) & 0xFF), (char)((p >> 16) & 0xFF)};
        o.write(rgb, 3);
    }
    return true;
}
bool writeWav(const std::string& path, const std::vector<int16_t>& pcm) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    auto u32 = [&](uint32_t v) { o.write((const char*)&v, 4); };
    auto u16 = [&](uint16_t v) { o.write((const char*)&v, 2); };
    const uint32_t bytes = (uint32_t)(pcm.size() * 2);
    o.write("RIFF", 4);
    u32(36 + bytes);
    o.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(48000);
    u32(48000 * 4);
    u16(4);
    u16(16);
    o.write("data", 4);
    u32(bytes);
    o.write((const char*)pcm.data(), bytes);
    return true;
}
} // namespace

void setFrontend(Frontend* f) { g_frontend = f; }
Frontend* frontend() { return g_frontend; }

int hostMain(int argc, char** argv, const ModuleInfo* mod) {
    RuntimeConfig cfg;
    std::string dumpFrame, configFile, dumpAudio;
    int testPad = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage(argv[0]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--elf") cfg.elfPath = next();
        else if (a == "--disc") cfg.discImage = next();
        else if (a == "--boot") cfg.bootPath = next();
        else if (a == "--host") cfg.hostRoot = next();
        else if (a == "--saves") cfg.saveDir = next();
        else if (a == "--headless") cfg.headless = true;
        else if (a == "--frames") cfg.maxFrames = std::stoull(next());
        else if (a == "--max-cycles") cfg.maxCycles = std::stoull(next());
        else if (a == "--missing-entries") cfg.missingEntriesFile = next();
        else if (a == "--dump-frame") dumpFrame = next();
        else if (a == "--log") logSetMask((uint32_t)std::stoul(next(), nullptr, 16));
        else if (a == "--log-file") logSetFile(next());
        else if (a == "--config") configFile = next();
        else if (a == "--test-pad") testPad = (int)std::stoul(next(), nullptr, 16);
        else if (a == "--dump-audio") dumpAudio = next();
        else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }
    if (cfg.elfPath.empty()) {
        usage(argv[0]);
        return 2;
    }
    if (cfg.bootPath.empty()) {
        std::string base = cfg.elfPath;
        const size_t s = base.find_last_of("/\\");
        if (s != std::string::npos) base = base.substr(s + 1);
        cfg.bootPath = cfg.discImage.empty() ? "host0:" + base : "cdrom0:\\" + base + ";1";
    }
    if (cfg.headless) cfg.realTimePacing = false;
    int rc;
    {
        Machine m(mod, cfg);
        if (testPad >= 0) {
            PadState ps;
            ps.buttons = (uint16_t)testPad;
            for (auto& p : ps.pressure) p = 0xFF;
            m.pad->setState(0, ps);
        }
        std::vector<int16_t> audio;
        if (!dumpAudio.empty())
            m.spu2->onSamples = [&](const int16_t* p, size_t frames) { audio.insert(audio.end(), p, p + frames * 2); };
        if (!cfg.headless && g_frontend) {
            rc = g_frontend->run(m, configFile);
        } else {
            rc = m.run();
        }
        if (!dumpFrame.empty()) {
            GsFrame f;
            if (m.gs->latestFrame(f)) writePpm(dumpFrame, f);
        }
        if (!dumpAudio.empty()) writeWav(dumpAudio, audio);
        if (m.missingEntryCount) logWarn("runtime: %llu dynamic jumps had no recompiled target", (unsigned long long)m.missingEntryCount);
    }
    std::fflush(stdout);
    return rc;
}

} // namespace ps2
