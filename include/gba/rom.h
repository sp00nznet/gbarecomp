#ifndef GBA_ROM_H
#define GBA_ROM_H

#include "types.h"

/* GBA ROM header (192 bytes at offset 0) */
typedef struct {
    u32 entry_point;        /* Branch instruction to ROM start */
    u8  logo[156];          /* Nintendo logo (compressed bitmap) */
    char title[12];         /* Game title (uppercase ASCII) */
    char game_code[4];      /* Game code (e.g., "AWRE" for Advance Wars) */
    char maker_code[2];     /* Maker code (e.g., "01" for Nintendo) */
    u8  fixed_value;        /* Must be 0x96 */
    u8  unit_code;          /* 0x00 for GBA */
    u8  device_type;        /* Usually 0x00 */
    u8  reserved1[7];
    u8  software_version;   /* Usually 0x00 */
    u8  header_checksum;    /* Complement check of bytes 0xA0-0xBC */
    u8  reserved2[2];
} GbaRomHeader;

/* Loaded ROM image */
typedef struct {
    u8*  data;              /* Raw ROM data */
    u32  size;              /* ROM size in bytes */
    GbaRomHeader header;    /* Parsed header */
    char title[13];         /* Null-terminated title */
    char game_code[5];      /* Null-terminated game code */
} GbaRom;

/* Load a ROM from a file path. Returns NULL on error. */
GbaRom* rom_load(const char* path);

/* Free a loaded ROM. */
void rom_free(GbaRom* rom);

/* Print ROM header info to stdout. */
void rom_print_info(const GbaRom* rom);

/* Read a 32-bit word from ROM at a given address (must be ROM-region address). */
u32 rom_read32(const GbaRom* rom, u32 addr);

/* Read a 16-bit halfword from ROM at a given address. */
u16 rom_read16(const GbaRom* rom, u32 addr);

/* Read a byte from ROM at a given address. */
u8 rom_read8(const GbaRom* rom, u32 addr);

/* Validate ROM header checksum. Returns true if valid. */
bool rom_validate_header(const GbaRom* rom);

#endif /* GBA_ROM_H */
