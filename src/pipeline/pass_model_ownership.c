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
     * and emit USES_MODEL (Models/ dir) or USES_REPOSITORY edges. */
    int us_es_model_count = 0;
    int us_es_repo_count = 0;

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

            /* Classify: Models/ dir → USES_MODEL, otherwise → USES_REPOSITORY */
            bool is_model = (model_node->file_path &&
                (strstr(model_node->file_path, "/Models/") != NULL ||
                 strstr(model_node->file_path, "\\Models\\") != NULL));
            const char *edge_type = is_model ? "USES_MODEL" : "USES_REPOSITORY";
            const char *via = is_model ? "injection" : "injection";

            char props[CBM_SZ_512];
            snprintf(props, sizeof(props),
                     "{\"model\":\"%s\",\"edge_type\":\"%s\",\"via\":\"%s\"}",
                     model_node->name ? model_node->name : "", edge_type, via);
            cbm_gbuf_insert_edge(ctx->gbuf, source->id, model_node->id,
                                 edge_type, props);
            if (is_model) us_es_model_count++; else us_es_repo_count++;
        }
    }

    free(model_ids);

    /* Step 3: Scan callee_suffix + php_static_resolved CALLS edges.
     * Many controllers call Models via static methods like
     * GameLogProxy::where(...) or SpecialEvent::query()->orderBy(...).
     * The callee text contains the class name before "::".
     * Extract it, match against Models/ dir classes, create USES_MODEL. */
    const cbm_gbuf_edge_t **all_calls = NULL;
    int all_count = 0;
    if (cbm_gbuf_find_edges_by_type(ctx->gbuf, "CALLS", &all_calls, &all_count) == 0) {
        for (int i = 0; i < all_count; i++) {
            const char *props = all_calls[i]->properties_json;
            if (!props) continue;
            /* Check for callee_suffix or php_static_resolved strategies */
            if (!strstr(props, "\"strategy\":\"callee_suffix\"") &&
                !strstr(props, "\"strategy\":\"php_static_resolved\"")) continue;

            /* Extract callee name from JSON */
            const char *ck = strstr(props, "\"callee\":\"");
            if (!ck) continue;
            ck += 10; /* skip "callee":" */
            const char *ce = strchr(ck, '"');
            if (!ce || ce <= ck) continue;

            /* Find "::" in callee text (static method call) */
            const char *dc = ck;
            while (dc < ce && *dc && !(*dc == ':' && *(dc + 1) == ':')) dc++;
            if (dc >= ce || dc == ck) continue; /* no :: prefix or empty class name */

            /* Extract class name before ::, normalize \ to . */
            size_t clen = (size_t)(dc - ck);
            if (clen >= CBM_SZ_256) continue;
            char class_name[CBM_SZ_256];
            memcpy(class_name, ck, clen);
            class_name[clen] = '\0';
            for (char *p = class_name; *p; p++) if (*p == '\\') *p = '.';

            /* Match against gbuf: find class by name (last segment) */
            const char *bare = strrchr(class_name, '.');
            bare = bare ? bare + 1 : class_name;
            const cbm_gbuf_node_t **cands = NULL;
            int nc = 0;
            cbm_gbuf_find_by_name(ctx->gbuf, bare, &cands, &nc);
            for (int ci = 0; ci < nc; ci++) {
                if (!cands[ci]->label || strcmp(cands[ci]->label, "Class") != 0) continue;
                if (!is_eloquent_model_class(ctx->gbuf, cands[ci]->id)) continue;
                /* Emit USES_MODEL from caller to the Model class */
                const cbm_gbuf_node_t *src = cbm_gbuf_find_by_id(ctx->gbuf, all_calls[i]->source_id);
                if (!src || src->file_path == NULL) continue;
                if (src->file_path && strstr(src->file_path, "test") != NULL) continue;
                char mp[CBM_SZ_512];
                snprintf(mp, sizeof(mp), "{\"model\":\"%s\",\"edge_type\":\"USES_MODEL\",\"via\":\"static_call\"}",
                         cands[ci]->name ? cands[ci]->name : "");
                cbm_gbuf_insert_edge(ctx->gbuf, src->id, cands[ci]->id, "USES_MODEL", mp);
                us_es_model_count++;
                break; /* one class per call */
            }
        }
    }

    /* Step 3b: receiver-based Model detection (2-Pass approach).
     * receiver_expr was stored in edge properties during call resolution.
     * Now the gbuf is fully populated — we can reliably look up class names.
     * Catches: SpecialEvent::where() → receiver="SpecialEvent" → USES_MODEL. */
    if (cbm_gbuf_find_edges_by_type(ctx->gbuf, "CALLS", &all_calls, &all_count) == 0) {
        for (int i = 0; i < all_count; i++) {
            const char *props = all_calls[i]->properties_json;
            if (!props) continue;
            const char *rk = strstr(props, "\"receiver\":\"");
            if (!rk) continue;
            rk += 12; /* skip "receiver":" */
            const char *re = strchr(rk, '"');
            if (!re || re <= rk) continue;
            size_t rlen = (size_t)(re - rk);
            if (rlen == 0 || rlen >= CBM_SZ_256) continue;

            /* Skip $variable receivers — only interested in ClassName::method() */
            if (rk[0] == '$' || rk[0] == '(') continue;

            /* Sanitize type name: normalize \ to ., strip * & */
            char rx[CBM_SZ_256];
            memcpy(rx, rk, rlen); rx[rlen] = '\0';
            for (char *p = rx; *p; p++) {
                if (*p == '\\') *p = '.';
            }
            /* Strip trailing * or & (pointer/reference type artifacts) */
            size_t sl = strlen(rx);
            while (sl > 0 && (rx[sl-1] == '*' || rx[sl-1] == '&')) rx[--sl] = '\0';

            /* Look up class in fully-populated gbuf */
            const char *bare = strrchr(rx, '.');
            bare = bare ? bare + 1 : rx;
            const cbm_gbuf_node_t **cands = NULL;
            int nc = 0;
            cbm_gbuf_find_by_name(ctx->gbuf, bare, &cands, &nc);
            for (int ci = 0; ci < nc; ci++) {
                if (!cands[ci]->label || strcmp(cands[ci]->label, "Class") != 0) continue;
                if (!cands[ci]->file_path) continue;
                if (!strstr(cands[ci]->file_path, "/Models/") &&
                    !strstr(cands[ci]->file_path, "\\Models\\")) continue;
                const cbm_gbuf_node_t *src = cbm_gbuf_find_by_id(ctx->gbuf, all_calls[i]->source_id);
                if (!src || !src->file_path) continue;
                if (strstr(src->file_path, "test") != NULL) continue;
                char mp[CBM_SZ_512];
                snprintf(mp, sizeof(mp), "{\"model\":\"%s\",\"edge_type\":\"USES_MODEL\",\"via\":\"receiver\"}",
                         cands[ci]->name ? cands[ci]->name : "");
                cbm_gbuf_insert_edge(ctx->gbuf, src->id, cands[ci]->id, "USES_MODEL", mp);
                us_es_model_count++;
                break;
            }
        }
    }

    /* Step 4: php_static_resolved edges targeting Methods on Model classes.
     * Walk DEFINES_METHOD from the method's parent Class; if parent is Eloquent
     * model, create USES_MODEL from caller to the parent Class. */
    if (cbm_gbuf_find_edges_by_type(ctx->gbuf, "CALLS", &all_calls, &all_count) == 0) {
        for (int i = 0; i < all_count; i++) {
            const char *props = all_calls[i]->properties_json;
            if (!props) continue;
            if (!strstr(props, "\"strategy\":\"php_static_resolved\"")) continue;
            const cbm_gbuf_node_t *tgt = cbm_gbuf_find_by_id(ctx->gbuf, all_calls[i]->target_id);
            if (!tgt || !tgt->label || strcmp(tgt->label, "Method") != 0) continue;
            /* Find parent Class via DEFINES_METHOD */
            const cbm_gbuf_edge_t **dm = NULL;
            int dmc = 0;
            if (cbm_gbuf_find_edges_by_target_type(ctx->gbuf, tgt->id, "DEFINES_METHOD",
                                                    &dm, &dmc) != 0 || dmc == 0) continue;
            const cbm_gbuf_node_t *parent = cbm_gbuf_find_by_id(ctx->gbuf, dm[0]->source_id);
            if (!parent || !is_eloquent_model_class(ctx->gbuf, parent->id)) continue;
            const cbm_gbuf_node_t *src = cbm_gbuf_find_by_id(ctx->gbuf, all_calls[i]->source_id);
            if (!src || src->file_path == NULL) continue;
            if (strstr(src->file_path, "test") != NULL) continue;
            char mp[CBM_SZ_512];
            snprintf(mp, sizeof(mp), "{\"model\":\"%s\",\"edge_type\":\"USES_MODEL\",\"via\":\"static_resolved\"}",
                     parent->name ? parent->name : "");
            cbm_gbuf_insert_edge(ctx->gbuf, src->id, parent->id, "USES_MODEL", mp);
            us_es_model_count++;
        }
    }

    /* Step 5: Repository chain propagation.
     * If X →USES_MODEL→ Repository AND Repository →OWNS_MODEL→ Model,
     * then create X →USES_MODEL→ Model. */
    const cbm_gbuf_edge_t **us_es = NULL;
    int us_count = 0;
    if (cbm_gbuf_find_edges_by_type(ctx->gbuf, "USES_MODEL", &us_es, &us_count) == 0) {
        for (int i = 0; i < us_count; i++) {
            /* Check if target of USES_MODEL has OWNS_MODEL to a Model */
            const cbm_gbuf_edge_t **owns = NULL;
            int oc = 0;
            if (cbm_gbuf_find_edges_by_source_type(ctx->gbuf, us_es[i]->target_id, "OWNS_MODEL",
                                                    &owns, &oc) != 0 || oc == 0) continue;
            for (int oi = 0; oi < oc; oi++) {
                const cbm_gbuf_node_t *model = cbm_gbuf_find_by_id(ctx->gbuf, owns[oi]->target_id);
                if (!model) continue;
                char mp[CBM_SZ_512];
                snprintf(mp, sizeof(mp), "{\"model\":\"%s\",\"edge_type\":\"USES_MODEL\",\"via\":\"repo_chain\"}",
                         model->name ? model->name : "");
                cbm_gbuf_insert_edge(ctx->gbuf, us_es[i]->source_id, model->id, "USES_MODEL", mp);
                us_es_model_count++;
            }
        }
    }

    /* Step 6: Field-based USES_MODEL — R5 approach.
     * Scan all Field nodes with return_type. Resolve type to Class QN.
     * Field on Controller with return_type in Models/ → USES_MODEL.
     * Field on Controller with return_type in Repositories/ → USES_REPOSITORY. */
    const cbm_gbuf_node_t **fields = NULL;
    int field_count = 0;
    if (cbm_gbuf_find_by_label(ctx->gbuf, "Field", &fields, &field_count) == 0) {
        for (int fi = 0; fi < field_count; fi++) {
            const cbm_gbuf_node_t *f = fields[fi];
            if (!f->properties_json) continue;
            const char *rt = strstr(f->properties_json, "\"return_type\":\"");
            if (!rt) continue;
            rt += 15; /* skip "return_type":" */
            const char *rte = strchr(rt, '"');
            if (!rte || rte <= rt) continue;
            size_t rtl = (size_t)(rte - rt);
            if (rtl == 0 || rtl >= CBM_SZ_256) continue;
            char rt_buf[CBM_SZ_256];
            memcpy(rt_buf, rt, rtl); rt_buf[rtl] = '\0';
            /* Normalize and get bare class name */
            for (char *p = rt_buf; *p; p++) if (*p == '\\') *p = '.';
            const char *bare = strrchr(rt_buf, '.');
            bare = bare ? bare + 1 : rt_buf;
            /* Look up class in gbuf */
            const cbm_gbuf_node_t **cands = NULL;
            int nc = 0;
            cbm_gbuf_find_by_name(ctx->gbuf, bare, &cands, &nc);
            for (int ci = 0; ci < nc; ci++) {
                if (!cands[ci]->label || strcmp(cands[ci]->label, "Class") != 0) continue;
                if (!cands[ci]->file_path) continue;
                bool is_model = (strstr(cands[ci]->file_path, "/Models/") != NULL ||
                                strstr(cands[ci]->file_path, "\\Models\\") != NULL);
                bool is_repo = (strstr(cands[ci]->file_path, "/Repositories/") != NULL ||
                               strstr(cands[ci]->file_path, "\\Repositories\\") != NULL);
                if (!is_model && !is_repo) continue;
                /* Get parent class from Field's parent_class property */
                const char *pc = strstr(f->properties_json, "\"parent_class\":\"");
                if (!pc) continue;
                pc += 16; /* skip "parent_class":" */
                const char *pce = strchr(pc, '"');
                if (!pce || pce <= pc) continue;
                /* Look up parent class in gbuf by QN */
                const cbm_gbuf_node_t *parent = NULL;
                for (int pi = 0; pi < class_count; pi++) {
                    if (classes[pi]->qualified_name &&
                        strncmp(classes[pi]->qualified_name, pc, (size_t)(pce - pc)) == 0 &&
                        classes[pi]->qualified_name[(size_t)(pce - pc)] == '\0') {
                        parent = classes[pi]; break;
                    }
                }
                if (!parent || !parent->file_path) continue;
                if (strstr(parent->file_path, "test") != NULL) continue;
                const char *edge_type = is_model ? "USES_MODEL" : "USES_REPOSITORY";
                char mp[CBM_SZ_512];
                snprintf(mp, sizeof(mp), "{\"model\":\"%s\",\"edge_type\":\"%s\",\"via\":\"field_type\"}",
                         cands[ci]->name ? cands[ci]->name : "", edge_type);
                cbm_gbuf_insert_edge(ctx->gbuf, parent->id, cands[ci]->id, edge_type, mp);
                if (is_model) us_es_model_count++; else us_es_repo_count++;
                break;
            }
        }
    }

    char count_buf[32];
    char repo_buf[32];
    snprintf(count_buf, sizeof(count_buf), "%d", us_es_model_count);
    snprintf(repo_buf, sizeof(repo_buf), "%d", us_es_repo_count);
    cbm_log_info("model_ownership", "pass", "us-es_model",
                 "us_es_model", count_buf,
                 "us_es_repo", repo_buf,
                 "done", "yes");
    return 0;
}
