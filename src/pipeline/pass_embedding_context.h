/*
 * pass_embedding_context.h — Embedding Context Builder Pass
 *
 * Post-extraction pass that enriches Method, Function, and Class nodes
 * with graph context tokens stored in a node property `embedding_context`.
 * Runs after route_resolve but before semantic_edges.
 *
 * Context tokens:
 *   CLASS:<parent_class_short_name>      — from parent_class property
 *   CALLS:<callee_names>                  — 1-hop outbound CALLS edges
 *   CALLED_BY:<caller_names>             — 1-hop inbound CALLS+ROUTES_TO edges
 *   ROUTE:<method>:<path>                 — from inbound ROUTES_TO route node
 *   INHERITS:<parent_class_names>         — from INHERITS edges
 *
 * Depth: 1 only, LIMIT 10 names per direction.
 */
#ifndef CBM_PASS_EMBEDDING_CONTEXT_H
#define CBM_PASS_EMBEDDING_CONTEXT_H

#include "pipeline/pipeline_internal.h"

int cbm_pipeline_pass_embedding_context(cbm_pipeline_ctx_t *ctx);

#endif
