/* Hope UI Engine - MutationObserver (DOM change notification) */
#ifndef HPE_JS_MUTATION_H
#define HPE_JS_MUTATION_H

#include "js/hpe_js.h"

/* per-interpreter observer bookkeeping; one allocation, owned by the js arena */
typedef struct HpeJsMutationState {
    void *observers;      /* JsMutationObserver list (opaque) */
    HpeJs *js;
    int sink_installed;   /* the DOM mutation sink is registered */
} HpeJsMutationState;

HpeJsMutationState *hpe_js_mutation_state(HpeJs *js);
/* registers the DOM mutation sink and installs the global MutationObserver */
void hpe_js_mutation_install(HpeJs *js);
void hpe_js_mutation_uninstall(HpeJs *js);
void hpe_js_mutation_install_ctor(HpeJs *js);

#endif /* HPE_JS_MUTATION_H */
