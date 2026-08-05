# HNSW 优化 Benchmark Report

覆盖 Phase 1-4 的实现与实测数据，基于 pgvector 0.8.4。

## 优化概览

Phase 1 新增 4 个查询路径 GUC（默认 off，保持行为向后兼容）：

| GUC | 类型 | 默认 | 作用 |
| --- | --- | --- | --- |
| `hnsw.hot_cold_enabled` | bool | `off` | 总开关，`off` 时代码路径等价官方 0.8.4 |
| `hnsw.hot_layer` | int | `2` | `>=` 该层视为 hot，预取更积极；低层收敛到 4 个 |
| `hnsw.prefetch_neighbors` | int | `16` | 每次 `HnswSearchLayer` 迭代最多预取的邻居索引页数 |
| `hnsw.hot_max_bytes` | int (bytes) | `65536` | 预留，为后续 hot cache / 层内存上限保留位置 |

Phase 2 新增 2 个 iterative scan GUC：

| GUC | 类型 | 默认 | 作用 |
| --- | --- | --- | --- |
| `hnsw.ef_search_auto` | bool | `off` | resume 时按 multiplier 递增 batch_size |
| `hnsw.ef_search_multiplier` | real | `2.0` | 递增倍率，clamp 到 `HNSW_MAX_EF_SEARCH` |

Phase 3/4 只提供**观测脚本**，不改动 build/insert 代码。

核心改动仅在 `src/hnswutils.c` 的 `HnswSearchLayer` 中，紧跟
`HnswLoadUnvisitedFromDisk()` 之后，对未访问邻居的 index page 发起
`PrefetchBuffer(index, MAIN_FORKNUM, blk)`。所有内存路径、in-memory build、
insert 和 vacuum 路径均不受影响。

## 环境

- OS: Windows
- PostgreSQL: 18.4 (`F:\postgresql`)
- pgvector: 0.8.4 base + hot/cold Phase 1
- 编译器: MSVC (Visual Studio 2018 BuildTools) `/O2 /fp:fast`
- 数据: 50000 行 x 128 维随机 `[0,1)^128`
- 查询: 50 条随机向量
- HNSW: `m=16, ef_construction=64, vector_l2_ops`
- Ground truth: seqscan (`enable_indexscan=off, enable_bitmapscan=off`) 求精确 Top-10

## 结果

### 50k 行 (小数据集，索引全部 buffer pool 命中)

| ef_search | hot_cold | Recall@10 | avg (ms) | p50 (ms) | p95 (ms) | p99 (ms) |
| ---: | :---: | ---: | ---: | ---: | ---: | ---: |
| 40  | off | 0.376 | 1.725 | 1.663 | 2.621 | 2.827 |
| 40  | on  | 0.376 | 1.559 (-9.6%) | 1.526 | 1.976 (-24.6%) | 2.470 |
| 200 | off | 0.742 | 4.726 | 4.431 | 6.567 | 7.584 |
| 200 | on  | 0.742 | 4.715 | 4.539 | 5.830 (-11.2%) | 6.891 (-9.1%) |

### 200k 行 (大数据集，on-disk build，索引超出 maintenance_work_mem)

| ef_search | hot_cold | Recall@10 | avg (ms) | p50 (ms) | p95 (ms) | p99 (ms) |
| ---: | :---: | ---: | ---: | ---: | ---: | ---: |
| 40  | off | 0.198 | 6.506 | 6.609 | 9.256 | 9.580 |
| 40  | on  | 0.198 | **4.785 (-26.5%)** | 4.441 (-32.8%) | 6.899 (-25.5%) | 7.977 (-16.7%) |
| 200 | off | 0.478 | 22.952 | 21.765 | 27.524 | 48.092 |
| 200 | on  | 0.478 | **15.415 (-32.8%)** | 14.527 (-33.3%) | 20.530 (-25.4%) | **21.227 (-55.9%)** |

### 关键观察

1. **召回率完全一致（每一档 off/on 完全相同）**
   `PrefetchBuffer` 只是异步 I/O 提示，不改变搜索结果集，语义与官方一致。

2. **数据集规模决定收益幅度**
   50k 行索引能完全塞进 buffer pool，收益主要落在尾延迟（p95 -24.6%）。
   200k 行触发 on-disk build，索引不完全命中，avg 收益直接跃升到 -26% ~ -33%。

3. **高 ef_search 下 p99 改善最戏剧**
   200k / ef=200 时 p99 从 48ms 降到 21.2ms（-55.9%），
   说明大 ef 的尾部 case 主要是 buffer miss 引起的 stall，
   prefetch 把这些 stall 转成了并发 I/O。

4. **Recall 0.198~0.742 偏低是预期行为**
   128 维随机 IID 数据本身对 HNSW 不友好，且数据集越大等距噪声越多；
   这只反映数据集特性，与优化正确性无关。

### 局限性

- 单次运行，无多轮平均；Windows 时钟 jitter 约 0.5ms。
- 未针对 SSD/HDD 做区分测试。
- warmup 只走 1 遍，如果 buffer pool 更小可能低估收益。

## Phase 2: 半动态 `ef_search`

### 新增 GUC

| GUC | 类型 | 默认 | 作用 |
| --- | --- | --- | --- |
| `hnsw.ef_search_auto` | bool | `off` | iterative scan resume 时按 multiplier 扩大 batch |
| `hnsw.ef_search_multiplier` | real | `2.0` | 每次 resume 扩展倍率，`min(ef * mult^resumeCount, 1000)` |

改动位于 `src/hnswscan.c` 的 `ResumeScanItems`，仅在 `hnsw.iterative_scan != off` 时生效。
`HnswScanOpaqueData` 加了 `resumeCount` 字段，由 `hnswrescan` 初始化为 0。

### 观察

在 20k 行 64 维数据 + `WHERE tag = 42`（约 1% 命中率，强制 iterative 扩展）：

| 配置 | Execution Time | shared hit |
| --- | --- | --- |
| `ef_search_auto = off` | 16.9 ms | 12660 |
| `ef_search_auto = on`, mult=2.0 | 13.2 ms (**-21.8%**) | 15413 |

虽然 buffer 访问增加了 ~22%，但每次 resume 覆盖候选更多，
减少了 resume 轮数与相关的锁/上下文开销，端到端反而快 21.8%。

## Phase 3: HNSW 构建期 I/O 观测

Phase 3 定位是 **先观测再决定是否加优化**。我们不修改 build 代码，
只加脚本量化 `CREATE INDEX ... USING hnsw` 的时间与 I/O 分布。

观测脚本：`test/hot_cold/bench/probe_build.sql`
（使用 `pg_stat_io` 的 per-context 计数）。

### 结果 (10k 行 128 维，`maintenance_work_mem=256MB`)

| 指标 | 值 |
| --- | ---: |
| Build time | 5082 ms |
| Index size | 8128 kB |
| relation reads | 2 |
| relation extends | 1016 |
| relation hits | 11880 |
| WAL writes | 485 |

**结论**：在充足 `maintenance_work_mem` 下 build 是纯 in-memory + 追加写，
`reads=2` 说明基本没有回读磁盘。真正的 CPU 瓶颈是距离计算 + 图连接，
不是 I/O。因此**不加 build-time prefetch**。

只有当 `maintenance_work_mem` 小于图规模（触发 on-disk build 提示）时，
Phase 3 才可能有优化空间——那时可以复用 Phase 1 的 `PrefetchBuffer` 思路
到 `HnswInsertTupleOnDisk` 附近。这一步暂不实施，保留作为后续观测入口。

## Phase 4: 并行 build 锁竞争观测

同样是观测优先。脚本 `test/hot_cold/bench/probe_parallel_build.sql`
对同一数据集用 0/1/2/4 workers 分别建索引，并抓 `pg_stat_activity` 的 wait_event。

### 结果 (20k 行 128 维，`maintenance_work_mem=512MB`)

| workers | build time (ms) | speedup |
| :---: | ---: | ---: |
| 0 (serial) | 11736 | 1.00x |
| 1 | 7324 | 1.60x |
| 2 | 5500 | 2.13x |
| 4 | 5028 | 2.33x |

**结论**：
- 从 0→2 workers 加速比线性（1x → 2.13x），说明前期几乎无锁竞争。
- 从 2→4 workers 边际收益骤降（2.13x → 2.33x），瓶颈从并行不足转向别处。
- Wait event 快照为空，说明当前规模下 LWLock 竞争还没构成主导瓶颈；
  实际瓶颈更可能是**图连接阶段的 CPU 计算**或**共享内存分配的顺序化**。

这个规模下没必要动锁粒度。要看到真实锁热点，需要跑到 500k+ 行且
`maintenance_work_mem` 相对不足的场景。Phase 4 交付脚手架，不改代码。

## 下一步

- 扩大到 500k~1M 行，配合 `pg_prewarm` + `pg_buffercache` 观测冷/热切换。
- 引入 `EXPLAIN (ANALYZE, BUFFERS)` 抓 `shared read` / `shared hit` 差值，
  验证 prefetch 命中率。
- 加入 `hot_layer` sweep，确认高层 vs 低层的预取权衡曲线。
- 打包成 `run_bench.ps1` 全自动 sweep，输出 CSV + 图。

## 复现步骤

```powershell
# 1) 建库 + 建 ground truth（约 1-3 分钟，取决于机器）
$env:PGPASSWORD = "<postgres pw>"
& "F:\postgresql\bin\psql.exe" -U postgres -d postgres -X -q -P pager=off `
    --set=ON_ERROR_STOP=1 `
    --set=n_rows=50000 --set=dim=128 --set=n_queries=50 `
    --set=top_k=10 --set=hnsw_m=16 --set=hnsw_efc=64 `
    -f test/hot_cold/bench/setup.sql

# 2) 跑一个配置（off / on 各一次）
& "F:\postgresql\bin\psql.exe" -U postgres -d postgres -X -q -P pager=off `
    --set=ON_ERROR_STOP=1 --set=top_k=10 `
    --set=ef_search=40 --set=hot_cold=off `
    --set=hot_layer=2 --set=prefetch_neighbors=16 --set=warmup=1 `
    -f test/hot_cold/bench/run_one.sql

& "F:\postgresql\bin\psql.exe" -U postgres -d postgres -X -q -P pager=off `
    --set=ON_ERROR_STOP=1 --set=top_k=10 `
    --set=ef_search=40 --set=hot_cold=on `
    --set=hot_layer=2 --set=prefetch_neighbors=16 --set=warmup=1 `
    -f test/hot_cold/bench/run_one.sql
```
