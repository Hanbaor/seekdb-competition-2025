# BM25S 索引构建 - Phase 1: TmpFile 分桶

## 1. 概述

### 1.1 目标
将当前使用标准 `FILE*` 的分桶临时文件机制，改造为使用 SeekDB 原生的 `ObTenantTmpFileManager` 接口。

### 1.2 背景
当前 `ObFtsBM25SIndex` 在构建阶段使用 128 个 `FILE*` 分桶文件存储原始数据：
```cpp
// 当前实现
FILE *bucket_files_[NUM_BUCKETS];  // 128 个标准 FILE*
char bucket_dir_path_[256];        // 桶文件目录路径
```

这违反了比赛约束："基于文件/共享内存的自定义缓存（未走 seekdb 接口）"。

### 1.3 约束
- 必须使用 `ObTenantTmpFileManager` 接口
- 桶数量根据数据量动态计算，保证单桶 < 200 MB
- 保持并发安全（多线程写入不同桶）

---

## 2. 数据结构

### 2.1 动态桶数量计算

```cpp
// 目标：单桶内存 < 200 MB
static const int64_t TARGET_BUCKET_SIZE_MB = 200;
static const int64_t RECORD_SIZE = sizeof(FileRecord);  // 20 bytes

// 根据预估数据量计算桶数量
int64_t calc_bucket_num(int64_t estimated_postings) {
    // 每桶最多容纳的记录数
    int64_t max_records_per_bucket = TARGET_BUCKET_SIZE_MB * 1024 * 1024 / RECORD_SIZE;
    // 需要的桶数量
    int64_t num = (estimated_postings + max_records_per_bucket - 1) / max_records_per_bucket;
    // 取 2 的幂次方，最小 128
    return next_power_of_2(std::max(128L, num));
}

// 不同数据规模的桶数量参考：
// - 1 亿文档（100 亿 posting）: ~1024 桶，单桶 ~100 MB
// - 10 亿文档（1000 亿 posting）: ~8192 桶，单桶 ~120 MB
// - 20 亿文档（2000 亿 posting）: ~16384 桶，单桶 ~120 MB
```

### 2.2 新增成员变量

```cpp
class ObFtsBM25SIndex {
private:
    // ========== 删除旧的 FILE* ==========
    // FILE *bucket_files_[NUM_BUCKETS];       // 删除
    // char bucket_dir_path_[256];             // 删除
    
    // ========== 新增 TmpFile ==========
    int64_t num_buckets_;                      // 动态计算的桶数量
    int64_t tmp_dir_id_;                       // TmpFile 目录 ID
    int64_t *bucket_fds_;                      // TmpFile 文件描述符数组（动态分配）
    int64_t *bucket_write_offsets_;            // 每个桶的当前写入偏移
    ObSpinLock *bucket_locks_;                 // 桶锁数组（动态分配）
    bool tmp_files_inited_;                    // TmpFile 是否已初始化
};
```

### 2.3 FileRecord 结构（保持不变）

```cpp
struct FileRecord {
    uint32_t token_id;
    int64_t doc_id;
    int32_t tf;
    int32_t doc_len;
    
    bool operator<(const FileRecord &other) const {
        return token_id < other.token_id || 
               (token_id == other.token_id && doc_id < other.doc_id);
    }
} __attribute__((packed));  // 20 字节
```

---

## 3. 接口设计

### 3.1 需要引入的头文件

```cpp
#include "storage/tmp_file/ob_tmp_file_manager.h"
#include "storage/tmp_file/ob_tmp_file_io_info.h"
```

### 3.2 核心接口

| 接口 | 说明 |
|-----|------|
| `init_tmp_files()` | 初始化 TmpFile 目录和文件 |
| `write_to_bucket(bucket_id, record)` | 写入一条记录到指定桶 |
| `read_bucket(bucket_id, records)` | 读取整个桶的所有记录 |
| `cleanup_tmp_files()` | 清理所有 TmpFile |

---

## 4. 详细实现

### 4.1 初始化 TmpFile

```cpp
int ObFtsBM25SIndex::init_tmp_files(int64_t estimated_postings)
{
    int ret = OB_SUCCESS;
    
    // 检查是否已初始化
    if (tmp_files_inited_) {
        return OB_SUCCESS;
    }
    
    // 1. 动态计算桶数量
    num_buckets_ = calc_bucket_num(estimated_postings);
    
    // 2. 动态分配数组
    bucket_fds_ = static_cast<int64_t*>(allocator_.alloc(num_buckets_ * sizeof(int64_t)));
    bucket_write_offsets_ = static_cast<int64_t*>(allocator_.alloc(num_buckets_ * sizeof(int64_t)));
    bucket_locks_ = static_cast<ObSpinLock*>(allocator_.alloc(num_buckets_ * sizeof(ObSpinLock)));
    
    if (OB_ISNULL(bucket_fds_) || OB_ISNULL(bucket_write_offsets_) || OB_ISNULL(bucket_locks_)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("fail to alloc bucket arrays", K(ret), K(num_buckets_));
        return ret;
    }
    
    // 初始化数组
    for (int64_t i = 0; i < num_buckets_; ++i) {
        bucket_fds_[i] = -1;
        bucket_write_offsets_[i] = 0;
        new (&bucket_locks_[i]) ObSpinLock();
    }
    
    // 3. 分配 TmpFile 目录
    ObTenantTmpFileManager *tmp_mgr = MTL(ObTenantTmpFileManager*);
    if (OB_ISNULL(tmp_mgr)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tmp file manager is null", K(ret));
        return ret;
    }
    
    if (OB_FAIL(tmp_mgr->alloc_dir(tmp_dir_id_))) {
        LOG_WARN("fail to alloc tmp dir", K(ret));
        return ret;
    }
    
    // 4. 打开 num_buckets_ 个临时文件
    for (int64_t i = 0; OB_SUCC(ret) && i < num_buckets_; ++i) {
        if (OB_FAIL(tmp_mgr->open(bucket_fds_[i], tmp_dir_id_, "bm25s_bucket"))) {
            LOG_WARN("fail to open tmp file", K(ret), K(i));
            // 清理已打开的文件
            for (int64_t j = 0; j < i; ++j) {
                tmp_mgr->remove(bucket_fds_[j]);
                bucket_fds_[j] = -1;
            }
            break;
        }
    }
    
    if (OB_SUCC(ret)) {
        tmp_files_inited_ = true;
        LOG_INFO("BM25S tmp files initialized", K(tmp_dir_id_), K(num_buckets_),
                 K(estimated_postings));
    }
    
    return ret;
}
```

### 4.2 写入记录到桶

```cpp
int ObFtsBM25SIndex::write_to_bucket(int64_t bucket_id, const FileRecord &record)
{
    int ret = OB_SUCCESS;
    
    // 参数检查
    if (bucket_id < 0 || bucket_id >= num_buckets_) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid bucket id", K(ret), K(bucket_id), K(num_buckets_));
        return ret;
    }
    
    if (!tmp_files_inited_ || bucket_fds_[bucket_id] < 0) {
        ret = OB_NOT_INIT;
        LOG_WARN("tmp file not initialized", K(ret), K(bucket_id));
        return ret;
    }
    
    // 构建 IO 信息
    ObTmpFileIOInfo io_info;
    io_info.fd_ = bucket_fds_[bucket_id];
    io_info.io_desc_.set_wait_event(ObWaitEventIds::DB_FILE_DATA_WRITE);
    io_info.buf_ = reinterpret_cast<char*>(const_cast<FileRecord*>(&record));
    io_info.size_ = sizeof(FileRecord);
    io_info.io_timeout_ms_ = 5000;  // 5 秒超时
    
    // 写入（append 模式，TmpFile 自动追加）
    ObTenantTmpFileManager *tmp_mgr = MTL(ObTenantTmpFileManager*);
    if (OB_FAIL(tmp_mgr->write(MTL_ID(), io_info))) {
        LOG_WARN("fail to write to tmp file", K(ret), K(bucket_id), K(sizeof(FileRecord)));
    } else {
        // 更新写入偏移（用于统计）
        ATOMIC_AAF(&bucket_write_offsets_[bucket_id], sizeof(FileRecord));
    }
    
    return ret;
}
```

### 4.3 批量写入优化（可选）

```cpp
// 批量写入多条记录，减少系统调用
int ObFtsBM25SIndex::write_records_to_bucket(int64_t bucket_id, 
                                              const FileRecord *records, 
                                              int64_t count)
{
    int ret = OB_SUCCESS;
    
    if (count <= 0 || OB_ISNULL(records)) {
        return OB_SUCCESS;
    }
    
    ObTmpFileIOInfo io_info;
    io_info.fd_ = bucket_fds_[bucket_id];
    io_info.io_desc_.set_wait_event(ObWaitEventIds::DB_FILE_DATA_WRITE);
    io_info.buf_ = reinterpret_cast<char*>(const_cast<FileRecord*>(records));
    io_info.size_ = count * sizeof(FileRecord);
    io_info.io_timeout_ms_ = 5000;
    
    ObTenantTmpFileManager *tmp_mgr = MTL(ObTenantTmpFileManager*);
    if (OB_FAIL(tmp_mgr->write(MTL_ID(), io_info))) {
        LOG_WARN("fail to batch write to tmp file", K(ret), K(bucket_id), K(count));
    } else {
        ATOMIC_AAF(&bucket_write_offsets_[bucket_id], count * sizeof(FileRecord));
    }
    
    return ret;
}
```

### 4.4 读取整个桶

```cpp
int ObFtsBM25SIndex::read_bucket(int64_t bucket_id, common::ObIArray<FileRecord> &records)
{
    int ret = OB_SUCCESS;
    
    if (bucket_id < 0 || bucket_id >= num_buckets_) {
        ret = OB_INVALID_ARGUMENT;
        return ret;
    }
    
    if (!tmp_files_inited_ || bucket_fds_[bucket_id] < 0) {
        ret = OB_NOT_INIT;
        return ret;
    }
    
    ObTenantTmpFileManager *tmp_mgr = MTL(ObTenantTmpFileManager*);
    
    // 1. 获取文件大小
    int64_t file_size = 0;
    if (OB_FAIL(tmp_mgr->get_tmp_file_size(bucket_fds_[bucket_id], file_size))) {
        LOG_WARN("fail to get tmp file size", K(ret), K(bucket_id));
        return ret;
    }
    
    if (file_size == 0) {
        // 空桶，直接返回
        return OB_SUCCESS;
    }
    
    // 2. 检查文件大小是否合法
    if (file_size % sizeof(FileRecord) != 0) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid tmp file size", K(ret), K(bucket_id), K(file_size), 
                 K(sizeof(FileRecord)));
        return ret;
    }
    
    int64_t record_count = file_size / sizeof(FileRecord);
    
    // 3. 分配读取缓冲区
    char *buf = static_cast<char*>(allocator_.alloc(file_size));
    if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("fail to alloc buffer", K(ret), K(file_size));
        return ret;
    }
    
    // 4. 读取整个文件
    ObTmpFileIOInfo io_info;
    io_info.fd_ = bucket_fds_[bucket_id];
    io_info.io_desc_.set_wait_event(ObWaitEventIds::DB_FILE_DATA_READ);
    io_info.buf_ = buf;
    io_info.size_ = file_size;
    io_info.io_timeout_ms_ = 30000;  // 30 秒超时（大文件）
    
    ObTmpFileIOHandle handle;
    if (OB_FAIL(tmp_mgr->pread(MTL_ID(), io_info, 0 /*offset*/, handle))) {
        LOG_WARN("fail to read tmp file", K(ret), K(bucket_id), K(file_size));
        allocator_.free(buf);
        return ret;
    }
    
    // 5. 检查实际读取大小
    if (handle.get_done_size() != file_size) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("read size mismatch", K(ret), K(handle.get_done_size()), K(file_size));
        allocator_.free(buf);
        return ret;
    }
    
    // 6. 转换为记录数组
    FileRecord *record_array = reinterpret_cast<FileRecord*>(buf);
    if (OB_FAIL(records.reserve(record_count))) {
        LOG_WARN("fail to reserve records", K(ret), K(record_count));
        allocator_.free(buf);
        return ret;
    }
    
    for (int64_t i = 0; OB_SUCC(ret) && i < record_count; ++i) {
        if (OB_FAIL(records.push_back(record_array[i]))) {
            LOG_WARN("fail to push record", K(ret), K(i));
        }
    }
    
    // 7. 释放缓冲区
    allocator_.free(buf);
    
    LOG_DEBUG("read bucket complete", K(bucket_id), K(record_count));
    return ret;
}
```

### 4.5 清理 TmpFile

```cpp
void ObFtsBM25SIndex::cleanup_tmp_files()
{
    if (!tmp_files_inited_) {
        return;
    }
    
    ObTenantTmpFileManager *tmp_mgr = MTL(ObTenantTmpFileManager*);
    if (OB_ISNULL(tmp_mgr)) {
        LOG_WARN("tmp file manager is null during cleanup");
        return;
    }
    
    // 删除所有桶文件
    for (int64_t i = 0; i < num_buckets_; ++i) {
        if (bucket_fds_[i] >= 0) {
            int ret = tmp_mgr->remove(bucket_fds_[i]);
            if (OB_FAIL(ret)) {
                LOG_WARN("fail to remove tmp file", K(ret), K(i), K(bucket_fds_[i]));
            }
            bucket_fds_[i] = -1;
            bucket_write_offsets_[i] = 0;
        }
    }
    
    // 释放动态分配的数组
    if (OB_NOT_NULL(bucket_fds_)) {
        allocator_.free(bucket_fds_);
        bucket_fds_ = nullptr;
    }
    if (OB_NOT_NULL(bucket_write_offsets_)) {
        allocator_.free(bucket_write_offsets_);
        bucket_write_offsets_ = nullptr;
    }
    if (OB_NOT_NULL(bucket_locks_)) {
        allocator_.free(bucket_locks_);
        bucket_locks_ = nullptr;
    }
    
    tmp_files_inited_ = false;
    tmp_dir_id_ = -1;
    num_buckets_ = 0;
    
    LOG_INFO("BM25S tmp files cleaned up");
}
```

---

## 5. 修改 collect_raw_data

### 5.1 原有逻辑（需要修改）

```cpp
// 原来使用 fwrite
int ObFtsBM25SIndex::collect_raw_data(const ObString &token, int64_t doc_id, 
                                       int32_t tf, int32_t doc_len)
{
    // ... token_id 分配逻辑 ...
    
    // 原来的写入
    FileRecord record;
    record.token_id = token_id;
    record.doc_id = doc_id;
    record.tf = tf;
    record.doc_len = doc_len;
    
    int64_t bucket_id = token_id % num_buckets_;
    
    // 旧代码：
    // ObSpinLockGuard guard(bucket_locks_[bucket_id]);
    // fwrite(&record, sizeof(FileRecord), 1, bucket_files_[bucket_id]);
    
    // ... 
}
```

### 5.2 新逻辑

```cpp
int ObFtsBM25SIndex::collect_raw_data(const ObString &token, int64_t doc_id, 
                                       int32_t tf, int32_t doc_len)
{
    int ret = OB_SUCCESS;
    
    // 1. 确保 TmpFile 已初始化
    if (!tmp_files_inited_) {
        ObSpinLockGuard guard(lock_);
        if (!tmp_files_inited_) {
            if (OB_FAIL(init_tmp_files())) {
                LOG_WARN("fail to init tmp files", K(ret));
                return ret;
            }
        }
    }
    
    // 2. 获取或分配 token_id（原有逻辑保持不变）
    uint32_t token_id = 0;
    {
        SpinRLockGuard guard(token_map_lock_);
        if (OB_SUCCESS == collect_token_to_id_.get_refactored(token, token_id)) {
            // 已存在
        } else {
            // 需要分配新 token_id
            SpinWLockGuard wguard(token_map_lock_);
            // double check
            if (OB_FAIL(collect_token_to_id_.get_refactored(token, token_id))) {
                token_id = static_cast<uint32_t>(collect_token_to_id_.size());
                // 复制 token 字符串
                char *token_copy = static_cast<char*>(allocator_.alloc(token.length()));
                if (OB_ISNULL(token_copy)) {
                    ret = OB_ALLOCATE_MEMORY_FAILED;
                    return ret;
                }
                MEMCPY(token_copy, token.ptr(), token.length());
                ObString stored_token(token.length(), token_copy);
                if (OB_FAIL(collect_token_to_id_.set_refactored(stored_token, token_id))) {
                    LOG_WARN("fail to set token", K(ret), K(token));
                    return ret;
                }
            }
        }
    }
    
    // 3. 记录唯一文档（原有逻辑）
    {
        SpinWLockGuard guard(doc_set_lock_);
        unique_docs_.set_refactored(doc_id);
    }
    ATOMIC_AAF(&total_doc_len_, doc_len);
    
    // 4. 写入 TmpFile（新逻辑）
    FileRecord record;
    record.token_id = token_id;
    record.doc_id = doc_id;
    record.tf = tf;
    record.doc_len = doc_len;
    
    int64_t bucket_id = token_id % num_buckets_;
    
    // 使用桶锁保护写入（TmpFile 的 write 可能不是线程安全的）
    {
        ObSpinLockGuard guard(bucket_locks_[bucket_id]);
        if (OB_FAIL(write_to_bucket(bucket_id, record))) {
            LOG_WARN("fail to write to bucket", K(ret), K(bucket_id));
            return ret;
        }
    }
    
    ATOMIC_AAF(&raw_entry_count_, 1);
    ATOMIC_AAF(&bucket_entry_counts_[bucket_id], 1);
    
    return ret;
}
```

---

## 6. 修改 finalize（与 Phase 2 配合）

Finalize 阶段与持久化合并，详见 `BM25S_PHASE2_PERSISTENCE.md`。

核心流程：
```
1. 逐桶读取 TmpFile
2. 排序（同一个 token 的记录变成连续的）
3. 第一遍遍历内存：统计桶内每个 token 的 df
4. 第二遍遍历内存：用 df 计算 BM25，直接写入持久化文件
5. 释放桶内存
6. 处理下一个桶
7. 最后清理所有 TmpFile
```

**关键点**：
- 同一个 token 的所有记录都在同一个桶中（因为按 `token_id % num_buckets_` 分桶）
- 两次遍历都是内存中的数据，不需要重新读取文件
- 不需要构建完整的内存 CSC 结构，直接写入持久化文件

---

## 7. 注意事项

### 7.1 线程安全
- `init_tmp_files()` 需要加锁保护，防止重复初始化
- `write_to_bucket()` 需要使用桶锁，因为同一个桶可能被多个线程写入
- `read_bucket()` 在 finalize 阶段调用，此时是单线程，不需要额外加锁

### 7.2 错误处理
- TmpFile 操作失败时，需要正确清理已分配的资源
- 读取失败时，释放已分配的缓冲区

### 7.3 性能优化
- 考虑批量写入，减少系统调用次数
- 大桶读取时，可以分批读取，避免一次分配过大内存

### 7.4 内存管理
- 使用 `allocator_` 分配的内存，需要在适当时候释放
- `read_bucket()` 返回的数据存储在 `records` 数组中，由调用者管理

### 7.5 桶数量与内存占用

| 数据规模 | 桶数量 | 单桶内存 |
|---------|-------|----------|
| 1 亿文档 | 1024 | ~100 MB |
| 10 亿文档 | 8192 | ~120 MB |
| 20 亿文档 | 16384 | ~120 MB |

内存峰值 = 单桶数据 + 元数据数组（~200 MB for 10M vocab）

---

## 8. 测试要点

1. **基本功能**：单线程写入、读取、清理
2. **并发写入**：多线程同时写入不同桶
3. **大数据量**：每个桶写入大量数据（>100MB）
4. **异常恢复**：写入过程中 TmpFile 满了的处理
5. **重复初始化**：多次调用 `init_tmp_files()` 不会出错

---

## 9. 相关文件

- `src/sql/das/ob_fts_bm25s_index.h` - 类定义修改
- `src/sql/das/ob_fts_bm25s_index.cpp` - 实现修改
- `src/storage/tmp_file/ob_tmp_file_manager.h` - TmpFile 接口参考
