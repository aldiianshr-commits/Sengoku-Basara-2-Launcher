// main() for a headless recompiled program (tests, CLI runs).
#include "ps2/host.hpp"

int main(int argc, char** argv) { return ps2::hostMain(argc, argv, &ps2_module_info); }
