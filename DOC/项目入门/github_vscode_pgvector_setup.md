# GitHub 版本管理与 VS Code + PostgreSQL + 自研 pgvector 环境配置指南

本文档说明两件事：

1. 如何用 GitHub 对当前项目做版本管理
2. 如何在本机配置 VS Code + PostgreSQL + 自己编写的 pgvector 扩展开发环境

当前项目是一个 PostgreSQL 扩展项目，扩展名是 `vector`。它不是 VS Code 插件，也不是 PostgreSQL 客户端插件，而是需要编译成 PostgreSQL 能加载的动态库，然后通过 `CREATE EXTENSION vector;` 在数据库中启用。

---

## 1. 推荐项目定位

建议将本项目包装为：

> 基于 pgvector 的 PostgreSQL HNSW 向量索引源码分析与 Buffer Pool 冷热分层优化项目

GitHub 仓库名称可以使用：

```text
pgvector-hnsw-hotcold
```

或：

```text
pgvector-optimization-lab
```

如果项目基于开源 pgvector 修改，请保留原项目 `LICENSE`，并在 README 中说明：

```text
This project is based on pgvector and focuses on HNSW index analysis and optimization experiments.
```

中文说明可以写：

```text
本项目基于 pgvector 源码进行学习、分析与优化实验，重点方向为 HNSW 索引查询路径和 Buffer Pool 冷热分层优化。
```

---

## 2. GitHub 版本管理方法

### 2.1 安装 Git√

先确认本机是否已经安装 Git：

```powershell
git --version
```

如果没有安装，到 Git 官网下载安装：

```text
https://git-scm.com/download/win
```

安装时推荐保持默认选项即可。

安装完成后配置用户名和邮箱：

```powershell
git config --global user.name "你的 GitHub 用户名"
git config --global user.email "你的 GitHub 邮箱"
```

查看配置：

```powershell
git config --global --list
```

### 2.2 在当前项目中初始化 Git√

进入项目目录：

```powershell
cd F:/_WORK/PgVector
```

初始化仓库：

```powershell
git init
```

查看当前文件状态：

```powershell
git status
```

当前项目已经有 `.gitignore`，建议继续保留，并确认不会提交这些内容：

```text
*.obj
*.dll
*.exe
*.lib
*.pdb
*.ilk
*.exp
*.so
*.dylib
Debug/
Release/
tmp_check/
```

如果 `.gitignore` 中没有这些规则，可以后续补充。

### 2.3 第一次提交

添加所有文件：

```powershell
git add .
```

提交：

```powershell
git commit -m "Initial pgvector optimization project"
```

建议第一条提交不要写成 `first commit`，而是说明项目是什么。

### 2.4 在 GitHub 创建远程仓库

打开 GitHub，新建仓库：

```text
https://github.com/new
```

推荐设置：

```text
Repository name: pgvector-hnsw-hotcold
Visibility: Public 或 Private
不要勾选 Add a README file
不要勾选 Add .gitignore
不要勾选 Choose a license
```

因为本地项目已经有这些文件。

创建成功后，GitHub 会给出远程地址，例如：

```text
https://github.com/你的用户名/pgvector-hnsw-hotcold.git
https://github.com/sin-of-lazy/pgvector-hnsw-hotcold.git
```

绑定远程仓库：

```powershell
git remote add origin https://github.com/你的用户名/pgvector-hnsw-hotcold.git
git remote add origin https://github.com/sin-of-lazy/pgvector-hnsw-hotcold.git
```

确认远程仓库：

```powershell
git remote -v
```

推送到 GitHub：

```powershell
git branch -M main
git push -u origin main
```

### 2.5 如果要保留上游 pgvector 地址

如果希望以后对比原版 pgvector，可以添加 upstream：

```powershell
git remote add upstream https://github.com/pgvector/pgvector.git
```

查看远程：

```powershell
git remote -v
```

以后可以获取上游更新：

```powershell
git fetch upstream
```

注意：不要随意把 upstream 的新代码直接合并进来。当前项目已经有大量中文注释和实验文档，合并前最好先建立分支。

### 2.6 推荐分支策略

建议使用简单、清晰的分支模型：

```text
main
  稳定分支，保证能编译，README 和文档完整

dev
  日常开发分支

feature/hot-cold-guc
  实现热冷分层 GUC 参数

feature/hot-cold-search-path
  实现 HNSW 查询路径冷热分层

feature/benchmark-report
  补充 benchmark 和实验结果文档
```

创建开发分支：

```powershell
git checkout -b dev
```

创建功能分支：

```powershell
git checkout -b feature/hot-cold-guc
```

开发完成后提交：

```powershell
git add .
git commit -m "Add HNSW hot/cold GUC definitions"
```

推送分支：

```powershell
git push -u origin feature/hot-cold-guc
```

然后在 GitHub 上创建 Pull Request，将功能分支合并到 `dev` 或 `main`。

### 2.7 推荐提交规范

提交信息建议写清楚“做了什么”，不要只写 `update`。

推荐格式：

```text
docs: add Windows setup guide
test: add hot/cold recall test
hnsw: add hot/cold GUC switches
hnsw: prototype hot layer cache lookup
bench: add latency probe results
```

示例：

```powershell
git commit -m "docs: add GitHub and VS Code setup guide"
```

### 2.8 日常开发流程

每次开始开发前：

```powershell
git status
git pull
```

新建分支：

```powershell
git checkout -b feature/xxx
```

修改代码后查看变化：

```powershell
git diff
git status
```

提交：

```powershell
git add .
git commit -m "清晰的提交说明"
```

推送：

```powershell
git push -u origin feature/xxx
```

### 2.9 推荐打标签

当项目到达一个可展示阶段，可以打 tag：

```powershell
git tag -a v0.1.0 -m "HNSW source reading and test scaffold"
git push origin v0.1.0
```

后续实现第一版热冷分层后：

```powershell
git tag -a v0.2.0 -m "Prototype HNSW hot/cold optimization"
git push origin v0.2.0
```

这样简历或面试时可以清楚说明项目演进。

---

## 3. VS Code 开发环境配置

### 3.1 安装 VS Code√

下载地址：

```text
https://code.visualstudio.com/
```

安装后，用 VS Code 打开项目目录：

```text
F:\_WORK\PgVector
```

### 3.2 推荐 VS Code 插件

建议安装：

```text
C/C++                     Microsoft 官方 C/C++ 支持
Git Graph                 可视化查看 Git 分支和提交
GitLens                   查看代码提交历史
Markdown All in One       写项目文档更方便
SQLTools                  可选，用于连接 PostgreSQL
SQLTools PostgreSQL Driver 可选，SQLTools 的 PostgreSQL 驱动
```

如果只想先编译项目，必须装的是 C/C++ 插件；SQLTools 不是必需。

### 3.3 推荐 VS Code 终端

普通 PowerShell 可以用来做 Git、查看文件、写文档。

但编译 Windows C 扩展时，推荐使用：

```text
x64 Native Tools Command Prompt for VS
```

原因是 `nmake`、`cl.exe` 等 Visual Studio 编译工具需要正确的环境变量。

在 VS Code 中也可以打开这个环境，但第一次建议先用开始菜单里的 `x64 Native Tools Command Prompt for VS 2022` 跑通编译。

---

## 4. 安装 PostgreSQL√

### 4.1 推荐安装方式

Windows 下推荐使用 EnterpriseDB 安装包：

```text
https://www.postgresql.org/download/windows/
```

安装时记录安装目录，例如：

```text
C:\Program Files\PostgreSQL\17
F:\postgresql
```

或：

```text
C:\Program Files\PostgreSQL\18
```

下文用 `%PGROOT%` 表示你的 PostgreSQL 安装目录。

### 4.2 确认 PostgreSQL 工具可用

打开 PowerShell：

```powershell
& "C:\Program Files\PostgreSQL\17\bin\psql.exe" --version
& "C:\Program Files\PostgreSQL\17\bin\pg_config.exe" --version
```

如果你的版本是 PostgreSQL 18，就把路径改成：

```powershell
& "C:\Program Files\PostgreSQL\18\bin\psql.exe" --version
& "C:\Program Files\PostgreSQL\18\bin\pg_config.exe" --version

& "F:\postgresql\bin\psql.exe" --version
& "F:\postgresql\bin\pg_config.exe" --version
```

`pg_config.exe` 很重要，它会告诉扩展编译系统 PostgreSQL 的头文件和库文件在哪里。

### 4.3 添加 PostgreSQL 到 PATH

可以把 PostgreSQL 的 `bin` 目录加入系统环境变量 PATH：

```text
C:\Program Files\PostgreSQL\17\bin
```

加入后重新打开终端，确认：

```powershell
psql --version
pg_config --version
```

如果不想改系统 PATH，也可以在命令中使用完整路径。

---

## 5. 安装 C/C++ 编译工具√

### 5.1 安装 Visual Studio Build Tools

下载：

```text
https://visualstudio.microsoft.com/visual-cpp-build-tools/
```

安装时勾选：

```text
Desktop development with C++
Windows 10/11 SDK
MSVC v143 或更新版本
C++ CMake tools for Windows 可选
```

安装完成后，在开始菜单搜索：

```text
x64 Native Tools Command Prompt for VS 2022
```

打开后确认：

```cmd
cl
nmake
```

能看到版本信息说明编译工具可用。

---

## 6. 编译并安装我们自己的 pgvector

### 6.1 重要说明

这个项目安装到 PostgreSQL 后，扩展名仍然是：

```sql
vector
```

也就是说，它会作为 PostgreSQL 的 `vector` 扩展被加载。

如果你之前安装过官方 pgvector，那么我们本地编译安装的版本可能会覆盖同名文件：

```text
vector.dll
vector.control
vector--*.sql
```

这是正常的，因为我们的目标就是让 PostgreSQL 加载“我们自己编写/修改过的 pgvector”。

### 6.2 打开正确的编译终端

从开始菜单打开：

```text
x64 Native Tools Command Prompt for VS 2022
```

进入项目目录：

```cmd
cd /d F:\_WORK\PgVector
```

设置 PostgreSQL 安装目录。

如果你安装的是 PostgreSQL 17：

```cmd
set "PGROOT=C:\Program Files\PostgreSQL\17"
```

如果你安装的是 PostgreSQL 18：

```cmd
set "PGROOT=C:\Program Files\PostgreSQL\18"
```

确认目录存在：

```cmd
dir "%PGROOT%"
dir "%PGROOT%\bin\pg_config.exe"
```

### 6.3 编译

执行：

```cmd
nmake /F Makefile.win
```

成功后通常会生成或更新编译产物，例如：

```text
vector.dll
*.obj
```

如果报错找不到 PostgreSQL 头文件或库文件，优先检查：

```cmd
echo %PGROOT%
dir "%PGROOT%\include"
dir "%PGROOT%\lib"
```

### 6.4 安装到 PostgreSQL

执行：

```cmd
nmake /F Makefile.win install
```

它会把扩展文件复制到 PostgreSQL 安装目录下，例如：

```text
%PGROOT%\lib\vector.dll
%PGROOT%\share\extension\vector.control
%PGROOT%\share\extension\vector--0.8.2.sql
```

如果 Windows 提示 `vector.dll` 正在被占用，通常是 PostgreSQL 服务已经加载了旧 DLL。可以先停止 PostgreSQL 服务，再安装。

用管理员 PowerShell 停止服务：

```powershell
Get-Service postgresql*
Stop-Service postgresql-x64-17
```

服务名以实际显示为准。安装完成后再启动：

```powershell
Start-Service postgresql-x64-17
```

也可以在 Windows 的“服务”管理器里手动停止和启动 PostgreSQL。

---

## 7. 在数据库中启用并验证扩展

### 7.1 进入 psql

使用 PostgreSQL 自带的 psql：

```powershell
psql -U postgres
```

如果没有配置 PATH，可以使用完整路径：

```powershell
& "C:\Program Files\PostgreSQL\17\bin\psql.exe" -U postgres
```

### 7.2 创建测试数据库

```sql
CREATE DATABASE pgvector_dev;
```

连接数据库：

```sql
\c pgvector_dev
```

启用扩展：

```sql
CREATE EXTENSION vector;
```

查看扩展：

```sql
\dx vector
```

或：

```sql
SELECT extname, extversion
FROM pg_extension
WHERE extname = 'vector';
```

### 7.3 最小功能验证

```sql
CREATE TABLE items (
    id bigserial PRIMARY KEY,
    embedding vector(3)
);

INSERT INTO items (embedding)
VALUES ('[1,2,3]'), ('[4,5,6]'), ('[1,1,1]');

SELECT id, embedding
FROM items
ORDER BY embedding <-> '[1,2,2]'
LIMIT 2;
```

如果能返回结果，说明扩展已经可用。

### 7.4 验证 HNSW 索引

```sql
CREATE INDEX items_embedding_hnsw_idx
ON items
USING hnsw (embedding vector_l2_ops);

SET enable_seqscan = off;

EXPLAIN ANALYZE
SELECT id
FROM items
ORDER BY embedding <-> '[1,2,2]'
LIMIT 2;
```

看到 `Index Scan using items_embedding_hnsw_idx`，说明 HNSW 索引路径可用。

---

## 8. 开发修改后的重新编译流程

每次修改 C 代码后，例如修改：

```text
src/hnsw.c
src/hnswscan.c
src/hnswutils.c
src/hnsw.h
```

推荐流程：

1. 停止 PostgreSQL 服务，避免 DLL 被占用
2. 重新编译
3. 重新安装
4. 启动 PostgreSQL 服务
5. 重新连接数据库测试

命令示例：

```cmd
cd /d F:\_WORK\PgVector
set "PGROOT=C:\Program Files\PostgreSQL\17"
nmake /F Makefile.win clean
nmake /F Makefile.win
nmake /F Makefile.win install
```

如果只改了 SQL 文件或 control 文件，也需要重新安装：

```cmd
nmake /F Makefile.win install
```

如果扩展 SQL 定义发生变化，可能需要在测试库中重建扩展：

```sql
DROP EXTENSION vector CASCADE;
CREATE EXTENSION vector;
```

注意：`DROP EXTENSION vector CASCADE;` 会删除依赖该扩展的对象，测试库可以这样做，重要数据库不要随便执行。

---

## 9. 使用 VS Code 辅助开发

### 9.1 打开项目

VS Code 中选择：

```text
File -> Open Folder -> F:\_WORK\PgVector
```

主要关注文件：

```text
src/hnsw.c          HNSW 参数注册、Index AM 注册、代价估算
src/hnswscan.c      HNSW 查询扫描入口
src/hnswutils.c     HNSW 搜索核心逻辑
src/hnsw.h          HNSW 数据结构和函数声明
sql/vector.sql      扩展 SQL 对象定义
test/hot_cold       热冷分层测试脚手架
```

### 9.2 推荐阅读顺序

```text
vector.c
  -> HnswInit()
  -> hnswhandler()
  -> hnswbeginscan()
  -> hnswrescan()
  -> hnswgettuple()
  -> HnswSearchLayer()
```

这个顺序能从 PostgreSQL 加载扩展一路看到 HNSW 查询核心路径。

### 9.3 VS Code 中运行命令

普通 Git 操作可以直接在 VS Code 终端中运行：

```powershell
git status
git diff
git add .
git commit -m "docs: update setup guide"
```

编译建议仍然使用 `x64 Native Tools Command Prompt for VS 2022`，因为它已经设置好 C/C++ 编译环境。

---

## 10. 测试 hot/cold 实验脚手架

当前项目已经有热冷分层测试目录：

```text
test/hot_cold
```

其中：

```text
001_hot_cold_smoke.pl       冒烟测试
002_hot_cold_recall.pl      Recall@10 对比测试
003_hot_cold_latency_probe.sql
004_hot_cold_buffers_probe.sql
```

注意：当前源码如果还没有实现并注册这些 GUC：

```text
hnsw.hot_cold_enabled
hnsw.hot_layer
hnsw.hot_max_bytes
hnsw.prefetch_neighbors
```

那么 Perl 自动化测试会自动 skip。这是测试脚本的设计，不是失败。

后续我们实现 GUC 后，测试就可以真正运行。

---

## 11. 安装 Perl 以运行自动化测试

PostgreSQL 的部分测试脚本依赖 Perl。

Windows 推荐安装 Strawberry Perl：

```text
https://strawberryperl.com/
```

安装后确认：

```powershell
perl -v
```

运行 hot/cold 测试：

```powershell
cd F:\_WORK\PgVector
perl test\hot_cold\001_hot_cold_smoke.pl
perl test\hot_cold\002_hot_cold_recall.pl
```

或：

```powershell
powershell -ExecutionPolicy Bypass -File test\hot_cold\run_hot_cold_tests.ps1
```

---

## 12. 常见问题

### 12.1 nmake 不是内部或外部命令

说明没有在 Visual Studio 的开发者命令行中运行。

请打开：

```text
x64 Native Tools Command Prompt for VS 2022
```

再执行：

```cmd
nmake /F Makefile.win
```

### 12.2 找不到 pg_config.exe

检查 PostgreSQL 是否安装完整，以及路径是否正确：

```cmd
dir "C:\Program Files\PostgreSQL\17\bin\pg_config.exe"
```

然后设置：

```cmd
set "PGROOT=C:\Program Files\PostgreSQL\17"
```

### 12.3 vector.dll 无法覆盖

通常是 PostgreSQL 服务正在运行并加载了旧 DLL。

解决方法：

1. 停止 PostgreSQL 服务
2. 重新执行 `nmake /F Makefile.win install`
3. 启动 PostgreSQL 服务

### 12.4 CREATE EXTENSION vector 失败

先查看错误信息。

常见原因：

```text
没有安装到正确 PostgreSQL 目录
PostgreSQL 版本和 PGROOT 不一致
vector.control 没有复制到 share/extension
vector.dll 没有复制到 lib
```

检查：

```powershell
dir "C:\Program Files\PostgreSQL\17\share\extension\vector.control"
dir "C:\Program Files\PostgreSQL\17\lib\vector.dll"
```

### 12.5 修改 C 代码后没有生效

可能原因：

```text
没有重新 install
PostgreSQL 服务仍在使用旧 DLL
psql 会话没有重连
测试数据库里扩展对象没有重建
```

建议完整执行：

```text
停止 PostgreSQL 服务
nmake clean
nmake
nmake install
启动 PostgreSQL 服务
重新打开 psql
DROP EXTENSION vector CASCADE;
CREATE EXTENSION vector;
```

### 12.6 已经装过官方 pgvector，会冲突吗

会使用同一个扩展名 `vector`，所以文件层面会覆盖。

开发阶段建议只保留一个 PostgreSQL 实例用于实验，避免影响正式数据库。

如果担心影响本机其他项目，可以单独安装一个 PostgreSQL 开发实例，例如：

```text
C:\Program Files\PostgreSQL\17-dev
```

或使用 Docker 做隔离。

---

## 13. 推荐的项目展示路线

建议后续把 GitHub 项目整理成以下结构：

```text
README.md
  项目背景、优化目标、快速开始、实验结果

doc/
  github_vscode_pgvector_setup.md
  hnsw_architecture.md
  hot_cold_design.md
  benchmark_report.md

src/
  pgvector 源码与优化实现

test/hot_cold/
  热冷分层测试与探针
```

建议 README 的核心亮点写成：

```text
- 深入分析 pgvector HNSW Index AM 查询路径，梳理 PostgreSQL 扩展、索引访问方法、Buffer Pool 协作机制
- 设计 HNSW hot/cold 分层策略，将高频上层节点作为热数据优先保留，底层节点通过 Buffer Pool 按需访问
- 增加可灰度的 GUC 开关与 recall/latency/buffer 探针，验证 Recall@10、执行时延和 shared buffer 命中变化
```

简历中可以写：

```text
基于 pgvector 源码实现 PostgreSQL HNSW 向量索引优化实验，分析 Index AM 查询路径与 HnswSearchLayer 热点，设计 Buffer Pool 冷热分层策略，并构建 Recall@10、EXPLAIN BUFFERS、延迟探针等验证体系。
```

---

## 14. 最小跑通清单

第一次配置时，按下面顺序完成即可：

```text
1. 安装 Git
2. 初始化本项目 Git 仓库
3. 创建 GitHub 仓库并 push
4. 安装 VS Code 和 C/C++ 插件
5. 安装 PostgreSQL
6. 安装 Visual Studio Build Tools
7. 打开 x64 Native Tools Command Prompt
8. 设置 PGROOT
9. 执行 nmake /F Makefile.win
10. 执行 nmake /F Makefile.win install
11. 在 psql 中 CREATE EXTENSION vector
12. 创建测试表并验证 HNSW 查询
13. 修改代码后重新编译安装
14. 用 Git 提交每个阶段成果
```

跑通以上流程后，本机就具备了完整的 VS Code + PostgreSQL + 自研 pgvector 扩展开发环境。
