/*
 * ImGui Menu System for GBA Recompiled
 *
 * Provides: File (save/load state), Config (debug), Graphics (scale/filter),
 * Audio (channels/volume), Controller (mapping/config)
 */

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <SDL2/SDL.h>

extern "C" {

/* ---- State ---- */
static SDL_Renderer* s_menu_renderer = NULL;
static bool menu_initialized = false;
static bool show_debug_console = false;
static bool show_controller_config = false;
static int graphics_scale = 3;
static int filter_mode = 0; /* 0=nearest, 1=linear */
static float audio_volume = 1.0f;
static bool audio_channels[6] = { true, true, true, true, true, true }; /* PSG1-4 + FIFOA + FIFOB */
static char debug_log[4096] = {0};
static int debug_log_offset = 0;

/* Key bindings */
static struct {
    SDL_Scancode a, b, start, select;
    SDL_Scancode up, down, left, right;
    SDL_Scancode l, r;
} keybinds = {
    SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE,
    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_A, SDL_SCANCODE_S
};

/* Callbacks (set by runtime) */
static void (*save_state_cb)(int slot) = NULL;
static void (*load_state_cb)(int slot) = NULL;
static void (*set_scale_cb)(int scale) = NULL;
static void (*set_filter_cb)(int filter) = NULL;
static void (*set_volume_cb)(float vol) = NULL;

/* ---- Public API (C linkage) ---- */

void menu_init(SDL_Window* window, SDL_Renderer* renderer) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.Alpha = 0.95f;

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    s_menu_renderer = renderer;
    menu_initialized = true;
}

void menu_shutdown(void) {
    if (!menu_initialized) return;
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    menu_initialized = false;
}

int menu_process_event(const SDL_Event* event) {
    if (!menu_initialized) return 0;
    ImGui_ImplSDL2_ProcessEvent(event);
    ImGuiIO& io = ImGui::GetIO();
    /* Return 1 if ImGui wants the input (menu is active) */
    return io.WantCaptureKeyboard || io.WantCaptureMouse ? 1 : 0;
}

void menu_set_callbacks(
    void (*save_cb)(int), void (*load_cb)(int),
    void (*scale_cb)(int), void (*filter_cb)(int),
    void (*volume_cb)(float))
{
    save_state_cb = save_cb;
    load_state_cb = load_cb;
    set_scale_cb = scale_cb;
    set_filter_cb = filter_cb;
    set_volume_cb = volume_cb;
}

void menu_add_debug_log(const char* msg) {
    int len = (int)strlen(msg);
    if (debug_log_offset + len + 2 < (int)sizeof(debug_log)) {
        memcpy(debug_log + debug_log_offset, msg, len);
        debug_log[debug_log_offset + len] = '\n';
        debug_log_offset += len + 1;
        debug_log[debug_log_offset] = '\0';
    } else {
        /* Wrap around */
        debug_log_offset = 0;
        memcpy(debug_log, msg, len);
        debug_log[len] = '\n';
        debug_log_offset = len + 1;
        debug_log[debug_log_offset] = '\0';
    }
}

static const char* scancode_name(SDL_Scancode sc) {
    return SDL_GetScancodeName(sc);
}

static bool editing_key = false;
static int editing_key_index = -1;

static void key_binding_row(const char* label, SDL_Scancode* key, int index) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("%s", label);
    ImGui::TableNextColumn();

    char buf[64];
    if (editing_key && editing_key_index == index) {
        snprintf(buf, sizeof(buf), "[Press a key...]##key%d", index);
        if (ImGui::Button(buf, ImVec2(-1, 0))) {
            editing_key = false;
        }
        /* Check for key press */
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        for (int i = 4; i < SDL_NUM_SCANCODES; i++) {
            if (keys[i] && i != SDL_SCANCODE_ESCAPE) {
                *key = (SDL_Scancode)i;
                editing_key = false;
                break;
            }
        }
    } else {
        snprintf(buf, sizeof(buf), "%s##key%d", scancode_name(*key), index);
        if (ImGui::Button(buf, ImVec2(-1, 0))) {
            editing_key = true;
            editing_key_index = index;
        }
    }
}

void menu_render(void) {
    if (!menu_initialized) return;

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    /* Main menu bar */
    if (ImGui::BeginMainMenuBar()) {
        /* File Menu */
        if (ImGui::BeginMenu("File")) {
            if (ImGui::BeginMenu("Save State")) {
                for (int i = 1; i <= 4; i++) {
                    char label[32];
                    snprintf(label, sizeof(label), "Slot %d", i);
                    if (ImGui::MenuItem(label)) {
                        if (save_state_cb) save_state_cb(i);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Load State")) {
                for (int i = 1; i <= 4; i++) {
                    char label[32];
                    snprintf(label, sizeof(label), "Slot %d", i);
                    if (ImGui::MenuItem(label)) {
                        if (load_state_cb) load_state_cb(i);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Escape")) {
                SDL_Event quit_event;
                quit_event.type = SDL_QUIT;
                SDL_PushEvent(&quit_event);
            }
            ImGui::EndMenu();
        }

        /* Config Menu */
        if (ImGui::BeginMenu("Config")) {
            ImGui::MenuItem("Debug Console", "F12", &show_debug_console);
            ImGui::MenuItem("Controller Config", NULL, &show_controller_config);
            ImGui::EndMenu();
        }

        /* Graphics Menu */
        if (ImGui::BeginMenu("Graphics")) {
            ImGui::Text("Window Scale");
            for (int s = 1; s <= 6; s++) {
                char label[16];
                snprintf(label, sizeof(label), "%dx", s);
                if (ImGui::RadioButton(label, graphics_scale == s)) {
                    graphics_scale = s;
                    if (set_scale_cb) set_scale_cb(s);
                }
                if (s < 6) ImGui::SameLine();
            }
            ImGui::Separator();
            ImGui::Text("Texture Filter");
            if (ImGui::RadioButton("Nearest (sharp)", filter_mode == 0)) {
                filter_mode = 0;
                if (set_filter_cb) set_filter_cb(0);
            }
            if (ImGui::RadioButton("Linear (smooth)", filter_mode == 1)) {
                filter_mode = 1;
                if (set_filter_cb) set_filter_cb(1);
            }
            ImGui::EndMenu();
        }

        /* Audio Menu */
        if (ImGui::BeginMenu("Audio")) {
            ImGui::SliderFloat("Master Volume", &audio_volume, 0.0f, 1.0f, "%.2f");
            if (set_volume_cb) set_volume_cb(audio_volume);
            ImGui::Separator();
            ImGui::Text("Channels");
            ImGui::Checkbox("PSG 1 (Pulse)", &audio_channels[0]);
            ImGui::Checkbox("PSG 2 (Pulse)", &audio_channels[1]);
            ImGui::Checkbox("PSG 3 (Wave)", &audio_channels[2]);
            ImGui::Checkbox("PSG 4 (Noise)", &audio_channels[3]);
            ImGui::Checkbox("FIFO A (DMA)", &audio_channels[4]);
            ImGui::Checkbox("FIFO B (DMA)", &audio_channels[5]);
            ImGui::EndMenu();
        }

        /* Controller Menu */
        if (ImGui::BeginMenu("Controller")) {
            ImGui::MenuItem("Configure Keys...", NULL, &show_controller_config);
            ImGui::Separator();
            int num_joysticks = SDL_NumJoysticks();
            if (num_joysticks > 0) {
                ImGui::Text("Gamepads: %d detected", num_joysticks);
                for (int i = 0; i < num_joysticks && i < 4; i++) {
                    ImGui::BulletText("%s", SDL_JoystickNameForIndex(i));
                }
            } else {
                ImGui::TextDisabled("No gamepads detected");
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }

    /* Debug Console Window */
    if (show_debug_console) {
        ImGui::SetNextWindowSize(ImVec2(500, 300), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Debug Console", &show_debug_console)) {
            ImGui::BeginChild("ScrollRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
            ImGui::TextUnformatted(debug_log);
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            if (ImGui::Button("Clear")) {
                debug_log[0] = '\0';
                debug_log_offset = 0;
            }
        }
        ImGui::End();
    }

    /* Controller Config Window */
    if (show_controller_config) {
        ImGui::SetNextWindowSize(ImVec2(300, 350), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Controller Configuration", &show_controller_config)) {
            ImGui::Text("Click a button to rebind, then press a key.");
            ImGui::Separator();

            if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                key_binding_row("A", &keybinds.a, 0);
                key_binding_row("B", &keybinds.b, 1);
                key_binding_row("Start", &keybinds.start, 2);
                key_binding_row("Select", &keybinds.select, 3);
                key_binding_row("Up", &keybinds.up, 4);
                key_binding_row("Down", &keybinds.down, 5);
                key_binding_row("Left", &keybinds.left, 6);
                key_binding_row("Right", &keybinds.right, 7);
                key_binding_row("L", &keybinds.l, 8);
                key_binding_row("R", &keybinds.r, 9);

                ImGui::EndTable();
            }

            ImGui::Separator();
            if (ImGui::Button("Reset to Defaults")) {
                keybinds.a = SDL_SCANCODE_Z;
                keybinds.b = SDL_SCANCODE_X;
                keybinds.start = SDL_SCANCODE_RETURN;
                keybinds.select = SDL_SCANCODE_BACKSPACE;
                keybinds.up = SDL_SCANCODE_UP;
                keybinds.down = SDL_SCANCODE_DOWN;
                keybinds.left = SDL_SCANCODE_LEFT;
                keybinds.right = SDL_SCANCODE_RIGHT;
                keybinds.l = SDL_SCANCODE_A;
                keybinds.r = SDL_SCANCODE_S;
            }
        }
        ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), s_menu_renderer);
}

/* Get current key state using configurable bindings */
unsigned short menu_get_keys(void) {
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    unsigned short state = 0;
    if (keys[keybinds.a])      state |= 0x001;
    if (keys[keybinds.b])      state |= 0x002;
    if (keys[keybinds.select]) state |= 0x004;
    if (keys[keybinds.start])  state |= 0x008;
    if (keys[keybinds.right])  state |= 0x010;
    if (keys[keybinds.left])   state |= 0x020;
    if (keys[keybinds.up])     state |= 0x040;
    if (keys[keybinds.down])   state |= 0x080;
    if (keys[keybinds.r])      state |= 0x100;
    if (keys[keybinds.l])      state |= 0x200;
    return state;
}

int menu_wants_input(void) {
    if (!menu_initialized) return 0;
    ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureKeyboard || io.WantCaptureMouse ? 1 : 0;
}

} /* extern "C" */
