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

int cbm_pipeline_pass_model_ownership(cbm_pipeline_ctx_t *ctx,
                                       const cbm_file_info_t *files, int file_count) {
    (void)files; (void)file_count;
    if (!ctx || !ctx->gbuf) return 0;
    cbm_log_info("model_ownership", "pass", "gbuf_noop");
    return 0;
}
