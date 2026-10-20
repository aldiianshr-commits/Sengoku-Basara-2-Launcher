#include "elf.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace recomp {

namespace {
template <typename T>
T rd(const std::vector<uint8_t>& b, size_t off) {
    if (off + sizeof(T) > b.size()) throw std::runtime_error("ELF: read out of bounds");
    T v;
    std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}
} // namespace

ElfImage loadElf(const std::vector<uint8_t>& b) {
    if (b.size() < 52 || std::memcmp(b.data(), "\x7f" "ELF", 4) != 0)
        throw std::runtime_error("not an ELF file");
    if (b[4] != 1) throw std::runtime_error("ELF: not 32-bit");
    if (b[5] != 1) throw std::runtime_error("ELF: not little-endian");
    if (rd<uint16_t>(b, 18) != 8) throw std::runtime_error("ELF: not a MIPS executable");

    ElfImage img;
    img.entry = rd<uint32_t>(b, 24);
    const uint32_t phoff = rd<uint32_t>(b, 28);
    const uint32_t shoff = rd<uint32_t>(b, 32);
    img.eflags = rd<uint32_t>(b, 36);
    const uint16_t phentsize = rd<uint16_t>(b, 42);
    const uint16_t phnum = rd<uint16_t>(b, 44);
    const uint16_t shentsize = rd<uint16_t>(b, 46);
    const uint16_t shnum = rd<uint16_t>(b, 48);
    const uint16_t shstrndx = rd<uint16_t>(b, 50);

    for (uint32_t i = 0; i < phnum; ++i) {
        const size_t o = phoff + (size_t)i * phentsize;
        const uint32_t type = rd<uint32_t>(b, o);
        if (type != 1 /*PT_LOAD*/) continue;
        ElfSegment s;
        const uint32_t off = rd<uint32_t>(b, o + 4);
        s.vaddr = rd<uint32_t>(b, o + 8);
        s.filesz = rd<uint32_t>(b, o + 16);
        s.memsz = rd<uint32_t>(b, o + 20);
        s.flags = rd<uint32_t>(b, o + 24);
        if ((size_t)off + s.filesz > b.size()) throw std::runtime_error("ELF: segment past end of file");
        s.data.assign(b.begin() + off, b.begin() + off + s.filesz);
        img.segments.push_back(std::move(s));
    }
    if (img.segments.empty()) throw std::runtime_error("ELF: no loadable segments");

    if (shoff != 0 && shnum != 0 && (size_t)shoff + (size_t)shnum * shentsize <= b.size()) {
        std::vector<ElfSection> raw;
        std::vector<uint32_t> nameOff, link;
        for (uint32_t i = 0; i < shnum; ++i) {
            const size_t o = shoff + (size_t)i * shentsize;
            ElfSection s;
            nameOff.push_back(rd<uint32_t>(b, o));
            s.type = rd<uint32_t>(b, o + 4);
            s.flags = rd<uint32_t>(b, o + 8);
            s.addr = rd<uint32_t>(b, o + 12);
            s.offset = rd<uint32_t>(b, o + 16);
            s.size = rd<uint32_t>(b, o + 20);
            link.push_back(rd<uint32_t>(b, o + 24));
            raw.push_back(s);
        }
        auto strAt = [&](uint32_t sec, uint32_t off) -> std::string {
            if (sec >= raw.size()) return {};
            size_t p = (size_t)raw[sec].offset + off;
            std::string out;
            while (p < b.size() && b[p] != 0 && out.size() < 512) out.push_back((char)b[p++]);
            return out;
        };
        for (uint32_t i = 0; i < raw.size(); ++i) raw[i].name = strAt(shstrndx, nameOff[i]);
        for (uint32_t i = 0; i < raw.size(); ++i) {
            if (raw[i].type != 2 /*SHT_SYMTAB*/) continue;
            const uint32_t strtab = link[i];
            for (uint32_t off = 0; off + 16 <= raw[i].size; off += 16) {
                const size_t o = raw[i].offset + off;
                if (o + 16 > b.size()) break;
                ElfSymbol sym;
                sym.name = strAt(strtab, rd<uint32_t>(b, o));
                sym.value = rd<uint32_t>(b, o + 4);
                sym.size = rd<uint32_t>(b, o + 8);
                sym.type = b[o + 12] & 0xF;
                const uint16_t shndx = rd<uint16_t>(b, o + 14);
                if (shndx == 0) continue; // undefined
                img.symbols.push_back(std::move(sym));
            }
        }
        img.sections = std::move(raw);
    }
    return img;
}

ElfImage loadElfFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return loadElf(bytes);
}

uint32_t ElfImage::loadLow() const {
    uint32_t lo = 0xFFFFFFFF;
    for (auto& s : segments) lo = std::min(lo, s.vaddr);
    return lo;
}
uint32_t ElfImage::loadHigh() const {
    uint32_t hi = 0;
    for (auto& s : segments) hi = std::max(hi, s.vaddr + s.memsz);
    return hi;
}

std::optional<uint32_t> ElfImage::word(uint32_t addr) const {
    for (auto& s : segments) {
        if (addr >= s.vaddr && (uint64_t)addr + 4 <= (uint64_t)s.vaddr + s.filesz) {
            uint32_t v;
            std::memcpy(&v, s.data.data() + (addr - s.vaddr), 4);
            return v;
        }
    }
    return std::nullopt;
}

std::vector<std::pair<uint32_t, uint32_t>> ElfImage::execRanges() const {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    bool haveExecSections = false;
    for (auto& s : sections) {
        if ((s.flags & 4) && (s.flags & 2) && s.size && s.type == 1 /*PROGBITS*/) {
            out.push_back({s.addr, s.addr + s.size});
            haveExecSections = true;
        }
    }
    if (!haveExecSections) {
        for (auto& s : segments)
            if (s.flags & 1) out.push_back({s.vaddr, s.vaddr + s.filesz});
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool ElfImage::isExecAddr(uint32_t addr) const {
    for (auto& r : execRanges())
        if (addr >= r.first && addr < r.second) return true;
    return false;
}

} // namespace recomp
