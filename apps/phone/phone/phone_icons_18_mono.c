/*******************************************************************************
 * Size: 18 px
 * Bpp: 1
 * Drawn by apps/phone/tools/gen_tab_icons.py: edit that, not this.
 ******************************************************************************/

#include "lvgl.h"

static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* star */
    /* |                  | */
    /* |                  | */
    /* |        ##        | */
    /* |        ##        | */
    /* |       ####       | */
    /* |       ####       | */
    /* |      ######      | */
    /* | ################ | */
    /* |  ##############  | */
    /* |   ############   | */
    /* |    ##########    | */
    /* |     ########     | */
    /* |     ########     | */
    /* |     ########     | */
    /* |    ####  ####    | */
    /* |    ###    ###    | */
    /* |    #        #    | */
    /* |                  | */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x03, 0x00, 0x01, 0xe0, 0x00,
    0x78, 0x00, 0x3f, 0x01, 0xff, 0xfe, 0x3f, 0xff, 0x07, 0xff, 0x80, 0xff,
    0xc0, 0x1f, 0xe0, 0x07, 0xf8, 0x01, 0xfe, 0x00, 0xf3, 0xc0, 0x38, 0x70,
    0x08, 0x04, 0x00, 0x00, 0x00,
    /* clock */
    /* |        ##        | */
    /* |     ########     | */
    /* |   ####    ####   | */
    /* |  ###        ###  | */
    /* |  ##    ##    ##  | */
    /* | ##     ##     ## | */
    /* | ##     ##     ## | */
    /* | #      ##      # | */
    /* |##      #####   ##| */
    /* |##      #####   ##| */
    /* | #              # | */
    /* | ##            ## | */
    /* | ##            ## | */
    /* |  ##          ##  | */
    /* |  ###        ###  | */
    /* |   ####    ####   | */
    /* |     ########     | */
    /* |        ##        | */
    0x00, 0xc0, 0x01, 0xfe, 0x01, 0xe1, 0xe0, 0xe0, 0x1c, 0x30, 0xc3, 0x18,
    0x30, 0x66, 0x0c, 0x19, 0x03, 0x02, 0xc0, 0xf8, 0xf0, 0x3e, 0x34, 0x00,
    0x09, 0x80, 0x06, 0x60, 0x01, 0x8c, 0x00, 0xc3, 0x80, 0x70, 0x78, 0x78,
    0x07, 0xf8, 0x00, 0x30, 0x00,
    /* person */
    /* |                  | */
    /* |        ##        | */
    /* |      ######      | */
    /* |     ########     | */
    /* |     ########     | */
    /* |     ########     | */
    /* |     ########     | */
    /* |      ######      | */
    /* |       ####       | */
    /* |                  | */
    /* |                  | */
    /* |      ######      | */
    /* |    ##########    | */
    /* |   ############   | */
    /* |  ##############  | */
    /* | ################ | */
    /* | ################ | */
    /* | ################ | */
    0x00, 0x00, 0x00, 0x30, 0x00, 0x3f, 0x00, 0x1f, 0xe0, 0x07, 0xf8, 0x01,
    0xfe, 0x00, 0x7f, 0x80, 0x0f, 0xc0, 0x01, 0xe0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0f, 0xc0, 0x0f, 0xfc, 0x07, 0xff, 0x83, 0xff, 0xf1, 0xff, 0xfe,
    0x7f, 0xff, 0x9f, 0xff, 0xe0,
    /* keypad */
    /* | ###   ###   ### | */
    /* | ###   ###   ### | */
    /* | ###   ###   ### | */
    /* |                 | */
    /* |                 | */
    /* | ###   ###   ### | */
    /* | ###   ###   ### | */
    /* | ###   ###   ### | */
    /* |                 | */
    /* |                 | */
    /* | ###   ###   ### | */
    /* | ###   ###   ### | */
    /* | ###   ###   ### | */
    /* |                 | */
    /* |                 | */
    /* |       ###       | */
    /* |       ###       | */
    /* |       ###       | */
    0x71, 0xc7, 0x38, 0xe3, 0x9c, 0x71, 0xc0, 0x00, 0x00, 0x00, 0x03, 0x8e,
    0x39, 0xc7, 0x1c, 0xe3, 0x8e, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x71, 0xce,
    0x38, 0xe7, 0x1c, 0x70, 0x00, 0x00, 0x00, 0x00, 0x03, 0x80, 0x01, 0xc0,
    0x00, 0xe0, 0x00,
};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 320, .box_w = 18, .box_h = 18, .ofs_x = 1, .ofs_y = 0}, /* star */
    {.bitmap_index = 41, .adv_w = 320, .box_w = 18, .box_h = 18, .ofs_x = 1, .ofs_y = 0}, /* clock */
    {.bitmap_index = 82, .adv_w = 320, .box_w = 18, .box_h = 18, .ofs_x = 1, .ofs_y = 0}, /* person */
    {.bitmap_index = 123, .adv_w = 304, .box_w = 17, .box_h = 18, .ofs_x = 1, .ofs_y = 0}, /* keypad */
};

static const lv_font_fmt_txt_cmap_t cmaps[] = {
    {
        .range_start = 0xE000, .range_length = 4, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,
        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};

static const lv_font_fmt_txt_dsc_t font_dsc = {
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
};

const lv_font_t phone_icons_18_mono = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 18,
    .base_line = 0,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = 0,
    .underline_thickness = 1,
    .dsc = &font_dsc,
    .fallback = NULL,
    .user_data = NULL,
};
