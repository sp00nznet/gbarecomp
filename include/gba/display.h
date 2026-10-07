#ifndef GBA_DISPLAY_H
#define GBA_DISPLAY_H
#include "gba/types.h"

/* Parse runtime flags (--headless, --record out.mp4, --frames N,
 * --screenshot out.bmp, --input script.txt). Returns the ROM path or NULL. */
const char* display_parse_args(int argc, char* argv[]);

/* Nonzero when running without a window (no frame pacing either). */
int display_headless(void);

/* --log-every K: frame-status line interval (0 = only the first 5 frames). */
long display_log_every(void);

/* --dump-at N file: path to snapshot memory to at frame N, or NULL. */
const char* display_dump_path(long frame);

/* Initialize SDL2 window (240x160 at 3x scale). Returns 0 on success. */
int display_init(void);

/* Compose scanline y from the current VRAM/palette/OAM/registers. The runtime
 * calls it at each visible line's HBlank, so mid-frame register changes show. */
void display_render_line(int y);

/* Present the composed frame (window, --record, --screenshot). */
void display_render_frame(void);

/* Poll SDL events. Returns 0 normally, 1 if quit requested. */
int display_poll_events(void);

/* Get current GBA key state (KEYINPUT register format, active-low). */
u16 display_get_keys(void);

/* Shutdown SDL. */
void display_shutdown(void);

#endif
