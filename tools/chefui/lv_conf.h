/*
 * lv_conf.h - LVGL v9.6.0 configuration for chefui (device and host).
 *
 * Only the settings chefui depends on are listed; everything else takes
 * LVGL's own default from lv_conf_internal.h. The values follow the
 * prototype (docs/research/lvgl-fbdev-prototype.md and the config delta
 * in logs/lvgl-proto-2026-10-03-lv_conf-delta.txt), with lv_evdev and
 * the stock Linux fbdev driver off: chefui has its own fbdev display and
 * multitouch reader. The host build (CHEFUI_SDL=1) adds LVGL's SDL2
 * driver; NEON blending is used only when compiling for aarch64.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* XRGB8888 rendering (the shadow's format; set per display as well). */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_XRGB8888

/* C library allocator, string and printf functions (musl / glibc). */
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* Single-threaded: rendering happens inside lv_timer_handler(). */
#define LV_USE_OS LV_OS_NONE

/* 60 fps target; actual throughput depends on rendering/copy/pan. */
#define LV_DEF_REFR_PERIOD 16

/* The shadow buffer is 64-byte aligned (cache line). */
#define LV_DRAW_BUF_ALIGN 64

#if defined(__aarch64__)
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NEON
#else
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#endif

/* Multi-touch gestures (pinch, rotate, two-finger swipe), fed from
 * chefui's own slot reader; they need float support. */
#define LV_USE_FLOAT 1
#define LV_USE_GESTURE_RECOGNITION 1

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_USE_ASSERT_NULL   1
#define LV_USE_ASSERT_MALLOC 1
/* A failed assert must kill the process, not spin (LVGL's default is
 * while(1)): death releases /run/fb0.lock, so fblog takes the screen back
 * and honours the screen-off flag. */
#define LV_ASSERT_HANDLER __builtin_abort();

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_28

/* chefui's own drivers replace these. */
#define LV_USE_EVDEV 0
#define LV_USE_LINUX_FBDEV 0
#define LV_USE_LINUX_DRM 0

#if defined(CHEFUI_SDL) && CHEFUI_SDL
#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH "SDL2/SDL.h"
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT 1
#define LV_SDL_ACCELERATED 1
#define LV_SDL_FULLSCREEN 0
#define LV_SDL_DIRECT_EXIT 0	/* closing the window goes through chefui's exit path */
#else
#define LV_USE_SDL 0
#endif

#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif /* LV_CONF_H */
