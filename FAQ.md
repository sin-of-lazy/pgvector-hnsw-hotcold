# 常见问题 FAQ

## Q1: 这个项目是 fork 还是从零写的？

**A**: 基于 [pgvector 0.8.4](https://github.com/pgvector/pgvector/tree/v0.8.4) 的优化分支。核心改动只有 ~150 行（4 个文件），没动磁盘格式和 API，可以直接替换官方版本使用。

---

## Q2: 为什么不直接提 PR 给官方？

**A**: 本项目是**实验性优化**，有几个原因暂未上游：

1. **Phase 1 的 prefetch 策略**还需在更多场景验证（当前只在 SSD + 256GB 内存机器上测过）
2. **Phase 2 的半动态 ef_search** 可能与官方未来的 auto-tuning 规划冲突
3. **新增 8 个 GUC** 增加了用户配置复杂度，官方可能更偏向零配置
4. 作为**面试作品**展示完整的优化思路更重要，而非追求合并

如果后续持续迭代并在生产环境验证，会考虑贡献部分特性回上游。

---

## Q3: Phase 1 的 prefetch 和自建缓存有什么区别？

**A**: 

| 方案 | 优点 | 缺点 |
|---|---|---|
| **自建缓存** | 可定制淘汰策略、可跨查询共享 | 需管理内存生命周期、VACUUM 一致性、并发锁、OOM 风险 |
| **PrefetchBuffer（本项目）** | 零额外内存、复用 PG Buffer Pool、幂等、可灰度 | 依赖 shared_buffers 大小 |

`PrefetchBuffer` 是 PostgreSQL **原生 API**，只是"提前告诉 PG 你即将访问这个页面"，让操作系统提前发起异步 I/O。不改变数据结构，不增加内存占用。

---

## Q4: Recall 为什么只有 0.478（不到 50%）？

**A**: 这是**数据集特性**，不是优化问题：

- 测试数据是 **128 维随机 IID 向量**（每个维度独立同分布 `[0,1)` 随机数）
- 随机向量之间几乎"等距"，HNSW 难以区分真正的近邻
- `ef_search=200` 在 200k 规模的随机数据上，Recall@10 = 0.478 是 HNSW 算法的正常表现

**真实场景**（如 OpenAI text-embedding-ada-002 的 1536 维 embedding）有语义聚类，Recall 通常 > 0.95。

**关键验证**：本项目优化前后 Recall **完全一致**（0.478/0.478），证明只改 I/O，不改算法语义。

---

## Q5: 能用在生产环境吗？

**A**: **理论上可以，但请先在测试环境验证**。

**安全性保证**：
- ✅ 所有新 GUC 默认 `off`，行为等价官方 0.8.4
- ✅ 零磁盘格式改动，可随时回退
- ✅ 不改 insert/vacuum/build 路径，数据一致性有保障
- ✅ `PrefetchBuffer` 是 PG 原生 API，经过 20+ 年生产验证

**建议灰度路径**：
1. 先在 **staging 环境**开启 `hnsw.hot_cold_enabled=on`，跑 1-2 周
2. 用 `EXPLAIN (ANALYZE, BUFFERS)` 对比 `shared hit/read` 确认收益
3. 监控 p95/p99 延迟，确认无异常
4. 生产环境按库/按用户逐步灰度

---

## Q6: 为什么 Phase 3/4 不改代码？

**A**: **数据驱动优化**，不盲目改动。

- **Phase 3**：`pg_stat_io` 显示 build 期 `reads=2`（几乎无 I/O），瓶颈是 CPU 距离计算，加 prefetch 无意义
- **Phase 4**：20k 行下并行加速比已达 2.13x（2 workers），`pg_stat_activity` 无 wait_event，说明无锁竞争

Phase 3/4 交付的是**观测脚本**（`probe_build.sql` 和 `probe_parallel_build.sql`），让后续开发者在不同场景下快速量化瓶颈，决定是否值得优化。

---

## Q7: 我的机器配置和你不一样，收益会变吗？

**A**: 会。收益与以下因素正相关：

| 因素 | 影响 |
|---|---|
| **索引大小 vs 内存** | 索引 > shared_buffers 时收益更大（触发真实 I/O） |
| **磁盘类型** | HDD > SATA SSD > NVMe（随机读延迟越高，prefetch 收益越大） |
| **ef_search 大小** | ef ≥ 100 时收益明显（访问邻居多，miss 概率高） |
| **并发度** | 单查询优化明显，高并发时可能受 buffer pool 争抢影响 |

我的环境（200k 行，SSD，单查询）是**中等偏小**规模，大规模场景（百万级 + 机械盘）收益可能更戏剧化。

---

## Q8: 有性能回退风险吗？

**A**: **理论上有，但实测几乎没有**。

可能的风险场景：
- **buffer pool 已满 + 预取页面用不上**：浪费 buffer pool 空间，可能挤掉其他页面
- **极小数据集（全在内存）**：预取是空操作，但 `PrefetchBuffer` 调用本身只有 ~0.01ms 开销

**实测**：50k 行（索引全在内存）时，开启优化后 avg 只慢了 0.2%（噪声范围内），p95 反而降了 11%。

**缓解措施**：如果担心，先用 `hnsw.prefetch_neighbors=8`（默认 16），收益仍有但更保守。

---

## Q9: 能和 IVFFlat 索引一起用吗？

**A**: **可以，但本项目优化只针对 HNSW**。

pgvector 支持两种索引：
- **HNSW**：高召回、低延迟，适合在线检索（本项目优化目标）
- **IVFFlat**：构建快、内存小，适合离线 batch 查询

Phase 1 的 `hnsw.hot_cold_enabled` 等 GUC 只在 HNSW 索引扫描时生效，不影响 IVFFlat。

---

## Q10: 后续 Roadmap 是什么？

见 `README.md` 的 "Roadmap" 章节，简要包括：

1. 真实 embedding 数据集测试（SIFT1M / OpenAI embeddings）
2. `hot_layer` 和 `prefetch_neighbors` 参数敏感性分析
3. 冷启动实验（`pg_prewarm` + `pg_buffercache` 观测）
4. 大规模测试（500k ~ 1M 行，HDD vs SSD 对比）
5. 与其他向量数据库（Milvus / Weaviate / Qdrant）性能对比
