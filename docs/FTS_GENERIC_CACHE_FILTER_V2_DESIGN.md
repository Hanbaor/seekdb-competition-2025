# FTS 通用缓存与过滤机制 V2 设计文档

> **前置文档**：[FTS_GENERIC_CACHE_FILTER_DESIGN.md](./FTS_GENERIC_CACHE_FILTER_DESIGN.md)（V1 基础版）
> 
> **本文档**：V2 持久化版，将缓存数据持久化到磁盘，查询时通过 KVCache 按需加载
> 
> **参考实现**：
> - [BM25S_PHASE2_PERSISTENCE.md](./BM25S_PHASE2_PERSISTENCE.md) - 持久化文件格式
> - [BM25S_PHASE3_KVCACHE_QUERY.md](./BM25S_PHASE3_KVCACHE_QUERY.md) - KVCache 查询机制

---

## ✅ 实现状态总结

> **状态**：已完成 ✅  
> **完成日期**：2025-12-16  
> **测试状态**：Memory Mode 和 KVCache Mode 均已验证通过

### 已实现功能清单

| 功能模块 | 状态 | 实现文件 |
|----------|------|----------|
| 文件格式定义 | ✅ 完成 | `ob_fts_scalar_kvcache.h` |
| 持久化写入 | ✅ 完成 | `ob_fts_generic_cache.cpp::persist_to_disk()` |
| KVCache Key/Value | ✅ 完成 | `ob_fts_scalar_kvcache.h/cpp` |
| 磁盘加载 | ✅ 完成 | `ob_fts_generic_cache.cpp::load_from_disk()` |
| 懒加载机制 | ✅ 完成 | `ob_fts_generic_cache.cpp::ensure_ready()` |
| KVCache 查询 | ✅ 完成 | `ob_fts_generic_cache.cpp::get_row_from_kvcache()` |
| 批量预取 | ✅ 完成 | `ob_fts_generic_cache.cpp::prefetch_rows()` |
| 过滤集成 | ✅ 完成 | `ob_das_tr_merge_iter.cpp` |
| 回表集成 | ✅ 完成 | `ob_das_cache_lookup_iter.cpp` |
| 过滤条件提取 | ✅ 完成 | `ob_table_scan_op.cpp` |

### 新增/修改的源文件

| 文件 | 操作 | 说明 |
|------|------|------|
| `src/sql/das/ob_fts_scalar_kvcache.h` | 新增 | KVCache Key/Value 定义、文件格式结构 |
| `src/sql/das/ob_fts_scalar_kvcache.cpp` | 新增 | KVCache 全局实例管理、Value 访问函数 |
| `src/sql/das/ob_fts_generic_cache.h` | 修改 | 添加持久化和 KVCache 相关接口 |
| `src/sql/das/ob_fts_generic_cache.cpp` | 修改 | 实现持久化写入、加载、KVCache 查询 |
| `src/sql/das/ob_fts_generic_filter.h` | 修改 | 添加 `check_row_from_cache()` |
| `src/sql/das/ob_fts_generic_filter.cpp` | 修改 | 实现基于 KVCache Value 的过滤检查 |
| `src/sql/das/iter/sparse_retrieval/ob_das_tr_merge_iter.cpp` | 修改 | 集成 KVCache 模式过滤 |
| `src/sql/das/iter/ob_das_cache_lookup_iter.cpp` | 修改 | 集成 KVCache 模式回表 |
| `src/sql/engine/table/ob_table_scan_op.cpp` | 修改 | 持久化调用、KVCache 模式条件提取 |
| `src/sql/CMakeLists.txt` | 修改 | 添加 `ob_fts_scalar_kvcache.cpp` |

### 运行模式说明

系统支持两种运行模式，自动切换：

| 模式 | 触发条件 | 数据来源 | 性能 |
|------|----------|----------|------|
| **Memory Mode** | DDL 后同一会话 | 内存数组 | 最快（O(1) 访问）|
| **KVCache Mode** | 服务器重启后 | 持久化文件 + LRU 缓存 | 快（热点数据 O(1)）|

### 持久化文件位置

```
/data/obcluster/store/fts_scalar_cache/fts_scalar_cache.dat
```

### 关键日志标识

```bash
# 过滤条件提取
[FTS_NEW_SYSTEM] filter extraction SUCCESS(condition_count=X, is_memory_mode=Y, is_kvcache_mode=Z)

# 过滤执行
[FTS_FILTER] Using NEW system (Generic Cache Memory Mode)
[FTS_FILTER] Using NEW system (Generic Cache KVCache Mode)

# 回表执行
[FTS_LOOKUP] Using NEW system (Generic Cache Memory Mode)
[FTS_LOOKUP] Using NEW system (Generic Cache KVCache Mode)

# 持久化
[FTS_GENERIC_CACHE] persist_to_disk: completed successfully
[FTS_GENERIC_CACHE] load_from_disk: completed successfully (KVCache Mode enabled)
```

### 实现过程中的关键修复

1. **`is_inited_` 状态修复**：`load_from_disk()` 后不再设置 `is_inited_ = true`，确保正确区分 Memory Mode 和 KVCache Mode
2. **过滤条件提取修复**：KVCache 模式下也能正确提取过滤条件
3. **列索引映射修复**：KVCache 模式使用 `find_column_by_type()` 获取正确的列索引

---

## 1. 概述

### 1.1 V1 的局限性

V1 方案将所有缓存数据存储在内存中：

```cpp
// V1: 全内存存储
class ObFtsGenericCache {
  ObFtsColumnValue *column_data_[MAX_COLUMNS];  // 所有数据常驻内存
};
```

**问题**：
- 数据量大时占用大量内存（如 20 万行 × 10 列 × 平均 50 字节 = ~100 MB）
- 数据库重启后缓存丢失，需要重新填充
- 无法利用系统的 LRU 缓存机制

### 1.2 V2 设计目标

| 特性 | V1 | V2 |
|------|----|----|
| 存储位置 | 全内存 | 持久化文件 + KVCache |
| 内存占用 | 全量数据 | 仅热点数据 |
| 重启恢复 | 需重新填充 | 直接加载文件 |
| 缓存淘汰 | 无 | LRU 自动淘汰 |

### 1.3 架构概览

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                         FTS 通用缓存 V2 架构                                     │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                  │
│  DDL/首次查询时：                                                                │
│  ┌───────────────────────────────────────────────────────────────────────────┐  │
│  │                        持久化阶段                                          │  │
│  │  ob_table_scan_op.cpp                                                     │  │
│  │       │                                                                    │  │
│  │       ▼                                                                    │  │
│  │  收集行数据 ──────▶ 写入持久化文件                                         │  │
│  │  (doc_id, col1, col2, ...)   fts_scalar_cache.dat                         │  │
│  └───────────────────────────────────────────────────────────────────────────┘  │
│                                                                                  │
│  查询时：                                                                        │
│  ┌───────────────────────────────────────────────────────────────────────────┐  │
│  │                        KVCache 查询阶段                                    │  │
│  │                                                                            │  │
│  │  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐                  │  │
│  │  │ 请求 doc_id │────▶│  KVCache    │────▶│ 返回行数据   │                  │  │
│  │  └─────────────┘     │  (LRU)      │     └─────────────┘                  │  │
│  │                      └──────┬──────┘                                       │  │
│  │                             │ 未命中                                       │  │
│  │                             ▼                                              │  │
│  │                      ┌─────────────┐                                       │  │
│  │                      │ 从文件加载   │                                       │  │
│  │                      │ (批量预取)   │                                       │  │
│  │                      └─────────────┘                                       │  │
│  └───────────────────────────────────────────────────────────────────────────┘  │
│                                                                                  │
└─────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 文件格式设计

### 2.1 整体结构

```
+============================================+
|          FTS Scalar Cache File             |
+============================================+
| Section 1: Header (256 bytes, 固定)        |
+--------------------------------------------+
| Section 2: Column Meta (变长)              |
+--------------------------------------------+
| Section 3: Row Index (16 bytes × row_count)|
+--------------------------------------------+
| Section 4: Row Data (变长)                 |
+============================================+
```

### 2.2 Header 结构 (256 bytes)

```cpp
struct FtsScalarCacheFileHeader {
    // Magic & Version (16 bytes)
    uint64_t magic;              // 0x4654535343414348 ("FTSCACHE")
    int32_t version;             // 版本号，当前为 1
    int32_t flags;               // 预留标志位
    
    // 统计信息 (32 bytes)
    int64_t row_count;           // 总行数
    int64_t column_count;        // 列数
    int64_t max_doc_id;          // 最大 doc_id
    int64_t reserved1;
    
    // Section 偏移和大小 (64 bytes)
    int64_t column_meta_offset;  // Column Meta 起始偏移
    int64_t column_meta_size;    // Column Meta 大小
    int64_t row_index_offset;    // Row Index 起始偏移
    int64_t row_index_size;      // Row Index 大小
    int64_t row_data_offset;     // Row Data 起始偏移
    int64_t row_data_size;       // Row Data 大小
    int64_t reserved2;
    int64_t reserved3;
    
    // 校验和 (16 bytes)
    uint64_t checksum;           // 文件校验和
    int64_t create_timestamp;    // 创建时间戳
    
    // 填充到 256 bytes
    char padding[256 - 128];
    
    // 常量
    static const uint64_t MAGIC = 0x4654535343414348ULL;
    static const int32_t CURRENT_VERSION = 1;
    static const int64_t HEADER_SIZE = 256;
    
    void reset() { memset(this, 0, sizeof(*this)); }
    bool is_valid() const { return magic == MAGIC && version == CURRENT_VERSION; }
};
```

### 2.3 Column Meta 结构

```cpp
struct ColumnMetaEntry {
    int64_t column_id;           // 列 ID（可选，用于校验）
    int32_t column_type;         // ObObjType
    int32_t column_name_len;     // 列名长度
    // char column_name[column_name_len];  // 列名（变长）
    int32_t is_primary_key;      // 是否主键
    int32_t is_nullable;         // 是否可空
};
// 固定部分 = 8 + 4 + 4 + 4 + 4 = 24 bytes
// 变长部分 = column_name_len bytes
```

### 2.4 Row Index 结构

```cpp
struct RowIndexEntry {
    int64_t data_offset;         // 该行在 Row Data 区域的偏移
    int64_t data_size;           // 该行数据的大小
};
// 每行 16 bytes
// 使用 doc_id 作为索引：row_index[doc_id - 1]
```

### 2.5 Row Data 结构

每行数据紧凑存储：

```cpp
// 对于 doc_id = X 的行：
// Row Data 布局:
// [col_0_value][col_1_value]...[col_n_value]
//
// 每个 col_value 的格式：
// [value_type: 1 byte][value_data: 变长]
//
// value_type:
//   0 = NULL
//   1 = INT64 (8 bytes)
//   2 = UINT64 (8 bytes)
//   3 = DOUBLE (8 bytes)
//   4 = STRING (4 bytes len + data)
//   5 = DATETIME (8 bytes)
```

---

## 3. KVCache Key/Value 定义

### 3.1 需要引入的头文件

```cpp
#include "share/cache/ob_kv_storecache.h"
#include "share/cache/ob_kvcache_struct.h"
```

### 3.2 ScalarCache Key

```cpp
// ob_fts_scalar_kvcache.h

#ifndef OCEANBASE_SQL_DAS_OB_FTS_SCALAR_KVCACHE_H_
#define OCEANBASE_SQL_DAS_OB_FTS_SCALAR_KVCACHE_H_

#include "share/cache/ob_kvcache_struct.h"

namespace oceanbase
{
namespace sql
{

/**
 * KVCache Key: doc_id
 * 
 * 设计说明：
 * - 使用 doc_id 作为 Key，直接 O(1) 定位
 * - 全局单例模式，无需 tenant_id/table_id（V3 会添加）
 */
class ObFtsScalarCacheKey : public common::ObIKVCacheKey
{
public:
    ObFtsScalarCacheKey() : doc_id_(0) {}
    ObFtsScalarCacheKey(uint64_t doc_id) : doc_id_(doc_id) {}
    
    virtual bool operator==(const ObIKVCacheKey &other) const override
    {
        const ObFtsScalarCacheKey &o = static_cast<const ObFtsScalarCacheKey&>(other);
        return doc_id_ == o.doc_id_;
    }
    
    virtual uint64_t hash() const override
    {
        return common::murmurhash(&doc_id_, sizeof(doc_id_), 0);
    }
    
    virtual uint64_t get_tenant_id() const override { return OB_SYS_TENANT_ID; }
    
    virtual int64_t size() const override { return sizeof(*this); }
    
    virtual int deep_copy(char *buf, const int64_t buf_len,
                          ObIKVCacheKey *&key) const override
    {
        int ret = OB_SUCCESS;
        if (OB_ISNULL(buf) || buf_len < static_cast<int64_t>(sizeof(*this))) {
            ret = OB_INVALID_ARGUMENT;
        } else {
            ObFtsScalarCacheKey *new_key = new (buf) ObFtsScalarCacheKey(doc_id_);
            key = new_key;
        }
        return ret;
    }
    
    uint64_t get_doc_id() const { return doc_id_; }
    
    TO_STRING_KV(K_(doc_id));
    
public:
    uint64_t doc_id_;
};
```

### 3.3 ScalarCache Value

```cpp
/**
 * KVCache Value: 一行的所有列值
 * 
 * 存储格式与文件中一致，方便零拷贝
 */
class ObFtsScalarCacheValue : public common::ObIKVCacheValue
{
public:
    ObFtsScalarCacheValue()
        : column_count_(0), data_size_(0), data_(nullptr) {}
    
    // 设置数据
    void set(int64_t column_count, int64_t data_size, const char *data)
    {
        column_count_ = column_count;
        data_size_ = data_size;
        data_ = const_cast<char*>(data);
    }
    
    virtual int64_t size() const override
    {
        return sizeof(*this) + data_size_;
    }
    
    virtual int deep_copy(char *buf, const int64_t buf_len,
                          ObIKVCacheValue *&value) const override
    {
        int ret = OB_SUCCESS;
        int64_t need_size = size();
        
        if (OB_ISNULL(buf) || buf_len < need_size) {
            ret = OB_INVALID_ARGUMENT;
        } else {
            ObFtsScalarCacheValue *new_val = new (buf) ObFtsScalarCacheValue();
            new_val->column_count_ = column_count_;
            new_val->data_size_ = data_size_;
            
            char *data_buf = buf + sizeof(ObFtsScalarCacheValue);
            if (data_size_ > 0 && OB_NOT_NULL(data_)) {
                MEMCPY(data_buf, data_, data_size_);
            }
            new_val->data_ = data_buf;
            
            value = new_val;
        }
        return ret;
    }
    
    // ========== 列值访问函数 ==========
    
    /**
     * 获取指定列的值
     * @param col_idx 列索引
     * @param out_value 输出值
     * @return OB_SUCCESS 或错误码
     */
    int get_column_value(int64_t col_idx, ObFtsColumnValue &out_value) const;
    
    /**
     * 快速获取整数列（用于 id < N 过滤）
     */
    int64_t get_int_column_fast(int64_t col_idx) const;
    
    /**
     * 快速获取字符串列（用于 base_id IN (...) 过滤）
     */
    common::ObString get_string_column_fast(int64_t col_idx) const;
    
    TO_STRING_KV(K_(column_count), K_(data_size));
    
public:
    int64_t column_count_;
    int64_t data_size_;
    char *data_;
};

// KVCache 类型定义
typedef common::ObKVCache<ObFtsScalarCacheKey, ObFtsScalarCacheValue> ObFtsScalarKVCache;

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_DAS_OB_FTS_SCALAR_KVCACHE_H_
```

---

## 4. 持久化写入实现

### 4.1 主入口函数

```cpp
int ObFtsGenericCache::persist_to_disk(const char *cache_dir)
{
    int ret = OB_SUCCESS;
    
    if (OB_ISNULL(cache_dir)) {
        ret = OB_INVALID_ARGUMENT;
        return ret;
    }
    
    if (!is_inited_ || count_ == 0) {
        ret = OB_NOT_INIT;
        return ret;
    }
    
    // 1. 创建目录
    if (OB_FAIL(ObIODeviceLocalFileOp::mkdir(cache_dir, 0755))) {
        if (OB_FILE_OR_DIRECTORY_EXIST == ret) {
            ret = OB_SUCCESS;
        } else {
            LOG_WARN("fail to mkdir", K(ret), K(cache_dir));
            return ret;
        }
    }
    
    // 2. 构建文件路径
    char file_path[OB_MAX_FILE_NAME_LENGTH];
    snprintf(file_path, sizeof(file_path), "%s/fts_scalar_cache.dat", cache_dir);
    
    // 3. 打开文件
    common::ObIOFd fd;
    if (OB_FAIL(ObIODeviceLocalFileOp::open(file_path, 
                                            O_CREAT | O_WRONLY | O_TRUNC, 
                                            0644, fd))) {
        LOG_WARN("fail to open file", K(ret), K(file_path));
        return ret;
    }
    
    // 4. 写入各个 Section
    do {
        if (OB_FAIL(write_header(fd))) break;
        if (OB_FAIL(write_column_meta(fd))) break;
        if (OB_FAIL(write_row_index_and_data(fd))) break;
        if (OB_FAIL(update_header(fd))) break;
    } while (0);
    
    // 5. 同步并关闭
    ObIODeviceLocalFileOp::fsync(fd);
    ObIODeviceLocalFileOp::close(fd);
    
    if (OB_SUCC(ret)) {
        LOG_INFO("FTS scalar cache persisted", K(file_path), 
                 K_(count), K_(column_count));
    }
    
    return ret;
}
```

### 4.2 写入 Row Index 和 Row Data

```cpp
int ObFtsGenericCache::write_row_index_and_data(const common::ObIOFd &fd)
{
    int ret = OB_SUCCESS;
    
    // 1. 分配 Row Index 数组
    int64_t index_size = count_ * sizeof(RowIndexEntry);
    RowIndexEntry *row_index = static_cast<RowIndexEntry*>(
        allocator_.alloc(index_size));
    if (OB_ISNULL(row_index)) {
        return OB_ALLOCATE_MEMORY_FAILED;
    }
    
    // 2. 计算每行的偏移和大小，同时写入 Row Data
    int64_t data_offset = 0;
    int64_t write_offset = file_header_.row_data_offset;
    
    for (uint64_t doc_id = 1; OB_SUCC(ret) && doc_id <= count_; ++doc_id) {
        // 2.1 序列化该行数据
        char row_buf[4096];  // 假设单行不超过 4KB
        int64_t row_size = 0;
        
        if (OB_FAIL(serialize_row(doc_id, row_buf, sizeof(row_buf), row_size))) {
            LOG_WARN("fail to serialize row", K(ret), K(doc_id));
            break;
        }
        
        // 2.2 记录索引
        row_index[doc_id - 1].data_offset = data_offset;
        row_index[doc_id - 1].data_size = row_size;
        
        // 2.3 写入文件
        int64_t write_size = 0;
        if (OB_FAIL(ObIODeviceLocalFileOp::pwrite_impl(
                fd.second_id_, row_buf, row_size, write_offset, write_size))) {
            LOG_WARN("fail to write row", K(ret), K(doc_id));
            break;
        }
        
        data_offset += row_size;
        write_offset += row_size;
    }
    
    // 3. 写入 Row Index
    if (OB_SUCC(ret)) {
        int64_t write_size = 0;
        if (OB_FAIL(ObIODeviceLocalFileOp::pwrite_impl(
                fd.second_id_, row_index, index_size,
                file_header_.row_index_offset, write_size))) {
            LOG_WARN("fail to write row index", K(ret));
        }
    }
    
    // 4. 更新 header 中的大小
    file_header_.row_data_size = data_offset;
    
    allocator_.free(row_index);
    return ret;
}

int ObFtsGenericCache::serialize_row(uint64_t doc_id, 
                                      char *buf, int64_t buf_len,
                                      int64_t &row_size)
{
    int ret = OB_SUCCESS;
    char *p = buf;
    
    for (int64_t col_idx = 0; col_idx < column_count_; ++col_idx) {
        const ObFtsColumnValue &val = column_data_[col_idx][doc_id - 1];
        
        // 写入类型标记
        *p++ = static_cast<char>(val.type_);
        
        // 写入值
        switch (val.type_) {
            case ObFtsColumnValue::TYPE_NULL:
                // 无额外数据
                break;
            case ObFtsColumnValue::TYPE_INT64:
                memcpy(p, &val.int_val_, sizeof(int64_t));
                p += sizeof(int64_t);
                break;
            case ObFtsColumnValue::TYPE_UINT64:
                memcpy(p, &val.uint_val_, sizeof(uint64_t));
                p += sizeof(uint64_t);
                break;
            case ObFtsColumnValue::TYPE_DOUBLE:
                memcpy(p, &val.double_val_, sizeof(double));
                p += sizeof(double);
                break;
            case ObFtsColumnValue::TYPE_STRING: {
                int32_t len = val.str_val_.len_;
                memcpy(p, &len, sizeof(int32_t));
                p += sizeof(int32_t);
                if (len > 0) {
                    memcpy(p, val.str_val_.ptr_, len);
                    p += len;
                }
                break;
            }
            case ObFtsColumnValue::TYPE_DATETIME:
                memcpy(p, &val.datetime_val_, sizeof(int64_t));
                p += sizeof(int64_t);
                break;
        }
    }
    
    row_size = p - buf;
    return ret;
}
```

---

## 5. KVCache 查询实现

### 5.1 全局缓存实例

```cpp
// ob_fts_scalar_kvcache.cpp

namespace oceanbase
{
namespace sql
{

static ObFtsScalarKVCache *g_scalar_cache = nullptr;
static bool g_scalar_cache_inited = false;

int init_fts_scalar_kvcache()
{
    int ret = OB_SUCCESS;
    
    if (g_scalar_cache_inited) {
        return OB_SUCCESS;
    }
    
    void *buf = ob_malloc(sizeof(ObFtsScalarKVCache), "FtsScalarKV");
    if (OB_ISNULL(buf)) {
        return OB_ALLOCATE_MEMORY_FAILED;
    }
    
    g_scalar_cache = new (buf) ObFtsScalarKVCache();
    
    if (OB_FAIL(g_scalar_cache->init("fts_scalar_cache", 1))) {
        LOG_WARN("fail to init scalar cache", K(ret));
        return ret;
    }
    
    g_scalar_cache_inited = true;
    LOG_INFO("FTS scalar KVCache initialized");
    
    return ret;
}

} // namespace sql
} // namespace oceanbase
```

### 5.2 从缓存获取行数据

```cpp
int ObFtsGenericCache::get_row_from_kvcache(uint64_t doc_id,
                                             const ObFtsScalarCacheValue *&row,
                                             common::ObKVCacheHandle &handle)
{
    int ret = OB_SUCCESS;
    
    if (!index_file_opened_) {
        ret = OB_NOT_INIT;
        return ret;
    }
    
    // 1. 尝试从缓存获取
    ObFtsScalarCacheKey key(doc_id);
    
    ret = g_scalar_cache->get(key, row, handle);
    
    if (OB_SUCCESS == ret) {
        // 缓存命中
        return OB_SUCCESS;
    }
    
    if (OB_HASH_NOT_EXIST == ret || OB_ENTRY_NOT_EXIST == ret) {
        // 2. 缓存未命中，从文件加载
        ret = load_row_to_kvcache(doc_id, row, handle);
    }
    
    return ret;
}
```

### 5.3 从文件加载行数据

```cpp
int ObFtsGenericCache::load_row_to_kvcache(uint64_t doc_id,
                                            const ObFtsScalarCacheValue *&row,
                                            common::ObKVCacheHandle &handle)
{
    int ret = OB_SUCCESS;
    
    if (doc_id == 0 || doc_id > static_cast<uint64_t>(file_header_.row_count)) {
        return OB_ENTRY_NOT_EXIST;
    }
    
    // 1. 从 Row Index 获取偏移和大小
    RowIndexEntry index_entry;
    int64_t index_offset = file_header_.row_index_offset + 
                           (doc_id - 1) * sizeof(RowIndexEntry);
    
    int64_t read_size = 0;
    if (OB_FAIL(ObIODeviceLocalFileOp::pread_impl(
            index_fd_.second_id_, &index_entry, sizeof(index_entry),
            index_offset, read_size))) {
        return ret;
    }
    
    // 2. 读取行数据
    char *row_data = static_cast<char*>(allocator_.alloc(index_entry.data_size));
    if (OB_ISNULL(row_data)) {
        return OB_ALLOCATE_MEMORY_FAILED;
    }
    
    int64_t data_offset = file_header_.row_data_offset + index_entry.data_offset;
    if (OB_FAIL(ObIODeviceLocalFileOp::pread_impl(
            index_fd_.second_id_, row_data, index_entry.data_size,
            data_offset, read_size))) {
        allocator_.free(row_data);
        return ret;
    }
    
    // 3. 构建临时 Value
    ObFtsScalarCacheValue tmp_value;
    tmp_value.set(file_header_.column_count, index_entry.data_size, row_data);
    
    // 4. 放入缓存
    ObFtsScalarCacheKey key(doc_id);
    ret = g_scalar_cache->put_and_fetch(key, tmp_value, row, handle);
    
    // 5. 释放临时缓冲区
    allocator_.free(row_data);
    
    return ret;
}
```

### 5.4 批量预取优化

```cpp
/**
 * 批量预取多行数据到 KVCache
 * 用于过滤场景，减少逐条加载的 I/O 开销
 */
int ObFtsGenericCache::prefetch_rows(const common::ObIArray<uint64_t> &doc_ids)
{
    int ret = OB_SUCCESS;
    
    for (int64_t i = 0; OB_SUCC(ret) && i < doc_ids.count(); ++i) {
        uint64_t doc_id = doc_ids.at(i);
        
        // 检查是否已在缓存中
        ObFtsScalarCacheKey key(doc_id);
        const ObFtsScalarCacheValue *row = nullptr;
        common::ObKVCacheHandle handle;
        
        if (OB_SUCCESS == g_scalar_cache->get(key, row, handle)) {
            // 已在缓存中，跳过
            continue;
        }
        
        // 加载到缓存
        if (OB_FAIL(load_row_to_kvcache(doc_id, row, handle))) {
            if (OB_ENTRY_NOT_EXIST == ret) {
                ret = OB_SUCCESS;  // 不存在的行不是错误
            }
        }
    }
    
    return ret;
}
```

---

## 6. 过滤条件检查（使用 KVCache）

### 6.1 在 `ob_das_tr_merge_iter.cpp` 中使用

```cpp
// 替换原有的内存访问
// 原代码：
// if (g_fts_generic_filter_ctx.has_conditions()) {
//   if (!g_fts_generic_filter_ctx.check_row_fast(doc_id, g_fts_generic_cache)) {
//     continue;
//   }
// }

// 新代码（使用 KVCache）：
if (g_fts_generic_filter_ctx.has_conditions()) {
    const ObFtsScalarCacheValue *row = nullptr;
    common::ObKVCacheHandle handle;
    
    int cache_ret = g_fts_generic_cache.get_row_from_kvcache(doc_id, row, handle);
    if (OB_SUCCESS != cache_ret) {
        // 无法获取行数据，跳过过滤（保守策略，允许通过）
        continue;
    }
    
    if (!g_fts_generic_filter_ctx.check_row_from_cache(row)) {
        filtered_count++;
        continue;
    }
    // handle 析构时自动释放缓存引用
}
```

### 6.2 `check_row_from_cache` 实现

```cpp
bool ObFtsGenericFilterCtx::check_row_from_cache(
    const ObFtsScalarCacheValue *row) const
{
    if (condition_count_ == 0 || OB_ISNULL(row)) {
        return true;
    }
    
    // 检查每个条件
    for (int64_t i = 0; i < condition_count_; ++i) {
        const ObFtsFilterCondition &cond = conditions_[i];
        
        // 从缓存值中获取列数据
        ObFtsColumnValue col_val;
        if (OB_SUCCESS != row->get_column_value(cond.column_idx_, col_val)) {
            continue;
        }
        
        if (!cond.check(col_val)) {
            return false;
        }
    }
    
    return true;
}
```

---

## 7. 回表场景使用（KVCache）

### 7.1 在 `ob_das_cache_lookup_iter.cpp` 中使用

```cpp
// 替换原有的回表逻辑
if (use_kvcache_mode && g_fts_generic_cache.is_file_opened()) {
    cache_fill_count = std::min(total_doc_ids, capacity);
    all_cache_hit = true;
    
    for (int64_t i = 0; OB_SUCC(ret) && i < cache_fill_count; ++i) {
        uint64_t doc_id = cached_doc_ids_.at(i);
        
        const ObFtsScalarCacheValue *row = nullptr;
        common::ObKVCacheHandle handle;
        
        int cache_ret = g_fts_generic_cache.get_row_from_kvcache(doc_id, row, handle);
        if (OB_SUCCESS != cache_ret) {
            all_cache_hit = false;
            break;
        }
        
        // 填充到输出列
        for (int64_t j = 0; OB_SUCC(ret) && j < output_col_count; ++j) {
            ObFtsColumnValue col_val;
            if (OB_FAIL(row->get_column_value(j, col_val))) {
                all_cache_hit = false;
                break;
            }
            
            if (OB_FAIL(column_value_to_datum(col_val, 
                    lookup_ctdef->result_output_.at(j),
                    col_datums[j][i],
                    eval_ctx_->exec_ctx_.get_allocator()))) {
                all_cache_hit = false;
                break;
            }
        }
    }
    
    // ... 后续处理
}
```

---

## 8. 文件路径设计

### 8.1 路径规范

```
${data_dir}/fts_scalar_cache/fts_scalar_cache.dat

示例:
/data/obcluster/store/fts_scalar_cache/fts_scalar_cache.dat
```

### 8.2 获取缓存路径

```cpp
int ObFtsGenericCache::get_cache_path(char *path_buf, int64_t buf_len)
{
    const char *data_dir = GCONF.data_dir.str();
    
    int n = snprintf(path_buf, buf_len, "%s/fts_scalar_cache", data_dir);
    if (n < 0 || n >= buf_len) {
        return OB_SIZE_OVERFLOW;
    }
    
    return OB_SUCCESS;
}
```

---

## 9. 启动加载流程

### 9.1 加载索引文件

```cpp
int ObFtsGenericCache::load_from_disk(const char *cache_dir)
{
    int ret = OB_SUCCESS;
    
    // 1. 构建文件路径
    char file_path[OB_MAX_FILE_NAME_LENGTH];
    snprintf(file_path, sizeof(file_path), "%s/fts_scalar_cache.dat", cache_dir);
    
    // 2. 检查文件是否存在
    bool is_exist = false;
    if (OB_FAIL(ObIODeviceLocalFileOp::exist(file_path, is_exist))) {
        return ret;
    }
    if (!is_exist) {
        return OB_FILE_NOT_EXIST;
    }
    
    // 3. 打开文件
    if (OB_FAIL(ObIODeviceLocalFileOp::open(file_path, O_RDONLY, 0, index_fd_))) {
        return ret;
    }
    
    // 4. 读取并验证 Header
    int64_t read_size = 0;
    if (OB_FAIL(ObIODeviceLocalFileOp::pread_impl(
            index_fd_.second_id_, &file_header_, sizeof(file_header_), 0, read_size))) {
        close_index_file();
        return ret;
    }
    
    if (!file_header_.is_valid()) {
        close_index_file();
        return OB_INVALID_DATA;
    }
    
    // 5. 初始化 KVCache
    if (!g_scalar_cache_inited) {
        if (OB_FAIL(init_fts_scalar_kvcache())) {
            close_index_file();
            return ret;
        }
    }
    
    // 6. 更新状态
    count_ = file_header_.row_count;
    column_count_ = file_header_.column_count;
    index_file_opened_ = true;
    is_inited_ = true;
    
    LOG_INFO("FTS scalar cache loaded from disk", K(file_path),
             K(file_header_.row_count), K(file_header_.column_count));
    
    return ret;
}
```

---

## 10. V1 与 V2 对比

| 方面 | V1 (全内存) | V2 (持久化 + KVCache) |
|------|-------------|----------------------|
| 存储 | 内存数组 | 文件 + KVCache |
| 内存占用 | 全量数据常驻 | 仅热点数据 |
| 首次填充 | 填充到内存 | 写入文件 |
| 查询延迟 | O(1) 内存访问 | 命中 O(1)，未命中需 I/O |
| 重启恢复 | 需重新填充 | 直接加载文件头 |
| 数据淘汰 | 无 | LRU 自动淘汰 |
| 实现复杂度 | 简单 | 中等 |

---

## 11. 开发工作量估算

| 功能 | 工作量 |
|------|--------|
| 文件格式定义 | 0.5 天 |
| 持久化写入 | 1 天 |
| KVCache Key/Value | 0.5 天 |
| 加载与查询 | 1 天 |
| 过滤/回表集成 | 1 天 |
| 测试与调试 | 1 天 |
| **总计** | **5 天** |

---

## 12. 测试要点

1. **持久化**：数据写入后能正确读取
2. **KVCache 命中**：首次未命中，二次命中
3. **过滤正确性**：使用 KVCache 数据过滤结果正确
4. **回表正确性**：使用 KVCache 数据回表结果正确
5. **重启恢复**：重启后能直接加载使用
6. **LRU 淘汰**：大量数据时，旧数据被正确淘汰
7. **并发访问**：多线程并发查询

---

## 13. 后续文档

- **[FTS_GENERIC_CACHE_FILTER_V3_DESIGN.md](./FTS_GENERIC_CACHE_FILTER_V3_DESIGN.md)**：V3 生产版，解决多表、多租户、缓存失效问题

---

## 14. 实现验证结果

### 14.1 功能验证

| 测试项 | 状态 | 说明 |
|--------|------|------|
| 持久化写入 | ✅ 通过 | 文件正确生成于 `/data/obcluster/store/fts_scalar_cache/` |
| 文件格式校验 | ✅ 通过 | Header magic、version、row_count、column_count 正确 |
| Memory Mode 过滤 | ✅ 通过 | DDL 后立即查询，使用内存缓存 |
| Memory Mode 回表 | ✅ 通过 | DDL 后立即查询，使用内存缓存 |
| KVCache Mode 过滤 | ✅ 通过 | 重启后查询，从文件加载 |
| KVCache Mode 回表 | ✅ 通过 | 重启后查询，从文件加载 |
| 过滤条件提取 | ✅ 通过 | 两种模式都能正确提取 |
| 列索引映射 | ✅ 通过 | 使用 `find_column_by_type()` 正确映射 |

### 14.2 性能对比（预期）

| 场景 | V1 (全内存) | V2 (持久化 + KVCache) |
|------|-------------|----------------------|
| DDL 后首次查询 | 直接使用 | 直接使用（Memory Mode）|
| 服务器重启后首次查询 | 需重建缓存 | 直接加载文件头（~1ms）|
| 热点数据查询 | O(1) | O(1)（KVCache 命中）|
| 冷数据查询 | O(1) | O(1) + 磁盘 I/O |
| 内存占用（20万行）| ~100 MB | 仅热点数据（由 LRU 控制）|

---

**文档版本**：v2.1（实现完成版）  
**创建日期**：2025-12-14  
**实现完成日期**：2025-12-16  
**依赖文档**：FTS_GENERIC_CACHE_FILTER_DESIGN.md (V1)  
**参考文档**：BM25S_PHASE2_PERSISTENCE.md, BM25S_PHASE3_KVCACHE_QUERY.md  
**作者**：FTS 优化开发组
