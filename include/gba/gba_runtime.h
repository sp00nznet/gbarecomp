/*
 * GBA Runtime Library Header
 * Provides the CPU state, memory bus, and hardware interface
 * that recompiled game code links against.
 *
 * This will eventually wrap libmgba. For now it's a standalone
 * implementation sufficient to compile and test generated code.
 */

#ifndef GBA_RUNTIME_H
#define GBA_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* ---- CPU State ---- */

/* ARM7TDMI register file: r0-r15 (r13=SP, r14=LR, r15=PC) */
extern u32 r[16];

/* Condition flags */
extern bool CPU_N;  /* Negative */
extern bool CPU_Z;  /* Zero */
extern bool CPU_C;  /* Carry */
extern bool CPU_V;  /* Overflow */

/* ---- Utility macros ---- */

static inline u32 ROR32(u32 val, u32 amount) {
    amount &= 31;
    if (amount == 0) return val;
    return (val >> amount) | (val << (32 - amount));
}

static inline u32 RRX(u32 val) {
    return (CPU_C ? 0x80000000u : 0) | (val >> 1);
}

/* ---- Flag updates ---- */

static inline void cpu_update_nz(u32 result) {
    CPU_N = (result >> 31) != 0;
    CPU_Z = (result == 0);
}

/* ADD with flag update. If dest is NULL, only flags are updated (CMN). */
static inline void cpu_add(u32* dest, u32 a, u32 b, bool set_flags) {
    u64 result64 = (u64)a + (u64)b;
    u32 result = (u32)result64;
    if (dest) *dest = result;
    if (set_flags) {
        CPU_N = (result >> 31) != 0;
        CPU_Z = (result == 0);
        CPU_C = (result64 >> 32) != 0;
        CPU_V = ((~(a ^ b) & (a ^ result)) >> 31) != 0;
    }
}

/* SUB with flag update. If dest is NULL, only flags are updated (CMP). */
static inline void cpu_sub(u32* dest, u32 a, u32 b, bool set_flags) {
    u32 result = a - b;
    if (dest) *dest = result;
    if (set_flags) {
        CPU_N = (result >> 31) != 0;
        CPU_Z = (result == 0);
        CPU_C = (a >= b); /* borrow = !carry for ARM */
        CPU_V = (((a ^ b) & (a ^ result)) >> 31) != 0;
    }
}

static inline void cpu_update_flags_adc(u32 result, u32 a, u32 b) {
    CPU_N = (result >> 31) != 0;
    CPU_Z = (result == 0);
    /* Simplified - full ADC flag calc needs carry input */
}

static inline void cpu_update_flags_sub(u32 result, u32 a, u32 b) {
    CPU_N = (result >> 31) != 0;
    CPU_Z = (result == 0);
    CPU_C = (a >= b);
    CPU_V = (((a ^ b) & (a ^ result)) >> 31) != 0;
}

/* ---- CPSR/SPSR ---- */

u32  cpu_get_cpsr(void);
void cpu_set_cpsr(u32 value, u32 mask);
u32  cpu_get_spsr(void);
void cpu_set_spsr(u32 value, u32 mask);

/* ---- Memory Bus ---- */

/* GBA memory map (active ranges):
 *   0x00000000 - BIOS (16KB)
 *   0x02000000 - EWRAM (256KB)
 *   0x03000000 - IWRAM (32KB)
 *   0x04000000 - I/O Registers
 *   0x05000000 - Palette RAM (1KB)
 *   0x06000000 - VRAM (96KB)
 *   0x07000000 - OAM (1KB)
 *   0x08000000 - ROM (up to 32MB, mirrored at 0x09-0x0D)
 *   0x0E000000 - SRAM/Flash
 */

u32 bus_read32(u32 addr);
u16 bus_read16(u32 addr);
u8  bus_read8(u32 addr);

void bus_write32(u32 addr, u32 value);
void bus_write16(u32 addr, u16 value);
void bus_write8(u32 addr, u8 value);

/* ---- Hardware ---- */

/* Software interrupt (BIOS call) */
void gba_swi(u32 number);

/* Indirect branch (BX to register value) - runtime dispatch */
void cpu_bx(u32 target);

/* Run a function in IWRAM/EWRAM via mGBA's interpreter fallback */
void run_iwram_function(u32 target);

/* Undefined instruction trap */
void cpu_undefined(u32 insn);

/* ---- PPU ---- */

void ppu_render_scanline(void);
bool ppu_in_vblank(void);
bool ppu_in_hblank(void);

/* ---- APU ---- */

void apu_step(u32 cycles);

/* ---- DMA ---- */

void dma_check(void);

/* ---- Timers ---- */

void timer_step(u32 cycles);

/* ---- Interrupts ---- */

void irq_check(void);

/* ---- Main loop ---- */

/* Called once per frame by the recompiled game's main loop.
 * Handles PPU rendering, audio mixing, input polling, etc. */
void gba_frame(void);

/* Initialize the GBA runtime (memory, PPU, APU, etc.) */
void gba_init(const char* rom_path);

/* Shutdown */
void gba_shutdown(void);

#endif /* GBA_RUNTIME_H */
