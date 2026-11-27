/* Hope UI Engine - HTML canvas 2D surface (v0.27)
 *
 * Each <canvas> element owns an offscreen 32bpp pixmap plus a 2D drawing state
 * (styles, transform, current path). The surface is created lazily on first
 * access from the element's width/height attributes, and lives in a registry
 * keyed by DOM node so the paint stage and the JS bridge share one surface.
 */
#ifndef HPE_CANVAS_H
#define HPE_CANVAS_H

#include "base/hpe_types.h"
#include "dom/hpe_dom.h"
#include "render/hpe_pixmap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HPE_CANVAS_PATH_MAX 256   /* maximum path points / 路径点上限 */
#define HPE_CANVAS_STACK_MAX 32   /* save/restore depth / 状态栈深度 */

/** a path point plus a flag marking the start of a subpath */
typedef struct HpeCanvasPoint {
    float x, y;
    int move; /* 1 when this point starts a new subpath */
} HpeCanvasPoint;

/** the mutable 2D drawing state of one canvas */
typedef struct HpeCanvas {
    HpePixmap pm;
    HpeColor fill_style;
    HpeColor stroke_style;
    float line_width;
    float global_alpha;
    /* affine transform [a c e; b d f] */
    float a, b, c, d, e, f;
    HpeCanvasPoint path[HPE_CANVAS_PATH_MAX];
    int path_count;
    int closed; /* current subpath closed */
    /* saved (fill,stroke,lineWidth,alpha,matrix) snapshots */
    struct {
        HpeColor fill, stroke;
        float line_width, alpha;
        float a, b, c, d, e, f;
    } stack[HPE_CANVAS_STACK_MAX];
    int stack_count;
} HpeCanvas;

/**
 * @brief 取得（或按需创建）某 `<canvas>` 节点的绘制表面 / Get (or lazily create) the drawing surface of a `<canvas>` node
 *
 * @param[in,out] node `<canvas>` DOM 节点 / A `<canvas>` DOM node
 *
 * @return 绘制表面指针；非 canvas 节点返回 `NULL` / The surface, or `NULL` for a non-canvas node
 *
 * @note 尺寸取自 width/height 属性（默认 300×150），上限 4096 防滥用 / Size comes from the width/height attributes (default 300x150), clamped to 4096
 */
HpeCanvas *hpe_canvas_get(HpeDomNode *node);

/**
 * @brief 查询节点是否已有绘制表面（不创建）/ Query whether a node already has a surface (without creating it)
 *
 * @param[in] node DOM 节点 / DOM node
 *
 * @return 绘制表面指针或 `NULL` / The surface or `NULL`
 */
HpeCanvas *hpe_canvas_find(const HpeDomNode *node);

/**
 * @brief 重置（清空）画布像素 / Reset (clear) the canvas pixels
 *
 * @param[in,out] canvas 绘制表面 / The drawing surface
 */
void hpe_canvas_clear(HpeCanvas *canvas);

/* ---- rectangle primitives ---- */
void hpe_canvas_fill_rect(HpeCanvas *c, float x, float y, float w, float h);
void hpe_canvas_clear_rect(HpeCanvas *c, float x, float y, float w, float h);
void hpe_canvas_stroke_rect(HpeCanvas *c, float x, float y, float w, float h);

/* ---- path primitives ---- */
void hpe_canvas_begin_path(HpeCanvas *c);
void hpe_canvas_close_path(HpeCanvas *c);
void hpe_canvas_move_to(HpeCanvas *c, float x, float y);
void hpe_canvas_line_to(HpeCanvas *c, float x, float y);
void hpe_canvas_arc(HpeCanvas *c, float x, float y, float r, float a0, float a1, int ccw);
void hpe_canvas_fill_path(HpeCanvas *c);
void hpe_canvas_stroke_path(HpeCanvas *c);

/* ---- transform / state ---- */
void hpe_canvas_save(HpeCanvas *c);
void hpe_canvas_restore(HpeCanvas *c);
void hpe_canvas_translate(HpeCanvas *c, float x, float y);
void hpe_canvas_rotate(HpeCanvas *c, float rad);
void hpe_canvas_scale(HpeCanvas *c, float x, float y);
void hpe_canvas_set_transform(HpeCanvas *c, float a, float b, float cc, float d, float e, float f);
void hpe_canvas_reset_transform(HpeCanvas *c);

/* ---- pixel read / write (RGBA byte order) ---- */
void hpe_canvas_get_image_data(HpeCanvas *c, int x, int y, int w, int h, uint8_t *out);
void hpe_canvas_put_image_data(HpeCanvas *c, int x, int y, int w, int h, const uint8_t *in);

/**
 * @brief 注册 canvas 内容变更回调 / Register the canvas-content-changed callback
 *
 * @param[in] fn 回调，无参数 / The callback, takes no arguments
 *
 * @note 任一 2D 绘制方法后调用，引擎据此标记重绘 / Called after every 2D drawing method so the engine marks a repaint
 */
void hpe_canvas_set_invalidate(void (*fn)(void));

/**
 * @brief 通知 canvas 内容已变更（触发重绘回调）/ Notify that canvas content changed (fires the repaint callback)
 */
void hpe_canvas_notify(void);

/**
 * @brief 释放全部 canvas 表面 / Release all canvas surfaces
 *
 * @note 引擎销毁或加载新文档时调用 / Called when the engine is destroyed or a new document loads
 */
void hpe_canvas_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* HPE_CANVAS_H */
