# Phase 3A.3 — Graph-Aware Retrieval Architecture

## Task 1 — Pipeline Comparison

| Approach | Precision | Recall | Complexity | Scalability |
|----------|-----------|--------|------------|-------------|
| **A. Pure Semantic** | 🟡 5/10 | 🟡 medium | Low | O(n) vector scan |
| **B. Semantic + Re-rank** | 🟢 estimated 7.5-8/10 | 🟢 high | Medium | O(n log n) with top-K sort |
| **C. Semantic + Expansion** | 🟡 6/10 | 🟢 high | Medium | O(n + k·d) neighbor expansion |
| **D. Embedding (Phase 3A.1)** | 🔴 4/10 | 🔴 low | High | O(n·d) + collapse |

### Key Finding from Benchmark

**Approach D (graph in embedding) FAILED** — same-class methods collapsed.

Why: `MemberGameLogController` has 20 methods all sharing `CALLS:MemberRepository,GameLogRepository`. Graph similarity dominated semantic meaning. All 20 methods became near-identical in vector space.

**Lesson**: Embeddings measure "what you ARE" (semantic). Graph measures "how you're CONNECTED" (structural). Mixing them dilutes both signals.

## Task 2 — Signal Classification

| Signal | Semantic Value | Structural Value | Ranking Value | Layer |
|--------|---------------|------------------|---------------|-------|
| `qualified_name` | 🟢 HIGH | 🟡 low | 🟡 low | Embedding |
| `signature` | 🟢 HIGH | — | — | Embedding |
| `docstring` | 🟢 HIGH | — | — | Embedding |
| `CALLS` (outbound) | 🟡 medium | 🟢 HIGH | 🟢 HIGH | **Re-rank** |
| `CALLED_BY` (inbound) | 🟡 low | 🟢 HIGH | 🟢 HIGH | **Re-rank** |
| `ROUTES_TO` | 🟡 low | 🟢 HIGH | 🟢 HIGH | **Re-rank** |
| `INHERITS` | 🟡 low | 🟢 HIGH | 🟡 medium | Re-rank |
| `class_field_type` | 🟡 low | 🟢 HIGH | 🟡 medium | Re-rank |
| CLASS name | 🔴 noise | 🟡 low | 🔴 noise | **Exclude** |

### Decision Rule

- **Embedding layer**: qualified_name, signature, docstring — "what this code does"
- **Re-rank layer**: CALLS, CALLED_BY, ROUTES_TO — "how this code is connected"
- **Excluded**: CLASS (collapses same-class methods), fan_in/fan_out (popularity, not meaning)

## Task 3 — Re-ranking Design

```
User Query: "wallet deposit credit"
         │
         ▼
    [Vector Search]
    cosine against 14K node vectors
    Return Top 50 candidates
         │
         ▼
    [Graph Boot Score]
    For each candidate in Top 50:
      base_score = cosine similarity
      boost = 0
      
      // Boost 1: Query term appears in CALLED_BY chain
      if any CALLED_BY name ≈ query term:
        boost += 0.1
      
      // Boost 2: Query term appears in CALLS chain  
      if any CALLS name ≈ query term:
        boost += 0.1
      
      // Boost 3: Route path contains query term
      if ROUTES_TO edge path contains query term:
        boost += 0.05
      
      final_score = base_score × (1.0 + boost)
         │
         ▼
    [Re-sort by final_score]
    Return Top 10
```

### Why Top 50 then re-rank

- Vector search is O(n·d) — must scan all vectors
- Re-ranking 50 candidates is O(50·k) where k = avg edges per node (~5)
- Total: O(n·d + 250) ≈ O(n·d) — no meaningful overhead
- Storage: read edges for 50 nodes — negligible

## Task 4 — Graph Expansion Design

```
[Vector Search: Top K]
         │
         ▼
    [Neighborhood Expansion]
    For each result in Top K:
      add 1-hop CALLS targets
      add 1-hop CALLED_BY sources
      add 1-hop ROUTES_TO sources
         │
         ▼
    [Dedup + score inherit]
    Return expanded results
```

### Parameters

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Top K | 20 | Expansion amplifies × avg_degree |
| Depth | 1 | 1-hop captures immediate neighbors |
| Limit per node | 5 | Prevent hub explosion |
| Score inheritance | parent × 0.8 | Graph proximity lowers score |

### Expected Token Growth

```
Before: 1 query → Top 10 results
After:  1 query → Top 20 → expand → ~40 results (dedup)
```

Growth: 4× results. Acceptable for the 200-result semantic_results limit.

## Task 5 — Final Recommendation

### **Approach B: Semantic + Graph Re-ranking**

**Why not embedding (D)**: Benchmark proved graph-in-embedding reduces precision. Same-class collapse is inherent, not configurable.

**Why not expansion (C) first**: Expansion dilutes relevance — adds less-relevant neighbors. Re-rank preserves precision while improving ranking.

**Why not pure semantic (A)**: Graph has signal — `WalletService.deposit` → `WalletRepository.credit` is meaningful. Just not at the embedding layer.

### Implementation Path

```
Phase 3B.1: Graph Re-rank Prototype
  - Add re-rank function to search_graph handler
  - Read embedding_context (already built by pass_embedding_context)
  - Boost candidates where CALLS/CALLED_BY names match query terms
  - Benchmark before/after

Phase 3B.2: Graph Expansion (optional)
  - Only if re-rank doesn't reach 8/10
  - Add neighborhood expansion to search_graph
```

### Expected Improvement

| Metric | Current (Pure Semantic) | With Graph Re-rank |
|--------|------------------------|---------------------|
| Precision@5 | ~30% | ~60% |
| Recall@10 | ~50% | ~80% |
| MRR | ~0.3 | ~0.6 |
