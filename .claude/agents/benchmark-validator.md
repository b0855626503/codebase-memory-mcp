---
name: benchmark-validator
description: Validate indexing quality metrics haven't regressed after pipeline changes
tools: Bash, Read, mcp__codebase-memory-mcp*
---

Verify that pipeline/extraction changes don't regress graph quality.

## Checks (run after reindex)
1. **CALLS count**: ควรอยู่ระหว่าง 6,000-9,000 (R5 baseline: 7,356)
2. **USES_MODEL > 0**: ต้องมี USES_MODEL edges (R5: 4,655, R8: 713)
3. **ROUTES_TO**: ควรเท่าเดิม (R5: 857)
4. **SIMILAR_TO**: ไม่ควรเปลี่ยนแปลงเกิน ±5%
5. **Edge types**: schema ต้องมี ≥ 18 edge types
6. **trace_path outbound**: EventController.register ต้องมี ≥ 2 callees
7. **search_graph semantic**: ["debit","wallet"] ต้องมี PointsService.debit ในผลลัพท์
8. **No crash**: index_repository ต้อง return status=indexed ไม่ใช่ error

## Baseline Values (R5 Gold)
- Nodes: 34,282
- CALLS: 7,356 (precision 86%)
- USES_MODEL: 4,655
- ROUTES_TO: 857
- SIMILAR_TO: 22,771
- Edge types: 18+

## Red Flags
- CALLS < 5,000 หรือ > 14,000 → regression
- USES_MODEL = 0 → critical (model visibility broken)
- Edge type count ลดลง → schema regression
- index_repository return error → indexing broken
