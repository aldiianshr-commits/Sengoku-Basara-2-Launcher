#include "ps2/iop.hpp"

#include <cstring>

#include "iop_services.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"
#include "ps2/sif.hpp"

namespace ps2 {

namespace {

uint32_t rd32(const std::vector<uint8_t>& v, size_t off) {
    uint32_t x = 0;
    if (off + 4 <= v.size()) std::memcpy(&x, v.data() + off, 4);
    return x;
}
std::string rdStr(const std::vector<uint8_t>& v, size_t off, size_t max = 256) {
    std::string s;
    for (size_t i = off; i < v.size() && i < off + max && v[i]; ++i) s.push_back((char)v[i]);
    return s;
}
std::vector<uint8_t> reply32(int32_t r) {
    std::vector<uint8_t> o(4);
    std::memcpy(o.data(), &r, 4);
    return o;
}

void writeStat(Machine& m, uint32_t addr, const VfsStat& st) {
    if (!addr) return;
    for (uint32_t i = 0; i < 64; i += 4) m.write32(addr + i, 0);
    m.write32(addr + 0, st.mode);
    m.write32(addr + 4, st.attr);
    m.write32(addr + 8, (uint32_t)st.size);
    m.write32(addr + 36, (uint32_t)(st.size >> 32)); // hisize
}

class FileioServer : public RpcServer {
public:
    explicit FileioServer(Machine& m) : m_(m) {}
    const char* name() const override { return "FILEIO"; }
    int64_t latency(uint32_t) const override { return 3000; }
    std::vector<uint8_t> call(uint32_t fno, const std::vector<uint8_t>& in, uint32_t) override {
        Vfs& vfs = m_.iop->vfs();
        switch (fno) {
        case 0: { // open
            const int mode = (int)rd32(in, 0);
            const std::string name = rdStr(in, 4);
            const int fd = vfs.open(name, mode);
            PS2_DEBUG(Iop, "fio open('%s', %x) = %d", name.c_str(), mode, fd);
            return reply32(fd);
        }
        case 1: return reply32(vfs.close((int)rd32(in, 0)));
        case 2: { // read
            const int fd = (int)rd32(in, 0);
            const uint32_t ptr = rd32(in, 4), size = rd32(in, 8), rdata = rd32(in, 12);
            std::vector<uint8_t> buf(size);
            const int n = vfs.read(fd, buf.data(), (int)size);
            for (int i = 0; i < n; ++i) m_.write8(ptr + (uint32_t)i, buf[(size_t)i]);
            if (rdata) {
                for (uint32_t i = 0; i < 48; i += 4) m_.write32(rdata + i, 0);
            }
            return reply32(n);
        }
        case 3: { // write
            const int fd = (int)rd32(in, 0);
            const uint32_t ptr = rd32(in, 4), size = rd32(in, 8), mis = rd32(in, 12);
            std::vector<uint8_t> data(size);
            for (uint32_t i = 0; i < size; ++i)
                data[i] = i < mis ? (16 + i < in.size() ? in[16 + i] : 0) : m_.read8(ptr + i);
            return reply32(vfs.write(fd, data.data(), (int)size));
        }
        case 4: { // lseek
            const int fd = (int)rd32(in, 0);
            const int32_t off = (int32_t)rd32(in, 4);
            const int whence = (int)rd32(in, 8);
            return reply32((int32_t)vfs.lseek(fd, off, whence));
        }
        case 5: return reply32(0); // ioctl
        case 6: return reply32(vfs.remove(rdStr(in, 0)));
        case 7: return reply32(vfs.mkdir(rdStr(in, 0)));
        case 8: return reply32(vfs.rmdir(rdStr(in, 0)));
        case 9: return reply32(vfs.dopen(rdStr(in, 0)));
        case 10: return reply32(vfs.dclose((int)rd32(in, 0)));
        case 11: { // dread
            const int fd = (int)rd32(in, 0);
            const uint32_t buf = rd32(in, 4);
            std::string nm;
            VfsStat st;
            const int r = vfs.dread(fd, nm, st);
            if (r > 0 && buf) {
                writeStat(m_, buf, st);
                for (size_t i = 0; i < 256; ++i) m_.write8(buf + 64 + (uint32_t)i, i < nm.size() ? (uint8_t)nm[i] : 0);
            }
            return reply32(r);
        }
        case 12: { // getstat
            const uint32_t buf = rd32(in, 0);
            const std::string nm = rdStr(in, 4);
            VfsStat st;
            const int r = vfs.getstat(nm, st);
            if (r == 0) writeStat(m_, buf, st);
            return reply32(r);
        }
        default:
            return reply32(0);
        }
    }

private:
    Machine& m_;
};

} // namespace

Iop::Iop(Machine& m) : m_(m), vfs_(std::make_unique<Vfs>(m)) {}
Iop::~Iop() = default;

void Iop::boot() {
    m_.sif->registerServer(0x80000001, std::make_shared<FileioServer>(m_));
    registerStandardServices(m_, *this);
}

} // namespace ps2
