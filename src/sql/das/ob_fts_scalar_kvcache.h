/*
 * Copyright (c) 2025 OceanBase.
 *
 * FTS Scalar Cache KVCache 定义
 * V2 版本：支持持久化和按需加载
 */

#ifndef OCEANBASE_SQL_DAS_OB_FTS_SCALAR_KVCACHE_H_
#define OCEANBASE_SQL_DAS_OB_FTS_SCALAR_KVCACHE_H_

#include "share/cache/ob_kvcache_struct.h"
#include "share/cache/ob_kv_storecache.h"
#include "lib/string/ob_string.h"
#include "lib/hash/ob_hashutils.h"
#include "lib/utility/ob_print_utils.h"
#include "sql/das/ob_fts_column_value.h"

namespace oceanbase
{
namespace sql
{

// ========================================================================
// V2 Phase 2: 持久化文件格式
// ========================================================================

/**
 * @brief FTS Scalar Cache 文件头 (256 bytes)
 */
struct FtsScalarCacheFileHeader
{
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
  static const uint64_t MAGIC = 0x4654535343414348ULL;  // "FTSCACHE"
  static const int32_t CURRENT_VERSION = 1;
  static const int64_t HEADER_SIZE = 256;
  
  void reset() { memset(this, 0, sizeof(*this)); }
  bool is_valid() const { return magic == MAGIC && version == CURRENT_VERSION; }
  
  TO_STRING_KV(K(magic), K(version), K(row_count), K(column_count), K(max_doc_id),
               K(column_meta_offset), K(row_index_offset), K(row_data_offset));
};

/**
 * @brief Row Index 条目 (16 bytes)
 * 使用 doc_id 作为索引：row_index[doc_id - 1]
 */
struct FtsRowIndexEntry
{
  int64_t data_offset;         // 该行在 Row Data 区域的偏移
  int64_t data_size;           // 该行数据的大小
  
  void reset() { data_offset = 0; data_size = 0; }
  
  TO_STRING_KV(K(data_offset), K(data_size));
};

/**
 * @brief Column Meta 条目（持久化格式）
 */
struct FtsColumnMetaEntry
{
  int32_t column_type;         // ObObjType
  int32_t type_order;          // 同类型列中的序号
  int64_t type_signature;      // 类型签名
  int32_t is_primary_key;      // 是否主键
  int32_t is_nullable;         // 是否可空
  
  void reset() { memset(this, 0, sizeof(*this)); }
  
  TO_STRING_KV(K(column_type), K(type_order), K(type_signature));
};

// ========================================================================
// V2 Phase 3: KVCache Key/Value 定义
// ========================================================================

/**
 * @brief ScalarCache Key - 使用 doc_id 作为 Key
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
  
  virtual ~ObFtsScalarCacheKey() {}
  
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

/**
 * @brief ScalarCache Value - 存储一行的所有列值
 * 
 * 存储格式与文件中一致，方便零拷贝
 * 每列格式: [type:1byte][data:变长]
 */
class ObFtsScalarCacheValue : public common::ObIKVCacheValue
{
public:
  ObFtsScalarCacheValue()
    : column_count_(0), data_size_(0), data_(nullptr) {}
  
  virtual ~ObFtsScalarCacheValue() {}
  
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
  
  int64_t column_count() const { return column_count_; }
  int64_t data_size() const { return data_size_; }
  const char* data() const { return data_; }
  
  TO_STRING_KV(K_(column_count), K_(data_size));
  
public:
  int64_t column_count_;
  int64_t data_size_;
  char *data_;
};

// KVCache 类型定义
typedef common::ObKVCache<ObFtsScalarCacheKey, ObFtsScalarCacheValue> ObFtsScalarKVCache;

// ========================================================================
// 全局缓存管理函数声明
// ========================================================================

/**
 * @brief 初始化 FTS Scalar KVCache
 */
int init_fts_scalar_kvcache();

/**
 * @brief 销毁 FTS Scalar KVCache
 */
void destroy_fts_scalar_kvcache();

/**
 * @brief 检查 KVCache 是否已初始化
 */
bool is_fts_scalar_kvcache_inited();

/**
 * @brief 获取全局 ScalarKVCache 实例
 */
ObFtsScalarKVCache* get_fts_scalar_kvcache();

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_DAS_OB_FTS_SCALAR_KVCACHE_H_

