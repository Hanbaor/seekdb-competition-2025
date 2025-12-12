# BM25S 索引构建 - Phase 2: 持久化落盘

## 1. 概述

### 1.1 目标
将 TmpFile 分桶数据的 Finalize 与持久化合并，逐桶处理并直接写入持久化文件，避免构建完整的内存 CSC 结构。

### 1.2 核心流程

```
两次遍历所有桶：

第一次遍历（统计 df）：
  for 每个桶:
    1. 读取 TmpFile 桶到内存
    2. 遍历记录，统计每个 token 的 df（不需要排序）
    3. 释放桶内存

计算偏移：
  - 根据所有 token 的 df 计算 posting_offsets
  - 写入 Header、Vocab Buckets

第二次遍历（计算 BM25 并写入）：
  for 每个桶:
    1. 读取 TmpFile 桶到内存
    2. 排序（按 token_id, doc_id）
    3. 遍历记录，用 df 计算 BM25，直接 pwrite 到文件
    4. 释放桶内存
```

**关键点**：
- 同一个 token 的所有记录都在同一个桶中（因为按 `token_id % num_buckets` 分桶）
- **第一次遍历不需要排序**，只统计 df
- 第二次遍历才排序，因为写入文件需要按 doc_id 顺序
- 不需要构建 `doc_ids_[]` 和 `scores_[]` 内存数组，直接写入文件

### 1.3 使用的 SeekDB 接口
- `ObIODeviceLocalFileOp` - 本地文件操作（open/write/read/close）
- 位于 `src/share/ob_io_device_helper.h`

### 1.4 持久化内容
| 数据 | 说明 |
|-----|------|
| Header | 元数据：total_docs, avg_doc_len, vocab_size 等 |
| Bucket Directory | 词汇表分桶目录，用于快速定位 |
| Vocab Buckets | 词汇表数据：token_str → token_id + posting 位置 |
| Posting Data | 倒排链数据：doc_ids[] + scores[] |

---

## 2. 文件格式设计

### 2.1 整体结构

```
+============================================+
|              BM25S Index File              |
+============================================+
| Section 1: Header (256 bytes, 固定)        |
+--------------------------------------------+
| Section 2: Bucket Directory (16 KB, 固定)  |
+--------------------------------------------+
| Section 3: Vocab Buckets (变长)            |
+--------------------------------------------+
| Section 4: Posting Data (变长)             |
+============================================+
```

### 2.2 Header 结构 (256 bytes)

```cpp
struct BM25SIndexFileHeader {
    // Magic & Version (16 bytes)
    uint64_t magic;              // 0x424D32355349445A ("BM25SIDZ")
    int32_t version;             // 版本号，当前为 1
    int32_t flags;               // 预留标志位
    
    // 统计信息 (32 bytes)
    int64_t total_docs;          // 总文档数
    double avg_doc_len;          // 平均文档长度
    int64_t vocab_size;          // 词汇表大小（token 数量）
    int64_t total_postings;      // posting 总条目数
    
    // Section 偏移和大小 (64 bytes)
    int64_t bucket_dir_offset;   // Bucket Directory 起始偏移
    int64_t bucket_dir_size;     // Bucket Directory 大小
    int64_t vocab_buckets_offset;// Vocab Buckets 起始偏移
    int64_t vocab_buckets_size;  // Vocab Buckets 总大小
    int64_t posting_data_offset; // Posting Data 起始偏移
    int64_t posting_data_size;   // Posting Data 总大小
    int64_t reserved1;
    int64_t reserved2;
    
    // 配置参数 (16 bytes)
    int32_t bucket_num;          // 桶数量（默认 1024）
    int32_t reserved3;
    int64_t reserved4;
    
    // 校验和 (16 bytes)
    uint64_t checksum;           // 文件校验和（可选）
    int64_t create_timestamp;    // 创建时间戳
    
    // 填充到 256 bytes
    char padding[256 - 144];
    
    // 常量
    static const uint64_t MAGIC = 0x424D32355349445A;
    static const int32_t CURRENT_VERSION = 1;
    static const int64_t HEADER_SIZE = 256;
};
```

### 2.3 Bucket Directory 结构 (16 KB)

```cpp
// 固定 1024 个桶（与 Phase 1 TmpFile 分桶数量一致）
static const int32_t NUM_VOCAB_BUCKETS = 1024;

struct BucketDirectoryEntry {
    int64_t offset;      // 该桶在文件中的偏移（相对于 vocab_buckets_offset）
    int32_t entry_count; // 该桶包含的 token 数量
    int32_t size;        // 该桶的数据大小（字节）
};
// sizeof(BucketDirectoryEntry) = 16 bytes
// 总大小 = 1024 * 16 = 16 KB
```

### 2.4 Vocab Bucket 结构

每个桶内按 token_hash 排序存储，支持二分查找：

```cpp
struct VocabEntry {
    uint64_t token_hash;      // token 的 hash 值
    int32_t token_len;        // token 字符串长度
    // char token_data[token_len];  // token 字符串（变长，紧随其后）
    uint32_t token_id;        // token ID
    int64_t posting_offset;   // posting 在 Posting Data 区域的偏移
    int32_t posting_count;    // posting 数量
    int32_t reserved;
};
// 固定部分 = 8 + 4 + 4 + 8 + 4 + 4 = 32 bytes
// 变长部分 = token_len bytes

// 桶内数据布局：
// [VocabEntry1_fixed][token1_data][VocabEntry2_fixed][token2_data]...
```

### 2.5 Posting Data 结构

每个 token 的 posting list 连续存储：

```cpp
// 对于 token_id = X:
// offset = vocab_entry[X].posting_offset
// count = vocab_entry[X].posting_count

// Posting Data 布局:
// [doc_ids: uint32_t * count][scores: float * count]
// 
// 示例：count = 100
// - doc_ids: 100 * 4 = 400 bytes
// - scores:  100 * 4 = 400 bytes
// - 总计: 800 bytes
```

---

## 3. 数据结构定义

### 3.1 在 ob_fts_bm25s_index.h 中添加

```cpp
// ========== 文件格式结构体 ==========

struct BM25SIndexFileHeader {
    uint64_t magic;
    int32_t version;
    int32_t flags;
    
    int64_t total_docs;
    double avg_doc_len;
    int64_t vocab_size;
    int64_t total_postings;
    
    int64_t bucket_dir_offset;
    int64_t bucket_dir_size;
    int64_t vocab_buckets_offset;
    int64_t vocab_buckets_size;
    int64_t posting_data_offset;
    int64_t posting_data_size;
    int64_t reserved1;
    int64_t reserved2;
    
    int32_t bucket_num;
    int32_t reserved3;
    int64_t reserved4;
    
    uint64_t checksum;
    int64_t create_timestamp;
    
    char padding[256 - 144];
    
    static const uint64_t MAGIC = 0x424D32355349445A;
    static const int32_t CURRENT_VERSION = 1;
    static const int64_t HEADER_SIZE = 256;
    static const int32_t DEFAULT_BUCKET_NUM = 1024;
    
    void reset() { memset(this, 0, sizeof(*this)); }
    bool is_valid() const { return magic == MAGIC && version == CURRENT_VERSION; }
};

struct BucketDirectoryEntry {
    int64_t offset;
    int32_t entry_count;
    int32_t size;
    
    void reset() { offset = 0; entry_count = 0; size = 0; }
};

// VocabEntry 用于内存中的结构
struct VocabEntryMem {
    uint64_t token_hash;
    ObString token;
    uint32_t token_id;
    int64_t posting_offset;
    int32_t posting_count;
};
```

### 3.2 新增成员变量

```cpp
class ObFtsBM25SIndex {
private:
    // ========== 持久化相关 ==========
    char index_file_path_[OB_MAX_FILE_NAME_LENGTH];  // 索引文件路径
    common::ObIOFd index_fd_;                         // 索引文件描述符
    bool index_file_opened_;                          // 文件是否已打开
    
    BM25SIndexFileHeader file_header_;                // 文件头（启动时加载）
    BucketDirectoryEntry *bucket_directory_;          // 桶目录（启动时加载）
    
    uint64_t tenant_id_;                              // 租户 ID
    uint64_t table_id_;                               // 表 ID
};
```

---

## 4. 持久化写入实现

### 4.1 需要引入的头文件

```cpp
#include "share/ob_io_device_helper.h"
#include <fcntl.h>
```

### 4.2 主入口函数

```cpp
int ObFtsBM25SIndex::persist_to_disk(const char *index_dir)
{
    int ret = OB_SUCCESS;
    
    if (OB_ISNULL(index_dir)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("index dir is null", K(ret));
        return ret;
    }
    
    if (!is_finalized_) {
        ret = OB_NOT_INIT;
        LOG_WARN("index not finalized", K(ret));
        return ret;
    }
    
    // 1. 创建目录
    if (OB_FAIL(ObIODeviceLocalFileOp::mkdir(index_dir, 0755))) {
        if (OB_FILE_OR_DIRECTORY_EXIST == ret) {
            ret = OB_SUCCESS;  // 目录已存在，继续
        } else {
            LOG_WARN("fail to mkdir", K(ret), K(index_dir));
            return ret;
        }
    }
    
    // 2. 构建文件路径
    char file_path[OB_MAX_FILE_NAME_LENGTH];
    int n = snprintf(file_path, sizeof(file_path), "%s/bm25s_index.dat", index_dir);
    if (n < 0 || n >= sizeof(file_path)) {
        ret = OB_SIZE_OVERFLOW;
        LOG_WARN("file path too long", K(ret), K(index_dir));
        return ret;
    }
    
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
        // 4.1 写入 Header 和 Bucket Directory（占位）
        if (OB_FAIL(write_header_and_directory(fd))) {
            LOG_WARN("fail to write header", K(ret));
            break;
        }
        
        // 4.2 预计算 posting offsets（在写入 vocab 之前）
        ObArray<int64_t> posting_offsets;
        if (OB_FAIL(calc_posting_offsets(posting_offsets))) {
            LOG_WARN("fail to calc posting offsets", K(ret));
            break;
        }
        
        // 4.3 写入 Vocab Buckets（posting_offset 已知）
        if (OB_FAIL(write_vocab_buckets(fd, posting_offsets))) {
            LOG_WARN("fail to write vocab buckets", K(ret));
            break;
        }
        
        // 4.4 写入 Posting Data
        if (OB_FAIL(write_posting_data(fd))) {
            LOG_WARN("fail to write posting data", K(ret));
            break;
        }
        
        // 4.5 更新 Header（更新各区域的实际大小）
        if (OB_FAIL(update_header(fd))) {
            LOG_WARN("fail to update header", K(ret));
            break;
        }
    } while (0);
    
    // 5. 同步并关闭
    if (fd.is_normal_file()) {
        ObIODeviceLocalFileOp::fsync(fd);
        ObIODeviceLocalFileOp::close(fd);
    }
    
    if (OB_SUCC(ret)) {
        MEMCPY(index_file_path_, file_path, strlen(file_path) + 1);
        LOG_INFO("BM25S index persisted", K(file_path), K_(total_docs), 
                 K(token_to_id_.size()), K(scores_.count()));
    }
    
    return ret;
}
```

### 4.3 写入 Header 和 Bucket Directory

```cpp
int ObFtsBM25SIndex::write_header_and_directory(const common::ObIOFd &fd)
{
    int ret = OB_SUCCESS;
    int64_t write_size = 0;
    
    // 1. 准备 Header（先写入占位，后面更新）
    BM25SIndexFileHeader header;
    header.reset();
    header.magic = BM25SIndexFileHeader::MAGIC;
    header.version = BM25SIndexFileHeader::CURRENT_VERSION;
    header.total_docs = total_docs_;
    header.avg_doc_len = avg_doc_len_;
    header.vocab_size = token_to_id_.size();
    header.total_postings = scores_.count();
    header.bucket_num = BM25SIndexFileHeader::DEFAULT_BUCKET_NUM;
    header.create_timestamp = ObTimeUtility::current_time();
    
    // 计算各 Section 偏移
    header.bucket_dir_offset = BM25SIndexFileHeader::HEADER_SIZE;
    header.bucket_dir_size = header.bucket_num * sizeof(BucketDirectoryEntry);
    header.vocab_buckets_offset = header.bucket_dir_offset + header.bucket_dir_size;
    // vocab_buckets_size 和 posting_data_offset 后面计算
    
    // 保存 header 供后续更新
    file_header_ = header;
    
    // 2. 写入 Header
    if (OB_FAIL(ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, &header, 
                                                    sizeof(header), 0, write_size))) {
        LOG_WARN("fail to write header", K(ret));
        return ret;
    }
    
    // 3. 准备 Bucket Directory（先写入空的，后面更新）
    int64_t dir_size = header.bucket_num * sizeof(BucketDirectoryEntry);
    char *dir_buf = static_cast<char*>(allocator_.alloc(dir_size));
    if (OB_ISNULL(dir_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        return ret;
    }
    memset(dir_buf, 0, dir_size);
    
    if (OB_FAIL(ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, dir_buf, dir_size,
                                                    header.bucket_dir_offset, write_size))) {
        LOG_WARN("fail to write bucket directory", K(ret));
        allocator_.free(dir_buf);
        return ret;
    }
    
    allocator_.free(dir_buf);
    return ret;
}
```

### 4.4 预计算 Posting Offsets

```cpp
// 在写入 vocab buckets 之前，先计算每个 token 的 posting_offset
int ObFtsBM25SIndex::calc_posting_offsets(ObArray<int64_t> &posting_offsets)
{
    int ret = OB_SUCCESS;
    
    int64_t vocab_size = token_ptr_.count() - 1;
    if (OB_FAIL(posting_offsets.prepare_allocate(vocab_size))) {
        return ret;
    }
    
    int64_t current_offset = 0;
    for (int64_t token_id = 0; token_id < vocab_size; ++token_id) {
        posting_offsets.at(token_id) = current_offset;
        
        uint64_t start = token_ptr_.at(token_id);
        uint64_t end = token_ptr_.at(token_id + 1);
        int64_t count = end - start;
        
        // 每个 posting entry = doc_id(4) + score(4) = 8 bytes
        current_offset += count * (sizeof(uint32_t) + sizeof(float));
    }
    
    file_header_.posting_data_size = current_offset;
    return ret;
}
```

### 4.5 写入 Vocab Buckets

```cpp
int ObFtsBM25SIndex::write_vocab_buckets(const common::ObIOFd &fd,
                                          const ObArray<int64_t> &posting_offsets)
{
    int ret = OB_SUCCESS;
    
    const int32_t bucket_num = file_header_.bucket_num;
    
    // 1. 分配 Bucket Directory 数组
    BucketDirectoryEntry *bucket_dir = static_cast<BucketDirectoryEntry*>(
        allocator_.alloc(bucket_num * sizeof(BucketDirectoryEntry)));
    if (OB_ISNULL(bucket_dir)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        return ret;
    }
    for (int32_t i = 0; i < bucket_num; ++i) {
        bucket_dir[i].reset();
    }
    
    // 2. 将所有 token 按桶分组
    ObArray<ObArray<VocabEntryMem>> bucket_entries;
    if (OB_FAIL(bucket_entries.prepare_allocate(bucket_num))) {
        allocator_.free(bucket_dir);
        return ret;
    }
    
    // 遍历 token_to_id_，分配到各个桶
    for (TokenIdMap::const_iterator it = token_to_id_.begin();
         OB_SUCC(ret) && it != token_to_id_.end(); ++it) {
        const ObString &token = it->first;
        uint32_t token_id = it->second;
        
        VocabEntryMem entry;
        entry.token_hash = token.hash();
        entry.token = token;
        entry.token_id = token_id;
        
        // 从 token_ptr_ 获取 posting 信息
        if (token_id < token_ptr_.count() - 1) {
            uint64_t start = token_ptr_.at(token_id);
            uint64_t end = token_ptr_.at(token_id + 1);
            entry.posting_count = static_cast<int32_t>(end - start);
            // posting_offset 已预计算！
            entry.posting_offset = posting_offsets.at(token_id);
        } else {
            entry.posting_count = 0;
            entry.posting_offset = 0;
        }
        
        int32_t bucket_id = entry.token_hash % bucket_num;
        if (OB_FAIL(bucket_entries.at(bucket_id).push_back(entry))) {
            LOG_WARN("fail to push entry", K(ret), K(bucket_id));
            break;
        }
    }
    
    if (OB_FAIL(ret)) {
        allocator_.free(bucket_dir);
        return ret;
    }
    
    // 3. 对每个桶内的 entry 按 hash 排序
    for (int32_t i = 0; i < bucket_num; ++i) {
        ObArray<VocabEntryMem> &entries = bucket_entries.at(i);
        if (entries.count() > 1) {
            std::sort(&entries.at(0), &entries.at(0) + entries.count(),
                [](const VocabEntryMem &a, const VocabEntryMem &b) {
                    return a.token_hash < b.token_hash;
                });
        }
    }
    
    // 4. 计算每个桶的大小和偏移
    int64_t current_offset = 0;
    for (int32_t i = 0; i < bucket_num; ++i) {
        ObArray<VocabEntryMem> &entries = bucket_entries.at(i);
        bucket_dir[i].offset = current_offset;
        bucket_dir[i].entry_count = entries.count();
        
        // 计算桶大小
        int64_t bucket_size = 0;
        for (int64_t j = 0; j < entries.count(); ++j) {
            // 固定部分 32 字节 + token 字符串
            bucket_size += 32 + entries.at(j).token.length();
        }
        bucket_dir[i].size = static_cast<int32_t>(bucket_size);
        current_offset += bucket_size;
    }
    
    file_header_.vocab_buckets_size = current_offset;
    file_header_.posting_data_offset = file_header_.vocab_buckets_offset + current_offset;
    
    // 5. 写入每个桶的数据
    int64_t write_offset = file_header_.vocab_buckets_offset;
    for (int32_t i = 0; OB_SUCC(ret) && i < bucket_num; ++i) {
        ObArray<VocabEntryMem> &entries = bucket_entries.at(i);
        if (entries.empty()) {
            continue;
        }
        
        // 分配桶缓冲区
        int64_t bucket_size = bucket_dir[i].size;
        char *buf = static_cast<char*>(allocator_.alloc(bucket_size));
        if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            break;
        }
        
        // 填充桶数据
        char *p = buf;
        for (int64_t j = 0; j < entries.count(); ++j) {
            VocabEntryMem &entry = entries.at(j);
            
            // token_hash (8 bytes)
            memcpy(p, &entry.token_hash, 8); p += 8;
            // token_len (4 bytes)
            int32_t token_len = entry.token.length();
            memcpy(p, &token_len, 4); p += 4;
            // token_data (变长)
            memcpy(p, entry.token.ptr(), token_len); p += token_len;
            // token_id (4 bytes)
            memcpy(p, &entry.token_id, 4); p += 4;
            // posting_offset (8 bytes) - 后面计算
            int64_t posting_off = 0;  // 占位
            memcpy(p, &posting_off, 8); p += 8;
            // posting_count (4 bytes)
            memcpy(p, &entry.posting_count, 4); p += 4;
            // reserved (4 bytes)
            int32_t reserved = 0;
            memcpy(p, &reserved, 4); p += 4;
        }
        
        // 写入桶
        int64_t write_size = 0;
        if (OB_FAIL(ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, buf, bucket_size,
                                                        write_offset, write_size))) {
            LOG_WARN("fail to write bucket", K(ret), K(i));
            allocator_.free(buf);
            break;
        }
        
        write_offset += bucket_size;
        allocator_.free(buf);
    }
    
    // 6. 保存 bucket_dir 供后续更新
    bucket_directory_ = bucket_dir;
    
    return ret;
}
```

### 4.6 写入 Posting Data

```cpp
int ObFtsBM25SIndex::write_posting_data(const common::ObIOFd &fd)
{
    int ret = OB_SUCCESS;
    
    if (scores_.empty()) {
        return ret;
    }
    
    int64_t posting_offset = file_header_.posting_data_offset;
    int64_t total_written = 0;
    
    // 按 token_id 顺序写入每个 token 的 posting list
    int64_t vocab_size = token_ptr_.count() - 1;
    
    for (int64_t token_id = 0; OB_SUCC(ret) && token_id < vocab_size; ++token_id) {
        uint64_t start = token_ptr_.at(token_id);
        uint64_t end = token_ptr_.at(token_id + 1);
        int64_t count = end - start;
        
        if (count == 0) {
            continue;
        }
        
        // 计算数据大小
        int64_t doc_ids_size = count * sizeof(uint32_t);
        int64_t scores_size = count * sizeof(float);
        int64_t total_size = doc_ids_size + scores_size;
        
        // 分配缓冲区
        char *buf = static_cast<char*>(allocator_.alloc(total_size));
        if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            break;
        }
        
        // 复制 doc_ids
        memcpy(buf, &doc_ids_.at(start), doc_ids_size);
        // 复制 scores
        memcpy(buf + doc_ids_size, &scores_.at(start), scores_size);
        
        // 写入
        int64_t write_size = 0;
        if (OB_FAIL(ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, buf, total_size,
                                                        posting_offset + total_written, 
                                                        write_size))) {
            LOG_WARN("fail to write posting", K(ret), K(token_id));
            allocator_.free(buf);
            break;
        }
        
        total_written += total_size;
        allocator_.free(buf);
    }
    
    // posting_data_size 已在 calc_posting_offsets() 中计算
    return ret;
}
```

### 4.7 更新 Header 和 Bucket Directory

```cpp
int ObFtsBM25SIndex::update_header(const common::ObIOFd &fd)
{
    int ret = OB_SUCCESS;
    int64_t write_size = 0;
    
    // 1. 更新并写入 Header
    if (OB_FAIL(ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, &file_header_,
                                                    sizeof(file_header_), 0, write_size))) {
        LOG_WARN("fail to update header", K(ret));
        return ret;
    }
    
    // 2. 更新并写入 Bucket Directory
    if (OB_NOT_NULL(bucket_directory_)) {
        int64_t dir_size = file_header_.bucket_num * sizeof(BucketDirectoryEntry);
        if (OB_FAIL(ObIODeviceLocalFileOp::pwrite_impl(fd.second_id_, bucket_directory_,
                                                        dir_size, file_header_.bucket_dir_offset,
                                                        write_size))) {
            LOG_WARN("fail to update bucket directory", K(ret));
            return ret;
        }
    }
    
    return ret;
}

// 注意：不再需要 update_vocab_posting_offsets() 函数
// 因为我们在 write_vocab_buckets() 之前就预计算好了 posting_offset
```

---

## 5. 文件路径设计

### 5.1 路径规范

```
${data_dir}/bm25s_index/${tenant_id}/${table_id}/bm25s_index.dat

示例:
/data/obcluster/store/bm25s_index/1001/500001/bm25s_index.dat
```

### 5.2 获取索引路径

```cpp
int ObFtsBM25SIndex::get_index_path(uint64_t tenant_id, uint64_t table_id,
                                     char *path_buf, int64_t buf_len)
{
    int ret = OB_SUCCESS;
    
    // 获取数据目录
    const char *data_dir = GCONF.data_dir.str();
    
    int n = snprintf(path_buf, buf_len, "%s/bm25s_index/%lu/%lu",
                     data_dir, tenant_id, table_id);
    if (n < 0 || n >= buf_len) {
        ret = OB_SIZE_OVERFLOW;
        LOG_WARN("path too long", K(ret), K(tenant_id), K(table_id));
    }
    
    return ret;
}
```

---

## 6. 加载实现

### 6.1 加载索引

```cpp
int ObFtsBM25SIndex::load_from_disk(const char *index_dir)
{
    int ret = OB_SUCCESS;
    
    // 1. 构建文件路径
    char file_path[OB_MAX_FILE_NAME_LENGTH];
    snprintf(file_path, sizeof(file_path), "%s/bm25s_index.dat", index_dir);
    
    // 2. 检查文件是否存在
    bool is_exist = false;
    if (OB_FAIL(ObIODeviceLocalFileOp::exist(file_path, is_exist))) {
        LOG_WARN("fail to check file exist", K(ret), K(file_path));
        return ret;
    }
    if (!is_exist) {
        ret = OB_FILE_NOT_EXIST;
        LOG_INFO("index file not exist", K(ret), K(file_path));
        return ret;
    }
    
    // 3. 打开文件
    if (OB_FAIL(ObIODeviceLocalFileOp::open(file_path, O_RDONLY, 0, index_fd_))) {
        LOG_WARN("fail to open file", K(ret), K(file_path));
        return ret;
    }
    
    // 4. 读取 Header
    int64_t read_size = 0;
    if (OB_FAIL(ObIODeviceLocalFileOp::pread_impl(index_fd_.second_id_, &file_header_,
                                                   sizeof(file_header_), 0, read_size))) {
        LOG_WARN("fail to read header", K(ret));
        close_index_file();
        return ret;
    }
    
    // 5. 验证 Header
    if (!file_header_.is_valid()) {
        ret = OB_INVALID_DATA;
        LOG_WARN("invalid header", K(ret), K(file_header_.magic), K(file_header_.version));
        close_index_file();
        return ret;
    }
    
    // 6. 加载 Bucket Directory
    int64_t dir_size = file_header_.bucket_num * sizeof(BucketDirectoryEntry);
    bucket_directory_ = static_cast<BucketDirectoryEntry*>(allocator_.alloc(dir_size));
    if (OB_ISNULL(bucket_directory_)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        close_index_file();
        return ret;
    }
    
    if (OB_FAIL(ObIODeviceLocalFileOp::pread_impl(index_fd_.second_id_, bucket_directory_,
                                                   dir_size, file_header_.bucket_dir_offset,
                                                   read_size))) {
        LOG_WARN("fail to read bucket directory", K(ret));
        close_index_file();
        return ret;
    }
    
    // 7. 恢复统计信息
    total_docs_ = file_header_.total_docs;
    avg_doc_len_ = file_header_.avg_doc_len;
    
    // 8. 保存文件路径
    MEMCPY(index_file_path_, file_path, strlen(file_path) + 1);
    index_file_opened_ = true;
    is_inited_ = true;
    is_finalized_ = true;  // 从文件加载视为已 finalize
    
    LOG_INFO("BM25S index loaded from disk", K(file_path),
             K(file_header_.total_docs), K(file_header_.vocab_size),
             K(file_header_.total_postings));
    
    return ret;
}

void ObFtsBM25SIndex::close_index_file()
{
    if (index_file_opened_ && index_fd_.is_normal_file()) {
        ObIODeviceLocalFileOp::close(index_fd_);
        index_file_opened_ = false;
    }
    if (OB_NOT_NULL(bucket_directory_)) {
        allocator_.free(bucket_directory_);
        bucket_directory_ = nullptr;
    }
}
```

---

## 7. 注意事项

### 7.1 字节序
- 所有整数使用小端序（x86 默认）
- 如果需要跨平台，需要添加字节序转换

### 7.2 对齐
- VocabEntry 的变长部分（token_data）不保证对齐
- 读取时需要按字节复制，不要直接转换指针

### 7.3 原子性
- 写入完成后才更新 Header
- 使用临时文件 + rename 可以保证原子性（可选优化）

### 7.4 文件大小估算

| 数据 | 计算公式 | 100万词汇/1亿posting |
|-----|---------|---------------------|
| Header | 256 B | 256 B |
| Bucket Directory | 1024 × 16 B | 16 KB |
| Vocab Buckets | vocab_size × (32 + avg_token_len) | ~50 MB |
| Posting Data | total_postings × 8 B | ~800 MB |
| **总计** | | **~850 MB** |

---

## 8. 流式 Finalize + 持久化实现

这是核心实现，将 TmpFile 分桶数据直接写入持久化文件。

### 8.1 主入口函数

```cpp
int ObFtsBM25SIndex::finalize_and_persist(const char *index_path)
{
    int ret = OB_SUCCESS;
    
    // 0. 更新统计信息（从收集阶段获取）
    total_docs_ = unique_docs_.size();
    avg_doc_len_ = (total_docs_ > 0) ? 
                   static_cast<double>(total_doc_len_) / total_docs_ : 0.0;
    
    // 1. 创建持久化文件
    common::ObIOFd fd;
    if (OB_FAIL(ObIODeviceLocalFileOp::open(index_path, 
                                            O_CREAT | O_WRONLY | O_TRUNC, 
                                            0644, fd))) {
        LOG_WARN("fail to open file", K(ret), K(index_path));
        return ret;
    }
    
    // 2. 初始化元数据数组
    int64_t vocab_size = collect_token_to_id_.size();
    ObArray<int64_t> token_df;           // 每个 token 的文档频率
    ObArray<int64_t> posting_offsets;    // 每个 token 的 posting 偏移
    ObArray<int64_t> token_written_count; // 每个 token 已写入的记录数
    
    if (OB_FAIL(token_df.prepare_allocate(vocab_size)) ||
        OB_FAIL(posting_offsets.prepare_allocate(vocab_size)) ||
        OB_FAIL(token_written_count.prepare_allocate(vocab_size))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        ObIODeviceLocalFileOp::close(fd);
        return ret;
    }
    for (int64_t i = 0; i < vocab_size; ++i) {
        token_df.at(i) = 0;
        posting_offsets.at(i) = 0;
        token_written_count.at(i) = 0;
    }
    
    // 3. 第一次遍历所有桶：统计 df（不需要排序）
    for (int64_t bucket_id = 0; OB_SUCC(ret) && bucket_id < NUM_BUCKETS; ++bucket_id) {
        ObArray<FileRecord> records;
        if (OB_FAIL(read_bucket(bucket_id, records))) {
            break;
        }
        if (records.empty()) {
            continue;
        }
        
        // 注意：这里不需要排序！只统计 df
        // 统计每个 token 的 df
        for (int64_t i = 0; i < records.count(); ++i) {
            token_df.at(records.at(i).token_id)++;
        }
        
        records.reset();
    }
    
    // 4. 计算 posting_offsets 和 posting_data_size
    int64_t current_offset = 0;
    for (int64_t token_id = 0; token_id < vocab_size; ++token_id) {
        posting_offsets.at(token_id) = current_offset;
        // 每个 posting = doc_id(4) + score(4) = 8 bytes
        current_offset += token_df.at(token_id) * 8;
    }
    int64_t posting_data_size = current_offset;
    
    // 5. 计算 vocab_buckets_size（遍历 token_to_id）
    int64_t vocab_buckets_size = 0;
    for (TokenIdMap::const_iterator it = collect_token_to_id_.begin();
         it != collect_token_to_id_.end(); ++it) {
        // 每个 entry = 32 bytes 固定部分 + token_len
        vocab_buckets_size += 32 + it->first.length();
    }
    
    // 6. 计算文件各区域偏移
    file_header_.reset();
    file_header_.magic = BM25SIndexFileHeader::MAGIC;
    file_header_.version = BM25SIndexFileHeader::CURRENT_VERSION;
    file_header_.total_docs = total_docs_;
    file_header_.avg_doc_len = avg_doc_len_;
    file_header_.vocab_size = vocab_size;
    file_header_.total_postings = posting_data_size / 8;
    file_header_.bucket_num = BM25SIndexFileHeader::DEFAULT_BUCKET_NUM;
    file_header_.create_timestamp = ObTimeUtility::current_time();
    
    file_header_.bucket_dir_offset = BM25SIndexFileHeader::HEADER_SIZE;
    file_header_.bucket_dir_size = file_header_.bucket_num * sizeof(BucketDirectoryEntry);
    file_header_.vocab_buckets_offset = file_header_.bucket_dir_offset + file_header_.bucket_dir_size;
    file_header_.vocab_buckets_size = vocab_buckets_size;
    file_header_.posting_data_offset = file_header_.vocab_buckets_offset + vocab_buckets_size;
    file_header_.posting_data_size = posting_data_size;
    
    // 7. 写入 Header 和 Bucket Directory（占位）
    if (OB_FAIL(write_header_and_directory(fd))) {
        LOG_WARN("fail to write header", K(ret));
        ObIODeviceLocalFileOp::close(fd);
        return ret;
    }
    
    // 8. 写入 Vocab Buckets（先写 Vocab，再写 Posting）
    if (OB_FAIL(write_vocab_buckets_streaming(fd, posting_offsets, token_df))) {
        LOG_WARN("fail to write vocab buckets", K(ret));
        ObIODeviceLocalFileOp::close(fd);
        return ret;
    }
    
    // 9. 第二次遍历所有桶：计算 BM25 并写入 Posting Data
    for (int64_t bucket_id = 0; OB_SUCC(ret) && bucket_id < NUM_BUCKETS; ++bucket_id) {
        ObArray<FileRecord> records;
        if (OB_FAIL(read_bucket(bucket_id, records))) {
            break;
        }
        if (records.empty()) {
            continue;
        }
        
        // 排序
        std::sort(&records.at(0), &records.at(0) + records.count());
        
        // 遍历记录，计算 BM25 并写入文件
        for (int64_t i = 0; OB_SUCC(ret) && i < records.count(); ++i) {
            const FileRecord &rec = records.at(i);
            uint32_t token_id = rec.token_id;
            
            // 获取这个 token 的信息
            int64_t df = token_df.at(token_id);
            int64_t token_base = file_header_.posting_data_offset + 
                                 posting_offsets.at(token_id);
            int64_t idx = token_written_count.at(token_id);
            
            // 计算 BM25 分数
            float score = calc_bm25_score(rec.tf, rec.doc_len, df);
            uint32_t doc_id = static_cast<uint32_t>(rec.doc_id);
            
            // 计算写入位置
            // doc_ids 区域：[token_base, token_base + df * 4)
            // scores 区域：[token_base + df * 4, token_base + df * 8)
            int64_t doc_id_offset = token_base + idx * sizeof(uint32_t);
            int64_t score_offset = token_base + df * sizeof(uint32_t) + idx * sizeof(float);
            
            // 写入 doc_id
            int64_t write_size = 0;
            if (OB_FAIL(ObIODeviceLocalFileOp::pwrite_impl(
                    fd.second_id_, &doc_id, sizeof(doc_id), 
                    doc_id_offset, write_size))) {
                LOG_WARN("fail to write doc_id", K(ret), K(token_id), K(idx));
                break;
            }
            
            // 写入 score
            if (OB_FAIL(ObIODeviceLocalFileOp::pwrite_impl(
                    fd.second_id_, &score, sizeof(score),
                    score_offset, write_size))) {
                LOG_WARN("fail to write score", K(ret), K(token_id), K(idx));
                break;
            }
            
            token_written_count.at(token_id)++;
        }
        
        records.reset();
    }
    
    // 10. 更新 Header
    if (OB_SUCC(ret)) {
        ret = update_header(fd);
    }
    
    // 11. 同步并关闭文件
    ObIODeviceLocalFileOp::fsync(fd);
    ObIODeviceLocalFileOp::close(fd);
    
    // 12. 清理 TmpFile
    cleanup_tmp_files();
    
    is_finalized_ = true;
    LOG_INFO("BM25S index finalized and persisted", K(index_path), 
             K(total_docs_), K(vocab_size), K(posting_data_size));
    return ret;
}
```

### 8.2 写入 Vocab Buckets

```cpp
int ObFtsBM25SIndex::write_vocab_buckets_streaming(
    const common::ObIOFd &fd,
    const ObArray<int64_t> &posting_offsets,
    const ObArray<int64_t> &token_df)
{
    int ret = OB_SUCCESS;
    const int32_t bucket_num = file_header_.bucket_num;
    
    // 1. 分配 Bucket Directory
    bucket_directory_ = static_cast<BucketDirectoryEntry*>(
        allocator_.alloc(bucket_num * sizeof(BucketDirectoryEntry)));
    if (OB_ISNULL(bucket_directory_)) {
        return OB_ALLOCATE_MEMORY_FAILED;
    }
    for (int32_t i = 0; i < bucket_num; ++i) {
        bucket_directory_[i].reset();
    }
    
    // 2. 将 token 按 hash 分桶并排序
    ObArray<ObArray<VocabEntryMem>> bucket_entries;
    if (OB_FAIL(bucket_entries.prepare_allocate(bucket_num))) {
        return ret;
    }
    
    for (TokenIdMap::const_iterator it = collect_token_to_id_.begin();
         OB_SUCC(ret) && it != collect_token_to_id_.end(); ++it) {
        VocabEntryMem entry;
        entry.token_hash = it->first.hash();
        entry.token = it->first;
        entry.token_id = it->second;
        entry.posting_offset = posting_offsets.at(it->second);
        entry.posting_count = static_cast<int32_t>(token_df.at(it->second));
        
        int32_t bucket_id = entry.token_hash % bucket_num;
        if (OB_FAIL(bucket_entries.at(bucket_id).push_back(entry))) {
            break;
        }
    }
    
    // 3. 对每个桶按 hash 排序并写入
    int64_t current_offset = 0;
    int64_t write_offset = file_header_.vocab_buckets_offset;
    
    for (int32_t i = 0; OB_SUCC(ret) && i < bucket_num; ++i) {
        ObArray<VocabEntryMem> &entries = bucket_entries.at(i);
        
        bucket_directory_[i].offset = current_offset;
        bucket_directory_[i].entry_count = entries.count();
        
        if (entries.empty()) {
            bucket_directory_[i].size = 0;
            continue;
        }
        
        // 排序
        std::sort(&entries.at(0), &entries.at(0) + entries.count(),
            [](const VocabEntryMem &a, const VocabEntryMem &b) {
                return a.token_hash < b.token_hash;
            });
        
        // 计算桶大小并写入
        int64_t bucket_size = 0;
        for (int64_t j = 0; j < entries.count(); ++j) {
            bucket_size += 32 + entries.at(j).token.length();
        }
        bucket_directory_[i].size = static_cast<int32_t>(bucket_size);
        
        // 分配缓冲区并填充
        char *buf = static_cast<char*>(allocator_.alloc(bucket_size));
        if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            break;
        }
        
        char *p = buf;
        for (int64_t j = 0; j < entries.count(); ++j) {
            VocabEntryMem &e = entries.at(j);
            memcpy(p, &e.token_hash, 8); p += 8;
            int32_t token_len = e.token.length();
            memcpy(p, &token_len, 4); p += 4;
            memcpy(p, e.token.ptr(), token_len); p += token_len;
            memcpy(p, &e.token_id, 4); p += 4;
            memcpy(p, &e.posting_offset, 8); p += 8;
            memcpy(p, &e.posting_count, 4); p += 4;
            int32_t reserved = 0;
            memcpy(p, &reserved, 4); p += 4;
        }
        
        // 写入文件
        int64_t write_size = 0;
        if (OB_FAIL(ObIODeviceLocalFileOp::pwrite_impl(
                fd.second_id_, buf, bucket_size, write_offset, write_size))) {
            allocator_.free(buf);
            break;
        }
        
        write_offset += bucket_size;
        current_offset += bucket_size;
        allocator_.free(buf);
    }
    
    return ret;
}
```

### 8.3 BM25 计算

```cpp
float ObFtsBM25SIndex::calc_bm25_score(int32_t tf, int32_t doc_len, int64_t df)
{
    // BM25 参数
    const float k1 = 1.2f;
    const float b = 0.75f;
    
    // IDF = log((N - df + 0.5) / (df + 0.5) + 1)
    float idf = logf((total_docs_ - df + 0.5f) / (df + 0.5f) + 1.0f);
    
    // TF 部分 = tf * (k1 + 1) / (tf + k1 * (1 - b + b * doc_len / avg_doc_len))
    float tf_part = static_cast<float>(tf) * (k1 + 1.0f) / 
                    (tf + k1 * (1.0f - b + b * doc_len / avg_doc_len_));
    
    return idf * tf_part;
}
```

### 8.4 内存占用分析

| 数据 | 大小 | 生命周期 |
|-----|------|----------|
| `token_df_[vocab_size]` | ~40 MB (10M vocab) | 常驻 |
| `posting_offsets_[vocab_size]` | ~80 MB | 常驻 |
| `token_written_count_[vocab_size]` | ~80 MB | 常驻 |
| 单桶数据 | ~200 MB | 临时 |
| **总计峰值** | **~400 MB** | |

比起原来全内存 CSC 结构（~800 MB for 1亿 posting），内存占用减少了一半。

---

## 9. 测试要点

1. **基本功能**：写入后能正确读取
2. **完整性**：Header、Bucket Directory、Vocab、Posting 都正确
3. **大文件**：测试 GB 级别的索引文件
4. **重启恢复**：模拟数据库重启后加载
5. **错误处理**：磁盘满、文件损坏等情况
6. **内存占用**：确认峰值内存不超过预期

---

## 10. 与当前代码的对比

### 10.1 当前代码流程

```
当前 finalize():
  1. 第一次遍历所有桶：统计 df（不排序）
  2. 预分配 scores_[] 和 doc_ids_[] 内存数组（全量！）
  3. 第二次遍历所有桶：排序 + 填充内存数组
  4. 查询时直接访问内存数组
```

### 10.2 新设计流程

```
新设计 finalize_and_persist():
  1. 第一次遍历所有桶：统计 df（不排序）
  2. 计算 posting_offsets，写入 Header + Vocab
  3. 第二次遍历所有桶：排序 + 直接 pwrite 到文件
  4. 查询时从 KVCache 获取（按需加载）
```

### 10.3 关键差异

| 方面 | 当前代码 | 新设计 |
|-----|---------|-------|
| **遍历次数** | 两次 | 两次 |
| **第一遍排序** | ❌ 不排序 | ❌ 不排序 |
| **输出目标** | 内存数组 | 持久化文件 |
| **内存占用** | ~800 MB（全量 CSC） | ~400 MB（单桶 + 元数据） |
| **查询数据源** | 内存数组 | KVCache（按需加载） |

---

## 11. 扩展计划：外部排序（处理超大桶）

### 11.1 问题场景

基础方案使用固定 1024 桶，配合停用词过滤可覆盖大多数场景。但在以下情况可能需要外部排序：

- 某个非停用词出现在极多文档中
- 数据量远超预期（如 20 亿文档）
- 单桶数据 > 可用内存

### 11.2 外部排序方案

```
桶数据（可能 4 GB）
        │
        ▼
┌───────────────────────────────────────┐
│ Phase 1: 分批排序                      │
│   读取 200 MB → 内存排序 → 写入临时文件  │
│   重复 N 次，产生 N 个有序临时文件       │
└───────────────────────────────────────┘
        │
        ▼
┌───────────────────────────────────────┐
│ Phase 2: K 路归并                      │
│   打开 N 个临时文件                    │
│   用最小堆归并                         │
│   边归并边计算 BM25 边写入              │
└───────────────────────────────────────┘
```

### 11.3 实现要点

```cpp
int finalize_bucket_with_external_sort(int64_t bucket_id) {
    int64_t bucket_size = get_bucket_size(bucket_id);
    const int64_t MAX_MEMORY = 200 * 1024 * 1024;  // 200 MB
    
    if (bucket_size <= MAX_MEMORY) {
        // 小桶：直接内存排序
        return finalize_bucket_in_memory(bucket_id);
    }
    
    // 大桶：外部排序
    ObArray<int64_t> sorted_chunk_fds;
    
    // Phase 1: 分批读取 + 排序 + 写入临时文件
    const int64_t BATCH_SIZE = 10 * 1000 * 1000;  // 每批 1000 万条
    int64_t offset = 0;
    while (offset < bucket_record_count) {
        ObArray<FileRecord> batch;
        read_batch(bucket_id, offset, BATCH_SIZE, batch);
        std::sort(batch.begin(), batch.end());
        
        int64_t chunk_fd;
        write_sorted_chunk(batch, chunk_fd);
        sorted_chunk_fds.push_back(chunk_fd);
        
        offset += BATCH_SIZE;
    }
    
    // Phase 2: K 路归并
    merge_sorted_chunks_and_write(bucket_id, sorted_chunk_fds);
    
    // 清理临时文件
    cleanup_sorted_chunks(sorted_chunk_fds);
    return OB_SUCCESS;
}
```

### 11.4 K 路归并核心逻辑

```cpp
int merge_sorted_chunks_and_write(int64_t bucket_id,
                                   const ObArray<int64_t> &chunk_fds) {
    // 最小堆：<FileRecord, chunk_idx>
    auto cmp = [](auto &a, auto &b) { return !(a.first < b.first); };
    std::priority_queue<...> heap(cmp);
    
    // 初始化：每个 chunk 的第一条记录入堆
    ObArray<ChunkReader> readers;
    for (int i = 0; i < chunk_fds.count(); i++) {
        readers[i].init(chunk_fds[i]);
        if (readers[i].read_next()) {
            heap.push({readers[i].current(), i});
        }
    }
    
    // 归并输出
    uint32_t current_token_id = UINT32_MAX;
    int64_t current_offset = posting_data_start_;
    
    while (!heap.empty()) {
        auto [record, idx] = heap.top();
        heap.pop();
        
        // Token 切换时记录元信息
        if (record.token_id != current_token_id) {
            if (current_token_id != UINT32_MAX) {
                token_metas_[current_token_id].count = ...;
            }
            current_token_id = record.token_id;
            token_metas_[current_token_id].offset = current_offset;
        }
        
        // 计算 BM25 并写入
        float score = calc_bm25(record.tf, record.doc_len, 
                                 token_df_[record.token_id]);
        pwrite(fd_, &record.doc_id, 4, current_offset);
        pwrite(fd_, &score, 4, current_offset + 4);
        current_offset += 8;
        
        // 从同一 chunk 读取下一条
        if (readers[idx].read_next()) {
            heap.push({readers[idx].current(), idx});
        }
    }
    
    // 回填 Vocab 和 Header
    write_vocab_and_header(token_metas_);
}
```

### 11.5 内存占用

| 组件 | 大小 |
|-----|------|
| token_df_[vocab_size] | ~40 MB（1000 万词） |
| token_metas_[vocab_size] | ~160 MB |
| 排序批次缓冲区 | ~200 MB |
| 归并堆 + 读取缓冲 | ~10 MB |
| **总计** | **~410 MB** |

### 11.6 查询时按需加载

外部排序保证 doc_id 有序，查询时可利用 id_limit 精确加载：

```cpp
int get_posting_with_limit(token, id_limit, posting, handle) {
    // 1. 获取 posting 元信息
    int64_t offset, count;
    get_posting_info(token, offset, count);
    
    // 2. 在文件中二分查找（不加载全部数据）
    int64_t actual_count = binary_search_in_file(offset, count, id_limit);
    
    // 3. 只读取需要的部分
    // 如果 count = 1 亿，但 actual_count = 1000
    // 只读取 8 KB 而不是 800 MB！
    return load_partial_posting(offset, actual_count, posting, handle);
}
```

### 11.7 实现优先级

| 优先级 | 方案 | 说明 |
|-------|------|------|
| **基础方案** | 固定 1024 桶 + 内存排序 + 按需加载 | 覆盖大多数场景 |
| **扩展方案** | 外部排序兜底 | 处理超大桶，内存可控 |

**注**：按需加载（KVCache + id_limit 精确读取）是 Phase 3 的基础设计，不是扩展优化。

---

## 12. 相关文件

- `src/sql/das/ob_fts_bm25s_index.h` - 结构体定义
- `src/sql/das/ob_fts_bm25s_index.cpp` - 实现
- `src/share/ob_io_device_helper.h` - 文件操作接口
- `src/storage/fts/ob_fts_stop_word.h` - 停用词过滤（已有）
- `BM25S_PHASE1_TMPFILE_BUCKET.md` - TmpFile 分桶设计
- `BM25S_PHASE3_KVCACHE_QUERY.md` - KVCache 查询设计
