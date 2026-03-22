/*
 * Function Verification Harness
 *
 * Compares recompiled C function output against mGBA interpreter output.
 * For each intercepted function:
 * 1. Capture input state (registers + flags)
 * 2. Run recompiled C version -> capture output
 * 3. Restore input, run mGBA interpreter -> capture output
 * 4. Compare. Log any differences.
 *
 * This systematically finds translator bugs by comparing every register
 * and flag after execution.
 */

#include "gba/gba_runtime.h"

#include <mgba/flags.h>
#include <mgba/core/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/arm/arm.h>

#include <stdio.h>
#include <string.h>

/* ---- Types ---- */

typedef void (*RecompFunc)(void);

typedef struct {
    u32 gprs[16];
    bool n, z, c, v;
} CpuState;

typedef struct {
    u32 addr;
    int tests;
    int passes;
    int fails;
    u32 first_fail_reg;   /* Which register first diverged */
    u32 expected;          /* What mGBA produced */
    u32 got;              /* What our C produced */
} FuncVerifyResult;

/* ---- External References ---- */

extern u32 r[16];
extern bool CPU_N, CPU_Z, CPU_C, CPU_V;
extern bool in_irq;

extern struct mCore* get_mgba_core(void);
extern struct GBA* get_mgba_gba(void);
extern struct ARMCore* get_mgba_arm(void);

/* ---- State Capture ---- */

static void capture_mgba_state(struct ARMCore* cpu, CpuState* state) {
    memcpy(state->gprs, cpu->gprs, sizeof(state->gprs));
    state->n = cpu->cpsr.n;
    state->z = cpu->cpsr.z;
    state->c = cpu->cpsr.c;
    state->v = cpu->cpsr.v;
}

static void restore_mgba_state(struct ARMCore* cpu, const CpuState* state) {
    memcpy(cpu->gprs, state->gprs, sizeof(state->gprs));
    cpu->cpsr.n = state->n;
    cpu->cpsr.z = state->z;
    cpu->cpsr.c = state->c;
    cpu->cpsr.v = state->v;
}

static void capture_recomp_state(CpuState* state) {
    memcpy(state->gprs, r, sizeof(state->gprs));
    state->n = CPU_N;
    state->z = CPU_Z;
    state->c = CPU_C;
    state->v = CPU_V;
}

static void restore_recomp_state(const CpuState* state) {
    memcpy(r, state->gprs, sizeof(r));
    CPU_N = state->n;
    CPU_Z = state->z;
    CPU_C = state->c;
    CPU_V = state->v;
}

/* ---- Comparison ---- */

static bool compare_states(const CpuState* expected, const CpuState* got,
                          u32 func_addr, FuncVerifyResult* result) {
    bool match = true;

    /* Compare registers (skip r15/PC - differs by design) */
    for (int i = 0; i < 15; i++) {
        if (expected->gprs[i] != got->gprs[i]) {
            if (match) { /* First failure */
                result->first_fail_reg = i;
                result->expected = expected->gprs[i];
                result->got = got->gprs[i];
            }
            match = false;
        }
    }

    /* Compare flags */
    if (expected->n != got->n || expected->z != got->z ||
        expected->c != got->c || expected->v != got->v) {
        if (match) {
            result->first_fail_reg = 16; /* Flags */
            result->expected = (expected->n << 3) | (expected->z << 2) |
                              (expected->c << 1) | expected->v;
            result->got = (got->n << 3) | (got->z << 2) |
                         (got->c << 1) | got->v;
        }
        match = false;
    }

    return match;
}

/* ---- Memory Snapshot ---- */

/* Key memory regions to compare after function execution */
#define IO_SNAPSHOT_SIZE   0x200  /* First 512 bytes of IO (display, sound, DMA) */
#define PAL_SNAPSHOT_SIZE  0x400  /* Full palette (1KB) */

typedef struct {
    u8 io[IO_SNAPSHOT_SIZE];
    u8 palette[PAL_SNAPSHOT_SIZE];
} MemSnapshot;

static void capture_mem_snapshot(MemSnapshot* snap) {
    struct mCore* core = get_mgba_core();
    /* Read IO registers directly from mGBA */
    struct GBA* gba = get_mgba_gba();
    memcpy(snap->io, gba->memory.io, IO_SNAPSHOT_SIZE);

    /* Read palette from mGBA */
    memcpy(snap->palette, gba->video.palette, PAL_SNAPSHOT_SIZE);
}

static bool compare_mem_snapshots(const MemSnapshot* expected, const MemSnapshot* got,
                                  u32 func_addr) {
    bool match = true;

    /* Compare IO registers */
    for (int i = 0; i < IO_SNAPSHOT_SIZE; i += 2) {
        u16 exp_val = expected->io[i] | (expected->io[i+1] << 8);
        u16 got_val = got->io[i] | (got->io[i+1] << 8);
        if (exp_val != got_val) {
            if (match) {
                fprintf(stderr, "  [MEM] IO[0x%03X]: mGBA=0x%04X recomp=0x%04X\n",
                        i, exp_val, got_val);
                fflush(stderr);
            }
            match = false;
        }
    }

    /* Compare palette */
    for (int i = 0; i < PAL_SNAPSHOT_SIZE; i += 2) {
        u16 exp_val = expected->palette[i] | (expected->palette[i+1] << 8);
        u16 got_val = got->palette[i] | (got->palette[i+1] << 8);
        if (exp_val != got_val) {
            if (match) {
                fprintf(stderr, "  [MEM] PAL[0x%03X]: mGBA=0x%04X recomp=0x%04X\n",
                        i, exp_val, got_val);
                fflush(stderr);
            }
            match = false;
        }
    }

    return match;
}

/* ---- Verification Hook ---- */

/* Max functions to track */
#define MAX_TRACKED 256
static FuncVerifyResult results[MAX_TRACKED];
static int num_tracked = 0;
static int total_tests = 0;
static int total_passes = 0;
static int total_fails = 0;

static FuncVerifyResult* get_result(u32 addr) {
    for (int i = 0; i < num_tracked; i++) {
        if (results[i].addr == addr) return &results[i];
    }
    if (num_tracked < MAX_TRACKED) {
        FuncVerifyResult* r = &results[num_tracked++];
        memset(r, 0, sizeof(*r));
        r->addr = addr;
        return r;
    }
    return NULL;
}

/* Called from the mGBA ARMRunLoop hook.
 * Returns true if we handled the instruction (skip ThumbStep). */
bool verify_hook(struct ARMCore* cpu, RecompFunc func, u32 func_addr) {
    struct mCore* core = get_mgba_core();

    /* Only verify occasionally to avoid massive slowdown */
    FuncVerifyResult* res = get_result(func_addr);
    if (!res) return false;
    if (res->tests >= 3) return false; /* Test each function up to 3 times */

    total_tests++;
    res->tests++;

    /* 1. Capture input state (registers + memory) */
    CpuState input;
    capture_mgba_state(cpu, &input);
    MemSnapshot mem_before;
    capture_mem_snapshot(&mem_before);

    /* 2. Run recompiled C function */
    restore_recomp_state(&input);
    bool saved_irq = in_irq;
    in_irq = true;

    func();

    in_irq = saved_irq;

    /* Fix BX LR return: if r[15] unchanged, use LR */
    if (r[15] == input.gprs[15]) {
        r[15] = r[14];
    }

    CpuState recomp_output;
    capture_recomp_state(&recomp_output);
    MemSnapshot mem_after_recomp;
    capture_mem_snapshot(&mem_after_recomp);

    /* 3. Restore input state AND memory, then run mGBA interpreter */
    restore_mgba_state(cpu, &input);
    /* Restore memory to pre-function state */
    {
        struct GBA* gba = get_mgba_gba();
        memcpy(gba->memory.io, mem_before.io, IO_SNAPSHOT_SIZE);
        memcpy(gba->video.palette, mem_before.palette, PAL_SNAPSHOT_SIZE);
    }

    /* Step mGBA until function returns (PC leaves this function) */
    int steps = 0;
    u32 start_sp = cpu->gprs[13];
    while (steps < 10000) {
        core->step(core);
        steps++;

        /* Function returned when SP is restored (stack balanced) */
        if (cpu->gprs[13] >= start_sp && steps > 1) {
            /* Also check if we're at a different function or returned */
            u32 cur_pc = cpu->gprs[15];
            if (cur_pc != func_addr && (cur_pc < func_addr || cur_pc > func_addr + 0x200)) {
                break; /* Left the function */
            }
        }
    }

    CpuState mgba_output;
    capture_mgba_state(cpu, &mgba_output);
    MemSnapshot mem_after_mgba;
    capture_mem_snapshot(&mem_after_mgba);

    /* 4. Compare registers AND memory */
    bool reg_match = compare_states(&mgba_output, &recomp_output, func_addr, res);
    bool mem_match = compare_mem_snapshots(&mem_after_mgba, &mem_after_recomp, func_addr);
    bool match = reg_match && mem_match;

    if (match) {
        res->passes++;
        total_passes++;
    } else {
        res->fails++;
        total_fails++;

        if (total_fails <= 20) {
            fprintf(stderr, "\n[VERIFY FAIL] func_0x%08X (test #%d) %s%s\n",
                    func_addr, res->tests,
                    !reg_match ? "REGS " : "",
                    !mem_match ? "MEMORY" : "");
            fprintf(stderr, "  Diverged at: %s\n",
                    res->first_fail_reg < 16 ?
                    (res->first_fail_reg == 13 ? "SP (r13)" :
                     res->first_fail_reg == 14 ? "LR (r14)" : "register") :
                    "FLAGS (NZCV)");
            if (res->first_fail_reg < 16) {
                fprintf(stderr, "  r%d: mGBA=0x%08X  recomp=0x%08X\n",
                        res->first_fail_reg, res->expected, res->got);
            } else {
                fprintf(stderr, "  NZCV: mGBA=%d%d%d%d  recomp=%d%d%d%d\n",
                        (res->expected >> 3) & 1, (res->expected >> 2) & 1,
                        (res->expected >> 1) & 1, res->expected & 1,
                        (res->got >> 3) & 1, (res->got >> 2) & 1,
                        (res->got >> 1) & 1, res->got & 1);
            }
            /* Print input state for reproduction */
            fprintf(stderr, "  Input: r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X\n",
                    input.gprs[0], input.gprs[1], input.gprs[2], input.gprs[3]);
            fprintf(stderr, "         SP=0x%08X LR=0x%08X NZCV=%d%d%d%d\n",
                    input.gprs[13], input.gprs[14],
                    input.n, input.z, input.c, input.v);
            fprintf(stderr, "  mGBA stepped %d instructions\n", steps);
            fflush(stderr);
        }
    }

    /* Leave mGBA in the post-interpretation state (correct) */
    /* The caller should NOT skip ThumbStep since we already ran mGBA */
    return false; /* Don't skip - mGBA already executed the function */
}

void verify_print_summary(void) {
    fprintf(stderr, "\n=== Verification Summary ===\n");
    fprintf(stderr, "Total tests: %d  Passes: %d  Fails: %d\n",
            total_tests, total_passes, total_fails);
    fprintf(stderr, "Functions tracked: %d\n", num_tracked);

    int fail_count = 0;
    for (int i = 0; i < num_tracked; i++) {
        if (results[i].fails > 0) {
            fail_count++;
            if (fail_count <= 30) {
                fprintf(stderr, "  FAIL: 0x%08X (%d/%d failed) - r%d: expected 0x%08X got 0x%08X\n",
                        results[i].addr, results[i].fails, results[i].tests,
                        results[i].first_fail_reg, results[i].expected, results[i].got);
            }
        }
    }
    fprintf(stderr, "Functions with failures: %d / %d\n", fail_count, num_tracked);
    fprintf(stderr, "============================\n");
    fflush(stderr);
}
