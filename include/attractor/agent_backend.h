#ifndef ATTRACTOR_AGENT_BACKEND_H
#define ATTRACTOR_AGENT_BACKEND_H

#include "agent/agent.h"
#include "attractor/engine.h"

/* Codergen backend that runs each stage as a fresh AgentSession using the
 * provider-aligned profile. The client and environment are borrowed; model and
 * provider are copied and may be overridden per node (llm_model/llm_provider).
 * A NULL provider uses the client's default. Returns NULL on allocation failure. */
CodergenBackend *agent_codergen_backend_new(LlmClient *client, ExecutionEnv *env,
                                            const char *model, const char *provider);
void             agent_codergen_backend_free(CodergenBackend *backend);

#endif
