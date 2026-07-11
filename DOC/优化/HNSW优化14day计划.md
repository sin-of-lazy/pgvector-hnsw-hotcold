# pgvector HNSW 索引优化 14-Day 实战计划

> 每日投入：3 小时 | 总计：42 小时  
> 背景假设：已完成 CMU 15-445、读过 pgvector 全部数据类型源码  
> 目标：产出可量化的性能提升数据，作为简历技术亮点

---

## 一、优化点总结

| # | 优化方向 | 所属维度 | 核心技术 | 预期收益 |
|---|----------|----------|----------|----------|
| A | 搜索候选队列邻居预取 + SIMD 距离加速 | 查询速度 | PrefetchBuffer、AVX2 | 查询延迟降低 20~35% |
| B | 基于召回反馈的自适应 ef_search | 查询速度 | GUC 动态覆盖、统计采样 | QPS 提升 15~25% |
| C | 并行 Build per-node LWLock 细粒度锁 | 构建速度 | LWLock Array、Optimistic Retry | 并行 Build 速度提升 30~50% |
| D | 向量 SQ8 量化压缩（int8 存储） | 内存占用 | Scalar Quantization、定点运算 | 内存降低 55~70% |
| E | HNSW 上层节点冷热分层（Buffer 感知） | 内存占用/查询 | PinBuffer、Page 局部性 | Cache Miss 降低 40~60% |

---

## 二、各优化点详细实现思路

### A. 搜索候选队列邻居预取 + SIMD 距离加速

**问题根因**

`HnswSearchLayer()` 的核心循环：从 candidates 最小堆 pop 节点 → 读取其邻居列表 → 对每个邻居计算距离 → 入堆。  
邻居节点的向量数据分散在不同 BufferPage，每次访问均可能触发 I/O 等待，且距离计算是纯标量循环。

**实现步骤**

```c
// 伪代码：在 HnswSearchLayer 的邻居遍历循环中
while (candidates 不为空) {
    HnswCandidate *cur = heap_pop(candidates);
    HnswElement neighbors = GetNeighbors(cur);

    // Step 1: 对后续 K 个邻居的 Page 发起异步预取
    for (int i = 0; i < MIN(neighbors->count, PREFETCH_LOOKAHEAD); i++) {
        BlockNumber blk = ItemPointerGetBlockNumber(&neighbors->items[i].heaptid);
        PrefetchBuffer(index, MAIN_FORKNUM, blk);
    }

    // Step 2: 遍历邻居时，向量已在 OS Page Cache 中，减少等待
    for (int i = 0; i < neighbors->count; i++) {
        float dist = HnswGetDistance_SIMD(query_vec, neighbor_vec, dim);
        // SIMD 路径：使用 AVX2 _mm256_fmadd_ps 展开 8-wide 内积
        ...
    }
}
```

**SIMD 加速要点**

- 针对 L2 距离：展开为 `sum += (a[i]-b[i])^2`，用 `_mm256_sub_ps` + `_mm256_mul_ps` + `_mm256_add_ps`
- 编译时通过 `pg_config --cflags` 确认 `-mavx2` 是否开启，必要时在 `Makefile` 中追加
- 维度不是 8 的倍数时用标量路径收尾

**关键文件**：`src/hnsw/hnsw.c`、`src/hnsw/hnswscan.c`、新建 `src/hnsw/simd_distance.h`

---

### B. 基于召回反馈的自适应 ef_search

**问题根因**

当前 `ef_search` 是静态 GUC，用户需手动调整。低 ef 时召回率差，高 ef 时查询慢——对不同查询难度缺乏自适应能力。

**实现思路**

维护一个轻量级的**滑动窗口统计器**，记录最近 N 次查询的"候选集耗尽时剩余距离差"作为召回压力信号，动态调整 per-session 的 ef 值。

```c
// 在 HnswScanOpaqueData 中新增：
typedef struct {
    int    ef_current;        // 当前动态 ef 值
    float  last_frontier_gap; // 上次查询：最优候选与截止候选的距离差
    int    consecutive_tight; // 连续"召回紧张"次数
} HnswAdaptiveEf;

// 查询结束后的反馈：
void HnswUpdateAdaptiveEf(HnswAdaptiveEf *state, float gap) {
    if (gap < TIGHT_THRESHOLD && state->consecutive_tight++ > 2)
        state->ef_current = MIN(state->ef_current * 1.5, MAX_EF);
    else if (gap > LOOSE_THRESHOLD)
        state->ef_current = MAX(state->ef_current * 0.85, so->base_ef);
}
```

**边界约束**：`ef_current >= limit`（pgvector 现有保护），`ef_current <= ef_search * 4`（防止失控）

**关键文件**：`src/hnsw/hnswscan.c`、`src/hnsw/hnsw.h`

---

### C. 并行 Build per-node LWLock 细粒度锁

**问题根因**

`HnswInsertElement()` 在图连接阶段，修改邻居列表需要对所在 Page 加 `LWLockAcquire(BufferDescGetContentLock(...), LW_EXCLUSIVE)`。  
多个 parallel worker 插入不同节点时，若邻居恰好在同一 Page，锁竞争严重，实测在 16 worker 时吞吐反而不如 4 worker。

**实现方案：LWLock Array + Optimistic Retry**

```c
// 1. 在 HNSW meta page 中分配 per-node lock array（类似 PG partition locks）
#define HNSW_NUM_NODE_LOCKS 256  // 取2的幂，用 node_id % 256 映射

typedef struct HnswMetaPageData {
    ...
    LWLock node_locks[HNSW_NUM_NODE_LOCKS]; // 新增
} HnswMetaPageData;

// 2. 插入时改用 per-node 粒度加锁
int lock_idx = element->neighborPage % HNSW_NUM_NODE_LOCKS;
LWLockAcquire(&meta->node_locks[lock_idx], LW_EXCLUSIVE);
// 修改邻居列表
LWLockRelease(&meta->node_locks[lock_idx]);

// 3. 若检测到 ABA（节点在加锁前已被其他 worker 修改），进行 Retry
// 通过在邻居列表头部写入 version counter 实现乐观检测
```

**注意事项**

- `LWLock` 在 PG 扩展中需通过 `RequestAddinShmemSpace` + `GetNamedLWLockTranche` 申请共享内存中的锁槽
- 锁槽数量 256 是 trade-off：太少退化为粗粒度，太多共享内存开销大
- 需修改 `HnswInitMetaPage()` 初始化锁数组

**关键文件**：`src/hnsw/hnswbuild.c`、`src/hnsw/hnsw.h`、`src/hnsw/hnswinsert.c`

---

### D. 向量 SQ8 量化压缩（int8 存储）

**问题根因**

每个 HNSW 节点在 index page 中存储完整的 float32 向量副本（用于图构建和距离计算），1536 维 OpenAI embedding 每个节点占 6KB，千万级数据集仅向量存储就需 60GB+。

**实现方案：Scalar Quantization to int8**

```c
// 训练阶段（build 时）：记录每个维度的 min/max
typedef struct SQ8Params {
    float min[MAX_DIM];
    float scale[MAX_DIM];  // scale[i] = 255.0 / (max[i] - min[i])
} SQ8Params;

// 量化编码（float32 → uint8）
void HnswEncodeVectorSQ8(float *src, uint8_t *dst, SQ8Params *params, int dim) {
    for (int i = 0; i < dim; i++) {
        float v = (src[i] - params->min[i]) * params->scale[i];
        dst[i] = (uint8_t) CLAMP(roundf(v), 0, 255);
    }
}

// 近似距离计算（int8 路径，可结合 SIMD）
float HnswApproxL2SQ8(uint8_t *a, uint8_t *b, SQ8Params *params, int dim) {
    int32_t sum = 0;
    for (int i = 0; i < dim; i++) {
        int32_t d = (int32_t)a[i] - (int32_t)b[i];
        sum += d * d;
    }
    // 反量化修正：乘以 scale^2（近似，忽略维度间 scale 差异）
    return sum * params->avg_scale_sq;
}
```

**与 pgvector halfvec 的区别**：`halfvec` 是用户暴露的 float16 类型，SQ8 是 index 内部存储优化，对用户透明，召回精度更可控。

**关键文件**：`src/hnsw/hnswbuild.c`、`src/hnsw/hnsw.h`，新建 `src/hnsw/sq8.c`

---

### E. HNSW 上层节点冷热分层（Buffer 感知）

**问题根因**

HNSW 分层结构中，layer ≥ 1 的上层节点数量约为 `总节点数 * (1/M)`（M 默认16），仅占总数的 6%，但每次查询都必须经过这些节点做导航。若这些 Page 被 PG Buffer Pool 淘汰，每次查询均需 I/O。

**实现方案**

```c
// 1. Build 完成后，遍历所有 layer >= 1 的节点，主动 Pin 其所在 Page
void HnswWarmUpperLayers(Relation index) {
    HnswElement entry = GetEntryPoint(index);
    // BFS 遍历所有 upper layer 节点
    List *upper_nodes = HnswCollectUpperLayerNodes(index, entry);

    foreach(lc, upper_nodes) {
        HnswElement elem = lfirst(lc);
        Buffer buf = ReadBufferExtended(index, MAIN_FORKNUM,
                                        elem->blkno, RBM_NORMAL,
                                        GetAccessStrategy(BAS_BULKREAD));
        // 标记为不优先淘汰（hint，非强制）
        MarkBufferDirtyHint(buf, false);
        ReleaseBuffer(buf);
    }
}

// 2. 在 index Page Layout 层面：Build 时把 upper layer 节点集中写入
//    index 的前 N 个 Page（提升空间局部性，对 OS readahead 友好）
```

**量化预期**：上层节点 Page 数 = `总节点数 / M / items_per_page`，以 100 万节点、M=16、每 Page 存 20 节点计算，约 3125 个 Page（25MB），完全可以常驻 Buffer Pool。

**关键文件**：`src/hnsw/hnswbuild.c`、`src/hnsw/hnsw.c`

---

## 三、Ubuntu 测试框架搭建

### 3.1 环境准备

```bash
# 系统依赖
sudo apt-get update
sudo apt-get install -y \
    build-essential git cmake \
    postgresql-16 postgresql-server-dev-16 \
    python3-pip python3-venv \
    linux-tools-common linux-tools-generic  # perf 工具

# Python 测试依赖
python3 -m venv ~/hnsw-bench
source ~/hnsw-bench/bin/activate
pip install psycopg2-binary numpy pgvector pandas matplotlib pytest
```

### 3.2 编译安装 pgvector（Debug + 优化版双版本）

```bash
# 克隆源码
git clone https://github.com/pgvector/pgvector.git
cd pgvector
git checkout v0.7.0  # 固定 baseline 版本

# 编译 baseline 版本
make PG_CONFIG=/usr/lib/postgresql/16/bin/pg_config
sudo make install PG_CONFIG=/usr/lib/postgresql/16/bin/pg_config

# 为自己的优化分支单独建目录
git checkout -b hnsw-optimize
# ... 修改代码后 ...
make clean && make PG_CONFIG=... OPTFLAGS="-O2 -mavx2"
```

### 3.3 测试数据集准备

```python
# scripts/gen_dataset.py
import numpy as np

def gen_dataset(n=1_000_000, dim=128, seed=42):
    """生成标准化高斯向量数据集"""
    rng = np.random.default_rng(seed)
    vecs = rng.standard_normal((n, dim)).astype(np.float32)
    # 归一化（余弦相似度场景）
    norms = np.linalg.norm(vecs, axis=1, keepdims=True)
    return vecs / norms

# 也可使用公开数据集（推荐）
# SIFT-1M: http://corpus-texmex.irisa.fr/  (128维，100万向量)
# GloVe-1M: 100维词向量，接近真实 NLP 场景
```

### 3.4 测试框架结构

```
bench/
├── conftest.py            # pytest fixtures，PG 连接管理
├── baseline.py            # Baseline 性能采集
├── test_query_speed.py    # 优化A/B 的查询延迟测试
├── test_build_speed.py    # 优化C 的构建速度测试
├── test_memory.py         # 优化D/E 的内存占用测试
├── metrics.py             # QPS、Recall@K、p99延迟计算
└── plot_results.py        # 生成对比图表
```

```python
# bench/metrics.py
import time, psycopg2
import numpy as np

def measure_recall_at_k(conn, query_vecs, ground_truth, k=10, ef=64):
    """
    ground_truth: shape (n_queries, k)，由暴力搜索预先生成
    返回：Recall@K, avg_latency_ms, p99_latency_ms
    """
    latencies = []
    hits = 0
    with conn.cursor() as cur:
        cur.execute(f"SET hnsw.ef_search = {ef}")
        for i, q in enumerate(query_vecs):
            vec_str = "[" + ",".join(map(str, q)) + "]"
            t0 = time.perf_counter()
            cur.execute(
                "SELECT id FROM items ORDER BY embedding <-> %s::vector LIMIT %s",
                (vec_str, k)
            )
            rows = cur.fetchall()
            latencies.append((time.perf_counter() - t0) * 1000)
            result_ids = {r[0] for r in rows}
            hits += len(result_ids & set(ground_truth[i]))
    
    recall = hits / (len(query_vecs) * k)
    return {
        "recall@k": recall,
        "avg_latency_ms": np.mean(latencies),
        "p99_latency_ms": np.percentile(latencies, 99),
        "qps": 1000 / np.mean(latencies)
    }

def measure_build_time(conn, table, index_params):
    """测量 CREATE INDEX 耗时"""
    with conn.cursor() as cur:
        cur.execute(f"DROP INDEX IF EXISTS items_embedding_idx")
        conn.commit()
        t0 = time.perf_counter()
        cur.execute(f"""
            CREATE INDEX items_embedding_idx ON {table}
            USING hnsw (embedding vector_l2_ops)
            WITH (m={index_params['m']}, ef_construction={index_params['ef_construction']})
        """)
        conn.commit()
        return time.perf_counter() - t0

def measure_index_memory(conn):
    """通过 pg_relation_size 估算索引内存占用"""
    with conn.cursor() as cur:
        cur.execute("SELECT pg_size_pretty(pg_relation_size('items_embedding_idx'))")
        return cur.fetchone()[0]
```

### 3.5 Baseline 采集脚本

```python
# bench/baseline.py
import psycopg2, json
from metrics import measure_recall_at_k, measure_build_time, measure_index_memory

PARAMS = {
    "m": 16,
    "ef_construction": 64,
    "ef_search_list": [32, 64, 128, 256]
}

def run_baseline():
    conn = psycopg2.connect("dbname=benchdb user=postgres")
    results = {}
    
    # 构建速度基准
    results["build_time_s"] = measure_build_time(conn, "items_1m", PARAMS)
    results["index_size"] = measure_index_memory(conn)
    
    # 查询基准（不同 ef_search）
    results["query"] = {}
    for ef in PARAMS["ef_search_list"]:
        results["query"][ef] = measure_recall_at_k(conn, query_vecs, gt, ef=ef)
    
    with open("results/baseline.json", "w") as f:
        json.dump(results, f, indent=2)
    print(json.dumps(results, indent=2))

if __name__ == "__main__":
    run_baseline()
```

### 3.6 Profiling 工具使用

```bash
# perf 热点分析（定位 HnswSearchLayer 内部瓶颈）
sudo perf record -g -p $(pgrep postgres) -- sleep 10
sudo perf report --sort=symbol | head -30

# valgrind 内存分析（验证优化D的效果）
valgrind --tool=massif --pages-as-heap=yes \
    postgres --single benchdb < query.sql

# 查看 Buffer Cache 命中率（验证优化E）
SELECT * FROM pg_statio_user_indexes WHERE indexrelname = 'items_embedding_idx';
```

---

## 四、Baseline 与目标提升指标

> 测试环境：Ubuntu 22.04，16 core CPU（支持 AVX2），64GB RAM  
> 数据集：SIFT-1M（100万向量，128维），m=16，ef_construction=64

### 查询速度（优化 A + B）

| 指标 | Baseline | 优化目标 | 提升幅度 |
|------|----------|----------|----------|
| 平均查询延迟 @ ef=64 | ~8ms | <6ms | **↓ 25%** |
| p99 查询延迟 @ ef=64 | ~25ms | <16ms | **↓ 35%** |
| QPS（并发8） | ~800 | >1000 | **↑ 25%** |
| Recall@10 @ ef=64 | ~0.92 | ≥0.92（不退化） | 持平 |
| Recall@10（自适应ef） | ~0.92 | >0.95 | **↑ 3pp** |

### 构建速度（优化 C）

| 指标 | Baseline | 优化目标 | 提升幅度 |
|------|----------|----------|----------|
| Build 时间（4 worker） | ~180s | <150s | **↓ 17%** |
| Build 时间（8 worker） | ~140s | <95s | **↓ 32%** |
| Build 时间（16 worker）| ~160s（锁退化）| <100s | **↓ 38%** |
| 并行扩展比（8/4 worker）| ~1.3x | >1.7x | **↑ 31%** |

### 内存占用（优化 D + E）

| 指标 | Baseline | 优化目标 | 提升幅度 |
|------|----------|----------|----------|
| Index 文件大小（1M，128维）| ~780MB | <320MB | **↓ 59%** |
| 查询期 Buffer Cache Miss率 | ~15% | <8% | **↓ 47%** |
| 冷启动首次查询延迟 | ~120ms | <50ms | **↓ 58%** |
| Recall@10（SQ8量化后）| ~0.92 | ≥0.90 | 损失 ≤2pp |

---

## 五、14 天时间安排

> 图例：📖 = 阅读/分析  🔧 = 编码实现  🧪 = 测试验证  📝 = 总结记录

---

### 第一阶段：环境搭建与基线建立（Day 1-2）

#### Day 1（3h）— 环境搭建与 Baseline 采集
**目标**：能跑通 pgvector，采集到完整的 baseline 数据。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-1:00 | 1h | 🔧 搭建 PostgreSQL 16 + pgvector 环境，编译安装，创建 benchdb |
| 1:00-2:00 | 1h | 🔧 生成/下载 SIFT-1M 数据集，导入 PostgreSQL，创建 baseline 索引 |
| 2:00-3:00 | 1h | 🧪 运行 `baseline.py`，记录查询延迟、构建时间、索引大小；配置 perf |

**产出**：`results/baseline.json`，后续所有优化的对比基准。

---

#### Day 2（3h）— 源码精读与 Profiling
**目标**：定位真实热点，验证优化假设。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-1:30 | 1.5h | 📖 精读 `hnswscan.c:HnswSearchLayer()`、`hnswbuild.c:HnswInsertElement()`，画出调用栈 |
| 1:30-2:30 | 1h | 🧪 `perf record` 采集查询热点，`perf report` 确认 heap 操作和距离计算占比 |
| 2:30-3:00 | 0.5h | 📝 整理 profiling 结果，对照五个优化点逐一确认假设成立 |

**产出**：热点分析报告（markdown 注释），确认 Day 3 起的实现顺序。

---

### 第二阶段：查询速度优化（Day 3-6）

#### Day 3（3h）— 优化A：邻居 PrefetchBuffer 实现
**目标**：在 `HnswSearchLayer` 中加入 look-ahead 预取。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-0:30 | 0.5h | 📖 阅读 PG `PrefetchBuffer` 文档和使用示例（参考 `nodeSeqscan.c`） |
| 0:30-2:00 | 1.5h | 🔧 修改 `hnswscan.c`：在邻居遍历前插入 `PrefetchBuffer`，PREFETCH_LOOKAHEAD=4 |
| 2:00-3:00 | 1h | 🧪 运行查询基准测试，对比 p99 延迟；用 `pg_statio` 验证 buffer hit 率提升 |

**验收标准**：p99 延迟改善 ≥10%，Recall 无退化。

---

#### Day 4（3h）— 优化A：SIMD 距离函数实现
**目标**：为 L2 距离计算增加 AVX2 向量化路径。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-0:30 | 0.5h | 📖 阅读 pgvector 现有 `HnswGetDistance` 实现；查看 `immintrin.h` AVX2 API |
| 0:30-2:00 | 1.5h | 🔧 新建 `simd_distance.h`，实现 AVX2 L2 距离；处理维度余数；Makefile 加 `-mavx2` |
| 2:00-3:00 | 1h | 🧪 单元测试验证数值正确性；集成到 `HnswSearchLayer`；运行查询基准 |

**验收标准**：avg 查询延迟再降 10~15%，SIMD 路径覆盖 ≥80% 的维度计算。

---

#### Day 5（3h）— 优化B：自适应 ef_search 实现
**目标**：per-session 动态调整 ef_search。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-0:30 | 0.5h | 📖 阅读 pgvector GUC 注册代码，理解 `ef_search` 如何传入 scan |
| 0:30-2:00 | 1.5h | 🔧 在 `HnswScanOpaqueData` 中添加 `HnswAdaptiveEf` 结构；实现反馈调整逻辑 |
| 2:00-3:00 | 1h | 🔧 在 `hnswscan.c:HnswGetScanItems` 末尾插入反馈更新调用 |

**注意**：仅修改 per-session 状态，不涉及共享内存，线程安全无需额外考虑。

---

#### Day 6（3h）— 优化B：测试与调参 + A/B 综合验证
**目标**：确认自适应 ef 在不同查询难度下的表现。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-1:00 | 1h | 🧪 构造"简单查询"（密集区域）和"难查询"（稀疏区域）两组测试向量 |
| 1:00-2:00 | 1h | 🧪 对比静态 ef=64 vs 自适应 ef：难查询的 Recall 提升验证 |
| 2:00-3:00 | 1h | 📝 汇总 A+B 优化结果，填写指标对比表；提交 git tag `opt-query-v1` |

**验收标准**：自适应 ef 在难查询上 Recall@10 ≥0.95，QPS 不低于静态 ef=64。

---

### 第三阶段：构建速度优化（Day 7-9）

#### Day 7（3h）— 优化C：锁竞争分析与方案设计
**目标**：精确定位 `HnswInsertElement` 的锁竞争热点，完成方案设计。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-1:00 | 1h | 📖 精读 `hnswinsert.c:HnswInsertElement`、`HnswLockPage`；理解 PG LWLock 机制 |
| 1:00-2:00 | 1h | 🧪 `perf record` 采集 16-worker 并行 build 的锁等待时间（`pgsql_lwlock_acquire`） |
| 2:00-3:00 | 1h | 📝 设计 LWLock Array 方案：确定数组大小256、hash 函数、初始化位置 |

**产出**：设计文档 `docs/per-node-lock-design.md`。

---

#### Day 8（3h）— 优化C：per-node LWLock 实现
**目标**：完成核心代码修改。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-0:30 | 0.5h | 📖 阅读 PG `GetNamedLWLockTranche`、`RequestNamedLWLockTranche` 用法 |
| 0:30-1:30 | 1h | 🔧 修改 `HnswInitMetaPage`：申请 LWLock array；修改 `_PG_init` 注册共享内存 |
| 1:30-3:00 | 1.5h | 🔧 修改 `HnswInsertElement`：替换 page-level lock 为 per-node lock + optimistic retry |

---

#### Day 9（3h）— 优化C：并行 Build 测试与调优
**目标**：验证锁优化效果，调整参数。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-1:00 | 1h | 🧪 测试 1/2/4/8/16 worker 的 build 时间，绘制扩展性曲线 |
| 1:00-2:00 | 1h | 🧪 `perf` 对比优化前后锁等待时间占比；验证索引正确性（Recall 不退化） |
| 2:00-3:00 | 1h | 📝 分析 16-worker 扩展比，记录结果；提交 git tag `opt-build-v1` |

**验收标准**：16 worker 并行扩展比 >1.5x（vs baseline 的 ~1.0x 锁退化）。

---

### 第四阶段：内存优化（Day 10-12）

#### Day 10（3h）— 优化D：SQ8 量化框架实现
**目标**：完成量化编解码核心逻辑。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-0:30 | 0.5h | 📖 阅读 pgvector 的 `halfvec` 实现，理解向量存储格式 |
| 0:30-2:00 | 1.5h | 🔧 新建 `sq8.c/sq8.h`：实现 `SQ8Params` 统计、`EncodeVectorSQ8`、`ApproxL2SQ8` |
| 2:00-3:00 | 1h | 🧪 单元测试：验证 1000 个随机向量的量化误差 < 1%；int8 距离与 float32 距离相关性 > 0.99 |

---

#### Day 11（3h）— 优化D：集成到 HNSW Build/Search 流程
**目标**：在 index page 存储 int8 向量，search 使用近似距离。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-1:30 | 1.5h | 🔧 修改 `hnswbuild.c`：build 完成后遍历节点，将向量副本量化为 int8 存储 |
| 1:30-2:30 | 1h | 🔧 修改 `hnswscan.c`：距离计算优先走 `ApproxL2SQ8`，rerank 阶段用原始 float32 |
| 2:30-3:00 | 0.5h | 🧪 验证索引文件大小缩减；快速 Recall 测试确保 ≥0.90 |

---

#### Day 12（3h）— 优化E：冷热分层实现与综合内存测试
**目标**：实现上层节点预热，综合测试 D+E 内存效果。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-1:30 | 1.5h | 🔧 实现 `HnswWarmUpperLayers()`：BFS 收集上层节点 → `ReadBufferExtended` 预热 |
| 1:30-2:30 | 1h | 🧪 模拟冷启动：`pg_prewarm` 清除缓存后测查询延迟；对比有无预热 |
| 2:30-3:00 | 0.5h | 📝 汇总 D+E 内存优化结果；提交 git tag `opt-memory-v1` |

**验收标准**：冷启动延迟降低 ≥40%，SQ8 索引大小 <基线的 45%。

---

### 第五阶段：综合验证与总结（Day 13-14）

#### Day 13（3h）— 全量综合性能测试
**目标**：在同一环境下对所有优化点做完整的对比测试。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-1:30 | 1.5h | 🧪 依次测试：baseline / opt-A / opt-A+B / opt-C / opt-D / opt-D+E / all-opt 七个版本 |
| 1:30-2:30 | 1h | 🧪 各版本均测：build time（4/8/16 worker）、query latency（avg/p99）、Recall@10、index size |
| 2:30-3:00 | 0.5h | 📝 汇总所有数据到 `results/final_benchmark.json`；生成对比图表 |

---

#### Day 14（3h）— 文档整理与简历素材提炼
**目标**：输出可直接用于简历和技术面试的材料。

| 时段 | 时长 | 任务 |
|------|------|------|
| 0:00-1:30 | 1.5h | 📝 撰写 `README.md`：问题背景、方案选择、实现要点、测试结果（含图表） |
| 1:30-2:30 | 1h | 📝 提炼简历条目：每个优化点一句话描述 + 量化数据（参考下方模板） |
| 2:30-3:00 | 0.5h | 🔧 整理代码：清理调试代码，补充关键注释，确保可 review |

---

## 六、简历条目参考模板

完成14天计划后，可参考以下格式写入简历：

```
优化 pgvector HNSW 索引（PostgreSQL 向量检索扩展）

• 查询加速：在 HnswSearchLayer 引入邻居 Page 异步预取（PrefetchBuffer）
  及 AVX2 SIMD 距离计算，p99 查询延迟降低 35%，QPS 提升 25%

• 自适应召回：实现基于frontier-gap反馈的动态 ef_search 调整，
  难查询场景 Recall@10 从 0.92 提升至 0.96

• 并行构建：将图连接阶段的 page-level 锁替换为 per-node LWLock Array，
  消除 16-worker 时的锁退化，并行构建速度提升 38%

• 内存压缩：实现向量 SQ8（int8）量化存储，索引体积降低 59%，
  Recall@10 损失 < 2pp；结合上层节点 Buffer 预热，冷启动延迟降低 58%
```

---

## 七、风险与应对

| 风险 | 概率 | 应对方案 |
|------|------|----------|
| AVX2 指令集不可用 | 低 | 用 SSE4.2 替代，收益约 AVX2 的 60% |
| SQ8 Recall 损失超过 2pp | 中 | 降低量化激进程度：用 int12 或局部维度保留 float32 |
| per-node LWLock 内存申请失败 | 低 | 减少锁槽数量到 128；或退回 page-level 锁但增加 retry |
| 并行 build 后索引正确性问题 | 中 | 每次优化后必须运行完整 Recall 测试作为回归 |
| 测试机器无 AVX2 | 低 | 在 CI 中用 `-march=native` 检测，无 AVX2 时编译时 fallback |

## 八、提示词

### 第二阶段

```
【项目背景】
我正在优化 pgvector（v0.7.0）的 HNSW 索引，项目是一个 RAG 求职助手，
使用 OpenAI text-embedding-ada-002（1536维），生产数据约50万条简历/JD向量，
并发查询约8~16个session，要求 p99 < 20ms，Recall@10 ≥ 0.92。

【本阶段任务：查询速度优化，包含优化A和优化B】

━━━━━━━━━━━━━━━━━━━━━━━━━━
优化A：邻居预取 + SIMD 距离加速
━━━━━━━━━━━━━━━━━━━━━━━━━━

**A-1 邻居 PrefetchBuffer（修改 src/hnsw/hnswscan.c）**
在 HnswSearchLayer() 的候选队列循环中，每次 pop 当前节点后，
对其后续 PREFETCH_LOOKAHEAD=4 个邻居的 BlockNumber 调用 PrefetchBuffer()，
使 OS/PG 提前发起异步 I/O。实现要点：
- 用 ItemPointerGetBlockNumber(&neighbors->items[i].heaptid) 获取 blkno
- PrefetchBuffer(index, MAIN_FORKNUM, blkno) 发起预取
- 预取数量不超过 neighbors->count，避免越界

**A-2 AVX2 SIMD 距离加速（新建 src/hnsw/simd_distance.h）**
新建头文件，实现 float32 L2 距离的 AVX2 路径：
- 使用 _mm256_loadu_ps / _mm256_sub_ps / _mm256_mul_ps / _mm256_add_ps 展开8-wide内积
- 维度不是8的倍数时用标量路径处理余数
- 文件顶部用 #ifdef __AVX2__ 宏保护，无AVX2时自动 fallback 到标量
- Makefile 中追加 OPTFLAGS += -mavx2（需检测 pg_config --cflags 是否已有）
- 在 HnswSearchLayer 中替换原有距离调用为 HnswGetDistanceSIMD()

━━━━━━━━━━━━━━━━━━━━━━━━━━
优化B：自适应 ef_search（修改 src/hnsw/hnswscan.c 和 src/hnsw/hnsw.h）
━━━━━━━━━━━━━━━━━━━━━━━━━━

在 HnswScanOpaqueData 中新增 HnswAdaptiveEf 结构体：
  - int ef_current（当前动态ef值，初始化为 GUC ef_search）
  - float last_frontier_gap（上次查询最优候选与截止候选的距离差）
  - int consecutive_tight（连续"召回紧张"次数）

查询结束后（在 HnswGetScanItems 末尾）调用反馈函数 HnswUpdateAdaptiveEf()：
  - gap < TIGHT_THRESHOLD 且 consecutive_tight > 2 时：ef_current *= 1.5，上限 ef_search * 4
  - gap > LOOSE_THRESHOLD 时：ef_current *= 0.85，下限 base_ef（GUC值）
  - 仅修改 per-session 状态，不涉及共享内存

━━━━━━━━━━━━━━━━━━━━━━━━━━
测试文件要求（./test/optimize/ 目录下新建）
━━━━━━━━━━━━━━━━━━━━━━━━━━

请新建以下测试文件：

1. test/optimize/test_simd_distance.c
   - 正确性测试：对随机float32向量，SIMD结果与标量结果误差 < 1e-4
   - 边界测试：dim=1 / dim=7 / dim=8 / dim=9 / dim=1536（非8倍数）
   - 零向量测试、相同向量测试（距离应为0）
   - 性能对比测试：记录1M次计算的耗时（SIMD vs 标量）

2. test/optimize/test_prefetch.c
   - 验证 PrefetchBuffer 调用不会在 neighbors->count=0 时越界
   - 验证 PREFETCH_LOOKAHEAD 宏在索引只有1个邻居时的边界行为
   - 并发测试：8个session同时查询，验证无crash、无内存错误

3. test/optimize/test_adaptive_ef.c
   - 单元测试 HnswUpdateAdaptiveEf()：gap极小时ef上升、gap极大时ef下降
   - 边界测试：ef_current 不超过 ef_search*4，不低于 base_ef
   - 连续调用100次，验证收敛稳定性
   - 并发测试：每个session独立状态，互不干扰

━━━━━━━━━━━━━━━━━━━━━━━━━━
阶段 Benchmark（针对 RAG 求职助手场景）
━━━━━━━━━━━━━━━━━━━━━━━━━━

请新建 bench/benchmark_query_speed.py，设计如下：

数据规模：50万条1536维向量（模拟简历+JD混合语料），m=16，ef_construction=64
查询集：2000条query向量，分两组——
  - "容易查询"：从数据集中心区域采样（高密度，gap大）
  - "困难查询"：从数据集边缘区域采样（稀疏，gap小）
并发：8个线程并发查询，模拟生产负载

测试版本：baseline / A（prefetch only）/ A+SIMD / A+SIMD+B（自适应ef）
测量指标：avg_latency_ms / p99_latency_ms / QPS / Recall@10

输出一个 Markdown 实验数据表格（数据列留空，供我填入），格式如下：
| 版本 | avg延迟(ms) | p99延迟(ms) | QPS | Recall@10(容易) | Recall@10(困难) |
|------|------------|------------|-----|----------------|----------------|
| baseline | | | | | |
| +Prefetch | | | | | |
| +SIMD | | | | | |
| +自适应ef | | | | | |

━━━━━━━━━━━━━━━━━━━━━━━━━━
阶段文档
━━━━━━━━━━━━━━━━━━━━━━━━━━

请新建 docs/phase2_query_optimization.md，包含：
1. 优化背景与问题根因
2. 优化A（Prefetch + SIMD）实现原理与关键代码片段
3. 优化B（自适应ef）实现原理与关键代码片段
4. 单元测试说明（每个test文件的测试覆盖点）
5. Benchmark设计说明
6. 实验数据表格（空白，供填入）
7. 风险说明（AVX2不可用的fallback策略）
```

### 第三阶段

```
【项目背景】
同上，RAG 求职助手，50万条1536维向量。构建索引时使用 parallel build，
目标是消除16-worker时的锁退化，使并行扩展比从~1.0x提升到>1.5x。

【本阶段任务：构建速度优化，优化C — per-node LWLock 细粒度锁】

━━━━━━━━━━━━━━━━━━━━━━━━━━
优化C 核心实现（涉及文件：src/hnsw/hnsw.h / hnswbuild.c / hnswinsert.c）
━━━━━━━━━━━━━━━━━━━━━━━━━━

**C-1 共享内存 LWLock Array 申请（修改 _PG_init 和 HnswInitMetaPage）**
- 在 _PG_init 中调用 RequestNamedLWLockTranche("hnsw_node_locks", HNSW_NUM_NODE_LOCKS)
  其中 HNSW_NUM_NODE_LOCKS = 256（2的幂，定义在 hnsw.h）
- 在 HnswInitMetaPage() 中通过 GetNamedLWLockTranche("hnsw_node_locks")
  获取锁槽指针，存入 meta page 的扩展字段（或维护一个全局静态指针）

**C-2 插入时替换为 per-node 粒度加锁（修改 HnswInsertElement）**
原逻辑：LWLockAcquire(BufferDescGetContentLock(buf), LW_EXCLUSIVE) 对整个Page加锁
新逻辑：
  - lock_idx = element->neighborPage % HNSW_NUM_NODE_LOCKS
  - LWLockAcquire(&hnsw_node_locks[lock_idx], LW_EXCLUSIVE)
  - 修改邻居列表
  - LWLockRelease(&hnsw_node_locks[lock_idx])

**C-3 Optimistic Retry（乐观冲突检测）**
在邻居列表头部写入 version counter（uint32）：
- 加锁前读取 version_before
- 加锁后重新读取 version_after
- 若 version_before != version_after，说明有其他 worker 已修改，
  释放锁后重试（最多 HNSW_MAX_RETRY=5 次）
- 每次邻居列表写入后 version++

━━━━━━━━━━━━━━━━━━━━━━━━━━
测试文件要求（./test/optimize/ 目录下新建）
━━━━━━━━━━━━━━━━━━━━━━━━━━

1. test/optimize/test_lwlock_array.c
   - 验证 LWLock array 初始化后各槽状态正常（未被持有）
   - 验证 lock_idx = neighborPage % 256 的 hash 均匀性（模拟1万个节点的分布）
   - 边界测试：neighborPage=0、neighborPage=255、neighborPage=256（取模回绕）

2. test/optimize/test_parallel_build.c
   - 使用 pg_background 或 pthreads 模拟8个并发 worker 同时插入节点
   - 验证无死锁（超时检测：单次build不超过60s）
   - 验证 version counter 在冲突时正确触发 Retry 逻辑
   - 验证 Retry 次数不超过 HNSW_MAX_RETRY 的情况下能成功插入
   - 构建完成后验证索引正确性：对100个query执行搜索，Recall@10 ≥ 0.90

3. test/optimize/test_build_correctness.c
   - 1/2/4/8/16 worker 分别构建同一数据集，验证最终索引的邻居关系一致性
   - 边界测试：只有1个节点时的构建、只有2个节点时的构建
   - 压力测试：10000次快速插入，验证无内存泄漏（配合 valgrind）

━━━━━━━━━━━━━━━━━━━━━━━━━━
阶段 Benchmark（RAG 求职助手场景）
━━━━━━━━━━━━━━━━━━━━━━━━━━

请新建 bench/benchmark_build_speed.py，设计如下：

数据规模：50万条1536维向量，分批插入（模拟生产环境离线批量建索引）
测试版本：baseline（page-level lock）/ 优化C（per-node lock）
Worker数量：1 / 2 / 4 / 8 / 16
测量指标：build_time_s（秒）、并行扩展比（T_1worker / T_Nworker）、
          perf stat中 lwlock_acquire 等待时间占比（如环境支持）

额外测试：构建完成后立即执行1000次查询，验证 Recall@10 ≥ 0.90（正确性回归）

输出实验数据表格（数据列留空）：
| Worker数 | baseline build时间(s) | 优化C build时间(s) | 扩展比(baseline) | 扩展比(优化C) |
|---------|---------------------|-----------------|----------------|-------------|
| 1  | | | 1.00 | 1.00 |
| 2  | | | | |
| 4  | | | | |
| 8  | | | | |
| 16 | | | | |

━━━━━━━━━━━━━━━━━━━━━━━━━━
阶段文档
━━━━━━━━━━━━━━━━━━━━━━━━━━

请新建 docs/phase3_build_optimization.md，包含：
1. 锁竞争问题根因分析（page-level lock 在多 worker 下的退化机制）
2. LWLock Array 方案设计（锁槽数量选择依据、hash 策略）
3. Optimistic Retry 实现原理与 version counter 设计
4. 关键代码改动说明（_PG_init / HnswInitMetaPage / HnswInsertElement 各自的改动）
5. 单元测试覆盖说明
6. Benchmark设计说明
7. 实验数据表格（空白）
8. 风险说明（锁槽不足时的降级策略、Retry超限的处理）
```

### 第四阶段

```
【项目背景】
同上，RAG 求职助手，50万条1536维向量。生产环境内存受限（8GB可用），
索引文件需能完整加载，冷启动（服务重启后首次查询）延迟需 < 100ms。

【本阶段任务：内存优化，包含优化D（SQ8量化）和优化E（冷热分层预热）】

━━━━━━━━━━━━━━━━━━━━━━━━━━
优化D：向量 SQ8 量化压缩（新建 src/hnsw/sq8.c + sq8.h）
━━━━━━━━━━━━━━━━━━━━━━━━━━

**D-1 sq8.h 数据结构定义**
typedef struct SQ8Params {
    int   dim;
    float min[HNSW_MAX_DIM];       // 每维最小值（build阶段统计）
    float scale[HNSW_MAX_DIM];     // scale[i] = 255.0f / (max[i] - min[i])
    float avg_scale_sq;            // 平均 scale^2，用于近似距离反量化
} SQ8Params;

**D-2 sq8.c 核心函数实现**
- SQ8ParamsInit()：初始化，所有min=+INF，scale=0
- SQ8ParamsUpdate(params, float* vec, dim)：用新向量更新每维 min/max
- SQ8ParamsFit(params)：build完成后，由 min/max 计算 scale 和 avg_scale_sq
- HnswEncodeVectorSQ8(float* src, uint8_t* dst, params, dim)：float32→uint8量化
- HnswDecodeVectorSQ8(uint8_t* src, float* dst, params, dim)：uint8→float32反量化
- HnswApproxL2SQ8(uint8_t* a, uint8_t* b, params, dim)：int8路径近似L2距离

**D-3 集成到 Build/Search 流程**
- hnswbuild.c：build 第一遍遍历时调用 SQ8ParamsUpdate 统计min/max，
  build 完成后 SQ8ParamsFit，然后遍历所有节点将向量副本替换为 uint8 存储
  （SQ8Params 序列化后存入 index meta page 的扩展区域）
- hnswscan.c：HnswSearchLayer 中距离计算优先走 HnswApproxL2SQ8，
  最终 top-k 候选的 rerank 阶段读取原始 heap tuple 用 float32 精确排序

━━━━━━━━━━━━━━━━━━━━━━━━━━
优化E：上层节点冷热分层预热（修改 src/hnsw/hnswbuild.c 和 hnsw.c）
━━━━━━━━━━━━━━━━━━━━━━━━━━

**E-1 HnswWarmUpperLayers() 实现**
- build 完成后，从 entry point 出发做 BFS，收集所有 layer >= 1 的节点
- 对每个上层节点的 blkno 调用：
    Buffer buf = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
                                    RBM_NORMAL, GetAccessStrategy(BAS_BULKREAD));
    ReleaseBuffer(buf);
  （目的是将这些 Page 预先加载进 PG Buffer Pool）
- 暴露一个 SQL 函数 hnsw_warm_upper_layers(index_name regclass)，
  供用户在服务重启后手动调用

**E-2 Build 时上层节点集中写入（可选，若工期允许）**
修改 hnswbuild.c 中节点写入顺序：优先为 layer>=1 的节点分配前 N 个 Page，
提升空间局部性，对 OS readahead 更友好。

━━━━━━━━━━━━━━━━━━━━━━━━━━
测试文件要求（./test/optimize/ 目录下新建）
━━━━━━━━━━━━━━━━━━━━━━━━━━

1. test/optimize/test_sq8.c
   - 正确性测试：1000个随机1536维向量，量化后反量化，每维误差 < 1/255
   - 距离相关性测试：2000对向量，SQ8近似距离与float32精确距离的 Pearson 相关系数 > 0.99
   - 边界测试：所有维度值相同（scale=0时的除零保护）、dim=1、dim=1536
   - 量化误差分布测试：统计误差均值、方差、最大值，打印直方图
   - 内存测试：50万个向量量化后内存占用（uint8 vs float32 的实际分配对比）

2. test/optimize/test_warm_upper_layers.c
   - 验证 BFS 收集到的上层节点数量符合理论值（约 总节点/M）
   - 验证 ReadBufferExtended 调用无报错，Buffer 正确 Release（无 Buffer leak）
   - 冷热对比测试：
       a. DROP CACHE（echo 3 > /proc/sys/vm/drop_caches）
       b. 执行100次查询，记录延迟
       c. 调用 hnsw_warm_upper_layers()
       d. 再次执行100次查询，验证 p99 延迟降低 ≥ 30%
   - 边界测试：只有 layer=0（无上层节点）时函数正常返回

3. test/optimize/test_memory_integration.c
   - D+E 联合测试：构建 SQ8 量化索引后调用预热，执行查询
   - 验证 Recall@10 ≥ 0.90（量化损失上限）
   - 验证索引文件大小 < baseline 的 45%
   - 并发测试：8个session同时查询量化索引，无 data race，结果一致

━━━━━━━━━━━━━━━━━━━━━━━━━━
阶段 Benchmark（RAG 求职助手场景）
━━━━━━━━━━━━━━━━━━━━━━━━━━

请新建 bench/benchmark_memory.py，设计如下：

数据规模：50万条1536维向量（模拟生产数据量）
测试版本：baseline（float32）/ 优化D（SQ8量化）/ 优化D+E（量化+预热）
冷启动模拟：每次测试前执行 SELECT pg_prewarm('index_name', 'buffer', 'main', NULL, NULL)
            的反向操作（通过 pg_buffercache 或重启 pg 服务清除 buffer）

测量指标：
- index_size_mb（索引文件大小）
- cold_start_p99_ms（清除 buffer 后前10次查询的 p99 延迟）
- warm_p99_ms（buffer 充分预热后的 p99 延迟）
- buffer_miss_rate（通过 pg_statio_user_indexes 计算）
- Recall@10（量化精度验证）

输出实验数据表格（数据列留空）：
| 版本 | 索引大小(MB) | 冷启动p99(ms) | 热查询p99(ms) | Buffer Miss率 | Recall@10 |
|------|------------|--------------|--------------|--------------|----------|
| baseline | | | | | |
| +SQ8量化 | | | | | |
| +量化+预热 | | | | | |

━━━━━━━━━━━━━━━━━━━━━━━━━━
阶段文档
━━━━━━━━━━━━━━━━━━━━━━━━━━

请新建 docs/phase4_memory_optimization.md，包含：
1. 内存问题根因（float32存储开销分析：50万×1536×4bytes = 3GB+）
2. SQ8量化原理（min/max统计、定点映射、近似距离误差分析）
3. 与 pgvector halfvec 的区别（内部优化 vs 用户暴露类型）
4. 冷热分层原理（HNSW分层结构中上层节点的访问频率分析）
5. 关键代码改动说明（sq8.c 各函数 / hnswbuild.c 集成 / HnswWarmUpperLayers）
6. 单元测试覆盖说明
7. Benchmark设计说明（冷启动模拟方法说明）
8. 实验数据表格（空白）
9. 风险说明（SQ8 Recall 损失超限时的降级方案：int12 或局部维度保留float32）
```

### 第五阶段

