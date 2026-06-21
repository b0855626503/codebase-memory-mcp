# Developer Guide

## Prerequisites

- GCC 13.3+ (C11) and G++ (C++14)
- `make`, `pkg-config`
- Optional: `libgit2-dev` (faster git history), `mimalloc`

```bash
sudo apt install build-essential pkg-config libgit2-dev
```

## Quick Start

```bash
git clone https://github.com/DeusData/codebase-memory-mcp.git
cd codebase-memory-mcp

# Build production binary
make -f Makefile.cbm cbm -j$(nproc)

# Run MCP server on stdio
./build/c/codebase-memory-mcp

# Install globally
./build/c/codebase-memory-mcp install
```

## Build Commands

| Command | Purpose |
|---------|---------|
| `make -f Makefile.cbm cbm -j$(nproc)` | Production binary |
| `make -f Makefile.cbm test -j$(nproc)` | Test build with ASan+UBSan |
| `make -f Makefile.cbm test-tsan -j$(nproc)` | Thread sanitizer build |
| `make -f Makefile.cbm clean-c` | Remove build artifacts |
| `make -f Makefile.cbm frontend` | Build web UI (requires Node.js) |
| `make -f Makefile.cbm cbm-with-ui` | Production binary with embedded UI |

### Build Flags

- **Prod**: `-std=c11 -O2` with mimalloc (`MI_OVERRIDE=1`)
- **Test**: `-std=c11 -g -O1 -fsanitize=address,undefined`
- **Vendored**: `GRAMMAR_CFLAGS` with `-w` (suppress warnings in generated code)

### Test Runner

```bash
make -f Makefile.cbm test -j$(nproc)
./build/c/test-runner                    # All tests
./build/c/test-runner --suite store      # Store tests only
./build/c/test-runner --suite pipeline   # Pipeline tests only
```

Add new test files to `Makefile.cbm` test target.

## Project Structure

```
src/
├── foundation/     # Arena, slab, platform, hashing, logging, string utils
├── store/          # SQLite CRUD, search, vector, BFS, architecture queries
├── graph_buffer/   # In-memory graph with hash table indexes
├── pipeline/       # 7-phase index pipeline, 16 pass_*.c files
├── cypher/         # Cypher query parser + executor
├── discover/       # File discovery, language detection, .gitignore matching
├── mcp/            # MCP JSON-RPC server, 22 tool handlers
├── cli/            # install/update/config CLI
├── ui/             # Optional HTTP server + embedded web UI
├── git/            # Git context (branch info, change detection)
├── traces/         # Runtime trace ingestion
├── semantic/       # Algorithmic code embeddings (TF-IDF, RI, pretrained)
├── simhash/        # MinHash + SimHash for code clone detection
├── watcher/        # File watcher for auto-reindex
└── main.c          # Entry point
internal/cbm/       # Vendored extraction engine (tree-sitter grammars, LSP resolvers)
vendored/           # Third-party: yyjson, sqlite3, mimalloc, xxhash
tests/              # Test runner + test_*.c files
```

## Key Patterns

### Adding a new MCP tool

1. Add entry to `TOOLS[]` array in `src/mcp/mcp.c` (name, description, JSON Schema)
2. Write handler function: extract args → resolve store → do work → format result
3. Add dispatch case in `cbm_mcp_handle_tool()` at `mcp.c:4736`
4. Free all heap-allocated args before returning

### Adding a new pipeline pass

1. Create `src/pipeline/pass_foo.c` with function `cbm_pipeline_pass_foo(ctx)`
2. Register in `run_predump_passes()` or `run_extraction_phase()` in `pipeline.c`
3. Pass reads from `ctx->gbuf` (graph buffer), writes nodes/edges

### Adding a new store query

1. Define SQL with `?N` parameter placeholders
2. Use `prepare_cached(s, &s->stmt_foo, SQL)` for statement reuse
3. Bind parameters with `sqlite3_bind_*()` using `MCP_SQLITE_TRANSIENT`
4. Iterate with `sqlite3_step()`, extract with `sqlite3_column_*()`
5. Call `sqlite3_reset()` + `sqlite3_clear_bindings()` for reuse

### Memory ownership

- `cbm_node_t` fields: `name`, `qualified_name`, `properties_json` → heap (free with `cbm_store_free_nodes`)
- `cbm_node_t` fields: `label`, `file_path` → intern pool (do NOT free)
- Edge `type` → intern pool; `properties_json` → heap
- Transfer ownership: `memcpy(&dest, &src, sizeof(...))` + `memset(&src, 0, sizeof(...))`

## Deployment

```bash
# Build and deploy (stop MCP server first)
make -f Makefile.cbm cbm -j$(nproc)
cp build/c/codebase-memory-mcp ~/.local/bin/codebase-memory-mcp

# Or use the installer
./build/c/codebase-memory-mcp install
```

The MCP server runs on stdio. It is launched by Claude Code / Claude Desktop automatically when configured in `claude_desktop_config.json` or `.claude/settings.json`.

## Debugging

```bash
# Build with ASan
make -f Makefile.cbm test -j$(nproc)

# Run under GDB
gdb --args ./build/c/test-runner --suite store

# Run server with verbose logging
CBM_LOG_LEVEL=debug ./build/c/codebase-memory-mcp

# Check memory with valgrind
valgrind --leak-check=full ./build/c/test-runner --suite store
```

## Cache Locations

| Path | Purpose |
|------|---------|
| `~/.cache/codebase-memory-mcp/<project>.db` | Index database |
| `~/.cache/codebase-memory-mcp/_config.db` | UI config, process state |
| `~/.config/codebase-memory-mcp/config.json` | User extensions, auto_index |
| `<repo>/.cbmignore` | Per-project file exclusion |
| `<repo>/.codebase-memory.json` | Per-project extension overrides |
| `<repo>/.codebase-memory/graph.db.zst` | Compressed artifact (team sharing) |

Environment variable `CBM_CACHE_DIR` overrides the cache directory.

## Benchmarking

When making index-affecting changes:

1. Delete DB + artifact to force full reindex
2. Re-index a real 30k+ node project (not self-index)
3. Compare before/after: node count, edge count, Function count
4. Run: trace_path, search_graph (semantic), check_architecture_rules, smart_analyze
5. Verify: no single-char function names in results, semantic search returns domain methods

## Related Documents

- [CLAUDE.md](../CLAUDE.md) — Build commands and codebase overview for Claude Code
- [Architecture Overview](architecture-overview.md) — System diagrams and layer stack
- [Pipeline State Machine](domain/pipeline-state-machine.md) — Index pipeline internals
- [Memory Architecture](patterns/memory-architecture.md) — Allocator design
- [Data Model](patterns/data-model.md) — SQLite schema and indexes
- [ADR-0001](adr/0001-exclude-minified-js-css-in-all-modes.md) — Minified JS exclusion
