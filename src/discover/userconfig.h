/*
 * userconfig.h — User-defined file extension → language mappings and
 *                embedding context configuration.
 *
 * Reads configuration from two optional JSON config files:
 *   Global:  $XDG_CONFIG_HOME/codebase-memory-mcp/config.json
 *            (falls back to ~/.config/codebase-memory-mcp/config.json)
 *   Project: {repo_root}/.codebase-memory.json
 *
 * Project config wins over global. Unknown values warn and are
 * skipped (fail-open). Missing files are silently ignored.
 *
 * Format (v1):
 *   {
 *     "version": 1,
 *     "extra_extensions": {".blade.php": "php", ".mjs": "javascript"},
 *     "embedding": {
 *       "enabled": true,
 *       "profile": "graph_full",
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
 * All fields are optional with sensible defaults.
 * "profile" is applied BEFORE individual "context.*" overrides.
 * Profiles: semantic_only, graph_light, graph_balanced, graph_full.
 */

#ifndef CBM_USERCONFIG_H
#define CBM_USERCONFIG_H

#include "cbm.h" /* CBMLanguage */
#include <stdbool.h>

/* Current config schema version. Bump when the schema meaningfully
 * changes so the parser can warn on future-version configs. */
#define CBM_USERCONFIG_CURRENT_VERSION 1

/* ── Types ──────────────────────────────────────────────────────── */

typedef struct {
    char *ext;        /* file extension including dot, e.g. ".blade.php" */
    CBMLanguage lang; /* resolved language enum */
} cbm_userext_t;

/* ── Embedding context configuration ─────────────────────────────── */

/* Per-signal toggles for embedding context builder.
 * Each controls whether a token appears in the embedding_context JSON
 * node property. Default: all ON except parent_class (collapses
 * same-class methods — see discovery-3a3.md Task 2). */
typedef struct {
    bool label;          /* "Method:" / "Function:" / "Class:" prefix */
    bool calls;          /* "CALLS:<callee_names>" 1-hop outbound */
    bool called_by;      /* "CALLED_BY:<caller_names>" 1-hop inbound */
    bool routes_to;      /* "ROUTE:<method>:<path>" first inbound route */
    bool inherits;       /* "INHERITS:<parent_names>" outbound */
    bool parent_class;   /* "CLASS:<short_name>" — disabled by default */
    bool qualified_name; /* "QN:full.qualified.name" — domain vocabulary */
    bool namespace_;        /* "NS:Package.Module" — package structure tokens */
    bool file_path;         /* "PATH:dir1,dir2" — normalized path tokens */
    bool identifier_tokens; /* "TOKENS:load,deposit" — camel/snake_case decomposition */
} cbm_embedding_signals_t;

typedef struct {
    int max_names_per_direction; /* default: 10 */
} cbm_embedding_limits_t;

typedef struct {
    bool enabled;                    /* default: true */
    char profile[32];                /* "" or "semantic_only"|"graph_light"|"graph_balanced"|"graph_full" */
    cbm_embedding_signals_t signals; /* per-signal toggles */
    cbm_embedding_limits_t limits;   /* sizing knobs */
} cbm_embedding_config_t;

typedef struct {
    int version;              /* 0 = unversioned/absent, 1+ = schema version */
    cbm_userext_t *entries;   /* heap-allocated array */
    int count;                /* number of entries */
    cbm_embedding_config_t embedding; /* embedding context config */
} cbm_userconfig_t;

/* ── API ────────────────────────────────────────────────────────── */

/*
 * Load user config from global + project files, merge (project wins).
 * repo_path: absolute path to the repository root (for project config).
 * Returns a heap-allocated cbm_userconfig_t (caller must free via
 * cbm_userconfig_free). Returns NULL only on allocation failure.
 * Missing config files are silently ignored.
 */
cbm_userconfig_t *cbm_userconfig_load(const char *repo_path);

/*
 * Look up a file extension in the user config.
 * ext: extension including dot, e.g. ".blade.php"
 * Returns the mapped CBMLanguage, or CBM_LANG_COUNT if not found.
 */
CBMLanguage cbm_userconfig_lookup(const cbm_userconfig_t *cfg, const char *ext);

/* Free a cbm_userconfig_t returned by cbm_userconfig_load. NULL-safe. */
void cbm_userconfig_free(cbm_userconfig_t *cfg);

/* ── Integration hook ───────────────────────────────────────────── */

/*
 * Set the process-global user config that cbm_language_for_extension()
 * will consult before the built-in table.
 * cfg may be NULL to clear the override.
 * Not thread-safe — call before spawning worker threads.
 */
void cbm_set_user_lang_config(const cbm_userconfig_t *cfg);

/*
 * Get the currently active process-global user config.
 * Returns NULL if none has been set.
 * Called internally by cbm_language_for_extension().
 */
const cbm_userconfig_t *cbm_get_user_lang_config(void);

/*
 * Get the effective embedding config (never NULL).
 * Returns a pointer to the config if userconfig is loaded and embedding
 * is enabled, or a static default config otherwise.
 * Safe to call from any pass — no allocation, always returns a valid pointer.
 */
const cbm_embedding_config_t *cbm_embedding_config_get(void);

/*
 * Initialise an embedding config struct with defaults.
 * Called by the parser before overlay; also used for the static fallback.
 * Defaults: enabled=true, all signals ON except parent_class=false,
 *            max_names_per_direction=10.
 */
void cbm_embedding_config_defaults(cbm_embedding_config_t *ec);

#endif /* CBM_USERCONFIG_H */
