#include "pipeline/pass_model_ownership.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"
#include "cbm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *cbm_extract_model_class(const char *source) {
    if (!source) return NULL;
    const char *func = strstr(source, "function model()");
    if (!func) { func = strstr(source, "function model():"); if (!func) return NULL; }
    const char *open = strchr(func, '{');
    if (!open) return NULL;
    const char *ret = strstr(open, "return");
    if (!ret || ret - open > 500) return NULL;
    ret += 6; while (*ret == ' ') ret++;
    char buf[256]; int ci = 0;
    if (*ret == '\\') {
        ret++;
        while (*ret && *ret != ':' && *ret != ';' && *ret != ' ' && *ret != '\n' && *ret != '\'' && *ret != '"' && ci < (int)sizeof(buf) - 1) {
            buf[ci++] = (*ret == '\\') ? '.' : *ret; ret++;
        }
    } else if (*ret == '\'' || *ret == '"') {
        char q = *ret; ret++;
        while (*ret && *ret != q && *ret != ';' && ci < (int)sizeof(buf) - 1) {
            buf[ci++] = (*ret == '\\') ? '.' : *ret; ret++;
        }
    } else return NULL;
    if (ci == 0) return NULL;
    buf[ci] = '\0';
    return strdup(buf);
}

/* Maximum depth for INHERITS chain traversal (prevent infinite loops). */
#define MAX_INHERITS_DEPTH 10

/* Check if a Class node inherits from an Eloquent Model base class.
 * Walks the INHERITS chain recursively. Also uses file-path heuristic
 * and Eloquent method call patterns as fallbacks. */
static bool is_eloquent_model_class(const cbm_gbuf_t *gbuf, int64_t class_id);
static bool has_eloquent_method_calls(const cbm_gbuf_t *gbuf, int64_t class_id) {
    const cbm_gbuf_edge_t **calls = NULL;
    int count = 0;
    if (cbm_gbuf_find_edges_by_target_type(gbuf, class_id, "CALLS",
                                            &calls, &count) != 0 || count == 0) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        const char *props = calls[i]->properties_json;
        if (!props) continue;
        /* Eloquent static method names — if a class receives CALLS with
         * these callee names, it is almost certainly an Eloquent Model.
         * Non-model classes don't get static calls like where()/find(). */
        if (strstr(props, "\"callee\":\"where\"") ||
            strstr(props, "\"callee\":\"find\"") ||
            strstr(props, "\"callee\":\"create\"") ||
            strstr(props, "\"callee\":\"first\"") ||
            strstr(props, "\"callee\":\"all\"") ||
            strstr(props, "\"callee\":\"save\"") ||
            strstr(props, "\"callee\":\"update\"") ||
            strstr(props, "\"callee\":\"delete\"") ||
            strstr(props, "\"callee\":\"with\"") ||
            strstr(props, "\"callee\":\"select\"") ||
            strstr(props, "\"callee\":\"orderBy\"") ||
            strstr(props, "\"callee\":\"groupBy\"") ||
            strstr(props, "\"callee\":\"count\"") ||
            strstr(props, "\"callee\":\"sum\"") ||
            strstr(props, "\"callee\":\"paginate\"") ||
            strstr(props, "\"callee\":\"chunk\"") ||
            strstr(props, "\"callee\":\"pluck\"") ||
            strstr(props, "\"callee\":\"insert\"") ||
            strstr(props, "\"callee\":\"destroy\"") ||
            strstr(props, "\"callee\":\"truncate\"")) {
            return true;
        }
    }
    return false;
}

static bool inherits_chain_has_eloquent(const cbm_gbuf_t *gbuf, int64_t class_id, int depth) {
    if (depth >= MAX_INHERITS_DEPTH) return false;
    const cbm_gbuf_edge_t **inherits = NULL;
    int inh_count = 0;
    if (cbm_gbuf_find_edges_by_source_type(gbuf, class_id, "INHERITS",
                                            &inherits, &inh_count) != 0 || inh_count == 0) {
        return false;
    }
    for (int i = 0; i < inh_count; i++) {
        const cbm_gbuf_node_t *parent = cbm_gbuf_find_by_id(gbuf, inherits[i]->target_id);
        if (!parent || !parent->qualified_name) continue;
        const char *qn = parent->qualified_name;
        /* Direct Eloquent ancestor patterns */
        if (strstr(qn, "Eloquent") != NULL ||
            strstr(qn, "Illuminate.Database") != NULL ||
            strstr(qn, "Database.Eloquent") != NULL ||
            strstr(qn, "Jenssegers.Mongodb") != NULL ||
            strstr(qn, "MongoDB.Laravel") != NULL) {
            return true;
        }
        /* Recurse up the chain: check if this parent inherits from Eloquent */
        if (inherits_chain_has_eloquent(gbuf, parent->id, depth + 1)) {
            return true;
        }
    }
    return false;
}

static bool is_eloquent_model_class(const cbm_gbuf_t *gbuf, int64_t class_id) {
    /* Primary check: walk the INHERITS chain */
    if (inherits_chain_has_eloquent(gbuf, class_id, 0)) {
        return true;
    }

    /* Fallback 1: file-path / namespace heuristic.
     * Eloquent models are almost always in a Models directory/namespace. */
    const cbm_gbuf_node_t *cls = cbm_gbuf_find_by_id(gbuf, class_id);
    if (!cls) return false;

    if (cls->file_path && (strstr(cls->file_path, "/Models/") != NULL ||
                           strstr(cls->file_path, "\\Models\\") != NULL)) {
        return true;
    }
    if (cls->qualified_name && (strstr(cls->qualified_name, ".Models.") != NULL ||
                                 strstr(cls->qualified_name, "\\Models\\") != NULL)) {
        return true;
    }

    /* Fallback 2: Eloquent method call patterns.
     * If a class receives CALLS with Eloquent static method names
     * (where, find, create, first, etc.), it IS an Eloquent model. */
    if (has_eloquent_method_calls(gbuf, class_id)) {
        return true;
    }

    return false;
}

int cbm_pipeline_pass_model_ownership(cbm_pipeline_ctx_t *ctx,
                                       const cbm_file_info_t *files, int file_count) {
    (void)files; (void)file_count;
    if (!ctx || !ctx->gbuf) return 0;

    /* Step 1: Find all Class nodes that inherit from Eloquent Model.
     * We collect their IDs in a dynamic array for O(1) membership test. */
    const cbm_gbuf_node_t **classes = NULL;
    int class_count = 0;
    if (cbm_gbuf_find_by_label(ctx->gbuf, "Class", &classes, &class_count) != 0 || class_count == 0) {
        cbm_log_info("model_ownership", "pass", "us-es_model",
                     "classes", "0", "skipped", "no_classes");
        return 0;
    }

    /* Build a set of Eloquent model class IDs.
     * Use a simple int64_t array with linear scan — typically < 500 models. */
    int64_t *model_ids = (int64_t *)calloc(class_count, sizeof(int64_t));
    if (!model_ids) {
        cbm_log_info("model_ownership", "pass", "us-es_model",
                     "error", "oom_model_ids");
        return 0;
    }
    int model_count = 0;

    for (int i = 0; i < class_count; i++) {
        if (is_eloquent_model_class(ctx->gbuf, classes[i]->id)) {
            model_ids[model_count++] = classes[i]->id;
        }
    }

    if (model_count == 0) {
        free(model_ids);
        cbm_log_info("model_ownership", "pass", "us-es_model",
                     "skipped", "no_eloquent_models");
        return 0;
    }

    cbm_log_info("model_ownership", "pass", "us-es_model",
                 "found_eloquent", "yes");

    /* Step 2: For each Eloquent Model class, find CALLS edges targeting it
     * and emit USES_MODEL edges from the caller to the model. */
    int us_es_model_count = 0;

    for (int m = 0; m < model_count; m++) {
        const cbm_gbuf_edge_t **calls = NULL;
        int calls_count = 0;
        if (cbm_gbuf_find_edges_by_target_type(ctx->gbuf, model_ids[m], "CALLS",
                                                &calls, &calls_count) != 0) {
            continue;
        }

        const cbm_gbuf_node_t *model_node = cbm_gbuf_find_by_id(ctx->gbuf, model_ids[m]);
        if (!model_node) continue;

        for (int c = 0; c < calls_count; c++) {
            const cbm_gbuf_node_t *source =
                cbm_gbuf_find_by_id(ctx->gbuf, calls[c]->source_id);
            if (!source) continue;

            /* Skip edges from test files */
            if (source->file_path && strstr(source->file_path, "test") != NULL) continue;

            /* Emit USES_MODEL edge with the CALLS confidence and strategy */
            char props[CBM_SZ_512];
            snprintf(props, sizeof(props),
                     "{\"model\":\"%s\",\"edge_type\":\"USES_MODEL\"}",
                     model_node->name ? model_node->name : "");
            cbm_gbuf_insert_edge(ctx->gbuf, source->id, model_node->id,
                                 "USES_MODEL", props);
            us_es_model_count++;
        }
    }

    free(model_ids);

    char count_buf[32];
    snprintf(count_buf, sizeof(count_buf), "%d", us_es_model_count);
    cbm_log_info("model_ownership", "pass", "us-es_model",
                 "us_es_model_edges", count_buf,
                 "done", "yes");
    return 0;
}
