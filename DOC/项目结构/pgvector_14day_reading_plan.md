# pgvector 14天逐日源码阅读计划（新手版）

> 目标：用 14 天建立“能看懂 + 能定位 + 能改小功能 + 能验证”的源码能力。  
> 方法：每天固定 5 步——**目标 -> 必读文件 -> 关键符号 -> 实践任务 -> 验收标准**。  
> 建议时长：每天 60-90 分钟（进阶可扩展到 2-3 小时）。

---

## 使用方式（先看）

- 每天结束时，至少提交一份自己的笔记（1 页即可）
- 每天必须完成一个“可验证动作”（跑测试、写 SQL、画流程图、解释函数）
- 如果当天内容偏难，优先保证：**读懂调用链 + 跑通一个验证**

---

## Day 1：建立全局地图（先不钻算法细节）

**目标**
- 认清 pgvector 在 PostgreSQL 扩展体系中的位置

**必读文件**
- `README.md`
- `pgvector_architecture.md`
- `vector.control`
- `sql/vector.sql`

**关键符号/概念**

- 扩展安装、类型注册、操作符、操作符类（opclass）、访问方法（AM）

**实践任务**
- 画一张一页图：`SQL -> operator -> opclass -> index AM handler`

**验收标准**
- 你能口头说明：为什么 `ORDER BY embedding <-> query LIMIT k` 能走向向量索引

---

## Day 2：向量类型基础（数据怎么存）

**目标**
- 看懂 `vector` 的内部表示和输入输出流程

**必读文件**
- `src/vector.h`
- `src/vector.c`

**关键符号/函数**
- `Vector`
- `InitVector`
- `vector_in`
- `vector_out`

**实践任务**
- 用自己的话写下 `vector_in -> internal representation -> vector_out` 的流程

**验收标准**
- 能解释维度检查、内存分配、错误路径大致在哪些位置

---

## Day 3：距离函数与运算符映射

**目标**
- 看懂距离计算函数如何对接 SQL 运算符

**必读文件**
- `src/vector.c`
- `sql/vector.sql`
- `test/sql/vector_type.sql`
- `test/expected/vector_type.out`

**关键符号/函数**
- `l2_distance`
- `inner_product`
- `cosine_distance`
- `l1_distance`
- `<->`, `<#>`, `<=>`, `<+>`

**实践任务**
- 建一张“运算符 -> C 函数 -> 语义”对照表

**验收标准**
- 能回答：为什么不同距离函数会影响索引策略/排序行为

---

## Day 4：多类型扩展（halfvec / sparsevec / bit）

**目标**
- 理解 pgvector 不止 `vector` 一种表示

**必读文件**
- `src/halfvec.h`, `src/halfvec.c`
- `src/sparsevec.h`, `src/sparsevec.c`
- `src/bitvec.h`, `src/bitvec.c`
- `src/halfutils.c`
- `src/bitutils.c`

**关键符号/概念**

- 紧凑表示、稀疏表示、位向量表示

**实践任务**
- 对比三类类型的“空间占用/计算路径/适用场景”

**验收标准**

- 能说明：何时选 `vector`，何时考虑其他类型

---

## Day 5：进入 HNSW（先看接口和结构体）

**目标**

- 建立 HNSW 模块全局认知

**必读文件**
- `src/hnsw.h`
- `src/hnsw.c`

**关键符号/函数**
- `hnswhandler`
- `HnswMetaPageData`
- `HnswElementData`

**实践任务**
- 画 HNSW 索引元信息与元素关系草图（层级、邻接）

**验收标准**
- 能说清：HNSW 为什么是“分层近邻图”

---

## Day 6：HNSW 建索引流程

**目标**
- 看懂 HNSW build 的主路径

**必读文件**

- `src/hnswbuild.c`
- `src/hnswutils.c`

**关键符号/函数**
- `hnswbuild`
- 构建阶段的邻居选择、层级处理

**实践任务**
- 追踪一次 build 调用链，写 10 行以内流程摘要

**验收标准**
- 能回答：构建参数（如 `m`, `ef_construction`）大致影响什么

---

## Day 7：HNSW 插入与在线维护

**目标**
- 理解新增数据如何并入现有图

**必读文件**
- `src/hnswinsert.c`
- `src/hnswutils.c`

**关键符号/函数**
- `hnswinsert`
- 邻居更新逻辑

**实践任务**
- 写出“插入时为什么需要从高层到低层搜索”的原因

**验收标准**
- 能解释增量插入与重建索引的差异和代价

---

## Day 8：HNSW 查询与参数调优

**目标**
- 看懂查询主循环和召回/延迟权衡

**必读文件**
- `src/hnswscan.c`
- `src/hnsw.c`
- `test/sql/hnsw_vector.sql`
- `test/expected/hnsw_vector.out`

**关键符号/函数**
- `hnswgettuple`
- `HnswSearchLayer`
- `hnsw.ef_search`

**实践任务**
- 总结：`ef_search` 增大后，通常对延迟和召回有什么影响

**验收标准**
- 能纠正误区：`ef_search` 不是 `LIMIT`

---

## Day 9：HNSW 测试与成本模型

**目标**
- 把“看懂代码”转成“能验证行为”

**必读文件**
- `test/t/012_hnsw_vector_build_recall.pl`
- `test/t/039_hnsw_cost.pl`
- `src/hnswvacuum.c`

**关键符号/概念**
- recall、cost、vacuum 对索引维护的意义

**实践任务**
- 记录一次测试输出中的关键指标（至少 2 个）

**验收标准**
- 能说明：为什么 ANN 索引必须关注 recall 而不只看耗时

---

## Day 10：进入 IVFFlat（先总览）

**目标**
- 建立 IVFFlat 模块总体心智图

**必读文件**
- `src/ivfflat.h`
- `src/ivfflat.c`

**关键符号/函数**
- `ivfflathandler`
- `IvfflatMetaPageData`
- `IvfflatListData`

**实践任务**
- 画“训练期（聚类）与查询期（探测列表）”双阶段图

**验收标准**
- 能解释 IVFFlat 的核心思想：先粗分桶，再桶内精排

---

## Day 11：IVFFlat 构建与 KMeans

**目标**
- 看懂为什么 build 前要训练中心点

**必读文件**
- `src/ivfbuild.c`
- `src/ivfkmeans.c`

**关键符号/函数**
- `ivfflatbuild`
- `IvfflatKmeans`

**实践任务**
- 用 5-8 句话说明：`lists` 设大/设小各自影响

**验收标准**
- 能说清：`lists` 决定索引粒度，不是查询覆盖度

---

## Day 12：IVFFlat 查询与 probes 参数

**目标**
- 掌握查询阶段的探测策略

**必读文件**
- `src/ivfscan.c`
- `src/ivfutils.c`
- `test/sql/ivfflat_vector.sql`
- `test/expected/ivfflat_vector.out`

**关键符号/函数**
- `GetScanLists`
- `GetScanItems`
- `ivfflat.probes`

**实践任务**
- 写出 `lists` 与 `probes` 的联动关系

**验收标准**
- 能解释：为什么只调 `probes` 往往不够

---

## Day 13：IVFFlat 测试、成本与维护

**目标**
- 对齐正确性、性能、维护三个维度

**必读文件**
- `test/t/003_ivfflat_vector_build_recall.pl`
- `test/t/040_ivfflat_cost.pl`
- `src/ivfinsert.c`
- `src/ivfvacuum.c`

**关键符号/概念**
- recall/cost 插入与维护开销

**实践任务**
- 对比 HNSW 与 IVFFlat 在“构建-查询-插入”上的体感差异

**验收标准**
- 能给出一个简短选型建议（面向你的业务场景）

---

## Day 14：大总结（形成你的“可复用方法论”）

**目标**
- 把零散知识组织成可复用的源码阅读框架

**必读文件**
- `HNSW.md`
- `IVFFlat.md`
- `test/hot_cold/README.md`
- `pgvector_architecture.md`

**关键产出**
- 一页对比表：HNSW vs IVFFlat
- 一页调参清单：目标（延迟/召回/内存）-> 优先参数
- 一页排障清单：常见误区与验证命令

**实践任务**
- 做一次“讲给别人听”的 10 分钟复盘

**验收标准**
- 你能独立回答三件事：
  1) 业务场景怎么选算法  
  2) 参数该从哪里开始调  
  3) 改代码后如何快速验证没退化

---

## 附：每日打卡模板（复制即用）

```markdown
## Day X 打卡
- 今日目标：
- 我读了哪些文件：
- 我确认的关键函数/结构体：
- 我做了什么验证：
- 我卡住的点：
- 明天准备：
```

---

## 常见卡点与建议

- 看不懂 PostgreSQL AM 回调：先回到 `sql/vector.sql` 对照 `hnswhandler` / `ivfflathandler`
- 被细节淹没：每次只追一条调用链，不要同时开 10 个文件
- 只看不验：每天至少跑一个测试或写一个最小 SQL 验证
- 追求“一次全懂”：先建立骨架，再补细节，重复三轮最有效

---

## 你完成 14 天后应该具备的能力

- 能快速定位 pgvector 中某个行为对应的源码位置
- 能解释 HNSW 与 IVFFlat 的核心差异和参数语义
- 能做小范围改动并用现有测试验证正确性
- 能围绕延迟/召回/资源做基本调优与取舍

