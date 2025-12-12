# BM25S 索引查询 - Phase 3: KVCache 动态加载

## 1. 概述

### 1.1 目标
实现基于 KVCache 的一级缓存查询机制，支持大规模索引的动态加载和自动淘汰。

### 1.2 缓存策略

| 场景 | 策略 | 说明 |
|-----|------|------|
| **冷数据** | 按需加载 | 从文件读取 posting list，放入 KVCache |
| **热数据** | 缓存命中 | 直接从 KVCache 获取 |
| **id_limit 过滤** | 二分查找 | 在内存中二分查找，O(log n) |

### 1.3 架构（一级 KVCache）

```
┌─────────────────────────────────────────────────────────────┐
│ 常驻内存（启动时加载）                                        │
│   - file_header_: 元数据                                    │
│   - bucket_directory_: 桶目录（用于从文件查找 token）          │
│   - index_fd_: 文件描述符                                    │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│ PostingCache (ObKVCache)                                    │
│   Key:   (token_str)  ← 全局单例，无需 tenant/table         │
│   Value: (count, doc_ids[], scores[])                       │
│   作用:  token 字符串 → 倒排链数据                           │
└─────────────────────────────────────────────────────────────┘
```

**简化说明**：
- 一级缓存足够，直接 token → posting
- Key 使用 token 字符串，避免 hash 碰撞
- 查询逻辑与当前代码一致

### 1.4 使用的 SeekDB 接口
- `ObKVCache<Key, Value>` - KV 缓存
- `ObKVCacheHandle` - 缓存句柄，管理生命周期
- `ObIKVCacheKey` / `ObIKVCacheValue` - Key/Value 基类

---

## 2. KVCache Key/Value 定义（一级缓存）

### 2.1 需要引入的头文件

```cpp
#include "share/cache/ob_kv_storecache.h"
#include "share/cache/ob_kvcache_struct.h"
```

### 2.2 PostingCache Key（使用 token 字符串）

```cpp
// ob_fts_bm25s_cache.h

#ifndef OCEANBASE_SQL_DAS_OB_FTS_BM25S_CACHE_H_
#define OCEANBASE_SQL_DAS_OB_FTS_BM25S_CACHE_H_

#include "share/cache/ob_kvcache_struct.h"
#include "lib/string/ob_string.h"
#include "lib/hash/ob_hashutils.h"

namespace oceanbase
{
namespace sql
{

// ========================================================================
// 一级缓存：PostingCache - 直接 token → posting
// 注意：当前比赛场景为全局单例，无需 tenant_id/table_id
// ========================================================================

class ObBM25SPostingKey : public common::ObIKVCacheKey
{
public:
    ObBM25SPostingKey()
        : token_len_(0), token_data_(nullptr) {}
    
    ObBM25SPostingKey(const common::ObString &token)
        : token_len_(token.length()), token_data_(const_cast<char*>(token.ptr())) {}
    
    // 比较操作符（完整比较 token 字符串，避免 hash 碰撞）
    virtual bool operator==(const ObIKVCacheKey &other) const override
    {
        const ObBM25SPostingKey &o = static_cast<const ObBM25SPostingKey&>(other);
        if (token_len_ != o.token_len_) {
            return false;
        }
        return 0 == MEMCMP(token_data_, o.token_data_, token_len_);
    }
    
    // Hash 函数
    virtual uint64_t hash() const override
    {
        if (token_len_ > 0 && OB_NOT_NULL(token_data_)) {
            return common::murmurhash(token_data_, token_len_, 0);
        }
        return 0;
    }
    
    // 返回系统租户（全局单例模式）
    virtual uint64_t get_tenant_id() const override { return OB_SYS_TENANT_ID; }
    
    virtual int64_t size() const override 
    { 
        return sizeof(*this) + token_len_; 
    }
    
    // 深拷贝（包括 token 字符串）
    virtual int deep_copy(char *buf, const int64_t buf_len,
                          ObIKVCacheKey *&key) const override
    {
        int ret = OB_SUCCESS;
        int64_t need_size = size();
        
        if (OB_ISNULL(buf) || buf_len < need_size) {
            ret = OB_INVALID_ARGUMENT;
        } else {
            ObBM25SPostingKey *new_key = new (buf) ObBM25SPostingKey();
            new_key->token_len_ = token_len_;
            
            // token 数据紧随对象之后
            char *token_buf = buf + sizeof(ObBM25SPostingKey);
            if (token_len_ > 0 && OB_NOT_NULL(token_data_)) {
                MEMCPY(token_buf, token_data_, token_len_);
            }
            new_key->token_data_ = token_buf;
            
            key = new_key;
        }
        return ret;
    }
    
    common::ObString get_token() const 
    { 
        return common::ObString(token_len_, token_data_); 
    }
    
    TO_STRING_KV(K_(token_len));
    
public:
    int32_t token_len_;
    char *token_data_;
};
```

### 2.3 PostingCache Value

```cpp
class ObBM25SPostingValue : public common::ObIKVCacheValue
{
public:
    ObBM25SPostingValue()
        : count_(0), doc_ids_(nullptr), scores_(nullptr) {}
    
    // 设置数据
    void set(int64_t count, const uint32_t *doc_ids, const float *scores)
    {
        count_ = count;
        doc_ids_ = const_cast<uint32_t*>(doc_ids);
        scores_ = const_cast<float*>(scores);
    }
    
    // 获取数据大小
    virtual int64_t size() const override
    {
        return sizeof(*this) + count_ * (sizeof(uint32_t) + sizeof(float));
    }
    
    // 深拷贝
    virtual int deep_copy(char *buf, const int64_t buf_len,
                          ObIKVCacheValue *&value) const override
    {
        int ret = OB_SUCCESS;
        int64_t need_size = size();
        
        if (OB_ISNULL(buf) || buf_len < need_size) {
            ret = OB_INVALID_ARGUMENT;
        } else {
            ObBM25SPostingValue *new_val = new (buf) ObBM25SPostingValue();
            new_val->count_ = count_;
            
            char *data_buf = buf + sizeof(ObBM25SPostingValue);
            
            // 复制 doc_ids
            new_val->doc_ids_ = reinterpret_cast<uint32_t*>(data_buf);
            if (count_ > 0 && OB_NOT_NULL(doc_ids_)) {
                MEMCPY(new_val->doc_ids_, doc_ids_, count_ * sizeof(uint32_t));
            }
            data_buf += count_ * sizeof(uint32_t);
            
            // 复制 scores
            new_val->scores_ = reinterpret_cast<float*>(data_buf);
            if (count_ > 0 && OB_NOT_NULL(scores_)) {
                MEMCPY(new_val->scores_, scores_, count_ * sizeof(float));
            }
            
            value = new_val;
        }
        return ret;
    }
    
    // ========== 查询辅助函数 ==========
    
    // 获取 posting 数量
    int64_t count() const { return count_; }
    
    // 获取指定位置的 doc_id
    uint32_t get_doc_id(int64_t idx) const 
    { 
        return (idx >= 0 && idx < count_) ? doc_ids_[idx] : 0; 
    }
    
    // 获取指定位置的 score
    float get_score(int64_t idx) const 
    { 
        return (idx >= 0 && idx < count_) ? scores_[idx] : 0.0f; 
    }
    
    // ========== 热数据二分查找支持 ==========
    // doc_ids 在构建时已按升序排列，支持 O(log n) 二分查找
    
    // 二分查找：找到第一个 doc_id >= limit 的位置
    // 用于 id_limit 过滤优化
    int64_t find_first_ge(int64_t limit) const
    {
        if (count_ == 0 || limit <= 0) {
            return 0;
        }
        
        // 快速判断
        if (doc_ids_[0] >= static_cast<uint32_t>(limit)) {
            return 0;  // 第一个就 >= limit
        }
        if (doc_ids_[count_ - 1] < static_cast<uint32_t>(limit)) {
            return count_;  // 所有都 < limit
        }
        
        // 二分查找
        int64_t lo = 0, hi = count_;
        while (lo < hi) {
            int64_t mid = lo + (hi - lo) / 2;
            if (doc_ids_[mid] < static_cast<uint32_t>(limit)) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }
    
    // 获取 doc_id < id_limit 的有效数量（二分查找，O(log n)）
    int64_t get_valid_count(int64_t id_limit) const
    {
        if (id_limit <= 0 || count_ == 0) {
            return count_;  // 无过滤，返回全部
        }
        return find_first_ge(id_limit);
    }
    
    TO_STRING_KV(K_(count));
    
public:
    int64_t count_;
    uint32_t *doc_ids_;
    float *scores_;
};

// ========================================================================
// KVCache 类型定义（一级缓存）
// ========================================================================

typedef common::ObKVCache<ObBM25SPostingKey, ObBM25SPostingValue> ObBM25SPostingCache;

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_DAS_OB_FTS_BM25S_CACHE_H_
```

---

## 3. 缓存管理

### 3.1 全局缓存实例（一级）

```cpp
// ob_fts_bm25s_index.cpp

#include "ob_fts_bm25s_cache.h"

namespace oceanbase
{
namespace sql
{

// 全局缓存实例（单例，一级）
static ObBM25SPostingCache *g_posting_cache = nullptr;
static bool g_cache_inited = false;

int init_bm25s_cache()
{
    int ret = OB_SUCCESS;
    
    if (g_cache_inited) {
        return OB_SUCCESS;
    }
    
    // 分配缓存对象
    void *posting_buf = ob_malloc(sizeof(ObBM25SPostingCache), "BM25SCache");
    
    if (OB_ISNULL(posting_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        return ret;
    }
    
    g_posting_cache = new (posting_buf) ObBM25SPostingCache();
    
    // 初始化缓存
    // 参数：cache_name, priority (1-10, 越高优先级越高)
    if (OB_FAIL(g_posting_cache->init("bm25s_posting_cache", 1))) {
        LOG_WARN("fail to init posting cache", K(ret));
        return ret;
    }
    
    g_cache_inited = true;
    LOG_INFO("BM25S posting cache initialized");
    
    return ret;
}

void destroy_bm25s_cache()
{
    if (g_posting_cache) {
        g_posting_cache->destroy();
        g_posting_cache->~ObBM25SPostingCache();
        ob_free(g_posting_cache);
        g_posting_cache = nullptr;
    }
    g_cache_inited = false;
}

} // namespace sql
} // namespace oceanbase
```

### 3.2 缓存操作封装（一级）

```cpp
// 在 ObFtsBM25SIndex 类中添加

class ObFtsBM25SIndex {
public:
    // 从 PostingCache 获取倒排链（一级缓存，直接 token → posting）
    int get_posting_from_cache(const common::ObString &token,
                               const ObBM25SPostingValue *&posting,
                               common::ObKVCacheHandle &handle);
    
private:
    // 从文件加载 Posting 并放入缓存
    int load_posting_to_cache(const common::ObString &token,
                              const ObBM25SPostingValue *&posting,
                              common::ObKVCacheHandle &handle);
};
```

---

## 4. PostingCache 查询实现（一级）

### 4.1 获取 Posting

```cpp
int ObFtsBM25SIndex::get_posting_from_cache(const common::ObString &token,
                                             const ObBM25SPostingValue *&posting,
                                             common::ObKVCacheHandle &handle)
{
    int ret = OB_SUCCESS;
    
    if (!index_file_opened_) {
        ret = OB_NOT_INIT;
        return ret;
    }
    
    // 1. 尝试从缓存获取（Key 使用 token 字符串）
    ObBM25SPostingKey key(token);
    
    ret = g_posting_cache->get(key, posting, handle);
    
    if (OB_SUCCESS == ret) {
        // 缓存命中
        return OB_SUCCESS;
    }
    
    if (OB_HASH_NOT_EXIST == ret || OB_ENTRY_NOT_EXIST == ret) {
        // 2. 缓存未命中，从文件加载
        ret = load_posting_to_cache(token, posting, handle);
    }
    
    return ret;
}
```

### 4.2 从文件加载 Posting

```cpp
int ObFtsBM25SIndex::load_posting_to_cache(const common::ObString &token,
                                            const ObBM25SPostingValue *&posting,
                                            common::ObKVCacheHandle &handle)
{
    int ret = OB_SUCCESS;
    
    // 1. 从文件查找 token 的 posting 信息
    int64_t posting_offset = 0;
    int64_t posting_count = 0;
    
    if (OB_FAIL(find_token_in_file(token, posting_offset, posting_count))) {
        return ret;
    }
    
    if (posting_count == 0) {
        ret = OB_ENTRY_NOT_EXIST;
        return ret;
    }
    
    // 2. 计算数据大小和文件偏移
    int64_t doc_ids_size = posting_count * sizeof(uint32_t);
    int64_t scores_size = posting_count * sizeof(float);
    int64_t total_size = doc_ids_size + scores_size;
    int64_t file_offset = file_header_.posting_data_offset + posting_offset;
    
    // 3. 分配临时缓冲区
    char *buf = static_cast<char*>(allocator_.alloc(total_size));
    if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        return ret;
    }
    
    // 4. 从文件读取
    int64_t read_size = 0;
    ret = ObIODeviceLocalFileOp::pread_impl(index_fd_.second_id_, buf, total_size,
                                            file_offset, read_size);
    if (OB_FAIL(ret)) {
        LOG_WARN("fail to read posting", K(ret), K(token), K(file_offset));
        allocator_.free(buf);
        return ret;
    }
    
    // 5. 构建临时 Value
    ObBM25SPostingValue tmp_value;
    uint32_t *doc_ids = reinterpret_cast<uint32_t*>(buf);
    float *scores = reinterpret_cast<float*>(buf + doc_ids_size);
    tmp_value.set(posting_count, doc_ids, scores);
    
    // 6. 放入缓存（put_and_fetch 会调用 deep_copy 并返回缓存中的指针）
    ObBM25SPostingKey key(token);
    
    ret = g_posting_cache->put_and_fetch(key, tmp_value, posting, handle);
    if (OB_FAIL(ret)) {
        LOG_WARN("fail to put posting to cache", K(ret), K(token));
    }
    
    // 7. 释放临时缓冲区（数据已被 deep_copy 到 KVCache）
    allocator_.free(buf);
    
    return ret;
}
```

### 4.3 从文件查找 Token

```cpp
int ObFtsBM25SIndex::find_token_in_file(const common::ObString &token,
                                         int64_t &posting_offset,
                                         int64_t &posting_count)
{
    int ret = OB_SUCCESS;
    uint64_t token_hash = token.hash();
    
    // 1. 计算桶 ID（与 Phase 2 写入时一致）
    int32_t bucket_id = token_hash % file_header_.bucket_num;
    BucketDirectoryEntry &bucket = bucket_directory_[bucket_id];
    
    if (bucket.entry_count == 0) {
        return OB_ENTRY_NOT_EXIST;
    }
    
    // 2. 读取整个桶数据
    int64_t bucket_file_offset = file_header_.vocab_buckets_offset + bucket.offset;
    char *bucket_data = static_cast<char*>(allocator_.alloc(bucket.size));
    if (OB_ISNULL(bucket_data)) {
        return OB_ALLOCATE_MEMORY_FAILED;
    }
    
    int64_t read_size = 0;
    ret = ObIODeviceLocalFileOp::pread_impl(index_fd_.second_id_, bucket_data, 
                                            bucket.size, bucket_file_offset, read_size);
    if (OB_FAIL(ret)) {
        allocator_.free(bucket_data);
        return ret;
    }
    
    // 3. 遍历桶内 Entry 查找 token（按 Phase 2 格式解析）
    ret = OB_ENTRY_NOT_EXIST;
    char *p = bucket_data;
    char *end = bucket_data + bucket.size;
    
    while (p < end) {
        // 解析 VocabEntry（与 Phase 2 写入顺序一致）
        uint64_t entry_hash = *reinterpret_cast<uint64_t*>(p); p += 8;
        int32_t entry_token_len = *reinterpret_cast<int32_t*>(p); p += 4;
        const char *entry_token_data = p; p += entry_token_len;
        uint32_t entry_token_id = *reinterpret_cast<uint32_t*>(p); p += 4;  // 不使用
        int64_t entry_posting_offset = *reinterpret_cast<int64_t*>(p); p += 8;
        int32_t entry_posting_count = *reinterpret_cast<int32_t*>(p); p += 4;
        p += 4;  // reserved
        
        // 比较 token（先比 hash，再比内容）
        if (entry_hash == token_hash &&
            entry_token_len == token.length() &&
            0 == MEMCMP(entry_token_data, token.ptr(), token.length())) {
            // 找到了！
            posting_offset = entry_posting_offset;
            posting_count = entry_posting_count;
            ret = OB_SUCCESS;
            break;
        }
        
        // 优化：桶内按 hash 排序，如果 entry_hash > token_hash 可以提前退出
        if (entry_hash > token_hash) {
            break;
        }
    }
    
    allocator_.free(bucket_data);
    return ret;
}
```

---

## 5. 完整查询接口

### 5.1 搜索主函数（一级缓存版）

```cpp
int ObFtsBM25SIndex::search_with_cache(const common::ObIArray<common::ObString> &tokens,
                                        int64_t id_limit,
                                        int64_t top_k,
                                        common::ObIArray<DocScore> &results)
{
    int ret = OB_SUCCESS;
    
    if (!index_file_opened_) {
        ret = OB_NOT_INIT;
        LOG_WARN("index not loaded", K(ret));
        return ret;
    }
    
    if (tokens.empty() || top_k <= 0) {
        return OB_SUCCESS;
    }
    
    // 1. 初始化缓存（如果还没初始化）
    if (!g_cache_inited) {
        if (OB_FAIL(init_bm25s_cache())) {
            LOG_WARN("fail to init cache", K(ret));
            return ret;
        }
    }
    
    // 2. 文档得分累加器
    common::hash::ObHashMap<int64_t, double, common::hash::NoPthreadDefendMode> doc_scores;
    if (OB_FAIL(doc_scores.create(1024, "BM25SSearch"))) {
        LOG_WARN("fail to create doc_scores map", K(ret));
        return ret;
    }
    
    // 3. 遍历每个 token（一级缓存：直接 token → posting）
    for (int64_t i = 0; OB_SUCC(ret) && i < tokens.count(); ++i) {
        const common::ObString &token = tokens.at(i);
        
        if (token.empty()) {
            continue;
        }
        
        // 获取 posting（从缓存或文件）
        const ObBM25SPostingValue *posting = nullptr;
        common::ObKVCacheHandle handle;
        
        int posting_ret = get_posting_from_cache(token, posting, handle);
        if (OB_ENTRY_NOT_EXIST == posting_ret) {
            // 未知词，跳过
            continue;
        } else if (OB_FAIL(posting_ret)) {
            LOG_WARN("fail to get posting", K(posting_ret), K(token));
            continue;
        }
        
        // 累加得分
        int64_t valid_count = posting->get_valid_count(id_limit);
        for (int64_t j = 0; j < valid_count; ++j) {
            int64_t doc_id = posting->get_doc_id(j);
            double score = posting->get_score(j);
            
            double existing = 0.0;
            doc_scores.get_refactored(doc_id, existing);
            doc_scores.set_refactored(doc_id, existing + score, 1 /*overwrite*/);
        }
        
        // handle 析构时自动释放缓存引用
    }
    
    // 4. Top-K 排序
    if (OB_SUCC(ret)) {
        ret = extract_topk(doc_scores, top_k, results);
    }
    
    doc_scores.destroy();
    return ret;
}
```

### 6.2 Top-K 提取

```cpp
int ObFtsBM25SIndex::extract_topk(
    const common::hash::ObHashMap<int64_t, double, common::hash::NoPthreadDefendMode> &doc_scores,
    int64_t top_k,
    common::ObIArray<DocScore> &results)
{
    int ret = OB_SUCCESS;
    
    // 使用最小堆维护 Top-K
    std::priority_queue<DocScore, std::vector<DocScore>, std::greater<DocScore>> min_heap;
    
    for (auto it = doc_scores.begin(); it != doc_scores.end(); ++it) {
        DocScore ds(it->first, it->second);
        
        if (min_heap.size() < static_cast<size_t>(top_k)) {
            min_heap.push(ds);
        } else if (ds.score > min_heap.top().score) {
            min_heap.pop();
            min_heap.push(ds);
        }
    }
    
    // 导出结果（按分数降序）
    std::vector<DocScore> tmp;
    tmp.reserve(min_heap.size());
    while (!min_heap.empty()) {
        tmp.push_back(min_heap.top());
        min_heap.pop();
    }
    
    for (int64_t i = tmp.size() - 1; i >= 0; --i) {
        if (OB_FAIL(results.push_back(tmp[i]))) {
            LOG_WARN("fail to push result", K(ret));
            break;
        }
    }
    
    return ret;
}
```

---

## 7. 性能优化

### 7.1 批量预加载

```cpp
// 对于多 token 查询，可以并行预加载
int ObFtsBM25SIndex::prefetch_vocabs(const common::ObIArray<common::ObString> &tokens)
{
    // TODO: 并行预加载多个 token 的 vocab 信息
    // 减少串行 I/O 等待时间
    return OB_SUCCESS;
}
```

### 7.2 热点缓存预热

```cpp
// 启动时预热常用词汇
int ObFtsBM25SIndex::warmup_cache(int64_t max_entries)
{
    // TODO: 加载 Top-N 高频词汇到缓存
    return OB_SUCCESS;
}
```

### 7.3 缓存统计

```cpp
void ObFtsBM25SIndex::print_cache_stats()
{
    if (g_posting_cache) {
        // 使用系统租户 ID（全局单例模式）
        uint64_t tenant_id = OB_SYS_TENANT_ID;
        LOG_INFO("PostingCache stats",
                 K(g_posting_cache->size(tenant_id)),
                 K(g_posting_cache->get_hit_cnt(tenant_id)),
                 K(g_posting_cache->get_miss_cnt(tenant_id)),
                 K(g_posting_cache->get_hit_rate(tenant_id)));
    }
}
```

---

## 8. 注意事项

### 8.1 KVCacheHandle 生命周期
- `ObKVCacheHandle` 管理缓存对象的引用
- handle 有效期间，缓存对象不会被淘汰
- handle 析构后，缓存对象可能被淘汰
- **不要保存 `posting` 指针超过 `handle` 的生命周期**

### 8.2 线程安全
- KVCache 本身是线程安全的
- 多线程可以并发调用 `get`/`put`
- 从文件加载时需要注意并发控制（同一个 token 可能被多个线程同时加载）

### 8.3 内存管理
- KVCache 自动管理内存，LRU 淘汰
- 临时缓冲区使用 `allocator_`，用完立即释放
- 不需要手动释放 KVCache 中的对象

### 8.4 Key 设计
- PostingCache 的 key 直接使用 token 字符串
- `operator==` 完整比较字符串，无 hash 冲突问题
- 不匹配时需要从文件重新加载

---

## 9. 测试要点

1. **基本功能**：单 token 查询、多 token 查询
2. **缓存命中**：首次查询未命中，二次查询命中
3. **id_limit 过滤**：验证过滤逻辑正确
4. **并发查询**：多线程同时查询
5. **大数据量**：大量 token、大 posting list
6. **缓存淘汰**：验证 LRU 淘汰机制
7. **重启恢复**：重启后缓存清空，重新加载

---

## 10. 扩展计划：按需加载（id_limit 精确读取）

### 10.1 基础方案 vs 扩展方案

| 方案 | 说明 | 内存使用 |
|-----|------|---------|
| **基础方案** | 缓存完整 posting，内存二分过滤 | 较大 |
| **扩展方案** | 只加载 id_limit 范围内的数据 | 更小 |

### 10.2 扩展方案设计

```cpp
class ObBM25SPostingValue : public ObIKVCacheValue {
public:
    int64_t loaded_limit_;   // 已加载的边界（doc_id < loaded_limit_）
    int64_t count_;          // 已加载的数量
    uint32_t *doc_ids_;
    float *scores_;
};
```

### 10.3 查询流程

```cpp
int get_posting(const ObString &token,
                 int64_t id_limit,
                 const ObBM25SPostingValue *&posting,
                 ObKVCacheHandle &handle)
{
    ObBM25SPostingKey key(token);
    
    // 1. 尝试从缓存获取
    if (OB_SUCCESS == posting_cache_->get(key, posting, handle)) {
        if (id_limit <= 0 || id_limit <= posting->loaded_limit_) {
            // 已加载的数据足够
            return OB_SUCCESS;
        }
        // id_limit 变大了，删除旧缓存，重新加载
        posting_cache_->erase(key);
    }
    
    // 2. 从文件加载需要的范围
    return load_posting(token, id_limit, posting, handle);
}
```

### 10.4 从文件按需加载

```cpp
int load_posting(const ObString &token,
                  int64_t id_limit,
                  const ObBM25SPostingValue *&posting,
                  ObKVCacheHandle &handle)
{
    // 1. 获取元信息
    int64_t offset, total_count;
    get_posting_info(token, offset, total_count);
    
    // 2. 确定加载数量
    int64_t load_count, load_limit;
    if (id_limit <= 0) {
        load_count = total_count;
        load_limit = INT64_MAX;
    } else {
        // 在文件中二分查找，确定需要加载多少条
        load_count = binary_search_in_file(offset, total_count, id_limit);
        load_limit = id_limit;
    }
    
    // 3. 只读取需要的部分
    int64_t data_size = load_count * (sizeof(uint32_t) + sizeof(float));
    pread(fd_, buf, data_size, offset);
    
    // 4. 构造 Value 并放入缓存
    ObBM25SPostingValue value;
    value.loaded_limit_ = load_limit;
    value.count_ = load_count;
    // ...
    
    return posting_cache_->put_and_fetch(key, value, posting, handle);
}
```

### 10.5 文件中二分查找

```cpp
int64_t binary_search_in_file(int64_t offset, int64_t count, int64_t id_limit)
{
    int64_t lo = 0, hi = count;
    
    while (lo < hi) {
        int64_t mid = (lo + hi) / 2;
        
        // 只读取一个 doc_id（4 bytes）
        uint32_t doc_id;
        pread(fd_, &doc_id, sizeof(doc_id), offset + mid * sizeof(uint32_t));
        
        if (doc_id < id_limit) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    
    return lo;  // 返回 doc_id < id_limit 的数量
}
```

### 10.6 场景分析

| 场景 | 操作 | 性能 |
|-----|------|-----|
| 首次查询 id_limit=1000 | 加载 1000 条 | 快（少量 IO） |
| 再次查询 id_limit=500 | 用缓存，二分取前 500 | ⚡ 极快 |
| 查询 id_limit=2000 | 删除旧缓存，重新加载 | 稍慢 |
| 热数据重复查询 | 缓存命中 | ⚡ 极快 |

### 10.7 实现优先级

| 优先级 | 方案 | 说明 |
|-------|------|------|
| **基础** | 缓存完整 posting | 简单，先实现 |
| **扩展** | 按需加载 + id_limit 精确读取 | 内存优化 |

---

## 11. 相关文件

- `src/sql/das/ob_fts_bm25s_cache.h` - KVCache Key/Value 定义（新建）
- `src/sql/das/ob_fts_bm25s_index.h` - 类定义修改
- `src/sql/das/ob_fts_bm25s_index.cpp` - 实现修改
- `src/share/cache/ob_kv_storecache.h` - KVCache 接口参考
- `BM25S_PHASE1_TMPFILE_BUCKET.md` - TmpFile 分桶设计
- `BM25S_PHASE2_PERSISTENCE.md` - 持久化设计
