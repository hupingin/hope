/* Hope UI Engine - engine internals shared between hope.c and hope_chrome.c
 *
 * Not part of the public API (see hope.h); keeping the struct here lets the
 * browser-chrome implementation in hope_chrome.c reach engine state without
 * adding accessors to the public header.
 */
#ifndef HPE_HOPE_INTERNAL_H
#define HPE_HOPE_INTERNAL_H

#include "hope.h"

#include "css/hpe_css.h"
#include "dom/hpe_dom.h"
#include "js/hpe_js.h"
#include "layout/hpe_layout.h"
#include "paint/hpe_paint.h"
#include "render/hpe_pixmap.h"

#define HPE_SCROLL_LINE_PX 48.0f
/* page-level scrollbar gutter width (shown only alongside the browser chrome,
 * so the headless --dump pixel baselines stay page-only) */
#define HPE_PAGE_SCROLLBAR_W 12.0f
#define HPE_ADDR_MAX 1024
/* navigation history depth for the chrome's back/forward buttons */
#define HPE_NAV_MAX 32
/* <link> resource hints we record (stylesheet excluded -- it is consumed) */
#define HPE_MAX_LINKS 32
#define HPE_MAX_PATH 1024
/* maximum number of simultaneously open browser tabs */
#define HPE_TAB_MAX 8

/* dirty flags: what the next render() has to do */
#define HPE_DIRTY_NONE 0
#define HPE_DIRTY_SCROLL 1 /* only the scroll offset moved */
#define HPE_DIRTY_FULL 2   /* full repaint (load, resize, DOM/style mutation) */
#define HPE_DIRTY_CHROME 3 /* only the address bar needs repainting */

/* how much work the accumulated DOM mutations require on the next flush.
 * Ordered: a larger value subsumes the smaller one. */
#define HPE_REFLOW_NONE 0   /* nothing changed: skip layout and repaint */
#define HPE_REFLOW_PAINT 1  /* paint-only change (colour, opacity, shadow): restyle, no geometry */
#define HPE_REFLOW_LAYOUT 2 /* geometry may have changed: rebuild the box tree */

/* page text selection endpoint: a byte offset inside a DOM text node. Keyed by
 * node (not box) so the selection survives relayouts, which rebuild the box
 * tree but keep DOM nodes alive. */
typedef struct HpeSelPos {
    HpeDomNode *node; /* 所属文本节点 / Owning DOM text node */
    int off;          /* 节点文本内的字节偏移 / Byte offset within the node's text */
} HpeSelPos;

/* one browser tab. Only the identity survives a background period (path,
 * per-tab history, scroll offset); switching tabs reloads the document from
 * disk, which keeps memory flat and the engine single-document. */
typedef struct HpeTab {
    int used;                                 /* 槽位在用 / Slot in use */
    char path[HPE_MAX_PATH];                  /* 当前文档路径（""=空白页）/ Current document path (""=blank) */
    char nav[HPE_NAV_MAX][HPE_MAX_PATH];      /* 每标签历史环 / Per-tab history ring */
    int nav_count;                            /* 历史项数 / History entry count */
    int nav_pos;                              /* 当前历史下标 / Current history index */
    float scroll_y;                           /* 切走时的滚动偏移 / Scroll offset when last left */
} HpeTab;

/* one running CSS animation instance bound to a @keyframes rule */
typedef struct HpeAnimRun {
    int used;
    char name[HPE_ANIM_NAME_MAX];
    double start;         /* 引擎动画时钟下的起始时刻 / Start on the engine's animation clock */
    HpeAnimDecl decl;     /* 声明快照（direction/fill/迭代等）/ Declaration snapshot */
} HpeAnimRun;

/* one running transition: from -> to on one animatable property */
typedef struct HpeAnimTransRun {
    int used;
    int prop;             /* HpeAnimProp */
    double start;
    float duration;
    HpeEasing easing;
    HpeAnimValue from, to;
} HpeAnimTransRun;

/* per-node runtime state; entries exist only for elements that declare
 * transitions/animations, keyed by DOM node (not box) so restyles and full
 * box-tree rebuilds keep the runs alive */
typedef struct HpeAnimNodeState {
    HpeDomNode *node;
    int used;
    HpeAnimValue prev[HPE_AP_COUNT]; /* 上次层叠的目标值（用于变化检测）/ Last cascade's target values */
    int has_prev;
    HpeAnimRun runs[HPE_ANIM_MAX];
    HpeAnimTransRun trans[HPE_TRANS_MAX];
} HpeAnimNodeState;

/* engine-level animation world: the interpolation clock and the per-node states */
typedef struct HpeAnimWorld {
    double clock;      /* 动画时钟（秒）/ Animation clock in seconds */
    double last_real;  /* 上次推进的墙钟 / Wall-clock time of the last advance */
    int active;        /* 尚有进行中的过渡或动画 / A transition or animation is in flight */
    int reflow_need;   /* 当前活动项所需的重排级别 / Reflow level the active items need */
    int layout_sticky; /* 活动项触几何属性：本段活动期内每次 flush 都要重建盒树 / An active
                          item hits layout properties: every flush of this active period
                          must rebuild the box tree */
    HpeAnimNodeState *nodes;
    int count;
    size_t cap;
} HpeAnimWorld;

struct HpeEngine {
    HpePixmap pm;
    int width;
    int height;
    char *path;
    HpeDomNode *doc;
    HpeStylesheet sheet;
    HpeBox *root;
    HpeJs *js;
    float scroll_y;
    float painted_scroll; /* scroll offset actually present in the pixmap */
    int painted;          /* 1 once a full frame exists in the pixmap */
    int dirty;
    HpeDomNode *hover_node; /* element currently under the pointer (:hover) */
    HpeDomNode *active_node; /* element the pointer is pressed on (:active) */
    HpeDomNode *focus_node;  /* element owning the focus (:focus) */
    /* form editing state (hope_form.c): the focused text-like control, its
     * caret offset (bytes into the value), and the value it had when focus
     * was gained (a `change` fires on blur only when they differ) */
    HpeDomNode *form_focus; /* focused editable control (text-like / textarea) */
    int form_caret;         /* caret offset in bytes within the control's value */
    char *form_value0;      /* value snapshot taken when focus was gained */
    float content_height;
    /* author stylesheet bookkeeping: the number of rules parsed after the
     * user-agent sheet and the last (dom node, text revision) that produced
     * them. lets re-collection skip when nothing about <style>/<link> changed. */
    int author_rule_base;
    HpeDomNode *style_sig;
    unsigned int style_rev;
    /* DOM-mutation tracker: the maximum reflow the mutations since the last
     * flush require, whether a flush has a pending change at all, and the DOM
     * element whose box subtree has to be rebuilt (NULL => full rebuild). */
    int reflow_need;
    int mutation_pending;
    HpeDomNode *reflow_node;
    double load_ms;
    double render_ms;
    char status[512];
    char error[256]; /* sticky load error, cleared on the next successful load */

    /* document metadata resolved at load time */
    char doc_encoding[16];      /* detected charset label, e.g. "utf-8" */
    int meta_charset;           /* 1 when <meta charset> declared the encoding */
    char base_href[HPE_MAX_PATH]; /* <base href> ("" when absent) */
    /* <link> resources other than the stylesheet, kept for resource hints */
    struct {
        char rel[24];  /* lowercased rel tokens joined by space */
        char href[HPE_MAX_PATH]; /* resolved absolute path */
    } links[HPE_MAX_LINKS];
    int link_count;

    /* browser chrome (address bar), disabled while chrome_h == 0 */
    int chrome_h;
    char addr[HPE_ADDR_MAX]; /* current text (utf-8) */
    int addr_len;
    int addr_focus;
    float addr_scroll; /* horizontal text scroll inside the field, px */
    /* text-editing state: `addr_caret` is the insertion point (byte index),
     * `addr_anchor` is where the current selection started. They differ only
     * while something is selected; the caret is hidden then (as in Chrome). */
    int addr_caret;
    int addr_anchor;
    int addr_drag;       /* left button held inside the field: dragging extends */
    int addr_drag_live;  /* the pointer moved far enough to start a selection */
    float addr_press_x;  /* press position, for the drag threshold */
    int addr_press_index; /* caret index under the press point (== anchor while dragging) */
    double addr_click_t; /* last press time/position, for double/triple click */
    float addr_click_x;
    int addr_click_count;
    char addr_committed[HPE_ADDR_MAX]; /* text as of focus time; Escape restores it */

    /* page-level scrollbar drag: 1 while the thumb is being dragged, and the
     * grab offset between the pointer and the thumb's top edge */
    int page_sb_drag;
    float page_sb_grab;

    /* cursor shown over the client area: the chrome strip resolves first
     * (I-beam in the field, arrow elsewhere), then the hovered box's CSS
     * `cursor` (inherited, so it already reflects the author/UA cascade) */
    int page_cursor;

    /* navigation history for the back/forward buttons: a ring of absolute
     * document paths ("" means the built-in blank page) with `nav_pos` as the
     * current entry. A fresh navigation (address bar, load) truncates the
     * forward tail; back/forward merely move `nav_pos`. */
    char nav[HPE_NAV_MAX][HPE_MAX_PATH];
    int nav_count;
    int nav_pos;
    int nav_suppress; /* 1 while replaying history, so the replay is not re-pushed */
    int chrome_hover_btn; /* nav button under the pointer, -1 when none */

    /* address-bar caret blink: `caret_t` accumulates seconds since the last
     * tick, `caret_on` is the current phase (visible for 0.5s, hidden for
     * 0.5s), `tick_last` is the previous tick's timestamp and `caret_key`
     * fingerprints caret position/focus so any change restarts the cycle */
    double caret_t;
    int caret_on;
    double tick_last;
    int caret_key;

    /* IME composition display: `ime_comp` is the in-progress string shown
     * inline at the caret (underlined); cleared on commit, cancel or blur */
    char ime_comp[HPE_ADDR_MAX];
    int ime_comp_len;

    /* page text selection (anchor = where the drag started, focus = where it
     * currently extends to); `sel_drag` is set between press and release.
     * `sel_map` is the per-box highlight table rebuilt before every repaint
     * and handed to the paint module via hpe_paint_set_selection. */
    HpeSelPos sel_anchor;
    HpeSelPos sel_focus;
    int sel_drag;
    double sel_click_t;   /* last press time/position, for multi-click counting */
    int sel_click_x;
    int sel_click_y;
    int sel_click_count;
    HpePaintSelEntry *sel_map;
    int sel_map_count;
    size_t sel_map_cap;
    HpePaintSel sel_ps; /* descriptor handed to the paint module (self-owned) */

    /* browser tabs: `tabs[tab_active]` mirrors the live navigation state (the
     * engine's nav[] ring IS the active tab's ring; switch copies in/out). */
    HpeTab tabs[HPE_TAB_MAX];
    int tab_count;
    int tab_active;

    /* CSS transition / animation runtime (hope_anim.c) */
    HpeAnimWorld anim;
};

/**
 * @brief 重建盒树并重新夹紧滚动偏移 / Rebuild the box tree and re-clamp the scroll offset
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 急切入口：无条件全量重建；实现位于 `hope.c`；加载、缩放、悬停、定时器等路径使用，惰性策略在 `hpe_engine_flush` / The eager entry point: unconditionally rebuilds everything; implemented in `hope.c`; used by load, resize, hover and timer paths, while the lazy policy lives in `hpe_engine_flush`
 *
 * @see hpe_engine_flush
 */
void hpe_engine_relayout(HpeEngine *engine);

/**
 * @brief 应用自上次以来记录的 DOM 变更，只做其所需的排版/重绘 / Apply the DOM mutations recorded since the last call, doing only as much layout/repaint as they require
 *
 * @details 惰性重排策略：LAYOUT 重建受影响的盒子子树（作用域无界时整树）；PAINT 重算样式并重绘；NONE 完全跳过。作用域由变更汇聚点记录，可把重建限制在局部子树。
 *          Lazy policy: LAYOUT rebuilds the affected box subtree (everything when unbounded), PAINT recomputes styles and repaints, and NONE skips entirely. The scope is recorded by the mutation sink, which can bound the rebuild to a partial subtree.
 *
 * @param[in,out] engine 引擎实例，NULL 时直接返回 / Engine instance, returns early when NULL
 * @param[in]     force 非零请求全量重建 / Non-zero requests a full rebuild
 *
 * @note 实现位于 `hope.c` / Implemented in `hope.c`
 *
 * @see hpe_engine_relayout
 */
void hpe_engine_flush(HpeEngine *engine, int force);

/**
 * @brief 刷新状态行文本 / Refresh the status line
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 实现位于 `hope.c`；汇总文件名、尺寸、是否含地址栏、加载/绘制耗时与滚动位置，并追加粘性加载错误 / Implemented in `hope.c`; formats the file name, size, chrome flag, load/paint timings and scroll position, and appends the sticky load error
 *
 * @see hpe_engine_status
 */
void hpe_engine_refresh_status(HpeEngine *engine);

/**
 * @brief 推进地址栏插入符闪烁时钟 / Advance the address-bar caret blink clock
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 闪烁相位翻转返回 1，否则 0 / 1 when the blink phase flipped, otherwise 0
 *
 * @note 实现位于 `hope.c`；由 `hpe_engine_tick` 与 `hpe_chrome_paint` 共同驱动 / Implemented in `hope.c`; driven from both `hpe_engine_tick` and `hpe_chrome_paint`
 *
 * @see hpe_engine_tick hpe_chrome_paint
 */
int hpe_engine_caret_advance(HpeEngine *engine);

/**
 * @brief 取页面视口高度（窗口高减地址栏）/ Get the page viewport height (window height minus chrome)
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 视口高度（像素），不小于 0；`engine` 为 NULL 时返回 0 / Viewport height in pixels, never below 0; 0 when `engine` is NULL
 *
 * @note 实现位于 `hope.c` / Implemented in `hope.c`
 */
int hpe_engine_view_height(HpeEngine *engine);

/* ---------------- hope_sel.c ---------------- */

/**
 * @brief 查询是否存在非折叠的页面选区 / Test whether a non-collapsed page selection exists
 *
 * @param[in]     engine 引擎实例 / Engine instance
 *
 * @return 有选区返回 1，否则 0 / 1 when a selection exists, otherwise 0
 */
int hpe_sel_has(const HpeEngine *engine);

/**
 * @brief 处理页面上的指针按下（开始/扩展/双击选词） / Handle a pointer press on the page (start/extend/double-click word select)
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标（窗口坐标）/ X coordinate in window space
 * @param[in]     y Y 坐标（窗口坐标）/ Y coordinate in window space
 * @param[in]     mods 修饰键掩码，`HPE_MOD_SHIFT` 扩展既有选区 / Modifier mask; `HPE_MOD_SHIFT` extends an existing selection
 *
 * @return 选区状态发生变化返回 1，否则 0 / 1 when the selection changed, otherwise 0
 *
 * @note 命中非文本区域时折叠既有选区；双击选中光标处的词 / A press on a non-text area collapses the selection; a double click selects the word under the pointer
 */
int hpe_sel_mouse_down(HpeEngine *engine, int x, int y, int mods);

/**
 * @brief 拖选进行中处理指针移动（扩展焦点端点） / Handle a pointer move while dragging (extends the focus endpoint)
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标（窗口坐标）/ X coordinate in window space
 * @param[in]     y Y 坐标（窗口坐标）/ Y coordinate in window space
 *
 * @return 选区变化返回 1，否则 0 / 1 when the selection changed, otherwise 0
 */
int hpe_sel_mouse_move(HpeEngine *engine, int x, int y);

/**
 * @brief 结束拖选 / End a selection drag
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 */
void hpe_sel_mouse_up(HpeEngine *engine);

/**
 * @brief 把选区文本写入剪贴板 / Copy the selected text to the clipboard
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 写入成功返回 1，无选区或文本为空返回 0 / 1 when written, 0 without a selection or when the text is empty
 *
 * @note 区块盒之间补换行符，与浏览器复制行为一致 / Newlines are inserted between block boxes, matching browser copy behaviour
 */
int hpe_sel_copy(HpeEngine *engine);

/**
 * @brief 全选文档文本 / Select all document text
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 文档含文本且选区变化返回 1，否则 0 / 1 when the document has text and the selection changed, otherwise 0
 */
int hpe_sel_select_all(HpeEngine *engine);

/**
 * @brief 清除页面选区 / Clear the page selection
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 原有选区被清除返回 1，本无选区返回 0 / 1 when an existing selection was cleared, 0 when there was none
 */
int hpe_sel_clear(HpeEngine *engine);

/**
 * @brief 重建逐盒选区区间表并交给绘制模块 / Rebuild the per-box selection table and hand it to the paint module
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 每次重绘页面前调用；选区失效（节点被移除）时自然产生空表 / Called before every page repaint; a stale selection (removed nodes) naturally yields an empty table
 */
void hpe_sel_prepare(HpeEngine *engine);

/**
 * @brief 无条件丢弃选区状态（换页时调用，不触发重绘） / Drop the selection state unconditionally (on page load, no repaint)
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 旧文档的 DOM 节点即将销毁，端点引用必须清空；释放区间表缓冲由 hpe_sel_shutdown 负责 / The old document's DOM nodes are about to be freed, so the endpoints must be cleared; freeing the table buffer is hpe_sel_shutdown's job
 *
 * @see hpe_sel_shutdown
 */
void hpe_sel_reset(HpeEngine *engine);

/**
 * @brief 释放选区模块的缓冲 / Free the selection module's buffers
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 */
void hpe_sel_shutdown(HpeEngine *engine);

/* ---------------- hope.c: tabs ---------------- */

/* ---------------- hope_anim.c ---------------- */

/**
 * @brief 注册层叠钩子并重置动画世界 / Register the cascade hook and reset the animation world
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 在引擎创建时调用；时钟归零、节点状态表清空 / Called at engine creation; the clock
 *       is zeroed and the node-state table emptied
 */
void hpe_anim_install(HpeEngine *engine);

/**
 * @brief 推进动画时钟并求值所有运行项 / Advance the animation clock and evaluate every run
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 仍有进行中的过渡或动画返回 1，否则 0 / 1 while a transition or animation is in flight, 0 otherwise
 *
 * @note `HOPE_TEST_ANIM_STEP=秒` 时每 tick 固定推进该秒数（无头回归用确定性时钟），否则按墙钟差推进（截断到 0.25s）；活动项所需重排级别写入 `engine->anim.reflow_need` / With `HOPE_TEST_ANIM_STEP=sec` each tick advances a fixed amount (a deterministic clock for headless regression), otherwise the wall-clock delta is used (clamped to 0.25s); the reflow level required by the active items lands in `engine->anim.reflow_need`
 */
int hpe_anim_tick(HpeEngine *engine);

/**
 * @brief 换页时重置动画世界 / Reset the animation world on a page load
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 时钟归零、节点状态表清空；缓冲保留复用 / The clock is zeroed and the node-state table emptied; the buffer is kept for reuse
 */
void hpe_anim_reset(HpeEngine *engine);

/**
 * @brief 释放动画世界缓冲 / Free the animation world's buffers
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 */
void hpe_anim_shutdown(HpeEngine *engine);

/**
 * @brief 打印某元素当前动画/过渡后的计算值快照 / Dump an element's post-animation computed value snapshot
 *
 * @param[in] engine 引擎实例 / Engine instance
 * @param[in] id 目标元素的 id 属性 / id attribute of the target element
 *
 * @note `HOPE_TEST_ANIM_SNAPSHOT=id` 测试钩子使用；输出 `ANIM-SNAP <id> ...` 行，值取该元素盒树的最新级叠结果 / Used by the `HOPE_TEST_ANIM_SNAPSHOT=id` test hook; prints an `ANIM-SNAP <id> ...` line with the element's latest cascaded values
 */
void hpe_anim_snapshot_dump(HpeEngine *engine, const char *id);

/**
 * @brief 新建一个空白标签并切换过去 / Open a new blank tab and switch to it
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 成功返回 1，已达上限返回 0 / 1 on success, 0 when the tab limit is reached
 */
int hpe_engine_tab_new(HpeEngine *engine);

/**
 * @brief 关闭指定下标的标签 / Close the tab at the given index
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     index 待关闭的标签下标 / Index of the tab to close
 *
 * @return 成功返回 1，下标非法返回 0 / 1 on success, 0 for an invalid index
 *
 * @note 关闭最后一个标签会重置为空白页而不是关闭 / Closing the last tab resets it to the blank page instead
 */
int hpe_engine_tab_close(HpeEngine *engine, int index);

/**
 * @brief 切换到指定下标的标签 / Switch to the tab at the given index
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     index 目标标签下标 / Index of the target tab
 *
 * @return 成功返回 1，下标非法或已是当前标签返回 0 / 1 on success, 0 for an invalid index or the already-active tab
 *
 * @note 切换会保存当前标签的路径/历史/滚动，重载目标标签的文档并恢复其滚动 / Switching saves the current tab's path/history/scroll, reloads the target document and restores its scroll
 */
int hpe_engine_tab_switch(HpeEngine *engine, int index);

/**
 * @brief 取当前打开的标签数 / Get the number of open tabs
 *
 * @param[in]     engine 引擎实例 / Engine instance
 *
 * @return 标签数，`engine` 为 NULL 时返回 0 / Tab count, 0 when `engine` is NULL
 */
int hpe_engine_tab_count(HpeEngine *engine);

/**
 * @brief 取当前活动标签下标 / Get the active tab's index
 *
 * @param[in]     engine 引擎实例 / Engine instance
 *
 * @return 活动标签下标，`engine` 为 NULL 时返回 -1 / Active tab index, -1 when `engine` is NULL
 */
int hpe_engine_tab_active(HpeEngine *engine);

/* ---------------- hope_chrome.c ---------------- */

/**
 * @brief 替换地址栏文本并把插入符移到末尾 / Replace the address-bar text and put the caret at the end
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     text 新文本，超长时按输入框容量截断 / New text, truncated to the field capacity when too long
 *
 * @note 选区折叠，并记录为 Escape 回退所用的地址；实现位于 `hope_chrome.c` / The selection is collapsed and the text is recorded as the address Escape falls back to; implemented in `hope_chrome.c`
 *
 * @see hpe_chrome_navigate
 */
void hpe_chrome_set_text(HpeEngine *engine, const char *text);

/**
 * @brief 把地址栏绘制到顶部 `chrome_h` 像素 / Paint the address bar into the top `chrome_h` pixels
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @note 实现位于 `hope_chrome.c`；由 `hpe_engine_render` 在整帧或仅地址栏脏时调用 / Implemented in `hope_chrome.c`; called by `hpe_engine_render` for a full frame or a chrome-only repaint
 *
 * @see hpe_engine_render
 */
void hpe_chrome_paint(HpeEngine *engine);

/**
 * @brief 处理地址栏条带内的一次指针按下 / 抬起 / Handle one pointer press/release inside the chrome strip
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标（设备坐标）/ X coordinate in device coordinates
 * @param[in]     y Y 坐标（设备坐标）/ Y coordinate in device coordinates
 * @param[in]     down 按下为 1、抬起为 0 / 1 when pressed, 0 when released
 * @param[in]     mods 修饰键掩码，`HPE_MOD_SHIFT` 时 Shift+点击扩展选区 / Modifier mask; `HPE_MOD_SHIFT` makes a Shift+click extend the selection
 *
 * @return 事件被地址栏消费返回 1，否则 0 / 1 when the bar consumed the event, otherwise 0
 *
 * @note 实现位于 `hope_chrome.c` / Implemented in `hope_chrome.c`
 *
 * @see hpe_chrome_mouse_move
 */
int hpe_chrome_mouse(HpeEngine *engine, int x, int y, int down, int mods);

/**
 * @brief 地址栏拖选进行中时处理指针移动 / Handle a pointer move while the field is being dragged
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标（设备坐标）/ X coordinate in device coordinates
 * @param[in]     y Y 坐标（设备坐标）/ Y coordinate in device coordinates
 *
 * @return 事件被地址栏消费返回 1，否则 0 / 1 when the bar consumed the event, otherwise 0
 *
 * @note 实现位于 `hope_chrome.c`；即使指针离开条带，源自条带内的拖拽仍继续接收移动 / Implemented in `hope_chrome.c`; a drag that began inside the strip keeps receiving moves even once the pointer leaves it
 *
 * @see hpe_chrome_mouse
 */
int hpe_chrome_mouse_move(HpeEngine *engine, int x, int y);

/**
 * @brief 查询条带内某点的光标语义 / Query the cursor semantics for a point inside the chrome strip
 *
 * @param[in]     engine 引擎实例 / Engine instance
 * @param[in]     x X 坐标（设备坐标）/ X coordinate in device coordinates
 * @param[in]     y Y 坐标（设备坐标）/ Y coordinate in device coordinates
 *
 * @return `HpeCursor` 值；点在条带之外返回 -1 / An `HpeCursor` value; -1 when the point is outside the strip
 *
 * @note 输入框内为 I-beam、条带其余处为箭头；实现位于 `hope_chrome.c` / I-beam inside the field and the arrow everywhere else in the strip; implemented in `hope_chrome.c`
 *
 * @see hpe_chrome_mouse
 */
int hpe_chrome_cursor_at(HpeEngine *engine, int x, int y);

/**
 * @brief 按下后退按钮并回到上一历史项 / Activate the back button and return to the previous history entry
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 发生了导航返回 1，否则 0 / 1 when a navigation happened, otherwise 0
 *
 * @note 无上一项时什么都不做；实现位于 `hope.c` / Does nothing without a previous entry; implemented in `hope.c`
 *
 * @see hpe_engine_forward
 */
int hpe_engine_back(HpeEngine *engine);

/**
 * @brief 按下前进按钮并前往下一历史项 / Activate the forward button and go to the next history entry
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 发生了导航返回 1，否则 0 / 1 when a navigation happened, otherwise 0
 *
 * @note 无下一项时什么都不做；实现位于 `hope.c` / Does nothing without a next entry; implemented in `hope.c`
 *
 * @see hpe_engine_back
 */
int hpe_engine_forward(HpeEngine *engine);

/**
 * @brief 按地址栏内容（`addr`）导航 / Navigate using the address-bar contents (`addr`)
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 加载成功返回 1，否则 0 / 1 when the load succeeded, otherwise 0
 *
 * @note 实现位于 `hope_chrome.c`；文本为空时重新加载内置空白页 / Implemented in `hope_chrome.c`; empty text reloads the built-in blank page
 *
 * @see hpe_chrome_set_text hpe_engine_load_blank
 */
int hpe_chrome_navigate(HpeEngine *engine);

#endif /* HPE_HOPE_INTERNAL_H */
