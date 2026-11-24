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

/**
 * @brief 初始化一个小型字符串缓冲 / Initialise a small string builder
 *
 * @param[in,out] sb 待初始化的字符串缓冲 / String builder to initialise
 *
 * @note 初始容量 256 字节，调用方随后须用 `sb_free` 释放 / Starts at 256 bytes; release it later with `sb_free`
 *
 * @see sb_add, sb_free
 */
static void sb_init(SB *sb)
{
    sb->cap = 256;
    sb->len = 0;
    sb->s = (char *)malloc(sb->cap);
    sb->s[0] = '\0';
}

/**
 * @brief 向字符串缓冲追加 n 字节 / Append n bytes to the string builder
 *
 * @param[out]    sb 字符串缓冲 / String builder
 * @param[in]     s 源字节 / Source bytes
 * @param[in]     n 追加的字节数 / Number of bytes to append
 *
 * @note 容量不足时反复翻倍并 realloc / Capacity is doubled (via realloc) until it fits
 */
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

/**
 * @brief 释放字符串缓冲 / Free the string builder
 *
 * @param[in,out] sb 待释放的字符串缓冲 / String builder to release
 *
 * @note 释放后把内部指针置 `NULL` / The internal pointer is set to `NULL`
 */
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

/**
 * @brief 把 DOM 节点包装成宿主值 / Wrap a DOM node in a host value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node 要包装的节点 / Node to wrap
 *
 * @return 包装该节点的 JsValue；`node` 为 `NULL` 时返回 `null` / A JsValue wrapping the node; `null` when `node` is `NULL`
 *
 * @note DocumentFragment 用 fragment 虚表，其余用 element 虚表 / A fragment uses the fragment vtable, everything else the element vtable
 *
 * @see arg_node
 */
static JsValue node_value(HpeJs *js, HpeDomNode *node)
{
    if (!node)
        return js_null();
    if (node->type == HPE_NODE_FRAGMENT)
        return js_make_host(js, node, HPE_HOST_FRAGMENT, &g_fragment_vt);
    return js_make_host(js, node, HPE_HOST_ELEMENT, &g_element_vt);
}

/**
 * @brief 从实参解包出 DOM 节点 / Unwrap a DOM node from an argument
 *
 * @param[in]     v 待解包的 JS 值 / JS value to unwrap
 *
 * @return 节点指针（element 或 fragment 宿主），否则 `NULL` / The node pointer (element or fragment host), or `NULL`
 *
 * @see node_value
 */
static HpeDomNode *arg_node(JsValue v)
{
    HpeDomNode *n = (HpeDomNode *)js_host_ptr(v, HPE_HOST_ELEMENT);
    if (!n)
        n = (HpeDomNode *)js_host_ptr(v, HPE_HOST_FRAGMENT);
    return n;
}

/**
 * @brief node_value 的公开包装，供 MutationObserver 复用 / Public wrapper over node_value, reused by the MutationObserver bridge
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node 要包装的节点 / Node to wrap
 *
 * @return 包装该节点的 JsValue；`node` 为 `NULL` 时返回 `null` / A JsValue wrapping the node; `null` when `node` is `NULL`
 *
 * @see node_value
 */
JsValue hpe_js_dom_node_value(HpeJs *js, HpeDomNode *node)
{
    return node_value(js, node);
}

/**
 * @brief 取第 idx 个实参并转成字符串 / Convert the idx-th argument to a string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in]     idx 实参下标 / Argument index
 *
 * @return 新分配的字符串；下标越界时返回 `NULL` / Newly allocated string, or `NULL` when the index is out of range
 */
static char *arg_string(HpeJs *js, JsValue *args, int argc, int idx)
{
    if (idx >= argc)
        return NULL;
    return js_to_string(js, args[idx]);
}

/* defined in the events section below; declared here so the method dispatcher can use it */
static void listener_append(HpeJs *js, HpeDomNode *node, const char *type, JsValue handler,
                            int via_on);

/**
 * @brief 把 camelCase 的 JS 属性名转成连字符小写的 CSS 名 / Convert a camelCase JS property name to a dashed lowercase CSS name
 *
 * @param[in]     jsname JS 侧属性名 / Property name on the JS side
 * @param[out]    out 目标缓冲区 / Destination buffer
 * @param[in]     out_size 接收缓冲区容量（字节）/ Capacity of the output buffer in bytes
 *
 * @note 每个大写字母展开为 `-` 加小写；输出始终以 NUL 结尾 / Each uppercase letter becomes `-` plus its lowercase; the output is NUL-terminated
 */
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

/**
 * @brief 递归收集子树中的文本 / Recursively collect the text of a subtree
 *
 * @param[in,out] node 子树根节点 / Subtree root node
 * @param[out]    sb 字符串缓冲 / String builder
 *
 * @note 只拼接文本节点，仅对元素节点递归 / Only text nodes are appended, and only elements are recursed
 */

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

/**
 * @brief 把节点序列化成 HTML 文本 / Serialise a node as HTML text
 *
 * @param[in,out] node DOM 节点 / DOM node
 * @param[out]    sb 字符串缓冲 / String builder
 *
 * @note 文本节点原样输出；元素输出开始/结束标签与全部属性（属性值不转义）/ Text nodes are emitted verbatim; elements emit their open/close tags and every attribute (values are not escaped)
 */
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

/**
 * @brief 取元素的 textContent / Read an element's `textContent`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 *
 * @return 子树文本拼接成的 JS 字符串 / A JS string holding the concatenated subtree text
 *
 * @see collect_text
 */
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

/**
 * @brief 取元素子树的 innerHTML / Read an element's `innerHTML`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 *
 * @return 子节点序列化后的 JS 字符串 / A JS string holding the serialised child nodes
 *
 * @note 不含元素自身的标签 / The element's own tag is not included
 *
 * @see serialize_node
 */
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

/**
 * @brief 删除全部子节点并追加一个文本节点 / Replace every child with a single text node
 *
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     text 新文本；`NULL` 按空串处理 / New text; `NULL` is treated as empty
 *
 * @note 逐个 detach 子节点，使 MutationObserver 与引擎脏标记能看到删除 / Children are detached one by one so the mutation sink sees the removals
 */
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

/**
 * @brief 解析 HTML 片段并替换元素的子节点 / Parse an HTML fragment and replace the element's children
 *
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     html HTML 文本；`NULL` 按空串处理 / HTML text; `NULL` is treated as empty
 *
 * @note 先清空原有子节点，再把解析结果体节点的子节点搬入 / Existing children are cleared first, then the parsed body's children are moved in
 *
 * @see node_replace_children_with_text
 */
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

/**
 * @brief 按文档顺序收集子树中的元素 / Collect the elements of a subtree in document order
 *
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] out 可增长的元素数组 / Growable element array
 * @param[in,out] cap 数组当前容量 / Current array capacity
 * @param[in]     count 已收集的元素个数 / Number of elements already collected
 *
 * @return 收集到的元素总数 / Total number of elements collected
 *
 * @note 不含 `node` 自身；数组按需倍增 / `node` itself is excluded; the array doubles on demand
 */
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

/**
 * @brief querySelector/querySelectorAll 的共用实现 / Shared implementation of querySelector and querySelectorAll
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in]     all 非 0 时返回全部匹配 / Non-zero to return every match
 *
 * @return `all` 非 0 时返回匹配元素数组，否则返回首个匹配元素或 `null` / An array of matches when `all` is set, else the first match or `null`
 *
 * @note 选择器为空时：`all` 返回空数组，否则返回 `null` / On an empty selector: an empty array when `all`, otherwise `null`
 *
 * @see dm_query_selector, dm_query_selector_all
 */
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

/**
 * @brief 广度优先查找指定 id 的首个元素 / Breadth-first search for the first element with the given id
 *
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     id 标识符 / Identifier
 *
 * @return 匹配的元素，未找到时返回 `NULL` / The matching element, or `NULL` when none is found
 */
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

/**
 * @brief `getElementById` 的实现 / Implementation of `getElementById`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 找到的元素宿主值，否则 `null` / The element host value, else `null`
 *
 * @see dom_find_by_id
 */
static JsValue dm_get_element_by_id(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    char *id = arg_string(js, args, argc, 0);
    HpeDomNode *found;
    if (!id)
        return js_null();
    found = dom_find_by_id(node, id);
    return found ? node_value(js, found) : js_null();
}

/**
 * @brief `createElement` 的实现 / Implementation of `createElement`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 新元素的宿主值；标签为空时返回 `null` / Host value of the new element, or `null` for an empty tag
 */
static JsValue dm_create_element(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    char *tag = arg_string(js, args, argc, 0);
    HPE_UNUSED(node);
    if (!tag || !*tag)
        return js_null();
    return node_value(js, hpe_dom_element(tag));
}

/**
 * @brief `createElementNS` 的实现 / Implementation of `createElementNS`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 新元素的宿主值；标签为空时返回 `null` / Host value of the new element, or `null` for an empty tag
 *
 * @note 命名空间被忽略，只取标签名（SVG 子元素按标签绘制）/ The namespace is ignored; only the tag name matters (SVG children are drawn by tag name)
 */
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

/**
 * @brief `createDocumentFragment` 的实现 / Implementation of `createDocumentFragment`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 新文档片段的宿主值 / Host value of the new document fragment
 */
static JsValue dm_create_fragment(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HPE_UNUSED(node);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    return node_value(js, hpe_dom_fragment());
}

/**
 * @brief `removeChild` 的实现 / Implementation of `removeChild`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 总是返回 `undefined` / Always returns `undefined`
 *
 * @note 仅当子节点确属该父节点时才 detach / The child is detached only when it really belongs to this parent
 */
static JsValue dm_remove_child(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HpeDomNode *child = argc > 0 ? arg_node(args[0]) : NULL;
    HPE_UNUSED(js);
    if (child && child->parent == node)
        hpe_dom_detach(child);
    return js_undef();
}

/**
 * @brief `cloneNode` 的实现 / Implementation of `cloneNode`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 克隆出的节点宿主值 / Host value of the cloned node
 */
static JsValue dm_clone_node(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    int deep = argc > 0 && js_truthy(args[0]);
    return node_value(js, hpe_dom_clone(node, deep));
}

/**
 * @brief `hasChildNodes` 的实现 / Implementation of `hasChildNodes`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 存在子节点返回 `true`，否则 `false` / `true` when a child exists, otherwise `false`
 */
static JsValue dm_has_child_nodes(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HPE_UNUSED(js);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    return js_bool(node->first_child != NULL);
}

/**
 * @brief `insertBefore` 的实现 / Implementation of `insertBefore`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 插入的子节点宿主值；子节点为空时返回 `null` / Host value of the inserted child, or `null` when the child is absent
 */
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

/**
 * @brief `replaceChild` 的实现 / Implementation of `replaceChild`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 被替换掉的旧节点宿主值；旧节点为空时返回 `null` / Host value of the replaced old node, or `null` when it is absent
 */
static JsValue dm_replace_child(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HpeDomNode *newc = argc > 0 ? arg_node(args[0]) : NULL;
    HpeDomNode *oldc = argc > 1 ? arg_node(args[1]) : NULL;
    HPE_UNUSED(js);
    if (!oldc)
        return js_null();
    return node_value(js, hpe_dom_replace_child(node, newc, oldc));
}

/**
 * @brief `getAttribute` 的实现 / Implementation of `getAttribute`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 属性值字符串，不存在时返回 `null` / The attribute value string, or `null` when absent
 */
static JsValue dm_get_attribute(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    char *name = arg_string(js, args, argc, 0);
    const char *v = name ? hpe_dom_attr(node, name) : NULL;
    return v ? js_string(js, v) : js_null();
}

/**
 * @brief `setAttribute` 的实现 / Implementation of `setAttribute`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 总是返回 `undefined` / Always returns `undefined`
 *
 * @note 属性值缺失时写入空串 / An absent value is stored as the empty string
 */
static JsValue dm_set_attribute(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    char *name = arg_string(js, args, argc, 0);
    char *value = arg_string(js, args, argc, 1);
    HPE_UNUSED(js);
    if (name)
        hpe_dom_set_attr(node, name, value ? value : "");
    return js_undef();
}

/**
 * @brief `removeAttribute` 的实现 / Implementation of `removeAttribute`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 总是返回 `undefined` / Always returns `undefined`
 */
static JsValue dm_remove_attribute(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    char *name = arg_string(js, args, argc, 0);
    HPE_UNUSED(js);
    if (name)
        hpe_dom_remove_attr(node, name);
    return js_undef();
}

/**
 * @brief `hasAttribute` 的实现 / Implementation of `hasAttribute`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 属性存在返回 `true`，否则 `false` / `true` when the attribute exists, otherwise `false`
 */
static JsValue dm_has_attribute(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    char *name = arg_string(js, args, argc, 0);
    return js_bool(name && hpe_dom_attr(node, name) != NULL);
}

/**
 * @brief `appendChild` 的实现 / Implementation of `appendChild`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 追加的子节点宿主值；子节点为空时返回 `null` / Host value of the appended child, or `null` when the child is absent
 */
static JsValue dm_append_child(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HpeDomNode *child = argc > 0 ? arg_node(args[0]) : NULL;
    if (child) {
        hpe_dom_append(node, child);
        return node_value(js, child);
    }
    return js_null();
}

/**
 * @brief `remove` 的实现：把自己从父节点上摘除 / Implementation of `remove`: detach this node from its parent
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 总是返回 `undefined` / Always returns `undefined`
 */
static JsValue dm_remove_self(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    HPE_UNUSED(js);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    hpe_dom_detach(node);
    return js_undef();
}

/**
 * @brief `querySelector` 的实现 / Implementation of `querySelector`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 首个匹配元素的宿主值，无匹配或空选择器时返回 `null` / Host value of the first match, or `null` when nothing matches or the selector is empty
 *
 * @see query_selector_impl
 */
static JsValue dm_query_selector(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    return query_selector_impl(js, node, args, argc, 0);
}

/**
 * @brief `querySelectorAll` 的实现 / Implementation of `querySelectorAll`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 匹配元素组成的数组 / An array of the matching elements
 *
 * @see query_selector_impl
 */
static JsValue dm_query_selector_all(HpeJs *js, HpeDomNode *node, JsValue *args, int argc)
{
    return query_selector_impl(js, node, args, argc, 1);
}

/**
 * @brief `addEventListener` 的实现 / Implementation of `addEventListener`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 *
 * @return 总是返回 `undefined` / Always returns `undefined`
 *
 * @note 事件类型为空时不注册 / Nothing is registered for an empty event type
 *
 * @see listener_append
 */
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

/**
 * @brief DOM 方法绑定对象的统一入口 / Common entry point for bound DOM method objects
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this（未使用）/ The `this` value (unused)
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 指向 DomMth 的绑定信息 / Binding info pointing at the `DomMth`
 *
 * @return 查表命中的方法返回值；未命中时返回 `undefined` / The matched method's result, or `undefined` when the name is unknown
 *
 * @note 纯查表分派，新增方法只需在表中加一行 / Dispatch is a pure table lookup, so adding a method is a one-line table edit
 *
 * @see bind_dom_method, method_is
 */
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

/**
 * @brief 把某个 DOM 方法绑定成原生函数对象 / Bind one DOM method as a native function object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     method 方法名 / Method name
 *
 * @return 绑定到该节点的方法函数值 / A function value bound to this node
 *
 * @note 绑定信息从 js arena 分配 / The binding info is allocated from the JS arena
 *
 * @see dom_method_dispatch
 */
static JsValue bind_dom_method(HpeJs *js, HpeDomNode *node, const char *method)
{
    DomMth *m = (DomMth *)hpe_js_alloc(js, sizeof(DomMth));
    m->node = node;
    m->method = hpe_js_strdup(js, method);
    return js_objval(js_native_new(js, dom_method_dispatch, m));
}

/**
 * @brief 判断属性名是否是受支持的 DOM 方法 / Test whether a property name is a supported DOM method
 *
 * @param[in]     prop 属性名 / Property name
 *
 * @return 是方法名返回 1，否则 0 / 1 when it is a method name, otherwise 0
 *
 * @see dom_method_dispatch
 */
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

/**
 * @brief 把 `on*` 属性名映射成事件类型名 / Map an `on*` property name to its event type name
 *
 * @param[in]     prop 属性名 / Property name
 *
 * @return 对应的事件类型串（如 `click`）；否则 `NULL` / The matching event type (e.g. `click`), or `NULL`
 *
 * @note 当前支持 `onclick` / `onmousedown` / `onmouseup` / Currently recognises `onclick`, `onmousedown` and `onmouseup`
 */
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

/**
 * @brief 取节点上的监听器链表头 / Get the head of a node's listener list
 *
 * @param[in,out] node DOM 节点 / DOM node
 *
 * @return 监听器链表头，可能为 `NULL` / The listener list head, possibly `NULL`
 */
static JsListener *node_listeners(HpeDomNode *node)
{
    return (JsListener *)node->listeners;
}

/**
 * @brief 在节点上登记一个事件监听器 / Store an event listener on a node
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     type 事件类型 / Event type
 * @param[in]     handler 处理函数 / Handler function
 * @param[in]     via_on 经 `on*` 属性登记为 1 / 1 when added through an `on*` property
 *
 * @note `via_on` 会替换同类型的旧监听；handler 为 `null`/`undefined` 时只做删除 / A `via_on` handler replaces any previous one of the same type; a `null`/`undefined` handler only removes
 *
 * @see on_handler_get, dm_add_event_listener
 */
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

/**
 * @brief 取某类型经 `on*` 登记的监听器 / Read the `on*` handler registered for a type
 *
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     type 事件类型 / Event type
 *
 * @return 对应的处理函数；没有时返回 `null` / The handler function, or `null` when none is set
 */
static JsValue on_handler_get(HpeDomNode *node, const char *type)
{
    JsListener *l;
    for (l = node_listeners(node); l; l = l->next)
        if (l->via_on && !strcmp(l->type, type))
            return l->handler;
    return js_null();
}

/**
 * @brief 什么都不做的原生函数 / A native function that does nothing
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 总是返回 `undefined` / Always returns `undefined`
 *
 * @note 用作 `reload`/`scrollTo` 等空实现 / Used as the empty implementation for `reload`, `scrollTo` and friends
 */
static JsValue native_noop(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(js);
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_undef();
}

/**
 * @brief 在事件对象上记录停止传播标志 / Record a stop-propagation flag on the event object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 事件对象 / The event object
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 总是返回 `undefined` / Always returns `undefined`
 *
 * @note `stopPropagation` 与 `stopImmediatePropagation` 共用此实现 / Both `stopPropagation` and `stopImmediatePropagation` share this implementation
 *
 * @see propagation_stopped
 */
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

/**
 * @brief 检查事件对象是否已请求停止传播 / Test whether the event object asked to stop propagation
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     ev 事件对象 / Event object
 *
 * @return 已停止返回 1，否则 0 / 1 when stopped, otherwise 0
 *
 * @see native_stop_propagation
 */
static int propagation_stopped(HpeJs *js, JsValue ev)
{
    JsValue v;
    if (ev.t != JS_OBJ || !ev.obj)
        return 0;
    v = js_obj_get(js, ev.obj, "_stopped");
    return v.t == JS_BOOL && v.num != 0.0;
}

/**
 * @brief 构造一个事件对象 / Build an event object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     type 事件类型 / Event type
 * @param[in]     x 客户端 X 坐标 / Client X coordinate
 * @param[in]     y 客户端 Y 坐标 / Client Y coordinate
 * @param[in,out] target 事件目标节点 / Event target node
 *
 * @return 带有 type/clientX/clientY/target 及方法的事件对象 / An event object carrying type/clientX/clientY/target plus methods
 *
 * @note 同时挂上 `preventDefault`（空实现）与两个停止传播方法 / Also attaches `preventDefault` (a no-op) and the two stop-propagation methods
 */
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

/**
 * @brief 从目标节点向上冒泡派发事件 / Dispatch an event by bubbling from the target upwards
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] target 事件目标节点 / Event target node
 * @param[in]     type 事件类型 / Event type
 * @param[in]     clientX 客户端 X 坐标 / Client X coordinate
 * @param[in]     clientY 客户端 Y 坐标 / Client Y coordinate
 *
 * @return 至少调用过一个处理函数时返回 1，否则 0 / 1 when at least one handler ran, otherwise 0
 *
 * @note 每层更新 `currentTarget`；处理函数调用 `stopPropagation` 后立即停止 / `currentTarget` is updated per level; dispatch stops as soon as a handler requests it
 *
 * @see event_object, listener_append
 */
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


/**
 * @brief 取元素标签名并转成大写 / Read an element's tag name uppercased
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 *
 * @return 大写标签名的 JS 字符串 / A JS string holding the uppercased tag name
 *
 * @note 模拟 HTML 文档中浏览器报告大写元素名的行为 / Mimics how browsers uppercase element names in HTML documents
 */

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

/**
 * @brief 取属性值，缺失时返回空串 / Read an attribute value, falling back to the empty string
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     attr 属性名 / Attribute name
 *
 * @return 属性值的 JS 字符串；缺失时为空串 / A JS string with the attribute value, or the empty string when absent
 */
static JsValue element_attr_str(HpeJs *js, HpeDomNode *node, const char *attr)
{
    const char *v = hpe_dom_attr(node, attr);
    return js_string(js, v ? v : "");
}

/**
 * @brief 取元素的 clientWidth/clientHeight / Read an element's `clientWidth`/`clientHeight`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     prop `clientWidth` 或 `clientHeight` / Either `clientWidth` or `clientHeight`
 *
 * @return 布局后的内容区宽度或高度；无盒时返回 0 / The laid-out content width or height, or 0 without a box
 *
 * @note 浏览器在此会强制布局；本引擎用元素到盒的反向指针取已布局尺寸 / Browsers force layout here; the engine reads the laid-out size via the element-to-box back pointer
 */
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

/**
 * @brief `nodeType` 读取器，元素固定返回 1 / `nodeType` getter, always 1 for elements
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     prop CSS 属性名 / CSS property name
 *
 * @return 数值 1（ELEMENT_NODE）/ The number 1 (ELEMENT_NODE)
 */
static JsValue eg_node_type(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(node);
    HPE_UNUSED(prop);
    return js_num(1); /* ELEMENT_NODE */
}

/**
 * @brief `tagName`/`nodeName` 读取器 / `tagName`/`nodeName` getter
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     prop CSS 属性名 / CSS property name
 *
 * @return 大写标签名的 JS 字符串 / A JS string with the uppercased tag name
 *
 * @see element_upper_tag
 */
static JsValue eg_tag_name(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return element_upper_tag(js, node);
}

/**
 * @brief `id` 读取器 / `id` getter
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     prop CSS 属性名 / CSS property name
 *
 * @return `id` 属性值的 JS 字符串 / A JS string with the `id` attribute
 */
static JsValue eg_id(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return element_attr_str(js, node, "id");
}

/**
 * @brief `className` 读取器 / `className` getter
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     prop CSS 属性名 / CSS property name
 *
 * @return `class` 属性值的 JS 字符串 / A JS string with the `class` attribute
 */
static JsValue eg_class_name(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return element_attr_str(js, node, "class");
}

/**
 * @brief `textContent` 读取器 / `textContent` getter
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     prop CSS 属性名 / CSS property name
 *
 * @return 子树文本拼接成的 JS 字符串 / A JS string holding the concatenated subtree text
 */
static JsValue eg_text_content(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return element_get_text(js, node);
}

/**
 * @brief `innerHTML` 读取器 / `innerHTML` getter
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     prop CSS 属性名 / CSS property name
 *
 * @return 子节点序列化后的 JS 字符串 / A JS string holding the serialised children
 */
static JsValue eg_inner_html(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return element_get_html(js, node);
}

/**
 * @brief `style` 读取器，返回样式宿主对象 / `style` getter, returning a style host object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     prop CSS 属性名 / CSS property name
 *
 * @return 绑定到该节点的 style 宿主值 / A style host value bound to this node
 */
static JsValue eg_style(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return js_make_host(js, node, HPE_HOST_STYLE, &g_style_vt);
}

/**
 * @brief `firstChild` 读取器 / `firstChild` getter
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     prop CSS 属性名 / CSS property name
 *
 * @return 首个子节点的宿主值，没有时返回 `null` / Host value of the first child, or `null` when there is none
 */
static JsValue eg_first_child(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return node->first_child ? node_value(js, node->first_child) : js_null();
}

/**
 * @brief `nextSibling` 读取器 / `nextSibling` getter
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     prop CSS 属性名 / CSS property name
 *
 * @return 下一个兄弟节点的宿主值，没有时返回 `null` / Host value of the next sibling, or `null` when there is none
 */
static JsValue eg_next_sibling(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return node->next_sibling ? node_value(js, node->next_sibling) : js_null();
}

/**
 * @brief `parentNode` 读取器 / `parentNode` getter
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     prop CSS 属性名 / CSS property name
 *
 * @return 父节点的宿主值，没有时返回 `null` / Host value of the parent, or `null` when there is none
 */
static JsValue eg_parent_node(HpeJs *js, HpeDomNode *node, const char *prop)
{
    HPE_UNUSED(prop);
    return node->parent ? node_value(js, node->parent) : js_null();
}

/**
 * @brief `clientWidth`/`clientHeight` 读取器 / `clientWidth`/`clientHeight` getter
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     prop CSS 属性名 / CSS property name
 *
 * @return 布局后的内容区尺寸 / The laid-out content size
 *
 * @see element_client_size
 */
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

/**
 * @brief 元素宿主的属性读取虚函数 / Property-get virtual for element hosts
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     target 元素宿主逐个的 DOM 节点指针 / DOM node behind the element host
 * @param[in]     prop 属性名 / Property name
 * @param[out]    out 接收读取结果的值 / Receives the read value
 *
 * @return 已处理返回 1，未识别返回 0 / 1 when handled, 0 when the property is unknown
 *
 * @note 顺序为 `<template>.content`、属性表、方法绑定、`on*` 处理函数 / Order: `<template>.content`, the property table, method binding, then `on*` handlers
 *
 * @see element_vt_set, bind_dom_method
 */
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

/**
 * @brief 元素宿主的属性写入虚函数 / Property-set virtual for element hosts
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     target 元素宿主背后的 DOM 节点指针 / DOM node behind the element host
 * @param[in]     prop 属性名 / Property name
 * @param[in]     value 待写入的值 / Value to write
 *
 * @return 已处理返回 1，未识别返回 0 / 1 when handled, 0 when the property is unknown
 *
 * @note 处理 `textContent`/`innerHTML`/`style`/`id`/`className` 与 `on*` 属性 / Handles `textContent`, `innerHTML`, `style`, `id`, `className` and `on*` properties
 */
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

/**
 * @brief 文档片段宿主的属性读取虚函数 / Property-get virtual for document-fragment hosts
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     target 片段宿主背后的 DOM 节点指针 / DOM node behind the fragment host
 * @param[in]     prop 属性名 / Property name
 * @param[out]    out 接收读取结果的值 / Receives the read value
 *
 * @return 已处理返回 1，未识别返回 0 / 1 when handled, 0 when the property is unknown
 *
 * @note `nodeType` 为 11；`childNodes`/`children` 每次读取都构造一次快照 / `nodeType` is 11; `childNodes`/`children` build a snapshot on each read
 *
 * @see fragment_vt_set
 */
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

/**
 * @brief 文档片段宿主的属性写入虚函数 / Property-set virtual for document-fragment hosts
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     target 片段宿主背后的 DOM 节点指针 / DOM node behind the fragment host
 * @param[in]     prop 属性名 / Property name
 * @param[in]     value 待写入的值 / Value to write
 *
 * @return 已处理返回 1，未识别返回 0 / 1 when handled, 0 when the property is unknown
 *
 * @note 仅处理 `textContent` 与 `innerHTML` / Only `textContent` and `innerHTML` are handled
 */
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

/**
 * @brief style 宿主的属性读取虚函数 / Property-get virtual for style hosts
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     target 样式宿主背后的 DOM 节点指针 / DOM node behind the style host
 * @param[in]     prop 属性名 / Property name
 * @param[out]    out 接收读取结果的值 / Receives the read value
 *
 * @return 始终返回 1 / Always returns 1
 *
 * @note camelCase 属性名转成 CSS 名后查内联样式，缺失返回空串 / The camelCase name is converted to a CSS name and looked up in the inline style; absent values give the empty string
 *
 * @see style_vt_set, css_name_from_js
 */
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

/**
 * @brief style 宿主的属性写入虚函数 / Property-set virtual for style hosts
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     target 样式宿主背后的 DOM 节点指针 / DOM node behind the style host
 * @param[in]     prop 属性名 / Property name
 * @param[in]     value 待写入的值 / Value to write
 *
 * @return 节点为空返回 0，否则返回 1 / 0 when the node is absent, otherwise 1
 *
 * @note 写入内联样式表 / Writes into the inline style table
 */
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

/**
 * @brief document 宿主的属性读取虚函数 / Property-get virtual for the document host
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     target 文档根节点指针 / Document root node
 * @param[in]     prop 属性名 / Property name
 * @param[out]    out 接收读取结果的值 / Receives the read value
 *
 * @return 已处理返回 1，未识别返回 0 / 1 when handled, 0 when the property is unknown
 *
 * @note `body`/`head`/`documentElement` 按标签查找；随后是方法绑定与 `on*` / `body`/`head`/`documentElement` are looked up by tag, followed by method binding and `on*`
 *
 * @see document_vt_set
 */
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

/**
 * @brief document 宿主的属性写入虚函数 / Property-set virtual for the document host
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     target 文档根节点指针 / Document root node
 * @param[in]     prop 属性名 / Property name
 * @param[in]     value 待写入的值 / Value to write
 *
 * @return 已处理返回 1，未识别返回 0 / 1 when handled, 0 when the property is unknown
 *
 * @note `title` 写入 head 中的 title 元素；`on*` 登记监听 / `title` writes into the title element within head; `on*` registers a listener
 */
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

/**
 * @brief `location.toString` 实现，返回 `href` / Implementation of `location.toString`, returning `href`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return `href` 字段的字符串形式 / The `href` field as a string
 */
static JsValue loc_toString(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_string(js, js_to_string(js, js_obj_get(js, thisv.obj, "href")));
}

/**
 * @brief `getComputedStyle` 的空实现 / Empty implementation of `getComputedStyle`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 一个新的空对象 / A fresh empty object
 *
 * @note 引擎不计算样式，仅让守卫代码不报错 / The engine does not compute styles; this only keeps guarding scripts from failing
 */
static JsValue dom_style_stub(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_objval(js_object_new(js));
}

/**
 * @brief `requestAnimationFrame` 的空实现 / Empty implementation of `requestAnimationFrame`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     thisv 调用时的 this 值 / The `this` value of the call
 * @param[in,out] args 实参数组 / Actual argument array
 * @param[in]     argc 参数个数 / Argument count
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 句柄 0 / The handle 0
 *
 * @note 回调不会被调度 / The callback is never scheduled
 */
static JsValue dom_raf(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud)
{
    HPE_UNUSED(thisv);
    HPE_UNUSED(args);
    HPE_UNUSED(argc);
    HPE_UNUSED(ud);
    return js_num(0.0);
}

/**
 * @brief 往对象上设置一个属性 / Set one property on an object
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] o 目标对象 / Target object
 * @param[in]     k 属性键 / Property key
 * @param[in]     v 属性值 / Property value
 *
 * @note `js_obj_set` 的薄封装 / A thin wrapper over `js_obj_set`
 */
static void set_prop(HpeJs *js, JsObj *o, const char *k, JsValue v)
{
    js_obj_set(js, o, k, v);
}

/**
 * @brief 把原生函数指针包成 JS 函数值 / Wrap a native function pointer as a JS function value
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     fn 原生函数指针 / Native function pointer
 * @param[in,out] ud 宿主回调透传的用户数据 / Opaque user data passed through to the host
 *
 * @return 包好的函数值 / The wrapped function value
 */
static JsValue nat(HpeJs *js, JsNativeFn fn, void *ud)
{
    return js_objval(js_native_new(js, fn, ud));
}

/**
 * @brief 注册 location/navigator/window 等浏览器全局对象 / Register browser globals such as `location`, `navigator` and `window`
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] document 文档根节点 / Document root node
 *
 * @note 引擎无网络也无导航，所有网络类方法都是空实现；只让检测这些对象的页面脚本继续运行 / The engine has no network or navigation, so network-ish methods are no-ops; this only keeps scripts that probe them running
 *
 * @see hpe_js_dom_install, set_prop, nat
 */
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

/**
 * @brief 安装 DOM 绑定、浏览器全局、定时器与 MutationObserver / Install the DOM bridge, browser globals, timers and the MutationObserver
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] document 文档根节点 / Document root node
 *
 * @note 把 `document` 注册为全局宿主，随后依次安装 window 内建、定时器与变更通知 / `document` is registered as a global host, then window builtins, timers and change notification are installed
 *
 * @see install_window_builtins
 */
void hpe_js_dom_install(HpeJs *js, HpeDomNode *document)
{
    hpe_js_set_global_host(js, "document", document, HPE_HOST_DOCUMENT, &g_document_vt);
    install_window_builtins(js, document);
    hpe_js_timers_install(js);
    /* DOM change notification: the sink forwards mutations to any observer */
    hpe_js_mutation_install(js);
    hpe_js_mutation_install_ctor(js);
}
