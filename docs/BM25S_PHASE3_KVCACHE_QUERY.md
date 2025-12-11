# BM25S 索引查询 - Phase 3: 两级 KVCache 动态加载

## 1. 概述

### 1.1 目标
实现基于 KVCache 的两级缓存查询机制，支持大规模索引的动态加载和自动淘汰。

### 1.2 缓存策略

| 场景 | 策略 | 说明 |
|-----|------|------|
| **冷数据** | 全量加载 | 从文件读取整个 posting list，放入 KVCache |
| **热数据** | 缓存命中 | 直接从 KVCache 获取 |
| **id_limit 过滤** | 二分查找 | 在内存中二分查找，O(log n) |

### 1.3 架构

```
┌─────────────────────────────────────────────────────────────┐
│ 常驻内存（启动时加载）                                        │
│   - file_header_: 元数据                                    │
│   - bucket_directory_: 桶目录（64 KB）                       │
│   - index_fd_: 文件描述符                                    │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│ Level 1: VocabCache (ObKVCache)                             │
│   Key:   (tenant_id, table_id, token_hash)                  │
│   Value: (token_str, token_id, posting_offset, posting_cnt) │
│   作用:  token 字符串 → token 元信息                         │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│ Level 2: PostingCache (ObKVCache)                           │
│   Key:   (tenant_id, table_id, token_id)                    │
│   Value: (doc_ids[], scores[])                              │
│   作用:  token_id → 倒排链数据                               │
└─────────────────────────────────────────────────────────────┘
```

### 1.4 使用的 SeekDB 接口
- `ObKVCache<Key, Value>` - KV 缓存
- `ObKVCacheHandle` - 缓存句柄，管理生命周期
- `ObIKVCacheKey` / `ObIKVCacheValue` - Key/Value 基类

---

## 2. KVCache Key/Value 定义

### 2.1 需要引入的头文件

```cpp
#include "share/cache/ob_kv_storecache.h"
#include "share/cache/ob_kvcache_struct.h"
```

### 2.2 VocabCache Key

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
// Level 1: VocabCache - 词汇表缓存
// ========================================================================

class ObBM25SVocabKey : public common::ObIKVCacheKey
{
public:
    ObBM25SVocabKey() 
        : tenant_id_(0), table_id_(0), token_hash_(0) {}
    
    ObBM25SVocabKey(uint64_t tenant_id, uint64_t table_id, uint64_t token_hash)
        : tenant_id_(tenant_id), table_id_(table_id), token_hash_(token_hash) {}
    
    // 比较操作符
    virtual bool operator==(const ObIKVCacheKey &other) const override
    {
        const ObBM25SVocabKey &o = static_cast<const ObBM25SVocabKey&>(other);
        return tenant_id_ == o.tenant_id_ 
            && table_id_ == o.table_id_ 
            && token_hash_ == o.token_hash_;
    }
    
    // Hash 函数
    virtual uint64_t hash() const override
    {
        uint64_t h = tenant_id_;
        h = common::murmurhash(&table_id_, sizeof(table_id_), h);
        h = common::murmurhash(&token_hash_, sizeof(token_hash_), h);
        return h;
    }
    
    // 获取租户 ID（KVCache 用于内存管理）
    virtual uint64_t get_tenant_id() const override 
    { 
        return tenant_id_; 
    }
    
    // 获取 Key 大小
    virtual int64_t size() const override 
    { 
        return sizeof(*this); 
    }
    
    // 深拷贝（KVCache 内部调用）
    virtual int deep_copy(char *buf, const int64_t buf_len, 
                          ObIKVCacheKey *&key) const override
    {
        int ret = OB_SUCCESS;
        if (OB_ISNULL(buf) || buf_len < static_cast<int64_t>(sizeof(*this))) {
            ret = OB_INVALID_ARGUMENT;
        } else {
            ObBM25SVocabKey *new_key = new (buf) ObBM25SVocabKey();
            new_key->tenant_id_ = tenant_id_;
            new_key->table_id_ = table_id_;
            new_key->token_hash_ = token_hash_;
            key = new_key;
        }
        return ret;
    }
    
    TO_STRING_KV(K_(tenant_id), K_(table_id), K_(token_hash));
    
public:
    uint64_t tenant_id_;
    uint64_t table_id_;
    uint64_t token_hash_;
};
```

### 2.3 VocabCache Value

```cpp
class ObBM25SVocabValue : public common::ObIKVCacheValue
{
public:
    ObBM25SVocabValue() 
        : token_len_(0), token_data_(nullptr), token_id_(0),
          posting_offset_(0), posting_count_(0) {}
    
    // 设置数据
    void set(const common::ObString &token, uint32_t token_id,
             int64_t posting_offset, int32_t posting_count)
    {
        token_len_ = token.length();
        token_data_ = const_cast<char*>(token.ptr());
        token_id_ = token_id;
        posting_offset_ = posting_offset;
        posting_count_ = posting_count;
    }
    
    // 获取 token 字符串
    common::ObString get_token() const 
    { 
        return common::ObString(token_len_, token_data_); 
    }
    
    // 获取 Value 大小（包括 token 字符串）
    virtual int64_t size() const override
    {
        return sizeof(*this) + token_len_;
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
            // 在 buf 中构造对象
            ObBM25SVocabValue *new_val = new (buf) ObBM25SVocabValue();
            new_val->token_len_ = token_len_;
            new_val->token_id_ = token_id_;
            new_val->posting_offset_ = posting_offset_;
            new_val->posting_count_ = posting_count_;
            
            // token 数据紧随对象之后
            char *token_buf = buf + sizeof(ObBM25SVocabValue);
            if (token_len_ > 0 && OB_NOT_NULL(token_data_)) {
                MEMCPY(token_buf, token_data_, token_len_);
            }
            new_val->token_data_ = token_buf;
            
            value = new_val;
        }
        return ret;
    }
    
    TO_STRING_KV(K_(token_len), K_(token_id), K_(posting_offset), K_(posting_count));
    
public:
    int32_t token_len_;
    char *token_data_;      // 指向 token 字符串
    uint32_t token_id_;
    int64_t posting_offset_;
    int32_t posting_count_;
};
```

### 2.4 PostingCache Key

```cpp
// ========================================================================
// Level 2: PostingCache - 倒排链缓存
// ========================================================================

class ObBM25SPostingKey : public common::ObIKVCacheKey
{
public:
    ObBM25SPostingKey()
        : tenant_id_(0), table_id_(0), token_id_(0) {}
    
    ObBM25SPostingKey(uint64_t tenant_id, uint64_t table_id, uint32_t token_id)
        : tenant_id_(tenant_id), table_id_(table_id), token_id_(token_id) {}
    
    virtual bool operator==(const ObIKVCacheKey &other) const override
    {
        const ObBM25SPostingKey &o = static_cast<const ObBM25SPostingKey&>(other);
        return tenant_id_ == o.tenant_id_ 
            && table_id_ == o.table_id_ 
            && token_id_ == o.token_id_;
    }
    
    virtual uint64_t hash() const override
    {
        uint64_t h = tenant_id_;
        h = common::murmurhash(&table_id_, sizeof(table_id_), h);
        h = common::murmurhash(&token_id_, sizeof(token_id_), h);
        return h;
    }
    
    virtual uint64_t get_tenant_id() const override { return tenant_id_; }
    virtual int64_t size() const override { return sizeof(*this); }
    
    virtual int deep_copy(char *buf, const int64_t buf_len,
                          ObIKVCacheKey *&key) const override
    {
        int ret = OB_SUCCESS;
        if (OB_ISNULL(buf) || buf_len < static_cast<int64_t>(sizeof(*this))) {
            ret = OB_INVALID_ARGUMENT;
        } else {
            ObBM25SPostingKey *new_key = new (buf) ObBM25SPostingKey();
            new_key->tenant_id_ = tenant_id_;
            new_key->table_id_ = table_id_;
            new_key->token_id_ = token_id_;
            key = new_key;
        }
        return ret;
    }
    
    TO_STRING_KV(K_(tenant_id), K_(table_id), K_(token_id));
    
public:
    uint64_t tenant_id_;
    uint64_t table_id_;
    uint32_t token_id_;
};
```

### 2.5 PostingCache Value

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
// KVCache 类型定义
// ========================================================================

typedef common::ObKVCache<ObBM25SVocabKey, ObBM25SVocabValue> ObBM25SVocabCache;
typedef common::ObKVCache<ObBM25SPostingKey, ObBM25SPostingValue> ObBM25SPostingCache;

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_DAS_OB_FTS_BM25S_CACHE_H_
```

---

## 3. 缓存管理

### 3.1 全局缓存实例

```cpp
// ob_fts_bm25s_index.cpp

#include "ob_fts_bm25s_cache.h"

namespace oceanbase
{
namespace sql
{

// 全局缓存实例（单例）
static ObBM25SVocabCache *g_vocab_cache = nullptr;
static ObBM25SPostingCache *g_posting_cache = nullptr;
static bool g_cache_inited = false;

int init_bm25s_cache()
{
    int ret = OB_SUCCESS;
    
    if (g_cache_inited) {
        return OB_SUCCESS;
    }
    
    // 分配缓存对象
    void *vocab_buf = ob_malloc(sizeof(ObBM25SVocabCache), "BM25SCache");
    void *posting_buf = ob_malloc(sizeof(ObBM25SPostingCache), "BM25SCache");
    
    if (OB_ISNULL(vocab_buf) || OB_ISNULL(posting_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        if (vocab_buf) ob_free(vocab_buf);
        if (posting_buf) ob_free(posting_buf);
        return ret;
    }
    
    g_vocab_cache = new (vocab_buf) ObBM25SVocabCache();
    g_posting_cache = new (posting_buf) ObBM25SPostingCache();
    
    // 初始化缓存
    // 参数：cache_name, priority (1-10, 越高优先级越高)
    if (OB_FAIL(g_vocab_cache->init("bm25s_vocab_cache", 1))) {
        LOG_WARN("fail to init vocab cache", K(ret));
        return ret;
    }
    
    if (OB_FAIL(g_posting_cache->init("bm25s_posting_cache", 1))) {
        LOG_WARN("fail to init posting cache", K(ret));
        return ret;
    }
    
    g_cache_inited = true;
    LOG_INFO("BM25S cache initialized");
    
    return ret;
}

void destroy_bm25s_cache()
{
    if (g_vocab_cache) {
        g_vocab_cache->destroy();
        g_vocab_cache->~ObBM25SVocabCache();
        ob_free(g_vocab_cache);
        g_vocab_cache = nullptr;
    }
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

### 3.2 缓存操作封装

```cpp
// 在 ObFtsBM25SIndex 类中添加

class ObFtsBM25SIndex {
public:
    // 从 VocabCache 获取 token 信息
    int get_vocab_from_cache(const common::ObString &token,
                             uint32_t &token_id,
                             int64_t &posting_offset,
                             int32_t &posting_count);
    
    // 从 PostingCache 获取倒排链
    int get_posting_from_cache(uint32_t token_id,
                               int64_t posting_offset,
                               int32_t posting_count,
                               const ObBM25SPostingValue *&posting,
                               common::ObKVCacheHandle &handle);
    
private:
    // 从文件加载 Vocab 并放入缓存
    int load_vocab_to_cache(uint64_t token_hash,
                            const common::ObString &token,
                            uint32_t &token_id,
                            int64_t &posting_offset,
                            int32_t &posting_count);
    
    // 从文件加载 Posting 并放入缓存
    int load_posting_to_cache(uint32_t token_id,
                              int64_t posting_offset,
                              int32_t posting_count,
                              const ObBM25SPostingValue *&posting,
                              common::ObKVCacheHandle &handle);
};
```

---

## 4. Level 1: VocabCache 查询实现

### 4.1 获取 Token 信息

```cpp
int ObFtsBM25SIndex::get_vocab_from_cache(const common::ObString &token,
                                           uint32_t &token_id,
                                           int64_t &posting_offset,
                                           int32_t &posting_count)
{
    int ret = OB_SUCCESS;
    
    if (!index_file_opened_) {
        ret = OB_NOT_INIT;
        LOG_WARN("index file not opened", K(ret));
        return ret;
    }
    
    uint64_t token_hash = token.hash();
    
    // 1. 尝试从缓存获取
    ObBM25SVocabKey key(tenant_id_, table_id_, token_hash);
    const ObBM25SVocabValue *value = nullptr;
    common::ObKVCacheHandle handle;
    
    ret = g_vocab_cache->get(key, value, handle);
    
    if (OB_SUCCESS == ret) {
        // 缓存命中，验证 token 字符串（处理 hash 冲突）
        if (value->get_token() == token) {
            token_id = value->token_id_;
            posting_offset = value->posting_offset_;
            posting_count = value->posting_count_;
            return OB_SUCCESS;
        }
        // Hash 冲突，需要从文件查找
        ret = OB_HASH_NOT_EXIST;
    }
    
    if (OB_HASH_NOT_EXIST == ret || OB_ENTRY_NOT_EXIST == ret) {
        // 2. 缓存未命中，从文件加载
        ret = load_vocab_to_cache(token_hash, token, token_id, posting_offset, posting_count);
    }
    
    return ret;
}
```

### 4.2 从文件加载 Vocab

```cpp
int ObFtsBM25SIndex::load_vocab_to_cache(uint64_t token_hash,
                                          const common::ObString &token,
                                          uint32_t &token_id,
                                          int64_t &posting_offset,
                                          int32_t &posting_count)
{
    int ret = OB_SUCCESS;
    
    // 1. 计算桶 ID
    int32_t bucket_id = token_hash % file_header_.bucket_num;
    BucketDirectoryEntry &bucket = bucket_directory_[bucket_id];
    
    if (bucket.entry_count == 0) {
        ret = OB_ENTRY_NOT_EXIST;
        return ret;
    }
    
    // 2. 读取整个桶的数据
    int64_t bucket_offset = file_header_.vocab_buckets_offset + bucket.offset;
    char *bucket_buf = static_cast<char*>(allocator_.alloc(bucket.size));
    if (OB_ISNULL(bucket_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        return ret;
    }
    
    int64_t read_size = 0;
    ret = ObIODeviceLocalFileOp::pread_impl(index_fd_.second_id_, bucket_buf,
                                            bucket.size, bucket_offset, read_size);
    if (OB_FAIL(ret)) {
        LOG_WARN("fail to read vocab bucket", K(ret), K(bucket_id));
        allocator_.free(bucket_buf);
        return ret;
    }
    
    // 3. 在桶内查找 token
    bool found = false;
    char *p = bucket_buf;
    char *end = bucket_buf + bucket.size;
    
    while (p < end) {
        // 读取 entry
        uint64_t entry_hash;
        int32_t entry_token_len;
        
        memcpy(&entry_hash, p, 8); p += 8;
        memcpy(&entry_token_len, p, 4); p += 4;
        
        char *entry_token_data = p;
        p += entry_token_len;
        
        uint32_t entry_token_id;
        int64_t entry_posting_offset;
        int32_t entry_posting_count;
        int32_t reserved;
        
        memcpy(&entry_token_id, p, 4); p += 4;
        memcpy(&entry_posting_offset, p, 8); p += 8;
        memcpy(&entry_posting_count, p, 4); p += 4;
        memcpy(&reserved, p, 4); p += 4;
        
        // 比较 hash 和 token 字符串
        if (entry_hash == token_hash) {
            common::ObString entry_token(entry_token_len, entry_token_data);
            if (entry_token == token) {
                // 找到了
                token_id = entry_token_id;
                posting_offset = entry_posting_offset;
                posting_count = entry_posting_count;
                found = true;
                
                // 放入缓存
                ObBM25SVocabKey cache_key(tenant_id_, table_id_, token_hash);
                ObBM25SVocabValue cache_value;
                cache_value.set(token, token_id, posting_offset, posting_count);
                
                int cache_ret = g_vocab_cache->put(cache_key, cache_value);
                if (OB_FAIL(cache_ret)) {
                    LOG_WARN("fail to put vocab to cache", K(cache_ret), K(token));
                    // 缓存失败不影响查询结果
                }
                
                break;
            }
        }
        
        // 桶内按 hash 排序，如果当前 hash > token_hash，可以提前退出
        if (entry_hash > token_hash) {
            break;
        }
    }
    
    allocator_.free(bucket_buf);
    
    if (!found) {
        ret = OB_ENTRY_NOT_EXIST;
    }
    
    return ret;
}
```

---

## 5. Level 2: PostingCache 查询实现

### 5.1 获取 Posting

```cpp
int ObFtsBM25SIndex::get_posting_from_cache(uint32_t token_id,
                                             int64_t posting_offset,
                                             int32_t posting_count,
                                             const ObBM25SPostingValue *&posting,
                                             common::ObKVCacheHandle &handle)
{
    int ret = OB_SUCCESS;
    
    if (posting_count == 0) {
        ret = OB_ENTRY_NOT_EXIST;
        return ret;
    }
    
    // 1. 尝试从缓存获取
    ObBM25SPostingKey key(tenant_id_, table_id_, token_id);
    
    ret = g_posting_cache->get(key, posting, handle);
    
    if (OB_SUCCESS == ret) {
        // 缓存命中
        return OB_SUCCESS;
    }
    
    if (OB_HASH_NOT_EXIST == ret || OB_ENTRY_NOT_EXIST == ret) {
        // 2. 缓存未命中，从文件加载
        ret = load_posting_to_cache(token_id, posting_offset, posting_count, posting, handle);
    }
    
    return ret;
}
```

### 5.2 从文件加载 Posting

```cpp
int ObFtsBM25SIndex::load_posting_to_cache(uint32_t token_id,
                                            int64_t posting_offset,
                                            int32_t posting_count,
                                            const ObBM25SPostingValue *&posting,
                                            common::ObKVCacheHandle &handle)
{
    int ret = OB_SUCCESS;
    
    // 1. 计算数据大小和文件偏移
    int64_t doc_ids_size = posting_count * sizeof(uint32_t);
    int64_t scores_size = posting_count * sizeof(float);
    int64_t total_size = doc_ids_size + scores_size;
    int64_t file_offset = file_header_.posting_data_offset + posting_offset;
    
    // 2. 分配临时缓冲区
    char *buf = static_cast<char*>(allocator_.alloc(total_size));
    if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        return ret;
    }
    
    // 3. 从文件读取
    int64_t read_size = 0;
    ret = ObIODeviceLocalFileOp::pread_impl(index_fd_.second_id_, buf, total_size,
                                            file_offset, read_size);
    if (OB_FAIL(ret)) {
        LOG_WARN("fail to read posting", K(ret), K(token_id), K(file_offset));
        allocator_.free(buf);
        return ret;
    }
    
    // 4. 构建临时 Value
    ObBM25SPostingValue tmp_value;
    uint32_t *doc_ids = reinterpret_cast<uint32_t*>(buf);
    float *scores = reinterpret_cast<float*>(buf + doc_ids_size);
    tmp_value.set(posting_count, doc_ids, scores);
    
    // 5. 放入缓存（put_and_fetch 会调用 deep_copy 并返回缓存中的指针）
    ObBM25SPostingKey key(tenant_id_, table_id_, token_id);
    
    ret = g_posting_cache->put_and_fetch(key, tmp_value, posting, handle);
    if (OB_FAIL(ret)) {
        LOG_WARN("fail to put posting to cache", K(ret), K(token_id));
    }
    
    // 6. 释放临时缓冲区（数据已被 deep_copy 到 KVCache）
    allocator_.free(buf);
    
    return ret;
}
```

---

## 6. 完整查询接口

### 6.1 搜索主函数

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
    
    // 3. 遍历每个 token
    for (int64_t i = 0; OB_SUCC(ret) && i < tokens.count(); ++i) {
        const common::ObString &token = tokens.at(i);
        
        if (token.empty()) {
            continue;
        }
        
        // 3.1 Level 1: 获取 token 信息
        uint32_t token_id = 0;
        int64_t posting_offset = 0;
        int32_t posting_count = 0;
        
        int vocab_ret = get_vocab_from_cache(token, token_id, posting_offset, posting_count);
        if (OB_ENTRY_NOT_EXIST == vocab_ret) {
            // 未知词，跳过
            continue;
        } else if (OB_FAIL(vocab_ret)) {
            LOG_WARN("fail to get vocab", K(vocab_ret), K(token));
            continue;  // 继续处理其他 token
        }
        
        if (posting_count == 0) {
            continue;
        }
        
        // 3.2 Level 2: 获取 posting list
        const ObBM25SPostingValue *posting = nullptr;
        common::ObKVCacheHandle handle;
        
        int posting_ret = get_posting_from_cache(token_id, posting_offset, posting_count,
                                                  posting, handle);
        if (OB_FAIL(posting_ret)) {
            LOG_WARN("fail to get posting", K(posting_ret), K(token_id));
            continue;
        }
        
        // 3.3 累加得分
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
    if (g_vocab_cache) {
        LOG_INFO("VocabCache stats",
                 K(g_vocab_cache->size(tenant_id_)),
                 K(g_vocab_cache->get_hit_cnt(tenant_id_)),
                 K(g_vocab_cache->get_miss_cnt(tenant_id_)),
                 K(g_vocab_cache->get_hit_rate(tenant_id_)));
    }
    if (g_posting_cache) {
        LOG_INFO("PostingCache stats",
                 K(g_posting_cache->size(tenant_id_)),
                 K(g_posting_cache->get_hit_cnt(tenant_id_)),
                 K(g_posting_cache->get_miss_cnt(tenant_id_)),
                 K(g_posting_cache->get_hit_rate(tenant_id_)));
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

### 8.4 Hash 冲突
- VocabCache 的 key 使用 `token_hash`，可能有冲突
- 获取到 value 后需要验证 `token_str` 是否匹配
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

## 10. 相关文件

- `src/sql/das/ob_fts_bm25s_cache.h` - KVCache Key/Value 定义（新建）
- `src/sql/das/ob_fts_bm25s_index.h` - 类定义修改
- `src/sql/das/ob_fts_bm25s_index.cpp` - 实现修改
- `src/share/cache/ob_kv_storecache.h` - KVCache 接口参考
