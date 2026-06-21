/*
 * pass_blade_calls.h — Blade Template → PHP Helper Resolution
 *
 * Post-extraction pass. Scans Blade template files for function calls
 * inside {{ }} directives and creates CALLS edges from the Blade File
 * node to the resolved PHP Function/Method node.
 */
#ifndef CBM_PASS_BLADE_CALLS_H
#define CBM_PASS_BLADE_CALLS_H

#include "pipeline/pipeline_internal.h"

int cbm_pipeline_pass_blade_calls(cbm_pipeline_ctx_t *ctx);

#endif
