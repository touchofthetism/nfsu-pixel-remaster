/*
 * widescreen.c - NFSU1 render size and 3D FOV for wide displays.
 *
 * A port of parts of ThirteenAG's NFSUnderground.WidescreenFix
 * (WidescreenFixesPack, MIT): Rendering.ixx (FixFOV) and the render-size side
 * of Resolution.ixx. That mod patches x86 bytes in memory; the game here is
 * lifted C, so each patch is a host hook (config/hooks.txt -> RECOMP_HOOK ->
 * recomp_hook) right after the instruction whose result it changes. The hook
 * VAs were located in the pinned Speed.exe 1.4 by the mod's byte patterns.
 *
 * Frontend.ixx (FixHUD, FMVWidescreenMode 1) is ported the same way: the HUD
 * projection scale and centre are data (0x6CC914, 0x6CC910); the 320.0 anchors
 * stored as immediates, the minimap, laps and HUD-element positions (offsets
 * from the mod's .dat, embedded in widescreen_hud.inc) and the FMV quad are
 * hooks, as are the 30 mouse hit tests (x - 320 -> x - fHudPosX). The touch
 * host hands out the mouse cursor in [0, 2*fHudPosX] (the mod's range) and the
 * cursor clamp (0x4131A0) is widened to it.
 *
 * Off until a host gives the display aspect (Android: the surface; the Windows
 * bring-up host keeps the original 4:3 game).
 * Shared by the Windows host and the Android runtime.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_types.h"
#include "nfs_widescreen.h"
#include "nfs_misc_fixes.h"
#include "nfs_reflections.h"
#include "nfs_garage.h"
#include "nfs_freeroam.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "nfs_runtime.h"
#elif defined(__ANDROID__)
#include <android/log.h>
#endif

static float s_display_aspect;          /* 0: off, the game keeps 4:3 */
static unsigned s_render_w, s_render_h;  /* host internal resolution, 0: the game's menu mode */
static float s_aspect = 4.0f / 3.0f;    /* the render target's, once widened */
/* FOV constants of the three reads after 0x40DDD2, chosen per view id. */
static double s_fov_scale[3] = {1.0, 0.5, 1.0};
static int s_fe_cursor;                 /* touch host: cursor in FE units */
void nfs_frontend_cursor_units(int on) { s_fe_cursor = on; }
/* FixHUD: fHudPosX (centre of the 4:3 HUD in wide HUD units) and the mod's
 * fWidescreenHudOffset (= fHudPosX - 320 with HudAspectRatioConstraint Auto). */
static float s_hud_pos_x = 320.0f;
/* Front-end mouse hit tests `reg = x - 320` (the mod repoints the immediate to
 * -fHudPosX): VA of the next instruction, register (0 eax, 1 ecx, 2 edx). */
static const struct { uint32_t va; uint8_t reg; } k_hit[] = {
    { 0x00415358u, 2 },
    { 0x00415CADu, 2 },
    { 0x004B7E14u, 2 },
    { 0x004B8C71u, 2 },
    { 0x004C1A39u, 2 },
    { 0x004D1DF9u, 2 },
    { 0x004D516Au, 2 },
    { 0x004D66D1u, 2 },
    { 0x004DC438u, 2 },
    { 0x004E39A8u, 2 },
    { 0x004EAA5Au, 2 },
    { 0x004F7EDFu, 2 },
    { 0x004F7F36u, 2 },
    { 0x0054F09Au, 2 },
    { 0x0054F363u, 2 },
    { 0x005518D8u, 2 },
    { 0x00557EDCu, 2 },
    { 0x00559BAAu, 2 },
    { 0x0055BAEAu, 2 },
    { 0x0055CD90u, 2 },
    { 0x0055D7F8u, 2 },
    { 0x0055F0DAu, 2 },
    { 0x00561C98u, 2 },
    { 0x00562128u, 2 },
    { 0x0056391Au, 2 },
    { 0x00563CFAu, 2 },
    { 0x004B8CF0u, 0 },
    { 0x004D6750u, 0 },
    { 0x004F7D65u, 0 },
    { 0x004F608Bu, 1 },
};
typedef struct { uint32_t x, y; float off_x, off_y; } hud_entry;
static const hud_entry k_hud[] = {
#include "widescreen_hud.inc"
};




static float rdf(uint32_t va) { uint32_t v = MEM32(va); float f; memcpy(&f, &v, 4); return f; }
static void wrf(uint32_t va, float f) { uint32_t v; memcpy(&v, &f, 4); MEM32(va) = v; }

/* The mod's WidescreenHud(): move a known HUD element (matched by the bits of
 * its anchor) by its .dat offset; not in the front end (game state 3). */
static void widescreen_hud(uint32_t *bx, uint32_t *by, int minimap) {
    const float base_offset = (480.0f * (16.0f / 9.0f) - 640.0f) / 2.0f;
    const float hud_offset = s_hud_pos_x - 320.0f;
    if (MEM32(0x0077A920u) == 3) return;
    for (unsigned i = 0; i < sizeof k_hud / sizeof k_hud[0]; i++) {
        float x, y;
        if (k_hud[i].x != *bx || k_hud[i].y != *by) continue;
        memcpy(&x, bx, 4); memcpy(&y, by, 4);
        if (k_hud[i].off_x >= 0.0f) x += hud_offset + (k_hud[i].off_x - base_offset);
        else x -= hud_offset - (k_hud[i].off_x + base_offset);
        y += k_hud[i].off_y;
        if (minimap) x += s_hud_pos_x - 320.0f;   /* - fMinimapPosX + fHudPosX */
        memcpy(bx, &x, 4); memcpy(by, &y, 4);
        return;
    }
}
static void hud_mem_pair(uint32_t va_x, uint32_t va_y, int minimap) {
    uint32_t x = MEM32(va_x), y = MEM32(va_y);
    widescreen_hud(&x, &y, minimap);
    MEM32(va_x) = x; MEM32(va_y) = y;
}
float nfs_widescreen_hud_pos_x(void) { return s_hud_pos_x; }

void nfs_widescreen_init(const char *game_root) { (void)game_root; }
void nfs_widescreen_set_display_aspect(float aspect) {
    s_display_aspect = aspect > 1.34f ? aspect : 0.0f;
}
void nfs_widescreen_set_resolution(unsigned width, unsigned height) {
    if (width && height) s_aspect = (float)width / (float)height;
}
void nfs_widescreen_set_render_size(unsigned width, unsigned height) {
    s_render_w = width;
    s_render_h = height;
}

/* End of the resolution setter (0x4086B0, width pushed, height in esi): it
 * filled D3DPRESENT_PARAMETERS (0x71A96C/0x71A970) and the screen size globals
 * (0x701034/0x701038) that the render targets, viewports and view setup read.
 * Every mode change (front end 800x600, the racing mode, a menu change) passes
 * here, so the device keeps the host's size throughout, as ThirteenAG's
 * Resolution.ixx makes every menu mode the desktop's. */
static void resolution_setter_end(void) {
    uint32_t game_w = MEM32(0x0071A96Cu), game_h = MEM32(0x0071A970u), w, h;
    static uint32_t logged_w, logged_h, logged_game_w, logged_game_h;
    nfs_ws_back_buffer(game_w, game_h, s_render_w, s_render_h, s_display_aspect, &w, &h);
    if (!w || !h) return;
    if (w != game_w || h != game_h) {
        MEM32(0x0071A96Cu) = w; MEM32(0x00701034u) = w;
        MEM32(0x0071A970u) = h; MEM32(0x00701038u) = h;
    }
    s_aspect = (float)w / (float)h;
    if (w != logged_w || h != logged_h || game_w != logged_game_w || game_h != logged_game_h) {
        logged_w = w; logged_h = h; logged_game_w = game_w; logged_game_h = game_h;
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "NFSU1", "Game render size %ux%u (%s; the game's own mode %ux%u)", w, h,
                            s_render_w ? "the launcher's internal resolution" : "the game's mode, widened", game_w, game_h);
#endif
    }
    /* FixHUD: fHudScaleX = 2/W * (H/480) (fmul at 0x40BF07), fHudPosX. */
    if (s_display_aspect > 0.0f) {
        float fw = (float)w, fh = (float)h;
        float scale = (1.0f / fw * (fh / 480.0f)) * 2.0f;
        s_hud_pos_x = 640.0f / (640.0f * scale);
        wrf(0x006CC914u, scale);
        wrf(0x006CC910u, s_hud_pos_x);   /* fcomp at 0x41E58F; the minimap is put back below */
    }
}
int nfs_widescreen_hud_entries(void) { return 0; }
void nfs_widescreen_set_minimap_offset(float dy) { (void)dy; }
/* Frame pacing (nfs_widescreen_set_frame_rate / apply) and its hooks live in
 * framerate.c; recomp_hook hands it every VA it does not know itself. */
int nfs_framerate_hook(uint32_t va);

void recomp_hook(uint32_t va) {
    /* Frame pacing (framerate.c), the other PC-community fixes (misc_fixes.c),
     * the reflection sizes (reflections.c), the garage freedoms (garage.c) and
     * free roam's tracks and names (freeroam.c) apply whatever the display aspect. */
    if (nfs_framerate_hook(va) || nfs_misc_hook(va) || nfs_reflections_hook(va) || nfs_garage_hook(va) ||
        nfs_freeroam_hook(va)) return;
    /* Arrow draw calls: X/Y loaded, about to be pushed for sub_004F3030. */
    /* Arrow draw calls: superseded (the touch cursor is already in FE units);
     * the lifted code still calls here, so they stay accepted and inert. */
    if (va == 0x00409EB8u || va == 0x00401832u) return;
    /* Mouse clamp (0x4131A0) after GetClientRect: the FE-unit cursor of a
     * touch host spans [0, 2 * fHudPosX] (the mod's ClientRectHook1/2). */
    if (va == 0x004131B5u) {
        if (!s_fe_cursor) return;
        MEM32(g_esp + 0x0) = 0;
        MEM32(g_esp + 0x4) = 0;
        MEM32(g_esp + 0x8) = (uint32_t)(int32_t)lroundf(2.0f * s_hud_pos_x);
        MEM32(g_esp + 0xC) = 480;
        return;
    }
    /* The video mode follows the host's internal resolution with or without the fix. */
    if (va == 0x0040875Au) { resolution_setter_end(); return; }
    if (s_display_aspect <= 0.0f) return;
    for (unsigned i = 0; i < sizeof k_hit / sizeof k_hit[0]; i++) {
        if (k_hit[i].va != va) continue;
        uint32_t *r = k_hit[i].reg == 0 ? &g_eax : k_hit[i].reg == 1 ? &g_ecx : &g_edx;
        *r += (uint32_t)(int32_t)lroundf(320.0f - s_hud_pos_x);
        return;
    }
    switch (va) {
    /* HUD anchors: `mov [esp+N], 320.0` -> fHudPosX. */
    case 0x004F20A7u: wrf(g_esp + 0x80, s_hud_pos_x); break;
    case 0x004F2333u: wrf(g_esp + 0x90, s_hud_pos_x); break;
    case 0x004F28B3u: wrf(g_esp + 0xA0, s_hud_pos_x); break;
    case 0x004F1E31u: wrf(g_esp + 0x60, s_hud_pos_x); break;
    case 0x004F26E9u: wrf(g_esp + 0x50, s_hud_pos_x); break;
    case 0x004F314Bu: wrf(g_esp + 0x19C, s_hud_pos_x); break;   /* mouse arrow origin */
    /* Minimap centre `fadd [0x6CC910]` keeps the original 320 (the mod's fMinimapPosX). */
    case 0x0058E5C3u: g_st[0] -= (double)(s_hud_pos_x - 320.0f); break;
    /* Rear-view mirror quad X (440/200/200/440), shifted with the HUD centre. */
    case 0x00409F75u: {
        const uint32_t xs[4] = { 0x6C, 0x7C, 0x8C, 0x9C };
        for (int i = 0; i < 4; i++) wrf(g_esp + xs[i], rdf(g_esp + xs[i]) + (s_hud_pos_x - 320.0f));
        break;
    }
    /* HudHook: eax/ecx = element X/Y just loaded from [edi+1C]/[edi+20]. */
    case 0x004F18D1u: widescreen_hud(&g_eax, &g_ecx, 0); break;
    /* Minimap hooks: positions read right after (fld [..+68]/[..+1C]). */
    case 0x00590DDFu: hud_mem_pair(g_ecx + 0x68, g_ecx + 0x6C, 1); break;
    case 0x0058FB1Cu: hud_mem_pair(g_eax + 0x68, g_eax + 0x6C, 1); break;
    case 0x0058E5B2u: hud_mem_pair(g_eax + 0x1C, g_eax + 0x20, 1); break;
    /* LapsHook: before `fadd [ebx+1C]`. */
    case 0x004F6DABu: hud_mem_pair(g_ebx + 0x1C, g_ebx + 0x20, 0); break;
    /* FMVWidescreenMode 1 (cropped 16:9): the four pushed quad extents
     * (left, top, right, bottom on the stack before the last push). */
    case 0x004F1FD7u: case 0x004F225Cu: case 0x004F25D1u: {
        const float v = 0.5f / ((4.0f / 3.0f) / (16.0f / 9.0f));
        wrf(g_esp + 0x0, -v); wrf(g_esp + 0x4, -v); wrf(g_esp + 0x8, v); wrf(g_esp + 0xC, v);
        break;
    }
    /* FixFOV: ST0 was viewport height/width; the mod loads a per-view scale
     * (rear-view mirror 3: 0.45) and picks the constants of the three later
     * reads by view id ([ebp+4]; 1 and 4: headlights and reflections). */
    case 0x0040DDE0u: {
        uint32_t view = MEM32(g_ebp + 4);
        if (view == 1 || view == 4) {
            s_fov_scale[0] = 1.0 / (s_aspect / (4.0 / 3.0)); s_fov_scale[1] = 0.4525; s_fov_scale[2] = 1.28;
        } else {
            s_fov_scale[0] = 1.0; s_fov_scale[1] = 0.5; s_fov_scale[2] = 1.0;
        }
        g_st[0] = view == 3 ? 0.45 : 1.0;
        MEM32(g_esp + 0x20) = 0x3F800000u;   /* 1.0f, as the mod stores */
        break;
    }
    /* `fmul [0.5]` (0x40DE43), `fdivr [1.0]` (0x40DEBB, 0x40DEEC): the mod
     * repoints those operands; rescale the results by new/original. */
    case 0x0040DE49u: g_st[0] *= s_fov_scale[1] / 0.5; break;
    case 0x0040DEC1u: g_st[0] *= s_fov_scale[0]; break;
    case 0x0040DEF2u: g_st[0] *= s_fov_scale[2]; break;
    default: {
        char why[96];
        snprintf(why, sizeof why, "RECOMP_HOOK at 0x%08X has no NFSU1 handler", (unsigned)va);
#ifdef _WIN32
        nfs_report(why, 0);
        ExitProcess(3);
#else
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_FATAL, "NFSU1", "%s", why);
#endif
        fprintf(stderr, "%s\n", why);
        abort();
#endif
    }
    }
}
