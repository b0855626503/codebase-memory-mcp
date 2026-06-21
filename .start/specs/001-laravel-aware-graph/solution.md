---
title: "Route Resolution — Solution Design"
status: draft
version: "1.0"
---

# Solution Design Document

## Validation Checklist

### CRITICAL GATES (Must Pass)

- [x] All required sections are complete
- [x] No [NEEDS CLARIFICATION] markers remain
- [x] Architecture pattern is clearly stated with rationale
- [x] **All architecture decisions confirmed by user**
- [x] Every interface has specification

### QUALITY CHECKS (Should Pass)

- [x] All context sources are listed with relevance ratings
- [x] Constraints → Strategy → Design → Implementation path is logical
- [x] Every component in diagram has directory mapping
- [x] Error handling covers all error types
- [x] Quality requirements are specific and measurable
- [x] Component names consistent across diagrams
- [x] A developer could implement from this design
- [x] Implementation examples use actual schema column names, verified against codebase
- [x] Complex queries include traced walkthroughs with example data
- [x] **MECE: Components** — each component has a single distinct responsibility
- [x] **MECE: Interfaces** — no duplicate interfaces, all paths documented
- [x] **MECE: Data Models** — each entity owns a distinct slice
- [x] **MECE: Acceptance Criteria** — each EARS criterion is unique, all PRD criteria mapped

---

## Architecture Decisions

### ADR-1: ROUTES_TO Edge Type

- **Choice**: New dedicated `ROUTES_TO` edge type (Route → Controller method)
- **Rationale**: Route invocation is framework infrastructure, not a direct function call. CALLS edges represent PHP function/method calls. Conflating route-to-controller with function-to-function loses semantic clarity. ROUTES_TO enables route-specific queries (e.g., "which routes invoke this controller?") without filtering out direct CALLS.
- **Tradeoffs**: Requires updating arch_hotspots and degree subqueries to count the new edge type. Existing queries that scan only CALLS won't see route invocations.
- **Confirmed**: ✅ User confirmed in brainstorm

### ADR-2: New Pipeline Pass

- **Choice**: New `pass_route_resolve.c` during extraction phase, AST-walk on route files
- **Rationale**: Follows existing pass-based architecture. AST-walk provides precise pattern matching for Route::verb() calls, nested groups, and ::class references. Regex-based alternatives are fragile with multi-line definitions and conditional routes. Extending the PHP LSP resolver would couple route logic to the LSP layer unnecessarily.
- **Tradeoffs**: New pass adds ~200-300 lines. AST-walk adds extraction time for route files (mitigated by limiting to routes/*.php paths).
- **Confirmed**: ✅ User confirmed in brainstorm

### ADR-3: Fan-in Counts All Invocation Types

- **Choice**: arch_hotspots SQL counts `e.type IN ('CALLS','ROUTES_TO','HANDLES')`
- **Rationale**: Controller fan_in must reflect ALL invocation sources — direct calls, route invocations, and cross-service handlers. Separate counting would show artificially low fan_in for controllers with only route invocations. Single SQL change at store.c:3428.
- **Tradeoffs**: Changes existing query semantics. Users must re-baseline after reindex. Existing CALLS-only numbers (before/after) won't be directly comparable.
- **Confirmed**: ✅ User confirmed in brainstorm

### ADR-4: AST-Walk Route Detection

- **Choice**: Tree-sitter AST walk on PHP route files during extraction
- **Rationale**: Tree-sitter provides node-type-accurate parsing of method_call, argument_list, and string nodes. Can distinguish `Route::get()` from arbitrary static method calls. Handles nested arrays, method chaining (->middleware(), ->name()), and ::class constants without fragile regex.
- **Tradeoffs**: Requires tree-sitter PHP grammar to be loaded. Route files must parse successfully. Variable-based or expression-based controller references cannot be resolved (logged at info, skipped).
- **Confirmed**: ✅ User confirmed in brainstorm

---

## Component Design

### Component Diagram

```
┌─────────────────────────────────────────────────────┐
│                 pass_route_resolve.c                │
│                                                     │
│  ┌───────────────┐   ┌──────────────────────────┐  │
│  │ Route Detector │   │ Controller Resolver       │  │
│  │                │   │                          │  │
│  │ - AST walk     │──▶│ - ::class → FQCN         │  │
│  │ - Route::verb()│   │ - string→ FQCN (imports) │  │
│  │   detection    │   │ - registry lookup        │  │
│  │ - resource()   │   │ - invokable detection    │  │
│  │   expansion    │   │                          │  │
│  └───────────────┘   └───────────┬──────────────┘  │
│                                   │                  │
│                          ┌────────▼──────────┐      │
│                          │ Edge Emitter        │      │
│                          │                    │      │
│                          │ - ROUTES_TO edges  │      │
│                          │ - Route node props │      │
│                          │ - HANDLES fallback │      │
│                          └────────────────────┘      │
│                                                     │
│  Dependencies:                                       │
│  - cbm_gbuf (upsert node, insert edge)               │
│  - cbm_registry (find method by class+name)          │
│  - tree-sitter PHP grammar (AST walk)                │
└─────────────────────────────────────────────────────┘
```

### Component: Route Detector

- **Directory**: `src/pipeline/pass_route_resolve.c`
- **Responsibility**: Walk PHP AST of route files, detect Laravel route definitions
- **Input**: `cbm_pipeline_ctx_t` (graph buffer, registry) + file path
- **Output**: List of `{route_method, route_path, controller_class, controller_method}` records
- **Dependencies**: tree-sitter PHP grammar, `cbm_gbuf`, `cbm_registry`

**Detection logic (traced walkthrough)**:

Given `routes/api.php`:
```php
Route::post('/deposit', [DepositController::class, 'store']);
```

1. tree-sitter parses file → AST root
2. Walk AST for `method_call` nodes where caller is `Route` (identified by `name` node = `Route`)
3. For each Route method_call:
   a. Extract method name from `function` child: `post`
   b. Extract first argument (path): string `/deposit`
   c. Extract second argument (action): `array_creation_expression` containing `[DepositController::class, 'store']`
      - Element 0: `name` node with `::class` suffix → `DepositController`
      - Element 1: `string` node → `store`
4. Resolve `DepositController` to FQCN using file's `use` imports
5. Output: `{method: "POST", path: "/deposit", controller_class: "App\\Http\\Controllers\\DepositController", controller_method: "store"}`

### Component: Controller Resolver

- **Directory**: `src/pipeline/pass_route_resolve.c`
- **Responsibility**: Resolve controller class references to graph Method nodes
- **Input**: Controller class FQCN + method name
- **Output**: Method node ID (or -1 if unresolved)
- **Dependencies**: `cbm_registry_find_by_name`, `cbm_gbuf_find_by_qn`

**Resolution strategies** (in order):
1. Build qualified_name: `project.packages.dir.ControllerClass.methodName`
2. Look up in registry via `cbm_registry_exists(registry, qn)`
3. If not found: try fuzzy lookup by bare method name + parent_class (when class QN is verifiable)
4. If still not found: log at warning, return -1 (skip edge creation)

**For `Route::resource()` expansion**:
- `Route::resource('photos', PhotoController::class)` expands to:
  - `GET /photos` → `index`
  - `GET /photos/create` → `create`
  - `POST /photos` → `store`
  - `GET /photos/{photo}` → `show`
  - `GET /photos/{photo}/edit` → `edit`
  - `PUT/PATCH /photos/{photo}` → `update`
  - `DELETE /photos/{photo}` → `destroy`
- Only creates edges for methods that exist on the controller class

### Component: Edge Emitter

- **Directory**: `src/pipeline/pass_route_resolve.c`
- **Responsibility**: Create/update Route nodes and ROUTES_TO edges in graph buffer
- **Input**: Resolved route records
- **Output**: Route nodes + ROUTES_TO edges in `cbm_gbuf_t`

**Emission flow**:
1. Build Route QN: `__route__<METHOD>__<path>` (same format as existing pass_route_nodes.c)
2. `cbm_gbuf_upsert_node("Route", path, route_qn, ...)` — dedup by QN
3. Build edge properties: `{"controller_class": "...", "method_name": "...", "route_file": "...", "line": N}`
4. `cbm_gbuf_insert_edge(route_node_id, method_node_id, "ROUTES_TO", props)`

**HANDLES fallback**: If a Method node already has a `route_path` property (from existing extraction), create a HANDLES edge in addition to ROUTES_TO for backward compatibility.

---

## Data Model Changes

### New Edge Type

| Edge Type | Source Label | Target Label | Properties |
|-----------|-------------|--------------|------------|
| `ROUTES_TO` | Route | Method, Function | `controller_class`, `method_name`, `route_file`, `line` |

### Modified Queries

**arch_hotspots** (`src/store/store.c:3428`):

Before:
```sql
SELECT n.name, n.qualified_name, COUNT(*) as fan_in
FROM nodes n JOIN edges e ON e.target_id = n.id AND e.type = 'CALLS'
...
```

After:
```sql
SELECT n.name, n.qualified_name, COUNT(*) as fan_in
FROM nodes n JOIN edges e ON e.target_id = n.id
  AND e.type IN ('CALLS', 'ROUTES_TO', 'HANDLES')
...
```

**degree subqueries** (`src/store/store.c:2514`):

Before:
```sql
(SELECT COUNT(*) FROM edges e
 WHERE e.target_id = n.id AND e.type IN ('CALLS', 'USAGE')) AS in_deg
```

After:
```sql
(SELECT COUNT(*) FROM edges e
 WHERE e.target_id = n.id AND e.type IN ('CALLS', 'ROUTES_TO', 'HANDLES', 'USAGE')) AS in_deg
```

**detect_dead_code** (`src/mcp/mcp.c` — SQL at ~line 4594):

The dead code query already filters by fan_in using degree subqueries. The updated subquery automatically excludes controller methods with ROUTES_TO edges.

---

## Interfaces

### Pass Interface

```c
// Registered in pipeline.c extraction phase
// Called per-file for PHP route files (identified by path matching "routes/" or "Http/routes/")
int cbm_pipeline_pass_route_resolve(cbm_pipeline_ctx_t *ctx);
```

**Input contract**:
- `ctx->gbuf` — graph buffer with existing nodes (Class, Method) from definition extraction
- `ctx->registry` — function registry seeded with all defined methods
- File path accessible via context

**Output contract**:
- Route nodes created/updated in gbuf (upsert by QN)
- ROUTES_TO edges inserted in gbuf
- Returns number of edges created, or -1 on error

### Route Detection Interface

```c
// Internal to pass_route_resolve.c
// Walks PHP AST and extracts route definitions
typedef struct {
    char *http_method;       // "GET", "POST", "PUT", "DELETE", "PATCH", "ANY"
    char *path;              // "/deposit", "/users/{id}"
    char *controller_class;  // FQCN or NULL if unresolved
    char *controller_method; // "store", "index", or NULL
    bool is_resource;        // true if Route::resource()
    char *route_name;        // route name from ->name(), or NULL
    int source_line;         // line number in source file
} route_def_t;

// Returns heap-allocated array, caller frees
int detect_laravel_routes(
    const char *source, int source_len,
    const char *file_path,
    route_def_t **out, int *count
);
```

### Controller Resolution Interface

```c
// Internal to pass_route_resolve.c  
// Resolves controller class + method to graph node ID
// Returns node_id or -1 if unresolved
int64_t resolve_controller_method(
    cbm_gbuf_t *gbuf,
    cbm_registry_t *registry,
    const char *controller_fqcn,
    const char *method_name
);
```

---

## Error Handling

| Error Scenario | Handling |
|----------------|----------|
| Route file fails to parse (tree-sitter error) | Log warning, skip file, continue with next file. Return partial results. |
| Controller class reference cannot be resolved | Log info with file:line, skip edge creation for that route. Do not fail the pass. |
| Controller method not found in registry | Log warning, skip edge. Controller may exist in a different namespace — no guessing. |
| Route::resource() on class with missing methods | Create edges only for methods that exist. Log info for missing methods. |
| Duplicate ROUTES_TO edge (same route + controller method) | gbuf edge dedup handles this — property merge on conflict. |
| Non-PHP file passed to route detector | Return 0 routes, no error. |

---

## Testing Strategy

### Unit Tests (`tests/test_route_resolve.c`)

| Test | Description |
|------|-------------|
| `test_detect_tuple_syntax` | `Route::post('/x', [C::class, 'm'])` → route_def with method=POST, controller=C, method=m |
| `test_detect_string_syntax` | `Route::get('/x', 'C@m')` → route_def with controller=C, method=m |
| `test_detect_invokable` | `Route::get('/x', C::class)` → invokable, method=__invoke |
| `test_detect_resource` | `Route::resource('x', C::class)` → 7 route_defs with correct methods and paths |
| `test_resolve_controller_found` | Given registry has Method node → resolve returns valid node_id |
| `test_resolve_controller_not_found` | Given registry lacks Method → resolve returns -1, logs warning |
| `test_skip_non_route_method_calls` | `SomeClass::post(...)` not `Route::post(...)` → no detection |
| `test_edge_inserted` | Route + Method exist → ROUTES_TO edge in gbuf |
| `test_fan_in_counts_routes_to` | Method has 2 ROUTES_TO edges → arch_hotspots returns fan_in=2 |
| `test_dead_code_excludes_routed_method` | Method with ROUTES_TO edge → not in dead_code results |

### Integration Tests

- Full re-index of test Laravel project with known route definitions
- Verify `query_graph`: `MATCH (r:Route)-[:ROUTES_TO]->(m:Method) RETURN r.name, m.name`
- Verify `trace_path` inbound on controller method includes Route nodes
- Verify `arch_hotspots` fan_in includes ROUTES_TO edges
- Verify extraction time increase < 5% compared to baseline

---

## Constraints and Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| AST walk misses edge case route syntax | Medium | Low | Skip unresolvable routes; log pattern for future support |
| Controller resolution fails for namespaced routes | Medium | Medium | Leverage existing `use` import resolution from pass_calls |
| Fan_in inflation creates false god objects | High | Low | Document; users re-baseline after reindex |
| ROUTES_TO edges not visible in existing tools | Medium | Low | Update arch_hotspots, degree queries, trace_path BFS |
