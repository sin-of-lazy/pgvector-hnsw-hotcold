/*
 * ivfvacuum.c - IVFFlat 索引的 VACUUM 实现
 *
 * IVFFlat 的 VACUUM 比 HNSW 简单，因为 IVFFlat 是倒排列表结构，
 * 删除一个向量不影响其他向量的查找路径（无图连接需要维护）。
 *
 * ivfflatbulkdelete：
 *   扫描所有向量数据页，对每个 (heap TID, 向量) 对：
 *   - 若 callback(tid) 返回 true，则从页中删除该条目
 *   - 使用 PageIndexDeleteNoCompact 标记删除（不立即整理页内空间）
 *
 * ivfflatvacuumcleanup：
 *   更新统计信息（dead tuples、页数等）。
 *   IVFFlat 不需要像 HNSW 那样修复邻居关系，
 *   因为倒排列表中的条目互相独立。
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/itup.h"
#include "commands/vacuum.h"
#include "ivfflat.h"
#include "storage/bufmgr.h"
#include "utils/relcache.h"

#if PG_VERSION_NUM >= 180000
#define vacuum_delay_point() vacuum_delay_point(false)
#endif

/*
 * Bulk delete tuples from the index
 */
IndexBulkDeleteResult *
ivfflatbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				  IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	BlockNumber blkno = IVFFLAT_HEAD_BLKNO;
	BufferAccessStrategy bas = GetAccessStrategy(BAS_BULKREAD);

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/* Iterate over list pages */
	while (BlockNumberIsValid(blkno))
	{
		Buffer		cbuf;
		Page		cpage;
		OffsetNumber coffno;
		OffsetNumber cmaxoffno;
		BlockNumber listPages[MaxOffsetNumber];
		ListInfo	listInfo;

		cbuf = ReadBuffer(index, blkno);
		LockBuffer(cbuf, BUFFER_LOCK_SHARE);
		cpage = BufferGetPage(cbuf);

		cmaxoffno = PageGetMaxOffsetNumber(cpage);

		/* Iterate over lists */
		for (coffno = FirstOffsetNumber; coffno <= cmaxoffno; coffno = OffsetNumberNext(coffno))
		{
			IvfflatList list = (IvfflatList) PageGetItem(cpage, PageGetItemId(cpage, coffno));

			listPages[coffno - FirstOffsetNumber] = list->startPage;
		}

		listInfo.blkno = blkno;
		blkno = IvfflatPageGetOpaque(cpage)->nextblkno;

		UnlockReleaseBuffer(cbuf);

		for (coffno = FirstOffsetNumber; coffno <= cmaxoffno; coffno = OffsetNumberNext(coffno))
		{
			BlockNumber searchPage = listPages[coffno - FirstOffsetNumber];
			BlockNumber insertPage = InvalidBlockNumber;

			/* Iterate over entry pages */
			while (BlockNumberIsValid(searchPage))
			{
				Buffer		buf;
				Page		page;
				GenericXLogState *state;
				OffsetNumber offno;
				OffsetNumber maxoffno;
				OffsetNumber deletable[MaxOffsetNumber];
				int			ndeletable;

				vacuum_delay_point();

				buf = ReadBufferExtended(index, MAIN_FORKNUM, searchPage, RBM_NORMAL, bas);

				/*
				 * ambulkdelete cannot delete entries from pages that are
				 * pinned by other backends
				 *
				 * https://www.postgresql.org/docs/current/index-locking.html
				 */
				LockBufferForCleanup(buf);

				state = GenericXLogStart(index);
				page = GenericXLogRegisterBuffer(state, buf, 0);

				maxoffno = PageGetMaxOffsetNumber(page);
				ndeletable = 0;

				/* Find deleted tuples */
				for (offno = FirstOffsetNumber; offno <= maxoffno; offno = OffsetNumberNext(offno))
				{
					IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offno));
					ItemPointer htup = &(itup->t_tid);

					if (callback(htup, callback_state))
					{
						deletable[ndeletable++] = offno;
						stats->tuples_removed++;
					}
					else
						stats->num_index_tuples++;
				}

				/* Set to first free page */
				/* Must be set before searchPage is updated */
				if (!BlockNumberIsValid(insertPage) && ndeletable > 0)
					insertPage = searchPage;

				searchPage = IvfflatPageGetOpaque(page)->nextblkno;

				if (ndeletable > 0)
				{
					/* Delete tuples */
					PageIndexMultiDelete(page, deletable, ndeletable);
					GenericXLogFinish(state);
				}
				else
					GenericXLogAbort(state);

				UnlockReleaseBuffer(buf);
			}

			/*
			 * Update after all tuples deleted.
			 *
			 * We don't add or delete items from lists pages, so offset won't
			 * change.
			 */
			if (BlockNumberIsValid(insertPage))
			{
				listInfo.offno = coffno;
				IvfflatUpdateList(index, listInfo, insertPage, InvalidBlockNumber, InvalidBlockNumber, MAIN_FORKNUM);
			}
		}
	}

	FreeAccessStrategy(bas);

	return stats;
}

/*
 * Clean up after a VACUUM operation
 */
IndexBulkDeleteResult *
ivfflatvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	rel = info->index;

	if (info->analyze_only)
		return stats;

	/* stats is NULL if ambulkdelete not called */
	/* OK to return NULL if index not changed */
	if (stats == NULL)
		return NULL;

	stats->num_pages = RelationGetNumberOfBlocks(rel);

	return stats;
}
