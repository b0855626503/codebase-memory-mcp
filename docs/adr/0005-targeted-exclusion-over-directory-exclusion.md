# ADR-0005: Targeted Exclusion Over Directory Exclusion

## Status

Accepted

## Date

2026-06-21

## Context

After excluding `.min.js`/`.min.css` suffixes (ADR-0001) and `publishable/assets/` directories, trace_path still showed ~90 callers for `CircuitBreaker.call` — dominated by single-char function names from `packages/*/src/Resources/assets/`. The initial hypothesis was to exclude the entire `Resources/assets/` directory.

A quantitative audit (Graph Quality Audit v2) revealed:

| Source | Functions | % of 689 total | Type |
|--------|-----------|-----------------|------|
| `app.629ea432.js` (webpack chunk) | 145 | 21% | 🔴 Compiled |
| `daterangepicker/require.js` | 30 | 4.3% | 🔴 Vendored |
| `Winwheel.js` × 3 copies | 33 | 4.8% | 🔴 Duplicated |
| `toasty.js` | 13 | 1.9% | 🟡 Source |
| `app.js`, `web.js`, `dropdown.js` | 12 | 1.7% | 🟡 Source |

**85% of noise came from 3 specific file categories**: webpack chunks with content hashes, duplicated vendor libraries, and vendored AMD loaders. **15% were legitimate source files** (Vue components, utility libraries, app entry points).

## Decision

**Use targeted `.cbmignore` patterns** instead of blanket directory exclusion:

```gitignore
# Webpack build output directory
**/wm356/**
# Duplicated vendored files
**/Resources/assets/js/Winwheel.js
**/Resources/assets/js/winwheel/**
# Vendored AMD loader
**/daterangepicker/**
```

This removes 215 noise functions while preserving 30 legitimate functions in `toasty.js`, `app.js`, `web.js`, `dropdown.js`, and `debounce.js`.

### Principles

1. **Measure before excluding**: Graph Quality Audit before every new exclusion rule
2. **Target specific noise patterns, not entire directories**: webpack chunks (`[hash].js`), duplicated vendored libs (`Winwheel.js` × 3), vendored loaders
3. **Preserve source code**: `app.js`, `web.js`, `toasty.js`, `dropdown.js` contain legitimate Vue components and utility functions
4. **Case-sensitivity awareness**: Gitignore patterns are case-sensitive — need both `publishable` and `Publishable` patterns

## Consequences

### Positive

- Functions reduced from 689 → ~445 (35% of remaining Functions were noise)
- trace_path callers dropped from 90+ → 7 (all legitimate source files)
- Remaining functions have meaningful names: `toggleDropdown`, `autoDropupDropdown`, `debounce`
- No single-char function names in graph

### Negative

- Targeted patterns require knowledge of specific noise files (discovered through audit)
- New webpack chunks with different hash patterns might slip through
- Case-sensitivity in gitignore patterns caused initial miss of `Publishable` (capital P)

### Mitigations

- Graph Quality Audit as a recurring process before adding new exclusion rules
- Document case-sensitivity gotcha in `.cbmignore` usage
- Consider adding a `[Pp]ublishable` convention for case-insensitive matching

## Related Decisions

- ADR-0001: `.min.js`/`.min.css` suffix exclusion
- ADR-0004: Semantic domain scoping (same Measure→Verify→Patch methodology)

## References

- `.cbmignore` at `home-boat-projects-1168lot-soccer`
- Graph Quality Audit v2 — top files by Function count
- `src/discover/gitignore.c` — glob pattern matching engine
