#include "attractor/dot_parser.h"
#include "attractor/validator.h"
#include "attractor/engine.h"
#include "llm/client.h"
#include "agent/agent.h"
#include "util/str.h"
#include "util/json.h"
#include "util/http.h"
#include "util/io.h"
#include "util/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

/*============================================================================
 * Usage
 *==========================================================================*/

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] <pipeline.dot>\n"
        "\n"
        "Options:\n"
        "  --logs-dir <dir>      Log/artifact directory (default: ./attractor-run-<ts>)\n"
        "  --validate-only       Parse and validate, do not execute\n"
        "  --dry-run             Execute with simulated LLM backend\n"
        "  --auto-approve        Auto-approve all human gates\n"
        "  --resume <checkpoint> Resume from a checkpoint file\n"
        "  --model <model>       Override default LLM model\n"
        "  --provider <name>     Override default LLM provider\n"
        "  --verbose             Verbose event output\n"
        "  --help                Show this help\n"
        "\n"
        "Environment:\n"
        "  ANTHROPIC_API_KEY     Anthropic API key\n"
        "  OPENAI_API_KEY        OpenAI API key\n"
        "  GEMINI_API_KEY        Google Gemini API key\n"
        "\n", argv0);
}

/*============================================================================
 * Read file to string
 *==========================================================================*/

static char *read_file(const char *path) { return io_read_text(path, ATTRACTOR_INPUT_LIMIT); }

/*============================================================================
 * Event callback (verbose mode)
 *==========================================================================*/

static const char *event_kind_str(PipelineEventKind kind) {
    switch (kind) {
        case PIPE_EVT_PIPELINE_STARTED:     return "PIPELINE_STARTED";
        case PIPE_EVT_PIPELINE_COMPLETED:   return "PIPELINE_COMPLETED";
        case PIPE_EVT_PIPELINE_FAILED:      return "PIPELINE_FAILED";
        case PIPE_EVT_STAGE_STARTED:        return "STAGE_STARTED";
        case PIPE_EVT_STAGE_COMPLETED:      return "STAGE_COMPLETED";
        case PIPE_EVT_STAGE_FAILED:         return "STAGE_FAILED";
        case PIPE_EVT_STAGE_RETRYING:       return "STAGE_RETRYING";
        case PIPE_EVT_INTERVIEW_STARTED:    return "INTERVIEW_STARTED";
        case PIPE_EVT_INTERVIEW_COMPLETED:  return "INTERVIEW_COMPLETED";
        case PIPE_EVT_CHECKPOINT_SAVED:     return "CHECKPOINT_SAVED";
        default:                            return "UNKNOWN";
    }
}

static void verbose_event_cb(const PipelineEvent *ev, void *userdata) {
    (void)userdata;
    fprintf(stderr, "[%s]", event_kind_str(ev->kind));
    if (ev->node_id)
        fprintf(stderr, " node=%s", ev->node_id);
    if (ev->attempt > 0)
        fprintf(stderr, " attempt=%d", ev->attempt);
    if (ev->data)
        fprintf(stderr, " %s", ev->data);
    fprintf(stderr, "\n");
}

/*============================================================================
 * Codergen backend using agent session
 *==========================================================================*/

typedef struct {
    LlmClient    *llm;
    const char   *model;
    const char   *provider;
    ExecutionEnv *exec_env;
} AgentBackendState;

static ReasoningEffort parse_reasoning_effort(const char *s) {
    if (!s || !*s) return REASONING_HIGH;
    if (str_eq(s, "low"))    return REASONING_LOW;
    if (str_eq(s, "medium")) return REASONING_MEDIUM;
    if (str_eq(s, "high"))   return REASONING_HIGH;
    if (str_eq(s, "none"))   return REASONING_NONE;
    return REASONING_HIGH;
}

static char *agent_backend_run(CodergenBackend *self, const DotNode *node,
                                const char *prompt, const PipelineContext *ctx) {
    AgentBackendState *st = self->impl;
    (void)ctx;

    /* Use node's model/provider if set, else fall back to runner defaults */
    const char *model    = (node && node->llm_model)    ? node->llm_model    : st->model;
    const char *provider = (node && node->llm_provider)  ? node->llm_provider  : st->provider;
    ReasoningEffort effort = (node && node->reasoning_effort)
        ? parse_reasoning_effort(node->reasoning_effort) : (str_eq(provider,"openai")?REASONING_HIGH:REASONING_NONE);

    ToolRegistry registry;tool_registry_init(&registry);agent_register_core_tools(&registry);
    ActiveTool tools[6]={0};ToolExecutionContext contexts[6]={0};
    if(registry.failed || registry.count>6) {tool_registry_free(&registry);return NULL;}
    for(size_t i=0;i<registry.count;i++) {
        contexts[i]=(ToolExecutionContext){.tool=registry.tools[i],.env=st->exec_env,.default_timeout_ms=10000,.max_timeout_ms=600000};
        tools[i]=(ActiveTool){.def=registry.tools[i]->def,.execute=agent_active_tool,.userdata=&contexts[i]};
    }

    LlmError err = {0};
    GenerateResult *result = llm_generate(
        st->llm, model, prompt,
        NULL, 0,      /* messages */
        NULL,          /* system_prompt */
        tools, registry.count,
        50,            /* max_tool_rounds */
        effort,
        provider,
        2,             /* max_retries */
        &err
    );

    tool_registry_free(&registry);

    if (!result) {
        fprintf(stderr, "LLM error: %s\n", err.message ? err.message : "unknown");
        llm_error_free(&err);
        return NULL;
    }

    char *text = result->text ? str_dup(result->text) : str_dup("");
    generate_result_free(result);
    return text;
}

/*============================================================================
 * Main
 *==========================================================================*/

int main(int argc, char **argv) {
    const char *dot_path = NULL;
    const char *logs_dir = NULL;
    const char *resume_path = NULL;
    const char *model_override = NULL;
    const char *provider_override = NULL;
    bool validate_only = false;
    bool dry_run = false;
    bool auto_approve = false;
    bool verbose = false;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--logs-dir") == 0 && i + 1 < argc) {
            logs_dir = argv[++i];
        } else if (strcmp(argv[i], "--validate-only") == 0) {
            validate_only = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--auto-approve") == 0) {
            auto_approve = true;
        } else if (strcmp(argv[i], "--resume") == 0 && i + 1 < argc) {
            resume_path = argv[++i];
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_override = argv[++i];
        } else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
            provider_override = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (argv[i][0] != '-') {
            dot_path = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!dot_path) {
        fprintf(stderr, "Error: no pipeline .dot file specified\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Read DOT source */
    char *source = read_file(dot_path);
    if (!source) {
        fprintf(stderr, "Error: cannot read file: %s\n", dot_path);
        return 1;
    }

    /* Parse */
    char *parse_err = NULL;
    DotGraph *graph = dot_parse(source, &parse_err);
    free(source);

    if (!graph) {
        fprintf(stderr, "Parse error: %s\n", parse_err ? parse_err : "unknown");
        free(parse_err);
        return 1;
    }

    printf("Parsed pipeline: %s (%zu nodes, %zu edges)\n",
           graph->name ? graph->name : "(unnamed)",
           graph->node_count, graph->edge_count);
    if (graph->goal)
        printf("Goal: %s\n", graph->goal);

    /* Apply transforms */
    transform_expand_variables(graph);
    transform_apply_stylesheet(graph);

    /* Validate */
    char *val_err = NULL;
    if (!validate_or_raise(graph, &val_err)) {
        fprintf(stderr, "Validation failed:\n%s\n", val_err);
        free(val_err);
        dot_graph_free(graph);
        return 1;
    }
    printf("Validation: OK\n");

    if (validate_only) {
        /* Print diagnostics summary */
        DiagnosticList dl = validate_graph(graph);
        int warnings = 0;
        for (size_t i = 0; i < dl.count; i++) {
            if (dl.items[i].severity == DIAG_WARNING) {
                warnings++;
                fprintf(stderr, "  WARNING [%s]: %s",
                        dl.items[i].rule, dl.items[i].message);
                if (dl.items[i].node_id)
                    fprintf(stderr, " (node: %s)", dl.items[i].node_id);
                fprintf(stderr, "\n");
            }
        }
        if (warnings)
            printf("%d warning(s)\n", warnings);
        diagnostic_list_free(&dl);
        dot_graph_free(graph);
        return 0;
    }

    /* Create logs directory */
    char logs_buf[512];
    if (!logs_dir) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        snprintf(logs_buf, sizeof(logs_buf),
                 "./attractor-run-%04d%02d%02d-%02d%02d%02d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
        logs_dir = logs_buf;
    }
    if(!io_mkdirs(logs_dir)) {fprintf(stderr,"Cannot create private logs directory\n");dot_graph_free(graph);return 1;}

    /* Initialize HTTP (for LLM calls) */
    if(!http_global_init()) {fprintf(stderr,"HTTP initialization failed\n");dot_graph_free(graph);return 1;}

    /* Create runner */
    PipelineRunner *runner = pipeline_runner_new(graph, logs_dir);
    if(!runner) {dot_graph_free(graph);http_global_cleanup();return 1;}
    pipeline_register_builtin_handlers(runner);

    if (verbose)
        pipeline_runner_on_event(runner, verbose_event_cb, NULL);

    /* Set up interviewer */
    if (auto_approve) {
        pipeline_runner_set_interviewer(runner, auto_approve_interviewer_new());
    } else {
        pipeline_runner_set_interviewer(runner, console_interviewer_new());
    }

    /* Set up codergen backend */
    LlmClient *llm = NULL;
    CodergenBackend *backend = NULL;
    AgentBackendState *backend_state = NULL;
    ExecutionEnv *exec_env = local_exec_env_new(NULL);
    if(!exec_env) {pipeline_runner_free(runner);dot_graph_free(graph);http_global_cleanup();return 1;}
    pipeline_runner_set_execution_env(runner,exec_env);

    if (!dry_run) {
        llm = llm_client_from_env();
        if (!llm) {fprintf(stderr,"Cannot initialize configured LLM providers\n");pipeline_runner_free(runner);dot_graph_free(graph);exec_env_free(exec_env);http_global_cleanup();return 1;}
        if (llm->provider_count == 0) {
            fprintf(stderr, "Warning: no LLM providers configured. Running in dry-run mode.\n");
            fprintf(stderr, "Set ANTHROPIC_API_KEY, OPENAI_API_KEY, or GEMINI_API_KEY.\n");
        } else {
            backend = mem_calloc(1, sizeof(CodergenBackend));
            backend_state = mem_calloc(1, sizeof(AgentBackendState));
            if(!backend || !backend_state) {free(backend);free(backend_state);pipeline_runner_free(runner);dot_graph_free(graph);exec_env_free(exec_env);llm_client_free(llm);http_global_cleanup();return 1;}
            backend_state->llm = llm;
            backend_state->exec_env = exec_env;
            /* Model/provider resolution: CLI override > graph default > auto-detect */
            backend_state->model = model_override ? model_override :
                        (graph->default_model ? graph->default_model :
                        ((llm->default_provider && strcmp(llm->default_provider, "anthropic") == 0)
                            ? "claude-sonnet-4-5" : "gpt-5.2-mini"));
            backend_state->provider = provider_override ? provider_override :
                           (graph->default_provider ? graph->default_provider :
                            llm->default_provider);
            backend->impl = backend_state;
            backend->run = agent_backend_run;
            pipeline_runner_set_backend(runner, backend);
        }
    }

    /* Execute or resume */
    printf("\nExecuting pipeline...\n\n");

    Outcome result;
    if (resume_path) {
        result = pipeline_resume(runner, resume_path);
    } else {
        result = pipeline_run(runner);
    }

    /* Report result */
    printf("\n=== Pipeline Result ===\n");
    const char *status_str;
    switch (result.status) {
        case STAGE_SUCCESS:         status_str = "SUCCESS"; break;
        case STAGE_PARTIAL_SUCCESS: status_str = "PARTIAL_SUCCESS"; break;
        case STAGE_RETRY:           status_str = "RETRY"; break;
        case STAGE_FAIL:            status_str = "FAIL"; break;
        case STAGE_SKIPPED:         status_str = "SKIPPED"; break;
        default: status_str="UNKNOWN";break;
    }
    printf("Status: %s\n", status_str);
    if (result.notes)
        printf("Notes: %s\n", result.notes);
    if (result.failure_reason)
        printf("Failure: %s\n", result.failure_reason);
    printf("Logs: %s\n", logs_dir);

    int exit_code = (result.status == STAGE_SUCCESS || result.status == STAGE_PARTIAL_SUCCESS) ? 0 : 1;

    /* Cleanup */
    outcome_free(&result);
    pipeline_runner_free(runner);
    dot_graph_free(graph); /* Runner borrows graph and backend. */
    free(backend_state);
    free(backend);
    if (exec_env) exec_env_free(exec_env);
    if (llm) llm_client_free(llm);
    http_global_cleanup();

    return exit_code;
}
