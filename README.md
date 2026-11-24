# Hope（希望）UI Engine

一款从零自研的、类浏览器的 **UI / 渲染引擎**。

- **遵循 W3C 规范**（HTML / CSS / DOM / JS 子集），架构上对标 Chromium 的分层设计
- **纯 C11，零第三方依赖**——软件光栅逐像素自己合成，不依赖 Skia / Direct2D
- **仅解析本地文件**，不做任何联网能力（无远程资源加载、无 JS 网络 API）
- 一条单向流水线打通：`HTML → DOM → CSS 层叠 → 布局（盒树）→ 绘制 → 软件光栅 → Win32 上屏`

> 当前版本：**v0.13**（HTML 解析补全：容错自动闭合 / table·select·template 专用规则 / 字符编码探测 / base href 与 link rel）

---

## 亮点

| 领域 | 能力概览 |
| --- | --- |
| **平台抽象** | 窗口 / 文件 / 字体 / 时间 / 剪贴板统一经 `src/platform/hpe_platform.h`；系统 API 只允许出现在 `src/platform/<os>/` |
| **HTML 解析** | 手写词法 + 树构建；隐式 `html/head/body`、rawtext、实体解码；**容错自动闭合**（`k_close_rules` 表驱动）、`<table>`（隐式 tbody/tr + foster parenting）、`<select>` 作用域、`<template>.content`；**字符编码探测**（BOM / `<meta charset>` / UTF-8 校验回退 Latin-1）、`<base href>` 与 `<link>` 资源提示 |
| **自研光栅** | 32bpp `0xAARRGGBB`，自上而下，与 Win32 `StretchDIBits` 零转换互通；矩形/覆盖率掩码/圆角/渐变/阴影 |
| **CSS 引擎** | 选择器组合符与属性选择器、**两趟层叠（`!important` 真正参与）**、**`@media` 条件求值**、继承、`em/rem/vw/vh/ch` 精确长度、四边独立边框、`opacity`/`visibility`、线性渐变、盒/文字阴影、2D `transform` |
| **布局** | 块级流、行内文本流与折行（拉丁按词 / CJK 按字）、margin 塌陷、`relative/absolute/fixed`、`inline-block`、`float`/`clear`、单行 `flex` |
| **绘制** | 按边边框、抗锯齿圆角、z-index 分层、`overflow` 裁剪、**脏矩形滚动重绘**（与全量逐字节一致）、设备 y 量化 1/64px |
| **SVG 子集** | 仿射 `transform`、`rect/ellipse/line/polygon/polyline/path`、`path` 全命令 `M L H V C S Q T A Z`、`defs`/`use`、线性/径向渐变（填充 + 描边）、`text` 度量、**命中测试** |
| **JS 引擎** | 手写词法 + 递归下降 + 树遍历解释器；`class`/解构/展开/`async`/生成器/正则/ES 模块；`Date`/`Map`/`Set` 与 Array/String/Number/Math/Object 全量方法；`querySelector(All)` 与事件冒泡；**DOM 变更 API + `MutationObserver`（微任务批量投递）** |
| **增量更新** | DOM 变更经多订阅者广播后分级重排：`none` 早退 / `paint` 只重刷样式 / `layout` 局部子树增量布局（与全量逐字节一致）；脚本增改 `<style>` 即时生效 |
| **浏览器外壳** | 自绘地址栏（完整文本编辑器：选区 / 双击选词 / 三击全选 / Shift 扩展 / `Ctrl+A/C/X/V` / 词移动），无参数时显示内置空白页 |
| **可测试性** | 无头 `--dump` 逐字节像素基线回归 + JS 断言页 + headless ES 模块校验，统一入口 `docs/run_tests.sh` |

---

## 快速开始

### 构建（CMake + MinGW GCC / MSVC，C11）

```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

### 运行

```bash
./build/bin/hope.exe examples/hello.html        # 打开一个本地页面（带地址栏的窗口）
./build/bin/hope.exe                            # 无参数 → 内置空白页
./build/bin/hope.exe examples/hello.html --dump out.bmp   # 无头渲染为 BMP
./build/bin/hope.exe --js script.js             # 无窗口执行一段脚本（console.log 输出）
./build/bin/hope.exe --module module.js         # 按 ES 模块解析（支持 import 相对路径）
```

### 命令行参数

| 参数 | 说明 |
| --- | --- |
| `[file.html]` | 要加载的本地页面；省略则打开内置空白页 |
| `--width N` / `--height N` | 视口尺寸 |
| `--dump out.bmp` | 无窗口渲染为 BMP（默认不含地址栏，保证像素基线稳定） |
| `--chrome` | 配合 `--dump`，把地址栏也画进截图 |
| `--scroll-lines N` | 先渲染一帧，再滚动 N 行二次渲染 —— 真正走脏矩形增量路径 |
| `--bench N` | 热身后重复渲染 N 次，报告平均 paint 毫秒 |
| `--js script.js` | 无窗口执行普通脚本，`console.log` 打印结果，出错返回非 0 |
| `--module module.js` | 按 ES 模块解析执行（`import`/`export`） |

窗口交互：滚轮滚动、`F5` 重新加载、`Esc` 退出；地址栏聚焦时 `Enter` 加载本地路径。

### 无头自测钩子（环境变量）

用于在无窗口下验证 DOM / 布局 / 事件 / 定时器 / 地址栏：

- `HOPE_DEBUG=1`：打印 DOM 树与盒树坐标
- `HOPE_TEST_TEXT=id`：打印指定元素的 `textContent`
- `HOPE_TEST_CLICK=id` / `HOPE_TEST_CLICK_AT=x,y[;x,y...]`：按 id 或坐标合成点击
- `HOPE_TEST_HOVER=x,y`：合成 hover（`:hover` + `mousemove`）
- `HOPE_TEST_WAIT=秒`：泵定时器 / `requestAnimationFrame`
- `HOPE_TRACE_PAINT=1` / `HOPE_TRACE_SVG=1`：打印盒几何 / SVG 视口与 `viewBox`
- `HOPE_TEST_ADDR=路径` + `HOPE_TEST_ADDR_ENTER=1`：模拟在地址栏键入并回车
- `HOPE_TEST_CLIPBOARD=文本` + `HOPE_TEST_PASTE=1`：模拟 `Ctrl+V`
- `HOPE_TEST_KEYS="LEFT;SHIFT_RIGHT;CTRL_C"` / `HOPE_TEST_TYPE="文本"`：具名按键 / 直接键入
- `HPE_NO_MASK_CACHE=1` / `HPE_NO_DIRTY=1`：分别旁路文本掩码缓存 / 脏矩形（用于对照）

---

## 回归测试

```bash
./docs/run_tests.sh            # 逐字节像素基线 + JS 断言 + headless ES 模块
./docs/run_tests.sh --rebase   # 用当前输出刷新像素基线
```

回归由三部分组成：

1. **逐字节像素基线**——`--dump` 无头输出与 `build/baseline/*.bmp` 逐字节比对（16 个页面：`boxes`、`css`、`css-cascade`、`css-props`、`dom-mutation`、`event`、`hello`、`hover`、`html-parse`、`js-arrow`、`js-features`、`js`、`js-builtins`、`layout`、`svg-shapes`、`timers`）
2. **JS/解析断言页**——`js-modern.html` 断言 `PASS 33/33`（语言语义）、`js-builtins.html` 断言 `PASS 37/37`（内置库）、`dom-mutation.html` 断言 `PASS 35/35`（DOM 变更 API + `MutationObserver`）、`html-parse.html` 断言 `PASS 26/26`（解析容错：自动闭合 / 隐式 tbody / foster parenting / select 过滤 / `template.content`）
3. **headless ES 模块**——`--module` 校验 `import`/`export`/默认导出与相对路径解析

全部通过时打印 `ALL TESTS PASSED`。

---

## 示例页面（`examples/`）

| 页面 | 内容 |
| --- | --- |
| `hello.html` | 浅色卡片页（最小示例） |
| `boxes.html` | 盒模型 / 对齐 / 滚动 |
| `layout.html` | margin 塌陷 / `relative` / `absolute` / `fixed` / `inline-block` / `z-index` |
| `float.html` | `float:left/right` 环绕 / `clear:both` |
| `css.html` | 选择器组合符 / 属性选择器 / `white-space` / `font-family` |
| `css-props.html` | **样式属性综合**（v0.9）：四色边框、min/max、`box-sizing`、`overflow`、`opacity`、渐变、阴影、`transform`、相对单位、间距、负 margin、list-style、`rgba/hsl` |
| `css-cascade.html` | **层叠与继承综合**（v0.10）：specificity + 来源顺序、`!important` 四类优先级、`@media`、继承属性、`visibility` 子树重现 |
| `svg-shapes.html` | **SVG 绘制子集综合**（v0.11）：新图元、`transform`、线性/径向渐变、`path` 全命令、`defs`/`use`、`text` 度量 |
| `event.html` | 事件：`onclick` / `addEventListener` / 冒泡 / 样式切换 |
| `hover.html` | `:hover` 伪类 + `mousemove` 监听器 |
| `timers.html` | `querySelector(All)` + `setTimeout` / `setInterval` / `rAF` |
| `js.html` / `js-features.html` / `js-arrow.html` | DOM 操作 / 语言特性 / 箭头函数 |
| `js-modern.html` | **现代 JS 综合断言**（v0.7）：33 项 `check()` |
| `js-builtins.html` | **内置库综合断言**（v0.8）：37 项 |
| `dom-mutation.html` | **DOM 变更与观测综合断言**（v0.12）：35 项——`cloneNode`/`insertBefore`/`replaceChild`/`DocumentFragment`/`MutationObserver` |
| `html-parse.html` | **解析容错综合断言**（v0.13）：26 项——`<p>`/`<li>`/`<td>`/标题等错误嵌套自动闭合、游离 `</p>`/`</br>`、隐式 `tbody`/`tr`、`td` 激活、foster parenting、`<select>` 过滤、嵌套 select、`template.content` 脱离主树 |

---

## 目录结构

```
src/
  main.c              命令行入口 + 窗口事件 + --dump 截图
  hope.h / hope.c     引擎门面：加载、重排（变更分类 + 惰性/增量 flush）、滚动、渲染、状态
  hope_internal.h     引擎结构体与 hope.c / hope_chrome.c 之间的内部接口
  hope_chrome.c       浏览器外壳：内置地址栏的绘制、文本编辑、导航
  base/               矩形/颜色/内存/utf8/CSS 值解析
  platform/           平台抽象接口 + win32 实现（窗口、GDI 覆盖率掩码、文件、剪贴板）
  render/             pixmap（像素缓冲） raster（矩形/掩码/渐变混合） font（字体 + 掩码缓存）
  dom/                DOM 树（节点/fragment/属性、clone/insertBefore/replaceChild、变更通知订阅）
  html/               HTML 词法 + 树构建（隐式 html/head/body，rawtext style/script，
                      容错自动闭合、table/select/template 专用规则、foster parenting，
                      hpe_encoding 字符编码探测 BOM/meta/UTF-8 校验）
  css/                选择器解析匹配、层叠与继承、@media、UA 样式表
  layout/             盒树 + 块/行内/浮动/flex 布局（margin 塌陷、定位、z-index、局部子树增量重排）
  paint/              递归绘制（背景、按边边框、圆角、文本、列表标记、阴影、渐变、transform）
                      hpe_svg.c —— SVG 子集绘制 + 命中测试
  js/                 JS 引擎：value / parser / interp / regex / date / dom 绑定 / mutation / timers
examples/             17 个演示与断言页面（见上）
docs/                 需求.md（需求清单）、design.md（设计文档）、AI代码生成规范.md、
                      run_tests.sh（回归入口）
```

**模块依赖单向**：`hope → html → dom → css → layout → paint → render → platform`，禁止反向；
只有 `src/platform/<os>/` 可以触碰系统 API。

---

## 里程碑

| 版本 | 内容 | 状态 |
| --- | --- | --- |
| v0.1 | 垂直竖切：HTML → CSS → 布局 → 光栅 → 上屏 | 完成 |
| v0.2 | JS 引擎（自研解释器 + DOM 绑定 + 事件） | 完成 |
| v0.3 | 布局补全（一）：margin 塌陷、`position`、`inline-block`、`z-index` | 完成 |
| v0.4 | 浮动与清除、脏矩形、定时器、字体掩码缓存 | 完成 |
| v0.5 | 浏览器外壳（内置地址栏）+ 内置空白页 | 完成 |
| v0.6 | flex 单行布局、圆角、SVG 子集、地址栏文本编辑 | 完成 |
| v0.7 | JS 语言特性大补齐（class / 解构 / async / 生成器 / 正则 / ES 模块 / 浏览器全局） | 完成 |
| v0.8 | JS 内置库补齐（`Date` / `Map`·`Set` 迭代 / 全量方法） | 完成 |
| v0.9 | 样式属性批次：四色边框、min/max、`box-sizing`、`overflow`、`opacity`/`visibility`、渐变、阴影、`transform`、相对单位、间距、list-style | 完成 |
| v0.10 | 层叠与继承补全：`!important` 参与层叠、`@media` 条件求值、继承属性集合 | 完成 |
| v0.11 | SVG 绘制子集扩展：仿射 `transform`、新图元、`path` 全命令、`defs`/`use`、线性/径向渐变、命中测试 | 完成 |
| v0.12 | DOM 模型补全：惰性重排、局部子树增量布局、`MutationObserver`、`DocumentFragment`/`cloneNode`/`insertBefore`/`replaceChild`、CSS 增量样式刷新 | 完成 |
| v0.13 | HTML 解析补全：容错自动闭合、`<table>`/`<select>`/`<template>` 专用规则、字符编码探测、`<base href>` 与 `<link>` 资源提示 | 完成 |
| v0.14+ | 布局补全（二）、绘制补全（二）、样式补全（三）、JS 补全、基础设施 | 规划中 |

---

## 已知缺口（后续里程碑）

- **绘制**：`<img>` / 图片解码（PNG / JPEG / GIF）、滚动条 UI、页面内文本选中与复制、插入符闪烁 / I 形光标、子像素文字定位、彩色 emoji / 字形回退链、CSS `radial-gradient`、`background-position/size` 完整应用
- **SVG**：`mask` / `clipPath` / `<marker>` / `filter` / `gradientTransform` / `<tspan>`、`lengthAdjust="spacingAndGlyphs"`
- **布局**：`grid`、`table`、flex 的 `wrap`/`shrink`/`order`/`stretch`、元素级滚动容器、完整层叠上下文
- **样式**：结构伪类（`:first-child`/`:nth-child`）、伪元素（`::before`/`::after`）、状态伪类（`:focus`/`:active`）、`skew`/`matrix`/3D、`transition`/`animation` 插值时钟
- **JS**：`getBoundingClientRect`、`Proxy`/`Reflect`/`Intl`、`WeakMap`·`WeakSet`、`BigInt`、TypedArray、真正的事件循环与 GC、宿主包装器同一性比较
- **HTML 解析**：非完整 HTML5 状态机（未实现 `<frameset>`、`<form>` 隐式闭合、`<svg>`/`<math>` 命名空间切换、`<textarea>`/`<title>` RCDATA、属性重复剔除、`<template>` 惰性解析）；编码仅 UTF-8/UTF-16/Latin-1（无 GBK/Big5/Shift-JIS）
- **基础设施**：JS GC（整页重载才回收）、跨平台后端（当前仅 Win32）

> 注：DOM / CSS 增量更新已于 v0.12 落地（变更分级 + 局部子树增量布局 + 脚本增改 `<style>` 即时生效）。
> 注：HTML 解析四大类（容错自动闭合 / table·select·template / 编码探测 / base href 与 link rel）已于 v0.13 落地。

> 完整清单见 `docs/需求.md`；设计权衡与实现细节见 `docs/design.md`。

---

## 开发约定

- 纯 C11，不使用 C++；头文件用 `hpe_` / `HPE_` 前缀，公共 API 在 `hope.h`。
- 平台相关代码只允许出现在 `src/platform/<os>/`，其余模块只能通过 `hpe_platform.h` 访问系统能力。
- 内存统一走 `hpe_alloc` / `hpe_calloc` / `hpe_free`。
- **写代码的同时补齐设计文档**（`docs/design.md` + `AGENTS.md`）。
- 代码规范见 `docs/AI代码生成规范.md`：单函数 ≤50 行，超长函数用「表驱动分发 + 内聚辅助函数」拆分；
  `python3 docs/func_len_audit.py 50` 自查（当前为空），改后回归须 `ALL TESTS PASSED`。

---

## 文档索引

| 文件 | 内容 |
| --- | --- |
| `README.md` | 本文件——项目总览与上手 |
| `docs/需求.md` | 需求清单与里程碑状态（含逐项 `[x]` / 待完成） |
| `docs/design.md` | 设计文档：架构、各子系统、关键决策与权衡、缺口 |
| `docs/AI代码生成规范.md` | 代码生成规范 |
| `docs/AI纯C语言注释规范.md` | Doxygen 中英双语函数注释规范 |
| `docs/func_len_audit.py` | 函数长度自查工具（`python3 docs/func_len_audit.py 50`） |
| `docs/run_tests.sh` | 回归入口（像素基线 + JS 断言 + ES 模块） |
| `AGENTS.md` | 项目说明：目录、构建、当前能力、缺口 |
| `.workbuddy/memory/` | 工作日志与项目长期记忆 |
