# Sprint O — Event/Queue Discovery

Date: 2026-06-23

## Current State

| Metric | Value |
|--------|------:|
| `event()` calls with `new XxxEvent()` | ~100+ |
| `dispatch()` calls | 165 |
| `Job::dispatch()` | 54 |
| `Bus::dispatch()` | 3 |
| Event classes defined | ~50+ (grep incomplete) |
| Listener classes | ~20+ |
| Job classes | ~109 (DB count) |
| **CBM EMITS edges** | **26** |
| **CBM LISTENS_ON edges** | **4** |
| **CBM DISPATCHES edges** | **0** |

## Event Patterns

### Pattern A: event(new XxxEvent(...))

```php
event(new MemberBalanceUpdated($member, $amount));
event(new RealtimeMemberActivityUpdated($data));
event(new LineOAChatConversationUpdated($conv));
```

Tree-sitter can extract the `new XxxEvent(...)` argument. The event class name is explicit.

### Pattern B: Job::dispatch()

```php
SettlementJob::dispatch($roundId);
NotifyListeners::dispatch($event);
```

Static dispatch on Job class. The class is explicit via `::class` reference or static call.

### Pattern C: Bus::dispatch()

```php
Bus::dispatch(new SettlementJob($data));
```

### Pattern D: EventServiceProvider::$listen

```php
protected $listen = [
    DepositConfirmed::class => [
        NotifyAdmin::class,
        UpdateBalance::class,
    ],
];
```

This is a STATIC event-listener mapping. Can be parsed without running the code.

## Implementation Approach

### Phase 1: Direct event() calls

```
event(new MemberBalanceUpdated(...))
  │
  ├─ 1. Extract 'new' expression from call->args[0]
  ├─ 2. Get class name: MemberBalanceUpdated
  ├─ 3. Find Event class in registry
  └─ 4. Create EMITS edge: caller → MemberBalanceUpdated
```

### Phase 2: Job::dispatch() static calls

```
SettlementJob::dispatch()
  │
  ├─ 1. callee_name = "SettlementJob.dispatch"
  ├─ 2. Extract class: SettlementJob
  └─ 3. Create DISPATCHES edge: caller → SettlementJob
```

### Phase 3: EventServiceProvider mapping

```
$listen = [
  DepositConfirmed::class => [NotifyAdmin::class],
]
  │
  ├─ 1. Parse EventServiceProvider.php
  ├─ 2. Extract $listen array
  └─ 3. Create LISTENS_ON edges: Event → Listener
```

## Estimated Impact

```
After fix:
  EMITS edges:      26 → ~150+ (all event() calls resolved)
  DISPATCHES edges:  0 → ~80  (Job::dispatch + Bus::dispatch)
  LISTENS_ON edges:  4 → ~30  (EventServiceProvider mapping)
```

## Related Graph Opportunities

- Queue worker → Job class HANDLES edges
- Job::handle() → downstream service CALLS edges
- Event → Listener → service chain traversal
