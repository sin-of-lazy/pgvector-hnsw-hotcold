/*
 * halfutils.c - 半精度向量距离计算的硬件加速实现
 *
 * 本文件为 halfvec（16位半精度向量）实现了四种距离函数：
 *   - L2 平方距离（用于 L2 索引）
 *   - 内积（用于最大内积搜索）
 *   - 余弦相似度（用于余弦距离）
 *   - L1 距离（曼哈顿距离）
 *
 * 每种距离函数有两套实现：
 *   1. Default（通用）：将 half 转为 float32 后计算，适用所有平台
 *   2. F16C（AVX+F16C+FMA）：使用 256 位 SIMD + F16C 指令集，
 *      一次处理 8 个 half 元素，性能显著提升
 *
 * F16C 指令说明：
 *   _mm256_cvtph_ps(__m128i)：将 8 个 float16（128位）转换为 8 个 float32（256位）
 *   _mm256_fmadd_ps(a, b, c)：FMA = a*b + c，融合乘加，减少舍入误差
 */
#include "postgres.h"

#include <math.h>

#include "halfutils.h"
#include "halfvec.h"

#ifdef HALFVEC_DISPATCH
#include <immintrin.h>   /* AVX/F16C/FMA intrinsic 函数 */

#if defined(USE__GET_CPUID)
#include <cpuid.h>
#else
#include <intrin.h>
#endif

/*
 * TARGET_F16C - 为单个函数启用 AVX + F16C + FMA 指令集
 *
 * __attribute__((target("avx,f16c,fma"))) 让编译器为标注的函数生成
 * 使用上述指令集的机器码，而不影响整个文件的编译选项。
 * MSVC 不支持此语法，使用空定义。
 */
#ifdef _MSC_VER
#define TARGET_F16C
#else
#define TARGET_F16C __attribute__((target("avx,f16c,fma")))
#endif
#endif

/*
 * 全局距离函数指针（运行时分发）：
 * 在 HalfvecInit() 中根据 CPU 特性被赋值为 Default 或 F16c 版本。
 */
float		(*HalfvecL2SquaredDistance) (int dim, half * ax, half * bx);
float		(*HalfvecInnerProduct) (int dim, half * ax, half * bx);
double		(*HalfvecCosineSimilarity) (int dim, half * ax, half * bx);
float		(*HalfvecL1Distance) (int dim, half * ax, half * bx);

/*
 * HalfvecL2SquaredDistanceDefault - L2 平方距离的通用实现
 *
 * 输入：dim - 向量维度，ax/bx - 两个 half 向量
 * 输出：sum((ax[i] - bx[i])^2)，即 L2 距离的平方
 *
 * 先将每个 half 元素转为 float32（HalfToFloat4），再计算差的平方和。
 * 编译器通常能自动向量化此循环（"Auto-vectorized"注释说明这一点）。
 * 返回平方距离而非 L2 距离，避免额外的 sqrt 开销（索引比较时不需要 sqrt）。
 */
static float
HalfvecL2SquaredDistanceDefault(int dim, half * ax, half * bx)
{
	float		distance = 0.0;

	/* 编译器可自动向量化此循环 */
	for (int i = 0; i < dim; i++)
	{
		float		diff = HalfToFloat4(ax[i]) - HalfToFloat4(bx[i]);

		distance += diff * diff;
	}

	return distance;
}

#ifdef HALFVEC_DISPATCH
/*
 * HalfvecL2SquaredDistanceF16c - L2 平方距离的 AVX F16C 加速实现
 *
 * 输入/输出与 Default 版本相同。
 *
 * 算法（每次处理 8 个元素）：
 *   1. _mm_loadu_si128：加载 8 个 half（128位 = 8×16位）
 *   2. _mm256_cvtph_ps：F16C 指令，将 8 个 half 转为 8 个 float32（256位）
 *   3. _mm256_sub_ps：向量减法（8路并行）
 *   4. _mm256_fmadd_ps(diff, diff, dist)：FMA，dist += diff*diff（8路并行）
 *
 * 最后 _mm256_storeu_ps 将 256 位结果存入 float[8]，再水平求和。
 * 剩余不足 8 个元素的部分用 Default 标量循环处理。
 */
TARGET_F16C static float
HalfvecL2SquaredDistanceF16c(int dim, half * ax, half * bx)
{
	float		distance;
	int			i;
	float		s[8];
	int			count = (dim / 8) * 8;  /* 向下取整到 8 的倍数 */
	__m256		dist = _mm256_setzero_ps();  /* 256位 SIMD 累加器，初始为 0 */

	for (i = 0; i < count; i += 8)
	{
		/* 加载 8 个 half 元素（16字节） */
		__m128i		axi = _mm_loadu_si128((__m128i *) (ax + i));
		__m128i		bxi = _mm_loadu_si128((__m128i *) (bx + i));
		/* F16C 指令：8个 half → 8个 float32 */
		__m256		axs = _mm256_cvtph_ps(axi);
		__m256		bxs = _mm256_cvtph_ps(bxi);
		__m256		diff = _mm256_sub_ps(axs, bxs);

		/* FMA：dist = diff*diff + dist（融合乘加，8路并行） */
		dist = _mm256_fmadd_ps(diff, diff, dist);
	}

	/* 将 8 个 float32 通道存入数组后水平求和 */
	_mm256_storeu_ps(s, dist);
	distance = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];

	/* 处理剩余元素（dim % 8 个） */
	for (; i < dim; i++)
	{
		float		diff = HalfToFloat4(ax[i]) - HalfToFloat4(bx[i]);

		distance += diff * diff;
	}

	return distance;
}
#endif

/*
 * HalfvecInnerProductDefault - 内积的通用实现
 *
 * 输入：dim - 向量维度，ax/bx - 两个 half 向量
 * 输出：sum(ax[i] * bx[i])，即点积
 *
 * 内积越大表示两个向量越"相似"（方向越接近）。
 * 最大内积搜索（MIPS）使用负内积作为距离（越小越相似）。
 */
static float
HalfvecInnerProductDefault(int dim, half * ax, half * bx)
{
	float		distance = 0.0;

	for (int i = 0; i < dim; i++)
		distance += HalfToFloat4(ax[i]) * HalfToFloat4(bx[i]);

	return distance;
}

#ifdef HALFVEC_DISPATCH
/*
 * HalfvecInnerProductF16c - 内积的 AVX F16C 加速实现
 *
 * 使用 FMA 指令 dist = ax*bx + dist 并行计算 8 路内积。
 */
TARGET_F16C static float
HalfvecInnerProductF16c(int dim, half * ax, half * bx)
{
	float		distance;
	int			i;
	float		s[8];
	int			count = (dim / 8) * 8;
	__m256		dist = _mm256_setzero_ps();

	for (i = 0; i < count; i += 8)
	{
		__m128i		axi = _mm_loadu_si128((__m128i *) (ax + i));
		__m128i		bxi = _mm_loadu_si128((__m128i *) (bx + i));
		__m256		axs = _mm256_cvtph_ps(axi);
		__m256		bxs = _mm256_cvtph_ps(bxi);

		/* FMA：dist = ax*bx + dist */
		dist = _mm256_fmadd_ps(axs, bxs, dist);
	}

	_mm256_storeu_ps(s, dist);
	distance = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];

	for (; i < dim; i++)
		distance += HalfToFloat4(ax[i]) * HalfToFloat4(bx[i]);

	return distance;
}
#endif

/*
 * HalfvecCosineSimilarityDefault - 余弦相似度的通用实现
 *
 * 输入：dim - 向量维度，ax/bx - 两个 half 向量
 * 输出：cos(θ) = (A·B) / (||A|| * ||B||)，取值 [-1, 1]
 *
 * 余弦距离 = 1 - 余弦相似度，值越小越相似。
 * 使用 sqrt(norma * normb) 而非 sqrt(norma) * sqrt(normb)，
 * 是因为前者只需一次 sqrt，减少计算量且数值更稳定。
 */
static double
HalfvecCosineSimilarityDefault(int dim, half * ax, half * bx)
{
	float		similarity = 0.0;
	float		norma = 0.0;
	float		normb = 0.0;

	for (int i = 0; i < dim; i++)
	{
		float		axi = HalfToFloat4(ax[i]);
		float		bxi = HalfToFloat4(bx[i]);

		similarity += axi * bxi;  /* 分子：点积 */
		norma += axi * axi;       /* ||A||^2 */
		normb += bxi * bxi;       /* ||B||^2 */
	}

	/* cos(θ) = A·B / sqrt(||A||^2 * ||B||^2) */
	return (double) similarity / sqrt((double) norma * (double) normb);
}

#ifdef HALFVEC_DISPATCH
/*
 * HalfvecCosineSimilarityF16c - 余弦相似度的 AVX F16C 加速实现
 *
 * 同时用 3 个 256 位 SIMD 寄存器分别累加 similarity/norma/normb，
 * 8路并行，最后水平求和后计算余弦值。
 */
TARGET_F16C static double
HalfvecCosineSimilarityF16c(int dim, half * ax, half * bx)
{
	float		similarity;
	float		norma;
	float		normb;
	int			i;
	float		s[8];
	int			count = (dim / 8) * 8;
	__m256		sim = _mm256_setzero_ps();
	__m256		na = _mm256_setzero_ps();
	__m256		nb = _mm256_setzero_ps();

	for (i = 0; i < count; i += 8)
	{
		__m128i		axi = _mm_loadu_si128((__m128i *) (ax + i));
		__m128i		bxi = _mm_loadu_si128((__m128i *) (bx + i));
		__m256		axs = _mm256_cvtph_ps(axi);
		__m256		bxs = _mm256_cvtph_ps(bxi);

		sim = _mm256_fmadd_ps(axs, bxs, sim);  /* 累加点积 */
		na = _mm256_fmadd_ps(axs, axs, na);    /* 累加 ||A||^2 */
		nb = _mm256_fmadd_ps(bxs, bxs, nb);   /* 累加 ||B||^2 */
	}

	/* 水平求和 */
	_mm256_storeu_ps(s, sim);
	similarity = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];

	_mm256_storeu_ps(s, na);
	norma = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];

	_mm256_storeu_ps(s, nb);
	normb = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];

	/* 处理剩余元素 */
	for (; i < dim; i++)
	{
		float		axi = HalfToFloat4(ax[i]);
		float		bxi = HalfToFloat4(bx[i]);

		similarity += axi * bxi;
		norma += axi * axi;
		normb += bxi * bxi;
	}

	return (double) similarity / sqrt((double) norma * (double) normb);
}
#endif

/*
 * HalfvecL1DistanceDefault - L1（曼哈顿）距离的通用实现
 *
 * 输入：dim - 向量维度，ax/bx - 两个 half 向量
 * 输出：sum(|ax[i] - bx[i]|)，即各维度绝对差之和
 */
static float
HalfvecL1DistanceDefault(int dim, half * ax, half * bx)
{
	float		distance = 0.0;

	for (int i = 0; i < dim; i++)
		distance += fabsf(HalfToFloat4(ax[i]) - HalfToFloat4(bx[i]));

	return distance;
}

#ifdef HALFVEC_DISPATCH
/*
 * HalfvecL1DistanceF16c - L1 距离的 AVX F16C 加速实现
 *
 * 取绝对值技巧：使用 _mm256_andnot_ps(sign_mask, x) 清除符号位，
 * 其中 sign_mask = -0.0f（仅符号位为 1），andnot 相当于 ~sign_mask & x。
 * 注意：FMA 对 L1 无收益（非乘加运算），但为了代码统一仍使用相同框架。
 */
TARGET_F16C static float
HalfvecL1DistanceF16c(int dim, half * ax, half * bx)
{
	float		distance;
	int			i;
	float		s[8];
	int			count = (dim / 8) * 8;
	__m256		dist = _mm256_setzero_ps();
	/* 符号位掩码：-0.0f 的 IEEE 754 表示只有符号位为 1 */
	__m256		sign = _mm256_set1_ps(-0.0);

	for (i = 0; i < count; i += 8)
	{
		__m128i		axi = _mm_loadu_si128((__m128i *) (ax + i));
		__m128i		bxi = _mm_loadu_si128((__m128i *) (bx + i));
		__m256		axs = _mm256_cvtph_ps(axi);
		__m256		bxs = _mm256_cvtph_ps(bxi);

		/* andnot(sign, diff) = 清除 diff 的符号位 = |diff| */
		dist = _mm256_add_ps(dist, _mm256_andnot_ps(sign, _mm256_sub_ps(axs, bxs)));
	}

	_mm256_storeu_ps(s, dist);
	distance = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7];

	for (; i < dim; i++)
		distance += fabsf(HalfToFloat4(ax[i]) - HalfToFloat4(bx[i]));

	return distance;
}
#endif

#ifdef HALFVEC_DISPATCH
/*
 * CPU 特性位掩码（CPUID leaf 1, ECX 寄存器）：
 *   FMA:    位 12 - 支持融合乘加指令（_mm256_fmadd_ps 等）
 *   OSXSAVE:位 27 - OS 已启用 XSAVE 状态保存
 *   AVX:    位 28 - 支持 256 位 AVX 向量指令
 *   F16C:   位 29 - 支持 half/float32 转换指令（_mm256_cvtph_ps 等）
 */
#define CPU_FEATURE_FMA     (1 << 12)
#define CPU_FEATURE_OSXSAVE (1 << 27)
#define CPU_FEATURE_AVX     (1 << 28)
#define CPU_FEATURE_F16C    (1 << 29)

#ifdef _MSC_VER
#define TARGET_XSAVE
#else
#define TARGET_XSAVE __attribute__((target("xsave")))
#endif

/*
 * SupportsCpuFeature(feature) - 检测 CPU 是否支持指定特性
 *
 * 输入：feature - 来自 CPUID leaf 1 ECX 的特性位掩码
 * 输出：true 表示支持，false 表示不支持
 *
 * 与 bitutils.c 中的检测逻辑类似，但只检查 XMM/YMM（不检查 ZMM），
 * 因为 F16C/AVX 只需要 256 位寄存器，不需要 AVX-512 的 512 位 ZMM。
 */
TARGET_XSAVE static bool
SupportsCpuFeature(unsigned int feature)
{
	unsigned int exx[4] = {0, 0, 0, 0};

#if defined(USE__GET_CPUID)
	__get_cpuid(1, &exx[0], &exx[1], &exx[2], &exx[3]);
#else
	__cpuid(exx, 1);
#endif

	/* 检查 OS 是否支持 XSAVE */
	if ((exx[2] & CPU_FEATURE_OSXSAVE) != CPU_FEATURE_OSXSAVE)
		return false;

	/* 检查 XMM(位1) 和 YMM(位2) 寄存器已被 OS 启用（0b110 = 6） */
	if ((_xgetbv(0) & 6) != 6)
		return false;

	/* 检查指定的 CPU 特性位 */
	return (exx[2] & feature) == feature;
}
#endif

/*
 * HalfvecInit() - 初始化半精度向量距离函数的全局函数指针
 *
 * 输入：无
 * 输出：无（设置 4 个全局距离函数指针）
 *
 * 在 PostgreSQL 扩展加载时（_PG_init → HalfvecInit）被调用一次。
 * 若 CPU 同时支持 AVX + F16C + FMA，则使用硬件加速版本，
 * 三个特性缺一不可：AVX 提供 256 位寄存器，F16C 提供转换指令，
 * FMA 提供融合乘加（虽然 L1 不用 FMA，但统一判断简化逻辑）。
 */
void
HalfvecInit(void)
{
	/* 默认使用通用实现 */
	HalfvecL2SquaredDistance = HalfvecL2SquaredDistanceDefault;
	HalfvecInnerProduct = HalfvecInnerProductDefault;
	HalfvecCosineSimilarity = HalfvecCosineSimilarityDefault;
	HalfvecL1Distance = HalfvecL1DistanceDefault;

#ifdef HALFVEC_DISPATCH
	/* 若 CPU 同时支持 AVX、F16C、FMA，则切换到硬件加速版本 */
	if (SupportsCpuFeature(CPU_FEATURE_AVX | CPU_FEATURE_F16C | CPU_FEATURE_FMA))
	{
		HalfvecL2SquaredDistance = HalfvecL2SquaredDistanceF16c;
		HalfvecInnerProduct = HalfvecInnerProductF16c;
		HalfvecCosineSimilarity = HalfvecCosineSimilarityF16c;
		HalfvecL1Distance = HalfvecL1DistanceF16c;
	}
#endif
}
