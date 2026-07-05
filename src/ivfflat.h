/*
 * ivfflat.h - IVFFlat（Inverted File with Flat compression）索引头文件
 *
 * IVFFlat 算法简介：
 *   IVFFlat 是一种基于聚类的近似最近邻索引。
 *   构建阶段：
 *     1. 从数据中采样，用 k-means 聚类得到 lists 个聚类中心（质心）
 *     2. 将每个向量分配到最近的质心，存入对应的"倒排列表"
 *   搜索阶段：
 *     1. 计算查询向量与所有质心的距离，选择最近的 probes 个列表
 *     2. 在这 probes 个列表中线性扫描，返回最近邻
 *
 * 主要参数：
 *   lists  - 聚类中心数（影响精度和构建时间），建议 sqrt(n)
 *   probes - 搜索时扫描的列表数（影响召回率和查询速度）
 *
 * PostgreSQL 集成方式与 HNSW 相同，通过 IndexAmRoutine 实现。
 * 磁盘存储布局：
 *   第 0 块：元数据页（IvfflatMetaPageData）
 *   第 1 块起：列表头页（IvfflatListData，含质心向量）
 *   后续页：各列表的向量数据页
 *
 * 实现分布在 ivfflat.c / ivfbuild.c / ivfinsert.c / ivfkmeans.c /
 * ivfscan.c / ivfutils.c / ivfvacuum.c 中。
 */
#ifndef IVFFLAT_H
#define IVFFLAT_H

#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/parallel.h"
#include "lib/pairingheap.h"
#include "nodes/execnodes.h"
#include "port.h"				/* for random() */
#include "utils/sampling.h"
#include "utils/tuplesort.h"
#include "vector.h"

#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

#if PG_VERSION_NUM >= 150000
#include "common/pg_prng.h"
#endif

#ifdef IVFFLAT_BENCH
#include "portability/instr_time.h"
#endif

#if PG_VERSION_NUM >= 190000
typedef Pointer Item;
#endif

#define IVFFLAT_MAX_DIM 2000

/* Support functions */
#define IVFFLAT_DISTANCE_PROC 1
#define IVFFLAT_NORM_PROC 2
#define IVFFLAT_KMEANS_DISTANCE_PROC 3
#define IVFFLAT_KMEANS_NORM_PROC 4
#define IVFFLAT_TYPE_INFO_PROC 5

#define IVFFLAT_VERSION	1
#define IVFFLAT_MAGIC_NUMBER 0x14FF1A7
#define IVFFLAT_PAGE_ID	0xFF84

/* Preserved page numbers */
#define IVFFLAT_METAPAGE_BLKNO	0
#define IVFFLAT_HEAD_BLKNO		1	/* first list page */

/* IVFFlat parameters */
/* IVFFlat 参数默认值和范围：
 * lists：聚类中心数，决定数据被分成多少组，影响精度和内存
 * probes：搜索时探测的列表数，越多召回越高但越慢
 *   默认 probes=1 性能好但召回率低；建议根据精度需求调大
 */
#define IVFFLAT_DEFAULT_LISTS	100
#define IVFFLAT_MIN_LISTS		1
#define IVFFLAT_MAX_LISTS		32768
#define IVFFLAT_DEFAULT_PROBES	1

/* Build phases */
/* PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE is 1 */
#define PROGRESS_IVFFLAT_PHASE_KMEANS	2
#define PROGRESS_IVFFLAT_PHASE_ASSIGN	3
#define PROGRESS_IVFFLAT_PHASE_LOAD		4

#define IVFFLAT_LIST_SIZE(size)	(offsetof(IvfflatListData, center) + size)

#define IvfflatPageGetOpaque(page)	((IvfflatPageOpaque) PageGetSpecialPointer(page))
#define IvfflatPageGetMeta(page)	((IvfflatMetaPageData *) PageGetContents(page))

#ifdef IVFFLAT_BENCH
#define IvfflatBench(name, code) \
	do { \
		instr_time	start; \
		instr_time	duration; \
		INSTR_TIME_SET_CURRENT(start); \
		(code); \
		INSTR_TIME_SET_CURRENT(duration); \
		INSTR_TIME_SUBTRACT(duration, start); \
		elog(INFO, "%s: %.3f ms", name, INSTR_TIME_GET_MILLISEC(duration)); \
	} while (0)
#else
#define IvfflatBench(name, code) (code)
#endif

#if PG_VERSION_NUM >= 150000
#define RandomDouble() pg_prng_double(&pg_global_prng_state)
#define RandomInt() pg_prng_uint32(&pg_global_prng_state)
#define SeedRandom(seed) pg_prng_seed(&pg_global_prng_state, seed)
#else
#define RandomDouble() (((double) random()) / MAX_RANDOM_VALUE)
#define RandomInt() random()
#define SeedRandom(seed) srandom(seed)
#endif

/* Variables */
extern int	ivfflat_probes;
extern int	ivfflat_iterative_scan;
extern int	ivfflat_max_probes;

typedef enum IvfflatIterativeScanMode
{
	IVFFLAT_ITERATIVE_SCAN_OFF,
	IVFFLAT_ITERATIVE_SCAN_RELAXED
}			IvfflatIterativeScanMode;

typedef struct VectorArrayData
{
	int			length;
	int			maxlen;
	int			dim;
	Size		itemsize;
	char	   *items;
}			VectorArrayData;

typedef VectorArrayData * VectorArray;

typedef struct ListInfo
{
	BlockNumber blkno;
	OffsetNumber offno;
}			ListInfo;

/* IVFFlat index options */
typedef struct IvfflatOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			lists;			/* number of lists */
}			IvfflatOptions;

typedef struct IvfflatSpool
{
	Tuplesortstate *sortstate;
	Relation	heap;
	Relation	index;
}			IvfflatSpool;

typedef struct IvfflatShared
{
	/* Immutable state */
	Oid			heaprelid;
	Oid			indexrelid;
	bool		isconcurrent;
	int			scantuplesortstates;

	/* Worker progress */
	ConditionVariable workersdonecv;

	/* Mutex for mutable state */
	slock_t		mutex;

	/* Mutable state */
	int			nparticipantsdone;
	double		reltuples;
	double		indtuples;

#ifdef IVFFLAT_KMEANS_DEBUG
	double		inertia;
#endif
}			IvfflatShared;

#define ParallelTableScanFromIvfflatShared(shared) \
	(ParallelTableScanDesc) ((char *) (shared) + BUFFERALIGN(sizeof(IvfflatShared)))

typedef struct IvfflatLeader
{
	ParallelContext *pcxt;
	int			nparticipanttuplesorts;
	IvfflatShared *ivfshared;
	Sharedsort *sharedsort;
	Snapshot	snapshot;
	char	   *ivfcenters;
}			IvfflatLeader;

/*
 * IvfflatTypeInfo - 类型相关的操作回调接口
 *
 * 不同向量类型（vector/halfvec/sparsevec）有不同的存储大小和
 * 聚类更新方式，通过此结构体实现多态：
 *   normalize   - 归一化函数（余弦距离时用）
 *   itemSize    - 计算存储一个该类型向量的字节数
 *   updateCenter - 用已有向量数据更新聚类中心
 *   sumCenter   - 将向量累加到聚类中心（用于 k-means 均值计算）
 */
typedef struct IvfflatTypeInfo
{
	int			maxDimensions;
	Datum		(*normalize) (PG_FUNCTION_ARGS);
	Size		(*itemSize) (int dimensions);
	void		(*updateCenter) (Pointer v, int dimensions, float *x);
	void		(*sumCenter) (Pointer v, float *x);
}			IvfflatTypeInfo;

typedef struct IvfflatBuildState
{
	/* Info */
	Relation	heap;
	Relation	index;
	IndexInfo  *indexInfo;
	const		IvfflatTypeInfo *typeInfo;
	TupleDesc	tupdesc;

	/* Settings */
	int			dimensions;
	int			lists;

	/* Statistics */
	double		indtuples;
	double		reltuples;

	/* Support functions */
	FmgrInfo   *procinfo;
	FmgrInfo   *normprocinfo;
	FmgrInfo   *kmeansnormprocinfo;
	Oid			collation;

	/* Variables */
	VectorArray samples;
	VectorArray centers;
	ListInfo   *listInfo;

#ifdef IVFFLAT_KMEANS_DEBUG
	double		inertia;
	double	   *listSums;
	int		   *listCounts;
#endif

	/* Sampling */
	BlockSamplerData bs;
	ReservoirStateData rstate;
	int			rowstoskip;

	/* Sorting */
	Tuplesortstate *sortstate;
	TupleDesc	sortdesc;
	TupleTableSlot *slot;

	/* Memory */
	MemoryContext tmpCtx;

	/* Parallel builds */
	IvfflatLeader *ivfleader;
}			IvfflatBuildState;

typedef struct IvfflatMetaPageData
{
	uint32		magicNumber;
	uint32		version;
	uint16		dimensions;
	uint16		lists;
}			IvfflatMetaPageData;

typedef IvfflatMetaPageData * IvfflatMetaPage;

typedef struct IvfflatPageOpaqueData
{
	BlockNumber nextblkno;
	uint16		unused;
	uint16		page_id;		/* for identification of IVFFlat indexes */
}			IvfflatPageOpaqueData;

typedef IvfflatPageOpaqueData * IvfflatPageOpaque;

typedef struct IvfflatListData
{
	BlockNumber startPage;
	BlockNumber insertPage;
	Vector		center;
}			IvfflatListData;

typedef IvfflatListData * IvfflatList;

typedef struct IvfflatScanList
{
	pairingheap_node ph_node;
	BlockNumber startPage;
	double		distance;
}			IvfflatScanList;

typedef struct IvfflatScanOpaqueData
{
	const		IvfflatTypeInfo *typeInfo;
	int			probes;
	int			maxProbes;
	int			dimensions;
	bool		first;
	Datum		value;
	MemoryContext tmpCtx;

	/* Sorting */
	Tuplesortstate *sortstate;
	TupleDesc	tupdesc;
	TupleTableSlot *vslot;
	TupleTableSlot *mslot;
	BufferAccessStrategy bas;

	/* Support functions */
	FmgrInfo   *procinfo;
	FmgrInfo   *normprocinfo;
	Oid			collation;
	Datum		(*distfunc) (FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2);

	/* Lists */
	pairingheap *listQueue;
	BlockNumber *listPages;
	int			listIndex;
	IvfflatScanList *lists;
}			IvfflatScanOpaqueData;

typedef IvfflatScanOpaqueData * IvfflatScanOpaque;

#define VECTOR_ARRAY_SIZE(_length, _size) (sizeof(VectorArrayData) + (_length) * MAXALIGN(_size))

/* Use functions instead of macros to avoid double evaluation */

static inline Pointer
VectorArrayGet(VectorArray arr, int offset)
{
	return ((char *) arr->items) + (offset * arr->itemsize);
}

static inline void
VectorArraySet(VectorArray arr, int offset, Pointer val)
{
	memcpy(VectorArrayGet(arr, offset), val, VARSIZE_ANY(val));
}

/* Methods */
VectorArray VectorArrayInit(int maxlen, int dimensions, Size itemsize);
void		VectorArrayFree(VectorArray arr);
void		IvfflatKmeans(Relation index, VectorArray samples, VectorArray centers, const IvfflatTypeInfo * typeInfo);
FmgrInfo   *IvfflatOptionalProcInfo(Relation index, uint16 procnum);
Datum		IvfflatNormValue(const IvfflatTypeInfo * typeInfo, Oid collation, Datum value);
bool		IvfflatCheckNorm(FmgrInfo *procinfo, Oid collation, Datum value);
int			IvfflatGetLists(Relation index);
void		IvfflatGetMetaPageInfo(Relation index, int *lists, int *dimensions);
void		IvfflatUpdateList(Relation index, ListInfo listInfo, BlockNumber insertPage, BlockNumber originalInsertPage, BlockNumber startPage, ForkNumber forkNum);
void		IvfflatCommitBuffer(Buffer buf, GenericXLogState *state);
void		IvfflatAppendPage(Relation index, Buffer *buf, Page *page, GenericXLogState **state, ForkNumber forkNum);
Buffer		IvfflatNewBuffer(Relation index, ForkNumber forkNum);
void		IvfflatInitPage(Buffer buf, Page page);
void		IvfflatInitRegisterPage(Relation index, Buffer *buf, Page *page, GenericXLogState **state);
void		IvfflatInit(void);
const		IvfflatTypeInfo *IvfflatGetTypeInfo(Relation index);
PGDLLEXPORT void IvfflatParallelBuildMain(dsm_segment *seg, shm_toc *toc);

/* Index access methods */
IndexBuildResult *ivfflatbuild(Relation heap, Relation index, IndexInfo *indexInfo);
void		ivfflatbuildempty(Relation index);
bool		ivfflatinsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap, IndexUniqueCheck checkUnique
#if PG_VERSION_NUM >= 140000
						  ,bool indexUnchanged
#endif
						  ,IndexInfo *indexInfo
);
IndexBulkDeleteResult *ivfflatbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state);
IndexBulkDeleteResult *ivfflatvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
IndexScanDesc ivfflatbeginscan(Relation index, int nkeys, int norderbys);
void		ivfflatrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);
bool		ivfflatgettuple(IndexScanDesc scan, ScanDirection dir);
void		ivfflatendscan(IndexScanDesc scan);

#endif
