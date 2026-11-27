# Hope UI 引擎 API 参考文档

本文档由 `tools/gen_api_docs.py` 从 `src/` 下的头文件自动抽取生成，共覆盖 29 个头文件、436 个函数、123 个类型定义。内容以各声明上方的 Doxygen 注释（`@brief`/`@details`/`@param`/`@return`/`@retval`/`@note`/`@see`）为准，仅保留中文部分。重复运行脚本会整体覆盖本文件。

> 标注「内部」的章节属于引擎内部实现接口（`*_internal.h`），不作为对外 API 使用。

## 目录

- [src/base（基础类型与工具）](#srcbase基础类型与工具) —— 31 个函数 / 5 个类型
- [src/canvas（）](#srccanvas) —— 0 个函数 / 0 个类型
- [src/css（CSS 解析与样式）](#srccsscss-解析与样式) —— 27 个函数 / 51 个类型
- [src/dom（DOM 树）](#srcdomdom-树) —— 21 个函数 / 4 个类型
- [src/html（HTML 解析）](#srchtmlhtml-解析) —— 3 个函数 / 1 个类型
- [src/js（JavaScript 引擎）](#srcjsjavascript-引擎) —— 162 个函数 / 30 个类型
- [src/layout（布局）](#srclayout布局) —— 5 个函数 / 7 个类型
- [src/paint（绘制）](#srcpaint绘制) —— 12 个函数 / 2 个类型
- [src/platform（平台层）](#srcplatform平台层) —— 34 个函数 / 7 个类型
- [src/render（渲染与资源）](#srcrender渲染与资源) —— 50 个函数 / 7 个类型
- [src（引擎入口）](#src引擎入口) —— 91 个函数 / 9 个类型

## src/base（基础类型与工具）

### hpe_types.h

4 个函数，3 个类型。

**函数**

#### `hpe_rgba`

```c
static inline HpeColor hpe_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    HpeColor c;
    c.r = r;
    c.g = g;
    c.b = b;
    c.a = a;
    return c;
}
```

用四个通道分量构造颜色

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `r` | in | 红色通道（0~255） |
| `g` | in | 绿色通道（0~255） |
| `b` | in | 蓝色通道（0~255） |
| `a` | in | 不透明度通道（0 全透明，255 全不透明） |

**返回**：构造出的颜色

**注意**：不做范围校验，超出 0~255 的值按 uint8_t 回绕

**参见**：`hpe_rgb`

#### `hpe_rgb`

```c
static inline HpeColor hpe_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return hpe_rgba(r, g, b, 255);
}
```

用红绿蓝三分量构造不透明颜色

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `r` | in | 红色通道（0~255） |
| `g` | in | 绿色通道（0~255） |
| `b` | in | 蓝色通道（0~255） |

**返回**：alpha 固定为 255 的颜色

**注意**：等价于 `hpe_rgba(r, g, b, 255)`

**参见**：`hpe_rgba`

#### `hpe_rect`

```c
static inline HpeRect hpe_rect(float x, float y, float w, float h)
{
    HpeRect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}
```

用左上角坐标与宽高构造矩形

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `x` | in | 左边界 X 坐标 |
| `y` | in | 上边界 Y 坐标 |
| `w` | in | 宽度，可正可负 |
| `h` | in | 高度，可正可负 |

**返回**：构造出的矩形

**注意**：输入不做规范化，负宽高会原样保留

**参见**：`hpe_rect_empty`

#### `hpe_rect_empty`

```c
static inline int hpe_rect_empty(HpeRect r)
{
    return r.w <= 0.0f || r.h <= 0.0f;
}
```

判断矩形是否为空

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `r` | in | 待检查的矩形，按值传入 |

**返回**：宽或高小于等于 0 返回 1

**注意**：空矩形不覆盖任何像素，光栅化阶段会直接跳过

**参见**：`hpe_rect`

**类型**

#### `HpeRect`

```c
typedef struct HpeRect {
    float x, y, w, h;
} HpeRect;
```

#### `HpeColor`

```c
typedef struct HpeColor {
    uint8_t r, g, b, a;
} HpeColor;
```

#### `HpeFontFamily`

```c
typedef enum HpeFontFamily {
    HPE_FONT_DEFAULT = 0, /* engine default (same as sans) */
    HPE_FONT_SERIF,
    HPE_FONT_SANS,
    HPE_FONT_MONO
} HpeFontFamily;
```

### hpe_util.h

27 个函数，2 个类型。

**函数**

#### `hpe_alloc`

```c
void *hpe_alloc(size_t size);
```

分配一块堆内存

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `size` | in | 请求的字节数（0 视为 1） |

**返回**：指向新内存的指针

**注意**：分配失败时打印错误并 abort，绝不返回 NULL

**参见**：`hpe_calloc`、`hpe_free`

#### `hpe_calloc`

```c
void *hpe_calloc(size_t count, size_t size);
```

分配并清零一块堆内存

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `count` | in | 元素个数（0 视为 1） |
| `size` | in | 单个元素字节数（0 视为 1） |

**返回**：指向已清零内存的指针

**注意**：分配失败时打印错误并 abort，绝不返回 NULL

#### `hpe_realloc`

```c
void *hpe_realloc(void *ptr, size_t size);
```

调整已分配内存块的大小

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `ptr` | in,out | 原内存块，可为 NULL |
| `size` | in | 新字节数（0 视为 1） |

**返回**：指向新内存块的指针

#### `hpe_strdup`

```c
char *hpe_strdup(const char *s);
```

复制字符串到新分配的堆内存

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `s` | in | 以 NUL 结尾的源字符串 |

**返回**：新分配的副本，调用方负责释放

#### `hpe_strndup`

```c
char *hpe_strndup(const char *s, size_t n);
```

复制字符串的前 n 个字节

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `s` | in | 源字符串 |
| `n` | in | 最多复制的字节数 |

**返回**：新分配且以 NUL 结尾的副本

**注意**：遇到 `\0` 提前结束；结果长度取 n 与 strlen(s) 的较小者

#### `hpe_free`

```c
void hpe_free(void *ptr);
```

释放引擎分配的内存

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `ptr` | in,out | 待释放的指针，可为 NULL |

#### `hpe_grow`

```c
void *hpe_grow(void *ptr, size_t *cap, size_t need, size_t elem_size);
```

按需倍增数组容量

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `ptr` | in,out | 原数组，可为 NULL |
| `cap` | out | 当前容量，写入新容量 |
| `need` | in | 需要的元素个数 |
| `elem_size` | in | 单个元素字节数 |

**返回**：必要时重新分配后的数组指针

**注意**：容量不足时从 8 起翻倍直到覆盖 need；已够用时原样返回

#### `hpe_ascii_lower`

```c
int hpe_ascii_lower(int c);
```

把 ASCII 大写字母转为小写

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `c` | in | 输入字符 |

**返回**：小写形式，非大写字母原样返回

#### `hpe_str_ieq`

```c
int hpe_str_ieq(const char *a, const char *b);
```

忽略大小写比较两个字符串

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `a` | in | 第一个字符串，可为 NULL |
| `b` | in | 第二个字符串，可为 NULL |

**返回**：相等返回 1

**注意**：两个指针同为 NULL 时视为相等

#### `hpe_str_ieq_n`

```c
int hpe_str_ieq_n(const char *a, const char *b, size_t n);
```

忽略大小写比较两个字符串的前 n 个字节

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `a` | in | 第一个字符串 |
| `b` | in | 第二个字符串 |
| `n` | in | 比较的字节数 |

**返回**：前 n 个字节都相同返回 1

#### `hpe_trim`

```c
char *hpe_trim(char *s);                    /* trims in place, returns s */
```

就地去除字符串首尾空白

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `s` | in,out | 可修改的字符串，可为 NULL |

**返回**：去掉前导空白后的起始指针

#### `hpe_is_space`

```c
int hpe_is_space(char c);
```

判断字符是否为 ASCII 空白

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `c` | in | 输入字符 |

**返回**：是空白返回 1

**注意**：空白集合为空格、`\t`、`\n`、`\r`、`\f`、`\v`

#### `hpe_starts_with_i`

```c
int hpe_starts_with_i(const char *s, const char *prefix);
```

忽略大小写判断字符串是否以某前缀开头

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `s` | in | 待检查字符串 |
| `prefix` | in | 前缀 |

**返回**：以前缀开头返回 1

#### `hpe_utf8_next`

```c
int hpe_utf8_next(const char *s, uint32_t *cp);
```

解码下一个 UTF-8 码点

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `s` | in | 指向码点起始字节 |
| `cp` | out | 解码出的码点 |

**返回**：消耗的字节数（至少 1）

**注意**：非法序列写出 U+FFFD 并只前进 1 字节，因此不会死循环

#### `hpe_utf8_prev`

```c
int hpe_utf8_prev(const char *begin, const char *cur);
```

向前回退一个 UTF-8 码点

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `begin` | in | 缓冲区起始指针 |
| `cur` | in | 当前指针 |

**返回**：回退消耗的字节数，已在起始处返回 0

**注意**：只扫描 continuation 字节（`0b10xxxxxx`）

#### `hpe_utf8_encode`

```c
int hpe_utf8_encode(uint32_t cp, char out[4]);
```

把一个码点编码为 UTF-8

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `cp` | in | Unicode 码点 |
| `out` | out | 至少 4 字节的输出缓冲 |

**返回**：写出的字节数（1~4）

**注意**：不做范围校验，码点大于 U+10FFFF 时按 4 字节写入

#### `hpe_len_auto`

```c
HpeLength hpe_len_auto(void);
```

构造 `auto` 长度

**返回**：单位为 `HPE_LEN_AUTO` 的长度值

#### `hpe_len_px`

```c
HpeLength hpe_len_px(float v);
```

构造像素长度

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 像素值 |

**返回**：单位为 `HPE_LEN_PX` 的长度值

#### `hpe_len_pct`

```c
HpeLength hpe_len_pct(float v);
```

构造百分比长度

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 百分比数值（未除 100） |

**返回**：单位为 `HPE_LEN_PCT` 的长度值

#### `hpe_len_unit`

```c
HpeLength hpe_len_unit(float v, HpeLenUnit unit);
```

按指定单位构造长度

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 数值 |
| `unit` | in | 长度单位 |

**返回**：构造出的长度值

#### `hpe_len_parse`

```c
HpeLength hpe_len_parse(const char *s);
```

解析 CSS 长度字面量

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `s` | in | 长度文本，可为 NULL |

**返回**：解析出的长度值

**注意**：支持 `auto`、`%`、`px`、`em`、`rem`、`vw`、`vh`、`ch`、`pt`、`pc`、`in`、`cm`、`mm`、`q`；物理单位按 96dpi 立即折算为 px，无法识别时按 px 处理

#### `hpe_len_resolve`

```c
float hpe_len_resolve(HpeLength len, float base);
```

把长度按基准值求值为像素

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `len` | in | 待求值长度 |
| `base` | in | 百分比基准（通常为包含块宽度） |

**返回**：像素值；`auto` 返回 -1

**注意**：相对单位（em/rem/vw/vh/ch）一律按 0 上下文求值，等价于传全 0 给 hpe_len_resolve_full

**参见**：`hpe_len_resolve_full`

#### `hpe_len_resolve_full`

```c
float hpe_len_resolve_full(HpeLength len, float base, float font_size, float root_font_size, float vw, float vh, float ch_width);
```

带完整上下文地把长度求值为像素

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `len` | in | 待求值长度 |
| `base` | in | 百分比基准（包含块宽度或高度） |
| `font_size` | in | 元素自身字号，供 em 使用（0 视为 16） |
| `root_font_size` | in | 根元素字号，供 rem 使用（0 视为 16） |
| `vw` | in | 视口宽度，供 vw 使用 |
| `vh` | in | 视口高度，供 vh 使用 |
| `ch_width` | in | `0` 字形步进宽度，供 ch 使用 |

**返回**：像素值；`auto` 返回 -1

**注意**：缺省上下文时 em/rem 退回 16px、vw/vh 退回 16px、ch 退回半字号

#### `hpe_len_is_px`

```c
int hpe_len_is_px(HpeLength len);
```

判断长度是否为绝对像素值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `len` | in | 待检查长度 |

**返回**：单位为 px 返回 1

**注意**：解析期已把物理单位换算为 px，因此此类长度无需上下文即可求值

#### `hpe_len_is_auto`

```c
int hpe_len_is_auto(HpeLength len);
```

判断长度是否为 `auto`

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `len` | in | 待检查长度 |

**返回**：`auto` 返回 1

#### `hpe_latin1_to_utf8`

```c
char *hpe_latin1_to_utf8(const char *src, size_t len, size_t *out_len);
```

把 Latin-1/cp1252 字节流转换为 UTF-8

**详细说明**：`0x80`~`0x9F` 经 cp1252 表映射（弯引号、破折号、省略号、欧元符号等），其余按 Latin-1 处理。 Bytes `0x80` to `0x9F` go through the cp1252 table (curly quotes, dashes, ellipsis, euro sign, ...), everything else is treated as Latin-1.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `src` | in | 源字节流 |
| `len` | in | 源字节数 |
| `out_len` | out | 结果的 UTF-8 字节数，可为 NULL |

**返回**：新分配且以 NUL 结尾的 UTF-8 字符串

**注意**：调用方负责释放返回值

#### `hpe_color_parse`

```c
int hpe_color_parse(const char *s, HpeColor *out);
```

解析任意 CSS 颜色写法

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `s` | in | 颜色文本，可为 NULL |
| `out` | out | 解析出的颜色 |

**返回**：解析成功返回 1

**注意**：依次尝试具名色、`#` 十六进制短/长写法、`rgb()`/`rgba()`、`hsl()`/`hsla()`

**类型**

#### `HpeLenUnit`

```c
typedef enum HpeLenUnit {
    HPE_LEN_AUTO = 0,
    HPE_LEN_PX,
    HPE_LEN_PCT,
    HPE_LEN_EM,  /* relative to the element's own font-size */
    HPE_LEN_REM, /* relative to the root font-size */
    HPE_LEN_VW,
    HPE_LEN_VH,
    HPE_LEN_CH,  /* advance width of the "0" glyph */
    HPE_LEN_PT   /* points, resolved to px at parse time as px via `pt_to_px` */
} HpeLenUnit;
```

#### `HpeLength`

```c
typedef struct HpeLength {
    float value;
    HpeLenUnit unit;
} HpeLength;
```

## src/canvas（）

### hpe_canvas.h

0 个函数，0 个类型。

**函数**

无。

**类型**

无。

## src/css（CSS 解析与样式）

### hpe_anim.h

13 个函数，0 个类型。

**函数**

#### `hpe_anim_parse_transition_shorthand`

```c
void hpe_anim_parse_transition_shorthand(const char *value, HpeStyle *d);
```

解析 transition 简写

**详细说明**：按顶层逗号切分列表，逐项扫描 token：时间 token 第一个为 duration、第二个为 delay；缓动关键字（含 `cubic-bezier(...)` / `steps(...)` 括号组）为 timing function；可插值属性名为 property；`none` 清空列表。每项写入 `d->transitions` 并置位 `HPE_PROP2_TRANSITION`。 The list is split on top-level commas; per item the first time token is the duration and the second the delay, easing keywords (including the `cubic-bezier(...)` / `steps(...)` parenthesised groups) set the timing function and an interpolable property name sets the property. `none` clears the list. Each item is stored in `d->transitions` and `HPE_PROP2_TRANSITION` is set.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `value` | in | 声明值 |
| `d` | in,out | 目标样式 |

**参见**：`hpe_anim_parse_transition_longhand`

#### `hpe_anim_parse_transition_longhand`

```c
void hpe_anim_parse_transition_longhand(const char *prop, const char *value, HpeStyle *d);
```

解析 transition 长属性

**详细说明**：`prop` 取 "transition-property" / "transition-duration" / "transition-timing-function" / "transition-delay" 之一，值按逗号列表写入 `d->transitions[i]` 对应字段并置位 `HPE_PROP2_TRANSITION`。 `prop` is one of "transition-property" / "transition-duration" / "transition-timing-function" / "transition-delay"; the comma list fills the matching field of `d->transitions[i]` and sets `HPE_PROP2_TRANSITION`.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `prop` | in | 长属性名 |
| `value` | in | 声明值 |
| `d` | in,out | 目标样式 |

#### `hpe_anim_parse_animation_shorthand`

```c
void hpe_anim_parse_animation_shorthand(const char *value, HpeStyle *d);
```

解析 animation 简写

**详细说明**：按顶层逗号切分，逐项按 CSS Animations §3 顺序无关规则识别：时间 token 第一个为 duration、第二个为 delay；缓动关键字为 timing function； `infinite` 或纯数字为迭代次数；direction / fill-mode 关键字各归其位； 余下的标识符为 @keyframes 名。`none` 清空列表。 The list is split on top-level commas; per item, in the order-free manner of CSS Animations §3: the first time token is the duration and the second the delay, easing keywords set the timing function, `infinite` or a plain number is the iteration count, direction / fill-mode keywords slot in, and the remaining identifier is the @keyframes name. `none` clears the list.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `value` | in | 声明值 |
| `d` | in,out | 目标样式 |

**参见**：`hpe_anim_parse_animation_longhand`

#### `hpe_anim_parse_animation_longhand`

```c
void hpe_anim_parse_animation_longhand(const char *prop, const char *value, HpeStyle *d);
```

解析 animation 长属性

**详细说明**：`prop` 取 "animation-name" / "animation-duration" / "animation-timing-function" / "animation-delay" / "animation-iteration-count" / "animation-direction" / "animation-fill-mode" 之一，值写入 `d->anims[i]` 对应字段并置位 `HPE_PROP2_ANIMATION`。 `prop` is one of "animation-name" / "animation-duration" / "animation-timing-function" / "animation-delay" / "animation-iteration-count" / "animation-direction" / "animation-fill-mode"; the value fills the matching field of `d->anims[i]` and sets `HPE_PROP2_ANIMATION`.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `prop` | in | 长属性名 |
| `value` | in | 声明值 |
| `d` | in,out | 目标样式 |

#### `hpe_anim_parse_time`

```c
void hpe_anim_parse_time(const char *v, float *out);
```

把时间值解析成秒

**详细说明**：接受 `<number>s` 与 `<number>ms`，无单位数字按秒处理；解析失败写 0。 Accepts `<number>s` and `<number>ms`; a bare number counts as seconds and a failed parse writes 0.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 声明值 token |
| `out` | out | 接收秒数 |

#### `hpe_anim_parse_easing`

```c
void hpe_anim_parse_easing(const char *v, HpeEasing *out);
```

解析缓动函数

**详细说明**：关键字 `linear` / `ease` / `ease-in` / `ease-out` / `ease-in-out`、 `cubic-bezier(x1, y1, x2, y2)` 与 `steps(n, start|end)`；未知值回退 `ease`。 Keywords `linear` / `ease` / `ease-in` / `ease-out` / `ease-in-out`, `cubic-bezier(x1, y1, x2, y2)` and `steps(n, start|end)`; unknown values fall back to `ease`.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 声明值 token |
| `out` | out | 接收解析结果 |

#### `hpe_anim_find_keyframes`

```c
HpeKeyframes *hpe_anim_find_keyframes(HpeStylesheet *sheet, const char *name);
```

按名查找样式表中的 @keyframes

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `sheet` | in | 样式表 |
| `name` | in | 动画名 |

**返回**：命中的规则，未找到返回 `NULL`

#### `hpe_anim_sheet_keyframes_slot`

```c
HpeKeyframes *hpe_anim_sheet_keyframes_slot(HpeStylesheet *sheet, const char *name);
```

按名找（或新建）一条空的 @keyframes

**详细说明**：已有同名规则时清空其 stops（后定义整体替换先定义）；否则追加一条空规则。 An existing rule with the same name has its stops cleared (a later definition replaces an earlier one wholesale); otherwise an empty rule is appended.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `sheet` | in,out | 目标样式表 |
| `name` | in | 动画名（不含空白） |

**返回**：命中的规则；容量耗尽返回 `NULL`

#### `hpe_anim_value_get`

```c
int hpe_anim_value_get(const HpeStyle *s, int prop, HpeAnimValue *out);
```

从计算样式取一个可插值属性的当前值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `s` | in | 计算样式 |
| `prop` | in | `HpeAnimProp` / An `HpeAnimProp` id |
| `out` | out | 接收值快照 |

**返回**：属性可参与插值返回 1（`out->valid` 同步置位），否则 0

#### `hpe_anim_value_set`

```c
void hpe_anim_value_set(HpeStyle *s, int prop, const HpeAnimValue *v);
```

把插值结果写回计算样式

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `s` | in,out | 目标样式 |
| `prop` | in | `HpeAnimProp` / An `HpeAnimProp` id |
| `v` | in | 值快照 |

#### `hpe_anim_value_lerp`

```c
int hpe_anim_value_lerp(const HpeAnimValue *a, const HpeAnimValue *b, float t, HpeAnimValue *out);
```

在两个值之间插值

**详细说明**：同类且单位一致时线性插值；颜色按 sRGB 通道插值；transform 做分解 （平移/旋转/缩放）插值，含倾斜或行列式退化时按 50% 离散切换。 Linear interpolation when both sides share type and unit; colours lerp per sRGB channel; transforms interpolate as decomposed (translation / rotation / scale) and fall back to a 50% discrete swap when skew is present or the determinant degenerates.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `a` | in | 起点值 |
| `b` | in | 终点值 |
| `t` | in | 进度 0..1 |
| `out` | out | 接收插值结果 |

**返回**：可插值返回 1，类型不兼容返回 0（调用方应走离散）

#### `hpe_easing_solve`

```c
float hpe_easing_solve(const HpeEasing *e, float t);
```

求缓动函数在 t 处的进度

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `e` | in | 缓动函数 |
| `t` | in | 归一化时间 0..1 |

**返回**：缓动后的进度（通常仍在 0..1，bezier 允许越界）

#### `hpe_anim_prop_is_layout`

```c
int hpe_anim_prop_is_layout(int prop);
```

判断属性是否影响几何（决定动画刷新级别）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `prop` | in | `HpeAnimProp` / An `HpeAnimProp` id |

**返回**：需要重新布局返回 1，仅需重算样式返回 0

**类型**

无。

### hpe_css.h

14 个函数，51 个类型。

**函数**

#### `hpe_css_init`

```c
void hpe_css_init(HpeStylesheet *sheet);
```

初始化样式表并载入内置 UA 样式

**详细说明**：先把整个结构清零，再把视口默认设为 800x600，最后解析 `hpe_ua_stylesheet()` 的文本，因此新样式表一开始就带有浏览器默认样式。 The struct is zeroed, the viewport defaults to 800x600, and the text from `hpe_ua_stylesheet()` is parsed, so a fresh sheet already carries the user-agent defaults.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `sheet` | in,out | 目标样式表 |

**注意**：传 `NULL` 时直接返回

**参见**：`hpe_css_free`、`hpe_css_parse`

#### `hpe_css_free`

```c
void hpe_css_free(HpeStylesheet *sheet);
```

释放样式表持有的全部规则

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `sheet` | in,out | 目标样式表 |

**注意**：逐条释放规则后把整个结构清零，可安全重复调用

**参见**：`hpe_css_init`

#### `hpe_css_parse`

```c
void hpe_css_parse(HpeStylesheet *sheet, const char *css, size_t length);
```

解析一段 CSS 文本并追加到样式表

**详细说明**：在栈上分配一个媒体查询栈后调用 `css_parse_into` 从第 0 层开始递归解析，规则按出现顺序追加，不会清空已有规则。 A media-query stack is allocated on the stack and `css_parse_into` recurses from depth 0; rules are appended in source order and existing rules are kept.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `sheet` | in,out | 目标样式表 |
| `css` | in | 待解析的 CSS 文本 |
| `length` | in | `css` 的字节长度 |

**参见**：`css_parse_into`、`hpe_css_init`

#### `hpe_css_set_viewport`

```c
void hpe_css_set_viewport(HpeStylesheet *sheet, float vw, float vh);
```

设置用于求值 @media 的视口尺寸

**详细说明**：只写入宽高两个字段，不重新解析样式表；规则的 `@media` 条件在层叠时按此尺寸即时求值，因此在重新层叠前调用即可让媒体查询随窗口变化生效。 Only the width and height fields are written and the sheet is not re-parsed; each rule's `@media` conditions are evaluated against this size at cascade time, so calling it before a re-cascade makes media queries follow the window.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `sheet` | in,out | 目标样式表 |
| `vw` | in | 视口宽度（像素） |
| `vh` | in | 视口高度（像素） |

**注意**：传 `NULL` 时直接返回

**参见**：`hpe_css_compute`

#### `hpe_css_selector_matches`

```c
int hpe_css_selector_matches(const HpeDomNode *node, const char *sel, size_t len);
```

测试单个选择器是否匹配给定节点

**详细说明**：把 `sel` 当作可能含逗号的完整选择器文本解析成规则，任一选择器命中即算匹配；组合符会沿节点的祖先与兄弟链回溯。 `sel` is parsed as a full selector list that may contain commas and a hit on any one selector counts as a match; combinators walk the node's ancestors and siblings.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in | 待测试的 DOM 节点 |
| `sel` | in | 选择器文本（可含逗号） |
| `len` | in | `sel` 的字节长度 |

**返回**：匹配返回 1，否则返回 0

**注意**：供 `querySelector` 一类接口使用

**参见**：`selector_matches`、`compound_matches`

#### `hpe_style_default`

```c
HpeStyle hpe_style_default(void);
```

返回一份填好 CSS 初始值的默认样式

**详细说明**：先 `memset` 清零，再依次调用 `style_init_box` 与 `style_init_scalars`，因此所有非零初值（如 `auto`、透明色、单位变换、`font-size: 16px`）都已就位。 The struct is zeroed first, then `style_init_box` and `style_init_scalars` fill in every non-zero initial value such as `auto`, transparent colours, the identity transform and `font-size: 16px`.

**返回**：初始化完毕的样式值（按值返回）

**参见**：`style_init_box`、`style_init_scalars`

#### `hpe_css_parse_declarations`

```c
void hpe_css_parse_declarations(const char *text, size_t length, HpeStyle *decl);
```

解析声明块并写入样式

**详细说明**：输入通常是 `style=""` 属性的内容；按 `;` 切分后逐条解析 `属性: 值`，识别并记录 `!important`，非法或空声明被忽略。 The input is normally the content of a `style=""` attribute; it is split on `;` and each `property: value` pair is parsed, `!important` is detected and recorded, and malformed or empty declarations are skipped.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `text` | in | 声明块文本 |
| `length` | in | `text` 的字节长度 |
| `decl` | out | 接收解析结果的样式 |

**注意**：进入时先清空 `decl`；`text` 为空时只做清零

**参见**：`apply_property`、`strip_important`

#### `hpe_css_style_custom_value`

```c
int hpe_css_style_custom_value(const HpeStyle *style, const char *name, char *out, size_t n);
```

读取计算样式中的自定义属性值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `style` | in | 计算样式 |
| `name` | in | 属性名（可带或不带 `--`） |
| `out` | out | 接收值的缓冲 |
| `n` | in | 缓冲容量 |

**返回**：找到返回 1，否则 0

#### `hpe_css_compute`

```c
void hpe_css_compute(HpeStylesheet *sheet, const HpeDomNode *node, const HpeStyle *parent, HpeStyle *out);
```

计算节点的最终样式（两趟层叠）

**详细说明**：第一趟从 `hpe_style_default` 出发并按 `parent` 继承可继承属性，收集所有媒体条件通过且选择器匹配的规则，按（specificity, 源码顺序）排序后依次应用，最后叠加内联 `style=""`（胜过普通样式表声明）。 Pass one starts from `hpe_style_default`, inherits the inheritable properties from `parent`, gathers every rule whose media conditions pass and whose selectors match, applies them sorted by (specificity, source order), and finally overlays the inline `style=""`, which beats a normal stylesheet declaration. 第二趟只处理 `!important` 声明：把它们（含内联）按同一种键排序后仅应用被标记为 important 的位；在 important 层内内联仍胜过样式表，其余由源码顺序决定。 Pass two handles `!important` declarations only: they are sorted by the same key and only the bits flagged important are applied; within the important origin an inline `style=""` still beats a stylesheet, otherwise source order decides.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `sheet` | in,out | 样式表；为 `NULL` 时跳过规则匹配 |
| `node` | in | 待计算样式的 DOM 节点 |
| `parent` | in | 父节点样式，用于继承；可为 `NULL` |
| `out` | out | 接收计算后的样式 |

**注意**：计算得到的样式保留“已声明”位（`set`/`set2`），供绘制阶段区分显式值与继承值

**参见**：`apply_decl`、`apply_decl_important`、`inherit_parent_style`

#### `hpe_css_compute_pseudo`

```c
void hpe_css_compute_pseudo(HpeStylesheet *sheet, const HpeDomNode *node, const HpeStyle *host_style, int which, HpeStyle *out);
```

计算 `::before` / `::after` 伪元素的计算样式

**详细说明**：从宿主元素的计算样式出发，套用所有选择器命中（末段带同向伪元素）的规则，并叠加宿主元素内联 `style` 的可继承部分；结果保留「已声明」位。 Starting from the host element's computed style, every rule whose selectors end in the matching pseudo-element is applied, and the inheritable part of the host's inline `style` is overlaid; the "declared" bits are kept.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `sheet` | in,out | 样式表；为 `NULL` 时只用宿主样式 |
| `node` | in | 宿主元素 |
| `host_style` | in | 宿主元素的计算样式 |
| `which` | in | `HPE_PE_BEFORE` 或 `HPE_PE_AFTER` |
| `out` | out | 接收计算后的样式 |

**注意**：只有末段复合选择器带有同向伪元素的选择器才参与匹配

**参见**：`hpe_css_compute`

#### `hpe_ua_stylesheet`

```c
const char *hpe_ua_stylesheet(void);
```

返回内置的用户代理默认样式表文本

**详细说明**：这份常量文本为 `html`/`body`、标题、列表、表格、表单控件等元素提供 `display`、`margin`、`font-size` 等浏览器默认值，在 `hpe_css_init` 时被解析进样式表。 This constant text supplies browser-default `display`, `margin`, `font-size` and similar values for `html`/`body`, headings, lists, tables and form controls; it is parsed into the sheet by `hpe_css_init`.

**返回**：以 NUL 结尾的 UA 样式表 CSS 文本

**参见**：`hpe_css_init`

#### `hpe_css_inline_set`

```c
void hpe_css_inline_set(HpeDomNode *node, const char *prop, const char *value);
```

在元素的内联 `style` 属性中设置一个属性

**详细说明**：扫描现有 `style` 属性，遇到同名属性就替换其值（值为空则删除），其余声明原样保留；没有同名属性时在末尾追加 `prop: value;`，最后整体写回属性。 The existing `style` attribute is scanned; a declaration with the same name has its value replaced (or removed when the new value is empty) while other declarations are kept verbatim, and when no match exists `prop: value;` is appended before writing the attribute back.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in,out | 目标 DOM 元素 |
| `prop` | in | CSS 属性名（小写、带连字符） |
| `value` | in | 属性值；空串或 `NULL` 表示删除该属性 |

**注意**：供 JavaScript DOM 桥使用

**参见**：`hpe_css_inline_get`、`inline_append`

#### `hpe_css_inline_get`

```c
int hpe_css_inline_get(const HpeDomNode *node, const char *prop, char *out, size_t out_size);
```

读取元素内联 `style` 属性中的一个属性值

**详细说明**：按 `;` 切分内联样式，逐个比较小写的属性名，命中后把去除首尾空白的值写入 `out` 并返回 1。 The inline style is split on `;` and property names are compared case-insensitively; on a hit the trimmed value is written to `out` and 1 is returned.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in | 目标 DOM 元素 |
| `prop` | in | 要查询的 CSS 属性名 |
| `out` | out | 接收属性值的缓冲区 |
| `out_size` | in | 接收缓冲区容量（字节） |

**返回**：找到返回 1，否则返回 0

**注意**：属性值以去空白后的文本原样返回，不做规范化

**参见**：`hpe_css_inline_set`

#### `hpe_css_set_anim_hook`

```c
void hpe_css_set_anim_hook(HpeCssAnimHook fn, void *ud);
```

注册层叠后的动画覆盖钩子

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `fn` | in | 钩子函数，`NULL` 表示注销 |
| `ud` | in | 透传给钩子的用户数据 |

**注意**：`hpe_css_compute` 在元素节点层叠完成后调用钩子，供动画时钟把插值结果写进计算样式； 由引擎在创建时注册（见 hope_anim.c）

**参见**：`hpe_css_compute`

**类型**

#### `HpeDisplay`

```c
typedef enum HpeDisplay {
    HPE_DISPLAY_INLINE = 0,
    HPE_DISPLAY_BLOCK,
    HPE_DISPLAY_NONE,
    HPE_DISPLAY_INLINE_BLOCK,
    HPE_DISPLAY_FLEX,
    HPE_DISPLAY_GRID,            /* grid container */
    HPE_DISPLAY_TABLE,           /* the table wrapper/root */
    HPE_DISPLAY_TABLE_ROW,       /* <tr> */
    HPE_DISPLAY_TABLE_CELL,      /* <td> / <th> */
    HPE_DISPLAY_TABLE_ROW_GROUP, /* <thead> / <tbody> / <tfoot> */
    HPE_DISPLAY_TABLE_CAPTION
} HpeDisplay;
```

#### `HpeFlexDir`

```c
typedef enum HpeFlexDir {
    HPE_FLEX_ROW = 0,
    HPE_FLEX_COLUMN
} HpeFlexDir;
```

#### `HpeAlignItems`

```c
typedef enum HpeAlignItems {
    HPE_ALIGN_ITEMS_STRETCH = 0,
    HPE_ALIGN_ITEMS_FLEX_START,
    HPE_ALIGN_ITEMS_CENTER,
    HPE_ALIGN_ITEMS_FLEX_END,
    HPE_ALIGN_ITEMS_BASELINE,
    HPE_ALIGN_ITEMS_AUTO
} HpeAlignItems;
```

#### `HpeJustify`

```c
typedef enum HpeJustify {
    HPE_JUSTIFY_START = 0,
    HPE_JUSTIFY_CENTER,
    HPE_JUSTIFY_END,
    HPE_JUSTIFY_SPACE_BETWEEN,
    HPE_JUSTIFY_SPACE_AROUND
} HpeJustify;
```

#### `HpePosition`

```c
typedef enum HpePosition {
    HPE_POS_STATIC = 0,
    HPE_POS_RELATIVE,
    HPE_POS_ABSOLUTE,
    HPE_POS_FIXED
} HpePosition;
```

#### `HpeFloatSide`

```c
typedef enum HpeFloatSide {
    HPE_FLOAT_NONE = 0,
    HPE_FLOAT_LEFT,
    HPE_FLOAT_RIGHT
} HpeFloatSide;
```

#### `HpeClearSide`

```c
typedef enum HpeClearSide {
    HPE_CLEAR_NONE = 0,
    HPE_CLEAR_LEFT,
    HPE_CLEAR_RIGHT,
    HPE_CLEAR_BOTH
} HpeClearSide;
```

#### `HpeTextAlign`

```c
typedef enum HpeTextAlign {
    HPE_ALIGN_LEFT = 0,
    HPE_ALIGN_CENTER,
    HPE_ALIGN_RIGHT
} HpeTextAlign;
```

#### `HpeWhiteSpace`

```c
typedef enum HpeWhiteSpace {
    HPE_WS_NORMAL = 0,
    HPE_WS_NOWRAP,
    HPE_WS_PRE,
    HPE_WS_PRE_WRAP,
    HPE_WS_PRE_LINE
} HpeWhiteSpace;
```

#### `HpeBoxSizing`

```c
typedef enum HpeBoxSizing {
    HPE_BOX_SIZING_CONTENT = 0,
    HPE_BOX_SIZING_BORDER
} HpeBoxSizing;
```

#### `HpeOverflow`

```c
typedef enum HpeOverflow {
    HPE_OVERFLOW_VISIBLE = 0,
    HPE_OVERFLOW_HIDDEN,
    HPE_OVERFLOW_SCROLL,
    HPE_OVERFLOW_AUTO
} HpeOverflow;
```

#### `HpeVisibility`

```c
typedef enum HpeVisibility {
    HPE_VISIBILITY_VISIBLE = 0,
    HPE_VISIBILITY_HIDDEN,
    HPE_VISIBILITY_COLLAPSE
} HpeVisibility;
```

#### `HpeTextDecoration`

```c
typedef enum HpeTextDecoration {
    HPE_TD_NONE = 0,
    HPE_TD_UNDERLINE = 1 << 0,
    HPE_TD_OVERLINE = 1 << 1,
    HPE_TD_LINE_THROUGH = 1 << 2
} HpeTextDecoration;
```

#### `HpeListStyleType`

```c
typedef enum HpeListStyleType {
    HPE_LIST_DISC = 0,
    HPE_LIST_CIRCLE,
    HPE_LIST_SQUARE,
    HPE_LIST_DECIMAL,
    HPE_LIST_NONE
} HpeListStyleType;
```

#### `HpeListStylePosition`

```c
typedef enum HpeListStylePosition {
    HPE_LIST_OUTSIDE = 0,
    HPE_LIST_INSIDE
} HpeListStylePosition;
```

#### `HpeVerticalAlign`

```c
typedef enum HpeVerticalAlign {
    HPE_VALIGN_BASELINE = 0,
    HPE_VALIGN_MIDDLE,
    HPE_VALIGN_TOP,
    HPE_VALIGN_BOTTOM,
    HPE_VALIGN_TEXT_TOP,
    HPE_VALIGN_TEXT_BOTTOM
} HpeVerticalAlign;
```

#### `HpeGradientStop`

```c
typedef struct HpeGradientStop {
    float pos; /* 0..1 */
    HpeColor color;
} HpeGradientStop;
```

#### `HpeBackgroundKind`

```c
typedef enum HpeBackgroundKind {
    HPE_BG_NONE = 0,
    HPE_BG_LINEAR_GRADIENT,
    HPE_BG_URL /* an external image referenced by `background-image: url(...)` */
} HpeBackgroundKind;
```

#### `HpeObjectFit`

```c
typedef enum HpeObjectFit {
    HPE_OBJECT_FIT_FILL = 0,
    HPE_OBJECT_FIT_CONTAIN,
    HPE_OBJECT_FIT_COVER,
    HPE_OBJECT_FIT_NONE,
    HPE_OBJECT_FIT_SCALE_DOWN
} HpeObjectFit;
```

#### `HpeBgRepeat`

```c
typedef enum HpeBgRepeat {
    HPE_BG_REPEAT_REPEAT = 0,
    HPE_BG_REPEAT_X,
    HPE_BG_REPEAT_Y,
    HPE_BG_REPEAT_NO_REPEAT
} HpeBgRepeat;
```

#### `HpeBackground`

```c
typedef struct HpeBackground {
    HpeBackgroundKind kind;
    float angle_deg; /* 0 = to top, 90 = to right (css `to` direction mapped to degrees) */
    HpeGradientStop stops[HPE_MAX_GRADIENT_STOPS];
    int stop_count;
    char url[HPE_BG_URL_MAX]; /* `kind == HPE_BG_URL`: the raw `url()` argument */
} HpeBackground;
```

#### `HpeShadow`

```c
typedef struct HpeShadow {
    float ox, oy, blur, spread;
    HpeColor color;
    int valid;
} HpeShadow;
```

#### `HpeTransform`

```c
typedef struct HpeTransform {
    float a, b, c, d, e, f;
    int identity;
} HpeTransform;
```

#### `HpeCursor`

```c
typedef enum HpeCursor {
    HPE_CURSOR_AUTO = 0,
    HPE_CURSOR_DEFAULT,
    HPE_CURSOR_POINTER,
    HPE_CURSOR_TEXT,
    HPE_CURSOR_MOVE,
    HPE_CURSOR_WAIT,
    HPE_CURSOR_HELP,
    HPE_CURSOR_CROSSHAIR,
    HPE_CURSOR_NOT_ALLOWED,
    HPE_CURSOR_GRAB,
    HPE_CURSOR_GRABBING,
    HPE_CURSOR_EW_RESIZE,
    HPE_CURSOR_NS_RESIZE
} HpeCursor;
```

#### `HpeBgPos`

```c
typedef struct HpeBgPos {
    float x, y;      /* position: px, or a percentage when `x_pct`/`y_pct` */
    int x_pct, y_pct;
    float size_x, size_y; /* size: px, or a percentage when `sx_pct`/`sy_pct` */
    int sx_pct, sy_pct;
    int size_auto;        /* 1 = `background-size: auto` (use the intrinsic size) */
    int size_cover, size_contain;
    int present;
} HpeBgPos;
```

#### `HpeGridTrack`

```c
typedef struct HpeGridTrack {
    int kind; /* 0 = px, 1 = fr, 2 = auto, 3 = % */
    float value;
} HpeGridTrack;
```

#### `HpeGridSpec`

```c
typedef struct HpeGridSpec {
    HpeGridTrack tracks[HPE_GRID_MAX_TRACKS];
    int count;
    int present;
} HpeGridSpec;
```

#### `HpeCombinator`

```c
typedef enum HpeCombinator {
    HPE_COMB_DESCENDANT = 0, /* (space) */
    HPE_COMB_CHILD,          /* > */
    HPE_COMB_ADJ_SIBLING,    /* + */
    HPE_COMB_GEN_SIBLING     /* ~ */
} HpeCombinator;
```

#### `HpeStyle`

```c
typedef struct HpeStyle HpeStyle; /* HpeKeyframeStop below holds one by pointer */
```

#### `HpeAnimProp`

```c
typedef enum HpeAnimProp {
    HPE_AP_NONE = 0,
    HPE_AP_COLOR,
    HPE_AP_BACKGROUND_COLOR,
    HPE_AP_BORDER_COLOR, /* 四边一起插值 / all four sides together */
    HPE_AP_OPACITY,
    HPE_AP_WIDTH,
    HPE_AP_HEIGHT,
    HPE_AP_MARGIN_TOP, HPE_AP_MARGIN_RIGHT, HPE_AP_MARGIN_BOTTOM, HPE_AP_MARGIN_LEFT,
    HPE_AP_PADDING_TOP, HPE_AP_PADDING_RIGHT, HPE_AP_PADDING_BOTTOM, HPE_AP_PADDING_LEFT,
    HPE_AP_INSET_TOP, HPE_AP_INSET_RIGHT, HPE_AP_INSET_BOTTOM, HPE_AP_INSET_LEFT,
    HPE_AP_FONT_SIZE,
    HPE_AP_LETTER_SPACING,
    HPE_AP_LINE_HEIGHT,
    HPE_AP_FONT_WEIGHT,
    HPE_AP_BORDER_RADIUS,
    HPE_AP_TRANSFORM,
    HPE_AP_ALL, /* 仅 `transition-property` 使用 / `transition-property` only */
    HPE_AP_COUNT
} HpeAnimProp;
```

#### `HpeAnimValue`

```c
typedef struct HpeAnimValue {
    int prop;  /* HpeAnimProp */
    int type;  /* HPE_AV_* */
    int valid; /* 0 when the property holds no interpolable value (e.g. auto) */
    HpeLength len;
    HpeColor color;
    HpeColor colors[4];
    float f;
    HpeTransform tf;
} HpeAnimValue;
```

#### `HpeEasingKind`

```c
typedef enum HpeEasingKind {
    HPE_EASING_LINEAR = 0,
    HPE_EASING_EASE,
    HPE_EASING_EASE_IN,
    HPE_EASING_EASE_OUT,
    HPE_EASING_EASE_IN_OUT,
    HPE_EASING_CUBIC,
    HPE_EASING_STEPS
} HpeEasingKind;
```

#### `HpeEasing`

```c
typedef struct HpeEasing {
    int kind; /* HpeEasingKind */
    float x1, y1, x2, y2; /* HPE_EASING_CUBIC control points */
    int steps;            /* HPE_EASING_STEPS interval count */
    int steps_start;      /* 1 = steps(n, start) */
} HpeEasing;
```

#### `HpeTransition`

```c
typedef struct HpeTransition {
    int prop; /* HpeAnimProp, or HPE_AP_ALL */
    float duration, delay; /* 秒 / seconds */
    HpeEasing easing;
} HpeTransition;
```

#### `HpeAnimDecl`

```c
typedef struct HpeAnimDecl {
    char name[HPE_ANIM_NAME_MAX]; /* @keyframes 名（"" = 未声明）/ keyframes name */
    float duration, delay;        /* 秒 / seconds */
    float iter_count;             /* 次数；`iter_infinite` 时忽略 / loop count */
    int iter_infinite;            /* `infinite` */
    int direction;                /* HPE_ANIM_DIR_* */
    int fill;                     /* HPE_ANIM_FILL_* */
    HpeEasing easing;
} HpeAnimDecl;
```

#### `HpeKeyframeStop`

```c
typedef struct HpeKeyframeStop {
    float pct; /* 0..100 */
    HpeStyle *decl;
} HpeKeyframeStop;
```

#### `HpeKeyframes`

```c
typedef struct HpeKeyframes {
    char name[HPE_ANIM_NAME_MAX];
    HpeKeyframeStop stops[HPE_KF_STOPS_MAX];
    int stop_count;
} HpeKeyframes;
```

#### `HpeCustomProp`

```c
typedef struct HpeCustomProp {
    char name[HPE_CUSTOM_NAME_MAX];  /* name without the leading `--` */
    char value[HPE_CUSTOM_VALUE_MAX]; /* raw token stream, may itself contain var() */
    int important;
    int declared; /* set by this element's cascade, 0 when merely inherited */
} HpeCustomProp;
```

#### `HpeVarDecl`

```c
typedef struct HpeVarDecl {
    char prop[HPE_CUSTOM_NAME_MAX];   /* property name receiving the var() value */
    char value[HPE_CUSTOM_VALUE_MAX]; /* raw value containing var() */
} HpeVarDecl;
```

#### `HpePropertyReg`

```c
typedef struct HpePropertyReg {
    char name[HPE_CUSTOM_NAME_MAX];
    char syntax[48];
    char initial[HPE_CUSTOM_VALUE_MAX];
    int inherits;
} HpePropertyReg;
```

#### `HpeStyle`

```c
typedef struct HpeStyle {
    uint64_t set;
    uint64_t set2; /* second property word, see HPE_PROP2_* */
    uint64_t imp;  /* important bits for `set` */
    uint64_t imp2; /* important bits for `set2` */
    HpeDisplay display;
    HpeColor color;
    HpeColor background;
    HpeColor border_color;            /* shorthand colour for all four sides */
    HpeColor border_colors[4];        /* per-side, top right bottom left */
    HpeLength width;
    HpeLength height;
    HpeLength margin[4];  /* top right bottom left */
    HpeLength padding[4]; /* top right bottom left */
    float border_w[4];    /* top right bottom left */
    float border_radius;
    float font_size;
    int font_weight;
    HpeTextAlign text_align;
    float line_height;
    HpeWhiteSpace white_space;
    HpeFontFamily family;
    HpePosition position;
    HpeLength inset[4];   /* top, right, bottom, left */
    int z_index;
    int z_auto;
    HpeFloatSide float_side;
    HpeClearSide clear_side;
    int flex_dir;
    int align_items;
    int justify;
    int align_self;  /* HPE_ALIGN_ITEMS_AUTO when unset */
    float gap;
    float flex_grow;
    HpeLength min_width;
    HpeLength min_height;
    HpeLength max_width;
    HpeLength max_height;
    HpeBoxSizing box_sizing;
    HpeOverflow overflow_x;
    HpeOverflow overflow_y;
    float opacity;
    HpeVisibility visibility;
    int cursor; /* HpeCursor id, parsed but not yet applied by the chrome */
    HpeTextDecoration text_decoration;
    HpeLength letter_spacing; /* may be negative; `auto` means `normal` (0) */
    HpeLength word_spacing;
    HpeLength text_indent;
    HpeVerticalAlign vertical_align;
    HpeListStyleType list_style_type;
    HpeListStylePosition list_style_position;
    HpeBackground background_image;
    HpeShadow box_shadow;
    HpeShadow text_shadow;
    HpeTransform transform;
    /* transition declarations (comma list, HPE_TRANS_MAX entries) */
    HpeTransition transitions[HPE_TRANS_MAX];
    int transition_count;
    /* animation declarations (comma list, HPE_ANIM_MAX entries) */
    HpeAnimDecl anims[HPE_ANIM_MAX];
    int anim_count;
    /* generated content, resolved to text for ::before / ::after */
    char content[HPE_CONTENT_MAX];
    char attr_content[96]; /* `attr(name)` argument of `content`, resolved at box build */
    int has_content;
    /* flex item longhands */
    int flex_wrap;         /* 0 = nowrap, 1 = wrap, 2 = wrap-reverse */
    int order;
    HpeLength flex_basis;
    float flex_shrink;
    int row_reverse;       /* flex-direction: *-reverse */
    /* background placement (background-position / background-size) */
    HpeBgPos bg_pos;
    /* radial gradient parameters (kind stays HPE_BG_LINEAR_GRADIENT for linear) */
    int bg_radial;         /* 1 when `background-image` was a radial-gradient */
    int bg_radial_circle;  /* 1 for `circle`, 0 for `ellipse` */
    float bg_radial_cx, bg_radial_cy, bg_radial_rx, bg_radial_ry; /* 0..1 relative */
    /* grid container specs */
    HpeGridSpec grid_cols;
    HpeGridSpec grid_rows;
    float grid_gap;
    /* grid item placement (`grid-column` / `grid-row`, 1-based; 0 = auto,
     * a negative value -n means `span n`) */
    int grid_col_start, grid_col_end;
    int grid_row_start, grid_row_end;
    int justify_items;   /* HpeAlignItems values, applied to grid items */
    int align_content;   /* HpeJustify values, applied to the whole grid */
    int grid_auto_flow;  /* 0 = row, 1 = column */
    int table_layout_fixed;
    float border_spacing_h, border_spacing_v;
    int direction_rtl;
    float scrollbar_width;
    /* replaced-element content fitting and external background-image tiling */
    HpeObjectFit object_fit;
    HpeBgPos object_pos; /* `object-position`, shares HpeBgPos' px / % form */
    HpeBgRepeat bg_repeat;
    /* vertical text (CSS Writing Modes): 0 = horizontal-tb, 1 = vertical-rl,
     * 2 = vertical-lr / 竖排文本：0 横排，1 从右向左竖排，2 从左向右竖排 */
    int writing_mode;
    /* CSS custom properties and pending var() declarations (v0.23) */
    HpeCustomProp custom[HPE_CUSTOM_PROP_MAX];
    int custom_count;
    HpeVarDecl vars[HPE_VAR_DECL_MAX];
    int var_count;
} HpeStyle;
```

#### `HpeAttrSel`

```c
typedef struct HpeAttrSel {
    char *name;
    char *op;   /* "", "=", "~=", "|=", "^=", "$=", "*=" */
    char *value;
} HpeAttrSel;
```

#### `HpeMediaAlt`

```c
typedef struct HpeMediaAlt {
    int negate;    /* leading `not` */
    int type;      /* HPE_MTYPE_* */
    int unknown;   /* an unrecognised type/feature forces a mismatch */
    int cond_count;
    struct {
        int feature; /* HPE_MF_* */
        float value; /* px */
    } conds[HPE_MEDIA_MAX_COND];
} HpeMediaAlt;
```

#### `HpeMediaQuery`

```c
typedef struct HpeMediaQuery {
    int present; /* 0 = the rule is unconditional */
    int alt_count;
    HpeMediaAlt alts[HPE_MEDIA_MAX_ALT];
} HpeMediaQuery;
```

#### `HpePseudoElement`

```c
typedef enum HpePseudoElement {
    HPE_PE_NONE = 0,
    HPE_PE_BEFORE,
    HPE_PE_AFTER
} HpePseudoElement;
```

#### `HpeNth`

```c
typedef struct HpeNth {
    int a; /* step; `odd` = 2, `even` = 2 with b = 0/1 */
    int b; /* offset */
    int present;
} HpeNth;
```

#### `HpeSelector`

```c
typedef struct HpeSelector HpeSelector;
```

#### `HpeCompound`

```c
typedef struct HpeCompound {
    char *tag; /* NULL or "*" means any element */
    char *id;
    char **classes;
    int class_count;
    size_t class_cap;
    HpeAttrSel *attrs;
    int attr_count;
    size_t attr_cap;
    unsigned pseudo; /* HPE_PS_* bits */
    HpeNth nth_child;      /* :nth-child(an+b) */
    HpeNth nth_last_child; /* :nth-last-child(an+b) */
    HpeNth nth_of_type;    /* :nth-of-type(an+b) */
    HpeNth nth_last_of_type; /* :nth-last-of-type(an+b) */
    HpeSelector *not_list;   /* :not(...) argument (any match fails) */
    int not_count;
    HpeSelector *is_list;    /* :is(...) / :where(...) argument */
    int is_count;
    int is_where;            /* 1 for `:where()`, which contributes no specificity */
    int pseudo_element;      /* HpePseudoElement */
    int comb; /* HpeCombinator for this part (relative to the previous part) */
} HpeCompound;
```

#### `HpeRule`

```c
typedef struct HpeRule {
    HpeSelector *selectors;
    int selector_count;
    size_t selector_cap;
    HpeStyle *decls;
    int decl_count;
    size_t decl_cap;
    int order;
    /* conditions from enclosing @media blocks (all must match); 0 = unconditional */
    HpeMediaQuery mq[HPE_MEDIA_MAX_DEPTH];
    int mq_depth;
} HpeRule;
```

#### `HpeStylesheet`

```c
typedef struct HpeStylesheet {
    HpeRule *rules;
    int rule_count;
    size_t rule_cap;
    int next_order;
    float vw; /* viewport width/height used to evaluate @media at cascade time */
    float vh;
    /* @keyframes store (heap, grown on demand; freed by hpe_css_free) */
    HpeKeyframes *kf;
    int kf_count;
    size_t kf_cap;
    /* @property registrations (heap, grown on demand; freed by hpe_css_free) */
    HpePropertyReg *props;
    int prop_count;
    size_t prop_cap;
} HpeStylesheet;
```

#### `HpeCssAnimHook`

```c
typedef void (*HpeCssAnimHook)(void *ud, const HpeDomNode *node, HpeStyle *style);
```

## src/dom（DOM 树）

### hpe_dom.h

21 个函数，4 个类型。

**函数**

#### `hpe_dom_children_root`

```c
HpeDomNode *hpe_dom_children_root(HpeDomNode *node);
```

取得节点子内容的容器

**详细说明**：`<template>` 的子节点存放在分离的 DocumentFragment 中（规范里的 `content`），解析器、`innerHTML` 与 `textContent` 都经此函数读写。 A `<template>`'s children live in a detached DocumentFragment (the spec's `content`); the parser, `innerHTML` and `textContent` all read and write through this function.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in,out | DOM 节点 |

**返回**：`<template>` 返回其内容片段，其余节点返回自身

**注意**：传入 `NULL` 返回 `NULL`；首次访问 `<template>` 时按需创建 content 片段

**参见**：`hpe_dom_append`

#### `hpe_dom_element`

```c
HpeDomNode *hpe_dom_element(const char *tag);
```

创建一个元素节点

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `tag` | in | 标签名（`NULL` 视为 `div`） |

**返回**：新的元素节点

#### `hpe_dom_text`

```c
HpeDomNode *hpe_dom_text(const char *text);
```

创建一个文本节点

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `text` | in | 文本内容（`NULL` 视为空串） |

**返回**：新的文本节点

#### `hpe_dom_set_text`

```c
void hpe_dom_set_text(HpeDomNode *node, const char *text);
```

设置文本节点的内容

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in,out | 文本节点（其他类型直接返回） |
| `text` | in | 新内容（`NULL` 视为空串） |

**注意**：以 `HPE_MUT_TEXT` 报告一次变更，使 `characterData` 观察者与引擎脏标记都能看到

**参见**：`hpe_dom_text`

#### `hpe_dom_document`

```c
HpeDomNode *hpe_dom_document(void);
```

创建一个文档节点

**返回**：新的文档节点

#### `hpe_dom_fragment`

```c
HpeDomNode *hpe_dom_fragment(void);
```

创建一个文档片段节点

**返回**：新的片段节点

**注意**：片段本身不属于树；把它追加到父节点时会搬移其子节点

#### `hpe_dom_append`

```c
void hpe_dom_append(HpeDomNode *parent, HpeDomNode *child);
```

把子节点追加到父节点末尾

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `parent` | in,out | 父节点 |
| `child` | in,out | 待插入的子节点；若为文档片段则按序搬移其所有子节点并释放空片段 |

**注意**：先解除子节点与原父节点的关联；插入成功后报告一次子节点列表变更

**参见**：`hpe_dom_insert_before`、`hpe_dom_detach`

#### `hpe_dom_detach`

```c
void hpe_dom_detach(HpeDomNode *node);
```

把节点从其父节点上摘下

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in,out | DOM 节点 |

**注意**：只更新兄弟链与父指针，不释放节点；未挂载的节点直接返回

**参见**：`hpe_dom_append`、`hpe_dom_free`

#### `hpe_dom_insert_before`

```c
void hpe_dom_insert_before(HpeDomNode *parent, HpeDomNode *child, HpeDomNode *ref);
```

在参照节点之前插入子节点

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `parent` | in,out | 父节点 |
| `child` | in,out | 待插入的子节点；若为文档片段则按序搬移其所有子节点并释放空片段 |
| `ref` | in,out | 参照节点；为 `NULL` 或不是 `parent` 的子节点时改为追加 |

**注意**：先解除 `child` 与原父节点的关联；`child == ref` 时不做任何事

**参见**：`hpe_dom_append`、`hpe_dom_replace_child`

#### `hpe_dom_replace_child`

```c
HpeDomNode *hpe_dom_replace_child(HpeDomNode *parent, HpeDomNode *new_child, HpeDomNode *old_child);
```

用一个节点替换另一个子节点

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `parent` | in,out | 父节点 |
| `new_child` | in,out | 新节点；为 `NULL` 时仅移除旧节点 |
| `old_child` | in,out | 被替换的子节点，必须是 `parent` 的子节点 |

**返回**：被摘下的旧节点（调用方接管）或 `NULL`

**注意**：返回值可能是 `old_child` 自身；`new_child == old_child` 时原样返回

**参见**：`hpe_dom_insert_before`、`hpe_dom_detach`

#### `hpe_dom_clone`

```c
HpeDomNode *hpe_dom_clone(const HpeDomNode *node, int deep);
```

克隆一个节点（可深度克隆）

**详细说明**：元素克隆会复制标签与全部属性；文本、文档、片段按其类型重建。 An element clone keeps its tag and every attribute; text, document and fragment nodes are rebuilt according to their type.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in | 被克隆的节点，可为 `NULL` |
| `deep` | in | 深度克隆为 1 |

**返回**：新的克隆节点，输入为 `NULL` 时返回 `NULL`

**注意**：`deep` 非 0 时递归克隆整个子树；浅拷贝的副本没有父节点、兄弟或子节点

#### `hpe_dom_attr`

```c
const char *hpe_dom_attr(const HpeDomNode *node, const char *name);
```

读取元素上的属性值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in | DOM 节点 |
| `name` | in | 属性名，大小写不敏感 |

**返回**：属性值（内部存储，调用方不得释放）或 `NULL`

**注意**：无该属性或参数为 `NULL` 时返回 `NULL`

#### `hpe_dom_set_attr`

```c
void hpe_dom_set_attr(HpeDomNode *node, const char *name, const char *value);
```

设置元素上的属性

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in,out | DOM 节点 |
| `name` | in | 名称 |
| `value` | in | 属性值（`NULL` 视为空串） |

**注意**：属性已存在时覆盖其值，否则新增；两种情况都会报告一次属性变更

#### `hpe_dom_remove_attr`

```c
void hpe_dom_remove_attr(HpeDomNode *node, const char *name);
```

移除元素上的属性

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in,out | DOM 节点 |
| `name` | in | 属性名，大小写不敏感 |

**注意**：属性不存在时不做任何事；成功移除后报告一次属性变更

#### `hpe_dom_prev_sibling`

```c
HpeDomNode *hpe_dom_prev_sibling(const HpeDomNode *node);
```

取得节点的前一个兄弟节点

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in | DOM 节点 |

**返回**：父节点下紧邻的前一个兄弟，无则 `NULL`

**注意**：无父节点或是首个子节点时返回 `NULL`

#### `hpe_dom_find_tag`

```c
HpeDomNode *hpe_dom_find_tag(HpeDomNode *root, const char *tag);
```

按文档顺序查找首个指定标签的元素

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `root` | in,out | 查找起点（自身不计入），可为 `NULL` |
| `tag` | in | 标签名，大小写不敏感 |

**返回**：命中的元素或 `NULL`

**注意**：递归深度优先，只比较元素节点

#### `hpe_dom_is_element`

```c
int hpe_dom_is_element(const HpeDomNode *node);
```

判断节点是否为元素

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in | DOM 节点 |

**返回**：元素节点返回 1

**注意**：`node` 为 `NULL` 时返回 0

#### `hpe_dom_free`

```c
void hpe_dom_free(HpeDomNode *node);
```

递归释放节点及其整个子树

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in,out | DOM 节点 |

**注意**：同时释放 `<template>` 的 content 片段、全部属性与标签/文本字符串；传 `NULL` 安全无操作

#### `hpe_dom_add_mutation_sink`

```c
void hpe_dom_add_mutation_sink(HpeDomMutationFn fn, void *ud);
```

追加一个 DOM 变更订阅者

**详细说明**：订阅者按安装顺序保存在固定大小的表内（最多 `HPE_MAX_MUT_SINKS` 个）。 Sinks are kept in an ordered fixed-size table of at most `HPE_MAX_MUT_SINKS` entries.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `fn` | in | 变更回调（`NULL` 时直接返回） |
| `ud` | in,out | 透传给 `fn` 的用户数据，与 `fn` 一起构成订阅标识 |

**注意**：同一 `(fn, ud)` 组合已存在时不再重复安装；表满时静默忽略

**参见**：`hpe_dom_remove_mutation_sink`、`hpe_dom_clear_mutation_sinks`

#### `hpe_dom_remove_mutation_sink`

```c
void hpe_dom_remove_mutation_sink(HpeDomMutationFn fn, void *ud);
```

移除一个已安装的 DOM 变更订阅者

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `fn` | in | 要移除的变更回调 |
| `ud` | in,out | 与之配对的用户数据 |

**注意**：按 `(fn, ud)` 精确匹配，未找到时不做任何事；后续订阅者前移保持顺序

**参见**：`hpe_dom_add_mutation_sink`

#### `hpe_dom_clear_mutation_sinks`

```c
void hpe_dom_clear_mutation_sinks(void);
```

移除全部变更订阅者

**详细说明**：文档销毁时调用，使 `notify_sinks` 之后不再回调任何人。 Called when the document is torn down so that `notify_sinks` no longer reaches anyone.

**类型**

#### `HpeNodeType`

```c
typedef enum HpeNodeType {
    HPE_NODE_DOCUMENT = 0,
    HPE_NODE_ELEMENT,
    HPE_NODE_TEXT,
    /* a DocumentFragment: a child-list container that is not itself part of the
     * tree; appending it to a parent moves its children instead (see
     * hpe_dom_append). It never reaches the box tree. */
    HPE_NODE_FRAGMENT
} HpeNodeType;
```

#### `HpeAttr`

```c
typedef struct HpeAttr {
    char *name;
    char *value;
} HpeAttr;
```

#### `HpeDomNode`

```c
typedef struct HpeDomNode {
    HpeNodeType type;
    char *tag;  /* element: lowercase tag name */
    char *text; /* text node: decoded content */
    HpeAttr *attrs;
    int attr_count;
    size_t attr_cap;
    struct HpeDomNode *parent;
    struct HpeDomNode *first_child;
    struct HpeDomNode *last_child;
    struct HpeDomNode *next_sibling;
    /* <template> only: its children live in this detached DocumentFragment
     * (the spec's HTMLTemplateElement "content"), never in the main tree. Every
     * other node leaves it NULL, so the box tree never sees the content. */
    struct HpeDomNode *content;
    void *listeners; /* DOM bridge: per-node event listener list (opaque) */
    void *box;       /* layout: box generated for this element (opaque) */
    /* dynamic state driving the state pseudo-classes; all are maintained by the
     * engine/host layer and read by the CSS matcher. */
    int hovered;  /* :hover — pointer is over this element */
    int focused;  /* :focus — this element owns the focus */
    int active;   /* :active — pointer is pressed on this element */
    int visited;  /* :visited — element was activated before */
    int targeted; /* :target — element is the fragment target of the URL */
} HpeDomNode;
```

#### `HpeDomMutationFn`

```c
typedef void (*HpeDomMutationFn)(void *ud, int kind, HpeDomNode *target,
                                 const char *name, const char *old_value,
                                 HpeDomNode *added, HpeDomNode *removed);
```

## src/html（HTML 解析）

### hpe_encoding.h

2 个函数，1 个类型。

**函数**

#### `hpe_html_decode`

```c
char *hpe_html_decode(const char *raw, size_t len, size_t *out_len, HpeDocEncoding *out_enc, int *out_from_meta);
```

把原始文档字节解码为 UTF-8

**详细说明**：嗅探顺序即 HTML 规范规定的顺序：BOM 优先，其次是前 1024 字节内的 `<meta charset>`（或 `http-equiv="content-type"`），最后用严格的 utf-8 校验在 utf-8 与 latin-1 之间抉择。 The sniff order follows the HTML spec: a BOM wins, then `<meta charset>` (or `http-equiv="content-type"`) within the first 1024 bytes, and finally a strict utf-8 validation decides between utf-8 and latin-1.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `raw` | in | 原始文档字节，为 `NULL` 时返回空串 |
| `len` | in | 原始字节数 |
| `out_len` | out | 结果的 UTF-8 字节数，可为 `NULL` |
| `out_enc` | out | 探测到的编码，可为 `NULL` |
| `out_from_meta` | out | 置 1 表示编码来自文档内声明，可为 `NULL` |

**返回**：新分配且以 NUL 结尾的 UTF-8 字符串，永不返回 `NULL`

**注意**：调用方负责用 `hpe_free` 释放返回值；UTF-8 BOM 会被剥掉

#### `hpe_encoding_name`

```c
const char *hpe_encoding_name(HpeDocEncoding enc);
```

取得编码的小写名称

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `enc` | in | 待命名的编码 |

**返回**：`utf-8` / `latin1` / `utf-16le` / `utf-16be` 之一

**注意**：供开发/测试钩子使用的标签

**类型**

#### `HpeDocEncoding`

```c
typedef enum HpeDocEncoding {
    HPE_ENC_UTF8 = 0,
    HPE_ENC_LATIN1,  /* iso-8859-1 / windows-1252 byte stream */
    HPE_ENC_UTF16LE,
    HPE_ENC_UTF16BE
} HpeDocEncoding;
```

### hpe_html.h

1 个函数，0 个类型。

**函数**

#### `hpe_html_parse`

```c
HpeDomNode *hpe_html_parse(const char *source, size_t length);
```

解析本地 HTML 文档为 DOM 树

**详细说明**：容错解析：隐式闭合、表格节点补全、foster parenting、原始文本与实体解码都在此过程中完成。 Fault-tolerant parsing: implicit closing, table-node synthesis, foster parenting, raw-text handling and entity decoding all happen here.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `source` | in | HTML 源文本 |
| `length` | in | 源文本字节数 |

**返回**：包含 `<html><head></head><body>...</body></html>` 的文档节点

**注意**：返回的文档树由调用方用 `hpe_dom_free` 释放

**参见**：`hpe_dom_free`

**类型**

无。

## src/js（JavaScript 引擎）

### hpe_js.h

11 个函数，9 个类型。

**函数**

#### `hpe_js_create`

```c
HpeJs *hpe_js_create(void);
```

创建并初始化一个 JS 解释器实例

**返回**：新的解释器实例；失败时返回 `NULL`

**注意**：实例内部自带 arena、全局对象与各内建原型，随后由 `hpe_js_destroy` 整体释放

**参见**：`hpe_js_destroy`

#### `hpe_js_destroy`

```c
void hpe_js_destroy(HpeJs *js);
```

销毁解释器并释放其全部内存

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | 要销毁的解释器实例 |

**注意**：arena 整体释放，实例内一切对象随之失效

**参见**：`hpe_js_create`

#### `hpe_js_gc_capture_main_base`

```c
void hpe_js_gc_capture_main_base(void);
```

在宿主最外层帧记录 GC 栈基线

**注意**：在 `main()` 开头调用一次：GC 保守扫描 C 栈时以此为上界，确保后续所有 深层 eval 帧上的引用都被视为根

#### `hpe_js_set_global_host`

```c
void hpe_js_set_global_host(HpeJs *js, const char *name, void *ptr, int type, const HpeJsHostVt *vt);
```

把一个宿主对象暴露为全局变量

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `name` | in | 全局变量名（如 `document`） |
| `ptr` | in,out | 宿主对象指针 |
| `type` | in | 宿主类型标记（`HPE_HOST_*`） |
| `vt` | in | 可选的属性拦截虚表 |

**注意**：典型用法是把 `document` 挂到全局，使脚本可直接访问

**参见**：`hpe_js_dom_install`

#### `hpe_js_eval`

```c
int hpe_js_eval(HpeJs *js, const char *source, char *errbuf, size_t errbuf_size);
```

执行一段脚本

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `source` | in | 脚本源码 |
| `errbuf` | out | 接收错误信息，可为 `NULL` |
| `errbuf_size` | in | `errbuf` 的容量（字节） |

**返回**：成功返回 0，出错返回非 0

**注意**：出错时把信息写入 `errbuf`

#### `hpe_js_eval_module`

```c
int hpe_js_eval_module(HpeJs *js, const char *source, const char *dir, char *errbuf, size_t errbuf_size);
```

按 ES 模块执行一段脚本

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `source` | in | 模块源码 |
| `dir` | in | 解析相对 `import` 说明符的基准目录 |
| `errbuf` | out | 接收错误信息，可为 `NULL` |
| `errbuf_size` | in | `errbuf` 的容量（字节） |

**返回**：成功返回 0，出错返回非 0

**注意**：允许 `import`/`export` 语法；模块解析器由 `hpe_js_set_module_loader` 注册

**参见**：`hpe_js_set_module_loader`、`hpe_js_set_current_dir`

#### `hpe_js_call`

```c
int hpe_js_call(HpeJs *js, JsValue callee, JsValue thisv, JsValue *args, int argc);
```

从 C 侧调用一个可调用的值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `callee` | in | 被调用的值（如事件处理函数） |
| `thisv` | in | 调用时的 `this` 值 |
| `args` | in,out | 实参数组，`argc` 为 0 时可为 `NULL` |
| `argc` | in | 参数个数 |

**返回**：成功返回 0，运行时错误返回 -1

**注意**：运行时错误会报告到 stderr

#### `hpe_js_run_timers`

```c
int hpe_js_run_timers(HpeJs *js, double now, int *fired);
```

执行到期定时器与一轮 rAF 回调

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `now` | in | 当前时间（秒），与定时器到期时刻同基准 |
| `fired` | out | 实际上执行的回调个数，可为 `NULL` |

**返回**：实际执行的回调个数

**注意**：宿主可据 `*fired` 决定是否重排重绘

#### `hpe_js_set_module_loader`

```c
void hpe_js_set_module_loader(HpeJs *js, const HpeJsModuleLoader *loader);
```

注册 `import` 说明符的解析器

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `loader` | in | 模块加载器回调结构 |

#### `hpe_js_set_current_dir`

```c
void hpe_js_set_current_dir(HpeJs *js, const char *dir);
```

设置相对说明符的基准目录

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `dir` | in | 基准目录 |

**注意**：相对 `import` 说明符以此目录为起点解析

#### `hpe_js_set_entry_path`

```c
void hpe_js_set_entry_path(HpeJs *js, const char *path);
```

设置入口模块路径（供 `import.meta.url` 使用）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `path` | in | 入口模块的文件路径；`NULL` 或空串表示未知 |

**注意**：入口模块不经 `module_run`，因此它的路径需要宿主显式提供；未设置时 `import.meta.url` 回退为 `<eval>`

**类型**

#### `JsType`

```c
typedef enum JsType {
    JS_UNDEF = 0,
    JS_NULL,
    JS_BOOL,
    JS_NUM,
    JS_STR,
    JS_OBJ,
    JS_BIGINT,
    JS_SYMBOL
} JsType;
```

#### `JsObj`

```c
typedef struct JsObj JsObj;
```

#### `HpeBigInt`

```c
typedef struct HpeBigInt HpeBigInt;
```

#### `HpeSymbol`

```c
typedef struct HpeSymbol HpeSymbol;
```

#### `JsValue`

```c
typedef struct JsValue {
    JsType t;
    double num;
    char *str; /* JS_STR: heap-owned immutable string */
    JsObj *obj;
    HpeBigInt *big; /* JS_BIGINT: arena-owned arbitrary-precision integer */
    HpeSymbol *sym; /* JS_SYMBOL: arena-owned symbol identity */
} JsValue;
```

#### `HpeJs`

```c
typedef struct HpeJs HpeJs;
```

#### `JsNativeFn`

```c
typedef JsValue (*JsNativeFn)(HpeJs *js, JsValue thisv, JsValue *args, int argc, void *ud);
```

#### `HpeJsHostVt`

```c
typedef struct HpeJsHostVt {
    /* return 1 if the property was handled */
    int (*get)(HpeJs *js, void *target, const char *prop, JsValue *out);
    int (*set)(HpeJs *js, void *target, const char *prop, JsValue value);
} HpeJsHostVt;
```

#### `HpeJsModuleLoader`

```c
typedef struct HpeJsModuleLoader {
    int (*load)(void *ud, const char *spec, const char *from_dir, char **out_path,
                char **out_src);
    void *ud;
} HpeJsModuleLoader;
```

### hpe_js_canvas.h

0 个函数，0 个类型。

**函数**

无。

**类型**

无。

### hpe_js_dom.h

7 个函数，1 个类型。

**函数**

#### `hpe_js_dom_install`

```c
void hpe_js_dom_install(HpeJs *js, HpeDomNode *document);
```

注册 `document` 宿主对象及其虚表

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `document` | in,out | 文档根 DOM 节点 |

**注意**：同时注册元素与样式表的属性拦截虚表

**参见**：`hpe_js_dom_node_value`

#### `hpe_js_dom_node_value`

```c
JsValue hpe_js_dom_node_value(HpeJs *js, HpeDomNode *node);
```

把 DOM 节点包装成脚本可见的宿主值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `node` | in,out | 待包装的 DOM 节点 |

**返回**：携带元素或片段虚表的宿主值

**注意**：MutationObserver 桥用它把完全可用的节点作为记录 `target` 交回脚本

#### `hpe_js_dom_dispatch`

```c
int hpe_js_dom_dispatch(HpeJs *js, HpeDomNode *target, const char *type, int clientX, int clientY);
```

向目标节点及其祖先派发事件（冒泡）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `target` | in,out | 事件目标节点 |
| `type` | in | 事件类型名（如 `click`） |
| `clientX` | in | 客户端 X 坐标 |
| `clientY` | in | 客户端 Y 坐标 |

**返回**：有处理函数被调用返回 1

**注意**：传给处理函数的事件对象为 `{ type, clientX, clientY, target, currentTarget, preventDefault }`

#### `hpe_js_dom_dispatch_ex`

```c
int hpe_js_dom_dispatch_ex(HpeJs *js, HpeDomNode *target, const char *type, const HpeJsEventInit *init);
```

派发一个带附加数据的事件

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `target` | in,out | 事件目标节点 |
| `type` | in | 事件类型名 |
| `init` | in | 附加数据；传 `NULL` 表示全默认 |

**返回**：有处理函数被调用返回 1

**注意**：事件流与 `hpe_js_dom_dispatch` 一致；键盘事件用 `key`/`code`/`keyCode` 与修饰键， 滚轮事件用 `deltaX`/`deltaY`/`deltaMode`，进出事件用 `relatedTarget` The flow matches `hpe_js_dom_dispatch`; keyboard events carry `key`/`code`/`keyCode` and the modifiers, wheel events carry `deltaX`/`deltaY`/`deltaMode`, and enter/leave events carry `relatedTarget`

**参见**：`hpe_js_dom_dispatch`

#### `hpe_js_dom_dispatch_cancelable`

```c
int hpe_js_dom_dispatch_cancelable(HpeJs *js, HpeDomNode *target, const char *type, const HpeJsEventInit *init, int *prevented);
```

派发一个可取消事件并回报默认行为是否被阻止

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `target` | in,out | 事件目标节点 |
| `type` | in | 事件类型名（如 `submit` / `reset`） |
| `init` | in | 附加数据；传 `NULL` 表示全默认 |
| `prevented` | out | 处理函数调用 `preventDefault` 时置 1，可为 `NULL` |

**返回**：有处理函数被调用返回 1

**注意**：事件对象带真正生效的 `preventDefault()`；嵌套的取消派发共享同一回报槽

#### `hpe_js_dom_dispatch_self`

```c
int hpe_js_dom_dispatch_self(HpeJs *js, HpeDomNode *target, const char *type, const HpeJsEventInit *init);
```

只在目标节点上触发、不传播的事件

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `target` | in,out | 事件目标节点 |
| `type` | in | 事件类型名 |
| `init` | in | 附加数据；传 `NULL` 表示全默认 |

**返回**：有处理函数被调用返回 1

**注意**：`mouseenter` / `mouseleave` 不冒泡，用它派发；`relatedTarget` 照常写入

#### `hpe_js_dom_active_element`

```c
HpeDomNode *hpe_js_dom_active_element(HpeJs *js);
```

查询当前拥有 `:focus` 的元素

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

**返回**：焦点元素，无焦点时返回 `NULL`

**类型**

#### `HpeJsEventInit`

```c
typedef struct HpeJsEventInit {
    int client_x;        /* pointer events */
    int client_y;
    int delta_x;         /* wheel */
    int delta_y;
    int delta_mode;      /* 0 = pixels, 1 = lines, 2 = pages */
    int key_code;        /* keyboard: the legacy numeric code */
    int ctrl;            /* modifier flags, keyboard and pointer alike */
    int shift;
    int alt;
    int meta;
    int repeat;          /* keyboard: 1 while the key is held down */
    int no_bubble;       /* 1 to keep the event on its target only (a `bubbles: false` Event) */
    const char *key;     /* keyboard: the printable key name (`a`, `Enter`) */
    const char *code;    /* keyboard: the physical key code (`KeyA`) */
    const char *new_state; /* toggle/beforetoggle: `"open"` or `"closed"` */
    HpeDomNode *related; /* mouseover/mouseout: where the pointer came from or went to */
} HpeJsEventInit;
```

事件初始化数据

### hpe_js_internal.h（内部）

140 个函数，19 个类型。

**函数**

#### `hpe_js_fiber_enter`

```c
void hpe_js_fiber_enter(HpeJs *js, HpeJsFiberCtx *ctx);
```

切进协程前安装协程侧的异常上下文

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `ctx` | in,out | 该协程的上下文块 |

#### `hpe_js_fiber_leave`

```c
void hpe_js_fiber_leave(HpeJs *js, HpeJsFiberCtx *ctx);
```

切出协程后还原调用方侧的异常上下文

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `ctx` | in,out | 该协程的上下文块 |

#### `hpe_js_alloc`

```c
void *hpe_js_alloc(HpeJs *js, size_t size);
```

从 arena 分配 8 字节对齐的已清零内存

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `size` | in | 请求字节数，向上对齐到 8 的倍数 |

**返回**：指向已清零内存的指针

**注意**：分配的内存不能单独释放，随解释器销毁整体回收

#### `hpe_js_alloc_typed`

```c
void *hpe_js_alloc_typed(HpeJs *js, size_t size, int tag);
```

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | — | — |
| `size` | — | — |
| `tag` | — | — |

#### `hpe_js_strndup`

```c
char *hpe_js_strndup(HpeJs *js, const char *s, size_t n);
```

在 arena 中复制指定长度的字符串

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `s` | in | 源字符串，可为 `NULL` |
| `n` | in | 复制的字节数 |

**返回**：以 NUL 结尾的新字符串

#### `hpe_js_strdup`

```c
char *hpe_js_strdup(HpeJs *js, const char *s);
```

在 arena 中复制以 NUL 结尾的字符串

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `s` | in | 源字符串，可为 `NULL` |

**返回**：复制的以 NUL 结尾的字符串

#### `js_undef`

```c
JsValue js_undef(void);
```

构造 `undefined` 值

**返回**：类型为 `JS_UNDEF` 的值

#### `js_null`

```c
JsValue js_null(void);
```

构造 `null` 值

**返回**：类型为 `JS_NULL` 的值

#### `js_bool`

```c
JsValue js_bool(int b);
```

由 C 布尔值构造 JS 布尔值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `b` | in | 非零为 `true` |

**返回**：类型为 `JS_BOOL` 的值

#### `js_num`

```c
JsValue js_num(double v);
```

构造 JS 数值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 数值（可为 `NaN`/`Infinity`） |

**返回**：类型为 `JS_NUM` 的值

#### `js_string`

```c
JsValue js_string(HpeJs *js, const char *s);
```

由以 NUL 结尾字符串构造 JS 字符串

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `s` | in | 源字符串，可为 `NULL` |

**返回**：类型为 `JS_STR` 的值

#### `js_string_len`

```c
JsValue js_string_len(HpeJs *js, const char *s, size_t n);
```

由定长字节构造 JS 字符串

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `s` | in | 源字节，`NULL` 视为空 |
| `n` | in | 字节数 |

**返回**：类型为 `JS_STR` 的值

#### `js_objval`

```c
JsValue js_objval(JsObj *o);
```

把对象指针包装成 JS 值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `o` | in,out | 目标对象 |

**返回**：类型为 `JS_OBJ` 的值

#### `js_object_new`

```c
JsObj *js_object_new(HpeJs *js);            /* plain object (proto = Object.prototype) */
```

创建一个原型为 `Object.prototype` 的普通对象

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

**返回**：新普通对象

#### `js_object_raw`

```c
JsObj *js_object_raw(HpeJs *js);            /* plain object with no prototype */
```

分配一个无原型的普通对象

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

**返回**：新对象（类名 `Object`、`proto` 为 `NULL`）

**注意**：需要原型链时改用 `js_object_new`

#### `js_array_new`

```c
JsObj *js_array_new(HpeJs *js);             /* array with element accessors */
```

创建一个 JS 数组对象

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

**返回**：类名为 `Array`、`length` 为 0 的新数组

#### `js_func_new`

```c
JsObj *js_func_new(HpeJs *js, struct JsNode *code, JsEnv *closure);
```

由 AST 函数节点创建用户函数对象

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `code` | in,out | 函数声明的 AST 节点 |
| `closure` | in,out | 创建时捕获的词法环境 |

**返回**：新的函数对象

**注意**：非箭头函数还会生成带 `constructor` 的 `.prototype` 对象

#### `js_native_new`

```c
JsObj *js_native_new(HpeJs *js, JsNativeFn fn, void *ud);
```

创建原生（C 实现）函数对象

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `fn` | in | 原生函数指针 |
| `ud` | in,out | 透传给原生函数的用户数据 |

**返回**：类名为 `Function` 的原生函数对象

#### `js_host_new`

```c
JsObj *js_host_new(HpeJs *js, void *ptr, int type, const HpeJsHostVt *vt);
```

创建宿主对象，把 C 指针暴露给脚本

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `ptr` | in,out | 宿主指针 |
| `type` | in | 宿主类型标记（`HPE_HOST_*`） |
| `vt` | in | 可选的属性拦截虚表 |

**返回**：类名为 `Host` 的宿主对象

**参见**：`js_make_host`、`js_host_ptr`

#### `js_promise_new`

```c
JsObj *js_promise_new(HpeJs *js);
```

创建一个处于 pending 状态的 Promise

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

**返回**：类名为 `Promise` 的新对象

#### `js_module_ns_new`

```c
JsObj *js_module_ns_new(HpeJs *js, JsModule *mod);
```

创建模块命名空间对象

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `mod` | in,out | 该命名空间所属的模块 |

**返回**：类名为 `Module`、无原型的对象

#### `js_array_install_methods`

```c
void js_array_install_methods(HpeJs *js, JsObj *arr);
```

为数组对象挂上数组原型方法

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `arr` | in,out | 目标数组对象 |

**注意**：由 `js_array_new` 调用，整进程只安装一次

#### `js_obj_find_prop`

```c
JsProp *js_obj_find_prop(JsObj *obj, const char *key); /* walks the prototype chain */
```

沿原型链查找属性记录

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `obj` | in,out | 起始对象 |
| `key` | in | 键名 |

**返回**：命中的属性指针，未找到返回 `NULL`

#### `js_obj_get`

```c
JsValue js_obj_get(HpeJs *js, JsObj *obj, const char *key);
```

读取属性值（含原型链）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `obj` | in,out | 目标对象 |
| `key` | in | 属性键名 |

**返回**：属性值；未找到或属性为访问器时返回 `undefined`

**注意**：访问器属性需由 `js_get_prop()` 调用读取

#### `js_obj_set`

```c
void js_obj_set(HpeJs *js, JsObj *obj, const char *key, JsValue v);
```

设置对象属性，不存在则创建

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `obj` | in,out | 目标对象 |
| `key` | in | 属性键名 |
| `v` | in | 要写入的值 |

**注意**：数组的 `length` 与 `Symbol.*` 前缀键会被置为非枚举

#### `js_obj_set_accessor`

```c
void js_obj_set_accessor(HpeJs *js, JsObj *obj, const char *key, JsValue getter, JsValue setter);
```

为属性安装 getter/setter 访问器

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `obj` | in,out | 目标对象 |
| `key` | in | 属性键名 |
| `getter` | in | 读取器函数值 |
| `setter` | in | 写入器函数值 |

**注意**：传入 `undefined` 的一侧保持不变

#### `js_obj_has`

```c
int js_obj_has(JsObj *obj, const char *key);
```

判断对象（含原型链）是否有某属性

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `obj` | in,out | JS 对象 |
| `key` | in | 键名 |

**返回**：自身或原型链上存在返回 1

#### `js_obj_own_has`

```c
int js_obj_own_has(JsObj *obj, const char *key);
```

判断对象自身是否有某属性

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `obj` | in,out | JS 对象 |
| `key` | in | 键名 |

**返回**：自身存在该属性返回 1

#### `js_obj_delete`

```c
int js_obj_delete(HpeJs *js, JsObj *obj, const char *key);
```

删除对象的自身属性

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文（未使用） |
| `obj` | in,out | 目标对象 |
| `key` | in | 属性键名 |

**返回**：总是返回 1

#### `js_obj_own_keys`

```c
int js_obj_own_keys(JsObj *obj, char ***out, int *count);
```

收集对象的可枚举自身键名

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `obj` | in,out | JS 对象，可为 `NULL` |
| `out` | out | 接收以 `NULL` 结尾的键名数组 |
| `count` | out | 键名个数 |

**返回**：键名个数

**注意**：键名数组用 `malloc` 分配，调用方需 `free`

#### `js_obj_own_symbols`

```c
int js_obj_own_symbols(JsObj *obj, long long **out, int *count);
```

取对象自身的符号键标识

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `obj` | in,out | 目标对象 |
| `out` | out | 接收符号标识数组（`malloc`，调用方 `free`） |
| `count` | out | 接收个数 |

**返回**：符号个数

#### `js_obj_prop_count`

```c
int js_obj_prop_count(JsObj *obj);
```

返回对象自身属性个数

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `obj` | in,out | JS 对象，可为 `NULL` |

**返回**：自身属性个数

#### `js_num_to_string`

```c
char *js_num_to_string(HpeJs *js, double v);
```

把数值格式化为 JS 风格字符串

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 待格式化的数值 |

**返回**：arena 中的字符串表示

**注意**：整数值用 `%lld`，其余用 `%.14g`；`NaN`/`Infinity` 有专门拼写

#### `js_to_string`

```c
char *js_to_string(HpeJs *js, JsValue v);
```

数值/语义上的 `ToString` 抽象操作

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 任意 JS 值 |

**返回**：arena 中的字符串表示

#### `js_primitive_of`

```c
JsValue js_primitive_of(HpeJs *js, JsValue v);
```

对象到原始值的 `ToPrimitive`（default 提示）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 待转换的值 |

**返回**：原始值；非对象时原样返回

**注意**：先调用自身 `valueOf()` 再 `toString()`，够支撑 `obj == 1` 这类宽松比较

#### `js_to_number`

```c
double js_to_number(HpeJs *js, JsValue v);
```

语义上的 `ToNumber` 抽象操作

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 任意 JS 值 |

**返回**：转换后的 double

**注意**：`undefined`→NaN、`null`→0、空串→0、含非法尾随的字符串→NaN

#### `js_truthy`

```c
int js_truthy(JsValue v);
```

语义上的 `ToBoolean` 抽象操作

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 任意 JS 值 |

**返回**：真值返回 1，假值返回 0

#### `js_values_equal`

```c
int js_values_equal(HpeJs *js, JsValue a, JsValue b, int strict);
```

按严格或宽松语义比较两个值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `a` | in | 左操作数 |
| `b` | in | 右操作数 |
| `strict` | in | 严格比较为 1（`===`） |

**返回**：相等返回 1

**注意**：宽松模式做数值/字符串互转、布尔转数，并把 `null` 与 `undefined` 视为相等

#### `js_is_callable`

```c
int js_is_callable(JsValue v);
```

判断值是否可调用

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 任意 JS 值 |

**返回**：函数或原生函数返回 1

#### `js_is_thenable`

```c
int js_is_thenable(HpeJs *js, JsValue v);
```

判断值是否为 thenable

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 任意 JS 值 |

**返回**：Promise 或带可调用 `then` 的对象返回 1

#### `js_obj_own_prop`

```c
struct JsProp *js_obj_own_prop(JsObj *obj, const char *key);
```

查找对象自身的属性记录

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `obj` | in,out | JS 对象，可为 `NULL` |
| `key` | in | 键名，可为 `NULL` |

**返回**：命中的属性指针，未找到返回 `NULL`

**注意**：不走原型链

#### `js_get_prop`

```c
JsValue js_get_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, int line);
```

`[[Get]]`：读取成员并执行访问器

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `env` | in,out | 当前词法环境 |
| `base` | in | 成员访问的基值 |
| `prop` | in | 属性名 |
| `line` | in | 出错时报告的行号 |

**返回**：读取到的属性值

**注意**：读取 getter 并处理原型链；解释器的成员表达式与 DOM 桥都走它

**参见**：`js_set_prop`

#### `js_set_prop`

```c
void js_set_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, JsValue value, int line);
```

`[[Set]]`：写入成员并执行访问器

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `env` | in,out | 当前词法环境 |
| `base` | in | 成员访问的基值 |
| `prop` | in | 属性名 |
| `value` | in | 要写入的值 |
| `line` | in | 出错时报告的行号 |

**注意**：写入 setter 并处理原型链

**参见**：`js_get_prop`

#### `js_error`

```c
void js_error(HpeJs *js, int line, const char *fmt, ...); /* throws an Error */
```

格式化并抛出一个 SyntaxError

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `line` | in | 行号，大于 0 时前缀 `line N:` |
| `fmt` | in | `printf` 风格格式串 |
| `` | — | — |

**注意**：变参经 `vsnprintf` 渲染；本函数不返回

#### `js_throw_value`

```c
void js_throw_value(HpeJs *js, JsValue v);
```

抛出值并展开到最近的 try 帧

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 要抛出的值 |

**注意**：记录到 `js->thrown` 后 `longjmp`，无 try 帧时回退到 `js->jmp`

#### `js_make_error`

```c
JsValue js_make_error(HpeJs *js, const char *name, const char *msg);
```

创建 Error 对象

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `name` | in | 错误类名（如 `TypeError`） |
| `msg` | in | 错误消息，可为 `NULL` |

**返回**：新的 Error 值

**注意**：按类名选择专用原型，并写入 `name` 与 `message`

#### `js_invoke`

```c
JsValue js_invoke(HpeJs *js, JsValue fn, JsValue thisv, JsValue *args, int argc, int line);
```

从 C 侧调用可调用的值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `fn` | in | 被调用的函数值 |
| `thisv` | in | 调用时的 `this` 值 |
| `args` | in,out | 实参数组，`argc` 为 0 时可为 `NULL` |
| `argc` | in | 参数个数 |
| `line` | in | 出错时报告的行号 |

**返回**：调用的返回值

**注意**：异常经 `longjmp` 向外传播

#### `js_construct`

```c
JsValue js_construct(HpeJs *js, JsValue fn, JsValue *args, int argc, int line);
```

以 `new` 方式构造对象

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `fn` | in | 构造函数值 |
| `args` | in,out | 实参数组，`argc` 为 0 时可为 `NULL` |
| `argc` | in | 参数个数 |
| `line` | in | 出错时报告的行号 |

**返回**：新建实例，或构造函数体返回的对象

**注意**：构造体显式返回对象时以该对象为准

#### `js_env_new`

```c
JsEnv *js_env_new(HpeJs *js, JsEnv *parent);
```

新建一个词法环境

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `parent` | in,out | 父环境，可为 `NULL` |

**返回**：新环境；`strict` 继承父环境或全局设置

#### `js_env_lookup`

```c
JsValue *js_env_lookup(JsEnv *env, const char *name);
```

沿环境链查找变量槽位

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `env` | in,out | 起始环境 |
| `name` | in | 变量名 |

**返回**：变量值槽位指针，未绑定返回 `NULL`

#### `js_env_define`

```c
void js_env_define(HpeJs *js, JsEnv *env, const char *name, JsValue v);
```

在当前环境定义变量（已存在则赋值）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `env` | in,out | 目标环境 |
| `name` | in | 变量名 |
| `v` | in | 初始值 |

**注意**：仅在当前环境建新槽位

#### `js_env_declare`

```c
void js_env_declare(HpeJs *js, JsEnv *env, const char *name, JsValue v);
```

在当前作用域做全新绑定（let/const/class）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `env` | in,out | 目标环境 |
| `name` | in | 变量名 |
| `v` | in | 初始值 |

**注意**：同名绑定已存在时直接改写其值，从而遮蔽外层

#### `js_env_this_env`

```c
JsEnv *js_env_this_env(JsEnv *env);
```

找到持有 `this` 绑定的最近环境

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `env` | in,out | 起始环境 |

**返回**：最近持有 `this` 的环境，找不到返回 `NULL`

#### `js_get_prop`

```c
JsValue js_get_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, int line);
```

`[[Get]]`：读取成员并执行访问器

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `env` | in,out | 当前词法环境 |
| `base` | in | 成员访问的基值 |
| `prop` | in | 属性名 |
| `line` | in | 出错时报告的行号 |

**返回**：读取到的属性值

**注意**：读取 getter 并处理原型链；解释器的成员表达式与 DOM 桥都走它

**参见**：`js_set_prop`

#### `js_set_prop`

```c
void js_set_prop(HpeJs *js, JsEnv *env, JsValue base, const char *prop, JsValue value, int line);
```

`[[Set]]`：写入成员并执行访问器

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `env` | in,out | 当前词法环境 |
| `base` | in | 成员访问的基值 |
| `prop` | in | 属性名 |
| `value` | in | 要写入的值 |
| `line` | in | 出错时报告的行号 |

**注意**：写入 setter 并处理原型链

**参见**：`js_get_prop`

#### `js_node_new`

```c
JsNode *js_node_new(HpeJs *js, int kind, int line);
```

新建一个 AST 节点

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `kind` | in | 节点种类（`JS_N_*`） |
| `line` | in | 源码行号 |

**返回**：新的 AST 节点

#### `js_node_add`

```c
void js_node_add(HpeJs *js, JsNode *parent, JsNode *child);
```

把子节点追加到父 AST 节点

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `parent` | in,out | 父节点 |
| `child` | in,out | 要追加的子节点 |

#### `js_parse_program`

```c
JsNode *js_parse_program(HpeJs *js, const char *source);
```

把源码解析为脚本程序

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `source` | in | 脚本源码 |

**返回**：程序根 AST 节点

**参见**：`js_parse_module`

#### `js_parse_module`

```c
JsNode *js_parse_module(HpeJs *js, const char *source);
```

把源码解析为 ES 模块

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `source` | in | 模块源码 |

**返回**：程序根 AST 节点

**注意**：允许 `import`/`export` 语法

**参见**：`js_parse_program`

#### `js_parse_expression`

```c
JsNode *js_parse_expression(HpeJs *js, const char *source);
```

把源码解析为单个表达式

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `source` | in | 表达式源码 |

**返回**：表达式 AST 节点

#### `hpe_js_gc_scan_range`

```c
void hpe_js_gc_scan_range(HpeJs *js, void *lo, void *hi);
```

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | — | — |
| `lo` | — | — |
| `hi` | — | — |

#### `hpe_js_gc_collect`

```c
size_t hpe_js_gc_collect(HpeJs *js);
```

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | — | — |

#### `hpe_js_gc_maybe`

```c
int hpe_js_gc_maybe(HpeJs *js);
```

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | — | — |

#### `hpe_js_gc_install`

```c
void hpe_js_gc_install(HpeJs *js);
```

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | — | — |

#### `hpe_js_timers_gc_scan`

```c
void hpe_js_timers_gc_scan(HpeJs *js); /* timer table as a GC root (timers.c) */
```

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | — | — |

#### `hpe_js_gen_gc_scan`

```c
void hpe_js_gen_gc_scan(HpeJs *js, void *state);
```

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | — | — |
| `state` | — | — |

#### `hpe_js_async_gc_scan`

```c
void hpe_js_async_gc_scan(HpeJs *js, void *state);
```

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | — | — |
| `state` | — | — |

#### `js_error`

```c
void js_error(HpeJs *js, int line, const char *fmt, ...);
```

格式化并抛出一个 SyntaxError

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `line` | in | 行号，大于 0 时前缀 `line N:` |
| `fmt` | in | `printf` 风格格式串 |
| `` | — | — |

**注意**：变参经 `vsnprintf` 渲染；本函数不返回

#### `js_make_host`

```c
JsValue js_make_host(HpeJs *js, void *ptr, int type, const HpeJsHostVt *vt);
```

创建宿主对象并包装为 JS 值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `ptr` | in,out | 宿主指针 |
| `type` | in | 宿主类型标记（`HPE_HOST_*`） |
| `vt` | in | 可选的属性拦截虚表 |

**返回**：包装宿主对象的 `JS_OBJ` 值

**参见**：`js_host_ptr`

#### `js_host_ptr`

```c
void *js_host_ptr(JsValue v, int type); /* NULL if not a host of that type */
```

取回宿主对象内的 C 指针

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 待检查的值 |
| `type` | in | 期望的宿主类型标记 |

**返回**：类型匹配时返回宿主指针，否则返回 `NULL`

#### `hpe_js_timers_install`

```c
void hpe_js_timers_install(HpeJs *js);
```

在全局对象上安装定时器与 rAF

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

#### `hpe_js_run_timers`

```c
int hpe_js_run_timers(HpeJs *js, double now, int *fired);
```

执行到期定时器与 rAF 回调

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `now` | in | 当前时间（秒），与定时器到期时刻同基准 |
| `fired` | out | 任一回调执行后置 1，可为 `NULL` |

**返回**：实际执行的回调个数

**注意**：宿主可据 `*fired` 决定是否重排重绘

#### `hpe_js_drain_jobs`

```c
void hpe_js_drain_jobs(HpeJs *js);
```

运行排队的 Promise 微任务直到清空

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

#### `js_job_push`

```c
void js_job_push(HpeJs *js, JsJob *job);
```

把作业追加到微任务队列尾

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `job` | in,out | 要入队的作业 |

#### `hpe_js_queue_microtask`

```c
void hpe_js_queue_microtask(HpeJs *js, JsValue fn, JsValue arg);
```

把 `fn(arg)` 排为微任务

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `fn` | in | 要调用的函数值 |
| `arg` | in | 传给函数的唯一实参 |

**注意**：MutationObserver 用它异步投递记录，对应规范中观察者微任务的一步

#### `js_settle_promise`

```c
void js_settle_promise(HpeJs *js, JsObj *p, int state, JsValue value);
```

兑现或拒绝一个 Promise

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `p` | in,out | 目标 Promise 对象 |
| `state` | in | 目标状态（1 兑现 / 2 拒绝） |
| `value` | in | 兑现值或拒绝原因 |

#### `hpe_js_set_module_loader`

```c
void hpe_js_set_module_loader(HpeJs *js, const HpeJsModuleLoader *loader);
```

注册 `import` 说明符的解析器

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `loader` | in | 模块加载器回调结构 |

#### `hpe_js_set_current_dir`

```c
void hpe_js_set_current_dir(HpeJs *js, const char *dir);
```

设置相对说明符的基准目录

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `dir` | in | 基准目录 |

#### `hpe_js_date_install`

```c
void hpe_js_date_install(HpeJs *js);
```

创建 `Date.prototype` 并安装全局 `Date`

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

#### `hpe_js_date_fields`

```c
int hpe_js_date_fields(JsValue datev, HpeDateFields *out);
```

把 Date 值拆成 UTC 日历字段

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `datev` | in | Date 值 |
| `out` | out | 接收拆解后的字段 |

**返回**：成功返回 1，非 Date 或无效日期返回 0

#### `hpe_js_intl_install`

```c
void hpe_js_intl_install(HpeJs *js);
```

创建 `Intl` 命名空间并挂在全局

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

**注意**：提供 `NumberFormat` / `DateTimeFormat` / `Collator` 及 `getCanonicalLocales`

#### `hpe_regex_compile`

```c
HpeRegex *hpe_regex_compile(HpeJs *js, const char *pattern);
```

编译正则模式串为匹配程序

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `pattern` | in | 模式串，`NULL` 视为空模式 |

**返回**：arena 中新建的正则对象

**注意**：标志位初始为 0，需另行调用 `hpe_regex_set_flags`

#### `hpe_regex_search`

```c
int hpe_regex_search(HpeRegex *re, const char *subject, int start, int *captures, int ncaps);
```

从指定位置起搜索主题串

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `re` | in,out | 已编译的正则对象 |
| `subject` | in | 主题串，`NULL` 视为空串 |
| `start` | in | 起始字节偏移 |
| `captures` | out | 接收捕获组的字节偏移数组 |
| `ncaps` | in | `captures` 的可用长度 |

**返回**：匹配则返回匹配结束后的字节偏移，否则返回 -1

**注意**：`captures[0]/[1]` 为整段匹配，`[2+2g]/[3+2g]` 为第 g 个捕获组；未参与匹配的组置 -1

#### `hpe_regex_group_count`

```c
int hpe_regex_group_count(HpeRegex *re);
```

返回正则的捕获组个数

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `re` | in,out | 正则对象，可为 `NULL` |

**返回**：捕获组个数，`re` 为 `NULL` 时返回 0

#### `hpe_regex_group_name`

```c
const char *hpe_regex_group_name(HpeRegex *re, int index);
```

取指定捕获组的名字

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `re` | in | 已编译的正则对象，可为 `NULL` |
| `index` | in | 捕获组序号（1 起） |

**返回**：组名字符串，无名或越界时返回 `NULL`

#### `hpe_regex_flags`

```c
int hpe_regex_flags(HpeRegex *re);
```

取正则对象的标志位掩码

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `re` | in | 已编译的正则对象，可为 `NULL` |

**返回**：标志位掩码（`i`=1、`m`=2、`s`=4、`u`=8、`y`=16、`d`=32），`re` 为 `NULL` 时返回 0

#### `hpe_regex_set_flags`

```c
void hpe_regex_set_flags(HpeRegex *re, const char *flags);
```

由标志串设置 `/i /m /s` 开关

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `re` | in,out | 目标正则对象 |
| `flags` | in | 标志串 |

**注意**：`i` 忽略大小写、`m` 多行、`s` 让 `.` 匹配换行

#### `js_regex_new`

```c
JsValue js_regex_new(HpeJs *js, const char *pattern, const char *flags);
```

由模式串与标志创建正则值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `pattern` | in | 模式串 |
| `flags` | in | 标志串 |

**返回**：包装正则对象的 JS 值

**参见**：`hpe_regex_compile`、`hpe_regex_set_flags`

#### `js_iter_len`

```c
int js_iter_len(HpeJs *js, JsValue v);
```

求可迭代值的元素个数

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 字符串、数组、Map/Set 或类数组对象 |

**返回**：元素个数，无法迭代时返回 0

**注意**：供展开运算符、`for-of` 与 `Array.from` 使用

**参见**：`js_iter_at`

#### `js_iter_at`

```c
JsValue js_iter_at(HpeJs *js, JsValue v, int i);
```

取可迭代值第 i 个元素

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 可迭代值 |
| `i` | in | 元素下标 |

**返回**：第 i 个元素，越界或不可迭代时返回 `undefined`

**参见**：`js_iter_len`

#### `js_iter_source_item`

```c
JsValue js_iter_source_item(HpeJs *js, JsValue src, int i, int mode);
```

按迭代模式从 Map / Set 取一项

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `src` | in | 数据源（Map / Set） |
| `i` | in | 元素下标 |
| `mode` | in | 模式：1 键、2 值、3 `[键,值]`、4 `[值,值]` |

**返回**：按模式换算出的项

**注意**：迭代器对象（`JS_OBJ_ITER`）的 `next`/展开共用此函数

**参见**：`js_iter_at`

#### `js_is_bigint`

```c
int js_is_bigint(JsValue v);
```

判断一个值是否为 BigInt 原语

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 值 |

**返回**：是 BigInt 返回 1，否则 0

#### `js_bigint_from_i64`

```c
JsValue js_bigint_from_i64(HpeJs *js, long long v);
```

由 `long long` 构造 BigInt

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 源数值 |

**返回**：新的 BigInt 值

#### `js_bigint_from_str`

```c
JsValue js_bigint_from_str(HpeJs *js, const char *digits, int radix);
```

由指定进制的数字串构造 BigInt

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `digits` | in | 数字串（不含符号与进制前缀） |
| `radix` | in | 进制（2..36） |

**返回**：新的 BigInt 值

**注意**：非数字字符被忽略，因此调用方应先做语法校验

#### `js_bigint_to_radix`

```c
char *js_bigint_to_radix(HpeJs *js, JsValue v, int radix);
```

按进制把 BigInt 转成字符串

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | BigInt 值 |
| `radix` | in | 进制（2..36） |

**返回**：arena 中的字符串

**注意**：进制越界时抛出 `RangeError`

#### `js_bigint_is_zero`

```c
int js_bigint_is_zero(JsValue v);
```

判断 BigInt 是否为零

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | BigInt 值 |

**返回**：为零返回 1

#### `js_bigint_sign`

```c
int js_bigint_sign(JsValue v);
```

取 BigInt 的符号

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | BigInt 值 |

**返回**：-1、0 或 1

#### `js_bigint_to_number`

```c
double js_bigint_to_number(JsValue v);
```

把 BigInt 近似转成 double

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | BigInt 值 |

**返回**：最接近的 double；溢出为 ±Infinity

#### `js_bigint_add`

```c
JsValue js_bigint_add(HpeJs *js, JsValue l, JsValue r, int sub);
```

BigInt 加/减

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `l` | in | 左操作数 |
| `r` | in | 右操作数 |
| `sub` | in | 非零表示做减法 |

**返回**：结果

#### `js_bigint_mul`

```c
JsValue js_bigint_mul(HpeJs *js, JsValue l, JsValue r);
```

BigInt 乘法

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `l` | in | 左操作数 |
| `r` | in | 右操作数 |

**返回**：积

#### `js_bigint_divmod`

```c
JsValue js_bigint_divmod(HpeJs *js, JsValue l, JsValue r, int want_rem);
```

BigInt 除法或取模

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `l` | in | 被除数 |
| `r` | in | 除数 |
| `want_rem` | in | 非零表示取模 |

**返回**：商或余数

**注意**：除数为零时抛出 `RangeError`

#### `js_bigint_neg`

```c
JsValue js_bigint_neg(HpeJs *js, JsValue v);
```

BigInt 求负

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 操作数 |

**返回**：`-v` / `-v`

#### `js_bigint_cmp`

```c
int js_bigint_cmp(JsValue l, JsValue r);
```

比较两个 BigInt

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `l` | in | 左操作数 |
| `r` | in | 右操作数 |

**返回**：负数、0 或正数

#### `js_bigint_pow`

```c
JsValue js_bigint_pow(HpeJs *js, JsValue l, JsValue r);
```

BigInt 幂运算

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `l` | in | 底数 |
| `r` | in | 指数 |

**返回**：`l ** r` / `l ** r`

**注意**：指数为负时抛出 `RangeError`

#### `js_bigint_bitop`

```c
JsValue js_bigint_bitop(HpeJs *js, int op, JsValue l, JsValue r);
```

BigInt 按位与/或/异或

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `op` | in | `JS_OP_BAND` / `JS_OP_BOR` / `JS_OP_BXOR` / Operator |
| `l` | in | 左操作数 |
| `r` | in | 右操作数 |

**返回**：结果

**注意**：按无限位二进制补码语义计算

#### `js_bigint_not`

```c
JsValue js_bigint_not(HpeJs *js, JsValue v);
```

BigInt 按位取反

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 操作数 |

**返回**：`~v` / `~v`

#### `js_bigint_shift`

```c
JsValue js_bigint_shift(HpeJs *js, JsValue l, JsValue r, int left);
```

BigInt 移位

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `l` | in | 被移位的值 |
| `r` | in | 移位量 |
| `left` | in | 非零表示左移 |

**返回**：结果

**注意**：右移按算术移位（向下取整）语义

#### `js_bigint_asn`

```c
JsValue js_bigint_asn(HpeJs *js, double bits, JsValue v, int signedp);
```

`BigInt.asIntN` / `BigInt.asUintN` 的共用实现

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `bits` | in | 位宽 |
| `v` | in | 被截断的 BigInt |
| `signedp` | in | 非零表示有符号截断 |

**返回**：截断后的 BigInt

**注意**：位宽非法（NaN、负数）时抛出 `RangeError`

#### `hpe_js_buffer_install`

```c
void hpe_js_buffer_install(HpeJs *js);
```

安装 ArrayBuffer、DataView 与全部 TypedArray

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

**注意**：由全局安装流程调用一次；TypedArray 原型按 `TaKind` 下标存入 `js->ta_proto`

#### `hpe_js_is_typed_array`

```c
int hpe_js_is_typed_array(JsValue v);
```

判断是否为本引擎的 TypedArray

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 待测值 |

**返回**：是 TypedArray 返回 1

**注意**：`Array.isArray` 与 `js_values_equal` 用它排除 TypedArray

#### `js_symbol_new`

```c
JsValue js_symbol_new(HpeJs *js, const char *desc);
```

新建一个唯一符号

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `desc` | in | 描述文本，可为 `NULL` |

**返回**：类型为 `JS_SYMBOL` 的值

#### `js_symbol_well_known`

```c
JsValue js_symbol_well_known(HpeJs *js, const char *desc);
```

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | — | — |
| `desc` | — | — |

#### `js_is_symbol`

```c
int js_is_symbol(JsValue v);
```

判断一个值是否为符号

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 待测值 |

**返回**：是符号返回 1

#### `js_symbol_id`

```c
long long js_symbol_id(JsValue v);
```

取符号的唯一标识

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 符号值 |

**返回**：自增标识；非符号返回 0

#### `js_symbol_desc`

```c
const char *js_symbol_desc(JsValue v);
```

取符号的描述文本

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `v` | in | 符号值 |

**返回**：描述指针，可为 `NULL`

#### `js_symbol_key`

```c
char *js_symbol_key(HpeJs *js, JsValue v);
```

把符号转成属性键

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 符号值 |

**返回**：合成键 `"\x01#<id>"`

#### `js_key_is_symbol`

```c
int js_key_is_symbol(const char *key);
```

判断属性键是否代表符号

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `key` | in | 属性键 |

**返回**：是符号键返回 1

#### `js_symbol_text`

```c
char *js_symbol_text(HpeJs *js, JsValue v);
```

取符号的 `Symbol(desc)` 文本

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 符号值 |

**返回**：arena 中的字符串

#### `js_symbol_for`

```c
JsValue js_symbol_for(HpeJs *js, const char *key);
```

在全局注册表中查取或新建符号

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `key` | in | 注册键 |

**返回**：该键对应的共享符号

#### `js_symbol_key_for`

```c
JsValue js_symbol_key_for(HpeJs *js, JsValue v);
```

反向查询注册键 `/` `Symbol.keyFor`

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 符号值 |

**返回**：注册键字符串；未注册时返回 `undefined`

#### `hpe_js_symbol_install`

```c
void hpe_js_symbol_install(HpeJs *js);
```

安装 `Symbol` 构造器、原型与全部知名符号

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

#### `hpe_js_gen_create`

```c
JsValue hpe_js_gen_create(HpeJs *js, JsObj *fn, JsEnv *closure, JsValue this_v, JsValue *args, int argc);
```

创建一个惰性生成器

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `fn` | in,out | 生成器函数对象 |
| `closure` | in,out | 闭包捕获的环境 |
| `this_v` | in | 调用时的 this 值 |
| `args` | in,out | 实参数组 |
| `argc` | in | 实参个数 |

**返回**：生成器对象值；创建协程失败时返回一个已结束的生成器

**注意**：函数体此时不执行，只建立协程

#### `hpe_js_gen_of`

```c
HpeJsGen *hpe_js_gen_of(JsObj *obj);
```

取对象背后的生成器状态

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `obj` | in | 待检查的对象，可为 `NULL` |

**返回**：生成器状态；非生成器对象返回 `NULL`

#### `hpe_js_gen_pull`

```c
int hpe_js_gen_pull(HpeJs *js, JsObj *obj, JsValue *out);
```

从生成器拉取下一项

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `obj` | in,out | 生成器对象 |
| `out` | out | 接收 `{ value, done }` |

**返回**：仍在运行返回 1；已结束返回 0

#### `hpe_js_gen_enter`

```c
void hpe_js_gen_enter(HpeJs *js, HpeJsGen *g);
```

登记当前正在执行的生成器

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `g` | in,out | 生成器状态 |

#### `hpe_js_gen_active`

```c
HpeJsGen *hpe_js_gen_active(HpeJs *js);
```

取出当前正在执行的生成器

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

**返回**：生成器状态；不在生成器体内时为 `NULL`

#### `hpe_js_gen_set_yielded`

```c
void hpe_js_gen_set_yielded(HpeJs *js, HpeJsGen *g, JsValue value);
```

记录本次 `yield` 产出的值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `g` | in,out | 生成器状态 |
| `value` | in | 产出的值 |

#### `hpe_js_gen_get_sent`

```c
JsValue hpe_js_gen_get_sent(HpeJs *js, HpeJsGen *g);
```

取回 `yield` 表达式的值（`next(v)` 送入的值）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `g` | in,out | 生成器状态 |

**返回**：送入的值

#### `hpe_js_gen_coro`

```c
void *hpe_js_gen_coro(HpeJs *js, HpeJsGen *g);
```

取生成器挂起所需的协程

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `g` | in,out | 生成器状态 |

**返回**：协程句柄

#### `hpe_js_gen_leave`

```c
void hpe_js_gen_leave(HpeJs *js);
```

清除当前生成器登记

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

#### `hpe_js_gen_suspend`

```c
void hpe_js_gen_suspend(HpeJs *js, JsValue value);
```

让当前生成器在 `yield` 处挂起

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `value` | in | 本次产出的值 |

**注意**：只在 `gen_active` 非空时有效；返回时 `sent` 为 `next(v)` 送入的值

#### `hpe_js_gen_take_pending_throw`

```c
int hpe_js_gen_take_pending_throw(HpeJs *js, HpeJsGen *g, JsValue *out);
```

取走 `throw(v)` 停放的异常

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `g` | in,out | 生成器状态 |
| `out` | out | 接收异常值 |

**返回**：有待抛异常返回 1

**注意**：由 `yield` 恢复点在**协程栈上**调用

#### `hpe_js_gen_take_pending_return`

```c
int hpe_js_gen_take_pending_return(HpeJs *js, HpeJsGen *g, JsValue *out);
```

取走 `return(v)` 停放的返回完成标记

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `g` | in,out | 生成器状态 |
| `out` | out | 接收 `js->gen_ret_sentinel` |

**返回**：有待处理返回完成时返回 1

**注意**：由 `yield` 恢复点在**协程栈上**调用

#### `js_run_generator_body`

```c
JsValue js_run_generator_body(HpeJs *js, JsObj *fn, JsEnv *closure, JsValue this_v, JsValue *args, int argc);
```

在独立栈上运行生成器函数体

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `fn` | in,out | 生成器函数对象 |
| `closure` | in,out | 捕获的环境 |
| `this_v` | in | 调用时的 this 值 |
| `args` | in,out | 实参数组 |
| `argc` | in | 实参个数 |

**返回**：函数体的返回值

**注意**：由 `hpe_js_gen.c` 的协程入口调用，实现留在解释器里以便访问静态执行器

#### `js_run_async_body`

```c
JsValue js_run_async_body(HpeJs *js, JsObj *fn, JsEnv *closure, JsValue this_v, JsValue *args, int argc);
```

在独立栈上运行 async 函数体

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `fn` | in,out | async 函数对象 |
| `closure` | in,out | 捕获的环境 |
| `this_v` | in | 调用时的 this 值 |
| `args` | in,out | 实参数组 |
| `argc` | in | 实参个数 |

**返回**：函数体的返回值

**注意**：由 `hpe_js_async.c` 的协程入口调用，实现留在解释器里以便访问静态执行器

#### `js_promise_on_settle`

```c
void js_promise_on_settle(HpeJs *js, JsObj *p, JsNativeFn on_ok, JsNativeFn on_err, void *ud);
```

在 Promise 上登记一对 C 函数反应

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `p` | in,out | 目标 Promise |
| `on_ok` | in | 兑现时调用的 C 函数 |
| `on_err` | in | 拒绝时调用的 C 函数 |
| `ud` | in,out | 传给两者的用户数据 |

**注意**：与 `then` 的差别是不产生下游 Promise，仅供 `await` 这类内部消费者使用

#### `js_resolve_promise`

```c
void js_resolve_promise(HpeJs *js, JsObj *p, JsValue v);
```

按 thenable 规则用值兑现 Promise

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `p` | in,out | 待兑现的 Promise |
| `v` | in | 兑现值 |

**参见**：`js_promise_on_settle`

#### `hpe_js_await`

```c
JsValue hpe_js_await(HpeJs *js, JsValue v);
```

`await` 的实现入口

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `v` | in | 被 await 的值 |

**返回**：兑现值

**注意**：实现在 `hpe_js_async.c`，这样挂起逻辑与协程管理放在一起

#### `hpe_js_async_create`

```c
JsValue hpe_js_async_create(HpeJs *js, JsObj *fn, JsEnv *closure, JsValue this_v, JsValue *args, int argc);
```

创建 async 调用

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `fn` | in,out | async 函数对象 |
| `closure` | in,out | 捕获的环境 |
| `this_v` | in | 调用时的 this 值 |
| `args` | in,out | 实参数组 |
| `argc` | in | 实参个数 |

**返回**：立即返回的 promise

#### `hpe_js_async_park`

```c
void hpe_js_async_park(HpeJs *js, JsObj *p);
```

挂起当前 async 体直到 promise 定局

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |
| `p` | in,out | 被 await 的 promise（必须仍为 pending） |

**类型**

#### `JsObjKind`

```c
typedef enum {
    JS_OBJ_PLAIN,
    JS_OBJ_ARRAY,
    JS_OBJ_FUNC,
    JS_OBJ_NATIVE,
    JS_OBJ_HOST,
    JS_OBJ_PROMISE,
    JS_OBJ_REGEX,
    JS_OBJ_MODULE,
    JS_OBJ_ERROR,
    JS_OBJ_MAP,
    JS_OBJ_ARGUMENTS,
    JS_OBJ_DATE,
    JS_OBJ_PROXY,
    JS_OBJ_WEAKMAP,
    JS_OBJ_ITER,
    JS_OBJ_GEN
} JsObjKind;
```

#### `TaKind`

```c
typedef enum {
    TA_INT8 = 0,
    TA_UINT8,
    TA_UINT8C,
    TA_INT16,
    TA_UINT16,
    TA_INT32,
    TA_UINT32,
    TA_FLOAT32,
    TA_FLOAT64,
    TA_BIGINT64,
    TA_BIGUINT64,
    k_ta_count
} TaKind;
```

#### `JsProp`

```c
typedef struct JsProp {
    char *key;
    JsValue val;
    JsValue getter;
    JsValue setter;
    int enumerable;
} JsProp;
```

#### `JsEnv`

```c
typedef struct JsEnv JsEnv;
```

#### `JsNode`

```c
typedef struct JsNode JsNode;
```

#### `JsBlock`

```c
typedef struct JsBlock JsBlock;
```

#### `JsReaction`

```c
typedef struct JsReaction JsReaction;
```

#### `JsJob`

```c
typedef struct JsJob JsJob;
```

#### `JsModule`

```c
typedef struct JsModule JsModule;
```

#### `HpeJs`

```c
typedef struct HpeJs HpeJs;
```

#### `JsTryFrame`

```c
typedef struct JsTryFrame {
    jmp_buf jmp;
    struct JsTryFrame *prev;
    int depth; /* call-depth snapshot taken at push, restored on pop (longjmp-safe) / 压入时的调用深度快照，弹出时恢复（对 longjmp 安全） */
} JsTryFrame;
```

#### `HpeJsFiberCtx`

```c
typedef struct HpeJsFiberCtx {
    JsTryFrame *coro_try_top;    /* the coroutine side's try chain / 协程侧的 try 链 */
    jmp_buf coro_jmp;            /* the coroutine side's landing pad / 协程侧的兜底着陆点 */
    JsTryFrame *resumer_try_top; /* the resumer side's try chain, stashed while inside / 进入协程时暂存的调用方 try 链 */
    jmp_buf resumer_jmp;         /* the resumer side's landing pad, stashed while inside / 进入协程时暂存的调用方着陆点 */
    int resumer_depth;           /* the resumer's call depth, stashed while inside / 进入协程时暂存的调用方调用深度 */
} HpeJsFiberCtx;
```

一个协程栈的异常上下文

#### `JsVar`

```c
typedef struct JsVar {
    char *name;
    JsValue val;
} JsVar;
```

#### `JsNode`

```c
typedef struct JsNode {
    int kind;
    int op;
    double num;
    char *str;
    struct JsNode **kids;
    int kid_count;
    size_t kid_cap;
    int line;
    int strict; /* program root: script is strict (directive prologue / module) */
} JsNode;
```

#### `HpeJsFiberScan`

```c
typedef void (*HpeJsFiberScan)(HpeJs *js, void *state);
```

#### `JsChunkHdr`

```c
typedef struct JsChunkHdr {
    size_t size;   /* payload size, 8-aligned / 载荷字节数，8 字节对齐 */
    uint16_t tag;  /* GC_T_* allocator hint / 分配器提示标签 */
    uint16_t mark; /* set while the chunk is reachable / 可达时置位 */
    uint32_t magic;
} JsChunkHdr;
```

#### `HpeDateFields`

```c
typedef struct HpeDateFields {
    int year;
    int month; /* 1..12 */
    int day;
    int weekday; /* 0 = Sunday */
    int hour;
    int minute;
    int second;
    int ms;
} HpeDateFields;
```

拆解后的公历字段（供 `Intl.DateTimeFormat` 使用）

#### `HpeRegex`

```c
/* regex (hpe_js_regex.c): compiled once per literal, matched on demand */typedef struct HpeRegex HpeRegex;
```

#### `HpeJsGen`

```c
typedef struct HpeJsGen HpeJsGen;
```

### hpe_js_mutation.h

4 个函数，1 个类型。

**函数**

#### `hpe_js_mutation_state`

```c
HpeJsMutationState *hpe_js_mutation_state(HpeJs *js);
```

取（必要时创建）解释器的观察者登记状态

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文，可为 `NULL` |

**返回**：解释器级状态块；`js` 为 `NULL` 时返回 `NULL`

**注意**：状态块由 arena 分配，字段含观察者链表、所属 `js` 与 `sink_installed`

#### `hpe_js_mutation_install`

```c
void hpe_js_mutation_install(HpeJs *js);
```

注册 DOM 变更接收器并安装全局 `MutationObserver`

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

**参见**：`hpe_js_mutation_uninstall`、`hpe_js_mutation_install_ctor`

#### `hpe_js_mutation_uninstall`

```c
void hpe_js_mutation_uninstall(HpeJs *js);
```

注销 DOM 变更接收器

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

**注意**：由 `hpe_js_mutation_install` 注册的接收器在此摘除

**参见**：`hpe_js_mutation_install`

#### `hpe_js_mutation_install_ctor`

```c
void hpe_js_mutation_install_ctor(HpeJs *js);
```

安装全局 `MutationObserver` 构造器

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `js` | in,out | JS 运行时上下文 |

**参见**：`hpe_js_mutation_install`

**类型**

#### `HpeJsMutationState`

```c
typedef struct HpeJsMutationState {
    void *observers;      /* JsMutationObserver list (opaque) */
    HpeJs *js;
    int sink_installed;   /* the DOM mutation sink is registered */
} HpeJsMutationState;
```

### hpe_js_selection.h

0 个函数，0 个类型。

**函数**

无。

**类型**

无。

## src/layout（布局）

### hpe_layout.h

5 个函数，7 个类型。

**函数**

#### `hpe_layout_build`

```c
HpeBox *hpe_layout_build(HpeDomNode *document, HpeStylesheet *sheet, float viewport_width, float viewport_height);
```

为文档构建并排版盒子树

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `document` | in | 文档根节点 |
| `sheet` | in | 样式表 |
| `viewport_width` | in | 视口宽度 |
| `viewport_height` | in | 视口高度 |

**返回**：盒子树根；无文档或 `<html>` 时返回 `NULL`

**注意**：视口即初始包含块

#### `hpe_layout_rebuild`

```c
HpeBox *hpe_layout_rebuild(HpeDomNode *document, HpeStylesheet *sheet, float viewport_width, float viewport_height, HpeBox *old_root, HpeDomNode *reflow_root);
```

DOM 变更后的增量重建

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `document` | in | 文档根节点 |
| `sheet` | in | 样式表 |
| `viewport_width` | in | 视口宽度 |
| `viewport_height` | in | 视口高度 |
| `old_root` | in,out | 上一轮的盒子树 |
| `reflow_root` | in | 子树发生变更的最高 DOM 节点 |

**返回**：成功时返回复用的 old_root；输入不允许局部重建时返回 `NULL`

**注意**：变更子树之外的盒原样复用，几何结果与 hpe_layout_build 一致

#### `hpe_layout_free`

```c
void hpe_layout_free(HpeBox *box);
```

释放整棵盒子树

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `box` | in,out | 待释放的盒；可为 `NULL` |

#### `hpe_layout_content_height`

```c
float hpe_layout_content_height(HpeBox *root);
```

求根盒的总内容高度

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `root` | in | 根盒 |

**返回**：根盒边框盒底边的文档 y；root 为 `NULL` 时返回 0

#### `form_is_replaced_control`

```c
int form_is_replaced_control(const HpeDomNode *node);
```

判断一个元素是否按替换盒处理的表单控件

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in | 目标元素 |

**返回**：input（button 型除外）与 select / progress / meter 返回 1

**类型**

#### `HpeBoxKind`

```c
typedef enum HpeBoxKind {
    HPE_BOX_BLOCK = 0,
    HPE_BOX_INLINE,
    HPE_BOX_TEXT,
    HPE_BOX_INLINE_BLOCK
} HpeBoxKind;
```

#### `HpeTextRun`

```c
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
```

#### `HpeLine`

```c
typedef struct HpeLine {
    HpeTextRun *runs;
    int run_count;
    size_t run_cap;
    float width;
    float height;
    float baseline;
    float y;
    /* vertical writing mode reinterprets this line as a text column:
     * `col_x` is the column's left offset relative to the content box,
     * `width` is the column width, and each run's `x`/`width` become the
     * vertical start offset and vertical length. */
    float col_x;
    /* per-line fragments of the inline boxes that contribute content: one
     * entry per still-open inline box when the line ends or closes */
    struct HpeInlineFrag *frags;
    int frag_count;
    size_t frag_cap;
} HpeLine;
```

#### `HpeInlineFrag`

```c
typedef struct HpeInlineFrag {
    struct HpeBox *box;
    float x0, x1;          /* content extent on this line */
    float ascent, descent; /* the inline box's own font metrics */
    int flags;             /* 1 = box starts here (paint left pad/border),
                              2 = box ends here (paint right pad/border) */
} HpeInlineFrag;
```

#### `HpeFloatBox`

```c
typedef struct HpeFloatBox {
    struct HpeBox *box;
    float top, bottom; /* band extent in document coords (margin box) */
    float left, right; /* horizontal extent in document coords (margin box) */
    int side;          /* HpeFloatSide */
} HpeFloatBox;
```

#### `HpeFloatBand`

```c
typedef struct HpeFloatBand {
    HpeFloatBox *items;
    int count;
    size_t cap;
} HpeFloatBand;
```

#### `HpeBox`

```c
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
```

## src/paint（绘制）

### hpe_paint.h

4 个函数，2 个类型。

**函数**

#### `hpe_paint_set_selection`

```c
void hpe_paint_set_selection(const HpePaintSel *sel);
```

设置绘制阶段的选区描述（下一帧起生效）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `sel` | in | 选区描述，NULL 清除 |

**注意**：指针由调用方持有且须在绘制期间保持有效；引擎在每次重绘前重建并传入

#### `hpe_paint_document`

```c
void hpe_paint_document(HpePixmap *pm, HpeBox *root, float scroll_y, HpeColor page_background, float origin_y);
```

绘制整个文档

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `root` | in | 盒子树根，可为 `NULL` |
| `scroll_y` | in | 纵向滚动偏移 |
| `page_background` | in | 页面背景色 |
| `origin_y` | in | 页面视口顶边的设备 y |

**注意**：内容被裁剪在 [origin_y, 像素缓冲高) 之内，避免画到浏览器外框上

#### `hpe_paint_region`

```c
void hpe_paint_region(HpePixmap *pm, HpeBox *root, float scroll_y, HpeColor page_background, HpeRect clip, float origin_y);
```

只重绘裁剪区域内的文档

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `root` | in | 盒子树根，可为 `NULL` |
| `scroll_y` | in | 纵向滚动偏移 |
| `page_background` | in | 页面背景色 |
| `clip` | in | 设备坐标下的裁剪矩形 |
| `origin_y` | in | 页面视口顶边的设备 y |

**注意**：供滚动损伤跟踪使用，clip 与页面视口求交

#### `hpe_paint_page_scrollbar`

```c
void hpe_paint_page_scrollbar(HpePixmap *pm, float view_top, float view_h, float scroll_y, float content_h, float width);
```

绘制页面级纵向滚动条

**详细说明**：视口右缘的一条 `width` 宽槽：轨道浅灰，滑块深灰、按 `scroll_y / max_scroll` 在 `[0, track_h - thumb_h]` 内线性投影，滑块高 = 视口高 ×（视口/内容）。 无溢出（`content_h <= view_h`）时直接返回不绘。 A `width`-wide gutter down the viewport's right edge: a light track with a darker thumb projected linearly into `[0, track_h - thumb_h]` by `scroll_y / max_scroll`; the thumb's height is the viewport/content ratio. Returns without drawing when the content does not overflow (`content_h <= view_h`).

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `view_top` | in | 视口顶边的设备 y |
| `view_h` | in | 视口高度（像素） |
| `scroll_y` | in | 当前滚动偏移 |
| `content_h` | in | 内容总高 |
| `width` | in | 滚动条宽度（像素），不大于 0 时不绘 |

**注意**：纯 chrome：调用方应在内容绘制并解除裁剪后于右缘覆盖调用

**参见**：`hpe_paint_document`

**类型**

#### `HpePaintSelEntry`

```c
typedef struct HpePaintSelEntry {
    const struct HpeBox *box; /* 选区命中的文本盒 / The selected text box */
    int start, end;           /* box->text 内的选中字节区间 [start, end) / Byte range within box->text */
} HpePaintSelEntry;
```

一个文本盒的选中字节区间

#### `HpePaintSel`

```c
typedef struct HpePaintSel {
    const HpePaintSelEntry *entries; /* 区间表，可为 NULL / Range table, may be NULL */
    int count;                       /* 区间个数 / Number of entries */
} HpePaintSel;
```

本帧选区描述：文档序排列的逐盒区间表

### hpe_paint_details.h

4 个函数，0 个类型。

**函数**

#### `hpe_paint_set_modal_dialog`

```c
void hpe_paint_set_modal_dialog(HpeDomNode *node);
```

记录当前模态 `<dialog>` 节点供绘制使用

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in | 模态对话框节点或 `NULL` |

**注意**：引擎在每帧绘制前调用；指针只在本帧读取

#### `hpe_paint_modal_dialog`

```c
HpeDomNode *hpe_paint_modal_dialog(void);
```

读取当前登记的模态 `<dialog>`

**返回**：模态对话框节点，无则 `NULL`

#### `hpe_paint_summary_marker`

```c
void hpe_paint_summary_marker(HpePixmap *pm, const HpeBox *box, HpeRect bb, int open);
```

绘制 `<summary>` 的展开指示三角

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `box` | in | summary 盒 |
| `bb` | in | 设备坐标边框盒 |
| `open` | in | 所属 details 是否展开 |

**注意**：三角画在 `padding-left` 预留的左侧空档

#### `hpe_paint_dialog_backdrop`

```c
void hpe_paint_dialog_backdrop(HpePixmap *pm, float y);
```

绘制模态 `<dialog>` 的背景遮罩

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `y` | in | 视口内容区顶边的设备 y |

**注意**：半透明黑色覆盖整个内容区；在非模态绘制阶段之前调用

**类型**

无。

### hpe_paint_form.h

2 个函数，0 个类型。

**函数**

#### `hpe_paint_form_set_focus`

```c
void hpe_paint_form_set_focus(const HpeDomNode *node, int caret);
```

记录当前聚焦的可编辑控件与插入符位置（绘制插入符用）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in | 焦点控件，`NULL` 表示无 |
| `caret` | in | 插入符在值内的字节偏移 |

**注意**：引擎在每帧绘制前调用；指针只在本帧读取

#### `hpe_paint_form_control`

```c
void hpe_paint_form_control(HpePixmap *pm, const HpeBox *box, HpeRect bb);
```

绘制一个替换型表单控件的控件内容

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `box` | in | 控件盒 |
| `bb` | in | 设备坐标边框盒 |

**类型**

无。

### hpe_svg.h

2 个函数，0 个类型。

**函数**

#### `hpe_svg_paint`

```c
void hpe_svg_paint(HpePixmap *pm, struct HpeBox *box, float y_dev);
```

绘制 `<svg>` 子树

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `box` | in | `<svg>` 盒 |
| `y_dev` | in | 盒子顶边的设备 y |

**注意**：内容直接来自 DOM，不经过盒子树

#### `hpe_svg_hit_test`

```c
struct HpeDomNode *hpe_svg_hit_test(struct HpeBox *box, float x, float y);
```

对 `<svg>` 子树做命中测试

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `box` | in | `<svg>` 盒 |
| `x` | in | 文档坐标 X |
| `y` | in | 文档坐标 Y |

**返回**：最深的包含该点的 SVG 元素（`<rect>` 上的点击会冒泡到其 `<g>`），未命中返回 `NULL`

**注意**：路径目前尚未参与命中测试

**类型**

无。

## src/platform（平台层）

### hpe_platform.h

34 个函数，7 个类型。

**函数**

#### `hpe_window_create`

```c
HpeWindow *hpe_window_create(const char *title, int width, int height, const HpeWindowCallbacks *callbacks, void *user);
```

创建平台窗口并绑定回调

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `title` | in | 窗口标题 |
| `width` | in | 客户区宽度（像素） |
| `height` | in | 客户区高度（像素） |
| `callbacks` | in | 窗口回调表，可为 NULL |
| `user` | in,out | 透传给每个回调的用户数据 |

**返回**：新窗口对象，失败返回 NULL

**注意**：窗口是运行期间唯一的顶层窗口；调用方随后需 `hpe_window_destroy`

**参见**：`hpe_window_run`、`hpe_window_destroy`

#### `hpe_window_present`

```c
void hpe_window_present(HpeWindow *win, const void *bgra_pixels, int width, int height, int stride_bytes);
```

提交一帧 BGRA 像素并请求重绘

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `win` | in,out | 目标窗口 |
| `bgra_pixels` | in | BGRA32 像素数据 |
| `width` | in | 像素宽度 |
| `height` | in | 像素高度 |
| `stride_bytes` | in | 每行字节数 |

**注意**：缓冲需在窗口下次重绘前保持有效

**参见**：`hpe_window_invalidate`

#### `hpe_window_set_title`

```c
void hpe_window_set_title(HpeWindow *win, const char *title);
```

设置窗口标题栏文本

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `win` | in,out | 窗口对象 |
| `title` | in | 新的标题文本（UTF-8） |

**参见**：`hpe_window_create`

#### `hpe_window_invalidate`

```c
void hpe_window_invalidate(HpeWindow *win);
```

请求重绘整个客户区

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `win` | in,out | 目标窗口 |

**参见**：`hpe_window_present`

#### `hpe_window_request_close`

```c
void hpe_window_request_close(HpeWindow *win);
```

异步请求关闭窗口

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `win` | in,out | 目标窗口 |

**参见**：`hpe_window_run`

#### `hpe_window_run`

```c
int hpe_window_run(HpeWindow *win); /* blocks until the window closes */
```

进入消息循环直到窗口关闭

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `win` | in,out | 目标窗口 |

**返回**：进程退出码

**注意**：阻塞调用，窗口关闭后返回

**参见**：`hpe_window_request_close`

#### `hpe_window_destroy`

```c
void hpe_window_destroy(HpeWindow *win);
```

销毁窗口并释放窗口对象

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `win` | in,out | 目标窗口 |

**参见**：`hpe_window_create`

#### `hpe_time_seconds`

```c
double hpe_time_seconds(void); /* monotonic seconds since an unspecified origin */
```

读取单调时钟秒数

**返回**：自某个未指定起点起的单调秒数

#### `hpe_time_epoch_ms`

```c
double hpe_time_epoch_ms(void);
```

读取自 Unix 纪元起的 UTC 墙钟毫秒

**返回**：自 1970-01-01 UTC 起的毫秒数

**注意**：`Date.now` 的数据来源

**参见**：`hpe_time_seconds`

#### `hpe_time_utc_offset_minutes`

```c
int hpe_time_utc_offset_minutes(double epoch_ms, char *out_name, size_t cap, int *out_dst);
```

求某个 UTC 时刻的系统时区偏移

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `epoch_ms` | in | 自 Unix 纪元起的 UTC 毫秒 |
| `out_name` | out | 接收时区名的缓冲，可为 `NULL` |
| `cap` | in | `out_name` 的容量（字节，UTF-8） |
| `out_dst` | out | 接收是否为夏令时的标志，可为 `NULL` |

**返回**：本地时间相对 UTC 的偏移分钟数（东为正，与 `getTimezoneOffset()` 反号）

**注意**：按**该时刻**的历史时区规则求解，因此跨夏令时切换点前后会得到不同偏移；无时区数据库的平台上退化为固定偏移

**参见**：`hpe_time_epoch_ms`

#### `hpe_coro_resume`

```c
int hpe_coro_resume(HpeCoro *coro);
```

把控制权交给协程，直到它 `yield` 或结束

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `coro` | in,out | 目标协程 |

**返回**：协程仍在运行（只是 `yield` 了）返回 1；已结束返回 0

**注意**：只能从创建它的线程调用

#### `hpe_coro_yield`

```c
void hpe_coro_yield(HpeCoro *coro);
```

从协程内部把控制权交还调用者

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `coro` | in,out | 当前协程 |

**注意**：只允许在协程入口函数内部调用

#### `hpe_coro_destroy`

```c
void hpe_coro_destroy(HpeCoro *coro);
```

销毁协程并释放其栈

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `coro` | in,out | 目标协程；`NULL` 时无操作 |

**注意**：仍在运行的协程会被直接丢弃，不会执行清理

#### `hpe_coro_stack_range`

```c
int hpe_coro_stack_range(HpeCoro *coro, void **lo, void **hi);
```

查询协程栈的边界（供 GC 保守扫描挂起协程）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `coro` | in | 协程句柄 |
| `lo` | out | 栈低端地址（含） |
| `hi` | out | 栈高端地址（不含） |

**返回**：边界可用返回 1；协程为空或从未挂起过返回 0

#### `hpe_read_file`

```c
char *hpe_read_file(const char *path, size_t *out_size); /* NUL terminated, hpe_free() it */
```

读取整个文件到新分配的缓冲

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `path` | in | 文件路径 |
| `out_size` | out | 接收读入的字节数，可为 NULL |

**返回**：以 NUL 结尾的内容缓冲，失败返回 NULL

**注意**：调用方需用 `hpe_free` 释放

#### `hpe_path_dir`

```c
void hpe_path_dir(const char *path, char *out, size_t out_size);
```

取出路径的目录部分

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `path` | in | 输入路径 |
| `out` | out | 接收目录的缓冲 |
| `out_size` | in | 接收缓冲区容量 |

#### `hpe_path_is_absolute`

```c
int hpe_path_is_absolute(const char *path);
```

判断路径是否为绝对路径

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `path` | in | 待检查路径 |

**返回**：绝对路径返回 1，否则 0

#### `hpe_path_join`

```c
void hpe_path_join(char *out, size_t out_size, const char *dir, const char *rel);
```

把相对路径接到目录上

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `out` | out | 接收拼接结果的缓冲 |
| `out_size` | in | 接收缓冲区容量 |
| `dir` | in | 基准目录 |
| `rel` | in | 相对路径 |

#### `hpe_path_absolute`

```c
void hpe_path_absolute(char *out, size_t out_size, const char *path);
```

把路径解析为绝对路径

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `out` | out | 接收绝对路径的缓冲 |
| `out_size` | in | 接收缓冲区容量 |
| `path` | in | 输入路径 |

**注意**：相对进程工作目录解析；失败时原样拷回，供地址栏显示绝对文件路径

#### `hpe_clipboard_get_text`

```c
char *hpe_clipboard_get_text(void);
```

取剪贴板文本或首个文件路径

**返回**：以 UTF-8 编码的剪贴板内容（需 `hpe_free`），无文本时返回 NULL

**注意**：资源管理器复制的文件以首个文件路径的形式暴露，符合地址栏用户的预期

**参见**：`hpe_clipboard_set_text`

#### `hpe_clipboard_set_text`

```c
int hpe_clipboard_set_text(const char *utf8);
```

用 UTF-8 文本替换剪贴板内容

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `utf8` | in | 待写入的 UTF-8 文本 |

**返回**：成功返回 1，失败返回 0

**注意**：失败时为无操作

**参见**：`hpe_clipboard_get_text`

#### `hpe_plat_font_create`

```c
HpePlatFont *hpe_plat_font_create(float pixel_size, int weight, HpeFontFamily family);
```

创建平台字体对象

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pixel_size` | in | 字号（像素） |
| `weight` | in | 字重 |
| `family` | in | 字体族 |

**返回**：新字体对象，失败返回 NULL

**参见**：`hpe_plat_font_destroy`

#### `hpe_plat_font_destroy`

```c
void hpe_plat_font_destroy(HpePlatFont *font);
```

销毁字体对象并释放其资源

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 待销毁的字体 |

**参见**：`hpe_plat_font_create`

#### `hpe_plat_font_ascent`

```c
float hpe_plat_font_ascent(HpePlatFont *font);
```

取字体的上伸部

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象 |

**返回**：上伸部像素数

#### `hpe_plat_font_descent`

```c
float hpe_plat_font_descent(HpePlatFont *font);
```

取字体的下伸部

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象 |

**返回**：下伸部像素数

#### `hpe_plat_font_line_height`

```c
float hpe_plat_font_line_height(HpePlatFont *font);
```

取字体的行高

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象 |

**返回**：行高像素数

#### `hpe_plat_font_advance`

```c
float hpe_plat_font_advance(HpePlatFont *font, uint32_t codepoint);
```

取单个码点的步进宽度

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象 |
| `codepoint` | in | Unicode 码点 |

**返回**：步进宽度（像素）

#### `hpe_plat_font_mask`

```c
int hpe_plat_font_mask(HpePlatFont *font, const char *utf8, int len, float dx, HpeTextMask *out);
```

把 UTF-8 文本渲染成 8 位覆盖率掩码

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象 |
| `utf8` | in | 待渲染的 UTF-8 文本 |
| `len` | in | 文本字节数 |
| `dx` | in | 水平亚像素相位（0..1），非整数平移参与抗锯齿 |
| `out` | out | 接收掩码的结构 |

**返回**：成功返回 1，失败返回 0

**注意**：覆盖区相对基线：x 从 0 起，y 在 `[-descent, ascent)`；`baseline` 为基线所在行索引

**参见**：`hpe_text_mask_free`

#### `hpe_plat_font_covers`

```c
int hpe_plat_font_covers(HpePlatFont *font, uint32_t codepoint);
```

判断字体是否覆盖某个码点

**详细说明**：对每个码点做一次字形索引查询并标记不存在的字形；结果由调用方缓存。 Each query maps the code point to a glyph index marking non-existing glyphs; the caller is expected to cache the result.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象 |
| `codepoint` | in | Unicode 码点 |

**返回**：覆盖返回 1，未覆盖或字体为空返回 0

**参见**：`hpe_plat_font_create`

#### `hpe_plat_font_create_face`

```c
HpePlatFont *hpe_plat_font_create_face(float pixel_size, int weight, const char *face_utf8);
```

用显式字体名创建平台字体对象

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pixel_size` | in | 字号（像素） |
| `weight` | in | 字重 |
| `face_utf8` | in | 字体名（UTF-8），NULL 时按字体族映射 |

**返回**：新字体对象，失败返回 NULL

**注意**：供字形回退链按具体字体名创建后备字体

**参见**：`hpe_plat_font_create`、`hpe_plat_font_destroy`

#### `hpe_plat_emoji_font_create`

```c
HpePlatEmojiFont *hpe_plat_emoji_font_create(float pixel_size);
```

打开彩色 emoji 字体（按字号）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pixel_size` | in | 字号（像素） |

**返回**：新对象，系统无彩色 emoji 字体时返回 NULL

**注意**：Win32 后端使用 `Segoe UI Emoji`；对象只承载 DC 与字体句柄

**参见**：`hpe_plat_emoji_font_destroy`

#### `hpe_plat_emoji_glyph`

```c
int hpe_plat_emoji_glyph(HpePlatEmojiFont *font, uint32_t glyph_id, HpeGlyphOutline *out);
```

按字形索引取抗锯齿覆盖率轮廓

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | emoji 字体对象 |
| `glyph_id` | in | 字形索引 |
| `out` | out | 接收轮廓的结构，coverage 由调用方 `hpe_free` |

**返回**：成功返回 1，空字形或失败返回 0

**注意**：内部使用 `GetGlyphOutlineW` 的 `GGO_GLYPH_INDEX|GGO_GRAY8_BITMAP` 形式

**参见**：`hpe_plat_emoji_font_create`

#### `hpe_plat_emoji_font_destroy`

```c
void hpe_plat_emoji_font_destroy(HpePlatEmojiFont *font);
```

关闭彩色 emoji 字体并释放资源

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | emoji 字体对象，NULL 时无操作 |

**参见**：`hpe_plat_emoji_font_create`

#### `hpe_text_mask_free`

```c
void hpe_text_mask_free(HpeTextMask *mask);
```

释放文本掩码

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `mask` | in,out | 待释放的掩码 |

**参见**：`hpe_plat_font_mask`

**类型**

#### `HpeWindow`

```c
typedef struct HpeWindow HpeWindow;
```

#### `HpeWindowCallbacks`

```c
typedef struct HpeWindowCallbacks {
    void (*on_paint)(void *user);
    void (*on_resize)(void *user, int width, int height);
    void (*on_scroll)(void *user, float delta_lines);
    /* virtual-key press; `mods` is a mask of HPE_MOD_* captured at the moment
     * of the key event (needed for shortcuts such as Ctrl+V) */
    void (*on_key)(void *user, int vk_code, int mods);
    /* virtual-key release, mirroring on_key; pages receive a `keyup` for it.
     * May be NULL, in which case no `keyup` is delivered. */
    void (*on_key_up)(void *user, int vk_code, int mods);
    /* typed text: unicode code point (ASCII today; surrogate pairs are skipped
     * by the Win32 backend). Used by the address bar for path entry and by
     * pages as the source of `keypress`. */
    void (*on_char)(void *user, int codepoint);
    /* IME composition updated: `utf8` is the in-progress string ("" when the
     * composition was cleared or ended without a result). May be NULL. */
    void (*on_ime_compose)(void *user, const char *utf8);
    /* IME composition committed: `utf8` is the final string to insert. May be
     * NULL. */
    void (*on_ime_commit)(void *user, const char *utf8);
    /* mouse: x,y are viewport (client) coords; button 0 = left; down: 1 = pressed, 0 = released;
     * button -1 = pointer move (no button state change). `mods` is a mask of
     * HPE_MOD_* captured with the event (Shift+click extends a text selection).
     * A left-button drag keeps delivering moves while the pointer is outside the
     * window: the backend captures the pointer until the button is released. */
    void (*on_mouse)(void *user, int x, int y, int button, int down, int mods);
    /* periodic tick (~15 ms) used to drain script timers / rAF callbacks */
    void (*on_tick)(void *user);
    /* resolve the system cursor for the client area: return one of the
     * HPE_PLAT_CURSOR_* ids above. Called on WM_SETCURSOR while the pointer is
     * over the client area; may be NULL for the backend's default arrow. */
    int (*query_cursor)(void *user);
} HpeWindowCallbacks;
```

#### `HpeCoro`

```c
typedef struct HpeCoro HpeCoro;
```

一段可挂起/恢复的执行栈

#### `HpePlatFont`

```c
typedef struct HpePlatFont HpePlatFont;
```

#### `HpeTextMask`

```c
typedef struct HpeTextMask {
    unsigned char *coverage;
    int width;
    int height;
    int baseline;
} HpeTextMask;
```

#### `HpePlatEmojiFont`

```c
typedef struct HpePlatEmojiFont HpePlatEmojiFont;
```

#### `HpeGlyphOutline`

```c
typedef struct HpeGlyphOutline {
    unsigned char *coverage;
    int w, h;
    int left;      /* box left offset from the pen x / 盒左缘相对笔位 x 的偏移 */
    int baseline;  /* row index of the baseline inside the box / 盒内基线所在行 */
    float advance; /* cell increment x / 水平步进宽度 */
} HpeGlyphOutline;
```

## src/render（渲染与资源）

### hpe_emoji.h

4 个函数，1 个类型。

**函数**

#### `hpe_emoji_seq_len`

```c
int hpe_emoji_seq_len(const char *utf8, int max_len);
```

判断文本开头是否为彩色 emoji 序列

**详细说明**：序列 = emoji 基字符 + 可选的 VS16（`U+FE0F`）/ 肤色修饰符（`U+1F3FB`..`U+1F3FF`）/ 区域指示符对（旗帜）。ZWJ 链不并入序列（无 GSUB 连字支持，各分量独立渲染）。 A sequence = one emoji base plus an optional VS16 (`U+FE0F`) / skin-tone modifiers (`U+1F3FB`..`U+1F3FF`) / a regional-indicator pair (flags). ZWJ chains are not merged (no GSUB ligature support; parts render separately).

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `utf8` | in | UTF-8 文本 |
| `max_len` | in | 参与扫描的最大字节数 |

**返回**：序列的字节长度；不是彩色序列返回 0

**注意**：仅默认彩色呈现或带 VS16 的字符才算；纯文本呈现符号交给单色掩码路径

#### `hpe_emoji_sprite`

```c
int hpe_emoji_sprite(float pixel_size, const char *seq, int len, HpeColor fg, HpeGlyphSprite *out);
```

取（并缓存）一个彩色 emoji 序列的精灵

**详细说明**：读取系统 emoji 字体（`seguiemj.ttf`）的 cmap/COLRv0/CPAL 表，逐层取字形 覆盖率并用调色板色合成 RGBA 精灵；结果按「序列内容 + 字号 + 前景色」缓存。 The cmap/COLRv0/CPAL tables of the system emoji font (`seguiemj.ttf`) are parsed, each layer's glyph coverage is fetched and composited with its palette colour into an RGBA sprite; results are cached per sequence, pixel size and foreground colour.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pixel_size` | in | 字号（像素） |
| `seq` | in | UTF-8 序列（`hpe_emoji_seq_len` 的输入） |
| `len` | in | 序列字节长度 |
| `fg` | in | 前景色（调色板索引 `0xFFFF` 的层使用） |
| `out` | out | 接收精灵 |

**返回**：成功返回 1；无 COLR 数据、字体缺失或序列非法返回 0

**注意**：返回的像素由缓存持有，勿释放；调用 `hpe_emoji_shutdown` 后全部失效

**参见**：`hpe_emoji_seq_len`、`hpe_emoji_shutdown`

#### `hpe_emoji_blit`

```c
void hpe_emoji_blit(HpePixmap *pm, const HpeGlyphSprite *sp, int x, int y);
```

按直通 alpha 把精灵混合到像素缓冲

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `sp` | in | 精灵 |
| `x` | in | 精灵盒左缘的设备 x |
| `y` | in | 精灵盒顶缘的设备 y |

**注意**：裁剪范围外的像素跳过；结果像素始终不透明（与文本掩码绘制一致）

**参见**：`hpe_emoji_sprite`

#### `hpe_emoji_shutdown`

```c
void hpe_emoji_shutdown(void);
```

释放 emoji 模块的全部缓存

**详细说明**：丢弃精灵缓存并关闭所有已打开的 emoji 字体对象。 The sprite cache is dropped and every opened emoji font object is closed.

**注意**：应在引擎退出时调用一次

**参见**：`hpe_emoji_sprite`

**类型**

#### `HpeGlyphSprite`

```c
typedef struct HpeGlyphSprite {
    int w, h;
    int left;          /* box left offset from the pen x / 盒左缘相对笔位 x 的偏移 */
    int baseline;      /* rows above the baseline / 基线之上的行数 */
    uint32_t *pixels;  /* straight-alpha 0xAARRGGBB pixels / 直通 alpha 像素
                        * (cache-owned; freed by sprite_flush, non-const so the
                        * owner can hpe_free it / 缓存持有，由 sprite_flush 释放，
                        * 非 const 以便持有方直接释放) */
} HpeGlyphSprite;
```

### hpe_font.h

12 个函数，1 个类型。

**函数**

#### `hpe_font_get`

```c
HpeFont *hpe_font_get(float pixel_size, int weight, HpeFontFamily family);
```

取（并缓存）指定字号的字体对象

**详细说明**：字号四舍五入后夹到 [6, 200]；字重非 700 一律按 400；字体族非 serif/mono 一律按 sans。相同三元组命中同一缓存对象。 The pixel size is rounded and clamped to [6, 200]; any weight other than 700 becomes 400; any family other than serif or mono becomes sans. Identical triples share one cached object.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pixel_size` | in | 字号（像素） |
| `weight` | in | 字重，仅 700 与 400 两种取值 |
| `family` | in | 字体族 |

**返回**：缓存的字体对象；平台创建失败时返回 `NULL`

**注意**：不要直接释放返回的对象，它由引擎的字体缓存持有

**参见**：`hpe_font_shutdown`

#### `hpe_font_size`

```c
float hpe_font_size(HpeFont *font);
```

取字体已归整的字号

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象，可为 NULL |

**返回**：字号（像素）；`font` 为 `NULL` 返回 0

#### `hpe_font_weight`

```c
int hpe_font_weight(HpeFont *font);
```

取字体已归整的字重

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象，可为 NULL |

**返回**：字重；`font` 为 `NULL` 返回 400

#### `hpe_font_ascent`

```c
float hpe_font_ascent(HpeFont *font);
```

取字体的上伸部高度

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象，可为 NULL |

**返回**：上伸部（像素）；`font` 为 `NULL` 返回 0

#### `hpe_font_descent`

```c
float hpe_font_descent(HpeFont *font);
```

取字体的下伸部高度

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象，可为 NULL |

**返回**：下伸部（像素）；`font` 为 `NULL` 返回 0

#### `hpe_font_line_height`

```c
float hpe_font_line_height(HpeFont *font);
```

取字体的默认行高

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象，可为 NULL |

**返回**：行高（像素）；`font` 为 `NULL` 返回 0

#### `hpe_font_advance`

```c
float hpe_font_advance(HpeFont *font, uint32_t codepoint);
```

取单个码点的步进宽度（带缓存）

**详细说明**：首次查询某码点时向平台取宽度并写入缓存，之后直接命中缓存。 The width is fetched from the platform on the first query for a code point and stored; later queries hit the cache.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象 |
| `codepoint` | in | Unicode 码点 |

**返回**：步进宽度（像素）；`font` 为 `NULL` 返回 0

**注意**：缓存无法分配时退化为每次向平台查询

**参见**：`hpe_font_measure`

#### `hpe_font_measure`

```c
float hpe_font_measure(HpeFont *font, const char *text, int len);
```

测量一段 UTF-8 文本的宽度

**详细说明**：逐码点累加步进宽度，换行 `\n` 与回车 `\r` 不占宽度；非法码点处停止累加。 Advance widths are summed code point by code point; `\n` and `\r` contribute nothing and an ill-formed code point stops the scan.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象 |
| `text` | in | UTF-8 文本 |
| `len` | in | 文本字节数 |

**返回**：文本总宽度（像素）；参数无效或长度为非正数返回 0

**参见**：`hpe_font_advance`

#### `hpe_font_mask`

```c
const HpeTextMask *hpe_font_mask(HpeFont *font, const char *text, int len);
```

取某段 UTF-8 文本的覆盖率掩码（带缓存）

**详细说明**：掩码按「字体 + 文本内容」缓存，稳定帧不再触碰平台光栅化器；缓存负载超过 3/4 时整体清空重建。 Masks are cached per font and text content, so steady-state frames never touch the platform rasterizer; the cache is flushed wholesale once its load factor exceeds 3/4.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象 |
| `text` | in | UTF-8 文本 |
| `len` | in | 文本字节数 |

**返回**：缓存持有的掩码指针（借出，勿释放）；失败返回 `NULL`

**注意**：掩码与整数位置无关，绘制时才四舍五入 x/y，因此仅文本内容即可标识

**参见**：`hpe_font_shutdown`

#### `hpe_font_mask_phase`

```c
const HpeTextMask *hpe_font_mask_phase(HpeFont *font, const char *text, int len, float frac_x);
```

取带水平亚像素相位的覆盖率掩码（带缓存）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 字体对象 |
| `text` | in | UTF-8 文本 |
| `len` | in | 文本字节数 |
| `frac_x` | in | 笔位的小数部分（0..1），量化为 3 档并计入缓存键 |

**返回**：缓存持有的掩码指针（借出，勿释放）；失败返回 `NULL`

**参见**：`hpe_font_mask`

#### `hpe_font_resolve`

```c
HpeFont *hpe_font_resolve(HpeFont *font, uint32_t codepoint);
```

解析某码点真正使用的字体（含回退链）

**详细说明**：主字体覆盖该码点时返回主字体本身；否则按字体族的硬编码回退链 （Segoe UI Symbol → Segoe UI Emoji → SimSun）逐个验证覆盖并返回第一个 覆盖的后备字体。结果按码点缓存在主字体上。 When the primary font covers the code point the primary itself is returned; otherwise the family's hardcoded fallback chain (Segoe UI Symbol → Segoe UI Emoji → SimSun) is probed and the first covering fallback is returned. Results are cached per code point.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `font` | in,out | 主字体对象 |
| `codepoint` | in | Unicode 码点 |

**返回**：服务该码点的字体；`font` 为 `NULL` 返回 `NULL`

**注意**：彩色 emoji 序列的步进也经此解析到 emoji 字体

**参见**：`hpe_font_advance`

#### `hpe_font_shutdown`

```c
void hpe_font_shutdown(void);
```

销毁全部缓存字体并释放其资源

**详细说明**：逐个释放掩码缓存、字宽缓存、平台字体与字体对象，并把缓存计数归零。 Each font's mask cache, advance cache, platform font and the object itself are freed, and the cache count is reset to zero.

**注意**：应在引擎退出时调用一次，调用后字体缓存为空

**参见**：`hpe_font_get`

**类型**

#### `HpeFont`

```c
typedef struct HpeFont HpeFont;
```

### hpe_image.h

15 个函数，3 个类型。

**函数**

#### `hpe_image_format_from_path`

```c
HpeImageFormat hpe_image_format_from_path(const char *path);
```

从文件扩展名推断图像格式

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `path` | in | 文件路径或 URL |

**返回**：推断出的格式；无法判断时返回 `HPE_IMG_UNKNOWN`

**注意**：只看扩展名，不读文件内容

**参见**：`hpe_image_sniff_format`

#### `hpe_image_sniff_format`

```c
HpeImageFormat hpe_image_sniff_format(const uint8_t *data, size_t len);
```

从文件头字节嗅探图像格式

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `data` | in | 文件起始字节 |
| `len` | in | 可用字节数 |

**返回**：嗅探出的格式；无法判断时返回 `HPE_IMG_UNKNOWN`

**注意**：识别 PNG / JPEG / GIF / BMP 四类魔数

**参见**：`hpe_image_format_from_path`

#### `hpe_image_decode`

```c
int hpe_image_decode(const uint8_t *data, size_t len, HpeImageFormat hint, HpeImage *out);
```

解码一段内存中的图像数据

**详细说明**：先按魔数判格式再分派到对应解码器；`HPE_IMG_UNKNOWN` 时按扩展名再试一次。 The format is sniffed from the magic bytes and dispatched to the matching decoder; an unknown magic falls back to the hint supplied by the caller.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `data` | in | 图像数据 |
| `len` | in | 数据长度 |
| `hint` | in | 扩展名提示（可为 `HPE_IMG_UNKNOWN`） |
| `out` | out | 接收解码结果的图像 |

**返回**：成功返回 1，失败返回 0

**注意**：失败时不修改 `*out`，调用方无需清理

**参见**：`hpe_image_free`、`hpe_image_load_file`

#### `hpe_image_load_file`

```c
int hpe_image_load_file(const char *path, HpeImageFormat hint, HpeImage *out);
```

解码磁盘上的图像文件

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `path` | in | 文件路径 |
| `hint` | in | 扩展名提示（可为 `HPE_IMG_UNKNOWN`） |
| `out` | out | 接收解码结果的图像 |

**返回**：成功返回 1，失败（含文件不存在）返回 0

**参见**：`hpe_image_decode`、`hpe_image_free`

#### `hpe_image_free`

```c
void hpe_image_free(HpeImage *img);
```

释放解码图像占用的内存

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `img` | in,out | 目标图像；为 `NULL` 时安全无操作 |

**注意**：只释放本模块自己分配的像素数组（`owned` 为 1 时）

**参见**：`hpe_image_decode`

#### `hpe_image_scale`

```c
int hpe_image_scale(const HpeImage *src, int w, int h, HpeImage *out);
```

把解码图像缩放为新的像素尺寸

**详细说明**：双线性重采样，按 `alpha` 加权以避免透明边缘发黑。 Bilinear resampling weighted by alpha, so transparent edges do not darken.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `src` | in | 源图像 |
| `w` | in | 目标宽度（至少 1） |
| `h` | in | 目标高度（至少 1） |
| `out` | out | 接收缩放结果 |

**返回**：成功返回 1，失败返回 0

**注意**：尺寸相同也走一次拷贝，保证 `out` 始终独立可释放

**参见**：`hpe_image_free`

#### `hpe_image_cache_new`

```c
HpeImageCache *hpe_image_cache_new(void);
```

新建一个解码图像缓存

**返回**：新缓存；分配失败返回 `NULL`

**参见**：`hpe_image_cache_free`、`hpe_image_cache_get`

#### `hpe_image_cache_free`

```c
void hpe_image_cache_free(HpeImageCache *cache);
```

释放解码图像缓存

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `cache` | in,out | 目标缓存；为 `NULL` 时安全无操作 |

**注意**：逐条释放其持有的解码图像

**参见**：`hpe_image_cache_new`

#### `hpe_image_cache_get`

```c
const HpeImage *hpe_image_cache_get(HpeImageCache *cache, const char *path, HpeImageFormat hint, int want_w, int want_h);
```

按路径取解码图像，缺失时解码并缓存

**详细说明**：命中缓存直接返回同一指针；未命中时读文件、解码、按需缩放并插入链表头部。 A cache hit returns the very same pointer; a miss reads the file, decodes it, resamples when a target size was given, and inserts the entry at the head.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `cache` | in,out | 目标缓存 |
| `path` | in | 资源路径 |
| `hint` | in | 格式提示（可为 `HPE_IMG_UNKNOWN`） |
| `want_w` | in | 目标像素宽；小于 0 表示保持固有尺寸 |
| `want_h` | in | 目标像素高；小于 0 表示保持固有尺寸 |

**返回**：解码图像（缓存持有，调用方不得释放）；失败返回 `NULL`

**注意**：目标尺寸参与缓存键，同一路径的不同尺寸各存一份

**参见**：`hpe_image_cache_new`、`hpe_image_decode`

#### `hpe_image_cache_count`

```c
int hpe_image_cache_count(const HpeImageCache *cache);
```

报告缓存中已解码图像的条目数

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `cache` | in | 目标缓存 |

**返回**：条目数；`cache` 为 `NULL` 时返回 0

**参见**：`hpe_image_cache_get`

#### `hpe_image_set_base_dir`

```c
void hpe_image_set_base_dir(const char *dir);
```

设置解析相对图像的基准目录

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `dir` | in | 文档（或 `<base href>`）所在目录；`NULL` 视为当前目录 |

**注意**：只记录路径字符串，不做任何解析

**参见**：`hpe_image_resolve_url`

#### `hpe_image_base_dir`

```c
const char *hpe_image_base_dir(void);
```

取当前的相对引用基准目录

**返回**：基准目录，从未设置时返回 `""`

#### `hpe_image_resolve_url`

```c
int hpe_image_resolve_url(const char *src, char *out, size_t out_size);
```

把 `src` / `url()` 引用解析为可打开的绝对路径

**详细说明**：绝对路径原样返回；`data:` 与非文件方案（`http:`、`https:`、`about:`）返回空串， 因为本引擎只加载本地资源。其余相对引用按 `hpe_image_set_base_dir` 装的基准目录拼接。 An absolute path is passed through, `data:` and non-file schemes (`http:`, `https:`, `about:`) yield an empty string because the engine only loads local resources, and anything else is joined onto the base directory installed by `hpe_image_set_base_dir`.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `src` | in | 原始引用文本，可含首尾空白与引号 |
| `out` | out | 接收绝对路径的缓冲区 |
| `out_size` | in | 输出缓冲区容量（字节） |

**返回**：得到可加载路径返回 1，否则 0

**参见**：`hpe_image_set_base_dir`

#### `hpe_image_shared_cache`

```c
HpeImageCache *hpe_image_shared_cache(void);
```

取进程级共享的解码图像缓存

**详细说明**：布局与绘制共用同一份缓存，因此一张图在一帧内只解码一次；缓存随引擎存续， 由 `hpe_image_shutdown` 统一释放。 Layout and paint share the cache, so an image is decoded at most once per frame; it lives as long as the engine and is released by `hpe_image_shutdown`.

**返回**：共享缓存（惰性创建）；分配失败返回 `NULL`

**参见**：`hpe_image_shutdown`、`hpe_image_cache_get`

#### `hpe_image_shutdown`

```c
void hpe_image_shutdown(void);
```

释放共享缓存与基准目录

**注意**：引擎销毁时调用；之后再取共享缓存会重新惰性创建

**参见**：`hpe_image_shared_cache`

**类型**

#### `HpeImageFormat`

```c
typedef enum HpeImageFormat {
    HPE_IMG_UNKNOWN = 0,
    HPE_IMG_PNG,
    HPE_IMG_JPEG,
    HPE_IMG_GIF,
    HPE_IMG_BMP
} HpeImageFormat;
```

#### `HpeImage`

```c
typedef struct HpeImage {
    int width;
    int height;
    uint32_t *pixels; /* row-major, width * height entries */
    int owned;
    HpeImageFormat format;
    int frame_count; /* GIF: number of frames found; 1 for the still formats */
} HpeImage;
```

#### `HpeImageCache`

```c
typedef struct HpeImageCache HpeImageCache;
```

### hpe_pixmap.h

10 个函数，1 个类型。

**函数**

#### `hpe_pixmap_init`

```c
void hpe_pixmap_init(HpePixmap *pm, int width, int height);
```

初始化像素缓冲并按需分配像素数组

**详细说明**：宽高小于等于 0 时被夹为 0，此时不会分配像素数组而把 `pixels` 置为 `NULL`。 Non-positive dimensions are clamped to 0, in which case no pixel array is allocated and `pixels` is left `NULL`.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `width` | in | 缓冲宽度（像素），非正数夹为 0 |
| `height` | in | 缓冲高度（像素），非正数夹为 0 |

**注意**：会以默认全幅范围重置裁剪矩形

**参见**：`hpe_pixmap_free`、`hpe_pixmap_resize`

#### `hpe_pixmap_resize`

```c
void hpe_pixmap_resize(HpePixmap *pm, int width, int height);
```

把像素缓冲调整为新的宽高

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `width` | in | 新宽度（像素） |
| `height` | in | 新高度（像素） |

**注意**：尺寸未变时直接返回；否则释放旧像素再走 `hpe_pixmap_init`，旧内容不保留

**参见**：`hpe_pixmap_init`、`hpe_pixmap_free`

#### `hpe_pixmap_free`

```c
void hpe_pixmap_free(HpePixmap *pm);
```

释放像素缓冲占用的内存

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |

**注意**：释放后宽、高、步长与 `pixels` 全部清零，可安全重复调用

**参见**：`hpe_pixmap_init`

#### `hpe_pixmap_clear`

```c
void hpe_pixmap_clear(HpePixmap *pm, HpeColor color);
```

用纯色填充整个像素缓冲

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `color` | in | 填充颜色，按 `0xAARRGGBB` 写入 |

**注意**：忽略裁剪矩形，直接覆盖全部像素；无像素数组时不做任何事

#### `hpe_pixmap_set_clip`

```c
void hpe_pixmap_set_clip(HpePixmap *pm, HpeRect rect);
```

设置裁剪矩形，替换当前裁剪范围

**详细说明**：矩形先按左/上边界 `floor`、右/下边界 `ceil` 取整，再夹到缓冲范围内；空矩形会退化为零宽或零高。 The rectangle is rounded with `floor` on the top/left edges and `ceil` on the bottom/right edges, then clamped to the buffer; a degenerate rectangle collapses to zero width or height.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `rect` | in | 新的裁剪矩形（用户空间坐标） |

**注意**：与 `hpe_pixmap_intersect_clip` 不同，这里会丢弃原有的裁剪范围

**参见**：`hpe_pixmap_intersect_clip`、`hpe_pixmap_reset_clip`

#### `hpe_pixmap_intersect_clip`

```c
void hpe_pixmap_intersect_clip(HpePixmap *pm, HpeRect rect);
```

把当前裁剪矩形收窄为与给定矩形的交集

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `rect` | in | 用于收窄裁剪的矩形（用户空间坐标） |

**注意**：嵌套裁剪（如破损区域内的 `<svg>` 视口）必须用它，`hpe_pixmap_set_clip` 会替换掉外层裁剪

**参见**：`hpe_pixmap_set_clip`、`hpe_pixmap_reset_clip`

#### `hpe_pixmap_reset_clip`

```c
void hpe_pixmap_reset_clip(HpePixmap *pm);
```

把裁剪矩形重置为整个缓冲

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |

**参见**：`hpe_pixmap_set_clip`、`hpe_pixmap_intersect_clip`

#### `hpe_pixmap_shift_v`

```c
int hpe_pixmap_shift_v(HpePixmap *pm, int dy);
```

把全部像素行纵向平移若干像素

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `dy` | in | 纵向位移，正数表示内容上移 |

**返回**：实际应用的位移，已夹到 ±高度

**注意**：空出的行保留旧像素，需由调用方重绘

**参见**：`hpe_pixmap_shift_v_rect`

#### `hpe_pixmap_shift_v_rect`

```c
int hpe_pixmap_shift_v_rect(HpePixmap *pm, int dy, int y0, int y1);
```

只平移行区间 [y0, y1) 的像素

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `dy` | in | 纵向位移，正数表示内容上移 |
| `y0` | in | 起始行（含），小于 0 时夹为 0 |
| `y1` | in | 结束行（不含），超过高度时夹到高度 |

**返回**：实际应用的位移，已夹到 ±区间行数

**注意**：让地址栏等浏览器外框保持固定、只让下方页面内容滚动

**参见**：`hpe_pixmap_shift_v`

#### `hpe_argb`

```c
static inline uint32_t hpe_argb(HpeColor c)
{
    return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}
```

把颜色打包为 `0xAARRGGBB` 像素值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `c` | in | 待打包的颜色 |

**返回**：打包后的 32 位像素值

**注意**：与 GDI 的 `RGBQUAD` 通道布局一致，可直接写入 HpePixmap 的 pixels

**类型**

#### `HpePixmap`

```c
typedef struct HpePixmap {
    int width;
    int height;
    int stride; /* bytes per row */
    uint32_t *pixels;
    /* damage-rect support: all raster ops clip to [clip_x0,clip_x1)x[clip_y0,clip_y1) */
    int clip_x0, clip_y0, clip_x1, clip_y1;
    /* offscreen composition: blend writes accumulated alpha instead of 255 */
    int alpha_record;
} HpePixmap;
```

### hpe_raster.h

9 个函数，1 个类型。

**函数**

#### `hpe_fill_rect`

```c
void hpe_fill_rect(HpePixmap *pm, HpeRect rect, HpeColor color);
```

用纯色填充一个矩形

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `rect` | in | 矩形区域（用户空间坐标） |
| `color` | in | 颜色 |

**注意**：坐标按左/上 `floor`、右/下 `ceil` 取整后交给 `hpe_fill_rect_i`

**参见**：`hpe_fill_rect_i`

#### `hpe_fill_rect_i`

```c
void hpe_fill_rect_i(HpePixmap *pm, int x, int y, int w, int h, HpeColor color);
```

按整数坐标用纯色填充矩形

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `x` | in | 矩形左边界 |
| `y` | in | 矩形上边界 |
| `w` | in | 矩形宽度 |
| `h` | in | 矩形高度 |
| `color` | in | 颜色 |

**注意**：矩形会夹到当前裁剪范围内，全不透明时按行整体写入，否则逐像素混合

**参见**：`hpe_fill_rect`

#### `hpe_fill_round_rect`

```c
void hpe_fill_round_rect(HpePixmap *pm, HpeRect rect, float radius, HpeColor color);
```

用纯色填充抗锯齿圆角矩形

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `rect` | in | 目标矩形（用户空间坐标） |
| `radius` | in | 圆角半径 |
| `color` | in | 颜色 |

**注意**：半径不超过 0.5 时退化为直角矩形，直接调用 `hpe_fill_rect`

**参见**：`hpe_stroke_round_rect`、`hpe_fill_rect`

#### `hpe_stroke_round_rect`

```c
void hpe_stroke_round_rect(HpePixmap *pm, HpeRect rect, float radius, float width, HpeColor color);
```

用纯色画圆角矩形的内描边环

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `rect` | in | 目标矩形（用户空间坐标） |
| `radius` | in | 圆角半径 |
| `width` | in | 描边宽度 |
| `color` | in | 颜色 |

**注意**：宽度非正时不做任何事；半径不超过 0.5 时改为画四条直角边带

**参见**：`hpe_fill_round_rect`

#### `hpe_draw_text_mask`

```c
void hpe_draw_text_mask(HpePixmap *pm, int x, int y, const HpeTextMask *mask, HpeColor color);
```

把 8 位覆盖率掩码合成到像素缓冲

**详细说明**：掩码逐行逐列按覆盖率与颜色 alpha 相乘后混合，超出裁剪范围的像素跳过。 Each mask pixel is blended after multiplying its coverage by the colour alpha; pixels outside the clip are skipped.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `x` | in | 笔位置 X（设备像素） |
| `y` | in | 基线 Y（设备像素） |
| `mask` | in | 字形覆盖率掩码 |
| `color` | in | 颜色 |

**注意**：`(x, y)` 是基线上的笔位置，掩码自身的 `baseline` 用于定位顶部

#### `hpe_sample_stops`

```c
void hpe_sample_stops(const HpeRasterStop *stops, int stop_count, float t, HpeColor *out);
```

按参数 t 在色标列表中采样颜色

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `stops` | in | 色标数组 |
| `stop_count` | in | 色标个数 |
| `t` | in | 采样位置 0..1 |
| `out` | out | 采样得到的颜色 |

**注意**：CSS 背景渐变与 SVG 渐变绘制共用的入口

**参见**：`hpe_fill_linear_gradient`

#### `hpe_fill_linear_gradient`

```c
void hpe_fill_linear_gradient(HpePixmap *pm, HpeRect rect, float angle_deg, const HpeRasterStop *stops, int stop_count);
```

在矩形内填充线性渐变

**详细说明**：渐变轴由 `angle_deg` 决定，方向向量为 `(sin a, -cos a)`，渐变线长度按 ` w*sin a + h*cos a ` 覆盖到四角。 The gradient axis comes from `angle_deg` with direction vector `(sin a, -cos a)`; the gradient line is sized as ` w*sin a + h*cos a ` so the corners are covered.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `rect` | in | 目标矩形（用户空间坐标） |
| `angle_deg` | in | 渐变角度（度），0 向上、90 向右 |
| `stops` | in | 色标数组 |
| `stop_count` | in | 色标个数 |

**注意**：色标按位置扫描采样，无需预先排序

**参见**：`hpe_fill_round_rect_gradient`、`hpe_sample_stops`

#### `hpe_fill_round_rect_gradient`

```c
void hpe_fill_round_rect_gradient(HpePixmap *pm, HpeRect rect, float radius, float angle_deg, const HpeRasterStop *stops, int stop_count);
```

在圆角矩形内填充线性渐变

**详细说明**：先把渐变画进与圆角矩形同尺寸的临时缓冲，再按圆角矩形的距离场覆盖率合成回目标缓冲。 The gradient is first painted into a scratch buffer the size of the rounded rectangle, then composited back through the rounded-rectangle distance-field coverage.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `rect` | in | 目标矩形（用户空间坐标） |
| `radius` | in | 圆角半径 |
| `angle_deg` | in | 渐变角度（度），0 向上、90 向右 |
| `stops` | in | 色标数组 |
| `stop_count` | in | 色标个数 |

**注意**：半径不超过 0.5 时退化为普通线性渐变，直接调用 `hpe_fill_linear_gradient`

**参见**：`hpe_fill_linear_gradient`、`hpe_fill_round_rect`

#### `hpe_fill_shadow`

```c
void hpe_fill_shadow(HpePixmap *pm, HpeRect rect, float radius, float blur, HpeColor color);
```

在矩形周围绘制柔和投影

**详细说明**：用若干层同心圆角矩形（alpha 由外向内递增）近似高斯模糊，最内层保持完全不透明。 A gaussian blur is approximated by concentric rounded rectangles whose alpha increases inward, with the innermost layer fully opaque.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `pm` | in,out | 目标像素缓冲 |
| `rect` | in | 投影矩形（用户空间坐标） |
| `radius` | in | 圆角半径 |
| `blur` | in | 模糊半径 |
| `color` | in | 颜色 |

**注意**：模糊半径小于 1 时退化为一次实心圆角矩形填充；层数夹在 1~24

**参见**：`hpe_fill_round_rect`

**类型**

#### `HpeRasterStop`

```c
typedef struct HpeRasterStop {
    float pos; /* 0..1, clamped */
    HpeColor color;
} HpeRasterStop;
```

## src（引擎入口）

### hope.h

44 个函数，2 个类型。

**函数**

#### `hpe_engine_create`

```c
HpeEngine *hpe_engine_create(int width, int height);
```

创建引擎实例并初始化像素缓冲

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `width` | in | 窗口宽度（像素），非正时回退为 800 |
| `height` | in | 窗口高度（像素），非正时回退为 600 |

**返回**：新引擎实例；调用方最终须用 `hpe_engine_destroy` 释放

**参见**：`hpe_engine_destroy`

#### `hpe_engine_destroy`

```c
void hpe_engine_destroy(HpeEngine *engine);
```

销毁引擎并释放全部资源

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，NULL 时直接返回 |

**注意**：释放盒树、DOM、样式表、像素缓冲与 JS 上下文，并关闭进程级字体子系统

**参见**：`hpe_engine_create`

#### `hpe_engine_load_file`

```c
int hpe_engine_load_file(HpeEngine *engine, const char *path);
```

加载并排版一个 HTML 文件

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `path` | in | 要加载的文件路径 |

**返回**：加载成功返回 1，否则 0

- `1`：已读取、解析并排版；启用地址栏时地址栏写入绝对路径
- `0`：`engine`/`path` 为 NULL，或文件无法打开（错误写入状态行）

**注意**：相对资源（样式表、脚本、`<base href>`）以该文件所在目录为基准

**参见**：`hpe_engine_load_blank`、`hpe_engine_reload`

#### `hpe_engine_load_blank`

```c
int hpe_engine_load_blank(HpeEngine *engine);
```

加载内置空白页

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，NULL 时返回 0 |

**返回**：加载成功返回 1，否则 0

**注意**：无文件参数启动时使用；相对资源以 `.` 为基准；启用地址栏时清空地址并失焦

**参见**：`hpe_engine_load_file`

#### `hpe_engine_reload`

```c
int hpe_engine_reload(HpeEngine *engine);
```

重新加载当前文档

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，NULL 时返回 0 |

**返回**：加载成功返回 1，否则 0

**注意**：尚无文档路径时改为加载内置空白页

**参见**：`hpe_engine_load_file`、`hpe_engine_load_blank`

#### `hpe_engine_resize`

```c
void hpe_engine_resize(HpeEngine *engine, int width, int height);
```

调整窗口与像素缓冲尺寸并重排

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `width` | in | 新的窗口宽度（像素） |
| `height` | in | 新的窗口高度（像素） |

**注意**：宽高非正或与当前完全一致时不做任何事；否则重设缓冲、重排并刷新状态行

**参见**：`hpe_engine_load_file`

#### `hpe_engine_scroll_by`

```c
void hpe_engine_scroll_by(HpeEngine *engine, float lines);
```

按行滚动页面

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，NULL 时直接返回 |
| `lines` | in | 滚动的行数，正为下滚、负为上滚 |

**注意**：一行等于 `HPE_SCROLL_LINE_PX` 像素；滚动偏移夹到 `[0, content_height - view_height]` 后取整到整像素，使损伤矩形路径与全量重绘逐字节一致；仅在未脏时把脏标记置为 `HPE_DIRTY_SCROLL`

**参见**：`hpe_engine_scroll_offset`、`hpe_engine_render`

#### `hpe_engine_wheel_scroll`

```c
int hpe_engine_wheel_scroll(HpeEngine *engine, int x, int y, float lines);
```

把滚轮事件投递给指针处的滚动容器

**详细说明**：指针落在可滚动元素（`overflow:scroll`/`auto` 且内容溢出）上时滚动该元素， 未被消费的行数继续冒泡给整页，模拟浏览器的滚动链。 When the pointer is over a scrollable element (`overflow:scroll`/`auto` with overflowing content) that element scrolls and the remaining lines bubble to the page, mirroring browser scroll chaining.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `x` | in | 视口 x（窗口坐标，含浏览器外框） |
| `y` | in | 视口 y（窗口坐标，含浏览器外框） |
| `lines` | in | 滚动的行数，正为下滚 |

**返回**：有元素消费该事件返回 1，否则返回 0

**参见**：`hpe_engine_scroll_by`

#### `hpe_engine_wheel_event`

```c
int hpe_engine_wheel_event(HpeEngine *engine, int x, int y, int delta_x, float lines, int mods);
```

向页面派发滚轮事件

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，无 JS 上下文时返回 0 |
| `x` | in | X 坐标（窗口坐标） |
| `y` | in | Y 坐标（窗口坐标） |
| `delta_x` | in | 水平滚动量（像素） |
| `lines` | in | 垂直滚动行数，正为下滚（按 `HPE_SCROLL_LINE_PX` 换算成 `deltaY`） |
| `mods` | in | `HPE_MOD_*` 修饰键掩码 |

**返回**：有监听器触发返回 1，否则 0

**注意**：打到指针下的元素上并向上冒泡；事件对象带 `deltaX`/`deltaY`/`deltaMode` Targets the element under the pointer and bubbles; the event object carries `deltaX`/`deltaY`/`deltaMode`

**参见**：`hpe_engine_wheel_scroll`

#### `hpe_engine_key_event`

```c
int hpe_engine_key_event(HpeEngine *engine, int vk, int printable, int mods, int phase, int repeat);
```

向页面派发键盘事件

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，无 JS 上下文时返回 0 |
| `vk` | in | 虚拟键码（非可打印键） |
| `printable` | in | 可打印字符（0 表示不是可打印键） |
| `mods` | in | `HPE_MOD_*` 修饰键掩码 |
| `phase` | in | 0 = `keydown`，1 = `keyup`，2 = `keypress` |
| `repeat` | in | 按住重复为 1 |

**返回**：有监听器触发返回 1，否则 0

**注意**：目标是当前获得 `:focus` 的元素，无焦点时退回 `document.body`；事件对象带 `key`/`code`/`keyCode`/`repeat` 与四个修饰键 The target is the element owning `:focus`, falling back to `document.body`; the event object carries `key`/`code`/`keyCode`/`repeat` and the four modifiers

#### `hpe_engine_scroll_event`

```c
int hpe_engine_scroll_event(HpeEngine *engine, HpeDomNode *target);
```

向页面派发一次 `scroll` 事件

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，无 JS 上下文时返回 0 |
| `target` | in,out | 滚动发生的元素；传 `NULL` 表示文档 |

**返回**：有监听器触发返回 1，否则 0

**注意**：`scroll` 不冒泡，因此只在目标元素上触发

#### `hpe_engine_mouse_event`

```c
int hpe_engine_mouse_event(HpeEngine *engine, int x, int y, int button, int down);
```

派发一次鼠标按下 / 抬起

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `x` | in | X 坐标（窗口坐标） |
| `y` | in | Y 坐标（窗口坐标） |
| `button` | in | 鼠标按键（0 = 左键） |
| `down` | in | 按下为 1、抬起为 0 |

**返回**：页面发生变更返回 1（引擎已重排，窗口应重绘），否则 0

**注意**：点击落在地址栏条带内或页面点击使地址栏失焦时同样返回 1；等价于修饰键掩码为 0 的 `hpe_engine_mouse_event_mods`

**参见**：`hpe_engine_mouse_event_mods`、`hpe_engine_mouse_move`

#### `hpe_engine_mouse_move`

```c
int hpe_engine_mouse_move(HpeEngine *engine, int x, int y);
```

处理指针移动：更新 `:hover` 状态并触发 `mousemove` 监听器

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，NULL 或无盒树时返回 0 |
| `x` | in | X 坐标（窗口坐标） |
| `y` | in | Y 坐标（窗口坐标） |

**返回**：需要重绘返回 1（悬停变化或有监听器改动了页面），否则 0

**注意**：悬停变化总是强制重排（`:hover` 是选择器，须重新级联）；否则仅在监听器产生变更时惰性重排；地址栏拖选进行中时优先交给地址栏

**参见**：`hpe_engine_mouse_event`、`hpe_engine_tick`

#### `hpe_engine_tick`

```c
int hpe_engine_tick(HpeEngine *engine);
```

泵到期定时器与 requestAnimationFrame 队列

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，NULL 时返回 0 |

**返回**：有回调运行或地址栏插入符闪烁相位翻转返回 1（页面应重绘），否则 0

**注意**：每轮只处理已到期的回调，期间新增的留待下一轮；插入符时钟独立于 JS 上下文，无脚本时同样闪烁（仅整帧/地址栏脏路径会重绘）

**参见**：`hpe_engine_mouse_move`

#### `hpe_engine_render`

```c
void hpe_engine_render(HpeEngine *engine);
```

按脏标记把引擎绘制到像素缓冲

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，NULL 时直接返回 |

**注意**：无变更且已绘制过时整帧跳过，使无实际效果的 DOM 变更免费；仅脏地址栏时只重画地址栏，仅滚动时走局部位移重绘，否则整帧重绘；用后取 `hpe_engine_pixmap` 获取结果

**参见**：`hpe_engine_pixmap`、`hpe_engine_scroll_by`

#### `hpe_engine_ime_compose`

```c
void hpe_engine_ime_compose(HpeEngine *engine, const char *utf8);
```

更新输入法组合串的内联显示

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `utf8` | in | 组合中的字符串（"" 表示清除） |

**注意**：组合串画在插入符处并带下划线，不改动地址文本；空串清除显示并恢复插入符

#### `hpe_engine_ime_commit`

```c
int hpe_engine_ime_commit(HpeEngine *engine, const char *utf8);
```

提交输入法组合串（插入插入符处）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `utf8` | in | 最终字符串 |

**返回**：插入成功返回 1，否则 0

**注意**：等价于在插入符处插入文本并清除组合显示；未聚焦或空串时忽略

#### `hpe_engine_enable_chrome`

```c
void hpe_engine_enable_chrome(HpeEngine *engine);
```

为引擎启用自绘地址栏

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，NULL 或已启用时直接返回 |

**注意**：启用后视口变窄，页面在地址栏下方排版并接收相对自身的鼠标坐标；已排版则重排以重新夹紧滚动，否则标记整帧重绘

**参见**：`hpe_engine_chrome_height`

#### `hpe_engine_tab_count`

```c
int hpe_engine_tab_count(HpeEngine *engine);
```

取当前打开的标签数

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in | 引擎实例 |

**返回**：标签数，`engine` 为 NULL 时返回 0

#### `hpe_engine_tab_active`

```c
int hpe_engine_tab_active(HpeEngine *engine);
```

取当前活动标签下标

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in | 引擎实例 |

**返回**：活动标签下标，`engine` 为 NULL 时返回 -1

#### `hpe_engine_tab_switch`

```c
int hpe_engine_tab_switch(HpeEngine *engine, int index);
```

切换到指定下标的标签

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `index` | in | 目标标签下标 |

**返回**：成功返回 1，下标非法或已是当前标签返回 0

**注意**：切换会保存当前标签的路径/历史/滚动，重载目标标签的文档并恢复其滚动

#### `hpe_engine_chrome_height`

```c
int hpe_engine_chrome_height(HpeEngine *engine);
```

取地址栏高度

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：地址栏高度（像素），未启用时为 0

**注意**：窗口坐标减去该值即得页面视口坐标

**参见**：`hpe_engine_enable_chrome`

#### `hpe_engine_address_text`

```c
const char *hpe_engine_address_text(HpeEngine *engine);
```

取地址栏当前文本

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：地址文本（UTF-8），`engine` 为 NULL 时返回空串

#### `hpe_engine_address_focused`

```c
int hpe_engine_address_focused(HpeEngine *engine);
```

查询地址栏是否获得焦点

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：聚焦返回 1，否则 0

**参见**：`hpe_engine_key`

#### `hpe_engine_address_selection`

```c
int hpe_engine_address_selection(HpeEngine *engine, int *start, int *end);
```

取地址栏选区的字节范围

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `start` | out | 接收选区起始字节偏移，可为 NULL |
| `end` | out | 接收选区结束字节偏移，可为 NULL |

**返回**：选取非空返回 1，否则 0

**注意**：空选区（只有插入符）返回 0 且不写出任何输出，与 omnibox 一致

#### `hpe_engine_address_caret`

```c
int hpe_engine_address_caret(HpeEngine *engine);
```

取插入符在地址文本中的字节偏移

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：插入符字节偏移，`engine` 为 NULL 时返回 0

**参见**：`hpe_engine_address_selection`

#### `hpe_engine_page_cursor`

```c
int hpe_engine_page_cursor(HpeEngine *engine);
```

取客户端区当前应显示的光标

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in | 引擎实例 |

**返回**：`HpeCursor` 枚举值，`engine` 为 NULL 时返回 `HPE_CURSOR_DEFAULT`

**注意**：由指针移动事件更新：地址栏条带内输入框为 I-beam、条带其余处为箭头；页面上取命中盒解析出的 CSS `cursor`（含 UA 默认：链接为 pointer、按钮为 default），文本盒回退为 I-beam；平台层可在窗口回调 `query_cursor` 中读取它映射为系统光标

**参见**：`hpe_engine_mouse_move`

#### `hpe_engine_caret_visible`

```c
int hpe_engine_caret_visible(HpeEngine *engine);
```

查询地址栏插入符此刻是否可见

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in | 引擎实例 |

**返回**：插入符可见返回 1，否则 0

**注意**：地址栏聚焦、选区折叠且处在 0.5s 可见相位时返回 1；`engine` 为 NULL 时返回 0

**参见**：`hpe_engine_tick`、`hpe_engine_address_focused`

#### `hpe_engine_nav_count`

```c
int hpe_engine_nav_count(HpeEngine *engine);
```

取导航历史项数

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in | 引擎实例 |

**返回**：历史项数，`engine` 为 NULL 时返回 0

**注意**：仅在启用地址栏时记录

**参见**：`hpe_engine_nav_pos`

#### `hpe_engine_nav_pos`

```c
int hpe_engine_nav_pos(HpeEngine *engine);
```

取当前历史项下标

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in | 引擎实例 |

**返回**：当前下标（0 起），无历史或 `engine` 为 NULL 时返回 -1

**参见**：`hpe_engine_nav_count`、`hpe_engine_back`

#### `hpe_engine_mouse_event_mods`

```c
int hpe_engine_mouse_event_mods(HpeEngine *engine, int x, int y, int button, int down, int mods);
```

带修饰键地派发一次鼠标按下 / 抬起

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，NULL 或无盒树时返回 0 |
| `x` | in | X 坐标（窗口坐标） |
| `y` | in | Y 坐标（窗口坐标） |
| `button` | in | 鼠标按键（0 = 左键；当前实现未使用） |
| `down` | in | 按下为 1、抬起为 0 |
| `mods` | in | `HPE_MOD_*` 修饰键掩码 |

**返回**：触发监听器或使地址栏失焦返回 1，否则 0

**注意**：与 `hpe_engine_mouse_event` 相同，但额外携带修饰键掩码：Shift+点击扩展地址栏选区

**参见**：`hpe_engine_mouse_event`

#### `hpe_engine_key`

```c
int hpe_engine_key(HpeEngine *engine, int key);
```

无修饰键地向地址栏送入一个按键

**详细说明**：接受的键包括可打印码点，以及 `HPE_KEY_BACKSPACE`、`HPE_KEY_DELETE`、`HPE_KEY_LEFT`/`RIGHT`/`HOME`/`END`、`HPE_KEY_RETURN`（导航到所输入的路径）、`HPE_KEY_ESCAPE`（回退到该字段获得焦点时的地址，然后失焦），以及 `HPE_KEY_PASTE`/`COPY`/`CUT`/`SELECT_ALL` 快捷键。 Accepted keys are printable code points plus `HPE_KEY_BACKSPACE`, `HPE_KEY_DELETE`, `HPE_KEY_LEFT`/`RIGHT`/`HOME`/`END`, `HPE_KEY_RETURN` (navigates to the typed path), `HPE_KEY_ESCAPE` (restores the address the field got focus with, then blurs) and the `HPE_KEY_PASTE`/`COPY`/`CUT`/`SELECT_ALL` shortcuts.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `key` | in | 可打印码点（Unicode）或 `HPE_KEY_*` 键码 |

**返回**：地址栏消费了该输入返回 1，否则 0

**注意**：所有键都先作用于选区，与普通文本框一致：输入或退格替换选区，方向键折叠选区，Ctrl+C/X 对选区操作；等价于修饰键掩码为 0 的 `hpe_engine_key_mods`

**参见**：`hpe_engine_key_mods`、`hpe_engine_address_caret`

#### `hpe_engine_key_mods`

```c
int hpe_engine_key_mods(HpeEngine *engine, int key, int mods);
```

带修饰键地把按键送给地址栏

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `key` | in | 可打印码点（Unicode）或 `HPE_KEY_*` 键码 |
| `mods` | in | `HPE_MOD_*` 修饰键掩码 |

**返回**：地址栏消费了该输入返回 1，否则 0

**注意**：与 `hpe_engine_key` 相同，但额外携带修饰键掩码：Shift 扩展选区、Ctrl 按单词移动

**参见**：`hpe_engine_key`

#### `hpe_engine_pixmap`

```c
const HpePixmap *hpe_engine_pixmap(HpeEngine *engine);
```

取引擎的像素缓冲（上一帧结果）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：像素缓冲指针，`engine` 为 NULL 时返回 `NULL`

**注意**：须在 `hpe_engine_render` 之后读取，直到下次 `hpe_engine_resize` 前有效

**参见**：`hpe_engine_render`

#### `hpe_engine_status`

```c
const char *hpe_engine_status(HpeEngine *engine);
```

取状态行文本

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：状态文本（含文件名、尺寸、加载/绘制耗时、滚动位置，及粘性加载错误），`engine` 为 NULL 时返回空串

**参见**：`hpe_engine_load_file`

#### `hpe_engine_scroll_offset`

```c
float hpe_engine_scroll_offset(HpeEngine *engine);
```

取当前纵向滚动偏移

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：滚动偏移（像素，整像素取整后），`engine` 为 NULL 时返回 0

**参见**：`hpe_engine_scroll_by`

#### `hpe_engine_set_focus`

```c
void hpe_engine_set_focus(HpeEngine *engine, HpeDomNode *node);
```

设置拥有焦点的 DOM 节点

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，NULL 时直接返回 |
| `node` | in,out | 新的焦点元素，传 NULL 清除焦点 |

**注意**：`:focus` 是选择器，因此焦点变化会触发一次重排并刷新状态行；`node` 与当前焦点相同时不做任何事

**参见**：`hpe_engine_focused_node`、`hpe_engine_set_target`

#### `hpe_engine_focused_node`

```c
HpeDomNode *hpe_engine_focused_node(HpeEngine *engine);
```

取当前拥有焦点的 DOM 节点

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：焦点元素，无焦点或 `engine` 为 NULL 时返回 `NULL`

**参见**：`hpe_engine_set_focus`

#### `hpe_engine_set_target`

```c
void hpe_engine_set_target(HpeEngine *engine, HpeDomNode *node);
```

设置 URL 片段目标元素

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，NULL 时直接返回 |
| `node` | in,out | 新的片段目标元素，传 NULL 表示无目标 |

**注意**：`:target` 是选择器，因此变化会触发一次重排

**参见**：`hpe_engine_set_focus`

#### `hpe_engine_document_encoding`

```c
const char *hpe_engine_document_encoding(HpeEngine *engine);
```

取上次加载文档所用的字符编码

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：编码名（`utf-8`、`latin1`、`utf-16le`、`utf-16be`），`engine` 为 NULL 时返回空串

**注意**：直到下次加载前有效

**参见**：`hpe_engine_meta_charset`

#### `hpe_engine_meta_charset`

```c
int hpe_engine_meta_charset(HpeEngine *engine);
```

查询编码是否来自文档内 `<meta charset>`

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：来自文档内 `<meta charset>` 返回 1，否则 0

**注意**：与来自 BOM 或 UTF-8 校验的情况相区分

**参见**：`hpe_engine_document_encoding`

#### `hpe_engine_base_href`

```c
const char *hpe_engine_base_href(HpeEngine *engine);
```

取解析后的 `<base href>`

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：绝对基址，文档未声明时返回空串

#### `hpe_engine_link_count`

```c
int hpe_engine_link_count(HpeEngine *engine);
```

取非 stylesheet 的 `<link>` 资源数

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：记录的 `<link>` 资源提示个数（icon/preload/alternate 等），`engine` 为 NULL 时返回 0

**注意**：stylesheet 链接已被消费、不计入

**参见**：`hpe_engine_link_at`

#### `hpe_engine_link_at`

```c
int hpe_engine_link_at(HpeEngine *engine, int index, const char **rel, const char **href);
```

取某条 `<link>` 资源提示的归一化 rel 与解析后的绝对 href

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `index` | in | 资源提示下标（0 起） |
| `rel` | out | 接收归一化（小写、空格分隔）的 rel token 串，可为 NULL |
| `href` | out | 接收解析后的绝对地址，可为 NULL |

**返回**：`index` 有效返回 1，越界返回 0

**注意**：`index` 越界时不写出任何输出

**参见**：`hpe_engine_link_count`

**类型**

#### `HpeEngine`

```c
typedef struct HpeEngine HpeEngine;
```

#### `HpeDomNode`

```c
typedef struct HpeDomNode HpeDomNode;
```

### hope_details.h

0 个函数，0 个类型。

**函数**

无。

**类型**

无。

### hope_form.h

14 个函数，1 个类型。

**函数**

#### `hpe_form_kind`

```c
HpeFormKind hpe_form_kind(const HpeDomNode *node);
```

按标签与 type 属性归类控件

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in | 目标元素 |

**返回**：控件类别；非控件元素返回 `HPE_FK_OTHER`

#### `hpe_form_value_get`

```c
void hpe_form_value_get(const HpeDomNode *node, char *out, int cap);
```

读控件当前值

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in | 目标控件 |
| `out` | out | 接收缓冲 |
| `cap` | in | 缓冲容量 |

#### `hpe_form_value_set`

```c
void hpe_form_value_set(HpeDomNode *node, const char *v);
```

写控件当前值（不派发事件）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in,out | 目标控件 |
| `v` | in | 新值 |

#### `hpe_form_checked_get`

```c
int hpe_form_checked_get(const HpeDomNode *node);
```

读控件勾选状态

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in | 目标控件 |

**返回**：`checked` 属性存在返回 1

#### `hpe_form_checked_set`

```c
void hpe_form_checked_set(HpeDomNode *node, int on);
```

写控件勾选状态

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in,out | 目标控件 |
| `on` | in | 1 勾选 |

#### `hpe_form_owner`

```c
HpeDomNode *hpe_form_owner(const HpeDomNode *node);
```

找控件所属的 `<form>`

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `node` | in | 目标控件 |

**返回**：最近的 form 祖先，无则 `NULL`

#### `hpe_form_record_defaults`

```c
void hpe_form_record_defaults(HpeDomNode *doc);
```

文档加载后为全部控件记录默认值快照

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `doc` | in,out | 文档节点 |

**注意**：快照存于保留属性 `data-defval` / `data-defchk` / `data-defsel`， 供 reset 恢复

#### `hpe_form_reset`

```c
void hpe_form_reset(HpeDomNode *doc, HpeDomNode *form);
```

重置一个 form 的全部控件

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `doc` | in,out | 文档节点（按全树找该 form 的控件） |
| `form` | in,out | 目标 form，`NULL` 表示整份文档 |

#### `hpe_form_click`

```c
int hpe_form_click(HpeEngine *engine, HpeDomNode *node, int x, int y);
```

处理一次页面点击的控件默认行为

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `node` | in,out | 点击命中的元素 |
| `x` | in | 窗口 X 坐标 |
| `y` | in | 窗口 Y 坐标 |

**返回**：控件状态被改变返回 1（调用方须 flush）

**注意**：复选框翻转、单选钮组互斥、submit/reset、select 换选项、label 转发、 range 落点取值、文本控件聚焦；点击非可编辑控件时使文本控件失焦（触发 change）

#### `hpe_form_key`

```c
int hpe_form_key(HpeEngine *engine, int vk, int printable, int mods);
```

处理一次键盘事件的控件默认行为

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `vk` | in | 虚拟键码 |
| `printable` | in | 可打印码点（0 表示非打印键） |
| `mods` | in | 修饰键掩码 |

**返回**：控件状态被改变返回 1（调用方须 flush）

**注意**：仅作用于获得焦点的可编辑控件：打印字符插入、退格/删除、方向/Home/End 移动插入符； 每次编辑派发 `input`

#### `hpe_form_blur`

```c
void hpe_form_blur(HpeEngine *engine);
```

使当前编辑控件失焦（值有变则派发 `change`）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

#### `hpe_form_focus_notify`

```c
void hpe_form_focus_notify(HpeEngine *engine, HpeDomNode *node);
```

JS `focus()`/`blur()` 后同步表单编辑状态

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `node` | in,out | 新焦点元素，`NULL` 表示失焦 |

#### `hpe_form_install`

```c
void hpe_form_install(HpeEngine *engine);
```

文档加载后安装表单控件（默认值快照 + 按钮标签注入）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**注意**：记录全部控件的默认值快照（供 `reset()` 恢复），并给仅以 `value` 声明标签的 按钮注入文本子节点

#### `hpe_form_shutdown`

```c
void hpe_form_shutdown(HpeEngine *engine);
```

释放表单编辑状态（引擎销毁时调用）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**类型**

#### `HpeFormKind`

```c
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
```

表单控件类别

### hope_internal.h（内部）

33 个函数，6 个类型。

**函数**

#### `hpe_engine_relayout`

```c
void hpe_engine_relayout(HpeEngine *engine);
```

重建盒树并重新夹紧滚动偏移

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**注意**：急切入口：无条件全量重建；实现位于 `hope.c`；加载、缩放、悬停、定时器等路径使用，惰性策略在 `hpe_engine_flush`

**参见**：`hpe_engine_flush`

#### `hpe_engine_flush`

```c
void hpe_engine_flush(HpeEngine *engine, int force);
```

应用自上次以来记录的 DOM 变更，只做其所需的排版/重绘

**详细说明**：惰性重排策略：LAYOUT 重建受影响的盒子子树（作用域无界时整树）；PAINT 重算样式并重绘；NONE 完全跳过。作用域由变更汇聚点记录，可把重建限制在局部子树。 Lazy policy: LAYOUT rebuilds the affected box subtree (everything when unbounded), PAINT recomputes styles and repaints, and NONE skips entirely. The scope is recorded by the mutation sink, which can bound the rebuild to a partial subtree.

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例，NULL 时直接返回 |
| `force` | in | 非零请求全量重建 |

**注意**：实现位于 `hope.c`

**参见**：`hpe_engine_relayout`

#### `hpe_engine_refresh_status`

```c
void hpe_engine_refresh_status(HpeEngine *engine);
```

刷新状态行文本

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**注意**：实现位于 `hope.c`；汇总文件名、尺寸、是否含地址栏、加载/绘制耗时与滚动位置，并追加粘性加载错误

**参见**：`hpe_engine_status`

#### `hpe_engine_caret_advance`

```c
int hpe_engine_caret_advance(HpeEngine *engine);
```

推进地址栏插入符闪烁时钟

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：闪烁相位翻转返回 1，否则 0

**注意**：实现位于 `hope.c`；由 `hpe_engine_tick` 与 `hpe_chrome_paint` 共同驱动

**参见**：`hpe_engine_tick`、`hpe_chrome_paint`

#### `hpe_engine_view_height`

```c
int hpe_engine_view_height(HpeEngine *engine);
```

取页面视口高度（窗口高减地址栏）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：视口高度（像素），不小于 0；`engine` 为 NULL 时返回 0

**注意**：实现位于 `hope.c`

#### `hpe_sel_has`

```c
int hpe_sel_has(const HpeEngine *engine);
```

查询是否存在非折叠的页面选区

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in | 引擎实例 |

**返回**：有选区返回 1，否则 0

#### `hpe_sel_mouse_down`

```c
int hpe_sel_mouse_down(HpeEngine *engine, int x, int y, int mods);
```

处理页面上的指针按下（开始/扩展/双击选词）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `x` | in | X 坐标（窗口坐标） |
| `y` | in | Y 坐标（窗口坐标） |
| `mods` | in | 修饰键掩码，`HPE_MOD_SHIFT` 扩展既有选区 |

**返回**：选区状态发生变化返回 1，否则 0

**注意**：命中非文本区域时折叠既有选区；双击选中光标处的词

#### `hpe_sel_mouse_move`

```c
int hpe_sel_mouse_move(HpeEngine *engine, int x, int y);
```

拖选进行中处理指针移动（扩展焦点端点）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `x` | in | X 坐标（窗口坐标） |
| `y` | in | Y 坐标（窗口坐标） |

**返回**：选区变化返回 1，否则 0

#### `hpe_sel_mouse_up`

```c
void hpe_sel_mouse_up(HpeEngine *engine);
```

结束拖选

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

#### `hpe_sel_copy`

```c
int hpe_sel_copy(HpeEngine *engine);
```

把选区文本写入剪贴板

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：写入成功返回 1，无选区或文本为空返回 0

**注意**：区块盒之间补换行符，与浏览器复制行为一致

#### `hpe_sel_select_all`

```c
int hpe_sel_select_all(HpeEngine *engine);
```

全选文档文本

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：文档含文本且选区变化返回 1，否则 0

#### `hpe_sel_clear`

```c
int hpe_sel_clear(HpeEngine *engine);
```

清除页面选区

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：原有选区被清除返回 1，本无选区返回 0

#### `hpe_sel_prepare`

```c
void hpe_sel_prepare(HpeEngine *engine);
```

重建逐盒选区区间表并交给绘制模块

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**注意**：每次重绘页面前调用；选区失效（节点被移除）时自然产生空表

#### `hpe_sel_reset`

```c
void hpe_sel_reset(HpeEngine *engine);
```

无条件丢弃选区状态（换页时调用，不触发重绘）

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**注意**：旧文档的 DOM 节点即将销毁，端点引用必须清空；释放区间表缓冲由 hpe_sel_shutdown 负责

**参见**：`hpe_sel_shutdown`

#### `hpe_sel_shutdown`

```c
void hpe_sel_shutdown(HpeEngine *engine);
```

释放选区模块的缓冲

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

#### `hpe_anim_install`

```c
void hpe_anim_install(HpeEngine *engine);
```

注册层叠钩子并重置动画世界

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**注意**：在引擎创建时调用；时钟归零、节点状态表清空

#### `hpe_anim_tick`

```c
int hpe_anim_tick(HpeEngine *engine);
```

推进动画时钟并求值所有运行项

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：仍有进行中的过渡或动画返回 1，否则 0

**注意**：`HOPE_TEST_ANIM_STEP=秒` 时每 tick 固定推进该秒数（无头回归用确定性时钟），否则按墙钟差推进（截断到 0.25s）；活动项所需重排级别写入 `engine->anim.reflow_need`

#### `hpe_anim_reset`

```c
void hpe_anim_reset(HpeEngine *engine);
```

换页时重置动画世界

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**注意**：时钟归零、节点状态表清空；缓冲保留复用

#### `hpe_anim_shutdown`

```c
void hpe_anim_shutdown(HpeEngine *engine);
```

释放动画世界缓冲

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

#### `hpe_anim_snapshot_dump`

```c
void hpe_anim_snapshot_dump(HpeEngine *engine, const char *id);
```

打印某元素当前动画/过渡后的计算值快照

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in | 引擎实例 |
| `id` | in | 目标元素的 id 属性 |

**注意**：`HOPE_TEST_ANIM_SNAPSHOT=id` 测试钩子使用；输出 `ANIM-SNAP <id> ...` 行，值取该元素盒树的最新级叠结果

#### `hpe_engine_tab_new`

```c
int hpe_engine_tab_new(HpeEngine *engine);
```

新建一个空白标签并切换过去

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：成功返回 1，已达上限返回 0

#### `hpe_engine_tab_close`

```c
int hpe_engine_tab_close(HpeEngine *engine, int index);
```

关闭指定下标的标签

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `index` | in | 待关闭的标签下标 |

**返回**：成功返回 1，下标非法返回 0

**注意**：关闭最后一个标签会重置为空白页而不是关闭

#### `hpe_engine_tab_switch`

```c
int hpe_engine_tab_switch(HpeEngine *engine, int index);
```

切换到指定下标的标签

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `index` | in | 目标标签下标 |

**返回**：成功返回 1，下标非法或已是当前标签返回 0

**注意**：切换会保存当前标签的路径/历史/滚动，重载目标标签的文档并恢复其滚动

#### `hpe_engine_tab_count`

```c
int hpe_engine_tab_count(HpeEngine *engine);
```

取当前打开的标签数

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in | 引擎实例 |

**返回**：标签数，`engine` 为 NULL 时返回 0

#### `hpe_engine_tab_active`

```c
int hpe_engine_tab_active(HpeEngine *engine);
```

取当前活动标签下标

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in | 引擎实例 |

**返回**：活动标签下标，`engine` 为 NULL 时返回 -1

#### `hpe_chrome_set_text`

```c
void hpe_chrome_set_text(HpeEngine *engine, const char *text);
```

替换地址栏文本并把插入符移到末尾

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `text` | in | 新文本，超长时按输入框容量截断 |

**注意**：选区折叠，并记录为 Escape 回退所用的地址；实现位于 `hope_chrome.c`

**参见**：`hpe_chrome_navigate`

#### `hpe_chrome_paint`

```c
void hpe_chrome_paint(HpeEngine *engine);
```

把地址栏绘制到顶部 `chrome_h` 像素

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**注意**：实现位于 `hope_chrome.c`；由 `hpe_engine_render` 在整帧或仅地址栏脏时调用

**参见**：`hpe_engine_render`

#### `hpe_chrome_mouse`

```c
int hpe_chrome_mouse(HpeEngine *engine, int x, int y, int down, int mods);
```

处理地址栏条带内的一次指针按下 / 抬起

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `x` | in | X 坐标（设备坐标） |
| `y` | in | Y 坐标（设备坐标） |
| `down` | in | 按下为 1、抬起为 0 |
| `mods` | in | 修饰键掩码，`HPE_MOD_SHIFT` 时 Shift+点击扩展选区 |

**返回**：事件被地址栏消费返回 1，否则 0

**注意**：实现位于 `hope_chrome.c`

**参见**：`hpe_chrome_mouse_move`

#### `hpe_chrome_mouse_move`

```c
int hpe_chrome_mouse_move(HpeEngine *engine, int x, int y);
```

地址栏拖选进行中时处理指针移动

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |
| `x` | in | X 坐标（设备坐标） |
| `y` | in | Y 坐标（设备坐标） |

**返回**：事件被地址栏消费返回 1，否则 0

**注意**：实现位于 `hope_chrome.c`；即使指针离开条带，源自条带内的拖拽仍继续接收移动

**参见**：`hpe_chrome_mouse`

#### `hpe_chrome_cursor_at`

```c
int hpe_chrome_cursor_at(HpeEngine *engine, int x, int y);
```

查询条带内某点的光标语义

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in | 引擎实例 |
| `x` | in | X 坐标（设备坐标） |
| `y` | in | Y 坐标（设备坐标） |

**返回**：`HpeCursor` 值；点在条带之外返回 -1

**注意**：输入框内为 I-beam、条带其余处为箭头；实现位于 `hope_chrome.c`

**参见**：`hpe_chrome_mouse`

#### `hpe_engine_back`

```c
int hpe_engine_back(HpeEngine *engine);
```

按下后退按钮并回到上一历史项

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：发生了导航返回 1，否则 0

**注意**：无上一项时什么都不做；实现位于 `hope.c`

**参见**：`hpe_engine_forward`

#### `hpe_engine_forward`

```c
int hpe_engine_forward(HpeEngine *engine);
```

按下前进按钮并前往下一历史项

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：发生了导航返回 1，否则 0

**注意**：无下一项时什么都不做；实现位于 `hope.c`

**参见**：`hpe_engine_back`

#### `hpe_chrome_navigate`

```c
int hpe_chrome_navigate(HpeEngine *engine);
```

按地址栏内容（`addr`）导航

**参数**：

| 名称 | 方向 | 说明 |
| --- | --- | --- |
| `engine` | in,out | 引擎实例 |

**返回**：加载成功返回 1，否则 0

**注意**：实现位于 `hope_chrome.c`；文本为空时重新加载内置空白页

**参见**：`hpe_chrome_set_text`、`hpe_engine_load_blank`

**类型**

#### `HpeSelPos`

```c
typedef struct HpeSelPos {
    HpeDomNode *node; /* 所属文本节点 / Owning DOM text node */
    int off;          /* 节点文本内的字节偏移 / Byte offset within the node's text */
} HpeSelPos;
```

#### `HpeTab`

```c
typedef struct HpeTab {
    int used;                                 /* 槽位在用 / Slot in use */
    char path[HPE_MAX_PATH];                  /* 当前文档路径（""=空白页）/ Current document path (""=blank) */
    char nav[HPE_NAV_MAX][HPE_MAX_PATH];      /* 每标签历史环 / Per-tab history ring */
    int nav_count;                            /* 历史项数 / History entry count */
    int nav_pos;                              /* 当前历史下标 / Current history index */
    float scroll_y;                           /* 切走时的滚动偏移 / Scroll offset when last left */
} HpeTab;
```

#### `HpeAnimRun`

```c
typedef struct HpeAnimRun {
    int used;
    char name[HPE_ANIM_NAME_MAX];
    double start;         /* 引擎动画时钟下的起始时刻 / Start on the engine's animation clock */
    HpeAnimDecl decl;     /* 声明快照（direction/fill/迭代等）/ Declaration snapshot */
} HpeAnimRun;
```

#### `HpeAnimTransRun`

```c
typedef struct HpeAnimTransRun {
    int used;
    int prop;             /* HpeAnimProp */
    double start;
    float duration;
    HpeEasing easing;
    HpeAnimValue from, to;
} HpeAnimTransRun;
```

#### `HpeAnimNodeState`

```c
typedef struct HpeAnimNodeState {
    HpeDomNode *node;
    int used;
    HpeAnimValue prev[HPE_AP_COUNT]; /* 上次层叠的目标值（用于变化检测）/ Last cascade's target values */
    int has_prev;
    HpeAnimRun runs[HPE_ANIM_MAX];
    HpeAnimTransRun trans[HPE_TRANS_MAX];
} HpeAnimNodeState;
```

#### `HpeAnimWorld`

```c
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
```
