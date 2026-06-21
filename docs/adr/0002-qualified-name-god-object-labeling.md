# ADR-0002: Use qualified_name for God Object Labeling

## Status

Accepted

## Date

2026-06-21

## Context

The `check_architecture_rules` and `smart_analyze` tools report "God Objects" — functions with fan_in exceeding configurable thresholds (default ≥100). These incidents are surfaced to users as actionable architecture violations.

Before this decision, the incident title used the node's bare `name`:

```
God Object: now (fan_in=4091)
God Object: create (fan_in=3220)
God Object: call (fan_in=1335)
```

Users could not determine whether `now` referred to `Carbon\Carbon::now` (a PHP helper), `SomeHelper::now` (a custom utility), or `admin.js:now` (a minified JS fragment). The `fan_in` value was correct (SQL already used `GROUP BY n.id` — per-node, not name-aggregated), but the label made the report unactionable.

Investigation of `src/store/store.c:3428` confirmed the SQL was correct:

```sql
SELECT n.name, n.qualified_name, COUNT(*) as fan_in
FROM nodes n JOIN edges e ON e.target_id = n.id AND e.type = 'CALLS'
...
GROUP BY n.id ORDER BY fan_in DESC LIMIT 10
```

`GROUP BY n.id` already produces per-unique-node fan_in. The problem was purely in the presentation layer — the `name` field was ambiguous.

## Decision

**Use `qualified_name` as the primary `function` field** in `check_architecture_rules` output and `smart_analyze` incident titles. Preserve `name` as a secondary field for backward compatibility.

Before:
```json
{"function": "now", "qualified_name": "Carbon\\Carbon::now", "fan_in": 4091}
```

After:
```json
{"function": "Carbon\\Carbon::now", "name": "now", "fan_in": 3971}
```

The `smart_analyze` incident title also changed from using `name` to `qualified_name`:
```
Before: God Object: now (fan_in=4091)
After:  God Object: AutoResultHardeningService.now (fan_in=3971)
```

This also fixed the incident dedup key — different classes' `create()` methods were incorrectly deduplicated because they shared the same bare name.

## Considered Options

### Option 1: qualified_name in labels only (chosen)

- **Pros**: Minimal change, preserves backward compat, SQL unchanged, only presentation layer
- **Cons**: Doesn't change fan_in calculation (but it was already correct)

### Option 2: Change fan_in aggregation from `GROUP BY n.id` to `GROUP BY n.qualified_name`

- **Pros**: Would deduplicate by full QN
- **Cons**: SQL already uses `GROUP BY n.id` which IS per-unique-node. No change needed.

### Option 3: Change labeling globally (all tools)

- **Pros**: Consistency
- **Cons**: Some reports intentionally aggregate by bare name for pattern discovery (top method names, common API patterns). Global change would break these use cases.

## Consequences

### Positive

- God Object incidents are immediately actionable — users can identify the specific class and method
- Dedup correctly handles cases where different classes share method names
- Architecture violations now read like real engineering reports, not opaque data dumps

### Negative

- incident titles are longer (full QN vs bare name)
- Backward compat: existing incident dedup relies on exact title match — titles changed

## Related Decisions

- ADR-0001: Exclude minified JS/CSS (made QN labeling more impactful by removing JS noise)
- ADR-0003: Symbol resolution in trace_path

## References

- Commit: `60c345c` — fix(observability): use qualified_name for God Object labeling
- `src/pipeline/pass_arch_rules.c:160` — `function` field now uses `qualified_name`
- `src/mcp/mcp.c:4314-4315` — smart_analyze incident title uses `qualified_name`
- `src/store/store.c:3428-3434` — arch_hotspots SQL (already `GROUP BY n.id`)
