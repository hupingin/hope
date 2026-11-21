# Hope UI Engine — 设计文档

> 项目名称 **Hope（希望）**。一款从零自研的、类浏览器的 UI / 渲染引擎。
> 设计目标：遵循 W3C 规范（HTML / CSS / DOM / JS 子集），**纯 C11、零第三方库**，
> 仅解析本地 `html + css + js` 文件，不做联网能力（区别于 Chromium 的完整目标）。

---

## 1. 总体设计目标与范围

| 项目 | 说明 |
| --- | --- |
| 终极目标 | 支持 W3C 规范、类似 Chromium 的渲染引擎 |
| 明确不做 | 网络连接 / 远程资源加载 / JS 网络 API |
| 输入 | 本地 HTML 文档 + 本地 CSS + 本地 JS（`<script src>` 也只取本地相对路径） |
| 输出 | 软件渲染到像素缓冲，经 Win32 上屏；支持无头 `--dump` 截图 |
| 语言 | 纯 C11，不用 C++ |
| 依赖 | 无第三方库；系统能力（窗口、字体、文件）仅经平台抽象层访问 |

**里程碑路线（已落地 → 规划中）**
1. ✅ 垂直竖切：HTML → CSS → 布局 → 光栅 → 上屏（v0.1）
2. ✅ JS 引擎（自研解释器 + DOM 绑定 + 事件）（v0.2）
3. ✅ 布局补全（一）：margin 塌陷、position relative/absolute/fixed、inline-block、z-index（v0.3）
4. ✅ 浮动/清除、脏矩形、定时器、字体掩码缓存（v0.4）
5. ✅ 浏览器外壳（自绘地址栏）、内置空白页（v0.5）
6. ✅ flex 单行、圆角、SVG 子集、地址栏文本编辑（v0.6）
7. ✅ JS 语言大补齐（class/解构/async/生成器/正则/ES 模块）（v0.7）
8. ✅ JS 内置库补齐（Date / Map·Set / Array·String·Number·Math·Object）（v0.8）
9. ✅ 样式属性批次：四色边框、min/max、box-sizing、overflow、opacity/visibility、渐变、阴影、transform、相对单位、间距、list-style（v0.9）
10. ✅ 层叠与继承补全：`!important` 参与层叠、`@media` 条件求值、继承属性集合（v0.10）
11. ✅ SVG 绘制子集扩展：仿射 transform、新图元、`defs`/`use`、线性/径向渐变、`path` 全命令与命中测试（v0.11）
12. 🔲 布局补全（二）：grid、table、flex wrap/shrink/order、元素级滚动容器
13. 🔲 绘制补全（二）：`<img>`、滚动条、页面内文本选中、radial 渐变、`background-position/size`
14. 🔲 样式补全（三）：结构伪类、伪元素、状态伪类、`skew`/`matrix`、`transition` 时钟
15. 🔲 JS 补全：`Proxy`/`Reflect`、`Intl`、`WeakMap`、TypedArray、真正的事件循环与 GC
16. 🔲 基础设施：DOM/CSS 增量更新、JS GC、跨平台后端

> 注：早期规划中的「float / flex 属绘制补全」已在 v0.4 / v0.6 提前落地，本列表据实回填。

---

## 2. 总体架构

引擎是一条单向流水线。每个阶段产出明确的中间结构，彼此解耦：

```
       本地文件
          │
          ▼
   ┌──────────────┐   HTML 词法 + 树构建
   │  HTML 解析    │─────────────────────────────►  DOM 树 (HpeDomNode)
   └──────────────┘                                   │
          │                                           │
          │ collect_styles                            │
          ▼                                           │
   ┌──────────────┐   选择器解析 + 层叠 + 继承        │ hpe_css_compute(node, parent)
   │  CSS 引擎     │──────────────────────────────────┘
   └──────────────┘   UA 样式表 + author 样式（<style>/<link>/行内）
          │
          ▼
   ┌──────────────┐   盒树 + 块流 / 行内流 / 折行
   │  布局         │─────────────────────────────►  盒树 (HpeBox) + 行/run
   └──────────────┘
          │
          ▼
   ┌──────────────┐   递归绘制：背景 / 边框 / 文本行 / 列表标记
   │  绘制         │─────────────────────────────►  Pixmap (0xAARRGGBB)
   └──────────────┘
          │
          ▼
   ┌──────────────┐   软件光栅：矩形填色 / 文本覆盖率掩码混合
   │  光栅 / 字体   │◄── GDI 字形覆盖率掩码（仅 Win32 平台层）
   └──────────────┘
          │
          ▼
   ┌──────────────┐   StretchDIBits 零转换上屏
   │  平台 / 窗口   │
   └──────────────┘
```

**模块依赖方向（自上而下，禁止反向）：**
`hope.c`(门面) → `html` → `dom` → `css` → `layout` → `paint` → `render`(raster/font/pixmap) → `platform`。
除 `src/platform/<os>/` 外，任何模块都只能通过 `hpe_platform.h` 接触系统能力；其余模块相互调用须经过头文件公共 API。

---

## 3. 内存模型

两级分配策略：

1. **通用堆（`base/hpe_util.h`）** — `hpe_alloc/hpe_calloc/hpe_realloc/hpe_free/hpe_grow/hpe_strdup`。
   DOM、CSS 样式表、盒树等长期结构使用它，并各自提供 `*_free` 析构（如 `hpe_dom_free`、`hpe_layout_free`、`hpe_css_free`）。
2. **JS 专用 bump arena（`js/hpe_js_internal.h`）** — `struct HpeJs` 持有一串 `JsBlock`（每块约 256KB）。
   AST 节点、JsObj、JsValue 字符串全部从 arena 分配；**整页重载或 `hpe_js_destroy` 时整块释放**，不做逐对象 GC。
   优点：分配零碎片、极快；代价：运行期的内存无法单对象回收（后续里程碑要补 GC）。

公共分配语义：任何模块需要内存都走 `hpe_alloc` 系列，便于将来统一换页/统计。

---

## 4. 平台抽象层（`src/platform/hpe_platform.h`）

把“操作系统相关”收敛到一层接口，使引擎其余部分可在无窗口环境编译与测试（无头 `--dump` 正是如此）。

| 能力 | 接口 | 说明 |
| --- | --- | --- |
| 窗口 | `hpe_window_create / present / run / destroy` | `present` 接收 BGRA 像素、宽高、行字节数，直接交给 GDI |
| 文件 | `hpe_read_file` | 读本地文件，返回 NUL 结尾缓冲（调用方 `hpe_free`） |
| 路径 | `hpe_path_dir / is_absolute / join` | 解析 `<link>`/`<script src>` 的本地相对路径 |
| 时间 | `hpe_time_seconds` | 统计 load / paint 耗时 |
| 剪贴板 | `hpe_clipboard_get_text / hpe_clipboard_set_text` | 地址栏 Ctrl+V（UTF-8 文本；文件以路径呈现） |
| 字体 | `hpe_plat_font_create / *_ascent / *_descent / *_line_height / *_advance / *_mask` | 见 §9 字体与光栅 |

**回调约定（`HpeWindowCallbacks`）：** `on_paint / on_resize / on_scroll / on_key / on_char / on_mouse / on_tick`。
鼠标：`x,y` 为视口（client）坐标，`button 0 = 左键`，`down: 1=按下 / 0=松开`，`button -1` 表示纯移动；
`mods` 是事件时刻的 `HPE_MOD_*` 掩码（Shift+点击扩展文本选区要用）。该约定贯穿到引擎门面的
`hpe_engine_mouse_event_mods`（旧签名 `hpe_engine_mouse_event` 退化为 `mods = 0`）。
按下左键时平台层 `SetCapture`、松开时 `ReleaseCapture`，因此拖选时指针移出窗口仍能收到移动事件。
键盘：`on_key(vk, mods)` 的 `mods` 是事件时刻的 `HPE_MOD_CTRL/SHIFT/ALT` 掩码（快捷键需要），`on_char(codepoint)` 承载已翻译的字符。

---

## 5. 像素格式与呈现

- 像素格式固定为 **`0xAARRGGBB`**，自上而下（top-to-bottom）排列，`HpePixmap.stride` 为每行字节数。
- 这一选择与 Win32 `BITMAPINFO` + `StretchDIBits` 完全兼容，**上屏时零格式转换**。
- `hpe_argb(c)` 把 `HpeColor{r,g,b,a}` 编码为该 32 位值；`hpe_pixmap_clear` 用页面底色铺满背景。

```c
typedef struct HpePixmap {
    int width, height;
    int stride;        /* bytes per row */
    uint32_t *pixels;  /* 0xAARRGGBB, top-down */
} HpePixmap;
```

---

## 6. HTML 解析（`src/html/hpe_html.c`）

手写 **词法分析 + 简化树构建**（非完整 HTML5 状态机，但覆盖常见文档）：

- 标签 / 属性 / 注释 / DOCTYPE 识别。
- **隐式结构**：自动补全 `<html><head></head><body>...</body></html>`。
- **原始文本元素（rawtext）**：`<style>` 与 `<script>` 的内容直接作为文本子节点挂到该元素下（早期 bug 曾把文本挂到 `head`，已修正——作者样式因此丢失的问题已解决）。
- **实体解码**：`&amp; &lt; &gt; &quot; &#NN;` 等还原为字符。
- 大小写：元素 `tag` 统一存为小写；属性名比较用 `hpe_str_ieq`（不区分大小写，符合 HTML 属性名语义）。

输出：始终一个 `HpeDomNode`（document）节点，内部含 `<html>/<head>/<body>`。

---

## 7. DOM 模型（`src/dom/hpe_dom.h`）

```c
typedef enum HpeNodeType { HPE_NODE_DOCUMENT, HPE_NODE_ELEMENT, HPE_NODE_TEXT } HpeNodeType;

typedef struct HpeDomNode {
    HpeNodeType type;
    char *tag;                 /* 元素：小写标签名 */
    char *text;                /* 文本：解码后的内容 */
    HpeAttr *attrs; int attr_count;   /* 属性表（name/value） */
    struct HpeDomNode *parent, *first_child, *last_child, *next_sibling;
    void *listeners;           /* DOM 桥：每节点事件监听器链表（opaque） */
} HpeDomNode;
```

- 树操作：`hpe_dom_element / text / document / append / detach`。
- 属性：`hpe_dom_attr`（名不区分大小写）/ `hpe_dom_set_attr`。
- 兄弟遍历：`hpe_dom_prev_sibling` —— 为 CSS 相邻/通用兄弟组合符（`+` / `~`）提供支撑（详见 §8）。
- `listeners` 指针由 JS DOM 桥使用，核心 DOM 不解释其内容。

---

## 8. CSS 引擎（`src/css/hpe_css.h/.c`）

### 8.1 选择器

（组合符、属性选择器见既有条目）**动态伪类 `:hover`（v0.4 新增）**：`HpeCompound.pseudo_hover`；
解析期识别 `:hover`（其他伪类仍忽略不致失败），specificity 按类计 +10；匹配期检查
`HpeDomNode.hovered` 标志。hover 状态由引擎命中测试维护（§14 `mouse_move`），变化即全量重层叠。
**存量 bug 修复（v0.4）**：`parse_selector_list` 曾因"先跳空格再判组合符"的顺序错误，把
`a b` 裸后代选择器的后续 part 全部丢弃（`>` `+` `~` 不受影响）——已修正，`.card h2`、`.box h3`
等规则此前从未生效。

- **基础选择器**：类型（标签）、`#id`、`.class`、通配 `*`。
- **组合符（`HpeCombinator`）**：后代 ` `、子代 `>`、相邻兄弟 `+`、通用兄弟 `~`。
  每条 compound 记录其与左侧部分的组合关系（`HpeCompound.comb`），匹配时回溯 `hpe_dom_prev_sibling`。
- **属性选择器（`HpeAttrSel`）**：`[a]`、`[a=v]`、`[a~=v]`、`[a|=v]`、`[a^=v]`、`[a$=v]`、`[a*=v]`。
  `op` 为空表示存在性检查，`name/value` 为属性名与值。

```c
typedef struct HpeCompound {
    char *tag;        /* NULL 或 "*" = 任意元素 */
    char *id;
    char **classes; int class_count;
    HpeAttrSel *attrs; int attr_count;
    int comb;         /* 与左侧 compound 的关系 */
} HpeCompound;
```

- **特异性（specificity）**：每条 `HpeSelector` 计算一个整数 `specificity`，与来源顺序共同决定层叠胜负。
- **`@media`（v0.10 条件求值）**：`@media` 条件在**层叠期**按当前视口求值，不匹配的规则整体跳过。
  支持的媒体类型为 `all` / `screen` / `print`（`print` 在屏幕渲染下恒不匹配），条件特性为
  `min-width` / `max-width` / `min-height` / `max-height` / `width` / `height`（`px` / `em` 等长度）。
  支持 `and` 串联、`not` / `only` 前缀、逗号分隔的「或」列表（任一分支匹配即整体匹配），以及
  嵌套 `@media`（逐层求值，全部匹配才生效）。解析期把每个 `@media` 块的条件压入
  `HpeRule.mq[]`（`mq_depth` 记录嵌套层数），规则本身仍完整收入样式表。
  视口尺寸在每次重层叠前由 `hpe_css_set_viewport(sheet, vw, vh)` 写入（`HpeStylesheet.vw/vh`，
  由 `hpe_engine_relayout` 传入引擎宽度与可视高度）。

### 8.2 层叠与继承

- 来源顺序：`UA 样式表`（`hpe_ua_stylesheet`，引擎内置的默认样式）→ `author 样式`（`<style>`、`<link>`、行内 `style`）。
- `HpeStyle` 用 **`set` 位掩码**记录“哪些属性被显式声明”，这是层叠/继承正确性的关键：
  未被声明的属性不会用默认值“覆盖”继承值。属性位分两个字：`set`（`HPE_PROP_*`）与
  `set2`（`HPE_PROP2_*`）；`imp` / `imp2` 镜像地记录“该属性是否以 `!important` 声明”。
- **层叠算法（v0.10 两趟）**：`hpe_css_compute` 先收集所有匹配规则（每条取最高 specificity 的选择器）
  并按 (specificity, 来源顺序) 稳定排序，**先应用全部 normal 声明**（样式表按序，随后行内 `style=""`
  的 normal 声明——行内 normal 胜任何 normal 规则），再**第二趟应用全部 important 声明**
  （important 胜任何 normal 声明，与 specificity 无关）。important 来源内部同样按
  (specificity, 来源顺序) 排序，其中行内 `style=""` 的 important 取 specificity=(1<<20)、
  order=(1<<30)，从而胜出样式表 important。
- **`!important` 解析（v0.10）**：`strip_important` 在剥除 `!important` 的同时回报其是否存在，
  逐条声明把命中的属性位记入 `imp` / `imp2`。
- **继承属性**（在 `hpe_css_compute` 中从 `parent` 继承）：`color`、`font-size`、`font-weight`、
  `text-align`、`line-height`、`white-space`、`font-family`（v0.1 起），以及
  **`visibility`、`cursor`、`list-style-type`、`list-style-position`、`letter-spacing`、
  `word-spacing`、`text-indent`（v0.10 补全）**。
- **`visibility` 子树的可见性回流（v0.10）**：`visibility: hidden` 隐藏本盒自身的绘制
  （背景 / 边框 / 文字 / 装饰）但**保留占位、且不阻断子树**——后代若声明
  `visibility: visible` 可重新显现。绘制期 `paint_box` 不再对 hidden 盒提前返回，而是继续递归；
  `paint_box_inner` 用 `self_visible` 门控本盒自身的绘制那一整块，子树递归照常执行。

### 8.3 已支持的样式属性

| 属性 | 取值 | 备注 |
| --- | --- | --- |
| `display` | inline / **inline-block** / block / **flex** / none | inline-block 见 §10.5，flex 见 §10.9（`inline-flex` 按块级 flex 处理）|
| `color` / `background(-color)` | 颜色 | `background` 解析为 background-color |
| `background-image` / `background`（渐变） | `linear-gradient(...)` | v0.9：线性渐变（方向关键字 / 角度、若干色标），作为背景图像层填充盒 |
| `width` / `height` / `min-width` | `px` / `%` / `auto` | 百分比相对包含块的宽/高（v0.6 起 `height` 百分比生效）|
| `min-width` / `max-width` / `min-height` / `max-height` | `px` / `%` / `em` / `rem` / `vw` / `vh` / `ch` | v0.9：显式尺寸求解后再按 min/max 夹取 |
| `box-sizing` | content-box / border-box | v0.9：border-box 时显式宽高即边框盒（减去 padding+border）|
| `overflow` | visible / hidden / auto / scroll | v0.9：`hidden/auto/scroll` 对 pad-box 做绘制裁剪（元素级滚动条未实现）|
| `opacity` | 0~1 | v0.9：整棵子树离屏合成后按 alpha 混合（嵌套上限 3 层）|
| `visibility` | visible / hidden / collapse | v0.9：hidden 保留占位、不绘制；**v0.10：hidden 不阻断子树，后代 `visible` 可重现**；继承 |
| `cursor` | 关键字 / `auto` | v0.9：解析并随盒记录（平台层可查询）；v0.10：继承 |
| `text-decoration` | none / underline / overline / line-through | v0.9：可叠加（位标志）|
| `box-shadow` / `text-shadow` | `[ox oy blur spread] color` | v0.9：盒阴影 / 文字阴影 |
| `transform` | `translate` / `scale` / `rotate` | v0.9：2D 仿射；绘制期按盒左上角为原点，离屏合成并逆映射采样 |
| `transition` / `animation` | 简写 | v0.9：**仅解析存储为标志，无插值时钟**（见 §17 缺口）|
| `letter-spacing` / `word-spacing` / `text-indent` | `px`（或长度）| v0.9：逐字形推进；`text-indent` 仅作用于首行；v0.10：三者均继承 |
| `vertical-align` | baseline / top / middle / bottom / text-top / text-bottom | v0.9：作用于原子行内盒（inline-block）|
| `list-style-type` | disc / circle / square / none / decimal | v0.9：列表项标记绘制；v0.10：继承（逐项可覆盖）|
| `list-style-position` | inside / outside | v0.9：outside 标记悬挂于内容盒左侧；v0.10：继承 |
| `margin[top/right/bottom/left]` | 1~4 值简写、`auto`、**负值** | `auto` 水平居中；v0.9 支持负 margin（溢出悬挂）+ CSS §8.3.1 塌陷 |
| `padding[top/right/bottom/left]` | 1~4 值简写 | — |
| `border` / `border-width` / **`border-color`** | 数值 / 颜色；`border-color` 支持 **1~4 值** | v0.9：`border-color` 四值按 T R B L 重复表展开，**四边独立颜色** |
| `border-top/right/bottom/left` | `宽度 颜色...` 简写 | v0.6：**按边独立宽度**；v0.9：其中颜色也按边独立 |
| `border-radius` | `px` / `%` | v0.6：抗锯齿圆角填充与描边（百分比在绘制期按边框盒解算）|
| `font-size` / `font-weight` | 数值 / `bold` 等；`font-size` 支持 `em` / `rem` | — |
| `text-align` | left / center / right | 继承 |
| `line-height` | `≤0`=auto；负值 `-n`=字号的 n 倍 | — |
| `white-space` | normal / nowrap / pre / pre-wrap / pre-line | 见 §8.4 |
| `font-family` | serif / sans-serif / monospace / 具体字体名 | 见 §9 映射 |
| `position` | static / **relative** / **absolute** / **fixed** | 见 §10.6 |
| `top/right/bottom/left` / `inset` | `px` / `%` / auto；`inset` 为 1~4 值简写 | 仅对 relative/absolute/fixed 生效 |
| `z-index` | 整数 / auto | 定位盒绘制排序 |
| `flex-direction` | row / column | v0.6，见 §10.9 |
| `align-items` / `align-self` | stretch / flex-start / center / flex-end / baseline | v0.6 |
| `justify-content` | flex-start / center / flex-end / space-between / space-around | v0.6 |
| `gap` | `px` | v0.6（`row-gap`/`column-gap` 视作同一值）|
| `flex` / `flex-grow` | 数值 | v0.6，仅分配剩余空间，不支持 `flex-shrink`/`basis` |

**长度单位解析（v0.9 精确化）**：`px` / `%` 之外，`em`（相对元素自身 font-size）、`rem`（相对根
font-size）、`vw` / `vh`（相对视口宽/高）、`ch`（相对 `'0'` 字形推进宽度）、`pt` 均在布局期按上
下文解算，`hpe_len_resolve_full(len, base, font_size, root_font_size, vw, vh, ch_width)` 为统一入口。
百分比一律相对包含块宽度（`height` 相对包含块高度）。

- `!important`：目前在解析期被**剥离**但未真正参与层叠优先级（列入缺口）。
- 颜色解析 `hpe_color_parse` 支持 `#rgb` / `#rgba` / `#rrggbb` / `#rrggbbaa`、
  `rgb()/rgba()`（数值或百分比、可选 alpha，逗号或斜杠分隔）、
  `hsl()/hsla()`、`transparent` 及**完整 CSS Color 4 具名色表（148 项）**。
  函数记号允许参数间含空格，值分词 `split_ws` 会保持括号内空白不切分。

### 8.4 white-space 处理

`flush_paragraph`（布局阶段）按模式决定空白与换行：
- `normal`：折叠空白、按容器宽度折行；
- `nowrap`：不折行；
- `pre` / `pre-wrap`：保留空格，遇 `\n` 硬断行（`pre-wrap` 仍允许软折行）；
- `pre-line`：折叠空格但保留硬换行。

---

## 9. 字体与光栅化

### 9.1 字体对象（`src/render/hpe_font.h/.c`）

- `hpe_font_get(pixel_size, weight, family)` 返回**按 (字号, 字重, 字族) 缓存**的 `HpeFont`，调用方不直接释放。
- 字族映射（`HpeFontFamily`）：`serif → Times New Roman`、`monospace → Consolas`、`sans/default → Microsoft YaHei`。
- 提供 `ascent/descent/line_height/advance/measure/mask`，`advance` 与 `measure` 内部按码点缓存字宽。
- **文本运行掩码缓存（新增）**：`hpe_font_mask(font, text, len)` 返回**缓存所有**的 `const HpeTextMask *`（借用指针，调用方不得 free）。每个 `HpeFont` 内含 1024 槽开放寻址哈希表，键 = 文本字节 FNV-1a 64 位哈希 + 长度；负载超过 3/4 时整表清空重建（与字体表刷新同风格）。DOM 文本运行跨帧内容不变，故稳态帧完全不再触达 GDI。`HPE_NO_MASK_CACHE=1` 可旁路缓存（基准对比用）。基准：hello 页 12.6→3.5ms、css 页 13.7→2.0ms（1000×700）。

### 9.2 软件光栅（`src/render/hpe_raster.h/.c`）

- `hpe_fill_rect` / `hpe_fill_rect_i`：矩形填色，**支持 alpha 混合**（与 0xAARRGGBB 一致）。
- `hpe_draw_text_mask`：把一个 8 位覆盖率掩码（文字字形）按目标颜色混合进像素缓冲，实现**字形抗锯齿**。
- 两者都受 pixmap **裁剪矩形**约束（`clip_x0/y0/x1/y1`，默认全图）：脏矩形重绘与区域重绘的基础设施（v0.4 新增）；`hpe_pixmap_shift_v` 支持整幅内容垂直 memmove 平移。

### 9.3 GDI 覆盖率掩码（仅 Win32 平台层）

- 平台层用 GDI 把字形绘制进 DIB，取亮度作为 **8 位 coverage 掩码**（0~255）。
- 引擎自己的光栅把掩码与目标颜色按比例混合 —— 中文等复杂字形因此可正确渲染。
- `HpeTextMask { coverage, width, height, baseline }`：相对基线的 (x∈[0,width), y∈[-descent,ascent))。
- 掩码由 `hpe_font` 层按"字体+文本"缓存（见 9.1），平台层仅在缓存未命中时被调用；掩码与绘制位置无关（绘制时 x/y 取整），故缓存键无需含坐标。

---

## 10. 布局（`src/layout/hpe_layout.h/.c`）

### 10.1 盒树（Box Tree）

```c
typedef enum HpeBoxKind { HPE_BOX_BLOCK, HPE_BOX_INLINE, HPE_BOX_TEXT, HPE_BOX_INLINE_BLOCK } HpeBoxKind;

typedef struct HpeBox {
    HpeBoxKind kind;
    HpeDomNode *node;
    HpeStyle style;
    HpeRect border_box, content_box;   /* 边框盒 / 内容盒（文档绝对坐标） */
    HpeRect pad_box;                   /* padding box：absolute 定位的包含块 */
    const char *text; int text_len;
    HpeLine *lines; int line_count;    /* 文本行（仅文本/行内盒） */
    struct HpeBox *parent, *first_child, *last_child, *next;
} HpeBox;
```

- 由 DOM 树 + 已计算样式构建；`border_box` / `content_box` 为绝对坐标。
- `HpeTextRun` / `HpeLine`：一行内的若干文本段（run），记录字形字体、颜色、x 偏移、宽度、ascent/descent/line-height；
  run 携带 `atomic` 指针时代表一个 **inline-block 原子盒**占位（本身不绘制文本，由其子树绘制）。
- 布局入口带一个内部 `LayoutCtx { vw, vh }`：视口尺寸作为 absolute/fixed 的最终包含块。

### 10.2 块级流（block flow）

- 块级盒子在垂直方向依次排布；宽度默认填满包含块。
- 水平 `margin: auto` 实现居中（`auto` 居中逻辑在布局阶段处理）。
- 百分比宽度相对包含块宽度解析（`hpe_len_resolve`）。
- **margin 塌陷**（§10.7）改变了块流游标的推进方式，是块布局的核心规则。

### 10.3 行内文本流与折行（inline text flow）

- 行内盒子收集文本，按**行**（`HpeLine`）切分；一行内多个 run。
- 折行规则：**拉丁文按单词（word）**、**CJK 按字符（char）**边界断行，兼顾英文与中文混排。
- 行高与基线：依据 `font.ascent/descent` 与 `line-height` 计算 `baseline`，保证多字号混排对齐。
- 文本节点在行内上下文**不做首尾空格裁剪**（早期 bug：trim 会误删 inline span 前的空格，已修正）。

### 10.4 原子行内盒：inline-block（v0.3 新增）

`display: inline-block` 的盒在**行内方向是原子的**（像一个大号字符），**块方向自成一行内流**：

1. `build_element` 生成 `HPE_BOX_INLINE_BLOCK` 盒；
2. `collect_inline` 遇到 inline-block 时**立即独立布局**（`layout_inline_block`）：
   显式 `width` 直接采用；`width:auto` 走 **shrink-to-fit**（先用超大可用宽度摆一遍，
   取实际内容最大宽度——行宽与子块右缘的最大值，再夹到父容器内宽后重排）；
3. 以 `atomic` run 的形式占进段落，参与正常折行（放不下整体换行）；
4. 行度量完成后按**基线对齐**放置：盒子的基线取末行基线（无内容行则取底边），
   通过 `box_shift` 把盒及其子树平移到行内位置；
5. 绘制阶段 inline-block 盒走普通递归（背景/边框/文本都正常绘制）。

### 10.5 定位（v0.3 新增）

| position | 文档流 | 包含块 | 偏移方式 |
| --- | --- | --- | --- |
| `static` | 正常 | — | — |
| `relative` | **保留占位** | 自身正常位置 | 布局完成后对整个子树 `box_shift(top/left…)`；`right/bottom` 取负 |
| `absolute` | **脱离流** | 最近非 static 祖先的 `pad_box`，没有则视口 | 由 `top/left/right/bottom` 解算；支持 `left+right` 拉宽、shrink-to-fit、`bottom` 先测高后平移 |
| `fixed` | 脱离流 | 视口（`LayoutCtx.vw/vh`） | 同 absolute |

- absolute/fixed 在 `build_element` 中**块化**（blockified）为块盒，`layout_block` 的普通流循环跳过它们，
  待本盒 `pad_box` 与 relative 偏移都完成后，再统一调用 `layout_abspos` 二次定位。
- 顺序保证：`pad_box 确定 → relative 偏移 → abspos 子盒定位`，使相对定位容器内的绝对定位子元素坐标正确。

### 10.6 堆叠顺序与 z-index（v0.3 新增）

`paint_box` 绘制子盒时分两批：
1. **static 子盒**按文档序绘制（inline/inline-block/block 全部递归）；
2. **定位子盒**（relative/absolute/fixed）按 `z-index` 升序稳定排序后绘制（天然压在普通流之上）。

> 简化说明：尚未按 CSS 2.1 附录 E 实现完整层叠上下文（如 `z-index:auto` 的嵌套上下文、opacity/transform 触发新上下文），
> 对常见页面（弹窗、角标、悬浮条）行为正确。

### 10.7 margin 塌陷（v0.3 新增）

`layout_block` 维护游标与"上一个子盒底部 margin"：
- **相邻兄弟**：间距 = `max(prev.margin-bottom, next.margin-top)`（不再是相加）；
- **父首子**：父无 border/padding-top 时，子 top margin 与父 top margin 塌陷为一个 `max()`，子盒不再额外下移父内容起点；
- **父末子**：父无 border/padding-bottom 时同理，父盒对外返回的高度用 `max(last-child-bottom, parent-bottom)`。

负 margin 当前按 0 钳制（`side_px`），暂不支持负 margin 参与塌陷代数。

### 10.8 浮动与清除（v0.4 新增）

- `float: left/right` 盒块化为原子 inline-level 盒（复用 inline-block 的 shrink-to-fit 路径），
  由 `place_float` 放置：从当前流位置起（`clear` 要求时下移越过同侧/两侧浮动），
  反复下移越过与 [y, y+h) 相交的既有浮动直到 margin-box 放得下；放置后记录进容器的
  `HpeFloatBand`（margin-box 外缘 top/bottom/left/right）。
- **行盒避让**：`band_edges_inherited` 沿祖先链（同一 BFC，遇浮动祖先即止——浮动建立 BFC）
  合并各层浮动带在行 y 区间的左右边缘，只取**浮动边缘**并夹紧到本块 content 区
  （祖先 content 框与空带不构成约束，横向溢出页不受影响）。
- 块盒 border box 不避让浮动（规范行为）；块容器高度扩展到包住其内全部浮动（BFC 式简化，
  因此浮动不会伸出容器）。
- `clear: left/right/both`：浮动盒放置时下移越过相应侧既有浮动；普通块上的 clear 暂不支持。
- **顺手修复（存量）**：shrink-to-fit 的 pref 未计入水平 margin，重排时被二次扣除，
  导致带 margin 的 inline-block 内容被挤窄折行（`pref + mL + mR` 作为重排 avail 修正）。

### 10.9 Flex 布局（v0.6 新增，`layout_flex*`）

`display: flex` 的盒在 `layout_block` 里走独立分支：先算出内容宽度，再交给 `layout_flex`，
其返回值直接作为内容高度（容器自身 `height` 仍优先）。

- **收集**：直接子盒按文档序成为 flex 项；绝对/固定定位子盒跳过（稍后按包含块定位）；
  行内/文本子盒在 `build_element` 阶段被**块化**（CSS Flexbox §4「flex item blockification」），
  因此 `<span>` 之类也能成为候选，真正的文本行由块级流处理（**未知**：跨多个文本/行内子项的匿名 flex item 仍未建模）。
- **主轴尺寸**：显式 `width` 用原值加内边距边框；`auto` 用 `measure_preferred_w`（= 收缩到适应宽度，
  即「布局一次到极大宽度再取内容实际范围」），并以 `min-width` 兜底、以容器内容宽度封顶。
- **剩余空间**：`free = 容器内容宽 - Σ(margin box) - gap·(n-1)`；分配优先级
  ① `margin-left/right: auto`（等分，把剩余空间吃光——`#hud{margin-left:auto}` 靠这条右对齐）
  ② `flex-grow`（按比例分）③ `justify-content`（lead/间隔）。写回：auto margin 的**解算值会写回子盒
  `style.margin`**，因为子盒自己的 `layout_block` 只会把 `auto` 当 0，不写回会导致双重计算。
- **交叉轴**：先按主轴摆一遍拿到各项高度（`line_h = max`，容器有确定高度时用它），
  再按 `align-items`/`align-self` 逐项 `box_shift` 到中线/末端（`stretch` 退化为 `flex-start`，
  因为项高由内容决定；`baseline` 未实现）。
- **column**：主轴为纵轴，逐项累加高度，交叉轴用同样的偏移量算 `dx` 后**一次性传入** `layout_block`。
- 定位原点约定：`layout_block(ctx, box, x, y, avail, avail_h)` 的 `(x, y)` 是**margin box 左上角**，
  内部再加 margin；flex 传参必须遵守这一点（传 `cursor`，不是 `cursor + margin`）。
- `avail_height`（v0.6）：`layout_block` 新增形参，承载包含块高度，用于解算百分比 `height`；
  根盒传入视口高度（`ctx.vh` = 真实视口高），子盒传入父内容高度，未知时为 `-1`。
  绝对定位在 `inset` 同时给出 `top` 与 `bottom` 且高度 `auto` 时，按包含块拉伸（`forced_h`）。

### 10.10 已知布局缺口

grid、table 布局；flex 的 `flex-wrap` / `row-reverse` / `order` / `flex-basis` / `flex-shrink` /
`stretch` 拉伸与 `baseline`；元素级滚动（`overflow` 只裁剪不产生可滚动条）；`content`/`vertical-align`
的完整基线对齐（仅支持原子行内盒的 top/middle/bottom 等）；完整层叠上下文。

### 10.11 尺寸求解与间距（v0.9 新增）

- **统一长度解算 `resolve_len(ctx, st, len, base)`**：`LayoutCtx` 增加 `vw` / `vh` / `root_font_size`，
  `ch` 由 `hpe_font_advance(f,'0')` 求得，最终调用 `hpe_len_resolve_full`。
- **`margin_px` 与 `side_px`**：`margin_px` 允许负值，且 `auto` 返回 `0`（auto 由块级/flex 分配阶段
  显式处理）；`side_px` 用于 padding/border，恒非负。
- **`solve_content_width(ctx, st, cb_width, avail_width, p, b)`**：显式宽度按 `box-sizing` 换算
  （border-box 减去 padding+border），百分比相对 `cb_width`（包含块宽度，而非可用宽度），
  最后按 `min-width`/`max-width` 夹取。
- **`layout_block` 高度分支**：显式 `height` 支持 px/em/rem/vh/vw/ch/%；border-box 需减去
  padding+border；随后按 `min-height`/`max-height` 夹取。
- **间距**：`letter-spacing`/`word-spacing` 以逐字形推进方式加入 `flush_paragraph` 的行宽与位置；
  `text-indent` 只作用于段首行。`vertical-align` 在 `finalize_line` 里决定原子行内盒的纵向落点。
- **负 margin**：`collapse_margins(a, b)`（CSS 2.1 §8.3.1）—— 同为正取大、同为负取更负、异号相加；
  负 margin 会令盒向上一行/左侧溢出，绘制由裁剪矩形约束。

---

## 11. 绘制（`src/paint/hpe_paint.c`）

`hpe_paint_document(pm, root, scroll_y, page_background, origin_y)`：

- 以 `scroll_y` 偏移遍历盒树，**只绘制进入视口的部分**（按盒的 border_box 与裁剪矩形相交裁剪）。
- 递归绘制顺序：背景色 → 边框（1px）→ 文本行（经光栅覆盖率混合）→ 列表标记（`ul > li` 的项目符号）。
- 子盒分两批绘制：static 子盒按文档序（含 inline-block 原子盒），定位子盒按 `z-index` 升序（见 §10.6）。
- **fixed 盒不减 scroll**：布局坐标即视口坐标，绘制时保持不动（滚动时钉在视口原位）。
- 行内元素（非 inline-block）的背景/边框目前不绘制（列入绘制缺口）。
- **边框按边绘制**（v0.6）：上/下两边取整宽，左/右两边取 `bb.h - top - bottom`，因此
  `border-bottom: 1px` 只画底部一条线（页头分隔线就靠它）。v0.9 起**四边颜色独立**
  （`border-color` 1~4 值按 T R B L 展开；`border-<side>` 的颜色也按边设置）。
- **圆角**（v0.6）：`border-radius > 0` 时背景用 `hpe_fill_round_rect`、边框用
  `hpe_stroke_round_rect`（同心带 SDF，覆盖率为 `0.5 - |d + w/2| + w/2` 的等价式
  `0.5 - (|d + w/2| - w/2)`，即环带到像素中心的距离），边角因此是抗锯齿的；
  负值表示百分比半径，绘制期按 `min(bb.w, bb.h)` 解算。

**v0.9 新增绘制能力**（`paint_background` / `paint_box_shadow` / `paint_text_shadow` /
`paint_list_marker` / `paint_text_decorations` / `paint_group`）：

- **背景图像层**：`background-image: linear-gradient(...)` 或 `background` 简写中的渐变，
  按方向/角度逐像素插值填充盒（圆角时仍走圆角裁剪）。
- **盒阴影 / 文字阴影**：按 `[ox oy blur [spread]] color` 生成高斯近似衰减，绘制在盒/字形之下。
- **列表标记**：`list-style-type`（disc/circle/square/decimal/none）与 `list-style-position`
  （outside 悬挂于内容盒左侧、inside 随行内流）。标记字形以度量掩码绘制，保证与文本同风格。
- **文字装饰**：underline / overline / line-through（可叠加），按字形基线定位横线。
- **`visibility: hidden`**：保留占位但跳过绘制（与 `display: none` 语义区分）。
- **`overflow: hidden/auto/scroll`**：绘制时把子盒裁剪到 pad-box；元素级滚动条未实现。
- **`opacity` 与 `transform` 的组合（`paint_group`）**：当 `opacity < 1` 或 `transform` 非单位阵时，
  把子盒先绘制进临时 pixmap，再按 `sa*opacity` 混合回主图；`transform` 于盒左上角为原点构造
  2D 仿射矩阵，合成时对每个目标像素逆映射（`det = a*d - b*c`）取最近邻纹素采样。
  嵌套分组上限 `MAX_OPACITY_DEPTH = 3`。`paint_box` 的递归顺序为：盒阴影 → 背景 → 边框 →
  文本/装饰 → 列表标记 → 子盒（static 按文档序、定位按 z-index）。
- `origin_y`（v0.5）= 页面视口顶边在设备坐标中的 y（有地址栏时为 44，否则 0）：所有设备 y 统一加该偏移，
  并把绘制裁剪到 `[origin_y, pixmap 高度)`，使内容永不上溢到浏览器外壳。
- **设备 y 量化到 1/64 px**（`snap_y`，v0.5）：布局会累积小数高度（如 `178.4 + 319.6 = 497.99997`），
  这类近整数边缘经 `floor/ceil` 后是否多画一行，取决于浮点尾数是否被平移吸收。1/64 的倍数在 float 中精确可表示，
  加整数偏移（滚动量、地址栏高度）后仍精确，因此「平移后的旧像素」与「重新绘制的像素」总是取整一致
  —— 这是脏矩形与全量绘制逐字节相等的前提。

`hpe_paint_region(pm, root, scroll_y, bg, clip, origin_y)`（脏矩形重绘，v0.4 新增）：

- 把 pixmap 裁剪矩形设为 `clip`（并与 `[origin_y, height)` 求交），先以页面背景擦除该区域，再重放整棵盒树 —— 所有光栅操作被裁剪矩形约束，最终该区域像素与全量绘制**逐字节一致**。调用方可连续重绘多个区域（暴露条带 + 各 fixed 盒区域，见 §14）。
- 依赖 `hpe_fill_rect_i` 的裁剪**收缩**语义（v0.5 修复）：矩形跨越裁剪上/左边缘时，把边内移必须**减少**范围
  （原实现误为 `h += clip_y0 - y`，导致跨越条带顶边的边框被纵向拉长，脏矩形滚动时出现整条色带）。

### 11.1 SVG 绘制子集（v0.6 新增，v0.11 大幅扩展，`src/paint/hpe_svg.c`）

`<svg>` 在布局里是**替换块**：宽度/高度取元素属性（脚本设置），覆盖 CSS；其子树**不建盒**，
由 `hpe_svg_paint` 直接从 DOM 读属性绘制（与浏览器的「SVG 内容不参与 CSS 盒模型」一致）。

- **坐标变换（v0.11）**：整棵 SVG 用一个 2×3 仿射矩阵 `Mat{a,b,c,d,e,f}` 把用户空间映射到设备空间，
  对每个元素复合其 `transform`（支持 `translate` / `scale` / `rotate`(含绕点) / `skewX` / `skewY` /
  `matrix`，可串联多个）。`viewBox` 的 `preserveAspectRatio="xMidYMid meet"` 语义
  （`scale = min(vp_w/vw, vp_h/vh)` 再居中平移）作为该矩阵的初值。描边宽度按矩阵的
  `sqrt(|det|)` 缩放。所有几何先变换到设备空间再做光栅，因此变换对填充/描边一致生效。
- **图元（v0.11 新增）**：`<rect>`（含 `rx`/`ry` 圆角，四角圆弧采样）、`<circle>`、`<ellipse>`、
  `<line>`、`<polygon>`、`<polyline>`、`<path>`、`<text>`、`<g>`；`<defs>` 不直接绘制。
  任意图元统一编译为设备空间的**多边形子路径列表**（`Polys`），填充用 4× 垂直超采样 +
  even-odd 扫描线（水平解析覆盖率），描边用逐段胶囊（capsule）。
- **`<path>` 全命令（v0.11）**：`M L H V C S Q T A Z`，绝对/相对；三次/二次贝塞尔按曲率自适应展平，
  平滑的 `S`/`T` 由前一段控制点镜像得出，`A` 用端点参数化转中心参数化后按弧长展平。
- **`<defs>` / `<use>`（v0.11）**：`find_by_id` 在整个 SVG 子树内按 `id` 检索；`<use href="#id"
  x y>`（兼容 `xlink:href`）把被引用元素以平移后的坐标系重放，支持嵌套（深度上限 6）。
- **渐变（v0.11）**：`<linearGradient>` / `<radialGradient>` / `<stop>`；`fill="url(#id)"` 与
  `stroke="url(#id)"` 均可引用。`gradientUnits="objectBoundingBox"`（默认）按图元的用户空间包围盒
  映射，`userSpaceOnUse` 直接使用用户坐标。填充逐像素、描边逐像素地采样色标
  （复用 `hpe_sample_stops`），支持 `stop-opacity`。
- **`<text>`（v0.11）**：`x`/`y`/`font-size`/`font-weight`/`text-anchor`（start/middle/end）/
  `dominant-baseline`（middle/central/hanging/text-before-edge/text-after-edge）/`fill`；
  `textLength` + `lengthAdjust="spacing"` 通过逐字符推进把整段拉伸到指定宽度。
- 颜色：`#rgb`、`#rrggbb`、`#rrggbbaa`、具名色、`rgb()`/`rgba()`/`hsl()`；`fill-opacity`/
  `stroke-opacity` 与颜色 alpha 相乘。
- 未支持（缺口）：`mask`/`clipPath`、`<marker>`、SVG `filter`、`stop` 的 `gradientTransform`、
  `<tspan>` 行内子文本。

**命中测试（v0.11 扩展）**：`hpe_svg_hit_test(box, x, y)` 把文档坐标经**逆矩阵**变换回用户空间，
按绘制顺序**后绘制的先命中**地遍历 `rect`/`circle`/`ellipse`/`line`/`polygon`/`polyline`/`path`/`text`
（`path`/`polygon`/`polyline` 展平为多边形后做 even-odd 点包含测试；`line` 用点到线段距离阈值），
返回最深元素（`<g>` 透明，事件随后向上冒泡到 `<g>` 的监听器）。`transform` 链在命中遍历中被完整复现。

---

## 12. JS 引擎（`src/js/*`）

纯 C 手写解释器，无第三方依赖。执行时机：DOM 解析 + 样式收集完成后、首次布局之前（等价于 `defer` 脚本）。

### 12.1 前端：词法 + 语法

- 手写词法：数字、字符串、`\u` 转义、注释；`=>` 是**独立词法单元**（箭头函数）。
- 递归下降解析，AST 节点 `JsNode { kind, op, num, str, kids[], line }` 全部存入 arena。
- 自动分号插入（ASI）在解析期处理常见换行边界。

### 12.2 值模型（`hpe_js.h`）

```c
typedef enum JsType { JS_UNDEF, JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_OBJ } JsType;
typedef struct JsValue { JsType t; double num; char *str; JsObj *obj; } JsValue;
```

`JsObj` 的 kind：`PLAIN / ARRAY / FUNC / NATIVE / HOST / PROMISE / REGEX / MODULE /
ERROR / MAP / ARGUMENTS / DATE`（`Set` 复用 `MAP`，以 `class_name == "Set"` 区分；引擎没有
独立的 `SET` 种类，避免为一个内建再加一条分支）。
- `FUNC`：持有 `code`(AST)、`closure`(JsEnv) 与 `fn_flags`（`FN_ARROW / FN_ASYNC /
  FN_GEN / FN_STRICT / FN_CTOR / FN_BOUND / FN_CLASS`），因此**支持闭包、生成器、
  async 与绑定函数**。
- `NATIVE`：C 实现的原生函数（`JsNativeFn`）。
- `HOST`：宿主对象（如 DOM 节点），通过 `HpeJsHostVt` 拦截属性 `get/set`（核心解释器不知道 DOM）。
- `DATE`：`num_val` 存自 Unix 纪元的毫秒数（`NaN` = 无效日期），数值双重用作
  `seal/freeze` 的标记位（`0` 可变 / `3` 冻结）。
- **原型链**：`JsObj.proto`；`js_obj_find_prop` 沿链查找（含访问器 `JsProp { getter, setter }`）。
  `class X extends Y` 同时建立 `X.prototype.__proto__ = Y.prototype` 与 `X.__proto__ = Y`（静态继承）。
- **`this` 绑定**：`JsEnv.has_this / thisv`（箭头函数不建自己的 `this`，直接沿作用域链读外层），
  `JsEnv.has_home / home` 保存 `[[HomeObject]]` 供 `super` 解析。
- **成员访问优先级**：`p_member_tail(base, allow_call)` 统一处理 `.` / `?.` / `[` / `(`；
  `p_unary` 的 `new` 分支以 `allow_call=0` 解析被构造的 callee，随后再以 `allow_call=1`
  续接尾链——保证 `new Date(0).getTime()` 绑定为 `(new Date(0)).getTime()` 而非
  `new (Date(0).getTime())`。

### 12.3 运行时：作用域 / 错误 / 分配

- **作用域链**：`JsEnv { parent, vars[] }`，词法作用域；`js_env_lookup` 沿链查找，`js_env_define` 声明，
  `js_env_declare` 在当前作用域新建绑定（`let/const/class` 的遮蔽语义）。
- **异常**：`throw` 走 `js_throw_value(js, v)`；`try` 压入 `JsTryFrame`（`HpeJs.try_top` 链），
  `js_throw_value` 总是 `longjmp` 到**最内层活动的 try 帧**，没有 try 帧时才落到 `js->jmp`
  （→ `hpe_js_eval` 的 errbuf）。`finally` 无论正常/`return`/`break`/异常都执行，
  且 `finally` 里的 `return` 覆盖之前的值。**语法/运行时错误都带行号**。
- **分配**：bump arena（§3.2），整页重载释放。

### 12.4 语言子集

语句：`var/let/const`、函数声明与表达式、`class`/`extends`/`super`（含 getter/setter、
静态成员、实例字段、私有 `#name`、静态初始化块）、`if/else`、`while`、`do/while`、
`for`、`for-in`、`for-of`（数组/字符串/Set/Map/arguments/NodeList，字符串按码点迭代）、
`switch`（含落空穿透）、`break/continue` + **标签**（`continue 外层标签` 由循环执行器
`exec_loop` 按标签名匹配消费，未命中的 Flow 继续向外传播）、`return`、`throw`、
`try/catch/finally`、`label`、以及 `import`/`export`（ES 模块）。

表达式：全部算术/比较/逻辑/位运算（`& | ^ ~ << >> >>>`）、`**`、三元、`typeof`、
`instanceof`、`in`、`delete`、`void`、可选链 `?.`、空值合并 `??` 与 `??=`、
复合赋值（含 `&&= ||= ??=`）、自增自减、逗号、成员访问与方法调用、`new`、
`this` 与 `call/apply/bind`、解构（数组/对象/嵌套/默认值/剩余）、展开 `...`
（调用参数、数组/对象字面量）、默认参数与剩余参数、模板字符串（含 `${}` 嵌套）、
正则字面量、`async`/`await`、生成器 `function*`。

**箭头函数**：`=>` 是独立词法单元；解析期把表达式体脱糖为 `{ return <expr>; }`，
复用 `JS_N_FUNC` 并以 `FN_ARROW` 标记；`this` 沿用外层作用域（不做自己的绑定）。
`function*` 与 `async function` 通过 `FN_GEN`/`FN_ASYNC` 标记，`async` 函数返回
Promise，`await` 同步 settle（§12.7）。

### 12.5 内置对象

- `console.log/info/warn/error/...`、`alert`
- `Math.*`（`PI E LN2 LN10 LOG2E LOG10E SQRT2 SQRT1_2`；`abs ceil floor round trunc sign
  min max pow hypot exp log log2 log10 log1p expm1 sqrt cbrt sin cos tan asin acos atan atan2
  sinh cosh tanh asinh acosh atanh random clz32 imul fround`；`random` 为
  以平台时钟播种的 xorshift64，确定性可复现）、`JSON.stringify/parse`、
  `parseInt/parseFloat/isNaN/isFinite`、`encodeURI(Component)/decodeURI(Component)`
- `Number`：`toFixed/toExponential/toPrecision/toString(radix)/toLocaleString/valueOf`；
  静态 `MAX_SAFE_INTEGER/MIN_SAFE_INTEGER/EPSILON/MAX_VALUE/MIN_VALUE/POSITIVE_INFINITY/NaN`、
  `isInteger/isFinite/isNaN/isSafeInteger`（`isNaN` 兼有全局函数形态）
- `Object.keys/values/entries/fromEntries/assign/freeze/seal/isFrozen/isSealed/
  create/defineProperty/getOwnPropertyNames/hasOwn/getPrototypeOf/setPrototypeOf/is`
- `Array.from/of/isArray`、`Function`（`new Function(params, body)` 走解析器编译）
- 数组：`push/pop/shift/unshift/slice/splice/concat/join/indexOf/lastIndexOf/includes/
  forEach/map/filter/some/every/find/findIndex/findLast/findLastIndex/reduce/reduceRight/
  reverse/sort/flat/flatMap/fill/copyWithin/at/keys/values/entries/toString`；
  `Array.prototype[Symbol.iterator]` 与 `keys/values/entries` 皆走统一迭代器协议
- 字符串：`charAt/charCodeAt/codePointAt/at/indexOf/lastIndexOf/slice/substring/substr/
  toUpperCase/toLowerCase/trim/trimStart/trimEnd/trimLeft/trimRight/padStart/padEnd/
  repeat/split/replace/replaceAll/match/matchAll/search/concat/includes/startsWith/
  endsWith/localeCompare/toString`；静态 `String.fromCharCode/fromCodePoint`；
  `String.prototype[Symbol.iterator]` 按码点迭代
- `Error / TypeError / RangeError / SyntaxError / ReferenceError`、`Symbol`、
  `Set`（`add/has/delete/clear/forEach/values/keys/entries/size`）、
  `Map`（`set/get/has/delete/clear/forEach/keys/values/entries/size`）、
  `Promise`（`then/catch/finally/resolve/reject/all/race`）、
  `RegExp`（`test/exec` + `g` 标志下的 `lastIndex`）
- **`Date`**（`src/js/hpe_js_date.c`）：实例为 `JS_OBJ_DATE`，`num_val` 存 Unix 毫秒
  （`NaN` = 无效日期）。构造支持 `new Date()`（当前时间）、毫秒数、日期字符串（ISO 8601 与
  若干常见格式）、另一个 `Date`、以及 `(y, m, d, h, mi, s, ms)` 多参形式（`0..99` 年份按
  1900 偏移，月/日等字段按 `civil_to_days` 自然溢出到相邻月）。日历换算用 Howard Hinnant
  的 `days_from_civil`/`civil_from_days`，与本地时区偏移（`getTimezoneOffset`）配合。
  原型提供全部 `get*/set*` 方法（`getUTC*`/`setUTC*` 通过 `def_alias` 复用本地时区槽位）、
  `toISOString/toJSON/toString/toDateString/toTimeString/toLocaleDateString/toLocaleTimeString/
  toLocaleString/valueOf`；静态 `Date.now/parse/UTC`。无效日期上 `toISOString` 抛 `RangeError`，
  其余格式化返回 `"Invalid Date"`；`JSON.stringify(date)` 经 `toJSON()` 输出 ISO 串。

`Map`/`Set`/`Array`/`String`/`arguments`/`NodeList` 等均可被 `for-of`、展开与解构消费——
统一经由 `js_iter_len`/`js_iter_at`（`Set` 输出自身值、`Map` 每条输出 `[k, v]`；
`Symbol.iterator` 以对象形式建模，属性键落在字符串 `"Symbol.iterator"` 槽位上）。

### 12.6 正则引擎（`src/js/hpe_js_regex.c`）

UTF-8 字节上的**回溯匹配器**，用 CPS（continuation-passing，`ReK` 链）实现序列拼接，
避免为量词单独造递归。支持：字符/字符类/范围、`.`、`* + ? {n} {n,} {n,m}`（含**惰性** `*?` 等）、
分组与捕获组、反向引用 `\1..\9`、前瞻 `(?=...)`/`(?!...)`、后顾 `(?<=...)`/`(?<!...)`、
锚点 `^ $`、词边界 `\b \B`、标志 `i / m / s`。捕获区间由 `hpe_regex_search` 写入
`captures[0..1]`（**整匹配**）与 `captures[2..]`（各组），`String.replace` 的 `$& $1..$9 $` $'`
与 `match/split/search` 都建在其上。有步数与深度上限（`RE_MAX_STEPS`/`RE_MAX_DEPTH`）防病态回溯。

### 12.7 Promise 与生成器

- **Promise**：同步 settle + 微任务队列（`JsJob`），`hpe_js_drain_jobs` 排空；
  `await` 在同步模型下直接取值（`async` 函数体仍返回 Promise）。
- **生成器**：**急切物化**——`yield` 把值写入 `js->yield_sink` 收集成数组，
  迭代器按数组顺序吐 `{ done, value }`。对「先跑完再遍历」的用例等价，
  代价是不支持在 `yield` 中途暂停以及无限生成器（列入缺口）。
- **ES 模块**：`hpe_js_eval_module(source, dir, ...)` + `HpeJsModuleLoader` 回调
  （引擎侧在 `hope.c` 里用 `hpe_path_*` 解析 `./x.js` 相对路径并读文件）；
  `JsModule` 记录 `evaluated` 状态（PENDING/RUNNING/DONE）以处理相互导入与循环引用，
  `export` 写入模块命名空间对象 `module->ns`。

---

## 13. DOM 绑定与事件（`src/js/hpe_js_dom.c`）

核心解释器不认识 DOM；DOM 桥通过 `HpeJsHostVt` 把宿主对象暴露成 JS 对象。

### 13.1 DOM API

- `document.getElementById / createElement / body`
- `document.querySelector(sel)`（返回首个匹配元素或 `null`）、`document.querySelectorAll(sel)`
  （返回数组式对象：数字键 + `length`；内部 DFS 收集元素后用 `hpe_css_selector_matches`
  逐个匹配——把选择器串解析成临时 `HpeRule`，逗号分隔的任一分支命中即算匹配）
- 元素：`textContent`（读写）、`innerHTML`（读写）、`style.xxx`（驼峰属性名 → CSS 属性名，写回 `style` 属性）、
  `setAttribute/getAttribute/appendChild/remove`
- **浏览器全局（v0.7 新增）**：`location`（`protocol/pathname/href/search/hash` + `reload/replace/assign` 空实现，
  `hash` 可写，供页面用 `#all` 之类的视图开关）、`navigator`（`userAgent/platform/language`）、
  `window`/`self`（`innerWidth/innerHeight/devicePixelRatio/scrollX/scrollY/pageYOffset`、
  `addEventListener/removeEventListener/scrollTo/scrollBy` 空实现、
  `getComputedStyle` 返回空对象、`requestAnimationFrame` 返回 0）。
  引擎无网络、无导航，这些对象只为让**普通页面脚本的兼容性判断与守卫代码**不抛
  `ReferenceError` 而提前中断（对齐基准样本 `copyResource/aim.html` 曾用到 `location.hash`；
  该样本目录当前不在工作区，见 §15）。

### 13.2 事件模型

- 注册：`el.onclick = fn`、`el.addEventListener(type, fn)`；监听器存于 `HpeDomNode.listeners`。
- 派发：`hpe_js_dom_dispatch` 沿 DOM 祖先**冒泡**，事件对象
  `{ type, clientX, clientY, target, currentTarget, preventDefault(), stopPropagation(), stopImmediatePropagation() }`。
  `stopPropagation` 在事件对象上置 `_stopped`，派发循环在每个监听器后检查它并提前返回（v0.6）。
- 支持类型：`mousedown` / `mouseup` / `click`（由平台鼠标按下/松开分别触发 `mousedown`→`mouseup`→`click`）。

### 13.3 派发链路与重排

```
Win32 WM_LBUTTONDOWN/UP
  → HpeWindowCallbacks.on_mouse
  → hpe_engine_mouse_event        (hope.c)
      → hit_test_box              (视口坐标 + scroll_y → 文档坐标，取最深匹配盒)
      → hpe_svg_hit_test          (命中盒是 <svg> 时，继续在 SVG 几何里找最深图形)
      → DOM 节点反查
      → hpe_js_dom_dispatch       (mousedown / mouseup / click，冒泡)
  → 若有监听器触发：relayout()    (事件内的 DOM/style 改动即时反映到下一帧)
```

> `hit_test_box` 返回**最深**包围盒；命中后上溯到最近的 ELEMENT 节点再派发。
> SVG 子树没有盒，所以 `<svg>` 盒命中后再用 `hpe_svg_hit_test` 细化到具体图形（如节点矩形），
> 事件从该图形冒泡，`<g class="n">` 上的 `click` 监听器就能收到 —— 思维导图节点折叠即依赖这条链路。

### 13.4 定时器与动画帧（v0.4 新增，`src/js/hpe_js_timers.c`）

- 全局函数：`setTimeout(fn, ms)` / `setInterval(fn, ms)` / `clearTimeout(id)` /
  `clearInterval(id)`（同一清除函数复用 id）与 `requestAnimationFrame(fn)`（近似 vsync，
  每轮 tick 执行一次）。返回值均为整数 id。
- 实现：`HpeJs` 持有 `JsTimer` 数组（`{id, repeating, is_raf, due, every, fn}`，arena 分配、
  容量倍增），到期判断基于 `hpe_time_seconds()` 的秒值。
- 泵：`hpe_js_run_timers(js, now, &fired)`（公开 API，见 `hpe_js.h`）——
  **rAF 回调每轮全部执行并在运行后移除**；到期定时器逆序执行，repeating 的 `due += every`
  （落后过多时夹紧到 `now + every`）。
- 引擎接入：`hpe_engine_tick()`（§14）每帧调用泵；`fired > 0` 则 `relayout`——
  定时器/rAF 回调里的 DOM 与 `style` 改动即时上屏。Win32 层 `SetTimer(hwnd, 1, 15)`
  驱动 `WM_TIMER → on_tick`（约 60fps 上限），窗口销毁时 `KillTimer`。

---

## 14. 引擎门面与生命周期（`src/hope.h/.c`）

```c
HpeEngine *hpe_engine_create(int w, int h);
int  hpe_engine_load_file(HpeEngine*, const char *path);
int  hpe_engine_load_blank(HpeEngine*);          /* 内置空白页（v0.5） */
int  hpe_engine_reload(HpeEngine*);
void hpe_engine_resize(HpeEngine*, int w, int h);
void hpe_engine_scroll_by(HpeEngine*, float lines);
int  hpe_engine_mouse_event(HpeEngine*, int x, int y, int button, int down);
void hpe_engine_render(HpeEngine*);
const HpePixmap *hpe_engine_pixmap(HpeEngine*);
/* 浏览器外壳（v0.5） */
void hpe_engine_enable_chrome(HpeEngine*);
int  hpe_engine_chrome_height(HpeEngine*);
const char *hpe_engine_address_text(HpeEngine*);
int  hpe_engine_address_focused(HpeEngine*);
int  hpe_engine_key(HpeEngine*, int key);        /* 可打印码点 / HPE_KEY_* */
```

**`hpe_engine_load_file` 顺序（核心流水线）：**
1. 读文件 → `hpe_html_parse` 生成 DOM；
2. `collect_styles` 收集 `<style>` 文本与 `<link>` 本地 CSS（UA 样式表在 `hpe_css_init` 已就位）；
3. `run_scripts`：重建 JS 环境，安装 DOM 桥，按文档序执行 `<script>`（内联 + 本地 `src`）；
4. `relayout`：`hpe_layout_build` 建盒树并布局；
5. 开发钩子：`HOPE_DEBUG=1` 打印 DOM 树与盒树坐标；`HOPE_TEST_CLICK=id` 触发对指定 `id` 元素的合成点击（无头验证事件驱动的 DOM 更新）；
   `HOPE_TEST_HOVER=x,y` 无头模拟指针；`HOPE_TEST_WAIT=秒` 忙等并逐轮 `hpe_engine_tick` 泵定时器（验证 setTimeout/interval/rAF）；
   `HOPE_TEST_TEXT=id` 打印该元素 textContent（断言脚本写入的 DOM 内容）。

**`hpe_engine_render` 与脏矩形滚动（v0.4 新增）**：引擎维护 `dirty` 标志
（`NONE=0` 干净 / `SCROLL=1` 仅滚动偏移变化 / `FULL=2` 需全量重绘；`relayout` 置 FULL，
`scroll_by` 在干净时置 SCROLL）。`render` 按标志分路：

- **FULL/干净**：`hpe_paint_document` 全量重绘，记录 `painted_scroll = scroll_y`。
- **SCROLL**：`render_scrolled` ——
  1. `dyi = round(scroll_y - painted_scroll)`（亚像素残差并入下次），`|dyi| ≥ 视口高` 则退化为全量；
  2. `hpe_pixmap_shift_v` 整体平移已绘像素（memmove 行）；
  3. 重绘暴露条带（`hpe_paint_region`，dyi>0 为底部、<0 为顶部）；
  4. 递归收集全部 `position:fixed` 盒，逐个以其视口 bbox 为 clip 重绘区域 —— fixed 盒不随内容平移，
     区域重绘会按绘制顺序重放其背后的全部文档内容，结果与全量绘制逐字节一致。
  `HPE_NO_DIRTY=1` 可旁路脏路径（基准对比用）。验证：8 示例 × 3 滚动量脏路径与全量输出逐字节一致。
**`resize`**：`hpe_pixmap_resize` + `relayout`。**`scroll_by`**：按 `HPE_SCROLL_LINE_PX`(48px) 滚动并夹紧到 `[0, content_height - height]`。
**`mouse_event`**：返回 1 表示页面发生变更（窗口应重绘）；见 §13.3。
**`mouse_move`（v0.4 新增）**：命中测试（fixed 盒按视口坐标比较）→ 更新 `hover_node` 与
`HpeDomNode.hovered` → 派发 `mousemove`（冒泡）→ hover 变化或有监听器改动 DOM 时 `relayout`。
平台层 `WM_MOUSEMOVE` 以 `button=-1` 进入同一 `on_mouse` 回调，main.c 分流到 `mouse_move`。
开发钩子 `HOPE_TEST_HOVER=x,y`：无头模拟指针位置，验证 `:hover` 与 mousemove 监听器。
**`hpe_engine_tick`（v0.4 新增）**：调用 `hpe_js_run_timers` 泵定时器/rAF；有回调触发则
`relayout` + `update_status` 并返回 fired（窗口层据此 invalidate 重绘）。Win32 以 15ms
`SetTimer` 驱动，无头模式由 `HOPE_TEST_WAIT` 驱动。

### 14.1 浏览器外壳：内置地址栏（v0.5 新增，`src/hope_chrome.c`）

外壳**由引擎自己绘制**（不使用 Win32 EDIT 子控件）：跨平台一致、可被 `--dump` 无头验证、
与像素管线共用字体/光栅/裁剪。引擎结构体与内部接口移入 `src/hope_internal.h`，供
`hope.c` 与 `hope_chrome.c` 共享。

- **几何**：`chrome_h = 44`；页面视口 = 窗口减去外壳，`hpe_engine_view_height()` 统一入口
  （滚动上限、fixed 视口、命中测试都用它）。外壳宽度跟随窗口。
- **绘制**（`hpe_chrome_paint`）：条带底色 `#e8ebef` + 1px 分隔线；输入框高 28、留白 10、圆角 8px，
  聚焦时描 2px 蓝 `#1a73e8` 焦点环、失焦 1px 灰 `#c6cdd6`；空文本时显示灰色占位提示；
  失焦时按浏览器习惯**只保留尾部**（水平偏移），聚焦时以插入符可见为准。
  选区为蓝底白字（先填圆角高亮带，再分三段绘制文本：前段常规色、选中段白色、后段常规色），
  有选区时不画插入符 —— 与 Chrome 的 omnibox 一致。插入符不闪烁（保证无头 dump 逐字节稳定，
  见 §16）。文本、高亮、插入符都裁剪在输入框内。
- **输入**：平台 `on_char(codepoint)`（Win32 `WM_CHAR`，跳过控制字符与 UTF-16 代理项）；
  `on_key(vk, mods)` 与 `on_mouse(x, y, button, down, mods)` 都带修饰键掩码
  （Win32 侧 `GetKeyState`），main.c 把 `VK_BACK/VK_DELETE/VK_LEFT/RIGHT/HOME/END/VK_RETURN/VK_ESC`
  与 `Ctrl+A/C/X/V` 映射为 `HPE_KEY_*` 先交给外壳，未被消费时才执行原有 F5 重载 / Esc 退出。
- **文本编辑模型（v0.6，对齐 Chrome omnibox）**：外壳保存 `addr_caret`（插入点，字节下标）与
  `addr_anchor`（选区锚点），二者不等即「有选区」。所有编辑都先作用于选区：
  - **鼠标**：点击未聚焦的输入框 → 聚焦并**全选**（Chrome 行为）；已聚焦时点击 → 落插入符；
    按下后移动超过 3px 才算拖选（锚点取按下点，因此「全选」状态下拖选会退化成普通区间选择）；
    双击选词（词 = 字母数字/下划线/非 ASCII 码点，点在分隔符上则选分隔符串）、三击全选
    （`CLICK_SECONDS = 0.45s`、`CLICK_SLOP = 4px` 判定连击）；Shift+点击移动选区活动端；
    按下中指针移出输入框仍继续扩展（平台 `SetCapture`），插入符被钳制在可视区内 → 自然产生自动横向滚动。
  - **键盘**：←/→ 有选区时先折叠到一端，否则移动一个**码点**；Ctrl+←/→ 按词移动；Home/End 两端；
    Shift+任一移动键 = 扩展选区；Backspace/Delete 先删选区，否则删邻接码点（Ctrl+Backspace /
    Ctrl+Delete 删一个词）；
    输入任意字符或 Ctrl+V 都用新文本**替换选区**；Ctrl+A 全选、Ctrl+C 复制（无选区时复制全文，
    与 Chrome 相同）、Ctrl+X 剪切（无选区时不做任何事）。
  - **焦点**：点击条带空白处或页面即失焦，失焦后不再绘制高亮；`hpe_chrome_set_text` 会把插入符与锚点
    一并收到文末并刷新 `addr_committed`（Esc 的还原基准）。
  - **Esc**：还原为「聚焦前/上次成功导航」的地址（`addr_committed`），然后失焦。
- **UTF-8 安全**：插入点/选区边界始终落在码点边界上（`addr_prev_index` / `addr_next_index` 走
  `hpe_utf8_prev` / `hpe_utf8_next`），水平位置用逐码点 advance 累加（`addr_width_to`），
  与 `hpe_font_measure` 对任意前缀一致；因此选择/删除中文路径不会切碎字节。
- **粘贴（Ctrl+V，v0.5）**：`hpe_clipboard_get_text()` 取 UTF-8（Win32 侧优先 `CF_UNICODETEXT`，
  未命中则回落 `CF_HDROP` —— 资源管理器里复制的文件直接变成其路径）。文本经 `sanitize_paste`
  去掉 `\r\n\t` 等控制字符并修剪首尾空格（多行复制不会把换行带进路径），按 `HPE_ADDR_MAX`
  截断且**不切断 UTF-8 序列**；剪贴板为空或清理后为空时返回 0（不标脏、不重绘）。
- **导航**（`hpe_chrome_navigate`）：去首尾空白与引号、剥离 `file://` → `hpe_path_absolute`
  （平台层 `GetFullPathNameA`）→ `hpe_engine_load_file`。成功则回填绝对路径；失败保留用户输入，
  且错误信息保存在引擎的 sticky `error` 字段里（状态行追加 `ERR: ...`，重绘不会丢失）；空输入重载内置空白页。
- **内置空白页**：常量 `BLANK_PAGE`（近空白 + 一行浅灰提示），走与文件完全相同的加载流水线
  （`path = NULL`，相对资源以 `.` 解析）。无参数启动、或在地址栏回车空串时显示。
- **脏标记**：新增 `HPE_DIRTY_CHROME`（只重绘外壳）；`painted` 标志确保首帧未绘制时不走局部重绘。
  外壳文本/焦点变化只标 CHROME，不触发重排。
- **滚动**：`hpe_pixmap_shift_v_rect(dy, chrome_h, height)` 只平移页面区域，外壳像素不动；
  fixed 盒区域的重绘矩形按 `origin_y` 平移。滚动偏移**对齐到整像素**，保证脏路径与全量一致。
- **鼠标坐标**：窗口坐标统一减 `chrome_h` 后再做命中测试与 JS 事件派发（`clientY` 即页面坐标）。

开发钩子：`HOPE_TEST_ADDR=<路径>`（先点击条带聚焦，再逐**码点**键入；配合 `HOPE_TEST_ADDR_ENTER=1` 回车）
可无头验证「输入路径 → 加载」的完整链路；`HOPE_TEST_CLIPBOARD=<文本>`（写入系统剪贴板）+
`HOPE_TEST_PASTE=1`（聚焦后按 `HPE_KEY_PASTE`，可加 `HOPE_TEST_PASTE_ENTER=1`）验证粘贴链路；
`HOPE_TEST_ADDR_TEXT=1` 打印文本/选区/插入符/焦点状态；`HPE_TRACE_CLIPBOARD=1` 打印剪贴板格式探测过程。
文本编辑钩子（v0.6）：`HOPE_TEST_ADDR_CLICK="x[,mods][;x[,mods]...]"`（合成点击，mods 2 = Shift）、
`HOPE_TEST_ADDR_DBLCLICK=x[,x2]`（两次点击）、`HOPE_TEST_ADDR_DRAG=x1,x2[,y]`（按下→多次移动→松开）、
`HOPE_TEST_TYPE="文本"`（不点击、直接键入，用于验证「输入替换选区」）、
`HOPE_TEST_KEYS="LEFT;SHIFT_RIGHT;CTRL_C;BACKSPACE"`（具名按键 + 修饰键）。

---

## 15. 构建与运行

```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
./build/bin/hope.exe examples/hello.html
```

- 参数：`[file.html] [--width N] [--height N] [--dump out.bmp] [--bench N] [--scroll-lines N] [--chrome]`
- `--js script.js` / `--module module.js`：**无窗口**执行一段脚本（后者按 ES 模块解析，
  支持 `import` 相对路径），用 `console.log` 打印结果，出错返回非 0 退出码。
  用于在没有页面的情况下验证语言特性与模块加载。
- **无文件参数**时显示内置空白页（v0.5）；窗口模式（非 `--dump`）总是带地址栏，顶部条带内即为可输入的本地路径。
- 交互：滚轮滚动、F5 重新加载、Esc 退出；地址栏聚焦时 Enter 加载路径、Esc 还原并失焦、
  Backspace/Delete 删字符或选区、方向键/Home/End 移动、Shift+移动扩展选区、`Ctrl+A/C/X/V`
  全选/复制/剪切/粘贴（复制文件则粘贴其路径）。鼠标可点击定位、拖选、双击选词、三击全选、Shift+点击扩展。
- `--dump out.bmp`：无窗口渲染为 BMP，用于无头验证渲染结果（配合 `HOPE_DEBUG` / `HOPE_TEST_*` 验证 DOM、布局、事件、定时器、地址栏）。`--dump` 默认**不带**地址栏（保持像素基线稳定），加 `--chrome` 可包含。
- `--scroll-lines N`：先渲染一帧建立完整画面，再滚动 N 行并二次渲染 —— 因此真正走**脏矩形**增量路径（配合 `HPE_NO_DIRTY=1` 对比全量输出）。
- `--bench N`：热身后重复渲染 N 次，报告平均 paint 毫秒（配合 `HPE_NO_MASK_CACHE` / `HPE_NO_DIRTY` 做对比基准）。
- 交互自测钩子（v0.6）：`HOPE_TEST_CLICK_AT="x,y[,btn][;x,y...]"` 在首帧前合成点击（驱动 SVG 节点折叠、
  页头按钮等交互路径）；`HOPE_TRACE_SVG=1` 打印 `<svg>` 视口尺寸与 `viewBox`；
  `HOPE_TRACE_PAINT=1` 打印每个盒的几何与背景色。
- 也可使用 MSVC 工具链；入口见 `src/main.c`。

### 示例页面（`examples/`）
- `hello.html`：浅色卡片页
- `boxes.html`：盒模型 / 对齐 / 滚动
- `layout.html`：布局演示
- `js.html`：DOM 操作
- `js-features.html`：JS 语言特性
- `js-arrow.html`：箭头函数
- `event.html`：事件（onclick / addEventListener / 冒泡 / 样式切换）
- `css.html`：选择器组合符 / 属性选择器 / white-space / font-family
- `css-props.html`：**样式属性综合页**（v0.9）——四色/按边边框、min/max 尺寸、`box-sizing`、
  `overflow: hidden`、`opacity`（含嵌套）、`visibility`、文字装饰、线性渐变（横/纵/45°）、
  盒/文字阴影、`transform`（translate/scale/rotate）、相对单位（em/rem/vw/ch）、
  letter/word-spacing、text-indent、vertical-align、负 margin、list-style、rgba/hsl/具名/transparent
- `css-cascade.html`：**层叠与继承综合页**（v0.10）——specificity+来源顺序、行内 normal/important、
  `!important` 四类优先级（样式表 important 胜行内 normal、行内 important 胜样式表 important、
  同特异性后者 important 胜、行内 normal 胜样式表 normal）、`@media`（min/max-width 命中/不命中、
  `screen and (...)`、`print` 排除、逗号「或」、`not print`、嵌套）、继承属性
  （letter/word-spacing、color、text-align、text-indent 首行）、`visibility: hidden` 子树内
  `visible` 重现、list-style 继承
- `hover.html`：`:hover` 伪类 + mousemove 监听器
- `float.html`：float 左右浮动、文字绕排、clear 清除
- `svg-shapes.html`：**SVG 绘制子集综合页**（v0.11）——新图元（ellipse/polygon/polyline/line）、
  `transform`（translate/rotate/scale/skewX/matrix）、渐变（linear userSpace + objectBoundingBox、
  radial，作为填充与描边）、`path` 全命令（C/S/Q/T/A/Z）、`defs`/`use`（含 `<g>` 属性继承）、
  `text` 的 anchor/dominant-baseline/textLength
- `timers.html`：querySelector(All) + setTimeout / setInterval / rAF
- `js-modern.html`：**JS 语言特性综合断言页**（v0.7）——33 项 `check(label, got, want)` 覆盖
  `try/catch/finally`+`throw`+`Error`、`new`/原型链/`class`·`extends`·`instanceof`、
  `this` 绑定与 `call/apply/bind`、解构/`...` 展开/默认与剩余参数/模板字符串、
  `for-in`·`for-of`/`switch`/`label`（含 `break 标签` 与 `continue 标签`）/位运算、
  `?.`/`??`、getter/setter、正则字面量、`Promise`/`async await`、生成器；
  结果写入 `document.title` 与 `#out` 首行（`PASS n/n`），并被 `run_tests.sh` 作为回归项。

**回归入口**：`docs/run_tests.sh`（脚本位于 `docs/`，内部自行 `cd` 到项目根后再操作
`build/`、`examples/`）给出统一自检——
① 逐字节像素基线（`--width 400 --height 600 --dump`，与 `build/baseline/*.bmp` 比对，`--rebase` 刷新；
含 `css-cascade`、`css-props`、`svg-shapes` 等 14 个页面）；
② `HOPE_TEST_TEXT=out hope examples/js-modern.html` 断言 `PASS 33/33`、`js-builtins.html` 断言 `PASS 37/37`；
③ headless ES 模块（`--module`）校验 `import`/`export`/默认导出与相对路径解析。

---

## 16. 关键设计决策与权衡

| 决策 | 理由 |
| --- | --- |
| 自研软件光栅，不用 Skia/Direct2D | 零依赖、完全可控；像素格式与 GDI 直接互通 |
| 抽象平台层 + `hpe_platform.h` | 系统相关代码隔离在 `platform/<os>/`，便于无头测试与跨平台 |
| 像素 `0xAARRGGBB` top-down | 与 Win32 `StretchDIBits` 零转换上屏 |
| GDI 字形覆盖率掩码 | 复用操作系统字体渲染，免自带字体文件；掩码按文本内容哈希缓存（v0.4，§9.1） |
| JS bump arena 整块释放 | 分配快、零碎片；代价：运行期无单对象回收（待 GC） |
| `HpeStyle.set` 位掩码 | 区分“未声明”与“声明为默认值”，保证层叠/继承正确 |
| 层叠分两趟（normal 后 important） | `!important` 语义要求 important 胜全部 normal（与 specificity 无关），单趟按 specificity 排序无法表达；两趟各自排序最简洁且与规范一致（v0.10） |
| `@media` 在层叠期求值而非解析期丢弃 | `viewport` 在重排时变化，样式表须保留全部规则、按 `HpeStylesheet.vw/vh` 逐次求值；解析期只记录条件不裁剪（v0.10） |
| 解释器 + 宿主 vtable | 核心 JS 不耦合 DOM；DOM 桥通过 `HpeJsHostVt` 注入 |
| 事件后整页 `relayout` | 简单正确；后续可演进为脏矩形/增量布局 |
| 地址栏由引擎自绘，不用 OS 控件 | 跨平台一致、无头可验证、与页面共用字体/光栅/裁剪；代价：需自己实现文本编辑与光标 |
| 外壳用 `origin_y` 平移页面而非改造布局 | 布局仍以页面视口为坐标系，绘制层单点加偏移，命中测试单点减偏移，改动面最小 |
| 设备 y 量化 1/64 px | 让绘制对整数平移严格不变，脏矩形与全量输出逐字节相等（否则近整数边缘会差 1 行） |
| SVG 子树不建盒，直接从 DOM 绘制 | 与浏览器一致（SVG 内容不进 CSS 盒模型），布局层零改动；代价：命中测试要在 SVG 模块里另做一遍几何 |
| 地址栏是自绘文本编辑器，不是 OS 子控件 | 可无头逐字节验证、跨平台一致；代价：选区/插入符/快捷键都要自己实现（约 400 行），且拿不到 IME |
| 插入符不闪烁 | 闪烁会让 `--dump` 的像素基线依赖时间相位。宁可牺牲这一点视觉真实度，保持回归可比 |
| 局部重绘的裁剪必须**相交**而不能覆盖 | `hpe_pixmap_set_clip` 是替换语义，`<svg>` 视口裁剪若直接调用就会丢掉脏矩形，半透明描边被二次混合（滚动时连线逐次变深）。新增 `hpe_pixmap_intersect_clip` |
| 以 Chrome 的渲染结果为对齐基准 | `copyResource/aim.html`（思维导图）这类真实页面会同时压到 JS 语法、flex、SVG、圆角、按边边框；逐区域比对能暴露「能跑但不对」的问题 |

---

## 17. 已知缺口与路线图（待补）

1. **JS**（v0.7 已补齐绝大多数语言特性：原型链/`class`/`extends`/`instanceof`、`try/catch/finally`、
   `this` 与 `call/apply/bind`、解构/展开/默认与剩余参数/模板字符串、`for-in`·`for-of`/`switch`/标签循环、
   位运算/`?.`/`??`、getter·setter、正则、`Promise`/`async await`、生成器、ES 模块；
   v0.8 补齐内置库：`Date`、`Map`/`Set` 迭代协议、Array/String/Number/Math/Object 全量方法）：
   仍缺 `getBoundingClientRect`；`tagName` 返回小写（规范大写）；脚本新增 `<style>` 不生效（未增量刷新样式）；
   生成器为**急切物化**（不支持 `yield` 中途暂停与无限生成器）；`await` 为同步 settle（无真正的事件循环挂起）；
   无 `Proxy`/`Reflect`/`Intl`/`WeakMap`·`WeakSet`/`Symbol.iterator` 之外的知名符号；
   `Date` 仅支持固定偏移而无历史时区库/夏令时规则，`Intl` 相关格式化未实现。
2. **布局**：grid、table；flex 的 wrap/shrink/order/stretch；元素级滚动容器
   （`overflow:auto` 目前不产生内部滚动条，内容把页面撑高）；
   完整层叠上下文（`z-index:auto` 嵌套、opacity/transform 触发）。
3. **绘制**：`<img>`/图片解码（PNG/JPEG/GIF）、滚动条 UI（页面级 + 元素级）、页面内文本选中与复制、
   插入符闪烁 / I 形光标、`background-position`/`background-size` 的完整应用
   （当前渐变铺满整个盒）、`transition`/`animation` 的插值时钟（当前仅解析存储为标志）、
   子像素文字定位、彩色 emoji / 字形回退链。
   （**v0.11 已补齐 SVG**：`transform`（translate/scale/rotate/skewX/skewY/matrix）、
   `linearGradient`/`radialGradient`（填充与描边、objectBoundingBox 与 userSpaceOnUse）、
   `<polygon>`/`<polyline>`/`<ellipse>`/`<line>`、`<defs>`/`<use>`、`path` 的 `S/Q/T/A`、
   `path`/`polygon`/`polyline`/`ellipse`/`line` 的命中测试、`textLength`/`dominant-baseline`。
   剩余 SVG 缺口：`mask`/`clipPath`/`<marker>`/`filter`/`gradientTransform`/`<tspan>`。）
4. **样式**：`:first-child`/`:nth-child` 等结构伪类、伪元素（`::before`/`::after`）、
   `:focus`/`:active` 等状态伪类、渐变的多层/多色标高级语法、`transform` 的 `skew`/`matrix`/3D、
   `cursor` 与平台光标的联动、`transition`/`animation` 插值时钟。
   （v0.9 已补齐：`em/rem/vh/vw/ch` 精确解析、`box-sizing`、`min/max-*`、`overflow` 裁剪、
   `opacity`、`visibility`、`text-decoration`、线性渐变、盒/文字阴影、2D `transform`、
   letter/word-spacing、`text-indent`、`vertical-align`、负 margin、`list-style-*`、
   四边独立 `border-color`、`rgba()/hsl()/transparent`/完整具名色表。
   **v0.10 已补齐：`!important` 参与层叠（两趟：normal 后 important）、`@media` 条件求值
   （媒体类型/`and`/`not`/`only`/逗号或列表/区间特性/嵌套，按视口求值）、继承属性集合
   （`visibility`/`cursor`/`list-style-*`/`letter-spacing`/`word-spacing`/`text-indent`）、
   `visibility: hidden` 子树内 `visible` 重现。**）
5. **基础设施**：DOM/CSS 增量更新、JS GC、事件内增量布局（当前事件/定时器后整页 relayout）。
   （字体掩码缓存、脏矩形滚动重绘已于 v0.4 完成。）
6. **浏览器外壳**（v0.6 后剩余）：地址栏已是完整文本编辑器（选区/双击选词/三击全选/Shift 扩展/
   `Ctrl+A/C/X/V`/方向键·Home·End·Ctrl+词移动/粘贴文件路径），仍缺**输入法组合输入**、
   插入符闪烁、鼠标 I 形光标、前进/后退历史与刷新按钮；滚轮在条带内仍会滚动页面
   （滚轮事件不带坐标）；**页面内文本仍不能选中复制**——选区模型目前只存在于地址栏。

---

*文档与代码同步维护；每次里程碑推进后更新本文件对应章节与 `AGENTS.md`。*
