#include "dom/hpe_dom.h"

#include <string.h>

#include "base/hpe_util.h"

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

void hpe_dom_append(HpeDomNode *parent, HpeDomNode *child)
{
    if (!parent || !child)
        return;
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
}

void hpe_dom_detach(HpeDomNode *node)
{
    HpeDomNode *prev, *cur;
    if (!node || !node->parent)
        return;
    prev = NULL;
    cur = node->parent->first_child;
    while (cur) {
        if (cur == node) {
            if (prev)
                prev->next_sibling = cur->next_sibling;
            else
                node->parent->first_child = cur->next_sibling;
            if (node->parent->last_child == cur)
                node->parent->last_child = prev;
            break;
        }
        prev = cur;
        cur = cur->next_sibling;
    }
    node->parent = NULL;
    node->next_sibling = NULL;
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
            hpe_free(node->attrs[i].value);
            node->attrs[i].value = hpe_strdup(value ? value : "");
            return;
        }
    }
    node->attrs = (HpeAttr *)hpe_grow(node->attrs, &node->attr_cap,
                                      (size_t)node->attr_count + 1, sizeof(HpeAttr));
    node->attrs[node->attr_count].name = hpe_strdup(name);
    node->attrs[node->attr_count].value = hpe_strdup(value ? value : "");
    node->attr_count++;
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
