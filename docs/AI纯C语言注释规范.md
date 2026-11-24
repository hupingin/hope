```markdown
# C语言函数注释规范（中英双语，Doxygen风格）

## 一、格式要求

- 采用 **Doxygen 风格**文档注释，以 `/**` 开头，`*/` 结尾，禁止用普通 `/*` 或 `//` 作为函数文档注释。
- 每个函数必须有注释，**中文在前、英文在后**，用 ` / ` 分隔，全文保持一致。
- 标签顺序固定：`@brief` → `@details` → `@param` → `@return` → `@retval` → `@note` → `@warning` → `@see`。
- 指针参数必须标注方向：`@param[in]`、`@param[out]`、`@param[in,out]`。
- 必须写明参数约束（NULL、范围、单位等）和返回值含义。

## 二、模板

```c
/**
 * @brief 函数功能简述 / Brief description of the function
 *
 * @details 详细说明（可选）/ Detailed description (optional)
 *
 * @param[in]     param1 参数1说明 / Description of param1
 * @param[out]    param2 参数2说明 / Description of param2
 * @param[in,out] param3 参数3说明 / Description of param3
 *
 * @return 返回值说明 / Description of return value
 *
 * @retval  0 成功 / Success
 * @retval -1 失败 / Failure
 *
 * @note 注意事项 / Notes
 * @warning 警告信息 / Warning
 * @see 相关函数 / Related functions
 */
```

## 三、示例

### 示例1：简单函数

```c
/**
 * @brief 计算两个整数的和 / Calculate the sum of two integers
 *
 * @param a 第一个加数 / The first addend
 * @param b 第二个加数 / The second addend
 *
 * @return 两数之和 / The sum of a and b
 */
int add(int a, int b)
{
    return a + b;
}
```

### 示例2：带错误码与指针参数

```c
/**
 * @brief 在数组中查找指定元素 / Search for a specified element in an array
 *
 * @details 采用线性查找，返回第一个匹配元素的下标。
 *          Uses linear search, returns the index of the first match.
 *
 * @param[in]  arr    待查找数组 / The array to search
 * @param[in]  len    数组长度 / Length of the array
 * @param[in]  target 目标元素 / The target element
 * @param[out] index  匹配元素下标（成功时写入）/ Index of matched element (written on success)
 *
 * @return 查找结果 / Search result
 * @retval  0 找到 / Found
 * @retval -1 未找到或参数非法 / Not found or invalid arguments
 *
 * @note arr 不能为 NULL，len 必须大于 0。
 *       arr must not be NULL, and len must be greater than 0.
 *
 * @warning 调用者需保证 index 指针有效。
 *          Caller must ensure the index pointer is valid.
 */
int find_element(const int *arr, size_t len, int target, size_t *index);
```

### 示例3：回调参数

```c
/**
 * @brief 对数组每个元素执行回调函数 / Apply a callback to each element of the array
 *
 * @param[in,out] arr 数组指针，元素可被回调修改
 *                     Array pointer, elements may be modified by the callback
 * @param[in]     len 元素个数 / Number of elements
 * @param[in]     cb  回调函数指针，签名为 void (*)(int *)
 *                     Callback function pointer with signature void (*)(int *)
 *
 * @return 实际处理的元素个数 / Number of elements processed
 *
 * @see sort_array
 */
size_t foreach(int *arr, size_t len, void (*cb)(int *));
```

## 四、要点速查

| 要点 | 说明 |
|------|------|
| 注释起始 | `/**` 开头，禁止 `/*` 或 `//` |
| 双语顺序 | 中文在前、英文在后，用 ` / ` 分隔 |
| 参数方向 | `[in]` / `[out]` / `[in,out]` 必须标注 |
| 标签顺序 | `@brief`→`@details`→`@param`→`@return`→`@retval`→`@note`→`@warning`→`@see` |
| 对齐 | 中英文间空格、标签后空格，保持整齐 |
| 约束 | NULL、范围、单位必须写明 |
| 工具兼容 | Doxygen / VS Code / CLion / clang-format 均可解析 |
```