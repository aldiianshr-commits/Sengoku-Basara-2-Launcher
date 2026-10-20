#include "r5900_isa.hpp"

#include <cstdio>

namespace recomp {

namespace {
struct OpInfo {
    const char* name;
    Kind kind;
};
const OpInfo kOpInfo[] = {
#define X(name, kind) {#name, Kind::kind},
    R5900_OPS(X)
#undef X
    {"INVALID", Kind::Invalid},
};

using O = Op;

// SPECIAL (opcode 0), indexed by funct.
const Op kSpecial[64] = {
    O::SLL,  O::INVALID, O::SRL,   O::SRA,    O::SLLV,  O::INVALID, O::SRLV,  O::SRAV,
    O::JR,   O::JALR,    O::MOVZ,  O::MOVN,   O::SYSCALL, O::BREAK, O::INVALID, O::SYNC,
    O::MFHI, O::MTHI,    O::MFLO,  O::MTLO,   O::DSLLV, O::INVALID, O::DSRLV, O::DSRAV,
    O::MULT, O::MULTU,   O::DIV,   O::DIVU,   O::DMULT, O::DMULTU,  O::DDIV,  O::DDIVU,
    O::ADD,  O::ADDU,    O::SUB,   O::SUBU,   O::AND,   O::OR,      O::XOR,   O::NOR,
    O::MFSA, O::MTSA,    O::SLT,   O::SLTU,   O::DADD,  O::DADDU,   O::DSUB,  O::DSUBU,
    O::TGE,  O::TGEU,    O::TLT,   O::TLTU,   O::TEQ,   O::INVALID, O::TNE,   O::INVALID,
    O::DSLL, O::INVALID, O::DSRL,  O::DSRA,   O::DSLL32, O::INVALID, O::DSRL32, O::DSRA32,
};

// REGIMM (opcode 1), indexed by rt.
const Op kRegimm[32] = {
    O::BLTZ,   O::BGEZ,   O::BLTZL,   O::BGEZL,   O::INVALID, O::INVALID, O::INVALID, O::INVALID,
    O::TGEI,   O::TGEIU,  O::TLTI,    O::TLTIU,   O::TEQI,    O::INVALID, O::TNEI,    O::INVALID,
    O::BLTZAL, O::BGEZAL, O::BLTZALL, O::BGEZALL, O::INVALID, O::INVALID, O::INVALID, O::INVALID,
    O::MTSAB,  O::MTSAH,  O::INVALID, O::INVALID, O::INVALID, O::INVALID, O::INVALID, O::INVALID,
};

const Op kMain[64] = {
    O::INVALID, O::INVALID, O::J,      O::JAL,    O::BEQ,   O::BNE,   O::BLEZ,  O::BGTZ,
    O::ADDI,    O::ADDIU,   O::SLTI,   O::SLTIU,  O::ANDI,  O::ORI,   O::XORI,  O::LUI,
    O::INVALID, O::INVALID, O::INVALID, O::INVALID, O::BEQL, O::BNEL, O::BLEZL, O::BGTZL,
    O::DADDI,   O::DADDIU,  O::LDL,    O::LDR,    O::INVALID, O::INVALID, O::LQ, O::SQ,
    O::LB,      O::LH,      O::LWL,    O::LW,     O::LBU,   O::LHU,   O::LWR,   O::LWU,
    O::SB,      O::SH,      O::SWL,    O::SW,     O::SDL,   O::SDR,   O::SWR,   O::CACHE,
    O::INVALID, O::LWC1,    O::INVALID, O::PREF,  O::INVALID, O::INVALID, O::LQC2, O::LD,
    O::INVALID, O::SWC1,    O::INVALID, O::INVALID, O::INVALID, O::INVALID, O::SQC2, O::SD,
};

const Op kMmi[64] = {
    O::MADD,  O::MADDU,  O::INVALID, O::INVALID, O::PLZCW,  O::INVALID, O::INVALID, O::INVALID,
    O::INVALID /*MMI0*/, O::INVALID /*MMI2*/, O::INVALID, O::INVALID, O::INVALID, O::INVALID, O::INVALID, O::INVALID,
    O::MFHI1, O::MTHI1,  O::MFLO1,   O::MTLO1,   O::INVALID, O::INVALID, O::INVALID, O::INVALID,
    O::MULT1, O::MULTU1, O::DIV1,    O::DIVU1,   O::INVALID, O::INVALID, O::INVALID, O::INVALID,
    O::MADD1, O::MADDU1, O::INVALID, O::INVALID, O::INVALID, O::INVALID, O::INVALID, O::INVALID,
    O::INVALID /*MMI1*/, O::INVALID /*MMI3*/, O::INVALID, O::INVALID, O::INVALID, O::INVALID, O::INVALID, O::INVALID,
    O::INVALID /*PMFHL*/, O::INVALID /*PMTHL*/, O::INVALID, O::INVALID, O::PSLLH, O::INVALID, O::PSRLH, O::PSRAH,
    O::INVALID, O::INVALID, O::INVALID, O::INVALID, O::PSLLW, O::INVALID, O::PSRLW, O::PSRAW,
};

const Op kMmi0[32] = {
    O::PADDW,  O::PSUBW,  O::PCGTW,  O::PMAXW,  O::PADDH,  O::PSUBH,  O::PCGTH,  O::PMAXH,
    O::PADDB,  O::PSUBB,  O::PCGTB,  O::INVALID, O::INVALID, O::INVALID, O::INVALID, O::INVALID,
    O::PADDSW, O::PSUBSW, O::PEXTLW, O::PPACW,  O::PADDSH, O::PSUBSH, O::PEXTLH, O::PPACH,
    O::PADDSB, O::PSUBSB, O::PEXTLB, O::PPACB,  O::INVALID, O::INVALID, O::PEXT5, O::PPAC5,
};
const Op kMmi1[32] = {
    O::INVALID, O::PABSW, O::PCEQW,  O::PMINW,  O::PADSBH, O::PABSH,  O::PCEQH,  O::PMINH,
    O::INVALID, O::INVALID, O::PCEQB, O::INVALID, O::INVALID, O::INVALID, O::INVALID, O::INVALID,
    O::PADDUW, O::PSUBUW, O::PEXTUW, O::INVALID, O::PADDUH, O::PSUBUH, O::PEXTUH, O::INVALID,
    O::PADDUB, O::PSUBUB, O::PEXTUB, O::QFSRV,  O::INVALID, O::INVALID, O::INVALID, O::INVALID,
};
const Op kMmi2[32] = {
    O::PMADDW, O::INVALID, O::PSLLVW, O::PSRLVW, O::PMSUBW, O::INVALID, O::INVALID, O::INVALID,
    O::PMFHI,  O::PMFLO,   O::PINTH,  O::INVALID, O::PMULTW, O::PDIVW,  O::PCPYLD, O::INVALID,
    O::PMADDH, O::PHMADH,  O::PAND,   O::PXOR,   O::PMSUBH, O::PHMSBH, O::INVALID, O::INVALID,
    O::INVALID, O::INVALID, O::PEXEH, O::PREVH,  O::PMULTH, O::PDIVBW, O::PEXEW,  O::PROT3W,
};
const Op kMmi3[32] = {
    O::PMADDUW, O::INVALID, O::INVALID, O::PSRAVW, O::INVALID, O::INVALID, O::INVALID, O::INVALID,
    O::PMTHI,   O::PMTLO,   O::PINTEH,  O::INVALID, O::PMULTUW, O::PDIVUW, O::PCPYUD, O::INVALID,
    O::INVALID, O::INVALID, O::POR,     O::PNOR,   O::INVALID, O::INVALID, O::INVALID, O::INVALID,
    O::INVALID, O::INVALID, O::PEXCH,   O::PCPYH,  O::INVALID, O::INVALID, O::PEXCW,  O::INVALID,
};

Op decodeCop1S(uint32_t f) {
    switch (f) {
    case 0: return O::ADD_S; case 1: return O::SUB_S; case 2: return O::MUL_S; case 3: return O::DIV_S;
    case 4: return O::SQRT_S; case 5: return O::ABS_S; case 6: return O::MOV_S; case 7: return O::NEG_S;
    case 22: return O::RSQRT_S; case 24: return O::ADDA_S; case 25: return O::SUBA_S; case 26: return O::MULA_S;
    case 28: return O::MADD_S; case 29: return O::MSUB_S; case 30: return O::MADDA_S; case 31: return O::MSUBA_S;
    case 36: return O::CVT_W_S; case 40: return O::MAX_S; case 41: return O::MIN_S;
    case 48: return O::C_F_S; case 50: return O::C_EQ_S; case 52: return O::C_LT_S; case 54: return O::C_LE_S;
    default: return O::INVALID;
    }
}

Op decodeVuSpecial1(uint32_t f) {
    if (f < 4) return O::VADDbc;
    if (f < 8) return O::VSUBbc;
    if (f < 12) return O::VMADDbc;
    if (f < 16) return O::VMSUBbc;
    if (f < 20) return O::VMAXbc;
    if (f < 24) return O::VMINIbc;
    if (f < 28) return O::VMULbc;
    switch (f) {
    case 28: return O::VMULq; case 29: return O::VMAXi; case 30: return O::VMULi; case 31: return O::VMINIi;
    case 32: return O::VADDq; case 33: return O::VMADDq; case 34: return O::VADDi; case 35: return O::VMADDi;
    case 36: return O::VSUBq; case 37: return O::VMSUBq; case 38: return O::VSUBi; case 39: return O::VMSUBi;
    case 40: return O::VADD; case 41: return O::VMADD; case 42: return O::VMUL; case 43: return O::VMAX;
    case 44: return O::VSUB; case 45: return O::VMSUB; case 46: return O::VOPMSUB; case 47: return O::VMINI;
    case 48: return O::VIADD; case 49: return O::VISUB; case 50: return O::VIADDI;
    case 52: return O::VIAND; case 53: return O::VIOR;
    case 56: return O::VCALLMS; case 57: return O::VCALLMSR;
    default: return O::INVALID;
    }
}

Op decodeVuSpecial2(uint32_t idx) {
    if (idx < 4) return O::VADDAbc;
    if (idx < 8) return O::VSUBAbc;
    if (idx < 12) return O::VMADDAbc;
    if (idx < 16) return O::VMSUBAbc;
    switch (idx) {
    case 16: return O::VITOF0; case 17: return O::VITOF4; case 18: return O::VITOF12; case 19: return O::VITOF15;
    case 20: return O::VFTOI0; case 21: return O::VFTOI4; case 22: return O::VFTOI12; case 23: return O::VFTOI15;
    case 24: case 25: case 26: case 27: return O::VMULAbc;
    case 28: return O::VMULAq; case 29: return O::VABS; case 30: return O::VMULAi; case 31: return O::VCLIPw;
    case 32: return O::VADDAq; case 33: return O::VMADDAq; case 34: return O::VADDAi; case 35: return O::VMADDAi;
    case 36: return O::VSUBAq; case 37: return O::VMSUBAq; case 38: return O::VSUBAi; case 39: return O::VMSUBAi;
    case 40: return O::VADDA; case 41: return O::VMADDA; case 42: return O::VMULA;
    case 44: return O::VSUBA; case 45: return O::VMSUBA; case 46: return O::VOPMULA; case 47: return O::VNOP;
    case 48: return O::VMOVE; case 49: return O::VMR32;
    case 52: return O::VLQI; case 53: return O::VSQI; case 54: return O::VLQD; case 55: return O::VSQD;
    case 56: return O::VDIV; case 57: return O::VSQRT; case 58: return O::VRSQRT; case 59: return O::VWAITQ;
    case 60: return O::VMTIR; case 61: return O::VMFIR; case 62: return O::VILWR; case 63: return O::VISWR;
    case 64: return O::VRNEXT; case 65: return O::VRGET; case 66: return O::VRINIT; case 67: return O::VRXOR;
    default: return O::INVALID;
    }
}

const char* kGprNames[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

} // namespace

const char* opName(Op op) { return kOpInfo[(size_t)op].name; }
Kind opKind(Op op) { return kOpInfo[(size_t)op].kind; }

Insn decode(uint32_t raw, uint32_t addr) {
    Insn in;
    in.raw = raw;
    in.addr = addr;
    const uint32_t opc = raw >> 26;
    const uint32_t funct = raw & 0x3F;
    const uint32_t rs = (raw >> 21) & 31;
    const uint32_t rt = (raw >> 16) & 31;
    const uint32_t sa = (raw >> 6) & 31;
    switch (opc) {
    case 0:
        in.op = kSpecial[funct];
        break;
    case 1:
        in.op = kRegimm[rt];
        break;
    case 16: // COP0
        if (rs == 0) in.op = O::MFC0;
        else if (rs == 4) in.op = O::MTC0;
        else if (rs == 8) {
            static const Op bc[4] = {O::BC0F, O::BC0T, O::BC0FL, O::BC0TL};
            in.op = rt < 4 ? bc[rt] : O::INVALID;
        } else if (rs == 16) {
            switch (funct) {
            case 1: in.op = O::TLBR; break;
            case 2: in.op = O::TLBWI; break;
            case 6: in.op = O::TLBWR; break;
            case 8: in.op = O::TLBP; break;
            case 24: in.op = O::ERET; break;
            case 56: in.op = O::EI; break;
            case 57: in.op = O::DI; break;
            default: in.op = O::INVALID;
            }
        } else in.op = O::INVALID;
        break;
    case 17: // COP1
        switch (rs) {
        case 0: in.op = O::MFC1; break;
        case 2: in.op = O::CFC1; break;
        case 4: in.op = O::MTC1; break;
        case 6: in.op = O::CTC1; break;
        case 8: {
            static const Op bc[4] = {O::BC1F, O::BC1T, O::BC1FL, O::BC1TL};
            in.op = rt < 4 ? bc[rt] : O::INVALID;
            break;
        }
        case 16: in.op = decodeCop1S(funct); break;
        case 20: in.op = funct == 32 ? O::CVT_S_W : O::INVALID; break;
        default: in.op = O::INVALID;
        }
        break;
    case 18: // COP2
        if (rs & 16) {
            if (funct < 60) in.op = decodeVuSpecial1(funct);
            else in.op = decodeVuSpecial2((sa << 2) | (funct & 3));
        } else {
            switch (rs) {
            case 1: in.op = O::QMFC2; break;
            case 2: in.op = O::CFC2; break;
            case 5: in.op = O::QMTC2; break;
            case 6: in.op = O::CTC2; break;
            case 8: {
                static const Op bc[4] = {O::BC2F, O::BC2T, O::BC2FL, O::BC2TL};
                in.op = rt < 4 ? bc[rt] : O::INVALID;
                break;
            }
            default: in.op = O::INVALID;
            }
        }
        break;
    case 28: // MMI
        switch (funct) {
        case 8: in.op = kMmi0[sa]; break;
        case 9: in.op = kMmi2[sa]; break;
        case 40: in.op = kMmi1[sa]; break;
        case 41: in.op = kMmi3[sa]; break;
        case 48: {
            static const Op t[5] = {O::PMFHL_LW, O::PMFHL_UW, O::PMFHL_SLW, O::PMFHL_LH, O::PMFHL_SH};
            in.op = sa < 5 ? t[sa] : O::INVALID;
            break;
        }
        case 49: in.op = sa == 0 ? O::PMTHL_LW : O::INVALID; break;
        default: in.op = kMmi[funct];
        }
        break;
    default:
        in.op = kMain[opc];
    }
    return in;
}

std::string disassemble(const Insn& in) {
    char buf[128];
    const char* n = opName(in.op);
    const uint32_t raw = in.raw;
    auto g = [](uint32_t r) { return kGprNames[r & 31]; };
    switch (in.kind()) {
    case Kind::Jump:
    case Kind::JumpLink:
        snprintf(buf, sizeof buf, "%s 0x%08x", n, in.jumpTarget());
        return buf;
    case Kind::JumpReg:
        snprintf(buf, sizeof buf, "%s $%s", n, g(in.rs()));
        return buf;
    case Kind::JumpLinkReg:
        snprintf(buf, sizeof buf, "%s $%s, $%s", n, g(in.rd()), g(in.rs()));
        return buf;
    case Kind::Branch:
    case Kind::BranchLikely:
    case Kind::BranchLink:
    case Kind::BranchLinkLikely: {
        uint32_t opc = raw >> 26;
        if (opc == 4 || opc == 5 || opc == 20 || opc == 21)
            snprintf(buf, sizeof buf, "%s $%s, $%s, 0x%08x", n, g(in.rs()), g(in.rt()), in.branchTarget());
        else if (opc >= 16 && opc <= 18)
            snprintf(buf, sizeof buf, "%s 0x%08x", n, in.branchTarget());
        else
            snprintf(buf, sizeof buf, "%s $%s, 0x%08x", n, g(in.rs()), in.branchTarget());
        return buf;
    }
    default: break;
    }
    const uint32_t opc = raw >> 26;
    if (raw == 0) return "nop";
    if (opc == 0 || opc == 28) {
        snprintf(buf, sizeof buf, "%s $%s, $%s, $%s (sa=%u)", n, g(in.rd()), g(in.rs()), g(in.rt()), in.sa());
    } else if (opc >= 32 || opc == 26 || opc == 27 || opc == 30 || opc == 31) {
        snprintf(buf, sizeof buf, "%s $%u, %d($%s)", n, in.rt(), in.simm(), g(in.rs()));
    } else if (opc >= 8 && opc <= 15) {
        snprintf(buf, sizeof buf, "%s $%s, $%s, 0x%x", n, g(in.rt()), g(in.rs()), in.uimm());
    } else if (opc == 24 || opc == 25) {
        snprintf(buf, sizeof buf, "%s $%s, $%s, %d", n, g(in.rt()), g(in.rs()), in.simm());
    } else {
        snprintf(buf, sizeof buf, "%s 0x%08x", n, raw);
    }
    return buf;
}

} // namespace recomp
