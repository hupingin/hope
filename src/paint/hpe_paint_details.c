/* Hope UI Engine - <details>/<dialog> painting (v0.26), see hpe_paint_details.h */
#include "paint/hpe_paint_details.h"

#include <math.h>

#include "render/hpe_raster.h"

/* triangle geometry: 8px wide, sits in the left gutter before the summary text */
#define MARKER_W 8.0f
#define MARKER_PAD 6.0f

/* the one dialog currently acting as a modal top layer (frame-scoped) */
static HpeDomNode *g_modal_dialog = NULL;

void hpe_paint_set_modal_dialog(HpeDomNode *node)
{
    g_modal_dialog = node;
}

HpeDomNode *hpe_paint_modal_dialog(void)
{
    return g_modal_dialog;
}

/**
 * @brief 逐行画一个朝右或朝下的实心三角 / Fill a right/down pointing solid triangle row by row
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     x 三角外接框左边界 / Triangle bounding-box left edge
 * @param[in]     y 三角外接框顶边界 / Triangle bounding-box top edge
 * @param[in]     open 展开为朝下、收起为朝右 / Down when open, right when closed
 * @param[in]     color 颜色 / Colour
 */
static void fill_triangle(HpePixmap *pm, float x, float y, int open, HpeColor color)
{
    int row, n = (int)MARKER_W;
    for (row = 0; row < n; row++) {
        float t = (float)row / (float)(n - 1); /* 0..1 down the bounding box */
        float span, rx, rw, ry;
        if (open) {
            /* downward triangle: width grows from 0 to full */
            span = MARKER_W * t;
            rx = x + (MARKER_W - span) * 0.5f;
            rw = span;
            ry = y + (float)row;
        } else {
            /* rightward triangle: height shrinks toward the tip */
            span = MARKER_W * (1.0f - t);
            rx = x;
            rw = 1.0f + span;
            ry = y + (float)row;
        }
        hpe_fill_rect(pm, hpe_rect(rx, ry, rw, 1.0f), color);
    }
}

void hpe_paint_summary_marker(HpePixmap *pm, const HpeBox *box, HpeRect bb, int open)
{
    float size, y, x;

    if (!pm || !box)
        return;
    size = box->style.font_size;
    if (size <= 0.0f)
        size = 16.0f;
    /* centre the triangle vertically on the first line's baseline */
    y = bb.y + (size - MARKER_W) * 0.5f;
    /* summary has padding-left:18px; place the marker inside the left gutter,
     * respecting list-style-position when the author overrides it */
    x = bb.x + MARKER_PAD;
    fill_triangle(pm, x, y, open, box->style.color);
}

void hpe_paint_dialog_backdrop(HpePixmap *pm, float y)
{
    HpeRect area;
    if (!pm || !pm->pixels)
        return;
    if (y < 0.0f)
        y = 0.0f;
    area = hpe_rect(0.0f, y, (float)pm->width, (float)pm->height - y);
    if (area.h <= 0.0f)
        return;
    /* Chrome-like ::backdrop: translucent black over the page content */
    hpe_fill_rect(pm, area, hpe_rgba(0, 0, 0, 90));
}
