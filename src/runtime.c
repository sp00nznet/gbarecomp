/*
 * GBA Runtime Library - Static Recompilation Runtime
 *
 * This is the standalone runtime for statically recompiled GBA games.
 * No emulator runs underneath - recompiled C code IS the CPU.
 * Memory is flat arrays, I/O writes dispatch to lightweight hardware modules.
 *
 * Hardware provided:
 *   - Flat memory bus (EWRAM, IWRAM, VRAM, Palette, OAM, ROM, SRAM)
 *   - 4 hardware timers with prescaler and cascade
 *   - 4-channel DMA with immediate, VBlank, and HBlank triggers
 *   - Scanline-based timing (VCOUNT, DISPSTAT, HBlank/VBlank flags)
 *   - Interrupt delivery (IE/IF/IME -> handler at [0x03007FFC])
 *   - BIOS HLE (SWI implementations for Div, Sqrt, CpuSet, etc.)
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

/* Save file path for SRAM persistence */
static char sram_path[512] = {0};

/* ---- I/O Register Helpers ---- */

static inline u16 io_read16(u32 offset) {
    return (u16)io_regs[offset] | ((u16)io_regs[offset + 1] << 8);
}

static inline void io_write16(u32 offset, u16 val) {
    io_regs[offset]     = (u8)(val);
    io_regs[offset + 1] = (u8)(val >> 8);
}

static inline u32 io_read32(u32 offset) {
    return io_regs[offset] | (io_regs[offset+1] << 8) |
           (io_regs[offset+2] << 16) | (io_regs[offset+3] << 24);
}

static inline void io_write32(u32 offset, u32 val) {
    io_regs[offset]   = (u8)(val);
    io_regs[offset+1] = (u8)(val >> 8);
    io_regs[offset+2] = (u8)(val >> 16);
    io_regs[offset+3] = (u8)(val >> 24);
}

/* ---- Hardware Timing ---- */

static u32 cycle_counter = 0;
static u32 scanline = 0;          /* Current VCOUNT (0-227) */
static u32 frame_count = 0;
static u32 scanline_cycles = 0;   /* Cycles within current scanline */
static u32 last_poll_cycle = 0;

#define CYCLES_PER_SCANLINE  1232
#define VISIBLE_SCANLINES    160
#define VBLANK_SCANLINES     68
#define SCANLINES_PER_FRAME  228
#define CYCLES_PER_FRAME     (CYCLES_PER_SCANLINE * SCANLINES_PER_FRAME)
#define HBLANK_START_CYCLE   960   /* HBlank starts ~960 cycles into scanline */
#define POLL_INTERVAL        10000

/* ---- Forward Declarations ---- */

static void timer_tick(u32 cycles);
static void dma_execute(int channel);
static void dma_trigger_vblank(void);
static void dma_trigger_hblank(void);
static void check_interrupts(void);
static void io_write_hook(u32 offset, u32 value, int size);

/* ---- Timer Hardware ---- */

/* GBA has 4 hardware timers (TM0-TM3).
 * Each has a 16-bit counter, reload value, and control register.
 * Timers can cascade (increment when the previous timer overflows). */

static const u16 timer_prescaler_shift[4] = { 0, 6, 8, 10 };
/* Prescaler 0=1, 1=64, 2=256, 3=1024 -> shift values for division */

typedef struct {
    u32 internal_counter;  /* Sub-prescaler accumulator */
    u16 counter;           /* Current timer value (TMXCNT_L read value) */
    u16 reload;            /* Reload value (TMXCNT_L write value) */
    u16 control;           /* TMXCNT_H */
    bool running;          /* Cached from control bit 7 */
    bool cascade;          /* Cached from control bit 2 */
    int prescaler;         /* Cached prescaler selection (0-3) */
} HWTimer;

static HWTimer timers[4] = {0};

#define TIMER_REG_BASE  0x100
#define TIMER_CNT_L(n)  (TIMER_REG_BASE + (n) * 4)
#define TIMER_CNT_H(n)  (TIMER_REG_BASE + (n) * 4 + 2)

/* Timer overflow: increment counter, check for cascade/IRQ */
static void timer_overflow(int idx) {
    timers[idx].counter = timers[idx].reload;

    /* Fire timer IRQ if enabled */
    if (timers[idx].control & (1 << 6)) {
        u16 if_val = io_read16(0x202);
        if_val |= (u16)(1 << (3 + idx)); /* Timer 0-3 = IRQ bits 3-6 */
        io_write16(0x202, if_val);
    }

    /* Cascade: if next timer exists and is in cascade mode, tick it */
    if (idx < 3 && timers[idx + 1].running && timers[idx + 1].cascade) {
        timers[idx + 1].counter++;
        if (timers[idx + 1].counter == 0) { /* Overflow */
            timer_overflow(idx + 1);
        }
    }
}

static void timer_tick(u32 cycles) {
    for (int i = 0; i < 4; i++) {
        if (!timers[i].running || timers[i].cascade) continue;

        u32 shift = timer_prescaler_shift[timers[i].prescaler];
        timers[i].internal_counter += cycles;

        u32 ticks = timers[i].internal_counter >> shift;
        timers[i].internal_counter &= (1u << shift) - 1;

        while (ticks > 0) {
            u32 until_overflow = (u32)(0x10000 - timers[i].counter);
            if (ticks >= until_overflow) {
                ticks -= until_overflow;
                timer_overflow(i);
            } else {
                timers[i].counter += (u16)ticks;
                ticks = 0;
            }
        }
    }
}

/* Handle timer control register write */
static void timer_write_control(int idx, u16 value) {
    bool was_running = timers[idx].running;
    bool now_running = (value >> 7) & 1;

    timers[idx].control = value;
    timers[idx].prescaler = value & 3;
    timers[idx].cascade = (value >> 2) & 1;
    timers[idx].running = now_running;

    /* Starting a stopped timer reloads the counter */
    if (!was_running && now_running) {
        timers[idx].counter = timers[idx].reload;
        timers[idx].internal_counter = 0;
    }

    /* Write back to I/O registers */
    io_write16(TIMER_CNT_H(idx), value);
}

/* ---- DMA Controller ---- */

#define DMA_REG_BASE  0x0B0
#define DMA_SAD(n)    (DMA_REG_BASE + (n) * 12)
#define DMA_DAD(n)    (DMA_REG_BASE + (n) * 12 + 4)
#define DMA_CNT_L(n)  (DMA_REG_BASE + (n) * 12 + 8)
#define DMA_CNT_H(n)  (DMA_REG_BASE + (n) * 12 + 10)

/* Latched source/destination addresses (reloaded on DMA enable) */
static u32 dma_src_latch[4] = {0};
static u32 dma_dst_latch[4] = {0};

static void dma_transfer(int channel) {
    u32 base = DMA_REG_BASE + channel * 12;
    u32 src = dma_src_latch[channel];
    u32 dst = dma_dst_latch[channel];
    u16 cnt_lo = io_read16(base + 8);
    u16 cnt_hi = io_read16(base + 10);

    u32 count = cnt_lo;
    if (count == 0) {
        count = (channel == 3) ? 0x10000 : 0x4000;
    }

    bool word = (cnt_hi >> 10) & 1;
    int dst_ctrl = (cnt_hi >> 5) & 3;
    int src_ctrl = (cnt_hi >> 7) & 3;
    u32 size = word ? 4 : 2;

    for (u32 i = 0; i < count; i++) {
        if (word) {
            /* Direct memory copy for DMA - bypass bus_read/write cycle counting */
            u32 val = bus_read32(src);
            bus_write32(dst, val);
        } else {
            u16 val = bus_read16(src);
            bus_write16(dst, val);
        }

        switch (src_ctrl) {
            case 0: src += size; break;
            case 1: src -= size; break;
            case 2: break;
        }
        switch (dst_ctrl) {
            case 0: case 3: dst += size; break;
            case 1: dst -= size; break;
            case 2: break;
        }
    }

    /* Update latched addresses */
    dma_src_latch[channel] = src;
    if (dst_ctrl != 3) {
        dma_dst_latch[channel] = dst;
    }
    /* dst_ctrl 3 = increment/reload: dst reloads on next trigger */

    /* Check repeat bit */
    bool repeat = (cnt_hi >> 9) & 1;
    int timing = (cnt_hi >> 12) & 3;

    if (!repeat || timing == 0) {
        /* Clear enable bit */
        cnt_hi &= ~(1u << 15);
        io_write16(base + 10, cnt_hi);
    }

    /* Fire DMA IRQ if enabled */
    if (cnt_hi & (1 << 14)) {
        u16 if_val = io_read16(0x202);
        if_val |= (u16)(1 << (8 + channel));
        io_write16(0x202, if_val);
    }
}

static void dma_execute(int channel) {
    u16 cnt_hi = io_read16(DMA_CNT_H(channel));
    if (!(cnt_hi & (1 << 15))) return;

    int timing = (cnt_hi >> 12) & 3;
    if (timing == 0) {
        /* Immediate DMA */
        dma_transfer(channel);
    }
    /* VBlank/HBlank/Special DMAs are deferred to their trigger points */
}

/* Latch addresses when DMA is enabled */
static void dma_enable(int channel) {
    u32 base = DMA_REG_BASE + channel * 12;
    dma_src_latch[channel] = io_read32(base);
    dma_dst_latch[channel] = io_read32(base + 4);
}

static void dma_trigger_vblank(void) {
    for (int ch = 0; ch < 4; ch++) {
        u16 cnt_hi = io_read16(DMA_CNT_H(ch));
        if ((cnt_hi & (1 << 15)) && ((cnt_hi >> 12) & 3) == 1) {
            dma_transfer(ch);
        }
    }
}

static void dma_trigger_hblank(void) {
    for (int ch = 0; ch < 4; ch++) {
        u16 cnt_hi = io_read16(DMA_CNT_H(ch));
        if ((cnt_hi & (1 << 15)) && ((cnt_hi >> 12) & 3) == 2) {
            dma_transfer(ch);
        }
    }
}

/* ---- Interrupt Delivery ---- */

static bool in_irq = false;

static void check_interrupts(void) {
    if (in_irq) return;

    u16 ime = io_read16(0x208);
    if (!ime) return;

    u16 ie = io_read16(0x200);
    u16 if_val = io_read16(0x202);
    u16 pending = ie & if_val;
    if (!pending) return;

    /* Read handler address from 0x03007FFC (set by game's crt0) */
    u32 handler_addr = iwram[0x7FFC] | (iwram[0x7FFD] << 8) |
                       (iwram[0x7FFE] << 16) | (iwram[0x7FFF] << 24);
    if (handler_addr == 0) return;

    in_irq = true;

    /* Set BIOS IF flags at 0x03007FF8 (for IntrWait/VBlankIntrWait) */
    u16 bios_if = iwram[0x7FF8] | (iwram[0x7FF9] << 8);
    bios_if |= pending;
    iwram[0x7FF8] = (u8)(bios_if);
    iwram[0x7FF9] = (u8)(bios_if >> 8);

    /* Save recompiled state (IRQ handler runs with its own context) */
    u32 saved_r[16];
    bool saved_N = CPU_N, saved_Z = CPU_Z, saved_C = CPU_C, saved_V = CPU_V;
    memcpy(saved_r, r, sizeof(r));

    /* Call the handler via BX dispatch */
    cpu_bx(handler_addr);

    /* Restore game state */
    memcpy(r, saved_r, sizeof(r));
    CPU_N = saved_N; CPU_Z = saved_Z; CPU_C = saved_C; CPU_V = saved_V;

    in_irq = false;
}

/* ---- Scanline Scheduler ---- */

/* Advance hardware by a number of cycles. This is the heartbeat:
 * tracks scanlines, fires HBlank/VBlank, triggers DMA, delivers IRQs. */
static void advance_cycles(u32 cycles) {
    cycle_counter += cycles;
    scanline_cycles += cycles;

    /* Tick timers */
    timer_tick(cycles);

    /* Process complete scanlines */
    while (scanline_cycles >= CYCLES_PER_SCANLINE) {
        scanline_cycles -= CYCLES_PER_SCANLINE;

        u32 prev_scanline = scanline;
        scanline = (scanline + 1) % SCANLINES_PER_FRAME;

        /* Update VCOUNT */
        io_regs[0x006] = (u8)(scanline & 0xFF);
        io_regs[0x007] = 0;

        /* Update DISPSTAT */
        u16 dispstat = io_read16(0x004);
        u16 vcount_target = (dispstat >> 8) & 0xFF;

        dispstat &= ~0x0007; /* Clear VBlank, HBlank, VCount match */
        if (scanline >= VISIBLE_SCANLINES)
            dispstat |= 1; /* VBlank flag */
        if (scanline == vcount_target)
            dispstat |= 4; /* VCount match flag */
        io_write16(0x004, dispstat);

        /* VBlank start (scanline 160) */
        if (scanline == VISIBLE_SCANLINES && prev_scanline != VISIBLE_SCANLINES) {
            /* Set VBlank IRQ flag if VBlank IRQ enabled in DISPSTAT */
            if (dispstat & (1 << 3)) {
                u16 if_val = io_read16(0x202);
                io_write16(0x202, if_val | 1); /* VBlank = bit 0 */
            }

            /* Trigger VBlank DMA */
            dma_trigger_vblank();

            /* Render frame and handle input */
            frame_count++;

            /* Update KEYINPUT from SDL */
            u16 keys = display_get_keys();
            io_regs[0x130] = (u8)(keys);
            io_regs[0x131] = (u8)(keys >> 8);

            display_render_frame();

            /* Frame pacing */
            SDL_Delay(16);

            /* Debug status */
            if (frame_count <= 5 || frame_count % 300 == 0) {
                u16 dispcnt = io_read16(0x000);
                fprintf(stderr, "[frame %u] DISPCNT=0x%04X mode=%d BG=%d%d%d%d OBJ=%d\n",
                        frame_count, dispcnt, dispcnt & 7,
                        (dispcnt >> 8) & 1, (dispcnt >> 9) & 1,
                        (dispcnt >> 10) & 1, (dispcnt >> 11) & 1,
                        (dispcnt >> 12) & 1);
                fflush(stderr);
            }
        }

        /* VCount match IRQ */
        if ((scanline == vcount_target) && (dispstat & (1 << 5))) {
            u16 if_val = io_read16(0x202);
            io_write16(0x202, if_val | 4); /* VCount = bit 2 */
        }

        /* HBlank fires at end of each visible scanline */
        if (scanline < VISIBLE_SCANLINES) {
            /* HBlank IRQ */
            if (dispstat & (1 << 4)) {
                u16 if_val = io_read16(0x202);
                io_write16(0x202, if_val | 2); /* HBlank = bit 1 */
            }
            /* HBlank DMA */
            dma_trigger_hblank();
        }

        /* Check for interrupts after each scanline */
        check_interrupts();
    }

    /* Periodically poll SDL events */
    if (cycle_counter - last_poll_cycle > POLL_INTERVAL) {
        last_poll_cycle = cycle_counter;
        if (display_poll_events()) {
            gba_shutdown();
            exit(0);
        }
    }
}

/* ---- I/O Write Hook ---- */

/* Dispatches side effects when the game writes to I/O registers. */
static void io_write_hook(u32 offset, u32 value, int size) {
    (void)value; (void)size;

    /* DMA control writes */
    for (int ch = 0; ch < 4; ch++) {
        u32 cnt_hi_off = DMA_CNT_H(ch);
        if (offset == cnt_hi_off || offset == cnt_hi_off + 1) {
            u16 cnt_hi = io_read16(cnt_hi_off);
            if (cnt_hi & (1 << 15)) {
                dma_enable(ch);
                dma_execute(ch);
            }
        }
    }

    /* Timer control writes */
    for (int t = 0; t < 4; t++) {
        if (offset == TIMER_CNT_L(t) || offset == TIMER_CNT_L(t) + 1) {
            /* Write to CNT_L sets the reload value (not the running counter) */
            timers[t].reload = io_read16(TIMER_CNT_L(t));
        }
        if (offset == TIMER_CNT_H(t) || offset == TIMER_CNT_H(t) + 1) {
            timer_write_control(t, io_read16(TIMER_CNT_H(t)));
        }
    }

    /* IF register write: writing 1 bits CLEARS them (acknowledge) */
    if (offset == 0x202 || offset == 0x203) {
        /* The game wrote to IF - but GBA IF is write-1-to-clear.
         * We need to read what was written and clear those bits from the
         * actual IF value. The write already happened to io_regs, so we
         * need to undo it and apply the clear semantics. */
        /* This is handled specially in bus_write16 for I/O region */
    }

    /* HALTCNT write (0x04000301) - power down / halt */
    if (offset == 0x301) {
        /* Halt: advance to next interrupt */
        /* In recompiled code, just advance a frame */
    }
}

/* ---- Memory Bus Implementation ---- */

static inline void mem_write8(u8* mem, u32 off, u8 val) { mem[off] = val; }

static inline void mem_write16_raw(u8* mem, u32 off, u16 val) {
    mem[off]   = (u8)(val);
    mem[off+1] = (u8)(val >> 8);
}

static inline void mem_write32_raw(u8* mem, u32 off, u32 val) {
    mem[off]   = (u8)(val);
    mem[off+1] = (u8)(val >> 8);
    mem[off+2] = (u8)(val >> 16);
    mem[off+3] = (u8)(val >> 24);
}

u32 bus_read32(u32 addr) {
    advance_cycles(4);
    addr &= ~3u;
    u32 region = addr >> 24;
    u32 offset;

    switch (region) {
    case 0x00:
        offset = addr & 0x3FFF;
        if (bios_mem) {
            return bios_mem[offset] | (bios_mem[offset+1] << 8) |
                   (bios_mem[offset+2] << 16) | (bios_mem[offset+3] << 24);
        }
        return 0;

    case 0x02:
        offset = addr & 0x3FFFF;
        return ewram[offset] | (ewram[offset+1] << 8) |
               (ewram[offset+2] << 16) | (ewram[offset+3] << 24);

    case 0x03:
        offset = addr & 0x7FFF;
        return iwram[offset] | (iwram[offset+1] << 8) |
               (iwram[offset+2] << 16) | (iwram[offset+3] << 24);

    case 0x04: {
        offset = addr & 0x3FF;
        /* Timer counter reads return the running counter, not the reload */
        for (int t = 0; t < 4; t++) {
            if (offset == (u32)TIMER_CNT_L(t)) {
                u16 cnt = timers[t].running ? timers[t].counter : timers[t].reload;
                u16 ctl = timers[t].control;
                return (u32)cnt | ((u32)ctl << 16);
            }
        }
        return io_regs[offset] | (io_regs[offset+1] << 8) |
               (io_regs[offset+2] << 16) | (io_regs[offset+3] << 24);
    }

    case 0x05:
        offset = addr & 0x3FF;
        return palette[offset] | (palette[offset+1] << 8) |
               (palette[offset+2] << 16) | (palette[offset+3] << 24);

    case 0x06:
        offset = addr & 0x1FFFF;
        if (offset >= 0x18000) offset -= 0x8000;
        return vram[offset] | (vram[offset+1] << 8) |
               (vram[offset+2] << 16) | (vram[offset+3] << 24);

    case 0x07:
        offset = addr & 0x3FF;
        return oam[offset] | (oam[offset+1] << 8) |
               (oam[offset+2] << 16) | (oam[offset+3] << 24);

    case 0x08: case 0x09:
    case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: {
        offset = (addr - 0x08000000) % rom_size;
        return rom_data[offset] | (rom_data[offset+1] << 8) |
               (rom_data[offset+2] << 16) | (rom_data[offset+3] << 24);
    }

    case 0x0E: case 0x0F:
        if (sram) {
            offset = addr & 0xFFFF;
            u8 val = sram[offset];
            return val | (val << 8) | (val << 16) | (val << 24);
        }
        return 0;

    default:
        return 0;
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
    case 0x04: {
        offset = addr & 0x3FF;
        /* Timer counter reads */
        for (int t = 0; t < 4; t++) {
            if (offset == (u32)TIMER_CNT_L(t))
                return timers[t].running ? timers[t].counter : timers[t].reload;
            if (offset == (u32)TIMER_CNT_H(t))
                return timers[t].control;
        }
        return io_regs[offset] | (io_regs[offset+1] << 8);
    }
    case 0x05: offset = addr & 0x3FF;    return palette[offset] | (palette[offset+1] << 8);
    case 0x06:
        offset = addr & 0x1FFFF;
        if (offset >= 0x18000) offset -= 0x8000;
        return vram[offset] | (vram[offset+1] << 8);
    case 0x07: offset = addr & 0x3FF;    return oam[offset] | (oam[offset+1] << 8);
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        offset = (addr - 0x08000000) % rom_size;
        return rom_data[offset] | (rom_data[offset+1] << 8);
    case 0x0E: case 0x0F:
        return sram ? sram[addr & 0xFFFF] : 0;
    default: return 0;
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

void bus_write32(u32 addr, u32 value) {
    addr &= ~3u;
    u32 region = addr >> 24;

    switch (region) {
    case 0x02: mem_write32_raw(ewram, addr & 0x3FFFF, value); break;
    case 0x03: mem_write32_raw(iwram, addr & 0x7FFF, value); break;
    case 0x04: {
        u32 off = addr & 0x3FF;
        /* IF register: write-1-to-clear semantics */
        if (off == 0x200) {
            /* Lower 16 bits = IE (normal write), upper 16 bits = IF (write-1-to-clear) */
            mem_write16_raw(io_regs, 0x200, (u16)value); /* IE */
            u16 if_clear = (u16)(value >> 16);
            u16 if_val = io_read16(0x202);
            io_write16(0x202, if_val & ~if_clear);
        } else {
            mem_write32_raw(io_regs, off, value);
            io_write_hook(off, value, 4);
            io_write_hook(off + 2, value >> 16, 4);
        }
        break;
    }
    case 0x05: mem_write32_raw(palette, addr & 0x3FF, value); break;
    case 0x06: {
        u32 offset = addr & 0x1FFFF;
        if (offset >= 0x18000) offset -= 0x8000;
        mem_write32_raw(vram, offset, value);
        break;
    }
    case 0x07: mem_write32_raw(oam, addr & 0x3FF, value); break;
    case 0x0E: case 0x0F:
        if (sram) sram[addr & 0xFFFF] = (u8)value;
        break;
    default: break;
    }
}

void bus_write16(u32 addr, u16 value) {
    addr &= ~1u;
    u32 region = addr >> 24;

    switch (region) {
    case 0x02: mem_write16_raw(ewram, addr & 0x3FFFF, value); break;
    case 0x03: mem_write16_raw(iwram, addr & 0x7FFF, value); break;
    case 0x04: {
        u32 off = addr & 0x3FF;
        /* IF register: write-1-to-clear */
        if (off == 0x202) {
            u16 if_val = io_read16(0x202);
            io_write16(0x202, if_val & ~value);
        } else {
            mem_write16_raw(io_regs, off, value);
            io_write_hook(off, value, 2);
        }
        break;
    }
    case 0x05: mem_write16_raw(palette, addr & 0x3FF, value); break;
    case 0x06: {
        u32 offset = addr & 0x1FFFF;
        if (offset >= 0x18000) offset -= 0x8000;
        mem_write16_raw(vram, offset, value);
        break;
    }
    case 0x07: mem_write16_raw(oam, addr & 0x3FF, value); break;
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
    case 0x04: {
        u32 off = addr & 0x3FF;
        /* IF byte write: write-1-to-clear */
        if (off == 0x202 || off == 0x203) {
            u16 if_val = io_read16(0x202);
            if (off == 0x202)
                io_write16(0x202, if_val & ~(u16)value);
            else
                io_write16(0x202, if_val & ~((u16)value << 8));
        } else {
            io_regs[off] = value;
            io_write_hook(off, value, 1);
        }
        break;
    }
    case 0x05: {
        u32 offset = addr & 0x3FE;
        palette[offset] = value;
        palette[offset+1] = value;
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
    if (mask & 8) {
        CPU_N = (value >> 31) & 1;
        CPU_Z = (value >> 30) & 1;
        CPU_C = (value >> 29) & 1;
        CPU_V = (value >> 28) & 1;
    }
    if (mask & 1) {
        cpsr = (cpsr & ~0xFF) | (value & 0xFF);
    }
}

u32 cpu_get_spsr(void) { return spsr; }

void cpu_set_spsr(u32 value, u32 mask) {
    if (mask & 8) spsr = (spsr & 0x0FFFFFFF) | (value & 0xF0000000);
    if (mask & 1) spsr = (spsr & ~0xFF) | (value & 0xFF);
}

/* ---- Software Interrupts (BIOS HLE) ---- */

void gba_swi(u32 number) {
    switch (number) {
    case 0x00: /* SoftReset */
        memset(iwram + 0x7E00, 0, 0x200);
        memset(r, 0, sizeof(r));
        r[13] = 0x03007F00;
        r[15] = 0x08000000;
        cpsr = 0x0000001F;
        break;

    case 0x01: { /* RegisterRamReset */
        u32 flags = r[0];
        if (flags & 0x01) memset(ewram, 0, 0x40000);
        if (flags & 0x02) memset(iwram, 0, 0x7E00); /* Preserve top 0x200 */
        if (flags & 0x04) memset(palette, 0, 0x400);
        if (flags & 0x08) memset(vram, 0, 0x18000);
        if (flags & 0x10) memset(oam, 0, 0x400);
        /* bits 5-7: SIO, sound, other registers */
        break;
    }

    case 0x02: /* Halt */
        /* Advance to next interrupt event */
        advance_cycles(CYCLES_PER_SCANLINE);
        break;

    case 0x04: { /* IntrWait */
        bool discard = r[0] != 0;
        u16 wait_flags = (u16)r[1];

        if (discard) {
            /* Clear the BIOS IF flags we're waiting for */
            u16 bios_if = iwram[0x7FF8] | (iwram[0x7FF9] << 8);
            bios_if &= ~wait_flags;
            iwram[0x7FF8] = (u8)(bios_if);
            iwram[0x7FF9] = (u8)(bios_if >> 8);
        }

        /* Spin until the requested interrupt fires */
        int safety = 0;
        while (safety < SCANLINES_PER_FRAME * 2) {
            advance_cycles(CYCLES_PER_SCANLINE);
            safety++;
            u16 bios_if = iwram[0x7FF8] | (iwram[0x7FF9] << 8);
            if (bios_if & wait_flags) break;
        }
        break;
    }

    case 0x05: /* VBlankIntrWait */
        /* Equivalent to IntrWait(1, 1) - wait for VBlank */
        r[0] = 1;
        r[1] = 1;
        gba_swi(0x04);
        break;

    case 0x06: { /* Div: r0/r1 */
        s32 num = (s32)r[0];
        s32 den = (s32)r[1];
        if (den != 0) {
            r[0] = (u32)(num / den);
            r[1] = (u32)(num % den);
            s32 abs_result = (s32)r[0];
            if (abs_result < 0) abs_result = -abs_result;
            r[3] = (u32)abs_result;
        }
        break;
    }

    case 0x07: { /* DivArm: r1/r0 */
        s32 den = (s32)r[0];
        s32 num = (s32)r[1];
        if (den != 0) {
            r[0] = (u32)(num / den);
            r[1] = (u32)(num % den);
            s32 abs_result = (s32)r[0];
            if (abs_result < 0) abs_result = -abs_result;
            r[3] = (u32)abs_result;
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

    case 0x09: { /* ArcTan */
        /* Approximation: atan(r0) where r0 is 1.14 fixed-point */
        s32 a = (s32)(s16)r[0];
        s32 result = a - (a * a * a / 3) / (1 << 28);
        r[0] = (u32)(s16)result;
        break;
    }

    case 0x0A: { /* ArcTan2 */
        /* Two-argument arctangent, returns angle in [0, 0xFFFF] */
        s16 x = (s16)r[0];
        s16 y = (s16)r[1];
        /* Simple approximation using atan2 */
        if (x == 0 && y == 0) {
            r[0] = 0;
        } else {
            double angle = SDL_atan2((double)y, (double)x);
            /* Convert from [-pi, pi] to [0, 0xFFFF] */
            double normalized = angle / (2.0 * 3.14159265358979323846) + 0.5;
            if (normalized < 0) normalized += 1.0;
            if (normalized >= 1.0) normalized -= 1.0;
            r[0] = (u32)(u16)(normalized * 65536.0);
        }
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

        /* CpuFastSet always works in 32-byte chunks */
        if (number == 0x0C) {
            count = (count + 7) & ~7u; /* Round up to multiple of 8 */
        }

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

    case 0x0E: { /* BgAffineSet */
        u32 src = r[0];
        u32 dst = r[1];
        u32 count = r[2];
        for (u32 i = 0; i < count; i++) {
            /* Read source parameters (20 bytes each) */
            s32 cx = (s32)bus_read32(src + i * 20);
            s32 cy = (s32)bus_read32(src + i * 20 + 4);
            s16 dispx = (s16)bus_read16(src + i * 20 + 8);
            s16 dispy = (s16)bus_read16(src + i * 20 + 10);
            s16 sx = (s16)bus_read16(src + i * 20 + 12);
            s16 sy = (s16)bus_read16(src + i * 20 + 14);
            u16 angle_raw = bus_read16(src + i * 20 + 16);

            double angle = (double)angle_raw / 65536.0 * 2.0 * 3.14159265358979323846;
            double cosA = SDL_cos(angle);
            double sinA = SDL_sin(angle);

            /* PA = sx * cos(angle) / 256, PB = -sx * sin(angle) / 256, etc. */
            s16 pa = (s16)(cosA * 256.0 * 256.0 / sx);
            s16 pb = (s16)(-sinA * 256.0 * 256.0 / sx);
            s16 pc = (s16)(sinA * 256.0 * 256.0 / sy);
            s16 pd = (s16)(cosA * 256.0 * 256.0 / sy);

            /* Reference point */
            s32 dx = cx - (s32)dispx * pa - (s32)dispy * pb;
            s32 dy = cy - (s32)dispx * pc - (s32)dispy * pd;

            /* Write dest (16 bytes each) */
            bus_write16(dst + i * 16, (u16)pa);
            bus_write16(dst + i * 16 + 2, (u16)pb);
            bus_write16(dst + i * 16 + 4, (u16)pc);
            bus_write16(dst + i * 16 + 6, (u16)pd);
            bus_write32(dst + i * 16 + 8, (u32)dx);
            bus_write32(dst + i * 16 + 12, (u32)dy);
        }
        break;
    }

    case 0x0F: { /* ObjAffineSet */
        u32 src = r[0];
        u32 dst = r[1];
        u32 count = r[2];
        u32 stride = r[3]; /* Offset between PA entries in dest (8 for OAM, 2 for buffer) */

        for (u32 i = 0; i < count; i++) {
            s16 sx = (s16)bus_read16(src + i * 8);
            s16 sy = (s16)bus_read16(src + i * 8 + 2);
            u16 angle_raw = bus_read16(src + i * 8 + 4);

            double angle = (double)angle_raw / 65536.0 * 2.0 * 3.14159265358979323846;
            double cosA = SDL_cos(angle);
            double sinA = SDL_sin(angle);

            s16 pa = (s16)(cosA * 256.0 * 256.0 / sx);
            s16 pb = (s16)(-sinA * 256.0 * 256.0 / sx);
            s16 pc = (s16)(sinA * 256.0 * 256.0 / sy);
            s16 pd = (s16)(cosA * 256.0 * 256.0 / sy);

            bus_write16(dst + i * stride * 4, (u16)pa);
            bus_write16(dst + i * stride * 4 + stride, (u16)pb);
            bus_write16(dst + i * stride * 4 + stride * 2, (u16)pc);
            bus_write16(dst + i * stride * 4 + stride * 3, (u16)pd);
        }
        break;
    }

    case 0x10: { /* BitUnPack */
        u32 src = r[0];
        u32 dst = r[1];
        u32 info = r[2];
        u16 src_len = bus_read16(info);
        u8 src_bpp = bus_read8(info + 2);
        u8 dst_bpp = bus_read8(info + 3);
        u32 data_offset = bus_read32(info + 4);
        bool zero_flag = (data_offset >> 31) & 1;
        data_offset &= 0x7FFFFFFF;

        u32 src_off = 0;
        u32 dst_off = 0;
        u32 buffer = 0;
        int bits_in_buffer = 0;
        u8 src_mask = (u8)((1 << src_bpp) - 1);

        while (src_off < src_len) {
            u8 byte = bus_read8(src + src_off++);
            for (int bit = 0; bit < 8; bit += src_bpp) {
                u32 val = (byte >> bit) & src_mask;
                if (val != 0 || !zero_flag) {
                    val += (u32)data_offset;
                }
                buffer |= val << bits_in_buffer;
                bits_in_buffer += dst_bpp;
                if (bits_in_buffer >= 32) {
                    bus_write32(dst + dst_off, buffer);
                    dst_off += 4;
                    buffer = 0;
                    bits_in_buffer = 0;
                }
            }
        }
        if (bits_in_buffer > 0) {
            bus_write32(dst + dst_off, buffer);
        }
        break;
    }

    case 0x11: { /* LZ77UnCompWram */
        u32 src = r[0];
        u32 dst = r[1];
        u32 header = bus_read32(src);
        u32 decomp_size = header >> 8;
        u32 sp = src + 4;
        u32 dp = 0;

        while (dp < decomp_size) {
            u8 flags = bus_read8(sp++);
            for (int i = 7; i >= 0 && dp < decomp_size; i--) {
                if (flags & (1 << i)) {
                    /* Compressed */
                    u8 b1 = bus_read8(sp++);
                    u8 b2 = bus_read8(sp++);
                    u32 length = ((b1 >> 4) & 0xF) + 3;
                    u32 disp = ((u32)(b1 & 0xF) << 8) | b2;
                    u32 ref = dp - disp - 1;
                    for (u32 j = 0; j < length && dp < decomp_size; j++) {
                        bus_write8(dst + dp, bus_read8(dst + ref + j));
                        dp++;
                    }
                } else {
                    /* Uncompressed */
                    bus_write8(dst + dp++, bus_read8(sp++));
                }
            }
        }
        break;
    }

    case 0x12: { /* LZ77UnCompVram (16-bit writes) */
        u32 src = r[0];
        u32 dst = r[1];
        u32 header = bus_read32(src);
        u32 decomp_size = header >> 8;
        u32 sp = src + 4;
        u32 dp = 0;
        u8 tmp_buf[2] = {0};
        int tmp_idx = 0;

        while (dp < decomp_size) {
            u8 flags = bus_read8(sp++);
            for (int i = 7; i >= 0 && dp < decomp_size; i--) {
                u8 byte;
                if (flags & (1 << i)) {
                    u8 b1 = bus_read8(sp++);
                    u8 b2 = bus_read8(sp++);
                    u32 length = ((b1 >> 4) & 0xF) + 3;
                    u32 disp = ((u32)(b1 & 0xF) << 8) | b2;
                    u32 ref = dp - disp - 1;
                    for (u32 j = 0; j < length && dp < decomp_size; j++) {
                        byte = bus_read8(dst + ref + j);
                        tmp_buf[tmp_idx++] = byte;
                        if (tmp_idx == 2) {
                            bus_write16(dst + dp - 1, (u16)tmp_buf[0] | ((u16)tmp_buf[1] << 8));
                            tmp_idx = 0;
                        }
                        dp++;
                    }
                } else {
                    byte = bus_read8(sp++);
                    tmp_buf[tmp_idx++] = byte;
                    if (tmp_idx == 2) {
                        bus_write16(dst + dp - 1, (u16)tmp_buf[0] | ((u16)tmp_buf[1] << 8));
                        tmp_idx = 0;
                    }
                    dp++;
                }
            }
        }
        break;
    }

    case 0x13: { /* HuffUnComp */
        /* Huffman decompression - stub for now */
        fprintf(stderr, "[swi] HuffUnComp not implemented\n");
        break;
    }

    case 0x14: { /* RLUnCompWram */
        u32 src = r[0];
        u32 dst = r[1];
        u32 header = bus_read32(src);
        u32 decomp_size = header >> 8;
        u32 sp = src + 4;
        u32 dp = 0;

        while (dp < decomp_size) {
            u8 flag = bus_read8(sp++);
            if (flag & 0x80) {
                /* Run-length */
                u32 length = (flag & 0x7F) + 3;
                u8 val = bus_read8(sp++);
                for (u32 j = 0; j < length && dp < decomp_size; j++) {
                    bus_write8(dst + dp++, val);
                }
            } else {
                /* Uncompressed */
                u32 length = (flag & 0x7F) + 1;
                for (u32 j = 0; j < length && dp < decomp_size; j++) {
                    bus_write8(dst + dp++, bus_read8(sp++));
                }
            }
        }
        break;
    }

    case 0x15: { /* RLUnCompVram (16-bit writes) */
        u32 src = r[0];
        u32 dst = r[1];
        u32 header = bus_read32(src);
        u32 decomp_size = header >> 8;
        u32 sp = src + 4;
        u32 dp = 0;
        u8 tmp_buf[2] = {0};
        int tmp_idx = 0;

        while (dp < decomp_size) {
            u8 flag = bus_read8(sp++);
            u32 length;
            if (flag & 0x80) {
                length = (flag & 0x7F) + 3;
                u8 val = bus_read8(sp++);
                for (u32 j = 0; j < length && dp < decomp_size; j++) {
                    tmp_buf[tmp_idx++] = val;
                    if (tmp_idx == 2) {
                        bus_write16(dst + dp - 1, (u16)tmp_buf[0] | ((u16)tmp_buf[1] << 8));
                        tmp_idx = 0;
                    }
                    dp++;
                }
            } else {
                length = (flag & 0x7F) + 1;
                for (u32 j = 0; j < length && dp < decomp_size; j++) {
                    u8 val = bus_read8(sp++);
                    tmp_buf[tmp_idx++] = val;
                    if (tmp_idx == 2) {
                        bus_write16(dst + dp - 1, (u16)tmp_buf[0] | ((u16)tmp_buf[1] << 8));
                        tmp_idx = 0;
                    }
                    dp++;
                }
            }
        }
        break;
    }

    default:
        fprintf(stderr, "[swi] Unhandled SWI 0x%02X at PC=0x%08X\n", number, r[15]);
        break;
    }
}

/* ---- IWRAM Code Execution ---- */

/* Mini Thumb interpreter for RAM code execution.
 * Games copy small routines to IWRAM/EWRAM (crt0 init, decompression,
 * DMA helpers). Rather than requiring all RAM code to be pre-compiled,
 * we interpret it on the fly. This covers the common case of init code
 * that the recompiled ROM functions call via BX to RAM addresses. */
void run_iwram_function(u32 target) {
    static int call_count = 0;
    call_count++;
    if (call_count <= 20) {
        u32 peek_addr = target & ~1u;
        fprintf(stderr, "[interp] Running RAM code at 0x%08X (bytes: %02X %02X %02X %02X) LR=0x%08X SP=0x%08X\n",
                target, bus_read8(peek_addr), bus_read8(peek_addr+1),
                bus_read8(peek_addr+2), bus_read8(peek_addr+3), r[14], r[13]);
        /* Show stack for backtrace */
        fprintf(stderr, "         Stack: [SP]=0x%08X [SP+4]=0x%08X [SP+8]=0x%08X [SP+12]=0x%08X\n",
                bus_read32(r[13]), bus_read32(r[13]+4), bus_read32(r[13]+8), bus_read32(r[13]+12));
        fflush(stderr);
    }

    bool thumb = (target & 1) != 0;
    u32 pc = target & ~1u;
    u32 saved_lr = r[14];

    /* Set LR to a sentinel so we know when to stop */
    u32 return_sentinel = 0xDEAD0000;
    r[14] = return_sentinel | 1; /* Thumb return address */

    int steps = 0;
    int max_steps = 500000;

    while (steps < max_steps) {
        steps++;

        if (!thumb) {
            /* ARM mode - not common for RAM code, but handle BX back */
            u32 insn = bus_read32(pc);
            pc += 4;

            /* Only handle BX LR (return) for now */
            if ((insn & 0x0FFFFFF0) == 0x012FFF10) {
                u32 rm = insn & 0xF;
                u32 addr = r[rm];
                if ((addr & ~1u) == (return_sentinel & ~1u)) break;
                thumb = (addr & 1) != 0;
                pc = addr & ~1u;
                continue;
            }
            /* Unknown ARM instruction in RAM - bail */
            break;
        }

        /* Thumb mode interpreter */
        u16 insn = bus_read16(pc);
        pc += 2;

        /* Format 2: Add/Sub - MUST check before Format 1 (overlapping encoding) */
        if ((insn & 0xF800) == 0x1800) {
            int i = (insn >> 10) & 1;
            int op = (insn >> 9) & 1;
            u32 rn_or_imm = (insn >> 6) & 7;
            int rs = (insn >> 3) & 7;
            int rd = insn & 7;
            u32 operand = i ? rn_or_imm : r[rn_or_imm];
            if (op) cpu_sub(&r[rd], r[rs], operand, true);
            else cpu_add(&r[rd], r[rs], operand, true);
            continue;
        }

        /* Format 1: Move shifted register - LSL/LSR/ASR (NOT add/sub which is Format 2) */
        if ((insn & 0xE000) == 0x0000) {
            int op = (insn >> 11) & 3;
            u32 offset = (insn >> 6) & 0x1F;
            int rs = (insn >> 3) & 7;
            int rd = insn & 7;
            u32 val = r[rs];
            switch (op) {
            case 0: /* LSL */
                if (offset > 0) { CPU_C = (val >> (32 - offset)) & 1; val <<= offset; }
                break;
            case 1: /* LSR */
                if (offset == 0) offset = 32;
                CPU_C = (val >> (offset - 1)) & 1;
                val = offset >= 32 ? 0 : val >> offset;
                break;
            case 2: /* ASR */
                if (offset == 0) offset = 32;
                CPU_C = ((s32)val >> (offset > 31 ? 31 : offset - 1)) & 1;
                val = (u32)((s32)val >> (offset > 31 ? 31 : offset));
                break;
            }
            r[rd] = val;
            cpu_update_nz(val);
            continue;
        }

        /* Format 3: Mov/Cmp/Add/Sub immediate */
        if ((insn & 0xE000) == 0x2000) {
            int op = (insn >> 11) & 3;
            int rd = (insn >> 8) & 7;
            u32 imm = insn & 0xFF;
            switch (op) {
            case 0: r[rd] = imm; cpu_update_nz(imm); break;
            case 1: cpu_sub(NULL, r[rd], imm, true); break;
            case 2: cpu_add(&r[rd], r[rd], imm, true); break;
            case 3: cpu_sub(&r[rd], r[rd], imm, true); break;
            }
            continue;
        }

        /* Format 4: ALU operations */
        if ((insn & 0xFC00) == 0x4000) {
            int op = (insn >> 6) & 0xF;
            int rs = (insn >> 3) & 7;
            int rd = insn & 7;
            u32 a = r[rd], b = r[rs], result;
            switch (op) {
            case 0x0: result = a & b; r[rd] = result; cpu_update_nz(result); break;
            case 0x1: result = a ^ b; r[rd] = result; cpu_update_nz(result); break;
            case 0x2: /* LSL */ {
                u32 shift = b & 0xFF;
                result = shift >= 32 ? 0 : a << shift;
                if (shift > 0 && shift <= 32) CPU_C = (a >> (32 - shift)) & 1;
                r[rd] = result; cpu_update_nz(result); break;
            }
            case 0x3: /* LSR */ {
                u32 shift = b & 0xFF;
                result = shift >= 32 ? 0 : a >> shift;
                if (shift > 0 && shift <= 32) CPU_C = (a >> (shift - 1)) & 1;
                r[rd] = result; cpu_update_nz(result); break;
            }
            case 0x4: /* ASR */ {
                u32 shift = b & 0xFF;
                result = shift >= 32 ? (u32)((s32)a >> 31) : (u32)((s32)a >> shift);
                r[rd] = result; cpu_update_nz(result); break;
            }
            case 0x5: cpu_adc(&r[rd], a, b, true); break;
            case 0x6: cpu_sbc(&r[rd], a, b, true); break;
            case 0x7: /* ROR */ {
                u32 shift = b & 0xFF;
                if (shift == 0) { result = a; }
                else { shift &= 31; result = shift ? (a >> shift) | (a << (32 - shift)) : a; CPU_C = (a >> 31) & 1; }
                r[rd] = result; cpu_update_nz(result); break;
            }
            case 0x8: result = a & b; cpu_update_nz(result); break; /* TST */
            case 0x9: cpu_sub(&r[rd], 0, b, true); break; /* NEG */
            case 0xA: cpu_sub(NULL, a, b, true); break; /* CMP */
            case 0xB: cpu_add(NULL, a, b, true); break; /* CMN */
            case 0xC: result = a | b; r[rd] = result; cpu_update_nz(result); break;
            case 0xD: result = (u32)((s32)a * (s32)b); r[rd] = result; cpu_update_nz(result); break;
            case 0xE: result = a & ~b; r[rd] = result; cpu_update_nz(result); break;
            case 0xF: result = ~b; r[rd] = result; cpu_update_nz(result); break;
            }
            continue;
        }

        /* Format 5: Hi register ops / BX */
        if ((insn & 0xFC00) == 0x4400) {
            int op = (insn >> 8) & 3;
            int h1 = (insn >> 7) & 1;
            int h2 = (insn >> 6) & 1;
            int rs = ((insn >> 3) & 7) | (h2 << 3);
            int rd = (insn & 7) | (h1 << 3);
            u32 val = r[rs];
            if (rs == 15) val = pc + 2;
            switch (op) {
            case 0: r[rd] = r[rd] + val; if (rd == 15) { pc = r[15] & ~1u; } break;
            case 1: cpu_sub(NULL, r[rd], val, true); break;
            case 2: r[rd] = val; if (rd == 15) { pc = r[15] & ~1u; } break;
            case 3: /* BX */
                if ((val & ~1u) == (return_sentinel & ~1u)) goto done;
                /* Check if target is back in ROM - return to recompiled code */
                if ((val >> 24) == 0x08) { r[15] = val; goto done; }
                thumb = (val & 1) != 0;
                pc = val & ~1u;
                break;
            }
            continue;
        }

        /* Format 6: PC-relative load */
        if ((insn & 0xF800) == 0x4800) {
            int rd = (insn >> 8) & 7;
            u32 offset = (insn & 0xFF) << 2;
            u32 addr = ((pc + 2) & ~3u) + offset;
            r[rd] = bus_read32(addr);
            continue;
        }

        /* Format 7/8: Load/store register offset */
        if ((insn & 0xF200) == 0x5000) {
            int ro = (insn >> 6) & 7;
            int rb = (insn >> 3) & 7;
            int rd = insn & 7;
            u32 addr = r[rb] + r[ro];
            int op = (insn >> 10) & 3;
            switch (op) {
            case 0: bus_write32(addr, r[rd]); break;        /* STR */
            case 1: bus_write16(addr, (u16)r[rd]); break;   /* STRH */
            case 2: r[rd] = bus_read32(addr); break;        /* LDR */
            case 3: r[rd] = bus_read16(addr); break;        /* LDRH - zero extend */
            }
            continue;
        }
        if ((insn & 0xF200) == 0x5200) {
            int ro = (insn >> 6) & 7;
            int rb = (insn >> 3) & 7;
            int rd = insn & 7;
            u32 addr = r[rb] + r[ro];
            int op = (insn >> 10) & 3;
            switch (op) {
            case 0: bus_write8(addr, (u8)r[rd]); break;     /* STRB */
            case 1: r[rd] = (u32)(s32)(s8)bus_read8(addr); break; /* LDSB */
            case 2: r[rd] = bus_read8(addr); break;         /* LDRB */
            case 3: r[rd] = (u32)(s32)(s16)bus_read16(addr); break; /* LDSH */
            }
            continue;
        }

        /* Format 9: Load/store immediate offset */
        if ((insn & 0xE000) == 0x6000) {
            int b = (insn >> 12) & 1;
            int l = (insn >> 11) & 1;
            u32 offset = (insn >> 6) & 0x1F;
            int rb = (insn >> 3) & 7;
            int rd = insn & 7;
            if (b) { /* Byte */
                u32 addr = r[rb] + offset;
                if (l) r[rd] = bus_read8(addr);
                else bus_write8(addr, (u8)r[rd]);
            } else { /* Word */
                u32 addr = r[rb] + (offset << 2);
                if (l) r[rd] = bus_read32(addr);
                else bus_write32(addr, r[rd]);
            }
            continue;
        }

        /* Format 10: Load/store halfword */
        if ((insn & 0xF000) == 0x8000) {
            int l = (insn >> 11) & 1;
            u32 offset = ((insn >> 6) & 0x1F) << 1;
            int rb = (insn >> 3) & 7;
            int rd = insn & 7;
            u32 addr = r[rb] + offset;
            if (l) r[rd] = bus_read16(addr);
            else bus_write16(addr, (u16)r[rd]);
            continue;
        }

        /* Format 11: SP-relative load/store */
        if ((insn & 0xF000) == 0x9000) {
            int l = (insn >> 11) & 1;
            int rd = (insn >> 8) & 7;
            u32 offset = (insn & 0xFF) << 2;
            u32 addr = r[13] + offset;
            if (l) r[rd] = bus_read32(addr);
            else bus_write32(addr, r[rd]);
            continue;
        }

        /* Format 12: Load address (ADD Rd, PC/SP, #imm) */
        if ((insn & 0xF000) == 0xA000) {
            int sp = (insn >> 11) & 1;
            int rd = (insn >> 8) & 7;
            u32 offset = (insn & 0xFF) << 2;
            r[rd] = sp ? (r[13] + offset) : (((pc + 2) & ~3u) + offset);
            continue;
        }

        /* Format 13: Add offset to SP */
        if ((insn & 0xFF00) == 0xB000) {
            u32 offset = (insn & 0x7F) << 2;
            if (insn & 0x80) r[13] -= offset;
            else r[13] += offset;
            continue;
        }

        /* Format 14: Push/Pop */
        if ((insn & 0xF600) == 0xB400) {
            int l = (insn >> 11) & 1;
            int pclr = (insn >> 8) & 1;
            u8 rlist = insn & 0xFF;
            if (l) { /* POP */
                for (int i = 0; i < 8; i++) {
                    if (rlist & (1 << i)) { r[i] = bus_read32(r[13]); r[13] += 4; }
                }
                if (pclr) {
                    u32 val = bus_read32(r[13]); r[13] += 4;
                    if ((val & ~1u) == (return_sentinel & ~1u)) goto done;
                    if ((val >> 24) == 0x08) { r[15] = val; goto done; }
                    thumb = (val & 1) != 0;
                    pc = val & ~1u;
                }
            } else { /* PUSH */
                if (pclr) { r[13] -= 4; bus_write32(r[13], r[14]); }
                for (int i = 7; i >= 0; i--) {
                    if (rlist & (1 << i)) { r[13] -= 4; bus_write32(r[13], r[i]); }
                }
            }
            continue;
        }

        /* Format 15: Multiple load/store (LDMIA/STMIA) */
        if ((insn & 0xF000) == 0xC000) {
            int l = (insn >> 11) & 1;
            int rb = (insn >> 8) & 7;
            u8 rlist = insn & 0xFF;
            u32 addr = r[rb];
            for (int i = 0; i < 8; i++) {
                if (rlist & (1 << i)) {
                    if (l) r[i] = bus_read32(addr);
                    else bus_write32(addr, r[i]);
                    addr += 4;
                }
            }
            r[rb] = addr;
            continue;
        }

        /* Format 16: Conditional branch */
        if ((insn & 0xF000) == 0xD000) {
            int cond = (insn >> 8) & 0xF;
            s32 offset = (s32)(s8)(insn & 0xFF) << 1;
            bool take = false;
            switch (cond) {
            case 0x0: take = CPU_Z; break;
            case 0x1: take = !CPU_Z; break;
            case 0x2: take = CPU_C; break;
            case 0x3: take = !CPU_C; break;
            case 0x4: take = CPU_N; break;
            case 0x5: take = !CPU_N; break;
            case 0x6: take = CPU_V; break;
            case 0x7: take = !CPU_V; break;
            case 0x8: take = CPU_C && !CPU_Z; break;
            case 0x9: take = !CPU_C || CPU_Z; break;
            case 0xA: take = CPU_N == CPU_V; break;
            case 0xB: take = CPU_N != CPU_V; break;
            case 0xC: take = !CPU_Z && (CPU_N == CPU_V); break;
            case 0xD: take = CPU_Z || (CPU_N != CPU_V); break;
            case 0xF: /* SWI */
                gba_swi(insn & 0xFF);
                take = false;
                break;
            }
            if (take) pc = (u32)((s32)pc + offset + 2);
            continue;
        }

        /* Format 17: SWI */
        if ((insn & 0xFF00) == 0xDF00) {
            gba_swi(insn & 0xFF);
            continue;
        }

        /* Format 18: Unconditional branch */
        if ((insn & 0xF800) == 0xE000) {
            s32 offset = (s32)(insn << 21) >> 20;
            pc = (u32)((s32)pc + offset + 2);
            continue;
        }

        /* Format 19: Long branch with link (BL) */
        if ((insn & 0xF800) == 0xF000) {
            u16 insn2 = bus_read16(pc);
            pc += 2;
            if ((insn2 & 0xF800) == 0xF800) {
                s32 off_hi = (s32)((insn & 0x07FF) << 21) >> 9;
                s32 off_lo = (insn2 & 0x07FF) << 1;
                u32 bl_target = (u32)((s32)pc + off_hi + off_lo);
                r[14] = pc | 1;

                /* If BL target is in ROM, dispatch to recompiled code */
                if ((bl_target >> 24) == 0x08) {
                    cpu_bx(bl_target | 1);
                    /* After BX returns, continue interpreting */
                } else {
                    /* BL to another RAM address - just update PC */
                    pc = bl_target;
                }
            }
            continue;
        }

        /* Unhandled instruction */
        fprintf(stderr, "[interp] Unknown Thumb insn 0x%04X at 0x%08X\n", insn, pc - 2);
        break;
    }

done:
    if (steps >= max_steps) {
        fprintf(stderr, "[interp] RAM code at 0x%08X hit step limit (%d steps)\n", target, steps);
    } else if (call_count <= 20) {
        fprintf(stderr, "[interp] RAM code at 0x%08X completed in %d steps\n", target, steps);
    }
}

/* ---- BX Dispatch Tracing ---- */

void cpu_bx_trace(u32 target) {
    static int count = 0;
    static u32 last = 0;
    count++;
    if (target != last && count <= 100) {
        fprintf(stderr, "[bx #%d] -> 0x%08X\n", count, target);
        fflush(stderr);
    }
    last = target;
}

/* cpu_bx() is generated in game_entry.c with a dispatch table */

void cpu_undefined(u32 insn) {
    fprintf(stderr, "[runtime] Undefined instruction 0x%08X at PC=0x%08X\n", insn, r[15]);
}

/* ---- Hardware Interface (called by gba_runtime.h API) ---- */

void ppu_render_scanline(void) { /* PPU is frame-based via display.c */ }
bool ppu_in_vblank(void) { return scanline >= VISIBLE_SCANLINES; }
bool ppu_in_hblank(void) { return scanline_cycles >= HBLANK_START_CYCLE; }
void apu_step(u32 cycles) { (void)cycles; /* Audio stub - future work */ }
void dma_check(void) { /* DMAs are triggered by io_write_hook */ }
void timer_step(u32 cycles) { timer_tick(cycles); }
void irq_check(void) { check_interrupts(); }

void gba_frame(void) {
    /* Advance to next VBlank. Called from VBlankIntrWait and similar. */
    u32 target_cycle = ((cycle_counter / CYCLES_PER_FRAME) + 1) * CYCLES_PER_FRAME
                       + VISIBLE_SCANLINES * CYCLES_PER_SCANLINE;
    while (cycle_counter < target_cycle) {
        advance_cycles(CYCLES_PER_SCANLINE);
    }
}

/* ---- SRAM Persistence ---- */

static void sram_save(void) {
    if (!sram || !sram_path[0]) return;
    FILE* f = fopen(sram_path, "wb");
    if (f) {
        fwrite(sram, 1, 0x10000, f);
        fclose(f);
    }
}

static void sram_load(void) {
    if (!sram || !sram_path[0]) return;
    FILE* f = fopen(sram_path, "rb");
    if (f) {
        fread(sram, 1, 0x10000, f);
        fclose(f);
        fprintf(stderr, "[runtime] Loaded SRAM from %s\n", sram_path);
    }
}

/* ---- Init/Shutdown ---- */

void gba_init(const char* rom_path) {
    /* Allocate memory regions */
    ewram   = calloc(1, 0x40000);
    iwram   = calloc(1, 0x8000);
    io_regs = calloc(1, 0x400);
    palette = calloc(1, 0x400);
    vram    = calloc(1, 0x18000);
    oam     = calloc(1, 0x400);
    sram    = calloc(1, 0x10000);

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

    /* Set up SRAM save path */
    strncpy(sram_path, rom_path, sizeof(sram_path) - 5);
    sram_path[sizeof(sram_path) - 5] = '\0';
    char* dot = strrchr(sram_path, '.');
    if (dot) strcpy(dot, ".sav");
    else strcat(sram_path, ".sav");
    sram_load();

    /* Initialize CPU state (post-BIOS) */
    memset(r, 0, sizeof(r));
    r[13] = 0x03007F00;  /* SP - system/user mode */
    r[15] = 0x08000000;  /* PC */
    cpsr  = 0x0000001F;  /* System mode */

    /* Set up IRQ stack pointer at standard location */
    /* (Games expect r13_irq = 0x03007FA0) */

    /* Initialize KEYINPUT to all released */
    io_regs[0x130] = 0xFF;
    io_regs[0x131] = 0x03;

    /* Initialize SOUNDBIAS to default */
    io_regs[0x088] = 0x00;
    io_regs[0x089] = 0x02; /* 0x0200 = default SOUNDBIAS */

    /* Initialize timer state */
    memset(timers, 0, sizeof(timers));

    fprintf(stderr, "[runtime] GBA static recomp runtime initialized\n");
    fprintf(stderr, "[runtime] ROM: %u bytes (%s)\n", rom_size, rom_path);
    fflush(stderr);

    /* Initialize display */
    if (display_init() != 0) {
        fprintf(stderr, "[runtime] Failed to initialize display\n");
        exit(1);
    }
}

void gba_shutdown(void) {
    sram_save();
    display_shutdown();
    free(ewram);    ewram = NULL;
    free(iwram);    iwram = NULL;
    free(io_regs);  io_regs = NULL;
    free(palette);  palette = NULL;
    free(vram);     vram = NULL;
    free(oam);      oam = NULL;
    free(rom_data); rom_data = NULL;
    free(sram);     sram = NULL;
    fprintf(stderr, "[runtime] Shutdown\n");
}
