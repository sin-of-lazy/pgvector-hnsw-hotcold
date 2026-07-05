/*
 * vector.c - float32 稠密向量类型的完整实现
 *
 * 本文件是 pgvector 扩展的核心入口，包含：
 *   1. 扩展初始化（_PG_init）
 *   2. vector 类型的 I/O 函数（text/binary 格式的输入输出）
 *   3. vector 类型的距离计算函数（L1/L2/余弦/内积等）
 *   4. vector 类型的运算符函数（+/-/*、拼接等）
 *   5. vector 类型的比较函数（<、<=、=、>= 等）
 *   6. 向量聚合函数（avg、sum）
 *   7. 类型转换函数（array↔vector、halfvec↔vector 等）
 *
 * 与 PostgreSQL 的集成方式：
 *   - PG_MODULE_MAGIC：模块魔数，PG 用于版本兼容性验证
 *   - PG_FUNCTION_INFO_V1：向 PG 注册函数的调用约定（Version 1）
 *   - Datum 类型：PG 内部值的统一表示，所有函数通过 Datum 传参/返回
 */
#include "postgres.h"

#include <math.h>

#include "bitutils.h"
#include "bitvec.h"
#include "catalog/pg_type.h"        /* FLOAT4OID、FLOAT8OID 等类型 OID 常量 */
#include "common/shortest_dec.h"    /* float_to_shortest_decimal_bufn：最短十进制输出 */
#include "fmgr.h"                   /* PG_FUNCTION_INFO_V1、PG_GETARG_*、PG_RETURN_* */
#include "halfutils.h"
#include "halfvec.h"
#include "hnsw.h"
#include "ivfflat.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"         /* pq_getmsgint、pq_sendfloat4 等：二进制协议收发 */
#include "port.h"                   /* strtof()：字符串转 float，跨平台兼容 */
#include "sparsevec.h"
#include "utils/array.h"            /* ArrayType、ARR_NDIM、deconstruct_array 等 */
#include "utils/float.h"            /* float_overflow_error、float_underflow_error */
#include "utils/fmgrprotos.h"       /* numeric_float4 等内置函数原型 */
#include "utils/lsyscache.h"        /* get_typlenbyvalalign */
#include "utils/varbit.h"
#include "vector.h"

#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

#if PG_VERSION_NUM >= 170000
#include "parser/scansup.h"         /* scanner_isspace：PG 17+ 使用解析器的空白检测 */
#endif

/*
 * STATE_DIMS(x) - 从聚合状态数组中获取向量维度
 *
 * 聚合状态数组的 layout：[count, val0, val1, ..., val_{dim-1}]
 * 共 dim+1 个元素，故 dim = ARR_DIMS(x)[0] - 1
 */
#define STATE_DIMS(x) (ARR_DIMS(x)[0] - 1)

/*
 * CreateStateDatums(dim) - 分配 dim+1 个 Datum 的数组用于聚合状态
 */
#define CreateStateDatums(dim) palloc(sizeof(Datum) * (dim + 1))

/*
 * VECTOR_TARGET_CLONES - 为距离计算函数启用 FMA 优化版本
 *
 * FMA（Fused Multiply-Add）指令将乘法和加法合并为一条指令，
 * 减少舍入误差，提升计算密集型循环的性能。
 * 若编译器已全局启用 FMA（__FMA__），则无需多版本。
 */
#if defined(USE_TARGET_CLONES) && !defined(__FMA__)
#define VECTOR_TARGET_CLONES __attribute__((target_clones("default", "fma")))
#else
#define VECTOR_TARGET_CLONES
#endif

/*
 * PG_MODULE_MAGIC / PG_MODULE_MAGIC_EXT - 模块魔数
 *
 * PostgreSQL 在加载共享库时检查此魔数，确保扩展与服务器版本兼容。
 * PG 18+ 使用新的 EXT 格式，包含扩展名和版本信息。
 */
#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(.name = "vector",.version = "0.8.2");
#else
PG_MODULE_MAGIC;
#endif

/*
 * _PG_init() - PostgreSQL 扩展模块入口函数
 *
 * 输入：无
 * 输出：无
 *
 * 当 PostgreSQL 加载本扩展时自动调用，完成所有子模块的初始化：
 *   BitvecInit()  - 检测 CPU AVX-512，设置位向量距离函数指针
 *   HalfvecInit() - 检测 CPU F16C，设置半精度向量距离函数指针
 *   HnswInit()    - 注册 HNSW GUC 参数（hnsw.ef_search 等）和索引选项
 *   IvfflatInit() - 注册 IVFFlat GUC 参数（ivfflat.probes 等）和索引选项
 */
PGDLLEXPORT void _PG_init(void);
void
_PG_init(void)
{
	BitvecInit();
	HalfvecInit();
	HnswInit();
	IvfflatInit();
}

/*
 * CheckDims(a, b) - 验证两个向量维度是否一致
 *
 * 输入：a, b - 两个 Vector 指针
 * 输出：无（维度不一致时触发 PostgreSQL 错误）
 *
 * errcode(ERRCODE_DATA_EXCEPTION) 对应 SQL 状态码 22000（数据异常）。
 * ereport 是 PostgreSQL 的错误报告机制，会向客户端返回错误并中断事务。
 */
static inline void
CheckDims(Vector * a, Vector * b)
{
	if (a->dim != b->dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("different vector dimensions %d and %d", a->dim, b->dim)));
}

/*
 * CheckExpectedDim(typmod, dim) - 验证向量维度是否符合类型修饰符要求
 *
 * 输入：
 *   typmod - 类型修饰符（如 vector(3) 的 3），-1 表示无约束
 *   dim    - 实际向量维度
 * 输出：无（不符合时触发错误）
 *
 * typmod 在 CREATE TABLE t (v vector(3)) 中指定，
 * 插入维度不为 3 的向量时会被此函数拦截。
 */
static inline void
CheckExpectedDim(int32 typmod, int dim)
{
	if (typmod != -1 && typmod != dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", typmod, dim)));
}

/*
 * CheckDim(dim) - 验证向量维度是否在有效范围内
 *
 * 输入：dim - 向量维度
 * 输出：无（越界时触发错误）
 *
 * 维度必须在 [1, VECTOR_MAX_DIM(16000)] 范围内。
 */
static inline void
CheckDim(int dim)
{
	if (dim < 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));

	if (dim > VECTOR_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("vector cannot have more than %d dimensions", VECTOR_MAX_DIM)));
}

/*
 * CheckElement(value) - 验证向量元素值是否有效
 *
 * 输入：value - float32 元素值
 * 输出：无（NaN 或 Inf 时触发错误）
 *
 * pgvector 不允许向量中出现 NaN 或无穷大，
 * 因为这些值在距离计算中会产生不确定结果，破坏索引的正确性。
 */
static inline void
CheckElement(float value)
{
	if (isnan(value))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("NaN not allowed in vector")));

	if (isinf(value))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("infinite value not allowed in vector")));
}

/*
 * InitVector(dim) - 分配并初始化一个 dim 维的零向量
 *
 * 输入：dim - 向量维度数
 * 输出：指向新分配 Vector 的指针
 *
 * palloc0：PostgreSQL 的内存分配函数，类似 calloc，在当前内存上下文中分配。
 *   内存由 PostgreSQL 管理，事务/函数结束后自动释放（无需手动 free）。
 * SET_VARSIZE：设置 varlena 头部的字节大小字段。
 */
Vector *
InitVector(int dim)
{
	Vector	   *result;
	int			size;

	size = VECTOR_SIZE(dim);
	result = (Vector *) palloc0(size);
	SET_VARSIZE(result, size);
	result->dim = dim;

	return result;
}

/*
 * vector_isspace(ch) - 判断字符是否为空白字符
 *
 * PG 17+ 直接使用解析器的 scanner_isspace 函数。
 * 早期版本使用本地实现，识别常见空白字符（空格、制表符、换行等）。
 */
#if PG_VERSION_NUM >= 170000
#define vector_isspace(ch) scanner_isspace(ch)
#else
static inline bool
vector_isspace(char ch)
{
	if (ch == ' ' ||
		ch == '\t' ||
		ch == '\n' ||
		ch == '\r' ||
		ch == '\v' ||
		ch == '\f')
		return true;
	return false;
}
#endif

/*
 * CheckStateArray(statearray, caller) - 验证聚合状态数组的合法性
 *
 * 输入：
 *   statearray - 聚合函数的状态数组（float8[]）
 *   caller     - 调用函数名（用于错误信息）
 * 输出：指向数组数据区域的 float8 指针
 *
 * 状态数组必须是一维、无 NULL、float8 类型的数组，
 * 否则说明发生了内部错误（不应向用户暴露此错误）。
 */
static float8 *
CheckStateArray(ArrayType *statearray, const char *caller)
{
	if (ARR_NDIM(statearray) != 1 ||
		ARR_DIMS(statearray)[0] < 1 ||
		ARR_HASNULL(statearray) ||
		ARR_ELEMTYPE(statearray) != FLOAT8OID)
		elog(ERROR, "%s: expected state array", caller);
	/* ARR_DATA_PTR：获取 PostgreSQL 数组的数据区起始指针 */
	return (float8 *) ARR_DATA_PTR(statearray);
}

/*
 * vector_in(cstring, oid, int4) - 将文本表示解析为 Vector 内部格式
 *
 * PostgreSQL 类型输入函数（type input function）。
 * 在以下情况被 PG 调用：
 *   - INSERT/UPDATE 将字符串字面量转换为 vector 类型
 *   - 显式类型转换 '[1,2,3]'::vector
 *
 * 输入参数（通过 PG_FUNCTION_ARGS 访问）：
 *   arg0 (cstring)：文本字符串，如 "[1.0, 2.0, 3.0]"
 *   arg1 (oid)：类型 OID（本函数忽略）
 *   arg2 (int4)：typmod（声明的维度，-1 表示无限制）
 *
 * 输出：Vector 指针（以 Datum 形式返回）
 *
 * 使用 strtof 而非 strtod 是为了避免"双舍入"问题：
 *   字符串→double→float 与字符串→float 的结果可能不同，
 *   strtof 直接到 float 保证与 PG 的 float4in 行为一致。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_in);
Datum
vector_in(PG_FUNCTION_ARGS)
{
	char	   *lit = PG_GETARG_CSTRING(0);
	int32		typmod = PG_GETARG_INT32(2);
	float		x[VECTOR_MAX_DIM];  /* 临时存储解析出的元素值 */
	int			dim = 0;
	char	   *pt = lit;
	Vector	   *result;

	/* 跳过前导空白 */
	while (vector_isspace(*pt))
		pt++;

	/* 期待 '[' 开头 */
	if (*pt != '[')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type vector: \"%s\"", lit),
				 errdetail("Vector contents must start with \"[\".")));

	pt++;

	while (vector_isspace(*pt))
		pt++;

	/* 空向量不合法 */
	if (*pt == ']')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));

	/* 循环解析每个 float 元素 */
	for (;;)
	{
		float		val;
		char	   *stringEnd;

		if (dim == VECTOR_MAX_DIM)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("vector cannot have more than %d dimensions", VECTOR_MAX_DIM)));

		while (vector_isspace(*pt))
			pt++;

		if (*pt == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type vector: \"%s\"", lit)));

		errno = 0;
		/* strtof：将字符串转为 float，stringEnd 指向未消耗的位置 */
		val = strtof(pt, &stringEnd);

		if (stringEnd == pt)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type vector: \"%s\"", lit)));

		/* errno == ERANGE && isinf(val) 表示数值溢出 */
		if (errno == ERANGE && isinf(val))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("\"%s\" is out of range for type vector", pnstrdup(pt, stringEnd - pt))));

		CheckElement(val);
		x[dim++] = val;

		pt = stringEnd;

		while (vector_isspace(*pt))
			pt++;

		if (*pt == ',')
			pt++;
		else if (*pt == ']')
		{
			pt++;
			break;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type vector: \"%s\"", lit)));
	}

	/* ']' 之后只允许空白 */
	while (vector_isspace(*pt))
		pt++;

	if (*pt != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type vector: \"%s\"", lit),
				 errdetail("Junk after closing right brace.")));

	CheckDim(dim);
	CheckExpectedDim(typmod, dim);

	result = InitVector(dim);
	for (int i = 0; i < dim; i++)
		result->x[i] = x[i];

	PG_RETURN_POINTER(result);
}

/*
 * AppendChar/AppendFloat - 输出缓冲区写入宏
 *
 * AppendChar：将单个字符写入 ptr 并前进指针
 * AppendFloat：使用最短十进制格式写入 float，返回写入字节数
 */
#define AppendChar(ptr, c) (*(ptr)++ = (c))
#define AppendFloat(ptr, f) ((ptr) += float_to_shortest_decimal_bufn((f), (ptr)))

/*
 * vector_out(vector) - 将 Vector 内部格式转换为文本表示
 *
 * PostgreSQL 类型输出函数，被 PG 在需要显示向量值时调用。
 *
 * 输入：Vector 指针
 * 输出：cstring，格式如 "[1,-0.1,2.3456]"
 *
 * 缓冲区大小计算：
 *   每个 float 最多需要 FLOAT_SHORTEST_DECIMAL_LEN - 1 字节（含小数点和指数）
 *   + dim-1 个逗号分隔符
 *   + '[' + ']' + '\0' = 3 字节
 *   合并为 FLOAT_SHORTEST_DECIMAL_LEN * dim + 2
 *
 * PG_FREE_IF_COPY：若参数是解压副本则释放，避免内存泄漏。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_out);
Datum
vector_out(PG_FUNCTION_ARGS)
{
	Vector	   *vector = PG_GETARG_VECTOR_P(0);
	int			dim = vector->dim;
	char	   *buf;
	char	   *ptr;

	buf = (char *) palloc(FLOAT_SHORTEST_DECIMAL_LEN * dim + 2);
	ptr = buf;

	AppendChar(ptr, '[');

	for (int i = 0; i < dim; i++)
	{
		if (i > 0)
			AppendChar(ptr, ',');

		AppendFloat(ptr, vector->x[i]);
	}

	AppendChar(ptr, ']');
	*ptr = '\0';

	PG_FREE_IF_COPY(vector, 0);
	PG_RETURN_CSTRING(buf);
}

/*
 * PrintVector(msg, vector) - 调试辅助函数，输出向量内容到 PG 日志
 *
 * 输入：msg - 标签字符串；vector - 要打印的向量
 * 输出：无（通过 elog(INFO) 输出到服务器日志）
 *
 * 内部调用 vector_out 获取文本表示，然后记录日志并释放临时字符串。
 */
void
PrintVector(char *msg, Vector * vector)
{
	char	   *out = DatumGetPointer(DirectFunctionCall1(vector_out, PointerGetDatum(vector)));

	elog(INFO, "%s = %s", msg, out);
	pfree(out);
}

/*
 * vector_typmod_in(cstring[]) - 解析 vector 类型修饰符
 *
 * 输入：cstring 数组（来自 vector(n) 中的 n）
 * 输出：typmod 整数值（即维度数）
 *
 * 在 CREATE TABLE 或 CAST 时，PG 调用此函数解析 vector(3) 中的 "3"，
 * 将其转换为内部 typmod 整数，供 CheckExpectedDim 使用。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_typmod_in);
Datum
vector_typmod_in(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);
	int32	   *tl;
	int			n;

	/* ArrayGetIntegerTypmods：从 cstring[] 中提取整数 typmod */
	tl = ArrayGetIntegerTypmods(ta, &n);

	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));

	if (*tl < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dimensions for type vector must be at least 1")));

	if (*tl > VECTOR_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dimensions for type vector cannot exceed %d", VECTOR_MAX_DIM)));

	PG_RETURN_INT32(*tl);
}

/*
 * vector_recv(StringInfo, oid, int4) - 从二进制协议流读取 vector
 *
 * PostgreSQL 类型的二进制输入函数（binary input function）。
 * 用于 COPY BINARY 和客户端使用二进制协议发送数据时。
 *
 * 二进制格式：[int16 dim][int16 unused][float32 x0][float32 x1]...
 *
 * 输入：StringInfo 缓冲区（网络字节序）
 * 输出：Vector 指针
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_recv);
Datum
vector_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int32		typmod = PG_GETARG_INT32(2);
	Vector	   *result;
	int16		dim;
	int16		unused;

	/* pq_getmsgint：从消息缓冲区读取指定字节数的整数 */
	dim = pq_getmsgint(buf, sizeof(int16));
	unused = pq_getmsgint(buf, sizeof(int16));

	CheckDim(dim);
	CheckExpectedDim(typmod, dim);

	if (unused != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected unused to be 0, not %d", unused)));

	result = InitVector(dim);
	for (int i = 0; i < dim; i++)
	{
		/* pq_getmsgfloat4：从消息中读取网络字节序的 float32 */
		result->x[i] = pq_getmsgfloat4(buf);
		CheckElement(result->x[i]);
	}

	PG_RETURN_POINTER(result);
}

/*
 * vector_send(vector) - 将 vector 序列化为二进制协议格式
 *
 * PostgreSQL 类型的二进制输出函数。
 * 用于 COPY BINARY 和客户端请求二进制格式时。
 *
 * 输出二进制格式：[int16 dim][int16 unused][float32...] 
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_send);
Datum
vector_send(PG_FUNCTION_ARGS)
{
	Vector	   *vec = PG_GETARG_VECTOR_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);      /* 初始化发送缓冲区 */
	pq_sendint(&buf, vec->dim, sizeof(int16));
	pq_sendint(&buf, vec->unused, sizeof(int16));
	for (int i = 0; i < vec->dim; i++)
		pq_sendfloat4(&buf, vec->x[i]);  /* 发送 float32（网络字节序） */

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * vector(vector, int4, bool) - vector 类型的类型强制函数
 *
 * 当向量在不同 typmod 间转换时（如 vector→vector(3)）调用此函数，
 * 仅做维度检查，不修改数据。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector);
Datum
vector(PG_FUNCTION_ARGS)
{
	Vector	   *vec = PG_GETARG_VECTOR_P(0);
	int32		typmod = PG_GETARG_INT32(1);

	CheckExpectedDim(typmod, vec->dim);

	PG_RETURN_POINTER(vec);
}

/*
 * array_to_vector(anyarray, int4, bool) - 将 PostgreSQL 数组转换为 vector
 *
 * 支持 int4[]、float4[]、float8[]、numeric[] → vector 的转换。
 *
 * 输入：一维非 NULL 数组
 * 输出：Vector
 *
 * deconstruct_array：将 PG 数组解包为 Datum[] 和 bool[]（isnull），
 * 便于按元素访问，但注意 pass-by-reference 类型的 Datum 指向原数组数据，
 * 不需要单独释放（只释放 elemsp 指针数组本身）。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(array_to_vector);
Datum
array_to_vector(PG_FUNCTION_ARGS)
{
	ArrayType  *array = PG_GETARG_ARRAYTYPE_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	Vector	   *result;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	Datum	   *elemsp;
	int			nelemsp;

	if (ARR_NDIM(array) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("array must be 1-D")));

	if (ARR_HASNULL(array) && array_contains_nulls(array))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array must not contain nulls")));

	/* 获取元素类型信息（长度、传值/传引用、对齐方式） */
	get_typlenbyvalalign(ARR_ELEMTYPE(array), &typlen, &typbyval, &typalign);
	deconstruct_array(array, ARR_ELEMTYPE(array), typlen, typbyval, typalign, &elemsp, NULL, &nelemsp);

	CheckDim(nelemsp);
	CheckExpectedDim(typmod, nelemsp);

	result = InitVector(nelemsp);

	/* 根据元素类型，将 Datum 转换为 float */
	if (ARR_ELEMTYPE(array) == INT4OID)
	{
		for (int i = 0; i < nelemsp; i++)
			result->x[i] = DatumGetInt32(elemsp[i]);
	}
	else if (ARR_ELEMTYPE(array) == FLOAT8OID)
	{
		for (int i = 0; i < nelemsp; i++)
			result->x[i] = DatumGetFloat8(elemsp[i]);
	}
	else if (ARR_ELEMTYPE(array) == FLOAT4OID)
	{
		for (int i = 0; i < nelemsp; i++)
			result->x[i] = DatumGetFloat4(elemsp[i]);
	}
	else if (ARR_ELEMTYPE(array) == NUMERICOID)
	{
		for (int i = 0; i < nelemsp; i++)
			result->x[i] = DatumGetFloat4(DirectFunctionCall1(numeric_float4, elemsp[i]));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("unsupported array type")));
	}

	/* 仅释放 Datum 指针数组（不释放 pass-by-reference 的元素数据，它们指向原数组） */
	pfree(elemsp);

	for (int i = 0; i < result->dim; i++)
		CheckElement(result->x[i]);

	PG_RETURN_POINTER(result);
}

/*
 * vector_to_float4(vector) - 将 vector 转换为 float4[]
 *
 * 输入：Vector 指针
 * 输出：PostgreSQL float4 数组（TYPALIGN_INT 对齐，与 float4 存储约定一致）
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_to_float4);
Datum
vector_to_float4(PG_FUNCTION_ARGS)
{
	Vector	   *vec = PG_GETARG_VECTOR_P(0);
	Datum	   *datums;
	ArrayType  *result;

	datums = (Datum *) palloc(sizeof(Datum) * vec->dim);

	for (int i = 0; i < vec->dim; i++)
		datums[i] = Float4GetDatum(vec->x[i]);

	result = construct_array(datums, vec->dim, FLOAT4OID, sizeof(float4), true, TYPALIGN_INT);

	pfree(datums);

	PG_RETURN_POINTER(result);
}

/*
 * halfvec_to_vector(halfvec, int4, bool) - 将半精度向量转换为 float32 向量
 *
 * 输入：HalfVector 指针，目标 typmod
 * 输出：Vector 指针（每个元素从 half 升精度为 float32）
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(halfvec_to_vector);
Datum
halfvec_to_vector(PG_FUNCTION_ARGS)
{
	HalfVector *vec = PG_GETARG_HALFVEC_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	Vector	   *result;

	CheckDim(vec->dim);
	CheckExpectedDim(typmod, vec->dim);

	result = InitVector(vec->dim);

	/* HalfToFloat4：将 half（16位）转为 float（32位） */
	for (int i = 0; i < vec->dim; i++)
		result->x[i] = HalfToFloat4(vec->x[i]);

	PG_RETURN_POINTER(result);
}

/*
 * VectorL2SquaredDistance - 计算 L2 平方距离（内部辅助函数）
 *
 * 输入：dim - 维度，ax/bx - 两个 float 数组
 * 输出：sum((ax[i]-bx[i])^2)
 *
 * VECTOR_TARGET_CLONES：若平台支持，编译器会生成使用 FMA 指令的优化版本。
 * 注释 "Auto-vectorized" 提示编译器此循环可自动 SIMD 向量化。
 */
VECTOR_TARGET_CLONES static float
VectorL2SquaredDistance(int dim, float *ax, float *bx)
{
	float		distance = 0.0;

	/* 编译器可自动向量化此循环（SIMD 化差值计算） */
	for (int i = 0; i < dim; i++)
	{
		float		diff = ax[i] - bx[i];

		distance += diff * diff;
	}

	return distance;
}

/*
 * l2_distance(vector, vector) - 计算 L2（欧氏）距离
 *
 * SQL：l2_distance(a, b) → float8  /  运算符：a <-> b
 *
 * 输入：两个相同维度的 vector
 * 输出：sqrt(sum((a[i]-b[i])^2))
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(l2_distance);
Datum
l2_distance(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8(sqrt((double) VectorL2SquaredDistance(a->dim, a->x, b->x)));
}

/*
 * vector_l2_squared_distance(vector, vector) - 计算 L2 平方距离
 *
 * 输出比 l2_distance 少一个 sqrt 操作，用于索引内部比较（比较平方距离
 * 与比较真实距离结果相同，但节省开销）。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_l2_squared_distance);
Datum
vector_l2_squared_distance(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8((double) VectorL2SquaredDistance(a->dim, a->x, b->x));
}

/*
 * VectorInnerProduct - 计算内积（点积）
 *
 * 输入：dim - 维度，ax/bx - 两个 float 数组
 * 输出：sum(ax[i] * bx[i])
 */
VECTOR_TARGET_CLONES static float
VectorInnerProduct(int dim, float *ax, float *bx)
{
	float		distance = 0.0;

	/* 编译器可自动向量化此循环 */
	for (int i = 0; i < dim; i++)
		distance += ax[i] * bx[i];

	return distance;
}

/*
 * inner_product(vector, vector) - 计算内积
 *
 * SQL：inner_product(a, b) → float8  /  运算符：a <#> b（负内积，用于 MIPS）
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(inner_product);
Datum
inner_product(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8((double) VectorInnerProduct(a->dim, a->x, b->x));
}

/*
 * vector_negative_inner_product(vector, vector) - 返回内积的相反数
 *
 * 输出：-(a · b)
 *
 * 最大内积搜索（Maximum Inner Product Search, MIPS）转化为
 * 最小距离搜索：找 a · b 最大 = 找 -(a · b) 最小。
 * pgvector 的 <#> 运算符使用此函数实现 MIPS 索引搜索。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_negative_inner_product);
Datum
vector_negative_inner_product(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8((double) -VectorInnerProduct(a->dim, a->x, b->x));
}

/*
 * VectorCosineSimilarity - 计算余弦相似度
 *
 * 输出：(A·B) / sqrt(||A||^2 * ||B||^2)，即 cos(θ)
 *
 * 使用 sqrt(norma * normb) 合并两个范数的 sqrt 运算，
 * 比分别 sqrt 再相乘精度更高（减少一次中间舍入）。
 */
VECTOR_TARGET_CLONES static double
VectorCosineSimilarity(int dim, float *ax, float *bx)
{
	float		similarity = 0.0;
	float		norma = 0.0;
	float		normb = 0.0;

	for (int i = 0; i < dim; i++)
	{
		similarity += ax[i] * bx[i];
		norma += ax[i] * ax[i];
		normb += bx[i] * bx[i];
	}

	return (double) similarity / sqrt((double) norma * (double) normb);
}

/*
 * cosine_distance(vector, vector) - 计算余弦距离
 *
 * SQL：cosine_distance(a, b) → float8  /  运算符：a <=> b
 *
 * 输出：1 - cos(θ)，范围 [0, 2]
 *   0 = 完全同向，1 = 正交，2 = 完全反向
 *
 * 结果被夹紧到 [−1,1] 后再计算 1-sim，防止浮点误差产生 NaN 或负距离。
 * MSVC /fp:fast 模式下 NaN 可能不被传播，故特殊处理。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(cosine_distance);
Datum
cosine_distance(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);
	double		similarity;

	CheckDims(a, b);

	similarity = VectorCosineSimilarity(a->dim, a->x, b->x);

#ifdef _MSC_VER
	/* MSVC /fp:fast 模式可能不传播 NaN，需要显式检查 */
	if (isnan(similarity))
		PG_RETURN_FLOAT8(NAN);
#endif

	/* 夹紧到 [-1, 1] 避免浮点误差导致距离为负 */
	if (similarity > 1)
		similarity = 1.0;
	else if (similarity < -1)
		similarity = -1.0;

	PG_RETURN_FLOAT8(1.0 - similarity);
}

/*
 * vector_spherical_distance(vector, vector) - 球面距离（用于球形 k-means）
 *
 * 输出：arccos(A·B) / π，即归一化角距离，范围 [0, 1]
 *
 * 满足三角不等式（余弦距离不满足），适用于球形 k-means 聚类。
 * 假设输入为单位向量（L2 范数为 1），跳过归一化步骤。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_spherical_distance);
Datum
vector_spherical_distance(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);
	double		distance;

	CheckDims(a, b);

	distance = (double) VectorInnerProduct(a->dim, a->x, b->x);

	/* 防止浮点精度损失导致 acos 输入超出 [-1,1] 范围（acos 对此会产生 NaN） */
	if (distance > 1)
		distance = 1;
	else if (distance < -1)
		distance = -1;

	PG_RETURN_FLOAT8(acos(distance) / M_PI);
}

/*
 * VectorL1Distance - 计算 L1（曼哈顿）距离
 *
 * 输出：sum(|ax[i] - bx[i]|)
 */
VECTOR_TARGET_CLONES static float
VectorL1Distance(int dim, float *ax, float *bx)
{
	float		distance = 0.0;

	for (int i = 0; i < dim; i++)
		distance += fabsf(ax[i] - bx[i]);

	return distance;
}

/*
 * l1_distance(vector, vector) - 计算 L1（曼哈顿）距离
 *
 * SQL：l1_distance(a, b) → float8  /  运算符：a <+> b
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(l1_distance);
Datum
l1_distance(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8((double) VectorL1Distance(a->dim, a->x, b->x));
}

/*
 * vector_dims(vector) - 返回向量维度
 *
 * SQL：vector_dims(v) → int4
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_dims);
Datum
vector_dims(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);

	PG_RETURN_INT32(a->dim);
}

/*
 * vector_norm(vector) - 计算 L2 范数（模长）
 *
 * SQL：vector_norm(v) → float8
 * 输出：sqrt(sum(v[i]^2))
 *
 * 使用 double 中间累加（避免 float 精度损失），最后 sqrt 返回。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_norm);
Datum
vector_norm(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	float	   *ax = a->x;
	double		norm = 0.0;

	for (int i = 0; i < a->dim; i++)
		norm += (double) ax[i] * (double) ax[i];

	PG_RETURN_FLOAT8(sqrt(norm));
}

/*
 * l2_normalize(vector) - L2 归一化（将向量缩放为单位向量）
 *
 * SQL：l2_normalize(v) → vector
 * 输出：v / ||v||（若 ||v||=0 则返回零向量）
 *
 * 归一化后的向量 L2 范数为 1，适合余弦相似度搜索的预处理。
 * 检查溢出：除以极小的范数可能产生 Inf，此时报错。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(l2_normalize);
Datum
l2_normalize(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	float	   *ax = a->x;
	double		norm = 0;
	Vector	   *result;
	float	   *rx;

	result = InitVector(a->dim);
	rx = result->x;

	for (int i = 0; i < a->dim; i++)
		norm += (double) ax[i] * (double) ax[i];

	norm = sqrt(norm);

	/* 零向量：范数为 0，直接返回零向量 */
	if (norm > 0)
	{
		for (int i = 0; i < a->dim; i++)
			rx[i] = ax[i] / norm;

		/* 检查溢出（范数极小时可能产生 Inf） */
		for (int i = 0; i < a->dim; i++)
		{
			if (isinf(rx[i]))
				float_overflow_error();
		}
	}

	PG_RETURN_POINTER(result);
}

/*
 * vector_add(vector, vector) - 向量逐元素加法，运算符 a + b
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_add);
Datum
vector_add(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);
	float	   *ax = a->x;
	float	   *bx = b->x;
	Vector	   *result;
	float	   *rx;

	CheckDims(a, b);

	result = InitVector(a->dim);
	rx = result->x;

	/* imax 提前取出，帮助编译器向量化 */
	for (int i = 0, imax = a->dim; i < imax; i++)
		rx[i] = ax[i] + bx[i];

	/* 检查溢出 */
	for (int i = 0, imax = a->dim; i < imax; i++)
	{
		if (isinf(rx[i]))
			float_overflow_error();
	}

	PG_RETURN_POINTER(result);
}

/*
 * vector_sub(vector, vector) - 向量逐元素减法，运算符 a - b
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_sub);
Datum
vector_sub(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);
	float	   *ax = a->x;
	float	   *bx = b->x;
	Vector	   *result;
	float	   *rx;

	CheckDims(a, b);

	result = InitVector(a->dim);
	rx = result->x;

	for (int i = 0, imax = a->dim; i < imax; i++)
		rx[i] = ax[i] - bx[i];

	for (int i = 0, imax = a->dim; i < imax; i++)
	{
		if (isinf(rx[i]))
			float_overflow_error();
	}

	PG_RETURN_POINTER(result);
}

/*
 * vector_mul(vector, vector) - 向量逐元素乘法，运算符 a * b
 *
 * 同时检查溢出（结果为 Inf）和下溢（两个非零数相乘结果为 0）。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_mul);
Datum
vector_mul(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);
	float	   *ax = a->x;
	float	   *bx = b->x;
	Vector	   *result;
	float	   *rx;

	CheckDims(a, b);

	result = InitVector(a->dim);
	rx = result->x;

	for (int i = 0, imax = a->dim; i < imax; i++)
		rx[i] = ax[i] * bx[i];

	for (int i = 0, imax = a->dim; i < imax; i++)
	{
		if (isinf(rx[i]))
			float_overflow_error();

		/* 下溢检查：两个非零数相乘结果为 0（精度丢失到零） */
		if (rx[i] == 0 && !(ax[i] == 0 || bx[i] == 0))
			float_underflow_error();
	}

	PG_RETURN_POINTER(result);
}

/*
 * vector_concat(vector, vector) - 向量拼接，运算符 a || b
 *
 * 输出：将 a 和 b 的元素依次拼接为一个更长的向量
 * 结果维度 = a.dim + b.dim，不能超过 VECTOR_MAX_DIM
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_concat);
Datum
vector_concat(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);
	Vector	   *result;
	int			dim = a->dim + b->dim;

	CheckDim(dim);
	result = InitVector(dim);

	for (int i = 0, imax = a->dim; i < imax; i++)
		result->x[i] = a->x[i];

	for (int i = 0, imax = b->dim, start = a->dim; i < imax; i++)
		result->x[i + start] = b->x[i];

	PG_RETURN_POINTER(result);
}

/*
 * binary_quantize(vector) - 将 float32 向量二值量化为 bit 向量
 *
 * SQL：binary_quantize(v) → bit
 *
 * 量化规则：v[i] > 0 → 位为 1，否则为 0
 * 位存储：高位优先（MSB first），8 个元素打包为 1 个字节
 *
 * 实现分两步：
 *   1. 以 8 元素为组批量处理（循环可被向量化）
 *   2. 处理尾部不足 8 个的元素
 *
 * result_byte |= (val > 0) << (7 - j)：
 *   将第 j 个（0-indexed）元素放到字节的第 7-j 位（MSB 优先）
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(binary_quantize);
Datum
binary_quantize(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	float	   *ax = a->x;
	VarBit	   *result = InitBitVector(a->dim);
	unsigned char *rx = VARBITS(result);
	int			i = 0;
	int			count = (a->dim / 8) * 8;  /* 向下取整到 8 的倍数 */

	/* 每次处理 8 个元素，打包为 1 个字节 */
	for (; i < count; i += 8)
	{
		unsigned char result_byte = 0;

		for (int j = 0; j < 8; j++)
			result_byte |= (ax[i + j] > 0) << (7 - j);

		rx[i / 8] = result_byte;
	}

	/* 处理剩余不足 8 个元素的部分 */
	for (; i < a->dim; i++)
		rx[i / 8] |= (ax[i] > 0) << (7 - (i % 8));

	PG_RETURN_VARBIT_P(result);
}

/*
 * subvector(vector, int4, int4) - 提取子向量
 *
 * SQL：subvector(v, start, count) → vector
 *
 * 输入：向量 v，起始索引 start（1-based），元素数量 count
 * 输出：v[start..start+count-1] 的子向量
 *
 * 索引从 1 开始（与 SQL substring 函数保持一致）。
 * 溢出检查：避免 start + count 整数溢出。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(subvector);
Datum
subvector(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	int32		start = PG_GETARG_INT32(1);
	int32		count = PG_GETARG_INT32(2);
	int32		end;
	float	   *ax = a->x;
	Vector	   *result;
	int			dim;

	if (count < 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));

	/* 避免 start + count 整数溢出：改写为 start > dim - count */
	if (start > a->dim - count)
		end = a->dim + 1;
	else
		end = start + count;

	/* 索引从 1 开始（类似 SQL substring），小于 1 则从头开始 */
	if (start < 1)
		start = 1;
	else if (start > a->dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("vector must have at least 1 dimension")));

	dim = end - start;
	CheckDim(dim);
	result = InitVector(dim);

	/* start-1：1-based 转 0-based 索引 */
	for (int i = 0; i < dim; i++)
		result->x[i] = ax[start - 1 + i];

	PG_RETURN_POINTER(result);
}

/*
 * vector_cmp_internal(a, b) - 向量比较（内部实现）
 *
 * 输入：两个 Vector（维度可以不同）
 * 输出：< 0（a < b），0（a == b），> 0（a > b）
 *
 * 比较顺序：先逐元素比较（字典序），相同前缀则短向量更小。
 * 此语义与 PostgreSQL 数组比较操作符保持一致。
 */
int
vector_cmp_internal(Vector * a, Vector * b)
{
	int			dim = Min(a->dim, b->dim);

	/* 先比较共有部分 */
	for (int i = 0; i < dim; i++)
	{
		if (a->x[i] < b->x[i])
			return -1;

		if (a->x[i] > b->x[i])
			return 1;
	}

	/* 共有部分相同时，维度较小的向量更小 */
	if (a->dim < b->dim)
		return -1;

	if (a->dim > b->dim)
		return 1;

	return 0;
}

/* 以下为 vector 类型的六个比较运算符实现，均调用 vector_cmp_internal */

/* vector_lt：< 运算符 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_lt);
Datum
vector_lt(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) < 0);
}

/* vector_le：<= 运算符 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_le);
Datum
vector_le(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) <= 0);
}

/* vector_eq：= 运算符 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_eq);
Datum
vector_eq(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) == 0);
}

/* vector_ne：<> 运算符 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_ne);
Datum
vector_ne(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) != 0);
}

/* vector_ge：>= 运算符 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_ge);
Datum
vector_ge(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) >= 0);
}

/* vector_gt：> 运算符 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_gt);
Datum
vector_gt(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_BOOL(vector_cmp_internal(a, b) > 0);
}

/*
 * vector_cmp(vector, vector) - 三值比较函数（用于 B-tree 排序）
 *
 * 返回 -1/0/1，供 ORDER BY 和 B-tree 索引使用。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_cmp);
Datum
vector_cmp(PG_FUNCTION_ARGS)
{
	Vector	   *a = PG_GETARG_VECTOR_P(0);
	Vector	   *b = PG_GETARG_VECTOR_P(1);

	PG_RETURN_INT32(vector_cmp_internal(a, b));
}

/*
 * vector_accum(float8[], vector) - avg 聚合的状态累积函数（transfn）
 *
 * SQL：作为 avg(vector) 聚合的过渡函数被 PG 调用
 *
 * 输入：
 *   statearray - 当前聚合状态（float8[]），layout：[count, sum0, sum1, ...]
 *   newval     - 新加入的向量
 * 输出：更新后的状态数组
 *
 * 首次调用时 dim == 0（空状态），以 newval 的维度初始化；
 * 后续调用时检查维度一致性，然后累加。
 * 使用 float8 存储累计和，避免大量向量求和时的精度损失。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_accum);
Datum
vector_accum(PG_FUNCTION_ARGS)
{
	ArrayType  *statearray = PG_GETARG_ARRAYTYPE_P(0);
	Vector	   *newval = PG_GETARG_VECTOR_P(1);
	float8	   *statevalues;
	int16		dim;
	bool		newarr;
	float8		n;
	Datum	   *statedatums;
	float	   *x = newval->x;
	ArrayType  *result;

	statevalues = CheckStateArray(statearray, "vector_accum");
	dim = STATE_DIMS(statearray);
	newarr = dim == 0;  /* 首次调用：状态为空 */

	if (newarr)
		dim = newval->dim;  /* 用第一个向量的维度初始化 */
	else
		CheckExpectedDim(dim, newval->dim);  /* 后续检查维度一致 */

	n = statevalues[0] + 1.0;  /* 计数加 1 */

	statedatums = CreateStateDatums(dim);
	statedatums[0] = Float8GetDatum(n);

	if (newarr)
	{
		/* 首次：直接复制向量元素作为初始和 */
		for (int i = 0; i < dim; i++)
			statedatums[i + 1] = Float8GetDatum((double) x[i]);
	}
	else
	{
		/* 累加：statevalues[i+1] += x[i] */
		for (int i = 0; i < dim; i++)
		{
			double		v = statevalues[i + 1] + x[i];

			if (isinf(v))
				float_overflow_error();

			statedatums[i + 1] = Float8GetDatum(v);
		}
	}

	/* 构建新的 float8[] 状态数组 */
	result = construct_array(statedatums, dim + 1,
							 FLOAT8OID,
							 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

	pfree(statedatums);

	PG_RETURN_ARRAYTYPE_P(result);
}

/*
 * vector_combine(float8[], float8[]) - 并行聚合的状态合并函数（combinefn）
 *
 * 在并行查询中，各 worker 分别维护局部状态，
 * 此函数将两个局部状态合并为一个全局状态。
 * 注意：halfvec_combine 与此函数共享相同参数逻辑，修改时需同步。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_combine);
Datum
vector_combine(PG_FUNCTION_ARGS)
{
	ArrayType  *statearray1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *statearray2 = PG_GETARG_ARRAYTYPE_P(1);
	float8	   *statevalues1;
	float8	   *statevalues2;
	float8		n;
	float8		n1;
	float8		n2;
	int16		dim;
	Datum	   *statedatums;
	ArrayType  *result;

	statevalues1 = CheckStateArray(statearray1, "vector_combine");
	statevalues2 = CheckStateArray(statearray2, "vector_combine");

	n1 = statevalues1[0];
	n2 = statevalues2[0];

	/* 处理其中一个状态为空的情况 */
	if (n1 == 0.0)
	{
		n = n2;
		dim = STATE_DIMS(statearray2);
		statedatums = CreateStateDatums(dim);
		for (int i = 1; i <= dim; i++)
			statedatums[i] = Float8GetDatum(statevalues2[i]);
	}
	else if (n2 == 0.0)
	{
		n = n1;
		dim = STATE_DIMS(statearray1);
		statedatums = CreateStateDatums(dim);
		for (int i = 1; i <= dim; i++)
			statedatums[i] = Float8GetDatum(statevalues1[i]);
	}
	else
	{
		/* 两个状态均非空：合并计数和累计和 */
		n = n1 + n2;
		dim = STATE_DIMS(statearray1);
		CheckExpectedDim(dim, STATE_DIMS(statearray2));
		statedatums = CreateStateDatums(dim);
		for (int i = 1; i <= dim; i++)
		{
			double		v = statevalues1[i] + statevalues2[i];

			if (isinf(v))
				float_overflow_error();

			statedatums[i] = Float8GetDatum(v);
		}
	}

	statedatums[0] = Float8GetDatum(n);

	result = construct_array(statedatums, dim + 1,
							 FLOAT8OID,
							 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

	pfree(statedatums);

	PG_RETURN_ARRAYTYPE_P(result);
}

/*
 * vector_avg(float8[]) - avg 聚合的终态函数（finalfn）
 *
 * 将累积的状态转换为最终平均向量。
 *
 * 输入：状态数组 [count, sum0, sum1, ...]
 * 输出：Vector，每个元素 = sum[i] / count；count=0 时返回 NULL（SQL 语义）
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_avg);
Datum
vector_avg(PG_FUNCTION_ARGS)
{
	ArrayType  *statearray = PG_GETARG_ARRAYTYPE_P(0);
	float8	   *statevalues;
	float8		n;
	uint16		dim;
	Vector	   *result;

	statevalues = CheckStateArray(statearray, "vector_avg");
	n = statevalues[0];

	/* SQL 语义：对空集求平均返回 NULL */
	if (n == 0.0)
		PG_RETURN_NULL();

	dim = STATE_DIMS(statearray);
	CheckDim(dim);
	result = InitVector(dim);
	for (int i = 0; i < dim; i++)
	{
		result->x[i] = statevalues[i + 1] / n;
		CheckElement(result->x[i]);  /* 防止平均值为 NaN 或 Inf */
	}

	PG_RETURN_POINTER(result);
}

/*
 * sparsevec_to_vector(sparsevec, int4, bool) - 将稀疏向量转换为稠密向量
 *
 * 输入：SparseVector，目标 typmod（维度约束）
 * 输出：Vector（全维度，非索引位置填 0.0）
 *
 * SPARSEVEC_VALUES(svec)：获取稀疏向量的 float 值数组
 * svec->indices[i]：第 i 个非零元素的维度索引（0-based）
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(sparsevec_to_vector);
Datum
sparsevec_to_vector(PG_FUNCTION_ARGS)
{
	SparseVector *svec = PG_GETARG_SPARSEVEC_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	Vector	   *result;
	int			dim = svec->dim;
	float	   *values = SPARSEVEC_VALUES(svec);

	CheckDim(dim);
	CheckExpectedDim(typmod, dim);

	/* InitVector 初始化为零，只需填入非零元素 */
	result = InitVector(dim);
	for (int i = 0; i < svec->nnz; i++)
		result->x[svec->indices[i]] = values[i];

	PG_RETURN_POINTER(result);
}
