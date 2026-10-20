// sb2scan: scan folders or images and print the analysis as JSON.
#include <cstdio>
#include <string>
#include <vector>

#include "sb2/scanner.hpp"

int main(int argc, char** argv) {
    std::vector<std::string> paths;
    std::string cache, extract, out;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--cache" && i + 1 < argc) cache = argv[++i];
        else if (a == "--extract-elf" && i + 1 < argc) out = argv[++i];
        else paths.push_back(a);
    }
    if (paths.empty()) {
        std::fprintf(stderr, "usage: sb2scan [--cache FILE] [--extract-elf OUT] PATH...\n");
        return 2;
    }
    sb2::ScanOptions opt;
    opt.cacheFile = cache;
    auto games = sb2::scanFolders(paths, opt);
    sb2::Json arr;
    arr.arr();
    for (auto& g : games) arr.push(g.toJson());
    std::printf("%s\n", arr.dump().c_str());
    if (!out.empty() && !games.empty() && games[0].valid) {
        std::string err;
        if (!sb2::extractFromImage(games[0].path, games[0].bootPath, out, err)) {
            std::fprintf(stderr, "extract failed: %s\n", err.c_str());
            return 1;
        }
    }
    return 0;
}
