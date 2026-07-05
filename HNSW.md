# HNSW：算法原理、功能与 pgvector 实现解读

## 任务回执与执行计划

- [x] 新建 `HNSW.md`
- [x] 解释 HNSW 的原理、功能与核心参数
- [x] 说明 HNSW 在本项目中的实现映射与执行链路
- [x] 提供带详细中文注释的源码片段，覆盖注册、构建、查询、插入、VACUUM、核心搜索算法

---

## 1. HNSW 是什么

HNSW（Hierarchical Navigable Small World，分层可导航小世界）是一种**图结构近似最近邻（ANN）**算法。它的核心思想是：

1. 把每个向量当作图中的一个节点
2. 图是分层的：
   - 底层（L0）包含全部节点
   - 上层是逐渐稀疏的“捷径层”
3. 查询时从高层开始“粗定位”，再逐层下沉到底层做精细搜索

这种结构的效果类似“导航地图”：
- 高层负责快速接近目标区域
- 底层负责局部精确查找

---

## 2. 算法原理（从直觉到流程）

### 2.1 图结构与层级

- 每个节点会随机分配一个最高层级 `level`
- 概率分布让高层节点更少（稀疏），形成“跳表式”结构
- 每层节点有邻居连接，连接数量受 `m` 控制

### 2.2 插入流程（简化）

给新向量 `x`：
1. 随机生成 `x` 的最高层 `l`
2. 从当前入口点（entry point）在高层贪心下降，找到接近 `x` 的位置
3. 在 `0..l` 每层执行近邻搜索，得到候选集
4. 通过邻居选择策略保留最合适邻居并建立双向边
5. 若 `x` 层级比当前入口更高，更新入口点

### 2.3 查询流程（简化）

给查询向量 `q`：
1. 从入口点最高层开始，逐层贪心逼近
2. 到 L0 后，做更宽的 best-first 搜索（候选集大小由 `ef_search` 决定）
3. 返回 top-k

### 2.4 参数直觉

- `m`：每层连接上限（L0 通常是 `2m`）
  - 大：图更密、召回高、内存与构建成本上升
- `ef_construction`：建索引时候选集宽度
  - 大：索引质量更高、构建更慢
- `ef_search`：查询时候选集宽度
  - 大：召回更高、查询更慢

---

## 3. HNSW 在 pgvector 中的实现映射

- `src/hnsw.h`：核心常量、结构体、函数声明（元页、元素、邻居、搜索候选）
- `src/hnsw.c`：Index AM 注册、GUC 参数、reloptions、代价估算
- `src/hnswbuild.c`：建索引主流程（单进程/并行、内存阶段到落盘阶段）
- `src/hnswinsert.c`：增量插入与并发更新图
- `src/hnswscan.c`：查询扫描（初始化、首次搜索、迭代扫描）
- `src/hnswutils.c`：核心算法函数（单层搜索、邻居选择、图连接）
- `src/hnswvacuum.c`：软删除与图修复
- `sql/vector.sql`：`CREATE ACCESS METHOD hnsw` 与 opclass 注册

---

## 4. 与 PostgreSQL 的集成方式

### 4.1 SQL 层注册

`sql/vector.sql` 中注册了：
- `CREATE FUNCTION hnswhandler(internal) RETURNS index_am_handler`
- `CREATE ACCESS METHOD hnsw TYPE INDEX HANDLER hnswhandler`

并定义了 `hnsw` 下的 opclass（如 `vector_l2_ops`、`vector_cosine_ops`、`vector_ip_ops`）。

### 4.2 AM 回调

`hnswhandler()` 返回 `IndexAmRoutine`，主要回调包括：
- `ambuild -> hnswbuild`
- `aminsert -> hnswinsert`
- `ambeginscan/amrescan/amgettuple/amendscan -> hnswscan.c`
- `ambulkdelete/amvacuumcleanup -> hnswvacuum.c`

这就是 PostgreSQL 执行器调用 HNSW 的主入口。

---

## 5. 执行链路（项目视角）

### 5.1 建索引链路

1. `hnswbuild()` 进入构建
2. 内存阶段构图（必要时并行）
3. 达到条件后落盘（元页 + 元素元组 + 邻居元组）
4. 最后写 WAL 范围日志（确保一致性）

### 5.2 查询链路

1. `hnswbeginscan()` 初始化扫描状态
2. `hnswrescan()` 重置上下文
3. `hnswgettuple()` 首次调用触发完整搜索并缓存候选
4. 后续调用持续返回候选，直到结束

### 5.3 插入链路

1. `hnswinsert()` 跳过 NULL
2. 形成索引值并进入 `HnswInsertTupleOnDisk`
3. 加锁读取入口点并搜索邻居
4. 更新图结构并释放锁

### 5.4 VACUUM 链路

1. `hnswbulkdelete`：先软删除标记（`deleted=1`）
2. `hnswvacuumcleanup`：修复邻居关系、必要时更新入口点

---

## 6. 存储结构（简化理解）

- Block 0：`HnswMetaPageData`
  - `magicNumber/version/dimensions/m/efConstruction`
  - `entryBlkno/entryOffno/entryLevel`
  - `insertPage`
- Block 1..：数据页
  - `HnswElementTuple`（元素信息 + heaptids + 向量）
  - `HnswNeighborTuple`（各层邻居 TID）

与 IVFFlat 对比：
- HNSW 更偏“图导航”结构，内存占用和并发路径更复杂
- IVFFlat 更偏“聚类分桶 + 列表扫描”

---

## 7. 带详细中文注释的源码片段（按功能模块）

> 说明：以下片段摘自本仓库并做解释性注释，省略了少量非主线代码。

### 7.1 参数初始化（`src/hnsw.c`）

```c
void
HnswInit(void)
{
    // 非 shared_preload 路径下，当前进程需要确保 tranche 初始化
    if (!process_shared_preload_libraries_in_progress)
        HnswInitLockTranche();

    // 注册 HNSW 的索引存储参数：m、ef_construction
    hnsw_relopt_kind = add_reloption_kind();
    add_int_reloption(hnsw_relopt_kind, "m", "Max number of connections",
                      HNSW_DEFAULT_M, HNSW_MIN_M, HNSW_MAX_M, AccessExclusiveLock);
    add_int_reloption(hnsw_relopt_kind, "ef_construction", "Size of the dynamic candidate list for construction",
                      HNSW_DEFAULT_EF_CONSTRUCTION, HNSW_MIN_EF_CONSTRUCTION, HNSW_MAX_EF_CONSTRUCTION, AccessExclusiveLock);

    // 查询参数：ef_search（查询候选宽度）
    DefineCustomIntVariable("hnsw.ef_search", "Sets the size of the dynamic candidate list for search",
                            "Valid range is 1..1000.", &hnsw_ef_search,
                            HNSW_DEFAULT_EF_SEARCH, HNSW_MIN_EF_SEARCH, HNSW_MAX_EF_SEARCH, PGC_USERSET, 0, NULL, NULL, NULL);

    // 迭代扫描模式：off / relaxed / strict
    DefineCustomEnumVariable("hnsw.iterative_scan", "Sets the mode for iterative scans",
                             NULL, &hnsw_iterative_scan,
                             HNSW_ITERATIVE_SCAN_OFF, hnsw_iterative_scan_options, PGC_USERSET, 0, NULL, NULL, NULL);

    // 迭代扫描的保护参数（元组访问上限、内存倍数）
    DefineCustomIntVariable("hnsw.max_scan_tuples", "Sets the max number of tuples to visit for iterative scans",
                            NULL, &hnsw_max_scan_tuples,
                            20000, 1, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);

    DefineCustomRealVariable("hnsw.scan_mem_multiplier", "Sets the multiple of work_mem to use for iterative scans",
                             NULL, &hnsw_scan_mem_multiplier,
                             1, 1, 1000, PGC_USERSET, 0, NULL, NULL, NULL);

    // 预留前缀，避免 GUC 名称冲突
    MarkGUCPrefixReserved("hnsw");
}
```

### 7.2 AM handler 注册（`src/hnsw.c`）

```c
Datum
hnswhandler(PG_FUNCTION_ARGS)
{
    static const IndexAmRoutine amroutine = {
        .amsupport = 3,
        .amcanorderbyop = true,      // 支持 ORDER BY 距离算子
        .amcanbuildparallel = true,  // 支持并行构建

        // 构建/插入/扫描/清理 回调绑定
        .ambuild = hnswbuild,
        .ambuildempty = hnswbuildempty,
        .aminsert = hnswinsert,
        .ambulkdelete = hnswbulkdelete,
        .amvacuumcleanup = hnswvacuumcleanup,
        .amcostestimate = hnswcostestimate,
        .ambeginscan = hnswbeginscan,
        .amrescan = hnswrescan,
        .amgettuple = hnswgettuple,
        .amendscan = hnswendscan,
    };

    // PostgreSQL 后续通过该结构体调度 HNSW 各生命周期函数
    PG_RETURN_POINTER(&amroutine);
}
```

### 7.3 构建入口（`src/hnswbuild.c`）

```c
IndexBuildResult *
hnswbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
    IndexBuildResult *result;
    HnswBuildState buildstate;

    // 构建主流程：初始化 -> 构图 -> 落盘/WAL
    BuildIndex(heap, index, indexInfo, &buildstate, MAIN_FORKNUM);

    // 返回统计信息给 PG
    result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
    result->heap_tuples = buildstate.reltuples;
    result->index_tuples = buildstate.indtuples;

    return result;
}
```

### 7.4 查询主路径（`src/hnswscan.c`）

```c
bool
hnswgettuple(IndexScanDesc scan, ScanDirection dir)
{
    HnswScanOpaque so = (HnswScanOpaque) scan->opaque;
    MemoryContext oldCtx = MemoryContextSwitchTo(so->tmpCtx);

    Assert(ScanDirectionIsForward(dir));

    if (so->first)
    {
        // 首次调用时做完整搜索并缓存候选列表 so->w
        if (scan->orderByData == NULL)
            elog(ERROR, "cannot scan hnsw index without order");

        if (!IsMVCCSnapshot(scan->xs_snapshot))
            elog(ERROR, "non-MVCC snapshots are not supported with hnsw");

        Datum value = GetScanValue(scan);

        // 扫描锁：与 vacuum 协调，避免读取到不一致图结构
        LockPage(scan->indexRelation, HNSW_SCAN_LOCK, ShareLock);
        so->w = GetScanItems(scan, value);
        UnlockPage(scan->indexRelation, HNSW_SCAN_LOCK, ShareLock);

        so->first = false;
    }

    // 后续从 so->w 逐个弹出结果；若启用 iterative_scan 可继续扩展搜索
    ...
}
```

### 7.5 插入关键并发控制（`src/hnswinsert.c`）

```c
// 先用较低锁级别进入，必要时升级为排他锁
LockPage(index, HNSW_UPDATE_LOCK, lockmode);

// 读取当前 m 和入口点
HnswGetMetaPageInfo(index, &m, &entryPoint);

// 创建新元素并写入 value
element = HnswInitElement(base, heaptid, m, HnswGetMl(m), HnswGetMaxLevel(m), NULL);
HnswPtrStore(base, element->value, (char *) DatumGetPointer(value));

// 如果新元素层级可能成为新入口点，升级为排他锁再做最终判断
if (entryPoint == NULL || element->level > entryPoint->level)
{
    UnlockPage(index, HNSW_UPDATE_LOCK, lockmode);
    lockmode = ExclusiveLock;
    LockPage(index, HNSW_UPDATE_LOCK, lockmode);
    entryPoint = HnswGetEntryPoint(index);
}

// 搜索邻居 + 更新磁盘图
HnswFindElementNeighbors(base, element, entryPoint, index, support, m, efConstruction, false);
UpdateGraphOnDisk(index, support, element, m, entryPoint, building);

UnlockPage(index, HNSW_UPDATE_LOCK, lockmode);
```

### 7.6 单层搜索核心（`src/hnswutils.c` 的 `HnswSearchLayer`）

```c
List *
HnswSearchLayer(...)
{
    // C: 候选最小堆（下一步待扩展）
    // W: 当前最好结果堆（用于维持 ef 大小窗口）
    pairingheap *C = pairingheap_allocate(CompareNearestCandidates, NULL);
    pairingheap *W = pairingheap_allocate(CompareFurthestCandidates, NULL);

    // visited 防止重复访问，避免图上循环
    if (initVisited)
        InitVisited(base, v, inMemory, ef, m);

    // 入口点先进入候选与结果
    foreach(lc2, ep)
    {
        ...
        pairingheap_add(C, &sc->c_node);
        pairingheap_add(W, &sc->w_node);
    }

    while (!pairingheap_is_empty(C))
    {
        // 取当前最有希望扩展的候选 c
        HnswSearchCandidate *c = ...remove_first(C);
        HnswSearchCandidate *f = ...first(W); // W 中最差者

        // 终止条件：当前最优扩展都比 W 中最差结果更差，搜索可停止
        if (c->distance > f->distance)
            break;

        // 加载 c 的邻居（内存图 or 磁盘图）并遍历
        ...

        for (int i = 0; i < unvisitedLength; i++)
        {
            // 计算邻居距离
            ...

            // 若比当前结果集更优（或结果集未满），加入 C/W
            if (eDistance < f->distance || wlen < ef)
            {
                pairingheap_add(C, &e->c_node);
                pairingheap_add(W, &e->w_node);

                // 超过 ef 则移除 W 中最差者，维持窗口大小
                if (wlen > ef)
                    ...remove_first(W);
            }
        }
    }

    // 最终把 W 转换成有序候选列表返回
    ...
}
```

### 7.7 VACUUM 的软删除思路（`src/hnswvacuum.c`）

```c
static void
RemoveHeapTids(HnswVacuumState * vacuumstate)
{
    // 扫描索引页中每个元素元组
    while (BlockNumberIsValid(blkno))
    {
        ...
        for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
        {
            HnswElementTuple etup = ...;

            // 把被 VACUUM 判定为 dead 的 heaptid 移除，并压紧数组
            if (vacuumstate->callback(&etup->heaptids[i], vacuumstate->callback_state))
            {
                itemUpdated = true;
                stats->tuples_removed++;
            }
            else
            {
                etup->heaptids[idx++] = etup->heaptids[i];
                stats->num_index_tuples++;
            }

            // 若该元素已无任何 heaptid，则加入 deleted 集，后续修复邻居关系
            if (!ItemPointerIsValid(&etup->heaptids[0]))
                tidhash_insert(vacuumstate->deleted, ip, &found);
        }
        ...
    }
}
```

---

## 8. 调参与选型建议（入门版）

- 小数据/低延迟优先：`m` 与 `ef_search` 可从默认值起步
- 追求召回：优先提高 `ef_search`，再考虑增大 `m` 与 `ef_construction`
- 构建慢但查询收益不明显时：检查数据分布是否已足够“可分离”
- 内存紧张：谨慎提高 `m`（图会更密），可先仅调 `ef_search`

---

## 9. 建议阅读顺序（和源码模块一一对应）

1. `src/hnsw.h`：把结构体与参数先看懂
2. `src/hnsw.c`：理解 PG 如何调用 HNSW
3. `src/hnswscan.c`：先打通查询主链路
4. `src/hnswinsert.c` + `src/hnswutils.c`：理解插入与核心搜索
5. `src/hnswbuild.c`：再看构建与并行细节
6. `src/hnswvacuum.c`：最后看维护与修复

如果你需要，我可以接着给你补一份《HNSW 读码任务清单（7 天版）》：每天“函数清单 + 验证 SQL + 预期现象”。

---

## 10. 一图看懂 HNSW（形象版）

把 HNSW 想象成“城市导航系统”：

- **高层道路**：车少路快，用于快速接近目标片区
- **底层道路**：路网更密，用于最终精确到达

查询像这样进行：

```
入口点（最高层）
      |
      |  贪心移动：每一步都朝更近的节点前进
      v
   第 L 层  ----->  第 L-1 层  -----> ... -----> 第 1 层
      |
      v
   第 0 层（全量节点）
      |
      |  以 ef_search 维护候选窗口，扩展更优邻居
      v
   返回 top-k
```

核心直觉：
- 高层负责“方向感”
- 底层负责“精细排序”

---

## 11. 参数调优速查（工程视角）

> 以下是经验起点，不同数据分布与硬件下需实测调整。

### 11.1 `ef_search`（优先调这个）

- 推荐先固定索引结构，仅调 `ef_search`
- 典型策略：`40 -> 80 -> 120 -> 200` 逐步提升
- 观察指标：`P95 延迟` 与 `召回率@k`

### 11.2 `m` 与 `ef_construction`（建索引阶段）

- `m` 越大：图更密，通常召回更好，但内存与构建成本上升
- `ef_construction` 越大：构图质量更好，但构建更慢
- 常见组合思路：先定 `m`，再令 `ef_construction` 约为其数倍并微调

### 11.3 实用调参流程

1. 先用默认参数建索引，拿到 baseline
2. 固定索引不重建，仅调 `ef_search` 找到可接受延迟/召回平衡点
3. 若召回仍不足，再考虑增大 `m` 与 `ef_construction` 后重建
4. 最后评估内存占用、构建时长、查询稳定性（特别是 P95/P99）

---

## 12. 最小可跑 SQL 观察脚本

```sql
-- 1) 建表示例
CREATE TABLE items (
    id bigserial PRIMARY KEY,
    embedding vector(768)
);

-- 2) 建 HNSW 索引（L2 示例）
CREATE INDEX ON items USING hnsw (embedding vector_l2_ops) WITH (m = 16, ef_construction = 64);

-- 3) 调整查询参数并对比
SET hnsw.ef_search = 40;
EXPLAIN ANALYZE SELECT id FROM items ORDER BY embedding <-> '[0.1,0.2,...]' LIMIT 10;

SET hnsw.ef_search = 120;
EXPLAIN ANALYZE SELECT id FROM items ORDER BY embedding <-> '[0.1,0.2,...]' LIMIT 10;
```

你通常会观察到：
- `ef_search` 增大 -> 搜索更充分 -> 召回上升趋势更明显
- 同时查询耗时和访问节点数量通常会增加

---

## 13. 常见误区与排障清单

- **误区 1：只调 `m`，不调 `ef_search`**  
  实际上查询质量往往先由 `ef_search` 直接决定。
- **误区 2：把 `ef_search` 拉很大就一定值得**  
  可能带来明显延迟增长，但召回收益边际变小。
- **误区 3：忽略数据分布变化**  
  数据漂移后图结构质量可能下降，需评估重建时机。
- **误区 4：只看平均延迟**  
  ANN 查询应重点关注 P95/P99 与召回下限。

排障优先检查：
1. 查询是否为 `ORDER BY 距离 LIMIT k` 的 KNN 模式
2. 距离算子与 opclass 是否匹配（例如 `<->` 对 `vector_l2_ops`）
3. `ef_search` 是否过小导致候选窗口不足
4. 索引构建参数与数据规模是否失衡（图过稀或构建质量不足）

---

## 14. HNSW 与 IVFFlat 选型对照（简版）

| 维度 | HNSW | IVFFlat |
|------|------|---------|
| 查询机制 | 分层图导航 + 候选扩展 | 质心分桶 + 列表扫描 |
| 调参主轴 | `ef_search`（查询）+ `m/ef_construction`（构建） | `probes`（查询）+ `lists`（构建） |
| 结构复杂度 | 更高 | 更低 |
| 常见体验 | 高召回下性能表现常更强，但调优更复杂 | 更直观易控，工程心智负担较小 |

---

## 15. 术语速记

- **entry point**：搜索入口节点（最高层起点）
- **layer**：图的层级
- **greedy descent**：逐层贪心下降
- **ef_search**：查询时候选窗口大小
- **ef_construction**：构建时候选窗口大小
- **m**：连接上限参数（控制图稠密度）
- **iterative scan**：分批扩展候选的扫描模式
- **soft delete**：先标记删除再做图修复的策略

---

## 16. 逐函数读码路线（7 天版）

> 目标：每天 1~2 小时，做到“知道函数在哪、做什么、怎么验证”。

### Day 1：先把入口打通（AM 注册）

- **读文件**：`src/hnsw.c`、`sql/vector.sql`
- **关键函数**：`HnswInit`、`hnswhandler`、`hnswcostestimate`
- **要回答的问题**：
  - HNSW 是如何注册成 PostgreSQL 索引 AM 的？
  - 哪些是 reloption（建索引时固定），哪些是 GUC（查询时可调）？
- **当日产出**：一张“SQL -> handler -> IndexAmRoutine 回调”映射图

### Day 2：先看查询主干（scan 生命周期）

- **读文件**：`src/hnswscan.c`
- **关键函数**：`hnswbeginscan`、`hnswrescan`、`hnswgettuple`、`hnswendscan`
- **要回答的问题**：
  - 第一次 `gettuple` 和后续 `gettuple` 的差别是什么？
  - 为什么要要求 MVCC snapshot？
- **当日产出**：画出 `beginscan -> rescan -> gettuple` 时序图

### Day 3：核心算法层（单层搜索）

- **读文件**：`src/hnswutils.c`
- **关键函数**：`HnswSearchLayer`、`HnswFindElementNeighbors`
- **要回答的问题**：
  - `C` 和 `W` 两个堆分别承担什么角色？
  - 何时停止扩展？`ef` 如何控制搜索宽度？
- **当日产出**：写出 `HnswSearchLayer` 的伪代码（10~20 行）

### Day 4：插入与并发控制

- **读文件**：`src/hnswinsert.c`
- **关键函数**：`hnswinsert`、`HnswInsertTuple`、`HnswInsertTupleOnDisk`
- **要回答的问题**：
  - 为什么会从共享锁升级到排他锁？
  - 入口点更新在并发下如何保证正确？
- **当日产出**：列出“插入流程中的锁点清单”

### Day 5：构建流程（单进程/并行）

- **读文件**：`src/hnswbuild.c`
- **关键函数**：`hnswbuild`、`BuildGraph`、`ParallelBuildGraph`、`FlushGraph`
- **要回答的问题**：
  - 内存阶段与落盘阶段为何拆开？
  - 并行 worker 如何共享图状态？
- **当日产出**：一页“构建阶段状态机”草图

### Day 6：VACUUM 与图修复

- **读文件**：`src/hnswvacuum.c`
- **关键函数**：`hnswbulkdelete`、`hnswvacuumcleanup`、`RemoveHeapTids`、`RepairGraphElement`
- **要回答的问题**：
  - 为什么 HNSW 倾向软删除而不是立即硬删除？
  - 删除后如何避免图不可达？
- **当日产出**：一段“删除与修复策略”的文字总结

### Day 7：回放全链路 + 复盘

- **回看文件**：`src/hnsw.c`、`src/hnswscan.c`、`src/hnswutils.c`
- **练习任务**：
  - 从 SQL 出发口述完整调用链（规划器 -> 执行器 -> AM 回调 -> 搜索函数）
  - 解释参数 `m`、`ef_construction`、`ef_search` 各自影响阶段
- **当日产出**：写一页“我现在如何调 HNSW”的个人手册

---

## 17. 实验模板（recall@k + 参数网格）

### 17.1 评估目标与指标

- **目标**：找到满足召回下限时的最优延迟配置
- **核心指标**：
  - `recall@k`
  - `P50/P95/P99 latency`
  - 索引大小、构建时长

### 17.2 `recall@k` 计算思路

- 先得到“近似检索结果”（HNSW）
- 再得到“精确基线结果”（精确 top-k）
- 每个查询计算：

`recall@k = |ANN_topk ∩ Exact_topk| / k`

### 17.3 最小实验步骤（模板）

```sql
-- A. 固定查询参数并跑 ANN
SET hnsw.ef_search = 80;
EXPLAIN ANALYZE
SELECT id
FROM items
ORDER BY embedding <-> :query_vec
LIMIT :k;

-- B. 取精确基线（思路：禁用索引路径或在无索引副本表上执行）
--    具体做法可按你的环境选择，目标是得到 exact top-k。

-- C. 计算交集并汇总 recall@k（可在应用层或 SQL 层完成）
```

### 17.4 参数网格建议（HNSW）

- 建索引参数网格：
  - `m`: 12, 16, 24, 32
  - `ef_construction`: 64, 100, 200
- 查询参数网格：
  - `ef_search`: 40, 80, 120, 200

建议顺序：
1. 先固定一个索引，仅扫 `ef_search`
2. 再换 `m/ef_construction` 重建索引
3. 最后按业务目标筛选 Pareto 最优点（延迟-召回）

### 17.5 结果记录模板（建议直接复制）

| 数据集 | 维度 | m | ef_construction | ef_search | k | recall@k | P95(ms) | 索引大小 | 构建时长 |
|--------|------|---|------------------|-----------|---|----------|---------|----------|----------|
| demoA  | 768  | 16| 64               | 80        | 10| 0.93     | 12.4    | 2.1GB    | 18m      |

### 17.6 实验注意事项

- 固定硬件与并发负载，避免噪声主导结论
- 至少准备 100~1000 条查询样本，不要只看单个 query
- 报告里同时给平均值和分位值（尤其 P95/P99）
- 参数变更后，明确记录是否重建索引

