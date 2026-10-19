/*******************************************************************************
 * Size: 14 px
 * Bpp: 1
 * Opts: --no-compress --no-prefilter --force-fast-kern-format --bpp 1 --size 14 --format lvgl --lv-include lvgl.h --lv-font-name home_latin_14_mono --font /home/mohamed/personal/thistle-os/components/apps_builtin/launcher/../../../managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf -r 0xA0-0x17F -o home_latin_14_mono.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef HOME_LATIN_14_MONO
#define HOME_LATIN_14_MONO 1
#endif

#if HOME_LATIN_14_MONO

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+00A0 " " */
    0x0,

    /* U+00A1 "¡" */
    0xf2, 0xaa, 0xa0,

    /* U+00A2 "¢" */
    0x10, 0x21, 0xf6, 0xa9, 0x12, 0x24, 0x6a, 0x7c,
    0x20, 0x40,

    /* U+00A3 "£" */
    0xf, 0x30, 0x20, 0x20, 0xfe, 0x20, 0x20, 0x20,
    0x20, 0xff,

    /* U+00A4 "¤" */
    0x0, 0x17, 0xa2, 0x11, 0x2, 0x40, 0x90, 0x24,
    0x8, 0x84, 0x5e, 0x80, 0x0,

    /* U+00A5 "¥" */
    0xc0, 0xa0, 0x88, 0x86, 0xc1, 0x43, 0xf8, 0x20,
    0xfe, 0x8, 0x4, 0x0,

    /* U+00A6 "¦" */
    0xf8, 0x7c,

    /* U+00A7 "§" */
    0x7e, 0x8, 0x38, 0xfa, 0x18, 0x5f, 0x1c, 0x18,
    0x7e,

    /* U+00A8 "¨" */
    0x90,

    /* U+00A9 "©" */
    0x1e, 0x8, 0x44, 0xa, 0x79, 0xb2, 0x68, 0x1a,
    0x6, 0xc9, 0x5c, 0x88, 0x41, 0xe0,

    /* U+00AA "ª" */
    0xf1, 0xf9, 0xf0,

    /* U+00AB "«" */
    0x6b, 0x4b, 0x34, 0x68,

    /* U+00AC "¬" */
    0xfc, 0x10, 0x41,

    /* U+00AD "­" */
    0xf0,

    /* U+00AE "®" */
    0x1e, 0x8, 0x44, 0xa, 0x79, 0x91, 0x64, 0x59,
    0xe6, 0x49, 0x51, 0x88, 0x41, 0xe0,

    /* U+00AF "¯" */
    0xf0,

    /* U+00B0 "°" */
    0x74, 0x63, 0x17, 0x0,

    /* U+00B1 "±" */
    0x10, 0x41, 0x3f, 0x10, 0x40, 0x0, 0xfc,

    /* U+00B2 "²" */
    0x78, 0x44, 0x4f, 0x80,

    /* U+00B3 "³" */
    0xf8, 0x8c, 0x17, 0x0,

    /* U+00B4 "´" */
    0x24,

    /* U+00B5 "µ" */
    0x83, 0x6, 0xc, 0x18, 0x30, 0x71, 0xfd, 0x81,
    0x2, 0x0,

    /* U+00B6 "¶" */
    0x7f, 0xe7, 0xcf, 0x97, 0x22, 0x44, 0x89, 0x12,
    0x24, 0x48, 0x91, 0x20,

    /* U+00B7 "·" */
    0xf0,

    /* U+00B8 "¸" */
    0x47, 0x80,

    /* U+00B9 "¹" */
    0xe2, 0x22, 0xf0,

    /* U+00BA "º" */
    0x74, 0x63, 0x17, 0x0,

    /* U+00BB "»" */
    0x48, 0xb3, 0x4b, 0x48,

    /* U+00BC "¼" */
    0xe0, 0x41, 0x4, 0x8, 0x20, 0x42, 0x2, 0x22,
    0x79, 0x10, 0x11, 0x1, 0x92, 0x8, 0xf8, 0x80,
    0x84, 0x4,

    /* U+00BD "½" */
    0xe0, 0x41, 0x2, 0x8, 0x20, 0x42, 0x2, 0x10,
    0x11, 0x3b, 0xd8, 0x20, 0x81, 0x8, 0x10, 0x43,
    0x4, 0x3e,

    /* U+00BE "¾" */
    0xf8, 0x20, 0x41, 0x2, 0x8, 0x6, 0x20, 0x89,
    0x11, 0xc8, 0x40, 0x22, 0x1, 0x12, 0x8, 0x7c,
    0x20, 0x21, 0x0, 0x80,

    /* U+00BF "¿" */
    0x30, 0x60, 0x1, 0x83, 0xc, 0x30, 0x40, 0xc6,
    0xf8,

    /* U+00C0 "À" */
    0x10, 0x2, 0x0, 0x40, 0x0, 0xc, 0x3, 0x1,
    0xa0, 0x48, 0x31, 0x8, 0x43, 0xf9, 0x2, 0x40,
    0xf0, 0x10,

    /* U+00C1 "Á" */
    0x3, 0x1, 0x80, 0x80, 0x0, 0xc, 0x3, 0x1,
    0xa0, 0x48, 0x31, 0x8, 0x43, 0xf9, 0x2, 0x40,
    0xf0, 0x10,

    /* U+00C2 "Â" */
    0xc, 0x3, 0x81, 0x30, 0x0, 0xc, 0x3, 0x1,
    0xa0, 0x48, 0x31, 0x8, 0x43, 0xf9, 0x2, 0x40,
    0xf0, 0x10,

    /* U+00C3 "Ã" */
    0x1a, 0x5, 0x80, 0x0, 0x30, 0xe, 0x6, 0x81,
    0x20, 0x44, 0x21, 0xf, 0xe6, 0x9, 0x3, 0xc0,
    0x40,

    /* U+00C4 "Ä" */
    0x12, 0x0, 0x0, 0x0, 0x30, 0x1e, 0x5, 0x83,
    0x20, 0xcc, 0x21, 0x1f, 0xe4, 0xb, 0x3, 0x80,
    0xc0,

    /* U+00C5 "Å" */
    0x4, 0x1, 0x40, 0x28, 0x2, 0x0, 0x0, 0x8,
    0x2, 0x80, 0x50, 0x1b, 0x2, 0x20, 0x44, 0x1f,
    0xc2, 0x8, 0x80, 0x90, 0x10,

    /* U+00C6 "Æ" */
    0x3, 0xfc, 0x14, 0x0, 0x50, 0x2, 0x40, 0x19,
    0xf8, 0x44, 0x3, 0xf0, 0x8, 0x40, 0x41, 0x1,
    0x7, 0xf0,

    /* U+00C7 "Ç" */
    0x1e, 0x30, 0x90, 0x10, 0x8, 0x4, 0x2, 0x0,
    0x80, 0x61, 0xf, 0x2, 0x0, 0x81, 0xc0,

    /* U+00C8 "È" */
    0x40, 0x40, 0x40, 0xf, 0xf0, 0x20, 0x40, 0xfd,
    0x2, 0x4, 0x8, 0x1f, 0xc0,

    /* U+00C9 "É" */
    0xc, 0x20, 0x80, 0xf, 0xf0, 0x20, 0x40, 0xfd,
    0x2, 0x4, 0x8, 0x1f, 0xc0,

    /* U+00CA "Ê" */
    0x30, 0x71, 0x30, 0xf, 0xf0, 0x20, 0x40, 0xfd,
    0x2, 0x4, 0x8, 0x1f, 0xc0,

    /* U+00CB "Ë" */
    0x28, 0x0, 0x7, 0xe8, 0x10, 0x20, 0x7e, 0x81,
    0x2, 0x4, 0xf, 0xe0,

    /* U+00CC "Ì" */
    0xcc, 0x82, 0x49, 0x24, 0x92, 0x40,

    /* U+00CD "Í" */
    0x7a, 0x9, 0x24, 0x92, 0x49, 0x0,

    /* U+00CE "Î" */
    0x22, 0xa2, 0x2, 0x10, 0x84, 0x21, 0x8, 0x42,
    0x10,

    /* U+00CF "Ï" */
    0xd0, 0x2, 0x22, 0x22, 0x22, 0x22, 0x20,

    /* U+00D0 "Ð" */
    0x7e, 0x10, 0x64, 0x9, 0x1, 0xf8, 0x50, 0x14,
    0x5, 0x2, 0x41, 0x9f, 0x80,

    /* U+00D1 "Ñ" */
    0x34, 0x4c, 0x0, 0x81, 0xc1, 0xe1, 0xb1, 0x99,
    0x99, 0x8d, 0x87, 0x83, 0x81,

    /* U+00D2 "Ò" */
    0x30, 0x2, 0x0, 0x40, 0x0, 0x1e, 0x18, 0x64,
    0xa, 0x1, 0x80, 0x60, 0x18, 0x5, 0x2, 0x61,
    0x87, 0x80,

    /* U+00D3 "Ó" */
    0x3, 0x1, 0x0, 0x80, 0x0, 0x1e, 0x18, 0x64,
    0xa, 0x1, 0x80, 0x60, 0x18, 0x5, 0x2, 0x61,
    0x87, 0x80,

    /* U+00D4 "Ô" */
    0xc, 0x7, 0x83, 0x30, 0x0, 0x1e, 0x18, 0x64,
    0xa, 0x1, 0x80, 0x60, 0x18, 0x5, 0x2, 0x61,
    0x87, 0x80,

    /* U+00D5 "Õ" */
    0x1a, 0x9, 0x80, 0x0, 0x78, 0x61, 0x90, 0x28,
    0x6, 0x1, 0x80, 0x60, 0x14, 0x9, 0x86, 0x1e,
    0x0,

    /* U+00D6 "Ö" */
    0x12, 0x0, 0x0, 0x0, 0x78, 0x61, 0x90, 0x28,
    0x6, 0x1, 0x80, 0x60, 0x14, 0x9, 0x86, 0x1e,
    0x0,

    /* U+00D7 "×" */
    0x49, 0xe3, 0xe, 0x48, 0x0,

    /* U+00D8 "Ø" */
    0x0, 0x40, 0x21, 0xf1, 0x86, 0x42, 0xa1, 0x18,
    0x46, 0x21, 0x90, 0x54, 0x26, 0x19, 0x78, 0x40,
    0x0,

    /* U+00D9 "Ù" */
    0x60, 0x30, 0x8, 0x0, 0x81, 0x81, 0x81, 0x81,
    0x81, 0x81, 0x81, 0x81, 0x42, 0x3c,

    /* U+00DA "Ú" */
    0x6, 0xc, 0x10, 0x0, 0x81, 0x81, 0x81, 0x81,
    0x81, 0x81, 0x81, 0x81, 0x42, 0x3c,

    /* U+00DB "Û" */
    0x18, 0x3c, 0x66, 0x0, 0x81, 0x81, 0x81, 0x81,
    0x81, 0x81, 0x81, 0x81, 0x42, 0x3c,

    /* U+00DC "Ü" */
    0x24, 0x0, 0x0, 0x81, 0x81, 0x81, 0x81, 0x81,
    0x81, 0x81, 0x81, 0x42, 0x3c,

    /* U+00DD "Ý" */
    0x6, 0x6, 0x2, 0x0, 0xc, 0x1a, 0x8, 0x88,
    0x64, 0x14, 0xe, 0x2, 0x1, 0x0, 0x80, 0x40,

    /* U+00DE "Þ" */
    0x80, 0xfc, 0x82, 0x81, 0x81, 0x81, 0x82, 0xfc,
    0x80, 0x80,

    /* U+00DF "ß" */
    0x3c, 0x8e, 0xc, 0x18, 0x73, 0xa1, 0xc1, 0x83,
    0xe, 0x70,

    /* U+00E0 "à" */
    0x60, 0xc0, 0x1e, 0x84, 0x17, 0xe1, 0x86, 0x37,
    0xc0,

    /* U+00E1 "á" */
    0x8, 0xc0, 0x1e, 0x84, 0x17, 0xe1, 0x86, 0x37,
    0xc0,

    /* U+00E2 "â" */
    0x31, 0x20, 0x1e, 0x84, 0x17, 0xe1, 0x86, 0x37,
    0xc0,

    /* U+00E3 "ã" */
    0x69, 0x60, 0x1e, 0x84, 0x17, 0xe1, 0x86, 0x37,
    0xc0,

    /* U+00E4 "ä" */
    0x28, 0x0, 0x1e, 0x84, 0x17, 0xe1, 0x86, 0x37,
    0xc0,

    /* U+00E5 "å" */
    0x38, 0xa3, 0x80, 0x7a, 0x10, 0x5f, 0x86, 0x18,
    0xdf,

    /* U+00E6 "æ" */
    0x79, 0xc8, 0x62, 0x4, 0x17, 0xff, 0x84, 0x8,
    0x60, 0x8e, 0x27, 0x9e,

    /* U+00E7 "ç" */
    0x3c, 0x8e, 0x4, 0x8, 0x10, 0x11, 0x9e, 0x10,
    0x10, 0xe0,

    /* U+00E8 "è" */
    0x20, 0x20, 0x1, 0xc4, 0x50, 0x7f, 0xc0, 0xc0,
    0x88, 0xf0,

    /* U+00E9 "é" */
    0x8, 0x20, 0x1, 0xc4, 0x50, 0x7f, 0xc0, 0xc0,
    0x88, 0xf0,

    /* U+00EA "ê" */
    0x38, 0x58, 0x1, 0xc4, 0x50, 0x7f, 0xc0, 0xc0,
    0x88, 0xf0,

    /* U+00EB "ë" */
    0x24, 0x0, 0x1, 0xc4, 0x58, 0x7f, 0xe0, 0xc0,
    0xc4, 0xf0,

    /* U+00EC "ì" */
    0x44, 0x12, 0x49, 0x24, 0x80,

    /* U+00ED "í" */
    0x50, 0x49, 0x24, 0x92, 0x0,

    /* U+00EE "î" */
    0x54, 0x24, 0x92, 0x49, 0x0,

    /* U+00EF "ï" */
    0xa0, 0x24, 0x92, 0x49, 0x0,

    /* U+00F0 "ð" */
    0x7c, 0xe, 0x73, 0x1, 0x3d, 0x47, 0x83, 0x81,
    0x82, 0x46, 0x3c,

    /* U+00F1 "ñ" */
    0x34, 0x98, 0x5, 0xec, 0x78, 0x60, 0xc1, 0x83,
    0x6, 0x8,

    /* U+00F2 "ò" */
    0x30, 0x18, 0x0, 0x3c, 0x42, 0x81, 0x81, 0x81,
    0x81, 0x42, 0x3c,

    /* U+00F3 "ó" */
    0xc, 0x18, 0x0, 0x3c, 0x42, 0x81, 0x81, 0x81,
    0x81, 0x42, 0x3c,

    /* U+00F4 "ô" */
    0x18, 0x66, 0x0, 0x3c, 0x42, 0x81, 0x81, 0x81,
    0x81, 0x42, 0x3c,

    /* U+00F5 "õ" */
    0x32, 0x4c, 0x0, 0x3c, 0x42, 0x81, 0x81, 0x81,
    0x81, 0x42, 0x3c,

    /* U+00F6 "ö" */
    0x24, 0x0, 0x0, 0x3c, 0x42, 0x81, 0x81, 0x81,
    0x81, 0x42, 0x3c,

    /* U+00F7 "÷" */
    0x10, 0x40, 0x0, 0xfc, 0x1, 0x4,

    /* U+00F8 "ø" */
    0x2, 0x3e, 0x46, 0x89, 0x89, 0x91, 0xa1, 0x62,
    0x7c, 0x80,

    /* U+00F9 "ù" */
    0x60, 0x20, 0x4, 0x18, 0x30, 0x60, 0xc1, 0x83,
    0x8d, 0xe8,

    /* U+00FA "ú" */
    0xc, 0x20, 0x4, 0x18, 0x30, 0x60, 0xc1, 0x83,
    0x8d, 0xe8,

    /* U+00FB "û" */
    0x38, 0xd8, 0x4, 0x18, 0x30, 0x60, 0xc1, 0x83,
    0x8d, 0xe8,

    /* U+00FC "ü" */
    0x28, 0x0, 0x4, 0x18, 0x30, 0x60, 0xc1, 0x83,
    0x8d, 0xe8,

    /* U+00FD "ý" */
    0xc, 0x10, 0x0, 0xc3, 0x42, 0x42, 0x64, 0x24,
    0x3c, 0x18, 0x18, 0x10, 0x10, 0xe0,

    /* U+00FE "þ" */
    0x80, 0x80, 0x80, 0xbc, 0xc2, 0x81, 0x81, 0x81,
    0x81, 0xc2, 0xbc, 0x80, 0x80, 0x80,

    /* U+00FF "ÿ" */
    0x24, 0x0, 0x0, 0xc1, 0x43, 0x62, 0x22, 0x34,
    0x34, 0x18, 0x18, 0x18, 0x90, 0xe0,

    /* U+0100 "Ā" */
    0x1e, 0x0, 0x0, 0x0, 0x30, 0xc, 0x6, 0x81,
    0x20, 0xc4, 0x21, 0xf, 0xe4, 0x9, 0x3, 0xc0,
    0x40,

    /* U+0101 "ā" */
    0x78, 0x7, 0xa1, 0x5, 0xf8, 0x61, 0x8d, 0xf0,

    /* U+0102 "Ă" */
    0x11, 0x4, 0x80, 0xe0, 0x0, 0xc, 0x3, 0x81,
    0xa0, 0x4c, 0x11, 0x8, 0x43, 0xf9, 0x2, 0x40,
    0xf0, 0x10,

    /* U+0103 "ă" */
    0x49, 0xe0, 0x1e, 0x84, 0x17, 0xe1, 0x86, 0x37,
    0xc0,

    /* U+0104 "Ą" */
    0x4, 0x1, 0x40, 0x28, 0xd, 0x81, 0x10, 0x63,
    0xf, 0xe3, 0x6, 0x40, 0x48, 0x8, 0x3, 0x0,
    0x40, 0xe,

    /* U+0105 "ą" */
    0x7a, 0x10, 0x5f, 0x86, 0x18, 0xdf, 0x8, 0x20,
    0xc0,

    /* U+0106 "Ć" */
    0x2, 0x2, 0x2, 0x0, 0x1, 0xe3, 0x9, 0x1,
    0x0, 0x80, 0x40, 0x20, 0x8, 0x6, 0x10, 0xf0,

    /* U+0107 "ć" */
    0x8, 0x20, 0x1, 0xe4, 0x70, 0x20, 0x40, 0x80,
    0x8c, 0xf0,

    /* U+0108 "Ĉ" */
    0x8, 0xa, 0x8, 0x80, 0x1, 0xe3, 0x9, 0x1,
    0x0, 0x80, 0x40, 0x20, 0x8, 0x6, 0x10, 0xf0,

    /* U+0109 "ĉ" */
    0x30, 0xd0, 0x1, 0xe4, 0x70, 0x20, 0x40, 0x80,
    0x8c, 0xf0,

    /* U+010A "Ċ" */
    0x4, 0x2, 0x0, 0x0, 0x1, 0xf3, 0xd, 0x1,
    0x0, 0x80, 0x40, 0x20, 0x8, 0x6, 0x18, 0xf8,

    /* U+010B "ċ" */
    0x10, 0x40, 0xf, 0x46, 0x8, 0x20, 0x81, 0x13,
    0xc0,

    /* U+010C "Č" */
    0x22, 0xe, 0x2, 0x0, 0x1, 0xe3, 0x9, 0x1,
    0x0, 0x80, 0x40, 0x20, 0x8, 0x6, 0x10, 0xf0,

    /* U+010D "č" */
    0x68, 0x60, 0x1, 0xe4, 0x70, 0x20, 0x40, 0x80,
    0x8c, 0xf0,

    /* U+010E "Ď" */
    0x44, 0x14, 0x4, 0x0, 0xf, 0xc4, 0x1a, 0x5,
    0x1, 0x80, 0xc0, 0x60, 0x30, 0x28, 0x37, 0xe0,

    /* U+010F "ď" */
    0x0, 0x40, 0x50, 0x14, 0x4, 0x3d, 0x10, 0xc8,
    0x12, 0x4, 0x81, 0x20, 0x44, 0x30, 0xf4,

    /* U+0110 "Đ" */
    0x7e, 0x10, 0x64, 0x9, 0x1, 0xf8, 0x50, 0x14,
    0x5, 0x2, 0x41, 0x9f, 0x80,

    /* U+0111 "đ" */
    0x1, 0xf, 0xc0, 0x47, 0xa4, 0x34, 0xa, 0x5,
    0x2, 0x81, 0x21, 0x8f, 0x40,

    /* U+0112 "Ē" */
    0x78, 0x0, 0x7, 0xf8, 0x10, 0x20, 0x7e, 0x81,
    0x2, 0x4, 0xf, 0xe0,

    /* U+0113 "ē" */
    0x7c, 0x0, 0xe2, 0x28, 0x3f, 0xe0, 0x60, 0x44,
    0x78,

    /* U+0114 "Ĕ" */
    0x44, 0x90, 0xe0, 0xf, 0xf0, 0x20, 0x40, 0xfd,
    0x2, 0x4, 0x8, 0x1f, 0xc0,

    /* U+0115 "ĕ" */
    0x44, 0x70, 0x1, 0xc4, 0x50, 0x7f, 0xc0, 0xc0,
    0x88, 0xf0,

    /* U+0116 "Ė" */
    0x10, 0x20, 0x7, 0xf8, 0x10, 0x20, 0x7e, 0x81,
    0x2, 0x4, 0xf, 0xe0,

    /* U+0117 "ė" */
    0x10, 0x20, 0x1, 0xc4, 0x50, 0xff, 0xc0, 0xc0,
    0x88, 0xf0,

    /* U+0118 "Ę" */
    0xfd, 0x2, 0x4, 0xf, 0xd0, 0x20, 0x40, 0x81,
    0xfc, 0x10, 0x40, 0xe0,

    /* U+0119 "ę" */
    0x38, 0x8b, 0xf, 0xf8, 0x18, 0x19, 0x1e, 0x8,
    0x10, 0x38,

    /* U+011A "Ě" */
    0x4c, 0x70, 0xc0, 0xf, 0xf0, 0x20, 0x40, 0xfd,
    0x2, 0x4, 0x8, 0x1f, 0xc0,

    /* U+011B "ě" */
    0x28, 0x70, 0x1, 0xc4, 0x50, 0x7f, 0xc0, 0xc0,
    0x88, 0xf0,

    /* U+011C "Ĝ" */
    0xc, 0xf, 0xc, 0x40, 0x1, 0xf3, 0xd, 0x1,
    0x0, 0x80, 0x40, 0x60, 0x28, 0x16, 0x8, 0xf8,

    /* U+011D "ĝ" */
    0x18, 0x26, 0x0, 0x3d, 0x43, 0x81, 0x81, 0x81,
    0x81, 0x43, 0x3d, 0x1, 0x42, 0x7c,

    /* U+011E "Ğ" */
    0x11, 0x9, 0x83, 0x80, 0x1, 0xf3, 0xd, 0x1,
    0x0, 0x80, 0x40, 0x60, 0x28, 0x16, 0x8, 0xf8,

    /* U+011F "ğ" */
    0x22, 0x3c, 0x0, 0x3d, 0x43, 0x81, 0x81, 0x81,
    0x81, 0x43, 0x3d, 0x1, 0x42, 0x7c,

    /* U+0120 "Ġ" */
    0x4, 0x2, 0x0, 0x0, 0x1, 0xf3, 0xd, 0x1,
    0x0, 0x80, 0x40, 0x60, 0x28, 0x16, 0x8, 0xf8,

    /* U+0121 "ġ" */
    0x8, 0x4, 0x0, 0x7, 0xd4, 0x1c, 0x6, 0x3,
    0x1, 0x80, 0xb1, 0xcf, 0xa0, 0x14, 0x13, 0xf0,

    /* U+0122 "Ģ" */
    0x1f, 0x30, 0xd0, 0x10, 0x8, 0x4, 0x6, 0x2,
    0x81, 0x60, 0x8f, 0x80, 0x0, 0x0, 0xc0, 0x40,

    /* U+0123 "ģ" */
    0x8, 0x18, 0x18, 0x0, 0x3d, 0x43, 0x81, 0x81,
    0x81, 0x81, 0x43, 0x3d, 0x1, 0x42, 0x7c,

    /* U+0124 "Ĥ" */
    0x18, 0x3c, 0x66, 0x0, 0x81, 0x81, 0x81, 0x81,
    0xff, 0x81, 0x81, 0x81, 0x81, 0x81,

    /* U+0125 "ĥ" */
    0x30, 0x2c, 0x0, 0x4, 0x2, 0x1, 0x0, 0xbc,
    0x63, 0x20, 0x90, 0x48, 0x24, 0x12, 0x9, 0x4,

    /* U+0126 "Ħ" */
    0x20, 0x4f, 0xff, 0x20, 0x42, 0x4, 0x3f, 0xc2,
    0x4, 0x20, 0x42, 0x4, 0x20, 0x42, 0x4,

    /* U+0127 "ħ" */
    0x20, 0x7e, 0x8, 0x5, 0xe3, 0x19, 0x4, 0x82,
    0x41, 0x20, 0x90, 0x48, 0x20,

    /* U+0128 "Ĩ" */
    0x6c, 0xc0, 0x42, 0x10, 0x84, 0x21, 0x8, 0x42,
    0x0,

    /* U+0129 "ĩ" */
    0x75, 0x80, 0x42, 0x10, 0x84, 0x21, 0x8,

    /* U+012A "Ī" */
    0xe0, 0x24, 0x92, 0x49, 0x24,

    /* U+012B "ī" */
    0xe1, 0x24, 0x92, 0x48,

    /* U+012C "Ĭ" */
    0x8a, 0x5c, 0x2, 0x10, 0x84, 0x21, 0x8, 0x42,
    0x10,

    /* U+012D "ĭ" */
    0xa6, 0x4, 0x44, 0x44, 0x44, 0x40,

    /* U+012E "Į" */
    0x49, 0x24, 0x92, 0x4a, 0x4c,

    /* U+012F "į" */
    0x48, 0x24, 0x92, 0x49, 0x49, 0x80,

    /* U+0130 "İ" */
    0xdf, 0xf8,

    /* U+0131 "ı" */
    0xff,

    /* U+0132 "Ĳ" */
    0x83, 0x6, 0xc, 0x18, 0x30, 0x40, 0x81, 0x85,
    0xf0,

    /* U+0133 "ĳ" */
    0x8c, 0x41, 0x18, 0xc6, 0x31, 0x8c, 0x62, 0x10,
    0xb8,

    /* U+0134 "Ĵ" */
    0x18, 0xe6, 0x40, 0x7c, 0x10, 0x41, 0x4, 0x10,
    0x41, 0x45, 0xe0,

    /* U+0135 "ĵ" */
    0x11, 0x40, 0x21, 0x8, 0x42, 0x10, 0x84, 0x21,
    0x70,

    /* U+0136 "Ķ" */
    0x83, 0x43, 0x23, 0x13, 0xb, 0x7, 0xc3, 0x31,
    0xc, 0x82, 0x41, 0x80, 0x0, 0x1, 0x0, 0x80,

    /* U+0137 "ķ" */
    0x80, 0x80, 0x80, 0x86, 0x8c, 0x98, 0xb0, 0xd8,
    0x8c, 0x84, 0x86, 0x0, 0x0, 0x30, 0x10,

    /* U+0138 "ĸ" */
    0x85, 0x32, 0xc7, 0x8d, 0x13, 0x23, 0x43,

    /* U+0139 "Ĺ" */
    0x63, 0x8, 0x0, 0x82, 0x8, 0x20, 0x82, 0x8,
    0x20, 0x83, 0xf0,

    /* U+013A "ĺ" */
    0x50, 0x49, 0x24, 0x92, 0x49, 0x0,

    /* U+013B "Ļ" */
    0x82, 0x8, 0x20, 0x82, 0x8, 0x20, 0x83, 0xf0,
    0x0, 0x10, 0x40,

    /* U+013C "ļ" */
    0xff, 0xe6,

    /* U+013D "Ľ" */
    0x5, 0xa, 0x14, 0x8, 0x10, 0x20, 0x40, 0x81,
    0x3, 0xf8,

    /* U+013E "ľ" */
    0x19, 0x98, 0x88, 0x88, 0x88, 0x88,

    /* U+013F "Ŀ" */
    0x82, 0x8, 0x22, 0x8a, 0x8, 0x20, 0x83, 0xf0,

    /* U+0140 "ŀ" */
    0x88, 0x88, 0x89, 0x88, 0x88, 0x80,

    /* U+0141 "Ł" */
    0x40, 0x81, 0x43, 0x86, 0x18, 0x10, 0x20, 0x40,
    0xfc,

    /* U+0142 "ł" */
    0x49, 0x26, 0xf2, 0x49, 0x0,

    /* U+0143 "Ń" */
    0x6, 0x8, 0x10, 0x0, 0x81, 0xc1, 0xe1, 0xb1,
    0x99, 0x99, 0x8d, 0x87, 0x83, 0x81,

    /* U+0144 "ń" */
    0xc, 0x20, 0x5, 0xec, 0x70, 0x60, 0xc1, 0x83,
    0x6, 0x8,

    /* U+0145 "Ņ" */
    0x81, 0xc1, 0xe1, 0xb1, 0x99, 0x99, 0x8d, 0x87,
    0x83, 0x81, 0x0, 0x0, 0x18, 0x10,

    /* U+0146 "ņ" */
    0xbd, 0x8e, 0xc, 0x18, 0x30, 0x60, 0xc1, 0x0,
    0x0, 0x40, 0x80,

    /* U+0147 "Ň" */
    0x66, 0x3c, 0x18, 0x0, 0x81, 0xc1, 0xe1, 0xb1,
    0x99, 0x99, 0x8d, 0x87, 0x83, 0x81,

    /* U+0148 "ň" */
    0x6c, 0x70, 0x5, 0xec, 0x70, 0x60, 0xc1, 0x83,
    0x6, 0x8,

    /* U+0149 "ŉ" */
    0x80, 0x40, 0x2b, 0xd7, 0xf3, 0x19, 0x4, 0x82,
    0x41, 0x20, 0x90, 0x40,

    /* U+014A "Ŋ" */
    0x81, 0xc1, 0xe1, 0xb1, 0x99, 0x99, 0x8d, 0x87,
    0x83, 0x81, 0x1, 0x13, 0x1e,

    /* U+014B "ŋ" */
    0xbd, 0x8e, 0xc, 0x18, 0x30, 0x60, 0xc1, 0x2,
    0x4, 0x70,

    /* U+014C "Ō" */
    0x1e, 0x0, 0x0, 0x0, 0x78, 0x61, 0x90, 0x28,
    0x6, 0x1, 0x80, 0x60, 0x14, 0x9, 0x86, 0x1e,
    0x0,

    /* U+014D "ō" */
    0x7e, 0x0, 0x3c, 0x42, 0x81, 0x81, 0x81, 0x81,
    0x42, 0x3c,

    /* U+014E "Ŏ" */
    0x12, 0x4, 0x80, 0xc0, 0x0, 0x1e, 0x18, 0x64,
    0xa, 0x1, 0x80, 0x60, 0x18, 0x5, 0x2, 0x61,
    0x87, 0x80,

    /* U+014F "ŏ" */
    0x66, 0x3c, 0x0, 0x3c, 0x42, 0x81, 0x81, 0x81,
    0x81, 0x42, 0x3c,

    /* U+0150 "Ő" */
    0xd, 0x6, 0x80, 0x0, 0x78, 0x61, 0x90, 0x28,
    0x6, 0x1, 0x80, 0x60, 0x14, 0x9, 0x86, 0x1e,
    0x0,

    /* U+0151 "ő" */
    0x1a, 0x36, 0x0, 0x3c, 0x42, 0x81, 0x81, 0x81,
    0x81, 0x42, 0x3c,

    /* U+0152 "Œ" */
    0x1f, 0xfe, 0xc2, 0x1, 0x4, 0x4, 0x8, 0x8,
    0x1f, 0xd0, 0x20, 0x20, 0x40, 0x20, 0x80, 0x61,
    0x0, 0x3f, 0xfc,

    /* U+0153 "œ" */
    0x3c, 0x71, 0xe, 0x28, 0x10, 0x60, 0x7f, 0x81,
    0x2, 0x6, 0x4, 0x38, 0x8f, 0x1e,

    /* U+0154 "Ŕ" */
    0x6, 0x8, 0x30, 0x0, 0xfc, 0x82, 0x81, 0x81,
    0x81, 0x83, 0xfe, 0x86, 0x82, 0x81,

    /* U+0155 "ŕ" */
    0x34, 0xb, 0xc8, 0x88, 0x88, 0x80,

    /* U+0156 "Ŗ" */
    0xfc, 0x82, 0x81, 0x81, 0x81, 0x83, 0xfe, 0x86,
    0x82, 0x81, 0x0, 0x0, 0x18, 0x10,

    /* U+0157 "ŗ" */
    0xbc, 0x88, 0x88, 0x88, 0x0, 0x88,

    /* U+0158 "Ř" */
    0x46, 0x3c, 0x10, 0x0, 0xfc, 0x82, 0x81, 0x81,
    0x81, 0x83, 0xfe, 0x86, 0x82, 0x81,

    /* U+0159 "ř" */
    0x59, 0x80, 0xb6, 0x21, 0x8, 0x42, 0x10,

    /* U+015A "Ś" */
    0xc, 0x30, 0xc0, 0x7, 0xd8, 0x20, 0x40, 0x70,
    0x78, 0x18, 0x18, 0x6f, 0x80,

    /* U+015B "ś" */
    0x18, 0xc0, 0x1f, 0x82, 0xf, 0xf, 0x6, 0x1f,
    0x80,

    /* U+015C "Ŝ" */
    0x10, 0x51, 0x10, 0x7, 0xd8, 0x20, 0x40, 0x70,
    0x78, 0x18, 0x18, 0x6f, 0x80,

    /* U+015D "ŝ" */
    0x31, 0x20, 0x1f, 0x82, 0xf, 0xf, 0x6, 0x1f,
    0x80,

    /* U+015E "Ş" */
    0x7d, 0x82, 0x4, 0x7, 0x7, 0x81, 0x81, 0x86,
    0xf8, 0x40, 0x43, 0x80,

    /* U+015F "ş" */
    0x7e, 0x8, 0x3c, 0x3c, 0x18, 0x7e, 0x20, 0x47,
    0x0,

    /* U+0160 "Š" */
    0x44, 0x50, 0x40, 0x7, 0xd8, 0x20, 0x40, 0x70,
    0x78, 0x18, 0x18, 0x6f, 0x80,

    /* U+0161 "š" */
    0x48, 0xc0, 0x1f, 0x82, 0xf, 0xf, 0x6, 0x1f,
    0x80,

    /* U+0162 "Ţ" */
    0xff, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8,
    0x8, 0x8, 0x10, 0x8, 0x18,

    /* U+0163 "ţ" */
    0x41, 0xf, 0x90, 0x41, 0x4, 0x10, 0x40, 0xf3,
    0x2, 0x78,

    /* U+0164 "Ť" */
    0x22, 0x14, 0x8, 0x0, 0xff, 0x8, 0x8, 0x8,
    0x8, 0x8, 0x8, 0x8, 0x8, 0x8,

    /* U+0165 "ť" */
    0x9, 0x24, 0x3e, 0x41, 0x4, 0x10, 0x41, 0x3,
    0xc0,

    /* U+0166 "Ŧ" */
    0xff, 0x8, 0x8, 0x8, 0x7e, 0x8, 0x8, 0x8,
    0x8, 0x8,

    /* U+0167 "ŧ" */
    0x41, 0xf, 0x90, 0x43, 0xe4, 0x10, 0x40, 0xf0,

    /* U+0168 "Ũ" */
    0x34, 0x4c, 0x0, 0x81, 0x81, 0x81, 0x81, 0x81,
    0x81, 0x81, 0x81, 0x42, 0x3c,

    /* U+0169 "ũ" */
    0x34, 0x98, 0x4, 0x18, 0x30, 0x60, 0xc1, 0x83,
    0x8d, 0xe8,

    /* U+016A "Ū" */
    0x3c, 0x0, 0x0, 0x81, 0x81, 0x81, 0x81, 0x81,
    0x81, 0x81, 0x81, 0x42, 0x3c,

    /* U+016B "ū" */
    0x7c, 0x2, 0xc, 0x18, 0x30, 0x60, 0xc1, 0xc6,
    0xf4,

    /* U+016C "Ŭ" */
    0x24, 0x24, 0x3c, 0x0, 0x81, 0x81, 0x81, 0x81,
    0x81, 0x81, 0x81, 0x81, 0x42, 0x3c,

    /* U+016D "ŭ" */
    0x44, 0x70, 0x4, 0x18, 0x30, 0x60, 0xc1, 0xc3,
    0x8d, 0xe8,

    /* U+016E "Ů" */
    0x18, 0x24, 0x24, 0x18, 0x0, 0x81, 0x81, 0x81,
    0x81, 0x81, 0x81, 0x81, 0x81, 0x42, 0x3c,

    /* U+016F "ů" */
    0x38, 0x50, 0xe0, 0x8, 0x30, 0x60, 0xc1, 0x83,
    0x7, 0x1b, 0xd0,

    /* U+0170 "Ű" */
    0x1a, 0x34, 0x0, 0x81, 0x81, 0x81, 0x81, 0x81,
    0x81, 0x81, 0x81, 0x42, 0x3c,

    /* U+0171 "ű" */
    0x16, 0x48, 0x4, 0x18, 0x30, 0x60, 0xc1, 0x83,
    0x8d, 0xe8,

    /* U+0172 "Ų" */
    0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81,
    0x42, 0x3c, 0x10, 0x10, 0x1c,

    /* U+0173 "ų" */
    0x83, 0x6, 0xc, 0x18, 0x30, 0x71, 0xbd, 0x4,
    0x10, 0x38,

    /* U+0174 "Ŵ" */
    0x1, 0x80, 0x7, 0x0, 0x19, 0x0, 0x0, 0x4,
    0x18, 0x28, 0x30, 0xd8, 0xe1, 0x91, 0x62, 0x22,
    0x4c, 0x6c, 0x98, 0x51, 0xa0, 0xa1, 0xc1, 0xc3,
    0x81, 0x6, 0x0,

    /* U+0175 "ŵ" */
    0x6, 0x0, 0x48, 0x0, 0x1, 0x84, 0x24, 0x61,
    0x22, 0x99, 0x94, 0x85, 0x24, 0x28, 0xe1, 0xc6,
    0x4, 0x30,

    /* U+0176 "Ŷ" */
    0x8, 0xa, 0x0, 0x18, 0x34, 0x11, 0x10, 0xc8,
    0x28, 0x1c, 0x4, 0x2, 0x1, 0x0, 0x80,

    /* U+0177 "ŷ" */
    0x18, 0x24, 0x0, 0xc3, 0x42, 0x42, 0x64, 0x24,
    0x3c, 0x18, 0x18, 0x10, 0x10, 0xe0,

    /* U+0178 "Ÿ" */
    0x1a, 0x0, 0x0, 0x1, 0x2, 0x61, 0x8c, 0x41,
    0x30, 0x28, 0xe, 0x1, 0x0, 0x40, 0x10, 0x4,
    0x0,

    /* U+0179 "Ź" */
    0x6, 0x8, 0x10, 0x0, 0xff, 0x2, 0x6, 0xc,
    0x18, 0x10, 0x30, 0x60, 0x40, 0xff,

    /* U+017A "ź" */
    0x18, 0xc0, 0x3f, 0xc, 0x61, 0x8, 0x63, 0xf,
    0xc0,

    /* U+017B "Ż" */
    0x8, 0x8, 0x0, 0xff, 0x3, 0x6, 0xc, 0x8,
    0x18, 0x30, 0x60, 0x60, 0xff,

    /* U+017C "ż" */
    0x10, 0x40, 0x3f, 0xc, 0x61, 0xc, 0x63, 0x8f,
    0xc0,

    /* U+017D "Ž" */
    0x24, 0x3c, 0x18, 0x0, 0xff, 0x2, 0x6, 0xc,
    0x18, 0x10, 0x30, 0x60, 0x40, 0xff,

    /* U+017E "ž" */
    0x48, 0xc0, 0x3f, 0xc, 0x61, 0x8, 0x63, 0xf,
    0xc0,

    /* U+017F "ſ" */
    0x3a, 0x11, 0x84, 0x21, 0x8, 0x42, 0x10
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 60, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 60, .box_w = 2, .box_h = 10, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 4, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 14, .adv_w = 145, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 24, .adv_w = 157, .box_w = 10, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 37, .adv_w = 158, .box_w = 9, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 49, .adv_w = 67, .box_w = 1, .box_h = 14, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 51, .adv_w = 112, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 60, .adv_w = 134, .box_w = 4, .box_h = 1, .ofs_x = 2, .ofs_y = 10},
    {.bitmap_index = 61, .adv_w = 180, .box_w = 10, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 75, .adv_w = 90, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 78, .adv_w = 113, .box_w = 6, .box_h = 5, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 82, .adv_w = 130, .box_w = 6, .box_h = 4, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 85, .adv_w = 86, .box_w = 4, .box_h = 1, .ofs_x = 1, .ofs_y = 4},
    {.bitmap_index = 86, .adv_w = 180, .box_w = 10, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 100, .adv_w = 134, .box_w = 4, .box_h = 1, .ofs_x = 2, .ofs_y = 10},
    {.bitmap_index = 101, .adv_w = 94, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 6},
    {.bitmap_index = 105, .adv_w = 130, .box_w = 6, .box_h = 9, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 112, .adv_w = 96, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 116, .adv_w = 96, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 120, .adv_w = 134, .box_w = 4, .box_h = 2, .ofs_x = 3, .ofs_y = 9},
    {.bitmap_index = 121, .adv_w = 153, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 131, .adv_w = 145, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 143, .adv_w = 60, .box_w = 2, .box_h = 2, .ofs_x = 1, .ofs_y = 3},
    {.bitmap_index = 144, .adv_w = 134, .box_w = 3, .box_h = 3, .ofs_x = 3, .ofs_y = -3},
    {.bitmap_index = 146, .adv_w = 96, .box_w = 4, .box_h = 5, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 149, .adv_w = 94, .box_w = 5, .box_h = 5, .ofs_x = 1, .ofs_y = 5},
    {.bitmap_index = 153, .adv_w = 113, .box_w = 6, .box_h = 5, .ofs_x = 0, .ofs_y = 1},
    {.bitmap_index = 157, .adv_w = 232, .box_w = 13, .box_h = 11, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 175, .adv_w = 232, .box_w = 13, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 193, .adv_w = 232, .box_w = 14, .box_h = 11, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 213, .adv_w = 128, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 222, .adv_w = 164, .box_w = 10, .box_h = 14, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 240, .adv_w = 164, .box_w = 10, .box_h = 14, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 258, .adv_w = 164, .box_w = 10, .box_h = 14, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 276, .adv_w = 164, .box_w = 10, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 293, .adv_w = 164, .box_w = 10, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 310, .adv_w = 164, .box_w = 11, .box_h = 15, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 331, .adv_w = 235, .box_w = 14, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 349, .adv_w = 162, .box_w = 9, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 364, .adv_w = 150, .box_w = 7, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 377, .adv_w = 150, .box_w = 7, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 390, .adv_w = 150, .box_w = 7, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 403, .adv_w = 150, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 415, .adv_w = 69, .box_w = 3, .box_h = 14, .ofs_x = -1, .ofs_y = 0},
    {.bitmap_index = 421, .adv_w = 69, .box_w = 3, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 427, .adv_w = 69, .box_w = 5, .box_h = 14, .ofs_x = -1, .ofs_y = 0},
    {.bitmap_index = 436, .adv_w = 69, .box_w = 4, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 443, .adv_w = 187, .box_w = 10, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 456, .adv_w = 182, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 469, .adv_w = 188, .box_w = 10, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 487, .adv_w = 188, .box_w = 10, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 505, .adv_w = 188, .box_w = 10, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 523, .adv_w = 188, .box_w = 10, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 540, .adv_w = 188, .box_w = 10, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 557, .adv_w = 130, .box_w = 6, .box_h = 6, .ofs_x = 1, .ofs_y = 2},
    {.bitmap_index = 562, .adv_w = 188, .box_w = 10, .box_h = 13, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 579, .adv_w = 177, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 593, .adv_w = 177, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 607, .adv_w = 177, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 621, .adv_w = 177, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 634, .adv_w = 145, .box_w = 9, .box_h = 14, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 650, .adv_w = 162, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 660, .adv_w = 151, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 670, .adv_w = 134, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 679, .adv_w = 134, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 688, .adv_w = 134, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 697, .adv_w = 134, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 706, .adv_w = 134, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 715, .adv_w = 134, .box_w = 6, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 724, .adv_w = 222, .box_w = 12, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 736, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 746, .adv_w = 137, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 756, .adv_w = 137, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 766, .adv_w = 137, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 776, .adv_w = 137, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 786, .adv_w = 62, .box_w = 3, .box_h = 11, .ofs_x = -1, .ofs_y = 0},
    {.bitmap_index = 791, .adv_w = 62, .box_w = 3, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 796, .adv_w = 62, .box_w = 3, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 801, .adv_w = 62, .box_w = 3, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 806, .adv_w = 143, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 817, .adv_w = 153, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 827, .adv_w = 142, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 838, .adv_w = 142, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 849, .adv_w = 142, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 860, .adv_w = 142, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 871, .adv_w = 142, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 882, .adv_w = 130, .box_w = 6, .box_h = 8, .ofs_x = 1, .ofs_y = 1},
    {.bitmap_index = 888, .adv_w = 142, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = -1},
    {.bitmap_index = 898, .adv_w = 152, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 908, .adv_w = 152, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 918, .adv_w = 152, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 928, .adv_w = 152, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 938, .adv_w = 125, .box_w = 8, .box_h = 14, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 952, .adv_w = 153, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 966, .adv_w = 125, .box_w = 8, .box_h = 14, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 980, .adv_w = 164, .box_w = 10, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 997, .adv_w = 134, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1005, .adv_w = 164, .box_w = 10, .box_h = 14, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1023, .adv_w = 134, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1032, .adv_w = 164, .box_w = 11, .box_h = 13, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 1050, .adv_w = 134, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1059, .adv_w = 162, .box_w = 9, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1075, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1085, .adv_w = 162, .box_w = 9, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1101, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1111, .adv_w = 162, .box_w = 9, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1127, .adv_w = 128, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1136, .adv_w = 162, .box_w = 9, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1152, .adv_w = 128, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1162, .adv_w = 185, .box_w = 9, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1178, .adv_w = 153, .box_w = 10, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1193, .adv_w = 187, .box_w = 10, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1206, .adv_w = 153, .box_w = 9, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1219, .adv_w = 150, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1231, .adv_w = 137, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1240, .adv_w = 150, .box_w = 7, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1253, .adv_w = 137, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1263, .adv_w = 150, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1275, .adv_w = 137, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1285, .adv_w = 150, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1297, .adv_w = 137, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1307, .adv_w = 150, .box_w = 7, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1320, .adv_w = 137, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1330, .adv_w = 173, .box_w = 9, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1346, .adv_w = 155, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1360, .adv_w = 173, .box_w = 9, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1376, .adv_w = 155, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1390, .adv_w = 173, .box_w = 9, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1406, .adv_w = 155, .box_w = 9, .box_h = 14, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1422, .adv_w = 173, .box_w = 9, .box_h = 14, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 1438, .adv_w = 155, .box_w = 8, .box_h = 15, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1453, .adv_w = 182, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1467, .adv_w = 153, .box_w = 9, .box_h = 14, .ofs_x = -1, .ofs_y = 0},
    {.bitmap_index = 1483, .adv_w = 185, .box_w = 12, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1498, .adv_w = 153, .box_w = 9, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1511, .adv_w = 69, .box_w = 5, .box_h = 13, .ofs_x = -1, .ofs_y = 0},
    {.bitmap_index = 1520, .adv_w = 62, .box_w = 5, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1527, .adv_w = 69, .box_w = 3, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1532, .adv_w = 62, .box_w = 3, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1536, .adv_w = 69, .box_w = 5, .box_h = 14, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1545, .adv_w = 62, .box_w = 4, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1551, .adv_w = 69, .box_w = 3, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1556, .adv_w = 62, .box_w = 3, .box_h = 14, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1562, .adv_w = 69, .box_w = 1, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1564, .adv_w = 62, .box_w = 1, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1565, .adv_w = 161, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1574, .adv_w = 126, .box_w = 5, .box_h = 14, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1583, .adv_w = 115, .box_w = 6, .box_h = 14, .ofs_x = -1, .ofs_y = 0},
    {.bitmap_index = 1594, .adv_w = 64, .box_w = 5, .box_h = 14, .ofs_x = -2, .ofs_y = -3},
    {.bitmap_index = 1603, .adv_w = 161, .box_w = 9, .box_h = 14, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 1619, .adv_w = 138, .box_w = 8, .box_h = 15, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 1634, .adv_w = 136, .box_w = 7, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1641, .adv_w = 133, .box_w = 6, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1652, .adv_w = 62, .box_w = 3, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1658, .adv_w = 133, .box_w = 6, .box_h = 14, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 1669, .adv_w = 62, .box_w = 1, .box_h = 15, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 1671, .adv_w = 133, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1681, .adv_w = 62, .box_w = 4, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1687, .adv_w = 133, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1695, .adv_w = 78, .box_w = 4, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1701, .adv_w = 135, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1710, .adv_w = 66, .box_w = 3, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1715, .adv_w = 182, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1729, .adv_w = 153, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1739, .adv_w = 182, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 1753, .adv_w = 153, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 1764, .adv_w = 182, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1778, .adv_w = 153, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1788, .adv_w = 172, .box_w = 9, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1800, .adv_w = 182, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1813, .adv_w = 153, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 1823, .adv_w = 188, .box_w = 10, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1840, .adv_w = 142, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1850, .adv_w = 188, .box_w = 10, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1868, .adv_w = 142, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1879, .adv_w = 188, .box_w = 10, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1896, .adv_w = 142, .box_w = 8, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1907, .adv_w = 252, .box_w = 15, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1926, .adv_w = 239, .box_w = 14, .box_h = 8, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1940, .adv_w = 163, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1954, .adv_w = 92, .box_w = 4, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1960, .adv_w = 163, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 1974, .adv_w = 92, .box_w = 4, .box_h = 12, .ofs_x = 1, .ofs_y = -4},
    {.bitmap_index = 1980, .adv_w = 163, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 1994, .adv_w = 92, .box_w = 5, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 2001, .adv_w = 139, .box_w = 7, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2014, .adv_w = 112, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2023, .adv_w = 139, .box_w = 7, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2036, .adv_w = 112, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2045, .adv_w = 139, .box_w = 7, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 2057, .adv_w = 112, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 2066, .adv_w = 139, .box_w = 7, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2079, .adv_w = 112, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2088, .adv_w = 131, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 2101, .adv_w = 93, .box_w = 6, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 2111, .adv_w = 131, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2125, .adv_w = 93, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2134, .adv_w = 131, .box_w = 8, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2144, .adv_w = 95, .box_w = 6, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2152, .adv_w = 177, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2165, .adv_w = 152, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2175, .adv_w = 177, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2188, .adv_w = 152, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2197, .adv_w = 177, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2211, .adv_w = 152, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2221, .adv_w = 177, .box_w = 8, .box_h = 15, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2236, .adv_w = 152, .box_w = 7, .box_h = 12, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2247, .adv_w = 177, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2260, .adv_w = 152, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2270, .adv_w = 177, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 2283, .adv_w = 152, .box_w = 7, .box_h = 11, .ofs_x = 1, .ofs_y = -3},
    {.bitmap_index = 2293, .adv_w = 252, .box_w = 15, .box_h = 14, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 2320, .adv_w = 201, .box_w = 13, .box_h = 11, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 2338, .adv_w = 145, .box_w = 9, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 2353, .adv_w = 125, .box_w = 8, .box_h = 14, .ofs_x = 0, .ofs_y = -3},
    {.bitmap_index = 2367, .adv_w = 145, .box_w = 10, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 2384, .adv_w = 147, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2398, .adv_w = 117, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2407, .adv_w = 147, .box_w = 8, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2420, .adv_w = 117, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2429, .adv_w = 147, .box_w = 8, .box_h = 14, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2443, .adv_w = 117, .box_w = 6, .box_h = 11, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 2452, .adv_w = 69, .box_w = 5, .box_h = 11, .ofs_x = 1, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 160, .range_length = 224, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};

/*-----------------
 *    KERNING
 *----------------*/


/*Map glyph_ids to kern left classes*/
static const uint8_t kern_left_class_mapping[] =
{
    0, 0, 1, 2, 3, 4, 5, 0,
    6, 0, 7, 8, 9, 0, 10, 7,
    0, 11, 0, 0, 0, 0, 12, 13,
    10, 0, 0, 8, 10, 0, 0, 0,
    14, 15, 15, 15, 15, 15, 15, 16,
    17, 16, 16, 16, 16, 13, 13, 13,
    13, 7, 13, 7, 7, 7, 7, 7,
    0, 7, 18, 18, 18, 18, 19, 20,
    21, 22, 22, 22, 22, 22, 22, 23,
    2, 23, 23, 23, 23, 12, 12, 24,
    12, 25, 22, 21, 21, 21, 21, 21,
    10, 21, 12, 12, 12, 12, 26, 21,
    26, 15, 22, 15, 22, 15, 27, 17,
    2, 17, 2, 17, 2, 17, 2, 7,
    28, 7, 29, 16, 23, 16, 23, 16,
    23, 16, 30, 16, 23, 0, 12, 0,
    12, 0, 12, 0, 12, 13, 22, 13,
    22, 13, 12, 13, 12, 13, 12, 13,
    31, 13, 12, 18, 12, 18, 12, 32,
    33, 33, 34, 29, 34, 29, 34, 28,
    34, 29, 34, 29, 13, 22, 13, 22,
    13, 22, 22, 13, 22, 7, 21, 7,
    21, 7, 21, 16, 23, 35, 36, 35,
    36, 35, 36, 37, 38, 37, 38, 37,
    38, 37, 38, 39, 40, 39, 41, 39,
    40, 18, 12, 18, 12, 18, 12, 18,
    12, 18, 12, 18, 42, 43, 26, 19,
    26, 19, 44, 45, 44, 45, 44, 45,
    0
};

/*Map glyph_ids to kern right classes*/
static const uint8_t kern_right_class_mapping[] =
{
    0, 0, 1, 2, 3, 4, 5, 0,
    6, 0, 7, 8, 9, 9, 9, 7,
    0, 10, 0, 0, 0, 0, 11, 12,
    9, 0, 0, 8, 13, 0, 0, 0,
    14, 15, 15, 15, 15, 15, 15, 15,
    7, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 7, 7, 7, 7, 7,
    0, 7, 17, 17, 17, 17, 18, 16,
    19, 20, 20, 20, 20, 20, 20, 20,
    2, 2, 2, 2, 2, 21, 11, 22,
    23, 2, 11, 2, 2, 2, 2, 2,
    9, 2, 24, 24, 24, 24, 25, 19,
    25, 15, 20, 15, 20, 15, 20, 7,
    2, 7, 2, 7, 2, 7, 2, 16,
    2, 16, 2, 16, 2, 16, 2, 16,
    2, 16, 2, 16, 2, 7, 2, 7,
    2, 7, 2, 7, 2, 16, 19, 16,
    19, 16, 26, 16, 27, 16, 28, 16,
    29, 16, 11, 16, 29, 30, 31, 16,
    19, 11, 16, 19, 16, 19, 16, 19,
    16, 19, 16, 19, 16, 11, 16, 11,
    16, 11, 11, 16, 11, 7, 2, 7,
    2, 7, 2, 7, 2, 16, 11, 16,
    11, 16, 11, 32, 33, 32, 33, 32,
    33, 32, 33, 34, 35, 34, 35, 34,
    35, 17, 24, 17, 24, 17, 24, 17,
    24, 17, 24, 17, 24, 36, 25, 18,
    25, 18, 37, 38, 37, 38, 37, 38,
    0
};

/*Kern values between classes*/
static const int8_t kern_class_values[] =
{
    0, 0, 0, 0, -4, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, -2,
    0, 0, 0, -2, -1, 3, -2, 4,
    0, 0, 0, -2, 0, 0, -2, -5,
    -2, 0, 0, 0, 0, 0, -2, 0,
    0, 0, 0, 0, 0, 0, 0, -20,
    2, -3, 0, -2, -6, -7, 0, 0,
    0, -2, -3, 6, -7, 0, -4, 3,
    -3, -5, -10, 0, 0, 2, 0, -9,
    -4, 6, 2, -6, -4, 2, 2, 5,
    -4, -4, 6, -2, -9, 0, -4, 0,
    0, -9, 0, 0, 0, 0, 0, 0,
    0, 0, 2, 0, 0, 0, 0, -2,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    -4, 0, 0, 0, 0, 0, 0, 0,
    -5, 0, 0, 0, 0, -9, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 4, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 2, 2, 0, 0,
    0, -4, -2, 0, 0, -7, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -3, 18, 0, 0, -2, 1, -5,
    -2, 0, 0, -4, 0, 2, 0, 0,
    0, 4, -2, 7, 18, 8, 3, -18,
    -21, 0, 0, 4, 0, 0, 18, 18,
    18, 0, 9, 7, 18, 18, 18, -3,
    0, 2, 0, 7, 7, 7, 2, 4,
    0, 0, 0, 0, 0, 0, 0, 0,
    -2, 0, 0, 0, 0, 2, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 2, -5, 2, 0, 0, 1, 0,
    0, -19, -2, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -14, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, -3, -11, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -4, 0, -19, 0, -2, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, -2, 0, 0, 0, 0,
    2, -4, -11, 1, 0, 0, -9, -13,
    0, -20, -2, -6, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 2, -1, 0, 0,
    0, 0, -2, -21, -4, -21, 0, -18,
    3, 4, 3, 0, -3, -9, 0, 0,
    0, 0, 0, -2, -6, 0, 0, 0,
    0, 3, 0, 0, -1, -8, -1, -8,
    0, 0, -2, -2, 0, 0, 0, -2,
    -1, 0, -4, 0, 0, 0, 0, 0,
    0, 0, 0, -2, 0, -2, 0, 0,
    0, -2, -2, 0, 0, 0, 0, 0,
    0, 0, -2, 0, -3, -2, 0, -2,
    -2, -5, 0, 0, 0, -2, -3, 4,
    -1, 2, 0, 2, -1, -3, -4, 0,
    -1, -3, 0, -2, 0, 0, 0, -4,
    0, 0, 0, 0, 0, 0, 0, -1,
    -4, -2, 0, -2, 0, -4, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, -2, -3, 0, 0, -2,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -1, 0, 0, -3,
    0, 0, 0, 0, -9, -13, 0, 0,
    0, -4, -7, 4, -7, 4, -9, 5,
    -4, -18, -9, -3, -2, -1, 0, -15,
    7, 6, 5, -8, -9, 5, 5, 8,
    -5, -2, 5, -3, -15, -2, -4, -5,
    0, -7, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, -6,
    -7, 0, 0, -7, 0, -2, 0, 0,
    0, 0, 0, 0, 0, 0, 0, -9,
    -2, 0, 0, -6, 1, -2, -4, 0,
    0, 0, 0, 0, 0, 0, 0, -4,
    2, -4, 0, -5, 0, -2, -2, 0,
    -4, -8, 0, -1, 0, 0, 0, 0,
    -3, 0, 0, 0, 0, 0, 0, 0,
    0, -18, 0, -9, 0, -2, 0, 1,
    0, 0, 0, 0, 0, -6, 0, -4,
    0, -5, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, -2, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, -4, 2, -3, 0, -4,
    0, -2, 0, 0, 0, -11, 0, -3,
    0, 0, 0, 0, -2, 0, 0, 0,
    0, 0, 0, 0, 0, -22, 0, -4,
    0, -1, 0, 0, 0, 0, 0, 0,
    0, 18, 0, 0, 0, 0, 0, 0,
    0, 0, -3, -11, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -4, 0, -19, 0, -2, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 2,
    0, 4, 0, 0, 0, -2, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, -3,
    0, 0, 0, 0, -2, 9, -3, 8,
    0, 7, 0, -7, -6, 0, 0, -6,
    0, -4, 0, 0, 0, 0, -2, 0,
    0, 0, 0, 0, -3, -2, -2, -9,
    3, 0, 0, -2, 0, 1, 0, 0,
    0, 0, 0, -6, 0, -4, 0, -5,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -2, 0, 0, 0,
    0, 0, 4, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 2, 0, 0, 0,
    0, 0, 0, 0, 15, 0, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 0,
    0, 0, 0, 0, 10, 0, 0, 2,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 3, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -4, 2, -3,
    0, -4, 0, -2, 0, 0, 0, -11,
    0, -3, 0, 0, 0, 0, -2, 0,
    0, 0, 0, 0, 7, 0, 0, -22,
    0, -4, 0, -1, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -3, -11, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 16, -4, 0, -19, 0, -2,
    0, 0, 0, -5, 0, 0, 0, 0,
    -6, 0, -7, -2, -2, -2, -1, 0,
    -6, 0, -2, -11, 0, -3, -2, -2,
    -2, -4, -8, -2, -2, -2, -2, 0,
    -1, -3, -4, -10, -6, -9, 0, -5,
    0, -5, 0, 0, 0, 0, 0, 0,
    -5, 0, -2, 0, 0, 0, 0, 0,
    0, -4, 0, -2, -2, -2, -2, -2,
    -6, -2, -2, -2, -2, 0, 0, 0,
    -4, -6, 0, -2, 0, -3, 0, -2,
    0, 0, 0, 0, -3, -16, -2, -15,
    0, -17, 2, 0, 1, 0, -3, -16,
    0, 1, 0, 0, 0, -2, -9, 0,
    0, 0, 0, 0, 0, -2, 0, -15,
    -1, -12, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 3, 0, 0,
    0, -2, 0, 0, 0, -2, 0, -1,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -2, 0, 0, 0, -2, 1, 0,
    0, 0, 0, -3, 0, 0, 0, 2,
    -5, 7, -2, 7, -2, 7, 1, -8,
    -9, -3, -4, -9, -2, -2, -2, -2,
    -2, 0, 3, -2, -2, -2, -2, 0,
    0, -4, -1, -9, 3, -6, 0, 0,
    0, 0, 0, 0, 0, 0, 0, -4,
    4, -3, 0, 0, 0, -2, 0, 0,
    0, -3, 0, 0, 0, 0, 0, 0,
    -2, 0, 0, 0, 0, 0, 0, 0,
    0, -2, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, -3,
    0, -5, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, -2, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -4, -12, 0, 0,
    0, 0, -2, 7, -7, 9, -3, 7,
    -3, -13, -8, 0, -3, -2, 0, -6,
    3, 8, 4, -3, -7, 5, 7, 6,
    -3, -2, 6, -2, -9, 2, 1, -2,
    0, -2, 0, -4, 0, 0, 0, -2,
    0, -4, -2, -1, 0, -4, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, -2, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -3, 0, 0, 0,
    0, -4, 0, 0, 0, -2, 0, -4,
    -2, -1, 3, -4, 0, 0, 0, 0,
    0, 0, 0, 0, 3, 3, 3, 0,
    -2, 3, 3, 3, 3, 0, 2, 0,
    0, 0, -3, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, -3, -11,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 9, -4, 0, -19,
    0, -2, 0, 0, -7, -12, 0, 0,
    0, 0, -5, 7, -5, 6, -8, 5,
    -3, -16, -8, -3, 0, -5, 0, -12,
    4, 8, 4, -7, -4, 6, 4, 10,
    -5, -3, 7, -3, -13, -2, -1, 0,
    -2, -6, 1, -3, 0, 0, 0, 0,
    -2, 2, -3, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -2, -3, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -2, 0, 0, 0,
    0, -2, 0, 0, 0, 0, 0, 2,
    -3, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    -2, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0
};


/*Collect the kern class' data in one place*/
static const lv_font_fmt_txt_kern_classes_t kern_classes =
{
    .class_pair_values   = kern_class_values,
    .left_class_mapping  = kern_left_class_mapping,
    .right_class_mapping = kern_right_class_mapping,
    .left_class_cnt      = 45,
    .right_class_cnt     = 38,
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
    .kern_dsc = &kern_classes,
    .kern_scale = 16,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 1,
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
const lv_font_t home_latin_14_mono = {
#else
lv_font_t home_latin_14_mono = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 19,          /*The maximum line height required by the font*/
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



#endif /*#if HOME_LATIN_14_MONO*/

