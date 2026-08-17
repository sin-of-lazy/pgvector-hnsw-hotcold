# 项目更新日志

本项目的版本变更记录（区别于官方 `CHANGELOG.md`）。

## v1.1.0 (2024-08-17)

### 新增 - Phase 5: 自适应预取增强

- **Phase 5a - Adaptive Prefetch Depth**：新增 `hnsw.prefetch_adaptive` GUC，
  开启后按 `min(2m, max(prefetch_neighbors, ef_search/4))` 动态计算预取深度。
  高 ef_search 场景下预取更积极。
  
- **Phase 5b - Entry Point Prewarm**：新增 `hnsw.prewarm_entry` GUC，
  在 `GetScanItems` 拿到 entry point 后立即 `PrefetchBuffer`，
  减少冷启动首次 buffer miss。

### 代码改动

- `src/hnsw.h`：新增 2 个 GUC extern 声明
- `src/hnsw.c`：定义并注册 `hnsw.prefetch_adaptive` 和 `hnsw.prewarm_entry`
- `src/hnswutils.c`：`HnswSearchLayer` 中根据 `prefetch_adaptive` 动态计算 depth
- `src/hnswscan.c`：`GetScanItems` 中根据 `prewarm_entry` 预取 entry point

### 兼容性

- 所有新 GUC 默认 `off`，行为完全等价 v1.0.0
- 可与 Phase 1/2 优化叠加使用
- 零磁盘格式改动，零 API 破坏

### 收益场景

- **冷启动/大索引**：entry prewarm 显著降低首次查询 miss
- **高 ef_search（≥100）**：adaptive depth 让预取深度更贴合实际访问量
- **温热 buffer pool**：本次实验（200k 行，热缓存）收益 ~6%，
  真正价值在冷启动/百万级索引场景

### 测试

- 200k 行 128 维 + ef_search=200
- Recall@10 = 0.478（与 v1.0 完全一致）
- avg 14.67 ms（相比 baseline off 的 15.68 ms，-6.4%）

---

## v1.0.0 (2024-08-16)

### 新增 - Phase 1-4 核心优化

- **Phase 1**: Buffer Pool 预取
  - 新增 `hnsw.hot_cold_enabled`、`hnsw.hot_layer`、`hnsw.hot_max_bytes`、`hnsw.prefetch_neighbors`
  - 在 `HnswSearchLayer` 加载邻居前发起 `PrefetchBuffer`
  - hot/cold 策略：高层激进预取，低层限流到 4

- **Phase 2**: 半动态 ef_search
  - 新增 `hnsw.ef_search_auto`、`hnsw.ef_search_multiplier`
  - `ResumeScanItems` 按倍率递增 batch_size

- **Phase 3**: 构建期 I/O 观测脚本（无代码改动）
- **Phase 4**: 并行 build 锁竞争观测脚本（无代码改动）

### 基准数据

200k 行 128 维 + ef_search=200：

| 指标 | off | on | 改善 |
|---|---:|---:|---:|
| Recall@10 | 0.478 | 0.478 | 0% |
| avg | 22.95 ms | 15.42 ms | **-32.8%** |
| p99 | 48.09 ms | **21.23 ms** | **-55.9%** |

### 文档

- `README.md`：面向 AI Infra 的详尽项目介绍
- `DOC/技术报告/`：7 章完整技术报告
- `DOC/优化/benchmark_report.md`：实验数据汇总
- `DOC/简历与描述.md`：多种格式的项目表述

### 代码规模

- 核心改动 ~100 行（zero-risk 增量）
- 4 个源文件（`hnsw.h/c`, `hnswutils.c`, `hnswscan.c`）
- 零磁盘格式改动
