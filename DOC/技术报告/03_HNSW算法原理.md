# 第 3 章：HNSW 算法原理与 pgvector 实现

## 3.1 HNSW 算法概述

**HNSW (Hierarchical Navigable Small World)** 是 Malkov & Yashunin 2016 提出的近似最近邻算法，兼顾**高召回率**和**低查询延迟**，是当前 ANN（Approximate Nearest Neighbor）搜索的事实标准之一。

### 3.1.1 核心思想

HNSW 构建一个**多层图**：

- **底层（Layer 0）**：包含所有向量节点，邻居数最多（2m）
- **上层（Layer 1, 2, ...）**：随机采样部分节点，邻居数减少（m）
- **最高层**：只有少数节点，用作查询入口

查询时从最高层入口开始，**逐层贪心下降**，最后在 Layer 0 做 beam search 找到近邻。

### 3.1.2 关键参数

| 参数 | 含义 | 影响 |
|---|---|---|
| **m** | 每层邻居数上限（Layer 0 是 2m） | 越大召回越高，索引越大 |
| **ef_construction** | 构建时 beam 宽度 | 越大召回越高，构建越慢 |
| **ef_search** | 查询时 beam 宽度 | 越大召回越高，查询越慢 |

### 3.1.3 时间复杂度

- **构建**：O(N × log N × ef_construction)
- **查询**：O(log N × ef_search)

对于 100 万向量，128 维，`m=16, ef_construction=64, ef_search=40`：
- 构建约 5-10 分钟
- 单次查询约 1-5 毫秒

## 3.2 pgvector 中的 HNSW 实现

### 3.2.1 数据结构

**核心结构体**（`src/hnsw.h`）：

```c
struct HnswElementData {
    HnswElementPtr next;              // 链表指针（用于遍历所有元素）
    ItemPointerData heaptids[10];     // 回表 TID（支持 HOT 更新）
    uint8       heaptidsLength;
    uint8       level;                // 最高层数
    uint8       deleted;              // VACUUM 标记
    uint8       version;
    uint32      hash;                 // 用于哈希查找
    HnswNeighborsPtr neighbors;       // 邻居数组指针
    BlockNumber blkno;                // 元素所在 index page
    OffsetNumber offno;               // 元素在 page 内的偏移
    OffsetNumber neighborOffno;
    BlockNumber neighborPage;
    DatumPtr    value;                // 向量数据
    LWLock      lock;
};
```

### 3.2.2 磁盘布局

```
Meta Page (block 0)
├── magic, version
├── m, ef_construction
├── entryBlkno, entryOffno    ← 查询入口
├── entryLevel
└── insertPage

Element Pages (block 1+)
├── HnswPageOpaque (nextblkno + page_id)
└── HnswElementTupleData[]
    ├── heaptids[10]
    ├── level, deleted, version
    ├── neighbortid            ← 指向 neighbor tuple
    └── data (vector)

Neighbor Pages (interleaved)
└── HnswNeighborTupleData[]
    ├── type, version, count
    └── indextids[]           ← 邻居的 (blkno, offno)
```

### 3.2.3 核心搜索函数

**`HnswSearchLayer`** (`src/hnswutils.c:823`) 是查询主循环，实现 Algorithm 2：

```
输入：查询向量 q, 入口点 ep, beam 宽度 ef, 层号 lc
输出：距离最近的 ef 个候选

初始化：
  C ← ep (候选堆，按距离升序)
  W ← ep (结果堆，按距离降序)
  visited ← ep

while C 非空:
  c ← C 中最近的候选
  f ← W 中最远的元素
  if dist(c, q) > dist(f, q): break  // 提前终止
  
  for each neighbor n of c (unvisited):    ← Phase 1 在这里加 prefetch
    visited ← visited ∪ {n}
    if dist(n, q) < dist(f, q) or |W| < ef:
      C ← C ∪ {n}
      W ← W ∪ {n}
      if |W| > ef: W 移除最远元素

return W
```

### 3.2.4 多层下降

**`GetScanItems`** (`src/hnswscan.c:26`)：

```c
// 从最高层贪心下降到 Layer 1
for (int lc = entryPoint->level; lc >= 1; lc--) {
    w = HnswSearchLayer(base, q, ep, /* ef= */ 1, lc, ...);
    ep = w;
}

// Layer 0 做 beam search
return HnswSearchLayer(base, q, ep, hnsw_ef_search, /* lc= */ 0, ...);
```

**关键**：

- 上层用 `ef=1` 只为快速导航
- Layer 0 用 `ef=hnsw_ef_search`（用户可配）做真正搜索
- Phase 1 的 prefetch **同时作用于所有层**

## 3.3 pgvector HNSW 的独特设计

### 3.3.1 heaptids[10]：支持 HOT 更新

传统 HNSW 每个节点对应一个向量。pgvector 允许一个节点关联多个 heap TID（HOT-updated tuples），减少索引膨胀。

### 3.3.2 in-memory / on-disk 双模式

- **构建期 in-memory**：如果 `maintenance_work_mem` 足够，全图在内存构建，最后 flush
- **构建期 on-disk**：内存不足时切换到磁盘模式，逐节点 flush
- **查询期永远 on-disk**：从 buffer pool 读取

Phase 1 优化针对**查询期的 on-disk 路径**。

### 3.3.3 支持多种向量类型

- `vector`：float32 稠密向量
- `halfvec`：float16 稠密向量（节省一半内存）
- `bit`：位向量（Hamming 距离）
- `sparsevec`：稀疏向量

HNSW 索引支持所有类型，通过 `opclass` 区分（如 `vector_l2_ops`, `halfvec_l2_ops`）。

### 3.3.4 Iterative Scan（关键机制）

**问题**：`ORDER BY <-> LIMIT k WHERE filter` 时，如果 filter 过滤掉大量候选，`ef_search` 可能不够。

**pgvector 0.8+ 引入 `hnsw.iterative_scan`**：

- `off`：官方默认行为，返回 `ef_search` 个候选后就停
- `relaxed_order`：从 `discarded` 堆继续 resume 搜索，可能打乱严格顺序
- `strict_order`：resume 时保证距离单调递增

**Phase 2 优化点**：`ResumeScanItems` 中按倍率扩大 batch。

## 3.4 SQL 层到 C 层的映射

### 3.4.1 操作符注册

`sql/vector.sql` 中：

```sql
CREATE OPERATOR <-> (
    LEFTARG = vector, RIGHTARG = vector,
    PROCEDURE = l2_distance,
    COMMUTATOR = '<->'
);

CREATE OPERATOR CLASS vector_l2_ops
    DEFAULT FOR TYPE vector USING hnsw AS
    OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_l2_squared_distance(vector, vector),
    FUNCTION 3 l2_normalize(vector);
```

**关键**：

- `OPERATOR 1 <-> ... FOR ORDER BY`：告诉 planner 这个操作符可以用于 ORDER BY 走索引
- `FUNCTION 1`：距离计算函数（HNSW 内部用）
- `FUNCTION 3`：归一化函数（用于 cosine 距离预处理）

### 3.4.2 Planner 如何选中 HNSW

当查询是：
```sql
SELECT ... FROM t ORDER BY embedding <-> '[...]' LIMIT 10;
```

Planner 流程：
1. 识别 `ORDER BY` 有距离操作符 `<->`
2. 查 `pg_amop` 找到支持 `<->` 的索引类型（`hnsw`, `ivfflat`）
3. 检查表上是否有匹配的索引
4. 调用 `hnswcostestimate` 估算代价
5. 如果 HNSW 代价最低，生成 Index Scan 计划

### 3.4.3 Index AM handler 注册

`hnsw.c` 中：

```c
Datum hnswhandler(PG_FUNCTION_ARGS) {
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);
    
    amroutine->amcanorderbyop = true;   // 关键：支持 ORDER BY operator
    amroutine->ambuild = hnswbuild;
    amroutine->aminsert = hnswinsert;
    amroutine->ambeginscan = hnswbeginscan;
    amroutine->amgettuple = hnswgettuple;
    // ... 更多回调
    
    PG_RETURN_POINTER(amroutine);
}
```

`amcanorderbyop = true` 是 HNSW 能被 `ORDER BY` 使用的关键。

## 3.5 HNSW 的 Recall-Latency 权衡

### 3.5.1 参数如何影响权衡

```
Recall ↑    m ↑, ef_construction ↑, ef_search ↑
Latency ↓   m ↓, ef_search ↓
Index Size ↑ m ↑
```

### 3.5.2 典型场景推荐

| 场景 | m | ef_construction | ef_search | Recall@10 | avg latency |
|---|---:|---:|---:|---:|---:|
| **高吞吐低召回** | 8 | 32 | 20 | ~0.85 | ~1 ms |
| **平衡（推荐）** | 16 | 64 | 40 | ~0.95 | ~3 ms |
| **高召回** | 32 | 128 | 100 | ~0.99 | ~10 ms |
| **极高召回** | 64 | 200 | 500 | ~0.999 | ~50 ms |

### 3.5.3 本项目优化的作用点

- Phase 1 **不改变 recall**，只降低 latency（尤其 p99）
- 相当于在同一 Recall 曲线上向左移动（更快达到相同 Recall）

## 3.6 与其他 ANN 算法对比

| 算法 | 结构 | Build 速度 | Query 速度 | Recall | 内存 |
|---|---|---|---|---|---|
| **HNSW** | 多层图 | 慢 | 极快 | 高 | 大 |
| **IVFFlat** | 倒排表 | 快 | 快 | 中 | 中 |
| **LSH** | 哈希 | 极快 | 快 | 低 | 小 |
| **DiskANN** | 单层图 + PQ | 中 | 快 | 高 | 小（PQ 压缩） |
| **ScaNN** | 树 + PQ | 快 | 极快 | 中高 | 中 |

**HNSW 的优势**：query 速度快、Recall 高；**劣势**：构建慢、内存占用大。

pgvector 同时支持 HNSW 和 IVFFlat，本项目专注 HNSW（因为 RAG 场景对 Recall 更敏感）。
