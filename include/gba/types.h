#ifndef GBA_TYPES_H
#define GBA_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* ARM condition codes (bits 31-28 of every ARM instruction) */
typedef enum {
    COND_EQ = 0x0,  /* Equal (Z set) */
    COND_NE = 0x1,  /* Not equal (Z clear) */
    COND_CS = 0x2,  /* Carry set / unsigned higher or same */
    COND_CC = 0x3,  /* Carry clear / unsigned lower */
    COND_MI = 0x4,  /* Minus / negative (N set) */
    COND_PL = 0x5,  /* Plus / positive or zero (N clear) */
    COND_VS = 0x6,  /* Overflow (V set) */
    COND_VC = 0x7,  /* No overflow (V clear) */
    COND_HI = 0x8,  /* Unsigned higher (C set and Z clear) */
    COND_LS = 0x9,  /* Unsigned lower or same (C clear or Z set) */
    COND_GE = 0xA,  /* Signed greater than or equal */
    COND_LT = 0xB,  /* Signed less than */
    COND_GT = 0xC,  /* Signed greater than */
    COND_LE = 0xD,  /* Signed less than or equal */
    COND_AL = 0xE,  /* Always (unconditional) */
    COND_NV = 0xF,  /* Never (ARMv1-v3) / special (ARMv4+) */
} ArmCondition;

/* ARM shift types */
typedef enum {
    SHIFT_LSL = 0,  /* Logical shift left */
    SHIFT_LSR = 1,  /* Logical shift right */
    SHIFT_ASR = 2,  /* Arithmetic shift right */
    SHIFT_ROR = 3,  /* Rotate right (RRX when amount=0) */
} ShiftType;

/* ARM registers */
typedef enum {
    REG_R0 = 0, REG_R1, REG_R2, REG_R3,
    REG_R4, REG_R5, REG_R6, REG_R7,
    REG_R8, REG_R9, REG_R10, REG_R11,
    REG_R12, REG_SP, REG_LR, REG_PC,
} ArmRegister;

/* GBA memory regions */
#define GBA_BIOS_START   0x00000000
#define GBA_BIOS_SIZE    0x00004000
#define GBA_EWRAM_START  0x02000000
#define GBA_EWRAM_SIZE   0x00040000
#define GBA_IWRAM_START  0x03000000
#define GBA_IWRAM_SIZE   0x00008000
#define GBA_IO_START     0x04000000
#define GBA_IO_SIZE      0x00000400
#define GBA_PAL_START    0x05000000
#define GBA_PAL_SIZE     0x00000400
#define GBA_VRAM_START   0x06000000
#define GBA_VRAM_SIZE    0x00018000
#define GBA_OAM_START    0x07000000
#define GBA_OAM_SIZE     0x00000400
#define GBA_ROM_START    0x08000000
#define GBA_ROM_MAX_SIZE 0x02000000

/* ROM entry point is always 0x08000000 after header */
#define GBA_ENTRY_POINT  0x08000000

#endif /* GBA_TYPES_H */
