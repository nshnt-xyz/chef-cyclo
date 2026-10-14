/*******************************************************************************
 * Size: 20 px
 * Bpp: 4
 * Opts: --no-compress --no-prefilter --bpp 4 --format lvgl --lv-include lvgl.h --font .tnum/Barlow-Regular.ttf --size 20 -r 0x61,0x6D,0x70 --lv-font-name chefkit_b400_20 -o chefkit_b400_20.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef CHEFKIT_B400_20
#define CHEFKIT_B400_20 1
#endif

#if CHEFKIT_B400_20

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0061 "a" */
    0x0, 0x4b, 0xef, 0xc5, 0x0, 0x4f, 0xa4, 0x49,
    0xf4, 0x6, 0xa0, 0x0, 0xb, 0xb0, 0x0, 0x0,
    0x0, 0x9d, 0x0, 0x5c, 0xef, 0xff, 0xd0, 0x8f,
    0x83, 0x22, 0xad, 0xf, 0x80, 0x0, 0x9, 0xd0,
    0xf6, 0x0, 0x0, 0xcd, 0xb, 0xd3, 0x12, 0xaf,
    0xd0, 0x19, 0xef, 0xd8, 0x9d,

    /* U+006D "m" */
    0x6f, 0x4c, 0xfe, 0x90, 0x4c, 0xfe, 0x90, 0x6,
    0xfd, 0x42, 0x6f, 0xcd, 0x41, 0x6f, 0x90, 0x6f,
    0x30, 0x0, 0x9f, 0x30, 0x0, 0x8f, 0x6, 0xf0,
    0x0, 0x6, 0xf0, 0x0, 0x5, 0xf1, 0x6f, 0x0,
    0x0, 0x6f, 0x0, 0x0, 0x5f, 0x16, 0xf0, 0x0,
    0x6, 0xf0, 0x0, 0x5, 0xf1, 0x6f, 0x0, 0x0,
    0x6f, 0x0, 0x0, 0x5f, 0x16, 0xf0, 0x0, 0x6,
    0xf0, 0x0, 0x5, 0xf1, 0x6f, 0x0, 0x0, 0x6f,
    0x0, 0x0, 0x5f, 0x16, 0xf0, 0x0, 0x5, 0xf0,
    0x0, 0x4, 0xf1,

    /* U+0070 "p" */
    0x3f, 0x4b, 0xfe, 0x91, 0x4, 0xff, 0x61, 0x3c,
    0xd0, 0x4f, 0x90, 0x0, 0x1f, 0x64, 0xf4, 0x0,
    0x0, 0xca, 0x4f, 0x30, 0x0, 0xb, 0xb4, 0xf3,
    0x0, 0x0, 0xbb, 0x4f, 0x50, 0x0, 0xd, 0xa4,
    0xfa, 0x0, 0x3, 0xf6, 0x4f, 0xf8, 0x46, 0xed,
    0x4, 0xf4, 0xbf, 0xea, 0x10, 0x4f, 0x20, 0x0,
    0x0, 0x4, 0xf2, 0x0, 0x0, 0x0, 0x4f, 0x20,
    0x0, 0x0, 0x3, 0xf2, 0x0, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 164, .box_w = 9, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 45, .adv_w = 265, .box_w = 15, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 120, .adv_w = 177, .box_w = 9, .box_h = 14, .ofs_x = 1, .ofs_y = -4}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0xc, 0xf
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 97, .range_length = 16, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 3, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t chefkit_b400_20 = {
#else
lv_font_t chefkit_b400_20 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 14,          /*The maximum line height required by the font*/
    .base_line = 4,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if CHEFKIT_B400_20*/
