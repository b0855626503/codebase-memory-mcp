/*
 * pass_blade_calls.c — Blade Template → PHP Helper Resolution
 *
 * Scans .blade.php files for function calls in {{ ... }} directives.
 * Extracts bare function names (e.g. showCleanRoutUrl from
 * {{ showCleanRoutUrl($arg) }}) and creates CALLS edges from the
 * Blade file's Module node to the matching PHP Function/Method node.
 */
#include "pipeline/pass_blade_calls.h"
#include "pipeline/pipeline.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/str_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Return true if file_path has a .blade.php extension. */
static bool is_blade_file(const char *fp) {
    if (!fp) return false;
    size_t len = strlen(fp);
    return len > 10 && strcmp(fp + len - 10, ".blade.php") == 0;
}

/* Scan Blade source for {{ function_name(...) }} patterns.
 * For each unique function name found, resolve to a PHP Function/Method
 * node and create a CALLS edge from the blade_module Node to the target. */
static int scan_blade_calls(const char *source, cbm_gbuf_t *gbuf,
                             const cbm_gbuf_node_t *blade_module) {
    if (!source || !gbuf || !blade_module) return 0;
    int created = 0;
    const char *p = source;

    while (*p) {
        /* Find "{{" */
        const char *open = strstr(p, "{{");
        if (!open) break;
        /* Skip whitespace after {{ */
        const char *start = open + 2;
        while (*start == ' ') start++;

        /* Check if this is a function call: identifier followed by '(' */
        const char *ident_end = start;
        while (*ident_end && ((*ident_end >= 'a' && *ident_end <= 'z') ||
                              (*ident_end >= 'A' && *ident_end <= 'Z') ||
                              (*ident_end >= '0' && *ident_end <= '9') ||
                              *ident_end == '_')) {
            ident_end++;
        }
        if (ident_end == start) { p = open + 2; continue; }

        /* Skip whitespace between identifier and '(' */
        const char *paren = ident_end;
        while (*paren == ' ') paren++;
        if (*paren != '(') { p = open + 2; continue; }

        /* Extract function name */
        size_t nlen = (size_t)(ident_end - start);
        if (nlen >= CBM_SZ_256) { p = open + 2; continue; }
        char fname[CBM_SZ_256];
        memcpy(fname, start, nlen);
        fname[nlen] = '\0';

        /* Skip Blade keywords and common directives */
        if (strcmp(fname, "dd") == 0 || strcmp(fname, "dump") == 0 ||
            strcmp(fname, "isset") == 0 || strcmp(fname, "empty") == 0 ||
            strcmp(fname, "auth") == 0 || strcmp(fname, "guest") == 0 ||
            strcmp(fname, "csrf") == 0 || strcmp(fname, "method") == 0 ||
            strcmp(fname, "old") == 0 || strcmp(fname, "session") == 0 ||
            strcmp(fname, "config") == 0 || strcmp(fname, "trans") == 0 ||
            strcmp(fname, "__") == 0 || strcmp(fname, "app") == 0 ||
            strcmp(fname, "request") == 0 || strcmp(fname, "route") == 0 ||
            strcmp(fname, "url") == 0 || strcmp(fname, "asset") == 0 ||
            strcmp(fname, "mix") == 0 || strcmp(fname, "env") == 0 ||
            strcmp(fname, "view") == 0 || strcmp(fname, "include") == 0 ||
            strcmp(fname, "extends") == 0 || strcmp(fname, "section") == 0 ||
            strcmp(fname, "yield") == 0 || strcmp(fname, "stack") == 0 ||
            strcmp(fname, "push") == 0 || strcmp(fname, "prepend") == 0 ||
            strcmp(fname, "end") == 0 || strcmp(fname, "stop") == 0 ||
            strcmp(fname, "show") == 0 || strcmp(fname, "each") == 0 ||
            strcmp(fname, "once") == 0 || strcmp(fname, "json") == 0 ||
            strcmp(fname, "class") == 0 || strcmp(fname, "style") == 0 ||
            strcmp(fname, "checked") == 0 || strcmp(fname, "selected") == 0 ||
            strcmp(fname, "disabled") == 0 || strcmp(fname, "readonly") == 0 ||
            strcmp(fname, "required") == 0 || strlen(fname) <= 1) {
            p = open + 2; continue;
        }

        /* Find the PHP Function node by bare name */
        const cbm_gbuf_node_t **nodes = NULL;
        int count = 0;
        if (cbm_gbuf_find_by_name(gbuf, fname, &nodes, &count) == 0 && count >= 1) {
            for (int ni = 0; ni < count && ni < 10; ni++) {
                const cbm_gbuf_node_t *target = nodes[ni];
                if (target->label &&
                    (strcmp(target->label, "Function") == 0 ||
                     strcmp(target->label, "Method") == 0)) {
                    char props[CBM_SZ_128];
                    snprintf(props, sizeof(props),
                             "{\"callee\":\"%s\",\"strategy\":\"blade_template\",\"confidence\":0.80}",
                             fname);
                    cbm_gbuf_insert_edge(gbuf, blade_module->id, target->id, "CALLS", props);
                    created++;
                }
            }
        }

        p = open + 2;
    }
    return created;
}

int cbm_pipeline_pass_blade_calls(cbm_pipeline_ctx_t *ctx) {
    if (!ctx || !ctx->gbuf || !ctx->repo_path) return CBM_NOT_FOUND;

    int total = 0;
    const cbm_gbuf_node_t **files = NULL;
    int fcount = 0;
    if (cbm_gbuf_find_by_label(ctx->gbuf, "File", &files, &fcount) != 0 || fcount == 0) return 0;

    for (int i = 0; i < fcount; i++) {
        const cbm_gbuf_node_t *fn = files[i];
        if (!fn->file_path || !is_blade_file(fn->file_path)) continue;

        char abs[CBM_SZ_4K];
        snprintf(abs, sizeof(abs), "%s/%s", ctx->repo_path, fn->file_path);
        FILE *f = fopen(abs, "r");
        if (!f) continue;
        (void)fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        (void)fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > CBM_SZ_64K) { (void)fclose(f); continue; }
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { (void)fclose(f); continue; }
        size_t nr = fread(buf, 1, (size_t)sz, f);
        (void)fclose(f);
        buf[nr] = '\0';

        /* Find the Module node for this file (source of CALLS edge).
         * Module QN format: project.dir.path.filename (extension stripped
         * to last dot segment, so .blade.php → .blade). Build via
         * cbm_pipeline_fqn_compute with __file__ sentinel replaced. */
        char *mod_fqn = cbm_pipeline_fqn_compute(ctx->project_name, fn->file_path, "__file__");
        if (!mod_fqn) { free(buf); continue; }
        /* Replace "__file__" with empty → module QN */
        char *sentinel = strstr(mod_fqn, ".__file__");
        if (sentinel) *sentinel = '\0';
        const cbm_gbuf_node_t *mod = cbm_gbuf_find_by_qn(ctx->gbuf, mod_fqn);
        if (mod) {
            int c = scan_blade_calls(buf, ctx->gbuf, mod);
            total += c;
        }
        free(mod_fqn);
        free(buf);
    }
    {
        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "%d", total);
        cbm_log_info("pass_blade_calls.done", "edges", tbuf);
    }
    return total;
}
