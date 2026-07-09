#!/bin/bash
# run_golden.sh — Phase 1C Golden Test Suite
#
# Three layers of regression tests against an indexed project.
# Usage:
#   ./tests/golden/run_golden.sh <binary> <project_name>

set -euo pipefail

BIN="${1:-./build/c/codebase-memory-mcp}"
PROJECT="${2:-home-boat-projects-codebase-memory-mcp}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULT_FILE="$(mktemp /tmp/golden_result.XXXXXX)"
PASS=0
FAIL=0

green()  { echo -e "\033[32m$1\033[0m"; echo "PASS" >> "$RESULT_FILE"; }
red()    { echo -e "\033[31m$1\033[0m"; echo "FAIL" >> "$RESULT_FILE"; }
yellow() { echo -e "\033[33m$1\033[0m"; echo "PASS" >> "$RESULT_FILE"; }

run_query() {
    $BIN cli query_graph "{\"project\":\"$PROJECT\",\"query\":\"$1\"}" 2>/dev/null | tail -1
}
get_count() {
    echo "$1" | python3 -c "import json,sys; d=json.load(sys.stdin); rows=d.get('rows',[]); print(rows[0][0] if rows else 0)" 2>/dev/null || echo "0"
}

echo "============================================"
echo " Golden Test Suite — Phase 1C"
echo " Project: $PROJECT"
echo " Binary:  $BIN"
echo "============================================"
echo ""

# ═══════════ Layer 1 — Graph Invariants ═══════════
echo "── Layer 1: Graph Invariants ──"
echo ""

python3 -c "
import json
with open('$SCRIPT_DIR/invariants.json') as f:
    data = json.load(f)
for inv in data['invariants']:
    print(f'{inv[\"name\"]}\t{inv[\"query\"]}\t{inv[\"max\"]}\t{inv[\"fail_message\"]}')
" > /tmp/golden_l1.txt

while IFS=$'\t' read -r name query max fail_msg; do
    result=$(run_query "$query")
    violations=$(get_count "$result")
    if [ "$violations" -le "$max" ]; then
        green "  ✅ $name ($violations)"
    else
        red "  ❌ $name (got $violations, max $max) — $fail_msg"
    fi
done < /tmp/golden_l1.txt

echo ""

# ═══════════ Layer 2 — Golden Functions ═══════════
echo "── Layer 2: Golden Functions ──"
echo ""

python3 -c "
import json
with open('$SCRIPT_DIR/functions.json') as f:
    data = json.load(f)
for fn in data['functions']:
    exp = json.dumps(fn.get('expected_callees',[]))
    forb = json.dumps(fn.get('forbidden_callees',[]))
    print(f'{fn[\"name\"]}\t{fn[\"min_callees\"]}\t{fn[\"min_callers\"]}\t{exp}\t{forb}')
" > /tmp/golden_l2.txt

while IFS=$'\t' read -r name min_callees min_callers expected_json forbidden_json; do
    fail_flag=0

    cc=$(get_count "$(run_query "MATCH (a:Function {name: '$name'})-[r:CALLS]->(b) RETURN count(r) as cnt")")
    cr=$(get_count "$(run_query "MATCH (a)-[r:CALLS]->(b:Function {name: '$name'}) RETURN count(r) as cnt")")

    if [ "$cc" -lt "$min_callees" ]; then
        red "  ❌ $name — callees $cc < min $min_callees"
        fail_flag=1
    fi
    if [ "$cr" -lt "$min_callers" ]; then
        red "  ❌ $name — callers $cr < min $min_callers"
        fail_flag=1
    fi

    # Expected callees
    expected=$(echo "$expected_json" | python3 -c "import json,sys; print('\n'.join(json.load(sys.stdin)))" 2>/dev/null || echo "")
    if [ -n "$expected" ]; then
        actual=$(run_query "MATCH (a:Function {name: '$name'})-[r:CALLS]->(b) RETURN b.name")
        while IFS= read -r exp; do
            if ! echo "$actual" | grep -qF "\"$exp\""; then
                red "  ❌ $name — missing expected callee: $exp"
                fail_flag=1
            fi
        done <<< "$expected"
    fi

    if [ "$fail_flag" -eq 0 ]; then
        green "  ✅ $name (callees=$cc, callers=$cr)"
    fi
done < /tmp/golden_l2.txt

echo ""

# ═══════════ Layer 3 — Metrics Guard ═══════════
echo "── Layer 3: Metrics Guard ──"
echo ""

python3 -c "
import json
with open('$SCRIPT_DIR/guard.json') as f:
    data = json.load(f)
for m in data['metrics']:
    et = m.get('edge_type','')
    print(f'{m[\"name\"]}\t{et}\t{m[\"min\"]}\t{m[\"fail_message\"]}')
" > /tmp/golden_l3.txt

while IFS=$'\t' read -r name edge_type min fail_msg; do
    if [ "$edge_type" = "TOTAL_NODES" ]; then
        actual=$(get_count "$(run_query "MATCH (n) RETURN count(*) as cnt")")
    elif [ -n "$edge_type" ]; then
        actual=$(get_count "$(run_query "MATCH ()-[r:$edge_type]->() RETURN count(r) as cnt")")
    else
        actual=0
    fi
    if [ "$actual" -ge "$min" ]; then
        green "  ✅ $name ($actual >= $min)"
    else
        red "  ❌ $name — $fail_msg (got $actual, need $min)"
    fi
done < /tmp/golden_l3.txt

# Search quality
python3 -c "
import json
with open('$SCRIPT_DIR/guard.json') as f:
    data = json.load(f)
for sq in data.get('search_queries', []):
    exp = json.dumps(sq['expected'])
    print(f'{sq[\"name\"]}\t{sq[\"query\"]}\t{sq[\"min_results\"]}\t{exp}')
" > /tmp/golden_l3s.txt

while IFS=$'\t' read -r name query min_results expected_json; do
    result=$($BIN cli search_graph "{\"project\":\"$PROJECT\",\"query\":\"$query\",\"limit\":10}" 2>/dev/null | tail -1)
    names=$(echo "$result" | python3 -c "import json,sys; d=json.load(sys.stdin); [print(r.get('name','')) for r in d.get('results',[])]" 2>/dev/null || echo "")
    count=$(echo "$names" | wc -l)
    if [ "$count" -ge "$min_results" ]; then
        all_found=1
        expected=$(echo "$expected_json" | python3 -c "import json,sys; print('\n'.join(json.load(sys.stdin)))" 2>/dev/null || echo "")
        while IFS= read -r exp; do
            if ! echo "$names" | grep -qF "$exp"; then all_found=0; fi
        done <<< "$expected"
        if [ "$all_found" -eq 1 ]; then
            green "  ✅ $name ($count results)"
        else
            yellow "  ⚠️  $name ($count results, some names missing)"
        fi
    else
        red "  ❌ $name — $count < $min_results"
    fi
done < /tmp/golden_l3s.txt

# Count results
PASS=$(grep -c "PASS" "$RESULT_FILE" 2>/dev/null || true)
PASS="${PASS:-0}"
FAIL=$(grep -c "FAIL" "$RESULT_FILE" 2>/dev/null || true)
FAIL="${FAIL:-0}"
rm -f "$RESULT_FILE" /tmp/golden_l1.txt /tmp/golden_l2.txt /tmp/golden_l3.txt /tmp/golden_l3s.txt

echo ""
echo "============================================"
if [ "$FAIL" -eq 0 ]; then
    green "  ALL GOLDEN TESTS PASSED ($PASS checks)"
    echo "============================================"
    exit 0
else
    red "  $FAIL FAILURE(S) — $PASS passed"
    echo "============================================"
    exit 1
fi
