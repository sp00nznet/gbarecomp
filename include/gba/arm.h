#ifndef GBA_ARM_H
#define GBA_ARM_H

#include "gba/types.h"

typedef enum {
    /* Data processing */
    ARM_AND, ARM_EOR, ARM_SUB, ARM_RSB,
    ARM_ADD, ARM_ADC, ARM_SBC, ARM_RSC,
    ARM_TST, ARM_TEQ, ARM_CMP, ARM_CMN,
    ARM_ORR, ARM_MOV, ARM_BIC, ARM_MVN,

    /* Multiply */
    ARM_MUL, ARM_MLA,

    /* Multiply long */
    ARM_UMULL, ARM_UMLAL, ARM_SMULL, ARM_SMLAL,

    /* Branch */
    ARM_B, ARM_BL,

    /* Branch exchange */
    ARM_BX,

    /* Single data transfer */
    ARM_LDR, ARM_STR,

    /* Halfword / signed transfer */
    ARM_LDRH, ARM_STRH, ARM_LDRSB, ARM_LDRSH,

    /* Block data transfer */
    ARM_LDM, ARM_STM,

    /* Swap */
    ARM_SWP, ARM_SWPB,

    /* Status register */
    ARM_MRS, ARM_MSR,

    /* Software interrupt */
    ARM_SWI,

    /* Undefined */
    ARM_UNDEFINED,

    ARM_INSN_TYPE_COUNT,
} ArmInsnType;

typedef struct {
    ArmInsnType type;
    ArmCondition cond;

    /* Common register fields */
    u8 rd;          /* Destination register (also RdLo for long multiply) */
    u8 rn;          /* First operand / base register (also RdHi for long multiply) */
    u8 rm;          /* Second operand register */
    u8 rs;          /* Shift register / multiply register */

    /* Flags */
    bool s;         /* Set condition codes */
    bool i;         /* Immediate operand (data processing) / immediate offset (transfer) */
    bool p;         /* Pre/post indexing */
    bool u;         /* Up/down (add/subtract offset) */
    bool b;         /* Byte/word transfer */
    bool w;         /* Write-back */
    bool l;         /* Load/store for transfers, link for branch */

    /* Data processing operand2 */
    ShiftType shift_type;
    u8 shift_amount;        /* Immediate shift amount (0-31) */
    bool shift_reg;         /* true = shift by register (rs), false = shift by immediate */
    u32 imm;                /* Immediate value (rotated for data processing) */
    u8 rotate;              /* Rotation amount for immediate (raw 4-bit value) */

    /* Branch */
    s32 branch_offset;      /* Sign-extended and shifted branch offset */

    /* Block transfer */
    u16 reg_list;           /* Register list bitmask for LDM/STM */

    /* SWI */
    u32 swi_number;         /* 24-bit SWI comment field */

    /* MSR field mask */
    u8 msr_mask;            /* Field mask bits: [c, x, s, f] in bits [0:3] */
    bool r;                 /* PSR select: 0 = CPSR, 1 = SPSR */

    /* Raw instruction */
    u32 raw;
} ArmInsn;

ArmInsn arm_decode(u32 insn);

const char* arm_insn_type_name(ArmInsnType type);
const char* arm_cond_name(ArmCondition cond);
const char* arm_reg_name(u8 reg);
const char* arm_shift_name(ShiftType type);

#endif /* GBA_ARM_H */
