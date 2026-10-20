// SDRDRV (libsd remote) service: sceSdRemote() calls from the EE are
// executed against the SPU2 model (see spu2.cpp / Spu2::sd*).
#include "iop_services.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"
#include "ps2/sif.hpp"
#include "ps2/spu2.hpp"

namespace ps2 {

using namespace hle;

namespace {

class SdrServer final : public RpcServer {
public:
    explicit SdrServer(Machine& m) : m_(m) {}
    const char* name() const override { return "SDRDRV"; }
    int64_t latency(uint32_t) const override { return 3000; }
    std::vector<uint8_t> call(uint32_t fno, const std::vector<uint8_t>& in, uint32_t rsize) override {
        std::vector<uint8_t> out(std::max<uint32_t>(rsize, 16), 0);
        // Generic layout: sbuff[0] = EE address of sbuff, sbuff[1..6] = arguments.
        const uint32_t a1 = rd32(in, 4), a2 = rd32(in, 8), a3 = rd32(in, 12), a4 = rd32(in, 16);
        const uint32_t cmd = fno & 0xFFF0;
        int32_t r = m_.spu2->sdRemote(cmd, fno & 0xF, a1, a2, a3, a4, in);
        wr32(out, 0, (uint32_t)r);
        return out;
    }

private:
    Machine& m_;
};

} // namespace

void registerSdrServices(Machine& m) { m.sif->registerServer(0x80000701, std::make_shared<SdrServer>(m)); }

} // namespace ps2
