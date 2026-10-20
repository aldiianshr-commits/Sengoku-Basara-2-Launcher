// Host program entry and front-end interface.
#pragma once
#include <string>

#include "context.hpp"

namespace ps2 {

class Machine;

// A windowed front-end (video/audio/input).  Implemented in the platform
// library; the runtime itself has no windowing dependency.
class Frontend {
public:
    virtual ~Frontend() = default;
    // Run the machine with a window until exit; returns the exit code.
    virtual int run(Machine& m, const std::string& configFile) = 0;
};

void setFrontend(Frontend* f);
Frontend* frontend();

int hostMain(int argc, char** argv, const ModuleInfo* mod);

} // namespace ps2

// Implemented by every recompiled module.
extern "C" const ps2::ModuleInfo ps2_module_info;
