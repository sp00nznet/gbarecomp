/*
 * mgba_oracle - run a ROM headless on mGBA and record what the hardware state
 * looks like, as ground truth for the recompiled build.
 *
 * Same flags and output shapes as the recompiled runtime (src/display.c,
 * src/runtime.c), so the two can be diffed line for line:
 *
 *   mgba_oracle game.gba [--frames N] [--input script.txt]
 *                        [--dump-at N out.bin] [--log-every K]
 *
 *   [frame N] DISPCNT=... BLDCNT=... BLDY=... pal0=... pal1=...   (stderr)
 *   --dump-at: EWRAM | IWRAM | IO | PAL | VRAM | OAM, raw, in that order
 *              (0x40000 + 0x8000 + 0x400 + 0x400 + 0x18000 + 0x400 bytes)
 *
 * mGBA is the reference, not a dependency: nothing in the recompiled game
 * links it. See docs/conformance.md.
 */
#include <mgba/flags.h>
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>

/* ORACLE_SWI=from,to: log each SWI in those frames with its scanline, to
 * line up against the runtime's GBA_SWI_LOG when timing drifts */
static struct GBA* s_gba;
static long s_frame, s_swi_from = -1, s_swi_to = -1;

static void quiet_log(struct mLogger* l, int cat, enum mLogLevel lvl, const char* fmt, va_list ap) {
    (void)l; (void)cat; (void)lvl;
    if (s_frame >= s_swi_from && s_frame <= s_swi_to && !strncmp(fmt, "SWI: ", 5)) {
        unsigned imm = va_arg(ap, unsigned);
        const int32_t* g = s_gba->cpu->gprs;
        fprintf(stderr, "SWI %02X f=%ld v=%d c=%u r0=%08X r1=%08X r2=%08X lr=%08X\n",
                imm, s_frame, s_gba->video.vcount, (unsigned)mTimingCurrentTime(&s_gba->timing),
                (unsigned)g[0], (unsigned)g[1], (unsigned)g[2], (unsigned)g[14]);
    }
}

static void dump(struct GBA* gba, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[oracle] cannot write %s\n", path); return; }
    fwrite(gba->memory.wram, 1, 0x40000, f);
    fwrite(gba->memory.iwram, 1, 0x8000, f);
    fwrite(gba->memory.io, 1, 0x400, f);
    fwrite(gba->video.palette, 1, 0x400, f);
    fwrite(gba->video.vram, 1, 0x18000, f);
    fwrite(gba->video.oam.raw, 1, 0x400, f);
    fclose(f);
}

int main(int argc, char* argv[]) {
    const char* rom = NULL;
    const char* input = NULL;
    const char* shot = NULL;   /* --screenshot out.bmp: last frame, like the runtime's */
    long frames = 600, log_every = 300;
    long dump_frame[16]; const char* dump_path[16]; int ndump = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "--log-every") && i + 1 < argc) log_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--dump-at") && i + 2 < argc && ndump < 16) {
            dump_frame[ndump] = atol(argv[++i]); dump_path[ndump++] = argv[++i];
        } else if (argv[i][0] != '-') rom = argv[i];
    }
    if (!rom) { fprintf(stderr, "usage: mgba_oracle game.gba [--frames N] [--input f] [--dump-at N out.bin]\n"); return 2; }

    static struct mLogger logger;
    logger.log = quiet_log;
    mLogSetDefaultLogger(&logger);

    struct mCore* core = GBACoreCreate();
    core->init(core);
    static mColor video[240 * 160];
    core->setVideoBuffer(core, video, 240);
    struct VFile* vf = VFileOpen(rom, O_RDONLY);
    if (!vf || !core->loadROM(core, vf)) { fprintf(stderr, "[oracle] cannot load %s\n", rom); return 1; }
    mCoreConfigInit(&core->config, "gbarecomp-oracle");
    mCoreConfigSetDefaultIntValue(&core->config, "skipBios", 1);
    mCoreConfigSetDefaultIntValue(&core->config, "useBios", 0);
    core->loadConfig(core, &core->config);
    core->reset(core);
    struct GBA* gba = core->board;
    s_gba = gba;
    {
        const char* e = getenv("ORACLE_SWI");
        if (e) sscanf(e, "%ld,%ld", &s_swi_from, &s_swi_to);
    }

    const char* we = getenv("ORACLE_WATCH");
    uint32_t watch = we ? (uint32_t)strtoul(we, NULL, 16) : 0;
    int watch_last = -1;

    FILE* in = input ? fopen(input, "r") : NULL;
    long next_frame = -1; unsigned next_keys = 0, keys = 0;

    for (long f = 1; f <= frames; f++) {
        /* Same script format as the runtime: "<frame> <keymask hex>", the
         * mask applying from that frame on (bit set = pressed) */
        while (in) {
            if (next_frame < 0) {
                char line[128];
                if (!fgets(line, sizeof(line), in)) { fclose(in); in = NULL; break; }
                if (line[0] == '#' || sscanf(line, "%ld %x", &next_frame, &next_keys) != 2) { next_frame = -1; continue; }
            }
            if (f < next_frame) break;
            keys = next_keys; next_frame = -1;
        }
        core->setKeys(core, keys);
        s_frame = f;
        core->runFrame(core);
        if (f <= 5 || (log_every && f % log_every == 0)) {
            uint16_t* io = gba->memory.io;
            uint16_t dispcnt = io[0];
            fprintf(stderr, "[frame %ld] DISPCNT=0x%04X mode=%d BG=%d%d%d%d OBJ=%d "
                    "BLDCNT=0x%04X BLDY=%u pal0=0x%04X pal1=0x%04X\n",
                    f, dispcnt, dispcnt & 7, (dispcnt >> 8) & 1, (dispcnt >> 9) & 1,
                    (dispcnt >> 10) & 1, (dispcnt >> 11) & 1, (dispcnt >> 12) & 1,
                    io[0x50 >> 1], io[0x54 >> 1] & 31, gba->video.palette[0], gba->video.palette[1]);
        }
        for (int d = 0; d < ndump; d++)
            if (dump_frame[d] == f) dump(gba, dump_path[d]);
        if (watch) {
            /* ORACLE_WATCH=addr: the byte there at each frame end, when it
             * changes; a game flag timeline to place script presses by */
            int v = core->rawRead8(core, watch, -1);
            if (v != watch_last) fprintf(stderr, "[watch] frame %ld: [%08X] = %02X\n", f, watch, v);
            watch_last = v;
        }
    }
    if (shot) {
        /* mColor is XBGR8888 here: write a top-down 32-bit BMP (BGRA) */
        FILE* f = fopen(shot, "wb");
        if (f) {
            uint32_t data = 240 * 160 * 4, size = 54 + data;
            int32_t w = 240, h = -160;
            uint8_t hd[54] = { 'B', 'M' };
            memcpy(hd + 2, &size, 4); hd[10] = 54; hd[14] = 40;
            memcpy(hd + 18, &w, 4); memcpy(hd + 22, &h, 4);
            hd[26] = 1; hd[28] = 32; memcpy(hd + 34, &data, 4);
            fwrite(hd, 1, 54, f);
            for (int i = 0; i < 240 * 160; i++) {
                uint32_t c = video[i];
                uint8_t px[4] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, 0xFF };
                fwrite(px, 1, 4, f);
            }
            fclose(f);
        }
    }
    core->deinit(core);
    return 0;
}
