# hot_cold 测试文件说明（中文）

本文档说明 `test/hot_cold` 目录下各测试文件的功能和使用方法，帮助你快速完成“基于 Buffer Pool 的 HNSW 冷热分层”测试。

## 1. 文件清单与功能

- `001_hot_cold_smoke.pl`
  - 冒烟测试。
  - 检查热冷分层相关 GUC 是否存在。
  - 创建测试数据和 HNSW 索引，确认查询走 `Index Scan`。
  - 验证 Top-K 查询可返回预期行数。

- `002_hot_cold_recall.pl`
  - 召回测试。
  - 对比 `hnsw.hot_cold_enabled = off/on` 两种模式。
  - 基于精确结果计算 recall，检查是否满足 `Recall@10` 目标。

- `003_hot_cold_latency_probe.sql`
  - 手工延迟探针。
  - 用 `EXPLAIN (ANALYZE, BUFFERS)` 对比 OFF/ON 两种模式的执行时间和 Buffer 使用情况。

- `004_hot_cold_buffers_probe.sql`
  - 手工 Buffer 探针。
  - 连续执行多条相近查询，观察命中/读取变化，辅助判断冷热分层对缓存行为的影响。

- `run_hot_cold_tests.ps1`
  - Windows PowerShell 一键运行脚本。
  - 顺序执行 `001` 和 `002` 两个 Perl 自动化测试。

## 2. 运行前准备

- 已安装并可连接 PostgreSQL。
- 可使用 `CREATE EXTENSION vector;`。
- 若要运行 `.pl` 自动化测试，需要可用的 Perl 环境（脚本依赖 PostgreSQL 测试模块）。
- 若当前代码尚未合入热冷分层 GUC，自动化脚本会按设计跳过（skip）。

## 3. 自动化测试用法

在仓库根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File test/hot_cold/run_hot_cold_tests.ps1
```

或分别执行：

```powershell
perl test/hot_cold/001_hot_cold_smoke.pl
perl test/hot_cold/002_hot_cold_recall.pl
```

## 4. 手工 SQL 探针用法

先准备测试表和索引（如 `tst` + HNSW 索引），再执行：

```powershell
psql -d postgres -f test/hot_cold/003_hot_cold_latency_probe.sql
psql -d postgres -f test/hot_cold/004_hot_cold_buffers_probe.sql
```

建议重点观察：

- 是否使用 HNSW 索引扫描
- OFF/ON 模式下 `Execution Time` 变化
- OFF/ON 模式下 `Buffers: shared hit/read` 变化

## 5. 常见问题

- 报错 `perl` 未找到
  - 说明本机未安装 Perl 或未加入 PATH。

- 脚本显示 skip
  - 通常是热冷分层 GUC 尚未注册（例如 `hnsw.hot_cold_enabled` 不存在）。

- SQL 探针报表不存在
  - 先按探针文件注释创建 `tst` 表与 HNSW 索引。

- 结果波动较大
  - 建议固定 `hnsw.ef_search`、执行多轮取平均，并在相同数据规模下对比。

## 6. 推荐测试顺序

1. 先跑 `001_hot_cold_smoke.pl`，确认基础功能和开关路径正常。
2. 再跑 `002_hot_cold_recall.pl`，确认准确率不明显下降。
3. 最后跑 `003/004` 手工探针，观察时延与 Buffer 行为。

