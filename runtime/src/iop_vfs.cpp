#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "ps2/cdvd.hpp"
#include "ps2/iop.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"

namespace ps2 {

namespace fs = std::filesystem;

namespace {
constexpr int kEIO = -5, kENOENT = -2, kEBADF = -9, kEINVAL = -22, kEEXIST = -17, kENOTDIR = -20;
constexpr int kO_WRONLY = 2, kO_RDWR = 3, kO_APPEND = 0x100, kO_CREAT = 0x200, kO_TRUNC = 0x400;

std::string lowerStr(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void splitDevice(const std::string& path, std::string& dev, std::string& rest) {
    const size_t c = path.find(':');
    if (c == std::string::npos) {
        dev = "";
        rest = path;
        return;
    }
    dev = lowerStr(path.substr(0, c));
    rest = path.substr(c + 1);
}
} // namespace

struct Vfs::Handle {
    enum Kind { Host, Iso, Console, Dir } kind = Host;
    std::fstream file;
    sb2::IsoEntry iso;
    uint64_t pos = 0;
    std::vector<std::pair<std::string, VfsStat>> dirEntries;
    size_t dirIndex = 0;
};

Vfs::Vfs(Machine& m) : m_(m) {}
Vfs::~Vfs() = default;

std::string Vfs::hostPath(const std::string& path) const {
    std::string dev, rest;
    splitDevice(path, dev, rest);
    std::string base;
    if (dev == "host" || dev == "host0") base = m_.cfg.hostRoot.empty() ? std::string(".") : m_.cfg.hostRoot;
    else if (dev == "mc0" || dev == "mc1") base = (fs::path(m_.cfg.saveDir.empty() ? "saves" : m_.cfg.saveDir) / dev).string();
    else return {};
    for (auto& c : rest)
        if (c == '\\') c = '/';
    while (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
    // Reject attempts to escape the sandbox.
    if (rest.find("..") != std::string::npos) return {};
    return (fs::path(base) / rest).string();
}

int Vfs::open(const std::string& path, int mode) {
    std::string dev, rest;
    splitDevice(path, dev, rest);
    auto h = std::make_unique<Handle>();
    if (dev == "tty" || dev == "tty0") {
        h->kind = Handle::Console;
    } else if (dev.rfind("cdrom", 0) == 0) {
        if (!m_.cdvd->present() || !m_.cdvd->fs()) return kENOENT;
        auto e = m_.cdvd->fs()->find(rest);
        if (!e || e->isDir) return kENOENT;
        h->kind = Handle::Iso;
        h->iso = *e;
    } else {
        const std::string hp = hostPath(path);
        if (hp.empty()) {
            PS2_DEBUG(Iop, "vfs: unsupported device in '%s'", path.c_str());
            return kENOENT;
        }
        std::ios::openmode om = std::ios::binary;
        const int acc = mode & 3;
        if (acc == kO_WRONLY || acc == kO_RDWR) {
            om |= std::ios::out;
            if (acc == kO_RDWR) om |= std::ios::in;
            if (mode & kO_APPEND) om |= std::ios::app;
            if (mode & kO_TRUNC) om |= std::ios::trunc;
            if (mode & kO_CREAT) {
                std::error_code ec;
                fs::create_directories(fs::path(hp).parent_path(), ec);
                if (!fs::exists(hp)) std::ofstream(hp, std::ios::binary).close();
            }
        } else {
            om |= std::ios::in;
        }
        h->file.open(hp, om);
        if (!h->file) return kENOENT;
        h->kind = Handle::Host;
    }
    const int fd = nextFd_++;
    handles_[fd] = std::move(h);
    return fd;
}

int Vfs::close(int fd) {
    if (fd >= 0 && fd < 3) return 0;
    return handles_.erase(fd) ? 0 : kEBADF;
}

int Vfs::read(int fd, uint8_t* buf, int size) {
    auto it = handles_.find(fd);
    if (it == handles_.end()) return kEBADF;
    Handle& h = *it->second;
    if (size <= 0) return 0;
    if (h.kind == Handle::Iso) {
        const uint64_t left = h.pos < h.iso.size ? h.iso.size - h.pos : 0;
        const uint32_t n = (uint32_t)std::min<uint64_t>((uint64_t)size, left);
        if (n && !m_.cdvd->fs()->read(h.iso, h.pos, n, buf)) return kEIO;
        h.pos += n;
        return (int)n;
    }
    if (h.kind == Handle::Host) {
        h.file.read((char*)buf, size);
        const int n = (int)h.file.gcount();
        h.file.clear();
        return n;
    }
    return 0;
}

int Vfs::write(int fd, const uint8_t* buf, int size) {
    if (fd == 1 || fd == 2) {
        m_.console((const char*)buf, (size_t)size);
        return size;
    }
    auto it = handles_.find(fd);
    if (it == handles_.end()) return kEBADF;
    Handle& h = *it->second;
    if (h.kind == Handle::Console) {
        m_.console((const char*)buf, (size_t)size);
        return size;
    }
    if (h.kind != Handle::Host) return kEIO;
    h.file.write((const char*)buf, size);
    h.file.flush();
    return h.file ? size : kEIO;
}

int64_t Vfs::lseek(int fd, int64_t off, int whence) {
    auto it = handles_.find(fd);
    if (it == handles_.end()) return kEBADF;
    Handle& h = *it->second;
    if (h.kind == Handle::Iso) {
        int64_t base = whence == 0 ? 0 : (whence == 1 ? (int64_t)h.pos : (int64_t)h.iso.size);
        const int64_t np = base + off;
        if (np < 0) return kEINVAL;
        h.pos = (uint64_t)np;
        return np;
    }
    if (h.kind == Handle::Host) {
        h.file.clear();
        const std::ios::seekdir d = whence == 0 ? std::ios::beg : (whence == 1 ? std::ios::cur : std::ios::end);
        h.file.seekg(off, d);
        h.file.seekp(h.file.tellg());
        return (int64_t)h.file.tellg();
    }
    return 0;
}

int Vfs::getstat(const std::string& path, VfsStat& st) {
    std::string dev, rest;
    splitDevice(path, dev, rest);
    if (dev.rfind("cdrom", 0) == 0) {
        if (!m_.cdvd->fs()) return kENOENT;
        auto e = m_.cdvd->fs()->find(rest);
        if (!e) return kENOENT;
        st.mode = e->isDir ? 0x1000 | 0x49 : 0x2000 | 0x24;
        st.size = e->size;
        return 0;
    }
    const std::string hp = hostPath(path);
    std::error_code ec;
    if (hp.empty() || !fs::exists(hp, ec)) return kENOENT;
    const bool dir = fs::is_directory(hp, ec);
    st.mode = dir ? 0x1000 | 0x1FF : 0x2000 | 0x1FF;
    st.attr = dir ? 0x8427 : 0x8497; // memory card style attributes
    st.size = dir ? 0 : fs::file_size(hp, ec);
    return 0;
}

int Vfs::mkdir(const std::string& path) {
    const std::string hp = hostPath(path);
    if (hp.empty()) return kEIO;
    std::error_code ec;
    if (fs::exists(hp, ec)) return kEEXIST;
    return fs::create_directories(hp, ec) ? 0 : kEIO;
}

int Vfs::remove(const std::string& path) {
    const std::string hp = hostPath(path);
    std::error_code ec;
    if (hp.empty() || !fs::remove(hp, ec)) return kENOENT;
    return 0;
}

int Vfs::rmdir(const std::string& path) { return remove(path); }

int Vfs::dopen(const std::string& path) {
    auto h = std::make_unique<Handle>();
    h->kind = Handle::Dir;
    std::string dev, rest;
    splitDevice(path, dev, rest);
    if (dev.rfind("cdrom", 0) == 0) {
        if (!m_.cdvd->fs()) return kENOENT;
        for (auto& e : m_.cdvd->fs()->list(rest)) {
            VfsStat st;
            st.mode = e.isDir ? 0x1000 : 0x2000;
            st.size = e.size;
            h->dirEntries.push_back({e.name, st});
        }
    } else {
        const std::string hp = hostPath(path);
        std::error_code ec;
        if (hp.empty() || !fs::is_directory(hp, ec)) return kENOTDIR;
        VfsStat dot;
        dot.mode = 0x1000 | 0x1FF;
        h->dirEntries.push_back({".", dot});
        h->dirEntries.push_back({"..", dot});
        for (auto& de : fs::directory_iterator(hp, ec)) {
            VfsStat st;
            st.mode = de.is_directory() ? 0x1000 | 0x1FF : 0x2000 | 0x1FF;
            st.size = de.is_directory() ? 0 : de.file_size(ec);
            h->dirEntries.push_back({de.path().filename().string(), st});
        }
    }
    const int fd = nextFd_++;
    handles_[fd] = std::move(h);
    return fd;
}

int Vfs::dread(int fd, std::string& name, VfsStat& st) {
    auto it = handles_.find(fd);
    if (it == handles_.end() || it->second->kind != Handle::Dir) return kEBADF;
    Handle& h = *it->second;
    if (h.dirIndex >= h.dirEntries.size()) return 0;
    name = h.dirEntries[h.dirIndex].first;
    st = h.dirEntries[h.dirIndex].second;
    h.dirIndex++;
    return 1;
}

int Vfs::dclose(int fd) { return close(fd); }

} // namespace ps2
