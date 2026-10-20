// Native implementations of standard IOP module RPC services.
#pragma once

namespace ps2 {
class Machine;
class Iop;
void registerStandardServices(Machine& m, Iop& iop);
} // namespace ps2
