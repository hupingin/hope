/* Hope UI Engine - box tree and block/inline layout */
#ifndef HPE_LAYOUT_H
#define HPE_LAYOUT_H

#include "css/hpe_css.h"
#include "dom/hpe_dom.h"
#include "render/hpe_font.h"

typedef enum HpeBoxKind {
    HPE_BOX_BLOCK = 0,
    HPE_BOX_INLINE,
    HPE_BOX_TEXT,
    HPE_BOX_INLINE_BLOCK
} HpeBoxKind;

typedef struct HpeTextRun {
    const char *text;
    int len;
    HpeFont *font;
    HpeColor color;
    float x;
    float width;
    float ascent;
    float descent;
    float line_height;
    struct HpeBox *atomic;   /* non-NULL => this run is an inline-block box */
} HpeTextRun;

typedef struct HpeLine {
    HpeTextRun *runs;
    int run_count;
    size_t run_cap;
    float width;
    float height;
    float baseline;
    float y;
} HpeLine;

/* one floated box inside a block container's float band */
typedef struct HpeFloatBox {
    struct HpeBox *box;
    float top, bottom; /* band extent in document coords (margin box) */
    float left, right; /* horizontal extent in document coords (margin box) */
    int side;          /* HpeFloatSide */
} HpeFloatBox;

typedef struct HpeFloatBand {
    HpeFloatBox *items;
    int count;
    size_t cap;
} HpeFloatBand;

typedef struct HpeBox {
    HpeBoxKind kind;
    HpeDomNode *node;
    HpeStyle style;
    HpeRect border_box;
    HpeRect content_box;
    HpeRect pad_box;     /* padding box in document coords (containing block for abspos) */
    const char *text;
    int text_len;
    /* generated content (`::before` / `::after`) owns its text; text borrowed
     * from a DOM text node must not be freed by hpe_layout_free */
    int owns_text;
    HpeLine *lines;
    int line_count;
    size_t line_cap;
    struct HpeBox *parent;
    struct HpeBox *first_child;
    struct HpeBox *last_child;
    struct HpeBox *next;
    HpeFloatBand floats; /* floated children of this block container */
    /* replaced element (`<img>`): the intrinsic pixel size of the decoded
     * content plus the size the author asked for (`auto` when none was given).
     * `repl_css_*` is kept verbatim so the used size can be recomputed on every
     * layout pass without the previously written px value being mistaken for an
     * author declaration. */
    int is_replaced;
    int repl_w;
    int repl_h;
    HpeLength repl_css_w;
    HpeLength repl_css_h;
    float repl_used_w; /* last solved used content width */
    float repl_used_h; /* last solved used content height */
    /* element-level scrolling: set when `overflow` is `scroll`/`auto` and the
     * content overflows. `scroll_max` is the largest legal `scroll_y`, and
     * `scroll_y` is the live offset the engine mutates on wheel input. */
    float scroll_y;
    float scroll_max;
    float scroll_port_h; /* the visible (padding-box) height used to clamp */
    float scroll_content_h; /* natural (unclamped) content height */
    int is_scroll_container;
} HpeBox;

/**
 * @brief 为文档构建并排版盒子树 / Build and lay out the box tree for a document
 *
 * @param[in]     document 文档根节点 / Document root node
 * @param[in]     sheet 样式表 / Stylesheet
 * @param[in]     viewport_width 视口宽度 / Viewport width
 * @param[in]     viewport_height 视口高度 / Viewport height
 *
 * @return 盒子树根；无文档或 `<html>` 时返回 `NULL` / Box tree root, or `NULL` when there is no document or no `<html>`
 *
 * @note 视口即初始包含块 / The viewport is the initial containing block
 */
HpeBox *hpe_layout_build(HpeDomNode *document, HpeStylesheet *sheet, float viewport_width,
                         float viewport_height);

/**
 * @brief DOM 变更后的增量重建 / Incremental rebuild after a DOM mutation
 *
 * @param[in]     document 文档根节点 / Document root node
 * @param[in]     sheet 样式表 / Stylesheet
 * @param[in]     viewport_width 视口宽度 / Viewport width
 * @param[in]     viewport_height 视口高度 / Viewport height
 * @param[in,out] old_root 上一轮的盒子树 / Previous pass's box tree
 * @param[in]     reflow_root 子树发生变更的最高 DOM 节点 / Highest DOM node whose subtree changed
 *
 * @return 成功时返回复用的 old_root；输入不允许局部重建时返回 `NULL` / The reused `old_root` on success, or `NULL` when the inputs do not permit a partial pass
 *
 * @note 变更子树之外的盒原样复用，几何结果与 hpe_layout_build 一致 / Boxes outside the changed subtree are reused verbatim and the geometry matches hpe_layout_build
 */
HpeBox *hpe_layout_rebuild(HpeDomNode *document, HpeStylesheet *sheet, float viewport_width,
                           float viewport_height, HpeBox *old_root,
                           HpeDomNode *reflow_root);

/**
 * @brief 释放整棵盒子树 / Free an entire box tree
 *
 * @param[in,out] box 待释放的盒；可为 `NULL` / Box to free, may be `NULL`
 */
void hpe_layout_free(HpeBox *box);

/**
 * @brief 求根盒的总内容高度 / Total content height of the root box
 *
 * @param[in]     root 根盒 / Root box
 *
 * @return 根盒边框盒底边的文档 y；root 为 `NULL` 时返回 0 / Document y of the root's border-box bottom, or 0 when `root` is `NULL`
 */
float hpe_layout_content_height(HpeBox *root);

#endif /* HPE_LAYOUT_H */
