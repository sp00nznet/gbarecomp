#include "gba/arm.h"

/* ---- Bit extraction helpers ---- */

static inline u32 bits(u32 v, int hi, int lo) {
    return (v >> lo) & ((1u << (hi - lo + 1)) - 1);
}

static inline u32 bit(u32 v, int n) {
    return (v >> n) & 1;
}

/* Sign-extend a value of `width` bits to 32 bits. */
static inline s32 sign_extend(u32 val, int width) {
    u32 mask = 1u << (width - 1);
    return (s32)((val ^ mask) - mask);
}

/* Rotate right a 32-bit value. */
static inline u32 ror32(u32 val, int amount) {
    if (amount == 0) return val;
    amount &= 31;
    return (val >> amount) | (val << (32 - amount));
}

/* ---- Decode helpers ---- */

static void decode_operand2_imm(ArmInsn *out, u32 insn) {
    out->i = true;
    out->rotate = (u8)bits(insn, 11, 8);
    u32 imm8 = bits(insn, 7, 0);
    out->imm = ror32(imm8, out->rotate * 2);
}

static void decode_operand2_reg(ArmInsn *out, u32 insn) {
    out->i = false;
    out->rm = (u8)bits(insn, 3, 0);
    out->shift_type = (ShiftType)bits(insn, 6, 5);

    if (bit(insn, 4)) {
        /* Shift by register */
        out->shift_reg = true;
        out->rs = (u8)bits(insn, 11, 8);
        out->shift_amount = 0;
    } else {
        /* Shift by immediate */
        out->shift_reg = false;
        out->shift_amount = (u8)bits(insn, 11, 7);
    }
}

static void decode_data_processing(ArmInsn *out, u32 insn) {
    u32 opcode = bits(insn, 24, 21);
    static const ArmInsnType dp_types[16] = {
        ARM_AND, ARM_EOR, ARM_SUB, ARM_RSB,
        ARM_ADD, ARM_ADC, ARM_SBC, ARM_RSC,
        ARM_TST, ARM_TEQ, ARM_CMP, ARM_CMN,
        ARM_ORR, ARM_MOV, ARM_BIC, ARM_MVN,
    };

    out->type = dp_types[opcode];
    out->s = bit(insn, 20) != 0;
    out->rn = (u8)bits(insn, 19, 16);
    out->rd = (u8)bits(insn, 15, 12);

    if (bit(insn, 25)) {
        decode_operand2_imm(out, insn);
    } else {
        decode_operand2_reg(out, insn);
    }
}

static void decode_multiply(ArmInsn *out, u32 insn) {
    bool accumulate = bit(insn, 21) != 0;
    out->type = accumulate ? ARM_MLA : ARM_MUL;
    out->s = bit(insn, 20) != 0;
    out->rd = (u8)bits(insn, 19, 16);
    out->rn = (u8)bits(insn, 15, 12);  /* Accumulate register (MLA) */
    out->rs = (u8)bits(insn, 11, 8);
    out->rm = (u8)bits(insn, 3, 0);
}

static void decode_multiply_long(ArmInsn *out, u32 insn) {
    bool is_signed = bit(insn, 22) != 0;
    bool accumulate = bit(insn, 21) != 0;

    if (is_signed) {
        out->type = accumulate ? ARM_SMLAL : ARM_SMULL;
    } else {
        out->type = accumulate ? ARM_UMLAL : ARM_UMULL;
    }

    out->s = bit(insn, 20) != 0;
    out->rn = (u8)bits(insn, 19, 16);  /* RdHi */
    out->rd = (u8)bits(insn, 15, 12);  /* RdLo */
    out->rs = (u8)bits(insn, 11, 8);
    out->rm = (u8)bits(insn, 3, 0);
}

static void decode_swap(ArmInsn *out, u32 insn) {
    out->type = bit(insn, 22) ? ARM_SWPB : ARM_SWP;
    out->b = bit(insn, 22) != 0;
    out->rn = (u8)bits(insn, 19, 16);
    out->rd = (u8)bits(insn, 15, 12);
    out->rm = (u8)bits(insn, 3, 0);
}

static void decode_bx(ArmInsn *out, u32 insn) {
    out->type = ARM_BX;
    out->rm = (u8)bits(insn, 3, 0);
}

static void decode_halfword_transfer(ArmInsn *out, u32 insn) {
    out->p = bit(insn, 24) != 0;
    out->u = bit(insn, 23) != 0;
    out->w = bit(insn, 21) != 0;
    out->rn = (u8)bits(insn, 19, 16);
    out->rd = (u8)bits(insn, 15, 12);

    bool is_load = bit(insn, 20) != 0;
    u32 sh = bits(insn, 6, 5);

    /* bit 22: immediate offset (1) or register offset (0) */
    if (bit(insn, 22)) {
        out->i = true;
        u32 hi = bits(insn, 11, 8);
        u32 lo = bits(insn, 3, 0);
        out->imm = (hi << 4) | lo;
    } else {
        out->i = false;
        out->rm = (u8)bits(insn, 3, 0);
    }

    if (is_load) {
        switch (sh) {
            case 1: out->type = ARM_LDRH;  break; /* Unsigned halfword */
            case 2: out->type = ARM_LDRSB; break; /* Signed byte */
            case 3: out->type = ARM_LDRSH; break; /* Signed halfword */
            default: out->type = ARM_UNDEFINED; break;
        }
    } else {
        /* Store: only SH=01 (STRH) is valid */
        if (sh == 1) {
            out->type = ARM_STRH;
        } else {
            out->type = ARM_UNDEFINED;
        }
    }
}

static void decode_mrs(ArmInsn *out, u32 insn) {
    out->type = ARM_MRS;
    out->r = bit(insn, 22) != 0;
    out->rd = (u8)bits(insn, 15, 12);
}

static void decode_msr(ArmInsn *out, u32 insn) {
    out->type = ARM_MSR;
    out->r = bit(insn, 22) != 0;
    out->msr_mask = (u8)bits(insn, 19, 16);

    if (bit(insn, 25)) {
        /* Immediate form */
        out->i = true;
        out->rotate = (u8)bits(insn, 11, 8);
        u32 imm8 = bits(insn, 7, 0);
        out->imm = ror32(imm8, out->rotate * 2);
    } else {
        /* Register form */
        out->i = false;
        out->rm = (u8)bits(insn, 3, 0);
    }
}

static void decode_single_transfer(ArmInsn *out, u32 insn) {
    out->type = bit(insn, 20) ? ARM_LDR : ARM_STR;
    out->l = bit(insn, 20) != 0;
    /*
     * Note: For single data transfer, bit 25 = 1 means REGISTER offset
     * (opposite of data processing where bit 25 = 1 means immediate).
     */
    out->i = bit(insn, 25) != 0;
    out->p = bit(insn, 24) != 0;
    out->u = bit(insn, 23) != 0;
    out->b = bit(insn, 22) != 0;
    out->w = bit(insn, 21) != 0;
    out->rn = (u8)bits(insn, 19, 16);
    out->rd = (u8)bits(insn, 15, 12);

    if (!out->i) {
        /* 12-bit immediate offset */
        out->imm = bits(insn, 11, 0);
    } else {
        /* Shifted register offset */
        out->rm = (u8)bits(insn, 3, 0);
        out->shift_type = (ShiftType)bits(insn, 6, 5);
        out->shift_amount = (u8)bits(insn, 11, 7);
        out->shift_reg = false; /* Always immediate shift for transfers */
    }
}

static void decode_block_transfer(ArmInsn *out, u32 insn) {
    out->type = bit(insn, 20) ? ARM_LDM : ARM_STM;
    out->l = bit(insn, 20) != 0;
    out->p = bit(insn, 24) != 0;
    out->u = bit(insn, 23) != 0;
    out->s = bit(insn, 22) != 0; /* S bit: PSR / force user mode */
    out->w = bit(insn, 21) != 0;
    out->rn = (u8)bits(insn, 19, 16);
    out->reg_list = (u16)bits(insn, 15, 0);
}

static void decode_branch(ArmInsn *out, u32 insn) {
    out->type = bit(insn, 24) ? ARM_BL : ARM_B;
    out->l = bit(insn, 24) != 0;
    /* 24-bit offset, sign-extended and shifted left by 2.
     * The caller must add PC+8 to compute the target address. */
    s32 offset = sign_extend(bits(insn, 23, 0), 24);
    out->branch_offset = offset << 2;
}

static void decode_swi(ArmInsn *out, u32 insn) {
    out->type = ARM_SWI;
    out->swi_number = bits(insn, 23, 0);
}

/* ---- Category 00 disambiguation ---- */

static void decode_cat00(ArmInsn *out, u32 insn) {
    u32 bits_7_4 = bits(insn, 7, 4);

    /*
     * BX: 0001 0010 1111 1111 1111 0001 Rn
     * Check bits[27:4] == 0x012FFF1
     */
    if ((insn & 0x0FFFFFF0) == 0x012FFF10) {
        decode_bx(out, insn);
        return;
    }

    /*
     * MRS: cond 0001 0R00 1111 Rd 0000 0000 0000
     * bits[27:23]=00010, bit[21]=0, bit[20]=0, Rn=1111, bits[11:0]=0
     */
    if ((insn & 0x0FBF0FFF) == 0x010F0000) {
        decode_mrs(out, insn);
        return;
    }

    /*
     * MSR register form: cond 0001 0R10 mask 1111 0000 0000 Rm
     * bits[27:23]=00010, bit[21]=1, bit[20]=0, Rd=1111, bits[11:4]=00000000
     */
    if ((insn & 0x0FB0FFF0) == 0x0120F000) {
        decode_msr(out, insn);
        return;
    }

    /*
     * MSR immediate form: cond 0011 0R10 mask 1111 rotate imm8
     * This is in category 00 with bit 25 = 1... actually this is category 00
     * bit 25=1, so bits[27:25]=001 which is still caught by bits[27:26]=00.
     */
    if ((insn & 0x0FB0F000) == 0x0320F000) {
        decode_msr(out, insn);
        return;
    }

    /*
     * Multiply and multiply-accumulate:
     * bits[27:22] = 000000, bits[7:4] = 1001
     */
    if (bits_7_4 == 0x9 && bits(insn, 27, 22) == 0x00) {
        decode_multiply(out, insn);
        return;
    }

    /*
     * Multiply long:
     * bits[27:23] = 00001, bits[7:4] = 1001
     */
    if (bits_7_4 == 0x9 && bits(insn, 27, 23) == 0x01) {
        decode_multiply_long(out, insn);
        return;
    }

    /*
     * SWP/SWPB:
     * bits[27:23] = 00010, bit[21:20] = 00, bits[11:4] = 00001001
     */
    if (bits_7_4 == 0x9 && bits(insn, 27, 23) == 0x02 &&
        bits(insn, 21, 20) == 0x0 && bits(insn, 11, 8) == 0x0) {
        decode_swap(out, insn);
        return;
    }

    /*
     * Halfword / signed byte transfers:
     * bits[7] = 1, bits[4] = 1, and not a multiply (bits[7:4] != 1001)
     * Pattern: bits[7:4] = 1xx1 where xx != 00 (xx=00 is multiply)
     */
    if (bit(insn, 7) && bit(insn, 4) && bits(insn, 6, 5) != 0) {
        decode_halfword_transfer(out, insn);
        return;
    }

    /* Data processing */
    decode_data_processing(out, insn);
}

/* ---- Main decoder ---- */

ArmInsn arm_decode(u32 insn) {
    ArmInsn out = {0};
    out.raw = insn;
    out.cond = (ArmCondition)bits(insn, 31, 28);
    out.type = ARM_UNDEFINED;

    u32 cat = bits(insn, 27, 26);

    switch (cat) {
    case 0x0:
        /*
         * Category 00 covers data processing, multiplies, swaps, BX,
         * halfword transfers, MRS/MSR.
         * Also bit 25 can be set for data processing immediates and
         * MSR immediate, which makes bits[27:26]=00 still (bit 25 is
         * within the category).
         */
        decode_cat00(&out, insn);
        break;

    case 0x1:
        /*
         * Category 01: single data transfer.
         * But undefined if bits[25]=1 and bit[4]=1 (register offset with
         * register-specified shift is not valid in SDT... actually it is
         * undefined instruction). ARM7TDMI treats this as undefined.
         */
        if (bit(insn, 25) && bit(insn, 4)) {
            out.type = ARM_UNDEFINED;
        } else {
            decode_single_transfer(&out, insn);
        }
        break;

    case 0x2:
        if (bit(insn, 25)) {
            /* 101: Branch / Branch with link */
            decode_branch(&out, insn);
        } else {
            /* 100: Block data transfer */
            decode_block_transfer(&out, insn);
        }
        break;

    case 0x3:
        if (bits(insn, 27, 24) == 0xF) {
            /* 1111: SWI */
            decode_swi(&out, insn);
        } else {
            /* Coprocessor or undefined on ARM7TDMI */
            out.type = ARM_UNDEFINED;
        }
        break;
    }

    return out;
}

/* ---- Name functions ---- */

const char* arm_insn_type_name(ArmInsnType type) {
    static const char* names[ARM_INSN_TYPE_COUNT] = {
        [ARM_AND]       = "AND",
        [ARM_EOR]       = "EOR",
        [ARM_SUB]       = "SUB",
        [ARM_RSB]       = "RSB",
        [ARM_ADD]       = "ADD",
        [ARM_ADC]       = "ADC",
        [ARM_SBC]       = "SBC",
        [ARM_RSC]       = "RSC",
        [ARM_TST]       = "TST",
        [ARM_TEQ]       = "TEQ",
        [ARM_CMP]       = "CMP",
        [ARM_CMN]       = "CMN",
        [ARM_ORR]       = "ORR",
        [ARM_MOV]       = "MOV",
        [ARM_BIC]       = "BIC",
        [ARM_MVN]       = "MVN",
        [ARM_MUL]       = "MUL",
        [ARM_MLA]       = "MLA",
        [ARM_UMULL]     = "UMULL",
        [ARM_UMLAL]     = "UMLAL",
        [ARM_SMULL]     = "SMULL",
        [ARM_SMLAL]     = "SMLAL",
        [ARM_B]         = "B",
        [ARM_BL]        = "BL",
        [ARM_BX]        = "BX",
        [ARM_LDR]       = "LDR",
        [ARM_STR]       = "STR",
        [ARM_LDRH]      = "LDRH",
        [ARM_STRH]      = "STRH",
        [ARM_LDRSB]     = "LDRSB",
        [ARM_LDRSH]     = "LDRSH",
        [ARM_LDM]       = "LDM",
        [ARM_STM]       = "STM",
        [ARM_SWP]       = "SWP",
        [ARM_SWPB]      = "SWPB",
        [ARM_MRS]       = "MRS",
        [ARM_MSR]       = "MSR",
        [ARM_SWI]       = "SWI",
        [ARM_UNDEFINED] = "UNDEFINED",
    };

    if (type >= ARM_INSN_TYPE_COUNT) return "UNKNOWN";
    return names[type] ? names[type] : "UNKNOWN";
}

const char* arm_cond_name(ArmCondition cond) {
    static const char* names[16] = {
        [COND_EQ] = "EQ", [COND_NE] = "NE", [COND_CS] = "CS", [COND_CC] = "CC",
        [COND_MI] = "MI", [COND_PL] = "PL", [COND_VS] = "VS", [COND_VC] = "VC",
        [COND_HI] = "HI", [COND_LS] = "LS", [COND_GE] = "GE", [COND_LT] = "LT",
        [COND_GT] = "GT", [COND_LE] = "LE", [COND_AL] = "",   [COND_NV] = "NV",
    };
    if ((unsigned)cond > 15) return "??";
    return names[cond];
}

const char* arm_reg_name(u8 reg) {
    static const char* names[16] = {
        "r0", "r1", "r2",  "r3",  "r4",  "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc",
    };
    if (reg > 15) return "r?";
    return names[reg];
}

const char* arm_shift_name(ShiftType type) {
    static const char* names[4] = {
        [SHIFT_LSL] = "LSL",
        [SHIFT_LSR] = "LSR",
        [SHIFT_ASR] = "ASR",
        [SHIFT_ROR] = "ROR",
    };
    if ((unsigned)type > 3) return "???";
    return names[type];
}
