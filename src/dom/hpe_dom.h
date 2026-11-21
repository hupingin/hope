/* Hope UI Engine - document object model */
#ifndef HPE_DOM_H
#define HPE_DOM_H

#include "base/hpe_types.h"

typedef enum HpeNodeType {
    HPE_NODE_DOCUMENT = 0,
    HPE_NODE_ELEMENT,
    HPE_NODE_TEXT
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
    void *listeners; /* DOM bridge: per-node event listener list (opaque) */
    void *box;       /* layout: box generated for this element (opaque) */
    int hovered;     /* dynamic state: pointer is over this element (:hover) */
} HpeDomNode;

HpeDomNode *hpe_dom_element(const char *tag);
HpeDomNode *hpe_dom_text(const char *text);
HpeDomNode *hpe_dom_document(void);
void hpe_dom_append(HpeDomNode *parent, HpeDomNode *child);
void hpe_dom_detach(HpeDomNode *node);

const char *hpe_dom_attr(const HpeDomNode *node, const char *name);
void hpe_dom_set_attr(HpeDomNode *node, const char *name, const char *value);

/* the element immediately preceding `node` among its parent's children, or NULL */
HpeDomNode *hpe_dom_prev_sibling(const HpeDomNode *node);

/* first element with the given tag in document order (root excluded) */
HpeDomNode *hpe_dom_find_tag(HpeDomNode *root, const char *tag);
int hpe_dom_is_element(const HpeDomNode *node);

void hpe_dom_free(HpeDomNode *node);

#endif /* HPE_DOM_H */
