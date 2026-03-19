#ifndef GBA_MENU_H
#define GBA_MENU_H

#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize ImGui menu system. Call after SDL window/renderer creation. */
void menu_init(SDL_Window* window, SDL_Renderer* renderer);

/* Shutdown ImGui. */
void menu_shutdown(void);

/* Process an SDL event. Returns 1 if ImGui consumed it (menu active). */
int menu_process_event(const SDL_Event* event);

/* Render the menu bar and any open windows. Call before SDL_RenderPresent. */
void menu_render(void);

/* Get GBA key state using configurable bindings. Returns active-high bitmask. */
unsigned short menu_get_keys(void);

/* Returns 1 if ImGui wants keyboard/mouse input (menu is active). */
int menu_wants_input(void);

/* Get the height of the menu bar in pixels. */
int menu_get_bar_height(void);

/* Add a line to the debug console. */
void menu_add_debug_log(const char* msg);

/* Set callbacks for menu actions. */
void menu_set_callbacks(
    void (*save_state_cb)(int slot),
    void (*load_state_cb)(int slot),
    void (*set_scale_cb)(int scale),
    void (*set_filter_cb)(int filter),
    void (*set_volume_cb)(float vol));

#ifdef __cplusplus
}
#endif

#endif /* GBA_MENU_H */
