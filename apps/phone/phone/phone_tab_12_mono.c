/*******************************************************************************
 * Size: 12 px
 * Bpp: 1
 * Opts: --no-compress --no-prefilter --force-fast-kern-format --bpp 1 --size 12 --format lvgl --lv-include lvgl.h --lv-font-name phone_tab_12_mono --font /usr/share/fonts/truetype/dejavu/DejaVuSansCondensed.ttf --symbols Favourites Recents Contacts Keypad -o phone_tab_12_mono.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef PHONE_TAB_12_MONO
#define PHONE_TAB_12_MONO 1
#endif

#if PHONE_TAB_12_MONO

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0043 "C" */
    0x3c, 0x86, 0x4, 0x8, 0x10, 0x20, 0x21, 0x3c,

    /* U+0046 "F" */
    0xf8, 0x88, 0xf8, 0x88, 0x80,

    /* U+004B "K" */
    0x8a, 0x6b, 0x38, 0xc2, 0x89, 0x26, 0x8c,

    /* U+0052 "R" */
    0xf2, 0x28, 0xa2, 0xf2, 0x48, 0xa2, 0x84,

    /* U+0061 "a" */
    0xf4, 0x42, 0xf8, 0xc5, 0xe0,

    /* U+0063 "c" */
    0x7e, 0x61, 0x8, 0x65, 0xe0,

    /* U+0064 "d" */
    0x8, 0x42, 0xdd, 0xc6, 0x31, 0xdb, 0x40,

    /* U+0065 "e" */
    0x76, 0x63, 0xf8, 0x61, 0xe0,

    /* U+0069 "i" */
    0x9f, 0xc0,

    /* U+006E "n" */
    0xb6, 0x63, 0x18, 0xc6, 0x20,

    /* U+006F "o" */
    0x76, 0xe3, 0x18, 0xed, 0xc0,

    /* U+0070 "p" */
    0xb6, 0xe3, 0x18, 0xee, 0xd0, 0x84, 0x0,

    /* U+0072 "r" */
    0xba, 0x49, 0x20,

    /* U+0073 "s" */
    0x78, 0x86, 0x11, 0xe0,

    /* U+0074 "t" */
    0x44, 0xf4, 0x44, 0x44, 0x70,

    /* U+0075 "u" */
    0x8c, 0x63, 0x18, 0xcd, 0xa0,

    /* U+0076 "v" */
    0xc5, 0x14, 0x92, 0x28, 0xc3, 0x0,

    /* U+0079 "y" */
    0x45, 0x14, 0x92, 0x28, 0xc1, 0x4, 0x21, 0x80
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 55, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 121, .box_w = 7, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 9, .adv_w = 99, .box_w = 4, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 14, .adv_w = 113, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 21, .adv_w = 120, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 28, .adv_w = 106, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 33, .adv_w = 95, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 38, .adv_w = 110, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 45, .adv_w = 106, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 50, .adv_w = 48, .box_w = 1, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 52, .adv_w = 110, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 57, .adv_w = 106, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 62, .adv_w = 110, .box_w = 5, .box_h = 10, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 69, .adv_w = 71, .box_w = 3, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 72, .adv_w = 90, .box_w = 4, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 76, .adv_w = 68, .box_w = 4, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 81, .adv_w = 110, .box_w = 5, .box_h = 7, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 86, .adv_w = 102, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 92, .adv_w = 102, .box_w = 6, .box_h = 10, .ofs_x = 0, .ofs_y = -3}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x23, 0x26, 0x2b, 0x32, 0x41, 0x43, 0x44,
    0x45, 0x49, 0x4e, 0x4f, 0x50, 0x52, 0x53, 0x54,
    0x55, 0x56, 0x59
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 90, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 19, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
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
    .bpp = 1,
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
const lv_font_t phone_tab_12_mono = {
#else
lv_font_t phone_tab_12_mono = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 13,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 0,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if PHONE_TAB_12_MONO*/

