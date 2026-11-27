/* Hope UI Engine - <details>/<summary> and <dialog> semantics (see hope_details.h) */
#include "hope_details.h"

#include <string.h>

/* the single dialog acting as a modal top layer; NULL when none is open */
static HpeDomNode *g_modal_dialog = NULL;

int hpe_details_is_summary(const HpeDomNode *node)
{
    return node && node->type == HPE_NODE_ELEMENT && node->tag &&
           !strcmp(node->tag, "summary");
}

HpeDomNode *hpe_details_owner(HpeDomNode *node)
{
    HpeDomNode *n;
    /* only a click on the <summary> (or inside it) toggles; clicking the
     * <details> content area must not, matching browser behaviour */
    for (n = node; n; n = n->parent) {
        if (hpe_details_is_summary(n)) {
            HpeDomNode *p = n->parent;
            return (p && p->type == HPE_NODE_ELEMENT && p->tag &&
                    !strcmp(p->tag, "details")) ? p : NULL;
        }
        /* stop if we climbed past the owning details without seeing a summary */
        if (n->type == HPE_NODE_ELEMENT && n->tag && !strcmp(n->tag, "details"))
            return NULL;
    }
    return NULL;
}

int hpe_details_is_open(const HpeDomNode *details)
{
    return details && hpe_dom_attr(details, "open") != NULL;
}

int hpe_details_toggle(HpeDomNode *details)
{
    int was_open;
    if (!details)
        return 0;
    was_open = hpe_details_is_open(details);
    if (was_open)
        hpe_dom_remove_attr(details, "open");
    else
        hpe_dom_set_attr(details, "open", "");
    return was_open;
}

int hpe_dialog_is(const HpeDomNode *node)
{
    return node && node->type == HPE_NODE_ELEMENT && node->tag &&
           !strcmp(node->tag, "dialog");
}

void hpe_dialog_set_modal(HpeDomNode *dialog)
{
    g_modal_dialog = dialog;
}

HpeDomNode *hpe_dialog_modal(void)
{
    return g_modal_dialog;
}
