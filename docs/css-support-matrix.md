# Hope UI Engine — CSS 支持矩阵

> 引擎版本基线：**v0.27**（`docs/需求.md` 最后更新 2026-11-27）
> 本表按 `docs/需求.md` §4/§5/§6 各条目整理，并抽样回源码核对（`src/css/hpe_css.c`、`src/layout/hpe_layout.c`）。
> 状态图例：✅ 支持 ｜ 🟡 部分支持（缺口见说明）｜ ❌ 不支持

**统计：64 支持 / 7 部分支持 / 10 不支持，共 81 条**

---

## 1. 选择器

| 特性 | 取值 / 范围 | 状态 | 说明 |
|---|---|---|---|
| 基础选择器 | 类型 / 类 / id / 通配 `*` | ✅ | — |
| 组合符 | 后代 ` `、子代 `>`、相邻 `+`、通用兄弟 `~` | ✅ | — |
| 属性选择器 | `[a]` `[a=v]` `[a~=v]` `[a\|=v]` `[a^=v]` `[a$=v]` `[a*=v]` | ✅ | — |
| 状态伪类 | `:hover` / `:focus` / `:active` / `:visited` | ✅ | — |
| 结构伪类 | `:first-child` / `:last-child` / `:nth-child()` / `:not()` | ✅ | — |
| 逻辑伪类 | `:is()` / `:where()`（含优先级细化） | ✅ | — |
| 伪元素 | `::before` / `::after` | ✅ | — |
| 其余伪类 / 伪元素 | `:checked` / `:disabled` 已支持；`:nth-of-type`、`::selection` 等 | 🟡 | 状态伪类 `:checked` / `:disabled` 已用于表单控件样式（v0.23）；其余仍未覆盖 |

## 2. 层叠与继承

| 特性 | 取值 / 范围 | 状态 | 说明 |
|---|---|---|---|
| specificity + 作者顺序层叠 | — | ✅ | — |
| 行内样式 | `style` 属性 | ✅ | — |
| UA 样式表 | 引擎内置默认样式 | ✅ | — |
| 自定义属性 `--x` / `var()` | 继承、回退、嵌套、`!important` | 🟡 | v0.24：层叠后展开，可用于任意属性/简写；`calc()` 组合与 `syntax` 类型校验未做，单元素上限 8 项 |
| `@property` | `syntax` / `inherits` / `initial-value` | 🟡 | v0.24：注册与初始值注入已支持，`syntax` 仅记录不求值 |
| 继承属性集合 | `color` / `font-size` / `font-weight` / `text-align` / `line-height` / `white-space` / `font-family` / `visibility` / `cursor` / `list-style-type` / `list-style-position` / `letter-spacing` / `word-spacing` / `text-indent` | ✅ | — |
| `!important` | 两趟层叠（normal 先、important 后） | ✅ | — |
| `@media` | 媒体类型 / `and` / `not` / `only` / 逗号列表 / 区间特性 / 嵌套 | ✅ | — |
| `@supports` | `(prop: value)` / `not` / `and` / `or` / 嵌套括号 | ✅ | — |
| `@font-face` | 自定义字体 | ❌ | 需求清单与源码均未覆盖 |
| UA 默认样式：`<details>` / `<summary>` / `<dialog>` | `details{display:block}`、`summary{display:list-item}`、`dialog[open]{display:block}` | 🟡 | v0.26：closed details 只呈现 summary、summary disclosure 三角、dialog 模态 `::backdrop` 遮罩逐像素绘制；`::backdrop` 选择器样式、dialog 自动居中未做 |
| 替换元素 `<canvas>` | width/height 属性定固有尺寸、作者 CSS 尺寸优先 | 🟡 | v0.27：按替换盒排版 + 离屏 2D 绘制混合进内容盒；fillText/drawImage/渐变未做 |

## 3. 盒模型

| 特性 | 取值 / 范围 | 状态 | 说明 |
|---|---|---|---|
| `display` | `inline` / `inline-block` / `block` / `flex` / `none` / `grid` / `inline-grid` / `table` / `inline-table` / `inline-flex` | ✅ | flex 子项按规范块化 |
| `display` 其余值 | `run-in` / `contents` / `flow-root` | ❌ | `flow-root` 见需求 §5 #153 未覆盖说明 |
| `width` / `height` | px / `%` / `auto` | ✅ | `%` 高度亦生效（v0.6 起） |
| `min-width` / `max-width` / `min-height` / `max-height` | 长度 / `%` | ✅ | 显式尺寸求解后夹取 |
| `margin` | 1~4 值简写、`auto` 居中、负值 | ✅ | 负 margin 含 CSS §8.3.1 塌陷 |
| `padding` | 1~4 值简写 | ✅ | — |
| `border-style` | `solid` / `dashed` / `dotted` / `double` | 🟡 | 源码 `hpe_css.c:1226` 仅识别这四种线型，`groove` / `ridge` / `inset` / `outset` 等未支持 |
| `border` / `border-width` / `border-color` | 四边同值 | ✅ | — |
| `border-top/right/bottom/left` | 按边独立宽度与颜色 | ✅ | 颜色 1~4 值按 T R B L 展开 |
| `border-radius` | px / %，抗锯齿圆角 | ✅ | 缺口：行内碎片圆角按直角绘制（需求 §6 #183） |
| `box-sizing` | `content-box` / `border-box` | ✅ | — |
| `overflow` | `visible` / `hidden` / `auto` / `scroll` | ✅ | `scroll`/`auto` 建立元素级滚动容器 |
| `scrollbar-width` | `auto`(12px) / `thin`(8px) / `none` / 具体长度 | ✅ | — |
| `float` / `clear` | `left` / `right` / `both` | ✅ | 缺口：clear 只查本容器浮动带，BFC 边界不阻断 margin 塌陷（需求 §5 #153） |

## 4. 文本与字体

| 特性 | 取值 / 范围 | 状态 | 说明 |
|---|---|---|---|
| `font-family` | serif / sans-serif / monospace / 具体字体名映射 | ✅ | — |
| `font-size` | 长度 / 相对单位 | ✅ | — |
| `font-weight` | 数值 / 关键字 | ✅ | — |
| `text-align` | `left` / `center` / `right` | ✅ | — |
| `text-align: justify` | 两端对齐 | ❌ | 需求清单仅列出 left / center / right |
| `line-height` | `auto` / 倍数 | ✅ | — |
| `white-space` | `normal` / `nowrap` / `pre` / `pre-wrap` / `pre-line` | ✅ | — |
| `letter-spacing` / `word-spacing` | 长度 | ✅ | — |
| `text-indent` | 长度（仅首行） | ✅ | — |
| `vertical-align` | 原子行内盒 | ✅ | — |
| `text-decoration` | 可叠加 | ✅ | — |
| 字形回退链 / 彩色 emoji | 字体缺字回退 | ❌ | 需求 §6 #191 待完成项 |
| `writing-mode` / 竖排文本 | — | ❌ | 需求 §5 #154 待完成项 |

## 5. 颜色与背景

| 特性 | 取值 / 范围 | 状态 | 说明 |
|---|---|---|---|
| `color` / `background-color` | 完整 CSS Color 4 具名色表 148 项、`rgba()` / `hsl()`、`transparent`，函数记号支持参数空格 | ✅ | — |
| `background-image` | `url()`（本地图片 BMP/GIF/PNG/JPEG）、`linear-gradient`（方向 / 角度 / 色标） | ✅ | — |
| `background-repeat` | 平铺 | ✅ | — |
| `background-position` / `background-size` | 位置 / 尺寸 | ✅ | 需求 §4 #119 记为「仅结构解析」，§6 #186 已实现背景图层的实际绘制 |
| `object-fit` / `object-position` | 图片适配 | ✅ | — |
| `radial-gradient`（CSS 背景） | 径向渐变 | ❌ | CSS 背景层未支持；SVG 元素内的 `radialGradient` 另见绘制子系统 |

## 6. 定位与层叠

| 特性 | 取值 / 范围 | 状态 | 说明 |
|---|---|---|---|
| `position` | `static` / `relative` / `absolute` / `fixed` | ✅ | abspos 延迟定位 pass 支持跨多层静态祖先的包含块几何 |
| `position: sticky` | 粘性定位 | ❌ | 源码未见 `sticky` 解析（抽样核对 `hpe_css.c` / `hpe_layout.c` 均无） |
| `top` / `right` / `bottom` / `left` / `inset` | 长度 / %，`top+bottom` 拉伸 | ✅ | 缺口：transform / filter 建立包含块、行内祖先下的 abspos 收集（需求 §5 #151） |
| `z-index` | 定位盒 + opacity/transform 分组盒排序 | ✅ | 缺口：z-index 参与命中测试逆序、fixed 强制建立上下文（需求 §5 #152） |
| `isolation` / `will-change` | 层叠上下文控制 | ❌ | 需求 §5 #152 未覆盖说明 |

## 7. Flex / Grid / Table

| 特性 | 取值 / 范围 | 状态 | 说明 |
|---|---|---|---|
| `flex-direction` | `row` / `column`（含 reverse） | ✅ | — |
| `justify-content` | `center` / `flex-end` / `space-between` / `space-around`，auto margin | ✅ | — |
| `align-items` / `align-self` | `stretch` / `baseline` 等 | ✅ | — |
| `flex` / `flex-grow` / `flex-shrink` / `flex-basis` / `order` | — | ✅ | — |
| `flex-wrap` | 换行 | ✅ | — |
| `gap` / `row-gap` / `column-gap` | px / % | ✅ | 源码 `apply_gap`（`hpe_css.c:1442`）仅存 px / % |
| `align-content` / `justify-items` | grid 对齐 | ✅ | — |
| `grid-template-columns` / `grid-template-rows` | `repeat()` / `fr` / `auto` / `%` | ✅ | — |
| `grid-column` / `grid-row` | 含 `span` | ✅ | — |
| `grid-auto-flow` | 自动放置 | ✅ | — |
| table 布局 | 自动列宽、`rowspan` / `colspan`、`border-spacing`、row group、`caption` | ✅ | — |

## 8. 视觉效果

| 特性 | 取值 / 范围 | 状态 | 说明 |
|---|---|---|---|
| `opacity` | <1 离屏合成并建立层叠上下文 | ✅ | — |
| `box-shadow` / `text-shadow` | 阴影 | ✅ | — |
| `transform` | `translate` / `scale` / `rotate`（2D 仿射，离屏合成） | 🟡 | `skew` / `matrix` 等其余函数未支持（需求 §4 #121）；SVG 侧的 skew/matrix 另见绘制子系统 |
| `transition` | 过渡（简写 + 4 个长属性；插值时钟，v0.22） | 🟢 | 简写顶层逗号列表逐项解析（时间→duration/delay、缓动、属性名）；`hpe_engine_tick` 驱动插值，几何属性触发 LAYOUT 级重排；遗留见需求 §14.2（伪元素、`animation*` 事件等） |
| `animation` / `@keyframes` | 关键帧动画（Web Animations 子集，v0.22） | 🟢 | `@keyframes`（`from`/`to`/百分比，同名重定义整体替换）；缓动 `linear`/`ease*`/`cubic-bezier()`/`steps(n, start\|end)`；简写顺序无关 token 归位；delay/direction/iteration/fill-mode；`transform` 分解插值；遗留：停靠点级 easing、`animationstart/end` 事件、`play-state: paused`（需求 §14.2） |
| `filter`（CSS 属性） | 滤镜 | ❌ | CSS 属性未支持；SVG `filter` 子集（feGaussianBlur/feOffset）另见绘制子系统 |
| `cursor` | 13 值全部映射（`pointer` / `move` / `not-allowed` / `crosshair` / `wait` / `ew-resize` / `grab` / `grabbing` 等） | ✅ | `grab` / `grabbing` 绘制近似为手形 |

## 9. 其他

| 特性 | 取值 / 范围 | 状态 | 说明 |
|---|---|---|---|
| 长度单位 | px / % / `em` / `rem` / `vh` / `vw` / `ch` | ✅ | `hpe_len_resolve_full` 精确解析 |
| `visibility` | `visible` / `hidden` | ✅ | — |
| `list-style-type` / `list-style-position` | `disc` / `circle` / `square` / `none` / `decimal`；`inside` / `outside` | ✅ | — |
| BFC 规则 | 浮动 / abspos / 行内块 / 表格单元 / overflow 非 visible 建立 BFC | ✅ | 缺口：BFC 边界阻断 margin 塌陷、行宽不足下沉循环（需求 §5 #153） |
| 表单控件 UA 样式（v0.23） | `input[type=…]` / `button` / `select` / `textarea` / `fieldset` / `legend` / `progress` / `meter` | 🟡 | UA 表提供边框/内边距/背景/字体与 `:checked`/`:disabled` 状态样式；`src/paint/hpe_paint_form.c` 绘制勾选/圆点/箭头/填充/色块/插入符；number/date/color/file/datalist 专用外观仍缺（需求 §14.1） |

---

## 源码抽样核对记录（≥20% 条目）

以下条目经关键词回源码确认存在（`src/css/hpe_css.c`、`src/layout/hpe_layout.c`）：

- `:is(`（11 处）、`nth-child`、`first-child`、`hover`、`@media`（18 处）、`!important`（13 处）、`@supports`（4 处）
- `linear-gradient`（6 处）、`border-radius`、`box-shadow` / `text-shadow`、`object-fit`、`background-repeat`
- `flex-wrap`、`justify-items`、`align-content`、`grid-auto-flow`、`border-spacing`
- `scrollbar-width`、`letter-spacing`、`word-spacing`、`vertical-align`、`list-style-type`、`box-sizing`、`inset`（28 处）、`transform`（35 处）、`transition` / `animation`
- `cursor` 十三值映射（`apply_cursor`，`hpe_css.c:1981`，含 `grab`/`grabbing`）
- `display` 的 `grid`/`inline-grid`/`table`/`inline-table`/`inline-flex` 取值（`hpe_css.c:974-976, 5159`）
- `border-style` 的 `solid`/`dashed`/`dotted`/`double`（`hpe_css.c:1226-1227`）
- 负向确认（v0.22 前的盘点）：`sticky`（0 处）→ 对应 ❌ 条目；`transition`/`animation` 时钟已于 v0.22 落地（`src/css/hpe_anim.c` + `src/hope_anim.c`）
