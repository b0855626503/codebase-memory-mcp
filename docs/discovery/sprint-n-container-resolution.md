# Sprint N — Container Resolution Discovery

Date: 2026-06-23

## Current State

| Metric | Value |
|--------|------:|
| `app()` calls in source | 1,241 |
| `app(Class::class)` | 74 |
| `app('Full\Class\Name')` | ~1,167 |
| `resolve()` calls | 39 |
| `make()` calls | 130 |
| **Total container calls** | **1,410** |
| **CBM resolves to actual class** | **0** |
| **CBM resolves to `app` helper** | **833** |
| **Coverage** | **0%** |

## Container Call Patterns

### Pattern A: String class name (94% of calls) — EASIEST

```php
$repo = app('Gametech\Payment\Repositories\BillRepository');
$bill = $repo->create([...]);
```

The string `'Gametech\Payment\Repositories\BillRepository'` is a LITERAL that tree-sitter
can extract as `call->first_string_arg`. This is the EASIEST pattern to resolve — no type
inference needed.

### Pattern B: Class::class constant (6%) — RESOLVABLE

```php
app(DashboardSummarySyncService::class)->dispatchForModelChange(...)
app(MemberPointLogRepository::class)
```

`ClassName::class` is a static class constant. Tree-sitter can extract the class name
from the `::class` reference.

### Pattern C: Variable (rare)

```php
app($someVariable)  // dynamic — not resolvable
```

## Implementation Approach

```
app('Gametech\Payment\Repositories\BillRepository')
  │
  ├─ 1. Extract first_string_arg from CBMCall
  ├─ 2. Match string to known Class QN in registry
  ├─ 3. The NEXT method call on the result:
  │     $repo = app('BillRepository')->create([...])
  │     → CALLS: BillRepository.create
  └─ 4. Emit CALLS edge: source → BillRepository.create
```

Chain resolution: `app()` returns the class instance, so the chained method call
`->create()` should resolve to the repository class, not the `app` helper.

## Estimated Impact

```
After fix:
  Resolvable container calls: ~1,200 (string + ::class patterns)
  New business CALLS edges:   ~600-800 (each container call → 1+ method calls)
  Container noise edges:      833 → 0 (app/resolve/make excluded)
```

## Edge Cases

- `app()` with no argument (returns Container) — skip
- `app()->make()` — indirect, skip
- `resolve()` is an alias for `app()` — same handling
- `make()` resolves from container — same handling
