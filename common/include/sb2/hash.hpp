// Small self-contained hashing utilities (SHA-256, CRC32).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sb2 {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    std::vector<uint8_t> digest();
    std::string hexdigest();

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t bufLen_ = 0;
    uint64_t total_ = 0;
};

std::string sha256Hex(const void* data, size_t len);
// Returns empty string if the file cannot be read.
std::string sha256File(const std::string& path);

uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);

} // namespace sb2
