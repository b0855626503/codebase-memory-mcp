/*
 * pass_route_resolve.h — Laravel Route → Controller Resolution Pass
 *
 * Post-extraction pass that detects Laravel route definitions in PHP route
 * files and creates ROUTES_TO edges connecting Route nodes to their handler
 * controller methods.  Supports tuple syntax ([C::class,'m']), string syntax
 * ('C@m'), invokable controllers, and Route::resource() expansion.
 */
#ifndef CBM_PASS_ROUTE_RESOLVE_H
#define CBM_PASS_ROUTE_RESOLVE_H

#include "pipeline/pipeline_internal.h"

/* Detect Laravel route definitions in PHP files and create ROUTES_TO edges.
 * Runs as a post-extraction pass — all Function/Method/Route nodes must
 * already exist in the graph buffer before this pass runs.
 *
 * Returns number of ROUTES_TO edges created, or -1 on error. */
int cbm_pipeline_pass_route_resolve(cbm_pipeline_ctx_t *ctx);

#endif /* CBM_PASS_ROUTE_RESOLVE_H */
