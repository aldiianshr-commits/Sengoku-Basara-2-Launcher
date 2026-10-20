#include "sb2/elf_loader.hpp"

#include <cstring>
#include <fstream>

namespace sb2 {

bool parseElfSegments(const std::vector<uint8_t>& b, LoadedElf& out, std::string& err) {
    auto rd32 = [&](size_t o) {
        uint32_t v = 0;
        if (o + 4 <= b.size()) std::memcpy(&v, b.data() + o, 4);
        return v;
    };
    auto rd16 = [&](size_t o) {
        uint16_t v = 0;
        if (o + 2 <= b.size()) std::memcpy(&v, b.data() + o, 2);
        return v;
    };
    if (b.size() < 52 || std::memcmp(b.data(), "\x7f" "ELF", 4) != 0) {
        err = "not an ELF file";
        return false;
    }
    if (b[4] != 1 || b[5] != 1 || rd16(18) != 8) {
        err = "not a 32-bit little-endian MIPS ELF";
        return false;
    }
    out = LoadedElf{};
    out.entry = rd32(24);
    const uint32_t phoff = rd32(28);
    const uint16_t phentsize = rd16(42), phnum = rd16(44);
    for (uint32_t i = 0; i < phnum; ++i) {
        const size_t o = phoff + (size_t)i * phentsize;
        if (o + 32 > b.size()) break;
        if (rd32(o) != 1) continue;
        LoadedSegment s;
        const uint32_t off = rd32(o + 4);
        s.vaddr = rd32(o + 8);
        const uint32_t filesz = rd32(o + 16);
        s.memsz = rd32(o + 20);
        s.flags = rd32(o + 24);
        if ((size_t)off + filesz > b.size()) {
            err = "segment extends past end of file";
            return false;
        }
        s.data.assign(b.begin() + off, b.begin() + off + filesz);
        out.segments.push_back(std::move(s));
    }
    if (out.segments.empty()) {
        err = "no loadable segments";
        return false;
    }
    return true;
}

bool loadElfSegments(const std::string& path, LoadedElf& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseElfSegments(bytes, out, err);
}

} // namespace sb2
