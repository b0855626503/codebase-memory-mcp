# Semantic Search Benchmark — 1168lot v1

30 human-curated queries across 9 business domains. Frozen 2026-06-22.

**Status**: FROZEN — do not modify queries, expected, or acceptable targets.

## Usage

```bash
cbm benchmark benchmarks/semantic-search-1168lot-v1.json \
  --project home-boat-projects-1168lot \
  --output results/current.json
```

## Baselines

| Profile | Recall@10 | MRR | Date |
|---------|----------:|-----|------|
| semantic (random indexing) | 0.1000 | 0.0242 | 2026-06-22 |
| fts5-and (lexical) | 0.1333 | 0.0881 | 2026-06-22 |

## Domain Coverage

| Domain | Queries | Expected Hits |
|--------|--------|---------------|
| Wallet | 5 | HistoryController, MemberController |
| Lotto | 5 | LottoController |
| Promotion | 3 | MemberGameLogRepository, PromotionController, SlotxoController |
| Provider | 3 | GameController, KickoffRepository |
| Member | 3 | ChatController, MemberController |
| Ticket | 3 | LottoController |
| Admin | 3 | MemberController, ChatController, GameSingleController |
| Settlement | 3 | RoyalSlotGaminngNewController, YeekeeResultEngineService, YeekeeShootingRewardService |
| Middleware | 2 | EnsureUserInCurrentGame, VerifySmsWebhookSignature |

## Expectations

Each query has:
- `expected`: primary target(s) that MUST be in results. Contribute to Recall@K.
- `acceptable`: related functions that are nice-to-have. Contribute to Precision@K.
- `weight`: importance multiplier (1.0 = default).

Queries use natural business language ("wallet deposit history"), not code identifiers
("loadDeposit"). This intentionally tests the vocabulary gap between human intent and
code implementation names.

## Known Limitations

1. 10/30 queries have expected targets NOT retrievable by FTS5 within top 500.
   These are queries where the expected function name shares ≤1 token with the
   query terms (e.g., "wallet balance credit" → `gamesetwallet`).

2. Expected targets are heavily Controller-weighted (80%) which matches corpus
   composition but under-represents Service/Repository layer.

3. Dataset is specific to 1168lot (PHP/Laravel). Results may not generalize to
   other codebases or languages.
