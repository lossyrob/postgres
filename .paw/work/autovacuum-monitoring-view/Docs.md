# Autovacuum Monitoring View

## Overview

This work adds a new collected-statistics view, `pg_catalog.pg_stat_autovacuum_candidates`, that exposes (per eligible relation) the same high-level inputs and computed thresholds that autovacuum uses to decide whether to run `VACUUM` and/or `ANALYZE`.

The view is **monitoring-only**:
- It does not change autovacuum behavior.
- It is designed to support DBA/operator triage by making churn counters, thresholds, due flags, and a sortable monitoring score visible in one place.

## What Was Implemented

### Public SQL surface

- View: `pg_catalog.pg_stat_autovacuum_candidates`
  - Defined in `src/backend/catalog/system_views.sql` as a wrapper over the SRF.
  - Returns one row per eligible relation in the current database.

- SRF: `pg_catalog.pg_stat_get_autovacuum_candidates()`
  - Defined in `src/backend/utils/adt/pgstatfuncs.c` and registered in `src/include/catalog/pg_proc.dat`.
  - Produces the full rowset consumed by the view.

### Eligibility rules (which relations appear)

The SRF filters `pg_class` and emits rows for:
- ordinary tables (`relkind = 'r'`)
- materialized views (`relkind = 'm'`)
- TOAST tables (`relkind = 't'`)

And excludes:
- temporary relations (`relpersistence = 't'`)
- partitioned tables (containers without storage)

### Autovacuum reloptions and TOAST fallback

For TOAST tables, the SRF mirrors autovacuum’s behavior of falling back to the parent table’s autovacuum reloptions when the TOAST table has no explicit reloptions set.

## Permissions / Visibility

Access is gated using the existing statistics privilege model:
- `REVOKE ALL ON pg_stat_autovacuum_candidates FROM PUBLIC`
- `GRANT SELECT ON pg_stat_autovacuum_candidates TO pg_read_all_stats`
- `REVOKE EXECUTE ON FUNCTION pg_stat_get_autovacuum_candidates() FROM PUBLIC`
- `GRANT EXECUTE ON FUNCTION pg_stat_get_autovacuum_candidates() TO pg_read_all_stats`

Unprivileged users receive a hard error when querying the view (rather than NULL masking).

## Column Semantics (v1)

The view columns are the OUT parameters of `pg_stat_get_autovacuum_candidates()`.

### Identity

- `relid oid`: OID of the relation
- `schemaname name`: schema name
- `relname name`: relation name
- `relkind "char"`: `r` (table), `m` (matview), `t` (TOAST)

### Config and stats availability

- `autovacuum_enabled boolean`: whether autovacuum is enabled for the relation via reloptions
- `has_stats boolean`: whether collected statistics are available for the relation
- `autovacuuming_active boolean`: whether autovacuum is enabled in the current server configuration

### Churn counters and computed thresholds

- Counters (NULL when `has_stats` is false):
  - `dead_tuples bigint`
  - `ins_since_vacuum bigint`
  - `mod_since_analyze bigint`

- Inputs:
  - `reltuples real`: estimated live tuples used for threshold computation
  - `pct_unfrozen real`: estimated fraction of pages that are not all-frozen (used for insert-trigger threshold)

- Thresholds:
  - `vacuum_threshold real`
  - `vacuum_insert_threshold real`: NULL when insert-trigger vacuum is disabled
  - `analyze_threshold real`: NULL for TOAST relations

### Wraparound context

- `xid_age integer`: age of `relfrozenxid`
- `freeze_max_age integer`: effective max age before vacuum is forced for XID wraparound
- `mxid_age integer`: age of `relminmxid`
- `multixact_freeze_max_age integer`: effective max age before vacuum is forced for multixact wraparound

### Due flags

- `vacuum_due boolean`
- `analyze_due boolean`
- `wraparound_forced boolean`

These are computed using the same core logic as autovacuum uses for threshold and wraparound decisions.

### Monitoring ratios and score

Ratios are intended to be *interpretable* “how far beyond the trigger are we?” signals:

- `vacuum_dead_ratio double precision`
  - `dead_tuples / vacuum_threshold` when stats are available and threshold > 0
- `vacuum_insert_ratio double precision`
  - `ins_since_vacuum / vacuum_insert_threshold` when enabled, stats available, and threshold > 0
- `analyze_ratio double precision`
  - `mod_since_analyze / analyze_threshold` when applicable and threshold > 0

The monitoring score is a simple max over trigger ratios, with stable tie-breaking:

- `autovacuum_priority_score double precision`
  - Starts from wraparound age ratios:
    - `xid_age / freeze_max_age`
    - `mxid_age / multixact_freeze_max_age`
  - Then compares against churn-based ratios (when non-NULL)
  - The highest value becomes the score

- `autovacuum_priority_reason text`
  - One of: `wraparound_xid`, `wraparound_mxid`, `vacuum_dead`, `vacuum_insert`, `analyze`
  - Chosen according to the same comparisons used to pick the score, so it explains which trigger dominated.

Notes:
- When stats are unavailable (`has_stats = false`), the churn-based ratios are NULL; the score still reflects wraparound ratios.

## Documentation Updates

- `doc/src/sgml/monitoring.sgml`
  - Adds `pg_stat_autovacuum_candidates` to the Collected Statistics Views list
  - Adds a dedicated section describing the view and its columns

## Testing Guide

### Quick functional checks (psql)

1. As a superuser (or member of `pg_read_all_stats`):
   - `SELECT * FROM pg_stat_autovacuum_candidates LIMIT 5;`
   - `SELECT relid, schemaname, relname, vacuum_due, analyze_due, autovacuum_priority_score
      FROM pg_stat_autovacuum_candidates
      ORDER BY autovacuum_priority_score DESC
      LIMIT 20;`

2. As an unprivileged role:
   - `SELECT count(*) FROM pg_stat_autovacuum_candidates;` should error with permission denied.

### Regression tests

This change set includes regression coverage for:
- view definition visibility (`rules.sql` / expected output)
- permissions gating (`privileges.sql` / expected output)
- basic correctness and ordering smoke tests (`stats.sql` / expected output)

## Edge Cases and Limitations

- Tables without statistics:
  - churn counters and churn ratios are NULL
  - score still reflects wraparound-related ratios
- TOAST tables:
  - `analyze_threshold` is NULL; analyze-related signals are not applicable
  - reloptions may be inherited from parent (mirroring autovacuum)
- The score is explicitly a *monitoring* metric and does not promise to match internal worker selection order.
