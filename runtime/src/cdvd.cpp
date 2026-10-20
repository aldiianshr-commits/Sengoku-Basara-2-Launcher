#include "ps2/cdvd.hpp"

#include "ps2/log.hpp"
#include "ps2/machine.hpp"

namespace ps2 {

Cdvd::Cdvd(Machine& m) : m_(m) {
    if (!m.cfg.discImage.empty()) {
        std::string err;
        if (!insert(m.cfg.discImage, err)) logError("cdvd: %s", err.c_str());
    }
}

Cdvd::~Cdvd() = default;

bool Cdvd::insert(const std::string& path, std::string& err) {
    disc_ = sb2::DiscImage::open(path, err);
    if (!disc_) return false;
    iso_ = std::make_unique<sb2::Iso9660>(*disc_);
    if (!iso_->valid()) {
        err = "disc image has no ISO9660 filesystem";
        iso_.reset();
        disc_.reset();
        return false;
    }
    logInfo("cdvd: inserted %s (%s, %u sectors, volume '%s')", path.c_str(), disc_->formatName().c_str(),
            disc_->sectorCount(), iso_->volumeId().c_str());
    return true;
}

bool Cdvd::readSectors(uint32_t lba, uint32_t count, uint8_t* out) {
    if (!disc_) return false;
    bytesRead += (uint64_t)count * 2048;
    return disc_->readSectors(lba, count, out);
}

} // namespace ps2
