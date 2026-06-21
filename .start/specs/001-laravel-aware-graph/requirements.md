---
title: "Laravel-Aware Knowledge Graph"
status: draft
version: "1.0"
specId: "001-laravel-aware-graph"
---

# Product Requirements Document

## Validation Checklist

### CRITICAL GATES (Must Pass)

- [x] All required sections are complete
- [x] No [NEEDS CLARIFICATION] markers remain
- [x] Problem statement is specific and measurable
- [x] Every feature has testable acceptance criteria (Gherkin format)
- [x] No contradictions between sections

### QUALITY CHECKS (Should Pass)

- [x] Problem is validated by evidence (not assumptions)
- [x] Context → Problem → Solution flow makes sense
- [x] Every persona has at least one user journey
- [x] All MoSCoW categories addressed (Must/Should/Could/Won't)
- [ ] Every metric has corresponding tracking events — partially addressed via index_status counts
- [x] No technical implementation details included
- [x] A new team member could understand this PRD
- [x] **MECE: Personas** — each persona is distinct, all user types represented
- [x] **MECE: Journeys** — each journey is a unique path, all paths covered
- [x] **MECE: Features** — no overlapping user stories, no capability gaps
- [x] **MECE: Acceptance Criteria** — each criterion tests a unique condition, all paths covered

---

## Product Overview

### Vision

A Laravel-aware knowledge graph where routes, controllers, commands, events, and jobs are connected — enabling accurate dead code detection, impact analysis, and architecture auditing for real-world PHP monoliths.

### Problem Statement

The current codebase knowledge graph indexes PHP code generically — extracting function definitions, calls, and class hierarchies — but does not understand Laravel-specific entry points. This causes three measurable failures:

1. **Routes are orphans**: 26 Route nodes exist but zero `ROUTES_TO` edges connect them to their handler controllers. A `Route::post('/deposit', [DepositController::class, 'store'])` creates a Route node for `/deposit` and a Method node for `store`, but no edge between them. Evidence: `query_graph` on `home-boat-projects-1168lot-soccer` returns 26 Routes, 0 ROUTES_TO edges.

2. **Dead code detection is unusable**: 5,744 dead code candidates reported — nearly all false positives. Controller methods, Artisan command handlers, event listeners, and queued jobs appear "dead" because the graph doesn't know they're invoked by framework infrastructure rather than direct PHP calls. A `DepositController::store` method with zero CALLS edges is flagged as dead code, despite being the handler for `POST /api/deposit`.

3. **Impact analysis is wrong**: fan-in for controller methods is artificially low because route invocations aren't counted. When analyzing "what happens if I change `DepositController::store`?", the graph shows no inbound callers — missing the route, middleware chain, and form request validation that depend on it.

**Consequences of not solving**: Architecture audits remain unreliable (false god objects, false dead code), dependency analysis misses critical paths (route → controller → service → repository), and the graph cannot answer "which endpoints call this function?" — a fundamental question for Laravel development.

### Value Proposition

This feature makes the knowledge graph understand Laravel's implicit invocation patterns — the framework-level wiring that static analysis alone cannot see. Unlike generic PHP static analyzers that treat routes as configuration strings, the graph connects routes to their handlers with typed edges, enabling:

- **Accurate fan-in**: Controller methods reflect real invocation count from routes, middleware, and commands
- **Actionable dead code**: Only methods unreachable from ANY entry point (routes, commands, events, jobs, scheduled tasks) are flagged
- **Complete impact analysis**: "Change `WalletService::deposit`" shows impact through Route → Controller → Service → Repository chain
- **Entry point discovery**: All HTTP routes, console commands, and event listeners visible as graph entry points

## User Personas

### Primary Persona: AI Coding Agent (Claude Code)

- **Goals**: Understand the real call graph of a Laravel codebase to make safe changes. Trace "what calls this controller method?" and get accurate answers including route invocations. Identify truly dead code before suggesting deletion.
- **Pain Points**: trace_path shows zero inbound callers for controller methods that are clearly invoked via routes. detect_dead_code reports thousands of false positives — 5,744 candidates, almost all noise. Cannot answer "which API endpoints write to the wallets table?" without manually reading route files.

### Secondary Persona: Staff Engineer doing Architecture Review

- **Goals**: Audit a Laravel monolith's architecture. Find god objects with accurate fan-in counts. Map dependency boundaries between packages. Verify that critical paths (deposit, withdrawal) have proper error handling layers.
- **Pain Points**: Architecture violations report `AutoResultHardeningService.now` with fan_in=3,971 — but this includes only direct CALLS edges, missing route invocations that would push fan_in even higher. Boundary crossing analysis can't distinguish "tests calling production code" from "routes calling controllers".

### Secondary Persona: Onboarding Developer

- **Goals**: Understand how a specific API endpoint works end-to-end. Trace `POST /api/deposit` from the HTTP layer through controllers, services, repositories, and external API calls.
- **Pain Points**: No single tool shows the complete path. Must manually grep route files, then controller files, then service files. The graph has all the nodes but none of the wiring between them.

### MECE Check: Personas
- [x] Each persona has distinct goals and pain points (no overlap)
- [x] All user types who interact with this feature are represented (no gaps)

## User Journey Maps

### Primary User Journey: AI Agent traces endpoint impact

1. **Awareness**: Agent needs to change `WalletService::deposit` and must understand what depends on it
2. **Consideration**: Agent runs `trace_path(symbol="WalletService.deposit", direction="inbound")` — gets some callers but no route entries
3. **Adoption**: Agent runs `search_graph(query="deposit route")` to manually find the route, then cross-references with controller
4. **Usage (desired)**: Agent runs `trace_path(symbol="WalletService.deposit", direction="inbound")` and sees: `Route:/api/deposit → DepositController::store → WalletService::deposit` — complete path in one call
5. **Retention**: Agent trusts the graph for impact analysis because route invocations are visible, fan-in is accurate, and dead code detection is reliable

### Secondary User Journey: Architect audits codebase health

1. **Awareness**: Architect runs `smart_analyze` on a newly indexed project
2. **Consideration**: Reviews God Object list — sees `AutoResultHardeningService.now` at 3,971 fan_in. Can't tell if this includes route-based calls
3. **Usage (desired)**: Runs `check_architecture_rules`. Sees violations with fan_in that COUNTS route invocations. Controller methods with routes show real fan-in > 0. Dead code list excludes route-handled methods.
4. **Retention**: Trusts the audit because the graph understands Laravel's invocation model

### Secondary User Journey: Developer explores an API endpoint

1. **Awareness**: Developer is assigned a bug on `POST /api/deposit`
2. **Consideration**: Greps route files → finds `DepositController@store` → reads controller → finds `WalletService::deposit` → reads service
3. **Usage (desired)**: Opens graph UI or runs `trace_path(symbol="/api/deposit", mode="cross_service")`. Sees full path: Route → Controller → Service → Repository → External API. Clicks through to read source code at each hop.
4. **Retention**: Uses the graph as the primary exploration tool instead of grep

### MECE Check: Journeys
- [x] Each journey describes a distinct path (no two journeys cover the same actions)
- [x] All primary, secondary, and discovery paths are mapped (no gaps)
- [x] Every persona has at least one journey

## Feature Requirements

### Must Have Features

#### Feature 1: Route → Controller Resolution (Sprint G)

- **User Story:** As an AI coding agent, I want `trace_path` to show route invocations as callers of controller methods, so that I can see the complete inbound dependency chain for any controller.
- **Acceptance Criteria (Gherkin Format):**

  - [ ] Given a Laravel project with `Route::post('/deposit', [DepositController::class, 'store'])`, When the project is indexed, Then a ROUTES_TO edge exists from the Route node for `/deposit` to the Method node for `DepositController::store`
  - [ ] Given a Laravel project with `Route::get('/users', 'UserController@index')`, When the project is indexed, Then a ROUTES_TO edge exists from the Route node for `/users` to the Method node for `UserController::index`
  - [ ] Given a Laravel project with closure-based route `Route::get('/health', fn() => response('ok'))`, When the project is indexed, Then a ROUTES_TO edge exists from the Route node to the closure's Function node
  - [ ] Given a Route is connected to a Controller method via ROUTES_TO, When `trace_path` is called on the controller method with direction=inbound, Then the Route appears as a caller at hop 1
  - [ ] Given a Route is connected to a Controller method via ROUTES_TO, When `arch_hotspots` computes fan_in for that method, Then the ROUTES_TO edge is counted in fan_in
  - [ ] Given a controller method has ONLY route invocations (no direct CALLS), When `detect_dead_code` runs, Then the method is NOT flagged as dead code
  - [ ] Given a project with `Route::resource('photos', PhotoController::class)`, When the project is indexed, Then ROUTES_TO edges exist for each RESTful action (index, store, show, update, destroy) to the corresponding controller methods

#### Feature 2: Artisan Command Registration (Sprint H)

- **User Story:** As an AI coding agent, I want Artisan console commands to appear as graph entry points, so that command handlers are not incorrectly flagged as dead code.
- **Acceptance Criteria (Gherkin Format):**

  - [ ] Given a Laravel project with `Artisan::command('report:generate', [ReportController::class, 'generate'])`, When indexed, Then a HANDLES edge exists from the Command node for `report:generate` to the handler method
  - [ ] Given a command handler is connected via HANDLES edge, When detect_dead_code runs, Then the handler method is not flagged as dead code
  - [ ] Given a command handler with zero direct CALLS but a HANDLES edge, When trace_path runs inbound, Then the Command node appears as a caller

#### Feature 3: Event Listener Registration (Sprint H)

- **User Story:** As an architect, I want event listeners recognized as live code, so that dead code detection doesn't flag event-driven architecture as unused.
- **Acceptance Criteria (Gherkin Format):**

  - [ ] Given a Laravel project with `Event::listen(OrderShipped::class, SendShipNotification::class)`, When indexed, Then a LISTENS_TO edge exists from the Event node to the listener class/method
  - [ ] Given a listener is connected via LISTENS_TO, When detect_dead_code runs, Then the listener is not flagged as dead code

### Should Have Features

#### Feature 4: Middleware Chain Wiring (Sprint G)

- **User Story:** As an architect, I want middleware chains visible in trace_path, so that I can see the complete request processing pipeline.
- **Acceptance Criteria:**
  - [ ] Given a route with `->middleware('auth')`, When indexed, Then a MIDDLEWARE edge connects the Route to the auth middleware class
  - [ ] Given a middleware is connected to a Route, When trace_path runs on the middleware, Then the Route appears in the dependency chain

#### Feature 5: Form Request Validation Wiring (Sprint G)

- **User Story:** As a developer, I want form request validation classes linked to controller methods, so that I can trace which endpoints use which validation rules.
- **Acceptance Criteria:**
  - [ ] Given `DepositController::store(DepositRequest $request)`, When indexed, Then a VALIDATES edge connects the controller method to the FormRequest class

### Could Have Features

#### Feature 6: Policy/Gate Authorization Wiring (Sprint H)

- **User Story:** As a security reviewer, I want to see which policies protect which controller methods.
- **Acceptance Criteria:**
  - [ ] Given `$this->authorize('update', $post)` in a controller, When indexed, Then an AUTHORIZES edge connects to the Policy method

#### Feature 7: Scheduled Task Registration (Sprint H)

- **User Story:** As an operations engineer, I want scheduled tasks visible in the graph.
- **Acceptance Criteria:**
  - [ ] Given `$schedule->command('report:generate')->daily()`, When indexed, Then a SCHEDULES edge connects the Schedule node to the command

### Won't Have (This Phase)

- **Runtime trace injection** (Sprint I) — deferred to Phase 2. Static analysis only for this phase.
- **Semantic search v2 with context enrichment** (Sprint J) — deferred to Phase 3.
- **Hybrid ranking with graph signals** (Sprint K) — deferred to Phase 3.
- **Risk label calibration** (Sprint L) — deferred to Phase 4.
- **Incident intelligence with fix suggestions** (Sprint M) — deferred to Phase 4.
- **Cross-repo route resolution** — only intra-project route wiring for this phase.
- **Livewire/Inertia component wiring** — defer until evidence of demand in real-world projects.

### MECE Check: Features
- [x] No two user stories describe the same capability (no overlap across MoSCoW categories)
- [x] All capabilities needed to solve the problem for every persona are present (no gaps)
- [x] Every feature has testable acceptance criteria
- [x] "Won't Have" explicitly accounts for capabilities that could be confused with in-scope features

## Detailed Feature Specifications

### Feature: Route → Controller Resolution

**Description:** During PHP extraction, the tree-sitter parser encounters Laravel route definitions (`Route::get()`, `Route::post()`, `Route::match()`, `Route::any()`, `Route::resource()`, `Route::prefix()` groups). Each route definition that specifies a controller action (string syntax `'Controller@method'`, tuple syntax `[Controller::class, 'method']`, or invokable class) creates a ROUTES_TO edge from the Route node to the resolved controller method.

**User Flow:**
1. Developer writes `Route::post('/api/deposit', [DepositController::class, 'store'])` in `routes/api.php`
2. Project is indexed via `index_repository` in full mode
3. Extraction resolves the controller class reference to its fully qualified name
4. Route node for `POST /api/deposit` is created with method=POST, path=/api/deposit
5. ROUTES_TO edge is created: Route → `DepositController::store` Method node
6. AI agent calls `trace_path(symbol="DepositController.store", direction="inbound")`
7. Response includes Route as a caller, with url_path and method in edge properties

**Business Rules:**
- Rule 1: When a controller class reference is a string literal (`'DepositController'`), resolve against the file's `use` imports and namespace
- Rule 2: When a controller action is specified as `[ClassName::class, 'method']`, extract the class name from the `::class` constant and the method name from the string literal
- Rule 3: When a Route uses `->name('deposit.store')`, store the route name as a property on the Route node
- Rule 4: When `Route::resource()` is used, expand to individual RESTful routes (index, create, store, show, edit, update, destroy) targeting the corresponding controller methods
- Rule 5: When `Route::prefix()` or `Route::group()` wraps routes, prepend the prefix to the path
- Rule 6: When controller method has middleware annotations or `->middleware()` chains, create MIDDLEWARE edges in Should Have scope
- Rule 7: Closure-based routes create an anonymous Function node with a ROUTES_TO edge

**Edge Cases:**
- Scenario 1: Controller class referenced by bare string (`'DepositController'`) without full namespace → Expected: Resolve via file's `use` imports. If unresolved, log warning and skip
- Scenario 2: Controller method doesn't exist in the class → Expected: Log at warning level, skip edge creation. Do not crash extraction
- Scenario 3: Route uses `__invoke` (invokable controller) → Expected: Connect Route to `__invoke` method on the controller class
- Scenario 4: Route definition is inside a conditional block (`if (app()->environment('local'))`) → Expected: Still extract the route; it's valid in some environments
- Scenario 5: Multiple routes point to the same controller method → Expected: Create separate ROUTES_TO edges for each route. Fan-in correctly counts all edges
- Scenario 6: Route file uses variables or expressions for controller class (`Route::post($path, $handler)`) → Expected: Skip if controller cannot be statically resolved. Log at info level

## Success Metrics

### Key Performance Indicators

- **ROUTES_TO edges**: Target > 0 (currently 0). Benchmark: `home-boat-projects-1168lot-soccer` re-index should produce ROUTES_TO edges for all controller-based routes
- **Dead code false positive reduction**: Target < 1,000 candidates (from 5,744 currently). Every controller method with a ROUTES_TO or HANDLES edge should be excluded from dead code
- **Fan-in accuracy**: Controller methods with route handlers should show fan_in ≥ 1 (currently many show 0). `trace_path` inbound on a route-handled controller method must include the Route node
- **Route nodes**: Should remain at current levels (~14 real routes in benchmark project). Closure-based routes may add new Function nodes
- **Indexing performance**: Route resolution adds at most 5% to extraction time. Benchmark: compare extraction time before/after on 1168lot-soccer

### Tracking Requirements

| Event | Properties | Purpose |
|-------|------------|---------|
| ROUTES_TO edge created | route_method, route_path, controller_class, controller_method | Track coverage of route patterns |
| Route resolution failure | file_path, line, reason | Identify unsupported patterns |
| Dead code exclusion (route) | method_qn, route_path | Verify routes prevent false dead code |
| fan_in change (controller) | method_qn, fan_in_before, fan_in_after | Measure fan_in accuracy improvement |

---

## Constraints and Assumptions

### Constraints
- **Extraction-only**: No changes to the store schema, graph buffer, or dump pipeline. Route resolution is a new extraction pass
- **PHP-only**: Route resolution targets PHP/Laravel only. Other frameworks deferred
- **Static analysis only**: No runtime tracing for this phase. Route wiring from static code analysis
- **Performance budget**: Extraction time increase limited to 5% for benchmark project

### Assumptions
- Laravel route definitions follow standard patterns (`Route::verb()`, string/tuple controller references)
- Controller classes referenced in routes exist in the codebase and are indexed
- Projects use standard Laravel route files (`routes/web.php`, `routes/api.php`)
- `::class` syntax resolves to the imported class name in the file's `use` statements

## Risks and Mitigations

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| Complex route patterns (closures, dynamic) can't be resolved statically | Medium | High | Skip unresolvable routes with info log; accept that some routes won't have edges |
| Controller class resolution fails for deeply nested route files | Medium | Medium | Leverage existing LSP infrastructure for type/class resolution |
| ROUTES_TO edges inflate fan_in, creating MORE god object violations | Low | High | Document new baseline; users should re-baseline after reindex |
| Resource routes expand to methods that don't exist | Low | Medium | Verify method existence via registry lookup before creating edge |

## Open Questions

- [ ] Should Route nodes also receive a CALLS edge (in addition to ROUTES_TO) so existing fan_in queries work without changes? Or should arch_hotspots be updated to also count ROUTES_TO edges?
- [ ] For `Route::group()` with shared middleware/prefix, should the Route node have properties for the group, or should a separate Group node exist?
- [ ] Should `Route::redirect()` and `Route::view()` create Route nodes with special handler types?

## Supporting Research

### Benchmark Evidence (home-boat-projects-1168lot-soccer)

After Sprint A-F cleanups (minified JS exclusion, targeted `.cbmignore` patterns):
- **34,321 nodes, 95,996 edges** — graph is structurally clean
- **26 Route nodes** — 14 real API routes, 12 pnpm-lock.yaml noise (addressed by future `.cbmignore` pattern)
- **0 ROUTES_TO edges** — confirms the gap: route nodes exist but are completely disconnected from controllers
- **5,744 dead code candidates** — dominated by false positives that are actually route/command/event handlers

### Existing Capabilities to Leverage

- **LSP type resolution**: The existing PHP LSP resolver (`internal/cbm/lsp/php_lsp.c`) already resolves class names and method references for CALLS edges. Route controller resolution can reuse the same class name resolution infrastructure.
- **Route node infrastructure**: `pass_route_nodes.c` already creates Route nodes from HTTP route definitions. The node infrastructure exists — only the edge creation is missing.
- **Registry lookup**: `cbm_registry_t` can verify that resolved controller methods exist in the graph before creating edges.
