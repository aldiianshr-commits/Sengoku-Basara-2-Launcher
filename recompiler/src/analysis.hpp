// Control-flow analysis: discovers function entry points (works on stripped
// executables), partitions the code into functions and computes, for each
// function, every address that must be reachable as a jump/dispatch target.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "elf.hpp"
#include "r5900_isa.hpp"

namespace recomp {

struct Function {
    uint32_t start = 0;
    uint32_t end = 0; // exclusive
    std::string name; // from symbols, may be empty
    std::set<uint32_t> labels;
};

struct AnalysisOptions {
    std::set<uint32_t> extraEntries; // e.g. discovered at runtime
    bool verbose = false;
};

struct AnalysisStats {
    size_t instructions = 0;
    size_t invalid = 0;
    size_t entriesFromSymbols = 0;
    size_t entriesFromCalls = 0;
    size_t entriesFromDataRefs = 0;
    size_t entriesFromPrologues = 0;
    size_t branchInDelaySlot = 0;
    size_t dataCodeRefs = 0;
};

class Program {
public:
    Program(const ElfImage& elf, const AnalysisOptions& opt);

    const ElfImage& elf() const { return elf_; }
    const std::vector<Function>& functions() const { return funcs_; }
    const std::vector<std::pair<uint32_t, uint32_t>>& execRanges() const { return exec_; }
    const AnalysisStats& stats() const { return stats_; }

    bool inExec(uint32_t addr) const;
    // Instruction at addr (decoded on demand from the image); INVALID if none.
    Insn insnAt(uint32_t addr) const;
    const Function* containing(uint32_t addr) const;
    const Function* functionAt(uint32_t start) const; // exact start

private:
    void discoverEntries(const AnalysisOptions& opt);
    void partition();
    void computeLabels();

    const ElfImage& elf_;
    std::vector<std::pair<uint32_t, uint32_t>> exec_;
    std::set<uint32_t> entries_;
    std::set<uint32_t> codeRefs_;    // addresses referenced as code from data/constants
    std::set<uint32_t> afterReturn_; // first instruction after "jr $ra; delay"
    std::map<uint32_t, std::string> symNames_;
    std::vector<Function> funcs_;
    AnalysisStats stats_;
};

} // namespace recomp
