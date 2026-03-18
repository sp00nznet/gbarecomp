#ifndef GBA_DISPLAY_H
#define GBA_DISPLAY_H
#include "gba/types.h"

/* Initialize SDL2 window (240x160 at 3x scale). Returns 0 on success. */
int display_init(void);

/* Render one frame from GBA VRAM/palette/OAM to the SDL window. */
void display_render_frame(void);

/* Poll SDL events. Returns 0 normally, 1 if quit requested. */
int display_poll_events(void);

/* Get current GBA key state (KEYINPUT register format, active-low). */
u16 display_get_keys(void);

/* Shutdown SDL. */
void display_shutdown(void);

#endif
