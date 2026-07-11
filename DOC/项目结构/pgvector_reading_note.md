# DAY1

## 任务要求

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

## `SQL -> operator -> opclass -> index AM handler`

operator 不是“把用户语句转为基本操作符”的步骤；它本身就是 SQL 里的运算符对象（如 <->），在 catalog 里绑定到具体函数。
opclass 不是语法概念，而是**“某个索引方法下，某种数据类型可用哪些操作符/支持函数”**的规则包。
index AM handler 你的理解基本正确：它是索引实现向 PostgreSQL 暴露回调能力的入口（和 Index AM API 对接）。

1) SQL
用户写查询语句，里面出现向量列、运算符、排序、LIMIT。
2) operator（如 <->）
<-> 是已注册的操作符对象，语义是“距离”。
它绑定到某个底层函数（比如 L2 distance 的实现）。
重点：operator 定义了“你在 SQL 里想做什么计算”。
3) opclass（操作符类）
当你建索引时会指定 opclass（如某列用哪种 distance 语义）。
opclass 告诉 planner/AM：
这个类型在该 AM 下支持哪些 operator；
这些 operator 如何映射为索引可理解的策略；
需要哪些 support function（比较、距离、代价相关支持等）。
重点：opclass 是“operator 能否被该索引用起来”的桥梁。
4) index AM handler（如 hnswhandler / ivfflathandler）
这是 AM 的入口函数，返回一组回调（build/insert/scan/vacuum/cost...）。
planner 选中该索引路径后，executor 通过这些回调真正执行索引扫描。
重点：handler 是“怎么做索引工作”的实现入口。

**一句话版心智模型**
operator：我要算什么（距离/比较）
opclass：这个“算什么”在该索引里如何被支持
AM handler：真正执行建索引和扫描的代码入口

**你提到的“PG 操控 PGV 的接口”怎么理解最准确**
可以这么说： PostgreSQL 通过 Index AM API + catalog（operator/opclass）机制，统一调度 pgvector 的 HNSW/IVFFlat 实现。 所以不是“PG 直接调用某个算法函数”，而是先走规则层（operator/opclass），再走实现层（AM callbacks）。

##  `ORDER BY embedding <-> query LIMIT k`

1. SQL 层表达需求 ORDER BY embedding <-> query LIMIT k 表示“按距离升序取 Top-k”。
2. operator 绑定函数 <-> 在 sql/vector.sql 里被注册为 operator，并绑定到距离函数（如 l2_distance）。 见：sql/vector.sql（CREATE OPERATOR <-> ... PROCEDURE = l2_distance）。
3. opclass 声明该 operator 可被某 AM 用于 ORDER BY 例如： CREATE OPERATOR CLASS vector_l2_ops ... USING hnsw AS OPERATOR 1 <-> ... FOR ORDER BY float_ops 同理也有 USING ivfflat 版本。 这一步决定了：planner 能识别“这个 ORDER BY operator 可以走该索引”。
4. AM 能力声明：支持 order-by operator 在 src/hnsw.c / src/ivfflat.c 的 handler 返回结构中，amcanorderbyop = true。 这告诉 PG：该 AM 支持按 operator 顺序做索引扫描（KNN/ANN 场景关键能力）。
5. planner 成本选择 + LIMIT 影响 LIMIT k 会让“从索引直接拿前 k 个候选”更有吸引力（避免全表排序），再结合 amcostestimate 估算决定是否用索引路径。
6. 执行器调用 AM 回调 选中后进入 hnswhandler / ivfflathandler 提供的回调（如 ambeginscan、amgettuple），实际执行向量近邻扫描。

# DAY2	

## 任务要求

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

## `src/vector.h`

**宏定义**

| 宏名                  | 替换文本                                     | 含义                                    |
| --------------------- | -------------------------------------------- | --------------------------------------- |
| VECTOR_MAX_DIM        | 16000                                        | vector数据类型的最大维度为16000         |
| VECTOR_SIZE(_dim)     | (offsetof(Vector, x) + sizeof(float)*(_dim)) | 存储一个_dim维vector所需字节数          |
| DatumGetVector(x)     | ((Vector *) PG_DETOAST_DATUM(x))             | 将PostgreSQL的Datum类型转换为Vector指针 |
| PG_GETARG_VECTOR_P(x) | DatumGetVector(PG_GETARG_DATUM(x))           | 获取函数第x个参数，返回Vector指针       |
| PG_RETURN_VECTOR_P(x) | PG_RETURN_POINTER(x)                         | 从PostgreSQL函数返回一个Vector指针      |

因为宏定义可以将宏定义直接转换为替换文本，因此替换文本中的函数名无需在这之前定义。

**vector结构体**

| 数据类型/成员变量              | 含义                      |
| ------------------------------ | ------------------------- |
| int32/vl_len_                  | varlena头部，不可直接操作 |
| int16/dim                      | vector维度数              |
| int16/unused                   | 保留字段，始终为0         |
| float/x[FLEXIBLE_ARRAY_MEMBER] | 向量元素数组，为柔性数组  |

varlena是PostgreSQL 对“可变长度数据类型”的统一内存格式，用于记录整个对象（即vector）的长度（含头部）。
unused用于对齐补位，int16 dim + int16 unused 后，float x[] 正好 4 字节对齐。
FLEXIBLE_ARRAY_MEMBER不是pgvector自己定义的，来自PostgreSQL公共头文件，本质是对 C 语言“柔性数组成员”的兼容宏封装。
vector的维度是运行时才知道的（dim 可变），所以数组长度不能写死。

**函数**

| 函数名              | 输入/输出                        | 功能                                                        |
| ------------------- | -------------------------------- | ----------------------------------------------------------- |
| InitVector          | int dim / Vector*                | 分配并初始化一个dim维的零向量，返回指向新Vector结构体的指针 |
| PrintVector         | char *msg, Vector *vector / void | 将向量内容打印到日志，msg为前缀说明字符串                   |
| vector_cmp_internal | Vector *a, Vector *b / int       | 比较两个向量的大小                                          |

## `src/vector.c`

**vector.c 是 pgvector 的“类型实现 + SQL 函数接口适配层”**：它一端对接 PostgreSQL 的 `Datum/PG_FUNCTION_ARGS/varlena` 约定，另一端实现向量的输入输出、运算、比较、聚合与类型转换。

**宏定义**

| 宏名 | 定义/来源 | 作用 |
| ---- | --------- | ---- |
| `STATE_DIMS(x)` | `vector.c` | 从聚合状态数组 `float8[]` 里取向量维度（`dim = length - 1`） |
| `CreateStateDatums(dim)` | `vector.c` | 为聚合中间态分配 `Datum` 数组（`dim + 1`） |
| `VECTOR_TARGET_CLONES` | `vector.c` | 在支持场景下为热点距离函数生成 FMA 优化版本 |
| `vector_isspace(ch)` | `vector.c` / PG17+ | 统一空白字符判断，兼容不同 PG 版本 |
| `AppendChar(ptr, c)` | `vector.c` | 向输出缓冲区写 1 个字符并移动指针 |
| `AppendFloat(ptr, f)` | `vector.c` | 以“最短十进制”写出 float，减少文本膨胀 |
| `VECTOR_SIZE(dim)` | `vector.h` | 计算 `Vector` 的实际内存大小（头 + 数据） |
| `PG_GETARG_VECTOR_P(n)` | `vector.h` | 从 PG 调用参数提取 `Vector*` |
| `SET_VARSIZE(ptr, len)` | PostgreSQL | 设置 varlena 对象总长度字段 |
| `PG_RETURN_POINTER(x)` | PostgreSQL | 将 C 指针按 `Datum` 返回给 PG |

> 补充理解：你在 `vector.c` 看到的大量 `PG_*` 宏，本质是 PostgreSQL 为“扩展函数 ABI”提供的胶水层。

**结构体汇总**

| 结构体 | 来源 | 在 `vector.c` 的职责 |
| ------ | ---- | -------------------- |
| `Vector` | `src/vector.h` | pgvector 稠密向量内部格式：`vl_len_ + dim + unused + x[]` |
| `ArrayType` | PostgreSQL | 承载 `anyarray/float8[]`，用于数组转换和聚合状态 |
| `StringInfoData` | PostgreSQL | 二进制协议发送缓冲（`vector_send`） |
| `VarBit` | PostgreSQL | `binary_quantize` 的 bit 向量结果类型 |
| `HalfVector` | `src/halfvec.h` | 半精度向量，供 `halfvec_to_vector` 升精度 |
| `SparseVector` | `src/sparsevec.h` | 稀疏向量，供 `sparsevec_to_vector` 转稠密 |

> 补充理解：`vector.c` 的主角是 `Vector`，其余结构体都在扮演“桥接器”（协议、数组、其他向量类型）。

### 模块1：类型基础与边界

代表函数/符号：`_PG_init`、`InitVector`、`CheckDim`、`CheckExpectedDim`、`CheckElement`  
目标：搞懂“一个 `vector` 在 PG 内部怎么存、哪里做安全校验”

| 函数名 | 输入/输出 | 功能 |
| ------ | --------- | ---- |
| `_PG_init` | 无 / `void` | 扩展加载时初始化 bit/halfvec/hnsw/ivfflat 子系统 |
| `InitVector` | `int dim` / `Vector*` | 按 varlena 规范分配并初始化零向量 |
| `CheckDim` | `int dim` / `void` | 校验维度范围 `[1, VECTOR_MAX_DIM]` |
| `CheckExpectedDim` | `int32 typmod, int dim` / `void` | 校验与 `vector(n)` 的维度约束一致 |
| `CheckElement` | `float value` / `void` | 禁止 `NaN/Inf`，保证距离计算可用 |

**`_PG_init`**

**函数功能**：扩展被 PostgreSQL `LOAD` 时自动执行，注册并初始化所有子模块。  
**输入/输出**：无参数，返回 `void`。  
**实现原理**：调用四个初始化入口，完成 CPU 特性探测、GUC 注册、索引 AM 选项注册。  
**核心代码解读**：

```c
PGDLLEXPORT void _PG_init(void); // PGDLLEXPORT为PG用导出符号宏，用于将该函数暴露给PG
void
_PG_init(void)
{
    BitvecInit();   // 位向量能力初始化（含 SIMD 检测）
    HalfvecInit();  // halfvec 能力初始化
    HnswInit();     // HNSW GUC/选项注册
    IvfflatInit();  // IVFFlat GUC/选项注册
}
```
这段代码是“扩展总开关”：若没有它，后续 SQL 运算符和索引参数都无法正确挂到 PG 上。

**`InitVector`**

**函数功能**：创建一个指定维度的 `Vector`，并把元素初始化为 `0`。  
**输入/输出**：输入 `dim`；输出 `Vector*`。  
**实现原理**：计算对象大小 `VECTOR_SIZE(dim)`，`palloc0` 申请并清零，`SET_VARSIZE` 写入 varlena 头。  
**核心代码解读**：

```c
/* SET_VARSIZE是PG内部varlena系统的宏，用于修改结构体最前面的varlena长度字段，从而使PG知道当前自定义类型占多少字节 */
size = VECTOR_SIZE(dim);
result = (Vector *) palloc0(size); // PG 内存上下文分配 + 清零
SET_VARSIZE(result, size);         // 写 varlena 长度头
result->dim = dim;                 // 记录维度
```
这是 `vector.c` 最关键的“对象构造函数”，几乎所有返回 `Vector` 的函数最终都走它。

**`CheckDim / CheckExpectedDim / CheckElement / CheckDims`**

**函数功能**：形成三层防线：维度合法、typmod 一致、元素可计算。CheckDims用于向量维度比较。  
**输入/输出**：输入维度或元素值；违规时 `ereport(ERROR, ...)`，否则无返回值。  
**实现原理**：统一在入口处 fail-fast，避免错误数据流入距离函数/索引层。  
**核心代码解读**：

```c
/* 
	CheckDim检查当前向量的维度是否在有效范围内，即[1, VECTOR_MAX_DIM(16000)]
	CheckExpectedDim检查vector维度是否符合类型修饰符typmod要求，typmod 在CREATE TABLE t (v vector(3))中指定
	CheckElement检查元素是否可用于计算，不可为NaN或Inf
	CheckDims检查所输入的两个vector指针指向的vector维度是否相等
*/
if (dim < 1) ERROR(...);                // 至少 1 维
if (dim > VECTOR_MAX_DIM) ERROR(...);   // 上限保护
if (typmod != -1 && typmod != dim) ERROR(...); // vector(n) 一致性
if (isnan(value) || isinf(value)) ERROR(...);  // 元素可计算性
if (a->dim != b->dim) ERROR(...); // 向量维度相等判别
```
这四类检查决定了 pgvector 的“数据卫生”底线。

### 模块2：I/O 与 typmod

代表函数：`vector_in`、`vector_out`、`vector_recv`、`vector_send`、`vector_typmod_in`、`vector`  
目标：搞懂“文本/二进制输入如何被解析并约束为合法向量”

| 函数名 | 输入/输出 | 功能 |
| ------ | --------- | ---- |
| `vector_in` | `cstring, oid, int4` / `Vector*` | 文本输入解析（如 `'[1,2,3]'::vector`） |
| `vector_out` | `Vector*` / `cstring` | 向量转文本（如查询结果显示） |
| `vector_recv` | `StringInfo, oid, int4` / `Vector*` | 二进制输入解析（COPY BINARY/协议） |
| `vector_send` | `Vector*` / `bytea` | 二进制输出序列化 |
| `vector_typmod_in` | `cstring[]` / `int32` | 解析 `vector(n)` 里的维度 `n` |
| `vector` | `Vector*, int4, bool` / `Vector*` | typmod 强制转换时做维度检查 |

**`vector_in`**

**函数功能**：把文本格式向量cstring解析成内部 `Vector`。  
**输入/输出**：输入字符串字面量 + typmod；输出 `Vector*`。  
**实现原理**：字符扫描 + `strtof` 转 float + 完整语法检查（`[`、`,`、`]`、尾随垃圾）+ 维度/元素检查。  
**核心代码解读**：

```c
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_in);
Datum
vector_in(PG_FUNCTION_ARGS)		// 这里的 PG_FUNCTION_ARGS 是 PG 的统一接口，真正的参数要用 PG_GETARG_* 宏取出来。
{
    char	   *lit = PG_GETARG_CSTRING(0);	// 原始字符串，比如 "[1, 2, 3]"
	int32		typmod = PG_GETARG_INT32(2);	// 类型修饰符
	float		x[VECTOR_MAX_DIM];  /* 临时存储解析出的元素值 */
	int			dim = 0;	// 当前已经解析了多少个元素
	char	   *pt = lit;	// 当前扫描位置的指针，在字符串里一路往前走
	Vector	   *result;		// 结果
    if (*pt != '[') ERROR(...);      // 格式检查，必须以 [ 开始
	if (*pt == ']') ERROR(...);		// 格式检查，不可为空vector
    
    for (;;)	// 循环解析
	{
        float		val;	// float值
		char	   *stringEnd;	// 字符串结尾指针
        if (dim == VECTOR_MAX_DIM) ERROR(...);		// 维度检查，不可超过上限
        while (vector_isspace(*pt)) pt++;			// 忽略空格，直接跳过
        if (*pt == '\0') ERROR(...);				// 不可出现字符串结束符
        
        errno = 0;	// errno是一个全局变量（准确说是thread-local global），C标准库的很多函数在发生错误时会自动修改errno
		val = strtof(pt, &stringEnd);	// strtof：将字符串转为 float，stringEnd 指向未消耗的位置
        if (stringEnd == pt) ERROR(...);	// pt未移动，解析失败
        if (errno == ERANGE && isinf(val)) ERROR(...);	// 专门捕获 “溢出导致的 Inf”，而不是用户输入的
        CheckElement(val);	// 不允许 NaN 或 Inf 出现在向量里
		x[dim++] = val;		// 结果存入可变数组x[]
		pt = stringEnd;		// 字符指针前进至未解析位置
        
        while (vector_isspace(*pt)) pt++;	// 跳过空格，寻找分隔符
        if (*pt == ',') 			//  ,直接跳过，开启下一轮解析
		else if (*pt == ']')		// ]则跳出循环、结束解析
		else ERROR(...);			// 非法字符报错
    }    
    while (vector_isspace(*pt)) pt++;
    if (*pt != '\0') ERROR(...);	// ] 后只可有空格
    CheckDim(dim);					// 维度必须在 [1, VECTOR_MAX_DIM] 范围内
    CheckExpectedDim(typmod, dim);	// 如果列类型是 vector(3)，那 dim 必须等于 3
    result = InitVector(dim);		// 新建一个vector变量
    for (int i = 0; i < dim; i++)	// 将数据写入
    	result->x[i] = x[i];
    PG_RETURN_POINTER(result);		// 以 Datum 形式返回给 PostgreSQL
}
```
`vector_in` 是“用户输入第一道门”，SQL 里大多数错误提示都来自这里。

**`vector_out`**

**函数功能**：将内部 `Vector` 格式化为可读文本。  
**输入/输出**：输入 `Vector*`；输出 `cstring`（如 `[1,2,3]`）。  
**实现原理**：预分配缓冲区，循环写入 `[`、`,`、元素最短十进制、`]`。  
**核心代码解读**：

```c
/*
 * 输入：Vector 指针
 * 输出：cstring，格式如 "[1,-0.1,2.3456]"
 *
 * 缓冲区大小计算：
 *   每个 float 最多需要 FLOAT_SHORTEST_DECIMAL_LEN - 1 字节（含小数点和指数）
 *   + dim-1 个逗号分隔符 + '[' + ']' + '\0' = 3 字节
 *   合并为 FLOAT_SHORTEST_DECIMAL_LEN * dim + 2
 *
 * PG_FREE_IF_COPY：若参数是解压副本则释放，避免内存泄漏。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_out);
Datum
vector_out(PG_FUNCTION_ARGS)
{
    Vector *vector = PG_GETARG_VECTOR_P(0);   // 输入向量
    int dim = vector->dim;                    // 维度
    char *buf, *ptr;

    buf = (char *) palloc(FLOAT_SHORTEST_DECIMAL_LEN * dim + 2); // 预分配输出缓冲区
    ptr = buf;

    *ptr++ = '[';                             // 写入 '['

    for (int i = 0; i < dim; i++)
    {
        if (i > 0) *ptr++ = ',';              // 逗号分隔符（从第二个元素开始）
        ptr += float_to_shortest_decimal_bufn(vector->x[i], ptr);  
        // ↑ 最短十进制输出 float，ptr 前进写入的字节数
    }

    *ptr++ = ']';                             // 写入 ']'
    *ptr = '\0';                              // C 字符串结束符

    PG_FREE_IF_COPY(vector, 0);               // 若参数是解压副本则释放
    PG_RETURN_CSTRING(buf);                   // 返回文本格式
}
```
“最短十进制”输出既保证 round-trip，又减少无意义小数位。

**`vector_recv / vector_send`**

**函数功能**：实现二进制协议的读写。二进制是指二进制流，我们需要掌握各个部分所占的字节数才能实现拆包  
**输入/输出**：`recv` 读 `StringInfo -> Vector*`；`send` 写 `Vector* -> bytea`。  
**实现原理**：固定头 `int16 dim + int16 unused`，后跟 `dim` 个 `float4`。  
**核心代码解读**：

```c
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_recv);
Datum
vector_recv(PG_FUNCTION_ARGS)
{
    StringInfo buf = (StringInfo) PG_GETARG_POINTER(0);   // 二进制输入缓冲区
    int32 typmod = PG_GETARG_INT32(2);                    // 类型修饰符
    Vector *result;
    int16 dim, unused;
    dim = pq_getmsgint(buf, sizeof(int16));               // 读取维度
    unused = pq_getmsgint(buf, sizeof(int16));            // 保留字段（必须为 0）
    CheckDim(dim);                                        // 维度合法性检查
    CheckExpectedDim(typmod, dim);                        // typmod 匹配检查
    if (unused != 0) ERROR(...);                          // unused 必须为 0
    result = InitVector(dim);                             // 分配向量
    for (int i = 0; i < dim; i++)
    {
        result->x[i] = pq_getmsgfloat4(buf);              // 读取 float32
        CheckElement(result->x[i]);                       // 禁止 NaN/Inf
    }
    PG_RETURN_POINTER(result);                            // 返回 Vector*
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_send);
Datum
vector_send(PG_FUNCTION_ARGS)
{
    Vector *vec = PG_GETARG_VECTOR_P(0);      // 输入向量
    StringInfoData buf;
    pq_begintypsend(&buf);                    // 初始化发送缓冲区
    pq_sendint(&buf, vec->dim, sizeof(int16)); // 写入维度
    pq_sendint(&buf, vec->unused, sizeof(int16)); // 写入 unused（通常为 0）
    for (int i = 0; i < vec->dim; i++)
        pq_sendfloat4(&buf, vec->x[i]);       // 写入 float32
    PG_RETURN_BYTEA_P(pq_endtypsend(&buf));   // 返回二进制格式
}

```
文本 I/O 面向人类，binary I/O 面向性能与网络效率。

**向量的二进制格式**

```
+--------+---------+-----------+-----------+------+
| int16  | int16   | float32   | float32   | ...  |
|  dim   | unused  |   x0      |    x1     | ...  |
+--------+---------+-----------+-----------+------+
```

**PG的Varlena格式（二进制流读取完成后补充varlena头）**

```
+------------+--------+--------+--------+------+
| varlena hdr|  dim   | unused |  x0    | ...  |
+------------+--------+--------+--------+------+
```

**`vector_typmod_in / vector`**

**函数功能**：前者解析 `vector(n)`，后者在强制转换时做维度约束检查。  
**输入/输出**：`vector_typmod_in(PG_FUNCTION_ARGS)`；`vector(PG_FUNCTION_ARGS)`。  
**实现原理**：把 DDL 中的 `n` 固化为 typmod，DML/CAST 阶段用 `CheckExpectedDim` 执行。  
**核心代码解读**：

```c
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_typmod_in);
Datum
vector_typmod_in(PG_FUNCTION_ARGS)
{
    ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);   // 输入：cstring[]，来自 vector(n) 的 n
    int32     *tl;                              // 解析出的typmod数组
    int        n;                               // typmod数组元素个数
    tl = ArrayGetIntegerTypmods(ta, &n);        // 从 cstring[] 中解析整数 typmod
    if (n != 1) ERROR(...);                     // 必须是 vector(单个参数)
    if (*tl < 1) ERROR(...);                    // 维度必须 >= 1
    if (*tl > VECTOR_MAX_DIM) ERROR(...);       // 维度不能超过 VECTOR_MAX_DIM
    PG_RETURN_INT32(*tl);                       // 返回 typmod（即维度）
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector);
Datum
vector(PG_FUNCTION_ARGS)
{
    Vector *vec = PG_GETARG_VECTOR_P(0);   // 输入向量
    int32 typmod = PG_GETARG_INT32(1);     // 目标 typmod（如 vector(3)）
    CheckExpectedDim(typmod, vec->dim);    // typmod 维度检查（不修改数据）
    PG_RETURN_POINTER(vec);                // 直接返回原向量
}
```
可把 typmod 理解为“列级维度契约”。

### 模块3：类型转换与工具函数

代表函数：`array_to_vector`、`vector_to_float4`、`halfvec_to_vector`、`sparsevec_to_vector`、`vector_dims`、`vector_norm`、`l2_normalize`  
目标：搞懂“`vector` 与 PG 其他类型如何互通”

| 函数名 | 输入/输出 | 功能 |
| ------ | --------- | ---- |
| `array_to_vector` | `anyarray, int4, bool` / `Vector*` | 一维数组转向量（int4/float4/float8/numeric） |
| `vector_to_float4` | `Vector*` / `float4[]` | 向量转 PG 数组 |
| `halfvec_to_vector` | `HalfVector*, int4, bool` / `Vector*` | 半精度转单精度向量 |
| `sparsevec_to_vector` | `SparseVector*, int4, bool` / `Vector*` | 稀疏转稠密向量 |
| `vector_dims` | `Vector*` / `int32` | 返回维度 |
| `vector_norm` | `Vector*` / `float8` | 返回 L2 范数 |
| `l2_normalize` | `Vector*` / `Vector*` | 归一化到单位向量 |

**`array_to_vector`**

**函数功能**：把 PG 数组规范化为 `Vector`。所有数据类型均转为float32。  
**输入/输出**：输入一维非空数组和 typmod；输出 `Vector*`。  
**实现原理**：`deconstruct_array` 拆箱 `Datum[]`，按元素 OID 分支转换为 float，再做维度/元素检查。  

| PG 类型 | 内部表示 | 转成 vector.x[i] 时的行为                        |
| ------- | -------- | ------------------------------------------------ |
| int4    | int32    | int32 → float32（无损）                          |
| float4  | float32  | float32 → float32（无损）                        |
| float8  | float64  | float64 → float32（可能损失精度）                |
| numeric | 任意精度 | numeric → float8 → float32（可能损失精度或溢出） |

**核心代码解读**：

```c
if (ARR_NDIM(array) > 1) ERROR(...);
if (ARR_HASNULL(array) && array_contains_nulls(array)) ERROR(...);
deconstruct_array(..., &elemsp, NULL, &nelemsp);
...
if (ARR_ELEMTYPE(array) == FLOAT8OID) result->x[i] = DatumGetFloat8(elemsp[i]);
...
CheckElement(result->x[i]);
```
这是用户把“原生数组”接入向量检索链路时的关键入口。

**`vector_norm` 与 `l2_normalize`**

**函数功能**：计算模长、生成单位向量。  
**输入/输出**：`vector_norm(Vector*) -> float8`；`l2_normalize(Vector*) -> Vector*`。  
**实现原理**：`double` 累加减少精度损失；归一化时处理零向量并检查 `Inf`。  
**核心代码解读**：
```c
for (...) norm += (double) ax[i] * (double) ax[i];
norm = sqrt(norm);
if (norm > 0) {
    for (...) rx[i] = ax[i] / norm;
    if (isinf(rx[i])) float_overflow_error();
}
```
余弦检索前常先做归一化，这两个函数是高频预处理工具。

**`halfvec_to_vector / sparsevec_to_vector`**

**函数功能**：不同向量表示之间的桥接转换。  
**输入/输出**：`halfvec(float16)/sparsevec(稀疏存储：只存非零索引和值) -> vector`。  
**实现原理**：half 逐元素升精度；sparse 先分配全零，再把非零项填回对应下标。  
**核心代码解读**：
```c
// half -> float32
for (int i = 0; i < vec->dim; i++)
    result->x[i] = HalfToFloat4(vec->x[i]);

// sparse -> dense
result = InitVector(dim); // 先全零
for (int i = 0; i < svec->nnz; i++)
    result->x[svec->indices[i]] = values[i];
```
这两类转换决定了“存储格式”和“计算格式”的边界。

### 模块4：距离与代数运算核心

代表函数：`l2_distance`、`inner_product`、`vector_negative_inner_product`、`cosine_distance`、`l1_distance`、`vector_add/sub/mul/concat`、`subvector`、`binary_quantize`  
目标：搞懂“SQL 运算符语义如何落到 C 计算”

| 函数名 | 输入/输出 | 功能 |
| ------ | --------- | ---- |
| `l2_distance` | `vector, vector` / `float8` | 欧氏距离（`<->`） |
| `inner_product` | `vector, vector` / `float8` | 内积 |
| `vector_negative_inner_product` | `vector, vector` / `float8` | 负内积（`<#>` 索引语义） |
| `cosine_distance` | `vector, vector` / `float8` | 余弦距离（`<=>`） |
| `l1_distance` | `vector, vector` / `float8` | 曼哈顿距离（`<+>`） |
| `vector_add/sub/mul` | `vector, vector` / `Vector*` | 逐元素加减乘 |
| `vector_concat` | `vector, vector` / `Vector*` | 拼接向量（`||`） |
| `binary_quantize` | `vector` / `bit` | 按符号位二值化 |
| `subvector` | `vector, int4, int4` / `Vector*` | 截取子向量 |

**`l2_distance / inner_product / cosine_distance`**

**函数功能**：三大基础相似度度量（此外还有l1_distance）。

![image-20260409124144480](C:\Users\97382\AppData\Roaming\Typora\typora-user-images\image-20260409124144480.png) 
**输入/输出**：输入两个同维向量，输出 `float8` 距离/相似度结果。  
**实现原理**：先 `CheckDims`，再分别调用内部循环函数；余弦距离对结果做 `[-1,1]` 夹紧防 NaN。  
**核心代码解读**：

```c
CheckDims(a, b);
PG_RETURN_FLOAT8(sqrt((double) VectorL2SquaredDistance(...)));
...
similarity = VectorCosineSimilarity(...);
if (similarity > 1) similarity = 1;
else if (similarity < -1) similarity = -1;
PG_RETURN_FLOAT8(1.0 - similarity);
```
注意：`<#>` 在索引里使用“负内积最小化”技巧实现 MIPS。

**`vector_add / vector_sub / vector_mul / vector_concat`**

**函数功能**：向量代数与拼接。
**输入/输出**：输入两个向量，输出新 `Vector`。  
**实现原理**：逐元素计算 + 溢出/下溢检查；拼接时先校验总维度上限。根据源码，这四个函数的对比如下：

| 函数            | SQL 运算符 | 作用                                | 输出维度        | 溢出检查   | 下溢检查                      |
| --------------- | ---------- | ----------------------------------- | --------------- | ---------- | ----------------------------- |
| `vector_add`    | `a + b`    | 逐元素相加，`rx[i] = ax[i] + bx[i]` | 与输入相同      | ✅ 检查 Inf | ❌                             |
| `vector_sub`    | `a - b`    | 逐元素相减，`rx[i] = ax[i] - bx[i]` | 与输入相同      | ✅ 检查 Inf | ❌                             |
| `vector_mul`    | `a * b`    | 逐元素相乘，`rx[i] = ax[i] * bx[i]` | 与输入相同      | ✅ 检查 Inf | ✅ 两非零数相乘结果为 0 时报错 |
| `vector_concat` | `a || b`   | 将两个向量首尾拼接为一个更长的向量  | `a.dim + b.dim` | ❌          | ❌                             |

**几点值得注意的细节：**
前三个函数都要求两个向量维度相同（通过 `CheckDims` 校验），`vector_concat` 则没有此限制，但拼接后的总维度不能超过 `VECTOR_MAX_DIM`（16000）。
`vector_mul` 比另外两个多了**下溢检查**——两个非零数相乘结果为 0 时（即精度损失到零），会调用 `float_underflow_error()` 报错，这是乘法特有的数值风险。
`vector_add` 和 `vector_sub` 的循环写法用了 `imax` 局部变量（`for (int i = 0, imax = a->dim; ...)`），这是一个小技巧，帮助编译器更好地做 SIMD 自动向量化，而 `vector_concat` 的两段循环则没有用此写法，因为拼接操作本身不是计算密集型的。 
**核心代码解读**：

```c
for (...) rx[i] = ax[i] * bx[i];
if (isinf(rx[i])) float_overflow_error();
if (rx[i] == 0 && !(ax[i] == 0 || bx[i] == 0)) float_underflow_error();

dim = a->dim + b->dim;
CheckDim(dim);
```
`vector_mul` 的下溢检查很关键：它能尽早暴露“看似 0、实则精度丢失”的问题。

**`binary_quantize / subvector`**

**函数功能**：分别用于量化压缩与局部切片。
**输入/输出**：`binary_quantize(vector)->bit`；`subvector(vector,start,count)->vector`。  
**实现原理**：量化按 `x[i] > 0` 置位并按字节打包；切片采用 SQL 风格 1-based 索引并做溢出保护。

<img src="C:\Users\97382\AppData\Roaming\Typora\typora-user-images\image-20260409130027106.png" alt="image-20260409130027106" style="zoom: 50%;" /><img src="C:\Users\97382\AppData\Roaming\Typora\typora-user-images\image-20260409130001552.png" alt="image-20260409130001552" style="zoom:50%;" />  
**核心代码解读**：

```c
result_byte |= (ax[i + j] > 0) << (7 - j); // MSB-first 位打包
...
if (start > a->dim - count) end = a->dim + 1; // 避免 start+count 溢出
for (...) result->x[i] = ax[start - 1 + i];   // 1-based -> 0-based
```
`binary_quantize` 经常用于“先粗筛再精排”的两阶段检索。

### 模块5：比较与排序语义

代表函数：`vector_cmp_internal`、`vector_lt/le/eq/ne/ge/gt`、`vector_cmp`  
目标：搞懂“为何能支持 B-tree 语义（字典序比较）”

| 函数名 | 输入/输出 | 功能 |
| ------ | --------- | ---- |
| `vector_cmp_internal` | `Vector*, Vector*` / `int` | 字典序三值比较核心实现 |
| `vector_lt/le/eq/ne/ge/gt` | `Vector*, Vector*` / `bool` | 六个比较运算符包装函数 |
| `vector_cmp` | `Vector*, Vector*` / `int32` | B-tree comparator 入口 |

**`vector_cmp_internal`**

**函数功能**：定义向量“可排序性”的基础语义。  
**输入/输出**：输入两个向量；输出 `-1/0/1`。  
**实现原理**：先比较公共前缀；若完全相同，短向量更小。  
**核心代码解读**：

```c
int dim = Min(a->dim, b->dim);
for (int i = 0; i < dim; i++) {
    if (a->x[i] < b->x[i]) return -1;
    if (a->x[i] > b->x[i]) return 1;
}
if (a->dim < b->dim) return -1;
if (a->dim > b->dim) return 1;
return 0;
```
这不是“语义相似度”，而是“排序规则”；它服务 B-tree/ORDER BY 稳定排序。

**`vector_lt/le/eq/ne/ge/gt` 与 `vector_cmp`**

**函数功能**：将内部比较结果暴露为 SQL 运算符和 B-tree 回调。  
**输入/输出**：输入两个向量；输出 `bool` 或 `int32`。  
**实现原理**：全部委托给 `vector_cmp_internal`，保持单一比较真源。  
**核心代码解读**：

```c
PG_RETURN_BOOL(vector_cmp_internal(a, b) < 0); // 其余运算符同理
...
PG_RETURN_INT32(vector_cmp_internal(a, b));    // btree comparator
```
这种“壳函数”设计降低了比较语义分叉风险。

**补充说明**

根据源码，这六个函数都只是对 `vector_cmp_internal` 返回值的不同判断，可以直接对比：

| 函数        | SQL 运算符 | 判断条件   | 返回 true 的含义     |
| ----------- | ---------- | ---------- | -------------------- |
| `vector_lt` | `<`        | `cmp < 0`  | a 字典序严格小于 b   |
| `vector_le` | `<=`       | `cmp <= 0` | a 字典序小于或等于 b |
| `vector_eq` | `=`        | `cmp == 0` | a 与 b 完全相同      |
| `vector_ne` | `<>`       | `cmp != 0` | a 与 b 存在任何差异  |
| `vector_ge` | `>=`       | `cmp >= 0` | a 字典序大于或等于 b |
| `vector_gt` | `>`        | `cmp > 0`  | a 字典序严格大于 b   |

六个函数结构完全一致，唯一区别就是最后一行的比较符号。它们都不做任何额外逻辑，全部委托给 `vector_cmp_internal`：

```c
static int vector_cmp_internal(Vector *a, Vector *b) {
    int dim = Min(a->dim, b->dim);
    for (int i = 0; i < dim; i++) {       // 先逐元素字典序比较
        if (a->x[i] < b->x[i]) return -1;
        if (a->x[i] > b->x[i]) return  1;
    }
    if (a->dim < b->dim) return -1;       // 前缀相同时，维度少的更小
    if (a->dim > b->dim) return  1;
    return 0;
}
```

有两点值得特别说明。

第一，这六个运算符用于**排序和 B-tree 索引**，与距离运算符（`<->`、`<=>`、`<#>`）是完全不同的体系——后者衡量相似度，前者只是为了让向量能被 `ORDER BY`、`DISTINCT`、建 B-tree 索引而存在的全序关系，语义上没有"向量 A 比向量 B 更好"的含义。

第二，源码里还有一个单独的 `vector_cmp` 函数（返回 `-1/0/1` 的整数），专门供 B-tree 的排序算法调用，与这六个布尔函数并列存在，是同一套比较语义的第七种导出形式。

### 模块6：聚合状态机

代表函数：`vector_accum`、`vector_combine`、`vector_avg`  
目标：搞懂“`avg(vector)` 在并行场景下如何维护状态并合并”

| 函数名 | 输入/输出 | 功能 |
| ------ | --------- | ---- |
| `vector_accum` | `float8[], vector` / `float8[]` | transfn：累计 count 与各维求和 |
| `vector_combine` | `float8[], float8[]` / `float8[]` | combinefn：并行 worker 状态合并 |
| `vector_avg` | `float8[]` / `Vector* or NULL` | finalfn：输出平均向量 |

**`vector_accum`**

**函数功能**：每来一行向量，就更新聚合中间态。  
**输入/输出**：输入状态数组与新向量；输出新状态数组。  
**实现原理**：状态布局固定为 `[count, sum0, sum1, ...]`；首次调用初始化，后续逐维累加并做溢出检查。  
**核心代码解读**：
```c
dim = STATE_DIMS(statearray);
newarr = dim == 0;
if (newarr) dim = newval->dim;
else CheckExpectedDim(dim, newval->dim);

statedatums[0] = Float8GetDatum(statevalues[0] + 1.0);
for (...) statedatums[i + 1] = Float8GetDatum(statevalues[i + 1] + x[i]);
```
用 `float8` 作为累计类型，是为了减少长期累加误差。

**`vector_combine`**

**函数功能**：把两个局部聚合状态合并成一个。  
**输入/输出**：输入两个 `float8[]` 状态；输出合并后状态。  
**实现原理**：处理“某一侧为空”分支；两侧非空时先校验维度，再逐维求和。  
**核心代码解读**：
```c
if (n1 == 0.0) { ...copy state2... }
else if (n2 == 0.0) { ...copy state1... }
else {
    n = n1 + n2;
    CheckExpectedDim(dim, STATE_DIMS(statearray2));
    for (...) statedatums[i] = Float8GetDatum(statevalues1[i] + statevalues2[i]);
}
```
这就是并行 `AVG(vector)` 能成立的关键：状态可交换、可结合。

**`vector_avg`**

**函数功能**：把中间态转为最终平均向量。  
**输入/输出**：输入状态数组；输出 `Vector*`，空集返回 `NULL`。  
**实现原理**：`sum[i] / count` 逐维计算并做元素合法性检查。  
**核心代码解读**：

```c
n = statevalues[0];
if (n == 0.0) PG_RETURN_NULL();
result = InitVector(dim);
for (...) {
    result->x[i] = statevalues[i + 1] / n;
    CheckElement(result->x[i]);
}
```
`vector_avg` 保持 SQL 语义一致：空集合平均值为 `NULL`。

# DAY3

## 任务要求

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

## vector.sql

它在 PostgreSQL 中定义了向量相关的**数据类型、函数、运算符、聚合、类型转换、索引访问方法和操作类**，以便在数据库内高效存储、比较和索引向量、半精度向量、稀疏向量与位向量。
一共创建了三个自定义类型，分别为：**vector**, **halfvec**, **sparsevec**，每种类型分别定义了多种行为。
文件开头也提醒应通过 `CREATE EXTENSION vector` 安装该脚本。

> **示例原文摘录**：`\echo Use "CREATE EXTENSION vector" to load this file. \quit`。

------

**关键功能对照表**

| **部分**              | **作用**                                                     | **SQL 示例**                                                 |
| --------------------- | ------------------------------------------------------------ | ------------------------------------------------------------ |
| **安装提示**          | 提示应通过扩展安装而非直接在 psql 中 source                  | `\echo Use "CREATE EXTENSION vector" to load this file. \quit` |
| **基础类型定义**      | 声明自定义数据类型（vector / halfvec / sparsevec）           | `CREATE TYPE vector;`                                        |
| **IO 与二进制接口**   | 注册类型的输入/输出、接收/发送函数                           | `CREATE TYPE vector (INPUT = vector_in, OUTPUT = vector_out, RECEIVE = vector_recv, SEND = vector_send, STORAGE = external);` |
| **距离/相似度函数**   | 提供 L2、L1、内积、余弦等度量函数                            | `CREATE FUNCTION l2_distance(vector, vector) RETURNS float8 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;` |
| **向量算术与变换**    | 加/减/乘、拼接、子向量、归一化、量化等                       | `CREATE FUNCTION vector_add(vector, vector) RETURNS vector AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;` |
| **聚合**              | 支持向量的 `avg` 与 `sum` 聚合实现                           | `CREATE AGGREGATE avg(vector) (SFUNC = vector_accum, STYPE = double precision[], FINALFUNC = vector_avg, INITCOND = '{0}', PARALLEL = SAFE);` |
| **类型转换与 CAST**   | 数组 ↔ 向量、向量类型间的转换函数与 CAST                     | `CREATE CAST (real[] AS vector) WITH FUNCTION array_to_vector(real[], integer, boolean) AS ASSIGNMENT;` |
| **运算符定义**        | 定义距离/相似度运算符与常规算术/比较运算符                   | `CREATE OPERATOR <-> (LEFTARG = vector, RIGHTARG = vector, PROCEDURE = l2_distance);` |
| **索引访问方法**      | 注册 ivfflat 与 hnsw 两种索引访问方法                        | `CREATE ACCESS METHOD ivfflat TYPE INDEX HANDLER ivfflathandler;` |
| **操作类（opclass）** | 为不同索引与度量组合注册 opclass（btree/ivfflat/hnsw）       | `CREATE OPERATOR CLASS vector_l2_ops DEFAULT FOR TYPE vector USING ivfflat AS OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops;` |
| **半精度与稀疏向量**  | 为 `halfvec` 与 `sparsevec` 重复类型、函数、运算符与 opclass 支持 | `CREATE TYPE halfvec;`                                       |
| **位向量距离**        | 提供 Hamming 与 Jaccard 距离及对应运算符与 opclass           | `CREATE FUNCTION hamming_distance(bit, bit) RETURNS float8 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;` |

------

**额外要点（简短说明）**

- **实现方式**：SQL 文件声明了大量函数但实际实现位于扩展的 C 模块（通过 `MODULE_PATHNAME` 引用），SQL 只做注册与绑定。
- **并行与不可变属性**：大多数函数标记为 **IMMUTABLE / STRICT / PARALLEL SAFE**，便于优化与并行执行。
- **索引用途**：`ivfflat` 与 `hnsw` 提供近似最近邻（ANN）索引支持，配合 opclass 可按不同距离度量加速查询。
- **兼容性**：通过 CAST 与转换函数，数组与不同向量类型可互转，方便与现有数据互操作。

**vector**

| **类别**                  | **说明**                                      | **示例 SQL（单行）**                                         |
| ------------------------- | --------------------------------------------- | ------------------------------------------------------------ |
| **类型声明**              | 声明自定义类型                                | `CREATE TYPE vector;`                                        |
| **IO / 二进制接口**       | 输入/输出、接收/发送、typmod 输入             | `CREATE TYPE vector (INPUT = vector_in, OUTPUT = vector_out, RECEIVE = vector_recv, SEND = vector_send, TYPMOD_IN = vector_typmod_in, STORAGE = external);` |
| **公有函数（度量/变换）** | 距离、相似度、维度、范数、归一化、子向量等    | `CREATE FUNCTION l2_distance(vector, vector) RETURNS float8 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;` |
| **私有/内部函数**         | 向量加减乘、拼接、比较、专用距离/内部实现     | `CREATE FUNCTION vector_add(vector, vector) RETURNS vector AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;` |
| **聚合**                  | avg、sum 等向量聚合实现                       | `CREATE AGGREGATE avg(vector) (SFUNC = vector_accum, STYPE = double precision[], FINALFUNC = vector_avg, INITCOND = '{0}', PARALLEL = SAFE);` |
| **类型转换函数 & CAST**   | 数组 ↔ vector、vector ↔ 其它类型的转换与 CAST | `CREATE CAST (real[] AS vector) WITH FUNCTION array_to_vector(real[], integer, boolean) AS ASSIGNMENT;` |
| **运算符**                | 距离/相似度运算符与算术/比较运算符            | `CREATE OPERATOR <-> (LEFTARG = vector, RIGHTARG = vector, PROCEDURE = l2_distance);` |
| **opclass（索引绑定）**   | 为 btree / ivfflat / hnsw 等注册 opclass      | `CREATE OPERATOR CLASS vector_l2_ops DEFAULT FOR TYPE vector USING ivfflat AS OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops;` |

**halfvec**

| **类别**                  | **说明**                                    | **示例 SQL（单行）**                                         |
| ------------------------- | ------------------------------------------- | ------------------------------------------------------------ |
| **类型声明**              | 声明半精度向量类型                          | `CREATE TYPE halfvec;`                                       |
| **IO / 二进制接口**       | 输入/输出、接收/发送、typmod 输入           | `CREATE TYPE halfvec (INPUT = halfvec_in, OUTPUT = halfvec_out, RECEIVE = halfvec_recv, SEND = halfvec_send, TYPMOD_IN = halfvec_typmod_in, STORAGE = external);` |
| **公有函数（度量/变换）** | L2、内积、余弦、L1、维度、范数、归一化等    | `CREATE FUNCTION l2_distance(halfvec, halfvec) RETURNS float8 AS 'MODULE_PATHNAME', 'halfvec_l2_distance' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;` |
| **私有/内部函数**         | halfvec 的加减乘、拼接、比较、专用距离等    | `CREATE FUNCTION halfvec_add(halfvec, halfvec) RETURNS halfvec AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;` |
| **聚合**                  | avg、sum 等                                 | `CREATE AGGREGATE avg(halfvec) (SFUNC = halfvec_accum, STYPE = double precision[], FINALFUNC = halfvec_avg, INITCOND = '{0}', PARALLEL = SAFE);` |
| **类型转换 & CAST**       | halfvec ↔ vector、数组 ↔ halfvec 等         | `CREATE CAST (halfvec AS vector) WITH FUNCTION halfvec_to_vector(halfvec, integer, boolean) AS ASSIGNMENT;` |
| **运算符**                | 与 vector 类似的距离/算术/比较运算符        | `CREATE OPERATOR <-> (LEFTARG = halfvec, RIGHTARG = halfvec, PROCEDURE = l2_distance);` |
| **opclass（索引绑定）**   | 为 ivfflat / hnsw 注册 halfvec 专用 opclass | `CREATE OPERATOR CLASS halfvec_l2_ops FOR TYPE halfvec USING ivfflat AS OPERATOR 1 <-> (halfvec, halfvec) FOR ORDER BY float_ops;` |

**sparsevec**

| **类别**                  | **说明**                                       | **示例 SQL（单行）**                                         |
| ------------------------- | ---------------------------------------------- | ---------------------------------------------------------- |
| **类型声明**              | 声明稀疏向量类型                               | `CREATE TYPE sparsevec;`                                     |
| **IO / 二进制接口**       | 输入/输出、接收/发送、typmod 输入              | `CREATE TYPE sparsevec (INPUT = sparsevec_in, OUTPUT = sparsevec_out, RECEIVE = sparsevec_recv, SEND = sparsevec_send, TYPMOD_IN = sparsevec_typmod_in, STORAGE = external);` |
| **公有函数（度量/变换）** | L2、内积、余弦、L1、范数、归一化等（稀疏实现） | `CREATE FUNCTION l2_distance(sparsevec, sparsevec) RETURNS float8 AS 'MODULE_PATHNAME', 'sparsevec_l2_distance' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;` |
| **私有/内部函数**         | 比较、专用距离、内部比较/排序函数              | `CREATE FUNCTION sparsevec_cmp(sparsevec, sparsevec) RETURNS int4 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;` |
| **类型转换 & CAST**       | sparsevec ↔ vector / halfvec / 数组 等         | `CREATE CAST (vector AS sparsevec) WITH FUNCTION vector_to_sparsevec(vector, integer, boolean) AS IMPLICIT;` |
| **运算符**                | 距离/相似度运算符与比较运算符                  | `CREATE OPERATOR <-> (LEFTARG = sparsevec, RIGHTARG = sparsevec, PROCEDURE = l2_distance);` |
| **opclass（索引绑定）**   | 为 ivfflat / hnsw 注册 sparsevec 专用 opclass  | `CREATE OPERATOR CLASS bit_hamming_ops FOR TYPE bit USING hnsw AS OPERATOR 1 <~> (bit, bit) FOR ORDER BY float_ops;` |

## vector_type.sql & vector_type.out

vector_type.sql是 pgvector 的**测试用例文件**，通常在 PostgreSQL 扩展开发中叫做 **SQL 回归测试文件**（regression test）。

**它的作用**

PostgreSQL 的测试框架（`pg_regress`）会执行这个文件里的每一条 SQL，然后把实际输出与预先保存的"期望输出文件"（通常在 `expected/` 目录下，即vector_type.out）逐行对比。两者完全一致则测试通过，否则报告差异。运行方式通常是：

```bash
make installcheck   # 或
make check
```

**文件内容的组织逻辑**

通读这个文件，可以发现它按功能模块分组，系统地覆盖了 `vector` 类型的所有行为：

| 测试组       | 代表用例                                    | 目的                                  |
| ------------ | ------------------------------------------- | ------------------------------------- |
| 合法输入解析 | `'[1,2,3]'`、`' [ 1, 2 ] '`                 | 验证空格、小数点等格式被正确接受      |
| 非法输入拒绝 | `'[hello,1]'`、`'[NaN,1]'`、`'[]'`          | 验证错误输入触发正确的报错信息        |
| 类型修饰符   | `::vector(3)`、`::vector(2)`、`::vector(0)` | 验证维度约束的合法性检查              |
| 数值边界     | `'[3e38]' + '[3e38,1]'`                     | 验证溢出、下溢、极值行为              |
| 运算符       | `+`、`-`、`*`、`||`                         | 验证向量算术的计算结果                |
| 比较运算符   | `<`、`=`、`>=` 等                           | 验证字典序比较结果                    |
| 距离函数     | `l2_distance`、`cosine_distance` 等         | 验证各距离度量的数值正确性            |
| 量化与切片   | `binary_quantize`、`subvector`              | 验证位量化和子向量提取                |
| 聚合函数     | `avg`、`sum`                                | 验证含 NULL、空集、维度不一致时的行为 |

**为什么非法用例也要写进测试**

这是回归测试的重要特点——不只测"正确输入得到正确结果"，也测"错误输入得到正确的错误"。比如 `'[NaN,1]'::vector` 应当报错，期望输出文件里记录的就是那条 `ERROR: NaN not allowed in vector` 消息。如果哪天有人修改了校验逻辑导致 NaN 被悄悄接受了，测试就会立刻发现。

**"回归"这个词的含义**

回归测试的目的是防止代码改动引入退步（regression）——即原本正确的行为被新代码破坏。每次提交代码后跑一遍，确保所有已知行为都没有变化。

# DAY4

## 任务要求

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

## `src/halfvec.h`

`halfvec.h` 与 `vector.h` 的结构骨架几乎一致（同为 varlena + dim + unused + 柔性数组），真正需要重点关注的是**元素类型与硬件分发策略**。

| 对比项   | `vector.h`                   | `halfvec.h`                                                  | 影响                                           |
| -------- | ---------------------------- | ------------------------------------------------------------ | ---------------------------------------------- |
| 元素类型 | `float` (32-bit)             | `half` (16-bit，可能是 `_Float16` 或 `uint16`)               | `halfvec` 内存约减半，但精度降低               |
| 尺寸宏   | `VECTOR_SIZE(dim)`           | `HALFVEC_SIZE(dim)`                                          | 头部一致，数据区按 `sizeof(half)` 计算         |
| 最大维度 | `VECTOR_MAX_DIM`             | `HALFVEC_MAX_DIM`                                            | 两者都为 `16000`，行为一致                     |
| 硬件分发 | 基本没有类型级分发宏         | `USE_DISPATCH` / `USE_TARGET_CLONES` / `F16C_SUPPORT` / `FLT16_SUPPORT` | `halfvec` 更依赖 CPU 特性选择最优转换/计算路径 |
| 数值上界 | 无单独 half 上界宏           | `HALF_MAX`                                                   | 转换时可精确判断 half 溢出                     |
| 导出接口 | `InitVector` + 调试/比较声明 | `InitHalfVector`（更精简）                                   | 比较/调试多在 `halfvec.c` 内部实现             |

可以把它记成一句话：**`halfvec.h` 本质是“vector 版式 + half 精度体系 + CPU 分发开关”。**

## `src/halfvec.c`

### 模块1：输入输出与二进制协议（I/O）差异

| 代表函数                        | 与 `vector.c` 的关键差异                                    |
| ------------------------------- | ----------------------------------------------------------- |
| `halfvec_in`                    | 文本先按 `float` 解析，再转 half；多了一层 half 溢出判定    |
| `halfvec_out`                   | 输出前必须 `HalfToFloat4` 升精度再格式化                    |
| `halfvec_recv` / `halfvec_send` | 二进制按 16-bit 元素读写（`pq_getmsghalf` / `pq_sendhalf`） |
| `halfvec_typmod_in`             | 逻辑与 `vector_typmod_in` 基本一致，仅类型名不同            |

核心差异点：

- `vector_in` 解析后直接存 `float`；`halfvec_in` 需要 `Float4ToHalfUnchecked`，随后用 `HalfIsInf/HalfIsNan` 做二次安全检查。
- `halfvec` 二进制协议每维 2 字节，因此网络与存储开销更小，但会引入量化误差。

### 模块2：类型转换与互操作差异

| 代表函数               | 与 `vector.c` 的关键差异                                |
| ---------------------- | ------------------------------------------------------- |
| `array_to_halfvec`     | 支持 `int4/float8/float4/numeric` 输入，但都要落到 half |
| `halfvec_to_float4`    | 返回 `float4[]`，逐元素升精度                           |
| `vector_to_halfvec`    | 从 `vector` 降精度到 `halfvec`，是精度损失主入口        |
| `sparsevec_to_halfvec` | 稀疏值写入 half 数据区，兼顾稀疏结构与低精度存储        |

实战理解：

- `halfvec` 不是“计算中间态类型”，而是“存储压缩类型”。
- 一旦从 `vector -> halfvec`，误差通常不可逆；后续再转回 `vector` 只能得到近似值。

### 模块3：距离与相似度实现差异

| 代表函数                                                   | 与 `vector.c` 的关键差异                               |
| ---------------------------------------------------------- | ------------------------------------------------------ |
| `halfvec_l2_distance` / `halfvec_l2_squared_distance`      | 调用 `Halfvec*` 内核（来自 `halfutils` 路径）          |
| `halfvec_inner_product` / `halfvec_negative_inner_product` | 同名语义一致，但底层先处理 half 再累加                 |
| `halfvec_cosine_distance`                                  | 与 `vector` 同样做范围夹紧，但前置 half 转换误差更明显 |
| `halfvec_spherical_distance`                               | 单位向量假设一致，数值边界保护同样保留                 |

核心差异点：

- 接口语义几乎与 `vector` 完全对齐，便于 SQL 层复用。
- 真正差异在数值内核：`halfvec` 更依赖 `HalfToFloat4`/SIMD 路径来平衡精度与吞吐。

### 模块4：算术、切片、量化差异

| 代表函数                               | 与 `vector.c` 的关键差异                                     |
| -------------------------------------- | ------------------------------------------------------------ |
| `halfvec_add/sub/mul`                  | 在 `FLT16_SUPPORT` 下可直接 half 运算，否则走“升精度算完再回写” |
| `halfvec_mul`                          | 除溢出检查外，还显式检查 half 下溢（结果为 0 但输入非 0）    |
| `halfvec_concat` / `halfvec_subvector` | 结构逻辑同 `vector`，但数据搬运单位变为 2 字节               |
| `halfvec_binary_quantize`              | 判正负时需先 `HalfToFloat4`，再写 bit                        |

一句话总结：**算术层的主要新增复杂度都来自“半精度边界处理”。**

### 模块5：比较与聚合差异

| 代表函数                            | 与 `vector.c` 的关键差异                            |
| ----------------------------------- | --------------------------------------------------- |
| `halfvec_cmp_internal` + 比较运算符 | 比较前要做 `HalfToFloat4`，语义仍保持与 PG 数组一致 |
| `halfvec_accum`                     | 聚合状态仍用 `float8[]`，避免 half 累加误差失控     |
| `halfvec_avg`                       | 最终结果回写 half，末端再次触发 half 合法性检查     |

额外关键点：

- `avg(halfvec)` 的 `COMBINEFUNC` 在 SQL 中复用了 `vector_combine`（`double precision[]` 状态通用），体现了“状态机层复用、类型层分化”的设计。

### 模块6：工程化取舍（为什么 halfvec 值得单独实现）

- 存储/缓存收益：元素减半，页缓存命中率通常更友好，ANN 场景常能换来更高吞吐。
- 精度代价：半精度仅约 3 位十进制有效精度，极值与细粒度排序更易受误差影响。
- 实现复杂度：需要额外 CPU 分发、转换函数、上下溢检查与协议适配。
- 设计哲学：SQL 接口尽量与 `vector` 对齐，底层实现针对 half 的物理特性做专门优化。

> 快速决策建议：对召回精度敏感、维度较低的数据优先 `vector`；对内存/吞吐敏感、可接受轻微误差的场景优先 `halfvec`。

## `src/sparsevec.h`

`sparsevec.h` 与 `vector.h` 的核心差异在于存储模型：`vector` 是稠密连续数组，`sparsevec` 是“索引 + 非零值”的紧凑结构。

| 对比项   | `vector.h`               | `sparsevec.h`                                  | 影响                         |
| -------- | ------------------------ | ---------------------------------------------- | ---------------------------- |
| 存储形态 | `x[dim]`（全量）         | `indices[nnz] + values[nnz]`（仅非零）         | 零值多时显著省空间           |
| 维度上限 | `VECTOR_MAX_DIM = 16000` | `SPARSEVEC_MAX_DIM = 1000000000`               | 更适合超高维稀疏特征         |
| 规模约束 | 无 `nnz` 字段            | `nnz` 明确存在，且 `SPARSEVEC_MAX_NNZ = 16000` | 计算复杂度取决于非零数       |
| 大小计算 | `VECTOR_SIZE(dim)`       | `SPARSEVEC_SIZE(nnz)`                          | 分配从“按维度”变为“按非零数” |
| 数据访问 | `x[i]` 直接取值          | `SPARSEVEC_VALUES(x)` 定位值区                 | 访问逻辑更复杂但更节省       |

对比记忆：`vector` 用空间换简洁，`sparsevec` 用结构复杂度换空间与稀疏计算效率。

## `src/sparsevec.c`

### 模块1：I/O 与 typmod 差异

| 代表函数                            | 与 `vector.c` 的关键差异                                     |
| ----------------------------------- | ------------------------------------------------------------ |
| `sparsevec_in`                      | 文本格式是 `{idx:val,...}/dim`，并将 SQL 的 1-based 索引转为 C 的 0-based |
| `sparsevec_out`                     | 输出时再从 0-based 转回 1-based，保证 SQL 可读性             |
| `sparsevec_recv` / `sparsevec_send` | 二进制包含 `dim + nnz + indices[] + values[]`，不是连续 `x[]` |
| `sparsevec_typmod_in` / `sparsevec` | 维度校验机制类似，但错误语义针对 sparsevec                   |

重点差异：

- `vector` 主要校验元素值；`sparsevec` 还必须校验索引有序、无重复、边界合法。
- 输入中的 `0` 值会被丢弃，保持“只存非零项”的不变式。

### 模块2：构造与类型转换差异

| 代表函数               | 与 `vector.c` 的关键差异                           |
| ---------------------- | -------------------------------------------------- |
| `InitSparseVector`     | 按 `nnz` 分配可变结构，而不是按 `dim` 分配元素数组 |
| `vector_to_sparsevec`  | 扫描稠密向量，仅抽取非零元素                       |
| `halfvec_to_sparsevec` | 使用 `HalfIsZero` 判零，再写入升精度值             |
| `array_to_sparsevec`   | 支持多种数组类型，但最终只保留非零项               |

理解要点：

- `vector -> sparsevec` 适合高零占比数据；若数据本身不稀疏，收益有限。
- `sparsevec` 的“高维可扩展性”来自不再按 `dim` 线性存储。

### 模块3：距离与范数计算差异

| 代表函数                                          | 与 `vector.c` 的关键差异                   |
| ------------------------------------------------- | ------------------------------------------ |
| `SparsevecL2SquaredDistance`                      | 通过有序索引归并计算，避免全维扫描         |
| `SparsevecInnerProduct`                           | 只在索引相同的维度上相乘                   |
| `sparsevec_l2_distance` / `sparsevec_l1_distance` | 复杂度由 `O(dim)` 转向 `O(nnz_a + nnz_b)`  |
| `sparsevec_cosine_distance`                       | 稀疏内积 + 稀疏范数归一化                  |
| `sparsevec_l2_norm` / `sparsevec_l2_normalize`    | 仅处理非零值，归一化后可能进一步压缩 `nnz` |

重点差异：

- `vector` 的性能瓶颈常在维度；`sparsevec` 的瓶颈常在非零元素规模与分布。
- 索引数组有序性是稀疏计算高效的基础。

### 模块4：比较与排序语义差异

| 代表函数                      | 与 `vector.c` 的关键差异                       |
| ----------------------------- | ---------------------------------------------- |
| `sparsevec_cmp_internal`      | 先比较索引轨道，再比较对应值，并考虑隐式零位置 |
| `sparsevec_lt/le/eq/ne/ge/gt` | 包装结构类似 `vector`，但比较真源逻辑不同      |
| `sparsevec_cmp`               | B-tree 比较入口，服务排序与去重                |

总结：`sparsevec` 的“大小关系”不仅取决于数值，还取决于非零值出现在哪些维度。

### 模块5：工程化取舍

- 优势：超高维、强稀疏数据下，存储和距离计算都更经济。
- 代价：写入与校验逻辑更复杂（索引有序/去重/越界）。
- 边界：能力重点在距离与比较，不走 `vector` 那套完整逐元素算术路径。
- 选型：当“零远多于非零”时优先 `sparsevec`，否则通常 `vector` 更直接。

## `src/bitvec.h`

`bitvec.h` 与 `vector.h` 的最大差异是：它不定义新的浮点向量结构，而是复用 PostgreSQL 内置 `VarBit`（varbit）。

| 对比项     | `vector.h`           | `bitvec.h`           | 影响                   |
| ---------- | -------------------- | -------------------- | ---------------------- |
| 类型来源   | 自定义 `Vector`      | 复用 PG `VarBit`     | 类型实现更轻量         |
| 元素语义   | 连续实数             | 二值位（0/1）        | 更适合集合/签名类特征  |
| 常见距离   | L2/IP/Cosine/L1      | Hamming/Jaccard      | 几何空间 vs 集合空间   |
| 头文件职责 | 宏/结构/比较声明较多 | 仅保留最小初始化接口 | 实现重心转移到距离内核 |

一句话：`bitvec` 不是“低精度 vector”，而是“位串 + 集合距离”。

## `src/bitvec.c`

### 模块1：类型复用与初始化差异

| 代表函数        | 与 `vector.c` 的关键差异                                |
| --------------- | ------------------------------------------------------- |
| `InitBitVector` | 用 `VARBITTOTALLEN` 分配 `VarBit`，而不是 `VECTOR_SIZE` |
| `CheckDims`     | 检查的是位长度 `VARBITLEN`，不是浮点维度                |

要点：`bitvec` 沿用 PG 原生 bit 存储协议，不需要 `vector_in/out` 那套自定义解析流程。

### 模块2：距离函数差异

| 代表函数           | 与 `vector.c` 的关键差异             |
| ------------------ | ------------------------------------ |
| `hamming_distance` | 基于 XOR + popcount 统计不同位数     |
| `jaccard_distance` | 基于位交并计数计算 `1 - |A∩B|/|A∪B|` |

要点：

- 这里不关心“数值幅值”，只关心位模式重合程度。
- 返回类型同为 `float8`，但语义属于集合距离。

### 模块3：计算路径与硬件优化差异

| 代表符号             | 与 `vector.c` 的关键差异                |
| -------------------- | --------------------------------------- |
| `BitHammingDistance` | 通过函数指针在运行时绑定最优实现        |
| `BitJaccardDistance` | 同样走动态分发路径                      |
| `BitvecInit`         | 在扩展加载期选择 CPU 最优 popcount 路径 |

总结：`bitvec` 的性能关键不在逐元素浮点循环，而在位运算与 popcount 加速。

### 模块4：工程化取舍

- 优势：内存占用最低，粗筛速度快。
- 局限：无法表达连续幅值信息，通常不适合最终精排。
- 配合方式：常见是 `bitvec` 粗筛后再用 `vector/halfvec` 精排。
- 选型：特征天然是布尔/集合语义时，`bitvec` 往往比 `vector` 更合适。

## `src/halfutils.h` & `src/halfutils.c`

`halfutils.h` 是**内联工具头文件**，负责类型判断与标量转换；`halfutils.c` 是**距离计算的运行时实现**，提供通用路径和硬件加速路径，并在初始化时决定走哪条路。两者合起来构成 `halfvec`（16位浮点向量）的底层计算层。

**功能一：`half` 类型的三种判断**

```c
HalfIsNan(num)   // (num & 0x7C00)==0x7C00 && (num & 0x7FFF)!=0x7C00
HalfIsInf(num)   // (num & 0x7FFF)==0x7C00
HalfIsZero(num)  // (num & 0x7FFF)==0x0000
```

直接操作 IEEE 754 half（1位符号 + 5位指数 + 10位尾数）的二进制表示，无需浮点运算，效率极高。编译器若定义了 `FLT16_SUPPORT`，则退回标准库的 `isnan`/`isinf`。

**功能二：`HalfToFloat4` — half → float32**

三路分支，纯位操作手动解包：符号位左移16位、指数做偏移量调整（half偏移15→float32偏移127）、尾数左移13位对齐。次正规数（exponent==0）需要额外的正规化循环。若平台支持 `F16C` 或 `_Float16`，则用单条指令/类型转换替代整段逻辑。

**功能三：`Float4ToHalf` / `Float4ToHalfUnchecked`**

逆向过程：同样三路（inf/nan/normal），包含**正确的舍入**（Guard/Round/Sticky位检查），溢出时调用 `Float4ToHalf` 的外层包装函数抛出 PostgreSQL 错误。

**核心结构：四组函数指针 + 两套实现**

每种距离有两个实现：

| 函数        | Default（通用）      | F16C（加速）                            |
| ----------- | -------------------- | --------------------------------------- |
| L2 平方距离 | `diff*diff` 标量累加 | `_mm256_fmadd_ps(diff,diff,dist)`       |
| 内积        | `ax*bx` 标量累加     | `_mm256_fmadd_ps(ax,bx,dist)`           |
| 余弦相似度  | 同时累加3个变量      | 3个 `__m256` 同步累加                   |
| L1 距离     | `fabsf(ax-bx)` 标量  | `_mm256_andnot_ps(sign, diff)` 清符号位 |

F16C 路径的关键步骤是每次迭代：`_mm_loadu_si128`（加载16字节=8个half）→ `_mm256_cvtph_ps`（8路并行 half→float32）→ FMA 计算→ 末尾处理余数。

**运行时分发机制 `SupportsCpuFeature`**

调用 `CPUID leaf 1 ECX`，依次检查三个必要条件：

1. `OSXSAVE`（位27）：OS 支持 XSAVE 状态保存；
2. `_xgetbv(0) & 6`：XMM（位1）和 YMM（位2）寄存器被 OS 激活；
3. `AVX | F16C | FMA`（位28/29/12）：CPU 本身支持这三个指令集。

三者缺一不可。`HalfvecInit()` 在扩展加载时被调用一次，根据结果将4个全局函数指针指向 Default 或 F16C 版本，之后的所有距离计算都通过这4个指针间接调用，开销仅为一次指针解引用。

**与其他文件的联系**

`halfutils.h` 依赖 `halfvec.h`（定义 `half` 类型及 `HalfVec` 结构体）和 `common/shortest_dec.h`（`Float4ToHalf` 溢出时格式化错误消息）。`immintrin.h` 仅在编译时探测到 `F16C_SUPPORT` 宏时才条件性包含。

对外，任何需要计算 halfvec 距离的模块（索引扫描、向量运算符、结果排序等）都通过这四个函数指针调用，不感知底层是标量还是 SIMD。这是典型的**运行时多态替代编译时条件编译**的设计——同一个二进制可以在不支持 F16C 的老机器上正确运行，又能在新机器上自动获得硬件加速。

## `src/bitutils.h` & `src/bitutils.c`

**bitutils.h** 是纯声明头文件，做三件事：用 `#if PG_VERSION_NUM < 130000` 强制编译期版本检查；用 `extern` 声明两个全局函数指针 `BitHammingDistance` 和 `BitJaccardDistance`（实体在 `.c` 中定义）；声明 `BitvecInit()` 供 `_PG_init` 调用。头文件本身不含任何逻辑，是模块对外的唯一接口契约。

**bitutils.c 的层次结构**可以从底向上理解。

最底层是 **`popcount64` 宏**。它在编译期根据平台能力三选一：`__builtin_popcountl`（long 为 64 位时）、`__builtin_popcountll`（long long 为 64 位时）、或 PostgreSQL 自带的 `pg_popcount64`。这一层保证了在任何 x86-64 平台上都能映射到硬件 `POPCNT` 指令，无需软件模拟。

往上是**两组距离实现**，每组各有 Default 和 AVX-512 两个版本。

**汉明距离**衡量两个位向量有多少位不同。Default 版本以 8 字节为步长，用 `memcpy` 安全读出 `uint64`（避免非对齐访问崩溃），`XOR` 后 `popcount64` 计数，余下不足 8 字节的部分查 `pg_number_of_ones` 表完成。`BIT_TARGET_CLONES` 宏让编译器额外生成一个带硬件 `popcnt` 指令的克隆版本，由 IFUNC 机制在加载时自动选择——这是比函数指针分发更细粒度的编译期优化。AVX-512 版本将步长扩大到 64 字节，用 `_mm512_loadu_si512` 加载一个 ZMM 寄存器，`_mm512_xor_si512` 做 XOR，`_mm512_popcnt_epi64` 对 8 个 64 位通道同时 popcount，`_mm512_add_epi64` 累加，最后 `_mm512_reduce_add_epi64` 水平求和。处理完整 64 字节块后，剩余部分直接转交 Default 函数——这是一个典型的"快速主路径 + 尾部回退"模式。

**Jaccard 距离**衡量两个集合的相似度，公式为 `1 - |A∩B| / (|A| + |B| - |A∩B|)`。Default 版本在同一个循环里同时累加三个量：`ab`（AND 后 popcount，即交集大小）、`aa`（A 的 1 个数）、`bb`（B 的 1 个数），最后一次性套公式。特殊情况 `ab == 0` 直接返回 1，避免除以零。AVX-512 版本将三个累加器升级为三个 `__m512i` 寄存器 `abx/aax/bbx`，并行累加，最后各自 `_mm512_reduce_add_epi64` 后再走 Default 的公式计算和余量处理。

**`SupportsAvx512Popcount` 的检测逻辑**比 `halfutils.c` 中更严格。`halfutils.c` 只需检查 XMM（位1）和 YMM（位2），掩码为 `0x6`；而这里还要检查 **ZMM 寄存器**（位5、6、7），掩码为 `0xe6`，因为 AVX-512 使用 512 位 ZMM，必须确认 OS 已通过 XSAVE 保存 ZMM 状态。此外还需要走 `CPUID leaf 7, subleaf 0`——AVX-512 特性不在 leaf 1 的 ECX 里，而在 leaf 7 的 EBX（`AVX512F`，位16）和 ECX（`AVX512VPOPCNTDQ`，位14）中，这与 `halfutils.c` 仅查 leaf 1 是明显的差异。

**与其他文件的联系**有两处值得注意。`bitutils.c` 包含 `halfvec.h` 不是为了使用 `half` 类型，而是借用其中定义的 `USE_DISPATCH` 和 `USE_TARGET_CLONES` 宏——这是两个模块共享编译配置的约定。`pg_bitutils.h` 提供了 `pg_number_of_ones`（256 项查找表，用于单字节 popcount）和 `pg_popcount64`（软件 popcount 兜底），是 PostgreSQL 核心对位运算的标准支持层。

**与 halfutils 的横向对比**：两者共享"全局函数指针 + `Init()` 运行时赋值"的分发模式，但加速层次不同。`halfutils` 用 F16C + FMA 做浮点转换和乘加，一次处理 8 个 half；`bitutils` 用 AVX-512 VPOPCNTDQ 做整数 popcount，一次处理 64 字节。指令集要求更高（AVX-512 vs AVX2），所以检测逻辑也更复杂，需要额外的 ZMM 状态检查和 leaf 7 查询。两者都在余量处理上回退到标量路径，保证了正确性的同时最大化了主路径的吞吐量。

# DAY 5

## 任务要求

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

## HNSW入门

HNSW（Hierarchical Navigable Small World）是当前最主流的近似最近邻（ANN）搜索算法，广泛用于向量数据库的核心索引层。下面从原理到实践进行系统介绍。

**核心思想**

HNSW 的灵感来自两个概念：

- **跳表（Skip List）**：多层索引结构，上层稀疏、下层密集，逐层缩小范围
- **小世界网络（Small World Graph）**：图中任意两点之间可通过少量跳转到达（"六度分隔"理论）

将两者结合，就得到了一个层次化的可导航图结构——越高层的图越稀疏，充当"高速公路"；底层图最密集，提供精确搜索。

插入一个新节点时，算法先随机决定该节点出现在哪些层（概率递减，越高层越少），然后从最高层开始贪心地搜索最近邻，逐层下降，每层选取 `M` 个最近邻与其相连。这样自然形成了稀疏到密集的层次。

**查询过程**

搜索时从最高层的入口点出发，在每层做贪心游走（始终移向距查询点更近的邻居），到达当前层最近点后下降一层，直到第 0 层，再从候选集中精确选出 top-K 结果。

**关键超参数**

| 参数               | 含义                 | 典型值  | 影响                    |
| ------------------ | -------------------- | ------- | ----------------------- |
| `M`                | 每个节点的最大连接数 | 8–64    | 越大精度↑，内存↑，构建↑ |
| `efConstruction`   | 构建时的候选集大小   | 100–400 | 越大精度↑，构建慢       |
| `ef`（`efSearch`） | 查询时的候选集大小   | 50–200  | 越大精度↑，查询慢       |

**与向量数据库的结合**

现代向量数据库（Pinecone、Weaviate、Milvus、Qdrant、pgvector 等）均将 HNSW 作为核心索引之一，集成方式大致如下：

**存储层**：向量与元数据一起持久化到磁盘，HNSW 图结构序列化存储并可内存映射加载（mmap），避免每次启动重建。

**查询层**：用户提交一个查询向量，数据库在 HNSW 图上执行 ANN 搜索，拿到候选 ID 列表，再与元数据过滤条件（如 `category = "finance"`）做后处理，返回 top-K 结果。这种 **ANN + 标量过滤** 的组合是向量数据库最常见的查询模式。

**更新层**：HNSW 支持动态插入（大多数实现也支持软删除），使向量数据库可以在不重建整个索引的情况下增量更新数据。

**性能特性**

- **精度（Recall@10）**：合理参数下通常可达 95%–99%，接近暴力精确搜索
- **查询延迟**：百万向量规模下，单次查询通常在 1–10ms 量级
- **内存占用**：每个向量需要额外存储图的邻接信息，M=16 时约比原始向量多消耗 30%–50% 内存
- **构建时间**：比 IVF 等量化方法慢，但一次构建多次受益

HNSW 的核心优势在于**无需训练**、**支持增量更新**、**精度-速度权衡灵活可调**，这使它在需要实时写入的生产环境中尤为适合。

## HNSW代码结构

<img src="C:\Users\97382\AppData\Roaming\Typora\typora-user-images\image-20260423101610449.png" alt="image-20260423101610449" style="zoom:50%;" />

**图1：HNSW 的核心直觉 —— 分层跳表**

HNSW（Hierarchical Navigable Small World）的核心思想就是这张图展示的内容。把所有向量想象成一个城市里的很多地点，最底层（第 0 层）是所有街道，上层是高速公路，更上层是高铁。搜索时先坐高铁快速接近目的地，再下到街道精细寻路。

节点只有概率性地"晋升"到高层——用 `-log(random) * ml` 决定层高，大多数节点只活在第 0 层，少数节点贯穿多层。这就是为什么搜索能做到 O(log n) 而不是 O(n)。

------

<img src="C:\Users\97382\AppData\Roaming\Typora\typora-user-images\image-20260410212842156.png" alt="image-20260410212842156" style="zoom:50%;" />

**图2：六个文件的职责划分**

所有文件都依赖 `hnsw.h`（结构体定义）和 `hnswutils.c`（核心算法）。其中 `hnswbuild.c` 有一个关键的"两阶段"切换：内存够时在内存里建图，内存满后无缝切换为调用 `hnswinsert.c` 的磁盘插入路径。

------

<img src="C:\Users\97382\AppData\Roaming\Typora\typora-user-images\image-20260410212923762.png" alt="image-20260410212923762" style="zoom:50%;" />

**图3：构建流程的两个阶段**

`hnswbuild.c` 的精髓在于这个分支：在 `maintenance_work_mem` 允许的范围内，整个图都活在内存里（速度极快），一旦内存满了，发出一条 NOTICE 然后无缝切换到逐条磁盘插入。最终无论哪条路，都会调用 `FlushPages` 将内存中构建好的图一次性批量写入磁盘。

------

<img src="C:\Users\97382\AppData\Roaming\Typora\typora-user-images\image-20260410212957691.png" alt="image-20260410212957691" style="zoom:50%;" />

**图4：核心搜索算法 —— 双堆贪心搜索**

`HnswSearchLayer` 是整个 HNSW 的心脏，被构建、插入、查询三个场景复用。它用两个优先堆（`C` 候选堆 + `W` 结果堆）实现贪心搜索：C 保证总是展开最近的节点，W 维护当前最好的 `ef` 个结果。**终止条件**是最精妙的设计：当 C 中最近的待展开节点比 W 中最差的已有结果还要远时，继续搜索已无意义。

------

<img src="C:\Users\97382\AppData\Roaming\Typora\typora-user-images\image-20260410213030318.png" alt="image-20260410213030318" style="zoom:50%;" />

**图5：磁盘存储结构**

每个向量在磁盘上对应**两个** tuple：`ElementTuple` 存向量本身的数据（向量值、堆表 TID、层号），`NeighborTuple` 存该节点在各层的邻居列表。两者通过 `neighbortid` 字段互相指向，尽量放在同一页以减少 IO。元页（Block 0）是整个索引的"目录"，记录入口点位置和最新的可插入页。

------

<img src="C:\Users\97382\AppData\Roaming\Typora\typora-user-images\image-20260410213054542.png" alt="image-20260410213054542" style="zoom:50%;" />

**图6：插入 vs 查询路径对比**

两条路径高度对称，都从读元页开始，都在高层贪心下降，都在第 0 层展开 `HnswSearchLayer`。最关键的差异是：

- **插入**还要做 `SelectNeighbors`（修剪候选邻居）+ `UpdateNeighborsOnDisk`（反向更新已有节点的邻居列表，这是最昂贵的步骤）
- **查询**有可选的**迭代扫描**模式，当 `ef_search` 不够时可以继续从 `discarded` 堆展开搜索，以换取更高召回率

------

到这里整个架构的脉络就清晰了。用一句话总结：**HNSW = 随机分层的跳表 + 贪心最近邻搜索 + PostgreSQL 的缓冲区管理**。六个文件各司其职，而 `hnswutils.c` 中的 `HnswSearchLayer` 是贯穿一切的灵魂函数。当你读某一个具体文件时，可以对照这些图找到自己的位置。

## `sql/vector.sql`

```sql
# 意思是：这个函数实现不在 SQL 里，而在扩展的 C 动态库里（这里是 vector 库, 具体为hnsw.c文件）。
# hnswhandler / ivfflathandler 是 C 里的入口函数名。
# RETURNS index_am_handler 说明这个函数返回的是“索引访问方法描述器”（一组回调函数），里面会告诉 PG：建索引调谁、插入调谁、扫描调谁、vacuum 调谁。
CREATE FUNCTION hnswhandler(internal) RETURNS index_am_handler
    AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE FUNCTION ivfflathandler(internal) RETURNS index_am_handler
    AS 'MODULE_PATHNAME' LANGUAGE C;
# 正式在 PostgreSQL 注册一个叫 hnsw 的索引方法。之后你写 CREATE INDEX ... USING hnsw，PG 就会去调用 hnswhandler 返回的那组回调。
CREATE ACCESS METHOD hnsw TYPE INDEX HANDLER hnswhandler;
```

```sql
# 距离运算符的绑定
# opclass 就是“把 <-> 和具体 C 距离函数打包后交给 ivfflat 使用”的声明。
# 前两行创建一个给ivfflat用的“规则包”（opclass），名为vector_l2_ops。这个规则包告诉索引比较“近不近”时该用哪些运算符和函数。
# DEFAULT含义为：对 vector 类型 + ivfflat 来说，它是默认 opclass（可被自动选中）。
CREATE OPERATOR CLASS vector_l2_ops
	DEFAULT FOR TYPE vector USING ivfflat AS
	# 指定“排序距离运算符”是 <->（L2 距离）。FOR ORDER BY float_ops 表示这个结果是 float，能用于 KNN 排序（ORDER BY embedding <-> query）。
	OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops,
	# 索引内部主要计算函数（常用平方距离，省掉开方，效率更高，排序等价）。
	FUNCTION 1 vector_l2_squared_distance(vector, vector),
	# 补充支持函数（返回真正 L2 距离，给某些场景/接口使用）。
	FUNCTION 3 l2_distance(vector, vector);
	
/* 
operator和function后的这些数字是“槽位编号”，不是随便写的。
 - OPERATOR 1 ... 里的 1 = strategy number（策略号）
 - FUNCTION 1/2/3 ... 里的数字 = support function number（支持函数号）
PG 用这些编号在运行时取对应函数/运算符，而不是按名字硬编码。
在这个仓库里你能直接看到对应关系，例如 src/hnsw.h 里有：
 - #define HNSW_DISTANCE_PROC 1
 - #define HNSW_NORM_PROC 2
 - #define HNSW_TYPE_INFO_PROC 3
然后代码里会用 index_getprocinfo(..., HNSW_DISTANCE_PROC) 去取 FUNCTION 1 对应的函数。
*/
```

```sql
/*
这里的 support 函数作用是：给索引 AM 提供“类型元信息”，不是直接算距离。
在 HNSW 里，它对应 FUNCTION 3（HNSW_TYPE_INFO_PROC），返回一个 HnswTypeInfo 结构，里面主要有：
 - maxDimensions：该类型允许的最大维度
 - normalize：是否/如何归一化（如 cosine 场景会用到）
 - checkValue：额外合法性检查函数（如 sparsevec 的约束）
你刚看的这几个函数就是分别给不同类型返回这份信息：
 - hnsw_halfvec_support → halfvec 的类型信息
 - hnsw_bit_support → bit 的类型信息
 - hnsw_sparsevec_support → sparsevec 的类型信息
所以它们“被复用多次”很正常：多个 opclass 都可共享同一套类型元信息。

hnsw_halfvec_support 的实现在：
 - src\hnswutils.c
 - hnsw_halfvec_support(PG_FUNCTION_ARGS)（约 L1457）
 - 同文件还有 hnsw_bit_support、hnsw_sparsevec_support
它被多次复用，是因为多个 opclass 都把它挂在 FUNCTION 3 这个槽位上。
也就是：同一个 support 函数服务多个距离 opclass。
*/
CREATE FUNCTION hnsw_halfvec_support(internal) RETURNS internal
	AS 'MODULE_PATHNAME' LANGUAGE C;
```



## `src/hnsw.h`

hnsw.h 不是“算法实现”，而是“全局协议定义”——把参数、内存布局、磁盘布局、并发状态、函数入口统一约定下来。

**宏定义**

| 宏名                                                    | 值/替换文本                        | 含义                                             |
| ------------------------------------------------------- | ---------------------------------- | ------------------------------------------------ |
| `HNSW_MAX_DIM`                                          | 2000                               | HNSW 索引支持的最大向量维度                      |
| `HNSW_MAX_NNZ`                                          | 1000                               | 稀疏向量非零元素上限                             |
| `HNSW_DISTANCE_PROC`                                    | 1                                  | 距离计算支持函数编号                             |
| `HNSW_NORM_PROC`                                        | 2                                  | 归一化支持函数编号                               |
| `HNSW_TYPE_INFO_PROC`                                   | 3                                  | 类型信息支持函数编号                             |
| `HNSW_VERSION`                                          | 1                                  | 索引格式版本号                                   |
| `HNSW_MAGIC_NUMBER`                                     | 0xA953A953                         | 元页魔数，用于识别合法 HNSW 索引                 |
| `HNSW_PAGE_ID`                                          | 0xFF90                             | 页面标识符，写入 opaque 区用于类型识别           |
| `HNSW_METAPAGE_BLKNO`                                   | 0                                  | 元页固定块号                                     |
| `HNSW_HEAD_BLKNO`                                       | 1                                  | 第一个元素页固定块号                             |
| `HNSW_UPDATE_LOCK`                                      | 0                                  | 更新操作使用的页锁编号                           |
| `HNSW_SCAN_LOCK`                                        | 1                                  | 扫描操作使用的页锁编号                           |
| `HNSW_DEFAULT_M`                                        | 16                                 | 默认最大连接数 M                                 |
| `HNSW_MIN_M` / `HNSW_MAX_M`                             | 2 / 100                            | M 的合法范围                                     |
| `HNSW_DEFAULT_EF_CONSTRUCTION`                          | 64                                 | 建索引时动态候选列表默认大小                     |
| `HNSW_MIN_EF_CONSTRUCTION` / `HNSW_MAX_EF_CONSTRUCTION` | 4 / 1000                           | efConstruction 合法范围                          |
| `HNSW_DEFAULT_EF_SEARCH`                                | 40                                 | 查询时动态候选列表默认大小                       |
| `HNSW_MIN_EF_SEARCH` / `HNSW_MAX_EF_SEARCH`             | 1 / 1000                           | efSearch 合法范围                                |
| `HNSW_ELEMENT_TUPLE_TYPE`                               | 1                                  | 元素 tuple 类型标识                              |
| `HNSW_NEIGHBOR_TUPLE_TYPE`                              | 2                                  | 邻居 tuple 类型标识                              |
| `HNSW_HEAPTIDS`                                         | 10                                 | 每个元素存储的堆 TID 上限，用于应对 non-HOT 更新 |
| `HNSW_UPDATE_ENTRY_GREATER`                             | 1                                  | 仅当新元素层级更高时更新入口点                   |
| `HNSW_UPDATE_ENTRY_ALWAYS`                              | 2                                  | 无条件更新入口点                                 |
| `PROGRESS_HNSW_PHASE_LOAD`                              | 2                                  | 索引构建进度阶段：加载 tuple                     |
| `HNSW_MAX_SIZE`                                         | 页内可用最大字节数                 | 单个 tuple 在页内可占用的最大空间                |
| `HNSW_ELEMENT_TUPLE_SIZE(size)`                         | 带对齐的 element tuple 大小        | 计算包含向量数据的 element tuple 所需字节        |
| `HNSW_NEIGHBOR_TUPLE_SIZE(level, m)`                    | 带对齐的 neighbor tuple 大小       | 计算存储给定层数和 M 值的邻居 tuple 字节数       |
| `HNSW_NEIGHBOR_ARRAY_SIZE(lm)`                          | 邻居数组大小                       | 计算内存中 HnswNeighborArray 结构体所需字节      |
| `HnswGetLayerM(m, layer)`                               | `layer==0 ? m*2 : m`               | 第 0 层连接数为 2M，其余层为 M                   |
| `HnswGetMl(m)`                                          | `1 / log(m)`                       | 论文推荐的层级归一化因子                         |
| `HnswGetMaxLevel(m)`                                    | 依页大小计算                       | 在页面容量约束下允许的最大层数（≤255）           |
| `HnswPageGetOpaque(page)`                               | 页 opaque 区指针                   | 获取页面的 HnswPageOpaqueData                    |
| `HnswPageGetMeta(page)`                                 | 元页内容指针                       | 获取元页的 HnswMetaPageData                      |
| `RandomDouble()`                                        | 平台相关随机双精度                 | 跨 PG 版本的随机数生成封装                       |
| `SeedRandom(seed)`                                      | 设置随机种子                       | 跨 PG 版本的随机数种子封装                       |
| `HnswIsElementTuple(tup)`                               | `type == HNSW_ELEMENT_TUPLE_TYPE`  | 判断 tuple 是否为 element 类型                   |
| `HnswIsNeighborTuple(tup)`                              | `type == HNSW_NEIGHBOR_TUPLE_TYPE` | 判断 tuple 是否为 neighbor 类型                  |
| `HnswGetValue(base, element)`                           | Datum 指针                         | 获取元素存储的向量 Datum                         |

**指针宏（HnswPtrDeclare 系列）**

HNSW 中的指针需要同时支持两种模式：构建时的绝对指针（`ptr`）和并行构建时共享内存中的相对偏移（`relptr`）。为此定义了以下联合体类型及配套操作宏：

| 类型名                 | 含义                                   |
| ---------------------- | -------------------------------------- |
| `HnswElementPtr`       | 指向 HnswElementData 的绝对/相对指针   |
| `HnswNeighborArrayPtr` | 指向 HnswNeighborArray 的绝对/相对指针 |
| `HnswNeighborsPtr`     | 指向邻居数组指针列表的绝对/相对指针    |
| `DatumPtr`             | 指向向量值（char）的绝对/相对指针      |

| 宏名                            | 功能                                                 |
| ------------------------------- | ---------------------------------------------------- |
| `HnswPtrAccess(base, hp)`       | 根据 base 是否为 NULL 决定使用绝对指针或相对偏移访问 |
| `HnswPtrStore(base, hp, value)` | 存储指针值（绝对或相对）                             |
| `HnswPtrIsNull(base, hp)`       | 判断指针是否为空                                     |
| `HnswPtrEqual(base, hp1, hp2)`  | 比较两个指针是否相等                                 |
| `HnswPtrPointer(hp)`            | 直接取绝对指针成员 `.ptr`                            |
| `HnswPtrOffset(hp)`             | 直接取相对偏移成员                                   |

> `base == NULL` 时走绝对指针路径（单进程构建），`base != NULL` 时走 `relptr` 路径（共享内存并行构建）。

**枚举类型**

| 枚举名                  | 成员                         | 含义                                     |
| ----------------------- | ---------------------------- | ---------------------------------------- |
| `HnswIterativeScanMode` | `OFF` / `RELAXED` / `STRICT` | 迭代扫描模式：关闭 / 宽松排序 / 严格排序 |

**全局变量**

| 变量名                     | 类型     | 含义                              |
| -------------------------- | -------- | --------------------------------- |
| `hnsw_ef_search`           | `int`    | 查询时动态候选列表大小（GUC）     |
| `hnsw_iterative_scan`      | `int`    | 迭代扫描模式（GUC）               |
| `hnsw_max_scan_tuples`     | `int`    | 迭代扫描最大访问 tuple 数（GUC）  |
| `hnsw_scan_mem_multiplier` | `double` | 迭代扫描内存倍数（GUC）           |
| `hnsw_lock_tranche_id`     | `int`    | LWLock tranche ID，供并行构建使用 |

**结构体汇总**

| 结构体名                | 职责                                                         |
| ----------------------- | ------------------------------------------------------------ |
| `HnswElementData`       | 图中一个节点的完整内存表示，含坐标、邻居、堆 TID、位置信息   |
| `HnswCandidate`         | 搜索/构建过程中的候选节点，记录元素指针、距离、是否为 closer 标志 |
| `HnswNeighborArray`     | 某层某节点的邻居集合，变长数组存储 HnswCandidate             |
| `HnswSearchCandidate`   | 搜索时优先队列节点，内含两个 pairingheap 节点（候选堆 c_node 和已访问堆 w_node） |
| `HnswOptions`           | 索引 reloptions，存储 m 和 efConstruction                    |
| `HnswGraph`             | 图的运行时状态，含入口点、内存分配状态、flush 状态及相关锁   |
| `HnswShared`            | 并行构建时的共享内存区域，含不可变信息、worker 进度、图数据  |
| `HnswLeader`            | 并行构建 leader 进程的本地状态，含并行上下文和共享区指针     |
| `HnswAllocator`         | 抽象内存分配器，通过函数指针支持不同分配策略                 |
| `HnswTypeInfo`          | 类型相关信息：最大维度、归一化函数、值检查函数               |
| `HnswSupport`           | 支持函数句柄集合：距离函数、归一化函数、collation            |
| `HnswQuery`             | 查询向量的封装                                               |
| `HnswBuildState`        | 索引构建全局状态，含所有设置、统计、内存上下文、并行构建信息 |
| `HnswMetaPageData`      | 元页内容：魔数、版本、维度、m、efConstruction、入口点位置、插入页号 |
| `HnswPageOpaqueData`    | 页面 opaque 区：下一页块号、page_id                          |
| `HnswElementTupleData`  | 磁盘上的 element tuple：类型、层、删除标志、版本、堆 TID、邻居 TID、向量数据 |
| `HnswNeighborTupleData` | 磁盘上的 neighbor tuple：类型、版本、连接数、邻居 index TID 数组 |
| `HnswScanOpaqueData`    | 索引扫描私有状态：候选列表、已访问集合、查询向量、迭代扫描内存控制 |
| `HnswVacuumState`       | VACUUM 状态：删除 TID 集合、邻居 tuple 缓冲、最高点记录      |

**关键结构体字段说明**

`HnswElementData` 是图节点的核心，主要字段如下：

| 字段                             | 类型                  | 含义                                       |
| -------------------------------- | --------------------- | ------------------------------------------ |
| `next`                           | `HnswElementPtr`      | 链表指针，串联所有元素（用于构建时遍历）   |
| `heaptids`                       | `ItemPointerData[10]` | 对应堆表行的 TID 数组（支持 non-HOT 更新） |
| `heaptidsLength`                 | `uint8`               | 实际使用的 TID 数量                        |
| `level`                          | `uint8`               | 本节点在图中的最高层级                     |
| `deleted`                        | `uint8`               | 是否已被标记删除                           |
| `version`                        | `uint8`               | tuple 版本，用于并发更新检测               |
| `hash`                           | `uint32`              | 向量哈希值，加速去重                       |
| `neighbors`                      | `HnswNeighborsPtr`    | 各层邻居数组的指针列表                     |
| `blkno` / `offno`                | block/offset          | element tuple 的磁盘位置                   |
| `neighborPage` / `neighborOffno` | block/offset          | neighbor tuple 的磁盘位置                  |
| `value`                          | `DatumPtr`            | 向量数据指针                               |
| `lock`                           | `LWLock`              | 节点级读写锁，用于并发构建                 |

**哈希表类型**

文件末尾通过 PostgreSQL 的 `simplehash.h` 模板声明了三种哈希表，用于搜索时记录已访问节点：

| 哈希表类型    | Key 类型          | 用途                           |
| ------------- | ----------------- | ------------------------------ |
| `tidhash`     | `ItemPointerData` | 按磁盘 TID 去重（磁盘扫描）    |
| `pointerhash` | `uintptr_t`       | 按内存指针去重（内存构建）     |
| `offsethash`  | `Size`            | 按共享内存偏移去重（并行构建） |

三种哈希表统一通过 `visited_hash` 联合体持有，在不同扫描阶段选择对应成员使用。

------

## `src/hnsw.c`

**hnsw.c 是 HNSW 索引的"初始化与查询规划层"**：它负责 LWLock tranche 注册、GUC 参数定义、查询代价估算、reloptions 解析，以及向 PostgreSQL 注册完整的索引访问方法（AM）。

### 模块1：初始化

代表函数：`HnswInitLockTranche`、`HnswInit`

**`HnswInitLockTranche`**

**函数功能**：在共享内存中为 HNSW 并行构建分配一个 LWLock tranche ID，确保多 backend 共享同一 ID。
 **输入/输出**：无参数，返回 void。
 **实现原理**：通过 `ShmemInitStruct` 在共享内存中分配一个 `int`，首次分配时调用 `LWLockNewTrancheId` 申请新 ID，后续 backend 复用同一 ID。PG 19 以前还需调用 `LWLockRegisterTranche` 进行 per-backend 注册。
 **核心代码解读**：

```c
LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
tranche_ids = ShmemInitStruct("hnsw LWLock ids", sizeof(int) * 1, &found);
if (!found)
    tranche_ids[0] = LWLockNewTrancheId(...);
hnsw_lock_tranche_id = tranche_ids[0];
LWLockRelease(AddinShmemInitLock);
```

`found` 为 true 时说明共享内存已由另一 backend 初始化，直接复用即可。

**`HnswInit`**

**函数功能**：注册 HNSW 索引的 reloptions 和所有 GUC 参数。
 **输入/输出**：无参数，返回 void。
 **实现原理**：若非 `shared_preload_libraries` 阶段则直接初始化 lock tranche；随后通过 `add_reloption_kind` 注册 `m` 和 `ef_construction` 两个索引级参数，以及四个会话级 GUC：

| GUC 名称                   | 默认值 | 范围                           | 含义                      |
| -------------------------- | ------ | ------------------------------ | ------------------------- |
| `hnsw.ef_search`           | 40     | 1~1000                         | 查询时候选列表大小        |
| `hnsw.iterative_scan`      | off    | off/relaxed_order/strict_order | 迭代扫描模式              |
| `hnsw.max_scan_tuples`     | 20000  | 1~INT_MAX                      | 迭代扫描最大访问 tuple 数 |
| `hnsw.scan_mem_multiplier` | 1      | 1~1000                         | 迭代扫描内存倍数          |

### 模块2：代价估算

代表函数：`hnswcostestimate`

**`hnswcostestimate`**

**函数功能**：向查询规划器提供 HNSW 索引扫描的代价估算，帮助规划器决定是否使用索引。
 **输入/输出**：输入 `PlannerInfo`、`IndexPath` 等；输出各项代价指标。
 **实现原理**：

首先处理无 ORDER BY 的特殊情况——HNSW 必须依赖向量距离排序，若查询不带 `ORDER BY`，直接将代价设为无穷大，强制规划器放弃该索引路径：

```c
if (path->indexorderbys == NIL) {
    *indexStartupCost = get_float8_infinity();
    *indexTotalCost = get_float8_infinity();
    ...
}
```

对于正常查询，基于以下公式估算实际扫描的 tuple 比例：

```
numIndexTuples = entryLevel * m + layer0TuplesMax * layer0Selectivity
```

各项含义如下：

| 项                  | 公式                                              | 含义                                     |
| ------------------- | ------------------------------------------------- | ---------------------------------------- |
| `entryLevel`        | `log(tuples) * Ml`                                | 从入口层下降到第 0 层经过的层数          |
| `layer0TuplesMax`   | `2M * ef_search`                                  | 第 0 层不去重情况下最多访问的 tuple 数   |
| `layer0Selectivity` | `0.55 * log(N) / (log(M) * (1 + log(ef_search)))` | 第 0 层实际扫描比例，含经验缩放因子 0.55 |

最终 `ratio = numIndexTuples / totalTuples`，用于将全量代价缩放为实际启动代价。此外还对 TOAST 场景做了页面代价修正（随机读转顺序读）。

### 模块3：reloptions 与验证

代表函数：`hnswoptions`、`hnswvalidate`、`hnswbuildphasename`

| 函数                 | 功能                                                         |
| -------------------- | ------------------------------------------------------------ |
| `hnswoptions`        | 解析并验证 `CREATE INDEX ... WITH (m=..., ef_construction=...)` 中的参数，映射到 `HnswOptions` 结构体 |
| `hnswvalidate`       | 验证 operator class catalog 条目合法性，当前恒返回 true      |
| `hnswbuildphasename` | 将构建阶段编号转为可读字符串（`"initializing"` / `"loading tuples"`），供 `pg_stat_progress_create_index` 使用 |

### 模块4：索引访问方法注册

代表函数：`hnswhandler`

**`hnswhandler`**

**函数功能**：PostgreSQL 索引 AM 的入口，返回填充好的 `IndexAmRoutine`，将所有回调函数注册到 PG 内核。
 **输入/输出**：`PG_FUNCTION_ARGS`；返回 `IndexAmRoutine*`（以 Datum 形式）。
 **实现原理**：PG 19 起使用静态结构体初始化（性能更优），低版本使用 `makeNode` 动态分配。函数将各个操作函数指针填入 `IndexAmRoutine`，完成 HNSW 索引与 PG 内核的全部对接。

注册的关键能力标志如下：

| 标志                      | 值                 | 含义                                   |
| ------------------------- | ------------------ | -------------------------------------- |
| `amcanorderbyop`          | true               | 支持按操作符结果排序（距离检索的基础） |
| `amcanbuildparallel`      | true（PG ≥ 17）    | 支持并行建索引                         |
| `amcanparallel`           | false              | 不支持并行索引扫描                     |
| `amcanunique`             | false              | 不支持唯一索引                         |
| `amcanmulticol`           | false              | 不支持多列索引                         |
| `amparallelvacuumoptions` | `PARALLEL_BULKDEL` | 支持并行批量删除 VACUUM                |

注册的回调函数一览：

| 回调               | 对应函数             | 触发时机                  |
| ------------------ | -------------------- | ------------------------- |
| `ambuild`          | `hnswbuild`          | `CREATE INDEX`            |
| `ambuildempty`     | `hnswbuildempty`     | 创建空索引（unlogged 表） |
| `aminsert`         | `hnswinsert`         | `INSERT` 时插入索引项     |
| `ambulkdelete`     | `hnswbulkdelete`     | VACUUM 批量删除           |
| `amvacuumcleanup`  | `hnswvacuumcleanup`  | VACUUM 后清理             |
| `amcostestimate`   | `hnswcostestimate`   | 查询规划代价估算          |
| `amoptions`        | `hnswoptions`        | 解析 reloptions           |
| `ambuildphasename` | `hnswbuildphasename` | 构建进度阶段名称          |
| `amvalidate`       | `hnswvalidate`       | 验证 opclass              |
| `ambeginscan`      | `hnswbeginscan`      | 开始索引扫描              |
| `amrescan`         | `hnswrescan`         | 重置扫描                  |
| `amgettuple`       | `hnswgettuple`       | 逐条返回扫描结果          |
| `amendscan`        | `hnswendscan`        | 结束扫描，释放资源        |

# DAY 6

## 任务要求

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

## `src/hnswbuild.c`

**hnswbuild.c 是 HNSW 索引的"构建实施层"**：它实现了将基础表（Heap）数据转换为 HNSW 导航图的核心逻辑，支持单进程内存构建、多进程并行构建以及索引页的磁盘持久化。

### 模块1：构建初始化与调度

代表函数：`hnswbuild`（逻辑入口）、`InitBuildState`

**`InitBuildState`**

**函数功能**：初始化构建状态结构体 `HnswBuildState`，解析 HNSW 关键超参数。

**实现原理**：从索引元数据中提取 `m`（层间连接数）和 `ef_construction`（构建动态列表大小），并计算 `ml`（层级衰减因子）和 `maxLevel`。同时创建一个专用的 `graphCtx` 内存上下文，用于管理内存阶段的图节点。

**核心代码解读**：

```c++
buildstate->m = HnswGetM(index);
buildstate->efConstruction = HnswGetEfConstruction(index);
buildstate->ml = HnswGetMl(buildstate->m);
buildstate->maxLevel = HnswGetMaxLevel(buildstate->m);
// 初始化内存构建所需的临时上下文
buildstate->graphCtx = GenerationContextCreate(...);
```

### 模块2：内存索引构建

代表函数：`InsertTupleInMemory`、`BuildCallback`

**`InsertTupleInMemory`**

**函数功能**：将单个向量插入到内存中的 HNSW 图。

**输入/输出**：输入待插入的 `HnswElement`，无返回值。

**实现原理**：

1. **入口点竞争**：通过 `entryLock` 获取当前图的入口点。如果新元素的层级高于现有入口点，则需升级为排他锁以更新全局入口。
2. **邻居搜索**：调用 `HnswFindElementNeighbors` 在各层寻找最近邻。
3. **双向连接**：在当前层及以下建立连接，并调用 `UpdateNeighborsInMemory` 更新邻居的反向指针，必要时触发连接缩减（Shrink）。

**`BuildCallback`**

**函数功能**：表扫描的回调函数，负责处理每一行抓取到的元组。

**实现原理**：跳过 NULL 值，随后调用 `InsertTuple`。如果 `maintenance_work_mem` 充足，数据将留在内存中；一旦内存触顶，则调用 `FlushPages` 将图强制刷入磁盘，后续构建转为磁盘模式。

------

### 模块3：磁盘持久化（落盘）

代表函数：`FlushPages`、`CreateGraphPages`、`WriteNeighborTuples`

**磁盘布局结构**：

HNSW 索引在磁盘上由三部分组成：**Metapage**（元数据）、**Element Tuple**（存储向量数据与基础信息）和 **Neighbor Tuple**（存储各层邻居指针）。

**`CreateGraphPages`**

**函数功能**：将内存中的 HNSW 节点序列化为 PostgreSQL 的标准 Page。

**实现原理**：遍历内存图链表，为每个元素分配块号（BlockNumber）和偏移量（OffsetNumber）。为了优化 I/O，程序尽可能将“元素元组”及其对应的“邻居元组”放在同一个 Page 内。

**`WriteNeighborTuples`**

**函数功能**：二次遍历，填充邻居关系。

**实现原理**：由于在初次写入 Page 时，某些邻居节点可能尚未确定磁盘位置，因此需要在所有节点位置确定后，进行第二次全量扫描，根据内存中的指针更新磁盘上的 `ntup`（邻居元组）数据。

------

### 模块4：并行构建

代表函数：`HnswParallelScanAndInsert`、`HnswParallelBuildMain`

**并行架构**：

HNSW 支持利用 PostgreSQL 的并行工作进程（Parallel Workers）加速构建。

| **组件名称**    | **职责**                                                     |
| --------------- | ------------------------------------------------------------ |
| **Leader 进程** | 初始化共享内存（DSM），启动 Worker，最后负责将共享图写入磁盘。 |
| **Worker 进程** | 扫描 Heap 的不同数据分区，竞争性地向共享内存中的图结构插入节点。 |
| **共享内存区**  | 存放 `HnswShared` 状态及节点数据，通过相对指针（Relative Pointers）解决地址映射偏移。 |

**并发控制**：

并行构建时，使用 `LWLock`（轻量级锁）保护共享资源：

- **`allocatorLock`**：保护共享内存分配器，防止多个 Worker 同时申请空间。
- **节点锁**：每个 `HnswElement` 拥有独立的锁，用于在更新其邻居列表时保持原子性。

### 代码

```c++
#include "postgres.h"
/* ... 包含头文件省略 ... */

// 元页初始化：记录HNSW版本、维度、M、ef等核心元数据
static void
CreateMetaPage(HnswBuildState * buildstate)
{
    Relation   index = buildstate->index;
    ForkNumber forkNum = buildstate->forkNum;
    Buffer    buf;
    Page      page;
    HnswMetaPage metap;

    buf = HnswNewBuffer(index, forkNum);
    page = BufferGetPage(buf);
    HnswInitPage(buf, page);

    metap = HnswPageGetMeta(page);
    metap->magicNumber = HNSW_MAGIC_NUMBER;
    metap->version = HNSW_VERSION;
    metap->dimensions = buildstate->dimensions;
    metap->m = buildstate->m;
    metap->efConstruction = buildstate->efConstruction;
    metap->entryBlkno = InvalidBlockNumber;
    metap->entryOffno = InvalidOffsetNumber;
    metap->entryLevel = -1;
    metap->insertPage = InvalidBlockNumber;
    ((PageHeader) page)->pd_lower = ((char *) metap + sizeof(HnswMetaPageData)) - (char *) page;

    MarkBufferDirty(buf);
    UnlockReleaseBuffer(buf);
}

// 追加新索引页：分配新Buffer并维护页间双向链表关系
static void
HnswBuildAppendPage(Relation index, Buffer *buf, Page *page, ForkNumber forkNum)
{
    Buffer    newbuf = HnswNewBuffer(index, forkNum);
    HnswPageGetOpaque(*page)->nextblkno = BufferGetBlockNumber(newbuf);
    MarkBufferDirty(*buf);
    UnlockReleaseBuffer(*buf);
    LockBuffer(newbuf, BUFFER_LOCK_UNLOCK);
    CHECK_FOR_INTERRUPTS(); // 允许中断以避免长时间锁定
    LockBuffer(newbuf, BUFFER_LOCK_EXCLUSIVE);
    *buf = newbuf;
    *page = BufferGetPage(*buf);
    HnswInitPage(*buf, *page);
}

// 构建磁盘图结构：遍历内存图节点，计算位置并写入Element和Neighbor占位符
static void
CreateGraphPages(HnswBuildState * buildstate)
{
    Relation   index = buildstate->index;
    ForkNumber forkNum = buildstate->forkNum;
    Size      maxSize = HNSW_MAX_SIZE;
    HnswElementTuple etup = palloc0(HNSW_TUPLE_ALLOC_SIZE);
    HnswNeighborTuple ntup = palloc0(HNSW_TUPLE_ALLOC_SIZE);
    Buffer    buf;
    Page      page;
    HnswElementPtr iter = buildstate->graph->head;
    char      *base = buildstate->hnswarea;

    buf = HnswNewBuffer(index, forkNum);
    page = BufferGetPage(buf);
    HnswInitPage(buf, page);

    while (!HnswPtrIsNull(base, iter))
    {
       HnswElement element = HnswPtrAccess(base, iter);
       Size etupSize, ntupSize, combinedSize;
       Pointer valuePtr = HnswPtrAccess(base, element->value);

       iter = element->next;
       MemSet(etup, 0, HNSW_TUPLE_ALLOC_SIZE);
       etupSize = HNSW_ELEMENT_TUPLE_SIZE(VARSIZE_ANY(valuePtr));
       ntupSize = HNSW_NEIGHBOR_TUPLE_SIZE(element->level, buildstate->m);
       combinedSize = etupSize + ntupSize + sizeof(ItemIdData);

       HnswSetElementTuple(base, etup, element);
       // 空间不足或无法容纳紧凑对时换页
       if (PageGetFreeSpace(page) < etupSize || (combinedSize <= maxSize && PageGetFreeSpace(page) < combinedSize))
          HnswBuildAppendPage(index, &buf, &page, forkNum);

       element->blkno = BufferGetBlockNumber(buf);
       element->offno = OffsetNumberNext(PageGetMaxOffsetNumber(page));
       // 判断邻居元组是跟在后面还是位于下一页
       if (combinedSize <= maxSize)
       {
          element->neighborPage = element->blkno;
          element->neighborOffno = OffsetNumberNext(element->offno);
       }
       else
       {
          element->neighborPage = element->blkno + 1;
          element->neighborOffno = FirstOffsetNumber;
       }

       ItemPointerSet(&etup->neighbortid, element->neighborPage, element->neighborOffno);
       if (PageAddItem(page, (Item) etup, etupSize, InvalidOffsetNumber, false, false) != element->offno)
          elog(ERROR, "failed to add index item");

       if (PageGetFreeSpace(page) < ntupSize)
          HnswBuildAppendPage(index, &buf, &page, forkNum);

       if (PageAddItem(page, (Item) ntup, ntupSize, InvalidOffsetNumber, false, false) != element->neighborOffno)
          elog(ERROR, "failed to add neighbor item");
    }

    BlockNumber insertPage = BufferGetBlockNumber(buf);
    MarkBufferDirty(buf);
    UnlockReleaseBuffer(buf);

    HnswElement entryPoint = HnswPtrAccess(base, buildstate->graph->entryPoint);
    HnswUpdateMetaPage(index, HNSW_UPDATE_ENTRY_ALWAYS, entryPoint, insertPage, forkNum, true);
    pfree(etup); pfree(ntup);
}

// 写入邻居连接详情：节点位置固定后，二次遍历填充实际的邻居指针列表
static void
WriteNeighborTuples(HnswBuildState * buildstate)
{
    Relation   index = buildstate->index;
    ForkNumber forkNum = buildstate->forkNum;
    HnswElementPtr iter = buildstate->graph->head;
    char      *base = buildstate->hnswarea;
    HnswNeighborTuple ntup = palloc0(HNSW_TUPLE_ALLOC_SIZE);

    while (!HnswPtrIsNull(base, iter))
    {
       HnswElement element = HnswPtrAccess(base, iter);
       Buffer buf; Page page;
       Size ntupSize = HNSW_NEIGHBOR_TUPLE_SIZE(element->level, buildstate->m);

       iter = element->next;
       MemSet(ntup, 0, HNSW_TUPLE_ALLOC_SIZE);
       CHECK_FOR_INTERRUPTS();

       buf = ReadBufferExtended(index, forkNum, element->neighborPage, RBM_NORMAL, NULL);
       LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
       page = BufferGetPage(buf);

       HnswSetNeighborTuple(base, ntup, element, buildstate->m);
       if (!PageIndexTupleOverwrite(page, element->neighborOffno, (Item) ntup, ntupSize))
          elog(ERROR, "failed to overwrite neighbor tuple");

       MarkBufferDirty(buf);
       UnlockReleaseBuffer(buf);
    }
    pfree(ntup);
}

// 刷盘操作：按序创建元页、节点页和邻居页，随后释放内存上下文
static void
FlushPages(HnswBuildState * buildstate)
{
    CreateMetaPage(buildstate);
    CreateGraphPages(buildstate);
    WriteNeighborTuples(buildstate);
    buildstate->graph->flushed = true;
    MemoryContextReset(buildstate->graphCtx);
}

// 处理重复值：若邻居中存在相同向量且有空间，则直接将TID存入现有节点
static bool
AddDuplicateInMemory(HnswElement element, HnswElement dup)
{
    LWLockAcquire(&dup->lock, LW_EXCLUSIVE);
    if (dup->heaptidsLength == HNSW_HEAPTIDS)
    {
       LWLockRelease(&dup->lock);
       return false;
    }
    HnswAddHeapTid(dup, &element->heaptids[0]);
    LWLockRelease(&dup->lock);
    return true;
}

// 内存中查重：按距离排序遍历邻居，寻找向量完全一致的节点
static bool
FindDuplicateInMemory(char *base, HnswElement element)
{
    HnswNeighborArray *neighbors = HnswGetNeighbors(base, element, 0);
    Datum value = HnswGetValue(base, element);

    for (int i = 0; i < neighbors->length; i++)
    {
       HnswElement neighborElement = HnswPtrAccess(base, neighbors->items[i].element);
       if (!datumIsEqual(value, HnswGetValue(base, neighborElement), false, -1))
          return false;
       if (AddDuplicateInMemory(element, neighborElement))
          return true;
    }
    return false;
}

// 注册新节点：加锁后将元素挂载到内存图的全局链表头部
static void
AddElementInMemory(char *base, HnswGraph * graph, HnswElement element)
{
    SpinLockAcquire(&graph->lock);
    element->next = graph->head;
    HnswPtrStore(base, graph->head, element);
    SpinLockRelease(&graph->lock);
}

// 建立双向连接：更新新节点的邻居，同时反向修改邻居的连接列表
static void
UpdateNeighborsInMemory(char *base, HnswSupport * support, HnswElement e, int m)
{
    for (int lc = e->level; lc >= 0; lc--)
    {
       int lm = HnswGetLayerM(m, lc);
       Size neighborsSize = HNSW_NEIGHBOR_ARRAY_SIZE(lm);
       HnswNeighborArray *neighbors = palloc(neighborsSize);

       LWLockAcquire(&e->lock, LW_SHARED);
       memcpy(neighbors, HnswGetNeighbors(base, e, lc), neighborsSize);
       LWLockRelease(&e->lock);

       for (int i = 0; i < neighbors->length; i++)
       {
          HnswElement neighborElement = HnswPtrAccess(base, neighbors->items[i].element);
          LWLockAcquire(&neighborElement->lock, LW_EXCLUSIVE);
          HnswUpdateConnection(base, HnswGetNeighbors(base, neighborElement, lc), e, neighbors->items[i].distance, lm, NULL, NULL, support);
          LWLockRelease(&neighborElement->lock);
       }
    }
}

// 维护内存图状态：处理查重、链表挂载及连接更新
static void
UpdateGraphInMemory(HnswSupport * support, HnswElement element, int m, HnswElement entryPoint, HnswBuildState * buildstate)
{
    char *base = buildstate->hnswarea;
    if (FindDuplicateInMemory(base, element)) return;
    AddElementInMemory(base, buildstate->graph, element);
    UpdateNeighborsInMemory(base, support, element, m);
    if (entryPoint == NULL || element->level > entryPoint->level)
       HnswPtrStore(base, buildstate->graph->entryPoint, element);
}

// 内存插入主逻辑：包含入口点竞争处理和邻居搜索
static void
InsertTupleInMemory(HnswBuildState * buildstate, HnswElement element)
{
    HnswGraph *graph = buildstate->graph;
    char *base = buildstate->hnswarea;
    LWLockAcquire(&graph->entryWaitLock, LW_EXCLUSIVE); LWLockRelease(&graph->entryWaitLock);
    LWLockAcquire(&graph->entryLock, LW_SHARED);
    HnswElement entryPoint = HnswPtrAccess(base, graph->entryPoint);

    // 如果新节点层级更高，需升级排他锁以安全更新全局入口点
    if (entryPoint == NULL || element->level > entryPoint->level)
    {
       LWLockRelease(&graph->entryLock);
       LWLockAcquire(&graph->entryWaitLock, LW_EXCLUSIVE);
       LWLockAcquire(&graph->entryLock, LW_EXCLUSIVE);
       LWLockRelease(&graph->entryWaitLock);
       entryPoint = HnswPtrAccess(base, graph->entryPoint);
    }

    HnswFindElementNeighbors(base, element, entryPoint, NULL, &buildstate->support, buildstate->m, buildstate->efConstruction, false);
    UpdateGraphInMemory(&buildstate->support, element, buildstate->m, entryPoint, buildstate);
    LWLockRelease(&graph->entryLock);
}

// 插入元组入口：根据内存状态决定是插入内存图、落盘后插入还是直接走磁盘逻辑
static bool
InsertTuple(Relation index, Datum *values, bool *isnull, ItemPointer heaptid, HnswBuildState * buildstate)
{
    HnswGraph  *graph = buildstate->graph;
    Datum value;
    if (!HnswFormIndexValue(&value, values, isnull, buildstate->typeInfo, &buildstate->support)) return false;

    LWLockAcquire(&graph->flushLock, LW_SHARED);
    if (graph->flushed) // 已转为磁盘模式
    {
       LWLockRelease(&graph->flushLock);
       return HnswInsertTupleOnDisk(index, &buildstate->support, value, heaptid, true);
    }

    LWLockAcquire(&graph->allocatorLock, LW_EXCLUSIVE);
    // 内存检查：若超出work_mem则触发强制落盘并切换模式
    if (graph->memoryUsed + (buildstate->hnswarea ? 1024*1024 : 0) >= graph->memoryTotal)
    {
       LWLockRelease(&graph->allocatorLock); LWLockRelease(&graph->flushLock);
       LWLockAcquire(&graph->flushLock, LW_EXCLUSIVE);
       if (!graph->flushed) FlushPages(buildstate);
       LWLockRelease(&graph->flushLock);
       return HnswInsertTupleOnDisk(index, &buildstate->support, value, heaptid, true);
    }

    HnswElement element = HnswInitElement(buildstate->hnswarea, heaptid, buildstate->m, buildstate->ml, buildstate->maxLevel, &buildstate->allocator);
    Size valueSize = VARSIZE_ANY(DatumGetPointer(value));
    Pointer valuePtr = HnswAlloc(&buildstate->allocator, valueSize);
    LWLockRelease(&graph->allocatorLock);

    memcpy(valuePtr, DatumGetPointer(value), valueSize);
    HnswPtrStore(buildstate->hnswarea, element->value, (char *) valuePtr);
    LWLockInitialize(&element->lock, hnsw_lock_tranche_id);
    InsertTupleInMemory(buildstate, element);
    LWLockRelease(&graph->flushLock);
    return true;
}

// 扫描回调：PG TableScan的核心接口，对每个有效元组调用InsertTuple
static void
BuildCallback(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool tupleIsAlive, void *state)
{
    HnswBuildState *buildstate = (HnswBuildState *) state;
    if (isnull[0]) return;
    MemoryContext oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);
    if (InsertTuple(index, values, isnull, tid, buildstate))
    {
       SpinLockAcquire(&buildstate->graph->lock);
       pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, ++buildstate->graph->indtuples);
       SpinLockRelease(&buildstate->graph->lock);
    }
    MemoryContextSwitchTo(oldCtx);
    MemoryContextReset(buildstate->tmpCtx);
}
```





## `src/hnswutils.c`

**hnswutils.c 是 HNSW 索引的"底层工具与算法实现层"**：它封装了 HNSW 论文中的核心算法（如启发式邻居选择、多层图搜索），并提供了跨内存与磁盘的一致性操作接口，是整个索引插件的基石。

### 模块1：多层图搜索 (Algorithm 2)

代表函数：`HnswSearchLayer`、`HnswEntryCandidate`

**`HnswSearchLayer`**

**函数功能**：在 HNSW 图的特定层级执行贪心搜索，找到距离查询向量最近的候选集。

**实现原理**：

1. **优先队列管理**：使用 `C`（工作队列，按距离升序）和 `W`（结果队列，按距离降序）维护搜索状态。
2. **贪心遍历**：不断从 `C` 中提取最近节点，遍历其邻居。若邻居距离小于 `W` 中的最远节点，则将其同时加入 `C` 和 `W`。
3. **收敛条件**：当 `C` 中最近节点的距离已大于 `W` 中最远节点的距离时，说明无法进一步优化，搜索停止。
4. **抽象加载**：通过 `inMemory` 标志位无缝切换“直接内存访问”与“Buffer管理器磁盘读取”。

### 模块2：邻居选择启发式 (Algorithm 4)

代表函数：`SelectNeighbors`、`CheckElementCloser`

**`SelectNeighbors`**

**函数功能**：从候选集中筛选 $M$ 个邻居建立连接，不仅考虑距离，更考虑图的连通性与覆盖范围。

**实现原理**：

1. **多样性剪枝**：遵循“若候选节点 $e$ 距离查询点，比已选邻居集 $R$ 中的任何点都近，才加入 $R$”的原则。
2. **启发式效果**：这使得算法倾向于选择分布在不同方向的邻居（类似于聚类中心），而非仅仅挤在最近的局部区域，从而显著提升搜索效率。
3. **状态缓存**：通过 `closerSet` 标志位缓存计算结果，优化并行构建时的重复计算开销。

------

### 模块3：对象初始化与内存管理

代表函数：`HnswInitElement`、`HnswAlloc`、`HnswInitNeighbors`

**`HnswInitElement`**

**函数功能**：初始化一个新的 HNSW 图节点。

**实现原理**：

1. **随机层级产生**：基于指数分布 $L = \lfloor -\ln(uniform(0,1)) \cdot ml \rfloor$ 确定节点最高层级。
2. **内存分配**：通过 `HnswAlloc` 适配器分配空间。在并行构建时，这指向共享内存；在普通构建时，指向进程私有内存。
3. **结构构造**：初始化 TID 列表、层级信息，并调用 `HnswInitNeighbors` 为该节点在每一层预留邻居指针数组。

------

### 模块4：磁盘与元数据操作

代表函数：`HnswGetMetaPageInfo`、`HnswUpdateMetaPage`、`HnswLoadElement`

**`HnswLoadElement`**

**函数功能**：根据磁盘地址（Block/Offset）将向量数据和节点元数据加载到内存。

**实现原理**：调用 PG 的 `ReadBuffer` 读取索引页，锁定后将 `HnswElementTuple` 数据拷贝至临时结构，计算与查询向量的距离后释放 Buffer。

**`HnswUpdateMetaPage`**

**函数功能**：原子性地更新索引元页（如修改入口点）。

**实现原理**：支持 `GenericXLog` 机制。在非构建阶段，通过 WAL（预写日志）确保元页更新的 crash-safety；在构建阶段则直接标记 Buffer 为 Dirty 以追求极致性能。

------

### 代码

```c++
#include "hnsw.h"
/* ... 基础哈希与辅助函数省略 ... */

/* * 索引参数获取：从 reloptions 中提取 M 和 ef_construction
 * M: 节点最大连接数；ef: 构建时动态候选集大小
 */
int HnswGetM(Relation index) {
    HnswOptions *opts = (HnswOptions *) index->rd_options;
    return opts ? opts->m : HNSW_DEFAULT_M;
}

/* * 初始化支持函数：绑定向量距离计算函数（如L2、Cosine）及其 Collation
 */
void HnswInitSupport(HnswSupport * support, Relation index) {
    support->procinfo = index_getprocinfo(index, 1, HNSW_DISTANCE_PROC);
    support->collation = index->rd_indcollation[0];
    support->normprocinfo = HnswOptionalProcInfo(index, HNSW_NORM_PROC);
}

/* * 节点初始化：根据指数概率分布随机决定节点最高层级 level
 * level = floor(-ln(uniform) * ml)，概率随层级升高递减
 */
HnswElement HnswInitElement(char *base, ItemPointer heaptid, int m, double ml, int maxLevel, HnswAllocator * allocator) {
    HnswElement element = HnswAlloc(allocator, sizeof(HnswElementData));
    int level = (int) (-log(RandomDouble()) * ml);
    if (level > maxLevel) level = maxLevel;

    element->level = level;
    element->heaptidsLength = 0;
    HnswAddHeapTid(element, heaptid);
    HnswInitNeighbors(base, element, m, allocator); // 预分配各层邻居数组
    return element;
}

/* * 获取/更新元数据页：
 * 包含 MagicNumber 校验、入口点(Entry Point)位置及层级
 * 非构建阶段更新需使用 GenericXLog 注册 WAL 以保证崩溃恢复
 */
void HnswGetMetaPageInfo(Relation index, int *m, HnswElement * entryPoint) {
    Buffer buf = ReadBuffer(index, HNSW_METAPAGE_BLKNO);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    HnswMetaPage metap = HnswPageGetMeta(BufferGetPage(buf));
    if (metap->magicNumber != HNSW_MAGIC_NUMBER) elog(ERROR, "invalid hnsw index");
    if (m) *m = metap->m;
    if (entryPoint && BlockNumberIsValid(metap->entryBlkno)) {
        *entryPoint = HnswInitElementFromBlock(metap->entryBlkno, metap->entryOffno);
        (*entryPoint)->level = metap->entryLevel;
    }
    UnlockReleaseBuffer(buf);
}

/* * HnswSearchLayer: 核心算法之单层贪心搜索
 * 1. 使用 pairingheap 维护两个队列：候选集 C (近处优先) 和 结果集 W (远处优先)
 * 2. 每次从 C 中取出最亲近的节点，并遍历其在当前层 lc 的所有邻居
 * 3. 距离剪枝：若邻居距离 > W 中最远节点的距离且结果集已满，则跳过
 * 4. 插入 discarded 堆：为迭代扫描模式预留被丢弃的候选节点
 */
List * HnswSearchLayer(char *base, HnswQuery * q, List *ep, int ef, int lc, Relation index, HnswSupport * support, int m, bool inserting, HnswElement skipElement, visited_hash * v, pairingheap **discarded, bool initVisited, int64 *tuples) {
    // ... 初始化 visited 集合，防止搜索回路 ...
    while (!pairingheap_is_empty(C)) {
        HnswSearchCandidate *c = HnswGetSearchCandidate(c_node, pairingheap_remove_first(C));
        HnswSearchCandidate *f = HnswGetSearchCandidate(w_node, pairingheap_first(W));
        if (c->distance > f->distance) break; // 局部最优解已找到，提前退出

        // 根据存储介质加载邻居：内存指针 vs 磁盘TID
        if (inMemory) HnswLoadUnvisitedFromMemory(...);
        else HnswLoadUnvisitedFromDisk(...);

        for (int i = 0; i < unvisitedLength; i++) {
            // 计算距离并维护动态 ef 大小的结果集 W
            if (eDistance < f->distance || wlen < ef) {
                // ... 将更近的节点加入 C 继续探索，加入 W 记录结果 ...
            }
        }
    }
    return w; // 返回按距离排序的结果列表
}

/* * SelectNeighbors: 核心算法之启发式邻居选择
 * 目的：不只选最近的，还要选“方向”最分散的，提升图的导航性
 * 逻辑：对于新节点 e，如果 e 距离目标比 R 中已存在的任何邻居更近，则保留 e
 */
static List * SelectNeighbors(char *base, List *c, int lm, HnswSupport * support, bool *closerSet, HnswCandidate * newCandidate, HnswCandidate **pruned, bool sortCandidates) {
    // 候选集 w 按距离降序排列
    while (list_length(w) > 0 && list_length(r) < lm) {
        HnswCandidate *e = llast(w); w = list_delete_last(w);
        // 启发式校验：检查新候选是否在现有邻居的“阴影区”内
        e->closer = CheckElementCloser(base, e, r, support);
        if (e->closer) r = lappend(r, e);
        else wd[wdlen++] = e; // 被剪枝的节点存入回收站
    }
    // 若启发式结果不足，从回收站捡回距离较近的节点直到补满 lm
    while (wdoff < wdlen && list_length(r) < lm) r = lappend(r, wd[wdoff++]);
    return r;
}
```





# DAY 7

## 任务要求

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

## `src/hnswinsert.c`

**hnswinsert.c 是 HNSW 索引的 "单行插入实现层"**：它负责处理 `INSERT/UPDATE` 操作时的向量索引写入，实现了带严格并发控制的 HNSW 图动态插入逻辑，是索引数据写入的核心模块。

### 模块 1：核心插入流程

**整体插入流程**：

1. 从输入值构造向量 Datum（处理 NULL、归一化等）

2. 在索引上加 `HNSW_UPDATE_LOCK` 写锁，防止并发修改图结构

3. 调用 `HnswInsertTupleOnDisk`完成磁盘写入：

   a. 获取当前索引入口点

   b. 随机生成新元素的层级（由 `HnswGetMl` 控制）

   c. 逐层搜索最近邻节点

   d. 写入元素元组和邻居元组到磁盘页

   e. 更新双向邻居连接（必要时执行边修剪）

   f. 若新元素层级更高，更新元数据入口点

4. 释放锁，完成插入

**并发安全性**：

每个元素持有独立 `LWLock`，修改邻居关系时必须同时持有**元素锁**和**索引锁**，保证图结构修改的原子性。

------

### 模块 2：顶层插入入口

代表函数：`hnswinsert`、`HnswInsertTuple`、`HnswInsertTupleOnDisk`

**`hnswinsert`**

**函数功能**：PostgreSQL 索引访问方法的标准插入回调，内核在 `INSERT/UPDATE` 时自动调用。

**输入 / 输出**：

- 输入：索引关系、列值、NULL 标记、heap TID、唯一性检查标志等

- 输出：bool（HNSW 不支持唯一索引，始终返回 false）

  **实现原理：**

1. 跳过 NULL 向量，不建立索引
2. 创建临时内存上下文，避免内存泄漏
3. 调用内部函数 `HnswInsertTuple` 执行实际插入
4. PG 14+ 支持 `indexUnchanged`，HOT 更新时可跳过索引写入

**`HnswInsertTuple`**

**函数功能**：插入准备层，格式化索引向量并调用磁盘写入函数。

**输入 / 输出**：索引关系、值数组、NULL 标记、heap 物理地址；无返回值。

**实现原理**：

初始化向量支持上下文，调用 `HnswFormIndexValue` 处理向量（归一化等），最终调用 `HnswInsertTupleOnDisk` 写入索引。

**`HnswInsertTupleOnDisk`**

**函数功能**：磁盘插入核心函数，统筹加锁、邻居搜索、图更新全流程。

**输入 / 输出**：索引关系、支持上下文、向量 Datum、heap TID、是否构建期；返回插入成功标志。

**实现原理**：

1. 加 `HNSW_UPDATE_LOCK` 共享锁，允许并发查询、阻塞并发修改
2. 从元数据获取索引参数 `m` 和当前入口点
3. 初始化新元素，随机生成层级
4. 若新元素层级高于入口点，升级为排他锁，避免竞争
5. 调用 `HnswFindElementNeighbors` 搜索各层最近邻
6. 调用 `UpdateGraphOnDisk` 写入磁盘并更新图结构
7. 释放锁

核心代码片段：

```c++
LockPage(index, HNSW_UPDATE_LOCK, lockmode);
HnswGetMetaPageInfo(index, &m, &entryPoint);
element = HnswInitElement(...);
// 搜索邻居
HnswFindElementNeighbors(...);
// 更新磁盘图结构
UpdateGraphOnDisk(...);
UnlockPage(index, HNSW_UPDATE_LOCK, lockmode);
```

------

### 模块 3：磁盘页管理

代表函数：`GetInsertPage`、`HnswFreeOffset`、`HnswInsertAppendPage`、`AddElementOnDisk`

**`GetInsertPage`**

**函数功能**：从元数据页读取当前插入页块号。

**输入 / 输出**：索引关系；返回插入页 BlockNumber。

**实现原理**：读取元数据页，获取 `metap->insertPage` 并返回。

**`HnswFreeOffset`**

**函数功能**：遍历页面，查找已删除元素的空闲空间，实现空间复用。

**输入 / 输出**：索引、缓冲区、页面、元素、元组大小等；返回是否找到空闲空间。

**实现原理**：

遍历页面上的元素元组，跳过邻居元组，检查 `deleted` 标记；

验证空闲空间是否足够存放新元素 + 邻居元组，若足够则返回空闲偏移量。

**`HnswInsertAppendPage`**

**函数功能**：索引空间不足时，扩展新的磁盘页。

**输入 / 输出**：索引、缓冲区、页面、WAL 状态、当前页、是否构建期；无返回值。

**实现原理**：

1. 加关系扩展锁，调用 `HnswNewBuffer` 创建新页
2. 初始化新页为 HNSW 标准页
3. 更新前一页的 `nextblkno` 指针，形成页链表

**`AddElementOnDisk`**

**函数功能**：将新元素和邻居数据写入磁盘页，支持空间复用与页扩展。

**输入 / 输出**：索引、元素、参数 m、插入页、更新后插入页、是否构建期；无返回值。

**实现原理**：

1. 计算元素元组、邻居元组大小
2. 循环查找可用页面：
   - 优先使用单页存放两个元组
   - 其次复用已删除元素空间
   - 最后扩展新页
3. 写入 / 覆盖元组，更新元素的物理地址（blkno/offno）
4. 提交 WAL 日志（非构建期），释放缓冲区

------

### 模块 4：邻居关系管理

代表函数：`HnswLoadNeighbors`、`GetUpdateIndex`、`UpdateNeighborOnDisk`、`HnswUpdateNeighborsOnDisk`

**`HnswLoadNeighbors`**

**函数功能**：从磁盘加载指定元素的邻居列表。

**输入 / 输出**：元素、索引、参数 m、层级最大 / 最小值；返回邻居数组。

**实现原理**：读取邻居元组中的 ItemPointer，批量加载邻居元素。

**`GetUpdateIndex`**

**函数功能**：计算新元素应插入到邻居列表的位置，支持边修剪。

**输入 / 输出**：原元素、新元素、距离、参数 m 等；返回插入下标。

**实现原理**：

1. 加载最新邻居列表
2. 若邻居未满，直接返回空闲位置
3. 否则执行距离排序，替换最远节点
4. 使用独立内存上下文提升大向量性能

**`UpdateNeighborOnDisk`**

**函数功能**：磁盘层面更新单个元素的邻居指针，建立双向连接。

**输入 / 输出**：原元素、新元素、插入下标、参数 m 等；无返回值。

**实现原理**：

1. 加锁读取邻居页
2. 计算层级对应的邻居起始下标
3. 检查连接是否已存在，避免重复
4. 更新磁盘上的邻居 ItemPointer
5. 提交 WAL 并释放锁

**`HnswUpdateNeighborsOnDisk`**

**函数功能**：遍历所有层级，批量更新新元素与邻居的双向连接。

**输入 / 输出**：索引、支持上下文、新元素、参数 m 等；无返回值。

**实现原理**：

从顶层到 0 层逐层处理，对每个邻居调用 `GetUpdateIndex` 和 `UpdateNeighborOnDisk`，完成全图双向连接更新。

------

### 模块 5：重复值与图更新

代表函数：`FindDuplicateOnDisk`、`AddDuplicateOnDisk`、`UpdateGraphOnDisk`

**`FindDuplicateOnDisk`**

**函数功能**：搜索是否存在完全相同的向量，避免重复存储。

**输入 / 输出**：索引、元素、是否构建期；返回是否找到重复值。

**实现原理**：

遍历 0 层邻居，对比向量数据；找到重复向量则调用 `AddDuplicateOnDisk`。

**`AddDuplicateOnDisk`**

**函数功能**：将重复行的 heap TID 添加到已有元素，实现多对一映射。

**输入 / 输出**：索引、原元素、重复元素、是否构建期；返回添加成功标志。

**实现原理**：

加锁读取元素页，找到空闲的 heap TID 槽位写入，避免重复索引。

**`UpdateGraphOnDisk`**

**函数功能**：插入顶层调度函数，处理重复检测、磁盘写入、元数据更新。

**输入 / 输出**：索引、支持上下文、元素、参数 m、入口点、是否构建期；无返回值。

**实现原理**：

1. 优先处理重复向量
2. 调用 `AddElementOnDisk` 写入磁盘
3. 更新插入页元数据
4. 调用 `HnswUpdateNeighborsOnDisk` 建立双向连接
5. 若新元素层级更高，更新索引入口点

### 总结

1. **核心定位**：HNSW 索引**单行写入引擎**，支撑 `INSERT/UPDATE` 实时索引更新
2. **关键能力**：支持**并发安全插入**、**磁盘空间复用**、**双向邻居连接维护**、**重复向量合并**
3. 架构分层：
   - 顶层：内核接口 `hnswinsert`
   - 中层：插入调度 `HnswInsertTupleOnDisk`
   - 底层：磁盘页操作 + 邻居关系维护
4. **可靠性**：基于 PostgreSQL 缓冲区、锁机制、WAL 日志实现事务安全，支持崩溃恢复



# DAY 8

## 任务要求

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

## `src/hnswscan.c`

**hnswscan.c 是 HNSW 索引的 "查询扫描实现层"**：它实现了 HNSW 索引的 KNN（K 最近邻）查询核心逻辑，提供 PostgreSQL 标准索引扫描接口，支持高效向量相似性检索与迭代式扫描增强。

### 模块 1：扫描核心流程

**标准 HNSW 查询算法**：

1. 从索引元数据获取顶层入口点
2. 从最高层开始，使用贪心搜索（ef=1）逐层向下逼近目标向量
3. 在第 0 层执行精细化搜索，获取 `ef_search` 个候选结果
4. 按距离升序返回结果，每次 `hnswgettuple` 返回一条最近邻

**迭代式扫描（Iterative Scan）**：

当 `hnsw.iterative_scan` 开启时，若初始搜索结果被 WHERE/LIMIT 过滤，自动扩展搜索范围，保证返回足量有效结果。

**MVCC 兼容性**：

索引仅返回 heap TID，由 PostgreSQL 内核负责可见性检查，不可见元组自动过滤，完美兼容 MVCC 机制。

### 模块 2：索引扫描标准回调

代表函数：`hnswbeginscan`、`hnswrescan`、`hnswgettuple`、`hnswendscan`

**`hnswbeginscan`**

**函数功能**：索引扫描入口，分配并初始化扫描上下文。

**输入 / 输出**：输入索引关系、条件键数、排序键数；返回扫描描述符 `IndexScanDesc`。

**实现原理**：

1. 创建索引扫描描述符
2. 分配 `HnswScanOpaque` 私有上下文，存储类型信息、支持函数
3. 创建临时内存上下文，优化扫描内存使用
4. 计算扫描内存上限（基于 `work_mem` 和 `hnsw_scan_mem_multiplier`）
5. 绑定私有上下文到扫描描述符

核心代码解读：

```
so = (HnswScanOpaque) palloc(sizeof(HnswScanOpaqueData));
so->tmpCtx = AllocSetContextCreate(...);
// 计算内存限制
maxMemory = (double) work_mem * hnsw_scan_mem_multiplier * 1024.0 + 256;
scan->opaque = so;
```

**`hnswrescan`**

**函数功能**：重置 / 重启索引扫描，清空上下文状态。

**输入 / 输出**：输入扫描描述符、扫描键、排序键；无返回值。

**实现原理**：

1. 重置扫描标记、结果列表、内存上下文
2. 初始化距离、计数等状态变量
3. 复制新的扫描条件和排序键
4. 为下一次查询做好准备

**`hnswgettuple`**

**函数功能**：获取下一个最近邻结果，是扫描的核心执行函数。

**输入 / 输出**：输入扫描描述符、扫描方向；返回 true 表示获取到有效元组。

**实现原理**：

1. 仅支持正向扫描，断言校验方向
2. 首次调用执行完整 HNSW 搜索算法，生成候选列表
3. 循环从候选列表取出元组，过滤无效 / 不可见元素
4. 支持迭代扫描：内存 / 数量超限后自动扩展搜索
5. 严格模式保证距离严格递增，符合排序语义
6. 设置结果 TID，标记无需重新检查

关键特性：

- 加 `HNSW_SCAN_LOCK` 共享锁，保证扫描期间图结构稳定
- 强制要求 MVCC 快照，保障事务一致性
- 自动跳过无有效 heap TID 的已删除元素
- 迭代扫描动态补充候选集，提升过滤场景下的召回率

**`hnswendscan`**

**函数功能**：结束扫描，释放所有资源。

**输入 / 输出**：输入扫描描述符；无返回值。

**实现原理**：

1. 删除临时内存上下文
2. 释放私有上下文
3. 清空扫描描述符指针，避免野指针

### 模块 3：搜索核心算法

代表函数：`GetScanItems`、`ResumeScanItems`

**`GetScanItems`**

**函数功能**：实现标准 HNSW 层级搜索（论文 Algorithm 5），生成初始候选集。

**输入 / 输出**：输入扫描描述符、查询向量；返回排序后的候选列表。

**实现原理**：

1. 获取索引参数 `m` 和入口点
2. 从顶层到 1 层执行贪心搜索（ef=1），快速逼近目标
3. 在 0 层执行精细化搜索，参数为 `hnsw_ef_search`
4. 返回最终有序候选结果列表
5. 迭代扫描模式下记录丢弃的候选，用于后续扩展

**`ResumeScanItems`**

**函数功能**：迭代扫描专用，从已丢弃的候选中恢复并扩展搜索。

**输入 / 输出**：输入扫描描述符；返回扩展后的候选列表。

**实现原理**：

1. 从废弃候选堆中取出一批候选
2. 基于这批候选继续在 0 层搜索
3. 生成新的候选集，补充到扫描结果中
4. 实现动态扩展搜索范围的能力

### 模块 4：辅助工具函数

代表函数：`GetScanValue`

**`GetScanValue`**

**函数功能**：预处理查询向量，获取标准化的搜索值。

**输入 / 输出**：输入扫描描述符；返回处理后的向量 Datum。

**实现原理**：

1. 从排序键中提取查询向量
2. 校验向量非压缩、非 toasted
3. 支持向量归一化（若配置对应函数）
4. 返回可直接用于搜索的向量数据

### 总结

1. **核心定位**：HNSW 索引**KNN 查询引擎**，实现标准层级搜索算法
2. **标准接口**：完整实现 PG 索引扫描四件套回调，无缝对接内核
3. 关键能力：
   - 标准 HNSW 高效最近邻搜索
   - 迭代式扫描，适配复杂过滤查询
   - MVCC 兼容，事务安全
   - 内存可控，基于 `work_mem` 智能限制
4. **执行流程**：开始扫描 → 重置状态 → 循环获取元组 → 释放资源





# DAY 9

## 任务要求

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

## `src/hnswvacuum.c`

**hnswvacuum.c 是 HNSW 索引的 "VACUUM 空间回收与图修复层"**：它实现了 PostgreSQL 索引的垃圾回收接口，负责清理已删除数据、修复 HNSW 图连通性、回收磁盘空间，保障索引长期稳定运行。

### 模块 1：VACUUM 核心流程

**HNSW 索引回收分为三个阶段**：

1. 标记阶段（`hnswbulkdelete`）
   - 遍历所有索引页，通过回调判断 heap 行是否已删除
   - 移除元素上无效的 heap TID，将无有效引用的元素加入删除列表
   - 记录当前最高层级的有效元素，用于后续入口点替换
2. 图修复阶段（`RepairGraph`）
   - 清理所有邻居列表中指向已删除节点的连接
   - 为有效元素重新搜索邻居，修复图连通性
   - 若原入口点被删除，使用最高层级有效节点替换
3. 软删除阶段（`MarkDeleted`）
   - 对无效元素打上 `deleted=1` 标记，清空向量数据
   - 重置邻居指针，更新元组版本号
   - 更新元数据插入页，实现空间复用

**设计原则**：

采用**软删除 + 图修复**策略，避免直接删除节点破坏图连通性，保证查询 / 插入操作在 VACUUM 期间安全运行。

### 模块 2：标准 VACUUM 回调函数

代表函数：`hnswbulkdelete`、`hnswvacuumcleanup`

**`hnswbulkdelete`**

**函数功能**：VACUUM 批量删除核心回调，执行完整的标记 - 修复 - 删除流程。

**输入 / 输出**：

- 输入：VACUUM 信息、统计结构体、删除判断回调

- 输出：更新后的索引删除统计信息

  实现原理：

1. 初始化 VACUUM 上下文、内存、哈希表、支持函数
2. 执行 `RemoveHeapTids`：清理无效 heap TID，收集待删元素
3. 执行 `RepairGraph`：修复 HNSW 图结构，保证导航能力
4. 执行 `MarkDeleted`：软删除无效元素，更新元数据
5. 释放资源，返回统计结果

**`hnswvacuumcleanup`**

**函数功能**：VACUUM 清理收尾回调，更新索引统计信息。

**输入 / 输出**：输入 VACUUM 信息、统计数据；返回最终统计。

**实现原理**：

1. 跳过仅分析（analyze_only）场景
2. 无删除操作时直接返回
3. 更新索引总页数，供查询优化器代价估算使用

### 模块 3：标记清理阶段

代表函数：`RemoveHeapTids`、`DeletedContains`

**`RemoveHeapTids`**

**函数功能**：第一阶段核心，移除无效堆指针并标记待删元素。

**输入 / 输出**：输入 VACUUM 上下文；无返回值。

**实现原理**：

1. 遍历索引所有页，逐个检查元素元组
2. 调用回调函数过滤已删除的 heap TID
3. 保留有效 TID，清空无效位置
4. 无有效 TID 的元素加入删除哈希表
5. 记录最高层级的有效非入口元素
6. 写入 WAL 保证事务安全

核心代码逻辑：

```c++
// 遍历元素，清理无效heap TID
for (i = 0; i < HNSW_HEAPTIDS; i++) {
    if (vacuum_callback(&etup->heaptids[i], ...)) {
        stats->tuples_removed++;
    } else {
        etup->heaptids[idx++] = etup->heaptids[i];
        stats->num_index_tuples++;
    }
}
// 无有效TID则加入删除列表
if (!ItemPointerIsValid(&etup->heaptids[0])) {
    ItemPointerSet(&ip, blkno, offno);
    tidhash_insert(vacuumstate->deleted, ip, &found);
}
```

**`DeletedContains`**

**函数功能**：检查元素 TID 是否在删除列表中。

**输入 / 输出**：输入删除哈希表、索引 TID；返回是否删除。

**实现原理**：封装 TID 哈希查询接口，快速判断元素有效性。

### 模块 4：图修复阶段

代表函数：`RepairGraph`、`RepairGraphEntryPoint`、`RepairGraphElement`、`NeedsUpdated`

**`RepairGraph`**

**函数功能**：第二阶段核心，全局修复 HNSW 图结构。

**输入 / 输出**：输入 VACUUM 上下文；无返回值。

**实现原理**：

1. 加排他锁等待正在执行的插入完成
2. 优先修复入口点，保证索引可用性
3. 遍历所有有效元素，检查邻居是否包含已删除节点
4. 对需要修复的元素重新搜索邻居并更新磁盘连接
5. 动态处理可能成为新入口点的高层级元素

**`RepairGraphEntryPoint`**

**函数功能**：修复 / 替换索引入口点，是索引正常工作的关键。

**输入 / 输出**：输入 VACUUM 上下文；无返回值。

**实现原理**：

1. 检查原入口点是否被删除
2. 已删除则使用记录的最高层级元素替换
3. 未删除则修复入口点的邻居连接
4. 加锁保证入口点更新的原子性

**`NeedsUpdated`**

**函数功能**：检查元素邻居列表是否包含已删除节点，判断是否需要修复。

**输入 / 输出**：输入 VACUUM 上下文、元素；返回是否需要修复。

**实现原理**：

1. 读取邻居元组
2. 遍历所有邻居指针，检查删除哈希表
3. 0 层邻居未满也标记为需要修复，保证图健壮性

**`RepairGraphElement`**

**函数功能**：单个元素图修复，重新建立邻居连接。

**输入 / 输出**：输入 VACUUM 上下文、元素、当前入口点；无返回值。

**实现原理**：

1. 跳过入口点元素
2. 重新为元素执行 HNSW 邻居搜索
3. 覆盖磁盘上的邻居元组
4. 更新双向连接，保证图结构正确

### 模块 5：软删除与空间复用

代表函数：`MarkDeleted`

**`MarkDeleted`**

**函数功能**：第三阶段核心，执行软删除并更新空间复用信息。

**输入 / 输出**：输入 VACUUM 上下文；无返回值。

**实现原理**：

1. 加排他锁等待扫描完成
2. 遍历所有索引页，处理待删元素
3. 打上 `deleted=1` 标记，清空向量数据
4. 无效化所有邻居指针
5. 递增元组版本号，避免迭代扫描错误
6. 记录第一个空闲页为新插入页，实现空间复用
7. 提交 WAL，更新元数据

### 模块 6：辅助函数

代表函数：`InitVacuumState`、`FreeVacuumState`

**`InitVacuumState`**

**函数功能**：初始化 VACUUM 运行上下文，统一管理资源。

**输入 / 输出**：输入 VACUUM 状态、信息、回调；无返回值。

**实现原理**：

分配统计结构、哈希表、临时内存、邻居元组缓存，初始化索引支持函数。

**`FreeVacuumState`**

**函数功能**：释放 VACUUM 所有临时资源。

**输入 / 输出**：输入 VACUUM 状态；无返回值。

**实现原理**：销毁哈希表、释放内存、删除临时上下文。

### 总结

1. **核心定位**：HNSW 索引**垃圾回收与健康维护模块**，保障索引长期高效运行
2. 三阶段设计：
   - 标记：清理无效 heap TID
   - 修复：重建图连接，保证连通性
   - 删除：软删除无效元素，支持空间复用
3. 关键能力：
   - 软删除机制，不破坏图结构
   - 自动修复入口点，保证索引可用性
   - 兼容 MVCC、WAL，事务安全
   - 支持空间复用，降低索引膨胀
4. **接口标准**：完整实现 PostgreSQL `ambulkdelete`/`amvacuumcleanup` 索引回调





# 内容扩展

## 1.PG_FUNCTION_ARGS等的作用

这是 PostgreSQL 扩展开发中的标准函数注册机制，几乎每个扩展函数都会用到这个固定的"三件套"写法。下面我来详细解析每个部分。下面逐层详细解读这三件套。

**一、`PG_FUNCTION_INFO_V1(vector_dims)` — 版本声明宏**

这个宏展开后大致等价于：

```c
extern Pg_finfo_record *pg_finfo_vector_dims(void);
Pg_finfo_record *pg_finfo_vector_dims(void) {
    static const Pg_finfo_record my_finfo = { 1 };
    return &my_finfo;
}
extern void _PG_init(void);  /* 可选初始化 */
```

**它解决的问题**：PostgreSQL 动态加载 `.so` 之后，第一件事是调用 `pg_finfo_<函数名>()` 来查询该函数使用哪套调用约定（version=0 的老接口 vs version=1 的现代接口）。这个宏就是在 `.so` 里注册这个"版本信息函数"。

- `version = 1` 表示使用 **Version-1 调用约定**（通过 `FunctionCallInfo` 结构体传参），这是 PostgreSQL 7.4 以后的标准方式。
- 若没有这个宏，PG 会认为是旧的 Version-0（直接 C 参数），行为完全不同。

**二、`Datum` — 通用返回值类型**

```c
typedef uintptr_t Datum;
```

`Datum` 本质是一个**与指针等宽的无符号整数**（64位系统上是 8 字节）。它是 PostgreSQL 内部所有类型的"统一容器"：

| 情况                               | 存储方式                         |
| ---------------------------------- | -------------------------------- |
| `int32`、`bool`、`float4` 等小类型 | 值直接按位存入 `Datum`           |
| `text`、`bytea`、向量等变长类型    | 存的是指向 palloc 内存的**指针** |

所有扩展函数必须返回 `Datum`，这样 PG 执行引擎才能用统一方式处理结果。返回时通常用宏封装，如 `PG_RETURN_INT32(n)`、`PG_RETURN_POINTER(p)`，它们内部都在做类型转 `Datum` 的操作。

**三、`PG_FUNCTION_ARGS` — 参数包宏**

```c
#define PG_FUNCTION_ARGS  FunctionCallInfo fcinfo
```

展开后函数签名变为：

```c
Datum vector_dims(FunctionCallInfo fcinfo)
```

`FunctionCallInfo` 是一个指向 `FunctionCallInfoData` 结构体的指针，包含：

- `fcinfo->args[i].value`：第 i 个参数的 `Datum` 值
- `fcinfo->args[i].isnull`：第 i 个参数是否为 SQL NULL
- `fcinfo->isnull`：**写这个字段**来告诉引擎返回值是否为 NULL
- `fcinfo->flinfo`：函数元信息（OID、缓存等）

取参数时通常用封装宏，例如 pgvector 里：

```c
Vector *vec = PG_GETARG_VECTOR_P(0);   // 取第0个参数并转为 Vector*
PG_RETURN_INT32(vec->dim);             // 返回维度
```

**四、为什么要这样写？**

**1. 调用约定隔离**：C 语言没有运行时类型信息，PG 不能"自动推断"参数类型。用 `FunctionCallInfo` 统一封装，解耦了 SQL 类型系统和 C 函数签名。

**2. 动态加载安全**：PG 通过 `dlopen/dlsym` 加载扩展，`PG_FUNCTION_INFO_V1` 生成的 `pg_finfo_*` 函数是两者之间的"握手协议"，确保版本兼容。

**3. NULL 处理能力**：SQL NULL 不等于 C 的 `NULL` 指针，通过 `fcinfo->args[i].isnull` 可以精确区分"值为零"和"值缺失"。

**4. `FUNCTION_PREFIX`**：在 pgvector 源码中这通常展开为空或 `static`，用于控制符号可见性，防止不同扩展间的符号冲突。

**五、PG 如何识别这些名称**

PostgreSQL 通过两个机制把 SQL 函数和 C 符号关联起来：

```sql
-- 扩展安装时执行的 SQL
CREATE FUNCTION vector_dims(vector)
RETURNS integer
AS '$libdir/vector', 'vector_dims'   -- ← .so路径 + C符号名
LANGUAGE C IMMUTABLE STRICT;
```

加载时的执行顺序是：`dlopen(.so)` → `dlsym("pg_finfo_vector_dims")` 检查版本 → 确认是 v1 → `dlsym("vector_dims")` 拿到函数指针 → 调用时传入 `FunctionCallInfo`。名称的一致性（SQL 里写的 `'vector_dims'` 必须和 C 函数名 `vector_dims` 完全一致）是这套机制正常工作的前提。

## 2.vector_add中imax的作用

这是一个很有意思的编译器优化细节，先看两种写法的对比：

```c
// 写法 A：直接访问结构体字段
for (int i = 0; i < a->dim; i++)
    rx[i] = ax[i] + bx[i];

// 写法 B：提前用 imax 缓存
for (int i = 0, imax = a->dim; i < imax; i++)
    rx[i] = ax[i] + bx[i];
```

------

**问题的根源：指针别名（Pointer Aliasing）**

C 语言中，编译器必须保守地假设：**任意两个指针都可能指向同一块内存**（即互为别名）。

在 `vector_add` 中，`rx`（result->x）、`ax`（a->x）、`bx`（b->x）都是 `float *`。编译器无法确定 `rx` 是否与 `ax` 或 `bx` 重叠。

这导致一个关键问题：`rx[i] = ax[i] + bx[i]` 这行写操作，**理论上可能修改了 `a->dim`**（如果 `rx` 恰好与 `a` 的内存重叠）。

所以对于写法 A，编译器每次循环都必须重新从内存读取 `a->dim`，因为它无法证明上一次迭代没有改变它：

```
循环第 0 次：读 a->dim → 比较 → 执行 rx[0] = ax[0] + bx[0]
循环第 1 次：重新读 a->dim（不敢用缓存）→ 比较 → ...
循环第 2 次：再次重新读 a->dim → ...
```

这使得循环上界是一个**每次都需要内存读取的"活变量"**，SIMD 自动向量化要求循环次数在进入循环前就确定，无法满足，于是放弃向量化。

------

**imax 的作用：向编译器证明循环边界不变**

写法 B 将 `a->dim` 提前读出存入局部变量 `imax`。局部变量存在**栈或寄存器**上，编译器可以确定 `rx[i] = ...` 这样的写操作绝对不会影响到一个栈上的 `int` 局部变量，因此：

```
进入循环前：读一次 a->dim → 存入寄存器 imax（固定不变）
循环第 0～N 次：直接用寄存器中的 imax 比较，无需重新读内存
```

循环边界变成了编译期/运行期入口处就已知的常量，编译器可以放心地展开循环、生成 SIMD 指令（如 SSE/AVX），一次处理 4 或 8 个 float。

------

**为什么 `vector_concat` 不需要这个技巧**

```c
for (int i = 0, imax = a->dim; i < imax; i++)
    result->x[i] = a->x[i];         // 只读 a，写 result

for (int i = 0, imax = b->dim, start = a->dim; i < imax; i++)
    result->x[i + start] = b->x[i]; // 只读 b，写 result
```

`vector_concat` 里虽然也用了 `imax`，但其循环体是**纯拷贝**，没有加减乘这类计算密集的操作，SIMD 向量化的收益本来就小，性能瓶颈在内存带宽而不在计算，所以优化意义不大。

------

**更彻底的写法：`restrict` 关键字**

其实 `imax` 是一种"间接"的告知方式，C99 提供了更直接的方案：

```c
void add(int dim, float * restrict rx,
                  float * restrict ax,
                  float * restrict bx) { ... }
```

`restrict` 显式告诉编译器"这三个指针保证不重叠"，编译器可以做出更激进的优化。pgvector 选择用 `imax` 而不是 `restrict`，可能是为了保持与更老的 C 标准或编译器的兼容性，也更容易局部理解，无需修改函数签名。

## 3.聚合状态机

这是 PostgreSQL 实现自定义聚合函数（如 `avg(vector)`）的标准机制，用状态机的方式把"累积计算"拆成三个独立步骤。

**先理解 PostgreSQL 聚合的执行模型**

当你写 `SELECT avg(embedding) FROM items` 时，PostgreSQL 并不是把所有行都收集到内存里再一次性计算。它定义了一个"状态"，然后每来一行就更新一次状态，最后把状态转换成最终结果。这就是状态机的含义——有一个持续演变的中间状态，输入驱动它不断转移，直到终止。

<img src="C:\Users\97382\AppData\Roaming\Typora\typora-user-images\image-20260409133044298.png" alt="image-20260409133044298" style="zoom: 50%;" />

**三个函数各自承担状态机的一个角色**

`vector_accum` 是**状态转移函数**（transfn）。每处理一行输入，状态就转移一次。它维护的状态数组 layout 是 `[count, sum₀, sum₁, ...]`，用 `float64` 而非 `float32` 累加，目的是在大量向量求和时避免精度损失。第一次调用时状态为空（`dim == 0`），此时用第一个向量初始化维度；之后每次调用只需累加。

`vector_combine` 是**状态合并函数**（combinefn）。只在并行查询时出现——PostgreSQL 将数据分给多个 worker 并行处理，每个 worker 用 `vector_accum` 维护自己的局部状态，最后由 `vector_combine` 把所有局部状态汇总成一个全局状态。合并逻辑很简单：count 相加，各维 sum 相加。它还处理了一个边界情况：某个 worker 没有处理任何行（count == 0），直接返回另一边的状态。

`vector_avg` 是**终态函数**（finalfn）。状态机"停机"时调用一次，把累积的中间状态转换成用户看到的最终结果：`result[i] = state[i+1] / state[0]`。如果 `count == 0`（整张表为空），返回 SQL `NULL`，这符合 SQL 对空集求平均的标准语义。

**为什么要用状态机而不是直接计算**

如果直接计算，就需要把所有向量都加载到内存才能求和，百万行数据会撑爆内存。状态机的设计让每一行处理完就可以丢弃，内存中只保留一个固定大小的状态数组（`dim + 1` 个 `float64`），无论处理多少行，内存占用都不变。这正是流式聚合的核心价值。

## 4.语法关键字、声明选项与属性标识符

在 `vector.sql` 中看到的大写词（例如 **CREATE**, **TYPE**, **INPUT**, **RETURNS**, **AS**, **MODULE_PATHNAME** 等）并不是“宏”而是 **PostgreSQL DDL/DML 语法关键字、函数/类型/运算符声明的选项与属性标识符**。它们把 SQL 层面的对象（类型、函数、运算符、聚合、索引方法、opclass 等）与底层实现（C 函数、二进制符号、索引 handler）以及运行时属性（不可变/并行安全/严格等）绑定起来。

### 对象创建与声明类

| **关键字**                |                       **含义（简短）** | **文件中示例**                                               |
| ------------------------- | -------------------------------------: | ------------------------------------------------------------ |
| **CREATE TYPE**           |                     声明自定义数据类型 | `CREATE TYPE vector;`                                        |
| **CREATE FUNCTION**       |                定义/注册函数并绑定实现 | `CREATE FUNCTION l2_distance(vector, vector) RETURNS float8 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;` |
| **CREATE AGGREGATE**      |   定义聚合（状态函数/类型/最终函数等） | `CREATE AGGREGATE avg(vector) (SFUNC = vector_accum, STYPE = double precision[], FINALFUNC = vector_avg, INITCOND = '{0}', PARALLEL = SAFE);` |
| **CREATE CAST**           |                   注册类型转换（CAST） | `CREATE CAST (real[] AS vector) WITH FUNCTION array_to_vector(real[], integer, boolean) AS ASSIGNMENT;` |
| **CREATE OPERATOR**       |         定义自定义运算符并绑定实现函数 | `CREATE OPERATOR <-> (LEFTARG = vector, RIGHTARG = vector, PROCEDURE = l2_distance);` |
| **CREATE ACCESS METHOD**  |             注册索引访问方法与 handler | `CREATE ACCESS METHOD ivfflat TYPE INDEX HANDLER ivfflathandler;` |
| **CREATE OPERATOR CLASS** | 将运算符/函数绑定到索引方法（opclass） | `CREATE OPERATOR CLASS vector_l2_ops DEFAULT FOR TYPE vector USING ivfflat AS OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops;` |
| **COMMENT ON**            |                         为对象添加注释 | `COMMENT ON ACCESS METHOD ivfflat IS 'ivfflat index access method';` |

------

### 类型接口与声明类

| **关键字**                |                                   **含义（简短）** | **文件中示例**                              |
| ------------------------- | -------------------------------------------------: | ------------------------------------------- |
| **INPUT / OUTPUT**       |                      指定文本输入/输出函数名 | `INPUT = vector_in, OUTPUT = vector_out`    |
| **RECEIVE / SEND**       |                      指定二进制接收/发送函数 | `RECEIVE = vector_recv, SEND = vector_send` |
| **TYPMOD_IN**            |                 typmod（类型修饰符）解析函数 | `TYPMOD_IN = vector_typmod_in`              |
| **STORAGE**              | 指定存储类别（plain/external/extended/main） | `STORAGE = external`                        |

------

### 函数实现与链接相关

| **关键字 / 语法**                         |                         **含义（简短）** | **文件中示例**                                           |
| ----------------------------------------- | ---------------------------------------: | -------------------------------------------------------- |
| **AS 'MODULE_PATHNAME'**                  | 占位符，扩展安装时替换为实际共享对象路径 | `AS 'MODULE_PATHNAME' LANGUAGE C`                        |
| **AS 'obj_file','link_symbol'**           |   指定共享对象文件与导出符号（显式链接） | `AS 'MODULE_PATHNAME', 'halfvec_l2_distance' LANGUAGE C` |
| **LANGUAGE C / sql / plpgsql / internal** |                         指定函数实现语言 | `LANGUAGE C`                                             |

------

### 函数属性与优化/并行标记

| **关键字**                              |                            **含义（简短）** | **文件中示例**                     |
| --------------------------------------- | ------------------------------------------: | ---------------------------------- |
| **IMMUTABLE / STABLE / VOLATILE**       |              函数稳定性，影响优化与常量折叠 | `IMMUTABLE`                        |
| **STRICT**                              |           若任一参数为 NULL 则函数返回 NULL | `STRICT`                           |
| **PARALLEL SAFE / RESTRICTED / UNSAFE** |            并行执行安全性标记，影响并行计划 | `PARALLEL SAFE`                    |
| **LEAKPROOF**                           | 与安全下推/行级安全相关的属性（常见可选项） | （未在该文件显式列出但为常见属性） |
| **SECURITY DEFINER / INVOKER**          |                      指定函数以谁的权限执行 | （常见可选项）                     |

------

### 运算符 / opclass / operator 子句

| **关键字**                                 |                                   **含义（简短）** | **文件中示例**                                               |
| ------------------------------------------ | -------------------------------------------------: | ------------------------------------------------------------ |
| **LEFTARG / RIGHTARG / PROCEDURE**         |                 运算符声明中指定参数类型与实现函数 | `LEFTARG = vector, RIGHTARG = vector, PROCEDURE = l2_distance` |
| **COMMUTATOR / NEGATOR**                   |                 指定运算符的对易运算符与否定运算符 | `COMMUTATOR = '<->'`                                         |
| **RESTRICT / JOIN**                        |                 指定 planner 使用的选择/连接选择器 | `RESTRICT = scalarltsel, JOIN = scalarltjoinsel`             |
| **OPERATOR n / FUNCTION n / FOR ORDER BY** | opclass 中按序号注册运算符与支持函数并指定排序行为 | `OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops`     |

------

### 聚合相关子句

| **关键字**                    |                     **含义（简短）** | **文件中示例**                                               |
| ----------------------------- | -----------------------------------: | ------------------------------------------------------------ |
| **SFUNC / STYPE / FINALFUNC** | 聚合的状态函数、状态类型、最终化函数 | `SFUNC = vector_accum, STYPE = double precision[], FINALFUNC = vector_avg` |
| **COMBINEFUNC**               |         并行聚合时合并部分状态的函数 | `COMBINEFUNC = vector_combine`                               |
| **INITCOND**                  |                       聚合初始状态值 | `INITCOND = '{0}'`                                           |
| **PARALLEL**                  |               指定聚合的并行安全级别 | `PARALLEL = SAFE`                                            |

## 5.
