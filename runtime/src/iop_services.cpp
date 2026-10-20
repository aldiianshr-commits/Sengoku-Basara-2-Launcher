// LOADFILE, IOP heap and CDVD (cdvdfsv) RPC services.
#include "iop_services.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

#include "ps2/cdvd.hpp"
#include "ps2/iop.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"
#include "ps2/sif.hpp"
#include "sb2/disc.hpp"
#include "sb2/elf_loader.hpp"

namespace ps2 {

using namespace hle;

namespace {

constexpr int64_t kNoReply = INT64_MAX;

std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// "cdrom0:\MODULES\PADMAN.IRX;1" -> "PADMAN"
std::string moduleBaseName(const std::string& path) {
    std::string s = path;
    const size_t colon = s.find(':');
    if (colon != std::string::npos) s = s.substr(colon + 1);
    const size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const size_t semi = s.find(';');
    if (semi != std::string::npos) s = s.substr(0, semi);
    const size_t dot = s.rfind('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    return upper(s);
}

// Reads the module name and version from an IRX's .iopmod section.
bool irxModuleInfo(const std::vector<uint8_t>& f, std::string& name, uint16_t& version) {
    if (f.size() < 52 || f[0] != 0x7F || f[1] != 'E' || f[2] != 'L' || f[3] != 'F') return false;
    auto r32 = [&](size_t o) -> uint32_t {
        uint32_t v = 0;
        if (o + 4 <= f.size()) std::memcpy(&v, f.data() + o, 4);
        return v;
    };
    auto r16 = [&](size_t o) -> uint16_t {
        uint16_t v = 0;
        if (o + 2 <= f.size()) std::memcpy(&v, f.data() + o, 2);
        return v;
    };
    const uint32_t shoff = r32(32);
    const uint16_t shentsize = r16(46), shnum = r16(48);
    for (uint16_t i = 0; i < shnum; ++i) {
        const size_t sh = shoff + (size_t)i * shentsize;
        if (r32(sh + 4) != 0x70000080u) continue; // SHT_IOPMOD
        const uint32_t off = r32(sh + 16), size = r32(sh + 20);
        if (off + 26 > f.size() || size < 26) return false;
        version = r16(off + 24);
        name.clear();
        for (size_t k = off + 26; k < f.size() && k < off + size && f[k]; ++k) name.push_back((char)f[k]);
        return true;
    }
    return false;
}

// Modules whose functionality is provided natively.
bool isNativeModule(const std::string& base, const std::string& irxName) {
    static const char* const kNative[] = {
        "SIO2MAN", "XSIO2MAN", "PADMAN", "XPADMAN", "MTAPMAN", "XMTAPMAN", "MCMAN", "MCSERV", "XMCMAN", "XMCSERV",
        "CDVDMAN", "CDVDFSV", "CDVDSTM", "LIBSD", "SDRDRV", "FILEIO", "IOMAN", "IOMANX", "SECRMAN", "SECRSIF",
        "DMACMAN", "SYSMEM", "LOADCORE", "INTRMAN", "THBASE", "THSEMAP", "THEVENT", "THMSGBX", "THFPOOL", "THVPOOL",
        "MODLOAD", "SIFCMD", "SIFMAN", "EESYNC", "CLEARSPU", "TIMRMAN", "HEAPLIB", "STDIO", "SYSCLIB", "VBLANK",
        "SIO2D", "DBCMAN", "PADMAN2", "FREESD", "FREEPAD", "FREEMTAP", "FREESIO2", "FILEXIO", "IOPRP"};
    const std::string n = upper(irxName);
    for (const char* k : kNative) {
        if (base == k) return true;
        if (!n.empty() && n.find(k) != std::string::npos) return true;
    }
    // IRX names of Sony modules that differ from their file names.
    static const char* const kNativeIrx[] = {"CDVD_EE_DRIVER", "CDVD_DRIVER", "FILEIO_SERVICE", "IO/FILE_MANAGER", "SIO2_MANAGER",
                                             "MC_SERVER", "MCMAN_CEX", "MCSERV_CEX", "SOUND_DEVICE_LIBRARY", "SDRDRV"};
    for (const char* k : kNativeIrx)
        if (n.find(k) != std::string::npos) return true;
    return false;
}

// ------------------------------------------------------------------ LOADFILE

class LoadfileServer final : public RpcServer {
public:
    explicit LoadfileServer(Machine& m) : m_(m) {}
    const char* name() const override { return "LOADFILE"; }
    int64_t latency(uint32_t) const override { return 30000; }

    std::vector<uint8_t> call(uint32_t fno, const std::vector<uint8_t>& in, uint32_t rsize) override {
        std::vector<uint8_t> out(std::max<uint32_t>(rsize, 8), 0);
        switch (fno) {
        case 0:   // LF_F_MOD_LOAD
        case 4:   // LF_F_MG_MOD_LOAD (signed)
        {
            const int argLen = (int)rd32(in, 0);
            const std::string path = rdStr(in, 8, 252);
            std::string args;
            if (argLen > 0 && in.size() >= 260) args.assign((const char*)in.data() + 260, std::min<size_t>((size_t)argLen, 252));
            const int id = m_.iop->loadModule(path, args);
            wr32(out, 0, (uint32_t)id);
            wr32(out, 4, id > 0 ? 2u : 0u); // module result: MODULE_RESIDENT_END
            return out;
        }
        case 6: { // LF_F_MOD_BUF_LOAD (module image already in IOP memory)
            const uint32_t iopAddr = rd32(in, 0);
            std::vector<uint8_t> img(std::min<uint32_t>(0x80000, IOP_RAM_SIZE - (iopAddr & (IOP_RAM_SIZE - 1))));
            std::memcpy(img.data(), m_.iopRam + (iopAddr & (IOP_RAM_SIZE - 1)), img.size());
            std::string nm;
            uint16_t ver = 0;
            irxModuleInfo(img, nm, ver);
            const int id = m_.iop->loadModule("iopmem:" + (nm.empty() ? std::string("buffer") : nm), "");
            wr32(out, 0, (uint32_t)id);
            wr32(out, 4, 2);
            return out;
        }
        case 1:   // LF_F_ELF_LOAD
        case 5: { // LF_F_MG_ELF_LOAD
            const std::string path = rdStr(in, 8, 252);
            const std::string sec = rdStr(in, 260, 252);
            uint32_t entry = 0, gp = 0;
            if (loadEeElf(path, sec, entry, gp)) {
                wr32(out, 0, entry);
                wr32(out, 4, gp);
            }
            return out;
        }
        case 2: { // LF_F_SET_ADDR: write IOP memory
            const uint32_t a = rd32(in, 0) & (IOP_RAM_SIZE - 1);
            const int type = (int)rd32(in, 4);
            const uint32_t v = rd32(in, 8);
            if (type == 0) m_.iopRam[a] = (uint8_t)v;
            else if (type == 1) std::memcpy(m_.iopRam + (a & ~1u), &v, 2);
            else std::memcpy(m_.iopRam + (a & ~3u), &v, 4);
            wr32(out, 0, 0);
            return out;
        }
        case 3: { // LF_F_GET_ADDR: read IOP memory
            const uint32_t a = rd32(in, 0) & (IOP_RAM_SIZE - 1);
            const int type = (int)rd32(in, 4);
            uint32_t v = 0;
            if (type == 0) v = m_.iopRam[a];
            else if (type == 1) std::memcpy(&v, m_.iopRam + (a & ~1u), 2);
            else std::memcpy(&v, m_.iopRam + (a & ~3u), 4);
            wr32(out, 0, v);
            return out;
        }
        case 7:   // LF_F_MOD_STOP
            wr32(out, 0, rd32(in, 0));
            wr32(out, 4, 1); // MODULE_NO_RESIDENT_END
            return out;
        case 8:   // LF_F_MOD_UNLOAD
            wr32(out, 0, rd32(in, 0));
            return out;
        case 9: { // LF_F_SEARCH_MOD_BY_NAME
            const std::string nm = upper(rdStr(in, 8, 252));
            int id = -202; // KE_UNKNOWN_MODULE
            for (const auto& mod : m_.iop->modules())
                if (upper(mod.name) == nm || moduleBaseName(mod.path) == nm) id = mod.id;
            wr32(out, 0, (uint32_t)id);
            return out;
        }
        case 10: // LF_F_SEARCH_MOD_BY_ADDRESS
            wr32(out, 0, (uint32_t)-202);
            return out;
        case 0xFF: // LF_F_GET_VERSION
            wr32(out, 0, 0x0101);
            return out;
        default:
            logWarn("loadfile: unsupported function %u", fno);
            wr32(out, 0, (uint32_t)-1);
            return out;
        }
    }

private:
    bool loadEeElf(const std::string& path, const std::string& section, uint32_t& entry, uint32_t& gp) {
        Vfs& vfs = m_.iop->vfs();
        const int fd = vfs.open(path, 0);
        if (fd < 0) {
            logWarn("loadfile: cannot open EE executable %s", path.c_str());
            return false;
        }
        std::vector<uint8_t> data;
        uint8_t buf[65536];
        int n;
        while ((n = vfs.read(fd, buf, sizeof buf)) > 0) data.insert(data.end(), buf, buf + n);
        vfs.close(fd);
        sb2::LoadedElf elf;
        std::string err;
        if (!sb2::parseElfSegments(data, elf, err)) {
            logWarn("loadfile: %s: %s", path.c_str(), err.c_str());
            return false;
        }
        for (auto& s : elf.segments) {
            for (size_t i = 0; i < s.data.size(); ++i) m_.write8(s.vaddr + (uint32_t)i, s.data[i]);
            for (uint32_t i = (uint32_t)s.data.size(); i < s.memsz; ++i) m_.write8(s.vaddr + i, 0);
        }
        entry = elf.entry;
        gp = 0;
        logInfo("loadfile: loaded EE executable %s (section '%s', entry %08x) - code outside the boot ELF must be "
                "recompiled as an overlay to run",
                path.c_str(), section.c_str(), entry);
        return true;
    }
    Machine& m_;
};

// ------------------------------------------------------------------ IOP heap

class IopHeapServer final : public RpcServer {
public:
    explicit IopHeapServer(Machine& m) : m_(m) {}
    const char* name() const override { return "IOPHEAP"; }
    std::vector<uint8_t> call(uint32_t fno, const std::vector<uint8_t>& in, uint32_t) override {
        switch (fno) {
        case 1: return reply32((int32_t)m_.sif->iopAlloc(rd32(in, 0)));
        case 2: return reply32((int32_t)m_.sif->iopFree(rd32(in, 0)));
        case 3: { // load a file into IOP memory: {addr, path[]}
            const uint32_t addr = rd32(in, 0) & (IOP_RAM_SIZE - 1);
            const std::string path = rdStr(in, 4, 252);
            Vfs& vfs = m_.iop->vfs();
            const int fd = vfs.open(path, 0);
            if (fd < 0) return reply32(-1);
            uint32_t pos = addr;
            uint8_t buf[16384];
            int n;
            while ((n = vfs.read(fd, buf, sizeof buf)) > 0 && pos < IOP_RAM_SIZE) {
                const uint32_t k = std::min<uint32_t>((uint32_t)n, IOP_RAM_SIZE - pos);
                std::memcpy(m_.iopRam + pos, buf, k);
                pos += k;
            }
            vfs.close(fd);
            return reply32(0);
        }
        case 4: return reply32((int32_t)m_.sif->iopHeapFree()); // QueryTotalFreeMemSize (unofficial)
        default: return reply32(0);
        }
    }

private:
    Machine& m_;
};

// ------------------------------------------------------------------ CDVD

int discType(Machine& m) {
    if (!m.cdvd->present()) return 0; // SCECdNODISC
    return m.cdvd->sectorCount() > 360000 ? 0x14 /* PS2 DVD */ : 0x12 /* PS2 CD */;
}

uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

class CdvdInitServer final : public RpcServer {
public:
    explicit CdvdInitServer(Machine& m) : m_(m) {}
    const char* name() const override { return "CDVDFSV init"; }
    std::vector<uint8_t> call(uint32_t, const std::vector<uint8_t>& in, uint32_t) override {
        PS2_DEBUG(Cdvd, "sceCdInit(%d)", (int)rd32(in, 0));
        std::vector<uint8_t> out(16, 0);
        wr32(out, 0, 1);
        wr32(out, 4, 0x0226); // cdvdfsv 2.38
        wr32(out, 8, 0x0226); // cdvdman
        wr32(out, 12, 0);
        return out;
    }

private:
    Machine& m_;
};

class CdvdDiskReadyServer final : public RpcServer {
public:
    explicit CdvdDiskReadyServer(Machine& m) : m_(m) {}
    const char* name() const override { return "CDVDFSV diskready"; }
    std::vector<uint8_t> call(uint32_t, const std::vector<uint8_t>&, uint32_t) override {
        return reply32(m_.cdvd->present() ? 2 /* SCECdComplete */ : 6 /* SCECdNotReady */);
    }

private:
    Machine& m_;
};

class CdvdPoffServer final : public RpcServer {
public:
    const char* name() const override { return "CDVDFSV poweroff"; }
    std::vector<uint8_t> call(uint32_t, const std::vector<uint8_t>&, uint32_t) override {
        nextLatency = kNoReply; // completes only when the console is switched off
        return {};
    }
};

class CdvdSearchFileServer final : public RpcServer {
public:
    explicit CdvdSearchFileServer(Machine& m) : m_(m) {}
    const char* name() const override { return "CDVDFSV searchfile"; }
    int64_t latency(uint32_t) const override { return 20000; }
    std::vector<uint8_t> call(uint32_t, const std::vector<uint8_t>& in, uint32_t) override {
        // Packet: sceCdlFILE fp (32) [+ int attr] + char path[256] + ee dest [+ layer]
        // Sizes: 0x124 (old), 0x128 (2.x), 0x12C (dual layer).
        const size_t len = in.size();
        size_t pathOff = 32, destOff = 0x120;
        size_t outSize = 32;
        if (len >= 0x12C) {
            pathOff = 36;
            destOff = 0x124;
            outSize = 36;
        } else if (len >= 0x128) {
            pathOff = 36;
            destOff = 0x124;
            outSize = 36;
        }
        std::string path = rdStr(in, pathOff, 256);
        const uint32_t dest = rd32(in, destOff);
        std::vector<uint8_t> fp(outSize, 0);
        int found = 0;
        if (m_.cdvd->fs()) {
            if (auto e = m_.cdvd->fs()->find(path)) {
                if (!e->isDir) {
                    found = 1;
                    wr32(fp, 0, e->lba);
                    wr32(fp, 4, e->size);
                    std::string nm = e->name;
                    for (size_t i = 0; i < 16 && i < nm.size(); ++i) fp[8 + i] = (uint8_t)nm[i];
                    // date: [0]=flags, [1]=sec, [2]=min, [3]=hour, [4]=day, [5]=mon, [6,7]=year
                    fp[24] = 0;
                    fp[25] = 0;
                    fp[26] = 0;
                    fp[27] = 12;
                    fp[28] = 1;
                    fp[29] = 1;
                    fp[30] = (uint8_t)(2007 & 0xFF);
                    fp[31] = (uint8_t)(2007 >> 8);
                }
            }
        }
        PS2_DEBUG(Cdvd, "sceCdSearchFile('%s') -> %d", path.c_str(), found);
        if (!found) logWarn("cdvd: sceCdSearchFile: '%s' not found on the disc", path.c_str());
        if (dest) {
            Machine* mm = &m_;
            onComplete = [mm, dest, fp]() {
                for (size_t i = 0; i < fp.size(); ++i) mm->write8(dest + (uint32_t)i, fp[i]);
            };
        }
        return reply32(found);
    }

private:
    Machine& m_;
};

class CdvdSCmdServer final : public RpcServer {
public:
    explicit CdvdSCmdServer(Machine& m) : m_(m) {}
    const char* name() const override { return "CDVDFSV S-cmd"; }
    std::vector<uint8_t> call(uint32_t fno, const std::vector<uint8_t>& in, uint32_t rsize) override {
        std::vector<uint8_t> out(std::max<uint32_t>(rsize, 16), 0);
        wr32(out, 0, 1); // default: success
        switch (fno) {
        case 0x01: { // READCLOCK: {result, sceCdCLOCK}
            const std::time_t t = std::time(nullptr);
            std::tm lt{};
#if defined(_WIN32)
            localtime_s(&lt, &t);
#else
            localtime_r(&t, &lt);
#endif
            // The PS2 RTC runs in JST (UTC+9) for Japanese titles; games
            // convert using the OSD time zone. Report local time.
            out[4] = 0; // stat
            out[5] = bcd(lt.tm_sec);
            out[6] = bcd(lt.tm_min);
            out[7] = bcd(lt.tm_hour);
            out[8] = 0;
            out[9] = bcd(lt.tm_mday);
            out[10] = bcd(lt.tm_mon + 1);
            out[11] = bcd(lt.tm_year % 100);
            break;
        }
        case 0x03: wr32(out, 0, (uint32_t)discType(m_)); break; // GETDISKTYPE
        case 0x04: wr32(out, 0, 0); break;                      // GETERROR: SCECdErNO
        case 0x05: wr32(out, 0, 1); wr32(out, 4, 0); break;     // TRAYREQ: result, traycnt
        case 0x0C: wr32(out, 0, m_.cdvd->present() ? 0x0A : 0x01); break; // STATUS: pause / open
        case 0x16: wr32(out, 0, 1); break;                      // BREAK
        case 0x22: wr32(out, 0, 1); break;                      // MMODE
        case 0x23: wr32(out, 0, 0); break;                      // SETTHREADPRI (0 = ok)
        case 0x14: { // READ_MECHACON_VERSION
            wr32(out, 0, 1);
            wr32(out, 4, 0x00020300);
            break;
        }
        default:
            PS2_DEBUG(Cdvd, "scmd %02x (default success)", fno);
            break;
        }
        (void)in;
        return out;
    }

private:
    Machine& m_;
};

class CdvdNCmdServer final : public RpcServer {
public:
    explicit CdvdNCmdServer(Machine& m) : m_(m) {}
    const char* name() const override { return "CDVDFSV N-cmd"; }
    int64_t latency(uint32_t) const override { return 6000; }

    std::vector<uint8_t> call(uint32_t fno, const std::vector<uint8_t>& in, uint32_t rsize) override {
        switch (fno) {
        case 1:   // READ {lbn, sectors, buf, mode, eereadx, readpos}
        case 3: { // DVDREAD (sceCdReadDVDV) - same layout, DVD-Video sectors
            const uint32_t lbn = rd32(in, 0), count = rd32(in, 4), buf = rd32(in, 8), mode = rd32(in, 12);
            const uint32_t readx = rd32(in, 16), readpos = in.size() >= 24 ? rd32(in, 20) : 0;
            const uint32_t pattern = (mode >> 16) & 0xFF;
            const uint32_t secSize = fno == 3 ? 2064 : (pattern == 1 ? 2328 : (pattern == 2 ? 2340 : 2048));
            std::vector<uint8_t> data((size_t)count * secSize, 0);
            bool ok = true;
            if (secSize == 2048) {
                ok = m_.cdvd->readSectors(lbn, count, data.data());
            } else {
                std::vector<uint8_t> sec(2048);
                for (uint32_t i = 0; i < count && ok; ++i) {
                    ok = m_.cdvd->readSectors(lbn + i, 1, sec.data());
                    const size_t hdr = secSize == 2064 ? 12 : (secSize == 2340 ? 12 : 8);
                    std::memcpy(&data[(size_t)i * secSize + hdr], sec.data(), 2048);
                }
            }
            if (!ok) {
                logWarn("cdvd: read of %u sectors at lbn %u failed (beyond end of image?)", count, lbn);
                lastError_ = 0x32; // SCECdErREAD
            } else {
                lastError_ = 0;
            }
            PS2_DEBUG(Cdvd, "sceCdRead(lbn=%u, n=%u, buf=%08x, size=%u)", lbn, count, buf, secSize);
            Machine* mm = &m_;
            const uint32_t total = (uint32_t)data.size();
            onComplete = [mm, buf, readx, readpos, total, data = std::move(data)]() {
                if (uint8_t* p = mm->hostPtr(buf, total)) std::memcpy(p, data.data(), data.size());
                else
                    for (uint32_t i = 0; i < total; ++i) mm->write8(buf + i, data[i]);
                if (readx) {
                    // Unaligned head/tail descriptor: nothing left to copy.
                    for (uint32_t i = 0; i < 16; i += 4) mm->write32(readx + i, 0);
                }
                if (readpos) mm->write32(readpos, total);
            };
            // ~24x DVD (33 MB/s): fast but keeps the asynchronous behaviour.
            nextLatency = 8000 + (int64_t)count * 18000;
            return {};
        }
        case 13: { // READIOPMEM {lbn, sectors, iopbuf, mode}
            const uint32_t lbn = rd32(in, 0), count = rd32(in, 4), buf = rd32(in, 8) & (IOP_RAM_SIZE - 1);
            std::vector<uint8_t> data((size_t)count * 2048, 0);
            m_.cdvd->readSectors(lbn, count, data.data());
            const size_t n = std::min<size_t>(data.size(), IOP_RAM_SIZE - buf);
            std::memcpy(m_.iopRam + buf, data.data(), n);
            nextLatency = 8000 + (int64_t)count * 18000;
            return {};
        }
        case 4: { // GETTOC {tocbuf(ee), ?, ?} -> {result, disctype}
            const uint32_t toc = rd32(in, 0);
            Machine* mm = &m_;
            onComplete = [mm, toc]() {
                if (!toc) return;
                for (uint32_t i = 0; i < 2064; i += 4) mm->write32(toc + i, 0);
                mm->write8(toc + 0, 0x04);  // DVD, single layer
                mm->write8(toc + 1, 0x02);
                mm->write8(toc + 2, 0xF2);
            };
            std::vector<uint8_t> out(std::max<uint32_t>(rsize, 8), 0);
            wr32(out, 0, 1);
            wr32(out, 4, (uint32_t)discType(m_));
            return out;
        }
        case 5:  // SEEK
        case 6:  // STANDBY
        case 7:  // STOP
        case 8:  // PAUSE
            nextLatency = 20000;
            return {};
        case 9: return reply32(stream(in)); // STREAM
        case 10: return reply32(0);         // CDDASTREAM: no CD-DA on PS2 DVDs
        case 11: {                          // READ_KEY
            std::vector<uint8_t> out(std::max<uint32_t>(rsize, 0x18), 0);
            wr32(out, 0, 1);
            return out;
        }
        case 12: { // generic N-command
            std::vector<uint8_t> out(std::max<uint32_t>(rsize, 16), 0);
            wr32(out, 0, 1);
            return out;
        }
        case 14: return reply32(m_.cdvd->present() ? 2 : 6); // DISKREADY
        case 15: { // READCHAIN: entries {lbn, sectors, buffer, bufferType} until lbn == -1
            Machine* mm = &m_;
            std::vector<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> chain;
            for (size_t off = 0; off + 16 <= in.size() && chain.size() < 64; off += 16) {
                const uint32_t lbn = rd32(in, off), n = rd32(in, off + 4), b = rd32(in, off + 8), type = rd32(in, off + 12);
                if (lbn == 0xFFFFFFFFu || n == 0xFFFFFFFFu || b == 0xFFFFFFFFu) break;
                chain.emplace_back(lbn, n, b, type);
            }
            int64_t total = 0;
            for (auto& [lbn, n, b, type] : chain) {
                std::vector<uint8_t> data((size_t)n * 2048);
                m_.cdvd->readSectors(lbn, n, data.data());
                if (type == 0) {
                    for (size_t i = 0; i < data.size(); ++i) mm->write8(b + (uint32_t)i, data[i]);
                } else {
                    const uint32_t ib = b & (IOP_RAM_SIZE - 1);
                    std::memcpy(m_.iopRam + ib, data.data(), std::min<size_t>(data.size(), IOP_RAM_SIZE - ib));
                }
                total += n;
            }
            nextLatency = 8000 + total * 18000;
            return {};
        }
        default:
            logWarn("cdvd: unsupported N-command %u", fno);
            return reply32(0);
        }
    }

private:
    // sceCdSt* streaming: data is always available immediately.
    int32_t stream(const std::vector<uint8_t>& in) {
        const uint32_t lbn = rd32(in, 0), n = rd32(in, 4), buf = rd32(in, 8), cmd = rd32(in, 12);
        switch (cmd) {
        case 1: // START
            streamPos_ = lbn;
            streaming_ = true;
            return 1;
        case 2: { // READ: returns sectors read | error << 16
            if (!streaming_) return 0;
            std::vector<uint8_t> data((size_t)n * 2048);
            const bool ok = m_.cdvd->readSectors(streamPos_, n, data.data());
            if (uint8_t* p = m_.hostPtr(buf, (uint32_t)data.size())) std::memcpy(p, data.data(), data.size());
            else
                for (size_t i = 0; i < data.size(); ++i) m_.write8(buf + (uint32_t)i, data[i]);
            streamPos_ += n;
            return ok ? (int32_t)n : (int32_t)(n | (0x32u << 16));
        }
        case 3: streaming_ = false; return 1;    // STOP
        case 4: streamPos_ = lbn; return 1;      // SEEK
        case 5: streamBufSectors_ = lbn * n; return 1; // INIT (bufsize, numbufs, buf)
        case 6: return streaming_ ? (int32_t)std::max<uint32_t>(streamBufSectors_, 16) : 0; // STAT
        case 7: return 1;                        // PAUSE
        case 8: return 1;                        // RESUME
        case 9: streamPos_ = lbn; return 1;      // SEEKF
        default: return 0;
        }
    }
    Machine& m_;
    uint32_t streamPos_ = 0;
    uint32_t streamBufSectors_ = 0;
    bool streaming_ = false;
    int lastError_ = 0;
};

} // namespace

// ------------------------------------------------------------------ Iop module bookkeeping

int Iop::loadModule(const std::string& path, const std::string& args) {
    IopModule mod;
    mod.id = nextModuleId_++;
    mod.path = path;
    const std::string base = moduleBaseName(path);
    std::string irxName;
    uint16_t ver = 0;
    const std::string lower = [&] {
        std::string s = path;
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }();
    if (lower.rfind("rom0:", 0) != 0 && lower.rfind("rom1:", 0) != 0 && lower.rfind("iopmem:", 0) != 0) {
        const int fd = vfs_->open(path, 0);
        if (fd < 0) {
            logWarn("iop: module %s not found", path.c_str());
            return -203; // KE_NOFILE
        }
        std::vector<uint8_t> data;
        uint8_t buf[65536];
        int n;
        while ((n = vfs_->read(fd, buf, sizeof buf)) > 0) data.insert(data.end(), buf, buf + n);
        vfs_->close(fd);
        irxModuleInfo(data, irxName, ver);
    }
    mod.name = irxName.empty() ? base : irxName;
    mod.version = ver;
    mod.native = isNativeModule(base, irxName);
    modules_.push_back(mod);
    loadedModules.push_back(mod.name);
    if (mod.native) {
        logInfo("iop: loaded %s (%s v%x.%02x) -> native implementation", path.c_str(), mod.name.c_str(), ver >> 8, ver & 0xFF);
    } else {
        logWarn("iop: module %s (%s v%x.%02x%s%s) has no native implementation; RPC services it would provide are unavailable",
                path.c_str(), mod.name.c_str(), ver >> 8, ver & 0xFF, args.empty() ? "" : ", args ", args.c_str());
    }
    return mod.id;
}

void Iop::onReboot(const std::string& arg) {
    (void)arg;
    modules_.clear();
    loadedModules.clear();
}

void registerStandardServices(Machine& m, Iop&) {
    m.sif->registerServer(0x80000003, std::make_shared<IopHeapServer>(m));
    m.sif->registerServer(0x80000006, std::make_shared<LoadfileServer>(m));
    m.sif->registerServer(0x80000592, std::make_shared<CdvdInitServer>(m));
    m.sif->registerServer(0x80000593, std::make_shared<CdvdSCmdServer>(m));
    m.sif->registerServer(0x80000595, std::make_shared<CdvdNCmdServer>(m));
    m.sif->registerServer(0x80000596, std::make_shared<CdvdPoffServer>());
    m.sif->registerServer(0x80000597, std::make_shared<CdvdSearchFileServer>(m));
    m.sif->registerServer(0x8000059A, std::make_shared<CdvdDiskReadyServer>(m));
    registerPadServices(m);
    registerMcServices(m);
    registerSdrServices(m);
}

} // namespace ps2
