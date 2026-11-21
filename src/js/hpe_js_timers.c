/* Hope UI Engine - javascript timer queue: setTimeout/setInterval/clear* and
 * requestAnimationFrame. Callbacks run from hpe_js_run_timers, driven by the
 * engine's tick (window WM_TIMER or the headless test hooks). */
#include "js/hpe_js_internal.h"

#include <stdio.h>
#include <string.h>

#include "platform/hpe_platform.h"

typedef struct JsTimer {
    int id;
    int repeating; /* setInterval */
    int is_raf;    /* requestAnimationFrame: runs on every drain, no id */
    double due;    /* absolute hpe_time_seconds() deadline */
    double every;  /* repeating interval, seconds */
    JsValue fn;
} JsTimer;

static JsTimer *timer_by_id(HpeJs *js, int id)
{
    int i;
    for (i = 0; i < js->timer_count; i++)
        if (js->timers[i].id == id)
            return &js->timers[i];
    return NULL;
}

static void timer_remove(HpeJs *js, JsTimer *t)
{
    int idx = (int)(t - js->timers);
    if (idx < 0 || idx >= js->timer_count)
        return;
    memmove(&js->timers[idx], &js->timers[idx + 1],
            sizeof(JsTimer) * (size_t)(js->timer_count - idx - 1));
    js->timer_count--;
}

static JsTimer *timer_grow(HpeJs *js)
{
    JsTimer *old = js->timers;
    int n = js->timer_count;
    JsTimer *arr = (JsTimer *)hpe_js_alloc(js, sizeof(JsTimer) * (size_t)(n + 1));
    if (n > 0)
        memcpy(arr, old, sizeof(JsTimer) * (size_t)n);
    js->timers = arr;
    js->timer_count = n + 1;
    return &arr[n];
}

static JsValue native_set_timeout(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    double ms = argc > 1 ? js_to_number(js, args[1]) : 0.0;
    int repeating = ud != NULL;
    JsTimer *t;
    HPE_UNUSED(thisv);
    if (argc < 1 || args[0].t != JS_OBJ || args[0].obj->kind != JS_OBJ_FUNC)
        return js_num(-1);
    if (ms < 0.0)
        ms = 0.0;
    t = timer_grow(js);
    t->id = ++js->next_timer_id;
    t->repeating = repeating;
    t->is_raf = 0;
    t->every = ms / 1000.0;
    t->due = hpe_time_seconds() + t->every;
    t->fn = args[0];
    return js_num((double)t->id);
}

static JsValue native_clear_timer(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    JsTimer *t;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (argc < 1)
        return js_undef();
    t = timer_by_id(js, (int)js_to_number(js, args[0]));
    if (t)
        timer_remove(js, t);
    return js_undef();
}

static JsValue native_request_animation_frame(HpeJs *js, JsValue thisv, JsValue *args, int argc,
                                              void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    HPE_UNUSED(argc);
    if (argc < 1 || args[0].t != JS_OBJ || args[0].obj->kind != JS_OBJ_FUNC)
        return js_undef();
    timer_grow(js);
    {
        JsTimer *t = &js->timers[js->timer_count - 1];
        t->id = 0;
        t->repeating = 0;
        t->is_raf = 1;
        t->due = 0.0;
        t->every = 0.0;
        t->fn = args[0];
    }
    return js_undef();
}

/* runs all due timers and the rAF queue. returns the number of callbacks that
 * actually ran (so the engine knows a repaint may be needed). */
int hpe_js_run_timers(HpeJs *js, double now, int *fired)
{
    int i, ran = 0;
    if (!js)
        return 0;
    for (i = js->timer_count - 1; i >= 0; i--) {
        JsTimer *t = &js->timers[i];
        if (t->is_raf || t->due <= now) {
            JsValue fn = t->fn;
            int repeating = t->repeating;
            int is_raf = t->is_raf;
            if (is_raf) {
                timer_remove(js, t);
            } else if (repeating) {
                t->due += t->every;
                if (t->due <= now)
                    t->due = now + t->every;
            } else {
                timer_remove(js, t);
            }
            if (hpe_js_call(js, fn, js_undef(), NULL, 0) == 0)
                ran++;
        }
    }
    if (fired)
        *fired = ran > 0;
    return ran;
}

void hpe_js_timers_install(HpeJs *js)
{
    js_obj_set(js, js->global_obj, "setTimeout",
               js_objval(js_native_new(js, native_set_timeout, NULL)));
    js_obj_set(js, js->global_obj, "setInterval",
               js_objval(js_native_new(js, native_set_timeout, (void *)1)));
    js_obj_set(js, js->global_obj, "clearTimeout",
               js_objval(js_native_new(js, native_clear_timer, NULL)));
    js_obj_set(js, js->global_obj, "clearInterval",
               js_objval(js_native_new(js, native_clear_timer, (void *)1)));
    js_obj_set(js, js->global_obj, "requestAnimationFrame",
               js_objval(js_native_new(js, native_request_animation_frame, NULL)));
}
