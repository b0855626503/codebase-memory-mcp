# Architecture Overview

## System Diagram

```mermaid
graph TB
    subgraph "MCP Client"
        CC[Claude Code / Desktop]
        VS[VS Code]
    end

    subgraph "codebase-memory-mcp"
        direction TB

        subgraph "Transport Layer"
            JSONRPC[JSON-RPC 2.0 over stdio]
            HTTP[Optional HTTP UI :9749]
        end

        subgraph "MCP Handler"
            TOOLS[22 Tool Handlers]
            DISPATCH[Linear strcmp dispatch]
            RESOLVE[Store resolution + integrity check]
        end

        subgraph "Query Engine"
            SEARCH[search_graph<br/>BM25 + Regex + Vector]
            CYPHER[Cypher Engine<br/>MATCH...RETURN]
            BFS[BFS Traversal<br/>Recursive CTE]
            ARCH[Architecture Analysis<br/>Hotspots + Boundaries]
        end

        subgraph "Store Layer"
            SQLITE[(SQLite WAL<br/>per-project .db)]
            FTS5[FTS5 Full-Text<br/>camelCase split]
            VECTOR[Vector Search<br/>brute-force cosine]
            CUSTOM[Custom SQL Functions<br/>regexp, cosine_i8]
        end

        subgraph "Pipeline Engine"
            DISCOVER[File Discovery<br/>walk + filter + .cbmignore]
            EXTRACT[Tree-sitter Extract<br/>parallel work-stealing]
            LSP[LSP Resolve<br/>per-file + cross-file]
            SEMANTIC[Semantic Edges<br/>SIMILAR_TO + SEMANTICALLY_RELATED]
            DUMP[SQLite Dump<br/>direct page writer]
        end

        subgraph "Foundation"
            ARENA[CBMArena<br/>bump allocator]
            SLAB[Slab Alloc<br/>tree-sitter]
            MIMALLOC[mimalloc<br/>general purpose]
            HASH[Verstable<br/>hash tables]
        end
    end

    subgraph "External"
        REPO[Source Repository]
        CACHE[~/.cache/codebase-memory-mcp/]
        ARTIFACT[.codebase-memory/graph.db.zst]
    end

    CC --> JSONRPC
    VS --> JSONRPC
    JSONRPC --> DISPATCH
    DISPATCH --> TOOLS
    TOOLS --> SEARCH
    TOOLS --> CYPHER
    TOOLS --> BFS
    TOOLS --> ARCH
    SEARCH --> SQLITE
    SEARCH --> FTS5
    SEARCH --> VECTOR
    CYPHER --> SQLITE
    BFS --> SQLITE
    ARCH --> SQLITE
    DISCOVER --> REPO
    EXTRACT --> DISCOVER
    LSP --> EXTRACT
    SEMANTIC --> LSP
    DUMP --> SQLITE
    SQLITE --> CACHE
    DUMP --> ARTIFACT
    EXTRACT --> ARENA
    EXTRACT --> SLAB
    SQLITE --> MIMALLOC
    SQLITE --> HASH
```

## Layer Stack

```
┌─────────────────────────────────────────────────┐
│  MCP Handler       src/mcp/                     │  JSON-RPC dispatch
│                    22 tools, resolve, auto-index │
├─────────────────────────────────────────────────┤
│  CLI               src/cli/                     │  install/update/config
├─────────────────────────────────────────────────┤
│  Cypher Engine     src/cypher/                  │  MATCH...RETURN
├─────────────────────────────────────────────────┤
│  Pipeline          src/pipeline/                │  7-phase orchestrator
│                    pass_*.c (16 passes)          │  parallel work-stealing
├─────────────────────────────────────────────────┤
│  Graph Buffer      src/graph_buffer/            │  in-memory graph
│                    7 hash table indexes          │  string intern pool
├─────────────────────────────────────────────────┤
│  Store             src/store/                   │  SQLite CRUD
│                    ~6000 lines                   │  FTS5, vector, BFS
├─────────────────────────────────────────────────┤
│  Discovery         src/discover/                │  file walk, gitignore
│                    language detection            │  .cbmignore patterns
├─────────────────────────────────────────────────┤
│  Foundation        src/foundation/              │  arena, slab, mem
│                    platform, compat, str_util    │  hash tables, logging
└─────────────────────────────────────────────────┘
```

## Data Flow: Index Pipeline

```mermaid
sequenceDiagram
    participant MCP as MCP Handler
    participant PIPE as Pipeline
    participant DISC as Discovery
    participant EXTR as Parallel Extract
    participant LSP as LSP Resolve
    participant SEM as Semantic
    participant DUMP as SQLite Dump

    MCP->>PIPE: cbm_pipeline_run()
    PIPE->>DISC: cbm_discover_ex()
    DISC-->>PIPE: file list + excluded dirs
    PIPE->>PIPE: try_incremental_or_delete_db()
    alt Incremental path
        PIPE-->>MCP: return (no-op or delta)
    else Full reindex
        PIPE->>EXTR: parallel extract (work-stealing)
        EXTR-->>PIPE: nodes + calls + imports
        PIPE->>LSP: cross-file LSP resolve
        LSP-->>PIPE: refined edges + field defs
        PIPE->>SEM: similarity + semantic edges
        SEM-->>PIPE: SIMILAR_TO + SEMANTICALLY_RELATED
        PIPE->>DUMP: direct page writer
        DUMP-->>PIPE: .db file + optional .zst artifact
        PIPE-->>MCP: return (success + stats)
    end
```

## Data Flow: Query Path

```mermaid
sequenceDiagram
    participant CC as Claude Code
    participant MCP as MCP Handler
    participant STORE as Store (SQLite)
    participant CACHE as Cache Dir

    CC->>MCP: JSON-RPC tools/call
    MCP->>MCP: cbm_mcp_server_handle()
    MCP->>MCP: resolve_store(project)
    MCP->>STORE: cbm_store_open_path_query()
    STORE->>CACHE: open <project>.db
    MCP->>MCP: verify_project_indexed()
    MCP->>MCP: dispatch to tool handler
    MCP->>STORE: query/bfs/search/vector
    STORE-->>MCP: results
    MCP->>MCP: format JSON response
    MCP-->>CC: {jsonrpc, id, result}
```

## Dependency Graph

```
foundation/          zero internal deps
  └─→ store/         depends: foundation
       └─→ graph_buffer/  depends: foundation, store structs
       └─→ cypher/        depends: store structs
            └─→ pipeline/  depends: all above + discover + extraction
                 └─→ mcp/  depends: all above
                      └─→ main.c  wires everything
```

No circular dependencies. Each layer depends only on layers below it.

## Key Files

| File | Lines | Purpose |
|------|-------|---------|
| `src/mcp/mcp.c` | ~5300 | MCP server, 22 tools, search, incidents |
| `src/store/store.c` | ~6000 | SQLite CRUD, FTS5, vector search, BFS |
| `src/pipeline/pipeline.c` | ~1200 | Pipeline orchestrator |
| `src/pipeline/pass_parallel.c` | ~2600 | Parallel extract + resolve |
| `src/graph_buffer/graph_buffer.c` | ~1600 | In-memory graph + SQLite dump |
| `src/discover/discover.c` | ~650 | File discovery + filtering |
| `src/pipeline/pass_definitions.c` | ~600 | Symbol extraction |
| `src/pipeline/pass_calls.c` | ~700 | Call resolution |
| `src/pipeline/pass_semantic_edges.c` | ~1000 | Vector embedding generation |
| `src/cypher/cypher.c` | ~4400 | Cypher parser + executor |
| `src/cli/cli.c` | ~4000 | Install/update/config CLI |
| `internal/cbm/lsp/php_lsp.c` | ~4500 | PHP LSP resolver |
| `internal/cbm/lsp/java_lsp.c` | ~3000 | Java LSP resolver |
