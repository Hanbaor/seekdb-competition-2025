# HashMap 实现代码评估报告

## 1. 整体评估：⚠️ 部分正确，但有严重问题

### 评分：5/10

**主要问题**：
1. ❌ **HashMap 填充位置错误** - 在查询时填充而非索引构建时
2. ❌ **字段识别逻辑脆弱** - 依赖列的顺序和类型推断
3. ⚠️ **SELECT 列添加正确** - 但填充逻辑完全错误
4. ⚠️ **缺少 doc_id 提取** - 没有从 `DOC_ID()` 表达式获取 doc_id

---

## 2. 文件1：ob_ddl_common.h & ob_ddl_common.cpp

### ✅ 正确部分

**函数声明和实现（ob_ddl_common.h:1109, ob_ddl_common.cpp:921）**：
```cpp
static int append_fts_scalar_columns(
    const share::schema::ObTableSchema &dest_table_schema,
    const share::schema::ObTableSchema &source_table_schema,
    ObArray<ObColumnNameInfo> &column_names,
    ObArray<int64_t> &select_column_ids);
```

**优点**：
- ✅ 正确添加了函数接口
- ✅ 在 `generate_build_replica_sql()` 中正确调用（第1140行）
- ✅ 只对 FTS 索引辅助表生效（第928行）
- ✅ 跳过隐藏列、生成列、TEXT列（第946-963行）
- ✅ 避免重复添加已有列（第955行）

**实现逻辑评估 - ✅ 基本正确**：
```cpp
// 只对 fts_index_aux 和 fts_doc_word 辅助表生效
if (share::schema::is_fts_index_aux(dest_table_schema.get_index_type()) ||
    share::schema::is_fts_doc_word_aux(dest_table_schema.get_index_type())) {
  
  // 遍历主表的所有列
  for (int64_t i = 0; i < src_column_ids.count(); ++i) {
    // 跳过不需要的列
    if (is_hidden || is_generated || is_text_type || already_in_select) {
      continue;
    }
    
    // 添加到 SELECT 子句
    column_names.push_back(col_name);
    select_column_ids.push_back(col_id);
  }
}
```

### ⚠️ 需要注意的点

1. **生成的 SQL 会包含额外列**：
   ```sql
   -- 索引构建时的 SQL 变为：
   INSERT INTO fts_index_aux (word, doc_id, word_count)
   SELECT WORD_SEGMENT(...), DOC_ID(...), ..., 
          id, base_id, docid_col  -- ← 额外的列（不会被INSERT）
   FROM items1;
   ```

2. **这些列只在 SELECT 中，不会被 INSERT**：
   - ✅ 这是正确的，因为辅助表不需要这些列
   - ✅ 目的是让这些列在执行器中可见，供填充 HashMap

---

## 3. 文件2：ob_table_scan_op.cpp

### ❌ **严重问题：填充位置完全错误！**

**问题代码（第4036-4171行）**：
```cpp
int ObTableScanOp::fetch_next_fts_index_rows()
{
  // ========== 问题1：在查询算子中初始化 HashMap ❌ ==========
  static bool fts_cache_initialized = false;
  if (!fts_cache_initialized) {
    g_fts_scalar_cache.init(1000000);
    fts_cache_initialized = true;
  }
  
  // ========== 问题2：在循环中填充 HashMap ❌ ==========
  while (OB_SUCC(iter_->get_next_row())) {
    // 读取一行数据...
    
    // 填充 HashMap
    if (g_fts_scalar_cache.is_inited()) {
      // 识别字段...
      g_fts_scalar_cache.add_mapping(doc_id_val, id_val, base_id_val, docid_col_val);
    }
  }
}
```

### 为什么是错误的？

#### 问题1：函数选择错误 ❌

**`fetch_next_fts_index_rows()` 是什么时候调用的？**

```cpp
// 这个函数在 索引构建 时被调用，BUT...
// 它是在扫描辅助表（fts_index_aux、fts_doc_word）时调用的！
// 不是在扫描主表时调用的！

调用链：
ObTableScanOp::inner_get_next_row()
  → get_next_row_with_das()
    → do_table_scan()
      → fetch_next_fts_index_rows()  ← 当扫描 FTS 辅助表时
```

**实际情况**：
```
索引构建流程：
1. 扫描主表 items1 → 生成辅助表数据
   ├─ 调用 fetch_next_fts_main_table_rows()  ← 应该在这里填充！
   ├─ 生成 (word, doc_id, word_count) 数据
   └─ 插入到 fts_index_aux

2. 后续扫描辅助表 fts_index_aux（可能用于验证）
   └─ 调用 fetch_next_fts_index_rows()  ← 你在这里填充，错了！
```

**结果**：
- ❌ 在扫描辅助表时，已经没有主表的标量字段了
- ❌ doc_id、id、base_id 这些字段根本不在辅助表中
- ❌ HashMap 无法被正确填充

#### 问题2：字段识别逻辑脆弱 ❌

```cpp
// 当前的字段识别逻辑（第4095-4146行）
for (int64_t i = 0; i < output_col_count; ++i) {
  ObDatum *col_datum = &output_row[i];
  ObObjType dtype = MY_SPEC.output_[i]->datum_meta_.type_;
  
  // 问题：依赖列的类型和顺序来识别字段
  if (dtype == ObUInt64Type && !id_found) {
    // 假设第一个 uint64 是 id
    id_val = col_datum->get_uint64();
    id_found = true;
  } else if (dtype == ObVarcharType && id_found) {
    // 假设 id 之后的两个 varchar 是 base_id 和 docid_col
    if (varchar_col_after_id == 0) {
      base_id_val = str_val;
    } else if (varchar_col_after_id == 1) {
      docid_col_val = str_val;
    }
  }
}
```

**问题**：
1. ❌ 假设列的顺序固定（id → base_id → docid_col）
2. ❌ 假设类型能唯一识别字段（第一个 uint64 就是 id）
3. ❌ 如果表结构变化（增加/删除列），逻辑失效
4. ❌ 如果有多个 uint64 列，识别错误

**正确的做法**：
```cpp
// 应该通过列名或列ID来识别
for (int64_t i = 0; i < output_col_count; ++i) {
  const ObExpr *expr = MY_SPEC.output_[i];
  const char *col_name = expr->expr_name_;  // 或者用 col_id
  
  if (strcmp(col_name, "id") == 0) {
    id_val = output_row[i].get_uint64();
  } else if (strcmp(col_name, "base_id") == 0) {
    base_id_val = output_row[i].get_string();
  } else if (strcmp(col_name, "docid_col") == 0) {
    docid_col_val = output_row[i].get_string();
  }
}
```

#### 问题3：doc_id 获取错误 ❌

```cpp
// 当前代码（第4084-4088行）
if (OB_NOT_NULL(doc_id_datum)) {
  // 问题：这里的 doc_id_datum 是什么？
  // 如果是从辅助表读取的，那它是正确的 doc_id
  // 但如果是从 DOC_ID() 表达式计算的，可能是新生成的 doc_id
  ObDocId doc_id_obj;
  doc_id_obj.from_string(doc_id_datum->get_string());
  doc_id_val = doc_id_obj.id();
}
```

**问题**：
- ❌ 没有明确说明 doc_id 从哪里来
- ❌ 在主表扫描时，doc_id 是通过 `DOC_ID()` 函数生成的
- ❌ 需要从特定的表达式中提取

---

## 4. 正确的实现应该是什么样？

### 方案1：在主表扫描时填充（推荐）⭐⭐⭐

**修改位置**：`ob_table_scan_op.cpp`

**正确的函数**：应该在扫描**主表**时填充

查找正确的扫描主表函数：
```bash
grep -n "fetch_next.*main" src/sql/engine/table/ob_table_scan_op.cpp
```

**可能的函数**：
- `get_next_row_with_das()` - 通用扫描
- 或者在 `inner_get_next_row()` 中判断是否在构建索引

**正确的实现逻辑**：
```cpp
int ObTableScanOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  
  // 1. 判断是否在索引构建阶段扫描主表
  bool is_fts_index_build = (执行上下文标记为索引构建 && 
                              当前扫描的是主表 &&
                              目标是FTS索引);
  
  if (is_fts_index_build) {
    // 2. 初始化 HashMap（仅一次）
    static bool initialized = false;
    if (!initialized) {
      g_fts_scalar_cache.init(1000000);
      initialized = true;
    }
    
    // 3. 扫描主表
    while (OB_SUCC(iter_->get_next_row())) {
      // 4. 获取当前行的所有字段
      uint64_t doc_id = 从_DOC_ID()_表达式获取;
      uint64_t id = 从_id_列获取;
      ObString base_id = 从_base_id_列获取;
      ObString docid_col = 从_docid_col_列获取;
      
      // 5. 填充 HashMap
      g_fts_scalar_cache.add_mapping(doc_id, id, base_id, docid_col);
      
      // 6. 继续索引构建流程
      // 生成分词、插入辅助表等...
    }
  } else {
    // 正常查询，不填充
  }
  
  return ret;
}
```

### 方案2：在索引构建任务中填充（更清晰）⭐⭐⭐⭐⭐

**修改位置**：`src/storage/ddl/ob_build_index_task.cpp`

**优点**：
- ✅ 逻辑更清晰，不污染查询执行代码
- ✅ 明确在索引构建时填充
- ✅ 容易控制生命周期

**实现位置**：
```cpp
// src/storage/ddl/ob_build_index_task.cpp
int ObUniqueIndexChecker::scan_main_table_with_column_checksum(...)
{
  // 在这个函数中，已经在扫描主表
  ObLocalScan local_scan;
  
  // 初始化 HashMap
  g_fts_scalar_cache.init(1000000);
  
  while (OB_SUCC(local_scan.get_next_row(datum_row))) {
    // 此时 datum_row 包含主表的所有列
    
    // 获取 doc_id（从某个表达式或列）
    uint64_t doc_id = ...;
    
    // 获取标量字段（通过列索引）
    uint64_t id = datum_row->storage_datums_[id_col_idx].get_uint64();
    ObString base_id = datum_row->storage_datums_[base_id_col_idx].get_string();
    ObString docid_col = datum_row->storage_datums_[docid_col_idx].get_string();
    
    // 填充 HashMap
    g_fts_scalar_cache.add_mapping(doc_id, id, base_id, docid_col);
    
    // 继续计算列校验和（原有逻辑）
    // ...
  }
  
  return ret;
}
```

---

## 5. 具体修改建议

### 修改1：移除 ob_table_scan_op.cpp 中的错误代码

**删除**：第4036-4171行的所有 HashMap 填充代码

**原因**：
- ❌ 位置错误（在扫描辅助表时，而非主表）
- ❌ 逻辑脆弱（依赖列顺序和类型）
- ❌ 无法正确获取字段

### 修改2：在正确的位置填充 HashMap

**选择方案2（推荐）**：在 `ob_build_index_task.cpp` 中填充

**需要找到的关键点**：
1. 扫描主表的循环
2. 能访问到 doc_id 和主表所有列
3. 能够通过列名或列ID识别字段

### 修改3：正确获取 doc_id

**关键**：doc_id 是通过 `DOC_ID()` 表达式生成的

**需要**：
1. 找到 `DOC_ID()` 表达式的输出
2. 或者从生成的辅助表数据中获取

---

## 6. 当前代码的风险评估

| 风险 | 严重程度 | 说明 |
|------|---------|------|
| **HashMap 无法被正确填充** | ⭐⭐⭐⭐⭐ 极高 | 填充位置错误，无法获取主表字段 |
| **字段识别错误** | ⭐⭐⭐⭐ 高 | 依赖列顺序和类型，容易出错 |
| **doc_id 获取不确定** | ⭐⭐⭐⭐ 高 | 没有明确的 doc_id 来源 |
| **SELECT列添加正确** | ✅ 低 | 这部分逻辑是正确的 |

---

## 7. 测试验证方法

### 测试1：检查 HashMap 是否被填充

```cpp
// 在索引构建完成后
LOG_INFO("HashMap fill status",
         "is_inited", g_fts_scalar_cache.is_inited(),
         "size", g_fts_scalar_cache.size(),
         "expected_size", 预期的行数);

// 如果 size = 0，说明填充失败
```

### 测试2：验证填充的数据

```cpp
// 查询一个已知的 doc_id
ObFtsScalarCache::ScalarFields fields;
int ret = g_fts_scalar_cache.get(100, fields);
if (OB_SUCCESS == ret) {
  LOG_INFO("HashMap content", 
           K(fields.id_), K(fields.base_id_), K(fields.docid_col_));
} else {
  LOG_WARN("doc_id not found in HashMap", K(ret));
}
```

### 测试3：添加断点调试

在以下位置添加断点：
1. `append_fts_scalar_columns()` - 验证 SELECT 列是否添加
2. `fetch_next_fts_index_rows()` - 验证当前扫描的是哪个表
3. HashMap 填充代码 - 验证字段值是否正确

---

## 8. 总结和评分

| 文件 | 评分 | 评价 |
|------|------|------|
| **ob_ddl_common.h/cpp** | 8/10 ✅ | SELECT列添加逻辑正确，但缺少后续正确使用 |
| **ob_table_scan_op.cpp** | 2/10 ❌ | 填充位置完全错误，字段识别脆弱 |
| **整体方案** | 5/10 ⚠️ | 思路正确，但实现有严重问题 |

### 关键问题

1. ❌ **填充位置错误** - 在扫描辅助表时填充，应该在扫描主表时
2. ❌ **字段识别脆弱** - 依赖列顺序和类型，应该用列名/列ID
3. ❌ **doc_id 获取不明确** - 需要从 DOC_ID() 表达式获取

### 下一步

1. **立即修改**：移除 ob_table_scan_op.cpp 中的错误代码
2. **找到正确位置**：在主表扫描时填充（ob_build_index_task.cpp）
3. **正确识别字段**：使用列名或列ID，而非类型和顺序
4. **测试验证**：确保 HashMap 被正确填充

**要我帮你找到正确的填充位置并修改代码吗？**
