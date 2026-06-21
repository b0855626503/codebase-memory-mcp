# ADR-0001: Exclude .min.js/.min.css in All Index Modes

## Status

Accepted

## Date

2026-06-21

## Context

The indexer has three modes: FULL (everything), MODERATE (filtered + similarity/semantic), FAST (filtered, no similarity/semantic). The `FAST_IGNORED_SUFFIXES` list included `.min.js` and `.min.css`, meaning minified JavaScript/CSS bundles were only excluded in FAST mode. In FULL and MODERATE modes, these files were indexed as regular source code.

A real-world benchmark against a 41k-node PHP monolith (`1168lot-soccer`) revealed:

- Minified JS bundles (e.g., `admin.js:a`, `admin.js:s`, `admin.js:f`) dominated trace_path output (80%+ of results were single-char function names)
- Semantic search returned minified JS functions instead of PHP domain logic
- Architecture clusters grouped by JS noise (`e,t,n,a`) instead of domain (`fixtureDetail, liveFixtureList`)
- Fan-in metrics were polluted by cross-language false edges from minified bundles

The fix involved two layers:

1. **Suffix-level**: Move `.min.js` and `.min.css` from `FAST_IGNORED_SUFFIXES` to `ALWAYS_IGNORED_SUFFIXES` (`src/discover/discover.c`)
2. **Path-level**: Discovered that many bundled assets use regular `.js`/`.css` extensions (e.g., `app.629ea432.js`, `admin.js`). These require `.cbmignore` path patterns (`**/publishable/assets/**`, `**/wm356/**`, `**/daterangepicker/**`)

## Decision

**Move `.min.js` and `.min.css` to `ALWAYS_IGNORED_SUFFIXES`** so they are excluded regardless of index mode.

For path-level exclusion of non-`.min`-suffixed bundles, use the existing `.cbmignore` mechanism (gitignore syntax at repo root).

## Considered Options

### Option 1: Move to ALWAYS_IGNORED_SUFFIXES (chosen)

- **Pros**: Simple, one-line change, covers the most common case, zero config needed
- **Cons**: Doesn't catch bundles without `.min.` prefix; requires separate path-level exclusions

### Option 2: Add configurable `exclude_patterns` to `.codebase-memory.json`

- **Pros**: Per-project configurability, no file needed in repo
- **Cons**: Larger scope: touches userconfig parser, pipeline, discover, and gitignore subsystems. No evidence it's needed beyond what `.cbmignore` already provides

### Option 3: Exclude entire `public/assets/` and `Resources/assets/` directories

- **Pros**: Catches all bundled assets
- **Cons**: Too aggressive — legitimate source JS (Vue components, Pinia stores, Axios services) lives in these directories. The benchmark proved 85% of noise was from specific files, not the entire directory

## Consequences

### Positive

- Edge count reduced by 50.3% (193,072 → 95,996) in the benchmark project
- Functions reduced by 95.5% (9,860 → ~445)
- Semantic search: top results shifted from JS `$`,`t`,`e` to PHP `MemberGameLogController.update`, `SeasonAdminController.edit`
- Architecture clusters: domain groups visible (`fixtureDetail`, `liveFixtureList`, `Log`, `RealTimeNewMessage`)
- trace_path: callers reduced from 90+ single-char names to 7 legitimate callers

### Negative

- `.cbmignore` patterns are case-sensitive (missed `Publishable` vs `publishable`)
- Users must add `.cbmignore` for project-specific patterns

### Mitigations

- Document `.cbmignore` as the recommended exclusion mechanism
- Use `[Pp]ublishable` or dual patterns when case sensitivity is a concern

## Related Decisions

- ADR-0002: Use qualified_name for God Object labeling
- ADR-0003: Symbol resolution in trace_path

## References

- Commit: `aa5c472` — fix: exclude .min.js/.min.css in all index modes
- `src/discover/discover.c:60-65` — ALWAYS_IGNORED_SUFFIXES
- Benchmark: Before/After report against `home-boat-projects-1168lot-soccer` (41k nodes)
