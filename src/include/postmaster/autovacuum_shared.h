/*-------------------------------------------------------------------------
 *
 * autovacuum_shared.h
 *	Shared helpers for autovacuum computations that are also useful for
 *	monitoring and statistics exposure.
 *
 * src/include/postmaster/autovacuum_shared.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTOVACUUM_SHARED_H
#define AUTOVACUUM_SHARED_H

#include "access/htup.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "catalog/pg_class.h"
#include "pgstat.h"
#include "utils/rel.h"

/*
 * Computed metrics derived from autovacuum configuration, catalog estimates,
 * and relation statistics.
 *
 * This is intended for read-only monitoring. Callers must not use this to
 * change autovacuum behavior.
 */
typedef struct AutovacuumCandidateMetrics
{
	bool		autovacuum_enabled;
	bool		has_stats;
	bool		autovacuuming_active;
	bool		vacuum_due;
	bool		analyze_due;
	bool		wraparound_forced;
	bool		vacuum_insert_enabled;

	int32		xid_age;
	int32		freeze_max_age;
	int32		mxid_age;
	int32		multixact_freeze_max_age;

	int64		dead_tuples;
	int64		ins_since_vacuum;
	int64		mod_since_analyze;

	float4		reltuples;
	float4		pct_unfrozen;

	float4		vacuum_threshold;
	float4		vacuum_insert_threshold;
	float4		analyze_threshold;
} AutovacuumCandidateMetrics;

extern AutoVacOpts *autovacuum_extract_autovac_opts(HeapTuple tup,
									  TupleDesc pg_class_desc);

extern void autovacuum_compute_candidate_metrics(Oid relid,
									  const AutoVacOpts *relopts,
									  Form_pg_class classForm,
									  PgStat_StatTabEntry *tabentry,
									  TransactionId nowXid,
									  MultiXactId nowMxid,
									  int effective_multixact_freeze_max_age,
									  bool autovacuuming_active,
									  AutovacuumCandidateMetrics *out);

#endif						/* AUTOVACUUM_SHARED_H */
