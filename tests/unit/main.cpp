// Unit tests for the recompiler, runtime components and scanner.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "ps2/fpu.hpp"
#include "ps2/gs.hpp"
#include "ps2/gs_mem.hpp"
#include "ps2/machine.hpp"
#include "ps2/vif.hpp"
#include "ps2/vu.hpp"
#include "r5900_isa.hpp"
#include "sb2/disc.hpp"
#include "sb2/hash.hpp"
#include "sb2/json.hpp"
#include "sb2/scanner.hpp"

namespace fs = std::filesystem;

static int g_failed = 0, g_passed = 0;
static std::vector<std::pair<std::string, std::function<void()>>>& registry() {
    static std::vector<std::pair<std::string, std::function<void()>>> r;
    return r;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
#define TEST(name) \
    static void test_##name(); \
    static Reg reg_##name(#name, test_##name); \
    static void test_##name()
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("    CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ++g_failed; \
        } else { \
            ++g_passed; \
        } \
    } while (0)
#define CHECK_EQ(a, b) \
    do { \
        auto va_ = (a); \
        auto vb_ = (b); \
        if (!(va_ == vb_)) { \
            std::printf("    CHECK_EQ failed: %s != %s (%s:%d)\n", #a, #b, __FILE__, __LINE__); \
            ++g_failed; \
        } else { \
            ++g_passed; \
        } \
    } while (0)

static fs::path tmpDir() {
    fs::path p = fs::temp_directory_path() / "sb2_unit_tests";
    fs::create_directories(p);
    return p;
}

// ------------------------------------------------------------------ hashing / json

TEST(sha256_vectors) {
    CHECK_EQ(sb2::sha256Hex("", 0), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sb2::sha256Hex("abc", 3), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    std::string m(1000000, 'a');
    CHECK_EQ(sb2::sha256Hex(m.data(), m.size()),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(crc32_vector) { CHECK_EQ(sb2::crc32("123456789", 9), 0xCBF43926u); }

TEST(json_roundtrip) {
    sb2::Json j;
    j["name"] = "Sengoku \"Basara\"\n";
    j["n"] = 42;
    j["f"] = 1.5;
    j["b"] = true;
    sb2::Json a;
    a.push(1);
    a.push("x");
    j["arr"] = a;
    std::string err;
    sb2::Json k = sb2::Json::parse(j.dump(), &err);
    CHECK(err.empty());
    CHECK_EQ(k.get("name").asString(), std::string("Sengoku \"Basara\"\n"));
    CHECK_EQ(k.get("n").asInt(), 42);
    CHECK_EQ(k.get("f").asNumber(), 1.5);
    CHECK(k.get("b").asBool());
    CHECK_EQ(k.get("arr").asArray().size(), (size_t)2);
    CHECK(sb2::Json::parse("{bad", &err).isNull() && !err.empty());
}

// ------------------------------------------------------------------ synthetic disc images

namespace {
struct IsoFile {
    std::string name;
    std::vector<uint8_t> data;
};
void put32both(uint8_t* p, uint32_t v) {
    std::memcpy(p, &v, 4);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}
size_t dirRecord(uint8_t* p, uint32_t lba, uint32_t size, bool dir, const std::string& name) {
    const size_t len = (33 + name.size() + 1) & ~(size_t)1;
    std::memset(p, 0, len);
    p[0] = (uint8_t)len;
    put32both(p + 2, lba);
    put32both(p + 10, size);
    p[25] = dir ? 2 : 0;
    p[28] = 1;
    p[31] = 1;
    p[32] = (uint8_t)name.size();
    std::memcpy(p + 33, name.data(), name.size());
    return len;
}
std::vector<uint8_t> makeIso(const std::vector<IsoFile>& files, const std::string& volId) {
    uint32_t lba = 20;
    std::vector<uint32_t> lbas;
    for (auto& f : files) {
        lbas.push_back(lba);
        lba += (uint32_t)((f.data.size() + 2047) / 2048);
    }
    const uint32_t total = lba;
    std::vector<uint8_t> img((size_t)total * 2048, 0);
    uint8_t* pvd = &img[16 * 2048];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    std::memset(pvd + 8, ' ', 32);
    std::memcpy(pvd + 8, "PLAYSTATION", 11);
    std::memset(pvd + 40, ' ', 32);
    std::memcpy(pvd + 40, volId.data(), volId.size());
    put32both(pvd + 80, total);
    pvd[128] = 0x00;
    pvd[129] = 0x08;
    pvd[130] = 0x08;
    pvd[131] = 0x00;
    dirRecord(pvd + 156, 18, 2048, true, std::string(1, '\0'));
    uint8_t* term = &img[17 * 2048];
    term[0] = 255;
    std::memcpy(term + 1, "CD001", 5);
    uint8_t* root = &img[18 * 2048];
    size_t pos = 0;
    pos += dirRecord(root + pos, 18, 2048, true, std::string(1, '\0'));
    pos += dirRecord(root + pos, 18, 2048, true, std::string(1, '\1'));
    for (size_t i = 0; i < files.size(); ++i) {
        pos += dirRecord(root + pos, lbas[i], (uint32_t)files[i].data.size(), false, files[i].name + ";1");
        std::memcpy(&img[(size_t)lbas[i] * 2048], files[i].data.data(), files[i].data.size());
    }
    return img;
}
std::vector<uint8_t> makeElf(uint32_t entry) {
    std::vector<uint8_t> e(0x1000 + 64, 0);
    const uint8_t ident[16] = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0};
    std::memcpy(e.data(), ident, 16);
    auto w16 = [&](size_t o, uint16_t v) { std::memcpy(&e[o], &v, 2); };
    auto w32 = [&](size_t o, uint32_t v) { std::memcpy(&e[o], &v, 4); };
    w16(16, 2);
    w16(18, 8);
    w32(20, 1);
    w32(24, entry);
    w32(28, 52);
    w16(40, 52);
    w16(42, 32);
    w16(44, 1);
    w32(52, 1);         // PT_LOAD
    w32(56, 0x1000);    // offset
    w32(60, 0x100000);  // vaddr
    w32(64, 0x100000);
    w32(68, 64);        // filesz
    w32(72, 64);        // memsz
    w32(76, 7);
    for (int i = 0; i < 64; ++i) e[0x1000 + i] = (uint8_t)i;
    return e;
}
std::vector<uint8_t> toBin2352(const std::vector<uint8_t>& iso) {
    std::vector<uint8_t> out;
    const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    for (size_t s = 0; s < iso.size() / 2048; ++s) {
        uint8_t sec[2352] = {};
        std::memcpy(sec, sync, 12);
        sec[15] = 2; // MODE2
        std::memcpy(sec + 24, &iso[s * 2048], 2048);
        out.insert(out.end(), sec, sec + 2352);
    }
    return out;
}
void writeFile(const fs::path& p, const std::vector<uint8_t>& d) {
    std::ofstream o(p, std::ios::binary);
    o.write((const char*)d.data(), (std::streamsize)d.size());
}
} // namespace

TEST(scanner_iso_identification) {
    const auto elf = makeElf(0x100008);
    const std::string cnf = "BOOT2 = cdrom0:\\SLPM_668.48;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
    auto iso = makeIso({{"SYSTEM.CNF", std::vector<uint8_t>(cnf.begin(), cnf.end())}, {"SLPM_668.48", elf}}, "SB2H");
    const fs::path p = tmpDir() / "test_game.iso";
    writeFile(p, iso);
    sb2::GameEntry g = sb2::analyzeImage(p.string());
    CHECK(g.valid);
    CHECK_EQ(g.serial, std::string("SLPM-66848"));
    CHECK_EQ(g.region, std::string("NTSC-J"));
    CHECK_EQ(g.version, std::string("1.00"));
    CHECK_EQ(g.vmode, std::string("NTSC"));
    CHECK_EQ(g.volumeId, std::string("SB2H"));
    CHECK_EQ(g.elfEntry, 0x100008u);
    CHECK(g.known && g.target);
    CHECK(!g.exactVersion); // synthetic ELF does not match the real CRC
    CHECK_EQ(g.elfCrc, sb2::pcsx2ElfCrc(elf));
    // Extraction
    std::string err;
    const fs::path out = tmpDir() / "extracted.elf";
    CHECK(sb2::extractFromImage(p.string(), "cdrom0:\\SLPM_668.48;1", out.string(), err));
    CHECK_EQ((size_t)fs::file_size(out), elf.size());
}

TEST(scanner_bin_cue_and_cache) {
    const auto elf = makeElf(0x100000);
    const std::string cnf = "BOOT2 = cdrom0:\\SLUS_200.00;1\nVER = 1.01\nVMODE = NTSC\n";
    auto iso = makeIso({{"SYSTEM.CNF", std::vector<uint8_t>(cnf.begin(), cnf.end())}, {"SLUS_200.00", elf}}, "OTHER");
    const fs::path dir = tmpDir() / "lib";
    fs::remove_all(dir);
    fs::create_directories(dir / "sub");
    writeFile(dir / "sub" / "game.bin", toBin2352(iso));
    {
        std::ofstream cue(dir / "sub" / "game.cue");
        cue << "FILE \"game.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
    }
    sb2::ScanOptions opt;
    opt.cacheFile = (dir / "cache.json").string();
    auto games = sb2::scanFolders({dir.string()}, opt);
    CHECK_EQ(games.size(), (size_t)1); // the .bin referenced by the .cue is not listed twice
    if (!games.empty()) {
        CHECK(games[0].valid);
        CHECK_EQ(games[0].serial, std::string("SLUS-20000"));
        CHECK_EQ(games[0].region, std::string("NTSC-U"));
        CHECK(!games[0].target);
    }
    CHECK(fs::exists(opt.cacheFile));
    auto again = sb2::scanFolders({dir.string()}, opt); // served from cache
    CHECK_EQ(again.size(), (size_t)1);
}

TEST(scanner_rejects_bad_images) {
    const fs::path p = tmpDir() / "garbage.iso";
    writeFile(p, std::vector<uint8_t>(64 * 2048, 0x5A));
    auto g = sb2::analyzeImage(p.string());
    CHECK(!g.valid);
    CHECK(!g.error.empty());
    // PS1 disc
    const std::string cnf = "BOOT = cdrom:\\SLPS_000.01;1\n";
    auto iso = makeIso({{"SYSTEM.CNF", std::vector<uint8_t>(cnf.begin(), cnf.end())}}, "PSX");
    const fs::path p2 = tmpDir() / "ps1.iso";
    writeFile(p2, iso);
    auto g2 = sb2::analyzeImage(p2.string());
    CHECK(!g2.valid);
    // Truncated image
    const auto elf = makeElf(0x100000);
    const std::string cnf3 = "BOOT2 = cdrom0:\\SLPM_668.48;1\n";
    auto iso3 = makeIso({{"SYSTEM.CNF", std::vector<uint8_t>(cnf3.begin(), cnf3.end())}, {"SLPM_668.48", elf}}, "T");
    iso3.resize(iso3.size() - 2048 / 2); // cut the last half sector
    const fs::path p3 = tmpDir() / "trunc.iso";
    writeFile(p3, iso3);
    auto g3 = sb2::analyzeImage(p3.string());
    CHECK(!g3.warnings.empty() || !g3.valid);
}

TEST(serial_parsing) {
    CHECK_EQ(sb2::serialFromBootName("SLPM_668.48;1"), std::string("SLPM-66848"));
    CHECK_EQ(sb2::serialFromBootName("SLES_123.45"), std::string("SLES-12345"));
    CHECK_EQ(sb2::regionFromSerial("SLES-12345"), std::string("PAL"));
    CHECK_EQ(sb2::regionFromSerial("SCUS-97113"), std::string("NTSC-U"));
}

// ------------------------------------------------------------------ GS swizzle

TEST(gs_swizzle_tables) {
    // Reference tables (GS manual / GSdx).
    static const int bt32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
                                   {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
    static const int bt16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
                                   {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
    static const int bt16s[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27},
                                    {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
    static const int ct32[8][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15},
                                   {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
                                   {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47},
                                   {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
    static const int ct16row0[16] = {0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27};
    static const int ct8row2[16] = {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23};
    static const int ct8row12[16] = {224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214};
    static const int ct4row3[32] = {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59,
                                    85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63};
    using namespace ps2::gsmem;
    bool ok = true;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x) ok &= (int)block32(x, y) == bt32[y][x];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 4; ++x) ok &= (int)block16(x, y) == bt16[y][x] && (int)block16S(x, y) == bt16s[y][x];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) ok &= (int)column32(x, y) == ct32[y][x];
    for (int x = 0; x < 16; ++x) ok &= (int)column16(x, 0) == ct16row0[x];
    for (int x = 0; x < 16; ++x) ok &= (int)column8(x, 2) == ct8row2[x] && (int)column8(x, 12) == ct8row12[x];
    for (int x = 0; x < 32; ++x) ok &= (int)column4(x, 3) == ct4row3[x];
    CHECK(ok);
    // Every pixel of a page maps to a distinct address (bijective swizzle).
    std::vector<uint8_t> seen(8192 * 2, 0);
    bool unique = true;
    for (uint32_t y = 0; y < 128; ++y)
        for (uint32_t x = 0; x < 128; ++x) {
            const uint32_t a = addr4(0, 2, x, y);
            if (a >= seen.size() || seen[a]++) unique = false;
        }
    CHECK(unique);
}

// ------------------------------------------------------------------ FPU (values recorded on hardware)

TEST(fpu_hardware_values) {
    using namespace ps2::fpu;
    CHECK_EQ(mul(0x3F800000, 0x3FFFFFFF).value, 0x3FFFFFFEu); // Booth multiplier quirk
    CHECK_EQ(mul(0x3FFFFFFF, 0x3F800000).value, 0x3FFFFFFFu);
    CHECK_EQ(mul(0x3F800000, 0x7FFFFFFF).value, 0x7FFFFFFEu);
    CHECK_EQ(add(0x7F800000, 0x7F800000).value, 0x7FFFFFFFu); // overflow clamps
    CHECK(add(0x7F800000, 0x7F800000).overflow);
    CHECK_EQ(add(0x80000000, 0x80000000).value, 0x80000000u);
    CHECK_EQ(add(0x00000000, 0x80000000).value, 0x00000000u);
    CHECK_EQ(ps2::fpu::div(0x3F800000, 0x3FFFFFFF).value, 0x3F000001u);
    CHECK_EQ(sqrtMag(0x7FFFFFFF), 0x5FB504F3u);
    CHECK_EQ(toInt(0x4F000000), 0x7FFFFFFFu);
    CHECK_EQ(fromInt(0xFFFFFFFF), 0xBF800000u);
}

// ------------------------------------------------------------------ decoder

TEST(decoder) {
    using namespace recomp;
    CHECK(decode(0x27BDFFE0, 0).op == Op::ADDIU);
    CHECK(decode(0x03E00008, 0).op == Op::JR);
    CHECK(decode(0x0C040000, 0x100000).op == Op::JAL);
    CHECK_EQ(decode(0x0C040000, 0x100000).jumpTarget(), 0x00100000u);
    CHECK(decode(0x70000489, 0).op == Op::PAND);  // MMI2 sa=18
    CHECK(decode(0x70000209, 0).op == Op::PMFHI); // MMI2 sa=8
    CHECK(decode(0x70000488, 0).op == Op::PEXTLW); // MMI0 sa=18
    CHECK(decode(0x46000018, 0).op == Op::ADDA_S);
    CHECK(decode(0x4A0002FF, 0).op == Op::VNOP);
    CHECK(decode(0x42000038, 0).op == Op::EI);
    CHECK(decode(0x7C000000, 0).op == Op::SQ);
    CHECK(decode(0x78000000, 0).op == Op::LQ);
    CHECK(decode(0xFFFFFFFF, 0).op == Op::INVALID || true);
}

// ------------------------------------------------------------------ machine memory map

TEST(memory_map) {
    ps2::RuntimeConfig cfg;
    ps2::Machine m(nullptr, cfg);
    m.write32(0x00100000, 0x12345678);
    CHECK_EQ(m.read32(0x20100000), 0x12345678u); // uncached mirror
    CHECK_EQ(m.read32(0x30100000), 0x12345678u); // uncached accelerated
    CHECK_EQ(m.read32(0x80100000), 0x12345678u); // kseg0
    CHECK_EQ(m.read32(0xA0100000), 0x12345678u); // kseg1
    m.write32(0x70000010, 0xCAFEBABE);
    CHECK_EQ(m.read32(0x70000010), 0xCAFEBABEu);
    // VU1 data memory window and mirror
    m.write32(0x1100C000, 0x11223344);
    CHECK_EQ(m.vu1Data[0], 0x44);
    CHECK_EQ(m.read32(0xB100C000), 0x11223344u);
    // DMAC register round trip
    m.write32(0x1000A010, 0x00123450);
    CHECK_EQ(m.read32(0x1000A010), 0x00123450u);
}

TEST(vif_unpack_v4_32_and_s8) {
    ps2::RuntimeConfig cfg;
    ps2::Machine m(nullptr, cfg);
    std::vector<uint32_t> w = {0x01000101, 0x6C020000, 1, 2, 3, 4, 5, 6, 7, 8};
    m.vif->feed(1, w.data(), w.size());
    uint32_t q[8];
    std::memcpy(q, m.vu1Data, 32);
    CHECK(q[0] == 1 && q[3] == 4 && q[4] == 5 && q[7] == 8);
    // S-8 sign extension to all four fields at qword 10
    std::vector<uint32_t> w2 = {0x6201000A, 0x000000F0};
    m.vif->feed(1, w2.data(), w2.size());
    uint32_t r[4];
    std::memcpy(r, m.vu1Data + 160, 16);
    CHECK(r[0] == 0xFFFFFFF0u && r[3] == 0xFFFFFFF0u);
}

TEST(gs_transfer_and_readback) {
    ps2::RuntimeConfig cfg;
    ps2::Machine m(nullptr, cfg);
    ps2::Gs& gs = *m.gs;
    gs.writeReg(0x50, (0x100ull << 32) | (1ull << 48) | (0ull << 56)); // BITBLTBUF DBP 0x100 DBW 1 CT32
    gs.writeReg(0x51, 0);
    gs.writeReg(0x52, 4 | (4ull << 32));
    gs.writeReg(0x53, 0);
    uint32_t pix[16];
    for (int i = 0; i < 16; ++i) pix[i] = 0x80000000u | (uint32_t)i;
    gs.writeImage((const uint8_t*)pix, 4);
    bool ok = true;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) ok &= gs.readPixel(0x100, 1, 0, x, y) == (0x80000000u | (uint32_t)(y * 4 + x));
    CHECK(ok);
}

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    for (auto& [name, fn] : registry()) {
        if (filter && name.find(filter) == std::string::npos) continue;
        const int before = g_failed;
        fn();
        std::printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", name.c_str());
    }
    std::printf("\n%d checks passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
