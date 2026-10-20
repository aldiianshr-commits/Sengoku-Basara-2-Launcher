// Module builder: turns a game disc into a native executable on the user's
// machine.
//
//   1. extract   boot ELF from the disc image (ISO/BIN/CUE)
//   2. recompile R5900 machine code -> C++ (ps2recomp, in-process)
//   3. compile   generated C++ with the bundled clang (parallel, incremental)
//   4. link      against the prebuilt runtime / front-end SDK
//   5. verify    run the executable headless for a few hundred frames and
//                collect code addresses reached dynamically that were not
//                discovered statically; repeat 2-5 with them (bounded)
//
// Everything is cached per game under <user data>/cache/<serial>-<sha8>.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sb2/json.hpp"
#include "sb2/scanner.hpp"
#include "sb2/settings.hpp"

namespace sb2 {

struct Toolchain {
    bool valid = false;
    std::string error;
    std::string root;               // SDK root (directory of sdk.json)
    std::string cxx;                // compiler driver
    std::vector<std::string> includeDirs;
    std::vector<std::string> cxxflags;
    std::vector<std::string> libs;  // static libraries (absolute paths), link order
    std::vector<std::string> syslibs;
    std::vector<std::string> ldflags;
    std::vector<std::string> extraObjects; // e.g. resource object with manifest + icon
    std::string exeSuffix;
    std::string sdkVersion;
    std::string sdkHash;            // changes when the runtime libraries change
    std::string compilerVersion;
};

// Locates <exe dir>/sdk/sdk.json (or $SB2_SDK).
Toolchain loadToolchain();
// Compiler flags for the host CPU (x86-64 / -v2 / -v3).
std::string hostCpuTarget();

struct BuildInfo {
    std::string dir;          // cache directory
    std::string exePath;
    std::string elfPath;
    bool built = false;       // executable present and up to date
    bool stale = false;       // executable exists but SDK/recompiler changed
    Json state;               // build.json contents
};

BuildInfo inspectBuild(const GameEntry& g, const Toolchain& tc);

class ModuleBuilder {
public:
    enum class Stage { Idle, Extract, Recompile, Compile, Link, Verify, Done, Failed, Cancelled };
    struct Progress {
        Stage stage = Stage::Idle;
        double fraction = 0.0;   // overall 0..1
        std::string message;
        size_t filesDone = 0, filesTotal = 0;
        int pass = 1;
    };

    ModuleBuilder();
    ~ModuleBuilder();
    bool start(const GameEntry& game, const Toolchain& tc, const Settings& settings, bool forceRebuild);
    void cancel();
    bool running() const { return running_; }
    Progress progress();
    std::vector<std::string> logTail(size_t maxLines);
    std::string logText();
    const std::string& gameSerial() const { return serial_; }
    static const char* stageName(Stage s);

private:
    void run(GameEntry game, Toolchain tc, Settings settings, bool force);
    void log(const std::string& line);
    void setStage(Stage s, double fraction, const std::string& msg);
    bool compileAll(const std::string& genDir, const std::string& objDir, const Toolchain& tc, int jobs, int optLevel,
                    std::vector<std::string>& objects, Json& hashes);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::mutex mutex_;
    Progress progress_;
    std::vector<std::string> log_;
    std::string serial_;
};

} // namespace sb2
