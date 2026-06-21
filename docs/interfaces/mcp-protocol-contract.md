# MCP Protocol Contract

## Transport

- **Primary**: Newline-delimited JSON-RPC 2.0 on stdio
- **Secondary**: LSP-style Content-Length framing (`Content-Length: N\r\n\r\n<body>`)
- **Buffering mitigation** (`mcp.c:5168-5220`): Three-phase poll to handle `getline()` draining kernel data into libc's FILE* buffer

## JSON-RPC Methods

| Method | Handler | Description |
|--------|---------|-------------|
| `initialize` | `cbm_mcp_initialize_response` | Capability negotiation |
| `ping` | Inline | Returns `{}` |
| `tools/list` | `cbm_mcp_tools_list` | Tool manifest |
| `tools/call` | `cbm_mcp_handle_tool` | Tool dispatch |
| `notifications/cancelled` | Inline | Cancels active pipeline |

### Protocol Versions Supported
`2025-11-25`, `2025-06-18`, `2025-03-26`, `2024-11-05` — newest shared version selected.

### Error Codes
- `-32700`: Parse error (standard JSON-RPC)
- `-32601`: Method not found (standard JSON-RPC)
- Tool errors: `{isError: true}` in result content (MCP convention)

## Tool Catalog (22 tools)

| # | Tool | Required Params |
|---|------|----------------|
| 1 | `index_repository` | repo_path |
| 2 | `search_graph` | project |
| 3 | `query_graph` | query, project |
| 4 | `trace_path` | project (symbol or function_name) |
| 5 | `get_code_snippet` | qualified_name, project |
| 6 | `get_graph_schema` | project |
| 7 | `get_architecture` | project |
| 8 | `search_code` | pattern, project |
| 9 | `list_projects` | (none) |
| 10 | `delete_project` | project |
| 11 | `index_status` | project |
| 12 | `detect_changes` | project |
| 13 | `manage_adr` | project |
| 14 | `ingest_traces` | traces, project |
| 15 | `smart_analyze` | project |
| 16 | `analyze_architecture_reasoning` | project |
| 17 | `check_architecture_rules` | project |
| 18 | `create_incident` | project, title |
| 19 | `list_incidents` | project |
| 20 | `detect_schema_drift` | project |
| 21 | `save_baseline` | project |
| 22 | `detect_dead_code` | project |

### trace_path Symbol Resolution
`symbol` parameter (short name) resolves via:
1. Exact QN match
2. QN LIKE '%symbol%' (Function/Method, LIMIT 20)
3. Single match → auto-trace; multiple → candidate list

### search_graph Hybrid Mode
- `query` → BM25 (FTS5 with camelCase splitting)
- `name_pattern` → regex with LIKE pre-filter
- `semantic_query` → vector cosine search (Function/Method/Class only)
- Results in separate `results` (BM25/regex) and `semantic_results` (vector) fields

## Artifact Format

`.codebase-memory/graph.db.zst`:
- zstd-compressed SQLite database
- Best mode: VACUUM INTO temp, DROP INDEXES, VACUUM, zstd -9
- Fast mode: direct zstd -3
- Metadata: `artifact.json` with schema_version, commit SHA, node/edge counts
- Atomic write: temp → rename
- `.gitattributes`: `graph.db.zst merge=ours binary` for conflict-free merges

## Config Files

| File | Format | Purpose |
|------|--------|---------|
| `~/.config/codebase-memory-mcp/config.json` | JSON | `extra_extensions`, `auto_index` |
| `<repo>/.codebase-memory.json` | JSON | Per-project extension overrides (wins over global) |
| `<repo>/.cbmignore` | Gitignore syntax | Per-project file exclusion patterns |
| `~/.cache/codebase-memory-mcp/<project>.db` | SQLite | Index data |
| `~/.cache/codebase-memory-mcp/_config.db` | SQLite | UI config, process state |

## Failure Modes

| Failure | Behavior |
|---------|----------|
| Corrupt DB | Integrity check → auto-delete .db+WAL+SHM → return NULL store → "re-index required" |
| Disk full | SQLite error → pipeline returns non-zero → MCP returns `{status: "error"}` |
| OOM during indexing | `safe_realloc` frees old pointer, returns NULL → caller-dependent (crash risk) |
| Index lock held | Non-blocking: watcher skips. Blocking: 100ms spin-wait for MCP handler |
| Invalid regex | `cbm_regcomp` pre-validation → explicit error message |
| Platform exit | Parent watchdog thread (`main.c:110-129`) polls `getppid()` every 500ms |

## Web UI (Optional)

`src/ui/http_server.c` — background pthread on `127.0.0.1`:
- Own MCP server instance (read-only, separate SQLite connection)
- REST endpoints: `/api/adr`, `/api/index`, `/api/layout`, `/api/browse`, `/api/logs`, `/api/processes`
- CORS: only `localhost`/`127.0.0.1` origins
- Embedded frontend assets via `cbm_embedded_lookup()`

## Cross-Repo Intelligence

`pass_cross_repo.c`: Matches Routes/Channels across indexed projects:
- Phase A: HTTP route matching (url_path + method)
- Phase B: Async matching (broker + path)
- Phase C: Channel matching (EMITS ↔ LISTENS_ON)
- Bidirectional edge creation: both source and target DBs get CROSS_* edges
- Edge types: CROSS_HTTP_CALLS, CROSS_ASYNC_CALLS, CROSS_CHANNEL, CROSS_GRPC_CALLS, CROSS_GRAPHQL_CALLS, CROSS_TRPC_CALLS
