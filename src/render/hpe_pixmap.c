#include "render/hpe_pixmap.h"

#include <math.h>
#include <string.h>

#include "base/hpe_util.h"

void hpe_pixmap_init(HpePixmap *pm, int width, int height)
{
    if (!pm)
        return;
    pm->width = width > 0 ? width : 0;
    pm->height = height > 0 ? height : 0;
    pm->stride = pm->width * 4;
    pm->pixels = pm->width && pm->height
                     ? (uint32_t *)hpe_calloc((size_t)pm->width * (size_t)pm->height,
                                              sizeof(uint32_t))
                     : NULL;
    hpe_pixmap_reset_clip(pm);
}

void hpe_pixmap_resize(HpePixmap *pm, int width, int height)
{
    if (!pm)
        return;
    if (width == pm->width && height == pm->height)
        return;
    hpe_pixmap_free(pm);
    hpe_pixmap_init(pm, width, height);
}

void hpe_pixmap_free(HpePixmap *pm)
{
    if (!pm)
        return;
    if (pm->pixels)
        hpe_free(pm->pixels);
    pm->pixels = NULL;
    pm->width = pm->height = pm->stride = 0;
}

void hpe_pixmap_clear(HpePixmap *pm, HpeColor color)
{
    uint32_t c;
    size_t count, i;
    if (!pm || !pm->pixels)
        return;
    c = hpe_argb(color);
    count = (size_t)pm->width * (size_t)pm->height;
    for (i = 0; i < count; i++)
        pm->pixels[i] = c;
}

void hpe_pixmap_reset_clip(HpePixmap *pm)
{
    if (!pm)
        return;
    pm->clip_x0 = 0;
    pm->clip_y0 = 0;
    pm->clip_x1 = pm->width;
    pm->clip_y1 = pm->height;
}

void hpe_pixmap_set_clip(HpePixmap *pm, HpeRect rect)
{
    int x0, y0, x1, y1;
    if (!pm)
        return;
    x0 = (int)floorf(rect.x);
    y0 = (int)floorf(rect.y);
    x1 = (int)ceilf(rect.x + rect.w);
    y1 = (int)ceilf(rect.y + rect.h);
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > pm->width)
        x1 = pm->width;
    if (y1 > pm->height)
        y1 = pm->height;
    if (x1 < x0)
        x1 = x0;
    if (y1 < y0)
        y1 = y0;
    pm->clip_x0 = x0;
    pm->clip_y0 = y0;
    pm->clip_x1 = x1;
    pm->clip_y1 = y1;
}

void hpe_pixmap_intersect_clip(HpePixmap *pm, HpeRect rect)
{
    int x0, y0, x1, y1;
    if (!pm)
        return;
    x0 = (int)floorf(rect.x);
    y0 = (int)floorf(rect.y);
    x1 = (int)ceilf(rect.x + rect.w);
    y1 = (int)ceilf(rect.y + rect.h);
    if (x0 > pm->clip_x0)
        pm->clip_x0 = x0;
    if (y0 > pm->clip_y0)
        pm->clip_y0 = y0;
    if (x1 < pm->clip_x1)
        pm->clip_x1 = x1;
    if (y1 < pm->clip_y1)
        pm->clip_y1 = y1;
    if (pm->clip_x1 < pm->clip_x0)
        pm->clip_x1 = pm->clip_x0;
    if (pm->clip_y1 < pm->clip_y0)
        pm->clip_y1 = pm->clip_y0;
}

int hpe_pixmap_shift_v(HpePixmap *pm, int dy)
{
    if (!pm)
        return 0;
    return hpe_pixmap_shift_v_rect(pm, dy, 0, pm->height);
}

int hpe_pixmap_shift_v_rect(HpePixmap *pm, int dy, int y0, int y1)
{
    size_t row_bytes;
    int rows;
    unsigned char *base;

    if (!pm || !pm->pixels || dy == 0)
        return 0;
    if (y0 < 0)
        y0 = 0;
    if (y1 > pm->height)
        y1 = pm->height;
    rows = y1 - y0;
    if (rows <= 0)
        return 0;
    if (dy > rows)
        dy = rows;
    if (dy < -rows)
        dy = -rows;
    row_bytes = (size_t)pm->stride;
    base = (unsigned char *)pm->pixels + (size_t)y0 * row_bytes;
    if (dy > 0)
        memmove(base, base + (size_t)dy * row_bytes, ((size_t)rows - (size_t)dy) * row_bytes);
    else
        memmove(base + (size_t)(-dy) * row_bytes, base, ((size_t)rows + (size_t)dy) * row_bytes);
    return dy;
}
