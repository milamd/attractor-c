#include "attractor/dot_parser.h"
#include "attractor/validator.h"
#include "attractor/engine.h"
#include "llm/client.h"
#include "agent/agent.h"
#include "util/str.h"
#include "util/json.h"
#include "util/http.h"
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

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

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

/*--- Tool wrappers: bridge ActiveTool (no env) to RegisteredTool (env) ---*/

static ExecutionEnv *g_tool_env = NULL;  /* set before each llm_generate call */

static char *active_read_file(const char *args_json) {
    if (!g_tool_env || !g_tool_env->read_file) return str_dup("Error: no execution environment");
    const char *parse_err = NULL;
    JsonValue *args = json_parse(args_json, &parse_err);
    if (!args) return str_dup("Error: invalid arguments");
    const char *path = json_get_string(args, "file_path");
    if (!path) path = "";
    int offset = (int)json_get_number(args, "offset", 0);
    int limit  = (int)json_get_number(args, "limit", 2000);
    char *content = g_tool_env->read_file(g_tool_env, path, offset, limit);
    json_free(args);
    return content ? content : str_dup("Error: could not read file");
}

static char *active_write_file(const char *args_json) {
    if (!g_tool_env || !g_tool_env->write_file) return str_dup("Error: no execution environment");
    const char *parse_err = NULL;
    JsonValue *args = json_parse(args_json, &parse_err);
    if (!args) return str_dup("Error: invalid arguments");
    const char *path    = json_get_string(args, "file_path");
    const char *content = json_get_string(args, "content");
    if (!path) path = "";
    if (!content) content = "";
    bool ok = g_tool_env->write_file(g_tool_env, path, content);
    json_free(args);
    return str_dup(ok ? "File written successfully" : "Error: could not write file");
}

static char *active_grep(const char *args_json) {
    if (!g_tool_env || !g_tool_env->grep) return str_dup("Error: no execution environment");
    const char *parse_err = NULL;
    JsonValue *args = json_parse(args_json, &parse_err);
    if (!args) return str_dup("Error: invalid arguments");
    const char *pattern = json_get_string(args, "pattern");
    const char *path    = json_get_string(args, "path");
    const char *glob_f  = json_get_string(args, "glob");
    if (!pattern) pattern = "";
    if (!path) path = ".";
    bool ci = json_get_bool(args, "case_insensitive", false);
    int max  = (int)json_get_number(args, "max_results", 50);
    char *result = g_tool_env->grep(g_tool_env, pattern, path, glob_f, ci, max);
    json_free(args);
    return result ? result : str_dup("No matches found");
}

static char *active_glob(const char *args_json) {
    if (!g_tool_env || !g_tool_env->glob) return str_dup("Error: no execution environment");
    const char *parse_err = NULL;
    JsonValue *args = json_parse(args_json, &parse_err);
    if (!args) return str_dup("Error: invalid arguments");
    const char *pattern = json_get_string(args, "pattern");
    const char *path    = json_get_string(args, "path");
    if (!pattern) pattern = "";
    if (!path) path = ".";
    char *result = g_tool_env->glob(g_tool_env, pattern, path);
    json_free(args);
    return result ? result : str_dup("No matches found");
}

static char *active_shell(const char *args_json) {
    if (!g_tool_env || !g_tool_env->exec_command) return str_dup("Error: no execution environment");
    const char *parse_err = NULL;
    JsonValue *args = json_parse(args_json, &parse_err);
    if (!args) return str_dup("Error: invalid arguments");
    const char *cmd = json_get_string(args, "command");
    if (!cmd) cmd = "";
    int timeout = (int)json_get_number(args, "timeout_ms", 30000);
    char *cwd = g_tool_env->working_directory ? g_tool_env->working_directory(g_tool_env) : NULL;
    ExecResult *er = g_tool_env->exec_command(g_tool_env, cmd, timeout, cwd);
    free(cwd);
    json_free(args);
    if (!er) return str_dup("Error: command execution failed");
    /* Format output */
    StrBuf sb;
    strbuf_init(&sb);
    if (er->stdout_buf && *er->stdout_buf)
        strbuf_append_cstr(&sb, er->stdout_buf);
    if (er->stderr_buf && *er->stderr_buf) {
        strbuf_append_cstr(&sb, "\n[stderr] ");
        strbuf_append_cstr(&sb, er->stderr_buf);
    }
    if (er->exit_code != 0)
        strbuf_appendf(&sb, "\n[exit_code=%d]", er->exit_code);
    if (er->timed_out)
        strbuf_append_cstr(&sb, "\n[timed out]");
    exec_result_free(er);
    return strbuf_detach(&sb);
}

/* ActiveTool definitions for llm_generate */
static ActiveTool g_active_tools[] = {
    {
        .def = { .name = "read_file",
                 .description = "Read a file from the filesystem. Returns line-numbered content.",
                 .parameters_json = "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"Absolute path to the file\"},\"offset\":{\"type\":\"integer\",\"description\":\"Line offset (0-based)\"},\"limit\":{\"type\":\"integer\",\"description\":\"Max lines to read (default 2000)\"}},\"required\":[\"file_path\"]}" },
        .execute = active_read_file
    },
    {
        .def = { .name = "write_file",
                 .description = "Write content to a file. Creates parent directories if needed.",
                 .parameters_json = "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"Absolute path to the file\"},\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}},\"required\":[\"file_path\",\"content\"]}" },
        .execute = active_write_file
    },
    {
        .def = { .name = "grep",
                 .description = "Search file contents for a regex pattern. Returns matching lines with file paths and line numbers.",
                 .parameters_json = "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Regex pattern to search for\"},\"path\":{\"type\":\"string\",\"description\":\"Directory or file to search in\"},\"glob\":{\"type\":\"string\",\"description\":\"Glob filter for filenames (e.g. *.c)\"},\"case_insensitive\":{\"type\":\"boolean\"},\"max_results\":{\"type\":\"integer\"}},\"required\":[\"pattern\"]}" },
        .execute = active_grep
    },
    {
        .def = { .name = "glob",
                 .description = "Find files matching a glob pattern. Returns list of matching file paths.",
                 .parameters_json = "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Glob pattern (e.g. **/*.c)\"},\"path\":{\"type\":\"string\",\"description\":\"Base directory to search from\"}},\"required\":[\"pattern\"]}" },
        .execute = active_glob
    },
    {
        .def = { .name = "shell",
                 .description = "Execute a shell command. Returns stdout, stderr, and exit code.",
                 .parameters_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute\"},\"timeout_ms\":{\"type\":\"integer\",\"description\":\"Timeout in milliseconds (default 30000)\"}},\"required\":[\"command\"]}" },
        .execute = active_shell
    }
};

#define ACTIVE_TOOL_COUNT (sizeof(g_active_tools) / sizeof(g_active_tools[0]))

static char *agent_backend_run(CodergenBackend *self, const DotNode *node,
                                const char *prompt, const PipelineContext *ctx) {
    AgentBackendState *st = self->impl;
    (void)ctx;

    /* Use node's model/provider if set, else fall back to runner defaults */
    const char *model    = (node && node->llm_model)    ? node->llm_model    : st->model;
    const char *provider = (node && node->llm_provider)  ? node->llm_provider  : st->provider;
    ReasoningEffort effort = (node && node->reasoning_effort)
        ? parse_reasoning_effort(node->reasoning_effort) : REASONING_HIGH;

    /* Set global env for tool wrappers */
    g_tool_env = st->exec_env;

    LlmError err = {0};
    GenerateResult *result = llm_generate(
        st->llm, model, prompt,
        NULL, 0,      /* messages */
        NULL,          /* system_prompt */
        g_active_tools, ACTIVE_TOOL_COUNT,
        50,            /* max_tool_rounds */
        effort,
        provider,
        2,             /* max_retries */
        &err
    );

    g_tool_env = NULL;  /* clear after use */

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
    mkdir(logs_dir, 0755);

    /* Initialize HTTP (for LLM calls) */
    http_global_init();

    /* Create runner */
    PipelineRunner *runner = pipeline_runner_new(graph, logs_dir);
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
    ExecutionEnv *exec_env = NULL;

    if (!dry_run) {
        llm = llm_client_from_env();
        if (llm->provider_count == 0) {
            fprintf(stderr, "Warning: no LLM providers configured. Running in dry-run mode.\n");
            fprintf(stderr, "Set ANTHROPIC_API_KEY, OPENAI_API_KEY, or GEMINI_API_KEY.\n");
        } else {
            /* Create execution environment rooted at cwd */
            char cwd[4096];
            if (getcwd(cwd, sizeof(cwd)))
                exec_env = local_exec_env_new(cwd);

            backend = calloc(1, sizeof(CodergenBackend));
            backend_state = calloc(1, sizeof(AgentBackendState));
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
    const char *status_str = "unknown";
    switch (result.status) {
        case STAGE_SUCCESS:         status_str = "SUCCESS"; break;
        case STAGE_PARTIAL_SUCCESS: status_str = "PARTIAL_SUCCESS"; break;
        case STAGE_RETRY:           status_str = "RETRY"; break;
        case STAGE_FAIL:            status_str = "FAIL"; break;
        case STAGE_SKIPPED:         status_str = "SKIPPED"; break;
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
    /* Note: graph is freed by runner; backend is not owned by runner */
    free(backend_state);
    free(backend);
    if (exec_env) exec_env_free(exec_env);
    if (llm) llm_client_free(llm);
    http_global_cleanup();

    return exit_code;
}
