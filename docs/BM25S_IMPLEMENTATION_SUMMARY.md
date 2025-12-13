# BM25S 索引实现总结

## 1. 实现概述

本文档总结 BM25S 索引三阶段设计的实现状态，并提出后续优化建议。

### 1.1 三阶段设计目标

| 阶段 | 目标 | 核心功能 |
|-----|------|---------|
| Phase 1 | TmpFile 分桶 | 使用 SeekDB 原生 TmpFile 接口分桶存储原始数据 |
| Phase 2 | 持久化落盘 | 流式构建索引，直接写入持久化文件 |
| Phase 3 | KVCache 查询 | 按需加载 posting 数据，支持 LRU 淘汰 |

### 1.2 当前实现状态

```
✅ Phase 1: TmpFile 分桶        - 已完成
✅ Phase 2: 持久化落盘          - 已完成（流式持久化）
✅ Phase 3: KVCache 查询        - 已完成
```

---

## 2. Phase 1: TmpFile 分桶 ✅

### 2.1 实现内容

| 功能 | 状态 | 说明 |
|-----|------|------|
| `init_tmp_files()` | ✅ | 初始化 1024 个 TmpFile 桶 |
| `write_to_bucket()` | ✅ | 写入单条记录到指定桶 |
| `write_records_to_bucket()` | ✅ | 批量写入优化 |
| `read_bucket()` | ✅ | 读取整个桶的所有记录 |
| `cleanup_tmp_files()` | ✅ | 清理所有 TmpFile |
| 桶锁保护 | ✅ | `bucket_locks_[]` 保护并发写入 |
| 双重检查锁定 | ✅ | 延迟初始化 TmpFile |

### 2.2 关键实现

```cpp
// 固定 1024 个桶
static const int64_t NUM_BUCKETS = 1024;

// TmpFile 文件描述符数组
int64_t bucket_fds_[NUM_BUCKETS];

// 桶锁保护并发写入
common::ObSpinLock bucket_locks_[NUM_BUCKETS];
```

### 2.3 数据流

```
collect_raw_data(token, doc_id, tf, doc_len)
        │
        ▼
┌─────────────────────────────────┐
│ 1. 确保 TmpFile 已初始化         │
│ 2. 分配/获取 token_id            │
│ 3. 计算 bucket_id = token_id % 1024 │
│ 4. 使用桶锁保护写入               │
└─────────────────────────────────┘
        │
        ▼
    TmpFile[bucket_id]
```

---

## 3. Phase 2: 持久化落盘 ✅

### 3.1 实现内容

| 功能 | 状态 | 说明 |
|-----|------|------|
| `finalize_and_persist()` | ✅ | 流式 Finalize + 持久化主入口 |
| `write_header_and_directory()` | ✅ | 写入 Header 和 Bucket Directory |
| `write_vocab_buckets_streaming()` | ✅ | 流式写入词汇表分桶 |
| `update_header()` | ✅ | 更新 Header 和 Bucket Directory |
| 两次遍历策略 | ✅ | 第一遍统计 df，第二遍计算 BM25 并写入 |
| 桶数统一 | ✅ | TmpFile 和持久化均使用 1024 个桶 |

### 3.2 文件格式

```
+============================================+
|              BM25S Index File              |
+============================================+
| Section 1: Header (256 bytes)              |
+--------------------------------------------+
| Section 2: Bucket Directory (16 KB)        |
|   1024 桶 × 16 bytes = 16 KB               |
+--------------------------------------------+
| Section 3: Vocab Buckets (变长)            |
|   token_hash + token_str + posting_info    |
+--------------------------------------------+
| Section 4: Posting Data (变长)             |
|   doc_ids[] + scores[]                     |
+============================================+
```

### 3.3 流式持久化流程

```
finalize_and_persist()
        │
        ▼
┌─────────────────────────────────────────────┐
│ 1. 第一次遍历所有桶：统计 df（不排序）         │
│ 2. 计算 posting_offsets 和各区域偏移          │
│ 3. 写入 Header + Bucket Directory 占位       │
│ 4. 写入 Vocab Buckets（流式）                 │
│ 5. 第二次遍历所有桶：排序 + 计算 BM25 + 写入   │
│ 6. 更新 Header + Bucket Directory            │
│ 7. 清理 TmpFile                              │
└─────────────────────────────────────────────┘
```

### 3.4 内存优化

| 方案 | 内存峰值 |
|-----|---------|
| 原方案（内存 CSC） | ~800 MB（1亿 posting） |
| 新方案（流式持久化） | ~200 MB（单桶 + 元数据） |

**优化效果**：内存使用减少 ~75%

---

## 4. Phase 3: KVCache 查询 ✅

### 4.1 实现内容

| 功能 | 状态 | 说明 |
|-----|------|------|
| `ObBM25SPostingKey` | ✅ | KVCache Key（使用 token 字符串） |
| `ObBM25SPostingValue` | ✅ | KVCache Value（doc_ids + scores） |
| `init_bm25s_cache()` | ✅ | 初始化全局 PostingCache |
| `get_posting_from_cache()` | ✅ | 缓存优先获取 posting |
| `load_posting_to_cache()` | ✅ | 从文件加载并放入缓存 |
| `find_token_in_file()` | ✅ | 从持久化文件查找 token |
| `load_from_disk()` | ✅ | 加载索引元数据（重启恢复） |
| `ensure_ready()` | ✅ | 确保索引就绪（带锁保护） |
| `search_with_cache()` | ✅ | 完整搜索流程 |
| `get_valid_count()` | ✅ | id_limit 二分查找过滤 |

### 4.2 缓存架构

```
┌─────────────────────────────────────────────────────────────┐
│ 常驻内存（启动/持久化后加载）                                  │
│   - file_header_: 元数据                                    │
│   - bucket_directory_: 桶目录（1024 × 16 bytes）             │
│   - persist_fd_: 文件描述符                                  │
│   - token_to_id_: token → token_id 映射                     │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│ PostingCache (ObKVCache)                                    │
│   Key:   token 字符串                                       │
│   Value: (count, doc_ids[], scores[])                       │
│   淘汰:  LRU 自动淘汰                                        │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│ 持久化文件 (bm25s_index.dat)                                 │
│   按需读取：缓存未命中时从文件加载                             │
└─────────────────────────────────────────────────────────────┘
```

### 4.3 查询流程

```
do_bm25s_search(query_tokens)
        │
        ▼
┌─────────────────────────────────────────────┐
│ for each token:                             │
│   1. get_posting_from_cache(token)          │
│      ├─ 缓存命中 → 直接使用                  │
│      └─ 缓存未命中 → load_posting_to_cache  │
│   2. get_valid_count(id_limit) 二分过滤      │
│   3. 累加文档得分                            │
│ end for                                     │
│                                             │
│ 4. Top-K 排序输出                            │
└─────────────────────────────────────────────┘
```

---

## 5. 关键修复记录

### 5.1 已修复问题

| 问题 | 原因 | 修复方案 |
|-----|------|---------|
| `token_to_id_.create()` 返回 `-4002` | 多线程并发调用 `load_from_disk()` | `ensure_ready()` 添加 double-checked locking |
| 查询时 token 找不到 | `write_header_and_directory()` 重置了 `file_header_` | 删除 `file_header_.reset()` 调用 |
| 桶数不一致 | TmpFile 用 1024，持久化用 4096 | 统一使用 `NUM_BUCKETS = 1024` |
| Hash 函数不一致 | 写入用 `murmurhash`，查询用 `token.hash()` | 统一使用 `token.hash()` |
| 持久化后查询失败 | 文件未重新打开，`persist_file_opened_` 未设置 | 持久化后重新打开文件并设置标志 |

### 5.2 线程安全改进

```cpp
// ensure_ready() - Double-checked locking
int ObFtsBM25SIndex::ensure_ready()
{
  // 先无锁检查
  if (persist_file_opened_) {
    return OB_SUCCESS;
  }
  
  // 加锁再检查
  SpinWLockGuard guard(lock_);
  if (persist_file_opened_) {
    return OB_SUCCESS;  // 其他线程已完成加载
  }
  
  // 执行加载
  return load_from_disk(default_index_dir);
}
```

---

## 6. 性能特性

### 6.1 已实现优化

| 优化项 | 状态 | 效果 |
|-------|------|------|
| 流式持久化 | ✅ | 内存峰值从 ~800MB 降至 ~200MB |
| KVCache 缓存 | ✅ | 热数据 O(1) 访问 |
| id_limit 二分查找 | ✅ | 过滤时间 O(log n) |
| 桶内 hash 排序 | ✅ | 查找可提前退出 |
| 批量写入 TmpFile | ✅ | 减少 I/O 次数 |

### 6.2 当前内存占用分析

| 组件 | 大小估算 |
|-----|---------|
| `file_header_` | 256 bytes |
| `bucket_directory_` (1024 桶) | 16 KB |
| `token_to_id_` (10 万词) | ~10 MB |
| 单桶 TmpFile 读取 | ~100-200 MB |
| KVCache（热数据） | 由系统 LRU 管理 |
| **总计峰值** | **~220 MB** |

---

## 7. 待改进内容

### 7.1 高优先级改进

#### 7.1.1 ⭐ 缓存预热（Warmup）

**现状**：首次查询需要从磁盘加载，冷启动性能较差

**改进方案**：
```cpp
// 启动时预热高频词汇
int ObFtsBM25SIndex::warmup_cache(int64_t max_entries)
{
  // 1. 按 posting_count 降序排列 token
  // 2. 加载 Top-N 高频 token 到 KVCache
  // 3. 可配置预热数量
}
```

**收益**：提升冷启动查询性能 50%+

#### 7.1.2 ⭐ 并行预加载（Prefetch）

**现状**：多 token 查询串行加载

**改进方案**：
```cpp
// 并行预加载多个 token
int ObFtsBM25SIndex::prefetch_postings(
    const ObIArray<ObString> &tokens)
{
  // 使用线程池并行加载
  // 减少串行 I/O 等待
}
```

**收益**：多 token 查询性能提升 30-50%

#### 7.1.3 ⭐ 缓存统计与监控

**现状**：无法观测缓存命中率

**改进方案**：
```cpp
void ObFtsBM25SIndex::print_cache_stats()
{
  LOG_INFO("BM25S Cache Stats",
           "hit_rate", g_posting_cache->get_hit_rate(),
           "size", g_posting_cache->size(),
           "evictions", g_posting_cache->get_evict_cnt());
}
```

**收益**：便于性能分析和调优

---

### 7.2 中优先级改进

#### 7.2.1 按需精确加载（id_limit 优化）

**现状**：缓存完整 posting，内存中二分过滤

**改进方案**：
```cpp
// 只加载 doc_id < id_limit 的部分
int load_posting_with_limit(
    const ObString &token,
    int64_t id_limit,
    const ObBM25SPostingValue *&posting,
    ObKVCacheHandle &handle)
{
  // 1. 在文件中二分查找确定加载范围
  // 2. 只读取需要的部分
  // 3. 减少内存占用和 I/O
}
```

**收益**：对于大 posting list，内存减少 80%+

#### 7.2.2 外部排序（超大桶处理）

**现状**：单桶数据必须全部载入内存排序

**改进方案**：
```cpp
// 单桶过大时使用外部排序
int finalize_bucket_with_external_sort(int64_t bucket_id)
{
  if (bucket_size > MAX_MEMORY) {
    // Phase 1: 分批排序写入临时文件
    // Phase 2: K 路归并写入最终文件
  }
}
```

**收益**：支持超大规模数据（10 亿+ 文档）

#### 7.2.3 压缩存储

**现状**：doc_ids 和 scores 无压缩

**改进方案**：
- doc_ids：差分编码 + VarInt 压缩
- scores：量化为 int16 或 int8

**收益**：文件大小减少 50%+，I/O 性能提升

---

### 7.3 低优先级改进

#### 7.3.1 多租户隔离

**现状**：全局单例，使用 `OB_SYS_TENANT_ID`

**改进方案**：
```cpp
// Key 添加 tenant_id
class ObBM25SPostingKey {
  uint64_t tenant_id_;
  uint64_t table_id_;
  ObString token_;
};
```

#### 7.3.2 增量更新

**现状**：不支持增量更新，需要重建索引

**改进方案**：
- 维护 delta 索引
- 定期合并到主索引
- 支持软删除标记

#### 7.3.3 原子性写入

**现状**：写入过程中断可能导致文件损坏

**改进方案**：
```cpp
// 使用临时文件 + rename 保证原子性
write_to_temp_file();
fsync(temp_file);
rename(temp_file, final_file);
```

---

## 8. 测试清单

### 8.1 已验证功能

| 测试项 | 状态 |
|-------|------|
| 基本查询流程 | ✅ |
| 重启后查询（无需重建索引） | ✅ |
| KVCache 缓存命中 | ✅ |
| id_limit 过滤 | ✅ |
| 多线程并发查询 | ✅ |

### 8.2 建议补充测试

| 测试项 | 说明 |
|-------|------|
| 大数据量压测 | 1000 万+ 文档 |
| 缓存淘汰验证 | 验证 LRU 正确性 |
| 磁盘故障恢复 | 文件损坏处理 |
| 内存压力测试 | 低内存环境 |

---

## 9. 总结

### 9.1 实现完成度

| 阶段 | 完成度 | 说明 |
|-----|-------|------|
| Phase 1: TmpFile 分桶 | **100%** | 基础功能完整 |
| Phase 2: 持久化落盘 | **100%** | 流式持久化已实现 |
| Phase 3: KVCache 查询 | **100%** | 缓存查询已实现 |
| 整体完成度 | **100%** | 核心功能完整可用 |

### 9.2 主要成就

1. **内存优化**：从全内存 CSC 结构改为流式持久化 + 按需加载，内存峰值降低 75%
2. **重启恢复**：支持集群重启后直接查询，无需重建索引
3. **缓存机制**：KVCache 自动管理热数据，LRU 淘汰冷数据
4. **线程安全**：关键路径添加锁保护，支持并发查询

### 9.3 推荐下一步

1. **短期**：添加缓存预热和统计监控
2. **中期**：实现 id_limit 精确加载优化
3. **长期**：支持压缩存储和增量更新

---

*文档生成时间：2025-12-13*
*版本：1.0*

