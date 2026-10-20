// Minimal ELF32 little-endian reader for PS2 EE executables.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace recomp {

struct ElfSegment {
    uint32_t vaddr = 0;
    uint32_t memsz = 0;
    uint32_t filesz = 0;
    uint32_t flags = 0; // PF_X=1, PF_W=2, PF_R=4
    std::vector<uint8_t> data; // filesz bytes
};

struct ElfSection {
    std::string name;
    uint32_t type = 0;
    uint32_t flags = 0; // SHF_WRITE=1, SHF_ALLOC=2, SHF_EXECINSTR=4
    uint32_t addr = 0;
    uint32_t size = 0;
    uint32_t offset = 0;
};

struct ElfSymbol {
    std::string name;
    uint32_t value = 0;
    uint32_t size = 0;
    uint8_t type = 0; // STT_FUNC=2
};

struct ElfImage {
    uint32_t entry = 0;
    uint32_t eflags = 0;
    std::vector<ElfSegment> segments;
    std::vector<ElfSection> sections;
    std::vector<ElfSymbol> symbols;

    // Lowest/highest address covered by loadable segments.
    uint32_t loadLow() const;
    uint32_t loadHigh() const;
    // Read a 32-bit word from loaded image (file data); nullopt if outside.
    std::optional<uint32_t> word(uint32_t addr) const;
    bool isExecAddr(uint32_t addr) const;
    // Executable ranges [start,end).
    std::vector<std::pair<uint32_t, uint32_t>> execRanges() const;
};

// Throws std::runtime_error on malformed input.
ElfImage loadElf(const std::vector<uint8_t>& bytes);
ElfImage loadElfFile(const std::string& path);

} // namespace recomp
