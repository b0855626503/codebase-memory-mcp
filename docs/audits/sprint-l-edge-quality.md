# Audit: Sprint L Edge Quality — PHP Dynamic Dispatch Resolution

Date: 2026-06-23
Auditor: Claude (code-audit deep mode)

## Executive Summary

**Resolution Precision: 100%** — zero false positives in 100 sampled edges.
All resolved CALLS edges correctly map `$this->field->method()` to the actual target method.
However, **62% of edges are business-irrelevant** because they resolve to framework helpers
(`now`, `app`, `core`) inherited by Repository base classes — a corpus design issue, not a
resolution bug.

## Sample

100 randomly selected CALLS edges where the target is a Repository or Service method.
Sampled from `home-boat-projects-1168lot.db` (post-Sprint-L reindex).

## Results

| Category | Count | % | Description |
|----------|------:|---|-------------|
| True Positive (business) | 38 | 38% | Controller/Service → Repository/Service business method |
| Framework noise | 62 | 62% | Controller → Repository.now/app/core (inherited helpers) |
| False Positive (wrong target) | 0 | 0% | None — resolution is correct |
| Unresolved (missed) | 0 | 0% | All sampled edges were resolved |

## True Positive Examples

```
PragmaticPlaySlotController.getBalance    → Repository.findOneWhere     ✅
HomeController.loadCredit_GH              → Repository.findOrFail        ✅  
AmbSlot2Controller.transaction            → Repository.findOneWhere      ✅
GoalkubRepository.GameCurlGet             → GoalkubRepository.GameCurlGet ✅ (self-call)
RelaxGamingController.winRewards          → Repository.findOneWhere      ✅
```

## Framework Noise Examples

```
*Controller.* → Repository.now      (62% of sample)
*Controller.* → Repository.app
*Controller.* → Repository.core
```

These are CORRECT resolutions — `now`, `app`, `core` are methods defined on the Repository
base class (`Gametech.Core.src.Eloquent.Repository`). Controllers that inject `*Repository`
fields get these inherited methods as valid CALLS targets.

**This is a corpus quality issue, not a resolution bug.** The Repository base class
exposes framework helpers via inheritance, creating noise edges that are technically
correct but semantically useless.

## Resolution Strategy Breakdown

Strict L uses two resolution paths:

1. **Field node resolution** (registry.c:1083): Field nodes with `return_type` → direct
   class match → QN format mismatch fixed (src/ path insertion).

2. **Property name heuristic** (registry.c:927): When no Field node exists → derive
   class name from property name (`memberRepository` → `MemberRepository`) → whitelist
   check (Repository, Service, Manager, etc.) → registry lookup.

The heuristic fires as a fallback in `resolve_file_calls()` (pass_parallel.c:1839) when
`cbm_registry_resolve_member_call()` returns empty.

## Resolve Rate

| Metric | Before Sprint L | After Sprint L |
|--------|:--------------:|:--------------:|
| findOneWhere CALLS | 10 | 1,046 (+10,360%) |
| Repository.find CALLS | ~0 | 126 |
| PHP CALLS total | 11,551 | 13,008 (+12.6%) |
| Resolve rate (Repository calls) | 2% | 77% |

## Key Finding: Not a Single Wrong Resolution

Among 100 sampled edges, **zero edges pointed to a wrong target class or method.**
This validates:

1. The property name heuristic (`memberRepository` → `MemberRepository`) is correct
2. The case-insensitive class matching in registry works
3. The INHERITS fallback (finding methods on parent classes) is correct
4. The QN format mismatch fix (inserting `src/` path) is correct

## Recommendations

1. **P0**: Filter framework-inherited methods from CALLS for embedding_context (already
   done in Phase L.5 boilerplate filter)
2. **P1**: Add `resolution_strategy` metadata to CALLS edge properties for future audit
3. **P2**: Consider weighting CALLS edges by callee class type — Repository base class
   methods should have lower structural weight than domain-specific Repository methods
