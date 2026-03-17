#include "gba/thumb.h"
#include <string.h>

/* Helper macros for bit extraction. */
#define BITS(v, hi, lo)  (((v) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
#define BIT(v, n)        (((v) >> (n)) & 1u)

static ThumbInsn make_undefined(u16 raw) {
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type = THUMB_UNDEFINED;
    insn.raw  = raw;
    return insn;
}

static ThumbInsn decode_move_shifted(u16 raw) {
    /* Format 1: 000 op(2) imm5 Rs Rd */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type         = THUMB_MOVE_SHIFTED;
    insn.raw          = raw;
    insn.rd           = (ArmRegister)BITS(raw, 2, 0);
    insn.rs           = (ArmRegister)BITS(raw, 5, 3);
    insn.shift_amount = (u8)BITS(raw, 10, 6);
    u32 op = BITS(raw, 12, 11);
    switch (op) {
        case 0: insn.shift_type = SHIFT_LSL; break;
        case 1: insn.shift_type = SHIFT_LSR; break;
        case 2: insn.shift_type = SHIFT_ASR; break;
        default: insn.shift_type = SHIFT_LSL; break; /* shouldn't happen */
    }
    return insn;
}

static ThumbInsn decode_add_sub(u16 raw) {
    /* Format 2: 000 11 I Op Rn/imm3 Rs Rd */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type        = THUMB_ADD_SUB;
    insn.raw         = raw;
    insn.rd          = (ArmRegister)BITS(raw, 2, 0);
    insn.rs          = (ArmRegister)BITS(raw, 5, 3);
    insn.is_subtract = BIT(raw, 9) != 0;
    insn.is_imm      = BIT(raw, 10) != 0;
    if (insn.is_imm) {
        insn.imm = BITS(raw, 8, 6);
    } else {
        insn.rm = (ArmRegister)BITS(raw, 8, 6);
    }
    return insn;
}

static ThumbInsn decode_mov_cmp_add_sub_imm(u16 raw) {
    /* Format 3: 001 op(2) Rd(3) imm8 */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type = THUMB_MOV_CMP_ADD_SUB_IMM;
    insn.raw  = raw;
    insn.rd   = (ArmRegister)BITS(raw, 10, 8);
    insn.imm  = BITS(raw, 7, 0);
    /* op: 00=MOV, 01=CMP, 10=ADD, 11=SUB — encode in alu_op for convenience */
    u32 op = BITS(raw, 12, 11);
    switch (op) {
        case 0: insn.alu_op = THUMB_ALU_AND; break; /* MOV — stored as op 0 */
        case 1: insn.alu_op = THUMB_ALU_CMP; break; /* CMP */
        case 2: insn.alu_op = THUMB_ALU_ADC; break; /* ADD — stored as op 2 */
        case 3: insn.alu_op = THUMB_ALU_SBC; break; /* SUB — stored as op 3 */
        default: break;
    }
    /* Store raw op bits so the consumer can distinguish MOV/CMP/ADD/SUB
       without relying on the alu_op mapping above.  We'll re-use hi_op for this. */
    insn.hi_op = (ThumbHiOp)op;
    return insn;
}

static ThumbInsn decode_alu_ops(u16 raw) {
    /* Format 4: 010000 op(4) Rs Rd */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type   = THUMB_ALU_OPS;
    insn.raw    = raw;
    insn.rd     = (ArmRegister)BITS(raw, 2, 0);
    insn.rs     = (ArmRegister)BITS(raw, 5, 3);
    insn.alu_op = (ThumbAluOp)BITS(raw, 9, 6);
    return insn;
}

static ThumbInsn decode_hi_reg_bx(u16 raw) {
    /* Format 5: 010001 op(2) H1 H2 Rs Rd */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.raw = raw;

    u32 op = BITS(raw, 9, 8);
    u32 h1 = BIT(raw, 7);  /* high bit for Rd */
    u32 h2 = BIT(raw, 6);  /* high bit for Rs */

    insn.rd = (ArmRegister)(BITS(raw, 2, 0) | (h1 << 3));
    insn.rs = (ArmRegister)(BITS(raw, 5, 3) | (h2 << 3));

    if (op == 3) {
        insn.type  = THUMB_BX;
        insn.hi_op = THUMB_HI_BX;
    } else {
        insn.type  = THUMB_HI_REG_OPS;
        insn.hi_op = (ThumbHiOp)op;
    }
    return insn;
}

static ThumbInsn decode_pc_rel_load(u16 raw) {
    /* Format 6: 01001 Rd(3) imm8 */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type    = THUMB_PC_REL_LOAD;
    insn.raw     = raw;
    insn.rd      = (ArmRegister)BITS(raw, 10, 8);
    insn.imm     = BITS(raw, 7, 0) << 2;  /* word-aligned offset */
    insn.is_load = true;
    return insn;
}

static ThumbInsn decode_load_store_reg(u16 raw) {
    /* Format 7: 0101 L B 0 Ro Rb Rd */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type    = THUMB_LOAD_STORE_REG;
    insn.raw     = raw;
    insn.rd      = (ArmRegister)BITS(raw, 2, 0);
    insn.rs      = (ArmRegister)BITS(raw, 5, 3);  /* Rb (base) */
    insn.rm      = (ArmRegister)BITS(raw, 8, 6);  /* Ro (offset) */
    insn.is_load = BIT(raw, 11) != 0;
    insn.is_byte = BIT(raw, 10) != 0;
    return insn;
}

static ThumbInsn decode_load_store_sign(u16 raw) {
    /* Format 8: 0101 H S 1 Ro Rb Rd
       H=bit10, S=bit11
       S=0,H=0 -> STRH; S=0,H=1 -> LDRH; S=1,H=0 -> LDRSB; S=1,H=1 -> LDRSH */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type = THUMB_LOAD_STORE_SIGN;
    insn.raw  = raw;
    insn.rd   = (ArmRegister)BITS(raw, 2, 0);
    insn.rs   = (ArmRegister)BITS(raw, 5, 3);  /* Rb */
    insn.rm   = (ArmRegister)BITS(raw, 8, 6);  /* Ro */

    u32 s = BIT(raw, 11);
    u32 h = BIT(raw, 10);

    if (s == 0 && h == 0) {
        /* STRH */
        insn.is_load   = false;
        insn.is_half   = true;
        insn.is_signed = false;
    } else if (s == 0 && h == 1) {
        /* LDRH */
        insn.is_load   = true;
        insn.is_half   = true;
        insn.is_signed = false;
    } else if (s == 1 && h == 0) {
        /* LDRSB */
        insn.is_load   = true;
        insn.is_byte   = true;
        insn.is_signed = true;
    } else {
        /* LDRSH */
        insn.is_load   = true;
        insn.is_half   = true;
        insn.is_signed = true;
    }
    return insn;
}

static ThumbInsn decode_load_store_imm(u16 raw) {
    /* Format 9: 011 B L imm5 Rb Rd */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type    = THUMB_LOAD_STORE_IMM;
    insn.raw     = raw;
    insn.rd      = (ArmRegister)BITS(raw, 2, 0);
    insn.rs      = (ArmRegister)BITS(raw, 5, 3);  /* Rb (base) */
    insn.is_load = BIT(raw, 11) != 0;
    insn.is_byte = BIT(raw, 12) != 0;
    u32 off5     = BITS(raw, 10, 6);
    /* Byte transfer: offset = imm5; Word transfer: offset = imm5 << 2 */
    insn.imm = insn.is_byte ? off5 : (off5 << 2);
    return insn;
}

static ThumbInsn decode_load_store_half(u16 raw) {
    /* Format 10: 1000 L imm5 Rb Rd */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type    = THUMB_LOAD_STORE_HALF;
    insn.raw     = raw;
    insn.rd      = (ArmRegister)BITS(raw, 2, 0);
    insn.rs      = (ArmRegister)BITS(raw, 5, 3);  /* Rb */
    insn.is_load = BIT(raw, 11) != 0;
    insn.is_half = true;
    insn.imm     = BITS(raw, 10, 6) << 1;  /* halfword-aligned */
    return insn;
}

static ThumbInsn decode_sp_rel_load_store(u16 raw) {
    /* Format 11: 1001 L Rd(3) imm8 */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type    = THUMB_SP_REL_LOAD_STORE;
    insn.raw     = raw;
    insn.rd      = (ArmRegister)BITS(raw, 10, 8);
    insn.rs      = REG_SP;
    insn.is_load = BIT(raw, 11) != 0;
    insn.imm     = BITS(raw, 7, 0) << 2;
    return insn;
}

static ThumbInsn decode_load_addr(u16 raw) {
    /* Format 12: 1010 SP Rd(3) imm8 */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type = THUMB_LOAD_ADDR;
    insn.raw  = raw;
    insn.rd   = (ArmRegister)BITS(raw, 10, 8);
    insn.is_sp = BIT(raw, 11) != 0;
    insn.rs    = insn.is_sp ? REG_SP : REG_PC;
    insn.imm   = BITS(raw, 7, 0) << 2;
    return insn;
}

static ThumbInsn decode_add_sp(u16 raw) {
    /* Format 13: 10110000 S imm7 */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type        = THUMB_ADD_SP;
    insn.raw         = raw;
    insn.rd          = REG_SP;
    insn.rs          = REG_SP;
    insn.is_subtract = BIT(raw, 7) != 0;
    insn.imm         = BITS(raw, 6, 0) << 2;
    return insn;
}

static ThumbInsn decode_push_pop(u16 raw) {
    /* Format 14: 1011 L 10 R rlist(8)
       L=bit11: 0=PUSH(store), 1=POP(load)
       R=bit8: PUSH->store LR, POP->load PC */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type    = THUMB_PUSH_POP;
    insn.raw     = raw;
    insn.is_load = BIT(raw, 11) != 0;
    insn.pc_or_lr = BIT(raw, 8) != 0;
    insn.rlist   = (u8)BITS(raw, 7, 0);
    insn.rs      = REG_SP;
    return insn;
}

static ThumbInsn decode_multi_load_store(u16 raw) {
    /* Format 15: 1100 L Rb(3) rlist(8) */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type    = THUMB_MULTI_LOAD_STORE;
    insn.raw     = raw;
    insn.rs      = (ArmRegister)BITS(raw, 10, 8);  /* Rb (base) */
    insn.is_load = BIT(raw, 11) != 0;
    insn.rlist   = (u8)BITS(raw, 7, 0);
    return insn;
}

static ThumbInsn decode_cond_branch(u16 raw) {
    /* Format 16: 1101 cond(4) soffset8 */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type   = THUMB_COND_BRANCH;
    insn.raw    = raw;
    insn.cond   = (ArmCondition)BITS(raw, 11, 8);
    /* Sign-extend 8-bit offset and shift left by 1 */
    s32 off     = (s32)(s8)BITS(raw, 7, 0);
    insn.offset = off << 1;
    return insn;
}

static ThumbInsn decode_swi(u16 raw) {
    /* Format 17: 11011111 imm8 */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type    = THUMB_SWI;
    insn.raw     = raw;
    insn.swi_num = (u8)BITS(raw, 7, 0);
    insn.imm     = BITS(raw, 7, 0);
    return insn;
}

static ThumbInsn decode_branch(u16 raw) {
    /* Format 18: 11100 soffset11 */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type = THUMB_BRANCH;
    insn.raw  = raw;
    /* Sign-extend 11-bit offset, shift left by 1 */
    s32 off   = (s32)BITS(raw, 10, 0);
    if (off & (1 << 10)) {
        off |= ~((s32)0x7FF);  /* sign extend */
    }
    insn.offset = off << 1;
    return insn;
}

static ThumbInsn decode_long_branch(u16 raw) {
    /* Format 19: prefix (11110) or suffix (11111)
       Prefix: offset[22:12] in bits 10:0
       Suffix: offset[11:1]  in bits 10:0 */
    ThumbInsn insn;
    memset(&insn, 0, sizeof(insn));
    insn.type = THUMB_LONG_BRANCH;
    insn.raw  = raw;

    u32 top5 = BITS(raw, 15, 11);
    if (top5 == 0x1E) {
        /* Prefix: bits 10:0 are high part of offset */
        insn.is_suffix = false;
        s32 off = (s32)BITS(raw, 10, 0);
        if (off & (1 << 10)) {
            off |= ~((s32)0x7FF);
        }
        insn.offset = off << 12;
    } else {
        /* Suffix: bits 10:0 are low part of offset */
        insn.is_suffix = true;
        insn.imm = BITS(raw, 10, 0) << 1;
    }
    return insn;
}

ThumbInsn thumb_decode(u16 raw) {
    u32 top8  = BITS(raw, 15, 8);
    u32 top5  = BITS(raw, 15, 11);
    u32 top3  = BITS(raw, 15, 13);
    u32 top6  = BITS(raw, 15, 10);
    u32 top4  = BITS(raw, 15, 12);

    /* Format 19: Long branch with link */
    if (top5 == 0x1E || top5 == 0x1F) {
        return decode_long_branch(raw);
    }

    /* Format 18: Unconditional branch */
    if (top5 == 0x1C) {
        return decode_branch(raw);
    }

    /* Format 17: SWI */
    if (top8 == 0xDF) {
        return decode_swi(raw);
    }

    /* Format 16: Conditional branch (1101 cond, cond != 1111) */
    if (top4 == 0xD) {
        u32 cond = BITS(raw, 11, 8);
        if (cond == 0xF) {
            return decode_swi(raw); /* 0xDF already handled above; 0xDF = 1101 1111 */
        }
        if (cond == 0xE) {
            /* Undefined in Thumb on ARM7TDMI */
            return make_undefined(raw);
        }
        return decode_cond_branch(raw);
    }

    /* Format 15: Multiple load/store */
    if (top4 == 0xC) {
        return decode_multi_load_store(raw);
    }

    /* Format 14: Push/Pop (1011 x 10 x) — check bits 15:12=1011, bits 10:9=10 */
    if (top4 == 0xB) {
        u32 bits_10_9 = BITS(raw, 10, 9);
        if (bits_10_9 == 2) {
            /* 1011 L 10 R rlist */
            return decode_push_pop(raw);
        }
        /* Format 13: ADD SP (10110000 x xxxxxxx) */
        if (top8 == 0xB0) {
            return decode_add_sp(raw);
        }
        return make_undefined(raw);
    }

    /* Format 12: Load address */
    if (top4 == 0xA) {
        return decode_load_addr(raw);
    }

    /* Format 11: SP-relative load/store */
    if (top4 == 0x9) {
        return decode_sp_rel_load_store(raw);
    }

    /* Format 10: Load/store halfword */
    if (top4 == 0x8) {
        return decode_load_store_half(raw);
    }

    /* Format 9: Load/store with immediate offset */
    if (top3 == 0x3) {
        return decode_load_store_imm(raw);
    }

    /* Formats 7 & 8: Load/store register offset / sign-extended
       0101 xx0 = Format 7 (register offset)
       0101 xx1 = Format 8 (sign-extended) */
    if (top4 == 0x5) {
        if (BIT(raw, 9) == 0) {
            return decode_load_store_reg(raw);
        } else {
            return decode_load_store_sign(raw);
        }
    }

    /* Format 6: PC-relative load */
    if (top5 == 0x09) {  /* 01001 */
        return decode_pc_rel_load(raw);
    }

    /* Format 5: Hi register operations / BX */
    if (top6 == 0x11) {  /* 010001 */
        return decode_hi_reg_bx(raw);
    }

    /* Format 4: ALU operations */
    if (top6 == 0x10) {  /* 010000 */
        return decode_alu_ops(raw);
    }

    /* Format 3: Move/Compare/Add/Sub immediate */
    if (top3 == 0x1) {
        return decode_mov_cmp_add_sub_imm(raw);
    }

    /* Formats 1 & 2: top3 == 000 */
    if (top3 == 0x0) {
        /* Format 2: bits 12:11 == 11 -> ADD/SUB */
        if (BITS(raw, 12, 11) == 3) {
            return decode_add_sub(raw);
        }
        /* Format 1: Move shifted register */
        return decode_move_shifted(raw);
    }

    return make_undefined(raw);
}

const char* thumb_insn_type_name(ThumbInsnType type) {
    switch (type) {
        case THUMB_MOVE_SHIFTED:        return "MOVE_SHIFTED";
        case THUMB_ADD_SUB:             return "ADD_SUB";
        case THUMB_MOV_CMP_ADD_SUB_IMM: return "MOV_CMP_ADD_SUB_IMM";
        case THUMB_ALU_OPS:             return "ALU_OPS";
        case THUMB_HI_REG_OPS:          return "HI_REG_OPS";
        case THUMB_BX:                  return "BX";
        case THUMB_PC_REL_LOAD:         return "PC_REL_LOAD";
        case THUMB_LOAD_STORE_REG:      return "LOAD_STORE_REG";
        case THUMB_LOAD_STORE_SIGN:     return "LOAD_STORE_SIGN";
        case THUMB_LOAD_STORE_IMM:      return "LOAD_STORE_IMM";
        case THUMB_LOAD_STORE_HALF:     return "LOAD_STORE_HALF";
        case THUMB_SP_REL_LOAD_STORE:   return "SP_REL_LOAD_STORE";
        case THUMB_LOAD_ADDR:           return "LOAD_ADDR";
        case THUMB_ADD_SP:              return "ADD_SP";
        case THUMB_PUSH_POP:            return "PUSH_POP";
        case THUMB_MULTI_LOAD_STORE:    return "MULTI_LOAD_STORE";
        case THUMB_COND_BRANCH:         return "COND_BRANCH";
        case THUMB_SWI:                 return "SWI";
        case THUMB_BRANCH:              return "BRANCH";
        case THUMB_LONG_BRANCH:         return "LONG_BRANCH";
        case THUMB_UNDEFINED:           return "UNDEFINED";
    }
    return "UNKNOWN";
}

bool thumb_is_branch(const ThumbInsn* insn) {
    switch (insn->type) {
        case THUMB_BX:
        case THUMB_COND_BRANCH:
        case THUMB_BRANCH:
        case THUMB_LONG_BRANCH:
            return true;
        default:
            return false;
    }
}

bool thumb_is_bl_prefix(u16 insn) {
    return BITS(insn, 15, 11) == 0x1E;
}

bool thumb_is_bl_suffix(u16 insn) {
    return BITS(insn, 15, 11) == 0x1F;
}
