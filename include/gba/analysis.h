#ifndef GBA_ANALYSIS_H
#define GBA_ANALYSIS_H

#include "gba/types.h"
#include "gba/rom.h"
#include "gba/arm.h"
#include "gba/thumb.h"

/* Code region mode */
typedef enum {
    CODE_UNKNOWN = 0,
    CODE_ARM     = 1,
    CODE_THUMB   = 2,
    CODE_DATA    = 3,
} CodeType;

/* A basic block: contiguous instructions with one entry, one exit */
typedef struct BasicBlock {
    u32 start;              /* Start address */
    u32 end;                /* End address (exclusive, past last insn) */
    CodeType mode;          /* ARM or Thumb */
    u32 successors[2];      /* [0] = fallthrough, [1] = branch target; 0 = none */
    int num_successors;
    bool is_call_target;    /* Reached via BL (function entry) */
    bool is_return;         /* Ends with BX LR or equivalent */
    bool has_indirect;      /* Contains indirect branch (BX Rm where Rm != LR) */
} BasicBlock;

/* A discovered function */
typedef struct Function {
    u32 entry;              /* Entry address */
    CodeType mode;          /* Entry mode (ARM or Thumb) */
    u32* block_addrs;       /* Addresses of basic blocks in this function */
    int num_blocks;
    int cap_blocks;
    bool is_leaf;           /* No BL instructions inside */
    bool has_switch;        /* Contains a detected jump table */
} Function;

/* Jump table entry */
typedef struct JumpTable {
    u32 branch_addr;        /* Address of the indirect branch instruction */
    u32 table_addr;         /* Address of the pointer/offset table in ROM */
    u32* targets;           /* Resolved target addresses */
    int num_entries;
    bool is_thumb;          /* Mode of the targets */
} JumpTable;

/* Work queue entry for recursive descent */
typedef struct WorkItem {
    u32 addr;
    CodeType mode;
    u32 caller;             /* Address that led to this (0 = entry point) */
    bool is_call;           /* true if reached via BL/BLX */
} WorkItem;

/* Main analysis context */
typedef struct AnalysisCtx {
    const GbaRom* rom;

    /* Code map: one byte per ROM halfword (covers both ARM and Thumb granularity) */
    u8* codemap;            /* CodeType for each halfword in ROM */
    u32 codemap_size;       /* Number of entries */

    /* Discovered basic blocks (sorted by start address) */
    BasicBlock* blocks;
    int num_blocks;
    int cap_blocks;

    /* Discovered functions */
    Function* functions;
    int num_functions;
    int cap_functions;

    /* Jump tables */
    JumpTable* jump_tables;
    int num_jump_tables;
    int cap_jump_tables;

    /* Work queue */
    WorkItem* queue;
    int queue_head;
    int queue_tail;
    int queue_cap;

    /* Stats */
    u32 arm_insn_count;
    u32 thumb_insn_count;
    u32 total_functions;
    u32 total_blocks;
} AnalysisCtx;

/* Create an analysis context for a ROM. */
AnalysisCtx* analysis_create(const GbaRom* rom);

/* Free an analysis context and all its data. */
void analysis_free(AnalysisCtx* ctx);

/* Run the full analysis pipeline: recursive descent from entry point. */
void analysis_run(AnalysisCtx* ctx);

/* Add an entry point to analyze (address + mode). Called before analysis_run. */
void analysis_add_entry(AnalysisCtx* ctx, u32 addr, CodeType mode);

/* Look up the basic block containing an address. Returns NULL if not found. */
BasicBlock* analysis_find_block(AnalysisCtx* ctx, u32 addr);

/* Look up a function by entry address. Returns NULL if not found. */
Function* analysis_find_function(AnalysisCtx* ctx, u32 entry);

/* Get the code type at a ROM address. */
CodeType analysis_get_type(const AnalysisCtx* ctx, u32 addr);

/* Print analysis summary to stdout. */
void analysis_print_summary(const AnalysisCtx* ctx);

/* Print all discovered functions to stdout. */
void analysis_print_functions(const AnalysisCtx* ctx);

/* Print detailed info for a specific function. */
void analysis_print_function_detail(const AnalysisCtx* ctx, const Function* func);

#endif /* GBA_ANALYSIS_H */
