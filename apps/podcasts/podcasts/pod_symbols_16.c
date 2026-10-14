/*******************************************************************************
 * Size: 16 px
 * Bpp: 4
 * Opts: --no-compress --no-prefilter --force-fast-kern-format --bpp 4 --size 16 --format lvgl --lv-include lvgl.h --lv-font-name pod_symbols_16 --font /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf -r 0x2605,0x2606 -o pod_symbols_16.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef POD_SYMBOLS_16
#define POD_SYMBOLS_16 1
#endif

#if POD_SYMBOLS_16

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+2605 "★" */
    0x0, 0x0, 0x0, 0x10, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x28, 0x0, 0x0, 0x0, 0x0, 0x0, 0x8,
    0xd0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xdf, 0x20,
    0x0, 0x0, 0x12, 0x22, 0x3f, 0xf8, 0x22, 0x22,
    0x3, 0xdf, 0xff, 0xff, 0xff, 0xff, 0x70, 0x0,
    0xaf, 0xff, 0xff, 0xfd, 0x30, 0x0, 0x0, 0x7f,
    0xff, 0xfc, 0x0, 0x0, 0x0, 0x7, 0xff, 0xff,
    0xd0, 0x0, 0x0, 0x0, 0xcf, 0xc9, 0xff, 0x20,
    0x0, 0x0, 0x1f, 0x80, 0x4, 0xe7, 0x0, 0x0,
    0x5, 0x40, 0x0, 0x1, 0x80, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0,

    /* U+2606 "☆" */
    0x0, 0x0, 0x0, 0x10, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x27, 0x0, 0x0, 0x0, 0x0, 0x0, 0x6,
    0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x75, 0x20,
    0x0, 0x0, 0x0, 0x0, 0x15, 0x6, 0x0, 0x0,
    0x5, 0xb6, 0x66, 0x0, 0x46, 0x69, 0x90, 0x1,
    0x71, 0x0, 0x0, 0x6, 0x40, 0x0, 0x0, 0x72,
    0x0, 0x8, 0x10, 0x0, 0x0, 0x6, 0x10, 0x0,
    0x70, 0x0, 0x0, 0x0, 0x70, 0x78, 0x25, 0x10,
    0x0, 0x0, 0x19, 0x80, 0x5, 0x66, 0x0, 0x0,
    0x5, 0x60, 0x0, 0x2, 0x90, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 230, .box_w = 13, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 85, .adv_w = 230, .box_w = 13, .box_h = 13, .ofs_x = 1, .ofs_y = -1}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 9733, .range_length = 2, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
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
const lv_font_t pod_symbols_16 = {
#else
lv_font_t pod_symbols_16 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 13,          /*The maximum line height required by the font*/
    .base_line = 1,             /*Baseline measured from the bottom of the line*/
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



#endif /*#if POD_SYMBOLS_16*/

