/*
 * bitvec.h - 二值向量（bit vector）类型辅助函数声明
 *
 * bit 类型直接复用 PostgreSQL 内置的 varbit 类型（可变位串），
 * pgvector 在其基础上添加了汉明距离、Jaccard 距离等向量计算能力。
 */
#ifndef BITVEC_H
#define BITVEC_H

/* PostgreSQL varbit（可变位串）类型的相关宏定义 */
#include "utils/varbit.h"

/*
 * InitBitVector(dim) - 分配并初始化一个 dim 位的零向量
 *
 * 输入：dim - 位向量的维度（位数）
 * 输出：指向新分配 VarBit 结构体的指针
 *
 * VarBit 是 PostgreSQL 内置的变长位串类型，
 * VARBITTOTALLEN(dim) 计算存储 dim 位所需的总字节数（含头部）。
 */
VarBit	   *InitBitVector(int dim);

#endif
