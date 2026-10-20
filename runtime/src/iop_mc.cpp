// MCSERV (libmc) service backed by host folders.
//
// Each memory card is a directory (<saves>/mc0, <saves>/mc1); save folders
// and files map 1:1 to host directories and files, so saves can be backed up
// or inspected directly.  Both command numberings are served: MCMAN/MCSERV
// (0x70..) and XMCMAN/XMCSERV (0x01.., init 0xFE).
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

#include "iop_services.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"
#include "ps2/sif.hpp"

namespace ps2 {

using namespace hle;
namespace fs = std::filesystem;

namespace {

enum McOp { OP_INIT, OP_GETINFO, OP_OPEN, OP_CLOSE, OP_SEEK, OP_READ, OP_WRITE, OP_FLUSH, OP_CHDIR, OP_GETDIR, OP_SETINFO,
            OP_DELETE, OP_FORMAT, OP_UNFORMAT, OP_GETENT, OP_PRIORITY, OP_CHECKBLOCK, OP_RAWPAGE, OP_UNKNOWN };

constexpr int kResSucceed = 0, kResChangedCard = -1, kResNoEntry = -4, kResDeniedPermit = -5, kResNotEmpty = -6,
              kResUpLimitHandle = -7;
constexpr int kCardKB = 8 * 1024 - 400; // usable clusters of an 8 MB card
constexpr uint16_t kAttrDir = 0x8427, kAttrFile = 0x8497;

McOp decode(uint32_t fno, bool& xmc) {
    xmc = fno < 0x70 || fno == 0xFE;
    switch (fno) {
    case 0x70: case 0xFE: return OP_INIT;
    case 0x78: case 0x01: return OP_GETINFO;
    case 0x71: case 0x02: return OP_OPEN;
    case 0x72: case 0x03: return OP_CLOSE;
    case 0x75: case 0x04: return OP_SEEK;
    case 0x73: case 0x05: return OP_READ;
    case 0x74: case 0x06: return OP_WRITE;
    case 0x7A: case 0x0A: return OP_FLUSH;
    case 0x7B: case 0x0C: return OP_CHDIR;
    case 0x76: case 0x0D: return OP_GETDIR;
    case 0x7C: case 0x0E: return OP_SETINFO;
    case 0x79: case 0x0F: return OP_DELETE;
    case 0x77: case 0x10: return OP_FORMAT;
    case 0x80: case 0x11: return OP_UNFORMAT;
    case 0x12: return OP_GETENT;
    case 0x14: return OP_PRIORITY;
    case 0x33: return OP_CHECKBLOCK;
    case 0x7D: case 0x7E: case 0x7F: return OP_RAWPAGE;
    default: return OP_UNKNOWN;
    }
}

fs::path U8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string u8name(const fs::path& p) {
    const auto u = p.filename().u8string();
    return std::string(u.begin(), u.end());
}

bool wildMatch(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') return wildMatch(p + 1, s) || (*s && wildMatch(p, s + 1));
    if (*s && (*p == '?' || *p == *s)) return wildMatch(p + 1, s + 1);
    return false;
}

struct McFile {
    std::fstream f;
    fs::path path;
    int port = 0;
};

class McService final : public RpcServer {
public:
    explicit McService(Machine& m) : m_(m) {}
    const char* name() const override { return "MCSERV"; }
    int64_t latency(uint32_t) const override { return 40000; }

    std::vector<uint8_t> call(uint32_t fno, const std::vector<uint8_t>& in, uint32_t rsize) override {
        bool xmc = false;
        const McOp op = decode(fno, xmc);
        std::vector<uint8_t> out(std::max<uint32_t>(rsize, 4), 0);
        int r = 0;
        switch (op) {
        case OP_INIT:
            // mcRpcStat_t {result, mcserv_version, mcman_version}
            wr32(out, 0, 0);
            wr32(out, 4, 0x020B);
            wr32(out, 8, 0x020E);
            return out;
        case OP_GETINFO: r = getInfo(in, xmc); break;
        case OP_OPEN: r = open(in); break;
        case OP_CLOSE: r = files_.erase((int)rd32(in, 0)) ? kResSucceed : kResDeniedPermit; break;
        case OP_FLUSH: r = kResSucceed; break;
        case OP_SEEK: r = seek(in); break;
        case OP_READ: r = read(in); break;
        case OP_WRITE: r = write(in); break;
        case OP_CHDIR: r = chdir(in); break;
        case OP_GETDIR: r = getdir(in); break;
        case OP_SETINFO: r = setinfo(in); break;
        case OP_DELETE: r = remove(in); break;
        case OP_FORMAT: r = format(in, true); break;
        case OP_UNFORMAT: r = format(in, false); break;
        case OP_GETENT: r = 64; break;
        case OP_PRIORITY: r = 0; break;
        case OP_CHECKBLOCK: r = 0; break;
        case OP_RAWPAGE:
            logWarn("mc: raw page access is not supported with folder memory cards");
            r = kResDeniedPermit;
            break;
        default:
            logWarn("mc: unsupported function %02x", fno);
            r = kResDeniedPermit;
            break;
        }
        wr32(out, 0, (uint32_t)r);
        return out;
    }

private:
    fs::path cardRoot(int port) const {
        const std::string base = m_.cfg.saveDir.empty() ? std::string("saves") : m_.cfg.saveDir;
        fs::path p = U8(base) / (port ? "mc1" : "mc0");
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }

    // Resolve a memory-card path (absolute or relative to the current dir).
    bool resolve(int port, const std::string& name, fs::path& out, std::string* mcPath = nullptr) const {
        std::string p = name;
        for (auto& c : p)
            if (c == '\\') c = '/';
        std::string full = p.size() && p[0] == '/' ? p : (curDir_[port & 1] + "/" + p);
        // normalise
        std::vector<std::string> parts;
        size_t i = 0;
        while (i <= full.size()) {
            const size_t j = full.find('/', i);
            const std::string seg = full.substr(i, j == std::string::npos ? std::string::npos : j - i);
            if (seg == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (!seg.empty() && seg != ".") {
                parts.push_back(seg);
            }
            if (j == std::string::npos) break;
            i = j + 1;
        }
        std::string norm;
        out = cardRoot(port);
        for (auto& s : parts) {
            norm += "/" + s;
            out /= U8(s);
        }
        if (mcPath) *mcPath = norm.empty() ? "/" : norm;
        return true;
    }

    int usedKB(int port) const {
        uint64_t total = 0;
        std::error_code ec;
        for (auto& e : fs::recursive_directory_iterator(cardRoot(port), ec)) {
            if (e.is_regular_file(ec)) total += (e.file_size(ec) + 1023) / 1024;
            else total += 1; // directory cluster
        }
        return (int)total;
    }

    int getInfo(const std::vector<uint8_t>& in, bool xmc) {
        const int port = (int)rd32(in, 4) & 1;
        const uint32_t param = rd32(in, 28);
        const int freeKB = std::max(0, kCardKB - usedKB(port));
        if (param) {
            // mcEndParam(2)_t: type @0, free @4, [formatted @144 in the new layout]
            m_.write32(param + 0, 2); // sceMcTypePS2
            m_.write32(param + 4, (uint32_t)freeKB);
            m_.write32(param + 8, 0);
            m_.write32(param + 12, 0);
            if (xmc) m_.write32(param + 144, 1);
        }
        const bool first = !seen_[port];
        seen_[port] = true;
        return first ? kResChangedCard : kResSucceed;
    }

    int open(const std::vector<uint8_t>& in) {
        const int port = (int)rd32(in, 0) & 1;
        const int flags = (int)rd32(in, 8);
        const std::string name = rdStr(in, 20, 1024);
        fs::path p;
        std::string mcp;
        resolve(port, name, p, &mcp);
        std::error_code ec;
        if (flags & 0x40) { // create directory (sceMcMkdir)
            if (fs::exists(p, ec)) return kResNoEntry;
            if (!fs::exists(p.parent_path(), ec)) return kResNoEntry;
            fs::create_directory(p, ec);
            PS2_DEBUG(Mc, "mc%d mkdir %s", port, mcp.c_str());
            return ec ? kResDeniedPermit : kResSucceed;
        }
        if (files_.size() >= 16) return kResUpLimitHandle;
        const int acc = flags & 3;
        const bool create = (flags & 0x200) != 0, trunc = (flags & 0x400) != 0;
        if (fs::is_directory(p, ec)) return kResDeniedPermit;
        if (!fs::exists(p, ec)) {
            if (!create) return kResNoEntry;
            if (!fs::is_directory(p.parent_path(), ec)) return kResNoEntry;
            std::ofstream(p, std::ios::binary).close();
        } else if (trunc && (acc == 2 || acc == 3)) {
            std::ofstream(p, std::ios::binary | std::ios::trunc).close();
        }
        auto f = std::make_unique<McFile>();
        std::ios::openmode om = std::ios::binary | std::ios::in;
        if (acc == 2 || acc == 3 || create) om |= std::ios::out;
        f->f.open(p, om);
        if (!f->f) return kResDeniedPermit;
        f->path = p;
        f->port = port;
        const int fd = nextFd_++;
        files_[fd] = std::move(f);
        PS2_DEBUG(Mc, "mc%d open %s flags=%x -> %d", port, mcp.c_str(), flags, fd);
        return fd;
    }

    int seek(const std::vector<uint8_t>& in) {
        auto it = files_.find((int)rd32(in, 0));
        if (it == files_.end()) return kResDeniedPermit;
        std::fstream& f = it->second->f;
        f.clear();
        const int origin = (int)rd32(in, 20);
        const int32_t off = (int32_t)rd32(in, 16);
        f.seekg(off, origin == 0 ? std::ios::beg : (origin == 1 ? std::ios::cur : std::ios::end));
        const std::streamoff pos = f.tellg();
        f.seekp(pos);
        return (int)pos;
    }

    int read(const std::vector<uint8_t>& in) {
        auto it = files_.find((int)rd32(in, 0));
        if (it == files_.end()) return kResDeniedPermit;
        const int size = (int)rd32(in, 12);
        const uint32_t buf = rd32(in, 24), param = rd32(in, 28);
        std::vector<uint8_t> data((size_t)std::max(0, size));
        std::fstream& f = it->second->f;
        f.clear();
        f.read((char*)data.data(), size);
        const int n = (int)f.gcount();
        f.clear();
        f.seekp(f.tellg());
        for (int i = 0; i < n; ++i) m_.write8(buf + (uint32_t)i, data[(size_t)i]);
        if (param) {
            m_.write32(param + 0, 0); // size1: no unaligned head to fix up
            m_.write32(param + 4, 0); // size2
        }
        return n;
    }

    int write(const std::vector<uint8_t>& in) {
        auto it = files_.find((int)rd32(in, 0));
        if (it == files_.end()) return kResDeniedPermit;
        const int size = (int)rd32(in, 12), head = (int)rd32(in, 20);
        const uint32_t buf = rd32(in, 24);
        std::vector<uint8_t> data;
        for (int i = 0; i < head && i < 16; ++i) data.push_back(in.size() > (size_t)(32 + i) ? in[(size_t)(32 + i)] : 0);
        for (int i = 0; i < size; ++i) data.push_back(m_.read8(buf + (uint32_t)i));
        std::fstream& f = it->second->f;
        f.clear();
        f.write((const char*)data.data(), (std::streamsize)data.size());
        f.flush();
        f.seekg(f.tellp());
        return f ? (int)data.size() : kResDeniedPermit;
    }

    int chdir(const std::vector<uint8_t>& in) {
        const int port = (int)rd32(in, 0) & 1;
        const uint32_t curBuf = rd32(in, 16);
        const std::string name = rdStr(in, 20, 1024);
        if (curBuf) {
            const std::string& cur = curDir_[port].empty() ? std::string("/") : curDir_[port];
            for (size_t i = 0; i <= cur.size(); ++i) m_.write8(curBuf + (uint32_t)i, i < cur.size() ? (uint8_t)cur[i] : 0);
        }
        fs::path p;
        std::string mcp;
        resolve(port, name, p, &mcp);
        std::error_code ec;
        if (!fs::is_directory(p, ec)) return kResNoEntry;
        curDir_[port] = mcp == "/" ? "" : mcp;
        return kResSucceed;
    }

    static void putDate(uint8_t* d, std::time_t t) {
        std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        d[0] = 0;
        d[1] = (uint8_t)lt.tm_sec;
        d[2] = (uint8_t)lt.tm_min;
        d[3] = (uint8_t)lt.tm_hour;
        d[4] = (uint8_t)lt.tm_mday;
        d[5] = (uint8_t)(lt.tm_mon + 1);
        const uint16_t y = (uint16_t)(lt.tm_year + 1900);
        std::memcpy(d + 6, &y, 2);
    }

    static std::time_t mtimeOf(const fs::path& p) {
        std::error_code ec;
        const auto ft = fs::last_write_time(p, ec);
        if (ec) return std::time(nullptr);
        const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now() +
                                                                                          std::chrono::system_clock::now());
        return std::chrono::system_clock::to_time_t(sys);
    }

    int getdir(const std::vector<uint8_t>& in) {
        const int port = (int)rd32(in, 0) & 1;
        const int flags = (int)rd32(in, 8), maxent = (int)rd32(in, 12);
        const uint32_t table = rd32(in, 16);
        const std::string name = rdStr(in, 20, 1024);
        if (flags == 0) {
            // New listing: split "dir/pattern".
            const size_t slash = name.find_last_of('/');
            const std::string dir = slash == std::string::npos ? "" : name.substr(0, slash == 0 ? 1 : slash);
            const std::string pattern = slash == std::string::npos ? name : name.substr(slash + 1);
            fs::path p;
            resolve(port, dir.empty() ? "." : dir, p);
            listing_[port].clear();
            listPos_[port] = 0;
            std::error_code ec;
            if (!fs::is_directory(p, ec)) return kResNoEntry;
            auto add = [&](const std::string& n, const fs::path& path, bool isDir) {
                if (!wildMatch(pattern.c_str(), n.c_str())) return;
                listing_[port].push_back({n, path, isDir});
            };
            add(".", p, true);
            add("..", p.parent_path(), true);
            std::vector<fs::directory_entry> ents;
            for (auto& e : fs::directory_iterator(p, ec)) ents.push_back(e);
            std::sort(ents.begin(), ents.end(), [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); });
            for (auto& e : ents) add(u8name(e.path()), e.path(), e.is_directory(ec));
        }
        int n = 0;
        auto& L = listing_[port];
        while (n < maxent && listPos_[port] < L.size()) {
            const auto& e = L[listPos_[port]++];
            uint8_t rec[64] = {};
            const std::time_t t = mtimeOf(e.path);
            putDate(rec + 0, t);
            putDate(rec + 8, t);
            std::error_code ec;
            uint32_t size = 0;
            if (e.isDir) {
                if (e.name != "." && e.name != "..") {
                    for (auto& c : fs::directory_iterator(e.path, ec)) (void)c, ++size;
                    size += 2;
                }
            } else {
                size = (uint32_t)fs::file_size(e.path, ec);
            }
            std::memcpy(rec + 16, &size, 4);
            const uint16_t attr = e.isDir ? kAttrDir : kAttrFile;
            std::memcpy(rec + 20, &attr, 2);
            for (size_t i = 0; i < 31 && i < e.name.size(); ++i) rec[32 + i] = (uint8_t)e.name[i];
            if (table)
                for (int k = 0; k < 64; ++k) m_.write8(table + (uint32_t)(n * 64 + k), rec[k]);
            ++n;
        }
        return n;
    }

    int setinfo(const std::vector<uint8_t>& in) {
        const int port = (int)rd32(in, 0) & 1;
        const int flags = (int)rd32(in, 8);
        const uint32_t tbl = rd32(in, 16);
        const std::string name = rdStr(in, 20, 1024);
        fs::path p;
        resolve(port, name, p);
        std::error_code ec;
        if (!fs::exists(p, ec)) return kResNoEntry;
        if ((flags & 0x10) && tbl) { // rename
            std::string nn;
            for (uint32_t i = 0; i < 32; ++i) {
                const char c = (char)m_.read8(tbl + 32 + i);
                if (!c) break;
                nn.push_back(c);
            }
            if (!nn.empty()) {
                fs::rename(p, p.parent_path() / U8(nn), ec);
                if (ec) return kResDeniedPermit;
            }
        }
        return kResSucceed;
    }

    int remove(const std::vector<uint8_t>& in) {
        const int port = (int)rd32(in, 0) & 1;
        const std::string name = rdStr(in, 20, 1024);
        fs::path p;
        resolve(port, name, p);
        std::error_code ec;
        if (!fs::exists(p, ec)) return kResNoEntry;
        if (fs::is_directory(p, ec) && !fs::is_empty(p, ec)) return kResNotEmpty;
        fs::remove(p, ec);
        return ec ? kResDeniedPermit : kResSucceed;
    }

    int format(const std::vector<uint8_t>& in, bool fmt) {
        const int port = (int)rd32(in, 4) & 1;
        logInfo("mc%d: %s requested by the game", port, fmt ? "format" : "unformat");
        std::error_code ec;
        for (auto& e : fs::directory_iterator(cardRoot(port), ec)) fs::remove_all(e.path(), ec);
        return kResSucceed;
    }

    struct Entry {
        std::string name;
        fs::path path;
        bool isDir;
    };
    Machine& m_;
    std::map<int, std::unique_ptr<McFile>> files_;
    int nextFd_ = 1;
    std::string curDir_[2];
    bool seen_[2] = {false, false};
    std::vector<Entry> listing_[2];
    size_t listPos_[2] = {0, 0};
};

} // namespace

void registerMcServices(Machine& m) {
    auto svc = std::make_shared<McService>(m);
    m.sif->registerServer(0x80000400, svc);
}

} // namespace ps2
