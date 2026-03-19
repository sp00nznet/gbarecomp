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
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/video.h>
#include <mgba/internal/arm/arm.h>
#include <mgba-util/vfs.h>

#include <SDL2/SDL.h>
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

/* ---- SDL Display ---- */

static SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;
static SDL_Texture* texture = NULL;
static u16 key_state = 0x03FF; /* All released (active-low) */

/* ---- Forward declarations ---- */
void display_render_frame(void);
int display_poll_events(void);
extern void cpu_bx(u32 target); /* BX dispatch from game_entry.c */

/* ---- Timing ---- */

static u32 frame_count = 0;
static u32 bus_access_count = 0;

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

    /* Check if mGBA rendered a new frame (VBlank started) */
    if (gba->video.frameCounter != last_frame_counter) {
        last_frame_counter = gba->video.frameCounter;
        frame_count++;

        /* Deliver VBlank interrupt to the recompiled game code.
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

u32 bus_read32(u32 addr) {
    bus_access_count++;
    advance_hardware(4);
    return core->busRead32(core, addr);
}

static u32 dispstat_read_count = 0;
u16 bus_read16(u32 addr) {
    bus_access_count++;
    advance_hardware(2);
    u16 val = (u16)core->busRead16(core, addr);
    /* Trace DISPSTAT reads */
    if (addr == 0x04000004 && ++dispstat_read_count <= 10) {
        fprintf(stderr, "[dispstat] read=0x%04X (vcount=%d)\n", val, gba->video.vcount);
        fflush(stderr);
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

void bus_write16(u32 addr, u16 value) {
    bus_access_count++;
    advance_hardware(2);
    core->busWrite16(core, addr, value);
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

int display_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
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

    /* Force alpha to 0xFF on all pixels (mGBA outputs with alpha=0) */
    for (int i = 0; i < GBA_WIDTH * GBA_HEIGHT; i++) {
        videoBuf[i] |= 0xFF000000;
    }

    /* mGBA has rendered into videoBuf - upload to SDL */
    SDL_UpdateTexture(texture, NULL, videoBuf, GBA_WIDTH * sizeof(color_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

int display_poll_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) return 1;
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) return 1;
    }

    /* Read keyboard state for GBA buttons */
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    u16 state = 0;
    if (keys[SDL_SCANCODE_Z])         state |= 0x001; /* A */
    if (keys[SDL_SCANCODE_X])         state |= 0x002; /* B */
    if (keys[SDL_SCANCODE_BACKSPACE]) state |= 0x004; /* Select */
    if (keys[SDL_SCANCODE_RETURN])    state |= 0x008; /* Start */
    if (keys[SDL_SCANCODE_RIGHT])     state |= 0x010; /* Right */
    if (keys[SDL_SCANCODE_LEFT])      state |= 0x020; /* Left */
    if (keys[SDL_SCANCODE_UP])        state |= 0x040; /* Up */
    if (keys[SDL_SCANCODE_DOWN])      state |= 0x080; /* Down */
    if (keys[SDL_SCANCODE_A])         state |= 0x100; /* R */
    if (keys[SDL_SCANCODE_S])         state |= 0x200; /* L */

    key_state = (~state) & 0x03FF; /* Active-low for KEYINPUT */

    /* Feed keys to mGBA (mGBA uses active-high: bit set = pressed) */
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

/* ---- Init / Shutdown ---- */

void gba_init(const char* rom_path) {
    /* Initialize SDL display first so we can show init progress */
    if (display_init() != 0) {
        fprintf(stderr, "[runtime] Display init failed\n");
        exit(1);
    }

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
    core->loadConfig(core, &core->config);
    fprintf(stderr, "[init] Config loaded\n"); fflush(stderr);

    core->reset(core);
    fprintf(stderr, "[init] Core reset complete\n"); fflush(stderr);

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
    printf("[runtime] Running init with mGBA CPU...\n");
    fflush(stdout);
    {
        int init_frames = 0;
        uint16_t prev_dispcnt = 0x0080;
        while (init_frames < 600) {
            core->runFrame(core);
            init_frames++;

            uint16_t dispcnt = gba->memory.io[0];
            uint16_t ie = gba->memory.io[0x100];
            uint16_t ime = gba->memory.io[0x104];

            if (init_frames <= 5 || init_frames % 60 == 0 || dispcnt != prev_dispcnt) {
                fprintf(stderr, "[init frame %d] DISPCNT=0x%04X IE=0x%04X IME=%d PC=0x%08X\n",
                        init_frames, dispcnt, ie, ime, arm_cpu->gprs[15]);
                fflush(stderr);
            }

            /* Stop when forced blank is cleared (game finished init) */
            if (dispcnt != 0x0080 && dispcnt != 0x0000 && init_frames > 2) {
                fprintf(stderr, "[init] Display active at frame %d! DISPCNT=0x%04X\n",
                        init_frames, dispcnt);
                fflush(stderr);
                break;
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

    /* Render the init frame immediately - this shows the title screen! */
    display_render_frame();
    printf("[runtime] Title screen rendered!\n");
    fflush(stdout);
}

void gba_shutdown(void) {
    display_shutdown();
    if (core) {
        core->deinit(core);
        core = NULL;
    }
    gba = NULL;
    arm_cpu = NULL;
    printf("[runtime] Shutdown\n");
}
