#ifndef GBA_TRANSLATE_H
#define GBA_TRANSLATE_H

#include "gba/types.h"
#include "gba/rom.h"
#include "gba/arm.h"
#include "gba/thumb.h"
#include "gba/analysis.h"
#include <stdio.h>

/* Translation context */
typedef struct {
    const GbaRom* rom;
    const AnalysisCtx* analysis;
    FILE* out;              /* Output C file */
    int indent;             /* Current indentation level */
} TranslateCtx;

/* Create a translator context. `out` is the output file to write C code to. */
TranslateCtx* translate_create(const GbaRom* rom, const AnalysisCtx* analysis, FILE* out);

/* Free a translator context. Does NOT close the output file. */
void translate_free(TranslateCtx* ctx);

/* Emit the C runtime header (register file, bus access macros, etc.) */
void translate_emit_header(TranslateCtx* ctx);

/* Emit the C runtime footer (main function, etc.) */
void translate_emit_footer(TranslateCtx* ctx);

/* Translate a single function to C code. */
void translate_function(TranslateCtx* ctx, const Function* func);

/* Translate all discovered functions. */
void translate_all(TranslateCtx* ctx);

/* Translate a single ARM instruction to C statements. */
void translate_arm_insn(TranslateCtx* ctx, const ArmInsn* insn, u32 addr);

/* Translate a single Thumb instruction to C statements. */
void translate_thumb_insn(TranslateCtx* ctx, const ThumbInsn* insn, u32 addr);

#endif /* GBA_TRANSLATE_H */
