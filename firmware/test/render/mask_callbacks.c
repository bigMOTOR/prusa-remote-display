// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
// Exercise each public mask initializer and the real indirect callback dispatch.
#include "lvgl.h"
#include "src/draw/sw/lv_draw_sw_mask_private.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void exercise(void* param, const char* name) {
    lv_opa_t row[8];
    memset(row, LV_OPA_COVER, sizeof(row));
    void* masks[] = {param, NULL};
    printf("callback=%s\n", name);
    fflush(stdout);
    (void)lv_draw_sw_mask_apply(masks, row, 0, 2, 8);
    lv_draw_sw_mask_free_param(param);
    printf("passed=%s\n", name);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    lv_init();
    const lv_area_t area = {0, 0, 7, 7};
    switch (atoi(argv[1])) {
    case 0: {
        lv_draw_sw_mask_line_param_t p;
        lv_draw_sw_mask_line_points_init(&p, 4, 0, 4, 7, LV_DRAW_SW_MASK_LINE_SIDE_LEFT);
        exercise(&p, "line");
        break;
    }
    case 1: {
        lv_draw_sw_mask_angle_param_t p;
        lv_draw_sw_mask_angle_init(&p, 4, 4, 0, 90);
        exercise(&p, "angle");
        break;
    }
    case 2: {
        lv_draw_sw_mask_radius_param_t p;
        lv_draw_sw_mask_radius_init(&p, &area, 2, false);
        exercise(&p, "radius");
        break;
    }
    case 3: {
        lv_draw_sw_mask_fade_param_t p;
        lv_draw_sw_mask_fade_init(&p, &area, LV_OPA_TRANSP, 0, LV_OPA_COVER, 7);
        exercise(&p, "fade");
        break;
    }
    case 4: {
        lv_opa_t map[64];
        memset(map, 128, sizeof(map));
        lv_draw_sw_mask_map_param_t p;
        lv_draw_sw_mask_map_init(&p, &area, map);
        exercise(&p, "map");
        break;
    }
    default:
        return 2;
    }
    lv_deinit();
    return 0;
}
