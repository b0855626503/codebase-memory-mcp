/*
 * pass_embedding_context.c — Embedding Context Builder Pass
 *
 * Enriches Method, Function, and Class nodes with graph context tokens
 * stored in a node property `embedding_context`. The context helps
 * downstream LLM embeddings understand a node's structural role.
 *
 * Context tokens (1-hop, max 10 names per direction):
 *   CLASS:<parent_class_short_name>      — from parent_class property
 *   CALLS:<callee_names>                  — 1-hop outbound CALLS edges
 *   CALLED_BY:<caller_names>             — 1-hop inbound CALLS+ROUTES_TO edges
 *   ROUTE:<method>:<path>                 — from inbound ROUTES_TO route node
 *   INHERITS:<parent_class_names>         — from INHERITS edges
 */
#include "pipeline/pass_embedding_context.h"
#include "graph_buffer/graph_buffer.h"
#include "pipeline/pipeline.h"
#include "foundation/constants.h"
#include "foundation/log.h"

#include <yyjson/yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum names to include per context direction. */
#define CTX_MAX_NAMES 10

/* Context buffer size. */
#define CTX_BUF_SZ CBM_SZ_2K

/* Properties buffer for upsert. */
#define PROPS_BUF_SZ (CBM_SZ_4K + CBM_SZ_2K)

/* ── Helpers ────────────────────────────────────────────────────── */

/* Extract the short name (last segment) from a qualified name.
 * Returns a pointer into qn or qn itself if no separator found.
 * E.g. "App.Services.WalletService" → "WalletService"
 *      "App\\Http\\Controllers\\DepositController" → "DepositController" */
/* CLASS tag disabled — short_name no longer needed */
static const char *short_name(const char *qn) __attribute__((unused));
static const char *short_name(const char *qn) {
    if (!qn) return "?";
    const char *dot = strrchr(qn, '.');
    const char *bs = strrchr(qn, '\\');
    const char *sep = dot > bs ? dot : bs;
    return sep ? sep + 1 : qn;
}

/* Append comma_separated_names from outbound edges of given type.
 * Limits to CTX_MAX_NAMES. Returns number of names appended. */
static int append_outbound_names(cbm_gbuf_t *gbuf, int64_t node_id, const char *edge_type,
                                  char *buf, size_t bufsz, size_t *pos) {
    const cbm_gbuf_edge_t **edges = NULL;
    int ecount = 0;
    if (cbm_gbuf_find_edges_by_source_type(gbuf, node_id, edge_type, &edges, &ecount) != 0 ||
        ecount == 0) {
        return 0;
    }

    int added = 0;
    for (int i = 0; i < ecount && added < CTX_MAX_NAMES; i++) {
        const cbm_gbuf_node_t *target = cbm_gbuf_find_by_id(gbuf, edges[i]->target_id);
        if (!target || !target->name) continue;
        if (added > 0) {
            if (*pos < bufsz) buf[(*pos)++] = ',';
        }
        size_t remain = bufsz - *pos;
        *pos += (size_t)snprintf(buf + *pos, remain, "%s", target->name);
        added++;
    }
    return added;
}

/* Append comma_separated names from inbound edges of given type.
 * Limits to CTX_MAX_NAMES. Returns number of names appended. */
static int append_inbound_names(cbm_gbuf_t *gbuf, int64_t node_id, const char *edge_type,
                                 char *buf, size_t bufsz, size_t *pos, bool *has_any) {
    const cbm_gbuf_edge_t **edges = NULL;
    int ecount = 0;
    if (cbm_gbuf_find_edges_by_target_type(gbuf, node_id, edge_type, &edges, &ecount) != 0 ||
        ecount == 0) {
        return 0;
    }

    int added = 0;
    for (int i = 0; i < ecount && added < CTX_MAX_NAMES; i++) {
        const cbm_gbuf_node_t *src = cbm_gbuf_find_by_id(gbuf, edges[i]->source_id);
        if (!src || !src->name) continue;
        if (*has_any) {
            if (*pos < bufsz) buf[(*pos)++] = ',';
        }
        size_t remain = bufsz - *pos;
        *pos += (size_t)snprintf(buf + *pos, remain, "%s", src->name);
        added++;
        *has_any = true;
    }
    return added;
}

/* Build embedding_context JSON for a single node and upsert it.
 * Returns 1 if the node was updated, 0 on skip/error. */
static int build_context_for_node(cbm_gbuf_t *gbuf, const cbm_gbuf_node_t *node) {
    if (!gbuf || !node || !node->id || !node->label || !node->qualified_name) return 0;

    char context[CTX_BUF_SZ];
    size_t pos = 0;

    /* 1. Prefix: LABEL:name */
    pos += (size_t)snprintf(context + pos, sizeof(context) - pos,
                            "%s:%s", node->label, node->name ? node->name : "?");

    /* 2. CLASS: parent_class — disabled (over-weights same-class methods) */

    /* 3. CALLS: 1-hop outbound CALLS */
    {
        size_t save = pos;
        pos += (size_t)snprintf(context + pos, sizeof(context) - pos, " CALLS:");
        int n = append_outbound_names(gbuf, node->id, "CALLS", context, sizeof(context), &pos);
        if (n == 0) pos = save; /* rollback if nothing added */
    }

    /* 4. CALLED_BY: 1-hop inbound CALLS + ROUTES_TO */
    {
        size_t save = pos;
        pos += (size_t)snprintf(context + pos, sizeof(context) - pos, " CALLED_BY:");
        bool has_any = false;
        (void)append_inbound_names(gbuf, node->id, "CALLS", context, sizeof(context), &pos,
                                    &has_any);
        (void)append_inbound_names(gbuf, node->id, "ROUTES_TO", context, sizeof(context), &pos,
                                    &has_any);
        if (!has_any) pos = save; /* rollback if nothing added */
    }

    /* 5. ROUTE: method + path from first inbound ROUTES_TO route node */
    {
        const cbm_gbuf_edge_t **r_edges = NULL;
        int r_ecount = 0;
        if (cbm_gbuf_find_edges_by_target_type(gbuf, node->id, "ROUTES_TO", &r_edges,
                                                &r_ecount) == 0 && r_ecount > 0) {
            const cbm_gbuf_node_t *route_node =
                cbm_gbuf_find_by_id(gbuf, r_edges[0]->source_id);
            if (route_node && route_node->properties_json) {
                yyjson_doc *rdoc =
                    yyjson_read(route_node->properties_json, strlen(route_node->properties_json), 0);
                if (rdoc) {
                    yyjson_val *rroot = yyjson_doc_get_root(rdoc);
                    yyjson_val *rmethod = yyjson_obj_get(rroot, "method");
                    yyjson_val *rpath = yyjson_obj_get(rroot, "path");
                    if (rmethod && rpath) {
                        pos += (size_t)snprintf(context + pos, sizeof(context) - pos,
                                                " ROUTE:%s:%s",
                                                yyjson_get_str(rmethod), yyjson_get_str(rpath));
                    }
                    yyjson_doc_free(rdoc);
                }
            }
        }
    }

    /* 6. INHERITS: outbound INHERITS edges */
    {
        size_t save = pos;
        pos += (size_t)snprintf(context + pos, sizeof(context) - pos, " INHERITS:");
        int n = append_outbound_names(gbuf, node->id, "INHERITS", context, sizeof(context), &pos);
        if (n == 0) pos = save;
    }

    /* Truncate and null-terminate the context string. The rollback logic
     * above only resets the position counter; stale text beyond pos must
     * not be read by subsequent %s formatters. Also guard against pos
     * exceeding buffer size (snprintf return value can exceed size). */
    if (pos >= sizeof(context)) pos = sizeof(context) - 1;
    context[pos] = '\0';

    /* ── Build merged properties JSON ──
     * Use yyjson to parse existing, re-serialize each value via
     * yyjson_val_write (read-only writer), and append the new
     * embedding_context. We avoid the yyjson mutable writer API
     * (yyjson_mut_write) which was returning NULL in this build. */
    const char *existing = node->properties_json ? node->properties_json : "{}";

    /* Escape context string for JSON (handle " and \). */
    char escaped[CTX_BUF_SZ * 2];
    size_t ep = 0;
    for (const char *cp = context; *cp && ep < sizeof(escaped) - 2; cp++) {
        if (*cp == '"' || *cp == '\\') escaped[ep++] = '\\';
        escaped[ep++] = *cp;
    }
    escaped[ep] = '\0';

    /* Parse existing JSON, re-serialize keys+values while skipping any
     * prior embedding_context field. Each value is written with
     * yyjson_val_write and immediately freed after use. */
    yyjson_doc *exist_doc = yyjson_read(existing, strlen(existing), 0);
    char new_props[PROPS_BUF_SZ];
    size_t np = 0;
    new_props[np++] = '{';
    bool first = true;

    if (exist_doc) {
        yyjson_val *root = yyjson_doc_get_root(exist_doc);
        if (yyjson_is_obj(root)) {
            size_t idx, max;
            yyjson_val *key, *val;
            yyjson_obj_foreach(root, idx, max, key, val) {
                const char *k = yyjson_get_str(key);
                if (!k || strcmp(k, "embedding_context") == 0) continue;
                char *vs = yyjson_val_write(val, 0, NULL);
                if (!vs) continue;
                if (!first) new_props[np++] = ',';
                first = false;
                np += (size_t)snprintf(new_props + np, sizeof(new_props) - np,
                                       "\"%s\":%s", k, vs);
                free(vs);
            }
        }
        yyjson_doc_free(exist_doc);
    }

    /* Append embedding_context (as a JSON object with "text" key). */
    if (!first) new_props[np++] = ',';
    np += (size_t)snprintf(new_props + np, sizeof(new_props) - np,
                           "\"embedding_context\":{\"text\":\"%s\"}}", escaped);

    (void)cbm_gbuf_upsert_node(gbuf, node->label, node->name, node->qualified_name,
                                node->file_path, node->start_line, node->end_line, new_props);
    return 1;
}

/* ── Entry Point ────────────────────────────────────────────────── */

int cbm_pipeline_pass_embedding_context(cbm_pipeline_ctx_t *ctx) {
    if (!ctx || !ctx->gbuf) return CBM_NOT_FOUND;

    cbm_gbuf_t *gbuf = ctx->gbuf;
    int total = 0;

    static const char *labels[] = {"Method", "Function", "Class"};
    for (int li = 0; li < 3; li++) {
        const cbm_gbuf_node_t **nodes = NULL;
        int count = 0;
        if (cbm_gbuf_find_by_label(gbuf, labels[li], &nodes, &count) != 0 || count == 0) continue;

        for (int ni = 0; ni < count; ni++) {
            total += build_context_for_node(gbuf, nodes[ni]);
        }
    }

    {
        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "%d", total);
        cbm_log_info("pass_embedding_context.done", "nodes", tbuf);
    }
    return total;
}
