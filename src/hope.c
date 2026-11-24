#include "hope.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"
#include "css/hpe_css.h"
#include "dom/hpe_dom.h"
#include "hope_internal.h"
#include "html/hpe_encoding.h"
#include "paint/hpe_svg.h"
#include "html/hpe_html.h"
#include "js/hpe_js.h"
#include "js/hpe_js_dom.h"
#include "layout/hpe_layout.h"
#include "paint/hpe_paint.h"
#include "platform/hpe_platform.h"

#define HPE_MAX_SCRIPTS 64

/* built-in "blank page" shown when the engine is started without a file */
static const char BLANK_PAGE[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>blank</title>"
    "<style>"
    "html,body{margin:0;padding:0;background:#ffffff;}"
    ".hint{margin:0;padding:56px 0 0 0;text-align:center;color:#b3bac4;"
    "font-family:sans-serif;font-size:14px;}"
    "</style></head><body>"
    "<p class=\"hint\">blank page \xE2\x80\x94 "
    "\xE5\x9C\xA8\xE4\xB8\x8A\xE6\x96\xB9\xE5\x9C\xB0\xE5\x9D\x80\xE6\xA0\x8F"
    "\xE8\xBE\x93\xE5\x85\xA5\xE6\x9C\xAC\xE5\x9C\xB0 HTML "
    "\xE6\x96\x87\xE4\xBB\xB6\xE8\xB7\xAF\xE5\xBE\x84\xEF\xBC\x8C"
    "\xE5\x9B\x9E\xE8\xBD\xA6\xE6\x89\x93\xE5\xBC\x80</p>"
    "</body></html>";

static int click_element_by_id(HpeEngine *engine, const char *id);
static void print_text_by_id(HpeEngine *engine, const char *id);

/* ---------------- stylesheets ---------------- */

/* Finds the document's <base href> (the first one wins, per spec). Returns a
 * pointer to the attribute value (owned by the DOM) or NULL. */
static const char *find_base_href(HpeDomNode *node)
{
    HpeDomNode *child;
    for (child = node->first_child; child; child = child->next_sibling) {
        if (child->type == HPE_NODE_ELEMENT) {
            const char *base = NULL;
            if (strcmp(child->tag, "base") == 0)
                base = hpe_dom_attr(child, "href");
            if (base && *base)
                return base;
            {
                const char *found = find_base_href(child);
                if (found)
                    return found;
            }
        }
    }
    return NULL;
}

/* Resolves a document-relative reference against <base href> (when present) and
 * the document directory, writing the absolute result to `out`. */
static void resolve_url(HpeEngine *engine, char *out, size_t out_size, const char *dir,
                        const char *rel)
{
    if (engine->base_href[0] && !hpe_path_is_absolute(rel)) {
        char basedir[HPE_MAX_PATH];
        hpe_path_dir(engine->base_href, basedir, sizeof(basedir));
        hpe_path_join(out, out_size, basedir, rel);
    } else {
        hpe_path_join(out, out_size, dir, rel);
    }
}

/* normalises a rel attribute into a lowercase space-separated token string so
 * `rel="ICON"` and `rel="shortcut icon"` compare predictably */
static void normalize_rel(const char *rel, char *out, size_t out_size)
{
    size_t o = 0;
    int last_space = 1;
    for (; rel && *rel && o + 1 < out_size; rel++) {
        if (hpe_is_space(*rel)) {
            if (!last_space) {
                out[o++] = ' ';
                last_space = 1;
            }
        } else {
            out[o++] = (char)hpe_ascii_lower((unsigned char)*rel);
            last_space = 0;
        }
    }
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = '\0';
}

static int rel_has_token(const char *rel, const char *token)
{
    const char *p = rel;
    size_t tl = strlen(token);
    while (*p) {
        while (*p == ' ')
            p++;
        if (hpe_str_ieq_n(p, token, tl) && (p[tl] == ' ' || p[tl] == '\0'))
            return 1;
        while (*p && *p != ' ')
            p++;
    }
    return 0;
}

static void collect_styles(HpeEngine *engine, HpeDomNode *node, const char *dir)
{
    HpeDomNode *child;

    for (child = node->first_child; child; child = child->next_sibling) {
        if (child->type != HPE_NODE_ELEMENT)
            continue;

        if (strcmp(child->tag, "style") == 0) {
            HpeDomNode *text = child->first_child;
            if (text && text->type == HPE_NODE_TEXT && text->text)
                hpe_css_parse(&engine->sheet, text->text, strlen(text->text));
        } else if (strcmp(child->tag, "link") == 0) {
            const char *rel = hpe_dom_attr(child, "rel");
            const char *href = hpe_dom_attr(child, "href");
            if (rel && href) {
                if (rel_has_token(rel, "stylesheet")) {
                    char full[HPE_MAX_PATH];
                    char *css;
                    resolve_url(engine, full, sizeof(full), dir, href);
                    css = hpe_read_file(full, NULL);
                    if (css) {
                        hpe_css_parse(&engine->sheet, css, strlen(css));
                        hpe_free(css);
                    } else {
                        fprintf(stderr, "hope: cannot open stylesheet %s\n", full);
                    }
                } else if (engine->link_count < HPE_MAX_LINKS) {
                    /* record non-stylesheet resource hints (icon/preload/...) */
                    int idx = engine->link_count++;
                    char tokens[24];
                    normalize_rel(rel, tokens, sizeof(tokens));
                    snprintf(engine->links[idx].rel, sizeof(engine->links[idx].rel), "%s",
                             tokens);
                    resolve_url(engine, engine->links[idx].href,
                                sizeof(engine->links[idx].href), dir, href);
                }
            }
        }
        collect_styles(engine, child, dir);
    }
}

/* A script can add/remove a <style>/<link> or rewrite a <style>'s text after the
 * first collection. The DOM sink below reports exactly those edits; a later
 * relayout then rebuilds the author rules. The sink fires before the sheet is
 * touched, so it only records "a sheet-affecting change happened" + a signature
 * of the last node involved; the real re-read happens in refresh_styles(). */
static HpeDomNode *g_style_sig;
static unsigned int g_style_rev;
static int g_style_dirty;

static int is_style_host(HpeDomNode *n)
{
    if (!n)
        return 0;
    if (n->type == HPE_NODE_ELEMENT)
        return strcmp(n->tag, "style") == 0 || strcmp(n->tag, "link") == 0;
    if (n->type == HPE_NODE_TEXT && n->parent)
        return strcmp(n->parent->tag, "style") == 0;
    return 0;
}

/* ---------------- mutation classification ----------------
 * Every DOM change reports here. Two consumers share the record:
 *   - the author-sheet tracker (a <style>/<link> touched -> re-read the sheet)
 *   - the reflow tracker (how much layout/repaint the change needs)
 * The reflow level only ever ratchets up; flush_mutations() acts on it and
 * then resets. */
/* Reflow needed by a style declaration diff. Any geometry-affecting property ->
 * LAYOUT; a purely visual property (colour, opacity, shadow) -> PAINT; a value
 * that only aliases something already painted (e.g. background-color on an
 * element whose box draws a background-image) still needs the box tree, so the
 * whole declaration set is conservatively treated as LAYOUT unless every single
 * changed bit is known to be paint-only. */
static int reflow_for_props(uint64_t set, uint64_t set2)
{
    /* geometry: box metrics, position, flow, typography (all change line boxes) */
    const uint64_t geom =
        HPE_PROP_DISPLAY | HPE_PROP_WIDTH | HPE_PROP_HEIGHT | HPE_PROP_MARGIN_TOP |
        HPE_PROP_MARGIN_RIGHT | HPE_PROP_MARGIN_BOTTOM | HPE_PROP_MARGIN_LEFT |
        HPE_PROP_PADDING_TOP | HPE_PROP_PADDING_RIGHT | HPE_PROP_PADDING_BOTTOM |
        HPE_PROP_PADDING_LEFT | HPE_PROP_BORDER_WIDTH | HPE_PROP_FONT_SIZE |
        HPE_PROP_FONT_WEIGHT | HPE_PROP_TEXT_ALIGN | HPE_PROP_LINE_HEIGHT |
        HPE_PROP_WHITE_SPACE | HPE_PROP_FONT_FAMILY | HPE_PROP_POSITION |
        HPE_PROP_INSET_TOP | HPE_PROP_INSET_RIGHT | HPE_PROP_INSET_BOTTOM |
        HPE_PROP_INSET_LEFT | HPE_PROP_Z_INDEX | HPE_PROP_FLOAT | HPE_PROP_CLEAR |
        HPE_PROP_BORDER_RADIUS | HPE_PROP_BORDER_TOP | HPE_PROP_BORDER_RIGHT |
        HPE_PROP_BORDER_BOTTOM | HPE_PROP_BORDER_LEFT | HPE_PROP_FLEX_DIR |
        HPE_PROP_ALIGN_ITEMS | HPE_PROP_JUSTIFY | HPE_PROP_GAP | HPE_PROP_FLEX_GROW |
        HPE_PROP_ALIGN_SELF | HPE_PROP_MIN_WIDTH;
    const uint64_t geom2 = HPE_PROP2_MIN_HEIGHT | HPE_PROP2_MAX_WIDTH | HPE_PROP2_MAX_HEIGHT |
                           HPE_PROP2_BOX_SIZING | HPE_PROP2_OVERFLOW_X | HPE_PROP2_OVERFLOW_Y |
                           HPE_PROP2_VISIBILITY | HPE_PROP2_LETTER_SPACING |
                           HPE_PROP2_WORD_SPACING | HPE_PROP2_TEXT_INDENT |
                           HPE_PROP2_VERTICAL_ALIGN | HPE_PROP2_LIST_STYLE_TYPE |
                           HPE_PROP2_LIST_STYLE_POSITION | HPE_PROP2_BACKGROUND_IMAGE |
                           HPE_PROP2_TRANSFORM;
    if ((set & geom) || (set2 & geom2))
        return HPE_REFLOW_LAYOUT;
    if (set || set2)
        return HPE_REFLOW_PAINT;
    return HPE_REFLOW_NONE;
}

/* Does the style attribute change from old to new in any way that can affect
 * layout or paint? Returns the reflow level. */
static int reflow_for_inline_style(const char *old_value, const char *new_value)
{
    HpeStyle a, b;
    a = hpe_style_default();
    b = hpe_style_default();
    if (old_value)
        hpe_css_parse_declarations(old_value, strlen(old_value), &a);
    if (new_value)
        hpe_css_parse_declarations(new_value, strlen(new_value), &b);
    if (a.set == b.set && a.set2 == b.set2)
        return HPE_REFLOW_NONE;
    return reflow_for_props(a.set ^ b.set, a.set2 ^ b.set2);
}

static int reflow_for_attr(HpeDomNode *target, const char *name, const char *old_value)
{
    const char *new_value;
    if (!target || !name)
        return HPE_REFLOW_LAYOUT;
    if (target->type == HPE_NODE_ELEMENT && target->tag && strcmp(target->tag, "svg") == 0)
        return HPE_REFLOW_LAYOUT; /* SVG geometry lives in attributes */
    /* `style` is the only attribute that reaches the cascade; class/id change
     * which rules match; everything else is inert for this engine. */
    if (hpe_str_ieq(name, "style")) {
        new_value = hpe_dom_attr(target, name);
        return reflow_for_inline_style(old_value, new_value);
    }
    if (hpe_str_ieq(name, "class") || hpe_str_ieq(name, "id"))
        return HPE_REFLOW_LAYOUT;
    return HPE_REFLOW_NONE;
}

static void note_reflow(HpeEngine *engine, int need)
{
    if (!engine || need <= engine->reflow_need)
        return;
    engine->reflow_need = need;
}

/* true when `anc` is `node` itself or one of its DOM ancestors */
static int dom_is_ancestor(HpeDomNode *anc, HpeDomNode *node)
{
    for (; node; node = node->parent)
        if (node == anc)
            return 1;
    return 0;
}

/* Records the DOM node whose box subtree an incremental rebuild has to replace.
 * Keeps the outermost (closest to the root) candidate seen, so a change nested
 * deep inside an already-recorded subtree does not narrow the scope. The node
 * must be a real element that is still attached; a fragment or a detached node
 * carries no box information and is ignored. */
static void note_reflow_node(HpeEngine *engine, HpeDomNode *node)
{
    if (!engine || !node)
        return;
    if (node->type == HPE_NODE_TEXT)
        node = node->parent;
    while (node && node->type != HPE_NODE_ELEMENT)
        node = node->parent;
    if (!node || node->type != HPE_NODE_ELEMENT || !node->parent)
        return; /* detached: no box to splice against (a full build covers it) */
    if (!engine->reflow_node || dom_is_ancestor(node, engine->reflow_node))
        engine->reflow_node = node;
}

static void style_sink(void *ud, int kind, HpeDomNode *target, const char *name,
                       const char *old_value, HpeDomNode *added, HpeDomNode *removed)
{
    HpeEngine *engine = (HpeEngine *)ud;
    int need = HPE_REFLOW_NONE;

    /* ---- author-sheet tracking ---- */
    if (is_style_host(target) || is_style_host(added) || is_style_host(removed)) {
        g_style_dirty = 1;
        if (is_style_host(added))
            g_style_sig = added;
        else if (is_style_host(removed))
            g_style_sig = removed;
        else
            g_style_sig = target;
        g_style_rev++;
        need = HPE_REFLOW_LAYOUT;
    }

    /* ---- reflow classification + scope ---- */
    if (kind == HPE_MUT_CHILD_LIST) {
        /* a node entering or leaving the tree moves boxes around its parent;
         * `target` is that parent (the child-list owner) */
        if (added || removed) {
            note_reflow(engine, HPE_REFLOW_LAYOUT);
            note_reflow_node(engine, target);
        }
    } else if (kind == HPE_MUT_ATTR) {
        need = reflow_for_attr(target, name, old_value);
        note_reflow(engine, need);
        if (need == HPE_REFLOW_LAYOUT)
            note_reflow_node(engine, target);
    } else if (kind == HPE_MUT_TEXT) {
        /* a text edit changes a line box's content -> relayout of its parent */
        note_reflow(engine, HPE_REFLOW_LAYOUT);
        note_reflow_node(engine, target);
    }
    if (need)
        note_reflow(engine, need);
    if (engine) {
        engine->mutation_pending = 1;
        /* any <style>/<link> edit changes every box: no partial rebuild possible */
        if (is_style_host(target) || is_style_host(added) || is_style_host(removed))
            engine->reflow_node = NULL;
    }
}

/* Rebuilds author rules when a <style>/<link> appeared, vanished or changed.
 * Returns 1 when the sheet was rebuilt (the caller must re-cascade). */
static int refresh_styles(HpeEngine *engine)
{
    char dir[HPE_MAX_PATH];
    if (!g_style_dirty)
        return 0;
    g_style_dirty = 0;

    /* start from a clean sheet: hpe_css_init appends the user agent rules, then
     * collect_styles walks the (possibly edited) tree for the author ones. */
    hpe_css_free(&engine->sheet);
    hpe_css_init(&engine->sheet);
    engine->author_rule_base = engine->sheet.rule_count;
    engine->link_count = 0; /* re-scan, so the hint list does not accumulate */
    hpe_path_dir(engine->path ? engine->path : ".", dir, sizeof(dir));
    collect_styles(engine, engine->doc, dir);
    engine->style_sig = g_style_sig;
    engine->style_rev = g_style_rev;
    return 1;
}

/* Recomputes the style of every building block from the (possibly edited) DOM,
 * leaving geometry untouched. Needed after a paint-only change so the colour/
 * opacity/shadow a box carries stays in sync; a no-op when no such property
 * exists, so pure-geometry edits pay nothing. Paint-only edits cannot change
 * `display`, so the box tree's shape is stable and we only overwrite styles. */
static void restyle_boxes(HpeBox *box, HpeStylesheet *sheet, const HpeStyle *parent)
{
    HpeBox *child;
    HpeStyle style;
    HpeStyle child_parent;

    if (!box)
        return;
    if (box->kind == HPE_BOX_TEXT) {
        if (parent) {
            hpe_css_compute(sheet, NULL, parent, &style);
            box->style = style;
        }
        return;
    }
    hpe_css_compute(sheet, box->node, parent, &style);
    box->style = style;
    child_parent = style;
    for (child = box->first_child; child; child = child->next)
        restyle_boxes(child, sheet, &child_parent);
}

/* Rebuilds the box tree for a LAYOUT-level flush: a partial subtree rebuild when
 * a valid scope was recorded, otherwise a full rebuild. */
static void flush_rebuild_layout(HpeEngine *engine, HpeDomNode *scope)
{
    HpeBox *partial = NULL;

    if (scope && !getenv("HPE_NO_PARTIAL"))
        partial = hpe_layout_rebuild(engine->doc, &engine->sheet, (float)engine->width,
                                     (float)engine->height, engine->root, scope);
    if (partial) {
        if (getenv("HOPE_TRACE_REFLOW"))
            fprintf(stderr, "hope: flush -> LAYOUT (partial)\n");
    } else {
        if (getenv("HOPE_TRACE_REFLOW"))
            fprintf(stderr, "hope: flush -> LAYOUT (full)\n");
        if (engine->root) {
            hpe_layout_free(engine->root);
            engine->root = NULL;
        }
        engine->root = hpe_layout_build(engine->doc, &engine->sheet, (float)engine->width,
                                        (float)engine->height);
    }
    engine->content_height = hpe_layout_content_height(engine->root);
}

/* keeps the scroll offset inside [0, content_height - view_height] */
static void clamp_scroll(HpeEngine *engine)
{
    float max_scroll =
        HPE_MAX(0.0f, engine->content_height - (float)hpe_engine_view_height(engine));
    if (engine->scroll_y > max_scroll)
        engine->scroll_y = max_scroll;
    if (engine->scroll_y < 0.0f)
        engine->scroll_y = 0.0f;
}

/* Applies the mutations recorded since the last call. `force` (a fresh load or
 * resize) requests a full rebuild regardless. Drives the lazy-relayout policy:
 *   LAYOUT -> rebuild the affected box subtree (or everything when unbounded)
 *   PAINT  -> recompute styles + repaint
 *   NONE   -> nothing to do (the render step is skipped entirely). */
void hpe_engine_flush(HpeEngine *engine, int force)
{
    int need;
    HpeDomNode *scope = NULL;

    if (!engine)
        return;
    if (force) {
        need = HPE_REFLOW_LAYOUT;
    } else {
        need = engine->reflow_need;
        scope = engine->reflow_node;
        if (refresh_styles(engine))
            need = HPE_REFLOW_LAYOUT; /* the sheet changed: scope is meaningless */
        /* diagnostic/benchmark aid: force the eager path so the lazy result can
         * be compared against a full rebuild */
        if (getenv("HPE_FORCE_EAGER") && need != HPE_REFLOW_NONE) {
            need = HPE_REFLOW_LAYOUT;
            scope = NULL;
        }
    }
    engine->reflow_need = HPE_REFLOW_NONE;
    engine->reflow_node = NULL;
    engine->mutation_pending = 0;
    if (!engine->doc)
        return;

    hpe_css_set_viewport(&engine->sheet, (float)engine->width,
                         (float)hpe_engine_view_height(engine));

    if (need == HPE_REFLOW_LAYOUT) {
        flush_rebuild_layout(engine, scope);
    } else if (need == HPE_REFLOW_PAINT && engine->root) {
        HpeStyle root_parent = hpe_style_default();
        if (getenv("HOPE_TRACE_REFLOW"))
            fprintf(stderr, "hope: flush -> PAINT (restyle only)\n");
        restyle_boxes(engine->root, &engine->sheet, &root_parent);
    } else {
        if (getenv("HOPE_TRACE_REFLOW"))
            fprintf(stderr, "hope: flush -> NONE\n");
        return; /* nothing changed: leave the existing frame alone */
    }

    clamp_scroll(engine);
    engine->dirty = HPE_DIRTY_FULL;
}

/* ---------------- status ---------------- */

static const char *path_basename(const char *path)
{
    const char *base = path;
    const char *p;
    if (!path)
        return "";
    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return base;
}

void hpe_engine_refresh_status(HpeEngine *engine)
{
    int n = snprintf(engine->status, sizeof(engine->status),
                     "%s  |  %dx%d%s  |  load %.1f ms  |  paint %.1f ms  |  scroll %.0f/%.0f",
                     engine->path ? path_basename(engine->path) : "(blank page)", engine->width,
                     engine->height, engine->chrome_h > 0 ? " +chrome" : "", engine->load_ms,
                     engine->render_ms, engine->scroll_y,
                     HPE_MAX(0.0f, engine->content_height - (float)hpe_engine_view_height(engine)));
    if (engine->error[0] && n > 0 && n < (int)sizeof(engine->status))
        snprintf(engine->status + n, sizeof(engine->status) - (size_t)n, "  |  ERR: %s",
                 engine->error);
}

int hpe_engine_view_height(HpeEngine *engine)
{
    int h;
    if (!engine)
        return 0;
    h = engine->height - engine->chrome_h;
    return h > 0 ? h : 0;
}

static void dump_boxes(HpeBox *box, int depth)
{
    HpeBox *child;
    int i;
    for (i = 0; i < depth; i++)
        fprintf(stderr, "  ");
    fprintf(stderr, "%s<%s> bb=(%.0f,%.0f %.0fx%.0f) cb=(%.0f,%.0f %.0fx%.0f) lines=%d fs=%.0f\n",
            box->kind == HPE_BOX_TEXT ? "T" : (box->kind == HPE_BOX_INLINE ? "I" : "B"),
            box->node && box->node->tag ? box->node->tag : "#text", box->border_box.x,
            box->border_box.y, box->border_box.w, box->border_box.h, box->content_box.x,
            box->content_box.y, box->content_box.w, box->content_box.h, box->line_count,
            box->style.font_size);
    for (i = 0; i < box->line_count; i++)
        fprintf(stderr, "        line y=%.0f h=%.0f w=%.0f runs=%d\n", box->lines[i].y,
                box->lines[i].height, box->lines[i].width, box->lines[i].run_count);
    for (i = 0; i < box->line_count; i++) {
        int ri;
        for (ri = 0; ri < box->lines[i].run_count; ri++) {
            HpeTextRun *run = &box->lines[i].runs[ri];
            if (run->atomic)
                fprintf(stderr, "          run x=%.1f w=%.1f <atomic>\n", run->x, run->width);
            else if (run->font)
                fprintf(stderr, "          run x=%.1f w=%.1f \"%.*s\" #%02x%02x%02x\n", run->x,
                        run->width, run->len > 24 ? 24 : run->len, run->text, run->color.r,
                        run->color.g, run->color.b);
        }
    }
    for (child = box->first_child; child; child = child->next)
        dump_boxes(child, depth + 1);
}

/* ---------------- scripts ---------------- */

static void collect_script_nodes(HpeDomNode *node, HpeDomNode **out, int max, int *count)
{
    HpeDomNode *child;
    for (child = node->first_child; child; child = child->next_sibling) {
        if (child->type != HPE_NODE_ELEMENT)
            continue;
        if (strcmp(child->tag, "script") == 0 && *count < max)
            out[(*count)++] = child;
        collect_script_nodes(child, out, max, count);
    }
}

/* resolves a module specifier against a base directory and reads the file */
static int hope_module_load(void *ud, const char *spec, const char *from_dir, char **out_path,
                            char **out_src)
{
    char full[1024];
    const char *base = from_dir && *from_dir ? from_dir : (const char *)ud;
    size_t n;

    if (!spec || !*spec)
        return 0;
    /* bare specifiers fall back to the document directory */
    if (spec[0] == '.' || spec[0] == '/')
        hpe_path_join(full, sizeof(full), base ? base : ".", spec);
    else
        hpe_path_join(full, sizeof(full), base ? base : ".", spec);
    /* allow extension-less specifiers */
    n = strlen(full);
    if (n < 3 || strcmp(full + n - 3, ".js") != 0) {
        if (n + 4 < sizeof(full))
            strcpy(full + n, ".js");
    }
    *out_src = hpe_read_file(full, NULL);
    if (!*out_src)
        return 0;
    *out_path = hpe_strdup(full);
    return 1;
}

/* loads one <script>'s source: the `src` file when present, else its inline raw
 * text. Returns a freshly allocated buffer (hpe_free it) or NULL. */
static char *script_source(HpeDomNode *node, const char *dir, const char **is_module)
{
    const char *src_attr = hpe_dom_attr(node, "src");
    const char *type_attr = hpe_dom_attr(node, "type");

    *is_module = type_attr && strcmp(type_attr, "module") == 0 ? "1" : NULL;
    if (src_attr && *src_attr) {
        char full[1024];
        char *code;
        hpe_path_join(full, sizeof(full), dir, src_attr);
        code = hpe_read_file(full, NULL);
        if (!code)
            fprintf(stderr, "hope: cannot open script %s\n", full);
        return code;
    }
    {
        HpeDomNode *text = node->first_child;
        if (text && text->type == HPE_NODE_TEXT && text->text && text->text[0])
            return hpe_strdup(text->text);
    }
    return NULL;
}

/* evaluates one already-collected <script> node in the engine's JS context */
static void run_one_script(HpeEngine *engine, HpeDomNode *node, const char *dir)
{
    const char *is_module = NULL;
    char errbuf[256];
    char *code = script_source(node, dir, &is_module);
    int rc;

    if (!code)
        return;
    errbuf[0] = '\0';
    rc = is_module ? hpe_js_eval_module(engine->js, code, dir, errbuf, sizeof(errbuf))
                   : hpe_js_eval(engine->js, code, errbuf, sizeof(errbuf));
    if (rc != 0)
        fprintf(stderr, "hope: script error: %s\n", errbuf);
    hpe_free(code);
}

static void run_scripts(HpeEngine *engine)
{
    HpeDomNode *scripts[HPE_MAX_SCRIPTS];
    int count = 0;
    int i;
    char dir[1024];
    HpeJsModuleLoader loader;

    if (engine->js) {
        hpe_js_destroy(engine->js);
        engine->js = NULL;
    }
    collect_script_nodes(engine->doc, scripts, HPE_MAX_SCRIPTS, &count);
    if (count == 0)
        return;

    engine->js = hpe_js_create();
    hpe_js_dom_install(engine->js, engine->doc);

    hpe_path_dir(engine->path ? engine->path : ".", dir, sizeof(dir));
    hpe_js_set_current_dir(engine->js, dir);
    loader.load = hope_module_load;
    loader.ud = dir;
    hpe_js_set_module_loader(engine->js, &loader);

    for (i = 0; i < count; i++)
        run_one_script(engine, scripts[i], dir);
}

/* ---------------- layout ---------------- */

static void dump_dom(HpeDomNode *node, int depth)
{
    HpeDomNode *child;
    int i;
    for (child = node->first_child; child; child = child->next_sibling) {
        for (i = 0; i < depth; i++)
            fprintf(stderr, "  ");
        if (child->type == HPE_NODE_ELEMENT) {
            const char *cls = hpe_dom_attr(child, "class");
            fprintf(stderr, "<%s%s%s>\n", child->tag, cls ? " class=" : "", cls ? cls : "");
            dump_dom(child, depth + 1);
        } else if (child->type == HPE_NODE_TEXT) {
            int len = child->text ? (int)strlen(child->text) : 0;
            fprintf(stderr, "\"%.40s\" (%d bytes)\n", child->text ? child->text : "", len);
        }
    }
}

void hpe_engine_relayout(HpeEngine *engine)
{
    /* the eager entry point (load, resize, hover, timers, dev hooks): it always
     * rebuilds. The lazy policy lives in hpe_engine_flush(), used by the event
     * paths that can prove a mutation is paint-only or absent. */
    hpe_engine_flush(engine, 1);
}

/* ---------------- public api ---------------- */

HpeEngine *hpe_engine_create(int width, int height)
{
    HpeEngine *engine = (HpeEngine *)hpe_calloc(1, sizeof(HpeEngine));
    engine->width = width > 0 ? width : 800;
    engine->height = height > 0 ? height : 600;
    hpe_pixmap_init(&engine->pm, engine->width, engine->height);
    snprintf(engine->status, sizeof(engine->status), "hope: ready");
    return engine;
}

void hpe_engine_destroy(HpeEngine *engine)
{
    if (!engine)
        return;
    hpe_dom_clear_mutation_sinks();
    if (engine->root)
        hpe_layout_free(engine->root);
    if (engine->doc)
        hpe_dom_free(engine->doc);
    hpe_css_free(&engine->sheet);
    hpe_pixmap_free(&engine->pm);
    if (engine->js) {
        hpe_js_destroy(engine->js);
        engine->js = NULL;
    }
    if (engine->path)
        hpe_free(engine->path);
    hpe_font_shutdown();
    hpe_free(engine);
}

/* tears down everything the previous document produced, ready for a new load */
static void reset_document(HpeEngine *engine, const char *path)
{
    if (engine->path) {
        hpe_free(engine->path);
        engine->path = NULL;
    }
    if (path)
        engine->path = hpe_strdup(path);
    if (engine->root) {
        hpe_layout_free(engine->root);
        engine->root = NULL;
    }
    if (engine->doc) {
        hpe_dom_free(engine->doc);
        engine->doc = NULL;
    }
    hpe_css_free(&engine->sheet);
    hpe_css_init(&engine->sheet); /* user agent styles */
}

/* decodes the raw bytes to utf-8, parses the tree, and resolves <base href>
 * before the author stylesheet is collected (so relative refs rebase correctly) */
static void parse_document(HpeEngine *engine, char *src, size_t size, const char *path)
{
    char dir[HPE_MAX_PATH];
    char *utf8;
    size_t declen = 0;
    HpeDocEncoding enc = HPE_ENC_UTF8;
    int from_meta = 0;
    const char *base;

    utf8 = hpe_html_decode(src, size, &declen, &enc, &from_meta);
    hpe_free(src);
    snprintf(engine->doc_encoding, sizeof(engine->doc_encoding), "%s", hpe_encoding_name(enc));
    engine->meta_charset = from_meta;
    engine->link_count = 0;
    engine->base_href[0] = '\0';

    engine->doc = hpe_html_parse(utf8, declen);
    hpe_free(utf8);

    hpe_path_dir(path ? path : ".", dir, sizeof(dir));
    base = find_base_href(engine->doc);
    if (base) {
        if (hpe_path_is_absolute(base))
            snprintf(engine->base_href, sizeof(engine->base_href), "%s", base);
        else
            hpe_path_join(engine->base_href, sizeof(engine->base_href), dir, base);
    }
}

/* collects author rules and installs the DOM sink that keeps them in sync with
 * later <style>/<link> edits made by page scripts */
static void install_styles(HpeEngine *engine, const char *path)
{
    char dir[HPE_MAX_PATH];

    hpe_path_dir(path ? path : ".", dir, sizeof(dir));
    collect_styles(engine, engine->doc, dir);
    engine->author_rule_base = engine->sheet.rule_count;
    engine->style_sig = NULL;
    engine->style_rev = g_style_rev;
    g_style_dirty = 0;
    g_style_sig = NULL;
    hpe_dom_add_mutation_sink(style_sink, engine);
}

/* HOPE_DEBUG: dumps the DOM tree and the first rules of the cascade */
static void debug_dump_document(HpeEngine *engine)
{
    int ri;
    if (!getenv("HOPE_DEBUG"))
        return;
    dump_dom(engine->doc, 0);
    fprintf(stderr, "hope: %d css rules after UA+author\n", engine->sheet.rule_count);
    for (ri = 0; ri < engine->sheet.rule_count && ri < 40; ri++) {
        HpeRule *r = &engine->sheet.rules[ri];
        int si, di;
        fprintf(stderr, "  rule %d: %d selectors, %d decls, spec=%d\n", ri, r->selector_count,
                r->decl_count, r->selector_count ? r->selectors[0].specificity : -1);
        for (si = 0; si < r->selector_count; si++) {
            HpeSelector *s = &r->selectors[si];
            int pi;
            fprintf(stderr, "    sel:");
            for (pi = 0; pi < s->count; pi++) {
                HpeCompound *c = &s->parts[pi];
                fprintf(stderr, " [%s%s%s cls=%d]", c->tag ? c->tag : "*", c->id ? "#" : "",
                        c->id ? c->id : "", c->class_count);
                if (c->class_count)
                    fprintf(stderr, "(.%s)", c->classes[0]);
            }
            fprintf(stderr, "\n");
        }
        for (di = 0; di < r->decl_count; di++)
            fprintf(stderr, "    decl set=0x%llx font=%.1f\n",
                    (unsigned long long)r->decls[di].set, r->decls[di].font_size);
    }
}

/* the synthetic-event dev hooks (HOPE_TEST_CLICK / HOVER / WAIT / TEXT), run
 * once after layout so headless --dump can verify interactive pages */
static void run_test_hooks(HpeEngine *engine)
{
    const char *test_click = getenv("HOPE_TEST_CLICK");
    const char *test_hover = getenv("HOPE_TEST_HOVER");
    const char *test_wait = getenv("HOPE_TEST_WAIT");
    const char *test_text = getenv("HOPE_TEST_TEXT");

    if (test_click && *test_click)
        click_element_by_id(engine, test_click);
    if (test_hover && *test_hover) {
        int hx, hy;
        if (sscanf(test_hover, "%d,%d", &hx, &hy) == 2)
            hpe_engine_mouse_move(engine, hx, hy);
    }
    if (test_wait && *test_wait) {
        double wait = atof(test_wait);
        double t0 = hpe_time_seconds();
        while (hpe_time_seconds() - t0 < wait)
            hpe_engine_tick(engine);
    }
    if (test_text && *test_text)
        print_text_by_id(engine, test_text);
}

/* shared load pipeline: takes ownership of `src`; `path` may be NULL for the
 * built-in blank page (relative resources then resolve against "."). `src` is a
 * raw byte buffer whose charset is detected here (BOM / <meta charset> / utf-8
 * validation), then decoded to utf-8 before parsing. */
static int load_source(HpeEngine *engine, char *src, size_t size, const char *path)
{
    double t0 = hpe_time_seconds();

    engine->error[0] = '\0'; /* a new document clears the previous load error */
    reset_document(engine, path);
    parse_document(engine, src, size, path);
    install_styles(engine, path);
    debug_dump_document(engine);

    engine->scroll_y = 0.0f;
    /* lay out once before running page scripts: they read clientWidth /
     * clientHeight (which force a layout in browsers) to size their output.
     * The second layout below picks up whatever the script changed. */
    hpe_engine_relayout(engine);
    run_scripts(engine);
    hpe_engine_relayout(engine);
    run_test_hooks(engine);

    if (getenv("HOPE_DEBUG") && engine->root)
        dump_boxes(engine->root, 0);
    engine->load_ms = (hpe_time_seconds() - t0) * 1000.0;
    hpe_engine_refresh_status(engine);
    return engine->root != NULL;
}

int hpe_engine_load_file(HpeEngine *engine, const char *path)
{
    char *src;
    size_t size = 0;
    char abs[2048];

    if (!engine || !path)
        return 0;

    src = hpe_read_file(path, &size);
    if (!src) {
        snprintf(engine->error, sizeof(engine->error), "cannot open %s", path);
        snprintf(engine->status, sizeof(engine->status), "hope: cannot open %s", path);
        return 0;
    }
    if (!load_source(engine, src, size, path))
        return 0;
    if (engine->chrome_h > 0) {
        hpe_path_absolute(abs, sizeof(abs), path);
        hpe_chrome_set_text(engine, abs);
    }
    return 1;
}

int hpe_engine_load_blank(HpeEngine *engine)
{
    char *src;
    if (!engine)
        return 0;
    src = hpe_strdup(BLANK_PAGE);
    if (!src)
        return 0;
    if (!load_source(engine, src, strlen(BLANK_PAGE), NULL))
        return 0;
    if (engine->chrome_h > 0) {
        hpe_chrome_set_text(engine, "");
        engine->addr_focus = 0;
    }
    return 1;
}

int hpe_engine_reload(HpeEngine *engine)
{
    if (!engine)
        return 0;
    if (!engine->path)
        return hpe_engine_load_blank(engine);
    return hpe_engine_load_file(engine, engine->path);
}

void hpe_engine_resize(HpeEngine *engine, int width, int height)
{
    if (!engine || width <= 0 || height <= 0)
        return;
    if (width == engine->width && height == engine->height)
        return;
    engine->width = width;
    engine->height = height;
    hpe_pixmap_resize(&engine->pm, width, height);
    hpe_engine_relayout(engine);
    hpe_engine_refresh_status(engine);
}

void hpe_engine_scroll_by(HpeEngine *engine, float lines)
{
    float max_scroll;
    if (!engine)
        return;
    max_scroll =
        HPE_MAX(0.0f, engine->content_height - (float)hpe_engine_view_height(engine));
    engine->scroll_y += lines * HPE_SCROLL_LINE_PX;
    if (engine->scroll_y < 0.0f)
        engine->scroll_y = 0.0f;
    if (engine->scroll_y > max_scroll)
        engine->scroll_y = max_scroll;
    /* the renderer is pixel-based: snapping the offset to whole pixels keeps the
     * damage-rect path byte-identical to a full repaint at any scroll position
     * (an un-snapped 204.4px offset would round differently per path) */
    engine->scroll_y = floorf(engine->scroll_y + 0.5f);
    if (engine->dirty == HPE_DIRTY_NONE)
        engine->dirty = HPE_DIRTY_SCROLL;
    hpe_engine_refresh_status(engine);
}

/* HPE_NO_DIRTY=1 bypasses damage-rect scrolling (benchmark aid) */
static int dirty_disabled(void)
{
    static int flag = -1;
    if (flag < 0)
        flag = getenv("HPE_NO_DIRTY") != NULL;
    return flag;
}

static void collect_fixed_boxes(HpeBox *box, HpeBox **out, int max, int *count)
{
    HpeBox *child;
    for (child = box->first_child; child; child = child->next) {
        if (child->style.position == HPE_POS_FIXED && *count < max)
            out[(*count)++] = child;
        collect_fixed_boxes(child, out, max, count);
    }
}

/* scroll-only repaint: shift the existing pixels, then repaint the exposed
 * strip and every fixed-position box region (fixed boxes do not move). */
static void render_scrolled(HpeEngine *engine)
{
    HpeColor bg = hpe_rgb(255, 255, 255);
    float dy = engine->scroll_y - engine->painted_scroll;
    int dyi = (int)(dy >= 0.0f ? dy + 0.5f : dy - 0.5f);
    int origin_y = engine->chrome_h;
    int view_h = hpe_engine_view_height(engine);
    HpeBox *fixed[64];
    int fc = 0, i;

    if (dyi == 0)
        return; /* sub-pixel remainder: keep accumulating for next time */
    if (dyi >= view_h || dyi <= -view_h) {
        hpe_paint_document(&engine->pm, engine->root, engine->scroll_y, bg, (float)origin_y);
        hpe_chrome_paint(engine);
        engine->painted_scroll = engine->scroll_y;
        return;
    }

    /* only the page viewport scrolls: the chrome strip stays pinned */
    hpe_pixmap_shift_v_rect(&engine->pm, dyi, origin_y, engine->height);
    engine->painted_scroll += (float)dyi;

    if (dyi > 0) {
        int strip_y = engine->height - dyi;
        if (strip_y < origin_y)
            strip_y = origin_y;
        hpe_paint_region(&engine->pm, engine->root, engine->painted_scroll, bg,
                         hpe_rect(0.0f, (float)strip_y, (float)engine->width,
                                  (float)(engine->height - strip_y)),
                         (float)origin_y);
    } else {
        hpe_paint_region(&engine->pm, engine->root, engine->painted_scroll, bg,
                         hpe_rect(0.0f, (float)origin_y, (float)engine->width,
                                  (float)(-dyi)),
                         (float)origin_y);
    }

    /* fixed boxes stay glued to the viewport: repaint their regions over the
     * shifted content (the region repaint replays the whole tree clipped) */
    collect_fixed_boxes(engine->root, fixed, (int)(sizeof(fixed) / sizeof(fixed[0])), &fc);
    for (i = 0; i < fc; i++) {
        HpeRect clip = fixed[i]->border_box;
        clip.y += (float)origin_y;
        hpe_paint_region(&engine->pm, engine->root, engine->painted_scroll, bg, clip,
                         (float)origin_y);
    }
}

void hpe_engine_render(HpeEngine *engine)
{
    double t0;
    if (!engine)
        return;
    /* nothing changed since the last frame: skip the whole paint (this is what
     * makes a DOM mutation that turns out to be a no-op free) */
    if (engine->dirty == HPE_DIRTY_NONE && engine->painted) {
        if (getenv("HOPE_TRACE_REFLOW"))
            fprintf(stderr, "hope: render -> skipped (no damage)\n");
        return;
    }
    t0 = hpe_time_seconds();
    if (engine->dirty == HPE_DIRTY_CHROME && engine->painted)
        hpe_chrome_paint(engine);
    else if (engine->dirty == HPE_DIRTY_SCROLL && engine->root && !dirty_disabled())
        render_scrolled(engine);
    else {
        hpe_paint_document(&engine->pm, engine->root, engine->scroll_y, hpe_rgb(255, 255, 255),
                           (float)engine->chrome_h);
        hpe_chrome_paint(engine);
        engine->painted_scroll = engine->scroll_y;
        engine->painted = 1;
    }
    engine->dirty = HPE_DIRTY_NONE;
    engine->render_ms = (hpe_time_seconds() - t0) * 1000.0;
    hpe_engine_refresh_status(engine);
}

const HpePixmap *hpe_engine_pixmap(HpeEngine *engine)
{
    return engine ? &engine->pm : NULL;
}

const char *hpe_engine_status(HpeEngine *engine)
{
    return engine ? engine->status : "";
}

float hpe_engine_scroll_offset(HpeEngine *engine)
{
    return engine ? engine->scroll_y : 0.0f;
}

/* ---------------- document metadata ---------------- */

const char *hpe_engine_document_encoding(HpeEngine *engine)
{
    return engine ? engine->doc_encoding : "";
}

int hpe_engine_meta_charset(HpeEngine *engine)
{
    return engine ? engine->meta_charset : 0;
}

const char *hpe_engine_base_href(HpeEngine *engine)
{
    return engine ? engine->base_href : "";
}

int hpe_engine_link_count(HpeEngine *engine)
{
    return engine ? engine->link_count : 0;
}

int hpe_engine_link_at(HpeEngine *engine, int index, const char **rel, const char **href)
{
    if (!engine || index < 0 || index >= engine->link_count)
        return 0;
    if (rel)
        *rel = engine->links[index].rel;
    if (href)
        *href = engine->links[index].href;
    return 1;
}

/* ---------------- hit testing & events ---------------- */

/* x, y are viewport coords; fixed boxes live in viewport coordinates while
 * normal-flow boxes live in document coordinates (offset by scroll). */
static HpeBox *hit_test_box(HpeBox *box, float x, float y_vp, float scroll)
{
    HpeBox *child;
    float ytest;
    if (!box)
        return NULL;
    ytest = box->style.position == HPE_POS_FIXED ? y_vp : y_vp + scroll;
    if (x < box->border_box.x || ytest < box->border_box.y ||
        x > box->border_box.x + box->border_box.w ||
        ytest > box->border_box.y + box->border_box.h)
        return NULL;
    for (child = box->first_child; child; child = child->next) {
        HpeBox *r = hit_test_box(child, x, y_vp, scroll);
        if (r)
            return r; /* deepest match wins */
    }
    return box;
}

static HpeDomNode *node_from_hit(HpeEngine *engine, int x, int y)
{
    HpeBox *hit;
    HpeDomNode *node;
    /* mouse coords are window coords: the page viewport starts below the chrome */
    int cy = y - engine->chrome_h;
    if (cy < 0)
        return NULL;
    hit = hit_test_box(engine->root, (float)x, (float)cy, engine->scroll_y);
    node = hit ? hit->node : NULL;
    /* an <svg> has no boxes for its children, so the deepest box is the element
     * itself: ask the SVG module which shape is under the pointer. */
    if (node && node->tag && !strcmp(node->tag, "svg")) {
        HpeDomNode *shape = hpe_svg_hit_test(hit, (float)x, (float)cy + engine->scroll_y);
        if (shape)
            node = shape;
    }
    while (node && node->type != HPE_NODE_ELEMENT)
        node = node->parent;
    return node;
}

static void clear_hover_flags(HpeDomNode *node)
{
    for (; node; node = node->next_sibling) {
        node->hovered = 0;
        if (node->first_child)
            clear_hover_flags(node->first_child);
    }
}

int hpe_engine_mouse_move(HpeEngine *engine, int x, int y)
{
    HpeDomNode *node;
    int hover_changed, fired = 0;

    if (!engine || !engine->root)
        return 0;
    /* a drag that started inside the address bar keeps receiving the moves, even
     * once the pointer leaves the strip */
    if (hpe_chrome_mouse_move(engine, x, y))
        return 1;
    node = node_from_hit(engine, x, y); /* NULL inside the chrome strip */

    hover_changed = node != engine->hover_node;
    if (hover_changed) {
        clear_hover_flags(engine->doc);
        if (node)
            node->hovered = 1;
        engine->hover_node = node;
    }

    if (engine->js && node)
        fired = hpe_js_dom_dispatch(engine->js, node, "mousemove", x, y - engine->chrome_h);

    /* a hover change always needs a fresh cascade (":hover" is a selector the
     * style resolver evaluates), so force; handlers may also have edited the DOM
     * -- the flush picks those up at whatever depth the sink recorded. */
    if (hover_changed) {
        hpe_engine_relayout(engine);
        hpe_engine_refresh_status(engine);
    } else if (fired && engine->reflow_need != HPE_REFLOW_NONE) {
        hpe_engine_flush(engine, 0);
        hpe_engine_refresh_status(engine);
    }
    return hover_changed || fired;
}

int hpe_engine_tick(HpeEngine *engine)
{
    int fired = 0;
    if (!engine || !engine->js)
        return 0;
    hpe_js_run_timers(engine->js, hpe_time_seconds(), &fired);
    if (fired) {
        hpe_engine_flush(engine, 0); /* only relayout if a timer touched the DOM */
        hpe_engine_refresh_status(engine);
    }
    return fired;
}

int hpe_engine_mouse_event(HpeEngine *engine, int x, int y, int button, int down)
{
    return hpe_engine_mouse_event_mods(engine, x, y, button, down, 0);
}

int hpe_engine_mouse_event_mods(HpeEngine *engine, int x, int y, int button, int down, int mods)
{
    HpeDomNode *node;
    int fired = 0;
    int blurred = 0;
    int py;

    HPE_UNUSED(button);
    if (!engine || !engine->root)
        return 0;

    /* clicks inside the chrome strip focus the address bar instead of the page */
    if (engine->chrome_h > 0 && (y < engine->chrome_h || engine->addr_drag))
        return hpe_chrome_mouse(engine, x, y, down, mods);
    if (engine->chrome_h > 0 && engine->addr_focus) {
        engine->addr_focus = 0; /* clicking the page leaves the address bar */
        if (engine->dirty == HPE_DIRTY_NONE)
            engine->dirty = HPE_DIRTY_CHROME;
        blurred = 1;
    }
    if (!engine->js)
        return blurred;

    node = node_from_hit(engine, x, y);
    if (!node)
        return blurred;
    py = y - engine->chrome_h;

    if (down)
        fired = hpe_js_dom_dispatch(engine->js, node, "mousedown", x, py);
    else {
        if (hpe_js_dom_dispatch(engine->js, node, "mouseup", x, py))
            fired = 1;
        if (hpe_js_dom_dispatch(engine->js, node, "click", x, py))
            fired = 1;
    }

    if (fired) {
        /* the handlers may have changed the DOM; only do the work their
         * mutations call for (a click that toggles a colour skips layout) */
        hpe_engine_flush(engine, 0);
        hpe_engine_refresh_status(engine);
    }
    return fired || blurred;
}

/* dev/test hook: fire a click on the element with the given id (used by HOPE_TEST_CLICK).
 * Exercises the real event-dispatch path without a window. */
static int click_element_by_id(HpeEngine *engine, const char *id)
{
    HpeDomNode **queue;
    int qh = 0, qt = 0, qcap = 1024;
    HpeDomNode *found = NULL;
    HpeDomNode *n = engine->doc;

    if (!engine->js || !id || !*id)
        return 0;
    queue = (HpeDomNode **)malloc(sizeof(HpeDomNode *) * (size_t)qcap);
    if (!queue)
        return 0;
    queue[qt++] = n;
    while (qh < qt) {
        HpeDomNode *cur = queue[qh++];
        HpeDomNode *c;
        for (c = cur->first_child; c; c = c->next_sibling) {
            if (c->type == HPE_NODE_ELEMENT) {
                const char *aid = hpe_dom_attr(c, "id");
                if (aid && strcmp(aid, id) == 0) {
                    found = c;
                    break;
                }
                if (qt < qcap)
                    queue[qt++] = c;
            }
        }
        if (found)
            break;
    }
    free(queue);
    if (!found)
        return 0;
    hpe_js_dom_dispatch(engine->js, found, "mousedown", 0, 0);
    hpe_js_dom_dispatch(engine->js, found, "mouseup", 0, 0);
    hpe_js_dom_dispatch(engine->js, found, "click", 0, 0);
    hpe_engine_flush(engine, 0);
    hpe_engine_refresh_status(engine);
    return 1;
}

/* dev/test hook helper: print the concatenated text content of the element with
 * the given id to stdout (used by HOPE_TEST_TEXT), so headless runs can assert
 * on DOM updates made by scripts. */
static void print_text_by_id(HpeEngine *engine, const char *id)
{
    HpeDomNode *stack[256];
    int sp = 0;
    HpeDomNode *n = engine->doc;

    if (!id || !*id)
        return;
    stack[sp++] = n;
    while (sp > 0) {
        HpeDomNode *cur = stack[--sp];
        HpeDomNode *c;
        if (cur->type == HPE_NODE_ELEMENT) {
            const char *aid = hpe_dom_attr(cur, "id");
            if (aid && strcmp(aid, id) == 0) {
                /* found: print own subtree text */
                HpeDomNode *tstack[256];
                int tsp = 0;
                tstack[tsp++] = cur;
                while (tsp > 0) {
                    HpeDomNode *t = tstack[--tsp];
                    HpeDomNode *tc;
                    if (t->type == HPE_NODE_TEXT && t->text)
                        fprintf(stdout, "%s", t->text);
                    for (tc = t->first_child; tc; tc = tc->next_sibling)
                        if (tsp < 256)
                            tstack[tsp++] = tc;
                }
                fprintf(stdout, "\n");
                return;
            }
        }
        for (c = cur->first_child; c; c = c->next_sibling)
            if (sp < 256)
                stack[sp++] = c;
    }
    fprintf(stdout, "<no element id=%s>\n", id);
}
