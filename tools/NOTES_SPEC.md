# 函数注释描述表（notes）编写规范

本文件供批量补齐 Doxygen 注释的作业使用。目标：为 `src/` 下每个 C 函数写一份
**中英双语**描述表，由 `tools/doxy_apply.py` 渲染成规范注释块插入源码。

## 产物位置

     tools/notes/<相对路径用 __ 连接>.json

例：

    src/render/hpe_pixmap.c   ->  tools/notes/render__hpe_pixmap.c.json
    src/css/hpe_css.c         ->  tools/notes/css__hpe_css.c.json

注意 **保留扩展名**（`.c` / `.h` 各一份，互不覆盖）。

## 文件结构

JSON 对象，键为**函数名**，值为描述对象：

```json
{
  "hpe_pixmap_init": {
    "brief": "初始化像素缓冲 / Initialise a pixel buffer",
    "details": "第一段 | 第二段",
    "params": { "pm": "inout:目标缓冲 / Target buffer" },
    "return": "成功返回 0 / 0 on success",
    "retval": { "-1": "参数非法 / Invalid arguments" },
    "note": "调用方随后必须调用 hpe_pixmap_free / Caller must call hpe_pixmap_free later",
    "warning": "禁止传 NULL / Must not be NULL",
    "see": "hpe_pixmap_free"
  }
}
```

字段说明：

| 字段 | 必填 | 说明 |
|---|---|---|
| `brief` | 是 | 一句话功能简述，「中文 / English」 |
| `details` | 否 | 补充说明，多段用 ` \| ` 分隔 |
| `params` | 视参数而定 | 参数名 → 说明；可写 `"in:说明 / Desc"` 覆盖方向 |
| `return` | 返回值非 void 时必填 | 「中文 / English」 |
| `retval` | 否 | 具体返回值 → 含义的映射 |
| `note` / `warning` / `see` | 否 | 注意事项 / 警告 / 相关函数 |

## 硬性规则

1. **双语**：每条描述都写成「中文 / English」，中间用 ` / ` 分隔（前后各一个空格）。
2. **参数完整性**：函数有参数时，`params` 必须覆盖**每一个具名参数**，键名与声明完全一致。
   漏参数或多参数都会让 `doxy_apply.py` 报错退出。无参函数可省略 `params`。
3. **全局参数词典兜底**：`tools/paramdict.json` 已覆盖全部常用参数名（如 `js`、`engine`、
   `node`、`out`、`s`、`pm`、`box`…）。若某参数名已在词典中，**可以省略**，渲染时自动取词典。
   只有当你要写**更贴合本函数语境**的说明时才在 `params` 里覆盖它。
4. **方向标注**：指针参数默认 `const` 指针→`in`、非 const 指针→`inout`、值参数→`in`。
   只有需要纠正时才手写前缀（`in:` / `out:` / `inout:`）。
5. **不要臆造**：只写源码能支撑的事实。返回值范围、NULL 约定、失败行为都应可从实现读出。
6. **不加代码**：描述里不要写完整代码块，可以用反引号引用标识符（如 `` `NULL` ``）。
7. **不要自己插入注释**：只产出 JSON 表；插入由 `doxy_apply.py` 统一完成。

## 工作方法

1. 先读 `tools/functions.json`，筛出目标文件的函数清单（含精确参数名与返回类型）：

       python -c "import json;d=json.load(open('tools/functions.json',encoding='utf-8'));[print(x['name'],[p['name'] for p in x['params']],x['return_type'],x['body_lines']) for x in d['defs'] if x['file']=='src/render/hpe_pixmap.c']"

2. 读源码（含函数体的注释与实现），理解每个函数真正的语义。
3. 写出 JSON 表。
4. **自检**（必做）：

       python tools/doxy_apply.py src/render/hpe_pixmap.c --dry

   - `skip=N` 表示还没有描述表的函数个数，**必须为 0**；
   - 若报 `undocumented params [...]` 或 `unknown params [...]`，按提示补齐；
   - 若某函数确实无法判断语义，也要写一个忠实的简述，不要留空，也不要从表里删掉。

## 风格示例

```json
"hpe_fill_rect": {
  "brief": "用纯色填充一个矩形 / Fill a rectangle with a solid colour",
  "params": {
    "pm": "inout:目标像素缓冲 / Target pixel buffer",
    "x": "矩形左边界 / Rectangle left edge",
    "y": "矩形上边界 / Rectangle top edge",
    "w": "矩形宽度 / Rectangle width",
    "h": "矩形高度 / Rectangle height",
    "color": "填充颜色 / Fill colour"
  },
  "note": "坐标与尺寸按设备像素取整后写入 / Coordinates and sizes are rounded to device pixels"
}
```

```json
"hpe_js_run_timers": {
  "brief": "泵到期定时器与 rAF 回调 / Drain due timers and rAF callbacks",
  "params": {
    "now": "当前时间（毫秒）/ Current time in milliseconds",
    "fired": "out:实际触发的回调个数 / Number of callbacks that ran"
  },
  "return": "触发数大于 0 返回 1 / 1 when at least one callback ran",
  "note": "每轮只处理到期的定时器，期间新增的等下一轮 / Only callbacks already due are run; ones added during the pass wait for the next round"
}
```
