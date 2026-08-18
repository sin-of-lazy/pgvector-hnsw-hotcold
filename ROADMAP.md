# 开发 Roadmap

本项目当前状态：**v1.1.0（Phase 1-5 完成）**

## 已完成 ✅

- [x] Phase 1: Buffer Pool prefetch（查询路径 I/O 优化）
- [x] Phase 2: 半动态 ef_search（iterative scan 自动扩展）
- [x] Phase 3: 构建期 I/O 观测脚手架
- [x] Phase 4: 并行 build 锁竞争观测脚手架
- [x] Phase 5: 自适应 prefetch depth + entry prewarm
- [x] 完整 benchmark 框架（setup/run_one/probe_build/probe_parallel_build）
- [x] 技术报告（7 章，架构+算法+Phase 1-4 详解+实验）
- [x] 简历表述模板（中英双语）

## 近期计划（v1.2.0 候选）

### 实验增强
- [ ] **真实 embedding 数据集**：SIFT1M / GIST1M / text-embedding-ada-002
- [ ] **冷启动实验**：清空 buffer pool 后测量首次查询延迟（`pg_prewarm` 对照组）
- [ ] **大规模测试**：500k ~ 1M 行，验证 prefetch 在真实 I/O 压力下的收益
- [ ] **HDD vs SSD**：机械盘场景下 prefetch 收益可能翻倍

### 参数调优
- [ ] **`hot_layer` sweep**：测试 0/1/2/3/4 层的最优配置
- [ ] **`prefetch_neighbors` 敏感性**：4/8/16/32/64 的 recall-latency 权衡曲线
- [ ] **`ef_search_multiplier` 优化**：1.5/2.0/3.0 对 iterative scan 的影响

### 工程完善
- [ ] **回归测试增强**：在 `test/sql/` 加 Phase 1-5 的功能测试
- [ ] **性能监控 GUC**：`hnsw.prefetch_hit_rate`（统计预取命中率，用于调优）
- [ ] **文档翻译**：英文版 README / 技术报告
- [ ] **Docker 镜像**：一键部署测试环境

## 中期探索（v1.3.0+）

### 算法增强（需深入验证）
- [ ] **Phase 6: 距离加权预取**：优先预取距离小的邻居（需要在 `unvisited` 预先计算部分距离，权衡 CPU vs I/O）
- [ ] **Phase 7: 层内缓存**：将 hot layer 的节点缓存在共享内存（需解决 VACUUM 一致性）
- [ ] **Phase 8: Async I/O batch**：将多次 `PrefetchBuffer` 合并为批量 async I/O（需改 PG 内核接口）

### 与上游社区交互
- [ ] **提 RFC 到 pgvector**：Phase 1 的 prefetch 作为可选特性
- [ ] **贡献 benchmark 工具**：`test/hot_cold/bench/` 可以独立成通用测试框架
- [ ] **参与 pgvector 性能优化讨论**：GitHub Discussions / Issues

### 横向对比
- [ ] **Milvus / Weaviate / Qdrant**：在相同数据集上对比 QPS / p99 / Recall
- [ ] **DiskANN**：与微软 DiskANN 的 prefetch 策略对比
- [ ] **发表 blog / 论文**：总结 PostgreSQL 生态中向量索引优化的工程实践

## 长期愿景

- 成为 pgvector 社区认可的**生产级优化参考实现**
- 为 PostgreSQL + AI 应用提供**开箱即用的高性能向量检索方案**
- 推动 PostgreSQL 内核增强对 ANN 索引的支持（如更灵活的 prefetch API）

---

## 如何贡献

欢迎：
- 提 Issue 报告 bug 或性能回退
- 提 PR 贡献新的 benchmark 数据（不同硬件/数据集/场景）
- 分享真实生产环境的使用经验
- 翻译文档（英文/日文/其他语言）

**贡献指南**：见 `CONTRIBUTING.md`（待补充）

---

**最后更新**：2026-08-18  
**当前版本**：v1.1.0  
**下一版本预计**：v1.2.0（2026-09 ~ 10，包含真实数据集实验）
