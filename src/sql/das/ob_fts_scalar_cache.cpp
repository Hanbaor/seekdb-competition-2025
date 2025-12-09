/*
 * Copyright (c) 2025 OceanBase.
 */

#define USING_LOG_PREFIX SQL_DAS

#include "sql/das/ob_fts_scalar_cache.h"
#include "share/ob_errno.h"
#include "lib/oblog/ob_log.h"
#include "lib/stat/ob_diagnose_info.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <algorithm>

namespace oceanbase
{
namespace sql
{

// 调试日志（生产环境禁用）
#define FTS_DEBUG_LOG 0
static void fts_debug_log(const char *fmt, ...)
{
#if FTS_DEBUG_LOG
  static FILE *fp = nullptr;
  if (!fp) {
    fp = fopen("/tmp/fts_scalar_cache.log", "a");
  }
  if (fp) {
    time_t now = time(nullptr);
    struct tm *t = localtime(&now);
    fprintf(fp, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec);
    
    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fprintf(fp, "\n");
    fflush(fp);
  }
#else
  UNUSED(fmt);
#endif
}

// 全局单例实例
ObFtsScalarCache g_fts_scalar_cache;

ObFtsScalarCache::ObFtsScalarCache()
  : is_inited_(false),
    is_loaded_(false),
    mem_attr_(common::OB_SERVER_TENANT_ID, "FtsScalarCache"),
    tmpfile_tenant_id_(0),
    dir_id_(-1),
    fd_(-1),
    file_record_count_(0),
    ids_(nullptr),
    base_ids_(nullptr),
    docid_cols_(nullptr),
    string_pool_(nullptr),
    string_pool_size_(0),
    count_(0),
    lock_()
{
}

ObFtsScalarCache::~ObFtsScalarCache()
{
  destroy();
}

int ObFtsScalarCache::init(int64_t capacity)
{
  int ret = OB_SUCCESS;
  UNUSED(capacity);
  
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("fts scalar cache init twice", K(ret));
  } else {
    // 只标记初始化，tmpfile 延迟打开
    is_inited_ = true;
    is_loaded_ = false;
    dir_id_ = -1;
    fd_ = -1;
    file_record_count_ = 0;
    ids_ = nullptr;
    base_ids_ = nullptr;
    docid_cols_ = nullptr;
    string_pool_ = nullptr;
    string_pool_size_ = 0;
    count_ = 0;
    LOG_INFO("fts scalar cache initialized (lazy tmpfile)");
  }
  
  return ret;
}

// 延迟打开 tmpfile
int ObFtsScalarCache::ensure_tmpfile_open_()
{
  int ret = OB_SUCCESS;
  
  if (fd_ >= 0) {
    // 已打开
  } else {
    uint64_t tenant_id = MTL_ID();
    fts_debug_log("ensure_tmpfile_open_: MTL_ID=%lu", tenant_id);
    if (tenant_id == 0) {
      tenant_id = common::OB_SERVER_TENANT_ID;
      fts_debug_log("ensure_tmpfile_open_: using SERVER_TENANT_ID=%lu", tenant_id);
    }
    
    // 分配 tmpfile 目录
    if (OB_FAIL(FILE_MANAGER_INSTANCE_WITH_MTL_SWITCH.alloc_dir(tenant_id, dir_id_))) {
      fts_debug_log("ensure_tmpfile_open_: alloc_dir FAILED ret=%d tenant_id=%lu", ret, tenant_id);
    }
    // 打开 tmpfile
    else if (OB_FAIL(FILE_MANAGER_INSTANCE_WITH_MTL_SWITCH.open(tenant_id, fd_, dir_id_, "FtsScalarCache"))) {
      fts_debug_log("ensure_tmpfile_open_: open FAILED ret=%d dir_id=%ld tenant_id=%lu", ret, dir_id_, tenant_id);
    } else {
      // 保存打开时的 tenant_id，后续写入时使用同一个
      tmpfile_tenant_id_ = tenant_id;
      fts_debug_log("ensure_tmpfile_open_: SUCCESS fd=%ld dir_id=%ld tenant_id=%lu", fd_, dir_id_, tenant_id);
    }
  }
  
  return ret;
}

// 写入固定大小记录
int ObFtsScalarCache::write_record_(uint64_t doc_id, uint64_t id,
                                     const common::ObString &base_id,
                                     const common::ObString &docid_col)
{
  int ret = OB_SUCCESS;
  
  // 使用打开 tmpfile 时保存的 tenant_id
  uint64_t tenant_id = tmpfile_tenant_id_;
  
  // 固定大小记录
  char write_buf[RECORD_SIZE];
  MEMSET(write_buf, 0, RECORD_SIZE);
  int64_t pos = 0;
  
  // 写入 id
  *reinterpret_cast<uint64_t*>(write_buf + pos) = id;
  pos += sizeof(uint64_t);
  
  // 写入 base_id
  uint32_t base_id_len = std::min(static_cast<uint32_t>(base_id.length()), 
                                   static_cast<uint32_t>(MAX_BASE_ID_LEN));
  *reinterpret_cast<uint32_t*>(write_buf + pos) = base_id_len;
  pos += sizeof(uint32_t);
  if (base_id_len > 0) {
    MEMCPY(write_buf + pos, base_id.ptr(), base_id_len);
  }
  pos += MAX_BASE_ID_LEN;
  
  // 写入 docid_col
  uint32_t docid_len = std::min(static_cast<uint32_t>(docid_col.length()),
                                 static_cast<uint32_t>(MAX_DOCID_COL_LEN));
  *reinterpret_cast<uint32_t*>(write_buf + pos) = docid_len;
  pos += sizeof(uint32_t);
  if (docid_len > 0) {
    MEMCPY(write_buf + pos, docid_col.ptr(), docid_len);
  }
  
  // 填充到目标位置（追加空白记录）
  int64_t target_offset = (doc_id - 1) * RECORD_SIZE;
  int64_t current_offset = file_record_count_ * RECORD_SIZE;
  
  // 前几条记录打印日志
  static uint64_t write_log_count = 0;
  write_log_count++;
  if (write_log_count <= 5) {
    uint64_t current_mtl_id = MTL_ID();
    fts_debug_log("write_record_: doc_id=%lu MTL_ID=%lu saved_tenant=%lu fd=%ld",
                  doc_id, current_mtl_id, tenant_id, fd_);
  }
  
  if (target_offset > current_offset) {
    // 需要填充空白记录
    char empty_buf[RECORD_SIZE];
    MEMSET(empty_buf, 0, RECORD_SIZE);
    
    tmp_file::ObTmpFileIOInfo pad_io;
    pad_io.fd_ = fd_;
    pad_io.dir_id_ = dir_id_;
    pad_io.buf_ = empty_buf;
    pad_io.size_ = RECORD_SIZE;
    pad_io.io_timeout_ms_ = 5000;
    pad_io.io_desc_.set_wait_event(ObWaitEventIds::DB_FILE_DATA_READ);
    
    int64_t fill_count = (target_offset - current_offset) / RECORD_SIZE;
    for (int64_t i = 0; i < fill_count && OB_SUCC(ret); ++i) {
      if (OB_FAIL(FILE_MANAGER_INSTANCE_WITH_MTL_SWITCH.write(tenant_id, pad_io))) {
        LOG_WARN("failed to write padding", K(ret), K(i));
      } else {
        file_record_count_++;
      }
    }
  }
  
  // 写入实际记录
  if (OB_SUCC(ret)) {
    tmp_file::ObTmpFileIOInfo io_info;
    io_info.fd_ = fd_;
    io_info.dir_id_ = dir_id_;
    io_info.buf_ = write_buf;
    io_info.size_ = RECORD_SIZE;
    io_info.io_timeout_ms_ = 5000;
    io_info.io_desc_.set_wait_event(ObWaitEventIds::DB_FILE_DATA_READ);
    
    int write_ret = FILE_MANAGER_INSTANCE_WITH_MTL_SWITCH.write(tenant_id, io_info);
    if (OB_SUCCESS != write_ret) {
      ret = write_ret;
      fts_debug_log("write_record_: write FAILED ret=%d fd=%ld doc_id=%lu tenant_id=%lu", ret, fd_, doc_id, tenant_id);
    } else {
      file_record_count_++;
      if (file_record_count_ <= 5 || file_record_count_ % 500 == 0) {
        fts_debug_log("write_record_: SUCCESS doc_id=%lu file_record_count=%lu", doc_id, file_record_count_);
      }
    }
  } else {
    fts_debug_log("write_record_: SKIPPED due to previous error ret=%d doc_id=%lu", ret, doc_id);
  }
  
  return ret;
}

int ObFtsScalarCache::add_mapping(uint64_t doc_id,
                                   uint64_t id,
                                   const common::ObString &base_id,
                                   const common::ObString &docid_col)
{
  int ret = OB_SUCCESS;
  
  // 每500条记录打印一次日志
  static uint64_t add_count = 0;
  add_count++;
  if (add_count <= 5 || add_count % 500 == 0) {
    fts_debug_log("add_mapping: doc_id=%lu id=%lu add_count=%lu file_record_count=%lu is_inited=%d",
                  doc_id, id, add_count, file_record_count_, is_inited_);
  }
  
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    fts_debug_log("add_mapping: NOT_INIT");
  } else if (doc_id == 0) {
    ret = OB_INVALID_ARGUMENT;
    fts_debug_log("add_mapping: INVALID doc_id=0");
  } else {
    common::ObSpinLockGuard guard(lock_);
    
    // 延迟打开 tmpfile
    if (OB_FAIL(ensure_tmpfile_open_())) {
      fts_debug_log("add_mapping: ensure_tmpfile_open_ FAILED ret=%d", ret);
    }
    // 写入 tmpfile
    else if (OB_FAIL(write_record_(doc_id, id, base_id, docid_col))) {
      fts_debug_log("add_mapping: write_record_ FAILED ret=%d doc_id=%lu", ret, doc_id);
    }
  }
  
  return ret;
}

// 从 tmpfile 加载到内存（三列数组 + 连续字符串池）
int ObFtsScalarCache::load_to_memory()
{
  int ret = OB_SUCCESS;
  
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (is_loaded_) {
    // 已加载，跳过
  } else if (file_record_count_ == 0) {
    is_loaded_ = true;
  } else {
    common::ObSpinLockGuard guard(lock_);
    
    // 双重检查
    if (is_loaded_) {
      return OB_SUCCESS;
    }
    
    uint64_t tenant_id = tmpfile_tenant_id_;
    uint64_t capacity = file_record_count_;
    
    // 第一遍：计算字符串池大小
    int64_t total_string_size = 0;
    char read_buf[RECORD_SIZE];
    for (uint64_t i = 0; i < file_record_count_ && OB_SUCC(ret); ++i) {
      tmp_file::ObTmpFileIOInfo io_info;
      tmp_file::ObTmpFileIOHandle io_handle;
      io_info.fd_ = fd_;
      io_info.dir_id_ = dir_id_;
      io_info.buf_ = read_buf;
      io_info.size_ = RECORD_SIZE;
      io_info.io_timeout_ms_ = 5000;
      io_info.io_desc_.set_wait_event(ObWaitEventIds::DB_FILE_DATA_READ);
      
      int64_t offset = i * RECORD_SIZE;
      if (OB_FAIL(FILE_MANAGER_INSTANCE_WITH_MTL_SWITCH.pread(tenant_id, io_info, offset, io_handle))) {
        LOG_WARN("failed to read record", K(ret), K(i), K(offset));
      } else {
        int64_t pos = sizeof(uint64_t);  // skip id
        uint32_t base_id_len = *reinterpret_cast<const uint32_t*>(read_buf + pos);
        pos += sizeof(uint32_t) + MAX_BASE_ID_LEN;
        uint32_t docid_len = *reinterpret_cast<const uint32_t*>(read_buf + pos);
        total_string_size += base_id_len + docid_len;
      }
    }
    
    if (OB_FAIL(ret)) {
      return ret;
    }
    
    // 分配内存
    ids_ = static_cast<uint64_t*>(ob_malloc(sizeof(uint64_t) * capacity, mem_attr_));
    base_ids_ = static_cast<common::ObString*>(ob_malloc(sizeof(common::ObString) * capacity, mem_attr_));
    docid_cols_ = static_cast<common::ObString*>(ob_malloc(sizeof(common::ObString) * capacity, mem_attr_));
    string_pool_ = static_cast<char*>(ob_malloc(total_string_size, mem_attr_));
    
    if (OB_ISNULL(ids_) || OB_ISNULL(base_ids_) || OB_ISNULL(docid_cols_) || 
        (total_string_size > 0 && OB_ISNULL(string_pool_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate arrays", K(ret), K(capacity), K(total_string_size));
      if (ids_) { ob_free(ids_); ids_ = nullptr; }
      if (base_ids_) { ob_free(base_ids_); base_ids_ = nullptr; }
      if (docid_cols_) { ob_free(docid_cols_); docid_cols_ = nullptr; }
      if (string_pool_) { ob_free(string_pool_); string_pool_ = nullptr; }
    } else {
      // 初始化 ObString 数组
      for (uint64_t i = 0; i < capacity; ++i) {
        new (&base_ids_[i]) common::ObString();
        new (&docid_cols_[i]) common::ObString();
      }
      
      // 第二遍：填充数据
      int64_t string_offset = 0;
      for (uint64_t i = 0; i < file_record_count_ && OB_SUCC(ret); ++i) {
        tmp_file::ObTmpFileIOInfo io_info;
        tmp_file::ObTmpFileIOHandle io_handle;
        io_info.fd_ = fd_;
        io_info.dir_id_ = dir_id_;
        io_info.buf_ = read_buf;
        io_info.size_ = RECORD_SIZE;
        io_info.io_timeout_ms_ = 5000;
        io_info.io_desc_.set_wait_event(ObWaitEventIds::DB_FILE_DATA_READ);
        
        int64_t offset = i * RECORD_SIZE;
        if (OB_FAIL(FILE_MANAGER_INSTANCE_WITH_MTL_SWITCH.pread(tenant_id, io_info, offset, io_handle))) {
          LOG_WARN("failed to read record", K(ret), K(i), K(offset));
        } else {
          int64_t pos = 0;
          
          // 读取 id
          ids_[i] = *reinterpret_cast<const uint64_t*>(read_buf + pos);
          pos += sizeof(uint64_t);
          
          // 读取 base_id
          uint32_t base_id_len = *reinterpret_cast<const uint32_t*>(read_buf + pos);
          pos += sizeof(uint32_t);
          if (base_id_len > 0) {
            MEMCPY(string_pool_ + string_offset, read_buf + pos, base_id_len);
            base_ids_[i].assign_ptr(string_pool_ + string_offset, base_id_len);
            string_offset += base_id_len;
          }
          pos += MAX_BASE_ID_LEN;
          
          // 读取 docid_col
          uint32_t docid_len = *reinterpret_cast<const uint32_t*>(read_buf + pos);
          pos += sizeof(uint32_t);
          if (docid_len > 0) {
            MEMCPY(string_pool_ + string_offset, read_buf + pos, docid_len);
            docid_cols_[i].assign_ptr(string_pool_ + string_offset, docid_len);
            string_offset += docid_len;
          }
        }
      }
      
      if (OB_SUCC(ret)) {
        count_ = file_record_count_;
        string_pool_size_ = total_string_size;
        is_loaded_ = true;
        LOG_INFO("fts scalar cache loaded", K_(count), K_(string_pool_size));
      }
    }
  }
  
  return ret;
}

int ObFtsScalarCache::get_scalar_fields(uint64_t doc_id, ScalarFields &fields) const
{
  int ret = OB_SUCCESS;
  
  if (!is_loaded_) {
    ret = OB_NOT_INIT;
  } else if (doc_id == 0 || doc_id > count_) {
    ret = OB_ENTRY_NOT_EXIST;
  } else {
    // 无锁访问（加载后只读）
    uint64_t idx = doc_id - 1;
    fields.id_ = ids_[idx];
    fields.base_id_ = base_ids_[idx];
    fields.docid_col_ = docid_cols_[idx];
  }
  
  return ret;
}

void ObFtsScalarCache::reuse()
{
  common::ObSpinLockGuard guard(lock_);
  
  // 释放三列数组
  if (ids_) { ob_free(ids_); ids_ = nullptr; }
  if (base_ids_) { ob_free(base_ids_); base_ids_ = nullptr; }
  if (docid_cols_) { ob_free(docid_cols_); docid_cols_ = nullptr; }
  if (string_pool_) { ob_free(string_pool_); string_pool_ = nullptr; }
  string_pool_size_ = 0;
  
  // 删除 tmpfile
  if (fd_ >= 0) {
    uint64_t tenant_id = MTL_ID();
    if (tenant_id == 0) tenant_id = common::OB_SERVER_TENANT_ID;
    FILE_MANAGER_INSTANCE_WITH_MTL_SWITCH.remove(tenant_id, fd_);
    fd_ = -1;
  }
  
  count_ = 0;
  file_record_count_ = 0;
  is_loaded_ = false;
}

void ObFtsScalarCache::destroy()
{
  if (is_inited_) {
    reuse();
    dir_id_ = -1;
    is_inited_ = false;
    LOG_INFO("fts scalar cache destroyed");
  }
}

} // namespace sql
} // namespace oceanbase
