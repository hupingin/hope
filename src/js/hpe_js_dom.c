/* Hope UI Engine - javascript DOM bridge implementation */
#include "js/hpe_js_dom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "css/hpe_css.h"
#include "html/hpe_html.h"
#include "js/hpe_js_internal.h"
#include "js/hpe_js_mutation.h"
#include "layout/hpe_layout.h"

/* ---------------- small malloc string builder ---------------- */

typedef struct {
    char *s;
    size_t len;
    size_t cap;
} SB;

static void sb_init(SB *sb)
{
    sb->cap = 256;
    sb->len = 0;
    sb->s = (char *)malloc(sb->cap);
    sb->s[0] = '\0';
}

static void sb_add(SB *sb, const char *s, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        while (sb->len + n + 1 > sb->cap)
            sb->cap *= 2;
        sb->s = (char *)realloc(sb->s, sb->cap);
    }
    memcpy(sb->s + sb->len, s, n);
    sb->len += n;
    sb->s[sb->len] = '\0';
}

static void sb_free(SB *sb)
{
    free(sb->s);
    sb->s = NULL;
}

/* ---------------- helpers ---------------- */

static const HpeJsHostVt g_element_vt;
static const HpeJsHostVt g_style_vt;
static const HpeJsHostVt g_document_vt;
static const HpeJsHostVt g_fragment_vt;

/* wraps any DOM node in its host value. a DocumentFragment gets the fragment
 * vtable (no tagName/id/style, but the child-list mutators); everything else is
 * exposed through the element vtable. */
static JsValue node_value(HpeJs *js, HpeDomNode *node)
{
    if (!node)
        return js_null();
    if (node->type == HPE_NODE_FRAGMENT)
        return js_make_host(js, node, HPE_HOST_FRAGMENT, &g_fragment_vt);
    return js_make_host(js, node, HPE_HOST_ELEMENT, &g_element_vt);
}

/* unwraps a node argument; accepts both element and fragment hosts */
static HpeDomNode *arg_node(JsValue v)
{
    HpeDomNode *n = (HpeDomNode *)js_host_ptr(v, HPE_HOST_ELEMENT);
    if (!n)
        n = (HpeDomNode *)js_host_ptr(v, HPE_HOST_FRAGMENT);
    return n;
}

/* public wrapper over node_value so the MutationObserver bridge can reuse it */
JsValue hpe_js_dom_node_value(HpeJs *js, HpeDomNode *node)
{
    return node_value(js, node);
}

static char *arg_string(HpeJs *js, JsValue *args, int argc, int idx)
{
    if (idx >= argc)
        return NULL;
    return js_to_string(js, args[idx]);
}

/* defined in the events section below; declared here so the method dispatcher can use it */
static void listener_append(HpeJs *js, HpeDomNode *node, const char *type, JsValue handler,
                            int via_on);

/* camelCase js property -> dashed lowercase css property */
static void css_name_from_js(const char *jsname, char *out, size_t out_size)
{
    size_t o = 0, i;
    for (i = 0; jsname[i] && o + 2 < out_size; i++) {
        char c = jsname[i];
        if (c >= 'A' && c <= 'Z') {
            out[o++] = '-';
            out[o++] = (char)(c - 'A' + 'a');
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

/* ---------------- text / html accessors ---------------- */

static void collect_text(HpeDomNode *node, SB *sb)
{
    HpeDomNode *child;
    for (child = node->first_child; child; child = child->next_sibling) {
        if (child->type == HPE_NODE_TEXT) {
            if (child->text)
                sb_add(sb, child->text, strlen(child->text));
        } else if (child->type == HPE_NODE_ELEMENT) {
            collect_text(child, sb);
        }
    }
}

static void serialize_node(HpeDomNode *node, SB *sb)
{
    int i;
    if (node->type == HPE_NODE_TEXT) {
        if (node->text)
            sb_add(sb, node->text, strlen(node->text));
        return;
    }
    if (node->type != HPE_NODE_ELEMENT)
        return;
    sb_add(sb, "<", 1);
    sb_add(sb, node->tag, strlen(node->tag));
    for (i = 0; i < node->attr_count; i++) {
        sb_add(sb, " ", 1);
        sb_add(sb, node->attrs[i].name, strlen(node->attrs[i].name));
        sb_add(sb, "=\"", 2);
        sb_add(sb, node->attrs[i].value, strlen(node->attrs[i].value));
        sb_add(sb, "\"", 1);
    }
    sb_add(sb, ">", 1);
    {
        HpeDomNode *child;
        for (child = node->first_child; child; child = child->next_sibling)
            serialize_node(child, sb);
    }
    sb_add(sb, "</", 2);
    sb_add(sb, node->tag, strlen(node->tag));
    sb_add(sb, ">", 1);
}

static JsValue element_get_text(HpeJs *js, HpeDomNode *node)
{
    SB sb;
    JsValue v;
    sb_init(&sb);
    collect_text(node, &sb);
    v = js_string_len(js, sb.s, sb.len);
    sb_free(&sb);
    return v;
}

static JsValue element_get_html(HpeJs *js, HpeDomNode *node)
{
    SB sb;
    JsValue v;
    HpeDomNode *child;
    sb_init(&sb);
    for (child = node->first_child; child; child = child->next_sibling)
        serialize_node(child, &sb);
    v = js_string_len(js, sb.s, sb.len);
    sb_free(&sb);
    return v;
}

static void node_replace_children_with_text(HpeDomNode *node, const char *text)
{
    HpeDomNode *child, *next;
    /* detach each child first so the mutation sink sees the removals
     * (MutationObserver + engine dirty tracking); appending notifies the add. */
    for (child = node->first_child; child; child = next) {
        next = child->next_sibling;
        hpe_dom_detach(child);
        hpe_dom_free(child);
    }
    hpe_dom_append(node, hpe_dom_text(text ? text : ""));
}

static void node_set_inner_html(HpeDomNode *node, const char *html)
{
    HpeDomNode *frag;
    HpeDomNode *body;
    HpeDomNode *child, *next;

    node_replace_children_with_text(node, "");
    frag = hpe_html_parse(html ? html : "", html ? strlen(html) : 0);
    body = hpe_dom_find_tag(frag, "body");
    for (child = body ? body->first_child : NULL; child; child = next) {
        next = child->next_sibling;
        hpe_dom_detach(child);
        hpe_dom_append(node, child);
    }
    hpe_dom_free(frag);
}

/* ---------------- querySelector ---------------- */

/* collects elements in document order whose subtree root is `node` (node itself
 * excluded) into a growable array. returns the element count. */
static int collect_elements(HpeDomNode *node, HpeDomNode ***out, int *cap, int count)
{
    HpeDomNode *child;
    for (child = node->first_child; child; child = child->next_sibling) {
        if (child->type != HPE_NODE_ELEMENT)
            continue;
        if (count >= *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *out = (HpeDomNode **)realloc(*out, sizeof(HpeDomNode *) * (size_t)*cap);
        }
        (*out)[count++] = child;
        count = collect_elements(child, out, cap, count);
    }
    return count;
}

static JsValue query_selector_impl(HpeJs *js, HpeDomNode *node, JsValue *args, int argc,
                                   int all)
{
    char *sel = arg_string(js, args, argc, 0);
    HpeDomNode **elems = NULL;
    int cap = 0, count, i;
    JsValue result;

    if (!sel || !*sel) {
        if (all)
            return js_objval(js_array_new(js));
        return js_null();
    }
    count = collect_elements(node, &elems, &cap, 0);
    if (all) {
        JsObj *arr = js_array_new(js);
        char key[32];
        int n = 0;
        for (i = 0; i < count; i++) {
            if (hpe_css_selector_matches(elems[i], sel, strlen(sel))) {
                snprintf(key, sizeof(key), "%d", n++);
                js_obj_set(js, arr, key, node_value(js, elems[i]));
            }
        }
        js_obj_set(js, arr, "length", js_num((double)n));
        result = js_objval(arr);
    } else {
        result = js_null();
        for (i = 0; i < count; i++) {
            if (hpe_css_selector_matches(elems[i], sel, strlen(sel))) {
                result = node_value(js, elems[i]);
                break;
            }
        }
    }
    free(elems);
    return result;
}

/* ---------------- bound methods ---------------- */

typedef struct {
    HpeDomNode *node;
    char *method;
} DomMth;

/* breadth-first search for the first element whose id attribute matches. */
static HpeDomNode *dom_find_by_id(HpeDomNode *node, const char *id)
{
    HpeDomNode **queue;
    int qh = 0, qt = 0, qcap = 1024;
    HpeDomNode *found = NULL;

    queue = (HpeDomNode **)malloc(sizeof(HpeDomNode *) * (size_t)qcap);
    if (!queue)
        return NULL;
    queue[qt++] = node;
    while (qh < qt) {
        HpeDomNode *n = queue[qh++];
        HpeDomNode *cur;
        for (cur = n->first_child; cur; cur = cur->next_sibling) {
            if (cur->type != HPE_NODE_ELEMENT)
                continue;
            const char *aid = hpe_dom_attr(cur, "id");
            if (aid && strcmp(aid, id) == 0) {
                found = cur;
                break;
            }
            if (qt < qcap)
                queue[qt++] = cur;
        }
        if (found)
            break;
    }
    free(queue);
    return found;
}

typedef JsValue (*DomMethodFn)(HpeJs *js, HpeDomNode *node, JsValue *args, int argc);

static JsValue dm_get_element_by_id(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    char *id = arg_string(js, args, argc, 0);
    HpeDomNode *found;
    if (!id)
        return js_null();
    found = dom_find_by_id(node, id);
    return found ? node_value(js, found) : js_null();
}

static JsValue dm_create_element(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    char *tag = arg_string(js, args, argc, 0);
    HPE_UNUSED(node);
    if (!tag || !*tag)
        return js_null();
    return node_value(js, hpe_dom_element(tag));
}

static JsValue dm_create_element_ns(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    /* namespace is irrelevant for our element model (SVG children are
     * drawn by tag name); only the tag matters */
    char *tag = arg_string(js, args, argc, 1);
    HPE_UNUSED(node);
    if (!tag || !*tag)
        return js_null();
    return node_value(js, hpe_dom_element(tag));
}

static JsValue dm_create_fragment(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HPE_UNUSED(node);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    return node_value(js, hpe_dom_fragment());
}

static JsValue dm_remove_child(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HpeDomNode *child = argc > 0 ? arg_node(args[0]) : NULL;
    HPE_UNUSED(js);
    if (child && child->parent == node)
        hpe_dom_detach(child);
    return js_undef();
}

static JsValue dm_clone_node(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    int deep = argc > 0 && js_truthy(args[0]);
    return node_value(js, hpe_dom_clone(node, deep));
}

static JsValue dm_has_child_nodes(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HPE_UNUSED(js);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    return js_bool(node->first_child != NULL);
}

static JsValue dm_insert_before(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HpeDomNode *child = argc > 0 ? arg_node(args[0]) : NULL;
    HpeDomNode *ref = argc > 1 ? arg_node(args[1]) : NULL;
    HPE_UNUSED(js);
    if (!child)
        return js_null();
    hpe_dom_insert_before(node, child, ref);
    return node_value(js, child);
}

static JsValue dm_replace_child(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HpeDomNode *newc = argc > 0 ? arg_node(args[0]) : NULL;
    HpeDomNode *oldc = argc > 1 ? arg_node(args[1]) : NULL;
    HPE_UNUSED(js);
    if (!oldc)
        return js_null();
    return node_value(js, hpe_dom_replace_child(node, newc, oldc));
}

static JsValue dm_get_attribute(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    char *name = arg_string(js, args, argc, 0);
    const char *v = name ? hpe_dom_attr(node, name) : NULL;
    return v ? js_string(js, v) : js_null();
}

static JsValue dm_set_attribute(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    char *name = arg_string(js, args, argc, 0);
    char *value = arg_string(js, args, argc, 1);
    HPE_UNUSED(js);
    if (name)
        hpe_dom_set_attr(node, name, value ? value : "");
    return js_undef();
}

static JsValue dm_remove_attribute(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    char *name = arg_string(js, args, argc, 0);
    HPE_UNUSED(js);
    if (name)
        hpe_dom_remove_attr(node, name);
    return js_undef();
}

static JsValue dm_has_attribute(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    char *name = arg_string(js, args, argc, 0);
    return js_bool(name && hpe_dom_attr(node, name) != NULL);
}

static JsValue dm_append_child(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HpeDomNode *child = argc > 0 ? arg_node(args[0]) : NULL;
    if (child) {
        hpe_dom_append(node, child);
        return node_value(js, child);
    }
    return js_null();
}

static JsValue dm_remove_self(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HPE_UNUSED(js);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    hpe_dom_detach(node);
    return js_undef();
}

static JsValue dm_query_selector(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    return query_selector_impl(js, node, args, argc, 0);
}

static JsValue dm_query_selector_all(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    return query_selector_impl(js, node, args, argc, 1);
}

static JsValue dm_add_event_listener(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    char *type = arg_string(js, args, argc, 0);
    JsValue handler = argc > 1 ? args[1] : js_undef();
    if (type && *type)
        listener_append(js, node, type, handler, 0);
    return js_undef();
}

/* method-name -> handler registry; the dispatch below is pure table lookup so
 * adding a DOM method is a one-line table edit, not a new branch. */
static const struct {
    const char *name;
    DomMethodFn fn;
} dom_method_table[] = {
    { "getElementById",         dm_get_element_by_id },
    { "createElement",          dm_create_element },
    { "createElementNS",        dm_create_element_ns },
    { "createDocumentFragment", dm_create_fragment },
    { "removeChild",            dm_remove_child },
    { "cloneNode",              dm_clone_node },
    { "hasChildNodes",          dm_has_child_nodes },
    { "insertBefore",           dm_insert_before },
    { "replaceChild",           dm_replace_child },
    { "getAttribute",           dm_get_attribute },
    { "setAttribute",           dm_set_attribute },
    { "removeAttribute",        dm_remove_attribute },
    { "hasAttribute",           dm_has_attribute },
    { "appendChild",            dm_append_child },
    { "remove",                 dm_remove_self },
    { "querySelector",          dm_query_selector },
    { "querySelectorAll",       dm_query_selector_all },
    { "addEventListener",       dm_add_event_listener },
    { NULL, NULL }
};

static JsValue dom_method_dispatch(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    DomMth *m = (DomMth *)ud;
    int i;
    HPE_UNUSED(thisv);
    for (i = 0; dom_method_table[i].name; i++) {
        if (strcmp(m->method, dom_method_table[i].name) == 0)
            return dom_method_table[i].fn(js, m->node, args, argc);
    }
    return js_undef();
}

static JsValue bind_dom_method(HpeJs *js, HpeDomNode *node, const char *method)
{
    DomMth *m = (DomMth *)hpe_js_alloc(js, sizeof(DomMth));
    m->node = node;
    m->method = hpe_js_strdup(js, method);
    return js_objval(js_native_new(js, dom_method_dispatch, m));
}

static int method_is(const char *prop)
{
    return !strcmp(prop, "getElementById") || !strcmp(prop, "createElement") ||
           !strcmp(prop, "createElementNS") || !strcmp(prop, "createDocumentFragment") ||
           !strcmp(prop, "getAttribute") || !strcmp(prop, "setAttribute") ||
           !strcmp(prop, "removeAttribute") || !strcmp(prop, "hasAttribute") ||
           !strcmp(prop, "appendChild") || !strcmp(prop, "removeChild") ||
           !strcmp(prop, "insertBefore") || !strcmp(prop, "replaceChild") ||
           !strcmp(prop, "cloneNode") || !strcmp(prop, "hasChildNodes") ||
           !strcmp(prop, "remove") || !strcmp(prop, "querySelector") ||
           !strcmp(prop, "querySelectorAll") || !strcmp(prop, "addEventListener");
}

/* ---------------- events ---------------- */

typedef struct JsListener {
    char *type;
    JsValue handler;
    int via_on; /* added through an onclick-style property (one per type) */
    struct JsListener *next;
} JsListener;

static const char *on_prop_event(const char *prop)
{
    static const char *evts[] = {"click", "mousedown", "mouseup", NULL};
    int i;
    if (prop[0] != 'o' || prop[1] != 'n')
        return NULL;
    for (i = 0; evts[i]; i++)
        if (!strcmp(prop + 2, evts[i]))
            return evts[i];
    return NULL;
}

static JsListener *node_listeners(HpeDomNode *node)
{
    return (JsListener *)node->listeners;
}

/* stores a listener on a node. via_on handlers replace any previous via_on of the
 * same type; a null/undefined handler removes it. list nodes live in the js arena. */
static void listener_append(HpeJs *js, HpeDomNode *node, const char *type, JsValue handler,
                            int via_on)
{
    JsListener *head = node_listeners(node);
    if (via_on) {
        JsListener **pp = &head;
        while (*pp) {
            JsListener *l = *pp;
            if (l->via_on && !strcmp(l->type, type)) {
                *pp = l->next;
                break;
            }
            pp = &l->next;
        }
    }
    if (handler.t == JS_UNDEF || handler.t == JS_NULL) {
        node->listeners = head;
        return;
    }
    {
        JsListener *l = (JsListener *)hpe_js_alloc(js, sizeof(JsListener));
        l->type = hpe_js_strdup(js, type);
        l->handler = handler;
        l->via_on = via_on;
        l->next = head;
        node->listeners = l;
    }
}

static JsValue on_handler_get(HpeDomNode *node, const char *type)
{
    JsListener *l;
    for (l = node_listeners(node); l; l = l->next)
        if (l->via_on && !strcmp(l->type, type))
            return l->handler;
    return js_null();
}

static JsValue native_noop(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_undef();
}

/* stopPropagation()/stopImmediatePropagation() are recorded on the event object
 * itself: the dispatch loop below re-checks the flag after every handler. */
static JsValue native_stop_propagation(HpeJs *js, JsValue thisv, JsValue *args, int argc,
                                       void *ud)
{
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (thisv.t == JS_OBJ && thisv.obj)
        js_obj_set(js, thisv.obj, "_stopped", js_bool(1));
    return js_undef();
}

static int propagation_stopped(HpeJs *js, JsValue ev)
{
    JsValue v;
    if (ev.t != JS_OBJ || !ev.obj)
        return 0;
    v = js_obj_get(js, ev.obj, "_stopped");
    return v.t == JS_BOOL && v.num != 0.0;
}

static JsValue event_object(HpeJs *js, const char *type, int x, int y, HpeDomNode *target)
{
    JsObj *o = js_object_new(js);
    js_obj_set(js, o, "type", js_string(js, type));
    js_obj_set(js, o, "clientX", js_num((double)x));
    js_obj_set(js, o, "clientY", js_num((double)y));
    js_obj_set(js, o, "target", node_value(js, target));
    js_obj_set(js, o, "preventDefault", js_objval(js_native_new(js, native_noop, NULL)));
    js_obj_set(js, o, "stopPropagation",
               js_objval(js_native_new(js, native_stop_propagation, NULL)));
    js_obj_set(js, o, "stopImmediatePropagation",
               js_objval(js_native_new(js, native_stop_propagation, NULL)));
    return js_objval(o);
}

int hpe_js_dom_dispatch(HpeJs *js, HpeDomNode *target, const char *type, int clientX,
                        int clientY)
{
    HpeDomNode *node = target;
    int fired = 0;
    JsValue args[1];
    if (!js || !target)
        return 0;
    args[0] = event_object(js, type, clientX, clientY, target);
    while (node) {
        JsListener *l;
        js_obj_set(js, args[0].obj, "currentTarget", node_value(js, node));
        for (l = node_listeners(node); l; l = l->next) {
            if (!strcmp(l->type, type)) {
                hpe_js_call(js, l->handler, js_undef(), args, 1);
                fired = 1;
                if (propagation_stopped(js, args[0]))
                    return fired;
            }
        }
        node = node->parent;
    }
    return fired;
}


/* ---------------- vtables ---------------- */

static JsValue element_upper_tag(HpeJs *js, HpeDomNode *node)
{
    /* browsers report element names uppercased for HTML documents */
    const char *t = node->tag ? node->tag : "";
    char upper[64];
    size_t u = 0;
    while (t[u] && u + 1 < sizeof(upper)) {
        char c = t[u];
        upper[u] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        u++;
    }
    upper[u] = '\0';
    return js_string(js, upper);
}

static JsValue element_attr_str(HpeJs *js, HpeDomNode *node, const char *attr)
{
    const char *v = hpe_dom_attr(node, attr);
    return js_string(js, v ? v : "");
}

static JsValue element_client_size(HpeJs *js, HpeDomNode *node, const char *prop)
{
    /* browsers force a layout here; we keep element -> box back pointers
     * (set by hpe_layout_build) so the value is the laid-out content size */
    HpeBox *box = (HpeBox *)node->box;
    double v = 0.0;
    if (box)
        v = (double)(prop[6] == 'W' ? box->content_box.w : box->content_box.h);
    return js_num(v);
}

typedef JsValue (*ElemGetFn)(HpeJs *js, HpeDomNode *node, const char *prop);

static JsValue eg_node_type(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(node);
    HPE_UNUSED(prop);
    return js_num(1); /* ELEMENT_NODE */
}

static JsValue eg_tag_name(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return element_upper_tag(js, node);
}

static JsValue eg_id(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return element_attr_str(js, node, "id");
}

static JsValue eg_class_name(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return element_attr_str(js, node, "class");
}

static JsValue eg_text_content(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return element_get_text(js, node);
}

static JsValue eg_inner_html(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return element_get_html(js, node);
}

static JsValue eg_style(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return js_make_host(js, node, HPE_HOST_STYLE, &g_style_vt);
}

static JsValue eg_first_child(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return node->first_child ? node_value(js, node->first_child) : js_null();
}

static JsValue eg_next_sibling(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return node->next_sibling ? node_value(js, node->next_sibling) : js_null();
}

static JsValue eg_parent_node(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return node->parent ? node_value(js, node->parent) : js_null();
}

static JsValue eg_client_size(HpeJs *js, HpeDomNode *node, const char *prop)
{
    return element_client_size(js, node, prop);
}

static const struct {
    const char *name;
    ElemGetFn fn;
} elem_get_table[] = {
    { "nodeType",     eg_node_type },
    { "tagName",      eg_tag_name },
    { "nodeName",     eg_tag_name },
    { "id",           eg_id },
    { "className",    eg_class_name },
    { "textContent",  eg_text_content },
    { "innerHTML",    eg_inner_html },
    { "style",        eg_style },
    { "firstChild",   eg_first_child },
    { "nextSibling",  eg_next_sibling },
    { "parentNode",   eg_parent_node },
    { "clientWidth",  eg_client_size },
    { "clientHeight", eg_client_size },
    { NULL, NULL }
};

static int element_vt_get(HpeJs *js, void *target, const char *prop, JsValue *out)
{
    HpeDomNode *node = (HpeDomNode *)target;
    int i;
    if (!node || node->type != HPE_NODE_ELEMENT)
        return 0;
    if (!strcmp(prop, "content") && node->tag && !strcmp(node->tag, "template")) {
        /* <template>.content: the detached fragment holding its children */
        *out = node_value(js, hpe_dom_children_root(node));
        return 1;
    }
    for (i = 0; elem_get_table[i].name; i++) {
        if (!strcmp(prop, elem_get_table[i].name)) {
            *out = elem_get_table[i].fn(js, node, prop);
            return 1;
        }
    }
    if (method_is(prop)) {
        *out = bind_dom_method(js, node, prop);
        return 1;
    }
    if (prop[0] == 'o' && prop[1] == 'n') {
        const char *ev = on_prop_event(prop);
        if (ev) {
            *out = on_handler_get(node, ev);
            return 1;
        }
    }
    return 0;
}

static int element_vt_set(HpeJs *js, void *target, const char *prop, JsValue value)
{
    HpeDomNode *node = (HpeDomNode *)target;
    if (!node || node->type != HPE_NODE_ELEMENT)
        return 0;
    if (!strcmp(prop, "textContent")) {
        node_replace_children_with_text(node, js_to_string(js, value));
        return 1;
    }
    if (!strcmp(prop, "innerHTML")) {
        node_set_inner_html(node, js_to_string(js, value));
        return 1;
    }
    if (!strcmp(prop, "style")) {
        hpe_dom_set_attr(node, "style", js_to_string(js, value));
        return 1;
    }
    if (!strcmp(prop, "id")) {
        hpe_dom_set_attr(node, "id", js_to_string(js, value));
        return 1;
    }
    if (!strcmp(prop, "className")) {
        hpe_dom_set_attr(node, "class", js_to_string(js, value));
        return 1;
    }
    if (prop[0] == 'o' && prop[1] == 'n') {
        const char *ev = on_prop_event(prop);
        if (ev) {
            listener_append(js, node, ev, value, 1);
            return 1;
        }
    }
    return 0;
}

static const HpeJsHostVt g_element_vt = {element_vt_get, element_vt_set};

/* DocumentFragment host: shares the node-level accessors (children, tree links,
 * methods) with elements but exposes no element-only members (tagName/id/style).
 * A fragment never reaches the box tree, so clientWidth/Height stay 0. */
static int fragment_vt_get(HpeJs *js, void *target, const char *prop, JsValue *out)
{
    HpeDomNode *node = (HpeDomNode *)target;
    if (!node || node->type != HPE_NODE_FRAGMENT)
        return 0;
    if (!strcmp(prop, "nodeType")) {
        *out = js_num(11); /* DOCUMENT_FRAGMENT_NODE */
        return 1;
    }
    if (!strcmp(prop, "childNodes") || !strcmp(prop, "children")) {
        /* a live-ish snapshot built on each read; enough for len/iteration */
        JsObj *arr = js_array_new(js);
        HpeDomNode *c;
        char key[32];
        int n = 0;
        for (c = node->first_child; c; c = c->next_sibling) {
            snprintf(key, sizeof(key), "%d", n++);
            js_obj_set(js, arr, key, node_value(js, c));
        }
        js_obj_set(js, arr, "length", js_num((double)n));
        *out = js_objval(arr);
        return 1;
    }
    if (!strcmp(prop, "firstChild")) {
        *out = node->first_child ? node_value(js, node->first_child) : js_null();
        return 1;
    }
    if (!strcmp(prop, "nextSibling")) {
        *out = node->next_sibling ? node_value(js, node->next_sibling) : js_null();
        return 1;
    }
    if (!strcmp(prop, "parentNode")) {
        *out = node->parent ? node_value(js, node->parent) : js_null();
        return 1;
    }
    if (!strcmp(prop, "textContent")) {
        *out = element_get_text(js, node);
        return 1;
    }
    if (!strcmp(prop, "innerHTML")) {
        *out = element_get_html(js, node);
        return 1;
    }
    if (method_is(prop)) {
        *out = bind_dom_method(js, node, prop);
        return 1;
    }
    return 0;
}

static int fragment_vt_set(HpeJs *js, void *target, const char *prop, JsValue value)
{
    HpeDomNode *node = (HpeDomNode *)target;
    if (!node || node->type != HPE_NODE_FRAGMENT)
        return 0;
    if (!strcmp(prop, "textContent")) {
        node_replace_children_with_text(node, js_to_string(js, value));
        return 1;
    }
    if (!strcmp(prop, "innerHTML")) {
        node_set_inner_html(node, js_to_string(js, value));
        return 1;
    }
    return 0;
}

static const HpeJsHostVt g_fragment_vt = {fragment_vt_get, fragment_vt_set};

static int style_vt_get(HpeJs *js, void *target, const char *prop, JsValue *out)
{
    HpeDomNode *node = (HpeDomNode *)target;
    char cssname[64];
    char val[512];
    if (!node)
        return 0;
    css_name_from_js(prop, cssname, sizeof(cssname));
    if (hpe_css_inline_get(node, cssname, val, sizeof(val))) {
        *out = js_string(js, val);
        return 1;
    }
    *out = js_string(js, "");
    return 1;
}

static int style_vt_set(HpeJs *js, void *target, const char *prop, JsValue value)
{
    HpeDomNode *node = (HpeDomNode *)target;
    char cssname[64];
    char *v;
    if (!node)
        return 0;
    css_name_from_js(prop, cssname, sizeof(cssname));
    v = js_to_string(js, value);
    hpe_css_inline_set(node, cssname, v);
    return 1;
}

static const HpeJsHostVt g_style_vt = {style_vt_get, style_vt_set};

static int document_vt_get(HpeJs *js, void *target, const char *prop, JsValue *out)
{
    HpeDomNode *doc = (HpeDomNode *)target;
    if (!doc)
        return 0;
    if (!strcmp(prop, "body")) {
        *out = node_value(js, hpe_dom_find_tag(doc, "body"));
        return 1;
    }
    if (!strcmp(prop, "head")) {
        *out = node_value(js, hpe_dom_find_tag(doc, "head"));
        return 1;
    }
    if (!strcmp(prop, "documentElement")) {
        *out = node_value(js, hpe_dom_find_tag(doc, "html"));
        return 1;
    }
    if (method_is(prop)) {
        *out = bind_dom_method(js, doc, prop);
        return 1;
    }
    if (prop[0] == 'o' && prop[1] == 'n') {
        const char *ev = on_prop_event(prop);
        if (ev) {
            *out = on_handler_get(doc, ev);
            return 1;
        }
    }
    return 0;
}

static int document_vt_set(HpeJs *js, void *target, const char *prop, JsValue value)
{
    HpeDomNode *doc = (HpeDomNode *)target;
    if (!doc)
        return 0;
    if (!strcmp(prop, "title")) {
        HpeDomNode *head = hpe_dom_find_tag(doc, "head");
        HpeDomNode *title = head ? hpe_dom_find_tag(head, "title") : NULL;
        char *text = js_to_string(js, value);
        if (title)
            node_replace_children_with_text(title, text);
        return 1;
    }
    if (prop[0] == 'o' && prop[1] == 'n') {
        const char *ev = on_prop_event(prop);
        if (ev) {
            listener_append(js, doc, ev, value, 1);
            return 1;
        }
    }
    return 0;
}

static const HpeJsHostVt g_document_vt = {document_vt_get, document_vt_set};

/* the engine only ever loads local files, so `location` is a plain data object
 * (`hash` stays writable because pages use it for view modes such as `#all`) */
static JsValue loc_toString(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_string(js, js_to_string(js, js_obj_get(js, thisv.obj, "href")));
}

static JsValue dom_style_stub(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_objval(js_object_new(js));
}

static JsValue dom_raf(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_num(0.0);
}

static void set_prop(HpeJs *js, JsObj *o, const char *k, JsValue v)
{
    js_obj_set(js, o, k, v);
}

static JsValue nat(HpeJs *js, JsNativeFn fn, void *ud)
{
    return js_objval(js_native_new(js, fn, ud));
}

/* registers `location` / `navigator` / `window` so ordinary page scripts that
 * guard on them keep running (the engine has no network and no navigation) */
static void install_window_builtins(HpeJs *js, HpeDomNode *document)
{
    JsObj *win = js_object_new(js);
    JsObj *loc = js_object_new(js);
    JsObj *nav = js_object_new(js);

    set_prop(js, loc, "protocol", js_string(js, "file:"));
    set_prop(js, loc, "host", js_string(js, ""));
    set_prop(js, loc, "hostname", js_string(js, ""));
    set_prop(js, loc, "port", js_string(js, ""));
    set_prop(js, loc, "pathname", js_string(js, ""));
    set_prop(js, loc, "href", js_string(js, "file:"));
    set_prop(js, loc, "search", js_string(js, ""));
    set_prop(js, loc, "hash", js_string(js, ""));
    set_prop(js, loc, "reload", nat(js, native_noop, NULL));
    set_prop(js, loc, "replace", nat(js, native_noop, NULL));
    set_prop(js, loc, "assign", nat(js, native_noop, NULL));
    set_prop(js, loc, "toString", nat(js, loc_toString, NULL));

    set_prop(js, nav, "userAgent", js_string(js, "Hope/1.0 (local)"));
    set_prop(js, nav, "platform", js_string(js, "local"));
    set_prop(js, nav, "language", js_string(js, "zh-CN"));

    set_prop(js, win, "innerWidth", js_num(0.0));
    set_prop(js, win, "innerHeight", js_num(0.0));
    set_prop(js, win, "devicePixelRatio", js_num(1.0));
    set_prop(js, win, "pageXOffset", js_num(0.0));
    set_prop(js, win, "pageYOffset", js_num(0.0));
    set_prop(js, win, "scrollX", js_num(0.0));
    set_prop(js, win, "scrollY", js_num(0.0));
    set_prop(js, win, "location", js_objval(loc));
    set_prop(js, win, "navigator", js_objval(nav));
    set_prop(js, win, "addEventListener", nat(js, native_noop, NULL));
    set_prop(js, win, "removeEventListener", nat(js, native_noop, NULL));
    set_prop(js, win, "scrollTo", nat(js, native_noop, NULL));
    set_prop(js, win, "scrollBy", nat(js, native_noop, NULL));
    set_prop(js, win, "getComputedStyle", nat(js, dom_style_stub, NULL));
    set_prop(js, win, "requestAnimationFrame", nat(js, dom_raf, NULL));
    set_prop(js, win, "cancelAnimationFrame", nat(js, native_noop, NULL));

    js_obj_set(js, js->global_obj, "location", js_objval(loc));
    js_obj_set(js, js->global_obj, "navigator", js_objval(nav));
    js_obj_set(js, js->global_obj, "window", js_objval(win));
    js_obj_set(js, js->global_obj, "self", js_objval(win));
}

void hpe_js_dom_install(HpeJs *js, HpeDomNode *document)
{
    hpe_js_set_global_host(js, "document", document, HPE_HOST_DOCUMENT, &g_document_vt);
    install_window_builtins(js, document);
    hpe_js_timers_install(js);
    /* DOM change notification: the sink forwards mutations to any observer */
    hpe_js_mutation_install(js);
    hpe_js_mutation_install_ctor(js);
}
