// Disc image access: plain ISO (2048-byte sectors), raw BIN (2352-byte
// MODE1/MODE2 sectors, with or without a CUE sheet), and an ISO9660
// filesystem reader on top.
#pragma once
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sb2 {

class DiscImage {
public:
    // Opens .iso, .bin, .img or .cue. Returns nullptr and sets err on failure.
    static std::unique_ptr<DiscImage> open(const std::string& path, std::string& err);

    // Read `count` 2048-byte user-data sectors starting at LBA.
    bool readSectors(uint32_t lba, uint32_t count, uint8_t* out);
    uint32_t sectorCount() const { return sectors_; }
    uint32_t rawSectorSize() const { return rawSize_; }
    uint32_t dataOffset() const { return dataOffset_; }
    const std::string& dataPath() const { return dataPath_; }
    std::string formatName() const;

private:
    DiscImage() = default;
    std::ifstream f_;
    std::mutex mutex_;
    std::string dataPath_;
    uint32_t rawSize_ = 2048;
    uint32_t dataOffset_ = 0; // offset of user data inside a raw sector
    uint32_t sectors_ = 0;
    uint64_t trackOffset_ = 0;
};

struct IsoEntry {
    std::string name; // without ";1" version suffix
    uint32_t lba = 0;
    uint32_t size = 0;
    bool isDir = false;
};

class Iso9660 {
public:
    explicit Iso9660(DiscImage& disc);
    bool valid() const { return valid_; }
    std::string volumeId() const { return volumeId_; }
    std::string systemId() const { return systemId_; }
    // Path like "SYSTEM.CNF", "\\DATA\\FILE.BIN" or "cdrom0:\\X.ELF;1" (case-insensitive).
    std::optional<IsoEntry> find(const std::string& path);
    std::vector<IsoEntry> list(const std::string& dirPath);
    bool readFile(const IsoEntry& e, std::vector<uint8_t>& out, uint64_t maxBytes = UINT64_MAX);
    bool read(const IsoEntry& e, uint64_t offset, uint32_t len, uint8_t* out);

private:
    std::vector<IsoEntry> readDir(uint32_t lba, uint32_t size);
    DiscImage& disc_;
    bool valid_ = false;
    uint32_t rootLba_ = 0, rootSize_ = 0;
    std::string volumeId_, systemId_;
};

// Normalise "cdrom0:\\SLPM_123.45;1" -> "SLPM_123.45"
std::string normalizeIsoPath(const std::string& p);

} // namespace sb2
