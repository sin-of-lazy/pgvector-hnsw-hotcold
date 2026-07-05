/*
 * bitutils.h - 二值向量距离计算函数的声明与 CPU 特性分发头文件
 *
 * 本文件声明了汉明距离（Hamming Distance）和 Jaccard 距离两个核心
 * 距离函数的函数指针，支持在运行时根据 CPU 能力选择最优实现。
 *
 * 函数指针模式（运行时分发）：
 *   BitHammingDistance 和 BitJaccardDistance 是全局函数指针变量，
 *   程序启动时在 BitvecInit() 中根据 CPU 是否支持 AVX-512 来赋值，
 *   从而在不影响可移植性的前提下利用硬件加速。
 */
#ifndef BITUTILS_H
#define BITUTILS_H

#include "postgres.h"

/* 版本检查：pgvector 要求 PostgreSQL 13+ */
#if PG_VERSION_NUM < 130000
#error "Requires PostgreSQL 13+"
#endif

/*
 * BitHammingDistance - 计算两个二值向量的汉明距离
 *
 * 函数指针，实际实现在运行时由 BitvecInit() 选择。
 *
 * 输入：
 *   bytes    - 两个位向量的字节长度
 *   ax       - 第一个位向量的字节数据指针
 *   bx       - 第二个位向量的字节数据指针
 *   distance - 累计距离初始值（通常为 0，支持分段累加）
 * 输出：
 *   两个位向量中不同位的总数（即汉明距离）
 *
 * 实现原理：通过 XOR 后 popcount（计算1的个数）得到不同位数。
 * 若 CPU 支持 AVX-512 VPOPCNTDQ，使用 512位 SIMD 批量处理以加速。
 */
extern uint64 (*BitHammingDistance) (uint32 bytes, unsigned char *ax, unsigned char *bx, uint64 distance);

/*
 * BitJaccardDistance - 计算两个二值向量的 Jaccard 距离
 *
 * 函数指针，实际实现在运行时由 BitvecInit() 选择。
 *
 * 输入：
 *   bytes - 两个位向量的字节长度
 *   ax    - 第一个位向量的字节数据指针
 *   bx    - 第二个位向量的字节数据指针
 *   ab    - |A ∩ B| 的累计值（通常为 0）
 *   aa    - |A| 的累计值（通常为 0）
 *   bb    - |B| 的累计值（通常为 0）
 * 输出：
 *   Jaccard 距离 = 1 - |A∩B| / |A∪B|，取值 [0, 1]
 *   若两个向量均为全零则返回 1（最大距离）。
 */
extern double (*BitJaccardDistance) (uint32 bytes, unsigned char *ax, unsigned char *bx, uint64 ab, uint64 aa, uint64 bb);

/*
 * BitvecInit() - 初始化位向量距离函数指针
 *
 * 输入：无
 * 输出：无（设置全局函数指针 BitHammingDistance / BitJaccardDistance）
 *
 * 在模块加载时（_PG_init → BitvecInit）被调用一次。
 * 检测 CPU 是否支持 AVX-512 VPOPCNTDQ 指令集，若支持则使用
 * 向量化版本，否则使用通用 64 位 popcount 版本。
 */
void		BitvecInit(void);

#endif
