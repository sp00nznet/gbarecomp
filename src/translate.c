#include "gba/translate.h"
#include "gba/disasm.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- Goto target validation ---- */

/* Check if target address is a block owned by the current function */
static bool is_local_label(TranslateCtx* ctx, u32 addr) {
    for (int i = 0; i < ctx->num_local_blocks; i++) {
        if (ctx->local_blocks[i] == addr) return true;
    }
    return false;
}

/* Emit a goto or tail-call depending on whether the target is local */
static void emit_goto_or_tailcall(TranslateCtx* ctx, u32 target) {
    if (is_local_label(ctx, target)) {
        for (int i = 0; i < ctx->indent; i++) fprintf(ctx->out, "    ");
        fprintf(ctx->out, "goto label_%08X;\n", target);
    } else {
        /* Target is in a different function - emit tail call */
        for (int i = 0; i < ctx->indent; i++) fprintf(ctx->out, "    ");
        fprintf(ctx->out, "func_%08X(); return; /* tail branch */\n", target);
    }
}

/* Emit a conditional goto or tail-call */
static void emit_cond_goto_or_tailcall(TranslateCtx* ctx, const char* cond, u32 target) {
    if (is_local_label(ctx, target)) {
        for (int i = 0; i < ctx->indent; i++) fprintf(ctx->out, "    ");
        fprintf(ctx->out, "if (%s) goto label_%08X;\n", cond, target);
    } else {
        for (int i = 0; i < ctx->indent; i++) fprintf(ctx->out, "    ");
        fprintf(ctx->out, "if (%s) { func_%08X(); return; } /* tail branch */\n", cond, target);
    }
}

/* ---- Output helpers ---- */

static void emit(TranslateCtx* ctx, const char* fmt, ...) {
    for (int i = 0; i < ctx->indent; i++) fprintf(ctx->out, "    ");
    va_list args;
    va_start(args, fmt);
    vfprintf(ctx->out, fmt, args);
    va_end(args);
    fprintf(ctx->out, "\n");
}

static void emit_raw(TranslateCtx* ctx, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(ctx->out, fmt, args);
    va_end(args);
}

static void emit_comment(TranslateCtx* ctx, const char* fmt, ...) {
    for (int i = 0; i < ctx->indent; i++) fprintf(ctx->out, "    ");
    fprintf(ctx->out, "/* ");
    va_list args;
    va_start(args, fmt);
    vfprintf(ctx->out, fmt, args);
    va_end(args);
    fprintf(ctx->out, " */\n");
}

/* ---- Condition code generation ---- */

static const char* cond_to_c(ArmCondition cond) {
    switch (cond) {
        case COND_EQ: return "CPU_Z";
        case COND_NE: return "!CPU_Z";
        case COND_CS: return "CPU_C";
        case COND_CC: return "!CPU_C";
        case COND_MI: return "CPU_N";
        case COND_PL: return "!CPU_N";
        case COND_VS: return "CPU_V";
        case COND_VC: return "!CPU_V";
        case COND_HI: return "(CPU_C && !CPU_Z)";
        case COND_LS: return "(!CPU_C || CPU_Z)";
        case COND_GE: return "(CPU_N == CPU_V)";
        case COND_LT: return "(CPU_N != CPU_V)";
        case COND_GT: return "(!CPU_Z && CPU_N == CPU_V)";
        case COND_LE: return "(CPU_Z || CPU_N != CPU_V)";
        case COND_AL: return NULL; /* unconditional */
        case COND_NV: return "0";  /* never */
    }
    return "0";
}

/* Register name for C code: r[0] through r[15] */
static const char* reg_c(u8 reg) {
    static const char* names[] = {
        "r[0]",  "r[1]",  "r[2]",  "r[3]",
        "r[4]",  "r[5]",  "r[6]",  "r[7]",
        "r[8]",  "r[9]",  "r[10]", "r[11]",
        "r[12]", "r[13]", "r[14]", "r[15]",
    };
    if (reg > 15) return "r[0]";
    return names[reg];
}

/* Emit operand2 as C expression (written to a buffer) */
static void operand2_to_c(const ArmInsn* insn, char* buf, size_t size) {
    if (insn->i) {
        snprintf(buf, size, "0x%Xu", insn->imm);
    } else {
        if (insn->shift_reg) {
            const char* shift_op;
            switch (insn->shift_type) {
                case SHIFT_LSL: shift_op = "<<"; break;
                case SHIFT_LSR: shift_op = ">>"; break;
                case SHIFT_ASR: shift_op = ">>"; break; /* needs cast for arithmetic */
                case SHIFT_ROR: shift_op = "ROR"; break;
                default: shift_op = "<<"; break;
            }
            if (insn->shift_type == SHIFT_ROR) {
                snprintf(buf, size, "ROR32(%s, %s & 0xFF)",
                         reg_c(insn->rm), reg_c(insn->rs));
            } else if (insn->shift_type == SHIFT_ASR) {
                snprintf(buf, size, "(u32)((s32)%s >> (%s & 0xFF))",
                         reg_c(insn->rm), reg_c(insn->rs));
            } else {
                snprintf(buf, size, "(%s %s (%s & 0xFF))",
                         reg_c(insn->rm), shift_op, reg_c(insn->rs));
            }
        } else if (insn->shift_amount == 0 && insn->shift_type == SHIFT_LSL) {
            snprintf(buf, size, "%s", reg_c(insn->rm));
        } else {
            switch (insn->shift_type) {
                case SHIFT_LSL:
                    snprintf(buf, size, "(%s << %u)", reg_c(insn->rm), insn->shift_amount);
                    break;
                case SHIFT_LSR:
                    snprintf(buf, size, "(%s >> %u)", reg_c(insn->rm),
                             insn->shift_amount == 0 ? 32 : insn->shift_amount);
                    break;
                case SHIFT_ASR:
                    snprintf(buf, size, "(u32)((s32)%s >> %u)", reg_c(insn->rm),
                             insn->shift_amount == 0 ? 32 : insn->shift_amount);
                    break;
                case SHIFT_ROR:
                    if (insn->shift_amount == 0) {
                        snprintf(buf, size, "RRX(%s)", reg_c(insn->rm));
                    } else {
                        snprintf(buf, size, "ROR32(%s, %u)", reg_c(insn->rm), insn->shift_amount);
                    }
                    break;
            }
        }
    }
}

/* ---- Condition wrapping ---- */

static void begin_cond(TranslateCtx* ctx, ArmCondition cond) {
    const char* c = cond_to_c(cond);
    if (c) {
        emit(ctx, "if (%s) {", c);
        ctx->indent++;
    }
}

static void end_cond(TranslateCtx* ctx, ArmCondition cond) {
    if (cond_to_c(cond)) {
        ctx->indent--;
        emit(ctx, "}");
    }
}

/* ---- ARM instruction translation ---- */

void translate_arm_insn(TranslateCtx* ctx, const ArmInsn* insn, u32 addr) {
    char disasm_buf[256];
    disasm_arm(insn, addr, disasm_buf, sizeof(disasm_buf));
    emit_comment(ctx, "0x%08X: %s", addr, disasm_buf);

    char op2[256];

    /* Set PC to the correct value for this instruction (ARM: addr+8) */
    /* This is needed for any instruction that reads r[15] */
    bool reads_pc = (insn->rn == REG_PC || insn->rm == REG_PC ||
                     (!insn->i && insn->shift_reg && insn->rs == REG_PC));
    if (insn->type == ARM_LDR || insn->type == ARM_STR) {
        reads_pc = (insn->rn == REG_PC);
    }
    if (insn->type == ARM_MOV || insn->type == ARM_MVN ||
        insn->type == ARM_ADD || insn->type == ARM_SUB) {
        if (!insn->i && insn->rm == REG_PC) reads_pc = true;
        if (insn->rn == REG_PC) reads_pc = true;
    }
    if (reads_pc) {
        emit(ctx, "r[15] = 0x%08Xu; /* PC */", addr + 8);
    }

    switch (insn->type) {
    /* ---- Data processing ---- */
    case ARM_MOV:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "%s = %s;", reg_c(insn->rd), op2);
        if (insn->s && insn->rd != REG_PC) {
            emit(ctx, "cpu_update_nz(%s);", reg_c(insn->rd));
        }
        end_cond(ctx, insn->cond);
        break;

    case ARM_MVN:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "%s = ~(%s);", reg_c(insn->rd), op2);
        if (insn->s && insn->rd != REG_PC) {
            emit(ctx, "cpu_update_nz(%s);", reg_c(insn->rd));
        }
        end_cond(ctx, insn->cond);
        break;

    case ARM_AND:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "%s = %s & %s;", reg_c(insn->rd), reg_c(insn->rn), op2);
        if (insn->s) emit(ctx, "cpu_update_nz(%s);", reg_c(insn->rd));
        end_cond(ctx, insn->cond);
        break;

    case ARM_ORR:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "%s = %s | %s;", reg_c(insn->rd), reg_c(insn->rn), op2);
        if (insn->s) emit(ctx, "cpu_update_nz(%s);", reg_c(insn->rd));
        end_cond(ctx, insn->cond);
        break;

    case ARM_EOR:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "%s = %s ^ %s;", reg_c(insn->rd), reg_c(insn->rn), op2);
        if (insn->s) emit(ctx, "cpu_update_nz(%s);", reg_c(insn->rd));
        end_cond(ctx, insn->cond);
        break;

    case ARM_BIC:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "%s = %s & ~(%s);", reg_c(insn->rd), reg_c(insn->rn), op2);
        if (insn->s) emit(ctx, "cpu_update_nz(%s);", reg_c(insn->rd));
        end_cond(ctx, insn->cond);
        break;

    case ARM_ADD:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        if (insn->s && insn->rd != REG_PC) {
            emit(ctx, "cpu_add(&%s, %s, %s, true);",
                 reg_c(insn->rd), reg_c(insn->rn), op2);
        } else {
            emit(ctx, "%s = %s + %s;", reg_c(insn->rd), reg_c(insn->rn), op2);
        }
        end_cond(ctx, insn->cond);
        break;

    case ARM_ADC:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "%s = %s + %s + CPU_C;", reg_c(insn->rd), reg_c(insn->rn), op2);
        if (insn->s) emit(ctx, "cpu_update_flags_adc(%s, %s, %s);",
                          reg_c(insn->rd), reg_c(insn->rn), op2);
        end_cond(ctx, insn->cond);
        break;

    case ARM_SUB:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        if (insn->s && insn->rd != REG_PC) {
            emit(ctx, "cpu_sub(&%s, %s, %s, true);",
                 reg_c(insn->rd), reg_c(insn->rn), op2);
        } else {
            emit(ctx, "%s = %s - %s;", reg_c(insn->rd), reg_c(insn->rn), op2);
        }
        end_cond(ctx, insn->cond);
        break;

    case ARM_RSB:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "%s = %s - %s;", reg_c(insn->rd), op2, reg_c(insn->rn));
        if (insn->s) emit(ctx, "cpu_update_flags_sub(%s, %s, %s);",
                          reg_c(insn->rd), op2, reg_c(insn->rn));
        end_cond(ctx, insn->cond);
        break;

    case ARM_SBC:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "%s = %s - %s - !CPU_C;", reg_c(insn->rd), reg_c(insn->rn), op2);
        if (insn->s) emit(ctx, "/* TODO: SBC flags */");
        end_cond(ctx, insn->cond);
        break;

    case ARM_RSC:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "%s = %s - %s - !CPU_C;", reg_c(insn->rd), op2, reg_c(insn->rn));
        if (insn->s) emit(ctx, "/* TODO: RSC flags */");
        end_cond(ctx, insn->cond);
        break;

    case ARM_CMP:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "cpu_sub(NULL, %s, %s, true);", reg_c(insn->rn), op2);
        end_cond(ctx, insn->cond);
        break;

    case ARM_CMN:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "cpu_add(NULL, %s, %s, true);", reg_c(insn->rn), op2);
        end_cond(ctx, insn->cond);
        break;

    case ARM_TST:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "cpu_update_nz(%s & %s);", reg_c(insn->rn), op2);
        end_cond(ctx, insn->cond);
        break;

    case ARM_TEQ:
        begin_cond(ctx, insn->cond);
        operand2_to_c(insn, op2, sizeof(op2));
        emit(ctx, "cpu_update_nz(%s ^ %s);", reg_c(insn->rn), op2);
        end_cond(ctx, insn->cond);
        break;

    /* ---- Multiply ---- */
    case ARM_MUL:
        begin_cond(ctx, insn->cond);
        emit(ctx, "%s = %s * %s;", reg_c(insn->rd), reg_c(insn->rm), reg_c(insn->rs));
        if (insn->s) emit(ctx, "cpu_update_nz(%s);", reg_c(insn->rd));
        end_cond(ctx, insn->cond);
        break;

    case ARM_MLA:
        begin_cond(ctx, insn->cond);
        emit(ctx, "%s = %s * %s + %s;",
             reg_c(insn->rd), reg_c(insn->rm), reg_c(insn->rs), reg_c(insn->rn));
        if (insn->s) emit(ctx, "cpu_update_nz(%s);", reg_c(insn->rd));
        end_cond(ctx, insn->cond);
        break;

    case ARM_UMULL:
        begin_cond(ctx, insn->cond);
        emit(ctx, "{");
        ctx->indent++;
        emit(ctx, "u64 _result = (u64)%s * (u64)%s;", reg_c(insn->rm), reg_c(insn->rs));
        emit(ctx, "%s = (u32)_result;", reg_c(insn->rd));          /* RdLo */
        emit(ctx, "%s = (u32)(_result >> 32);", reg_c(insn->rn));  /* RdHi */
        ctx->indent--;
        emit(ctx, "}");
        end_cond(ctx, insn->cond);
        break;

    case ARM_SMULL:
        begin_cond(ctx, insn->cond);
        emit(ctx, "{");
        ctx->indent++;
        emit(ctx, "s64 _result = (s64)(s32)%s * (s64)(s32)%s;",
             reg_c(insn->rm), reg_c(insn->rs));
        emit(ctx, "%s = (u32)_result;", reg_c(insn->rd));
        emit(ctx, "%s = (u32)((u64)_result >> 32);", reg_c(insn->rn));
        ctx->indent--;
        emit(ctx, "}");
        end_cond(ctx, insn->cond);
        break;

    case ARM_UMLAL:
        begin_cond(ctx, insn->cond);
        emit(ctx, "{");
        ctx->indent++;
        emit(ctx, "u64 _acc = ((u64)%s << 32) | %s;", reg_c(insn->rn), reg_c(insn->rd));
        emit(ctx, "u64 _result = _acc + (u64)%s * (u64)%s;",
             reg_c(insn->rm), reg_c(insn->rs));
        emit(ctx, "%s = (u32)_result;", reg_c(insn->rd));
        emit(ctx, "%s = (u32)(_result >> 32);", reg_c(insn->rn));
        ctx->indent--;
        emit(ctx, "}");
        end_cond(ctx, insn->cond);
        break;

    case ARM_SMLAL:
        begin_cond(ctx, insn->cond);
        emit(ctx, "{");
        ctx->indent++;
        emit(ctx, "s64 _acc = ((s64)(s32)%s << 32) | (u32)%s;",
             reg_c(insn->rn), reg_c(insn->rd));
        emit(ctx, "s64 _result = _acc + (s64)(s32)%s * (s64)(s32)%s;",
             reg_c(insn->rm), reg_c(insn->rs));
        emit(ctx, "%s = (u32)_result;", reg_c(insn->rd));
        emit(ctx, "%s = (u32)((u64)_result >> 32);", reg_c(insn->rn));
        ctx->indent--;
        emit(ctx, "}");
        end_cond(ctx, insn->cond);
        break;

    /* ---- Branch ---- */
    case ARM_B: {
        u32 target = addr + 8 + (u32)insn->branch_offset;
        if (insn->cond == COND_AL) {
            emit_goto_or_tailcall(ctx, target);
        } else {
            const char* c = cond_to_c(insn->cond);
            if (c) emit_cond_goto_or_tailcall(ctx, c, target);
        }
        break;
    }

    case ARM_BL: {
        u32 target = addr + 8 + (u32)insn->branch_offset;
        begin_cond(ctx, insn->cond);
        emit(ctx, "r[14] = 0x%08Xu; /* return address */", addr + 4);
        emit(ctx, "func_%08X(); /* BL */", target);
        end_cond(ctx, insn->cond);
        break;
    }

    case ARM_BX:
        begin_cond(ctx, insn->cond);
        if (insn->rm == REG_LR) {
            emit(ctx, "return; /* BX LR */");
        } else {
            emit(ctx, "cpu_bx(%s); /* indirect branch */", reg_c(insn->rm));
        }
        end_cond(ctx, insn->cond);
        break;

    /* ---- Memory access ---- */
    case ARM_LDR:
        begin_cond(ctx, insn->cond);
        if (insn->p) {
            /* Pre-indexed */
            if (!insn->i) {
                /* Immediate offset */
                if (insn->b) {
                    emit(ctx, "%s = bus_read8(%s %s 0x%X);",
                         reg_c(insn->rd), reg_c(insn->rn),
                         insn->u ? "+" : "-", insn->imm);
                } else {
                    emit(ctx, "%s = bus_read32(%s %s 0x%X);",
                         reg_c(insn->rd), reg_c(insn->rn),
                         insn->u ? "+" : "-", insn->imm);
                }
            } else {
                operand2_to_c(insn, op2, sizeof(op2));
                emit(ctx, "%s = bus_read%s(%s %s %s);",
                     reg_c(insn->rd), insn->b ? "8" : "32",
                     reg_c(insn->rn), insn->u ? "+" : "-", op2);
            }
            if (insn->w) {
                if (!insn->i) {
                    emit(ctx, "%s %s= 0x%X;",
                         reg_c(insn->rn), insn->u ? "+" : "-", insn->imm);
                } else {
                    operand2_to_c(insn, op2, sizeof(op2));
                    emit(ctx, "%s %s= %s;",
                         reg_c(insn->rn), insn->u ? "+" : "-", op2);
                }
            }
        } else {
            /* Post-indexed */
            emit(ctx, "%s = bus_read%s(%s);",
                 reg_c(insn->rd), insn->b ? "8" : "32", reg_c(insn->rn));
            if (!insn->i) {
                emit(ctx, "%s %s= 0x%X;",
                     reg_c(insn->rn), insn->u ? "+" : "-", insn->imm);
            } else {
                operand2_to_c(insn, op2, sizeof(op2));
                emit(ctx, "%s %s= %s;",
                     reg_c(insn->rn), insn->u ? "+" : "-", op2);
            }
        }
        end_cond(ctx, insn->cond);
        break;

    case ARM_STR:
        begin_cond(ctx, insn->cond);
        if (insn->p) {
            if (!insn->i) {
                emit(ctx, "bus_write%s(%s %s 0x%X, %s);",
                     insn->b ? "8" : "32",
                     reg_c(insn->rn), insn->u ? "+" : "-",
                     insn->imm, reg_c(insn->rd));
            } else {
                operand2_to_c(insn, op2, sizeof(op2));
                emit(ctx, "bus_write%s(%s %s %s, %s);",
                     insn->b ? "8" : "32",
                     reg_c(insn->rn), insn->u ? "+" : "-",
                     op2, reg_c(insn->rd));
            }
            if (insn->w) {
                if (!insn->i) {
                    emit(ctx, "%s %s= 0x%X;",
                         reg_c(insn->rn), insn->u ? "+" : "-", insn->imm);
                }
            }
        } else {
            emit(ctx, "bus_write%s(%s, %s);",
                 insn->b ? "8" : "32", reg_c(insn->rn), reg_c(insn->rd));
            if (!insn->i) {
                emit(ctx, "%s %s= 0x%X;",
                     reg_c(insn->rn), insn->u ? "+" : "-", insn->imm);
            }
        }
        end_cond(ctx, insn->cond);
        break;

    /* ---- Halfword loads/stores ---- */
    case ARM_LDRH:
        begin_cond(ctx, insn->cond);
        if (insn->i) {
            emit(ctx, "%s = bus_read16(%s %s 0x%X);",
                 reg_c(insn->rd), reg_c(insn->rn),
                 insn->u ? "+" : "-", insn->imm);
        } else {
            emit(ctx, "%s = bus_read16(%s %s %s);",
                 reg_c(insn->rd), reg_c(insn->rn),
                 insn->u ? "+" : "-", reg_c(insn->rm));
        }
        end_cond(ctx, insn->cond);
        break;

    case ARM_LDRSH:
        begin_cond(ctx, insn->cond);
        if (insn->i) {
            emit(ctx, "%s = (u32)(s32)(s16)bus_read16(%s %s 0x%X);",
                 reg_c(insn->rd), reg_c(insn->rn),
                 insn->u ? "+" : "-", insn->imm);
        } else {
            emit(ctx, "%s = (u32)(s32)(s16)bus_read16(%s %s %s);",
                 reg_c(insn->rd), reg_c(insn->rn),
                 insn->u ? "+" : "-", reg_c(insn->rm));
        }
        end_cond(ctx, insn->cond);
        break;

    case ARM_LDRSB:
        begin_cond(ctx, insn->cond);
        if (insn->i) {
            emit(ctx, "%s = (u32)(s32)(s8)bus_read8(%s %s 0x%X);",
                 reg_c(insn->rd), reg_c(insn->rn),
                 insn->u ? "+" : "-", insn->imm);
        } else {
            emit(ctx, "%s = (u32)(s32)(s8)bus_read8(%s %s %s);",
                 reg_c(insn->rd), reg_c(insn->rn),
                 insn->u ? "+" : "-", reg_c(insn->rm));
        }
        end_cond(ctx, insn->cond);
        break;

    case ARM_STRH:
        begin_cond(ctx, insn->cond);
        if (insn->i) {
            emit(ctx, "bus_write16(%s %s 0x%X, %s);",
                 reg_c(insn->rn), insn->u ? "+" : "-",
                 insn->imm, reg_c(insn->rd));
        } else {
            emit(ctx, "bus_write16(%s %s %s, %s);",
                 reg_c(insn->rn), insn->u ? "+" : "-",
                 reg_c(insn->rm), reg_c(insn->rd));
        }
        end_cond(ctx, insn->cond);
        break;

    /* ---- Block transfer ---- */
    case ARM_LDM: case ARM_STM:
        begin_cond(ctx, insn->cond);
        emit(ctx, "{");
        ctx->indent++;
        emit(ctx, "u32 _addr = %s;", reg_c(insn->rn));

        /* Determine direction and ordering */
        if (insn->type == ARM_LDM) {
            for (int i = 0; i < 16; i++) {
                if (!(insn->reg_list & (1 << i))) continue;
                if (insn->p) {
                    emit(ctx, "_addr %s= 4;", insn->u ? "+" : "-");
                    emit(ctx, "r[%d] = bus_read32(_addr);", i);
                } else {
                    emit(ctx, "r[%d] = bus_read32(_addr);", i);
                    emit(ctx, "_addr %s= 4;", insn->u ? "+" : "-");
                }
            }
        } else {
            for (int i = 0; i < 16; i++) {
                if (!(insn->reg_list & (1 << i))) continue;
                if (insn->p) {
                    emit(ctx, "_addr %s= 4;", insn->u ? "+" : "-");
                    emit(ctx, "bus_write32(_addr, r[%d]);", i);
                } else {
                    emit(ctx, "bus_write32(_addr, r[%d]);", i);
                    emit(ctx, "_addr %s= 4;", insn->u ? "+" : "-");
                }
            }
        }
        if (insn->w) {
            emit(ctx, "%s = _addr;", reg_c(insn->rn));
        }
        ctx->indent--;
        emit(ctx, "}");
        end_cond(ctx, insn->cond);
        break;

    /* ---- Status register ---- */
    case ARM_MRS:
        begin_cond(ctx, insn->cond);
        emit(ctx, "%s = cpu_get_%s();", reg_c(insn->rd), insn->r ? "spsr" : "cpsr");
        end_cond(ctx, insn->cond);
        break;

    case ARM_MSR:
        begin_cond(ctx, insn->cond);
        if (insn->i) {
            emit(ctx, "cpu_set_%s(0x%X, 0x%X);",
                 insn->r ? "spsr" : "cpsr", insn->imm, (u32)insn->msr_mask);
        } else {
            emit(ctx, "cpu_set_%s(%s, 0x%X);",
                 insn->r ? "spsr" : "cpsr", reg_c(insn->rm), (u32)insn->msr_mask);
        }
        end_cond(ctx, insn->cond);
        break;

    /* ---- Swap ---- */
    case ARM_SWP:
        begin_cond(ctx, insn->cond);
        emit(ctx, "{");
        ctx->indent++;
        emit(ctx, "u32 _tmp = bus_read32(%s);", reg_c(insn->rn));
        emit(ctx, "bus_write32(%s, %s);", reg_c(insn->rn), reg_c(insn->rm));
        emit(ctx, "%s = _tmp;", reg_c(insn->rd));
        ctx->indent--;
        emit(ctx, "}");
        end_cond(ctx, insn->cond);
        break;

    case ARM_SWPB:
        begin_cond(ctx, insn->cond);
        emit(ctx, "{");
        ctx->indent++;
        emit(ctx, "u32 _tmp = bus_read8(%s);", reg_c(insn->rn));
        emit(ctx, "bus_write8(%s, %s);", reg_c(insn->rn), reg_c(insn->rm));
        emit(ctx, "%s = _tmp;", reg_c(insn->rd));
        ctx->indent--;
        emit(ctx, "}");
        end_cond(ctx, insn->cond);
        break;

    /* ---- SWI ---- */
    case ARM_SWI:
        begin_cond(ctx, insn->cond);
        emit(ctx, "gba_swi(0x%X);", insn->swi_number);
        end_cond(ctx, insn->cond);
        break;

    case ARM_UNDEFINED:
    default:
        emit_comment(ctx, "UNHANDLED: 0x%08X", insn->raw);
        emit(ctx, "cpu_undefined(0x%08Xu);", insn->raw);
        break;
    }
}

/* ---- Thumb instruction translation ---- */

void translate_thumb_insn(TranslateCtx* ctx, const ThumbInsn* insn, u32 addr) {
    char disasm_buf[256];
    disasm_thumb(insn, addr, disasm_buf, sizeof(disasm_buf));
    emit_comment(ctx, "0x%08X: %s", addr, disasm_buf);

    switch (insn->type) {
    case THUMB_MOVE_SHIFTED:
        switch (insn->shift_type) {
            case SHIFT_LSL:
                emit(ctx, "%s = %s << %u;", reg_c((u8)insn->rd), reg_c((u8)insn->rs), insn->shift_amount);
                break;
            case SHIFT_LSR:
                emit(ctx, "%s = %s >> %u;", reg_c((u8)insn->rd), reg_c((u8)insn->rs),
                     insn->shift_amount == 0 ? 32 : (u32)insn->shift_amount);
                break;
            case SHIFT_ASR:
                emit(ctx, "%s = (u32)((s32)%s >> %u);", reg_c((u8)insn->rd), reg_c((u8)insn->rs),
                     insn->shift_amount == 0 ? 32 : (u32)insn->shift_amount);
                break;
            default:
                emit(ctx, "%s = %s; /* unexpected shift */", reg_c((u8)insn->rd), reg_c((u8)insn->rs));
                break;
        }
        emit(ctx, "cpu_update_nz(%s);", reg_c((u8)insn->rd));
        break;

    case THUMB_ADD_SUB:
        if (insn->is_imm) {
            emit(ctx, "cpu_%s(&%s, %s, %uu, true);",
                 insn->is_subtract ? "sub" : "add",
                 reg_c((u8)insn->rd), reg_c((u8)insn->rs), insn->imm);
        } else {
            emit(ctx, "cpu_%s(&%s, %s, %s, true);",
                 insn->is_subtract ? "sub" : "add",
                 reg_c((u8)insn->rd), reg_c((u8)insn->rs), reg_c((u8)insn->rm));
        }
        break;

    case THUMB_MOV_CMP_ADD_SUB_IMM: {
        u32 op = (u32)insn->hi_op;
        switch (op) {
            case 0: /* MOV */
                emit(ctx, "%s = 0x%Xu;", reg_c((u8)insn->rd), insn->imm);
                emit(ctx, "cpu_update_nz(%s);", reg_c((u8)insn->rd));
                break;
            case 1: /* CMP */
                emit(ctx, "cpu_sub(NULL, %s, 0x%Xu, true);", reg_c((u8)insn->rd), insn->imm);
                break;
            case 2: /* ADD */
                emit(ctx, "cpu_add(&%s, %s, 0x%Xu, true);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rd), insn->imm);
                break;
            case 3: /* SUB */
                emit(ctx, "cpu_sub(&%s, %s, 0x%Xu, true);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rd), insn->imm);
                break;
        }
        break;
    }

    case THUMB_ALU_OPS:
        switch (insn->alu_op) {
            case THUMB_ALU_AND:
                emit(ctx, "%s &= %s; cpu_update_nz(%s);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rs), reg_c((u8)insn->rd));
                break;
            case THUMB_ALU_EOR:
                emit(ctx, "%s ^= %s; cpu_update_nz(%s);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rs), reg_c((u8)insn->rd));
                break;
            case THUMB_ALU_ORR:
                emit(ctx, "%s |= %s; cpu_update_nz(%s);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rs), reg_c((u8)insn->rd));
                break;
            case THUMB_ALU_BIC:
                emit(ctx, "%s &= ~%s; cpu_update_nz(%s);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rs), reg_c((u8)insn->rd));
                break;
            case THUMB_ALU_MVN:
                emit(ctx, "%s = ~%s; cpu_update_nz(%s);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rs), reg_c((u8)insn->rd));
                break;
            case THUMB_ALU_NEG:
                emit(ctx, "cpu_sub(&%s, 0, %s, true);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rs));
                break;
            case THUMB_ALU_CMP:
                emit(ctx, "cpu_sub(NULL, %s, %s, true);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rs));
                break;
            case THUMB_ALU_CMN:
                emit(ctx, "cpu_add(NULL, %s, %s, true);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rs));
                break;
            case THUMB_ALU_TST:
                emit(ctx, "cpu_update_nz(%s & %s);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rs));
                break;
            case THUMB_ALU_LSL:
                emit(ctx, "%s <<= (%s & 0xFF); cpu_update_nz(%s);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rs), reg_c((u8)insn->rd));
                break;
            case THUMB_ALU_LSR:
                emit(ctx, "%s >>= (%s & 0xFF); cpu_update_nz(%s);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rs), reg_c((u8)insn->rd));
                break;
            case THUMB_ALU_ASR:
                emit(ctx, "%s = (u32)((s32)%s >> (%s & 0xFF)); cpu_update_nz(%s);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rd),
                     reg_c((u8)insn->rs), reg_c((u8)insn->rd));
                break;
            case THUMB_ALU_ROR:
                emit(ctx, "%s = ROR32(%s, %s & 0xFF); cpu_update_nz(%s);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rd),
                     reg_c((u8)insn->rs), reg_c((u8)insn->rd));
                break;
            case THUMB_ALU_ADC:
                emit(ctx, "%s = %s + %s + CPU_C; /* TODO: ADC flags */",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rd), reg_c((u8)insn->rs));
                break;
            case THUMB_ALU_SBC:
                emit(ctx, "%s = %s - %s - !CPU_C; /* TODO: SBC flags */",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rd), reg_c((u8)insn->rs));
                break;
            case THUMB_ALU_MUL:
                emit(ctx, "%s *= %s; cpu_update_nz(%s);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rs), reg_c((u8)insn->rd));
                break;
        }
        break;

    case THUMB_HI_REG_OPS:
        switch (insn->hi_op) {
            case THUMB_HI_ADD:
                emit(ctx, "%s += %s;", reg_c((u8)insn->rd), reg_c((u8)insn->rs));
                break;
            case THUMB_HI_CMP:
                emit(ctx, "cpu_sub(NULL, %s, %s, true);",
                     reg_c((u8)insn->rd), reg_c((u8)insn->rs));
                break;
            case THUMB_HI_MOV:
                emit(ctx, "%s = %s;", reg_c((u8)insn->rd), reg_c((u8)insn->rs));
                break;
            default:
                break;
        }
        break;

    case THUMB_BX:
        if (insn->rs == REG_LR) {
            emit(ctx, "return; /* BX LR */");
        } else {
            emit(ctx, "cpu_bx(%s);", reg_c((u8)insn->rs));
        }
        break;

    case THUMB_PC_REL_LOAD: {
        u32 base = (addr + 4) & ~3u;
        u32 pool_addr = base + insn->imm;
        emit(ctx, "%s = bus_read32(0x%08Xu); /* [0x%08X] */",
             reg_c((u8)insn->rd), pool_addr, pool_addr);
        break;
    }

    case THUMB_LOAD_STORE_REG:
        if (insn->is_load) {
            emit(ctx, "%s = bus_read%s(%s + %s);",
                 reg_c((u8)insn->rd), insn->is_byte ? "8" : "32",
                 reg_c((u8)insn->rs), reg_c((u8)insn->rm));
        } else {
            emit(ctx, "bus_write%s(%s + %s, %s);",
                 insn->is_byte ? "8" : "32",
                 reg_c((u8)insn->rs), reg_c((u8)insn->rm), reg_c((u8)insn->rd));
        }
        break;

    case THUMB_LOAD_STORE_SIGN:
        if (!insn->is_load) {
            emit(ctx, "bus_write16(%s + %s, %s);",
                 reg_c((u8)insn->rs), reg_c((u8)insn->rm), reg_c((u8)insn->rd));
        } else if (insn->is_half && !insn->is_signed) {
            emit(ctx, "%s = bus_read16(%s + %s);",
                 reg_c((u8)insn->rd), reg_c((u8)insn->rs), reg_c((u8)insn->rm));
        } else if (insn->is_byte && insn->is_signed) {
            emit(ctx, "%s = (u32)(s32)(s8)bus_read8(%s + %s);",
                 reg_c((u8)insn->rd), reg_c((u8)insn->rs), reg_c((u8)insn->rm));
        } else {
            emit(ctx, "%s = (u32)(s32)(s16)bus_read16(%s + %s);",
                 reg_c((u8)insn->rd), reg_c((u8)insn->rs), reg_c((u8)insn->rm));
        }
        break;

    case THUMB_LOAD_STORE_IMM:
        if (insn->is_load) {
            emit(ctx, "%s = bus_read%s(%s + 0x%Xu);",
                 reg_c((u8)insn->rd), insn->is_byte ? "8" : "32",
                 reg_c((u8)insn->rs), insn->imm);
        } else {
            emit(ctx, "bus_write%s(%s + 0x%Xu, %s);",
                 insn->is_byte ? "8" : "32",
                 reg_c((u8)insn->rs), insn->imm, reg_c((u8)insn->rd));
        }
        break;

    case THUMB_LOAD_STORE_HALF:
        if (insn->is_load) {
            emit(ctx, "%s = bus_read16(%s + 0x%Xu);",
                 reg_c((u8)insn->rd), reg_c((u8)insn->rs), insn->imm);
        } else {
            emit(ctx, "bus_write16(%s + 0x%Xu, %s);",
                 reg_c((u8)insn->rs), insn->imm, reg_c((u8)insn->rd));
        }
        break;

    case THUMB_SP_REL_LOAD_STORE:
        if (insn->is_load) {
            emit(ctx, "%s = bus_read32(r[13] + 0x%Xu);",
                 reg_c((u8)insn->rd), insn->imm);
        } else {
            emit(ctx, "bus_write32(r[13] + 0x%Xu, %s);",
                 insn->imm, reg_c((u8)insn->rd));
        }
        break;

    case THUMB_LOAD_ADDR:
        if (insn->is_sp) {
            emit(ctx, "%s = r[13] + 0x%Xu;", reg_c((u8)insn->rd), insn->imm);
        } else {
            u32 base = (addr + 4) & ~3u;
            emit(ctx, "%s = 0x%08Xu; /* PC + 0x%X */",
                 reg_c((u8)insn->rd), base + insn->imm, insn->imm);
        }
        break;

    case THUMB_ADD_SP:
        if (insn->is_subtract) {
            emit(ctx, "r[13] -= 0x%Xu;", insn->imm);
        } else {
            emit(ctx, "r[13] += 0x%Xu;", insn->imm);
        }
        break;

    case THUMB_PUSH_POP:
        emit(ctx, "{");
        ctx->indent++;
        if (insn->is_load) {
            /* POP */
            for (int i = 0; i < 8; i++) {
                if (insn->rlist & (1 << i)) {
                    emit(ctx, "r[%d] = bus_read32(r[13]); r[13] += 4;", i);
                }
            }
            if (insn->pc_or_lr) {
                emit(ctx, "r[15] = bus_read32(r[13]); r[13] += 4;");
                emit(ctx, "return; /* POP {PC} */");
            }
        } else {
            /* PUSH */
            if (insn->pc_or_lr) {
                emit(ctx, "r[13] -= 4; bus_write32(r[13], r[14]);");
            }
            for (int i = 7; i >= 0; i--) {
                if (insn->rlist & (1 << i)) {
                    emit(ctx, "r[13] -= 4; bus_write32(r[13], r[%d]);", i);
                }
            }
        }
        ctx->indent--;
        emit(ctx, "}");
        break;

    case THUMB_MULTI_LOAD_STORE:
        emit(ctx, "{");
        ctx->indent++;
        emit(ctx, "u32 _addr = %s;", reg_c((u8)insn->rs));
        for (int i = 0; i < 8; i++) {
            if (!(insn->rlist & (1 << i))) continue;
            if (insn->is_load) {
                emit(ctx, "r[%d] = bus_read32(_addr); _addr += 4;", i);
            } else {
                emit(ctx, "bus_write32(_addr, r[%d]); _addr += 4;", i);
            }
        }
        emit(ctx, "%s = _addr;", reg_c((u8)insn->rs));
        ctx->indent--;
        emit(ctx, "}");
        break;

    case THUMB_COND_BRANCH: {
        u32 target = addr + 4 + (u32)insn->offset;
        const char* c = cond_to_c(insn->cond);
        if (c) {
            emit_cond_goto_or_tailcall(ctx, c, target);
        } else {
            emit_goto_or_tailcall(ctx, target);
        }
        break;
    }

    case THUMB_SWI:
        emit(ctx, "gba_swi(0x%02X);", insn->swi_num);
        break;

    case THUMB_BRANCH: {
        u32 target = addr + 4 + (u32)insn->offset;
        emit_goto_or_tailcall(ctx, target);
        break;
    }

    case THUMB_LONG_BRANCH:
        if (insn->is_suffix) {
            emit(ctx, "/* BL suffix - handled with prefix */");
        } else {
            emit(ctx, "/* BL prefix - see next instruction */");
        }
        break;

    case THUMB_UNDEFINED:
    default:
        emit_comment(ctx, "UNHANDLED: 0x%04X", insn->raw);
        emit(ctx, "cpu_undefined(0x%04Xu);", insn->raw);
        break;
    }
}

/* ---- File-level generation ---- */

TranslateCtx* translate_create(const GbaRom* rom, const AnalysisCtx* analysis, FILE* out) {
    TranslateCtx* ctx = calloc(1, sizeof(TranslateCtx));
    ctx->rom = rom;
    ctx->analysis = analysis;
    ctx->out = out;
    ctx->indent = 0;
    return ctx;
}

void translate_free(TranslateCtx* ctx) {
    free(ctx);
}

void translate_emit_header(TranslateCtx* ctx) {
    emit_raw(ctx, "/*\n");
    emit_raw(ctx, " * Auto-generated by gbarecomp\n");
    emit_raw(ctx, " * ROM: %s (%s)\n", ctx->rom->title, ctx->rom->game_code);
    emit_raw(ctx, " * DO NOT EDIT - this file is generated from the ROM binary\n");
    emit_raw(ctx, " */\n\n");
    emit_raw(ctx, "#include \"gba_runtime.h\"\n\n");
    emit_raw(ctx, "/* Forward declarations */\n");

    /* Forward-declare all functions */
    for (int i = 0; i < ctx->analysis->num_functions; i++) {
        const Function* f = &ctx->analysis->functions[i];
        emit_raw(ctx, "void func_%08X(void);\n", f->entry);
    }
    emit_raw(ctx, "\n");
}

void translate_emit_footer(TranslateCtx* ctx) {
    emit_raw(ctx, "\n/* Entry point */\n");
    emit_raw(ctx, "void game_entry(void) {\n");

    /* Find the entry function */
    u32 entry_raw = rom_read32(ctx->rom, GBA_ROM_START);
    ArmInsn entry = arm_decode(entry_raw);
    u32 target = GBA_ROM_START;
    if (entry.type == ARM_B) {
        target = GBA_ROM_START + 8 + (u32)entry.branch_offset;
    }
    emit_raw(ctx, "    func_%08X();\n", target);
    emit_raw(ctx, "}\n");
}

void translate_function(TranslateCtx* ctx, const Function* func) {
    emit_raw(ctx, "\n/* Function at 0x%08X (%s) */\n",
             func->entry, func->mode == CODE_ARM ? "ARM" : "Thumb");
    emit_raw(ctx, "void func_%08X(void) {\n", func->entry);
    ctx->indent = 1;

    /* Build local block address set for goto validation */
    ctx->local_blocks = (u32*)func->block_addrs;
    ctx->num_local_blocks = func->num_blocks;

    /* Translate each block */
    for (int b = 0; b < func->num_blocks; b++) {
        u32 block_addr = func->block_addrs[b];

        /* Find the block */
        BasicBlock* block = NULL;
        for (int j = 0; j < ctx->analysis->num_blocks; j++) {
            if (ctx->analysis->blocks[j].start == block_addr) {
                block = &ctx->analysis->blocks[j];
                break;
            }
        }
        if (!block) continue;

        /* Emit label for this block */
        emit_raw(ctx, "label_%08X: ;\n", block->start);

        if (block->mode == CODE_ARM) {
            for (u32 addr = block->start; addr < block->end; addr += 4) {
                u32 raw = rom_read32(ctx->rom, addr);
                ArmInsn insn = arm_decode(raw);
                translate_arm_insn(ctx, &insn, addr);
            }
        } else {
            u32 addr = block->start;
            while (addr < block->end) {
                u16 raw = rom_read16(ctx->rom, addr);
                ThumbInsn insn = thumb_decode(raw);

                /* Handle BL (two-part) */
                if (insn.type == THUMB_LONG_BRANCH && !insn.is_suffix) {
                    u16 next_raw = rom_read16(ctx->rom, addr + 2);
                    if (thumb_is_bl_suffix(next_raw)) {
                        ThumbInsn suffix = thumb_decode(next_raw);
                        u32 bl_target = (addr + 4) + (u32)insn.offset + suffix.imm;

                        char disasm_buf[256];
                        snprintf(disasm_buf, sizeof(disasm_buf), "BL 0x%08X", bl_target);
                        emit_comment(ctx, "0x%08X: %s", addr, disasm_buf);
                        emit(ctx, "r[14] = 0x%08Xu; /* return address */", (addr + 4) | 1);
                        emit(ctx, "func_%08X(); /* BL */", bl_target);
                        addr += 4;
                        continue;
                    }
                }

                translate_thumb_insn(ctx, &insn, addr);
                addr += 2;
            }
        }
        emit_raw(ctx, "\n");
    }

    ctx->indent = 0;
    ctx->local_blocks = NULL;
    ctx->num_local_blocks = 0;
    emit_raw(ctx, "}\n");
}

void translate_all(TranslateCtx* ctx) {
    translate_emit_header(ctx);

    for (int i = 0; i < ctx->analysis->num_functions; i++) {
        translate_function(ctx, &ctx->analysis->functions[i]);
    }

    translate_emit_footer(ctx);
}

/* ---- Multi-file translation ---- */

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define MKDIR(path) mkdir(path, 0755)
#endif

/* Collect all block start addresses that are branch targets but not function entries.
 * These need stub functions generated for them. */
static void collect_missing_targets(const AnalysisCtx* analysis,
                                    u32** out_stubs, int* out_count) {
    /* For each function, find branch targets that are NOT in that function's block list
     * and NOT already known function entries. These will become tail calls needing stubs. */
    int cap = 4096;
    u32* targets = malloc(sizeof(u32) * cap);
    int count = 0;

    for (int fi = 0; fi < analysis->num_functions; fi++) {
        const Function* func = &analysis->functions[fi];

        for (int bi = 0; bi < func->num_blocks; bi++) {
            /* Find this block */
            const BasicBlock* block = NULL;
            for (int j = 0; j < analysis->num_blocks; j++) {
                if (analysis->blocks[j].start == func->block_addrs[bi]) {
                    block = &analysis->blocks[j];
                    break;
                }
            }
            if (!block) continue;

            for (int s = 0; s < block->num_successors; s++) {
                u32 target = block->successors[s];
                if (target == 0) continue;

                /* Is it in this function's blocks? (would be a goto, not a call) */
                bool is_local = false;
                for (int k = 0; k < func->num_blocks; k++) {
                    if (func->block_addrs[k] == target) {
                        is_local = true;
                        break;
                    }
                }
                if (is_local) continue;

                /* Is it a known function entry? (already has implementation) */
                bool is_function = false;
                for (int j = 0; j < analysis->num_functions; j++) {
                    if (analysis->functions[j].entry == target) {
                        is_function = true;
                        break;
                    }
                }
                if (is_function) continue;

                /* Check for duplicates */
                bool dup = false;
                for (int j = 0; j < count; j++) {
                    if (targets[j] == target) { dup = true; break; }
                }
                if (dup) continue;

                if (count >= cap) {
                    cap *= 2;
                    targets = realloc(targets, sizeof(u32) * cap);
                }
                targets[count++] = target;
            }
        }
    }

    *out_stubs = targets;
    *out_count = count;
}

int translate_multi(const GbaRom* rom, const AnalysisCtx* analysis, const char* outdir) {
    MKDIR(outdir);

    int funcs_per_file = 100;
    int num_func_files = (analysis->num_functions + funcs_per_file - 1) / funcs_per_file;

    /* Collect block addresses that need stub functions */
    u32* stubs = NULL;
    int num_stubs = 0;
    int cap = 4096;
    collect_missing_targets(analysis, &stubs, &num_stubs);
    cap = num_stubs > cap ? num_stubs * 2 : cap;
    stubs = realloc(stubs, sizeof(u32) * cap);

    /* Also collect BL targets not in function list */
    for (int i = 0; i < analysis->num_blocks; i++) {
        const BasicBlock* block = &analysis->blocks[i];
        if (block->mode == CODE_THUMB) {
            for (u32 addr = block->start; addr + 4 <= block->end; addr += 2) {
                u16 raw = rom_read16(rom, addr);
                if (thumb_is_bl_prefix(raw)) {
                    u16 next = rom_read16(rom, addr + 2);
                    if (thumb_is_bl_suffix(next)) {
                        ThumbInsn prefix = thumb_decode(raw);
                        ThumbInsn suffix = thumb_decode(next);
                        u32 bl_target = (addr + 4) + (u32)prefix.offset + suffix.imm;

                        bool known = false;
                        for (int j = 0; j < analysis->num_functions && !known; j++)
                            if (analysis->functions[j].entry == bl_target) known = true;
                        for (int j = 0; j < num_stubs && !known; j++)
                            if (stubs[j] == bl_target) known = true;

                        if (!known) {
                            if (num_stubs >= cap) { cap *= 2; stubs = realloc(stubs, sizeof(u32) * cap); }
                            stubs[num_stubs++] = bl_target;
                        }
                        addr += 2;
                    }
                }
            }
        }
        if (block->mode == CODE_ARM) {
            for (u32 addr = block->start; addr < block->end; addr += 4) {
                u32 raw32 = rom_read32(rom, addr);
                ArmInsn insn = arm_decode(raw32);
                if (insn.type == ARM_BL) {
                    u32 bl_target = addr + 8 + (u32)insn.branch_offset;
                    bool known = false;
                    for (int j = 0; j < analysis->num_functions && !known; j++)
                        if (analysis->functions[j].entry == bl_target) known = true;
                    for (int j = 0; j < num_stubs && !known; j++)
                        if (stubs[j] == bl_target) known = true;
                    if (!known) {
                        if (num_stubs >= cap) { cap *= 2; stubs = realloc(stubs, sizeof(u32) * cap); }
                        stubs[num_stubs++] = bl_target;
                    }
                }
            }
        }
    }
    printf("[translate] %d stub functions needed\n", num_stubs);

    /* 1. Write game.h - forward declarations (including stubs) */
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/game.h", outdir);
        FILE* f = fopen(path, "w");
        if (!f) return -1;

        fprintf(f, "/* Auto-generated by gbarecomp - %s (%s) */\n", rom->title, rom->game_code);
        fprintf(f, "#ifndef GAME_H\n#define GAME_H\n\n");
        fprintf(f, "#include \"gba_runtime.h\"\n\n");
        for (int i = 0; i < analysis->num_functions; i++) {
            fprintf(f, "void func_%08X(void);\n", analysis->functions[i].entry);
        }
        for (int i = 0; i < num_stubs; i++) {
            fprintf(f, "void func_%08X(void);\n", stubs[i]);
        }
        fprintf(f, "\nvoid game_entry(void);\n");
        fprintf(f, "\n#endif /* GAME_H */\n");
        fclose(f);
    }

    /* 2. Write function files (funcs_000.c, funcs_001.c, ...) */
    for (int file_idx = 0; file_idx < num_func_files; file_idx++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/funcs_%03d.c", outdir, file_idx);
        FILE* f = fopen(path, "w");
        if (!f) return -1;

        fprintf(f, "/* Auto-generated by gbarecomp - %s (%s) - Part %d/%d */\n",
                rom->title, rom->game_code, file_idx + 1, num_func_files);
        fprintf(f, "#include \"game.h\"\n\n");

        TranslateCtx* ctx = translate_create(rom, analysis, f);

        int start = file_idx * funcs_per_file;
        int end = start + funcs_per_file;
        if (end > analysis->num_functions) end = analysis->num_functions;

        for (int i = start; i < end; i++) {
            translate_function(ctx, &analysis->functions[i]);
        }

        translate_free(ctx);
        fclose(f);
    }

    /* 3. Write stub files with real translated code where possible */
    {
        int stubs_per_file = 200;
        int num_stub_files = (num_stubs + stubs_per_file - 1) / stubs_per_file;

        for (int sf = 0; sf < num_stub_files; sf++) {
            char path[512];
            snprintf(path, sizeof(path), "%s/stubs_%03d.c", outdir, sf);
            FILE* f = fopen(path, "w");
            if (!f) continue;

            fprintf(f, "/* Stub functions - part %d/%d */\n", sf + 1, num_stub_files);
            fprintf(f, "#include \"game.h\"\n\n");

            TranslateCtx* stub_ctx = translate_create(rom, analysis, f);

            int start_idx = sf * stubs_per_file;
            int end_idx = start_idx + stubs_per_file;
            if (end_idx > num_stubs) end_idx = num_stubs;

            for (int i = start_idx; i < end_idx; i++) {
                u32 addr = stubs[i];

                /* Try to find the block at this address and translate it */
                bool found = false;
                for (int j = 0; j < analysis->num_blocks; j++) {
                    if (analysis->blocks[j].start == addr) {
                        /* Build a mini-function with this block and reachable successors */
                        Function temp_func;
                        memset(&temp_func, 0, sizeof(temp_func));
                        temp_func.entry = addr;
                        temp_func.mode = analysis->blocks[j].mode;

                        int temp_cap = 32; /* Limit block count per stub */
                        temp_func.block_addrs = malloc(sizeof(u32) * temp_cap);
                        temp_func.num_blocks = 0;

                        /* BFS: collect this block and nearby reachable non-function blocks */
                        u32 queue[32];
                        int qh = 0, qt = 0;
                        queue[qt++] = addr;

                        while (qh < qt && temp_func.num_blocks < temp_cap) {
                            u32 cur = queue[qh++];

                            /* Skip if already added */
                            bool dup = false;
                            for (int k = 0; k < temp_func.num_blocks; k++) {
                                if (temp_func.block_addrs[k] == cur) { dup = true; break; }
                            }
                            if (dup) continue;

                            /* Find this block in analysis */
                            const BasicBlock* blk = NULL;
                            for (int k = 0; k < analysis->num_blocks; k++) {
                                if (analysis->blocks[k].start == cur) {
                                    blk = &analysis->blocks[k];
                                    break;
                                }
                            }
                            if (!blk) continue;

                            temp_func.block_addrs[temp_func.num_blocks++] = cur;

                            /* Queue successors that aren't function entries */
                            for (int s = 0; s < blk->num_successors && qt < 32; s++) {
                                u32 succ = blk->successors[s];
                                if (succ == 0) continue;
                                bool is_func = false;
                                for (int k = 0; k < analysis->num_functions; k++) {
                                    if (analysis->functions[k].entry == succ) {
                                        is_func = true;
                                        break;
                                    }
                                }
                                if (!is_func) {
                                    queue[qt++] = succ;
                                }
                            }
                        }

                        if (temp_func.num_blocks > 0) {
                            translate_function(stub_ctx, &temp_func);
                            found = true;
                        }
                        free(temp_func.block_addrs);
                        break;
                    }
                }

                if (!found) {
                    fprintf(f, "void func_%08X(void) { /* no block found */ }\n", addr);
                }
            }

            translate_free(stub_ctx);
            fclose(f);
        }

        /* Update CMakeLists to use stub files */
        /* (handled below in CMakeLists generation) */
    }

    /* 4. Write game_entry.c with BX dispatch table */
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/game_entry.c", outdir);
        FILE* f = fopen(path, "w");
        if (!f) return -1;

        fprintf(f, "/* Auto-generated by gbarecomp - %s (%s) */\n", rom->title, rom->game_code);
        fprintf(f, "#include \"game.h\"\n");
        fprintf(f, "#include <stdio.h>\n\n");

        TranslateCtx* ctx = translate_create(rom, analysis, f);
        translate_emit_footer(ctx);
        translate_free(ctx);

        /* Generate BX dispatch table */
        fprintf(f, "\n/* BX dispatch table - maps runtime addresses to recompiled functions */\n");
        fprintf(f, "typedef struct { u32 addr; void (*func)(void); } BxEntry;\n\n");
        fprintf(f, "static const BxEntry bx_table[] = {\n");
        for (int i = 0; i < analysis->num_functions; i++) {
            u32 entry = analysis->functions[i].entry;
            /* Thumb functions have bit 0 set in BX target */
            u32 bx_addr = entry;
            if (analysis->functions[i].mode == CODE_THUMB) {
                bx_addr |= 1;
            }
            fprintf(f, "    { 0x%08Xu, func_%08X },\n", bx_addr, entry);
        }
        fprintf(f, "};\n");
        fprintf(f, "static const int bx_table_size = %d;\n\n", analysis->num_functions);

        /* Binary search BX dispatcher */
        fprintf(f, "void cpu_bx(u32 target) {\n");
        fprintf(f, "    /* Binary search the dispatch table */\n");
        fprintf(f, "    int lo = 0, hi = bx_table_size - 1;\n");
        fprintf(f, "    while (lo <= hi) {\n");
        fprintf(f, "        int mid = (lo + hi) / 2;\n");
        fprintf(f, "        if (bx_table[mid].addr == target) {\n");
        fprintf(f, "            bx_table[mid].func();\n");
        fprintf(f, "            return;\n");
        fprintf(f, "        } else if (bx_table[mid].addr < target) {\n");
        fprintf(f, "            lo = mid + 1;\n");
        fprintf(f, "        } else {\n");
        fprintf(f, "            hi = mid - 1;\n");
        fprintf(f, "        }\n");
        fprintf(f, "    }\n");
        fprintf(f, "    /* NULL targets */\n");
        fprintf(f, "    if (target == 0 || target == 1) return;\n");
        fprintf(f, "    /* RAM targets: run via mGBA interpreter */\n");
        fprintf(f, "    if ((target >> 24) == 0x02 || (target >> 24) == 0x03) {\n");
        fprintf(f, "        run_iwram_function(target);\n");
        fprintf(f, "        return;\n");
        fprintf(f, "    }\n");
        fprintf(f, "    static int _bxmiss = 0;\n");
        fprintf(f, "    if (++_bxmiss <= 20) fprintf(stderr, \"[bx] No function for 0x%%08X\\n\", target);\n");
        fprintf(f, "}\n\n");

        /* Interception setup - populates the function table for interception.c */
        fprintf(f, "\n/* Set up function interception table from BX dispatch entries */\n");
        fprintf(f, "typedef struct { unsigned int addr; void (*func)(void); } FuncEntry;\n");
        fprintf(f, "extern void interception_init(FuncEntry* table, int size);\n\n");
        fprintf(f, "void interception_setup_from_bx_table(void) {\n");
        fprintf(f, "    /* Reuse the BX table for interception - only ROM functions */\n");
        fprintf(f, "    static FuncEntry intercept_table[] = {\n");
        for (int i = 0; i < analysis->num_functions; i++) {
            u32 entry = analysis->functions[i].entry;
            /* Only intercept ROM functions (not IWRAM) */
            if ((entry >> 24) == 0x08) {
                fprintf(f, "        { 0x%08Xu, func_%08X },\n", entry, entry);
            }
        }
        fprintf(f, "    };\n");
        fprintf(f, "    interception_init(intercept_table, %d);\n",
                analysis->num_functions); /* approximate - some are IWRAM */
        fprintf(f, "}\n\n");

        /* Add main() - after mGBA init, call the main game function directly */
        fprintf(f, "int main(int argc, char* argv[]) {\n");
        fprintf(f, "    const char* rom_path = argc > 1 ? argv[1] : \"game.gba\";\n");
        fprintf(f, "    gba_init(rom_path); /* mGBA runs init, then hands off */\n");
        fprintf(f, "    /* Call the main game function directly (skip crt0 - mGBA did that) */\n");

        /* Find the main game function by looking at the call chain:
         * crt0 -> AgbMain -> quick_init -> MAIN_GAME_FUNCTION */
        u32 main_func = 0;
        for (int i = 0; i < analysis->num_functions; i++) {
            /* The main game function is the largest one discovered from entry point */
            if (analysis->functions[i].num_blocks > 100) {
                main_func = analysis->functions[i].entry;
                break;
            }
        }
        if (main_func) {
            fprintf(f, "    func_%08X(); /* main game function */\n", main_func);
        } else {
            fprintf(f, "    game_entry(); /* fallback to full entry */\n");
        }

        fprintf(f, "    gba_shutdown();\n");
        fprintf(f, "    return 0;\n");
        fprintf(f, "}\n");
        fclose(f);
    }

    /* 4. Write CMakeLists.txt */
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/CMakeLists.txt", outdir);
        FILE* f = fopen(path, "w");
        if (!f) return -1;

        fprintf(f, "# Auto-generated by gbarecomp\n");
        fprintf(f, "cmake_minimum_required(VERSION 3.16)\n");
        fprintf(f, "project(%s C CXX)\n\n", rom->game_code);
        fprintf(f, "set(CMAKE_C_STANDARD 11)\n");
        fprintf(f, "set(CMAKE_C_STANDARD_REQUIRED ON)\n\n");
        fprintf(f, "if(MSVC)\n");
        fprintf(f, "    add_compile_options(/W2 /wd4244 /wd4146 /wd4018 /wd4047 /wd4024 /O2)\n");
        fprintf(f, "else()\n");
        fprintf(f, "    add_compile_options(-Wall -Wno-unused-label -Wno-pointer-to-int-cast -O2)\n");
        fprintf(f, "endif()\n\n");
        fprintf(f, "# ImGui (Dear ImGui with SDL2+SDL_Renderer backend)\n");
        fprintf(f, "set(IMGUI_DIR \"D:/recomp/gba/imgui\")\n");
        fprintf(f, "set(IMGUI_SOURCES\n");
        fprintf(f, "    ${IMGUI_DIR}/imgui.cpp\n");
        fprintf(f, "    ${IMGUI_DIR}/imgui_draw.cpp\n");
        fprintf(f, "    ${IMGUI_DIR}/imgui_tables.cpp\n");
        fprintf(f, "    ${IMGUI_DIR}/imgui_widgets.cpp\n");
        fprintf(f, "    ${IMGUI_DIR}/imgui_demo.cpp\n");
        fprintf(f, "    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp\n");
        fprintf(f, "    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp\n");
        fprintf(f, ")\n\n");
        fprintf(f, "set(SOURCES\n");
        fprintf(f, "    game_entry.c\n");
        fprintf(f, "    runtime.c\n");
        fprintf(f, "    display.c\n");
        fprintf(f, "    menu.cpp\n");
        fprintf(f, "    interception.c\n");
        {
            int stubs_per_file = 200;
            int nsf = (num_stubs + stubs_per_file - 1) / stubs_per_file;
            for (int i = 0; i < nsf; i++) {
                fprintf(f, "    stubs_%03d.c\n", i);
            }
        }
        for (int i = 0; i < num_func_files; i++) {
            fprintf(f, "    funcs_%03d.c\n", i);
        }
        fprintf(f, ")\n\n");
        fprintf(f, "add_executable(%s ${SOURCES} ${IMGUI_SOURCES})\n", rom->game_code);
        fprintf(f, "target_include_directories(%s PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)\n\n", rom->game_code);
        /* SDL2 integration */
        fprintf(f, "# SDL2 display\n");
        fprintf(f, "find_package(SDL2 CONFIG)\n");
        fprintf(f, "if(SDL2_FOUND)\n");
        fprintf(f, "    target_link_libraries(%s PRIVATE SDL2::SDL2 SDL2::SDL2main)\n", rom->game_code);
        fprintf(f, "else()\n");
        fprintf(f, "    # Fallback: manual SDL2 paths\n");
        fprintf(f, "    target_include_directories(%s PRIVATE C:/vcpkg/installed/x64-windows/include)\n", rom->game_code);
        fprintf(f, "    target_link_directories(%s PRIVATE C:/vcpkg/installed/x64-windows/lib)\n", rom->game_code);
        fprintf(f, "    target_link_libraries(%s PRIVATE SDL2 SDL2main)\n", rom->game_code);
        fprintf(f, "endif()\n\n");
        /* Stack size for deep call chains */
        fprintf(f, "if(MSVC)\n");
        fprintf(f, "    target_link_options(%s PRIVATE /STACK:16777216 /SUBSYSTEM:CONSOLE)\n", rom->game_code);
        fprintf(f, "endif()\n");
        fclose(f);
    }

    /* 5. Write include shims pointing to real headers */
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/gba_runtime.h", outdir);
        FILE* f = fopen(path, "w");
        if (f) {
            fprintf(f, "/* Include shim - points to gbarecomp headers */\n");
            fprintf(f, "#include \"../../include/gba/gba_runtime.h\"\n");
            fclose(f);
        }
        /* types.h shim (needed by display.h) */
        snprintf(path, sizeof(path), "%s/gba", outdir);
        MKDIR(path);
        snprintf(path, sizeof(path), "%s/gba/types.h", outdir);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "#include \"../../../include/gba/types.h\"\n");
            fclose(f);
        }
        snprintf(path, sizeof(path), "%s/gba/display.h", outdir);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "#include \"../../../include/gba/display.h\"\n");
            fclose(f);
        }
    }

    free(stubs);
    return num_func_files + 3 + (num_stubs > 0 ? 1 : 0);
}
