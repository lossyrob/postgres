---
date: 2025-12-17 08:32:01 EST
git_commit: dfe45c74d9ebee29af8b96a08eab6e49478f8d75
branch: feature/autovacuum-monitoring-view
repository: postgres
topic: "Autovacuum monitoring stats view (pg_stat_autovacuum_candidates)"
tags: [research, postgres-core, pg_stat, autovacuum, system_views, privileges, regression-tests, docs]
status: complete
last_updated: 2025-12-17
---

# Research: Autovacuum monitoring stats view (pg_stat_autovacuum_candidates)

**Date**: 2025-12-17 08:32:01 EST  
**Git Commit**: dfe45c74d9ebee29af8b96a08eab6e49478f8d75  
**Branch**: feature/autovacuum-monitoring-view  
**Repository**: postgres

## Research Question
Perform code research for implementing `.paw/work/autovacuum-monitoring-view/Spec.md` in PostgreSQL core. Focus on:
1) existing SRFs and views that expose per-table stats and computed thresholds (pg_stat_*), where to add a new SRF and how views are wired
2) how privileges are enforced for stats views (especially pg_read_all_stats) and any naming/privilege conventions
3) regression test patterns for stats/views/privileges in src/test/regress
4) docs update locations/patterns for new system views

## Summary
- System stats views like `pg_stat_all_tables` are defined in SQL in `src/backend/catalog/system_views.sql` and are built primarily by selecting from `pg_class/pg_namespace` plus calling `pg_stat_get_*` C functions (e.g., `pg_stat_get_dead_tuples`, `pg_stat_get_ins_since_vacuum`, `pg_stat_get_mod_since_analyze`) (src/backend/catalog/system_views.sql:697-749).
- The C entry points for the underlying `pg_stat_get_*` functions live in `src/backend/utils/adt/pgstatfuncs.c` and are registered as built-in functions via `src/include/catalog/pg_proc.dat` (src/backend/utils/adt/pgstatfuncs.c:24-90; src/include/catalog/pg_proc.dat:5540-5690).
- Autovacuum’s “needs VACUUM / needs ANALYZE” threshold computation is implemented in `relation_needs_vacanalyze()` in `src/backend/postmaster/autovacuum.c`, combining `AutoVacOpts` (table reloptions) with autovacuum GUCs and `pg_class` statistics (`reltuples`, `relpages`, `relallfrozen`) and table stats counters (`dead_tuples`, `ins_since_vacuum`, `mod_since_analyze`) (src/backend/postmaster/autovacuum.c:2959-3235).
- PostgreSQL’s stats visibility model uses the predefined role `pg_read_all_stats` (ROLE_PG_READ_ALL_STATS) and a common permission macro in `pgstatfuncs.c` to gate “sensitive” fields in some SRFs (like `pg_stat_get_activity`) while leaving other `pg_stat_get_*` functions unrestricted (src/include/catalog/pg_authid.dat:35-70; src/backend/utils/adt/pgstatfuncs.c:37, 250-460).
- Regression coverage patterns relevant to a new stats view include: (a) `rules.sql` snapshots `pg_views`/`pg_rules` for `pg_catalog` and will change if a new system view appears, (b) `privileges.sql` contains tests that validate `pg_read_all_stats` role membership and SELECT privileges on specific system views, and (c) `stats.sql` covers many `pg_stat_*` view behaviors and uses consistent patterns for taking a stats snapshot and asserting counters (src/test/regress/sql/rules.sql:772-796; src/test/regress/sql/privileges.sql:90-170, 1940-2015; src/test/regress/sql/stats.sql:1-120).
- Documentation for collected statistics views lives in `doc/src/sgml/monitoring.sgml` (both an overview table entry and a dedicated per-view section), while `doc/src/sgml/system-views.sgml` explicitly points readers to `monitoring.sgml` for statistics views (doc/src/sgml/monitoring.sgml:426-590, 3850-3920; doc/src/sgml/system-views.sgml:1-45).

## Detailed Findings

### 1) Existing stats views + SRFs (how they are wired)

#### 1.1 System view definitions live in system_views.sql
- `src/backend/catalog/system_views.sql` defines system views using plain SQL `CREATE VIEW ... AS SELECT ...` statements.
- The "Statistics views" block begins around `pg_stat_all_tables` and uses `pg_stat_get_*` functions as scalar expressions to expose cumulative stats (src/backend/catalog/system_views.sql:695-749).

Key example:
- `pg_stat_all_tables` joins `pg_class` to `pg_index` and `pg_namespace`, groups by table, and exposes stats from `pg_stat_get_*` (src/backend/catalog/system_views.sql:697-749).
  - Per-table counters relevant to the spec are already present in this view:
    - `pg_stat_get_dead_tuples(C.oid) AS n_dead_tup` (src/backend/catalog/system_views.sql:718-721)
    - `pg_stat_get_mod_since_analyze(C.oid) AS n_mod_since_analyze` (src/backend/catalog/system_views.sql:721-722)
    - `pg_stat_get_ins_since_vacuum(C.oid) AS n_ins_since_vacuum` (src/backend/catalog/system_views.sql:722-723)

Filtered variants follow the same pattern:
- `pg_stat_sys_tables` is `SELECT * FROM pg_stat_all_tables WHERE schemaname ...` (src/backend/catalog/system_views.sql:759-763)
- `pg_stat_user_tables` is `SELECT * FROM pg_stat_all_tables WHERE schemaname ...` (src/backend/catalog/system_views.sql:769-773)

#### 1.2 Views can be thin wrappers over SRFs
Some system views are a thin wrapper over a set-returning function (SRF):
- `pg_stat_activity` is defined as a `SELECT ... FROM pg_stat_get_activity(NULL) AS S ...` and then joined to `pg_database`/`pg_authid` for names (src/backend/catalog/system_views.sql:900-926).

This pattern is used when:
- the underlying data comes from shared memory / internal state requiring an SRF to materialize rows (e.g., "one row per backend") (src/backend/catalog/system_views.sql:900-926).

#### 1.3 Where SRFs / pg_stat_get_* functions are implemented
- Many `pg_stat_get_*` scalar functions used by `pg_stat_all_tables` are generated via macros in `src/backend/utils/adt/pgstatfuncs.c` (src/backend/utils/adt/pgstatfuncs.c:37-90).
  - Example macro-generated functions include:
    - `pg_stat_get_dead_tuples` (src/backend/utils/adt/pgstatfuncs.c:55-58)
    - `pg_stat_get_ins_since_vacuum` (src/backend/utils/adt/pgstatfuncs.c:58-61)
    - `pg_stat_get_mod_since_analyze` (src/backend/utils/adt/pgstatfuncs.c:64-67)

- SRFs that return record sets (e.g., `pg_stat_get_activity`) are implemented explicitly as materialized SRFs in `pgstatfuncs.c` using `InitMaterializedSRF()` and filling a tuplestore (src/backend/utils/adt/pgstatfuncs.c:250-460).

#### 1.4 How SRFs are registered / typed
- Built-in function signatures (including SRFs returning `record` with named OUT parameters) are declared in `src/include/catalog/pg_proc.dat`.
  - Scalar functions used by `pg_stat_all_tables` (e.g., `pg_stat_get_dead_tuples`, `pg_stat_get_mod_since_analyze`, `pg_stat_get_ins_since_vacuum`) are declared with `prorettype => int8` and `proargtypes => oid` (src/include/catalog/pg_proc.dat:5568-5600).
  - `pg_stat_get_activity` is declared as `proretset => 't'`, `prorettype => 'record'`, with `proallargtypes`, `proargmodes`, and `proargnames` defining its output columns (src/include/catalog/pg_proc.dat:5653-5663).

#### 1.5 Where “computed thresholds” exist today (autovacuum)
- Autovacuum’s threshold computations are not exposed directly via an existing `pg_stat_*` view; the formulas are implemented in autovacuum code:
  - `relation_needs_vacanalyze()` describes and computes:
    - vacuum threshold: `vacthresh = vac_base_thresh + vac_scale_factor * reltuples`, with optional max threshold clamp (src/backend/postmaster/autovacuum.c:2975-2993, 3110-3114)
    - insert-vacuum threshold: `vacinsthresh = vac_ins_base_thresh + vac_ins_scale_factor * reltuples * pcnt_unfrozen` (src/backend/postmaster/autovacuum.c:3091-3118)
    - analyze threshold: `anlthresh = anl_base_thresh + anl_scale_factor * reltuples` (src/backend/postmaster/autovacuum.c:2975-2993, 3116-3118)
  - The decision flags are computed from those thresholds:
    - `*dovacuum = force_vacuum || (vactuples > vacthresh) || (vac_ins_base_thresh >= 0 && instuples > vacinsthresh);` (src/backend/postmaster/autovacuum.c:3186-3190)
    - `*doanalyze = (anltuples > anlthresh);` (src/backend/postmaster/autovacuum.c:3190)

Supporting inputs used by those formulas:
- Table-level reloptions for autovacuum are modeled as `AutoVacOpts` in `src/include/utils/rel.h` (src/include/utils/rel.h:311-345).
- Autovacuum code extracts reloptions for a table with `extract_autovac_opts()` and contains explicit logic for TOAST reloption fallback to the parent table via `table_toast_map` (src/backend/postmaster/autovacuum.c:2720-2860).
- Unfrozen percentage (`pcnt_unfrozen`) uses `pg_class.relpages` and `pg_class.relallfrozen`, with clamping when `relallfrozen > relpages` (src/backend/postmaster/autovacuum.c:3074-3108).


### 2) Privileges and visibility model (pg_read_all_stats)

#### 2.1 Predefined roles and symbols
- `pg_read_all_stats` is a predefined role (OID 3375) defined in `src/include/catalog/pg_authid.dat` and also has the C symbol `ROLE_PG_READ_ALL_STATS` via `oid_symbol` (src/include/catalog/pg_authid.dat:49-60).
- Related predefined roles in the same area include `pg_monitor` and `pg_stat_scan_tables` (src/include/catalog/pg_authid.dat:41-69).

#### 2.2 “Stats permissions” macro used by pgstat SRFs
- `src/backend/utils/adt/pgstatfuncs.c` defines a helper macro:
  - `HAS_PGSTAT_PERMISSIONS(role) (has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS) || has_privs_of_role(GetUserId(), role))` (src/backend/utils/adt/pgstatfuncs.c:37).
- This macro is used to restrict visibility of some SRF output columns.
  - Example: `pg_stat_get_progress_info` returns `pid` and `datid` to all callers, but only exposes the target `relid` and progress counters to callers passing `HAS_PGSTAT_PERMISSIONS(beentry->st_userid)` (src/backend/utils/adt/pgstatfuncs.c:300-360).
  - Example: `pg_stat_get_activity` exposes some columns to all callers (pid/datid/usesysid/appname/xids), while gating “state” and “query” values behind `HAS_PGSTAT_PERMISSIONS(beentry->st_userid)` (src/backend/utils/adt/pgstatfuncs.c:380-460).

#### 2.3 View-level privilege patterns in system_views.sql
- `system_views.sql` sometimes explicitly revokes public access and grants view/function access to `pg_read_all_stats`.
  - Example restricted stats-adjacent views:
    - `pg_shmem_allocations` is created from `pg_get_shmem_allocations()` and then `REVOKE ALL ... FROM PUBLIC; GRANT SELECT ... TO pg_read_all_stats; REVOKE EXECUTE ON FUNCTION ... FROM PUBLIC; GRANT EXECUTE ... TO pg_read_all_stats;` (src/backend/catalog/system_views.sql:662-670).
    - Similar REVOKE/GRANT patterns exist for `pg_shmem_allocations_numa`, `pg_dsm_registry_allocations`, and `pg_backend_memory_contexts` (src/backend/catalog/system_views.sql:672-693).

#### 2.4 Role membership wiring for “monitor” roles
- `system_functions.sql` includes grants that make `pg_monitor` inherit other predefined roles:
  - `GRANT pg_read_all_settings TO pg_monitor;`
  - `GRANT pg_read_all_stats TO pg_monitor;`
  - `GRANT pg_stat_scan_tables TO pg_monitor;`
  (src/backend/catalog/system_functions.sql:811-815).


### 3) Regression test patterns (stats/views/privileges)

#### 3.1 System view definition snapshot tests (rules.sql)
- `src/test/regress/sql/rules.sql` includes a section that queries `pg_views` and `pg_rules` for `schemaname = 'pg_catalog'` and orders by name (src/test/regress/sql/rules.sql:772-796).
  - This produces a stable dump of built-in view definitions in `src/test/regress/expected/rules.out`.
  - Any new system view under `pg_catalog` is expected to appear in that ordered output.

#### 3.2 Privilege regression tests for pg_read_all_stats (privileges.sql)
- `src/test/regress/sql/privileges.sql` contains general role/SET ROLE mechanics tests involving `pg_read_all_stats` (granted with SET = false, and verifying `SET ROLE pg_read_all_stats` is denied) (src/test/regress/sql/privileges.sql:114-134).
- It also contains a dedicated test block checking SELECT privileges on certain system views that are only granted to `pg_read_all_stats`, including verifying that the underlying functions can be executed when selecting from the view (src/test/regress/sql/privileges.sql:1940-2015).

#### 3.3 Stats behavior tests (stats.sql)
- `src/test/regress/sql/stats.sql` uses a consistent pattern for stable assertions:
  - capture a stats snapshot (`SET LOCAL stats_fetch_consistency = snapshot;`)
  - run DML / operations
  - force stats flush (`SELECT pg_stat_force_next_flush();`)
  - re-check `pg_stat_*` views and compare counters (src/test/regress/sql/stats.sql:24-80, 98-140).
- This file also uses direct `pg_stat_all_tables` queries for specific behaviors, e.g., verifying `last_seq_scan` / `last_idx_scan` timestamps (src/test/regress/sql/stats.sql:312-372).


### 4) Documentation locations/patterns for new collected statistics views

#### 4.1 Collected stats views are documented in monitoring.sgml
- The “Collected Statistics Views” summary table is in `doc/src/sgml/monitoring.sgml` as `<table id="monitoring-stats-views-table">` (doc/src/sgml/monitoring.sgml:426-590).
  - Each view typically has a row with a short description and a link to a dedicated section (e.g., `pg_stat_all_tables` uses `linkend="monitoring-pg-stat-all-tables-view"`) (doc/src/sgml/monitoring.sgml:528-541).
- Detailed documentation per collected stats view appears later as its own `<sect2>`.
  - Example: `pg_stat_all_tables` has a dedicated `<sect2 id="monitoring-pg-stat-all-tables-view">` and a full column table (doc/src/sgml/monitoring.sgml:3850-3925).

#### 4.2 System-views.sgml cross-references monitoring.sgml for stats views
- `doc/src/sgml/system-views.sgml` explicitly notes that accumulated statistics views are not documented in that chapter, but in the “Collected Statistics Views” table in `monitoring.sgml` (doc/src/sgml/system-views.sgml:24-40).


## Code References
- src/backend/catalog/system_views.sql:695-775 — `pg_stat_all_tables` and related per-table stats views; existing use of `pg_stat_get_dead_tuples`, `pg_stat_get_ins_since_vacuum`, `pg_stat_get_mod_since_analyze`.
- src/backend/catalog/system_views.sql:662-693 — Example of system-view privilege gating via REVOKE/GRANT to `pg_read_all_stats`.
- src/backend/utils/adt/pgstatfuncs.c:37 — `HAS_PGSTAT_PERMISSIONS()` macro based on `ROLE_PG_READ_ALL_STATS`.
- src/backend/utils/adt/pgstatfuncs.c:300-460 — Materialized SRF patterns and permission-gated output fields (progress/activity).
- src/include/catalog/pg_proc.dat:5540-5690 — Built-in function declarations for `pg_stat_get_*` scalar functions and SRFs returning `record`.
- src/include/catalog/pg_authid.dat:41-69 — Predefined roles: `pg_monitor`, `pg_read_all_settings`, `pg_read_all_stats`, `pg_stat_scan_tables`.
- src/backend/catalog/system_functions.sql:811-815 — Role membership wiring for `pg_monitor`.
- src/include/utils/rel.h:311-345 — `AutoVacOpts` definition (reloptions fields used by autovacuum threshold logic).
- src/backend/postmaster/autovacuum.c:2720-2920 — Reloptions extraction and TOAST fallback behavior in `table_recheck_autovac()`.
- src/backend/postmaster/autovacuum.c:2959-3235 — Threshold computations and due-flag decisions in `relation_needs_vacanalyze()`.
- src/test/regress/sql/rules.sql:772-796 — Regression snapshot of `pg_views`/`pg_rules` for `pg_catalog`.
- src/test/regress/sql/privileges.sql:114-134 — `pg_read_all_stats` SET ROLE mechanics checks.
- src/test/regress/sql/privileges.sql:1940-2015 — View SELECT privilege checks gated by `pg_read_all_stats`.
- src/test/regress/sql/stats.sql:24-140 — Stats test patterns (snapshot consistency, flushing, asserting counters).
- doc/src/sgml/monitoring.sgml:426-590 — Collected Statistics Views overview table.
- doc/src/sgml/monitoring.sgml:3850-3925 — Example detailed per-view documentation section.
- doc/src/sgml/system-views.sgml:24-40 — Cross-reference to monitoring.sgml for stats views.

## Architecture Documentation (as implemented today)
- Collected stats views in `pg_catalog` are primarily defined in `system_views.sql` and are composed from:
  - catalog tables (`pg_class`, `pg_namespace`, `pg_index`)
  - scalar `pg_stat_get_*` functions (cumulative stats)
  - SRFs for shared-memory-backed rowsets (e.g., backend activity, progress)
  (src/backend/catalog/system_views.sql:695-775; src/backend/utils/adt/pgstatfuncs.c:250-460).
- Autovacuum “due” decisions are internal to the postmaster autovacuum subsystem and are computed from a combination of:
  - reloptions (`AutoVacOpts`) and autovacuum GUCs
  - catalog estimates (`pg_class.reltuples`, `pg_class.relpages`, `pg_class.relallfrozen`)
  - cumulative stats counters (`PgStat_StatTabEntry` values)
  (src/include/utils/rel.h:311-345; src/backend/postmaster/autovacuum.c:2998-3235).
- Stats visibility and privilege behavior appears in two layers:
  - view/function ACLs set in `system_views.sql` / `system_functions.sql` for some objects
  - per-row / per-column masking in SRFs using `HAS_PGSTAT_PERMISSIONS()` for "sensitive" backend-level information
  (src/backend/catalog/system_views.sql:662-693; src/backend/catalog/system_functions.sql:811-815; src/backend/utils/adt/pgstatfuncs.c:37, 300-460).

## Open Questions
- None for the requested mapping; the relevant existing wiring points and conventions were located in the files referenced above.
