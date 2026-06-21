# Pipeline State Machine

## 7-Phase Full Index Pipeline

Entry: `cbm_pipeline_run()` at `src/pipeline/pipeline.c:1090`.

| Phase | What | Key Files |
|-------|------|-----------|
| 0 | Userconfig load + macro gate | `pipeline.c:1103-1109` |
| 1 | File discovery (`cbm_discover_ex`) | `discover.c:525` |
| 1.5 | Incremental-or-delete decision gate | `pipeline.c:784-845` |
| 2 | Graph buffer + registry + path aliases | `pipeline.c:1148-1153` |
| 2b | Structure pass (Project/Branch/Folder/File nodes) | `pipeline.c:1070-1073` |
| 3 | Extraction (parallel or sequential) | `pipeline.c:1078-1083` |
| 4 | Post-extraction (tests, git history, similarity, semantic) | `pipeline.c:987-1061` |
| 5 | Dump to SQLite + optional artifact export | `pipeline.c:860-935` |
| 6 | Incident + ADR restore | `pipeline.c:1019-1058` |
| 7 | Active learning self-check | `pipeline.c:1184-1219` |

## Incremental Decision Gate

`try_incremental_or_delete_db()` at `pipeline.c:784-845`:

```
DB exists?
 ├─ NO → return -1 (full reindex)
 └─ YES → integrity check
      ├─ FAIL → delete DB, return -1
      └─ OK → load file_hashes
           ├─ hashes > 0 AND files ≤ hashes + hashes/2 → INCREMENTAL
           └─ else → save incidents+ADR, delete DB, return -1 (full)
```

**Threshold**: New discovery must not exceed ~50% more files than previously indexed. Mode changes, major refactors, or large additions trigger full reindex.

**State preservation**: Incidents and ADR are captured BEFORE old DB deletion, restored after new dump completes (lines 1019-1058).

**Mode-skipped hash preservation** (`pipeline_incremental.c:335-360`): Prevents the orphaned-node bug where fast-mode reindex after full-mode index silently drops nodes under fast-excluded directories.

## Error/Cancel Paths

- **Cancel**: Atomic `p->cancelled` flag checked after every phase via `check_cancel()` (line 254). Propagates `CBM_NOT_FOUND` up.
- **Discovery failure**: Logs error, goto cleanup.
- **Phase failures**: Non-`ignore_err` passes cause early exit.
- **Cleanup** (lines 1221-1234): Always frees pkgmap, files, graph buffer, registry, path aliases, userconfig.

## Incident Management

### Schema
`store.c:259-270`: `incidents(id, project, title, description, affected_functions JSON, root_cause, resolution, severity, timestamps)`

### smart_analyze Flow
`mcp.c:4374-4449`:
1. Load existing incidents (dedup by exact title match)
2. Get architecture (hotspots, boundaries)
3. For each hotspot with fan_in ≥ 100: create "God Object" incident
4. For each boundary with call_count ≥ 50: create "Architecture Drift" incident
5. Generate ADR markdown from package list

### Severity Rules
| Trigger | Severity |
|---------|----------|
| fan_in ≥ 200 | critical |
| fan_in ≥ 100 | high |
| boundary call_count ≥ 50 | high |
| schema drift > 30% | critical |
| schema drift > 10% | high |

## Artifact Persistence

- **Export**: `artifact.c:332-411` — VACUUM INTO temp, drop indexes, zstd compress (level 9 best, level 3 fast), atomic write
- **Import**: `artifact.c:415-520` — decompress, integrity check, rename to cache
- **Bootstrap**: `mcp.c:2686-2694` — import artifact before indexing if no local DB exists
- **Metadata**: `artifact.json` with schema_version, commit SHA, node/edge counts, compression stats
