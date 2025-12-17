/*
 * Copyright (c) 2025 OceanBase.
 * FTS 通用缓存 - 支持任意数量、任意类型的列缓存
 */

#ifndef OB_FTS_GENERIC_CACHE_H_
#define OB_FTS_GENERIC_CACHE_H_

#include "lib/container/ob_array.h"
#include "lib/allocator/page_arena.h"
#include "lib/lock/ob_spin_lock.h"
#include "lib/oblog/ob_log_module.h"
#include "sql/das/ob_fts_column_value.h"
#include "sql/das/ob_fts_scalar_kvcache.h"
#include "sql/engine/expr/ob_expr.h"
#include "share/ob_io_device_helper.h"
#include "common/storage/ob_io_device.h"
#include "share/cache/ob_kv_storecache.h"

namespace oceanbase
{
namespace sql
{

/**
 * 列元数据 - 描述缓存中每一列的信息
 *
 * 重要：使用 type_signature_ 进行跨阶段列匹配
 * type_signature_ = (obj_type << 16) | type_order
 * - obj_type: 列的数据类型
 * - type_order: 该类型在所有同类型列中的序号（0, 1, 2...）
 * 这样可以确保 DDL 阶段和查询阶段能正确匹配同一列
 */
struct ObFtsColumnMeta
{
  int64_t output_idx_;           // 在 output_exprs 中的索引（仅调试用，不用于匹配）
  common::ObObjType obj_type_;   // 列的数据类型
  int64_t type_order_;           // 同类型列中的序号（用于匹配）
  int64_t type_signature_;       // 类型签名 = (obj_type << 16) | type_order
  common::ObString column_name_; // 列名（可选，用于调试）
  bool is_primary_key_;          // 是否是主键列
  bool is_nullable_;             // 是否允许 NULL

  ObFtsColumnMeta()
    : output_idx_(-1),
      obj_type_(common::ObNullType),
      type_order_(0),
      type_signature_(0),
      column_name_(),
      is_primary_key_(false),
      is_nullable_(true)
  {}

  // 计算类型签名
  void compute_signature()
  {
    type_signature_ = (static_cast<int64_t>(obj_type_) << 16) | (type_order_ & 0xFFFF);
  }

  // 从类型和序号创建签名（静态方法）
  static int64_t make_signature(common::ObObjType type, int64_t order)
  {
    return (static_cast<int64_t>(type) << 16) | (order & 0xFFFF);
  }

  TO_STRING_KV(K_(output_idx), K_(obj_type), K_(type_order), K_(type_signature), K_(is_primary_key));
};

/**
 * 行数据 - 存储一行的所有列值
 */
struct ObFtsRowData
{
  ObFtsColumnValue *columns_;    // 列值数组
  int64_t column_count_;         // 列数量

  ObFtsRowData() : columns_(nullptr), column_count_(0) {}

  int init(int64_t col_count, common::ObIAllocator &alloc)
  {
    int ret = common::OB_SUCCESS;
    if (col_count <= 0) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      columns_ = static_cast<ObFtsColumnValue*>(
          alloc.alloc(sizeof(ObFtsColumnValue) * col_count));
      if (OB_ISNULL(columns_)) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else {
        for (int64_t i = 0; i < col_count; ++i) {
          new (&columns_[i]) ObFtsColumnValue();
        }
        column_count_ = col_count;
      }
    }
    return ret;
  }

  void set_column(int64_t idx, const ObFtsColumnValue &val)
  {
    if (idx >= 0 && idx < column_count_ && OB_NOT_NULL(columns_)) {
      columns_[idx] = val;
    }
  }

  const ObFtsColumnValue& get_column(int64_t idx) const
  {
    static ObFtsColumnValue null_val;
    if (idx >= 0 && idx < column_count_ && OB_NOT_NULL(columns_)) {
      return columns_[idx];
    }
    return null_val;
  }

  void reset()
  {
    columns_ = nullptr;
    column_count_ = 0;
  }
};

/**
 * 通用全文检索列缓存
 *
 * 设计特点：
 * 1. 支持任意数量、任意类型的列
 * 2. 使用 doc_id 作为主键，O(1) 访问
 * 3. 列式存储，内存高效
 * 4. 线程安全
 */
class ObFtsGenericCache
{
public:
  static const int64_t DEFAULT_CAPACITY = 200000;
  static const int64_t MAX_COLUMNS = 64;  // 最多支持 64 列

  ObFtsGenericCache();
  ~ObFtsGenericCache();

  /**
   * 初始化缓存
   * @param column_metas 列元数据数组，定义要缓存的列
   * @param capacity 初始容量
   */
  int init(const common::ObIArray<ObFtsColumnMeta> &column_metas,
           int64_t capacity = DEFAULT_CAPACITY);

  /**
   * 添加一行数据
   * @param doc_id 文档 ID（作为主键）
   * @param column_values 列值数组，长度必须等于 column_count
   */
  int add_row(uint64_t doc_id, const ObFtsColumnValue *column_values);

  /**
   * 从 ObExpr 和 ObEvalCtx 直接添加行
   * 自动根据 column_metas 中的 output_idx 提取数据
   */
  int add_row_from_exprs(uint64_t doc_id,
                         const common::ObIArray<ObExpr*> &output_exprs,
                         ObEvalCtx &eval_ctx);

  /**
   * 获取指定文档的指定列值
   */
  int get_column_value(uint64_t doc_id, int64_t column_idx,
                       ObFtsColumnValue &out) const;

  /**
   * 获取指定文档的整行数据
   */
  int get_row(uint64_t doc_id, ObFtsRowData &out) const;

  /**
   * 批量获取列值（用于缓存回表场景）
   * @param doc_ids 文档 ID 数组
   * @param column_idx 要获取的列索引
   * @param out 输出数组
   */
  int batch_get_column(const common::ObIArray<uint64_t> &doc_ids,
                       int64_t column_idx,
                       common::ObIArray<ObFtsColumnValue> &out) const;

  // ========== 快速访问方法（无锁，调用者需确保参数有效）==========

  /**
   * 快速获取列值（内联，无锁，用于高频访问场景）
   */
  inline const ObFtsColumnValue& get_column_fast(uint64_t doc_id, int64_t col_idx) const
  {
    static ObFtsColumnValue null_val;
    if (doc_id > 0 && doc_id <= count_ &&
        col_idx >= 0 && col_idx < column_count_ &&
        OB_NOT_NULL(column_data_[col_idx])) {
      return column_data_[col_idx][doc_id - 1];
    }
    return null_val;
  }

  /**
   * 快速获取字符串列的 ObString
   */
  inline common::ObString get_string_column_fast(uint64_t doc_id, int64_t col_idx) const
  {
    if (doc_id > 0 && doc_id <= count_ &&
        col_idx >= 0 && col_idx < column_count_ &&
        OB_NOT_NULL(column_data_[col_idx])) {
      const ObFtsColumnValue &val = column_data_[col_idx][doc_id - 1];
      if (val.type_ == ObFtsColumnValue::TYPE_STRING) {
        return common::ObString(val.str_val_.len_, val.str_val_.ptr_);
      }
    }
    return common::ObString();
  }

  /**
   * 快速获取整数列
   */
  inline int64_t get_int_column_fast(uint64_t doc_id, int64_t col_idx) const
  {
    if (doc_id > 0 && doc_id <= count_ &&
        col_idx >= 0 && col_idx < column_count_ &&
        OB_NOT_NULL(column_data_[col_idx])) {
      const ObFtsColumnValue &val = column_data_[col_idx][doc_id - 1];
      if (val.type_ == ObFtsColumnValue::TYPE_INT64) {
        return val.int_val_;
      } else if (val.type_ == ObFtsColumnValue::TYPE_UINT64) {
        return static_cast<int64_t>(val.uint_val_);
      }
    }
    return 0;
  }

  // ========== 状态查询 ==========
  bool is_inited() const { return is_inited_; }
  int64_t size() const { return count_; }
  int64_t capacity() const { return capacity_; }
  int64_t column_count() const { return column_count_; }
  const ObFtsColumnMeta& get_column_meta(int64_t idx) const;

  /**
   * 根据类型签名查找列索引
   * @param type_signature 类型签名 = (obj_type << 16) | type_order
   * @return 列索引，未找到返回 -1
   */
  int64_t find_column_by_signature(int64_t type_signature) const
  {
    for (int64_t i = 0; i < column_count_; ++i) {
      if (column_metas_[i].type_signature_ == type_signature) {
        return i;
      }
    }
    return -1;
  }

  /**
   * 根据类型和序号查找列索引
   * @param obj_type 列类型
   * @param type_order 同类型列中的序号
   * @return 列索引，未找到返回 -1
   */
  int64_t find_column_by_type(common::ObObjType obj_type, int64_t type_order) const
  {
    int64_t signature = ObFtsColumnMeta::make_signature(obj_type, type_order);
    return find_column_by_signature(signature);
  }

  // 兼容旧接口
  bool is_loaded() const { return is_inited_; }
  int load_to_memory() { return common::OB_SUCCESS; }

  // ========== 生命周期管理 ==========
  void reuse();
  void destroy();

  // ========== V2: 持久化接口 ==========
  
  /**
   * 获取缓存文件路径
   * @param path_buf 输出路径缓冲区
   * @param buf_len 缓冲区长度
   */
  static int get_cache_path(char *path_buf, int64_t buf_len);
  
  /**
   * 将缓存持久化到磁盘（一次性写入，需要完整内存缓存）
   * @param cache_dir 缓存目录路径
   */
  int persist_to_disk(const char *cache_dir);
  
  /**
   * 从磁盘加载缓存
   * @param cache_dir 缓存目录路径
   */
  int load_from_disk(const char *cache_dir);
  
  /**
   * 关闭索引文件
   */
  void close_cache_file();
  
  /**
   * 检查缓存文件是否已打开
   */
  bool is_cache_file_opened() const { return persist_file_opened_; }
  
  /**
   * 获取文件头信息
   */
  const FtsScalarCacheFileHeader& get_file_header() const { return file_header_; }
  
  // ========== V3: 流式存储接口（减少内存占用）==========
  
  /**
   * 初始化流式写入
   * 打开持久化文件，预留 Header 和 Column Meta 空间
   * @param cache_dir 缓存目录路径
   * @param column_metas 列元数据数组
   * @param estimated_rows 预估行数（用于预分配 Row Index 缓冲区）
   */
  int init_streaming_persist(const char *cache_dir,
                             const common::ObIArray<ObFtsColumnMeta> &column_metas,
                             int64_t estimated_rows = DEFAULT_CAPACITY);
  
  /**
   * 流式写入一行数据
   * 数据直接写入文件，不在内存中保留
   * @param doc_id 文档 ID（作为主键）
   * @param column_values 列值数组，长度必须等于 column_count
   */
  int streaming_add_row(uint64_t doc_id, const ObFtsColumnValue *column_values);
  
  /**
   * 从 ObExpr 和 ObEvalCtx 流式写入一行数据
   */
  int streaming_add_row_from_exprs(uint64_t doc_id,
                                   const common::ObIArray<ObExpr*> &output_exprs,
                                   ObEvalCtx &eval_ctx);
  
  /**
   * 结束流式写入
   * 写入 Row Index，更新 Header，关闭文件
   */
  int finish_streaming_persist();
  
  /**
   * 检查是否处于流式写入模式
   */
  bool is_streaming_mode() const { return streaming_mode_; }
  
  // ========== V2: KVCache 查询接口 ==========
  
  /**
   * 从 KVCache 获取行数据
   * @param doc_id 文档 ID
   * @param row 输出：行数据指针
   * @param handle 缓存句柄（管理生命周期）
   */
  int get_row_from_kvcache(uint64_t doc_id,
                           const ObFtsScalarCacheValue *&row,
                           common::ObKVCacheHandle &handle);
  
  /**
   * 批量预取行数据到 KVCache
   */
  int prefetch_rows(const common::ObIArray<uint64_t> &doc_ids);
  
  /**
   * 确保缓存就绪（如果未就绪，尝试从磁盘加载）
   */
  int ensure_ready();

private:
  int ensure_capacity(uint64_t required);
  int copy_column_value(const ObFtsColumnValue &src, ObFtsColumnValue &dst);
  
  // ========== V2: 持久化私有方法 ==========
  int write_header(const common::ObIOFd &fd);
  int write_column_meta(const common::ObIOFd &fd);
  int write_row_index_and_data(const common::ObIOFd &fd);
  int update_header(const common::ObIOFd &fd);
  int serialize_row(uint64_t doc_id, char *buf, int64_t buf_len, int64_t &row_size);
  
  // ========== V2: KVCache 私有方法 ==========
  int load_row_to_kvcache(uint64_t doc_id,
                          const ObFtsScalarCacheValue *&row,
                          common::ObKVCacheHandle &handle);
  
  // ========== V3: 流式存储私有方法 ==========
  int serialize_row_from_values(const ObFtsColumnValue *column_values, 
                                char *buf, int64_t buf_len, int64_t &row_size);

private:
  bool is_inited_;
  lib::ObMemAttr mem_attr_;
  common::ObArenaAllocator allocator_;  // 用于字符串等变长数据

  // 列元数据
  ObFtsColumnMeta column_metas_[MAX_COLUMNS];
  int64_t column_count_;

  // 列式存储：每列一个数组
  // column_data_[col_idx][row_idx] = 第 row_idx 行的第 col_idx 列的值
  ObFtsColumnValue *column_data_[MAX_COLUMNS];

  uint64_t count_;     // 已存储的行数（最大 doc_id）
  uint64_t capacity_;  // 容量

  mutable common::ObSpinLock lock_;
  
  // ========== V2: 持久化相关成员 ==========
  char cache_file_path_[512];             // 缓存文件路径
  common::ObIOFd persist_fd_;             // 持久化文件描述符
  bool persist_file_opened_;              // 持久化文件是否已打开
  FtsScalarCacheFileHeader file_header_;  // 文件头
  FtsRowIndexEntry *row_index_;           // 行索引
  common::ObArenaAllocator cache_allocator_;  // KVCache 查询时的临时内存分配器
  
  // ========== V3: 流式存储相关成员 ==========
  bool streaming_mode_;                   // 是否处于流式写入模式
  common::ObIOFd streaming_fd_;           // 流式写入文件描述符
  int64_t streaming_data_offset_;         // 当前 Row Data 写入位置（相对于 row_data_start）
  int64_t streaming_row_data_start_;      // Row Data 区域的固定起始位置（文件偏移）
  FtsRowIndexEntry *streaming_row_index_; // 流式写入时的行索引缓冲区
  int64_t streaming_row_index_capacity_;  // 行索引缓冲区容量
  char *streaming_row_buf_;               // 行数据序列化缓冲区
  static const int64_t STREAMING_ROW_BUF_SIZE = 8192;  // 单行最大 8KB
  
  // ========== 并行任务计数 ==========
  int64_t active_collectors_;             // 活跃的收集任务数
  
public:
  // ========== 统一状态检查 ==========
  // IDLE:      !streaming_mode_ && !persist_file_opened_
  // STREAMING: streaming_mode_
  // READY:     !streaming_mode_ && persist_file_opened_
  bool is_idle() const { return !streaming_mode_ && !persist_file_opened_; }
  bool is_ready() const { return !streaming_mode_ && persist_file_opened_; }
  
  // ========== 并行任务管理（与 BM25S 同步）==========
  void begin_collect() { ATOMIC_AAF(&active_collectors_, 1); }
  bool end_collect() { return ATOMIC_SAF(&active_collectors_, 1) == 0; }
  bool can_finalize() const { return ATOMIC_LOAD(&active_collectors_) == 0; }
};

// 全局单例
extern ObFtsGenericCache g_fts_generic_cache;

} // namespace sql
} // namespace oceanbase

#endif // OB_FTS_GENERIC_CACHE_H_
