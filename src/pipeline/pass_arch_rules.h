#ifndef CBM_PIPELINE_PASS_ARCH_RULES_H
#define CBM_PIPELINE_PASS_ARCH_RULES_H

#include "store/store.h"

/* Check architecture rules against a project's knowledge graph.
 * Returns a heap-allocated JSON string of violations. Caller frees. */
char *cbm_check_architecture_rules(cbm_store_t *store, const char *project);

#endif /* CBM_PIPELINE_PASS_ARCH_RULES_H */
