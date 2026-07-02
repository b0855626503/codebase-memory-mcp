#!/usr/bin/env bash
# benchmark-regression.sh — Graph regression benchmark for custom fork stability
#
# Usage:
#   scripts/benchmark-regression.sh <binary> <project_name> <repo_path>
#   scripts/benchmark-regression.sh --compare <baseline.json> <current.json>
#
# Captures per-edge-type counts and functional metrics, saves as JSON baseline.
# Before merging any upstream PR, run this to detect regressions.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY=""
PROJECT=""
REPO=""
BASELINE_DIR="$ROOT/.benchmark-baselines"
COMPARE_MODE=false
BASELINE_FILE=""
CURRENT_FILE=""

# ── Parse args ──────────────────────────────────────────────────────

if [ "${1:-}" = "--compare" ]; then
    COMPARE_MODE=true
    BASELINE_FILE="${2:?Usage: --compare <baseline.json> <current.json>}"
    CURRENT_FILE="${3:?}"
elif [ $# -ge 3 ]; then
    BINARY="${1:?}"
    PROJECT="${2:?}"
    REPO="${3:?}"
else
    echo "Usage:"
    echo "  benchmark-regression.sh <binary> <project_name> <repo_path>"
    echo "  benchmark-regression.sh --compare <baseline.json> <current.json>"
    exit 1
fi

# ── Helper: call MCP tool via binary CLI ────────────────────────────

call_mcp() {
    local tool="$1" args="$2"
    "$BINARY" cli "$tool" "$args" 2>/dev/null || echo '{"error":"call failed"}'
}

# ── Helper: extract value from CLI response ─────────────────────────
# CLI returns raw JSON (no MCP content envelope), format:
#   {"columns":["cnt"],"rows":[["123"]],"total":1}

extract_field() {
    python3 -c "
import json,sys
try:
    d=json.load(sys.stdin)
    # CLI query_graph returns {\"columns\":[...],\"rows\":[[...]],\"total\":N}
    if 'rows' in d and len(d['rows'])>0 and len(d['rows'][0])>0:
        print(d['rows'][0][0])
    else:
        print(0)
except: print(0)
"
}

# ── Query edge count by type ────────────────────────────────────────

query_edge_count() {
    local project="$1" edge_type="$2"
    call_mcp query_graph "{\"project\":\"$project\",\"query\":\"MATCH ()-[e:$edge_type]->() RETURN count(e) AS cnt\"}" \
        | extract_field
}

# ── Query node count by label ───────────────────────────────────────

query_node_count() {
    local project="$1" label="$2"
    if [ -z "$label" ]; then
        call_mcp query_graph "{\"project\":\"$project\",\"query\":\"MATCH (n) RETURN count(n) AS cnt\"}" \
            | extract_field
    else
        call_mcp query_graph "{\"project\":\"$project\",\"query\":\"MATCH (n:$label) RETURN count(n) AS cnt\"}" \
            | extract_field
    fi
}

# ── Collect all metrics ─────────────────────────────────────────────

collect_metrics() {
    local project="$1"

    echo "  Collecting graph metrics..." >&2

    # Edge types
    local calls=$(query_edge_count "$project" "CALLS")
    local routes_to=$(query_edge_count "$project" "ROUTES_TO")
    local uses_model=$(query_edge_count "$project" "USES_MODEL")
    local owns_model=$(query_edge_count "$project" "OWNS_MODEL")
    local imports=$(query_edge_count "$project" "IMPORTS")
    local defines=$(query_edge_count "$project" "DEFINES")
    local extends=$(query_edge_count "$project" "EXTENDS")
    local implements=$(query_edge_count "$project" "IMPLEMENTS")
    local http_calls=$(query_edge_count "$project" "HTTP_CALLS")
    local uses_service=$(query_edge_count "$project" "USES_SERVICE")
    local similar_to=$(query_edge_count "$project" "SIMILAR_TO")
    local semantically_related=$(query_edge_count "$project" "SEMANTICALLY_RELATED")

    # Node labels
    local functions=$(query_node_count "$project" "Function")
    local methods=$(query_node_count "$project" "Method")
    local classes=$(query_node_count "$project" "Class")
    local routes=$(query_node_count "$project" "Route")
    local models=$(query_node_count "$project" "Model")
    local controllers=$(query_node_count "$project" "Controller")
    local files=$(query_node_count "$project" "File")

    # Total
    local total_nodes=$(query_node_count "$project" "")
    local total_edges=$(query_edge_count "$project" "")

    # ── trace_path smoke tests ──────────────────────────────────────
    echo "  Running trace_path smoke tests..." >&2

    # Find a function to trace
    local trace_fn=$(call_mcp query_graph \
        "{\"project\":\"$project\",\"query\":\"MATCH (n:Function) WHERE n.name CONTAINS 'handle' OR n.name CONTAINS 'process' RETURN n.qualified_name AS qn LIMIT 3\"}" \
        | python3 -c "
import json,sys
try:
    d=json.load(sys.stdin)
    if 'rows' in d and len(d['rows'])>0:
        print(d['rows'][0][0])
    else:
        print('')
except: print('')
" 2>/dev/null)

    local trace_depth=0
    if [ -n "$trace_fn" ] && [ "$trace_fn" != "0" ]; then
        # Escape the QN for JSON
        local escaped_qn=$(python3 -c "import json; print(json.dumps('$trace_fn'))")
        trace_depth=$(call_mcp trace_path \
            "{\"project\":\"$project\",\"function_name\":$escaped_qn,\"mode\":\"calls\",\"max_depth\":3}" \
            | python3 -c "
import json,sys
try:
    d=json.load(sys.stdin)
    if 'content' in d:
        inner=json.loads(d['content'][0]['text'])
    else:
        inner=d
    # Count nodes in the traced path
    def count_nodes(obj, depth=0):
        if isinstance(obj, dict):
            total=0
            if 'id' in obj: total+=1
            for v in obj.values(): total+=count_nodes(v,depth+1)
            return total
        elif isinstance(obj, list):
            return sum(count_nodes(i,depth+1) for i in obj)
        return 0
    print(count_nodes(inner))
except: print(0)
" 2>/dev/null || echo "0")
    fi

    # ── Architecture ────────────────────────────────────────────────
    echo "  Collecting architecture..." >&2

    local arch_layers=$(call_mcp get_architecture \
        "{\"project\":\"$project\",\"aspects\":[\"layers\"]}" \
        | python3 -c "
import json,sys
try:
    d=json.load(sys.stdin)
    if 'content' in d:
        inner=json.loads(d['content'][0]['text'])
    else:
        inner=d
    layers=inner.get('layers',[]) if isinstance(inner,dict) else []
    print(len(layers))
except: print(0)
" 2>/dev/null || echo "0")

    local arch_pkgs=$(call_mcp get_architecture \
        "{\"project\":\"$project\",\"aspects\":[\"packages\"]}" \
        | python3 -c "
import json,sys
try:
    d=json.load(sys.stdin)
    if 'content' in d:
        inner=json.loads(d['content'][0]['text'])
    else:
        inner=d
    pkgs=inner.get('packages',[]) if isinstance(inner,dict) else []
    print(len(pkgs))
except: print(0)
" 2>/dev/null || echo "0")

    # ── Output JSON ─────────────────────────────────────────────────
    python3 -c "
import json
print(json.dumps({
    'project': '$project',
    'timestamp': '$(date -Iseconds)',
    'commit': '$(git -C "$ROOT" rev-parse --short HEAD)',
    'edges': {
        'CALLS': $calls,
        'ROUTES_TO': $routes_to,
        'USES_MODEL': $uses_model,
        'OWNS_MODEL': $owns_model,
        'IMPORTS': $imports,
        'DEFINES': $defines,
        'EXTENDS': $extends,
        'IMPLEMENTS': $implements,
        'HTTP_CALLS': $http_calls,
        'USES_SERVICE': $uses_service,
        'SIMILAR_TO': $similar_to,
        'SEMANTICALLY_RELATED': $semantically_related,
    },
    'nodes': {
        'Function': $functions,
        'Method': $methods,
        'Class': $classes,
        'Route': $routes,
        'Model': $models,
        'Controller': $controllers,
        'File': $files,
        'total': $total_nodes,
    },
    'total_edges': $total_edges,
    'trace_path': {
        'function': '${trace_fn//\"/\\\"}',
        'depth_3_nodes': $trace_depth,
    },
    'architecture': {
        'layers': $arch_layers,
        'packages': $arch_pkgs,
    },
}, indent=2))
"
}

# ── Compare two baselines ───────────────────────────────────────────

compare_baselines() {
    python3 << 'PYEOF'
import json, sys

with open(sys.argv[1]) as f: before = json.load(f)
with open(sys.argv[2]) as f: after = json.load(f)

# Only compare same-project baselines
b_proj = before.get('project','?')
a_proj = after.get('project','?')
if b_proj != a_proj:
    print(f"WARNING: comparing different projects: {b_proj} vs {a_proj}")
    print()

def delta(b, a):
    """Return (abs_change, pct_change_str)"""
    try:
        bv = int(b); av = int(a)
    except (ValueError, TypeError):
        return 0, "N/A"
    d = av - bv
    if bv == 0:
        pct = "NEW" if d > 0 else "0%"
    else:
        pct = f"{d/bv*100:+.1f}%"
    return d, pct

def status_str(d, pct):
    """Return status indicator."""
    if pct == "N/A": return "  "
    if pct == "0%": return "✓ "
    try:
        pct_f = float(pct.replace('%','').replace('+',''))
        if abs(pct_f) < 1: return "≈ "
        if pct_f > 0: return "↑+"
        else: return "↓-"
    except: return "? "

# Header
print(f"{'Metric':<30} {'Before':>10} {'After':>10} {'Δ':>10} {'%':>10}")
print("-" * 70)

# Edges
print("── EDGES ──")
for key in before.get('edges',{}):
    b = before['edges'].get(key,0)
    a = after['edges'].get(key,0)
    d, pct = delta(b,a)
    s = status_str(d,pct)
    if d != 0 or pct == "0%":
        print(f"{s}{key:<28} {b:>10} {a:>10} {d:>+10} {pct:>10}")

# Nodes
print("── NODES ──")
for key in before.get('nodes',{}):
    b = before['nodes'].get(key,0)
    a = after['nodes'].get(key,0)
    d, pct = delta(b,a)
    s = status_str(d,pct)
    if d != 0 or pct == "0%":
        print(f"{s}{key:<28} {b:>10} {a:>10} {d:>+10} {pct:>10}")

# Totals
print("── TOTALS ──")
for key in ['total_edges']:
    b = before.get(key,0)
    a = after.get(key,0)
    d, pct = delta(b,a)
    s = status_str(d,pct)
    print(f"{s}{key:<28} {b:>10} {a:>10} {d:>+10} {pct:>10}")

# Trace
print("── TRACE ──")
b = before.get('trace_path',{}).get('depth_3_nodes',0)
a = after.get('trace_path',{}).get('depth_3_nodes',0)
d, pct = delta(b,a)
s = status_str(d,pct)
print(f"{s}{'trace_path nodes':<28} {b:>10} {a:>10} {d:>+10} {pct:>10}")

# Architecture
print("── ARCH ──")
for key in ['layers','packages']:
    b = before.get('architecture',{}).get(key,0)
    a = after.get('architecture',{}).get(key,0)
    d, pct = delta(b,a)
    s = status_str(d,pct)
    print(f"{s}{'arch_'+key:<28} {b:>10} {a:>10} {d:>+10} {pct:>10}")

# Summary
print()
regressions = 0
improvements = 0
for section in ['edges','nodes']:
    for key in before.get(section,{}):
        b = before[section].get(key,0)
        a = after[section].get(key,0)
        if isinstance(b,(int,float)) and isinstance(a,(int,float)):
            if a < b: regressions += 1
            elif a > b: improvements += 1

print(f"Summary: {improvements} metrics improved, {regressions} regressed")
if regressions > 0:
    print("⚠️  REGRESSION DETECTED — investigate before merging")
    sys.exit(1)
else:
    print("✅ No regressions — safe to merge")
    sys.exit(0)
PYEOF "$BASELINE_FILE" "$CURRENT_FILE"
}

# ── Main ────────────────────────────────────────────────────────────

if $COMPARE_MODE; then
    compare_baselines "$BASELINE_FILE" "$CURRENT_FILE"
    exit $?
fi

# Resolve paths
REPO=$(cd "$REPO" && pwd -P)
BINARY=$(command -v "$BINARY" || echo "$BINARY")

echo "═══ Regression Benchmark ═══"
echo "  Binary:  $BINARY"
echo "  Project: $PROJECT"
echo "  Repo:    $REPO"
echo "  Commit:  $(git -C "$ROOT" rev-parse --short HEAD)"
echo ""

# Step 1: Re-index
echo "Step 1/3: Indexing $PROJECT..."
START_MS=$(python3 -c "import time; print(int(time.time()*1000))")

INDEX_OUT=$(call_mcp index_repository "{\"repo_path\":\"$REPO\",\"mode\":\"full\"}")
END_MS=$(python3 -c "import time; print(int(time.time()*1000))")
ELAPSED=$((END_MS - START_MS))

NODES=$(echo "$INDEX_OUT" | extract_field "nodes")
EDGES=$(echo "$INDEX_OUT" | extract_field "edges")
echo "  Done: ${ELAPSED}ms, $NODES nodes, $EDGES edges"

# Step 2: Collect detailed metrics
echo ""
echo "Step 2/3: Collecting detailed metrics..."
METRICS=$(collect_metrics "$PROJECT")

# Step 3: Save baseline
echo ""
echo "Step 3/3: Saving baseline..."
mkdir -p "$BASELINE_DIR"

BASELINE_FILE="$BASELINE_DIR/${PROJECT}-$(date +%Y%m%d-%H%M%S)-$(git -C "$ROOT" rev-parse --short HEAD).json"
echo "$METRICS" > "$BASELINE_FILE"

# Also save as "latest" for easy comparison
cp "$BASELINE_FILE" "$BASELINE_DIR/${PROJECT}-latest.json"

echo ""
echo "═══ Baseline saved ═══"
echo "  $BASELINE_FILE"
echo ""

# Print summary table
echo "$METRICS" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(f\"{'Metric':<25} {'Count':>10}\")
print('-' * 37)
for k,v in d.get('edges',{}).items():
    print(f'  {k:<23} {v:>10}')
print(f\"  {'total_edges':<23} {d.get('total_edges',0):>10}\")
for k,v in d.get('nodes',{}).items():
    print(f'  {k:<23} {v:>10}')
print(f\"  trace_depth3_nodes  {d.get('trace_path',{}).get('depth_3_nodes',0):>10}\")
print(f\"  arch_layers         {d.get('architecture',{}).get('layers',0):>10}\")
print(f\"  arch_packages       {d.get('architecture',{}).get('packages',0):>10}\")
"

echo ""
echo "To compare later:"
echo "  scripts/benchmark-regression.sh --compare $BASELINE_DIR/${PROJECT}-latest.json <new-baseline.json>"
