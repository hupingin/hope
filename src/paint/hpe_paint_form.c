/* Hope UI Engine - form control painting (see hpe_paint_form.h) */
#include "hpe_paint_form.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"
#include "render/hpe_font.h"
#include "render/hpe_raster.h"

/* the focused editable control this frame (set by the engine) */
static const HpeDomNode *g_focus_node = NULL;
static int g_focus_caret = 0;

/* control chrome palette (Chrome-ish defaults) */
#define FORM_BORDER hpe_rgb(118, 118, 118)
#define FORM_TRACK hpe_rgb(210, 210, 210)
#define FORM_THUMB hpe_rgb(90, 90, 90)
#define FORM_PLACEHOLDER hpe_rgb(160, 160, 160)
#define FORM_TICK hpe_rgb(0, 100, 200)
#define FORM_FILL hpe_rgb(0, 128, 0)

void hpe_paint_form_set_focus(const HpeDomNode *node, int caret)
{
    g_focus_node = node;
    g_focus_caret = caret;
}

/**
 * @brief 读控件某个数值属性 / Read one numeric attribute of a control
 */
static float ctl_float(const HpeDomNode *n, const char *name, float dflt)
{
    const char *v = hpe_dom_attr(n, name);
    float f;
    if (!v || !*v || sscanf(v, "%f", &f) != 1)
        return dflt;
    return f;
}

/**
 * @brief 取 select 当前选中的 option / Take a select's currently selected option
 */
static const HpeDomNode *selected_option_of(const HpeDomNode *sel)
{
    const HpeDomNode *c;
    for (c = sel->first_child; c; c = c->next_sibling) {
        if (c->type == HPE_NODE_ELEMENT && c->tag && !strcmp(c->tag, "option") &&
            hpe_dom_attr(c, "selected"))
            return c;
    }
    return NULL;
}

/**
 * @brief 取控件的显示文本（input 用 value，select 用选中项文本）/ Take a control's display text
 */
static void ctl_text(const HpeDomNode *node, char *out, int cap)
{
    const HpeDomNode *opt;
    const char *v;
    int len = 0;
    HpeDomNode *c;

    out[0] = '\0';
    if (!node)
        return;
    if (node->tag && !strcmp(node->tag, "select")) {
        opt = selected_option_of(node);
        if (!opt)
            return;
        v = hpe_dom_attr(opt, "value");
        if (v && *v) {
            snprintf(out, (size_t)cap, "%s", v);
            return;
        }
        for (c = opt->first_child; c; c = c->next_sibling) {
            if (c->type == HPE_NODE_TEXT && c->text) {
                snprintf(out + len, (size_t)(cap - len > 0 ? cap - len : 0), "%s", c->text);
                len = (int)strlen(out);
            }
        }
        return;
    }
    v = hpe_dom_attr(node, "value");
    snprintf(out, (size_t)cap, "%s", v ? v : "");
}

/**
 * @brief 求字符串的码点数 / Count the code points of a string
 */
static int cps_len(const char *s)
{
    int n = 0;
    while (*s) {
        uint32_t cp;
        int k = hpe_utf8_next(s, &cp);
        if (k <= 0)
            break;
        s += k;
        n++;
    }
    return n;
}

/**
 * @brief 在内容框内左侧绘制一段文本 / Draw a text string at the content box's left edge
 */
static void draw_value_text(HpePixmap *pm, const HpeBox *box, HpeRect bb, const char *text,
                            HpeColor color)
{
    HpeFont *font = hpe_font_get(box->style.font_size, box->style.font_weight,
                                 box->style.family);
    const HpeTextMask *mask;
    int x, y;

    if (!font || !text[0])
        return;
    x = (int)(bb.x + 4.0f);
    y = (int)(bb.y + bb.h * 0.5f + hpe_font_ascent(font) * 0.5f);
    mask = hpe_font_mask(font, text, (int)strlen(text));
    if (mask)
        hpe_draw_text_mask(pm, x, y, mask, color);
}

/**
 * @brief 绘制聚焦文本控件的插入符 / Paint the caret of the focused text control
 */
static void draw_caret(HpePixmap *pm, const HpeBox *box, HpeRect bb, const char *text)
{
    HpeFont *font = hpe_font_get(box->style.font_size, box->style.font_weight,
                                 box->style.family);
    float w;
    int pos;

    if (!font || g_focus_node != box->node)
        return;
    /* clamp the caret into the value and measure the prefix width */
    pos = g_focus_caret;
    if (pos > (int)strlen(text))
        pos = (int)strlen(text);
    w = hpe_font_measure(font, text, pos);
    hpe_fill_rect(pm, hpe_rect(bb.x + 4.0f + w, bb.y + 3.0f, 1.0f, bb.h - 6.0f),
                  hpe_rgb(20, 20, 20));
}

/**
 * @brief 绘制单行文本族控件（value / placeholder / 圆点 / 插入符）/ Paint a one-line text-family control
 */
static void paint_field(HpePixmap *pm, const HpeBox *box, HpeRect bb)
{
    char text[512];
    const char *type = hpe_dom_attr(box->node, "type");

    ctl_text(box->node, text, (int)sizeof(text));
    if (type && !strcmp(type, "password")) {
        int i, n = cps_len(text);
        float x = bb.x + 6.0f;
        float cy = bb.y + bb.h * 0.5f;
        for (i = 0; i < n; i++) {
            hpe_fill_rect(pm, hpe_rect(x, cy - 2.0f, 4.0f, 4.0f), hpe_rgb(40, 40, 40));
            x += 7.0f;
            if (x > bb.x + bb.w - 6.0f)
                break;
        }
    } else if (text[0]) {
        draw_value_text(pm, box, bb, text, box->style.color);
    } else {
        const char *ph = hpe_dom_attr(box->node, "placeholder");
        if (ph && *ph)
            draw_value_text(pm, box, bb, ph, FORM_PLACEHOLDER);
    }
    draw_caret(pm, box, bb, text);
}

/**
 * @brief 绘制复选框的勾选标记 / Paint a checkbox's tick mark
 */
static void paint_checkbox(HpePixmap *pm, HpeRect bb)
{
    float x0 = bb.x + bb.w * 0.2f;
    float y0 = bb.y + bb.h * 0.55f;
    float mid_x = bb.x + bb.w * 0.42f;
    float mid_y = bb.y + bb.h * 0.75f;
    float x1 = bb.x + bb.w * 0.8f;
    float y1 = bb.y + bb.h * 0.28f;
    float t;

    /* a tick drawn as two thick segments walked pixel by pixel */
    for (t = 0.0f; t <= 1.0f; t += 0.02f) {
        float ax = x0 + (mid_x - x0) * t;
        float ay = y0 + (mid_y - y0) * t;
        float bx = mid_x + (x1 - mid_x) * t;
        float by = mid_y + (y1 - mid_y) * t;
        hpe_fill_rect(pm, hpe_rect(ax, ay, 2.0f, 2.0f), FORM_TICK);
        hpe_fill_rect(pm, hpe_rect(bx, by, 2.0f, 2.0f), FORM_TICK);
    }
}

/**
 * @brief 绘制单选钮的圆点 / Paint a radio's inner dot
 */
static void paint_radio_dot(HpePixmap *pm, HpeRect bb)
{
    float cx = bb.x + bb.w * 0.5f;
    float cy = bb.y + bb.h * 0.5f;
    float r = bb.w * 0.22f;
    int x, y;

    for (y = 0; y < (int)bb.h; y++) {
        for (x = 0; x < (int)bb.w; x++) {
            float dx = (float)x + 0.5f - bb.w * 0.5f;
            float dy = (float)y + 0.5f - bb.h * 0.5f;
            if (dx * dx + dy * dy <= r * r)
                hpe_fill_rect(pm, hpe_rect(cx + dx, cy + dy, 1.0f, 1.0f), FORM_TICK);
        }
    }
}

/**
 * @brief 绘制滑杆（轨道 + 拇指）/ Paint a range slider (track + thumb)
 */
static void paint_range(HpePixmap *pm, const HpeBox *box, HpeRect bb)
{
    float min = ctl_float(box->node, "min", 0.0f);
    float max = ctl_float(box->node, "max", 100.0f);
    float val = ctl_float(box->node, "value", min);
    float frac = (max > min) ? (val - min) / (max - min) : 0.0f;
    float tx;
    float cy = bb.y + bb.h * 0.5f;

    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;
    hpe_fill_rect(pm, hpe_rect(bb.x + 2.0f, cy - 1.0f, bb.w - 4.0f, 2.0f), FORM_TRACK);
    tx = bb.x + 2.0f + frac * (bb.w - 14.0f);
    hpe_fill_round_rect(pm, hpe_rect(tx, bb.y + 2.0f, 12.0f, bb.h - 4.0f), 3.0f, FORM_THUMB);
}

/**
 * @brief 解析 `#rrggbb` 颜色 / Parse an `#rrggbb` colour
 */
static HpeColor parse_swatch(const char *v)
{
    int r = 0, g = 0, b = 0;
    if (v && v[0] == '#' && sscanf(v + 1, "%2x%2x%2x", &r, &g, &b) == 3)
        return hpe_rgb(r, g, b);
    return hpe_rgb(0, 0, 0);
}

/**
 * @brief 绘制文件选择（内部按钮 + 文件名）/ Paint a file picker (inner button + filename)
 */
static void paint_file(HpePixmap *pm, const HpeBox *box, HpeRect bb)
{
    char name[256];
    HpeRect btn = hpe_rect(bb.x + 3.0f, bb.y + 3.0f, 60.0f, bb.h - 6.0f);
    HpeFont *font = hpe_font_get(box->style.font_size, box->style.font_weight,
                                 box->style.family);
    const HpeTextMask *mask;
    const char *v;

    hpe_fill_round_rect(pm, btn, 2.0f, hpe_rgb(239, 239, 239));
    if (font) {
        mask = hpe_font_mask(font, "浏览...", (int)strlen("浏览..."));
        if (mask)
            hpe_draw_text_mask(pm, (int)(btn.x + 8.0f),
                               (int)(btn.y + btn.h * 0.5f + hpe_font_ascent(font) * 0.5f),
                               mask, hpe_rgb(20, 20, 20));
    }
    v = hpe_dom_attr(box->node, "value");
    snprintf(name, sizeof(name), "%s", (v && *v) ? v : "未选择文件");
    draw_value_text(pm, box, hpe_rect(btn.x + btn.w + 4.0f, bb.y, bb.w, bb.h), name,
                    hpe_rgb(20, 20, 20));
}

/**
 * @brief 绘制 select 的下拉箭头 / Paint a select's dropdown arrow
 */
static void paint_select_arrow(HpePixmap *pm, HpeRect bb)
{
    float cx = bb.x + bb.w - 12.0f;
    float cy = bb.y + bb.h * 0.5f;
    int i;

    for (i = 0; i < 5; i++)
        hpe_fill_rect(pm, hpe_rect(cx - 5.0f + (float)i, cy - 1.0f + (float)i,
                                   10.0f - 2.0f * (float)i, 1.0f),
                      FORM_THUMB);
}

/**
 * @brief 绘制进度 / 度量条的填充 / Paint a progress / meter fill
 */
static void paint_bar_fill(HpePixmap *pm, const HpeBox *box, HpeRect bb)
{
    float max = ctl_float(box->node, "max", 1.0f);
    float val = ctl_float(box->node, "value", 0.0f);
    float frac;

    if (max <= 0.0f)
        max = 1.0f;
    frac = val / max;
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;
    if (frac > 0.0f)
        hpe_fill_rect(pm, hpe_rect(bb.x + 2.0f, bb.y + 2.0f,
                                   (bb.w - 4.0f) * frac, bb.h - 4.0f),
                      FORM_FILL);
}

void hpe_paint_form_control(HpePixmap *pm, const HpeBox *box, HpeRect bb)
{
    const char *tag = box->node->tag ? box->node->tag : "";
    const char *type = hpe_dom_attr(box->node, "type");

    if (!strcmp(tag, "input")) {
        if (!type || !*type || !strcmp(type, "text") || !strcmp(type, "search") ||
            !strcmp(type, "email") || !strcmp(type, "url") || !strcmp(type, "tel") ||
            !strcmp(type, "number") || !strcmp(type, "date") || !strcmp(type, "time") ||
            !strcmp(type, "datetime-local"))
            paint_field(pm, box, bb);
        else if (!strcmp(type, "password"))
            paint_field(pm, box, bb);
        else if (!strcmp(type, "checkbox")) {
            if (hpe_dom_attr(box->node, "checked"))
                paint_checkbox(pm, bb);
        } else if (!strcmp(type, "radio")) {
            if (hpe_dom_attr(box->node, "checked"))
                paint_radio_dot(pm, bb);
        } else if (!strcmp(type, "range"))
            paint_range(pm, box, bb);
        else if (!strcmp(type, "color")) {
            HpeRect sw = hpe_rect(bb.x + 3.0f, bb.y + 3.0f, bb.w - 6.0f, bb.h - 6.0f);
            hpe_fill_rect(pm, sw, parse_swatch(hpe_dom_attr(box->node, "value")));
        } else if (!strcmp(type, "file"))
            paint_file(pm, box, bb);
        return;
    }
    if (!strcmp(tag, "select")) {
        char text[256];
        ctl_text(box->node, text, (int)sizeof(text));
        draw_value_text(pm, box, bb, text, box->style.color);
        paint_select_arrow(pm, bb);
        return;
    }
    if (!strcmp(tag, "progress") || !strcmp(tag, "meter")) {
        hpe_fill_rect(pm, hpe_rect(bb.x + 2.0f, bb.y + 2.0f, bb.w - 4.0f, bb.h - 4.0f),
                      FORM_TRACK);
        paint_bar_fill(pm, box, bb);
        return;
    }
}
