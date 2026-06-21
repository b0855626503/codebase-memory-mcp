# Route → Controller Resolution — Design

**Date**: 2026-06-21
**Status**: Approved
**Spec**: 001-laravel-aware-graph

## Design Decisions

| # | Decision | Rationale |
|---|----------|-----------|
| 1 | New `ROUTES_TO` edge type | Semantically distinct from CALLS. Route invoking a controller is framework infrastructure, not a direct function call. |
| 2 | New pipeline pass `pass_route_resolve.c` | Follows existing pass pattern. Runs during extraction phase. AST-walk, not regex. |
| 3 | arch_hotspots counts CALLS + ROUTES_TO + HANDLES | Controller fan_in must reflect all invocation sources. Single SQL change at store.c:3428. |
| 4 | AST-walk detection during extraction | Tree-sitter provides precise pattern matching for Route::verb() calls, nested groups, and ::class references. |

## Architecture

```
routes/api.php (AST walk during extraction)
    │
    ▼
pass_route_resolve.c  ← NEW PASS
    │  Detects: Route::get|post|put|delete|patch|match|any|resource
    │  Resolves: controller class → Method node
    │
    ├── Route node (existing)  ──ROUTES_TO──→  Controller::method
    │
    ▼
arch_hotspots (updated SQL)
    │  COUNT(*) WHERE e.type IN ('CALLS','ROUTES_TO','HANDLES')
    │
    ▼
trace_path / detect_dead_code / smart_analyze
    (consume ROUTES_TO edges naturally)
```

## Edge: ROUTES_TO

| Field | Value |
|-------|-------|
| source | Route node (label=Route) |
| target | Method node (label=Method) |
| type | `ROUTES_TO` |
| properties | `{controller_class, method_name, route_file, line}` |

## Controller Reference Resolution

| Pattern | Example | Resolution |
|---------|---------|------------|
| Tuple with ::class | `[DepositController::class, 'store']` | Resolve ::class → FQCN → find Method |
| String syntax | `'DepositController@store'` | Parse class@method → resolve via imports |
| Invokable | `DepositController::class` | Find `__invoke` method |
| Resource | `Route::resource('photos', PhotoController::class)` | Expand to 7 RESTful methods |

## Changes

| File | Change |
|------|--------|
| `src/pipeline/pass_route_resolve.c` | **NEW** — AST-walk route extraction |
| `src/store/store.c:3428` | Update arch_hotspots: `IN ('CALLS','ROUTES_TO','HANDLES')` |
| `src/store/store.c:2514` | Update degree subqueries |
| `src/pipeline/pipeline.c` | Register new pass |

## Parking Lot

- Middleware chain wiring → Should Have
- Form request validation wiring → Should Have
- Policy/Gate authorization → Could Have
- Runtime trace injection → Phase 2
