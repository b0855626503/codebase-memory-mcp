/*
 * pass_embedding_context.c — Embedding Context Builder Pass
 *
 * Enriches Method, Function, and Class nodes with graph context tokens
 * stored in a node property `embedding_context`. The context helps
 * downstream LLM embeddings understand a node's structural role.
 *
 * Context tokens are controlled by .codebase-memory.json:
 *
 *   {
 *     "embedding": {
 *       "context": {
 *         "label": true,
 *         "calls": true,
 *         "called_by": true,
 *         "routes_to": true,
 *         "inherits": true,
 *         "parent_class": false
 *       },
 *       "limits": {"max_names_per_direction": 10}
 *     }
 *   }
 *
 * When no config is present, defaults match the legacy hardcoded behaviour
 * (all signals ON except parent_class, max 10 names per direction).
 * Set "embedding": {"enabled": false} to skip the pass entirely.
 *
 * Signal guide (from discovery-3a3.md Task 2):
 *   Embedding layer: label (what this code is)
 *   Re-rank layer:  calls, called_by, routes_to (how this code is connected)
 *   Excluded:       parent_class (collapses same-class methods)
 */
#include "pipeline/pass_embedding_context.h"
#include "graph_buffer/graph_buffer.h"
#include "pipeline/pipeline.h"
#include "discover/userconfig.h"
#include "foundation/constants.h"
#include "foundation/log.h"

#include <yyjson/yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Context buffer size. */
#define CTX_BUF_SZ CBM_SZ_2K

/* Properties buffer for upsert. */
#define PROPS_BUF_SZ (CBM_SZ_4K + CBM_SZ_2K)

/* ── Helpers ────────────────────────────────────────────────────── */

/* Framework/JS noise names to exclude from embedding context.
 * These are minified JS artifacts, framework helpers, and language
 * builtins that carry zero business signal. Filtering them keeps
 * the embedding context focused on meaningful structural relationships.
 *
 * Maintained in order of observed frequency (1168lot Audit A 2026-06-22):
 *   79.3% JS minified noise (single-char + Web API builtins)
 *   11.9% Framework boilerplate (Laravel helpers)
 *    7.4% Generic helpers
 *    1.4% Business calls (what we WANT to keep)
 */
static bool is_noise_name(const char *name) {
    if (!name || !name[0]) return true;

    /* Single characters — minified JS artifact */
    if (name[1] == '\0') return true;

    /* Two-character names from minified JS */
    if (name[2] == '\0') return true;

    static const char *noise[] = {
        /* JS builtins / Web API */
        "toString", "appendChild", "setAttribute", "removeChild",
        "createElement", "getAttribute", "concat", "match",
        /* Laravel framework helpers */
        "now", "app", "config", "resolve", "request", "response",
        "view", "redirect", "collect", "dispatch", "event",
        "auth", "gate", "back", "abort", "trans", "validator",
        "validatorMake", "middleware", "policy",
        /* Generic helpers (too common to be useful) */
        "apply", "trim", "test", "join", "map", "count", "get",
        "set", "add", "round", "date", "extend",
        /* PHP builtins */
        "is_null", "is_array", "array_merge", "array_keys",
        "array_values", "array_map", "array_filter",
        "str_replace", "strtolower", "strtoupper", "substr",
        "preg_match", "preg_replace", "sprintf",
        NULL};
    for (int i = 0; noise[i]; i++) {
        if (strcmp(name, noise[i]) == 0) return true;
    }
    return false;
}

/* Phase L.5: Repository/ORM boilerplate methods that carry zero
 * domain-specific semantic signal. These are inherited from base
 * Repository/Eloquent classes (find, where, create, update, …).
 * They are CORRECT CALLS edges — we just don't want them in
 * embedding_context because they drown out business-domain signals
 * (UserDeposit, sumWithdraw, getPro, checkPromotion, …).
 *
 * KEPT in graph for trace_path / dead_code / architecture.
 * REMOVED from embedding_context for semantic search quality.
 *
 * 1168lot Post-Sprint-L audit (Jun 2026):
 *   Business signals:         UserDeposit(46), getPro(42), sumWithdraw(20), …
 *   Repository boilerplate:   find(1177), where(1048), findOneWhere(1047), …
 * → Boilerplate outnumbers business 30:1 without this filter.
 */
static bool is_repository_boilerplate(const char *name) {
    static const char *boilerplate[] = {
        /* Eloquent / Repository base API */
        "find", "findOneWhere", "findOneByField", "findWhere",
        "findOrFail", "findOrNew", "first", "firstOrFail",
        "firstOrNew", "firstOrCreate", "findMany", "findById",
        "where", "orWhere", "whereIn", "whereNotIn",
        "whereBetween", "whereNotBetween", "whereNull",
        "whereNotNull", "whereDate", "whereMonth", "whereDay",
        "whereYear", "whereTime", "whereColumn",
        "orderBy", "groupBy", "having", "orderByDesc",
        "create", "update", "delete", "destroy", "save",
        "insert", "insertGetId", "upsert",
        "query", "getQuery", "newQuery", "newModelQuery",
        "paginate", "simplePaginate", "cursorPaginate",
        "chunk", "chunkById", "each", "eachById",
        "lazy", "lazyById", "cursor",
        /* Eloquent relationships */
        "hasOne", "hasMany", "belongsTo", "belongsToMany",
        "hasManyThrough", "morphTo", "morphOne", "morphMany",
        "morphToMany", "morphedByMany",
        /* Generic accessors */
        "getAttribute", "setAttribute", "getAttributes",
        "getOriginal", "getRawOriginal", "getDirty",
        "isDirty", "isClean", "wasChanged",
        /* Model events */
        "boot", "booted", "bootIfNotBooted",
        "getTable", "setTable", "getKeyName", "setKeyName",
        NULL};
    for (int i = 0; boilerplate[i]; i++) {
        if (strcmp(name, boilerplate[i]) == 0) return true;
    }
    return false;
}

/* Split camelCase or snake_case identifier into lowercase tokens.
 * "loadDeposit" -> "load","deposit"
 * "findOneWhere" -> "find","one","where" */
static int append_identifier_tokens(const char *name, char *buf, size_t bufsz, size_t *pos) {
    if (!name || !name[0]) return 0;
    int added = 0;
    const char *p = name;
    char token[64];
    int ti = 0;
    while (*p && added < 20) {
        if (ti > 0 && p[0] >= 'A' && p[0] <= 'Z' && p[-1] >= 'a' && p[-1] <= 'z') {
            token[ti] = '\0';
            if (ti > 1) {
                if (added > 0 && *pos < bufsz) buf[(*pos)++] = ',';
                *pos += (size_t)snprintf(buf + *pos, bufsz - *pos, "%s", token);
                added++;
            }
            ti = 0;
        }
        if (*p == '_' || *p == '-') {
            token[ti] = '\0';
            if (ti > 1) {
                if (added > 0 && *pos < bufsz) buf[(*pos)++] = ',';
                *pos += (size_t)snprintf(buf + *pos, bufsz - *pos, "%s", token);
                added++;
            }
            ti = 0; p++;
            continue;
        }
        char c = (*p >= 'A' && *p <= 'Z') ? *p + ('a' - 'A') : *p;
        if (ti < 63) token[ti++] = c;
        p++;
    }
    token[ti] = '\0';
    if (ti > 1) {
        if (added > 0 && *pos < bufsz) buf[(*pos)++] = ',';
        *pos += (size_t)snprintf(buf + *pos, bufsz - *pos, "%s", token);
        added++;
    }
    return added;
}

/* Extract the short name (last segment) from a qualified name.
 * Returns a pointer into qn or qn itself if no separator found.
 * E.g. "App.Services.WalletService" -> "WalletService"
 *      "App\\Http\\Controllers\\DepositController" -> "DepositController" */
static const char *short_name(const char *qn) {
    if (!qn) return "?";
    const char *dot = strrchr(qn, '.');
    const char *bs = strrchr(qn, '\\');
    const char *sep = dot > bs ? dot : bs;
    return sep ? sep + 1 : qn;
}

/* Append comma_separated_names from outbound edges of given type.
 * Limits to max_names. Returns number of names appended. */
static int append_outbound_names(cbm_gbuf_t *gbuf, int64_t node_id, const char *edge_type,
                                  char *buf, size_t bufsz, size_t *pos, int max_names) {
    const cbm_gbuf_edge_t **edges = NULL;
    int ecount = 0;
    if (cbm_gbuf_find_edges_by_source_type(gbuf, node_id, edge_type, &edges, &ecount) != 0 ||
        ecount == 0) {
        return 0;
    }

    int added = 0;
    for (int i = 0; i < ecount && added < max_names; i++) {
        const cbm_gbuf_node_t *target = cbm_gbuf_find_by_id(gbuf, edges[i]->target_id);
        if (!target || !target->name) continue;
        /* Skip framework/JS noise + Repository boilerplate */
        if (is_noise_name(target->name)) continue;
        if (is_repository_boilerplate(target->name)) continue;
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
 * Limits to max_names. Returns number of names appended. */
static int append_inbound_names(cbm_gbuf_t *gbuf, int64_t node_id, const char *edge_type,
                                 char *buf, size_t bufsz, size_t *pos, bool *has_any,
                                 int max_names) {
    const cbm_gbuf_edge_t **edges = NULL;
    int ecount = 0;
    if (cbm_gbuf_find_edges_by_target_type(gbuf, node_id, edge_type, &edges, &ecount) != 0 ||
        ecount == 0) {
        return 0;
    }

    int added = 0;
    for (int i = 0; i < ecount && added < max_names; i++) {
        const cbm_gbuf_node_t *src = cbm_gbuf_find_by_id(gbuf, edges[i]->source_id);
        if (!src || !src->name) continue;
        /* Skip framework/JS noise + Repository boilerplate */
        if (is_noise_name(src->name)) continue;
        if (is_repository_boilerplate(src->name)) continue;
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

/* ── Config-driven context builder ───────────────────────────────── */

/*
 * Build embedding_context JSON for a single node and upsert it.
 * All signal selection is driven by cbm_embedding_config_get().
 * Returns 1 if the node was updated, 0 on skip/error.
 */
static int build_context_for_node(cbm_gbuf_t *gbuf, const cbm_gbuf_node_t *node) {
    if (!gbuf || !node || !node->id || !node->label || !node->qualified_name) return 0;

    const cbm_embedding_config_t *ec = cbm_embedding_config_get();
    if (!ec || !ec->enabled) return 0;

    int max_names = ec->limits.max_names_per_direction;
    if (max_names <= 0) max_names = 10;

    char context[CTX_BUF_SZ];
    size_t pos = 0;

    /* 1. LABEL prefix: "Method:deposit" / "Function:helper" / "Class:WalletService" */
    if (ec->signals.label) {
        pos += (size_t)snprintf(context + pos, sizeof(context) - pos,
                                "%s:%s", node->label, node->name ? node->name : "?");
    }

    /* 2. CLASS: parent_class (default OFF — collapses same-class methods) */
    if (ec->signals.parent_class) {
        /* parent_class is stored in node properties, not as an edge.
         * Look for "parent_class" key in properties_json. */
        if (node->properties_json) {
            yyjson_doc *pdoc = yyjson_read(node->properties_json,
                                            strlen(node->properties_json), 0);
            if (pdoc) {
                yyjson_val *proot = yyjson_doc_get_root(pdoc);
                yyjson_val *pc = yyjson_obj_get(proot, "parent_class");
                if (pc) {
                    const char *pc_str = yyjson_get_str(pc);
                    if (pc_str && pc_str[0]) {
                        /* If label wasn't emitted, add separator */
                        if (pos > 0) context[pos++] = ' ';
                        pos += (size_t)snprintf(context + pos, sizeof(context) - pos,
                                                "CLASS:%s", short_name(pc_str));
                    }
                }
                yyjson_doc_free(pdoc);
            }
        }
    }

    /* 2b. QN: qualified name tokens — domain vocabulary bridge.
     * Splits "Gametech.Wallet.Http.Controllers.HistoryController.loadDeposit"
     * into "QN:Gametech,Wallet,Http,Controllers,HistoryController,loadDeposit"
     * This gives the embedding model the business-domain words that queries
     * use ("wallet", "history") but which are absent from method names. */
    if (ec->signals.qualified_name && node->qualified_name) {
        size_t save = pos;
        if (pos > 0) context[pos++] = ' ';
        pos += (size_t)snprintf(context + pos, sizeof(context) - pos, "QN:");
        int added = 0;
        const char *qn = node->qualified_name;
        const char *seg = qn;
        while (*seg && added < max_names * 2) {
            const char *dot = strchr(seg, '.');
            size_t slen = dot ? (size_t)(dot - seg) : strlen(seg);
            if (slen > 0 && slen < 64) {
                /* Skip noise segments: project prefix, single-char, file extensions */
                if (slen > 1 && seg[0] != '.' && !(slen == 3 && seg[0] == 's' && seg[1] == 'r' && seg[2] == 'c')) {
                    if (added > 0 && pos < sizeof(context)) context[pos++] = ',';
                    size_t remain = sizeof(context) - pos;
                    pos += (size_t)snprintf(context + pos, remain, "%.*s", (int)slen, seg);
                    added++;
                }
            }
            if (!dot) break;
            seg = dot + 1;
        }
        if (added == 0) pos = save;
    }

    /* 2c. NS: namespace/package structure tokens.
     * From "Gametech.Wallet.Http.Controllers" → "NS:Wallet,Http,Controllers"
     * Skips project prefix, vendor name, and build-path noise. */
    if (ec->signals.namespace_ && node->qualified_name) {
        size_t save = pos;
        if (pos > 0) context[pos++] = ' ';
        pos += (size_t)snprintf(context + pos, sizeof(context) - pos, "NS:");
        int added = 0;
        const char *qn = node->qualified_name;
        /* Skip past project prefix (home-boat-projects-1168lot.packages.) */
        const char *pkg = strstr(qn, ".packages.");
        const char *start = pkg ? pkg + 10 : qn;
        /* Also try app.Services → skip to after "app." */
        if (!pkg) {
            const char *app_dot = strstr(qn, ".app.");
            if (app_dot) start = app_dot + 5;
        }
        const char *seg = start;
        /* Find the class name (second-to-last segment before method) */
        const char *last_dot = strrchr(start, '.');
        const char *class_start = last_dot;
        if (class_start) {
            while (class_start > start && *(class_start - 1) != '.') class_start--;
        }
        while (*seg && added < max_names) {
            const char *dot = strchr(seg, '.');
            size_t slen = dot ? (size_t)(dot - seg) : strlen(seg);
            /* Stop at class name — only emit namespace segments */
            if (seg >= class_start) break;
            if (slen > 1 && slen < 64 &&
                strcmp(seg, "src") != 0 && strcmp(seg, "app") != 0 &&
                strcmp(seg, "packages") != 0) {
                if (added > 0 && pos < sizeof(context)) context[pos++] = ',';
                size_t remain = sizeof(context) - pos;
                pos += (size_t)snprintf(context + pos, remain, "%.*s", (int)slen, seg);
                added++;
            }
            if (!dot) break;
            seg = dot + 1;
        }
        if (added == 0) pos = save;
    }

    /* 2d. PATH: normalized file path tokens (off by default).
     * From "packages/Gametech/Wallet/src/Http/Controllers/HistoryController.php"
     * → "PATH:Wallet,Http,Controllers"
     * Strips project root, src/, .php extension, and common noise dirs. */
    if (ec->signals.file_path && node->file_path) {
        size_t save = pos;
        if (pos > 0) context[pos++] = ' ';
        pos += (size_t)snprintf(context + pos, sizeof(context) - pos, "PATH:");
        int added = 0;
        const char *fp = node->file_path;
        /* Skip to after "packages/" or "app/" */
        const char *pkg_slash = strstr(fp, "packages/");
        const char *app_slash = strstr(fp, "app/");
        const char *start = pkg_slash ? pkg_slash + 9 :
                             (app_slash ? app_slash + 4 : fp);
        const char *seg = start;
        while (*seg && added < max_names) {
            const char *slash = strchr(seg, '/');
            size_t slen = slash ? (size_t)(slash - seg) : strcspn(seg, ".");
            if (slen > 1 && slen < 64 &&
                strcmp(seg, "src") != 0 && strcmp(seg, "Http") != 0 &&
                strcmp(seg, "Controllers") != 0 && strcmp(seg, "Repositories") != 0) {
                if (added > 0 && pos < sizeof(context)) context[pos++] = ',';
                size_t remain = sizeof(context) - pos;
                pos += (size_t)snprintf(context + pos, remain, "%.*s", (int)slen, seg);
                added++;
            }
            if (!slash) break;
            seg = slash + 1;
            if (strstr(seg, ".php") && strcspn(seg, "/") < 6) break; /* stop at filename */
        }
        if (added == 0) pos = save;
    }

    /* 2e. TOKENS: camelCase/snake_case decomposition of identifiers.
     * "loadDeposit" → "load,deposit" — bridges the vocabulary gap between
     * human queries ("wallet deposit") and implementation names (loadDeposit). */
    if (ec->signals.identifier_tokens) {
        size_t save = pos;
        if (pos > 0) context[pos++] = ' ';
        pos += (size_t)snprintf(context + pos, sizeof(context) - pos, "TOKENS:");
        int added = 0;
        if (node->name) {
            added += append_identifier_tokens(node->name, context, sizeof(context), &pos);
            /* Also decompose parent class name (second-last QN segment) */
            if (node->qualified_name) {
                const char *last_dot = strrchr(node->qualified_name, '.');
                if (last_dot && last_dot > node->qualified_name) {
                    /* Back up to find class name: ...HistoryController.loadDeposit */
                    const char *cls_end = last_dot;
                    const char *cls_start = cls_end - 1;
                    while (cls_start > node->qualified_name && *cls_start != '.') cls_start--;
                    if (*cls_start == '.') cls_start++;
                    size_t clen = (size_t)(cls_end - cls_start);
                    if (clen > 1 && clen < 64) {
                        char cls_name[64];
                        memcpy(cls_name, cls_start, clen);
                        cls_name[clen] = '\0';
                        if (added > 0 && pos < sizeof(context)) context[pos++] = ',';
                        (void)append_identifier_tokens(cls_name, context, sizeof(context), &pos);
                    }
                }
            }
        }
        if (added == 0) pos = save;
    }

    /* 3. CALLS: 1-hop outbound CALLS edges */
    if (ec->signals.calls) {
        size_t save = pos;
        if (pos > 0) context[pos++] = ' ';
        pos += (size_t)snprintf(context + pos, sizeof(context) - pos, "CALLS:");
        int n = append_outbound_names(gbuf, node->id, "CALLS", context, sizeof(context),
                                       &pos, max_names);
        if (n == 0) pos = save; /* rollback if nothing added */
    }

    /* 4. CALLED_BY: 1-hop inbound CALLS + ROUTES_TO */
    if (ec->signals.called_by) {
        size_t save = pos;
        if (pos > 0) context[pos++] = ' ';
        pos += (size_t)snprintf(context + pos, sizeof(context) - pos, "CALLED_BY:");
        bool has_any = false;
        (void)append_inbound_names(gbuf, node->id, "CALLS", context, sizeof(context), &pos,
                                    &has_any, max_names);
        (void)append_inbound_names(gbuf, node->id, "ROUTES_TO", context, sizeof(context), &pos,
                                    &has_any, max_names);
        if (!has_any) pos = save;
    }

    /* 5. ROUTE: method + path from first inbound ROUTES_TO route node */
    if (ec->signals.routes_to) {
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
                        if (pos > 0) context[pos++] = ' ';
                        pos += (size_t)snprintf(context + pos, sizeof(context) - pos,
                                                "ROUTE:%s:%s",
                                                yyjson_get_str(rmethod), yyjson_get_str(rpath));
                    }
                    yyjson_doc_free(rdoc);
                }
            }
        }
    }

    /* 6. INHERITS: outbound INHERITS edges */
    if (ec->signals.inherits) {
        size_t save = pos;
        if (pos > 0) context[pos++] = ' ';
        pos += (size_t)snprintf(context + pos, sizeof(context) - pos, "INHERITS:");
        int n = append_outbound_names(gbuf, node->id, "INHERITS", context, sizeof(context),
                                       &pos, max_names);
        if (n == 0) pos = save;
    }

    /* If no signals produced any content, skip this node. */
    if (pos == 0) return 0;

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

    const cbm_embedding_config_t *ec = cbm_embedding_config_get();
    if (!ec || !ec->enabled) {
        cbm_log_info("pass_embedding_context.skip", "reason", "disabled by config");
        return 0;
    }

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
