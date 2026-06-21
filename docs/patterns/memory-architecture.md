# Memory Architecture

## Three Allocator Systems

### CBMArena — Bump Allocator
`src/foundation/arena.c:58-76`

- 64KB blocks, 2x growth, max 256 blocks
- Never frees individual allocations; bulk free at end is O(1)
- `cbm_arena_reset()` keeps first block, frees rest — enables per-file reuse
- Used for: per-file extraction allocations, cross-LSP registries

### cbm_slab_alloc — Thread-Local Slab
`src/foundation/slab_alloc.c:107-125`

- Replaces tree-sitter's `ts_set_allocator()`
- 64KB slab pages, 1024 × 64B chunks, O(1) singly-linked free lists
- Rationale: `SubtreeHeapData` is exactly 64 bytes. Without slab, ptmalloc2 creates per-thread arenas that fragment to ~321GB VSZ with 12 workers
- Per-file: `cbm_slab_reclaim()` frees all slabs after `ts_tree_delete` + `ts_parser_delete`

### mimalloc — General Purpose
`src/foundation/mem.c:109-176`

- Linked via `MI_OVERRIDE=1` in production
- `mi_option_set(mi_option_purge_decommits, 1)` — immediate page return
- Budget tracking: 50% RAM default, hysteresis-based pressure logging
- `cbm_mem_collect()` called between major phases

## Memory Back-Pressure
`src/pipeline/pass_parallel.c:571-578`

When RSS exceeds budget during parallel extract:
1. `mi_collect(true)` — force mimalloc to return pages
2. 3ms nap per spin (up to 40 spins)
3. Prevents OOM while peer workers finish

Documented OOM scenario at `pipeline.c:666-668`: extract phase held ~13GB reclaimable pages.

## Node Ownership Rules
`src/graph_buffer/graph_buffer.c:113-207`

| Field | Ownership | Free Strategy |
|-------|-----------|---------------|
| `name`, `qualified_name`, `properties_json` | Heap (strdup) | `free_node_strings()` |
| `label`, `file_path` | Intern pool | NOT freed by `free_node_strings()` |
| Edge `type` | Intern pool | NOT freed |
| Edge `properties_json` | Heap | `free_edge_strings()` |

**Transfer pattern**: `memcpy(&dest, &src, sizeof(...))` + `memset(&src, 0, sizeof(...))` — shallow copies struct, zeros source (transferring ownership). Free the outer array with `free()`, NOT `cbm_store_free_nodes()`.

## Concurrency Model

### Work-Stealing Pool
`src/pipeline/worker_pool.c:35-93`

- Shared `_Atomic int next_idx` counter
- Each worker `atomic_fetch_add` to claim next item
- Main thread also participates (N+1 workers)
- 8MB pthread stacks (required for deep tree-sitter AST recursion)

### Producer-Consumer Rings
`src/pipeline/pass_parallel.c:471-499`

Three-phase parallel pipeline:
1. **Parallel Extract**: Worker-local `cbm_gbuf_t` with shared atomic IDs. Cache-line-aligned state to prevent false sharing.
2. **Serial Registry Build**: Single-threaded — hash table mutation under insert.
3. **Parallel Resolve**: Shared read-only registry + cross-LSP registries. Worker-local edge buffers, merged serially after.

### Global Index Lock
`src/pipeline/pipeline.c:53-69`

- Atomic spinlock: `cbm_pipeline_try_lock()` (non-blocking, watcher) / `cbm_pipeline_lock()` (100ms retry, MCP)
- Single-process guard only — no cross-process coordination

## Graph Buffer Design
`src/graph_buffer/graph_buffer.c:60-109`

- Pointers to individually heap-allocated nodes (stable across realloc)
- 7 Verstable hash tables: QN→node, ID→node, label→nodes, name→nodes, edge dedup, edge source/type, edge target/type, edge type
- String intern pool for repetitive fields (label, file_path, edge type)
- Edge dedup: composite key `"sourceID:targetID:type"`, property merge on conflict

## Dump to SQLite
`src/graph_buffer/graph_buffer.c:1412-1501`

- Direct B-tree page construction (`sqlite_writer.c`) bypasses SQL INSERT overhead
- 64K-node partitions, with `free_heavy` logic under memory pressure
- Alternative: `cbm_gbuf_flush_to_store()` uses standard CRUD API for incremental updates
