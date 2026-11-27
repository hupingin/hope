#include "css/hpe_css.h"

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* forward declarations used by the public selector-matching entry point */
static int selector_matches(const HpeSelector *sel, const HpeDomNode *node);
static void parse_selector_list(HpeRule *rule, const char *text, size_t length);
static int parse_compound(const char **pp, const char *end, HpeCompound *out);
static void selector_free(HpeSelector *sel);
/* recursive stylesheet walker; @media blocks re-enter it with a deeper stack */
static void css_parse_into(HpeStylesheet *sheet, const char *css, size_t length,
                           HpeMediaQuery *mq_stack, int mq_depth);

/**
 * @brief 判断字符是否为 ASCII 十进制数字 / Test whether a character is an ASCII decimal digit
 *
 * @param[in]     c 待检查字符 / Character to test
 *
 * @return 数字返回 1，否则 0 / 1 for a digit, 0 otherwise
 */
static int css_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* ---------------- defaults ---------------- */

/**
 * @brief 写入盒模型相关长属性的非零初值 / Write the non-zero initial values of the box-model longhands
 *
 * @details 填充背景/边框/宽高/内外边距/inset 等长度与颜色字段，并把 `transform` 置为单位矩阵。
 *          It fills the length and colour fields for background, border, width/height, margins, padding and inset, and sets `transform` to the identity matrix.
 *          CSS 初值本身即 0 的字段交给调用方的 `memset`，这里只写非零初值。
 *          Fields whose CSS initial value is already zero are left to the caller's `memset`; only non-zero initials are written here.
 *
 * @param[in,out] s 待初始化的样式 / Style being initialised
 *
 * @see style_init_scalars hpe_style_default
 */
static void style_init_box(HpeStyle *s)
{
    int i;

    s->background = hpe_rgba(0, 0, 0, 0);
    s->border_color = hpe_rgba(0, 0, 0, 0);
    s->width = hpe_len_auto();
    s->height = hpe_len_auto();
    s->min_width = hpe_len_auto();
    s->min_height = hpe_len_auto();
    s->max_width = hpe_len_auto();
    s->max_height = hpe_len_auto();
    for (i = 0; i < 4; i++) {
        s->margin[i] = hpe_len_px(0.0f);
        s->padding[i] = hpe_len_px(0.0f);
        s->border_colors[i] = hpe_rgba(0, 0, 0, 0);
        s->inset[i] = hpe_len_auto();
    }
    s->letter_spacing = hpe_len_px(0.0f);
    s->word_spacing = hpe_len_px(0.0f);
    s->text_indent = hpe_len_px(0.0f);
    /* 2-D identity: the off-diagonals stay zero from the memset above */
    s->transform.a = 1.0f;
    s->transform.d = 1.0f;
    s->transform.identity = 1;
}

/**
 * @brief 写入枚举与标量长属性的非零初值 / Write the non-zero initial values of the enum and scalar longhands
 *
 * @details 设置颜色、`display: inline`、`font-size: 16px`、`font-weight: 400`、`line-height: 0`（表示 `normal`）以及各枚举属性的初始枚举值，并令 `background_image.kind` 为 `HPE_BG_NONE`。
 *          It sets the colour, `display: inline`, `font-size: 16px`, `font-weight: 400`, `line-height: 0` (meaning `normal`), the initial enum value of every enum property, and `background_image.kind` to `HPE_BG_NONE`.
 *
 * @param[in,out] s 待初始化的样式 / Style being initialised
 *
 * @see style_init_box hpe_style_default
 */
static void style_init_scalars(HpeStyle *s)
{
    s->color = hpe_rgb(0, 0, 0);
    s->display = HPE_DISPLAY_INLINE;
    s->font_size = 16.0f;   /* CSS `medium` */
    s->font_weight = 400;   /* CSS `normal` */
    s->line_height = 0.0f;  /* 0 means `normal` */
    s->family = HPE_FONT_DEFAULT;
    s->flex_dir = HPE_FLEX_ROW;
    s->align_items = HPE_ALIGN_ITEMS_STRETCH;
    s->justify = HPE_JUSTIFY_START;
    s->align_self = HPE_ALIGN_ITEMS_AUTO;
    s->text_align = HPE_ALIGN_LEFT;
    s->white_space = HPE_WS_NORMAL;
    s->position = HPE_POS_STATIC;
    s->z_auto = 1;
    s->box_sizing = HPE_BOX_SIZING_CONTENT;
    s->overflow_x = HPE_OVERFLOW_VISIBLE;
    s->overflow_y = HPE_OVERFLOW_VISIBLE;
    s->opacity = 1.0f;
    s->visibility = HPE_VISIBILITY_VISIBLE;
    s->cursor = HPE_CURSOR_AUTO;
    s->text_decoration = HPE_TD_NONE;
    s->vertical_align = HPE_VALIGN_BASELINE;
    s->list_style_type = HPE_LIST_DISC;
    s->list_style_position = HPE_LIST_OUTSIDE;
    s->background_image.kind = HPE_BG_NONE;
    s->flex_wrap = 0;      /* nowrap */
    s->order = 0;
    s->flex_shrink = 1.0f; /* CSS initial value */
    s->flex_basis = hpe_len_auto();
    s->table_layout_fixed = 0;
    s->border_spacing_h = 0.0f;
    s->border_spacing_v = 0.0f;
    s->direction_rtl = 0;
    /* 经典滚动条的占位宽度：`scrollbar-width` 的初始值是 `auto`，即 12px 的经典
     * 滚动条（不是 overlay 式）。0 是一个合法值，所以默认值不能用 0 当哨兵。 */
    s->scrollbar_width = HPE_SCROLLBAR_WIDTH_AUTO;
    /* replaced elements default to `object-fit: fill` and centre their content */
    s->object_fit = HPE_OBJECT_FIT_FILL;
    s->bg_repeat = HPE_BG_REPEAT_REPEAT;
    s->object_pos.present = 0;
    s->object_pos.x = 50.0f;
    s->object_pos.y = 50.0f;
    s->object_pos.x_pct = 1;
    s->object_pos.y_pct = 1;
}

/**
 * @brief 返回一份填好 CSS 初始值的默认样式 / Return a default style seeded with CSS initial values
 *
 * @details 先 `memset` 清零，再依次调用 `style_init_box` 与 `style_init_scalars`，使所有非零初值就位。
 *          The struct is zeroed and then `style_init_box` and `style_init_scalars` bring every non-zero initial value into place.
 *
 * @return 初始化完毕的样式值（按值返回）/ A fully initialised style returned by value
 *
 * @see style_init_box style_init_scalars
 */
HpeStyle hpe_style_default(void)
{
    HpeStyle s;

    memset(&s, 0, sizeof(s));
    style_init_box(&s);
    style_init_scalars(&s);
    return s;
}

/**
 * @brief 按空白切分字符串，并保持括号内的空白 / Split a string on whitespace while keeping whitespace inside parentheses
 *
 * @details 就地以 `\0` 截断并写入各段起始指针，最多记录 `max` 段；括号深度大于 0 的空白不视为分隔，因此 `func(a, b, c)` 仍是一个 token。
 *          Segments are NUL-terminated in place and their start pointers stored, at most `max` of them; whitespace at paren depth greater than 0 is not a separator, so `func(a, b, c)` stays a single token.
 *
 * @param[in,out] s 待切分的字符串（会被就地改写）/ String to split, modified in place
 * @param[out]    out 各段起始指针数组 / Array that receives each segment pointer
 * @param[in]     max 最多可写入的段数 / Maximum number of segments to store
 *
 * @return 实际切分出的段数 / Number of segments written
 *
 * @see split_top_level
 */

static int split_ws(char *s, char **out, int max)
{
    int n = 0;
    int depth = 0;
    char *p = s;
    while (*p) {
        while (*p && hpe_is_space(*p))
            p++;
        if (!*p)
            break;
        if (n < max)
            out[n++] = p;
        /* keep `func(a, b, c)` whitespace inside the parens as one token */
        depth = 0;
        while (*p) {
            if (*p == '(')
                depth++;
            else if (*p == ')') {
                if (depth > 0)
                    depth--;
            } else if (hpe_is_space(*p) && depth <= 0) {
                break;
            }
            p++;
        }
        if (*p)
            *p++ = '\0';
    }
    return n;
}

/**
 * @brief 解析一个不含单位的纯数字 / Parse a bare number with no unit
 *
 * @details 用 `strtod` 读取后要求其后只剩空白，否则视为解析失败，因此带单位的长度不会通过。
 *          After `strtod` the remainder must be whitespace only, so a length carrying a unit does not pass.
 *
 * @param[in]     s 待解析的字符串 / String to parse
 * @param[out]    out 解析出的数值 / Receives the parsed number
 *
 * @return 成功返回 1，失败返回 0 / 1 on success, 0 on failure
 *
 * @see parse_float_token media_feature_value
 */
static int parse_plain_number(const char *s, float *out)
{
    char *end = NULL;
    float v;
    if (!s || !*s)
        return 0;
    v = (float)strtod(s, &end);
    if (end == s)
        return 0;
    while (end && *end && hpe_is_space(*end))
        end++;
    if (end && *end)
        return 0;
    *out = v;
    return 1;
}

static void set_sides(HpeStyle *d, const char *value, int is_margin){
    char buf[256];
    char *parts[4];
    int n, i;
    static const uint64_t bits[4][4] = {
        {HPE_PROP_MARGIN_TOP, HPE_PROP_MARGIN_RIGHT, HPE_PROP_MARGIN_BOTTOM, HPE_PROP_MARGIN_LEFT},
        {HPE_PROP_PADDING_TOP, HPE_PROP_PADDING_RIGHT, HPE_PROP_PADDING_BOTTOM,
         HPE_PROP_PADDING_LEFT},
        {0, 0, 0, 0},
        {0, 0, 0, 0}};
    HpeLength vals[4];

    snprintf(buf, sizeof(buf), "%s", value ? value : "0");
    n = split_ws(buf, parts, 4);
    if (n <= 0)
        return;
    for (i = 0; i < 4; i++) {
        const char *v = parts[i < n ? i : n - 1];
        vals[i] = hpe_len_parse(v);
    }
    if (n < 4) {
        /* expand: 1 value -> all, 2 -> tb/lr, 3 -> t/lr/b */
        HpeLength t = vals[0];
        HpeLength r = n > 1 ? vals[1] : t;
        HpeLength b = n > 2 ? vals[2] : t;
        HpeLength l = n > 1 ? vals[1] : t;
        vals[0] = t;
        vals[1] = r;
        vals[2] = b;
        vals[3] = l;
    }
    for (i = 0; i < 4; i++) {
        if (is_margin)
            d->margin[i] = vals[i];
        else
            d->padding[i] = vals[i];
        d->set |= bits[is_margin ? 0 : 1][i];
    }
}

/* ---------------- gradient / shadow / transform helpers ---------------- */

/**
 * @brief 解析角度/数值 token，支持 `deg`/`rad`/`turn` 等后缀 / Parse an angle or number token, accepting `deg`, `rad` and `turn` suffixes
 *
 * @details 带 `rad` 时换算为度、带 `turn` 时乘以 360；允许的后缀为 `deg`/`px`/`%`/`rad`/`turn`，其它非空后缀判为失败。
 *          `rad` is converted to degrees and `turn` multiplied by 360; the accepted suffixes are `deg`/`px`/`%`/`rad`/`turn`, and any other trailing text fails the parse.
 *
 * @param[in]     s 待解析的 token / Token to parse
 * @param[out]    out 解析出的数值（角度已换算为度）/ Receives the parsed number with angles converted to degrees
 *
 * @return 成功返回 1，失败返回 0 / 1 on success, 0 on failure
 *
 * @see parse_plain_number
 */
static int parse_float_token(const char *s, float *out)
{
    char *end = NULL;
    float v;
    if (!s || !*s)
        return 0;
    v = (float)strtod(s, &end);
    if (end == s)
        return 0;
    while (*end && hpe_is_space(*end))
        end++;
    if (*end && strcmp(end, "deg") != 0 && strcmp(end, "px") != 0 &&
        strcmp(end, "%") != 0 && strcmp(end, "rad") != 0 && strcmp(end, "turn") != 0)
        return 0;
    if (*end == '\0')
        ; /* plain number */
    else if (!strcmp(end, "rad"))
        v = v * 180.0f / 3.14159265f;
    else if (!strcmp(end, "turn"))
        v = v * 360.0f;
    *out = v;
    return 1;
}

/**
 * @brief 按顶层逗号切分，忽略括号内的逗号 / Split on top-level commas, ignoring commas inside parentheses
 *
 * @details 只在括号深度为 0 的逗号处切断，颜色函数自带的逗号因此不会拆开参数；最多记录 `max` 段并返回段数。
 *          Splitting only happens at commas where the paren depth is 0, so commas inside a colour function do not break a segment; up to `max` segments are stored and their count returned.
 *
 * @param[in,out] body 待切分的文本（会被就地改写）/ Text to split, modified in place
 * @param[out]    seg 各段起始指针数组 / Array that receives each segment pointer
 * @param[in]     max 最多可写入的段数 / Maximum number of segments to store
 *
 * @return 实际切分出的段数 / Number of segments written
 *
 * @see split_ws collect_gradient_stops
 */
static int split_top_level(char *body, char **seg, int max)
{
    int depth = 0;
    int nseg = 0;
    char *start = body;
    char *q;

    for (q = body; *q; q++) {
        if (*q == '(')
            depth++;
        else if (*q == ')')
            depth--;
        else if (*q == ',' && depth == 0) {
            *q = '\0';
            if (nseg < max)
                seg[nseg++] = start;
            start = q + 1;
        }
    }
    if (nseg < max)
        seg[nseg++] = start;
    return nseg;
}

/**
 * @brief 把 `to <side>` 关键字或角度 token 映射为度数 / Map a `to <side>` keyword or angle token to degrees
 *
 * @details `to top`/`bottom`/`left`/`right` 及四个对角关键字映射为对应的度数值（0 表示向上、90 表示向右）；也接受带 `deg`/`rad`/`turn` 的角度 token。
 *          `to top`/`bottom`/`left`/`right` and the four corner keywords map to their degree values (0 points up, 90 points right); an angle token carrying `deg`/`rad`/`turn` is also accepted.
 *
 * @param[in]     tok 待识别的 token / Token to recognise
 * @param[out]    angle 解析出的角度（度）/ Receives the angle in degrees
 *
 * @return 是方向 token 返回 1，否则返回 0 / 1 when the token was a direction, 0 otherwise
 *
 * @note 返回 1 的 token 不应再当作色标处理 / A token that returns 1 must not also be treated as a colour stop
 *
 * @see collect_gradient_stops parse_gradient_stop
 */
static int gradient_direction(const char *tok, float *angle)
{
    if (hpe_starts_with_i(tok, "to ")) {
        char *dir = hpe_trim((char *)tok + 3);
        if (hpe_str_ieq(dir, "top"))
            *angle = 0.0f;
        else if (hpe_str_ieq(dir, "bottom"))
            *angle = 180.0f;
        else if (hpe_str_ieq(dir, "left"))
            *angle = 270.0f;
        else if (hpe_str_ieq(dir, "right"))
            *angle = 90.0f;
        else if (hpe_str_ieq(dir, "top right") || hpe_str_ieq(dir, "right top"))
            *angle = 45.0f;
        else if (hpe_str_ieq(dir, "bottom right") || hpe_str_ieq(dir, "right bottom"))
            *angle = 135.0f;
        else if (hpe_str_ieq(dir, "top left") || hpe_str_ieq(dir, "left top"))
            *angle = 315.0f;
        else if (hpe_str_ieq(dir, "bottom left") || hpe_str_ieq(dir, "left bottom"))
            *angle = 225.0f;
        return 1;
    }
    {
        float a;
        if (parse_float_token(tok, &a) &&
            (strstr(tok, "deg") || strstr(tok, "rad") || strstr(tok, "turn"))) {
            *angle = a;
            return 1;
        }
    }
    return 0;
}

/**
 * @brief 解析 `<颜色> [<位置>]` 形式的色标 / Parse a `<color> [<position>]` gradient stop
 *
 * @details 颜色与位置以空白分隔，位置缺省时置为 -1（稍后由 `distribute_gradient_stops` 补齐）；位置大于 1 或带 `%` 时按百分数换算为 0..1。
 *          The colour and position are whitespace-separated and a missing position is marked -1 for `distribute_gradient_stops` to fill in later; a position above 1 or carrying `%` is treated as a percentage scaled to 0..1.
 *
 * @param[in]     tok 待解析的色标 token / Stop token to parse
 * @param[out]    c 解析出的颜色 / Receives the parsed colour
 * @param[out]    pos 解析出的位置 0..1，缺省为 -1 / Receives the position in 0..1, or -1 when absent
 *
 * @return token 中含有颜色返回 1，否则返回 0 / 1 when the token held a colour, 0 otherwise
 *
 * @see collect_gradient_stops distribute_gradient_stops
 */
static int parse_gradient_stop(const char *tok, HpeColor *c, float *pos)
{
    char buf[128];
    char *sp;
    float pv;

    *pos = -1.0f;
    snprintf(buf, sizeof(buf), "%s", tok);
    sp = buf;
    while (*sp && !hpe_is_space(*sp))
        sp++;
    if (*sp) {
        *sp = '\0';
        if (parse_float_token(hpe_trim(sp + 1), &pv))
            *pos = (pv > 1.0f || strstr(hpe_trim(sp + 1), "%")) ? pv / 100.0f : pv;
    }
    return hpe_color_parse(hpe_trim(buf), c);
}

/**
 * @brief 为未设置位置的色标按均匀分布补齐位置 / Fill in unset stop positions by even distribution
 *
 * @details 首尾色标缺省时分别置为 0 与 1，中间色标缺省时按下标均分到 [0,1]，已显式给出的位置保持不变。
 *          The first and last stops default to 0 and 1 and interior stops default to an even division of [0,1]; explicitly given positions are left untouched.
 *
 * @param[in,out] bg 渐变描述 / Gradient description
 * @param[in]     count 色标个数 / Number of colour stops
 *
 * @see collect_gradient_stops
 */
static void distribute_gradient_stops(HpeBackground *bg, int count)
{
    int i;
    if (bg->stops[0].pos < 0.0f)
        bg->stops[0].pos = 0.0f;
    if (bg->stops[count - 1].pos < 0.0f)
        bg->stops[count - 1].pos = 1.0f;
    for (i = 1; i < count - 1; i++) {
        if (bg->stops[i].pos < 0.0f)
            bg->stops[i].pos = (float)i / (float)(count - 1);
    }
}

/**
 * @brief 把 `linear-gradient(...)` 的参数体解析为色标数组 / Parse a `linear-gradient(...)` body into the stop array
 *
 * @details 按顶层逗号分段，跳过空段与方向段，颜色段写入 `bg->stops`（上限 `HPE_MAX_GRADIENT_STOPS`），同时输出首个方向段解析出的角度。
 *          The body is split on top-level commas; empty and direction segments are skipped, colour segments are written into `bg->stops` up to `HPE_MAX_GRADIENT_STOPS`, and the angle parsed from the leading direction segment is returned.
 *
 * @param[in,out] body 渐变的参数体（会被就地改写）/ Gradient argument body, modified in place
 * @param[in,out] bg 接收色标的渐变描述 / Gradient description that receives the stops
 * @param[out]    angle 渐变角度（度）/ Receives the gradient angle in degrees
 *
 * @return 写入的色标个数 / Number of stops written
 *
 * @see parse_linear_gradient distribute_gradient_stops
 */
static int collect_gradient_stops(char *body, HpeBackground *bg, float *angle)
{
    char *seg[16];
    int nseg = split_top_level(body, seg, 16);
    int count = 0;
    int i;

    for (i = 0; i < nseg; i++) {
        char *tok = hpe_trim(seg[i]);
        HpeColor c;
        float pos;
        if (!*tok)
            continue;
        if (gradient_direction(tok, angle))
            continue;
        if (!parse_gradient_stop(tok, &c, &pos))
            continue;
        if (count < HPE_MAX_GRADIENT_STOPS) {
            bg->stops[count].color = c;
            bg->stops[count].pos = pos;
            count++;
        }
    }
    return count;
}

/**
 * @brief 解析 `linear-gradient(...)` 为背景描述 / Parse `linear-gradient(...)` into a background description
 *
 * @details 仅支持带可选起始方向（`to right`、`to bottom`、`Ndeg` 等）的 2 段及以上的颜色列表形式；角度缺省为 `180deg`（to bottom），色标少于 2 个视为失败。
 *          Only the 2-or-more-stop colour-list form with an optional leading direction such as `to right`, `to bottom` or `Ndeg` is supported; the angle defaults to `180deg` (to bottom) and fewer than two stops fails the parse.
 *
 * @param[in]     value 属性值文本 / Property value text
 * @param[in,out] bg 接收解析结果的背景描述 / Background description that receives the result
 *
 * @return 成功返回 1，否则返回 0 / 1 on success, 0 otherwise
 *
 * @note 成功时把 `bg->kind` 置为 `HPE_BG_LINEAR_GRADIENT` / On success `bg->kind` is set to `HPE_BG_LINEAR_GRADIENT`
 *
 * @see collect_gradient_stops apply_background apply_background_image
 */
static int parse_linear_gradient(const char *value, HpeBackground *bg)
{
    const char *lp, *rp;
    int count;
    float angle = 180.0f; /* css default: to bottom */
    bg->kind = HPE_BG_NONE;
    bg->stop_count = 0;
    bg->angle_deg = 180.0f;
    if (!value)
        return 0;
    while (*value && hpe_is_space(*value))
        value++;
    if (!hpe_starts_with_i(value, "linear-gradient"))
        return 0;
    lp = strchr(value, '(');
    if (!lp)
        return 0;
    rp = strrchr(lp, ')');
    if (!rp || rp <= lp)
        return 0;

    {
        char *body = hpe_strndup(lp + 1, (size_t)(rp - lp - 1));
        count = collect_gradient_stops(body, bg, &angle);
        hpe_free(body);
    }
    if (count < 2)
        return 0;
    distribute_gradient_stops(bg, count);
    bg->kind = HPE_BG_LINEAR_GRADIENT;
    bg->angle_deg = angle;
    bg->stop_count = count;
    return 1;
}

/**
 * @brief 解析 `[ox oy blur spread] color` 形式的阴影 / Parse a `[ox oy blur spread] color` shadow
 *
 * @details 逐个 token 分类：能解析为颜色的作为颜色（缺省为半透明黑），能解析为数字的依次填入偏移/模糊/扩散；至少要有两个数字才算成功，颜色可出现在任意位置（前导或尾随）。
 *          Each token is classified: one that parses as a colour becomes the colour (defaulting to translucent black) and numeric ones fill offset, blur and spread in order; at least two numbers are required and the colour may lead or trail.
 *
 * @param[in]     value 属性值文本 / Property value text
 * @param[out]    out 接收阴影描述 / Receives the shadow description
 *
 * @return 成功返回 1，否则返回 0 / 1 on success, 0 otherwise
 *
 * @note 成功时把 `out->valid` 置为 1 / `out->valid` is set to 1 on success
 *
 * @see apply_box_shadow apply_text_shadow
 */
static int parse_shadow(const char *value, HpeShadow *out)
{
    char buf[256];
    char *parts[8];
    float nums[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int nnums = 0;
    HpeColor c = hpe_rgba(0, 0, 0, 160);
    int n, i, got_color = 0;

    memset(out, 0, sizeof(*out));
    if (!value)
        return 0;
    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 8);
    for (i = 0; i < n; i++) {
        HpeColor cc;
        float v;
        if (hpe_color_parse(parts[i], &cc)) {
            c = cc;
            got_color = 1;
        } else if (parse_float_token(parts[i], &v) && nnums < 4) {
            nums[nnums++] = v;
        }
    }
    if (nnums < 2)
        return 0;
    HPE_UNUSED(got_color);
    out->ox = nums[0];
    out->oy = nums[1];
    out->blur = nums[2];
    out->spread = nums[3];
    out->color = c;
    out->valid = 1;
    return 1;
}

/**
 * @brief 计算两个仿射矩阵的乘积 `m * n` / Compute the product `m * n` of two affine matrices
 *
 * @details 计算方式是先应用 `n` 再应用 `m`，结果为 2D 仿射（末行为 `0 0 1`），并据结果是否等于单位矩阵更新 `identity` 标志。
 *          It applies `n` first and then `m`, produces a 2D affine matrix (implicit last row `0 0 1`), and updates the `identity` flag according to whether the result equals the identity.
 *
 * @param[out]    dst 接收乘积的矩阵 / Receives the product matrix
 * @param[in]     m 左矩阵 / Left matrix
 * @param[in]     n 右矩阵 / Right matrix
 *
 * @note `dst` 可与 `m` 或 `n` 为同一对象（就地累乘）/ `dst` may alias `m` or `n` for in-place accumulation
 *
 * @see transform_from_fn apply_transform_fn
 */
static void mat_mul(HpeTransform *dst, const HpeTransform *m, const HpeTransform *n)
{
    /* dst = m * n (apply n then m) */
    float a = m->a * n->a + m->c * n->b;
    float b = m->b * n->a + m->d * n->b;
    float c = m->a * n->c + m->c * n->d;
    float d = m->b * n->c + m->d * n->d;
    float e = m->a * n->e + m->c * n->f + m->e;
    float f = m->b * n->e + m->d * n->f + m->f;
    dst->a = a;
    dst->b = b;
    dst->c = c;
    dst->d = d;
    dst->e = e;
    dst->f = f;
    dst->identity = (a == 1.0f && b == 0.0f && c == 0.0f && d == 1.0f && e == 0.0f && f == 0.0f);
}

/**
 * @brief 为单个变换函数名与两个参数构建仿射矩阵 / Build the affine matrix for one transform function name and its two arguments
 *
 * @details 支持 `translate`/`translatex`/`translatey`、`scale`/`scalex`/`scaley`、`rotate` 与 `skewx`/`skewy`；`rotate` 与 `skew` 的角度参数按度转弧度；`matrix(a,b,c,d,e,f)` 由 `apply_transform_fn` 单独处理。
 *          It supports `translate`/`translatex`/`translatey`, `scale`/`scalex`/`scaley`, `rotate` and `skewx`/`skewy`; the angles of `rotate` and the skew functions convert from degrees to radians; `matrix(a,b,c,d,e,f)` is handled by `apply_transform_fn` directly.
 *
 * @param[in]     fn 小写的变换函数名 / Lowercased transform function name
 * @param[in]     v1 第一个参数 / First argument
 * @param[in]     v2 第二个参数 / Second argument
 * @param[out]    m 接收构建出的矩阵 / Receives the built matrix
 *
 * @see apply_transform_fn parse_transform
 */
static void transform_from_fn(const char *fn, float v1, float v2, HpeTransform *m)
{
    m->a = 1.0f;
    m->b = 0.0f;
    m->c = 0.0f;
    m->d = 1.0f;
    m->e = 0.0f;
    m->f = 0.0f;

    if (!strcmp(fn, "translate"))
        m->e = v1, m->f = v2;
    else if (!strcmp(fn, "translatex"))
        m->e = v1;
    else if (!strcmp(fn, "translatey"))
        m->f = v1;
    else if (!strcmp(fn, "scale"))
        m->a = m->d = v1, m->b = m->c = 0.0f, m->e = m->f = 0.0f;
    else if (!strcmp(fn, "scalex"))
        m->a = v1;
    else if (!strcmp(fn, "scaley"))
        m->d = v1;
    else if (!strcmp(fn, "rotate")) {
        float r = v1 * 3.14159265f / 180.0f;
        m->a = cosf(r);
        m->b = sinf(r);
        m->c = -sinf(r);
        m->d = cosf(r);
    } else if (!strcmp(fn, "skewx")) {
        float r = v1 * 3.14159265f / 180.0f;
        m->c = tanf(r);
    } else if (!strcmp(fn, "skewy")) {
        float r = v1 * 3.14159265f / 180.0f;
        m->b = tanf(r);
    }
    m->identity =
        (m->a == 1.0f && m->b == 0.0f && m->c == 0.0f && m->d == 1.0f && m->e == 0.0f &&
         m->f == 0.0f);
}

/**
 * @brief 把一个 `name(args)` 变换累积到累加矩阵上 / Accumulate one `name(args)` transform onto the accumulator matrix
 *
 * @details 复制参数体后按逗号拆出最多两个数值，构建该函数的矩阵；仅在矩阵非单位矩阵时左乘累加，因此 `translate(0)` 一类不改变结果。
 *          The argument body is copied and split on the comma into at most two numbers to build the function's matrix; it is left-multiplied onto the accumulator only when the matrix is not the identity, so calls like `translate(0)` change nothing.
 *
 * @param[in]     fn 小写的变换函数名 / Lowercased transform function name
 * @param[in]     body 函数括号内的参数文本 / Argument text inside the function parentheses
 * @param[in,out] acc 累加矩阵 / Accumulator matrix
 *
 * @see transform_from_fn parse_transform mat_mul
 */
static void apply_transform_fn(const char *fn, const char *body, HpeTransform *acc)
{
    char *copy = hpe_strdup(body);
    float v1 = 0.0f, v2 = 0.0f;
    char *comma = strchr(copy, ',');
    HpeTransform m;

    if (!strcmp(fn, "matrix")) {
        /* matrix(a, b, c, d, e, f): the full 2D affine matrix */
        float mv[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        char *tok = copy;
        int i;
        for (i = 0; i < 6 && tok; i++) {
            char *next = strchr(tok, ',');
            if (next)
                *next = '\0';
            parse_float_token(hpe_trim(tok), &mv[i]);
            tok = next ? next + 1 : NULL;
        }
        m.a = mv[0];
        m.b = mv[1];
        m.c = mv[2];
        m.d = mv[3];
        m.e = mv[4];
        m.f = mv[5];
        m.identity = (m.a == 1.0f && m.b == 0.0f && m.c == 0.0f && m.d == 1.0f &&
                      m.e == 0.0f && m.f == 0.0f);
        if (!m.identity)
            mat_mul(acc, acc, &m);
        hpe_free(copy);
        return;
    }
    if (comma) {
        *comma = '\0';
        parse_float_token(hpe_trim(comma + 1), &v2);
    }
    parse_float_token(hpe_trim(copy), &v1);
    transform_from_fn(fn, v1, v2, &m);
    if (!m.identity)
        mat_mul(acc, acc, &m);
    hpe_free(copy);
}

/**
 * @brief 解析逗号/空白分隔的 2D 变换函数列表 / Parse a comma- or space-separated list of 2D transform functions
 *
 * @details 从单位矩阵开始，逐个识别 `name(...)` 并累积相乘；值为 `none` 或为空时输出单位矩阵，函数名会被转为小写，未知函数名由 `transform_from_fn` 视为不变。
 *          Starting from the identity it recognises each `name(...)` in turn and multiplies it in; `none` or an empty value yields the identity, function names are lowercased, and unknown names are treated as no-ops by `transform_from_fn`.
 *
 * @param[in]     value 属性值文本 / Property value text
 * @param[out]    out 接收解析出的矩阵 / Receives the parsed matrix
 *
 * @see apply_transform_fn apply_transform
 */
static void parse_transform(const char *value, HpeTransform *out)
{
    HpeTransform acc;
    const char *p = value;

    acc.a = 1.0f;
    acc.b = 0.0f;
    acc.c = 0.0f;
    acc.d = 1.0f;
    acc.e = 0.0f;
    acc.f = 0.0f;
    acc.identity = 1;
    if (!value || !strcmp(value, "none")) {
        *out = acc;
        return;
    }
    while (*p) {
        const char *lp;
        const char *rp;
        char fn[32];
        size_t flen;
        size_t i;

        while (*p && (hpe_is_space(*p) || *p == ','))
            p++;
        if (!*p)
            break;
        lp = strchr(p, '(');
        if (!lp)
            break;
        rp = strchr(lp, ')');
        if (!rp)
            break;
        flen = (size_t)(lp - p);
        if (flen >= sizeof(fn))
            flen = sizeof(fn) - 1;
        memcpy(fn, p, flen);
        fn[flen] = '\0';
        for (i = 0; fn[i]; i++)
            fn[i] = (char)hpe_ascii_lower((unsigned char)fn[i]);
        {
            char *body = hpe_strndup(lp + 1, (size_t)(rp - lp - 1));
            apply_transform_fn(fn, body, &acc);
            hpe_free(body);
        }
        p = rp + 1;
    }
    *out = acc;
}

/* ---------------- property appliers ----------------
 * Each applier writes one property (or one shorthand family) into a style.
 * They are registered by name in k_prop_table and dispatched by apply_property,
 * so adding a property is a data change, and every applier stays small enough
 * to be read and reasoned about on its own. */

/**
 * @brief 返回以空白分隔后最后一个能解析为颜色的 token / Return the last whitespace-separated token that parses as a colour
 *
 * @details 从末段向前扫描，遇到第一个颜色即返回，用于处理颜色位于尾部的简写（如 `1px solid #ccc`）。
 *          It scans from the last segment backwards and returns on the first colour, handling shorthands whose colour trails such as `1px solid #ccc`.
 *
 * @param[in]     value 属性值文本 / Property value text
 * @param[out]    out 接收解析出的颜色 / Receives the parsed colour
 *
 * @return 找到颜色返回 1，否则返回 0 / 1 when a colour was found, 0 otherwise
 *
 * @see apply_background
 */
static int last_color_token(const char *value, HpeColor *out)
{
    char buf[256];
    char *parts[8];
    int n, i;

    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 8);
    for (i = n - 1; i >= 0; i--) {
        if (hpe_color_parse(parts[i], out))
            return 1;
    }
    return 0;
}

/**
 * @brief 把 1..4 个边框颜色按 CSS 规则展开到四边 / Expand 1..4 border colours across the four sides per the CSS rule
 *
 * @details 按 (上,右,下,左) 顺序重复取值：1 个值四边相同，2 个值上下取首值、左右取次值，3 个值下边取次值，4 个值一一对应；同时置位各边颜色位并把简写颜色同步为第一个值。
 *          Values repeat in (top,right,bottom,left) order: one value fills all sides, two values give the first to top/bottom and the second to right/left, three values give the second to the bottom, and four map one-to-one; each side colour bit is set and the shorthand colour is synced to the first value.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     vals 已解析出的颜色数组 / Array of parsed colours
 * @param[in]     got 有效颜色个数 1..4 / Number of valid colours, 1..4
 *
 * @see apply_border_color
 */
static void expand_border_colors(HpeStyle *d, const HpeColor *vals, int got)
{
    static const int pick4[4] = {0, 1, 2, 3};
    static const int pick3[4] = {0, 1, 2, 1};
    static const int pick2[4] = {0, 1, 0, 1};
    static const int pick1[4] = {0, 0, 0, 0};
    const int *pick = got == 1 ? pick1 : (got == 2 ? pick2 : (got == 3 ? pick3 : pick4));
    int i;

    for (i = 0; i < 4; i++) {
        d->border_colors[i] = vals[pick[i]];
        d->set2 |= HPE_PROP2_BORDER_TOP_COLOR << i;
    }
    d->border_color = vals[0]; /* keep the shorthand default in sync */
    d->set |= HPE_PROP_BORDER_COLOR;
}

/**
 * @brief 设置某一侧或全部四侧的边框宽度 / Set the border width of one side or of all four sides
 *
 * @details `side` 非负时只写该侧并置位其宽度位；`side` 为负时把四侧统一写入并置位简写宽度位。
 *          When `side` is non-negative only that side is written and its width bit set; when `side` is negative all four sides are written uniformly and the shorthand width bit is set.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     w 边框宽度（像素）/ Border width in pixels
 * @param[in]     side 边序号 0..3，小于 0 表示全部四边 / Side index 0..3, negative for all four sides
 * @param[in]     side_bit 该边的宽度属性位 / Width property bit for that side
 *
 * @see apply_border_side
 */
static void set_border_width(HpeStyle *d, float w, int side, uint64_t side_bit)
{
    if (side >= 0) {
        d->border_w[side] = w;
        d->set |= side_bit;
    } else {
        int k;
        for (k = 0; k < 4; k++)
            d->border_w[k] = w;
        d->set |= HPE_PROP_BORDER_WIDTH;
    }
}

/**
 * @brief 按名称关键字把字体族归入 mono/serif/sans / Classify a font family name into mono, serif or sans by keyword
 *
 * @details 名称同时匹配中英文关键字，例如 `mono`/`courier`/`consolas` 归等宽，`serif`/`times`/`宋` 归衬线，`sans`/`yahei`/`arial`/`黑` 归无衬线；无法识别时返回默认族。
 *          The name is matched against both Latin and Chinese keywords: `mono`/`courier`/`consolas` map to monospace, `serif`/`times`/`宋` to serif, `sans`/`yahei`/`arial`/`黑` to sans-serif; an unrecognised name falls back to the default family.
 *
 * @param[in]     name 已小写的字体族名 / Lowercased font family name
 *
 * @return 对应的 `HpeFontFamily` 枚举 / The matching `HpeFontFamily` value
 *
 * @see normalize_family_name apply_font_family
 */
static HpeFontFamily classify_font_family(const char *name)
{
    if (strstr(name, "mono") || !strcmp(name, "courier") || strstr(name, "consolas"))
        return HPE_FONT_MONO;
    if (strstr(name, "serif") || strstr(name, "times") || strstr(name, "georgia") ||
        strstr(name, "simsun") || strstr(name, "song") || strstr(name, "宋"))
        return HPE_FONT_SERIF;
    if (strstr(name, "sans") || strstr(name, "yahei") || strstr(name, "arial") ||
        strstr(name, "helvetica") || strstr(name, "segoe") || strstr(name, "黑") ||
        strstr(name, "微软雅黑") || strstr(name, "hei"))
        return HPE_FONT_SANS;
    return HPE_FONT_DEFAULT;
}

/**
 * @brief 就地去除字体名两侧引号并转为小写 / Strip surrounding quotes from a family name in place and lowercase it
 *
 * @details 名称首尾同为 `"` 时去掉这对引号，然后把整串按 ASCII 转为小写，便于后续关键字匹配。
 *          When the name starts and ends with `"` that pair is removed, then the whole string is lowercased by ASCII so keyword matching works.
 *
 * @param[in,out] name 待规范化的字体名（会被就地改写）/ Family name to normalise, modified in place
 *
 * @see classify_font_family apply_font_family
 */
static void normalize_family_name(char *name)
{
    size_t len = strlen(name);
    size_t j;

    if (len >= 2 && name[0] == '"' && name[len - 1] == '"') {
        memmove(name, name + 1, len - 2);
        name[len - 2] = '\0';
        len -= 2;
    }
    for (j = 0; name[j]; j++)
        name[j] = (char)hpe_ascii_lower((unsigned char)name[j]);
}

/**
 * @brief 把 overflow 关键字映射为枚举值 / Map an overflow keyword to its enum value
 *
 * @details `hidden`/`scroll`/`auto` 分别映射到对应枚举，其余（含 `visible`）一律返回 `HPE_OVERFLOW_VISIBLE`。
 *          `hidden`, `scroll` and `auto` map to their enums and anything else, including `visible`, returns `HPE_OVERFLOW_VISIBLE`.
 *
 * @param[in]     tok 关键字 token / Keyword token
 *
 * @return 对应的 `HpeOverflow` 枚举 / The matching `HpeOverflow` value
 *
 * @see apply_overflow apply_overflow_x apply_overflow_y
 */
static HpeOverflow overflow_keyword(const char *tok)
{
    if (!strcmp(tok, "hidden"))
        return HPE_OVERFLOW_HIDDEN;
    if (!strcmp(tok, "scroll"))
        return HPE_OVERFLOW_SCROLL;
    if (!strcmp(tok, "auto"))
        return HPE_OVERFLOW_AUTO;
    return HPE_OVERFLOW_VISIBLE;
}

/**
 * @brief 把 `display` 声明写入样式 / Write a `display` declaration into the style
 *
 * @details 识别 `none`/`inline-block`/`flex`/`inline`，其余（含缺省）按 `block` 处理；`inline-flex` 因引擎没有行内级弹性容器而被提升为 `flex`。
 *          It recognises `none`, `inline-block`, `flex` and `inline` and treats everything else, including the default, as `block`; `inline-flex` is blockified to `flex` because the engine has no inline-level flex container.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @note 同时置位 `HPE_PROP_DISPLAY` / It also sets `HPE_PROP_DISPLAY`
 *
 * @see apply_property
 */

static void apply_display(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "none"))
        d->display = HPE_DISPLAY_NONE;
    else if (!strcmp(value, "inline-block"))
        d->display = HPE_DISPLAY_INLINE_BLOCK;
    else if (!strcmp(value, "flex") || !strcmp(value, "inline-flex"))
        /* inline-flex is blockified: the engine has no inline-level flex container */
        d->display = HPE_DISPLAY_FLEX;
    else if (!strcmp(value, "grid") || !strcmp(value, "inline-grid"))
        d->display = HPE_DISPLAY_GRID;
    else if (!strcmp(value, "table") || !strcmp(value, "inline-table"))
        d->display = HPE_DISPLAY_TABLE;
    else if (!strcmp(value, "table-row"))
        d->display = HPE_DISPLAY_TABLE_ROW;
    else if (!strcmp(value, "table-cell"))
        d->display = HPE_DISPLAY_TABLE_CELL;
    else if (!strcmp(value, "table-row-group") || !strcmp(value, "table-header-group") ||
             !strcmp(value, "table-footer-group"))
        d->display = HPE_DISPLAY_TABLE_ROW_GROUP;
    else if (!strcmp(value, "table-caption"))
        d->display = HPE_DISPLAY_TABLE_CAPTION;
    else if (!strcmp(value, "inline"))
        d->display = HPE_DISPLAY_INLINE;
    else
        d->display = HPE_DISPLAY_BLOCK;
    d->set |= HPE_PROP_DISPLAY;
}

/**
 * @brief 把 `color` 声明写入样式的文本颜色 / Write a `color` declaration into the style's text colour
 *
 * @details 用 `hpe_color_parse` 解析成功才写入；解析失败时保持原值不变。
 *          The value is written only when `hpe_color_parse` succeeds; on failure the previous value is kept.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see hpe_color_parse apply_property
 */
static void apply_color(HpeStyle *d, const char *value)
{
    HpeColor c;
    if (hpe_color_parse(value, &c)) {
        d->color = c;
        d->set |= HPE_PROP_COLOR;
    }
}

/* the `background` shorthand and `object-position` reuse parsers that are
 * defined further down with the other background longhands */
static int parse_bg_url(const char *value, HpeBackground *bg);
static void apply_background_position(HpeStyle *d, const char *value);

/**
 * @brief 解析 `background`/`background-color` 简写 / Parse the `background`/`background-color` shorthand
 *
 * @details 先尝试 `linear-gradient(...)` 并写入背景图层；`none`/`transparent` 置为透明色；否则先整体解析颜色（`rgba(...)` 等带空格的函数式写法不拆分），失败再取最后一个颜色 token（处理 `1px solid #ccc` 之类）。
 *          It first tries `linear-gradient(...)` and writes the image layer; `none`/`transparent` become transparent; otherwise the whole value is parsed as a colour first (so `rgba(...)` with spaces is not split) and on failure the last colour token is used, handling cases like `1px solid #ccc`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see parse_linear_gradient last_color_token
 */
static void apply_background(HpeStyle *d, const char *value)
{
    HpeColor c;

    /* `background: linear-gradient(...)` also sets the image layer */
    if (parse_linear_gradient(value, &d->background_image)) {
        d->set2 |= HPE_PROP2_BACKGROUND_IMAGE;
        return;
    }
    /* `background: url(...) ...` sets the external image layer; the colour, if
     * any, is still picked up below by last_color_token */
    if (parse_bg_url(value, &d->background_image))
        d->set2 |= HPE_PROP2_BACKGROUND_IMAGE;
    if (!strcmp(value, "none") || !strcmp(value, "transparent")) {
        d->background = hpe_rgba(0, 0, 0, 0);
        d->set |= HPE_PROP_BACKGROUND;
        return;
    }
    /* try the whole value first: functional notations such as
       `rgba(217, 48, 37, 0.45)` contain spaces and must not be split */
    if (hpe_color_parse(value, &c) || last_color_token(value, &c)) {
        d->background = c;
        d->set |= HPE_PROP_BACKGROUND;
    }
}

/**
 * @brief 把 `min-width` 声明写入样式 / Write a `min-width` declaration into the style
 *
 * @details 仅接受 `px` 或百分比长度，其它单位（如 `auto`）不改动样式。
 *          Only `px` and percentage lengths are accepted; other units such as `auto` leave the style unchanged.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_width hpe_len_parse
 */
static void apply_min_width(HpeStyle *d, const char *value)
{
    HpeLength len = hpe_len_parse(value);
    if (len.unit == HPE_LEN_PX || len.unit == HPE_LEN_PCT) {
        d->min_width = len;
        d->set |= HPE_PROP_MIN_WIDTH;
    }
}

/**
 * @brief 把 `width` 声明写入样式 / Write a `width` declaration into the style
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @note 无条件写入并置位 `HPE_PROP_WIDTH`，`auto` 由 `hpe_len_parse` 表示 / It writes unconditionally and sets `HPE_PROP_WIDTH`; `auto` is represented by `hpe_len_parse`
 *
 * @see apply_height hpe_len_parse
 */
static void apply_width(HpeStyle *d, const char *value)
{
    d->width = hpe_len_parse(value);
    d->set |= HPE_PROP_WIDTH;
}

/**
 * @brief 把 `height` 声明写入样式 / Write a `height` declaration into the style
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @note 无条件写入并置位 `HPE_PROP_HEIGHT` / It writes unconditionally and sets `HPE_PROP_HEIGHT`
 *
 * @see apply_width hpe_len_parse
 */
static void apply_height(HpeStyle *d, const char *value)
{
    d->height = hpe_len_parse(value);
    d->set |= HPE_PROP_HEIGHT;
}

/**
 * @brief 解析 `margin` 简写并展开到四条边 / Parse the `margin` shorthand and expand it across the four sides
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see set_sides apply_padding
 */
static void apply_margin(HpeStyle *d, const char *value)
{
    set_sides(d, value, 1);
}

/**
 * @brief 解析 `padding` 简写并展开到四条边 / Parse the `padding` shorthand and expand it across the four sides
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see set_sides apply_margin
 */
static void apply_padding(HpeStyle *d, const char *value)
{
    set_sides(d, value, 0);
}

/**
 * @brief 写入 margin/padding 的单边长属性 / Write one side of the margin/padding shorthand as a longhand
 *
 * @details `idx` 取 0..3 对应上右下左，`is_margin` 决定写入外边距还是内边距，并据类型置位对应的单边属性位。
 *          `idx` 0..3 selects top, right, bottom and left and `is_margin` chooses the outer or inner edge, setting the matching per-side property bit.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 * @param[in]     idx 边序号 0..3（上右下左）/ Side index 0..3 (top, right, bottom, left)
 * @param[in]     is_margin 为 1 时写外边距，为 0 时写内边距 / 1 writes a margin, 0 writes padding
 *
 * @see apply_margin apply_padding
 */
static void apply_box_side(HpeStyle *d, const char *value, int idx, int is_margin)
{
    HpeLength len = hpe_len_parse(value);
    if (is_margin) {
        d->margin[idx] = len;
        d->set |= HPE_PROP_MARGIN_TOP << idx;
    } else {
        d->padding[idx] = len;
        d->set |= HPE_PROP_PADDING_TOP << idx;
    }
}

static void apply_margin_top(HpeStyle *d, const char *v) { apply_box_side(d, v, 0, 1); }
static void apply_margin_right(HpeStyle *d, const char *v) { apply_box_side(d, v, 1, 1); }
static void apply_margin_bottom(HpeStyle *d, const char *v) { apply_box_side(d, v, 2, 1); }
static void apply_margin_left(HpeStyle *d, const char *v) { apply_box_side(d, v, 3, 1); }
static void apply_padding_top(HpeStyle *d, const char *v) { apply_box_side(d, v, 0, 0); }
static void apply_padding_right(HpeStyle *d, const char *v) { apply_box_side(d, v, 1, 0); }
static void apply_padding_bottom(HpeStyle *d, const char *v) { apply_box_side(d, v, 2, 0); }
static void apply_padding_left(HpeStyle *d, const char *v) { apply_box_side(d, v, 3, 0); }

/**
 * @brief 解析 `border-color` 简写 / Parse the `border-color` shorthand
 *
 * @details 按空白最多拆出四段并逐个尝试解析颜色，至少解析出一个颜色才展开到四边。
 *          At most four whitespace-separated segments are tried as colours and expansion happens only when at least one colour parses.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see expand_border_colors
 */
static void apply_border_color(HpeStyle *d, const char *value)
{
    char buf[256];
    char *parts[4];
    HpeColor vals[4];
    int n, i, got = 0;

    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 4);
    for (i = 0; i < n; i++) {
        if (hpe_color_parse(parts[i], &vals[got]))
            got++;
    }
    if (got > 0)
        expand_border_colors(d, vals, got);
}

/**
 * @brief 解析 `border` 与 `border-<side>` 简写 / Parse the `border` and `border-<side>` shorthands
 *
 * @details 逐 token 处理宽度、样式与颜色（顺序无关）：`none` 把该侧宽度置 0，`solid`/`dashed`/`dotted`/`double` 被跳过，颜色分别写入该侧或四边，`px` 长度作为宽度；`side` 小于 0 表示四边。
 *          Each token is handled for width, style or colour in any order: `none` zeroes that side's width, `solid`/`dashed`/`dotted`/`double` are skipped, a colour goes to that side or to all four, and a `px` length becomes the width; `side` below 0 means all four sides.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 * @param[in]     side 边序号 0..3，小于 0 表示全部四边 / Side index 0..3, negative for all four sides
 * @param[in]     side_bit 该边的宽度属性位 / Width property bit for that side
 *
 * @see set_border_width apply_border_width
 */
static void apply_border_side(HpeStyle *d, const char *value, int side, uint64_t side_bit)
{
    char buf[256];
    char *parts[8];
    int n, i;

    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 8);
    for (i = 0; i < n; i++) {
        HpeColor c;
        HpeLength len;

        if (!strcmp(parts[i], "none")) {
            /* `border: none` / `border-top: none` -> zero width on that side */
            set_border_width(d, 0.0f, side, side_bit);
            continue;
        }
        if (!strcmp(parts[i], "solid") || !strcmp(parts[i], "dashed") ||
            !strcmp(parts[i], "dotted") || !strcmp(parts[i], "double"))
            continue;
        if (hpe_color_parse(parts[i], &c)) {
            if (side >= 0) {
                /* a side shorthand sets only that side */
                d->border_colors[side] = c;
                d->set2 |= HPE_PROP2_BORDER_TOP_COLOR << side;
            } else {
                int k;
                d->border_color = c;
                for (k = 0; k < 4; k++)
                    d->border_colors[k] = c;
                d->set |= HPE_PROP_BORDER_COLOR;
            }
            continue;
        }
        len = hpe_len_parse(parts[i]);
        if (len.unit == HPE_LEN_PX)
            set_border_width(d, len.value, side, side_bit);
    }
}

static void apply_border(HpeStyle *d, const char *v) { apply_border_side(d, v, -1, 0); }
static void apply_border_width(HpeStyle *d, const char *v) { apply_border_side(d, v, -1, 0); }
static void apply_border_top(HpeStyle *d, const char *v) { apply_border_side(d, v, 0, HPE_PROP_BORDER_TOP); }
static void apply_border_right(HpeStyle *d, const char *v) { apply_border_side(d, v, 1, HPE_PROP_BORDER_RIGHT); }
static void apply_border_bottom(HpeStyle *d, const char *v) { apply_border_side(d, v, 2, HPE_PROP_BORDER_BOTTOM); }
static void apply_border_left(HpeStyle *d, const char *v) { apply_border_side(d, v, 3, HPE_PROP_BORDER_LEFT); }

/**
 * @brief 把 `border-radius` 声明写入样式 / Write a `border-radius` declaration into the style
 *
 * @details `px` 长度原样写入；百分比因需依赖边框盒而被存为负值，留待绘制阶段求解。
 *          A `px` length is stored as-is; a percentage is stored negated because it depends on the border box, and is solved at paint time.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see hpe_len_parse
 */
static void apply_border_radius(HpeStyle *d, const char *value)
{
    HpeLength len = hpe_len_parse(value);

    if (len.unit == HPE_LEN_PX) {
        d->border_radius = len.value;
        d->set |= HPE_PROP_BORDER_RADIUS;
    } else if (len.unit == HPE_LEN_PCT) {
        /* percentages need the border box: stored negated, solved at paint time */
        d->border_radius = -len.value;
        d->set |= HPE_PROP_BORDER_RADIUS;
    }
}

/**
 * @brief 解析 `inset` 简写并展开到四边 / Parse the `inset` shorthand and expand it across the four sides
 *
 * @details 按 1/2/3/4 值的 CSS 规则展开到 (上,右,下,左)，逐边写入并置位对应的 inset 属性位。
 *          It expands 1/2/3/4 values across (top,right,bottom,left) per the CSS rule, writing each edge and setting the inset property bit.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_inset_side hpe_len_parse
 */
static void apply_inset(HpeStyle *d, const char *value)
{
    char buf[256];
    char *parts[4];
    HpeLength vals[4];
    int n, i;

    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 4);
    if (n <= 0)
        return;
    for (i = 0; i < n && i < 4; i++)
        vals[i] = hpe_len_parse(parts[i]);
    if (n < 4) {
        HpeLength t = vals[0];
        HpeLength r = n > 1 ? vals[1] : t;
        HpeLength b = n > 2 ? vals[2] : t;
        HpeLength l = n > 1 ? vals[1] : t;
        vals[0] = t;
        vals[1] = r;
        vals[2] = b;
        vals[3] = l;
    }
    for (i = 0; i < 4; i++) {
        d->inset[i] = vals[i];
        d->set |= HPE_PROP_INSET_TOP << i;
    }
}

/**
 * @brief 把 `flex-direction` 声明写入样式 / Write a `flex-direction` declaration into the style
 *
 * @details `column`/`column-reverse` 映射为 `HPE_FLEX_COLUMN`，`row`/`row-reverse` 为 `HPE_FLEX_ROW`；带 `-reverse` 时置 `row_reverse`。
 *          `column`/`column-reverse` map to `HPE_FLEX_COLUMN` and `row`/`row-reverse` to `HPE_FLEX_ROW`; the `-reverse` suffix sets `row_reverse`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_property
 */
static void apply_flex_direction(HpeStyle *d, const char *value)
{
    char buf[32];
    size_t len = strlen(value);
    int reverse = 0;
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, value, len);
    buf[len] = '\0';
    hpe_trim(buf);
    if (len > 8 && !strcmp(buf + len - 8, "-reverse")) {
        reverse = 1;
        buf[len - 8] = '\0';
        hpe_trim(buf);
    }
    d->flex_dir = !strcmp(buf, "column") ? HPE_FLEX_COLUMN : HPE_FLEX_ROW;
    d->row_reverse = reverse;
    d->set |= HPE_PROP_FLEX_DIR;
    d->set2 |= HPE_PROP2_ROW_REVERSE;
}

/**
 * @brief 把 `align-items` 声明写入样式 / Write an `align-items` declaration into the style
 *
 * @details 识别 `center`、`flex-start`/`start`、`flex-end`/`end`、`baseline`，其余一律为 `stretch`。
 *          It recognises `center`, `flex-start`/`start`, `flex-end`/`end` and `baseline`, and treats everything else as `stretch`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_align_self
 */
static void apply_align_items(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "center"))
        d->align_items = HPE_ALIGN_ITEMS_CENTER;
    else if (!strcmp(value, "flex-start") || !strcmp(value, "start"))
        d->align_items = HPE_ALIGN_ITEMS_FLEX_START;
    else if (!strcmp(value, "flex-end") || !strcmp(value, "end"))
        d->align_items = HPE_ALIGN_ITEMS_FLEX_END;
    else if (!strcmp(value, "baseline"))
        d->align_items = HPE_ALIGN_ITEMS_BASELINE;
    else
        d->align_items = HPE_ALIGN_ITEMS_STRETCH;
    d->set |= HPE_PROP_ALIGN_ITEMS;
}

/**
 * @brief 把 `align-self` 声明写入样式 / Write an `align-self` declaration into the style
 *
 * @details 识别 `center`、`flex-start`/`start`、`flex-end`/`end`、`stretch`，其余（含 `auto`）为 `HPE_ALIGN_ITEMS_AUTO`。
 *          It recognises `center`, `flex-start`/`start`, `flex-end`/`end` and `stretch`, and treats everything else, including `auto`, as `HPE_ALIGN_ITEMS_AUTO`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_align_items
 */
static void apply_align_self(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "center"))
        d->align_self = HPE_ALIGN_ITEMS_CENTER;
    else if (!strcmp(value, "flex-start") || !strcmp(value, "start"))
        d->align_self = HPE_ALIGN_ITEMS_FLEX_START;
    else if (!strcmp(value, "flex-end") || !strcmp(value, "end"))
        d->align_self = HPE_ALIGN_ITEMS_FLEX_END;
    else if (!strcmp(value, "stretch"))
        d->align_self = HPE_ALIGN_ITEMS_STRETCH;
    else
        d->align_self = HPE_ALIGN_ITEMS_AUTO;
    d->set |= HPE_PROP_ALIGN_SELF;
}

/**
 * @brief 把 `justify-content` 声明写入样式 / Write a `justify-content` declaration into the style
 *
 * @details 识别 `center`、`flex-end`/`end`、`space-between`、`space-around`，其余（含 `flex-start`/`start`）为起始对齐。
 *          It recognises `center`, `flex-end`/`end`, `space-between` and `space-around`; everything else, including `flex-start`/`start`, is start-aligned.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_property
 */
static void apply_justify(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "center"))
        d->justify = HPE_JUSTIFY_CENTER;
    else if (!strcmp(value, "flex-end") || !strcmp(value, "end"))
        d->justify = HPE_JUSTIFY_END;
    else if (!strcmp(value, "space-between"))
        d->justify = HPE_JUSTIFY_SPACE_BETWEEN;
    else if (!strcmp(value, "space-around"))
        d->justify = HPE_JUSTIFY_SPACE_AROUND;
    else
        d->justify = HPE_JUSTIFY_START;
    d->set |= HPE_PROP_JUSTIFY;
}

/**
 * @brief 把 `gap`/`row-gap`/`column-gap` 声明写入样式 / Write a `gap`/`row-gap`/`column-gap` declaration into the style
 *
 * @details 仅 `px` 与百分比长度取值为间距，其它单位落为 0。
 *          Only `px` and percentage lengths yield a spacing; other units become 0.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_flex_grow
 */
static void apply_gap(HpeStyle *d, const char *value)
{
    HpeLength len = hpe_len_parse(value);
    d->gap = (len.unit == HPE_LEN_PX || len.unit == HPE_LEN_PCT) ? len.value : 0.0f;
    d->set |= HPE_PROP_GAP;
}

/**
 * @brief 把 `flex`/`flex-grow` 声明写入样式 / Write a `flex`/`flex-grow` declaration into the style
 *
 * @details 仅 `px` 与百分比长度取值为伸缩系数（复用长度解析读取数值），其它单位落为 0。
 *          Only `px` and percentage lengths yield a grow factor (the length parser supplies the number); other units become 0.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_gap
 */
static void apply_flex_grow(HpeStyle *d, const char *value)
{
    /* `flex: <grow> <shrink>? <basis>?` — also used for the lone flex-grow */
    char buf[96];
    char *sp, *tok;
    int n = 0;
    snprintf(buf, sizeof(buf), "%s", value);
    if (hpe_str_ieq(buf, "none")) {
        d->flex_grow = 0.0f;
        d->flex_shrink = 0.0f;
        d->flex_basis = hpe_len_auto();
        d->set |= HPE_PROP_FLEX_GROW;
        d->set2 |= HPE_PROP2_FLEX_SHRINK | HPE_PROP2_FLEX_BASIS;
        return;
    }
    if (hpe_str_ieq(buf, "auto")) {
        d->flex_grow = 1.0f;
        d->flex_shrink = 1.0f;
        d->flex_basis = hpe_len_auto();
        d->set |= HPE_PROP_FLEX_GROW;
        d->set2 |= HPE_PROP2_FLEX_SHRINK | HPE_PROP2_FLEX_BASIS;
        return;
    }
    sp = buf;
    while ((tok = strtok(sp, " \t")) != NULL && n < 3) {
        HpeLength len;
        sp = NULL;
        len = hpe_len_parse(tok);
        if (n == 0) {
            d->flex_grow = (len.unit == HPE_LEN_PX || len.unit == HPE_LEN_PCT) ? len.value : 0.0f;
            d->set |= HPE_PROP_FLEX_GROW;
        } else if (n == 1 && (len.unit == HPE_LEN_PX || len.unit == HPE_LEN_PCT)) {
            d->flex_shrink = len.value;
            d->set2 |= HPE_PROP2_FLEX_SHRINK;
        } else {
            d->flex_basis = hpe_len_parse(tok);
            d->set2 |= HPE_PROP2_FLEX_BASIS;
        }
        n++;
    }
}


/**
 * @brief 把 `font-size` 声明写入样式 / Write a `font-size` declaration into the style
 *
 * @details 支持 `small`/`medium`/`large`/`x-large`/`xx-large`/`smaller`/`larger` 关键字；`px` 直接取值，百分比与 `em` 以父级 16px 为基准换算，`rem` 以 16px 根字号换算；结果非正时不写入。
 *          It supports the `small`/`medium`/`large`/`x-large`/`xx-large`/`smaller`/`larger` keywords; `px` is taken directly, percentage and `em` resolve against a 16px parent and `rem` against a 16px root size; a non-positive result is not written.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see hpe_len_parse
 */
static void apply_font_size(HpeStyle *d, const char *value)
{
    float v = 0.0f;

    if (!strcmp(value, "small"))
        v = 13.0f;
    else if (!strcmp(value, "medium"))
        v = 16.0f;
    else if (!strcmp(value, "large"))
        v = 18.0f;
    else if (!strcmp(value, "x-large"))
        v = 24.0f;
    else if (!strcmp(value, "xx-large"))
        v = 32.0f;
    else if (!strcmp(value, "smaller"))
        v = 13.0f;
    else if (!strcmp(value, "larger"))
        v = 19.0f;
    else {
        HpeLength len = hpe_len_parse(value);
        if (len.unit == HPE_LEN_PCT || len.unit == HPE_LEN_EM)
            v = 16.0f * len.value / 100.0f; /* resolved against the parent (16px default) */
        else if (len.unit == HPE_LEN_PX)
            v = len.value;
        else if (len.unit == HPE_LEN_REM)
            v = 16.0f * len.value; /* the root font size is 16px by default */
    }
    if (v > 0.0f) {
        d->font_size = v;
        d->set |= HPE_PROP_FONT_SIZE;
    }
}

/**
 * @brief 把 `font-weight` 声明归一为 400 或 700 / Normalise a `font-weight` declaration to 400 or 700
 *
 * @details `bold` 取 700、`normal` 取 400，其它情况解析为数值后按是否大于等于 600 归为 700 或 400（只有常规与粗体两档）。
 *          `bold` is 700 and `normal` is 400; otherwise the value is parsed as a number and mapped to 700 when at least 600 and 400 otherwise (only a regular and a bold weight exist).
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see parse_plain_number
 */
static void apply_font_weight(HpeStyle *d, const char *value)
{
    float v;
    int w = 400;

    if (!strcmp(value, "bold"))
        w = 700;
    else if (!strcmp(value, "normal"))
        w = 400;
    else if (parse_plain_number(value, &v))
        w = (int)v;
    d->font_weight = w >= 600 ? 700 : 400;
    d->set |= HPE_PROP_FONT_WEIGHT;
}

/**
 * @brief 把 `text-align` 声明写入样式 / Write a `text-align` declaration into the style
 *
 * @details 识别 `center` 与 `right`，其余（含 `left`/`justify`）一律左对齐。
 *          It recognises `center` and `right` and treats everything else, including `left`/`justify`, as left-aligned.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_property
 */
static void apply_text_align(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "center"))
        d->text_align = HPE_ALIGN_CENTER;
    else if (!strcmp(value, "right"))
        d->text_align = HPE_ALIGN_RIGHT;
    else
        d->text_align = HPE_ALIGN_LEFT;
    d->set |= HPE_PROP_TEXT_ALIGN;
}

/**
 * @brief 把 `line-height` 声明写入样式 / Write a `line-height` declaration into the style
 *
 * @details `normal` 置 0；无单位数字存为负值（表示字号倍数）；`px` 长度原样写入；其它单位不生效。
 *          `normal` stores 0; a unitless number is stored negated to mean a multiple of the font size; a `px` length is stored as-is; other units have no effect.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @note `line_height <= 0` 表示 `auto`，负值 -n 表示 n 倍字号 / `line_height <= 0` means `auto` and a negative -n means an n-multiple of the font size
 *
 * @see parse_plain_number hpe_len_parse
 */
static void apply_line_height(HpeStyle *d, const char *value)
{
    float v;
    HpeLength len;

    if (!strcmp(value, "normal")) {
        d->line_height = 0.0f;
        d->set |= HPE_PROP_LINE_HEIGHT;
        return;
    }
    if (parse_plain_number(value, &v)) {
        d->line_height = -v; /* unitless: multiplier of the font size */
        d->set |= HPE_PROP_LINE_HEIGHT;
        return;
    }
    len = hpe_len_parse(value);
    if (len.unit == HPE_LEN_PX) {
        d->line_height = len.value;
        d->set |= HPE_PROP_LINE_HEIGHT;
    }
}

/**
 * @brief 把 `white-space` 声明写入样式 / Write a `white-space` declaration into the style
 *
 * @details 识别 `nowrap`、`pre`、`pre-wrap`、`pre-line`，其余（含 `normal`）为 `HPE_WS_NORMAL`。
 *          It recognises `nowrap`, `pre`, `pre-wrap` and `pre-line`; everything else, including `normal`, is `HPE_WS_NORMAL`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_property
 */
static void apply_white_space(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "nowrap"))
        d->white_space = HPE_WS_NOWRAP;
    else if (!strcmp(value, "pre"))
        d->white_space = HPE_WS_PRE;
    else if (!strcmp(value, "pre-wrap"))
        d->white_space = HPE_WS_PRE_WRAP;
    else if (!strcmp(value, "pre-line"))
        d->white_space = HPE_WS_PRE_LINE;
    else
        d->white_space = HPE_WS_NORMAL;
    d->set |= HPE_PROP_WHITE_SPACE;
}

/**
 * @brief 从字体族列表中取首个可识别的族 / Take the first recognisable family from a font family list
 *
 * @details 按逗号分段，逐段去引号、转小写并按关键字归类，取第一个非默认族即停止；整串都识别不出时落到 `HPE_FONT_SANS`。
 *          Segments are split on commas, each is de-quoted, lowercased and classified by keyword, and the first non-default family wins; when none is recognisable it falls back to `HPE_FONT_SANS`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see classify_font_family normalize_family_name
 */
static void apply_font_family(HpeStyle *d, const char *value)
{
    char buf[256];
    HpeFontFamily fam = HPE_FONT_DEFAULT;
    char *p;

    /* take the first recognized family in the comma list */
    snprintf(buf, sizeof(buf), "%s", value);
    p = buf;
    while (*p) {
        char name[128];
        char *q = strchr(p, ',');
        char *t;
        size_t tl;
        HpeFontFamily got;

        if (q)
            *q = '\0';
        t = hpe_trim(p);
        tl = strlen(t);
        if (tl >= sizeof(name))
            tl = sizeof(name) - 1;
        memcpy(name, t, tl);
        name[tl] = '\0';
        normalize_family_name(name);
        got = classify_font_family(name);
        if (got != HPE_FONT_DEFAULT)
            fam = got;
        if (q) {
            p = q + 1;
            if (fam != HPE_FONT_DEFAULT)
                break;
        } else {
            if (fam == HPE_FONT_DEFAULT)
                fam = HPE_FONT_SANS;
            break;
        }
    }
    d->family = fam;
    d->set |= HPE_PROP_FONT_FAMILY;
}

/**
 * @brief 把 `position` 声明写入样式 / Write a `position` declaration into the style
 *
 * @details 识别 `relative`、`absolute`、`fixed`，其余（含 `static`）为 `HPE_POS_STATIC`。
 *          It recognises `relative`, `absolute` and `fixed`; everything else, including `static`, is `HPE_POS_STATIC`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_inset_side
 */
static void apply_position(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "relative"))
        d->position = HPE_POS_RELATIVE;
    else if (!strcmp(value, "absolute"))
        d->position = HPE_POS_ABSOLUTE;
    else if (!strcmp(value, "fixed"))
        d->position = HPE_POS_FIXED;
    else
        d->position = HPE_POS_STATIC;
    d->set |= HPE_PROP_POSITION;
}

/**
 * @brief 写入 top/right/bottom/left 中的一条 inset 长属性 / Write one inset longhand out of top, right, bottom and left
 *
 * @details 解析长度后写入 `inset[idx]` 并置位 `bit`。
 *          The length is parsed into `inset[idx]` and `bit` is set.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 * @param[in]     idx 边序号 0..3（上右下左）/ Side index 0..3 (top, right, bottom, left)
 * @param[in]     bit 该边的 inset 属性位 / Inset property bit for that side
 *
 * @see apply_inset
 */
static void apply_inset_side(HpeStyle *d, const char *value, int idx, uint32_t bit)
{
    d->inset[idx] = hpe_len_parse(value);
    d->set |= bit;
}

static void apply_top(HpeStyle *d, const char *v) { apply_inset_side(d, v, 0, HPE_PROP_INSET_TOP); }
static void apply_right(HpeStyle *d, const char *v) { apply_inset_side(d, v, 1, HPE_PROP_INSET_RIGHT); }
static void apply_bottom(HpeStyle *d, const char *v) { apply_inset_side(d, v, 2, HPE_PROP_INSET_BOTTOM); }
static void apply_left(HpeStyle *d, const char *v) { apply_inset_side(d, v, 3, HPE_PROP_INSET_LEFT); }

/**
 * @brief 把 `z-index` 声明写入样式 / Write a `z-index` declaration into the style
 *
 * @details `auto` 置 `z_auto=1` 且 `z_index=0`；纯数字则关闭 `auto` 并取整数值；其它值不改动。
 *          `auto` sets `z_auto=1` with `z_index=0`; a bare number clears `auto` and stores the integer value; other values are ignored.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see parse_plain_number
 */
static void apply_z_index(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "auto")) {
        d->z_auto = 1;
        d->z_index = 0;
    } else {
        float v;
        if (parse_plain_number(value, &v)) {
            d->z_auto = 0;
            d->z_index = (int)v;
        }
    }
    d->set |= HPE_PROP_Z_INDEX;
}

/**
 * @brief 把 `float` 声明写入样式 / Write a `float` declaration into the style
 *
 * @details 识别 `left`、`right`，其余（含 `none`）为 `HPE_FLOAT_NONE`。
 *          It recognises `left` and `right`; everything else, including `none`, is `HPE_FLOAT_NONE`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_clear
 */
static void apply_float(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "left"))
        d->float_side = HPE_FLOAT_LEFT;
    else if (!strcmp(value, "right"))
        d->float_side = HPE_FLOAT_RIGHT;
    else
        d->float_side = HPE_FLOAT_NONE;
    d->set |= HPE_PROP_FLOAT;
}

/**
 * @brief 把 `clear` 声明写入样式 / Write a `clear` declaration into the style
 *
 * @details 识别 `left`、`right`、`both`，其余（含 `none`）为 `HPE_CLEAR_NONE`。
 *          It recognises `left`, `right` and `both`; everything else, including `none`, is `HPE_CLEAR_NONE`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_float
 */
static void apply_clear(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "left"))
        d->clear_side = HPE_CLEAR_LEFT;
    else if (!strcmp(value, "right"))
        d->clear_side = HPE_CLEAR_RIGHT;
    else if (!strcmp(value, "both"))
        d->clear_side = HPE_CLEAR_BOTH;
    else
        d->clear_side = HPE_CLEAR_NONE;
    d->set |= HPE_PROP_CLEAR;
}

/**
 * @brief 写入第二属性字中的一条尺寸长属性 / Write one size longhand carried in the second property word
 *
 * @details 解析长度写入 `slot` 并置位 `bit`，用于 `min-height`/`max-width`/`max-height`（`min-width` 单独处理）。
 *          The length is parsed into `slot` and `bit` is set; used by `min-height`, `max-width` and `max-height` while `min-width` is handled separately.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 * @param[in,out] slot 目标长度槽位 / Length member that receives the value
 * @param[in]     bit 对应的属性位 / The matching property bit
 *
 * @see apply_min_width hpe_len_parse
 */
static void apply_size2(HpeStyle *d, const char *value, HpeLength *slot, uint64_t bit)
{
    *slot = hpe_len_parse(value);
    d->set2 |= bit;
}

static void apply_min_height(HpeStyle *d, const char *v) { apply_size2(d, v, &d->min_height, HPE_PROP2_MIN_HEIGHT); }
static void apply_max_width(HpeStyle *d, const char *v) { apply_size2(d, v, &d->max_width, HPE_PROP2_MAX_WIDTH); }
static void apply_max_height(HpeStyle *d, const char *v) { apply_size2(d, v, &d->max_height, HPE_PROP2_MAX_HEIGHT); }

/**
 * @brief 把 `box-sizing` 声明写入样式 / Write a `box-sizing` declaration into the style
 *
 * @details `border-box` 映射为 `HPE_BOX_SIZING_BORDER`，其余为 `HPE_BOX_SIZING_CONTENT`。
 *          `border-box` maps to `HPE_BOX_SIZING_BORDER` and everything else to `HPE_BOX_SIZING_CONTENT`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_property
 */
static void apply_box_sizing(HpeStyle *d, const char *value)
{
    d->box_sizing = !strcmp(value, "border-box") ? HPE_BOX_SIZING_BORDER : HPE_BOX_SIZING_CONTENT;
    d->set2 |= HPE_PROP2_BOX_SIZING;
}

/**
 * @brief 解析 `overflow` 简写写入两轴 / Parse the `overflow` shorthand into the two axes
 *
 * @details 一个值同时设置两轴，两个值依次为 x、y；关键字经 `overflow_keyword` 映射后同时置位两轴的属性位。
 *          One value sets both axes and two values give x then y; keywords are mapped by `overflow_keyword` and both axis property bits are set.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see overflow_keyword apply_overflow_x apply_overflow_y
 */
static void apply_overflow(HpeStyle *d, const char *value)
{
    char buf[128];
    char *parts[2];
    int n;

    /* one value sets both axes; two values are x then y */
    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 2);
    if (n <= 0)
        return;
    d->overflow_x = overflow_keyword(parts[0]);
    d->overflow_y = n > 1 ? overflow_keyword(parts[1]) : d->overflow_x;
    d->set2 |= HPE_PROP2_OVERFLOW_X | HPE_PROP2_OVERFLOW_Y;
}

/**
 * @brief 把 `overflow-x` 声明写入横向溢出 / Write an `overflow-x` declaration into the horizontal overflow
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see overflow_keyword apply_overflow
 */
static void apply_overflow_x(HpeStyle *d, const char *value)
{
    d->overflow_x = overflow_keyword(value);
    d->set2 |= HPE_PROP2_OVERFLOW_X;
}

/**
 * @brief 把 `overflow-y` 声明写入纵向溢出 / Write an `overflow-y` declaration into the vertical overflow
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see overflow_keyword apply_overflow
 */
static void apply_overflow_y(HpeStyle *d, const char *value)
{
    d->overflow_y = overflow_keyword(value);
    d->set2 |= HPE_PROP2_OVERFLOW_Y;
}

/**
 * @brief 把 `opacity` 声明写入样式并按 0..1 夹取 / Write an `opacity` declaration into the style, clamped to 0..1
 *
 * @details 只有能解析为纯数字时才写入，写入前把值夹到 [0,1]。
 *          The value is written only when it parses as a bare number and is clamped to [0,1] beforehand.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see parse_plain_number
 */
static void apply_opacity(HpeStyle *d, const char *value)
{
    float v;
    if (parse_plain_number(value, &v)) {
        if (v < 0.0f)
            v = 0.0f;
        if (v > 1.0f)
            v = 1.0f;
        d->opacity = v;
        d->set2 |= HPE_PROP2_OPACITY;
    }
}

/**
 * @brief 把 `visibility` 声明写入样式 / Write a `visibility` declaration into the style
 *
 * @details 识别 `hidden`、`collapse`，其余（含 `visible`）为 `HPE_VISIBILITY_VISIBLE`。
 *          It recognises `hidden` and `collapse`; everything else, including `visible`, is `HPE_VISIBILITY_VISIBLE`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_property
 */
static void apply_visibility(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "hidden"))
        d->visibility = HPE_VISIBILITY_HIDDEN;
    else if (!strcmp(value, "collapse"))
        d->visibility = HPE_VISIBILITY_COLLAPSE;
    else
        d->visibility = HPE_VISIBILITY_VISIBLE;
    d->set2 |= HPE_PROP2_VISIBILITY;
}

/**
 * @brief 把 `cursor` 声明映射为光标枚举 / Map a `cursor` declaration to the cursor enum
 *
 * @details 识别 pointer/text/move/wait/help/crosshair/not-allowed/grab/grabbing、`ew-resize`/`col-resize`、`ns-resize`/`row-resize`、`default`；其余（含 `auto`）为 `HPE_CURSOR_AUTO`。
 *          It recognises pointer/text/move/wait/help/crosshair/not-allowed/grab/grabbing, `ew-resize`/`col-resize`, `ns-resize`/`row-resize` and `default`; everything else, including `auto`, is `HPE_CURSOR_AUTO`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_property
 */
static void apply_cursor(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "pointer"))
        d->cursor = HPE_CURSOR_POINTER;
    else if (!strcmp(value, "text"))
        d->cursor = HPE_CURSOR_TEXT;
    else if (!strcmp(value, "move"))
        d->cursor = HPE_CURSOR_MOVE;
    else if (!strcmp(value, "wait"))
        d->cursor = HPE_CURSOR_WAIT;
    else if (!strcmp(value, "help"))
        d->cursor = HPE_CURSOR_HELP;
    else if (!strcmp(value, "crosshair"))
        d->cursor = HPE_CURSOR_CROSSHAIR;
    else if (!strcmp(value, "not-allowed"))
        d->cursor = HPE_CURSOR_NOT_ALLOWED;
    else if (!strcmp(value, "grab"))
        d->cursor = HPE_CURSOR_GRAB;
    else if (!strcmp(value, "grabbing"))
        d->cursor = HPE_CURSOR_GRABBING;
    else if (!strcmp(value, "ew-resize") || !strcmp(value, "col-resize"))
        d->cursor = HPE_CURSOR_EW_RESIZE;
    else if (!strcmp(value, "ns-resize") || !strcmp(value, "row-resize"))
        d->cursor = HPE_CURSOR_NS_RESIZE;
    else if (!strcmp(value, "default"))
        d->cursor = HPE_CURSOR_DEFAULT;
    else
        d->cursor = HPE_CURSOR_AUTO;
    d->set2 |= HPE_PROP2_CURSOR;
}

/**
 * @brief 解析 `text-decoration` 写入装饰位掩码 / Parse `text-decoration` into the decoration bit mask
 *
 * @details 按空白分段，`underline`/`overline`/`line-through` 按位或累加，`none` 清空；多个装饰可叠加。
 *          Segments are split on whitespace and `underline`/`overline`/`line-through` are OR-ed in while `none` clears the mask, so several decorations can combine.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_property
 */
static void apply_text_decoration(HpeStyle *d, const char *value)
{
    char buf[128];
    char *parts[4];
    int n, i;
    HpeTextDecoration dec = HPE_TD_NONE;

    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 4);
    for (i = 0; i < n; i++) {
        if (!strcmp(parts[i], "underline"))
            dec = (HpeTextDecoration)(dec | HPE_TD_UNDERLINE);
        else if (!strcmp(parts[i], "overline"))
            dec = (HpeTextDecoration)(dec | HPE_TD_OVERLINE);
        else if (!strcmp(parts[i], "line-through"))
            dec = (HpeTextDecoration)(dec | HPE_TD_LINE_THROUGH);
        else if (!strcmp(parts[i], "none"))
            dec = HPE_TD_NONE;
    }
    d->text_decoration = dec;
    d->set2 |= HPE_PROP2_TEXT_DECORATION;
}

/**
 * @brief 写入 letter/word-spacing 与 text-indent 长属性 / Write the letter/word-spacing and text-indent longhands
 *
 * @details 解析长度写入 `slot`；`auto`（即 `normal`）被替换为 0px，然后置位 `bit`。
 *          The length is parsed into `slot`; `auto`, i.e. `normal`, is replaced with 0px, then `bit` is set.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 * @param[in,out] slot 目标长度槽位 / Length member that receives the value
 * @param[in]     bit 对应的属性位 / The matching property bit
 *
 * @see apply_letter_spacing apply_vertical_align
 */
static void apply_text_spacing(HpeStyle *d, const char *value, HpeLength *slot, uint64_t bit)
{
    HpeLength len = hpe_len_parse(value);
    if (len.unit == HPE_LEN_AUTO) /* `normal` */
        len = hpe_len_px(0.0f);
    *slot = len;
    d->set2 |= bit;
}

static void apply_letter_spacing(HpeStyle *d, const char *v) { apply_text_spacing(d, v, &d->letter_spacing, HPE_PROP2_LETTER_SPACING); }
static void apply_word_spacing(HpeStyle *d, const char *v) { apply_text_spacing(d, v, &d->word_spacing, HPE_PROP2_WORD_SPACING); }
static void apply_text_indent(HpeStyle *d, const char *v) { apply_text_spacing(d, v, &d->text_indent, HPE_PROP2_TEXT_INDENT); }

/**
 * @brief 把 `vertical-align` 声明写入样式 / Write a `vertical-align` declaration into the style
 *
 * @details 识别 middle/top/bottom/text-top/text-bottom，其余（含 `baseline`）为 `HPE_VALIGN_BASELINE`。
 *          It recognises middle/top/bottom/text-top/text-bottom; everything else, including `baseline`, is `HPE_VALIGN_BASELINE`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_text_spacing
 */
static void apply_vertical_align(HpeStyle *d, const char *value)
{
    if (!strcmp(value, "middle"))
        d->vertical_align = HPE_VALIGN_MIDDLE;
    else if (!strcmp(value, "top"))
        d->vertical_align = HPE_VALIGN_TOP;
    else if (!strcmp(value, "bottom"))
        d->vertical_align = HPE_VALIGN_BOTTOM;
    else if (!strcmp(value, "text-top"))
        d->vertical_align = HPE_VALIGN_TEXT_TOP;
    else if (!strcmp(value, "text-bottom"))
        d->vertical_align = HPE_VALIGN_TEXT_BOTTOM;
    else
        d->vertical_align = HPE_VALIGN_BASELINE;
    d->set2 |= HPE_PROP2_VERTICAL_ALIGN;
}

/**
 * @brief 解析 `list-style` 简写写入类型与位置 / Parse the `list-style` shorthand into type and position
 *
 * @details 按空白分段，circle/square/disc/decimal/none 设置列表类型，inside/outside 设置位置，每命中一项即置位对应的属性位。
 *          Segments are split on whitespace: circle/square/disc/decimal/none set the type and inside/outside set the position, each setting the matching property bit.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_list_style_position
 */
static void apply_list_style(HpeStyle *d, const char *value)
{
    char buf[128];
    char *parts[4];
    int n, i;

    snprintf(buf, sizeof(buf), "%s", value);
    n = split_ws(buf, parts, 4);
    for (i = 0; i < n; i++) {
        if (!strcmp(parts[i], "circle")) {
            d->list_style_type = HPE_LIST_CIRCLE;
            d->set2 |= HPE_PROP2_LIST_STYLE_TYPE;
        } else if (!strcmp(parts[i], "square")) {
            d->list_style_type = HPE_LIST_SQUARE;
            d->set2 |= HPE_PROP2_LIST_STYLE_TYPE;
        } else if (!strcmp(parts[i], "disc")) {
            d->list_style_type = HPE_LIST_DISC;
            d->set2 |= HPE_PROP2_LIST_STYLE_TYPE;
        } else if (!strcmp(parts[i], "decimal")) {
            d->list_style_type = HPE_LIST_DECIMAL;
            d->set2 |= HPE_PROP2_LIST_STYLE_TYPE;
        } else if (!strcmp(parts[i], "none")) {
            d->list_style_type = HPE_LIST_NONE;
            d->set2 |= HPE_PROP2_LIST_STYLE_TYPE;
        } else if (!strcmp(parts[i], "inside")) {
            d->list_style_position = HPE_LIST_INSIDE;
            d->set2 |= HPE_PROP2_LIST_STYLE_POSITION;
        } else if (!strcmp(parts[i], "outside")) {
            d->list_style_position = HPE_LIST_OUTSIDE;
            d->set2 |= HPE_PROP2_LIST_STYLE_POSITION;
        }
    }
}

/**
 * @brief 把 `list-style-position` 声明写入样式 / Write a `list-style-position` declaration into the style
 *
 * @details `inside` 映射为 `HPE_LIST_INSIDE`，其余（含 `outside`）为 `HPE_LIST_OUTSIDE`。
 *          `inside` maps to `HPE_LIST_INSIDE`; everything else, including `outside`, is `HPE_LIST_OUTSIDE`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_list_style
 */
static void apply_list_style_position(HpeStyle *d, const char *value)
{
    d->list_style_position = !strcmp(value, "inside") ? HPE_LIST_INSIDE : HPE_LIST_OUTSIDE;
    d->set2 |= HPE_PROP2_LIST_STYLE_POSITION;
}

/**
 * @brief 解析 `url(...)` 形式的背景图像引用 / Parse a `url(...)` background-image reference
 *
 * @details 只接受 `url(` 前缀并把括号内的引用（可带成对引号）抄进 `bg->url`；`none` 与其它取值返回 0。
 *          Only a `url(` prefix is accepted and the reference inside the parentheses
 *          (optionally quoted) is copied into `bg->url`; `none` and anything else return 0.
 *
 * @param[in]     value 属性值文本 / Property value text
 * @param[in,out] bg 接收解析结果的背景描述 / Background description that receives the result
 *
 * @return 成功返回 1，否则返回 0 / 1 on success, 0 otherwise
 *
 * @note 成功时把 `bg->kind` 置为 `HPE_BG_URL` / On success `bg->kind` is set to `HPE_BG_URL`
 *
 * @see apply_background_image
 */
static int parse_bg_url(const char *value, HpeBackground *bg)
{
    const char *lp, *rp;
    size_t n;

    if (!value)
        return 0;
    while (*value && hpe_is_space(*value))
        value++;
    if (!hpe_starts_with_i(value, "url("))
        return 0;
    lp = strchr(value, '(');
    rp = strrchr(lp, ')');
    if (!rp || rp <= lp + 1)
        return 0;
    lp++;
    while (lp < rp && hpe_is_space(*lp))
        lp++;
    while (rp > lp && hpe_is_space(rp[-1]))
        rp--;
    n = (size_t)(rp - lp);
    if (n >= 2 && (lp[0] == '"' || lp[0] == '\'') && lp[n - 1] == lp[0]) {
        lp++;
        n -= 2;
    }
    if (n == 0 || n >= sizeof(bg->url))
        return 0;
    memcpy(bg->url, lp, n);
    bg->url[n] = '\0';
    bg->kind = HPE_BG_URL;
    return 1;
}

/**
 * @brief 把 `background-image` 声明写入背景图层 / Write a `background-image` declaration into the image layer
 *
 * @details 能解析为线性渐变则写入渐变图层，能解析为 `url(...)` 则写入外链图层，`none` 则把图层类型置为 `HPE_BG_NONE`；各种情形都置位背景图层属性位。
 *          A linear gradient is written into the gradient layer, a `url(...)` into the external
 *          image layer and `none` clears the layer kind to `HPE_BG_NONE`; every case sets the
 *          background-image property bit.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see parse_linear_gradient parse_bg_url apply_background
 */
static void apply_background_image(HpeStyle *d, const char *value)
{
    if (parse_linear_gradient(value, &d->background_image)) {
        d->set2 |= HPE_PROP2_BACKGROUND_IMAGE;
    } else if (parse_bg_url(value, &d->background_image)) {
        d->set2 |= HPE_PROP2_BACKGROUND_IMAGE;
    } else if (!strcmp(value, "none")) {
        d->background_image.kind = HPE_BG_NONE;
        d->set2 |= HPE_PROP2_BACKGROUND_IMAGE;
    }
}

/**
 * @brief 把 `background-repeat` 声明写入样式 / Write a `background-repeat` declaration into the style
 *
 * @details 接受 `repeat` / `repeat-x` / `repeat-y` / `no-repeat`；两个 token 的简写只取第一个轴。
 *          Accepts `repeat`, `repeat-x`, `repeat-y` and `no-repeat`; for the two-token shorthand
 *          only the first axis is kept.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_background_image
 */
static void apply_background_repeat(HpeStyle *d, const char *value)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", value ? value : "");
    {
        char *sp = buf;
        int n = 0;
        while (sp && *sp && n < 2) {
            char *tok;
            while (*sp == ' ' || *sp == '\t')
                sp++;
            if (!*sp)
                break;
            tok = sp;
            while (*sp && *sp != ' ' && *sp != '\t')
                sp++;
            if (*sp)
                *sp++ = '\0';
            if (hpe_str_ieq(tok, "no-repeat"))
                d->bg_repeat = HPE_BG_REPEAT_NO_REPEAT;
            else if (hpe_str_ieq(tok, "repeat-x"))
                d->bg_repeat = HPE_BG_REPEAT_X;
            else if (hpe_str_ieq(tok, "repeat-y"))
                d->bg_repeat = HPE_BG_REPEAT_Y;
            else if (hpe_str_ieq(tok, "repeat"))
                d->bg_repeat = HPE_BG_REPEAT_REPEAT;
            n++;
        }
    }
    d->set2 |= HPE_PROP2_BACKGROUND_REPEAT;
}

/**
 * @brief 把 `object-fit` 声明写入样式 / Write an `object-fit` declaration into the style
 *
 * @details 接受 `fill` / `contain` / `cover` / `none` / `scale-down`，无法识别时按 `fill`。
 *          Accepts `fill`, `contain`, `cover`, `none` and `scale-down`; anything else means `fill`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_object_position
 */
static void apply_object_fit(HpeStyle *d, const char *value)
{
    if (hpe_str_ieq(value, "contain"))
        d->object_fit = HPE_OBJECT_FIT_CONTAIN;
    else if (hpe_str_ieq(value, "cover"))
        d->object_fit = HPE_OBJECT_FIT_COVER;
    else if (hpe_str_ieq(value, "none"))
        d->object_fit = HPE_OBJECT_FIT_NONE;
    else if (hpe_str_ieq(value, "scale-down"))
        d->object_fit = HPE_OBJECT_FIT_SCALE_DOWN;
    else
        d->object_fit = HPE_OBJECT_FIT_FILL;
    d->set2 |= HPE_PROP2_OBJECT_FIT;
}

/**
 * @brief 把 `object-position` 声明写入样式 / Write an `object-position` declaration into the style
 *
 * @details 复用 `background-position` 的解析（关键字 / `%` / 长度）。
 *          Reuses the `background-position` grammar (keywords, `%` and lengths).
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_object_fit
 */
static void apply_object_position(HpeStyle *d, const char *value)
{
    HpeStyle tmp;
    memset(&tmp, 0, sizeof(tmp));
    apply_background_position(&tmp, value);
    d->object_pos = tmp.bg_pos;
    d->set2 |= HPE_PROP2_OBJECT_POSITION;
}

/**
 * @brief 把 `box-shadow` 声明写入样式 / Write a `box-shadow` declaration into the style
 *
 * @details 解析成功写入阴影并使 `valid` 为 1；`none` 则把 `valid` 置 0；两种情形都置位阴影属性位。
 *          A successful parse writes the shadow with `valid` 1 and `none` clears `valid`; both set the box-shadow property bit.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see parse_shadow apply_text_shadow
 */
static void apply_box_shadow(HpeStyle *d, const char *value)
{
    if (parse_shadow(value, &d->box_shadow))
        d->set2 |= HPE_PROP2_BOX_SHADOW;
    else if (!strcmp(value, "none")) {
        d->box_shadow.valid = 0;
        d->set2 |= HPE_PROP2_BOX_SHADOW;
    }
}

/**
 * @brief 把 `text-shadow` 声明写入样式 / Write a `text-shadow` declaration into the style
 *
 * @details 解析成功写入文本阴影并使 `valid` 为 1；`none` 则把 `valid` 置 0；两种情形都置位文本阴影属性位。
 *          A successful parse writes the text shadow with `valid` 1 and `none` clears `valid`; both set the text-shadow property bit.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see parse_shadow apply_box_shadow
 */
static void apply_text_shadow(HpeStyle *d, const char *value)
{
    if (parse_shadow(value, &d->text_shadow))
        d->set2 |= HPE_PROP2_TEXT_SHADOW;
    else if (!strcmp(value, "none")) {
        d->text_shadow.valid = 0;
        d->set2 |= HPE_PROP2_TEXT_SHADOW;
    }
}

/**
 * @brief 把 `transform` 声明写入样式 / Write a `transform` declaration into the style
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @note 无条件写入矩阵并置位 `HPE_PROP2_TRANSFORM` / It writes the matrix unconditionally and sets `HPE_PROP2_TRANSFORM`
 *
 * @see parse_transform
 */
static void apply_transform(HpeStyle *d, const char *value)
{
    parse_transform(value, &d->transform);
    d->set2 |= HPE_PROP2_TRANSFORM;
}

/**
 * @brief 记录 `transition` 是否被声明 / Record whether `transition` was declared
 *
 * @details 值为 `none` 时 `has_transition` 置 0，否则置 1；只记录存在性，引擎尚无插值时钟。
 *          `has_transition` is 0 for `none` and 1 otherwise; only presence is recorded as the engine has no interpolation clock yet.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_animation
 */
static void apply_transition(HpeStyle *d, const char *value)
{
    d->has_transition = !strcmp(value, "none") ? 0 : 1;
    d->set2 |= HPE_PROP2_TRANSITION;
}

/**
 * @brief 记录 `animation` 是否被声明 / Record whether `animation` was declared
 *
 * @details 值为 `none` 时 `has_animation` 置 0，否则置 1；只记录存在性，尚未实现动画播放。
 *          `has_animation` is 0 for `none` and 1 otherwise; only presence is recorded and animation playback is not implemented.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_transition
 */
static void apply_animation(HpeStyle *d, const char *value)
{
    d->has_animation = !strcmp(value, "none") ? 0 : 1;
    d->set2 |= HPE_PROP2_ANIMATION;
}

/**
 * @brief 写入某一边的边框颜色长属性 / Write the border colour longhand of one side
 *
 * @details 解析成功才写入该侧颜色，同时把简写颜色同步为该值，并置位简写颜色位与该侧的颜色位。
 *          The colour is written to that side only on a successful parse, the shorthand colour is synced to the same value, and both the shorthand bit and that side's colour bit are set.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 * @param[in]     side 边序号 0..3（上右下左）/ Side index 0..3 (top, right, bottom, left)
 * @param[in]     bit 该边的颜色属性位 / Colour property bit for that side
 *
 * @see apply_border_color
 */
static void apply_border_side_color(HpeStyle *d, const char *value, int side, uint64_t bit)
{
    HpeColor c;
    if (!hpe_color_parse(value, &c))
        return;
    d->border_colors[side] = c;
    d->border_color = c; /* keep the shorthand in sync */
    d->set |= HPE_PROP_BORDER_COLOR;
    d->set2 |= bit;
}

static void apply_border_top_color(HpeStyle *d, const char *v) { apply_border_side_color(d, v, 0, HPE_PROP2_BORDER_TOP_COLOR); }
static void apply_border_right_color(HpeStyle *d, const char *v) { apply_border_side_color(d, v, 1, HPE_PROP2_BORDER_RIGHT_COLOR); }
static void apply_border_bottom_color(HpeStyle *d, const char *v) { apply_border_side_color(d, v, 2, HPE_PROP2_BORDER_BOTTOM_COLOR); }
static void apply_border_left_color(HpeStyle *d, const char *v) { apply_border_side_color(d, v, 3, HPE_PROP2_BORDER_LEFT_COLOR); }

/**
 * @brief 解析 `content` 声明并存入样式 / Parse a `content` declaration into the style
 *
 * @details 支持 `none`/`normal`（清除）、`"文本"` / `'文本'` 字符串字面量、`attr(name)`（取同名属性值）以及 `open-quote`/`close-quote` 等关键字；多个字面量按出现顺序拼接；转义序列 `\XX` 与 `\n` 被解码。
 *          Accepts `none`/`normal` (clear), a `"text"` / `'text'` string literal, `attr(name)` (the named attribute's value) and keywords such as `open-quote`/`close-quote`; consecutive literals concatenate in order and the escapes `\XX` and `\n` are decoded.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @note 仅在伪元素选择器（`::before` / `::after`）上有可见效果；正文元素的 `content` 被记录但不生成盒 / Only visible on pseudo-element selectors (`::before` / `::after`); on a regular element the value is recorded but no box is generated
 *
 * @see apply_gen_content pseudo_content_for
 */
/* A quoted string in `content`, with the CSS `\\n` / `\\t` / escaped-quote
 * handling. Returns the position just past the closing quote. */
static const char *content_parse_string(HpeStyle *d, const char *p, int *n)
{
    char quote = *p++;
    while (*p && *p != quote && *n < HPE_CONTENT_MAX - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n')
                d->content[(*n)++] = '\n';
            else if (*p == 't')
                d->content[(*n)++] = '\t';
            else
                d->content[(*n)++] = *p;
            p++;
        } else {
            d->content[(*n)++] = *p++;
        }
    }
    if (*p == quote)
        p++;
    return p;
}

/* An `attr(name)` token: the name is stored for later resolution against the
 * node. Returns the position past the closing parenthesis, or NULL on a
 * malformed argument. */
static const char *content_parse_attr(HpeStyle *d, const char *p)
{
    const char *close = strchr(p + 5, ')');
    char name[96];
    size_t ln;
    if (!close)
        return NULL;
    ln = (size_t)(close - p - 5);
    if (ln >= sizeof(name))
        ln = sizeof(name) - 1;
    memcpy(name, p + 5, ln);
    name[ln] = '\0';
    hpe_trim(name);
    d->attr_content[0] = '\0';
    snprintf(d->attr_content, sizeof(d->attr_content), "%s", name);
    return close + 1;
}

static void apply_content(HpeStyle *d, const char *value)
{
    const char *p = value;
    int n = 0;
    d->content[0] = '\0';
    d->has_content = 0;
    d->set2 |= HPE_PROP2_CONTENT;
    if (!*p || hpe_str_ieq(p, "none") || hpe_str_ieq(p, "normal"))
        return;

    while (*p && n < HPE_CONTENT_MAX - 1) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (*p == '"' || *p == '\'') {
            p = content_parse_string(d, p, &n);
        } else if (!strncmp(p, "attr(", 5)) {
            p = content_parse_attr(d, p);
            if (!p)
                break;
        } else if (!strncmp(p, "counter(", 8)) {
            const char *close = strchr(p + 8, ')');
            p = close ? close + 1 : p + 8;
        } else if (!strncmp(p, "open-quote", 10)) {
            d->content[n++] = '"';
            p += 10;
        } else if (!strncmp(p, "close-quote", 11)) {
            d->content[n++] = '"';
            p += 11;
        } else {
            break;
        }
        while (*p == ' ' || *p == '\t')
            p++;
    }
    d->content[n] = '\0';
    d->has_content = 1;
}

/**
 * @brief 解析 `flex-wrap` / `flex-flow` 声明 / Parse a `flex-wrap` / `flex-flow` declaration
 *
 * @details `nowrap`/`wrap`/`wrap-reverse` 依次记 0/1/2；`flex-flow` 中同时出现的 `row`/`column` 部分交给 `apply_flex_direction` 处理。
 *          `nowrap`/`wrap`/`wrap-reverse` map to 0/1/2; the `row`/`column` part of a `flex-flow` shorthand is forwarded to `apply_flex_direction`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_flex_direction
 */
static void apply_flex_wrap(HpeStyle *d, const char *value)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", value);
    {
        char *sp = buf;
        char *w;
        while ((w = strtok(sp, " \t")) != NULL) {
            sp = NULL;
            if (hpe_str_ieq(w, "wrap"))
                d->flex_wrap = 1;
            else if (hpe_str_ieq(w, "wrap-reverse"))
                d->flex_wrap = 2;
            else if (hpe_str_ieq(w, "nowrap"))
                d->flex_wrap = 0;
            else if (hpe_str_ieq(w, "row") || hpe_str_ieq(w, "row-reverse") ||
                     hpe_str_ieq(w, "column") || hpe_str_ieq(w, "column-reverse"))
                apply_flex_direction(d, w);
        }
    }
    d->set2 |= HPE_PROP2_FLEX_WRAP;
}

/**
 * @brief 解析 `order` 声明 / Parse an `order` declaration
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_flex_shrink
 */
static void apply_order(HpeStyle *d, const char *value)
{
    d->order = atoi(value);
    d->set2 |= HPE_PROP2_ORDER;
}

/**
 * @brief 解析 `flex-basis` 声明 / Parse a `flex-basis` declaration
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_flex_shrink
 */
static void apply_flex_basis(HpeStyle *d, const char *value)
{
    d->flex_basis = hpe_len_parse(value);
    d->set2 |= HPE_PROP2_FLEX_BASIS;
}

/**
 * @brief 解析 `flex-shrink` 声明 / Parse a `flex-shrink` declaration
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_flex_basis
 */
static void apply_flex_shrink(HpeStyle *d, const char *value)
{
    d->flex_shrink = (float)atof(value);
    d->set2 |= HPE_PROP2_FLEX_SHRINK;
}

/**
 * @brief 解析 `background-position` 声明 / Parse a `background-position` declaration
 *
 * @details 支持关键字 `left`/`right`/`top`/`bottom`/`center` 与 `%`/`px` 长度；单值时第二位默认 `center`。
 *          Accepts the keywords `left`/`right`/`top`/`bottom`/`center` and `%`/`px` lengths; with a single value the second defaults to `center`.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_background_size
 */
static void apply_background_position(HpeStyle *d, const char *value)
{
    char buf[128];
    char *sp, *tok;
    int n = 0;
    d->bg_pos.present = 1;
    d->bg_pos.x_pct = d->bg_pos.y_pct = 0;
    d->bg_pos.x = d->bg_pos.y = 0.0f;
    snprintf(buf, sizeof(buf), "%s", value);
    sp = buf;
    while ((tok = strtok(sp, " \t,")) != NULL && n < 2) {
        float v = 0.0f;
        int is_pct = 0;
        sp = NULL;
        if (hpe_str_ieq(tok, "left"))
            v = 0.0f, is_pct = 1;
        else if (hpe_str_ieq(tok, "right"))
            v = 100.0f, is_pct = 1;
        else if (hpe_str_ieq(tok, "top"))
            v = 0.0f, is_pct = 1;
        else if (hpe_str_ieq(tok, "bottom"))
            v = 100.0f, is_pct = 1;
        else if (hpe_str_ieq(tok, "center"))
            v = 50.0f, is_pct = 1;
        else {
            size_t ln = strlen(tok);
            if (ln && tok[ln - 1] == '%') {
                v = (float)atof(tok);
                is_pct = 1;
            } else {
                v = (float)atof(tok);
            }
        }
        if (n == 0)
            d->bg_pos.x = v, d->bg_pos.x_pct = is_pct;
        else
            d->bg_pos.y = v, d->bg_pos.y_pct = is_pct;
        n++;
    }
    if (n == 0) {
        d->bg_pos.x = 0.0f, d->bg_pos.x_pct = 1;
        d->bg_pos.y = 50.0f, d->bg_pos.y_pct = 1;
    } else if (n == 1) {
        d->bg_pos.y = 50.0f;
        d->bg_pos.y_pct = 1;
    }
    d->set2 |= HPE_PROP2_BACKGROUND_POSITION;
}

/**
 * @brief 解析 `background-size` 声明 / Parse a `background-size` declaration
 *
 * @details 支持 `auto`、`cover`、`contain` 与一到两个 `%`/`px` 长度。
 *          Accepts `auto`, `cover`, `contain` and one or two `%`/`px` lengths.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_background_position
 */
static void apply_background_size(HpeStyle *d, const char *value)
{
    char buf[128];
    char *sp, *tok;
    int n = 0;
    d->bg_pos.present = 1;
    d->bg_pos.size_auto = d->bg_pos.size_cover = d->bg_pos.size_contain = 0;
    d->bg_pos.sx_pct = d->bg_pos.sy_pct = 0;
    d->bg_pos.size_x = d->bg_pos.size_y = 0.0f;
    snprintf(buf, sizeof(buf), "%s", value);
    sp = buf;
    while ((tok = strtok(sp, " \t,")) != NULL && n < 2) {
        sp = NULL;
        if (hpe_str_ieq(tok, "auto")) {
            if (n == 0)
                d->bg_pos.size_auto = 1;
        } else if (hpe_str_ieq(tok, "cover")) {
            d->bg_pos.size_cover = 1;
        } else if (hpe_str_ieq(tok, "contain")) {
            d->bg_pos.size_contain = 1;
        } else {
            size_t ln = strlen(tok);
            int is_pct = (ln && tok[ln - 1] == '%');
            float v = (float)atof(tok);
            if (n == 0)
                d->bg_pos.size_x = v, d->bg_pos.sx_pct = is_pct;
            else
                d->bg_pos.size_y = v, d->bg_pos.sy_pct = is_pct;
        }
        n++;
    }
    d->set2 |= HPE_PROP2_BACKGROUND_SIZE;
}

/**
 * @brief 解析 `grid-template-columns` / `grid-template-rows` 的轨道串 / Parse a `grid-template-*` track list
 *
 * @details 按空白切分轨道，识别 `Nfr`、`auto`、`%` 与普通长度；超过 `HPE_GRID_MAX_TRACKS` 的部分被忽略。
 *          Tracks are split on whitespace and recognised as `Nfr`, `auto`, `%` or a plain length; anything beyond `HPE_GRID_MAX_TRACKS` is ignored.
 *
 * @param[in]     value 声明值 / Declaration value
 * @param[out]    spec 接收解析出的轨道表 / Receives the parsed track list
 *
 * @see apply_grid_columns apply_grid_gap
 */
static void grid_push_track(HpeGridSpec *spec, const char *tok, size_t ln)
{
    HpeGridTrack *t;
    if (spec->count >= HPE_GRID_MAX_TRACKS || ln == 0)
        return;
    t = &spec->tracks[spec->count++];
    if (ln > 2 && tok[ln - 1] == 'r' && tok[ln - 2] == 'f') {
        t->kind = 1; /* fr */
        t->value = (float)atof(tok);
    } else if (hpe_str_ieq(tok, "auto")) {
        t->kind = 2;
        t->value = 0.0f;
    } else if (tok[ln - 1] == '%') {
        t->kind = 3;
        t->value = (float)atof(tok);
    } else {
        t->kind = 0; /* px / plain number */
        t->value = (float)atof(tok);
    }
}

/* Expand `repeat(N, <track list>)` by pushing the inner list N times. Returns
 * the position just past the `)`, or NULL when the token is malformed. */
static const char *grid_expand_repeat(const char *p, HpeGridSpec *spec)
{
    const char *open = p + 7;
    const char *comma = strchr(open, ',');
    int times = 0;
    if (!comma)
        return NULL;
    {
        const char *q;
        for (q = open; q < comma && *q >= '0' && *q <= '9'; q++)
            times = times * 10 + (*q - '0');
        if (q != comma || times <= 0)
            return NULL;
    }
    {
        const char *in = comma + 1;
        const char *close = strchr(in, ')');
        int rep;
        for (rep = 0; rep < times && spec->count < HPE_GRID_MAX_TRACKS; rep++) {
            const char *ip = in;
            while (ip < close) {
                const char *ts;
                size_t tl;
                while (ip < close && (*ip == ' ' || *ip == '\t'))
                    ip++;
                ts = ip;
                while (ip < close && *ip != ' ' && *ip != '\t')
                    ip++;
                tl = (size_t)(ip - ts);
                if (tl)
                    grid_push_track(spec, ts, tl);
            }
        }
        return close ? close + 1 : in;
    }
}

static void parse_grid_tracks(const char *value, HpeGridSpec *spec)
{
    const char *p = value;
    memset(spec, 0, sizeof(*spec));
    while (*p) {
        const char *tok;
        size_t ln;
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (!*p)
            break;
        if (!strncmp(p, "none", 4) && !(p[4] && (p[4] == '-' || (p[4] >= 'a' && p[4] <= 'z')))) {
            p += 4;
            continue;
        }
        /* `repeat(N, <track list>)`: expand the inner list N times */
        if (!strncmp(p, "repeat(", 7)) {
            const char *next = grid_expand_repeat(p, spec);
            if (next) {
                p = next;
                continue;
            }
        }
        tok = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        ln = (size_t)(p - tok);
        grid_push_track(spec, tok, ln);
    }
    spec->present = spec->count > 0;
}

/**
 * @brief 解析 `grid-template-columns` 声明 / Parse a `grid-template-columns` declaration
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_grid_rows
 */
static void apply_grid_columns(HpeStyle *d, const char *value)
{
    parse_grid_tracks(value, &d->grid_cols);
    d->set2 |= HPE_PROP2_GRID_TEMPLATE_COLUMNS;
}

/**
 * @brief 解析 `grid-template-rows` 声明 / Parse a `grid-template-rows` declaration
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_grid_columns
 */
static void apply_grid_rows(HpeStyle *d, const char *value)
{
    parse_grid_tracks(value, &d->grid_rows);
    d->set2 |= HPE_PROP2_GRID_TEMPLATE_ROWS;
}

/**
 * @brief 解析 `gap` / `row-gap` / `column-gap` 声明 / Parse a `gap` / `row-gap` / `column-gap` declaration
 *
 * @details 视觉上沿用单值 `gap`（与 flex 的 `gap` 一致），多值时取第一个。
 *          A single `gap` value is reused visually (matching the flex `gap`); with several values the first wins.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_gap
 */
static void apply_grid_gap(HpeStyle *d, const char *value)
{
    char buf[64];
    char *sp;
    snprintf(buf, sizeof(buf), "%s", value);
    sp = strtok(buf, " \t");
    if (sp)
        d->grid_gap = (float)atof(sp);
    d->gap = d->grid_gap;
    d->set2 |= HPE_PROP2_GRID_GAP;
}

/**
 * @brief 解析一条 `grid-*-start` / `grid-*-end` 的线值 / Parse one `grid-*-start` / `grid-*-end` line value
 *
 * @details 支持 `auto`（返回 0）、`N`（1 基线）与 `span N`（返回 `-N`）；无法识别时按 `auto` 处理。
 *          `auto` yields 0, a bare `N` yields the 1-based line number and `span N` yields `-N`; anything unrecognised falls back to `auto`.
 *
 * @param[in] tok 去空白后的记号 / Whitespace-trimmed token
 *
 * @return 解析出的线值 / The parsed line value
 *
 * @note `span N` 需要调用方把 `span` 与数字作为两个记号分别处理 / The caller splits `span N` into two tokens
 */
static int parse_grid_line(const char *tok)
{
    if (!tok || !*tok || hpe_str_ieq(tok, "auto"))
        return 0;
    if (tok[0] >= '0' && tok[0] <= '9')
        return atoi(tok);
    return 0;
}

/**
 * @brief 解析 `grid-column` / `grid-row` 声明 / Parse a `grid-column` / `grid-row` declaration
 *
 * @details 值可以是 `start`、`start / end`、`span N` 或 `start / span N`；`span` 记为负数以便与线号区分。
 *          The value may be `start`, `start / end`, `span N` or `start / span N`; `span` is stored negative so it cannot be confused with a line number.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 * @param[in]     is_row 非零表示 `grid-row` / Non-zero selects `grid-row`
 *
 * @see apply_grid_columns
 */
static void apply_grid_line(HpeStyle *d, const char *value, int is_row)
{
    char buf[96];
    char *sp, *tok;
    int vals[2];
    int n = 0;

    snprintf(buf, sizeof(buf), "%s", value);
    sp = buf;
    while (n < 2 && (tok = strtok(sp, " /")) != NULL) {
        sp = NULL;
        if (hpe_str_ieq(tok, "span")) {
            /* the next token is the span length */
            char *num = strtok(NULL, " /");
            int k = num ? atoi(num) : 1;
            vals[n++] = k > 0 ? -k : -1;
            continue;
        }
        vals[n++] = parse_grid_line(tok);
    }
    if (is_row) {
        d->grid_row_start = vals[0];
        d->grid_row_end = n > 1 ? vals[1] : 0;
        d->set2 |= HPE_PROP2_GRID_ROW;
    } else {
        d->grid_col_start = vals[0];
        d->grid_col_end = n > 1 ? vals[1] : 0;
        d->set2 |= HPE_PROP2_GRID_COLUMN;
    }
}

/**
 * @brief 解析 `grid-column` 声明 / Parse a `grid-column` declaration
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_grid_line
 */
static void apply_grid_column(HpeStyle *d, const char *value)
{
    apply_grid_line(d, value, 0);
}

/**
 * @brief 解析 `grid-row` 声明 / Parse a `grid-row` declaration
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_grid_line
 */
static void apply_grid_row(HpeStyle *d, const char *value)
{
    apply_grid_line(d, value, 1);
}

/**
 * @brief 解析 `justify-items` 声明 / Parse a `justify-items` declaration
 *
 * @details 复用 `HpeAlignItems` 取值；`stretch` 是默认值。
 *          Reuses the `HpeAlignItems` values; `stretch` is the default.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_align_items
 */
static void apply_justify_items(HpeStyle *d, const char *value)
{
    if (hpe_str_ieq(value, "start") || hpe_str_ieq(value, "flex-start"))
        d->justify_items = HPE_ALIGN_ITEMS_FLEX_START;
    else if (hpe_str_ieq(value, "center"))
        d->justify_items = HPE_ALIGN_ITEMS_CENTER;
    else if (hpe_str_ieq(value, "end") || hpe_str_ieq(value, "flex-end"))
        d->justify_items = HPE_ALIGN_ITEMS_FLEX_END;
    else
        d->justify_items = HPE_ALIGN_ITEMS_STRETCH;
    d->set2 |= HPE_PROP2_JUSTIFY_ITEMS;
}

/**
 * @brief 解析 `align-content` 声明 / Parse an `align-content` declaration
 *
 * @details 复用 `HpeJustify` 取值，作用于整张网格在容器内的分布。
 *          Reuses the `HpeJustify` values and distributes the whole grid inside its container.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_justify_content
 */
static void apply_align_content(HpeStyle *d, const char *value)
{
    if (hpe_str_ieq(value, "center"))
        d->align_content = HPE_JUSTIFY_CENTER;
    else if (hpe_str_ieq(value, "end") || hpe_str_ieq(value, "flex-end"))
        d->align_content = HPE_JUSTIFY_END;
    else if (hpe_str_ieq(value, "space-between"))
        d->align_content = HPE_JUSTIFY_SPACE_BETWEEN;
    else if (hpe_str_ieq(value, "space-around"))
        d->align_content = HPE_JUSTIFY_SPACE_AROUND;
    else
        d->align_content = HPE_JUSTIFY_START;
    d->set2 |= HPE_PROP2_ALIGN_CONTENT;
}

/**
 * @brief 解析 `grid-auto-flow` 声明 / Parse a `grid-auto-flow` declaration
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @note 只区分 `row`（默认）与 `column` / Only `row` (the default) and `column` are distinguished
 */
static void apply_grid_auto_flow(HpeStyle *d, const char *value)
{
    d->grid_auto_flow = hpe_str_ieq(value, "column") ? 1 : 0;
    d->set2 |= HPE_PROP2_GRID_AUTO_FLOW;
}

/**
 * @brief 解析 `table-layout` 声明 / Parse a `table-layout` declaration
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_border_spacing
 */
static void apply_table_layout(HpeStyle *d, const char *value)
{
    d->table_layout_fixed = hpe_str_ieq(value, "fixed");
    d->set2 |= HPE_PROP2_TABLE_LAYOUT;
}

/**
 * @brief 解析 `border-spacing` 声明 / Parse a `border-spacing` declaration
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_table_layout
 */
static void apply_border_spacing(HpeStyle *d, const char *value)
{
    float h = (float)atof(value);
    const char *sp = strpbrk(value, " \t");
    float v = h;
    if (sp)
        v = (float)atof(sp + 1);
    d->border_spacing_h = h;
    d->border_spacing_v = v;
    d->set2 |= HPE_PROP2_BORDER_SPACING;
}

/**
 * @brief 解析 `scrollbar-width` 声明 / Parse a `scrollbar-width` declaration
 *
 * @details 接受 `auto`（缺省 12px）、`thin`（8px）、`none`（0，等效隐藏滚动条）与
 *          具体长度；元素级滚动容器的滚动条占位与绘制都取该值。
 *          Accepts `auto` (the 12px default), `thin` (8px), `none` (0, i.e. a hidden scrollbar) and a concrete length; the element scrollbar's reserved space and its painted width both read this value.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_overflow
 */
static void apply_scrollbar_width(HpeStyle *d, const char *value)
{
    char buf[32];
    size_t n;
    if (!value)
        return;
    while (*value == ' ' || *value == '\t')
        value++;
    n = strlen(value);
    while (n > 0 && (value[n - 1] == ' ' || value[n - 1] == '\t' || value[n - 1] == ';'))
        n--;
    if (n >= sizeof(buf))
        return;
    memcpy(buf, value, n);
    buf[n] = '\0';
    if (hpe_str_ieq(buf, "auto")) {
        d->scrollbar_width = 12.0f;
    } else if (hpe_str_ieq(buf, "thin")) {
        d->scrollbar_width = 8.0f;
    } else if (hpe_str_ieq(buf, "none")) {
        d->scrollbar_width = 0.0f;
    } else {
        const char *p = buf;
        char *end = NULL;
        double v;
        if (*p == '-' || *p == '+')
            p++;
        if (*p < '0' || *p > '9')
            return;
        v = strtod(buf, &end);
        if (end == buf)
            return;
        d->scrollbar_width = (float)v;
    }
    d->set2 |= HPE_PROP2_SCROLLBAR_WIDTH;
}

/**
 * @brief 解析 `direction` 声明 / Parse a `direction` declaration
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     value 声明值 / Declaration value
 *
 * @see apply_white_space
 */
static void apply_direction(HpeStyle *d, const char *value)
{
    d->direction_rtl = hpe_str_ieq(value, "rtl");
    d->set2 |= HPE_PROP2_DIRECTION;
}

/* name -> applier registry; the single source of truth for the property surface */
typedef void (*PropApplier)(HpeStyle *d, const char *value);
typedef struct {
    const char *name;
    PropApplier fn;
} PropEntry;

static const PropEntry k_prop_table[] = {
    {"display", apply_display},
    {"color", apply_color},
    {"background", apply_background},
    {"background-color", apply_background},
    {"min-width", apply_min_width},
    {"width", apply_width},
    {"height", apply_height},
    {"margin", apply_margin},
    {"padding", apply_padding},
    {"margin-top", apply_margin_top},
    {"margin-right", apply_margin_right},
    {"margin-bottom", apply_margin_bottom},
    {"margin-left", apply_margin_left},
    {"padding-top", apply_padding_top},
    {"padding-right", apply_padding_right},
    {"padding-bottom", apply_padding_bottom},
    {"padding-left", apply_padding_left},
    {"border-color", apply_border_color},
    {"border", apply_border},
    {"border-width", apply_border_width},
    {"border-top", apply_border_top},
    {"border-right", apply_border_right},
    {"border-bottom", apply_border_bottom},
    {"border-left", apply_border_left},
    {"border-radius", apply_border_radius},
    {"inset", apply_inset},
    {"flex-direction", apply_flex_direction},
    {"align-items", apply_align_items},
    {"align-self", apply_align_self},
    {"justify-content", apply_justify},
    {"gap", apply_gap},
    {"column-gap", apply_gap},
    {"row-gap", apply_gap},
    {"flex", apply_flex_grow},
    {"flex-grow", apply_flex_grow},
    {"font-size", apply_font_size},
    {"font-weight", apply_font_weight},
    {"text-align", apply_text_align},
    {"line-height", apply_line_height},
    {"white-space", apply_white_space},
    {"font-family", apply_font_family},
    {"position", apply_position},
    {"top", apply_top},
    {"right", apply_right},
    {"bottom", apply_bottom},
    {"left", apply_left},
    {"z-index", apply_z_index},
    {"float", apply_float},
    {"clear", apply_clear},
    {"min-height", apply_min_height},
    {"max-width", apply_max_width},
    {"max-height", apply_max_height},
    {"box-sizing", apply_box_sizing},
    {"overflow", apply_overflow},
    {"overflow-x", apply_overflow_x},
    {"overflow-y", apply_overflow_y},
    {"opacity", apply_opacity},
    {"visibility", apply_visibility},
    {"cursor", apply_cursor},
    {"text-decoration", apply_text_decoration},
    {"text-decoration-line", apply_text_decoration},
    {"letter-spacing", apply_letter_spacing},
    {"word-spacing", apply_word_spacing},
    {"text-indent", apply_text_indent},
    {"vertical-align", apply_vertical_align},
    {"list-style-type", apply_list_style},
    {"list-style", apply_list_style},
    {"list-style-position", apply_list_style_position},
    {"background-image", apply_background_image},
    {"box-shadow", apply_box_shadow},
    {"text-shadow", apply_text_shadow},
    {"transform", apply_transform},
    {"transition", apply_transition},
    {"animation", apply_animation},
    {"border-top-color", apply_border_top_color},
    {"border-right-color", apply_border_right_color},
    {"border-bottom-color", apply_border_bottom_color},
    {"border-left-color", apply_border_left_color},
    {"content", apply_content},
    {"flex-wrap", apply_flex_wrap},
    {"flex-flow", apply_flex_wrap},
    {"order", apply_order},
    {"flex-basis", apply_flex_basis},
    {"flex-shrink", apply_flex_shrink},
    {"background-position", apply_background_position},
    {"background-size", apply_background_size},
    {"background-repeat", apply_background_repeat},
    {"object-fit", apply_object_fit},
    {"object-position", apply_object_position},
    {"grid-template-columns", apply_grid_columns},
    {"grid-template-rows", apply_grid_rows},
    {"grid-column", apply_grid_column},
    {"grid-column-start", apply_grid_column},
    {"grid-column-end", apply_grid_column},
    {"grid-row", apply_grid_row},
    {"grid-row-start", apply_grid_row},
    {"grid-row-end", apply_grid_row},
    {"justify-items", apply_justify_items},
    {"align-content", apply_align_content},
    {"grid-auto-flow", apply_grid_auto_flow},
    {"grid-gap", apply_grid_gap},
    {"table-layout", apply_table_layout},
    {"border-spacing", apply_border_spacing},
    {"direction", apply_direction},
    {"scrollbar-width", apply_scrollbar_width},
};

/**
 * @brief 按属性名查表并分派到对应应用函数 / Look a property name up in the table and dispatch to its applier
 *
 * @details 先把属性名转为小写，`inherit` 直接忽略（层叠已从父样式起步）；随后在 `k_prop_table` 中按名匹配并调用对应函数，未注册的属性名静默忽略。
 *          The property name is lowercased, `inherit` is ignored outright because the cascade already starts from the parent style, and the name is then matched in `k_prop_table` and dispatched; an unregistered name is silently ignored.
 *
 * @param[in,out] d 目标样式 / Target style
 * @param[in]     prop CSS 属性名（大小写不敏感）/ CSS property name, case-insensitive
 * @param[in]     value 已剥离 `!important` 的声明值 / Declaration value with `!important` already stripped
 *
 * @see hpe_css_parse_declarations
 */
static void apply_property(HpeStyle *d, const char *prop, const char *value)
{
    char lprop[64];
    size_t i;

    if (!prop || !value)
        return;
    snprintf(lprop, sizeof(lprop), "%s", prop);
    for (i = 0; lprop[i]; i++)
        lprop[i] = (char)hpe_ascii_lower((unsigned char)lprop[i]);

    /* the cascade already starts from the parent's computed style, so "inherit"
     * needs no work; being lenient here keeps `font-family: inherit` usable. */
    if (!strcmp(value, "inherit"))
        return;

    for (i = 0; i < sizeof(k_prop_table) / sizeof(k_prop_table[0]); i++) {
        if (!strcmp(lprop, k_prop_table[i].name)) {
            k_prop_table[i].fn(d, value);
            return;
        }
    }
}

/**
 * @brief 去除尾部 `!important` 并报告是否存在 / Strip a trailing `!important` and report whether it was present
 *
 * @details 先把值复制到 `buf` 并去掉尾部空白，若末尾恰好是 `!important` 则截断并把 `*out_important` 置 1。
 *          The value is copied into `buf` and trailing whitespace trimmed; when the tail is exactly `!important` it is cut off and `*out_important` is set to 1.
 *
 * @param[in]     value 待处理的声明值 / Declaration value to process
 * @param[out]    buf 存放剥离后文本的缓冲 / Buffer that holds the stripped text
 * @param[in]     buf_size `buf` 的容量 / Capacity of `buf`
 * @param[out]    out_important 置 1 表示存在 `!important` / Set to 1 when `!important` was present
 *
 * @return 指向 `buf` 内去空白后的值文本 / Pointer to the trimmed value text inside `buf`
 *
 * @note `out_important` 可为 `NULL` / `out_important` may be `NULL`
 *
 * @see hpe_css_parse_declarations
 */

static const char *strip_important(const char *value, char *buf, size_t buf_size, int *out_important)
{
    size_t len;
    if (out_important)
        *out_important = 0;
    snprintf(buf, buf_size, "%s", value);
    len = strlen(buf);
    while (len > 0 && hpe_is_space(buf[len - 1]))
        buf[--len] = '\0';
    if (len >= 10 && hpe_str_ieq(buf + len - 10, "!important")) {
        buf[len - 10] = '\0';
        if (out_important)
            *out_important = 1;
        return hpe_trim(buf);
    }
    return hpe_trim(buf);
}

/**
 * @brief 解析声明块并写入样式 / Parse a declaration block into a style
 *
 * @details 输入通常是 `style=""` 属性的内容；按 `;` 逐条拆分，对每条解析 `属性: 值` 并调用 `apply_property`，随后据该条是否带 `!important` 把新置位的属性位补记到 `imp`/`imp2`。
 *          The input is normally the content of a `style=""` attribute; it is split on `;` and each `property: value` pair is parsed through `apply_property`, after which the bits newly set by that declaration are copied into `imp`/`imp2` when it carried `!important`.
 *
 * @param[in]     text 声明块文本 / Declaration block text
 * @param[in]     length `text` 的字节长度 / Byte length of `text`
 * @param[out]    decl 接收解析结果的样式 / Receives the parsed style
 *
 * @note 进入时先清零 `decl`；属性名或值为空、或无冒号的声明会被跳过 / `decl` is zeroed on entry; a declaration with a missing name or value, or without a colon, is skipped
 *
 * @see apply_property strip_important
 */
void hpe_css_parse_declarations(const char *text, size_t length, HpeStyle *decl)
{
    char *copy;
    char *p;
    if (!decl)
        return;
    memset(decl, 0, sizeof(*decl));
    if (!text || length == 0)
        return;
    copy = hpe_strndup(text, length);
    p = copy;
    while (*p) {
        char *decl_end = p;
        char *colon;
        char *prop, *value;
        char vbuf[512];

        while (*decl_end && *decl_end != ';')
            decl_end++;
        {
            char saved = *decl_end;
            *decl_end = '\0';
            colon = strchr(p, ':');
            if (colon) {
                *colon = '\0';
                prop = hpe_trim(p);
                value = hpe_trim(colon + 1);
                if (*prop && *value) {
                    int important = 0;
                    uint64_t before, before2;
                    const char *clean =
                        strip_important(value, vbuf, sizeof(vbuf), &important);
                    before = decl->set;
                    before2 = decl->set2;
                    apply_property(decl, prop, clean);
                    if (important) {
                        /* mark exactly the properties this declaration set */
                        decl->imp |= decl->set & ~before;
                        decl->imp2 |= decl->set2 & ~before2;
                    }
                }
            }
            *decl_end = saved;
        }
        if (*decl_end == ';')
            p = decl_end + 1;
        else
            break;
    }
    hpe_free(copy);
}

/**
 * @brief 判断字符是否为标识符字符 / Test whether a character may appear in an identifier
 *
 * @details 只接受 ASCII 字母、数字、下划线与连字符。
 *          Only ASCII letters, digits, underscores and hyphens are accepted.
 *
 * @param[in]     c 待判断的字符 / Character to test
 *
 * @return 是标识符字符返回 1，否则返回 0 / 1 when it is an identifier character, 0 otherwise
 *
 * @see read_ident
 */

static int is_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

/**
 * @brief 从游标处读取一个标识符 / Read one identifier starting at the cursor
 *
 * @details 连续读取标识符字符，超过 `out_size-1` 的部分被截断；什么都没读到则返回 `NULL`；成功后 `*pp` 前移到标识符末尾。
 *          Consecutive identifier characters are read and anything beyond `out_size-1` is truncated; `NULL` is returned when nothing was read and otherwise `*pp` advances past the identifier.
 *
 * @param[in,out] pp 解析游标指针 / Parse cursor pointer
 * @param[in]     end 输入缓冲区末尾 / End of the input buffer
 * @param[out]    out 标识符缓冲 / Identifier buffer
 * @param[in]     out_size `out` 的容量 / Capacity of `out`
 *
 * @return 标识符缓冲 `out`，无标识符时返回 `NULL` / The identifier buffer `out`, or `NULL` when none was read
 *
 * @see is_ident_char
 */
static char *read_ident(const char **pp, const char *end, char *out, size_t out_size)
{
    const char *p = *pp;
    const char *start = p;
    size_t n;
    while (p < end && is_ident_char(*p))
        p++;
    if (p == start)
        return NULL;
    n = (size_t)(p - start);
    if (n >= out_size)
        n = out_size - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    *pp = p;
    return out;
}

/**
 * @brief 给复合选择器追加一个类名 / Append a class name to a compound selector
 *
 * @details 扩容 `classes` 数组并存入类名的副本，类计数加一。
 *          The `classes` array is grown and a copy of the name stored, incrementing the class count.
 *
 * @param[in,out] c 目标复合选择器 / Target compound selector
 * @param[in]     name 类名 / Class name
 *
 * @see compound_free parse_class_or_id
 */
static void compound_add_class(HpeCompound *c, const char *name)
{
    c->classes = (char **)hpe_grow(c->classes, &c->class_cap, (size_t)c->class_count + 1,
                                   sizeof(char *));
    c->classes[c->class_count++] = hpe_strdup(name);
}

/**
 * @brief 释放复合选择器持有的全部内存 / Release every allocation held by a compound selector
 *
 * @details 依次释放标签、id、各类名与属性选择器的名字/运算符/值，最后把整个结构清零。
 *          The tag, id, each class name and every attribute selector's name, operator and value are released before the struct is zeroed.
 *
 * @param[in,out] c 目标复合选择器 / Target compound selector
 *
 * @see compound_add_class compound_add_attr
 */
static void compound_free(HpeCompound *c);

/**
 * @brief 释放 `:not()` / `:is()` 参数中的嵌套选择器 / Release the nested selectors of `:not()` / `:is()`
 *
 * @details 每个嵌套选择器由若干复合部分组成，逐部分释放其内部资源后释放数组本体；`not_list` 与 `is_list` 都会处理。
 *          Each nested selector consists of several compounds whose internal resources are released one by one before the array itself; both `not_list` and `is_list` are handled.
 *
 * @param[in,out] c 目标复合选择器 / Target compound selector
 *
 * @see compound_free
 */
static void nested_sel_lists_free(HpeCompound *c)
{
    int i, j;
    for (i = 0; i < c->not_count; i++) {
        for (j = 0; j < c->not_list[i].count; j++)
            compound_free(&c->not_list[i].parts[j]);
        if (c->not_list[i].parts)
            hpe_free(c->not_list[i].parts);
    }
    for (i = 0; i < c->is_count; i++) {
        for (j = 0; j < c->is_list[i].count; j++)
            compound_free(&c->is_list[i].parts[j]);
        if (c->is_list[i].parts)
            hpe_free(c->is_list[i].parts);
    }
}

static void compound_free(HpeCompound *c)
{
    int i;
    if (c->tag)
        hpe_free(c->tag);
    if (c->id)
        hpe_free(c->id);
    for (i = 0; i < c->class_count; i++)
        hpe_free(c->classes[i]);
    if (c->classes)
        hpe_free(c->classes);
    for (i = 0; i < c->attr_count; i++) {
        if (c->attrs[i].name)
            hpe_free(c->attrs[i].name);
        if (c->attrs[i].op)
            hpe_free(c->attrs[i].op);
        if (c->attrs[i].value)
            hpe_free(c->attrs[i].value);
    }
    if (c->attrs)
        hpe_free(c->attrs);
    nested_sel_lists_free(c);
    if (c->not_list)
        hpe_free(c->not_list);
    if (c->is_list)
        hpe_free(c->is_list);
    memset(c, 0, sizeof(*c));
}



/**
 * @brief 解析一个 `.class` 或 `#id` token / Parse one `.class` or `#id` token
 *
 * @details 读取标记符（`.` 或 `#`）后的标识符：`.` 追加类名，`#` 写入 id（已有 id 时替换）；标识符为空视为格式错误。
 *          The identifier after the sigil (`.` or `#`) is read: `.` appends a class and `#` writes the id, replacing any previous one; an empty identifier is a malformed selector.
 *
 * @param[in,out] out 目标复合选择器 / Target compound selector
 * @param[in,out] pp 解析游标指针 / Parse cursor pointer
 * @param[in]     end 输入缓冲区末尾 / End of the input buffer
 * @param[out]    ident 标识符临时缓冲 / Scratch identifier buffer
 * @param[in]     ident_size `ident` 的容量 / Capacity of `ident`
 *
 * @return 成功返回 1，格式错误返回 0 / 1 on success, 0 on a malformed selector
 *
 * @see read_ident compound_add_class
 */
static int parse_class_or_id(HpeCompound *out, const char **pp, const char *end, char *ident,
                             size_t ident_size)
{
    const char *p = *pp;
    char sigil = *p++;

    if (!read_ident(&p, end, ident, ident_size))
        return 0;
    if (sigil == '.')
        compound_add_class(out, ident);
    else {
        if (out->id)
            hpe_free(out->id);
        out->id = hpe_strdup(ident);
    }
    *pp = p;
    return 1;
}

/**
 * @brief 解析 `:nth-*(an+b)` 的括号参数 / Parse the parenthesised argument of `:nth-*(an+b)`
 *
 * @details 支持 `odd` / `even` 关键字、纯整数、`an+b` / `an-b` / `an` / `n` / `-n+a` 等写法；系数与常数允许带正负号并跳过空白。解析失败时把 `out->present` 清零（该伪类匹配任何下标），并发出一条调试信息。
 *          Accepts the `odd` / `even` keywords, a plain integer and the `an+b` / `an-b` / `an` / `n` / `-n+a` forms; coefficients and constants may carry a sign and whitespace is skipped. On failure `out->present` is cleared (the pseudo then matches any index) and a debug message is emitted.
 *
 * @param[in,out] pp 解析游标指针，指向函数名之后 / Parse cursor pointing just past the function name
 * @param[in]     end 输入缓冲区末尾 / End of the input buffer
 * @param[in]     name 伪类名（用于诊断输出）/ Pseudo-class name, used for diagnostics
 * @param[out]    out 接收解析出的 `an+b` / Receives the parsed `an+b`
 *
 * @return 成功返回 1，缺少左括号或右括号不配返回 0 / 1 on success, 0 when a bracket is missing or unbalanced
 *
 * @see nth_matches parse_pseudo
 */
/* Parse the optional `+b` / `-b` tail that follows an `an` term. Fills *b and
 * *has_b; advances *q past the sign and digits. */
static void nth_parse_b(const char **qq, int *b, int *has_b)
{
    const char *q = *qq;
    int sneg, v = 0;
    if (!(*q == '+' || *q == '-'))
        return;
    sneg = (*q++ == '-');
    while (*q && hpe_is_space(*q))
        q++;
    while (css_is_digit(*q))
        v = v * 10 + (*q++ - '0');
    *b = sneg ? -v : v;
    *has_b = 1;
    *qq = q;
}

/* Parse the `an` (or bare integer / the `odd` / `even` keywords) head of an
 * `:nth-*()` argument into *a / *b / *has_a / *has_b. */
static void nth_parse_head(const char **qq, int *a, int *b, int *has_a, int *has_b)
{
    const char *q = *qq;
    int neg = 0;
    if (*q == '+' || *q == '-')
        neg = (*q++ == '-');
    if (*q == 'n' || *q == 'N') {
        *a = neg ? -1 : 1;
        *has_a = 1;
        q++;
    } else if (css_is_digit(*q)) {
        int v = 0;
        while (css_is_digit(*q))
            v = v * 10 + (*q++ - '0');
        if (*q == 'n' || *q == 'N') {
            *a = neg ? -v : v;
            *has_a = 1;
            q++;
        } else {
            *b = neg ? -v : v;
            *has_b = 1;
        }
    }
    *qq = q;
}

static int parse_nth_arg(const char **pp, const char *end, const char *name, HpeNth *out)
{
    const char *p = *pp;
    const char *close;
    char body[64];
    size_t n;
    int a = 0, b = 0, has_a = 0, has_b = 0;
    const char *q;

    out->present = 0;
    if (p >= end || *p != '(')
        return 0;
    close = p + 1;
    while (close < end && *close != ')')
        close++;
    if (close >= end)
        return 0;
    n = (size_t)(close - p - 1);
    if (n >= sizeof(body))
        n = sizeof(body) - 1;
    memcpy(body, p + 1, n);
    body[n] = '\0';
    q = body;
    while (*q && hpe_is_space(*q))
        q++;
    if (hpe_str_ieq(q, "odd")) {
        a = 2;
        b = 1;
        has_a = has_b = 1;
    } else if (hpe_str_ieq(q, "even")) {
        a = 2;
        b = 0;
        has_a = has_b = 1;
    } else {
        nth_parse_head(&q, &a, &b, &has_a, &has_b);
        while (*q && hpe_is_space(*q))
            q++;
        if (has_a)
            nth_parse_b(&q, &b, &has_b);
    }
    *pp = close + 1;
    if (!has_a && !has_b) {
        fprintf(stderr, "[hope] malformed :%s() ignored: '%.*s'\n", name, (int)n, body);
        return 1;
    }
    out->a = a;
    out->b = b;
    out->present = 1;
    return 1;
}

/**
 * @brief 解析 `:not()` / `:is()` / `:where()` 的括号参数 / Parse the parenthesised argument of `:not()` / `:is()` / `:where()`
 *
 * @details 参数是一份以逗号分隔的选择器列表，按嵌套规则解析后挂到 `not_list` 或 `is_list`；`:where()` 只影响特异性（记 0）而不改变匹配语义。
 *          The argument is a comma-separated selector list parsed recursively and attached to `not_list` or `is_list`; `:where()` only affects specificity (counted as zero) rather than matching.
 *
 * @param[in,out] out 目标复合选择器 / Target compound selector
 * @param[in,out] pp 解析游标指针，指向函数名之后 / Parse cursor pointing just past the function name
 * @param[in]     end 输入缓冲区末尾 / End of the input buffer
 * @param[in]     name 伪类名 / Pseudo-class name
 *
 * @return 成功返回 1，括号不配返回 0 / 1 on success, 0 when the brackets are unbalanced
 *
 * @note `:not()`/`:is()` 只接受简单选择器（复合链），不支持组合符——与浏览器一致地降级 / Only compound selectors are accepted (no combinators), degrading the same way browsers do
 *
 * @see parse_one_selector parse_pseudo
 */
/* Find the `)` that closes the `(` at p, honouring nesting. Returns NULL when
 * the parenthesis is unbalanced. */
static const char *find_matching_paren(const char *p, const char *end)
{
    const char *close = p + 1;
    int depth = 1;
    while (close < end && depth > 0) {
        if (*close == '(')
            depth++;
        else if (*close == ')')
            depth--;
        if (depth == 0)
            break;
        close++;
    }
    return close >= end ? NULL : close;
}

/* Parse the comma-separated selector list in `body` into a fresh array. */
static void parse_sel_arg_list(const char *body, HpeSelector **arr, int *cnt)
{
    HpeRule tmp;
    int i;
    memset(&tmp, 0, sizeof(tmp));
    parse_selector_list(&tmp, body, strlen(body));
    if (tmp.selector_count > 0) {
        *arr = (HpeSelector *)hpe_calloc((size_t)tmp.selector_count, sizeof(HpeSelector));
        for (i = 0; i < tmp.selector_count; i++)
            (*arr)[i] = tmp.selectors[i];
        *cnt = tmp.selector_count;
    }
    if (tmp.selectors)
        hpe_free(tmp.selectors);
}

static int parse_sel_arg(HpeCompound *out, const char **pp, const char *end, const char *name)
{
    const char *p = *pp;
    const char *close;
    HpeSelector **arr;
    int *cnt;
    char *body;

    if (p >= end || *p != '(')
        return 0;
    close = find_matching_paren(p, end);
    if (!close)
        return 0;
    body = hpe_strndup(p + 1, (size_t)(close - p - 1));
    if (hpe_str_ieq(name, "not")) {
        arr = &out->not_list;
        cnt = &out->not_count;
    } else {
        arr = &out->is_list;
        cnt = &out->is_count;
        if (hpe_str_ieq(name, "where"))
            out->is_where = 1;
    }
    parse_sel_arg_list(body, arr, cnt);
    hpe_free(body);
    *pp = close + 1;
    return 1;
}

/**
 * @brief 解析一个 `:pseudo` token / Parse one `:pseudo` token
 *
 * @details `::before`/`::after` 记为伪元素；`:root` 在缺少标签时补上 `html`；`:hover`/`:focus`/`:active`/`:visited`/`:link`/`:target` 置对应状态位；`:first-child` 一族与 `:empty`/`:checked`/`:disabled`/`:enabled` 置结构位；`:nth-*()` 解析 `an+b`；`:not()`/`:is()`/`:where()` 递归解析参数。未知伪类被有意忽略，使选择器仍能解析而不失败。
 *          `::before`/`::after` mark a pseudo-element; `:root` supplies an `html` tag when absent; `:hover`/`:focus`/`:active`/`:visited`/`:link`/`:target` set the matching state bit; the `:first-child` family and `:empty`/`:checked`/`:disabled`/`:enabled` set structural bits; `:nth-*()` parse `an+b`; `:not()`/`:is()`/`:where()` recurse into their argument. Unknown pseudo-classes are ignored on purpose so the selector still parses.
 *
 * @param[in,out] out 目标复合选择器 / Target compound selector
 * @param[in,out] pp 解析游标指针 / Parse cursor pointer
 * @param[in]     end 输入缓冲区末尾 / End of the input buffer
 * @param[out]    ident 标识符临时缓冲 / Scratch identifier buffer
 * @param[in]     ident_size `ident` 的容量 / Capacity of `ident`
 *
 * @return 成功返回 1，缺少标识符返回 0 / 1 on success, 0 when the identifier is missing
 *
 * @see read_ident parse_compound parse_nth_arg parse_sel_arg
 */
/* The `:hover` / `:focus` / `:active` / `:visited` / `:link` / `:target` state
 * bits. Returns 1 when the identifier was recognised. */
static int pseudo_state_bit(HpeCompound *out, const char *ident)
{
    if (hpe_str_ieq(ident, "hover"))
        out->pseudo |= HPE_PS_HOVER;
    else if (hpe_str_ieq(ident, "focus") || hpe_str_ieq(ident, "focus-visible") ||
             hpe_str_ieq(ident, "focus-within"))
        out->pseudo |= HPE_PS_FOCUS;
    else if (hpe_str_ieq(ident, "active"))
        out->pseudo |= HPE_PS_ACTIVE;
    else if (hpe_str_ieq(ident, "visited"))
        out->pseudo |= HPE_PS_VISITED;
    else if (hpe_str_ieq(ident, "link") || hpe_str_ieq(ident, "any-link"))
        out->pseudo |= HPE_PS_LINK;
    else if (hpe_str_ieq(ident, "target"))
        out->pseudo |= HPE_PS_TARGET;
    else
        return 0;
    return 1;
}

/* The `:first-child` family and `:empty` / `:checked` / `:disabled` / `:enabled`
 * structural bits. Returns 1 when recognised. */
static int pseudo_structural_bit(HpeCompound *out, const char *ident)
{
    if (hpe_str_ieq(ident, "first-child"))
        out->pseudo |= HPE_PS_FIRST_CHILD;
    else if (hpe_str_ieq(ident, "last-child"))
        out->pseudo |= HPE_PS_LAST_CHILD;
    else if (hpe_str_ieq(ident, "only-child"))
        out->pseudo |= HPE_PS_ONLY_CHILD;
    else if (hpe_str_ieq(ident, "first-of-type"))
        out->pseudo |= HPE_PS_FIRST_OF_TYPE;
    else if (hpe_str_ieq(ident, "last-of-type"))
        out->pseudo |= HPE_PS_LAST_OF_TYPE;
    else if (hpe_str_ieq(ident, "only-of-type"))
        out->pseudo |= HPE_PS_ONLY_OF_TYPE;
    else if (hpe_str_ieq(ident, "empty"))
        out->pseudo |= HPE_PS_EMPTY;
    else if (hpe_str_ieq(ident, "checked"))
        out->pseudo |= HPE_PS_CHECKED;
    else if (hpe_str_ieq(ident, "disabled"))
        out->pseudo |= HPE_PS_DISABLED;
    else if (hpe_str_ieq(ident, "enabled"))
        out->pseudo |= HPE_PS_ENABLED;
    else
        return 0;
    return 1;
}

/* The `:nth-*()` family: pick the target slot and parse the `an+b` argument.
 * Returns 1 on success, 0 when the argument is malformed. */
static int pseudo_nth_arg(HpeCompound *out, const char **pp, const char *end, const char *ident)
{
    HpeNth *dst = &out->nth_last_of_type;
    if (hpe_str_ieq(ident, "nth-child"))
        dst = &out->nth_child;
    else if (hpe_str_ieq(ident, "nth-last-child"))
        dst = &out->nth_last_child;
    else if (hpe_str_ieq(ident, "nth-of-type"))
        dst = &out->nth_of_type;
    return parse_nth_arg(pp, end, ident, dst);
}

static int parse_pseudo(HpeCompound *out, const char **pp, const char *end, char *ident,
                        size_t ident_size)
{
    const char *p = *pp + 1; /* skip ':' */

    if (p < end && *p == ':')
        p++; /* `::before` — the double-colon form of a pseudo-element */
    if (!read_ident(&p, end, ident, ident_size))
        return 0;
    if (hpe_str_ieq(ident, "before") || hpe_str_ieq(ident, "after")) {
        out->pseudo_element = hpe_str_ieq(ident, "before") ? HPE_PE_BEFORE : HPE_PE_AFTER;
    } else if (hpe_str_ieq(ident, "root")) {
        if (!out->tag)
            out->tag = hpe_strdup("html");
    } else if (pseudo_state_bit(out, ident) || pseudo_structural_bit(out, ident)) {
        /* state / structural bits already set */
    } else if (hpe_str_ieq(ident, "nth-child") || hpe_str_ieq(ident, "nth-last-child") ||
               hpe_str_ieq(ident, "nth-of-type") || hpe_str_ieq(ident, "nth-last-of-type")) {
        if (!pseudo_nth_arg(out, &p, end, ident))
            return 0;
    } else if (hpe_str_ieq(ident, "not") || hpe_str_ieq(ident, "is") ||
               hpe_str_ieq(ident, "where")) {
        if (!parse_sel_arg(out, &p, end, ident))
            return 0;
    }
    /* unknown pseudo-classes are ignored on purpose so the selector still parses */
    *pp = p;
    return 1;
}

/**
 * @brief 把 `[...]` 内容拆为属性名/运算符/值 / Split an `[...]` body into name, operator and value
 *
 * @details 属性名读到空格或 `=` 为止；遇到 `=` 时记录运算符（`~`/`
 *          `/`^`/`$`/`*` 后接连写 `=`），并读取可选的、可带引号的值；无 `=` 但以运算符结尾的畸形输入也被容忍。
 *          The name is read up to a space or `=`, the operator is recorded when `=` is present (with `~`/`
 *          `/`^`/`$`/`*` prefixed), and the optional possibly-quoted value is read; malformed input that trails an operator without `=` is also tolerated.
 *
 * @param[in]     body `[...]` 括号内的文本 / Text inside the `[...]` brackets
 * @param[out]    aname 属性名 / Attribute name
 * @param[out]    aop 属性匹配运算符 / Attribute match operator
 * @param[out]    avalue 属性值 / Attribute value
 * @param[in]     avalue_size `avalue` 的容量 / Capacity of `avalue`
 * @param[out]    has_val 是否带值 / Whether a value is present
 *
 * @note 沿用引擎既有的宽松（非规范）分词方式以保持行为不变 / It mirrors the engine's lenient, non-spec tokenisation so behaviour is unchanged
 *
 * @see parse_attr_selector compound_add_attr
 */
static void dissect_attr_expr(const char *body, char *aname, char *aop, char *avalue,
                              size_t avalue_size, int *has_val)
{
    char *tmp = hpe_strdup(body);
    char *sp = tmp;
    char *namep = aname;

    aname[0] = '\0';
    aop[0] = '\0';
    avalue[0] = '\0';
    *has_val = 0;

    while (*sp && *sp != ' ' && *sp != '=')
        *namep++ = *sp++;
    *namep = '\0';
    while (*sp == ' ')
        sp++;
    if (*sp == '=') {
        const char *v;
        size_t vl;
        aop[0] = '=';
        aop[1] = '\0';
        sp++;
        while (*sp == ' ')
            sp++;
        if (*sp == '~' || *sp == '|' || *sp == '^' || *sp == '$' || *sp == '*') {
            aop[0] = *sp++;
            aop[1] = '=';
            aop[2] = '\0';
        }
        while (*sp == ' ')
            sp++;
        v = sp;
        vl = strlen(v);
        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) {
            v++;
            vl -= 2;
        }
        if (vl >= avalue_size)
            vl = avalue_size - 1;
        memcpy(avalue, v, vl);
        avalue[vl] = '\0';
        *has_val = 1;
    } else if (*sp == '~' || *sp == '|' || *sp == '^' || *sp == '$' || *sp == '*') {
        /* malformed but tolerate */
        aop[0] = *sp;
        aop[1] = '=';
        aop[2] = '\0';
        *has_val = 1;
    }
    hpe_free(tmp);
}

/**
 * @brief 给复合选择器追加一个属性选择器 / Append an attribute selector to a compound selector
 *
 * @details 先把属性名就地转为小写，再扩容 `attrs` 数组并存入名字、运算符与值的副本；无值时值为空串。
 *          The attribute name is lowercased in place, the `attrs` array is grown and copies of the name, operator and value are stored, with an empty value when none was given.
 *
 * @param[in,out] out 目标复合选择器 / Target compound selector
 * @param[in]     aname 属性名（会被就地转小写）/ Attribute name, lowercased in place
 * @param[in]     aop 属性匹配运算符 / Attribute match operator
 * @param[in]     avalue 属性值 / Attribute value
 * @param[in]     has_val 是否带值 / Whether a value is present
 *
 * @see dissect_attr_expr compound_free
 */
static void compound_add_attr(HpeCompound *out, const char *aname, const char *aop,
                              const char *avalue, int has_val)
{
    HpeAttrSel *as;
    int j;

    for (j = 0; aname[j]; j++)
        ((char *)aname)[j] = (char)hpe_ascii_lower((unsigned char)aname[j]);
    out->attrs = (HpeAttrSel *)hpe_grow(out->attrs, &out->attr_cap,
                                        (size_t)out->attr_count + 1, sizeof(HpeAttrSel));
    as = &out->attrs[out->attr_count++];
    as->name = hpe_strdup(aname);
    as->op = hpe_strdup(aop);
    as->value = hpe_strdup(has_val ? avalue : "");
}

/**
 * @brief 解析一个 `[attr]` / `[attr=v]` / `[attr~=v]` 选择器 / Parse a `[attr]`, `[attr=v]` or `[attr~=v]` selector
 *
 * @details 先找到配对的 `]`，缺失则视为格式错误；把括号内文本交给 `dissect_attr_expr` 拆分后追加到复合选择器，游标移到 `]` 之后。
 *          The matching `]` is located and its absence is a malformed selector; the bracketed text is split by `dissect_attr_expr` and appended to the compound, and the cursor moves past `]`.
 *
 * @param[in,out] out 目标复合选择器 / Target compound selector
 * @param[in,out] pp 解析游标指针 / Parse cursor pointer
 * @param[in]     end 输入缓冲区末尾 / End of the input buffer
 *
 * @return 成功返回 1，括号不配返回 0 / 1 on success, 0 when the brackets are unbalanced
 *
 * @see dissect_attr_expr compound_add_attr
 */
static int parse_attr_selector(HpeCompound *out, const char **pp, const char *end)
{
    const char *a = *pp + 1;
    const char *close = a;
    char aname[128], aop[8], avalue[256];
    int has_val = 0;

    while (close < end && *close != ']')
        close++;
    if (close >= end)
        return 0; /* unbalanced */
    {
        char *body = hpe_strndup(a, (size_t)(close - a));
        dissect_attr_expr(body, aname, aop, avalue, sizeof(avalue), &has_val);
        hpe_free(body);
    }
    compound_add_attr(out, aname, aop, avalue, has_val);
    *pp = close + 1;
    return 1;
}

/**
 * @brief 解析一个复合选择器（标签 + 若干简单选择器）/ Parse one compound selector (a tag plus simple selectors)
 *
 * @details 先处理可选的 `*` 或标签名（标签转小写存入），随后循环读取 `.class`/`#id`/`:pseudo`/`[attr]`，遇到其它字符即停止；任一简单选择器解析失败则整体失败并清零结果。
 *          The optional `*` or a tag name is handled first (the tag lowercased), after which `.class`/`#id`/`:pseudo`/`[attr]` are read in a loop until any other character stops it; a failing simple selector fails the whole compound and leaves the result zeroed.
 *
 * @param[in,out] pp 解析游标指针 / Parse cursor pointer
 * @param[in]     end 输入缓冲区末尾 / End of the input buffer
 * @param[out]    out 接收解析出的复合选择器 / Receives the parsed compound selector
 *
 * @return 成功返回 1，失败返回 0 / 1 on success, 0 on failure
 *
 * @see parse_class_or_id parse_pseudo parse_attr_selector
 */
static int parse_compound(const char **pp, const char *end, HpeCompound *out)
{
    const char *p = *pp;
    char ident[128];

    memset(out, 0, sizeof(*out));

    if (p < end && *p == '*') {
        p++;
    } else if (p < end && is_ident_char(*p) && read_ident(&p, end, ident, sizeof(ident))) {
        size_t i;
        for (i = 0; ident[i]; i++)
            ident[i] = (char)hpe_ascii_lower((unsigned char)ident[i]);
        out->tag = hpe_strdup(ident);
    }

    while (p < end) {
        if (*p == '.' || *p == '#') {
            if (!parse_class_or_id(out, &p, end, ident, sizeof(ident)))
                return 0;
        } else if (*p == ':') {
            if (!parse_pseudo(out, &p, end, ident, sizeof(ident)))
                return 0;
        } else if (*p == '[') {
            if (!parse_attr_selector(out, &p, end))
                return 0;
        } else {
            break;
        }
    }
    *pp = p;
    return 1;
}

/**
 * @brief 计算一个复合选择器贡献的特异性 / Specificity contributed by one compound selector
 *
 * @details id 记 100、每个类/属性选择器记 10、伪类记 10、伪元素记 1、标签记 1；`:where()` 的嵌套参数贡献 0，`:is()`/`:not()` 取参数中最高者。
 *          An id counts 100, each class/attribute selector 10, each pseudo-class 10, a pseudo-element 1 and a tag 1; `:where()` contributes 0 and `:is()`/`:not()` take the highest value found among their arguments.
 *
 * @param[in]     part 复合选择器 / Compound selector
 *
 * @return 该部分贡献的特异性 / The specificity this part contributes
 *
 * @see selector_add_part sel_inner_specificity
 */
static int compound_specificity(const HpeCompound *part)
{
    int spec = 0;
    int i, j;
    if (part->id)
        spec += 100;
    spec += part->class_count * 10;
    spec += part->attr_count * 10;
    /* one pseudo-class bit pair per declaration; count bits */
    {
        unsigned bits = part->pseudo;
        while (bits) {
            if (bits & 1u)
                spec += 10;
            bits >>= 1;
        }
    }
    if (part->nth_child.present || part->nth_last_child.present ||
        part->nth_of_type.present || part->nth_last_of_type.present)
        spec += 10;
    if (!part->is_where) {
        for (i = 0; i < part->is_count; i++) {
            int best = 0;
            for (j = 0; j < part->is_list[i].count; j++)
                best = HPE_MAX(best, part->is_list[i].specificity);
            spec += best;
        }
        for (i = 0; i < part->not_count; i++) {
            int best = 0;
            for (j = 0; j < part->not_list[i].count; j++)
                best = HPE_MAX(best, part->not_list[i].specificity);
            spec += best;
        }
    }
    if (part->pseudo_element != HPE_PE_NONE)
        spec += 1;
    if (part->tag)
        spec += 1;
    return spec;
}

/**
 * @brief 给选择器追加一个复合部分并累加特异性 / Append one compound part to a selector and accumulate its specificity
 *
 * @details 扩容 `parts` 并以值拷贝存入该部分，然后按 `compound_specificity` 累加特异性（id 100、类/属性/伪类 10、伪元素/标签 1，`:where()` 记 0）。
 *          The `parts` array is grown and the compound copied in by value, then specificity accumulates through `compound_specificity` (id 100, class/attribute/pseudo-class 10, pseudo-element/tag 1, `:where()` zero).
 *
 * @param[in,out] sel 目标选择器 / Target selector
 * @param[in]     part 待追加的复合选择器 / Compound selector to append
 *
 * @return 恒返回 1 / Always returns 1
 *
 * @see parse_one_selector compound_specificity
 */
static int selector_add_part(HpeSelector *sel, const HpeCompound *part)
{
    sel->parts = (HpeCompound *)hpe_grow(sel->parts, &sel->cap, (size_t)sel->count + 1,
                                         sizeof(HpeCompound));
    sel->parts[sel->count++] = *part;
    sel->specificity += compound_specificity(part);
    return 1;
}


/**
 * @brief 释放选择器持有的全部内存 / Release every allocation held by a selector
 *
 * @details 逐部分调用 `compound_free`，再释放 `parts` 数组并把结构清零。
 *          Each part is released through `compound_free`, then the `parts` array is freed and the struct zeroed.
 *
 * @param[in,out] sel 目标选择器 / Target selector
 *
 * @see compound_free rule_free
 */
static void selector_free(HpeSelector *sel)
{
    int i;
    for (i = 0; i < sel->count; i++)
        compound_free(&sel->parts[i]);
    if (sel->parts)
        hpe_free(sel->parts);
    memset(sel, 0, sizeof(*sel));
}

/**
 * @brief 释放规则持有的选择器与声明 / Release the selectors and declarations held by a rule
 *
 * @details 逐条释放选择器，再释放选择器数组与声明数组，最后把规则清零。
 *          Every selector is released, then the selector array and the declaration array are freed and the rule zeroed.
 *
 * @param[in,out] rule 目标 CSS 规则 / Target CSS rule
 *
 * @see selector_free
 */
static void rule_free(HpeRule *rule)
{
    int i;
    for (i = 0; i < rule->selector_count; i++)
        selector_free(&rule->selectors[i]);
    if (rule->selectors)
        hpe_free(rule->selectors);
    if (rule->decls)
        hpe_free(rule->decls);
    memset(rule, 0, sizeof(*rule));
}

/**
 * @brief 读取组合符 token，缺省为后代组合符 / Read a combinator token, defaulting to the descendant combinator
 *
 * @details 识别 `>`/`+`/`~` 分别为子代/相邻兄弟/一般兄弟，其后跳过空白并把游标前移；无组合符时返回后代。
 *          `>`/`+`/`~` map to child, adjacent sibling and general sibling respectively, whitespace is then skipped and the cursor advanced; without a combinator the descendant combinator is returned.
 *
 * @param[in,out] pq 解析游标指针 / Parse cursor pointer
 * @param[in]     end 输入缓冲区末尾 / End of the input buffer
 *
 * @return 读到的 `HpeCombinator` 枚举 / The `HpeCombinator` value that was read
 *
 * @see parse_one_selector
 */
static int read_combinator(const char **pq, const char *end)
{
    const char *q = *pq;
    int comb = HPE_COMB_DESCENDANT;

    if (q < end && *q == '>') {
        comb = HPE_COMB_CHILD;
        q++;
    } else if (q < end && *q == '+') {
        comb = HPE_COMB_ADJ_SIBLING;
        q++;
    } else if (q < end && *q == '~') {
        comb = HPE_COMB_GEN_SIBLING;
        q++;
    }
    while (q < end && hpe_is_space(*q))
        q++;
    *pq = q;
    return comb;
}

/**
 * @brief 解析一个以逗号分隔的单个选择器（复合链）/ Parse one comma-separated selector (a chain of compounds)
 *
 * @details 逐个读取复合部分并记录其相对前一部分的组合符，首部分的组合符固定为后代；某部分解析失败时把 `count` 置 -1 以标记整个选择器无效。
 *          Compound parts are read one by one with each recording its combinator relative to the previous part, the first part always using the descendant combinator; a failing part sets `count` to -1 to mark the whole selector invalid.
 *
 * @param[in]     part_start 选择器文本起始指针 / Start pointer of the selector text
 * @param[in]     part_end 选择器文本结束指针 / End pointer of the selector text
 * @param[out]    sel 接收解析出的选择器 / Receives the parsed selector
 *
 * @note 进入时先清零 `sel` / `sel` is zeroed on entry
 *
 * @see read_combinator parse_compound selector_add_part
 */
static void parse_one_selector(const char *part_start, const char *part_end, HpeSelector *sel)
{
    const char *q = part_start;
    int first = 1;

    memset(sel, 0, sizeof(*sel));
    while (q < part_end) {
        int comb = HPE_COMB_DESCENDANT;
        HpeCompound comp;

        while (q < part_end && hpe_is_space(*q))
            q++;
        if (!first) {
            /* leading whitespace was already skipped above, so any token that is
             * not an explicit combinator means the parts were space-separated:
             * descendant */
            comb = read_combinator(&q, part_end);
        }
        if (q >= part_end)
            break;
        if (!parse_compound(&q, part_end, &comp)) {
            compound_free(&comp);
            sel->count = -1;
            break;
        }
        comp.comb = first ? HPE_COMB_DESCENDANT : comb;
        selector_add_part(sel, &comp);
        first = 0;
    }
}

/**
 * @brief 把 `a b, c > d` 形式的文本解析为选择器列表 / Parse text such as `a b, c > d` into a selector list
 *
 * @details 按逗号切分为各选择器，逐个解析后仅把 `count > 0` 的有效选择器追加到规则；空或无效的选择器被释放丢弃。
 *          Segments are split on commas and parsed individually, and only valid selectors with `count > 0` are appended to the rule; empty or invalid selectors are freed and dropped.
 *
 * @param[in,out] rule 接收选择器的 CSS 规则 / CSS rule that receives the selectors
 * @param[in]     text 选择器列表文本 / Selector list text
 * @param[in]     length `text` 的字节长度 / Byte length of `text`
 *
 * @see parse_one_selector selector_free
 */
static void parse_selector_list(HpeRule *rule, const char *text, size_t length)
{
    const char *p = text;
    const char *end = text + length;

    while (p < end) {
        const char *part_start = p;
        const char *part_end;
        HpeSelector sel;
        int depth = 0;

        /* a comma only separates selectors at parenthesis depth 0, so the
         * argument lists of :not()/:is()/:where() stay intact */
        while (p < end) {
            if (*p == '(')
                depth++;
            else if (*p == ')')
                depth--;
            else if (*p == ',' && depth == 0)
                break;
            p++;
        }
        part_end = p;
        if (p < end)
            p++;

        parse_one_selector(part_start, part_end, &sel);
        if (sel.count > 0) {
            rule->selectors = (HpeSelector *)hpe_grow(rule->selectors, &rule->selector_cap,
                                                      (size_t)rule->selector_count + 1,
                                                      sizeof(HpeSelector));
            rule->selectors[rule->selector_count++] = sel;
        } else {
            selector_free(&sel);
        }
    }
}

/**
 * @brief 判断 `class` 属性值是否含某个类名 / Test whether a `class` attribute value contains a given class name
 *
 * @details 把 `list` 按空白切成单词，逐个与目标名做等长比较，命中返回 1；`list` 为 `NULL` 返回 0。
 *          The `list` is split into whitespace-separated words compared for equal length against the name and 1 is returned on a hit; a `NULL` list returns 0.
 *
 * @param[in]     list 空的 `class` 属性值 / The `class` attribute value
 * @param[in]     name 要查找的类名 / Class name to look for
 *
 * @return 包含返回 1，否则返回 0 / 1 when present, 0 otherwise
 *
 * @see compound_matches
 */
static int class_list_contains(const char *list, const char *name)
{
    const char *p = list;
    size_t n = strlen(name);
    if (!list)
        return 0;
    while (*p) {
        const char *start;
        while (*p && hpe_is_space(*p))
            p++;
        start = p;
        while (*p && !hpe_is_space(*p))
            p++;
        if ((size_t)(p - start) == n && !strncmp(start, name, n))
            return 1;
    }
    return 0;
}

/**
 * @brief 判断 `val` 是否作为空白分隔的整词出现在 `list` 中 / Test whether `val` appears as a whitespace-separated word inside `list`
 *
 * @details 用于 `~=` 运算符：按空白分词后做等长比较；`list` 为 `NULL` 或 `val` 为空返回 0。
 *          Used by the `~=` operator: the list is split on whitespace and compared for equal length; a `NULL` list or an empty `val` returns 0.
 *
 * @param[in]     list 被搜索的文本 / Text to search
 * @param[in]     val 要查找的整词 / Word to look for
 *
 * @return 命中返回 1，否则返回 0 / 1 on a hit, 0 otherwise
 *
 * @see attr_op_matches
 */
static int attr_word_contains(const char *list, const char *val)
{
    size_t vlen = strlen(val);
    const char *p = list;
    if (!list || vlen == 0)
        return 0;
    while (*p) {
        const char *start;
        while (*p && hpe_is_space(*p))
            p++;
        start = p;
        while (*p && !hpe_is_space(*p))
            p++;
        if ((size_t)(p - start) == vlen && !strncmp(start, val, vlen))
            return 1;
    }
    return 0;
}

/**
 * @brief 按运算符把属性选择器与节点属性值匹配 / Match an attribute selector against a node's raw attribute value
 *
 * @details 空运算符表示仅测试存在性（`attr_val` 非 `NULL` 即匹配）；`=` 全等、`~=` 整词、`
 *          =` 前缀且其后为 `-` 或结束、`^=` 前缀、`$=` 后缀、`*=` 子串；任何无法识别的运算符都按匹配处理，避免将来新增形式时整条规则被静默丢弃。
 *          An empty operator is a bare presence test (`attr_val` non-`NULL` matches); `=` is equality, `~=` a whole word, `
 *          =` a prefix followed by `-` or end, `^=` a prefix, `$=` a suffix and `*=` a substring; any unrecognised operator is treated as a match so a future selector form degrades gracefully rather than silently dropping the rule.
 *
 * @param[in]     as 属性选择器 / Attribute selector
 * @param[in]     attr_val 节点的属性值，属性不存在时为 `NULL` / The node's attribute value, `NULL` when the attribute is absent
 *
 * @return 匹配返回 1，否则返回 0 / 1 on a match, 0 otherwise
 *
 * @see attr_word_contains compound_matches
 */
static int attr_op_matches(const HpeAttrSel *as, const char *attr_val)
{
    if (as->op[0] == '\0')
        return attr_val != NULL;
    if (!attr_val)
        return 0;
    if (as->op[0] == '=' && as->op[1] == '\0')
        return strcmp(attr_val, as->value) == 0;
    if (as->op[0] == '~')
        return attr_word_contains(attr_val, as->value);
    if (as->op[0] == '|') {
        size_t vl = strlen(as->value);
        if (strncmp(attr_val, as->value, vl) != 0)
            return 0;
        return attr_val[vl] == '\0' || attr_val[vl] == '-';
    }
    if (as->op[0] == '^')
        return strncmp(attr_val, as->value, strlen(as->value)) == 0;
    if (as->op[0] == '$') {
        size_t al = strlen(attr_val), vl = strlen(as->value);
        return al >= vl && strcmp(attr_val + al - vl, as->value) == 0;
    }
    if (as->op[0] == '*')
        return strstr(attr_val, as->value) != NULL;
    return 1;
}

/**
 * @brief 判断 `an+b` 公式是否命中某个 1 基下标 / Test whether an `an+b` formula hits a 1-based index
 *
 * @details `a == 0` 时只比较 `b`；`a > 0` 时命中条件为 `i >= b` 且 `(i - b) % a == 0`；`a < 0` 时命中条件为 `i <= b` 且 `(b - i) % (-a) == 0`。未启用的公式（`present == 0`）恒返回 1。
 *          With `a == 0` only `b` is compared; with `a > 0` the index hits when `i >= b` and `(i - b) % a == 0`; with `a < 0` it hits when `i <= b` and `(b - i) % (-a) == 0`. A disabled formula (`present == 0`) always returns 1.
 *
 * @param[in]     nth 公式 / The formula
 * @param[in]     i 1 基下标 / 1-based index
 *
 * @return 命中返回 1，否则 0 / 1 on a hit, 0 otherwise
 *
 * @see compound_matches
 */
static int nth_matches(const HpeNth *nth, int i)
{
    int a = nth->a, b = nth->b;
    if (!nth->present)
        return 1;
    if (a == 0)
        return i == b;
    if (a > 0) {
        if (i < b)
            return 0;
        return ((i - b) % a) == 0;
    }
    if (i > b)
        return 0;
    return ((b - i) % (-a)) == 0;
}

/**
 * @brief 统计节点在元素兄弟中的 1 基位置（同类型或全部）/ 1-based position of a node among its element siblings
 *
 * @details `of_type` 非 0 时只统计同名标签的兄弟；无视文本节点等非元素节点。数据来自 DOM 兄弟链，无需额外缓存。
 *          With a non-zero `of_type` only siblings sharing the tag name are counted; non-element nodes such as text are ignored. The DOM sibling chain is walked directly, so no extra cache is needed.
 *
 * @param[in]     node 目标元素 / Target element
 * @param[in]     of_type 是否只统计同类型元素 / Whether to only count same-type elements
 * @param[out]    total 接收兄弟总数，可为 `NULL` / Receives the total number of siblings, may be `NULL`
 *
 * @return 该节点的 1 基下标 / The 1-based index of the node
 *
 * @see nth_matches compound_matches
 */
static int elem_index_of(const HpeDomNode *node, int of_type, int *total)
{
    const HpeDomNode *p;
    int idx = 1;
    int n = 1;
    if (total)
        *total = 1;
    if (!node || !node->parent)
        return 1;
    for (p = node->parent->first_child; p; p = p->next_sibling) {
        if (p == node)
            break;
        if (p->type == HPE_NODE_ELEMENT && (!of_type || (p->tag && node->tag && !strcmp(p->tag, node->tag))))
            idx++;
    }
    for (p = node->next_sibling; p; p = p->next_sibling) {
        if (p->type == HPE_NODE_ELEMENT && (!of_type || (p->tag && node->tag && !strcmp(p->tag, node->tag))))
            n++;
    }
    if (total)
        *total = idx + n - 1;
    return idx;
}

/**
 * @brief 判断元素是否为空（无元素或非空文本子节点）/ Test whether an element is empty
 *
 * @details 依 CSS 定义，只有元素子节点与含非空白的文本子节点会使其非空；注释与空白文本不算。
 *          Per CSS an element is non-empty only when it has an element child or a text child containing non-whitespace; comments and whitespace-only text do not count.
 *
 * @param[in]     node 目标元素 / Target element
 *
 * @return 空元素返回 1 / 1 for an empty element
 *
 * @see compound_matches
 */
static int element_is_empty(const HpeDomNode *node)
{
    const HpeDomNode *p;
    for (p = node->first_child; p; p = p->next_sibling) {
        if (p->type == HPE_NODE_ELEMENT)
            return 0;
        if (p->type == HPE_NODE_TEXT && p->text && p->text[0]) {
            const char *s = p->text;
            while (*s) {
                if (!hpe_is_space(*s))
                    return 0;
                s++;
            }
        }
    }
    return 1;
}

/**
 * @brief 判断复合选择器是否匹配一个元素节点 / Test whether a compound selector matches an element node
 *
 * @details 元素类型不符直接不匹配；依次校验标签、状态伪类、结构伪类（`an+b` 与 `:first-child` 一族）、id、全部类名与全部属性选择器，最后校验 `:not()`/`:is()` 参数，全部通过才匹配。
 *          A non-element node fails immediately; the tag, state pseudo-classes, structural pseudo-classes (`an+b` and the `:first-child` family), id, every class and every attribute selector are checked in turn, then the `:not()`/`:is()` arguments, and all must pass.
 *
 * @param[in]     c 复合选择器 / Compound selector
 * @param[in]     node 待测试的 DOM 节点 / DOM node to test
 *
 * @return 匹配返回 1，否则返回 0 / 1 on a match, 0 otherwise
 *
 * @see attr_op_matches class_list_contains nth_matches hpe_css_sel_matches_node
 */
/* State pseudo-classes driven by the node's interaction flags and attributes. */
static int compound_state_matches(const HpeCompound *c, const HpeDomNode *node)
{
    if ((c->pseudo & HPE_PS_HOVER) && !node->hovered)
        return 0;
    if ((c->pseudo & HPE_PS_FOCUS) && !node->focused)
        return 0;
    if ((c->pseudo & HPE_PS_ACTIVE) && !node->active)
        return 0;
    if ((c->pseudo & HPE_PS_VISITED) && !node->visited)
        return 0;
    if ((c->pseudo & HPE_PS_LINK)) {
        const char *href = hpe_dom_attr(node, "href");
        if (!href || (strcmp(node->tag, "a") != 0 && strcmp(node->tag, "area") != 0))
            return 0;
    }
    if ((c->pseudo & HPE_PS_TARGET) && !node->targeted)
        return 0;
    if ((c->pseudo & HPE_PS_CHECKED)) {
        const char *chk = hpe_dom_attr(node, "checked");
        const char *sel = hpe_dom_attr(node, "selected");
        if (!chk && !sel)
            return 0;
    }
    if ((c->pseudo & HPE_PS_DISABLED) && !hpe_dom_attr(node, "disabled"))
        return 0;
    if (c->pseudo & HPE_PS_ENABLED) {
        const char *t = node->tag;
        int form = t && (!strcmp(t, "input") || !strcmp(t, "button") || !strcmp(t, "select") ||
                         !strcmp(t, "textarea") || !strcmp(t, "option"));
        if (!form || hpe_dom_attr(node, "disabled"))
            return 0;
    }
    return 1;
}

/* Positional pseudo-classes: the `:first-child` family, `:empty`, and `an+b`. */
static int compound_positional_matches(const HpeCompound *c, const HpeDomNode *node)
{
    if (c->pseudo & (HPE_PS_FIRST_CHILD | HPE_PS_LAST_CHILD | HPE_PS_ONLY_CHILD |
                     HPE_PS_FIRST_OF_TYPE | HPE_PS_LAST_OF_TYPE | HPE_PS_ONLY_OF_TYPE)) {
        int total = 1;
        int idx = elem_index_of(node, (c->pseudo & (HPE_PS_FIRST_OF_TYPE | HPE_PS_LAST_OF_TYPE |
                                                     HPE_PS_ONLY_OF_TYPE)) != 0,
                                &total);
        if ((c->pseudo & HPE_PS_FIRST_CHILD) && idx != 1)
            return 0;
        if ((c->pseudo & HPE_PS_LAST_CHILD) && idx != total)
            return 0;
        if ((c->pseudo & HPE_PS_ONLY_CHILD) && total != 1)
            return 0;
        if ((c->pseudo & HPE_PS_FIRST_OF_TYPE) && idx != 1)
            return 0;
        if ((c->pseudo & HPE_PS_LAST_OF_TYPE) && idx != total)
            return 0;
        if ((c->pseudo & HPE_PS_ONLY_OF_TYPE) && total != 1)
            return 0;
    }
    if ((c->pseudo & HPE_PS_EMPTY) && !element_is_empty(node))
        return 0;
    if (c->nth_child.present || c->nth_last_child.present || c->nth_of_type.present ||
        c->nth_last_of_type.present) {
        int total = 1;
        int idx = elem_index_of(node, c->nth_of_type.present || c->nth_last_of_type.present, &total);
        if (!nth_matches(&c->nth_child, idx))
            return 0;
        if (!nth_matches(&c->nth_last_child, total - idx + 1))
            return 0;
        if (!nth_matches(&c->nth_of_type, idx))
            return 0;
        if (!nth_matches(&c->nth_last_of_type, total - idx + 1))
            return 0;
    }
    return 1;
}

/* The id, class list and attribute selectors. */
static int compound_ident_matches(const HpeCompound *c, const HpeDomNode *node)
{
    int i;
    if (c->id) {
        const char *id = hpe_dom_attr(node, "id");
        if (!id || strcmp(id, c->id) != 0)
            return 0;
    }
    if (c->class_count > 0) {
        const char *cls = hpe_dom_attr(node, "class");
        if (!cls)
            return 0;
        for (i = 0; i < c->class_count; i++) {
            if (!class_list_contains(cls, c->classes[i]))
                return 0;
        }
    }
    for (i = 0; i < c->attr_count; i++) {
        const HpeAttrSel *as = &c->attrs[i];
        if (!attr_op_matches(as, hpe_dom_attr(node, as->name)))
            return 0;
    }
    return 1;
}

/* `:not(...)` fails on any hit; `:is(...)` needs at least one. */
static int compound_logical_matches(const HpeCompound *c, const HpeDomNode *node)
{
    int i;
    for (i = 0; i < c->not_count; i++) {
        int j;
        for (j = 0; j < c->not_list[i].count; j++) {
            if (selector_matches(&c->not_list[i], node))
                return 0; /* :not(...) — any hit fails */
        }
    }
    if (c->is_count > 0) {
        int any = 0;
        for (i = 0; i < c->is_count && !any; i++) {
            int j;
            for (j = 0; j < c->is_list[i].count && !any; j++)
                any = selector_matches(&c->is_list[i], node);
        }
        if (!any)
            return 0;
    }
    return 1;
}

static int compound_matches(const HpeCompound *c, const HpeDomNode *node)
{
    if (!node || node->type != HPE_NODE_ELEMENT)
        return 0;
    if (c->tag && strcmp(c->tag, node->tag) != 0)
        return 0;
    if (!compound_state_matches(c, node))
        return 0;
    if (!compound_positional_matches(c, node))
        return 0;
    if (!compound_ident_matches(c, node))
        return 0;
    return compound_logical_matches(c, node);
}


/**
 * @brief 测试单个选择器是否匹配给定节点 / Test whether a standalone selector matches a node
 *
 * @details 把 `sel` 解析成临时规则，依次测试其中每个选择器，一旦命中即用 `hit` 提前跳出循环并返回。
 *          `sel` is parsed into a temporary rule and its selectors are tested in turn; `hit` breaks the loop on the first match.
 *
 * @param[in]     node 待测试的 DOM 节点 / DOM node to test
 * @param[in]     sel 选择器文本（可含逗号）/ Selector text, possibly comma-separated
 * @param[in]     len `sel` 的字节长度 / Byte length of `sel`
 *
 * @return 匹配返回 1，否则返回 0 / 1 on a match, 0 otherwise
 *
 * @note 供 `querySelector` 一类接口使用 / Used by interfaces such as `querySelector`
 *
 * @see parse_selector_list selector_matches rule_free
 */
int hpe_css_selector_matches(const HpeDomNode *node, const char *sel, size_t len)
{
    HpeRule rule;
    int i, hit = 0;

    if (!node || !sel || len == 0)
        return 0;
    memset(&rule, 0, sizeof(rule));
    parse_selector_list(&rule, sel, len);
    for (i = 0; i < rule.selector_count && !hit; i++)
        hit = selector_matches(&rule.selectors[i], node);
    rule_free(&rule);
    return hit;
}

/**
 * @brief 从句尾（主体）向左逐部分匹配选择器 / Match a selector right-to-left from its subject part
 *
 * @details 先要求最后一个部分匹配当前节点，再依次向左处理组合符：子代回退到直接父节点、相邻兄弟回退到前一个兄弟、一般兄弟向前搜索任意匹配的兄弟、后代则沿父链向上找首个匹配的祖先，中途失败即返回 0。
 *          The last part must match the current node, after which combinators are walked leftwards: a child moves to the direct parent, an adjacent sibling to the previous sibling, a general sibling searches earlier siblings for any match, and a descendant climbs the parent chain to the first matching ancestor; any failure returns 0.
 *
 * @param[in]     sel 选择器 / Selector
 * @param[in]     node 待测试的 DOM 节点 / DOM node to test
 *
 * @return 匹配返回 1，否则返回 0 / 1 on a match, 0 otherwise
 *
 * @see compound_matches hpe_css_selector_matches
 */
static int selector_matches(const HpeSelector *sel, const HpeDomNode *node)
{
    int part;
    const HpeDomNode *cur = node;

    if (sel->count <= 0)
        return 0;
    part = sel->count - 1;
    if (!compound_matches(&sel->parts[part], cur))
        return 0;
    part--;
    while (part >= 0) {
        const HpeCompound *c = &sel->parts[part];
        if (c->comb == HPE_COMB_CHILD) {
            cur = cur->parent;
            if (!cur || !compound_matches(c, cur))
                return 0;
        } else if (c->comb == HPE_COMB_ADJ_SIBLING) {
            cur = hpe_dom_prev_sibling(cur);
            if (!cur || !compound_matches(c, cur))
                return 0;
        } else if (c->comb == HPE_COMB_GEN_SIBLING) {
            const HpeDomNode *s = hpe_dom_prev_sibling(cur);
            int ok = 0;
            while (s) {
                if (compound_matches(c, s)) {
                    ok = 1;
                    break;
                }
                s = hpe_dom_prev_sibling(s);
            }
            if (!ok)
                return 0;
            cur = s;
        } else { /* descendant */
            cur = cur->parent;
            while (cur && !compound_matches(c, cur))
                cur = cur->parent;
            if (!cur)
                return 0;
        }
        part--;
    }
    return 1;
}

/* ---------------- media queries ---------------- */

/**
 * @brief 把媒体特性名映射为 `HPE_MF_*` 编号 / Map a media feature name to its `HPE_MF_*` id
 *
 * @details 识别 min-width/max-width/min-height/max-height/width/height，未知名称返回 0。
 *          It recognises min-width/max-width/min-height/max-height/width/height and returns 0 for an unknown name.
 *
 * @param[in]     name 媒体特性名 / Media feature name
 *
 * @return 对应的特性编号，未识别返回 0 / The feature id, or 0 when unrecognised
 *
 * @see media_parse_condition media_alt_matches
 */
static int media_feature_id(const char *name)
{
    if (hpe_str_ieq(name, "min-width"))
        return HPE_MF_MIN_WIDTH;
    if (hpe_str_ieq(name, "max-width"))
        return HPE_MF_MAX_WIDTH;
    if (hpe_str_ieq(name, "min-height"))
        return HPE_MF_MIN_HEIGHT;
    if (hpe_str_ieq(name, "max-height"))
        return HPE_MF_MAX_HEIGHT;
    if (hpe_str_ieq(name, "width"))
        return HPE_MF_WIDTH;
    if (hpe_str_ieq(name, "height"))
        return HPE_MF_HEIGHT;
    return 0;
}

/**
 * @brief 解析媒体特性值（仅接受 px 长度）/ Parse a media feature value, accepting only px lengths
 *
 * @details 读取数值后允许可选的 `px` 后缀与空白，其后必须为空；带其它单位的输入判为失败，只理解裸的 px 长度。
 *          A number is read followed by an optional `px` suffix and whitespace, after which nothing may remain; input with any other unit fails as only a bare px length is understood.
 *
 * @param[in]     s 待解析的值文本 / Value text to parse
 * @param[out]    out 解析出的像素值 / Receives the value in pixels
 *
 * @return 成功返回 1，失败返回 0 / 1 on success, 0 on failure
 *
 * @see media_parse_condition
 */
static int media_feature_value(const char *s, float *out)
{
    char *end = NULL;
    float v;
    if (!s || !*s)
        return 0;
    v = (float)strtod(s, &end);
    if (end == s)
        return 0;
    while (end && hpe_is_space(*end))
        end++;
    if (end && hpe_str_ieq_n(end, "px", 2))
        end += 2;
    while (end && hpe_is_space(*end))
        end++;
    if (end && *end)
        return 0; /* only a bare px length is understood */
    *out = v;
    return 1;
}

/* parses the text between `@media` and `{` into `mq`. */
/**
 * @brief 消费前导的 `not`/`only` 并返回取反标志 / Consume a leading `not`/`only` and return the negate flag
 *
 * @details 以 `not` 开头时前移游标并返回 1；以 `only` 开头时只前移游标并返回 0；均以不区分大小写匹配并跳过其后空白。
 *          A leading `not` advances the cursor and returns 1, a leading `only` advances the cursor and returns 0, both matched case-insensitively with trailing whitespace skipped.
 *
 * @param[in,out] pq 解析游标指针 / Parse cursor pointer
 *
 * @return 存在 `not` 返回 1，否则返回 0 / 1 when `not` was present, 0 otherwise
 *
 * @see parse_media_query
 */
static int media_consume_qualifier(char **pq)
{
    char *q = *pq;
    if (hpe_starts_with_i(q, "not")) {
        *pq = hpe_trim((char *)q + 3);
        return 1;
    }
    if (hpe_starts_with_i(q, "only"))
        *pq = hpe_trim((char *)q + 4);
    return 0;
}

/**
 * @brief 解析一个 `(feature: value)` 条件到备选分支 / Parse one `(feature: value)` condition into an alternative
 *
 * @details 截取到 `)` 为止并拆出特性名与值，能识别且未超过条件上限时写入 `conds` 并计数；特性名或值无法识别时把该备选标记为 `unknown`。
 *          The text up to `)` is taken and split into feature and value; a recognised pair within the condition limit is stored in `conds` and counted, while an unrecognised name or value marks the alternative `unknown`.
 *
 * @param[in,out] pq 解析游标指针 / Parse cursor pointer
 * @param[in,out] alt 媒体条件备选分支 / Alternative media branch
 *
 * @note 缺少 `)` 时直接返回，不消费任何输入 / It returns without consuming anything when `)` is missing
 *
 * @see media_feature_id media_feature_value media_parse_alt_body
 */
static void media_parse_condition(char **pq, HpeMediaAlt *alt)
{
    char *q = *pq;
    char *close = strchr(q, ')');
    char *inner;
    char *colon;
    int fid;
    float fv;

    if (!close)
        return;
    *close = '\0';
    inner = hpe_trim(q + 1);
    colon = strchr(inner, ':');
    if (colon)
        *colon = '\0';
    fid = media_feature_id(hpe_trim(inner));
    if (!fid || !colon || !media_feature_value(hpe_trim(colon + 1), &fv)) {
        alt->unknown = 1;
    } else if (alt->cond_count < HPE_MEDIA_MAX_COND) {
        alt->conds[alt->cond_count].feature = fid;
        alt->conds[alt->cond_count].value = fv;
        alt->cond_count++;
    }
    *pq = close + 1;
}

/**
 * @brief 解析一个裸媒体类型 token 到备选分支 / Parse a bare media-type token into an alternative
 *
 * @details 截取到空白为止，`all`/`screen`/`print` 分别映射到对应 `HPE_MTYPE_*`，其它值把备选标记为 `unknown`。
 *          The text up to whitespace is taken and `all`/`screen`/`print` map to their `HPE_MTYPE_*` values while anything else marks the alternative `unknown`.
 *
 * @param[in,out] pq 解析游标指针 / Parse cursor pointer
 * @param[in,out] alt 媒体条件备选分支 / Alternative media branch
 *
 * @see media_parse_alt_body
 */
static void media_parse_type(char **pq, HpeMediaAlt *alt)
{
    char *q = *pq;
    char *sp = q;

    while (*sp && !hpe_is_space(*sp))
        sp++;
    {
        char saved = *sp;
        *sp = '\0';
        if (hpe_str_ieq(q, "all"))
            alt->type = HPE_MTYPE_ALL;
        else if (hpe_str_ieq(q, "screen"))
            alt->type = HPE_MTYPE_SCREEN;
        else if (hpe_str_ieq(q, "print"))
            alt->type = HPE_MTYPE_PRINT;
        else
            alt->unknown = 1;
        *sp = saved;
    }
    *pq = sp;
}

/**
 * @brief 解析一个备选分支内以 `and` 连接的合取 / Parse the `and`-joined conjunction inside one alternative
 *
 * @details 循环读取条件或媒体类型，之后跳过空白；遇 `and` 继续、直接遇 `(` 亦继续，否则结束。
 *          Conditions or media types are read in a loop with trailing whitespace skipped; an `and` continues the loop, a following `(` also continues, and anything else ends it.
 *
 * @param[in,out] q 备选分支的文本游标 / Text cursor of the alternative
 * @param[in,out] alt 媒体条件备选分支 / Alternative media branch
 *
 * @see media_parse_condition media_parse_type
 */
static void media_parse_alt_body(char *q, HpeMediaAlt *alt)
{
    while (*q) {
        if (*q == '(') {
            media_parse_condition(&q, alt);
        } else {
            media_parse_type(&q, alt);
        }
        while (*q && hpe_is_space(*q))
            q++;
        if (hpe_starts_with_i(q, "and")) {
            q = hpe_trim(q + 3);
        } else if (*q == '(') {
            continue;
        } else {
            break;
        }
    }
}

/**
 * @brief 解析 `@media` 与 `{` 之间的文本为媒体查询 / Parse the text between `@media` and `{` into a media query
 *
 * @details 按逗号切分为各备选分支，每段复制成 NUL 结尾串后先消费 `not`/`only` 限定符，再解析类型与 `and` 条件；备选数超过上限即停止。
 *          The text is split into alternatives on commas; each segment is copied into a NUL-terminated string, its `not`/`only` qualifier consumed, and its type and `and` conditions parsed; parsing stops once the alternative limit is reached.
 *
 * @param[in]     text `@media` 关键字之后的文本 / Text after the `@media` keyword
 * @param[in]     length `text` 的字节长度 / Byte length of `text`
 * @param[out]    mq 接收解析出的媒体查询 / Receives the parsed media query
 *
 * @note 进入时先清零并置 `present=1`；`alt_count` 记录实际备选数 / The struct is zeroed with `present=1` on entry and `alt_count` records the alternatives parsed
 *
 * @see media_consume_qualifier media_parse_alt_body
 */
static void parse_media_query(const char *text, size_t length, HpeMediaQuery *mq)
{
    const char *end = text + length;
    const char *p = text;
    int ai = 0;

    memset(mq, 0, sizeof(*mq));
    mq->present = 1;
    while (p < end) {
        const char *seg_end = p;
        char *buf;
        char *q;

        if (ai >= HPE_MEDIA_MAX_ALT)
            break;
        while (seg_end < end && *seg_end != ',')
            seg_end++;

        mq->alts[ai].type = HPE_MTYPE_UNSET;
        /* operate on a NUL-terminated copy so the tokeniser can be reused */
        buf = hpe_strndup(p, (size_t)(seg_end - p));
        q = hpe_trim(buf);
        mq->alts[ai].negate = media_consume_qualifier(&q);
        media_parse_alt_body(q, &mq->alts[ai]);
        hpe_free(buf);
        ai++;
        p = (seg_end < end) ? seg_end + 1 : seg_end;
    }
    mq->alt_count = ai;
}

/**
 * @brief 按视口尺寸求值一组媒体条件 / Evaluate one media condition list against the viewport size
 *
 * @details 逐个备选分支计算：`unknown` 或 `print` 类型直接不匹配；各条件按 min/max-width/height 与视口宽高比较（width/height 取整后相等），再按需取反；任一备选匹配即返回 1（备选之间为或关系）。
 *          Each alternative is evaluated: `unknown` or a `print` type never matches; conditions compare the feature against the viewport width or height (with width/height requiring integer equality) and the result is negated when needed; any matching alternative returns 1 since alternatives are OR-ed.
 *
 * @param[in]     mq 媒体查询 / Media query
 * @param[in]     vw 视口宽度 / Viewport width
 * @param[in]     vh 视口高度 / Viewport height
 *
 * @return 匹配返回 1，否则返回 0 / 1 on a match, 0 otherwise
 *
 * @see rule_media_ok
 */
static int media_alt_matches(const HpeMediaQuery *mq, float vw, float vh)
{
    int alt;
    for (alt = 0; alt < mq->alt_count; alt++) {
        int match = 1;
        int i;
        if (mq->alts[alt].unknown)
            match = 0;
        if (mq->alts[alt].type == HPE_MTYPE_PRINT)
            match = 0; /* the engine is a screen medium */
        /* an unset type is `all` */
        for (i = 0; i < mq->alts[alt].cond_count; i++) {
            int f = mq->alts[alt].conds[i].feature;
            float v = mq->alts[alt].conds[i].value;
            switch (f) {
            case HPE_MF_MIN_WIDTH: match = match && (vw >= v); break;
            case HPE_MF_MAX_WIDTH: match = match && (vw <= v); break;
            case HPE_MF_MIN_HEIGHT: match = match && (vh >= v); break;
            case HPE_MF_MAX_HEIGHT: match = match && (vh <= v); break;
            case HPE_MF_WIDTH: match = match && ((int)vw == (int)v); break;
            case HPE_MF_HEIGHT: match = match && ((int)vh == (int)v); break;
            default: match = 0; break;
            }
        }
        if (mq->alts[alt].negate)
            match = !match;
        if (match)
            return 1;
    }
    return 0;
}

/**
 * @brief 设置用于求值 @media 的视口尺寸 / Set the viewport size used to evaluate @media
 *
 * @details 只写入宽高两个字段；规则的 `@media` 条件在层叠时按此尺寸即时求值，因此在重新层叠前调用即可让媒体查询随窗口变化生效。
 *          Only the width and height fields are written; each rule's `@media` conditions are evaluated against this size at cascade time, so calling it before a re-cascade makes media queries follow the window.
 *
 * @param[in,out] sheet 目标样式表 / Target stylesheet
 * @param[in]     vw 视口宽度（像素）/ Viewport width in pixels
 * @param[in]     vh 视口高度（像素）/ Viewport height in pixels
 *
 * @note 传 `NULL` 时直接返回 / Returns immediately when `NULL` is passed
 *
 * @see hpe_css_compute
 */
void hpe_css_set_viewport(HpeStylesheet *sheet, float vw, float vh)
{
    if (!sheet)
        return;
    sheet->vw = vw;
    sheet->vh = vh;
}

/**
 * @brief 跳过空白与 `/ * * /` 注释 / Skip whitespace and `/ * * /` comments
 *
 * @details 循环跳过空白；遇到 `/ *` 则一路跳到配对的 `* /` 之后继续，注释未闭合时直接跳到输入末尾。
 *          Whitespace is skipped in a loop; on `/ *` the scan runs to the matching `* /` and continues, and an unterminated comment sends the cursor to the end of the input.
 *
 * @param[in,out] pp 解析游标指针 / Parse cursor pointer
 * @param[in]     end 输入缓冲区末尾 / End of the input buffer
 *
 * @see css_parse_into
 */

static void skip_ws_comments(const char **pp, const char *end)
{
    const char *p = *pp;
    for (;;) {
        while (p < end && hpe_is_space(*p))
            p++;
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            const char *q = p + 2;
            while (q + 1 < end && !(q[0] == '*' && q[1] == '/'))
                q++;
            p = (q + 1 < end) ? q + 2 : end;
            continue;
        }
        break;
    }
    *pp = p;
}

/**
 * @brief 找到与起始 `{` 配对的 `}` / Find the `}` matching a block's opening `{`
 *
 * @details 从 `block` 开始按大括号计数扫描，每遇到 `}` 深度减一，深度归零即返回该位置；块未闭合时返回 `end`。
 *          The scan counts braces from `block`, decrementing on each `}` and returning that position when the depth reaches zero; an unterminated block returns `end`.
 *
 * @param[in]     block 块体起始指针（`{` 之后）/ Pointer to the block body, just past `{`
 * @param[in]     end 输入缓冲区末尾 / End of the input buffer
 *
 * @return 配对的 `}` 位置，或未闭合时的 `end` / The matching `}` position, or `end` when unterminated
 *
 * @see parse_at_rule
 */
static const char *find_block_end(const char *block, const char *end)
{
    const char *scan = block;
    int depth = 1;

    while (scan < end && depth > 0) {
        if (*scan == '{')
            depth++;
        else if (*scan == '}')
            depth--;
        if (depth > 0)
            scan++;
    }
    return scan;
}

/**
 * @brief 把一条已解析的规则连同其 @media 链追加到样式表 / Append a fully-parsed rule with its @media chain to a stylesheet
 *
 * @details `mq_depth` 大于 0 时把外层 `@media` 的媒体查询栈整段复制进规则的 `mq` 并记录深度，随后扩容规则数组并以值拷贝追加该规则。
 *          When `mq_depth` is above 0 the enclosing `@media` query stack is copied wholesale into the rule's `mq` array with its depth recorded, after which the rule array is grown and the rule appended by value.
 *
 * @param[in,out] sheet 目标样式表 / Target stylesheet
 * @param[in,out] rule 待追加的规则（其声明数组所有权移交样式表）/ Rule being appended, whose declaration array ownership transfers to the sheet
 * @param[in]     mq_stack 外层 `@media` 的媒体查询栈 / Media query stack of enclosing `@media` blocks
 * @param[in]     mq_depth 嵌套深度 / Nesting depth
 *
 * @see parse_style_rule rule_media_ok
 */
static void sheet_add_rule(HpeStylesheet *sheet, HpeRule *rule,
                           const HpeMediaQuery *mq_stack, int mq_depth)
{
    int k;

    if (mq_depth > 0) {
        for (k = 0; k < mq_depth; k++)
            rule->mq[k] = mq_stack[k];
        rule->mq_depth = mq_depth;
    }
    sheet->rules = (HpeRule *)hpe_grow(sheet->rules, &sheet->rule_cap,
                                       (size_t)sheet->rule_count + 1, sizeof(HpeRule));
    sheet->rules[sheet->rule_count++] = *rule;
}

/**
 * @brief 校验一个属性取值是否被引擎支持 / Validate whether a property value is supported by the engine
 *
 * @details 只对取值有限枚举的属性做严格校验（`display`、`position`、`overflow`、`float`、`text-align`、
 *          `flex-direction`、`flex-wrap`、`box-sizing`、`visibility`），其余属性只要属性名本身已知即视为支持，
 *          这与引擎「未知属性名是主要拒绝判据」的宽松策略一致。
 *          Only properties with a small enumerable value set are validated strictly (`display`, `position`, `overflow`, `float`, `text-align`, `flex-direction`, `flex-wrap`, `box-sizing`, `visibility`); every other property counts as supported as soon as its name is known, mirroring the engine's lenient policy where an unknown property name is the main rejection criterion.
 *
 * @param[in]     name 属性名（小写）/ Property name, lower case
 * @param[in]     value 属性值（已去空白）/ Property value, already trimmed
 *
 * @return 支持返回 1，否则返回 0 / 1 when supported, 0 otherwise
 *
 * @see supports_condition_ok
 */
/* The enumerated value set for a property, or NULL when the property accepts
 * any value. */
static const char *const *supports_value_set(const char *name)
{
    static const char *const display_vals[] = {"none", "inline", "block", "inline-block", "flex",
                                               "inline-flex", "grid", "inline-grid", "table",
                                               "table-row", "table-cell", "table-row-group",
                                               "table-header-group", "table-footer-group",
                                               "table-caption", "inline-table", NULL};
    static const char *const position_vals[] = {"static", "relative", "absolute", "fixed", NULL};
    static const char *const overflow_vals[] = {"visible", "hidden", "auto", "scroll", NULL};
    static const char *const float_vals[] = {"none", "left", "right", "inline-start",
                                             "inline-end", NULL};
    static const char *const align_vals[] = {"left", "right", "center", "justify", "start",
                                             "end", NULL};
    static const char *const flexdir_vals[] = {"row", "row-reverse", "column",
                                               "column-reverse", NULL};
    static const char *const flexwrap_vals[] = {"nowrap", "wrap", "wrap-reverse", NULL};
    static const char *const boxsizing_vals[] = {"content-box", "border-box", NULL};
    static const char *const visibility_vals[] = {"visible", "hidden", "collapse", NULL};
    static const char *const scrollbar_vals[] = {"auto", "thin", "none", NULL};
    if (!strcmp(name, "display"))
        return display_vals;
    if (!strcmp(name, "position"))
        return position_vals;
    if (!strcmp(name, "overflow") || !strcmp(name, "overflow-x") || !strcmp(name, "overflow-y"))
        return overflow_vals;
    if (!strcmp(name, "float"))
        return float_vals;
    if (!strcmp(name, "text-align"))
        return align_vals;
    if (!strcmp(name, "flex-direction"))
        return flexdir_vals;
    if (!strcmp(name, "flex-wrap"))
        return flexwrap_vals;
    if (!strcmp(name, "box-sizing"))
        return boxsizing_vals;
    if (!strcmp(name, "visibility"))
        return visibility_vals;
    if (!strcmp(name, "scrollbar-width"))
        return scrollbar_vals;
    return NULL;
}

static int supports_value_ok(const char *name, const char *value)
{
    const char *const *tbl = supports_value_set(name);
    int i;

    if (!tbl)
        return 1;
    for (i = 0; tbl[i]; i++) {
        if (!strcmp(tbl[i], value))
            return 1;
    }
    /* `scrollbar-width` also accepts any non-negative length */
    if (!strcmp(name, "scrollbar-width")) {
        const char *p = value;
        if (*p == '+' || *p == '-')
            p++;
        if (*p >= '0' && *p <= '9')
            return 1;
    }
    return 0;
}

/**
 * @brief 测试一条 `(prop: value)` 支持条件 / Test one `(prop: value)` support condition
 *
 * @details 在属性表中查找属性名；不带 `:` 时仅测试属性是否存在。带值时先确认属性存在，再对取值有限枚举的属性
 *          用 `supports_value_ok` 校验取值，其余属性名已知即视为支持。
 *          The property name is looked up in the property table; without a `:` only its existence is tested. With a value the property must exist and, for properties with a fixed value set, the value is validated through `supports_value_ok`; every other known property is accepted.
 *
 * @param[in]     text 条件文本（可含外层空白）/ Condition text, possibly with surrounding spaces
 * @param[in]     len 文本长度 / Text length
 *
 * @return 支持返回 1，否则返回 0 / 1 when supported, 0 otherwise
 *
 * @see supports_value_ok eval_supports
 */
static int supports_condition_ok(const char *text, size_t len)
{
    char buf[160];
    char *colon;
    size_t i;
    size_t prop_count = sizeof(k_prop_table) / sizeof(k_prop_table[0]);
    int known = 0;

    while (len > 0 && (*text == ' ' || *text == '\t' || *text == '\n'))
        text++, len--;
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\n'))
        len--;
    if (len >= sizeof(buf))
        return 1; /* too long to be a property test: assume supported */
    memcpy(buf, text, len);
    buf[len] = '\0';
    colon = strchr(buf, ':');
    if (colon)
        *colon = '\0';
    hpe_trim(buf);
    if (!buf[0])
        return 0;
    for (i = 0; i < prop_count; i++) {
        if (!strcmp(k_prop_table[i].name, buf)) {
            known = 1;
            break;
        }
    }
    if (!known)
        return 0;
    if (!colon)
        return 1;
    /* `hpe_trim` edits the buffer in place, so trim into the value pointer
     * before passing it on; the `: ` separator leaves a leading space */
    {
        char *val = colon + 1;
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[0] == ' ' || val[0] == '\t'))
            val++, vlen--;
        while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
            val[--vlen] = '\0';
        if (!val[0])
            return 1;
        return supports_value_ok(buf, val);
    }
}

/**
 * @brief 求值 `@supports` 条件（支持 `and` / `or` / `not` 与括号）/ Evaluate a `@supports` condition
 *
 * @details 支持 `(prop: value)`、`not <cond>`、`<cond> and <cond>`（用括号分组）以及 `<cond> or <cond>`；
 *          裸标识符（如 `(display: grid)` 之外的语法）按不支持处理。求值失败时保守返回 0。
 *          It supports `(prop: value)`, `not <cond>`, `<cond> and <cond>` (grouped with parentheses) and `<cond> or <cond>`; a bare identifier outside those forms is treated as unsupported. Evaluation failures conservatively return 0.
 *
 * @param[in]     text 条件文本 / Condition text
 * @param[in]     len 文本长度 / Text length
 *
 * @return 条件成立返回 1，否则返回 0 / 1 when the condition holds, 0 otherwise
 *
 * @see supports_condition_ok parse_at_rule
 */
/* When the whole condition is wrapped in one matching pair of parentheses,
 * unwrap it so `and`/`or`/`not` inside become visible. Returns 1 when the
 * caller should recurse into `buf + 1 .. len - 2`. */
static int supports_unwrap_parens(const char *buf, size_t len)
{
    int depth = 0;
    size_t i;
    if (buf[0] != '(')
        return 0;
    for (i = 0; i < len; i++) {
        if (buf[i] == '(')
            depth++;
        else if (buf[i] == ')') {
            depth--;
            if (depth == 0) {
                if (i == len - 1) {
                    if (strchr(buf + 1, ':') && !strchr(buf + 1, '('))
                        return 2; /* a bare (prop: value) group */
                    return 1;
                }
                break; /* the leading group is only part of the condition */
            }
        }
    }
    return 0;
}

static int eval_supports(const char *text, size_t len);

/* Split at the first top-level ` and ` / ` or ` outside parentheses and
 * evaluate both sides. Returns -1 when no top-level operator is present, else
 * 0 or 1 for the combined result. */
static int supports_eval_split(const char *buf, size_t len)
{
    int depth = 0;
    size_t i;
    const char *op = NULL;
    int leave = 0;
    for (i = 0; i + 5 <= len; i++) {
        if (buf[i] == '(')
            depth++;
        else if (buf[i] == ')')
            depth--;
        else if (depth == 0) {
            if (!strncmp(buf + i, " and ", 5)) {
                op = buf + i;
                leave = 5;
            } else if (!strncmp(buf + i, " or ", 4)) {
                op = buf + i;
                leave = 4;
            } else {
                continue;
            }
            break;
        }
    }
    if (!op)
        return -1;
    if (leave == 5)
        return eval_supports(buf, (size_t)(op - buf)) &&
               eval_supports(op + 5, len - (size_t)(op - buf) - 5);
    return eval_supports(buf, (size_t)(op - buf)) ||
           eval_supports(op + 4, len - (size_t)(op - buf) - 4);
}

static int eval_supports(const char *text, size_t len)
{
    char buf[256];
    int unwrapped, split;

    while (len > 0 && (*text == ' ' || *text == '\t' || *text == '\n'))
        text++, len--;
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\n'))
        len--;
    if (len == 0 || len >= sizeof(buf))
        return 0;
    memcpy(buf, text, len);
    buf[len] = '\0';

    /* unwrap a fully parenthesised condition so `and`/`or`/`not` can be seen */
    unwrapped = supports_unwrap_parens(buf, len);
    if (unwrapped == 1)
        return eval_supports(buf + 1, len - 2);
    if (unwrapped == 2)
        return supports_condition_ok(buf + 1, len - 2);
    if (len >= 6 && hpe_str_ieq_n(buf, "not ", 4))
        return !eval_supports(buf + 4, len - 4);

    /* find a top-level ` and ` / ` or ` outside any parentheses */
    split = supports_eval_split(buf, len);
    if (split >= 0)
        return split;
    return supports_condition_ok(buf, len);
}

/**
 * @brief 解析一个 at-rule 的体 / Parse the body of an at-rule
 *
 * @details 以 `;` 结尾的语句型 at-rule 直接跳过；否则找到块体匹配的 `}`。`@media` 在未超过嵌套上限时解析条件并压栈再递归；
 *          `@supports` 条件成立时同样递归进入其块（不压媒体栈）；`@supports` 条件不成立或其它未知 at-rule 整体跳过，
 *          避免未知块破坏后续规则流。
 *          A statement at-rule ending in `;` is skipped; otherwise the matching `}` of its body is found. An `@media` within the nesting limit has its conditions parsed and pushed before recursing; a satisfied `@supports` also recurses into its block (without touching the media stack), while an unsatisfied `@supports` and any other unknown at-rule are skipped wholesale so an unknown block never corrupts the surrounding rule stream.
 *
 * @param[in,out] sheet 目标样式表 / Target stylesheet
 * @param[in,out] pp 解析游标指针 / Parse cursor pointer
 * @param[in]     q at-rule 头部结束处（`;` 或 `{`）/ End of the at-rule prelude (`;` or `{`)
 * @param[in]     end 输入缓冲区末尾 / End of the input buffer
 * @param[in,out] mq_stack 媒体查询栈 / Media query stack
 * @param[in]     mq_depth 当前嵌套深度 / Current nesting depth
 *
 * @see parse_media_query eval_supports css_parse_into find_block_end
 */
static void parse_at_rule(HpeStylesheet *sheet, const char **pp, const char *q,
                          const char *end, HpeMediaQuery *mq_stack, int mq_depth)
{
    const char *p = *pp;
    int is_media, is_supports;

    if (*q == ';') {
        *pp = q + 1;
        return;
    }
    is_media = (size_t)(q - p) >= 6 && hpe_str_ieq_n(p, "@media", 6);
    is_supports = (size_t)(q - p) >= 9 && hpe_str_ieq_n(p, "@supports", 9);
    {
        const char *block = q + 1;
        const char *scan = find_block_end(block, end);
        int stacked = 0;

        if (is_media && mq_depth < HPE_MEDIA_MAX_DEPTH) {
            HpeMediaQuery mq;

            parse_media_query(p + 6, (size_t)(q - (p + 6)), &mq);
            if (mq.alt_count > 0) {
                mq_stack[mq_depth] = mq;
                stacked = 1;
            }
        }
        if (is_media)
            css_parse_into(sheet, block, (size_t)(scan - block), mq_stack,
                           mq_depth + (stacked ? 1 : 0));
        else if (is_supports && eval_supports(p + 9, (size_t)(q - (p + 9))))
            css_parse_into(sheet, block, (size_t)(scan - block), mq_stack, mq_depth);
        *pp = scan < end ? scan + 1 : end;
    }
}

/**
 * @brief 前进到声明块结束，识别字符串中的 `}` / Advance to the end of a declaration block, honouring quoted strings
 *
 * @details 扫描到 `}` 为止，但遇到引号时先跳到配对的引号之后，因此字符串值里的 `}` 不会提前结束块。
 *          The scan runs to `}` but on a quote it jumps past the matching quote first, so a `}` inside a string value does not terminate the block early.
 *
 * @param[in]     p 块体起始指针 / Pointer to the start of the block body
 * @param[in]     end 输入缓冲区末尾 / End of the input buffer
 *
 * @return 块结束的 `}` 位置，或未闭合时的 `end` / The terminating `}` position, or `end` when unterminated
 *
 * @see parse_style_rule
 */
static const char *find_decl_block_end(const char *p, const char *end)
{
    const char *scan = p;

    while (scan < end && *scan != '}') {
        if (*scan == '"' || *scan == '\'') {
            char quote = *scan++;

            while (scan < end && *scan != quote)
                scan++;
            if (scan < end)
                scan++;
            continue;
        }
        scan++;
    }
    return scan;
}

/**
 * @brief 解析一条 `选择器 { 声明 }` 规则 / Parse one `selector { declarations }` rule
 *
 * @details 先定位 `{` 得到选择器与声明区间，解析选择器列表后解析声明块；仅当选择器非空且声明确实设置了属性时，才把该规则连同递增的源码顺序号追加到样式表。
 *          The `{` is located to delimit the selector and declaration spans, the selector list and declaration block are then parsed, and only when selectors are present and the declarations actually set a property is the rule appended to the sheet with an incremented source-order number.
 *
 * @param[in,out] sheet 目标样式表 / Target stylesheet
 * @param[in,out] pp 解析游标指针 / Parse cursor pointer
 * @param[in]     end 输入缓冲区末尾 / End of the input buffer
 * @param[in]     mq_stack 外层 `@media` 的媒体查询栈 / Media query stack of enclosing `@media` blocks
 * @param[in]     mq_depth 嵌套深度 / Nesting depth
 *
 * @note 找不到 `{` 或声明为空时游标推进到末尾且不产生规则 / When no `{` is found or the declarations are empty the cursor moves to the end and no rule is produced
 *
 * @see parse_selector_list hpe_css_parse_declarations sheet_add_rule
 */
static void parse_style_rule(HpeStylesheet *sheet, const char **pp, const char *end,
                             const HpeMediaQuery *mq_stack, int mq_depth)
{
    const char *p = *pp;
    const char *sel_start = p;
    const char *decl_start;
    const char *scan;
    size_t sel_len, decl_len;
    HpeRule rule;

    while (p < end && *p != '{')
        p++;
    if (p >= end) {
        *pp = end;
        return;
    }
    sel_len = (size_t)(p - sel_start);
    p++; /* '{' */
    decl_start = p;
    scan = find_decl_block_end(p, end);
    decl_len = (size_t)(scan - decl_start);
    *pp = (scan < end) ? scan + 1 : end;

    memset(&rule, 0, sizeof(rule));
    parse_selector_list(&rule, sel_start, sel_len);
    if (rule.selector_count > 0) {
        HpeStyle decl;

        hpe_css_parse_declarations(decl_start, decl_len, &decl);
        if (decl.set != 0 || decl.set2 != 0) {
            /* store one single-property declaration per entry for ordered application */
            rule.decls = (HpeStyle *)hpe_alloc(sizeof(HpeStyle));
            rule.decls[0] = decl;
            rule.decl_count = 1;
            rule.order = sheet->next_order++;
            sheet_add_rule(sheet, &rule, mq_stack, mq_depth);
            return;
        }
    }
    rule_free(&rule);
}

/**
 * @brief 递归解析样式表文本并追加规则 / Recursively parse stylesheet text and append rules
 *
 * @details 跳过空白与注释后，`@` 开头的交给 `parse_at_rule`（`@media` 会以更深的栈递归回本函数），其余交给 `parse_style_rule`；规则的 `mq_stack` 保存外层 `@media` 条件（最外层在前），层叠时须整条链都匹配视口才生效。
 *          After whitespace and comments are skipped, `@`-prefixed input goes to `parse_at_rule` (whose `@media` re-enters this function with a deeper stack) and anything else to `parse_style_rule`; a rule's `mq_stack` holds the enclosing `@media` conditions outermost first and only matches at cascade time when the whole chain matches the viewport.
 *
 * @param[in,out] sheet 目标样式表 / Target stylesheet
 * @param[in]     css 待解析的 CSS 文本 / CSS text to parse
 * @param[in]     length `css` 的字节长度 / Byte length of `css`
 * @param[in,out] mq_stack 媒体查询栈 / Media query stack
 * @param[in]     mq_depth 当前嵌套深度 / Current nesting depth
 *
 * @note `sheet` 或 `css` 为 `NULL` 时直接返回 / It returns immediately when `sheet` or `css` is `NULL`
 *
 * @see parse_at_rule parse_style_rule hpe_css_parse
 */
static void css_parse_into(HpeStylesheet *sheet, const char *css, size_t length,
                           HpeMediaQuery *mq_stack, int mq_depth)
{
    const char *p = css;
    const char *end = css + length;

    if (!sheet || !css)
        return;

    while (p < end) {
        skip_ws_comments(&p, end);
        if (p >= end)
            break;
        if (*p == '@') {
            const char *q = p;

            while (q < end && *q != '{' && *q != ';')
                q++;
            if (q >= end)
                break;
            parse_at_rule(sheet, &p, q, end, mq_stack, mq_depth);
        } else {
            parse_style_rule(sheet, &p, end, mq_stack, mq_depth);
        }
    }
}

/**
 * @brief 解析一段 CSS 文本并追加到样式表 / Parse CSS text and append it to a stylesheet
 *
 * @details 在栈上分配媒体查询栈后从第 0 层开始调用 `css_parse_into`，规则按出现顺序追加且不会清空已有规则。
 *          A media-query stack is allocated on the stack and `css_parse_into` is entered at depth 0; rules are appended in source order and existing rules are kept.
 *
 * @param[in,out] sheet 目标样式表 / Target stylesheet
 * @param[in]     css 待解析的 CSS 文本 / CSS text to parse
 * @param[in]     length `css` 的字节长度 / Byte length of `css`
 *
 * @see css_parse_into hpe_css_init
 */
void hpe_css_parse(HpeStylesheet *sheet, const char *css, size_t length)
{
    HpeMediaQuery stack[HPE_MEDIA_MAX_DEPTH];
    css_parse_into(sheet, css, length, stack, 0);
}

/**
 * @brief 初始化样式表并载入内置 UA 样式 / Initialise a stylesheet and load the built-in UA stylesheet
 *
 * @details 先把整个结构清零、把视口默认设为 800x600，再解析 `hpe_ua_stylesheet()` 的文本，因此新样式表一开始就带有浏览器默认样式。
 *          The struct is zeroed, the viewport defaults to 800x600 and the text from `hpe_ua_stylesheet()` is parsed, so a fresh sheet already carries the user-agent defaults.
 *
 * @param[in,out] sheet 目标样式表 / Target stylesheet
 *
 * @note 传 `NULL` 时直接返回 / Returns immediately when `NULL` is passed
 *
 * @see hpe_css_free hpe_css_parse hpe_ua_stylesheet
 */
void hpe_css_init(HpeStylesheet *sheet)
{
    if (!sheet)
        return;
    memset(sheet, 0, sizeof(*sheet));
    sheet->vw = 800.0f; /* a sensible default until the engine sets the viewport */
    sheet->vh = 600.0f;
    hpe_css_parse(sheet, hpe_ua_stylesheet(), strlen(hpe_ua_stylesheet()));
}

/**
 * @brief 释放样式表持有的全部规则 / Release every rule held by a stylesheet
 *
 * @details 逐条调用 `rule_free` 后释放规则数组并把整个结构清零，可安全重复调用。
 *          Every rule is released through `rule_free`, the rule array is freed and the struct zeroed, so repeated calls are safe.
 *
 * @param[in,out] sheet 目标样式表 / Target stylesheet
 *
 * @note 传 `NULL` 时直接返回 / Returns immediately when `NULL` is passed
 *
 * @see hpe_css_init rule_free
 */
void hpe_css_free(HpeStylesheet *sheet)
{
    int i;
    if (!sheet)
        return;
    for (i = 0; i < sheet->rule_count; i++)
        rule_free(&sheet->rules[i]);
    if (sheet->rules)
        hpe_free(sheet->rules);
    memset(sheet, 0, sizeof(*sheet));
}

/* ---------------- cascade ---------------- */

/**
 * @brief 按掩码拷贝四边的 margin/padding/边框宽/inset / Copy the four-sided margin, padding, border-width and inset longhands selected by a mask
 *
 * @details 逐边检查 `HPE_PROP_MARGIN_TOP`/`PADDING_TOP`/`BORDER_TOP`/`INSET_TOP` 及其 `+i` 位，命中即拷贝该边长属性；掩码含对应的“四边”位时再统一拷贝四条边框宽度。
 *          Each edge is checked for the `HPE_PROP_MARGIN_TOP`/`PADDING_TOP`/`BORDER_TOP`/`INSET_TOP` bit and its `+i` variants, copying that longhand on a hit; when the shorthand border-width bit is present all four border widths are copied.
 *
 * @param[in,out] dst 接收拷贝的目标样式 / Target style that receives the copies
 * @param[in]     src 源样式 / Source style
 * @param[in]     s 本次生效的属性位掩码 / Property bit mask that is in effect
 *
 * @see apply_decl_masked
 */
static void apply_box_edges(HpeStyle *dst, const HpeStyle *src, uint64_t s)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (s & (HPE_PROP_MARGIN_TOP << i))
            dst->margin[i] = src->margin[i];
        if (s & (HPE_PROP_PADDING_TOP << i))
            dst->padding[i] = src->padding[i];
        if (s & (HPE_PROP_BORDER_TOP << i))
            dst->border_w[i] = src->border_w[i];
        if (s & (HPE_PROP_INSET_TOP << i))
            dst->inset[i] = src->inset[i];
    }
    if (s & HPE_PROP_BORDER_WIDTH) {
        for (i = 0; i < 4; i++)
            dst->border_w[i] = src->border_w[i];
    }
}

/* Table-driven longhand copy: every scalar longhand is "if its bit is set in
 * the declaration mask, copy the member bytes verbatim". Keeping the mapping
 * as data removes the previous long if-chains and guarantees that adding a
 * property only means adding one row here. */
typedef struct ScalarCopy {
    uint64_t bit;  /* property bit that gates the copy */
    size_t offset; /* offsetof the HpeStyle member */
    size_t size;   /* sizeof that member */
} ScalarCopy;

#define SCALAR_COPY(bit, member) \
    { (bit), offsetof(HpeStyle, member), sizeof(((HpeStyle *)0)->member) }

/* longhands carried in the first property word (HPE_PROP_*) */
static const ScalarCopy k_scalars_word1[] = {
    SCALAR_COPY(HPE_PROP_DISPLAY, display),
    SCALAR_COPY(HPE_PROP_COLOR, color),
    SCALAR_COPY(HPE_PROP_BACKGROUND, background),
    SCALAR_COPY(HPE_PROP_WIDTH, width),
    SCALAR_COPY(HPE_PROP_MIN_WIDTH, min_width),
    SCALAR_COPY(HPE_PROP_HEIGHT, height),
    SCALAR_COPY(HPE_PROP_BORDER_RADIUS, border_radius),
    SCALAR_COPY(HPE_PROP_BORDER_COLOR, border_color),
    SCALAR_COPY(HPE_PROP_FLEX_DIR, flex_dir),
    SCALAR_COPY(HPE_PROP_ALIGN_ITEMS, align_items),
    SCALAR_COPY(HPE_PROP_ALIGN_SELF, align_self),
    SCALAR_COPY(HPE_PROP_JUSTIFY, justify),
    SCALAR_COPY(HPE_PROP_GAP, gap),
    SCALAR_COPY(HPE_PROP_FLEX_GROW, flex_grow),
    SCALAR_COPY(HPE_PROP_FONT_SIZE, font_size),
    SCALAR_COPY(HPE_PROP_FONT_WEIGHT, font_weight),
    SCALAR_COPY(HPE_PROP_TEXT_ALIGN, text_align),
    SCALAR_COPY(HPE_PROP_LINE_HEIGHT, line_height),
    SCALAR_COPY(HPE_PROP_WHITE_SPACE, white_space),
    SCALAR_COPY(HPE_PROP_FONT_FAMILY, family),
    SCALAR_COPY(HPE_PROP_POSITION, position),
    /* `z-index` carries both the value and its `auto` flag */
    SCALAR_COPY(HPE_PROP_Z_INDEX, z_index),
    SCALAR_COPY(HPE_PROP_Z_INDEX, z_auto),
    SCALAR_COPY(HPE_PROP_FLOAT, float_side),
    SCALAR_COPY(HPE_PROP_CLEAR, clear_side),
};

/* longhands carried in the second property word (HPE_PROP2_*) */
static const ScalarCopy k_scalars_word2[] = {
    SCALAR_COPY(HPE_PROP2_MIN_HEIGHT, min_height),
    SCALAR_COPY(HPE_PROP2_MAX_WIDTH, max_width),
    SCALAR_COPY(HPE_PROP2_MAX_HEIGHT, max_height),
    SCALAR_COPY(HPE_PROP2_BOX_SIZING, box_sizing),
    SCALAR_COPY(HPE_PROP2_OVERFLOW_X, overflow_x),
    SCALAR_COPY(HPE_PROP2_OVERFLOW_Y, overflow_y),
    SCALAR_COPY(HPE_PROP2_OPACITY, opacity),
    SCALAR_COPY(HPE_PROP2_VISIBILITY, visibility),
    SCALAR_COPY(HPE_PROP2_CURSOR, cursor),
    SCALAR_COPY(HPE_PROP2_TEXT_DECORATION, text_decoration),
    SCALAR_COPY(HPE_PROP2_LETTER_SPACING, letter_spacing),
    SCALAR_COPY(HPE_PROP2_WORD_SPACING, word_spacing),
    SCALAR_COPY(HPE_PROP2_TEXT_INDENT, text_indent),
    SCALAR_COPY(HPE_PROP2_VERTICAL_ALIGN, vertical_align),
    SCALAR_COPY(HPE_PROP2_LIST_STYLE_TYPE, list_style_type),
    SCALAR_COPY(HPE_PROP2_LIST_STYLE_POSITION, list_style_position),
    SCALAR_COPY(HPE_PROP2_BACKGROUND_IMAGE, background_image),
    SCALAR_COPY(HPE_PROP2_BOX_SHADOW, box_shadow),
    SCALAR_COPY(HPE_PROP2_TEXT_SHADOW, text_shadow),
    SCALAR_COPY(HPE_PROP2_TRANSFORM, transform),
    SCALAR_COPY(HPE_PROP2_TRANSITION, has_transition),
    SCALAR_COPY(HPE_PROP2_ANIMATION, has_animation),
    /* generated content (`content` carries the resolved text plus the
     * `attr()` argument that is only resolved once the node is known) */
    SCALAR_COPY(HPE_PROP2_CONTENT, content),
    SCALAR_COPY(HPE_PROP2_CONTENT, attr_content),
    SCALAR_COPY(HPE_PROP2_CONTENT, has_content),
    /* flex item longhands */
    SCALAR_COPY(HPE_PROP2_FLEX_WRAP, flex_wrap),
    SCALAR_COPY(HPE_PROP2_ROW_REVERSE, row_reverse),
    SCALAR_COPY(HPE_PROP2_ORDER, order),
    SCALAR_COPY(HPE_PROP2_FLEX_BASIS, flex_basis),
    SCALAR_COPY(HPE_PROP2_FLEX_SHRINK, flex_shrink),
    /* background placement: `background-position` and `background-size` share
     * the single `bg_pos` member, so both bits gate the same copy */
    SCALAR_COPY(HPE_PROP2_BACKGROUND_POSITION | HPE_PROP2_BACKGROUND_SIZE, bg_pos),
    /* `object-position` also reuses the HpeBgPos form */
    SCALAR_COPY(HPE_PROP2_OBJECT_POSITION, object_pos),
    SCALAR_COPY(HPE_PROP2_OBJECT_FIT, object_fit),
    SCALAR_COPY(HPE_PROP2_BACKGROUND_REPEAT, bg_repeat),
    SCALAR_COPY(HPE_PROP2_RADIAL_GRADIENT, bg_radial),
    SCALAR_COPY(HPE_PROP2_RADIAL_GRADIENT, bg_radial_circle),
    SCALAR_COPY(HPE_PROP2_RADIAL_GRADIENT, bg_radial_cx),
    SCALAR_COPY(HPE_PROP2_RADIAL_GRADIENT, bg_radial_cy),
    SCALAR_COPY(HPE_PROP2_RADIAL_GRADIENT, bg_radial_rx),
    SCALAR_COPY(HPE_PROP2_RADIAL_GRADIENT, bg_radial_ry),
    /* grid container specs */
    SCALAR_COPY(HPE_PROP2_GRID_TEMPLATE_COLUMNS, grid_cols),
    SCALAR_COPY(HPE_PROP2_GRID_TEMPLATE_ROWS, grid_rows),
    SCALAR_COPY(HPE_PROP2_GRID_GAP, grid_gap),
    /* grid item placement and container distribution */
    SCALAR_COPY(HPE_PROP2_GRID_COLUMN, grid_col_start),
    SCALAR_COPY(HPE_PROP2_GRID_COLUMN, grid_col_end),
    SCALAR_COPY(HPE_PROP2_GRID_ROW, grid_row_start),
    SCALAR_COPY(HPE_PROP2_GRID_ROW, grid_row_end),
    SCALAR_COPY(HPE_PROP2_JUSTIFY_ITEMS, justify_items),
    SCALAR_COPY(HPE_PROP2_ALIGN_CONTENT, align_content),
    SCALAR_COPY(HPE_PROP2_GRID_AUTO_FLOW, grid_auto_flow),
    /* table / writing mode / scrollbar */
    SCALAR_COPY(HPE_PROP2_TABLE_LAYOUT, table_layout_fixed),
    SCALAR_COPY(HPE_PROP2_BORDER_SPACING, border_spacing_h),
    SCALAR_COPY(HPE_PROP2_BORDER_SPACING, border_spacing_v),
    SCALAR_COPY(HPE_PROP2_DIRECTION, direction_rtl),
    SCALAR_COPY(HPE_PROP2_SCROLLBAR_WIDTH, scrollbar_width),
};

/**
 * @brief 按掩码表逐项把源样式的标量成员拷到目标 / Copy the scalar members selected by a mask table from source to destination
 *
 * @details 遍历拷贝表，凡其位在 `mask` 中的项即用表中记录的偏移与大小整体拷贝成员字节，因此新增属性只需在表中加一行。
 *          The table is walked and every entry whose bit is set in `mask` has its member bytes copied wholesale using the recorded offset and size, so adding a property means adding one row.
 *
 * @param[in,out] dst 接收拷贝的目标样式 / Target style that receives the copies
 * @param[in]     src 源样式 / Source style
 * @param[in]     mask 生效的属性位掩码 / Property bit mask in effect
 * @param[in]     tbl 长属性拷贝表 / Scalar longhand copy table
 * @param[in]     count 表中的条目数 / Number of entries in the table
 *
 * @see apply_first_word_scalars apply_second_word_scalars
 */
static void apply_scalar_table(HpeStyle *dst, const HpeStyle *src, uint64_t mask,
                               const ScalarCopy *tbl, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (mask & tbl[i].bit)
            memcpy((char *)dst + tbl[i].offset, (const char *)src + tbl[i].offset,
                   tbl[i].size);
    }
}

/**
 * @brief 拷贝第一属性字中的标量长属性 / Copy the scalar longhands carried in the first property word
 *
 * @details 以第一属性字掩码调用 `apply_scalar_table`。
 *          It calls `apply_scalar_table` with the first-word mask.
 *
 * @param[in,out] dst 接收拷贝的目标样式 / Target style that receives the copies
 * @param[in]     src 源样式 / Source style
 * @param[in]     s 第一属性字的位掩码 / Bit mask of the first property word
 *
 * @see apply_scalar_table apply_second_word_scalars
 */
static void apply_first_word_scalars(HpeStyle *dst, const HpeStyle *src, uint64_t s)
{
    apply_scalar_table(dst, src, s, k_scalars_word1,
                       sizeof(k_scalars_word1) / sizeof(k_scalars_word1[0]));
}

/**
 * @brief 拷贝第二属性字中的标量长属性 / Copy the scalar longhands carried in the second property word
 *
 * @details 以第二属性字掩码调用 `apply_scalar_table`。
 *          It calls `apply_scalar_table` with the second-word mask.
 *
 * @param[in,out] dst 接收拷贝的目标样式 / Target style that receives the copies
 * @param[in]     src 源样式 / Source style
 * @param[in]     s2 第二属性字的位掩码 / Bit mask of the second property word
 *
 * @see apply_scalar_table apply_first_word_scalars
 */
static void apply_second_word_scalars(HpeStyle *dst, const HpeStyle *src, uint64_t s2)
{
    apply_scalar_table(dst, src, s2, k_scalars_word2,
                       sizeof(k_scalars_word2) / sizeof(k_scalars_word2[0]));
}

/**
 * @brief 解析并拷贝四边边框颜色 / Resolve and copy the per-side border colours
 *
 * @details 逐边优先取该侧颜色位对应的值；若该侧未声明但简写颜色位存在，则用简写颜色兜底，使 `border-color` 仍能画出四边。
 *          Each edge prefers the value of its own colour bit and falls back to the shorthand colour when that edge was not declared but the shorthand bit is set, so a `border-color` still paints all four sides.
 *
 * @param[in,out] dst 接收颜色拷贝的目标样式 / Target style that receives the copies
 * @param[in]     src 源样式 / Source style
 * @param[in]     s 第一属性字的位掩码 / Bit mask of the first property word
 * @param[in]     s2 第二属性字的位掩码 / Bit mask of the second property word
 *
 * @see apply_decl_masked
 */
static void apply_border_color_resolved(HpeStyle *dst, const HpeStyle *src, uint64_t s,
                                        uint64_t s2)
{
    int i;

    for (i = 0; i < 4; i++) {
        uint64_t bit = HPE_PROP2_BORDER_TOP_COLOR << i;
        if (s2 & bit)
            dst->border_colors[i] = src->border_colors[i];
        else if (s & HPE_PROP_BORDER_COLOR)
            dst->border_colors[i] = src->border_color;
    }
}

/**
 * @brief 按掩码把一条声明应用到样式 / Apply one declaration onto a style, restricted to the given masks
 *
 * @details 先取出 `src->set & mask` 与 `src->set2 & mask2` 并把它们并入目标的“已声明”位（供后续阶段区分显式值与继承值），再依次拷贝四边、第一属性字标量、第二属性字标量、四边边框颜色；若没有任何单边颜色位而简写颜色位在掩码内，则四边统一用简写颜色。
 *          It first takes `src->set & mask` and `src->set2 & mask2` and folds them into the destination's "declared" bits (so later stages can tell explicit from inherited values), then copies the box edges, the first-word scalars, the second-word scalars and the per-side border colours; when no per-side colour bit is present but the shorthand colour bit is inside the mask, all four edges take the shorthand colour.
 *
 * @param[in,out] dst 目标样式 / Target style
 * @param[in]     src 源声明 / Source declaration
 * @param[in]     mask 第一属性字的生效掩码 / Mask applied to the first property word
 * @param[in]     mask2 第二属性字的生效掩码 / Mask applied to the second property word
 *
 * @see apply_decl apply_decl_important
 */
static void apply_decl_masked(HpeStyle *dst, const HpeStyle *src, uint64_t mask,
                              uint64_t mask2)
{
    uint64_t s = src->set & mask;
    uint64_t s2 = src->set2 & mask2;
    int any_side;

    /* the computed style keeps the "declared" bits so consumers (e.g. paint's
     * list-style-type) can tell an explicit value from an inherited one. */
    dst->set |= s;
    dst->set2 |= s2;

    apply_box_edges(dst, src, s);
    apply_first_word_scalars(dst, src, s);
    apply_second_word_scalars(dst, src, s2);

    any_side = (s2 & (HPE_PROP2_BORDER_TOP_COLOR | HPE_PROP2_BORDER_RIGHT_COLOR |
                      HPE_PROP2_BORDER_BOTTOM_COLOR | HPE_PROP2_BORDER_LEFT_COLOR)) != 0;
    apply_border_color_resolved(dst, src, s, s2);
    if (!any_side && (s & HPE_PROP_BORDER_COLOR)) {
        int i;
        for (i = 0; i < 4; i++)
            dst->border_colors[i] = src->border_color;
    }
}

/**
 * @brief 把一条声明的全部属性应用到样式 / Apply every property of one declaration onto a style
 *
 * @details 以全 1 掩码调用 `apply_decl_masked`，即不加筛选地应用所有已声明属性。
 *          It calls `apply_decl_masked` with all-ones masks, applying every declared property without filtering.
 *
 * @param[in,out] dst 目标样式 / Target style
 * @param[in]     src 源声明 / Source declaration
 *
 * @see apply_decl_masked apply_decl_important
 */
static void apply_decl(HpeStyle *dst, const HpeStyle *src)
{
    apply_decl_masked(dst, src, ~(uint64_t)0, ~(uint64_t)0);
}

/**
 * @brief 只应用一条声明中被标为 `!important` 的属性 / Apply only the `!important` properties of one declaration
 *
 * @details 以该声明的 `imp`/`imp2` 掩码调用 `apply_decl_masked`。
 *          It calls `apply_decl_masked` with the declaration's `imp`/`imp2` masks.
 *
 * @param[in,out] dst 目标样式 / Target style
 * @param[in]     src 源声明 / Source declaration
 *
 * @see apply_decl_masked apply_important_pass
 */
static void apply_decl_important(HpeStyle *dst, const HpeStyle *src)
{
    apply_decl_masked(dst, src, src->imp, src->imp2);
}

typedef struct {
    int specificity;
    int order;
    const HpeStyle *decl;
} HpeMatch;

/* one important declaration is applied in a second pass; remember its origin
 * (inline style attribute beats a stylesheet) and its cascade rank so
 * specificity + source order still decide within the important origin. */
typedef struct {
    const HpeStyle *decl;
    int specificity;
    int order;
    int inline_style;
} HpeImpMatch;

/**
 * @brief 从父样式拷贝可继承属性 / Copy the inheritable properties from the parent style
 *
 * @details 把颜色、字号、字重、文本对齐、行高、空白处理、字体族、可见性、光标、列表类型与位置以及字/词间距、文本缩进等可继承属性从 `parent` 复制到当前样式，作为层叠的起点。
 *          Inheritable properties such as colour, font size, font weight, text alignment, line height, white-space, family, visibility, cursor, list type and position, letter/word spacing and text indent are copied from `parent` to seed the cascade.
 *
 * @param[in,out] style 目标样式 / Target style
 * @param[in]     parent 父元素样式 / Parent style
 *
 * @see hpe_css_compute
 */
static void inherit_parent_style(HpeStyle *style, const HpeStyle *parent)
{
    style->color = parent->color;
    style->font_size = parent->font_size;
    style->font_weight = parent->font_weight;
    style->text_align = parent->text_align;
    style->line_height = parent->line_height;
    style->white_space = parent->white_space;
    style->family = parent->family;
    style->visibility = parent->visibility;
    style->cursor = parent->cursor;
    style->list_style_type = parent->list_style_type;
    style->list_style_position = parent->list_style_position;
    style->letter_spacing = parent->letter_spacing;
    style->word_spacing = parent->word_spacing;
    style->text_indent = parent->text_indent;
}

/**
 * @brief 判断规则的全部外层 `@media` 条件是否都命中视口 / Test whether every enclosing `@media` condition of a rule matches the viewport
 *
 * @details 沿规则保存的媒体查询链逐层用视口宽高求值，任一层不匹配即返回 0；无条件的规则（深度 0）直接返回 1。
 *          The rule's stored query chain is evaluated layer by layer against the viewport size and any failing layer returns 0; an unconditional rule (depth 0) returns 1 immediately.
 *
 * @param[in]     rule CSS 规则 / CSS rule
 * @param[in]     sheet 提供视口尺寸的样式表 / Stylesheet that supplies the viewport size
 *
 * @return 全部命中返回 1，否则返回 0 / 1 when all match, 0 otherwise
 *
 * @see media_alt_matches collect_matches
 */
static int rule_media_ok(const HpeRule *rule, const HpeStylesheet *sheet)
{
    int k;
    for (k = 0; k < rule->mq_depth; k++) {
        if (!media_alt_matches(&rule->mq[k], sheet->vw, sheet->vh))
            return 0;
    }
    return 1;
}

/**
 * @brief 返回规则中匹配该节点的最高特异性 / Return the highest specificity among a rule's selectors that match the node
 *
 * @details 遍历规则的选择器，仅统计匹配节点者，取其特异性最大值；无任何选择器匹配时返回 -1。
 *          It walks the rule's selectors, considering only those matching the node, and takes the maximum specificity; -1 is returned when none matches.
 *
 * @param[in]     rule CSS 规则 / CSS rule
 * @param[in]     node 待测试的 DOM 节点 / DOM node to test
 *
 * @return 最高特异性，或无匹配时的 -1 / The highest specificity, or -1 when none matches
 *
 * @see selector_matches collect_matches
 */
static int rule_best_specificity(const HpeRule *rule, const HpeDomNode *node)
{
    int best = -1;
    int j;
    for (j = 0; j < rule->selector_count; j++) {
        if (selector_matches(&rule->selectors[j], node) &&
            rule->selectors[j].specificity > best)
            best = rule->selectors[j].specificity;
    }
    return best;
}

/**
 * @brief 收集所有匹配节点的样式表规则 / Collect every stylesheet rule matching a node
 *
 * @details 遍历样式表规则，跳过媒体条件未命中的规则，对命中的记录其最高特异性、源码顺序与声明指针；匹配数组按需扩容。
 *          The sheet's rules are walked, media conditions that fail are skipped, and each hit records its best specificity, source order and declaration pointer; the match array grows on demand.
 *
 * @param[in]     sheet 样式表 / Stylesheet
 * @param[in]     node 待计算样式的 DOM 节点 / DOM node being styled
 * @param[in,out] matches 匹配结果数组 / Match result array
 * @param[in,out] count 已收集的匹配个数 / Number of matches collected so far
 * @param[in,out] cap 匹配数组当前容量 / Current capacity of the match array
 *
 * @see rule_media_ok rule_best_specificity hpe_css_compute
 */
static void collect_matches(const HpeStylesheet *sheet, const HpeDomNode *node,
                            HpeMatch **matches, size_t *count, size_t *cap)
{
    int i;

    for (i = 0; i < sheet->rule_count; i++) {
        const HpeRule *rule = &sheet->rules[i];
        int best;

        if (!rule_media_ok(rule, sheet))
            continue;
        best = rule_best_specificity(rule, node);
        if (best < 0)
            continue;
        *matches = (HpeMatch *)hpe_grow(*matches, cap, *count + 1, sizeof(HpeMatch));
        (*matches)[*count].specificity = best;
        (*matches)[*count].order = rule->order;
        (*matches)[*count].decl = rule->decls;
        (*count)++;
    }
}

/**
 * @brief 按（特异性, 源码顺序）稳定排序匹配项 / Stable sort the matches by (specificity, source order)
 *
 * @details 用插入排序按键升序排列，特异性相同时按源码顺序，稳定排序保证同键项的相对顺序不变，从而使后写的规则覆盖先写的。
 *          An insertion sort orders the entries ascending by key with source order breaking ties; being stable it keeps equal-key entries in their original relative order, so later rules override earlier ones.
 *
 * @param[in,out] m 待排序的匹配数组 / Match array to sort
 * @param[in]     n 数组元素个数 / Number of entries in the array
 *
 * @see apply_normal_declarations
 */
static void sort_matches(HpeMatch *m, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        HpeMatch key = m[i];
        j = i - 1;
        while (j >= 0 && (m[j].specificity > key.specificity ||
                          (m[j].specificity == key.specificity && m[j].order > key.order))) {
            m[j + 1] = m[j];
            j--;
        }
        m[j + 1] = key;
    }
}

/**
 * @brief 按（特异性, 源码顺序）稳定排序 important 匹配项 / Stable sort the important matches by (specificity, source order)
 *
 * @details 与 `sort_matches` 相同的键与稳定插入排序，作用于 `HpeImpMatch` 数组。
 *          It uses the same key and stable insertion sort as `sort_matches`, applied to the `HpeImpMatch` array.
 *
 * @param[in,out] m 待排序的 important 匹配数组 / Important match array to sort
 * @param[in]     n 数组元素个数 / Number of entries in the array
 *
 * @see apply_important_pass
 */
static void sort_imps(HpeImpMatch *m, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        HpeImpMatch key = m[i];
        j = i - 1;
        while (j >= 0 && (m[j].specificity > key.specificity ||
                          (m[j].specificity == key.specificity && m[j].order > key.order))) {
            m[j + 1] = m[j];
            j--;
        }
        m[j + 1] = key;
    }
}

/**
 * @brief 解析元素的内联 `style` 属性 / Parse the element's inline `style` attribute
 *
 * @details 非元素节点或无 `style` 属性时返回 0；否则把属性文本交给 `hpe_css_parse_declarations` 解析进 `out` 并返回 1。
 *          A non-element node or a missing `style` attribute returns 0; otherwise the attribute text is parsed into `out` by `hpe_css_parse_declarations` and 1 is returned.
 *
 * @param[in]     node 目标 DOM 节点 / Target DOM node
 * @param[out]    out 接收解析出的样式 / Receives the parsed style
 *
 * @return 存在非空 `style` 属性返回 1，否则返回 0 / 1 when a non-empty `style` attribute exists, 0 otherwise
 *
 * @see hpe_css_parse_declarations apply_normal_declarations
 */
static int parse_inline_style(const HpeDomNode *node, HpeStyle *out)
{
    const char *inline_style;

    if (!node || node->type != HPE_NODE_ELEMENT)
        return 0;
    inline_style = hpe_dom_attr(node, "style");
    if (!inline_style || !*inline_style)
        return 0;
    hpe_css_parse_declarations(inline_style, strlen(inline_style), out);
    return 1;
}

/**
 * @brief 应用普通声明：先规则后内联样式 / Apply the normal declarations: matching rules first, then the inline style
 *
 * @details 按排序后的顺序逐条应用样式表声明，其后若存在内联 `style` 属性再应用一次，从而内联样式胜过任何普通样式表声明。
 *          The sorted stylesheet declarations are applied in order and the inline `style` attribute, when present, is applied afterwards, so the inline style beats any normal stylesheet declaration.
 *
 * @param[in,out] style 目标样式 / Target style
 * @param[in]     matches 已排序的匹配数组 / The sorted match array
 * @param[in]     n 匹配个数 / Number of matches
 * @param[in]     node 待计算样式的 DOM 节点 / DOM node being styled
 *
 * @see apply_decl parse_inline_style hpe_css_compute
 */
static void apply_normal_declarations(HpeStyle *style, const HpeMatch *matches, int n,
                                      const HpeDomNode *node)
{
    HpeStyle decl;
    int i;

    for (i = 0; i < n; i++)
        apply_decl(style, matches[i].decl);
    if (parse_inline_style(node, &decl))
        apply_decl(style, &decl);
}

/**
 * @brief 收集待第二趟处理的 `!important` 声明 / Gather the `!important` declarations for the second pass
 *
 * @details 逐条检查匹配规则中确实含 important 位的声明并记录其特异性与顺序；若内联 `style` 也含 important 项，则以高于任何选择器的特异性（`1<<20`）与更晚的顺序（`1<<30`）作为单独一项加入，并以 `inline_style` 标记。
 *          Each matching rule whose declaration actually carries important bits is recorded with its specificity and order; when the inline `style` also has important declarations it is added as a separate entry with a specificity above any selector (`1<<20`) and a later order (`1<<30`), flagged through `inline_style`.
 *
 * @param[in]     matches 匹配数组 / Match array
 * @param[in]     n 匹配个数 / Number of matches
 * @param[in]     node 待计算样式的 DOM 节点 / DOM node being styled
 * @param[in,out] imps important 匹配列表 / `!important` match array
 * @param[in,out] count 已收集的 important 个数 / Number of important matches collected so far
 * @param[in,out] cap important 数组当前容量 / Current capacity of the important array
 *
 * @see apply_important_pass sort_imps
 */
static void collect_imps(const HpeMatch *matches, int n, const HpeDomNode *node,
                         HpeImpMatch **imps, size_t *count, size_t *cap)
{
    int i;

    for (i = 0; i < n; i++) {
        const HpeStyle *d = matches[i].decl;
        if (!d->imp && !d->imp2)
            continue;
        *imps = (HpeImpMatch *)hpe_grow(*imps, cap, *count + 1, sizeof(HpeImpMatch));
        (*imps)[*count].decl = d;
        (*imps)[*count].specificity = matches[i].specificity;
        (*imps)[*count].order = matches[i].order;
        (*imps)[*count].inline_style = 0;
        (*count)++;
    }
    {
        HpeStyle decl;
        if (parse_inline_style(node, &decl) && (decl.imp || decl.imp2)) {
            *imps = (HpeImpMatch *)hpe_grow(*imps, cap, *count + 1, sizeof(HpeImpMatch));
            (*imps)[*count].decl = (const HpeStyle *)hpe_dom_attr(node, "style"); /* marker */
            (*imps)[*count].specificity = (1 << 20); /* higher than any selector */
            (*imps)[*count].order = 1 << 30;         /* and later than any rule */
            (*imps)[*count].inline_style = 1;
            (*count)++;
        }
    }
}

/**
 * @brief 按排序结果应用 important 声明 / Apply the sorted important declarations
 *
 * @details 按顺序逐项处理：内联项重新解析 `style` 属性后只应用其 important 位，规则项直接调用 `apply_decl_important`，因此 important 声明整体覆盖所有普通声明。
 *          The entries are handled in order: an inline entry re-parses the `style` attribute and applies only its important bits while a rule entry goes straight through `apply_decl_important`, so important declarations override every normal one.
 *
 * @param[in,out] style 目标样式 / Target style
 * @param[in]     imps 已排序的 important 匹配数组 / The sorted important match array
 * @param[in]     n important 个数 / Number of important matches
 * @param[in]     node 待计算样式的 DOM 节点 / DOM node being styled
 *
 * @see apply_decl_important collect_imps hpe_css_compute
 */
static void apply_important_pass(HpeStyle *style, const HpeImpMatch *imps, int n,
                                 const HpeDomNode *node)
{
    int i;
    for (i = 0; i < n; i++) {
        if (imps[i].inline_style) {
            HpeStyle decl;
            parse_inline_style(node, &decl);
            apply_decl_important(style, &decl);
        } else {
            apply_decl_important(style, imps[i].decl);
        }
    }
}

/**
 * @brief 计算节点的最终样式（两趟层叠）/ Compute a node's final style with a two-pass cascade
 *
 * @details 第一趟从 `hpe_style_default` 出发，按 `parent` 继承可继承属性，收集所有媒体条件通过且选择器匹配的规则，按（特异性, 源码顺序）排序后依次应用，最后叠加内联 `style=""`（胜过普通样式表声明）。
 *          Pass one starts from `hpe_style_default`, inherits the inheritable properties from `parent`, gathers every rule whose media conditions pass and whose selectors match, applies them sorted by (specificity, source order), and finally overlays the inline `style=""`, which beats a normal stylesheet declaration.
 *          第二趟只处理 `!important`：把（含内联的）important 声明按同一种键排序后只应用被标记为 important 的位；在 important 层内内联仍胜过样式表，其余由源码顺序决定。
 *          Pass two handles `!important` only: the important declarations, inline ones included, are sorted by the same key and only the bits flagged important are applied; within the important origin an inline `style=""` still beats a stylesheet and otherwise source order decides.
 *
 * @param[in,out] sheet 样式表；为 `NULL` 时跳过规则匹配 / Stylesheet; rule matching is skipped when `NULL`
 * @param[in]     node 待计算样式的 DOM 节点 / DOM node whose style is computed
 * @param[in]     parent 父节点样式，用于继承；可为 `NULL` / Parent style used for inheritance; may be `NULL`
 * @param[out]    out 接收计算后的样式 / Receives the computed style
 *
 * @note 计算得到的样式保留“已声明”位（`set`/`set2`），供绘制阶段区分显式值与继承值 / The computed style keeps the "declared" bits (`set`/`set2`) so later stages can tell an explicit value from an inherited one
 *
 * @see collect_matches apply_normal_declarations collect_imps apply_important_pass
 */
/**
 * @brief 选择器末段是否带有指定伪元素 / Whether a selector's final compound carries the given pseudo-element
 *
 * @param[in]     sel 选择器 / Selector
 * @param[in]     which `HPE_PE_BEFORE` 或 `HPE_PE_AFTER` / `HPE_PE_BEFORE` or `HPE_PE_AFTER`
 *
 * @return 末段带该伪元素返回 1 / 1 when the subject compound carries it
 *
 * @see hpe_css_compute_pseudo
 */
static int selector_has_pseudo_element(const HpeSelector *sel, int which)
{
    if (sel->count <= 0)
        return 0;
    return sel->parts[sel->count - 1].pseudo_element == which;
}

/**
 * @brief 收集命中指定伪元素的规则 / Collect the rules whose selector matches the given pseudo-element
 *
 * @details 只考虑末段带同向伪元素、媒体条件通过、且以宿主元素（末段去伪元素后）匹配的选择器；特异性取命中中的最高者。
 *          Only selectors whose final compound carries the matching pseudo-element, whose media conditions pass and whose host part matches count; the specificity is the highest among the hits.
 *
 * @param[in]     sheet 样式表 / Stylesheet
 * @param[in]     node 宿主元素 / Host element
 * @param[in]     which 目标伪元素 / Target pseudo-element
 * @param[in,out] matches 匹配数组（按需扩容）/ Match array, grown on demand
 * @param[out]    count 载荷数量 / Payload count
 * @param[in,out] cap 数组容量 / Array capacity
 *
 * @see hpe_css_compute_pseudo
 */
static void collect_pseudo_matches(const HpeStylesheet *sheet, const HpeDomNode *node, int which,
                                   HpeMatch **matches, size_t *count, size_t *cap)
{
    int i, j;
    for (i = 0; i < sheet->rule_count; i++) {
        const HpeRule *rule = &sheet->rules[i];
        int best = -1;
        if (!rule_media_ok(rule, sheet))
            continue;
        for (j = 0; j < rule->selector_count; j++) {
            const HpeSelector *sel = &rule->selectors[j];
            if (!selector_has_pseudo_element(sel, which))
                continue;
            if (!selector_matches(sel, node))
                continue;
            if (sel->specificity > best)
                best = sel->specificity;
        }
        if (best < 0)
            continue;
        *matches = (HpeMatch *)hpe_grow(*matches, cap, *count + 1, sizeof(HpeMatch));
        (*matches)[*count].specificity = best;
        (*matches)[*count].order = rule->order;
        (*matches)[*count].decl = rule->decls;
        (*count)++;
    }
}

void hpe_css_compute_pseudo(HpeStylesheet *sheet, const HpeDomNode *node, const HpeStyle *host_style,
                            int which, HpeStyle *out)
{
    HpeMatch *matches = NULL;
    size_t match_count = 0, match_cap = 0;
    HpeStyle style;

    style = hpe_style_default();
    if (host_style) {
        /* generated content inherits from its host element, exactly like the
         * host's own children do */
        inherit_parent_style(&style, host_style);
        style.color = host_style->color;
        style.font_size = host_style->font_size;
        style.font_weight = host_style->font_weight;
        style.family = host_style->family;
        style.line_height = host_style->line_height;
        style.white_space = host_style->white_space;
        style.text_align = host_style->text_align;
        style.visibility = host_style->visibility;
        style.list_style_type = host_style->list_style_type;
        style.list_style_position = host_style->list_style_position;
        style.letter_spacing = host_style->letter_spacing;
        style.word_spacing = host_style->word_spacing;
        style.text_indent = host_style->text_indent;
        style.cursor = host_style->cursor;
    }
    if (sheet && node)
        collect_pseudo_matches(sheet, node, which, &matches, &match_count, &match_cap);
    sort_matches(matches, (int)match_count);
    apply_normal_declarations(&style, matches, (int)match_count, NULL);
    if (matches)
        hpe_free(matches);
    *out = style;
}

void hpe_css_compute(HpeStylesheet *sheet, const HpeDomNode *node, const HpeStyle *parent,
                     HpeStyle *out)
{
    HpeMatch *matches = NULL;
    size_t match_count = 0, match_cap = 0;
    HpeImpMatch *imps = NULL;
    size_t imp_count = 0, imp_cap = 0;
    HpeStyle style;

    style = hpe_style_default();
    if (parent)
        inherit_parent_style(&style, parent);

    if (sheet && node)
        collect_matches(sheet, node, &matches, &match_count, &match_cap);
    sort_matches(matches, (int)match_count);
    apply_normal_declarations(&style, matches, (int)match_count, node);

    /* second pass: important declarations win over every normal one. Within the
     * important origin an inline `style=""` beats a stylesheet important, then
     * source order decides. */
    collect_imps(matches, (int)match_count, node, &imps, &imp_count, &imp_cap);
    sort_imps(imps, (int)imp_count);
    apply_important_pass(&style, imps, (int)imp_count, node);

    if (matches)
        hpe_free(matches);
    if (imps)
        hpe_free(imps);
    *out = style;
}

/**
 * @brief 按格式串向输出缓冲追加文本 / Append formatted text to the output buffer
 *
 * @details 缓冲已满时直接返回，否则用 `vsnprintf` 追加并累加长度；写入长度会被夹到 `out_size` 以防越界。
 *          It returns immediately when the buffer is full, otherwise appends through `vsnprintf` and accumulates the length, clamping the length to `out_size` to avoid overrun.
 *
 * @param[in,out] out 输出缓冲 / Output buffer
 * @param[in]     out_size `out` 的容量 / Capacity of `out`
 * @param[in,out] olen 已写入长度 / Length written so far
 * @param[in]     fmt printf 风格格式串 / printf-style format string
 *
 * @see hpe_css_inline_set
 */

static void inline_append(char *out, size_t out_size, size_t *olen, const char *fmt, ...)
{
    va_list ap;
    if (*olen >= out_size)
        return;
    va_start(ap, fmt);
    *olen += (size_t)vsnprintf(out + *olen, out_size - *olen, fmt, ap);
    va_end(ap);
    if (*olen > out_size)
        *olen = out_size;
}

/**
 * @brief 在元素的内联 `style` 属性中设置一个属性 / Set one property in an element's inline `style` attribute
 *
 * @details 扫描现有 `style` 属性，遇到同名属性就替换其值（值为空则删除），其余声明原样保留；没有同名属性时在末尾追加 `prop: value;`，最后整体写回属性。
 *          The existing `style` attribute is scanned; a declaration with the same name has its value replaced (or removed when the new value is empty) while other declarations are kept verbatim, and when no match exists `prop: value;` is appended before the attribute is written back.
 *
 * @param[in,out] node 目标 DOM 元素 / Target DOM element
 * @param[in]     prop CSS 属性名（小写、带连字符）/ CSS property name, lowercase and dashed
 * @param[in]     value 属性值；空串或 `NULL` 表示删除该属性 / Property value; an empty string or `NULL` removes the property
 *
 * @note 供 JavaScript DOM 桥使用 / Used by the JavaScript DOM bridge
 *
 * @see inline_append hpe_css_inline_get
 */
void hpe_css_inline_set(HpeDomNode *node, const char *prop, const char *value)
{
    char out[2048];
    size_t olen = 0;
    const char *cur = hpe_dom_attr(node, "style");
    int replaced = 0;

    if (cur) {
        const char *p = cur;
        while (*p) {
            const char *semi = strchr(p, ';');
            const char *end = semi ? semi : p + strlen(p);
            const char *colon = (const char *)memchr(p, ':', (size_t)(end - p));
            if (colon && colon > p) {
                char name[64];
                size_t nlen = (size_t)(colon - p);
                size_t i;
                if (nlen >= sizeof(name))
                    nlen = sizeof(name) - 1;
                for (i = 0; i < nlen; i++)
                    name[i] = (char)hpe_ascii_lower((unsigned char)p[i]);
                name[nlen] = '\0';
                if (strcmp(name, prop) == 0) {
                    if (!replaced && value && *value)
                        inline_append(out, sizeof(out), &olen, "%s: %s; ", prop, value);
                    replaced = 1;
                } else {
                    char pair[512];
                    size_t plen = (size_t)(end - p);
                    if (plen >= sizeof(pair))
                        plen = sizeof(pair) - 1;
                    memcpy(pair, p, plen);
                    pair[plen] = '\0';
                    inline_append(out, sizeof(out), &olen, "%s; ", hpe_trim(pair));
                }
            }
            p = semi ? semi + 1 : end;
        }
    }
    if (!replaced && value && *value)
        inline_append(out, sizeof(out), &olen, "%s: %s;", prop, value);
    out[olen < sizeof(out) ? olen : sizeof(out) - 1] = '\0';
    hpe_dom_set_attr(node, "style", out);
}

/**
 * @brief 读取元素内联 `style` 属性中的一个属性值 / Read one property value from an element's inline `style` attribute
 *
 * @details 按 `;` 拆分内联样式，逐个以大小写不敏感的方式比较属性名，命中后把去除首尾空白的值写入 `out` 并返回 1。
 *          The inline style is split on `;` and property names are compared case-insensitively; on a hit the trimmed value is written to `out` and 1 is returned.
 *
 * @param[in]     node 目标 DOM 元素 / Target DOM element
 * @param[in]     prop 要查询的 CSS 属性名 / CSS property name to look up
 * @param[out]    out 接收属性值的缓冲区 / Buffer that receives the property value
 * @param[in]     out_size 接收缓冲区容量（字节）/ Capacity of the output buffer in bytes
 *
 * @return 找到返回 1，否则返回 0 / 1 when found, 0 otherwise
 *
 * @note 属性值以去空白后的文本原样返回，不做规范化 / The value is returned as trimmed text without normalisation
 *
 * @see hpe_css_inline_set
 */
int hpe_css_inline_get(const HpeDomNode *node, const char *prop, char *out, size_t out_size)
{
    const char *cur = hpe_dom_attr(node, "style");
    if (!cur)
        return 0;
    {
        const char *p = cur;
        while (*p) {
            const char *semi = strchr(p, ';');
            const char *end = semi ? semi : p + strlen(p);
            const char *colon = (const char *)memchr(p, ':', (size_t)(end - p));
            if (colon && colon > p) {
                char name[64];
                size_t nlen = (size_t)(colon - p);
                size_t i;
                if (nlen >= sizeof(name))
                    nlen = sizeof(name) - 1;
                for (i = 0; i < nlen; i++)
                    name[i] = (char)hpe_ascii_lower((unsigned char)p[i]);
                name[nlen] = '\0';
                if (strcmp(name, prop) == 0) {
                    char val[512];
                    size_t vlen = (size_t)(end - colon - 1);
                    if (vlen >= sizeof(val))
                        vlen = sizeof(val) - 1;
                    memcpy(val, colon + 1, vlen);
                    val[vlen] = '\0';
                    snprintf(out, out_size, "%s", hpe_trim(val));
                    return 1;
                }
            }
            p = semi ? semi + 1 : end;
        }
    }
    return 0;
}

/**
 * @brief 返回内置的用户代理默认样式表文本 / Return the built-in user-agent stylesheet text
 *
 * @details 这份常量文本为 `html`/`body`、标题、列表、表格、表单控件等元素提供 `display`、`margin`、`font-size` 等浏览器默认值，在 `hpe_css_init` 时被解析进样式表。
 *          This constant text supplies browser-default `display`, `margin`, `font-size` and similar values for `html`/`body`, headings, lists, tables and form controls; it is parsed into the sheet by `hpe_css_init`.
 *
 * @return 以 NUL 结尾的 UA 样式表 CSS 文本 / NUL-terminated CSS text of the user-agent stylesheet
 *
 * @see hpe_css_init
 */

const char *hpe_ua_stylesheet(void)
{
    return
        "html { display: block; }\n"
        "body { display: block; margin: 8px; }\n"
        "head, script, style, title, meta, link, base, noscript, template { display: none; }\n"
        "div, p, section, article, header, footer, main, nav, aside, ul, ol, li, dl, dt, dd,\n"
        "blockquote, pre, form, fieldset, figure,\n"
        "figcaption, hr, address, center { display: block; }\n"
        "table { display: table; border-spacing: 0; border-color: #c0c0c0; }\n"
        "thead, tbody, tfoot { display: table-row-group; }\n"
        "tr { display: table-row; }\n"
        "td, th { display: table-cell; }\n"
        "caption { display: table-caption; }\n"
        "th { font-weight: bold; text-align: center; }\n"
        "span, a, b, i, em, strong, code, small, sub, sup, label, br, img, u, s, mark\n"
        "  { display: inline; }\n"
        "button, input, select, textarea { display: inline-block; font-size: 13px; text-align: center; }\n"
        "button { padding: 1px 6px; border: 1px solid #767676; background: #efefef; }\n"
        "h1 { display: block; font-size: 32px; font-weight: bold; margin: 21px 0; }\n"
        "h2 { display: block; font-size: 24px; font-weight: bold; margin: 19px 0; }\n"
        "h3 { display: block; font-size: 19px; font-weight: bold; margin: 18px 0; }\n"
        "h4 { display: block; font-size: 16px; font-weight: bold; margin: 21px 0; }\n"
        "h5 { display: block; font-size: 13px; font-weight: bold; margin: 22px 0; }\n"
        "h6 { display: block; font-size: 11px; font-weight: bold; margin: 24px 0; }\n"
        "p { margin: 16px 0; }\n"
        "ul, ol { margin: 16px 0; padding-left: 40px; }\n"
        "blockquote { margin: 16px 40px; }\n"
        "pre { font-size: 13px; margin: 13px 0; }\n"
        "code { font-size: 13px; }\n"
        "b, strong { font-weight: bold; }\n"
        "a { color: #1a0dab; cursor: pointer; }\n"
        "button, input, select, textarea { cursor: default; }\n"
        "hr { border: 1px solid #c0c0c0; height: 0; margin: 8px 0; }\n";
}
