#ifndef GBA_INTERCEPTION_H
#define GBA_INTERCEPTION_H

#include "gba/types.h"

struct mCore;

typedef void (*RecompFunc)(void);

typedef struct {
    u32 addr;
    RecompFunc func;
} FuncEntry;

/* Initialize function interception with a sorted table of ROM functions. */
void interception_init(FuncEntry* table, int size);

/* Shutdown interception. */
void interception_shutdown(void);

/* Run one frame with function interception active. */
void interception_run_frame(struct mCore* core);

/* Get total number of interceptions so far. */
int interception_get_count(void);

#endif
