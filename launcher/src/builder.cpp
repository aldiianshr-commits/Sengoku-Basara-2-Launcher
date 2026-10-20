#include "builder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <set>
#include <sstream>

#include "analysis.hpp"
#include "codegen.hpp"
#include "elf.hpp"
#include "sb2/hash.hpp"
#include "sb2/platform.hpp"

#ifndef SB2_VERSION
#define SB2_VERSION "0.0.0"
#endif

namespace sb2 {

namespace {

constexpr int kMaxPasses = 6;
constexpr const char* kExeBase = "SB2H-Game";

std::string resolve(const std::string& base, const std::string& p) {
    if (p.empty()) return p;
    const bool abs = p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':');
    if (abs) return p;
    return std::filesystem::path(std::u8string((const char8_t*)joinPath(base, p).c_str()))
        .lexically_normal()
        .generic_string();
}

std::vector<std::string> strList(const Json& j) {
    std::vector<std::string> v;
    if (j.isArray())
        for (const auto& e : j.asArray()) v.push_back(e.asString());
    return v;
}


std::set<uint32_t> readEntries(const std::string& path) {
    std::set<uint32_t> s;
    std::string text;
    if (!readTextFile(path, text)) return s;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        try {
            s.insert((uint32_t)std::stoul(line, nullptr, 16));
        } catch (...) {
        }
    }
    return s;
}

void writeEntries(const std::string& path, const std::set<uint32_t>& s) {
    std::string out = "# Extra function entry points discovered at run time\n";
    char b[16];
    for (uint32_t a : s) {
        std::snprintf(b, sizeof b, "%08x\n", a);
        out += b;
    }
    writeTextFile(path, out);
}

std::string cacheDirFor(const GameEntry& g) {
    std::string serial = g.serial.empty() ? "UNKNOWN" : g.serial;
    const std::string sha = g.elfSha256.size() >= 8 ? g.elfSha256.substr(0, 8) : "nohash";
    return joinPath(joinPath(userDataDir(), "cache"), serial + "-" + sha);
}

} // namespace

// ------------------------------------------------------------------ toolchain

std::string hostCpuTarget() {
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    const bool v2 = __builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt") && __builtin_cpu_supports("ssse3");
    const bool v3 = v2 && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi2") && __builtin_cpu_supports("fma");
    if (v3) return "x86-64-v3";
    if (v2) return "x86-64-v2";
#endif
#endif
    return "x86-64";
}

Toolchain loadToolchain() {
    Toolchain tc;
    std::string sdkJson;
    if (const char* env = std::getenv("SB2_SDK")) sdkJson = joinPath(env, "sdk.json");
    if (sdkJson.empty() || !fileExists(sdkJson)) sdkJson = joinPath(joinPath(exeDir(), "sdk"), "sdk.json");
    if (!fileExists(sdkJson)) sdkJson = joinPath(joinPath(exeDir(), "../sdk"), "sdk.json"); // tools/ subfolder
    if (!fileExists(sdkJson)) {
        tc.error = "SDK not found (" + sdkJson + ")";
        return tc;
    }
    std::string err;
    const Json j = Json::load(sdkJson, &err);
    if (!err.empty()) {
        tc.error = "invalid sdk.json: " + err;
        return tc;
    }
    tc.root = std::filesystem::path(std::u8string((const char8_t*)sdkJson.c_str())).parent_path().generic_string();
    tc.cxx = resolve(tc.root, j.get("cxx").asString());
    for (auto& s : strList(j.get("include"))) tc.includeDirs.push_back(resolve(tc.root, s));
    tc.cxxflags = strList(j.get("cxxflags"));
    for (auto& s : strList(j.get("libs"))) tc.libs.push_back(resolve(tc.root, s));
    tc.syslibs = strList(j.get("syslibs"));
    tc.ldflags = strList(j.get("ldflags"));
    for (auto& s : strList(j.get("objects"))) tc.extraObjects.push_back(resolve(tc.root, s));
    tc.exeSuffix = j.get("exe_suffix").asString();
    tc.sdkVersion = j.get("version").asString();
    // The SDK hash covers the manifest and every library (size + content
    // hash of the first and last 64 KB keeps this fast).
    Sha256 h;
    const std::string manifest = j.dump(0);
    h.update(manifest.data(), manifest.size());
    for (const auto& lib : tc.libs) {
        std::vector<uint8_t> d;
        if (!readFile(lib, d)) {
            tc.error = "missing SDK library " + lib;
            return tc;
        }
        const uint64_t n = d.size();
        h.update(&n, sizeof n);
        h.update(d.data(), std::min<size_t>(d.size(), 65536));
        if (d.size() > 65536) h.update(d.data() + d.size() - 65536, 65536);
    }
    tc.sdkHash = h.hexdigest().substr(0, 16);
    if (tc.cxx.empty() || (!fileExists(tc.cxx) && tc.cxx.find('/') != std::string::npos)) {
        tc.error = "compiler not found: " + tc.cxx;
        return tc;
    }
    std::string out;
    if (runProcess({tc.cxx, "--version"}, out) != 0) {
        tc.error = "compiler does not run: " + out;
        return tc;
    }
    tc.compilerVersion = out.substr(0, out.find('\n'));
    tc.valid = true;
    return tc;
}

BuildInfo inspectBuild(const GameEntry& g, const Toolchain& tc) {
    BuildInfo b;
    b.dir = cacheDirFor(g);
    b.exePath = joinPath(b.dir, std::string(kExeBase) + tc.exeSuffix);
    b.elfPath = joinPath(b.dir, "game.elf");
    std::string err;
    b.state = Json::load(joinPath(b.dir, "build.json"), &err);
    if (!err.empty() || !b.state.isObject()) b.state = Json();
    if (fileExists(b.exePath) && b.state.isObject() && b.state.get("success").asBool()) {
        const bool sameSdk = b.state.get("sdkHash").asString() == tc.sdkHash;
        const bool sameRecomp = b.state.get("recompiler").asString() == SB2_VERSION;
        b.built = true;
        b.stale = !(sameSdk && sameRecomp);
    }
    return b;
}

// ------------------------------------------------------------------ builder

ModuleBuilder::ModuleBuilder() = default;

ModuleBuilder::~ModuleBuilder() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

const char* ModuleBuilder::stageName(Stage s) {
    switch (s) {
    case Stage::Idle: return "idle";
    case Stage::Extract: return "extract";
    case Stage::Recompile: return "recompile";
    case Stage::Compile: return "compile";
    case Stage::Link: return "link";
    case Stage::Verify: return "verify";
    case Stage::Done: return "done";
    case Stage::Failed: return "failed";
    case Stage::Cancelled: return "cancelled";
    }
    return "?";
}

bool ModuleBuilder::start(const GameEntry& game, const Toolchain& tc, const Settings& settings, bool force) {
    if (running_) return false;
    if (thread_.joinable()) thread_.join();
    cancel_ = false;
    running_ = true;
    serial_ = game.serial;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        log_.clear();
        progress_ = Progress{};
    }
    thread_ = std::thread(&ModuleBuilder::run, this, game, tc, settings, force);
    return true;
}

void ModuleBuilder::cancel() { cancel_ = true; }

ModuleBuilder::Progress ModuleBuilder::progress() {
    std::lock_guard<std::mutex> lk(mutex_);
    return progress_;
}

std::vector<std::string> ModuleBuilder::logTail(size_t n) {
    std::lock_guard<std::mutex> lk(mutex_);
    const size_t start = log_.size() > n ? log_.size() - n : 0;
    return std::vector<std::string>(log_.begin() + (ptrdiff_t)start, log_.end());
}

std::string ModuleBuilder::logText() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::string s;
    for (auto& l : log_) s += l + "\n";
    return s;
}

void ModuleBuilder::log(const std::string& line) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::istringstream in(line);
    std::string l;
    while (std::getline(in, l)) log_.push_back(l);
    if (log_.size() > 20000) log_.erase(log_.begin(), log_.begin() + 5000);
}

void ModuleBuilder::setStage(Stage s, double fraction, const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        progress_.stage = s;
        progress_.fraction = fraction;
        progress_.message = msg;
    }
    log(std::string("[") + stageName(s) + "] " + msg);
}

bool ModuleBuilder::compileAll(const std::string& genDir, const std::string& objDir, const Toolchain& tc, int jobs, int optLevel,
                               std::vector<std::string>& objects, Json& hashes) {
    namespace fs = std::filesystem;
    std::vector<std::string> sources;
    for (const auto& e : fs::directory_iterator(fs::path(std::u8string((const char8_t*)genDir.c_str())))) {
        if (e.path().extension() == ".cpp") sources.push_back(e.path().generic_string());
    }
    std::sort(sources.begin(), sources.end());
    makeDirs(objDir);
    std::vector<std::string> baseArgs = {tc.cxx};
    for (auto& f : tc.cxxflags) baseArgs.push_back(f);
    baseArgs.push_back("-O" + std::to_string(optLevel));
    baseArgs.push_back("-march=" + hostCpuTarget());
    for (auto& d : tc.includeDirs) baseArgs.push_back("-I" + d);
    baseArgs.push_back("-I" + genDir);
    std::string flagKey;
    for (auto& a : baseArgs) flagKey += a + " ";

    struct Job {
        std::string src, obj, hash;
        bool needed;
    };
    std::vector<Job> work;
    for (const auto& src : sources) {
        std::vector<uint8_t> d;
        readFile(src, d);
        Sha256 h;
        h.update(d.data(), d.size());
        h.update(flagKey.data(), flagKey.size());
        const std::string hash = h.hexdigest();
        const std::string name = fs::path(src).stem().string();
        const std::string obj = joinPath(objDir, name + ".o");
        const bool upToDate = fileExists(obj) && hashes.get(name).asString() == hash;
        work.push_back({src, obj, hash, !upToDate});
        objects.push_back(obj);
    }
    const size_t total = work.size();
    size_t needed = 0;
    for (auto& w : work) needed += w.needed ? 1 : 0;
    log("compile: " + std::to_string(total) + " files, " + std::to_string(total - needed) + " up to date, " + std::to_string(jobs) +
        " parallel jobs, -march=" + hostCpuTarget());
    {
        std::lock_guard<std::mutex> lk(mutex_);
        progress_.filesTotal = total;
        progress_.filesDone = total - needed;
    }
    std::mutex qm;
    size_t next = 0;
    std::atomic<bool> failed{false};
    std::vector<std::thread> pool;
    const auto t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < std::max(1, jobs); ++t) {
        pool.emplace_back([&]() {
            for (;;) {
                size_t i;
                {
                    std::lock_guard<std::mutex> lk(qm);
                    while (next < work.size() && !work[next].needed) ++next;
                    if (next >= work.size() || failed || cancel_) return;
                    i = next++;
                }
                Job& j = work[i];
                std::vector<std::string> args = baseArgs;
                args.push_back("-c");
                args.push_back(j.src);
                args.push_back("-o");
                args.push_back(j.obj);
                std::string out;
                const int rc = runProcess(args, out);
                if (rc != 0) {
                    log("error compiling " + j.src + ":\n" + out.substr(0, 6000));
                    failed = true;
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(qm);
                    hashes[fs::path(j.src).stem().string()] = j.hash;
                }
                size_t done;
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    done = ++progress_.filesDone;
                    progress_.fraction = 0.25 + 0.6 * (double)done / (double)std::max<size_t>(1, total);
                    char m[128];
                    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    std::snprintf(m, sizeof m, "%zu / %zu (%.0f s)", done, total, el);
                    progress_.message = m;
                }
            }
        });
    }
    for (auto& t : pool) t.join();
    return !failed && !cancel_;
}

void ModuleBuilder::run(GameEntry game, Toolchain tc, Settings settings, bool force) {
    const auto t0 = std::chrono::steady_clock::now();
    auto finish = [&](Stage s, const std::string& msg) {
        setStage(s, s == Stage::Done ? 1.0 : progress().fraction, msg);
        running_ = false;
    };
    if (!tc.valid) return finish(Stage::Failed, "toolchain unavailable: " + tc.error);
    if (!game.valid) return finish(Stage::Failed, "disc image is not valid: " + game.error);
    BuildInfo info = inspectBuild(game, tc);
    makeDirs(info.dir);
    const std::string genDir = joinPath(info.dir, "gen");
    const std::string objDir = joinPath(info.dir, "obj");
    const std::string entriesPath = joinPath(info.dir, "entries.txt");
    const std::string foundPath = joinPath(info.dir, "entries_found.txt");
    log("game: " + game.title + " [" + game.serial + "] " + game.path);
    log("cache: " + info.dir);
    log("toolchain: " + tc.compilerVersion + " (SDK " + tc.sdkVersion + ", " + tc.sdkHash + ")");

    // 1. Extract
    setStage(Stage::Extract, 0.02, "extracting " + game.bootPath);
    if (force || !fileExists(info.elfPath) || sha256File(info.elfPath) != game.elfSha256) {
        std::string err;
        if (!extractFromImage(game.path, game.bootPath, info.elfPath, err)) return finish(Stage::Failed, "extract failed: " + err);
    }
    if (sha256File(info.elfPath) != game.elfSha256) return finish(Stage::Failed, "extracted ELF does not match the scanned hash");
    if (cancel_) return finish(Stage::Cancelled, "cancelled");

    Json state = info.state.isObject() ? info.state : Json();
    Json hashes = force ? Json() : state.get("objectHashes");
    if (!hashes.isObject()) hashes = Json(Json::Object{});
    const int jobs = settings.system.compileJobs > 0 ? settings.system.compileJobs : cpuThreads();
    const int opt = settings.system.optimization;
    bool success = false;
    std::string failure;
    Json recompStats;
    Json verify;

    for (int pass = 1; pass <= kMaxPasses; ++pass) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            progress_.pass = pass;
        }
        // 2. Recompile
        std::set<uint32_t> extra = readEntries(entriesPath);
        setStage(Stage::Recompile, 0.05, "pass " + std::to_string(pass) + ": recompiling (" + std::to_string(extra.size()) +
                                              " extra entry points)");
        try {
            removeTree(genDir);
            makeDirs(genDir);
            recomp::ElfImage elf = recomp::loadElfFile(info.elfPath);
            recomp::AnalysisOptions aopt;
            aopt.extraEntries = extra;
            recomp::Program prog(elf, aopt);
            recomp::CodegenOptions copt;
            copt.outDir = genDir;
            copt.moduleName = game.serial.empty() ? "game" : game.serial;
            copt.comments = false;
            copt.instructionsPerFile = 12000;
            copt.elfSha256 = game.elfSha256;
            const recomp::CodegenResult r = recomp::generate(prog, copt);
            const auto& st = prog.stats();
            Json js;
            js["instructions"] = (uint64_t)st.instructions;
            js["invalid"] = (uint64_t)st.invalid;
            js["functions"] = (uint64_t)r.functions;
            js["labels"] = (uint64_t)r.labels;
            js["files"] = (uint64_t)r.files;
            js["unsupported"] = (uint64_t)r.unsupported;
            js["extraEntries"] = (uint64_t)extra.size();
            recompStats = js;
            char m[200];
            std::snprintf(m, sizeof m, "%zu instructions, %zu functions, %zu files, %zu unsupported constructs", st.instructions,
                          r.functions, r.files, r.unsupported);
            log(m);
        } catch (const std::exception& e) {
            failure = std::string("recompilation failed: ") + e.what();
            break;
        }
        // main() for the game executable.
        writeTextFile(joinPath(genDir, "zz_main.cpp"),
                      "// Generated by the SB2H launcher.\n#include \"sb2/game.hpp\"\n#include \"recomp_funcs.h\"\n"
                      "#if defined(_WIN32)\n#include <windows.h>\n"
                      "int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return sb2::gameMain(__argc, __argv, &ps2_module_info); }\n"
                      "#else\nint main(int argc, char** argv) { return sb2::gameMain(argc, argv, &ps2_module_info); }\n#endif\n");
        if (cancel_) return finish(Stage::Cancelled, "cancelled");

        // 3. Compile
        setStage(Stage::Compile, 0.25, "compiling generated C++");
        std::vector<std::string> objects;
        if (!compileAll(genDir, objDir, tc, jobs, opt, objects, hashes)) {
            if (cancel_) return finish(Stage::Cancelled, "cancelled");
            failure = "compilation failed (see log)";
            break;
        }
        state["objectHashes"] = hashes;
        // Remove objects of files that no longer exist.
        {
            namespace fs = std::filesystem;
            std::set<std::string> keep(objects.begin(), objects.end());
            for (const auto& e : fs::directory_iterator(fs::path(std::u8string((const char8_t*)objDir.c_str()))))
                if (!keep.count(e.path().generic_string())) fs::remove(e.path());
        }

        // 4. Link
        setStage(Stage::Link, 0.86, "linking " + std::string(kExeBase) + tc.exeSuffix);
        {
            std::string rsp;
            auto q = [](const std::string& s) {
                std::string r = "\"";
                for (char c : s) {
                    if (c == '\\' || c == '"') r.push_back('\\');
                    r.push_back(c);
                }
                return r + "\"";
            };
            for (auto& o : objects) rsp += q(o) + "\n";
            for (auto& o : tc.extraObjects) rsp += q(o) + "\n";
            rsp += "-Wl,--start-group\n";
            for (auto& l : tc.libs) rsp += q(l) + "\n";
            rsp += "-Wl,--end-group\n";
            for (auto& l : tc.syslibs) rsp += "-l" + l + "\n";
            for (auto& f : tc.ldflags) rsp += f + "\n";
            const std::string rspPath = joinPath(info.dir, "link.rsp");
            writeTextFile(rspPath, rsp);
            const std::string tmpExe = info.exePath + ".new" + tc.exeSuffix;
            std::string out;
            const int rc = runProcess({tc.cxx, "-o", tmpExe, "@" + rspPath}, out);
            if (rc != 0) {
                log("link failed:\n" + out.substr(0, 8000));
                failure = "link failed (see log)";
                break;
            }
            std::error_code ec;
            std::filesystem::path from(std::u8string((const char8_t*)tmpExe.c_str())), to(std::u8string((const char8_t*)info.exePath.c_str()));
            std::filesystem::remove(to, ec);
            std::filesystem::rename(from, to, ec);
            if (ec) {
                failure = "cannot replace executable: " + ec.message();
                break;
            }
            log("linked " + info.exePath + " (" + std::to_string(fileSize(info.exePath) / 1024) + " KB)");
        }
        if (cancel_) return finish(Stage::Cancelled, "cancelled");

        // 5. Verify: run headless and look for dynamically reached code that
        // was not recompiled.
        setStage(Stage::Verify, 0.92, "pass " + std::to_string(pass) + ": verifying (headless boot test)");
        {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(std::u8string((const char8_t*)foundPath.c_str())), ec);
        }
        const std::string verifySaves = joinPath(info.dir, "verify_memcards");
        makeDirs(verifySaves);
        Process::Options po;
        po.args = {info.exePath, "--elf", info.elfPath, "--disc", game.path, "--boot", game.bootPath, "--headless",
                   "--frames", "900", "--saves", verifySaves, "--missing-entries", foundPath};
        std::string perr;
        auto proc = Process::start(po, perr);
        if (!proc) {
            failure = "cannot start verification run: " + perr;
            break;
        }
        std::string out;
        std::thread reader([&]() { out = proc->readAll(); });
        const auto vt0 = std::chrono::steady_clock::now();
        int code = 0;
        bool timedOut = false;
        while (!proc->tryWait(code)) {
            if (cancel_ || std::chrono::steady_clock::now() - vt0 > std::chrono::seconds(240)) {
                proc->kill();
                timedOut = !cancel_;
                code = proc->wait();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        reader.join();
        if (cancel_) return finish(Stage::Cancelled, "cancelled");
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - vt0).count();
        std::string tail = out.size() > 4000 ? out.substr(out.size() - 4000) : out;
        log("verification output (tail):\n" + tail);
        const std::set<uint32_t> found = readEntries(foundPath);
        std::set<uint32_t> merged = extra;
        size_t added = 0;
        for (uint32_t a : found) added += merged.insert(a).second ? 1 : 0;
        verify = Json();
        verify["exitCode"] = code;
        verify["timedOut"] = timedOut;
        verify["seconds"] = secs;
        verify["newEntries"] = (uint64_t)added;
        verify["pass"] = pass;
        char m[200];
        std::snprintf(m, sizeof m, "verification exit code %d after %.1f s, %zu new entry points%s", code, secs, added,
                      timedOut ? " (time limit)" : "");
        log(m);
        if (added > 0 && settings.system.autoRebuildMissing && pass < kMaxPasses) {
            writeEntries(entriesPath, merged);
            continue;
        }
        if (added > 0) writeEntries(entriesPath, merged);
        success = true;
        break;
    }

    state["success"] = success;
    state["sdkHash"] = tc.sdkHash;
    state["recompiler"] = SB2_VERSION;
    state["serial"] = game.serial;
    state["elfSha256"] = game.elfSha256;
    state["compiler"] = tc.compilerVersion;
    state["cpuTarget"] = hostCpuTarget();
    state["optimization"] = opt;
    state["recompile"] = recompStats;
    state["verify"] = verify;
    state["buildSeconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    state["builtAt"] = (int64_t)std::time(nullptr);
    if (!success) state["error"] = failure;
    state.save(joinPath(info.dir, "build.json"));
    writeTextFile(joinPath(info.dir, "build.log"), logText());
    if (success) finish(Stage::Done, "build complete");
    else finish(Stage::Failed, failure.empty() ? "build failed" : failure);
}

} // namespace sb2
