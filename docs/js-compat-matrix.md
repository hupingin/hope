# Hope UI Engine — JS 兼容性矩阵

> 引擎版本基线：**v0.13**（`docs/需求.md` 最后更新 2026-11-24）
> 本表按 `docs/需求.md` §7 各小节整理，并抽样回源码核对（`src/js/` 各文件）。
> 状态图例：✅ 支持 ｜ 🟡 部分支持（缺口见说明）｜ ❌ 不支持

**统计：114 支持 / 3 部分支持 / 9 不支持，共 126 条**

---

## 1. 词法与解析

| 特性 | 说明 | 状态 |
|---|---|---|
| 手写词法 | 数字 / 字符串 / `\u` 转义 / 注释 / 正则字面量 | ✅ |
| 模板字符串 | 含 `${}` 嵌套表达式 | ✅ |
| 自动分号插入（ASI） | — | ✅ |
| 递归下降解析 | AST 存 arena | ✅ |
| 错误报告 | 语法 / 运行时错误带行号 | ✅ |
| 解析护栏 | 表达式嵌套上限 400 层 | ✅ |

## 2. 运行时基础

| 特性 | 说明 | 状态 |
|---|---|---|
| 树遍历解释器 + arena 堆 | 整块释放 | ✅ |
| 异常传播 | `setjmp/longjmp`（`try_top` 活动帧链） | ✅ |
| 垃圾回收 | Boehm 式保守 mark-sweep；根含栈 / 寄存器 / 定时器表 / DOM 监听器 / 协程栈；`__gc()` / `__gcStats()` 调试内建 | ✅ |
| 严格模式 | `"use strict"` 识别与传播、类体恒严格、拒绝未声明赋值 / `with` / 重复参数名 / `eval`·`arguments` 绑定等；严格调用 `this` 为 undefined | 🟡 |
| 深递归栈保护 | `call_depth` 上限 300（约 2-3KB C 栈/层），超限抛 `Maximum call stack size exceeded` | ✅ |

严格模式缺口（对应 🟡）：八进制字面量拒绝、保留字标识符拒绝、`arguments.callee`（需求 §7.1 #206）。

## 3. 语句

| 特性 | 状态 |
|---|---|
| `var` / `let` / `const` | ✅ |
| 函数声明与函数表达式 | ✅ |
| `class` / `extends` / `super`（getter/setter、静态成员、实例字段、私有 `#name`、静态初始化块） | ✅ |
| `if` / `else` / `while` / `do-while` / `for` | ✅ |
| `for-in` / `for-of` | ✅ |
| `switch`（含落空穿透） | ✅ |
| `break` / `continue` + 标签 | ✅ |
| `return` / `throw` | ✅ |
| `try` / `catch` / `finally` | ✅ |
| `with` 语句（对象环境注入、沿原型链查找） | ✅ |
| `debugger` 语句（无调试器时空操作） | ✅ |
| `import` / `export`（ES 模块） | ✅ |
| 动态 `import()`（返回 Promise） / `import.meta` | ✅ |

## 4. 表达式与运算符

| 特性 | 状态 |
|---|---|
| 全部算术 / 比较 / 逻辑运算符 | ✅ |
| 位运算 `&` `\|` `^` `~` `<<` `>>` `>>>` | ✅ |
| `**` 幂运算 | ✅ |
| 三元 / 逗号运算符 | ✅ |
| `typeof` / `instanceof` / `in` / `delete` / `void` | ✅ |
| 可选链 `?.` | ✅ |
| 空值合并 `??` / `??=` | ✅ |
| 复合赋值（含 `&&=` / `\|\|=`） | ✅ |
| 自增自减 | ✅ |
| 成员访问与方法调用、`new` | ✅ |
| `this` 与 `call` / `apply` / `bind` | ✅ |
| 解构（数组 / 对象 / 嵌套 / 默认 / 剩余） | ✅ |
| 展开 `...`（调用参数、数组 / 对象字面量） | ✅ |
| 默认参数与剩余参数 | ✅ |

## 5. 函数进阶

| 特性 | 说明 | 状态 |
|---|---|---|
| 箭头函数 | 独立词法单元，表达式体脱糖，不建自己的 `this` | ✅ |
| 生成器 `function*` | 函数体跑在平台协程上（`hpe_js_gen.c`），支持无限生成器、`yield*` 委派、`throw()` / `return()` 及 `finally` 展开 | ✅ |
| `async` / `await` | 事件循环挂起（`hpe_js_async.c`），`await` 不阻塞调用方，`try`/`finally` 跨 `await` 正常 | ✅ |
| `new.target` | 构造 / 普通调用、箭头继承、`super()` 传播 | ✅ |
| 标签模板 | cooked/raw 冻结数组、`String.raw`、方法标签的 `this` | ✅ |

## 6. 知名符号（Well-Known Symbols）

| 特性 | 说明 | 状态 |
|---|---|---|
| `Symbol.toPrimitive` / `toStringTag` / `hasInstance` / `unscopables` / `isConcatSpreadable` / 自定义 `iterator` / 计算名 | 内部槽已建模，符号按真实 symbol 身份（`JS_SYMBOL` 原语） | ✅ |
| `Symbol.species` / `match`·`replace`·`search`·`split` 委托 / `Symbol.asyncIterator` | 未实现（需求 §7.3 #244 未覆盖说明） | ❌ |

## 7. 内置对象

| 特性 | 说明 | 状态 |
|---|---|---|
| `console.*` | log / info / warn / error | ✅ |
| `console` 扩展 | table / time / timeEnd / count | ✅ |
| `alert` | — | ✅ |
| `Math.*` | 常量 + 全量函数（含 `random` xorshift64、`clz32` / `imul` / `fround` / `log1p` / `expm1` / `acosh` / `asinh` / `atanh`） | ✅ |
| `JSON` | stringify / parse | ✅ |
| 全局函数 | `parseInt` / `parseFloat` / `isNaN` / `isFinite` | ✅ |
| URI 编解码 | `encodeURI(Component)` / `decodeURI(Component)` | ✅ |
| 老式全局 | `escape` / `unescape`（含代理对与 `%uXXXX`） | ✅ |
| `Object` | keys / values / entries / fromEntries / assign / freeze / seal / isFrozen / isSealed / create / defineProperty / getOwnPropertyNames / hasOwn / getPrototypeOf / setPrototypeOf / is | ✅ |
| `Array` | from / of / isArray + 全量方法（push…reduce / reduceRight / findLast / findLastIndex / copyWithin / sort / flat / flatMap / fill / at / keys / values / entries） | ✅ |
| `Function` | `new Function(params, body)` 走解析器编译 | ✅ |
| `Symbol` | 普通符号 + 知名符号建模 | ✅ |
| Error 家族 | Error / TypeError / RangeError / SyntaxError / ReferenceError | ✅ |
| `Set` | add / has / delete / clear / forEach / values / keys / entries / size | ✅ |
| `Map` | set / get / has / delete / clear / forEach / keys / values / entries / size | ✅ |
| 统一迭代协议 | Map / Set / Array / String / arguments / NodeList 走 `Symbol.iterator`；Map/Set 迭代器 `.next()` 惰性游标 | ✅ |
| `Promise` | then / catch / finally / resolve / reject / all / race | ✅ |
| `RegExp` | test / exec + `g` 下 `lastIndex` | ✅ |
| `Date` | `get*/set*` + `getUTC*/setUTC*`、`toISOString` / `toJSON` / `toString` / `toLocale*` / `valueOf`、静态 now / parse / UTC；Hinnant civil 日历 + 历史时区 / 夏令时 | ✅ |
| `Number` | toFixed / toExponential / toPrecision / toLocaleString + 静态 isInteger / isFinite / isNaN / isSafeInteger | ✅ |
| `String` | 全量方法（charAt / codePointAt…trim / trimStart / trimEnd / padStart / padEnd / repeat / split / replace / replaceAll / match / matchAll / search / startsWith / endsWith / localeCompare / at）+ 静态 fromCharCode / fromCodePoint | ✅ |
| `Intl` | NumberFormat / DateTimeFormat / Collator（`hpe_js_intl.c`） | ✅ |
| `Proxy` / `Reflect` | Proxy 八种捕获器；Reflect 全部静态方法 | ✅ |
| `WeakMap` / `WeakSet` / `WeakRef` / `FinalizationRegistry` | — | ✅ |
| `BigInt` | `hpe_js_bigint.c` | ✅ |
| `ArrayBuffer` / TypedArray / `DataView` | `hpe_js_buffer.c` | ✅ |

## 8. 正则引擎（`hpe_js_regex.c`）

| 特性 | 说明 | 状态 |
|---|---|---|
| 匹配模型 | UTF-8 字节上的回溯匹配（CPS continuation 链） | ✅ |
| 字符 / 字符类 / 范围 / `.` | — | ✅ |
| 量词 | `* + ? {n} {n,} {n,m}`，含惰性 `*?` 等 | ✅ |
| 分组与捕获组 | — | ✅ |
| 反向引用 | `\1`..`\9` | ✅ |
| 环视 | 前瞻 `(?=)` / `(?!)`、后顾 `(?<=)` / `(?<!)` | ✅ |
| 锚点 | `^` `$`、词边界 `\b` `\B` | ✅ |
| 标志 | `i` / `m` / `s` | ✅ |
| 标志 | `u`（Unicode）/ `y`（sticky）/ `d`（`indices`，含具名 `indices.groups`） | ✅ |
| 具名捕获组 | `(?<name>...)` 与 `groups` | ✅ |
| Unicode 属性类 | `\p{...}` / `\P{...}`，类内外皆可 | 🟡 |
| 安全护栏 | `RE_MAX_STEPS` / `RE_MAX_DEPTH` 防病态回溯 | ✅ |

`\p{...}` 缺口（对应 🟡）：覆盖常用大类（`L`/`Lu`/`Ll`/`N`/`Nd`/`P`/`S`/`Z`/`Zs` 与 `Letter`/`Number`/`ASCII`/`White_Space`/`Punctuation`/`Symbol` 等），非全部 Unicode 属性（需求 §7.5）。

## 9. DOM 绑定与事件（`hpe_js_dom.c`）

| 特性 | 说明 | 状态 |
|---|---|---|
| `document` 基础 | getElementById / createElement / body / createTextNode / createDocumentFragment | ✅ |
| 选择器查询 | querySelector / querySelectorAll | ✅ |
| 内容读写 | textContent / innerHTML | ✅ |
| 内联样式 | `el.style.xxx`（驼峰转 CSS 属性写回） | ✅ |
| 属性与树操作 | setAttribute / getAttribute / appendChild / remove | ✅ |
| 扩展属性 | classList / dataset / innerText | ✅ |
| 遍历属性 | children / childNodes / parentNode / nextSibling / firstElementChild 等 | ✅ |
| 布局读取 | offsetWidth/Height/Left/Top、clientWidth/Height | ✅ |
| `getBoundingClientRect` | 视口坐标，先强制排版 | ✅ |
| `tagName` | 规范大写 | 🟡 |
| `window.getComputedStyle` | 返回真实计算样式（源码含 `getPropertyValue` 实现） | ✅ |
| 事件绑定 | onclick / addEventListener / removeEventListener、`once` / `capture` 选项 | ✅ |
| 事件对象 | type / clientX / clientY / target / currentTarget / preventDefault / stopPropagation / stopImmediatePropagation | ✅ |
| 事件传播 | 沿 DOM 祖先冒泡 + 完整捕获阶段 | ✅ |
| 鼠标事件 | mousedown / mouseup / click / mousemove | ✅ |
| 键盘与滚轮 | keydown / keyup / keypress、wheel / scroll | ✅ |
| 进入离开 | mouseover / mouseout / mouseenter / mouseleave（enter/leave 不冒泡） | ✅ |
| 自定义事件 | dispatchEvent(ev) 与 `new Event(type, { bubbles })` | ✅ |
| 焦点管理 | focus / blur / activeElement | ✅ |

`tagName` 缺口（对应 🟡）：需求 §7.6 条目注记「当前返回小写」。

## 10. 浏览器全局

| 特性 | 说明 | 状态 |
|---|---|---|
| `location` | protocol / host / pathname / href / search / hash；`reload` / `replace` / `assign` 为空实现 | ✅ |
| `navigator` | userAgent / platform / language | ✅ |
| `window` / `self` | innerWidth / innerHeight / devicePixelRatio / scrollX / scrollY / pageXOffset | ✅ |
| `history` | pushState / replaceState 记内部 URL 栈；back / forward / go 空实现 | ✅ |
| `localStorage` / `sessionStorage` | 键值读写，两区互不干扰，属性读写与 `*Item` 互为镜像 | ✅ |
| `performance.now()` | `window.console` / `window.performance` 与裸全局同对象 | ✅ |

## 11. 定时器与事件循环（`hpe_js_timers.c`）

| 特性 | 说明 | 状态 |
|---|---|---|
| `setTimeout` / `setInterval` / `clearTimeout` / `clearInterval` | — | ✅ |
| `requestAnimationFrame` | 引擎每帧调泵，有触发则 relayout | ✅ |
| 微任务 / 宏任务调度 | 宏任务后立即排空微任务检查点 | ✅ |
| 4ms 钳制 | 嵌套层数 > 5 且延迟 < 4ms 时抬到 4ms（HTML 规范） | ✅ |
| 泵语义 | 一遍泵内新排定时器等下一遍（`seq` 快照）；同刻到期按创建顺序；`HPE_TIMER_MAX_PER_PUMP` 防自递归挂帧 | ✅ |

## 12. ES 模块

| 特性 | 状态 |
|---|---|
| `import` / `export`（默认导出、具名导出、相对路径解析） | ✅ |
| `HpeJsModuleLoader` 回调（引擎侧解析 `./x.js`） | ✅ |
| `JsModule` PENDING / RUNNING / DONE 状态机（循环引用） | ✅ |
| `export * from` / `import * as ns` / 重命名导出 `{ a as b }` | ✅ |
| `import.meta.url`（含 `hpe_js_set_entry_path`） | ✅ |

## 13. 明确不支持

| 特性 | 说明 | 状态 |
|---|---|---|
| 网络 API | fetch / XHR / WebSocket —— 项目硬约束「无网络能力」（需求 §0） | ❌ |
| Worker / 多线程 | — | ❌ |
| 真尾调用（PTC） | 采用与 V8 一致的深递归栈保护而非 PTC（需求 §7.1 #207） | ❌ |
| `Symbol.species` | 需求 §7.3 #244 未覆盖说明 | ❌ |
| `match` / `replace` / `search` / `split` 符号委托 | 同上 | ❌ |
| `Symbol.asyncIterator` / 异步迭代 | 同上 | ❌ |
| 严格模式细节 | 八进制字面量拒绝、保留字标识符拒绝、`arguments.callee`（需求 §7.1 #206 未覆盖说明） | ❌ |
| `@stream` 等草案语法 / 其余未列出的全局与 API | 需求清单未覆盖 | ❌ |

---

## 源码抽样核对记录（≥20% 条目）

以下条目经关键词回源码确认存在：

- **内置对象**：`toFixed` / `findLast` / `padStart` / `replaceAll` / `matchAll` / `codePointAt` / `copyWithin`（`hpe_js_interp.c` 各 3-5 处）；`fround` / `clz32` / `expm1`；`toLocaleString`（5 处）
- **知名符号**：`toPrimitive`、`unscopables`、`hasInstance`、`isConcatSpreadable`（`hpe_js_interp.c` / `hpe_js_value.c`）；负向确认 `species` / `asyncIterator`（0 处）→ 对应 ❌ 条目
- **模块 / 语句**：`import.meta`（6 处）、`debugger`（`hpe_js_parser.c:4163` `p_debugger_stmt`）、`with`（parser 关键字表）
- **进阶特性**：`BigInt`（34 处）、`WeakRef` / `FinalizationRegistry`（14/13 处）、`new.target`（parser + interp `new_target` 12 处）
- **正则**：`(?<`（具名组，6 处）、`\p{`、`sticky`、`unicode`、`indices`（`hpe_js_regex.c`）
- **DOM**：`getComputedStyle` 实现（`hpe_js_dom.c:3644`，含 `getPropertyValue`）
- **文件级确认**：`hpe_js_bigint.c` / `hpe_js_buffer.c` / `hpe_js_intl.c` / `hpe_js_gen.c` / `hpe_js_async.c` / `hpe_js_symbol.c` 等独立模块与需求条目一一对应
