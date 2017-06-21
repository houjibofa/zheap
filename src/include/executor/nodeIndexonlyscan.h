/*-------------------------------------------------------------------------
 *
 * nodeIndexonlyscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeIndexonlyscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEINDEXONLYSCAN_H
#define NODEINDEXONLYSCAN_H

#include "nodes/execnodes.h"
#include "access/parallel.h"

extern IndexOnlyScanState *ExecInitIndexOnlyScan(IndexOnlyScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecIndexOnlyScan(IndexOnlyScanState *node);
extern void ExecEndIndexOnlyScan(IndexOnlyScanState *node);
extern void ExecIndexOnlyMarkPos(IndexOnlyScanState *node);
extern void ExecIndexOnlyRestrPos(IndexOnlyScanState *node);
extern void ExecReScanIndexOnlyScan(IndexOnlyScanState *node);

/* Support functions for parallel index-only scans */
extern void ExecIndexOnlyScanEstimate(IndexOnlyScanState *node,
						  ParallelContext *pcxt);
extern void ExecIndexOnlyScanInitializeDSM(IndexOnlyScanState *node,
							   ParallelContext *pcxt);
extern void ExecIndexOnlyScanInitializeWorker(IndexOnlyScanState *node,
								  shm_toc *toc);

#endif							/* NODEINDEXONLYSCAN_H */
