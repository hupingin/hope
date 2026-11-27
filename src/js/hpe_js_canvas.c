/* Hope UI Engine - JavaScript canvas 2D bridge (v0.27), see hpe_js_canvas.h */
#include "js/hpe_js_canvas.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"
#include "canvas/hpe_canvas.h"
#include "js/hpe_js_dom.h"
#include "js/hpe_js_internal.h"

/* the context host wraps the DOM node (surface is looked up on each call) */
static const HpeJsHostVt g_ctx_vt;

static HpeCanvas *ctx_node(HpeJs *js, JsValue thisv)
{
    HpeCanvas *c;
    HPE_UNUSED(js);
    c = (HpeCanvas *)js_host_ptr(thisv, HPE_HOST_CANVAS_CTX);
    /* every 2D method touches its context: mark the canvas dirty so the
     * engine repaints even when drawing happened without a DOM mutation */
    hpe_canvas_notify();
    return c;
}

/* ---- argument helpers ---- */

static float argf(HpeJs *js, JsValue *args, int argc, int i, float fallback)
{
    if (i >= argc)
        return fallback;
    return (float)js_to_number(js, args[i]);
}

static double argd(HpeJs *js, JsValue *args, int argc, int i, double fallback)
{
    if (i >= argc)
        return fallback;
    return js_to_number(js, args[i]);
}

/* ---- 2D native methods ---- */

static JsValue ctx_fill_rect(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeCanvas *c = ctx_node(js, thisv);
    HPE_UNUSED(ud);
    if (c)
        hpe_canvas_fill_rect(c, argf(js, args, argc, 0, 0), argf(js, args, argc, 1, 0),
                             argf(js, args, argc, 2, 0), argf(js, args, argc, 3, 0));
    return js_undef();
}

static JsValue ctx_clear_rect(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeCanvas *c = ctx_node(js, thisv);
    HPE_UNUSED(ud);
    if (c)
        hpe_canvas_clear_rect(c, argf(js, args, argc, 0, 0), argf(js, args, argc, 1, 0),
                              argf(js, args, argc, 2, 0), argf(js, args, argc, 3, 0));
    return js_undef();
}

static JsValue ctx_stroke_rect(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeCanvas *c = ctx_node(js, thisv);
    HPE_UNUSED(ud);
    if (c)
        hpe_canvas_stroke_rect(c, argf(js, args, argc, 0, 0), argf(js, args, argc, 1, 0),
                               argf(js, args, argc, 2, 0), argf(js, args, argc, 3, 0));
    return js_undef();
}

static JsValue ctx_noarg(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeCanvas *c = ctx_node(js, thisv);
    int kind = (int)(intptr_t)ud;
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    if (!c)
        return js_undef();
    if (kind == 0) hpe_canvas_begin_path(c);
    else if (kind == 1) hpe_canvas_close_path(c);
    else if (kind == 2) hpe_canvas_fill_path(c);
    else if (kind == 3) hpe_canvas_stroke_path(c);
    else if (kind == 4) hpe_canvas_save(c);
    else if (kind == 5) hpe_canvas_restore(c);
    else if (kind == 6) hpe_canvas_reset_transform(c);
    return js_undef();
}

static JsValue ctx_move_line(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeCanvas *c = ctx_node(js, thisv);
    float x = argf(js, args, argc, 0, 0), y = argf(js, args, argc, 1, 0);
    HPE_UNUSED(ud);
    if (c) {
        if ((intptr_t)ud == 0) hpe_canvas_move_to(c, x, y);
        else hpe_canvas_line_to(c, x, y);
    }
    return js_undef();
}

static JsValue ctx_arc(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeCanvas *c = ctx_node(js, thisv);
    HPE_UNUSED(ud);
    if (c)
        hpe_canvas_arc(c, argf(js, args, argc, 0, 0), argf(js, args, argc, 1, 0),
                       argf(js, args, argc, 2, 0), argf(js, args, argc, 3, 0),
                       argf(js, args, argc, 4, (float)(2.0 * M_PI)),
                       argc > 5 && js_truthy(args[5]));
    return js_undef();
}

static JsValue ctx_translate(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeCanvas *c = ctx_node(js, thisv);
    HPE_UNUSED(ud);
    if (c)
        hpe_canvas_translate(c, argf(js, args, argc, 0, 0), argf(js, args, argc, 1, 0));
    return js_undef();
}

static JsValue ctx_scale(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeCanvas *c = ctx_node(js, thisv);
    HPE_UNUSED(ud);
    if (c)
        hpe_canvas_scale(c, argf(js, args, argc, 0, 1), argf(js, args, argc, 1, 1));
    return js_undef();
}

static JsValue ctx_rotate(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeCanvas *c = ctx_node(js, thisv);
    HPE_UNUSED(ud);
    if (c)
        hpe_canvas_rotate(c, (float)argd(js, args, argc, 0, 0));
    return js_undef();
}

static JsValue ctx_set_transform(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeCanvas *c = ctx_node(js, thisv);
    HPE_UNUSED(ud);
    if (c)
        hpe_canvas_set_transform(c, argf(js, args, argc, 0, 1), argf(js, args, argc, 1, 0),
                                 argf(js, args, argc, 2, 0), argf(js, args, argc, 3, 1),
                                 argf(js, args, argc, 4, 0), argf(js, args, argc, 5, 0));
    return js_undef();
}

/* ---- pixel methods ---- */

static JsValue ctx_get_image_data(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeCanvas *c = ctx_node(js, thisv);
    JsObj *arr, *data;
    int x, y, w, h, i, n;
    uint8_t *buf;
    char key[32];
    HPE_UNUSED(ud);
    if (!c)
        return js_null();
    x = (int)argd(js, args, argc, 0, 0);
    y = (int)argd(js, args, argc, 1, 0);
    w = (int)argd(js, args, argc, 2, 0);
    h = (int)argd(js, args, argc, 3, 0);
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    n = w * h * 4;
    buf = (uint8_t *)hpe_calloc((size_t)(n > 0 ? n : 1), 1);
    hpe_canvas_get_image_data(c, x, y, w, h, buf);
    data = js_array_new(js);
    for (i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "%d", i);
        js_obj_set(js, data, key, js_num(buf[i]));
    }
    js_obj_set(js, data, "length", js_num(n));
    hpe_free(buf);
    arr = js_object_new(js);
    js_obj_set(js, arr, "width", js_num(w));
    js_obj_set(js, arr, "height", js_num(h));
    js_obj_set(js, arr, "data", js_objval(data));
    return js_objval(arr);
}

static JsValue ctx_put_image_data(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeCanvas *c = ctx_node(js, thisv);
    JsValue img, data;
    JsObj *imgobj;
    int x, y, w, h, i, n;
    uint8_t *buf;
    char key[32];
    HPE_UNUSED(ud);
    if (!c || argc < 1 || args[0].t != JS_OBJ || !args[0].obj)
        return js_undef();
    x = (int)argd(js, args, argc, 1, 0);
    y = (int)argd(js, args, argc, 2, 0);
    imgobj = args[0].obj;
    w = (int)js_to_number(js, js_obj_get(js, imgobj, "width"));
    h = (int)js_to_number(js, js_obj_get(js, imgobj, "height"));
    data = js_obj_get(js, imgobj, "data");
    img = data;
    HPE_UNUSED(img);
    if (w <= 0 || h <= 0 || data.t != JS_OBJ || !data.obj)
        return js_undef();
    n = w * h * 4;
    buf = (uint8_t *)hpe_calloc((size_t)n, 1);
    for (i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "%d", i);
        buf[i] = (uint8_t)js_to_number(js, js_obj_get(js, data.obj, key));
    }
    hpe_canvas_put_image_data(c, x, y, w, h, buf);
    hpe_free(buf);
    return js_undef();
}

/* ---- style colour helpers ---- */

static void color_to_string(HpeColor c, char *out, size_t n)
{
    snprintf(out, n, "#%02x%02x%02x", c.r, c.g, c.b);
}

/* ---- context host vtable (fillStyle / strokeStyle / lineWidth / globalAlpha) ---- */

static int ctx_get_prop(HpeJs *js, void *target, const char *prop, JsValue *out)
{
    HpeCanvas *c = (HpeCanvas *)target;
    char buf[16];
    HPE_UNUSED(js);
    if (!c)
        return 0;
    if (!strcmp(prop, "fillStyle")) {
        color_to_string(c->fill_style, buf, sizeof(buf));
        *out = js_string(js, buf);
        return 1;
    }
    if (!strcmp(prop, "strokeStyle")) {
        color_to_string(c->stroke_style, buf, sizeof(buf));
        *out = js_string(js, buf);
        return 1;
    }
    if (!strcmp(prop, "lineWidth")) {
        *out = js_num(c->line_width);
        return 1;
    }
    if (!strcmp(prop, "globalAlpha")) {
        *out = js_num(c->global_alpha);
        return 1;
    }
    return 0;
}

static int ctx_set_prop(HpeJs *js, void *target, const char *prop, JsValue value)
{
    HpeCanvas *c = (HpeCanvas *)target;
    HpeColor col;
    if (!c)
        return 0;
    if (!strcmp(prop, "fillStyle") || !strcmp(prop, "strokeStyle")) {
        char *s = js_to_string(js, value);
        if (s && hpe_color_parse(s, &col)) {
            if (!strcmp(prop, "fillStyle")) c->fill_style = col;
            else c->stroke_style = col;
        }
        return 1;
    }
    if (!strcmp(prop, "lineWidth")) {
        c->line_width = (float)js_to_number(js, value);
        return 1;
    }
    if (!strcmp(prop, "globalAlpha")) {
        c->global_alpha = (float)js_to_number(js, value);
        if (c->global_alpha < 0.0f) c->global_alpha = 0.0f;
        if (c->global_alpha > 1.0f) c->global_alpha = 1.0f;
        return 1;
    }
    return 0;
}

static const HpeJsHostVt g_ctx_vt = {ctx_get_prop, ctx_set_prop};

/* ---- method table ---- */

typedef struct {
    const char *name;
    JsNativeFn fn;
    intptr_t ud;
} CtxMethod;

static const CtxMethod ctx_methods[] = {
    { "fillRect",       ctx_fill_rect,    0 },
    { "clearRect",      ctx_clear_rect,   0 },
    { "strokeRect",     ctx_stroke_rect,  0 },
    { "beginPath",      ctx_noarg,        0 },
    { "closePath",      ctx_noarg,        1 },
    { "fill",           ctx_noarg,        2 },
    { "stroke",         ctx_noarg,        3 },
    { "save",           ctx_noarg,        4 },
    { "restore",        ctx_noarg,        5 },
    { "resetTransform", ctx_noarg,        6 },
    { "moveTo",         ctx_move_line,    0 },
    { "lineTo",         ctx_move_line,    1 },
    { "arc",            ctx_arc,          0 },
    { "translate",      ctx_translate,    0 },
    { "scale",          ctx_scale,        0 },
    { "rotate",         ctx_rotate,       0 },
    { "setTransform",   ctx_set_transform, 0 },
    { "getImageData",   ctx_get_image_data, 0 },
    { "putImageData",   ctx_put_image_data, 0 },
    { NULL, NULL, 0 }
};

JsValue hpe_js_canvas_get_context(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HpeCanvas *c;
    JsValue obj;
    const CtxMethod *m;
    char *kind;
    if (!node || !node->tag || strcmp(node->tag, "canvas"))
        return js_null();
    kind = argc > 0 ? js_to_string(js, args[0]) : NULL;
    if (!kind || (strcmp(kind, "2d") && strcmp(kind, "2d-context")))
        return js_null();
    c = hpe_canvas_get(node);
    if (!c)
        return js_null();
    obj = js_make_host(js, c, HPE_HOST_CANVAS_CTX, &g_ctx_vt);
    for (m = ctx_methods; m->name; m++)
        js_obj_set(js, obj.obj, m->name,
                   js_objval(js_native_new(js, m->fn, (void *)m->ud)));
    return obj;
}

void hpe_js_canvas_install(HpeJs *js)
{
    HPE_UNUSED(js);
    /* getContext is dispatched from the DOM element method table; nothing to
     * install globally, this hook exists for symmetry / future 3d contexts. */
}
