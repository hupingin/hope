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
/* <link> resource hints we record (stylesheet excluded -- it is consumed) */
#define HPE_MAX_LINKS 32
#define HPE_MAX_PATH 1024

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
 * @brief 取页面视口高度（窗口高减地址栏）/ Get the page viewport height (window height minus chrome)
 *
 * @param[in,out] engine 引擎实例 / Engine instance
 *
 * @return 视口高度（像素），不小于 0；`engine` 为 NULL 时返回 0 / Viewport height in pixels, never below 0; 0 when `engine` is NULL
 *
 * @note 实现位于 `hope.c` / Implemented in `hope.c`
 */
int hpe_engine_view_height(HpeEngine *engine);

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
