# Performance Analysis

## Indexing Hot Path

The parallel pipeline (`pass_parallel.c`) dominates indexing time. Design:

- **Work distribution**: Atomic `next_file_idx` counter, contention-free work-stealing. Main thread participates (N+1 workers).
- **File sorting**: Descending by size before dispatch — biggest files first, minimizing tail latency.
- **Worker-local graph buffers**: No mutex contention; shared atomic ID counter ensures unique node IDs.
- **File caching**: Each file read+parsed ONCE; `CBMFileResult` cached in `result_cache[]` for subsequent phases.
- **Source retention**: Up to 25% RAM budget for source code in result arena — enables fused cross-file LSP without re-reading from disk.

### Memory Back-Pressure
`pass_parallel.c:571-578`: RSS check → `mi_collect(true)` → 3ms nap (up to 40 spins). Documented at `pipeline.c:666-668`: extract phase held ~13GB reclaimable arenas on large repos.

### Fused Resolve
Phase 4 (`pass_parallel.c:984-1067`) eliminates separate `pass_lsp_cross` — previously ~520s on kubernetes (`pipeline.c:689`). Cross-LSP registries pre-built once in arena, shared read-only across all workers.

## Query Hot Paths

### Vector Search — O(n·d) Brute Force
`store.c:5855-5862`: Full scan of `node_vectors` table. No project index, no vector index (no ANN/LSH/HNSW). Cosine computed per-row via `cbm_cosine_i8()` — 768-dimensional int8 dot product + magnitudes.

- **Multi-keyword**: Client-side re-ranking via bubble sort O(k²) at `store.c:5912-5920`
- **Fetch limit**: `limit × 5` candidates pre-fetched
- **For 40k-function project**: ~40k × 768 × 4 ops ≈ 123M operations per query

### Graph Search — Per-Row Degree Subqueries
`store.c:2512-2517`: Two correlated subqueries per result row:
```sql
(SELECT COUNT(*) FROM edges e WHERE e.target_id=n.id AND e.type IN ('CALLS','USAGE')) AS in_deg,
(SELECT COUNT(*) FROM edges e WHERE e.source_id=n.id AND e.type IN ('CALLS','USAGE')) AS out_deg
```
With proper composite indexes (`idx_edges_source`, `idx_edges_target`), these are O(log N) B-tree lookups. For 200 results: 400 probes.

### Architecture Hotspots — json_extract Per-Row
`store.c:3480`: `json_extract(properties, '$.is_test')` is evaluated per-row with no index possible. Combined with `file_path NOT LIKE '%test%'` (leading-wildcard LIKE, no index usable).

### BFS Traversal
`store.c:2745`: SQLite recursive CTE with depth limit. Uses `idx_edges_source`/`idx_edges_target`. Depth-limited — bounded by average_degree × max_depth, typically < 1000 nodes.

## Caching

| Cache | Strategy | TTL |
|-------|----------|-----|
| File hashes | mtime+size in SQLite (`file_hashes` table) | Until file changes |
| Regex compile | `sqlite3_set_auxdata` per-statement | Statement lifetime |
| Import map | Built from graph buffer per-extraction | One extraction run |
| Registry resolve | Pre-built inverted index | One resolve phase |
| Cross-LSP | Arena-allocated per-language registries | One pipeline run |

**No query result cache, no LRU cache, no cross-query cache.**

## Bottlenecks and Fixes

| Bottleneck | Location | Complexity | Fix |
|---|---|---|---|
| Vector full scan | `store.c:5855` | O(n·d) | `CREATE INDEX ON node_vectors(project)` |
| Bubble sort re-rank | `store.c:5912` | O(k²) | Replace with `qsort()` |
| Degree subqueries | `store.c:2512` | O(log n) per row | Pre-aggregate or materialize |
| `json_extract` | `store.c:3480` | Per-row parse | Promote `is_test` to column |
| No vector index | `store.c:5855` | O(n) scan | Consider ANN (FAISS/hnswlib) |
| No query cache | — | Repeat computation | LRU query cache with mtime invalidation |

## Memory Budget

- **mimalloc**: 50% of total physical RAM (`mem.c:127`)
- **RSS tracking**: `mi_process_info()` with hysteresis-based logging
- **Configurable**: `CBM_MEM_BUDGET_FRACTION` environment variable
- **Collection points**: After extraction, after registry build, after dump

## Complexity Metrics

**At extraction** (O(1) per function): cyclomatic, cognitive, loop_count, loop_depth, linear_scan_in_loop, alloc_in_loop, recursive flags

**Post-hoc** (`pass_complexity.c:104-138`): `transitive_loop_depth` via memoized DFS on CALLS graph. O(V+E). States: 0=unvisited, 1=in-progress (cycle), 2=done. Depth limit 256.

**Storage**: All metrics in unindexed `properties` JSON blob. Not promoted to indexed columns — full-scan required for filtering.
