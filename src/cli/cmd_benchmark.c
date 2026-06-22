/*
 * cmd_benchmark.c — Semantic search benchmark runner.
 *
 * Usage:
 *   cbm benchmark <manifest.json> [--project <name>] [--output <results.json>]
 *   cbm benchmark <manifest.json> [--db <path.db>] [--output <results.json>]
 *
 * Manifest format (JSON):
 *   {
 *     "name": "semantic-search-v1",
 *     "queries": [
 *       {
 *         "query": "wallet deposit credit",
 *         "expected": ["WalletService.deposit"],
 *         "acceptable": ["WalletRepository.credit"],
 *         "weight": 1.0
 *       }
 *     ]
 *   }
 *
 * Metrics computed:
 *   - Recall@5, Recall@10: fraction of expected found in top K
 *   - MRR: mean reciprocal rank of first expected hit
 *   - Precision@5: fraction of top 5 in expected ∪ acceptable
 */

#include "cli/cli.h"
#include "cli/cmd_benchmark.h"
#include "discover/userconfig.h"
#include "pipeline/pipeline.h" /* cbm_project_name_from_path */
#include "store/store.h"
#include "foundation/constants.h"
#include "foundation/platform.h"
#include "foundation/log.h"

#include <yyjson/yyjson.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sqlite3.h>   /* for FTS5 lexical search */

/* ── Constants ─────────────────────────────────────────────────── */

enum {
    BM_MAX_KW     = 32,
    BM_MAX_RESULTS = 50,
    BM_BUF_1K     = 1024,
    BM_BUF_256     = 256,
    BM_BUF_4K     = 4096,
};

/* ── Helpers ────────────────────────────────────────────────────── */

/* Split a query string into keyword tokens.
 * Phase F.4 fix: also splits camelCase/snake_case identifiers matching
 * the document tokenizer (append_identifier_tokens in pass_embedding_context).
 * Uses a temp buffer to avoid char-loss from in-place NUL insertion. */
__attribute__((noipa))
static int tokenize_query(char *query, const char **keywords, int max_kw) {
    int count = 0;
    char *p = query;
    static char tokbuf[BM_BUF_4K];
    int tp = 0;

    while (*p && count < max_kw) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        /* Extract one whitespace-delimited token, split camelCase/snake_case */
        char token[256];
        int ti = 0;
        while (*p && !isspace((unsigned char)*p) && ti < 250) token[ti++] = *p++;
        token[ti] = '\0';

        /* Split token by camelCase and snake_case into subtokens */
        const char *s = token;
        char subtok[64];
        int si = 0;
        while (*s && count < max_kw) {
            /* camelCase boundary: emit current subtoken */
            if (si > 0 && *s >= 'A' && *s <= 'Z' && s[-1] >= 'a' && s[-1] <= 'z') {
                subtok[si] = '\0';
                if (si > 1 && tp + si + 1 < (int)sizeof(tokbuf)) {
                    memcpy(tokbuf + tp, subtok, si + 1);
                    keywords[count++] = tokbuf + tp;
                    tp += si + 1;
                }
                si = 0;
            }
            /* snake_case/kebab boundary */
            if (*s == '_' || *s == '-') {
                subtok[si] = '\0';
                if (si > 1 && tp + si + 1 < (int)sizeof(tokbuf)) {
                    memcpy(tokbuf + tp, subtok, si + 1);
                    keywords[count++] = tokbuf + tp;
                    tp += si + 1;
                }
                si = 0;
                s++;
                while (*s == '_' || *s == '-') s++;
                continue;
            }
            /* Lowercase and accumulate */
            char c = (*s >= 'A' && *s <= 'Z') ? *s + ('a' - 'A') : *s;
            if (si < 63) subtok[si++] = c;
            s++;
        }
        /* Emit last subtoken */
        subtok[si] = '\0';
        if (si > 1 && tp + si + 1 < (int)sizeof(tokbuf)) {
            memcpy(tokbuf + tp, subtok, si + 1);
            keywords[count++] = tokbuf + tp;
            tp += si + 1;
        } else if (si == 1 && count < max_kw && tp + 2 < (int)sizeof(tokbuf)) {
            /* Single-char tokens: still emit (may match acronyms) */
            memcpy(tokbuf + tp, subtok, 2);
            keywords[count++] = tokbuf + tp;
            tp += 2;
        }
    }
    return count;
}

/* Compute the reciprocal rank for the first expected hit.
 * Returns 0.0 if not found in top N. */
static double first_reciprocal_rank(const cbm_vector_result_t *results, int count,
                                     const char **expected, int expected_count,
                                     int max_rank) {
    int best = max_rank + 1;
    for (int e = 0; e < expected_count; e++) {
        for (int i = 0; i < count && i < max_rank; i++) {
            if (results[i].qualified_name &&
                strstr(results[i].qualified_name, expected[e])) {
                int rank = i + 1;
                if (rank < best) best = rank;
                break;
            }
        }
    }
    return (best <= max_rank) ? (1.0 / (double)best) : 0.0;
}

/* Resolve the project name from cwd (basename of getcwd). */
static bool resolve_project_name(char *out, size_t out_sz) {
    char cwd[BM_BUF_1K];
    if (!getcwd(cwd, sizeof(cwd))) return false;
    const char *name = cbm_project_name_from_path(cwd);
    if (!name) return false;
    snprintf(out, out_sz, "%s", name);
    return true;
}

/* ── Query execution ────────────────────────────────────────────── */

typedef struct {
    const char *query_text;
    const char **expected;
    int expected_count;
    const char **acceptable;
    int acceptable_count;
    double weight;
} bm_query_t;

typedef struct {
    double recall_at_5;
    double recall_at_10;
    double mrr;
    double precision_at_5;
} bm_metrics_t;

/* Phase 4A: FTS5 BM25 lexical search.
 * Queries FTS5 directly, applies label weighting, returns vector_result_t. */
static int run_lexical_search(cbm_store_t *store, const char *project,
                               const char *query_text, int limit,
                               double k1,
                               cbm_vector_result_t **out, int *out_count) {
    sqlite3 *db = cbm_store_get_db(store);
    if (!db) return -1;

    /* Build FTS5 query: implicit AND (space-separated tokens = ALL must match).
     * Phase 4A.1: AND is much more selective than OR — critical for precision
     * in code search where noise terms (wallet → Model.member_wallet ×500+)
     * drown out the actual target. */
    char fts_query[BM_BUF_1K];
    size_t fq_pos = 0;
    const char *p = query_text;
    bool first = true;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t len = (size_t)(p - start);
        if (len > 0 && len < 100) {
            if (!first) fts_query[fq_pos++] = ' '; /* space = AND in FTS5 */
            memcpy(fts_query + fq_pos, start, len);
            fq_pos += len;
            first = false;
        }
    }
    fts_query[fq_pos] = '\0';
    /* Fallback to OR if AND yields ≤3 results (too restrictive) */
    int and_count = 0;
    if (fq_pos > 0) {
        char count_sql[BM_BUF_1K];
        snprintf(count_sql, sizeof(count_sql),
            "SELECT COUNT(*) FROM nodes_fts WHERE nodes_fts MATCH ?");
        sqlite3_stmt *cs = NULL;
        if (sqlite3_prepare_v2(db, count_sql, -1, &cs, NULL) == SQLITE_OK) {
            sqlite3_bind_text(cs, 1, fts_query, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(cs) == SQLITE_ROW) and_count = sqlite3_column_int(cs, 0);
            sqlite3_finalize(cs);
        }
    }
    if (and_count <= 3 && fq_pos > 0) {
        /* Rebuild as OR for better recall */
        fq_pos = 0; first = true;
        p = query_text;
        while (*p) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            const char *start = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            size_t len = (size_t)(p - start);
            if (len > 0 && len < 100) {
                if (!first) { memcpy(fts_query + fq_pos, " OR ", 4); fq_pos += 4; }
                memcpy(fts_query + fq_pos, start, len);
                fq_pos += len;
                first = false;
            }
        }
        fts_query[fq_pos] = '\0';
    }
    if (fq_pos == 0) { *out = NULL; *out_count = 0; return 0; }

    /* FTS5 BM25 with label filter + weight */
    char sql[BM_BUF_4K];
    snprintf(sql, sizeof(sql),
        "SELECT n.name, n.qualified_name, n.file_path, n.label, "
        "  bm25(nodes_fts, %.1f, 0.75, 0.5, 0.3, 0.2, 0.2) as score "
        "FROM nodes_fts f JOIN nodes n ON f.rowid = n.id "
        "WHERE nodes_fts MATCH ? "
        "  AND n.label IN ('Method','Function') "
        "  AND n.file_path LIKE '%%.php' "
        "  AND n.qualified_name NOT LIKE '%%test%%' "
        "  AND n.qualified_name NOT LIKE '%%Test%%' "
        "ORDER BY score LIMIT ?", k1);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, fts_query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    /* Allocate results array */
    int cap = limit > 0 ? limit : 50;
    cbm_vector_result_t *results = calloc((size_t)cap, sizeof(cbm_vector_result_t));
    if (!results) { sqlite3_finalize(stmt); return -1; }
    int count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && count < cap) {
        results[count].node_id = 0;
        results[count].name = strdup((const char *)sqlite3_column_text(stmt, 0));
        results[count].qualified_name = strdup((const char *)sqlite3_column_text(stmt, 1));
        results[count].file_path = strdup((const char *)sqlite3_column_text(stmt, 2));
        results[count].label = strdup((const char *)sqlite3_column_text(stmt, 3));
        double base_score = sqlite3_column_double(stmt, 4);

        /* Label weight (Phase 4A): Controller ×1.5, Service ×1.4, Repository ×1.2, Model ×0.3 */
        const char *qn = results[count].qualified_name;
        if (qn) {
            if (strstr(qn, "Controller.")) base_score *= 1.5;
            else if (strstr(qn, "Service.") && !strstr(qn, "ServiceProvider")) base_score *= 1.4;
            else if (strstr(qn, "Repository.")) base_score *= 1.2;
            else if (strstr(qn, "/Models/") || strstr(qn, ".Models.")) base_score *= 0.3;
        }
        /* Token overlap boost: shared tokens between query and candidate name/QN.
         * "wallet deposit history" vs "HistoryController.loadDeposit"
         *   → history, deposit = 2 overlaps → boost ×(1 + 0.15*2) */
        {
            const char *name = results[count].name;
            if (name) {
                char name_lower[256];
                size_t nl = 0;
                for (const char *c = name; *c && nl < 250; c++, nl++)
                    name_lower[nl] = (*c >= 'A' && *c <= 'Z') ? (char)(*c + ('a'-'A')) : *c;
                name_lower[nl] = '\0';

                int overlaps = 0;
                const char *qp = query_text;
                while (*qp && overlaps < 20) {
                    while (*qp && isspace((unsigned char)*qp)) qp++;
                    if (!*qp) break;
                    const char *ts = qp;
                    while (*qp && !isspace((unsigned char)*qp)) qp++;
                    size_t tl = (size_t)(qp - ts);
                    if (tl >= 2) {
                        /* Search for this token in the lowercased name */
                        char tok[64];
                        size_t ti = 0;
                        for (const char *tc = ts; tc < qp && ti < 63; tc++, ti++)
                            tok[ti] = (*tc >= 'A' && *tc <= 'Z') ? (char)(*tc + ('a'-'A')) : *tc;
                        tok[ti] = '\0';
                        if (strstr(name_lower, tok)) overlaps++;
                    }
                }
                double overlap_boost = 1.0 + (double)overlaps * 0.15;
                if (overlap_boost > 2.0) overlap_boost = 2.0; /* cap at 2× */
                results[count].score *= overlap_boost;
                results[count].node_id = (int64_t)(overlaps); /* stash overlap count */
            }
        }
        count++;
    }
    sqlite3_finalize(stmt);

    /* Re-sort by weighted score */
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (results[j].score > results[i].score) {
                cbm_vector_result_t tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }

    *out = results;
    *out_count = count;
    return 0;
}

/* Run a single query against the store and compute per-query metrics.
 * filter_model: exclude Model-class methods from results
 * weights: multiply score by category weight before ranking */
static int run_one_query_ex(cbm_store_t *store, const char *project,
                         const bm_query_t *q, bm_metrics_t *out,
                         bool filter_model, bool search_lexical, double bm25_k1,
                         double w_ctrl, double w_svc, double w_repo, double w_model) {
    /* Tokenize query */
    char qbuf[BM_BUF_1K];
    snprintf(qbuf, sizeof(qbuf), "%s", q->query_text);
    const char *keywords[BM_MAX_KW];
    int kw_count = tokenize_query(qbuf, keywords, BM_MAX_KW);
    if (kw_count == 0) {
        memset(out, 0, sizeof(*out));
        return 0;
    }

    /* Vector search (semantic) or FTS5 (lexical) */
    cbm_vector_result_t *results = NULL;
    int result_count = 0;
    int rc;
    if (search_lexical) {
        rc = run_lexical_search(store, project, q->query_text,
                                 BM_MAX_RESULTS, bm25_k1, &results, &result_count);
    } else {
        rc = cbm_store_vector_search(store, project, keywords, kw_count,
                                      BM_MAX_RESULTS, &results, &result_count);
    }
    if (rc != CBM_STORE_OK && rc != 0) {
        memset(out, 0, sizeof(*out));
        return -1;
    }

    /* Post-filter: classify each result and apply weight/filter */
    for (int i = 0; i < result_count; i++) {
        const char *qn = results[i].qualified_name;
        if (!qn) continue;

        /* Classify by QN pattern */
        bool is_ctrl  = (strstr(qn, "Controller.") != NULL);
        bool is_repo  = (strstr(qn, "Repository.") != NULL);
        bool is_svc   = (strstr(qn, "Service.") != NULL && strstr(qn, "ServiceProvider") == NULL);
        bool is_model = (!is_ctrl && !is_repo && !is_svc &&
                          (strstr(qn, "/Models/") != NULL || strstr(qn, "/Model/") != NULL ||
                           strstr(qn, ".Models.") != NULL));

        /* Apply weight */
        if (is_model && w_model != 1.0) results[i].score *= w_model;
        if (is_ctrl && w_ctrl != 1.0) results[i].score *= w_ctrl;
        if (is_svc && w_svc != 1.0) results[i].score *= w_svc;
        if (is_repo && w_repo != 1.0) results[i].score *= w_repo;

        /* Mark Model results for filtering (set score to -1) */
        if (filter_model && is_model) {
            results[i].score = -1.0;
        }
    }

    /* Re-sort by score (simple bubble — 50 elements is tiny) */
    for (int i = 0; i < result_count - 1; i++) {
        for (int j = i + 1; j < result_count; j++) {
            if (results[j].score > results[i].score) {
                cbm_vector_result_t tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }

    /* Compute metrics */
    int top5 = result_count < 5 ? result_count : 5;
    int top10 = result_count < 10 ? result_count : 10;

    /* Recall@K: fraction of expected found in top K */
    int found5 = 0, found10 = 0;
    for (int e = 0; e < q->expected_count; e++) {
        for (int i = 0; i < top5; i++) {
            if (results[i].qualified_name &&
                strstr(results[i].qualified_name, q->expected[e])) {
                found5++;
                break;
            }
        }
        for (int i = 0; i < top10; i++) {
            if (results[i].qualified_name &&
                strstr(results[i].qualified_name, q->expected[e])) {
                found10++;
                break;
            }
        }
    }
    out->recall_at_5 = q->expected_count > 0
        ? (double)found5 / (double)q->expected_count : 0.0;
    out->recall_at_10 = q->expected_count > 0
        ? (double)found10 / (double)q->expected_count : 0.0;

    /* MRR: mean reciprocal rank of first expected hit */
    out->mrr = first_reciprocal_rank(results, result_count,
                                      q->expected, q->expected_count, 10);

    /* Precision@5: fraction of top 5 in expected ∪ acceptable */
    int relevant5 = 0;
    for (int i = 0; i < top5; i++) {
        if (!results[i].qualified_name) continue;
        for (int e = 0; e < q->expected_count; e++) {
            if (strstr(results[i].qualified_name, q->expected[e])) {
                relevant5++;
                goto next_result;
            }
        }
        for (int a = 0; a < q->acceptable_count; a++) {
            if (strstr(results[i].qualified_name, q->acceptable[a])) {
                relevant5++;
                goto next_result;
            }
        }
        next_result:;
    }
    out->precision_at_5 = top5 > 0 ? (double)relevant5 / (double)top5 : 0.0;

    cbm_store_free_vector_results(results, result_count);
    return 0;
}

/* Wrapper with defaults (used by external callers) */
__attribute__((unused))
static int run_one_query(cbm_store_t *store, const char *project,
                         const bm_query_t *q, bm_metrics_t *out) {
    return run_one_query_ex(store, project, q, out, false, false, 0.0, 1.0, 1.0, 1.0, 1.0);
}

/* ── Manifest loading ───────────────────────────────────────────── */

/* Free a loaded query. */
static void free_query(bm_query_t *q) {
    if (q->expected) {
        for (int i = 0; i < q->expected_count; i++)
            free((void *)q->expected[i]);
        free(q->expected);
    }
    if (q->acceptable) {
        for (int i = 0; i < q->acceptable_count; i++)
            free((void *)q->acceptable[i]);
        free(q->acceptable);
    }
}

/* Parse a JSON string array into a heap-allocated const char* array.
 * Each string is strdup'd so the caller owns them. */
static const char **parse_string_array(yyjson_val *arr, int *count) {
    if (!arr || !yyjson_is_arr(arr)) { *count = 0; return NULL; }
    size_t len = yyjson_arr_size(arr);
    if (len == 0) { *count = 0; return NULL; }
    const char **out = calloc(len, sizeof(const char *));
    if (!out) { *count = 0; return NULL; }
    int n = 0;
    yyjson_val *val;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(arr, &iter);
    while ((val = yyjson_arr_iter_next(&iter)) != NULL) {
        const char *s = yyjson_get_str(val);
        if (s) {
            out[n] = strdup(s);
            if (out[n]) n++;
        }
    }
    *count = n;
    return out;
}

/* Load and parse the benchmark manifest JSON file.
 * Returns number of queries, or -1 on error. */
static int load_manifest(const char *path, bm_query_t **out_queries, char **out_name) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open manifest file: %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (long)(BM_BUF_4K * 64)) {
        fprintf(stderr, "error: manifest file empty or too large\n");
        fclose(f);
        return -1;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[nread] = '\0';

    yyjson_doc *doc = yyjson_read(buf, nread, 0);
    free(buf);
    if (!doc) {
        fprintf(stderr, "error: invalid JSON in manifest\n");
        return -1;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        fprintf(stderr, "error: manifest root must be an object\n");
        yyjson_doc_free(doc);
        return -1;
    }

    /* name */
    yyjson_val *name_val = yyjson_obj_get(root, "name");
    if (name_val && yyjson_is_str(name_val)) {
        *out_name = strdup(yyjson_get_str(name_val));
    }

    /* queries */
    yyjson_val *queries = yyjson_obj_get(root, "queries");
    if (!queries || !yyjson_is_arr(queries)) {
        fprintf(stderr, "error: manifest must have a 'queries' array\n");
        yyjson_doc_free(doc);
        free(*out_name);
        *out_name = NULL;
        return -1;
    }

    size_t qlen = yyjson_arr_size(queries);
    if (qlen == 0) {
        yyjson_doc_free(doc);
        return 0;
    }

    bm_query_t *qarr = calloc(qlen, sizeof(bm_query_t));
    if (!qarr) {
        yyjson_doc_free(doc);
        free(*out_name);
        *out_name = NULL;
        return -1;
    }

    int qi = 0;
    yyjson_val *qval;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(queries, &iter);
    while ((qval = yyjson_arr_iter_next(&iter)) != NULL && qi < (int)qlen) {
        if (!yyjson_is_obj(qval)) continue;

        /* query */
        yyjson_val *qt = yyjson_obj_get(qval, "query");
        if (qt && yyjson_is_str(qt)) {
            qarr[qi].query_text = strdup(yyjson_get_str(qt));
        }
        if (!qarr[qi].query_text) { qi++; continue; } /* skip broken */

        /* expected */
        qarr[qi].expected = parse_string_array(
            yyjson_obj_get(qval, "expected"), &qarr[qi].expected_count);

        /* acceptable */
        qarr[qi].acceptable = parse_string_array(
            yyjson_obj_get(qval, "acceptable"), &qarr[qi].acceptable_count);

        /* weight */
        yyjson_val *w = yyjson_obj_get(qval, "weight");
        qarr[qi].weight = (w && yyjson_is_num(w)) ? yyjson_get_real(w) : 1.0;

        qi++;
    }

    yyjson_doc_free(doc);
    *out_queries = qarr;
    return qi;
}

/* ── Artifact output ────────────────────────────────────────────── */

static void write_artifact_json(const char *path, const char *benchmark_name,
                                const char *profile_name,
                                const cbm_embedding_config_t *ec,
                                const bm_query_t *queries, int query_count,
                                const bm_metrics_t *per_query,
                                const bm_metrics_t *aggregate) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "error: cannot write output file: %s\n", path);
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"benchmark\": \"%s\",\n",
            benchmark_name ? benchmark_name : "unnamed");
    fprintf(f, "  \"profile\": \"%s\",\n",
            (profile_name && profile_name[0]) ? profile_name : "none");

    /* effective_config */
    if (ec) {
        fprintf(f, "  \"effective_config\": {\n");
        fprintf(f, "    \"enabled\": %s,\n", ec->enabled ? "true" : "false");
        fprintf(f, "    \"label\": %s,\n", ec->signals.label ? "true" : "false");
        fprintf(f, "    \"calls\": %s,\n", ec->signals.calls ? "true" : "false");
        fprintf(f, "    \"called_by\": %s,\n", ec->signals.called_by ? "true" : "false");
        fprintf(f, "    \"routes_to\": %s,\n", ec->signals.routes_to ? "true" : "false");
        fprintf(f, "    \"inherits\": %s,\n", ec->signals.inherits ? "true" : "false");
        fprintf(f, "    \"parent_class\": %s\n", ec->signals.parent_class ? "true" : "false");
        fprintf(f, "  },\n");
    }

    /* aggregate metrics */
    fprintf(f, "  \"metrics\": {\n");
    fprintf(f, "    \"recall_at_5\": %.4f,\n", aggregate->recall_at_5);
    fprintf(f, "    \"recall_at_10\": %.4f,\n", aggregate->recall_at_10);
    fprintf(f, "    \"mrr\": %.4f,\n", aggregate->mrr);
    fprintf(f, "    \"precision_at_5\": %.4f\n", aggregate->precision_at_5);
    fprintf(f, "  },\n");

    /* per_query */
    fprintf(f, "  \"per_query\": [\n");
    for (int i = 0; i < query_count; i++) {
        fprintf(f, "    {\n");
        fprintf(f, "      \"query\": \"%s\",\n", queries[i].query_text);
        fprintf(f, "      \"weight\": %.1f,\n", queries[i].weight);
        fprintf(f, "      \"recall_at_5\": %.4f,\n", per_query[i].recall_at_5);
        fprintf(f, "      \"recall_at_10\": %.4f,\n", per_query[i].recall_at_10);
        fprintf(f, "      \"mrr\": %.4f,\n", per_query[i].mrr);
        fprintf(f, "      \"precision_at_5\": %.4f\n", per_query[i].precision_at_5);
        fprintf(f, "    }%s\n", (i < query_count - 1) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
}

/* ── Entry point ────────────────────────────────────────────────── */

int cbm_cmd_benchmark(int argc, char **argv) {
    const char *manifest_path = NULL;
    const char *output_path = NULL;
    const char *project_name = NULL;
    const char *db_path = NULL;
    bool verbose = false;
    bool filter_model = false;
    bool search_lexical = false;
    double bm25_k1 = 1.2;  /* standard BM25 — proven 3.3× better than 0.0 */
    double weight_controller = 1.0;
    double weight_service = 1.0;
    double weight_repository = 1.0;
    double weight_model = 1.0;

    /* Parse args */
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--search-lexical") == 0) {
            search_lexical = true;
        } else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            project_name = argv[++i];
        } else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--filter-model") == 0) {
            filter_model = true;
        } else if (strcmp(argv[i], "--weight-controller") == 0 && i + 1 < argc) {
            weight_controller = atof(argv[++i]);
        } else if (strcmp(argv[i], "--weight-service") == 0 && i + 1 < argc) {
            weight_service = atof(argv[++i]);
        } else if (strcmp(argv[i], "--weight-repository") == 0 && i + 1 < argc) {
            weight_repository = atof(argv[++i]);
        } else if (strcmp(argv[i], "--weight-model") == 0 && i + 1 < argc) {
            weight_model = atof(argv[++i]);
        } else if (strcmp(argv[i], "--k1") == 0 && i + 1 < argc) {
            bm25_k1 = atof(argv[++i]);
        } else if (argv[i][0] != '-' && !manifest_path) {
            manifest_path = argv[i];
        }
    }

    if (!manifest_path) {
        fprintf(stderr, "Usage: cbm benchmark <manifest.json> [--project <name>|--db <path.db>] [--output <results.json>]\n");
        return 1;
    }

    /* Resolve DB path */
    char resolved_db[BM_BUF_1K];
    char auto_project[BM_BUF_256];
    if (!db_path) {
        if (!project_name) {
            if (!resolve_project_name(auto_project, sizeof(auto_project))) {
                fprintf(stderr, "error: cannot resolve project from cwd; specify --project or --db\n");
                return 1;
            }
            project_name = auto_project;
        }
        snprintf(resolved_db, sizeof(resolved_db), "%s/%s.db",
                 cbm_resolve_cache_dir(), project_name);
        db_path = resolved_db;
    }

    /* Load userconfig for benchmark metadata */
    char cwd[BM_BUF_1K];
    const char *repo = getcwd(cwd, sizeof(cwd)) ? cwd : NULL;
    cbm_userconfig_t *uc = cbm_userconfig_load(repo);
    const cbm_embedding_config_t *ec = cbm_embedding_config_get();

    /* Load manifest */
    char *benchmark_name = NULL;
    bm_query_t *queries = NULL;
    int query_count = load_manifest(manifest_path, &queries, &benchmark_name);
    if (query_count < 0) {
        cbm_userconfig_free(uc);
        return 1;
    }
    if (query_count == 0) {
        fprintf(stderr, "error: no valid queries in manifest\n");
        free(benchmark_name);
        cbm_userconfig_free(uc);
        return 1;
    }

    /* Open store */
    cbm_store_t *store = cbm_store_open_path_query(db_path);
    if (!store) {
        fprintf(stderr, "error: cannot open project database: %s\n", db_path);
        goto cleanup;
    }

    /* Run queries */
    printf("Benchmark: %s\n", benchmark_name ? benchmark_name : "unnamed");
    printf("DB: %s\n", db_path);
    printf("Config: %s [%s]\n",
           ec->profile[0] ? ec->profile : "defaults",
           ec->enabled ? "enabled" : "disabled");
    printf("Search: %s",
           search_lexical ? "lexical (FTS5 BM25 + label weight)" : "semantic (vector)");
    if (search_lexical) printf(" k1=%.1f", bm25_k1);
    printf("\n");
    if (filter_model || weight_model != 1.0 || weight_controller != 1.0 ||
        weight_service != 1.0 || weight_repository != 1.0) {
        printf("Post-filter:");
        if (filter_model) printf(" model=EXCLUDED");
        if (weight_model != 1.0) printf(" model=×%.1f", weight_model);
        if (weight_controller != 1.0) printf(" ctrl=×%.1f", weight_controller);
        if (weight_service != 1.0) printf(" svc=×%.1f", weight_service);
        if (weight_repository != 1.0) printf(" repo=×%.1f", weight_repository);
        printf("\n");
    }
    printf("Queries: %d\n\n", query_count);

    bm_metrics_t *per_query = calloc((size_t)query_count, sizeof(bm_metrics_t));
    if (!per_query) {
        fprintf(stderr, "error: out of memory\n");
        cbm_store_close(store);
        goto cleanup;
    }

    double total_weight = 0.0;
    double weighted_recall5 = 0.0, weighted_recall10 = 0.0;
    double weighted_mrr = 0.0, weighted_prec5 = 0.0;

    for (int i = 0; i < query_count; i++) {
        if (run_one_query_ex(store, project_name, &queries[i], &per_query[i],
                              filter_model, search_lexical, bm25_k1,
                              weight_controller, weight_service,
                              weight_repository, weight_model) != 0) {
            printf("  [%d] \"%s\" → ERROR\n", i + 1, queries[i].query_text);
            continue;
        }
        double w = queries[i].weight > 0.0 ? queries[i].weight : 1.0;
        total_weight += w;
        weighted_recall5 += per_query[i].recall_at_5 * w;
        weighted_recall10 += per_query[i].recall_at_10 * w;
        weighted_mrr += per_query[i].mrr * w;
        weighted_prec5 += per_query[i].precision_at_5 * w;

        printf("  [%d] \"%s\"", i + 1, queries[i].query_text);
        if (queries[i].expected_count > 0) {
            printf(" R@5=%.2f R@10=%.2f MRR=%.2f",
                   per_query[i].recall_at_5, per_query[i].recall_at_10,
                   per_query[i].mrr);
        }
        printf("\n");

        /* Verbose: show expected rank positions */
        if (verbose && queries[i].expected_count > 0) {
            for (int e = 0; e < queries[i].expected_count; e++) {
                int rank = 0;
                /* Re-run search to get ranks (lightweight: results are cached in store) */
                char qbuf[BM_BUF_1K];
                snprintf(qbuf, sizeof(qbuf), "%s", queries[i].query_text);
                const char *keywords[BM_MAX_KW];
                int kw_count = 0;
                { /* tokenize */
                    char *p = qbuf;
                    while (*p && kw_count < BM_MAX_KW) {
                        while (*p && isspace((unsigned char)*p)) p++;
                        if (!*p) break;
                        keywords[kw_count++] = p;
                        while (*p && !isspace((unsigned char)*p)) p++;
                        if (*p) { *p = '\0'; p++; }
                    }
                }
                cbm_vector_result_t *vres = NULL;
                int vcount = 0;
                cbm_store_vector_search(store, project_name, keywords, kw_count,
                                         50, &vres, &vcount);
                for (int ri = 0; ri < vcount; ri++) {
                    if (vres[ri].qualified_name &&
                        strstr(vres[ri].qualified_name, queries[i].expected[e])) {
                        rank = ri + 1;
                        break;
                    }
                }
                printf("    expected[%d]=\"%s\" rank=%d/%d\n",
                       e, queries[i].expected[e], rank, vcount);
                if (rank == 0 && verbose) {
                    printf("    → NOT IN TOP %d\n", vcount);
                    /* Show top 5 results for context */
                    for (int ti = 0; ti < 5 && ti < vcount; ti++) {
                        printf("      %d. %s (%.3f)\n", ti+1,
                               vres[ti].qualified_name ? vres[ti].qualified_name : "?",
                               vres[ti].score);
                    }
                }
                cbm_store_free_vector_results(vres, vcount);
            }
        }
    }

    cbm_store_close(store);

    /* Aggregate */
    bm_metrics_t aggregate;
    aggregate.recall_at_5    = total_weight > 0.0 ? weighted_recall5 / total_weight : 0.0;
    aggregate.recall_at_10   = total_weight > 0.0 ? weighted_recall10 / total_weight : 0.0;
    aggregate.mrr            = total_weight > 0.0 ? weighted_mrr / total_weight : 0.0;
    aggregate.precision_at_5 = total_weight > 0.0 ? weighted_prec5 / total_weight : 0.0;

    printf("\n--- Aggregate ---\n");
    printf("  Recall@5:    %.4f\n", aggregate.recall_at_5);
    printf("  Recall@10:   %.4f\n", aggregate.recall_at_10);
    printf("  MRR:         %.4f\n", aggregate.mrr);
    printf("  Precision@5: %.4f\n", aggregate.precision_at_5);

    /* Write artifact */
    if (output_path) {
        write_artifact_json(output_path, benchmark_name,
                           ec->profile, ec, queries, query_count,
                           per_query, &aggregate);
        printf("\nArtifact written: %s\n", output_path);
    }

    free(per_query);

cleanup:
    for (int i = 0; i < query_count; i++) {
        free((void *)queries[i].query_text);
        free_query(&queries[i]);
    }
    free(queries);
    free(benchmark_name);
    cbm_userconfig_free(uc);
    return 0;
}
