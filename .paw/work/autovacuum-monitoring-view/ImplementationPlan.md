# Autovacuum Monitoring View Implementation Plan

## Overview

Implement a new built-in collected-statistics view, `pg_catalog.pg_stat_autovacuum_candidates`, that exposes (per eligible relation) the raw churn counters and the computed VACUUM/ANALYZE thresholds used by autovacuum decision logic, plus derived “due” flags and a sortable monitoring score.

This is a **monitoring-only** feature: it must not alter autovacuum behavior.

## Current State Analysis

- Collected-statistics system views are defined in SQL in `src/backend/catalog/system_views.sql` (for example `pg_stat_all_tables`), typically selecting from `pg_class` / `pg_namespace` and calling `pg_stat_get_*` functions (see `src/backend/catalog/system_views.sql:662-830`).
- Many `pg_stat_get_*` scalar functions are implemented in `src/backend/utils/adt/pgstatfuncs.c` via macros over `pgstat_fetch_stat_tabentry()` (`src/backend/utils/adt/pgstatfuncs.c:34-120`). `pgstat_fetch_stat_tabentry()` itself is a thin lookup without access checks (`src/backend/utils/activity/pgstat_relation.c:469-496`).
- Autovacuum’s “needs VACUUM / needs ANALYZE” threshold computation and due-flag logic are implemented in `relation_needs_vacanalyze()` (`src/backend/postmaster/autovacuum.c:2998-3240`). It combines:
  - reloptions (`AutoVacOpts`) and GUCs,
  - `pg_class` estimates (`reltuples`, `relpages`, `relallfrozen`, `relfrozenxid`, `relminmxid`),
  - stats counters (`dead_tuples`, `ins_since_vacuum`, `mod_since_analyze`).
- PostgreSQL already has a stats-visibility privilege model centered on the predefined role `pg_read_all_stats` (`ROLE_PG_READ_ALL_STATS`) and the helper macro `HAS_PGSTAT_PERMISSIONS(role)` (`src/backend/utils/adt/pgstatfuncs.c:37`).
- There are established patterns for restricting access to a view and its underlying function to `pg_read_all_stats` using `REVOKE`/`GRANT` in `system_views.sql` (for example `pg_shmem_allocations`, `pg_backend_memory_contexts`, etc. in `src/backend/catalog/system_views.sql:654-694`).

## Desired End State

After completion:

- A new view `pg_catalog.pg_stat_autovacuum_candidates` exists and returns one row per **eligible relation** in the current database:
  - ordinary tables (`relkind='r'`), TOAST tables (`relkind='t'`), materialized views (`relkind='m'`)
  - excludes: temporary tables, partitioned tables that have no storage (`relkind='p'`), and any other relation types not relevant to autovacuum
- The view exposes:
  - raw counters used by autovacuum decision-making
  - computed thresholds and “due” flags matching current autovacuum logic
  - wraparound forcing context
  - a monitoring-only `autovacuum_priority_score` and `autovacuum_priority_reason`
- Privileges follow the statistics privilege model:
  - callers without the required privilege are **denied** access (not merely masked)
  - callers with `pg_read_all_stats` succeed
- Regression tests are updated and stable:
  - `rules.sql` snapshot output changes to include the new view definition
  - `privileges.sql` validates the `pg_read_all_stats` gating
  - `stats.sql` validates correctness of thresholds/due flags and ordering behavior under controlled settings
- Documentation (`doc/src/sgml/monitoring.sgml`) includes the new view in the Collected Statistics Views table and adds a dedicated section describing the view and its columns.

### Key Discoveries

- A view-only solution would require duplicating `relation_needs_vacanalyze()` logic in SQL; that logic includes TOAST fallback behavior and effective multixact freeze handling and is not trivial to keep in sync (see `src/backend/postmaster/autovacuum.c:2998-3240`).
- PostgreSQL commonly uses a **view wrapper over an SRF** where the SRF materializes a rowset (`pg_stat_activity` pattern; see `src/backend/catalog/system_views.sql:900-926` referenced in CodeResearch.md).
- The desired security semantics (“query denied”) align best with view/function ACL gating (existing `REVOKE ALL ... FROM PUBLIC; GRANT ... TO pg_read_all_stats;` patterns in `src/backend/catalog/system_views.sql:654-694`).

## What We’re NOT Doing

- No changes to autovacuum scheduling, internal worker selection, or decision-making.
- No attempt to predict which table will be vacuumed next.
- No additional UI, contrib modules, or external tooling.
- No new configuration parameters.

## Implementation Approach

### Options Considered

1. **SQL-only view (no new C code)**
   - Pros: simplest wiring (just `system_views.sql`).
   - Cons: requires duplicating threshold and wraparound logic in SQL; risks drift with future autovacuum changes; difficult TOAST/reloptions handling.

2. **Per-relation scalar function + SQL view**
   - Pros: keeps `system_views.sql` style; easy to join with `pg_class`.
   - Cons: repeats function call per relation and still must duplicate core threshold logic unless refactored; more overhead and more moving parts.

3. **SRF that produces the complete rowset + SQL view wrapper (Recommended)**
   - Pros: one place to implement and keep logic consistent; matches existing SRF+view patterns; easiest to enforce `pg_read_all_stats` at view/function level.
   - Cons: requires adding a new C SRF and a `pg_proc.dat` entry.

### Selected Approach

Implement a new SQL-callable **materialized SRF** `pg_stat_get_autovacuum_candidates()` returning `SETOF record` with named output columns, and define the user-visible view `pg_stat_autovacuum_candidates` as `SELECT * FROM pg_stat_get_autovacuum_candidates() AS ...`.

To avoid logic drift, factor the threshold/due computation into a shared helper in the autovacuum subsystem so both autovacuum and the SRF use the same computation.

## Phase Summary

1. **Phase 1: Backend SRF + shared computation** — Add a C SRF and shared helper to compute thresholds/due flags using existing autovacuum logic.
2. **Phase 2: System view + privilege model** — Add the `pg_stat_autovacuum_candidates` view and enforce `pg_read_all_stats` access via REVOKE/GRANT.
3. **Phase 3: Regression + docs updates** — Update regression tests (`rules.sql`, `privileges.sql`, `stats.sql`) and document the new collected stats view in `monitoring.sgml`.

---

## Phase 1: Backend SRF + shared computation

### Overview

Create the backend function that produces the view’s rows, ensuring correctness by reusing the same threshold/due logic as autovacuum.

### Changes Required

#### 1. Extract reusable computation from autovacuum
**Files**:
- `src/backend/postmaster/autovacuum.c`
- `src/include/postmaster/autovacuum.h` (or a new small header under `src/include/postmaster/` if separation is preferred)

**Changes**:
- Introduce a non-SQL helper (e.g., `autovacuum_compute_candidate_metrics(...)`) that encapsulates the logic currently inside `relation_needs_vacanalyze()` and the related computation of:
  - effective constants (threshold bases/scale factors/max threshold)
  - `pcnt_unfrozen` computation (`relpages`, `relallfrozen` clamping)
  - computed thresholds (`vacuum_threshold`, `vacuum_insert_threshold`, `analyze_threshold`)
  - churn counters (`dead_tuples`, `ins_since_vacuum`, `mod_since_analyze`) and decision flags
  - wraparound forcing state based on `relfrozenxid` / `relminmxid`
- Keep `relation_needs_vacanalyze()` as the single source of truth by either:
  - (preferred) rewriting it to call the new helper and then mapping helper outputs to existing `*dovacuum`, `*doanalyze`, `*wraparound`; or
  - moving most of its body into the helper and keeping `relation_needs_vacanalyze()` a thin wrapper.

**Notes/constraints**:
- Preserve the existing semantics exactly as seen in `relation_needs_vacanalyze()` (`src/backend/postmaster/autovacuum.c:2998-3240`).
- Use the same effective multixact freeze age logic autovacuum uses: `effective_multixact_freeze_max_age = MultiXactMemberFreezeThreshold()` (`src/backend/postmaster/autovacuum.c:1942-1946`, declaration in `src/include/access/multixact.h:147`).

#### 2. Implement `pg_stat_get_autovacuum_candidates()` SRF
**Files**:
- `src/backend/utils/adt/pgstatfuncs.c`
- `src/include/catalog/pg_proc.dat`

**Changes**:
- Add a new SRF in `pgstatfuncs.c` implemented using `InitMaterializedSRF()` / tuplestore, following the pattern used by `pg_stat_get_activity()` / `pg_stat_get_progress_info()` (`src/backend/utils/adt/pgstatfuncs.c:300-520`).
- Include the appropriate headers:
  - `postmaster/autovacuum.h` for `AutoVacuumingActive()` and GUCs
  - `access/multixact.h` for `MultiXactMemberFreezeThreshold()`
  - catalog access needed to scan `pg_class` safely
- Scan `pg_class` (via `table_beginscan_catalog(...)`) to produce one row per eligible relation in the current database.
  - Filter to match spec: exclude temp tables (`relpersistence = 't'`) and partitioned tables (`relkind='p'`).
  - Include TOAST tables (`relkind='t'`) and apply the same “reloptions fallback to parent” behavior as autovacuum’s second pass (`src/backend/postmaster/autovacuum.c:2050-2160`).
- Populate the output columns per Spec.md Appendix (v1 schema):
  - Identity: `relid`, `schemaname`, `relname`, `relkind`
  - Eligibility/config: `autovacuum_enabled`, `has_stats`, `autovacuuming_active`
  - Inputs/counters: `dead_tuples`, `ins_since_vacuum`, `mod_since_analyze`, `reltuples`, `pct_unfrozen`
  - Thresholds: `vacuum_threshold`, `vacuum_insert_threshold` (NULL when disabled), `analyze_threshold`
  - Wraparound context: `xid_age`, `freeze_max_age`, `mxid_age`, `multixact_freeze_max_age`
  - Decisions: `vacuum_due`, `analyze_due`, `wraparound_forced`
  - Ratios: `vacuum_dead_ratio`, `vacuum_insert_ratio`, `analyze_ratio`
  - Derived ranking: `autovacuum_priority_score`, `autovacuum_priority_reason`

**Priority score semantics (explicit, monitoring-only)**:
- Define per-trigger ratios as:
  - `vacuum_dead_ratio = dead_tuples / vacuum_threshold` (NULL if no stats)
  - `vacuum_insert_ratio = ins_since_vacuum / vacuum_insert_threshold` (NULL if disabled)
  - `analyze_ratio = mod_since_analyze / analyze_threshold` (NULL if no stats or analyze not applicable)
  - `xid_age_ratio = xid_age / freeze_max_age`
  - `mxid_age_ratio = mxid_age / multixact_freeze_max_age`
- Define `autovacuum_priority_score` as `GREATEST(vacuum_dead_ratio, vacuum_insert_ratio, analyze_ratio, xid_age_ratio, mxid_age_ratio)` with NULLs ignored.
- Define `autovacuum_priority_reason` as a stable textual label indicating which ratio matched the chosen score (ties can break by a fixed precedence order: wraparound xid > wraparound mxid > vacuum_dead > vacuum_insert > analyze).

**pg_proc registration**:
- Add a new entry for `pg_stat_get_autovacuum_candidates` in `pg_proc.dat` near other `pg_stat_get_*` SRFs.
- Return type: `record`, `proretset => 't'`, with `proallargtypes`, `proargmodes`, and `proargnames` listing the concrete output schema (pattern in `src/include/catalog/pg_proc.dat:5638-5670`).

**Tests**:
- Add a small unit-test-like C regression is not typical in core; rely on SQL regression tests in Phase 3.

### Success Criteria

#### Automated Verification
- [ ] `make -C src/backend` (or full tree) succeeds.
- [ ] `pg_proc.dat` builds cleanly (no duplicate OIDs / catalog build errors).
- [ ] `make check-world` (or at minimum `make check` for regress) passes once tests are updated.

#### Manual Verification
- [ ] In psql as superuser, `SELECT * FROM pg_stat_autovacuum_candidates LIMIT 5;` returns sensible rows and does not take heavyweight locks.

---

## Phase 2: System view + privilege model

### Overview

Expose the SRF as a `pg_catalog` collected statistics view and enforce access using the existing `pg_read_all_stats` model.

### Changes Required

#### 1. Add `pg_stat_autovacuum_candidates` to `system_views.sql`
**File**: `src/backend/catalog/system_views.sql`

**Changes**:
- Add:
  - `CREATE VIEW pg_stat_autovacuum_candidates AS SELECT * FROM pg_stat_get_autovacuum_candidates() AS S(...);`
    - Prefer `SELECT * FROM ...` if `pg_proc.dat` provides OUT column names.
- Apply privilege gating consistent with existing patterns (`src/backend/catalog/system_views.sql:654-694`):
  - `REVOKE ALL ON pg_stat_autovacuum_candidates FROM PUBLIC;`
  - `GRANT SELECT ON pg_stat_autovacuum_candidates TO pg_read_all_stats;`
  - `REVOKE EXECUTE ON FUNCTION pg_stat_get_autovacuum_candidates() FROM PUBLIC;`
  - `GRANT EXECUTE ON FUNCTION pg_stat_get_autovacuum_candidates() TO pg_read_all_stats;`

**Privilege model rationale**:
- Spec.md acceptance requires “query denied” for callers without stats privilege; view/function ACL gating provides that behavior.

### Success Criteria

#### Automated Verification
- [ ] `src/backend/catalog/system_views.sql` installs cleanly during initdb / regression.
- [ ] Regression `privileges.sql` tests (Phase 3) confirm access is denied/granted correctly.

#### Manual Verification
- [ ] As non-member of `pg_read_all_stats`, `SELECT 1 FROM pg_stat_autovacuum_candidates;` fails with permission denied.
- [ ] As member of `pg_read_all_stats`, the same query succeeds.

---

## Phase 3: Regression + docs updates

### Overview

Update regression tests to cover the new view/function and document the feature in the monitoring chapter.

### Changes Required

#### 1. Update view snapshot regression output
**Files**:
- `src/test/regress/sql/rules.sql` (query already exists at `src/test/regress/sql/rules.sql:772-796`)
- `src/test/regress/expected/rules.out`

**Changes**:
- Adjust expected output of the `pg_views` / `pg_rules` snapshot so the new `pg_stat_autovacuum_candidates` view appears in the ordered list.

#### 2. Add privileges regression coverage
**Files**:
- `src/test/regress/sql/privileges.sql`
- `src/test/regress/expected/privileges.out`

**Changes**:
- Extend the existing “system views restricted to pg_read_all_stats” block (`src/test/regress/sql/privileges.sql:1940-2015`) to include:
  - `has_table_privilege(..., 'pg_stat_autovacuum_candidates', 'SELECT')` checks before/after granting `pg_read_all_stats`.
  - A `SET ROLE regress_readallstats; SELECT COUNT(*) >= 0 AS ok FROM pg_stat_autovacuum_candidates;` smoke query.

#### 3. Add correctness/ordering regression coverage
**Files**:
- `src/test/regress/sql/stats.sql`
- `src/test/regress/expected/stats.out`

**Changes**:
- Add a small deterministic test section near other relation stats tests (early in file is fine):
  - Create two test tables with reloptions set to remove dependence on `reltuples`:
    - set `autovacuum_vacuum_scale_factor = 0`, `autovacuum_vacuum_threshold = 10`
    - set `autovacuum_analyze_scale_factor = 0`, `autovacuum_analyze_threshold = 10`
    - set `autovacuum_vacuum_insert_threshold = -1` to disable insert-vacuum
  - Produce stats:
    - `ANALYZE` to establish baseline
    - perform deletes/updates to exceed thresholds
    - use `SELECT pg_stat_force_next_flush();` and `SET LOCAL stats_fetch_consistency = snapshot;` patterns (see `src/test/regress/sql/stats.sql:24-80`).
  - Assert:
    - thresholds equal expected constants (10)
    - `vacuum_due` and/or `analyze_due` become true as expected
    - `vacuum_insert_threshold` is NULL when disabled
    - ordering by `autovacuum_priority_score DESC` ranks the more-beyond-threshold table first
  - Add one small coverage check for exclusions:
    - create a TEMP table and a partitioned table; confirm they do not appear in the view.

#### 4. Document the new view
**File**: `doc/src/sgml/monitoring.sgml`

**Changes**:
- Add a row for `pg_stat_autovacuum_candidates` to the “Collected Statistics Views” table (`doc/src/sgml/monitoring.sgml:426-620`):
  - include a short description and a `<link linkend="...">` to the detailed section.
- Add a new `<sect2>` describing the view, modeled after existing collected-stat views such as `pg_stat_all_tables` (`doc/src/sgml/monitoring.sgml:3850+`):
  - describe purpose and note “monitoring-only score” semantics
  - include a column table describing each output column (at least the v1 schema columns from Spec.md Appendix)
  - add an explicit note that access requires `pg_read_all_stats`

### Success Criteria

#### Automated Verification
- [ ] `make check` passes for updated regression tests (`rules`, `privileges`, `stats`).
- [ ] `make -C doc` (or doc build target used in this repo) succeeds with the new `monitoring.sgml` section.

#### Manual Verification
- [ ] Grep/scan rendered docs show the view listed and described under Collected Statistics Views.

---

## Cross-Phase Testing Strategy

### Integration Tests
- Regression suite coverage via `src/test/regress/sql/stats.sql` ensures the view stays aligned with stats collection semantics and threshold computation.

### Manual Testing Steps
1. Create a small table, set autovacuum reloptions to fixed thresholds, generate dead tuples, and confirm `vacuum_due` toggles as expected.
2. Verify `wraparound_forced` becomes true when `relfrozenxid` is artificially advanced (if safe/possible in a dev instance), otherwise rely on unit logic correctness.
3. Confirm access control by testing as a role without and with `pg_read_all_stats`.

## Performance Considerations

- The SRF should scan `pg_class` with catalog scan APIs and avoid opening relations unnecessarily.
- Avoid heavyweight locking; use only the minimum locks required to read catalog metadata.
- Use one catalog scan pass for main relations and a second pass for TOAST only if necessary for accurate reloption fallback, mirroring autovacuum’s existing approach.

## Migration Notes

- No catalog migrations are required; this is a new built-in view + function.
- Regression expected files must be updated to account for the new view definition appearing in system view snapshots.

## References

- Spec: `.paw/work/autovacuum-monitoring-view/Spec.md`
- Code research: `.paw/work/autovacuum-monitoring-view/CodeResearch.md`
- Autovacuum threshold logic: `src/backend/postmaster/autovacuum.c:2998-3240`
- View wiring and privilege patterns: `src/backend/catalog/system_views.sql:654-830`
- SRF implementation pattern: `src/backend/utils/adt/pgstatfuncs.c:300-520`
- Regression patterns: `src/test/regress/sql/rules.sql:772-796`, `src/test/regress/sql/privileges.sql:1940-2015`, `src/test/regress/sql/stats.sql:24-80`
- Docs location: `doc/src/sgml/monitoring.sgml:426-620`
