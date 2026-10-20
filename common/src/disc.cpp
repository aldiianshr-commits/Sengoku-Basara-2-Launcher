#include "sb2/disc.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace sb2 {

namespace fs = std::filesystem;

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}
const uint8_t kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

// Parse the first data track of a CUE sheet. Returns the BIN path.
bool parseCue(const std::string& cuePath, std::string& bin, uint32_t& rawSize, uint64_t& offset, std::string& err) {
    std::ifstream f(cuePath);
    if (!f) {
        err = "cannot open cue sheet";
        return false;
    }
    std::string line, curFile;
    bool inData = false;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string kw;
        ss >> kw;
        kw = upper(kw);
        if (kw == "FILE") {
            const size_t q1 = line.find('"'), q2 = line.rfind('"');
            curFile = (q1 != std::string::npos && q2 > q1) ? line.substr(q1 + 1, q2 - q1 - 1) : std::string();
            if (curFile.empty()) ss >> curFile;
        } else if (kw == "TRACK") {
            std::string num, mode;
            ss >> num >> mode;
            mode = upper(mode);
            if (mode.rfind("MODE", 0) == 0 && bin.empty()) {
                inData = true;
                rawSize = mode.find("2048") != std::string::npos ? 2048 : 2352;
                bin = curFile;
            } else {
                inData = false;
            }
        } else if (kw == "INDEX" && inData) {
            std::string idx, msf;
            ss >> idx >> msf;
            if (idx == "01" && msf.size() >= 8) {
                const int mm = std::stoi(msf.substr(0, 2)), sec = std::stoi(msf.substr(3, 2)),
                          fr = std::stoi(msf.substr(6, 2));
                offset = (uint64_t)((mm * 60 + sec) * 75 + fr) * rawSize;
            }
            inData = false;
        }
    }
    if (bin.empty()) {
        err = "cue sheet has no data track";
        return false;
    }
    fs::path p = fs::path(cuePath).parent_path() / bin;
    bin = p.string();
    return true;
}
} // namespace

std::unique_ptr<DiscImage> DiscImage::open(const std::string& path, std::string& err) {
    std::unique_ptr<DiscImage> d(new DiscImage());
    std::string dataPath = path;
    uint32_t raw = 0;
    uint64_t trackOff = 0;
    const std::string ext = lower(fs::path(path).extension().string());
    if (ext == ".cue") {
        std::string bin;
        if (!parseCue(path, bin, raw, trackOff, err)) return nullptr;
        dataPath = bin;
    }
    d->f_.open(dataPath, std::ios::binary);
    if (!d->f_) {
        err = "cannot open " + dataPath;
        return nullptr;
    }
    std::error_code ec;
    const uint64_t size = fs::file_size(dataPath, ec);
    if (ec || size < 2048 * 17) {
        err = "file too small to be a disc image";
        return nullptr;
    }
    // Detect raw sectors by looking for a sync pattern.
    uint8_t head[16];
    d->f_.seekg((std::streamoff)trackOff);
    d->f_.read((char*)head, 16);
    if (raw == 0) raw = (std::memcmp(head, kSync, 12) == 0) ? 2352 : 2048;
    if (raw == 2352 && std::memcmp(head, kSync, 12) != 0) {
        // CUE said 2352 but no sync: treat as cooked
        raw = 2048;
    }
    d->rawSize_ = raw;
    if (raw == 2352) {
        const uint8_t mode = head[15];
        d->dataOffset_ = mode == 2 ? 24 : 16; // MODE2 form1 has an 8-byte subheader
    }
    d->trackOffset_ = trackOff;
    d->sectors_ = (uint32_t)((size - trackOff) / raw);
    d->dataPath_ = dataPath;
    return d;
}

std::string DiscImage::formatName() const {
    if (rawSize_ == 2048) return "ISO (2048)";
    return dataOffset_ == 24 ? "BIN (2352, MODE2)" : "BIN (2352, MODE1)";
}

bool DiscImage::readSectors(uint32_t lba, uint32_t count, uint8_t* out) {
    if ((uint64_t)lba + count > sectors_) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    if (rawSize_ == 2048) {
        f_.clear();
        f_.seekg((std::streamoff)(trackOffset_ + (uint64_t)lba * 2048));
        f_.read((char*)out, (std::streamsize)count * 2048);
        return (bool)f_ || f_.gcount() == (std::streamsize)count * 2048;
    }
    std::vector<uint8_t> buf(rawSize_);
    for (uint32_t i = 0; i < count; ++i) {
        f_.clear();
        f_.seekg((std::streamoff)(trackOffset_ + (uint64_t)(lba + i) * rawSize_));
        f_.read((char*)buf.data(), rawSize_);
        if (f_.gcount() != (std::streamsize)rawSize_) return false;
        std::memcpy(out + (size_t)i * 2048, buf.data() + dataOffset_, 2048);
    }
    return true;
}

// ------------------------------------------------------------------ ISO9660

Iso9660::Iso9660(DiscImage& disc) : disc_(disc) {
    uint8_t pvd[2048];
    if (!disc_.readSectors(16, 1, pvd)) return;
    if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) return;
    systemId_ = trim(std::string((const char*)pvd + 8, 32));
    volumeId_ = trim(std::string((const char*)pvd + 40, 32));
    const uint8_t* root = pvd + 156;
    std::memcpy(&rootLba_, root + 2, 4);
    std::memcpy(&rootSize_, root + 10, 4);
    valid_ = rootLba_ != 0;
}

std::vector<IsoEntry> Iso9660::readDir(uint32_t lba, uint32_t size) {
    std::vector<IsoEntry> out;
    if (size > 16 * 1024 * 1024) return out;
    const uint32_t nsec = (size + 2047) / 2048;
    std::vector<uint8_t> buf((size_t)nsec * 2048);
    if (!disc_.readSectors(lba, nsec, buf.data())) return out;
    size_t pos = 0;
    while (pos < size) {
        const uint8_t len = buf[pos];
        if (len == 0) { // skip to next sector
            pos = (pos / 2048 + 1) * 2048;
            continue;
        }
        if (pos + len > buf.size()) break;
        const uint8_t* r = buf.data() + pos;
        const uint8_t nameLen = r[32];
        IsoEntry e;
        std::memcpy(&e.lba, r + 2, 4);
        std::memcpy(&e.size, r + 10, 4);
        e.isDir = (r[25] & 2) != 0;
        std::string name((const char*)r + 33, nameLen);
        if (!(nameLen == 1 && (r[33] == 0 || r[33] == 1))) {
            const size_t semi = name.find(';');
            if (semi != std::string::npos) name = name.substr(0, semi);
            if (!name.empty() && name.back() == '.') name.pop_back();
            e.name = name;
            out.push_back(e);
        }
        pos += len;
    }
    return out;
}

std::string normalizeIsoPath(const std::string& p) {
    std::string s = p;
    const size_t colon = s.find(':');
    if (colon != std::string::npos) s = s.substr(colon + 1);
    const size_t semi = s.find(';');
    if (semi != std::string::npos) s = s.substr(0, semi);
    for (auto& c : s)
        if (c == '/') c = '\\';
    while (!s.empty() && s.front() == '\\') s.erase(s.begin());
    return upper(s);
}

std::vector<IsoEntry> Iso9660::list(const std::string& dirPath) {
    if (!valid_) return {};
    std::string norm = normalizeIsoPath(dirPath);
    uint32_t lba = rootLba_, size = rootSize_;
    std::stringstream ss(norm);
    std::string part;
    while (std::getline(ss, part, '\\')) {
        if (part.empty()) continue;
        bool found = false;
        for (auto& e : readDir(lba, size)) {
            if (e.isDir && upper(e.name) == part) {
                lba = e.lba;
                size = e.size;
                found = true;
                break;
            }
        }
        if (!found) return {};
    }
    return readDir(lba, size);
}

std::optional<IsoEntry> Iso9660::find(const std::string& path) {
    if (!valid_) return std::nullopt;
    std::string norm = normalizeIsoPath(path);
    const size_t slash = norm.rfind('\\');
    const std::string dir = slash == std::string::npos ? std::string() : norm.substr(0, slash);
    const std::string file = slash == std::string::npos ? norm : norm.substr(slash + 1);
    for (auto& e : list(dir))
        if (upper(e.name) == file) return e;
    return std::nullopt;
}

bool Iso9660::readFile(const IsoEntry& e, std::vector<uint8_t>& out, uint64_t maxBytes) {
    const uint32_t len = (uint32_t)std::min<uint64_t>(e.size, maxBytes);
    out.resize(len);
    return read(e, 0, len, out.data());
}

bool Iso9660::read(const IsoEntry& e, uint64_t offset, uint32_t len, uint8_t* out) {
    if (offset >= e.size) return len == 0;
    len = (uint32_t)std::min<uint64_t>(len, e.size - offset);
    const uint32_t firstSec = e.lba + (uint32_t)(offset / 2048);
    const uint32_t skip = (uint32_t)(offset % 2048);
    const uint32_t nsec = (skip + len + 2047) / 2048;
    std::vector<uint8_t> buf((size_t)nsec * 2048);
    if (!disc_.readSectors(firstSec, nsec, buf.data())) return false;
    std::memcpy(out, buf.data() + skip, len);
    return true;
}

} // namespace sb2
