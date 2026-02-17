#include "agent/agent.h"
#include "util/str.h"
#include "util/json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fnmatch.h>
#include <uuid/uuid.h>

/*============================================================================
 * UUID helper
 *==========================================================================*/

static char *gen_uuid(void) {
    uuid_t u;
    uuid_generate(u);
    char *s = malloc(37);
    uuid_unparse_lower(u, s);
    return s;
}

/*============================================================================
 * Event helpers
 *==========================================================================*/

void agent_event_free(AgentEvent *ev) {
    if (!ev) return;
    free(ev->session_id);
    free(ev->data);
    free(ev->tool_name);
    free(ev->call_id);
    free(ev);
}

static void emit(AgentSession *s, AgentEventKind kind, const char *data,
                 const char *tool_name, const char *call_id) {
    if (!s->event_cb) return;
    AgentEvent ev = {
        .kind = kind,
        .session_id = s->id,  /* don't free, owned by session */
        .data = (char *)data,
        .tool_name = (char *)tool_name,
        .call_id = (char *)call_id,
    };
    s->event_cb(&ev, s->event_userdata);
}

/*============================================================================
 * Local Execution Environment
 *==========================================================================*/

typedef struct {
    char *working_dir;
} LocalEnvState;

static char *local_read_file(ExecutionEnv *self, const char *path, int offset, int limit) {
    (void)self;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    StrBuf sb;
    strbuf_init(&sb);
    char line[4096];
    int linenum = 0;
    int lines_read = 0;
    int start = offset > 0 ? offset : 1;
    int max_lines = limit > 0 ? limit : 2000;

    while (fgets(line, sizeof(line), f)) {
        linenum++;
        if (linenum < start) continue;
        if (lines_read >= max_lines) break;
        strbuf_appendf(&sb, "%4d\t%s", linenum, line);
        lines_read++;
    }
    fclose(f);
    return strbuf_detach(&sb);
}

static bool local_write_file(ExecutionEnv *self, const char *path, const char *content) {
    (void)self;
    /* Create parent directories */
    char *dir = str_dup(path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        /* Recursively create dirs */
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
        if (system(cmd) != 0) { /* ignore errors */ }
    }
    free(dir);

    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(content ? content : "", f);
    fclose(f);
    return true;
}

static bool local_file_exists(ExecutionEnv *self, const char *path) {
    (void)self;
    return access(path, F_OK) == 0;
}

static ExecResult *local_exec_command(ExecutionEnv *self, const char *cmd,
                                       int timeout_ms, const char *working_dir) {
    LocalEnvState *st = self->impl;
    ExecResult *r = calloc(1, sizeof(ExecResult));

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        r->exit_code = -1;
        r->stdout_buf = str_dup("Failed to create pipe");
        return r;
    }

    struct timeval start_tv;
    gettimeofday(&start_tv, NULL);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /* Set process group for killability */
        setpgid(0, 0);

        const char *wd = working_dir ? working_dir : st->working_dir;
        if (wd) chdir(wd);

        /* Filter sensitive env vars */
        unsetenv("ANTHROPIC_API_KEY");
        unsetenv("OPENAI_API_KEY");
        unsetenv("GEMINI_API_KEY");
        unsetenv("GOOGLE_API_KEY");

        execl("/bin/bash", "bash", "-c", cmd, NULL);
        _exit(127);
    }

    close(pipefd[1]);

    /* Read output with timeout */
    StrBuf output;
    strbuf_init(&output);
    char buf[4096];
    ssize_t n;

    /* Simple timeout: use alarm-style approach */
    if (timeout_ms <= 0) timeout_ms = 10000;

    /* Non-blocking read with timeout */
    fd_set fds;
    struct timeval tv;
    bool timed_out = false;

    while (1) {
        FD_ZERO(&fds);
        FD_SET(pipefd[0], &fds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int sel = select(pipefd[0] + 1, &fds, NULL, NULL, &tv);
        if (sel <= 0) {
            if (sel == 0) timed_out = true;
            break;
        }
        n = read(pipefd[0], buf, sizeof(buf) - 1);
        if (n <= 0) break;
        strbuf_append(&output, buf, (size_t)n);
    }
    close(pipefd[0]);

    if (timed_out) {
        kill(-pid, SIGTERM);
        usleep(2000000); /* 2 seconds */
        kill(-pid, SIGKILL);
        r->timed_out = true;
    }

    int status;
    waitpid(pid, &status, 0);
    r->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    r->stdout_buf = strbuf_detach(&output);

    struct timeval end_tv;
    gettimeofday(&end_tv, NULL);
    r->duration_ms = (int)((end_tv.tv_sec - start_tv.tv_sec) * 1000 +
                           (end_tv.tv_usec - start_tv.tv_usec) / 1000);

    return r;
}

static char *local_grep(ExecutionEnv *self, const char *pattern, const char *path,
                        const char *glob_filter, bool case_insensitive, int max_results) {
    (void)glob_filter;
    StrBuf cmd;
    strbuf_init(&cmd);
    strbuf_appendf(&cmd, "grep -rn %s", case_insensitive ? "-i " : "");
    if (max_results > 0) strbuf_appendf(&cmd, "-m %d ", max_results);
    strbuf_appendf(&cmd, "-- '%s' '%s' 2>/dev/null", pattern, path ? path : ".");

    ExecResult *r = local_exec_command(self, cmd.data, 10000, NULL);
    strbuf_free(&cmd);
    char *result = r->stdout_buf;
    r->stdout_buf = NULL;
    exec_result_free(r);
    return result;
}

static char *local_glob_fn(ExecutionEnv *self, const char *pattern, const char *path) {
    StrBuf cmd;
    strbuf_init(&cmd);
    LocalEnvState *st = self->impl;
    const char *base = path ? path : st->working_dir;
    strbuf_appendf(&cmd, "find '%s' -name '%s' -type f 2>/dev/null | head -500 | sort",
                   base ? base : ".", pattern);

    ExecResult *r = local_exec_command(self, cmd.data, 10000, NULL);
    strbuf_free(&cmd);
    char *result = r->stdout_buf;
    r->stdout_buf = NULL;
    exec_result_free(r);
    return result;
}

static char *local_working_directory(ExecutionEnv *self) {
    LocalEnvState *st = self->impl;
    return str_dup(st->working_dir);
}

static char *local_platform(ExecutionEnv *self) {
    (void)self;
#ifdef __APPLE__
    return str_dup("darwin");
#else
    return str_dup("linux");
#endif
}

static void local_cleanup(ExecutionEnv *self) {
    LocalEnvState *st = self->impl;
    free(st->working_dir);
    free(st);
}

ExecutionEnv *local_exec_env_new(const char *working_dir) {
    ExecutionEnv *env = calloc(1, sizeof(ExecutionEnv));
    LocalEnvState *st = calloc(1, sizeof(LocalEnvState));

    if (working_dir) {
        st->working_dir = str_dup(working_dir);
    } else {
        char buf[4096];
        if (getcwd(buf, sizeof(buf)))
            st->working_dir = str_dup(buf);
        else
            st->working_dir = str_dup(".");
    }

    env->impl = st;
    env->read_file = local_read_file;
    env->write_file = local_write_file;
    env->file_exists = local_file_exists;
    env->exec_command = local_exec_command;
    env->grep = local_grep;
    env->glob = local_glob_fn;
    env->working_directory = local_working_directory;
    env->platform = local_platform;
    env->cleanup = local_cleanup;
    return env;
}

void exec_env_free(ExecutionEnv *env) {
    if (!env) return;
    if (env->cleanup) env->cleanup(env);
    free(env);
}

void exec_result_free(ExecResult *r) {
    if (!r) return;
    free(r->stdout_buf);
    free(r->stderr_buf);
    free(r);
}

/*============================================================================
 * Tool Registry
 *==========================================================================*/

void tool_registry_init(ToolRegistry *reg) {
    memset(reg, 0, sizeof(*reg));
}

void tool_registry_free(ToolRegistry *reg) {
    for (size_t i = 0; i < reg->count; i++) {
        tool_definition_free(&reg->tools[i]->def);
        free(reg->tools[i]);
    }
    free(reg->tools);
}

void tool_registry_register(ToolRegistry *reg, RegisteredTool *tool) {
    /* Replace if exists */
    for (size_t i = 0; i < reg->count; i++) {
        if (str_eq(reg->tools[i]->def.name, tool->def.name)) {
            tool_definition_free(&reg->tools[i]->def);
            free(reg->tools[i]);
            reg->tools[i] = tool;
            return;
        }
    }
    if (reg->count >= reg->cap) {
        reg->cap = reg->cap ? reg->cap * 2 : 8;
        reg->tools = realloc(reg->tools, reg->cap * sizeof(RegisteredTool *));
    }
    reg->tools[reg->count++] = tool;
}

void tool_registry_unregister(ToolRegistry *reg, const char *name) {
    for (size_t i = 0; i < reg->count; i++) {
        if (str_eq(reg->tools[i]->def.name, name)) {
            tool_definition_free(&reg->tools[i]->def);
            free(reg->tools[i]);
            memmove(&reg->tools[i], &reg->tools[i+1], (reg->count - i - 1) * sizeof(RegisteredTool *));
            reg->count--;
            return;
        }
    }
}

RegisteredTool *tool_registry_get(const ToolRegistry *reg, const char *name) {
    for (size_t i = 0; i < reg->count; i++) {
        if (str_eq(reg->tools[i]->def.name, name))
            return reg->tools[i];
    }
    return NULL;
}

/*============================================================================
 * Built-in Tool Implementations
 *==========================================================================*/

static char *tool_read_file(const char *args_json, ExecutionEnv *env) {
    const char *err = NULL;
    JsonValue *args = json_parse(args_json, &err);
    if (!args) return str_dup("Error: invalid arguments");

    const char *path = json_get_string(args, "file_path");
    int offset = json_get_int(args, "offset", 0);
    int limit = json_get_int(args, "limit", 2000);

    if (!path) { json_free(args); return str_dup("Error: file_path required"); }

    char *content = env->read_file(env, path, offset, limit);
    json_free(args);
    return content ? content : str_dup("Error: file not found");
}

static char *tool_write_file(const char *args_json, ExecutionEnv *env) {
    const char *err = NULL;
    JsonValue *args = json_parse(args_json, &err);
    if (!args) return str_dup("Error: invalid arguments");

    const char *path = json_get_string(args, "file_path");
    const char *content = json_get_string(args, "content");

    if (!path) { json_free(args); return str_dup("Error: file_path required"); }

    bool ok = env->write_file(env, path, content ? content : "");
    json_free(args);
    return ok ? str_dup("File written successfully") : str_dup("Error: write failed");
}

static char *tool_edit_file(const char *args_json, ExecutionEnv *env) {
    const char *err = NULL;
    JsonValue *args = json_parse(args_json, &err);
    if (!args) return str_dup("Error: invalid arguments");

    const char *path = json_get_string(args, "file_path");
    const char *old_str = json_get_string(args, "old_string");
    const char *new_str = json_get_string(args, "new_string");
    bool replace_all = json_get_bool(args, "replace_all", false);

    if (!path || !old_str || !new_str) {
        json_free(args);
        return str_dup("Error: file_path, old_string, new_string required");
    }

    /* Read file */
    char *content = env->read_file(env, path, 0, 0);
    if (!content) { json_free(args); return str_dup("Error: file not found"); }

    /* Strip line numbers from read_file output */
    /* Actually, we need raw content for edit. Re-read without line numbers. */
    FILE *f = fopen(path, "r");
    if (!f) { free(content); json_free(args); return str_dup("Error: cannot read file"); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *raw = malloc((size_t)sz + 1);
    size_t read_sz = fread(raw, 1, (size_t)sz, f);
    raw[read_sz] = '\0';
    fclose(f);
    free(content);

    /* Find and replace */
    int count = 0;
    char *pos = strstr(raw, old_str);
    if (!pos) {
        free(raw);
        json_free(args);
        return str_dup("Error: old_string not found in file");
    }

    StrBuf result;
    strbuf_init(&result);
    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    char *cursor = raw;

    while (pos) {
        strbuf_append(&result, cursor, (size_t)(pos - cursor));
        strbuf_append(&result, new_str, new_len);
        cursor = pos + old_len;
        count++;
        if (!replace_all) break;
        pos = strstr(cursor, old_str);
    }
    strbuf_append_cstr(&result, cursor);

    char *new_content = strbuf_detach(&result);
    env->write_file(env, path, new_content);
    free(new_content);
    free(raw);
    json_free(args);

    char msg[128];
    snprintf(msg, sizeof(msg), "Replaced %d occurrence(s)", count);
    return str_dup(msg);
}

static char *tool_shell(const char *args_json, ExecutionEnv *env) {
    const char *err = NULL;
    JsonValue *args = json_parse(args_json, &err);
    if (!args) return str_dup("Error: invalid arguments");

    const char *command = json_get_string(args, "command");
    int timeout = json_get_int(args, "timeout_ms", 10000);

    if (!command) { json_free(args); return str_dup("Error: command required"); }

    ExecResult *r = env->exec_command(env, command, timeout, NULL);
    json_free(args);

    StrBuf sb;
    strbuf_init(&sb);
    if (r->stdout_buf) strbuf_append_cstr(&sb, r->stdout_buf);
    if (r->timed_out) {
        strbuf_appendf(&sb, "\n[ERROR: Command timed out after %dms]", timeout);
    }
    strbuf_appendf(&sb, "\n[exit code: %d, duration: %dms]", r->exit_code, r->duration_ms);
    exec_result_free(r);
    return strbuf_detach(&sb);
}

static char *tool_grep_fn(const char *args_json, ExecutionEnv *env) {
    const char *err = NULL;
    JsonValue *args = json_parse(args_json, &err);
    if (!args) return str_dup("Error: invalid arguments");

    const char *pattern = json_get_string(args, "pattern");
    const char *path = json_get_string(args, "path");
    bool ci = json_get_bool(args, "case_insensitive", false);
    int max = json_get_int(args, "max_results", 100);

    if (!pattern) { json_free(args); return str_dup("Error: pattern required"); }

    char *result = env->grep(env, pattern, path, NULL, ci, max);
    json_free(args);
    return result ? result : str_dup("");
}

static char *tool_glob_fn(const char *args_json, ExecutionEnv *env) {
    const char *err = NULL;
    JsonValue *args = json_parse(args_json, &err);
    if (!args) return str_dup("Error: invalid arguments");

    const char *pattern = json_get_string(args, "pattern");
    const char *path = json_get_string(args, "path");

    if (!pattern) { json_free(args); return str_dup("Error: pattern required"); }

    char *result = env->glob(env, pattern, path);
    json_free(args);
    return result ? result : str_dup("");
}

void agent_register_core_tools(ToolRegistry *reg) {
    /* read_file */
    {
        RegisteredTool *t = calloc(1, sizeof(RegisteredTool));
        t->def.name = str_dup("read_file");
        t->def.description = str_dup("Read a file from the filesystem. Returns line-numbered content.");
        t->def.parameters_json = str_dup(
            "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},"
            "\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}},"
            "\"required\":[\"file_path\"]}");
        t->execute = tool_read_file;
        tool_registry_register(reg, t);
    }
    /* write_file */
    {
        RegisteredTool *t = calloc(1, sizeof(RegisteredTool));
        t->def.name = str_dup("write_file");
        t->def.description = str_dup("Write content to a file. Creates parent directories if needed.");
        t->def.parameters_json = str_dup(
            "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},"
            "\"content\":{\"type\":\"string\"}},\"required\":[\"file_path\",\"content\"]}");
        t->execute = tool_write_file;
        tool_registry_register(reg, t);
    }
    /* edit_file */
    {
        RegisteredTool *t = calloc(1, sizeof(RegisteredTool));
        t->def.name = str_dup("edit_file");
        t->def.description = str_dup("Replace an exact string occurrence in a file.");
        t->def.parameters_json = str_dup(
            "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},"
            "\"old_string\":{\"type\":\"string\"},\"new_string\":{\"type\":\"string\"},"
            "\"replace_all\":{\"type\":\"boolean\"}},\"required\":[\"file_path\",\"old_string\",\"new_string\"]}");
        t->execute = tool_edit_file;
        tool_registry_register(reg, t);
    }
    /* shell */
    {
        RegisteredTool *t = calloc(1, sizeof(RegisteredTool));
        t->def.name = str_dup("shell");
        t->def.description = str_dup("Execute a shell command. Returns stdout, stderr, and exit code.");
        t->def.parameters_json = str_dup(
            "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},"
            "\"timeout_ms\":{\"type\":\"integer\"},\"description\":{\"type\":\"string\"}},"
            "\"required\":[\"command\"]}");
        t->execute = tool_shell;
        tool_registry_register(reg, t);
    }
    /* grep */
    {
        RegisteredTool *t = calloc(1, sizeof(RegisteredTool));
        t->def.name = str_dup("grep");
        t->def.description = str_dup("Search file contents using regex patterns.");
        t->def.parameters_json = str_dup(
            "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},"
            "\"path\":{\"type\":\"string\"},\"case_insensitive\":{\"type\":\"boolean\"},"
            "\"max_results\":{\"type\":\"integer\"}},\"required\":[\"pattern\"]}");
        t->execute = tool_grep_fn;
        tool_registry_register(reg, t);
    }
    /* glob */
    {
        RegisteredTool *t = calloc(1, sizeof(RegisteredTool));
        t->def.name = str_dup("glob");
        t->def.description = str_dup("Find files matching a glob pattern.");
        t->def.parameters_json = str_dup(
            "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},"
            "\"path\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}");
        t->execute = tool_glob_fn;
        tool_registry_register(reg, t);
    }
}

/*============================================================================
 * Provider Profiles
 *==========================================================================*/

static char *anthropic_system_prompt(const char *env_info, const char *project_docs) {
    StrBuf sb;
    strbuf_init(&sb);
    strbuf_append_cstr(&sb, "You are a coding agent. You help users with software engineering tasks.\n\n");
    strbuf_append_cstr(&sb, "# Tool Usage\n");
    strbuf_append_cstr(&sb, "- Read files before editing them\n");
    strbuf_append_cstr(&sb, "- Use edit_file with old_string/new_string for modifications\n");
    strbuf_append_cstr(&sb, "- Prefer editing existing files over creating new ones\n\n");
    if (env_info) { strbuf_append_cstr(&sb, env_info); strbuf_append_cstr(&sb, "\n\n"); }
    if (project_docs) { strbuf_append_cstr(&sb, project_docs); }
    return strbuf_detach(&sb);
}

ProviderProfile *anthropic_profile_new(const char *model) {
    ProviderProfile *p = calloc(1, sizeof(ProviderProfile));
    p->id = str_dup("anthropic");
    p->model = str_dup(model ? model : "claude-sonnet-4-5");
    p->supports_reasoning = true;
    p->supports_streaming = true;
    p->supports_parallel_tool_calls = false;
    p->context_window_size = 200000;
    p->build_system_prompt = anthropic_system_prompt;
    tool_registry_init(&p->tool_registry);
    agent_register_core_tools(&p->tool_registry);
    return p;
}

ProviderProfile *openai_profile_new(const char *model) {
    ProviderProfile *p = calloc(1, sizeof(ProviderProfile));
    p->id = str_dup("openai");
    p->model = str_dup(model ? model : "gpt-5.2");
    p->supports_reasoning = true;
    p->supports_streaming = true;
    p->supports_parallel_tool_calls = true;
    p->context_window_size = 1047576;
    p->build_system_prompt = anthropic_system_prompt; /* reuse for now */
    tool_registry_init(&p->tool_registry);
    agent_register_core_tools(&p->tool_registry);
    return p;
}

ProviderProfile *gemini_profile_new(const char *model) {
    ProviderProfile *p = calloc(1, sizeof(ProviderProfile));
    p->id = str_dup("gemini");
    p->model = str_dup(model ? model : "gemini-3-flash-preview");
    p->supports_reasoning = true;
    p->supports_streaming = true;
    p->supports_parallel_tool_calls = true;
    p->context_window_size = 1048576;
    p->build_system_prompt = anthropic_system_prompt;
    tool_registry_init(&p->tool_registry);
    agent_register_core_tools(&p->tool_registry);
    return p;
}

void provider_profile_free(ProviderProfile *p) {
    if (!p) return;
    free(p->id);
    free(p->model);
    tool_registry_free(&p->tool_registry);
    free(p);
}

/*============================================================================
 * Session
 *==========================================================================*/

SessionConfig agent_default_config(void) {
    return (SessionConfig){
        .max_turns = 0,
        .max_tool_rounds_per_input = 0,
        .default_command_timeout_ms = 10000,
        .max_command_timeout_ms = 600000,
        .reasoning_effort = REASONING_NONE,
        .enable_loop_detection = true,
        .loop_detection_window = 10,
        .max_subagent_depth = 1,
    };
}

AgentSession *agent_session_new(ProviderProfile *profile, ExecutionEnv *env,
                                 LlmClient *client, SessionConfig config) {
    AgentSession *s = calloc(1, sizeof(AgentSession));
    s->id = gen_uuid();
    s->profile = profile;
    s->exec_env = env;
    s->llm_client = client;
    s->config = config;
    s->state = SESSION_IDLE;
    s->history_cap = 64;
    s->history = calloc(s->history_cap, sizeof(Message *));
    return s;
}

void agent_session_free(AgentSession *s) {
    if (!s) return;
    free(s->id);
    for (size_t i = 0; i < s->history_count; i++)
        message_free(s->history[i]);
    free(s->history);
    for (size_t i = 0; i < s->steering_count; i++)
        free(s->steering_queue[i]);
    free(s->steering_queue);
    for (size_t i = 0; i < s->followup_count; i++)
        free(s->followup_queue[i]);
    free(s->followup_queue);
    free(s);
}

void agent_session_on_event(AgentSession *s, AgentEventCallback cb, void *userdata) {
    s->event_cb = cb;
    s->event_userdata = userdata;
}

void agent_session_steer(AgentSession *s, const char *message) {
    s->steering_queue = realloc(s->steering_queue, (s->steering_count + 1) * sizeof(char *));
    s->steering_queue[s->steering_count++] = str_dup(message);
}

void agent_session_follow_up(AgentSession *s, const char *message) {
    s->followup_queue = realloc(s->followup_queue, (s->followup_count + 1) * sizeof(char *));
    s->followup_queue[s->followup_count++] = str_dup(message);
}

void agent_session_abort(AgentSession *s) {
    s->abort_signaled = true;
}

static void session_add_message(AgentSession *s, Message *m) {
    if (s->history_count >= s->history_cap) {
        s->history_cap *= 2;
        s->history = realloc(s->history, s->history_cap * sizeof(Message *));
    }
    s->history[s->history_count++] = m;
}

static void drain_steering(AgentSession *s) {
    for (size_t i = 0; i < s->steering_count; i++) {
        session_add_message(s, message_user(s->steering_queue[i]));
        emit(s, AGENT_EVT_STEERING_INJECTED, s->steering_queue[i], NULL, NULL);
        free(s->steering_queue[i]);
    }
    s->steering_count = 0;
}

/* Simple loop detection: check if last N tool call names repeat */
static bool detect_loop(AgentSession *s) {
    int window = s->config.loop_detection_window;
    if (window <= 0 || (int)s->history_count < window) return false;

    /* Extract recent tool call names from history */
    char **calls = NULL;
    int call_count = 0;
    for (int i = (int)s->history_count - 1; i >= 0 && call_count < window; i--) {
        Message *m = s->history[i];
        if (m->role == ROLE_ASSISTANT) {
            for (size_t j = 0; j < m->part_count; j++) {
                if (m->parts[j]->kind == CONTENT_TOOL_CALL && m->parts[j]->tool_call) {
                    calls = realloc(calls, (size_t)(call_count + 1) * sizeof(char *));
                    calls[call_count++] = m->parts[j]->tool_call->name;
                    if (call_count >= window) break;
                }
            }
        }
    }

    bool detected = false;
    if (call_count >= window) {
        /* Check for pattern of length 1 */
        bool all_same = true;
        for (int i = 1; i < call_count; i++) {
            if (!str_eq(calls[0], calls[i])) { all_same = false; break; }
        }
        detected = all_same;
    }

    free(calls);
    return detected;
}

/*============================================================================
 * Core Agentic Loop
 *==========================================================================*/

void agent_session_submit(AgentSession *s, const char *input) {
    s->state = SESSION_PROCESSING;
    session_add_message(s, message_user(input));
    emit(s, AGENT_EVT_USER_INPUT, input, NULL, NULL);

    drain_steering(s);

    /* Build tool definitions */
    ToolRegistry *reg = &s->profile->tool_registry;
    ToolDefinition **tool_defs = calloc(reg->count, sizeof(ToolDefinition *));
    for (size_t i = 0; i < reg->count; i++) {
        tool_defs[i] = calloc(1, sizeof(ToolDefinition));
        tool_defs[i]->name = str_dup(reg->tools[i]->def.name);
        tool_defs[i]->description = str_dup(reg->tools[i]->def.description);
        tool_defs[i]->parameters_json = str_dup(reg->tools[i]->def.parameters_json);
    }

    int round_count = 0;

    while (1) {
        /* Check limits */
        if (s->config.max_tool_rounds_per_input > 0 && round_count >= s->config.max_tool_rounds_per_input) {
            emit(s, AGENT_EVT_TURN_LIMIT, "Round limit reached", NULL, NULL);
            break;
        }
        if (s->abort_signaled) break;

        /* Build system prompt */
        char *wd = s->exec_env->working_directory(s->exec_env);
        char *plat = s->exec_env->platform(s->exec_env);
        char env_info[1024];
        snprintf(env_info, sizeof(env_info),
                 "<environment>\nWorking directory: %s\nPlatform: %s\nModel: %s\n</environment>",
                 wd, plat, s->profile->model);
        free(wd); free(plat);

        char *sys_prompt = s->profile->build_system_prompt(env_info, NULL);

        /* Build request */
        size_t msg_count = s->history_count + 1;
        Message **msgs = calloc(msg_count, sizeof(Message *));
        msgs[0] = message_system(sys_prompt);
        for (size_t i = 0; i < s->history_count; i++)
            msgs[i + 1] = s->history[i];  /* shallow ref */

        LlmRequest req = {
            .model = str_dup(s->profile->model),
            .messages = msgs,
            .message_count = msg_count,
            .provider = str_dup(s->profile->id),
            .tools = tool_defs,
            .tool_count = reg->count,
            .temperature = -1,
            .top_p = -1,
            .reasoning_effort = s->config.reasoning_effort,
        };

        LlmError err = {0};
        LlmResponse *resp = llm_client_complete(s->llm_client, &req, &err);

        free(req.model);
        free(req.provider);
        message_free(msgs[0]); /* free system message */
        free(msgs);
        free(sys_prompt);

        if (!resp) {
            emit(s, AGENT_EVT_ERROR, err.message ? err.message : "LLM call failed", NULL, NULL);
            llm_error_free(&err);
            break;
        }

        /* Record assistant turn */
        /* Clone the response message into history */
        Message *asst_msg = calloc(1, sizeof(Message));
        asst_msg->role = ROLE_ASSISTANT;
        asst_msg->parts = calloc(resp->message->part_count, sizeof(ContentPart *));
        asst_msg->part_count = resp->message->part_count;
        for (size_t i = 0; i < resp->message->part_count; i++) {
            ContentPart *src = resp->message->parts[i];
            ContentPart *dst = calloc(1, sizeof(ContentPart));
            dst->kind = src->kind;
            if (src->text) dst->text = str_dup(src->text);
            if (src->tool_call) {
                dst->tool_call = calloc(1, sizeof(ToolCallData));
                dst->tool_call->id = str_dup(src->tool_call->id);
                dst->tool_call->name = str_dup(src->tool_call->name);
                dst->tool_call->arguments_json = str_dup(src->tool_call->arguments_json);
            }
            if (src->thinking) {
                dst->thinking = calloc(1, sizeof(ThinkingData));
                dst->thinking->text = str_dup(src->thinking->text);
                dst->thinking->signature = str_dup(src->thinking->signature);
                dst->thinking->redacted = src->thinking->redacted;
            }
            asst_msg->parts[i] = dst;
        }
        session_add_message(s, asst_msg);

        emit(s, AGENT_EVT_ASSISTANT_TEXT_END, resp->text, NULL, NULL);

        /* If no tool calls, done */
        if (resp->tool_call_count == 0) {
            llm_response_free(resp);
            break;
        }

        /* Execute tool calls */
        round_count++;
        for (size_t i = 0; i < resp->tool_call_count; i++) {
            ToolCall *tc = resp->tool_calls[i];
            emit(s, AGENT_EVT_TOOL_CALL_START, NULL, tc->name, tc->id);

            RegisteredTool *rtool = tool_registry_get(reg, tc->name);
            char *result_str = NULL;
            bool is_error = false;

            if (rtool && rtool->execute) {
                result_str = rtool->execute(tc->arguments_json, s->exec_env);
                if (!result_str) {
                    result_str = str_dup("Tool execution error");
                    is_error = true;
                }
            } else {
                char buf[256];
                snprintf(buf, sizeof(buf), "Unknown tool: %s", tc->name);
                result_str = str_dup(buf);
                is_error = true;
            }

            /* Truncate output for LLM (keep full for event) */
            emit(s, AGENT_EVT_TOOL_CALL_END, result_str, tc->name, tc->id);

            /* Truncate to 50000 chars for the LLM */
            if (strlen(result_str) > 50000) {
                char *truncated = malloc(50001 + 200);
                memcpy(truncated, result_str, 25000);
                int removed = (int)(strlen(result_str) - 50000);
                int offset_pos = sprintf(truncated + 25000,
                    "\n\n[WARNING: Tool output truncated. %d chars removed.]\n\n", removed);
                memcpy(truncated + 25000 + offset_pos, result_str + strlen(result_str) - 25000, 25000);
                truncated[50000 + (size_t)offset_pos] = '\0';
                free(result_str);
                result_str = truncated;
            }

            session_add_message(s, message_tool_result(tc->id, result_str, is_error));
            free(result_str);
        }

        llm_response_free(resp);

        /* Drain steering between rounds */
        drain_steering(s);

        /* Loop detection */
        if (s->config.enable_loop_detection && detect_loop(s)) {
            const char *warning = "Loop detected: recent tool calls follow a repeating pattern. Try a different approach.";
            session_add_message(s, message_user(warning));
            emit(s, AGENT_EVT_LOOP_DETECTION, warning, NULL, NULL);
        }
    }

    /* Clean up tool defs */
    for (size_t i = 0; i < reg->count; i++) {
        tool_definition_free(tool_defs[i]);
        free(tool_defs[i]);
    }
    free(tool_defs);

    /* Process follow-up queue */
    if (s->followup_count > 0) {
        char *next = s->followup_queue[0];
        memmove(&s->followup_queue[0], &s->followup_queue[1],
                (s->followup_count - 1) * sizeof(char *));
        s->followup_count--;
        agent_session_submit(s, next);
        free(next);
        return;
    }

    s->state = SESSION_IDLE;
    emit(s, AGENT_EVT_SESSION_END, NULL, NULL, NULL);
}
