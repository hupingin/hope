#include "dom/hpe_dom.h"

#include <string.h>

#include "base/hpe_util.h"

/* ---------------- mutation sink ----------------
 * A small ordered list of sinks; two are installed in practice -- the engine's
 * dirty-layout tracker and the JS MutationObserver bridge. The DOM layer itself
 * stays free of any js/ or layout/ dependency. */
#define HPE_MAX_MUT_SINKS 4
typedef struct {
    HpeDomMutationFn fn;
    void *ud;
} HpeMutSink;

static HpeMutSink g_mut_sinks[HPE_MAX_MUT_SINKS];
static int g_mut_sink_count;

/**
 * @brief 追加一个 DOM 变更订阅者 / Append a DOM mutation sink
 *
 * @details 订阅者按安装顺序保存在固定大小的表内（最多 `HPE_MAX_MUT_SINKS` 个）。
 *          Sinks are kept in an ordered fixed-size table of at most `HPE_MAX_MUT_SINKS` entries.
 *
 * @param[in]     fn 变更回调（`NULL` 时直接返回）/ Mutation callback, a `NULL` value returns immediately
 * @param[in,out] ud 透传给 `fn` 的用户数据，与 `fn` 一起构成订阅标识 / User data forwarded to `fn`, paired with `fn` to identify the sink
 *
 * @note 同一 `(fn, ud)` 组合已存在时不再重复安装；表满时静默忽略 / A duplicate `(fn, ud)` pair is ignored, and a full table is silently left unchanged
 *
 * @see hpe_dom_remove_mutation_sink hpe_dom_clear_mutation_sinks
 */
void hpe_dom_add_mutation_sink(HpeDomMutationFn fn, void *ud)
{
    int i;
    if (!fn)
        return;
    for (i = 0; i < g_mut_sink_count; i++)
        if (g_mut_sinks[i].fn == fn && g_mut_sinks[i].ud == ud)
            return; /* already installed */
    if (g_mut_sink_count >= HPE_MAX_MUT_SINKS)
        return;
    g_mut_sinks[g_mut_sink_count].fn = fn;
    g_mut_sinks[g_mut_sink_count].ud = ud;
    g_mut_sink_count++;
}

/**
 * @brief 移除一个已安装的 DOM 变更订阅者 / Remove an installed DOM mutation sink
 *
 * @param[in]     fn 要移除的变更回调 / Mutation callback to remove
 * @param[in,out] ud 与之配对的用户数据 / The user data paired with it
 *
 * @note 按 `(fn, ud)` 精确匹配，未找到时不做任何事；后续订阅者前移保持顺序 / Matches on `(fn, ud)` exactly; when absent nothing happens and later sinks shift up to preserve order
 *
 * @see hpe_dom_add_mutation_sink
 */
void hpe_dom_remove_mutation_sink(HpeDomMutationFn fn, void *ud)
{
    int i;
    for (i = 0; i < g_mut_sink_count; i++) {
        if (g_mut_sinks[i].fn == fn && g_mut_sinks[i].ud == ud) {
            int j;
            for (j = i; j + 1 < g_mut_sink_count; j++)
                g_mut_sinks[j] = g_mut_sinks[j + 1];
            g_mut_sink_count--;
            return;
        }
    }
}

/**
 * @brief 移除全部变更订阅者 / Remove every mutation sink
 *
 * @details 文档销毁时调用，使 `notify_sinks` 之后不再回调任何人。
 *          Called when the document is torn down so that `notify_sinks` no longer reaches anyone.
 */
void hpe_dom_clear_mutation_sinks(void)
{
    g_mut_sink_count = 0;
}

/**
 * @brief 旧版单一订阅者入口 / Legacy single-sink entry point
 *
 * @param[in]     fn 变更回调；传 `NULL` 表示清空全部订阅者 / Mutation callback; `NULL` clears all sinks
 * @param[in,out] ud 透传给 `fn` 的用户数据 / User data forwarded to `fn`
 *
 * @note 非 `NULL` 时等价于 `hpe_dom_add_mutation_sink`，并不替换已有订阅者 / With a non-`NULL` value it is equivalent to `hpe_dom_add_mutation_sink` and does not replace existing sinks
 *
 * @see hpe_dom_add_mutation_sink hpe_dom_clear_mutation_sinks
 */
void hpe_dom_set_mutation_sink(HpeDomMutationFn fn, void *ud)
{
    if (!fn) {
        hpe_dom_clear_mutation_sinks();
        return;
    }
    hpe_dom_add_mutation_sink(fn, ud);
}

/**
 * @brief 把一次变更广播给所有订阅者 / Broadcast one change to every sink
 *
 * @details `added` / `removed` 描述子节点列表增删：新增节点与移除节点，属性与文本变更时均为 `NULL`。
 *          `added` and `removed` describe child-list insertions and removals; both are `NULL` for attribute and text changes.
 *
 * @param[in]     kind 变更种类（`HPE_MUT_CHILD_LIST` / `HPE_MUT_ATTR` / `HPE_MUT_TEXT`）/ Change kind
 * @param[in,out] target 变更作用的节点（子节点列表变更时为其父节点）/ Node the change applies to, the parent for child-list changes
 * @param[in]     name 属性变更的属性名，其余种类为 `NULL` / Attribute name for attribute changes, `NULL` otherwise
 * @param[in]     old_value 变更前的旧值（属性值或文本），无则为 `NULL` / Old attribute or text value, `NULL` when none
 * @param[in,out] added 新增的节点 / Added node
 * @param[in,out] removed 被移除的节点 / Removed node
 *
 * @note 循环开始时先取出 `fn` 与 `ud` 的副本，因此订阅者在回调中再次增删订阅者不会破坏遍历 / The pair is copied up front, so a sink may add or remove sinks re-entrantly without breaking the walk
 */
static void notify_sinks(int kind, HpeDomNode *target, const char *name,
                         const char *old_value, HpeDomNode *added, HpeDomNode *removed)
{
    int i;
    if (!g_mut_sink_count)
        return;
    /* copy the count/ud up front: a sink may add a listener that mutates again */
    for (i = 0; i < g_mut_sink_count; i++) {
        HpeDomMutationFn fn = g_mut_sinks[i].fn;
        void *ud = g_mut_sinks[i].ud;
        if (fn)
            fn(ud, kind, target, name, old_value, added, removed);
    }
}

/**
 * @brief 报告一次子节点列表变更 / Report a child-list change
 *
 * @param[in,out] parent 父节点 / Parent node
 * @param[in,out] added 被插入的节点，无则为 `NULL` / Inserted node, `NULL` when none
 * @param[in,out] removed 被移除的节点，无则为 `NULL` / Removed node, `NULL` when none
 *
 * @note 以 `HPE_MUT_CHILD_LIST` 转调 `notify_sinks` / Forwards to `notify_sinks` with `HPE_MUT_CHILD_LIST`
 *
 * @see notify_sinks
 */
static void notify_child_list(HpeDomNode *parent, HpeDomNode *added, HpeDomNode *removed)
{
    notify_sinks(HPE_MUT_CHILD_LIST, parent, NULL, NULL, added, removed);
}

/**
 * @brief 报告一次属性变更 / Report an attribute change
 *
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     name 名称 / Name
 * @param[in]     old_value 属性原值，新增属性时为 `NULL` / Previous attribute value, `NULL` when the attribute was newly added
 *
 * @note 以 `HPE_MUT_ATTR` 转调 `notify_sinks` / Forwards to `notify_sinks` with `HPE_MUT_ATTR`
 *
 * @see notify_sinks
 */
static void notify_attr(HpeDomNode *node, const char *name, const char *old_value)
{
    notify_sinks(HPE_MUT_ATTR, node, name, old_value, NULL, NULL);
}

/**
 * @brief 报告一次文本节点内容变更 / Report a text-node content change
 *
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     old_value 文本原值 / Previous text value
 *
 * @note 以 `HPE_MUT_TEXT` 转调 `notify_sinks` / Forwards to `notify_sinks` with `HPE_MUT_TEXT`
 *
 * @see notify_sinks
 */
static void notify_text(HpeDomNode *node, const char *old_value)
{
    notify_sinks(HPE_MUT_TEXT, node, NULL, old_value, NULL, NULL);
}

/**
 * @brief 分配并初始化一个指定类型的 DOM 节点 / Allocate and initialise a DOM node of the given type
 *
 * @param[in]     type 节点类型 / Node type
 *
 * @return 清零后的新节点 / A zero-initialised new node
 *
 * @note 内部构造函数，分配失败即 abort，绝不返回 `NULL` / Internal constructor; aborts on allocation failure and never returns `NULL`
 */
static HpeDomNode *dom_new(HpeNodeType type)
{
    HpeDomNode *node = (HpeDomNode *)hpe_calloc(1, sizeof(HpeDomNode));
    node->type = type;
    return node;
}

/**
 * @brief 创建一个元素节点 / Create an element node
 *
 * @param[in]     tag 标签名（`NULL` 视为 `div`）/ Tag name, `NULL` falls back to `div`
 *
 * @return 新的元素节点 / The new element node
 */
HpeDomNode *hpe_dom_element(const char *tag)
{
    HpeDomNode *node = dom_new(HPE_NODE_ELEMENT);
    node->tag = hpe_strdup(tag ? tag : "div");
    return node;
}

/**
 * @brief 创建一个文本节点 / Create a text node
 *
 * @param[in]     text 文本内容（`NULL` 视为空串）/ Text content, `NULL` falls back to the empty string
 *
 * @return 新的文本节点 / The new text node
 */
HpeDomNode *hpe_dom_text(const char *text)
{
    HpeDomNode *node = dom_new(HPE_NODE_TEXT);
    node->text = hpe_strdup(text ? text : "");
    return node;
}

/**
 * @brief 创建一个文档节点 / Create a document node
 *
 * @return 新的文档节点 / The new document node
 */
HpeDomNode *hpe_dom_document(void)
{
    return dom_new(HPE_NODE_DOCUMENT);
}

/**
 * @brief 创建一个文档片段节点 / Create a document-fragment node
 *
 * @return 新的片段节点 / The new fragment node
 *
 * @note 片段本身不属于树；把它追加到父节点时会搬移其子节点 / The fragment itself is not part of the tree; appending it moves its children instead
 */
HpeDomNode *hpe_dom_fragment(void)
{
    return dom_new(HPE_NODE_FRAGMENT);
}

/**
 * @brief 取得节点子内容的容器 / Obtain the container holding a node's children
 *
 * @details `<template>` 的子节点存放在分离的 DocumentFragment 中（规范里的 `content`），解析器、`innerHTML` 与 `textContent` 都经此函数读写。
 *          A `<template>`'s children live in a detached DocumentFragment (the spec's `content`); the parser, `innerHTML` and `textContent` all read and write through this function.
 *
 * @param[in,out] node DOM 节点 / DOM node
 *
 * @return `<template>` 返回其内容片段，其余节点返回自身 / The content fragment for `<template>`, otherwise the node itself
 *
 * @note 传入 `NULL` 返回 `NULL`；首次访问 `<template>` 时按需创建 content 片段 / A `NULL` input yields `NULL`; the content fragment is created lazily on first access
 *
 * @see hpe_dom_append
 */
HpeDomNode *hpe_dom_children_root(HpeDomNode *node)
{
    if (!node)
        return NULL;
    if (node->type == HPE_NODE_ELEMENT && node->tag && strcmp(node->tag, "template") == 0) {
        if (!node->content)
            node->content = hpe_dom_fragment();
        return node->content;
    }
    return node;
}

/**
 * @brief 把子节点追加到父节点末尾 / Append a child node to the end of a parent
 *
 * @param[in,out] parent 父节点 / Parent node
 * @param[in,out] child 待插入的子节点；若为文档片段则按序搬移其所有子节点并释放空片段 / Child to insert; a DocumentFragment has its children moved in order and is then freed
 *
 * @note 先解除子节点与原父节点的关联；插入成功后报告一次子节点列表变更 / The child is detached from any previous parent first, and a child-list mutation is reported on success
 *
 * @warning `parent` 与 `child` 任一为 `NULL` 时直接返回 / Returns immediately when either `parent` or `child` is `NULL`
 *
 * @see hpe_dom_insert_before hpe_dom_detach
 */
void hpe_dom_append(HpeDomNode *parent, HpeDomNode *child)
{
    if (!parent || !child)
        return;
    /* appending a DocumentFragment moves each of its children in order, then
     * frees the (now empty) fragment -- the fragment itself never enters the
     * tree (DOM spec "insert" with a fragment node). */
    if (child->type == HPE_NODE_FRAGMENT) {
        HpeDomNode *c, *next;
        for (c = child->first_child; c; c = next) {
            next = c->next_sibling;
            hpe_dom_detach(c); /* unlinks from the fragment */
            hpe_dom_append(parent, c);
        }
        hpe_dom_free(child);
        return;
    }
    if (child->parent)
        hpe_dom_detach(child);
    child->parent = parent;
    child->next_sibling = NULL;
    if (parent->last_child) {
        parent->last_child->next_sibling = child;
        parent->last_child = child;
    } else {
        parent->first_child = parent->last_child = child;
    }
    notify_child_list(parent, child, NULL);
}

/**
 * @brief 把节点从其父节点上摘下 / Detach a node from its parent
 *
 * @param[in,out] node DOM 节点 / DOM node
 *
 * @note 只更新兄弟链与父指针，不释放节点；未挂载的节点直接返回 / Only the sibling links and parent pointer are updated, the node is not freed; an unattached node returns unchanged
 *
 * @see hpe_dom_append hpe_dom_free
 */
void hpe_dom_detach(HpeDomNode *node)
{
    HpeDomNode *prev, *cur;
    HpeDomNode *owner;
    if (!node || !node->parent)
        return;
    owner = node->parent;
    prev = NULL;
    cur = owner->first_child;
    while (cur) {
        if (cur == node) {
            if (prev)
                prev->next_sibling = cur->next_sibling;
            else
                owner->first_child = cur->next_sibling;
            if (owner->last_child == cur)
                owner->last_child = prev;
            break;
        }
        prev = cur;
        cur = cur->next_sibling;
    }
    node->parent = NULL;
    node->next_sibling = NULL;
    notify_child_list(owner, NULL, node);
}

/**
 * @brief 判断 `ref` 是否为 `parent` 的直接子节点 / Test whether `ref` is a direct child of `parent`
 *
 * @param[in]     parent 父节点，可为 `NULL` / Parent node, may be `NULL`
 * @param[in]     ref 待检查节点，可为 `NULL` / Node to check, may be `NULL`
 *
 * @return 是直接子节点返回 1 / 1 when it is a direct child
 *
 * @note 内部辅助函数，用于 `hpe_dom_insert_before` 与 `hpe_dom_replace_child` 的合法性检查 / Internal helper used for the validity checks in `hpe_dom_insert_before` and `hpe_dom_replace_child`
 */
static int is_child_of(const HpeDomNode *parent, const HpeDomNode *ref)
{
    const HpeDomNode *c;
    if (!parent || !ref)
        return 0;
    for (c = parent->first_child; c; c = c->next_sibling)
        if (c == ref)
            return 1;
    return 0;
}

/**
 * @brief 在参照节点之前插入子节点 / Insert a child before a reference node
 *
 * @param[in,out] parent 父节点 / Parent node
 * @param[in,out] child 待插入的子节点；若为文档片段则按序搬移其所有子节点并释放空片段 / Child to insert; a DocumentFragment has its children moved in order and is then freed
 * @param[in,out] ref 参照节点；为 `NULL` 或不是 `parent` 的子节点时改为追加 / Reference node; a `NULL` value or one that is not a child of `parent` falls back to appending
 *
 * @note 先解除 `child` 与原父节点的关联；`child == ref` 时不做任何事 / `child` is detached first; when `child == ref` nothing is done
 *
 * @warning `parent` 或 `child` 为 `NULL` 时直接返回 / Returns immediately when `parent` or `child` is `NULL`
 *
 * @see hpe_dom_append hpe_dom_replace_child
 */
void hpe_dom_insert_before(HpeDomNode *parent, HpeDomNode *child, HpeDomNode *ref)
{
    HpeDomNode *prev;
    if (!parent || !child)
        return;
    /* a fragment splices its children in order before `ref` */
    if (child->type == HPE_NODE_FRAGMENT) {
        HpeDomNode *c, *next;
        for (c = child->first_child; c; c = next) {
            next = c->next_sibling;
            hpe_dom_detach(c);
            hpe_dom_insert_before(parent, c, ref);
        }
        hpe_dom_free(child);
        return;
    }
    if (!ref || !is_child_of(parent, ref)) {
        hpe_dom_append(parent, child);
        return;
    }
    if (child == ref)
        return; /* nothing to do (DOM would move it before itself) */
    if (child->parent)
        hpe_dom_detach(child);
    child->parent = parent;
    child->next_sibling = ref;
    prev = hpe_dom_prev_sibling(ref);
    if (prev)
        prev->next_sibling = child;
    else
        parent->first_child = child;
}

/**
 * @brief 用一个节点替换另一个子节点 / Replace one child node with another
 *
 * @param[in,out] parent 父节点 / Parent node
 * @param[in,out] new_child 新节点；为 `NULL` 时仅移除旧节点 / Replacement node; `NULL` removes the old child only
 * @param[in,out] old_child 被替换的子节点，必须是 `parent` 的子节点 / Child being replaced, must be a child of `parent`
 *
 * @return 被摘下的旧节点（调用方接管）或 `NULL` / The detached old node owned by the caller, or `NULL`
 *
 * @note 返回值可能是 `old_child` 自身；`new_child == old_child` 时原样返回 / The returned node may be `old_child` itself; when `new_child == old_child` it is returned unchanged
 *
 * @see hpe_dom_insert_before hpe_dom_detach
 */
HpeDomNode *hpe_dom_replace_child(HpeDomNode *parent, HpeDomNode *new_child,
                                  HpeDomNode *old_child)
{
    if (!parent || !old_child || !is_child_of(parent, old_child))
        return NULL;
    if (new_child == old_child)
        return old_child;
    if (new_child)
        hpe_dom_insert_before(parent, new_child, old_child);
    hpe_dom_detach(old_child);
    return old_child;
}

/**
 * @brief 克隆一个节点（可深度克隆）/ Clone a node, optionally deeply
 *
 * @details 元素克隆会复制标签与全部属性；文本、文档、片段按其类型重建。
 *          An element clone keeps its tag and every attribute; text, document and fragment nodes are rebuilt according to their type.
 *
 * @param[in]     node 被克隆的节点，可为 `NULL` / Node to clone, may be `NULL`
 * @param[in]     deep 深度克隆为 1 / 1 for a deep clone
 *
 * @return 新的克隆节点，输入为 `NULL` 时返回 `NULL` / The new clone, or `NULL` when the input was `NULL`
 *
 * @note `deep` 非 0 时递归克隆整个子树；浅拷贝的副本没有父节点、兄弟或子节点 / A non-zero `deep` recursively clones the whole subtree; a shallow copy has no parent, siblings or children
 */
HpeDomNode *hpe_dom_clone(const HpeDomNode *node, int deep)
{
    HpeDomNode *copy;
    int i;
    if (!node)
        return NULL;
    switch (node->type) {
    case HPE_NODE_TEXT:
        copy = hpe_dom_text(node->text ? node->text : "");
        break;
    case HPE_NODE_DOCUMENT:
        copy = hpe_dom_document();
        break;
    case HPE_NODE_FRAGMENT:
        copy = hpe_dom_fragment();
        break;
    default:
        copy = hpe_dom_element(node->tag ? node->tag : "div");
        for (i = 0; i < node->attr_count; i++)
            hpe_dom_set_attr(copy, node->attrs[i].name, node->attrs[i].value);
        break;
    }
    if (deep) {
        const HpeDomNode *c;
        for (c = node->first_child; c; c = c->next_sibling)
            hpe_dom_append(copy, hpe_dom_clone(c, 1));
    }
    return copy;
}

/**
 * @brief 读取元素上的属性值 / Read an attribute value from an element
 *
 * @param[in]     node DOM 节点 / DOM node
 * @param[in]     name 属性名，大小写不敏感 / Attribute name, matched case-insensitively
 *
 * @return 属性值（内部存储，调用方不得释放）或 `NULL` / The attribute value as internal storage that must not be freed, or `NULL`
 *
 * @note 无该属性或参数为 `NULL` 时返回 `NULL` / Returns `NULL` when the attribute is absent or a parameter is `NULL`
 */
const char *hpe_dom_attr(const HpeDomNode *node, const char *name)
{
    int i;
    if (!node || !name)
        return NULL;
    for (i = 0; i < node->attr_count; i++) {
        if (hpe_str_ieq(node->attrs[i].name, name))
            return node->attrs[i].value;
    }
    return NULL;
}

/**
 * @brief 取得节点的前一个兄弟节点 / Obtain the preceding sibling of a node
 *
 * @param[in]     node DOM 节点 / DOM node
 *
 * @return 父节点下紧邻的前一个兄弟，无则 `NULL` / The immediately preceding sibling, or `NULL`
 *
 * @note 无父节点或是首个子节点时返回 `NULL` / Returns `NULL` when the node has no parent or is the first child
 */
HpeDomNode *hpe_dom_prev_sibling(const HpeDomNode *node)
{
    const HpeDomNode *p, *prev;
    if (!node || !node->parent)
        return NULL;
    prev = NULL;
    for (p = node->parent->first_child; p; p = p->next_sibling) {
        if (p == node)
            return (HpeDomNode *)prev;
        prev = p;
    }
    return NULL;
}

/**
 * @brief 设置元素上的属性 / Set an attribute on an element
 *
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     name 名称 / Name
 * @param[in]     value 属性值（`NULL` 视为空串）/ Attribute value, `NULL` falls back to the empty string
 *
 * @note 属性已存在时覆盖其值，否则新增；两种情况都会报告一次属性变更 / An existing attribute is overwritten, otherwise a new one is appended; both cases report an attribute mutation
 *
 * @warning `node` 或 `name` 为 `NULL` 时直接返回 / Returns immediately when `node` or `name` is `NULL`
 */
void hpe_dom_set_attr(HpeDomNode *node, const char *name, const char *value)
{
    int i;
    if (!node || !name)
        return;
    for (i = 0; i < node->attr_count; i++) {
        if (hpe_str_ieq(node->attrs[i].name, name)) {
            char *old = hpe_strdup(node->attrs[i].value);
            hpe_free(node->attrs[i].value);
            node->attrs[i].value = hpe_strdup(value ? value : "");
            notify_attr(node, name, old);
            hpe_free(old);
            return;
        }
    }
    node->attrs = (HpeAttr *)hpe_grow(node->attrs, &node->attr_cap,
                                      (size_t)node->attr_count + 1, sizeof(HpeAttr));
    node->attrs[node->attr_count].name = hpe_strdup(name);
    node->attrs[node->attr_count].value = hpe_strdup(value ? value : "");
    node->attr_count++;
    notify_attr(node, name, NULL);
}

/**
 * @brief 移除元素上的属性 / Remove an attribute from an element
 *
 * @param[in,out] node DOM 节点 / DOM node
 * @param[in]     name 属性名，大小写不敏感 / Attribute name, matched case-insensitively
 *
 * @note 属性不存在时不做任何事；成功移除后报告一次属性变更 / A no-op when the attribute is absent; a successful removal reports an attribute mutation
 *
 * @warning `node` 或 `name` 为 `NULL` 时直接返回 / Returns immediately when `node` or `name` is `NULL`
 */
void hpe_dom_remove_attr(HpeDomNode *node, const char *name)
{
    int i;
    if (!node || !name)
        return;
    for (i = 0; i < node->attr_count; i++) {
        if (hpe_str_ieq(node->attrs[i].name, name)) {
            char *old = hpe_strdup(node->attrs[i].value);
            int j;
            hpe_free(node->attrs[i].name);
            hpe_free(node->attrs[i].value);
            for (j = i; j + 1 < node->attr_count; j++)
                node->attrs[j] = node->attrs[j + 1];
            node->attr_count--;
            notify_attr(node, name, old);
            hpe_free(old);
            return;
        }
    }
}

/**
 * @brief 设置文本节点的内容 / Set the content of a text node
 *
 * @details 仅接受文本节点；写入后以 `HPE_MUT_TEXT` 报告一次变更，使 MutationObserver 与引擎脏标记都能看到 `characterData` 修改。
 *          Only text nodes are accepted; the write reports one `HPE_MUT_TEXT` change so both the MutationObserver bridge and the engine's dirty tracker observe a `characterData` modification.
 *
 * @param[in,out] node 文本节点（其他类型直接返回）/ Text node; other node types return immediately
 * @param[in]     text 新内容（`NULL` 视为空串）/ New content, `NULL` falls back to the empty string
 *
 * @note 新值与旧值相同则不做任何事 / Does nothing when the new value equals the old one
 *
 * @see hpe_dom_text
 */
void hpe_dom_set_text(HpeDomNode *node, const char *text)
{
    const char *next = text ? text : "";
    char *old;
    if (!node || node->type != HPE_NODE_TEXT)
        return;
    if (node->text && !strcmp(node->text, next))
        return;
    old = hpe_strdup(node->text ? node->text : "");
    hpe_free(node->text);
    node->text = hpe_strdup(next);
    notify_text(node, old);
    hpe_free(old);
}

/**
 * @brief 按文档顺序查找首个指定标签的元素 / Find the first element with the given tag in document order
 *
 * @param[in,out] root 查找起点（自身不计入），可为 `NULL` / Search root, itself excluded; may be `NULL`
 * @param[in]     tag 标签名，大小写不敏感 / Tag name, matched case-insensitively
 *
 * @return 命中的元素或 `NULL` / The matching element, or `NULL`
 *
 * @note 递归深度优先，只比较元素节点 / Recurses depth-first and only compares element nodes
 */
HpeDomNode *hpe_dom_find_tag(HpeDomNode *root, const char *tag)
{
    HpeDomNode *child, *found;
    if (!root || !tag)
        return NULL;
    for (child = root->first_child; child; child = child->next_sibling) {
        if (child->type == HPE_NODE_ELEMENT && hpe_str_ieq(child->tag, tag))
            return child;
        found = hpe_dom_find_tag(child, tag);
        if (found)
            return found;
    }
    return NULL;
}

/**
 * @brief 判断节点是否为元素 / Test whether a node is an element
 *
 * @param[in]     node DOM 节点 / DOM node
 *
 * @return 元素节点返回 1 / 1 when the node is an element
 *
 * @note `node` 为 `NULL` 时返回 0 / Returns 0 for a `NULL` node
 */
int hpe_dom_is_element(const HpeDomNode *node)
{
    return node && node->type == HPE_NODE_ELEMENT;
}

/**
 * @brief 递归释放节点及其整个子树 / Recursively free a node and its whole subtree
 *
 * @param[in,out] node DOM 节点 / DOM node
 *
 * @note 同时释放 `<template>` 的 content 片段、全部属性与标签/文本字符串；传 `NULL` 安全无操作 / Also frees a `<template>`'s content fragment, every attribute and the tag/text strings; a `NULL` input is a safe no-op
 */
void hpe_dom_free(HpeDomNode *node)
{
    HpeDomNode *child, *next;
    int i;
    if (!node)
        return;
    /* a <template>'s content is a detached fragment; free it with the element */
    if (node->content)
        hpe_dom_free(node->content);
    for (child = node->first_child; child; child = next) {
        next = child->next_sibling;
        hpe_dom_free(child);
    }
    for (i = 0; i < node->attr_count; i++) {
        hpe_free(node->attrs[i].name);
        hpe_free(node->attrs[i].value);
    }
    if (node->attrs)
        hpe_free(node->attrs);
    if (node->tag)
        hpe_free(node->tag);
    if (node->text)
        hpe_free(node->text);
    hpe_free(node);
}
