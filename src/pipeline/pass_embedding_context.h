/*
 * pass_embedding_context.h — Embedding Context Builder Pass
 *
 * Post-extraction pass that enriches Method, Function, and Class nodes
 * with graph context tokens stored in a node property `embedding_context`.
 * Runs after route_resolve but before semantic_edges.
 *
 * Context signals are configurable via .codebase-memory.json:
 *
 *   embedding.context.label        — "Method:deposit" prefix
 *   embedding.context.calls        — "CALLS:<callee_names>" 1-hop outbound
 *   embedding.context.called_by    — "CALLED_BY:<caller_names>" 1-hop inbound
 *   embedding.context.routes_to    — "ROUTE:<method>:<path>" first inbound
 *   embedding.context.inherits     — "INHERITS:<parent_names>" outbound
 *   embedding.context.parent_class — "CLASS:<short_name>" (default OFF)
 *
 *   embedding.limits.max_names_per_direction — default 10
 *   embedding.enabled              — set false to skip pass entirely
 *
 * When no config is present, defaults match the legacy hardcoded behaviour.
 */
#ifndef CBM_PASS_EMBEDDING_CONTEXT_H
#define CBM_PASS_EMBEDDING_CONTEXT_H

#include "pipeline/pipeline_internal.h"

int cbm_pipeline_pass_embedding_context(cbm_pipeline_ctx_t *ctx);

#endif
