/*-------------------------------------------------------------------------
 *
 * ztqual.c
 *	  POSTGRES "time qualification" code, ie, ztuple visibility rules.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/time/ztqual.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "access/zheap.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/tqual.h"
#include "utils/ztqual.h"
#include "storage/proc.h"

/*
 * GetTupleFromUndo
 *
 *	Fetch the record from undo and determine if previous version of tuple
 *	is visible for the given snapshot.  If there exists a visible version
 *	of tuple in undo, then return the same, else return NULL.
 *
 *	During undo chain traversal, we need to ensure that we switch the undo
 *	chain if the current version of undo tuple is modified by a transaction
 *	that is different from transaction that has modified the previous version
 *	of undo tuple.  This is primarily done because undo chain for a particular
 *	tuple is formed based on the transaction id that has modified the tuple.
 *
 *	Also we don't need to process the chain if the latest xid that has changed
 *  the tuple precedes smallest xid that has undo.
 */
static ZHeapTuple
GetTupleFromUndo(UndoRecPtr urec_ptr, ZHeapTuple zhtup, Snapshot snapshot,
				 Buffer buffer, TransactionId prev_undo_xid)
{
	UnpackedUndoRecord	*urec;
	ZHeapPageOpaque	opaque;
	ZHeapTuple	undo_tup;
	UndoRecPtr	prev_urec_ptr = InvalidUndoRecPtr;
	TransactionId	xid;
	CommandId	cid = InvalidCommandId;
	int	trans_slot_id = InvalidXactSlotId;
	int	prev_trans_slot_id = ZHeapTupleHeaderGetXactSlot(zhtup->t_data);
	int	undo_oper = -1;

	opaque = (ZHeapPageOpaque) PageGetSpecialPointer(BufferGetPage(buffer));

	/*
	 * tuple is modified after the scan is started, fetch the prior record
	 * from undo to see if it is visible.
	 */
fetch_undo_record:
	urec = UndoFetchRecord(urec_ptr,
						   ItemPointerGetBlockNumber(&zhtup->t_self),
						   ItemPointerGetOffsetNumber(&zhtup->t_self),
						   prev_undo_xid);

	/*
	 * Skip the undo record for transaction slot reuse, it is used only for
	 * the purpose of fetching transaction information for tuples that point
	 * to such slots.
	 */
	if (urec->uur_type == UNDO_INVALID_XACT_SLOT)
	{
		urec_ptr = urec->uur_blkprev;
		UndoRecordRelease(urec);

		goto fetch_undo_record;
	}

	undo_tup = CopyTupleFromUndoRecord(urec, zhtup, true);
	trans_slot_id = ZHeapTupleHeaderGetXactSlot(undo_tup->t_data);
	prev_urec_ptr = urec->uur_blkprev;
	xid = urec->uur_prevxid;

	if (undo_tup->t_data->t_infomask & ZHEAP_INPLACE_UPDATED)
	{
		undo_oper = ZHEAP_INPLACE_UPDATED;
	}
	else if (undo_tup->t_data->t_infomask & ZHEAP_XID_LOCK_ONLY)
	{
		undo_oper = ZHEAP_XID_LOCK_ONLY;
	}
	else
	{
		/* we can't further operate on deleted or non-inplace-updated tuple */
		Assert(!(undo_tup->t_data->t_infomask & ZHEAP_DELETED) ||
			   !(undo_tup->t_data->t_infomask & ZHEAP_UPDATED));
	}

	UndoRecordRelease(urec);

	/*
	 * Change the undo chain if the undo tuple is stamped with the different
	 * transaction slot.
	 */
	if (trans_slot_id != ZHTUP_SLOT_FROZEN &&
		trans_slot_id != prev_trans_slot_id)
		prev_urec_ptr = ZHeapTupleHeaderGetRawUndoPtr(undo_tup->t_data, opaque);

	/*
	 * We need to fetch all the transaction related information from undo
	 * record for the tuples that point to a slot that gets invalidated for
	 * reuse at some point of time.  See PageFreezeTransSlots.
	 */
	if ((ZHeapTupleHeaderGetXactSlot(undo_tup->t_data) != ZHTUP_SLOT_FROZEN)
		&& !TransactionIdPrecedes(xid, RecentGlobalXmin))
	{
		if (undo_tup->t_data->t_infomask & ZHEAP_INVALID_XACT_SLOT)
		{
			uint8	uur_type;
			TransactionId	undo_tup_xid;

			undo_tup_xid = xid;

			do
			{
				urec = UndoFetchRecord(prev_urec_ptr,
									   ItemPointerGetBlockNumber(&undo_tup->t_self),
									   ItemPointerGetOffsetNumber(&undo_tup->t_self),
									   InvalidTransactionId);

				/*
				 * The undo tuple must be visible, if the undo record containing
				 * the information of the last transaction that has updated the
				 * tuple is discarded or the transaction that has last updated the
				 * undo tuple precedes RecentGlobalXmin.
				 */
				if (urec == NULL ||
					TransactionIdPrecedes(urec->uur_prevxid, RecentGlobalXmin))
				{
					xid = InvalidTransactionId;
					cid = InvalidCommandId;

					if (urec)
						UndoRecordRelease(urec);
					break;
				}

				xid = urec->uur_prevxid;
				cid = urec->uur_cid;
				prev_urec_ptr = urec->uur_blkprev;
				uur_type = urec->uur_type;

				/*
				 * transaction slot won't change for such a tuple, so we can rely on
				 * the same from current undo tuple.
				 */

				UndoRecordRelease(urec);
			} while (uur_type != UNDO_INVALID_XACT_SLOT || undo_tup_xid != xid);
		}
		else
		{
			/*
 			 * we don't use prev_undo_xid to fetch the undo record for cid as it is
 			 * required only when transaction is current transaction in which case
 			 * there is no risk of transaction chain switching, so we are safe.  It
 			 * might be better to move this check near to it's usage, but that will
 			 * make code look ugly, so keeping it here.  
 			 */
			cid = ZHeapTupleGetCid(undo_tup, buffer);
		}
	}

	/*
	 * The tuple must be all visible if the transaction slot is cleared or
	 * latest xid that has changed the tuple precedes smallest xid that has
	 * undo.
	 */
	if (trans_slot_id == ZHTUP_SLOT_FROZEN ||
		TransactionIdPrecedes(xid, RecentGlobalXmin))
		return undo_tup;

	if (undo_oper == ZHEAP_INPLACE_UPDATED ||
		undo_oper == ZHEAP_XID_LOCK_ONLY)
	{
		if (TransactionIdIsCurrentTransactionId(xid))
		{
			if (undo_oper == ZHEAP_XID_LOCK_ONLY)
				return undo_tup;
			if (cid >= snapshot->curcid)
			{
				/* updated after scan started */
				return GetTupleFromUndo(prev_urec_ptr,
										undo_tup,
										snapshot,
										buffer,
										xid);
			}
			else
				return undo_tup;	/* updated before scan started */
		}
		else if (XidInMVCCSnapshot(xid, snapshot))
			return GetTupleFromUndo(prev_urec_ptr,
									undo_tup,
									snapshot,
									buffer,
									xid);
		else if (TransactionIdDidCommit(xid))
			return undo_tup;
		else
			return GetTupleFromUndo(prev_urec_ptr,
									undo_tup,
									snapshot,
									buffer,
									xid);
	}
	else	/* undo tuple is the root tuple */
	{
		if (TransactionIdIsCurrentTransactionId(xid))
		{
			if (cid >= snapshot->curcid)
				return NULL;	/* inserted after scan started */
			else
				return undo_tup;	/* inserted before scan started */
		}
		else if (XidInMVCCSnapshot(xid, snapshot))
			return NULL;
		else if (TransactionIdDidCommit(xid))
			return undo_tup;
		else
			return NULL;
	}
}

/*
 * UndoTupleSatisfiesUpdate
 *
 *	Returns true, if there exists a visible version of zhtup in undo,
 *	false otherwise.
 *
 *	This function returns ctid for the undo tuple which will be always
 *	same as the ctid of zhtup except for non-in-place update case.
 *
 *	The Undo chain traversal follows similar protocol as mentioned atop
 *	GetTupleFromUndo.
 */
static bool
UndoTupleSatisfiesUpdate(UndoRecPtr urec_ptr, ZHeapTuple zhtup,
						 CommandId curcid, Buffer buffer,
						 ItemPointer ctid, TransactionId  prev_undo_xid,
						 bool free_zhtup, bool *in_place_updated_or_locked)
{
	UnpackedUndoRecord	*urec;
	ZHeapPageOpaque	opaque;
	ZHeapTuple	undo_tup = NULL;
	UndoRecPtr	prev_urec_ptr = InvalidUndoRecPtr;
	TransactionId	xid;
	CommandId	cid = InvalidCommandId;
	int	trans_slot_id = InvalidXactSlotId;
	int prev_trans_slot_id = ZHeapTupleHeaderGetXactSlot(zhtup->t_data);
	int	undo_oper = -1;
	bool result = false;

	opaque = (ZHeapPageOpaque) PageGetSpecialPointer(BufferGetPage(buffer));

	/*
	 * tuple is modified after the scan is started, fetch the prior record
	 * from undo to see if it is visible.
	 */
fetch_undo_record:
	urec = UndoFetchRecord(urec_ptr,
						   ItemPointerGetBlockNumber(&zhtup->t_self),
						   ItemPointerGetOffsetNumber(&zhtup->t_self),
						   prev_undo_xid);

	/*
	 * Skip the undo record for transaction slot reuse, it is used only for
	 * the purpose of fetching transaction information for tuples that point
	 * to such slots.
	 */
	if (urec->uur_type == UNDO_INVALID_XACT_SLOT)
	{
		urec_ptr = urec->uur_blkprev;
		UndoRecordRelease(urec);

		goto fetch_undo_record;
	}

	undo_tup = CopyTupleFromUndoRecord(urec, zhtup, free_zhtup);
	trans_slot_id = ZHeapTupleHeaderGetXactSlot(undo_tup->t_data);
	prev_urec_ptr = urec->uur_blkprev;
	xid = urec->uur_prevxid;
	/*
	 * For non-inplace-updates, ctid needs to be retrieved from undo
	 * record if required.
	 */
	if (ctid)
	{
		if (urec->uur_type == UNDO_UPDATE)
			*ctid = *((ItemPointer) urec->uur_payload.data);
		else
			*ctid = undo_tup->t_self;
	}

	if (undo_tup->t_data->t_infomask & ZHEAP_INPLACE_UPDATED)
	{
		undo_oper = ZHEAP_INPLACE_UPDATED;
		*in_place_updated_or_locked = true;
	}
	else if (undo_tup->t_data->t_infomask & ZHEAP_XID_LOCK_ONLY)
	{
		undo_oper = ZHEAP_XID_LOCK_ONLY;
		*in_place_updated_or_locked = true;
	}
	else
	{
		/* we can't further operate on deleted or non-inplace-updated tuple */
		Assert(!(undo_tup->t_data->t_infomask & ZHEAP_DELETED) ||
			   !(undo_tup->t_data->t_infomask & ZHEAP_UPDATED));
	}

	UndoRecordRelease(urec);

	/*
	 * Change the undo chain if the undo tuple is stamped with the different
	 * transaction slot.
	 */
	if (trans_slot_id != ZHTUP_SLOT_FROZEN &&
		trans_slot_id != prev_trans_slot_id)
		prev_urec_ptr = ZHeapTupleHeaderGetRawUndoPtr(undo_tup->t_data, opaque);

	/*
	 * We need to fetch all the transaction related information from undo
	 * record for the tuples that point to a slot that gets invalidated for
	 * reuse at some point of time.  See PageFreezeTransSlots.
	 */
	if ((ZHeapTupleHeaderGetXactSlot(undo_tup->t_data) != ZHTUP_SLOT_FROZEN)
		&& !TransactionIdPrecedes(xid, RecentGlobalXmin))
	{
		if (undo_tup->t_data->t_infomask & ZHEAP_INVALID_XACT_SLOT)
		{
			uint8	uur_type;
			TransactionId	undo_tup_xid;

			undo_tup_xid = xid;

			do
			{
				urec = UndoFetchRecord(prev_urec_ptr,
									   ItemPointerGetBlockNumber(&undo_tup->t_self),
									   ItemPointerGetOffsetNumber(&undo_tup->t_self),
									   InvalidTransactionId);

				/*
				 * The undo tuple must be visible, if the undo record containing
				 * the information of the last transaction that has updated the
				 * tuple is discarded or the transaction that has last updated the
				 * undo tuple precedes RecentGlobalXmin.
				 */
				if (urec == NULL ||
					TransactionIdPrecedes(urec->uur_prevxid, RecentGlobalXmin))
				{
					xid = InvalidTransactionId;
					cid = InvalidCommandId;

					if (urec)
						UndoRecordRelease(urec);
					break;
				}

				xid = urec->uur_prevxid;
				cid = urec->uur_cid;
				prev_urec_ptr = urec->uur_blkprev;
				uur_type = urec->uur_type;

				/*
				 * transaction slot won't change for such a tuple, so we can rely on
				 * the same from current undo tuple.
				 */

				UndoRecordRelease(urec);
			} while (uur_type != UNDO_INVALID_XACT_SLOT || undo_tup_xid != xid);
		}
		else
		{
			/*
 			 * we don't use prev_undo_xid to fetch the undo record for cid as it is
 			 * required only when transaction is current transaction in which case
 			 * there is no risk of transaction chain switching, so we are safe.  It
 			 * might be better to move this check near to it's usage, but that will
 			 * make code look ugly, so keeping it here.  
 			 */
			cid = ZHeapTupleGetCid(undo_tup, buffer);
		}
	}

	/*
	 * The tuple must be all visible if the transaction slot is cleared or
	 * latest xid that has changed the tuple precedes smallest xid that has
	 * undo.
	 */
	if (trans_slot_id == ZHTUP_SLOT_FROZEN ||
		TransactionIdPrecedes(xid, RecentGlobalXmin))
	{
		result = true;
		goto result_available;
	}

	if (undo_oper == ZHEAP_INPLACE_UPDATED ||
		undo_oper == ZHEAP_XID_LOCK_ONLY)
	{
		if (TransactionIdIsCurrentTransactionId(xid))
		{
			if (undo_oper == ZHEAP_XID_LOCK_ONLY)
			{
				result = true;
				goto result_available;
			}
			if (cid >= curcid)
			{
				/* updated after scan started */
				return UndoTupleSatisfiesUpdate(prev_urec_ptr,
												undo_tup,
												curcid,
												buffer,
												ctid,
												xid,
												true,
												in_place_updated_or_locked);
			}
			else
				result = true;	/* updated before scan started */
		}
		else if (TransactionIdIsInProgress(xid))
			return UndoTupleSatisfiesUpdate(prev_urec_ptr,
											undo_tup,
											curcid,
											buffer,
											ctid,
											xid,
											true,
											in_place_updated_or_locked);
		else if (TransactionIdDidCommit(xid))
			result = true;
		else
			return UndoTupleSatisfiesUpdate(prev_urec_ptr,
											undo_tup,
											curcid,
											buffer,
											ctid,
											xid,
											true,
											in_place_updated_or_locked);
	}
	else	/* undo tuple is the root tuple */
	{
		if (TransactionIdIsCurrentTransactionId(xid))
		{
			if (cid >= curcid)
				result = false;	/* inserted after scan started */
			else
				result = true;	/* inserted before scan started */
		}
		else if (TransactionIdIsInProgress(xid))
			result = false;
		else if (TransactionIdDidCommit(xid))
			result = true;
		else
			result = false;
	}

result_available:
	if (undo_tup)
		pfree(undo_tup);
	return result;
}

/*
 * ZHeapTupleSatisfiesMVCC
 *
 *	Returns the visible version of tuple if any, NULL otherwise. We need to
 *	traverse undo record chains to determine the visibility of tuple.  In
 *	this function we need to first the determine the visibility of modified
 *	tuple and if it is not visible, then we need to fetch the prior version
 *	of tuple from undo chain and decide based on its visibility.  The undo
 *	chain needs to be traversed till we reach root version of the tuple.
 *
 *	Here, we consider the effects of:
 *		all transactions committed as of the time of the given snapshot
 *		previous commands of this transaction
 *
 *	Does _not_ include:
 *		transactions shown as in-progress by the snapshot
 *		transactions started after the snapshot was taken
 *		changes made by the current command
 *
 *	The tuple will be considered visible iff latest operation on tuple is
 *	Insert, In-Place update or tuple is locked and the transaction that has
 *	performed operation is current transaction (and the operation is performed
 *	by some previous command) or is committed.
 *
 *	We traverse the undo chain to get the visible tuple if any, in case the
 *	the latest transaction that has operated on tuple is shown as in-progress
 *	by the snapshot or is started after the snapshot was taken or is current
 *	transaction and the changes are made by current command.
 */
ZHeapTuple
ZHeapTupleSatisfiesMVCC(ZHeapTuple zhtup, Snapshot snapshot,
						Buffer buffer, ItemPointer ctid)
{
	ZHeapPageOpaque	opaque;
	ZHeapTupleHeader tuple = zhtup->t_data;
	UnpackedUndoRecord	*urec;
	UndoRecPtr	urec_ptr = InvalidUndoRecPtr;
	TransactionId	xid;
	CommandId		cid = InvalidCommandId;

	opaque = (ZHeapPageOpaque) PageGetSpecialPointer(BufferGetPage(buffer));

	Assert(ItemPointerIsValid(&zhtup->t_self));
	Assert(zhtup->t_tableOid != InvalidOid);

	/*
	 * We need to fetch all the transaction related information from undo
	 * record for the tuples that point to a slot that gets invalidated for
	 * reuse at some point of time.  See PageFreezeTransSlots.
	 */
	if (ZHeapTupleHeaderGetXactSlot(tuple) != ZHTUP_SLOT_FROZEN)
	{
		if (tuple->t_infomask & ZHEAP_INVALID_XACT_SLOT)
		{
			uint8	uur_type;

			urec_ptr = ZHeapTupleHeaderGetRawUndoPtr(tuple, opaque);

			do
			{
				urec = UndoFetchRecord(urec_ptr,
									   ItemPointerGetBlockNumber(&zhtup->t_self),
									   ItemPointerGetOffsetNumber(&zhtup->t_self),
									   InvalidTransactionId);

				/*
				 * The undo tuple must be visible, if the undo record containing
				 * the information of the last transaction that has updated the
				 * tuple is discarded or the transaction that has last updated the
				 * undo tuple precedes RecentGlobalXmin.
				 */
				if (urec == NULL)
				{
					xid = InvalidTransactionId;
					cid = InvalidCommandId;
					break;
				}

				xid = urec->uur_prevxid;
				cid = urec->uur_cid;
				urec_ptr = urec->uur_blkprev;
				uur_type = urec->uur_type;

				/*
				 * transaction slot won't change for such a tuple, so we can rely on
				 * the same from current undo tuple.
				 */

				UndoRecordRelease(urec);
			} while (uur_type != UNDO_INVALID_XACT_SLOT);
		}
		else
		{
			xid = ZHeapTupleHeaderGetRawXid(tuple, opaque);
			cid = ZHeapTupleGetCid(zhtup, buffer);
			urec_ptr = ZHeapTupleHeaderGetRawUndoPtr(tuple, opaque);
		}
	}
	else
		xid = InvalidTransactionId;

	if (tuple->t_infomask & ZHEAP_DELETED ||
		tuple->t_infomask & ZHEAP_UPDATED)
	{
		/*
		 * The tuple is deleted and must be all visible if the transaction slot
		 * is cleared or latest xid that has changed the tuple precedes
		 * smallest xid that has undo.
		 */
		if (ZHeapTupleHeaderGetXactSlot(tuple) == ZHTUP_SLOT_FROZEN ||
			TransactionIdPrecedes(xid, RecentGlobalXmin))
			return NULL;

		if (TransactionIdIsCurrentTransactionId(xid))
		{
			if (cid >= snapshot->curcid)
			{
				/* deleted after scan started, get previous tuple from undo */
				return GetTupleFromUndo(urec_ptr,
										zhtup,
										snapshot,
										buffer,
										InvalidTransactionId);
			}
			else
				return NULL;	/* deleted before scan started */
		}
		else if (XidInMVCCSnapshot(xid, snapshot))
			return GetTupleFromUndo(urec_ptr,
									zhtup,
									snapshot,
									buffer,
									InvalidTransactionId);
		else if (TransactionIdDidCommit(xid))
			return NULL;	/* tuple is deleted */
		else	/* transaction is aborted */
			return GetTupleFromUndo(urec_ptr,
									zhtup,
									snapshot,
									buffer,
									InvalidTransactionId);
	}
	else if (tuple->t_infomask & ZHEAP_INPLACE_UPDATED ||
			 tuple->t_infomask & ZHEAP_XID_LOCK_ONLY)
	{
		/*
		 * The tuple is updated/locked and must be all visible if the
		 * transaction slot is cleared or latest xid that has changed the
		 * tuple precedes smallest xid that has undo.
		 */
		if (ZHeapTupleHeaderGetXactSlot(tuple) == ZHTUP_SLOT_FROZEN ||
			TransactionIdPrecedes(xid, RecentGlobalXmin))
			return zhtup;	/* tuple is updated */

		if (TransactionIdIsCurrentTransactionId(xid))
		{
			if (ZHEAP_XID_IS_LOCKED_ONLY(tuple->t_infomask))
				return zhtup;
			if (cid >= snapshot->curcid)
			{
				/* updated after scan started, get previous tuple from undo */
				return GetTupleFromUndo(urec_ptr,
										zhtup,
										snapshot,
										buffer,
										InvalidTransactionId);
			}
			else
				return zhtup;	/* updated before scan started */
		}
		else if (XidInMVCCSnapshot(xid, snapshot))
			return GetTupleFromUndo(urec_ptr,
									zhtup,
									snapshot,
									buffer,
									InvalidTransactionId);
		else if (TransactionIdDidCommit(xid))
			return zhtup;	/* tuple is updated */
		else	/* transaction is aborted */
			return GetTupleFromUndo(urec_ptr,
									zhtup,
									snapshot,
									buffer,
									InvalidTransactionId);
	}

	/*
	 * The tuple must be all visible if the transaction slot
	 * is cleared or latest xid that has changed the tuple precedes
	 * smallest xid that has undo.
	 */
	if (ZHeapTupleHeaderGetXactSlot(tuple) == ZHTUP_SLOT_FROZEN ||
		TransactionIdPrecedes(xid, RecentGlobalXmin))
		return zhtup;

	if (TransactionIdIsCurrentTransactionId(xid))
	{
		if (cid >= snapshot->curcid)
			return NULL;	/* inserted after scan started */
		else
			return zhtup;	/* inserted before scan started */
	}
	else if (XidInMVCCSnapshot(xid, snapshot))
		return NULL;
	else if (TransactionIdDidCommit(xid))
		return zhtup;
	else
		return NULL;

	return NULL;
}

/*
 * ZHeapTupleSatisfiesUpdate
 *
 *	The retrun values for this API are same as HeapTupleSatisfiesUpdate.
 *	However, there is a notable difference in the way to determine visibility
 *	of tuples.  We need to traverse undo record chains to determine the
 *	visibility of tuple.
 *
 *	ctid - returns the ctid of visible tuple if the tuple is either deleted or
 *	updated.  ctid needs to be retrieved from undo tuple.
 *	xid - returns the xid that has modified the visible tuple.
 *	cid - returns the cid of visible tuple.
 *	lock_allowed - allow caller to lock the tuple if it is in-place updated
 *	in_place_updated - returns whether the current visible version of tuple is
 *	updated in place.
 */
HTSU_Result
ZHeapTupleSatisfiesUpdate(ZHeapTuple zhtup, CommandId curcid,
						  Buffer buffer, ItemPointer ctid, TransactionId *xid,
						  CommandId *cid, bool free_zhtup, bool lock_allowed,
						  Snapshot snapshot, bool *in_place_updated_or_locked)
{
	ZHeapPageOpaque	opaque;
	ZHeapTupleHeader tuple = zhtup->t_data;
	UnpackedUndoRecord	*urec;
	UndoRecPtr	urec_ptr = InvalidUndoRecPtr;
	bool	visible;

	opaque = (ZHeapPageOpaque) PageGetSpecialPointer(BufferGetPage(buffer));
	*in_place_updated_or_locked = false;

	Assert(ItemPointerIsValid(&zhtup->t_self));
	Assert(zhtup->t_tableOid != InvalidOid);

	/*
	 * We need to fetch all the transaction related information from undo
	 * record for the tuples that point to a slot that gets invalidated for
	 * reuse at some point of time.  See PageFreezeTransSlots.
	 */
	if (ZHeapTupleHeaderGetXactSlot(tuple) != ZHTUP_SLOT_FROZEN)
	{
		if (tuple->t_infomask & ZHEAP_INVALID_XACT_SLOT)
		{
			uint8	uur_type;

			urec_ptr = ZHeapTupleHeaderGetRawUndoPtr(tuple, opaque);

			do
			{
				urec = UndoFetchRecord(urec_ptr,
									   ItemPointerGetBlockNumber(&zhtup->t_self),
									   ItemPointerGetOffsetNumber(&zhtup->t_self),
									   InvalidTransactionId);

				/*
				 * The undo tuple must be visible, if the undo record containing
				 * the information of the last transaction that has updated the
				 * tuple is discarded or the transaction that has last updated the
				 * undo tuple precedes RecentGlobalXmin.
				 */
				if (urec == NULL)
				{
					*xid = InvalidTransactionId;
					*cid = InvalidCommandId;
					break;
				}

				*xid = urec->uur_prevxid;
				*cid = urec->uur_cid;
				urec_ptr = urec->uur_blkprev;
				uur_type = urec->uur_type;

				/*
				 * transaction slot won't change for such a tuple, so we can rely on
				 * the same from current undo tuple.
				 */

				UndoRecordRelease(urec);
			} while (uur_type != UNDO_INVALID_XACT_SLOT);
		}
		else
		{
			*xid = ZHeapTupleHeaderGetRawXid(tuple, opaque);
			*cid = ZHeapTupleGetCid(zhtup, buffer);
			urec_ptr = ZHeapTupleHeaderGetRawUndoPtr(tuple, opaque);
		}
	}
	else
		*xid = InvalidTransactionId;

	if (tuple->t_infomask & ZHEAP_DELETED ||
		tuple->t_infomask & ZHEAP_UPDATED)
	{
		/*
		 * The tuple is deleted or non-inplace-updated and must be all visible
		 * if the transaction slot is cleared or latest xid that has changed
		 * the tuple precedes smallest xid that has undo.  However, that is
		 * not possible at this stage as the tuple has already passed snapshot
		 * check.
		 */
		Assert(!(ZHeapTupleHeaderGetXactSlot(tuple) == ZHTUP_SLOT_FROZEN &&
			   TransactionIdPrecedes(*xid, RecentGlobalXmin)));

		if (TransactionIdIsCurrentTransactionId(*xid))
		{
			if (*cid >= curcid)
			{
				/* deleted after scan started, check previous tuple from undo */
				visible = UndoTupleSatisfiesUpdate(urec_ptr,
												   zhtup,
												   curcid,
												   buffer,
												   ctid,
												   InvalidTransactionId,
												   free_zhtup,
												   in_place_updated_or_locked);
				if (visible)
					return HeapTupleSelfUpdated;
				else
					return HeapTupleInvisible;
			}
			else
				return HeapTupleInvisible;	/* deleted before scan started */
		}
		else if (TransactionIdIsInProgress(*xid))
		{
			visible = UndoTupleSatisfiesUpdate(urec_ptr,
											   zhtup,
											   curcid,
											   buffer,
											   ctid,
											   InvalidTransactionId,
											   free_zhtup,
											   in_place_updated_or_locked);

			if (visible)
				return HeapTupleBeingUpdated;
			else
				return HeapTupleInvisible;
		}
		else if (TransactionIdDidCommit(*xid))
		{
			/*
			 * For non-inplace-updates, ctid needs to be retrieved from undo
			 * record if required.
			 */
			if (tuple->t_infomask & ZHEAP_UPDATED && ctid)
				ZHeapTupleGetCtid(zhtup, buffer, ctid);

			/* tuple is deleted or non-inplace-updated */
			return HeapTupleUpdated;
		}
		else	/* transaction is aborted */
		{
			/*
			 * Fixme - For aborted transactions, we should either wait for undo
			 * to be applied or apply undo by ourselves before modifying the
			 * the tuple.
			 */
			visible = UndoTupleSatisfiesUpdate(ZHeapTupleHeaderGetRawUndoPtr(tuple, opaque),
											   zhtup,
											   curcid,
											   buffer,
											   ctid,
											   InvalidTransactionId,
											   free_zhtup,
											   in_place_updated_or_locked);

			if (visible)
				return HeapTupleMayBeUpdated;
			else
				return HeapTupleInvisible;
		}
	}
	else if (tuple->t_infomask & ZHEAP_INPLACE_UPDATED ||
			 tuple->t_infomask & ZHEAP_XID_LOCK_ONLY)
	{
		*in_place_updated_or_locked = true;

		/*
		 * The tuple is updated/locked and must be all visible if the
		 * transaction slot is cleared or latest xid that has touched the
		 * tuple precedes smallest xid that has undo.
		 */
		if (ZHeapTupleHeaderGetXactSlot(tuple) == ZHTUP_SLOT_FROZEN ||
			TransactionIdPrecedes(*xid, RecentGlobalXmin))
			return HeapTupleMayBeUpdated;

		if (TransactionIdIsCurrentTransactionId(*xid))
		{
			if (ZHEAP_XID_IS_LOCKED_ONLY(tuple->t_infomask))
				return HeapTupleBeingUpdated;

			if (*cid >= curcid)
			{
				/* updated after scan started, check previous tuple from undo */
				visible = UndoTupleSatisfiesUpdate(urec_ptr,
												   zhtup,
												   curcid,
												   buffer,
												   ctid,
												   InvalidTransactionId,
												   free_zhtup,
												   in_place_updated_or_locked);
				if (visible)
					return HeapTupleSelfUpdated;
				else
					return HeapTupleInvisible;
			}
			else
				return HeapTupleMayBeUpdated;	/* updated before scan started */
		}
		else if (TransactionIdIsInProgress(*xid))
		{
			visible = UndoTupleSatisfiesUpdate(urec_ptr,
											   zhtup,
											   curcid,
											   buffer,
											   ctid,
											   InvalidTransactionId,
											   free_zhtup,
											   in_place_updated_or_locked);

			if (visible)
				return HeapTupleBeingUpdated;
			else
				return HeapTupleInvisible;
		}
		else if (TransactionIdDidCommit(*xid))
		{
			/* if tuple is updated and not in our snapshot, then allow to update it. */
			if (lock_allowed || !XidInMVCCSnapshot(*xid, snapshot))
				return HeapTupleMayBeUpdated;
			else
				return HeapTupleUpdated;
		}
		else	/* transaction is aborted */
		{
			/*
			 * Fixme - For aborted transactions, we should either wait for undo
			 * to be applied or apply undo by ourselves before modifying the
			 * the tuple.
			 */
			visible = UndoTupleSatisfiesUpdate(urec_ptr,
											   zhtup,
											   curcid,
											   buffer,
											   ctid,
											   InvalidTransactionId,
											   free_zhtup,
											   in_place_updated_or_locked);

			if (visible)
				return HeapTupleMayBeUpdated;
			else
				return HeapTupleInvisible;
		}
	}

	/*
	 * The tuple must be all visible if the transaction slot
	 * is cleared or latest xid that has changed the tuple precedes
	 * smallest xid that has undo.
	 */
	if (ZHeapTupleHeaderGetXactSlot(tuple) == ZHTUP_SLOT_FROZEN ||
		TransactionIdPrecedes(*xid, RecentGlobalXmin))
		return HeapTupleMayBeUpdated;

	if (TransactionIdIsCurrentTransactionId(*xid))
	{
		if (*cid >= curcid)
			return HeapTupleInvisible;	/* inserted after scan started */
		else
			return HeapTupleMayBeUpdated;	/* inserted before scan started */
	}
	else if (TransactionIdIsInProgress(*xid))
		return HeapTupleInvisible;
	else if (TransactionIdDidCommit(*xid))
		return HeapTupleMayBeUpdated;
	else
		return HeapTupleInvisible;

	return HeapTupleInvisible;
}

/*
 * ZHeapTupleIsSurelyDead
 *
 * Similar to HeapTupleIsSurelyDead, but for zheap tuples.
 */
bool
ZHeapTupleIsSurelyDead(ZHeapTuple zhtup, TransactionId OldestXmin, Buffer buffer)
{
	ZHeapPageOpaque	opaque;
	ZHeapTupleHeader tuple = zhtup->t_data;
	UnpackedUndoRecord	*urec;
	UndoRecPtr	urec_ptr;
	TransactionId	xid;

	opaque = (ZHeapPageOpaque) PageGetSpecialPointer(BufferGetPage(buffer));

	Assert(ItemPointerIsValid(&zhtup->t_self));
	Assert(zhtup->t_tableOid != InvalidOid);

	/*
	 * We need to fetch all the transaction related information from undo
	 * record for the tuples that point to a slot that gets invalidated for
	 * reuse at some point of time.  See PageFreezeTransSlots.
	 */
	if (ZHeapTupleHeaderGetXactSlot(tuple) != ZHTUP_SLOT_FROZEN)
	{
		if (tuple->t_infomask & ZHEAP_INVALID_XACT_SLOT)
		{
			uint8	uur_type;

			urec_ptr = ZHeapTupleHeaderGetRawUndoPtr(tuple, opaque);

			do
			{
				urec = UndoFetchRecord(urec_ptr,
									   ItemPointerGetBlockNumber(&zhtup->t_self),
									   ItemPointerGetOffsetNumber(&zhtup->t_self),
									   InvalidTransactionId);

				/*
				 * The undo tuple must be visible, if the undo record containing
				 * the information of the last transaction that has updated the
				 * tuple is discarded or the transaction that has last updated the
				 * undo tuple precedes RecentGlobalXmin.
				 */
				if (urec == NULL)
				{
					xid = InvalidTransactionId;
					break;
				}

				xid = urec->uur_prevxid;
				urec_ptr = urec->uur_blkprev;
				uur_type = urec->uur_type;

				/*
				 * transaction slot won't change for such a tuple, so we can rely on
				 * the same from current undo tuple.
				 */

				UndoRecordRelease(urec);
			} while (uur_type != UNDO_INVALID_XACT_SLOT);
		}
		else
		{
			xid = ZHeapTupleHeaderGetRawXid(tuple, opaque);
		}
	}
	else
		xid = InvalidTransactionId;

	if (tuple->t_infomask & ZHEAP_DELETED ||
		tuple->t_infomask & ZHEAP_UPDATED)
	{
		/*
		 * The tuple is deleted and must be all visible if the transaction slot
		 * is cleared or latest xid that has changed the tuple precedes
		 * smallest xid that has undo.
		 */
		if (ZHeapTupleHeaderGetXactSlot(tuple) == ZHTUP_SLOT_FROZEN ||
			TransactionIdPrecedes(xid, RecentGlobalXmin))
			return true;
	}

	return false; /* Tuple is still alive */
}

/*
 * ZHeapTupleSatisfiesDirty
 *		Returns the visible version of tuple (including effects of open
 *		transactions) if any, NULL otherwise.
 *
 *	Here, we consider the effects of:
 *		all committed and in-progress transactions (as of the current instant)
 *		previous commands of this transaction
 *		changes made by the current command
 *
 *	The tuple will be considered visible iff:
 *	(a) Latest operation on tuple is Delete or non-inplace-update and the
 *		current transaction is in progress.
 *	(b) Latest operation on tuple is Insert, In-Place update or tuple is
 *		locked and the transaction that has performed operation is current
 *		transaction or is in-progress or is committed.
 */
ZHeapTuple
ZHeapTupleSatisfiesDirty(ZHeapTuple zhtup, Snapshot snapshot,
						 Buffer buffer, ItemPointer ctid)
{
	ZHeapPageOpaque	opaque;
	ZHeapTupleHeader tuple = zhtup->t_data;
	UnpackedUndoRecord	*urec;
	UndoRecPtr	urec_ptr;
	TransactionId	xid;

	opaque = (ZHeapPageOpaque) PageGetSpecialPointer(BufferGetPage(buffer));

	Assert(ItemPointerIsValid(&zhtup->t_self));
	Assert(zhtup->t_tableOid != InvalidOid);

	snapshot->xmin = snapshot->xmax = InvalidTransactionId;
	snapshot->speculativeToken = 0;

	/*
	 * We need to fetch all the transaction related information from undo
	 * record for the tuples that point to a slot that gets invalidated for
	 * reuse at some point of time.  See PageFreezeTransSlots.
	 */
	if (ZHeapTupleHeaderGetXactSlot(tuple) != ZHTUP_SLOT_FROZEN)
	{
		if (tuple->t_infomask & ZHEAP_INVALID_XACT_SLOT)
		{
			uint8	uur_type;

			urec_ptr = ZHeapTupleHeaderGetRawUndoPtr(tuple, opaque);

			do
			{
				urec = UndoFetchRecord(urec_ptr,
									   ItemPointerGetBlockNumber(&zhtup->t_self),
									   ItemPointerGetOffsetNumber(&zhtup->t_self),
									   InvalidTransactionId);

				/*
				 * The undo tuple must be visible, if the undo record containing
				 * the information of the last transaction that has updated the
				 * tuple is discarded or the transaction that has last updated the
				 * undo tuple precedes RecentGlobalXmin.
				 */
				if (urec == NULL)
				{
					xid = InvalidTransactionId;
					break;
				}

				xid = urec->uur_prevxid;
				urec_ptr = urec->uur_blkprev;
				uur_type = urec->uur_type;

				/*
				 * transaction slot won't change for such a tuple, so we can rely on
				 * the same from current undo tuple.
				 */

				UndoRecordRelease(urec);
			} while (uur_type != UNDO_INVALID_XACT_SLOT);
		}
		else
		{
			xid = ZHeapTupleHeaderGetRawXid(tuple, opaque);
		}
	}
	else
		xid = InvalidTransactionId;

	if (tuple->t_infomask & ZHEAP_DELETED ||
		tuple->t_infomask & ZHEAP_UPDATED)
	{
		/*
		 * The tuple is deleted and must be all visible if the transaction slot
		 * is cleared or latest xid that has changed the tuple precedes
		 * smallest xid that has undo.  However, that is not possible at this
		 * stage as the tuple has already passed snapshot check.
		 */
		Assert(!(ZHeapTupleHeaderGetXactSlot(tuple) == ZHTUP_SLOT_FROZEN &&
			   TransactionIdPrecedes(xid, RecentGlobalXmin)));

		if (TransactionIdIsCurrentTransactionId(xid))
		{
			/*
			 * For non-inplace-updates, ctid needs to be retrieved from undo
			 * record if required.
			 */
			if (tuple->t_infomask & ZHEAP_UPDATED && ctid)
				ZHeapTupleGetCtid(zhtup, buffer, ctid);
			return NULL;
		}
		else if (TransactionIdIsInProgress(xid))
		{
			snapshot->xmax = xid;
			return zhtup;		/* in deletion by other */
		}
		else if (TransactionIdDidCommit(xid))
		{
			/*
			 * For non-inplace-updates, ctid needs to be retrieved from undo
			 * record if required.
			 */
			if (tuple->t_infomask & ZHEAP_UPDATED && ctid)
				ZHeapTupleGetCtid(zhtup, buffer, ctid);

			/* tuple is deleted or non-inplace-updated */
			return NULL;
		}
		else	/* transaction is aborted */
		{
			/*
			 * Fixme - Here we need to fetch the tuple from undo, something similar
			 * to GetTupleFromUndo but for DirtySnapshots.
			 */
			Assert(false);
			return NULL;
		}
	}
	else if (tuple->t_infomask & ZHEAP_INPLACE_UPDATED ||
			 tuple->t_infomask & ZHEAP_XID_LOCK_ONLY)
	{
		/*
		 * The tuple is updated/locked and must be all visible if the
		 * transaction slot is cleared or latest xid that has changed the
		 * tuple precedes smallest xid that has undo.
		 */
		if (ZHeapTupleHeaderGetXactSlot(tuple) == ZHTUP_SLOT_FROZEN ||
			TransactionIdPrecedes(xid, RecentGlobalXmin))
			return zhtup;	/* tuple is updated */

		if (TransactionIdIsCurrentTransactionId(xid))
			return zhtup;
		else if (TransactionIdIsInProgress(xid))
		{
			if (!ZHEAP_XID_IS_LOCKED_ONLY(tuple->t_infomask))
				snapshot->xmax = xid;
			return zhtup;		/* being updated */
		}
		else if (TransactionIdDidCommit(xid))
			return zhtup;	/* tuple is updated by someone else */
		else	/* transaction is aborted */
		{
			/*
			 * Fixme - Here we need to fetch the tuple from undo, something similar
			 * to GetTupleFromUndo but for DirtySnapshots.
			 */
			Assert(false);
			return NULL;
		}
	}

	/*
	 * The tuple must be all visible if the transaction slot is cleared or
	 * latest xid that has changed the tuple precedes smallest xid that has
	 * undo.
	 */
	if (ZHeapTupleHeaderGetXactSlot(tuple) == ZHTUP_SLOT_FROZEN ||
		TransactionIdPrecedes(xid, RecentGlobalXmin))
		return zhtup;

	if (TransactionIdIsCurrentTransactionId(xid))
		return zhtup;
	else if (TransactionIdIsInProgress(xid))
	{
		snapshot->xmin = xid;
		return zhtup;		/* in insertion by other */
	}
	else if (TransactionIdDidCommit(xid))
		return zhtup;
	else
	{
		/*
		 * Fixme - Here we need to fetch the tuple from undo, something similar
		 * to GetTupleFromUndo but for DirtySnapshots.
		 */
		Assert(false);
		return NULL;
	}

	return NULL;
}

/*
 * ZHeapTupleSatisfiesAny
 *		Dummy "satisfies" routine: any tuple satisfies SnapshotAny.
 */
ZHeapTuple
ZHeapTupleSatisfiesAny(ZHeapTuple zhtup, Snapshot snapshot, Buffer buffer,
					   ItemPointer ctid)
{
	return zhtup;
}

/*
 * ZHeapTupleSatisfiesOldestXmin
 *	The tuple will be considered visible if it is visible to any open
 *	transaction.
 */
HTSV_Result
ZHeapTupleSatisfiesOldestXmin(ZHeapTuple zhtup, TransactionId OldestXmin,
							  Buffer buffer, TransactionId *xid)
{
	ZHeapPageOpaque	opaque;
	ZHeapTupleHeader tuple = zhtup->t_data;
	UnpackedUndoRecord	*urec;
	UndoRecPtr	urec_ptr;

	opaque = (ZHeapPageOpaque) PageGetSpecialPointer(BufferGetPage(buffer));

	Assert(ItemPointerIsValid(&zhtup->t_self));
	Assert(zhtup->t_tableOid != InvalidOid);

	/*
	 * We need to fetch all the transaction related information from undo
	 * record for the tuples that point to a slot that gets invalidated for
	 * reuse at some point of time.  See PageFreezeTransSlots.
	 */
	if ((ZHeapTupleHeaderGetXactSlot(tuple) != ZHTUP_SLOT_FROZEN)
		&& !TransactionIdPrecedes(ZHeapTupleHeaderGetRawXid(tuple, opaque),
								  RecentGlobalXmin))
	{
		if (tuple->t_infomask & ZHEAP_INVALID_XACT_SLOT)
		{
			uint8	uur_type;

			urec_ptr = ZHeapTupleHeaderGetRawUndoPtr(tuple, opaque);

			do
			{
				urec = UndoFetchRecord(urec_ptr,
									   ItemPointerGetBlockNumber(&zhtup->t_self),
									   ItemPointerGetOffsetNumber(&zhtup->t_self),
									   InvalidTransactionId);

				/*
				 * The undo tuple must be visible, if the undo record containing
				 * the information of the last transaction that has updated the
				 * tuple is discarded or the transaction that has last updated the
				 * undo tuple precedes RecentGlobalXmin.
				 */
				if (urec == NULL)
				{
					*xid = InvalidTransactionId;
					break;
				}

				*xid = urec->uur_prevxid;
				urec_ptr = urec->uur_blkprev;
				uur_type = urec->uur_type;

				/*
				 * transaction slot won't change for such a tuple, so we can rely on
				 * the same from current undo tuple.
				 */

				UndoRecordRelease(urec);
			} while (uur_type != UNDO_INVALID_XACT_SLOT);
		}
		else
		{
			*xid = ZHeapTupleHeaderGetRawXid(tuple, opaque);
			urec_ptr = ZHeapTupleHeaderGetRawUndoPtr(tuple, opaque);
		}
	}
	else if (ZHeapTupleHeaderGetXactSlot(tuple) != ZHTUP_SLOT_FROZEN)
		*xid = ZHeapTupleHeaderGetRawXid(tuple, opaque);
	else
		*xid = InvalidTransactionId;

	if (tuple->t_infomask & ZHEAP_DELETED ||
		tuple->t_infomask & ZHEAP_UPDATED)
	{
		/*
		 * The tuple is deleted and must be all visible if the transaction slot
		 * is cleared or latest xid that has changed the tuple precedes
		 * smallest xid that has undo.
		 */
		if (ZHeapTupleHeaderGetXactSlot(tuple) == ZHTUP_SLOT_FROZEN ||
			TransactionIdPrecedes(*xid, RecentGlobalXmin))
			return HEAPTUPLE_DEAD;

		if (TransactionIdIsCurrentTransactionId(*xid))
			return HEAPTUPLE_DELETE_IN_PROGRESS;
		else if (TransactionIdIsInProgress(*xid))
		{
			return HEAPTUPLE_DELETE_IN_PROGRESS;
		}
		else if (TransactionIdDidCommit(*xid))
		{
			/*
			 * Deleter committed, but perhaps it was recent enough that some open
			 * transactions could still see the tuple.
			 */
			if (!TransactionIdPrecedes(*xid, OldestXmin))
				return HEAPTUPLE_RECENTLY_DEAD;

			/* Otherwise, it's dead and removable */
			return HEAPTUPLE_DEAD;
		}
		else	/* transaction is aborted */
			return HEAPTUPLE_LIVE;
	}
	else if (tuple->t_infomask & ZHEAP_XID_LOCK_ONLY)
	{
		/*
		 * "Deleting" xact really only locked it, so the tuple is live in any
		 * case.
		 */
		return HEAPTUPLE_LIVE;
	}

	/* The tuple is either a newly inserted tuple or is in-place updated. */

	/*
	 * The tuple must be all visible if the transaction slot is cleared or
	 * latest xid that has changed the tuple precedes smallest xid that has
	 * undo.
	 */
	if (ZHeapTupleHeaderGetXactSlot(tuple) == ZHTUP_SLOT_FROZEN ||
		TransactionIdPrecedes(*xid, RecentGlobalXmin))
		return HEAPTUPLE_LIVE;

	if (TransactionIdIsCurrentTransactionId(*xid))
		return HEAPTUPLE_INSERT_IN_PROGRESS;
	else if (TransactionIdIsInProgress(*xid))
		return HEAPTUPLE_INSERT_IN_PROGRESS;		/* in insertion by other */
	else if (TransactionIdDidCommit(*xid))
		return HEAPTUPLE_LIVE;
	else	/* transaction is aborted */
	{
		if (tuple->t_infomask & ZHEAP_INPLACE_UPDATED)
		{
			/*
			 * Fixme - For aborted transactions, either we need to fetch the
			 * visible tuple from undo chain if the rollback is still not
			 * performed or we can perform rollback here itself or trigger undo
			 * worker and wait for rollback to finish and return the status as
			 * per new tuple.
			 */
		}

		return HEAPTUPLE_DEAD;
	}

	return HEAPTUPLE_LIVE;
}