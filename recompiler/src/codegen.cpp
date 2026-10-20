#include "codegen.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace recomp {

namespace {

std::string hex8(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "%08x", v);
    return b;
}
std::string fnName(uint32_t a) { return "f_" + hex8(a); }
std::string label(uint32_t a) { return "L_" + hex8(a); }
std::string imm(uint32_t a) { return "0x" + hex8(a) + "u"; }

const char* condFn(Op op) {
    switch (op) {
    case Op::BEQ: case Op::BEQL: return "BC_BEQ";
    case Op::BNE: case Op::BNEL: return "BC_BNE";
    case Op::BLEZ: case Op::BLEZL: return "BC_BLEZ";
    case Op::BGTZ: case Op::BGTZL: return "BC_BGTZ";
    case Op::BLTZ: case Op::BLTZL: case Op::BLTZAL: case Op::BLTZALL: return "BC_BLTZ";
    case Op::BGEZ: case Op::BGEZL: case Op::BGEZAL: case Op::BGEZALL: return "BC_BGEZ";
    case Op::BC0F: case Op::BC0FL: return "BC_BC0F";
    case Op::BC0T: case Op::BC0TL: return "BC_BC0T";
    case Op::BC1F: case Op::BC1FL: return "BC_BC1F";
    case Op::BC1T: case Op::BC1TL: return "BC_BC1T";
    case Op::BC2F: case Op::BC2FL: return "BC_BC2F";
    case Op::BC2T: case Op::BC2TL: return "BC_BC2T";
    default: return nullptr;
    }
}

class FunctionEmitter {
public:
    FunctionEmitter(const Program& p, const Function& f, const CodegenOptions& o, CodegenResult& r)
        : prog_(p), f_(f), opt_(o), res_(r) {}

    std::string run() {
        out_ << "// " << hex8(f_.start) << " - " << hex8(f_.end);
        if (!f_.name.empty()) out_ << "  " << f_.name;
        out_ << "\nvoid " << fnName(f_.start) << "(ps2::Ctx* __restrict c) {\n";
        out_ << "    using namespace ps2::ops;\n";
        out_ << "    if (PS2_LIKELY(c->pc == " << imm(f_.start) << ")) goto " << label(f_.start) << ";\n";
        out_ << "dispatch:\n    switch (c->pc) {\n";
        for (uint32_t l : f_.labels) out_ << "    case " << imm(l) << ": goto " << label(l) << ";\n";
        out_ << "    default:\n";
        out_ << "        if (c->pc >= " << imm(f_.start) << " && c->pc < " << imm(f_.end)
             << ") { ps2::rt_badEntry(c, " << imm(f_.start) << "); return; }\n";
        out_ << "        ps2::rt_jump(c);\n        return;\n    }\n";
        res_.labels += f_.labels.size();

        uint32_t pos = f_.start;
        bool canFallThrough = true;
        while (pos < f_.end) {
            if (f_.labels.count(pos)) {
                flushCycles();
                out_ << label(pos) << ":\n";
            }
            Insn in = prog_.insnAt(pos);
            if (in.isControl()) {
                emitControl(in);
                canFallThrough = !(in.kind() == Kind::Jump || in.kind() == Kind::JumpReg ||
                                   (in.op == Op::BEQ && in.rs() == 0 && in.rt() == 0));
                // Fall-through continues after the delay slot.  If the delay
                // slot itself is a jump target, emit it standalone.
                if (f_.labels.count(pos + 4) && pos + 4 < f_.end) {
                    if (canFallThrough) out_ << "    " << transfer(pos + 8, pos) << "\n";
                    pos += 4;
                    canFallThrough = true;
                } else {
                    pos += 8;
                }
                continue;
            }
            ++pending_;
            emitSimple(in, "    ");
            canFallThrough = !(in.kind() == Kind::Eret);
            pos += 4;
        }
        flushCycles();
        // Running off the end of the function: continue in the next one.
        out_ << "    c->pc = " << imm(pos) << ";\n";
        if (const Function* nf = prog_.functionAt(pos)) out_ << "    " << fnName(nf->start) << "(c);\n";
        else out_ << "    ps2::rt_jump(c);\n";
        out_ << "}\n\n";
        (void)canFallThrough;
        return out_.str();
    }

private:
    void flushCycles() {
        if (pending_) {
            out_ << "    PS2_CYCLES(" << pending_ << ");\n";
            pending_ = 0;
        }
    }
    bool inF(uint32_t a) const { return a >= f_.start && a < f_.end; }

    void comment(const Insn& in, const std::string& ind) {
        if (opt_.comments) out_ << ind << "// " << hex8(in.addr) << ": " << disassemble(in) << "\n";
    }

    void emitSimple(const Insn& in, const std::string& ind) {
        comment(in, ind);
        switch (in.kind()) {
        case Kind::Normal:
            out_ << ind << opName(in.op) << "(c, " << imm(in.raw) << ");\n";
            break;
        case Kind::Syscall:
            out_ << ind << "PS2_CYCLES(" << pending_ << "); c->pc = " << imm(in.addr + 4)
                 << "; ps2::rt_syscall(c, " << imm(in.addr) << ");\n";
            out_ << ind << "if (PS2_UNLIKELY(c->pc != " << imm(in.addr + 4) << ")) goto dispatch;\n";
            pending_ = 0;
            break;
        case Kind::Break:
            out_ << ind << "c->pc = " << imm(in.addr + 4) << "; ps2::rt_break(c, " << imm(in.addr) << ", "
                 << imm(in.raw) << ");\n";
            out_ << ind << "if (PS2_UNLIKELY(c->pc != " << imm(in.addr + 4) << ")) goto dispatch;\n";
            break;
        case Kind::Eret:
            out_ << ind << "ps2::rt_eret(c); return;\n";
            break;
        case Kind::Invalid:
        default:
            out_ << ind << "ps2::rt_invalid(c, " << imm(in.addr) << ", " << imm(in.raw) << ");\n";
            break;
        }
    }

    // Transfer to a static address (after delay slot executed).
    std::string transfer(uint32_t target, uint32_t from) {
        std::ostringstream s;
        if (inF(target)) {
            if (target <= from) s << "PS2_CHECK_EVENTS(); ";
            s << "goto " << label(target) << ";";
        } else if (const Function* tf = prog_.functionAt(target)) {
            s << "c->pc = " << imm(target) << "; " << fnName(tf->start) << "(c); return;";
        } else {
            s << "c->pc = " << imm(target) << "; ps2::rt_jump(c); return;";
        }
        return s.str();
    }

    std::string call(uint32_t target, uint32_t ret) {
        std::ostringstream s;
        s << "c->pc = " << imm(target) << "; ";
        if (const Function* tf = prog_.functionAt(target)) s << fnName(tf->start) << "(c); ";
        else s << "ps2::rt_jump(c); ";
        s << "if (PS2_UNLIKELY(c->pc != " << imm(ret) << ")) goto dispatch;";
        return s.str();
    }

    void emitDelay(const Insn& d, const std::string& ind) {
        if (d.isControl()) {
            // Handled by emitBranchInDelay; should not get here.
            out_ << ind << "ps2::rt_unsupported(c, " << imm(d.addr) << ", \"control transfer in delay slot\");\n";
            res_.unsupported++;
            return;
        }
        if (d.kind() == Kind::Syscall) {
            out_ << ind << "c->pc = " << imm(d.addr + 4) << "; ps2::rt_syscall(c, " << imm(d.addr) << ");\n";
            return;
        }
        emitSimple(d, ind);
    }

    void emitControl(const Insn& in) {
        const uint32_t a = in.addr;
        const Insn d = prog_.insnAt(a + 4);
        comment(in, "    ");
        if (d.isControl()) {
            emitBranchInDelay(in, d);
            return;
        }
        const size_t n = pending_ + 2;
        pending_ = 0;
        const std::string cyc = "PS2_CYCLES(" + std::to_string(n) + "); ";
        const uint32_t link = a + 8;
        switch (in.kind()) {
        case Kind::Branch: {
            out_ << "    {\n        const bool t = " << condFn(in.op) << "(c, " << imm(in.raw) << ");\n";
            emitDelay(d, "        ");
            out_ << "        " << cyc << "if (t) { " << transfer(in.branchTarget(), a) << " }\n    }\n";
            break;
        }
        case Kind::BranchLikely: {
            out_ << "    " << cyc << "if (" << condFn(in.op) << "(c, " << imm(in.raw) << ")) {\n";
            emitDelay(d, "        ");
            out_ << "        " << transfer(in.branchTarget(), a) << "\n    }\n";
            break;
        }
        case Kind::BranchLink:
        case Kind::BranchLinkLikely: {
            const bool likely = in.kind() == Kind::BranchLinkLikely;
            out_ << "    {\n        const bool t = " << condFn(in.op) << "(c, " << imm(in.raw) << ");\n";
            out_ << "        c->r[31].u64[0] = ps2::ops::sx32(" << imm(link) << ");\n";
            if (likely) out_ << "        if (t) {\n";
            emitDelay(d, likely ? "            " : "        ");
            if (likely) out_ << "        }\n";
            out_ << "        " << cyc << "if (t) { " << call(in.branchTarget(), link) << " }\n    }\n";
            break;
        }
        case Kind::Jump: {
            emitDelay(d, "    ");
            out_ << "    " << cyc << transfer(in.jumpTarget(), a) << "\n";
            break;
        }
        case Kind::JumpLink: {
            out_ << "    c->r[31].u64[0] = ps2::ops::sx32(" << imm(link) << ");\n";
            emitDelay(d, "    ");
            out_ << "    " << cyc << call(in.jumpTarget(), link) << "\n";
            break;
        }
        case Kind::JumpReg: {
            out_ << "    {\n        const uint32_t tgt = c->r[" << in.rs() << "].u32[0];\n";
            emitDelay(d, "        ");
            out_ << "        " << cyc << "c->pc = tgt;";
            if (in.rs() == 31) out_ << " return;\n    }\n";
            else out_ << " PS2_CHECK_EVENTS(); goto dispatch;\n    }\n";
            break;
        }
        case Kind::JumpLinkReg: {
            out_ << "    {\n        const uint32_t tgt = c->r[" << in.rs() << "].u32[0];\n";
            if (in.rd() != 0) out_ << "        c->r[" << in.rd() << "].u64[0] = ps2::ops::sx32(" << imm(link) << ");\n";
            emitDelay(d, "        ");
            out_ << "        " << cyc << "c->pc = tgt; ps2::rt_jump(c);\n";
            out_ << "        if (PS2_UNLIKELY(c->pc != " << imm(link) << ")) goto dispatch;\n    }\n";
            break;
        }
        default:
            out_ << "    ps2::rt_unsupported(c, " << imm(a) << ", \"unknown control op\");\n";
            res_.unsupported++;
        }
    }

    // A branch/jump whose delay slot holds another branch/jump.  Hardware
    // executes the second transfer in the delay slot: its own delay slot is
    // the first instruction at the first target.
    void emitBranchInDelay(const Insn& b1, const Insn& b2) {
        const uint32_t a = b1.addr;
        const bool b1Static = b1.isConditional() || b1.kind() == Kind::Jump;
        const bool b2Static = b2.isConditional() || b2.kind() == Kind::Jump;
        if (!b1Static || !b2Static) {
            out_ << "    ps2::rt_unsupported(c, " << imm(a) << ", \"branch in delay slot\");\n";
            res_.unsupported++;
            return;
        }
        const uint32_t t1 = b1.staticTarget();
        const uint32_t t2 = b2.staticTarget();
        flushCycles();
        auto isBc1 = [](Op o) { return o == Op::BC1F || o == Op::BC1T || o == Op::BC1FL || o == Op::BC1TL; };
        if ((b1.op == Op::BC1F || b1.op == Op::BC1T) && isBc1(b2.op)) {
            // Hardware quirk (verified on a real PS2): a non-likely BC1x whose
            // delay slot holds another BC1x has no effect of its own; the
            // second branch decides control flow.
            out_ << "    // BC1 in BC1 delay slot: first branch is overridden\n";
            out_ << "    PS2_CYCLES(1); " << transfer(a + 4, a) << "\n";
            return;
        }
        out_ << "    // branch with a branch in its delay slot\n";
        out_ << "    {\n        const bool t1 = "
             << (b1.isConditional() ? std::string(condFn(b1.op)) + "(c, " + imm(b1.raw) + ")" : "true") << ";\n";
        out_ << "        if (t1) {\n            const bool t2 = "
             << (b2.isConditional() ? std::string(condFn(b2.op)) + "(c, " + imm(b2.raw) + ")" : "true") << ";\n";
        if (b2.kind() == Kind::BranchLink || b2.kind() == Kind::BranchLinkLikely)
            out_ << "            c->r[31].u64[0] = ps2::ops::sx32(" << imm(b2.addr + 8) << ");\n";
        const Insn d2 = prog_.insnAt(t1);
        if (d2.isControl()) {
            out_ << "            ps2::rt_unsupported(c, " << imm(a) << ", \"triple branch chain\");\n";
            res_.unsupported++;
        } else {
            if (b2.isLikely()) out_ << "            if (t2) {\n";
            emitDelay(d2, b2.isLikely() ? "                " : "            ");
            if (b2.isLikely()) out_ << "            }\n";
        }
        out_ << "            PS2_CYCLES(3);\n";
        out_ << "            if (t2) { " << transfer(t2, a) << " }\n";
        out_ << "            " << transfer(t1 + 4, a) << "\n        }\n";
        out_ << "        PS2_CYCLES(1);\n";
        if (b1.isLikely()) out_ << "        " << transfer(a + 8, a) << "\n";
        else out_ << "        " << transfer(a + 4, a) << "\n";
        out_ << "    }\n";
    }

    const Program& prog_;
    const Function& f_;
    const CodegenOptions& opt_;
    CodegenResult& res_;
    std::ostringstream out_;
    size_t pending_ = 0;
};

void writeFile(const std::filesystem::path& p, const std::string& s) {
    // Only rewrite when content changed, so incremental builds stay fast.
    {
        std::ifstream in(p, std::ios::binary);
        if (in) {
            std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (old == s) return;
        }
    }
    std::ofstream o(p, std::ios::binary);
    if (!o) throw std::runtime_error("cannot write " + p.string());
    o << s;
}

} // namespace

CodegenResult generate(const Program& prog, const CodegenOptions& opt) {
    namespace fs = std::filesystem;
    CodegenResult res;
    fs::create_directories(opt.outDir);
    const auto& funcs = prog.functions();

    // Header with declarations.
    {
        std::ostringstream h;
        h << "// Generated by ps2recomp - do not edit.\n#pragma once\n#include <ps2/recomp_ops.hpp>\n\n";
        for (auto& f : funcs) h << "void " << fnName(f.start) << "(ps2::Ctx* __restrict c);\n";
        writeFile(fs::path(opt.outDir) / "recomp_funcs.h", h.str());
    }

    // Function bodies, split into several translation units.
    size_t fileIndex = 0, insnInFile = 0;
    std::ostringstream body;
    auto flush = [&]() {
        if (body.tellp() <= 0) return;
        std::ostringstream full;
        full << "// Generated by ps2recomp - do not edit.\n#include \"recomp_funcs.h\"\n";
        full << "#if defined(__clang__)\n#pragma clang diagnostic ignored \"-Wunused-label\"\n"
                "#elif defined(__GNUC__)\n#pragma GCC diagnostic ignored \"-Wunused-label\"\n#endif\n\n";
        full << body.str();
        char name[64];
        snprintf(name, sizeof name, "recomp_%03zu.cpp", fileIndex++);
        writeFile(fs::path(opt.outDir) / name, full.str());
        body.str("");
        body.clear();
        insnInFile = 0;
    };
    for (auto& f : funcs) {
        FunctionEmitter e(prog, f, opt, res);
        body << e.run();
        insnInFile += (f.end - f.start) / 4;
        res.functions++;
        if (insnInFile >= opt.instructionsPerFile) flush();
    }
    flush();
    res.files = fileIndex;

    // Function table + module info.
    {
        std::ostringstream t;
        t << "// Generated by ps2recomp - do not edit.\n#include \"recomp_funcs.h\"\n\n";
        t << "static const ps2::FuncEntry kFuncs[] = {\n";
        for (auto& f : funcs)
            t << "    {" << imm(f.start) << ", " << imm(f.end) << ", " << fnName(f.start) << "},\n";
        t << "};\n\n";
        uint32_t textLo = 0xFFFFFFFF, textHi = 0;
        for (auto& r : prog.execRanges()) {
            textLo = std::min(textLo, r.first);
            textHi = std::max(textHi, r.second);
        }
        t << "extern \"C\" const ps2::ModuleInfo ps2_module_info = {\n";
        t << "    \"" << opt.moduleName << "\",\n    \"" << opt.elfSha256 << "\",\n";
        t << "    " << imm(prog.elf().entry) << ",\n    kFuncs,\n    sizeof(kFuncs) / sizeof(kFuncs[0]),\n";
        t << "    " << imm(textLo) << ", " << imm(textHi) << ",\n    \"" << "0.1.0" << "\",\n};\n";
        writeFile(fs::path(opt.outDir) / "recomp_table.cpp", t.str());
        res.files += 1;
    }
    return res;
}

} // namespace recomp
