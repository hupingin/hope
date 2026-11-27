/* Hope UI Engine - form controls: engine-side semantics
 *
 * Control classification (tag + `type` attribute), the current-value model
 * (input attribute / textarea text child / select selected option), default
 * snapshots for form reset, click interactions (toggle, radio group, submit /
 * reset, select cycling, label forwarding, range thumb) and keyboard editing
 * of the focused text-like control.  Painting lives in paint/hpe_paint_form.c,
 * the JS bindings in js/hpe_js_dom.c (form hooks + value hooks).
 */
#ifndef HPE_FORM_H
#define HPE_FORM_H

#include "dom/hpe_dom.h"
#include "hope_internal.h"

/**
 * @brief 表单控件类别 / Form control kinds
 */
typedef enum HpeFormKind {
    HPE_FK_OTHER = 0,   /* 非控件 / not a control */
    HPE_FK_FIELD,       /* 单行文本族（text/search/email/url/tel/number/date…）/ one-line text family */
    HPE_FK_PASSWORD,    /* 密码（显示圆点）/ password (dots) */
    HPE_FK_CHECKBOX,    /* 复选框 / checkbox */
    HPE_FK_RADIO,       /* 单选钮 / radio button */
    HPE_FK_RANGE,       /* 滑杆 / range slider */
    HPE_FK_COLOR,       /* 颜色样本 / colour swatch */
    HPE_FK_FILE,        /* 文件选择 / file picker */
    HPE_FK_BUTTON,      /* 按钮（button/submit/reset）/ button family */
    HPE_FK_SELECT,      /* 下拉选择 / dropdown select */
    HPE_FK_TEXTAREA,    /* 多行文本 / multi-line text */
    HPE_FK_PROGRESS,    /* 进度条 / progress bar */
    HPE_FK_METER,       /* 度量条 / meter bar */
    HPE_FK_OUTPUT       /* 输出（只读文本）/ output (read-only text) */
} HpeFormKind;

/**
 * @brief 按标签与 type 属性归类控件 / Classify a control by tag and `type` attribute
 *
 * @param[in] node 目标元素 / Target element
 *
 * @return 控件类别；非控件元素返回 `HPE_FK_OTHER` / The control kind; `HPE_FK_OTHER` when not a control
 */
HpeFormKind hpe_form_kind(const HpeDomNode *node);

/**
 * @brief 读控件当前值 / Read a control's current value
 *
 * @param[in]  node 目标控件 / Target control
 * @param[out] out 接收缓冲 / Receive buffer
 * @param[in]  cap 缓冲容量 / Buffer capacity
 */
void hpe_form_value_get(const HpeDomNode *node, char *out, int cap);

/**
 * @brief 写控件当前值（不派发事件）/ Write a control's current value (no events fired)
 *
 * @param[in,out] node 目标控件 / Target control
 * @param[in]     v 新值 / The new value
 */
void hpe_form_value_set(HpeDomNode *node, const char *v);

/**
 * @brief 读控件勾选状态 / Read a control's checked state
 *
 * @param[in] node 目标控件 / Target control
 *
 * @return `checked` 属性存在返回 1 / 1 when the `checked` attribute is present
 */
int hpe_form_checked_get(const HpeDomNode *node);

/**
 * @brief 写控件勾选状态 / Write a control's checked state
 *
 * @param[in,out] node 目标控件 / Target control
 * @param[in]     on 1 勾选 / 1 to check
 */
void hpe_form_checked_set(HpeDomNode *node, int on);

/**
 * @brief 找控件所属的 `<form>` / Find a control's owning `<form>`
 *
 * @param[in] node 目标控件 / Target control
 *
 * @return 最近的 form 祖先，无则 `NULL` / The nearest form ancestor, or `NULL`
 */
HpeDomNode *hpe_form_owner(const HpeDomNode *node);

/**
 * @brief 文档加载后为全部控件记录默认值快照 / Record default-value snapshots for every control after load
 *
 * @param[in,out] doc 文档节点 / Document node
 *
 * @note 快照存于保留属性 `data-defval` / `data-defchk` / `data-defsel`，
 *       供 reset 恢复 / Snapshots live in the reserved attributes `data-defval` /
 *       `data-defchk` / `data-defsel` for `reset()` to restore
 */
void hpe_form_record_defaults(HpeDomNode *doc);

/**
 * @brief 重置一个 form 的全部控件 / Reset every control of one form
 *
 * @param[in,out] doc 文档节点（按全树找该 form 的控件）/ Document node (controls are found tree-wide)
 * @param[in,out] form 目标 form，`NULL` 表示整份文档 / The target form, `NULL` for the whole document
 */
void hpe_form_reset(HpeDomNode *doc, HpeDomNode *form);

/**
 * @brief 处理一次页面点击的控件默认行为 / Apply a page click's control default behaviour
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in,out] node 点击命中的元素 / The element under the click
 * @param[in]     x 窗口 X 坐标 / Window X coordinate
 * @param[in]     y 窗口 Y 坐标 / Window Y coordinate
 *
 * @return 控件状态被改变返回 1（调用方须 flush）/ 1 when control state changed (the caller must flush)
 *
 * @note 复选框翻转、单选钮组互斥、submit/reset、select 换选项、label 转发、
 *       range 落点取值、文本控件聚焦；点击非可编辑控件时使文本控件失焦（触发 change）/
 *       Toggles a checkbox, applies radio-group exclusivity, runs submit/reset,
 *       cycles the select, forwards label clicks, hits the range thumb, focuses
 *       text controls; clicking anything non-editable blurs the focused text
 *       control (firing `change`)
 */
int hpe_form_click(HpeEngine *engine, HpeDomNode *node, int x, int y);

/**
 * @brief 处理一次键盘事件的控件默认行为 / Apply a keyboard event's control default behaviour
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     vk 虚拟键码 / Virtual key code
 * @param[in]     printable 可打印码点（0 表示非打印键）/ Printable code point (0 when not printable)
 * @param[in]     mods 修饰键掩码 / Modifier mask
 *
 * @return 控件状态被改变返回 1（调用方须 flush）/ 1 when control state changed (the caller must flush)
 *
 * @note 仅作用于获得焦点的可编辑控件：打印字符插入、退格/删除、方向/Home/End 移动插入符；
 *       每次编辑派发 `input` / Only the focused editable control is affected: printable
 *       characters insert, Backspace/Delete erase, arrows/Home/End move the caret;
 *       every edit dispatches `input`
 */
int hpe_form_key(HpeEngine *engine, int vk, int printable, int mods);

/**
 * @brief 使当前编辑控件失焦（值有变则派发 `change`）/ Blur the edited control (`change` when the value changed)
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 */
void hpe_form_blur(HpeEngine *engine);

/**
 * @brief JS `focus()`/`blur()` 后同步表单编辑状态 / Sync the form editing state after a JS `focus()`/`blur()`
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in,out] node 新焦点元素，`NULL` 表示失焦 / New focus element, `NULL` to blur
 */
void hpe_form_focus_notify(HpeEngine *engine, HpeDomNode *node);

/**
 * @brief 文档加载后安装表单控件（默认值快照 + 按钮标签注入）/ Install form controls after a document load
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 记录全部控件的默认值快照（供 `reset()` 恢复），并给仅以 `value` 声明标签的
 *       按钮注入文本子节点 / Records the default snapshots of every control (for
 *       `reset()` to restore) and injects label text into value-only buttons
 */
void hpe_form_install(HpeEngine *engine);

/**
 * @brief 释放表单编辑状态（引擎销毁时调用）/ Release the form editing state on engine shutdown
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 */
void hpe_form_shutdown(HpeEngine *engine);

#endif /* HPE_FORM_H */
