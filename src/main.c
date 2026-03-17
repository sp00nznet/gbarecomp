#include "gba/rom.h"
#include "gba/arm.h"
#include "gba/thumb.h"
#include "gba/disasm.h"
#include "gba/analysis.h"
#include "gba/translate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char* prog) {
    fprintf(stderr, "gbarecomp - GBA Static Recompilation Toolkit\n\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s info <rom.gba>                          Show ROM info\n", prog);
    fprintf(stderr, "  %s disasm <rom.gba> [options]              Disassemble ROM\n", prog);
    fprintf(stderr, "  %s analyze <rom.gba> [options]             Analyze ROM code\n", prog);
    fprintf(stderr, "  %s translate <rom.gba> -o <output.c>       Translate to C\n", prog);
    fprintf(stderr, "\nDisasm options:\n");
    fprintf(stderr, "  --start <addr>    Start address (hex, default: entry point)\n");
    fprintf(stderr, "  --count <n>       Number of instructions (default: 64)\n");
    fprintf(stderr, "  --thumb           Disassemble as Thumb code\n");
    fprintf(stderr, "  --arm             Disassemble as ARM code (default)\n");
    fprintf(stderr, "\nAnalyze options:\n");
    fprintf(stderr, "  --functions       List all discovered functions\n");
    fprintf(stderr, "  --detail <addr>   Show detailed disassembly of a function\n");
    fprintf(stderr, "\nTranslate options:\n");
    fprintf(stderr, "  -o <file>         Output C file path (required)\n");
}

static u32 parse_hex(const char* s) {
    return (u32)strtoul(s, NULL, 16);
}

static int cmd_info(const char* rom_path) {
    GbaRom* rom = rom_load(rom_path);
    if (!rom) return 1;

    rom_print_info(rom);

    u32 entry_raw = rom_read32(rom, GBA_ROM_START);
    ArmInsn entry = arm_decode(entry_raw);
    char buf[256];
    disasm_arm(&entry, GBA_ROM_START, buf, sizeof(buf));
    printf("\nEntry instruction:\n");
    printf("  %08X:  %08X  %s\n", GBA_ROM_START, entry_raw, buf);

    if (entry.type == ARM_B || entry.type == ARM_BL) {
        u32 target = GBA_ROM_START + 8 + (u32)entry.branch_offset;
        printf("  -> Branch target: 0x%08X (offset +0x%X into ROM)\n",
               target, target - GBA_ROM_START);
    }

    rom_free(rom);
    return 0;
}

static int cmd_disasm(const char* rom_path, int argc, char* argv[]) {
    u32 start = 0;
    bool start_set = false;
    int count = 64;
    bool thumb = false;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
            start = parse_hex(argv[++i]);
            start_set = true;
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            count = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--thumb") == 0) {
            thumb = true;
        } else if (strcmp(argv[i], "--arm") == 0) {
            thumb = false;
        }
    }

    GbaRom* rom = rom_load(rom_path);
    if (!rom) return 1;

    if (!start_set) {
        u32 entry_raw = rom_read32(rom, GBA_ROM_START);
        ArmInsn entry = arm_decode(entry_raw);
        if (entry.type == ARM_B) {
            start = GBA_ROM_START + 8 + (u32)entry.branch_offset;
            printf("; Entry point branches to 0x%08X\n", start);
        } else {
            start = GBA_ROM_START;
        }
    }

    u32 insn_size = thumb ? 2 : 4;
    u32 end = start + (u32)(count * (int)insn_size);

    u32 rom_end = GBA_ROM_START + rom->size;
    if (end > rom_end) end = rom_end;

    printf("; Disassembly of %s (%s mode)\n", rom->title, thumb ? "Thumb" : "ARM");
    printf("; Range: 0x%08X - 0x%08X (%d instructions)\n\n", start, end, count);

    if (thumb) {
        disasm_thumb_range(rom, start, end);
    } else {
        disasm_arm_range(rom, start, end);
    }

    rom_free(rom);
    return 0;
}

static int cmd_analyze(const char* rom_path, int argc, char* argv[]) {
    bool show_functions = false;
    u32 detail_addr = 0;
    bool show_detail = false;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--functions") == 0) {
            show_functions = true;
        } else if (strcmp(argv[i], "--detail") == 0 && i + 1 < argc) {
            detail_addr = parse_hex(argv[++i]);
            show_detail = true;
        }
    }

    GbaRom* rom = rom_load(rom_path);
    if (!rom) return 1;

    rom_print_info(rom);

    AnalysisCtx* ctx = analysis_create(rom);
    analysis_run(ctx);
    analysis_print_summary(ctx);

    if (show_functions) {
        analysis_print_functions(ctx);
    }

    if (show_detail) {
        Function* func = analysis_find_function(ctx, detail_addr);
        if (func) {
            analysis_print_function_detail(ctx, func);
        } else {
            fprintf(stderr, "Function at 0x%08X not found\n", detail_addr);
        }
    }

    analysis_free(ctx);
    rom_free(rom);
    return 0;
}

static int cmd_translate(const char* rom_path, int argc, char* argv[]) {
    const char* output_path = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        }
    }

    if (!output_path) {
        fprintf(stderr, "Error: -o <output.c> is required\n");
        return 1;
    }

    GbaRom* rom = rom_load(rom_path);
    if (!rom) return 1;

    rom_print_info(rom);

    /* Analyze */
    printf("\n--- Analysis Phase ---\n");
    AnalysisCtx* analysis = analysis_create(rom);
    analysis_run(analysis);
    analysis_print_summary(analysis);

    /* Translate */
    printf("\n--- Translation Phase ---\n");
    FILE* out = fopen(output_path, "w");
    if (!out) {
        fprintf(stderr, "Error: cannot open '%s' for writing\n", output_path);
        analysis_free(analysis);
        rom_free(rom);
        return 1;
    }

    TranslateCtx* translator = translate_create(rom, analysis, out);
    translate_all(translator);
    translate_free(translator);

    fclose(out);
    printf("Generated: %s\n", output_path);
    printf("Functions translated: %d\n", analysis->num_functions);

    analysis_free(analysis);
    rom_free(rom);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char* command = argv[1];
    const char* rom_path = argv[2];

    if (strcmp(command, "info") == 0) {
        return cmd_info(rom_path);
    } else if (strcmp(command, "disasm") == 0) {
        return cmd_disasm(rom_path, argc - 3, argv + 3);
    } else if (strcmp(command, "analyze") == 0) {
        return cmd_analyze(rom_path, argc - 3, argv + 3);
    } else if (strcmp(command, "translate") == 0) {
        return cmd_translate(rom_path, argc - 3, argv + 3);
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        print_usage(argv[0]);
        return 1;
    }
}
