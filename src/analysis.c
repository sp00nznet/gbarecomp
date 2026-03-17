#include "gba/analysis.h"
#include "gba/disasm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Dynamic array helpers ---- */

#define GROW(arr, count, cap, type) do { \
    if ((count) >= (cap)) { \
        (cap) = (cap) ? (cap) * 2 : 64; \
        (arr) = realloc((arr), sizeof(type) * (cap)); \
    } \
} while (0)

/* ---- Codemap helpers ---- */

/* Convert ROM address to codemap index (halfword granularity) */
static u32 addr_to_idx(u32 addr) {
    if (addr >= GBA_ROM_START) {
        return (addr - GBA_ROM_START) >> 1;
    }
    return (u32)-1;
}

static bool addr_in_rom(const AnalysisCtx* ctx, u32 addr) {
    if (addr < GBA_ROM_START) return false;
    return (addr - GBA_ROM_START) < ctx->rom->size;
}

static void mark_code(AnalysisCtx* ctx, u32 addr, CodeType type) {
    u32 idx = addr_to_idx(addr);
    if (idx < ctx->codemap_size) {
        ctx->codemap[idx] = (u8)type;
    }
}

static bool is_visited(const AnalysisCtx* ctx, u32 addr) {
    u32 idx = addr_to_idx(addr);
    if (idx >= ctx->codemap_size) return true; /* out of bounds = visited */
    return ctx->codemap[idx] != CODE_UNKNOWN;
}

/* ---- Work queue ---- */

static void queue_init(AnalysisCtx* ctx) {
    ctx->queue_cap = 4096;
    ctx->queue = malloc(sizeof(WorkItem) * ctx->queue_cap);
    ctx->queue_head = 0;
    ctx->queue_tail = 0;
}

static bool queue_empty(const AnalysisCtx* ctx) {
    return ctx->queue_head == ctx->queue_tail;
}

static void queue_push(AnalysisCtx* ctx, u32 addr, CodeType mode, u32 caller, bool is_call) {
    /* Don't queue already-visited addresses */
    if (is_visited(ctx, addr)) return;
    if (!addr_in_rom(ctx, addr)) return;

    /* Circular buffer growth */
    int next = (ctx->queue_tail + 1) % ctx->queue_cap;
    if (next == ctx->queue_head) {
        /* Grow the queue */
        int old_cap = ctx->queue_cap;
        ctx->queue_cap *= 2;
        WorkItem* new_q = malloc(sizeof(WorkItem) * ctx->queue_cap);
        int count = 0;
        for (int i = ctx->queue_head; i != ctx->queue_tail; i = (i + 1) % old_cap) {
            new_q[count++] = ctx->queue[i];
        }
        free(ctx->queue);
        ctx->queue = new_q;
        ctx->queue_head = 0;
        ctx->queue_tail = count;
        next = ctx->queue_tail + 1;
    }

    ctx->queue[ctx->queue_tail].addr = addr;
    ctx->queue[ctx->queue_tail].mode = mode;
    ctx->queue[ctx->queue_tail].caller = caller;
    ctx->queue[ctx->queue_tail].is_call = is_call;
    ctx->queue_tail = (ctx->queue_tail + 1) % ctx->queue_cap;
}

static WorkItem queue_pop(AnalysisCtx* ctx) {
    WorkItem item = ctx->queue[ctx->queue_head];
    ctx->queue_head = (ctx->queue_head + 1) % ctx->queue_cap;
    return item;
}

/* ---- Block management ---- */

static BasicBlock* add_block(AnalysisCtx* ctx) {
    GROW(ctx->blocks, ctx->num_blocks, ctx->cap_blocks, BasicBlock);
    BasicBlock* b = &ctx->blocks[ctx->num_blocks++];
    memset(b, 0, sizeof(BasicBlock));
    return b;
}

static Function* add_function(AnalysisCtx* ctx, u32 entry, CodeType mode) {
    /* Check if function already exists */
    for (int i = 0; i < ctx->num_functions; i++) {
        if (ctx->functions[i].entry == entry) {
            return &ctx->functions[i];
        }
    }

    GROW(ctx->functions, ctx->num_functions, ctx->cap_functions, Function);
    Function* f = &ctx->functions[ctx->num_functions++];
    memset(f, 0, sizeof(Function));
    f->entry = entry;
    f->mode = mode;
    f->is_leaf = true;
    return f;
}

static void function_add_block(Function* func, u32 block_addr) {
    GROW(func->block_addrs, func->num_blocks, func->cap_blocks, u32);
    func->block_addrs[func->num_blocks++] = block_addr;
}

/* ---- ARM analysis ---- */

static bool arm_is_return(const ArmInsn* insn) {
    /* BX LR */
    if (insn->type == ARM_BX && insn->rm == REG_LR) return true;
    /* MOV PC, LR */
    if (insn->type == ARM_MOV && insn->rd == REG_PC && !insn->i && insn->rm == REG_LR)
        return true;
    /* LDMFD SP!, {..., PC} */
    if (insn->type == ARM_LDM && insn->rn == REG_SP && (insn->reg_list & (1 << REG_PC)))
        return true;
    return false;
}

static bool arm_is_block_end(const ArmInsn* insn) {
    if (insn->type == ARM_B || insn->type == ARM_BL) return true;
    if (insn->type == ARM_BX) return true;
    if (insn->type == ARM_SWI) return true;
    /* MOV/LDR to PC */
    if ((insn->type == ARM_MOV || insn->type == ARM_LDR || insn->type == ARM_ADD ||
         insn->type == ARM_SUB) && insn->rd == REG_PC)
        return true;
    if (insn->type == ARM_LDM && (insn->reg_list & (1 << REG_PC)))
        return true;
    return false;
}

static void analyze_arm_block(AnalysisCtx* ctx, u32 start, Function* func) {
    BasicBlock* block = add_block(ctx);
    block->start = start;
    block->mode = CODE_ARM;

    u32 addr = start;
    while (addr_in_rom(ctx, addr)) {
        if (addr != start && is_visited(ctx, addr)) {
            /* Hit already-analyzed code, end block here */
            block->end = addr;
            block->successors[0] = addr;
            block->num_successors = 1;
            if (func) function_add_block(func, block->start);
            return;
        }

        mark_code(ctx, addr, CODE_ARM);
        /* Mark both halfwords for a 32-bit ARM instruction */
        mark_code(ctx, addr + 2, CODE_ARM);

        u32 raw = rom_read32(ctx->rom, addr);
        ArmInsn insn = arm_decode(raw);
        ctx->arm_insn_count++;

        if (arm_is_block_end(&insn)) {
            block->end = addr + 4;

            if (arm_is_return(&insn)) {
                block->is_return = true;
                block->num_successors = 0;
            } else if (insn.type == ARM_B) {
                u32 target = addr + 8 + (u32)insn.branch_offset;
                if (insn.cond == COND_AL) {
                    /* Unconditional branch */
                    block->successors[0] = target;
                    block->num_successors = 1;
                    queue_push(ctx, target, CODE_ARM, addr, false);
                } else {
                    /* Conditional branch: fallthrough + target */
                    block->successors[0] = addr + 4; /* fallthrough */
                    block->successors[1] = target;
                    block->num_successors = 2;
                    queue_push(ctx, addr + 4, CODE_ARM, addr, false);
                    queue_push(ctx, target, CODE_ARM, addr, false);
                }
            } else if (insn.type == ARM_BL) {
                u32 target = addr + 8 + (u32)insn.branch_offset;
                /* BL is a call - the block continues after it */
                block->end = addr + 4;
                block->successors[0] = addr + 4;
                block->num_successors = 1;
                if (func) func->is_leaf = false;
                /* Queue the call target as a new function */
                queue_push(ctx, target, CODE_ARM, addr, true);
                /* Continue analyzing after the BL */
                queue_push(ctx, addr + 4, CODE_ARM, addr, false);
            } else if (insn.type == ARM_BX) {
                if (insn.rm == REG_LR) {
                    block->is_return = true;
                    block->num_successors = 0;
                } else {
                    /* Try to resolve BX target by scanning backward in this block */
                    bool resolved = false;
                    bool is_call = false;
                    u8 target_reg = insn.rm;

                    for (u32 scan = addr - 4; scan >= start && scan >= addr - 32; scan -= 4) {
                        u32 scan_raw = rom_read32(ctx->rom, scan);
                        ArmInsn scan_insn = arm_decode(scan_raw);

                        /* MOV LR, PC = this BX is a call */
                        if (scan_insn.type == ARM_MOV && scan_insn.rd == REG_LR &&
                            !scan_insn.i && scan_insn.rm == REG_PC &&
                            scan_insn.shift_amount == 0) {
                            is_call = true;
                        }

                        /* LDR target_reg, [PC, #offset] = resolve from literal pool */
                        if (scan_insn.type == ARM_LDR && scan_insn.rd == target_reg &&
                            scan_insn.rn == REG_PC && !scan_insn.i && scan_insn.p) {
                            u32 pc_val = scan + 8;
                            u32 pool_addr = scan_insn.u ?
                                pc_val + scan_insn.imm : pc_val - scan_insn.imm;
                            u32 target = rom_read32(ctx->rom, pool_addr);
                            bool target_thumb = (target & 1) != 0;
                            target &= ~1u;

                            if (addr_in_rom(ctx, target)) {
                                resolved = true;
                                if (is_call) {
                                    /* BX-call: continues after BX */
                                    block->successors[0] = addr + 4;
                                    block->num_successors = 1;
                                    if (func) func->is_leaf = false;
                                    queue_push(ctx, target,
                                              target_thumb ? CODE_THUMB : CODE_ARM,
                                              addr, true);
                                    queue_push(ctx, addr + 4, CODE_ARM, addr, false);
                                } else {
                                    /* Tail branch */
                                    block->successors[0] = target;
                                    block->num_successors = 1;
                                    queue_push(ctx, target,
                                              target_thumb ? CODE_THUMB : CODE_ARM,
                                              addr, false);
                                }
                            }
                            break;
                        }
                        if (scan == 0) break;
                    }

                    if (!resolved) {
                        block->has_indirect = true;
                        block->num_successors = 0;
                    }
                }
            } else if (insn.type == ARM_LDM && (insn.reg_list & (1 << REG_PC))) {
                /* LDM with PC - likely a return */
                block->is_return = true;
                block->num_successors = 0;
            } else {
                /* Other PC-modifying instruction */
                block->has_indirect = true;
                block->num_successors = 0;
            }

            if (func) function_add_block(func, block->start);
            return;
        }

        addr += 4;
    }

    /* Ran off the end of ROM */
    block->end = addr;
    block->num_successors = 0;
    if (func) function_add_block(func, block->start);
}

/* ---- Thumb analysis ---- */

static bool thumb_is_return(const ThumbInsn* insn) {
    /* POP {.., PC} */
    if (insn->type == THUMB_PUSH_POP && insn->is_load && insn->pc_or_lr)
        return true;
    /* BX LR */
    if (insn->type == THUMB_BX && insn->rs == REG_LR)
        return true;
    return false;
}

static bool thumb_is_block_end(const ThumbInsn* insn) {
    if (insn->type == THUMB_BRANCH) return true;
    if (insn->type == THUMB_COND_BRANCH) return true;
    if (insn->type == THUMB_BX) return true;
    if (insn->type == THUMB_LONG_BRANCH && insn->is_suffix) return true;
    if (insn->type == THUMB_SWI) return true;
    /* POP {.., PC} */
    if (insn->type == THUMB_PUSH_POP && insn->is_load && insn->pc_or_lr) return true;
    return false;
}

static void analyze_thumb_block(AnalysisCtx* ctx, u32 start, Function* func) {
    BasicBlock* block = add_block(ctx);
    block->start = start;
    block->mode = CODE_THUMB;

    u32 addr = start;
    u32 bl_prefix_offset = 0;  /* Track BL prefix for two-part BL */
    bool have_bl_prefix = false;

    while (addr_in_rom(ctx, addr)) {
        if (addr != start && is_visited(ctx, addr)) {
            block->end = addr;
            block->successors[0] = addr;
            block->num_successors = 1;
            if (func) function_add_block(func, block->start);
            return;
        }

        mark_code(ctx, addr, CODE_THUMB);

        u16 raw = rom_read16(ctx->rom, addr);
        ThumbInsn insn = thumb_decode(raw);
        ctx->thumb_insn_count++;

        /* Handle BL prefix (first half) - not a block end, just save the offset */
        if (insn.type == THUMB_LONG_BRANCH && !insn.is_suffix) {
            bl_prefix_offset = (u32)insn.offset;
            have_bl_prefix = true;
            addr += 2;
            continue;
        }

        /* Handle BL suffix (second half) */
        if (insn.type == THUMB_LONG_BRANCH && insn.is_suffix) {
            if (have_bl_prefix) {
                /* Compute full BL target: PC + prefix_offset + suffix_offset */
                u32 bl_addr = addr - 2; /* Address of prefix instruction */
                u32 target = (bl_addr + 4) + bl_prefix_offset + insn.imm;

                block->end = addr + 2;
                block->successors[0] = addr + 2;
                block->num_successors = 1;
                if (func) func->is_leaf = false;

                /* Queue call target as new function (Thumb mode) */
                queue_push(ctx, target, CODE_THUMB, addr, true);
                /* Continue after BL */
                queue_push(ctx, addr + 2, CODE_THUMB, addr, false);
            } else {
                /* Orphaned BL suffix - shouldn't happen, treat as block end */
                block->end = addr + 2;
                block->num_successors = 0;
            }
            have_bl_prefix = false;
            if (func) function_add_block(func, block->start);
            return;
        }

        have_bl_prefix = false;

        if (thumb_is_block_end(&insn)) {
            block->end = addr + 2;

            if (thumb_is_return(&insn)) {
                block->is_return = true;
                block->num_successors = 0;
            } else if (insn.type == THUMB_BRANCH) {
                u32 target = addr + 4 + (u32)insn.offset;
                block->successors[0] = target;
                block->num_successors = 1;
                queue_push(ctx, target, CODE_THUMB, addr, false);
            } else if (insn.type == THUMB_COND_BRANCH) {
                u32 target = addr + 4 + (u32)insn.offset;
                block->successors[0] = addr + 2;
                block->successors[1] = target;
                block->num_successors = 2;
                queue_push(ctx, addr + 2, CODE_THUMB, addr, false);
                queue_push(ctx, target, CODE_THUMB, addr, false);
            } else if (insn.type == THUMB_BX) {
                if (insn.rs == REG_LR) {
                    block->is_return = true;
                    block->num_successors = 0;
                } else {
                    block->has_indirect = true;
                    block->num_successors = 0;
                    /* BX to a register - might switch to ARM mode.
                     * We can't resolve this statically without data flow analysis. */
                }
            } else {
                block->num_successors = 0;
            }

            if (func) function_add_block(func, block->start);
            return;
        }

        addr += 2;
    }

    block->end = addr;
    block->num_successors = 0;
    if (func) function_add_block(func, block->start);
}

/* ---- Main analysis loop ---- */

static void process_work_item(AnalysisCtx* ctx, WorkItem* item) {
    if (is_visited(ctx, item->addr)) return;
    if (!addr_in_rom(ctx, item->addr)) return;

    /* If this is a call target, register it as a function */
    Function* func = NULL;
    if (item->is_call || item->caller == 0) {
        func = add_function(ctx, item->addr, item->mode);
    } else {
        /* Find which function we belong to by looking up the caller */
        for (int i = ctx->num_functions - 1; i >= 0; i--) {
            Function* f = &ctx->functions[i];
            /* Simple heuristic: blocks belong to the most recently created function
             * whose entry is <= our address */
            if (f->entry <= item->addr) {
                func = f;
                break;
            }
        }
    }

    if (item->mode == CODE_ARM) {
        analyze_arm_block(ctx, item->addr, func);
    } else {
        analyze_thumb_block(ctx, item->addr, func);
    }
}

/* ---- Detect initial ARM/Thumb mode from entry ---- */

static CodeType detect_entry_mode(const GbaRom* rom) {
    /* The GBA ROM header at 0x08000000 is always ARM (it's a B instruction) */
    u32 entry_raw = rom_read32(rom, GBA_ROM_START);
    ArmInsn entry = arm_decode(entry_raw);

    if (entry.type == ARM_B) {
        u32 target = GBA_ROM_START + 8 + (u32)entry.branch_offset;
        /* Check if the target switches to Thumb via BX */
        /* For now, assume the branch target starts in ARM mode */
        /* The BX instruction in the startup code will handle mode switch */
        (void)target;
    }

    return CODE_ARM;
}

/* ---- SWI vector analysis ---- */

/* Many GBA games set up SWI handlers. We can find them from the BIOS vector table.
 * However, the BIOS handles SWIs itself - games use SWI for BIOS calls.
 * We don't need to analyze the BIOS. */

/* ---- Jump table detection ---- */

/* Common GBA jump table patterns in Thumb:
 *   ADD Rd, Rd     (or LSL Rd, Rd, #1 for halfword tables)
 *   ADD Rd, PC
 *   LDRH Rd, [Rd, #0]  (or LDR for word tables)
 *   ADD PC, Rd (via hi-reg MOV PC, Rd)
 *
 * In ARM:
 *   ADD PC, PC, Rn, LSL #2
 *   LDR PC, [PC, Rn, LSL #2]
 */
static void detect_arm_jump_table(AnalysisCtx* ctx, u32 addr) {
    u32 raw = rom_read32(ctx->rom, addr);
    ArmInsn insn = arm_decode(raw);

    /* Pattern: ADD PC, PC, Rn, LSL #2 */
    if (insn.type == ARM_ADD && insn.rd == REG_PC && insn.rn == REG_PC &&
        !insn.i && insn.shift_type == SHIFT_LSL && insn.shift_amount == 2) {
        /* The table starts right after this instruction.
         * Each entry is a branch instruction. */
        u32 table_start = addr + 8; /* PC+8 in ARM mode */

        /* Try to determine table size by looking for non-branch instructions */
        GROW(ctx->jump_tables, ctx->num_jump_tables, ctx->cap_jump_tables, JumpTable);
        JumpTable* jt = &ctx->jump_tables[ctx->num_jump_tables++];
        memset(jt, 0, sizeof(JumpTable));
        jt->branch_addr = addr;
        jt->table_addr = table_start;
        jt->is_thumb = false;

        /* Scan for branch instructions in the table */
        int max_entries = 256; /* reasonable upper bound */
        jt->targets = malloc(sizeof(u32) * max_entries);
        jt->num_entries = 0;

        for (int i = 0; i < max_entries; i++) {
            u32 entry_addr = table_start + (u32)(i * 4);
            if (!addr_in_rom(ctx, entry_addr)) break;

            u32 entry_raw = rom_read32(ctx->rom, entry_addr);
            ArmInsn entry_insn = arm_decode(entry_raw);

            if (entry_insn.type != ARM_B) break; /* End of table */

            u32 target = entry_addr + 8 + (u32)entry_insn.branch_offset;
            jt->targets[jt->num_entries++] = target;

            /* Queue target for analysis */
            queue_push(ctx, target, CODE_ARM, addr, false);
            /* Mark the table entry as data/code */
            mark_code(ctx, entry_addr, CODE_ARM);
            mark_code(ctx, entry_addr + 2, CODE_ARM);
        }
    }

    /* Pattern: LDR PC, [PC, Rn, LSL #2] */
    if (insn.type == ARM_LDR && insn.rd == REG_PC && insn.rn == REG_PC &&
        insn.i && insn.shift_type == SHIFT_LSL && insn.shift_amount == 2) {
        u32 table_start = addr + 8;

        GROW(ctx->jump_tables, ctx->num_jump_tables, ctx->cap_jump_tables, JumpTable);
        JumpTable* jt = &ctx->jump_tables[ctx->num_jump_tables++];
        memset(jt, 0, sizeof(JumpTable));
        jt->branch_addr = addr;
        jt->table_addr = table_start;
        jt->is_thumb = false;

        int max_entries = 256;
        jt->targets = malloc(sizeof(u32) * max_entries);
        jt->num_entries = 0;

        for (int i = 0; i < max_entries; i++) {
            u32 entry_addr = table_start + (u32)(i * 4);
            if (!addr_in_rom(ctx, entry_addr)) break;

            u32 target = rom_read32(ctx->rom, entry_addr);
            /* Validate that target looks like a code address */
            if (target < GBA_ROM_START || target >= GBA_ROM_START + ctx->rom->size) break;

            bool target_thumb = (target & 1) != 0;
            target &= ~1u; /* Clear Thumb bit */

            jt->targets[jt->num_entries++] = target;
            queue_push(ctx, target, target_thumb ? CODE_THUMB : CODE_ARM, addr, false);

            mark_code(ctx, entry_addr, CODE_DATA);
            mark_code(ctx, entry_addr + 2, CODE_DATA);
        }
    }
}

/* Scan analyzed blocks for jump table patterns */
static void detect_jump_tables(AnalysisCtx* ctx) {
    for (int i = 0; i < ctx->num_blocks; i++) {
        BasicBlock* block = &ctx->blocks[i];
        if (!block->has_indirect) continue;

        if (block->mode == CODE_ARM) {
            /* Check the last instruction in the block */
            u32 last_insn_addr = block->end - 4;
            detect_arm_jump_table(ctx, last_insn_addr);
        }
        /* TODO: Thumb jump table detection */
    }

    /* Process any newly queued addresses from jump table detection */
    while (!queue_empty(ctx)) {
        WorkItem item = queue_pop(ctx);
        process_work_item(ctx, &item);
    }
}

/* ---- BX target resolution ---- */

/* Look backward from a BX instruction to find where the register was loaded.
 * Common pattern:
 *   LDR Rn, [PC, #offset]   -> loads address from literal pool
 *   BX Rn
 */
static void resolve_bx_targets(AnalysisCtx* ctx) {
    for (int i = 0; i < ctx->num_blocks; i++) {
        BasicBlock* block = &ctx->blocks[i];
        if (!block->has_indirect) continue;

        if (block->mode == CODE_ARM && block->end >= block->start + 8) {
            /* Look at the last two instructions */
            u32 bx_addr = block->end - 4;
            u32 prev_addr = bx_addr - 4;

            u32 bx_raw = rom_read32(ctx->rom, bx_addr);
            ArmInsn bx_insn = arm_decode(bx_raw);

            if (bx_insn.type != ARM_BX || bx_insn.rm == REG_LR) continue;

            u32 prev_raw = rom_read32(ctx->rom, prev_addr);
            ArmInsn prev_insn = arm_decode(prev_raw);

            /* Check for LDR Rn, [PC, #offset] where Rn matches BX operand */
            if (prev_insn.type == ARM_LDR && prev_insn.rd == bx_insn.rm &&
                prev_insn.rn == REG_PC && !prev_insn.i && prev_insn.p) {
                u32 pc_val = prev_addr + 8;
                u32 pool_addr = prev_insn.u ?
                    pc_val + prev_insn.imm : pc_val - prev_insn.imm;
                u32 target = rom_read32(ctx->rom, pool_addr);
                bool target_thumb = (target & 1) != 0;
                target &= ~1u;

                if (addr_in_rom(ctx, target)) {
                    block->successors[0] = target;
                    block->num_successors = 1;
                    block->has_indirect = false;
                    queue_push(ctx, target, target_thumb ? CODE_THUMB : CODE_ARM,
                              bx_addr, false);
                }
            }
        }

        if (block->mode == CODE_THUMB && block->end >= block->start + 4) {
            /* Similar for Thumb: look for LDR Rd, [PC, #imm] before BX */
            u32 bx_addr = block->end - 2;
            u32 prev_addr = bx_addr - 2;

            u16 bx_raw = rom_read16(ctx->rom, bx_addr);
            ThumbInsn bx_insn = thumb_decode(bx_raw);

            if (bx_insn.type != THUMB_BX || bx_insn.rs == REG_LR) continue;

            u16 prev_raw = rom_read16(ctx->rom, prev_addr);
            ThumbInsn prev_insn = thumb_decode(prev_raw);

            if (prev_insn.type == THUMB_PC_REL_LOAD) {
                u32 pc_val = (prev_addr + 4) & ~3u;
                u32 pool_addr = pc_val + prev_insn.imm;
                u32 target = rom_read32(ctx->rom, pool_addr);
                bool target_thumb = (target & 1) != 0;
                target &= ~1u;

                if (addr_in_rom(ctx, target)) {
                    block->successors[0] = target;
                    block->num_successors = 1;
                    block->has_indirect = false;
                    queue_push(ctx, target, target_thumb ? CODE_THUMB : CODE_ARM,
                              bx_addr, false);
                }
            }
        }
    }

    /* Process any newly queued addresses */
    while (!queue_empty(ctx)) {
        WorkItem item = queue_pop(ctx);
        process_work_item(ctx, &item);
    }
}

/* ---- Sort blocks by address ---- */

static int block_cmp(const void* a, const void* b) {
    const BasicBlock* ba = (const BasicBlock*)a;
    const BasicBlock* bb = (const BasicBlock*)b;
    if (ba->start < bb->start) return -1;
    if (ba->start > bb->start) return 1;
    return 0;
}

static int func_cmp(const void* a, const void* b) {
    const Function* fa = (const Function*)a;
    const Function* fb = (const Function*)b;
    if (fa->entry < fb->entry) return -1;
    if (fa->entry > fb->entry) return 1;
    return 0;
}

/* ---- Public API ---- */

AnalysisCtx* analysis_create(const GbaRom* rom) {
    AnalysisCtx* ctx = calloc(1, sizeof(AnalysisCtx));
    ctx->rom = rom;

    /* Allocate codemap: one byte per halfword of ROM */
    ctx->codemap_size = rom->size / 2;
    ctx->codemap = calloc(ctx->codemap_size, 1);

    queue_init(ctx);
    return ctx;
}

void analysis_free(AnalysisCtx* ctx) {
    if (!ctx) return;
    free(ctx->codemap);
    free(ctx->blocks);

    for (int i = 0; i < ctx->num_functions; i++) {
        free(ctx->functions[i].block_addrs);
    }
    free(ctx->functions);

    for (int i = 0; i < ctx->num_jump_tables; i++) {
        free(ctx->jump_tables[i].targets);
    }
    free(ctx->jump_tables);

    free(ctx->queue);
    free(ctx);
}

void analysis_add_entry(AnalysisCtx* ctx, u32 addr, CodeType mode) {
    queue_push(ctx, addr, mode, 0, true);
}

void analysis_run(AnalysisCtx* ctx) {
    /* If no explicit entries, find from ROM header */
    if (queue_empty(ctx)) {
        CodeType mode = detect_entry_mode(ctx->rom);
        u32 entry_raw = rom_read32(ctx->rom, GBA_ROM_START);
        ArmInsn entry = arm_decode(entry_raw);

        if (entry.type == ARM_B) {
            u32 target = GBA_ROM_START + 8 + (u32)entry.branch_offset;
            analysis_add_entry(ctx, target, mode);
        } else {
            analysis_add_entry(ctx, GBA_ROM_START, mode);
        }
    }

    printf("[analysis] Starting recursive descent...\n");

    /* Phase 1: Recursive descent */
    int iteration = 0;
    while (!queue_empty(ctx)) {
        WorkItem item = queue_pop(ctx);
        process_work_item(ctx, &item);
        iteration++;
        if (iteration % 10000 == 0) {
            printf("[analysis] %d items processed, %d blocks, %d functions\n",
                   iteration, ctx->num_blocks, ctx->num_functions);
        }
    }

    printf("[analysis] Phase 1 complete: %d blocks, %d functions\n",
           ctx->num_blocks, ctx->num_functions);

    /* Phase 2: Resolve BX targets via backward analysis */
    printf("[analysis] Phase 2: Resolving indirect branches...\n");
    resolve_bx_targets(ctx);
    printf("[analysis] After BX resolution: %d blocks, %d functions\n",
           ctx->num_blocks, ctx->num_functions);

    /* Phase 3: Jump table detection */
    printf("[analysis] Phase 3: Detecting jump tables...\n");
    detect_jump_tables(ctx);
    printf("[analysis] Found %d jump tables, now %d blocks, %d functions\n",
           ctx->num_jump_tables, ctx->num_blocks, ctx->num_functions);

    /* Sort blocks and functions by address */
    qsort(ctx->blocks, ctx->num_blocks, sizeof(BasicBlock), block_cmp);
    qsort(ctx->functions, ctx->num_functions, sizeof(Function), func_cmp);

    ctx->total_blocks = ctx->num_blocks;
    ctx->total_functions = ctx->num_functions;
}

BasicBlock* analysis_find_block(AnalysisCtx* ctx, u32 addr) {
    /* Binary search since blocks are sorted */
    int lo = 0, hi = ctx->num_blocks - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ctx->blocks[mid].start == addr) {
            return &ctx->blocks[mid];
        } else if (ctx->blocks[mid].start < addr) {
            /* Check if addr falls within this block */
            if (addr < ctx->blocks[mid].end) {
                return &ctx->blocks[mid];
            }
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

Function* analysis_find_function(AnalysisCtx* ctx, u32 entry) {
    /* Binary search */
    int lo = 0, hi = ctx->num_functions - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ctx->functions[mid].entry == entry) return &ctx->functions[mid];
        if (ctx->functions[mid].entry < entry) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

CodeType analysis_get_type(const AnalysisCtx* ctx, u32 addr) {
    u32 idx = addr_to_idx(addr);
    if (idx >= ctx->codemap_size) return CODE_UNKNOWN;
    return (CodeType)ctx->codemap[idx];
}

void analysis_print_summary(const AnalysisCtx* ctx) {
    /* Count code bytes */
    u32 arm_bytes = 0, thumb_bytes = 0, data_bytes = 0, unknown_bytes = 0;
    for (u32 i = 0; i < ctx->codemap_size; i++) {
        switch (ctx->codemap[i]) {
            case CODE_ARM:     arm_bytes += 2; break;
            case CODE_THUMB:   thumb_bytes += 2; break;
            case CODE_DATA:    data_bytes += 2; break;
            default:           unknown_bytes += 2; break;
        }
    }

    int indirect_blocks = 0;
    for (int i = 0; i < ctx->num_blocks; i++) {
        if (ctx->blocks[i].has_indirect) indirect_blocks++;
    }

    int leaf_functions = 0;
    for (int i = 0; i < ctx->num_functions; i++) {
        if (ctx->functions[i].is_leaf) leaf_functions++;
    }

    printf("\n=== Analysis Summary ===\n");
    printf("  Functions:          %d (%d leaf)\n", ctx->num_functions, leaf_functions);
    printf("  Basic blocks:       %d (%d with unresolved indirect branches)\n",
           ctx->num_blocks, indirect_blocks);
    printf("  Jump tables:        %d\n", ctx->num_jump_tables);
    printf("  ARM instructions:   %u\n", ctx->arm_insn_count);
    printf("  Thumb instructions: %u\n", ctx->thumb_insn_count);
    printf("  Code coverage:\n");
    printf("    ARM code:         %u bytes (%.1f%%)\n", arm_bytes,
           100.0 * arm_bytes / ctx->rom->size);
    printf("    Thumb code:       %u bytes (%.1f%%)\n", thumb_bytes,
           100.0 * thumb_bytes / ctx->rom->size);
    printf("    Data:             %u bytes (%.1f%%)\n", data_bytes,
           100.0 * data_bytes / ctx->rom->size);
    printf("    Unanalyzed:       %u bytes (%.1f%%)\n", unknown_bytes,
           100.0 * unknown_bytes / ctx->rom->size);
    printf("========================\n");
}

void analysis_print_functions(const AnalysisCtx* ctx) {
    printf("\n=== Discovered Functions ===\n");
    printf("%-12s  %-6s  %-7s  %-7s\n", "Address", "Mode", "Blocks", "Leaf");
    printf("%-12s  %-6s  %-7s  %-7s\n", "--------", "----", "------", "----");
    for (int i = 0; i < ctx->num_functions; i++) {
        const Function* f = &ctx->functions[i];
        printf("0x%08X    %-6s  %-7d  %s\n",
               f->entry,
               f->mode == CODE_ARM ? "ARM" : "Thumb",
               f->num_blocks,
               f->is_leaf ? "yes" : "no");
    }
    printf("============================\n");
}

void analysis_print_function_detail(const AnalysisCtx* ctx, const Function* func) {
    char buf[256];
    printf("\n=== Function 0x%08X (%s) ===\n",
           func->entry, func->mode == CODE_ARM ? "ARM" : "Thumb");
    printf("  Blocks: %d, Leaf: %s\n\n",
           func->num_blocks, func->is_leaf ? "yes" : "no");

    /* Disassemble each block */
    for (int i = 0; i < func->num_blocks; i++) {
        /* Find the block by address */
        BasicBlock* block = NULL;
        for (int j = 0; j < ctx->num_blocks; j++) {
            if (ctx->blocks[j].start == func->block_addrs[i]) {
                block = &ctx->blocks[j];
                break;
            }
        }
        if (!block) continue;

        printf("  ; Block 0x%08X - 0x%08X", block->start, block->end);
        if (block->is_return) printf(" [return]");
        if (block->has_indirect) printf(" [indirect]");
        printf("\n");

        if (block->mode == CODE_ARM) {
            for (u32 addr = block->start; addr < block->end; addr += 4) {
                u32 raw = rom_read32(ctx->rom, addr);
                ArmInsn insn = arm_decode(raw);
                disasm_arm(&insn, addr, buf, sizeof(buf));
                printf("    %08X:  %08X  %s\n", addr, raw, buf);
            }
        } else {
            for (u32 addr = block->start; addr < block->end; addr += 2) {
                u16 raw = rom_read16(ctx->rom, addr);
                ThumbInsn insn = thumb_decode(raw);
                disasm_thumb(&insn, addr, buf, sizeof(buf));
                printf("    %08X:  %04X      %s\n", addr, raw, buf);
            }
        }
        printf("\n");
    }
}
