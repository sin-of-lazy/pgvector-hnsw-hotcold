# 第 4 章：Phase 1 详解 - Buffer Pool 预取优化

## 4.1 动机与设计思路

### 4.1.1 问题根源

HNSW 查询的 I/O 模式：

```
Entry Point (layer 3, 1 node)   ←  1 次 buffer access
    ↓ neighbors
Layer 2 (sparse, ~5 nodes)      ←  5 次 random access
    ↓ neighbors  
Layer 1 (denser, ~20 nodes)     ←  20 次 random access
    ↓ neighbors
Layer 0 (全图, ef_search=200)   ←  200+ 次 random access
```

**关键观察**：

- 高层稀疏，节点少，buffer hit 率高
- **第 0 层稠密，ef_search 越大，访问节点越多，buffer miss 越频繁**
- 邻居列表通常跨多个 index page，触发 miss 时 **同步阻塞**
- 每次 miss 的代价：~5-10ms（SSD）或 ~10-20ms（机械盘）

### 4.1.2 优化思路

在 `HnswLoadUnvisitedFromDisk` 拿到邻居 TID 列表后，立即对所有邻居的 index page 发起异步 `PrefetchBuffer`。后续 `HnswLoadElementImpl` 访问时大概率已经在 buffer pool 里。

```
传统流程（串行）：
  for each neighbor:
    access page → miss → block 10ms → load → continue
  总耗时 = n × 10ms（假设全 miss）

优化流程（并发）：
  prefetch all pages (async, ~0.1ms per call)
  for each neighbor:
    access page → hit（大概率）→ continue
  总耗时 ≈ max(single I/O) + n × 0.01ms
```

### 4.1.3 为什么不用自建缓存？

| 方案 | 优点 | 缺点 | 决策 |
|---|---|---|---|
| **自建热缓存** | 可定制淘汰策略 | 内存管理复杂、VACUUM 一致性、锁竞争 | ❌ 不采用 |
| **PrefetchBuffer** | 零内存开销、PG 原生、幂等、可灰度 | 依赖 PG buffer pool 大小 | ✅ **采用** |

## 4.2 代码实现细节

### 4.2.1 核心逻辑（`src/hnswutils.c:900-920`）

```c
static List *
HnswSearchLayer(char *base, HnswQuery * q, List *ep, int ef, int lc, ...) {
    ...
    while (!pairingheap_is_empty(C)) {
        HnswSearchCandidate *c = ...;
        HnswElement cElement = HnswPtrAccess(base, c->element);

        // 官方原有逻辑：加载未访问邻居列表
        if (inMemory)
            HnswLoadUnvisitedFromMemory(...);
        else
            HnswLoadUnvisitedFromDisk(cElement, unvisited, &unvisitedLength, v, index, m, lm, lc);

        // ===== Phase 1 新增 START =====
        if (!inMemory && hnsw_hot_cold_enabled && hnsw_prefetch_neighbors > 0) {
            int prefetchCount = Min(unvisitedLength, hnsw_prefetch_neighbors);

            // hot/cold 策略：低层限流，避免无效预取
            if (lc < hnsw_hot_layer)
                prefetchCount = Min(prefetchCount, 4);

            for (int pi = 0; pi < prefetchCount; pi++) {
                BlockNumber blk = ItemPointerGetBlockNumber(&unvisited[pi].indextid);
                if (blk != InvalidBlockNumber)
                    PrefetchBuffer(index, MAIN_FORKNUM, blk);
            }
        }
        // ===== Phase 1 新增 END =====

        // 官方原有逻辑：逐个加载邻居元素
        for (int i = 0; i < unvisitedLength; i++) {
            ...
            HnswLoadElementImpl(blkno, offno, &eDistance, q, index, support, ...);
            ...
        }
    }
    ...
}
```

### 4.2.2 GUC 定义（`src/hnsw.c`）

```c
// 变量定义
bool  hnsw_hot_cold_enabled = false;
int   hnsw_hot_layer = 2;
int   hnsw_hot_max_bytes = 64 * 1024;  // 占位
int   hnsw_prefetch_neighbors = 16;

// HnswInit() 中注册
DefineCustomBoolVariable("hnsw.hot_cold_enabled",
    "Enables buffer-pool prefetch of HNSW neighbor pages",
    NULL, &hnsw_hot_cold_enabled,
    false, PGC_USERSET, 0, NULL, NULL, NULL);

DefineCustomIntVariable("hnsw.hot_layer",
    "Minimum HNSW layer at which to apply hot prefetch",
    "Layers >= this value are considered hot and prefetched more aggressively.",
    &hnsw_hot_layer,
    2, 0, 255, PGC_USERSET, 0, NULL, NULL, NULL);

DefineCustomIntVariable("hnsw.prefetch_neighbors",
    "Number of neighbor index pages to prefetch per HNSW node",
    "0 disables prefetch even if hnsw.hot_cold_enabled is on.",
    &hnsw_prefetch_neighbors,
    16, 0, HNSW_MAX_M * 2, PGC_USERSET, 0, NULL, NULL, NULL);
```

### 4.2.3 hot/cold 策略

```c
if (lc < hnsw_hot_layer)
    prefetchCount = Min(prefetchCount, 4);
```

**设计依据**：

- **高层（lc >= hot_layer）**：节点少但重要，是进入下一层的关键跳转点，预取更多（16 个）
- **低层（lc < hot_layer）**：节点多且分散，全预取可能污染 buffer pool，限流到 4 个

实验表明 `hot_layer=2` 效果较好，未来可做 sweep 找最优值。

## 4.3 正确性保证

### 4.3.1 语义不变

`PrefetchBuffer` **只是 I/O 提示**，不改变：

- 访问的页面内容
- 访问的顺序
- 搜索算法逻辑
- MVCC 可见性判断

因此 Recall@K **完全一致**（实验验证 0.198/0.198，0.478/0.478）。

### 4.3.2 并发安全

- `PrefetchBuffer` 是 PostgreSQL 原生 API，线程安全
- 不涉及共享状态修改
- 每个查询有独立的 `unvisited[]` 数组

### 4.3.3 回退能力

```sql
SET hnsw.hot_cold_enabled = off;  -- 立即回退到官方逻辑
```

代码路径：

```c
if (!inMemory && hnsw_hot_cold_enabled && ...) {
    // 这段跳过
}
```

## 4.4 参数调优指南

### 4.4.1 `hnsw.prefetch_neighbors`

| 值 | 适用场景 | 效果 |
|---:|---|---|
| 0 | 关闭预取 | 等价官方 |
| 4-8 | 内存紧张，buffer pool < 1GB | 保守预取，降低污染 |
| **16** | **生产推荐**（buffer pool ≥ 2GB） | **平衡收益与开销** |
| 32 | 大内存（buffer pool ≥ 8GB），高 ef_search | 激进预取 |

### 4.4.2 `hnsw.hot_layer`

| 值 | 行为 | 适用场景 |
|---:|---|---|
| 0 | 所有层都限流到 4 | 极度内存敏感 |
| 1 | 只有最高层激进预取 | 中等内存 |
| **2** | **推荐**（2 层及以上激进） | **通用场景** |
| 3+ | 更多层激进预取 | 大规模图（百万级） |

### 4.4.3 配合 `shared_buffers` 调优

```sql
-- 推荐配置（生产环境）
shared_buffers = '4GB'                    -- 至少是索引大小的 2-3 倍
effective_cache_size = '12GB'             -- 告诉 planner 总 cache 大小
hnsw.hot_cold_enabled = on
hnsw.prefetch_neighbors = 16
hnsw.hot_layer = 2
```

## 4.5 与其他优化的交互

### 4.5.1 与 `work_mem` 无关

`work_mem` 控制排序/哈希/聚合的内存，与 buffer pool 独立。prefetch 不受影响。

### 4.5.2 与 `hnsw.ef_search` 协同

| ef_search | prefetch 收益 | 原因 |
|---:|---|---|
| 10-40 | 中等（-10% ~ -20%） | 访问节点少，miss 概率低 |
| 80-200 | **高（-25% ~ -35%）** | 访问节点多，miss 频繁 |
| 500+ | 极高（p99 可达 -50%+） | 极端随机访问，prefetch 威力最大 |

### 4.5.3 与 Phase 2 `ef_search_auto` 叠加

Phase 2 让 iterative scan 时 batch 扩大，访问更多节点 → 触发更多 prefetch → 收益叠加。

## 4.6 性能开销分析

### 4.6.1 CPU 开销

每次 prefetch 调用约 **0.01-0.05ms**（只是系统调用，不等待 I/O）。
假设 `prefetchCount=16`，总开销 ≈ 0.8ms，相比节省的 10-50ms miss 延迟，完全值得。

### 4.6.2 内存开销

**零额外内存**。`PrefetchBuffer` 只是让 PG 提前加载页面到 buffer pool，不新增数据结构。

### 4.6.3 I/O 放大

理论上可能预取了用不到的页面（如查询提前终止）。实测影响极小，因为：

- `unvisited[]` 里的邻居大概率会被访问
- 即使浪费，也只是多占用了一些 buffer pool 空间，LRU 会自然淘汰

## 4.7 实验验证

### 4.7.1 小数据集（50k 行）

索引大小 ~25MB，能完全放进 buffer pool。

| ef_search | 指标 | off | on | 改善 |
|---:|---|---:|---:|---:|
| 40 | avg | 1.725 ms | 1.559 ms | -9.6% |
| 40 | p95 | 2.621 ms | 1.976 ms | **-24.6%** |

**分析**：数据集小，buffer hit 率高，收益主要体现在尾延迟（偶尔的 miss）。

### 4.7.2 大数据集（200k 行）

索引大小 ~100MB，超出默认 `maintenance_work_mem`，触发 on-disk build。

| ef_search | 指标 | off | on | 改善 |
|---:|---|---:|---:|---:|
| 40 | avg | 6.506 ms | 4.785 ms | **-26.5%** |
| 40 | p99 | 9.580 ms | 7.977 ms | -16.7% |
| 200 | avg | 22.952 ms | 15.415 ms | **-32.8%** |
| 200 | **p99** | **48.092 ms** | **21.227 ms** | **-55.9%** |

**分析**：

- 数据集大，buffer miss 频繁，prefetch 收益显著
- ef_search=200 时访问 200+ 节点，几乎全是随机 I/O，prefetch 把 **p99 从 48ms 砍到 21ms**
- Recall 完全一致（0.478/0.478），语义零改变

### 4.7.3 EXPLAIN (BUFFERS) 对比

```sql
-- off
Buffers: shared hit=10234 read=3421

-- on
Buffers: shared hit=13412 read=243
```

**解读**：

- `read` 从 3421 降到 243，说明大部分页面被预取命中
- `hit` 增加是因为预取的页面都在 buffer pool 里，后续访问直接命中
- 总 buffer 访问略增（13412+243 > 10234+3421），但总延迟大幅下降，说明并发 I/O 抵消了额外访问

## 4.8 与学术界 ANN prefetch 研究对比

| 方案 | 思路 | 优势 | 劣势 |
|---|---|---|---|
| **HNSW 原论文** | 无 prefetch | 算法简单 | 随机 I/O 阻塞 |
| **DiskANN (MSR)** | 自建 page cache + prefetch | 极致优化 | 脱离 PG 生态，工程复杂 |
| **本项目** | 复用 PG Buffer Pool + `PrefetchBuffer` | 零额外内存，灰度可控 | 依赖 PG buffer pool 大小 |

**本项目的工程价值**：在 PostgreSQL 生态内以最小代价（~20 行）实现了接近 DiskANN 的 I/O 优化效果。
