// Native (HLE) implementations of the RPC services provided by the standard
// IOP modules.  The packet layouts follow the protocols spoken by Sony's EE
// libraries (and by their open-source ps2sdk counterparts):
//
//   0x80000001  FILEIO        (iop.cpp)
//   0x80000003  IOP heap      sceSifAllocIopHeap / sceSifFreeIopHeap
//   0x80000006  LOADFILE      sceSifLoadModule / sceSifLoadElf / IOP memory
//   0x80000592  CDVDFSV init  sceCdInit
//   0x80000593  CDVDFSV S-cmd sceCdReadClock, sceCdGetDiskType, sceCdStatus ...
//   0x80000595  CDVDFSV N-cmd sceCdRead, sceCdSt*, sceCdReadIOPMem ...
//   0x80000596  CDVDFSV poweroff callback
//   0x80000597  CDVDFSV       sceCdSearchFile
//   0x8000059A  CDVDFSV       sceCdDiskReady
//   0x80000100/101, 0x8000010F/11F  PADMAN (new / old protocol)  iop_pad.cpp
//   0x80000400  MCSERV        iop_mc.cpp
//   0x80000701  SDRDRV (libsd remote)                             iop_sdr.cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ps2 {
class Machine;
class Iop;

void registerStandardServices(Machine& m, Iop& iop);
void registerPadServices(Machine& m);
void registerMcServices(Machine& m);
void registerSdrServices(Machine& m);

namespace hle {
inline uint32_t rd32(const std::vector<uint8_t>& v, size_t off) {
    uint32_t x = 0;
    if (off + 4 <= v.size()) std::memcpy(&x, v.data() + off, 4);
    return x;
}
inline uint16_t rd16(const std::vector<uint8_t>& v, size_t off) {
    uint16_t x = 0;
    if (off + 2 <= v.size()) std::memcpy(&x, v.data() + off, 2);
    return x;
}
inline void wr32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    if (v.size() < off + 4) v.resize(off + 4, 0);
    std::memcpy(v.data() + off, &x, 4);
}
inline std::string rdStr(const std::vector<uint8_t>& v, size_t off, size_t max = 1024) {
    std::string s;
    for (size_t i = off; i < v.size() && i < off + max && v[i]; ++i) s.push_back((char)v[i]);
    return s;
}
inline std::vector<uint8_t> reply32(int32_t r) {
    std::vector<uint8_t> o(4);
    std::memcpy(o.data(), &r, 4);
    return o;
}
} // namespace hle

} // namespace ps2
