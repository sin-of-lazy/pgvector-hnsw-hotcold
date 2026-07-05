# pgvector

Postgres 开源向量相似度搜索扩展

将向量与您的其他数据存储在一起。支持：

  - 精确和近似最近邻搜索
  - 单精度、半精度、二进制和稀疏向量
  - L2 距离、内积、余弦距离、L1 距离、汉明距离（Hamming distance）和雅卡尔距离（Jaccard distance）
  - 任何带有 Postgres 客户端的 [语言](https://www.google.com/search?q=%23%E8%AF%AD%E8%A8%80)

此外，还具备 [ACID](https://en.wikipedia.org/wiki/ACID) 合规性、点进点恢复（PITR）、JOIN 查询以及 Postgres 的所有其他 [优秀特性](https://www.postgresql.org/about/)。

[](https://github.com/pgvector/pgvector/actions)

## 安装

### Linux 和 Mac

编译并安装扩展（支持 Postgres 13+）：

```sh
cd /tmp
git clone --branch v0.8.2 https://github.com/pgvector/pgvector.git
cd pgvector
make
make install # 可能需要 sudo
```

如果遇到问题，请参阅 [安装说明 - Linux 和 Mac](https://www.google.com/search?q=%23%E5%AE%89%E8%A3%85%E8%AF%B4%E6%98%8E---linux-%E5%92%8C-mac)。

您还可以通过 [Docker](https://www.google.com/search?q=%23docker)、[Homebrew](https://www.google.com/search?q=%23homebrew)、[PGXN](https://www.google.com/search?q=%23pgxn)、[APT](https://www.google.com/search?q=%23apt)、[Yum](https://www.google.com/search?q=%23yum)、[pkg](https://www.google.com/search?q=%23pkg)、[APK](https://www.google.com/search?q=%23apk) 或 [conda-forge](https://www.google.com/search?q=%23conda-forge) 进行安装，该扩展已预装在 [Postgres.app](https://www.google.com/search?q=%23postgresapp) 和许多 [托管服务商](https://www.google.com/search?q=%23%E6%89%98%E7%AE%A1-postgres) 中。此外还有针对 [GitHub Actions](https://github.com/pgvector/setup-pgvector) 的指令。

### Windows

确保已安装 [Visual Studio 中的 C++ 支持](https://learn.microsoft.com/en-us/cpp/build/building-on-the-command-line?view=msvc-170#download-and-install-the-tools)，并以管理员身份运行 `x64 Native Tools Command Prompt for VS [版本]`。然后使用 `nmake` 进行构建：

```cmd
set "PGROOT=C:\Program Files\PostgreSQL\18"
cd %TEMP%
git clone --branch v0.8.2 https://github.com/pgvector/pgvector.git
cd pgvector
nmake /F Makefile.win
nmake /F Makefile.win install
```

如果遇到问题，请参阅 [安装说明 - Windows](https://www.google.com/search?q=%23%E5%AE%89%E8%A3%85%E8%AF%B4%E6%98%8E---windows)。

您还可以通过 [Docker](https://www.google.com/search?q=%23docker) 或 [conda-forge](https://www.google.com/search?q=%23conda-forge) 安装。

## 快速入门

启用扩展（在每个需要使用的数据库中执行一次）：

```tsql
CREATE EXTENSION vector;
```

创建一个具有 3 维向量列的表：

```sql
CREATE TABLE items (id bigserial PRIMARY KEY, embedding vector(3));
```

插入向量：

```sql
INSERT INTO items (embedding) VALUES ('[1,2,3]'), ('[4,5,6]');
```

通过 L2 距离获取最近邻：

```sql
SELECT * FROM items ORDER BY embedding <-> '[3,1,2]' LIMIT 5;
```

同时支持内积 (`<#>`)、余弦距离 (`<=>`) 和 L1 距离 (`<+>`)。

注意：`<#>` 返回负内积，因为 Postgres 的索引扫描仅支持 `ASC`（升序）排序。

## 存储

创建一个带有向量列的新表：

```sql
CREATE TABLE items (id bigserial PRIMARY KEY, embedding vector(3));
```

或者向现有表中添加向量列：

```sql
ALTER TABLE items ADD COLUMN embedding vector(3);
```

还支持 [半精度向量](https://www.google.com/search?q=%23%E5%8D%8A%E7%B2%BE%E5%BA%A6%E5%90%91%E9%87%8F)、[二进制向量](https://www.google.com/search?q=%23%E4%BA%8C%E8%BF%9B%E5%88%B6%E5%90%91%E9%87%8F) 和 [稀疏向量](https://www.google.com/search?q=%23%E7%A8%80%E7%96%8F%E5%90%91%E9%87%8F)。

插入向量：

```sql
INSERT INTO items (embedding) VALUES ('[1,2,3]'), ('[4,5,6]');
```

或者使用 `COPY` 批量加载向量（[示例](https://github.com/pgvector/pgvector-python/blob/master/examples/loading/example.py)）：

```sql
COPY items (embedding) FROM STDIN WITH (FORMAT BINARY);
```

Upsert（插入或更新）向量：

```sql
INSERT INTO items (id, embedding) VALUES (1, '[1,2,3]'), (2, '[4,5,6]')
    ON CONFLICT (id) DO UPDATE SET embedding = EXCLUDED.embedding;
```

更新向量：

```sql
UPDATE items SET embedding = '[1,2,3]' WHERE id = 1;
```

删除向量：

```sql
DELETE FROM items WHERE id = 1;
```

## 查询

获取与某个向量最近的邻居：

```sql
SELECT * FROM items ORDER BY embedding <-> '[3,1,2]' LIMIT 5;
```

支持的距离函数包括：

  - `<->` - L2 距离
  - `<#>` - （负）内积
  - `<=>` - 余弦距离
  - `<+>` - L1 距离
  - `<~>` - 汉明距离（用于二进制向量）
  - `<%>` - 雅卡尔距离（用于二进制向量）

获取与某一行最近的邻居：

```sql
SELECT * FROM items WHERE id != 1 ORDER BY embedding <-> (SELECT embedding FROM items WHERE id = 1) LIMIT 5;
```

获取指定距离范围内的行：

```sql
SELECT * FROM items WHERE embedding <-> '[3,1,2]' < 5;
```

注意：结合 `ORDER BY` 和 `LIMIT` 可以利用索引加速查询。

#### 距离计算

获取距离值：

```sql
SELECT embedding <-> '[3,1,2]' AS distance FROM items;
```

对于内积，需乘以 -1（因为 `<#>` 返回负内积）：

```tsql
SELECT (embedding <#> '[3,1,2]') * -1 AS inner_product FROM items;
```

对于余弦相似度，使用 1 减去余弦距离：

```sql
SELECT 1 - (embedding <=> '[3,1,2]') AS cosine_similarity FROM items;
```

#### 聚合

计算向量平均值：

```sql
SELECT AVG(embedding) FROM items;
```

对分组向量求平均：

```sql
SELECT category_id, AVG(embedding) FROM items GROUP BY category_id;
```

## 索引

默认情况下，pgvector 执行精确最近邻搜索，这能提供完美的召回率。

您可以添加索引以使用近似最近邻（ANN）搜索，这会牺牲一定的召回率来换取速度。与典型索引不同，添加近似索引后，您可能会在查询结果中看到细微差异。

支持的索引类型有：

  - [HNSW](https://www.google.com/search?q=%23hnsw)
  - [IVFFlat](https://www.google.com/search?q=%23ivfflat)

## HNSW

HNSW 索引通过创建一个多层图来实现。在速度与召回率的平衡上，它的查询性能优于 IVFFlat，但构建速度较慢且内存占用更多。此外，HNSW 可以在表中没有数据时直接创建索引，因为它不像 IVFFlat 那样需要训练步骤。

为您希望使用的每个距离函数添加索引。

L2 距离：

```sql
CREATE INDEX ON items USING hnsw (embedding vector_l2_ops);
```

注意：对于 `halfvec` 类型使用 `halfvec_l2_ops`，对于 `sparsevec` 使用 `sparsevec_l2_ops`（其他距离函数以此类推）。

内积：

```sql
CREATE INDEX ON items USING hnsw (embedding vector_ip_ops);
```

余弦距离：

```sql
CREATE INDEX ON items USING hnsw (embedding vector_cosine_ops);
```

L1 距离：

```sql
CREATE INDEX ON items USING hnsw (embedding vector_l1_ops);
```

汉明距离：

```sql
CREATE INDEX ON items USING hnsw (embedding bit_hamming_ops);
```

雅卡尔距离：

```sql
CREATE INDEX ON items USING hnsw (embedding bit_jaccard_ops);
```

支持的类型包括：

  - `vector` - 最高 2,000 维
  - `halfvec` - 最高 4,000 维
  - `bit` - 最高 64,000 维
  - `sparsevec` - 最高 1,000 个非零元素

### 索引选项

指定 HNSW 参数：

  - `m` - 每层最大连接数（默认 16）
  - `ef_construction` - 构建图时的动态候选列表大小（默认 64）

<!-- end list -->

```sql
CREATE INDEX ON items USING hnsw (embedding vector_l2_ops) WITH (m = 16, ef_construction = 64);
```

较高的 `ef_construction` 值可提供更好的召回率，但会增加索引构建时间和插入成本。

### 查询选项

指定搜索时的动态候选列表大小（默认 40）：

```sql
SET hnsw.ef_search = 100;
```

较高的值可提供更好的召回率，但会降低查询速度。

在事务内使用 `SET LOCAL` 以仅针对单个查询进行设置：

```sql
BEGIN;
SET LOCAL hnsw.ef_search = 100;
SELECT ...
COMMIT;
```

### 索引构建时间

当图能够完全装入 `maintenance_work_mem` 时，索引构建速度最快：

```sql
SET maintenance_work_mem = '8GB';
```

如果图超出了 `maintenance_work_mem` 的范围，系统会显示通知：

```text
NOTICE:  hnsw graph no longer fits into maintenance_work_mem after 100000 tuples
DETAIL:  Building will take significantly more time.
HINT:  Increase maintenance_work_mem to speed up builds.
```

注意：请勿将 `maintenance_work_mem` 设置过高，以免耗尽服务器内存。

与其他索引类型一样，在加载初始数据后再创建索引会更快。

您还可以通过增加并行工作进程数（默认 2）来加速索引创建：

```sql
SET max_parallel_maintenance_workers = 7; -- 加上 leader
```

对于大量的并行工人，可能还需要增加 `max_parallel_workers`（默认 8）。

[索引选项](https://www.google.com/search?q=%23%E7%B4%A2%E5%BC%95%E9%80%89%E9%A1%B9) 对构建时间也有显著影响（除非召回率过低，否则建议使用默认值）。

### 索引进度

查看 [索引进度](https://www.postgresql.org/docs/current/progress-reporting.html#CREATE-INDEX-PROGRESS-REPORTING)：

```sql
SELECT phase, round(100.0 * blocks_done / nullif(blocks_total, 0), 1) AS "%" FROM pg_stat_progress_create_index;
```

HNSW 的阶段包括：

1.  `initializing` (初始化)
2.  `loading tuples` (加载元组)

## IVFFlat

IVFFlat 索引将向量划分为多个列表，然后搜索与查询向量最接近的一组子列表。它的构建速度比 HNSW 快，内存占用更少，但查询性能较低（就速度/召回率权衡而言）。

实现高召回率的三个关键点：

1.  在表中已有数据 **后** 创建索引。
2.  选择合适的列表数量 - 建议 100 万行以内使用 `rows / 1000`，100 万行以上使用 `sqrt(rows)`。
3.  查询时，指定合适的 [probes](https://www.google.com/search?q=%23%E6%9F%A5%E8%AF%A2%E9%80%89%E9%A1%B9-1) 数量（越大召回率越高，越小速度越快）- 建议从 `sqrt(lists)` 开始。

为您希望使用的每个距离函数添加索引。

L2 距离：

```sql
CREATE INDEX ON items USING ivfflat (embedding vector_l2_ops) WITH (lists = 100);
```

注意：对于 `halfvec` 使用 `halfvec_l2_ops`（其他距离函数以此类推）。

内积：

```sql
CREATE INDEX ON items USING ivfflat (embedding vector_ip_ops) WITH (lists = 100);
```

余弦距离：

```sql
CREATE INDEX ON items USING ivfflat (embedding vector_cosine_ops) WITH (lists = 100);
```

汉明距离：

```sql
CREATE INDEX ON items USING ivfflat (embedding bit_hamming_ops) WITH (lists = 100);
```

支持的类型包括：

  - `vector` - 最高 2,000 维
  - `halfvec` - 最高 4,000 维
  - `bit` - 最高 64,000 维

### 查询选项

指定探查列表（probes）的数量（默认 1）：

```sql
SET ivfflat.probes = 10;
```

值越大召回率越高，但速度越慢。如果设置为列表总数，则变为精确最近邻搜索（此时优化器可能不再使用索引）。

在事务内使用 `SET LOCAL` 以仅针对单个查询设置：

```sql
BEGIN;
SET LOCAL ivfflat.probes = 10;
SELECT ...
COMMIT;
```

### 索引构建时间

通过增加并行工作进程数（默认 2）来加速大型表上的索引创建：

```sql
SET max_parallel_maintenance_workers = 7; -- 加上 leader
```

对于大量的并行工人，可能还需要增加 `max_parallel_workers`（默认 8）。

### 索引进度

查看 [索引进度](https://www.postgresql.org/docs/current/progress-reporting.html#CREATE-INDEX-PROGRESS-REPORTING)：

```sql
SELECT phase, round(100.0 * tuples_done / nullif(tuples_total, 0), 1) AS "%" FROM pg_stat_progress_create_index;
```

IVFFlat 的阶段包括：

1.  `initializing` (初始化)
2.  `performing k-means` (执行 k-means 聚类)
3.  `assigning tuples` (分配元组)
4.  `loading tuples` (加载元组)

注意：`%` 仅在 `loading tuples` 阶段有值。

## 过滤

有几种方法可以对带有 `WHERE` 子句的最近邻查询进行索引。

```sql
SELECT * FROM items WHERE category_id = 123 ORDER BY embedding <-> '[3,1,2]' LIMIT 5;
```

建议先在过滤列上创建索引。在许多情况下，这可以提供快速且精确的搜索。Postgres 提供了多种 [索引类型](https://www.postgresql.org/docs/current/indexes-types.html)：B-tree（默认）、hash、GiST、SP-GiST、GIN 和 BRIN。

```sql
CREATE INDEX ON items (category_id);
```

如果是多列过滤，考虑 [多列索引](https://www.postgresql.org/docs/current/indexes-multicolumn.html)。

```sql
CREATE INDEX ON items (location_id, category_id);
```

当条件匹配的行数百分比较低时，精确索引效果很好。否则，[近似索引](https://www.google.com/search?q=%23%E7%B4%A2%E5%BC%95) 可能表现更好。

```sql
CREATE INDEX ON items USING hnsw (embedding vector_l2_ops);
```

使用近似索引时，过滤是在扫描索引 **之后** 应用的。如果条件匹配 10% 的行，而在 HNSW 默认 `hnsw.ef_search = 40` 的情况下，平均只有 4 行会匹配条件。如需更多结果，请增加 `hnsw.ef_search`。

```sql
SET hnsw.ef_search = 200;
```

从 0.8.0 版本开始，您可以启用 [迭代索引扫描](https://www.google.com/search?q=%23%E8%BF%AD%E4%BB%A3%E7%B4%A2%E5%BC%95%E6%89%AB%E6%8F%8F)，它会在需要时自动扫描更多索引。

```sql
SET hnsw.iterative_scan = strict_order;
```

如果过滤的值只有几个固定的枚举值，考虑 [部分索引（partial indexing）](https://www.postgresql.org/docs/current/indexes-partial.html)。

```sql
CREATE INDEX ON items USING hnsw (embedding vector_l2_ops) WHERE (category_id = 123);
```

如果过滤的值非常多，考虑 [分区（partitioning）](https://www.postgresql.org/docs/current/ddl-partitioning.html)。

```sql
CREATE TABLE items (embedding vector(3), category_id int) PARTITION BY LIST(category_id);
```

## 迭代索引扫描

使用近似索引时，带过滤条件的查询可能会返回较少结果，因为过滤是在索引扫描后进行的。从 0.8.0 开始，您可以启用迭代索引扫描，它会自动持续扫描索引，直到找到足够的匹配结果（或达到 `hnsw.max_scan_tuples` 或 `ivfflat.max_probes`）。

迭代扫描可以使用“严格”或“宽松”排序。

**Strict（严格）** 确保结果严格按距离排序：

```sql
SET hnsw.iterative_scan = strict_order;
```

**Relaxed（宽松）** 允许结果在距离上略微失序，但能提供更好的召回率：

```sql
SET hnsw.iterative_scan = relaxed_order;
# 或者
SET ivfflat.iterative_scan = relaxed_order;
```

在宽松排序下，您可以使用 [物化 CTE](https://www.postgresql.org/docs/current/queries-with.html#QUERIES-WITH-CTE-MATERIALIZATION) 来获得严格排序：

```sql
WITH relaxed_results AS MATERIALIZED (
    SELECT id, embedding <-> '[1,2,3]' AS distance FROM items WHERE category_id = 123 ORDER BY distance LIMIT 5
) SELECT * FROM relaxed_results ORDER BY distance + 0;
```

注意：Postgres 17+ 需要 `+ 0` 技巧。

对于按距离过滤的查询，为了获得最佳性能，请使用物化 CTE 并将距离过滤放在外部（由于 Postgres 执行器的 [当前行为](https://www.postgresql.org/message-id/flat/CAOdR5yGUoMQ6j7M5hNUXrySzaqZVGf_Ne%2B8fwZMRKTFxU1nbJg%40mail.gmail.com)）：

```sql
WITH nearest_results AS MATERIALIZED (
    SELECT id, embedding <-> '[1,2,3]' AS distance FROM items ORDER BY distance LIMIT 5
) SELECT * FROM nearest_results WHERE distance < 5 ORDER BY distance;
```

注意：将任何其他过滤器放在 CTE 内部。

### 迭代扫描选项

由于扫描大部分近似索引代价高昂，可以使用以下选项控制扫描何时结束。

#### HNSW

指定访问的最大元组数（默认 20,000）：

```sql
SET hnsw.max_scan_tuples = 20000;
```

注意：这是近似值，且不影响初始扫描。

指定使用的最大内存量，作为 `work_mem` 的倍数（默认 1）：

```sql
SET hnsw.scan_mem_multiplier = 2;
```

注意：如果增加 `hnsw.max_scan_tuples` 未能提高召回率，请尝试增加此项。

#### IVFFlat

指定最大探查次数：

```sql
SET ivfflat.max_probes = 100;
```

注意：如果此值低于 `ivfflat.probes`，将使用 `ivfflat.probes`。

## 半精度向量

使用 `halfvec` 类型存储半精度向量（16 位浮点数）：

```sql
CREATE TABLE items (id bigserial PRIMARY KEY, embedding halfvec(3));
```

## 半精度索引

以半精度对向量建立索引，以减小索引体积：

```sql
CREATE INDEX ON items USING hnsw ((embedding::halfvec(3)) halfvec_l2_ops);
```

查询最近邻：

```sql
SELECT * FROM items ORDER BY embedding::halfvec(3) <-> '[1,2,3]' LIMIT 5;
```

## 二进制向量

使用 `bit` 类型存储二进制向量（[示例](https://github.com/pgvector/pgvector-python/blob/master/examples/imagehash/example.py)）：

```sql
CREATE TABLE items (id bigserial PRIMARY KEY, embedding bit(3));
INSERT INTO items (embedding) VALUES ('000'), ('111');
```

通过汉明距离获取最近邻：

```sql
SELECT * FROM items ORDER BY embedding <~> '101' LIMIT 5;
```

也支持雅卡尔距离 (`<%>`)。

## 二进制量化

使用表达式索引进行二进制量化（Binary Quantization）：

```sql
CREATE INDEX ON items USING hnsw ((binary_quantize(embedding)::bit(3)) bit_hamming_ops);
```

通过汉明距离获取最近邻：

```sql
SELECT * FROM items ORDER BY binary_quantize(embedding)::bit(3) <~> binary_quantize('[1,-2,3]') LIMIT 5;
```

利用原始向量重新排序以提高召回率：

```sql
SELECT * FROM (
    SELECT * FROM items ORDER BY binary_quantize(embedding)::bit(3) <~> binary_quantize('[1,-2,3]') LIMIT 20
) ORDER BY embedding <=> '[1,-2,3]' LIMIT 5;
```

## 稀疏向量

使用 `sparsevec` 类型存储稀疏向量：

```sql
CREATE TABLE items (id bigserial PRIMARY KEY, embedding sparsevec(5));
```

插入向量：

```sql
INSERT INTO items (embedding) VALUES ('{1:1,3:2,5:3}/5'), ('{1:4,3:5,5:6}/5');
```

格式为 `{索引1:值1,索引2:值2}/总维数`，索引从 1 开始（与 SQL 数组一致）。

获取 L2 距离最近邻：

```sql
SELECT * FROM items ORDER BY embedding <-> '{1:3,3:1,5:2}/5' LIMIT 5;
```

## 混合搜索

结合 Postgres [全文搜索](https://www.postgresql.org/docs/current/textsearch-intro.html) 实现混合搜索。

```sql
SELECT id, content FROM items, plainto_tsquery('hello search') query
    WHERE textsearch @@ query ORDER BY ts_rank_cd(textsearch, query) DESC LIMIT 5;
```

您可以使用 [倒数排名融合 (RRF)](https://github.com/pgvector/pgvector-python/blob/master/examples/hybrid_search/rrf.py) 或 [交叉编码器 (cross-encoder)](https://github.com/pgvector/pgvector-python/blob/master/examples/hybrid_search/cross_encoder.py) 来合并结果。

## 子向量索引

使用表达式索引对子向量建立索引：

```sql
CREATE INDEX ON items USING hnsw ((subvector(embedding, 1, 3)::vector(3)) vector_cosine_ops);
```

按余弦距离获取最近邻：

```sql
SELECT * FROM items ORDER BY subvector(embedding, 1, 3)::vector(3) <=> subvector('[1,2,3,4,5]'::vector, 1, 3) LIMIT 5;
```

利用全量向量重新排序以提高召回率：

```sql
SELECT * FROM (
    SELECT * FROM items ORDER BY subvector(embedding, 1, 3)::vector(3) <=> subvector('[1,2,3,4,5]'::vector, 1, 3) LIMIT 20
) ORDER BY embedding <=> '[1,2,3,4,5]' LIMIT 5;
```

## 性能表现

### 调优

使用 [PgTune](https://pgtune.leopard.in.ua/) 之类的工具来设置 Postgres 服务器参数的初始值。例如，`shared_buffers` 通常应设置为服务器内存的 25%。您可以通过以下命令查找配置文件路径：

```sql
SHOW config_file;
```

并查看单项设置：

```sql
SHOW shared_buffers;
```

请务必在修改配置后重启 Postgres。

### 加载数据

使用 `COPY` 进行批量数据加载（[示例](https://github.com/pgvector/pgvector-python/blob/master/examples/loading/example.py)）。

```sql
COPY items (embedding) FROM STDIN WITH (FORMAT BINARY);
```

在加载初始数据 **后** 添加索引，以获得最佳性能。

### 建立索引

参阅 [HNSW](https://www.google.com/search?q=%23%E7%B4%A2%E5%BC%95%E6%9E%84%E5%BB%BA%E6%97%B6%E9%97%B4) 和 [IVFFlat](https://www.google.com/search?q=%23%E7%B4%A2%E5%BC%95%E6%9E%84%E5%BB%BA%E6%97%B6%E9%97%B4-1) 的构建时间说明。

在生产环境中，使用 `CONCURRENTLY` 创建索引以避免阻塞写操作：

```sql
CREATE INDEX CONCURRENTLY ...
```

### 查询优化

使用 `EXPLAIN (ANALYZE, BUFFERS)` 调试性能问题。

```sql
EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM items ORDER BY embedding <-> '[3,1,2]' LIMIT 5;
```

#### 精确搜索

若要加速无索引查询，增加 `max_parallel_workers_per_gather`：

```sql
SET max_parallel_workers_per_gather = 4;
```

如果向量已归一化（模长为 1，如 [OpenAI embeddings](https://platform.openai.com/docs/guides/embeddings/which-distance-function-should-i-use)），使用内积可获得最佳性能：

```tsql
SELECT * FROM items ORDER BY embedding <#> '[3,1,2]' LIMIT 5;
```

#### 近似搜索

若要加速 IVFFlat 索引查询，可以增加倒排列表的数量（会牺牲召回率）：

```sql
CREATE INDEX ON items USING ivfflat (embedding vector_l2_ops) WITH (lists = 1000);
```

### 清理（Vacuuming）

HNSW 索引的 Vacuum 过程可能比较耗时。可以通过先重新建立索引来加速：

```sql
REINDEX INDEX CONCURRENTLY index_name;
VACUUM table_name;
```

## 监控

使用 [pg\_stat\_statements](https://www.postgresql.org/docs/current/pgstatstatements.html) 监控性能（确保将其添加到 `shared_preload_libraries` 中）。

```sql
CREATE EXTENSION pg_stat_statements;
```

获取最耗时的查询：

```sql
SELECT query, calls, ROUND((total_plan_time + total_exec_time) / calls) AS avg_time_ms,
    ROUND((total_plan_time + total_exec_time) / 60000) AS total_time_min
    FROM pg_stat_statements ORDER BY total_plan_time + total_exec_time DESC LIMIT 20;
```

通过对比近似搜索和精确搜索的结果来监控召回率：

```sql
BEGIN;
SET LOCAL enable_indexscan = off; -- 强制执行精确搜索
SELECT ...
COMMIT;
```

## 扩展性

扩展 pgvector 的方式与扩展 Postgres 相同。

垂直扩展：增加单个实例的内存、CPU 和存储。使用现有工具进行 [参数调优](https://www.google.com/search?q=%23%E8%B0%83%E4%BC%98) 和 [监控](https://www.google.com/search?q=%23%E7%9B%91%E6%8E%A7)。

水平扩展：使用 [副本 (replicas)](https://www.postgresql.org/docs/current/hot-standby.html)，或者使用 [Citus](https://github.com/citusdata/citus) 或其他分片方案（[示例](https://github.com/pgvector/pgvector-python/blob/master/examples/citus/example.py)）。

## 语言支持

任何带有 Postgres 客户端的语言都可以使用 pgvector。您甚至可以在一种语言中生成并存储向量，然后在另一种语言中查询。

| 语言                        | 库 / 示例                                                    |
| --------------------------- | ------------------------------------------------------------ |
| Ada                         | [pgvector-ada](https://github.com/pgvector/pgvector-ada)     |
| Algol                       | [pgvector-algol](https://github.com/pgvector/pgvector-algol) |
| C                           | [pgvector-c](https://github.com/pgvector/pgvector-c)         |
| C++                         | [pgvector-cpp](https://github.com/pgvector/pgvector-cpp)     |
| C#, F#, Visual Basic        | [pgvector-dotnet](https://github.com/pgvector/pgvector-dotnet) |
| COBOL                       | [pgvector-cobol](https://github.com/pgvector/pgvector-cobol) |
| Crystal                     | [pgvector-crystal](https://github.com/pgvector/pgvector-crystal) |
| D                           | [pgvector-d](https://github.com/pgvector/pgvector-d)         |
| Dart                        | [pgvector-dart](https://github.com/pgvector/pgvector-dart)   |
| Elixir                      | [pgvector-elixir](https://github.com/pgvector/pgvector-elixir) |
| Erlang                      | [pgvector-erlang](https://github.com/pgvector/pgvector-erlang) |
| Fortran                     | [pgvector-fortran](https://github.com/pgvector/pgvector-fortran) |
| Gleam                       | [pgvector-gleam](https://github.com/pgvector/pgvector-gleam) |
| Go                          | [pgvector-go](https://github.com/pgvector/pgvector-go)       |
| Haskell                     | [pgvector-haskell](https://github.com/pgvector/pgvector-haskell) |
| Java, Kotlin, Groovy, Scala | [pgvector-java](https://github.com/pgvector/pgvector-java)   |
| JavaScript, TypeScript      | [pgvector-node](https://github.com/pgvector/pgvector-node)   |
| Julia                       | [Pgvector.jl](https://github.com/pgvector/Pgvector.jl)       |
| Lisp                        | [pgvector-lisp](https://github.com/pgvector/pgvector-lisp)   |
| Lua                         | [pgvector-lua](https://github.com/pgvector/pgvector-lua)     |
| Nim                         | [pgvector-nim](https://github.com/pgvector/pgvector-nim)     |
| OCaml                       | [pgvector-ocaml](https://github.com/pgvector/pgvector-ocaml) |
| Pascal                      | [pgvector-pascal](https://github.com/pgvector/pgvector-pascal) |
| Perl                        | [pgvector-perl](https://github.com/pgvector/pgvector-perl)   |
| PHP                         | [pgvector-php](https://github.com/pgvector/pgvector-php)     |
| Prolog                      | [pgvector-prolog](https://github.com/pgvector/pgvector-prolog) |
| Python                      | [pgvector-python](https://github.com/pgvector/pgvector-python) |
| R                           | [pgvector-r](https://github.com/pgvector/pgvector-r)         |
| Racket                      | [pgvector-racket](https://github.com/pgvector/pgvector-racket) |
| Raku                        | [pgvector-raku](https://github.com/pgvector/pgvector-raku)   |
| Ruby                        | [pgvector-ruby](https://github.com/pgvector/pgvector-ruby), [Neighbor](https://github.com/ankane/neighbor) |
| Rust                        | [pgvector-rust](https://github.com/pgvector/pgvector-rust)   |
| Swift                       | [pgvector-swift](https://github.com/pgvector/pgvector-swift) |
| Tcl                         | [pgvector-tcl](https://github.com/pgvector/pgvector-tcl)     |
| Zig                         | [pgvector-zig](https://github.com/pgvector/pgvector-zig)     |

## 常见问题

#### 单张表中可以存储多少向量？

默认情况下，Postgres 的非分区表限制为 32 TB。分区表可以拥有数千个相同容量的分区。

#### 是否支持副本（Replication）？

支持。pgvector 使用写前日志（WAL），支持复制和点进点恢复。

#### 如果我想对超过 2,000 维的向量建立索引怎么办？

您可以使用 [半精度向量](https://www.google.com/search?q=%23%E5%8D%8A%E7%B2%BE%E5%BA%A6%E5%90%91%E9%87%8F) 或 [半精度索引](https://www.google.com/search?q=%23%E5%8D%8A%E7%B2%BE%E5%BA%A6%E7%B4%A2%E5%BC%95) 来索引最多 4,000 维，或使用 [二进制量化](https://www.google.com/search?q=%23%E4%BA%8C%E8%BF%9B%E5%88%B6%E9%87%8F%E5%8C%96) 索引最多 64,000 维。其他选择包括 [子向量索引](https://www.google.com/search?q=%23%E5%AD%90%E5%90%91%E9%87%8F%E7%B4%A2%E5%BC%95)（适用于支持它的模型）或 [降维](https://en.wikipedia.org/wiki/Dimensionality_reduction)。

#### 能在同一列存储不同维度的向量吗？

可以使用 `vector` 类型（不带 `(n)`）。

```sql
CREATE TABLE embeddings (model_id bigint, item_id bigint, embedding vector, PRIMARY KEY (model_id, item_id));
```

但是，您只能在具有相同维度的行上创建索引（利用 [表达式](https://www.postgresql.org/docs/current/indexes-expressional.html) 和 [部分](https://www.postgresql.org/docs/current/indexes-partial.html) 索引）：

```sql
CREATE INDEX ON embeddings USING hnsw ((embedding::vector(3)) vector_l2_ops) WHERE (model_id = 123);
```

#### 能存储更高精度的向量吗？

可以使用 `double precision[]` 或 `numeric[]` 类型。

```sql
CREATE TABLE items (id bigserial PRIMARY KEY, embedding double precision[]);
INSERT INTO items (embedding) VALUES ('{1,2,3}'), ('{4,5,6}');
```

您可以添加 [CHECK 约束](https://www.postgresql.org/docs/current/ddl-constraints.html) 确保其可以转换为 `vector` 类型并符合预期维度。

#### 索引必须装进内存吗？

不强制要求，但与所有索引一样，如果能装进内存，性能会更好。可以使用以下命令查看索引大小：

```sql
SELECT pg_size_pretty(pg_relation_size('index_name'));
```

## 故障排除

#### 为什么查询没有使用索引？

查询必须包含 `ORDER BY` 和 `LIMIT`，且 `ORDER BY` 必须是距离操作符的结果（而非表达式），并且是升序排列。

```sql
-- 使用索引
ORDER BY embedding <=> '[3,1,2]' LIMIT 5;

-- 不使用索引
ORDER BY 1 - (embedding <=> '[3,1,2]') DESC LIMIT 5;
```

#### 为什么添加 HNSW 索引后结果变少了？

结果数量受动态候选列表大小 (`hnsw.ef_search`) 限制，默认 40。死元组（dead tuples）或查询中的过滤条件可能导致结果更少。启用 [迭代索引扫描](https://www.google.com/search?q=%23%E8%BF%AD%E4%BB%A3%E7%B4%A2%E5%BC%95%E6%89%AB%E6%8F%8F) 可以解决此问题。另外请注意，`NULL` 向量和零向量（针对余弦距离）不会被索引。

#### 为什么添加 IVFFlat 索引后结果变少了？

通常是因为创建索引时表中的数据过少，导致列表分配不均。建议删除索引，等数据量增加后再创建。此外，增加探查次数 (`ivfflat.probes`) 或启用迭代扫描也有帮助。

## 参考

  - [Vector 类型](https://www.google.com/search?q=%23vector-%E7%B1%BB%E5%9E%8B)
  - [Halfvec 类型](https://www.google.com/search?q=%23halfvec-%E7%B1%BB%E5%9E%8B)
  - [Bit 类型](https://www.google.com/search?q=%23bit-%E7%B1%BB%E5%9E%8B)
  - [Sparsevec 类型](https://www.google.com/search?q=%23sparsevec-%E7%B1%BB%E5%9E%8B)

### Vector 类型

每个向量占用 `4 * 维度 + 8` 字节。每个元素是单精度浮点数（类似 Postgres 的 `real` 类型），元素必须是有限的（不允许 `NaN`、`Infinity`）。向量最高支持 16,000 维。

### 向量运算符

| 运算符 | 描述       | 新增版本 |
| ------ | ---------- | -------- |
| +      | 逐元素加法 |          |
| -      | 逐元素减法 |          |
| *      | 逐元素乘法 | 0.5.0    |
| \|\|   | 向量拼接   | 0.7.0    |
| <->    | 欧氏距离   |          |
| <#>    | 负内积     |          |
| <=>    | 余弦距离   |          |
| <+>    | 出租车距离 | 0.7.0    |

### 向量函数

| 函数                                               | 描述           | 新增版本 |
| -------------------------------------------------- | -------------- | -------- |
| binary_quantize(vector) → bit                      | 二值量化       | 0.7.0    |
| cosine_distance(vector, vector) → double precision | 余弦距离       |          |
| inner_product(vector, vector) → double precision   | 内积           |          |
| l1_distance(vector, vector) → double precision     | 出租车距离     | 0.5.0    |
| l2_distance(vector, vector) → double precision     | 欧氏距离       |          |
| l2_normalize(vector) → vector                      | 欧氏范数归一化 | 0.7.0    |
| subvector(vector, integer, integer) → vector       | 子向量         | 0.7.0    |
| vector_dims(vector) → integer                      | 维度数量       |          |
| vector_norm(vector) → double precision             | 欧氏范数       |          |

### 向量聚合函数

| 函数                 | 描述   | 新增版本 |
| -------------------- | ------ | -------- |
| avg(vector) → vector | 平均值 |          |
| sum(vector) → vector | 求和   | 0.5.0    |

### Halfvec 类型

每个半精度向量占用 `2 * 维度数 + 8` 字节存储空间。每个元素为半精度浮点数，所有元素必须为有限值（不允许 `NaN`、`Infinity` 或 `-Infinity`）。半精度向量最多支持 16,000 维。

### Halfvec 运算符

| 运算符 | 描述       | 新增版本 |
| ------ | ---------- | -------- |
| +      | 逐元素加法 | 0.7.0    |
| -      | 逐元素减法 | 0.7.0    |
| *      | 逐元素乘法 | 0.7.0    |
| \|\|   | 向量拼接   | 0.7.0    |
| <->    | 欧氏距离   | 0.7.0    |
| <#>    | 负内积     | 0.7.0    |
| <=>    | 余弦距离   | 0.7.0    |
| <+>    | 出租车距离 | 0.7.0    |

### Halfvec 函数

| 函数                                                 | 描述           | 新增版本 |
| ---------------------------------------------------- | -------------- | -------- |
| binary_quantize(halfvec) → bit                       | 二值量化       | 0.7.0    |
| cosine_distance(halfvec, halfvec) → double precision | 余弦距离       | 0.7.0    |
| inner_product(halfvec, halfvec) → double precision   | 内积           | 0.7.0    |
| l1_distance(halfvec, halfvec) → double precision     | 出租车距离     | 0.7.0    |
| l2_distance(halfvec, halfvec) → double precision     | 欧氏距离       | 0.7.0    |
| l2_norm(halfvec) → double precision                  | 欧氏范数       | 0.7.0    |
| l2_normalize(halfvec) → halfvec                      | 欧氏范数归一化 | 0.7.0    |
| subvector(halfvec, integer, integer) → halfvec       | 子向量         | 0.7.0    |
| vector_dims(halfvec) → integer                       | 维度数量       | 0.7.0    |

### Halfvec 聚合函数

| 函数                   | 描述   | 新增版本 |
| ---------------------- | ------ | -------- |
| avg(halfvec) → halfvec | 平均值 | 0.7.0    |
| sum(halfvec) → halfvec | 求和   | 0.7.0    |

### Bit 类型

每个位向量占用 `维度数 / 8 + 8` 字节存储空间。详情参见 [Postgres 文档](https://www.postgresql.org/docs/current/datatype-bit.html)。

### Bit 运算符

| 运算符 | 描述         | 新增版本 |
| ------ | ------------ | -------- |
| <~>    | 汉明距离     | 0.7.0    |
| <%>    | Jaccard 距离 | 0.7.0    |

### Bit 函数

| 函数                                          | 描述         | 新增版本 |
| --------------------------------------------- | ------------ | -------- |
| hamming_distance(bit, bit) → double precision | 汉明距离     | 0.7.0    |
| jaccard_distance(bit, bit) → double precision | Jaccard 距离 | 0.7.0    |

### Sparsevec 类型

每个稀疏向量占用 `8 * 非零元素数 + 16` 字节存储空间。每个元素为单精度浮点数，所有元素必须为有限值（不允许 `NaN`、`Infinity` 或 `-Infinity`）。稀疏向量最多支持 16,000 个非零元素。

### Sparsevec 运算符

| 运算符 | 描述       | 新增版本 |
| ------ | ---------- | -------- |
| <->    | 欧氏距离   | 0.7.0    |
| <#>    | 负内积     | 0.7.0    |
| <=>    | 余弦距离   | 0.7.0    |
| <+>    | 出租车距离 | 0.7.0    |

### Sparsevec 函数

| 函数                                                     | 描述           | 新增版本 |
| -------------------------------------------------------- | -------------- | -------- |
| cosine_distance(sparsevec, sparsevec) → double precision | 余弦距离       | 0.7.0    |
| inner_product(sparsevec, sparsevec) → double precision   | 内积           | 0.7.0    |
| l1_distance(sparsevec, sparsevec) → double precision     | 出租车距离     | 0.7.0    |
| l2_distance(sparsevec, sparsevec) → double precision     | 欧氏距离       | 0.7.0    |
| l2_norm(sparsevec) → double precision                    | 欧氏范数       | 0.7.0    |
| l2_normalize(sparsevec) → sparsevec                      | 欧氏范数归一化 | 0.7.0    |

## 安装说明 - Linux 和 Mac

### Postgres 路径

如果机器上有多个 Postgres 版本，需指定 `pg_config` 路径：

```sh
export PG_CONFIG=/Library/PostgreSQL/18/bin/pg_config
```

### 缺少头文件

如果报错 `fatal error: postgres.h: No such file or directory`，请确保已安装 Postgres 开发库（如 Ubuntu 上的 `postgresql-server-dev-18`）。

## 托管服务

pgvector 已在以下主流托管商提供服务：[列表详情](https://github.com/pgvector/pgvector/issues/54)。

## 升级

[安装](https://www.google.com/search?q=%23%E5%AE%89%E8%A3%85) 最新版本后，在每个数据库执行：

```sql
ALTER EXTENSION vector UPDATE;
```

# 知识扩展

## 1.精确（近似）最近邻搜索/向量类型/距离度量/含PgSQL客户端语言

**1. 精确和近似最近邻搜索（Exact & Approximate Nearest Neighbor Search）**

向量数据库的核心任务是：

> 给定一个向量，找到最相似的向量（最近邻）。

pgvector 支持两种方式：

- **精确搜索（Exact Search）**
  - 逐个计算距离
  - 结果绝对准确
  - 但在向量数量巨大时速度较慢
- **近似搜索（Approximate Nearest Neighbor, ANN）**
  - 使用索引（如 HNSW）加速
  - 结果非常接近真实最近邻
  - 性能极高，适合百万级、亿级向量

这让 pgvector 既能用于小规模高精度任务，也能用于大规模高性能检索。

------

**2. 多种向量类型：单精度、半精度、二进制、稀疏向量**

pgvector 不只支持普通的 float 向量，还支持多种格式：

| 类型                           | 说明                 | 使用场景                         |
| ------------------------------ | -------------------- | -------------------------------- |
| **单精度（float4）**           | 常见的 32-bit 浮点数 | OpenAI、Cohere 等 embedding      |
| **半精度（float2）**           | 16-bit 浮点数        | 节省存储、加速计算               |
| **二进制向量（bit vectors）**  | 0/1 组成             | Hamming 距离、局部敏感哈希       |
| **稀疏向量（sparse vectors）** | 只存非零项           | 词袋模型、TF-IDF、稀疏 embedding |

这让 pgvector 能适配不同模型、不同存储需求。

------

**3. 多种距离度量（L2、内积、余弦、L1、Hamming、Jaccard）**

向量相似度不是只有“余弦相似度”，pgvector 支持多种距离：

| 距离                            | 说明           | 常见用途                         |
| ------------------------------- | -------------- | -------------------------------- |
| **L2 距离（欧氏距离）**         | 直线距离       | 图像 embedding                   |
| **内积（Inner Product）**       | 越大越相似     | 推荐系统、向量归一化后等价于余弦 |
| **余弦距离（Cosine Distance）** | 方向相似度     | 文本 embedding                   |
| **L1 距离（曼哈顿距离）**       | 绝对差之和     | 稀疏向量                         |
| **Hamming 距离**                | bit 不同的数量 | 二进制向量、LSH                  |
| **Jaccard 距离**                | 集合相似度     | 标签集合、稀疏特征               |

这让你可以根据任务选择最合适的相似度方式。

------

**4. 支持任何有 PostgreSQL 客户端的语言**

因为 pgvector 是 PostgreSQL 扩展，所以：

- 你用什么语言连接 PostgreSQL
- 就能用什么语言做向量搜索

包括：

- Python（psycopg2、asyncpg）
- Java（JDBC）
- Go（pgx）
- Node.js（pg）
- Rust（tokio-postgres）
- C/C++
- Ruby、PHP、C# 等等

换句话说：

> **pgvector = PostgreSQL + 向量搜索能力**  
>  你不需要额外学习新的数据库客户端。

## 2.ACID/PITR

**ACID**

ACID 是数据库事务的四大特性：

| 字母  | 全称                  | 含义                       |
| ----- | --------------------- | -------------------------- |
| **A** | Atomicity（原子性）   | 要么全部成功，要么全部失败 |
| **C** | Consistency（一致性） | 数据始终保持合法状态       |
| **I** | Isolation（隔离性）   | 并发事务互不干扰           |
| **D** | Durability（持久性）  | 提交后的数据不会丢失       |

pgvector 继承 PostgreSQL 的 ACID 特性，因此：

- 插入向量是事务性的
- 更新/删除向量不会破坏一致性
- 并发向量写入安全
- 崩溃后数据仍然可靠

这点是很多“纯向量数据库”做不到的。

------

**PITR（Point-In-Time Recovery）**

PITR = **时间点恢复**。

它允许你把数据库恢复到过去某一秒的状态，例如：

- 恢复到误删数据之前
- 恢复到某次错误写入之前
- 恢复到某次系统故障之前

PostgreSQL 通过 WAL 日志（Write-Ahead Logging）实现：

> 你可以把数据库“倒回到过去”。

pgvector 作为扩展，也自动享受这一能力。

------

**总结**

pgvector 的四大支持点让它成为一个**真正可用于生产的向量数据库**：

- 支持精确/近似搜索
- 支持多种向量格式
- 支持多种相似度距离
- 支持所有 PostgreSQL 客户端语言

并且继承 PostgreSQL 的：

- **ACID 事务安全**
- **PITR 时间点恢复**
- **JOIN、索引、权限、备份等完整数据库能力**

这就是为什么很多公司选择 pgvector，而不是独立的向量数据库。

## 3.HNSW/IVFFlat原理及特性

| 维度         | **HNSW**                  | **IVFFlat**                  |
| ------------ | ------------------------- | ---------------------------- |
| 核心思想     | 多层小世界图 + 贪心导航   | KMeans 聚类 + 倒排列表       |
| 是否需要训练 | ❌ 不需要                  | ✅ 需要（KMeans）             |
| 查询方式     | 从高层向下贪心 + 局部扩展 | 先找最近簇，再簇内暴力       |
| 构建成本     | 较高（维护图结构）        | 较低（一次聚类 + 分桶）      |
| 内存占用     | 高（图 + 多层邻接）       | 低（质心 + 列表）            |
| 动态更新     | 非常友好                  | 不友好（需重建）             |
| 召回率       | 高（95–99%）              | 中–高（依赖 nprobe）         |
| 适用场景     | 高精度、低延迟、在线写入  | 亿级规模、批量导入、可调召回 |

HNSW 的核心是构建一个 **多层图结构**：

- 顶层稀疏 → 长距离跳跃
- 底层密集 → 精细搜索
- 查询时从顶层入口点开始，逐层向下贪心逼近目标区域
- 底层使用 **候选池（ef）** 做更广搜索以提高召回

**HNSW 特性**

- **高召回、高精度**
- **低延迟（log 级别）**
- **适合在线插入**
- 内存占用较高（图结构）

------

IVFFlat 的核心思想是：

> **先粗分桶，再桶内精确搜索。**

**构建阶段**

1. 使用 **KMeans** 将向量分成 `nlist` 个簇
2. 每个向量被分配到最近的质心
3. 每个簇形成一个倒排列表（inverted list）

**查询阶段**

1. 计算查询向量与所有质心的距离
2. 选出最近的 `nprobe` 个簇
3. 在这些簇内做暴力搜索
4. 返回 Top-K

------

**HNSW 适用：**

- 实时推荐系统
- 在线向量写入（如用户行为 embedding）
- 高精度语义搜索
- 延迟敏感（<10ms）场景

**IVFFlat 适用：**

- 亿级向量库
- 离线批量构建
- 对召回率要求可调
- 内存有限的场景

## 4.