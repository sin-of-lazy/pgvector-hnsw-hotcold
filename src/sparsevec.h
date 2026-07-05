/*
 * sparsevec.h - 稀疏向量类型的头文件
 *
 * SparseVector 用于存储大多数元素为零的高维向量（如词袋模型、BM25向量等）。
 * 相比 Vector（稠密），它只存储非零元素的索引和值，节省空间和计算。
 *
 * 内存布局（紧凑格式）：
 *   [vl_len_ 4B][dim 4B][nnz 4B][unused 4B]
 *   [indices[0]...indices[nnz-1]  各4B（int32，0-based）]
 *   [values[0]...values[nnz-1]   各4B（float32）]
 *
 * indices 和 values 紧邻存储，通过 SPARSEVEC_VALUES 宏定位 values 起始位置。
 * 索引必须按升序排列，且均为 0-based（与 C 数组一致）。
 */
#ifndef SPARSEVEC_H
#define SPARSEVEC_H

/* 稀疏向量支持的最大总维度数（10亿），远超稠密向量的 16000 */
#define SPARSEVEC_MAX_DIM 1000000000
/* 稀疏向量中非零元素（Non-Zero）的最大数量 */
#define SPARSEVEC_MAX_NNZ 16000

/*
 * 类型转换宏 - 与 Vector 的宏含义相同，专用于 SparseVector：
 *   DatumGetSparseVector   : Datum → SparseVector*（含解压）
 *   PG_GETARG_SPARSEVEC_P  : 获取函数参数
 *   PG_RETURN_SPARSEVEC_P  : 返回函数结果
 */
#define DatumGetSparseVector(x)		((SparseVector *) PG_DETOAST_DATUM(x))
#define PG_GETARG_SPARSEVEC_P(x)	DatumGetSparseVector(PG_GETARG_DATUM(x))
#define PG_RETURN_SPARSEVEC_P(x)	PG_RETURN_POINTER(x)

/*
 * SparseVector - 稀疏向量结构体
 *
 * vl_len_: PostgreSQL varlena 头（请勿直接操作）
 * dim:     向量的总维度（包含零元素）
 * nnz:     非零元素的数量（Number of Non-Zeros）
 * unused:  保留字段，始终为 0
 * indices: 非零元素的索引数组（0-based，升序），
 *          紧跟在 unused 后面，共 nnz 个 int32。
 *          values 数组紧跟在 indices 后面，需用 SPARSEVEC_VALUES 宏访问。
 */
typedef struct SparseVector
{
	int32		vl_len_;		/* varlena 头部（请勿直接操作！） */
	int32		dim;			/* 总维度数 */
	int32		nnz;			/* 非零元素数量 */
	int32		unused;			/* 保留字段，始终为 0 */
	int32		indices[FLEXIBLE_ARRAY_MEMBER]; /* 非零元素索引数组（柔性数组） */
}			SparseVector;

/*
 * SPARSEVEC_SIZE(nnz) - 计算存储 nnz 个非零元素的稀疏向量所需字节数
 *
 * 输入：nnz - 非零元素数量
 * 输出：总字节数 = 头部偏移 + nnz个int32索引 + nnz个float32值
 *
 * 注意：这是内联函数而非宏，避免 nnz 被多次求值的副作用。
 */
static inline Size
SPARSEVEC_SIZE(int nnz)
{
	return offsetof(SparseVector, indices) + (nnz * sizeof(int32)) + (nnz * sizeof(float));
}

/*
 * SPARSEVEC_VALUES(x) - 获取稀疏向量 x 中 values 数组的起始指针
 *
 * 输入：x - SparseVector 指针
 * 输出：指向 float 数组的指针，该数组紧跟在 indices 数组之后
 *
 * 内存布局：...indices[nnz]...values[nnz]...
 * values 的地址 = 结构体基地址 + indices字段偏移 + nnz*sizeof(int32)
 */
static inline float *
SPARSEVEC_VALUES(SparseVector * x)
{
	return (float *) (((char *) x) + offsetof(SparseVector, indices) + (x->nnz * sizeof(int32)));
}

/*
 * InitSparseVector(dim, nnz) - 分配并初始化稀疏向量
 *
 * 输入：
 *   dim - 向量总维度
 *   nnz - 非零元素数量
 * 输出：指向新分配 SparseVector 结构体的指针（零初始化）
 */
SparseVector *InitSparseVector(int dim, int nnz);

#endif
