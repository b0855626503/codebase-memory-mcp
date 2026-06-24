#ifndef CBM_PASS_MODEL_OWNERSHIP_H
#define CBM_PASS_MODEL_OWNERSHIP_H

#include "pipeline/pipeline_internal.h"

int cbm_pipeline_pass_model_ownership(cbm_pipeline_ctx_t *ctx,
                                       const cbm_file_info_t *files, int file_count);

char *cbm_extract_model_class(const char *source);

#endif /* CBM_PASS_MODEL_OWNERSHIP_H */
