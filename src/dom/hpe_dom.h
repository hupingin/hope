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
    int hovered;     /* dynamic state: pointer is over this element (:hover) */
} HpeDomNode;

/* the child-list container a node's children should be read from / written to:
 * a <template> redirects to its content fragment, everything else is the node
 * itself. Used by the parser, innerHTML and textContent. */
HpeDomNode *hpe_dom_children_root(HpeDomNode *node);

HpeDomNode *hpe_dom_element(const char *tag);
HpeDomNode *hpe_dom_text(const char *text);
HpeDomNode *hpe_dom_document(void);
HpeDomNode *hpe_dom_fragment(void);
void hpe_dom_append(HpeDomNode *parent, HpeDomNode *child);
void hpe_dom_detach(HpeDomNode *node);

/* inserts `child` as a child of `parent`, immediately before `ref`. `ref` may be
 * NULL or not a child of `parent`, in which case this appends. If `child` is a
 * DocumentFragment its children are moved in order and the empty fragment is
 * freed. Detaches `child` from any previous parent first. */
void hpe_dom_insert_before(HpeDomNode *parent, HpeDomNode *child, HpeDomNode *ref);
/* replaces `old` (a child of `parent`) with `new`; appends when `old` is NULL.
 * returns the node that was removed (detached, caller owns it) or NULL. */
HpeDomNode *hpe_dom_replace_child(HpeDomNode *parent, HpeDomNode *new_child,
                                  HpeDomNode *old_child);
/* deep (deep != 0) or shallow clone. Cloned element keeps its attributes;
 * a cloned node has no parent/siblings/children unless deep. */
HpeDomNode *hpe_dom_clone(const HpeDomNode *node, int deep);

const char *hpe_dom_attr(const HpeDomNode *node, const char *name);
void hpe_dom_set_attr(HpeDomNode *node, const char *name, const char *value);
/* removes an attribute (a no-op when absent); reports a mutation when removed */
void hpe_dom_remove_attr(HpeDomNode *node, const char *name);

/* the element immediately preceding `node` among its parent's children, or NULL */
HpeDomNode *hpe_dom_prev_sibling(const HpeDomNode *node);

/* first element with the given tag in document order (root excluded) */
HpeDomNode *hpe_dom_find_tag(HpeDomNode *root, const char *tag);
int hpe_dom_is_element(const HpeDomNode *node);

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
/* Adds a sink. Several may be installed (the engine's dirty tracker and the JS
 * MutationObserver bridge); they are all notified, in installation order.
 * Passing fn == NULL with a matching ud removes that sink. */
void hpe_dom_add_mutation_sink(HpeDomMutationFn fn, void *ud);
void hpe_dom_remove_mutation_sink(HpeDomMutationFn fn, void *ud);
/* removes every sink (used when the document is torn down) */
void hpe_dom_clear_mutation_sinks(void);

#endif /* HPE_DOM_H */
