# 基于 Buffer Pool 的 HNSW 冷热分层优化（易懂版）

## 怎么读这份文档

- **30 秒看结论**：先看第 1 节
- **3 分钟懂原理**：看第 2、3 节
- **10 分钟可开干**：看第 4、5、6 节

---

## 1. 一句话结论

把 HNSW 的“常访问上层节点”当作**热数据**优先放内存，把“访问少的底层节点”当作**冷数据**按需从 Buffer Pool 读取。目标是：

- 内存占用下降 **50%+**
- 同时保持 `Recall@10 >= 95%`

这不是锦上添花，而是规模上来后必须做的成本优化。

---

## 2. 先用一个形象类比

把 HNSW 想成一个商场：

- **上层稀疏图**像“门口黄金柜台”，几乎每个顾客都会经过（高频）
- **底层 L0 大图**像“后仓备货区”，货多但每次只拿一小部分（低频）

如果把所有货都堆在柜台（全量常驻内存），会很贵、很乱、效率也不稳定。
更合理做法是：

- 柜台（热层）只放高频商品
- 仓库（冷层）按需补货

这就是 HNSW 冷热分层。

---

## 3. 为什么必须做（好处 + 必要性）

### 3.1 好处

- **降成本**：减少常驻内存，机器成本更可控
- **更稳时延**：热点常驻，减少随机 I/O 抖动，P95/P99 更稳定
- **更高吞吐**：内存争用下降后，同机可承载更多并发查询
- **可灰度可回滚**：开关化上线，风险可控

### 3.2 必要性

- HNSW 本身就有“上层热、下层冷”的访问偏置
- 数据规模增大时，L0 膨胀很快，不分层很容易先撞内存瓶颈
- 在 PostgreSQL 生态里，应该尽量利用 Buffer Pool，而不是重复造一个全量用户态缓存

### 3.3 不做的代价

- 只能靠加内存扩容，成本高
- 延迟尾部波动更难压
- 为了保召回被迫加大 `ef_search`，进一步推高内存和延迟

---

## 4. 在 pgvector 里的最小落地点

先只改**查询路径**，不改磁盘格式。

- `src/hnswscan.c`
  - `GetScanItems`
  - `ResumeScanItems`
  - `hnswgettuple`
- `src/hnswutils.c`
  - `HnswSearchLayer`
  - `HnswLoadElement`
  - `HnswLoadNeighborTids`
- `src/hnsw.c`
  - 新增 GUC 开关
- `src/hnsw.h`
  - 扩展扫描态字段

> 第一阶段不碰 `HNSW_VERSION`，先做运行时策略优化，回滚最简单。

---

## 5. 最小改动代码草案（可直接作为第一版）

> 目标：低侵入、可回滚、先验证收益。

### 5.1 结构体字段（`src/hnsw.h`）

```c
/* 轻量热缓存（示意） */
typedef struct HnswHotCacheData
{
    HTAB *table;       /* key: (blkno, offno, level) 或 index tid */
    Size  usedBytes;
    Size  maxBytes;
    int64 hits;
    int64 misses;
} HnswHotCacheData;

typedef HnswHotCacheData *HnswHotCache;

/* 扫描态扩展（示意） */
typedef struct HnswScanOpaqueData
{
    /* ...existing fields... */

    HnswHotCache hotCache;
    bool         hotColdEnabled;
    int          hotLayer;
    int64        cacheEvictions;
    int64        cacheBypass;

    /* ...existing fields... */
} HnswScanOpaqueData;
```

### 5.2 GUC 开关（`src/hnsw.h` + `src/hnsw.c`）

```c
/* src/hnsw.h */
extern bool hnsw_hot_cold_enabled;
extern int  hnsw_hot_layer;
extern int  hnsw_hot_max_bytes;      /* KB */
extern int  hnsw_prefetch_neighbors; /* 0=off */
```

```c
/* src/hnsw.c - globals */
bool hnsw_hot_cold_enabled = false;
int  hnsw_hot_layer = 2;
int  hnsw_hot_max_bytes = 64 * 1024;
int  hnsw_prefetch_neighbors = 16;
```

```c
/* src/hnsw.c - HnswInit() 注册（示意） */
DefineCustomBoolVariable("hnsw.hot_cold_enabled", ...);
DefineCustomIntVariable("hnsw.hot_layer", ...);
DefineCustomIntVariable("hnsw.hot_max_bytes", ...);
DefineCustomIntVariable("hnsw.prefetch_neighbors", ...);
```

### 5.3 `HnswSearchLayer` 改造点（`src/hnswutils.c`）

核心逻辑改为：

1. 先查热缓存
2. 未命中走原有 `HnswLoadElement`
3. 命中/未命中打点
4. 仅高层回填缓存（`lc >= hotLayer`）

```c
/* 伪代码示意 */
element = HotCacheLookup(...);
if (element == NULL)
{
    HnswLoadElement(...);  /* 原路径，保证行为一致 */
    if (lc >= so->hotLayer)
        HotCacheInsertOrEvict(...);
}
```

---

## 6. 实施路线（简单版）

### Phase A：先立基线（必须）

记录四个指标：

- `Recall@10`
- P95/P99 延迟
- 查询峰值内存
- 构建峰值内存

可参考脚本：

- `test/t/044_hnsw_iterative_scan_recall.pl`
- `test/t/045_hnsw_low_memory_build.pl`

### Phase B：只上查询路径热缓存

- 加结构体字段 + GUC
- 在 `HnswSearchLayer` 接入缓存
- 默认 `hnsw.hot_cold_enabled = off`

### Phase C：小步灰度

- 小流量开启开关
- 观察命中率、内存降幅、Recall 是否达标
- 不达标先调 `ef_search` 和 `hnsw.hot_max_bytes`

### Phase D：稳定后再扩到构建路径

- 再考虑 `src/hnswbuild.c` 的内存压缩策略

---

## 7. 验收标准（你可以直接贴到评审里）

满足以下三条即可认为第一阶段成功：

1. **内存下降**：>= 50%
2. **准确率**：`Recall@10 >= 95%`
3. **时延护栏**：P95 回退不超过 10%

---

## 8. 风险与回滚

### 主要风险

- 热层预算过小导致召回下降
- 预取策略不稳导致 I/O 抖动
- 缓存生命周期处理不当导致内存泄漏

### 回滚方式

```sql
SET hnsw.hot_cold_enabled = off;
```

一键回到旧路径，不改索引格式，回滚成本低。

---

## 9. 推荐初始参数

- `hnsw.hot_cold_enabled = off`（先灰度再开）
- `hnsw.hot_layer = 2`
- `hnsw.hot_max_bytes = 64MB`
- `hnsw.prefetch_neighbors = 16`

若 Recall 接近阈值，按顺序调整：

1. 提高 `ef_search`
2. 增大 `hnsw.hot_max_bytes`
3. 微调 `hnsw.hot_layer`

---

## 10. 你下一步该做什么

- 先按第 6 节完成 **Phase A + B**
- 第一次验证先追求“可测、可回滚”，不要一步到位追极致性能
- 拿到首轮数据后再做参数网格优化

---

## 11. 术语小抄（速查版）

- `HNSW`：一种分层图近似近邻索引，查询时先走高层再落到底层。调参方向：先保证 `ef_search` 再做性能优化。
- `L0`（底层）：包含全部节点，规模最大、最耗内存。调参方向：冷热分层主要就是降低 L0 常驻内存。
- `高层/上层`：节点少但访问频繁，类似导航层。调参方向：`hnsw.hot_layer` 越低，保热范围越大。
- `Entry Point`（入口点）：每次搜索的起点。调参方向：入口稳定有助于减少无效遍历。
- `Recall@10`：前 10 个结果里找对了多少，越高越准。调参方向：不达标先增 `ef_search`。
- `P95/P99`：尾延迟指标，反映慢查询稳定性。调参方向：关注冷热分层是否让尾部更平稳。
- `ef_search`：查询候选集大小，越大通常召回越高但更慢。调参顺序：先提它保召回，再调缓存参数。
- `m`：图连接度，越大图质量常更好但构建/内存更重。调参方向：先固定 `m`，避免与冷热分层耦合过多。
- `Buffer Pool`：PostgreSQL 的页缓存，负责把热点页留在内存。调参方向：冷层数据尽量依赖它按需读取。
- `热数据`：查询高频访问的数据（上层节点、关键候选）。调参方向：预算有限时优先保热高层。
- `冷数据`：访问频率低的数据（大量 L0 节点）。调参方向：按需加载，避免全量常驻。
- `hnsw.hot_cold_enabled`：冷热分层总开关。上线建议：先 `off` 灰度验证，再逐步开启。
- `hnsw.hot_layer`：从哪一层开始优先缓存。经验值：先从 `2` 起步，小步调整。
- `hnsw.hot_max_bytes`：单次扫描热缓存上限。调参方向：Recall 不够时可适当增大。
- `hnsw.prefetch_neighbors`：邻居页预取批量。调参方向：I/O 抖动时先降，再观察延迟变化。

> 一句话记忆：先用 `ef_search` 守住准确率，再用 `hot_layer + hot_max_bytes` 压内存和稳时延。
