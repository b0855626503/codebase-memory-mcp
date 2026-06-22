# ADR-0042: Search Evaluation — Retrieval Architecture Findings

Date: 2026-06-22
Status: Accepted

## Context

CBM's semantic search uses sparse random indexing (8 non-zero entries in 768-dim vectors)
with min-cosine scoring. After completing Graph Quality improvements (Sprint L: +1,359
business CALLS edges, 77% resolve rate), we ran systematic benchmarks to evaluate
retrieval quality against a 30-query human-curated dataset on the 1168lot codebase
(~40K nodes, PHP Laravel).

## Decision

We will adopt a **lexical-first retrieval architecture** (FTS5 BM25 → label weight → token overlap boost)
and deprecate semantic-only search as the primary retrieval path. Sparse random indexing
remains available for SIMILAR_TO edges but is not suitable for precise code search.

## Evidence

### Benchmark Trajectory (Recall@10 on 30 queries)

| Phase | Recall@10 | What Changed |
|-------|----------:|--------------|
| Baseline | 0.0333 | Random indexing only |
| Sprint L | 0.0333 | +2,501 business CALLS edges (zero impact on recall) |
| F.3 Vocabulary | 0.0667 | QN/NS tokens in embedding documents (+100%) |
| F.4 Token Decomp | 0.1000 | CamelCase splitting in documents (+50%) |
| 4A Lexical FT5 | 0.1000 | Switched to FTS5 BM25 |
| 4A.1 AND query | 0.1333 | AND instead of OR in FTS5 (+33%) |

### Key Findings

1. **Random Indexing fails for exact code search**: Querying with exact function names
   (e.g., "loadDeposit") returns rank=0/50. Single tokens ("load", "deposit") also fail.
   The 8-dimensional sparse vectors cannot distinguish between related functions.

2. **Graph signals (CALLS) do not improve retrieval**: Despite 2,501 new business CALLS
   edges, Recall@10 remained at 0.0333. Graph context helps structural analysis
   (trace_path, dead_code) but not semantic search.

3. **Vocabulary bridge is the single biggest lever**: Adding domain vocabulary from
   qualified names and namespace tokens doubled recall. Token decomposition
   (camelCase → tokens) added another 50%.

4. **FTS5 AND query improves precision 3×**: Switching from OR to AND in FTS5
   improved Precision@5 from 0.0200 to 0.0700 (+250%).

5. **Candidate generation is the remaining bottleneck**: FTS5 retrieves 67% of
   expected functions somewhere in the top 500, but only 30% in the top 50.
   The remaining 33% are not retrievable by FTS5 at all.

### Human Vocabulary ≠ Code Vocabulary

The fundamental gap: users search with business intent terms ("wallet deposit history"),
but code is named with implementation patterns (HistoryController::loadDeposit).
Documents now contain both vocabularies (TOKENS + QN + NS), but lexical ranking
still favors noise (Model.member_wallet) over signal (loadDeposit).

## Consequences

- **Semantic-only search is deprecated** as primary retrieval. FTS5 BM25 is the new default.
- **Phase 5: Structured Retrieval** is the next architectural boundary — explicit
  PACKAGE, CLASS, METHOD fields with BM25 field-level weighting.
- **Sparse random indexing is retained** for SIMILAR_TO edges only.
- **Benchmark dataset (`semantic-search-1168lot-v1.json`) is frozen** as regression gate.
- **Baseline artifacts** at `results/baseline-semantic.json` and `results/baseline-fts5-and.json`.
