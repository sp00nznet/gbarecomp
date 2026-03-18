/*
 * GBA Runtime Library - Standalone Implementation
 *
 * Provides memory bus, CPU state, and basic hardware emulation
 * for statically recompiled GBA games.
 *
 * This is the minimal standalone runtime. For full accuracy,
 * replace with libmgba integration.
 */

#include "gba/gba_runtime.h"
#include <stdio.h>
#include <stdlib.h>

/* ---- CPU State ---- */

u32  r[16] = {0};
bool CPU_N = false;
bool CPU_Z = false;
bool CPU_C = false;
bool CPU_V = false;

static u32 cpsr = 0x0000001F; /* System mode, all flags clear */
static u32 spsr = 0;

/* ---- Memory ---- */

/* GBA memory regions */
static u8* bios_mem  = NULL;  /* 16KB */
static u8* ewram     = NULL;  /* 256KB */
static u8* iwram     = NULL;  /* 32KB */
static u8* io_regs   = NULL;  /* 1KB */
static u8* palette   = NULL;  /* 1KB */
static u8* vram      = NULL;  /* 96KB */
static u8* oam       = NULL;  /* 1KB */
static u8* rom_data  = NULL;  /* Up to 32MB */
static u32 rom_size  = 0;
static u8* sram      = NULL;  /* 64KB */

/* ---- Memory Bus Implementation ---- */

u32 bus_read32(u32 addr) {
    addr &= ~3u; /* Word-align */
    u32 region = addr >> 24;
    u32 offset;

    switch (region) {
    case 0x00: /* BIOS */
        offset = addr & 0x3FFF;
        if (bios_mem) {
            return bios_mem[offset] | (bios_mem[offset+1] << 8) |
                   (bios_mem[offset+2] << 16) | (bios_mem[offset+3] << 24);
        }
        return 0;

    case 0x02: /* EWRAM */
        offset = addr & 0x3FFFF;
        return ewram[offset] | (ewram[offset+1] << 8) |
               (ewram[offset+2] << 16) | (ewram[offset+3] << 24);

    case 0x03: /* IWRAM */
        offset = addr & 0x7FFF;
        return iwram[offset] | (iwram[offset+1] << 8) |
               (iwram[offset+2] << 16) | (iwram[offset+3] << 24);

    case 0x04: /* I/O */
        offset = addr & 0x3FF;
        return io_regs[offset] | (io_regs[offset+1] << 8) |
               (io_regs[offset+2] << 16) | (io_regs[offset+3] << 24);

    case 0x05: /* Palette */
        offset = addr & 0x3FF;
        return palette[offset] | (palette[offset+1] << 8) |
               (palette[offset+2] << 16) | (palette[offset+3] << 24);

    case 0x06: /* VRAM */
        offset = addr & 0x1FFFF;
        if (offset >= 0x18000) offset -= 0x8000; /* Mirror */
        return vram[offset] | (vram[offset+1] << 8) |
               (vram[offset+2] << 16) | (vram[offset+3] << 24);

    case 0x07: /* OAM */
        offset = addr & 0x3FF;
        return oam[offset] | (oam[offset+1] << 8) |
               (oam[offset+2] << 16) | (oam[offset+3] << 24);

    case 0x08: case 0x09: /* ROM */
    case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: {
        offset = (addr - 0x08000000) % rom_size;
        return rom_data[offset] | (rom_data[offset+1] << 8) |
               (rom_data[offset+2] << 16) | (rom_data[offset+3] << 24);
    }

    case 0x0E: case 0x0F: /* SRAM */
        if (sram) {
            offset = addr & 0xFFFF;
            /* SRAM is 8-bit bus, reads return same byte in all positions */
            u8 val = sram[offset];
            return val | (val << 8) | (val << 16) | (val << 24);
        }
        return 0;

    default:
        return 0; /* Open bus */
    }
}

u16 bus_read16(u32 addr) {
    addr &= ~1u;
    u32 region = addr >> 24;
    u32 offset;

    switch (region) {
    case 0x02: offset = addr & 0x3FFFF;  return ewram[offset] | (ewram[offset+1] << 8);
    case 0x03: offset = addr & 0x7FFF;   return iwram[offset] | (iwram[offset+1] << 8);
    case 0x04: offset = addr & 0x3FF;    return io_regs[offset] | (io_regs[offset+1] << 8);
    case 0x05: offset = addr & 0x3FF;    return palette[offset] | (palette[offset+1] << 8);
    case 0x06:
        offset = addr & 0x1FFFF;
        if (offset >= 0x18000) offset -= 0x8000;
        return vram[offset] | (vram[offset+1] << 8);
    case 0x07: offset = addr & 0x3FF;    return oam[offset] | (oam[offset+1] << 8);
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        offset = (addr - 0x08000000) % rom_size;
        return rom_data[offset] | (rom_data[offset+1] << 8);
    default: return (u16)bus_read32(addr);
    }
}

u8 bus_read8(u32 addr) {
    u32 region = addr >> 24;
    u32 offset;

    switch (region) {
    case 0x02: return ewram[addr & 0x3FFFF];
    case 0x03: return iwram[addr & 0x7FFF];
    case 0x04: return io_regs[addr & 0x3FF];
    case 0x05: return palette[addr & 0x3FF];
    case 0x06:
        offset = addr & 0x1FFFF;
        if (offset >= 0x18000) offset -= 0x8000;
        return vram[offset];
    case 0x07: return oam[addr & 0x3FF];
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        offset = (addr - 0x08000000) % rom_size;
        return rom_data[offset];
    case 0x0E: case 0x0F:
        return sram ? sram[addr & 0xFFFF] : 0;
    default: return 0;
    }
}

static inline void mem_write8(u8* mem, u32 offset, u8 val) {
    mem[offset] = val;
}

static inline void mem_write16(u8* mem, u32 offset, u16 val) {
    mem[offset]   = (u8)(val);
    mem[offset+1] = (u8)(val >> 8);
}

static inline void mem_write32(u8* mem, u32 offset, u32 val) {
    mem[offset]   = (u8)(val);
    mem[offset+1] = (u8)(val >> 8);
    mem[offset+2] = (u8)(val >> 16);
    mem[offset+3] = (u8)(val >> 24);
}

void bus_write32(u32 addr, u32 value) {
    addr &= ~3u;
    u32 region = addr >> 24;

    switch (region) {
    case 0x02: mem_write32(ewram, addr & 0x3FFFF, value); break;
    case 0x03: mem_write32(iwram, addr & 0x7FFF, value); break;
    case 0x04: mem_write32(io_regs, addr & 0x3FF, value); break;
    case 0x05: mem_write32(palette, addr & 0x3FF, value); break;
    case 0x06: {
        u32 offset = addr & 0x1FFFF;
        if (offset >= 0x18000) offset -= 0x8000;
        mem_write32(vram, offset, value);
        break;
    }
    case 0x07: mem_write32(oam, addr & 0x3FF, value); break;
    case 0x0E: case 0x0F:
        if (sram) sram[addr & 0xFFFF] = (u8)value;
        break;
    default: break; /* ROM writes ignored */
    }
}

void bus_write16(u32 addr, u16 value) {
    addr &= ~1u;
    u32 region = addr >> 24;

    switch (region) {
    case 0x02: mem_write16(ewram, addr & 0x3FFFF, value); break;
    case 0x03: mem_write16(iwram, addr & 0x7FFF, value); break;
    case 0x04: mem_write16(io_regs, addr & 0x3FF, value); break;
    case 0x05: mem_write16(palette, addr & 0x3FF, value); break;
    case 0x06: {
        u32 offset = addr & 0x1FFFF;
        if (offset >= 0x18000) offset -= 0x8000;
        mem_write16(vram, offset, value);
        break;
    }
    case 0x07: mem_write16(oam, addr & 0x3FF, value); break;
    case 0x0E: case 0x0F:
        if (sram) sram[addr & 0xFFFF] = (u8)value;
        break;
    default: break;
    }
}

void bus_write8(u32 addr, u8 value) {
    u32 region = addr >> 24;

    switch (region) {
    case 0x02: ewram[addr & 0x3FFFF] = value; break;
    case 0x03: iwram[addr & 0x7FFF] = value; break;
    case 0x04: io_regs[addr & 0x3FF] = value; break;
    /* Palette, VRAM: 8-bit writes behave specially on GBA */
    case 0x05: {
        u32 offset = addr & 0x3FE; /* Force halfword aligned */
        palette[offset] = value;
        palette[offset+1] = value; /* Duplicate byte */
        break;
    }
    case 0x06: {
        u32 offset = addr & 0x1FFFE;
        if (offset >= 0x18000) offset -= 0x8000;
        vram[offset] = value;
        vram[offset+1] = value;
        break;
    }
    case 0x0E: case 0x0F:
        if (sram) sram[addr & 0xFFFF] = value;
        break;
    default: break;
    }
}

/* ---- CPSR/SPSR ---- */

u32 cpu_get_cpsr(void) {
    u32 flags = 0;
    if (CPU_N) flags |= (1u << 31);
    if (CPU_Z) flags |= (1u << 30);
    if (CPU_C) flags |= (1u << 29);
    if (CPU_V) flags |= (1u << 28);
    return flags | (cpsr & 0x0FFFFFFF);
}

void cpu_set_cpsr(u32 value, u32 mask) {
    if (mask & 8) { /* Flags field */
        CPU_N = (value >> 31) & 1;
        CPU_Z = (value >> 30) & 1;
        CPU_C = (value >> 29) & 1;
        CPU_V = (value >> 28) & 1;
    }
    if (mask & 1) { /* Control field */
        cpsr = (cpsr & ~0xFF) | (value & 0xFF);
    }
}

u32 cpu_get_spsr(void) { return spsr; }

void cpu_set_spsr(u32 value, u32 mask) {
    if (mask & 8) spsr = (spsr & 0x0FFFFFFF) | (value & 0xF0000000);
    if (mask & 1) spsr = (spsr & ~0xFF) | (value & 0xFF);
}

/* ---- Software Interrupts (BIOS Calls) ---- */

void gba_swi(u32 number) {
    switch (number) {
    case 0x00: /* SoftReset */
        /* TODO: reset state */
        break;
    case 0x01: /* RegisterRamReset */
        /* TODO: selective reset */
        break;
    case 0x02: /* Halt */
        /* Wait for interrupt - in recompiled code, this is a no-op */
        break;
    case 0x04: /* IntrWait */
    case 0x05: /* VBlankIntrWait */
        /* In recompiled code: advance frame */
        gba_frame();
        break;
    case 0x06: { /* Div */
        s32 num = (s32)r[0];
        s32 den = (s32)r[1];
        if (den != 0) {
            r[0] = (u32)(num / den);
            r[1] = (u32)(num % den);
            r[3] = (u32)(num < 0 ? -num / (den < 0 ? -den : den)
                                 : num / (den < 0 ? -den : den));
        }
        break;
    }
    case 0x07: { /* DivArm (swapped args) */
        s32 den = (s32)r[0];
        s32 num = (s32)r[1];
        if (den != 0) {
            r[0] = (u32)(num / den);
            r[1] = (u32)(num % den);
            r[3] = (u32)(num < 0 ? -num / (den < 0 ? -den : den)
                                 : num / (den < 0 ? -den : den));
        }
        break;
    }
    case 0x08: { /* Sqrt */
        u32 val = r[0];
        u32 result = 0;
        u32 bit = 1u << 30;
        while (bit > val) bit >>= 2;
        while (bit != 0) {
            if (val >= result + bit) {
                val -= result + bit;
                result = (result >> 1) + bit;
            } else {
                result >>= 1;
            }
            bit >>= 2;
        }
        r[0] = result;
        break;
    }
    case 0x0B: /* CpuSet */
    case 0x0C: { /* CpuFastSet */
        u32 src = r[0];
        u32 dst = r[1];
        u32 cnt = r[2];
        u32 count = cnt & 0x1FFFFF;
        bool fill = (cnt >> 24) & 1;
        bool word = (number == 0x0C) || ((cnt >> 26) & 1);

        if (word) {
            u32 fill_val = bus_read32(src);
            for (u32 i = 0; i < count; i++) {
                u32 val = fill ? fill_val : bus_read32(src + i * 4);
                bus_write32(dst + i * 4, val);
            }
        } else {
            u16 fill_val = bus_read16(src);
            for (u32 i = 0; i < count; i++) {
                u16 val = fill ? fill_val : bus_read16(src + i * 2);
                bus_write16(dst + i * 2, val);
            }
        }
        break;
    }
    default:
        fprintf(stderr, "[runtime] Unhandled SWI 0x%02X at PC=0x%08X\n", number, r[15]);
        break;
    }
}

/* ---- Indirect Branch Dispatch ---- */

void cpu_bx(u32 target) {
    fprintf(stderr, "[runtime] Unresolved BX to 0x%08X at PC=0x%08X\n", target, r[15]);
    /* TODO: function pointer table lookup */
}

void cpu_undefined(u32 insn) {
    fprintf(stderr, "[runtime] Undefined instruction 0x%08X at PC=0x%08X\n", insn, r[15]);
}

/* ---- Hardware Stubs ---- */

void ppu_render_scanline(void) { /* TODO: libmgba PPU */ }
bool ppu_in_vblank(void) { return false; }
bool ppu_in_hblank(void) { return false; }
void apu_step(u32 cycles) { (void)cycles; }
void dma_check(void) { }
void timer_step(u32 cycles) { (void)cycles; }
void irq_check(void) { }

void gba_frame(void) {
    /* Placeholder: one frame of hardware advancement */
    /* In the future, this drives libmgba's PPU/APU/timers */
}

/* ---- Init/Shutdown ---- */

void gba_init(const char* rom_path) {
    /* Allocate memory regions */
    ewram   = calloc(1, 0x40000);  /* 256KB */
    iwram   = calloc(1, 0x8000);   /* 32KB */
    io_regs = calloc(1, 0x400);    /* 1KB */
    palette = calloc(1, 0x400);    /* 1KB */
    vram    = calloc(1, 0x18000);  /* 96KB */
    oam     = calloc(1, 0x400);    /* 1KB */
    sram    = calloc(1, 0x10000);  /* 64KB */

    /* Load ROM */
    FILE* f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "[runtime] Cannot open ROM: %s\n", rom_path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    rom_size = (u32)ftell(f);
    fseek(f, 0, SEEK_SET);
    rom_data = malloc(rom_size);
    if (fread(rom_data, 1, rom_size, f) != rom_size) {
        fprintf(stderr, "[runtime] Failed to read ROM\n");
        exit(1);
    }
    fclose(f);

    /* Initialize CPU state */
    memset(r, 0, sizeof(r));
    r[13] = 0x03007F00; /* SP (IRQ mode) / system mode */
    r[15] = 0x08000000; /* PC */
    cpsr  = 0x0000001F; /* System mode */

    printf("[runtime] GBA initialized, ROM: %u bytes\n", rom_size);
}

void gba_shutdown(void) {
    free(ewram);   ewram = NULL;
    free(iwram);   iwram = NULL;
    free(io_regs); io_regs = NULL;
    free(palette); palette = NULL;
    free(vram);    vram = NULL;
    free(oam);     oam = NULL;
    free(rom_data); rom_data = NULL;
    free(sram);    sram = NULL;
    printf("[runtime] GBA shutdown\n");
}
