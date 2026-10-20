// Game library scanner: finds PS2 disc images, identifies them (serial,
// region, version), validates their structure and extracts the boot ELF.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sb2/json.hpp"

namespace sb2 {

struct KnownGame {
    std::string serial;   // e.g. SLPM-66848
    std::string title;
    uint32_t elfCrc = 0;  // PCSX2-style CRC (XOR of all 32-bit words of the boot ELF), 0 = any
    std::string region;
    std::string compatibility; // status of this project for the title
    bool target = false;       // the title this project ports
};

struct GameEntry {
    std::string path;
    std::string fileName;
    std::string format;      // ISO / BIN / CUE
    uint64_t fileSize = 0;
    int64_t mtime = 0;
    bool valid = false;      // structurally valid PS2 disc
    std::string error;       // why it is not valid
    std::vector<std::string> warnings;
    std::string volumeId, systemId;
    std::string serial, region, version, vmode, bootPath, elfName;
    uint32_t sectorCount = 0, volumeSectors = 0;
    uint32_t elfSize = 0, elfCrc = 0, elfEntry = 0;
    std::string elfSha256;
    std::string title;
    bool known = false;      // matches a known game entry
    bool target = false;     // is Sengoku Basara 2 Heroes
    bool exactVersion = false; // executable CRC matches the known version
    std::string compatibility;

    Json toJson() const;
    static GameEntry fromJson(const Json& j);
};

const std::vector<KnownGame>& knownGames();
std::string regionFromSerial(const std::string& serial);
// "SLPM_668.48" -> "SLPM-66848"
std::string serialFromBootName(const std::string& name);
uint32_t pcsx2ElfCrc(const std::vector<uint8_t>& elf);

bool isDiscImagePath(const std::string& path);
GameEntry analyzeImage(const std::string& path);

struct ScanOptions {
    bool recursive = true;
    std::string cacheFile; // JSON cache of previous results (keyed by path+size+mtime)
    std::function<void(const std::string& path, size_t index, size_t total)> progress;
    const std::atomic<bool>* cancel = nullptr;
};
std::vector<GameEntry> scanFolders(const std::vector<std::string>& dirs, const ScanOptions& opt);

// Extract a file from a disc image (path inside the ISO filesystem).
bool extractFromImage(const std::string& image, const std::string& isoPath, const std::string& outPath,
                      std::string& err);

} // namespace sb2
