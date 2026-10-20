#include "sb2/scanner.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "sb2/disc.hpp"
#include "sb2/elf_loader.hpp"
#include "sb2/hash.hpp"

namespace sb2 {

namespace fs = std::filesystem;

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
int64_t mtimeOf(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}
} // namespace

const std::vector<KnownGame>& knownGames() {
    static const std::vector<KnownGame> games = {
        {"SLPM-66848", "Sengoku Basara 2 Heroes", 0xCA857E71u, "NTSC-J",
         "Target title: recompilation pipeline supported; game-specific compatibility in progress", true},
        {"SLKA-25412", "Sengoku Basara 2 Heroes (Korea)", 0, "NTSC-K",
         "Target title (Korean release): recompilation pipeline supported; untested", true},
    };
    return games;
}

std::string regionFromSerial(const std::string& serial) {
    const std::string p = serial.substr(0, std::min<size_t>(4, serial.size()));
    if (p == "SLPM" || p == "SLPS" || p == "SCPS" || p == "SLAJ" || p == "SCAJ" || p == "PAPX" || p == "SCPM") return "NTSC-J";
    if (p == "SLKA" || p == "SCKA") return "NTSC-K";
    if (p == "SLUS" || p == "SCUS" || p == "PBPX") return "NTSC-U";
    if (p == "SLES" || p == "SCES" || p == "SCED" || p == "SLED") return "PAL";
    return "Unknown";
}

std::string serialFromBootName(const std::string& name) {
    // SLPM_668.48 -> SLPM-66848
    std::string s;
    for (char c : name) {
        if (c == ';') break;
        s += c;
    }
    const size_t us = s.find('_');
    if (us == std::string::npos || us != 4) return {};
    std::string digits;
    for (size_t i = us + 1; i < s.size(); ++i)
        if (std::isdigit((unsigned char)s[i])) digits += s[i];
    std::string prefix = s.substr(0, 4);
    for (auto& c : prefix) c = (char)std::toupper((unsigned char)c);
    return prefix + "-" + digits;
}

uint32_t pcsx2ElfCrc(const std::vector<uint8_t>& elf) {
    uint32_t crc = 0;
    for (size_t i = 0; i + 4 <= elf.size(); i += 4) {
        uint32_t w;
        std::memcpy(&w, elf.data() + i, 4);
        crc ^= w;
    }
    return crc;
}

bool isDiscImagePath(const std::string& path) {
    const std::string e = lower(fs::path(path).extension().string());
    return e == ".iso" || e == ".bin" || e == ".img" || e == ".cue";
}

GameEntry analyzeImage(const std::string& path) {
    GameEntry g;
    g.path = path;
    g.fileName = fs::path(path).filename().string();
    std::error_code ec;
    g.fileSize = fs::file_size(path, ec);
    g.mtime = mtimeOf(path);
    const std::string ext = lower(fs::path(path).extension().string());
    g.format = ext == ".cue" ? "CUE" : (ext == ".iso" ? "ISO" : "BIN");
    std::string err;
    auto disc = DiscImage::open(path, err);
    if (!disc) {
        g.error = err;
        return g;
    }
    g.format = ext == ".cue" ? "CUE/" + disc->formatName() : disc->formatName();
    g.sectorCount = disc->sectorCount();
    if (disc->rawSectorSize() == 2048 && g.fileSize % 2048) g.warnings.push_back("file size is not a multiple of 2048 bytes");
    Iso9660 iso(*disc);
    if (!iso.valid()) {
        g.error = "no ISO9660 primary volume descriptor (not a PS2 data disc)";
        return g;
    }
    g.volumeId = iso.volumeId();
    g.systemId = iso.systemId();
    {
        uint8_t pvd[2048];
        if (disc->readSectors(16, 1, pvd)) std::memcpy(&g.volumeSectors, pvd + 80, 4);
        if (g.volumeSectors > g.sectorCount)
            g.warnings.push_back("image is truncated: volume has " + std::to_string(g.volumeSectors) + " sectors, image " +
                                 std::to_string(g.sectorCount));
    }
    auto cnf = iso.find("SYSTEM.CNF");
    if (!cnf) {
        g.error = "SYSTEM.CNF not found (not a PS2 disc)";
        return g;
    }
    std::vector<uint8_t> cnfData;
    iso.readFile(*cnf, cnfData, 4096);
    std::string text(cnfData.begin(), cnfData.end());
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string val = trim(line.substr(eq + 1));
        if (key == "boot2") g.bootPath = val;
        else if (key == "ver") g.version = val;
        else if (key == "vmode") g.vmode = val;
        else if (key == "boot" && g.bootPath.empty()) {
            g.error = "PlayStation 1 disc (BOOT=) is not supported";
            return g;
        }
    }
    if (g.bootPath.empty()) {
        g.error = "SYSTEM.CNF has no BOOT2 entry";
        return g;
    }
    g.elfName = normalizeIsoPath(g.bootPath);
    const size_t slash = g.elfName.rfind('\\');
    g.serial = serialFromBootName(slash == std::string::npos ? g.elfName : g.elfName.substr(slash + 1));
    g.region = regionFromSerial(g.serial);
    auto elfEntry = iso.find(g.bootPath);
    if (!elfEntry) {
        g.error = "boot executable " + g.bootPath + " not found on the disc";
        return g;
    }
    std::vector<uint8_t> elf;
    if (!iso.readFile(*elfEntry, elf) || elf.size() != elfEntry->size) {
        g.error = "cannot read the boot executable (damaged image?)";
        return g;
    }
    LoadedElf le;
    if (!parseElfSegments(elf, le, err)) {
        g.error = "boot executable is not a valid ELF: " + err;
        return g;
    }
    g.elfSize = (uint32_t)elf.size();
    g.elfEntry = le.entry;
    g.elfCrc = pcsx2ElfCrc(elf);
    g.elfSha256 = sha256Hex(elf.data(), elf.size());
    g.valid = true;
    g.title = g.volumeId.empty() ? g.serial : g.volumeId;
    g.compatibility = "Not supported (this build targets Sengoku Basara 2 Heroes)";
    for (const auto& k : knownGames()) {
        if (k.serial == g.serial) {
            g.known = true;
            g.target = k.target;
            g.title = k.title;
            g.exactVersion = k.elfCrc == 0 || k.elfCrc == g.elfCrc;
            g.compatibility = k.compatibility;
            if (!g.exactVersion) {
                char b[96];
                snprintf(b, sizeof b, "executable CRC %08X differs from the known release (%08X)", g.elfCrc, k.elfCrc);
                g.warnings.push_back(b);
            }
        }
    }
    return g;
}

Json GameEntry::toJson() const {
    Json j;
    j["path"] = path;
    j["fileName"] = fileName;
    j["format"] = format;
    j["fileSize"] = fileSize;
    j["mtime"] = mtime;
    j["valid"] = valid;
    j["error"] = error;
    Json w;
    w.arr();
    for (auto& s : warnings) w.push(s);
    j["warnings"] = w;
    j["volumeId"] = volumeId;
    j["systemId"] = systemId;
    j["serial"] = serial;
    j["region"] = region;
    j["version"] = version;
    j["vmode"] = vmode;
    j["bootPath"] = bootPath;
    j["elfName"] = elfName;
    j["sectorCount"] = sectorCount;
    j["volumeSectors"] = volumeSectors;
    j["elfSize"] = elfSize;
    char crc[16];
    snprintf(crc, sizeof crc, "%08X", elfCrc);
    j["elfCrc"] = std::string(crc);
    j["elfEntry"] = elfEntry;
    j["elfSha256"] = elfSha256;
    j["title"] = title;
    j["known"] = known;
    j["target"] = target;
    j["exactVersion"] = exactVersion;
    j["compatibility"] = compatibility;
    return j;
}

GameEntry GameEntry::fromJson(const Json& j) {
    GameEntry g;
    g.path = j.get("path").asString();
    g.fileName = j.get("fileName").asString();
    g.format = j.get("format").asString();
    g.fileSize = (uint64_t)j.get("fileSize").asInt();
    g.mtime = j.get("mtime").asInt();
    g.valid = j.get("valid").asBool();
    g.error = j.get("error").asString();
    for (auto& w : j.get("warnings").asArray()) g.warnings.push_back(w.asString());
    g.volumeId = j.get("volumeId").asString();
    g.systemId = j.get("systemId").asString();
    g.serial = j.get("serial").asString();
    g.region = j.get("region").asString();
    g.version = j.get("version").asString();
    g.vmode = j.get("vmode").asString();
    g.bootPath = j.get("bootPath").asString();
    g.elfName = j.get("elfName").asString();
    g.sectorCount = (uint32_t)j.get("sectorCount").asInt();
    g.volumeSectors = (uint32_t)j.get("volumeSectors").asInt();
    g.elfSize = (uint32_t)j.get("elfSize").asInt();
    g.elfCrc = (uint32_t)std::stoul(j.get("elfCrc").asString("0"), nullptr, 16);
    g.elfEntry = (uint32_t)j.get("elfEntry").asInt();
    g.elfSha256 = j.get("elfSha256").asString();
    g.title = j.get("title").asString();
    g.known = j.get("known").asBool();
    g.target = j.get("target").asBool();
    g.exactVersion = j.get("exactVersion").asBool();
    g.compatibility = j.get("compatibility").asString();
    return g;
}

std::vector<GameEntry> scanFolders(const std::vector<std::string>& dirs, const ScanOptions& opt) {
    // Load cache
    std::map<std::string, GameEntry> cache;
    if (!opt.cacheFile.empty()) {
        Json c = Json::load(opt.cacheFile);
        for (auto& e : c.get("games").asArray()) {
            GameEntry g = GameEntry::fromJson(e);
            cache[g.path] = g;
        }
    }
    // Collect candidate files. .bin files referenced by a .cue are skipped.
    std::vector<std::string> files;
    std::vector<std::string> cueBins;
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) {
            if (fs::is_regular_file(d, ec) && isDiscImagePath(d)) files.push_back(fs::absolute(d).string());
            continue;
        }
        auto visit = [&](const fs::directory_entry& de) {
            if (!de.is_regular_file()) return;
            const std::string p = de.path().string();
            if (isDiscImagePath(p)) files.push_back(fs::absolute(de.path()).string());
        };
        if (opt.recursive) {
            for (auto it = fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                visit(*it);
            }
        } else {
            for (auto& de : fs::directory_iterator(d, ec)) visit(de);
        }
    }
    for (const auto& f : files) {
        if (lower(fs::path(f).extension().string()) != ".cue") continue;
        std::ifstream cf(f);
        std::string line;
        while (std::getline(cf, line)) {
            const size_t q1 = line.find('"'), q2 = line.rfind('"');
            if (line.find("FILE") != std::string::npos && q1 != std::string::npos && q2 > q1)
                cueBins.push_back(fs::absolute(fs::path(f).parent_path() / line.substr(q1 + 1, q2 - q1 - 1)).string());
        }
    }
    std::vector<GameEntry> out;
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    size_t index = 0;
    for (const auto& f : files) {
        if (opt.cancel && opt.cancel->load()) break;
        ++index;
        if (std::find(cueBins.begin(), cueBins.end(), f) != cueBins.end()) continue;
        if (opt.progress) opt.progress(f, index, files.size());
        std::error_code ec;
        const uint64_t size = fs::file_size(f, ec);
        const int64_t mt = mtimeOf(f);
        auto it = cache.find(f);
        if (it != cache.end() && it->second.fileSize == size && it->second.mtime == mt) {
            out.push_back(it->second);
            continue;
        }
        out.push_back(analyzeImage(f));
    }
    if (!opt.cacheFile.empty()) {
        Json c;
        c["version"] = 1;
        Json arr;
        arr.arr();
        for (auto& g : out) arr.push(g.toJson());
        c["games"] = arr;
        std::error_code ec;
        fs::create_directories(fs::path(opt.cacheFile).parent_path(), ec);
        c.save(opt.cacheFile);
    }
    return out;
}

bool extractFromImage(const std::string& image, const std::string& isoPath, const std::string& outPath,
                      std::string& err) {
    auto disc = DiscImage::open(image, err);
    if (!disc) return false;
    Iso9660 iso(*disc);
    auto e = iso.find(isoPath);
    if (!e) {
        err = "file not found in image: " + isoPath;
        return false;
    }
    std::vector<uint8_t> data;
    if (!iso.readFile(*e, data)) {
        err = "read error";
        return false;
    }
    std::error_code ec;
    fs::create_directories(fs::path(outPath).parent_path(), ec);
    std::ofstream o(outPath, std::ios::binary);
    o.write((const char*)data.data(), (std::streamsize)data.size());
    if (!o) {
        err = "cannot write " + outPath;
        return false;
    }
    return true;
}

} // namespace sb2
