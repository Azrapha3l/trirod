// In-game overlay menu (Dear ImGui). Open with F1 / Esc, or Minus+Plus (Back+Start) on a controller.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef union SDL_Event SDL_Event;

// Main (UI) thread: returns true if the event was consumed by the menu (the game must not see it).
bool menu_handle_event(const SDL_Event *e);
bool menu_is_open(void);
// Any thread: a centred progress panel that menu_render draws (title NULL hides it; fraction < 0: no bar). Used by the
// shader preparation Trirod.exe runs after a GPU / driver change (device.c prepare_shaders_in_game). While it shows,
// Esc or B (East) / Back on a controller asks to skip: menu_progress_skipped().
void menu_set_progress(const char *title, const char *detail, float fraction);
bool menu_progress_skipped(void);
// Main thread: apply window requests (fullscreen) made from the menu.
void menu_apply_window_requests(void *sdl_window);

#if defined(EOW_METAL) && defined(__OBJC__)
// Render thread: draw the overlay into the drawable after the game image (no-op while closed).
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target);
#elif !defined(EOW_METAL)
// Render thread, inside the window image's rendering (Vulkan command buffer cmd).
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h);
#endif

#ifdef __cplusplus
}
#endif
