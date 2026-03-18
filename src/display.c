/*
 * GBA Display - SDL2 rendering backend for gbarecomp
 *
 * Implements Mode 0 (tiled), Mode 3 (bitmap 16bpp), Mode 4 (bitmap 8bpp),
 * plus basic OBJ (sprite) rendering.
 */

#include <SDL2/SDL.h>
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

/* ---------- OBJ (sprite) rendering ---------- */

static void render_objs(u16 dispcnt) {
    /* Check if OBJ rendering is enabled (bit 12) */
    if (!(dispcnt & 0x1000))
        return;

    /* OBJ VRAM base: 0x06010000 = vram offset 0x10000 */
    u32 obj_vram_base = 0x10000;

    /* OBJ tile mapping: bit 6 of DISPCNT
     * 0 = 2D mapping (tiles arranged in 32-tile-wide grid)
     * 1 = 1D mapping (tiles sequential) */
    int mapping_1d = (dispcnt >> 6) & 1;

    /* Render back-to-front: higher OAM index = lower priority when overlapping.
     * Iterate in reverse so lower-index sprites draw on top. */
    for (int i = 127; i >= 0; i--) {
        u32 oam_off = (u32)i * 8;
        u16 attr0 = (u16)oam[oam_off]     | ((u16)oam[oam_off + 1] << 8);
        u16 attr1 = (u16)oam[oam_off + 2] | ((u16)oam[oam_off + 3] << 8);
        u16 attr2 = (u16)oam[oam_off + 4] | ((u16)oam[oam_off + 5] << 8);

        /* OBJ mode: 0=normal, 1=semi-transparent, 2=OBJ window, 3=forbidden(hidden) */
        int obj_mode = (attr0 & OAM_ATTR0_MODE_MASK) >> OAM_ATTR0_MODE_SHIFT;
        if (obj_mode == 2 || obj_mode == 3)
            continue; /* Skip disabled/window sprites */

        /* If affine bit set but we only support non-rotated, check double-size disable bit */
        int is_affine = (attr0 >> 8) & 1;
        if (is_affine)
            continue; /* Skip affine/rotated sprites for now */

        /* Check for hidden flag: attr0 bits 8-9 == 0b10 means OBJ is hidden (non-affine) */
        /* Actually: bit 8=0 (non-affine) and bit 9=1 means hidden */
        int hidden = (attr0 >> 9) & 1;
        if (!is_affine && hidden)
            continue;

        int shape = (attr0 & OAM_ATTR0_SHAPE_MASK) >> OAM_ATTR0_SHAPE_SHIFT;
        int size  = (attr1 & OAM_ATTR1_SIZE_MASK) >> OAM_ATTR1_SIZE_SHIFT;

        if (shape > 2) continue;

        int w = obj_width[shape][size];
        int h = obj_height[shape][size];

        int obj_x = attr1 & OAM_ATTR1_X_MASK;
        int obj_y = attr0 & OAM_ATTR0_Y_MASK;

        /* X is 9-bit signed, Y is 8-bit and wraps */
        if (obj_x >= 240) obj_x -= 512;
        if (obj_y >= 160) obj_y -= 256;

        int hflip = (attr1 & OAM_ATTR1_HFLIP) ? 1 : 0;
        int vflip = (attr1 & OAM_ATTR1_VFLIP) ? 1 : 0;

        u16 tile_idx  = attr2 & OAM_ATTR2_TILE_MASK;
        int pal_num   = (attr2 & OAM_ATTR2_PAL_MASK) >> OAM_ATTR2_PAL_SHIFT;
        int bpp8      = (attr0 & OAM_ATTR0_BPP) ? 1 : 0;

        /* Tile size in bytes */
        int tile_bytes = bpp8 ? 64 : 32;

        /* Width of a tile row in the 2D mapping grid (in tiles) */
        int row_tiles_2d = bpp8 ? 16 : 32;

        /* Number of 8x8 tiles this sprite spans */
        int tiles_w = w / 8;
        int tiles_h = h / 8;

        for (int py = 0; py < h; py++) {
            int screen_y = obj_y + py;
            if (screen_y < 0 || screen_y >= GBA_HEIGHT) continue;

            int fy = vflip ? (h - 1 - py) : py;

            for (int px = 0; px < w; px++) {
                int screen_x = obj_x + px;
                if (screen_x < 0 || screen_x >= GBA_WIDTH) continue;

                int fx = hflip ? (w - 1 - px) : px;

                /* Which 8x8 tile within the sprite */
                int tx = fx / 8;
                int ty = fy / 8;

                /* Pixel within the 8x8 tile */
                int tpx = fx & 7;
                int tpy = fy & 7;

                /* Calculate tile number */
                u32 tile_num;
                if (mapping_1d) {
                    /* 1D: tiles are sequential.
                     * In 8bpp mode, tile_idx is in units of 32 bytes (same as 4bpp tile),
                     * so each 8bpp tile consumes 2 "tile slots". */
                    if (bpp8) {
                        tile_num = tile_idx + (u32)(ty * tiles_w * 2 + tx * 2);
                    } else {
                        tile_num = tile_idx + (u32)(ty * tiles_w + tx);
                    }
                } else {
                    /* 2D: tiles arranged in a 32-wide (4bpp) or 16-wide (8bpp) grid */
                    if (bpp8) {
                        tile_num = tile_idx + (u32)(ty * row_tiles_2d * 2 + tx * 2);
                    } else {
                        tile_num = tile_idx + (u32)(ty * row_tiles_2d + tx);
                    }
                }

                u32 tile_addr = obj_vram_base + tile_num * 32; /* always 32-byte units in VRAM */

                u8 color_idx;
                if (bpp8) {
                    color_idx = vram_read8(tile_addr + (u32)(tpy * 8 + tpx));
                } else {
                    u8 byte = vram_read8(tile_addr + (u32)(tpy * 4 + tpx / 2));
                    color_idx = (tpx & 1) ? (byte >> 4) : (byte & 0x0F);
                }

                if (color_idx == 0)
                    continue; /* Transparent */

                /* OBJ palette is the second half of palette RAM (offset 0x200) */
                u16 pal_color;
                if (bpp8) {
                    pal_color = pal_read16(0x200 + (u32)color_idx * 2);
                } else {
                    pal_color = pal_read16(0x200 + (u32)(pal_num * 16 + color_idx) * 2);
                }

                s_framebuf[screen_y * GBA_WIDTH + screen_x] = gba_to_argb(pal_color);
            }
        }
    }
}

/* ---------- Public API ---------- */

int display_init(void) {
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

    /* Upload framebuffer to texture and present */
    SDL_UpdateTexture(s_texture, NULL, s_framebuf, GBA_WIDTH * (int)sizeof(u32));
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

int display_poll_events(void) {
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
    if (s_texture)  { SDL_DestroyTexture(s_texture);   s_texture  = NULL; }
    if (s_renderer) { SDL_DestroyRenderer(s_renderer); s_renderer = NULL; }
    if (s_window)   { SDL_DestroyWindow(s_window);     s_window   = NULL; }
    SDL_Quit();
}
