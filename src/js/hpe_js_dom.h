/* Hope UI Engine - javascript DOM bridge */
#ifndef HPE_JS_DOM_H
#define HPE_JS_DOM_H

#include "dom/hpe_dom.h"
#include "js/hpe_js.h"

enum {
    HPE_HOST_DOCUMENT = 1,
    HPE_HOST_ELEMENT = 2,
    HPE_HOST_STYLE = 3,
    HPE_HOST_FRAGMENT = 4,
    /* a MutationObserver host object (its own methods live as own properties) */
    HPE_HOST_MUTATION_OBSERVER = 5
};

/* registers the "document" host object (plus element/style vtables) */
void hpe_js_dom_install(HpeJs *js, HpeDomNode *document);

/* wraps a DOM node in the host value scripts see (element or fragment vtable).
 * exposed so the MutationObserver bridge can hand back a fully usable node as a
 * MutationRecord's target. */
JsValue hpe_js_dom_node_value(HpeJs *js, HpeDomNode *node);

/* dispatches an event to `target` and its ancestors (bubbling); passes an
 * event object { type, clientX, clientY, target, currentTarget, preventDefault }.
 * returns 1 if any handler was invoked. */
int hpe_js_dom_dispatch(HpeJs *js, HpeDomNode *target, const char *type, int clientX,
                        int clientY);

#endif /* HPE_JS_DOM_H */
