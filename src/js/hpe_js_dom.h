/* Hope UI Engine - javascript DOM bridge */
#ifndef HPE_JS_DOM_H
#define HPE_JS_DOM_H

#include "dom/hpe_dom.h"
#include "js/hpe_js.h"

enum {
    HPE_HOST_DOCUMENT = 1,
    HPE_HOST_ELEMENT = 2,
    HPE_HOST_STYLE = 3
};

/* registers the "document" host object (plus element/style vtables) */
void hpe_js_dom_install(HpeJs *js, HpeDomNode *document);

/* dispatches an event to `target` and its ancestors (bubbling); passes an
 * event object { type, clientX, clientY, target, currentTarget, preventDefault }.
 * returns 1 if any handler was invoked. */
int hpe_js_dom_dispatch(HpeJs *js, HpeDomNode *target, const char *type, int clientX,
                        int clientY);

#endif /* HPE_JS_DOM_H */
