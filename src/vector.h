/*
 * vector.h - float32 稠密向量类型的核心头文件
 *
 * 本文件定义了 pgvector 最基础的数据类型 Vector（32位浮点稠密向量），
 * 以及操作该类型所需的宏、结构体和函数声明。
 * 所有索引（HNSW/IVFFlat）和其他向量类型都依赖本头文件。
 */
#ifndef VECTOR_H
#define VECTOR_H

/* Vector 的最大维度数，超过此限制会报错 */
#define VECTOR_MAX_DIM 16000

/*
 * VECTOR_SIZE(_dim) - 计算存储一个 _dim 维向量所需的字节数
 *
 * offsetof(Vector, x) 是结构体头部（vl_len_ + dim + unused）的字节偏移，
 * 加上 dim 个 float 元素的大小即为总字节数。
 * 这是 PostgreSQL varlena（变长）类型的标准内存布局。
 */
#define VECTOR_SIZE(_dim)		(offsetof(Vector, x) + sizeof(float)*(_dim))

/*
 * DatumGetVector(x) - 将 PostgreSQL 的 Datum 类型转换为 Vector 指针
 *
 * PG_DETOAST_DATUM 会将压缩/外部存储的 varlena 数据解压到内存中，
 * 然后强制转换为 Vector*。在读取用户传入的向量参数时必须使用此宏。
 */
#define DatumGetVector(x)		((Vector *) PG_DETOAST_DATUM(x))

/*
 * PG_GETARG_VECTOR_P(x) - 获取函数第 x 个参数，返回 Vector 指针
 * PG_RETURN_VECTOR_P(x) - 从 PostgreSQL 函数返回一个 Vector 指针
 *
 * 这是 PostgreSQL 扩展函数参数传递的标准模式：
 * PG_GETARG_* 用于读取参数，PG_RETURN_* 用于返回值。
 */
#define PG_GETARG_VECTOR_P(x)	DatumGetVector(PG_GETARG_DATUM(x))
#define PG_RETURN_VECTOR_P(x)	PG_RETURN_POINTER(x)

/*
 * Vector - 32位浮点稠密向量的内存表示结构体
 *
 * 这是一个 PostgreSQL varlena（变长）类型，在磁盘和内存中的布局如下：
 *   [vl_len_ 4字节][dim 2字节][unused 2字节][x[0] ... x[dim-1] 各4字节]
 *
 * vl_len_: PostgreSQL 变长类型头，编码了整个结构体的字节大小，
 *          不要直接读写，应通过 VARSIZE/SET_VARSIZE 宏操作。
 * dim:     向量的维度数（元素个数）。
 * unused:  保留字段，目前始终为 0，供未来扩展使用。
 * x[]:    FLEXIBLE_ARRAY_MEMBER 是 C99 柔性数组，
 *          表示 x 数组紧跟在结构体后面，长度由 dim 决定。
 *          访问时通过 vector->x[i] 即可。
 */
typedef struct Vector
{
	int32		vl_len_;		/* varlena 头部（请勿直接操作！） */
	int16		dim;			/* 向量维度数 */
	int16		unused;			/* 保留字段，始终为 0 */
	float		x[FLEXIBLE_ARRAY_MEMBER]; /* 向量元素数组（柔性数组） */
}			Vector;

/*
 * InitVector(dim) - 分配并初始化一个 dim 维的零向量
 *
 * 输入：dim - 向量维度数，取值范围 [1, VECTOR_MAX_DIM]
 * 输出：指向新分配 Vector 结构体的指针（在 PostgreSQL 内存上下文中分配）
 *
 * 内部使用 palloc0 分配零初始化内存，并设置好 varlena 头和 dim 字段。
 */
Vector	   *InitVector(int dim);

/*
 * PrintVector(msg, vector) - 调试辅助函数，将向量内容打印到日志
 *
 * 输入：msg    - 前缀说明字符串
 *       vector - 要打印的向量指针
 * 输出：无（通过 elog(INFO,...) 输出到服务器日志）
 */
void		PrintVector(char *msg, Vector *vector);

/*
 * vector_cmp_internal(a, b) - 比较两个向量的大小
 *
 * 输入：a, b - 两个 Vector 指针（维度可以不同）
 * 输出：< 0 表示 a < b，0 表示 a == b，> 0 表示 a > b
 *
 * 比较规则：先逐元素比较（类似字典序），若公共部分相等则短向量更小。
 * 此规则与 PostgreSQL 数组的比较语义保持一致。
 */
int			vector_cmp_internal(Vector * a, Vector * b);

/*
 * FUNCTION_PREFIX - 函数导出符号前缀宏
 *
 * PostgreSQL 16+ 通过新机制自动导出函数符号，不需要 PGDLLEXPORT。
 * 旧版本需要显式加 PGDLLEXPORT 才能让 PostgreSQL 找到扩展函数。
 * 此宏统一处理两种情况的兼容性。
 */
#if PG_VERSION_NUM >= 160000
#define FUNCTION_PREFIX
#else
#define FUNCTION_PREFIX PGDLLEXPORT
#endif

#endif
