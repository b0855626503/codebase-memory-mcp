# Data Model

## SQLite Schema

All tables created at `src/store/store.c:215-298` (`init_schema`).

### nodes
| Column | Type | Notes |
|--------|------|-------|
| id | INTEGER PK AUTOINCREMENT | |
| project | TEXT NOT NULL FK→projects(name) CASCADE | |
| label | TEXT NOT NULL | Function, Method, Class, File, Folder, Route, Channel, Variable, Module, Package, etc. |
| name | TEXT NOT NULL | Bare identifier |
| qualified_name | TEXT NOT NULL UNIQUE(project, qn) | Fully qualified: `project.dir.file.symbol` |
| file_path | TEXT DEFAULT '' | Relative path |
| start_line | INTEGER DEFAULT 0 | |
| end_line | INTEGER DEFAULT 0 | |
| properties | TEXT DEFAULT '{}' | JSON blob: complexity, decorators, docstring, params, return_type, is_test, is_entry_point |

### edges
| Column | Type | Notes |
|--------|------|-------|
| id | INTEGER PK AUTOINCREMENT | |
| project | TEXT NOT NULL FK→projects CASCADE | |
| source_id | INTEGER NOT NULL FK→nodes CASCADE | |
| target_id | INTEGER NOT NULL FK→nodes CASCADE | |
| type | TEXT NOT NULL | CALLS, IMPORTS, DEFINES, INHERITS, HTTP_CALLS, etc. |
| properties | TEXT DEFAULT '{}' | JSON blob |
| url_path_gen | TEXT GENERATED ALWAYS | `json_extract(properties,'$.url_path')` — virtual |

**Dedup**: `UNIQUE(source_id, target_id, type)` — properties merged via `json_patch` on conflict.

### node_vectors
`sqlite_writer.c:2165-2167`:
- `node_id INTEGER PRIMARY KEY FK→nodes`
- `project TEXT NOT NULL`
- `vector BLOB NOT NULL` — int8 quantized, 768 bytes

### token_vectors
`sqlite_writer.c:2168-2171`:
- `id INTEGER PRIMARY KEY`
- `project TEXT NOT NULL`
- `token TEXT NOT NULL`
- `vector BLOB NOT NULL` — enriched RI vector, 768 bytes
- `idf INTEGER NOT NULL` — IDF × 1000 fixed-point

### incidents
`store.c:259-271`:
- `id INTEGER PK`, `project TEXT`, `title TEXT NOT NULL`, `description TEXT`
- `affected_functions TEXT` — JSON array of qualified_names
- `severity TEXT DEFAULT 'medium'` — critical, high, medium, low, info

### FTS5
`store.c:278-296`:
```sql
CREATE VIRTUAL TABLE nodes_fts USING fts5(
    name, qualified_name, label, file_path,
    content='', tokenize='unicode61 remove_diacritics 2'
)
```
Contentless — text is camelCase-split via `cbm_camel_split()` before insertion.

## Index Design

| Index | Columns | Serves |
|-------|---------|--------|
| `idx_nodes_label` | (project, label) | Label-filtered queries |
| `idx_nodes_name` | (project, name) | `cbm_store_find_nodes_by_name` |
| `idx_nodes_file` | (project, file_path) | File-filtered queries |
| `autoindex_nodes_1` | (project, qualified_name) | Exact QN lookup (UNIQUE) |
| `idx_edges_source` | (source_id, type) | Outbound degree, BFS source join |
| `idx_edges_target` | (target_id, type) | Inbound degree, BFS target join |
| `idx_edges_type` | (project, type) | Type-filtered edge scans |
| `idx_edges_target_type` | (project, target_id, type) | Fan-in computation (arch_hotspots) |
| `idx_edges_source_type` | (project, source_id, type) | Fan-out computation, BFS |
| `idx_edges_url_path` | (project, url_path_gen) | URL path lookup |
| `idx_incidents_project` | (project, id) | Incident listing |

## Key Edge Types

| Type | Semantics | Source |
|------|-----------|--------|
| CALLS | Function/method call | `pass_calls.c` |
| IMPORTS | Import/include/require | `pass_definitions.c` |
| DEFINES | Container defines symbol | `pass_definitions.c` |
| INHERITS | Class inheritance | `pass_semantic.c` |
| IMPLEMENTS | Interface implementation | `pass_semantic.c` |
| DEFINES_METHOD | Class defines method | `pass_semantic.c` |
| TESTS | Test→code under test | `pass_tests.c` |
| HTTP_CALLS | HTTP endpoint call | `pass_route_nodes.c` |
| HANDLES | Route→handler | `pass_route_nodes.c` |
| SIMILAR_TO | Code clone detection | `pass_similarity.c` |
| SEMANTICALLY_RELATED | Vector embedding similarity | `pass_semantic_edges.c` |
| CONFIGURES | Config→service wiring | `pass_configlink.c` |
| CROSS_HTTP_CALLS | Cross-repo HTTP | `pass_cross_repo.c` |

## Vector Storage

- **Dimension**: 768 (`VS_VEC_DIM` at `store.c:5681`)
- **Format**: int8 quantized, float × 127 clamped to [-127, 127]
- **Index**: None — brute-force cosine scan
- **Token vectors**: Pre-computed enriched RI vectors with IDF weights
- **Fallback**: Sparse random indexing (8 positions, ±1, XXH3 hashing) for unknown tokens

## Query Patterns

### arch_hotspots (`store.c:3476`)
```sql
SELECT n.name, n.qualified_name, COUNT(*) as fan_in
FROM nodes n JOIN edges e ON e.target_id = n.id AND e.type = 'CALLS'
WHERE n.project=? AND n.label IN ('Function','Method')
  AND json_extract(n.properties, '$.is_test') IS NULL
  AND n.file_path NOT LIKE '%test%'
GROUP BY n.id ORDER BY fan_in DESC LIMIT 10
```

### vector_search (`store.c:5855`)
```sql
SELECT n.id, n.name, n.qualified_name, n.file_path, n.label,
       cbm_cosine_i8(v.vector, ?1) as score, v.vector
FROM node_vectors v INNER JOIN nodes n ON n.id = v.node_id
WHERE v.project = ?2 AND n.label IN ('Function','Method','Class')
ORDER BY score DESC LIMIT ?3
```

### BFS traversal (`store.c:2745`)
SQLite recursive CTE with depth limit, using `idx_edges_source`/`idx_edges_target`. Two-phase: nodes via CTE, edges via separate `id IN (...)` query.

## Schema Evolution

- No `PRAGMA user_version` migration system
- `CREATE TABLE IF NOT EXISTS` — additive only
- No ALTER TABLE logic for new columns
- Integrity check validates project count + root_path format only
