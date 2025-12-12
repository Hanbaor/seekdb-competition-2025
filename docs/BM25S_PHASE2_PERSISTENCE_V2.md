# BM25S 索引构建 - Phase 2: 持久化落盘（统一设计 V2）

## 1. 概述

### 1.1 设计目标
1. **统一分桶逻辑**：全程使用 `hash(token) % NUM_BUCKETS` 分桶
2. **简化 Finalize**：TmpFile 桶直接转换为 Vocab Bucket，无需重新分桶
3. **省内存**：收集阶段不需要 HashMap，省去 ~500MB 内存
4. **与 Phase 3 一致**：查询时使用相同的 hash 分桶逻辑

### 1.2 与 V1 设计的区别

| 方面 | V1 设计 | V2 设计（本文档） |
|-----|---------|------------------|
| 收集分桶 | token_id % 1024 | **hash(token) % 1024** |
| FileRecord | 固定 20 字节 | **变长（含 token_str）** |
| 需要 HashMap | 是（~500MB） | **否** |
| Finalize 重分桶 | 需要 | **不需要** |
| 概念数量 | 2 套桶 | **1 套桶** |

### 1.3 核心流程

```
收集阶段（Phase 1）：
├─ bucket_id = hash(token) % NUM_BUCKETS
├─ 写入 TmpFile[bucket_id]：{token_str, doc_id, tf, doc_len}（变长）
└─ 不需要 HashMap！

Finalize 阶段（Phase 2）：
├─ 逐桶处理 TmpFile
├─ 桶内排序（按 token_str, doc_id）
├─ 统计 df，计算 BM25
├─ 直接写入对应的 Vocab Bucket（不需要重新分桶）
└─ 写入 Posting Data

查询阶段（Phase 3）：
├─ bucket_id = hash(token) % NUM_BUCKETS
├─ 从 Vocab Bucket[bucket_id] 查找
└─ 与收集阶段使用相同的 hash 逻辑
```

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
    int32_t version;             // 版本号，当前为 2（V2 设计）
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
    static const int32_t CURRENT_VERSION = 2;  // V2 设计
    static const int64_t HEADER_SIZE = 256;
    static const int32_t DEFAULT_BUCKET_NUM = 1024;
};
```

### 2.3 Bucket Directory 结构 (16 KB)

```cpp
// 固定 1024 个桶（与 Phase 1 TmpFile 分桶数量一致）
static const int32_t NUM_BUCKETS = 1024;

struct BucketDirectoryEntry {
    int64_t vocab_offset;    // 该桶的 Vocab 数据在文件中的偏移
    int32_t vocab_size;      // 该桶的 Vocab 数据大小（字节）
    int32_t entry_count;     // 该桶包含的 token 数量
};
// sizeof(BucketDirectoryEntry) = 16 bytes
// 总大小 = 1024 * 16 = 16 KB
```

### 2.4 Vocab Bucket 结构

每个桶内按 token_hash 排序存储，支持二分查找：

```cpp
// 变长 VocabEntry 格式：
// [token_hash: 8][token_len: 2][token_str: N][posting_offset: 8][posting_count: 4]
// 
// 固定部分 = 8 + 2 + 8 + 4 = 22 bytes
// 变长部分 = token_len bytes

struct VocabEntryHeader {
    uint64_t token_hash;      // token 的 hash 值
    uint16_t token_len;       // token 字符串长度
    // char token_data[token_len];  // token 字符串（变长）
    // int64_t posting_offset;      // 紧随 token_data 之后
    // int32_t posting_count;       // 紧随 posting_offset 之后
};

// 桶内数据布局（按 token_hash 排序）：
// [Entry1][Entry2][Entry3]...
// 每个 Entry = [token_hash][token_len][token_str][posting_offset][posting_count]
```

**注意**：V2 设计中移除了 token_id 字段，因为查询时不需要它。

### 2.5 Posting Data 结构

每个 token 的 posting list 连续存储：

```cpp
// Posting Data 布局:
// [doc_ids: uint32_t * count][scores: float * count]
// 
// 示例：count = 100
// - doc_ids: 100 * 4 = 400 bytes
// - scores:  100 * 4 = 400 bytes
// - 总计: 800 bytes
```

---

## 3. Phase 1 变长 FileRecord 格式

### 3.1 TmpFile 记录格式

```cpp
// 变长记录格式：
// [token_len: 2][token_str: N][doc_id: 8][tf: 4][doc_len: 4]
// 
// 每条记录大小 = 2 + token_len + 8 + 4 + 4 = 18 + token_len

struct TmpFileRecord {
    uint16_t token_len;
    // char token_str[token_len];  // 变长
    // int64_t doc_id;
    // int32_t tf;
    // int32_t doc_len;
};
```

### 3.2 写入 TmpFile

```cpp
int write_variable_record(int64_t bucket_id, const ObString &token,
                          int64_t doc_id, int32_t tf, int32_t doc_len)
{
    // 计算记录大小
    uint16_t token_len = static_cast<uint16_t>(token.length());
    int64_t record_size = 2 + token_len + 8 + 4 + 4;
    
    // 构建记录
    char buf[record_size];
    char *p = buf;
    
    memcpy(p, &token_len, 2); p += 2;
    memcpy(p, token.ptr(), token_len); p += token_len;
    memcpy(p, &doc_id, 8); p += 8;
    memcpy(p, &tf, 4); p += 4;
    memcpy(p, &doc_len, 4); p += 4;
    
    // 写入 TmpFile
    ObSpinLockGuard guard(bucket_locks_[bucket_id]);
    return tmp_mgr->write(bucket_fds_[bucket_id], buf, record_size);
}
```

### 3.3 读取 TmpFile

```cpp
int read_variable_bucket(int64_t bucket_id, ObArray<TmpRecord> &records)
{
    int fd = bucket_fds_[bucket_id];
    lseek(fd, 0, SEEK_SET);
    
    while (true) {
        uint16_t token_len;
        if (read(fd, &token_len, 2) != 2) break;
        
        TmpRecord r;
        char *token_buf = static_cast<char*>(allocator_.alloc(token_len));
        read(fd, token_buf, token_len);
        r.token.assign_ptr(token_buf, token_len);
        
        read(fd, &r.doc_id, 8);
        read(fd, &r.tf, 4);
        read(fd, &r.doc_len, 4);
        
        records.push_back(r);
    }
    return OB_SUCCESS;
}
```

---

## 4. Finalize 流程（简化版）

### 4.1 主入口

```cpp
int ObFtsBM25SIndex::finalize_and_persist_v2(const char *index_path)
{
    int ret = OB_SUCCESS;
    
    // 1. 计算全局统计
    total_docs_ = unique_docs_.size();
    avg_doc_len_ = (total_docs_ > 0) ? 
                   static_cast<double>(total_doc_len_) / total_docs_ : 0.0;
    
    // 2. 创建持久化文件
    int fd = open(index_path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        return OB_IO_ERROR;
    }
    
    // 3. 预留 Header 和 Bucket Directory 空间
    int64_t header_size = BM25SIndexFileHeader::HEADER_SIZE;
    int64_t dir_size = NUM_BUCKETS * sizeof(BucketDirectoryEntry);
    lseek(fd, header_size + dir_size, SEEK_SET);
    
    BucketDirectoryEntry bucket_dir[NUM_BUCKETS] = {};
    
    // 4. 逐桶处理：直接转换，不需要重新分桶！
    int64_t vocab_size = 0;
    int64_t total_postings = 0;
    int64_t current_vocab_offset = 0;
    int64_t current_posting_offset = 0;
    
    // 记录 Vocab Buckets 起始位置
    int64_t vocab_section_start = lseek(fd, 0, SEEK_CUR);
    
    // 临时存储每个 token 的 posting 位置
    ObArray<TokenPostingInfo> all_token_infos;
    
    for (int64_t bucket_id = 0; OB_SUCC(ret) && bucket_id < NUM_BUCKETS; bucket_id++) {
        ret = process_bucket_v2(fd, bucket_id, bucket_dir[bucket_id],
                                vocab_size, total_postings,
                                current_vocab_offset, current_posting_offset,
                                all_token_infos);
    }
    
    // 5. 记录 Vocab Section 结束位置
    int64_t vocab_section_end = lseek(fd, 0, SEEK_CUR);
    int64_t vocab_section_size = vocab_section_end - vocab_section_start;
    
    // 6. 写入 Posting Data
    int64_t posting_section_start = vocab_section_end;
    ret = write_posting_data_v2(fd, all_token_infos, posting_section_start);
    
    // 7. 构建并写入 Header
    BM25SIndexFileHeader header = {};
    header.magic = BM25SIndexFileHeader::MAGIC;
    header.version = BM25SIndexFileHeader::CURRENT_VERSION;
    header.total_docs = total_docs_;
    header.avg_doc_len = avg_doc_len_;
    header.vocab_size = vocab_size;
    header.total_postings = total_postings;
    header.bucket_dir_offset = header_size;
    header.bucket_dir_size = dir_size;
    header.vocab_buckets_offset = vocab_section_start;
    header.vocab_buckets_size = vocab_section_size;
    header.posting_data_offset = posting_section_start;
    header.posting_data_size = total_postings * 8;
    header.bucket_num = NUM_BUCKETS;
    header.create_timestamp = time(nullptr);
    
    pwrite(fd, &header, sizeof(header), 0);
    pwrite(fd, bucket_dir, dir_size, header_size);
    
    // 8. 同步并关闭
    fsync(fd);
    close(fd);
    
    // 9. 清理 TmpFile
    cleanup_tmp_files();
    
    return ret;
}
```

### 4.2 处理单个桶

```cpp
int process_bucket_v2(int fd, int64_t bucket_id, BucketDirectoryEntry &dir_entry,
                      int64_t &vocab_size, int64_t &total_postings,
                      int64_t &current_vocab_offset, int64_t &current_posting_offset,
                      ObArray<TokenPostingInfo> &all_token_infos)
{
    int ret = OB_SUCCESS;
    
    // 1. 读取 TmpFile 桶（变长记录）
    ObArray<TmpRecord> records;
    if (OB_FAIL(read_variable_bucket(bucket_id, records))) {
        return ret;
    }
    
    if (records.empty()) {
        dir_entry.vocab_offset = current_vocab_offset;
        dir_entry.vocab_size = 0;
        dir_entry.entry_count = 0;
        return OB_SUCCESS;
    }
    
    // 2. 桶内排序（按 token_str, doc_id）
    std::sort(records.begin(), records.end(),
              [](const TmpRecord &a, const TmpRecord &b) {
                  int cmp = a.token.compare(b.token);
                  if (cmp != 0) return cmp < 0;
                  return a.doc_id < b.doc_id;
              });
    
    // 3. 按 token 分组，统计 df，准备 Vocab Entry
    dir_entry.vocab_offset = current_vocab_offset;
    int64_t bucket_vocab_size = 0;
    int64_t bucket_entry_count = 0;
    
    ObString current_token;
    int64_t group_start = 0;
    
    for (int64_t i = 0; i <= records.count(); i++) {
        bool is_new_token = (i == records.count()) || 
                            (records.at(i).token != current_token);
        
        if (is_new_token && !current_token.empty()) {
            int64_t df = i - group_start;
            
            // 记录 token 信息
            TokenPostingInfo info;
            info.token = current_token;
            info.token_hash = current_token.hash();
            info.posting_offset = current_posting_offset;
            info.posting_count = df;
            info.records_start = group_start;
            info.records_end = i;
            info.bucket_id = bucket_id;
            all_token_infos.push_back(info);
            
            // 计算 Vocab Entry 大小
            int64_t entry_size = 8 + 2 + current_token.length() + 8 + 4;
            bucket_vocab_size += entry_size;
            bucket_entry_count++;
            
            current_posting_offset += df * 8;  // 每个 posting = 8 bytes
            total_postings += df;
            vocab_size++;
        }
        
        if (i < records.count() && is_new_token) {
            current_token = records.at(i).token;
            group_start = i;
        }
    }
    
    // 4. 写入 Vocab Bucket（按 token_hash 排序）
    // 先对 all_token_infos 中该桶的 entry 按 hash 排序
    // （这里简化处理，实际可以在写入时排序）
    
    int64_t write_pos = lseek(fd, 0, SEEK_CUR);
    
    // 临时收集该桶的 token 信息
    ObArray<TokenPostingInfo*> bucket_infos;
    for (int64_t i = all_token_infos.count() - bucket_entry_count; 
         i < all_token_infos.count(); i++) {
        bucket_infos.push_back(&all_token_infos.at(i));
    }
    
    // 按 hash 排序
    std::sort(bucket_infos.begin(), bucket_infos.end(),
              [](TokenPostingInfo *a, TokenPostingInfo *b) {
                  return a->token_hash < b->token_hash;
              });
    
    // 写入 Vocab Entries
    for (auto *info : bucket_infos) {
        // 写入：token_hash(8) + token_len(2) + token_str(N) + 
        //       posting_offset(8) + posting_count(4)
        write(fd, &info->token_hash, 8);
        uint16_t token_len = info->token.length();
        write(fd, &token_len, 2);
        write(fd, info->token.ptr(), token_len);
        write(fd, &info->posting_offset, 8);
        int32_t count = info->posting_count;
        write(fd, &count, 4);
    }
    
    dir_entry.vocab_size = bucket_vocab_size;
    dir_entry.entry_count = bucket_entry_count;
    current_vocab_offset += bucket_vocab_size;
    
    return ret;
}
```

### 4.3 写入 Posting Data

```cpp
int write_posting_data_v2(int fd, const ObArray<TokenPostingInfo> &token_infos,
                          int64_t posting_section_start)
{
    int ret = OB_SUCCESS;
    
    // 需要重新读取 TmpFile 来获取每条记录的数据
    // 按 token 的 posting_offset 顺序写入
    
    for (const auto &info : token_infos) {
        // 读取该 token 所在的桶
        ObArray<TmpRecord> records;
        read_variable_bucket(info.bucket_id, records);
        
        // 排序
        std::sort(records.begin(), records.end(),
                  [](const TmpRecord &a, const TmpRecord &b) {
                      int cmp = a.token.compare(b.token);
                      if (cmp != 0) return cmp < 0;
                      return a.doc_id < b.doc_id;
                  });
        
        // 找到该 token 的记录范围
        int64_t df = info.posting_count;
        
        // 计算写入位置
        int64_t doc_ids_offset = posting_section_start + info.posting_offset;
        int64_t scores_offset = doc_ids_offset + df * sizeof(uint32_t);
        
        // 写入 doc_ids 和 scores
        for (int64_t i = info.records_start; i < info.records_end; i++) {
            const TmpRecord &r = records.at(i);
            
            uint32_t doc_id = static_cast<uint32_t>(r.doc_id);
            float score = calc_bm25_score(r.tf, r.doc_len, df);
            
            pwrite(fd, &doc_id, sizeof(doc_id), doc_ids_offset);
            pwrite(fd, &score, sizeof(score), scores_offset);
            
            doc_ids_offset += sizeof(uint32_t);
            scores_offset += sizeof(float);
        }
    }
    
    return ret;
}
```

### 4.4 优化版本：一遍写入

上面的实现会多次读取 TmpFile，可以优化为一遍处理：

```cpp
int finalize_and_persist_v2_optimized(const char *index_path)
{
    // 第一遍：统计每个 token 的 df 和 posting 位置
    // 第二遍：写入 Vocab + Posting
    
    // 详见下文的优化实现...
}
```

---

## 5. 优化的两遍 Finalize

### 5.1 第一遍：统计 + 预分配

```cpp
int finalize_pass1(ObArray<BucketInfo> &bucket_infos)
{
    int ret = OB_SUCCESS;
    
    int64_t current_posting_offset = 0;
    
    for (int64_t bucket_id = 0; bucket_id < NUM_BUCKETS; bucket_id++) {
        BucketInfo &bi = bucket_infos.at(bucket_id);
        
        // 读取桶
        ObArray<TmpRecord> records;
        read_variable_bucket(bucket_id, records);
        
        if (records.empty()) continue;
        
        // 排序
        std::sort(records.begin(), records.end(), ...);
        
        // 按 token 分组统计
        ObString current_token;
        int64_t group_start = 0;
        
        for (int64_t i = 0; i <= records.count(); i++) {
            bool is_new_token = ...;
            
            if (is_new_token && !current_token.empty()) {
                int64_t df = i - group_start;
                
                TokenInfo ti;
                ti.token = current_token;
                ti.token_hash = current_token.hash();
                ti.df = df;
                ti.posting_offset = current_posting_offset;
                bi.tokens.push_back(ti);
                
                current_posting_offset += df * 8;
            }
            
            if (i < records.count() && is_new_token) {
                current_token = records.at(i).token;
                group_start = i;
            }
        }
        
        // 按 hash 排序（用于写入 Vocab Bucket）
        std::sort(bi.tokens.begin(), bi.tokens.end(),
                  [](const TokenInfo &a, const TokenInfo &b) {
                      return a.token_hash < b.token_hash;
                  });
    }
    
    return ret;
}
```

### 5.2 第二遍：写入文件

```cpp
int finalize_pass2(int fd, const ObArray<BucketInfo> &bucket_infos)
{
    int ret = OB_SUCCESS;
    BucketDirectoryEntry bucket_dir[NUM_BUCKETS] = {};
    
    // 计算各 Section 位置
    int64_t header_size = 256;
    int64_t dir_size = NUM_BUCKETS * 16;
    int64_t vocab_start = header_size + dir_size;
    
    // 先计算 Vocab Section 总大小
    int64_t vocab_total_size = 0;
    for (const auto &bi : bucket_infos) {
        for (const auto &ti : bi.tokens) {
            vocab_total_size += 8 + 2 + ti.token.length() + 8 + 4;
        }
    }
    
    int64_t posting_start = vocab_start + vocab_total_size;
    
    // 写入 Vocab Buckets
    lseek(fd, vocab_start, SEEK_SET);
    int64_t current_vocab_offset = 0;
    
    for (int64_t bucket_id = 0; bucket_id < NUM_BUCKETS; bucket_id++) {
        const BucketInfo &bi = bucket_infos.at(bucket_id);
        
        bucket_dir[bucket_id].vocab_offset = current_vocab_offset;
        bucket_dir[bucket_id].entry_count = bi.tokens.count();
        
        int64_t bucket_size = 0;
        for (const auto &ti : bi.tokens) {
            // 写入 Vocab Entry
            write(fd, &ti.token_hash, 8);
            uint16_t token_len = ti.token.length();
            write(fd, &token_len, 2);
            write(fd, ti.token.ptr(), token_len);
            write(fd, &ti.posting_offset, 8);
            int32_t count = ti.df;
            write(fd, &count, 4);
            
            bucket_size += 8 + 2 + token_len + 8 + 4;
        }
        
        bucket_dir[bucket_id].vocab_size = bucket_size;
        current_vocab_offset += bucket_size;
    }
    
    // 写入 Posting Data（需要再次遍历桶）
    for (int64_t bucket_id = 0; bucket_id < NUM_BUCKETS; bucket_id++) {
        const BucketInfo &bi = bucket_infos.at(bucket_id);
        
        if (bi.tokens.empty()) continue;
        
        // 读取桶
        ObArray<TmpRecord> records;
        read_variable_bucket(bucket_id, records);
        
        // 排序
        std::sort(records.begin(), records.end(), ...);
        
        // 按 token 分组写入 Posting
        ObString current_token;
        int64_t group_start = 0;
        int64_t token_idx = 0;
        
        for (int64_t i = 0; i <= records.count(); i++) {
            bool is_new_token = ...;
            
            if (is_new_token && !current_token.empty()) {
                // 找到对应的 TokenInfo（按原始顺序）
                const TokenInfo &ti = bi.tokens.at(token_idx++);
                int64_t df = ti.df;
                
                // 计算写入位置
                int64_t base = posting_start + ti.posting_offset;
                int64_t doc_pos = base;
                int64_t score_pos = base + df * 4;
                
                // 写入
                for (int64_t j = group_start; j < i; j++) {
                    const TmpRecord &r = records.at(j);
                    uint32_t doc_id = r.doc_id;
                    float score = calc_bm25_score(r.tf, r.doc_len, df);
                    
                    pwrite(fd, &doc_id, 4, doc_pos); doc_pos += 4;
                    pwrite(fd, &score, 4, score_pos); score_pos += 4;
                }
            }
            
            if (i < records.count() && is_new_token) {
                current_token = records.at(i).token;
                group_start = i;
            }
        }
    }
    
    // 写入 Header 和 Directory
    // ...
    
    return ret;
}
```

---

## 6. 与 Phase 3 的一致性

### 6.1 查询时的分桶逻辑

```cpp
// Phase 3: find_token_in_file
int32_t bucket_id = token.hash() % file_header_.bucket_num;

// 与 Phase 1/2 的分桶逻辑完全一致！
// Phase 1: bucket_id = token.hash() % NUM_BUCKETS
// Phase 2: 直接转换，不重新分桶
```

### 6.2 Vocab Entry 解析

```cpp
// Phase 3 解析格式与 Phase 2 写入格式一致：
uint64_t entry_hash = *reinterpret_cast<uint64_t*>(p); p += 8;
uint16_t entry_token_len = *reinterpret_cast<uint16_t*>(p); p += 2;
const char *entry_token_data = p; p += entry_token_len;
int64_t entry_posting_offset = *reinterpret_cast<int64_t*>(p); p += 8;
int32_t entry_posting_count = *reinterpret_cast<int32_t*>(p); p += 4;
```

**注意**：V2 设计中没有 token_id 字段。

---

## 7. 内存占用分析

| 阶段 | V1 设计 | V2 设计 |
|-----|---------|---------|
| **收集阶段** | ~500 MB (HashMap) | **~0**（只有 unique_docs） |
| **Finalize** | ~200 MB | ~200 MB |
| **总峰值** | ~700 MB | **~200 MB** |

---

## 8. 文件大小估算

| 数据 | 计算公式 | 100万词汇/1亿posting |
|-----|---------|---------------------|
| Header | 256 B | 256 B |
| Bucket Directory | 1024 × 16 B | 16 KB |
| Vocab Buckets | vocab_size × (22 + avg_token_len) | ~40 MB |
| Posting Data | total_postings × 8 B | ~800 MB |
| **总计** | | **~840 MB** |

---

## 9. 总结

### 9.1 V2 设计的优势

1. **统一分桶逻辑**：全程 `hash(token) % 1024`
2. **省内存**：收集阶段不需要 HashMap
3. **简化 Finalize**：不需要重新分桶
4. **概念清晰**：一套桶贯穿始终
5. **与 Phase 3 一致**：查询用相同逻辑

### 9.2 代价

1. TmpFile 使用变长记录
2. 变长解析代码稍复杂

### 9.3 适用场景

- 大规模索引构建（省内存）
- 追求设计简洁性
- 需要持久化 + 按需加载
