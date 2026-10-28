/* nfs_widescreen.h - wide-display HUD/FOV geometry (see widescreen.c). */
#ifndef NFS_WIDESCREEN_H
#define NFS_WIDESCREEN_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <math.h>

/* The game's back buffer at the end of its resolution setter (0x4086B0). The
 * game only knows its five 4:3 menu modes (640x480 .. 1280x1024: at most 1024
 * lines). A host render size (render_w x render_h, the launcher's internal
 * resolution; 0 = none) replaces the mode: with the widescreen fix (display
 * aspect > 0) at its height with the display's shape, else at its height with a
 * 4:3 width that the swap chain stretches like the original. Without one the
 * mode's height stays and only the width follows the display. Sizes are even. */
static inline void nfs_ws_back_buffer(uint32_t game_w, uint32_t game_h, uint32_t render_w, uint32_t render_h,
                                      float display_aspect, uint32_t *out_w, uint32_t *out_h) {
    uint32_t w = game_w, h = game_h;
    if (render_w >= 320u && render_h >= 240u) {
        h = render_h & ~1u;
        w = (uint32_t)lroundf((float)h * (display_aspect > 0.0f ? display_aspect : 4.0f / 3.0f)) & ~1u;
    } else if (display_aspect > 0.0f && h) {
        uint32_t wide = (uint32_t)lroundf((float)h * display_aspect) & ~1u;
        if (wide > w) w = wide;
    }
    *out_w = w;
    *out_h = h;
}

/* Kept for the shared host interface (no data file needed for NFSU1 yet). */
void nfs_widescreen_init(const char* game_root);
/* Display width/height (> 4:3 turns the fix on: render size widened, 3D FOV
 * corrected); 0 keeps the original 4:3 game. */
void nfs_widescreen_set_display_aspect(float aspect);
/* The game's render size; call before its HUD draws and on every change. */
void nfs_widescreen_set_resolution(unsigned width, unsigned height);
/* The host's internal resolution (Android launcher), before the game sets its
 * video mode: the game's back buffer follows it (nfs_ws_back_buffer). 0 keeps
 * the game's own menu mode. Applies with and without the widescreen fix. */
void nfs_widescreen_set_render_size(unsigned width, unsigned height);
/* Touch hosts (Android): the mouse cursor is handed out in front-end units,
 * so its clamp spans the wide HUD space (see nfs_widescreen_hud_pos_x). */
void nfs_frontend_cursor_units(int on);
/* fHudPosX: the FE/HUD centre; the visible FE space is 320 -/+ this value. */
float nfs_widescreen_hud_pos_x(void);
/* HUD offset entries loaded from the .dat (0: elements keep 4:3 anchors). */
int nfs_widescreen_hud_entries(void);
/* Vertical offset (480-line HUD units, negative = up) added to the minimap. */
void nfs_widescreen_set_minimap_offset(float dy);
/* Game frame pacing (framerate.c; the game caps itself at 60 with a busy-wait
 * and clamps its time step to 1/60): fps >= 30 becomes the limiter's target
 * and, above 60, the time-step floor; 0 keeps the original. host_paced: the
 * host sleeps to the cap after each present, so the game's own busy-wait is
 * switched off (nfs_fr_plan). apply() re-asserts the target once per present.
 * The hooks that make rates above 60 correct (nfs_framerate.h) are always in
 * effect, as in ThirteenAG's fix. */
void nfs_widescreen_set_frame_rate(unsigned fps, int host_paced);
void nfs_widescreen_apply_frame_rate(void);

#ifdef __cplusplus
}
#endif
#endif
