/*
 * bitvec.c - 二值向量类型的 PostgreSQL 函数实现
 *
 * 本文件实现了 pgvector 中 bit 类型（二值向量）的两个距离函数：
 *   - hamming_distance：汉明距离（不同位的数量）
 *   - jaccard_distance：Jaccard 距离（1 - 交集/并集）
 *
 * bit 类型直接复用 PostgreSQL 内置的 varbit 变长位串类型，
 * 距离计算的底层实现委托给 bitutils.c 中的函数指针（支持 AVX-512 加速）。
 */
#include "postgres.h"

#include "bitutils.h"
#include "bitvec.h"
#include "fmgr.h"			/* PG_FUNCTION_INFO_V1、PG_GETARG_* 等宏 */
#include "utils/varbit.h"	/* VarBit、VARBITLEN、VARBITBYTES、VARBITS 宏 */
#include "vector.h"

#if PG_VERSION_NUM >= 160000
#include "varatt.h"			/* PostgreSQL 16+ 将 varatt 宏移到独立头文件 */
#endif

/*
 * InitBitVector(dim) - 分配并初始化一个 dim 位的零向量
 *
 * 输入：dim - 位向量的维度（位数）
 * 输出：指向新分配 VarBit 结构体的指针
 *
 * VARBITTOTALLEN(dim) 计算存储 dim 位所需总字节数：
 *   = VARHDRSZ（4字节 varlena 头）+ sizeof(int32)（位长度字段）+ ceil(dim/8) 字节数据
 * palloc0 确保数据部分全部初始化为 0（零向量）。
 * VARBITLEN(result) = dim 设置位长度字段。
 */
VarBit *
InitBitVector(int dim)
{
	VarBit	   *result;
	int			size;

	size = VARBITTOTALLEN(dim);
	result = (VarBit *) palloc0(size);
	SET_VARSIZE(result, size);
	VARBITLEN(result) = dim;

	return result;
}

/*
 * CheckDims(a, b) - 验证两个位向量的维度是否一致
 *
 * 输入：a, b - 两个 VarBit 指针
 * 输出：无（维度不一致时抛出 PostgreSQL 错误）
 *
 * VARBITLEN 宏返回位向量的位数（不是字节数）。
 * 所有二元距离函数在计算前都必须调用此检查。
 */
static inline void
CheckDims(VarBit *a, VarBit *b)
{
	if (VARBITLEN(a) != VARBITLEN(b))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("different bit lengths %u and %u", VARBITLEN(a), VARBITLEN(b))));
}

/*
 * hamming_distance(a, b) - 计算两个 bit 向量的汉明距离
 *
 * PostgreSQL 函数签名：hamming_distance(bit, bit) → float8
 *
 * 输入：两个相同长度的 bit 向量
 * 输出：不同位的数量（返回 float8 类型以与其他距离函数保持一致）
 *
 * PG_FUNCTION_INFO_V1 向 PostgreSQL 注册此为 V1 调用约定函数，
 * 允许 PostgreSQL 通过动态链接找到并调用它。
 *
 * VARBITBYTES(a) = ceil(VARBITLEN(a) / 8)，位向量的字节数
 * VARBITS(a) 返回指向实际位数据的 unsigned char* 指针
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(hamming_distance);
Datum
hamming_distance(PG_FUNCTION_ARGS)
{
	VarBit	   *a = PG_GETARG_VARBIT_P(0);
	VarBit	   *b = PG_GETARG_VARBIT_P(1);

	CheckDims(a, b);

	/* 调用全局函数指针（运行时已根据 CPU 选择最优实现），初始距离为 0 */
	PG_RETURN_FLOAT8((double) BitHammingDistance(VARBITBYTES(a), VARBITS(a), VARBITS(b), 0));
}

/*
 * jaccard_distance(a, b) - 计算两个 bit 向量的 Jaccard 距离
 *
 * PostgreSQL 函数签名：jaccard_distance(bit, bit) → float8
 *
 * 输入：两个相同长度的 bit 向量
 * 输出：Jaccard 距离，取值 [0, 1]
 *   0 = 完全相同（所有位一致）
 *   1 = 完全不相交（没有公共置 1 位，或其中一个全为 0）
 *
 * 初始累计值 ab=0, aa=0, bb=0，由 BitJaccardDistance 从头开始计算。
 */
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(jaccard_distance);
Datum
jaccard_distance(PG_FUNCTION_ARGS)
{
	VarBit	   *a = PG_GETARG_VARBIT_P(0);
	VarBit	   *b = PG_GETARG_VARBIT_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8(BitJaccardDistance(VARBITBYTES(a), VARBITS(a), VARBITS(b), 0, 0, 0));
}
