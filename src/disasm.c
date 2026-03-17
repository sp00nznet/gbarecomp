#include "gba/disasm.h"
#include <stdio.h>
#include <string.h>

/* snprintf wrapper that returns chars written and advances buffer */
#define BUF_PRINTF(buf, remaining, ...) do { \
    int _n = snprintf(buf, remaining, __VA_ARGS__); \
    if (_n > 0) { buf += _n; remaining -= (size_t)_n; } \
} while (0)

static void fmt_shift(char** buf, size_t* rem, const ArmInsn* insn) {
    if (insn->shift_reg) {
        BUF_PRINTF(*buf, *rem, ", %s %s",
                   arm_shift_name(insn->shift_type),
                   arm_reg_name(insn->rs));
    } else if (insn->shift_amount != 0 || insn->shift_type != SHIFT_LSL) {
        BUF_PRINTF(*buf, *rem, ", %s #%u",
                   arm_shift_name(insn->shift_type),
                   insn->shift_amount);
    }
}

static void fmt_operand2(char** buf, size_t* rem, const ArmInsn* insn) {
    if (insn->i) {
        BUF_PRINTF(*buf, *rem, "#0x%X", insn->imm);
    } else {
        BUF_PRINTF(*buf, *rem, "%s", arm_reg_name(insn->rm));
        fmt_shift(buf, rem, insn);
    }
}

static void fmt_addr_mode(char** buf, size_t* rem, const ArmInsn* insn) {
    /* [Rn, offset] or [Rn], offset */
    if (insn->p) {
        /* Pre-indexed */
        BUF_PRINTF(*buf, *rem, "[%s, ", arm_reg_name(insn->rn));
        if (!insn->i) {
            /* Immediate offset for single data transfer (i=0 means immediate) */
            BUF_PRINTF(*buf, *rem, "#%s0x%X",
                       insn->u ? "" : "-", insn->imm);
        } else {
            BUF_PRINTF(*buf, *rem, "%s%s",
                       insn->u ? "" : "-",
                       arm_reg_name(insn->rm));
            fmt_shift(buf, rem, insn);
        }
        BUF_PRINTF(*buf, *rem, "]%s", insn->w ? "!" : "");
    } else {
        /* Post-indexed */
        BUF_PRINTF(*buf, *rem, "[%s], ", arm_reg_name(insn->rn));
        if (!insn->i) {
            BUF_PRINTF(*buf, *rem, "#%s0x%X",
                       insn->u ? "" : "-", insn->imm);
        } else {
            BUF_PRINTF(*buf, *rem, "%s%s",
                       insn->u ? "" : "-",
                       arm_reg_name(insn->rm));
            fmt_shift(buf, rem, insn);
        }
    }
}

static void fmt_halfword_addr(char** buf, size_t* rem, const ArmInsn* insn) {
    if (insn->p) {
        BUF_PRINTF(*buf, *rem, "[%s, ", arm_reg_name(insn->rn));
        if (insn->i) {
            BUF_PRINTF(*buf, *rem, "#%s0x%X",
                       insn->u ? "" : "-", insn->imm);
        } else {
            BUF_PRINTF(*buf, *rem, "%s%s",
                       insn->u ? "" : "-",
                       arm_reg_name(insn->rm));
        }
        BUF_PRINTF(*buf, *rem, "]%s", insn->w ? "!" : "");
    } else {
        BUF_PRINTF(*buf, *rem, "[%s], ", arm_reg_name(insn->rn));
        if (insn->i) {
            BUF_PRINTF(*buf, *rem, "#%s0x%X",
                       insn->u ? "" : "-", insn->imm);
        } else {
            BUF_PRINTF(*buf, *rem, "%s%s",
                       insn->u ? "" : "-",
                       arm_reg_name(insn->rm));
        }
    }
}

static void fmt_reg_list(char** buf, size_t* rem, u16 list) {
    BUF_PRINTF(*buf, *rem, "{");
    bool first = true;
    for (int i = 0; i < 16; i++) {
        if (list & (1 << i)) {
            if (!first) BUF_PRINTF(*buf, *rem, ", ");
            BUF_PRINTF(*buf, *rem, "%s", arm_reg_name((u8)i));
            first = false;
        }
    }
    BUF_PRINTF(*buf, *rem, "}");
}

int disasm_arm(const ArmInsn* insn, u32 addr, char* buf, size_t buf_size) {
    char* p = buf;
    size_t rem = buf_size;
    const char* cond = arm_cond_name(insn->cond);

    switch (insn->type) {
    /* Data processing */
    case ARM_AND: case ARM_EOR: case ARM_SUB: case ARM_RSB:
    case ARM_ADD: case ARM_ADC: case ARM_SBC: case ARM_RSC:
    case ARM_ORR: case ARM_BIC:
        BUF_PRINTF(p, rem, "%s%s%s %s, %s, ",
                   arm_insn_type_name(insn->type), cond,
                   insn->s ? "S" : "",
                   arm_reg_name(insn->rd),
                   arm_reg_name(insn->rn));
        fmt_operand2(&p, &rem, insn);
        break;

    case ARM_MOV: case ARM_MVN:
        BUF_PRINTF(p, rem, "%s%s%s %s, ",
                   arm_insn_type_name(insn->type), cond,
                   insn->s ? "S" : "",
                   arm_reg_name(insn->rd));
        fmt_operand2(&p, &rem, insn);
        break;

    case ARM_TST: case ARM_TEQ: case ARM_CMP: case ARM_CMN:
        BUF_PRINTF(p, rem, "%s%s %s, ",
                   arm_insn_type_name(insn->type), cond,
                   arm_reg_name(insn->rn));
        fmt_operand2(&p, &rem, insn);
        break;

    /* Multiply */
    case ARM_MUL:
        BUF_PRINTF(p, rem, "MUL%s%s %s, %s, %s",
                   cond, insn->s ? "S" : "",
                   arm_reg_name(insn->rd),
                   arm_reg_name(insn->rm),
                   arm_reg_name(insn->rs));
        break;

    case ARM_MLA:
        BUF_PRINTF(p, rem, "MLA%s%s %s, %s, %s, %s",
                   cond, insn->s ? "S" : "",
                   arm_reg_name(insn->rd),
                   arm_reg_name(insn->rm),
                   arm_reg_name(insn->rs),
                   arm_reg_name(insn->rn));
        break;

    /* Multiply long */
    case ARM_UMULL: case ARM_UMLAL: case ARM_SMULL: case ARM_SMLAL:
        BUF_PRINTF(p, rem, "%s%s%s %s, %s, %s, %s",
                   arm_insn_type_name(insn->type), cond,
                   insn->s ? "S" : "",
                   arm_reg_name(insn->rd),   /* RdLo */
                   arm_reg_name(insn->rn),   /* RdHi */
                   arm_reg_name(insn->rm),
                   arm_reg_name(insn->rs));
        break;

    /* Branch */
    case ARM_B: case ARM_BL: {
        u32 target = addr + 8 + (u32)insn->branch_offset;
        BUF_PRINTF(p, rem, "%s%s 0x%08X",
                   arm_insn_type_name(insn->type), cond, target);
        break;
    }

    /* Branch exchange */
    case ARM_BX:
        BUF_PRINTF(p, rem, "BX%s %s", cond, arm_reg_name(insn->rm));
        break;

    /* Single data transfer */
    case ARM_LDR: case ARM_STR:
        BUF_PRINTF(p, rem, "%s%s%s %s, ",
                   arm_insn_type_name(insn->type), cond,
                   insn->b ? "B" : "",
                   arm_reg_name(insn->rd));
        fmt_addr_mode(&p, &rem, insn);
        break;

    /* Halfword / signed transfers */
    case ARM_LDRH: case ARM_STRH: case ARM_LDRSB: case ARM_LDRSH:
        BUF_PRINTF(p, rem, "%s%s %s, ",
                   arm_insn_type_name(insn->type), cond,
                   arm_reg_name(insn->rd));
        fmt_halfword_addr(&p, &rem, insn);
        break;

    /* Block transfer */
    case ARM_LDM: case ARM_STM: {
        const char* mode;
        if (insn->type == ARM_LDM) {
            if (insn->u && insn->p) mode = "IB";
            else if (insn->u && !insn->p) mode = "IA";
            else if (!insn->u && insn->p) mode = "DB";
            else mode = "DA";
        } else {
            if (insn->u && insn->p) mode = "IB";
            else if (insn->u && !insn->p) mode = "IA";
            else if (!insn->u && insn->p) mode = "DB";
            else mode = "DA";
        }
        BUF_PRINTF(p, rem, "%s%s%s %s%s, ",
                   arm_insn_type_name(insn->type), cond, mode,
                   arm_reg_name(insn->rn),
                   insn->w ? "!" : "");
        fmt_reg_list(&p, &rem, insn->reg_list);
        if (insn->s) BUF_PRINTF(p, rem, "^");
        break;
    }

    /* Swap */
    case ARM_SWP: case ARM_SWPB:
        BUF_PRINTF(p, rem, "%s%s %s, %s, [%s]",
                   arm_insn_type_name(insn->type), cond,
                   arm_reg_name(insn->rd),
                   arm_reg_name(insn->rm),
                   arm_reg_name(insn->rn));
        break;

    /* Status register */
    case ARM_MRS:
        BUF_PRINTF(p, rem, "MRS%s %s, %s",
                   cond, arm_reg_name(insn->rd),
                   insn->r ? "SPSR" : "CPSR");
        break;

    case ARM_MSR:
        BUF_PRINTF(p, rem, "MSR%s %s_",
                   cond, insn->r ? "SPSR" : "CPSR");
        if (insn->msr_mask & 1) BUF_PRINTF(p, rem, "c");
        if (insn->msr_mask & 2) BUF_PRINTF(p, rem, "x");
        if (insn->msr_mask & 4) BUF_PRINTF(p, rem, "s");
        if (insn->msr_mask & 8) BUF_PRINTF(p, rem, "f");
        BUF_PRINTF(p, rem, ", ");
        if (insn->i) {
            BUF_PRINTF(p, rem, "#0x%X", insn->imm);
        } else {
            BUF_PRINTF(p, rem, "%s", arm_reg_name(insn->rm));
        }
        break;

    /* SWI */
    case ARM_SWI:
        BUF_PRINTF(p, rem, "SWI%s 0x%X", cond, insn->swi_number);
        break;

    case ARM_UNDEFINED:
    default:
        BUF_PRINTF(p, rem, "DCD 0x%08X  ; undefined", insn->raw);
        break;
    }

    return (int)(p - buf);
}

/* ---- Thumb disassembly ---- */

static const char* thumb_alu_op_name(ThumbAluOp op) {
    static const char* names[] = {
        "AND", "EOR", "LSL", "LSR", "ASR", "ADC", "SBC", "ROR",
        "TST", "NEG", "CMP", "CMN", "ORR", "MUL", "BIC", "MVN",
    };
    if ((unsigned)op > 15) return "???";
    return names[op];
}

static void fmt_thumb_rlist(char** buf, size_t* rem, u8 list, int extra_reg) {
    BUF_PRINTF(*buf, *rem, "{");
    bool first = true;
    for (int i = 0; i < 8; i++) {
        if (list & (1 << i)) {
            if (!first) BUF_PRINTF(*buf, *rem, ", ");
            BUF_PRINTF(*buf, *rem, "%s", arm_reg_name((u8)i));
            first = false;
        }
    }
    if (extra_reg >= 0) {
        if (!first) BUF_PRINTF(*buf, *rem, ", ");
        BUF_PRINTF(*buf, *rem, "%s", arm_reg_name((u8)extra_reg));
    }
    BUF_PRINTF(*buf, *rem, "}");
}

int disasm_thumb(const ThumbInsn* insn, u32 addr, char* buf, size_t buf_size) {
    char* p = buf;
    size_t rem = buf_size;

    switch (insn->type) {
    case THUMB_MOVE_SHIFTED:
        BUF_PRINTF(p, rem, "%s %s, %s, #%u",
                   arm_shift_name(insn->shift_type),
                   arm_reg_name((u8)insn->rd),
                   arm_reg_name((u8)insn->rs),
                   insn->shift_amount);
        break;

    case THUMB_ADD_SUB:
        if (insn->is_imm) {
            BUF_PRINTF(p, rem, "%s %s, %s, #%u",
                       insn->is_subtract ? "SUB" : "ADD",
                       arm_reg_name((u8)insn->rd),
                       arm_reg_name((u8)insn->rs),
                       insn->imm);
        } else {
            BUF_PRINTF(p, rem, "%s %s, %s, %s",
                       insn->is_subtract ? "SUB" : "ADD",
                       arm_reg_name((u8)insn->rd),
                       arm_reg_name((u8)insn->rs),
                       arm_reg_name((u8)insn->rm));
        }
        break;

    case THUMB_MOV_CMP_ADD_SUB_IMM: {
        const char* ops[] = { "MOV", "CMP", "ADD", "SUB" };
        u32 op = (u32)insn->hi_op;
        if (op > 3) op = 0;
        BUF_PRINTF(p, rem, "%s %s, #0x%X",
                   ops[op],
                   arm_reg_name((u8)insn->rd),
                   insn->imm);
        break;
    }

    case THUMB_ALU_OPS:
        BUF_PRINTF(p, rem, "%s %s, %s",
                   thumb_alu_op_name(insn->alu_op),
                   arm_reg_name((u8)insn->rd),
                   arm_reg_name((u8)insn->rs));
        break;

    case THUMB_HI_REG_OPS: {
        const char* ops[] = { "ADD", "CMP", "MOV" };
        u32 op = (u32)insn->hi_op;
        if (op > 2) op = 0;
        BUF_PRINTF(p, rem, "%s %s, %s",
                   ops[op],
                   arm_reg_name((u8)insn->rd),
                   arm_reg_name((u8)insn->rs));
        break;
    }

    case THUMB_BX:
        BUF_PRINTF(p, rem, "BX %s", arm_reg_name((u8)insn->rs));
        break;

    case THUMB_PC_REL_LOAD: {
        u32 base = (addr + 4) & ~3u; /* PC is word-aligned */
        BUF_PRINTF(p, rem, "LDR %s, [pc, #0x%X]  ; =0x%08X",
                   arm_reg_name((u8)insn->rd),
                   insn->imm,
                   base + insn->imm);
        break;
    }

    case THUMB_LOAD_STORE_REG:
        BUF_PRINTF(p, rem, "%s%s %s, [%s, %s]",
                   insn->is_load ? "LDR" : "STR",
                   insn->is_byte ? "B" : "",
                   arm_reg_name((u8)insn->rd),
                   arm_reg_name((u8)insn->rs),
                   arm_reg_name((u8)insn->rm));
        break;

    case THUMB_LOAD_STORE_SIGN:
        if (!insn->is_load && insn->is_half) {
            BUF_PRINTF(p, rem, "STRH %s, [%s, %s]",
                       arm_reg_name((u8)insn->rd),
                       arm_reg_name((u8)insn->rs),
                       arm_reg_name((u8)insn->rm));
        } else if (insn->is_load && insn->is_half && !insn->is_signed) {
            BUF_PRINTF(p, rem, "LDRH %s, [%s, %s]",
                       arm_reg_name((u8)insn->rd),
                       arm_reg_name((u8)insn->rs),
                       arm_reg_name((u8)insn->rm));
        } else if (insn->is_load && insn->is_byte && insn->is_signed) {
            BUF_PRINTF(p, rem, "LDRSB %s, [%s, %s]",
                       arm_reg_name((u8)insn->rd),
                       arm_reg_name((u8)insn->rs),
                       arm_reg_name((u8)insn->rm));
        } else {
            BUF_PRINTF(p, rem, "LDRSH %s, [%s, %s]",
                       arm_reg_name((u8)insn->rd),
                       arm_reg_name((u8)insn->rs),
                       arm_reg_name((u8)insn->rm));
        }
        break;

    case THUMB_LOAD_STORE_IMM:
        BUF_PRINTF(p, rem, "%s%s %s, [%s, #0x%X]",
                   insn->is_load ? "LDR" : "STR",
                   insn->is_byte ? "B" : "",
                   arm_reg_name((u8)insn->rd),
                   arm_reg_name((u8)insn->rs),
                   insn->imm);
        break;

    case THUMB_LOAD_STORE_HALF:
        BUF_PRINTF(p, rem, "%s %s, [%s, #0x%X]",
                   insn->is_load ? "LDRH" : "STRH",
                   arm_reg_name((u8)insn->rd),
                   arm_reg_name((u8)insn->rs),
                   insn->imm);
        break;

    case THUMB_SP_REL_LOAD_STORE:
        BUF_PRINTF(p, rem, "%s %s, [sp, #0x%X]",
                   insn->is_load ? "LDR" : "STR",
                   arm_reg_name((u8)insn->rd),
                   insn->imm);
        break;

    case THUMB_LOAD_ADDR:
        BUF_PRINTF(p, rem, "ADD %s, %s, #0x%X",
                   arm_reg_name((u8)insn->rd),
                   insn->is_sp ? "sp" : "pc",
                   insn->imm);
        break;

    case THUMB_ADD_SP:
        BUF_PRINTF(p, rem, "ADD sp, #%s0x%X",
                   insn->is_subtract ? "-" : "",
                   insn->imm);
        break;

    case THUMB_PUSH_POP:
        BUF_PRINTF(p, rem, "%s ", insn->is_load ? "POP" : "PUSH");
        fmt_thumb_rlist(&p, &rem, insn->rlist,
                        insn->pc_or_lr ? (insn->is_load ? REG_PC : REG_LR) : -1);
        break;

    case THUMB_MULTI_LOAD_STORE:
        BUF_PRINTF(p, rem, "%s %s!, ",
                   insn->is_load ? "LDMIA" : "STMIA",
                   arm_reg_name((u8)insn->rs));
        fmt_thumb_rlist(&p, &rem, insn->rlist, -1);
        break;

    case THUMB_COND_BRANCH: {
        u32 target = addr + 4 + (u32)insn->offset;
        BUF_PRINTF(p, rem, "B%s 0x%08X",
                   arm_cond_name(insn->cond), target);
        break;
    }

    case THUMB_SWI:
        BUF_PRINTF(p, rem, "SWI 0x%02X", insn->swi_num);
        break;

    case THUMB_BRANCH: {
        u32 target = addr + 4 + (u32)insn->offset;
        BUF_PRINTF(p, rem, "B 0x%08X", target);
        break;
    }

    case THUMB_LONG_BRANCH:
        if (insn->is_suffix) {
            BUF_PRINTF(p, rem, "BL <suffix> offset_lo=0x%X", insn->imm);
        } else {
            BUF_PRINTF(p, rem, "BL <prefix> offset_hi=0x%X", (u32)insn->offset);
        }
        break;

    case THUMB_UNDEFINED:
    default:
        BUF_PRINTF(p, rem, "DCW 0x%04X  ; undefined", insn->raw);
        break;
    }

    return (int)(p - buf);
}

/* ---- Range disassembly ---- */

void disasm_arm_range(const GbaRom* rom, u32 start, u32 end) {
    char buf[256];
    for (u32 addr = start; addr < end; addr += 4) {
        u32 raw = rom_read32(rom, addr);
        ArmInsn insn = arm_decode(raw);
        disasm_arm(&insn, addr, buf, sizeof(buf));
        printf("  %08X:  %08X  %s\n", addr, raw, buf);
    }
}

void disasm_thumb_range(const GbaRom* rom, u32 start, u32 end) {
    char buf[256];
    for (u32 addr = start; addr < end; addr += 2) {
        u16 raw = rom_read16(rom, addr);
        ThumbInsn insn = thumb_decode(raw);
        disasm_thumb(&insn, addr, buf, sizeof(buf));
        printf("  %08X:  %04X      %s\n", addr, raw, buf);
    }
}
