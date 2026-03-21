/*
 * GBA Runtime Library - libmgba Backend
 *
 * Replaces the standalone runtime with mGBA for accurate hardware emulation.
 * The recompiled game code calls bus_read/write which delegates to mGBA's
 * memory system. mGBA handles PPU, APU, DMA, timers, and interrupts.
 */

#include "gba/gba_runtime.h"

/* mGBA headers - flags.h must come first for correct struct layout */
#include <mgba/flags.h>
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/video.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#include <mgba/internal/arm/arm.h>
#include <mgba-util/vfs.h>

#include <SDL2/SDL.h>
#include "gba/menu.h"
#include "gba/interception.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- CPU State (owned by recompiled code, NOT mGBA's ARMCore) ---- */

u32  r[16] = {0};
bool CPU_N = false;
bool CPU_Z = false;
bool CPU_C = false;
bool CPU_V = false;

static u32 cpsr_val = 0x0000001F;
static u32 spsr_val = 0;

/* ---- mGBA Logging ---- */
static void _mgba_log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
    (void)logger; (void)category; (void)level;
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
}

/* ---- mGBA State ---- */

static struct mCore* core = NULL;
static struct GBA* gba = NULL;
static struct ARMCore* arm_cpu = NULL;

#define GBA_WIDTH  240
#define GBA_HEIGHT 160
static color_t videoBuf[GBA_WIDTH * GBA_HEIGHT];
static color_t savedFrame[GBA_WIDTH * GBA_HEIGHT]; /* Saved init frame */
static bool has_saved_frame = false;

/* ---- SDL Display ---- */

static SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;
static SDL_Texture* texture = NULL;
static SDL_AudioDeviceID audio_device = 0;
static struct mAudioResampler audio_resampler;
static struct mAudioBuffer audio_output_buf;
static bool audio_resampler_init = false;
static u16 key_state = 0x03FF; /* All released (active-low) */

/* ---- Forward declarations ---- */
void display_render_frame(void);
int display_poll_events(void);
extern void cpu_bx(u32 target); /* BX dispatch from game_entry.c */
static void deliver_interrupts(void);
void run_iwram_function(u32 target);

/* ---- Timing ---- */

static u32 frame_count = 0;
static u32 bus_access_count = 0;
static bool recomp_mode = false; /* true when recompiled code is running */
static u32 dispstat_poll_count = 0; /* consecutive DISPSTAT reads */
bool in_irq = false; /* Guard against re-entrant IRQ (non-static for interception.c) */
static int irq_deliver_count = 0;

/* Advance mGBA hardware by a number of cycles */
static u32 total_cycles = 0;
static u32 last_poll = 0;
static u32 last_frame_counter = 0;

static void advance_hardware(int cycles) {
    arm_cpu->cycles += cycles;
    total_cycles += cycles;

    /* When cycles reach nextEvent, mGBA processes pending events */
    while (arm_cpu->cycles >= arm_cpu->nextEvent) {
        gba->cpu->irqh.processEvents(arm_cpu);
    }

    /* Check for any pending interrupts after timing advance.
     * Read IO registers directly (not via bus) to avoid recursive advance_hardware. */
    if (recomp_mode && !in_irq) {
        u16 ie = gba->memory.io[0x200 >> 1];   /* IE */
        u16 if_val = gba->memory.io[0x202 >> 1]; /* IF */
        u16 ime = gba->memory.io[0x208 >> 1];  /* IME */
        if ((ie & if_val) && ime) {
            deliver_interrupts();
        }
    }

    /* Check if mGBA rendered a new frame (VBlank started) */
    if (gba->video.frameCounter != last_frame_counter) {
        last_frame_counter = gba->video.frameCounter;
        frame_count++;

        /* Deliver VBlank interrupt to recompiled code */
        if (recomp_mode) {
            /* Set IF VBlank bit since mGBA just hit VBlank */
            gba->memory.io[0x202 >> 1] |= 1;
            deliver_interrupts();
        }

        /* Legacy VBlank interrupt delivery (kept for reference)
         * Deliver VBlank interrupt to the recompiled game code.
         * On real GBA: IRQ fires -> BIOS saves regs -> calls [0x03007FFC].
         * We simulate this by directly calling the handler. */
        u16 ie = core->busRead16(core, 0x04000200); /* IE */
        u16 ime = core->busRead16(core, 0x04000208); /* IME */
        u32 handler = core->busRead32(core, 0x03007FFC);
        if (frame_count <= 5) {
            fprintf(stderr, "[irq] IE=0x%04X IME=%d handler=0x%08X\n", ie, ime, handler);
            fflush(stderr);
        }
        if ((ie & 1) && ime) { /* VBlank IRQ enabled + master enable */
            /* Set IF VBlank bit */
            u16 if_val = core->busRead16(core, 0x04000202);
            core->busWrite16(core, 0x04000202, if_val | 1);

            /* Read the game's IRQ handler address */
            u32 handler = core->busRead32(core, 0x03007FFC);
            if (handler != 0 && (handler >> 24) == 0x08) {
                /* Save recompiled CPU state (like BIOS would) */
                u32 saved_r[16];
                bool saved_N = CPU_N, saved_Z = CPU_Z, saved_C = CPU_C, saved_V = CPU_V;
                memcpy(saved_r, r, sizeof(r));

                /* Call the handler via BX dispatch */
                cpu_bx(handler);

                /* Acknowledge VBlank in IF (handler should do this but just in case) */
                u16 if_after = core->busRead16(core, 0x04000202);
                core->busWrite16(core, 0x04000202, if_after & ~1);

                /* Also set BIOS IF flag at 0x03007FF8 (IntrWait checks this) */
                u16 bios_if = core->busRead16(core, 0x03007FF8);
                core->busWrite16(core, 0x03007FF8, bios_if | 1);
            }
        }

        /* Debug: periodic status */
        if (frame_count <= 5 || frame_count % 60 == 0) {
            u16 dispcnt = gba->memory.io[0];
            /* Check if videoBuf has any non-black pixels */
            int nonblack = 0;
            for (int i = 0; i < 240 * 160 && nonblack < 10; i++) {
                if (videoBuf[i] != 0 && videoBuf[i] != 0xFF000000) nonblack++;
            }
            fprintf(stderr, "[frame %u] DISPCNT=0x%04X bus=%u vidpix=%d sample=0x%08X,0x%08X,0x%08X\n",
                    frame_count, dispcnt, bus_access_count, nonblack,
                    videoBuf[0], videoBuf[120*240+120], videoBuf[80*240+120]);
            fflush(stderr);
        }

        display_render_frame();
    }

    /* Poll SDL events and force frame rendering periodically */
    if (total_cycles - last_poll > 50000) {
        last_poll = total_cycles;
        if (display_poll_events()) {
            gba_shutdown();
            exit(0);
        }

        /* Advance mGBA timing for one frame and render */
        {
            /* Tick hardware enough for one frame */
            u32 start_fc = gba->video.frameCounter;
            int ticks = 0;
            while (gba->video.frameCounter == start_fc && ticks < 300000) {
                arm_cpu->cycles += 4;
                while (arm_cpu->cycles >= arm_cpu->nextEvent) {
                    gba->cpu->irqh.processEvents(arm_cpu);
                }
                ticks++;
            }
            if (gba->video.frameCounter != start_fc) {
                last_frame_counter = gba->video.frameCounter;
                frame_count++;
                if (frame_count <= 5 || frame_count % 60 == 0) {
                    u16 dispcnt = gba->memory.io[0];
                    fprintf(stderr, "[frame %u] DISPCNT=0x%04X\n", frame_count, dispcnt);
                    fflush(stderr);
                }
                display_render_frame();
                SDL_Delay(16); /* ~60fps */
            }
        }
    }
}

/* ---- Memory Bus (delegates to mGBA) ---- */

static u32 io_read32_count = 0;
u32 bus_read32(u32 addr) {
    bus_access_count++;
    advance_hardware(4);

    /* Log first reads in recomp mode (excluding IRQ handler) */
    if (recomp_mode && !in_irq && ++io_read32_count <= 20) {
        fprintf(stderr, "[rd32 #%d] addr=0x%08X\n", io_read32_count, addr);
        fflush(stderr);
    }

    return core->busRead32(core, addr);
}

u16 bus_read16(u32 addr) {
    bus_access_count++;
    advance_hardware(2);
    u16 val = (u16)core->busRead16(core, addr);

    /* When recompiled code polls DISPSTAT, advance extra cycles per read
     * to speed things up without skipping scanline rendering. */
    if (recomp_mode && addr == 0x04000004) {
        advance_hardware(16);
        val = (u16)core->busRead16(core, addr); /* Re-read after advance */

        static u16 last_dispstat = 0xFFFF;
        if ((val & 1) != (last_dispstat & 1)) {
            static int ds_log = 0;
            if (++ds_log <= 20) {
                fprintf(stderr, "[dispstat] VBlank %s (vcount=%d)\n",
                        (val & 1) ? "START" : "END", gba->video.vcount);
                fflush(stderr);
            }
        }
        last_dispstat = val;
    }

    return val;
}

u8 bus_read8(u32 addr) {
    bus_access_count++;
    advance_hardware(2);
    return (u8)core->busRead8(core, addr);
}

void bus_write32(u32 addr, u32 value) {
    bus_access_count++;
    advance_hardware(4);
    core->busWrite32(core, addr, value);
}

static int bldy_write_count = 0;
void bus_write16(u32 addr, u16 value) {
    bus_access_count++;
    advance_hardware(2);
    core->busWrite16(core, addr, value);

    /* Track BLDY writes */
    if (recomp_mode && addr == 0x04000054 && ++bldy_write_count <= 10) {
        fprintf(stderr, "[bldy] write 0x%04X (frame %u)\n", value, frame_count);
        fflush(stderr);
    }
}

void bus_write8(u32 addr, u8 value) {
    bus_access_count++;
    advance_hardware(2);
    core->busWrite8(core, addr, value);
}

/* ---- CPSR/SPSR ---- */

u32 cpu_get_cpsr(void) {
    u32 flags = 0;
    if (CPU_N) flags |= (1u << 31);
    if (CPU_Z) flags |= (1u << 30);
    if (CPU_C) flags |= (1u << 29);
    if (CPU_V) flags |= (1u << 28);
    return flags | (cpsr_val & 0x0FFFFFFF);
}

void cpu_set_cpsr(u32 value, u32 mask) {
    if (mask & 8) {
        CPU_N = (value >> 31) & 1;
        CPU_Z = (value >> 30) & 1;
        CPU_C = (value >> 29) & 1;
        CPU_V = (value >> 28) & 1;
    }
    if (mask & 1) {
        cpsr_val = (cpsr_val & ~0xFF) | (value & 0xFF);
    }
}

u32 cpu_get_spsr(void) { return spsr_val; }

void cpu_set_spsr(u32 value, u32 mask) {
    if (mask & 8) spsr_val = (spsr_val & 0x0FFFFFFF) | (value & 0xF0000000);
    if (mask & 1) spsr_val = (spsr_val & ~0xFF) | (value & 0xFF);
}

/* ---- Software Interrupts ---- */

void gba_swi(u32 number) {
    switch (number) {
    case 0x02: /* Halt */
        /* Advance to next interrupt */
        arm_cpu->halted = 1;
        while (arm_cpu->halted) {
            advance_hardware(16);
        }
        break;

    case 0x04: /* IntrWait */
    case 0x05: /* VBlankIntrWait */
        gba_frame();
        break;

    case 0x06: { /* Div */
        s32 num = (s32)r[0];
        s32 den = (s32)r[1];
        if (den != 0) {
            r[0] = (u32)(num / den);
            r[1] = (u32)(num % den);
            s32 abs_result = num / den;
            r[3] = (u32)(abs_result < 0 ? -abs_result : abs_result);
        }
        break;
    }

    case 0x07: { /* DivArm */
        s32 den = (s32)r[0];
        s32 num = (s32)r[1];
        if (den != 0) {
            r[0] = (u32)(num / den);
            r[1] = (u32)(num % den);
            s32 abs_result = num / den;
            r[3] = (u32)(abs_result < 0 ? -abs_result : abs_result);
        }
        break;
    }

    case 0x08: { /* Sqrt */
        u32 val = r[0];
        u32 result = 0, bit = 1u << 30;
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
        u32 src = r[0], dst = r[1], cnt = r[2];
        u32 count = cnt & 0x1FFFFF;
        bool fill = (cnt >> 24) & 1;
        bool word = (number == 0x0C) || ((cnt >> 26) & 1);
        u32 size = word ? 4 : 2;

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
        (void)size;
        break;
    }

    default:
        fprintf(stderr, "[swi] Unhandled SWI 0x%02X\n", number);
        break;
    }
}

/* ---- Display ---- */

/* ---- Audio Callback ---- */

static int audio_cb_count = 0;
static void audio_callback(void* userdata, Uint8* stream, int len) {
    (void)userdata;
    if (!audio_resampler_init || !core) { memset(stream, 0, len); return; }

    /* Read directly from mGBA's audio buffer (skip resampler for now) */
    struct mAudioBuffer* src = core->getAudioBuffer(core);

    int samples_requested = len / (2 * sizeof(int16_t));
    size_t available = mAudioBufferAvailable(src);
    int to_read = samples_requested < (int)available ? samples_requested : (int)available;

    audio_cb_count++;

    if (to_read > 0) {
        mAudioBufferRead(src, (int16_t*)stream, to_read);

        if (audio_cb_count <= 5 || audio_cb_count % 500 == 0) {
            int16_t* s = (int16_t*)stream;
            int16_t maxval = 0;
            for (int j = 0; j < to_read * 2; j++) {
                int16_t v = s[j] < 0 ? -s[j] : s[j];
                if (v > maxval) maxval = v;
            }
            /* Also check raw buffer bytes for any non-zero */
            int rawnonzero = 0;
            u8* rawdata = (u8*)src->data.data;
            if (rawdata) {
                for (int j = 0; j < 256; j++) {
                    if (rawdata[j]) rawnonzero++;
                }
            }
            fprintf(stderr, "[audio #%d] read=%d peak=%d rawNZ=%d\n",
                    audio_cb_count, to_read, maxval, rawnonzero);
            fflush(stderr);
        }
    }
    int filled = to_read * 2 * sizeof(int16_t);
    if (filled < len) memset(stream + filled, 0, len - filled);
}

int display_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "[display] SDL init failed: %s\n", SDL_GetError());
        return -1;
    }

    window = SDL_CreateWindow("Advance Wars Recompiled",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        GBA_WIDTH * 3, GBA_HEIGHT * 3, SDL_WINDOW_SHOWN);
    if (!window) return -1;

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) return -1;

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, GBA_WIDTH, GBA_HEIGHT);
    if (!texture) return -1;

    return 0;
}

void display_render_frame(void) {
    if (!texture) return;

    /* Fix color channels: mGBA outputs 0xXXBBGGRR, SDL wants 0xFFRRGGBB */
    static color_t renderBuf[GBA_WIDTH * GBA_HEIGHT];
    for (int i = 0; i < GBA_WIDTH * GBA_HEIGHT; i++) {
        u32 p = videoBuf[i];
        u32 r = (p >> 0) & 0xFF;
        u32 g = (p >> 8) & 0xFF;
        u32 b = (p >> 16) & 0xFF;
        renderBuf[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    /* Upload to SDL */
    SDL_UpdateTexture(texture, NULL, renderBuf, GBA_WIDTH * sizeof(color_t));
    SDL_RenderClear(renderer);

    /* Offset game below menu bar - tight, no gap */
    int bar_h = menu_get_bar_height() - 2; /* Remove padding gap */
    if (bar_h < 0) bar_h = 0;
    int win_w, win_h;
    SDL_GetWindowSize(window, &win_w, &win_h);
    SDL_Rect dst = { 0, bar_h, win_w, win_h - bar_h };
    SDL_RenderCopy(renderer, texture, NULL, &dst);

    menu_render();
    SDL_RenderPresent(renderer);
}

int display_poll_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        /* Let ImGui process first */
        menu_process_event(&event);

        if (event.type == SDL_QUIT) return 1;
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE &&
            !menu_wants_input()) return 1;

        /* Toggle debug console with F12 */
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F12) {
            extern bool show_debug_console;
            /* Can't access C++ static from C - use menu API instead */
        }
    }

    /* Use configurable key bindings from menu */
    u16 state = menu_get_keys();

    /* Don't send input to game if ImGui wants it */
    if (menu_wants_input()) {
        state = 0;
    }

    key_state = (~state) & 0x03FF;

    if (core) {
        core->setKeys(core, state);
    }

    return 0;
}

u16 display_get_keys(void) {
    return key_state;
}

void display_shutdown(void) {
    if (texture)  { SDL_DestroyTexture(texture);   texture = NULL; }
    if (renderer) { SDL_DestroyRenderer(renderer); renderer = NULL; }
    if (window)   { SDL_DestroyWindow(window);     window = NULL; }
    SDL_Quit();
}

/* ---- Hardware Stubs (now handled by mGBA) ---- */

void ppu_render_scanline(void) { /* mGBA handles this */ }
bool ppu_in_vblank(void) { return gba ? gba->video.vcount >= 160 : false; }
bool ppu_in_hblank(void) { return false; }
void apu_step(u32 cycles) { (void)cycles; }
void dma_check(void) { /* mGBA handles this */ }
void timer_step(u32 cycles) { (void)cycles; }
void irq_check(void) { /* mGBA handles this */ }

/* ---- Frame Advancement ---- */

void gba_frame(void) {
    if (!gba) return;

    u32 start_frame = gba->video.frameCounter;

    /* Advance mGBA hardware until the next frame is rendered */
    int safety = 0;
    while (gba->video.frameCounter == start_frame && safety < 500000) {
        advance_hardware(4);
        safety++;
    }

    frame_count++;

    /* Debug: periodic status */
    if (frame_count <= 5 || frame_count % 60 == 0) {
        u16 dispcnt = core->busRead16(core, 0x04000000);
        fprintf(stderr, "[frame %u] DISPCNT=0x%04X mode=%d BG=%d%d%d%d OBJ=%d\n",
                frame_count, dispcnt, dispcnt & 7,
                (dispcnt >> 8) & 1, (dispcnt >> 9) & 1,
                (dispcnt >> 10) & 1, (dispcnt >> 11) & 1,
                (dispcnt >> 12) & 1);
        fflush(stderr);
    }

    /* Render the frame from mGBA's video buffer */
    display_render_frame();

    /* Poll input */
    if (display_poll_events()) {
        gba_shutdown();
        exit(0);
    }
}

/* ---- Undefined Instruction ---- */

void cpu_undefined(u32 insn) {
    fprintf(stderr, "[runtime] Undefined: 0x%08X\n", insn);
}

/* ---- IWRAM Interpreter Fallback ---- */

/* Run an IWRAM/EWRAM function via mGBA's CPU interpreter.
 * Syncs our register file to mGBA, steps until PC returns to ROM, syncs back. */
static int iwram_call_count = 0;
void run_iwram_function(u32 target) {
    if (!core || !arm_cpu) return;

    iwram_call_count++;
    if (iwram_call_count <= 20) {
        fprintf(stderr, "[iwram #%d] call 0x%08X\n", iwram_call_count, target);
        fflush(stderr);
    }

    /* Sync recompiled state -> mGBA's ARMCore */
    for (int i = 0; i < 16; i++) arm_cpu->gprs[i] = r[i];
    arm_cpu->cpsr.packed = cpu_get_cpsr();

    /* Set PC to target (clear Thumb bit for BX) */
    bool thumb = (target & 1) != 0;
    arm_cpu->gprs[15] = target & ~1u;
    arm_cpu->cpsr.t = thumb ? 1 : 0;

    /* Step mGBA's interpreter until PC returns to ROM space */
    int steps = 0;
    while (steps < 500000) {
        core->step(core);
        steps++;

        u32 pc = arm_cpu->gprs[15];
        /* Check if we returned to ROM */
        if ((pc >> 24) == 0x08) break;
        /* Check if we returned to address 0 (BIOS return) */
        if (pc < 0x100) break;
    }

    /* Sync mGBA state -> recompiled */
    for (int i = 0; i < 16; i++) r[i] = arm_cpu->gprs[i];
    CPU_N = arm_cpu->cpsr.n;
    CPU_Z = arm_cpu->cpsr.z;
    CPU_C = arm_cpu->cpsr.c;
    CPU_V = arm_cpu->cpsr.v;
}

/* ---- VBlank Interrupt Delivery ---- */

/* Deliver any pending interrupts to the recompiled game code.
 * Checks IE & IF for ANY matching bits, not just VBlank. */
static void deliver_interrupts(void) {
    if (in_irq) return;

    /* Read IO directly to avoid recursive bus access */
    u16 ie = gba->memory.io[0x200 >> 1];
    u16 ime = gba->memory.io[0x208 >> 1];
    u16 if_val = gba->memory.io[0x202 >> 1];

    /* Check if any enabled interrupt is pending */
    u16 pending = ie & if_val;
    if (!pending || !ime) return;

    u32 handler = core->busRead32(core, 0x03007FFC);
    if (handler == 0) return;

    /* Set BIOS IF flags for IntrWait */
    u16 bios_if = core->busRead16(core, 0x03007FF8);
    core->busWrite16(core, 0x03007FF8, bios_if | pending);

    in_irq = true;
    irq_deliver_count++;

    /* Capture IO state before handler */
    u16 bldy_before = gba->memory.io[0x054 >> 1]; /* BLDY */
    u16 bldcnt_before = gba->memory.io[0x050 >> 1]; /* BLDCNT */

    /* Save recompiled state */
    u32 saved_r[16];
    bool saved_N = CPU_N, saved_Z = CPU_Z, saved_C = CPU_C, saved_V = CPU_V;
    memcpy(saved_r, r, sizeof(r));

    /* Run interrupt handler */
    if ((handler >> 24) == 0x03 || (handler >> 24) == 0x02) {
        run_iwram_function(handler);
    } else if ((handler >> 24) == 0x08) {
        extern void cpu_bx(u32 target);
        cpu_bx(handler);
    }

    /* Restore game state (IRQ handler state is discarded) */
    memcpy(r, saved_r, sizeof(r));
    CPU_N = saved_N; CPU_Z = saved_Z; CPU_C = saved_C; CPU_V = saved_V;

    /* Check what the handler changed */
    u16 bldy_after = gba->memory.io[0x054 >> 1];
    u16 bldcnt_after = gba->memory.io[0x050 >> 1];
    if (irq_deliver_count <= 10) {
        fprintf(stderr, "[irq #%d] BLDCNT: 0x%04X->0x%04X  BLDY: 0x%04X->0x%04X  IE now=0x%04X\n",
                irq_deliver_count, bldcnt_before, bldcnt_after, bldy_before, bldy_after,
                gba->memory.io[0x200 >> 1]);
        fflush(stderr);
    }

    /* Re-enable IME (BIOS restores this on IRQ return) */
    gba->memory.io[0x208 >> 1] = 1;

    in_irq = false;

    /* Immediately check for more pending interrupts.
     * The handler may have changed IE (e.g. VBlank -> Timer),
     * and mGBA may have already set the new IF bits. */
    {
        u16 ie2 = gba->memory.io[0x200 >> 1];
        u16 if2 = gba->memory.io[0x202 >> 1];
        if (ie2 & if2) {
            deliver_interrupts(); /* Recursive - safe due to in_irq guard */
        }
    }
}

/* ---- Menu Callbacks ---- */

static char rom_base_path[512] = {0};

static void cb_save_state(int slot) {
    if (!core) return;
    char path[512];
    snprintf(path, sizeof(path), "%s.ss%d", rom_base_path, slot);
    fprintf(stderr, "[save] Saving to: %s\n", path); fflush(stderr);
    struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_RDWR);
    if (!vf) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Failed to open save file: %s", path);
        menu_add_debug_log(msg);
        return;
    }
    if (mCoreSaveStateNamed(core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "State saved to slot %d", slot);
        menu_add_debug_log(msg);
    } else {
        menu_add_debug_log("mCoreSaveStateNamed failed!");
    }
    vf->close(vf);
}

static void cb_load_state(int slot) {
    if (!core) return;
    char path[512];
    snprintf(path, sizeof(path), "%s.ss%d", rom_base_path, slot);
    fprintf(stderr, "[load] Loading from: %s\n", path); fflush(stderr);
    struct VFile* vf = VFileOpen(path, O_RDONLY);
    if (!vf) {
        menu_add_debug_log("No save state in this slot");
        return;
    }
    if (mCoreLoadStateNamed(core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "State loaded from slot %d", slot);
        menu_add_debug_log(msg);
    } else {
        menu_add_debug_log("Load state failed!");
    }
    vf->close(vf);
}

static void cb_set_scale(int scale) {
    if (!window) return;
    SDL_SetWindowSize(window, GBA_WIDTH * scale, GBA_HEIGHT * scale + menu_get_bar_height());
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    fprintf(stderr, "[gfx] Scale set to %dx\n", scale);
}

static void cb_set_filter(int filter) {
    if (!renderer) return;
    /* Recreate texture with new filter mode */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, filter ? "1" : "0");
    if (texture) SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, GBA_WIDTH, GBA_HEIGHT);
    fprintf(stderr, "[gfx] Filter set to %s\n", filter ? "linear" : "nearest");
}

/* ---- mGBA Accessors (for interception.c) ---- */

struct mCore* get_mgba_core(void) { return core; }
struct GBA* get_mgba_gba(void) { return gba; }
struct ARMCore* get_mgba_arm(void) { return arm_cpu; }

/* ---- Init / Shutdown ---- */

void gba_init(const char* rom_path) {
    /* Initialize SDL display first so we can show init progress */
    if (display_init() != 0) {
        fprintf(stderr, "[runtime] Display init failed\n");
        exit(1);
    }
    menu_init(window, renderer);

    /* Set up menu callbacks */
    menu_set_callbacks(cb_save_state, cb_load_state, cb_set_scale, cb_set_filter, NULL);

    /* Store base path for save states */
    strncpy(rom_base_path, rom_path, sizeof(rom_base_path) - 1);
    char* ext = strrchr(rom_base_path, '.');
    if (ext) *ext = '\0'; /* Strip extension */

    fprintf(stderr, "[init] Starting mGBA init...\n"); fflush(stderr);

    /* Initialize mGBA logging with a real callback */
    static struct mLogger myLogger;
    memset(&myLogger, 0, sizeof(myLogger));
    myLogger.log = _mgba_log;
    mLogSetDefaultLogger(&myLogger);

    /* Create mGBA core */
    core = GBACoreCreate();
    if (!core) {
        fprintf(stderr, "[runtime] Failed to create mGBA core\n");
        exit(1);
    }
    fprintf(stderr, "[init] Core created\n"); fflush(stderr);

    fprintf(stderr, "[init] core=%p, init=%p\n", (void*)core, (void*)core->init); fflush(stderr);
    bool ok = core->init(core);
    fprintf(stderr, "[init] Core initialized: %d\n", ok); fflush(stderr);

    /* Set video buffer */
    unsigned stride = GBA_WIDTH;
    core->setVideoBuffer(core, videoBuf, stride);
    fprintf(stderr, "[init] Video buffer set\n"); fflush(stderr);

    /* Load ROM */
    struct VFile* rom_vf = VFileOpen(rom_path, O_RDONLY);
    if (!rom_vf) {
        fprintf(stderr, "[runtime] Cannot open ROM: %s\n", rom_path);
        exit(1);
    }
    fprintf(stderr, "[init] ROM file opened\n"); fflush(stderr);

    if (!core->loadROM(core, rom_vf)) {
        fprintf(stderr, "[runtime] Failed to load ROM\n");
        exit(1);
    }
    fprintf(stderr, "[init] ROM loaded\n"); fflush(stderr);

    /* Reset - initializes all hardware (uses HLE BIOS if no BIOS loaded) */
    fprintf(stderr, "[init] Calling reset...\n"); fflush(stderr);

    /* Set skip BIOS option before reset */
    mCoreConfigInit(&core->config, "gbarecomp");
    mCoreConfigSetDefaultIntValue(&core->config, "skipBios", 1);
    mCoreConfigSetDefaultIntValue(&core->config, "useBios", 0);
    mCoreConfigSetDefaultIntValue(&core->config, "sampleRate", 48000);
    core->loadConfig(core, &core->config);
    fprintf(stderr, "[init] Config loaded\n"); fflush(stderr);

    /* Set up save file (.sav next to ROM) */
    {
        char save_path[512];
        strncpy(save_path, rom_path, sizeof(save_path) - 5);
        save_path[sizeof(save_path) - 5] = '\0';
        /* Replace .gba extension with .sav */
        char* dot = strrchr(save_path, '.');
        if (dot) strcpy(dot, ".sav");
        else strcat(save_path, ".sav");

        struct VFile* save_vf = VFileOpen(save_path, O_CREAT | O_RDWR);
        if (save_vf) {
            core->loadSave(core, save_vf);
            fprintf(stderr, "[init] Save file: %s\n", save_path);
        } else {
            fprintf(stderr, "[init] Warning: could not open save file\n");
        }
        fflush(stderr);
    }

    core->reset(core);
    fprintf(stderr, "[init] Core reset complete\n"); fflush(stderr);

    /* Set up audio */
    {
        unsigned mgba_rate = core->audioSampleRate(core);
        fprintf(stderr, "[init] mGBA native sample rate: %u\n", mgba_rate);

        core->setAudioBufferSize(core, 2048);

        /* Set up audio resampler: mGBA internal buffer -> our output buffer */
        struct mAudioBuffer* src_buf = core->getAudioBuffer(core);
        mAudioBufferInit(&audio_output_buf, 4096, 2);
        mAudioResamplerInit(&audio_resampler, mINTERPOLATOR_SINC);
        mAudioResamplerSetSource(&audio_resampler, src_buf, mgba_rate, true);
        mAudioResamplerSetDestination(&audio_resampler, &audio_output_buf, 48000);
        audio_resampler_init = true;
        fprintf(stderr, "[init] Audio resampler: src=%p -> dst, %u -> 32768Hz\n",
                (void*)src_buf, mgba_rate);

        SDL_AudioSpec want, have;
        memset(&want, 0, sizeof(want));
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = audio_callback;
        want.userdata = core;

        audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (audio_device > 0) {
            SDL_PauseAudioDevice(audio_device, 0);
            fprintf(stderr, "[init] Audio: %dHz %dch %d samples (device %d)\n",
                    have.freq, have.channels, have.samples, audio_device);
        } else {
            fprintf(stderr, "[init] Audio failed: %s\n", SDL_GetError());
        }
        fflush(stderr);
    }

    /* Cache internal pointers */
    gba = core->board;
    arm_cpu = core->cpu;

    fprintf(stderr, "[init] gba=%p, cpu=%p, video.renderer=%p\n",
            (void*)gba, (void*)arm_cpu, (void*)gba->video.renderer);
    fprintf(stderr, "[init] cpu->cycles=%d, nextEvent=%d\n",
            arm_cpu->cycles, arm_cpu->nextEvent);
    fprintf(stderr, "[init] video.frameCounter=%u, vcount=%d\n",
            gba->video.frameCounter, gba->video.vcount);
    fflush(stderr);

    /* Let mGBA's real CPU run the game's initialization.
     * The game copies init code to IWRAM and executes it there,
     * using BIOS decompression routines (LZ77, RLE) to unpack
     * graphics into VRAM. This can't be done by recompiled code. */
    printf("[runtime] Running game...\n");
    fflush(stdout);
    {
        int init_frames = 0;
        uint16_t prev_dispcnt = 0x0080;
        bool interception_active = false;

        while (1) {
            /* Use normal runFrame during init, interception after */
            if (interception_active) {
                interception_run_frame(core);
            } else {
                core->runFrame(core);
            }

            init_frames++;

            uint16_t dispcnt = gba->memory.io[0];
            uint16_t ie = gba->memory.io[0x100];
            uint16_t ime = gba->memory.io[0x104];

            if (init_frames <= 5 || init_frames % 60 == 0 || dispcnt != prev_dispcnt) {
                fprintf(stderr, "[init frame %d] DISPCNT=0x%04X IE=0x%04X IME=%d PC=0x%08X\n",
                        init_frames, dispcnt, ie, ime, arm_cpu->gprs[15]);
                fflush(stderr);
            }

            /* Check if video buffer has real graphics (multiple colors) */
            int unique = 0;
            color_t seen[8] = {0};
            for (int i = 0; i < 240*160 && unique < 8; i++) {
                color_t p = videoBuf[i];
                bool found2 = false;
                for (int j = 0; j < unique; j++) if (seen[j] == p) { found2 = true; break; }
                if (!found2) seen[unique++] = p;
            }

            /* Render each frame to SDL during init so we can see the intro */
            if (unique >= 2 && init_frames > 10) {
                /* Save frame and render */
                memcpy(savedFrame, videoBuf, sizeof(savedFrame));
                has_saved_frame = true;
                display_render_frame();
                if (display_poll_events()) { gba_shutdown(); exit(0); }

                /* Feed keyboard input to mGBA using configurable bindings */
                u16 state = menu_get_keys();
                if (!menu_wants_input()) {
                    core->setKeys(core, state);
                } else {
                    core->setKeys(core, 0);
                }
            }

            /* Activate interception after init completes */
            /* Enable interception via mGBA run loop hook (no ROM patching!) */
            if (!interception_active && unique >= 6 && ie != 0 && ime != 0 && init_frames > 50) {
                extern void interception_setup_from_bx_table(void);
                interception_setup_from_bx_table();
                interception_active = true;
                recomp_mode = true;
                fprintf(stderr, "[runtime] Recomp hook activated at frame %d\n", init_frames);
                fflush(stderr);
            }
            prev_dispcnt = dispcnt;
        }
        fprintf(stderr, "[init] mGBA CPU ran %d frames\n", init_frames);
        fflush(stderr);
    }

    /* Copy mGBA's CPU state to our recompiled register file */
    for (int i = 0; i < 16; i++) {
        r[i] = arm_cpu->gprs[i];
    }
    cpsr_val = arm_cpu->cpsr.packed;
    CPU_N = arm_cpu->cpsr.n;
    CPU_Z = arm_cpu->cpsr.z;
    CPU_C = arm_cpu->cpsr.c;
    CPU_V = arm_cpu->cpsr.v;

    /* Render the init frame immediately */
    {
        int nonblack = 0;
        for (int i = 0; i < 240*160; i++) {
            if (videoBuf[i] != 0) nonblack++;
        }
        fprintf(stderr, "[init] videoBuf has %d non-zero pixels after init\n", nonblack);
        fflush(stderr);
    }

    printf("[runtime] mGBA init complete, handing off to recompiled code\n");
    printf("[runtime] SP=0x%08X PC=0x%08X DISPCNT=0x%04X\n",
           r[13], r[15], gba->memory.io[0]);
    fflush(stdout);

    /* Render the last init frame */
    display_render_frame();

    /* Interception is set up inside the init loop (after frame 51) */

    printf("[runtime] Init complete\n");
    printf("[runtime] SP=0x%08X PC=0x%08X DISPCNT=0x%04X\n",
           r[13], r[15], gba->memory.io[0]);
    fflush(stdout);

    /* gba_init returns, then main() calls func_080386E4() (the main game function) */
}

void gba_shutdown(void) {
    if (audio_device > 0) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
    menu_shutdown();
    display_shutdown();
    if (core) {
        core->deinit(core);
        core = NULL;
    }
    gba = NULL;
    arm_cpu = NULL;
    printf("[runtime] Shutdown\n");
}
