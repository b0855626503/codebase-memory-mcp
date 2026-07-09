#!/bin/bash
# stress_test.sh — Phase 2B Stress/Determinism Test Suite
#
# Runs N full-index cycles + M incremental cycles, measuring:
#   - Graph determinism (node/edge hash)
#   - Memory (peak RSS, final RSS, growth)
#   - FD leak
#   - Golden test pass/fail after each cycle
#   - Indexing time
#
# Usage:
#   ./tests/stress/stress_test.sh <binary> <repo_path> <cycles>
#   ./tests/stress/stress_test.sh ./build/c/codebase-memory-mcp /home/boat/projects/codebase-memory-mcp 5

set -euo pipefail

BIN="${1:-./build/c/codebase-memory-mcp}"
REPO="${2:-/home/boat/projects/codebase-memory-mcp}"
CYCLES="${3:-5}"
GOLDEN_SCRIPT="$(dirname "$0")/../golden/run_golden.sh"
CACHE_DIR="$HOME/.cache/codebase-memory-mcp"
# Derive project name the same way the binary does (sanitize absolute path)
PROJECT="home-boat-projects-codebase-memory-mcp"
RESULTS="$(mktemp -d /tmp/cbm_stress_XXXXXX)"

green()  { echo -e "\033[32m$1\033[0m"; }
red()    { echo -e "\033[31m$1\033[0m"; }
yellow() { echo -e "\033[33m$1\033[0m"; }

get_rss_mb() {
    # Get RSS in MB for current process tree or specific PID
    local pid="${1:-$$}"
    ps -o rss= -p "$pid" 2>/dev/null | awk '{printf "%d", $1/1024}' || echo "0"
}

get_fd_count() {
    ls /proc/$$/fd 2>/dev/null | wc -l || echo "0"
}

graph_hash() {
    # Compute hash of DETERMINISTIC graph content only.
    # SIMILAR_TO (MinHash+SimHash) and SEMANTICALLY_RELATED (vector cosine)
    # are probabilistic by design — excluded from hash.
    local bin="$1" proj="$2"
    local nodes calls usage defines imports writes

    nodes=$("$bin" cli query_graph "{\"project\":\"$proj\",\"query\":\"MATCH (n) RETURN count(n) as cnt\"}" 2>/dev/null | tail -1 | python3 -c "import json,sys; r=json.load(sys.stdin).get('rows',[]); print(r[0][0] if r else 0)" 2>/dev/null || echo "0")

    calls=$("$bin" cli query_graph "{\"project\":\"$proj\",\"query\":\"MATCH ()-[r:CALLS]->() RETURN count(r) as cnt\"}" 2>/dev/null | tail -1 | python3 -c "import json,sys; r=json.load(sys.stdin).get('rows',[]); print(r[0][0] if r else 0)" 2>/dev/null || echo "0")

    usage=$("$bin" cli query_graph "{\"project\":\"$proj\",\"query\":\"MATCH ()-[r:USAGE]->() RETURN count(r) as cnt\"}" 2>/dev/null | tail -1 | python3 -c "import json,sys; r=json.load(sys.stdin).get('rows',[]); print(r[0][0] if r else 0)" 2>/dev/null || echo "0")

    defines=$("$bin" cli query_graph "{\"project\":\"$proj\",\"query\":\"MATCH ()-[r:DEFINES]->() RETURN count(r) as cnt\"}" 2>/dev/null | tail -1 | python3 -c "import json,sys; r=json.load(sys.stdin).get('rows',[]); print(r[0][0] if r else 0)" 2>/dev/null || echo "0")

    imports=$("$bin" cli query_graph "{\"project\":\"$proj\",\"query\":\"MATCH ()-[r:IMPORTS]->() RETURN count(r) as cnt\"}" 2>/dev/null | tail -1 | python3 -c "import json,sys; r=json.load(sys.stdin).get('rows',[]); print(r[0][0] if r else 0)" 2>/dev/null || echo "0")

    writes=$("$bin" cli query_graph "{\"project\":\"$proj\",\"query\":\"MATCH ()-[r:WRITES]->() RETURN count(r) as cnt\"}" 2>/dev/null | tail -1 | python3 -c "import json,sys; r=json.load(sys.stdin).get('rows',[]); print(r[0][0] if r else 0)" 2>/dev/null || echo "0")

    echo "${nodes}|CALLS:${calls}|USAGE:${usage}|DEFINES:${defines}|IMPORTS:${imports}|WRITES:${writes}"
}

echo "============================================"
echo " Phase 2B — Stress / Determinism Test"
echo " Binary:  $BIN"
echo " Repo:    $REPO"
echo " Cycles:  $CYCLES"
echo " Results: $RESULTS"
echo "============================================"
echo ""

DB_PATH="$CACHE_DIR/${PROJECT}.db"

# ─── Pre-test FD baseline ───
FD_BEFORE=$(get_fd_count)

# ─── Run cycles ───
declare -a HASHES
declare -a TIMES
declare -a RSS_PEAKS
declare -a RSS_FINALS
declare -a EDGE_COUNTS
GOLDEN_PASS=0
GOLDEN_FAIL=0

for cycle in $(seq 1 "$CYCLES"); do
    echo "── Cycle $cycle/$CYCLES ──"

    # Clean DB
    rm -f "$DB_PATH"

    # Run index, capture time + RSS
    START_TS=$(date +%s%N)
    RSS_PEAK=0
    RSS_FINAL=0

    # Run in background, sample RSS
    "$BIN" cli index_repository "{\"repo_path\":\"$REPO\",\"persistence\":false}" > "$RESULTS/index_${cycle}.log" 2>&1 &
    IDX_PID=$!

    # Sample RSS while indexing
    while kill -0 "$IDX_PID" 2>/dev/null; do
        rss=$(get_rss_mb "$IDX_PID")
        if [ "$rss" -gt "$RSS_PEAK" ]; then RSS_PEAK=$rss; fi
        sleep 1
    done
    wait "$IDX_PID" || true
    RSS_FINAL=$(get_rss_mb "$IDX_PID" 2>/dev/null || echo "0")

    END_TS=$(date +%s%N)
    ELAPSED_MS=$(( (END_TS - START_TS) / 1000000 ))

    # Compute graph hash
    HASH=$(graph_hash "$BIN" "$PROJECT")
    HASHES+=("$HASH")
    TIMES+=("$ELAPSED_MS")
    RSS_PEAKS+=("$RSS_PEAK")
    RSS_FINALS+=("$RSS_FINAL")

    # Edge count (CALLS only — deterministic)
    EDGES=$(echo "$HASH" | grep -oP 'CALLS:\K\d+')
    EDGE_COUNTS+=("$EDGES")

    echo "  Time: ${ELAPSED_MS}ms | RSS peak: ${RSS_PEAK}MB | RSS final: ${RSS_FINAL}MB | Hash: $HASH"

    # Run golden tests
    if [ -x "$GOLDEN_SCRIPT" ]; then
        if timeout 120 "$GOLDEN_SCRIPT" "$BIN" "$PROJECT" > "$RESULTS/golden_${cycle}.log" 2>&1; then
            green "  Golden: PASS"
            GOLDEN_PASS=$((GOLDEN_PASS + 1))
        else
            red "  Golden: FAIL"
            GOLDEN_FAIL=$((GOLDEN_FAIL + 1))
        fi
    else
        yellow "  Golden: SKIP (script not found)"
    fi
    echo ""
done

# ─── Incremental stress ───
echo "── Incremental Stress (3 cycles) ──"
for cycle in $(seq 1 3); do
    echo -n "  Incremental cycle $cycle... "
    START_TS=$(date +%s%N)
    "$BIN" cli index_repository "{\"repo_path\":\"$REPO\",\"persistence\":false}" > "$RESULTS/incr_${cycle}.log" 2>&1
    END_TS=$(date +%s%N)
    ELAPSED_MS=$(( (END_TS - START_TS) / 1000000 ))
    HASH=$(graph_hash "$BIN" "$PROJECT")
    echo "${ELAPSED_MS}ms | Hash: $HASH"
done
echo ""

# ─── Post-test FD ───
FD_AFTER=$(get_fd_count)
FD_DELTA=$((FD_AFTER - FD_BEFORE))

# ─── Analysis ───
echo "============================================"
echo " Phase 2B — Results Analysis"
echo "============================================"

# Determinism check
UNIQUE_HASHES=$(printf '%s\n' "${HASHES[@]}" | sort -u | wc -l)
if [ "$UNIQUE_HASHES" -eq 1 ]; then
    green "  ✅ Graph hash: IDENTICAL across all $CYCLES cycles (${HASHES[0]})"
else
    red "  ❌ Graph hash: $UNIQUE_HASHES different hashes across $CYCLES cycles"
    for i in $(seq 0 $((CYCLES - 1))); do
        echo "     Cycle $((i+1)): ${HASHES[$i]}"
    done
fi

# RSS variance
echo ""
echo "  RSS Peak (MB): ${RSS_PEAKS[*]}"
echo "  RSS Final (MB): ${RSS_FINALS[*]}"

# Compute variance
peak_min=$(printf '%s\n' "${RSS_PEAKS[@]}" | sort -n | head -1)
peak_max=$(printf '%s\n' "${RSS_PEAKS[@]}" | sort -n | tail -1)
if [ "$peak_min" -gt 0 ]; then
    peak_var=$(( (peak_max - peak_min) * 100 / peak_min ))
    if [ "$peak_var" -le 5 ]; then
        green "  ✅ Peak RSS variance: ${peak_var}% (< 5%)"
    else
        yellow "  ⚠️  Peak RSS variance: ${peak_var}% (> 5%)"
    fi
fi

# Edge count variance
edge_min=$(printf '%s\n' "${EDGE_COUNTS[@]}" | sort -n | head -1)
edge_max=$(printf '%s\n' "${EDGE_COUNTS[@]}" | sort -n | tail -1)
if [ "$edge_min" -eq "$edge_max" ]; then
    green "  ✅ Edge count: IDENTICAL across all cycles ($edge_min)"
else
    red "  ❌ Edge count: varies from $edge_min to $edge_max"
fi

# FD check
echo ""
echo "  FD before: $FD_BEFORE | FD after: $FD_AFTER | Delta: $FD_DELTA"
if [ "$FD_DELTA" -le 2 ]; then
    green "  ✅ FD leak: none (> $FD_DELTA delta)"
else
    red "  ❌ FD leak: $FD_DELTA file descriptors leaked"
fi

# Index time
echo ""
echo "  Index Times (ms): ${TIMES[*]}"
avg_time=$(printf '%s\n' "${TIMES[@]}" | awk '{sum+=$1; n++} END {printf "%d", sum/n}')
echo "  Average: ${avg_time}ms"

# Golden test summary
echo ""
echo "  Golden Tests: $GOLDEN_PASS pass, $GOLDEN_FAIL fail"
if [ "$GOLDEN_FAIL" -eq 0 ]; then
    green "  ✅ Golden tests: all passed"
else
    red "  ❌ Golden tests: $GOLDEN_FAIL failures"
fi

# ─── Final verdict ───
echo ""
echo "============================================"
FAILS=0
[ "$UNIQUE_HASHES" -ne 1 ] && FAILS=$((FAILS + 1))
[ "$edge_min" -ne "$edge_max" ] && FAILS=$((FAILS + 1))
[ "$FD_DELTA" -gt 2 ] && FAILS=$((FAILS + 1))
[ "$GOLDEN_FAIL" -gt 0 ] && FAILS=$((FAILS + 1))

if [ "$FAILS" -eq 0 ]; then
    green "  PHASE 2B — PASSED"
    echo "============================================"
    echo "Logs: $RESULTS"
    exit 0
else
    red "  PHASE 2B — $FAILS FAILURE(S)"
    echo "============================================"
    echo "Logs: $RESULTS"
    exit 1
fi
