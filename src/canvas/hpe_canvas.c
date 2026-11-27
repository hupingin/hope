/* Hope UI Engine - HTML canvas 2D surface (v0.27), see hpe_canvas.h */
#include "canvas/hpe_canvas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"

/* node -> surface registry; surfaces are owned here, not by the DOM node */
typedef struct CanvasEntry {
    HpeDomNode *node;
    HpeCanvas *surface;
} CanvasEntry;

static CanvasEntry *g_entries;
static size_t g_count, g_cap;
static void (*g_invalidate)(void) = NULL;

void hpe_canvas_set_invalidate(void (*fn)(void))
{
    g_invalidate = fn;
}

static void canvas_invalidate(void)
{
    if (g_invalidate)
        g_invalidate();
}

void hpe_canvas_notify(void)
{
    canvas_invalidate();
}

/* ---- registry ---- */

HpeCanvas *hpe_canvas_find(const HpeDomNode *node)
{
    size_t i;
    if (!node)
        return NULL;
    for (i = 0; i < g_count; i++)
        if (g_entries[i].node == node)
            return g_entries[i].surface;
    return NULL;
}

static void canvas_init_state(HpeCanvas *c)
{
    c->fill_style = hpe_rgb(0, 0, 0);
    c->stroke_style = hpe_rgb(0, 0, 0);
    c->line_width = 1.0f;
    c->global_alpha = 1.0f;
    c->a = 1.0f;
    c->d = 1.0f;
}

static int canvas_attr_size(const HpeDomNode *node, const char *name, int fallback)
{
    const char *v = hpe_dom_attr(node, name);
    int n;
    if (!v || !v[0])
        return fallback;
    n = atoi(v);
    if (n < 1)
        n = 1;
    if (n > 4096)
        n = 4096;
    return n;
}

HpeCanvas *hpe_canvas_get(HpeDomNode *node)
{
    HpeCanvas *c;
    int w, h;
    if (!node || !node->tag || strcmp(node->tag, "canvas"))
        return NULL;
    c = hpe_canvas_find(node);
    if (c)
        return c;
    w = canvas_attr_size(node, "width", 300);
    h = canvas_attr_size(node, "height", 150);
    c = (HpeCanvas *)hpe_calloc(1, sizeof(*c));
    canvas_init_state(c);
    hpe_pixmap_init(&c->pm, w, h);
    hpe_pixmap_clear(&c->pm, hpe_rgba(0, 0, 0, 0));
    if (g_count == g_cap) {
        g_entries = (CanvasEntry *)hpe_grow(g_entries, &g_cap,
                                            g_count + 1, sizeof(CanvasEntry));
    }
    g_entries[g_count].node = node;
    g_entries[g_count].surface = c;
    g_count++;
    return c;
}

void hpe_canvas_shutdown(void)
{
    size_t i;
    for (i = 0; i < g_count; i++) {
        hpe_pixmap_free(&g_entries[i].surface->pm);
        hpe_free(g_entries[i].surface);
    }
    hpe_free(g_entries);
    g_entries = NULL;
    g_count = g_cap = 0;
}

void hpe_canvas_clear(HpeCanvas *c)
{
    if (c)
        hpe_pixmap_clear(&c->pm, hpe_rgba(0, 0, 0, 0));
}

/* ---- float helpers ---- */

static float min4f(float a, float b, float c2, float d)
{
    float m = a;
    if (b < m) m = b;
    if (c2 < m) m = c2;
    if (d < m) m = d;
    return m;
}

static float max4f(float a, float b, float c2, float d)
{
    float m = a;
    if (b > m) m = b;
    if (c2 > m) m = c2;
    if (d > m) m = d;
    return m;
}

/* ---- pixel helpers ---- */

static void canvas_put_pixel(HpeCanvas *c, int x, int y, HpeColor col)
{
    uint32_t src, dst, sr, sa, dr, da, out;
    unsigned r, g, b, a;
    if (x < 0 || y < 0 || x >= c->pm.width || y >= c->pm.height)
        return;
    if (col.a == 0)
        return;
    src = ((uint32_t)col.a << 24) | ((uint32_t)col.r << 16) |
          ((uint32_t)col.g << 8) | (uint32_t)col.b;
    dst = c->pm.pixels[(size_t)y * (size_t)c->pm.width + (size_t)x];
    if (col.a == 255 || ((dst >> 24) & 0xff) == 0) {
        c->pm.pixels[(size_t)y * (size_t)c->pm.width + (size_t)x] = src;
        return;
    }
    sa = (src >> 24) & 0xff;
    da = (dst >> 24) & 0xff;
    a = sa + da * (255 - sa) / 255;
    if (a == 0) {
        c->pm.pixels[(size_t)y * (size_t)c->pm.width + (size_t)x] = 0;
        return;
    }
    sr = (src >> 16) & 0xff;
    dr = (dst >> 16) & 0xff;
    r = (sr * sa + dr * da * (255 - sa) / 255) / a;
    sr = (src >> 8) & 0xff;
    dr = (dst >> 8) & 0xff;
    g = (sr * sa + dr * da * (255 - sa) / 255) / a;
    sr = src & 0xff;
    dr = dst & 0xff;
    b = (sr * sa + dr * da * (255 - sa) / 255) / a;
    out = (a << 24) | (r << 16) | (g << 8) | b;
    c->pm.pixels[(size_t)y * (size_t)c->pm.width + (size_t)x] = out;
}

static void canvas_tf(HpeCanvas *c, float x, float y, float *ox, float *oy)
{
    *ox = c->a * x + c->c * y + c->e;
    *oy = c->b * x + c->d * y + c->f;
}

static HpeColor canvas_with_alpha(const HpeCanvas *c, HpeColor col)
{
    if (c->global_alpha < 1.0f) {
        col.a = (uint8_t)(col.a * c->global_alpha);
    }
    return col;
}

/* ---- rectangles ---- */

/* scan-convert a convex quad (four transformed corners) with fill colour */
static void canvas_fill_quad(HpeCanvas *c, float x0, float y0, float x1, float y1,
                             float x2, float y2, float x3, float y3, HpeColor col)
{
    int y, ymin, ymax;
    col = canvas_with_alpha(c, col);
    ymin = (int)floorf(min4f(y0, y1, y2, y3));
    ymax = (int)ceilf(max4f(y0, y1, y2, y3));
    for (y = ymin; y <= ymax; y++) {
        float ty = (float)y + 0.5f;
        float xs[2];
        int nx = 0;
        float ey[4] = {y0, y1, y2, y3};
        int i;
        for (i = 0; i < 4; i++) {
            int j = (i + 1) & 3;
            float ax = i == 0 ? x0 : (i == 1 ? x1 : (i == 2 ? x2 : x3));
            float bx = j == 0 ? x0 : (j == 1 ? x1 : (j == 2 ? x2 : x3));
            if ((ey[i] <= ty && ey[j] > ty) || (ey[j] <= ty && ey[i] > ty)) {
                if (nx < 2)
                    xs[nx++] = ax + (bx - ax) * (ty - ey[i]) / (ey[j] - ey[i]);
            }
        }
        if (nx >= 2) {
            int xa = (int)ceilf(xs[0] < xs[1] ? xs[0] : xs[1]);
            int xb = (int)ceilf(xs[0] < xs[1] ? xs[1] : xs[0]);
            int x;
            for (x = xa; x < xb; x++)
                canvas_put_pixel(c, x, y, col);
        }
    }
}

void hpe_canvas_fill_rect(HpeCanvas *c, float x, float y, float w, float h)
{
    float ax, ay, bx, by, cx2, cy2, dx, dy;
    if (!c || w <= 0.0f || h <= 0.0f)
        return;
    canvas_tf(c, x, y, &ax, &ay);
    canvas_tf(c, x + w, y, &bx, &by);
    canvas_tf(c, x + w, y + h, &cx2, &cy2);
    canvas_tf(c, x, y + h, &dx, &dy);
    canvas_fill_quad(c, ax, ay, bx, by, cx2, cy2, dx, dy, c->fill_style);
}

/* erase a convex quad by writing transparent black directly (bypasses blending) */
static void canvas_erase_quad(HpeCanvas *c, float x0, float y0, float x1, float y1,
                              float x2, float y2, float x3, float y3)
{
    int ymin, ymax, y, x, xa, xb;
    float ey[4] = {y0, y1, y2, y3};
    float ex[4] = {x0, x1, x2, x3};
    ymin = (int)floorf(min4f(y0, y1, y2, y3));
    ymax = (int)ceilf(max4f(y0, y1, y2, y3));
    for (y = ymin; y <= ymax; y++) {
        float ty = (float)y + 0.5f, xs[2];
        int nx = 0, i;
        for (i = 0; i < 4; i++) {
            int j = (i + 1) & 3;
            if ((ey[i] <= ty && ey[j] > ty) || (ey[j] <= ty && ey[i] > ty)) {
                if (nx < 2)
                    xs[nx++] = ex[i] + (ex[j] - ex[i]) * (ty - ey[i]) / (ey[j] - ey[i]);
            }
        }
        if (nx < 2)
            continue;
        xa = (int)ceilf(xs[0] < xs[1] ? xs[0] : xs[1]);
        xb = (int)ceilf(xs[0] < xs[1] ? xs[1] : xs[0]);
        for (x = xa; x < xb; x++)
            if (x >= 0 && y >= 0 && x < c->pm.width && y < c->pm.height)
                c->pm.pixels[(size_t)y * (size_t)c->pm.width + (size_t)x] = 0;
    }
}

void hpe_canvas_clear_rect(HpeCanvas *c, float x, float y, float w, float h)
{
    float ax, ay, bx, by, cx2, cy2, dx, dy;
    if (!c || w <= 0.0f || h <= 0.0f)
        return;
    canvas_tf(c, x, y, &ax, &ay);
    canvas_tf(c, x + w, y, &bx, &by);
    canvas_tf(c, x + w, y + h, &cx2, &cy2);
    canvas_tf(c, x, y + h, &dx, &dy);
    canvas_erase_quad(c, ax, ay, bx, by, cx2, cy2, dx, dy);
}

/* draw a transformed line with a simple disc brush of the given radius */
static void canvas_brush_line(HpeCanvas *c, float x0, float y0, float x1, float y1,
                              HpeColor col)
{
    float ax, ay, bx, by, dx, dy, len, step, t;
    int r = (int)ceilf(c->line_width * 0.5f);
    int ix, iy;
    if (r < 0) r = 0;
    canvas_tf(c, x0, y0, &ax, &ay);
    canvas_tf(c, x1, y1, &bx, &by);
    dx = bx - ax;
    dy = by - ay;
    len = sqrtf(dx * dx + dy * dy);
    step = len > 0.0f ? 0.5f / len : 1.0f;
    col = canvas_with_alpha(c, col);
    for (t = 0.0f; t <= 1.0f; t += step) {
        float px = ax + dx * t, py = ay + dy * t;
        for (iy = -r; iy <= r; iy++)
            for (ix = -r; ix <= r; ix++)
                canvas_put_pixel(c, (int)floorf(px) + ix, (int)floorf(py) + iy, col);
    }
}

void hpe_canvas_stroke_rect(HpeCanvas *c, float x, float y, float w, float h)
{
    if (!c)
        return;
    canvas_brush_line(c, x, y, x + w, y, c->stroke_style);
    canvas_brush_line(c, x + w, y, x + w, y + h, c->stroke_style);
    canvas_brush_line(c, x + w, y + h, x, y + h, c->stroke_style);
    canvas_brush_line(c, x, y + h, x, y, c->stroke_style);
}

/* ---- paths ---- */

void hpe_canvas_begin_path(HpeCanvas *c)
{
    if (c) {
        c->path_count = 0;
        c->closed = 0;
    }
}

void hpe_canvas_close_path(HpeCanvas *c)
{
    if (c && c->path_count > 0)
        c->closed = 1;
}

void hpe_canvas_move_to(HpeCanvas *c, float x, float y)
{
    if (!c || c->path_count >= HPE_CANVAS_PATH_MAX)
        return;
    c->path[c->path_count].x = x;
    c->path[c->path_count].y = y;
    c->path[c->path_count].move = 1;
    c->path_count++;
    c->closed = 0;
}

void hpe_canvas_line_to(HpeCanvas *c, float x, float y)
{
    int i;
    if (!c || c->path_count >= HPE_CANVAS_PATH_MAX)
        return;
    if (c->path_count == 0) {
        hpe_canvas_move_to(c, x, y);
        return;
    }
    i = c->path_count;
    c->path[i].x = x;
    c->path[i].y = y;
    c->path[i].move = 0;
    c->path_count++;
}

void hpe_canvas_arc(HpeCanvas *c, float x, float y, float r, float a0, float a1, int ccw)
{
    float sweep, steps, t, px, py, ang;
    int i, n;
    if (!c || r <= 0.0f)
        return;
    sweep = a1 - a0;
    if (!ccw) {
        while (sweep <= 0.0f) sweep += 2.0f * (float)M_PI;
    } else {
        while (sweep >= 0.0f) sweep -= 2.0f * (float)M_PI;
    }
    steps = fabsf(sweep) * r * 0.5f;
    n = (int)steps;
    if (n < 3) n = 3;
    if (n > 128) n = 128;
    for (i = 0; i <= n; i++) {
        t = (float)i / (float)n;
        ang = a0 + sweep * t;
        px = x + r * cosf(ang);
        py = y + r * sinf(ang);
        if (i == 0 && c->path_count == 0)
            hpe_canvas_move_to(c, px, py);
        else
            hpe_canvas_line_to(c, px, py);
    }
}

/* even-odd scanline fill of a subpath delimited by [start,end) */
static void canvas_fill_subpath(HpeCanvas *c, int start, int end, HpeColor col)
{
    int y, ymin = 0x7fffffff, ymax = -0x7fffffff;
    float miny, maxy;
    int i;
    col = canvas_with_alpha(c, col);
    if (end - start < 3)
        return;
    /* transform points into a small local array */
    {
        float tx[HPE_CANVAS_PATH_MAX], ty[HPE_CANVAS_PATH_MAX];
        for (i = start; i < end; i++)
            canvas_tf(c, c->path[i].x, c->path[i].y, &tx[i], &ty[i]);
        miny = ty[start];
        maxy = ty[start];
        for (i = start + 1; i < end; i++) {
            if (ty[i] < miny) miny = ty[i];
            if (ty[i] > maxy) maxy = ty[i];
        }
        ymin = (int)floorf(miny);
        ymax = (int)ceilf(maxy);
        for (y = ymin; y <= ymax; y++) {
            float tyv = (float)y + 0.5f, xs[HPE_CANVAS_PATH_MAX];
            int nx = 0, j;
            for (j = start; j < end; j++) {
                int k = j + 1 >= end ? start : j + 1;
                if ((ty[j] <= tyv && ty[k] > tyv) || (ty[k] <= tyv && ty[j] > tyv)) {
                    if (nx < HPE_CANVAS_PATH_MAX)
                        xs[nx++] = tx[j] + (tx[k] - tx[j]) * (tyv - ty[j]) / (ty[k] - ty[j]);
                }
            }
            /* sort crossings by x, then fill even-odd spans */
            for (j = 1; j < nx; j++) {
                float key = xs[j];
                int m = j - 1;
                while (m >= 0 && xs[m] > key) {
                    xs[m + 1] = xs[m];
                    m--;
                }
                xs[m + 1] = key;
            }
            for (j = 0; j + 1 < nx; j += 2) {
                int xa = (int)ceilf(xs[j]), xb = (int)ceilf(xs[j + 1]), x;
                for (x = xa; x < xb; x++)
                    canvas_put_pixel(c, x, y, col);
            }
        }
    }
}

void hpe_canvas_fill_path(HpeCanvas *c)
{
    int start, i;
    if (!c || c->path_count < 3)
        return;
    start = 0;
    for (i = 1; i <= c->path_count; i++) {
        if (i == c->path_count || c->path[i].move) {
            canvas_fill_subpath(c, start, i, c->fill_style);
            start = i;
        }
    }
}

void hpe_canvas_stroke_path(HpeCanvas *c)
{
    int i;
    if (!c || c->path_count < 2)
        return;
    for (i = 1; i < c->path_count; i++) {
        if (c->path[i].move)
            continue;
        canvas_brush_line(c, c->path[i - 1].x, c->path[i - 1].y,
                          c->path[i].x, c->path[i].y, c->stroke_style);
    }
    if (c->closed)
        canvas_brush_line(c, c->path[c->path_count - 1].x, c->path[c->path_count - 1].y,
                          c->path[0].x, c->path[0].y, c->stroke_style);
}

/* ---- transform / state ---- */

void hpe_canvas_reset_transform(HpeCanvas *c)
{
    if (!c)
        return;
    c->a = 1.0f; c->b = 0.0f; c->c = 0.0f;
    c->d = 1.0f; c->e = 0.0f; c->f = 0.0f;
}

void hpe_canvas_save(HpeCanvas *c)
{
    if (!c || c->stack_count >= HPE_CANVAS_STACK_MAX)
        return;
    {
        int i = c->stack_count;
        c->stack[i].fill = c->fill_style;
        c->stack[i].stroke = c->stroke_style;
        c->stack[i].line_width = c->line_width;
        c->stack[i].alpha = c->global_alpha;
        c->stack[i].a = c->a; c->stack[i].b = c->b;
        c->stack[i].c = c->c; c->stack[i].d = c->d;
        c->stack[i].e = c->e; c->stack[i].f = c->f;
        c->stack_count++;
    }
}

void hpe_canvas_restore(HpeCanvas *c)
{
    int i;
    if (!c || c->stack_count <= 0)
        return;
    i = --c->stack_count;
    c->fill_style = c->stack[i].fill;
    c->stroke_style = c->stack[i].stroke;
    c->line_width = c->stack[i].line_width;
    c->global_alpha = c->stack[i].alpha;
    c->a = c->stack[i].a; c->b = c->stack[i].b;
    c->c = c->stack[i].c; c->d = c->stack[i].d;
    c->e = c->stack[i].e; c->f = c->stack[i].f;
}

/* multiply current matrix by T: M = M * T */
static void canvas_mul(HpeCanvas *c, float a, float b, float cc, float d, float e, float f)
{
    float na = c->a * a + c->c * b;
    float nb = c->b * a + c->d * b;
    float nc = c->a * cc + c->c * d;
    float nd = c->b * cc + c->d * d;
    float ne = c->a * e + c->c * f + c->e;
    float nf = c->b * e + c->d * f + c->f;
    c->a = na; c->b = nb; c->c = nc; c->d = nd; c->e = ne; c->f = nf;
}

void hpe_canvas_translate(HpeCanvas *c, float x, float y)
{
    if (c)
        canvas_mul(c, 1, 0, 0, 1, x, y);
}

void hpe_canvas_rotate(HpeCanvas *c, float rad)
{
    float co = cosf(rad), si = sinf(rad);
    if (c)
        canvas_mul(c, co, si, -si, co, 0, 0);
}

void hpe_canvas_scale(HpeCanvas *c, float x, float y)
{
    if (c)
        canvas_mul(c, x, 0, 0, y, 0, 0);
}

void hpe_canvas_set_transform(HpeCanvas *c, float a, float b, float cc, float d, float e, float f)
{
    if (!c)
        return;
    c->a = a; c->b = b; c->c = cc; c->d = d; c->e = e; c->f = f;
}

/* ---- pixel read / write ---- */

void hpe_canvas_get_image_data(HpeCanvas *c, int x, int y, int w, int h, uint8_t *out)
{
    int i, j, k = 0;
    if (!c || !out)
        return;
    for (j = y; j < y + h; j++) {
        for (i = x; i < x + w; i++) {
            uint32_t p = 0;
            if (i >= 0 && j >= 0 && i < c->pm.width && j < c->pm.height)
                p = c->pm.pixels[(size_t)j * (size_t)c->pm.width + (size_t)i];
            out[k++] = (uint8_t)((p >> 16) & 0xff);
            out[k++] = (uint8_t)((p >> 8) & 0xff);
            out[k++] = (uint8_t)(p & 0xff);
            out[k++] = (uint8_t)((p >> 24) & 0xff);
        }
    }
}

void hpe_canvas_put_image_data(HpeCanvas *c, int x, int y, int w, int h, const uint8_t *in)
{
    int i, j, k = 0;
    if (!c || !in)
        return;
    for (j = y; j < y + h; j++) {
        for (i = x; i < x + w; i++) {
            uint32_t p = ((uint32_t)in[k + 3] << 24) | ((uint32_t)in[k] << 16) |
                         ((uint32_t)in[k + 1] << 8) | (uint32_t)in[k + 2];
            k += 4;
            if (i >= 0 && j >= 0 && i < c->pm.width && j < c->pm.height)
                c->pm.pixels[(size_t)j * (size_t)c->pm.width + (size_t)i] = p;
        }
    }
}
