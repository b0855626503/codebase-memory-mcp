# ADR-0003: Symbol Resolution in trace_path

## Status

Accepted

## Date

2026-06-21

## Context

The `trace_path` tool required an exact `qualified_name` to trace call paths. Users had to know and type the full FQN:

```
trace_path(function_name="App\\Services\\WalletService::deposit")
```

This created friction: the standard workflow was `search_graph` → copy FQN → `trace_path`. Users wanted to type short names:

```
trace_path(symbol="deposit")
trace_path(symbol="CircuitBreaker.call")
```

## Decision

**Add a `symbol` parameter to `trace_path`** that resolves short names to qualified_names before tracing.

**Resolution strategy** (in priority order):
1. Exact `qualified_name` match via `cbm_store_find_node_by_qn()` — if found, auto-resolve immediately
2. `qualified_name LIKE '%symbol%'` via new `cbm_store_find_nodes_by_qn_contains()` — scoped to Function/Method labels, ranked by exact QN → exact name → contains, LIMIT 20

**Behavior**:
- 1 result → auto-resolve and trace immediately
- 2+ results → return candidate list with name, qualified_name, label, file_path
- 0 results → error with hint to use `search_graph`

**Why single query?** The original design used two queries (name match + QN contains) with complex node ownership transfer. This caused a double-free bug (commit `d1e2040`). The simplified single-query approach eliminates the transfer complexity — QN contains naturally covers exact name matches since all names appear in their qualified_name.

**New store function**: `cbm_store_find_nodes_by_qn_contains()` (`src/store/store.c`) — parameterized SQL query with dedup and ranking, LIMIT 20 to bound result size.

## Considered Options

### Option 1: QN LIKE query (chosen)

- **Pros**: Simple, single query, leverages existing SQLite, no new dependencies
- **Cons**: Full scan of nodes table for LIKE query (mitigated by project filter + LIMIT 20)

### Option 2: Separate name + QN queries with node transfer

- **Pros**: Name match is index-assisted (uses `idx_nodes_name`)
- **Cons**: Complex node ownership transfer between arrays caused double-free. More code, more bugs.

### Option 3: Fuzzy search (Levenshtein, trigram, semantic)

- **Pros**: Handles typos, natural language queries
- **Cons**: Premature — the immediate pain point is FQN requirement, not search quality. Can be added later as a separate resolution strategy.

## Consequences

### Positive

- `symbol="deposit"` → resolves 20 candidates across Game repositories
- `symbol="CircuitBreaker.fromConfig"` → auto-resolves and traces 2 callers
- `symbol="CircuitBreaker.call"` → auto-resolves from the LIKE query
- Backward compatible: `function_name` parameter still works

### Negative

- LIKE query performs full scan on `qualified_name` column (no leading-wildcard index possible)
- LIMIT 20 bounds the worst case, but for very common names in massive projects, ranking may miss results

## Related Decisions

- ADR-0001: Minified JS exclusion (made symbol resolution more effective by removing noise matches)
- ADR-0002: Qualified_name labeling

## References

- Commit: `796061f` — feat(trace_path): symbol resolution
- Commit: `0aef7f5` — fix: simplify symbol resolver, remove double-free source
- Commit: `d1e2040` — fix: prevent double-free after single-match auto-resolve
- `src/store/store.c` — `cbm_store_find_nodes_by_qn_contains()`
- `src/mcp/mcp.c:2337-2384` — symbol resolution block in `handle_trace_call_path`
