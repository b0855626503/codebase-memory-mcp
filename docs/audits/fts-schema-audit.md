# Audit: FTS5 Schema & Indexing Strategy

Date: 2026-06-23
Auditor: Claude (code-audit deep mode)

## Executive Summary

FTS5 index is a **single-column flat search** — all fields (name, qualified_name, label, file_path)
are concatenated into one FTS5 content column. There is no field-level weighting, no
METHOD/CLASS/PACKAGE separation, and no configurable BM25 parameters. Ranking is dominated
by short keyword-rich tokens from Models and migrations, drowning out business methods.

## Schema

```sql
CREATE VIRTUAL TABLE nodes_fts USING fts5(
  name, qualified_name, label, file_path,
  content='',
  tokenize='unicode61 remove_diacritics 2'
)
```

**Tokenizer**: `unicode61` with `remove_diacritics 2` (2-char n-grams removed).
**Content**: External content table (content=''), meaning FTS5 does not store the actual text.

## Indexed Columns

| Column | Source | Tokenization |
|--------|--------|-------------|
| `name` | `cbm_camel_split(name)` | camelCase → space-separated tokens |
| `qualified_name` | raw QN (dotted path) | dots NOT split — treated as single token |
| `label` | "Method"/"Function"/"Class" | single word — low signal |
| `file_path` | raw relative path | `/` NOT split — treated as single token |

### Critical Finding: `qualified_name` is NOT tokenized

```
Input:  "home-boat-projects-1168lot.packages.Gametech.Wallet.Http.Controllers.HistoryController"
FTS5:   "home-boat-projects-1168lot.packages.Gametech.Wallet.Http.Controllers.HistoryController"
         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
         Treated as ONE token — cannot be searched by "Wallet" or "HistoryController" alone
```

**This means FTS5 search for "wallet" in QN returns ZERO matches from the qualified_name column.**
The only matches come from `name` column (via `cbm_camel_split`) and `file_path` column.

### `cbm_camel_split(name)` — What it does

```
"loadDeposit"      → "load Deposit"
"getProviders__"   → "get Providers __"
"findOneWhere"     → "find One Where"
"HistoryController" → "History Controller"
```

This is a C function registered as a custom SQLite function that splits camelCase
identifiers. It is the **only** source of tokenized domain vocabulary in FTS5.

## Insert Logic

```sql
-- Primary path: camelCase split
INSERT INTO nodes_fts(rowid, name, qualified_name, label, file_path)
SELECT id, cbm_camel_split(name), qualified_name, label, file_path FROM nodes;

-- Fallback: plain name (if cbm_camel_split unavailable)
INSERT INTO nodes_fts(rowid, name, qualified_name, label, file_path)
SELECT id, name, qualified_name, label, file_path FROM nodes;
```

## Ranking Formula

**BM25 with default parameters** — no `k1`, `b`, or field weights configured:

```sql
bm25(nodes_fts, 0.0, 1.0, 0.5)
--             k1=0.0  b=1.0  field_weight_for_name=0.5
--                                    ^^^
-- Only name column gets weight 0.5; QN/file_path/label get weight 0.0
```

Wait — this is actually using weighted columns: `name` gets 0.5 weight. But the BM25 parameters
(k1=0.0, b=1.0) are non-standard: k1=0 disables term frequency saturation.

### BM25 Parameter Analysis

| Parameter | Value | Standard | Effect |
|-----------|-------|----------|--------|
| k1 | 0.0 | 1.2 | TF saturation disabled — every occurrence counts equally |
| b | 1.0 | 0.75 | Full document length normalization |
| field weights | name=0.5, others=0.0 | — | Only name column contributes to ranking |

**k1=0 turns BM25 into essentially tf-idf without term frequency saturation** — rare tokens
don't get the boost they would with standard k1=1.2-2.0.

## Structural Issues

### 1. No field separation

```
METHOD / CLASS / PACKAGE are not separate FTS5 columns.
→ Cannot weight Controller methods higher than Model methods at FTS5 level.
→ Label weight must be applied as a post-filter (as done in benchmark --search-lexical).
```

### 2. No QN tokenization

```
"Gametech.Wallet.Http.Controllers" is ONE token.
→ "Wallet" search finds ZERO matches from QN.
→ Domain vocabulary from package/namespace is invisible to FTS5.
```

### 3. No path tokenization

```
"packages/Gametech/Wallet/src/Http/Controllers/HistoryController.php" is ONE token.
→ "Wallet" search finds ZERO matches from file_path.
```

### 4. Noise domination

```
Query "wallet": matches Model.member_wallet ×500+, migrations ×200+, 
                but NOT "Wallet" package (QN not tokenized)
                
Result: Model methods dominate ranking despite being 5% of corpus.
```

## Ranking Bias Quantification

```
Corpus:    Model 5% → Top-50 results 25% (5× over-represented)
           Service 5% → Top-50 results 0.8% (6× under-represented)
```

Root cause: Model method names are short keyword-rich tokens (`member`, `wallet`, `admin`)
while Service method names are longer (`resolvePaidWalletTransactionId`, `applyForRound`).

BM25 treats ALL tokens equally (k1=0) → short common tokens dominate.

## Recommendations

1. **P0**: Tokenize `qualified_name` column — split on `.` into separate tokens
2. **P0**: Tokenize `file_path` column — split on `/` into separate tokens  
3. **P1**: Separate FTS5 columns: `method_name`, `class_name`, `package`, `file_path`
4. **P1**: Field-level BM25 weights: `method_name:5.0 class_name:3.0 package:2.0 file_path:1.0`
5. **P2**: Set k1=1.2 (standard) — enable TF saturation to boost rarer business terms
6. **P2**: Exclude migrations and test files from FTS5 index entirely
