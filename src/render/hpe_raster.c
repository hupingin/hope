#include "render/hpe_raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"

static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t alpha)
{
    uint32_t inv, dr, dg, db, sr, sg, sb, r, g, b;
    if (alpha == 0)
        return dst;
    if (alpha >= 255)
        return (src & 0x00FFFFFFu) | 0xFF000000u;
    inv = 255u - alpha;
    dr = (dst >> 16) & 0xFFu;
    dg = (dst >> 8) & 0xFFu;
    db = dst & 0xFFu;
    sr = (src >> 16) & 0xFFu;
    sg = (src >> 8) & 0xFFu;
    sb = src & 0xFFu;
    r = (sr * alpha + dr * inv) / 255u;
    g = (sg * alpha + dg * inv) / 255u;
    b = (sb * alpha + db * inv) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void hpe_fill_rect(HpePixmap *pm, HpeRect rect, HpeColor color)
{
    int x0, y0, x1, y1;
    if (!pm || !pm->pixels || color.a == 0)
        return;
    x0 = (int)floorf(rect.x);
    y0 = (int)floorf(rect.y);
    x1 = (int)ceilf(rect.x + rect.w);
    y1 = (int)ceilf(rect.y + rect.h);
    hpe_fill_rect_i(pm, x0, y0, x1 - x0, y1 - y0, color);
}

void hpe_fill_rect_i(HpePixmap *pm, int x, int y, int w, int h, HpeColor color)
{
    int row, col;
    uint32_t src;

    if (!pm || !pm->pixels || color.a == 0 || w <= 0 || h <= 0)
        return;

    /* clamp to the damage-rect clip: moving an edge inward must shrink the
     * extent, never grow it (a rect crossing the clip's top/left edge). */
    if (x < pm->clip_x0) {
        w -= pm->clip_x0 - x;
        x = pm->clip_x0;
    }
    if (y < pm->clip_y0) {
        h -= pm->clip_y0 - y;
        y = pm->clip_y0;
    }
    if (x + w > pm->clip_x1)
        w = pm->clip_x1 - x;
    if (y + h > pm->clip_y1)
        h = pm->clip_y1 - y;
    if (w <= 0 || h <= 0)
        return;

    src = hpe_argb(color);
    if (color.a == 255) {
        uint32_t *row_ptr;
        int i;
        for (row = y; row < y + h; row++) {
            row_ptr = pm->pixels + (size_t)row * (size_t)pm->width + x;
            for (i = 0; i < w; i++)
                row_ptr[i] = src;
        }
        return;
    }

    for (row = y; row < y + h; row++) {
        uint32_t *row_ptr = pm->pixels + (size_t)row * (size_t)pm->width + x;
        for (col = 0; col < w; col++)
            row_ptr[col] = blend(row_ptr[col], src, color.a);
    }
}

void hpe_draw_text_mask(HpePixmap *pm, int x, int y, const HpeTextMask *mask, HpeColor color)
{
    int row, col;
    int top;
    uint32_t src;

    if (!pm || !pm->pixels || !mask || !mask->coverage || color.a == 0)
        return;

    top = y - mask->baseline;
    src = hpe_argb(color);

    for (row = 0; row < mask->height; row++) {
        int dy = top + row;
        const unsigned char *cov_row;
        uint32_t *dst_row;
        if (dy < pm->clip_y0 || dy >= pm->clip_y1)
            continue;
        cov_row = mask->coverage + (size_t)row * (size_t)mask->width;
        dst_row = pm->pixels + (size_t)dy * (size_t)pm->width;
        for (col = 0; col < mask->width; col++) {
            int dx = x + col;
            uint32_t a;
            if (dx < pm->clip_x0 || dx >= pm->clip_x1)
                continue;
            if (!cov_row[col])
                continue;
            a = ((uint32_t)cov_row[col] * color.a) / 255u;
            dst_row[dx] = blend(dst_row[dx], src, a);
        }
    }
}

/* ---------------- anti-aliased rounded rectangles ----------------
 * Coverage comes from the signed distance to a rounded box: the corners are the
 * only place where the distance is not axis aligned, which keeps this cheap
 * enough to use for every button and badge. */

static void blend_px_pm(HpePixmap *pm, int x, int y, HpeColor c, float cov)
{
    uint32_t *dst, d;
    float a, ia, dr, dg, db;

    if (!pm || !pm->pixels)
        return;
    if (x < pm->clip_x0 || x >= pm->clip_x1 || y < pm->clip_y0 || y >= pm->clip_y1)
        return;
    if (x < 0 || y < 0 || x >= pm->width || y >= pm->height)
        return;
    if (cov <= 0.0f)
        return;
    if (cov > 1.0f)
        cov = 1.0f;
    a = cov * ((float)c.a / 255.0f);
    if (a <= 0.0f)
        return;
    dst = &pm->pixels[(size_t)y * (size_t)pm->width + (size_t)x];
    d = *dst;
    dr = (float)((d >> 16) & 0xFFu);
    dg = (float)((d >> 8) & 0xFFu);
    db = (float)(d & 0xFFu);
    ia = 1.0f - a;
    *dst = 0xFF000000u |
           ((uint32_t)((float)c.r * a + dr * ia + 0.5f) << 16) |
           ((uint32_t)((float)c.g * a + dg * ia + 0.5f) << 8) |
           (uint32_t)((float)c.b * a + db * ia + 0.5f);
}

/* distance from a pixel centre to the rounded rect outline (negative inside) */
static float rrect_sdf(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float dx = fabsf(px - cx) - (hw - r);
    float dy = fabsf(py - cy) - (hh - r);
    float ax = dx > 0.0f ? dx : 0.0f;
    float ay = dy > 0.0f ? dy : 0.0f;
    return sqrtf(ax * ax + ay * ay) - r;
}

static void rrect_span(HpePixmap *pm, HpeRect rect, float r, float width, HpeColor color,
                       int stroke)
{
    int x0, y0, x1, y1, px, py;
    float cx = rect.x + rect.w * 0.5f, cy = rect.y + rect.h * 0.5f;
    float hw = rect.w * 0.5f, hh = rect.h * 0.5f;

    if (rect.w <= 0.0f || rect.h <= 0.0f)
        return;
    if (r > hw)
        r = hw;
    if (r > hh)
        r = hh;
    x0 = (int)floorf(rect.x) - 1;
    y0 = (int)floorf(rect.y) - 1;
    x1 = (int)ceilf(rect.x + rect.w) + 1;
    y1 = (int)ceilf(rect.y + rect.h) + 1;
    for (py = y0; py < y1; py++) {
        for (px = x0; px < x1; px++) {
            float d = rrect_sdf((float)px + 0.5f, (float)py + 0.5f, cx, cy, hw, hh, r);
            if (stroke) {
                /* the ring spans d in [-width, 0]; distance to that band is
                 * |d + width/2| - width/2 */
                float band = fabsf(d + width * 0.5f) - width * 0.5f;
                blend_px_pm(pm, px, py, color, 0.5f - band);
            } else {
                blend_px_pm(pm, px, py, color, 0.5f - d);
            }
        }
    }
}

void hpe_fill_round_rect(HpePixmap *pm, HpeRect rect, float radius, HpeColor color)
{
    if (radius <= 0.5f) {
        hpe_fill_rect(pm, rect, color);
        return;
    }
    rrect_span(pm, rect, radius, 0.0f, color, 0);
}

void hpe_stroke_round_rect(HpePixmap *pm, HpeRect rect, float radius, float width, HpeColor color)
{
    if (width <= 0.0f)
        return;
    if (radius <= 0.5f) {
        /* square corners: four axis-aligned bands */
        hpe_fill_rect(pm, hpe_rect(rect.x, rect.y, rect.w, width), color);
        hpe_fill_rect(pm, hpe_rect(rect.x, rect.y + rect.h - width, rect.w, width), color);
        hpe_fill_rect(pm, hpe_rect(rect.x, rect.y, width, rect.h), color);
        hpe_fill_rect(pm, hpe_rect(rect.x + rect.w - width, rect.y, width, rect.h), color);
        return;
    }
    rrect_span(pm, rect, radius, width, color, 1);
}

/* ---------------- gradients ----------------
 * A linear gradient is a projection of the pixel onto the gradient axis: for
 * CSS `angle` (0 = to top, clockwise), the direction vector is
 * (sin a, -cos a). The gradient line is sized so the corners are covered
 * (|w*sin a| + |h*cos a|), matching the spec. */

static void stop_sample(const HpeRasterStop *stops, int n, float t, HpeColor *out)
{
    int i;
    float prev_pos, span;
    if (n <= 0) {
        *out = hpe_rgba(0, 0, 0, 0);
        return;
    }
    if (n == 1 || t <= stops[0].pos) {
        *out = stops[0].color;
        return;
    }
    if (t >= stops[n - 1].pos) {
        *out = stops[n - 1].color;
        return;
    }
    for (i = 1; i < n; i++) {
        if (t <= stops[i].pos) {
            float f;
            prev_pos = stops[i - 1].pos;
            span = stops[i].pos - prev_pos;
            f = span > 1e-6f ? (t - prev_pos) / span : 0.0f;
            out->r = (uint8_t)(stops[i - 1].color.r +
                               (stops[i].color.r - stops[i - 1].color.r) * f);
            out->g = (uint8_t)(stops[i - 1].color.g +
                               (stops[i].color.g - stops[i - 1].color.g) * f);
            out->b = (uint8_t)(stops[i - 1].color.b +
                               (stops[i].color.b - stops[i - 1].color.b) * f);
            out->a = (uint8_t)(stops[i - 1].color.a +
                               (stops[i].color.a - stops[i - 1].color.a) * f);
            return;
        }
    }
    *out = stops[n - 1].color;
}

void hpe_sample_stops(const HpeRasterStop *stops, int stop_count, float t, HpeColor *out)
{
    stop_sample(stops, stop_count, t, out);
}

void hpe_fill_linear_gradient(HpePixmap *pm, HpeRect rect, float angle_deg,
                              const HpeRasterStop *stops, int stop_count)
{
    int x0, y0, x1, y1, px, py;
    float rad = angle_deg * 3.14159265f / 180.0f;
    float dx = sinf(rad), dy = -cosf(rad);
    float cx = rect.x + rect.w * 0.5f, cy = rect.y + rect.h * 0.5f;
    float len = fabsf(rect.w * dx) + fabsf(rect.h * dy);
    float t0 = cx * dx + cy * dy - len * 0.5f;

    if (!pm || !pm->pixels || stop_count <= 0 || len <= 0.0f)
        return;
    x0 = (int)floorf(rect.x);
    y0 = (int)floorf(rect.y);
    x1 = (int)ceilf(rect.x + rect.w);
    y1 = (int)ceilf(rect.y + rect.h);
    if (x0 < pm->clip_x0)
        x0 = pm->clip_x0;
    if (y0 < pm->clip_y0)
        y0 = pm->clip_y0;
    if (x1 > pm->clip_x1)
        x1 = pm->clip_x1;
    if (y1 > pm->clip_y1)
        y1 = pm->clip_y1;
    if (x0 >= x1 || y0 >= y1)
        return;

    for (py = y0; py < y1; py++) {
        uint32_t *row = pm->pixels + (size_t)py * (size_t)pm->width;
        for (px = x0; px < x1; px++) {
            float proj = ((float)px + 0.5f) * dx + ((float)py + 0.5f) * dy;
            float t = (proj - t0) / len;
            HpeColor c;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            stop_sample(stops, stop_count, t, &c);
            if (c.a == 0)
                continue;
            row[px] = blend(row[px], hpe_argb(c), c.a);
        }
    }
}

/* gradient clipped to a rounded rect: reuse the coverage of the filled rrect */
/* fills a scratch pixmap with the linear gradient, sized to the rrect bounds */
static void gradient_fill_scratch(HpePixmap *local, float dx, float dy, const HpeRasterStop *stops,
                                  int stop_count)
{
    int w = local->width, h = local->height;
    float cx = (float)w * 0.5f, cy = (float)h * 0.5f;
    float len = fabsf((float)w * dx) + fabsf((float)h * dy);
    float t0 = cx * dx + cy * dy - len * 0.5f;
    int px, py;
    for (py = 0; py < h; py++) {
        uint32_t *row = local->pixels + (size_t)py * (size_t)w;
        for (px = 0; px < w; px++) {
            float proj = ((float)px + 0.5f) * dx + ((float)py + 0.5f) * dy;
            float t = len > 0.0f ? (proj - t0) / len : 0.0f;
            HpeColor c;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            stop_sample(stops, stop_count, t, &c);
            row[px] = hpe_argb(c);
        }
    }
}

/* composites the scratch gradient onto `pm` through the rrect's SDF coverage */
static void gradient_composite_rrect(HpePixmap *pm, const HpePixmap *local, HpeRect rect,
                                     float radius)
{
    int w = local->width, h = local->height;
    float hw = rect.w * 0.5f, hh = rect.h * 0.5f;
    float rcx = rect.x + hw, rcy = rect.y + hh;
    float r = radius;
    int x0, y0, x1, y1, px, py;
    if (r > hw)
        r = hw;
    if (r > hh)
        r = hh;
    x0 = (int)floorf(rect.x) - 1;
    y0 = (int)floorf(rect.y) - 1;
    x1 = (int)ceilf(rect.x + rect.w) + 1;
    y1 = (int)ceilf(rect.y + rect.h) + 1;
    for (py = y0; py < y1; py++) {
        for (px = x0; px < x1; px++) {
            float d = rrect_sdf((float)px + 0.5f, (float)py + 0.5f, rcx, rcy, hw, hh, r);
            float cov = 0.5f - d;
            int sx, sy;
            HpeColor c;
            uint32_t p;
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f)
                cov = 1.0f;
            sx = px - (int)floorf(rect.x) + 1;
            sy = py - (int)floorf(rect.y) + 1;
            if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                continue;
            p = local->pixels[(size_t)sy * (size_t)w + (size_t)sx];
            c.r = (uint8_t)((p >> 16) & 0xFFu);
            c.g = (uint8_t)((p >> 8) & 0xFFu);
            c.b = (uint8_t)(p & 0xFFu);
            c.a = (uint8_t)((p >> 24) & 0xFFu);
            blend_px_pm(pm, px, py, c, cov);
        }
    }
}

/* gradient clipped to a rounded rect: reuse the coverage of the filled rrect */
void hpe_fill_round_rect_gradient(HpePixmap *pm, HpeRect rect, float radius, float angle_deg,
                                  const HpeRasterStop *stops, int stop_count)
{
    HpePixmap local;
    float rad = angle_deg * 3.14159265f / 180.0f;
    float dx = sinf(rad), dy = -cosf(rad);
    int w, h;

    if (!pm || !pm->pixels || stop_count <= 0)
        return;
    if (radius <= 0.5f) {
        hpe_fill_linear_gradient(pm, rect, angle_deg, stops, stop_count);
        return;
    }
    if (rect.w <= 0.0f || rect.h <= 0.0f)
        return;

    /* Paint the gradient into a scratch pixmap the size of the rrect bounds so
     * the rounded coverage math stays in one place, then composite it back with
     * the same SDF coverage the solid fill uses. */
    w = (int)ceilf(rect.w) + 2;
    h = (int)ceilf(rect.h) + 2;
    if (w <= 0 || h <= 0)
        return;
    memset(&local, 0, sizeof(local));
    local.width = w;
    local.height = h;
    local.stride = w * 4;
    local.pixels = (uint32_t *)hpe_calloc((size_t)w * (size_t)h, sizeof(uint32_t));

    gradient_fill_scratch(&local, dx, dy, stops, stop_count);
    gradient_composite_rrect(pm, &local, rect, radius);
    hpe_free(local.pixels);
}

/* ---------------- soft shadow ----------------
 * A gaussian blur would need a separable pass; a few concentric rounded rects
 * with decreasing alpha approximate it well enough for card shadows and keep
 * the inner region fully opaque. */
void hpe_fill_shadow(HpePixmap *pm, HpeRect rect, float radius, float blur, HpeColor color)
{
    int steps, i;
    if (!pm || !pm->pixels || color.a == 0)
        return;
    if (blur < 1.0f) {
        hpe_fill_round_rect(pm, rect, radius, color);
        return;
    }
    steps = (int)(blur * 0.5f);
    if (steps < 1)
        steps = 1;
    if (steps > 24)
        steps = 24;
    /* draw from the outside in, so the opaque core lands on top */
    for (i = steps; i >= 0; i--) {
        float t = (float)i / (float)steps;
        float grow = blur * t;
        float a = (1.0f - t) * (1.0f - t);
        HpeColor c = color;
        HpeRect r = hpe_rect(rect.x - grow, rect.y - grow, rect.w + grow * 2.0f,
                             rect.h + grow * 2.0f);
        c.a = (uint8_t)((float)color.a * a);
        if (i == 0)
            c.a = color.a;
        if (c.a == 0)
            continue;
        hpe_fill_round_rect(pm, r, radius + grow, c);
    }
}
