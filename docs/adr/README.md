# Architecture Decision Records

This directory contains Architecture Decision Records (ADRs) for codebase-memory-mcp.

## Index

| ADR | Title | Status | Date |
|-----|-------|--------|------|
| [0001](0001-exclude-minified-js-css-in-all-modes.md) | Exclude .min.js/.min.css in all index modes | Accepted | 2026-06-21 |
| [0002](0002-qualified-name-god-object-labeling.md) | Use qualified_name for God Object labeling | Accepted | 2026-06-21 |
| [0003](0003-symbol-resolution-in-trace-path.md) | Symbol resolution in trace_path | Accepted | 2026-06-21 |
| [0004](0004-semantic-domain-scoping-for-hybrid-search.md) | Semantic domain scoping for hybrid search results | Accepted | 2026-06-21 |
| [0005](0005-targeted-exclusion-over-directory-exclusion.md) | Targeted exclusion over directory exclusion | Accepted | 2026-06-21 |

## Decision Timeline

```
ADR-0001 ──→ ADR-0002 ──→ ADR-0003 ──→ ADR-0004 ──→ ADR-0005
(Data        (Observ-     (Usability)   (Search      (Precision
 Quality)     ability)                   Quality)     in Noise
                                                      Removal)
```

## Themes

All five ADRs share a common methodology: **Measure → Verify → Patch**.

- **ADR-0001 + ADR-0005**: Data quality — removing noise at the indexing layer, not patching downstream tools
- **ADR-0002**: Observability — making reports actionable without changing underlying calculations
- **ADR-0003**: Usability — reducing friction in the primary user workflow
- **ADR-0004**: Search quality — fixing output mixing, not embedding quality

## Creating a New ADR

1. Copy an existing ADR as a template
2. Use the next sequential number (`NNNN`)
3. Fill in: Status, Context, Decision, Considered Options, Consequences
4. Link to related ADRs
5. Update this index

## Status Values

- **Proposed**: Under discussion
- **Accepted**: Decision made, implementing
- **Deprecated**: No longer relevant
- **Superseded**: Replaced by another ADR
- **Rejected**: Considered but not adopted
