/* Hope UI Engine - CSS transition / animation parsing and interpolation
 *
 * Declaration parsing (transition-* / animation-* shorthands and longhands),
 * @keyframes lookup on the stylesheet and the value get/set/lerp primitives
 * the engine's animation clock (hope_anim.c) is built on.  Data types live in
 * hpe_css.h next to HpeStyle.
 */
#ifndef HPE_ANIM_H
#define HPE_ANIM_H

#include "hpe_css.h"

/* ---------- declaration parsing ---------- */

/**
 * @brief 解析 transition 简写 / Parse the `transition` shorthand
 *
 * @details 按顶层逗号切分列表，逐项扫描 token：时间 token 第一个为 duration、第二个为
 *          delay；缓动关键字（含 `cubic-bezier(...)` / `steps(...)` 括号组）为 timing
 *          function；可插值属性名为 property；`none` 清空列表。每项写入
 *          `d->transitions` 并置位 `HPE_PROP2_TRANSITION`。
 *          The list is split on top-level commas; per item the first time token is the
 *          duration and the second the delay, easing keywords (including the
 *          `cubic-bezier(...)` / `steps(...)` parenthesised groups) set the timing
 *          function and an interpolable property name sets the property. `none` clears
 *          the list. Each item is stored in `d->transitions` and
 *          `HPE_PROP2_TRANSITION` is set.
 *
 * @param[in]     value 声明值 / Declaration value
 * @param[in,out] d 目标样式 / Target style
 *
 * @see hpe_anim_parse_transition_longhand
 */
void hpe_anim_parse_transition_shorthand(const char *value, HpeStyle *d);

/**
 * @brief 解析 transition 长属性 / Parse a `transition-*` longhand
 *
 * @details `prop` 取 "transition-property" / "transition-duration" /
 *          "transition-timing-function" / "transition-delay" 之一，值按逗号列表写入
 *          `d->transitions[i]` 对应字段并置位 `HPE_PROP2_TRANSITION`。
 *          `prop` is one of "transition-property" / "transition-duration" /
 *          "transition-timing-function" / "transition-delay"; the comma list fills the
 *          matching field of `d->transitions[i]` and sets `HPE_PROP2_TRANSITION`.
 *
 * @param[in]     prop 长属性名 / Longhand name
 * @param[in]     value 声明值 / Declaration value
 * @param[in,out] d 目标样式 / Target style
 */
void hpe_anim_parse_transition_longhand(const char *prop, const char *value, HpeStyle *d);

/**
 * @brief 解析 animation 简写 / Parse the `animation` shorthand
 *
 * @details 按顶层逗号切分，逐项按 CSS Animations §3 顺序无关规则识别：时间 token
 *          第一个为 duration、第二个为 delay；缓动关键字为 timing function；
 *          `infinite` 或纯数字为迭代次数；direction / fill-mode 关键字各归其位；
 *          余下的标识符为 @keyframes 名。`none` 清空列表。
 *          The list is split on top-level commas; per item, in the order-free manner of
 *          CSS Animations §3: the first time token is the duration and the second the
 *          delay, easing keywords set the timing function, `infinite` or a plain number
 *          is the iteration count, direction / fill-mode keywords slot in, and the
 *          remaining identifier is the @keyframes name. `none` clears the list.
 *
 * @param[in]     value 声明值 / Declaration value
 * @param[in,out] d 目标样式 / Target style
 *
 * @see hpe_anim_parse_animation_longhand
 */
void hpe_anim_parse_animation_shorthand(const char *value, HpeStyle *d);

/**
 * @brief 解析 animation 长属性 / Parse an `animation-*` longhand
 *
 * @details `prop` 取 "animation-name" / "animation-duration" / "animation-timing-function" /
 *          "animation-delay" / "animation-iteration-count" / "animation-direction" /
 *          "animation-fill-mode" 之一，值写入 `d->anims[i]` 对应字段并置位
 *          `HPE_PROP2_ANIMATION`。
 *          `prop` is one of "animation-name" / "animation-duration" /
 *          "animation-timing-function" / "animation-delay" /
 *          "animation-iteration-count" / "animation-direction" /
 *          "animation-fill-mode"; the value fills the matching field of `d->anims[i]`
 *          and sets `HPE_PROP2_ANIMATION`.
 *
 * @param[in]     prop 长属性名 / Longhand name
 * @param[in]     value 声明值 / Declaration value
 * @param[in,out] d 目标样式 / Target style
 */
void hpe_anim_parse_animation_longhand(const char *prop, const char *value, HpeStyle *d);

/**
 * @brief 把时间值解析成秒 / Parse a CSS time value into seconds
 *
 * @details 接受 `<number>s` 与 `<number>ms`，无单位数字按秒处理；解析失败写 0。
 *          Accepts `<number>s` and `<number>ms`; a bare number counts as seconds and a
 *          failed parse writes 0.
 *
 * @param[in]  v 声明值 token / Declaration value token
 * @param[out] out 接收秒数 / Receives the seconds
 */
void hpe_anim_parse_time(const char *v, float *out);

/**
 * @brief 解析缓动函数 / Parse an easing function
 *
 * @details 关键字 `linear` / `ease` / `ease-in` / `ease-out` / `ease-in-out`、
 *          `cubic-bezier(x1, y1, x2, y2)` 与 `steps(n, start|end)`；未知值回退 `ease`。
 *          Keywords `linear` / `ease` / `ease-in` / `ease-out` / `ease-in-out`,
 *          `cubic-bezier(x1, y1, x2, y2)` and `steps(n, start|end)`; unknown values
 *          fall back to `ease`.
 *
 * @param[in]  v 声明值 token / Declaration value token
 * @param[out] out 接收解析结果 / Receives the parsed easing
 */
void hpe_anim_parse_easing(const char *v, HpeEasing *out);

/* ---------- @keyframes ---------- */

/**
 * @brief 按名查找样式表中的 @keyframes / Find @keyframes by name in a stylesheet
 *
 * @param[in] sheet 样式表 / Stylesheet
 * @param[in] name 动画名 / Animation name
 *
 * @return 命中的规则，未找到返回 `NULL` / The matching rule, `NULL` when absent
 */
HpeKeyframes *hpe_anim_find_keyframes(HpeStylesheet *sheet, const char *name);

/**
 * @brief 按名找（或新建）一条空的 @keyframes / Find or create an empty @keyframes slot
 *
 * @details 已有同名规则时清空其 stops（后定义整体替换先定义）；否则追加一条空规则。
 *          An existing rule with the same name has its stops cleared (a later
 *          definition replaces an earlier one wholesale); otherwise an empty rule is
 *          appended.
 *
 * @param[in,out] sheet 目标样式表 / Target stylesheet
 * @param[in]     name 动画名（不含空白）/ Animation name (no whitespace)
 *
 * @return 命中的规则；容量耗尽返回 `NULL` / The rule, or `NULL` when capacity runs out
 */
HpeKeyframes *hpe_anim_sheet_keyframes_slot(HpeStylesheet *sheet, const char *name);

/* ---------- value primitives ---------- */

/**
 * @brief 从计算样式取一个可插值属性的当前值 / Extract one animatable property's current value
 *
 * @param[in]  s 计算样式 / Computed style
 * @param[in]  prop `HpeAnimProp` / An `HpeAnimProp` id
 * @param[out] out 接收值快照 / Receives the value snapshot
 *
 * @return 属性可参与插值返回 1（`out->valid` 同步置位），否则 0 / 1 when the property holds
 *         an interpolable value (`out->valid` set accordingly), 0 otherwise
 */
int hpe_anim_value_get(const HpeStyle *s, int prop, HpeAnimValue *out);

/**
 * @brief 把插值结果写回计算样式 / Write an interpolated value back into a computed style
 *
 * @param[in,out] s 目标样式 / Target style
 * @param[in]     prop `HpeAnimProp` / An `HpeAnimProp` id
 * @param[in]     v 值快照 / The value snapshot
 */
void hpe_anim_value_set(HpeStyle *s, int prop, const HpeAnimValue *v);

/**
 * @brief 在两个值之间插值 / Interpolate between two values
 *
 * @details 同类且单位一致时线性插值；颜色按 sRGB 通道插值；transform 做分解
 *          （平移/旋转/缩放）插值，含倾斜或行列式退化时按 50% 离散切换。
 *          Linear interpolation when both sides share type and unit; colours lerp per
 *          sRGB channel; transforms interpolate as decomposed
 *          (translation / rotation / scale) and fall back to a 50% discrete swap when
 *          skew is present or the determinant degenerates.
 *
 * @param[in]  a 起点值 / Start value
 * @param[in]  b 终点值 / End value
 * @param[in]  t 进度 0..1 / Progress 0 to 1
 * @param[out] out 接收插值结果 / Receives the interpolated value
 *
 * @return 可插值返回 1，类型不兼容返回 0（调用方应走离散） / 1 when interpolable, 0 when
 *         the types are incompatible (the caller should fall back to discrete)
 */
int hpe_anim_value_lerp(const HpeAnimValue *a, const HpeAnimValue *b, float t, HpeAnimValue *out);

/**
 * @brief 求缓动函数在 t 处的进度 / Evaluate an easing function at t
 *
 * @param[in] e 缓动函数 / Easing function
 * @param[in] t 归一化时间 0..1 / Normalised time 0 to 1
 *
 * @return 缓动后的进度（通常仍在 0..1，bezier 允许越界）/ The eased progress (usually
 *         still 0 to 1; a bezier may overshoot)
 */
float hpe_easing_solve(const HpeEasing *e, float t);

/**
 * @brief 判断属性是否影响几何（决定动画刷新级别）/ Test whether a property affects geometry
 *
 * @param[in] prop `HpeAnimProp` / An `HpeAnimProp` id
 *
 * @return 需要重新布局返回 1，仅需重算样式返回 0 / 1 when a relayout is required, 0 when a
 *         style-only restyle suffices
 */
int hpe_anim_prop_is_layout(int prop);

#endif /* HPE_ANIM_H */
