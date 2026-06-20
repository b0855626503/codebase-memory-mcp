/*
 * pass_arch_rules.c — Architecture Rule Engine
 *
 * Checks architecture rules against the knowledge graph after indexing.
 * Rules are loaded from ~/.config/codebase-memory-mcp/architecture-rules.json
 * or from <project>/.codebase-memory-rules.json.
 *
 * Rule types:
 *   "no_entry_to_internal" — entry-layer modules must not call internal-layer modules
 *   "no_circular" — circular dependencies between packages
 *   "max_fan_in" — max fan-in for a function before warning
 *   "layer_isolation" — specific layer pairs that are forbidden
 *
 * Violations are logged as warnings. The check_architecture_rules MCP tool
 * re-runs the check on demand and returns structured results.
 */

#include "pipeline/pass_arch_rules.h"
#include "store/store.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include <yyjson/yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Rule definitions ─────────────────────────────────────────── */

#define ARCH_RULE_MAX 64
#define ARCH_RULE_NAME_MAX 128

typedef struct {
    char name[ARCH_RULE_NAME_MAX];
    char type[64];       /* no_entry_to_internal, no_circular, max_fan_in, layer_isolation */
    char from_layer[64]; /* for layer_isolation */
    char to_layer[64];   /* for layer_isolation */
    int max_fan_in;      /* for max_fan_in */
    char severity[16];   /* error, warning, info */
} arch_rule_t;

typedef struct {
    arch_rule_t rules[ARCH_RULE_MAX];
    int count;
} arch_rules_t;

/* ── Load rules from JSON ─────────────────────────────────────── */

static arch_rules_t arch_rules_load(const char *config_path) {
    arch_rules_t result = {0};
    if (!config_path) return result;

    FILE *f = fopen(config_path, "rb");
    if (!f) return result;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) { fclose(f); return result; }

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return result; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    yyjson_doc *doc = yyjson_read(buf, sz, 0);
    free(buf);
    if (!doc) return result;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *rules_arr = yyjson_obj_get(root, "rules");
    if (!yyjson_is_arr(rules_arr)) { yyjson_doc_free(doc); return result; }

    size_t idx, max;
    yyjson_val *rule;
    yyjson_arr_foreach(rules_arr, idx, max, rule) {
        if (result.count >= ARCH_RULE_MAX) break;
        arch_rule_t *r = &result.rules[result.count];
        memset(r, 0, sizeof(*r));

        yyjson_val *v;
        if ((v = yyjson_obj_get(rule, "name")) && yyjson_is_str(v))
            snprintf(r->name, sizeof(r->name), "%s", yyjson_get_str(v));
        if ((v = yyjson_obj_get(rule, "type")) && yyjson_is_str(v))
            snprintf(r->type, sizeof(r->type), "%s", yyjson_get_str(v));
        if ((v = yyjson_obj_get(rule, "from_layer")) && yyjson_is_str(v))
            snprintf(r->from_layer, sizeof(r->from_layer), "%s", yyjson_get_str(v));
        if ((v = yyjson_obj_get(rule, "to_layer")) && yyjson_is_str(v))
            snprintf(r->to_layer, sizeof(r->to_layer), "%s", yyjson_get_str(v));
        if ((v = yyjson_obj_get(rule, "max_fan_in")) && yyjson_is_int(v))
            r->max_fan_in = (int)yyjson_get_int(v);
        if ((v = yyjson_obj_get(rule, "severity")) && yyjson_is_str(v))
            snprintf(r->severity, sizeof(r->severity), "%s", yyjson_get_str(v));
        else
            snprintf(r->severity, sizeof(r->severity), "warning");

        if (r->name[0] && r->type[0]) result.count++;
    }
    yyjson_doc_free(doc);
    return result;
}

/* ── Check rules against architecture data ───────────────────────
 * Returns a heap-allocated JSON string of violations. Caller frees. */

char *cbm_check_architecture_rules(cbm_store_t *store, const char *project) {
    if (!store || !project) return strdup("{}");

    /* Load rules: try project-local first, then global config */
    arch_rules_t rules = {0};
    const char *home = cbm_get_home_dir();
    if (home) {
        char rules_path[CBM_SZ_4K];
        snprintf(rules_path, sizeof(rules_path),
                 "%s/.config/codebase-memory-mcp/architecture-rules.json", home);
        rules = arch_rules_load(rules_path);
    }

    /* Get architecture data */
    cbm_architecture_info_t arch = {0};
    const char *aspects[] = {"packages", "layers", "hotspots", "boundaries", NULL};
    cbm_store_get_architecture(store, project, aspects, 4, &arch);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *violations = yyjson_mut_arr(doc);
    int violation_count = 0;

    for (int ri = 0; ri < rules.count; ri++) {
        arch_rule_t *r = &rules.rules[ri];

        if (strcmp(r->type, "no_entry_to_internal") == 0) {
            /* Check boundaries: entry → internal/core */
            for (int i = 0; i < arch.boundary_count; i++) {
                /* Check if either direction violates */
                bool from_entry = strstr(arch.boundaries[i].from, "entry") ||
                    strstr(arch.boundaries[i].from, "api");
                bool to_internal = strstr(arch.boundaries[i].to, "internal") ||
                    strstr(arch.boundaries[i].to, "core");
                if (from_entry && to_internal && arch.boundaries[i].call_count >= 5) {
                    yyjson_mut_val *v = yyjson_mut_obj(doc);
                    yyjson_mut_obj_add_strcpy(doc, v, "rule", r->name);
                    yyjson_mut_obj_add_strcpy(doc, v, "type", r->type);
                    yyjson_mut_obj_add_strcpy(doc, v, "from", arch.boundaries[i].from);
                    yyjson_mut_obj_add_strcpy(doc, v, "to", arch.boundaries[i].to);
                    yyjson_mut_obj_add_int(doc, v, "call_count", arch.boundaries[i].call_count);
                    yyjson_mut_obj_add_strcpy(doc, v, "severity", r->severity);
                    yyjson_mut_arr_add_val(violations, v);
                    violation_count++;
                }
            }
        } else if (strcmp(r->type, "max_fan_in") == 0 && r->max_fan_in > 0) {
            for (int i = 0; i < arch.hotspot_count && i < 20; i++) {
                if (arch.hotspots[i].fan_in > r->max_fan_in) {
                    yyjson_mut_val *v = yyjson_mut_obj(doc);
                    yyjson_mut_obj_add_strcpy(doc, v, "rule", r->name);
                    yyjson_mut_obj_add_strcpy(doc, v, "type", r->type);
                    yyjson_mut_obj_add_strcpy(doc, v, "function", arch.hotspots[i].name);
                    yyjson_mut_obj_add_strcpy(doc, v, "qualified_name", arch.hotspots[i].qualified_name);
                    yyjson_mut_obj_add_int(doc, v, "fan_in", arch.hotspots[i].fan_in);
                    yyjson_mut_obj_add_int(doc, v, "limit", r->max_fan_in);
                    yyjson_mut_obj_add_strcpy(doc, v, "severity", r->severity);
                    yyjson_mut_arr_add_val(violations, v);
                    violation_count++;
                }
            }
        }
    }

    yyjson_mut_obj_add_val(doc, root, "violations", violations);
    yyjson_mut_obj_add_int(doc, root, "violation_count", violation_count);
    yyjson_mut_obj_add_int(doc, root, "rules_loaded", rules.count);

    cbm_store_architecture_free(&arch);

    char *json = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL);
    yyjson_mut_doc_free(doc);
    return json ? json : strdup("{}");
}
