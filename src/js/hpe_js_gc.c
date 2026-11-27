/* Hope UI Engine - conservative mark-sweep GC for the JS arena
 *
 * The arena is a bump allocator (hpe_js_alloc), so nothing can be freed
 * individually. This collector walks every allocation through the chunk
 * headers that hpe_js_alloc now prepends, marks everything reachable from a
 * set of conservative roots, and returns unreachable chunks to a free list
 * that hpe_js_alloc reuses (splitting oversized chunks).
 *
 * Roots (all scanned conservatively as raw memory ranges):
 *   - the current C stack down to the frame that started the engine
 *   - the HpeJs struct itself, the timer table and each malloc'd JsModule
 *   - the suspended coroutine stacks of parked generators / async bodies
 *   - every DOM node's listener chain head (the chains live in the arena)
 * Anything else reachable only through other unmarked arena chunks stays
 * unmarked and is reclaimed. Marking is transitive: each marked chunk's
 * payload bytes are scanned for pointers back into the arena.
 *
 * GC runs only at safe points (end of script evaluation / microtask drain),
 * never while a coroutine is active and never during parsing or execution.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "js/hpe_js_internal.h"
#include "dom/hpe_dom.h"

/* collections start once the arena has grown past this (before any live set) */
#define GC_MIN_ARENA (512u * 1024u)

/* stack high-water mark captured at the outermost host frame (main). Eval
 * frames are always *below* their caller, but hpe_js_create and hpe_js_eval
 * are sibling calls, so a baseline captured inside the create chain would sit
 * below later eval frames and miss their locals as roots. main() records the
 * true base once; every GC run scans down from there. */
static char *gc_main_base;

/* storing the address of a local is the whole point (the GC scans from here
 * down); silence the -Wdangling-pointer analysis around it */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
void hpe_js_gc_capture_main_base(void)
{
    char local;
    gc_main_base = &local;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* chunk walk state shared by the mark helpers (single-threaded engine) */
typedef struct GcWork {
    JsChunkHdr **items;
    size_t count;
    size_t cap;
} GcWork;

static GcWork gc_work;

/* ------------------------------------------------------------------ chunks */

/**
 * @brief 校验一个疑似块头 / Validate a suspected chunk header
 *
 * @param[in] h 疑似块头指针 / Suspected header pointer
 *
 * @return magic 匹配返回 1，否则 0 / 1 when the magic matches, otherwise 0
 */
static int chunk_valid(JsChunkHdr *h)
{
    return h && h->magic == GC_CHUNK_MAGIC && h->mark <= 1 &&
           h->size < (size_t)1 << 30 && (h->size & 7u) == 0;
}

/**
 * @brief 在单个 block 内定位包含地址 p 的块 / Locate the chunk containing `p` inside one block
 *
 * @param[in] b 目标 block / The block to walk
 * @param[in] p 任意地址 / An arbitrary address
 *
 * @return 包围该地址的块头，未命中返回 NULL / The enclosing header, or NULL on a miss
 */
static JsChunkHdr *chunk_in_block(JsBlock *b, void *p)
{
    char *cursor = b->mem;
    char *end = b->mem + b->used;
    if ((char *)p < cursor || (char *)p >= end)
        return NULL;
    while (cursor < end) {
        JsChunkHdr *h = (JsChunkHdr *)cursor;
        if (!chunk_valid(h))
            return NULL;
        if ((char *)p >= cursor && (char *)p < cursor + sizeof(JsChunkHdr) + h->size)
            return h;
        cursor += sizeof(JsChunkHdr) + h->size;
    }
    return NULL;
}

/**
 * @brief 把任意地址解析成它所属的块头 / Resolve an arbitrary address to its enclosing chunk header
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     p 任意地址 / An arbitrary address
 *
 * @return 块头指针；不属于 arena 或校验失败返回 NULL / The header, or NULL when outside the arena or invalid
 *
 * @note 必须先确认地址落在某个已映射 block 内再读块头：候选指针来自栈/寄存器等
 *       保守来源，可能指向未映射内存，先解引用会段错误。快路径要求 p 相对 block
 *       起点 16 字节对齐（块头位置），否则退回线性定位内部指针
 *       / The address must be confirmed to lie inside a mapped block before any
 *       header dereference: candidates come from conservative sources (stack,
 *       registers) and may point at unmapped memory, which would segfault. The
 *       fast path additionally requires an 8-byte-aligned offset from the block
 *       start (where headers live); anything else falls back to the linear walk
 */
static JsChunkHdr *chunk_of(HpeJs *js, void *p)
{
    JsBlock *b;
    char *c = (char *)p;
    if (!p)
        return NULL;
    for (b = js->blocks; b; b = b->next) {
        JsChunkHdr *h;
        if (c < b->mem || c >= b->mem + b->used)
            continue;
        if (((uintptr_t)c - (uintptr_t)b->mem) % 8 != 0)
            return chunk_in_block(b, p);
        h = (JsChunkHdr *)c - 1;
        if ((char *)h >= b->mem && chunk_valid(h) &&
            (char *)h + sizeof(JsChunkHdr) + h->size <= b->mem + b->used)
            return h;
        return chunk_in_block(b, p);
    }
    return NULL;
}

/* ------------------------------------------------------------------ marking */

/**
 * @brief 压入一个待扫描的块 / Push a chunk onto the scan worklist
 *
 * @param[in] h 已标记待扫描的块 / A marked chunk awaiting its payload scan
 */
static void work_push(JsChunkHdr *h)
{
    if (gc_work.count == gc_work.cap) {
        size_t ncap = gc_work.cap ? gc_work.cap * 2 : 256;
        JsChunkHdr **ni = (JsChunkHdr **)realloc(gc_work.items,
                                                 ncap * sizeof(JsChunkHdr *));
        if (!ni)
            return; /* drop the chunk: conservative GC keeps it marked already */
        gc_work.items = ni;
        gc_work.cap = ncap;
    }
    gc_work.items[gc_work.count++] = h;
}

/**
 * @brief 标记一个块；首次标记时将其加入扫描队列 / Mark a chunk; newly marked chunks join the scan queue
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     h 目标块头 / The chunk header to mark
 */
static void chunk_mark(HpeJs *js, JsChunkHdr *h)
{
    if (!h || h->mark)
        return;
    h->mark = 1;
    work_push(h);
}

/**
 * @brief 扫描一段内存，把其中所有指向 arena 的地址标记入队 / Scan a memory range and mark every address that points into the arena
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     lo 低端地址（含）/ Low address (inclusive)
 * @param[in]     hi 高端地址（不含）/ High address (exclusive)
 *
 * @see hpe_js_gc_scan_range
 */
static void scan_range_internal(HpeJs *js, void *lo, void *hi)
{
    char *p;
    if (!lo || !hi || (char *)lo >= (char *)hi)
        return;
    for (p = (char *)(((uintptr_t)lo + 7u) & ~(uintptr_t)7u);
         p + sizeof(void *) <= (char *)hi; p += sizeof(void *)) {
        void *cand;
        memcpy(&cand, p, sizeof(cand));
        chunk_mark(js, chunk_of(js, cand));
    }
}

/**
 * @brief 对外暴露的根范围扫描入口（协程栈扫描钩子调用）/ Public root-range scan entry (used by the coroutine-stack hooks)
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     lo 低端地址（含）/ Low address (inclusive)
 * @param[in]     hi 高端地址（不含）/ High address (exclusive)
 */
void hpe_js_gc_scan_range(HpeJs *js, void *lo, void *hi)
{
    scan_range_internal(js, lo, hi);
}

/**
 * @brief 排空扫描队列，传递标记所有可达块 / Drain the scan worklist, transitively marking reachable chunks
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 本次排空新标记的块数 / The number of chunks newly marked during this drain
 */
static size_t work_drain(HpeJs *js)
{
    size_t marked = gc_work.count;
    while (gc_work.count) {
        JsChunkHdr *h = gc_work.items[--gc_work.count];
        scan_range_internal(js, h + 1, (char *)(h + 1) + h->size);
    }
    return marked;
}

/**
 * @brief 扫描所有带标签的挂起协程状态块 / Scan every tagged suspended-coroutine state chunk
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 本次扫描新发现的块数（0 表示已收敛）/ Chunks newly discovered during this pass (0 means converged)
 */
static size_t scan_fibers(HpeJs *js)
{
    JsBlock *b;
    size_t found = 0;
    for (b = js->blocks; b; b = b->next) {
        char *cursor = b->mem;
        char *end = b->mem + b->used;
        while (cursor + sizeof(JsChunkHdr) <= end) {
            JsChunkHdr *h = (JsChunkHdr *)cursor;
            if (!chunk_valid(h))
                break;
            if (h->mark && h->tag == GC_T_GEN && js->gc_scan_gen) {
                size_t before = gc_work.count;
                js->gc_scan_gen(js, h + 1);
                found += gc_work.count - before;
            } else if (h->mark && h->tag == GC_T_ASYNC && js->gc_scan_async) {
                size_t before = gc_work.count;
                js->gc_scan_async(js, h + 1);
                found += gc_work.count - before;
            }
            cursor += sizeof(JsChunkHdr) + h->size;
        }
    }
    return found;
}

/* ------------------------------------------------------------------- roots */

/**
 * @brief 递归扫描 DOM 树上每个节点的监听器链头 / Recursively mark each DOM node's listener-chain head
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     node DOM 节点（可为 NULL）/ DOM node, may be NULL
 *
 * @note 监听器链本体在 arena 里，因此只需把链头指针当根标记，链上的 handler 值由传递扫描覆盖 / Listener chains live in the arena, so only the chain head needs marking as a root; handler values are covered by transitive scanning
 */
static void scan_dom_node(HpeJs *js, HpeDomNode *node)
{
    HpeDomNode *child;
    if (!node)
        return;
    chunk_mark(js, chunk_of(js, node->listeners));
    for (child = node->first_child; child; child = child->next_sibling)
        scan_dom_node(js, child);
    scan_dom_node(js, node->content);
}

/**
 * @brief 收集全部保守根并压入标记队列 / Gather all conservative roots and queue them for marking
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in,out] regs setjmp 捕获的寄存器缓冲 / Register buffer captured with setjmp
 */
static void gather_roots(HpeJs *js, jmp_buf *regs)
{
    char sp_local;
    JsModule *m;
    /* registers first (callee-saved registers can hold the only reference) */
    scan_range_internal(js, regs, (char *)regs + sizeof(*regs));
    /* the HpeJs struct itself: global env/obj, job queue heads, module
     * pointers, generator registries and every proto field */
    scan_range_internal(js, js, (char *)js + sizeof(*js));
    /* the current C stack, from here up to the outermost host frame (main);
     * GC never runs on a coroutine stack, so this is always the main stack */
    if (gc_main_base && (char *)&sp_local < gc_main_base)
        scan_range_internal(js, &sp_local, gc_main_base);
    else if (js->stack_base && (char *)&sp_local < (char *)js->stack_base)
        scan_range_internal(js, &sp_local, js->stack_base);
    /* the realloc-managed timer table (layout is private to hpe_js_timers.c) */
    hpe_js_timers_gc_scan(js);
    /* malloc'd module records and their module-registry chain */
    for (m = js->modules; m; m = m->next)
        scan_range_internal(js, m, (char *)m + sizeof(*m));
    /* DOM listener chains: only the out-of-arena head pointers need marking */
    if (js->dom_document)
        scan_dom_node(js, (HpeDomNode *)js->dom_document);
}

/* ------------------------------------------------------------------ sweep */

/**
 * @brief 清零所有块的标记位 / Clear the mark bit of every chunk
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 */
static void sweep_clear_marks(HpeJs *js)
{
    JsBlock *b;
    for (b = js->blocks; b; b = b->next) {
        char *cursor = b->mem;
        char *end = b->mem + b->used;
        while (cursor + sizeof(JsChunkHdr) <= end) {
            JsChunkHdr *h = (JsChunkHdr *)cursor;
            if (!chunk_valid(h))
                break;
            h->mark = 0;
            cursor += sizeof(JsChunkHdr) + h->size;
        }
    }
}

/**
 * @brief 把一段连续未标记块合并成一个空闲块挂到空闲链表 / Merge a run of unmarked chunks into one free chunk
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 * @param[in]     first 段首块头 / The header starting the run
 * @param[in]     payload 段的总载荷字节数（含中间块头）/ Total payload bytes of the run (interior headers included)
 * @param[out]    freed 累计释放的载荷字节 / Accumulated freed payload bytes
 */
static void sweep_free_run(HpeJs *js, JsChunkHdr *first, size_t payload, size_t *freed)
{
    void **nextp = (void **)((char *)(first + 1));
    first->size = payload;
    first->tag = GC_T_PLAIN;
    first->mark = 0;
    first->magic = GC_CHUNK_MAGIC;
    *nextp = js->gc_free;
    js->gc_free = (void *)first;
    *freed += payload;
}

/**
 * @brief 线性遍历所有块：未标记的进空闲链表，统计存活字节 / Walk all chunks: unmarked ones join the free list, live bytes are counted
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 存活载荷字节数 / Live payload byte count
 */
static size_t sweep(HpeJs *js)
{
    size_t live = 0;
    size_t freed = 0;
    JsBlock *b;
    js->gc_free = NULL;
    for (b = js->blocks; b; b = b->next) {
        char *cursor = b->mem;
        char *end = b->mem + b->used;
        JsChunkHdr *pending = NULL;
        size_t pending_payload = 0;
        while (cursor + sizeof(JsChunkHdr) <= end) {
            JsChunkHdr *h = (JsChunkHdr *)cursor;
            if (!chunk_valid(h))
                break;
            if (h->mark) {
                if (pending) {
                    sweep_free_run(js, pending, pending_payload, &freed);
                    pending = NULL;
                }
                live += h->size;
            } else if (!pending) {
                pending = h;
                pending_payload = h->size;
            } else {
                pending_payload += sizeof(JsChunkHdr) + h->size;
            }
            cursor += sizeof(JsChunkHdr) + h->size;
        }
        if (pending)
            sweep_free_run(js, pending, pending_payload, &freed);
    }
    js->gc_freed = freed;
    return live;
}

/* ----------------------------------------------------------------- entries */

/**
 * @brief 执行一次完整 GC：清标记 → 收根 → 传递标记 → 清扫 / Run one full collection: clear marks, gather roots, mark transitively, sweep
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @return 存活载荷字节数 / Live payload byte count
 *
 * @note 标记与协程栈扫描交替进行直至收敛（协程状态块可能引用其他挂起栈）/ Marking and coroutine-stack scans alternate until convergence (fiber state chunks may reference other suspended stacks)
 */
size_t hpe_js_gc_collect(HpeJs *js)
{
    jmp_buf regs;
    void *saved_free = js->gc_free;
    size_t live;
    /* the free list must NOT act as a root: sweep rebuilds it from scratch,
     * and marking it would retain every freed chunk (each holds the next
     * pointer of the chain) */
    js->gc_free = NULL;
    sweep_clear_marks(js);
    if (setjmp(regs) == 0) {
        gather_roots(js, &regs);
        for (;;) {
            work_drain(js);
            if (scan_fibers(js) == 0)
                break;
        }
    } else {
        /* longjmp safety net: never taken in practice, keeps gcc -Wextra happy */
        js->gc_free = saved_free;
        return js->gc_last_live;
    }
    live = sweep(js);
    js->gc_last_live = live;
#ifdef HPE_GC_DEBUG
    fprintf(stderr, "DBG gc: total=%zu freed=%zu live=%zu\n", js->gc_total, js->gc_freed, live);
#endif
    return live;
}

/**
 * @brief 按阈值判断是否触发一次 GC / Decide whether a collection is due and run it
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 协程活跃或尚未安装栈基线时跳过；总分配量超过 512KB 且达到上次存活量的 2 倍时收集 / Skipped while a coroutine is active or before the stack baseline is installed; collects once the arena exceeds 512KB and twice the last live size
 *
 * @return 本次确实执行了收集返回 1，否则 0 / 1 when a collection actually ran, otherwise 0
 */
int hpe_js_gc_maybe(HpeJs *js)
{
    if (!js->stack_base && !gc_main_base)
        return 0;
    if (js->gen_active || js->async_active)
        return 0;
    if (js->gc_total < GC_MIN_ARENA)
        return 0;
    if (js->gc_last_live && js->gc_total < js->gc_last_live * 2)
        return 0;
    hpe_js_gc_collect(js);
    return 1;
}

/* --------------------------------------------------------------- builtins */

/**
 * @brief `__gc()` 原生实现：立即执行一次收集 / Native body of `__gc()`: run one collection right away
 */
static JsValue native_gc_collect(HpeJs *js, JsValue thisv, JsValue *a, int argc,
                                 void *ud)
{
    (void)thisv;
    (void)a;
    (void)argc;
    (void)ud;
    return js_num((double)hpe_js_gc_collect(js));
}

/**
 * @brief `__gcStats()` 原生实现：返回 {freed, live, total} 统计对象 / Native body of `__gcStats()`: returns a {freed, live, total} object
 */
static JsValue native_gc_stats(HpeJs *js, JsValue thisv, JsValue *a, int argc,
                               void *ud)
{
    JsObj *o;
    (void)thisv;
    (void)a;
    (void)argc;
    (void)ud;
    o = js_object_new(js);
    js_obj_set(js, o, "freed", js_num((double)js->gc_freed));
    js_obj_set(js, o, "live", js_num((double)js->gc_last_live));
    js_obj_set(js, o, "total", js_num((double)js->gc_total));
    return js_objval(o);
}

/**
 * @brief 安装 `__gc` / `__gcStats` 调试内建并记录栈基线 / Install the `__gc` / `__gcStats` debug builtins and record the stack baseline
 *
 * @param[in,out] js JS 运行时上下文 / JS runtime context
 *
 * @note 由 `hpe_js_create` 在主栈顶层调用，此时取局部变量地址即得栈基线 / Called by `hpe_js_create` at the top of the main stack, so taking a local's address yields the baseline
 */
void hpe_js_gc_install(HpeJs *js)
{
    char sp_local;
    js->stack_base = &sp_local;
    js_obj_set(js, js->global_obj, "__gc",
               js_objval(js_native_new(js, native_gc_collect, NULL)));
    js_obj_set(js, js->global_obj, "__gcStats",
               js_objval(js_native_new(js, native_gc_stats, NULL)));
}
