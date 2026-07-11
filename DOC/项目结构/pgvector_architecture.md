# pgvector 文件协作关系图与架构说明

## 整体架构概览

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│                         PostgreSQL 服务进程                                       │
│  ┌─────────────────┐   ┌──────────────┐   ┌──────────────┐  ┌────────────────┐  │
│  │  SQL Parser /   │   │  Query       │   │  Executor    │  │  Storage       │  │
│  │  Planner        │   │  Optimizer   │   │              │  │  Manager       │  │
│  └────────┬────────┘   └──────┬───────┘   └──────┬───────┘  └───────┬────────┘  │
│           │                  │                   │                  │            │
│           └──────────────────┴────────── Index AM API ─────────────┘            │
│                                               │                                  │
│  ┌────────────────────────────────────────────▼─────────────────────────────┐   │
│  │                    pgvector 扩展（vector.so / vector.dll）                │   │
│  │                                                                          │   │
│  │   ┌───────────┐     ┌────────────────────┐     ┌────────────────────┐   │   │
│  │   │  vector.c │     │   HNSW 索引子系统    │     │  IVFFlat 索引子系统 │   │   │
│  │   │  (入口+    │     │                    │     │                    │   │   │
│  │   │   类型)    │     │  hnsw.c (注册/初始化)│     │  ivfflat.c (注册)  │   │   │
│  │   └─────┬─────┘     │  hnswbuild.c (构建) │     │  ivfbuild.c (构建) │   │   │
│  │         │           │  hnswinsert.c (插入)│     │  ivfinsert.c (插入)│   │   │
│  │         │           │  hnswscan.c (查询)  │     │  ivfscan.c (查询)  │   │   │
│  │         │           │  hnswutils.c (算法) │     │  ivfkmeans.c (聚类)│   │   │
│  │         │           │  hnswvacuum.c (清理)│     │  ivfutils.c (工具) │   │   │
│  │         │           └────────────────────┘     │  ivfvacuum.c (清理)│   │   │
│  │         │                                       └────────────────────┘   │   │
│  │         │           ┌────────────────────────────────────────────────┐   │   │
│  │         │           │              向量类型子系统                    │   │   │
│  │         └──────────►│  vector.h    (float32 向量类型)                │   │   │
│  │                     │  halfvec.h/c (float16 半精度向量)              │   │   │
│  │                     │  sparsevec.h/c (稀疏向量)                      │   │   │
│  │                     │  bitvec.h/c  (二值向量)                        │   │   │
│  │                     └────────────────────────────────────────────────┘   │   │
│  │                                                                          │   │
│  │                     ┌────────────────────────────────────────────────┐   │   │
│  │                     │           硬件加速子系统                        │   │   │
│  │                     │  halfutils.h/c (F16C SIMD 半精度距离计算)       │   │   │
│  │                     │  bitutils.h/c  (AVX-512 位向量距离计算)         │   │   │
│  │                     └────────────────────────────────────────────────┘   │   │
│  └──────────────────────────────────────────────────────────────────────────┘   │
│                                                                                  │
│  ┌────────────────────────────────────────────────────────────────────────────┐  │
│  │                     配置与 SQL 定义文件                                    │  │
│  │  vector.control   - 扩展版本和依赖声明                                     │  │
│  │  sql/vector.sql   - CREATE TYPE/FUNCTION/OPERATOR/INDEX 定义               │  │
│  │  sql/vector--*.sql- 各版本升级脚本                                          │  │
│  │  Makefile         - 编译规则（PGXS 扩展构建系统）                           │  │
│  └────────────────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────────────────┘
```

**问题：**首先是DBMS也就是postgresql自带的结构，也即计划器、查询优化器、执行器和存储管理器，这都是sql自带的。而pgvector应是pg的扩展组件，依附于pg实现功能，而二者之间依靠Index AM API相联系，这个index am api具体是什么？大致包含何种内容？HNSW和IVFFlat索引子系统我大致理解，应该是用于构建、维护向量索引的部分，并可实现数据的增删改查功能；重点是vector.c，它与向量类型子系统息息相关，那它是否是用于确定存储向量类型的工具？我感觉它可能还是整个扩展系统的管理器，否则索引子系统将无法被调用；至于硬件加速子系统，我认为其可能是可选项，用于常规计算方式的替换，可能为索引子系统所使用；至于配置与SQL定义文件，这属于pg，用途我不太清楚，是否是将SQL语句翻译为计划器可以理解的语句？最后一个问题，pg是关系型数据库，因此查询方式可能并不是HNSW或是IVFFlat，向量存储在数据库中时，和其它关系型数据的关系是怎样的？它们是不是依靠两套不同的索引？

**解答：**

1) Index AM API 到底是什么？
它是 PostgreSQL 提供给“索引实现者”的一组标准接口契约。 你可以把它理解成：**“只要你实现这套回调函数，PG 就能把你的索引当成一等公民使用”**。
大致包含几类能力（概念层面）：
建索引：build、buildempty
插入/删除相关维护：insert、bulkdelete、vacuumcleanup
查询扫描：beginscan、rescan、gettuple/getbitmap、endscan
优化器支持：cost estimate（让 planner 知道大概代价）
索引选项与元信息：options、validate 等
所以，PG 与 HNSW/IVFFlat 的连接点，核心就是这套 AM 回调。

2) 你说 vector.c 是不是“管理器”？
这个点要修正：vector.c 不是整个扩展的总管理器。 它主要负责的是：
vector 数据类型本身（输入/输出、存储格式、基础运算）
距离函数（L2、内积、cosine 等）
和 SQL 运算符的函数绑定
索引子系统（hnsw*.c、ivf*.c）会调用“距离计算能力”，但**它们的生命周期和扫描流程由各自 AM 实现负责，不靠 vector.c 调度**。

3) HNSW/IVFFlat 是不是“实现增删改查”？
更准确说法：
**表数据（行） 的增删改查由 PostgreSQL heap/执行器负责**
**HNSW/IVFFlat 负责的是索引加速（特别是 ANN 检索）**
插入/更新/删除时，PG 会驱动索引 AM 做对应维护
所以不是“索引系统单独实现一套 CRUD”，而是“PG 的 CRUD + 索引协同维护”。

4) 硬件加速子系统是可选替换吗？
你的理解基本对： 它通常是 SIMD/CPU 指令优化等“快路径”，用于加速距离计算等热点逻辑。语义不变，只是更快。没有这些加速，通常也有基础实现可运行（性能不同）。

5) vector.control / sql/vector.sql 是做 SQL 翻译吗？
这个点要修正：不是 SQL 翻译器。
vector.control：扩展元信息（版本、入口等）
sql/vector.sql：在安装扩展时执行，向 PG catalog 注册对象 （类型、函数、操作符、操作符类、索引方法关联等）
也就是说它们是在“**声明和注册能力**”，不是把 SQL 翻译给 planner。

6) 关系型数据和向量数据是什么关系？是不是两套索引？
非常关键的问题，你问得很好。
向量通常就是表中的一个列（如 embedding vector(768)）
同一行里可以同时有结构化字段（id, category, created_at）和向量字段
你可以同时建：
普通 B-tree 索引（给结构化过滤/排序）
HNSW 或 IVFFlat 索引（给向量近邻检索）
所以可以理解为：同一张关系表上，按不同列/查询模式使用不同索引，不是两套数据库体系。

---

## 文件依赖关系详图

```
vector.c ──────────────────────────────────────────────────────────────────────────
  │  引用所有头文件，是扩展的唯一入口（_PG_init）
  ├── #include "vector.h"      → 核心向量类型定义
  ├── #include "halfvec.h"     → 半精度向量类型
  ├── #include "halfutils.h"   → 半精度距离函数（通过 HalfToFloat4）
  ├── #include "sparsevec.h"   → 稀疏向量类型
  ├── #include "bitvec.h"      → 位向量类型
  ├── #include "bitutils.h"    → 位向量距离函数
  ├── #include "hnsw.h"        → HNSW 初始化（HnswInit）
  └── #include "ivfflat.h"     → IVFFlat 初始化（IvfflatInit）

hnsw.h ──────────────────────────────────────────────────────────────────────────
  │  被所有 hnsw*.c 引用，定义完整的 HNSW 数据结构
  ├── #include "vector.h"      → HnswElementTuple 中嵌入 Vector 类型
  └── #include <lib/simplehash.h>  → PG 哈希表（tidhash/pointerhash/offsethash）

ivfflat.h ───────────────────────────────────────────────────────────────────────
  │  被所有 ivf*.c 引用，定义完整的 IVFFlat 数据结构
  ├── #include "vector.h"      → IvfflatListData 中嵌入质心 Vector
  └── #include <utils/tuplesort.h> → 用于扫描结果排序

halfutils.h ─────────────────────────────────────────────────────────────────────
  │  被 halfvec.c 和需要半精度距离计算的文件引用
  ├── #include "halfvec.h"     → half 类型定义（HalfToFloat4 等宏/函数）
  └── inline functions        → HalfToFloat4/Float4ToHalf（内联，无 .c 依赖）

bitutils.h ──────────────────────────────────────────────────────────────────────
  │  被 bitvec.c 和 vector.c 引用
  └── 声明 BitHammingDistance/BitJaccardDistance 函数指针

halfutils.c ─────────────────────────────────────────────────────────────────────
  │  实现 HalfvecInit 和四个半精度距离函数（Default + F16C 版本）
  └── #include "halfvec.h"     → half 类型和 HalfToFloat4

bitutils.c ──────────────────────────────────────────────────────────────────────
  │  实现 BitvecInit 和汉明/Jaccard 距离（Default + AVX-512 版本）
  └── #include "halfvec.h"     → 借用 USE_DISPATCH 宏定义
```

---

## pgvector 与 PostgreSQL 的集成方式

### 1. 扩展加载机制

```
CREATE EXTENSION vector;
  │
  ├── 执行 sql/vector.sql：
  │     CREATE TYPE vector (INPUT=vector_in, OUTPUT=vector_out, ...)
  │     CREATE OPERATOR <-> (FUNCTION=l2_distance, ...)
  │     CREATE ACCESS METHOD hnsw TYPE INDEX HANDLER hnswhandler
  │     CREATE ACCESS METHOD ivfflat TYPE INDEX HANDLER ivfflathandler
  │     ...
  │
  └── 加载 vector.so（由 MODULE_PATHNAME 指定）
        → 调用 _PG_init()
              → BitvecInit()   (检测 CPU, 设置函数指针)
              → HalfvecInit()  (检测 CPU, 设置函数指针)
              → HnswInit()     (注册 GUC + reloption)
              → IvfflatInit()  (注册 GUC + reloption)
```

### 2. Index Access Method（索引访问方法）API

PostgreSQL 定义了一套 `IndexAmRoutine` 接口，pgvector 通过实现这些回调与 PG 集成：

```
hnswhandler() / ivfflathandler()
  │
  └── 返回 IndexAmRoutine 结构体，包含：
        ┌─────────────────────────────────────────────────────────┐
        │ 操作          │ HNSW 实现      │ IVFFlat 实现           │
        ├─────────────────────────────────────────────────────────┤
        │ ambuild       │ hnswbuild      │ ivfflatbuild           │
        │ ambuildempty  │ hnswbuildempty │ ivfflatbuildempty      │
        │ aminsert      │ hnswinsert     │ ivfflatinsert          │
        │ ambulkdelete  │ hnswbulkdelete │ ivfflatbulkdelete      │
        │ amvacuumcleanup│hnswvacuumclean│ ivfflatvacuumcleanup   │
        │ amcostestimate│ hnswcost...    │ ivfflatcost...         │
        │ amoptions     │ hnswoptions    │ ivfflatoptions         │
        │ ambeginscan   │ hnswbeginscan  │ ivfflatbeginscan       │
        │ amrescan      │ hnswrescan     │ ivfflatrescan          │
        │ amgettuple    │ hnswgettuple   │ ivfflatgettuple        │
        │ amendscan     │ hnswendscan    │ ivfflatendscan         │
        └─────────────────────────────────────────────────────────┘
```

### 3. 数据在 PostgreSQL Buffer Pool 中的存储布局

```
HNSW 索引文件布局：
  Block 0: HnswMetaPageData
    ├── magicNumber (0xA953A953)
    ├── version
    ├── dimensions
    ├── m, efConstruction
    ├── entryBlkno, entryOffno, entryLevel  ← 当前入口点位置
    └── insertPage  ← 下一次插入的目标页

  Block 1+: 数据页（每页含多个元组）
    ├── HnswElementTuple（type=1）
    │     ├── level, deleted, version
    │     ├── heaptids[10]  ← 对应 heap 行的 TID
    │     ├── neighbortid   ← 指向该元素的 HnswNeighborTuple
    │     └── data (Vector) ← 向量数据
    └── HnswNeighborTuple（type=2）
          ├── count
          └── indextids[]   ← 各层邻居的索引 TID 列表

IVFFlat 索引文件布局：
  Block 0: IvfflatMetaPageData
    ├── magicNumber (0x14FF1A7)
    ├── version, dimensions, lists

  Block 1 ~ lists: 列表头页（每个质心一个）
    └── IvfflatListData
          ├── startPage   ← 该列表第一个数据页
          ├── insertPage  ← 该列表当前插入页
          └── center (Vector) ← 质心向量

  后续页: 向量数据页
    └── 每行：(vector_datum, heap_tid)
```

### 4. WAL（Write-Ahead Logging）与崩溃恢复

```
HNSW 构建期（hnswbuild）：
  使用 smgrwrite 直接写磁盘（绕过 WAL），
  依靠 CREATE INDEX 的事务原子性保证一致性（失败则回滚整个索引）

HNSW 插入期（hnswinsert）：
  使用 START_CRIT_SECTION / END_CRIT_SECTION 标记，
  通过 MarkBufferDirty + GenericXLogFinish 记录 WAL

IVFFlat 全程：
  使用 generic_xlog（GenericXLogStart/Finish）记录所有页修改，
  确保崩溃后可通过 WAL 重放恢复索引一致性
```

### 5. 并行索引构建

```
HNSW 并行构建（ParallelBuildGraph）：
  Leader 进程                    Worker 进程（N 个）
    │                               │
    ├── 分配共享内存（HnswShared）  ←─┤
    ├── 启动 worker（LaunchParallelWorkers）
    │                               ├── 扫描 heap 的一个分区
    │                               ├── 对每个向量：加锁→插入共享图→解锁
    │                               └── 结束后发送 workersdonecv 信号
    ├── 等待所有 worker 完成
    └── FlushGraph：将共享内存中的图写入磁盘页

IVFFlat 并行构建（AssignTuples）：
  Leader 进程                    Worker 进程（N 个）
    │                               │
    ├── 共享质心数据（ivfcenters）  ←─┤
    ├── 启动 worker
    │                               ├── 扫描 heap 分区
    │                               ├── 找最近质心，插入 shared sort
    │                               └── 结束通知
    ├── 合并 sort，写入倒排列表页
    └── 更新列表元数据
```

---

## 各文件功能速查表

| 文件 | 所属子系统 | 核心职责 |
|------|-----------|---------|
| `vector.h` | 类型系统 | float32 向量结构体和宏定义 |
| `vector.c` | 类型系统 | 扩展入口、类型 I/O、距离函数、运算符、聚合 |
| `halfvec.h` | 类型系统 | float16 向量结构体和类型选择宏 |
| `halfvec.c` | 类型系统 | 半精度向量完整实现 |
| `sparsevec.h` | 类型系统 | 稀疏向量结构体和内存布局宏 |
| `sparsevec.c` | 类型系统 | 稀疏向量完整实现 |
| `bitvec.h` | 类型系统 | 位向量初始化函数声明 |
| `bitvec.c` | 类型系统 | 汉明距离、Jaccard 距离函数 |
| `halfutils.h` | 硬件加速 | half↔float 转换内联函数、函数指针声明 |
| `halfutils.c` | 硬件加速 | F16C SIMD 半精度距离计算（+通用版） |
| `bitutils.h` | 硬件加速 | 位距离函数指针声明 |
| `bitutils.c` | 硬件加速 | AVX-512 位距离计算（+通用版）|
| `hnsw.h` | HNSW 索引 | 所有 HNSW 数据结构、常量和函数声明 |
| `hnsw.c` | HNSW 索引 | IndexAmRoutine 注册、GUC 参数、代价估算 |
| `hnswbuild.c` | HNSW 索引 | 索引构建（单进程+并行）、磁盘写入 |
| `hnswinsert.c` | HNSW 索引 | 单行插入、并发控制 |
| `hnswscan.c` | HNSW 索引 | KNN 查询扫描、迭代式扫描 |
| `hnswutils.c` | HNSW 索引 | 图搜索算法、磁盘 I/O、元素管理 |
| `hnswvacuum.c` | HNSW 索引 | 软删除标记、邻居修复、空间回收 |
| `ivfflat.h` | IVFFlat 索引 | 所有 IVFFlat 数据结构和函数声明 |
| `ivfflat.c` | IVFFlat 索引 | IndexAmRoutine 注册、GUC 参数 |
| `ivfbuild.c` | IVFFlat 索引 | 三阶段构建（采样→k-means→写入）|
| `ivfinsert.c` | IVFFlat 索引 | 单行插入到最近质心的倒排列表 |
| `ivfscan.c` | IVFFlat 索引 | probes 个列表的 KNN 扫描 |
| `ivfkmeans.c` | IVFFlat 索引 | k-means++ 初始化 + Lloyd 迭代聚类 |
| `ivfutils.c` | IVFFlat 索引 | 页管理、WAL、列表元数据更新 |
| `ivfvacuum.c` | IVFFlat 索引 | 删除失效条目、统计更新 |
| `sql/vector.sql` | SQL 定义 | 类型/函数/运算符/索引的 DDL 定义 |
| `sql/vector--*.sql` | SQL 定义 | 各版本间的增量升级脚本 |
| `vector.control` | 元数据 | 扩展名、版本、依赖声明 |
| `Makefile` | 构建系统 | PGXS 编译规则（编译→安装→测试）|
| `Dockerfile` | 部署 | 基于 postgres 镜像的容器构建 |
| `.github/workflows/build.yml` | CI | 自动化构建和测试工作流 |
| `test/` | 测试 | 回归测试（.sql 输入 + .out 期望输出）|

---

## 查询执行示例：KNN 搜索完整调用链

```sql
SELECT * FROM items ORDER BY embedding <-> '[1,2,3]' LIMIT 5;
```

```
PostgreSQL Planner
  │  发现 <-> 运算符对应 HNSW/IVFFlat 索引
  │  调用 hnswcostestimate 估算索引代价
  └── 生成 Index Scan 计划节点

PostgreSQL Executor
  │
  ├── hnswbeginscan(index, nkeys=0, norderbys=1)
  │     → 分配 HnswScanOpaqueData，加载 typeInfo 和 support 函数
  │
  ├── hnswrescan(scan, orderbys=[('[1,2,3]', <->)])
  │     → 解析查询向量 '[1,2,3]' → Vector
  │     → 若需要归一化（余弦距离），调用 HnswNormValue
  │     → 存入 so->q
  │
  ├── hnswgettuple(scan, ForwardScanDirection)  [第 1 次调用]
  │     → so->first == true，执行完整图搜索：
  │           HnswGetMetaPageInfo(index, &m, &entryPoint)
  │           for layer = entryPoint->level downto 1:
  │               ep = HnswSearchLayer(ep, q, ef=1, layer) // 贪心逼近
  │           so->w = HnswSearchLayer(ep, q, ef_search, layer=0)
  │     → 从 so->w 中取距离最近的候选
  │     → scan->xs_heaptid = candidate->heaptid
  │     → return true
  │
  ├── hnswgettuple(scan, ...)  [第 2-5 次调用]
  │     → so->first == false，直接从 so->w 中取下一个候选
  │     → return true
  │
  ├── hnswgettuple(scan, ...)  [第 6 次调用]
  │     → LIMIT 5 已满足，Executor 不再调用
  │
  └── hnswendscan(scan)
        → 释放 so->tmpCtx 和相关资源
```

---

## 新手学习路线：从“会用”到“看懂实现”

> 目标：按最小认知负担逐步进入 pgvector 源码，不要求一开始就理解全部细节。

### 阶段 0：先把系统边界看清（半天）

**先读文件**
- `README.md`
- `pgvector_architecture.md`（本文）
- `sql/vector.sql`

**学习目标**
- 明确 pgvector 对外提供了什么：数据类型、距离算子、索引类型（HNSW/IVFFlat）
- 先建立“SQL 功能 -> C 函数符号”的映射，不急着深挖实现

**阶段产出**
- 你能回答：`<->`、`<=>`、`<#>` 分别对应哪类距离
- 你能指出：HNSW 与 IVFFlat 的入口 handler 在哪里

**自检方式**
- 在 `sql/vector.sql` 里定位 `CREATE FUNCTION` 与 `CREATE OPERATOR`
- 在 `src/hnsw.c`、`src/ivfflat.c` 中找到 `hnswhandler`、`ivfflathandler`

### 阶段 1：理解扩展入口与类型系统（1~2 天）

**先读文件**
- `src/vector.c`
- `src/vector.h`
- `src/halfvec.c` / `src/halfvec.h`
- `src/sparsevec.c` / `src/sparsevec.h`
- `src/bitvec.c` / `src/bitvec.h`

**学习目标**
- 理解 `_PG_init()` 做了哪些初始化（CPU dispatch、索引子系统初始化）
- 理解 PostgreSQL 自定义类型常见函数：输入输出、比较、距离计算、聚合支持
- 看懂 4 类向量在内存布局上的差异（dense/half/sparse/bit）

**阶段产出**
- 画出“SQL 函数名 -> C 实现函数 -> 依赖头文件”的小图
- 能解释为什么 `halfvec` 与 `bitvec` 需要单独的 `*utils.c`

**自检方式**
- 对照 `test/sql/vector_type.sql`、`test/sql/halfvec.sql`、`test/sql/bit.sql`
- 对照 `test/expected/*.out`，确认自己对函数行为的理解一致

### 阶段 2：先打通 HNSW 主链路（2~3 天）

**先读文件（推荐顺序）**
- `src/hnsw.h`
- `src/hnsw.c`
- `src/hnswscan.c`
- `src/hnswinsert.c`
- `src/hnswbuild.c`
- `src/hnswutils.c`
- `src/hnswvacuum.c`

**学习目标**
- 先理解“查询路径”再回看“构建路径”：`beginscan -> rescan -> gettuple`
- 理解 HNSW 元页、元素元组、邻居元组如何落盘
- 理解插入并发控制与 vacuum 修复的基本思想

**阶段产出**
- 能口述一次 KNN 查询完整调用链（参照本文上一节）
- 能说明 `ef_search`、`m`、`ef_construction` 对行为和性能的影响方向

**自检方式**
- 阅读 `test/sql/hnsw_vector.sql`、`test/sql/hnsw_halfvec.sql`、`test/sql/hnsw_bit.sql`
- 对照 `test/expected/hnsw_*.out` 观察搜索结果与排序行为

### 阶段 3：再打通 IVFFlat 主链路（2~3 天）

**先读文件（推荐顺序）**
- `src/ivfflat.h`
- `src/ivfflat.c`
- `src/ivfscan.c`
- `src/ivfinsert.c`
- `src/ivfbuild.c`
- `src/ivfkmeans.c`
- `src/ivfutils.c`
- `src/ivfvacuum.c`

**学习目标**
- 理解 IVFFlat 三阶段构建：采样 -> 聚类 -> 写入列表
- 理解查询时 `probes` 的作用：召回率与延迟的折中
- 理解列表页组织、WAL 记录与 vacuum 清理过程

**阶段产出**
- 能解释“为什么 IVF 要先训练聚类中心”
- 能说明 `lists`、`probes` 的调参直觉

**自检方式**
- 阅读 `test/sql/ivfflat_vector.sql`、`test/sql/ivfflat_halfvec.sql`、`test/sql/ivfflat_bit.sql`
- 对照 `test/expected/ivfflat_*.out`，验证对 top-k 行为的理解

### 阶段 4：横向专题与工程化能力（持续）

**建议专题**
- 并行构建：`hnswbuild.c`、`ivfbuild.c` 中 shared state 与 worker 协作
- WAL 与崩溃恢复：`hnswinsert.c`、`ivfutils.c`
- CPU 指令分发：`halfutils.c`、`bitutils.c`
- 升级兼容：`sql/vector--*.sql` 的增量变更模式

**目标**
- 不只“看懂”，而是具备修改/调参/定位性能问题的能力

**建议实践**
- 每看完一个模块就做一次“最小改动实验”（例如新增调试日志、调整默认参数并观察测试变化）

---

## 学习 pgvector 需要补齐的背景知识

> 建议按“必需 -> 进阶”补齐，不要一次性把所有理论学完再看代码。

### A. 必需背景（先掌握，能直接提升读码效率）

| 主题 | 你需要掌握什么 | 在 pgvector 中对应位置 |
|------|----------------|------------------------|
| PostgreSQL 扩展机制 | `CREATE EXTENSION`、`_PG_init()`、`PG_FUNCTION_INFO_V1` | `sql/vector.sql`、`src/vector.c` |
| PostgreSQL Datum/内存上下文 | varlena、`palloc`、MemoryContext 基本使用 | `src/vector.c`、`src/*vec.c` |
| 自定义类型与操作符 | type I/O、operator class、support function | `sql/vector.sql`、`src/vector.c` |
| Index AM 接口 | `IndexAmRoutine` 回调生命周期 | `src/hnsw.c`、`src/ivfflat.c` |
| 基础向量检索概念 | L2 / cosine / inner product，top-k 近邻 | `src/vector.c`、`test/sql/*.sql` |

### B. 重要进阶（进入索引实现前后补齐）

| 主题 | 你需要掌握什么 | 在 pgvector 中对应位置 |
|------|----------------|------------------------|
| HNSW 算法 | 分层图、贪心下降、候选集扩展 | `src/hnswscan.c`、`src/hnswutils.c` |
| IVF/IVFFlat 算法 | k-means++、Lloyd 迭代、倒排列表 | `src/ivfkmeans.c`、`src/ivfbuild.c` |
| PostgreSQL Buffer/WAL | buffer 脏页、WAL 记录、崩溃恢复语义 | `src/hnswinsert.c`、`src/ivfutils.c` |
| 并发与锁 | page lock、LWLock、临界区语义 | `src/hnswinsert.c`、`src/hnswbuild.c` |
| Vacuum 机制 | dead tuple 清理、统计更新 | `src/hnswvacuum.c`、`src/ivfvacuum.c` |

### C. 性能优化背景（按需学习）

| 主题 | 你需要掌握什么 | 在 pgvector 中对应位置 |
|------|----------------|------------------------|
| SIMD 指令基础 | AVX/F16C 的收益边界、fallback 路径 | `src/halfutils.c`、`src/bitutils.c` |
| cache/内存局部性 | 顺序访问 vs 随机访问对吞吐的影响 | `src/hnswutils.c`、`src/ivfscan.c` |
| 参数调优方法 | 精度-延迟-内存三角平衡 | HNSW/IVFFlat GUC 与 reloptions |

### D. 推荐学习顺序（背景知识版）

1. 先会用 PostgreSQL 扩展与自定义类型（A 前三项）
2. 再看 Index AM 生命周期（A 第四项）
3. 然后补 HNSW/IVF 算法直觉（B 前两项）
4. 最后进入 WAL/并发/SIMD 等工程细节（B 后三项 + C）

### E. 常见卡点与建议

- **卡点 1：** 看不懂宏和 PG 风格类型转换。\
  **建议：** 先从 `src/vector.c` 的 I/O 与距离函数读起，避免一开始进入复杂索引代码。
- **卡点 2：** 算法能懂，但不理解为什么这么落盘。\
  **建议：** 结合本文“索引文件布局”小节，对照 `hnsw.h` / `ivfflat.h` 数据结构看。
- **卡点 3：** SQL 行为与 C 代码对应不上。\
  **建议：** 固定使用“`test/sql/*.sql` 输入 + `test/expected/*.out` 输出”双向验证。
- **卡点 4：** 读到并发代码容易失去主线。\
  **建议：** 先按单线程逻辑理解，再补锁与临界区，不要反过来。
