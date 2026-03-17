#ifndef GBA_DISASM_H
#define GBA_DISASM_H

#include "gba/types.h"
#include "gba/arm.h"
#include "gba/thumb.h"
#include "gba/rom.h"

/* Format a decoded ARM instruction into a human-readable string.
 * `addr` is the address of the instruction (used for branch target calculation).
 * Returns number of chars written (excluding null terminator). */
int disasm_arm(const ArmInsn* insn, u32 addr, char* buf, size_t buf_size);

/* Format a decoded Thumb instruction into a human-readable string.
 * `addr` is the address of the instruction.
 * Returns number of chars written (excluding null terminator). */
int disasm_thumb(const ThumbInsn* insn, u32 addr, char* buf, size_t buf_size);

/* Disassemble a range of ROM as ARM code, printing to stdout.
 * `start` and `end` are ROM-region addresses (0x08xxxxxx). */
void disasm_arm_range(const GbaRom* rom, u32 start, u32 end);

/* Disassemble a range of ROM as Thumb code, printing to stdout. */
void disasm_thumb_range(const GbaRom* rom, u32 start, u32 end);

#endif /* GBA_DISASM_H */
