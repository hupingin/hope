/* Hope UI Engine - JavaScript Selection / Range bridge (v0.24)
 *
 * Range objects are plain JS objects holding their endpoints as properties, so
 * no C-side allocation is needed. The Selection object is a host whose getters
 * read the engine's live page selection through HpeJsSelApi.
 */
#include "js/hpe_js_selection.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "js/hpe_js_dom.h"
#include "js/hpe_js_internal.h"

static HpeJsSelApi g_sel_api;

void hpe_js_sel_set_api(const HpeJsSelApi *api)
{
    if (api)
        g_sel_api = *api;
    else
        memset(&g_sel_api, 0, sizeof(g_sel_api));
}

/**
 * @brief 把 JS 参数转成 DOM 节点 / Convert a JS argument into a DOM node
 */
static HpeDomNode *sel_arg_node(JsValue v)
{
    HpeDomNode *n = (HpeDomNode *)js_host_ptr(v, HPE_HOST_ELEMENT);
    if (!n)
        n = (HpeDomNode *)js_host_ptr(v, HPE_HOST_TEXT);
    if (!n)
        n = (HpeDomNode *)js_host_ptr(v, HPE_HOST_DOCUMENT);
    if (!n)
        n = (HpeDomNode *)js_host_ptr(v, HPE_HOST_FRAGMENT);
    return n;
}

/**
 * @brief 读取 Range 对象上的端点 / Read one endpoint from a Range object
 */
static void range_get_end(HpeJs *js, JsValue obj, const char *np, const char *op,
                          HpeDomNode **node, int *off)
{
    *node = sel_arg_node(js_get_prop(js, NULL, obj, np, 0));
    *off = (int)js_to_number(js, js_get_prop(js, NULL, obj, op, 0));
    if (*off < 0)
        *off = 0;
}

/**
 * @brief 写入 Range 对象上的端点 / Write one endpoint onto a Range object
 */
static void range_set_end(HpeJs *js, JsValue obj, const char *np, const char *op,
                          HpeDomNode *node, int off)
{
    js_set_prop(js, NULL, obj, np, hpe_js_dom_node_value(js, node), 0);
    js_set_prop(js, NULL, obj, op, js_num(off), 0);
}

/**
 * @brief 求节点在文档前序中的位置 / Find a node's position in document preorder
 */
static int node_index(HpeDomNode *root, const HpeDomNode *target, int *counter)
{
    HpeDomNode *c;
    int idx;
    if (!root)
        return -1;
    if (root == target)
        return *counter;
    (*counter)++;
    for (c = root->first_child; c; c = c->next_sibling) {
        idx = node_index(c, target, counter);
        if (idx >= 0)
            return idx;
    }
    return -1;
}

/**
 * @brief 统计两个端点在文档前序中的先后 / Compare two endpoints in document order
 */
static int endpoint_order(HpeJs *js, HpeDomNode *an, int ao, HpeDomNode *bn, int bo)
{
    int counter = 0, ai, bi;

    if (!an || !bn)
        return 0;
    ai = node_index(js->dom_document, an, &counter);
    counter = 0;
    bi = node_index(js->dom_document, bn, &counter);
    if (ai == bi)
        return ao - bo;
    return ai - bi;
}

/* ---------------- range text extraction ---------------- */

typedef struct {
    HpeDomNode *start;
    HpeDomNode *end;
    int soff, eoff;
    int active;
    char *out;
    size_t cap, len;
} TextWalk;

/**
 * @brief 把一个文本节点中落在选区内的部分追加到缓冲 / Append the selected slice of a text node
 */
static void walk_text_node(TextWalk *w, HpeDomNode *n)
{
    const char *s = n->text ? n->text : "";
    int len = (int)strlen(s), from = 0, to = len;
    int i;

    if (n == w->start)
        from = w->soff;
    if (n == w->end)
        to = w->eoff;
    if (n == w->start && n == w->end)
        to = w->eoff;
    if (from < 0)
        from = 0;
    if (to > len)
        to = len;
    for (i = from; i < to && w->len + 1 < w->cap; i++)
        w->out[w->len++] = s[i];
    w->out[w->len] = '\0';
}

/**
 * @brief 深度优先遍历文本节点，收集选区覆盖的文字 / Walk text nodes in preorder collecting selected text
 */
static void walk_text(TextWalk *w, HpeDomNode *n)
{
    HpeDomNode *c;
    if (!n)
        return;
    if (n == w->start)
        w->active = 1;
    if (n->type == HPE_NODE_TEXT && w->active)
        walk_text_node(w, n);
    for (c = n->first_child; c; c = c->next_sibling)
        walk_text(w, c);
    if (n == w->end)
        w->active = 0;
}

/**
 * @brief 读取一段端点之间的文本 / Read the text covered by two endpoints
 */
static void range_text(HpeJs *js, HpeDomNode *an, int ao, HpeDomNode *bn, int bo,
                       char *out, size_t cap)
{
    TextWalk w;

    out[0] = '\0';
    if (!cap || !an || !bn)
        return;
    if (endpoint_order(js, an, ao, bn, bo) > 0) {
        HpeDomNode *t = an;
        int i = ao;
        an = bn;
        ao = bo;
        bn = t;
        bo = i;
    }
    memset(&w, 0, sizeof(w));
    w.start = an;
    w.end = bn;
    w.soff = ao;
    w.eoff = bo;
    w.out = out;
    w.cap = cap;
    walk_text(&w, js->dom_document);
}

/* ---------------- Range methods ---------------- */

/**
 * @brief 更新 Range 的 collapsed 标志 / Refresh a Range's collapsed flag
 */
static void range_sync_collapsed(HpeJs *js, JsValue obj)
{
    HpeDomNode *an, *bn;
    int ao, bo;

    range_get_end(js, obj, "startContainer", "startOffset", &an, &ao);
    range_get_end(js, obj, "endContainer", "endOffset", &bn, &bo);
    js_set_prop(js, NULL, obj, "collapsed", js_bool(an == bn && ao == bo), 0);
}

/**
 * @brief 创建一个 Range 对象 / Create a Range JS object
 */
static JsValue make_range(HpeJs *js, HpeDomNode *an, int ao, HpeDomNode *bn, int bo)
{
    JsValue obj = js_objval(js_object_new(js));

    range_set_end(js, obj, "startContainer", "startOffset", an, ao);
    range_set_end(js, obj, "endContainer", "endOffset", bn, bo);
    range_sync_collapsed(js, obj);
    return obj;
}

/**
 * @brief `Range.setStart()` 实现 / Implementation of `Range.setStart()`
 */
static JsValue range_set_start(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *n;
    int off;
    HPE_UNUSED(ud);
    if (!thisv.obj || argc < 2)
        return js_undef();
    n = sel_arg_node(args[0]);
    if (!n)
        return js_undef();
    off = (int)js_to_number(js, args[1]);
    if (off < 0)
        off = 0;
    range_set_end(js, thisv, "startContainer", "startOffset", n, off);
    range_sync_collapsed(js, thisv);
    return js_undef();
}

/**
 * @brief `Range.setEnd()` 实现 / Implementation of `Range.setEnd()`
 */
static JsValue range_set_end_native(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *n;
    int off;
    HPE_UNUSED(ud);
    if (!thisv.obj || argc < 2)
        return js_undef();
    n = sel_arg_node(args[0]);
    if (!n)
        return js_undef();
    off = (int)js_to_number(js, args[1]);
    if (off < 0)
        off = 0;
    range_set_end(js, thisv, "endContainer", "endOffset", n, off);
    range_sync_collapsed(js, thisv);
    return js_undef();
}

/**
 * @brief `Range.collapse()` 实现 / Implementation of `Range.collapse()`
 */
static JsValue range_collapse(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *n, *e;
    int off, eoff;
    HPE_UNUSED(ud);
    if (!thisv.obj)
        return js_undef();
    range_get_end(js, thisv, "startContainer", "startOffset", &n, &off);
    range_get_end(js, thisv, "endContainer", "endOffset", &e, &eoff);
    if (argc > 0 && js_truthy(args[0])) {
        n = e;
        off = eoff;
    }
    range_set_end(js, thisv, "startContainer", "startOffset", n, off);
    range_set_end(js, thisv, "endContainer", "endOffset", n, off);
    js_set_prop(js, NULL, thisv, "collapsed", js_bool(1), 0);
    return js_undef();
}

/**
 * @brief `Range.selectNodeContents()` 实现 / Implementation of `Range.selectNodeContents()`
 */
static JsValue range_select_contents(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *n, *last;
    int len = 0;
    HPE_UNUSED(ud);
    if (!thisv.obj || argc < 1)
        return js_undef();
    n = sel_arg_node(args[0]);
    if (!n)
        return js_undef();
    range_set_end(js, thisv, "startContainer", "startOffset", n, 0);
    last = n;
    {
        HpeDomNode *c;
        for (c = n->first_child; c; c = c->next_sibling)
            if (c->type == HPE_NODE_TEXT)
                last = c;
    }
    if (last->type == HPE_NODE_TEXT)
        len = last->text ? (int)strlen(last->text) : 0;
    range_set_end(js, thisv, "endContainer", "endOffset", last, len);
    range_sync_collapsed(js, thisv);
    return js_undef();
}

/**
 * @brief `Range.toString()` 实现 / Implementation of `Range.toString()`
 */
static JsValue range_to_string(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *an, *bn;
    int ao, bo;
    char buf[4096];
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!thisv.obj)
        return js_string(js, "");
    range_get_end(js, thisv, "startContainer", "startOffset", &an, &ao);
    range_get_end(js, thisv, "endContainer", "endOffset", &bn, &bo);
    range_text(js, an, ao, bn, bo, buf, sizeof(buf));
    return js_string(js, buf);
}

/**
 * @brief 删除一段端点覆盖的文本 / Delete the text covered by two endpoints
 */
static void range_delete_text(HpeJs *js, HpeDomNode *an, int ao, HpeDomNode *bn, int bo)
{
    char buf[4096], left[4096], right[4096];
    size_t ln = 0, rn = 0;
    const char *s;
    int i, len;

    if (!an || !bn)
        return;
    range_text(js, an, ao, bn, bo, buf, sizeof(buf));
    if (an == bn && an->type == HPE_NODE_TEXT) {
        s = an->text ? an->text : "";
        len = (int)strlen(s);
        for (i = 0; i < ao && i < len && ln + 1 < sizeof(left); i++)
            left[ln++] = s[i];
        left[ln] = '\0';
        for (i = bo; i < len && rn + 1 < sizeof(right); i++)
            right[rn++] = s[i];
        right[rn] = '\0';
        snprintf(buf, sizeof(buf), "%s%s", left, right);
        hpe_dom_set_text(an, buf);
    }
}

/**
 * @brief `Range.extractContents()` 实现 / Implementation of `Range.extractContents()`
 */
static JsValue range_extract(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *an, *bn;
    int ao, bo;
    JsValue frag;
    char buf[4096];
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!thisv.obj)
        return js_undef();
    range_get_end(js, thisv, "startContainer", "startOffset", &an, &ao);
    range_get_end(js, thisv, "endContainer", "endOffset", &bn, &bo);
    range_text(js, an, ao, bn, bo, buf, sizeof(buf));
    /* The extracted content is returned as a lightweight fragment-like object:
     * a real DocumentFragment would be a C DOM node owned by JS, and this
     * bridge has no destruction hook to reclaim it. */
    frag = js_objval(js_object_new(js));
    js_set_prop(js, NULL, frag, "textContent", js_string(js, buf), 0);
    range_delete_text(js, an, ao, bn, bo);
    return frag;
}

/**
 * @brief `Range.deleteContents()` 实现 / Implementation of `Range.deleteContents()`
 */
static JsValue range_delete_contents(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *an, *bn;
    int ao, bo;
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (thisv.obj) {
        range_get_end(js, thisv, "startContainer", "startOffset", &an, &ao);
        range_get_end(js, thisv, "endContainer", "endOffset", &bn, &bo);
        range_delete_text(js, an, ao, bn, bo);
    }
    return js_undef();
}

/**
 * @brief `Range.cloneRange()` 实现 / Implementation of `Range.cloneRange()`
 */
static JsValue range_clone(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *an, *bn;
    int ao, bo;
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (!thisv.obj)
        return js_undef();
    range_get_end(js, thisv, "startContainer", "startOffset", &an, &ao);
    range_get_end(js, thisv, "endContainer", "endOffset", &bn, &bo);
    return make_range(js, an, ao, bn, bo);
}

/**
 * @brief 给一个 Range 对象挂上方法 / Attach methods to a Range object
 */
static void range_install_methods(HpeJs *js, JsValue obj)
{
    js_set_prop(js, NULL, obj, "setStart",
                js_objval(js_native_new(js, range_set_start, NULL)), 0);
    js_set_prop(js, NULL, obj, "setEnd",
                js_objval(js_native_new(js, range_set_end_native, NULL)), 0);
    js_set_prop(js, NULL, obj, "collapse",
                js_objval(js_native_new(js, range_collapse, NULL)), 0);
    js_set_prop(js, NULL, obj, "selectNodeContents",
                js_objval(js_native_new(js, range_select_contents, NULL)), 0);
    js_set_prop(js, NULL, obj, "toString",
                js_objval(js_native_new(js, range_to_string, NULL)), 0);
    js_set_prop(js, NULL, obj, "extractContents",
                js_objval(js_native_new(js, range_extract, NULL)), 0);
    js_set_prop(js, NULL, obj, "deleteContents",
                js_objval(js_native_new(js, range_delete_contents, NULL)), 0);
    js_set_prop(js, NULL, obj, "cloneRange",
                js_objval(js_native_new(js, range_clone, NULL)), 0);
}

/**
 * @brief `document.createRange()` 实现 / Implementation of `document.createRange()`
 */
static JsValue sel_create_range(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    JsValue obj;
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    obj = make_range(js, js->dom_document, 0, js->dom_document, 0);
    range_install_methods(js, obj);
    return obj;
}

/* ---------------- Selection object ---------------- */

/**
 * @brief 读取引擎当前的页面选区 / Read the engine's current page selection
 */
static void selection_get(HpeJs *js, HpeDomNode **an, int *ao, HpeDomNode **bn, int *bo)
{
    *an = NULL;
    *ao = 0;
    *bn = NULL;
    *bo = 0;
    if (g_sel_api.get)
        g_sel_api.get(g_sel_api.ud, an, ao, bn, bo);
    else
        (void)js;
}

/**
 * @brief Selection 宿主的属性读取虚函数 / Property-get virtual for the Selection host
 */
static int selection_vt_get(HpeJs *js, void *target, const char *prop, JsValue *out)
{
    HpeDomNode *an, *bn;
    int ao, bo;
    HPE_UNUSED(target);
    selection_get(js, &an, &ao, &bn, &bo);
    if (!strcmp(prop, "anchorNode"))
        *out = hpe_js_dom_node_value(js, an);
    else if (!strcmp(prop, "anchorOffset"))
        *out = js_num(ao);
    else if (!strcmp(prop, "focusNode"))
        *out = hpe_js_dom_node_value(js, bn);
    else if (!strcmp(prop, "focusOffset"))
        *out = js_num(bo);
    else if (!strcmp(prop, "isCollapsed"))
        *out = js_bool(an == bn && ao == bo);
    else if (!strcmp(prop, "rangeCount"))
        *out = js_num((an && bn && !(an == bn && ao == bo)) ? 1 : 0);
    else
        return 0;
    return 1;
}

static const HpeJsHostVt g_selection_vt = {selection_vt_get, NULL};

/**
 * @brief `Selection.addRange()` 实现 / Implementation of `Selection.addRange()`
 */
static JsValue selection_add_range(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *an, *bn;
    int ao, bo;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (argc < 1 || !args[0].obj || !g_sel_api.set)
        return js_undef();
    range_get_end(js, args[0], "startContainer", "startOffset", &an, &ao);
    range_get_end(js, args[0], "endContainer", "endOffset", &bn, &bo);
    g_sel_api.set(g_sel_api.ud, an, ao, bn, bo);
    return js_undef();
}

/**
 * @brief `Selection.removeAllRanges()` 实现 / Implementation of `Selection.removeAllRanges()`
 */
static JsValue selection_clear(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    if (g_sel_api.clear)
        g_sel_api.clear(g_sel_api.ud);
    return js_undef();
}

/**
 * @brief `Selection.collapse()` / `Selection.extend()` 实现 / Shared collapse and extend implementation
 */
static JsValue selection_collapse_to(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *an, *bn, *nn;
    int ao, bo, no, extend = (int)(intptr_t)ud;
    HPE_UNUSED(thisv);
    if (argc < 1 || !g_sel_api.set)
        return js_undef();
    nn = sel_arg_node(args[0]);
    if (!nn)
        return js_undef();
    no = argc > 1 ? (int)js_to_number(js, args[1]) : 0;
    if (no < 0)
        no = 0;
    selection_get(js, &an, &ao, &bn, &bo);
    if (!extend || !an) {
        an = nn;
        ao = no;
    }
    g_sel_api.set(g_sel_api.ud, an, ao, nn, no);
    return js_undef();
}

/**
 * @brief `Selection.getRangeAt()` 实现 / Implementation of `Selection.getRangeAt()`
 */
static JsValue selection_get_range(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *an, *bn;
    int ao, bo;
    JsValue obj;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    selection_get(js, &an, &ao, &bn, &bo);
    if (argc < 1 || (int)js_to_number(js, args[0]) != 0 || !an || !bn)
        return js_undef();
    obj = make_range(js, an, ao, bn, bo);
    range_install_methods(js, obj);
    return obj;
}

/**
 * @brief `Selection.toString()` 实现 / Implementation of `Selection.toString()`
 */
static JsValue selection_to_string(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *an, *bn;
    int ao, bo;
    char buf[4096];
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    selection_get(js, &an, &ao, &bn, &bo);
    range_text(js, an, ao, bn, bo, buf, sizeof(buf));
    return js_string(js, buf);
}

/**
 * @brief `Selection.deleteFromDocument()` 实现 / Implementation of `Selection.deleteFromDocument()`
 */
static JsValue selection_delete(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *an, *bn;
    int ao, bo;
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    selection_get(js, &an, &ao, &bn, &bo);
    range_delete_text(js, an, ao, bn, bo);
    if (g_sel_api.clear)
        g_sel_api.clear(g_sel_api.ud);
    return js_undef();
}

/**
 * @brief `Selection.selectAllChildren()` 实现 / Implementation of `Selection.selectAllChildren()`
 */
static JsValue selection_all_children(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HpeDomNode *n, *last;
    int len = 0;
    HPE_UNUSED(thisv);
    HPE_UNUSED(ud);
    if (argc < 1 || !g_sel_api.set)
        return js_undef();
    n = sel_arg_node(args[0]);
    if (!n)
        return js_undef();
    last = n;
    if (n->first_child)
        last = n->last_child;
    if (last && last->type == HPE_NODE_TEXT)
        len = last->text ? (int)strlen(last->text) : 0;
    g_sel_api.set(g_sel_api.ud, n, 0, last, len);
    return js_undef();
}

/**
 * @brief 创建 `window.getSelection()` 返回的宿主对象 / Create the object returned by getSelection()
 */
static JsValue sel_make_selection(HpeJs *js)
{
    JsValue obj = js_make_host(js, js, HPE_HOST_SELECTION, &g_selection_vt);

    js_obj_set(js, obj.obj, "addRange",
               js_objval(js_native_new(js, selection_add_range, NULL)));
    js_obj_set(js, obj.obj, "removeAllRanges",
               js_objval(js_native_new(js, selection_clear, NULL)));
    js_obj_set(js, obj.obj, "collapse",
               js_objval(js_native_new(js, selection_collapse_to, NULL)));
    js_obj_set(js, obj.obj, "extend",
               js_objval(js_native_new(js, selection_collapse_to, (void *)(intptr_t)1)));
    js_obj_set(js, obj.obj, "getRangeAt",
               js_objval(js_native_new(js, selection_get_range, NULL)));
    js_obj_set(js, obj.obj, "toString",
               js_objval(js_native_new(js, selection_to_string, NULL)));
    js_obj_set(js, obj.obj, "deleteFromDocument",
               js_objval(js_native_new(js, selection_delete, NULL)));
    js_obj_set(js, obj.obj, "selectAllChildren",
               js_objval(js_native_new(js, selection_all_children, NULL)));
    return obj;
}

/**
 * @brief `window.getSelection()` 实现 / Implementation of `window.getSelection()`
 */
static JsValue sel_get_selection(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return sel_make_selection(js);
}

void hpe_js_sel_install(HpeJs *js)
{
    JsValue global = js_objval(js->global_obj);
    JsValue doc = js_get_prop(js, NULL, global, "document", 0);

    if (doc.t == JS_OBJ && doc.obj)
        js_obj_set(js, doc.obj, "createRange",
                   js_objval(js_native_new(js, sel_create_range, NULL)));
    js_set_prop(js, NULL, global, "getSelection",
                js_objval(js_native_new(js, sel_get_selection, NULL)), 0);
}
