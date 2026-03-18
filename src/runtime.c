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
#include "gba/display.h"
#include <SDL2/SDL.h>
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
u8* io_regs   = NULL;  /* 1KB - non-static for display.c access */
u8* palette   = NULL;  /* 1KB - non-static for display.c access */
u8* vram      = NULL;  /* 96KB - non-static for display.c access */
u8* oam       = NULL;  /* 1KB - non-static for display.c access */
static u8* rom_data  = NULL;  /* Up to 32MB */
static u32 rom_size  = 0;
static u8* sram      = NULL;  /* 64KB */

/* ---- Hardware Timing ---- */

static u32 cycle_counter = 0;   /* Approximate cycle counter */
static u32 scanline = 0;        /* Current VCOUNT (0-227) */
static u32 frame_count = 0;
static u32 last_poll_cycle = 0;

#define CYCLES_PER_SCANLINE  1232  /* ~1232 cycles per scanline */
#define SCANLINES_PER_FRAME  228   /* 160 visible + 68 VBlank */
#define CYCLES_PER_FRAME     (CYCLES_PER_SCANLINE * SCANLINES_PER_FRAME)
#define POLL_INTERVAL        10000 /* Poll SDL events every N cycles */

/* Advance the hardware cycle counter. Called periodically from bus access. */
static void advance_cycles(u32 cycles) {
    cycle_counter += cycles;

    /* Update scanline counter */
    u32 new_scanline = (cycle_counter / CYCLES_PER_SCANLINE) % SCANLINES_PER_FRAME;
    if (new_scanline != scanline) {
        scanline = new_scanline;
        /* Update VCOUNT I/O register */
        io_regs[0x006] = (u8)(scanline & 0xFF);
        io_regs[0x007] = 0;

        /* Update DISPSTAT VBlank/HBlank flags */
        u16 dispstat = io_regs[0x004] | (io_regs[0x005] << 8);
        dispstat &= ~0x0003; /* Clear VBlank and HBlank */
        if (scanline >= 160) dispstat |= 1; /* VBlank */
        io_regs[0x004] = (u8)(dispstat);
        io_regs[0x005] = (u8)(dispstat >> 8);

        /* At start of VBlank, render frame */
        if (scanline == 160) {
            frame_count++;
            /* Update KEYINPUT */
            u16 keys = display_get_keys();
            io_regs[0x130] = (u8)(keys);
            io_regs[0x131] = (u8)(keys >> 8);

            /* Debug: print DISPCNT and VRAM status every 60 frames */
            if (frame_count <= 5 || frame_count % 60 == 0) {
                u16 dispcnt = io_regs[0] | (io_regs[1] << 8);
                /* Check if VRAM has any non-zero data */
                int vram_nonzero = 0;
                for (int i = 0; i < 0x18000; i++) {
                    if (vram[i] != 0) { vram_nonzero++; }
                }
                int pal_nonzero = 0;
                for (int i = 0; i < 0x400; i++) {
                    if (palette[i] != 0) { pal_nonzero++; }
                }
                fprintf(stderr, "[frame %u] DISPCNT=0x%04X mode=%d BG_en=%d%d%d%d OBJ=%d | VRAM: %d bytes | PAL: %d bytes\n",
                        frame_count, dispcnt, dispcnt & 7,
                        (dispcnt >> 8) & 1, (dispcnt >> 9) & 1,
                        (dispcnt >> 10) & 1, (dispcnt >> 11) & 1,
                        (dispcnt >> 12) & 1,
                        vram_nonzero, pal_nonzero);
                fflush(stderr);
            }

            display_render_frame();

            /* Frame timing (~60fps) */
            SDL_Delay(16);
        }
    }

    /* Periodically poll SDL events to prevent window freeze */
    if (cycle_counter - last_poll_cycle > POLL_INTERVAL) {
        last_poll_cycle = cycle_counter;
        if (display_poll_events()) {
            gba_shutdown();
            exit(0);
        }
    }
}

/* ---- DMA Controller ---- */

/* DMA register offsets from 0x040000B0 */
#define DMA_REG_BASE  0x0B0
#define DMA_SAD(n)    (DMA_REG_BASE + (n) * 12 + 0)
#define DMA_DAD(n)    (DMA_REG_BASE + (n) * 12 + 4)
#define DMA_CNT(n)    (DMA_REG_BASE + (n) * 12 + 8)

static int dma_count = 0;
static void dma_execute(int channel) {
    u32 base = DMA_REG_BASE + channel * 12;
    u32 src = io_regs[base] | (io_regs[base+1] << 8) |
              (io_regs[base+2] << 16) | (io_regs[base+3] << 24);
    u32 dst = io_regs[base+4] | (io_regs[base+5] << 8) |
              (io_regs[base+6] << 16) | (io_regs[base+7] << 24);
    u16 cnt_lo = io_regs[base+8] | (io_regs[base+9] << 8);
    u16 cnt_hi = io_regs[base+10] | (io_regs[base+11] << 8);

    bool enable = (cnt_hi >> 15) & 1;
    if (!enable) return;

    u32 count = cnt_lo;
    if (count == 0) {
        /* DMA0-2: 0 means 0x4000, DMA3: 0 means 0x10000 */
        count = (channel == 3) ? 0x10000 : 0x4000;
    }

    bool word = (cnt_hi >> 10) & 1;     /* 0=halfword, 1=word */
    int dst_ctrl = (cnt_hi >> 5) & 3;   /* 0=inc, 1=dec, 2=fixed, 3=inc/reload */
    int src_ctrl = (cnt_hi >> 7) & 3;   /* 0=inc, 1=dec, 2=fixed */
    int timing = (cnt_hi >> 12) & 3;    /* 0=immediate, 1=VBlank, 2=HBlank, 3=special */

    dma_count++;
    if (dma_count <= 20) {
        fprintf(stderr, "[dma #%d] ch%d: 0x%08X -> 0x%08X, cnt=%u, %s, timing=%d\n",
                dma_count, channel, src, dst, count,
                word ? "word" : "half", timing);
        fflush(stderr);
    }

    /* Only handle immediate DMA for now */
    if (timing != 0) {
        /* VBlank/HBlank/Special DMA - defer */
        return;
    }

    u32 size = word ? 4 : 2;

    for (u32 i = 0; i < count; i++) {
        if (word) {
            bus_write32(dst, bus_read32(src));
        } else {
            bus_write16(dst, bus_read16(src));
        }

        /* Source address control */
        switch (src_ctrl) {
            case 0: src += size; break;
            case 1: src -= size; break;
            case 2: break; /* fixed */
        }

        /* Destination address control */
        switch (dst_ctrl) {
            case 0: case 3: dst += size; break;
            case 1: dst -= size; break;
            case 2: break; /* fixed */
        }
    }

    /* Clear enable bit after transfer (for immediate DMA) */
    cnt_hi &= ~(1 << 15);
    io_regs[base+10] = (u8)(cnt_hi);
    io_regs[base+11] = (u8)(cnt_hi >> 8);
}

/* ---- I/O Write Hook ---- */

static int io_write_count = 0;
static void io_write_hook(u32 offset, u32 value, int size) {
    io_write_count++;
    if (io_write_count <= 30) {
        fprintf(stderr, "[io #%d] write 0x%03X = 0x%04X (size=%d)\n",
                io_write_count, offset, value & 0xFFFF, size);
        fflush(stderr);
    }
    /* Check for DMA enable writes */
    for (int ch = 0; ch < 4; ch++) {
        u32 cnt_hi_offset = DMA_REG_BASE + ch * 12 + 10;
        if (offset == cnt_hi_offset || offset == cnt_hi_offset + 1) {
            /* DMA control high byte was written - check if enable bit set */
            u16 cnt_hi = io_regs[cnt_hi_offset] | (io_regs[cnt_hi_offset+1] << 8);
            if (cnt_hi & (1 << 15)) {
                dma_execute(ch);
            }
        }
    }
}

/* ---- Memory Bus Implementation ---- */

u32 bus_read32(u32 addr) {
    advance_cycles(4); /* Approximate: each bus access ~4 cycles */
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
    advance_cycles(2);
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
    advance_cycles(2);
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
    case 0x04: {
        u32 off = addr & 0x3FF;
        mem_write32(io_regs, off, value);
        io_write_hook(off, value, 4);
        io_write_hook(off + 2, value >> 16, 4);
        break;
    }
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
    case 0x04: {
        u32 off = addr & 0x3FF;
        mem_write16(io_regs, off, value);
        io_write_hook(off, value, 2);
        break;
    }
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

static int swi_count = 0;
void gba_swi(u32 number) {
    swi_count++;
    if (swi_count <= 20) {
        fprintf(stderr, "[swi #%d] SWI 0x%02X\n", swi_count, number);
        fflush(stderr);
    }
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

/* ---- BX Dispatch Tracing ---- */
static int bx_call_count = 0;
static u32 last_bx_target = 0;
static int last_bx_repeat = 0;

void cpu_bx_trace(u32 target) {
    bx_call_count++;
    if (target == last_bx_target) {
        last_bx_repeat++;
    } else {
        if (last_bx_repeat > 0 && bx_call_count < 200) {
            fprintf(stderr, "  (repeated %d times)\n", last_bx_repeat);
        }
        last_bx_repeat = 0;
        last_bx_target = target;
        if (bx_call_count <= 100) {
            fprintf(stderr, "[bx #%d] -> 0x%08X\n", bx_call_count, target);
            fflush(stderr);
        }
    }
}

/* cpu_bx() is generated in game_entry.c with a dispatch table */

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
    /* Fast-forward to next VBlank by advancing cycles.
     * This is called from SWI VBlankIntrWait. */
    u32 target = ((cycle_counter / CYCLES_PER_FRAME) + 1) * CYCLES_PER_FRAME
                 + 160 * CYCLES_PER_SCANLINE;
    while (cycle_counter < target) {
        advance_cycles(CYCLES_PER_SCANLINE);
    }
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

    /* Initialize KEYINPUT to all buttons released (active-low) */
    io_regs[0x130] = 0xFF;
    io_regs[0x131] = 0x03;

    printf("[runtime] GBA initialized, ROM: %u bytes\n", rom_size);
    fflush(stdout);

    /* Initialize display */
    if (display_init() != 0) {
        fprintf(stderr, "[runtime] Failed to initialize display\n");
        exit(1);
    }
    printf("[runtime] Display initialized (SDL2)\n");
    fflush(stdout);
}

void gba_shutdown(void) {
    display_shutdown();
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
