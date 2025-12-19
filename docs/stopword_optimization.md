# StopWord 检查优化文档

## 概述

本文档描述了 `ObStopWordChecker::check_stopword` 函数的性能优化，通过避免不必要的字符集转换，显著降低了停用词检查的 CPU 开销。

## 问题背景

### 原始实现

原始的 `check_stopword` 函数在每次调用时都会执行字符集转换：

```cpp
int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
    // 每次调用都创建 allocator
    ObArenaAllocator allocator(...);
    
    // 无条件执行字符集转换
    ObCharset::charset_convert(allocator, word, word_cs_type, stopword_cs_type, cmp_str);
    
    // 构造临时对象后查找
    ObFTWord converted(...);
    stopword_set_.exist_refactored(converted);
}
```

### 性能问题

通过日志分析发现：
- `word_cs_type = 224` (CS_TYPE_UTF8MB4_UNICODE_CI)
- `stopword_cs_type = 45` (CS_TYPE_UTF8MB4_GENERAL_CI)

由于两者类型不同，**100% 的调用都走慢路径**，即使：
1. 两种 collation 对于 ASCII 停用词的字节表示完全相同
2. 转换操作实际上只是内存拷贝，没有实际的编码转换

### 开销分析

单次调用的额外开销：

| 操作 | 说明 |
|------|------|
| `ObArenaAllocator` 创建 | 内存分配器初始化 |
| `charset_convert` 调用 | 函数调用 + 内存拷贝 |
| `ObFTWord` 临时对象构造 | 对象初始化 |
| `ObArenaAllocator` 析构 | 内存释放 |

在 MLDR 测试中，`check_stopword` 被调用约 **45,000 次**，累计开销显著。

## 优化方案

### 核心思路

将 stopword 的 collation type 设置为与查询 word 相同的类型，使字符集匹配时可以直接查找，跳过转换步骤。

### 代码修改

#### 1. 修改 stopword 初始化 (ob_fts_stop_word.cpp)

```cpp
// 修改前
stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

// 修改后：使用与查询 word 匹配的 collation type
stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_UNICODE_CI);
```

#### 2. 添加快速路径判断 (ob_fts_stop_word.cpp)

```cpp
int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
    const ObCollationType word_cs_type = word.get_collation_type();
    const ObCollationType stopword_cs_type = stopword_type_.get_collation_type();
    
    if (word_cs_type == stopword_cs_type) {
        // 快速路径：字符集相同，直接查找
        ret = stopword_set_.exist_refactored(word);
        // ... 处理结果
    } else {
        // 慢路径：字符集不同，需要转换（保持兼容性）
        ObArenaAllocator allocator(...);
        ObCharset::charset_convert(...);
        // ...
    }
}
```

## 优化效果

### 路径命中统计

| 指标 | 优化前 | 优化后 |
|------|--------|--------|
| 快速路径命中 | 0% | 100% |
| 慢速路径命中 | 100% | 0% |

### 性能对比

| 操作 | 优化前 | 优化后 |
|------|--------|--------|
| 内存分配 | 每次 | 无 |
| 字符集转换 | 每次 | 无 |
| 临时对象构造 | 每次 | 无 |
| HashSet 查找 | 1次 | 1次 |

## 兼容性说明

1. **保留慢路径**：当 word 的 collation type 与 stopword 不同时，仍会走原有的转换逻辑，确保功能正确性。

2. **Collation 选择**：当前固定使用 `CS_TYPE_UTF8MB4_UNICODE_CI`，如果未来查询使用其他 collation，可能需要动态适配。

## 相关文件

- `src/storage/fts/ob_fts_stop_word.cpp` - 主要修改文件
- `src/storage/fts/ob_fts_stop_word.h` - 头文件

## 验证方法

可通过添加临时计数器验证优化效果：

```cpp
static std::atomic<int64_t> g_fast_path_cnt{0};
static std::atomic<int64_t> g_slow_path_cnt{0};

// 在快速/慢速路径分别递增
g_fast_path_cnt.fetch_add(1);
g_slow_path_cnt.fetch_add(1);

// 定期打印
LOG_INFO("stopword stats", K(g_fast_path_cnt), K(g_slow_path_cnt));
```

## 总结

通过匹配 stopword 与查询 word 的 collation type，避免了不必要的字符集转换开销，在高频调用场景下可显著提升性能。
