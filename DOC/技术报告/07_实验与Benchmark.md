# 第 7 章：Benchmark 方法论与完整实验数据

## 7.1 实验设计原则

### 7.1.1 可重现性

- **确定性数据集**：使用 `setseed(0.42)` 生成随机向量，多次运行结果一致
- **Ground Truth**：用 `enable_indexscan=off` 的暴力顺序扫描求精确 Top-K
- **Warmup**：每组实验前先跑 1 遍所有查询，让 buffer pool 预热
- **多轮平均**（受限于时间，本次单轮，生产建议 5-10 轮）

### 7.1.2 对比公平性

| 维度 | 保证 |
|---|---|
| **数据集** | 同一份 `bench_items` 表，同一份 ground truth |
| **索引参数** | `m=16, ef_construction=64` 固定 |
| **查询负载** | 同一份 50 条 `bench_queries` |
| **环境** | 同一台机器，同一 PG 实例，连续运行避免系统状态差异 |
| **唯一变量** | `hnsw.hot_cold_enabled = off / on` |

### 7.1.3 指标选择

#### Recall@K

$$
\text{Recall@K} = \frac{|\text{IndexResult} \cap \text{GroundTruth}|}{K}
$$

- **含义**：索引返回的 Top-K 中有多少是真正的 Top-K
- **重要性**：RAG 场景下 Recall 直接影响 LLM 回答质量，不可妥协
- **预期**：优化前后 Recall 应**完全一致**（语义不变）

#### 延迟分布

- **avg**：平均延迟，反映整体性能
- **p50（中位数）**：一半查询的延迟，反映典型情况
- **p95**：95% 查询的延迟，反映尾部情况
- **p99**：99% 查询的延迟，**SLA 关键指标**（长尾查询）

对于在线服务，p95/p99 比 avg 更重要。

#### Buffer 行为

- `shared hit`：命中 buffer pool 的次数
- `shared read`：从磁盘读取的次数
- `shared read` 下降 + `shared hit` 上升 = prefetch 生效

## 7.2 数据集规格

| 参数 | 小数据集 | 大数据集 | 说明 |
|---|---:|---:|---|
| 行数 | 50,000 | 200,000 | 向量数量 |
| 维度 | 128 | 128 | 与 OpenAI text-embedding-ada-002 一致 |
| 查询数 | 50 | 50 | 每次 benchmark 执行的查询数 |
| Top-K | 10 | 10 | 召回前 10 个 |
| 索引 m | 16 | 16 | HNSW 每层邻居数上限 |
| ef_construction | 64 | 64 | 构建时 beam 宽度 |
| 索引大小 | ~25 MB | ~100 MB | on-disk 大小 |
| ground truth 构建 | ~30 秒 | ~3-5 分钟 | 250 万次距离计算（50k×50） / 1000 万次（200k×50） |

## 7.3 Phase 1 完整实验数据

### 7.3.1 小数据集（50k 行）

#### ef_search=40

| 配置 | Recall@10 | avg (ms) | p50 (ms) | p95 (ms) | p99 (ms) | min (ms) | max (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|
| off | 0.3760 | 1.725 | 1.663 | 2.621 | 2.827 | 1.088 | 2.880 |
| on  | 0.3760 | **1.559** | 1.526 | **1.976** | 2.470 | 1.040 | 2.923 |
| **改善** | **0%** | **-9.6%** | -8.2% | **-24.6%** | -12.6% | +4.4% | -1.5% |

#### ef_search=200

| 配置 | Recall@10 | avg (ms) | p50 (ms) | p95 (ms) | p99 (ms) | min (ms) | max (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|
| off | 0.7420 | 4.726 | 4.431 | 6.567 | 7.584 | 3.118 | 7.697 |
| on  | 0.7420 | 4.715 | 4.539 | **5.830** | **6.891** | 3.443 | 7.145 |
| **改善** | **0%** | -0.2% | +2.4% | **-11.2%** | **-9.1%** | -10.4% | +7.2% |

**小数据集结论**：

- Recall 完全一致，语义零改变 ✅
- 数据集能完全放进 buffer pool，收益主要在**尾延迟**（p95 -24.6%）
- avg 改善有限（-9.6% / -0.2%），因为大部分查询本就命中 buffer

### 7.3.2 大数据集（200k 行）

#### ef_search=40

| 配置 | Recall@10 | avg (ms) | p50 (ms) | p95 (ms) | p99 (ms) | min (ms) | max (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|
| off | 0.1980 | 6.506 | 6.609 | 9.256 | 9.580 | 3.891 | 9.732 |
| on  | 0.1980 | **4.785** | **4.441** | **6.899** | **7.977** | 3.105 | 8.653 |
| **改善** | **0%** | **-26.5%** | **-32.8%** | **-25.5%** | **-16.7%** | +20.2% | +11.1% |

#### ef_search=200

| 配置 | Recall@10 | avg (ms) | p50 (ms) | p95 (ms) | p99 (ms) | min (ms) | max (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|
| off | 0.4780 | 22.952 | 21.765 | 27.524 | **48.092** | 17.152 | 64.575 |
| on  | 0.4780 | **15.415** | **14.527** | **20.530** | **21.227** | 12.110 | 21.276 |
| **改善** | **0%** | **-32.8%** | **-33.3%** | **-25.4%** | **-55.9%** | +29.4% | +67.1% |

**大数据集结论**：

- Recall 完全一致 ✅
- **索引超出 buffer pool → buffer miss 频繁 → prefetch 收益显著**
- ef_search=200 时 **p99 从 48ms 降到 21ms（-55.9%）**，这是最重要的结果
- avg 改善达 -26.5% ~ -32.8%，远超小数据集

### 7.3.3 Recall 偏低的原因分析

为什么 ef_search=40 时 Recall 只有 0.198？

1. **128 维随机向量**：IID（独立同分布）随机数，几乎所有向量等距，HNSW 难以区分
2. **小 ef_search**：beam 宽度只有 40，容易被局部最优困住
3. **数据集规模大**：200k 行时噪声更多

**验证**：ef_search=200 时 Recall 提升到 0.478，说明算法本身没问题。

真实 embedding（如 text-embedding-ada-002）有语义聚类，Recall 通常 > 0.9。

## 7.4 Phase 2 实验数据

### 7.4.1 Iterative Scan + WHERE 过滤场景

**数据集**：20k 行 64 维，`tag` 字段（100 个值，1% 命中率）

**查询**：
```sql
SELECT id FROM iter_bench
WHERE  tag = 42
ORDER  BY v <-> '[...]'::vector
LIMIT  10;
```

**配置**：
- `hnsw.ef_search = 40`
- `hnsw.iterative_scan = strict_order`

| 配置 | Execution Time | shared hit | 过滤掉行数 |
|---|---:|---:|---:|
| `ef_search_auto = off` | 16.924 ms | 12660 | 760 |
| `ef_search_auto = on`, mult=2.0 | **13.232 ms** | 15413 | 771 |
| **改善** | **-21.8%** | +21.8% | +1.4% |

**分析**：

- auto=on 时 buffer 访问增加了 ~22%（因为 batch 变大）
- 但 resume 轮数减少，总延迟反而降低 21.8%
- 过滤掉行数略增（771 vs 760），说明 batch 变大时探索了更多候选

## 7.5 Phase 3/4 观测数据

### 7.5.1 Phase 3：构建期 I/O

**数据集**：10k 行 128 维，`maintenance_work_mem=256MB`

| 指标 | 值 |
|---|---:|
| Build time | 5082 ms |
| Index size | 8128 kB (8 MB) |
| relation reads | **2** |
| relation extends | 1016 |
| relation hits | 11880 |
| WAL writes | 485 |

**结论**：

- `reads=2` 说明几乎零回读，瓶颈是 **CPU 距离计算 + 图连接**
- `hits=11880` 说明图连接过程中大量访问已写入的页面，但全在 buffer pool 里
- 不需要 build-time prefetch

### 7.5.2 Phase 4：并行加速比

**数据集**：20k 行 128 维，`maintenance_work_mem=512MB`

| workers | build time (ms) | speedup | 边际收益 |
|:---:|---:|---:|---:|
| 0 (serial) | 11736 | 1.00x | - |
| 1 | 7324 | 1.60x | +0.60x |
| 2 | 5500 | **2.13x** | +0.53x |
| 4 | 5028 | 2.33x | +0.20x |

**结论**：

- 0→2 workers：线性加速（2.13x），说明并行化良好
- 2→4 workers：边际递减（只增加 0.20x），瓶颈转向别处
- `pg_stat_activity` 的 wait_event 为空，说明**无显著锁竞争**
- 真正瓶颈可能是：CPU 单核性能（距离计算）或共享内存分配顺序化

## 7.6 Benchmark 脚手架使用指南

### 7.6.1 setup.sql

**功能**：建数据集 + ground truth

**参数**：
```powershell
--set=n_rows=200000      # 向量数量
--set=dim=128            # 维度
--set=n_queries=50       # 查询数
--set=top_k=10           # Top-K
--set=hnsw_m=16          # HNSW m
--set=hnsw_efc=64        # ef_construction
```

**耗时**：200k 行约 3-5 分钟（主要是 ground truth 的 1000 万次距离计算）

### 7.6.2 run_one.sql

**功能**：跑单配置，输出 recall + latency 摘要

**参数**：
```powershell
--set=top_k=10
--set=ef_search=40
--set=hot_cold=on        # on / off
--set=hot_layer=2
--set=prefetch_neighbors=16
--set=warmup=1           # warmup 遍数
```

**输出**（单行）：
```
hot_cold | ef_search | top_k | n_queries | recall_avg | latency_avg_ms | p50_ms | p95_ms | p99_ms | min_ms | max_ms
```

### 7.6.3 probe_build.sql

**功能**：观测 `CREATE INDEX` 的耗时 + I/O 分布

**使用场景**：怀疑 build 期是 I/O 瓶颈时，先跑这个脚本确认

### 7.6.4 probe_parallel_build.sql

**功能**：sweep 0/1/2/4 workers，输出加速比

**使用场景**：决定生产环境 `max_parallel_maintenance_workers` 设多少

## 7.7 与其他向量数据库对比（未来工作）

| 方案 | Recall@10 | avg (ms) | p99 (ms) | 生态 |
|---|---:|---:|---:|---|
| pgvector 0.8.4（官方） | 0.478 | 22.95 | 48.09 | PostgreSQL |
| **pgvector 优化版（本项目）** | **0.478** | **15.42** | **21.23** | PostgreSQL |
| Milvus | ? | ? | ? | 独立服务 |
| Weaviate | ? | ? | ? | 独立服务 |
| Qdrant | ? | ? | ? | 独立服务 |

未来可在相同数据集（如 SIFT1M）上做公平对比。

## 7.8 实验环境详情

| 组件 | 版本/配置 |
|---|---|
| OS | Windows 10 |
| PostgreSQL | 18.4 |
| pgvector | 0.8.4 base + 本项目优化 |
| 编译器 | MSVC 14.51 (Visual Studio 2018 BuildTools) |
| 编译选项 | `/O2 /fp:fast` |
| CPU | 未记录（多核 x64） |
| 内存 | 未记录（推测 ≥ 16GB） |
| 磁盘 | SSD（推测，未验证 HDD） |
| `shared_buffers` | 默认 128MB（未调优） |
| `maintenance_work_mem` | 默认 64MB |

**局限**：

- 未记录硬件规格（CPU 型号、核数、内存大小）
- 未测 HDD vs SSD 差异
- 单次运行，无多轮平均
- Windows 时钟精度 ~0.5ms，有 jitter

生产 benchmark 建议：
- Linux 环境，`perf` / `iostat` 抓详细指标
- 多轮平均（至少 5 轮）
- 固定 CPU affinity 避免迁移
- 禁用 Turbo Boost 保证频率稳定
