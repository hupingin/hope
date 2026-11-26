/* Hope UI Engine - document object model */
#ifndef HPE_DOM_H
#define HPE_DOM_H

#include "base/hpe_types.h"

typedef enum HpeNodeType {
    HPE_NODE_DOCUMENT = 0,
    HPE_NODE_ELEMENT,
    HPE_NODE_TEXT,
    /* a DocumentFragment: a child-list container that is not itself part of the
     * tree; appending it to a parent moves its children instead (see
     * hpe_dom_append). It never reaches the box tree. */
    HPE_NODE_FRAGMENT
} HpeNodeType;

typedef struct HpeAttr {
    char *name;
    char *value;
} HpeAttr;

typedef struct HpeDomNode {
    HpeNodeType type;
    char *tag;  /* element: lowercase tag name */
    char *text; /* text node: decoded content */
    HpeAttr *attrs;
    int attr_count;
    size_t attr_cap;
    struct HpeDomNode *parent;
    struct HpeDomNode *first_child;
    struct HpeDomNode *last_child;
    struct HpeDomNode *next_sibling;
    /* <template> only: its children live in this detached DocumentFragment
     * (the spec's HTMLTemplateElement "content"), never in the main tree. Every
     * other node leaves it NULL, so the box tree never sees the content. */
    struct HpeDomNode *content;
    void *listeners; /* DOM bridge: per-node event listener list (opaque) */
    void *box;       /* layout: box generated for this element (opaque) */
    /* dynamic state driving the state pseudo-classes; all are maintained by the
     * engine/host layer and read by the CSS matcher. */
    int hovered;  /* :hover — pointer is over this element */
    int focused;  /* :focus — this element owns the focus */
    int active;   /* :active — pointer is pressed on this element */
    int visited;  /* :visited — element was activated before */
    int targeted; /* :target — element is the fragment target of the URL */
} HpeDomNode;

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
HpeDomNode *hpe_dom_children_root(HpeDomNode *node);

/**
 * @brief 创建一个元素节点 / Create an element node
 *
 * @param[in]     tag 标签名（`NULL` 视为 `div`）/ Tag name, `NULL` falls back to `div`
 *
 * @return 新的元素节点 / The new element node
 */
HpeDomNode *hpe_dom_element(const char *tag);

/**
 * @brief 创建一个文本节点 / Create a text node
 *
 * @param[in]     text 文本内容（`NULL` 视为空串）/ Text content, `NULL` falls back to the empty string
 *
 * @return 新的文本节点 / The new text node
 */
HpeDomNode *hpe_dom_text(const char *text);

/**
 * @brief 设置文本节点的内容 / Set the content of a text node
 *
 * @param[in,out] node 文本节点（其他类型直接返回）/ Text node; other node types return immediately
 * @param[in]     text 新内容（`NULL` 视为空串）/ New content, `NULL` falls back to the empty string
 *
 * @note 以 `HPE_MUT_TEXT` 报告一次变更，使 `characterData` 观察者与引擎脏标记都能看到 / Reports one `HPE_MUT_TEXT` change so `characterData` observers and the engine's dirty tracker both see it
 *
 * @see hpe_dom_text
 */
void hpe_dom_set_text(HpeDomNode *node, const char *text);

/**
 * @brief 创建一个文档节点 / Create a document node
 *
 * @return 新的文档节点 / The new document node
 */
HpeDomNode *hpe_dom_document(void);
/**
 * @brief 创建一个文档片段节点 / Create a document-fragment node
 *
 * @return 新的片段节点 / The new fragment node
 *
 * @note 片段本身不属于树；把它追加到父节点时会搬移其子节点 / The fragment itself is not part of the tree; appending it moves its children instead
 */
HpeDomNode *hpe_dom_fragment(void);

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
void hpe_dom_append(HpeDomNode *parent, HpeDomNode *child);

/**
 * @brief 把节点从其父节点上摘下 / Detach a node from its parent
 *
 * @param[in,out] node DOM 节点 / DOM node
 *
 * @note 只更新兄弟链与父指针，不释放节点；未挂载的节点直接返回 / Only the sibling links and parent pointer are updated, the node is not freed; an unattached node returns unchanged
 *
 * @see hpe_dom_append hpe_dom_free
 */
void hpe_dom_detach(HpeDomNode *node);

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
void hpe_dom_insert_before(HpeDomNode *parent, HpeDomNode *child, HpeDomNode *ref);

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
                                  HpeDomNode *old_child);

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
HpeDomNode *hpe_dom_clone(const HpeDomNode *node, int deep);

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
const char *hpe_dom_attr(const HpeDomNode *node, const char *name);

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
void hpe_dom_set_attr(HpeDomNode *node, const char *name, const char *value);

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
void hpe_dom_remove_attr(HpeDomNode *node, const char *name);

/**
 * @brief 取得节点的前一个兄弟节点 / Obtain the preceding sibling of a node
 *
 * @param[in]     node DOM 节点 / DOM node
 *
 * @return 父节点下紧邻的前一个兄弟，无则 `NULL` / The immediately preceding sibling, or `NULL`
 *
 * @note 无父节点或是首个子节点时返回 `NULL` / Returns `NULL` when the node has no parent or is the first child
 */
HpeDomNode *hpe_dom_prev_sibling(const HpeDomNode *node);

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
HpeDomNode *hpe_dom_find_tag(HpeDomNode *root, const char *tag);

/**
 * @brief 判断节点是否为元素 / Test whether a node is an element
 *
 * @param[in]     node DOM 节点 / DOM node
 *
 * @return 元素节点返回 1 / 1 when the node is an element
 *
 * @note `node` 为 `NULL` 时返回 0 / Returns 0 for a `NULL` node
 */
int hpe_dom_is_element(const HpeDomNode *node);

/**
 * @brief 递归释放节点及其整个子树 / Recursively free a node and its whole subtree
 *
 * @param[in,out] node DOM 节点 / DOM node
 *
 * @note 同时释放 `<template>` 的 content 片段、全部属性与标签/文本字符串；传 `NULL` 安全无操作 / Also frees a `<template>`'s content fragment, every attribute and the tag/text strings; a `NULL` input is a safe no-op
 */
void hpe_dom_free(HpeDomNode *node);

/* ---------------- mutation notification ----------------
 * Structural + attribute changes report through a single installed sink. It
 * serves two consumers: the JS MutationObserver bridge (which builds records
 * and queues a microtask) and the engine's dirty-layout tracker (which records
 * whether the change can affect layout). The DOM layer itself stays free of
 * any js/ or layout/ dependency. */
#define HPE_MUT_CHILD_LIST 1 /* target(child list owner), added/removed node */
#define HPE_MUT_ATTR 2       /* target element, name, old_value */
#define HPE_MUT_TEXT 3       /* target text node, old_value */

typedef void (*HpeDomMutationFn)(void *ud, int kind, HpeDomNode *target,
                                 const char *name, const char *old_value,
                                 HpeDomNode *added, HpeDomNode *removed);

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
void hpe_dom_add_mutation_sink(HpeDomMutationFn fn, void *ud);

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
void hpe_dom_remove_mutation_sink(HpeDomMutationFn fn, void *ud);

/**
 * @brief 移除全部变更订阅者 / Remove every mutation sink
 *
 * @details 文档销毁时调用，使 `notify_sinks` 之后不再回调任何人。
 *          Called when the document is torn down so that `notify_sinks` no longer reaches anyone.
 */
void hpe_dom_clear_mutation_sinks(void);

#endif /* HPE_DOM_H */
