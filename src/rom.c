#include "gba/rom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GbaRom* rom_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < (long)sizeof(GbaRomHeader)) {
        fprintf(stderr, "Error: file too small to be a GBA ROM (%ld bytes)\n", file_size);
        fclose(f);
        return NULL;
    }

    if (file_size > GBA_ROM_MAX_SIZE) {
        fprintf(stderr, "Warning: file larger than max GBA ROM size (%ld bytes)\n", file_size);
    }

    GbaRom* rom = calloc(1, sizeof(GbaRom));
    if (!rom) {
        fclose(f);
        return NULL;
    }

    rom->size = (u32)file_size;
    rom->data = malloc(rom->size);
    if (!rom->data) {
        free(rom);
        fclose(f);
        return NULL;
    }

    if (fread(rom->data, 1, rom->size, f) != rom->size) {
        fprintf(stderr, "Error: failed to read ROM data\n");
        free(rom->data);
        free(rom);
        fclose(f);
        return NULL;
    }
    fclose(f);

    /* Parse header */
    memcpy(&rom->header, rom->data, sizeof(GbaRomHeader));

    /* Extract null-terminated title */
    memcpy(rom->title, rom->header.title, 12);
    rom->title[12] = '\0';

    /* Extract null-terminated game code */
    memcpy(rom->game_code, rom->header.game_code, 4);
    rom->game_code[4] = '\0';

    return rom;
}

void rom_free(GbaRom* rom) {
    if (rom) {
        free(rom->data);
        free(rom);
    }
}

void rom_print_info(const GbaRom* rom) {
    printf("=== GBA ROM Info ===\n");
    printf("  Title:       %s\n", rom->title);
    printf("  Game Code:   %s\n", rom->game_code);
    printf("  Maker Code:  %.2s\n", rom->header.maker_code);
    printf("  ROM Size:    %u bytes (%.1f MB)\n", rom->size, rom->size / (1024.0 * 1024.0));
    printf("  Entry Point: 0x%08X\n", rom->header.entry_point);
    printf("  Fixed Value: 0x%02X %s\n", rom->header.fixed_value,
           rom->header.fixed_value == 0x96 ? "(OK)" : "(INVALID)");
    printf("  Unit Code:   0x%02X\n", rom->header.unit_code);
    printf("  Version:     %u\n", rom->header.software_version);
    printf("  Checksum:    0x%02X %s\n", rom->header.header_checksum,
           rom_validate_header(rom) ? "(OK)" : "(INVALID)");
    printf("====================\n");
}

u32 rom_read32(const GbaRom* rom, u32 addr) {
    u32 offset;
    /* Handle mirrored ROM regions: 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D */
    if (addr >= 0x08000000 && addr < 0x0E000000) {
        offset = (addr - 0x08000000) % rom->size;
    } else {
        offset = addr;
    }
    if (offset + 4 > rom->size) return 0;
    return rom->data[offset]
         | (rom->data[offset + 1] << 8)
         | (rom->data[offset + 2] << 16)
         | (rom->data[offset + 3] << 24);
}

u16 rom_read16(const GbaRom* rom, u32 addr) {
    u32 offset;
    if (addr >= 0x08000000 && addr < 0x0E000000) {
        offset = (addr - 0x08000000) % rom->size;
    } else {
        offset = addr;
    }
    if (offset + 2 > rom->size) return 0;
    return rom->data[offset] | (rom->data[offset + 1] << 8);
}

u8 rom_read8(const GbaRom* rom, u32 addr) {
    u32 offset;
    if (addr >= 0x08000000 && addr < 0x0E000000) {
        offset = (addr - 0x08000000) % rom->size;
    } else {
        offset = addr;
    }
    if (offset >= rom->size) return 0;
    return rom->data[offset];
}

bool rom_validate_header(const GbaRom* rom) {
    /* Checksum is complement of sum of bytes 0xA0-0xBC */
    u8 checksum = 0;
    for (int i = 0xA0; i <= 0xBC; i++) {
        checksum -= rom->data[i];
    }
    checksum -= 0x19;
    return checksum == rom->header.header_checksum;
}
