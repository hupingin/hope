/* Hope UI Engine - engine-side CSS transition / animation clock
 *
 * Owns the interpolation clock and the per-node runtime state. The css module
 * calls back into anim_apply after every element cascade (hpe_css_set_anim_hook)
 * so interpolated values overlay the freshly computed style; hope.c advances the
 * clock once per engine tick and flushes while anything is in flight.
 */
#include "hope_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "css/hpe_anim.h"
#include "platform/hpe_platform.h"

static void anim_apply(void *ud, const HpeDomNode *node, HpeStyle *style);

/**
 * @brief 把可插值属性 id 映射到「已声明」位 / Map an animatable property id to its "declared" bit
 *
 * @param[in] prop `HpeAnimProp` / An `HpeAnimProp` id
 * @param[out] word2 置 1 表示位在 `set2` 中 / Set to 1 when the bit lives in `set2`
 *
 * @return 属性对应的 `HPE_PROP*` 位；无对应位返回 0 / The matching `HPE_PROP*` bit, 0 when none
 *
 * @note keyframe 停靠点用它判断「该停靠点是否声明了此属性」/ Keyframe stops use this to
 *       tell "declares the property" from "carries the default"
 */
static uint64_t anim_prop_bit(int prop, int *word2)
{
    *word2 = 0;
    switch (prop) {
    case HPE_AP_COLOR: return HPE_PROP_COLOR;
    case HPE_AP_BACKGROUND_COLOR: return HPE_PROP_BACKGROUND;
    case HPE_AP_BORDER_COLOR: return HPE_PROP_BORDER_COLOR;
    case HPE_AP_OPACITY: *word2 = 1; return HPE_PROP2_OPACITY;
    case HPE_AP_WIDTH: return HPE_PROP_WIDTH;
    case HPE_AP_HEIGHT: return HPE_PROP_HEIGHT;
    case HPE_AP_FONT_SIZE: return HPE_PROP_FONT_SIZE;
    case HPE_AP_LETTER_SPACING: *word2 = 1; return HPE_PROP2_LETTER_SPACING;
    case HPE_AP_LINE_HEIGHT: return HPE_PROP_LINE_HEIGHT;
    case HPE_AP_FONT_WEIGHT: return HPE_PROP_FONT_WEIGHT;
    case HPE_AP_BORDER_RADIUS: return HPE_PROP_BORDER_RADIUS;
    case HPE_AP_TRANSFORM: *word2 = 1; return HPE_PROP2_TRANSFORM;
    default: break;
    }
    if (prop >= HPE_AP_MARGIN_TOP && prop <= HPE_AP_MARGIN_LEFT)
        return HPE_PROP_MARGIN_TOP << (prop - HPE_AP_MARGIN_TOP);
    if (prop >= HPE_AP_PADDING_TOP && prop <= HPE_AP_PADDING_LEFT)
        return HPE_PROP_PADDING_TOP << (prop - HPE_AP_PADDING_TOP);
    if (prop >= HPE_AP_INSET_TOP && prop <= HPE_AP_INSET_LEFT)
        return HPE_PROP_INSET_TOP << (prop - HPE_AP_INSET_TOP);
    return 0;
}

/**
 * @brief 测试停靠点样式是否声明了某属性 / Test whether a stop's style declares a property
 */
static int stop_declares(const HpeStyle *s, int prop)
{
    uint64_t bit;
    int w2;
    bit = anim_prop_bit(prop, &w2);
    if (!bit)
        return 0;
    return w2 ? (s->set2 & bit) != 0 : (s->set & bit) != 0;
}

/**
 * @brief 判断方向修正后的迭代进度 / Resolve the direction-adjusted progress of an iteration
 *
 * @details normal 同向、reverse 反向；alternate 第偶数轮正向、奇数轮反向；
 *          alternate-reverse 相反。
 *          normal plays forward and reverse backward; alternate plays even iterations
 *          forward and odd backward; alternate-reverse is the opposite.
 */
static float run_progress_u(const HpeAnimDecl *decl, double local, float dur, double *iter_out)
{
    double k = dur > 0.0f ? floor(local / (double)dur) : 0.0;
    float u = dur > 0.0f ? (float)(local / (double)dur - k) : 1.0f;
    int reverse;

    if (k < 0.0)
        k = 0.0;
    *iter_out = k;
    reverse = decl->direction == HPE_ANIM_DIR_REVERSE;
    if (decl->direction == HPE_ANIM_DIR_ALTERNATE && ((long)k & 1))
        reverse = 1;
    if (decl->direction == HPE_ANIM_DIR_ALTERNATE_REVERSE && !((long)k & 1))
        reverse = 1;
    if (reverse)
        u = 1.0f - u;
    return u;
}

/* ---------------- state table ---------------- */

/**
 * @brief 查找节点的动画状态 / Find a node's animation state
 */
static HpeAnimNodeState *anim_state_find(HpeAnimWorld *w, const HpeDomNode *node)
{
    int i;
    for (i = 0; i < w->count; i++) {
        if (w->nodes[i].used && w->nodes[i].node == node)
            return &w->nodes[i];
    }
    return NULL;
}

/**
 * @brief 查找（或创建）节点的动画状态 / Find (or create) a node's animation state
 *
 * @details `create` 为 0 时不新建；表满时复用最旧的已结束条目，仍无空位则返回 `NULL`。
 *          With `create` 0 no entry is made; a full table recycles the oldest finished
 *          entry and otherwise returns `NULL`.
 */
static HpeAnimNodeState *anim_state_get(HpeEngine *engine, const HpeDomNode *node, int create)
{
    HpeAnimWorld *w = &engine->anim;
    HpeAnimNodeState *st;

    st = anim_state_find(w, node);
    if (st)
        return st;
    if (!create)
        return NULL;
    w->nodes = (HpeAnimNodeState *)hpe_grow(w->nodes, &w->cap, (size_t)w->count + 1,
                                            sizeof(HpeAnimNodeState));
    if (!w->nodes)
        return NULL;
    st = &w->nodes[w->count++];
    memset(st, 0, sizeof(*st));
    st->used = 1;
    st->node = (HpeDomNode *)node;
    return st;
}

/**
 * @brief 计算一个动画运行项的总时长 / Compute an animation run's total duration
 */
static double run_total_time(const HpeAnimRun *run)
{
    float dur = run->decl.duration;
    double iters = run->decl.iter_infinite ? 1e18 : (double)run->decl.iter_count;
    return (double)run->decl.delay + dur * iters;
}

/**
 * @brief 修剪已脱离文档的节点状态 / Prune states of nodes detached from the document
 *
 * @details 父链断开的元素（脚本移除等）状态随剪随删，避免悬空指针；文档根豁免。
 *          Elements whose parent chain broke (scripted removal) have their state freed
 *           to avoid dangling pointers; the document root is exempt.
 */
static void anim_prune_detached(HpeEngine *engine)
{
    HpeAnimWorld *w = &engine->anim;
    int i;

    for (i = 0; i < w->count; i++) {
        HpeAnimNodeState *st = &w->nodes[i];
        if (!st->used)
            continue;
        if (!st->node || (!st->node->parent && st->node != engine->doc))
            st->used = 0;
    }
}

/**
 * @brief 求值当前活动状态与所需重排级别 / Evaluate the active flag and the required reflow level
 *
 * @details 运行项在「本次推进跨越过其播放区间」时视为活动（`clock - dt` 仍早于总时长），
 *          因此结束帧恰好也能刷出一帧；几何类过渡把所需级别抬到 LAYOUT，其余活动给
 *          PAINT；几何类动画经 `layout_sticky` 在 apply 期标记（keyframes 的属性集合只有
 *          层叠时才知道），不在此处推断。
 *          A run counts as active when this advance crossed its playing interval
 *          (`clock - dt` is still before the total time), so the final frame is flushed
 *          exactly once; a geometry transition raises the need to LAYOUT and any other
 *          active item asks for PAINT; geometry animations are flagged through
 *          `layout_sticky` during apply (a keyframes rule's property set is only known at
 *          cascade time), so they are not inferred here.
 */
static void anim_evaluate_active(HpeEngine *engine, double dt)
{
    HpeAnimWorld *w = &engine->anim;
    double since = w->clock - dt;
    int i, j;
    int active = 0;
    int need = HPE_REFLOW_NONE;

    for (i = 0; i < w->count && !active; i++) {
        HpeAnimNodeState *st = &w->nodes[i];
        if (!st->used)
            continue;
        for (j = 0; j < HPE_ANIM_MAX && !active; j++) {
            if (st->runs[j].used && since < run_total_time(&st->runs[j]) - 1e-4)
                active = 1;
        }
        for (j = 0; j < HPE_TRANS_MAX && !active; j++) {
            if (st->trans[j].used &&
                since < st->trans[j].start + (double)st->trans[j].duration - 1e-4)
                active = 1;
        }
    }
    for (i = 0; i < w->count; i++) {
        HpeAnimNodeState *st = &w->nodes[i];
        if (!st->used)
            continue;
        for (j = 0; j < HPE_TRANS_MAX; j++) {
            if (st->trans[j].used && hpe_anim_prop_is_layout(st->trans[j].prop))
                need = HPE_REFLOW_LAYOUT;
        }
    }
    if (!active)
        w->layout_sticky = 0; /* the active period ended: drop the sticky request */
    w->active = active;
    if (active && need == HPE_REFLOW_NONE)
        need = HPE_REFLOW_PAINT;
    if (need > w->reflow_need || !active)
        w->reflow_need = need;
}

/* ---------------- transitions ---------------- */

/**
 * @brief 找声明中匹配某属性的 transition 项 / Find the transition entry matching a property
 *
 * @details 精确匹配优先；没有精确项时取最后一个 `all` 项。
 *          An exact match wins; without one the last `all` entry applies.
 */
static const HpeTransition *trans_for(const HpeStyle *s, int prop)
{
    const HpeTransition *all = NULL;
    int i;

    for (i = 0; i < s->transition_count && i < HPE_TRANS_MAX; i++) {
        if (s->transitions[i].prop == prop)
            return &s->transitions[i];
        if (s->transitions[i].prop == HPE_AP_ALL)
            all = &s->transitions[i];
    }
    return all;
}

/**
 * @brief 判断两个值快照是否相等 / Test whether two value snapshots are equal
 */
static int anim_value_equal(const HpeAnimValue *a, const HpeAnimValue *b)
{
    if (a->type != b->type || a->valid != b->valid)
        return 0;
    switch (a->type) {
    case HPE_AV_LEN:
        return a->len.unit == b->len.unit && a->len.value == b->len.value;
    case HPE_AV_COLOR:
        return a->color.r == b->color.r && a->color.g == b->color.g &&
               a->color.b == b->color.b && a->color.a == b->color.a;
    case HPE_AV_COLOR4:
        return memcmp(a->colors, b->colors, sizeof(a->colors)) == 0;
    case HPE_AV_FLOAT:
        return a->f == b->f;
    case HPE_AV_TRANSFORM:
        return memcmp(&a->tf, &b->tf, sizeof(a->tf)) == 0;
    default:
        return 1;
    }
}

/**
 * @brief 对照上一帧的目标值启动过渡 / Start transitions against the previous cascade's targets
 *
 * @details 逐可插值属性对比 `st->prev` 与本次目标值：有过渡声明、两侧可插值且值有变化时，
 *          在（可能为空或已结束的）槽位上记录 from/to 与起始时刻。延迟期按规范显示
 *          from 值。
 *          Each animatable property is compared between `st->prev` and this cascade's
 *          target: with a matching transition declaration, two interpolable sides and an
 *          actual change, a slot records from/to and the start time. The delay shows the
 *          from value per spec.
 */
static void anim_start_transitions(HpeEngine *engine, HpeAnimNodeState *st, const HpeStyle *style,
                                   const HpeAnimValue *target)
{
    HpeAnimWorld *w = &engine->anim;
    int prop;

    for (prop = 1; prop < HPE_AP_COUNT; prop++) {
        const HpeTransition *t = trans_for(style, prop);
        HpeAnimTransRun *run = NULL;
        HpeAnimValue probe;
        int i, slot = -1;

        if (!t)
            continue;
        if (anim_value_equal(&st->prev[prop], &target[prop]))
            continue;
        if (!hpe_anim_value_lerp(&st->prev[prop], &target[prop], 0.0f, &probe))
            continue;
        for (i = 0; i < HPE_TRANS_MAX && slot < 0; i++) {
            if (!st->trans[i].used)
                slot = i;
            else if (st->trans[i].prop == prop)
                slot = i;
        }
        if (slot < 0)
            continue;
        run = &st->trans[slot];
        run->used = 1;
        run->prop = prop;
        run->start = w->clock + (double)t->delay;
        run->duration = t->duration;
        run->easing = t->easing;
        run->from = st->prev[prop];
        run->to = target[prop];
        if (hpe_anim_prop_is_layout(prop)) {
            engine->anim.reflow_need = HPE_REFLOW_LAYOUT;
            engine->anim.layout_sticky = 1;
        } else if (engine->anim.reflow_need < HPE_REFLOW_PAINT) {
            engine->anim.reflow_need = HPE_REFLOW_PAINT;
        }
    }
}

/* ---------------- animations ---------------- */

/**
 * @brief 同步动画声明与运行项 / Sync animation declarations with runs
 *
 * @details 声明中出现的新名字开跑（start = 时钟 + 延迟）；运行项的名字不再被声明时移除。
 *          A newly declared name starts a run (start = clock + delay); a run whose name
 *          is no longer declared is dropped.
 */
static void anim_sync_runs(HpeEngine *engine, HpeAnimNodeState *st, const HpeStyle *style)
{
    HpeAnimWorld *w = &engine->anim;
    int i, j;

    for (i = 0; i < style->anim_count && i < HPE_ANIM_MAX; i++) {
        const HpeAnimDecl *decl = &style->anims[i];
        int have = 0;
        if (!decl->name[0])
            continue;
        for (j = 0; j < HPE_ANIM_MAX; j++) {
            if (st->runs[j].used && !strcmp(st->runs[j].name, decl->name))
                have = 1;
        }
        if (have)
            continue;
        for (j = 0; j < HPE_ANIM_MAX && !have; j++) {
            if (!st->runs[j].used) {
                st->runs[j].used = 1;
                snprintf(st->runs[j].name, sizeof(st->runs[j].name), "%s", decl->name);
                st->runs[j].start = w->clock + (double)decl->delay;
                st->runs[j].decl = *decl;
                have = 1;
            }
        }
    }
    for (i = 0; i < HPE_ANIM_MAX; i++) {
        int keep = 0;
        if (!st->runs[i].used)
            continue;
        for (j = 0; j < style->anim_count && j < HPE_ANIM_MAX; j++) {
            if (!strcmp(st->runs[i].name, style->anims[j].name))
                keep = 1;
        }
        if (!keep)
            st->runs[i].used = 0;
    }
}

/**
 * @brief 在停靠点中找进度两侧声明了该属性的端点 / Find the stops bracketing the progress
 *
 * @details 声明了该属性的停靠点构成时间线：下方端点取进度之前（含）最近的，上方端点取
 *          进度之后（含）最近的。
 *          Stops declaring the property form the timeline: the lower endpoint is the
 *          nearest stop at or before the progress and the upper one the nearest at or
 *          after it.
 *
 * @return 找到至少一个声明该属性的停靠点返回 1 / Returns 1 when at least one stop declares it
 */
static int kf_bounds(const HpeKeyframes *kf, int prop, float u, const HpeStyle **lo,
                     float *lo_pct, const HpeStyle **hi, float *hi_pct)
{
    int i, found = 0;

    for (i = 0; i < kf->stop_count; i++) {
        const HpeKeyframeStop *stop = &kf->stops[i];
        if (!stop->decl || !stop_declares(stop->decl, prop))
            continue;
        if (stop->pct <= u * 100.0f + 1e-4f && (!*lo || stop->pct >= *lo_pct)) {
            *lo = stop->decl;
            *lo_pct = stop->pct;
        }
        if (stop->pct >= u * 100.0f - 1e-4f && (!*hi || stop->pct <= *hi_pct)) {
            *hi = stop->decl;
            *hi_pct = stop->pct;
        }
        found = 1;
    }
    return found;
}

/**
 * @brief 在两个停靠点端点间求插值 / Interpolate between the two bracketing stops
 */
static void kf_lerp(const HpeStyle *lo, const HpeStyle *hi, float lo_pct, float hi_pct,
                    const HpeEasing *ease, float u, int prop, HpeAnimValue *out)
{
    HpeAnimValue a, b, res;
    float t;

    hpe_anim_value_get(lo, prop, &a);
    hpe_anim_value_get(hi, prop, &b);
    if (hi_pct - lo_pct < 1e-4f) {
        *out = b;
        return;
    }
    t = hpe_easing_solve(ease, (u * 100.0f - lo_pct) / (hi_pct - lo_pct));
    if (!hpe_anim_value_lerp(&a, &b, t, &res)) {
        *out = t < 0.5f ? a : b; /* non-interpolable pair: discrete swap at 50% */
        return;
    }
    *out = res;
}

/**
 * @brief 在 keyframes 停靠点间求某属性的插值 / Evaluate one property across keyframe stops
 *
 * @details 缺首尾端点时用邻侧停靠点补齐；段内缓动用动画声明的 easing
 *          （停靠点级 easing 尚未实现）。
 *          A missing endpoint falls back to the neighbouring stop. The animation's
 *          easing applies per segment (per-stop easing is not implemented).
 */
static int anim_eval_kf_prop(HpeEngine *engine, const HpeAnimRun *run, int prop, float u,
                             HpeAnimValue *out)
{
    HpeKeyframes *kf = hpe_anim_find_keyframes(&engine->sheet, run->name);
    const HpeStyle *lo = NULL, *hi = NULL;
    float lo_pct = 0.0f, hi_pct = 1.0f;

    if (!kf) {
        if (getenv("HOPE_TRACE_ANIM"))
            fprintf(stderr, "anim: kf %s not found\n", run->name);
        return 0;
    }
    if (!kf_bounds(kf, prop, u, &lo, &lo_pct, &hi, &hi_pct))
        return 0;
    if (!hi)
        hpe_anim_value_get(lo, prop, out);
    else if (!lo)
        hpe_anim_value_get(hi, prop, out);
    else
        kf_lerp(lo, hi, lo_pct, hi_pct, &run->decl.easing, u, prop, out);
    return 1;
}

/**
 * @brief 把一个运行项在当前时刻的取值套到样式上 / Overlay one run's current values onto the style
 */
static void anim_apply_run(HpeEngine *engine, const HpeAnimRun *run, HpeStyle *style,
                           int *layout_hit)
{
    HpeAnimWorld *w = &engine->anim;
    double local = w->clock - run->start;
    float dur = run->decl.duration;
    double iters = run->decl.iter_infinite ? 1e18 : (double)run->decl.iter_count;
    double k = 0.0;
    float u;
    int backwards = run->decl.fill == HPE_ANIM_FILL_BACKWARDS ||
                    run->decl.fill == HPE_ANIM_FILL_BOTH;
    int forwards = run->decl.fill == HPE_ANIM_FILL_FORWARDS ||
                   run->decl.fill == HPE_ANIM_FILL_BOTH;
    int prop;

    if (local < 0.0) {
        if (!backwards)
            return;
        u = run_progress_u(&run->decl, 0.0, dur, &k); /* delay shows the 0% frame */
    } else if (dur <= 0.0f || local >= dur * iters) {
        if (!forwards)
            return;
        /* the fill frame is the very end of the last played iteration */
        u = run_progress_u(&run->decl, (double)dur * iters, dur, &k);
    } else {
        u = run_progress_u(&run->decl, local, dur, &k);
    }
    for (prop = 1; prop < HPE_AP_COUNT; prop++) {
        HpeAnimValue v;
        if (!anim_eval_kf_prop(engine, run, prop, u, &v))
            continue;
        if (!v.valid)
            continue;
        hpe_anim_value_set(style, prop, &v);
        if (hpe_anim_prop_is_layout(prop))
            *layout_hit = 1;
    }
}

/**
 * @brief 输出一次钩子调用的跟踪行 / Emit one trace line for a hook call
 */
static void anim_trace_apply(const HpeDomNode *node, const HpeStyle *style,
                             const HpeAnimNodeState *st)
{
    if (!getenv("HOPE_TRACE_ANIM"))
        return;
    if (node->type == HPE_NODE_ELEMENT) {
        const char *el_id = hpe_dom_attr(node, "id");
        fprintf(stderr, "anim: apply %s#%s trans=%d anims=%d prev=%d run0=%d\n",
                node->tag, el_id ? el_id : "-", style->transition_count, style->anim_count,
                st->has_prev, st->runs[0].used);
    }
}

/**
 * @brief 把过渡运行项的当前取值覆盖到样式 / Overlay transition runs onto the style
 *
 * @return 有影响几何的过渡在途返回 1 / Returns 1 when a layout-affecting transition is active
 */
static int anim_apply_trans(HpeEngine *engine, HpeAnimNodeState *st, HpeStyle *style)
{
    HpeAnimWorld *w = &engine->anim;
    int layout_hit = 0;
    int i;

    for (i = 0; i < HPE_TRANS_MAX; i++) {
        HpeAnimTransRun *run = &st->trans[i];
        HpeAnimValue v;
        double local;
        float p;
        if (!run->used)
            continue;
        local = w->clock - run->start;
        if (local >= (double)run->duration && run->duration > 0.0f) {
            run->used = 0; /* done: the target values already sit in the style */
            continue;
        }
        if (run->duration <= 0.0f) {
            hpe_anim_value_set(style, run->prop, &run->to);
            run->used = 0;
            continue;
        }
        p = hpe_easing_solve(&run->easing, local < 0.0 ? 0.0f : (float)(local / run->duration));
        if (!hpe_anim_value_lerp(&run->from, &run->to, p, &v))
            v = p < 0.5f ? run->from : run->to;
        hpe_anim_value_set(style, run->prop, &v);
        if (hpe_anim_prop_is_layout(run->prop))
            layout_hit = 1;
    }
    return layout_hit;
}

/**
 * @brief 层叠钩子主体：过渡/动画覆盖到最新级联结果上 / Hook body: overlay transitions and animations onto the fresh cascade
 */
static void anim_apply(void *ud, const HpeDomNode *node, HpeStyle *style)
{
    HpeEngine *engine = (HpeEngine *)ud;
    HpeAnimWorld *w = &engine->anim;
    HpeAnimNodeState *st;
    HpeAnimValue target[HPE_AP_COUNT];
    int layout_hit = 0;
    int i;

    if (!engine->doc || !node)
        return;
    st = anim_state_get(engine, node, style->transition_count > 0 || style->anim_count > 0);
    if (!st)
        return;
    anim_trace_apply(node, style, st);
    for (i = 0; i < HPE_AP_COUNT; i++)
        hpe_anim_value_get(style, i, &target[i]);
    if (st->has_prev)
        anim_start_transitions(engine, st, style, target);
    anim_sync_runs(engine, st, style);
    if (anim_apply_trans(engine, st, style))
        layout_hit = 1;
    for (i = 0; i < HPE_ANIM_MAX; i++) {
        if (st->runs[i].used)
            anim_apply_run(engine, &st->runs[i], style, &layout_hit);
    }
    if (layout_hit) {
        /* geometry animation in flight: every flush of this active period must
         * rebuild the box tree, so keep the request sticky until it ends */
        w->reflow_need = HPE_REFLOW_LAYOUT;
        w->layout_sticky = 1;
    }
    memcpy(st->prev, target, sizeof(target));
    st->has_prev = 1;
}

/* ---------------- public entries ---------------- */

/**
 * @brief 注册层叠钩子并重置动画世界 / Register the cascade hook and reset the animation world
 */
void hpe_anim_install(HpeEngine *engine)
{
    if (!engine)
        return;
    memset(&engine->anim, 0, sizeof(engine->anim));
    hpe_css_set_anim_hook(anim_apply, engine);
}

/**
 * @brief 推进动画时钟并求值所有运行项 / Advance the animation clock and evaluate every run
 */
int hpe_anim_tick(HpeEngine *engine)
{
    HpeAnimWorld *w;
    const char *step;
    double dt;

    if (!engine || !engine->doc)
        return 0;
    w = &engine->anim;
    step = getenv("HOPE_TEST_ANIM_STEP");
    if (step && *step) {
        dt = atof(step); /* deterministic clock for headless regression */
    } else {
        double now = hpe_time_seconds();
        dt = now - w->last_real;
        w->last_real = now;
        if (dt < 0.0)
            dt = 0.0;
        if (dt > 0.25)
            dt = 0.25;
    }
    w->clock += dt;
    anim_prune_detached(engine);
    anim_evaluate_active(engine, dt);
    return w->active;
}

/**
 * @brief 换页时重置动画世界 / Reset the animation world on a page load
 */
void hpe_anim_reset(HpeEngine *engine)
{
    if (!engine)
        return;
    engine->anim.clock = 0.0;
    engine->anim.last_real = 0.0;
    engine->anim.active = 0;
    engine->anim.reflow_need = HPE_REFLOW_NONE;
    engine->anim.count = 0;
}

/**
 * @brief 释放动画世界缓冲 / Free the animation world's buffers
 */
void hpe_anim_shutdown(HpeEngine *engine)
{
    if (!engine)
        return;
    if (engine->anim.nodes)
        hpe_free(engine->anim.nodes);
    memset(&engine->anim, 0, sizeof(engine->anim));
    hpe_css_set_anim_hook(NULL, NULL);
}

/**
 * @brief 按 id 查找 DOM 元素 / Find a DOM element by its id attribute
 */
static HpeDomNode *anim_find_by_id(HpeDomNode *node, const char *id)
{
    HpeDomNode *child;
    for (child = node; child; child = child->next_sibling) {
        HpeDomNode *sub;
        if (child->type == HPE_NODE_ELEMENT) {
            const char *v = hpe_dom_attr(child, "id");
            if (v && !strcmp(v, id))
                return child;
        }
        sub = anim_find_by_id(child->first_child, id);
        if (sub)
            return sub;
    }
    return NULL;
}

/**
 * @brief 在盒树中查找属于某 DOM 节点的盒 / Find the box belonging to a DOM node
 */
static HpeBox *anim_box_for(HpeBox *box, const HpeDomNode *node)
{
    HpeBox *child;
    if (!box)
        return NULL;
    if (box->node == node)
        return box;
    for (child = box->first_child; child; child = child->next) {
        HpeBox *hit = anim_box_for(child, node);
        if (hit)
            return hit;
    }
    return NULL;
}

/**
 * @brief 调试：打印盒树中每个元素盒的透明度 / Debug: print every element box's opacity
 */
static void anim_trace_boxes(HpeBox *box)
{
    HpeBox *child;
    if (!box)
        return;
    if (box->node && box->node->type == HPE_NODE_ELEMENT)
        fprintf(stderr, "anim: box %s opacity=%.3f w=%.2f ml=%.2f\n", box->node->tag,
                box->style.opacity, box->style.width.value, box->style.margin[3].value);
    for (child = box->first_child; child; child = child->next)
        anim_trace_boxes(child);
}

/**
 * @brief 打印某元素当前动画/过渡后的计算值快照 / Dump an element's post-animation computed value snapshot
 */
void hpe_anim_snapshot_dump(HpeEngine *engine, const char *id)
{
    HpeDomNode *node;
    HpeBox *box;

    if (!engine || !id || !*id)
        return;
    if (!engine->doc || !engine->root)
        return;
    node = anim_find_by_id(engine->doc, id);
    if (!node) {
        printf("ANIM-SNAP %s: no such element\n", id);
        return;
    }
    box = anim_box_for(engine->root, node);
    if (!box) {
        printf("ANIM-SNAP %s: no box (display:none?)\n", id);
        return;
    }
    if (getenv("HOPE_TRACE_ANIM"))
        anim_trace_boxes(engine->root);
    printf("ANIM-SNAP %s opacity=%.3f width=%.2f height=%.2f ml=%.2f pl=%.2f fs=%.2f "
           "radius=%.2f color=%d,%d,%d,%d bg=%d,%d,%d,%d tf=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
           id, box->style.opacity, box->style.width.value, box->style.height.value,
           box->style.margin[3].value, box->style.padding[0].value, box->style.font_size,
           box->style.border_radius, box->style.color.r, box->style.color.g, box->style.color.b,
           box->style.color.a, box->style.background.r, box->style.background.g,
           box->style.background.b, box->style.background.a, box->style.transform.a,
           box->style.transform.b, box->style.transform.c, box->style.transform.d,
           box->style.transform.e, box->style.transform.f);
}
