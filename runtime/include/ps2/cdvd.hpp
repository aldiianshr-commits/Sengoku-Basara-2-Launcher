// CD/DVD drive backed by a disc image.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "sb2/disc.hpp"

namespace ps2 {

class Machine;

class Cdvd {
public:
    explicit Cdvd(Machine& m);
    ~Cdvd();
    bool insert(const std::string& imagePath, std::string& err);
    bool present() const { return disc_ != nullptr; }
    bool readSectors(uint32_t lba, uint32_t count, uint8_t* out);
    sb2::Iso9660* fs() { return iso_.get(); }
    uint32_t sectorCount() const { return disc_ ? disc_->sectorCount() : 0; }
    uint64_t bytesRead = 0;

private:
    Machine& m_;
    std::unique_ptr<sb2::DiscImage> disc_;
    std::unique_ptr<sb2::Iso9660> iso_;
};

} // namespace ps2
