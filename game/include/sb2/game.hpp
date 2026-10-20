// Entry point of a recompiled game executable with the windowed front-end
// (SDL3 window/input/audio, Direct3D 12 presentation, in-game menu).
#pragma once
#include "ps2/context.hpp"

namespace sb2 {

// Parses the command line (see --help), creates the PS2 machine for the
// recompiled module and runs it with the front-end.  Recognised extra
// options (on top of the runtime's):
//   --title TEXT            window title
//   --control-stdin         accept "pause", "resume", "stop", "screenshot" on stdin
//   --screenshot-dir DIR    where F12 screenshots are written
//   --exit-screenshot F     write a screenshot of the last presented frame (PNG/BMP)
int gameMain(int argc, char** argv, const ps2::ModuleInfo* mod);

} // namespace sb2

// Exported by the recompiled module (recomp_table.cpp).
extern "C" const ps2::ModuleInfo ps2_module_info;
