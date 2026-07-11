# pgvector 优化实现文件导览

本文档用于回答一个核心问题：

> 如果要完成 pgvector 的 HNSW 查询优化，需要修改哪些文件？这些文件各自负责什么？它们又是如何和 PostgreSQL 协作的？

当前项目已经融合到 pgvector 0.8.4。后续我们自己的优化方向建议聚焦在：

- HNSW 查询路径 neighbor prefetch
- HNSW hot/cold GUC 开关
- 基于 Buffer Pool 行为的冷热分层实验
- Recall / latency / buffers benchmark 验证

---

## 1. PostgreSQL 与 pgvector 的协作流程

pgvector 不是一个独立数据库，而是 PostgreSQL 的扩展。

PostgreSQL 负责：

- SQL 解析
- 查询优化
- 执行计划生成
- 表数据存储
- MVCC 可见性判断
- Buffer Pool 管理
- WAL / VACUUM / 并发控制

pgvector 负责：

- 定义 `vector`、`halfvec`、`sparsevec`、`bit` 等向量相关类型
- 定义距离函数和操作符
- 注册 HNSW / IVFFlat 索引访问方法
- 在 PostgreSQL 调用索引时，完成向量近邻检索

整体调用链可以理解为：

```text
用户 SQL
  -> PostgreSQL Parser / Planner
  -> 识别 ORDER BY embedding <-> query LIMIT k
  -> Planner 判断是否使用 HNSW / IVFFlat 索引
  -> Executor 调用 pgvector 的 Index AM 回调
  -> pgvector 在 HNSW 图中搜索候选向量
  -> 返回 heap TID
  -> PostgreSQL 根据 heap TID 回表并做 MVCC 可见性判断
  -> 返回结果
```

典型 SQL：

```sql
SELECT id
FROM items
ORDER BY embedding <-> '[1,2,3]'
LIMIT 10;
```

如果建立了 HNSW 索引：

```sql
CREATE INDEX ON items USING hnsw (embedding vector_l2_ops);
```

PostgreSQL 可能会选择 HNSW Index Scan。此时真正的近邻搜索逻辑由 pgvector 的 HNSW 模块执行。

---

## 2. 文件分层总览

项目中的关键文件可以按职责分为 6 层。

```text
SQL 扩展声明层
  vector.control
  sql/vector.sql
  sql/vector--*.sql

扩展入口层
  src/vector.c

类型与距离函数层
  src/vector.c / src/vector.h
  src/halfvec.c / src/halfvec.h
  src/sparsevec.c / src/sparsevec.h
  src/bitvec.c / src/bitvec.h
  src/halfutils.c / src/bitutils.c

HNSW 索引层
  src/hnsw.c
  src/hnsw.h
  src/hnswbuild.c
  src/hnswinsert.c
  src/hnswscan.c
  src/hnswutils.c
  src/hnswvacuum.c

IVFFlat 索引层
  src/ivfflat.c
  src/ivfflat.h
  src/ivfbuild.c
  src/ivfinsert.c
  src/ivfscan.c
  src/ivfutils.c
  src/ivfkmeans.c
  src/ivfvacuum.c

测试与验证层
  test/sql
  test/expected
  test/t
  test/hot_cold
```

我们的 HNSW 优化主要会集中在：

```text
src/hnsw.h
src/hnsw.c
src/hnswscan.c
src/hnswutils.c
test/hot_cold
```

如果涉及 SQL 函数、诊断视图或扩展版本升级，才需要改：

```text
sql/vector.sql
sql/vector--0.8.4--自定义版本.sql
vector.control
```

---

## 3. SQL 扩展声明层

### 3.1 `vector.control`

作用：

- 告诉 PostgreSQL 这是一个扩展
- 声明默认版本
- 声明动态库路径

关键内容：

```text
default_version = '0.8.4'
module_pathname = '$libdir/vector'
```

PostgreSQL 执行：

```sql
CREATE EXTENSION vector;
```

时，会读取 `vector.control`，然后执行对应版本的 SQL 文件。

优化时是否需要修改：

- 只改 C 代码和 GUC：通常不需要
- 新增 SQL 函数、视图、操作符：需要
- 改扩展版本号：需要

### 3.2 `sql/vector.sql`

作用：

- 创建向量类型
- 创建距离函数
- 创建操作符
- 创建 HNSW / IVFFlat operator class
- 把 C 函数暴露给 SQL

例如：

```sql
CREATE TYPE vector;
CREATE FUNCTION l2_distance(vector, vector) RETURNS float8 ...
CREATE OPERATOR <-> ...
CREATE OPERATOR CLASS vector_l2_ops FOR TYPE vector USING hnsw ...
```

优化时是否需要修改：

- 只做 HNSW 内部查询优化：通常不需要
- 新增 SQL 可见的调试函数：需要
- 新增统计函数：可能需要

### 3.3 `sql/vector--*.sql`

作用：

- 支持扩展升级

例如：

```sql
ALTER EXTENSION vector UPDATE;
```

PostgreSQL 会根据当前版本和目标版本执行对应的升级脚本。

优化时是否需要修改：

- 如果我们保持实验分支，不发布新扩展版本：可以先不改
- 如果要把项目包装成正式版本：建议新增升级脚本

示例：

```text
sql/vector--0.8.4--0.8.4-hotcold.sql
```

---

## 4. 扩展入口层

### 4.1 `src/vector.c`

作用：

- pgvector 扩展的入口之一
- 定义基础 vector 类型的输入、输出、距离计算
- 在 `_PG_init()` 中初始化 HNSW 和 IVFFlat

关键理解：

`vector.c` 不是 HNSW 查询主逻辑，但它负责让整个扩展被 PostgreSQL 加载。

典型关系：

```text
PostgreSQL 加载 vector.dll
  -> 调用 _PG_init()
  -> HnswInit()
  -> IvfflatInit()
```

优化时是否需要修改：

- 注册新的 HNSW GUC：一般不直接在这里改，而是在 `HnswInit()` 中改
- 新增 vector 距离函数：需要
- 新增全局扩展初始化逻辑：可能需要

我们的 hot/cold 优化通常不需要直接修改 `vector.c`。

---

## 5. HNSW 核心文件

### 5.1 `src/hnsw.h`

作用：

- HNSW 的核心头文件
- 定义 HNSW 常量
- 定义 HNSW 内存结构
- 声明 HNSW 全局 GUC 变量
- 声明 HNSW 函数接口

后续优化需要在这里做的事：

1. 声明新的 GUC 全局变量

```c
extern bool hnsw_hot_cold_enabled;
extern int hnsw_hot_layer;
extern int hnsw_hot_max_bytes;
extern int hnsw_prefetch_neighbors;
```

2. 如果扫描状态需要保存优化信息，扩展 `HnswScanOpaqueData`

可能新增：

```c
bool hotColdEnabled;
int hotLayer;
int prefetchNeighbors;
```

3. 如果做热缓存，需要新增缓存结构体

例如：

```c
typedef struct HnswHotCacheData
{
    HTAB *table;
    Size usedBytes;
    Size maxBytes;
    int64 hits;
    int64 misses;
} HnswHotCacheData;
```

第一阶段建议只声明 GUC，不急着做复杂缓存结构。

### 5.2 `src/hnsw.c`

作用：

- 注册 HNSW Index Access Method
- 注册 HNSW GUC 参数
- 注册索引 reloptions
- 实现 planner cost estimate

PostgreSQL 协作点：

```text
CREATE INDEX ... USING hnsw
  -> hnswhandler()
  -> 返回 IndexAmRoutine
  -> PostgreSQL 知道 HNSW 支持哪些回调函数
```

重要函数：

```c
HnswInit()
hnswhandler()
hnswcostestimate()
hnswoptions()
```

后续优化需要在这里做的事：

1. 定义新的 GUC 全局变量

```c
bool hnsw_hot_cold_enabled = false;
int hnsw_hot_layer = 2;
int hnsw_hot_max_bytes = 64 * 1024;
int hnsw_prefetch_neighbors = 16;
```

2. 在 `HnswInit()` 中注册参数

```c
DefineCustomBoolVariable("hnsw.hot_cold_enabled", ...);
DefineCustomIntVariable("hnsw.hot_layer", ...);
DefineCustomIntVariable("hnsw.hot_max_bytes", ...);
DefineCustomIntVariable("hnsw.prefetch_neighbors", ...);
```

3. 如果优化影响 planner 代价，可以调整 `hnswcostestimate()`

第一阶段不建议改 cost estimate。先保证行为稳定，再考虑 planner 侧优化。

### 5.3 `src/hnswscan.c`

作用：

- HNSW 查询扫描入口
- PostgreSQL Executor 执行 HNSW Index Scan 时，会调用这里的回调

关键函数：

```c
hnswbeginscan()
hnswrescan()
hnswgettuple()
hnswendscan()
GetScanItems()
ResumeScanItems()
```

典型查询流程：

```text
hnswbeginscan()
  分配 HnswScanOpaque

hnswrescan()
  接收查询向量

hnswgettuple()
  第一次调用时触发 HNSW 搜索
  后续调用逐个返回候选 heap TID

GetScanItems()
  从入口点开始，逐层搜索
  最后调用 HnswSearchLayer()
```

后续优化需要在这里做的事：

1. 在 `hnswbeginscan()` 中保存当前 GUC 快照

原因：

- GUC 是会话级参数
- 单次扫描过程中最好使用稳定配置

例如：

```c
so->hotColdEnabled = hnsw_hot_cold_enabled;
so->hotLayer = hnsw_hot_layer;
so->prefetchNeighbors = hnsw_prefetch_neighbors;
```

2. 在 `GetScanItems()` 调用 `HnswSearchLayer()` 时传递优化参数

当前 `HnswSearchLayer()` 参数已经很多，是否继续加参数要谨慎。

可选方案：

- 方案 A：给 `HnswSearchLayer()` 新增参数
- 方案 B：把优化参数放进一个小结构体
- 方案 C：先在 `HnswSearchLayer()` 里直接读取 GUC

第一阶段为了低风险，可以直接读取 GUC。

第二阶段建议使用结构体，避免函数参数继续膨胀。

### 5.4 `src/hnswutils.c`

作用：

- HNSW 算法核心
- 加载元素
- 加载邻居
- 计算距离
- 执行单层图搜索

关键函数：

```c
HnswSearchLayer()
HnswLoadElement()
HnswLoadElementImpl()
HnswLoadNeighborTids()
HnswLoadUnvisitedFromDisk()
GetElementDistance()
```

这是我们优化最核心的文件。

当前 HNSW 搜索大致流程：

```text
HnswSearchLayer()
  从候选堆 C 中取最近候选
  读取该候选的邻居列表
  对未访问邻居逐个加载元素
  计算距离
  更新候选堆 C / W
```

优化点 1：neighbor prefetch

推荐落点：

```c
HnswLoadUnvisitedFromDisk()
```

原因：

- 这个函数已经拿到了当前节点的邻居 `indextids`
- 每个 neighbor tid 里包含 block number
- 可以在真正加载邻居元素前，对未来要访问的 block 发起 `PrefetchBuffer`

伪代码：

```c
if (hnsw_hot_cold_enabled && hnsw_prefetch_neighbors > 0)
{
    for (int i = 0; i < Min(lm, hnsw_prefetch_neighbors); i++)
    {
        BlockNumber blkno = ItemPointerGetBlockNumber(&indextids[i]);
        PrefetchBuffer(index, MAIN_FORKNUM, blkno);
    }
}
```

需要引入：

```c
#include "storage/bufmgr.h"
```

注意：

- `PrefetchBuffer` 只是提示，不保证一定命中
- 不能改变搜索结果
- 不能越界访问无效 `ItemPointer`
- 需要跳过 `InvalidBlockNumber`

优化点 2：hot layer 策略

可能落点：

```c
HnswSearchLayer()
HnswLoadUnvisitedFromDisk()
HnswLoadElementImpl()
```

思路：

- 高层 `lc >= hnsw_hot_layer` 时更积极预取
- 第 0 层数据量最大，访问更分散，可以限制预取数量

第一版可以做成：

```text
只在 lc >= hot_layer 时 prefetch 更多邻居
lc < hot_layer 时 prefetch 数量降低或关闭
```

这比一上来做热缓存更稳。

优化点 3：热缓存

可能落点：

```c
HnswLoadElementImpl()
```

但不建议第一阶段就做。

原因：

- PostgreSQL 已经有 Buffer Pool
- 自己缓存元素容易引入内存生命周期、MVCC、并发、vacuum 一致性问题
- 简历项目第一版更适合做 prefetch + benchmark

### 5.5 `src/hnswbuild.c`

作用：

- HNSW 索引构建
- `CREATE INDEX ... USING hnsw` 时使用

关键函数：

```c
hnswbuild()
HnswBuildCallback()
HnswParallelBuildMain()
```

后续可能优化：

- 构建阶段 tuple 预取
- 并行 build 锁竞争降低
- build 内存占用观测

目前不建议第一阶段修改。

原因：

- 构建路径复杂
- 并发和 WAL 风险更高
- 查询路径优化更贴 RAG / AI Agent 在线检索热点

### 5.6 `src/hnswinsert.c`

作用：

- 单条 INSERT / UPDATE 时维护 HNSW 索引

关键函数：

```c
hnswinsert()
HnswInsertTuple()
HnswInsertTupleOnDisk()
```

后续可能优化：

- 插入时锁粒度优化
- 插入时图连接策略优化
- vacuum 并发期间插入稳定性验证

目前不建议第一阶段修改。

### 5.7 `src/hnswvacuum.c`

作用：

- HNSW VACUUM 清理
- 删除失效 heap TID
- 修复图连接
- 标记被删除节点

0.8.3 / 0.8.4 的重要修复主要集中在这里。

后续优化一般不要轻易动这个文件。

原因：

- 这里涉及索引正确性
- 改错会导致图损坏
- 和 vacuum / insert / scan 并发强相关

我们的 hot/cold 查询优化第一阶段不需要改它。

---

## 6. IVFFlat 文件

虽然当前优化主线建议押 HNSW，但理解 IVFFlat 有助于对比。

### 6.1 `src/ivfflat.c`

作用：

- 注册 IVFFlat Index Access Method
- 注册 IVFFlat GUC
- 实现 planner cost estimate

类似 HNSW 的 `src/hnsw.c`。

### 6.2 `src/ivfbuild.c`

作用：

- IVFFlat 索引构建
- 抽样
- K-means 聚类
- 分配向量到 list
- 写入磁盘

0.8.4 / 0.8.5 相关内存优化主要与这里有关。

### 6.3 `src/ivfscan.c`

作用：

- IVFFlat 查询扫描

IVFFlat 查询路径：

```text
选择 probes 个最近 list
扫描这些 list 中的向量
计算距离
返回 top-k
```

如果未来做 HNSW vs IVFFlat benchmark，会重点看：

```text
src/hnswscan.c / src/hnswutils.c
src/ivfscan.c / src/ivfutils.c
```

---

## 7. 推荐的优化实现顺序

### Phase 0：确认官方 0.8.4 基线

目标：

- 能编译
- 能安装
- 能 `CREATE EXTENSION vector`
- 能跑基础 HNSW 查询

涉及文件：

```text
无代码修改
```

验证：

```sql
CREATE EXTENSION vector;
CREATE TABLE items (id bigserial, embedding vector(3));
CREATE INDEX ON items USING hnsw (embedding vector_l2_ops);
EXPLAIN ANALYZE SELECT * FROM items ORDER BY embedding <-> '[1,2,3]' LIMIT 10;
```

### Phase 1：增加 hot/cold GUC

目标：

- 让参数存在
- 测试脚手架不再 skip
- 不改变查询结果

修改文件：

```text
src/hnsw.h
src/hnsw.c
test/sql/hnsw_vector.sql
test/expected/hnsw_vector.out
```

自定义测试：

```text
test/hot_cold/001_hot_cold_smoke.pl
```

新增参数：

```text
hnsw.hot_cold_enabled
hnsw.hot_layer
hnsw.hot_max_bytes
hnsw.prefetch_neighbors
```

### Phase 2：实现 neighbor prefetch

目标：

- 不改变结果
- 减少随机 I/O 等待
- 用 `EXPLAIN (ANALYZE, BUFFERS)` 观察 shared hit/read 和执行时间

修改文件：

```text
src/hnswutils.c
src/hnsw.h
src/hnsw.c
```

核心落点：

```text
HnswLoadUnvisitedFromDisk()
```

验证：

```text
test/hot_cold/001_hot_cold_smoke.pl
test/hot_cold/002_hot_cold_recall.pl
test/hot_cold/003_hot_cold_latency_probe.sql
test/hot_cold/004_hot_cold_buffers_probe.sql
```

### Phase 3：加入 hot layer 策略

目标：

- 高层节点更积极预取
- 底层节点控制预取规模
- 观察 P95 / P99 延迟是否稳定

修改文件：

```text
src/hnswutils.c
```

可能逻辑：

```text
lc >= hnsw_hot_layer
  使用 hnsw_prefetch_neighbors

lc < hnsw_hot_layer
  使用较小 prefetch 或关闭
```

### Phase 4：benchmark 报告

目标：

- 形成可用于简历和面试的结果闭环

新增或修改文件：

```text
DOC/优化/benchmark_report.md
test/hot_cold/*.sql
test/hot_cold/*.pl
```

指标：

```text
Recall@10
Avg latency
P95 latency
P99 latency
shared hit
shared read
index scan 是否稳定
```

---

## 8. 每个优化点与文件的对应关系

| 优化点 | 主要文件 | 是否第一阶段建议做 | 风险 |
| --- | --- | --- | --- |
| GUC 开关 | `src/hnsw.c`, `src/hnsw.h` | 是 | 低 |
| 查询参数读取 | `src/hnswscan.c` | 是 | 低 |
| neighbor prefetch | `src/hnswutils.c` | 是 | 中 |
| hot layer 策略 | `src/hnswutils.c` | 是 | 中 |
| 热缓存 | `src/hnswutils.c`, `src/hnswscan.c`, `src/hnsw.h` | 暂不建议 | 高 |
| 动态 ef_search | `src/hnswscan.c`, `src/hnsw.c` | 第二阶段可做 | 中 |
| planner cost 调整 | `src/hnsw.c` | 暂不建议 | 中 |
| build 阶段预取 | `src/hnswbuild.c` | 后续可做 | 中高 |
| 插入锁粒度优化 | `src/hnswinsert.c`, `src/hnswutils.c` | 暂不建议 | 高 |
| vacuum 逻辑修改 | `src/hnswvacuum.c` | 不建议轻易做 | 很高 |

---

## 9. 为什么第一版推荐 prefetch，而不是自建热缓存

PostgreSQL 已经有 Buffer Pool。

如果我们自己再做一套热缓存，需要处理：

- 缓存生命周期
- 内存上限
- 并发查询隔离
- VACUUM 后数据是否仍然有效
- INSERT / UPDATE 后图结构变化
- scan context 与 backend memory context

这些都容易把项目复杂度推高。

而 `PrefetchBuffer` 的优势是：

- 利用 PostgreSQL 原生 Buffer Pool
- 不改变数据结构
- 不改变搜索结果
- 容易灰度开关
- 容易 benchmark 验证

所以第一版优化建议定位为：

> 基于 HNSW 图遍历邻居访问模式的 Buffer Pool 预取优化。

这比“重新设计缓存”更严谨，也更适合作为数据库内核方向的简历项目。

---

## 10. PostgreSQL Index AM 回调对应表

HNSW 通过 `IndexAmRoutine` 注册给 PostgreSQL。

常见回调关系：

| PostgreSQL 事件 | pgvector HNSW 函数 | 所在文件 |
| --- | --- | --- |
| 创建索引 | `hnswbuild` | `src/hnswbuild.c` |
| 插入索引项 | `hnswinsert` | `src/hnswinsert.c` |
| 开始扫描 | `hnswbeginscan` | `src/hnswscan.c` |
| 重新扫描 | `hnswrescan` | `src/hnswscan.c` |
| 获取下一条结果 | `hnswgettuple` | `src/hnswscan.c` |
| 结束扫描 | `hnswendscan` | `src/hnswscan.c` |
| VACUUM 删除 | `hnswbulkdelete` | `src/hnswvacuum.c` |
| VACUUM 清理 | `hnswvacuumcleanup` | `src/hnswvacuum.c` |
| 估算索引代价 | `hnswcostestimate` | `src/hnsw.c` |
| 解析索引选项 | `hnswoptions` | `src/hnsw.c` |

理解这张表，就能理解 pgvector 如何嵌入 PostgreSQL。

---

## 11. 一个完整查询如何走到我们的优化代码

假设执行：

```sql
SET hnsw.hot_cold_enabled = on;
SET hnsw.prefetch_neighbors = 16;

SELECT id
FROM items
ORDER BY embedding <-> '[0.1,0.2,0.3]'
LIMIT 10;
```

调用链大致是：

```text
PostgreSQL Executor
  -> hnswbeginscan()
  -> hnswrescan()
  -> hnswgettuple()
  -> GetScanItems()
  -> HnswSearchLayer()
  -> HnswLoadUnvisitedFromDisk()
  -> PrefetchBuffer()
  -> HnswLoadElementImpl()
  -> 距离计算
  -> 返回候选 heap TID
  -> PostgreSQL 回表检查可见性
  -> 返回 SQL 结果
```

因此，我们第一版优化最关键的插入点是：

```text
HnswLoadUnvisitedFromDisk()
```

它处在“已经知道邻居 TID，但还没有逐个加载邻居元素”的位置，非常适合做预取。

---

## 12. 推荐阅读顺序

如果你要理解整个项目和 PostgreSQL 的协作流程，建议按这个顺序读：

```text
1. vector.control
2. sql/vector.sql
3. src/vector.c
4. src/hnsw.c
5. src/hnsw.h
6. src/hnswscan.c
7. src/hnswutils.c
8. src/hnswbuild.c
9. src/hnswinsert.c
10. src/hnswvacuum.c
11. test/sql/hnsw_vector.sql
12. test/hot_cold/README.md
```

不要一开始就钻进 `hnswutils.c` 的算法细节。先理解 PostgreSQL 如何调用 HNSW，再看 HNSW 如何搜索。

---

## 13. 最小实现检查清单

第一版完成后，应满足：

```text
编译通过
CREATE EXTENSION vector 成功
SHOW hnsw.hot_cold_enabled 成功
SHOW hnsw.hot_layer 成功
SHOW hnsw.hot_max_bytes 成功
SHOW hnsw.prefetch_neighbors 成功
test/hot_cold/001_hot_cold_smoke.pl 不再 skip
test/hot_cold/002_hot_cold_recall.pl Recall@10 >= 0.95
EXPLAIN (ANALYZE, BUFFERS) 可以对比 on/off
```

如果这些都满足，就可以说：

> 已完成 HNSW 查询路径 hot/cold prefetch 优化的第一版原型。

如果只完成 GUC，还不能说优化完成，只能说：

> 完成优化开关与验证框架接入。

---

## 14. 简历表达边界

当前还没有改源码前，只能写：

```text
设计 HNSW Buffer Pool 冷热分层优化方案，并搭建 recall / latency / buffer 行为验证脚手架。
```

实现 GUC 后可以写：

```text
为 pgvector HNSW 查询路径增加可灰度的 hot/cold 优化开关，并接入自动化 smoke / recall 测试。
```

实现 prefetch 后可以写：

```text
基于 HNSW 邻居遍历访问模式，在查询路径中引入 Buffer Pool 预取策略，降低随机 I/O 等待，并通过 Recall@10、EXPLAIN BUFFERS 和延迟探针验证效果。
```

完成 benchmark 后可以写：

```text
面向 RAG / AI Agent Memory 场景，优化 PostgreSQL pgvector HNSW 向量索引查询路径，在保持 Recall@10 的前提下对比验证延迟、Buffer 命中和尾延迟变化。
```

