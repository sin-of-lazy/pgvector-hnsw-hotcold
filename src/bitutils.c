/*
 * bitutils.c - 二值向量距离计算的硬件加速实现
 *
 * 本文件实现了汉明距离（Hamming Distance）和 Jaccard 距离的两套版本：
 *   1. 通用版（Default）：使用 64 位 popcount 指令，适用所有 x86-64 CPU
 *   2. AVX-512 版：使用 512 位 SIMD 向量化 + VPOPCNTDQ 指令，性能最优
 *
 * 运行时分发机制：
 *   在 BitvecInit() 中通过 CPUID 检测 CPU 是否支持 AVX-512 VPOPCNTDQ，
 *   然后将全局函数指针指向对应实现，此后所有调用均通过函数指针进行。
 */
#include "postgres.h"

#include "bitutils.h"
#include "halfvec.h"			/* 借用 USE_DISPATCH / USE_TARGET_CLONES 宏定义 */
#include "port/pg_bitutils.h"	/* pg_popcount64、pg_number_of_ones 等 PG 位运算工具 */

/* 若平台支持运行时 CPU 分发，则启用位运算的 AVX-512 加速 */
#if defined(USE_DISPATCH)
#define BIT_DISPATCH
#endif

#ifdef BIT_DISPATCH
#include <immintrin.h>	/* AVX-512 intrinsic 函数头文件（_mm512_*） */

#if defined(USE__GET_CPUID)
#include <cpuid.h>		/* GCC/Clang 的 CPUID 检测函数 */
#else
#include <intrin.h>		/* MSVC 的 CPUID 检测函数 */
#endif

/*
 * TARGET_AVX512_POPCOUNT - 函数属性：为单个函数启用 AVX-512 + VPOPCNTDQ 优化
 *
 * 这是 GCC/Clang 的 __attribute__((target(...))) 用法：
 * 仅对标注的函数启用指定的 CPU 特性，而不影响全局编译选项。
 * 这样即使主程序不支持 AVX-512，也能链接到这些函数（只是不会被调用）。
 */
#ifdef _MSC_VER
#define TARGET_AVX512_POPCOUNT
#else
#define TARGET_AVX512_POPCOUNT __attribute__((target("avx512f,avx512vpopcntdq")))
#endif
#endif

/*
 * BIT_TARGET_CLONES - 为函数生成多个 CPU 特性版本（popcnt vs 默认）
 *
 * __attribute__((target_clones("default", "popcnt"))) 让编译器生成两个版本：
 *   - "popcnt"：使用硬件 POPCNT 指令（Intel Nehalem+），通过 IFUNC 自动选择
 *   - "default"：使用软件模拟
 * 由于 LLVM 在生成 bitcode 时 target_clones 有已知崩溃问题，故排除 __llvm__。
 * 若已有 __POPCNT__ 定义，说明整个程序已启用 POPCNT，不需要多版本。
 */
#if defined(USE_TARGET_CLONES) && !defined(__POPCNT__) && !defined(__llvm__)
#define BIT_TARGET_CLONES __attribute__((target_clones("default", "popcnt")))
#else
#define BIT_TARGET_CLONES
#endif

/*
 * popcount64(x) - 统计 64 位整数中 1 的个数（Population Count）
 *
 * 优先使用编译器内置函数（__builtin_popcountl/ll），可以直接映射到
 * 硬件 POPCNT 指令，性能远好于软件实现。
 * HAVE_LONG_INT_64 / HAVE_LONG_LONG_INT_64 区分 long 是否为 64 位（平台相关）。
 * 若编译器不支持内置函数，则回退到 PG 提供的 pg_popcount64。
 */
#if defined(HAVE__BUILTIN_POPCOUNT) && defined(HAVE_LONG_INT_64)
#define popcount64(x) __builtin_popcountl(x)
#elif defined(HAVE__BUILTIN_POPCOUNT) && defined(HAVE_LONG_LONG_INT_64)
#define popcount64(x) __builtin_popcountll(x)
#elif !defined(_MSC_VER)
#define popcount64(x) pg_popcount64(x)
#endif

/* 全局函数指针，在 BitvecInit() 中被赋值为对应实现 */
uint64		(*BitHammingDistance) (uint32 bytes, unsigned char *ax, unsigned char *bx, uint64 distance);
double		(*BitJaccardDistance) (uint32 bytes, unsigned char *ax, unsigned char *bx, uint64 ab, uint64 aa, uint64 bb);

/*
 * BitHammingDistanceDefault - 汉明距离的通用实现
 *
 * 输入：
 *   bytes    - 两个位向量的字节长度
 *   ax       - 第一个位向量的字节数据
 *   bx       - 第二个位向量的字节数据
 *   distance - 初始累计距离（通常为 0）
 * 输出：两个位向量不同位的总数
 *
 * 算法：
 *   1. 以 8 字节为步长，将字节数组视为 uint64 读取（需 memcpy 保证对齐安全），
 *      通过 XOR 得到不同位，再用 popcount64 统计 1 的个数。
 *   2. 剩余不足 8 字节的部分，使用预计算的 pg_number_of_ones 查表完成。
 *
 * BIT_TARGET_CLONES 宏让编译器可以生成一个带硬件 POPCNT 的优化版本。
 */
BIT_TARGET_CLONES static uint64
BitHammingDistanceDefault(uint32 bytes, unsigned char *ax, unsigned char *bx, uint64 distance)
{
#ifdef popcount64
	/* 每次处理 8 字节（64位），利用 popcount64 加速 */
	for (; bytes >= sizeof(uint64); bytes -= sizeof(uint64))
	{
		uint64		axs;
		uint64		bxs;

		/* memcpy 避免未对齐内存访问（某些平台上直接解引用非对齐指针会崩溃） */
		memcpy(&axs, ax, sizeof(uint64));
		memcpy(&bxs, bx, sizeof(uint64));

		/* XOR 后统计 1 的个数：不同位 = 1 */
		distance += popcount64(axs ^ bxs);

		ax += sizeof(uint64);
		bx += sizeof(uint64);
	}
#endif

	/* 处理剩余字节（<8字节），使用查找表 pg_number_of_ones[byte] */
	for (uint32 i = 0; i < bytes; i++)
		distance += pg_number_of_ones[ax[i] ^ bx[i]];

	return distance;
}

#ifdef BIT_DISPATCH
/*
 * BitHammingDistanceAvx512Popcount - 汉明距离的 AVX-512 VPOPCNTDQ 加速实现
 *
 * 输入/输出与 BitHammingDistanceDefault 相同。
 *
 * 算法：
 *   使用 512 位（64字节）的 ZMM 寄存器一次处理 64 个字节，
 *   _mm512_popcnt_epi64 对每个 64 位通道做 popcount，
 *   _mm512_add_epi64 累加，最后 _mm512_reduce_add_epi64 水平求和。
 *   剩余部分回退到 Default 实现。
 *
 * TARGET_AVX512_POPCOUNT 确保此函数仅在支持 AVX-512F+VPOPCNTDQ 的 CPU 上执行。
 */
TARGET_AVX512_POPCOUNT static uint64
BitHammingDistanceAvx512Popcount(uint32 bytes, unsigned char *ax, unsigned char *bx, uint64 distance)
{
	/* 512 位 SIMD 累加器，初始化为全零 */
	__m512i		dist = _mm512_setzero_si512();

	/* 每次处理 64 字节（512 位） */
	for (; bytes >= sizeof(__m512i); bytes -= sizeof(__m512i))
	{
		/* 非对齐 512 位加载 */
		__m512i		axs = _mm512_loadu_si512((const __m512i *) ax);
		__m512i		bxs = _mm512_loadu_si512((const __m512i *) bx);

		/* XOR → 每个 64 位通道 popcount → 累加 */
		dist = _mm512_add_epi64(dist, _mm512_popcnt_epi64(_mm512_xor_si512(axs, bxs)));

		ax += sizeof(__m512i);
		bx += sizeof(__m512i);
	}

	/* 将 8 个 64 位通道的 popcount 结果水平求和 */
	distance += _mm512_reduce_add_epi64(dist);

	/* 处理剩余不足 64 字节的部分 */
	return BitHammingDistanceDefault(bytes, ax, bx, distance);
}
#endif

/*
 * BitJaccardDistanceDefault - Jaccard 距离的通用实现
 *
 * 输入：
 *   bytes - 位向量的字节长度
 *   ax, bx - 两个位向量
 *   ab    - |A ∩ B| 初始累计值（即 AND 后 1 的个数）
 *   aa    - |A| 初始累计值（A 中 1 的个数）
 *   bb    - |B| 初始累计值（B 中 1 的个数）
 * 输出：Jaccard 距离 = 1 - |A∩B| / (|A| + |B| - |A∩B|)
 *
 * Jaccard 距离度量两个集合的差异，0 表示完全相同，1 表示完全不相交。
 * 特殊情况：若 ab == 0（无共同置 1 位），返回最大距离 1。
 */
BIT_TARGET_CLONES static double
BitJaccardDistanceDefault(uint32 bytes, unsigned char *ax, unsigned char *bx, uint64 ab, uint64 aa, uint64 bb)
{
#ifdef popcount64
	for (; bytes >= sizeof(uint64); bytes -= sizeof(uint64))
	{
		uint64		axs;
		uint64		bxs;

		memcpy(&axs, ax, sizeof(uint64));
		memcpy(&bxs, bx, sizeof(uint64));

		ab += popcount64(axs & bxs);	/* 交集：AND 后 popcount */
		aa += popcount64(axs);			/* A 的 1 个数 */
		bb += popcount64(bxs);			/* B 的 1 个数 */

		ax += sizeof(uint64);
		bx += sizeof(uint64);
	}
#endif

	for (uint32 i = 0; i < bytes; i++)
	{
		ab += pg_number_of_ones[ax[i] & bx[i]];
		aa += pg_number_of_ones[ax[i]];
		bb += pg_number_of_ones[bx[i]];
	}

	/* 若无交集，距离为最大值 1 */
	if (ab == 0)
		return 1;
	else
		/* Jaccard 公式：1 - |A∩B| / |A∪B|，|A∪B| = |A| + |B| - |A∩B| */
		return 1 - (ab / ((double) (aa + bb - ab)));
}

#ifdef BIT_DISPATCH
/*
 * BitJaccardDistanceAvx512Popcount - Jaccard 距离的 AVX-512 加速实现
 *
 * 同时用 3 个 512 位寄存器分别累加 ab/aa/bb，最后汇总后调用 Default 处理余量。
 */
TARGET_AVX512_POPCOUNT static double
BitJaccardDistanceAvx512Popcount(uint32 bytes, unsigned char *ax, unsigned char *bx, uint64 ab, uint64 aa, uint64 bb)
{
	__m512i		abx = _mm512_setzero_si512();
	__m512i		aax = _mm512_setzero_si512();
	__m512i		bbx = _mm512_setzero_si512();

	for (; bytes >= sizeof(__m512i); bytes -= sizeof(__m512i))
	{
		__m512i		axs = _mm512_loadu_si512((const __m512i *) ax);
		__m512i		bxs = _mm512_loadu_si512((const __m512i *) bx);

		abx = _mm512_add_epi64(abx, _mm512_popcnt_epi64(_mm512_and_si512(axs, bxs)));
		aax = _mm512_add_epi64(aax, _mm512_popcnt_epi64(axs));
		bbx = _mm512_add_epi64(bbx, _mm512_popcnt_epi64(bxs));

		ax += sizeof(__m512i);
		bx += sizeof(__m512i);
	}

	ab += _mm512_reduce_add_epi64(abx);
	aa += _mm512_reduce_add_epi64(aax);
	bb += _mm512_reduce_add_epi64(bbx);

	return BitJaccardDistanceDefault(bytes, ax, bx, ab, aa, bb);
}
#endif

#ifdef BIT_DISPATCH
/*
 * CPU 特性位掩码定义（来自 Intel 手册）：
 *   OSXSAVE:          CPUID.1:ECX[27] - OS 是否启用了 XSAVE 状态保存
 *   AVX512F:          CPUID.7,0:EBX[16] - AVX-512 基础指令集
 *   AVX512VPOPCNTDQ:  CPUID.7,0:ECX[14] - AVX-512 向量 popcount 指令
 */
#define CPU_FEATURE_OSXSAVE         (1 << 27)
#define CPU_FEATURE_AVX512F         (1 << 16)
#define CPU_FEATURE_AVX512VPOPCNTDQ (1 << 14)

#ifdef _MSC_VER
#define TARGET_XSAVE
#else
/* 需要 xsave 目标特性才能调用 _xgetbv() */
#define TARGET_XSAVE __attribute__((target("xsave")))
#endif

/*
 * SupportsAvx512Popcount() - 运行时检测 CPU 是否支持 AVX-512 VPOPCNTDQ
 *
 * 输入：无
 * 输出：true 表示支持，false 表示不支持
 *
 * 检测步骤：
 *   1. CPUID leaf 1 检查 OSXSAVE（OS 是否通过 XSAVE 保存/恢复 AVX 状态）
 *   2. _xgetbv(0) 检查 XMM/YMM/ZMM 寄存器是否被 OS 启用（位掩码 0xe6）
 *   3. CPUID leaf 7, subleaf 0 检查 AVX512F 和 AVX512VPOPCNTDQ 支持
 */
TARGET_XSAVE static bool
SupportsAvx512Popcount()
{
	unsigned int exx[4] = {0, 0, 0, 0};

#if defined(USE__GET_CPUID)
	__get_cpuid(1, &exx[0], &exx[1], &exx[2], &exx[3]);
#else
	__cpuid(exx, 1);
#endif

	/* 检查 OS 是否启用 XSAVE（没有 XSAVE 则无法使用 AVX-512） */
	if ((exx[2] & CPU_FEATURE_OSXSAVE) != CPU_FEATURE_OSXSAVE)
		return false;

	/* 检查 XMM(位1)、YMM(位2)、ZMM(位5-7) 寄存器均已被 OS 启用 */
	if ((_xgetbv(0) & 0xe6) != 0xe6)
		return false;

#if defined(USE__GET_CPUID)
	__get_cpuid_count(7, 0, &exx[0], &exx[1], &exx[2], &exx[3]);
#else
	__cpuidex(exx, 7, 0);
#endif

	/* 检查 AVX-512F（基础 AVX-512 指令集） */
	if ((exx[1] & CPU_FEATURE_AVX512F) != CPU_FEATURE_AVX512F)
		return false;

	/* 检查 AVX-512 VPOPCNTDQ（向量 popcount 指令） */
	return (exx[2] & CPU_FEATURE_AVX512VPOPCNTDQ) == CPU_FEATURE_AVX512VPOPCNTDQ;
}
#endif

/*
 * BitvecInit() - 初始化位向量距离函数的全局函数指针
 *
 * 输入：无
 * 输出：无
 *
 * 在 PostgreSQL 扩展加载时（_PG_init → BitvecInit）被调用一次。
 * 默认使用通用 popcount 实现；若 CPU 支持 AVX-512 VPOPCNTDQ，
 * 则切换到 512 位 SIMD 加速版本，可大幅提升位向量批量处理性能。
 */
void
BitvecInit(void)
{
	/* 默认使用基于 popcount64 的通用实现 */
	BitHammingDistance = BitHammingDistanceDefault;
	BitJaccardDistance = BitJaccardDistanceDefault;

#ifdef BIT_DISPATCH
	/* 运行时检测 CPU，若支持 AVX-512 VPOPCNTDQ 则使用加速版本 */
	if (SupportsAvx512Popcount())
	{
		BitHammingDistance = BitHammingDistanceAvx512Popcount;
		BitJaccardDistance = BitJaccardDistanceAvx512Popcount;
	}
#endif
}
