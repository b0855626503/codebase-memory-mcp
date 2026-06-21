# Phase 3A — Embedding Architecture (Revised)

## Configuration Schema (Final)

```yaml
# .codebase-memory.json — embedding section

embedding:

  targets: [Function, Method, Class]

  graph:

    inbound:
      edge_types: [CALLS, ROUTES_TO]  # ROUTES_TO is Core — every framework has entry points
      depth: 1                         # prevent explosion
      limit: 10                        # max edge names per node

    outbound:
      edge_types: [CALLS]
      depth: 1
      limit: 10

    structural:
      edge_types: [INHERITS]
      depth: 1

  metrics: []                           # empty — no population stats in embeddings

  format:
    Method: "METHOD:{name} CLASS:{parent_class} SIG:{signature_first_line} CALLS:{outbound_1hop_names} CALLED_BY:{inbound_1hop_names}"
    Function: "FUNC:{name} SIG:{signature_first_line} CALLS:{outbound_1hop_names}"
    Class: "CLASS:{name} INHERITS:{parent_class_names} METHODS:{method_names_list}"

  plugins:
    laravel:
      route_properties: [method, path]     # appended when ROUTES_TO exists
```

## Signal Classification (Revised)

| Signal | Layer | Rationale |
|--------|-------|-----------|
| qualified_name | **Core** | Universal |
| parent_class | **Core** | OOP languages |
| signature | **Core** | Universal |
| CALLS (outbound) | **Core** | Universal |
| CALLED_BY (inbound) | **Core** | Universal |
| INHERITS | **Core** | OOP |
| **ROUTES_TO** | **Core** | Every framework has entry-point→handler |
| fan_in | **Excluded** | Population, not semantics |
| fan_out | **Excluded** | Coupling, not semantics |
| complexity | **Excluded** | Structural, not semantic |
| route path/method | **Plugin:Laravel** | Framework-specific naming |
| decorators | **Plugin:Framework** | Language-specific annotations |

## Architecture

```
Node (Method)
    │
    ▼
[Layer 1: Core Builder]        ← always runs, no config needed
    qualified_name
    parent_class
    signature (first line)
    │
    ▼
[Layer 2: Graph Traversal]     ← config-driven, depth/limit bounded
    inbound 1-hop: CALLS, ROUTES_TO → caller names (LIMIT 10)
    outbound 1-hop: CALLS → callee names (LIMIT 10)
    structural: INHERITS → parent class names
    │
    ▼
[Layer 3: Plugin Extension]    ← reads embedding.plugins config
    if ROUTES_TO edge exists → append route {method} {path}
    │
    ▼
[Layer 4: Format]              ← template from config
    apply format string
    produce text document
    │
    ▼
[Tokenize → RI Vector → Quantize → Store]
```

## Benchmark Design

Queries:

| # | Query | Expected Top-3 |
|---|-------|---------------|
| 1 | wallet deposit | WalletService.deposit, DepositController.store, WalletRepository |
| 2 | cancel ticket | LottoTicketsCancelReportController, LottoTicket.cancel, CancelPolicy |
| 3 | lotto payout settlement | LottoPayoutService, SettlementService, DrawSettlement |
| 4 | member balance | MemberRepository.getBalance, Member.balance, WalletController |
| 5 | commission calculate | CommissionService, CommissionCalculator, RewardCommission |
| 6 | reward points redeem | RewardPointController, PointRedemption, RewardService |
| 7 | bank withdraw | WithdrawController, BankOutService, WithdrawRepository |
| 8 | match fixture result | FixtureController, MatchResult, SoccerService |
| 9 | promotion coupon | PromotionController, CouponRepository, CouponService |
| 10 | line message send | ChatService, LineMessaging, LineController |

Metric: Recall@5 (correct hit in top 5)

## Storage Impact

| Component | Size |
|-----------|------|
| Core names | 0 (already in graph) |
| 1-hop edge names | ~250KB for 14K nodes |
| Plugin append | ~50KB |
| **Total corpus growth** | **~300KB** |
| Vector storage | 0 change (768B × 14K = 10MB) |
| Index time | +~10s for 1-hop graph traversal |
