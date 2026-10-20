// ps2recomp: statically recompile a PS2 EE ELF into C++.
//
//   ps2recomp --elf GAME.ELF --out DIR [--name NAME] [--entries FILE] [--no-comments]
//
// FILE lists extra function entry points (hex addresses, one per line); the
// runtime appends addresses it could not dispatch to such a file so a second
// recompilation pass can include them.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

#include "analysis.hpp"
#include "codegen.hpp"
#include "elf.hpp"
#include "sb2/hash.hpp"

using namespace recomp;

static void usage() {
    std::fprintf(stderr,
                 "usage: ps2recomp --elf FILE --out DIR [--name NAME] [--entries FILE] [--no-comments]\n"
                 "                 [--per-file N] [--stats]\n");
}

int main(int argc, char** argv) {
    std::string elfPath, outDir, name = "game", entriesPath;
    bool comments = true, stats = false;
    size_t perFile = 16000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--elf") elfPath = next();
        else if (a == "--out") outDir = next();
        else if (a == "--name") name = next();
        else if (a == "--entries") entriesPath = next();
        else if (a == "--no-comments") comments = false;
        else if (a == "--per-file") perFile = std::stoul(next());
        else if (a == "--stats") stats = true;
        else {
            usage();
            return 2;
        }
    }
    if (elfPath.empty() || outDir.empty()) {
        usage();
        return 2;
    }
    try {
        ElfImage elf = loadElfFile(elfPath);
        AnalysisOptions aopt;
        if (!entriesPath.empty()) {
            std::ifstream f(entriesPath);
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty() || line[0] == '#') continue;
                aopt.extraEntries.insert((uint32_t)std::stoul(line, nullptr, 16));
            }
        }
        Program prog(elf, aopt);
        CodegenOptions copt;
        copt.outDir = outDir;
        copt.moduleName = name;
        copt.comments = comments;
        copt.instructionsPerFile = perFile;
        copt.elfSha256 = sb2::sha256File(elfPath);
        CodegenResult r = generate(prog, copt);
        const auto& s = prog.stats();
        std::printf("ps2recomp: %s -> %s\n", elfPath.c_str(), outDir.c_str());
        std::printf("  instructions: %zu (invalid: %zu)\n", s.instructions, s.invalid);
        std::printf("  functions: %zu  labels: %zu  files: %zu\n", r.functions, r.labels, r.files);
        if (stats) {
            std::printf("  entries: symbols=%zu calls=%zu datarefs=%zu prologues=%zu\n", s.entriesFromSymbols,
                        s.entriesFromCalls, s.entriesFromDataRefs, s.entriesFromPrologues);
            std::printf("  data->code refs: %zu  branch-in-delay-slot: %zu\n", s.dataCodeRefs,
                        s.branchInDelaySlot);
        }
        if (r.unsupported) std::printf("  WARNING: %zu unsupported constructs (trap at runtime)\n", r.unsupported);
        // Machine-readable summary for the launcher.
        std::ofstream js(std::string(outDir) + "/recomp_summary.json");
        js << "{\n  \"elf\": \"" << copt.elfSha256 << "\",\n  \"entry\": " << elf.entry
           << ",\n  \"instructions\": " << s.instructions << ",\n  \"invalid\": " << s.invalid
           << ",\n  \"functions\": " << r.functions << ",\n  \"files\": " << r.files
           << ",\n  \"unsupported\": " << r.unsupported << "\n}\n";
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ps2recomp: error: %s\n", e.what());
        return 1;
    }
    return 0;
}
