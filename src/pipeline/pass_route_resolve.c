/*
 * pass_route_resolve.c — Laravel Route → Controller Resolution Pass
 *
 * Post-extraction pass. Scans PHP route files for Laravel route definitions
 * and creates ROUTES_TO edges from Route nodes to controller Method nodes.
 *
 * Controller reference patterns detected:
 *   - Tuple:   Route::post('/p', [C::class, 'method'])
 *   - String:  Route::get('/p',  'Controller@method')
 *   - Resource: Route::resource('photos', PhotoController::class)
 *   - Invokable: Route::get('/p', InvokableController::class)
 *
 * Resolution: controller class FQCN is looked up in the registry by
 * constructing the expected qualified_name from the project+path+class+method.
 */
#include "pipeline/pass_route_resolve.h"
#include "graph_buffer/graph_buffer.h"
#include "pipeline/pipeline.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/str_util.h"
#include "cbm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ──────────────────────────────────────────────────── */

/* Return true if file_path looks like a Laravel route file.
 * Matches: routes/web.php, routes/api.php, packages/X/routes/Y.php */
static bool is_route_file(const char *file_path) {
    if (!file_path) return false;
    /* Match routes/web.php, routes/api.php, etc. */
    const char *last_slash = strrchr(file_path, '/');
    if (!last_slash) return false;
    /* Check if parent directory is named "routes" */
    const char *dir_start = last_slash;
    while (dir_start > file_path && dir_start[-1] != '/') dir_start--;
    if (dir_start == file_path) return false; /* no parent dir */
    /* "routes" is exactly between two slashes */
    size_t dir_len = (size_t)(last_slash - dir_start);
    if (dir_len == 6 && strncmp(dir_start, "routes", 6) == 0) return true;
    return false;
}

/* Find a Method node by controller class name + method name.
 * Searches the graph buffer for nodes whose qualified_name contains
 * the controller class name AND whose bare name matches the method.
 * Returns node_id or -1 if not found or ambiguous. */
static int64_t find_controller_method(cbm_gbuf_t *gbuf, const char *controller_class,
                                       const char *method_name) {
    if (!gbuf || !controller_class || !method_name) return CBM_NOT_FOUND;

    /* Find all nodes with this method name */
    const cbm_gbuf_node_t **nodes = NULL;
    int count = 0;
    if (cbm_gbuf_find_by_name(gbuf, method_name, &nodes, &count) != 0 || count == 0) {
        return CBM_NOT_FOUND;
    }

    /* Filter: keep only nodes whose QN contains the controller class name.
     * Also require Method label (not Function). */
    int64_t found_id = CBM_NOT_FOUND;
    int matches = 0;
    for (int i = 0; i < count; i++) {
        const cbm_gbuf_node_t *n = nodes[i];
        if (!n->qualified_name || !n->label) continue;
        if (strcmp(n->label, "Method") != 0) continue;
        /* Check if QN contains the controller class (e.g. ".HomeController.") */
        if (strstr(n->qualified_name, controller_class)) {
            found_id = n->id;
            matches++;
        }
    }

    /* Ambiguous or not found */
    if (matches != 1) return CBM_NOT_FOUND;
    return found_id;
}

/* Create or find a Route node and return its ID.
 * Route QN format: __route__METHOD__/path (matching pass_route_nodes.c convention) */
static int64_t ensure_route_node(cbm_gbuf_t *gbuf, const char *method, const char *path) {
    char route_qn[CBM_SZ_512];
    snprintf(route_qn, sizeof(route_qn), "__route__%s__%s", method, path);
    char props[CBM_SZ_512];
    snprintf(props, sizeof(props), "{\"method\":\"%s\",\"path\":\"%s\"}", method, path);
    return cbm_gbuf_upsert_node(gbuf, "Route", path, route_qn, "", 0, 0, props);
}

/* ── Route Detection ──────────────────────────────────────────── */

/* Parse a PHP source string to find Route::verb() calls.
 * Uses simple text scanning — the tree-sitter AST walk in the extraction
 * engine hasn't run yet when this pass executes, so we do text-based
 * detection.  This covers the most common Laravel route syntax.
 *
 * For each route found, calls the edge creation helpers.
 *
 * Returns number of ROUTES_TO edges created. */
static int scan_route_definitions(const char *source, const char *file_path,
                                   cbm_gbuf_t *gbuf, const char *project) {
    if (!source || !file_path || !gbuf) return 0;
    int created = 0;
    const char *p = source;

    while (*p) {
        /* Look for "Route::" */
        const char *route = strstr(p, "Route::");
        if (!route) break;

        /* Extract verb: the word after "Route::" */
        const char *verb_start = route + 7; /* skip "Route::" */
        char verb[16] = {0};
        int vi = 0;
        while (*verb_start && ((*verb_start >= 'a' && *verb_start <= 'z') ||
                                (*verb_start >= 'A' && *verb_start <= 'Z'))) {
            if (vi < 15) verb[vi++] = *verb_start;
            verb_start++;
        }
        if (vi == 0) { p = route + 7; continue; }

        /* Skip non-route static calls like Route::group, Route::prefix */
        bool is_route_def = (strcmp(verb, "get") == 0 || strcmp(verb, "post") == 0 ||
                             strcmp(verb, "put") == 0 || strcmp(verb, "patch") == 0 ||
                             strcmp(verb, "delete") == 0 || strcmp(verb, "options") == 0 ||
                             strcmp(verb, "any") == 0 || strcmp(verb, "match") == 0 ||
                             strcmp(verb, "resource") == 0);
        if (!is_route_def) { p = route + 7; continue; }

        /* Convert verb to uppercase for Route node */
        char method_upper[16];
        snprintf(method_upper, sizeof(method_upper), "%s", verb);
        for (char *vp = method_upper; *vp; vp++)
            if (*vp >= 'a' && *vp <= 'z') *vp = (char)(*vp - 'a' + 'A');

        /* Find the opening parenthesis after the verb */
        const char *paren = strchr(verb_start, '(');
        if (!paren) { p = route + 7; continue; }

        /* Extract first argument: path string */
        const char *path_start = NULL;
        const char *path_end = NULL;
        const char *q = paren + 1;
        while (*q && *q == ' ') q++;
        if (*q == '\'' || *q == '"') {
            char quote = *q;
            path_start = q + 1;
            path_end = strchr(path_start, quote);
        }
        if (!path_start || !path_end) { p = route + 7; continue; }
        char path[CBM_SZ_256];
        size_t plen = (size_t)(path_end - path_start);
        if (plen >= sizeof(path)) plen = sizeof(path) - 1;
        memcpy(path, path_start, plen);
        path[plen] = '\0';

        /* For resource routes, expand to RESTful actions */
        bool is_resource = (strcmp(verb, "resource") == 0);

        /* Extract controller reference from arguments */
        /* Look for [Xxx::class, 'method'] or 'Xxx@method' after the path */
        const char *arg2 = path_end + 1;
        while (*arg2 && (*arg2 == ' ' || *arg2 == ',')) arg2++;

        char controller[256] = {0};
        char method[128] = {0};

        if (*arg2 == '[') {
            /* Tuple syntax: [Controller::class, 'method'] */
            const char *class_start = arg2 + 1;
            while (*class_start == ' ') class_start++;
            const char *class_end = strstr(class_start, "::class");
            if (class_end) {
                size_t clen = (size_t)(class_end - class_start);
                if (clen >= sizeof(controller)) clen = sizeof(controller) - 1;
                memcpy(controller, class_start, clen);
                /* Extract method name after comma */
                const char *comma = strchr(class_end, ',');
                if (comma) {
                    const char *m = comma + 1;
                    while (*m == ' ' || *m == '\'') m++;
                    const char *method_end = strchr(m, '\'');
                    if (method_end) {
                        size_t mlen = (size_t)(method_end - m);
                        if (mlen >= sizeof(method)) mlen = sizeof(method) - 1;
                        memcpy(method, m, mlen);
                    }
                }
            }
        } else if (*arg2 == '\'' || *arg2 == '"') {
            /* String syntax: 'Controller@method' */
            char quote = *arg2;
            const char *str_start = arg2 + 1;
            const char *str_end = strchr(str_start, quote);
            if (str_end) {
                const char *at = memchr(str_start, '@', (size_t)(str_end - str_start));
                if (at) {
                    size_t clen = (size_t)(at - str_start);
                    if (clen >= sizeof(controller)) clen = sizeof(controller) - 1;
                    memcpy(controller, str_start, clen);
                    size_t mlen = (size_t)(str_end - at - 1);
                    if (mlen >= sizeof(method)) mlen = sizeof(method) - 1;
                    memcpy(method, at + 1, mlen);
                } else {
                    /* Invokable: just a class name, method = __invoke */
                    size_t clen = (size_t)(str_end - str_start);
                    if (clen >= sizeof(controller)) clen = sizeof(controller) - 1;
                    memcpy(controller, str_start, clen);
                    snprintf(method, sizeof(method), "__invoke");
                }
            }
        }

        /* If no controller detected, skip */
        if (controller[0] == '\0') { p = route + 7; continue; }

        /* ── Create Route nodes and ROUTES_TO edges ── */

        /* For resource routes, expand to multiple RESTful actions */
        if (is_resource) {
            static const char *actions[]  = {"index","store","show","update","destroy",NULL};
            static const char *methods[]  = {"GET","POST","GET","PUT","DELETE",NULL};
            static const char *suffixes[] = {"","","/{id}","/{id}","/{id}",NULL};
            for (int ai = 0; actions[ai]; ai++) {
                char full_path[CBM_SZ_256];
                snprintf(full_path, sizeof(full_path), "%s%s", path, suffixes[ai]);

                /* Find the controller method in the graph */
                int64_t method_id = find_controller_method(gbuf, controller, actions[ai]);
                if (method_id >= 0) {
                    int64_t route_id = ensure_route_node(gbuf, methods[ai], full_path);
                    if (route_id >= 0) {
                        char edge_props[CBM_SZ_256];
                        snprintf(edge_props, sizeof(edge_props),
                                 "{\"controller_class\":\"%s\",\"method_name\":\"%s\","
                                 "\"route_file\":\"%s\"}",
                                 controller, actions[ai], file_path);
                        cbm_gbuf_insert_edge(gbuf, route_id, method_id, "ROUTES_TO", edge_props);
                        created++;
                    }
                }
            }
        } else {
            /* Single route: create one ROUTES_TO edge */
            const char *mname = method[0] ? method : "__invoke";
            int64_t method_id = find_controller_method(gbuf, controller, mname);
            if (method_id >= 0) {
                int64_t route_id = ensure_route_node(gbuf, method_upper, path);
                if (route_id >= 0) {
                    char edge_props[CBM_SZ_256];
                    snprintf(edge_props, sizeof(edge_props),
                             "{\"controller_class\":\"%s\",\"method_name\":\"%s\","
                             "\"route_file\":\"%s\"}",
                             controller, method[0] ? method : "__invoke", file_path);
                    cbm_gbuf_insert_edge(gbuf, route_id, method_id, "ROUTES_TO", edge_props);
                    created++;
                }
            }
        }

        /* Advance past this route definition */
        p = route + 7;
    }
    return created;
}

/* ── Public API ───────────────────────────────────────────────── */

int cbm_pipeline_pass_route_resolve(cbm_pipeline_ctx_t *ctx) {
    if (!ctx || !ctx->gbuf || !ctx->project_name || !ctx->repo_path) return CBM_NOT_FOUND;

    int total_created = 0;
    int files_scanned = 0;

    /* Get all File nodes from the graph buffer (populated by pass_definitions) */
    const cbm_gbuf_node_t **file_nodes = NULL;
    int file_count = 0;
    if (cbm_gbuf_find_by_label(ctx->gbuf, "File", &file_nodes, &file_count) != 0 || file_count == 0) {
        return 0;
    }

    int route_file_count = 0;
    for (int i = 0; i < file_count; i++) {
        const cbm_gbuf_node_t *fn = file_nodes[i];
        if (!fn->file_path) continue;

        /* Only scan PHP route files */
        if (!is_route_file(fn->file_path)) continue;
        route_file_count++;

        /* Build absolute path */
        char abs_path[CBM_SZ_4K];
        snprintf(abs_path, sizeof(abs_path), "%s/%s", ctx->repo_path, fn->file_path);

        /* Read file */
        FILE *f = fopen(abs_path, "r");
        if (!f) continue;
        (void)fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        (void)fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > CBM_SZ_64K) { (void)fclose(f); continue; }
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { (void)fclose(f); continue; }
        size_t nread = fread(buf, 1, (size_t)sz, f);
        (void)fclose(f);
        buf[nread] = '\0';

        /* Scan for route definitions */
        int created = scan_route_definitions(buf, fn->file_path, ctx->gbuf, ctx->project_name);
        free(buf);

        if (created > 0) {
            files_scanned++;
            total_created += created;
        }
    }

    /* file_nodes is owned by gbuf — do NOT free */
    {
        char ebuf[32], fbuf[32], rbuf[32];
        snprintf(ebuf, sizeof(ebuf), "%d", total_created);
        snprintf(fbuf, sizeof(fbuf), "%d", files_scanned);
        snprintf(rbuf, sizeof(rbuf), "%d", route_file_count);
        cbm_log_info("pass_route_resolve.done", "edges", ebuf, "scanned", fbuf,
                     "route_files", rbuf);
    }
    return total_created;
}
