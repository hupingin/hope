## 项目概述
- 本项目是一个UI引擎项目。
- 本项目计划从0开始开发一款UI引擎。
- 项目名：Hope.
- 中文名：希望
- 开发都以英文名进行。

## 项目终极目标
- 引擎要支持w3c规范，类似于chromium.
- 但是引擎不要有上网功能。
- 引擎只解析本地html+css+js文件。

## 架构与目录
- 技术栈：纯c，不要c++.
- 项目代码都放在src下。

## 规范
- 代码生成：参考 docs/AI代码生成规范.md
- 函数注释：参考 docs/AI纯C语言注释规范.md —— 所有函数必须写 Doxygen 风格文档注释
  （`/**` 开头、中英双语「中文 / English」、`@param[in|out|in,out]` 标注方向、
  标签顺序 `@brief`→`@details`→`@param`→`@return`→`@retval`→`@note`→`@warning`→`@see`）。
- **落地状态**：`src/` 全部 1742 个函数定义与原型（含头文件里的 `static inline`）均已带合规
  注释块（1742 个 `/**`、5184 个 `@param`）。新增/修改函数时按同规范补注释，可复用 `tools/`
  下的生成管线（见下）。

## 注释维护工具（tools/）
- `tools/doxy_extract.py` —— 字符串/注释感知地提取全部函数定义与原型（`.c` 与 `.h` 都会扫，
  头文件里的 `static inline` 定义同样计入），产出 `tools/functions.json`。
- `tools/doxy_apply.py` —— 依据描述表把 Doxygen 块插入源码，幂等（已有 `/**` 则跳过），
  支持 `--dry` 预演、`--force` 覆盖；自动合并 `*.part*.json` 分片。
  参数方向按类型推断（`const` 指针→`in`，其余指针→`in,out`，按值→`in`），
  `@param[...]` 标签统一 15 列宽对齐，参数名恒从第 18 列开始。
- `tools/paramdict.json` —— 全局参数名词典（561 条，覆盖全部 5184 个参数名），避免逐函数重复描述
  通用参数（`js`/`engine`/`node`/`out`/`s`…）。
- `tools/notes/<模块>__<文件>.json` —— 每文件的函数描述表（`brief`/`params`/`return`/`retval`/
  `note`/`warning`/`see`）。
- `tools/NOTES_SPEC.md` —— 描述表编写规范。

改动函数签名后的标准流程：

```
python tools/doxy_extract.py            # 刷新函数清单
python tools/doxy_apply.py <文件> --dry  # 预演，确认 skip=0
python tools/doxy_apply.py <文件>        # 插入
python tools/doxy_apply.py --dry         # 全树校验：应为 0 blocks（幂等）
```

## 已确定的技术选型
- 抽象平台层 + 自研软件光栅（像素级自己合成，不依赖 Direct2D/Skia）。
- 构建：CMake + MinGW GCC（也可 MSVC）。C11。
- 像素格式：32bpp `0xAARRGGBB`，自上而下，可直接与 Win32 `StretchDIBits` 互通。

## 一定要
- 写代码的同时补齐设计文档

## 规范参考：Chromium 源码（OpenGrok）
- OpenGrok 实例当前索引 **`chromium-120.0.6099.298`**，可查 Blink 的布局/样式/解析/绘制实现，
  用于**对齐 W3C/CSS/HTML 规范行为**、核对边界情形与命名。
- **仅作参考**：本项目是纯 C 自研引擎，不照抄其 C++ 代码与架构，查清规范行为后用纯 C 自己实现。
- 关键路径：`src/third_party/blink/renderer/core/{layout,css,paint,html}/`、`core/layout/{grid,ng,svg}/`。
- 查法：`opengrok_list_projects` → `opengrok_search`
  （`projects=chromium-120.0.6099.298`，`kind`=full/def/symbol/path/hist）
  → `opengrok_get_file(path, start, end)`，path 如
  `/chromium-120.0.6099.298/src/third_party/blink/renderer/core/layout/grid/layout_grid.h`。

## 渲染对齐基准
- 参考页面 `copyResource/aim.html` 曾用作「真页面」验收样本（页头 flex 横排、SVG 思维导图
  （viewBox 缩放、三次曲线边、圆角节点）、JS 生成 DOM、点击折叠、`border-bottom` 分隔线）。
  **注意：该目录当前不在工作区中**（未被本流程改动，亦不属于 `run_tests.sh` 回归项）；
  若需重建对齐基准，请将其放回 `copyResource/` 后再执行下述流程。
- 对齐方法（样本就位时）：`python -m http.server` 起本地服务 → Chrome 截图（与引擎同尺寸，如 1920x953）→
  `hope.exe copyResource/aim.html --width 1920 --height 953 --dump out.bmp` → 逐区域比对；
  几何不确定时用探针页面把 `W/H/scale/rect` 写进 `document.title` 读出来对账。

## 目录结构
```
src/
  main.c                 命令行入口 + 窗口事件 + --dump 截图
  hope.h / hope.c        引擎门面：加载、重排（变更分类 + 惰性/增量 hpe_engine_flush）、滚动、渲染、状态
  hope_internal.h        引擎结构体 + hope.c/hope_chrome.c 之间的内部接口（含脏标记与重排级别）
  hope_chrome.c          浏览器外壳：内置地址栏的绘制、文本编辑、导航（无参数时显示内置空白页）
  base/                  hpe_types.h（矩形/颜色） hpe_util（内存/utf8/CSS值解析）
  platform/              hpe_platform.h（窗口/文件/字体/时间抽象）
  platform/win32/        Win32 实现：窗口、GDI 字体覆盖率掩码、文件读取、剪贴板（UTF-16 ⇄ UTF-8、CF_HDROP）
  render/                pixmap（像素缓冲） raster（矩形/掩码混合） font（字体缓存+字宽缓存）
  dom/                   DOM 树（节点/fragment/属性、树操作 clone/insertBefore/replaceChild、
                         变更通知多订阅者 hpe_dom_add_mutation_sink）
  html/                  HTML 词法 + 树构建（隐式 html/head/body、rawtext style/script、
                         容错自动闭合 k_close_rules、table/select/template 专用规则、
                         foster parenting；hpe_encoding 字符编码探测 BOM/meta/UTF-8 校验）
  css/                   选择器解析匹配、层叠、UA 样式表
  layout/                盒树 + 块布局(含 margin 塌陷/relative/absolute/fixed/inline-block/flex)
                         + 行内文本流与折行
  paint/                 递归绘制：背景、按边边框、圆角、文本行、列表标记
                         hpe_svg（SVG 子集绘制 + 命中测试）
  js/                    JS 引擎：hpe_js.h（公共 API+宿主 vtable）
                         value（arena/值/对象/原型/转换/异常/环境） parser（词法+递归下降，含正则/模板字符串脱糖）
                         interp（树遍历执行+内置对象，含统一循环执行器 exec_loop） regex（回溯正则子集）
                         date（Date 内建：civil 日历算法 + ISO 解析/格式化，数值存 Unix 毫秒）
                         dom（DOM 绑定+浏览器全局+变更 API） mutation（MutationObserver：订阅/过滤/
                         微任务批量投递） timers（setTimeout/rAF）
examples/                hello.html（浅色卡片页） boxes.html（盒模型/对齐/滚动）
                         js.html（DOM 操作） js-features.html（语言特性） js-arrow.html（箭头函数）
                         js-modern.html（现代 JS 综合断言：class/异常/解构/spread/标签循环/async/生成器等 33 项）
                         js-builtins.html（内置库综合断言：Date/Map/Set/Array/String/Number/Math/Object 共 37 项）
                         dom-mutation.html（DOM 变更与观测综合断言：cloneNode/insertBefore/replaceChild/
                         DocumentFragment/MutationObserver 共 35 项）
                         html-parse.html（解析容错综合断言：自动闭合/隐式 tbody/td 激活/foster parenting/
                         select 过滤/嵌套 select/template.content 共 26 项）
                         event.html（事件：onclick/addEventListener/冒泡/样式切换）
                         css.html（选择器组合符/属性选择器/white-space/font-family）
                         css-props.html（样式属性综合：四色边框/min-max/box-sizing/overflow/opacity/
                         渐变/阴影/transform/相对单位/间距/负margin/list-style/rgba-hsl）
                         css-cascade.html（层叠与继承综合：specificity/!important/@media/继承属性/
                         visibility 子树重现）
                         svg-shapes.html（SVG 综合：ellipse/polygon/polyline/line、transform、
                         渐变、path 全命令、defs/use、text 度量）
                         layout.html（margin塌陷/relative/absolute/fixed/inline-block/z-index）
                         hover.html（:hover 伪类/mousemove 监听）
                         float.html（float:left/right 环绕/clear:both）
                         timers.html（querySelector/querySelectorAll/setTimeout/setInterval/rAF）
copyResource/            真实参考页面（aim.html：思维导图，用 Chrome 渲染做逐区域对齐基准）— 当前不在工作区
docs/                    design.md（设计文档）、需求.md（需求清单）、
                         AI代码生成规范.md（代码生成规范）、
                         AI纯C语言注释规范.md（Doxygen 中英双语函数注释规范）、
                         func_len_audit.py（函数长度自查：python3 docs/func_len_audit.py 50）、
                         run_tests.sh（回归入口：逐字节像素基线 + js-modern/js-builtins/dom-mutation/
                         html-parse 断言 + headless ES 模块）
tools/                   doxy_extract.py / doxy_apply.py（Doxygen 注释批量提取与插入）、
                         paramdict.json（全局参数词典 561 条）、NOTES_SPEC.md（描述表规范）、
                         notes/（每文件的函数描述表）、functions.json（函数清单缓存）
```

## 构建与运行
```
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
./build/bin/hope.exe examples/hello.html
```
- 参数：`[file.html] [--width N] [--height N] [--dump out.bmp] [--bench N] [--scroll-lines N] [--chrome] [--js script.js] [--module module.js]`
- `--js script.js` / `--module module.js`：**无窗口**执行一段脚本（后者按 ES 模块解析、支持 `import`
  相对路径），用 `console.log` 打印结果，出错返回非 0 退出码；用于在没有页面的情况下验证语言特性与模块加载。
- 无文件参数 → 内置空白页；窗口模式顶部带可输入本地路径的地址栏
  （Enter 加载、Esc 还原并失焦、Backspace/Delete 删字符或选区、方向键/Home/End 移动、
  Shift+移动扩展选区、Ctrl+A/C/X/V 全选/复制/剪切/粘贴、Ctrl+Backspace/Delete 删一个词）。
- 交互：滚轮滚动、F5 重新加载、Esc 退出。
- 回归自检：`./docs/run_tests.sh`（脚本位于 `docs/`，内部自行 `cd` 到项目根；逐字节像素基线
  `build/baseline/*.bmp` + `js-modern.html`/`js-builtins.html`/`dom-mutation.html`/`html-parse.html` 断言 +
  headless 模块校验；`--rebase` 刷新基线）。
- 代码规范自查：`python3 docs/func_len_audit.py 50`（C 感知，剥离注释/字符串后计花括号配平）列出
  `src/**/*.c` 中超过 50 行的函数；当前为空（全项目无函数超 50 行）。
- `--dump` 无窗口渲染为 BMP，用于无头验证渲染结果（默认不含地址栏，加 `--chrome` 才包含）。
- 环境变量 `HOPE_DEBUG=1` 会打印 DOM 树与盒树坐标；`HOPE_TEST_CLICK=id`（按 id 触发点击）/
  `HOPE_TEST_CLICK_AT=x,y[;x,y...]`（按坐标合成点击，可驱动 SVG 节点折叠等交互）/
  `HOPE_TEST_HOVER=x,y` /
  `HOPE_TEST_WAIT=秒`（泵定时器）/ `HOPE_TEST_TEXT=id`（打印 textContent）/
  `HOPE_TEST_META=1`（打印文档编码/meta/base href/link 资源提示）/
  `HOPE_TRACE_PAINT=1`（盒几何）/ `HOPE_TRACE_SVG=1`（svg 视口与 viewBox）/
  `HOPE_TEST_ADDR=路径` + `HOPE_TEST_ADDR_ENTER=1`（模拟在地址栏键入并回车）/
  `HOPE_TEST_CLIPBOARD=文本` + `HOPE_TEST_PASTE=1`（+`HOPE_TEST_PASTE_ENTER=1`，模拟 Ctrl+V）/
  `HOPE_TEST_ADDR_TEXT=1`（打印文本/选区/插入符/焦点）/
  `HOPE_TEST_ADDR_CLICK="x[,mods][;x...]"` / `HOPE_TEST_ADDR_DBLCLICK=x[,x2]` /
  `HOPE_TEST_ADDR_DRAG=x1,x2[,y]` / `HOPE_TEST_TYPE="文本"`（直接键入）/
  `HOPE_TEST_KEYS="LEFT;SHIFT_RIGHT;CTRL_C;CTRL_BACKSPACE"`（具名按键）用于验证地址栏编辑。

## 当前能力（v0.1，完整竖切已打通）
HTML 解析 → CSS 层叠 → 块/行内布局 → 软件光栅 → 上屏。
- HTML：标签/属性/注释/DOCTYPE/实体解码，隐式 html/head/body，`<style>`、`<link rel=stylesheet>`（本地相对路径）；
  **v0.13 补齐**：容错自动闭合（`<li>`/`<p>`/`<td>`/标题/`<a>` 等错误嵌套自愈、游离 `</p>`/`</br>`）、
  `<table>`（隐式 tbody/tr、foster parenting）、`<select>` 作用域过滤、`<template>.content`、
  字符编码探测（BOM / `<meta charset>` / 严格 UTF-8 校验回退 Latin-1）、`<base href>` 与 `<link>` 其它 `rel` 资源提示。
- CSS：类型/类/id/通配选择器、组合符 ` `（后代）`>`（子代）`+`（相邻兄弟）`~`（通用兄弟）、属性选择器 `[a]` `[a=v]` `[a~=v]` `[a|=v]` `[a^=v]` `[a$=v]` `[a*=v]`、**`@media` 条件求值**（媒体类型/`and`/`not`/`only`/逗号或列表/区间特性/嵌套，按视口层叠期求值）、行内 style、specificity+顺序层叠（**两趟：normal 后 important，`!important` 真正参与层叠**）、继承（color/font-size/font-weight/text-align/line-height/white-space/font-family，**v0.10 增补 visibility/cursor/list-style-*/letter-spacing/word-spacing/text-indent**）。
- 样式属性：display（block/inline/inline-block/none）、color、background(-color)、width/height（px/%/auto）、margin/padding（1~4 值、auto 居中）、border(-width/-color)、font-size、font-weight、text-align、line-height、**white-space**（normal/nowrap/pre/pre-wrap/pre-line）、**font-family**（serif/sans-serif/monospace + 具体字体名映射）、**position**（static/relative/absolute/fixed）+ **top/right/bottom/left**、**z-index**、伪类 **`:hover`**。
- **样式属性扩展（v0.9）**：`min-width/max-width/min-height/max-height`、`box-sizing`（content-box/border-box）、
  `overflow`（visible/hidden/auto/scroll）、`opacity`、`visibility`、`cursor`、`text-decoration`、
  `background-image` 与 `background` 的线性渐变、`box-shadow`/`text-shadow`、
  `transform`（translate/scale/rotate）、`transition`/`animation`（仅解析存储）、
  `em/rem/vh/vw/ch` 精确长度、`letter-spacing`/`word-spacing`/`text-indent`/`vertical-align`、负 `margin`、
  `list-style-type`/`list-style-position`、四值 `border-color`（四边独立）、`rgba()/hsl()/transparent` 与完整具名色表。
- **层叠与继承扩展（v0.10）**：`!important` 参与层叠（`strip_important` 回报存在性，逐条声明记入
  `HpeStyle.imp/imp2`；`hpe_css_compute` 两趟——normal 按 specificity+顺序应用，再应用 important，
  important 内部行内 `style` 优先）；`@media` 条件求值（`HpeMediaQuery` 记录每个 `@media` 块，
  `HpeStylesheet.vw/vh` 由 `hpe_css_set_viewport` 在重排前写入）；继承属性集合补全；
  `visibility: hidden` 保留占位、**不阻断子树**（后代 `visible` 可重现，绘制期 `self_visible` 门控本盒绘制）。
- 布局：块级流、行内文本流、拉丁按单词/CJK 按字折行、行高与基线、水平 auto 居中、百分比宽度、**margin 塌陷**（相邻兄弟 + 无边界分隔的父首/末子）、**relative**（整棵子树偏移）、**absolute/fixed**（脱离文档流，含块包含块=最近定位祖先 padding box，fixed=视口）、**inline-block**（原子行内盒，支持显式宽度与 shrink-to-fit）、**float:left/right + clear**（浮动带避让、行盒环绕、BFC 简化：容器高度包住浮动）、
  **flex 单行布局**（row/column、收缩到适应宽度、gap、align-items/justify-content、auto margin 右推、flex-grow；
  子项按规范块化；`inset` 上下同时给出且高度 auto 时按包含块拉伸）。
- 绘制：矩形填色（含 alpha）、**按边边框**、**抗锯齿圆角**（填充 + 环带描边）、文字覆盖率掩码抗锯齿混合、
  `ul > li` 项目符号、**z-index 分层**（定位盒排序后绘制）、**SVG 子集**（见下）、
  **脏矩形滚动**（裁剪矩形约束的光栅 + 区域重放，与全量逐字节一致）、设备 y 量化 1/64px（保证平移不变的取整）。
- **SVG 绘制子集（v0.6 起，v0.11 大幅扩展，`src/paint/hpe_svg.c`）**：整棵子树用一个 2×3 仿射矩阵
  `Mat` 映射；元素 `g`/`rect`(rx)/`circle`/`ellipse`/`line`/`polygon`/`polyline`/`path`/`text`；
  `transform`（translate/scale/rotate/skewX/skewY/matrix，可串联）；`defs`/`use`（href + x/y，
  嵌套上限 6）；`linearGradient`/`radialGradient`/`stop`（`fill`/`stroke` 均可 `url(#id)`，
  `objectBoundingBox` 与 `userSpaceOnUse`）；`path` 全命令 `M L H V C S Q T A Z`；`text` 的
  `text-anchor`/`dominant-baseline`/`textLength`。几何统一编译为设备空间多边形，填充用 4× 超采样
  even-odd 扫描线、描边用逐段胶囊。**命中测试**：逆向矩阵回用户空间，`rect`/`circle`/`ellipse`/
  `line`/`polygon`/`polyline`/`path`/`text` 全覆盖（点到多边形/线段），后绘制者先命中、冒泡到 `<g>`。
- **绘制扩展（v0.9）**：四边独立 `border-color`、`background-image`/`background` 的 `linear-gradient`
  （方向/角度/色标）、盒阴影与文字阴影、`list-style-type`（disc/circle/square/decimal/none）与
  `list-style-position`、文字装饰（underline/overline/line-through）、`visibility: hidden`（保留占位）、
  `overflow` 裁剪（pad-box）、`opacity`（离屏合成 + alpha 混合，嵌套上限 3 层）、
  `transform`（translate/scale/rotate，2D 仿射、逆映射最近邻采样）。
- **浏览器外壳（v0.5 + v0.6 文本编辑）**：引擎自绘 44px 地址栏（条带底色/分隔线/圆角输入框/焦点环/
  占位提示/失焦尾部对齐/插入符），页面绘制下移 `origin_y`；无参数启动显示内置空白页；
  地址栏可键入本地路径、Enter 加载（相对路径经 `hpe_path_absolute` 解析）、Esc 还原为上次地址并失焦、
  点击条带聚焦（未聚焦时**全选**，Chrome 行为）、点击页面或条带空白处失焦；
  **鼠标**：点击定位、拖选（自动横向滚动）、双击选词、三击全选、Shift+点击扩展（平台 `SetCapture` 保证
  拖出窗口仍跟踪）；**选区**：蓝底白字、有选区时不画插入符、失焦不画高亮；
  **Ctrl+V 粘贴**（平台剪贴板 `hpe_clipboard_get_text`：优先 `CF_UNICODETEXT`，否则 `CF_HDROP` 取首个文件
  路径；粘贴文本去掉换行/控制符并按地址容量截断，替换当前选区）；编辑全程以 UTF-8 码点为最小单位，
  不会切碎中文；加载失败保留输入并在状态行显示 `ERR:`；滚动时外壳固定不动。
- **浏览器能力批次（v0.21）**：
  - **页面文本选中与复制**：选区端点按 DOM 文本节点存储（`HpeSelPos{node,off}`，盒树重建后存活）；
    命中→光标映射两级（DFS 命中带行盒 → `sel_caret_at` 逐 run 半字形对齐 → `sel_run_owner` 反查
    文本盒）；拖选/Shift 扩展/双击选词（0.4s·4px 多击检测）；`hpe_sel_prepare` 生成文档序选区表，
    paint 逐 run 高亮（折叠空格字面量 run 以 `carry` 继承、整行选中边到边）；`hpe_sel_copy` 按
    块级祖先插 `\n` 写剪贴板；Ctrl+A/C/Escape。核心在 `src/hope_sel.c`，钩子 `HOPE_TEST_SEL`/
    `SEL_DRAG`、trace `HPE_TRACE_SEL=1`。
  - **多标签页**：`src/hope_chrome.c` 28px 标签条（标签体/×/＋）；每标签仅存身份
    （路径/`nav[32]` 历史环/滚动），切换 memcpy 进出 + `nav_suppress` 重载；
    Ctrl+T/W/Tab/Shift+Tab；API `hpe_engine_tab_count/active/switch`。
  - **IME 组合输入**：Win32 `WM_IME_*` → `ImmGetCompositionStringW` → 回调
    `on_ime_compose/on_ime_commit`；组合串在地址栏插入符处内联显示（下划线），提交即插入。
  - **竖排文本**：`writing-mode: vertical-rl/lr`（CJK 直立下排、拉丁旋转 90°，
    `mask_rotate90` 顶对齐掩码）；基线 `examples/vertical.html`。
  - **彩色 emoji**：COLRv0/CPAL 逐层合成精灵（`g_sprites` 缓存含前景色键），ZWJ/VS16/修饰符/
    地区旗序列；回退链 `fb[]` 分段合成段落掩码；基线 `examples/emoji.html`。
  - **测试与质量**：C 单测框架 `hope_tests`（42 用例/223 断言，CTest 接入）；CSS 布局矩阵
    `css-layout-matrix.html`（56 过 + 12 XFAIL）；基准入库 `tools/bench_run.py` +
    `bench_compare.py`；内存账本 `HPE_ALLOC_STATS=1|2`（借其修复 emoji 精灵/竖排掩码/
    PNG 解压缓冲/图片缓存键四类泄漏）。

## JS 引擎（v0.7）
纯 C 手写，无第三方依赖。执行时机：DOM 解析 + 样式收集完成后、首次布局之前（等价于 defer 脚本）。
- 前端：手写词法（数字/字符串/\u 转义/注释/模板字符串/正则字面量）+ 递归下降解析，AST 存于 arena。
- 运行时：树遍历解释器；arena 堆（整块释放）；`setjmp/longjmp` 做运行时错误传播（`try_top` 活动帧链）。
- **语言子集（完整清单）**：
  - 语句：`var/let/const`、函数声明与表达式、`class`/`extends`/`super`（getter/setter、静态成员、
    实例字段、私有 `#name`、静态初始化块）、`if/else`、`while`、`do/while`、`for`、`for-in`、`for-of`、
    `switch`（含落空穿透）、`break/continue` + **标签**、`return`、`throw`、`try/catch/finally`、
    `import`/`export`（ES 模块）。
  - 表达式：全部算术/比较/逻辑/**位运算**（`& | ^ ~ << >> >>>`）、`**`、三元、`typeof`、`instanceof`、`in`、
    `delete`、`void`、**可选链 `?.`**、**空值合并 `??`/`??=`**、复合赋值（含 `&&= ||= ??=`）、自增自减、
    逗号、成员访问与方法调用、`new`、**`this` 与 `call/apply/bind`**、**解构**（数组/对象/嵌套/默认/剩余）、
    **展开 `...`**（调用参数、数组/对象字面量）、**默认参数与剩余参数**、**模板字符串**（含 `${}` 嵌套）、
    正则字面量、**`async`/`await`**、**生成器 `function*`**。
- 函数标志 `fn_flags`：`FN_ARROW / FN_ASYNC / FN_GEN / FN_STRICT / FN_CTOR / FN_BOUND / FN_CLASS`
  （与 `JsNode.op` 共享位）。`FN_BOUND` 用独立的 `JsObj.bound_target` 记目标函数。
- **箭头函数**：`=>` 是独立词法单元；表达式体脱糖为 `{ return <expr>; }` 复用 `JS_N_FUNC`；
  不建自己的 `this`（沿作用域链读外层）。
- **统一循环执行器 `exec_loop`**：`WHILE/DO/FOR/FOROF/FORIN` 走同一条路径，接收整条标签链
  （`JS_N_LABEL` 收集后传入）；`Flow{action,ret,label}` 逐层展开，命中自身标签（或无标签）的
  `break/continue` 被本层消费，未命中的继续向外传播——因此 `continue outer` / `break label` 语义正确。
- **原型链**：`JsObj.proto`，`js_obj_find_prop` 沿链查找（含访问器 `JsProp{getter,setter}`）；
  `class X extends Y` 同时建 `X.prototype.__proto__=Y.prototype` 与 `X.__proto__=Y`。
- **异常**：`throw` → `js_throw_value`，优先 `longjmp` 到最内层 `JsTryFrame`；无 try 帧才落到 `js->jmp`。
  `finally` 在正常/`return`/`break`/异常下都执行，其 `return` 覆盖此前值。
- 内置：`console.*`、`alert`、`Math.*`（含 `random`（时钟播种 xorshift64）/`clz32`/`imul`/`fround`/
  `log1p`/`expm1`/`acosh`/`asinh`/`atanh`）、`JSON.stringify/parse`、`parseInt/parseFloat/isNaN/isFinite`、
  `encodeURI(Component)/decodeURI(Component)`、`Object.keys/values/entries/fromEntries/assign/freeze/seal/
  isFrozen/isSealed/create/defineProperty/getOwnPropertyNames/hasOwn/getPrototypeOf/is`、
  `Array.from/of/isArray`、`Function`、`Symbol`、
  `Error/TypeError/RangeError/SyntaxError/ReferenceError`、`Set`、`Map`（`Set` 复用 `MAP` 以 `class_name` 区分，
  两者 `keys/values/entries` 与 `Symbol.iterator` 均走统一迭代协议）、
  `Promise`（`then/catch/finally/resolve/reject/all/race`）、`RegExp`（`test/exec` + `g` 下 `lastIndex`）、
  **`Date`**（`js/hpe_js_date.c`，Hinnant civil 日历算法；`get*/set*`+`toISOString/toJSON/toString/...`、
  静态 `now/parse/UTC`；实例 `JS_OBJ_DATE` 的 `num_val` 存 Unix 毫秒，`NaN` = 无效日期）、
  数组全量方法（`push…reduce/reduceRight/findLast/findLastIndex/copyWithin/sort/flat/flatMap/fill/at/
  keys/values/entries`）、字符串全量方法
  （`charAt/codePointAt…trim/trimStart/trimEnd/padStart/padEnd/repeat/split/replace/replaceAll/match/matchAll/
  search/startsWith/endsWith/localeCompare/at`，静态 `fromCharCode/fromCodePoint`）、
  `Number` 全量（`toFixed/toExponential/toPrecision/toLocaleString` + 静态 `isInteger/isFinite/isNaN/isSafeInteger`）。
- **正则引擎（`js/hpe_js_regex.c`）**：UTF-8 字节上的回溯匹配（CPS continuation 链）；字符类/范围、`.`、
  `* + ? {n,m}`（含惰性）、分组/捕获、反向引用、前瞻与后顾、锚点、`\b\B`、标志 `i/m/s`；
  `captures[0..1]` 为整匹配；有 `RE_MAX_STEPS/RE_MAX_DEPTH` 防病态回溯。
- **Promise / 生成器 / 模块**：Promise 同步 settle + 微任务队列（`hpe_js_drain_jobs`）；
  生成器**急切物化**（`js->yield_sink` 收集后按序吐 `{done,value}`）；
  ES 模块 `hpe_js_eval_module` + `HpeJsModuleLoader` 回调（引擎侧解析 `./x.js` 相对路径），
  `JsModule` 有 PENDING/RUNNING/DONE 状态机以处理循环引用。
- DOM 绑定（`js/hpe_js_dom.c`，通过 `HpeJsHostVt` 宿主接口，核心解释器不认识 DOM）：
  `document.getElementById/createElement/body/head`、**`document.querySelector(sel)`（首个匹配或 null）、
  `document.querySelectorAll(sel)`（数字键+length 的数组对象，`hpe_css_selector_matches` 逐元素匹配）**、
  `el.textContent`（读写）、`el.innerHTML`（读写）、
  `el.style.xxx`（驼峰转 CSS 属性，写回 style 属性）、`el.setAttribute/getAttribute/removeAttribute/hasAttribute/appendChild/remove`、
  **事件**：`el.onclick = fn`、`el.addEventListener(type, fn)`；派发 `mousedown`/`mouseup`/`click`/`mousemove`，
  事件对象 `{ type, clientX, clientY, target, currentTarget, preventDefault(),
  stopPropagation(), stopImmediatePropagation() }`，沿 DOM 祖先**冒泡**（stopPropagation 生效）。
- **DOM 变更 API（v0.12）**：`document.createDocumentFragment()`、`node.cloneNode(deep)`、
  `parent.insertBefore(newNode, ref)`（ref 为 null 时追加）、`parent.replaceChild(newNode, old)`（返回旧节点）、
  `node.hasChildNodes()`、`node.nodeType`（元素 1 / fragment 11）/ `tagName`·`nodeName`（大写）。
  节点/`childNodes`/`parentNode` 等返回 **host 包装对象**；包装器每次读取新建，`===` 同一性仅对同一次读取有效。
- **`MutationObserver`（v0.12，`js/hpe_js_mutation.c`）**：`new MutationObserver(cb)` +
  `observe(target, {childList,attributes,subtree,attributeFilter,attributeOldValue,characterData,
  characterDataOldValue})` / `takeRecords()` / `disconnect()`；`MutationRecord` 有
  `type/target/addedNodes/removedNodes/attributeName/oldValue`；投递为**微任务批量**（同一同步块内 N 次变更
  合并为 1 次回调）；`target` 是完整可用的元素包装器。DOM 变更经 `hpe_dom_add_mutation_sink` 订阅者广播。
- **浏览器全局（v0.7）**：`location`（`protocol/host/pathname/href/search/hash` + `reload/replace/assign`
  空实现）、`navigator`（`userAgent/platform/language`）、`window`/`self`
  （`innerWidth/innerHeight/devicePixelRatio/scrollX/scrollY/pageXOffset`、`addEventListener` 等空实现、
  `getComputedStyle` 返回空对象、`requestAnimationFrame` 返回 0）。引擎无网络无导航，
  这些对象只为让**普通页面脚本的兼容性守卫代码**不抛 `ReferenceError` 而中断
  （`copyResource/aim.html` 用到 `location.hash`）。
- **定时器（v0.4，`js/hpe_js_timers.c`）**：全局 `setTimeout/setInterval/clearTimeout/clearInterval`
  与 `requestAnimationFrame`；`HpeJs` 内 `JsTimer` 数组（id/repeating/is_raf/due/every/fn）；
  `hpe_js_run_timers(js, now, &fired)`（公开 API）逐轮泵到期定时器与 rAF 回调；
  引擎 `hpe_engine_tick()` 每帧调泵，fired>0 则 relayout；Win32 `SetTimer(15ms)` 驱动，无头用 `HOPE_TEST_WAIT`。
- 事件派发链路：平台层 Win32 `WM_LBUTTONDOWN/UP`（`WM_MOUSEMOVE` 以 button=-1）→ `HpeWindowCallbacks.on_mouse` →
  `hpe_engine_mouse_event`（点击）/ `hpe_engine_mouse_move`（:hover + mousemove，盒树命中测试 `hit_test_box` 反查 DOM 节点，
  fixed 盒按视口坐标比较）→ `hpe_js_dom_dispatch`（冒泡）。
  处理完若有监听器触发，引擎按变更级别 `hpe_engine_flush`（`none` 直接返回 / `paint` 仅重刷样式 /
  `layout` 局部子树增量重排），事件里的 DOM/style 改动即时反映到下一帧。
- 脚本来源：内联 `<script>` 与 `<script src="本地文件">`。

## 已知缺口（后续里程碑）
1. JS（v0.7 已补齐绝大多数语言特性，见上）：仍缺 `getBoundingClientRect`；
   节点 host 包装器每次读取新建，`===` 同一性不可靠（见上）；
   生成器为急切物化（不支持 `yield` 中途暂停与无限生成器）；
   `await` 同步 settle；无 `Proxy`/`Reflect`/`Intl`/`WeakMap`·`WeakSet`。
   事件：暂仅 `mousedown/mouseup/click/mousemove`（无 `mouseover`）。
   （已解决：try/catch/finally、原型链/class/extends/instanceof、`this`+call/apply/bind、解构/展开/默认与剩余参数、
   模板字符串、for-in/of、switch、标签、位运算、`?.`/`??`、getter/setter、正则、Promise/async await、生成器、
   ES 模块、定时器、`querySelector(All)`、`new`、`stopPropagation`/`preventDefault`、
   **v0.8 内置库**：`Date`、Map/Set 迭代、`new` 成员访问优先级修复、Array/String/Number/Math/Object 全量方法、
   **v0.12 DOM**：`cloneNode`/`insertBefore`/`replaceChild`/`DocumentFragment`/`MutationObserver`、
   节点包装器 `tagName` 大写、`document.head`、`removeAttribute`/`hasAttribute`）
2. 布局：见 `docs/design.md` §10.10（flex `baseline` 细节；grid `gap`/auto track/`span` 组合与
   table separate 模式的 12 条 XFAIL 边界；`flow-root`、跨容器 `clear`）。
   （已解决：grid/table 布局、flex wrap/shrink/order/stretch、元素级滚动容器、
   absolute 百分比 inset 精确解算、完整层叠上下文（v0.14–v0.18））
3. 绘制：`transition`/`animation` 插值时钟（仍仅解析存储为标志）、渐变多层/多色标高级语法。
   （已解决：`<img>` 四格式解码、滚动条 UI、`background-position/size`、子像素文字定位、
   彩色 emoji/字形回退链、**页面内文本选中与复制**、竖排文本 `writing-mode`；SVG 的
   `mask`/`clipPath`/`marker`/`filter`/`gradientTransform` 也已落地——见 `svg-fx.html` 基线）
4. 样式：渐变多层/多色标高级语法、`transform` 的 3D、`transition`/`animation` 插值时钟。
   （已解决：结构伪类、伪元素、状态伪类、`skew`/`matrix`、`cursor` 与平台光标联动（v0.20））
5. 基础设施：无跨平台后端（仅 Win32，**显式豁免项**）、真实系统时钟之外的单调时钟
   `hpe_time_monotonic`（**显式豁免项**）。
   （已解决：JS GC（`hpe_js_gc_*`，`js-gc.html` 断言 10/10）、C 单测框架 `hope_tests`
   （42 用例/223 断言）、性能基准入库 `tools/bench_run.py`+`bench_compare.py`、
   内存泄漏检测账本 `HPE_ALLOC_STATS=1|2` + `docs/memory-debug.md`；
   **v0.21 借账本修复四类泄漏**：emoji 精灵释放错字段（14 块/52KB）、竖排旋转掩码结构未释放
   （每字形 24B）、PNG 成功路径不解压缓冲、图片缓存键存嗅探后格式致永不命中反复解码；
   字体掩码缓存 `HPE_NO_MASK_CACHE=1` 旁路、脏矩形滚动 `HPE_NO_DIRTY=1` 旁路、
   `--bench N`/`--scroll-lines N`；
   **v0.12 DOM/CSS 增量更新**：变更分类（`NONE/PAINT/LAYOUT`）+ 脏标记惰性重排 + 局部子树增量布局
   （`hpe_layout_rebuild`，与全量逐字节一致）+ 脚本增改移除 `<style>` 即时生效；调试开关
   `HOPE_TRACE_REFLOW=1`、`HPE_FORCE_EAGER=1`、`HPE_NO_PARTIAL=1`）
6. 浏览器外壳：标签页无拖拽排序/会话持久化/中键关闭；IME 无组合窗定位（ImmSetCompositionWindow）；
   页面正文无插入符编辑（无 contenteditable）。
   （已解决：插入符闪烁与 I 形光标、前进/后退/刷新按钮、滚轮条带穿透、
   **v0.21**：多标签页（每标签历史/滚动 + Ctrl+T/W/Tab）、IME 组合输入
   （`WM_IME_COMPOSITION` → `on_ime_compose/commit` 回调，内联下划线组合串）、
   页面文本选中与复制（`HpeSelPos{node,off}` DOM 端点选区、拖选/双击选词/整行高亮/
   跨块复制写剪贴板；回归 `runtests.py` §3b sel-drag + §3c tabs））
7. HTML 解析：非完整 HTML5 状态机——未实现 `<frameset>`、`<form>` 隐式闭合细节、`<svg>`/`<math>`
   命名空间切换、`<textarea>`/`<title>` 的 RCDATA 规则、属性重复剔除、`<template>` 内容惰性解析；
   编码仅 UTF-8/UTF-16/Latin-1(cp1252)，无 GBK/Big5/Shift-JIS；`<link>` 资源提示只记录不加载。
   （**v0.13 已解决**：容错自动闭合、`<table>`/`<select>`/`<template>` 专用解析规则、
   字符编码探测、`<base href>` 与 `<link>` 其它 `rel`——详见「当前能力」。）

## 开发约定
- 纯 C11，不用 C++；头文件用 `hpe_`/`HPE_` 前缀，公共 API 在 `hope.h`。
- 平台相关代码只允许出现在 `src/platform/<os>/`，其余模块只能通过 `hpe_platform.h` 访问系统能力。
- 内存统一走 `hpe_alloc/hpe_calloc/hpe_free`。
- 函数注释按 docs/AI纯C语言注释规范.md 执行（Doxygen 风格 + 中英双语）。
- 单函数 ≤50 行（docs/AI代码生成规范.md）；超长函数用「表驱动分发 + 内聚辅助函数」拆分——
  「名字 → 处理函数」这类纯数据做成 `static const` 表，控制流只留一次查表。改后必须
  `python3 docs/func_len_audit.py 50` 为空且 `./docs/run_tests.sh` 仍 `ALL TESTS PASSED`。
