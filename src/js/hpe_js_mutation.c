/* Hope UI Engine - MutationObserver (DOM change notification)
 *
 * Implements the subset of the W3C DOM MutationObserver interface that fits a
 * local-file engine:
 *
 *   var mo = new MutationObserver(callback);
 *   mo.observe(target, { childList: true, attributes: true, characterData: true,
 *                        subtree: true, attributeOldValue: true,
 *                        characterDataOldValue: true, attributeFilter: [...] });
 *   mo.disconnect();
 *   var records = mo.takeRecords();
 *
 * The DOM layer reports every change through hpe_dom.h's mutation sinks; this
 * module turns those reports into MutationRecord objects and queues one
 * delivery microtask per observer, mirroring the specification's "queue a
 * mutation observer microtask" step (so callbacks run asynchronously).
 */
#include "js/hpe_js_mutation.h"

#include <stdio.h>
#include <string.h>

#include "dom/hpe_dom.h"
#include "js/hpe_js_dom.h"
#include "js/hpe_js_internal.h"

/* one pending record, held until the delivery microtask runs */
typedef struct JsMutationRecord {
    int type; /* HPE_MUT_* */
    HpeDomNode *target;
    char *attribute_name;
    char *old_value;
    struct JsMutationRecord *next;
} JsMutationRecord;

typedef struct JsMutationObserver {
    JsValue callback;
    HpeDomNode *target; /* the observed node */
    int child_list;
    int attributes;
    int character_data;
    int subtree;
    int attr_old_value;
    int char_old_value;
    char **attr_filter;
    int attr_filter_count;
    JsMutationRecord *head;
    JsMutationRecord *tail;
    int queued; /* a delivery microtask is pending */
    struct JsMutationObserver *next; /* the js-wide observer list */
} JsMutationObserver;

/* ---------------- per-interpreter state ---------------- */

HpeJsMutationState *hpe_js_mutation_state(HpeJs *js)
{
    HpeJsMutationState *st;
    if (!js)
        return NULL;
    if (js->mutation_state)
        return js->mutation_state;
    st = (HpeJsMutationState *)hpe_js_alloc(js, sizeof(HpeJsMutationState));
    memset(st, 0, sizeof(*st));
    js->mutation_state = st;
    return st;
}

/* ---------------- record bookkeeping ---------------- */

static JsMutationRecord *record_new(HpeJs *js)
{
    JsMutationRecord *r = (JsMutationRecord *)hpe_js_alloc(js, sizeof(JsMutationRecord));
    memset(r, 0, sizeof(*r));
    return r;
}

static void record_push(JsMutationObserver *o, JsMutationRecord *r)
{
    if (o->tail)
        o->tail->next = r;
    else
        o->head = r;
    o->tail = r;
}

/* does this observer watch `target`? walks ancestors when subtree is set */
static int observes_node(JsMutationObserver *o, HpeDomNode *target)
{
    HpeDomNode *n = target;
    if (!o->target)
        return 0;
    for (; n; n = n->parent) {
        if (n == o->target)
            return 1;
        if (!o->subtree)
            return 0;
    }
    return 0;
}

static int attr_passes_filter(JsMutationObserver *o, const char *name)
{
    int i;
    if (!o->attr_filter_count)
        return 1;
    if (!name)
        return 0;
    for (i = 0; i < o->attr_filter_count; i++)
        if (o->attr_filter[i] && !strcmp(o->attr_filter[i], name))
            return 1;
    return 0;
}

/* ---------------- delivery ---------------- */

static JsValue record_to_object(HpeJs *js, JsMutationObserver *o, JsMutationRecord *r)
{
    JsObj *obj = js_object_new(js);
    const char *type = r->type == HPE_MUT_ATTR ? "attributes"
                       : r->type == HPE_MUT_TEXT ? "characterData"
                                                 : "childList";
    js_obj_set(js, obj, "type", js_string(js, type));
    js_obj_set(js, obj, "target", hpe_js_dom_node_value(js, r->target));
    if (r->type == HPE_MUT_ATTR) {
        js_obj_set(js, obj, "attributeName",
                   js_string(js, r->attribute_name ? r->attribute_name : ""));
        if (o->attr_old_value && r->old_value)
            js_obj_set(js, obj, "oldValue", js_string(js, r->old_value));
        else
            js_obj_set(js, obj, "oldValue", js_null());
    } else if (r->type == HPE_MUT_TEXT) {
        if (o->char_old_value && r->old_value)
            js_obj_set(js, obj, "oldValue", js_string(js, r->old_value));
        else
            js_obj_set(js, obj, "oldValue", js_null());
    } else {
        js_obj_set(js, obj, "oldValue", js_null());
    }
    return js_objval(obj);
}

/* builds the JS array of records, clearing the observer's queue */
static JsValue take_records_value(HpeJs *js, JsMutationObserver *o)
{
    JsObj *arr = js_array_new(js);
    JsMutationRecord *r = o->head;
    char key[32];
    int n = 0;
    o->head = o->tail = NULL;
    for (; r; r = r->next) {
        snprintf(key, sizeof(key), "%d", n++);
        js_obj_set(js, arr, key, record_to_object(js, o, r));
    }
    js_obj_set(js, arr, "length", js_num((double)n));
    return js_objval(arr);
}

/* the microtask body: deliver every record queued so far */
static JsValue observer_deliver(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    JsMutationObserver *o = (JsMutationObserver *)ud;
    JsValue recs, cbargs[1], cbthis;
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    if (!o)
        return js_undef();
    o->queued = 0;
    if (!o->head)
        return js_undef();
    recs = take_records_value(js, o);
    cbargs[0] = recs;
    cbthis = js_undef();
    hpe_js_call(js, o->callback, cbthis, cbargs, 1);
    return js_undef();
}

/* queues one microtask so delivery is asynchronous, as in browsers */
static void queue_delivery(HpeJs *js, JsMutationObserver *o)
{
    JsObj *fn;
    if (o->queued)
        return;
    o->queued = 1;
    fn = js_native_new(js, observer_deliver, o);
    hpe_js_queue_microtask(js, js_objval(fn), js_undef());
}

/* ---------------- DOM sink ---------------- */

static void mutation_bridge(void *ud, int kind, HpeDomNode *target, const char *name,
                            const char *old_value, HpeDomNode *added, HpeDomNode *removed)
{
    HpeJs *js = (HpeJs *)ud;
    HpeJsMutationState *st = hpe_js_mutation_state(js);
    JsMutationObserver *o;
    HPE_UNUSED(added);
    HPE_UNUSED(removed);

    if (!st)
        return;
    for (o = st->observers; o; o = o->next) {
        int match = 0;
        JsMutationRecord *r;

        if (kind == HPE_MUT_CHILD_LIST)
            match = o->child_list && observes_node(o, target);
        else if (kind == HPE_MUT_ATTR)
            match = o->attributes && observes_node(o, target) && attr_passes_filter(o, name);
        else if (kind == HPE_MUT_TEXT)
            match = o->character_data && observes_node(o, target);
        if (!match)
            continue;

        r = record_new(js);
        r->type = kind;
        r->target = target;
        if (kind == HPE_MUT_ATTR && name)
            r->attribute_name = hpe_js_strdup(js, name);
        if (old_value)
            r->old_value = hpe_js_strdup(js, old_value);
        record_push(o, r);
        queue_delivery(js, o);
    }
}

/* Registers the bridge sink on the DOM layer (once per document). */
void hpe_js_mutation_install(HpeJs *js)
{
    HpeJsMutationState *st = hpe_js_mutation_state(js);
    if (!st || st->sink_installed)
        return;
    st->sink_installed = 1;
    st->js = js;
    hpe_dom_add_mutation_sink(mutation_bridge, js);
}

void hpe_js_mutation_uninstall(HpeJs *js)
{
    HpeJsMutationState *st = js ? js->mutation_state : NULL;
    if (!st || !st->sink_installed)
        return;
    hpe_dom_remove_mutation_sink(mutation_bridge, js);
    st->sink_installed = 0;
}

/* ---------------- observer methods ---------------- */

static JsValue observer_observe(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    JsMutationObserver *o = (JsMutationObserver *)ud;
    HpeDomNode *node;
    JsValue opts;
    HPE_UNUSED(thisv);
    if (!o || argc < 1)
        return js_undef();
    node = (HpeDomNode *)js_host_ptr(args[0], HPE_HOST_ELEMENT);
    if (!node)
        node = (HpeDomNode *)js_host_ptr(args[0], HPE_HOST_FRAGMENT);
    if (!node)
        return js_undef();
    o->target = node;
    if (argc < 2 || args[1].t != JS_OBJ || !args[1].obj)
        return js_undef();
    opts = args[1];
    o->child_list = js_truthy(js_obj_get(js, opts.obj, "childList"));
    o->attributes = js_truthy(js_obj_get(js, opts.obj, "attributes"));
    o->character_data = js_truthy(js_obj_get(js, opts.obj, "characterData"));
    o->subtree = js_truthy(js_obj_get(js, opts.obj, "subtree"));
    o->attr_old_value = js_truthy(js_obj_get(js, opts.obj, "attributeOldValue"));
    o->char_old_value = js_truthy(js_obj_get(js, opts.obj, "characterDataOldValue"));
    {
        JsValue filter = js_obj_get(js, opts.obj, "attributeFilter");
        if (filter.t == JS_OBJ && filter.obj) {
            JsValue len = js_obj_get(js, filter.obj, "length");
            int n = (int)js_to_number(js, len), i;
            if (n < 0)
                n = 0;
            o->attr_filter = (char **)hpe_js_alloc(js, sizeof(char *) * (size_t)(n ? n : 1));
            o->attr_filter_count = 0;
            for (i = 0; i < n; i++) {
                char key[16];
                snprintf(key, sizeof(key), "%d", i);
                o->attr_filter[o->attr_filter_count++] =
                    hpe_js_strdup(js, js_to_string(js, js_obj_get(js, filter.obj, key)));
            }
        }
    }
    return js_undef();
}

static JsValue observer_disconnect(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    JsMutationObserver *o = (JsMutationObserver *)ud;
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    if (o) {
        o->target = NULL;
        o->head = o->tail = NULL;
        o->queued = 0;
    }
    return js_undef();
}

static JsValue observer_take_records(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    JsMutationObserver *o = (JsMutationObserver *)ud;
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    if (!o)
        return js_objval(js_array_new(js));
    return take_records_value(js, o);
}

/* MutationObserver(<callback>) */
static JsValue observer_ctor(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeJsMutationState *st = hpe_js_mutation_state(js);
    JsMutationObserver *o = (JsMutationObserver *)hpe_js_alloc(js, sizeof(JsMutationObserver));
    JsObj *obj;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    memset(o, 0, sizeof(*o));
    o->callback = argc > 0 ? args[0] : js_undef();
    obj = js_host_new(js, o, HPE_HOST_MUTATION_OBSERVER, NULL);
    if (st) {
        o->next = st->observers;
        st->observers = o;
    }
    /* attach the methods as ordinary own properties */
    js_obj_set(js, obj, "observe", js_objval(js_native_new(js, observer_observe, o)));
    js_obj_set(js, obj, "disconnect", js_objval(js_native_new(js, observer_disconnect, o)));
    js_obj_set(js, obj, "takeRecords", js_objval(js_native_new(js, observer_take_records, o)));
    return js_objval(obj);
}

/* installs the global MutationObserver constructor */
void hpe_js_mutation_install_ctor(HpeJs *js)
{
    JsObj *fn;
    if (!js)
        return;
    fn = js_native_new(js, observer_ctor, NULL);
    js_obj_set(js, fn, "name", js_string(js, "MutationObserver"));
    js_obj_set(js, js->global_obj, "MutationObserver", js_objval(fn));
}
