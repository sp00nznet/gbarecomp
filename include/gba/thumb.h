#ifndef GBA_THUMB_H
#define GBA_THUMB_H

#include "gba/types.h"

typedef enum {
    THUMB_MOVE_SHIFTED,        /* Format 1: LSL/LSR/ASR Rd, Rs, #imm5 */
    THUMB_ADD_SUB,             /* Format 2: ADD/SUB Rd, Rs, Rn/#imm3 */
    THUMB_MOV_CMP_ADD_SUB_IMM, /* Format 3: MOV/CMP/ADD/SUB Rd, #imm8 */
    THUMB_ALU_OPS,             /* Format 4: ALU operations on low regs */
    THUMB_HI_REG_OPS,         /* Format 5: ADD/CMP/MOV with high regs */
    THUMB_BX,                  /* Format 5: BX Rs */
    THUMB_PC_REL_LOAD,         /* Format 6: LDR Rd, [PC, #imm8*4] */
    THUMB_LOAD_STORE_REG,      /* Format 7: LDR/STR Rd, [Rb, Ro] */
    THUMB_LOAD_STORE_SIGN,     /* Format 8: STRH/LDRH/LDRSB/LDRSH */
    THUMB_LOAD_STORE_IMM,      /* Format 9: LDR/STR Rd, [Rb, #imm5] */
    THUMB_LOAD_STORE_HALF,     /* Format 10: LDRH/STRH Rd, [Rb, #imm5*2] */
    THUMB_SP_REL_LOAD_STORE,   /* Format 11: LDR/STR Rd, [SP, #imm8*4] */
    THUMB_LOAD_ADDR,           /* Format 12: ADD Rd, PC/SP, #imm8*4 */
    THUMB_ADD_SP,              /* Format 13: ADD SP, #imm7*4 (signed) */
    THUMB_PUSH_POP,            /* Format 14: PUSH/POP {Rlist} */
    THUMB_MULTI_LOAD_STORE,    /* Format 15: LDMIA/STMIA Rb!, {Rlist} */
    THUMB_COND_BRANCH,         /* Format 16: B<cond> label */
    THUMB_SWI,                 /* Format 17: SWI imm8 */
    THUMB_BRANCH,              /* Format 18: B label (unconditional) */
    THUMB_LONG_BRANCH,         /* Format 19: BL (prefix or suffix half) */
    THUMB_UNDEFINED,
} ThumbInsnType;

/* ALU operation sub-opcodes for Format 4 */
typedef enum {
    THUMB_ALU_AND = 0x0,
    THUMB_ALU_EOR = 0x1,
    THUMB_ALU_LSL = 0x2,
    THUMB_ALU_LSR = 0x3,
    THUMB_ALU_ASR = 0x4,
    THUMB_ALU_ADC = 0x5,
    THUMB_ALU_SBC = 0x6,
    THUMB_ALU_ROR = 0x7,
    THUMB_ALU_TST = 0x8,
    THUMB_ALU_NEG = 0x9,
    THUMB_ALU_CMP = 0xA,
    THUMB_ALU_CMN = 0xB,
    THUMB_ALU_ORR = 0xC,
    THUMB_ALU_MUL = 0xD,
    THUMB_ALU_BIC = 0xE,
    THUMB_ALU_MVN = 0xF,
} ThumbAluOp;

/* Hi-register operation sub-opcodes for Format 5 */
typedef enum {
    THUMB_HI_ADD = 0,
    THUMB_HI_CMP = 1,
    THUMB_HI_MOV = 2,
    THUMB_HI_BX  = 3,
} ThumbHiOp;

typedef struct {
    ThumbInsnType type;
    u16           raw;          /* original 16-bit instruction word */

    /* Registers */
    ArmRegister   rd;           /* destination / first operand */
    ArmRegister   rs;           /* source / second operand (also called Rn/Rb) */
    ArmRegister   rm;           /* third operand where applicable */

    /* Immediates & offsets */
    u32           imm;          /* unsigned immediate value */
    s32           offset;       /* signed offset (branches, etc.) */

    /* Condition code (Format 16 conditional branches) */
    ArmCondition  cond;

    /* Shift */
    ShiftType     shift_type;
    u8            shift_amount;

    /* ALU / hi-reg sub-operation */
    ThumbAluOp    alu_op;
    ThumbHiOp     hi_op;

    /* Flags */
    bool          is_load;      /* true = load, false = store */
    bool          is_byte;      /* true = byte transfer */
    bool          is_signed;    /* true = sign-extended load */
    bool          is_half;      /* true = halfword transfer */
    bool          is_imm;       /* true = immediate operand, false = register */
    bool          is_subtract;  /* ADD_SUB / ADD_SP: true = subtract */
    bool          is_sp;        /* LOAD_ADDR: true = SP base, false = PC base */
    bool          is_suffix;    /* LONG_BRANCH: true = suffix (second) half */
    bool          pc_or_lr;     /* PUSH_POP: true = store LR / load PC */

    /* Register list (Format 14, 15) */
    u8            rlist;        /* bits 0-7 map to r0-r7 */

    /* SWI comment field */
    u8            swi_num;
} ThumbInsn;

/* Decode a 16-bit Thumb instruction into a ThumbInsn. */
ThumbInsn thumb_decode(u16 insn);

/* Return a human-readable name for a ThumbInsnType. */
const char* thumb_insn_type_name(ThumbInsnType type);

/* Return true if the instruction is any kind of branch (B, BL, BX, cond branch). */
bool thumb_is_branch(const ThumbInsn* insn);

/* Return true if this 16-bit word is the first half of a BL (bits[15:11] == 11110). */
bool thumb_is_bl_prefix(u16 insn);

/* Return true if this 16-bit word is the second half of a BL (bits[15:11] == 11111). */
bool thumb_is_bl_suffix(u16 insn);

#endif /* GBA_THUMB_H */
