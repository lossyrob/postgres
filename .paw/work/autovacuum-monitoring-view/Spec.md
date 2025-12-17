# Feature Specification: Autovacuum Monitoring View

**Branch**: auto  |  **Created**: 2025-12-17  |  **Status**: Draft
**Input Brief**: Expose autovacuum per-table prioritization heuristics/score for monitoring.

## Overview
Autovacuum decisions can be surprising to users because the system’s behavior depends on a mix of per-table settings, global settings, and live statistics. When a table is not vacuumed (or is vacuumed “too often”), users lack a single place to see the inputs and thresholds that autovacuum is applying at that moment. This results in guesswork, over-tuning, and difficulty diagnosing problems like table bloat, churn-heavy workloads, or wraparound risk.

This feature adds a read-only monitoring view that reports, for each eligible table, the same counters and thresholds that drive autovacuum’s “needs VACUUM / needs ANALYZE” decisions. The view makes the logic observable by returning both the raw counters (dead tuples, inserts since vacuum, modifications since analyze) and the derived threshold values that are computed from table statistics and configuration.

To make the output immediately actionable, the view also provides a derived “priority score” that indicates how far beyond each trigger a table is, allowing users to sort tables by urgency. This score is explicitly a monitoring construct: it is intended for ranking and diagnosis, and does not promise to reflect any internal worker ordering.

The view is intended for DBAs and operators. It follows the existing statistics privilege model so that it can be used safely in production without granting broad access to table contents.

## Objectives
- Enable operators to determine whether a specific table is currently due for autovacuum and/or autoanalyze, and why.
- Expose effective per-table thresholds after combining table settings with global defaults (Rationale: reduces misconfiguration and guesswork).
- Provide a single sortable value expressing urgency based on existing trigger distances (Rationale: speeds up triage across many tables).
- Preserve behavior: the feature must not change autovacuum decision-making.

## User Scenarios & Testing
### User Story P1 – Diagnose autovacuum eligibility per table
Narrative: As a DBA, I query a built-in monitoring view to see, per table, whether autovacuum considers VACUUM and/or ANALYZE due, along with the key counters and thresholds that determine those flags.
Independent Test: Query the view for a known table and confirm the due flags and thresholds match the expected formulas under controlled settings.
Acceptance Scenarios:
1. Given a table with deterministic autovacuum thresholds and known dead tuples, When I query the view, Then `vacuum_due` is true and the `vacuum_threshold` matches the configured formula.
2. Given a table with deterministic analyze thresholds and known modifications since analyze, When I query the view, Then `analyze_due` is true and the `analyze_threshold` matches the configured formula.

### User Story P1 – Rank tables by urgency
Narrative: As an operator, I sort the view by a priority score to quickly identify tables most beyond their autovacuum triggers.
Independent Test: Create two tables with different degrees of “beyond-threshold” activity and verify ordering by the score.
Acceptance Scenarios:
1. Given two eligible tables with different trigger distances, When I order by `autovacuum_priority_score` descending, Then the table with the larger trigger distance appears first.

### User Story P1 – Respect stats visibility and permissions
Narrative: As a security-conscious admin, I want access to the monitoring view to follow existing statistics permissions.
Independent Test: Verify permissions for a role without and with the appropriate statistics privilege.
Acceptance Scenarios:
1. Given a role that is not permitted to read global statistics, When it queries the view, Then the query is denied.
2. Given a role that is permitted to read global statistics, When it queries the view, Then the query succeeds.

### Edge Cases
- Tables without statistics: threshold-based values are unavailable; due flags reflect only wraparound forcing (if applicable).
- Insert-trigger vacuum disabled: insert-threshold and insert-ratio are NULL.
- Toast relations: analyze is never due; toast may inherit autovacuum settings from its parent when not set explicitly.
- Temporary tables: never included in the output.
- Partitioned tables (containers without storage): not included in the output.

## Requirements
### Functional Requirements
- FR-001: Provide a built-in monitoring view named `pg_stat_autovacuum_candidates` returning one row per eligible relation in the current database (Stories: P1).
- FR-002: The view exposes autovacuum decision flags `vacuum_due`, `analyze_due`, and `wraparound_forced`, matching the system’s existing decision logic (Stories: P1).
- FR-003: The view exposes the raw counters used in the decision: dead tuples, inserts since last vacuum, modifications since last analyze, and table size estimates used by the thresholds (Stories: P1).
- FR-004: The view exposes the computed thresholds for vacuum, insert-driven vacuum (when enabled), and analyze (Stories: P1).
- FR-005: The view exposes a derived `autovacuum_priority_score` and a textual `autovacuum_priority_reason` so users can sort and understand the highest contributing trigger (Stories: P1).
- FR-006: Access to the view follows the existing statistics privilege model; it is not readable by default by unprivileged roles (Stories: P1).
- FR-007: The feature is read-only and does not alter autovacuum behavior or configuration (Stories: P1).

### Key Entities
- Relation (table or materialized view): an eligible object for which autovacuum can consider VACUUM and/or ANALYZE.
- Statistics counters: cumulative counts that represent churn since last vacuum/analyze.
- Thresholds: derived values computed from configuration and table statistics.

### Cross-Cutting / Non-Functional
- Correctness: reported thresholds and decision flags must align with current autovacuum logic.
- Performance: the view must be safe to query on busy systems and avoid heavyweight locking.
- Stability: column meanings are stable; additions are allowed, but breaking changes are avoided.

## Success Criteria
- SC-001: For a table with controlled configuration and known churn, the view reports thresholds consistent with the published formula and flags the table as due accordingly (FR-002, FR-003, FR-004).
- SC-002: When insert-trigger vacuum is disabled for a table, `vacuum_insert_threshold` and `vacuum_insert_ratio` are NULL (FR-004).
- SC-003: When ordering by `autovacuum_priority_score` descending, tables with larger trigger distances are ranked higher in controlled tests (FR-005).
- SC-004: A role without the appropriate statistics privilege cannot read the view; a permitted role can (FR-006).

## Assumptions
- The initial release targets a minimal v1 schema that can be extended later; new columns may be added, but existing column meanings remain stable.
- The priority score is explicitly a monitoring metric, not a promise about internal worker ordering.

## Scope
In Scope:
- A built-in monitoring view: `pg_stat_autovacuum_candidates`.
- Concrete, minimal v1 columns (names/types/semantics) sufficient to explain vacuum/analyze decisions.
- Deterministic test coverage validating formulas, due flags, and privileges.

Out of Scope:
- Predicting exact scheduling time or guaranteeing the next table a worker will process.
- Exposing internal scheduling lists or internal worker bookkeeping.
- Changing autovacuum thresholds, policies, or runtime behavior.

## Dependencies
- Requires statistics tracking to populate most counters; when statistics are unavailable, the view still reports what it can (including wraparound-related fields).

## Risks & Mitigations
- Risk: Users interpret the score as “the exact next table autovacuum will pick.” Mitigation: document `autovacuum_priority_score` as derived and “monitoring only,” and provide `autovacuum_priority_reason` to clarify what it reflects.
- Risk: Stats visibility timing can cause slight discrepancies. Mitigation: document that results reflect the current stats snapshot and may differ from concurrent autovacuum decisions.

## References
- Issue: none
- Research: None (this spec is based on observed current behavior and needs verification in implementation/testing)
- External: None

## Appendix: v1 Output Schema (Concrete Columns)
The view `pg_stat_autovacuum_candidates` exposes the following columns.

Identity:
- `relid oid`
- `schemaname name`
- `relname name`
- `relkind "char"`

Eligibility/config visibility:
- `autovacuum_enabled boolean`
- `has_stats boolean`
- `autovacuuming_active boolean`

Counters and inputs:
- `dead_tuples bigint`
- `ins_since_vacuum bigint`
- `mod_since_analyze bigint`
- `reltuples real`
- `pct_unfrozen real`

Computed thresholds:
- `vacuum_threshold real`
- `vacuum_insert_threshold real` (NULL when insert-trigger vacuum is disabled)
- `analyze_threshold real`

Wraparound context:
- `xid_age integer`
- `freeze_max_age integer`
- `mxid_age integer`
- `multixact_freeze_max_age integer`

Decisions and derived ranking:
- `vacuum_due boolean`
- `analyze_due boolean`
- `wraparound_forced boolean`
- `vacuum_dead_ratio double precision`
- `vacuum_insert_ratio double precision`
- `analyze_ratio double precision`
- `autovacuum_priority_score double precision`
- `autovacuum_priority_reason text`

