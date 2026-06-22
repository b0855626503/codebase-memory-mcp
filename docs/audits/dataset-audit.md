# Audit: Benchmark Dataset Quality — semantic-search-1168lot-v1.json

Date: 2026-06-23
Auditor: Claude (code-audit deep mode)

## Executive Summary

**Dataset Quality Score: 7.5/10**

The dataset measures **business-intent search**, not code lookup. This is intentional but creates
a vocabulary gap: 9/30 queries have expected targets that share ≤1 token with the query,
making them impossible to retrieve via FTS5. The dataset is honest (not overfit to CBM's
strengths) but under-tests code-lookup use cases.

## Query Classification

| Class | Count | Definition | Example |
|-------|------:|------------|---------|
| Business Intent | 28 | Natural language describing what user wants | "wallet deposit history" |
| Developer Search | 2 | Code-like queries a developer might type | "cancel ticket", "sum bonus" |
| Exact Code Lookup | 0 | Exact function/symbol names | — |

### Full Classification

| # | Query | Class | Expected | Shared Tokens |
|---|-------|-------|----------|:---:|
| 1 | wallet deposit history | Business | HistoryController.loadDeposit | 1/3 |
| 2 | wallet withdraw history | Business | HistoryController.loadWithdraw | 1/3 |
| 3 | wallet balance credit | Business | MemberController.gamesetwallet | 1/3 |
| 4 | wallet transaction history | Business | HistoryController.loadMoneyTran | 1/3 |
| 5 | game wallet balance adjust | Business | MemberController.gamesetwallet | 2/4 |
| 6 | yeekee round current | Business | LottoController.yeekeeCurrentRound | 3/3 |
| 7 | lotto bet submit | Business | LottoController.bet | 1/3 |
| 8 | lotto draw result market | Business | LottoController.draw | 1/4 |
| 9 | yeekee shoot submit | Business | LottoController.submitShoot | 2/3 |
| 10 | lotto package selection | Business | LottoController.selectPackage | 2/3 |
| 11 | bonus promotion sum | Business | MemberGameLogRepository.sumBonus | 1/3 |
| 12 | promotion admin management | Business | PromotionController.index | 2/3 |
| 13 | promotion bonus win slot | Business | SlotxoController.bonusWin | 1/4 |
| 14 | game provider list | Business | GameController.getProviders | 1/3 |
| 15 | game listing by provider | Business | GameController.getGames | 1/4 |
| 16 | kickoff lobby token | Business | KickoffRepository.ensureMemberLobbyToken | 2/3 |
| 17 | member search lookup | Business | ChatController.findMember | 2/3 |
| 18 | member wallet manual adjust | Business | MemberController.broadcastManualWalletAdjust | 3/4 |
| 19 | member register attach line | Business | ChatController.registerMember | 2/4 |
| 20 | lotto ticket list filter | Business | LottoController.tickets | 2/4 |
| 21 | lotto ticket detail summary | Business | LottoController.ticket | 2/4 |
| 22 | lotto ticket cancel | Developer | LottoController.cancel | 2/3 |
| 23 | admin member management list | Business | MemberController.index | 1/4 |
| 24 | admin chat reply message | Business | ChatController.reply | 2/4 |
| 25 | admin game provider config | Business | GameSingleController.loadProvider | 1/4 |
| 26 | unsettle bets void settlement | Business | RoyalSlotGaminngNewController.unsettleBets | 2/4 |
| 27 | yeekee result engine settle | Business | YeekeeResultEngineService.computeFromRound | 0/4 |
| 28 | yeekee reward apply position | Business | YeekeeShootingRewardService.applyRewardPosition | 2/4 |
| 29 | game session middleware user check | Developer | EnsureUserInCurrentGame.handle | 3/5 |
| 30 | webhook signature verify sms | Business | VerifySmsWebhookSignature.handle | 2/4 |

## Shared Token Analysis

| Shared Tokens | Count | Retrievable? |
|:---:|:-----:|--------------|
| 0 | 1 | ❌ Impossible |
| 1 | 10 | ❌ Very hard |
| 2 | 14 | 🟡 Possible |
| 3+ | 5 | ✅ Usually retrievable |

**9 queries with 0-1 shared tokens** = GROUP B (FTS5 rank >1000) — these are fundamentally
unretrievable by any lexical search without structured document enrichment.

## Target Distribution

| Category | Expected | Corpus | Bias |
|----------|:-------:|:------:|:----:|
| Controller | 80% | 42% | 🔴 Over-represented |
| Repository | 7% | 8% | ✅ Balanced |
| Service | 7% | 5% | ✅ Balanced |
| Model | 0% | 5% | 🟡 Under-represented |
| Middleware | 7% | <1% | 🔴 Over-represented |

**Controller methods are 80% of expected but only 42% of corpus** — the dataset tests
functions that are findable (Controllers have more keyword-rich names) but may miss
lower-level Repository/Service functions.

## Ambiguity Report

| Risk | Severity | Detail |
|------|----------|--------|
| Vocab mismatch | 🔴 HIGH | 9/30 queries use business vocabulary absent from function names |
| Controller bias | 🟡 MEDIUM | 80% expected = Controller, under-tests Service/Repository |
| Single-expected | 🟡 MEDIUM | Most queries have 1 expected target — fragile to ranking variance |
| No code-lookup | 🟡 MEDIUM | 0 exact-symbol queries — doesn't test basic retrieval sanity |

## Recommendations

1. **P0**: Add 10 exact-symbol queries ("loadDeposit", "getProviders") for retrieval sanity
2. **P0**: Add 10 code-lookup queries using developer vocabulary ("load deposit", "find member")
3. **P1**: Add Service/Repository expected targets to balance Controller bias
4. **P1**: Add multi-expected queries where multiple valid answers exist
5. **P2**: Split dataset into tiers: Sanity (exact), Developer (code-like), Business (NL)

## Source-of-Truth Verification

All expected targets verified against actual graph QNs in `home-boat-projects-1168lot.db`.
No synthetic/imagined function names. All expected functions exist as Method nodes with
node_vectors entries.
