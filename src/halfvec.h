/*
 * halfvec.h - 16位半精度浮点向量类型的头文件
 *
 * HalfVector 使用 16 位半精度浮点数（half / float16）存储向量元素，
 * 相比 float32 的 Vector，占用一半内存，适合对精度要求不高的场景。
 *
 * 半精度浮点格式（IEEE 754-2008）：
 *   1位符号 + 5位指数 + 10位尾数，范围约 ±65504，精度约 3 位十进制。
 *
 * 硬件支持检测（编译期）：
 *   F16C_SUPPORT  : x86 F16C 指令集（_cvtsh_ss/_cvtss_sh），性能最佳
 *   FLT16_SUPPORT : C 编译器原生 _Float16 类型支持
 *   默认          : 纯软件位操作模拟，兼容所有平台
 */
#ifndef HALFVEC_H
#define HALFVEC_H

/* C 标准扩展：启用 _Float16 类型支持（若编译器支持） */
#define __STDC_WANT_IEC_60559_TYPES_EXT__

#include <float.h>

/*
 * CPU 分发（USE_DISPATCH）宏检测
 *
 * pgvector 支持"运行时 CPU 特性分发"：
 * 在支持的编译器（GCC 9+、Clang 7+、MSVC 2019+）和 x86-64 平台上，
 * 定义 USE_DISPATCH，允许为不同 CPU 特性编译多个函数版本，
 * 运行时自动选择最优实现（类似 ifunc/target_clones 机制）。
 */
#ifndef DISABLE_DISPATCH
#if defined(__x86_64__) && defined(__GNUC__) && __GNUC__ >= 9
#define USE_DISPATCH
#elif defined(__x86_64__) && defined(__clang_major__) && __clang_major__ >= 7
#define USE_DISPATCH
#elif defined(_M_AMD64) && defined(_MSC_VER) && _MSC_VER >= 1920
#define USE_DISPATCH
#endif
#endif

/*
 * USE_TARGET_CLONES - 启用 GCC target_clones 属性
 *
 * target_clones 让编译器为同一函数生成多个针对不同 CPU 特性的版本，
 * 运行时自动通过 IFUNC（间接函数）选择最优版本。
 * 需要 glibc 支持（Linux GNU 环境），且编译器支持该属性。
 */
#if defined(USE_DISPATCH) && defined(__gnu_linux__) && defined(__has_attribute)
#if __has_attribute(target_clones)
#define USE_TARGET_CLONES
#endif
#endif

/*
 * USE__GET_CPUID - 使用 <cpuid.h> 的 __get_cpuid() 函数检测 CPU 特性
 *
 * Apple Clang 在 Mac 上编译通用二进制（arm64+x86_64）时，
 * 也需要通过 __get_cpuid 检测 x86 特性，故特殊处理。
 */
#if defined(USE_DISPATCH) && (defined(HAVE__GET_CPUID) || defined(__apple_build_version__))
#define USE__GET_CPUID
#endif

#if defined(USE_DISPATCH)
#define HALFVEC_DISPATCH
#endif

/*
 * 半精度浮点实现选择：
 *
 * F16C_SUPPORT: 使用 x86 F16C 硬件指令（_cvtsh_ss），性能最优
 * FLT16_SUPPORT: 使用编译器原生 _Float16 类型，由编译器生成高效代码
 * 默认: 使用位操作手动模拟 IEEE 754 half 格式（见 halfutils.h）
 *
 * 注意：在 x86-64 上 F16C 通常比 _Float16 更快，故优先选择 F16C。
 * FreeBSD 上 _Float16 有已知问题，故排除。
 * i386 上若没有 SSE2 也不能使用 _Float16（需要 SSE2 对齐保证）。
 */
#if defined(__F16C__)
#define F16C_SUPPORT
#elif defined(__FLT16_MAX__) && !defined(HALFVEC_DISPATCH) && !defined(__FreeBSD__) && (!defined(__i386__) || defined(__SSE2__))
#define FLT16_SUPPORT
#endif

/*
 * half 类型定义：
 * 若有原生 _Float16 支持，则 half == _Float16（编译器负责转换）；
 * 否则 half == uint16（纯位操作模拟，通过 HalfToFloat4 等函数转换）。
 *
 * HALF_MAX: 半精度浮点的最大有限正值（约 65504）
 */
#ifdef FLT16_SUPPORT
#define half _Float16
#define HALF_MAX FLT16_MAX
#else
#define half uint16
#define HALF_MAX 65504
#endif

/* HalfVector 的最大维度数（与 Vector 相同） */
#define HALFVEC_MAX_DIM 16000

/*
 * HALFVEC_SIZE(_dim) - 计算存储一个 _dim 维半精度向量所需字节数
 * DatumGetHalfVector  - Datum → HalfVector*（含解压）
 * PG_GETARG_HALFVEC_P - 获取函数参数
 * PG_RETURN_HALFVEC_P - 返回函数结果
 */
#define HALFVEC_SIZE(_dim)		(offsetof(HalfVector, x) + sizeof(half)*(_dim))
#define DatumGetHalfVector(x)	((HalfVector *) PG_DETOAST_DATUM(x))
#define PG_GETARG_HALFVEC_P(x)	DatumGetHalfVector(PG_GETARG_DATUM(x))
#define PG_RETURN_HALFVEC_P(x)	PG_RETURN_POINTER(x)

/*
 * HalfVector - 16位半精度浮点稠密向量结构体
 *
 * 布局与 Vector 完全相同，只是元素类型从 float 变为 half（16位）：
 *   [vl_len_ 4B][dim 2B][unused 2B][x[0]...x[dim-1] 各2B]
 */
typedef struct HalfVector
{
	int32		vl_len_;		/* varlena 头部（请勿直接操作！） */
	int16		dim;			/* 向量维度数 */
	int16		unused;			/* 保留字段，始终为 0 */
	half		x[FLEXIBLE_ARRAY_MEMBER]; /* 半精度浮点元素数组（柔性数组） */
}			HalfVector;

/*
 * InitHalfVector(dim) - 分配并初始化一个 dim 维的半精度零向量
 *
 * 输入：dim - 向量维度数
 * 输出：指向新分配 HalfVector 结构体的指针
 */
HalfVector *InitHalfVector(int dim);

#endif
