#include "paint/hpe_paint.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paint/hpe_svg.h"
#include "base/hpe_util.h"
#include "render/hpe_raster.h"

/* Quantize a device y to a 1/64 px grid. Layout accumulates fractional heights
 * (178.4 + 319.6 = 497.99997...), and floor/ceil on such near-integer edges is
 * only stable if the value survives translation exactly. Multiples of 1/64 are
 * exactly representable and integer offsets (scroll, chrome height) keep them
 * that way, so a shifted copy of a row and a re-painted row always agree. */
static float snap_y(float y)
{
    return (float)((double)(int)(y * 64.0f + (y >= 0.0f ? 0.5f : -0.5f))) / 64.0f;
}

/* A paint pass maps document coordinates to device coordinates as
 *   device_x = doc_x + dev_x
 *   device_y = doc_y - scroll + dev_y
 * `dev_x`/`dev_y` are non-zero only inside an opacity group (which paints into
 * a scratch pixmap anchored at the box's top-left) and later by transforms. */
typedef struct PaintCtx {
    float dev_x, dev_y;
    int group_depth; /* opacity/transform-group nesting, to cap pathological recursion */
} PaintCtx;

#define MAX_OPACITY_DEPTH 3

static float ctx_dev_y(PaintCtx *ctx, float doc_y, float scroll)
{
    return doc_y - scroll + ctx->dev_y;
}

static HpeRect box_device_rect(PaintCtx *ctx, HpeBox *box, float scroll)
{
    HpeRect r = box->border_box;
    r.x += ctx->dev_x;
    r.y = snap_y(r.y - scroll + ctx->dev_y);
    return r;
}

/* ------------------------------------------------------------------ */
/* list markers                                                        */
/* ------------------------------------------------------------------ */

static int is_list_item(HpeBox *box)
{
    return box->node && box->node->tag && !strcmp(box->node->tag, "li") && box->parent &&
           box->parent->node && box->parent->node->tag &&
           (!strcmp(box->parent->node->tag, "ul") || !strcmp(box->parent->node->tag, "ol"));
}

/* an <ol> child shows its 1-based position among the <li> siblings */
static int list_index(HpeBox *box)
{
    HpeBox *sib;
    int index = 0;
    if (!box->parent || !box->parent->node || strcmp(box->parent->node->tag, "ol") != 0)
        return 0;
    for (sib = box->parent->first_child; sib && sib != box; sib = sib->next)
        if (sib->node && sib->node->tag && !strcmp(sib->node->tag, "li"))
            index++;
    return index;
}

/* The box tree inherits only a fixed property list, so `list-style-type` set on
 * <ul>/<ol> does not reach the <li>. Read it from the parent when the item
 * itself declares none. */
static HpeListStyleType effective_list_type(HpeBox *box)
{
    if (box->style.set2 & HPE_PROP2_LIST_STYLE_TYPE)
        return box->style.list_style_type;
    if (box->parent)
        return box->parent->style.list_style_type;
    return HPE_LIST_DISC;
}

/* `list-style-type: square`: a small filled square at the marker anchor */
static void paint_marker_square(HpePixmap *pm, HpeBox *box, float marker_x, float baseline)
{
    float size = box->style.font_size * 0.32f;
    hpe_fill_rect(pm, hpe_rect(marker_x - size - 3.0f,
                               baseline - box->style.font_size * 0.55f, size, size),
                  box->style.color);
}

/* `list-style-type: circle`: a small hollow circle at the marker anchor */
static void paint_marker_circle(HpePixmap *pm, HpeBox *box, float marker_x, float baseline)
{
    float r = box->style.font_size * 0.18f;
    hpe_stroke_round_rect(pm, hpe_rect(marker_x - r * 2.0f - 3.0f,
                                       baseline - box->style.font_size * 0.55f, r * 2.0f,
                                       r * 2.0f),
                          r, 1.0f, box->style.color);
}

static void paint_list_marker(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll)
{
    static const char bullet[] = "\xE2\x80\xA2"; /* U+2022 */
    HpeListStyleType type;
    HpeFont *font;
    const HpeTextMask *mask;
    float baseline, marker_x, base_x;

    if (!is_list_item(box) || box->line_count == 0)
        return;
    type = effective_list_type(box);
    if (type == HPE_LIST_NONE)
        return;
    font = hpe_font_get(box->style.font_size, box->style.font_weight, box->style.family);
    if (!font)
        return;
    baseline = snap_y(ctx_dev_y(ctx, box->lines[0].y + box->lines[0].baseline, scroll));
    base_x = box->content_box.x + ctx->dev_x;
    marker_x = (box->style.list_style_position == HPE_LIST_INSIDE) ? base_x + 6.0f
                                                                   : base_x - 18.0f;

    switch (type) {
    case HPE_LIST_DECIMAL: {
        char num[16];
        int n = snprintf(num, sizeof(num), "%d.", list_index(box) + 1);
        float adv = hpe_font_measure(font, num, n);
        mask = hpe_font_mask(font, num, n);
        if (mask)
            hpe_draw_text_mask(pm, (int)floorf(marker_x - adv), (int)floorf(baseline), mask,
                               box->style.color);
        break;
    }
    case HPE_LIST_SQUARE:
        paint_marker_square(pm, box, marker_x, baseline);
        break;
    case HPE_LIST_CIRCLE:
        paint_marker_circle(pm, box, marker_x, baseline);
        break;
    default: /* disc: a U+2022 bullet, left-aligned at the marker anchor */
        mask = hpe_font_mask(font, bullet, 3);
        if (mask)
            hpe_draw_text_mask(pm, (int)floorf(marker_x), (int)floorf(baseline), mask,
                               box->style.color);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* text decoration                                                     */
/* ------------------------------------------------------------------ */

static void paint_text_decorations(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll,
                                   HpeTextDecoration dec)
{
    int i, r;
    if (dec == HPE_TD_NONE || box->line_count == 0)
        return;
    for (i = 0; i < box->line_count; i++) {
        HpeLine *line = &box->lines[i];
        float baseline = snap_y(ctx_dev_y(ctx, line->y + line->baseline, scroll));
        float run_x0 = 1e9f, run_x1 = -1e9f;
        for (r = 0; r < line->run_count; r++) {
            HpeTextRun *run = &line->runs[r];
            float x0, x1;
            if (run->atomic || !run->font || run->color.a == 0)
                continue;
            x0 = box->content_box.x + ctx->dev_x + run->x;
            x1 = x0 + run->width;
            if (x0 < run_x0)
                run_x0 = x0;
            if (x1 > run_x1)
                run_x1 = x1;
        }
        if (run_x1 <= run_x0)
            continue;
        if (dec & HPE_TD_UNDERLINE)
            hpe_fill_rect(pm, hpe_rect(run_x0, baseline + 1.0f, run_x1 - run_x0, 1.0f),
                          box->style.color);
        if (dec & HPE_TD_OVERLINE)
            hpe_fill_rect(pm,
                          hpe_rect(run_x0, ctx_dev_y(ctx, line->y, scroll), run_x1 - run_x0, 1.0f),
                          box->style.color);
        if (dec & HPE_TD_LINE_THROUGH)
            hpe_fill_rect(pm,
                          hpe_rect(run_x0, baseline - box->style.font_size * 0.32f,
                                   run_x1 - run_x0, 1.0f),
                          box->style.color);
    }
}

/* ------------------------------------------------------------------ */
/* shadows                                                             */
/* ------------------------------------------------------------------ */

static void paint_box_shadow(HpePixmap *pm, HpeBox *box, HpeRect bb, float radius)
{
    HpeShadow *sh = &box->style.box_shadow;
    HpeRect r;
    if (!sh->valid || sh->color.a == 0)
        return;
    r = hpe_rect(bb.x + sh->ox - sh->spread, bb.y + sh->oy - sh->spread, bb.w + sh->spread * 2.0f,
                 bb.h + sh->spread * 2.0f);
    hpe_fill_shadow(pm, r, radius > 0.0f ? radius : 0.0f, sh->blur, sh->color);
}

static void paint_text_shadow(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll)
{
    HpeShadow *sh = &box->style.text_shadow;
    int i, r;
    if (!sh->valid || sh->color.a == 0)
        return;
    for (i = 0; i < box->line_count; i++) {
        HpeLine *line = &box->lines[i];
        float baseline = snap_y(ctx_dev_y(ctx, line->y + line->baseline, scroll));
        for (r = 0; r < line->run_count; r++) {
            HpeTextRun *run = &line->runs[r];
            const HpeTextMask *mask;
            if (run->atomic || !run->font)
                continue;
            mask = hpe_font_mask(run->font, run->text, run->len);
            if (!mask)
                continue;
            hpe_draw_text_mask(pm, (int)floorf(box->content_box.x + ctx->dev_x + run->x + sh->ox),
                               (int)floorf(baseline + sh->oy), mask, sh->color);
        }
    }
}

/* ------------------------------------------------------------------ */
/* background / borders                                                */
/* ------------------------------------------------------------------ */

static void paint_background(HpeStyle *bs, HpePixmap *pm, HpeRect bb, float radius)
{
    if (bs->background_image.kind == HPE_BG_LINEAR_GRADIENT) {
        HpeRasterStop stops[HPE_MAX_GRADIENT_STOPS];
        int i;
        for (i = 0; i < bs->background_image.stop_count; i++) {
            stops[i].pos = bs->background_image.stops[i].pos;
            stops[i].color = bs->background_image.stops[i].color;
        }
        hpe_fill_round_rect_gradient(pm, bb, radius, bs->background_image.angle_deg, stops,
                                     bs->background_image.stop_count);
        return;
    }
    if (bs->background.a > 0) {
        if (radius > 0.5f)
            hpe_fill_round_rect(pm, bb, radius, bs->background);
        else
            hpe_fill_rect(pm, bb, bs->background);
    }
}

/* Draws the four border bands with their own colours. Rounded corners collapse
 * to a single ring (four colours cannot be split there cheaply). */
static void paint_borders(HpeStyle *bs, HpePixmap *pm, HpeRect bb, float radius)
{
    float bt = bs->border_w[0], br = bs->border_w[1];
    float bbw = bs->border_w[2], bl = bs->border_w[3];
    float maxb = 0.0f;
    int i;
    int any = 0;

    for (i = 0; i < 4; i++) {
        if (bs->border_w[i] > maxb)
            maxb = bs->border_w[i];
        if (bs->border_w[i] > 0.0f && bs->border_colors[i].a > 0)
            any = 1;
    }
    if (!any)
        return;

    if (radius > 0.5f && maxb > 0.0f) {
        HpeColor best = bs->border_colors[0];
        for (i = 1; i < 4; i++)
            if (bs->border_colors[i].a > best.a)
                best = bs->border_colors[i];
        if (best.a > 0)
            hpe_stroke_round_rect(pm, bb, radius, maxb, best);
        return;
    }

    if (bt > 0.0f && bs->border_colors[0].a > 0)
        hpe_fill_rect(pm, hpe_rect(bb.x, bb.y, bb.w, bt), bs->border_colors[0]);
    if (bbw > 0.0f && bs->border_colors[2].a > 0)
        hpe_fill_rect(pm, hpe_rect(bb.x, bb.y + bb.h - bbw, bb.w, bbw), bs->border_colors[2]);
    if (bl > 0.0f && bs->border_colors[3].a > 0)
        hpe_fill_rect(pm, hpe_rect(bb.x, bb.y + bt, bl, bb.h - bt - bbw), bs->border_colors[3]);
    if (br > 0.0f && bs->border_colors[1].a > 0)
        hpe_fill_rect(pm, hpe_rect(bb.x + bb.w - br, bb.y + bt, br, bb.h - bt - bbw),
                      bs->border_colors[1]);
}

/* ------------------------------------------------------------------ */
/* the box walk                                                        */
/* ------------------------------------------------------------------ */

static void paint_box_inner(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll);

/* Paints `box` (and its subtree) into a scratch pixmap and blends the result
 * back with a per-pixel alpha that is `opacity` uniformly multiplied on top of
 * each source pixel's own alpha.
 *
 * A subtree painted directly into the destination would let a child blend with
 * a *sibling* it should never see (both share the page background otherwise),
 * and would leak the group's opacity into every descendant separately; the
 * scratch buffer composites the subtree in isolation first. `bbox` is the
 * device-space region to capture; `opacity` is 1.0 when only a transform is
 * being applied. */
/* clamps the scratch rect [x0,y0,w,h) to the destination clip; returns 0 when empty */
static int group_clip_scratch(HpePixmap *pm, int *x0, int *y0, int *w, int *h)
{
    /* clip the scratch to what the destination can accept */
    if (*x0 < pm->clip_x0) {
        *w -= pm->clip_x0 - *x0;
        *x0 = pm->clip_x0;
    }
    if (*y0 < pm->clip_y0) {
        *h -= pm->clip_y0 - *y0;
        *y0 = pm->clip_y0;
    }
    if (*x0 + *w > pm->clip_x1)
        *w = pm->clip_x1 - *x0;
    if (*y0 + *h > pm->clip_y1)
        *h = pm->clip_y1 - *y0;
    return (*w > 0 && *h > 0);
}

/* composites the scratch subtree onto `pm`, mapping each destination pixel back
 * through `xf` when a transform is in effect and multiplying source alpha by
 * `opacity` on the way. */
static void group_composite(HpePixmap *pm, const HpePixmap *scratch, int x0, int y0, int w, int h,
                            float opacity, HpeTransform *xf, int anchor_x, int anchor_y)
{
    /* the transform is applied about the box's top-left corner, which is
     * where the scratch origin was anchored (see paint_box) */
    int use_xf = xf && !xf->identity;
    float ox = (float)anchor_x - (float)x0;
    float oy = (float)anchor_y - (float)y0;
    int px, py;

    for (py = 0; py < h; py++) {
        uint32_t *dst_row = pm->pixels + (size_t)(y0 + py) * (size_t)pm->width + (size_t)x0;
        for (px = 0; px < w; px++) {
            uint32_t s;
            uint32_t sa, a, inv, sr, sg, sb, dr, dg, db;
            if (use_xf) {
                /* inverse-map the destination pixel back into scratch space */
                float rx = (float)px - ox, ry = (float)py - oy;
                float det = xf->a * xf->d - xf->b * xf->c;
                float ux, uy;
                int ix, iy;
                if (fabsf(det) < 1e-9f)
                    continue;
                ux = (xf->d * rx - xf->c * ry) / det;
                uy = (-xf->b * rx + xf->a * ry) / det;
                ix = (int)floorf(ux + ox);
                iy = (int)floorf(uy + oy);
                if (ix < 0 || iy < 0 || ix >= w || iy >= h)
                    continue;
                s = scratch->pixels[(size_t)iy * (size_t)w + (size_t)ix];
            } else {
                s = scratch->pixels[(size_t)py * (size_t)w + (size_t)px];
            }
            sa = (s >> 24) & 0xFFu;
            if (sa == 0)
                continue;
            a = (uint32_t)((float)sa * opacity + 0.5f);
            if (a == 0)
                continue;
            dr = (dst_row[px] >> 16) & 0xFFu;
            dg = (dst_row[px] >> 8) & 0xFFu;
            db = dst_row[px] & 0xFFu;
            sr = (s >> 16) & 0xFFu;
            sg = (s >> 8) & 0xFFu;
            sb = s & 0xFFu;
            inv = 255u - a;
            dst_row[px] = 0xFF000000u | (((sr * a + dr * inv) / 255u) << 16) |
                          (((sg * a + dg * inv) / 255u) << 8) | ((sb * a + db * inv) / 255u);
        }
    }
}

/* Paints `box` (and its subtree) into a scratch pixmap and blends the result
 * back with a per-pixel alpha that is `opacity` uniformly multiplied on top of
 * each source pixel's own alpha.
 *
 * A subtree painted directly into the destination would let a child blend with
 * a *sibling* it should never see (both share the page background otherwise),
 * and would leak the group's opacity into every descendant separately; the
 * scratch buffer composites the subtree in isolation first. `bbox` is the
 * device-space region to capture; `opacity` is 1.0 when only a transform is
 * being applied. */
static void paint_group(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll, HpeRect bbox,
                        float opacity, HpeTransform *xf, int anchor_x, int anchor_y)
{
    HpePixmap scratch;
    PaintCtx sub;
    int pad = 2;
    int x0, y0, w, h;

    if (opacity <= 0.0f)
        return; /* fully transparent: nothing to paint */

    if (box->style.box_shadow.valid) {
        HpeShadow *sh = &box->style.box_shadow;
        float bleed = sh->blur + sh->spread + fabsf(sh->ox) + fabsf(sh->oy);
        pad += (int)ceilf(bleed);
    }
    x0 = (int)floorf(bbox.x) - pad;
    y0 = (int)floorf(bbox.y) - pad;
    w = (int)ceilf(bbox.w) + pad * 2;
    h = (int)ceilf(bbox.h) + pad * 2;
    if (w <= 0 || h <= 0)
        return;
    if (!group_clip_scratch(pm, &x0, &y0, &w, &h))
        return;

    memset(&scratch, 0, sizeof(scratch));
    scratch.width = w;
    scratch.height = h;
    scratch.stride = w * 4;
    scratch.pixels = (uint32_t *)hpe_calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    hpe_pixmap_reset_clip(&scratch);

    /* shift the device origin so the box's top-left lands at the scratch origin */
    sub.dev_x = ctx->dev_x - (float)x0;
    sub.dev_y = ctx->dev_y - (float)y0;
    sub.group_depth = ctx->group_depth + 1;
    paint_box_inner(&sub, &scratch, box, scroll);

    group_composite(pm, &scratch, x0, y0, w, h, opacity, xf, anchor_x, anchor_y);
    hpe_free(scratch.pixels);
}

/* Axis-aligned bounds of the transformed box, in device space: rotate/skew turn
 * the box into a rotated quad, whose four corners bound it. */
static HpeRect transform_bounds(HpeRect bb, HpeTransform *xf)
{
    float cx[4], cy[4];
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    int i;
    if (xf->identity)
        return bb;
    cx[0] = bb.x;
    cy[0] = bb.y;
    cx[1] = bb.x + bb.w;
    cy[1] = bb.y;
    cx[2] = bb.x;
    cy[2] = bb.y + bb.h;
    cx[3] = bb.x + bb.w;
    cy[3] = bb.y + bb.h;
    for (i = 0; i < 4; i++) {
        /* the transform is about the box's top-left corner */
        float lx = cx[i] - bb.x, ly = cy[i] - bb.y;
        float x = bb.x + (xf->a * lx + xf->c * ly + xf->e);
        float y = bb.y + (xf->b * lx + xf->d * ly + xf->f);
        if (x < minx)
            minx = x;
        if (y < miny)
            miny = y;
        if (x > maxx)
            maxx = x;
        if (y > maxy)
            maxy = y;
    }
    return hpe_rect(minx, miny, maxx - minx, maxy - miny);
}

static void paint_box(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll)
{
    HpeStyle *bs = &box->style;
    int needs_group;
    if (bs->display == HPE_DISPLAY_NONE)
        return;
    /* `visibility: hidden` suppresses this box's own painting but not its
     * subtree: a descendant may set `visibility: visible` to reappear, so we
     * still recurse (paint_box_inner skips the self-painting when hidden). */
    needs_group = (bs->opacity < 1.0f) || !bs->transform.identity;
    if (needs_group && ctx->group_depth < MAX_OPACITY_DEPTH) {
        HpeRect bb = box_device_rect(ctx, box, scroll);
        float radius = bs->border_radius;
        HpeRect bbox = bb;
        if (radius < 0.0f) {
            float side = bb.w < bb.h ? bb.w : bb.h;
            radius = side * (-radius) * 0.01f;
        }
        if (!bs->transform.identity)
            bbox = transform_bounds(bb, &bs->transform);
        paint_group(ctx, pm, box, scroll, bbox, bs->opacity < 1.0f ? bs->opacity : 1.0f,
                    &bs->transform, (int)floorf(bb.x), (int)floorf(bb.y));
        return;
    }
    paint_box_inner(ctx, pm, box, scroll);
}

/* paints this box's own visuals: shadow, background, borders, marker, text
 * shadow, text runs and decorations. `radius` may be negative (percentage). */
static void paint_box_own_content(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float own_scroll,
                                  HpeRect bb, float radius)
{
    HpeStyle *bs = &box->style;
    int i, r;

    if (bs->box_shadow.valid)
        paint_box_shadow(pm, box, bb, radius);
    paint_background(bs, pm, bb, radius);
    paint_borders(bs, pm, bb, radius);

    if (getenv("HOPE_TRACE_PAINT"))
        fprintf(stderr, "paint %s<%s> bb=(%.2f,%.2f %.2fx%.2f) bg=%02x%02x%02x%02x runs=%d\n",
                box->kind == HPE_BOX_TEXT ? "T" : (box->kind == HPE_BOX_INLINE ? "I" : "B"),
                box->node && box->node->tag ? box->node->tag : "?", bb.x, bb.y, bb.w, bb.h,
                bs->background.a, bs->background.r, bs->background.g, bs->background.b,
                box->line_count);

    paint_list_marker(ctx, pm, box, own_scroll);
    if (bs->text_shadow.valid)
        paint_text_shadow(ctx, pm, box, own_scroll);

    for (i = 0; i < box->line_count; i++) {
        HpeLine *line = &box->lines[i];
        float baseline = snap_y(ctx_dev_y(ctx, line->y + line->baseline, own_scroll));
        float line_top = snap_y(ctx_dev_y(ctx, line->y, own_scroll));
        float line_bottom = line_top + line->height;

        if (line_bottom < (float)pm->clip_y0 || line_top > (float)pm->clip_y1)
            continue;

        for (r = 0; r < line->run_count; r++) {
            HpeTextRun *run = &line->runs[r];
            const HpeTextMask *mask;
            int x, y;
            /* atomic inline-block runs are painted via child recursion, not here */
            if (run->atomic || !run->font)
                continue;
            x = (int)floorf(box->content_box.x + ctx->dev_x + run->x);
            y = (int)floorf(baseline);
            if (run->color.a == 0 || x > pm->clip_x1)
                continue;
            mask = hpe_font_mask(run->font, run->text, run->len);
            if (!mask)
                continue;
            hpe_draw_text_mask(pm, x, y, mask, run->color);
        }
    }
    if (bs->text_decoration != HPE_TD_NONE)
        paint_text_decorations(ctx, pm, box, own_scroll, bs->text_decoration);
}

/* inserts `key` into the z-order stack by increasing z-index (insertion sort) */
static void stacked_insert_sorted(HpeBox **stacked, int n)
{
    HpeBox *key;
    int j;
    if (n <= 1)
        return;
    key = stacked[n - 1];
    j = n - 2;
    while (j >= 0 && stacked[j]->style.z_index > key->style.z_index) {
        stacked[j + 1] = stacked[j];
        j--;
    }
    stacked[j + 1] = key;
}

/* paints the child subtree: static children in order, positioned children on top
 * in ascending z-index */
static void paint_box_children(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll)
{
    HpeBox **stacked = NULL;
    size_t sc = 0, scap = 0;
    HpeBox *child;
    int r;

    for (child = box->first_child; child; child = child->next) {
        if (child->style.position == HPE_POS_STATIC) {
            paint_box(ctx, pm, child, scroll);
        } else {
            stacked = (HpeBox **)hpe_grow(stacked, &scap, sc + 1, sizeof(HpeBox *));
            stacked[sc++] = child;
            stacked_insert_sorted(stacked, (int)sc);
        }
    }
    for (r = 0; r < (int)sc; r++)
        paint_box(ctx, pm, stacked[r], scroll);
    if (stacked)
        hpe_free(stacked);
}

/* overflow clipping: hidden/scroll/auto establish a clip rect for the subtree.
 * Element-level scrolling is not modelled yet, so `scroll` and `auto` behave
 * like `hidden` for content that exceeds the box. */
static void paint_box_clipped_children(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll,
                                       float own_scroll)
{
    HpeStyle *bs = &box->style;
    int had_clip = 0;
    int saved[4] = {0, 0, 0, 0};
    if (bs->overflow_x != HPE_OVERFLOW_VISIBLE || bs->overflow_y != HPE_OVERFLOW_VISIBLE) {
        HpeRect clip = box->pad_box;
        clip.x += ctx->dev_x;
        clip.y = snap_y(ctx_dev_y(ctx, box->pad_box.y, own_scroll));
        saved[0] = pm->clip_x0;
        saved[1] = pm->clip_y0;
        saved[2] = pm->clip_x1;
        saved[3] = pm->clip_y1;
        hpe_pixmap_intersect_clip(pm, clip);
        had_clip = 1;
    }

    paint_box_children(ctx, pm, box, scroll);

    if (had_clip) {
        pm->clip_x0 = saved[0];
        pm->clip_y0 = saved[1];
        pm->clip_x1 = saved[2];
        pm->clip_y1 = saved[3];
    }
}

static void paint_box_inner(PaintCtx *ctx, HpePixmap *pm, HpeBox *box, float scroll)
{
    HpeStyle *bs = &box->style;
    HpeRect bb;
    /* fixed boxes are laid out in viewport coordinates: never scroll them,
     * and their subtrees inherit the same (zero) scroll offset */
    float own_scroll = bs->position == HPE_POS_FIXED ? 0.0f : scroll;
    float radius = bs->border_radius;
    /* `visibility: hidden` hides this box's own visuals (background, borders,
     * text, decorations) but the box still occupies space and its subtree is
     * still painted: descendants may set `visibility: visible` to reappear. */
    int self_visible = bs->visibility != HPE_VISIBILITY_HIDDEN;

    bb = box->border_box;
    bb.x += ctx->dev_x;
    bb.y = snap_y(ctx_dev_y(ctx, box->border_box.y, own_scroll));

    if (radius < 0.0f) { /* percentage: solved against the border box */
        float side = bb.w < bb.h ? bb.w : bb.h;
        radius = side * (-radius) * 0.01f;
    }

    if (self_visible)
        paint_box_own_content(ctx, pm, box, own_scroll, bb, radius);

    if (box->node && box->node->tag && !strcmp(box->node->tag, "svg")) {
        if (self_visible)
            hpe_svg_paint(pm, box, bb.y);
        return; /* SVG content is painted from the DOM, not from boxes */
    }

    paint_box_clipped_children(ctx, pm, box, scroll, own_scroll);
}

void hpe_paint_document(HpePixmap *pm, HpeBox *root, float scroll_y, HpeColor page_background,
                        float origin_y)
{
    PaintCtx ctx;
    if (!pm || !pm->pixels)
        return;
    hpe_pixmap_reset_clip(pm);
    hpe_pixmap_clear(pm, page_background);
    if (root) {
        ctx.dev_x = 0.0f;
        ctx.dev_y = origin_y;
        ctx.group_depth = 0;
        /* keep page content below the browser chrome */
        hpe_pixmap_set_clip(pm, hpe_rect(0.0f, origin_y, (float)pm->width,
                                         (float)pm->height - origin_y));
        paint_box(&ctx, pm, root, scroll_y);
        hpe_pixmap_reset_clip(pm);
    }
}

/* Partial repaint for damage-rect scrolling: erases only `clip` with the page
 * background, then repaints the document intersected with it. The pixmap's
 * clip rect constrains every raster op; callers may repaint several regions
 * (e.g. fixed-position boxes plus the exposed scroll strip). */
void hpe_paint_region(HpePixmap *pm, HpeBox *root, float scroll_y, HpeColor page_background,
                      HpeRect clip, float origin_y)
{
    PaintCtx ctx;
    if (!pm || !pm->pixels || hpe_rect_empty(clip))
        return;
    /* never paint into the chrome strip */
    if (clip.y < origin_y) {
        float bottom = clip.y + clip.h;
        clip.y = origin_y;
        clip.h = bottom - origin_y;
        if (clip.h <= 0.0f)
            return;
    }
    hpe_pixmap_set_clip(pm, clip);
    hpe_fill_rect(pm, clip, page_background);
    if (root) {
        ctx.dev_x = 0.0f;
        ctx.dev_y = origin_y;
        ctx.group_depth = 0;
        paint_box(&ctx, pm, root, scroll_y);
    }
    hpe_pixmap_reset_clip(pm);
}
