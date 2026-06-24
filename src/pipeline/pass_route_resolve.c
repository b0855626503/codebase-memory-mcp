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
 * Sprint M: expanded to match:
 *   routes/web.php          (parent dir = "routes")
 *   packages/X/Routes/Y.php (parent dir = "Routes", case-insensitive)
 *   packages/X/Http/routes.php (file named "routes.php" or "route.php")
 *   packages/X/src/routes.php  (file named "routes.php")
 */
static bool is_route_file(const char *file_path) {
    if (!file_path) return false;

    /* Check 1: Parent directory named "routes" (case-insensitive) */
    const char *last_slash = strrchr(file_path, '/');
    if (last_slash) {
        const char *dir_start = last_slash;
        while (dir_start > file_path && dir_start[-1] != '/') dir_start--;
        size_t dir_len = (size_t)(last_slash - dir_start);
        if (dir_len == 6 &&
            (dir_start[0] == 'r' || dir_start[0] == 'R') &&
            (dir_start[1] == 'o' || dir_start[1] == 'O') &&
            (dir_start[2] == 'u' || dir_start[2] == 'U') &&
            (dir_start[3] == 't' || dir_start[3] == 'T') &&
            (dir_start[4] == 'e' || dir_start[4] == 'E') &&
            (dir_start[5] == 's' || dir_start[5] == 'S')) return true;
    }

    /* Check 2: File basename contains "route" (case-insensitive)
     * Matches: routes.php, route.php, shop-routes.php, admin-routes.php,
     *          routesub.php, routes_addon.php */
    const char *basename = last_slash ? last_slash + 1 : file_path;
    const char *route_in_name = strstr(basename, "route");
    if (!route_in_name) route_in_name = strstr(basename, "Route");
    if (!route_in_name) route_in_name = strstr(basename, "ROUTE");
    if (route_in_name) return true;

    return false;
}

/* Normalize a PHP controller class name for QN matching.
 * Converts \ to . and strips leading \ so that:
 *   'Gametech\Admin\Http\Controllers\WebhookController'
 * becomes:
 *   'Gametech.Admin.Http.Controllers.WebhookController'
 * The graph QN uses dot-separated paths, while PHP uses backslash namespaces. */
static void normalize_controller_qn(const char *src, char *dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) return;
    const char *s = src;
    /* Strip leading backslash (e.g. '\App\Http\...') */
    if (*s == '\\') s++;
    size_t di = 0;
    while (*s && di < dst_size - 1) {
        if (*s == '\\') {
            dst[di++] = '.';
        } else {
            dst[di++] = *s;
        }
        s++;
    }
    dst[di] = '\0';
}

/* Check if all dot-separated segments of needle appear in haystack in order.
 * Allows gaps between segments (handles .src. / .app. directory skew).
 * e.g. "Gametech.Admin.Http.Controllers.WebhookController"
 *   matches "Gametech.Admin.src.Http.Controllers.WebhookController.WebhookController.index"
 * Each segment must match at a dot-boundary in haystack. */
static bool segments_match_ordered(const char *needle, const char *haystack) {
    if (!needle || !haystack) return false;
    const char *n = needle;
    while (*n) {
        const char *dot = strchr(n, '.');
        size_t seg_len = dot ? (size_t)(dot - n) : strlen(n);
        bool found = false;
        const char *h = haystack;
        while (*h) {
            if (h == haystack || h[-1] == '.') {
                if (strncmp(h, n, seg_len) == 0) {
                    char after = h[seg_len];
                    if (after == '.' || after == '\0') {
                        haystack = h + seg_len;
                        found = true;
                        break;
                    }
                }
            }
            h++;
        }
        if (!found) return false;
        if (!dot) break;
        n = dot + 1;
    }
    return true;
}

/* Find a Method node by controller class name + method name.
 * Searches the graph buffer for nodes whose qualified_name contains
 * the controller class name (with \ normalized to .) AND whose bare name
 * matches the method.
 * Returns node_id or -1 if not found or ambiguous. */
static int64_t find_controller_method(cbm_gbuf_t *gbuf, const char *controller_class,
                                       const char *method_name) {
    if (!gbuf || !controller_class || !method_name) return CBM_NOT_FOUND;

    const cbm_gbuf_node_t **nodes = NULL;
    int count = 0;
    if (cbm_gbuf_find_by_name(gbuf, method_name, &nodes, &count) != 0 || count == 0) {
        return CBM_NOT_FOUND;
    }

    char normalized[256];
    normalize_controller_qn(controller_class, normalized, sizeof(normalized));

    int64_t found_id = CBM_NOT_FOUND;
    int matches = 0;
    for (int i = 0; i < count; i++) {
        const cbm_gbuf_node_t *n = nodes[i];
        if (!n->qualified_name || !n->label) continue;
        if (strcmp(n->label, "Method") != 0) continue;
        if (segments_match_ordered(normalized, n->qualified_name)) {
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

#define MAX_GROUP_DEPTH 8

/* Accumulated context for one group level */
typedef struct {
    char domain[CBM_SZ_128];
    char prefix[CBM_SZ_128];
    char middleware[CBM_SZ_512];
    char name[CBM_SZ_128];
    char controller[CBM_SZ_256];
    bool has_domain;
} route_group_ctx_t;

/* One entry on the group scope stack */
typedef struct {
    route_group_ctx_t ctx;
    int  brace_depth;    /* brace depth just BEFORE group's opening '{' */
} group_scope_t;

/* Clear a context to defaults */
static void clear_pending(route_group_ctx_t *c) {
    memset(c, 0, sizeof(*c));
}

/* Count { and } in [start, end) and update brace_depth.
 * Auto-pops group scopes when brace_depth drops to or below
 * a group's recorded depth. */
static void track_braces(const char *start, const char *end,
                         int *brace_depth,
                         group_scope_t *group_stack, int *group_depth) {
    for (const char *q = start; q < end; q++) {
        if (*q == '{') {
            (*brace_depth)++;
        } else if (*q == '}') {
            (*brace_depth)--;
            /* A '}' at or below a group's recorded entry depth closes that group */
            while (*group_depth > 0 &&
                   *brace_depth <= group_stack[*group_depth - 1].brace_depth) {
                (*group_depth)--;
            }
        }
    }
}

/* Push a group scope onto the stack.
 * Returns true if pushed, false if stack is full. */
static bool push_group_scope(const route_group_ctx_t *ctx, int opening_brace_depth,
                             group_scope_t *group_stack, int *group_depth) {
    if (*group_depth >= MAX_GROUP_DEPTH) return false;
    group_stack[*group_depth].ctx = *ctx;
    group_stack[*group_depth].brace_depth = opening_brace_depth - 1;
    (*group_depth)++;
    return true;
}

/* Find the opening '{' of a closure after a '('.
 * Handles: function() {, function(Request $r) {, function() use($var) {, fn() => {
 * Returns pointer to '{' or NULL if not found.
 * Sets *no_brace_scope to true for fn() => expr (single-expression, no { } body). */
static const char *find_closure_brace(const char *paren_open, bool *no_brace_scope) {
    *no_brace_scope = false;
    const char *q = paren_open + 1;
    while (*q && *q == ' ') q++;

    /* Skip past 'function' keyword */
    if (strncmp(q, "function", 8) == 0) {
        q += 8;
        /* Skip past parameters: ( ... ) */
        while (*q && *q != '(' && *q != '{') q++;
        if (*q == '(') {
            int p_depth = 1;
            q++;
            while (*q && p_depth > 0) {
                if (*q == '(') p_depth++;
                else if (*q == ')') p_depth--;
                q++;
            }
        }
        /* Skip past 'use' clause: use ($var) */
        while (*q && *q == ' ') q++;
        if (strncmp(q, "use", 3) == 0 && (q[3] == ' ' || q[3] == '(')) {
            q += 3;
            while (*q && *q != '(' && *q != '{') q++;
            if (*q == '(') {
                int u_depth = 1;
                q++;
                while (*q && u_depth > 0) {
                    if (*q == '(') u_depth++;
                    else if (*q == ')') u_depth--;
                    q++;
                }
            }
        }
        /* Now find '{' */
        while (*q && *q == ' ') q++;
        if (*q == '{') return q;
        return NULL;
    }

    /* Arrow function: fn() => ... */
    if (strncmp(q, "fn", 2) == 0 && (q[2] == '(' || q[2] == ' ')) {
        /* Skip to => arrow */
        const char *arrow = strstr(q, "=>");
        if (!arrow) return NULL;
        q = arrow + 2;
        while (*q && *q == ' ') q++;
        if (*q == '{') return q;
        /* Single-expression arrow fn — no brace scope */
        *no_brace_scope = true;
        return NULL;
    }

    /* Variable callback: $cb — no scope to track */
    return NULL;
}

/* Merge all active group scopes + pending into an effective context.
 * Prefix paths concatenate with '/'. Domain/controller: innermost non-empty wins. */
static route_group_ctx_t resolve_effective_context(
    const group_scope_t *group_stack, int group_depth,
    const route_group_ctx_t *pending) {
    route_group_ctx_t eff;
    memset(&eff, 0, sizeof(eff));

    /* Layer 1: Merge all pushed group scopes (bottom to top) */
    for (int i = 0; i < group_depth; i++) {
        const route_group_ctx_t *g = &group_stack[i].ctx;
        if (g->has_domain) {
            memcpy(eff.domain, g->domain, CBM_SZ_128);
            eff.has_domain = true;
        }
        if (g->prefix[0]) {
            if (eff.prefix[0]) {
                size_t el = strlen(eff.prefix);
                snprintf(eff.prefix + el, sizeof(eff.prefix) - el, "/%s", g->prefix);
            } else {
                snprintf(eff.prefix, sizeof(eff.prefix), "%s", g->prefix);
            }
        }
        if (g->middleware[0]) {
            if (eff.middleware[0]) {
                size_t el = strlen(eff.middleware);
                snprintf(eff.middleware + el, sizeof(eff.middleware) - el, ",%s", g->middleware);
            } else {
                snprintf(eff.middleware, sizeof(eff.middleware), "%s", g->middleware);
            }
        }
        if (g->name[0]) {
            size_t el = strlen(eff.name);
            snprintf(eff.name + el, sizeof(eff.name) - el, "%s", g->name);
        }
        if (g->controller[0]) {
            memcpy(eff.controller, g->controller, CBM_SZ_256);
        }
    }

    /* Layer 2: Overlay pending context (highest priority) */
    if (pending->has_domain) {
        memcpy(eff.domain, pending->domain, CBM_SZ_128);
        eff.has_domain = true;
    }
    if (pending->prefix[0]) {
        if (eff.prefix[0]) {
            size_t el = strlen(eff.prefix);
            snprintf(eff.prefix + el, sizeof(eff.prefix) - el, "/%s", pending->prefix);
        } else {
            snprintf(eff.prefix, sizeof(eff.prefix), "%s", pending->prefix);
        }
    }
    if (pending->middleware[0]) {
        if (eff.middleware[0]) {
            size_t el = strlen(eff.middleware);
            snprintf(eff.middleware + el, sizeof(eff.middleware) - el, ",%s", pending->middleware);
        } else {
            snprintf(eff.middleware, sizeof(eff.middleware), "%s", pending->middleware);
        }
    }
    if (pending->name[0]) {
        size_t el = strlen(eff.name);
        snprintf(eff.name + el, sizeof(eff.name) - el, "%s", pending->name);
    }
    if (pending->controller[0]) {
        memcpy(eff.controller, pending->controller, CBM_SZ_256);
    }

    return eff;
}

/* Build full path by prepending effective prefix context to the route path.
 * Also prepends the legacy prefix_stack for backward compatibility. */
static void build_full_path(char *out, size_t out_size,
                            const route_group_ctx_t *eff,
                            const char prefix_stack[][CBM_SZ_128], int prefix_depth,
                            const char *path, const char *suffix) {
    size_t fp = 0;

    /* First: effective group prefix (e.g. "api/v2/member") */
    if (eff->prefix[0]) {
        size_t pl = strlen(eff->prefix);
        if (fp + pl + 1 < out_size) {
            memcpy(out + fp, eff->prefix, pl);
            fp += pl;
        }
    }

    /* Then: legacy prefix_stack prefixes */
    for (int pi = 0; pi < prefix_depth; pi++) {
        if (fp + strlen(prefix_stack[pi]) + 1 < out_size) {
            if (fp > 0) out[fp++] = '/';
            size_t pl = strlen(prefix_stack[pi]);
            memcpy(out + fp, prefix_stack[pi], pl);
            fp += pl;
        }
    }

    /* Append the route path */
    if (fp > 0 && path[0] != '/') out[fp++] = '/';
    if (suffix && suffix[0]) {
        snprintf(out + fp, out_size - fp, "%s%s", path, suffix);
    } else {
        snprintf(out + fp, out_size - fp, "%s", path);
    }
}

/* Extract a quoted string value from position p.
 * Returns pointer after the closing quote, or p if not a string.
 * Stores the extracted value in out (truncated to out_size). */
static const char *extract_quoted_string(const char *p, char *out, size_t out_size) {
    while (*p == ' ') p++;
    if (*p != '\'' && *p != '"') return NULL;
    char quote = *p;
    const char *start = p + 1;
    const char *end = strchr(start, quote);
    if (!end) return NULL;
    size_t len = (size_t)(end - start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return end + 1;
}

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

    /* Prefix stack for Route::prefix()->group() nesting (legacy — kept for backward compat).
     * M3-A1: group_stack with brace-depth tracking runs alongside. */
    char prefix_stack[4][CBM_SZ_128];
    int prefix_depth = 0;
    #define PREFIX_STACK_PUSH(s) do { \
        if (prefix_depth < 4) snprintf(prefix_stack[prefix_depth++], CBM_SZ_128, "%s", s); \
    } while(0)
    #define PREFIX_STACK_POP() do { if (prefix_depth > 0) prefix_depth--; } while(0)

    /* M3-A1: Group scope stack with brace-depth-aware push/pop */
    group_scope_t group_stack[MAX_GROUP_DEPTH];
    int group_depth = 0;
    route_group_ctx_t pending;
    clear_pending(&pending);
    int brace_depth = 0;
    bool in_chain = false;

    while (*p) {
        /* Dual scanning: find nearest of "Route::" or "->" */
        const char *route_hit = strstr(p, "Route::");
        const char *chain_hit = strstr(p, "->");

        const char *next = NULL;
        bool is_route = false;

        if (route_hit && chain_hit) {
            if (route_hit < chain_hit) { next = route_hit; is_route = true; }
            else                       { next = chain_hit; is_route = false; }
        } else if (route_hit) { next = route_hit; is_route = true; }
        else if (chain_hit)   { next = chain_hit; is_route = false; }
        else break;

        /* Track braces in the skipped region */
        track_braces(p, next, &brace_depth, group_stack, &group_depth);

        if (!is_route) {
            /* ── Handle ->chained call ── */
            /* Only process -> if we're in a Route chain (Route::xxx()->...).
             * Bare -> outside a chain (e.g. $obj->save()) should be skipped. */
            if (!in_chain) { p = next + 2; continue; }

            const char *arrow = next; /* points to "->" */
            const char *verb_start = arrow + 2;
            char verb[16] = {0};
            int vi = 0;
            while (*verb_start && ((*verb_start >= 'a' && *verb_start <= 'z') ||
                                    (*verb_start >= 'A' && *verb_start <= 'Z'))) {
                if (vi < 15) verb[vi++] = *verb_start;
                verb_start++;
            }
            if (vi == 0) { p = arrow + 2; continue; }

            if (strcmp(verb, "group") == 0) {
                /* ->group(function() { ... }) — end of a modifier chain */
                const char *paren = strchr(verb_start, '(');
                if (paren) {
                    bool no_brace = false;
                    const char *open_brace = find_closure_brace(paren, &no_brace);
                    if (open_brace) {
                        push_group_scope(&pending, brace_depth + 1,
                                         group_stack, &group_depth);
                    } else if (no_brace) {
                        /* fn() => expr — apply pending directly to next route,
                         * then clear after one use (handled by route verb case). */
                    }
                }
                clear_pending(&pending);
                in_chain = false;
                p = verb_start;
                continue;

            } else if (strcmp(verb, "prefix") == 0 || strcmp(verb, "domain") == 0 ||
                       strcmp(verb, "middleware") == 0 || strcmp(verb, "name") == 0 ||
                       strcmp(verb, "controller") == 0) {
                /* Accumulate chained modifier into pending context.
                 * Set in_chain=true even if arg is a variable (can't parse). */
                const char *paren = strchr(verb_start, '(');
                in_chain = true;
                if (!paren) { p = arrow + 2; continue; }
                const char *q = paren + 1;
                while (*q && *q == ' ') q++;

                if (strcmp(verb, "prefix") == 0) {
                    const char *after = extract_quoted_string(q, pending.prefix, CBM_SZ_128);
                    if (after) { p = after; continue; }
                } else if (strcmp(verb, "domain") == 0) {
                    const char *after = extract_quoted_string(q, pending.domain, CBM_SZ_128);
                    if (after) { pending.has_domain = true; p = after; continue; }
                } else if (strcmp(verb, "middleware") == 0) {
                    if (*q == '[') {
                        /* ->middleware(['auth', 'throttle']) */
                        const char *br = strchr(q, ']');
                        if (br) {
                            /* Extract comma-separated quoted strings */
                            size_t mlen = 0;
                            const char *mq = q + 1;
                            while (mq < br && mlen < CBM_SZ_512 - 1) {
                                while (mq < br && *mq != '\'' && *mq != '"') mq++;
                                if (mq >= br) break;
                                char mq_quote = *mq;
                                const char *ms = mq + 1;
                                const char *me = strchr(ms, mq_quote);
                                if (me && me < br) {
                                    if (mlen > 0) pending.middleware[mlen++] = ',';
                                    size_t slen = (size_t)(me - ms);
                                    if (mlen + slen >= CBM_SZ_512) slen = CBM_SZ_512 - mlen - 1;
                                    memcpy(pending.middleware + mlen, ms, slen);
                                    mlen += slen;
                                    mq = me + 1;
                                } else break;
                            }
                            pending.middleware[mlen] = '\0';
                            in_chain = true;
                            p = br + 1;
                            continue;
                        }
                    } else {
                        const char *after = extract_quoted_string(q, pending.middleware, CBM_SZ_512);
                        if (after) { in_chain = true; p = after; continue; }
                    }
                } else if (strcmp(verb, "name") == 0) {
                    const char *after = extract_quoted_string(q, pending.name, CBM_SZ_128);
                    if (after) { in_chain = true; p = after; continue; }
                } else if (strcmp(verb, "controller") == 0) {
                    /* ->controller(SomeClass::class) */
                    const char *cc_end = strstr(q, "::class");
                    if (cc_end) {
                        /* Backtrack to find start of class name */
                        const char *cc_start = cc_end;
                        while (cc_start > q && cc_start[-1] != '(' && cc_start[-1] != ' ')
                            cc_start--;
                        while (*cc_start == ' ') cc_start++;
                        size_t clen = (size_t)(cc_end - cc_start);
                        if (clen >= CBM_SZ_256) clen = CBM_SZ_256 - 1;
                        memcpy(pending.controller, cc_start, clen);
                        pending.controller[clen] = '\0';
                        in_chain = true;
                        p = cc_end + 7; /* skip "::class" */
                        continue;
                    }
                }
                /* Couldn't parse — reset chain */
                clear_pending(&pending);
                in_chain = false;
                p = arrow + 2;
                continue;
            } else {
                /* Unknown chained method — reset chain */
                clear_pending(&pending);
                in_chain = false;
                p = arrow + 2;
                continue;
            }
        }

        /* ── Handle Route:: call ── */
        const char *route = next; /* points to "Route::" */

        /* Legacy: Track prefix stack — only for standalone prefixes not in a chain.
         * When prefix is part of a ->group() chain, the group context handles it. */
        if (strncmp(route, "Route::prefix(", 14) == 0) {
            const char *pref = route + 14;
            while (*pref == ' ') pref++;
            if (*pref == '\'' || *pref == '"') {
                char pq = *pref;
                const char *ps = pref + 1;
                const char *pe = strchr(ps, pq);
                if (pe && pe > ps) {
                    char prefix_val[CBM_SZ_128];
                    size_t pvl = (size_t)(pe - ps);
                    if (pvl >= sizeof(prefix_val)) pvl = sizeof(prefix_val) - 1;
                    memcpy(prefix_val, ps, pvl);
                    prefix_val[pvl] = '\0';
                    /* M3-A1: if followed by ->group(), skip legacy push —
                     * the prefix will propagate via group context instead.
                     * Check for -> within 256 chars after closing paren */
                    bool will_chain = false;
                    const char *after = pe + 1; /* skip closing quote */
                    while (*after && *after != ')' && *after != ';') after++;
                    if (*after == ')') {
                        const char *look = after + 1;
                        int look_remaining = 256;
                        while (*look && look_remaining-- > 0) {
                            if (*look == '-' && *(look+1) == '>' &&
                                strncmp(look+2, "group(", 6) == 0) {
                                will_chain = true;
                                break;
                            }
                            look++;
                        }
                    }
                    if (!will_chain) {
                        PREFIX_STACK_PUSH(prefix_val);
                    }
                    /* Always accumulate into pending for group context */
                    memcpy(pending.prefix, prefix_val, pvl + 1);
                    in_chain = true;
                }
            }
            p = route + 14;
            continue;
        }

        /* Legacy: Track group closure — now brace-depth-aware */
        if (strncmp(route, "});", 3) == 0 || strncmp(route, "} );", 4) == 0) {
            PREFIX_STACK_POP();
        }

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

        /* M3-A1: Handle Route:: modifiers that start a chain */
        if (strcmp(verb, "prefix") == 0) {
            /* Already handled above via strncmp — but be safe */
            p = route + 7; continue;
        }
        if (strcmp(verb, "domain") == 0 || strcmp(verb, "middleware") == 0 ||
            strcmp(verb, "name") == 0 || strcmp(verb, "controller") == 0) {
            /* These start a chain: Route::domain(...)->prefix(...)->group(...)
             * Set in_chain even if the arg is a variable (can't resolve textually)
             * so that subsequent ->group() is detected. */
            const char *paren = strchr(verb_start, '(');
            in_chain = true; /* always — even if we can't parse the arg */
            if (paren) {
                const char *q = paren + 1;
                while (*q && *q == ' ') q++;
                if (strcmp(verb, "domain") == 0) {
                    const char *after = extract_quoted_string(q, pending.domain, CBM_SZ_128);
                    if (after) pending.has_domain = true;
                } else if (strcmp(verb, "middleware") == 0) {
                    if (*q == '[') {
                        const char *br = strchr(q, ']');
                        if (br) {
                            size_t mlen = 0;
                            const char *mq = q + 1;
                            while (mq < br && mlen < CBM_SZ_512 - 1) {
                                while (mq < br && *mq != '\'' && *mq != '"') mq++;
                                if (mq >= br) break;
                                char mq_quote = *mq;
                                const char *ms = mq + 1;
                                const char *me = strchr(ms, mq_quote);
                                if (me && me < br) {
                                    if (mlen > 0) pending.middleware[mlen++] = ',';
                                    size_t slen = (size_t)(me - ms);
                                    if (mlen + slen >= CBM_SZ_512) slen = CBM_SZ_512 - mlen - 1;
                                    memcpy(pending.middleware + mlen, ms, slen);
                                    mlen += slen;
                                    mq = me + 1;
                                } else break;
                            }
                            pending.middleware[mlen] = '\0';
                            in_chain = true;
                        }
                    } else {
                        const char *after = extract_quoted_string(q, pending.middleware, CBM_SZ_512);
                        if (after) in_chain = true;
                    }
                } else if (strcmp(verb, "name") == 0) {
                    const char *after = extract_quoted_string(q, pending.name, CBM_SZ_128);
                    if (after) in_chain = true;
                } else if (strcmp(verb, "controller") == 0) {
                    const char *cc_end = strstr(q, "::class");
                    if (cc_end) {
                        const char *cc_start = cc_end;
                        while (cc_start > q && cc_start[-1] != '(' && cc_start[-1] != ' ')
                            cc_start--;
                        while (*cc_start == ' ') cc_start++;
                        size_t clen = (size_t)(cc_end - cc_start);
                        if (clen >= CBM_SZ_256) clen = CBM_SZ_256 - 1;
                        memcpy(pending.controller, cc_start, clen);
                        pending.controller[clen] = '\0';
                        in_chain = true;
                    }
                }
            }
            p = route + 7;
            continue;
        }
        if (strcmp(verb, "group") == 0) {
            /* Route::group([...], function() {}) — traditional array group */
            const char *paren = strchr(verb_start, '(');
            if (paren) {
                const char *q = paren + 1;
                while (*q && *q == ' ') q++;
                /* Parse array attributes: ['prefix' => 'x', 'middleware' => 'y'] */
                if (*q == '[') {
                    const char *br = strchr(q, ']');
                    if (br) {
                        const char *ak = q + 1;
                        while (ak < br) {
                            while (ak < br && *ak != '\'' && *ak != '"') ak++;
                            if (ak >= br) break;
                            char kq = *ak;
                            const char *ks = ak + 1;
                            const char *ke = strchr(ks, kq);
                            if (!ke || ke >= br) break;
                            char key[32] = {0};
                            size_t klen = (size_t)(ke - ks);
                            if (klen >= 32) klen = 31;
                            memcpy(key, ks, klen);

                            /* Find => separator */
                            const char *arrow2 = strstr(ke, "=>");
                            if (!arrow2 || arrow2 >= br) break;

                            const char *val = arrow2 + 2;
                            while (val < br && *val == ' ') val++;
                            if (*val == '\'' || *val == '"') {
                                char vq = *val;
                                const char *vs = val + 1;
                                const char *ve = strchr(vs, vq);
                                if (ve && ve < br) {
                                    size_t vlen = (size_t)(ve - vs);
                                    if (strcmp(key, "prefix") == 0) {
                                        if (vlen >= CBM_SZ_128) vlen = CBM_SZ_128 - 1;
                                        memcpy(pending.prefix, vs, vlen);
                                        pending.prefix[vlen] = '\0';
                                    } else if (strcmp(key, "domain") == 0) {
                                        if (vlen >= CBM_SZ_128) vlen = CBM_SZ_128 - 1;
                                        memcpy(pending.domain, vs, vlen);
                                        pending.domain[vlen] = '\0';
                                        pending.has_domain = true;
                                    } else if (strcmp(key, "middleware") == 0) {
                                        if (vlen >= CBM_SZ_512) vlen = CBM_SZ_512 - 1;
                                        memcpy(pending.middleware, vs, vlen);
                                        pending.middleware[vlen] = '\0';
                                    } else if (strcmp(key, "name") == 0) {
                                        if (vlen >= CBM_SZ_128) vlen = CBM_SZ_128 - 1;
                                        memcpy(pending.name, vs, vlen);
                                        pending.name[vlen] = '\0';
                                    }
                                    ak = ve + 1;
                                    continue;
                                }
                            }
                            ak = val + 1;
                            if (!ak) break;
                        }
                    }
                }
                /* Find the closure's '{' and push group scope */
                bool no_brace = false;
                const char *open_brace = find_closure_brace(paren, &no_brace);
                if (open_brace) {
                    push_group_scope(&pending, brace_depth + 1, group_stack, &group_depth);
                }
                clear_pending(&pending);
                in_chain = false;
            }
            p = route + 7;
            continue;
        }

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

        /* Extract path string argument.
         * Route::get('/path', ...)    → first arg is path
         * Route::match(['GET','POST'], '/path', ...) → second arg is path */
        const char *path_start = NULL;
        const char *path_end = NULL;
        const char *q = paren + 1;
        while (*q && *q == ' ') q++;
        if (*q == '\'' || *q == '"') {
            char quote = *q;
            path_start = q + 1;
            path_end = strchr(path_start, quote);
        } else if (*q == '[' && strcmp(verb, "match") == 0) {
            /* Route::match(['GET','POST'], '/path', ...)
             * Skip the first array argument, extract second string arg */
            const char *arr_end = strchr(q, ']');
            if (arr_end) {
                q = arr_end + 1;
                while (*q && (*q == ' ' || *q == ',')) q++;
                if (*q == '\'' || *q == '"') {
                    char quote = *q;
                    path_start = q + 1;
                    path_end = strchr(path_start, quote);
                }
            }
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

        /* M3-A2: Controller group support.
         * If effective group context has a controller set, and the second arg
         * is a plain string without '@' (no standard 'Controller@method'),
         * use the group controller as the class and the string as the method name. */
        route_group_ctx_t effective = resolve_effective_context(group_stack, group_depth, &pending);
        if (controller[0] == '\0' && effective.controller[0]) {
            if (*arg2 == '\'' || *arg2 == '"') {
                char q2 = *arg2;
                const char *s2 = arg2 + 1;
                const char *e2 = strchr(s2, q2);
                if (e2) {
                    size_t mlen = (size_t)(e2 - s2);
                    if (mlen >= sizeof(method)) mlen = sizeof(method) - 1;
                    memcpy(method, s2, mlen);
                    method[mlen] = '\0';
                    /* Use group controller as the class */
                    memcpy(controller, effective.controller, sizeof(controller) - 1);
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
                build_full_path(full_path, sizeof(full_path), &effective,
                                prefix_stack, prefix_depth, path, suffixes[ai]);

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
                /* Sprint M2: prepend prefix stack to route path.
                 * Route::prefix('sms_campaign')->group( fn() {
                 *   Route::get('/',     ...) → /sms_campaign/
                 *   Route::post('edit', ...) → /sms_campaign/edit */
                char full_path[CBM_SZ_256];
                build_full_path(full_path, sizeof(full_path), &effective,
                                prefix_stack, prefix_depth, path, NULL);
                int64_t route_id = ensure_route_node(gbuf, method_upper, full_path);
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

        /* Clear pending chain context after creating route(s) */
        clear_pending(&pending);
        in_chain = false;

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

    (void)files_scanned; (void)route_file_count; /* used in log below */

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
