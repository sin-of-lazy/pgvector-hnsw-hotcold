# IVFFlat：算法原理、功能与 pgvector 实现解读

## 1. IVFFlat 是什么

IVFFlat（Inverted File + Flat）是一种**近似最近邻（ANN）**索引方法，核心思想是：

1. 先把全量向量用聚类方法分成 `lists` 个簇（每个簇一个质心）
2. 每条向量只放入“离自己最近的质心”对应的倒排列表
3. 查询时不扫全表，只扫描与查询向量最接近的 `probes` 个列表

它的工程价值是：
- 比精确扫描快得多（尤其数据量大时）
- 召回率可调（通过 `probes`）
- 内存占用通常比图索引更稳

但也有代价：
- 召回率不一定 100%
- 需要训练（构建时先做 k-means）

---

## 2. 算法原理（从直觉到公式）

### 2.1 构建阶段

- 输入：N 条 d 维向量
- 目标：得到 `lists = K` 个质心
- 方法：k-means（本项目是 k-means++ 初始化 + Lloyd 迭代）

得到质心后，对每个向量 `x`：
- 找 `argmin_i dist(x, c_i)`
- 把 `x` 放到第 `i` 个列表

### 2.2 查询阶段

给定查询向量 `q`：
1. 先计算 `q` 到所有质心的距离
2. 选最近的 `probes` 个列表
3. 在这 `probes` 个列表里线性扫描并计算精确距离
4. 返回 top-k

### 2.3 复杂度直觉

- 精确扫描：`O(N)`
- IVFFlat：大致 `O(K + probes * N / K)`（忽略常数）

因此：
- `K` 太小：每个列表太大，扫描仍慢
- `K` 太大：训练和管理成本高，且可能过度稀疏
- `probes` 越大，召回率通常越高，但查询更慢

---

## 3. 主要参数与功能

### 3.1 `lists`（建索引参数）

- 含义：倒排列表个数（聚类中心数）
- 在 pgvector 中：索引 reloption（`CREATE INDEX ... WITH (lists=...)`）
- 影响：
  - 更大 `lists`：每个列表更小，单次扫描更快，但训练更重
  - 过大可能导致很多空/小列表，收益下降

### 3.2 `probes`（查询参数）

- 含义：查询时探测多少个列表
- 在 pgvector 中：GUC `ivfflat.probes`
- 影响：
  - 小 `probes`：更快但召回率可能较低
  - 大 `probes`：更慢但更接近精确结果

### 3.3 `iterative_scan` / `max_probes`

- 作用：先按较小批次扫描，不够再扩展，平衡延迟与召回
- 在 pgvector 中：`ivfflat.iterative_scan`、`ivfflat.max_probes`

---

## 4. 在本项目中的实现映射（源码导航）

- `src/ivfflat.h`：核心数据结构、常量、参数定义
- `src/ivfflat.c`：Index AM 注册、GUC/reloptions、代价估算
- `src/ivfbuild.c`：构建主流程（采样 -> k-means -> 分配 -> 写页）
- `src/ivfkmeans.c`：k-means++ 与 Lloyd 迭代
- `src/ivfscan.c`：查询扫描（选列表 + 扫列表 + 排序返回）
- `src/ivfinsert.c`：增量插入（找最近中心 -> 写入对应列表）
- `src/ivfutils.c`：页/WAL 工具函数、类型适配
- `src/ivfvacuum.c`：清理失效条目与维护统计
- `sql/vector.sql`：`CREATE ACCESS METHOD ivfflat` 与 opclass 定义

---

## 5. pgvector 中 IVFFlat 的执行链路

### 5.1 建索引链路

1. `ivfflatbuild()` 进入构建
2. 采样：水塘抽样拿到代表性样本
3. 聚类：`IvfflatKmeans()` 得到 `lists` 个中心
4. 分配：每条向量分配到最近中心（写入排序器）
5. 落盘：创建 metapage + list pages + entry pages

### 5.2 查询链路

1. `ivfflatbeginscan()` 初始化扫描状态
2. `ivfflatrescan()` 重置状态并准备查询参数
3. `GetScanLists()` 选最近 `probes` 个列表
4. `GetScanItems()` 扫描候选列表并按距离排序
5. `ivfflatgettuple()` 逐条返回 top-k

### 5.3 插入链路

1. `ivfflatinsert()` 跳过 NULL
2. 计算向量应进入哪个列表
3. 写入列表尾页（必要时追加页）
4. 通过 generic_xlog 记录 WAL，保证恢复一致性

---

## 6. 存储与恢复（简化理解）

- Block 0：metapage（magic/version/dimensions/lists）
- Block 1..：列表头页（包含质心、列表起始页、插入页）
- 后续：各列表的数据页（向量 + heap tid）

与 HNSW 的一个关键差异：
- IVFFlat 在页修改上广泛使用 generic_xlog，崩溃后可重放恢复

---

## 7. 带详细中文注释的源码片段（按模块）

> 说明：以下片段根据本仓库源码摘录并做中文解释性注释，省略了少量与主线无关的代码。

### 7.1 访问方法注册与参数初始化（`src/ivfflat.c`）

```c
void
IvfflatInit(void)
{
    // 注册 IVFFlat 专属的 reloption 类型；后面 lists 会挂在这个 kind 下
    ivfflat_relopt_kind = add_reloption_kind();

    // 建索引参数：lists（倒排列表数量）
    add_int_reloption(ivfflat_relopt_kind, "lists", "Number of inverted lists",
                      IVFFLAT_DEFAULT_LISTS, IVFFLAT_MIN_LISTS, IVFFLAT_MAX_LISTS, AccessExclusiveLock);

    // 查询参数：probes（每次查询要探测的列表数）
    DefineCustomIntVariable("ivfflat.probes", "Sets the number of probes",
                            "Valid range is 1..lists.", &ivfflat_probes,
                            IVFFLAT_DEFAULT_PROBES, IVFFLAT_MIN_LISTS, IVFFLAT_MAX_LISTS, PGC_USERSET, 0, NULL, NULL, NULL);

    // 迭代扫描模式：off / relaxed_order
    DefineCustomEnumVariable("ivfflat.iterative_scan", "Sets the mode for iterative scans",
                             NULL, &ivfflat_iterative_scan,
                             IVFFLAT_ITERATIVE_SCAN_OFF, ivfflat_iterative_scan_options, PGC_USERSET, 0, NULL, NULL, NULL);

    // 迭代扫描的最大探测上限（防止无限扩张）
    DefineCustomIntVariable("ivfflat.max_probes", "Sets the max number of probes for iterative scans",
                            NULL, &ivfflat_max_probes,
                            IVFFLAT_MAX_LISTS, IVFFLAT_MIN_LISTS, IVFFLAT_MAX_LISTS, PGC_USERSET, 0, NULL, NULL, NULL);

    // 保留 GUC 前缀，避免被其他插件抢占同名参数
    MarkGUCPrefixReserved("ivfflat");
}
```

### 7.2 建索引总入口（`src/ivfbuild.c`）

```c
IndexBuildResult *
ivfflatbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
    IndexBuildResult *result;
    IvfflatBuildState buildstate;

    // 主流程：初始化 -> 训练中心 -> 建页并写入数据
    BuildIndex(heap, index, indexInfo, &buildstate, MAIN_FORKNUM);

    // 把统计信息返回给 PostgreSQL（用于元数据/优化器等）
    result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
    result->heap_tuples = buildstate.reltuples;   // 表中扫描到的数据量
    result->index_tuples = buildstate.indtuples;  // 实际进入索引的条目数

    return result;
}
```

### 7.3 k-means++ 初始化核心（`src/ivfkmeans.c`）

```c
static void
InitCenters(Relation index, VectorArray samples, VectorArray centers, float *lowerBound)
{
    // 先随机选第 1 个中心
    VectorArraySet(centers, 0, VectorArrayGet(samples, RandomInt() % samples->length));
    centers->length++;

    // weight[j] 维护样本 j 到“最近已选中心”的距离平方
    for (int64 j = 0; j < samples->length; j++)
        weight[j] = FLT_MAX;

    // 逐步挑选剩余中心
    for (int i = 0; i < numCenters; i++)
    {
        double sum = 0.0;

        for (int64 j = 0; j < numSamples; j++)
        {
            // 只计算“当前新增中心”带来的距离变化
            double distance = DatumGetFloat8(FunctionCall2Coll(procinfo, collation,
                                 PointerGetDatum(VectorArrayGet(samples, j)),
                                 PointerGetDatum(VectorArrayGet(centers, i))));

            // 记录下界（供后续迭代剪枝/优化使用）
            lowerBound[j * numCenters + i] = distance;

            // k-means++ 的关键：按 D(x)^2 做加权采样
            distance *= distance;
            if (distance < weight[j])
                weight[j] = distance;

            sum += weight[j];
        }

        // 最后一轮只需更新权重，不再选新中心
        if (i + 1 == numCenters)
            break;

        // 按累计权重随机落点，决定下一个中心
        double choice = sum * RandomDouble();
        for (int64 j = 0; j < numSamples - 1; j++)
        {
            choice -= weight[j];
            if (choice <= 0)
            {
                VectorArraySet(centers, i + 1, VectorArrayGet(samples, j));
                centers->length++;
                break;
            }
        }
    }
}
```

### 7.4 查询时选列表 + 扫描候选（`src/ivfscan.c`）

```c
static void
GetScanLists(IndexScanDesc scan, Datum value)
{
    // 扫描所有列表头页，找与查询向量最近的 maxProbes 个列表
    // 这里用 pairingheap 维护“当前候选集合”
    while (BlockNumberIsValid(nextblkno))
    {
        // 遍历列表头中的每个 list
        for (OffsetNumber offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
        {
            // 距离 = 查询向量到该列表质心的距离
            double distance = DatumGetFloat8(so->distfunc(so->procinfo, so->collation,
                               PointerGetDatum(&list->center), value));

            // 维护 top-maxProbes 最近列表（小根/大根逻辑由 CompareLists 决定）
            ...
        }
        nextblkno = IvfflatPageGetOpaque(cpage)->nextblkno;
    }

    // 输出为 listPages[]，供后续逐列表扫描
    ...
}

static void
GetScanItems(IndexScanDesc scan, Datum value)
{
    // 每轮最多扫 probes 个列表，并把结果塞进 tuplesort
    while (so->listIndex < so->maxProbes && (++batchProbes) <= so->probes)
    {
        // 遍历该列表链上的所有数据页
        while (BlockNumberIsValid(searchPage))
        {
            // 对页内每条向量计算距离，构造虚拟 tuple
            slot->tts_values[0] = so->distfunc(so->procinfo, so->collation, datum, value); // 距离
            slot->tts_values[1] = PointerGetDatum(&itup->t_tid);                            // 回表 TID
            tuplesort_puttupleslot(so->sortstate, slot);                                     // 进入排序器
        }
    }

    // 排完后 ivfflatgettuple() 逐条弹出 top-k
    tuplesort_performsort(so->sortstate);
}
```

### 7.5 插入流程（`src/ivfinsert.c`）

```c
bool
ivfflatinsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid,
              Relation heap, IndexUniqueCheck checkUnique,
              IndexInfo *indexInfo)
{
    // 1) NULL 向量不进索引（与多数 PG 索引行为一致）
    if (isnull[0])
        return false;

    // 2) 建临时内存上下文，避免 detoast/归一化产生的碎片影响外层上下文
    insertCtx = AllocSetContextCreate(CurrentMemoryContext,
                                      "Ivfflat insert temporary context",
                                      ALLOCSET_DEFAULT_SIZES);
    oldCtx = MemoryContextSwitchTo(insertCtx);

    // 3) 真正执行插入：内部会定位最近列表并写入索引页
    InsertTuple(index, values, isnull, heap_tid);

    // 4) 清理临时上下文
    MemoryContextSwitchTo(oldCtx);
    MemoryContextDelete(insertCtx);

    // IVFFlat 不做唯一性约束检查，固定返回 false
    return false;
}
```

### 7.6 页追加与 WAL 一致性（`src/ivfutils.c`）

```c
void
IvfflatAppendPage(Relation index, Buffer *buf, Page *page, GenericXLogState **state, ForkNumber forkNum)
{
    // 1) 申请新页，并把它注册到 generic_xlog
    Buffer newbuf = IvfflatNewBuffer(index, forkNum);
    Page newpage = GenericXLogRegisterBuffer(*state, newbuf, GENERIC_XLOG_FULL_IMAGE);

    // 2) 先改旧页：把 nextblkno 指向新页（链表串起来）
    IvfflatPageGetOpaque(*page)->nextblkno = BufferGetBlockNumber(newbuf);

    // 3) 初始化新页头（page_id / nextblkno 等）
    IvfflatInitPage(newbuf, newpage);

    // 4) 一次性提交 WAL + 页修改，确保崩溃恢复可重放
    GenericXLogFinish(*state);

    // 5) 释放旧页锁，切换上下文到新页，供后续继续写入
    UnlockReleaseBuffer(*buf);
    *state = GenericXLogStart(index);
    *page = GenericXLogRegisterBuffer(*state, newbuf, GENERIC_XLOG_FULL_IMAGE);
    *buf = newbuf;
}
```

---

## 8. 如何用这份文档读源码（建议顺序）

1. 先看 `src/ivfflat.h`（把结构体和常量认全）
2. 再看 `src/ivfflat.c`（理解 AM 生命周期入口）
3. 接着看 `src/ivfscan.c`（先打通查询链路）
4. 然后看 `src/ivfbuild.c` + `src/ivfkmeans.c`（理解训练与建索引）
5. 最后看 `src/ivfinsert.c` + `src/ivfutils.c` + `src/ivfvacuum.c`（增量写入、WAL、维护）

如果你希望，我可以继续给你补一版《IVFFlat 读码任务清单（7 天版）》：每天列出“看哪些函数 + 做什么验证 SQL + 预期看到什么现象”。

---

## 9. 一图看懂 IVFFlat（形象版）

把 IVFFlat 想象成“先分区，再精查”的图书馆检索：

- **建库阶段**：先按主题把书分到 `lists` 个书架（质心）
- **查询阶段**：先判断你的问题最可能在哪几个书架（`probes`）
- **精查阶段**：只在这些书架里逐本比对，找最相关 top-k

```
全量向量
   |
   |  k-means 训练
   v
[ lists 个质心 ]
   |
   |  向量分配到最近质心
   v
[ list0 ] [ list1 ] ... [ listK-1 ]

查询 q
   |
   |  计算 q 到各质心距离
   v
选最近 probes 个 list
   |
   |  仅扫描这些 list 内部向量
   v
排序并返回 top-k
```

---

## 10. 调参速查（从“能用”到“好用”）

> 下面是经验起点，不是硬规则；不同数据分布会有差异。

### 10.1 `lists` 建议起点

- 数据量 10 万级：可从 `100 ~ 500` 起步
- 数据量 100 万级：可从 `500 ~ 2000` 起步
- 常见经验是 `lists` 与数据规模次线性增长（例如接近 `sqrt(N)` 的量级）

### 10.2 `probes` 建议起点

- 先从 `1` 开始测延迟（最快）
- 召回不足时，逐步升到 `4/8/16/...`
- 当 `probes` 接近 `lists` 时，行为会逐步接近“全量扫描列表”

### 10.3 一个实用调参流程

1. 固定数据与查询集，先定 `lists` 的 2~3 个候选值
2. 每个 `lists` 下扫描 `probes`（1, 4, 8, 16...）
3. 记录三指标：`P95 延迟`、`召回率@k`、`索引大小`
4. 选“满足召回下的最低延迟”配置，而不是盲目追求最高召回

---

## 11. 最小可跑的 SQL 观察脚本

> 用于建立“参数变化 -> 查询行为变化”的直觉。

```sql
-- 1) 建表示例
CREATE TABLE items (
    id bigserial PRIMARY KEY,
    embedding vector(768)
);

-- 2) 建 IVFFlat 索引（以 L2 为例）
CREATE INDEX ON items USING ivfflat (embedding vector_l2_ops) WITH (lists = 1000);

-- 3) 调整 probes 并观察
SET ivfflat.probes = 1;
EXPLAIN ANALYZE SELECT id FROM items ORDER BY embedding <-> '[0.1,0.2,...]' LIMIT 10;

SET ivfflat.probes = 16;
EXPLAIN ANALYZE SELECT id FROM items ORDER BY embedding <-> '[0.1,0.2,...]' LIMIT 10;
```

你通常会观察到：
- `probes` 变大 -> 访问更多列表 -> 延迟上升
- 同时结果质量（相对精确检索的接近程度）通常提升

---

## 12. 常见误区与排障清单

- **误区 1：`lists` 越大越好**  
  现实：过大可能导致空列表增多，训练和维护成本上升。
- **误区 2：只看单次延迟，不看召回**  
  现实：ANN 的核心是“精度-速度折中”，需同时评估。
- **误区 3：忽略数据分布变化**  
  现实：数据漂移后，旧质心代表性下降，可能需要重建索引。
- **误区 4：把 `probes` 长期拉满**  
  现实：会逼近全列表扫描，失去 IVFFlat 的加速意义。

排障时优先检查：
1. 查询是否是 `ORDER BY 距离 LIMIT k` 这种 KNN 形态
2. 使用的 opclass 是否与距离算子一致（如 `<->` 对 `vector_l2_ops`）
3. `lists/probes` 是否处于合理区间
4. 是否存在明显数据偏斜（大量向量集中在少数簇）

---

## 13. IVFFlat 与 HNSW 如何选

- **优先 IVFFlat 的场景**
  - 希望结构更直观、参数更少、调优路径更线性
  - 接受通过 `probes` 逐步换召回
- **优先 HNSW 的场景**
  - 更看重高召回下的低查询延迟
  - 接受更复杂图结构与更高内存/构建复杂度

简化记忆：
- IVFFlat：先分桶再桶内扫
- HNSW：图导航逐层逼近

---

## 14. 术语速记

- **ANN**：近似最近邻
- **centroid / center**：聚类中心（质心）
- **list**：倒排列表
- **probes**：查询探测的列表数
- **recall@k**：前 k 个结果里命中真实近邻的比例
- **iterative scan**：分批扩展探测范围的扫描模式

---

## 15. 逐函数读码路线（7 天版）

> 目标：每天 1~2 小时，从“看懂模块”走到“能解释行为变化”。

### Day 1：入口与参数框架

- **读文件**：`src/ivfflat.c`、`sql/vector.sql`
- **关键函数**：`IvfflatInit`、`ivfflathandler`、`ivfflatcostestimate`
- **要回答的问题**：
  - IVFFlat 如何注册为 AM？
  - `lists`、`probes`、`iterative_scan` 分别在何时生效？
- **当日产出**：一页“SQL -> handler -> 回调函数”映射图

### Day 2：查询生命周期

- **读文件**：`src/ivfscan.c`
- **关键函数**：`ivfflatbeginscan`、`ivfflatrescan`、`GetScanLists`、`GetScanItems`、`ivfflatgettuple`
- **要回答的问题**：
  - 为什么先选列表再扫条目？
  - tuplesort 在查询路径里起什么作用？
- **当日产出**：查询时序图（含 `listPages` 与 `tuplesort`）

### Day 3：构建总流程

- **读文件**：`src/ivfbuild.c`
- **关键函数**：`ivfflatbuild`、`BuildIndex`、`SampleRows`、`AddTupleToSort`
- **要回答的问题**：
  - 为什么构建分“采样/分配/写入”三阶段？
  - 为什么先写排序结构再落盘？
- **当日产出**：构建阶段状态图（输入/输出）

### Day 4：k-means 细节

- **读文件**：`src/ivfkmeans.c`
- **关键函数**：`InitCenters`、`NormCenters`、`UpdateCenters`
- **要回答的问题**：
  - k-means++ 如何减少坏初始化概率？
  - 余弦场景为何要归一化质心？
- **当日产出**：k-means++ 伪代码（10~15 行）

### Day 5：插入与页管理

- **读文件**：`src/ivfinsert.c`、`src/ivfutils.c`
- **关键函数**：`ivfflatinsert`、`InsertTuple`、`IvfflatAppendPage`、`IvfflatUpdateList`
- **要回答的问题**：
  - 新向量如何定位到最近列表？
  - 页满时链表如何安全扩展？
- **当日产出**：一份“插入写页路径”流程卡片

### Day 6：VACUUM 与维护

- **读文件**：`src/ivfvacuum.c`
- **关键函数**：`ivfflatbulkdelete`、`ivfflatvacuumcleanup`
- **要回答的问题**：
  - 失效条目如何清理？
  - 清理后统计信息如何更新？
- **当日产出**：一段“维护动作影响查询”的总结

### Day 7：全链路复盘与口述

- **回看文件**：`src/ivfflat.c`、`src/ivfscan.c`、`src/ivfbuild.c`
- **练习任务**：
  - 从 SQL 出发口述完整调用链
  - 解释 `lists` 和 `probes` 分别影响哪个阶段
- **当日产出**：一页“我如何调 IVFFlat”的个人手册

---

## 16. 实验模板（recall@k + 参数网格）

### 16.1 评估目标与指标

- **目标**：在满足召回约束下找到最优查询延迟
- **核心指标**：
  - `recall@k`
  - `P50/P95/P99 latency`
  - 索引大小、构建时长

### 16.2 `recall@k` 计算思路

- 获取 ANN 结果（IVFFlat）
- 获取 exact 结果（精确 top-k）
- 按查询计算交集比例：

`recall@k = |ANN_topk ∩ Exact_topk| / k`

### 16.3 最小实验步骤（模板）

```sql
-- A. 固定 probes 跑 ANN
SET ivfflat.probes = 8;
EXPLAIN ANALYZE
SELECT id
FROM items
ORDER BY embedding <-> :query_vec
LIMIT :k;

-- B. 获取 exact top-k（思路：禁用索引路径或在无索引副本表上执行）
--    具体实现按你的环境选型，目标是得到稳定的精确基线。

-- C. 统计 ANN 与 exact 的交集，汇总 recall@k
```

### 16.4 参数网格建议（IVFFlat）

- 建索引参数网格：
  - `lists`: 200, 500, 1000, 2000
- 查询参数网格：
  - `probes`: 1, 4, 8, 16, 32
- 可选：
  - `ivfflat.iterative_scan`: off / relaxed_order
  - `ivfflat.max_probes`: 16, 32, 64

建议顺序：
1. 先固定 `lists`，扫 `probes`
2. 再调整 `lists` 重建索引
3. 最后引入 iterative 参数做细调

### 16.5 结果记录模板（建议直接复制）

| 数据集 | 维度 | lists | probes | iterative_scan | max_probes | k | recall@k | P95(ms) | 索引大小 | 构建时长 |
|--------|------|-------|--------|----------------|------------|---|----------|---------|----------|----------|
| demoA  | 768  | 1000  | 8      | off            | 1000       | 10| 0.89     | 9.7     | 1.4GB    | 11m      |

### 16.6 实验注意事项

- 至少准备 100~1000 条查询样本，减少偶然波动
- 对比时保持硬件、并发、缓存状态尽量一致
- 参数变化后明确记录“是否重建索引”
- 结论要用“召回-延迟曲线”而非单点数字

