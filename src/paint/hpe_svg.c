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

/**
 * @brief 返回单位仿射矩阵 / Return the identity affine matrix
 *
 * @note 该矩阵为 {1,0,0,1,0,0}，点映射为恒等 / The matrix is `{1,0,0,1,0,0}`, mapping points unchanged
 */
static Mat mat_identity(void)
{
    Mat m = {1, 0, 0, 1, 0, 0};
    return m;
}

/**
 * @brief 矩阵相乘 / Multiply two affine matrices
 *
 * @param[in]     A 左矩阵 / Left matrix
 * @param[in]     B 右矩阵 / Right matrix
 *
 * @return 乘积 A*B / The product `A*B`
 *
 * @note 结果等价于先应用 B 再应用 A / The result applies B first and then A
 */
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

/**
 * @brief 用矩阵变换一个点 / Transform a point by a matrix
 *
 * @param[in]     m 仿射矩阵 / Affine matrix
 * @param[in]     x 输入 X / Input X
 * @param[in]     y 输入 Y / Input Y
 * @param[out]    ox 变换后 X / Transformed X
 * @param[out]    oy 变换后 Y / Transformed Y
 */
static void mat_apply(Mat m, float x, float y, float *ox, float *oy)
{
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}

/**
 * @brief 求仿射矩阵的逆 / Invert an affine matrix
 *
 * @param[in]     m 仿射矩阵 / Affine matrix
 *
 * @return 逆矩阵；行列式接近 0 时返回单位矩阵 / The inverse, or the identity matrix when the determinant is near zero
 */
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

/**
 * @brief 求矩阵的名义均匀缩放系数 / Nominal uniform scale factor of a matrix
 *
 * @param[in]     m 仿射矩阵 / Affine matrix
 *
 * @return 缩放系数，下限钳到 1.0 / Scale factor, floored at 1.0
 *
 * @note 用于描边宽度与字号 / Used for stroke widths and font sizes
 */
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

/**
 * @brief 按覆盖率混合一个设备像素 / Blend one device pixel with a coverage value
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     x 设备 X / Device X
 * @param[in]     y 设备 Y / Device Y
 * @param[in]     c 源颜色 / Source colour
 * @param[in]     cov 覆盖率 0..1 / Coverage in 0..1
 *
 * @note 超出裁剪区、图像外或覆盖率非正的像素直接跳过 / Pixels outside the clip, outside the image, or with non-positive coverage are skipped
 */

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

/**
 * @brief 求渐变（或纯色回退）在设备像素处的颜色 / Resolve the colour of a gradient (or a flat fallback) at a device pixel
 *
 * @param[in]     grad 渐变描述，可为 `NULL` / Gradient description, may be `NULL`
 * @param[in]     ctm 用户空间到设备空间的矩阵 / User-to-device matrix
 * @param[in]     flat 无渐变时的纯色 / Flat colour used when there is no gradient
 * @param[in]     px 设备 X / Device X
 * @param[in]     py 设备 Y / Device Y
 *
 * @return 该像素处的颜色 / The colour at that pixel
 *
 * @note 线性渐变按沿轴的投影参数采样，径向渐变按到中心的距离采样 / A linear gradient samples the axial projection parameter; a radial one samples the distance to the centre
 */
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

/**
 * @brief 以胶囊形描边一条线段 / Stroke one segment as a capsule
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     x0 起点设备 X / Start device X
 * @param[in]     y0 起点设备 Y / Start device Y
 * @param[in]     x1 终点设备 X / End device X
 * @param[in]     y1 终点设备 Y / End device Y
 * @param[in]     width 描边宽度（设备像素）/ Stroke width in device pixels
 * @param[in]     c 纯色回退 / Flat colour fallback
 * @param[in]     grad 渐变描述，可为 `NULL` / Gradient description, may be `NULL`
 * @param[in]     ctm 用户空间到设备空间的矩阵 / User-to-device matrix
 *
 * @note 每条线段各自绘制，端点为圆头；宽度非正时直接返回 / Each segment is drawn separately with round caps; returns early when the width is not positive
 */
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

/**
 * @brief 向子路径追加一个顶点 / Append a vertex to a subpath
 *
 * @param[in,out] sp 子路径 / Subpath
 * @param[in]     x X 坐标 / X coordinate
 * @param[in]     y Y 坐标 / Y coordinate
 *
 * @note 必要时按倍增扩容顶点数组 / Grows the vertex array by doubling when needed
 */
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

/**
 * @brief 在多边形列表中新建一个空子路径 / Create an empty subpath in a polygon list
 *
 * @param[in,out] p 多边形列表 / Polygon list
 *
 * @return 新分配的子路径 / The newly allocated subpath
 */
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

/**
 * @brief 释放多边形列表及其全部子路径 / Free a polygon list and all its subpaths
 *
 * @param[in,out] p 多边形列表 / Polygon list
 *
 * @note 释放后把列表重置为空 / The list is reset to empty afterwards
 */
static void polys_free(Polys *p)
{
    int i;
    for (i = 0; i < p->count; i++)
        hpe_free(p->subs[i].pts);
    hpe_free(p->subs);
    p->subs = NULL;
    p->count = p->cap = 0;
}

/**
 * @brief 把一段水平覆盖按像素分箱累加 / Distribute a horizontal coverage span across pixel bins
 *
 * @param[in,out] cov 逐像素覆盖率数组 / Per-pixel coverage array
 * @param[in]     x0 数组首列的设备 X / Device X of the array's first column
 * @param[in]     x1 数组列数 / Number of columns
 * @param[in]     a 区间起点 X / Span start X
 * @param[in]     b 区间终点 X / Span end X
 * @param[in]     w 该段的权重 / Weight of the span
 *
 * @note 区间为空（b<=a）时无操作 / No-op when the span is empty (`b <= a`)
 */
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
/**
 * @brief 求所有子路径的设备包围盒并夹紧到裁剪与限界 / Device bounding box of every subpath, clipped to the clip rect and a caller limit
 *
 * @param[in]     p 多边形列表 / Polygon list
 * @param[in]     pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     bx0 限界左 / Limit left
 * @param[in]     by0 限界上 / Limit top
 * @param[in]     bx1 限界右 / Limit right
 * @param[in]     by1 限界下 / Limit bottom
 * @param[out]    ox0 裁剪后左边界 / Clipped left edge
 * @param[out]    oy0 裁剪后上边界 / Clipped top edge
 * @param[out]    ox1 裁剪后右边界 / Clipped right edge
 * @param[out]    oy1 裁剪后下边界 / Clipped bottom edge
 */
static void polys_bbox_clipped(Polys *p, HpePixmap *pm, float bx0, float by0, float bx1, float by1,
                               int *ox0, int *oy0, int *ox1, int *oy1)
{
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    int i;
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
    *ox0 = (int)floorf(minx);
    *ox1 = (int)ceilf(maxx);
    *oy0 = (int)floorf(miny);
    *oy1 = (int)ceilf(maxy);
    if (*ox0 < pm->clip_x0)
        *ox0 = pm->clip_x0;
    if (*oy0 < pm->clip_y0)
        *oy0 = pm->clip_y0;
    if (*ox1 > pm->clip_x1)
        *ox1 = pm->clip_x1;
    if (*oy1 > pm->clip_y1)
        *oy1 = pm->clip_y1;
}

/**
 * @brief 收集所有边与水平线 y = sy 的交点 / Gather the x-intersections of every edge with the horizontal line `y = sy`
 *
 * @param[in]     p 多边形列表 / Polygon list
 * @param[in]     sy 扫描线 y / Scanline y
 * @param[in,out] pxs 交点数组，跨调用复用 / Intersection array, reused across calls
 * @param[out]    pnx 交点个数 / Number of intersections
 * @param[in,out] pncap 交点数组容量 / Intersection array capacity
 */
static void polys_scanline(Polys *p, float sy, float **pxs, int *pnx, int *pncap)
{
    float *xs = *pxs;
    int nx = 0, ncap = *pncap, i;
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
    *pxs = xs;
    *pnx = nx;
    *pncap = ncap;
}

/**
 * @brief 对浮点数组做升序插入排序 / Sort a float array ascending (insertion sort)
 *
 * @param[in,out] xs 待排序数组 / Array to sort
 * @param[in]     nx 元素个数 / Element count
 */
static void sort_floats(float *xs, int nx)
{
    int i;
    for (i = 1; i < nx; i++) {
        float key = xs[i];
        int j = i - 1;
        while (j >= 0 && xs[j] > key) {
            xs[j + 1] = xs[j];
            j--;
        }
        xs[j + 1] = key;
    }
}

/**
 * @brief 求线性渐变在设备空间的原点、方向与长度平方 / Device-space origin, direction and squared length of a linear gradient
 *
 * @param[in]     ctm 用户空间到设备空间的矩阵 / User-to-device matrix
 * @param[in]     grad 渐变描述 / Gradient description
 * @param[out]    lin 原点与方向（lin[0..1] 为原点，lin[2..3] 为方向）/ Origin and direction: `lin[0..1]` origin, `lin[2..3]` direction
 * @param[out]    len2 方向长度的平方 / Squared length of the direction
 */
static void grad_linear_proj(Mat ctm, const SvgGrad *grad, float lin[4], float *len2)
{
    float gx0, gy0, gx1, gy1, dx, dy;
    mat_apply(ctm, grad->x1, grad->y1, &gx0, &gy0);
    mat_apply(ctm, grad->x2, grad->y2, &gx1, &gy1);
    dx = gx1 - gx0;
    dy = gy1 - gy0;
    lin[0] = gx0;
    lin[1] = gy0;
    lin[2] = dx;
    lin[3] = dy;
    *len2 = dx * dx + dy * dy;
}

/**
 * @brief 求径向渐变在设备空间的中心与半径 / Device-space centre and radius of a radial gradient
 *
 * @param[in]     ctm 用户空间到设备空间的矩阵 / User-to-device matrix
 * @param[in]     grad 渐变描述 / Gradient description
 * @param[out]    pcx 中心设备 X / Centre device X
 * @param[out]    pcy 中心设备 Y / Centre device Y
 * @param[out]    pr 半径（已缩放）/ Radius, scaled
 */
static void grad_radial_proj(Mat ctm, const SvgGrad *grad, float *pcx, float *pcy, float *pr)
{
    mat_apply(ctm, grad->cx, grad->cy, pcx, pcy);
    *pr = grad->r * mat_scale(ctm);
}

/**
 * @brief 求设备像素中心处的填充颜色 / Resolve the fill colour at a device pixel centre
 *
 * @param[in]     grad 渐变描述，可为 `NULL` / Gradient description, may be `NULL`
 * @param[in]     color 纯色回退 / Flat colour fallback
 * @param[in]     dcx 设备像素中心 X / Device pixel centre X
 * @param[in]     dcy 设备像素中心 Y / Device pixel centre Y
 * @param[in]     lin 线性渐变原点与方向 / Linear gradient origin and direction
 * @param[in]     len2 线性渐变方向长度平方 / Squared direction length of the linear gradient
 * @param[in]     rcx 径向渐变中心 X / Radial gradient centre X
 * @param[in]     rcy 径向渐变中心 Y / Radial gradient centre Y
 * @param[in]     rr 径向渐变半径 / Radial gradient radius
 *
 * @return 该处的颜色 / The colour at that point
 */
static HpeColor fill_grad_color(const SvgGrad *grad, HpeColor color, float dcx, float dcy,
                                const float *lin, float len2, float rcx, float rcy, float rr)
{
    HpeColor c;
    float t;
    if (grad && grad->kind == 1) {
        t = len2 > 1e-6f ? ((dcx - lin[0]) * lin[2] + (dcy - lin[1]) * lin[3]) / len2 : 0.0f;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        hpe_sample_stops(grad->stops, grad->stop_count, t, &c);
        return c;
    }
    if (grad && grad->kind == 2) {
        float dx = dcx - rcx, dy = dcy - rcy;
        t = rr > 1e-6f ? sqrtf(dx * dx + dy * dy) / rr : 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        hpe_sample_stops(grad->stops, grad->stop_count, t, &c);
        return c;
    }
    return color;
}

/**
 * @brief 对所有子路径做奇偶规则扫描线填充 / Even-odd scanline fill of every subpath
 *
 * @param[in,out] ctx SVG 上下文 / SVG context
 * @param[in]     p 多边形列表 / Polygon list
 * @param[in]     color 纯色回退 / Flat colour fallback
 * @param[in]     grad 渐变描述，可为 `NULL` / Gradient description, may be `NULL`
 * @param[in]     ctm 用户空间到设备空间的矩阵 / User-to-device matrix
 * @param[in]     bx0 限界左 / Limit left
 * @param[in]     by0 限界上 / Limit top
 * @param[in]     bx1 限界右 / Limit right
 * @param[in]     by1 限界下 / Limit bottom
 *
 * @note 每行取四个子扫描线并按 0.25 权重累加覆盖率；支持纯色与渐变 / Each row uses four sub-scanlines accumulated with 0.25 weight; supports solid colour and gradients
 */
static void fill_polys(SvgCtx *ctx, Polys *p, HpeColor color, const SvgGrad *grad, Mat ctm,
                       float bx0, float by0, float bx1, float by1)
{
    HpePixmap *pm = ctx->pm;
    int x0, y0, x1, y1, py;
    float *cov;
    float lin[4] = {0, 0, 0, 0}, len2 = 0.0f, rcx = 0.0f, rcy = 0.0f, rr = 0.0f;
    int i, s;

    if (p->count == 0)
        return;
    polys_bbox_clipped(p, pm, bx0, by0, bx1, by1, &x0, &y0, &x1, &y1);
    if (x0 >= x1 || y0 >= y1)
        return;
    if (ctx->cov_cap < x1 - x0) {
        ctx->cov_cap = x1 - x0;
        ctx->cov_scratch =
                (float *)hpe_realloc(ctx->cov_scratch, (size_t)ctx->cov_cap * sizeof(float));
    }
    cov = ctx->cov_scratch;
    if (grad && grad->kind == 1)
        grad_linear_proj(ctm, grad, lin, &len2);
    else if (grad && grad->kind == 2)
        grad_radial_proj(ctm, grad, &rcx, &rcy, &rr);

    for (py = y0; py < y1; py++) {
        int px;
        float *xs = NULL;
        int nx = 0, ncap = 0;
        memset(cov, 0, (size_t)(x1 - x0) * sizeof(float));
        for (s = 0; s < 4; s++) {
            float sy = (float)py + ((float)s + 0.5f) * 0.25f;
            polys_scanline(p, sy, &xs, &nx, &ncap);
            sort_floats(xs, nx);
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
            c = fill_grad_color(grad, color, (float)(x0 + px) + 0.5f, (float)py + 0.5f, lin, len2,
                                rcx, rcy, rr);
            blend_px(pm, x0 + px, py, c, cvg);
        }
    }
}

/**
 * @brief 沿所有子路径逐段描边 / Stroke every subpath segment by segment
 *
 * @param[in,out] ctx SVG 上下文 / SVG context
 * @param[in]     p 多边形列表 / Polygon list
 * @param[in]     color 纯色回退 / Flat colour fallback
 * @param[in]     width 描边宽度 / Stroke width
 * @param[in]     grad 渐变描述，可为 `NULL` / Gradient description, may be `NULL`
 *
 * @note 闭合子路径额外补画首尾相连的一段以保持视觉连续 / A closed subpath also draws the wrap-around segment for visual continuity
 */
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

/**
 * @brief 读取非空属性值 / Read a non-empty attribute value
 *
 * @param[in]     n DOM 元素 / DOM element
 * @param[in]     name 属性名 / Attribute name
 *
 * @return 属性值；缺失或为空时返回 `NULL` / The value, or `NULL` when absent or empty
 */

static const char *attr(HpeDomNode *n, const char *name)
{
    const char *v = hpe_dom_attr(n, name);
    return (v && *v) ? v : NULL;
}

/**
 * @brief 把数值属性读入浮点数 / Parse a numeric attribute into a float
 *
 * @param[in]     n DOM 元素 / DOM element
 * @param[in]     name 属性名 / Attribute name
 * @param[out]    out 解析结果 / Parsed value
 *
 * @return 解析成功返回 1，否则 0 / 1 on success, else 0
 */
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

/**
 * @brief 解析颜色属性 / Parse a colour attribute
 *
 * @param[in]     n DOM 元素 / DOM element
 * @param[in]     name 属性名 / Attribute name
 * @param[out]    out 解析出的颜色 / Parsed colour
 *
 * @return 属性存在返回 1，否则 0 / 1 when the attribute exists, else 0
 *
 * @note `none` 得到全透明色；无法解析时回退为不透明黑 / `none` yields a fully transparent colour; an unparsable value falls back to opaque black
 */
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

/**
 * @brief 把元素上的绘制属性应用到上下文 / Apply a node's paint attributes to the context
 *
 * @param[in,out] ctx SVG 上下文 / SVG context
 * @param[in]     n DOM 元素 / DOM element
 *
 * @note 读取 fill、stroke、stroke-width、fill-opacity、stroke-opacity / Reads fill, stroke, stroke-width, fill-opacity and stroke-opacity
 */
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

/**
 * @brief 构造绕点旋转（rotate 三参数形式）的矩阵 / Build the matrix for a rotation about a point (3-argument `rotate`)
 *
 * @param[in]     ang_deg 旋转角度（度）/ Rotation angle in degrees
 * @param[in]     v 参数数组 / Argument array
 * @param[in]     vn 参数个数 / Argument count
 *
 * @return 旋转矩阵 / The rotation matrix
 *
 * @note 参数至少 3 个时以 v[1],v[2] 为旋转中心 / When at least three arguments are given, `v[1]` and `v[2]` are the rotation centre
 */
static Mat transform_rotate(float ang_deg, const float *v, int vn)
{
    float ang = ang_deg * 3.14159265f / 180.0f;
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
    return r;
}

/**
 * @brief 把变换名与参数映射为矩阵 / Map a transform name and its arguments to a matrix
 *
 * @param[in]     name 变换名 / Transform name
 * @param[in]     v 参数数组 / Argument array
 * @param[in]     vn 参数个数 / Argument count
 *
 * @return 对应的矩阵 / The corresponding matrix
 *
 * @note 支持 translate/scale/rotate/skewX/skewY/matrix；名称未知或参数不足时返回单位矩阵 / Supports translate, scale, rotate, skewX, skewY and matrix; returns the identity when the name is unknown or the argument count is short
 */
static Mat transform_from_name(const char *name, const float *v, int vn)
{
    Mat t = mat_identity();
    if (!strcmp(name, "translate") && vn >= 1) {
        t.e = v[0];
        t.f = vn >= 2 ? v[1] : 0.0f;
    } else if (!strcmp(name, "scale") && vn >= 1) {
        t.a = v[0];
        t.d = vn >= 2 ? v[1] : v[0];
    } else if (!strcmp(name, "rotate") && vn >= 1) {
        t = transform_rotate(v[0], v, vn);
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
    return t;
}

/**
 * @brief 读取变换名并前移游标 / Read a transform name and advance the cursor
 *
 * @param[in,out] pp 解析游标指针 / Parse cursor pointer
 * @param[out]    name 接收变换名 / Receives the transform name
 * @param[in]     cap `name` 缓冲区容量 / Capacity of the `name` buffer
 */
static void transform_read_name(const char **pp, char *name, size_t cap)
{
    const char *p = *pp;
    size_t n = 0;
    while (*p && (hpe_ascii_lower((unsigned char)*p) >= 'a' &&
                  hpe_ascii_lower((unsigned char)*p) <= 'z') && n + 1 < cap)
        name[n++] = *p++;
    name[n] = '\0';
    *pp = p;
}

/**
 * @brief 读取括号内最多 cap 个数值参数 / Read up to `cap` numeric arguments inside the parentheses
 *
 * @param[in,out] pp 解析游标指针 / Parse cursor pointer
 * @param[out]    v 接收参数值 / Receives the argument values
 * @param[in]     cap 参数数组容量 / Capacity of the argument array
 *
 * @return 实际解析到的参数个数 / Number of arguments actually parsed
 *
 * @note 会跳过逗号与空白，并至少前进到右括号之后 / Skips commas and whitespace and always advances past the closing parenthesis
 */
static int transform_read_args(const char **pp, float *v, int cap)
{
    const char *p = *pp;
    int vn = 0;
    while (*p && *p != ')' && vn < cap) {
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
    *pp = p;
    return vn;
}

/**
 * @brief 跳过一段括号参数（避免死循环）/ Skip a parenthesised argument list to avoid an endless loop
 *
 * @param[in]     p 起始指针 / Start pointer
 *
 * @return 右括号之后的指针 / Pointer past the closing parenthesis
 */
static const char *transform_skip_parens(const char *p)
{
    while (*p && *p != ')')
        p++;
    if (*p == ')')
        p++;
    return p;
}

/**
 * @brief 解析 transform 属性为累积矩阵 / Parse a `transform` attribute into an accumulated matrix
 *
 * @param[in]     s transform 属性文本 / The `transform` attribute text
 *
 * @return 累积后的变换矩阵 / The accumulated transform matrix
 *
 * @note 多个变换按出现顺序左乘累加；`s` 为 `NULL` 时返回单位矩阵 / Multiple transforms accumulate left to right; returns the identity when `s` is `NULL`
 */
static Mat parse_transform(const char *s)
{
    Mat acc = mat_identity();
    const char *p = s;

    if (!s)
        return acc;
    while (*p) {
        char name[16];
        float v[6];
        int vn;
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;
        transform_read_name(&p, name, sizeof(name));
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '(') {
            /* malformed: bail out to avoid an infinite loop */
            p = transform_skip_parens(p);
            continue;
        }
        p++;
        vn = transform_read_args(&p, v, 6);
        acc = mat_mul(acc, transform_from_name(name, v, vn));
    }
    return acc;
}

/**
 * @brief 在子树中按 id 查找元素 / Find an element by id within a subtree
 *
 * @param[in]     root 子树根 / Subtree root
 * @param[in]     id 目标 id / Target id
 *
 * @return 匹配的元素；未找到时返回 `NULL` / The matching element, or `NULL` when not found
 */

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

/**
 * @brief 解析渐变色标偏移 / Parse a gradient stop offset
 *
 * @param[in]     s 偏移文本 / Offset text
 *
 * @return 归一化偏移；带 `%` 时除以 100 / The normalised offset, divided by 100 when it carries a `%`
 */
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

/**
 * @brief 读取线性/径向渐变的几何属性 / Read the geometry attributes of a linear or radial gradient
 *
 * @param[in]     n 渐变元素 / Gradient element
 * @param[out]    g 渐变描述，被写入 / Gradient description, written to
 *
 * @note 线性读 x1/y1/x2/y2，径向读 cx/cy/r / Linear reads x1/y1/x2/y2; radial reads cx/cy/r
 */
static void parse_gradient_geometry(HpeDomNode *n, SvgGrad *g)
{
    const char *v;
    if (g->kind == 1) {
        if ((v = attr(n, "x1")))
            g->x1 = parse_offset(v);
        if ((v = attr(n, "y1")))
            g->y1 = parse_offset(v);
        if ((v = attr(n, "x2")))
            g->x2 = parse_offset(v);
        if ((v = attr(n, "y2")))
            g->y2 = parse_offset(v);
    } else {
        if ((v = attr(n, "cx")))
            g->cx = parse_offset(v);
        if ((v = attr(n, "cy")))
            g->cy = parse_offset(v);
        if ((v = attr(n, "r")))
            g->r = parse_offset(v);
    }
}

/**
 * @brief 把一个 `<stop>` 子元素追加到色标列表 / Append one `<stop>` child to the gradient's stop list
 *
 * @param[in]     c `<stop>` 元素 / The `<stop>` element
 * @param[out]    g 渐变描述 / Gradient description
 *
 * @note 读取 offset、stop-color、stop-opacity / Reads offset, stop-color and stop-opacity
 */
static void parse_gradient_stop(HpeDomNode *c, SvgGrad *g)
{
    HpeColor col = hpe_rgb(0, 0, 0);
    float off = 0.0f;
    const char *v = attr(c, "offset");
    if (v)
        off = parse_offset(v);
    if ((v = attr(c, "stop-color")))
        hpe_color_parse(v, &col);
    if ((v = attr(c, "stop-opacity")))
        col.a = (uint8_t)(col.a * strtod(v, NULL));
    g->stops[g->stop_count].pos = off;
    g->stops[g->stop_count].color = col;
    g->stop_count++;
}

/**
 * @brief 从 `<linearGradient>`/`<radialGradient>` 元素填充渐变描述 / Fill a gradient description from a `<linearGradient>`/`<radialGradient>` element
 *
 * @param[in]     n 渐变元素 / Gradient element
 * @param[out]    g 渐变描述 / Gradient description
 *
 * @return 有色标返回 1，否则 0 / 1 when the gradient has stops, else 0
 *
 * @note 标签不匹配时返回 0；色标最多收集 SVG_MAX_STOPS 个 / Returns 0 when the tag does not match; collects at most SVG_MAX_STOPS stops
 */
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
    parse_gradient_geometry(n, g);
    for (c = n->first_child; c; c = c->next_sibling) {
        if (c->type != HPE_NODE_ELEMENT || strcmp(c->tag, "stop"))
            continue;
        if (g->stop_count >= SVG_MAX_STOPS)
            break;
        parse_gradient_stop(c, g);
    }
    return g->stop_count > 0;
}

/**
 * @brief 把 `fill`/`stroke` 的 `url(#id)` 解析为渐变 / Resolve a `fill`/`stroke` `url(#id)` into a gradient
 *
 * @param[in,out] ctx SVG 上下文 / SVG context
 * @param[in]     n DOM 元素 / DOM element
 * @param[in]     which 属性名（fill 或 stroke）/ Attribute name, `fill` or `stroke`
 * @param[out]    out 解析出的渐变 / Resolved gradient
 * @param[in]     ux0 形状用户空间包围盒左 / Shape user-space bbox left
 * @param[in]     uy0 形状用户空间包围盒上 / Shape user-space bbox top
 * @param[in]     ux1 形状用户空间包围盒右 / Shape user-space bbox right
 * @param[in]     uy1 形状用户空间包围盒下 / Shape user-space bbox bottom
 *
 * @return 解析成功返回 1，否则 0 / 1 on success, else 0
 *
 * @note objectBoundingBox 单位下把分数坐标映射到形状的用户包围盒 / For objectBoundingBox units the fractional coordinates are mapped onto the shape's user bbox
 */
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

/**
 * @brief 开启新子路径并落到设备空间起点 / Open a new subpath at a device-space point
 *
 * @param[in,out] p 多边形列表 / Polygon list
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     ux 用户空间 X / User-space X
 * @param[in]     uy 用户空间 Y / User-space Y
 *
 * @note 用户坐标经 CTM 变换后再入队 / User coordinates are mapped by the CTM before being stored
 */
static void dev_move(Polys *p, SvgCtx *ctx, float ux, float uy)
{
    SubPath *sp = polys_new(p);
    float dx, dy;
    mat_apply(ctx->ctm, ux, uy, &dx, &dy);
    sub_push(sp, dx, dy);
}

/**
 * @brief 在当前子路径追加一条线段 / Append a line segment to the current subpath
 *
 * @param[in,out] p 多边形列表 / Polygon list
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     ux 用户空间 X / User-space X
 * @param[in]     uy 用户空间 Y / User-space Y
 *
 * @note 尚无子路径时退化为 dev_move / Degrades to dev_move when no subpath exists yet
 */
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

/**
 * @brief 把三次贝塞尔曲线展平为线段 / Flatten a cubic Bezier curve into line segments
 *
 * @param[in,out] p 多边形列表 / Polygon list
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     x0 起点 X / Start X
 * @param[in]     y0 起点 Y / Start Y
 * @param[in]     x1 第一控制点 X / First control point X
 * @param[in]     y1 第一控制点 Y / First control point Y
 * @param[in]     x2 第二控制点 X / Second control point X
 * @param[in]     y2 第二控制点 Y / Second control point Y
 * @param[in]     x3 终点 X / End X
 * @param[in]     y3 终点 Y / End Y
 *
 * @note 按控制多边形长度与缩放估算步数，上限 200 / The step count is estimated from the control polygon length and the scale, capped at 200
 */
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

/**
 * @brief 把二次贝塞尔曲线展平为线段 / Flatten a quadratic Bezier curve into line segments
 *
 * @param[in,out] p 多边形列表 / Polygon list
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     x0 起点 X / Start X
 * @param[in]     y0 起点 Y / Start Y
 * @param[in]     x1 控制点 X / Control point X
 * @param[in]     y1 控制点 Y / Control point Y
 * @param[in]     x2 终点 X / End X
 * @param[in]     y2 终点 Y / End Y
 *
 * @note 按控制多边形长度与缩放估算步数，上限 200 / The step count is estimated from the control polygon length and the scale, capped at 200
 */
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

/**
 * @brief 把椭圆弧的端点式参数化为中心式 / Endpoint-to-centre parameterisation of an SVG elliptical arc
 *
 * @param[in]     x0 起点 X / Start X
 * @param[in]     y0 起点 Y / Start Y
 * @param[in,out] rx X 半径，过大时被放大 / X radius, scaled up when too small
 * @param[in,out] ry Y 半径，过大时被放大 / Y radius, scaled up when too small
 * @param[in]     phi X 轴旋转角（度）/ X-axis rotation in degrees
 * @param[in]     large 大弧标志 / Large-arc flag
 * @param[in]     sweep 扫描方向标志 / Sweep flag
 * @param[in]     x1 终点 X / End X
 * @param[in]     y1 终点 Y / End Y
 * @param[out]    trig 旋转角的 cos/sin / Cosine and sine of the rotation angle
 * @param[out]    out_cx 中心 X / Centre X
 * @param[out]    out_cy 中心 Y / Centre Y
 * @param[out]    out_theta1 起始角（弧度）/ Start angle in radians
 * @param[out]    out_dtheta 扫过角（弧度）/ Swept angle in radians
 *
 * @note 按 SVG 规范 F.6.5 实现 / Follows the SVG spec's implementation notes F.6.5
 */
static void arc_center_params(float x0, float y0, float *rx, float *ry, float phi, int large,
                              int sweep, float x1, float y1, float trig[2], float *out_cx,
                              float *out_cy, float *out_theta1, float *out_dtheta)
{
    float cosp = cosf(phi * 3.14159265f / 180.0f);
    float sinp = sinf(phi * 3.14159265f / 180.0f);
    float dx2 = (x0 - x1) * 0.5f, dy2 = (y0 - y1) * 0.5f;
    float x1p = cosp * dx2 + sinp * dy2;
    float y1p = -sinp * dx2 + cosp * dy2;
    float rx2 = *rx * *rx, ry2 = *ry * *ry;
    float lam = (x1p * x1p) / rx2 + (y1p * y1p) / ry2;
    float cx1, cy1, ux, uy, vx, vy, dot, len, ang;

    if (lam > 1.0f) {
        float sc = sqrtf(lam);
        *rx *= sc;
        *ry *= sc;
        rx2 = *rx * *rx;
        ry2 = *ry * *ry;
    }
    {
        float num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
        float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
        float coef = den > 0.0f ? sqrtf(fmaxf(0.0f, num / den)) : 0.0f;
        float sign = (large != sweep) ? 1.0f : -1.0f;
        cx1 = coef * sign * (*rx * y1p / *ry);
        cy1 = coef * sign * (-*ry * x1p / *rx);
    }
    *out_cx = cosp * cx1 - sinp * cy1 + (x0 + x1) * 0.5f;
    *out_cy = sinp * cx1 + cosp * cy1 + (y0 + y1) * 0.5f;
    ux = (x1p - cx1) / *rx, uy = (y1p - cy1) / *ry;
    vx = (-x1p - cx1) / *rx, vy = (-y1p - cy1) / *ry;
    dot = ux * vx + uy * vy;
    len = sqrtf(ux * ux + uy * uy) * sqrtf(vx * vx + vy * vy);
    ang = acosf(fmaxf(-1.0f, fminf(1.0f, len > 0.0f ? dot / len : 0.0f)));
    *out_theta1 = acosf(fmaxf(-1.0f, fminf(1.0f, ux)));
    if (uy < 0.0f)
        *out_theta1 = -*out_theta1;
    if (ux * vy - uy * vx < 0.0f)
        ang = -ang;
    *out_dtheta = ang;
    trig[0] = cosp;
    trig[1] = sinp;
}

/**
 * @brief 把椭圆弧展平为线段 / Flatten an elliptical arc into line segments
 *
 * @param[in,out] p 多边形列表 / Polygon list
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     x0 起点 X / Start X
 * @param[in]     y0 起点 Y / Start Y
 * @param[in]     rx X 半径 / X radius
 * @param[in]     ry Y 半径 / Y radius
 * @param[in]     phi X 轴旋转角（度）/ X-axis rotation in degrees
 * @param[in]     large 大弧标志 / Large-arc flag
 * @param[in]     sweep 扫描方向标志 / Sweep flag
 * @param[in]     x1 终点 X / End X
 * @param[in]     y1 终点 Y / End Y
 *
 * @note 半径之一为 0 时退化为直线；步数按扫过角与半径估算，上限 200 / Degrades to a straight line when either radius is zero; the step count is estimated from the sweep and radius, capped at 200
 */
static void flatten_arc(Polys *p, SvgCtx *ctx, float x0, float y0, float rx, float ry, float phi,
                        int large, int sweep, float x1, float y1)
{
    float trig[2], cx, cy, theta1, dtheta, t;
    int steps, i;

    if (rx == 0.0f || ry == 0.0f) {
        dev_line(p, ctx, x1, y1);
        return;
    }
    rx = fabsf(rx);
    ry = fabsf(ry);
    arc_center_params(x0, y0, &rx, &ry, phi, large, sweep, x1, y1, trig, &cx, &cy, &theta1,
                      &dtheta);

    steps = (int)(fabsf(dtheta) * fmaxf(rx, ry) * mat_scale(ctx->ctm) / 3.0f) + 4;
    if (steps > 200)
        steps = 200;
    for (i = 1; i <= steps; i++) {
        float ex, ey, px, py;
        t = theta1 + dtheta * (float)i / (float)steps;
        ex = rx * cosf(t);
        ey = ry * sinf(t);
        px = trig[0] * ex - trig[1] * ey + cx;
        py = trig[1] * ex + trig[0] * ey + cy;
        dev_line(p, ctx, px, py);
    }
}

/* running state while walking an SVG path's `d` data */
typedef struct {
    Polys *p;
    SvgCtx *ctx;
    const char *q;
    char cmd;              /* last command letter seen; 0 = none */
    float cx, cy;          /* current point */
    float sx, sy;          /* current subpath start */
    float last_c1x, last_c1y; /* last cubic control point (for S) */
    float last_qx, last_qy;   /* last quadratic control point (for T) */
    int started;           /* a subpath has been opened */
} PathBuild;

/**
 * @brief 查路径命令所需的参数个数 / Look up the argument count a path command needs
 *
 * @param[in]     upper 大写命令字母 / Uppercased command letter
 *
 * @return 参数个数；未知命令返回 -1 / The argument count, or -1 for an unknown command
 */
static int path_cmd_argc(char upper)
{
    static const struct {
        char cmd;
        int argc;
    } table[] = {
        {'M', 2}, {'L', 2}, {'T', 2}, {'H', 1}, {'V', 1},
        {'C', 6}, {'S', 4}, {'Q', 4}, {'A', 7}, {'Z', 0},
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (table[i].cmd == upper)
            return table[i].argc;
    return -1;
}

/**
 * @brief 判断字符是否为路径分隔空白 / Test whether a character is a path separator
 *
 * @param[in]     c 字符 / Character
 *
 * @return 是空格/逗号/制表/换行返回 1 / 1 for space, comma, tab or newline
 */
static int path_isspace(char c)
{
    return c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r';
}

/**
 * @brief 跳过当前位置的分隔符 / Skip separators at the current position
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 */
static void path_skip_sep(PathBuild *pb)
{
    while (path_isspace(*pb->q))
        pb->q++;
}

/**
 * @brief 读取最多 want 个数字参数 / Read up to `want` numeric arguments
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 * @param[out]    a 接收参数值 / Receives the argument values
 * @param[in]     want 期望的参数个数 / Desired argument count
 *
 * @return 实际解析到的个数 / Number actually parsed
 */
static int path_read_args(PathBuild *pb, float *a, int want)
{
    int i, n = 0;
    for (i = 0; i < want; i++) {
        char *end = NULL;
        double v;
        path_skip_sep(pb);
        v = strtod(pb->q, &end);
        if (end == pb->q)
            break;
        a[n++] = (float)v;
        pb->q = end;
    }
    return n;
}

/**
 * @brief 闭合当前子路径 / Close the current subpath
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 *
 * @note 标记子路径闭合并把当前点移回子路径起点 / Marks the subpath closed and moves the current point back to the subpath start
 */
static void path_cmd_z(PathBuild *pb)
{
    if (pb->started) {
        SubPath *sp = &pb->p->subs[pb->p->count - 1];
        sp->closed = 1;
        pb->cx = pb->sx;
        pb->cy = pb->sy;
    }
    pb->cmd = 0;
}

/**
 * @brief 执行 M 移动命令 / Execute an `M` move command
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 * @param[in]     a 参数数组 / Argument array
 * @param[in]     rel 相对命令为 1 / 1 for a relative command
 *
 * @note 按规范，紧随的坐标对被视为隐式 lineto / Per the spec, following coordinate pairs are treated as implicit line-tos
 */
static void path_cmd_move(PathBuild *pb, float *a, int rel)
{
    float nx = rel ? pb->cx + a[0] : a[0];
    float ny = rel ? pb->cy + a[1] : a[1];
    pb->cx = pb->sx = nx;
    pb->cy = pb->sy = ny;
    dev_move(pb->p, pb->ctx, pb->cx, pb->cy);
    pb->started = 1;
    pb->cmd = rel ? 'l' : 'L';
}

/**
 * @brief 执行 L 直线命令 / Execute an `L` line command
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 * @param[in]     a 参数数组 / Argument array
 * @param[in]     rel 相对命令为 1 / 1 for a relative command
 *
 * @note 尚无子路径时以该点为起点 / Uses the point as the new subpath start when none exists
 */
static void path_cmd_line(PathBuild *pb, float *a, int rel)
{
    float nx = rel ? pb->cx + a[0] : a[0];
    float ny = rel ? pb->cy + a[1] : a[1];
    if (!pb->started) {
        dev_move(pb->p, pb->ctx, nx, ny);
        pb->sx = nx;
        pb->sy = ny;
        pb->started = 1;
    } else {
        dev_line(pb->p, pb->ctx, nx, ny);
    }
    pb->cx = nx;
    pb->cy = ny;
}

/**
 * @brief 执行 H 水平线命令 / Execute an `H` horizontal line command
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 * @param[in]     a 参数数组 / Argument array
 * @param[in]     rel 相对命令为 1 / 1 for a relative command
 */
static void path_cmd_h(PathBuild *pb, float *a, int rel)
{
    float nx = rel ? pb->cx + a[0] : a[0];
    dev_line(pb->p, pb->ctx, nx, pb->cy);
    pb->cx = nx;
}

/**
 * @brief 执行 V 垂直线命令 / Execute a `V` vertical line command
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 * @param[in]     a 参数数组 / Argument array
 * @param[in]     rel 相对命令为 1 / 1 for a relative command
 */
static void path_cmd_v(PathBuild *pb, float *a, int rel)
{
    float ny = rel ? pb->cy + a[0] : a[0];
    dev_line(pb->p, pb->ctx, pb->cx, ny);
    pb->cy = ny;
}

/**
 * @brief 执行 C 三次贝塞尔命令 / Execute a `C` cubic curve command
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 * @param[in]     a 参数数组 / Argument array
 * @param[in]     rel 相对命令为 1 / 1 for a relative command
 *
 * @note 记录第二控制点供后续 S 反射使用 / Records the second control point for a following `S` reflection
 */
static void path_cmd_cubic(PathBuild *pb, float *a, int rel)
{
    float x1 = rel ? pb->cx + a[0] : a[0], y1 = rel ? pb->cy + a[1] : a[1];
    float x2 = rel ? pb->cx + a[2] : a[2], y2 = rel ? pb->cy + a[3] : a[3];
    float x3 = rel ? pb->cx + a[4] : a[4], y3 = rel ? pb->cy + a[5] : a[5];
    flatten_cubic(pb->p, pb->ctx, pb->cx, pb->cy, x1, y1, x2, y2, x3, y3);
    pb->last_c1x = x2;
    pb->last_c1y = y2;
    pb->cx = x3;
    pb->cy = y3;
}

/**
 * @brief 执行 S 平滑三次贝塞尔命令 / Execute an `S` smooth cubic curve command
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 * @param[in]     a 参数数组 / Argument array
 * @param[in]     rel 相对命令为 1 / 1 for a relative command
 *
 * @note 首个控制点由前一控制点关于当前点反射得到 / The first control point is the previous control point reflected about the current point
 */
static void path_cmd_smooth_cubic(PathBuild *pb, float *a, int rel)
{
    float x2 = rel ? pb->cx + a[0] : a[0], y2 = rel ? pb->cy + a[1] : a[1];
    float x3 = rel ? pb->cx + a[2] : a[2], y3 = rel ? pb->cy + a[3] : a[3];
    float x1 = 2 * pb->cx - pb->last_c1x, y1 = 2 * pb->cy - pb->last_c1y;
    flatten_cubic(pb->p, pb->ctx, pb->cx, pb->cy, x1, y1, x2, y2, x3, y3);
    pb->last_c1x = x2;
    pb->last_c1y = y2;
    pb->cx = x3;
    pb->cy = y3;
}

/**
 * @brief 执行 Q 二次贝塞尔命令 / Execute a `Q` quadratic curve command
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 * @param[in]     a 参数数组 / Argument array
 * @param[in]     rel 相对命令为 1 / 1 for a relative command
 *
 * @note 记录控制点供后续 T 反射使用 / Records the control point for a following `T` reflection
 */
static void path_cmd_quad(PathBuild *pb, float *a, int rel)
{
    float x1 = rel ? pb->cx + a[0] : a[0], y1 = rel ? pb->cy + a[1] : a[1];
    float x2 = rel ? pb->cx + a[2] : a[2], y2 = rel ? pb->cy + a[3] : a[3];
    flatten_quad(pb->p, pb->ctx, pb->cx, pb->cy, x1, y1, x2, y2);
    pb->last_qx = x1;
    pb->last_qy = y1;
    pb->cx = x2;
    pb->cy = y2;
}

/**
 * @brief 执行 T 平滑二次贝塞尔命令 / Execute a `T` smooth quadratic curve command
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 * @param[in]     a 参数数组 / Argument array
 * @param[in]     rel 相对命令为 1 / 1 for a relative command
 *
 * @note 控制点由前一控制点关于当前点反射得到 / The control point is the previous control point reflected about the current point
 */
static void path_cmd_smooth_quad(PathBuild *pb, float *a, int rel)
{
    float x2 = rel ? pb->cx + a[0] : a[0], y2 = rel ? pb->cy + a[1] : a[1];
    float x1 = 2 * pb->cx - pb->last_qx, y1 = 2 * pb->cy - pb->last_qy;
    flatten_quad(pb->p, pb->ctx, pb->cx, pb->cy, x1, y1, x2, y2);
    pb->last_qx = x1;
    pb->last_qy = y1;
    pb->cx = x2;
    pb->cy = y2;
}

/**
 * @brief 执行 A 椭圆弧命令 / Execute an `A` elliptical arc command
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 * @param[in]     a 参数数组 / Argument array
 * @param[in]     rel 相对命令为 1 / 1 for a relative command
 */
static void path_cmd_arc(PathBuild *pb, float *a, int rel)
{
    float rx = a[0], ry = a[1], phi = a[2];
    int large = a[3] != 0.0f, sweep = a[4] != 0.0f;
    float x2 = rel ? pb->cx + a[5] : a[5], y2 = rel ? pb->cy + a[6] : a[6];
    flatten_arc(pb->p, pb->ctx, pb->cx, pb->cy, rx, ry, phi, large, sweep, x2, y2);
    pb->cx = x2;
    pb->cy = y2;
}

/**
 * @brief 跳过一个无法识别的命令 / Skip one unrecognised command
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 *
 * @note 若当前位置不含数字则只前进一个字符 / Advances just one character when the current position holds no number
 */
static void path_skip_unknown(PathBuild *pb)
{
    char *end = NULL;
    strtod(pb->q, &end);
    pb->q = (end == pb->q) ? pb->q + 1 : end;
}

/**
 * @brief 把一个命令的参数分派给对应处理器 / Dispatch one command's arguments to its handler
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 * @param[in]     upper 大写命令字母 / Uppercased command letter
 * @param[in]     a 参数数组 / Argument array
 * @param[in]     rel 相对命令为 1 / 1 for a relative command
 *
 * @note 默认分支按 A（椭圆弧）处理 / The default branch handles `A` (elliptical arc)
 */
static void path_apply(PathBuild *pb, char upper, float *a, int rel)
{
    switch (upper) {
    case 'M':
        path_cmd_move(pb, a, rel);
        break;
    case 'L':
        path_cmd_line(pb, a, rel);
        break;
    case 'H':
        path_cmd_h(pb, a, rel);
        break;
    case 'V':
        path_cmd_v(pb, a, rel);
        break;
    case 'C':
        path_cmd_cubic(pb, a, rel);
        break;
    case 'S':
        path_cmd_smooth_cubic(pb, a, rel);
        break;
    case 'Q':
        path_cmd_quad(pb, a, rel);
        break;
    case 'T':
        path_cmd_smooth_quad(pb, a, rel);
        break;
    default:
        path_cmd_arc(pb, a, rel);
        break;
    }
}

/**
 * @brief 消费当前命令的参数并执行 / Consume the current command's argument list and apply it
 *
 * @param[in,out] pb 路径构建状态 / Path build state
 *
 * @note 参数不足时清空游标以停止扫描；Z 直接闭合 / Clears the cursor to stop the scan when arguments run short; `Z` closes directly
 */
static void path_run_command(PathBuild *pb)
{
    char upper = (char)(pb->cmd >= 'a' && pb->cmd <= 'z' ? pb->cmd - 'a' + 'A' : pb->cmd);
    int rel = pb->cmd >= 'a' && pb->cmd <= 'z';
    int want = path_cmd_argc(upper);
    float a[7];
    int n;

    if (want < 0) {
        path_skip_unknown(pb);
        return;
    }
    if (want == 0) {
        path_cmd_z(pb);
        return;
    }
    n = path_read_args(pb, a, want);
    if (n < want) {
        pb->q = ""; /* truncated command: stop the scan */
        return;
    }
    path_apply(pb, upper, a, rel);
}

/**
 * @brief 解析路径 `d` 数据构建设备空间多边形 / Parse path `d` data into device-space polygons
 *
 * @param[out]    p 接收几何的多边形列表 / Polygon list receiving the geometry
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     d 路径数据 / Path data
 *
 * @note `d` 为 `NULL` 时无操作；命令字母切换当前命令，其余交给 path_run_command / No-op when `d` is `NULL`; command letters switch the current command and the rest goes to path_run_command
 */
static void build_path(Polys *p, SvgCtx *ctx, const char *d)
{
    PathBuild pb;
    if (!d)
        return;
    memset(&pb, 0, sizeof(pb));
    pb.p = p;
    pb.ctx = ctx;
    pb.q = d;
    while (*pb.q) {
        path_skip_sep(&pb);
        if (!*pb.q)
            break;
        if ((*pb.q >= 'A' && *pb.q <= 'Z') || (*pb.q >= 'a' && *pb.q <= 'z')) {
            pb.cmd = *pb.q++;
            continue;
        }
        if (!pb.cmd)
            break;
        path_run_command(&pb);
    }
}

/**
 * @brief 收集元素子树内的文本 / Collect the text inside an element's subtree
 *
 * @param[in]     n 元素 / Element
 * @param[out]    buf 接收文本的缓冲区 / Buffer receiving the text
 * @param[in]     cap 缓冲区容量 / Buffer capacity
 * @param[in,out] len 已写入长度，被累加 / Length written so far, accumulated
 *
 * @note 跳过 title/desc 子元素；缓冲区满时不再追加 / Skips `title`/`desc` children; stops appending when the buffer is full
 */

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

/* resolved text-drawing attributes for one <text> element */
typedef struct {
    char buf[1024];
    size_t len;
    float x, y, size, text_len;
    int weight;
    HpeColor color;
    int align;    /* 0 start, 1 middle, 2 end */
    int baseline; /* 0 none, 1 middle, 2 hanging, 3 after-edge */
} TextDraw;

/**
 * @brief 读取 `<text>` 的文本与锚点属性 / Read a `<text>` element's text and anchor attributes
 *
 * @param[in]     n `<text>` 元素 / The `<text>` element
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[out]    dt 文本绘制参数 / Text draw parameters
 *
 * @return 有文本返回 1，否则 0 / 1 when there is text, else 0
 *
 * @note 解析 x/y/font-size/textLength/font-weight/fill 与 text-anchor、dominant-baseline / Parses x/y/font-size/textLength/font-weight/fill plus text-anchor and dominant-baseline
 */
static int draw_text_read_attrs(HpeDomNode *n, SvgCtx *ctx, TextDraw *dt)
{
    const char *anchor, *baseline;
    float fw;
    HpeColor c;

    dt->buf[0] = '\0';
    dt->len = 0;
    collect_text(n, dt->buf, sizeof(dt->buf), &dt->len);
    if (dt->len == 0)
        return 0;
    attr_num(n, "x", &dt->x);
    attr_num(n, "y", &dt->y);
    attr_num(n, "font-size", &dt->size);
    attr_num(n, "textLength", &dt->text_len);
    if (attr_num(n, "font-weight", &fw))
        dt->weight = (int)fw;
    if (attr_color(n, "fill", &c))
        dt->color = c;

    anchor = attr(n, "text-anchor");
    dt->align = 0;
    if (anchor && !strcmp(anchor, "middle"))
        dt->align = 1;
    else if (anchor && !strcmp(anchor, "end"))
        dt->align = 2;

    baseline = attr(n, "dominant-baseline");
    if (!baseline)
        baseline = attr(n, "alignment-baseline");
    dt->baseline = 0; /* 0 none, 1 middle, 2 hanging, 3 after-edge */
    if (baseline) {
        if (!strcmp(baseline, "middle") || !strcmp(baseline, "central"))
            dt->baseline = 1;
        else if (!strcmp(baseline, "hanging") || !strcmp(baseline, "text-before-edge"))
            dt->baseline = 2;
        else if (!strcmp(baseline, "text-after-edge"))
            dt->baseline = 3;
    }
    return 1;
}

/**
 * @brief 按 textLength 拉伸 advanc 逐字绘制文本 / Draw text glyph by glyph with advances stretched to match `textLength`
 *
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     font 字体 / Font
 * @param[in]     buf 文本缓冲区 / Text buffer
 * @param[in]     len 文本字节长度 / Text byte length
 * @param[in]     dev_x 设备起点 X / Device start X
 * @param[in]     dev_y 设备基线 Y / Device baseline Y
 * @param[in]     w 自然宽度 / Natural width
 * @param[in]     text_len 目标 textLength / Target `textLength`
 * @param[in]     scale CTM 缩放系数 / CTM scale factor
 * @param[in]     color 文本颜色 / Text colour
 *
 * @note 对应 lengthAdjust="spacing" / Corresponds to `lengthAdjust="spacing"`
 */
static void draw_text_stretched(SvgCtx *ctx, HpeFont *font, const char *buf, int len, float dev_x,
                                float dev_y, float w, float text_len, float scale, HpeColor color)
{
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
}

/**
 * @brief 绘制一个 `<text>` 元素 / Draw a `<text>` element
 *
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     n `<text>` 元素 / The `<text>` element
 *
 * @note 锚点与基线关键字修正绘制起点；给出 textLength 且多字符时走拉伸绘制 / Anchor and baseline keywords adjust the origin; a `textLength` with more than one character goes through the stretched path
 */
static void draw_text(SvgCtx *ctx, HpeDomNode *n)
{
    TextDraw dt;
    HpeFont *font;
    float w, dev_x, dev_y, font_dev;
    float scale = mat_scale(ctx->ctm);

    if (!draw_text_read_attrs(n, ctx, &dt))
        return;
    font_dev = dt.size * scale;
    font = hpe_font_get(font_dev, dt.weight, HPE_FONT_SANS);
    if (!font)
        return;
    w = hpe_font_measure(font, dt.buf, (int)dt.len);
    mat_apply(ctx->ctm, dt.x, dt.y, &dev_x, &dev_y);

    if (dt.align == 1)
        dev_x -= w * 0.5f;
    else if (dt.align == 2)
        dev_x -= w;

    if (dt.baseline == 1)
        dev_y += hpe_font_ascent(font) * 0.5f - hpe_font_descent(font) * 0.25f;
    else if (dt.baseline == 2)
        dev_y += hpe_font_ascent(font);
    else if (dt.baseline == 3)
        dev_y -= hpe_font_descent(font);

    if (dt.text_len > 0.0f && dt.len > 1) {
        draw_text_stretched(ctx, font, dt.buf, (int)dt.len, dev_x, dev_y, w, dt.text_len, scale,
                            dt.color);
        return;
    }

    {
        const HpeTextMask *mask = hpe_font_mask(font, dt.buf, (int)dt.len);
        if (mask)
            hpe_draw_text_mask(ctx->pm, (int)floorf(dev_x), (int)floorf(dev_y), mask, dt.color);
    }
}

/**
 * @brief 构建椭圆/圆的多边形路径（用户空间）/ Build an ellipse/circle polygon path in user space
 *
 * @param[out]    p 多边形列表 / Polygon list
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     cx 中心 X / Centre X
 * @param[in]     cy 中心 Y / Centre Y
 * @param[in]     rx X 半径 / X radius
 * @param[in]     ry Y 半径 / Y radius
 *
 * @note 用 64 段采样逼近，路径闭合 / Approximated with 64 samples and closed
 */

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

/**
 * @brief 构建圆角矩形多边形路径 / Build a rounded-rectangle polygon path
 *
 * @param[out]    p 多边形列表 / Polygon list
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     x 左边界 X / Left X
 * @param[in]     y 上边界 Y / Top Y
 * @param[in]     w 宽度 / Width
 * @param[in]     h 高度 / Height
 * @param[in]     r 圆角半径 / Corner radius
 *
 * @note 半径夹紧到半宽/半高；每个圆角用 8 段采样，路径闭合 / The radius is clamped to half the width and height; each corner is sampled with 8 segments and the path is closed
 */
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

/**
 * @brief 从 points 属性构建多边形/折线 / Build a polygon/polyline from a `points` attribute
 *
 * @param[out]    p 多边形列表 / Polygon list
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     pts 顶点串 / Point list string
 * @param[in]     closed 闭合为 1 / 1 for a closed path
 *
 * @note `pts` 为 `NULL` 时无操作；顶点成对解析 / No-op when `pts` is `NULL`; points are parsed in pairs
 */
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

/**
 * @brief 求 points 串的用户空间包围盒 / User-space bbox of a `points` list
 *
 * @param[in]     pts 顶点串 / Point list string
 * @param[out]    x0 左边界 / Left edge
 * @param[out]    y0 上边界 / Top edge
 * @param[out]    x1 右边界 / Right edge
 * @param[out]    y1 下边界 / Bottom edge
 */
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

/**
 * @brief 求路径串的用户空间包围盒 / User-space bbox of a path string
 *
 * @param[in]     d 路径数据 / Path data
 * @param[out]    x0 左边界 / Left edge
 * @param[out]    y0 上边界 / Top edge
 * @param[out]    x1 右边界 / Right edge
 * @param[out]    y1 下边界 / Bottom edge
 *
 * @note 用单位矩阵求值，用于把 objectBoundingBox 渐变映射到路径上 / Evaluated with an identity matrix so objectBoundingBox gradients can map onto paths
 */
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

/**
 * @brief 按给定系数缩放所有色标的 alpha / Scale every gradient stop's alpha by a factor
 *
 * @param[in,out] grad 渐变描述 / Gradient description
 * @param[in]     op 透明度系数 / Opacity factor
 */
static void grad_scale_alpha(SvgGrad *grad, float op)
{
    int i;
    for (i = 0; i < grad->stop_count; i++)
        grad->stops[i].color.a = (uint8_t)(grad->stops[i].color.a * op);
}

/**
 * @brief 用纯色或渐变填充一个形状 / Fill a shape with a solid colour or a gradient
 *
 * @param[in,out] ctx SVG 上下文 / SVG context
 * @param[in]     n DOM 元素 / DOM element
 * @param[in]     p 多边形列表 / Polygon list
 * @param[in]     fill 纯色回退 / Flat colour fallback
 * @param[in]     fop 填充不透明度 / Fill opacity
 * @param[in]     ux0 用户空间包围盒左 / User-space bbox left
 * @param[in]     uy0 用户空间包围盒上 / User-space bbox top
 * @param[in]     ux1 用户空间包围盒右 / User-space bbox right
 * @param[in]     uy1 用户空间包围盒下 / User-space bbox bottom
 *
 * @note 渐变解析成功时优先使用并缩放其 alpha / When the gradient resolves it is used and its alpha is scaled
 */
static void paint_shape_fill(SvgCtx *ctx, HpeDomNode *n, Polys *p, HpeColor fill, float fop,
                             float ux0, float uy0, float ux1, float uy1)
{
    SvgGrad grad;
    int have_grad = resolve_gradient(ctx, n, "fill", &grad, ux0, uy0, ux1, uy1);
    if (have_grad)
        grad_scale_alpha(&grad, fop);
    else
        fill.a = (uint8_t)(fill.a * fop);
    fill_polys(ctx, p, fill, have_grad ? &grad : NULL, ctx->ctm, 0, 0, 1e9f, 1e9f);
}

/**
 * @brief 用纯色或渐变描边一个形状 / Stroke a shape with a solid colour or a gradient
 *
 * @param[in,out] ctx SVG 上下文 / SVG context
 * @param[in]     n DOM 元素 / DOM element
 * @param[in]     p 多边形列表 / Polygon list
 * @param[in]     stroke 纯色回退 / Flat colour fallback
 * @param[in]     sop 描边不透明度 / Stroke opacity
 * @param[in]     ux0 用户空间包围盒左 / User-space bbox left
 * @param[in]     uy0 用户空间包围盒上 / User-space bbox top
 * @param[in]     ux1 用户空间包围盒右 / User-space bbox right
 * @param[in]     uy1 用户空间包围盒下 / User-space bbox bottom
 *
 * @note 描边宽度按 CTM 缩放 / The stroke width is scaled by the CTM
 */
static void paint_shape_stroke(SvgCtx *ctx, HpeDomNode *n, Polys *p, HpeColor stroke, float sop,
                               float ux0, float uy0, float ux1, float uy1)
{
    float width = ctx->stroke_w * mat_scale(ctx->ctm);
    SvgGrad sgrad;
    int have_sgrad = resolve_gradient(ctx, n, "stroke", &sgrad, ux0, uy0, ux1, uy1);
    if (have_sgrad) {
        grad_scale_alpha(&sgrad, sop);
        stroke_polys(ctx, p, stroke, width, &sgrad);
    } else {
        stroke.a = (uint8_t)(stroke.a * sop);
        stroke_polys(ctx, p, stroke, width, NULL);
    }
}

/**
 * @brief 填充并/或描边一个已构建的形状 / Fill and/or stroke an already-built shape
 *
 * @param[in,out] ctx SVG 上下文 / SVG context
 * @param[in]     n DOM 元素 / DOM element
 * @param[in]     p 多边形列表 / Polygon list
 * @param[in]     ux0 用户空间包围盒左 / User-space bbox left
 * @param[in]     uy0 用户空间包围盒上 / User-space bbox top
 * @param[in]     ux1 用户空间包围盒右 / User-space bbox right
 * @param[in]     uy1 用户空间包围盒下 / User-space bbox bottom
 * @param[in]     fillable 允许填充为 1 / 1 when fillable
 * @param[in]     strokable 允许描边为 1 / 1 when strokable
 *
 * @note 元素自身的 fill/stroke/opacity 属性覆盖继承状态 / The element's own fill/stroke/opacity attributes override the inherited state
 */
static void paint_shape(SvgCtx *ctx, HpeDomNode *n, Polys *p, float ux0, float uy0, float ux1,
                        float uy1, int fillable, int strokable)
{
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

    if (fillable && has_fill)
        paint_shape_fill(ctx, n, p, fill, fop, ux0, uy0, ux1, uy1);
    if (strokable && has_stroke)
        paint_shape_stroke(ctx, n, p, stroke, sop, ux0, uy0, ux1, uy1);
}

static void draw_element(SvgCtx *ctx, HpeDomNode *n, Mat ctm);

/**
 * @brief 绘制父元素的所有元素子节点 / Draw all element children of a parent
 *
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     parent 父元素 / Parent element
 * @param[in]     ctm 当前变换矩阵 / Current transformation matrix
 *
 * @note 非元素子节点被跳过 / Non-element children are skipped
 */
static void draw_children(SvgCtx *ctx, HpeDomNode *parent, Mat ctm)
{
    HpeDomNode *n;
    for (n = parent->first_child; n; n = n->next_sibling) {
        if (n->type != HPE_NODE_ELEMENT)
            continue;
        draw_element(ctx, n, ctm);
    }
}

static int use_ref_id(HpeDomNode *node, char *id, size_t cap); /* defined in hit testing */

/**
 * @brief 判断标签是否只承载元数据、不直接绘制 / Test whether a tag carries metadata only and is never painted directly
 *
 * @param[in]     tag 标签名 / Tag name
 *
 * @return 是元数据标签返回 1，否则 0 / 1 for a metadata tag, else 0
 *
 * @note title、desc、defs、linearGradient、radialGradient、stop / `title`, `desc`, `defs`, `linearGradient`, `radialGradient`, `stop`
 */
static int svg_is_metadata_tag(const char *tag)
{
    return !strcmp(tag, "title") || !strcmp(tag, "desc") || !strcmp(tag, "defs") ||
           hpe_str_ieq(tag, "linearGradient") || hpe_str_ieq(tag, "radialGradient") ||
           !strcmp(tag, "stop");
}

/**
 * @brief 绘制 `<use>` 引用的元素 / Draw the element referenced by a `<use>`
 *
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[out]    sub 派生子上下文 / Derived sub-context
 * @param[in]     n `<use>` 元素 / The `<use>` element
 * @param[in]     ctm 当前变换矩阵 / Current transformation matrix
 *
 * @note 按 x/y 平移并应用 use 的样式，随后递归绘制引用元素 / Translated by x/y, applies the `use` style, then recursively draws the referenced element
 */
static void draw_use_ref(SvgCtx *ctx, SvgCtx *sub, HpeDomNode *n, Mat ctm)
{
    HpeDomNode *ref;
    char id[128];
    float x = 0.0f, y = 0.0f;
    Mat t;
    if (!use_ref_id(n, id, sizeof(id)))
        return;
    ref = find_by_id(ctx->root, id);
    if (!ref)
        return;
    attr_num(n, "x", &x);
    attr_num(n, "y", &y);
    *sub = *ctx;
    sub->ctm = ctm;
    if (x != 0.0f || y != 0.0f) {
        t = mat_identity();
        t.e = x;
        t.f = y;
        sub->ctm = mat_mul(sub->ctm, t);
    }
    ctx_apply_style(sub, n);
    sub->use_depth = ctx->use_depth + 1;
    draw_element(sub, ref, sub->ctm);
}

/**
 * @brief 绘制 `<g>`/`<svg>` 容器 / Draw a `<g>`/`<svg>` container
 *
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[out]    sub 派生子上下文 / Derived sub-context
 * @param[in]     n 容器元素 / Container element
 * @param[in]     ctm 当前变换矩阵 / Current transformation matrix
 * @param[in]     tag 标签名 / Tag name
 *
 * @note 压入样式后递归；`<svg>` 额外按其 x/y 偏移 / Pushes the style and recurses; `<svg>` is additionally offset by its x/y
 */
static void draw_container(SvgCtx *ctx, SvgCtx *sub, HpeDomNode *n, Mat ctm, const char *tag)
{
    *sub = *ctx;
    sub->ctm = ctm;
    ctx_apply_style(sub, n);
    if (!strcmp(tag, "svg")) {
        float x = 0.0f, y = 0.0f;
        if (attr_num(n, "x", &x) || attr_num(n, "y", &y)) {
            Mat t = mat_identity();
            t.e = x;
            t.f = y;
            sub->ctm = mat_mul(sub->ctm, t);
        }
    }
    draw_children(sub, n, sub->ctm);
}

/**
 * @brief 绘制 `<rect>` 元素 / Draw a `<rect>` element
 *
 * @param[in,out] sub SVG 子上下文 / SVG sub-context
 * @param[in]     n `<rect>` 元素 / The `<rect>` element
 *
 * @note rx 缺失时回退到 ry；宽或高非正时不绘制 / Falls back to `ry` when `rx` is absent; draws nothing when the width or height is not positive
 */
static void draw_rect_node(SvgCtx *sub, HpeDomNode *n)
{
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
    round_rect_path(&p, sub, x, y, w, h, r);
    paint_shape(sub, n, &p, x, y, x + w, y + h, 1, 1);
    polys_free(&p);
}

/**
 * @brief 绘制 `<circle>`/`<ellipse>` 元素 / Draw a `<circle>`/`<ellipse>` element
 *
 * @param[in,out] sub SVG 子上下文 / SVG sub-context
 * @param[in]     n 元素 / Element
 * @param[in]     tag 标签名 / Tag name
 *
 * @note 圆形取 r 作为两半径；半径非正时不绘制 / A circle uses `r` for both radii; draws nothing when a radius is not positive
 */
static void draw_ellipse_node(SvgCtx *sub, HpeDomNode *n, const char *tag)
{
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
    ellipse_path(&p, sub, cx, cy, rx, ry);
    paint_shape(sub, n, &p, cx - rx, cy - ry, cx + rx, cy + ry, 1, 1);
    polys_free(&p);
}

/**
 * @brief 绘制 `<line>` 元素 / Draw a `<line>` element
 *
 * @param[in,out] sub SVG 子上下文 / SVG sub-context
 * @param[in]     n `<line>` 元素 / The `<line>` element
 *
 * @note 只描边不填充 / Stroked only, never filled
 */
static void draw_line_node(SvgCtx *sub, HpeDomNode *n)
{
    float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    Polys p;
    SubPath *sp;
    float ddx, ddy;
    attr_num(n, "x1", &x1);
    attr_num(n, "y1", &y1);
    attr_num(n, "x2", &x2);
    attr_num(n, "y2", &y2);
    memset(&p, 0, sizeof(p));
    sp = polys_new(&p);
    mat_apply(sub->ctm, x1, y1, &ddx, &ddy);
    sub_push(sp, ddx, ddy);
    mat_apply(sub->ctm, x2, y2, &ddx, &ddy);
    sub_push(sp, ddx, ddy);
    paint_shape(sub, n, &p, fminf(x1, x2), fminf(y1, y2), fmaxf(x1, x2), fmaxf(y1, y2), 0, 1);
    polys_free(&p);
}

/**
 * @brief 绘制 `<polygon>`/`<polyline>` 元素 / Draw a `<polygon>`/`<polyline>` element
 *
 * @param[in,out] sub SVG 子上下文 / SVG sub-context
 * @param[in]     n 元素 / Element
 * @param[in]     tag 标签名 / Tag name
 *
 * @note polygon 填充并描边，polyline 只描边 / `polygon` is filled and stroked while `polyline` is stroked only
 */
static void draw_poly_node(SvgCtx *sub, HpeDomNode *n, const char *tag)
{
    Polys p;
    int closed = !strcmp(tag, "polygon");
    const char *pts = attr(n, "points");
    float minx = 0.0f, miny = 0.0f, maxx = 0.0f, maxy = 0.0f;
    memset(&p, 0, sizeof(p));
    points_bbox(pts, &minx, &miny, &maxx, &maxy);
    points_path(&p, sub, pts, closed);
    if (p.count > 0)
        paint_shape(sub, n, &p, minx, miny, maxx, maxy, closed, 1);
    polys_free(&p);
}

/**
 * @brief 绘制 `<path>` 元素 / Draw a `<path>` element
 *
 * @param[in,out] sub SVG 子上下文 / SVG sub-context
 * @param[in]     n `<path>` 元素 / The `<path>` element
 *
 * @note 以单位矩阵求包围盒供渐变映射 / The bbox for gradient mapping is computed with an identity matrix
 */
static void draw_path_node(SvgCtx *sub, HpeDomNode *n)
{
    Polys p;
    float bx0, by0, bx1, by1;
    memset(&p, 0, sizeof(p));
    build_path(&p, sub, attr(n, "d"));
    path_bbox(attr(n, "d"), &bx0, &by0, &bx1, &by1);
    if (p.count > 0)
        paint_shape(sub, n, &p, bx0, by0, bx1, by1, 1, 1);
    polys_free(&p);
}

/**
 * @brief 在独立样式的子上下文中绘制一个叶子形状 / Draw a leaf shape in its own styled sub-context
 *
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     n DOM 元素 / DOM element
 * @param[in]     ctm 当前变换矩阵 / Current transformation matrix
 * @param[in]     tag 标签名 / Tag name
 *
 * @note 按标签分派到 text/rect/circle/ellipse/line/polygon/polyline/path；未知元素递归子节点以免内容丢失 / Dispatches on the tag to text/rect/circle/ellipse/line/polygon/polyline/path; an unknown element recurses into its children so nested content is not lost
 */
static void draw_shape(SvgCtx *ctx, HpeDomNode *n, Mat ctm, const char *tag)
{
    SvgCtx sub;
    sub = *ctx;
    sub.ctm = ctm;
    ctx_apply_style(&sub, n);
    if (!strcmp(tag, "text"))
        draw_text(&sub, n);
    else if (!strcmp(tag, "rect"))
        draw_rect_node(&sub, n);
    else if (!strcmp(tag, "circle") || !strcmp(tag, "ellipse"))
        draw_ellipse_node(&sub, n, tag);
    else if (!strcmp(tag, "line"))
        draw_line_node(&sub, n);
    else if (!strcmp(tag, "polygon") || !strcmp(tag, "polyline"))
        draw_poly_node(&sub, n, tag);
    else if (!strcmp(tag, "path"))
        draw_path_node(&sub, n);
    else /* unknown element: recurse so nested content is not lost */
        draw_children(&sub, n, sub.ctm);
}

/**
 * @brief 绘制一个 SVG 元素 / Draw one SVG element
 *
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     n DOM 元素 / DOM element
 * @param[in]     ctm 当前变换矩阵 / Current transformation matrix
 *
 * @note 元数据标签跳过；先累积元素的 transform，再按标签分派到容器/use/形状 / Metadata tags are skipped; the element transform is accumulated first, then dispatch goes to container, `use` or shape
 */
static void draw_element(SvgCtx *ctx, HpeDomNode *n, Mat ctm)
{
    const char *tag = n->tag;
    SvgCtx sub;

    if (!tag || svg_is_metadata_tag(tag))
        return;
    ctm = mat_mul(ctm, parse_transform(attr(n, "transform")));

    if (!strcmp(tag, "g") || !strcmp(tag, "svg")) {
        draw_container(ctx, &sub, n, ctm, tag);
        return;
    }
    if (!strcmp(tag, "use")) {
        if (ctx->use_depth <= 6)
            draw_use_ref(ctx, &sub, n, ctm);
        return;
    }
    draw_shape(ctx, n, ctm, tag);
}

/**
 * @brief 从 `<svg>` 盒初始化 SVG 绘制上下文 / Seed the SVG drawing context from the `<svg>` box
 *
 * @param[out]    ctx SVG 上下文 / SVG context
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box `<svg>` 盒 / The `<svg>` box
 * @param[in]     node `<svg>` DOM 节点 / The `<svg>` DOM node
 * @param[in]     y_dev 盒子顶边的设备 y / Device y of the box top edge
 *
 * @note 设置设备空间 CTM 原点、视口尺寸与默认绘制状态 / Sets the device-space CTM origin, viewport size and default paint state
 */
static void svg_ctx_init(SvgCtx *ctx, HpePixmap *pm, HpeBox *box, HpeDomNode *node, float y_dev)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->pm = pm;
    ctx->ctm = mat_identity();
    ctx->ctm.e = box->content_box.x;
    ctx->ctm.f = y_dev + (box->content_box.y - box->border_box.y);
    ctx->vp_w = box->content_box.w;
    ctx->vp_h = box->content_box.h;
    ctx->stroke_w = 1.0f;
    ctx->has_stroke = 0;
    ctx->has_fill = 0;
    ctx->fill_opacity = 1.0f;
    ctx->stroke_opacity = 1.0f;
    ctx->root = node;
}

/**
 * @brief 应用 viewBox 变换 / Apply the viewBox transform
 *
 * @param[in,out] ctx SVG 上下文 / SVG context
 * @param[in]     node `<svg>` DOM 节点 / The `<svg>` DOM node
 * @param[out]    vw viewBox 宽度（存在时）/ viewBox width when present
 * @param[out]    vh viewBox 高度（存在时）/ viewBox height when present
 *
 * @note 取均匀缩放并居中；无 viewBox 或格式非法时不变换 / Uses a uniform scale plus centring; does nothing when there is no viewBox or it is malformed
 */
static void svg_apply_viewbox(SvgCtx *ctx, HpeDomNode *node, float *vw, float *vh)
{
    const char *vbs = attr(node, "viewBox");
    float a, b, c, d, sx, sy, scale;
    if (!vbs)
        return;
    if (sscanf(vbs, "%f %f %f %f", &a, &b, &c, &d) != 4 || c <= 0.0f || d <= 0.0f)
        return;
    *vw = c;
    *vh = d;
    sx = ctx->vp_w / *vw;
    sy = ctx->vp_h / *vh;
    scale = sx < sy ? sx : sy;
    if (scale <= 0.0f)
        scale = 1.0f;
    ctx->ctm.e += (ctx->vp_w - *vw * scale) * 0.5f - a * scale;
    ctx->ctm.f += (ctx->vp_h - *vh * scale) * 0.5f - b * scale;
    ctx->ctm.a = scale;
    ctx->ctm.d = scale;
}

/**
 * @brief 绘制 `<svg>` 子树 / Paint the `<svg>` subtree
 *
 * @param[in,out] pm 目标像素缓冲 / Target pixel buffer
 * @param[in]     box `<svg>` 盒 / The `<svg>` box
 * @param[in]     y_dev 盒子顶边的设备 y / Device y of the box top edge
 *
 * @note 裁剪到 SVG 视口并与已激活的损伤矩形求交，避免半透明描边在滚动中被重复混合而变暗 / Clips to the SVG viewport and intersects the already-active damage rect so translucent strokes are not blended twice and darkened on scroll
 */
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

    svg_ctx_init(&ctx, pm, box, node, y_dev);

    if (getenv("HOPE_TRACE_SVG"))
        fprintf(stderr, "svg box=(%.1f,%.1f %.1fx%.1f) viewBox=%s\n", box->content_box.x,
                box->content_box.y, ctx.vp_w, ctx.vp_h,
                attr(node, "viewBox") ? attr(node, "viewBox") : "-");

    svg_apply_viewbox(&ctx, node, &vw, &vh);
    HPE_UNUSED(vw);
    HPE_UNUSED(vh);

    /* root <svg> style attributes apply to the whole subtree */
    ctx_apply_style(&ctx, node);

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

/**
 * @brief 读取数值属性，缺失时返回默认值 / Read a numeric attribute, returning a default when absent
 *
 * @param[in]     n DOM 元素 / DOM element
 * @param[in]     name 属性名 / Attribute name
 * @param[in]     def 默认值 / Default value
 *
 * @return 属性值或默认值 / The attribute value, or the default
 */

static float attr_f(HpeDomNode *n, const char *name, float def)
{
    float f;
    return attr_num(n, name, &f) ? f : def;
}

/**
 * @brief 用奇偶规则判断点是否在多边形内 / Test whether a point is inside the polygons using the even-odd rule
 *
 * @param[in]     p 多边形列表 / Polygon list
 * @param[in]     x X 坐标 / X coordinate
 * @param[in]     y Y 坐标 / Y coordinate
 *
 * @return 在内部返回 1，否则 0 / 1 when inside, else 0
 */
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

static HpeDomNode *hit_walk(SvgCtx *ctx, HpeDomNode *node, float dx, float dy);

/**
 * @brief 从 `<use>` 的 href/xlink:href 取片段 id / Extract the fragment id from a `<use>` `href`/`xlink:href`
 *
 * @param[in]     node `<use>` 元素 / The `<use>` element
 * @param[out]    id 接收片段 id / Receives the fragment id
 * @param[in]     cap `id` 缓冲区容量 / Capacity of the `id` buffer
 *
 * @return 解析成功返回 1，否则 0 / 1 on success, else 0
 */
static int use_ref_id(HpeDomNode *node, char *id, size_t cap)
{
    const char *href = attr(node, "href");
    const char *hash;
    size_t len = 0;
    if (!href)
        href = attr(node, "xlink:href");
    if (!href)
        return 0;
    hash = strchr(href, '#');
    if (!hash)
        return 0;
    hash++;
    while (hash[len] && len < cap - 1) {
        id[len] = hash[len];
        len++;
    }
    id[len] = '\0';
    return 1;
}

/**
 * @brief 命中测试所有元素子节点 / Hit test all element children
 *
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     node 父节点 / Parent node
 * @param[in]     dx 设备点 X / Device point X
 * @param[in]     dy 设备点 Y / Device point Y
 *
 * @return 最深命中子节点，未命中返回 `NULL` / The deepest hit child, or `NULL` when nothing is hit
 *
 * @note 后出现的兄弟覆盖先出现的，因此取最后一个命中 / Later siblings paint on top, so the last hit wins
 */
static HpeDomNode *hit_children(SvgCtx *ctx, HpeDomNode *node, float dx, float dy)
{
    HpeDomNode *c, *best = NULL;
    for (c = node->first_child; c; c = c->next_sibling) {
        HpeDomNode *r = hit_walk(ctx, c, dx, dy);
        if (r)
            best = r;
    }
    return best;
}

/**
 * @brief 命中测试一个 `<rect>` / Hit test a `<rect>`
 *
 * @param[in]     n `<rect>` 元素 / The `<rect>` element
 * @param[in]     ux 用户空间 X / User-space X
 * @param[in]     uy 用户空间 Y / User-space Y
 *
 * @return 命中则返回该元素，否则 `NULL` / The element when hit, else `NULL`
 */
static HpeDomNode *hit_rect(HpeDomNode *n, float ux, float uy)
{
    float x = attr_f(n, "x", 0.0f), y = attr_f(n, "y", 0.0f);
    float w = attr_f(n, "width", 0.0f), h = attr_f(n, "height", 0.0f);
    if (w > 0.0f && h > 0.0f && ux >= x && ux <= x + w && uy >= y && uy <= y + h)
        return n;
    return NULL;
}

/**
 * @brief 命中测试一个 `<circle>`/`<ellipse>` / Hit test a `<circle>`/`<ellipse>`
 *
 * @param[in]     n 元素 / Element
 * @param[in]     ux 用户空间 X / User-space X
 * @param[in]     uy 用户空间 Y / User-space Y
 *
 * @return 命中则返回该元素，否则 `NULL` / The element when hit, else `NULL`
 *
 * @note 把点归一化到单位圆判断 / The point is normalised against the unit circle
 */
static HpeDomNode *hit_ellipse(HpeDomNode *n, float ux, float uy)
{
    float cx = attr_f(n, "cx", 0.0f), cy = attr_f(n, "cy", 0.0f);
    float rx, ry, nx, ny;
    if (!strcmp(n->tag, "circle"))
        rx = ry = attr_f(n, "r", 0.0f);
    else
        rx = attr_f(n, "rx", 0.0f), ry = attr_f(n, "ry", 0.0f);
    if (rx <= 0.0f || ry <= 0.0f)
        return NULL;
    nx = (ux - cx) / rx;
    ny = (uy - cy) / ry;
    return (nx * nx + ny * ny <= 1.0f) ? n : NULL;
}

/**
 * @brief 命中测试一条 `<line>` / Hit test a `<line>`
 *
 * @param[in]     n `<line>` 元素 / The `<line>` element
 * @param[in]     ux 用户空间 X / User-space X
 * @param[in]     uy 用户空间 Y / User-space Y
 *
 * @return 命中则返回该元素，否则 `NULL` / The element when hit, else `NULL`
 *
 * @note 线段周围约 3px 容差 / About 3 px tolerance around the segment
 */
static HpeDomNode *hit_line(HpeDomNode *n, float ux, float uy)
{
    float x1 = attr_f(n, "x1", 0.0f), y1 = attr_f(n, "y1", 0.0f);
    float x2 = attr_f(n, "x2", 0.0f), y2 = attr_f(n, "y2", 0.0f);
    float vx = x2 - x1, vy = y2 - y1;
    float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? ((ux - x1) * vx + (uy - y1) * vy) / len2 : 0.0f;
    float px, py;
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    px = x1 + t * vx;
    py = y1 + t * vy;
    return ((ux - px) * (ux - px) + (uy - py) * (uy - py) <= 9.0f) ? n : NULL;
}

/**
 * @brief 命中测试 `<polygon>`/`<polyline>` / Hit test a `<polygon>`/`<polyline>`
 *
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     n 元素 / Element
 * @param[in]     ux 用户空间 X / User-space X
 * @param[in]     uy 用户空间 Y / User-space Y
 *
 * @return 命中则返回该元素，否则 `NULL` / The element when hit, else `NULL`
 *
 * @note 在单位矩阵空间构建几何后测点 / The geometry is built in identity space before the point test
 */
static HpeDomNode *hit_points(SvgCtx *ctx, HpeDomNode *n, float ux, float uy)
{
    Polys pp;
    SvgCtx tmp = *ctx;
    int hit;
    memset(&pp, 0, sizeof(pp));
    tmp.cov_scratch = NULL;
    tmp.cov_cap = 0;
    tmp.ctm = mat_identity();
    points_path(&pp, &tmp, attr(n, "points"), !strcmp(n->tag, "polygon"));
    hit = point_in_polys(&pp, ux, uy);
    polys_free(&pp);
    return hit ? n : NULL;
}

/**
 * @brief 命中测试一个 `<path>` / Hit test a `<path>`
 *
 * @param[in]     ctx SVG 上下文 / SVG context
 * @param[in]     n `<path>` 元素 / The `<path>` element
 * @param[in]     ux 用户空间 X / User-space X
 * @param[in]     uy 用户空间 Y / User-space Y
 *
 * @return 命中则返回该元素，否则 `NULL` / The element when hit, else `NULL`
 *
 * @note 在单位矩阵空间构建几何后测点 / The geometry is built in identity space before the point test
 */
static HpeDomNode *hit_path(SvgCtx *ctx, HpeDomNode *n, float ux, float uy)
{
    Polys pp;
    SvgCtx tmp = *ctx;
    int hit;
    memset(&pp, 0, sizeof(pp));
    tmp.cov_scratch = NULL;
    tmp.cov_cap = 0;
    tmp.ctm = mat_identity();
    build_path(&pp, &tmp, attr(n, "d"));
    hit = point_in_polys(&pp, ux, uy);
    polys_free(&pp);
    return hit ? n : NULL;
}

/**
 * @brief 命中测试一个 `<text>` / Hit test a `<text>`
 *
 * @param[in]     n `<text>` 元素 / The `<text>` element
 * @param[in]     ux 用户空间 X / User-space X
 * @param[in]     uy 用户空间 Y / User-space Y
 *
 * @return 命中则返回该元素，否则 `NULL` / The element when hit, else `NULL`
 *
 * @note 以锚点与字号估算文本框 / The text box is estimated from the anchor point and the font size
 */
static HpeDomNode *hit_text(HpeDomNode *n, float ux, float uy)
{
    float x = attr_f(n, "x", 0.0f), y = attr_f(n, "y", 0.0f);
    float fs = attr_f(n, "font-size", 12.0f);
    if (ux >= x - 2.0f && uy >= y - fs && uy <= y + fs * 0.4f)
        return n;
    return NULL;
}

/**
 * @brief 命中测试一个叶子形状 / Hit test a leaf shape
 *
 * @param[in,out] ctx SVG 上下文 / SVG context
 * @param[in]     node 元素 / Element
 * @param[in]     tag 标签名 / Tag name
 * @param[in]     dx 设备点 X / Device point X
 * @param[in]     dy 设备点 Y / Device point Y
 *
 * @return 命中则返回该元素，否则 `NULL` / The element when hit, else `NULL`
 *
 * @note 先把设备点经逆矩阵映射到用户空间 / The device point is inverse-mapped into user space first
 */
static HpeDomNode *hit_shape(SvgCtx *ctx, HpeDomNode *node, const char *tag, float dx, float dy)
{
    Mat inv;
    float ux, uy;
    ctx->ctm = mat_mul(ctx->ctm, parse_transform(attr(node, "transform")));
    inv = mat_invert(ctx->ctm);
    mat_apply(inv, dx, dy, &ux, &uy);
    if (!strcmp(tag, "rect"))
        return hit_rect(node, ux, uy);
    if (!strcmp(tag, "circle") || !strcmp(tag, "ellipse"))
        return hit_ellipse(node, ux, uy);
    if (!strcmp(tag, "line"))
        return hit_line(node, ux, uy);
    if (!strcmp(tag, "polygon") || !strcmp(tag, "polyline"))
        return hit_points(ctx, node, ux, uy);
    if (!strcmp(tag, "path"))
        return hit_path(ctx, node, ux, uy);
    if (!strcmp(tag, "text"))
        return hit_text(node, ux, uy);
    return NULL;
}

/**
 * @brief 命中测试一个 `<use>` / Hit test a `<use>`
 *
 * @param[in,out] ctx SVG 上下文 / SVG context
 * @param[in]     node `<use>` 元素 / The `<use>` element
 * @param[in]     dx 设备点 X / Device point X
 * @param[in]     dy 设备点 Y / Device point Y
 *
 * @return 最深命中节点，未命中返回 `NULL` / The deepest hit node, or `NULL`
 *
 * @note 把 use 的 transform 与 x/y 压入 CTM 后递归引用元素 / Pushes the `use` transform and x/y onto the CTM, then recurses into the referenced element
 */
static HpeDomNode *hit_use(SvgCtx *ctx, HpeDomNode *node, float dx, float dy)
{
    HpeDomNode *ref, *best;
    char id[128];
    Mat saved = ctx->ctm, t;
    if (!use_ref_id(node, id, sizeof(id)))
        return NULL;
    ref = find_by_id(ctx->root, id);
    if (!ref)
        return NULL;
    ctx->ctm = mat_mul(ctx->ctm, parse_transform(attr(node, "transform")));
    t = mat_identity();
    t.e = attr_f(node, "x", 0.0f);
    t.f = attr_f(node, "y", 0.0f);
    ctx->ctm = mat_mul(ctx->ctm, t);
    best = hit_walk(ctx, ref, dx, dy);
    ctx->ctm = saved;
    return best;
}

/**
 * @brief 递归遍历 SVG 树做命中测试 / Walk the SVG tree recursively for hit testing
 *
 * @param[in,out] ctx SVG 上下文 / SVG context
 * @param[in]     node 当前节点 / Current node
 * @param[in]     dx 设备点 X / Device point X
 * @param[in]     dy 设备点 Y / Device point Y
 *
 * @return 最深命中节点，未命中返回 `NULL` / The deepest hit node, or `NULL`
 *
 * @note defs/title/desc 不参与命中；g/svg 压入 transform 后递归子节点，use 与形状各自处理 / `defs`/`title`/`desc` never hit; `g`/`svg` push their transform and recurse while `use` and shapes are handled separately
 */
static HpeDomNode *hit_walk(SvgCtx *ctx, HpeDomNode *node, float dx, float dy)
{
    const char *tag;
    Mat saved;
    if (!node || node->type != HPE_NODE_ELEMENT || !node->tag)
        return NULL;
    tag = node->tag;
    if (!strcmp(tag, "defs") || !strcmp(tag, "title") || !strcmp(tag, "desc"))
        return NULL;
    if (!strcmp(tag, "g") || !strcmp(tag, "svg")) {
        saved = ctx->ctm;
        ctx->ctm = mat_mul(ctx->ctm, parse_transform(attr(node, "transform")));
        node = hit_children(ctx, node, dx, dy);
        ctx->ctm = saved;
        return node;
    }
    if (!strcmp(tag, "use"))
        return hit_use(ctx, node, dx, dy);
    return hit_shape(ctx, node, tag, dx, dy);
}

/**
 * @brief 对 `<svg>` 子树做命中测试 / Hit test an `<svg>` subtree
 *
 * @param[in]     box `<svg>` 盒 / The `<svg>` box
 * @param[in]     x 文档坐标 X / Document-space X
 * @param[in]     y 文档坐标 Y / Document-space Y
 *
 * @return 最深的包含该点的 SVG 元素，未命中返回 `NULL` / The deepest SVG element containing the point, or `NULL`
 *
 * @note 以盒内容盒左上角为原点，并按 viewBox 缩放居中；路径目前尚未参与命中测试 / The origin is the box's content-box top-left with viewBox scaling and centring applied; paths are not hit tested yet
 */
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
