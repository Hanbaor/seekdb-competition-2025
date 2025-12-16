# FTS 通用缓存与过滤机制 V3 设计文档

> **前置文档**：
> - [FTS_GENERIC_CACHE_FILTER_DESIGN.md](./FTS_GENERIC_CACHE_FILTER_DESIGN.md)（V1 基础版 - 解决硬编码问题）
> - [FTS_GENERIC_CACHE_FILTER_V2_DESIGN.md](./FTS_GENERIC_CACHE_FILTER_V2_DESIGN.md)（V2 持久化版 - 文件存储 + KVCache）
> 
> **本文档**：V3 生产版，解决多表、多租户、缓存失效等生产环境问题
> 
> **优先级**：低优先级，在 V1/V2 稳定后再实施

---

## 1. V1 存在的架构问题

V1 版本解决了**硬编码列和过滤**的问题，但在生产环境中还存在以下架构问题：

### 🚨 问题一：全局单例无法区分表

**现状**：
```cpp
// ob_table_scan_op.cpp 第 4135 行
static bool fts_cache_initialized = false;  // 全局静态变量！
```

**问题**：
- 如果用户先查询 `table_A`，缓存填充 `table_A` 的数据
- 然后查询 `table_B`，缓存仍然是 `table_A` 的数据
- 结果完全错误！

**解决方案**：缓存需要按 `(tenant_id, table_id)` 进行隔离

### 🚨 问题二：多租户隔离缺失

**现状**：
```cpp
// 全局单例，所有租户共享
extern ObFtsGenericCache g_fts_generic_cache;
```

**问题**：
- OceanBase 是多租户架构
- 不同租户的数据不应该共享缓存
- 存在数据泄露风险

**解决方案**：使用租户级别的缓存管理器

### 🚨 问题三：缓存失效机制缺失

**现状**：
- 缓存在 DDL 创建索引时填充
- 后续 DML（INSERT/UPDATE/DELETE）操作后，缓存数据过期
- 没有任何失效机制

**问题**：
- 删除一行数据后，缓存仍返回旧数据
- 更新数据后，返回错误的过滤结果

**解决方案**：需要增量更新或版本控制机制

### 🚨 问题四：列索引映射不稳定

**现状**：
```cpp
struct ObFtsColumnMeta {
  int64_t output_idx_;  // 在 output_exprs 中的索引
};
```

**问题**：
- `output_idx_` 是 SQL 执行时的动态索引
- 不同查询的 output_exprs 顺序可能不同
- 同一张表的不同查询会导致映射错乱

**解决方案**：使用 `column_id` 而非 `output_idx_`

---

## 2. V2 架构设计

### 2.1 多表多租户架构

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                   ObFtsCacheManager (缓存管理器 - 每租户一个)                      │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                  │
│   cache_map_: HashMap<table_id, ObFtsTableCache*>                               │
│                                                                                  │
│   ┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────┐    │
│   │ ObFtsTableCache     │  │ ObFtsTableCache     │  │ ObFtsTableCache     │    │
│   │ (table_id = 1001)   │  │ (table_id = 1002)   │  │ (table_id = 1003)   │    │
│   │                     │  │                     │  │                     │    │
│   │ - column_metas_[]   │  │ - column_metas_[]   │  │ - column_metas_[]   │    │
│   │ - column_data_[][]  │  │ - column_data_[][]  │  │ - column_data_[][]  │    │
│   │ - version_          │  │ - version_          │  │ - version_          │    │
│   └─────────────────────┘  └─────────────────────┘  └─────────────────────┘    │
│                                                                                  │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 核心数据结构

```cpp
/**
 * 表级缓存 - 每个表一个实例
 */
class ObFtsTableCache {
public:
  /**
   * 初始化表缓存
   * @param table_id 表 ID（全局唯一）
   * @param column_metas 要缓存的列元数据（使用 column_id 标识）
   */
  int init(uint64_t table_id,
           const common::ObIArray<ObFtsColumnMeta> &column_metas,
           int64_t capacity = DEFAULT_CAPACITY);
  
  uint64_t get_table_id() const { return table_id_; }
  int64_t get_version() const { return version_; }
  
  // 版本控制：DML 后调用 invalidate() 或 update_row()
  void invalidate() { version_++; }
  int update_row(uint64_t doc_id, const ObFtsColumnValue *column_values);
  int delete_row(uint64_t doc_id);
  
  // ... 其他方法同 ObFtsGenericCache ...
  
private:
  uint64_t table_id_;
  int64_t version_;  // 缓存版本，用于失效检测
  // ... 其他成员 ...
};

/**
 * 租户级缓存管理器
 */
class ObFtsCacheManager {
public:
  /**
   * 获取或创建表缓存
   */
  int get_or_create_table_cache(uint64_t table_id,
                                 const common::ObIArray<ObFtsColumnMeta> &column_metas,
                                 ObFtsTableCache *&cache);
  
  /**
   * 获取已存在的表缓存（不创建）
   */
  int get_table_cache(uint64_t table_id, ObFtsTableCache *&cache);
  
  /**
   * 失效指定表的缓存（DML 后调用）
   */
  int invalidate_table_cache(uint64_t table_id);
  
  /**
   * 清理所有缓存
   */
  void clear_all();
  
private:
  common::hash::ObHashMap<uint64_t, ObFtsTableCache*> cache_map_;
  common::ObSpinLock lock_;
};

// 通过 MTL (Multi-Tenant Local) 获取当前租户的缓存管理器
#define FTS_CACHE_MGR (MTL(ObFtsCacheManager*))
```

### 2.3 列元数据（V2 版本）

```cpp
/**
 * 列元数据 - 使用 column_id 而非 output_idx
 */
struct ObFtsColumnMeta {
  uint64_t column_id_;           // 列 ID（表 Schema 中的唯一标识）
  common::ObObjType obj_type_;   // 列的数据类型
  common::ObString column_name_; // 列名（用于调试）
  bool is_primary_key_;          // 是否是主键列
  bool is_nullable_;             // 是否允许 NULL
  
  // 运行时动态填充（从 output_exprs 中查找）
  mutable int64_t runtime_output_idx_;  // -1 表示未初始化
  
  ObFtsColumnMeta()
    : column_id_(OB_INVALID_ID),
      obj_type_(common::ObNullType),
      column_name_(),
      is_primary_key_(false),
      is_nullable_(true),
      runtime_output_idx_(-1) {}
      
  TO_STRING_KV(K_(column_id), K_(obj_type), K_(column_name));
};
```

### 2.4 过滤上下文（V2 版本）

```cpp
/**
 * 过滤上下文 - 绑定到特定表
 */
class ObFtsGenericFilterCtx {
public:
  /**
   * 设置绑定的表 ID
   */
  void set_table_id(uint64_t table_id) { table_id_ = table_id; }
  uint64_t get_table_id() const { return table_id_; }
  
  /**
   * 检查过滤条件是否适用于指定表
   */
  bool is_valid_for_table(uint64_t table_id) const {
    return table_id_ == table_id;
  }
  
  // ... 其他方法同 V1 设计 ...
  
private:
  uint64_t table_id_;  // 绑定的表 ID
  // ... 其他成员 ...
};
```

### 2.5 使用流程

```cpp
// ========== 1. 在 ob_table_scan_op.cpp 中 ==========

// 获取表 ID
uint64_t table_id = MY_CTDEF.ref_table_id_;

// 获取表缓存（通过租户级管理器）
ObFtsTableCache *table_cache = nullptr;
if (OB_FAIL(FTS_CACHE_MGR->get_or_create_table_cache(
        table_id, column_metas, table_cache))) {
  LOG_WARN("failed to get table cache", K(ret), K(table_id));
}

// 填充数据
if (OB_NOT_NULL(table_cache)) {
  table_cache->add_row_from_exprs(doc_id, MY_SPEC.output_, eval_ctx_);
}

// ========== 2. 在过滤时 ==========

// 绑定表 ID
g_fts_generic_filter_ctx.set_table_id(table_id);

// 检查时验证
if (g_fts_generic_filter_ctx.is_valid_for_table(current_table_id)) {
  // 执行过滤
}

// ========== 3. DML 后失效缓存 ==========

// 在 INSERT/UPDATE/DELETE 操作后调用
FTS_CACHE_MGR->invalidate_table_cache(table_id);
```

---

## 3. 缓存失效策略

### 3.1 策略一：版本号机制（推荐）

```cpp
class ObFtsTableCache {
  int64_t version_;  // 每次 DML 后递增
  
  // 查询时检查版本
  bool is_valid(int64_t expected_version) const {
    return version_ == expected_version;
  }
};

// 使用方式
int64_t cache_version = table_cache->get_version();
// ... 执行查询 ...
if (!table_cache->is_valid(cache_version)) {
  // 缓存已失效，需要重新回表
}
```

### 3.2 策略二：延迟重建

```cpp
// DML 后标记为失效
table_cache->mark_invalid();

// 下次查询时重建
if (table_cache->is_invalid()) {
  table_cache->rebuild();  // 重新扫描表填充缓存
}
```

### 3.3 策略三：增量更新（性能最优）

```cpp
// INSERT 时添加
table_cache->add_row(new_doc_id, new_values);

// UPDATE 时更新
table_cache->update_row(doc_id, new_values);

// DELETE 时标记删除
table_cache->delete_row(doc_id);  // 标记为已删除，不物理删除
```

**推荐**：对于 POC 阶段，使用**策略一（版本号）**最简单；生产环境可以升级到**策略三（增量更新）**。

---

## 4. V1 与 V2 对比

| 场景 | V1 状态 | V2 状态 |
|------|---------|---------|
| 任意列名称 | ✅ | ✅ 通过 column_id 访问 |
| 任意列类型 | ✅ | ✅ |
| 任意数量的列 | ✅ | ✅ |
| 任意比较操作 | ✅ | ✅ |
| IN 操作 | ✅ | ✅ |
| 多条件 AND 组合 | ✅ | ✅ |
| 缓存回表 | ✅ | ✅ |
| 过滤下推 | ✅ | ✅ |
| **多表支持** | ❌ 全局单例 | ✅ 按 table_id 隔离 |
| **多租户隔离** | ❌ | ✅ MTL 租户级管理 |
| **缓存失效** | ❌ | ✅ 版本号机制 |
| **列标识稳定性** | ❌ output_idx | ✅ column_id |

---

## 5. 开发工作量估算

### P1 - 高优先级（生产就绪）

| 功能 | 原因 | 工作量 |
|------|------|--------|
| 多表支持 (`table_id` 隔离) | 避免不同表数据混淆 | 1 天 |
| 使用 `column_id` 替代 `output_idx` | 避免不同查询映射错乱 | 0.5 天 |
| 多租户支持 (MTL) | 生产环境必需 | 1 天 |
| 缓存失效机制（版本号） | 数据一致性 | 1 天 |

**小计**：约 3.5 天

### P2 - 中优先级（性能优化）

| 功能 | 原因 | 工作量 |
|------|------|--------|
| 增量缓存更新 | 高级优化 | 2 天 |

### P3 - 低优先级（锦上添花）

| 功能 | 原因 | 工作量 |
|------|------|--------|
| 缓存持久化 | 加快重启恢复 | 2 天 |
| LRU 淘汰策略 | 内存管理 | 1 天 |

---

## 6. 简化版实现方案（可选）

如果不想完整实现 V2，可以采用简化方案解决最严重的问题：

### 6.1 简化的多表支持

不使用完整的 MTL 管理器，而是使用简单的 table_id 检查：

```cpp
// 在 ObFtsGenericCache 中添加 table_id 字段
class ObFtsGenericCache {
private:
  uint64_t current_table_id_;
  
public:
  void set_table_id(uint64_t table_id) {
    if (current_table_id_ != table_id) {
      // 表切换，需要重置缓存
      reuse();
      current_table_id_ = table_id;
    }
  }
  
  uint64_t get_table_id() const { return current_table_id_; }
};

// 使用时
g_fts_generic_cache.set_table_id(MY_CTDEF.ref_table_id_);
```

**优点**：实现简单（0.5 天），解决了最严重的多表混淆问题  
**缺点**：切换表时会清空缓存，不支持多表同时缓存

### 6.2 简化的列标识

使用列在 Schema 中的顺序索引（而非 column_id），但记录 table_id 验证：

```cpp
struct ObFtsColumnMeta {
  int64_t schema_col_idx_;  // 列在表 Schema 中的顺序（0-based）
  common::ObObjType obj_type_;
};
```

### 6.3 简化的缓存失效

不实现自动失效，提供手动清除方法：

```cpp
// 在可能导致缓存失效的场景调用
g_fts_generic_cache.reuse();
```

---

## 7. 总结

| 问题 | V1 设计 | 问题影响 | V2 解决方案 |
|------|---------|---------|-------------|
| **全局单例** | `extern g_fts_generic_cache;` | 查询表 B 时返回表 A 的数据 | 按 table_id 隔离或表切换时清空 |
| **output_idx** | 使用执行时的输出列索引 | 不同查询的索引可能不同 | 使用 column_id 或 schema_col_idx |
| **无失效机制** | 缓存一次填充永久使用 | DML 后返回脏数据 | 版本号或手动 reuse |
| **无租户隔离** | 所有租户共享一个缓存 | 数据安全问题（生产环境） | MTL 租户级管理 |

**建议**：
- **POC 阶段**：先完成 V1，用简化方案（6.1 节）解决多表问题
- **生产环境**：完整实现 V2

---

**文档版本**：v2.0  
**创建日期**：2025-12-14  
**依赖文档**：FTS_GENERIC_CACHE_FILTER_DESIGN.md (V1)  
**作者**：FTS 优化开发组
