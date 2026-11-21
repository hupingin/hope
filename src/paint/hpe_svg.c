/* Hope UI Engine - SVG painter (subset)
 *
 * The <svg> element participates in layout as a replaced block whose intrinsic
 * size comes from its width/height attributes; its children are *not* turned
 * into boxes. They are painted straight from the DOM, which is what a browser
 * effectively does for SVG content, and keeps the CSS box model untouched.
 *
 * Supported elements: <g>, <rect> (with rx), <circle>, <ellipse>, <line>,
 * <polygon>, <polyline>, <path>, <text>, <defs>/<use> and the gradient
 * definitions <linearGradient>/<radialGradient>/<stop>.
 * Attributes: fill/stroke/stroke-width/fill-opacity/stroke-opacity,
 * transform (translate/scale/rotate/skewX/skewY/matrix), viewBox scaling,
 * text x/y/font-size/font-weight/text-anchor/textLength/dominant-baseline and
 * fill="url(#id)" gradient references.
 * Path commands: M/L/H/V/C/S/Q/T/A/Z, absolute and relative.
 * Colours support #rgb #rrggbb #rrggbbaa, names, rgb()/rgba()/hsl().
 */
#include "paint/hpe_svg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"
#include "dom/hpe_dom.h"
#include "layout/hpe_layout.h"
#include "render/hpe_font.h"
#include "render/hpe_raster.h"

#define SVG_MAX_STOPS 8

/* ---------------- affine matrix (user space -> device space) ----------------
 * point' = (a*x + c*y + e, b*x + d*y + f)                                   */

typedef struct {
    float a, b, c, d, e, f;
} Mat;

static Mat mat_identity(void)
{
    Mat m = {1, 0, 0, 1, 0, 0};
    return m;
}

/* M = A * B : A is applied after B */
static Mat mat_mul(Mat A, Mat B)
{
    Mat r;
    r.a = A.a * B.a + A.c * B.b;
    r.b = A.b * B.a + A.d * B.b;
    r.c = A.a * B.c + A.c * B.d;
    r.d = A.b * B.c + A.d * B.d;
    r.e = A.a * B.e + A.c * B.f + A.e;
    r.f = A.b * B.e + A.d * B.f + A.f;
    return r;
}

static void mat_apply(Mat m, float x, float y, float *ox, float *oy)
{
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}

/* inverse of an affine matrix; identity when singular */
static Mat mat_invert(Mat m)
{
    float det = m.a * m.d - m.b * m.c;
    Mat r = mat_identity();
    if (fabsf(det) < 1e-9f)
        return r;
    r.a = m.d / det;
    r.b = -m.b / det;
    r.c = -m.c / det;
    r.d = m.a / det;
    r.e = (m.c * m.f - m.d * m.e) / det;
    r.f = (m.b * m.e - m.a * m.f) / det;
    return r;
}

/* nominal uniform scale factor (used for stroke widths / font sizes) */
static float mat_scale(Mat m)
{
    float det = m.a * m.d - m.b * m.c;
    float s = sqrtf(fabsf(det));
    return s > 1e-6f ? s : 1.0f;
}

/* ---------------- gradient definition ---------------- */

typedef struct {
    int kind;   /* 0 = none, 1 = linear, 2 = radial */
    int units;  /* 0 = objectBoundingBox, 1 = userSpaceOnUse */
    float x1, y1, x2, y2; /* linear axis */
    float cx, cy, r, fx, fy;
    HpeRasterStop stops[SVG_MAX_STOPS];
    int stop_count;
} SvgGrad;

typedef struct SvgCtx {
    HpePixmap *pm;
    Mat ctm;        /* user -> device */
    float vp_w, vp_h;
    HpeColor fill;
    int has_fill;
    HpeColor stroke;
    int has_stroke;
    float stroke_w;
    float fill_opacity;
    float stroke_opacity;
    HpeDomNode *root; /* the <svg> root, for defs/id lookup */
    int use_depth;
    float *cov_scratch; /* reusable per-row coverage buffer */
    int cov_cap;
} SvgCtx;

/* ---------------- raster primitives ---------------- */

static void blend_px(HpePixmap *pm, int x, int y, HpeColor c, float cov)
{
    uint32_t *dst;
    float a, ia, dr, dg, db, sr, sg, sb;
    uint32_t d;

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
    dr = (float)((d >> 16) & 0xFF);
    dg = (float)((d >> 8) & 0xFF);
    db = (float)(d & 0xFF);
    ia = 1.0f - a;
    sr = (float)c.r;
    sg = (float)c.g;
    sb = (float)c.b;
    {
        int r = (int)(sr * a + dr * ia + 0.5f);
        int g = (int)(sg * a + dg * ia + 0.5f);
        int b = (int)(sb * a + db * ia + 0.5f);
        *dst = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

/* resolves the colour of a gradient (or a flat colour) at device pixel (px,py) */
static HpeColor grad_color_at(const SvgGrad *grad, Mat ctm, HpeColor flat, float px, float py)
{
    HpeColor c;
    if (!grad || grad->kind == 0)
        return flat;
    if (grad->kind == 1) {
        float gx0, gy0, gx1, gy1, dx, dy, len2, t;
        mat_apply(ctm, grad->x1, grad->y1, &gx0, &gy0);
        mat_apply(ctm, grad->x2, grad->y2, &gx1, &gy1);
        dx = gx1 - gx0;
        dy = gy1 - gy0;
        len2 = dx * dx + dy * dy;
        t = len2 > 1e-6f ? ((px + 0.5f - gx0) * dx + (py + 0.5f - gy0) * dy) / len2 : 0.0f;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        hpe_sample_stops(grad->stops, grad->stop_count, t, &c);
        return c;
    }
    {
        float cx, cy, r, d, t;
        mat_apply(ctm, grad->cx, grad->cy, &cx, &cy);
        r = grad->r * mat_scale(ctm);
        d = sqrtf((px + 0.5f - cx) * (px + 0.5f - cx) + (py + 0.5f - cy) * (py + 0.5f - cy));
        t = r > 1e-6f ? d / r : 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        hpe_sample_stops(grad->stops, grad->stop_count, t, &c);
        return c;
    }
}

/* stroke a single segment as a capsule of `width` in device space */
static void stroke_capsule(HpePixmap *pm, float x0, float y0, float x1, float y1, float width,
                           HpeColor c, const SvgGrad *grad, Mat ctm)
{
    float hw = width * 0.5f;
    float vx = x1 - x0, vy = y1 - y0;
    float len2 = vx * vx + vy * vy;
    int bx0 = (int)floorf(fminf(x0, x1) - hw) - 1;
    int bx1 = (int)ceilf(fmaxf(x0, x1) + hw) + 1;
    int by0 = (int)floorf(fminf(y0, y1) - hw) - 1;
    int by1 = (int)ceilf(fmaxf(y0, y1) + hw) + 1;
    int px, py;

    if (width <= 0.0f)
        return;
    for (py = by0; py < by1; py++) {
        for (px = bx0; px < bx1; px++) {
            float wx = (float)px + 0.5f - x0;
            float wy = (float)py + 0.5f - y0;
            float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
            float dx, dy, d;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            dx = wx - t * vx;
            dy = wy - t * vy;
            d = sqrtf(dx * dx + dy * dy);
            blend_px(pm, px, py, grad_color_at(grad, ctm, c, (float)px, (float)py), hw + 0.5f - d);
        }
    }
}

/* ---------------- polygon list (device space) ---------------- */

typedef struct {
    float *pts; /* x,y pairs */
    int count;
    int cap;
    int closed;
} SubPath;

typedef struct {
    SubPath *subs;
    int count;
    int cap;
} Polys;

static void sub_push(SubPath *sp, float x, float y)
{
    if (sp->count + 1 >= sp->cap) {
        sp->cap = sp->cap ? sp->cap * 2 : 16;
        sp->pts = (float *)hpe_realloc(sp->pts, (size_t)sp->cap * 2 * sizeof(float));
    }
    sp->pts[sp->count * 2] = x;
    sp->pts[sp->count * 2 + 1] = y;
    sp->count++;
}

static SubPath *polys_new(Polys *p)
{
    SubPath *sp;
    if (p->count + 1 > p->cap) {
        p->cap = p->cap ? p->cap * 2 : 4;
        p->subs = (SubPath *)hpe_realloc(p->subs, (size_t)p->cap * sizeof(SubPath));
    }
    sp = &p->subs[p->count++];
    memset(sp, 0, sizeof(*sp));
    return sp;
}

static void polys_free(Polys *p)
{
    int i;
    for (i = 0; i < p->count; i++)
        hpe_free(p->subs[i].pts);
    hpe_free(p->subs);
    p->subs = NULL;
    p->count = p->cap = 0;
}

/* distribute `w` of horizontal coverage across the pixel bins spanned by [a,b) */
static void add_span(float *cov, int x0, int x1, float a, float b, float w)
{
    int ia = (int)floorf(a), ib = (int)ceilf(b);
    int i;
    if (b <= a)
        return;
    for (i = ia; i < ib; i++) {
        float l = (float)i, r = (float)i + 1.0f;
        float s = fmaxf(a, l), e = fminf(b, r);
        int idx = i - x0;
        if (e > s && idx >= 0 && idx < x1)
            cov[idx] += (e - s) * w;
    }
}

/* even-odd scanline fill of every subpath; solid colour or gradient */
static void fill_polys(SvgCtx *ctx, Polys *p, HpeColor color, const SvgGrad *grad, Mat ctm,
                       float bx0, float by0, float bx1, float by1)
{
    HpePixmap *pm = ctx->pm;
    int y0, y1, x0, x1, py;
    float *cov;
    int i, s;

    if (p->count == 0)
        return;
    /* overall device bbox */
    {
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        for (i = 0; i < p->count; i++) {
            SubPath *sp = &p->subs[i];
            int k;
            for (k = 0; k < sp->count; k++) {
                float x = sp->pts[k * 2], y = sp->pts[k * 2 + 1];
                if (x < minx)
                    minx = x;
                if (x > maxx)
                    maxx = x;
                if (y < miny)
                    miny = y;
                if (y > maxy)
                    maxy = y;
            }
        }
        if (bx0 > minx)
            minx = bx0;
        if (by0 > miny)
            miny = by0;
        if (bx1 < maxx)
            maxx = bx1;
        if (by1 < maxy)
            maxy = by1;
        x0 = (int)floorf(minx);
        x1 = (int)ceilf(maxx);
        y0 = (int)floorf(miny);
        y1 = (int)ceilf(maxy);
    }
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

    if (ctx->cov_cap < x1 - x0) {
        ctx->cov_cap = x1 - x0;
        ctx->cov_scratch = (float *)hpe_realloc(ctx->cov_scratch, (size_t)ctx->cov_cap * sizeof(float));
    }
    cov = ctx->cov_scratch;

    for (py = y0; py < y1; py++) {
        int px;
        float *xs = NULL;
        int nx = 0, ncap = 0;
        memset(cov, 0, (size_t)(x1 - x0) * sizeof(float));
        for (s = 0; s < 4; s++) {
            float sy = (float)py + ((float)s + 0.5f) * 0.25f;
            nx = 0;
            for (i = 0; i < p->count; i++) {
                SubPath *sp = &p->subs[i];
                int k, n = sp->count;
                for (k = 0; k < n; k++) {
                    int k2 = (k + 1) % n;
                    float ax = sp->pts[k * 2], ay = sp->pts[k * 2 + 1];
                    float bxp = sp->pts[k2 * 2], byp = sp->pts[k2 * 2 + 1];
                    float ymin = fminf(ay, byp), ymax = fmaxf(ay, byp);
                    if (sy < ymin || sy >= ymax)
                        continue;
                    if (nx + 1 > ncap) {
                        ncap = ncap ? ncap * 2 : 16;
                        xs = (float *)hpe_realloc(xs, (size_t)ncap * sizeof(float));
                    }
                    xs[nx++] = ax + (sy - ay) / (byp - ay) * (bxp - ax);
                }
            }
            /* insertion sort the intersections */
            for (i = 1; i < nx; i++) {
                float key = xs[i];
                int j = i - 1;
                while (j >= 0 && xs[j] > key) {
                    xs[j + 1] = xs[j];
                    j--;
                }
                xs[j + 1] = key;
            }
            for (i = 0; i + 1 < nx; i += 2)
                add_span(cov, x0, x1 - x0, xs[i], xs[i + 1], 0.25f);
        }
        hpe_free(xs);

        for (px = 0; px < x1 - x0; px++) {
            float cvg = cov[px];
            HpeColor c;
            if (cvg <= 0.002f)
                continue;
            if (cvg > 1.0f)
                cvg = 1.0f;
            if (grad && grad->kind == 1) {
                float gx0, gy0, gx1, gy1, dx, dy, len2, t;
                mat_apply(ctm, grad->x1, grad->y1, &gx0, &gy0);
                mat_apply(ctm, grad->x2, grad->y2, &gx1, &gy1);
                dx = gx1 - gx0;
                dy = gy1 - gy0;
                len2 = dx * dx + dy * dy;
                t = len2 > 1e-6f
                        ? (((float)(x0 + px) + 0.5f - gx0) * dx +
                           ((float)py + 0.5f - gy0) * dy) / len2
                        : 0.0f;
                if (t < 0.0f)
                    t = 0.0f;
                if (t > 1.0f)
                    t = 1.0f;
                hpe_sample_stops(grad->stops, grad->stop_count, t, &c);
            } else if (grad && grad->kind == 2) {
                float cx, cy, r, d, t;
                mat_apply(ctm, grad->cx, grad->cy, &cx, &cy);
                r = grad->r * mat_scale(ctm);
                d = sqrtf(((float)(x0 + px) + 0.5f - cx) * ((float)(x0 + px) + 0.5f - cx) +
                          ((float)py + 0.5f - cy) * ((float)py + 0.5f - cy));
                t = r > 1e-6f ? d / r : 0.0f;
                if (t > 1.0f)
                    t = 1.0f;
                hpe_sample_stops(grad->stops, grad->stop_count, t, &c);
            } else {
                c = color;
            }
            blend_px(pm, x0 + px, py, c, cvg);
        }
    }
}

static void stroke_polys(SvgCtx *ctx, Polys *p, HpeColor color, float width, const SvgGrad *grad)
{
    int i, k;
    if (width <= 0.0f)
        return;
    for (i = 0; i < p->count; i++) {
        SubPath *sp = &p->subs[i];
        int n = sp->count;
        for (k = 0; k + 1 < n; k++)
            stroke_capsule(ctx->pm, sp->pts[k * 2], sp->pts[k * 2 + 1], sp->pts[(k + 1) * 2],
                           sp->pts[(k + 1) * 2 + 1], width, color, grad, ctx->ctm);
        if (n >= 2 && sp->closed) /* close the loop for visual continuity */
            stroke_capsule(ctx->pm, sp->pts[(n - 1) * 2], sp->pts[(n - 1) * 2 + 1], sp->pts[0],
                           sp->pts[1], width, color, grad, ctx->ctm);
    }
}

/* ---------------- attribute helpers ---------------- */

static const char *attr(HpeDomNode *n, const char *name)
{
    const char *v = hpe_dom_attr(n, name);
    return (v && *v) ? v : NULL;
}

static int attr_num(HpeDomNode *n, const char *name, float *out)
{
    const char *v = attr(n, name);
    char *end = NULL;
    double d;
    if (!v)
        return 0;
    d = strtod(v, &end);
    if (end == v)
        return 0;
    *out = (float)d;
    return 1;
}

/* returns 1 when the attribute exists; "none" yields a fully transparent colour */
static int attr_color(HpeDomNode *n, const char *name, HpeColor *out)
{
    const char *v = attr(n, name);
    if (!v)
        return 0;
    if (hpe_str_ieq(v, "none")) {
        *out = hpe_rgb(0, 0, 0);
        out->a = 0;
        return 1;
    }
    if (hpe_color_parse(v, out))
        return 1;
    *out = hpe_rgb(0, 0, 0);
    return 1;
}

static void ctx_apply_style(SvgCtx *ctx, HpeDomNode *n)
{
    HpeColor c;
    float f;
    if (attr_color(n, "fill", &c)) {
        ctx->fill = c;
        ctx->has_fill = c.a > 0;
    }
    if (attr_color(n, "stroke", &c)) {
        ctx->stroke = c;
        ctx->has_stroke = c.a > 0;
    }
    if (attr_num(n, "stroke-width", &f))
        ctx->stroke_w = f;
    if (attr_num(n, "fill-opacity", &f))
        ctx->fill_opacity = f;
    if (attr_num(n, "stroke-opacity", &f))
        ctx->stroke_opacity = f;
}

/* ---------------- transform parsing ---------------- */

static Mat parse_transform(const char *s)
{
    Mat acc = mat_identity();
    const char *p = s;

    if (!s)
        return acc;
    while (*p) {
        char name[16];
        int n = 0;
        float v[6];
        int vn = 0;
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;
        while (*p && (hpe_ascii_lower((unsigned char)*p) >= 'a' &&
                      hpe_ascii_lower((unsigned char)*p) <= 'z') && n < 15)
            name[n++] = *p++;
        name[n] = '\0';
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '(') {
            /* malformed: bail out to avoid an infinite loop */
            while (*p && *p != ')')
                p++;
            if (*p == ')')
                p++;
            continue;
        }
        p++;
        while (*p && *p != ')' && vn < 6) {
            char *end = NULL;
            double d;
            while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')
                p++;
            if (*p == ')')
                break;
            d = strtod(p, &end);
            if (end == p)
                break;
            v[vn++] = (float)d;
            p = end;
        }
        while (*p && *p != ')')
            p++;
        if (*p == ')')
            p++;

        {
            Mat t = mat_identity();
            if (!strcmp(name, "translate") && vn >= 1) {
                t.e = v[0];
                t.f = vn >= 2 ? v[1] : 0.0f;
            } else if (!strcmp(name, "scale") && vn >= 1) {
                t.a = v[0];
                t.d = vn >= 2 ? v[1] : v[0];
            } else if (!strcmp(name, "rotate") && vn >= 1) {
                float ang = v[0] * 3.14159265f / 180.0f;
                float ca = cosf(ang), sa = sinf(ang);
                Mat r = mat_identity();
                r.a = ca;
                r.b = sa;
                r.c = -sa;
                r.d = ca;
                if (vn >= 3) {
                    Mat t1 = mat_identity(), t2 = mat_identity();
                    t1.e = v[1];
                    t1.f = v[2];
                    t2.e = -v[1];
                    t2.f = -v[2];
                    r = mat_mul(t1, mat_mul(r, t2));
                }
                t = r;
            } else if (!strcmp(name, "skewX") && vn >= 1) {
                t.c = tanf(v[0] * 3.14159265f / 180.0f);
            } else if (!strcmp(name, "skewY") && vn >= 1) {
                t.b = tanf(v[0] * 3.14159265f / 180.0f);
            } else if (!strcmp(name, "matrix") && vn >= 6) {
                t.a = v[0];
                t.b = v[1];
                t.c = v[2];
                t.d = v[3];
                t.e = v[4];
                t.f = v[5];
            }
            acc = mat_mul(acc, t);
        }
    }
    return acc;
}

/* ---------------- gradient lookup ---------------- */

static HpeDomNode *find_by_id(HpeDomNode *root, const char *id)
{
    HpeDomNode *c;
    const char *v;
    if (!root)
        return NULL;
    v = hpe_dom_attr(root, "id");
    if (v && !strcmp(v, id))
        return root;
    for (c = root->first_child; c; c = c->next_sibling) {
        HpeDomNode *r = find_by_id(c, id);
        if (r)
            return r;
    }
    return NULL;
}

static float parse_offset(const char *s)
{
    char *end = NULL;
    double d = strtod(s, &end);
    if (end == s)
        return 0.0f;
    if (*end == '%')
        return (float)(d / 100.0);
    return (float)d;
}

/* fills `g` from a <linearGradient>/<radialGradient> element */
static int parse_gradient(HpeDomNode *n, SvgGrad *g)
{
    HpeDomNode *c;
    const char *units;
    memset(g, 0, sizeof(*g));
    if (hpe_str_ieq(n->tag, "linearGradient")) {
        g->kind = 1;
        g->x1 = 0.0f;
        g->y1 = 0.0f;
        g->x2 = 1.0f;
        g->y2 = 0.0f;
    } else if (hpe_str_ieq(n->tag, "radialGradient")) {
        g->kind = 2;
        g->cx = 0.5f;
        g->cy = 0.5f;
        g->r = 0.5f;
    } else {
        return 0;
    }
    units = attr(n, "gradientUnits");
    g->units = (units && !strcmp(units, "userSpaceOnUse")) ? 1 : 0;
    if (g->kind == 1) {
        const char *v;
        if ((v = attr(n, "x1")))
            g->x1 = parse_offset(v);
        if ((v = attr(n, "y1")))
            g->y1 = parse_offset(v);
        if ((v = attr(n, "x2")))
            g->x2 = parse_offset(v);
        if ((v = attr(n, "y2")))
            g->y2 = parse_offset(v);
    } else {
        const char *v;
        if ((v = attr(n, "cx")))
            g->cx = parse_offset(v);
        if ((v = attr(n, "cy")))
            g->cy = parse_offset(v);
        if ((v = attr(n, "r")))
            g->r = parse_offset(v);
    }
    for (c = n->first_child; c; c = c->next_sibling) {
        HpeColor col;
        float off = 0.0f;
        if (c->type != HPE_NODE_ELEMENT || strcmp(c->tag, "stop"))
            continue;
        if (g->stop_count >= SVG_MAX_STOPS)
            break;
        {
            const char *v = attr(c, "offset");
            if (v)
                off = parse_offset(v);
        }
        col = hpe_rgb(0, 0, 0);
        {
            const char *v = attr(c, "stop-color");
            if (v)
                hpe_color_parse(v, &col);
        }
        {
            const char *v = attr(c, "stop-opacity");
            if (v)
                col.a = (uint8_t)(col.a * strtod(v, NULL));
        }
        g->stops[g->stop_count].pos = off;
        g->stops[g->stop_count].color = col;
        g->stop_count++;
    }
    return g->stop_count > 0;
}

/* resolves fill="url(#id)" into a gradient; returns 1 on success */
static int resolve_gradient(SvgCtx *ctx, HpeDomNode *n, const char *which, SvgGrad *out,
                            float ux0, float uy0, float ux1, float uy1)
{
    const char *v = attr(n, which);
    const char *hash;
    char id[128];
    HpeDomNode *def;
    size_t len;

    if (!v || strncmp(v, "url(", 4) != 0)
        return 0;
    hash = strchr(v, '#');
    if (!hash)
        return 0;
    hash++;
    len = 0;
    while (hash[len] && hash[len] != ')' && len < sizeof(id) - 1) {
        id[len] = hash[len];
        len++;
    }
    id[len] = '\0';
    def = find_by_id(ctx->root, id);
    if (!def)
        return 0;
    if (!parse_gradient(def, out))
        return 0;
    /* objectBoundingBox: map fractional coordinates onto the shape's user bbox */
    if (out->units == 0) {
        float bw = ux1 - ux0, bh = uy1 - uy0;
        if (out->kind == 1) {
            out->x1 = ux0 + out->x1 * bw;
            out->y1 = uy0 + out->y1 * bh;
            out->x2 = ux0 + out->x2 * bw;
            out->y2 = uy0 + out->y2 * bh;
        } else {
            out->cx = ux0 + out->cx * bw;
            out->cy = uy0 + out->cy * bh;
            out->r = out->r * ((bw + bh) * 0.5f);
        }
    }
    return 1;
}

/* ---------------- path ---------------- */

#define MAX_FLAT 8192

static void dev_move(Polys *p, SvgCtx *ctx, float ux, float uy)
{
    SubPath *sp = polys_new(p);
    float dx, dy;
    mat_apply(ctx->ctm, ux, uy, &dx, &dy);
    sub_push(sp, dx, dy);
}

static void dev_line(Polys *p, SvgCtx *ctx, float ux, float uy)
{
    SubPath *sp;
    float dx, dy;
    if (p->count == 0)
        dev_move(p, ctx, ux, uy);
    sp = &p->subs[p->count - 1];
    mat_apply(ctx->ctm, ux, uy, &dx, &dy);
    sub_push(sp, dx, dy);
}

static void flatten_cubic(Polys *p, SvgCtx *ctx, float x0, float y0, float x1, float y1, float x2,
                          float y2, float x3, float y3)
{
    float approx = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)) +
                   sqrtf((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1)) +
                   sqrtf((x3 - x2) * (x3 - x2) + (y3 - y2) * (y3 - y2));
    int steps = (int)(approx * mat_scale(ctx->ctm) / 4.0f) + 6;
    int s;
    if (steps > 200)
        steps = 200;
    for (s = 1; s <= steps; s++) {
        float t = (float)s / (float)steps, mt = 1.0f - t;
        float bx = mt * mt * mt * x0 + 3 * mt * mt * t * x1 + 3 * mt * t * t * x2 + t * t * t * x3;
        float by = mt * mt * mt * y0 + 3 * mt * mt * t * y1 + 3 * mt * t * t * y2 + t * t * t * y3;
        dev_line(p, ctx, bx, by);
    }
}

static void flatten_quad(Polys *p, SvgCtx *ctx, float x0, float y0, float x1, float y1, float x2,
                         float y2)
{
    float approx = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)) +
                   sqrtf((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
    int steps = (int)(approx * mat_scale(ctx->ctm) / 4.0f) + 6;
    int s;
    if (steps > 200)
        steps = 200;
    for (s = 1; s <= steps; s++) {
        float t = (float)s / (float)steps, mt = 1.0f - t;
        float bx = mt * mt * x0 + 2 * mt * t * x1 + t * t * x2;
        float by = mt * mt * y0 + 2 * mt * t * y1 + t * t * y2;
        dev_line(p, ctx, bx, by);
    }
}

/* endpoint->center parameterisation of an SVG elliptical arc, per the spec's
 * implementation notes; flattens the arc into line segments */
static void flatten_arc(Polys *p, SvgCtx *ctx, float x0, float y0, float rx, float ry, float phi,
                        int large, int sweep, float x1, float y1)
{
    float cosp, sinp, dx2, dy2, x1p, y1p, rx2, ry2, lam, sign, cx1, cy1, cx, cy;
    float theta1, dtheta, t;
    int steps, i;

    if (rx == 0.0f || ry == 0.0f) {
        dev_line(p, ctx, x1, y1);
        return;
    }
    rx = fabsf(rx);
    ry = fabsf(ry);
    cosp = cosf(phi * 3.14159265f / 180.0f);
    sinp = sinf(phi * 3.14159265f / 180.0f);
    dx2 = (x0 - x1) * 0.5f;
    dy2 = (y0 - y1) * 0.5f;
    x1p = cosp * dx2 + sinp * dy2;
    y1p = -sinp * dx2 + cosp * dy2;
    rx2 = rx * rx;
    ry2 = ry * ry;
    lam = (x1p * x1p) / rx2 + (y1p * y1p) / ry2;
    if (lam > 1.0f) {
        float sc = sqrtf(lam);
        rx *= sc;
        ry *= sc;
        rx2 = rx * rx;
        ry2 = ry * ry;
    }
    {
        float num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
        float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
        float coef = den > 0.0f ? sqrtf(fmaxf(0.0f, num / den)) : 0.0f;
        sign = (large != sweep) ? 1.0f : -1.0f;
        cx1 = coef * sign * (rx * y1p / ry);
        cy1 = coef * sign * (-ry * x1p / rx);
    }
    cx = cosp * cx1 - sinp * cy1 + (x0 + x1) * 0.5f;
    cy = sinp * cx1 + cosp * cy1 + (y0 + y1) * 0.5f;
    {
        float ux = (x1p - cx1) / rx, uy = (y1p - cy1) / ry;
        float vx = (-x1p - cx1) / rx, vy = (-y1p - cy1) / ry;
        float dot = ux * vx + uy * vy;
        float len = sqrtf(ux * ux + uy * uy) * sqrtf(vx * vx + vy * vy);
        float ang = acosf(fmaxf(-1.0f, fminf(1.0f, len > 0.0f ? dot / len : 0.0f)));
        theta1 = acosf(fmaxf(-1.0f, fminf(1.0f, ux)));
        if (uy < 0.0f)
            theta1 = -theta1;
        if (ux * vy - uy * vx < 0.0f)
            ang = -ang;
        dtheta = ang;
    }
    steps = (int)(fabsf(dtheta) * fmaxf(rx, ry) * mat_scale(ctx->ctm) / 3.0f) + 4;
    if (steps > 200)
        steps = 200;
    for (i = 1; i <= steps; i++) {
        float ex, ey, px, py;
        t = theta1 + dtheta * (float)i / (float)steps;
        ex = rx * cosf(t);
        ey = ry * sinf(t);
        px = cosp * ex - sinp * ey + cx;
        py = sinp * ex + cosp * ey + cy;
        dev_line(p, ctx, px, py);
    }
}

static void build_path(Polys *p, SvgCtx *ctx, const char *d)
{
    const char *q = d;
    char cmd = 0;
    float cx = 0.0f, cy = 0.0f, sx = 0.0f, sy = 0.0f;
    float last_c1x = 0.0f, last_c1y = 0.0f, last_qx = 0.0f, last_qy = 0.0f;
    int started = 0;

    if (!d)
        return;
    while (*q) {
        while (*q == ' ' || *q == ',' || *q == '\t' || *q == '\n' || *q == '\r')
            q++;
        if (!*q)
            break;
        if ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')) {
            cmd = *q++;
            continue;
        }
        if (!cmd)
            break;
        {
            char upper = (char)(cmd >= 'a' && cmd <= 'z' ? cmd - 'a' + 'A' : cmd);
            int rel = cmd >= 'a' && cmd <= 'z';
            float a[7];
            int want, n = 0, i;
            switch (upper) {
            case 'M':
            case 'L':
            case 'T':
                want = 2;
                break;
            case 'H':
            case 'V':
                want = 1;
                break;
            case 'C':
                want = 6;
                break;
            case 'S':
            case 'Q':
                want = 4;
                break;
            case 'A':
                want = 7;
                break;
            case 'Z':
                if (started) {
                    SubPath *sp = &p->subs[p->count - 1];
                    sp->closed = 1;
                    cx = sx;
                    cy = sy;
                }
                cmd = 0;
                continue;
            default:
                /* unknown command: consume one number to make progress */
                {
                    char *end = NULL;
                    strtod(q, &end);
                    q = (end == q) ? q + 1 : end;
                }
                continue;
            }
            for (i = 0; i < want; i++) {
                char *end = NULL;
                double v;
                while (*q == ' ' || *q == ',' || *q == '\t' || *q == '\n' || *q == '\r')
                    q++;
                v = strtod(q, &end);
                if (end == q)
                    break;
                a[n++] = (float)v;
                q = end;
            }
            if (n < want)
                break;
            if (upper == 'M') {
                float nx = rel ? cx + a[0] : a[0];
                float ny = rel ? cy + a[1] : a[1];
                cx = sx = nx;
                cy = sy = ny;
                dev_move(p, ctx, cx, cy);
                started = 1;
                cmd = rel ? 'l' : 'L'; /* subsequent pairs are line-tos */
            } else if (upper == 'L') {
                float nx = rel ? cx + a[0] : a[0];
                float ny = rel ? cy + a[1] : a[1];
                if (!started) {
                    dev_move(p, ctx, nx, ny);
                    sx = nx;
                    sy = ny;
                    started = 1;
                } else {
                    dev_line(p, ctx, nx, ny);
                }
                cx = nx;
                cy = ny;
            } else if (upper == 'H') {
                float nx = rel ? cx + a[0] : a[0];
                dev_line(p, ctx, nx, cy);
                cx = nx;
            } else if (upper == 'V') {
                float ny = rel ? cy + a[0] : a[0];
                dev_line(p, ctx, cx, ny);
                cy = ny;
            } else if (upper == 'C') {
                float x1 = rel ? cx + a[0] : a[0], y1 = rel ? cy + a[1] : a[1];
                float x2 = rel ? cx + a[2] : a[2], y2 = rel ? cy + a[3] : a[3];
                float x3 = rel ? cx + a[4] : a[4], y3 = rel ? cy + a[5] : a[5];
                flatten_cubic(p, ctx, cx, cy, x1, y1, x2, y2, x3, y3);
                last_c1x = x2;
                last_c1y = y2;
                cx = x3;
                cy = y3;
            } else if (upper == 'S') {
                float x2 = rel ? cx + a[0] : a[0], y2 = rel ? cy + a[1] : a[1];
                float x3 = rel ? cx + a[2] : a[2], y3 = rel ? cy + a[3] : a[3];
                float x1 = 2 * cx - last_c1x, y1 = 2 * cy - last_c1y;
                flatten_cubic(p, ctx, cx, cy, x1, y1, x2, y2, x3, y3);
                last_c1x = x2;
                last_c1y = y2;
                cx = x3;
                cy = y3;
            } else if (upper == 'Q') {
                float x1 = rel ? cx + a[0] : a[0], y1 = rel ? cy + a[1] : a[1];
                float x2 = rel ? cx + a[2] : a[2], y2 = rel ? cy + a[3] : a[3];
                flatten_quad(p, ctx, cx, cy, x1, y1, x2, y2);
                last_qx = x1;
                last_qy = y1;
                cx = x2;
                cy = y2;
            } else if (upper == 'T') {
                float x2 = rel ? cx + a[0] : a[0], y2 = rel ? cy + a[1] : a[1];
                float x1 = 2 * cx - last_qx, y1 = 2 * cy - last_qy;
                flatten_quad(p, ctx, cx, cy, x1, y1, x2, y2);
                last_qx = x1;
                last_qy = y1;
                cx = x2;
                cy = y2;
            } else if (upper == 'A') {
                float rx = a[0], ry = a[1], phi = a[2];
                int large = a[3] != 0.0f, sweep = a[4] != 0.0f;
                float x2 = rel ? cx + a[5] : a[5], y2 = rel ? cy + a[6] : a[6];
                flatten_arc(p, ctx, cx, cy, rx, ry, phi, large, sweep, x2, y2);
                cx = x2;
                cy = y2;
            }
        }
    }
}

/* ---------------- text ---------------- */

static void collect_text(HpeDomNode *n, char *buf, size_t cap, size_t *len)
{
    HpeDomNode *c;
    for (c = n->first_child; c; c = c->next_sibling) {
        if (c->type == HPE_NODE_TEXT && c->text) {
            size_t l = strlen(c->text);
            if (*len + l + 1 < cap) {
                memcpy(buf + *len, c->text, l);
                *len += l;
                buf[*len] = '\0';
            }
        } else if (c->type == HPE_NODE_ELEMENT) {
            if (!strcmp(c->tag, "title") || !strcmp(c->tag, "desc"))
                continue;
            collect_text(c, buf, cap, len);
        }
    }
}

static void draw_text(SvgCtx *ctx, HpeDomNode *n)
{
    char buf[1024];
    size_t len = 0;
    float x = 0.0f, y = 0.0f, size = 16.0f, text_len = 0.0f;
    HpeColor color = ctx->fill;
    int weight = 400;
    HpeFont *font;
    const char *anchor, *baseline;
    float w, dev_x, dev_y;
    float scale = mat_scale(ctx->ctm);
    float font_dev;

    buf[0] = '\0';
    collect_text(n, buf, sizeof(buf), &len);
    if (len == 0)
        return;
    attr_num(n, "x", &x);
    attr_num(n, "y", &y);
    attr_num(n, "font-size", &size);
    attr_num(n, "textLength", &text_len);
    {
        float fw;
        if (attr_num(n, "font-weight", &fw))
            weight = (int)fw;
    }
    {
        HpeColor c;
        if (attr_color(n, "fill", &c))
            color = c;
    }
    font_dev = size * scale;
    font = hpe_font_get(font_dev, weight, HPE_FONT_SANS);
    if (!font)
        return;
    w = hpe_font_measure(font, buf, (int)len);
    mat_apply(ctx->ctm, x, y, &dev_x, &dev_y);

    anchor = attr(n, "text-anchor");
    if (anchor && !strcmp(anchor, "middle"))
        dev_x -= w * 0.5f;
    else if (anchor && !strcmp(anchor, "end"))
        dev_x -= w;

    baseline = attr(n, "dominant-baseline");
    if (!baseline)
        baseline = attr(n, "alignment-baseline");
    if (baseline) {
        if (!strcmp(baseline, "middle") || !strcmp(baseline, "central"))
            dev_y += hpe_font_ascent(font) * 0.5f - hpe_font_descent(font) * 0.25f;
        else if (!strcmp(baseline, "hanging") || !strcmp(baseline, "text-before-edge"))
            dev_y += hpe_font_ascent(font);
        else if (!strcmp(baseline, "text-after-edge"))
            dev_y -= hpe_font_descent(font);
    }

    if (text_len > 0.0f && len > 1) {
        /* stretch the run by advancing each codepoint so the total matches
         * textLength (lengthAdjust="spacing"); glyphs are drawn individually */
        float target = text_len * (scale > 0.0f ? scale : 1.0f);
        float ratio = w > 1.0f ? target / w : 1.0f;
        const char *p = buf;
        float pen = dev_x;
        while ((int)(p - buf) < (int)len && *p) {
            uint32_t cp = 0;
            int adv = hpe_utf8_next(p, &cp);
            const HpeTextMask *mask;
            float cw;
            if (adv <= 0)
                break;
            mask = hpe_font_mask(font, p, adv);
            if (mask)
                hpe_draw_text_mask(ctx->pm, (int)floorf(pen), (int)floorf(dev_y), mask, color);
            cw = hpe_font_advance(font, cp) * ratio;
            pen += cw;
            p += adv;
        }
        return;
    }

    {
        const HpeTextMask *mask = hpe_font_mask(font, buf, (int)len);
        if (mask)
            hpe_draw_text_mask(ctx->pm, (int)floorf(dev_x), (int)floorf(dev_y), mask, color);
    }
}

/* ---------------- shape builders (user space) ---------------- */

static void ellipse_path(Polys *p, SvgCtx *ctx, float cx, float cy, float rx, float ry)
{
    int k, n = 64;
    SubPath *sp;
    dev_move(p, ctx, cx + rx, cy);
    sp = &p->subs[p->count - 1];
    for (k = 1; k <= n; k++) {
        float ang = (float)k / n * 6.2831853f;
        float ux = cx + cosf(ang) * rx;
        float uy = cy + sinf(ang) * ry;
        float dx, dy;
        mat_apply(ctx->ctm, ux, uy, &dx, &dy);
        sub_push(sp, dx, dy);
    }
    sp->closed = 1;
}

static void round_rect_path(Polys *p, SvgCtx *ctx, float x, float y, float w, float h, float r)
{
    SubPath *sp;
    int c;
    if (r > w * 0.5f)
        r = w * 0.5f;
    if (r > h * 0.5f)
        r = h * 0.5f;
    dev_move(p, ctx, x + r, y);
    sp = &p->subs[p->count - 1];
    {
        float dx, dy;
        /* top edge then corner arcs, using per-corner arc sampling */
        struct {
            float cx, cy, a0, a1;
        } corners[4] = {
            {x + w - r, y + r, -1.5707963f, 0.0f},
            {x + w - r, y + h - r, 0.0f, 1.5707963f},
            {x + r, y + h - r, 1.5707963f, 3.14159265f},
            {x + r, y + r, 3.14159265f, 4.71238898f},
        };
        mat_apply(ctx->ctm, x + w - r, y, &dx, &dy);
        sub_push(sp, dx, dy);
        for (c = 0; c < 4; c++) {
            int k, steps = 8;
            for (k = 1; k <= steps; k++) {
                float ang = corners[c].a0 + (corners[c].a1 - corners[c].a0) * (float)k / steps;
                float ux = corners[c].cx + cosf(ang) * r;
                float uy = corners[c].cy + sinf(ang) * r;
                mat_apply(ctx->ctm, ux, uy, &dx, &dy);
                sub_push(sp, dx, dy);
            }
        }
    }
    sp->closed = 1;
}

static void points_path(Polys *p, SvgCtx *ctx, const char *pts, int closed)
{
    SubPath *sp = NULL;
    const char *q = pts;
    if (!pts)
        return;
    while (*q) {
        char *end = NULL;
        double vx, vy;
        while (*q == ' ' || *q == ',' || *q == '\t' || *q == '\n' || *q == '\r')
            q++;
        if (!*q)
            break;
        vx = strtod(q, &end);
        if (end == q)
            break;
        q = end;
        while (*q == ' ' || *q == ',' || *q == '\t' || *q == '\n' || *q == '\r')
            q++;
        vy = strtod(q, &end);
        if (end == q)
            break;
        q = end;
        if (!sp) {
            dev_move(p, ctx, (float)vx, (float)vy);
            sp = &p->subs[p->count - 1];
        } else {
            float dx, dy;
            mat_apply(ctx->ctm, (float)vx, (float)vy, &dx, &dy);
            sub_push(sp, dx, dy);
        }
    }
    if (sp)
        sp->closed = closed;
}

/* user-space bbox of a points="..." list */
static void points_bbox(const char *pts, float *x0, float *y0, float *x1, float *y1)
{
    const char *q = pts;
    int have = 0;
    *x0 = *y0 = 1e9f;
    *x1 = *y1 = -1e9f;
    if (!pts)
        return;
    while (*q) {
        char *end = NULL;
        double vx, vy;
        while (*q == ' ' || *q == ',' || *q == '\t' || *q == '\n' || *q == '\r')
            q++;
        if (!*q)
            break;
        vx = strtod(q, &end);
        if (end == q)
            break;
        q = end;
        while (*q == ' ' || *q == ',' || *q == '\t' || *q == '\n' || *q == '\r')
            q++;
        vy = strtod(q, &end);
        if (end == q)
            break;
        q = end;
        if (!have) {
            *x0 = *x1 = (float)vx;
            *y0 = *y1 = (float)vy;
            have = 1;
        } else {
            if ((float)vx < *x0)
                *x0 = (float)vx;
            if ((float)vx > *x1)
                *x1 = (float)vx;
            if ((float)vy < *y0)
                *y0 = (float)vy;
            if ((float)vy > *y1)
                *y1 = (float)vy;
        }
    }
}

/* user-space bbox of a path string, evaluated with an identity matrix (used to
 * map objectBoundingBox gradients onto paths) */
static void path_bbox(const char *d, float *x0, float *y0, float *x1, float *y1)
{
    Polys p;
    SvgCtx tmp;
    int i, k;
    *x0 = *y0 = 1e9f;
    *x1 = *y1 = -1e9f;
    if (!d)
        return;
    memset(&p, 0, sizeof(p));
    memset(&tmp, 0, sizeof(tmp));
    tmp.ctm = mat_identity();
    build_path(&p, &tmp, d);
    for (i = 0; i < p.count; i++) {
        SubPath *sp = &p.subs[i];
        for (k = 0; k < sp->count; k++) {
            float x = sp->pts[k * 2], y = sp->pts[k * 2 + 1];
            if (x < *x0)
                *x0 = x;
            if (x > *x1)
                *x1 = x;
            if (y < *y0)
                *y0 = y;
            if (y > *y1)
                *y1 = y;
        }
    }
    polys_free(&p);
}

/* ---------------- element walk ---------------- */

/* paint one shape: compute its user-space bbox for gradient mapping, build the
 * geometry, then fill and/or stroke */
static void paint_shape(SvgCtx *ctx, HpeDomNode *n, Polys *p, float ux0, float uy0, float ux1,
                        float uy1, int fillable, int strokable)
{
    SvgGrad grad;
    int have_grad = 0;
    HpeColor fill = ctx->fill;
    HpeColor stroke = ctx->stroke;
    int has_fill = ctx->has_fill;
    int has_stroke = ctx->has_stroke;
    float fop = ctx->fill_opacity;
    float sop = ctx->stroke_opacity;

    {
        HpeColor c;
        if (attr_color(n, "fill", &c)) {
            fill = c;
            has_fill = c.a > 0;
        }
        if (attr_color(n, "stroke", &c)) {
            stroke = c;
            has_stroke = c.a > 0;
        }
        {
            float f;
            if (attr_num(n, "fill-opacity", &f))
                fop = f;
            if (attr_num(n, "stroke-opacity", &f))
                sop = f;
        }
    }

    if (fillable && has_fill) {
        have_grad = resolve_gradient(ctx, n, "fill", &grad, ux0, uy0, ux1, uy1);
        if (have_grad) {
            int i;
            for (i = 0; i < grad.stop_count; i++)
                grad.stops[i].color.a = (uint8_t)(grad.stops[i].color.a * fop);
        } else {
            fill.a = (uint8_t)(fill.a * fop);
        }
        fill_polys(ctx, p, fill, have_grad ? &grad : NULL, ctx->ctm, 0, 0, 1e9f, 1e9f);
    }
    if (strokable && has_stroke) {
        SvgGrad sgrad;
        int have_sgrad = resolve_gradient(ctx, n, "stroke", &sgrad, ux0, uy0, ux1, uy1);
        if (have_sgrad) {
            int i;
            for (i = 0; i < sgrad.stop_count; i++)
                sgrad.stops[i].color.a = (uint8_t)(sgrad.stops[i].color.a * sop);
            stroke_polys(ctx, p, stroke, ctx->stroke_w * mat_scale(ctx->ctm), &sgrad);
        } else {
            stroke.a = (uint8_t)(stroke.a * sop);
            stroke_polys(ctx, p, stroke, ctx->stroke_w * mat_scale(ctx->ctm), NULL);
        }
    }
}

static void draw_element(SvgCtx *ctx, HpeDomNode *n, Mat ctm);

static void draw_children(SvgCtx *ctx, HpeDomNode *parent, Mat ctm)
{
    HpeDomNode *n;
    for (n = parent->first_child; n; n = n->next_sibling) {
        if (n->type != HPE_NODE_ELEMENT)
            continue;
        draw_element(ctx, n, ctm);
    }
}

static void draw_element(SvgCtx *ctx, HpeDomNode *n, Mat ctm)
{
    const char *tag = n->tag;
    SvgCtx sub;

    if (!tag)
        return;
    if (!strcmp(tag, "title") || !strcmp(tag, "desc") || !strcmp(tag, "defs") ||
        hpe_str_ieq(tag, "linearGradient") || hpe_str_ieq(tag, "radialGradient") || !strcmp(tag, "stop"))
        return;

    ctm = mat_mul(ctm, parse_transform(attr(n, "transform")));

    if (!strcmp(tag, "g") || !strcmp(tag, "svg")) {
        sub = *ctx;
        sub.ctm = ctm;
        ctx_apply_style(&sub, n);
        if (!strcmp(tag, "svg")) {
            float x = 0.0f, y = 0.0f;
            if (attr_num(n, "x", &x) || attr_num(n, "y", &y)) {
                Mat t = mat_identity();
                t.e = x;
                t.f = y;
                sub.ctm = mat_mul(sub.ctm, t);
            }
        }
        draw_children(&sub, n, sub.ctm);
        return;
    }
    if (!strcmp(tag, "use")) {
        const char *href = attr(n, "href");
        HpeDomNode *ref;
        char id[128];
        size_t len = 0;
        float x = 0.0f, y = 0.0f;
        const char *hash;
        if (!href)
            href = attr(n, "xlink:href");
        if (!href || ctx->use_depth > 6)
            return;
        hash = strchr(href, '#');
        if (!hash)
            return;
        hash++;
        while (hash[len] && len < sizeof(id) - 1) {
            id[len] = hash[len];
            len++;
        }
        id[len] = '\0';
        ref = find_by_id(ctx->root, id);
        if (!ref)
            return;
        attr_num(n, "x", &x);
        attr_num(n, "y", &y);
        sub = *ctx;
        sub.ctm = ctm;
        if (x != 0.0f || y != 0.0f) {
            Mat t = mat_identity();
            t.e = x;
            t.f = y;
            sub.ctm = mat_mul(sub.ctm, t);
        }
        ctx_apply_style(&sub, n);
        sub.use_depth = ctx->use_depth + 1;
        draw_element(&sub, ref, sub.ctm);
        return;
    }

    sub = *ctx;
    sub.ctm = ctm;
    ctx_apply_style(&sub, n);

    if (!strcmp(tag, "text")) {
        draw_text(&sub, n);
        return;
    }
    if (!strcmp(tag, "rect")) {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f, r = 0.0f;
        Polys p;
        attr_num(n, "x", &x);
        attr_num(n, "y", &y);
        attr_num(n, "width", &w);
        attr_num(n, "height", &h);
        attr_num(n, "rx", &r);
        if (!attr_num(n, "rx", &r))
            attr_num(n, "ry", &r);
        if (w <= 0.0f || h <= 0.0f)
            return;
        memset(&p, 0, sizeof(p));
        round_rect_path(&p, &sub, x, y, w, h, r);
        paint_shape(&sub, n, &p, x, y, x + w, y + h, 1, 1);
        polys_free(&p);
        return;
    }
    if (!strcmp(tag, "circle") || !strcmp(tag, "ellipse")) {
        float cx = 0.0f, cy = 0.0f, rx = 0.0f, ry = 0.0f;
        Polys p;
        attr_num(n, "cx", &cx);
        attr_num(n, "cy", &cy);
        if (!strcmp(tag, "circle")) {
            attr_num(n, "r", &rx);
            ry = rx;
        } else {
            attr_num(n, "rx", &rx);
            attr_num(n, "ry", &ry);
        }
        if (rx <= 0.0f || ry <= 0.0f)
            return;
        memset(&p, 0, sizeof(p));
        ellipse_path(&p, &sub, cx, cy, rx, ry);
        paint_shape(&sub, n, &p, cx - rx, cy - ry, cx + rx, cy + ry, 1, 1);
        polys_free(&p);
        return;
    }
    if (!strcmp(tag, "line")) {
        float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
        Polys p;
        SubPath *sp;
        float dx, dy;
        attr_num(n, "x1", &x1);
        attr_num(n, "y1", &y1);
        attr_num(n, "x2", &x2);
        attr_num(n, "y2", &y2);
        memset(&p, 0, sizeof(p));
        sp = polys_new(&p);
        mat_apply(sub.ctm, x1, y1, &dx, &dy);
        sub_push(sp, dx, dy);
        mat_apply(sub.ctm, x2, y2, &dx, &dy);
        sub_push(sp, dx, dy);
        paint_shape(&sub, n, &p, fminf(x1, x2), fminf(y1, y2), fmaxf(x1, x2), fmaxf(y1, y2), 0, 1);
        polys_free(&p);
        return;
    }
    if (!strcmp(tag, "polygon") || !strcmp(tag, "polyline")) {
        Polys p;
        int closed = !strcmp(tag, "polygon");
        const char *pts = attr(n, "points");
        float minx = 0.0f, miny = 0.0f, maxx = 0.0f, maxy = 0.0f;
        memset(&p, 0, sizeof(p));
        points_bbox(pts, &minx, &miny, &maxx, &maxy);
        points_path(&p, &sub, pts, closed);
        if (p.count > 0)
            paint_shape(&sub, n, &p, minx, miny, maxx, maxy, closed, 1);
        polys_free(&p);
        return;
    }
    if (!strcmp(tag, "path")) {
        Polys p;
        float bx0, by0, bx1, by1;
        memset(&p, 0, sizeof(p));
        build_path(&p, &sub, attr(n, "d"));
        path_bbox(attr(n, "d"), &bx0, &by0, &bx1, &by1);
        if (p.count > 0)
            paint_shape(&sub, n, &p, bx0, by0, bx1, by1, 1, 1);
        polys_free(&p);
        return;
    }
    /* unknown element: recurse so nested content is not lost */
    draw_children(&sub, n, sub.ctm);
}

void hpe_svg_paint(HpePixmap *pm, HpeBox *box, float y_dev)
{
    SvgCtx ctx;
    HpeDomNode *node;
    float vw = 0.0f, vh = 0.0f;
    HpeRect vp;
    int sx0, sy0, sx1, sy1;

    if (!pm || !pm->pixels || !box || !box->node)
        return;
    node = box->node;
    if (strcmp(node->tag, "svg") != 0)
        return;

    memset(&ctx, 0, sizeof(ctx));
    ctx.pm = pm;
    ctx.ctm = mat_identity();
    ctx.ctm.e = box->content_box.x;
    ctx.ctm.f = y_dev + (box->content_box.y - box->border_box.y);
    ctx.vp_w = box->content_box.w;
    ctx.vp_h = box->content_box.h;
    ctx.stroke_w = 1.0f;
    ctx.has_stroke = 0;
    ctx.has_fill = 0;
    ctx.fill_opacity = 1.0f;
    ctx.stroke_opacity = 1.0f;
    ctx.root = node;

    if (getenv("HOPE_TRACE_SVG"))
        fprintf(stderr, "svg box=(%.1f,%.1f %.1fx%.1f) viewBox=%s\n", box->content_box.x,
                box->content_box.y, ctx.vp_w, ctx.vp_h,
                attr(node, "viewBox") ? attr(node, "viewBox") : "-");

    if (attr(node, "viewBox")) {
        float a, b, c, d;
        if (sscanf(attr(node, "viewBox"), "%f %f %f %f", &a, &b, &c, &d) == 4 && c > 0.0f &&
            d > 0.0f) {
            float sx, sy, scale;
            vw = c;
            vh = d;
            sx = ctx.vp_w / vw;
            sy = ctx.vp_h / vh;
            scale = sx < sy ? sx : sy;
            if (scale <= 0.0f)
                scale = 1.0f;
            ctx.ctm.e += (ctx.vp_w - vw * scale) * 0.5f - a * scale;
            ctx.ctm.f += (ctx.vp_h - vh * scale) * 0.5f - b * scale;
            ctx.ctm.a = scale;
            ctx.ctm.d = scale;
        }
    }

    /* root <svg> style attributes apply to the whole subtree */
    ctx_apply_style(&ctx, node);
    {
        const char *vb = attr(node, "viewBox");
        HPE_UNUSED(vb);
    }

    /* clip to the SVG viewport *and* to the damage rect that is already active
     * (a partial repaint must not touch pixels outside it, or translucent
     * strokes would be blended twice and darken on every scroll) */
    vp = hpe_rect(ctx.ctm.e, ctx.ctm.f, ctx.vp_w, ctx.vp_h);
    sx0 = pm->clip_x0;
    sy0 = pm->clip_y0;
    sx1 = pm->clip_x1;
    sy1 = pm->clip_y1;
    hpe_pixmap_set_clip(pm, vp);
    hpe_pixmap_intersect_clip(pm, hpe_rect((float)sx0, (float)sy0, (float)(sx1 - sx0),
                                           (float)(sy1 - sy0)));
    draw_children(&ctx, node, ctx.ctm);
    pm->clip_x0 = sx0;
    pm->clip_y0 = sy0;
    pm->clip_x1 = sx1;
    pm->clip_y1 = sy1;
    hpe_free(ctx.cov_scratch);
}

/* ---------------- hit testing ---------------- */

static float attr_f(HpeDomNode *n, const char *name, float def)
{
    float f;
    return attr_num(n, name, &f) ? f : def;
}

static int point_in_polys(Polys *p, float x, float y)
{
    int inside = 0, i, k;
    for (i = 0; i < p->count; i++) {
        SubPath *sp = &p->subs[i];
        int n = sp->count;
        for (k = 0; k < n; k++) {
            int k2 = sp->closed ? (k + 1) % n : k + 1;
            float ax, ay, bx, by;
            if (k2 >= n)
                break;
            ax = sp->pts[k * 2];
            ay = sp->pts[k * 2 + 1];
            bx = sp->pts[k2 * 2];
            by = sp->pts[k2 * 2 + 1];
            if (((ay > y) != (by > y)) && (x < (bx - ax) * (y - ay) / (by - ay) + ax))
                inside = !inside;
        }
    }
    return inside;
}

static HpeDomNode *hit_walk(SvgCtx *ctx, HpeDomNode *node, float dx, float dy)
{
    HpeDomNode *c, *best = NULL;
    const char *tag;

    if (!node || node->type != HPE_NODE_ELEMENT || !node->tag)
        return NULL;
    tag = node->tag;
    if (!strcmp(tag, "defs") || !strcmp(tag, "title") || !strcmp(tag, "desc"))
        return NULL;
    if (!strcmp(tag, "g") || !strcmp(tag, "svg")) {
        Mat saved = ctx->ctm;
        ctx->ctm = mat_mul(ctx->ctm, parse_transform(attr(node, "transform")));
        for (c = node->first_child; c; c = c->next_sibling) {
            HpeDomNode *r = hit_walk(ctx, c, dx, dy);
            if (r)
                best = r;
        }
        ctx->ctm = saved;
        return best;
    }
    if (!strcmp(tag, "use")) {
        const char *href = attr(node, "href");
        HpeDomNode *ref;
        char id[128];
        size_t len = 0;
        const char *hash;
        Mat saved = ctx->ctm;
        if (!href)
            href = attr(node, "xlink:href");
        if (!href)
            return NULL;
        hash = strchr(href, '#');
        if (!hash)
            return NULL;
        hash++;
        while (hash[len] && len < sizeof(id) - 1) {
            id[len] = hash[len];
            len++;
        }
        id[len] = '\0';
        ref = find_by_id(ctx->root, id);
        if (!ref)
            return NULL;
        ctx->ctm = mat_mul(ctx->ctm, parse_transform(attr(node, "transform")));
        {
            float ux = attr_f(node, "x", 0.0f), uy = attr_f(node, "y", 0.0f);
            Mat t = mat_identity();
            t.e = ux;
            t.f = uy;
            ctx->ctm = mat_mul(ctx->ctm, t);
        }
        best = hit_walk(ctx, ref, dx, dy);
        ctx->ctm = saved;
        return best;
    }

    /* shapes */
    ctx->ctm = mat_mul(ctx->ctm, parse_transform(attr(node, "transform")));
    {
        /* convert device point into this element's user space */
        Mat inv = mat_invert(ctx->ctm);
        float ux, uy;
        mat_apply(inv, dx, dy, &ux, &uy);

        if (!strcmp(tag, "rect")) {
            float x = attr_f(node, "x", 0.0f), y = attr_f(node, "y", 0.0f);
            float w = attr_f(node, "width", 0.0f), h = attr_f(node, "height", 0.0f);
            if (w > 0.0f && h > 0.0f && ux >= x && ux <= x + w && uy >= y && uy <= y + h)
                return node;
        } else if (!strcmp(tag, "circle") || !strcmp(tag, "ellipse")) {
            float cx = attr_f(node, "cx", 0.0f), cy = attr_f(node, "cy", 0.0f);
            float rx, ry;
            if (!strcmp(tag, "circle")) {
                rx = ry = attr_f(node, "r", 0.0f);
            } else {
                rx = attr_f(node, "rx", 0.0f);
                ry = attr_f(node, "ry", 0.0f);
            }
            if (rx > 0.0f && ry > 0.0f) {
                float nx = (ux - cx) / rx, ny = (uy - cy) / ry;
                if (nx * nx + ny * ny <= 1.0f)
                    return node;
            }
        } else if (!strcmp(tag, "line")) {
            float x1 = attr_f(node, "x1", 0.0f), y1 = attr_f(node, "y1", 0.0f);
            float x2 = attr_f(node, "x2", 0.0f), y2 = attr_f(node, "y2", 0.0f);
            float vx = x2 - x1, vy = y2 - y1;
            float len2 = vx * vx + vy * vy;
            float t = len2 > 0.0f ? ((ux - x1) * vx + (uy - y1) * vy) / len2 : 0.0f;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            {
                float px = x1 + t * vx, py = y1 + t * vy;
                if ((ux - px) * (ux - px) + (uy - py) * (uy - py) <= 9.0f)
                    return node;
            }
        } else if (!strcmp(tag, "polygon") || !strcmp(tag, "polyline")) {
            Polys pp;
            SvgCtx tmp = *ctx;
            int hit;
            memset(&pp, 0, sizeof(pp));
            tmp.cov_scratch = NULL;
            tmp.cov_cap = 0;
            tmp.ctm = mat_identity();
            points_path(&pp, &tmp, attr(node, "points"), !strcmp(tag, "polygon"));
            hit = point_in_polys(&pp, ux, uy);
            polys_free(&pp);
            if (hit)
                return node;
        } else if (!strcmp(tag, "path")) {
            Polys pp;
            SvgCtx tmp = *ctx;
            int hit;
            memset(&pp, 0, sizeof(pp));
            tmp.cov_scratch = NULL;
            tmp.cov_cap = 0;
            tmp.ctm = mat_identity();
            build_path(&pp, &tmp, attr(node, "d"));
            hit = point_in_polys(&pp, ux, uy);
            polys_free(&pp);
            if (hit)
                return node;
        } else if (!strcmp(tag, "text")) {
            float x = attr_f(node, "x", 0.0f), y = attr_f(node, "y", 0.0f);
            float fs = attr_f(node, "font-size", 12.0f);
            if (ux >= x - 2.0f && uy >= y - fs && uy <= y + fs * 0.4f)
                return node;
        }
    }
    return NULL;
}

HpeDomNode *hpe_svg_hit_test(HpeBox *box, float x, float y)
{
    SvgCtx ctx;
    HpeDomNode *node;
    Mat m;

    if (!box || !box->node)
        return NULL;
    node = box->node;
    if (!node->tag || strcmp(node->tag, "svg") != 0)
        return NULL;

    memset(&ctx, 0, sizeof(ctx));
    ctx.root = node;
    m = mat_identity();
    m.e = box->content_box.x;
    m.f = box->content_box.y;
    if (attr(node, "viewBox")) {
        float a, b, c, d;
        if (sscanf(attr(node, "viewBox"), "%f %f %f %f", &a, &b, &c, &d) == 4 && c > 0.0f &&
            d > 0.0f) {
            float sx = box->content_box.w / c, sy = box->content_box.h / d;
            float scale = sx < sy ? sx : sy;
            if (scale <= 0.0f)
                scale = 1.0f;
            m.e += (box->content_box.w - c * scale) * 0.5f - a * scale;
            m.f += (box->content_box.h - d * scale) * 0.5f - b * scale;
            m.a = scale;
            m.d = scale;
        }
    }
    ctx.ctm = m;
    return hit_walk(&ctx, node, x, y);
}
