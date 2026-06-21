# ADR-0004: Semantic Domain Scoping for Hybrid Search Results

## Status

Accepted

## Date

2026-06-21

## Context

`search_graph` with `semantic_query` returns TWO result sets in a single response:

- `results` — from the regex/BM25 search path (`cbm_store_search`)
- `semantic_results` — from the vector cosine search (`cbm_store_vector_search`)

The vector search correctly filters to `Function`, `Method`, `Class` labels (`store.c:5860`). The regex path had NO label filter when no explicit filters were specified — returning ALL node labels including `Variable`, `Decorator`, and `File`.

This caused a confusing user experience: `semantic_query=["wallet","debit","credit"]` returned clean `semantic_results` (PHP methods like `CheckInRewardAdminController.update`) alongside polluted `results` (SCSS variables like `$bg-primary`, `$active-color`, `$blue`). Users perceived semantic search as broken when the actual issue was output mixing.

The root cause investigation found:

1. Semantic embeddings were clean — `cbm_store_vector_search` correctly filters by `Function/Method/Class`
2. The `semantic_results` field was always correct
3. The `results` field from the regex path had no default label filter
4. SCSS Variable nodes (~1,225 nodes) were dominating the unfiltered regex output

## Decision

**When `semantic_query` is provided without explicit search filters (`label`, `name_pattern`, `qn_pattern`, `file_pattern`), default the regex/BM25 path to `Function,Method,Class` labels** — matching the semantic search domain.

Implementation (`src/mcp/mcp.c` — `handle_search_graph`):
```c
if (!label && !name_pattern && !qn_pattern && !file_pattern) {
    // Check if semantic_query is active
    if (has_semantic_query(args)) {
        label = heap_strdup("Function,Method,Class");
    }
}
```

## Considered Options

### Option 1: Scope regex results to semantic domain (chosen)

- **Pros**: Targeted, only affects semantic_query path, no behavior change for other searches
- **Cons**: Requires early JSON parse to detect semantic_query presence

### Option 2: Exclude `.scss`/`.sass`/`.css` from indexing

- **Pros**: Removes the noise at the source
- **Cons**: CSS variables ARE legitimate graph nodes for template analysis. Excluding files fixes a symptom, not the root cause. This would set a precedent of excluding file types whenever embeddings show bias.

### Option 3: Lower Variable/Decorator BM25 ranking weight

- **Pros**: Graded approach, Variable nodes still appear if relevant
- **Cons**: More complex, requires weight tuning, doesn't fully solve the output mixing issue

### Option 4: Separate output sections (bm25_results + semantic_results)

- **Pros**: Total transparency, no data loss
- **Cons**: Breaking API change. Existing MCP clients parse the `results` field.

## Consequences

### Positive

- `semantic_query=["wallet","debit","credit"]` now returns empty `results` + clean `semantic_results`
- `semantic_query=["fixture","match","result"]` returns `FixtureLite.isLiveStatus`, `FixtureDetailSyncService.updateRedisCache`
- No SCSS variable pollution in combined output
- Only affects semantic_query path — no impact on name_pattern, label, or other search modes

### Negative

- The `results` field may be empty when only `semantic_query` is used (a hint message explains available labels)
- Future callers who depend on regex results alongside semantic results will get narrower results

## Related Decisions

- ADR-0001: Minified JS exclusion (same pattern — noise removal at the right layer)

## References

- Commit: `37da7a0` — fix(search_graph): scope regex results to semantic domain
- `src/store/store.c:5860` — vector search label filter
- `src/mcp/mcp.c:1619-1635` — semantic domain scoping logic
