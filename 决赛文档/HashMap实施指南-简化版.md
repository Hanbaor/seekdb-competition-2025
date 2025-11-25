# HashMap 方案实施指南（简化版）

## 已完成的工作

### 1. 创建了HashMap数据结构

**文件**：
- `src/sql/das/ob_fts_scalar_cache.h` - 头文件
- `src/sql/das/ob_fts_scalar_cache.cpp` - 实现文件
- `src/sql/das/ob_fts_scalar_cache_init.cpp` - 初始化工具

**核心类**：
```cpp
class ObFtsScalarCache {
  // HashMap: doc_id → ScalarFields
  ObHashMap<uint64_t, ScalarFields> mapping_;
  
  // 方法
  int init(int64_t capacity);
  int add_mapping(uint64_t doc_id, uint64_t id, 
                  const ObString &base_id, const ObString &docid_col);
  int get_scalar_fields(uint64_t doc_id, ScalarFields &fields);
};

// 全局实例
extern ObFtsScalarCache g_fts_scalar_cache;
```

---

## 下一步：修改查询代码

### 修改 ob_das_text_retrieval_iter.cpp

需要在 `inner_get_next_row()` 函数中添加HashMap查询和过滤逻辑：

```cpp
int ObDASTextRetrievalIter::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  
  // 原有的初始化检查...
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    return ret;
  }
  
  // ========== 新增：循环扫描 + 过滤 ==========
  while (OB_SUCC(ret)) {
    // 原有：扫描倒排索引
    if (OB_FAIL(inverted_idx_scan_iter_->get_next_row())) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
        LOG_WARN("failed to get next row from inverted index", K(ret));
      }
      break;
    }
    
    // ========== 新增：获取 doc_id ==========
    ObDocIdExt doc_id_ext;
    if (OB_FAIL(get_inv_idx_scan_doc_id(doc_id_ext))) {
      LOG_WARN("failed to get doc_id", K(ret));
      return ret;
    }
    uint64_t doc_id = doc_id_ext.get_datum().get_uint64();
    
    // ========== 新增：查询 HashMap ==========
    ScalarFields fields;
    if (OB_FAIL(g_fts_scalar_cache.get_scalar_fields(doc_id, fields))) {
      if (OB_HASH_NOT_EXIST == ret) {
        LOG_DEBUG("doc_id not in cache, skip", K(doc_id));
        ret = OB_SUCCESS;
        continue;  // 跳过这个 doc_id
      } else {
        LOG_WARN("failed to get scalar fields", K(ret), K(doc_id));
        return ret;
      }
    }
    
    LOG_DEBUG("Got scalar fields from cache", K(doc_id), K(fields));
    
    // ========== 新增：应用标量过滤条件 ==========
    // 过滤条件1：id < 1000
    if (fields.id >= 1000) {
      LOG_DEBUG("filtered by id", K(doc_id), K(fields.id));
      continue;  // 跳过
    }
    
    // 过滤条件2：base_id IN ('base_1', 'base_2')
    // 这里硬编码测试，实际应该从条件表达式中获取
    if (fields.base_id != "base_1" && fields.base_id != "base_2") {
      LOG_DEBUG("filtered by base_id", K(doc_id), K(fields.base_id));
      continue;  // 跳过
    }
    
    LOG_INFO("Passed all filters", K(doc_id), K(fields));
    
    // ========== 新增：设置输出字段（替代回表）==========
    ObEvalCtx *eval_ctx = ir_rtdef_->get_inv_idx_scan_rtdef()->eval_ctx_;
    
    // 设置 docid_col
    // 需要找到 docid_col_expr_ 并设置
    // TODO: 添加 docid_col_expr_ 的设置代码
    
    // 原有：计算 relevance（如果需要）
    if (ir_ctdef_->need_calc_relevance()) {
      clear_row_wise_evaluated_flag();
      if (OB_FAIL(get_next_doc_token_cnt(need_fwd_idx_agg_))) {
        LOG_WARN("failed to get next doc token count", K(ret));
      } else if (OB_FAIL(fill_token_doc_cnt())) {
        LOG_WARN("failed to get token doc cnt", K(ret));
      } else if (OB_FAIL(project_relevance_expr())) {
        LOG_WARN("failed to evaluate simarity expr", K(ret));
      }
    }
    
    // ========== 通过所有过滤，返回这一行 ==========
    return OB_SUCCESS;
  }
  
  return ret;
}
```

---

## 手动测试步骤

### 步骤1：编译新代码

```bash
cd /home/tian/seekdb
./build.sh
```

### 步骤2：手工初始化HashMap

在查询前，需要调用初始化函数填充HashMap。

**选项A：在代码中硬编码调用**

找到一个合适的位置（如服务器启动或首次查询时）调用：

```cpp
#include "sql/das/ob_fts_scalar_cache_init.h"

// 在某个初始化函数中
init_fts_scalar_cache_for_items1();
```

**选项B：使用 GDB 手动调用**

```bash
# 启动 observer
gdb ./observer

# 设置断点
b ObDASTextRetrievalIter::inner_get_next_row

# 运行
run

# 当断点触发时，手动调用初始化
call init_fts_scalar_cache_for_items1()

# 继续执行
continue
```

### 步骤3：执行测试查询

```sql
SELECT docid_col, MATCH(fulltext_col) AGAINST('test') as _score
FROM items1
WHERE MATCH(fulltext_col) AGAINST('test')
  AND id < 1000
  AND base_id IN ('base_1', 'base_2')
ORDER BY _score DESC
LIMIT 10;
```

### 步骤4：观察日志

查看日志确认HashMap是否工作：

```bash
grep "Got scalar fields from cache" observer.log
grep "Passed all filters" observer.log
grep "filtered by" observer.log
```

---

## 性能对比测试

### 测试1：不使用HashMap（传统回表）

```sql
-- 记录执行时间
SELECT COUNT(*) FROM items1 
WHERE MATCH(fulltext_col) AGAINST('test')
  AND id < 1000;
```

### 测试2：使用HashMap（无回表）

```sql
-- 初始化HashMap后，再次执行
SELECT COUNT(*) FROM items1 
WHERE MATCH(fulltext_col) AGAINST('test')
  AND id < 1000;
```

**预期结果**：
- 传统方式：~100秒（10ms × 10,000次回表）
- HashMap方式：~1秒（0.1ms × 10,000次内存查询）

---

## 当前状态总结

### ✅ 已完成
1. HashMap数据结构实现
2. 全局单例创建
3. 添加映射API
4. 查询映射API
5. 头文件引入查询代码

### ⏳ 待完成
1. 完整修改 inner_get_next_row() - 添加while循环和过滤
2. 屏蔽回表逻辑（在 ObDASFuncDataIter 中）
3. 设置输出字段的完整代码
4. 编译和测试

### 🔧 需要手动调整的地方
1. 过滤条件：目前硬编码 `id < 1000` 和 `base_id IN ('base_1', 'base_2')`
2. 输出字段设置：需要找到正确的 ObExpr 并设置
3. 初始化调用：需要找到合适的调用位置

---

## 下一步建议

### 快速验证路径
1. 先不修改 inner_get_next_row()，只在启动时初始化HashMap
2. 使用 GDB 确认HashMap已填充
3. 在查询代码中添加简单的HashMap查询日志
4. 逐步添加过滤和输出逻辑

### 完整实现路径
1. 完成 inner_get_next_row() 的完整修改
2. 修改 ObDASFuncDataIter 屏蔽回表
3. 实现从实际数据库表填充HashMap的逻辑
4. 添加持久化支持
5. 性能测试和优化

---

## 关键文件清单

```
已创建的新文件：
├─ src/sql/das/ob_fts_scalar_cache.h          - HashMap头文件
├─ src/sql/das/ob_fts_scalar_cache.cpp        - HashMap实现
├─ src/sql/das/ob_fts_scalar_cache_init.cpp   - 初始化工具
└─ 决赛文档/HashMap实施指南-简化版.md         - 本文档

需要修改的现有文件：
├─ src/sql/das/iter/ob_das_text_retrieval_iter.cpp  - 查询逻辑（已添加头文件）
└─ src/sql/das/iter/ob_das_func_data_iter.cpp       - 回表屏蔽（待修改）
```

---

## 需要帮助？

如果需要：
1. 完成 inner_get_next_row() 的完整修改
2. 屏蔽回表的具体代码
3. 从数据库自动填充HashMap的实现
4. 编译和调试支持

请告诉我，我会继续实现！
