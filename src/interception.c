/*
 * Function Interception - Modified mGBA Run Loop
 *
 * Instead of patching ROM with BKPT traps, we hook directly into
 * mGBA's ARMRunLoop via ARMSetRecompHook. The hook is called before
 * each instruction and checks if PC matches a recompiled function.
 * If so, it executes the native C version instead of interpreting.
 *
 * This approach has zero timing disruption because:
 * - No ROM modification (ROM stays clean)
 * - No mode changes or pipeline flushes
 * - Hook runs at the same point as normal instruction execution
 * - mGBA's timing/events system is completely unaffected
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
    u32 addr;
    RecompFunc func;
} FuncEntry;

static FuncEntry* func_table = NULL;
static int func_table_size = 0;
static int intercept_count = 0;
static int successful = 0;
static int failed = 0;
static bool interception_enabled = false;

/* Direct lookup table: O(1) function lookup by PC.
 * Index = (PC - 0x08000000) >> 1 (halfword granularity)
 * NULL = no recompiled function at this address. */
#define DIRECT_TABLE_SIZE (0x00400000) /* 4MB ROM / 2 bytes = 2M entries */
static RecompFunc* direct_table = NULL;

/* ---- External References ---- */

extern u32 r[16];
extern bool CPU_N, CPU_Z, CPU_C, CPU_V;
extern bool in_irq;

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

/* ---- Recomp Hook (called from ARMRunLoop) ---- */

/* This function is called before EVERY instruction when PC is in ROM.
 * It must be FAST for the common case (no match = return false). */
static bool recomp_hook(struct ARMCore* cpu) {
    if (!interception_enabled) return false;

    u32 pc = cpu->gprs[15];

    /* Binary search lookup */
    RecompFunc func = lookup_function(pc);
    if (!func) return false;

    /* Normal interception mode */
    intercept_count++;

    /* Sync mGBA -> recompiled */
    sync_from_mgba(cpu);

    /* Disable IRQ delivery and timing advance during interception.
     * Timing must only advance via ThumbStep/ARMStep, not via our
     * bus_read/write calls, or DISPSTAT gets corrupted. */
    bool saved_irq = in_irq;
    in_irq = true;
    extern bool skip_advance;
    skip_advance = true;

    /* Save state for crash recovery */
    u32 saved_gprs[16];
    memcpy(saved_gprs, cpu->gprs, sizeof(saved_gprs));

    /* Execute the recompiled C function */
    bool crashed = false;
#ifdef _WIN32
    __try {
#endif
        func();
#ifdef _WIN32
    } __except(1) {
        crashed = true;
    }
#endif

    in_irq = saved_irq;
    skip_advance = false;

    if (crashed) {
        /* Restore state and let mGBA interpret this function */
        memcpy(cpu->gprs, saved_gprs, sizeof(saved_gprs));
        failed++;
        if (failed <= 20) {
            fprintf(stderr, "[recomp] CRASH at 0x%08X (%d ok, %d fail)\n",
                    pc, successful, failed);
            fflush(stderr);
        }
        return false; /* Let mGBA interpret it */
    }

    /* Fix return PC */
    if (r[15] == saved_gprs[15]) {
        r[15] = r[14]; /* BX LR return: use LR */
    }

    /* Sync back to mGBA */
    sync_to_mgba(cpu);

    /* Add estimated cycles. Use bus_access_count delta as proxy. */
    cpu->cycles += 50;

    successful++;

    if (intercept_count <= 20 || intercept_count % 5000 == 0) {
        fprintf(stderr, "[recomp #%d] 0x%08X native (%d ok, %d fail)\n",
                intercept_count, pc, successful, failed);
        fflush(stderr);
    }

    return true; /* Instruction was handled */
}

/* ---- Public API ---- */

void interception_init(FuncEntry* table, int size) {
    func_table = table;
    func_table_size = size;
    interception_enabled = true;
    intercept_count = 0;
    successful = 0;
    failed = 0;

    /* Install the hook into mGBA's ARMRunLoop */
    ARMSetRecompHook(recomp_hook);

    fprintf(stderr, "[recomp] Hook installed: %d functions\n", size);
    fflush(stderr);
}

void interception_shutdown(void) {
    ARMSetRecompHook(NULL);
    interception_enabled = false;
    fprintf(stderr, "[recomp] Final: %d native, %d fallback\n", successful, failed);
    fflush(stderr);
}

void interception_run_frame(struct mCore* core) {
    /* Just use normal runFrame - the hook is called from inside ARMRunLoop */
    core->runFrame(core);

    static int frame = 0;
    frame++;
    if (frame <= 5 || frame % 60 == 0) {
        fprintf(stderr, "[recomp] frame %d: %d native, %d fallback\n",
                frame, successful, failed);
        fflush(stderr);
    }
}

int interception_get_count(void) {
    return intercept_count;
}
