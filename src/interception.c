/*
 * Function Interception - N64Recomp style
 *
 * mGBA's CPU runs the game, but when it enters a ROM function that
 * we've recompiled, we swap to our C code instead. This gives us
 * native speed for game logic while mGBA handles the hardware and
 * any IWRAM/BIOS code.
 *
 * Architecture:
 * - A hash table maps ROM addresses to recompiled function pointers
 * - After each mGBA CPU step, we check if PC matches a known function
 * - If so: sync mGBA regs -> our regs, call C func, sync back, skip interpreter
 * - The game's main loop, task dispatcher, and IWRAM code run via mGBA
 * - Individual game logic functions (movement, AI, rendering) run as native C
 */

#include "gba/gba_runtime.h"

#include <mgba/flags.h>
#include <mgba/core/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/arm/arm.h>

/* From arm.c - injects instruction into pipeline */
extern void ARMRunFake(struct ARMCore* cpu, uint32_t opcode);

#include <stdio.h>
#include <string.h>

/* ---- Forward declarations ---- */
static void hooked_swi16(struct ARMCore* cpu, int immediate);
void interception_handle_swi(struct ARMCore* cpu, int immediate);

/* ---- Function Table ---- */

typedef void (*RecompFunc)(void);

typedef struct {
    u32 addr;           /* ROM address (with Thumb bit cleared) */
    RecompFunc func;    /* Recompiled C function pointer */
} FuncEntry;

static FuncEntry* func_table = NULL;
static int func_table_size = 0;
static int intercept_count = 0;
static bool interception_enabled = false;

/* ---- External References ---- */

extern struct mCore* get_mgba_core(void);
extern struct GBA* get_mgba_gba(void);
extern struct ARMCore* get_mgba_arm(void);

/* CPU state from runtime */
extern u32 r[16];
extern bool CPU_N, CPU_Z, CPU_C, CPU_V;
extern u32 cpu_get_cpsr(void);
extern void cpu_set_cpsr(u32 value, u32 mask);

/* ---- Register Sync ---- */

static void sync_from_mgba(struct ARMCore* cpu) {
    for (int i = 0; i < 16; i++) r[i] = cpu->gprs[i];
    CPU_N = cpu->cpsr.n;
    CPU_Z = cpu->cpsr.z;
    CPU_C = cpu->cpsr.c;
    CPU_V = cpu->cpsr.v;
}

static void sync_to_mgba(struct ARMCore* cpu) {
    for (int i = 0; i < 16; i++) cpu->gprs[i] = r[i];
    cpu->cpsr.n = CPU_N;
    cpu->cpsr.z = CPU_Z;
    cpu->cpsr.c = CPU_C;
    cpu->cpsr.v = CPU_V;
}

/* ---- Binary Search Lookup ---- */

static RecompFunc lookup_function(u32 addr) {
    /* Clear Thumb bit */
    addr &= ~1u;

    int lo = 0, hi = func_table_size - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (func_table[mid].addr == addr) return func_table[mid].func;
        if (func_table[mid].addr < addr) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* ---- Public API ---- */

/* ---- SWI Hook ---- */

static void (*original_swi16)(struct ARMCore*, int) = NULL;
static void (*original_bkpt16)(struct ARMCore*, int) = NULL;

static void hooked_swi16(struct ARMCore* cpu, int immediate) {
    if (immediate == 0xFE && interception_enabled) {
        interception_handle_swi(cpu, immediate);
        return;
    }
    if (original_swi16) original_swi16(cpu, immediate);
}

static void hooked_bkpt16(struct ARMCore* cpu, int immediate) {
    if (immediate == 0xFE && interception_enabled) {
        interception_handle_swi(cpu, immediate);
        return;
    }
    if (original_bkpt16) original_bkpt16(cpu, immediate);
}

/* ---- ROM Patching ---- */

/* We patch the first Thumb instruction of each function with SWI 0xFE (0xDFFE).
 * When mGBA's CPU hits this SWI, our handler looks up the function by (PC-2)
 * and calls the recompiled C version. The original instruction is saved. */

static u16* original_insns = NULL; /* Saved original first instructions */
static struct mCore* s_core = NULL;

/* SWI hook - called by mGBA when SWI 0xFE is executed */
void interception_handle_swi(struct ARMCore* cpu, int immediate) {
    if (immediate != 0xFE || !interception_enabled) return;

    /* PC points past the BKPT. In Thumb: PC = BKPT_addr + 2 (no pipeline advance for BKPT).
     * Actually mGBA's BKPT handler: PC = instruction_addr + WORD_SIZE_THUMB
     * The function entry is at the BKPT instruction address. */
    u32 func_addr = cpu->gprs[15] - 4; /* PC is 2 ahead + Thumb pipeline */

    RecompFunc func = lookup_function(func_addr);
    if (!func) {
        /* Try nearby addresses */
        func = lookup_function(func_addr - 2);
        if (!func) func = lookup_function(func_addr - 4);
    }

    if (func) {
        intercept_count++;

        if (intercept_count <= 50) {
            fprintf(stderr, "[intercept!] PC=0x%08X -> native C (#%d)\n",
                    func_addr, intercept_count);
            fflush(stderr);
        }

        /* Sync mGBA -> recompiled register file */
        sync_from_mgba(cpu);

        /* Disable interrupt delivery during interception */
        extern bool in_irq;
        bool saved_in_irq = in_irq;
        in_irq = true;

        /* Save mGBA state for crash recovery */
        u32 saved_gprs[16];
        memcpy(saved_gprs, cpu->gprs, sizeof(saved_gprs));

        /* Call the recompiled C function */
        func();

        in_irq = saved_in_irq;

        /* If the function returned via BX LR (return;), r[15] isn't updated.
         * In that case, the return address is in r[14] (LR). */
        if (r[15] == cpu->gprs[15]) {
            /* r[15] unchanged - function returned via BX LR, use LR as return PC */
            r[15] = r[14];
        }

        /* Sync recompiled registers back to mGBA */
        sync_to_mgba(cpu);

        /* Tell mGBA to resume from the return address. */
        u32 ret_pc = cpu->gprs[15];
        cpu->gprs[15] = ret_pc & ~1u;

        /* Set Thumb/ARM mode from return address bit 0 */
        if (ret_pc & 1) {
            cpu->cpsr.t = 1;
            cpu->executionMode = MODE_THUMB;
        } else {
            cpu->cpsr.t = 0;
            cpu->executionMode = MODE_ARM;
        }

        /* Flush mGBA's pipeline to refetch from new PC */
        if (cpu->executionMode == MODE_THUMB) {
            ARMRunFake(cpu, 0x46C0); /* Thumb NOP */
        } else {
            ARMRunFake(cpu, 0xE1A00000); /* ARM NOP */
        }
    } else {
        /* No recompiled function found for this address.
         * Restore the original instruction and let mGBA execute it. */
        /* For now, just skip the BKPT by advancing PC */
        /* (This shouldn't happen since we only patch known functions) */
    }
}

void interception_init(FuncEntry* table, int size) {
    func_table = table;
    func_table_size = size;
    interception_enabled = true;
    intercept_count = 0;

    fprintf(stderr, "[intercept] Initialized with %d ROM functions\n", size);
    fflush(stderr);

    /* Hook mGBA's SWI handler to intercept our custom SWI 0xFE.
     * Save the original handler and chain to it for real SWIs. */
    s_core = get_mgba_core();
    if (s_core) {
        struct ARMCore* arm = get_mgba_arm();
        original_swi16 = arm->irqh.swi16;
        arm->irqh.swi16 = hooked_swi16;
        original_bkpt16 = arm->irqh.bkpt16;
        arm->irqh.bkpt16 = hooked_bkpt16;
        fprintf(stderr, "[intercept] SWI+BKPT handlers hooked\n");
        fflush(stderr);

        /* Patch ROM: replace first instruction of each function with BKPT 0xFE.
         * Thumb BKPT 0xFE = opcode 0xBEFE
         * BKPT doesn't change CPU mode (unlike SWI), making it cleaner. */
        struct GBA* gba = get_mgba_gba();
        original_insns = (u16*)malloc(size * sizeof(u16));
        int patched = 0;

        /* For testing: only patch a few small functions to validate the approach */
        for (int i = 0; i < size; i++) {
            u32 addr = table[i].addr;
            if ((addr >> 24) != 0x08) continue;

            u32 rom_offset = addr - 0x08000000;
            if (rom_offset + 2 > gba->memory.romSize) continue;

            /* Save original instruction */
            u16 first_insn = *(u16*)((u8*)gba->memory.rom + rom_offset);
            original_insns[i] = first_insn;

            /* Skip BX trampoline functions (BX Rn as first instruction) */
            bool is_bx = (first_insn & 0xFF87) == 0x4700;
            if (is_bx) continue;

            /* Skip ARM functions */
            if (addr < 0x080000C4) continue;

            /* Only intercept safe leaf functions:
             * - No PUSH {LR} (don't manipulate stack for LR)
             * - Must have BX LR within 20 bytes (simple return)
             * - No BL calls (no nested function calls)
             * - No SWI calls (avoid gba_swi complications during interception) */
            {
                bool safe = false;
                bool is_push = (first_insn & 0xFF00) == 0xB500;
                if (is_push) continue; /* Skip PUSH functions */

                /* Scan for BX LR and check for BL/SWI */
                bool has_bx_lr = false;
                bool has_bl_or_swi = false;
                for (u32 off = rom_offset; off < rom_offset + 20 && off + 2 <= gba->memory.romSize; off += 2) {
                    u16 insn = *(u16*)((u8*)gba->memory.rom + off);
                    if (insn == 0x4770) { has_bx_lr = true; break; }
                    if ((insn & 0xF800) == 0xF000) has_bl_or_swi = true; /* BL */
                    if ((insn & 0xFF00) == 0xDF00) has_bl_or_swi = true; /* SWI */
                }
                safe = has_bx_lr && !has_bl_or_swi;
                if (!safe) continue;
            }

            /* Patch with BKPT 0xFE (Thumb: 0xBEFE) */
            *(u16*)((u8*)gba->memory.rom + rom_offset) = 0xBEFE;
            patched++;
        }
        fprintf(stderr, "[intercept] Patched %d ROM functions (limited for testing)\n", patched);
        fflush(stderr);
    }
}

void interception_shutdown(void) {
    interception_enabled = false;
    fprintf(stderr, "[intercept] Total interceptions: %d\n", intercept_count);
    fflush(stderr);
}

/* Run one frame with function interception.
 * mGBA's CPU executes, but ROM function entries are replaced with
 * recompiled C code. Returns when a full frame has been rendered. */
void interception_run_frame(struct mCore* core) {
    struct GBA* gba = core->board;
    struct ARMCore* cpu = core->cpu;

    if (!interception_enabled || !func_table) {
        core->runFrame(core);
        return;
    }

    /* Run a full frame via mGBA's fast interpreter.
     * Function interception happens via SWI traps patched into ROM. */
    core->runFrame(core);

    static int frame_log = 0;
    frame_log++;
    if (frame_log <= 5 || frame_log % 120 == 0) {
        fprintf(stderr, "[intercept] frame %d: %d total interceptions\n", frame_log, intercept_count);
        fflush(stderr);
    }
}

int interception_get_count(void) {
    return intercept_count;
}
