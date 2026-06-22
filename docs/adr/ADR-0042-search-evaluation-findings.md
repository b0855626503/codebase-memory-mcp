# ADR-0042: Search Evaluation — Retrieval Architecture Findings

Date: 2026-06-22
Updated: 2026-06-23 (Phase 4A results + k1 breakthrough)
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
| **4A.2 k1=1.2** | **0.3333** | Standard BM25 TF saturation (+150%) |
| **4A.3 QN+Path tok** | **0.3667** | QN/path tokenization, structured fields (+10%) |

**Total: 11× improvement from baseline without changing the embedding model.**

### Key Findings

1. **k1=0.0 was a critical bug**: The default BM25 configuration disabled term frequency
   saturation (k1=0.0, b=1.0). Changing to standard parameters (k1=1.2, b=0.75)
   produced a 3.3× improvement in Recall@10 (0.10 → 0.33). This was the single
   highest-impact change in the entire evaluation.

2. **QN/path tokenization is essential**: qualified_name (dotted paths) and file_path
   (slash paths) were stored as single FTS5 tokens. "Wallet" returned 0 FTS5 matches
   despite being in thousands of QNs. After splitting on `.` and `/`, "Wallet" = 2,816
   matches, "HistoryController" = 21.

3. **Graph signals (CALLS) do not improve retrieval**: Despite 2,501 new business CALLS
   edges (77% resolve rate, zero false positives), Recall@10 remained at 0.0333.
   Graph context helps structural analysis (trace_path, dead_code) but not semantic
   search — the retrieval layer was the bottleneck, not graph quality.

4. **Random Indexing was NOT the primary bottleneck**: After k1 fix + QN tokenization,
   Recall@10 reached 0.367 without changing the embedding model. The bottleneck was
   in the lexical/indexing layer (BM25 config, tokenization, document structure).

5. **Vocabulary bridge shows diminishing returns after k1 fix**: Adding QN/NS/TOKENS
   to documents was critical when ranking was broken, but after k1=1.2 the contribution
   of additional vocabulary enrichment is smaller than proper BM25 configuration.

### The Real Bottleneck: Lexical Layer, Not Embedding

The original hypothesis ("random indexing is the problem") was disproven by achieving
11× improvement purely through FTS5 configuration and document structure. The remaining
19/30 queries that fail are evenly split between:
- Retrievable with better ranking (GROUP A: 11 queries, rank 11-500)
- Not retrievable by FTS5 (GROUP B: 8 queries, rank >500 — need structured documents)

## Consequences

- **Semantic-only search is deprecated** as primary retrieval. FTS5 BM25 is the new default.
- **BM25 parameters fixed**: k1=1.2, b=0.75 (was k1=0.0, b=1.0 — effectively disabled TF saturation).
- **QN/path tokenization enabled**: dots and slashes split before FTS5 insertion.
- **Structured FTS5 columns added**: class_name + package_name for field-level weighting.
- **Phase 5: Structured Retrieval** is the next step — explicit METHOD^5 CLASS^3 PACKAGE^2 weights.
- **Sparse random indexing is retained** for SIMILAR_TO edges only.
- **Benchmark dataset (`semantic-search-1168lot-v1.json`) is frozen** as regression gate.
- **Baseline updated**: Recall@10 = 0.3667 (was 0.0333, 11× improvement).

## Updated Baselines

```
results/baseline-semantic.json        Recall@10 = 0.10  (semantic only)
results/baseline-fts5-and.json        Recall@10 = 0.13  (FTS5 AND, before k1 fix)
results/baseline-structured-bm25.json Recall@10 = 0.37  (current, after all fixes)
```
