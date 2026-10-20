#include "analysis.hpp"

#include <algorithm>
#include <cstdio>

namespace recomp {

namespace {
constexpr uint32_t kJrRa = 0x03E00008;
bool isPrologue(uint32_t raw) {
    // addiu $sp, $sp, -N   or   daddiu $sp, $sp, -N
    const uint32_t top = raw & 0xFFFF0000u;
    return (top == 0x27BD0000u || top == 0x67BD0000u) && (raw & 0x8000u);
}
} // namespace

Program::Program(const ElfImage& elf, const AnalysisOptions& opt) : elf_(elf) {
    exec_ = elf.execRanges();
    for (auto& s : elf.symbols)
        if (s.type == 2 && !s.name.empty()) symNames_[s.value] = s.name;
    discoverEntries(opt);
    partition();
    computeLabels();
}

bool Program::inExec(uint32_t addr) const {
    for (auto& r : exec_)
        if (addr >= r.first && addr < r.second) return true;
    return false;
}

Insn Program::insnAt(uint32_t addr) const {
    auto w = elf_.word(addr);
    if (!w) {
        Insn in;
        in.addr = addr;
        in.op = Op::INVALID;
        return in;
    }
    return decode(*w, addr);
}

void Program::discoverEntries(const AnalysisOptions& opt) {
    auto addEntry = [&](uint32_t a) {
        if ((a & 3) == 0 && inExec(a)) return entries_.insert(a).second;
        return false;
    };
    addEntry(elf_.entry);
    for (auto& r : exec_) addEntry(r.first);

    for (auto& s : elf_.symbols)
        if (s.type == 2 /*FUNC*/ && addEntry(s.value)) stats_.entriesFromSymbols++;
    for (uint32_t a : opt.extraEntries) addEntry(a);

    // Pass over code: call targets, address constants, returns.
    for (auto& r : exec_) {
        uint32_t luiVal[32] = {};
        uint32_t luiAt[32] = {};
        for (uint32_t a = r.first; a + 4 <= r.second; a += 4) {
            Insn in = insnAt(a);
            stats_.instructions++;
            if (in.op == Op::INVALID) {
                stats_.invalid++;
                continue;
            }
            if (in.kind() == Kind::JumpLink) {
                if (addEntry(in.jumpTarget())) stats_.entriesFromCalls++;
            } else if (in.kind() == Kind::BranchLink || in.kind() == Kind::BranchLinkLikely) {
                if (addEntry(in.branchTarget())) stats_.entriesFromCalls++;
            }
            if (in.raw == kJrRa) {
                uint32_t n = a + 8;
                while (n + 4 <= r.second && elf_.word(n).value_or(1) == 0 && n < a + 64) n += 4;
                if (n < r.second) afterReturn_.insert(n);
            }
            // Track lui/addiu|ori pairs that build code addresses.
            const uint32_t opc = in.raw >> 26;
            if (in.op == Op::LUI) {
                luiVal[in.rt()] = in.uimm() << 16;
                luiAt[in.rt()] = a;
            } else if ((opc == 9 /*addiu*/ || opc == 13 /*ori*/ || opc == 25 /*daddiu*/) && in.rs() != 0 &&
                       luiAt[in.rs()] != 0 && a - luiAt[in.rs()] <= 64) {
                uint32_t v = opc == 13 ? (luiVal[in.rs()] | in.uimm()) : (luiVal[in.rs()] + (uint32_t)in.simm());
                if ((v & 3) == 0 && inExec(v)) codeRefs_.insert(v);
            }
        }
    }

    // Data words that point into code (function pointer tables, jump tables, vtables).
    auto scanData = [&](uint32_t lo, uint32_t hi) {
        for (uint32_t a = (lo + 3) & ~3u; a + 4 <= hi; a += 4) {
            auto w = elf_.word(a);
            if (w && (*w & 3) == 0 && inExec(*w)) {
                codeRefs_.insert(*w);
                stats_.dataCodeRefs++;
            }
        }
    };
    bool haveSections = false;
    for (auto& s : elf_.sections) {
        if (!(s.flags & 2) || (s.flags & 4) || s.type != 1) continue; // ALLOC, !EXEC, PROGBITS
        haveSections = true;
        scanData(s.addr, s.addr + s.size);
    }
    if (!haveSections) {
        for (auto& seg : elf_.segments)
            if (!(seg.flags & 1)) scanData(seg.vaddr, seg.vaddr + seg.filesz);
    }

    // Promote code references that look like function starts.
    auto precededByReturn = [&](uint32_t t) {
        uint32_t p = t;
        for (int i = 0; i < 8 && p >= 8; ++i) {
            if (elf_.word(p - 8).value_or(0) == kJrRa) return true;
            if (elf_.word(p - 4).value_or(1) != 0) return false; // only skip nop padding
            p -= 4;
        }
        return false;
    };
    for (uint32_t t : codeRefs_) {
        bool rangeStart = false;
        for (auto& r : exec_) rangeStart |= (t == r.first);
        if (rangeStart || precededByReturn(t)) {
            if (addEntry(t)) stats_.entriesFromDataRefs++;
        }
    }
    for (uint32_t t : afterReturn_) {
        auto w = elf_.word(t);
        if (w && isPrologue(*w) && addEntry(t)) stats_.entriesFromPrologues++;
    }
}

void Program::partition() {
    std::vector<uint32_t> starts(entries_.begin(), entries_.end());
    for (size_t i = 0; i < starts.size(); ++i) {
        const uint32_t s = starts[i];
        uint32_t rangeEnd = 0;
        for (auto& r : exec_)
            if (s >= r.first && s < r.second) rangeEnd = r.second;
        uint32_t e = rangeEnd;
        if (i + 1 < starts.size() && starts[i + 1] < e) e = starts[i + 1];
        Function f;
        f.start = s;
        f.end = e;
        auto it = symNames_.find(s);
        if (it != symNames_.end()) f.name = it->second;
        funcs_.push_back(std::move(f));
    }
}

const Function* Program::containing(uint32_t addr) const {
    auto it = std::upper_bound(funcs_.begin(), funcs_.end(), addr,
                               [](uint32_t a, const Function& f) { return a < f.start; });
    if (it == funcs_.begin()) return nullptr;
    --it;
    return (addr >= it->start && addr < it->end) ? &*it : nullptr;
}

const Function* Program::functionAt(uint32_t start) const {
    const Function* f = containing(start);
    return (f && f->start == start) ? f : nullptr;
}

void Program::computeLabels() {
    // Pass 1: labels inside each function and cross-function targets.
    for (auto& f : funcs_) {
        auto inF = [&](uint32_t a) { return a >= f.start && a < f.end; };
        f.labels.insert(f.start);
        for (auto it = codeRefs_.lower_bound(f.start); it != codeRefs_.end() && *it < f.end; ++it)
            f.labels.insert(*it);
        for (auto it = afterReturn_.lower_bound(f.start); it != afterReturn_.end() && *it < f.end; ++it)
            f.labels.insert(*it);
        for (uint32_t a = f.start; a < f.end; a += 4) {
            Insn in = insnAt(a);
            if (!in.isControl()) continue;
            if (in.hasStaticTarget()) {
                const uint32_t t = in.staticTarget();
                if (inF(t)) f.labels.insert(t);
                else if (Function* g = const_cast<Function*>(containing(t))) g->labels.insert(t);
            }
            const Kind k = in.kind();
            if (k == Kind::JumpLink || k == Kind::JumpLinkReg || k == Kind::BranchLink ||
                k == Kind::BranchLinkLikely) {
                if (inF(a + 8)) f.labels.insert(a + 8); // return address
            }
            Insn d = insnAt(a + 4);
            if (d.isControl()) {
                stats_.branchInDelaySlot++;
                if (inF(a + 4)) f.labels.insert(a + 4);
                if (in.hasStaticTarget()) {
                    const uint32_t t4 = in.staticTarget() + 4;
                    if (inF(t4)) f.labels.insert(t4);
                    else if (Function* g = const_cast<Function*>(containing(t4))) g->labels.insert(t4);
                }
                if (inF(a + 8)) f.labels.insert(a + 8);
            }
        }
    }
    // Pass 2: delay-slot cascade.  If a delay slot is itself a jump target,
    // the fall-through path needs a label after it (see codegen).
    for (auto& f : funcs_) {
        for (uint32_t a = f.start; a < f.end; a += 4) {
            Insn in = insnAt(a);
            if (in.isControl() && f.labels.count(a + 4) && a + 8 < f.end) f.labels.insert(a + 8);
        }
    }
}

} // namespace recomp
