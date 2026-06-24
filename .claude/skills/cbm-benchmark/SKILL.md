---
name: cbm-benchmark
description: Reindex target repo and measure graph quality metrics (CALLS, USES_MODEL, precision) against baseline
disable-model-invocation: true
---

# CBM Benchmark

วัดผล regression test หลังแก้ pipeline/extraction — delete DB → reindex → query metrics → report

## Usage
```
/cbm-benchmark <repo-path> [baseline-round]
```
ตัวอย่าง: `/cbm-benchmark /home/boat/projects/1168lot-soccer R8`

## Flow
1. Backup DB ปัจจุบัน (ถ้ามี)
2. Delete project DB → force fresh reindex
3. Build + deploy binary
4. Reindex ด้วย `cli index_repository` mode=full
5. Query metrics: nodes, edges, CALLS count, USES_MODEL count, edge types
6. เทียบกับ baseline (R5=gold: CALLS 7,356, USES_MODEL 4,655, precision 86%)
7. Report delta

## Metrics Tracked
- Nodes / Edges total
- CALLS count + strategy breakdown
- USES_MODEL count
- ROUTES_TO count
- SIMILAR_TO count
- Edge type count (schema completeness)
- trace_path inbound/outbound for key functions
- search_graph semantic relevance

## Baseline (R5 Gold)
| Metric | Value |
|--------|-------|
| Nodes | 34,282 |
| Edges | 93,913 |
| CALLS | 7,356 |
| USES_MODEL | 4,655 |
| ROUTES_TO | 857 |
| Precision | 86% |
| Grade | A- (90/100) |
