// Loads the PT_LOAD segments of a 32-bit little-endian MIPS ELF.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sb2 {

struct LoadedSegment {
    uint32_t vaddr = 0;
    uint32_t memsz = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> data;
};

struct LoadedElf {
    uint32_t entry = 0;
    std::vector<LoadedSegment> segments;
};

bool parseElfSegments(const std::vector<uint8_t>& bytes, LoadedElf& out, std::string& err);
bool loadElfSegments(const std::string& path, LoadedElf& out, std::string& err);

} // namespace sb2
