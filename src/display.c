/*
 * GBA Display - SDL2 rendering backend for gbarecomp
 *
 * Implements Mode 0 (tiled), Mode 3 (bitmap 16bpp), Mode 4 (bitmap 8bpp),
 * plus basic OBJ (sprite) rendering.
 */

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gba/display.h"
#include "gba/types.h"

/* Direct memory access for rendering (defined in runtime.c) */
extern u8* io_regs;
extern u8* palette;
extern u8* vram;
extern u8* oam;

/* ---------- Constants ---------- */

#define GBA_WIDTH   240
#define GBA_HEIGHT  160
#define SCALE       3
#define WIN_WIDTH   (GBA_WIDTH * SCALE)
#define WIN_HEIGHT  (GBA_HEIGHT * SCALE)

/* I/O register offsets (relative to 0x04000000) */
#define REG_DISPCNT   0x000
#define REG_DISPSTAT  0x004
#define REG_VCOUNT    0x006
#define REG_BG0CNT    0x008
#define REG_BG1CNT    0x00A
#define REG_BG2CNT    0x00C
#define REG_BG3CNT    0x00E
#define REG_BG0HOFS   0x010
#define REG_BG0VOFS   0x012
#define REG_BG1HOFS   0x014
#define REG_BG1VOFS   0x016
#define REG_BG2HOFS   0x018
#define REG_BG2VOFS   0x01A
#define REG_BG3HOFS   0x01C
#define REG_BG3VOFS   0x01E

/* OAM attribute bits */
#define OAM_ATTR0_Y_MASK       0x00FF
#define OAM_ATTR0_MODE_MASK    0x0300
#define OAM_ATTR0_MODE_SHIFT   8
#define OAM_ATTR0_GFX_MASK     0x0C00
#define OAM_ATTR0_GFX_SHIFT    10
#define OAM_ATTR0_MOSAIC       0x1000
#define OAM_ATTR0_BPP          0x2000  /* 0=4bpp, 1=8bpp */
#define OAM_ATTR0_SHAPE_MASK   0xC000
#define OAM_ATTR0_SHAPE_SHIFT  14

#define OAM_ATTR1_X_MASK       0x01FF
#define OAM_ATTR1_HFLIP        0x1000
#define OAM_ATTR1_VFLIP        0x2000
#define OAM_ATTR1_SIZE_MASK    0xC000
#define OAM_ATTR1_SIZE_SHIFT   14

#define OAM_ATTR2_TILE_MASK    0x03FF
#define OAM_ATTR2_PRIO_MASK    0x0C00
#define OAM_ATTR2_PRIO_SHIFT   10
#define OAM_ATTR2_PAL_MASK     0xF000
#define OAM_ATTR2_PAL_SHIFT    12

/* ---------- SDL state ---------- */

static SDL_Window*   s_window   = NULL;
static SDL_Renderer* s_renderer = NULL;
static SDL_Texture*  s_texture  = NULL;

/* 240x160 ARGB8888 framebuffer */
static u32 s_framebuf[GBA_WIDTH * GBA_HEIGHT];

/* Key state: bit set = pressed. Mapped to GBA button bits. */
static u16 s_keys_pressed = 0;

/* Headless mode (house style, see docs/headless.md): no window, frames go to
 * ffmpeg over a pipe so runs work over RDP and never take a screen. */
static int   s_headless = 0;
static FILE* s_record = NULL;
static const char* s_record_path = NULL;
static const char* s_shot_path = NULL;
static long  s_max_frames = 0;
static long  s_frame_no = 0;
static FILE* s_input = NULL;     /* --input script: "<frame> <keymask hex>" lines */
static long  s_next_input_frame = -1;
static u16   s_next_input_keys = 0;

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

/* ---------- Helper: read 16-bit from memory arrays ---------- */

static inline u16 io_read16(u32 offset) {
    return (u16)io_regs[offset] | ((u16)io_regs[offset + 1] << 8);
}

static inline u16 pal_read16(u32 offset) {
    return (u16)palette[offset] | ((u16)palette[offset + 1] << 8);
}

static inline u16 vram_read16(u32 offset) {
    return (u16)vram[offset] | ((u16)vram[offset + 1] << 8);
}

static inline u8 vram_read8(u32 offset) {
    return vram[offset];
}

/* ---------- Color conversion ---------- */

/* GBA 15-bit color (xBBBBBGGGGGRRRRR) -> 32-bit ARGB8888 */
static inline u32 gba_to_argb(u16 color) {
    u32 r = (color & 0x001F);
    u32 g = (color & 0x03E0) >> 5;
    u32 b = (color & 0x7C00) >> 10;
    /* Expand 5-bit to 8-bit: (x << 3) | (x >> 2) */
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* ---------- OBJ size lookup ---------- */

/* shape (0-2) x size (0-3) -> width, height */
static const u8 obj_width[3][4] = {
    {  8, 16, 32, 64 },  /* Square */
    { 16, 32, 32, 64 },  /* Horizontal */
    {  8,  8, 16, 32 },  /* Vertical */
};
static const u8 obj_height[3][4] = {
    {  8, 16, 32, 64 },  /* Square */
    {  8,  8, 16, 32 },  /* Horizontal */
    { 16, 32, 32, 64 },  /* Vertical */
};

/* ---------- Mode 3: 240x160 direct-color bitmap ---------- */

static void render_mode3(void) {
    for (int y = 0; y < GBA_HEIGHT; y++) {
        for (int x = 0; x < GBA_WIDTH; x++) {
            u32 offset = (u32)(y * GBA_WIDTH + x) * 2;
            u16 color = vram_read16(offset);
            s_framebuf[y * GBA_WIDTH + x] = gba_to_argb(color);
        }
    }
}

/* ---------- Mode 4: 240x160 8-bit indexed bitmap ---------- */

static void render_mode4(u16 dispcnt) {
    /* Bit 4 of DISPCNT selects page: 0 = 0x06000000, 1 = 0x0600A000 */
    u32 page_offset = (dispcnt & 0x0010) ? 0xA000 : 0x0000;

    for (int y = 0; y < GBA_HEIGHT; y++) {
        for (int x = 0; x < GBA_WIDTH; x++) {
            u8 idx = vram_read8(page_offset + (u32)(y * GBA_WIDTH + x));
            u16 color = pal_read16((u32)idx * 2);
            s_framebuf[y * GBA_WIDTH + x] = gba_to_argb(color);
        }
    }
}

/* ---------- Mode 0: tiled backgrounds ---------- */

static void render_bg_mode0(int bg, u16 dispcnt) {
    /* Check if this BG is enabled */
    if (!(dispcnt & (0x0100 << bg)))
        return;

    u16 bgcnt = io_read16((u16)(REG_BG0CNT + bg * 2));
    u16 hofs  = io_read16((u16)(REG_BG0HOFS + bg * 4)) & 0x1FF;
    u16 vofs  = io_read16((u16)(REG_BG0VOFS + bg * 4)) & 0x1FF;

    /* BGCNT fields */
    u32 char_base   = (u32)((bgcnt >> 2) & 0x3) * 0x4000;   /* tile data */
    u32 screen_base = (u32)((bgcnt >> 8) & 0x1F) * 0x800;   /* map data */
    int bpp8        = (bgcnt >> 7) & 1;                       /* 0=4bpp, 1=8bpp */
    int screen_size = (bgcnt >> 14) & 0x3;

    /*
     * Screen sizes for regular BG:
     *   0 = 256x256 (32x32 tiles, 1 screen)
     *   1 = 512x256 (64x32 tiles, 2 screens horizontal)
     *   2 = 256x512 (32x64 tiles, 2 screens vertical)
     *   3 = 512x512 (64x64 tiles, 4 screens)
     */
    int map_w_tiles = (screen_size & 1) ? 64 : 32;
    int map_h_tiles = (screen_size & 2) ? 64 : 32;
    int map_w_px = map_w_tiles * 8;
    int map_h_px = map_h_tiles * 8;

    for (int sy = 0; sy < GBA_HEIGHT; sy++) {
        for (int sx = 0; sx < GBA_WIDTH; sx++) {
            /* Apply scroll and wrap */
            int bx = ((int)hofs + sx) % map_w_px;
            int by = ((int)vofs + sy) % map_h_px;
            if (bx < 0) bx += map_w_px;
            if (by < 0) by += map_h_px;

            /* Determine which tile in the map */
            int tile_x = bx / 8;
            int tile_y = by / 8;

            /* Pixel within the tile */
            int px = bx & 7;
            int py = by & 7;

            /*
             * Screen block addressing for >32x32 maps:
             * The 32x32 screens are laid out in memory as:
             *   screen_size 0: [SC0]
             *   screen_size 1: [SC0][SC1]          (side by side)
             *   screen_size 2: [SC0]
             *                  [SC1]               (stacked)
             *   screen_size 3: [SC0][SC1]
             *                  [SC2][SC3]
             */
            u32 map_addr = screen_base;
            int local_tx = tile_x;
            int local_ty = tile_y;

            if (tile_x >= 32) {
                map_addr += 0x800; /* next screen block horizontally */
                local_tx -= 32;
            }
            if (tile_y >= 32) {
                /* If width is 64 tiles, vertical offset skips 2 screen blocks */
                map_addr += (screen_size & 1) ? 0x1000 : 0x800;
                local_ty -= 32;
            }

            u32 entry_addr = map_addr + (u32)(local_ty * 32 + local_tx) * 2;
            u16 entry = vram_read16(entry_addr);

            u16 tile_idx = entry & 0x03FF;
            int hflip    = (entry >> 10) & 1;
            int vflip    = (entry >> 11) & 1;
            int pal_num  = (entry >> 12) & 0xF;

            /* Apply flip */
            int fx = hflip ? (7 - px) : px;
            int fy = vflip ? (7 - py) : py;

            u8 color_idx;
            if (bpp8) {
                /* 8bpp: each tile is 64 bytes */
                u32 tile_addr = char_base + (u32)tile_idx * 64;
                color_idx = vram_read8(tile_addr + (u32)(fy * 8 + fx));
            } else {
                /* 4bpp: each tile is 32 bytes, 2 pixels per byte */
                u32 tile_addr = char_base + (u32)tile_idx * 32;
                u8 byte = vram_read8(tile_addr + (u32)(fy * 4 + fx / 2));
                color_idx = (fx & 1) ? (byte >> 4) : (byte & 0x0F);
            }

            /* Color index 0 is transparent for BG layers > 0 or OBJ overlay */
            if (color_idx == 0)
                continue;

            u16 pal_color;
            if (bpp8) {
                pal_color = pal_read16((u32)color_idx * 2);
            } else {
                pal_color = pal_read16((u32)(pal_num * 16 + color_idx) * 2);
            }

            s_framebuf[sy * GBA_WIDTH + sx] = gba_to_argb(pal_color);
        }
    }
}

void display_render_line(int y) {
    if (!io_regs || !vram || !palette || !oam || y < 0 || y >= GBA_HEIGHT) return;
    u16 dispcnt = io_read16(REG_DISPCNT);
    int mode = dispcnt & 7;
    u32* out = &s_framebuf[y * GBA_WIDTH];

    if (y == 0) { affine_latch(0, 1); affine_latch(1, 1); }
    if (dispcnt & 0x80) {   /* forced blank: white */
        for (int x = 0; x < GBA_WIDTH; x++) out[x] = 0xFFFFFFFFu;
        return;
    }

    u16 backdrop = pal_read16(0);
    for (int x = 0; x < GBA_WIDTH; x++) {
        s_top_c[x] = s_bot_c[x] = backdrop;
        s_top_l[x] = s_bot_l[x] = 5;
        s_top_semi[x] = 0;
    }

    /* Back to front: priority 3 first; within a priority, higher BG numbers
     * are further back, and OBJs sit in front of BGs of the same priority */
    for (int prio = 3; prio >= 0; prio--) {
        for (int bg = 3; bg >= 0; bg--) {
            if (!(dispcnt & (0x100 << bg))) continue;
            if ((io_read16((u16)(REG_BG0CNT + bg * 2)) & 3) != prio) continue;
            if (mode == 0 || (mode == 1 && bg < 2)) line_text_bg(bg, y);
            else if ((mode == 1 && bg == 2) || (mode == 2 && bg >= 2)) line_affine_bg(bg, y);
            else if (bg == 2 && mode == 3) line_mode3(y);
            else if (bg == 2 && mode == 4) line_mode4(y, dispcnt);
        }
        line_objs(y, dispcnt, prio);
    }
    /* affine reference points still advance for layers that weren't drawn */
    for (int i = 0; i < 2; i++) {
        int bg = i + 2;
        int drawn = (dispcnt & (0x100 << bg)) && ((mode == 1 && bg == 2) || (mode == 2));
        if (!drawn) {
            u32 base = 0x020 + (u32)i * 0x10;
            s_aff_x[i] += (s16)io_read16((u16)(base + 2));
            s_aff_y[i] += (s16)io_read16((u16)(base + 6));
        }
    }

    /* Colour effects (BLDCNT): alpha blend, brighten, darken. Windows aren't
     * applied, so an effect covers the whole line. */
    u16 bldcnt = io_read16(0x050), bldalpha = io_read16(0x052);
    int effect = (bldcnt >> 6) & 3;
    int t1 = bldcnt & 0x3F, t2 = (bldcnt >> 8) & 0x3F;
    int eva = bldalpha & 0x1F, evb = (bldalpha >> 8) & 0x1F, evy = io_read16(0x054) & 0x1F;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    for (int x = 0; x < GBA_WIDTH; x++) {
        u16 c = s_top_c[x];
        int top_in_t1 = (t1 >> s_top_l[x]) & 1, bot_in_t2 = (t2 >> s_bot_l[x]) & 1;
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        if ((s_top_semi[x] || (effect == 1 && top_in_t1)) && bot_in_t2) {
            u16 d = s_bot_c[x];
            r = (r * eva + (d & 31) * evb) >> 4;
            g = (g * eva + ((d >> 5) & 31) * evb) >> 4;
            b = (b * eva + ((d >> 10) & 31) * evb) >> 4;
            if (r > 31) r = 31;
            if (g > 31) g = 31;
            if (b > 31) b = 31;
        } else if (effect == 2 && top_in_t1) {
            r += ((31 - r) * evy) >> 4; g += ((31 - g) * evy) >> 4; b += ((31 - b) * evy) >> 4;
        } else if (effect == 3 && top_in_t1) {
            r -= (r * evy) >> 4; g -= (g * evy) >> 4; b -= (b * evy) >> 4;
        }
        out[x] = gba_to_argb((u16)(r | (g << 5) | (b << 10)));
    }
}

/* ---------- Public API ---------- */

int display_headless(void) { return s_headless; }

static long s_log_every = 300;
long display_log_every(void) { return s_log_every; }

/* --dump-at N file (repeatable): raw memory snapshots, same layout as
 * tools/oracle/mgba_oracle so the two diff directly */
static long s_dump_frame[16];
static const char* s_dump_path[16];
static int s_ndump;
const char* display_dump_path(long frame) {
    for (int d = 0; d < s_ndump; d++)
        if (s_dump_frame[d] == frame) return s_dump_path[d];
    return NULL;
}

const char* display_parse_args(int argc, char* argv[]) {
    const char* rom = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) s_headless = 1;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) { s_record_path = argv[++i]; s_headless = 1; }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) s_max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--log-every") && i + 1 < argc) s_log_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--dump-at") && i + 2 < argc && s_ndump < 16) {
            s_dump_frame[s_ndump] = atol(argv[++i]);
            s_dump_path[s_ndump++] = argv[++i];
        }
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) s_shot_path = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            s_input = fopen(argv[++i], "r");
            if (!s_input) fprintf(stderr, "[display] cannot open input script %s\n", argv[i]);
        }
        else if (argv[i][0] != '-' && !rom) rom = argv[i];
        else fprintf(stderr, "[display] unknown option %s\n", argv[i]);
    }
    return rom;
}

static void input_script_step(void) {
    while (s_input) {
        if (s_next_input_frame < 0) {
            char line[128];
            if (!fgets(line, sizeof(line), s_input)) { fclose(s_input); s_input = NULL; return; }
            unsigned keys;
            if (line[0] == '#' || sscanf(line, "%ld %x", &s_next_input_frame, &keys) != 2) {
                s_next_input_frame = -1; continue;
            }
            s_next_input_keys = (u16)keys;
        }
        if (s_frame_no < s_next_input_frame) return;
        s_keys_pressed = s_next_input_keys;
        s_next_input_frame = -1;
    }
}

static void write_bmp(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    u32 data = GBA_WIDTH * GBA_HEIGHT * 4, size = 54 + data;
    u8 h[54] = { 'B','M' };
    memcpy(h + 2, &size, 4); h[10] = 54; h[14] = 40;
    s32 w = GBA_WIDTH, hh = -GBA_HEIGHT; /* top-down */
    memcpy(h + 18, &w, 4); memcpy(h + 22, &hh, 4);
    h[26] = 1; h[28] = 32; memcpy(h + 34, &data, 4);
    fwrite(h, 1, 54, f);
    fwrite(s_framebuf, 1, data, f);
    fclose(f);
}

int display_init(void) {
    if (s_record_path) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s %dx%d -r 60 -i - "
            "-vf scale=%d:%d:flags=neighbor -pix_fmt yuv420p \"%s\"",
            GBA_WIDTH, GBA_HEIGHT, WIN_WIDTH, WIN_HEIGHT, s_record_path);
        s_record = popen(cmd, "wb");
        if (!s_record) { fprintf(stderr, "[display] cannot start ffmpeg\n"); return -1; }
    }
    if (s_headless) return 0;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return -1;
    }

    s_window = SDL_CreateWindow(
        "gbarecomp",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_WIDTH, WIN_HEIGHT,
        SDL_WINDOW_SHOWN
    );
    if (!s_window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(s_window);
        SDL_Quit();
        return -1;
    }

    s_texture = SDL_CreateTexture(s_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        GBA_WIDTH, GBA_HEIGHT);
    if (!s_texture) {
        SDL_Log("SDL_CreateTexture failed: %s", SDL_GetError());
        SDL_DestroyRenderer(s_renderer);
        SDL_DestroyWindow(s_window);
        SDL_Quit();
        return -1;
    }

    /* Set scaling to nearest neighbor for crisp pixels */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    return 0;
}

void display_render_frame(void) {
    if (!io_regs || !vram || !palette || !oam)
        return;

    u16 dispcnt = io_read16(REG_DISPCNT);
    int mode = dispcnt & 0x07;

    /* Clear framebuffer to backdrop color (palette entry 0) */
    {
        u16 backdrop = pal_read16(0);
        u32 bg_color = gba_to_argb(backdrop);
        for (int i = 0; i < GBA_WIDTH * GBA_HEIGHT; i++)
            s_framebuf[i] = bg_color;
    }

    switch (mode) {
    case 0:
        /* Mode 0: 4 regular tiled BG layers.
         * Render back-to-front by priority. BG3 is often lowest priority. */
        /* Simple approach: render BG3, BG2, BG1, BG0 in that order.
         * A fully correct implementation would sort by BGCNT priority bits,
         * but this ordering covers most games. */
        render_bg_mode0(3, dispcnt);
        render_bg_mode0(2, dispcnt);
        render_bg_mode0(1, dispcnt);
        render_bg_mode0(0, dispcnt);
        break;

    case 3:
        render_mode3();
        break;

    case 4:
        render_mode4(dispcnt);
        break;

    case 1:
        /* Mode 1: BG0, BG1 regular tiled; BG2 affine (stub as regular) */
        render_bg_mode0(2, dispcnt);
        render_bg_mode0(1, dispcnt);
        render_bg_mode0(0, dispcnt);
        break;

    case 2:
        /* Mode 2: BG2, BG3 affine only - stub */
        break;

    case 5:
        /* Mode 5: 160x128 bitmap, 16bpp, 2 pages - stub */
        break;

    default:
        break;
    }

    /* Render sprites on top */
    render_objs(dispcnt);

    s_frame_no++;
    input_script_step();
    if (s_record) fwrite(s_framebuf, 4, GBA_WIDTH * GBA_HEIGHT, s_record);
    if (s_max_frames && s_frame_no >= s_max_frames) {
        if (s_shot_path) write_bmp(s_shot_path);
        return;
    }
    if (s_headless) return;

    /* Upload framebuffer to texture and present */
    SDL_UpdateTexture(s_texture, NULL, s_framebuf, GBA_WIDTH * (int)sizeof(u32));
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

int display_poll_events(void) {
    if (s_max_frames && s_frame_no >= s_max_frames) return 1;
    if (s_headless) return 0;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT)
            return 1;

        if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            u16 bit = 0;
            switch (ev.key.keysym.sym) {
            case SDLK_z:         bit = (1 << 0); break;  /* A */
            case SDLK_x:         bit = (1 << 1); break;  /* B */
            case SDLK_BACKSPACE: bit = (1 << 2); break;  /* Select */
            case SDLK_RETURN:    bit = (1 << 3); break;  /* Start */
            case SDLK_RIGHT:     bit = (1 << 4); break;  /* Right */
            case SDLK_LEFT:      bit = (1 << 5); break;  /* Left */
            case SDLK_UP:        bit = (1 << 6); break;  /* Up */
            case SDLK_DOWN:      bit = (1 << 7); break;  /* Down */
            case SDLK_s:         bit = (1 << 8); break;  /* R */
            case SDLK_a:         bit = (1 << 9); break;  /* L */
            default: break;
            }

            if (bit) {
                if (ev.type == SDL_KEYDOWN)
                    s_keys_pressed |= bit;
                else
                    s_keys_pressed &= ~bit;
            }
        }
    }
    return 0;
}

u16 display_get_keys(void) {
    /* KEYINPUT is active-low: 0 = pressed, 1 = not pressed */
    return (u16)(~s_keys_pressed) & 0x03FF;
}

void display_shutdown(void) {
    if (s_shot_path && !(s_max_frames && s_frame_no >= s_max_frames)) write_bmp(s_shot_path);
    if (s_record) { pclose(s_record); s_record = NULL; }
    if (s_headless) return;
    if (s_texture)  { SDL_DestroyTexture(s_texture);   s_texture  = NULL; }
    if (s_renderer) { SDL_DestroyRenderer(s_renderer); s_renderer = NULL; }
    if (s_window)   { SDL_DestroyWindow(s_window);     s_window   = NULL; }
    SDL_Quit();
}
