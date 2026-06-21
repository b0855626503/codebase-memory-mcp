# Security Audit

## Executive Summary

This is a well-hardened C11 MCP server with layered defenses. The codebase demonstrates strong security awareness: SQLite authorizer blocking ATTACH/DETACH, 100% parameterized SQL, project-name validation for path traversal, and 8-layer CI security suite. No exploitable vulnerability was found.

## SQL Injection — None Found

**100% parameterized**: All data-bearing SQL uses `sqlite3_prepare_v2` + `sqlite3_bind_*`. The `exec_sql()` function at `store.c:148` is used ONLY for hardcoded DDL/PRAGMA (zero user input).

**FTS5 BM25**: `bm25_build_match()` at `mcp.c:1308` strips every non-`[a-zA-Z0-9_]` character. Result bound as `?1` to `MATCH ?1`.

**SQLite authorizer** (`store.c:558-571`): Blocks `SQLITE_ATTACH`/`SQLITE_DETACH` at bytecode level — defense-in-depth.

**Cypher engine**: Never generates SQL text. Parses Cypher → evaluates graph by calling parameterized store APIs.

## Path Traversal — None Found

`cbm_validate_project_name()` at `str_util.c:273` blocks `..`, `/`, `\`, leading `.`, and non-alphanumeric characters (except `-`, `_`, `.`). Called before constructing `<cache_dir>/<project>.db`.

Symlinks silently skipped during discovery (`discover.c:371`: `S_ISLNK` check).

## Input Validation

- **JSON**: `yyjson` with internal bounds checking. Malformed input → NULL, caught at `mcp.c:128-130`.
- **Content-Length framing**: 10MB max body size (`mcp.c:5286`).
- **String extraction**: `heap_strdup()` with `malloc(strlen+1)`. No format string vulnerabilities.
- **Regex patterns**: Pre-validated via `cbm_regcomp` before passing to grep (`mcp.c:3866`).
- **Shell metacharacters**: `validate_search_path_arg` blocks `'`, `"`, `;`, `|`, `$`, backtick, `<`, `>`, `\n`, `\r`, `\` (non-Windows).

## Memory Safety

### Defensive Patterns
- `safe_realloc()` (`platform.h:22`): Frees old pointer on failure, returns NULL.
- `safe_free()`: NULLs pointer after free.
- `safe_str_free()`: Same for `const char*`.

### Recent Fixes
- Double-free in trace_path symbol resolver (`d1e2040`): `cbm_store_free_nodes` called after ownership transfer without NULLing source.
- Simplified resolver to eliminate ownership transfer complexity (`0aef7f5`).

### Rotating Static Buffers
`cypher.c:2077-2080`: 8-slot rotating `_Thread_local` buffers in `node_prop()`/`edge_prop()`. Callers copy values out before rotation. Previously a bug at `cypher.c:3299-3302` (noted in comment).

## Trust Boundaries

```
MCP Client → stdin → JSON-RPC parser → Tool dispatch → Store (SQLite) → File I/O
```

- **stdin boundary**: yyjson parsing, string extraction, no format string usage
- **SQL boundary**: 100% parameterized, SQLite authorizer, no ATTACH
- **Filesystem boundary**: project name validation, symlink skipping, `.cbmignore`/`.gitignore` pattern validation

## Observations Worth Monitoring

### Glob Pattern Backtracking
`gitignore.c:72-80, 59-69`: `glob_match_star()` and `glob_match_doublestar_any()` could exhibit O(2^n) with crafted patterns. A malicious `.gitignore` with `*a*a*a*a*a*a*a*a*a*b` in a repo being indexed could cause DoS.

### Lightweight DB Integrity Check
`store.c:680-730`: Only checks `projects` table row count and `root_path` format. Does not run `PRAGMA integrity_check`. Corrupt B-tree pages or index damage would not be detected.

### Error Information Exposure
- DB integrity failure logs project name + full DB path to stderr (not MCP response) — appropriate
- Cache directory path exposed in `build_project_list_error()` when directory can't be read
- Cypher parser errors expose internal token type numbers (low severity)
- Unknown tool names echoed in error responses (low severity)

## Threat Model (STRIDE)

| Category | Status |
|----------|--------|
| Spoofing | N/A — local MCP process, no authentication layer |
| Tampering | Mitigated — SQLite corruption detection, auto-delete on integrity failure |
| Repudiation | N/A |
| Info Disclosure | Mitigated — path traversal blocked, error messages sanitized |
| Denial of Service | Partially mitigated — 10MB frame limit, 100k Cypher ceiling, no query timeouts |
| Elevation of Privilege | Mitigated — no SQL injection, SQLite authorizer, bounded allocations |
