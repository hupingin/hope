/* Hope UI Engine - form controls: engine-side semantics (see hope_form.h) */
#include "hope_form.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hpe_util.h"
#include "js/hpe_js_dom.h"
#include "layout/hpe_layout.h"

/* reserved attribute names holding the reset snapshots */
#define DEFVAL_ATTR "data-defval"
#define DEFCHK_ATTR "data-defchk"
#define DEFSEL_ATTR "data-defsel"

/* ---------------- classification ---------------- */

/**
 * @brief 读 input 的 type 属性并归类 / Read an input's `type` attribute and classify it
 */
static HpeFormKind input_kind(const HpeDomNode *node)
{
    const char *t = hpe_dom_attr(node, "type");
    if (!t || !*t || !strcmp(t, "text") || !strcmp(t, "search") || !strcmp(t, "email") ||
        !strcmp(t, "url") || !strcmp(t, "tel") || !strcmp(t, "number") ||
        !strcmp(t, "date") || !strcmp(t, "time") || !strcmp(t, "datetime-local"))
        return HPE_FK_FIELD;
    if (!strcmp(t, "password"))
        return HPE_FK_PASSWORD;
    if (!strcmp(t, "checkbox"))
        return HPE_FK_CHECKBOX;
    if (!strcmp(t, "radio"))
        return HPE_FK_RADIO;
    if (!strcmp(t, "range"))
        return HPE_FK_RANGE;
    if (!strcmp(t, "color"))
        return HPE_FK_COLOR;
    if (!strcmp(t, "file"))
        return HPE_FK_FILE;
    return HPE_FK_BUTTON; /* button / submit / reset / image */
}

HpeFormKind hpe_form_kind(const HpeDomNode *node)
{
    const char *tag;
    if (!node || node->type != HPE_NODE_ELEMENT || !node->tag)
        return HPE_FK_OTHER;
    tag = node->tag;
    if (!strcmp(tag, "input"))
        return input_kind(node);
    if (!strcmp(tag, "textarea"))
        return HPE_FK_TEXTAREA;
    if (!strcmp(tag, "select"))
        return HPE_FK_SELECT;
    if (!strcmp(tag, "button"))
        return HPE_FK_BUTTON;
    if (!strcmp(tag, "progress"))
        return HPE_FK_PROGRESS;
    if (!strcmp(tag, "meter"))
        return HPE_FK_METER;
    if (!strcmp(tag, "output"))
        return HPE_FK_OUTPUT;
    return HPE_FK_OTHER;
}

/**
 * @brief 判断控件是否可编辑文本 / Test whether a control accepts text editing
 */
static int kind_is_editable(HpeFormKind k)
{
    return k == HPE_FK_FIELD || k == HPE_FK_PASSWORD || k == HPE_FK_TEXTAREA;
}

/**
 * @brief 取 select 当前选中的 option / Take a select's currently selected option
 */
static HpeDomNode *selected_option(HpeDomNode *node)
{
    HpeDomNode *c;
    for (c = node->first_child; c; c = c->next_sibling) {
        if (c->type == HPE_NODE_ELEMENT && c->tag && !strcmp(c->tag, "option") &&
            hpe_dom_attr(c, "selected"))
            return c;
    }
    return NULL;
}

/**
 * @brief 把节点子文本拼进缓冲 / Append a node's child text into a buffer
 */
static void append_child_text(HpeDomNode *node, char *out, int cap, int *len)
{
    HpeDomNode *c;
    for (c = node->first_child; c; c = c->next_sibling) {
        if (c->type != HPE_NODE_TEXT || !c->text)
            continue;
        *len += snprintf(out + *len, (size_t)(cap - *len > 0 ? cap - *len : 0), "%s", c->text);
        if (*len >= cap)
            break;
    }
}

/* ---------------- value model ---------------- */

void hpe_form_value_get(const HpeDomNode *node, char *out, int cap)
{
    HpeDomNode *opt;
    const char *v;
    HpeDomNode *mut;
    int len = 0;

    if (cap <= 0)
        return;
    out[0] = '\0';
    if (!node)
        return;
    switch (hpe_form_kind(node)) {
    case HPE_FK_FIELD:
    case HPE_FK_PASSWORD:
    case HPE_FK_RANGE:
    case HPE_FK_COLOR:
    case HPE_FK_FILE:
    case HPE_FK_BUTTON:
        v = hpe_dom_attr(node, "value");
        snprintf(out, (size_t)cap, "%s", v ? v : "");
        return;
    case HPE_FK_SELECT:
        opt = selected_option((HpeDomNode *)node);
        if (!opt)
            return;
        v = hpe_dom_attr(opt, "value");
        if (v)
            snprintf(out, (size_t)cap, "%s", v);
        else
            append_child_text(opt, out, cap, &len);
        return;
    case HPE_FK_TEXTAREA:
    case HPE_FK_OUTPUT:
    case HPE_FK_PROGRESS:
    case HPE_FK_METER:
    default:
        mut = (HpeDomNode *)node;
        append_child_text(mut, out, cap, &len);
        return;
    }
}

/**
 * @brief 清空元素全部子节点 / Remove all of an element's child nodes
 */
static void remove_children(HpeDomNode *node)
{
    while (node->first_child) {
        HpeDomNode *c = node->first_child;
        node->first_child = c->next_sibling;
        hpe_dom_free(c);
    }
    node->last_child = NULL;
}

/**
 * @brief 把 select 的选中标记移到指定 option / Move a select's `selected` mark onto one option
 */
static void select_pick(HpeDomNode *select, HpeDomNode *opt)
{
    HpeDomNode *c;
    for (c = select->first_child; c; c = c->next_sibling) {
        if (c->type != HPE_NODE_ELEMENT || !c->tag || strcmp(c->tag, "option"))
            continue;
        if (c == opt)
            hpe_dom_set_attr(c, "selected", "");
        else
            hpe_dom_remove_attr(c, "selected");
    }
}

void hpe_form_value_set(HpeDomNode *node, const char *v)
{
    HpeDomNode *c;
    if (!node)
        return;
    switch (hpe_form_kind(node)) {
    case HPE_FK_FIELD:
    case HPE_FK_PASSWORD:
    case HPE_FK_RANGE:
    case HPE_FK_COLOR:
    case HPE_FK_FILE:
    case HPE_FK_BUTTON:
        hpe_dom_set_attr(node, "value", v ? v : "");
        return;
    case HPE_FK_SELECT:
        /* match by `value` first, then by option text */
        for (c = node->first_child; c; c = c->next_sibling) {
            const char *ov;
            if (c->type != HPE_NODE_ELEMENT || !c->tag || strcmp(c->tag, "option"))
                continue;
            ov = hpe_dom_attr(c, "value");
            if ((ov && !strcmp(ov, v)) || (!ov && c->text && !strcmp(c->text, v))) {
                select_pick(node, c);
                return;
            }
        }
        return;
    default:
        remove_children(node);
        hpe_dom_append(node, hpe_dom_text(v ? v : ""));
        return;
    }
}

int hpe_form_checked_get(const HpeDomNode *node)
{
    return node && hpe_dom_attr(node, "checked") != NULL;
}

void hpe_form_checked_set(HpeDomNode *node, int on)
{
    if (!node)
        return;
    if (on)
        hpe_dom_set_attr(node, "checked", "");
    else
        hpe_dom_remove_attr(node, "checked");
}

HpeDomNode *hpe_form_owner(const HpeDomNode *node)
{
    const HpeDomNode *n;
    for (n = node ? node->parent : NULL; n; n = n->parent) {
        if (n->type == HPE_NODE_ELEMENT && n->tag && !strcmp(n->tag, "form"))
            return (HpeDomNode *)n;
    }
    return NULL;
}

/* ---------------- default snapshots & reset ---------------- */

/**
 * @brief 为单个控件记录默认值快照 / Record the default snapshot of one control
 */
static void record_one_default(HpeDomNode *n)
{
    HpeFormKind k = hpe_form_kind(n);
    char buf[512];

    if (k == HPE_FK_OTHER || hpe_dom_attr(n, DEFVAL_ATTR))
        return;
    hpe_form_value_get(n, buf, (int)sizeof(buf));
    hpe_dom_set_attr(n, DEFVAL_ATTR, buf);
    if (k == HPE_FK_CHECKBOX || k == HPE_FK_RADIO)
        hpe_dom_set_attr(n, DEFCHK_ATTR, hpe_form_checked_get(n) ? "1" : "0");
    /* every option remembers its default selectedness for reset */
    if (k == HPE_FK_SELECT) {
        HpeDomNode *c;
        for (c = n->first_child; c; c = c->next_sibling) {
            if (c->type != HPE_NODE_ELEMENT || !c->tag || strcmp(c->tag, "option"))
                continue;
            if (!hpe_dom_attr(c, DEFSEL_ATTR))
                hpe_dom_set_attr(c, DEFSEL_ATTR, hpe_dom_attr(c, "selected") ? "1" : "0");
        }
    }
}

/**
 * @brief 递归遍历兄弟链记录默认值 / Walk a sibling chain recording defaults
 */
static void record_walk(HpeDomNode *n)
{
    while (n) {
        record_one_default(n);
        if (n->first_child)
            record_walk(n->first_child);
        n = n->next_sibling;
    }
}

void hpe_form_record_defaults(HpeDomNode *doc)
{
    if (doc)
        record_walk(doc);
}

/**
 * @brief 把单个控件恢复到默认值 / Restore one control to its default value
 */
static void reset_one(HpeDomNode *n)
{
    HpeFormKind k = hpe_form_kind(n);
    const char *defval;
    const char *defchk;
    const char *defsel;

    if (k == HPE_FK_OTHER)
        return;
    defval = hpe_dom_attr(n, DEFVAL_ATTR);
    hpe_form_value_set(n, defval ? defval : "");
    defchk = hpe_dom_attr(n, DEFCHK_ATTR);
    if (defchk)
        hpe_form_checked_set(n, defchk[0] == '1');
    if (k == HPE_FK_SELECT) {
        HpeDomNode *c;
        for (c = n->first_child; c; c = c->next_sibling) {
            if (c->type != HPE_NODE_ELEMENT || !c->tag || strcmp(c->tag, "option"))
                continue;
            defsel = hpe_dom_attr(c, DEFSEL_ATTR);
            if (defsel && defsel[0] == '1')
                hpe_dom_set_attr(c, "selected", "");
            else
                hpe_dom_remove_attr(c, "selected");
        }
    }
}

/**
 * @brief 在子树内重置所有控件（可选过滤到某个 form）/ Reset every control in a subtree (optionally filtered to one form)
 */
static void reset_walk(HpeDomNode *n, HpeDomNode *form)
{
    while (n) {
        if (hpe_form_kind(n) != HPE_FK_OTHER && (!form || hpe_form_owner(n) == form))
            reset_one(n);
        if (n->first_child)
            reset_walk(n->first_child, form);
        n = n->next_sibling;
    }
}

void hpe_form_reset(HpeDomNode *doc, HpeDomNode *form)
{
    if (doc)
        reset_walk(doc->first_child ? doc : doc->first_child, form);
}

/* ---------------- event plumbing ---------------- */

/**
 * @brief 派发一个事件并回报是否未被取消 / Dispatch an event and report whether it stayed uncancelled
 */
static int fire_event(HpeEngine *engine, HpeDomNode *node, const char *type, int *prevented)
{
    HpeJsEventInit init;
    if (!engine || !engine->js || !node)
        return 0;
    memset(&init, 0, sizeof(init));
    return hpe_js_dom_dispatch_cancelable(engine->js, node, type, &init, prevented);
}

/**
 * @brief 派发 `input` + `change`（状态型控件的点击）/ Dispatch `input` plus `change` (stateful control click)
 */
static void fire_input_change(HpeEngine *engine, HpeDomNode *node)
{
    fire_event(engine, node, "input", NULL);
    fire_event(engine, node, "change", NULL);
}

/**
 * @brief 读浮点属性，缺失/非法时用默认值 / Read a float attribute with a default fallback
 */
static float attr_float(const HpeDomNode *n, const char *name, float dflt)
{
    const char *v = hpe_dom_attr(n, name);
    float f;
    if (!v || !*v || sscanf(v, "%f", &f) != 1)
        return dflt;
    return f;
}

/* ---------------- focus / blur ---------------- */

/**
 * @brief 失焦当前编辑控件（值有变则派发 `change`）/ Blur the currently edited control (`change` when the value changed)
 */
void hpe_form_blur(HpeEngine *engine)
{
    char now[1024];

    if (!engine || !engine->form_focus)
        return;
    if (engine->form_value0) {
        hpe_form_value_get(engine->form_focus, now, (int)sizeof(now));
        if (strcmp(now, engine->form_value0) != 0)
            fire_event(engine, engine->form_focus, "change", NULL);
        hpe_free(engine->form_value0);
        engine->form_value0 = NULL;
    }
    engine->form_focus = NULL;
    engine->form_caret = 0;
    hpe_engine_set_focus(engine, NULL);
}

/**
 * @brief 聚焦一个可编辑控件并记录插入符/快照 / Focus an editable control, recording caret and snapshot
 */
static void focus_text_control(HpeEngine *engine, HpeDomNode *node)
{
    char buf[1024];

    if (engine->form_focus == node)
        return;
    if (engine->form_focus)
        hpe_form_blur(engine);
    hpe_form_value_get(node, buf, (int)sizeof(buf));
    engine->form_focus = node;
    engine->form_caret = (int)strlen(buf);
    engine->form_value0 = hpe_strdup(buf);
    hpe_engine_set_focus(engine, node);
}

/* ---------------- radio groups ---------------- */

/**
 * @brief 遍历兄弟链取消同组其它 radio 的勾选 / Walk a sibling chain unchecking other radios of the group
 */
static void radio_walk_exclusive(HpeDomNode *n, HpeDomNode *keep, const char *name)
{
    while (n) {
        if (n != keep && n->tag && !strcmp(n->tag, "input")) {
            const char *t = hpe_dom_attr(n, "type");
            const char *on = hpe_dom_attr(n, "name");
            if (t && on && !strcmp(t, "radio") && !strcmp(on, name))
                hpe_form_checked_set(n, 0);
        }
        if (n->first_child)
            radio_walk_exclusive(n->first_child, keep, name);
        n = n->next_sibling;
    }
}

/**
 * @brief 在 radio 组内强制互斥（同 name 的兄弟钮取消勾选）/ Enforce radio-group exclusivity among same-name siblings
 */
static void radio_exclusive(HpeDomNode *radio)
{
    const char *name;
    HpeDomNode *scope;

    if (!radio->parent)
        return;
    name = hpe_dom_attr(radio, "name");
    if (!name)
        return;
    /* the group scope is the owning form when there is one, else the document */
    scope = radio;
    while (scope->parent && scope->parent->type == HPE_NODE_ELEMENT)
        scope = scope->parent;
    radio_walk_exclusive(scope, radio, name);
}

/* ---------------- select cycling ---------------- */

/**
 * @brief 求 select 的第一个 option 子节点 / Find a select's first option child
 */
static HpeDomNode *first_option(HpeDomNode *sel)
{
    HpeDomNode *c = sel->first_child;
    while (c && !(c->type == HPE_NODE_ELEMENT && c->tag && !strcmp(c->tag, "option")))
        c = c->next_sibling;
    return c;
}

/**
 * @brief 求下一个 option 兄弟（跳过文本节点）/ Find the next option sibling (skipping text nodes)
 */
static HpeDomNode *next_option(HpeDomNode *opt)
{
    HpeDomNode *c = opt ? opt->next_sibling : NULL;
    while (c && !(c->type == HPE_NODE_ELEMENT && c->tag && !strcmp(c->tag, "option")))
        c = c->next_sibling;
    return c;
}

/**
 * @brief select 点击时循环到下一个 option / Cycle a select to its next option on click
 */
static void select_cycle(HpeEngine *engine, HpeDomNode *sel)
{
    HpeDomNode *cur = selected_option(sel);
    HpeDomNode *first = first_option(sel);
    HpeDomNode *target = next_option(cur);

    /* fell off the end (or no selection yet): wrap to the first option */
    if (!target)
        target = first;
    if (!target)
        return;
    select_pick(sel, target);
    fire_input_change(engine, sel);
}

/* ---------------- button submit / reset ---------------- */

/**
 * @brief 找一个元素（按 id）/ Find an element by its `id` attribute
 */
static HpeDomNode *find_by_id(HpeDomNode *n, const char *id)
{
    while (n) {
        HpeDomNode *hit;
        const char *aid = hpe_dom_attr(n, "id");
        if (aid && !strcmp(aid, id))
            return n;
        if (n->first_child) {
            hit = find_by_id(n->first_child, id);
            if (hit)
                return hit;
        }
        n = n->next_sibling;
    }
    return NULL;
}

/**
 * @brief label 点击转发到它关联的控件 / Forward a label click to the control it labels
 *
 * @return 找到关联控件返回该控件，否则 `NULL` / The labelled control, or `NULL`
 */
static HpeDomNode *label_target(HpeDomNode *label)
{
    const char *forid = hpe_dom_attr(label, "for");
    HpeDomNode *doc = label;
    HpeDomNode *c;

    if (forid && *forid) {
        while (doc->parent)
            doc = doc->parent;
        return find_by_id(doc, forid);
    }
    /* no `for`: the first labelable descendant (input, textarea, select,
     * button, output, progress, meter) is implicit */
    for (c = label->first_child; c; c = c->next_sibling) {
        if (hpe_form_kind(c) != HPE_FK_OTHER)
            return c;
        if (c->first_child) {
            HpeDomNode *deep = label_target(c);
            if (deep)
                return deep;
        }
    }
    return NULL;
}

/**
 * @brief 触发 button 控件的 submit / reset 语义 / Run a button control's submit / reset semantics
 */
static int button_activate(HpeEngine *engine, HpeDomNode *btn)
{
    HpeDomNode *form = hpe_form_owner(btn);
    const char *type = hpe_dom_attr(btn, "type");
    const char *action;
    int prevented = 0;

    if (!form)
        return 0;
    /* a plain `<button>` defaults to submit */
    if (!type || !*type || !strcmp(type, "submit") || !strcmp(type, "button")) {
        if (type && !strcmp(type, "button"))
            return 0;
        fire_event(engine, form, "submit", &prevented);
        if (prevented)
            return 1;
        action = hpe_dom_attr(form, "action");
        if (action && *action)
            hpe_engine_load_file(engine, action);
        return 1;
    }
    if (!strcmp(type, "reset")) {
        fire_event(engine, form, "reset", &prevented);
        if (!prevented)
            hpe_form_reset(engine->doc, form);
        return 1;
    }
    return 0;
}

/* ---------------- range ---------------- */

/**
 * @brief 在盒树中找节点对应的盒 / Find the box generated for a DOM node
 */
static HpeBox *box_for_node(HpeBox *root, const HpeDomNode *node)
{
    while (root) {
        HpeBox *hit;
        if (root->node == node)
            return root;
        if (root->first_child) {
            hit = box_for_node(root->first_child, node);
            if (hit)
                return hit;
        }
        root = root->next;
    }
    return NULL;
}

/**
 * @brief 按 range 落点位置写入取值 / Write a range's value from the click position
 *
 * @return 取值发生变化返回 1 / 1 when the value changed
 */
static int range_from_x(HpeEngine *engine, HpeDomNode *node, int x)
{
    HpeBox *box = box_for_node(engine->root, node);
    float min = attr_float(node, "min", 0.0f);
    float max = attr_float(node, "max", 100.0f);
    float frac, val;
    char buf[32];

    if (!box || box->border_box.w <= 4.0f || max <= min)
        return 0;
    frac = ((float)x - box->border_box.x - 2.0f) / (box->border_box.w - 4.0f);
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;
    val = min + frac * (max - min);
    snprintf(buf, sizeof(buf), "%g", (double)val);
    if (!strcmp(buf, hpe_dom_attr(node, "value") ? hpe_dom_attr(node, "value") : ""))
        return 0;
    hpe_dom_set_attr(node, "value", buf);
    return 1;
}

/* ---------------- click ---------------- */

/**
 * @brief 执行可交互控件的类型相关默认行为 / Run an interactive control's kind-specific activation
 *
 * @return 控件状态发生变化返回 1 / 1 when the control's state changed
 */
static int activate_kind(HpeEngine *engine, HpeDomNode *node, int x)
{
    switch (hpe_form_kind(node)) {
    case HPE_FK_CHECKBOX:
        hpe_form_checked_set(node, !hpe_form_checked_get(node));
        fire_input_change(engine, node);
        return 1;
    case HPE_FK_RADIO:
        hpe_form_checked_set(node, 1);
        radio_exclusive(node);
        fire_input_change(engine, node);
        return 1;
    case HPE_FK_RANGE:
        if (!range_from_x(engine, node, x))
            return 0;
        fire_input_change(engine, node);
        return 1;
    case HPE_FK_SELECT:
        select_cycle(engine, node);
        return 1;
    case HPE_FK_BUTTON:
        return button_activate(engine, node);
    case HPE_FK_FIELD:
    case HPE_FK_PASSWORD:
    case HPE_FK_TEXTAREA:
        focus_text_control(engine, node);
        return 0;
    default:
        return 0;
    }
}

int hpe_form_click(HpeEngine *engine, HpeDomNode *node, int x, int y)
{
    HpeDomNode *target;

    if (!engine || !node) {
        hpe_form_blur(engine);
        return 0;
    }
    /* a label forwards activation to the control it names */
    if (node->tag && !strcmp(node->tag, "label")) {
        target = label_target(node);
        if (target)
            return hpe_form_click(engine, target, x, y);
        hpe_form_blur(engine);
        return 0;
    }
    if (hpe_form_kind(node) == HPE_FK_OTHER) {
        hpe_form_blur(engine); /* clicking elsewhere blurs the edited control */
        return 0;
    }
    if (hpe_dom_attr(node, "disabled"))
        return 0; /* a disabled control takes no part in interaction */
    HPE_UNUSED(y);
    return activate_kind(engine, node, x);
}

/* ---------------- keyboard editing ---------------- */

/**
 * @brief 求上一个 UTF-8 字符的起始偏移 / Find the start offset of the previous UTF-8 char
 */
static int utf8_prev_off(const char *s, int pos)
{
    while (pos > 0 && ((unsigned char)s[pos - 1] & 0xC0) == 0x80)
        pos--;
    return pos > 0 ? pos - 1 : 0;
}

/**
 * @brief 求下一个 UTF-8 字符的起始偏移 / Find the start offset of the next UTF-8 char
 */
static int utf8_next_off(const char *s, int pos)
{
    uint32_t cp;
    int n;
    if (!s[pos])
        return pos;
    n = hpe_utf8_next(s + pos, &cp);
    return n > 0 ? pos + n : pos + 1;
}

/**
 * @brief 在值的插入符处插入一段文本 / Insert text at the control's caret
 */
static void edit_insert(HpeEngine *engine, char *buf, int cap, const char *ins)
{
    int len = (int)strlen(buf);
    int ilen = (int)strlen(ins);

    if (ilen <= 0 || len + ilen >= cap)
        return;
    memmove(buf + engine->form_caret + ilen, buf + engine->form_caret,
            (size_t)(len - engine->form_caret) + 1);
    memcpy(buf + engine->form_caret, ins, (size_t)ilen);
    engine->form_caret += ilen;
}

int hpe_form_key(HpeEngine *engine, int vk, int printable, int mods)
{
    HpeDomNode *node;
    char buf[1024];
    int changed = 0;

    if (!engine || (mods & HPE_MOD_CTRL) != 0)
        return 0;
    node = engine->form_focus;
    if (!node || !kind_is_editable(hpe_form_kind(node)))
        return 0;
    hpe_form_value_get(node, buf, (int)sizeof(buf));
    if (printable > 0) {
        char ins[8];
        int n = hpe_utf8_encode(printable, ins);
        ins[n] = '\0';
        edit_insert(engine, buf, (int)sizeof(buf), ins);
        changed = 1;
    } else if (vk == HPE_KEY_BACKSPACE) {
        if (engine->form_caret > 0) {
            int prev = utf8_prev_off(buf, engine->form_caret);
            memmove(buf + prev, buf + engine->form_caret,
                    strlen(buf + engine->form_caret) + 1);
            engine->form_caret = prev;
            changed = 1;
        }
    } else if (vk == HPE_KEY_DELETE) {
        int next = utf8_next_off(buf, engine->form_caret);
        if (next > engine->form_caret) {
            memmove(buf + engine->form_caret, buf + next, strlen(buf + next) + 1);
            changed = 1;
        }
    } else if (vk == HPE_KEY_LEFT && engine->form_caret > 0) {
        engine->form_caret = utf8_prev_off(buf, engine->form_caret);
    } else if (vk == HPE_KEY_RIGHT) {
        engine->form_caret = utf8_next_off(buf, engine->form_caret);
    } else if (vk == HPE_KEY_HOME) {
        engine->form_caret = 0;
    } else if (vk == HPE_KEY_END) {
        engine->form_caret = (int)strlen(buf);
    }
    if (changed) {
        hpe_form_value_set(node, buf);
        fire_event(engine, node, "input", NULL);
    }
    return changed;
}

/**
 * @brief JS `focus()`/`blur()` 后同步表单编辑状态 / Sync the form editing state after a JS `focus()`/`blur()`
 */
void hpe_form_focus_notify(HpeEngine *engine, HpeDomNode *node)
{
    if (!engine)
        return;
    if (node && kind_is_editable(hpe_form_kind(node)))
        focus_text_control(engine, node);
    else
        hpe_form_blur(engine);
}

/* ---------------- document load fixup ---------------- */

/**
 * @brief 给无子内容、有 value 的按钮注入标签文本 / Inject label text into value-only buttons
 *
 * @details `<input type=button|submit|reset>` 与无子内容的 `<button>` 以 `value`
 *          属性（或 submit 的默认标签）作为可见标签；注入一个文本子节点让常规
 *          排版/绘制管线渲染它。
 *          `<input type=button|submit|reset>` and a childless `<button>` take their
 *          visible label from the `value` attribute (or the default submit label);
 *          a text child is injected so the ordinary layout / paint pipeline renders it.
 */
static void inject_button_label(HpeDomNode *n)
{
    const char *type;
    const char *value;

    if (n->type != HPE_NODE_ELEMENT || !n->tag)
        return;
    if (!strcmp(n->tag, "button")) {
        type = hpe_dom_attr(n, "type");
        if (type && !strcmp(type, "button") && !n->first_child) {
            value = hpe_dom_attr(n, "value");
            if (value && *value)
                hpe_dom_append(n, hpe_dom_text(value));
        }
        return;
    }
    if (strcmp(n->tag, "input"))
        return;
    type = hpe_dom_attr(n, "type");
    if (!type)
        return;
    if ((!strcmp(type, "button") || !strcmp(type, "submit") || !strcmp(type, "reset")) &&
        !n->first_child) {
        if (!strcmp(type, "submit"))
            hpe_dom_append(n, hpe_dom_text(hpe_dom_attr(n, "value") &&
                                                   hpe_dom_attr(n, "value")[0]
                                               ? hpe_dom_attr(n, "value")
                                               : "Submit"));
        else if (hpe_dom_attr(n, "value") && hpe_dom_attr(n, "value")[0])
            hpe_dom_append(n, hpe_dom_text(hpe_dom_attr(n, "value")));
    }
}

/**
 * @brief 递归遍历兄弟链跑安装步骤 / Walk a sibling chain running the install steps
 */
static void install_walk(HpeDomNode *n)
{
    while (n) {
        inject_button_label(n);
        record_one_default(n);
        if (n->first_child)
            install_walk(n->first_child);
        n = n->next_sibling;
    }
}

void hpe_form_install(HpeEngine *engine)
{
    if (!engine || !engine->doc)
        return;
    install_walk(engine->doc);
}

/**
 * @brief 释放表单编辑状态（引擎销毁时）/ Release the form editing state on engine shutdown
 */
void hpe_form_shutdown(HpeEngine *engine)
{
    if (!engine)
        return;
    if (engine->form_value0) {
        hpe_free(engine->form_value0);
        engine->form_value0 = NULL;
    }
    engine->form_focus = NULL;
    engine->form_caret = 0;
}
