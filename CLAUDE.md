# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
# Production binary
make -f Makefile.cbm cbm -j$(nproc)

# Test build with ASan + UBSan
make -f Makefile.cbm test -j$(nproc)

# Thread sanitizer build
make -f Makefile.cbm test-tsan -j$(nproc)

# Clean
make -f Makefile.cbm clean-c

# Deploy (after stopping running MCP server)
cp build/c/codebase-memory-mcp ~/.local/bin/codebase-memory-mcp
```

**Binary location**: `build/c/codebase-memory-mcp` (prod) / `build/c/test-runner` (test).
**Deploy target**: `~/.local/bin/codebase-memory-mcp` (MCP server binary).

**Compiler**: GCC 13.3+ with `-std=c11` and `-std=c++14` (preprocessor only). Uses `-Werror` in test builds, `-w` for vendored grammar/LSP object files via `GRAMMAR_CFLAGS`.

## Architecture

The project is a **single-binary MCP server** that indexes codebases into a SQLite knowledge graph, then serves graph queries over JSON-RPC on stdio.

### Layer stack (top-to-bottom)

```
MCP handler        src/mcp/mcp.c         — JSON-RPC tool dispatch, search, trace, incidents
CLI                src/cli/cli.c         — install/update/config CLI commands
Pipeline           src/pipeline/         — multi-pass index orchestrator
Graph buffer       src/graph_buffer/     — in-memory graph before SQLite dump
Store              src/store/store.c     — SQLite CRUD, search, vector search, architecture queries
Cypher             src/cypher/           — Cypher query engine (MATCH...RETURN)
Discovery           src/discover/         — file walk, gitignore, language detection
Foundation          src/foundation/       — arena, str, platform, logging, compat
```

### Index pipeline (src/pipeline/pipeline.c `cbm_pipeline_run`)

1. **Discover** files via `cbm_discover_ex()` — applies hardcoded dir/suffix filters, `.gitignore`, `.cbmignore`
2. **Extract** definitions/calls/imports via tree-sitter (vendored in `internal/cbm/`)
3. **LSP resolve** — per-file + cross-file type-aware call resolution (Go, PHP, Python, C/C++, TypeScript, Java, etc.)
4. **Semantic edges** — SIMILAR_TO (MinHash+SimHash), SEMANTICALLY_RELATED (nomic-embed-code vectors)
5. **Post-passes** — tests, communities (Leiden clustering), HTTP routes, git history, complexity
6. **Dump** to SQLite + optional `.codebase-memory/graph.db.zst` artifact

### File discovery filtering (src/discover/discover.c)

Three tiers of exclusion:
- **Hardcoded**: `ALWAYS_SKIP_DIRS` (`.git`, `node_modules`, `vendor`...), `ALWAYS_IGNORED_SUFFIXES` (`.pyc`, `.png`, `.min.js`, `.min.css`...)
- **Mode-dependent**: `FAST_SKIP_DIRS` (applies to MODERATE/FAST, skipped in FULL), `FAST_IGNORED_SUFFIXES`
- **Config-based**: `.gitignore`, `.cbmignore` (gitignore syntax at repo root), `.git/info/exclude`

The `.cbmignore` file is the recommended way to add per-project exclusions. Patterns are gitignore-syntax, case-sensitive. The file is loaded automatically from repo root if it exists.

### Key data flow

- Config: `~/.cache/codebase-memory-mcp/_config.db` (SQLite) + `~/.config/codebase-memory-mcp/config.json` (user extensions)
- Project DB: `~/.cache/codebase-memory-mcp/<project>.db` (resolved by `cbm_resolve_cache_dir()` in `src/foundation/platform.c:417`)
- Artifact: `<repo>/.codebase-memory/graph.db.zst` (compressed DB for team sharing)
- ADR: stored in SQLite `project_summaries` table, legacy file at `<repo>/.codebase-memory/adr.md`

### MCP tools (src/mcp/mcp.c TOOLS array)

Core: `search_graph`, `trace_path`, `get_code_snippet`, `query_graph`, `get_architecture`, `search_code`, `get_graph_schema`
Analysis: `check_architecture_rules`, `analyze_architecture_reasoning`, `smart_analyze`, `detect_dead_code`, `detect_schema_drift`
Lifecycle: `index_repository`, `index_status`, `list_projects`, `delete_project`, `save_baseline`, `create_incident`, `list_incidents`

### trace_path symbol resolution

`trace_path` accepts `symbol` (short name like `deposit`) OR `function_name` (exact QN). Symbol resolution: exact QN match → `cbm_store_find_nodes_by_qn_contains()` (QN LIKE '%symbol%', Function/Method only, LIMIT 20). Single match auto-traces; multiple matches returns candidate list.

### BM25 + Semantic search interaction

`search_graph` with `semantic_query` returns BOTH `results` (BM25/regex) and `semantic_results` (vector cosine). When only `semantic_query` is used without other filters, the regex path defaults to Function/Method/Class labels to match the vector search domain. The vector search (`cbm_store_vector_search`) already filters to `n.label IN ('Function','Method','Class')` via SQL at `store.c:5860`.

### Important patterns

- **Global index lock**: `cbm_pipeline_try_lock()` / `cbm_pipeline_lock()` — prevents concurrent pipeline runs on same DB. Atomic spinlock.
- **Incremental indexing**: `pipeline_incremental.c` — uses file hashes to skip unchanged files. Re-index is a no-op if no files changed. To force full re-index, delete both the DB and the `.codebase-memory/graph.db.zst` artifact.
- **Memory**: Arena-based allocation in extraction (`CBMArena`), `safe_realloc` wrapper (frees old on failure, returns NULL) in `platform.h`, mimalloc in prod builds.
- **Node ownership transfer**: When moving `cbm_node_t` between arrays, use memcpy + memset(0) on source. Free the outer array with `free()`, not `cbm_store_free_nodes()` (which frees inner strings + array). Prefer `cbm_store_free_nodes()` for the final owner.
- **`CBMType` union**: Named `data` (not anonymous) — access members as `type->data.named.qualified_name`, NOT `type->named.qualified_name`. Defined in `internal/cbm/lsp/type_rep.h:55-145`.

### Testing

Tests use a custom runner (`tests/test_main.c`) that discovers and runs all `test_*.c` files. Add new test files to `Makefile.cbm`'s test target. ASan+UBSan enabled in test builds.

```bash
# Run all tests
make -f Makefile.cbm test -j$(nproc) && ./build/c/test-runner

# Run a single test category
./build/c/test-runner --suite store
```

### Vendored code

- `internal/cbm/` — tree-sitter grammars, LSP resolvers, extraction engine. Single-compilation-unit via `lsp_all.c`.
- `vendored/` — yyjson, sqlite3, mimalloc, xxhash, nomic embeddings
- The LSP files in `internal/cbm/lsp/` are included via `lsp_all.c` and compiled with `GRAMMAR_CFLAGS` (separate from main build flags, uses `-w`).

### Benchmark methodology

When making changes that affect indexing, always: (1) delete DB + artifact to force full re-index, (2) verify with real 30k+ node projects (not just the codebase-memory-mcp self-index), (3) compare node/edge counts before/after, (4) run trace_path, search_graph (semantic), check_architecture_rules, and smart_analyze to verify signal quality.
