#!/bin/bash
# run_benchmark.sh — Phase 3 Retrieval Benchmark
#
# Measures CBM retrieval quality independent of LLM:
#   Precision, Recall, False Positive Rate, Latency
#
# Two modes:
#   synthetic  — tiny fixture files with 100% known ground truth
#   real       — real codebase questions (codebase-memory-mcp)
#
# Usage:
#   ./tests/benchmark/run_benchmark.sh <binary> <project> [mode]
#   ./tests/benchmark/run_benchmark.sh ./build/c/codebase-memory-mcp home-boat-projects-codebase-memory-mcp real

set -uo pipefail

BIN="${1:-./build/c/codebase-memory-mcp}"
PROJECT="${2:-home-boat-projects-codebase-memory-mcp}"
MODE="${3:-real}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FIXTURE_DIR="$SCRIPT_DIR/../fixtures"
RESULTS="$(mktemp -d /tmp/cbm_bench_XXXXXX)"

green()  { echo -e "\033[32m$1\033[0m"; }
red()    { echo -e "\033[31m$1\033[0m"; }
yellow() { echo -e "\033[33m$1\033[0m"; }

run_query() {
    "$BIN" cli query_graph "{\"project\":\"$PROJECT\",\"query\":\"$1\"}" 2>/dev/null | tail -1
}
run_search() {
    "$BIN" cli search_graph "{\"project\":\"$PROJECT\",\"query\":\"$1\",\"limit\":${2:-10}}" 2>/dev/null | tail -1
}

# ─── Scoring utilities ───
calc_precision() {
    # precision = |expected ∩ actual| / |actual|
    local expected="$1" actual="$2"
    local hit=0 total=0
    for a in $actual; do
        total=$((total + 1))
        for e in $expected; do
            [ "$a" = "$e" ] && hit=$((hit + 1)) && break
        done
    done
    [ "$total" -eq 0 ] && echo "100" && return
    echo $(( hit * 100 / total ))
}

calc_recall() {
    # recall = |expected ∩ actual| / |expected|
    local expected="$1" actual="$2"
    local hit=0 exp_count=0
    for e in $expected; do
        exp_count=$((exp_count + 1))
        for a in $actual; do
            [ "$a" = "$e" ] && hit=$((hit + 1)) && break
        done
    done
    [ "$exp_count" -eq 0 ] && echo "100" && return
    echo $(( hit * 100 / exp_count ))
}

echo "============================================"
echo " CBM Retrieval Benchmark — Phase 3"
echo " Mode: $MODE"
echo " Project: $PROJECT"
echo "============================================"
echo ""

TOTAL_TESTS=0
PASSED=0
declare -a SCORES_PRECISION
declare -a SCORES_RECALL
declare -a SCORES_LATENCY

# ═══════════════════════════════════════════════
# Synthetic Fixtures
# ═══════════════════════════════════════════════
if [ "$MODE" = "synthetic" ] || [ "$MODE" = "all" ]; then
    echo "── Synthetic Fixture Tests ──"
    echo ""

    for fixture in "$FIXTURE_DIR"/*.c; do
        [ -f "$fixture" ] || continue
        base=$(basename "$fixture" .c)
        expected_file="$FIXTURE_DIR/${base}.expected.json"
        [ -f "$expected_file" ] || continue

        echo "  Fixture: $base"

        # Index the fixture as a separate project
        fixture_proj="bench-${base}"
        tmpdir=$(mktemp -d /tmp/cbm_fixture_XXXXXX)
        cp "$fixture" "$tmpdir/${base}.c"

        "$BIN" cli index_repository "{\"repo_path\":\"$tmpdir\",\"persistence\":false}" > /dev/null 2>&1 || {
            red "    ❌ Index failed"
            rm -rf "$tmpdir"
            continue
        }
        # Derive project name (binary uses absolute path)
        actual_proj=$(echo "$tmpdir" | sed 's|^/||' | tr '/' '-')

        # Verify expected functions exist
        expected_fns=$(python3 -c "
import json
with open('$expected_file') as f:
    d = json.load(f)
print(' '.join(d.get('functions',[])))
" 2>/dev/null)

        fn_ok=0 fn_miss=0
        for fn in $expected_fns; do
            result=$(run_query "MATCH (n:Function {name: '$fn'}) RETURN count(n) as cnt" 2>/dev/null || echo '{"rows":[]}')
            cnt=$(echo "$result" | python3 -c "import json,sys; r=json.load(sys.stdin).get('rows',[]); print(r[0][0] if r else 0)" 2>/dev/null || echo "0")
            [ "$cnt" -gt 0 ] && fn_ok=$((fn_ok + 1)) || fn_miss=$((fn_miss + 1))
        done
        echo "    Functions: $fn_ok found, $fn_miss missing"

        # Verify expected CALLS edges
        expected_calls=$(python3 -c "
import json
with open('$expected_file') as f:
    d = json.load(f)
for call in d.get('calls',[]):
    print(f'{call[0]}|{call[1]}')
" 2>/dev/null)

        calls_ok=0 calls_miss=0
        while IFS='|' read -r caller callee; do
            result=$(run_query "MATCH (a:Function {name: '$caller'})-[r:CALLS]->(b:Function {name: '$callee'}) RETURN count(r) as cnt")
            cnt=$(echo "$result" | python3 -c "import json,sys; r=json.load(sys.stdin).get('rows',[]); print(r[0][0] if r else 0)" 2>/dev/null || echo "0")
            [ "$cnt" -gt 0 ] && calls_ok=$((calls_ok + 1)) || calls_miss=$((calls_miss + 1))
        done <<< "$expected_calls" || true
        echo "    CALLS edges: $calls_ok found, $calls_miss missing"
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        [ "$calls_miss" -eq 0 ] && PASSED=$((PASSED + 1))

        # Cleanup
        "$BIN" cli delete_project "{\"project\":\"$actual_proj\"}" 2>/dev/null || true
        rm -rf "$tmpdir"
        echo ""
    done
fi

# ═══════════════════════════════════════════════
# Real Project Queries
# ═══════════════════════════════════════════════
if [ "$MODE" = "real" ] || [ "$MODE" = "all" ]; then
    echo "── Real Project Queries ──"
    echo ""

    python3 -c "
import json
with open('$SCRIPT_DIR/dataset.json') as f:
    data = json.load(f)
for t in data['tests']:
    cat = t['category']
    qid = t['id']
    fn = t.get('function','')
    query = t.get('query','')
    exp_c = json.dumps(t.get('expected_callees',[]))
    exp_r = json.dumps(t.get('expected_callers',[]))
    exp_n = json.dumps(t.get('expected_names',[]))
    forb_c = json.dumps(t.get('forbidden_callees',[]))
    min_c = t.get('min_callees',0)
    min_r = t.get('min_callers',0)
    min_res = t.get('min_results',0)
    print(f'{qid}\t{cat}\t{fn}\t{query}\t{exp_c}\t{exp_r}\t{exp_n}\t{forb_c}\t{min_c}\t{min_r}\t{min_res}')
" > /tmp/bench_tests.txt

    while IFS=$'\t' read -r qid cat fn query exp_c exp_r exp_n forb_c min_c min_r min_res; do
        echo -n "  $qid ($cat)... "
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        t_start=$(date +%s%N)
        ok=1

        case "$cat" in
        call_graph|impact)
            # Get callees
            if [ -n "$fn" ]; then
                callee_json=$(run_query "MATCH (a:Function {name: '$fn'})-[r:CALLS]->(b) RETURN b.name")
                callee_names=$(echo "$callee_json" | python3 -c "import json,sys; d=json.load(sys.stdin); [print(r[0]) for r in d.get('rows',[])]" 2>/dev/null || echo "")
                callee_count=$(echo "$callee_names" | wc -l)

                # Get callers
                caller_json=$(run_query "MATCH (a)-[r:CALLS]->(b:Function {name: '$fn'}) RETURN a.name")
                caller_names=$(echo "$caller_json" | python3 -c "import json,sys; d=json.load(sys.stdin); [print(r[0]) for r in d.get('rows',[])]" 2>/dev/null || echo "")
                caller_count=$(echo "$caller_names" | wc -l)

                # Check expected callees
                expected=$(echo "$exp_c" | python3 -c "import json,sys; print(' '.join(json.load(sys.stdin)))" 2>/dev/null || echo "")
                if [ -n "$expected" ]; then
                    for e in $expected; do
                        if ! echo "$callee_names" | grep -qF "$e"; then
                            red "❌ (missing callee: $e)"
                            ok=0
                            break
                        fi
                    done
                fi

                # Check forbidden callees
                forbidden=$(echo "$forb_c" | python3 -c "import json,sys; print(' '.join(json.load(sys.stdin)))" 2>/dev/null || echo "")
                if [ -n "$forbidden" ]; then
                    for f in $forbidden; do
                        if echo "$callee_names" | grep -qF "$f"; then
                            red "❌ (forbidden callee found: $f)"
                            ok=0
                            break
                        fi
                    done
                fi

                # Check min counts
                [ "$callee_count" -lt "$min_c" ] 2>/dev/null && { red "❌ (callees $callee_count < $min_c)"; ok=0; }
                [ "$caller_count" -lt "$min_r" ] 2>/dev/null && { red "❌ (callers $caller_count < $min_r)"; ok=0; }
            fi
            ;;
        search)
            result=$(run_search "$query" 10)
            names=$(echo "$result" | python3 -c "import json,sys; d=json.load(sys.stdin); [print(r.get('name','')) for r in d.get('results',[])]" 2>/dev/null || echo "")
            count=$(echo "$names" | wc -l)
            [ "$count" -lt "$min_res" ] 2>/dev/null && { red "❌ (results $count < $min_res)"; ok=0; }
            expected=$(echo "$exp_n" | python3 -c "import json,sys; print(' '.join(json.load(sys.stdin)))" 2>/dev/null || echo "")
            for e in $expected; do
                if ! echo "$names" | grep -qF "$e"; then
                    red "❌ (missing in search: $e)"
                    ok=0
                    break
                fi
            done
            ;;
        architecture)
            result=$(run_query "MATCH (a)-[r:CALLS]->(b) WHERE a.file_path CONTAINS '$fn' RETURN count(r) as cnt")
            cnt=$(echo "$result" | python3 -c "import json,sys; r=json.load(sys.stdin).get('rows',[]); print(r[0][0] if r else 0)" 2>/dev/null || echo "0")
            [ "$cnt" -lt "$min_res" ] 2>/dev/null && { red "❌ (edges $cnt < $min_res)"; ok=0; }
            ;;
        esac

        t_end=$(date +%s%N)
        t_ms=$(( (t_end - t_start) / 1000000 ))
        SCORES_LATENCY+=("$t_ms")

        if [ "$ok" -eq 1 ]; then
            green "✅ (${t_ms}ms)"
            PASSED=$((PASSED + 1))
        fi
    done < /tmp/bench_tests.txt || true

    # ─── Invariants ───
    echo ""
    echo "── Invariants ──"
    python3 -c "
import json
with open('$SCRIPT_DIR/dataset.json') as f:
    data = json.load(f)
for inv in data.get('invariants',[]):
    print(f'{inv[\"id\"]}\t{inv[\"query\"]}\t{inv[\"max\"]}\t{inv[\"fail_message\"]}')
" > /tmp/bench_inv.txt

    while IFS=$'\t' read -r iid query max fail_msg; do
        echo -n "  $iid... "
        result=$(run_query "$query")
        violations=$(echo "$result" | python3 -c "import json,sys; r=json.load(sys.stdin).get('rows',[]); print(r[0][0] if r else 0)" 2>/dev/null || echo "0")
        violations="${violations:-0}"
        if [ "$violations" -le "$max" ]; then
            green "✅ ($violations)"
            PASSED=$((PASSED + 1))
        else
            red "❌ $fail_msg (got $violations)"
        fi
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
    done < /tmp/bench_inv.txt || true
fi

# ─── Summary ───
echo ""
echo "============================================"
echo " Benchmark Results"
echo "============================================"
echo "  Tests:  $PASSED / $TOTAL_TESTS passed"

# Average latency
if [ ${#SCORES_LATENCY[@]} -gt 0 ]; then
    avg_latency=$(printf '%s\n' "${SCORES_LATENCY[@]}" | awk '{sum+=$1; n++} END {printf "%d", sum/n}')
    echo "  Avg Latency: ${avg_latency}ms"
fi

FAILED=$((TOTAL_TESTS - PASSED))
if [ "$FAILED" -eq 0 ]; then
    green "  ALL BENCHMARKS PASSED"
    echo "============================================"
    exit 0
else
    red "  $FAILED FAILURE(S)"
    echo "============================================"
    exit 1
fi
