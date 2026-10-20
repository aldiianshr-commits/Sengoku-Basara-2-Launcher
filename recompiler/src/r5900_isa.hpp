// R5900 (PS2 Emotion Engine) instruction set description and decoder.
//
// Every instruction the recompiler understands is listed once in the
// X-macro tables below.  The decoder turns a raw 32-bit word into an Op;
// the code generator emits `NAME(c, 0xRAWWORD);` and the runtime header
// ps2/recomp_ops.hpp implements NAME.  Control-flow instructions get
// special treatment in the code generator.
#pragma once
#include <cstdint>
#include <string>

namespace recomp {

// Instruction classes used by analysis / codegen.
enum class Kind : uint8_t {
    Normal,       // straight-line, emitted as NAME(c, raw)
    Branch,       // conditional PC-relative branch with delay slot
    BranchLikely, // as Branch, delay slot nullified when not taken
    BranchLink,   // conditional branch that writes $ra (bltzal...)
    BranchLinkLikely,
    Jump,         // j
    JumpLink,     // jal
    JumpReg,      // jr
    JumpLinkReg,  // jalr
    Syscall,
    Break,
    Eret,
    Invalid,
};

#define R5900_OPS(X) \
    /* name        kind */ \
    X(SLL, Normal) X(SRL, Normal) X(SRA, Normal) X(SLLV, Normal) X(SRLV, Normal) X(SRAV, Normal) \
    X(JR, JumpReg) X(JALR, JumpLinkReg) X(MOVZ, Normal) X(MOVN, Normal) \
    X(SYSCALL, Syscall) X(BREAK, Break) X(SYNC, Normal) \
    X(MFHI, Normal) X(MTHI, Normal) X(MFLO, Normal) X(MTLO, Normal) \
    X(DSLLV, Normal) X(DSRLV, Normal) X(DSRAV, Normal) \
    X(MULT, Normal) X(MULTU, Normal) X(DIV, Normal) X(DIVU, Normal) \
    X(DMULT, Normal) X(DMULTU, Normal) X(DDIV, Normal) X(DDIVU, Normal) \
    X(ADD, Normal) X(ADDU, Normal) X(SUB, Normal) X(SUBU, Normal) \
    X(AND, Normal) X(OR, Normal) X(XOR, Normal) X(NOR, Normal) \
    X(MFSA, Normal) X(MTSA, Normal) X(SLT, Normal) X(SLTU, Normal) \
    X(DADD, Normal) X(DADDU, Normal) X(DSUB, Normal) X(DSUBU, Normal) \
    X(TGE, Normal) X(TGEU, Normal) X(TLT, Normal) X(TLTU, Normal) X(TEQ, Normal) X(TNE, Normal) \
    X(DSLL, Normal) X(DSRL, Normal) X(DSRA, Normal) X(DSLL32, Normal) X(DSRL32, Normal) X(DSRA32, Normal) \
    /* REGIMM */ \
    X(BLTZ, Branch) X(BGEZ, Branch) X(BLTZL, BranchLikely) X(BGEZL, BranchLikely) \
    X(TGEI, Normal) X(TGEIU, Normal) X(TLTI, Normal) X(TLTIU, Normal) X(TEQI, Normal) X(TNEI, Normal) \
    X(BLTZAL, BranchLink) X(BGEZAL, BranchLink) X(BLTZALL, BranchLinkLikely) X(BGEZALL, BranchLinkLikely) \
    X(MTSAB, Normal) X(MTSAH, Normal) \
    /* main opcodes */ \
    X(J, Jump) X(JAL, JumpLink) X(BEQ, Branch) X(BNE, Branch) X(BLEZ, Branch) X(BGTZ, Branch) \
    X(ADDI, Normal) X(ADDIU, Normal) X(SLTI, Normal) X(SLTIU, Normal) X(ANDI, Normal) X(ORI, Normal) \
    X(XORI, Normal) X(LUI, Normal) \
    X(BEQL, BranchLikely) X(BNEL, BranchLikely) X(BLEZL, BranchLikely) X(BGTZL, BranchLikely) \
    X(DADDI, Normal) X(DADDIU, Normal) X(LDL, Normal) X(LDR, Normal) X(LQ, Normal) X(SQ, Normal) \
    X(LB, Normal) X(LH, Normal) X(LWL, Normal) X(LW, Normal) X(LBU, Normal) X(LHU, Normal) X(LWR, Normal) \
    X(LWU, Normal) X(SB, Normal) X(SH, Normal) X(SWL, Normal) X(SW, Normal) X(SDL, Normal) X(SDR, Normal) \
    X(SWR, Normal) X(CACHE, Normal) X(LWC1, Normal) X(PREF, Normal) X(LQC2, Normal) X(LD, Normal) \
    X(SWC1, Normal) X(SQC2, Normal) X(SD, Normal) \
    /* MMI */ \
    X(MADD, Normal) X(MADDU, Normal) X(PLZCW, Normal) X(MFHI1, Normal) X(MTHI1, Normal) X(MFLO1, Normal) \
    X(MTLO1, Normal) X(MULT1, Normal) X(MULTU1, Normal) X(DIV1, Normal) X(DIVU1, Normal) X(MADD1, Normal) \
    X(MADDU1, Normal) X(PMFHL_LW, Normal) X(PMFHL_UW, Normal) X(PMFHL_SLW, Normal) X(PMFHL_LH, Normal) \
    X(PMFHL_SH, Normal) X(PMTHL_LW, Normal) X(PSLLH, Normal) X(PSRLH, Normal) X(PSRAH, Normal) \
    X(PSLLW, Normal) X(PSRLW, Normal) X(PSRAW, Normal) \
    /* MMI0 */ \
    X(PADDW, Normal) X(PSUBW, Normal) X(PCGTW, Normal) X(PMAXW, Normal) X(PADDH, Normal) X(PSUBH, Normal) \
    X(PCGTH, Normal) X(PMAXH, Normal) X(PADDB, Normal) X(PSUBB, Normal) X(PCGTB, Normal) X(PADDSW, Normal) \
    X(PSUBSW, Normal) X(PEXTLW, Normal) X(PPACW, Normal) X(PADDSH, Normal) X(PSUBSH, Normal) X(PEXTLH, Normal) \
    X(PPACH, Normal) X(PADDSB, Normal) X(PSUBSB, Normal) X(PEXTLB, Normal) X(PPACB, Normal) X(PEXT5, Normal) \
    X(PPAC5, Normal) \
    /* MMI1 */ \
    X(PABSW, Normal) X(PCEQW, Normal) X(PMINW, Normal) X(PADSBH, Normal) X(PABSH, Normal) X(PCEQH, Normal) \
    X(PMINH, Normal) X(PCEQB, Normal) X(PADDUW, Normal) X(PSUBUW, Normal) X(PEXTUW, Normal) X(PADDUH, Normal) \
    X(PSUBUH, Normal) X(PEXTUH, Normal) X(PADDUB, Normal) X(PSUBUB, Normal) X(PEXTUB, Normal) X(QFSRV, Normal) \
    /* MMI2 */ \
    X(PMADDW, Normal) X(PSLLVW, Normal) X(PSRLVW, Normal) X(PMSUBW, Normal) X(PMFHI, Normal) X(PMFLO, Normal) \
    X(PINTH, Normal) X(PMULTW, Normal) X(PDIVW, Normal) X(PCPYLD, Normal) X(PMADDH, Normal) X(PHMADH, Normal) \
    X(PAND, Normal) X(PXOR, Normal) X(PMSUBH, Normal) X(PHMSBH, Normal) X(PEXEH, Normal) X(PREVH, Normal) \
    X(PMULTH, Normal) X(PDIVBW, Normal) X(PEXEW, Normal) X(PROT3W, Normal) \
    /* MMI3 */ \
    X(PMADDUW, Normal) X(PSRAVW, Normal) X(PMTHI, Normal) X(PMTLO, Normal) X(PINTEH, Normal) X(PMULTUW, Normal) \
    X(PDIVUW, Normal) X(PCPYUD, Normal) X(POR, Normal) X(PNOR, Normal) X(PEXCH, Normal) X(PCPYH, Normal) \
    X(PEXCW, Normal) \
    /* COP0 */ \
    X(MFC0, Normal) X(MTC0, Normal) X(BC0F, Branch) X(BC0T, Branch) X(BC0FL, BranchLikely) X(BC0TL, BranchLikely) \
    X(TLBR, Normal) X(TLBWI, Normal) X(TLBWR, Normal) X(TLBP, Normal) X(ERET, Eret) X(EI, Normal) X(DI, Normal) \
    /* COP1 */ \
    X(MFC1, Normal) X(CFC1, Normal) X(MTC1, Normal) X(CTC1, Normal) \
    X(BC1F, Branch) X(BC1T, Branch) X(BC1FL, BranchLikely) X(BC1TL, BranchLikely) \
    X(ADD_S, Normal) X(SUB_S, Normal) X(MUL_S, Normal) X(DIV_S, Normal) X(SQRT_S, Normal) X(ABS_S, Normal) \
    X(MOV_S, Normal) X(NEG_S, Normal) X(RSQRT_S, Normal) X(ADDA_S, Normal) X(SUBA_S, Normal) X(MULA_S, Normal) \
    X(MADD_S, Normal) X(MSUB_S, Normal) X(MADDA_S, Normal) X(MSUBA_S, Normal) X(CVT_W_S, Normal) \
    X(MAX_S, Normal) X(MIN_S, Normal) X(C_F_S, Normal) X(C_EQ_S, Normal) X(C_LT_S, Normal) X(C_LE_S, Normal) \
    X(CVT_S_W, Normal) \
    /* COP2 (VU0 macro mode) */ \
    X(QMFC2, Normal) X(CFC2, Normal) X(QMTC2, Normal) X(CTC2, Normal) \
    X(BC2F, Branch) X(BC2T, Branch) X(BC2FL, BranchLikely) X(BC2TL, BranchLikely) \
    X(VADDbc, Normal) X(VSUBbc, Normal) X(VMADDbc, Normal) X(VMSUBbc, Normal) X(VMAXbc, Normal) \
    X(VMINIbc, Normal) X(VMULbc, Normal) X(VMULq, Normal) X(VMAXi, Normal) X(VMULi, Normal) X(VMINIi, Normal) \
    X(VADDq, Normal) X(VMADDq, Normal) X(VADDi, Normal) X(VMADDi, Normal) X(VSUBq, Normal) X(VMSUBq, Normal) \
    X(VSUBi, Normal) X(VMSUBi, Normal) X(VADD, Normal) X(VMADD, Normal) X(VMUL, Normal) X(VMAX, Normal) \
    X(VSUB, Normal) X(VMSUB, Normal) X(VOPMSUB, Normal) X(VMINI, Normal) X(VIADD, Normal) X(VISUB, Normal) \
    X(VIADDI, Normal) X(VIAND, Normal) X(VIOR, Normal) X(VCALLMS, Normal) X(VCALLMSR, Normal) \
    X(VADDAbc, Normal) X(VSUBAbc, Normal) X(VMADDAbc, Normal) X(VMSUBAbc, Normal) X(VITOF0, Normal) \
    X(VITOF4, Normal) X(VITOF12, Normal) X(VITOF15, Normal) X(VFTOI0, Normal) X(VFTOI4, Normal) \
    X(VFTOI12, Normal) X(VFTOI15, Normal) X(VMULAbc, Normal) X(VMULAq, Normal) X(VABS, Normal) \
    X(VMULAi, Normal) X(VCLIPw, Normal) X(VADDAq, Normal) X(VMADDAq, Normal) X(VADDAi, Normal) \
    X(VMADDAi, Normal) X(VSUBAq, Normal) X(VMSUBAq, Normal) X(VSUBAi, Normal) X(VMSUBAi, Normal) \
    X(VADDA, Normal) X(VMADDA, Normal) X(VMULA, Normal) X(VSUBA, Normal) X(VMSUBA, Normal) X(VOPMULA, Normal) \
    X(VNOP, Normal) X(VMOVE, Normal) X(VMR32, Normal) X(VLQI, Normal) X(VSQI, Normal) X(VLQD, Normal) \
    X(VSQD, Normal) X(VDIV, Normal) X(VSQRT, Normal) X(VRSQRT, Normal) X(VWAITQ, Normal) X(VMTIR, Normal) \
    X(VMFIR, Normal) X(VILWR, Normal) X(VISWR, Normal) X(VRNEXT, Normal) X(VRGET, Normal) X(VRINIT, Normal) \
    X(VRXOR, Normal)

enum class Op : uint16_t {
#define X(name, kind) name,
    R5900_OPS(X)
#undef X
    INVALID,
    COUNT
};

const char* opName(Op op);
Kind opKind(Op op);

struct Insn {
    uint32_t raw = 0;
    uint32_t addr = 0;
    Op op = Op::INVALID;

    uint32_t rs() const { return (raw >> 21) & 31; }
    uint32_t rt() const { return (raw >> 16) & 31; }
    uint32_t rd() const { return (raw >> 11) & 31; }
    uint32_t sa() const { return (raw >> 6) & 31; }
    int32_t simm() const { return (int16_t)(raw & 0xFFFF); }
    uint32_t uimm() const { return raw & 0xFFFF; }
    Kind kind() const { return opKind(op); }

    bool isControl() const {
        Kind k = kind();
        return k == Kind::Branch || k == Kind::BranchLikely || k == Kind::BranchLink ||
               k == Kind::BranchLinkLikely || k == Kind::Jump || k == Kind::JumpLink ||
               k == Kind::JumpReg || k == Kind::JumpLinkReg;
    }
    bool hasDelaySlot() const { return isControl(); }
    bool isConditional() const {
        Kind k = kind();
        return k == Kind::Branch || k == Kind::BranchLikely || k == Kind::BranchLink ||
               k == Kind::BranchLinkLikely;
    }
    bool isLikely() const { return kind() == Kind::BranchLikely || kind() == Kind::BranchLinkLikely; }
    // Static target of a branch / j / jal.
    uint32_t branchTarget() const { return addr + 4 + (uint32_t)(simm() << 2); }
    uint32_t jumpTarget() const { return ((addr + 4) & 0xF0000000u) | ((raw & 0x03FFFFFFu) << 2); }
    bool hasStaticTarget() const {
        return isConditional() || kind() == Kind::Jump || kind() == Kind::JumpLink;
    }
    uint32_t staticTarget() const { return isConditional() ? branchTarget() : jumpTarget(); }
};

Insn decode(uint32_t raw, uint32_t addr);
std::string disassemble(const Insn& in);

} // namespace recomp
