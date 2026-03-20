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

#include <stdio.h>
#include <string.h>

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

void interception_init(FuncEntry* table, int size) {
    func_table = table;
    func_table_size = size;
    interception_enabled = true;
    intercept_count = 0;
    fprintf(stderr, "[intercept] Initialized with %d functions\n", size);
    fflush(stderr);
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

    u32 start_frame = gba->video.frameCounter;

    while (gba->video.frameCounter == start_frame) {
        /* Check PC before stepping */
        u32 pc = cpu->gprs[15];

        /* Only intercept ROM addresses */
        if ((pc >> 24) == 0x08) {
            RecompFunc func = lookup_function(pc);
            if (func) {
                /* Sync mGBA -> recompiled */
                sync_from_mgba(cpu);

                /* Call the recompiled function.
                 * The function reads/writes memory via mGBA bus.
                 * POP {PC} does r[15] = bus_read32(r[13]) which sets the return address.
                 * BX LR does return; which exits the C function. */
                func();
                intercept_count++;

                /* Sync recompiled state -> mGBA */
                sync_to_mgba(cpu);

                /* Set mGBA's execution mode based on return PC (Thumb bit) */
                u32 return_pc = cpu->gprs[15];
                if (return_pc & 1) {
                    cpu->cpsr.t = 1;
                    cpu->executionMode = MODE_THUMB;
                    cpu->gprs[15] &= ~1u;
                } else {
                    cpu->cpsr.t = 0;
                    cpu->executionMode = MODE_ARM;
                }

                /* mGBA needs to refetch the pipeline at the new PC.
                 * Simplest way: let it step once from the new PC. */
                continue;
            }
        }

        /* Normal mGBA step */
        core->step(core);
    }
}

int interception_get_count(void) {
    return intercept_count;
}
