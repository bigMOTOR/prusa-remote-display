// SPDX-FileCopyrightText: Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter)
// SPDX-License-Identifier: OFL-1.1-no-RFN
//
// GENERATED FILE — DO NOT EDIT.
//
// Regenerate with:  ./firmware/tools/generate-fonts.py
//
// Owner-reviewed operational motion (2026-09-06): Owner-reviewed motion unit
//   font.size  24 px
//   weight     Inter-Bold.ttf
//
// Derived from Inter 4.1 (SIL Open Font License 1.1), vendored at
// assets/fonts/inter/ with its licence, copyright notice and full provenance.
// Inter 4.1 declares no Reserved Font Name; see assets/fonts/inter/PROVENANCE.md.
//
// This is a MODIFIED version of Inter within the meaning of the OFL: the `tnum`
// feature has been frozen into the character map (122 codepoints repointed at
// their tabular variants) and the outlines rasterised to fixed-size bitmaps. It is
// named for the production role it serves rather than presented as the official
// Inter font.
//
//   source sha256   288316099b1e0a47a4716d159098005eef7c0066921f34e3200393dbdb01947f
//   codepoints      0x25
//   bpp             4
//   fontTools       4.63.0
//   lv_font_conv    1.5.3
//
// TABULAR FIGURES ARE NOT ASSERTED HERE. Every digit's advance width is compared
// at renderer start-up and reported; see prd::ui::fonts::verify() in
// ui/src/fonts/fonts.cpp. A comment claiming tabular figures would be a claim; a
// start-up check is a measurement.

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef PRD_FONT_MOTION_UNIT_24
#define PRD_FONT_MOTION_UNIT_24 1
#endif

#if PRD_FONT_MOTION_UNIT_24

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0025 "%" */
    0x3, 0xbf, 0xfb, 0x30, 0x0, 0x0, 0x0, 0x7f,
    0xe1, 0x0, 0x2, 0xff, 0xff, 0xff, 0x20, 0x0,
    0x0, 0x2f, 0xf4, 0x0, 0x0, 0xaf, 0xe3, 0x3e,
    0xf9, 0x0, 0x0, 0xd, 0xf9, 0x0, 0x0, 0xc,
    0xfa, 0x0, 0xaf, 0xc0, 0x0, 0x9, 0xfd, 0x0,
    0x0, 0x0, 0xcf, 0xa0, 0xa, 0xfc, 0x0, 0x4,
    0xff, 0x30, 0x0, 0x0, 0xa, 0xfe, 0x33, 0xef,
    0x90, 0x1, 0xef, 0x70, 0x0, 0x0, 0x0, 0x3f,
    0xff, 0xff, 0xf2, 0x0, 0xaf, 0xb0, 0x0, 0x0,
    0x0, 0x0, 0x3b, 0xff, 0xb3, 0x0, 0x6f, 0xe1,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x2f, 0xf5, 0x0, 0x0, 0x10, 0x0, 0x0, 0x0,
    0x0, 0x0, 0xc, 0xfa, 0x0, 0x1a, 0xff, 0xe8,
    0x0, 0x0, 0x0, 0x0, 0x8, 0xfd, 0x0, 0xc,
    0xff, 0xff, 0xf9, 0x0, 0x0, 0x0, 0x3, 0xff,
    0x30, 0x4, 0xff, 0x60, 0x9f, 0xf0, 0x0, 0x0,
    0x0, 0xdf, 0x80, 0x0, 0x6f, 0xf0, 0x4, 0xff,
    0x20, 0x0, 0x0, 0xaf, 0xc0, 0x0, 0x6, 0xff,
    0x0, 0x4f, 0xf2, 0x0, 0x0, 0x5f, 0xf2, 0x0,
    0x0, 0x3f, 0xf7, 0x2a, 0xff, 0x0, 0x0, 0x1e,
    0xf6, 0x0, 0x0, 0x0, 0xbf, 0xff, 0xff, 0x80,
    0x0, 0xb, 0xfb, 0x0, 0x0, 0x0, 0x0, 0x9e,
    0xfd, 0x70, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 390, .box_w = 21, .box_h = 17, .ofs_x = 2, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 37, .range_length = 1, .glyph_id_start = 1,
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
const lv_font_t prd_font_motion_unit_24 = {
#else
lv_font_t prd_font_motion_unit_24 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 17,          /*The maximum line height required by the font*/
    .base_line = 0,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -4,
    .underline_thickness = 2,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if PRD_FONT_MOTION_UNIT_24*/
