/* Hope UI Engine - unit tests for CSS transition / animation parsing,
 * easing solving, value interpolation and @keyframes lookup */
#include "hpe_test.h"

#include <string.h>

#include "css/hpe_anim.h"
#include "css/hpe_css.h"

/* ---------------- time & easing parsing ---------------- */

/**
 * @brief 测试时间值的解析 / Test the parsing of time values
 */
HPE_TEST_CASE(anim, parse_time)
{
    float t = -1.0f;

    hpe_anim_parse_time("500ms", &t);
    HPE_TEST_ASSERT_NEAR(t, 0.5, 1e-6);
    hpe_anim_parse_time("1s", &t);
    HPE_TEST_ASSERT_NEAR(t, 1.0, 1e-6);
    hpe_anim_parse_time("2", &t);
    HPE_TEST_ASSERT_NEAR(t, 2.0, 1e-6);
    hpe_anim_parse_time("abc", &t);
    HPE_TEST_ASSERT_NEAR(t, 0.0, 1e-6);
    hpe_anim_parse_time("", &t);
    HPE_TEST_ASSERT_NEAR(t, 0.0, 1e-6);
}

/**
 * @brief 测试缓动函数关键字的解析 / Test the parsing of easing keywords
 */
HPE_TEST_CASE(anim, parse_easing_keywords)
{
    HpeEasing e;

    hpe_anim_parse_easing("linear", &e);
    HPE_TEST_ASSERT_INT(e.kind, HPE_EASING_LINEAR);
    hpe_anim_parse_easing("ease-in-out", &e);
    HPE_TEST_ASSERT_INT(e.kind, HPE_EASING_EASE_IN_OUT);
    hpe_anim_parse_easing("ease", &e);
    HPE_TEST_ASSERT_INT(e.kind, HPE_EASING_EASE);
}

/**
 * @brief 测试 cubic-bezier 与 steps 的解析 / Test the parsing of cubic-bezier and steps
 */
HPE_TEST_CASE(anim, parse_easing_functions)
{
    HpeEasing e;

    hpe_anim_parse_easing("cubic-bezier(0.1, 0.7, 1, 0.2)", &e);
    HPE_TEST_ASSERT_INT(e.kind, HPE_EASING_CUBIC);
    HPE_TEST_ASSERT_NEAR(e.x1, 0.1, 1e-5);
    HPE_TEST_ASSERT_NEAR(e.y1, 0.7, 1e-5);
    HPE_TEST_ASSERT_NEAR(e.x2, 1.0, 1e-5);
    HPE_TEST_ASSERT_NEAR(e.y2, 0.2, 1e-5);
    hpe_anim_parse_easing("steps(4, start)", &e);
    HPE_TEST_ASSERT_INT(e.kind, HPE_EASING_STEPS);
    HPE_TEST_ASSERT_INT(e.steps, 4);
    HPE_TEST_ASSERT_INT(e.steps_start, 1);
    hpe_anim_parse_easing("steps(3)", &e);
    HPE_TEST_ASSERT_INT(e.steps, 3);
    HPE_TEST_ASSERT_INT(e.steps_start, 0);
    hpe_anim_parse_easing("nonsense", &e);
    HPE_TEST_ASSERT_INT(e.kind, HPE_EASING_EASE);
}

/* ---------------- easing solving ---------------- */

/**
 * @brief 测试 linear 与关键字缓动的端点 / Test linear and keyword easing endpoints
 */
HPE_TEST_CASE(anim, easing_solve_linear)
{
    HpeEasing e;

    hpe_anim_parse_easing("linear", &e);
    HPE_TEST_ASSERT_NEAR(hpe_easing_solve(&e, 0.0f), 0.0, 1e-6);
    HPE_TEST_ASSERT_NEAR(hpe_easing_solve(&e, 0.25f), 0.25, 1e-6);
    HPE_TEST_ASSERT_NEAR(hpe_easing_solve(&e, 1.0f), 1.0, 1e-6);
    hpe_anim_parse_easing("ease-in", &e);
    HPE_TEST_ASSERT_NEAR(hpe_easing_solve(&e, 0.0f), 0.0, 1e-6);
    HPE_TEST_ASSERT_NEAR(hpe_easing_solve(&e, 1.0f), 1.0, 1e-6);
    /* ease-in is slow at the start */
    HPE_TEST_ASSERT(hpe_easing_solve(&e, 0.5f) < 0.5f);
}

/**
 * @brief 测试 steps 缓动的阶梯取值 / Test the staircase values of steps easing
 */
HPE_TEST_CASE(anim, easing_solve_steps)
{
    HpeEasing e;

    hpe_anim_parse_easing("steps(4, end)", &e);
    HPE_TEST_ASSERT_NEAR(hpe_easing_solve(&e, 0.0f), 0.0, 1e-6);
    /* just past the first interval boundary -> step 1 */
    HPE_TEST_ASSERT_NEAR(hpe_easing_solve(&e, 0.26f), 0.25, 1e-6);
    HPE_TEST_ASSERT_NEAR(hpe_easing_solve(&e, 1.0f), 1.0, 1e-6);
    hpe_anim_parse_easing("steps(4, start)", &e);
    /* start: the first jump happens immediately */
    HPE_TEST_ASSERT_NEAR(hpe_easing_solve(&e, 0.0f), 0.25, 1e-6);
    HPE_TEST_ASSERT_NEAR(hpe_easing_solve(&e, 1.0f), 1.0, 1e-6);
}

/* ---------------- transition declarations ---------------- */

/**
 * @brief 测试 transition 简写的逐项解析 / Test per-item parsing of the transition shorthand
 */
HPE_TEST_CASE(anim, transition_shorthand)
{
    HpeStyle s;
    HpeStyle d;

    memset(&s, 0, sizeof(s));
    hpe_anim_parse_transition_shorthand("width 1s linear 0.5s", &s);
    HPE_TEST_ASSERT_INT(s.transition_count, 1);
    HPE_TEST_ASSERT_INT(s.transitions[0].prop, HPE_AP_WIDTH);
    HPE_TEST_ASSERT_NEAR(s.transitions[0].duration, 1.0, 1e-6);
    HPE_TEST_ASSERT_NEAR(s.transitions[0].delay, 0.5, 1e-6);
    HPE_TEST_ASSERT_INT(s.transitions[0].easing.kind, HPE_EASING_LINEAR);

    memset(&d, 0, sizeof(d));
    hpe_anim_parse_transition_shorthand("opacity 0.2s, transform 300ms ease-in", &d);
    HPE_TEST_ASSERT_INT(d.transition_count, 2);
    HPE_TEST_ASSERT_INT(d.transitions[0].prop, HPE_AP_OPACITY);
    HPE_TEST_ASSERT_NEAR(d.transitions[0].duration, 0.2, 1e-6);
    HPE_TEST_ASSERT_INT(d.transitions[1].prop, HPE_AP_TRANSFORM);
    HPE_TEST_ASSERT_NEAR(d.transitions[1].duration, 0.3, 1e-6);
    HPE_TEST_ASSERT_INT(d.transitions[1].easing.kind, HPE_EASING_EASE_IN);

    hpe_anim_parse_transition_shorthand("none", &d);
    HPE_TEST_ASSERT_INT(d.transition_count, 0);
}

/**
 * @brief 测试 transition 长属性的解析 / Test the parsing of transition longhands
 */
HPE_TEST_CASE(anim, transition_longhand)
{
    HpeStyle d;

    memset(&d, 0, sizeof(d));
    hpe_anim_parse_transition_longhand("transition-property", "opacity, width", &d);
    hpe_anim_parse_transition_longhand("transition-duration", "1s, 250ms", &d);
    HPE_TEST_ASSERT_INT(d.transition_count, 2);
    HPE_TEST_ASSERT_INT(d.transitions[0].prop, HPE_AP_OPACITY);
    HPE_TEST_ASSERT_INT(d.transitions[1].prop, HPE_AP_WIDTH);
    HPE_TEST_ASSERT_NEAR(d.transitions[0].duration, 1.0, 1e-6);
    HPE_TEST_ASSERT_NEAR(d.transitions[1].duration, 0.25, 1e-6);
}

/* ---------------- animation declarations ---------------- */

/**
 * @brief 测试 animation 简写的顺序无关解析 / Test the order-free parsing of the animation shorthand
 */
HPE_TEST_CASE(anim, animation_shorthand)
{
    HpeStyle d;

    memset(&d, 0, sizeof(d));
    hpe_anim_parse_animation_shorthand("fade 1s linear 2 alternate both 0.5s", &d);
    HPE_TEST_ASSERT_INT(d.anim_count, 1);
    HPE_TEST_ASSERT_STR(d.anims[0].name, "fade");
    HPE_TEST_ASSERT_NEAR(d.anims[0].duration, 1.0, 1e-6);
    HPE_TEST_ASSERT_NEAR(d.anims[0].delay, 0.5, 1e-6);
    HPE_TEST_ASSERT_NEAR(d.anims[0].iter_count, 2.0, 1e-6);
    HPE_TEST_ASSERT_INT(d.anims[0].iter_infinite, 0);
    HPE_TEST_ASSERT_INT(d.anims[0].direction, HPE_ANIM_DIR_ALTERNATE);
    HPE_TEST_ASSERT_INT(d.anims[0].fill, HPE_ANIM_FILL_BOTH);
    HPE_TEST_ASSERT_INT(d.anims[0].easing.kind, HPE_EASING_LINEAR);

    memset(&d, 0, sizeof(d));
    hpe_anim_parse_animation_shorthand("slide 2s cubic-bezier(0.1, 0.7, 1, 1) infinite", &d);
    HPE_TEST_ASSERT_STR(d.anims[0].name, "slide");
    HPE_TEST_ASSERT_INT(d.anims[0].iter_infinite, 1);
    HPE_TEST_ASSERT_INT(d.anims[0].easing.kind, HPE_EASING_CUBIC);
    HPE_TEST_ASSERT_NEAR(d.anims[0].easing.x1, 0.1, 1e-5);

    memset(&d, 0, sizeof(d));
    hpe_anim_parse_animation_shorthand("fade 1s steps(4, end), slide 2s reverse forwards", &d);
    HPE_TEST_ASSERT_INT(d.anim_count, 2);
    HPE_TEST_ASSERT_INT(d.anims[0].easing.kind, HPE_EASING_STEPS);
    HPE_TEST_ASSERT_INT(d.anims[1].direction, HPE_ANIM_DIR_REVERSE);
    HPE_TEST_ASSERT_INT(d.anims[1].fill, HPE_ANIM_FILL_FORWARDS);

    hpe_anim_parse_animation_shorthand("none", &d);
    HPE_TEST_ASSERT_INT(d.anim_count, 0);
}

/**
 * @brief 测试 animation 长属性的解析 / Test the parsing of animation longhands
 */
HPE_TEST_CASE(anim, animation_longhand)
{
    HpeStyle d;

    memset(&d, 0, sizeof(d));
    hpe_anim_parse_animation_longhand("animation-name", "a, b", &d);
    hpe_anim_parse_animation_longhand("animation-duration", "1s, 2s", &d);
    hpe_anim_parse_animation_longhand("animation-iteration-count", "infinite, 3", &d);
    HPE_TEST_ASSERT_INT(d.anim_count, 2);
    HPE_TEST_ASSERT_STR(d.anims[0].name, "a");
    HPE_TEST_ASSERT_NEAR(d.anims[0].duration, 1.0, 1e-6);
    HPE_TEST_ASSERT_INT(d.anims[0].iter_infinite, 1);
    HPE_TEST_ASSERT_STR(d.anims[1].name, "b");
    HPE_TEST_ASSERT_NEAR(d.anims[1].iter_count, 3.0, 1e-6);
}

/* ---------------- @keyframes ---------------- */

/**
 * @brief 测试样式表中 @keyframes 的解析与查找 / Test parsing and lookup of @keyframes in a stylesheet
 */
HPE_TEST_CASE(anim, keyframes_lookup)
{
    HpeStylesheet sheet;
    HpeKeyframes *kf;
    const char *css =
        "@keyframes fade { from { opacity: 0; } 50% { opacity: 0.5; } to { opacity: 1; } }";

    hpe_css_init(&sheet);
    hpe_css_parse(&sheet, css, strlen(css));
    kf = hpe_anim_find_keyframes(&sheet, "fade");
    HPE_TEST_ASSERT(kf != NULL);
    if (kf) {
        HPE_TEST_ASSERT_INT(kf->stop_count, 3);
        HPE_TEST_ASSERT_NEAR(kf->stops[0].pct, 0.0, 1e-5);
        HPE_TEST_ASSERT_NEAR(kf->stops[1].pct, 50.0, 1e-5);
        HPE_TEST_ASSERT_NEAR(kf->stops[2].pct, 100.0, 1e-5);
        HPE_TEST_ASSERT_NEAR(kf->stops[1].decl->opacity, 0.5, 1e-6);
    }
    HPE_TEST_ASSERT(hpe_anim_find_keyframes(&sheet, "missing") == NULL);
    /* a later definition replaces the earlier one wholesale */
    hpe_css_parse(&sheet, "@keyframes fade { to { opacity: 0.2; } }", 36);
    kf = hpe_anim_find_keyframes(&sheet, "fade");
    HPE_TEST_ASSERT(kf != NULL);
    HPE_TEST_ASSERT_INT(kf->stop_count, 1);
    hpe_css_free(&sheet);
}

/* ---------------- value get/set/lerp ---------------- */

/**
 * @brief 测试长度值的取值与插值 / Test getting and lerping length values
 */
HPE_TEST_CASE(anim, value_lerp_length)
{
    HpeStyle a, b;
    HpeAnimValue va, vb, out;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.width.unit = HPE_LEN_PX;
    a.width.value = 100.0f;
    a.set = HPE_PROP_WIDTH;
    b.width.unit = HPE_LEN_PX;
    b.width.value = 200.0f;
    b.set = HPE_PROP_WIDTH;
    HPE_TEST_ASSERT_INT(hpe_anim_value_get(&a, HPE_AP_WIDTH, &va), 1);
    HPE_TEST_ASSERT_INT(hpe_anim_value_get(&b, HPE_AP_WIDTH, &vb), 1);
    HPE_TEST_ASSERT_INT(hpe_anim_value_lerp(&va, &vb, 0.5f, &out), 1);
    HPE_TEST_ASSERT_NEAR(out.len.value, 150.0, 1e-5);
    HPE_TEST_ASSERT_NEAR(hpe_anim_value_lerp(&va, &vb, 0.25f, &out) ? out.len.value : -1.0,
                         125.0, 1e-5);
}

/**
 * @brief 测试浮点与颜色值的插值 / Test lerping float and colour values
 */
HPE_TEST_CASE(anim, value_lerp_float_color)
{
    HpeStyle a, b;
    HpeAnimValue va, vb, out;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.opacity = 0.0f;
    a.set2 = HPE_PROP2_OPACITY;
    b.opacity = 1.0f;
    b.set2 = HPE_PROP2_OPACITY;
    hpe_anim_value_get(&a, HPE_AP_OPACITY, &va);
    hpe_anim_value_get(&b, HPE_AP_OPACITY, &vb);
    HPE_TEST_ASSERT_INT(hpe_anim_value_lerp(&va, &vb, 0.35f, &out), 1);
    HPE_TEST_ASSERT_NEAR(out.f, 0.35, 1e-5);

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.color.r = 0;
    a.color.g = 0;
    a.color.b = 0;
    a.set = HPE_PROP_COLOR;
    b.color.r = 255;
    b.color.g = 0;
    b.color.b = 0;
    b.set = HPE_PROP_COLOR;
    hpe_anim_value_get(&a, HPE_AP_COLOR, &va);
    hpe_anim_value_get(&b, HPE_AP_COLOR, &vb);
    HPE_TEST_ASSERT_INT(hpe_anim_value_lerp(&va, &vb, 0.5f, &out), 1);
    HPE_TEST_ASSERT_NEAR(out.color.r, 127.5, 0.6);
}

/**
 * @brief 测试 transform 值的插值 / Test lerping transform values
 */
HPE_TEST_CASE(anim, value_lerp_transform)
{
    HpeStyle a, b;
    HpeAnimValue va, vb, out;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    hpe_css_parse_declarations("transform: translateX(0px)", strlen("transform: translateX(0px)"),
                               &a);
    hpe_css_parse_declarations("transform: translateX(100px)",
                               strlen("transform: translateX(100px)"), &b);
    HPE_TEST_ASSERT(hpe_anim_value_get(&a, HPE_AP_TRANSFORM, &va));
    HPE_TEST_ASSERT(hpe_anim_value_get(&b, HPE_AP_TRANSFORM, &vb));
    HPE_TEST_ASSERT_INT(hpe_anim_value_lerp(&va, &vb, 0.5f, &out), 1);
    HPE_TEST_ASSERT_INT(out.type, HPE_AV_TRANSFORM);
    /* the midpoint translation is exactly half the distance */
    HPE_TEST_ASSERT_NEAR(out.tf.e, 50.0, 1e-4);
}

/* ---------------- layout classification ---------------- */

/**
 * @brief 测试几何属性分类 / Test the geometry classification of properties
 */
HPE_TEST_CASE(anim, prop_is_layout)
{
    HPE_TEST_ASSERT_INT(hpe_anim_prop_is_layout(HPE_AP_WIDTH), 1);
    HPE_TEST_ASSERT_INT(hpe_anim_prop_is_layout(HPE_AP_MARGIN_LEFT), 1);
    HPE_TEST_ASSERT_INT(hpe_anim_prop_is_layout(HPE_AP_OPACITY), 0);
    HPE_TEST_ASSERT_INT(hpe_anim_prop_is_layout(HPE_AP_COLOR), 0);
}
