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

void hpe_dom_clear_mutation_sinks(void)
{
    g_mut_sink_count = 0;
}

/* legacy single-sink entry point: installs/replaces nothing, just adds */
void hpe_dom_set_mutation_sink(HpeDomMutationFn fn, void *ud)
{
    if (!fn) {
        hpe_dom_clear_mutation_sinks();
        return;
    }
    hpe_dom_add_mutation_sink(fn, ud);
}

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

static void notify_child_list(HpeDomNode *parent, HpeDomNode *added, HpeDomNode *removed)
{
    notify_sinks(HPE_MUT_CHILD_LIST, parent, NULL, NULL, added, removed);
}

static void notify_attr(HpeDomNode *node, const char *name, const char *old_value)
{
    notify_sinks(HPE_MUT_ATTR, node, name, old_value, NULL, NULL);
}

static void notify_text(HpeDomNode *node, const char *old_value)
{
    notify_sinks(HPE_MUT_TEXT, node, NULL, old_value, NULL, NULL);
}

static HpeDomNode *dom_new(HpeNodeType type)
{
    HpeDomNode *node = (HpeDomNode *)hpe_calloc(1, sizeof(HpeDomNode));
    node->type = type;
    return node;
}

HpeDomNode *hpe_dom_element(const char *tag)
{
    HpeDomNode *node = dom_new(HPE_NODE_ELEMENT);
    node->tag = hpe_strdup(tag ? tag : "div");
    return node;
}

HpeDomNode *hpe_dom_text(const char *text)
{
    HpeDomNode *node = dom_new(HPE_NODE_TEXT);
    node->text = hpe_strdup(text ? text : "");
    return node;
}

HpeDomNode *hpe_dom_document(void)
{
    return dom_new(HPE_NODE_DOCUMENT);
}

HpeDomNode *hpe_dom_fragment(void)
{
    return dom_new(HPE_NODE_FRAGMENT);
}

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

/* true when `ref` is a direct child of `parent` */
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

int hpe_dom_is_element(const HpeDomNode *node)
{
    return node && node->type == HPE_NODE_ELEMENT;
}

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
