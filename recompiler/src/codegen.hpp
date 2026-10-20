// C++ code generation from analysed R5900 code.
#pragma once
#include <string>

#include "analysis.hpp"

namespace recomp {

struct CodegenOptions {
    std::string outDir;
    std::string moduleName = "game";
    std::string elfSha256;
    size_t instructionsPerFile = 16000;
    bool comments = true; // emit disassembly comments
};

struct CodegenResult {
    size_t files = 0;
    size_t functions = 0;
    size_t labels = 0;
    size_t unsupported = 0;
};

CodegenResult generate(const Program& prog, const CodegenOptions& opt);

} // namespace recomp
