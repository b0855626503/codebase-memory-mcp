/*
 * userconfig.c — User-defined extension→language mappings.
 *
 * Reads extra_extensions from:
 *   Global:  $XDG_CONFIG_HOME/codebase-memory-mcp/config.json
 *            (falls back to ~/.config/codebase-memory-mcp/config.json)
 *   Project: {repo_root}/.codebase-memory.json
 *
 * Project config wins over global. Unknown language values warn and are
 * skipped (fail-open). Missing files are silently ignored.
 */
#include "discover/userconfig.h"
#include "cbm.h" /* CBMLanguage, CBM_LANG_* */
#include "foundation/constants.h"
#include "foundation/platform.h" /* cbm_safe_getenv */

enum { MAX_CONFIG_SIZE = 65536 };
#include "foundation/log.h"

#include <yyjson/yyjson.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Process-global user config pointer ──────────────────────────── */

static const cbm_userconfig_t *g_userconfig = NULL;

void cbm_set_user_lang_config(const cbm_userconfig_t *cfg) {
    g_userconfig = cfg;
}

const cbm_userconfig_t *cbm_get_user_lang_config(void) {
    return g_userconfig;
}

/* Forward declarations for embedding config helpers (defined below). */
static void init_embedding_defaults(cbm_embedding_config_t *ec);
static int  parse_embedding_section(yyjson_val *root, cbm_embedding_config_t *ec,
                                    const char *source_label);

const cbm_embedding_config_t *cbm_embedding_config_get(void) {
    static cbm_embedding_config_t fallback;
    static bool fallback_init = false;
    if (!fallback_init) {
        init_embedding_defaults(&fallback);
        fallback_init = true;
    }
    /* No userconfig loaded → use defaults (backward compatible). */
    if (!g_userconfig) {
        return &fallback;
    }
    /* Userconfig loaded → return real config; consumer checks enabled flag. */
    return &g_userconfig->embedding;
}

/* ── Language name → enum table ──────────────────────────────────── */

/*
 * Reverse-mapping from lowercase language name strings to CBMLanguage.
 * Covers all names exposed by cbm_language_name() plus common aliases.
 */
typedef struct {
    const char *name; /* lowercase */
    CBMLanguage lang;
} lang_name_entry_t;

static const lang_name_entry_t LANG_NAME_TABLE[] = {
    {"go", CBM_LANG_GO},
    {"python", CBM_LANG_PYTHON},
    {"javascript", CBM_LANG_JAVASCRIPT},
    {"typescript", CBM_LANG_TYPESCRIPT},
    {"tsx", CBM_LANG_TSX},
    {"rust", CBM_LANG_RUST},
    {"java", CBM_LANG_JAVA},
    {"c++", CBM_LANG_CPP},
    {"cpp", CBM_LANG_CPP},
    {"c#", CBM_LANG_CSHARP},
    {"csharp", CBM_LANG_CSHARP},
    {"php", CBM_LANG_PHP},
    {"lua", CBM_LANG_LUA},
    {"scala", CBM_LANG_SCALA},
    {"kotlin", CBM_LANG_KOTLIN},
    {"ruby", CBM_LANG_RUBY},
    {"c", CBM_LANG_C},
    {"bash", CBM_LANG_BASH},
    {"sh", CBM_LANG_BASH},
    {"zig", CBM_LANG_ZIG},
    {"elixir", CBM_LANG_ELIXIR},
    {"haskell", CBM_LANG_HASKELL},
    {"ocaml", CBM_LANG_OCAML},
    {"objective-c", CBM_LANG_OBJC},
    {"objc", CBM_LANG_OBJC},
    {"swift", CBM_LANG_SWIFT},
    {"dart", CBM_LANG_DART},
    {"perl", CBM_LANG_PERL},
    {"groovy", CBM_LANG_GROOVY},
    {"erlang", CBM_LANG_ERLANG},
    {"r", CBM_LANG_R},
    {"html", CBM_LANG_HTML},
    {"css", CBM_LANG_CSS},
    {"scss", CBM_LANG_SCSS},
    {"yaml", CBM_LANG_YAML},
    {"toml", CBM_LANG_TOML},
    {"hcl", CBM_LANG_HCL},
    {"terraform", CBM_LANG_HCL},
    {"sql", CBM_LANG_SQL},
    {"dockerfile", CBM_LANG_DOCKERFILE},
    {"clojure", CBM_LANG_CLOJURE},
    {"f#", CBM_LANG_FSHARP},
    {"fsharp", CBM_LANG_FSHARP},
    {"julia", CBM_LANG_JULIA},
    {"vimscript", CBM_LANG_VIMSCRIPT},
    {"nix", CBM_LANG_NIX},
    {"common lisp", CBM_LANG_COMMONLISP},
    {"commonlisp", CBM_LANG_COMMONLISP},
    {"lisp", CBM_LANG_COMMONLISP},
    {"elm", CBM_LANG_ELM},
    {"fortran", CBM_LANG_FORTRAN},
    {"cuda", CBM_LANG_CUDA},
    {"cobol", CBM_LANG_COBOL},
    {"verilog", CBM_LANG_VERILOG},
    {"emacs lisp", CBM_LANG_EMACSLISP},
    {"emacslisp", CBM_LANG_EMACSLISP},
    {"json", CBM_LANG_JSON},
    {"xml", CBM_LANG_XML},
    {"markdown", CBM_LANG_MARKDOWN},
    {"makefile", CBM_LANG_MAKEFILE},
    {"cmake", CBM_LANG_CMAKE},
    {"protobuf", CBM_LANG_PROTOBUF},
    {"graphql", CBM_LANG_GRAPHQL},
    {"vue", CBM_LANG_VUE},
    {"svelte", CBM_LANG_SVELTE},
    {"meson", CBM_LANG_MESON},
    {"glsl", CBM_LANG_GLSL},
    {"ini", CBM_LANG_INI},
    {"matlab", CBM_LANG_MATLAB},
    {"lean", CBM_LANG_LEAN},
    {"form", CBM_LANG_FORM},
    {"magma", CBM_LANG_MAGMA},
    {"wolfram", CBM_LANG_WOLFRAM},
};

#define LANG_NAME_TABLE_SIZE (sizeof(LANG_NAME_TABLE) / sizeof(LANG_NAME_TABLE[0]))

/*
 * Parse a language string (case-insensitive) to a CBMLanguage enum.
 * Returns CBM_LANG_COUNT if the string is not recognized.
 */
static CBMLanguage lang_from_string(const char *s) {
    if (!s || !s[0]) {
        return CBM_LANG_COUNT;
    }

    /* Build a lowercase copy for comparison */
    char lower[CBM_SZ_64];
    size_t i;
    for (i = 0; i < sizeof(lower) - SKIP_ONE && s[i]; i++) {
        lower[i] = (char)tolower((unsigned char)s[i]);
    }
    lower[i] = '\0';

    for (size_t j = 0; j < LANG_NAME_TABLE_SIZE; j++) {
        if (strcmp(LANG_NAME_TABLE[j].name, lower) == 0) {
            return LANG_NAME_TABLE[j].lang;
        }
    }
    return CBM_LANG_COUNT;
}

/* ── Config directory helper ─────────────────────────────────────── */

/* cbm_app_config_dir() is now in platform.c (cross-platform). */

/* ── JSON parsing ────────────────────────────────────────────────── */

/*
 * Parse extra_extensions from a yyjson object root.
 * Appends valid entries to *entries / *count (growing via realloc).
 * Project-level entries (from_project=true) are appended after global
 * entries so that a later dedup pass can prefer project values.
 *
 * Returns 0 on success, -1 on alloc failure.
 */
static int parse_extra_extensions(yyjson_val *root, cbm_userext_t **entries, int *count,
                                  const char *source_label) {
    if (!yyjson_is_obj(root)) {
        cbm_log_warn("userconfig.bad_root", "file", source_label);
        return 0;
    }

    yyjson_val *extra = yyjson_obj_get(root, "extra_extensions");
    if (!extra) {
        return 0; /* key absent — fine */
    }
    if (!yyjson_is_obj(extra)) {
        cbm_log_warn("userconfig.bad_extra_extensions", "file", source_label);
        return 0;
    }

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(extra, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
        yyjson_val *val = yyjson_obj_iter_get_val(key);

        const char *ext_str = yyjson_get_str(key);
        const char *lang_str = yyjson_get_str(val);

        if (!ext_str || !lang_str) {
            cbm_log_warn("userconfig.skip_non_string", "file", source_label);
            continue;
        }

        /* Extension must start with '.' */
        if (ext_str[0] != '.') {
            cbm_log_warn("userconfig.skip_bad_ext", "file", source_label, "ext", ext_str);
            continue;
        }

        CBMLanguage lang = lang_from_string(lang_str);
        if (lang == CBM_LANG_COUNT) {
            cbm_log_warn("userconfig.unknown_lang", "file", source_label, "lang", lang_str);
            continue; /* fail-open: skip unknown languages */
        }

        /* Grow the array */
        cbm_userext_t *tmp = realloc(*entries, (size_t)(*count + SKIP_ONE) * sizeof(cbm_userext_t));
        if (!tmp) {
            return CBM_NOT_FOUND;
        }
        *entries = tmp;

        char *ext_copy = strdup(ext_str);
        if (!ext_copy) {
            return CBM_NOT_FOUND;
        }

        (*entries)[*count].ext = ext_copy;
        (*entries)[*count].lang = lang;
        (*count)++;
    }
    return 0;
}

/*
 * Read a JSON file and parse extra_extensions + embedding from it.
 * Silently ignores missing files. Logs warnings for corrupt JSON.
 * ec may be NULL (caller doesn't want embedding config).
 * version_out receives the "version" field (0 if absent/unreadable).
 * Returns 0 on success (or absent file), -1 on alloc failure.
 */
static int load_config_file(const char *path, cbm_userext_t **entries, int *count,
                            cbm_embedding_config_t *ec, int *version_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0; /* file absent — silently ignore */
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return 0;
    }
    long len = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return 0;
    }

    if (len <= 0 || len > MAX_CONFIG_SIZE) {
        (void)fclose(f);
        if (len > MAX_CONFIG_SIZE) {
            cbm_log_warn("userconfig.file_too_large", "path", path);
        }
        return 0;
    }

    char *buf = malloc((size_t)len + SKIP_ONE);
    if (!buf) {
        (void)fclose(f);
        return CBM_NOT_FOUND;
    }

    size_t nread = fread(buf, SKIP_ONE, (size_t)len, f);
    (void)fclose(f);
    if (nread > (size_t)len) {
        nread = (size_t)len;
    }
    buf[nread] = '\0';

    yyjson_doc *doc = yyjson_read(buf, nread, 0);
    free(buf);

    if (!doc) {
        cbm_log_warn("userconfig.corrupt_json", "path", path);
        return 0; /* corrupt JSON — silently ignore (fail-open) */
    }

    yyjson_val *root = yyjson_doc_get_root(doc);

    /* Parse top-level version (0 if absent). */
    if (version_out) {
        yyjson_val *ver = yyjson_obj_get(root, "version");
        if (ver && yyjson_is_int(ver)) {
            int v = yyjson_get_int(ver);
            if (v >= 0) *version_out = v;
        }
    }

    int rc = parse_extra_extensions(root, entries, count, path);
    if (rc == 0 && ec) {
        rc = parse_embedding_section(root, ec, path);
    }
    yyjson_doc_free(doc);
    return rc;
}

/* ── Embedding config parsing ────────────────────────────────────── */

/* Benchmark profile → signal preset lookup. */
typedef struct {
    const char *name;
    bool calls;
    bool called_by;
    bool routes_to;
    bool inherits;
} embedding_profile_t;

static const embedding_profile_t EMBEDDING_PROFILES[] = {
    {"semantic_only",  false, false, false, false},
    {"graph_light",    true,  false, false, false},
    {"graph_balanced", true,  true,  false, false},
    {"graph_full",     true,  true,  true,  true},
};
#define EMBEDDING_PROFILE_COUNT \
    (sizeof(EMBEDDING_PROFILES) / sizeof(EMBEDDING_PROFILES[0]))

/*
 * Apply a named profile to the signal toggles.
 * Only graph signals are affected — label/parent_class/limits
 * are left alone so they can be set independently via context.*.
 * Returns true if the profile was found and applied.
 */
static bool apply_profile(cbm_embedding_config_t *ec, const char *name) {
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < EMBEDDING_PROFILE_COUNT; i++) {
        if (strcmp(EMBEDDING_PROFILES[i].name, name) == 0) {
            ec->signals.calls     = EMBEDDING_PROFILES[i].calls;
            ec->signals.called_by = EMBEDDING_PROFILES[i].called_by;
            ec->signals.routes_to = EMBEDDING_PROFILES[i].routes_to;
            ec->signals.inherits  = EMBEDDING_PROFILES[i].inherits;
            return true;
        }
    }
    return false;
}

/*
 * Initialise an embedding config struct with safe defaults.
 * Mirror of the public cbm_embedding_config_defaults().
 */
static void init_embedding_defaults(cbm_embedding_config_t *ec) {
    ec->enabled = true;
    ec->profile[0] = '\0'; /* no profile → explicit signals used */
    ec->signals.label = true;
    ec->signals.calls = true;
    ec->signals.called_by = true;
    ec->signals.routes_to = true;
    ec->signals.inherits = true;
    ec->signals.parent_class = false; /* collapses same-class methods */
    ec->signals.qualified_name = true;  /* domain vocabulary from QN */
    ec->signals.namespace_ = true;      /* package/module structure */
    ec->signals.file_path = false;       /* off by default — noisy */
    ec->signals.identifier_tokens = true; /* camelCase → tokens */
    ec->limits.max_names_per_direction = 10;
}

void cbm_embedding_config_defaults(cbm_embedding_config_t *ec) {
    if (ec) init_embedding_defaults(ec);
}

/*
 * Overlay embedding config from a JSON "embedding" key onto *ec.
 * Only sets fields that are present in the JSON — absent keys leave
 * the current value untouched (so project overlays on global).
 * Returns 0 on success or absent key, -1 on malformed JSON.
 */
static int parse_embedding_section(yyjson_val *root, cbm_embedding_config_t *ec,
                                    const char *source_label) {
    if (!yyjson_is_obj(root)) return 0;

    yyjson_val *emb = yyjson_obj_get(root, "embedding");
    if (!emb) return 0; /* key absent — fine */
    if (!yyjson_is_obj(emb)) {
        cbm_log_warn("userconfig.bad_embedding", "file", source_label);
        return 0;
    }

    /* top-level "enabled" */
    yyjson_val *en = yyjson_obj_get(emb, "enabled");
    if (en && yyjson_is_bool(en)) {
        ec->enabled = yyjson_get_bool(en);
    }

    /* "profile" — apply BEFORE context.* so individual fields win.
     * Store the profile name even if we also have context overrides. */
    yyjson_val *prof = yyjson_obj_get(emb, "profile");
    if (prof && yyjson_is_str(prof)) {
        const char *pname = yyjson_get_str(prof);
        if (pname && pname[0]) {
            if (apply_profile(ec, pname)) {
                snprintf(ec->profile, sizeof(ec->profile), "%s", pname);
            } else {
                cbm_log_warn("userconfig.unknown_profile", "file", source_label,
                             "profile", pname);
            }
        }
    }

    /* "context" sub-object — overlays on top of profile defaults */
    yyjson_val *ctx = yyjson_obj_get(emb, "context");
    if (ctx && yyjson_is_obj(ctx)) {
        #define OVERLAY_BOOL(keyname, field) do { \
            yyjson_val *v_##field = yyjson_obj_get(ctx, keyname); \
            if (v_##field && yyjson_is_bool(v_##field)) { \
                ec->signals.field = yyjson_get_bool(v_##field); \
            } \
        } while(0)

        OVERLAY_BOOL("label",          label);
        OVERLAY_BOOL("calls",          calls);
        OVERLAY_BOOL("called_by",      called_by);
        OVERLAY_BOOL("routes_to",      routes_to);
        OVERLAY_BOOL("inherits",       inherits);
        OVERLAY_BOOL("parent_class",   parent_class);
        OVERLAY_BOOL("qualified_name", qualified_name);
        OVERLAY_BOOL("namespace",      namespace_);
        OVERLAY_BOOL("file_path",         file_path);
        OVERLAY_BOOL("identifier_tokens", identifier_tokens);

        #undef OVERLAY_BOOL
    }

    /* "limits" sub-object */
    yyjson_val *lim = yyjson_obj_get(emb, "limits");
    if (lim && yyjson_is_obj(lim)) {
        yyjson_val *v_max = yyjson_obj_get(lim, "max_names_per_direction");
        if (v_max && yyjson_is_int(v_max)) {
            int val = yyjson_get_int(v_max);
            if (val >= 0 && val <= 100) {
                ec->limits.max_names_per_direction = val;
            }
        }
    }

    return 0;
}

/* ── Public API ──────────────────────────────────────────────────── */

cbm_userconfig_t *cbm_userconfig_load(const char *repo_path) {
    cbm_userconfig_t *cfg = calloc(CBM_ALLOC_ONE, sizeof(cbm_userconfig_t));
    if (!cfg) {
        return NULL;
    }

    /* Initialise embedding config with defaults so that missing keys
     * in both global and project configs still produce valid behaviour. */
    init_embedding_defaults(&cfg->embedding);

    cbm_userext_t *entries = NULL;
    int count = 0;

    /* ── Step 1: Load global config ── */
    enum { PATH_BUF_SZ = 1280 };
    const char *cfg_base = cbm_app_config_dir();
    const char *cfg_fallback = cfg_base ? cfg_base : "/tmp";
    char global_path[PATH_BUF_SZ];
    snprintf(global_path, sizeof(global_path), "%s/codebase-memory-mcp/config.json", cfg_fallback);

    if (load_config_file(global_path, &entries, &count, &cfg->embedding, &cfg->version) != 0) {
        for (int i = 0; i < count; i++) {
            free(entries[i].ext);
        }
        free(entries);
        free(cfg);
        return NULL;
    }

    int global_count = count; /* entries[0..global_count) are from global */

    /* ── Step 2: Load project config ── */
    if (repo_path && repo_path[0]) {
        char project_path[PATH_BUF_SZ];
        snprintf(project_path, sizeof(project_path), "%s/.codebase-memory.json", repo_path);

        if (load_config_file(project_path, &entries, &count, &cfg->embedding, &cfg->version) != 0) {
            /* Free already-allocated entries */
            for (int i = 0; i < count; i++) {
                free(entries[i].ext);
            }
            free(entries);
            free(cfg);
            return NULL;
        }
    }

    /*
     * ── Step 3: Dedup — project entries win over global ──
     *
     * For any extension that appears in both global (indices 0..global_count)
     * and project (indices global_count..count), remove the global entry by
     * replacing it with the last global entry (order-insensitive dedup).
     */
    for (int p = global_count; p < count; p++) {
        for (int g = 0; g < global_count; g++) {
            if (entries[g].ext && strcmp(entries[g].ext, entries[p].ext) == 0) {
                /* Remove global entry: overwrite with last global entry */
                free(entries[g].ext);
                entries[g] = entries[global_count - SKIP_ONE];
                entries[global_count - SKIP_ONE].ext = NULL; /* mark as consumed */
                global_count--;
                break;
            }
        }
    }

    /*
     * Compact: remove any NULL-ext slots left by the dedup step.
     * (Those are the consumed "last global" entries.)
     */
    int write_idx = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].ext != NULL) {
            entries[write_idx++] = entries[i];
        }
    }
    count = write_idx;

    cfg->entries = entries;
    cfg->count = count;

    /* ── Step 4: Version check ── */
    if (cfg->version == 0) {
        /* Unversioned config — treat as current. No migration needed
         * since v1 is the first versioned schema. */
        cfg->version = CBM_USERCONFIG_CURRENT_VERSION;
    } else if (cfg->version > CBM_USERCONFIG_CURRENT_VERSION) {
        char vbuf[16], cb[16];
        snprintf(vbuf, sizeof(vbuf), "%d", cfg->version);
        snprintf(cb, sizeof(cb), "%d", CBM_USERCONFIG_CURRENT_VERSION);
        cbm_log_warn("userconfig.future_version",
                     "version", vbuf,
                     "current", cb,
                     "hint", "this config was written for a newer CBM — some keys may be ignored");
    }

    return cfg;
}

CBMLanguage cbm_userconfig_lookup(const cbm_userconfig_t *cfg, const char *ext) {
    if (!cfg || !ext || !ext[0]) {
        return CBM_LANG_COUNT;
    }
    for (int i = 0; i < cfg->count; i++) {
        if (cfg->entries[i].ext && strcmp(cfg->entries[i].ext, ext) == 0) {
            return cfg->entries[i].lang;
        }
    }
    return CBM_LANG_COUNT;
}

void cbm_userconfig_free(cbm_userconfig_t *cfg) {
    if (!cfg) {
        return;
    }
    for (int i = 0; i < cfg->count; i++) {
        free(cfg->entries[i].ext);
    }
    free(cfg->entries);
    free(cfg);
}
