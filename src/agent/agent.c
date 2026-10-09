#include "agent/agent.h"
#include "util/str.h"
#include "util/json.h"
#include "util/io.h"
#include "util/mem.h"
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
#include <regex.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <fcntl.h>
#include <uuid/uuid.h>

/*============================================================================
 * UUID helper
 *==========================================================================*/

static char *gen_uuid(void) {
    uuid_t u;
    uuid_generate(u);
    char *s = mem_alloc(37);
    if(!s) return NULL;
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
    bool contained;
} LocalEnvState;

static char *local_read_raw(ExecutionEnv *self,const char *path);
static char *local_read_file(ExecutionEnv *self,const char *path,int offset,int limit) {
    char *raw=local_read_raw(self,path);if(!raw) return NULL;
    StrBuf b;strbuf_init_limit(&b,ATTRACTOR_OUTPUT_LIMIT);
    size_t start=offset>0?(size_t)offset:1,maximum=limit>0?(size_t)limit:2000,line=1,printed=0;
    for(char *cursor=raw;*cursor && printed<maximum;line++) {
        char *end=strchr(cursor,'\n');size_t length=end?(size_t)(end-cursor)+1:strlen(cursor);
        if(line>=start) {strbuf_appendf(&b,"%4zu\t",line);strbuf_append(&b,cursor,length);printed++;}
        cursor+=length;
    }
    free(raw);return strbuf_detach(&b);
}

static char *local_read_raw(ExecutionEnv *self,const char *path) {
    LocalEnvState *st=self->impl;
    int fd=io_open_workspace(st->working_dir,path,O_RDONLY,0,st->contained);if(fd<0) return NULL;
    FILE *f=fdopen(fd,"rb");if(!f) {close(fd);return NULL;}
    size_t length=0;char *data=io_read_stream(f,ATTRACTOR_INPUT_LIMIT,&length);
    if(fclose(f)!=0 || (data && memchr(data,0,length))) {free(data);return NULL;}return data;
}
static bool local_write_file(ExecutionEnv *self,const char *path,const char *content) {
    LocalEnvState *st=self->impl;
    int fd=io_open_workspace(st->working_dir,path,O_WRONLY|O_CREAT|O_TRUNC,0600,st->contained);if(fd<0) return false;
    FILE *f=fdopen(fd,"w");if(!f) {close(fd);return false;}
    bool ok=fputs(content?content:"",f)>=0;
    if(fclose(f)!=0) ok=false;return ok;
}
static bool local_file_exists(ExecutionEnv *self,const char *path) {
    LocalEnvState *st=self->impl;
    int fd=io_open_workspace(st->working_dir,path,O_RDONLY,0,st->contained);if(fd<0) return false;close(fd);return true;
}

static ExecResult *local_exec_command(ExecutionEnv *self,const char *cmd,int timeout_ms,const char *working_dir) {
    LocalEnvState *st=self->impl;StrBuf path;strbuf_init(&path);
    if(working_dir && working_dir[0]=='/') strbuf_append_cstr(&path,working_dir);
    else if(working_dir) strbuf_appendf(&path,"%s/%s",st->working_dir,working_dir);
    else strbuf_append_cstr(&path,st->working_dir);
    char *directory=strbuf_detach(&path);
    const char *argv[]={"/bin/sh","-c",cmd,NULL};
    ExecResult *result=directory?process_run(&(ProcessOptions){.argv=argv,.working_dir=directory,.timeout_ms=timeout_ms,.cancel=self->cancel,.extra_env=self->extra_env}):NULL;
    free(directory);return result;
}
typedef struct {
    const char *pattern,*filter;regex_t regex;bool grep;size_t visited,found,maximum;StrBuf output;
} Search;
static bool search_fd(Search *search,int fd,const char *name,unsigned depth) {
    if(depth>64 || ++search->visited>10000) return false;
    struct stat st;if(fstat(fd,&st)!=0) return false;
    if(S_ISDIR(st.st_mode)) {
        int copy=dup(fd);if(copy<0) return false;DIR *dir=fdopendir(copy);if(!dir) {close(copy);return false;}
        bool ok=true;struct dirent *entry;
        while(search->found<search->maximum) {
            errno=0;entry=readdir(dir);if(!entry) {if(errno) ok=false;break;}
            if(str_eq(entry->d_name,".") || str_eq(entry->d_name,"..")) continue;
            struct stat child_stat;
            if(fstatat(fd,entry->d_name,&child_stat,AT_SYMLINK_NOFOLLOW)!=0) {ok=false;break;}
            if(!S_ISDIR(child_stat.st_mode) && !S_ISREG(child_stat.st_mode)) continue;
            int child=openat(fd,entry->d_name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
            StrBuf path;strbuf_init(&path);strbuf_appendf(&path,"%s/%s",name,entry->d_name);char *label=strbuf_detach(&path);
            if(child<0 || !label || !search_fd(search,child,label,depth+1)) ok=false;
            if(child>=0) close(child);free(label);if(!ok) break;
        }
        if(closedir(dir)!=0) ok=false;return ok;
    }
    if(!S_ISREG(st.st_mode)) return true;
    const char *base=strrchr(name,'/');base=base?base+1:name;
    if(search->filter && fnmatch(search->filter,base,0)!=0) return true;
    if(!search->grep) {
        if(fnmatch(search->pattern,strchr(search->pattern,'/')?name:base,0)==0) {strbuf_appendf(&search->output,"%s\n",name);search->found++;}
        return !search->output.failed;
    }
    int copy=dup(fd);if(copy<0) return false;FILE *file=fdopen(copy,"rb");if(!file) {close(copy);return false;}
    size_t length=0;char *raw=io_read_stream(file,ATTRACTOR_INPUT_LIMIT,&length);bool ok=fclose(file)==0 && raw;
    if(!ok) {free(raw);return false;}
    if(memchr(raw,0,length)) {free(raw);return true;} /* Binary files have no text matches. */
    size_t line=1;
    for(char *cursor=raw;*cursor && search->found<search->maximum;line++) {
        char *end=strchr(cursor,'\n');if(end) *end=0;
        int rc=regexec(&search->regex,cursor,0,NULL,0);
        if(rc==0) {strbuf_appendf(&search->output,"%s:%zu:%s\n",name,line,cursor);search->found++;}
        else if(rc!=REG_NOMATCH) {ok=false;break;}
        if(!end) break;cursor=end+1;
    }
    free(raw);return ok && !search->output.failed;
}
static char *local_search(ExecutionEnv *self,const char *pattern,const char *path,const char *filter,bool ci,int maximum,bool grep) {
    if(!pattern) return NULL;LocalEnvState *st=self->impl;
    Search search={.pattern=pattern,.filter=filter,.grep=grep,.maximum=maximum>0?(size_t)maximum:500};
    if(search.maximum>10000) search.maximum=10000;
    if(grep && regcomp(&search.regex,pattern,REG_NOSUB|(ci?REG_ICASE:0))!=0) return NULL;
    strbuf_init_limit(&search.output,ATTRACTOR_OUTPUT_LIMIT);
    int fd=io_open_workspace(st->working_dir,path?path:".",O_RDONLY|O_NONBLOCK,0,st->contained);
    bool ok=fd>=0 && search_fd(&search,fd,path?path:".",0);
    if(fd>=0) close(fd);if(grep) regfree(&search.regex);
    if(!ok) {strbuf_free(&search.output);return NULL;}return strbuf_detach(&search.output);
}
static char *local_grep(ExecutionEnv *self,const char *pattern,const char *path,const char *filter,bool ci,int maximum) {
    return local_search(self,pattern,path,filter,ci,maximum,true);
}
static char *local_glob_fn(ExecutionEnv *self,const char *pattern,const char *path) {
    return local_search(self,pattern,path,NULL,false,500,false);
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

ExecutionEnv *local_exec_env_new_policy(const char *working_dir, bool contained) {
    ExecutionEnv *env = mem_calloc(1, sizeof(ExecutionEnv));
    LocalEnvState *st = mem_calloc(1, sizeof(LocalEnvState));
    if(!env || !st) {free(env);free(st);return NULL;}

    if (working_dir) {
        st->working_dir = str_dup(working_dir);
    } else {
        char buf[4096];
        if (getcwd(buf, sizeof(buf)))
            st->working_dir = str_dup(buf);
        else
            st->working_dir = str_dup(".");
    }

    char *canonical=st->working_dir?realpath(st->working_dir,NULL):NULL;
    if(!canonical) {free(st->working_dir);free(st);free(env);return NULL;}
    free(st->working_dir);st->working_dir=canonical;st->contained=contained;
    env->impl = st;
    env->read_raw = local_read_raw;
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

ExecutionEnv *local_exec_env_new(const char *working_dir) {return local_exec_env_new_policy(working_dir,false);}

void exec_env_free(ExecutionEnv *env) {
    if (!env) return;
    if (env->cleanup) env->cleanup(env);
    free(env);
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
    if(!tool || !tool->def.name || !tool->def.description || !tool->def.parameters_json || !tool->execute) {
        reg->failed=true;if(tool) {tool_definition_free(&tool->def);free(tool);}return;
    }
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
        size_t cap=reg->cap?reg->cap*2:8;
        RegisteredTool **tools=mem_reallocarray(reg->tools,cap,sizeof(*tools));
        if(!tools) {reg->failed=true;tool_definition_free(&tool->def);free(tool);return;}reg->tools=tools;reg->cap=cap;
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

static char *tool_error(bool *is_error,const char *message) {if(is_error) *is_error=true;return str_dup(message);}
static char *tool_read_file(const char *args_json, ExecutionEnv *env,bool *is_error) {
    if(is_error) *is_error=false;
    if(!env || !env->read_file) return tool_error(is_error,"Error: file reads unsupported");
    const char *err = NULL;
    JsonValue *args = json_parse(args_json, &err);
    if (!args) return tool_error(is_error,"Error: invalid arguments");

    const char *path = json_get_string(args, "file_path");
    int offset = json_get_int(args, "offset", 0);
    int limit = json_get_int(args, "limit", 2000);

    if (!path) { json_free(args); return tool_error(is_error,"Error: file_path required"); }

    char *content = env->read_file(env, path, offset, limit);
    json_free(args);
    return content ? content : tool_error(is_error,"Error: file not found");
}

static char *tool_write_file(const char *args_json, ExecutionEnv *env,bool *is_error) {
    if(is_error) *is_error=false;
    if(!env || !env->write_file) return tool_error(is_error,"Error: file writes unsupported");
    const char *err = NULL;
    JsonValue *args = json_parse(args_json, &err);
    if (!args) return tool_error(is_error,"Error: invalid arguments");

    const char *path = json_get_string(args, "file_path");
    const char *content = json_get_string(args, "content");

    if (!path) { json_free(args); return tool_error(is_error,"Error: file_path required"); }

    bool ok = env->write_file(env, path, content ? content : "");
    json_free(args);
    return ok ? str_dup("File written successfully") : tool_error(is_error,"Error: write failed");
}

static char *tool_edit_file(const char *args_json, ExecutionEnv *env,bool *is_error) {
    if(is_error) *is_error=false;
    if(!env || !env->read_raw || !env->write_file) return tool_error(is_error,"Error: raw reads or file writes unsupported");
    const char *err = NULL;
    JsonValue *args = json_parse(args_json, &err);
    if (!args) return tool_error(is_error,"Error: invalid arguments");

    const char *path = json_get_string(args, "file_path");
    const char *old_str = json_get_string(args, "old_string");
    const char *new_str = json_get_string(args, "new_string");
    bool replace_all = json_get_bool(args, "replace_all", false);

    if (!path || !old_str || !new_str) {
        json_free(args);
        return tool_error(is_error,"Error: file_path, old_string, new_string required");
    }

    if (!*old_str) { json_free(args); return tool_error(is_error,"Error: old_string must not be empty"); }
    if(!env->read_raw) {json_free(args);return tool_error(is_error,"Error: raw reads unsupported");}
    char *raw=env->read_raw(env,path);
    if(!raw) {json_free(args);return tool_error(is_error,"Error: cannot read file");}

    /* Find and replace */
    int count = 0;
    char *pos = strstr(raw, old_str);
    if (!pos) {
        free(raw);
        json_free(args);
        return tool_error(is_error,"Error: old_string not found in file");
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
    bool written=new_content && env->write_file(env, path, new_content);
    free(new_content);
    free(raw);
    json_free(args);

    if(!written) return tool_error(is_error,"Error: write failed");
    char msg[128];
    snprintf(msg, sizeof(msg), "Replaced %d occurrence(s)", count);
    return str_dup(msg);
}

static char *tool_shell(const char *args_json, ExecutionEnv *env,bool *is_error) {
    if(is_error) *is_error=false;
    if(!env || !env->exec_command) return tool_error(is_error,"Error: command execution unsupported");
    const char *err = NULL;
    JsonValue *args = json_parse(args_json, &err);
    if (!args) return tool_error(is_error,"Error: invalid arguments");

    const char *command = json_get_string(args, "command");
    int timeout = json_get_int(args, "timeout_ms", 10000);

    if (!command) { json_free(args); return tool_error(is_error,"Error: command required"); }

    ExecResult *r = env->exec_command(env, command, timeout, NULL);
    json_free(args);

    StrBuf sb;
    strbuf_init(&sb);
    if(!r) return tool_error(is_error,"Error: process allocation failed");
    if(is_error) *is_error=r->timed_out || r->cancelled || r->exit_code!=0 || r->output_limited;
    if(r->timed_out || r->cancelled || r->exit_code!=0 || r->output_limited) strbuf_appendf(&sb,"Error: command failed (exit=%d timeout=%s output_limit=%s)\n",r->exit_code,r->timed_out?"true":"false",r->output_limited?"true":"false");
    if (r->stdout_buf) strbuf_append_cstr(&sb, r->stdout_buf);
    if(r->stderr_buf && *r->stderr_buf) strbuf_appendf(&sb,"\n[stderr] %s",r->stderr_buf);
    if (r->timed_out) {
        strbuf_appendf(&sb, "\n[ERROR: Command timed out after %dms]", timeout);
    }
    strbuf_appendf(&sb, "\n[exit code: %d, duration: %dms]", r->exit_code, r->duration_ms);
    exec_result_free(r);
    return strbuf_detach(&sb);
}

static char *tool_grep_fn(const char *args_json, ExecutionEnv *env,bool *is_error) {
    if(is_error) *is_error=false;
    if(!env || !env->grep) return tool_error(is_error,"Error: search unsupported");
    const char *err = NULL;
    JsonValue *args = json_parse(args_json, &err);
    if (!args) return tool_error(is_error,"Error: invalid arguments");

    const char *pattern = json_get_string(args, "pattern");
    const char *path = json_get_string(args, "path");
    bool ci = json_get_bool(args, "case_insensitive", false);
    int max = json_get_int(args, "max_results", 100);

    if (!pattern) { json_free(args); return tool_error(is_error,"Error: pattern required"); }

    char *result = env->grep(env, pattern, path, NULL, ci, max);
    json_free(args);
    return result ? result : tool_error(is_error,"Error: search failed");
}

static char *tool_glob_fn(const char *args_json, ExecutionEnv *env,bool *is_error) {
    if(is_error) *is_error=false;
    if(!env || !env->glob) return tool_error(is_error,"Error: glob unsupported");
    const char *err = NULL;
    JsonValue *args = json_parse(args_json, &err);
    if (!args) return tool_error(is_error,"Error: invalid arguments");

    const char *pattern = json_get_string(args, "pattern");
    const char *path = json_get_string(args, "path");

    if (!pattern) { json_free(args); return tool_error(is_error,"Error: pattern required"); }

    char *result = env->glob(env, pattern, path);
    json_free(args);
    return result ? result : tool_error(is_error,"Error: search failed");
}

void agent_register_core_tools(ToolRegistry *reg) {
    /* read_file */
    {
        RegisteredTool *t = mem_calloc(1, sizeof(RegisteredTool));
        if(!t) {reg->failed=true;return;}
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
        RegisteredTool *t = mem_calloc(1, sizeof(RegisteredTool));
        if(!t) {reg->failed=true;return;}
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
        RegisteredTool *t = mem_calloc(1, sizeof(RegisteredTool));
        if(!t) {reg->failed=true;return;}
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
        RegisteredTool *t = mem_calloc(1, sizeof(RegisteredTool));
        if(!t) {reg->failed=true;return;}
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
        RegisteredTool *t = mem_calloc(1, sizeof(RegisteredTool));
        if(!t) {reg->failed=true;return;}
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
        RegisteredTool *t = mem_calloc(1, sizeof(RegisteredTool));
        if(!t) {reg->failed=true;return;}
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
    ProviderProfile *p = mem_calloc(1, sizeof(ProviderProfile));
    if(!p) return NULL;
    p->id = str_dup("anthropic");
    p->model = str_dup(model ? model : "claude-sonnet-4-5");
    p->supports_reasoning = false;
    p->supports_streaming = false;
    p->supports_parallel_tool_calls = false;
    p->context_window_size = 200000;
    p->build_system_prompt = anthropic_system_prompt;
    tool_registry_init(&p->tool_registry);
    agent_register_core_tools(&p->tool_registry);
    if(!p->id || !p->model || p->tool_registry.failed) {provider_profile_free(p);return NULL;}
    return p;
}

ProviderProfile *openai_profile_new(const char *model) {
    ProviderProfile *p = mem_calloc(1, sizeof(ProviderProfile));
    if(!p) return NULL;
    p->id = str_dup("openai");
    p->model = str_dup(model ? model : "gpt-5.2");
    p->supports_reasoning = true;
    p->supports_streaming = false;
    p->supports_parallel_tool_calls = false;
    p->context_window_size = 1047576;
    p->build_system_prompt = anthropic_system_prompt; /* reuse for now */
    tool_registry_init(&p->tool_registry);
    agent_register_core_tools(&p->tool_registry);
    if(!p->id || !p->model || p->tool_registry.failed) {provider_profile_free(p);return NULL;}
    return p;
}

ProviderProfile *gemini_profile_new(const char *model) {
    ProviderProfile *p = mem_calloc(1, sizeof(ProviderProfile));
    if(!p) return NULL;
    p->id = str_dup("gemini");
    p->model = str_dup(model ? model : "gemini-3-flash-preview");
    p->supports_reasoning = false;
    p->supports_streaming = false;
    p->supports_parallel_tool_calls = false;
    p->context_window_size = 1048576;
    p->build_system_prompt = anthropic_system_prompt;
    tool_registry_init(&p->tool_registry);
    agent_register_core_tools(&p->tool_registry);
    if(!p->id || !p->model || p->tool_registry.failed) {provider_profile_free(p);return NULL;}
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
        .max_turns = 100,
        .max_tool_rounds_per_input = 50,
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
    if(!profile || !profile->id || !profile->model || !profile->build_system_prompt || profile->tool_registry.failed || !env || !env->working_directory || !env->platform || !client || config.max_turns<0 || config.max_tool_rounds_per_input<0 || config.default_command_timeout_ms<=0 || config.max_command_timeout_ms<config.default_command_timeout_ms || config.loop_detection_window<0) return NULL;
    AgentSession *s = mem_calloc(1, sizeof(AgentSession));
    if(!s) return NULL;
    s->id = gen_uuid();
    s->profile = profile;
    s->exec_env = env;
    s->llm_client = client;
    s->config = config;
    s->state = SESSION_IDLE;
    s->history_cap = 64;
    s->history = mem_calloc(s->history_cap, sizeof(Message *));
    if(!s->id || !s->history) {agent_session_free(s);return NULL;}
    return s;
}

void agent_session_free(AgentSession *s) {
    if (!s) return;
    free(s->id);
    for (size_t i = 0; s->history && i < s->history_count; i++)
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

static void queue_message(AgentSession *s,char ***queue,size_t *count,const char *text) {
    if(!text || strnlen(text,65537)>65536 || *count>=128) {s->failed=true;return;}
    char *owned=str_dup(text);if(!owned) {s->failed=true;return;}
    char **items=mem_reallocarray(*queue,*count+1,sizeof(char *));
    if(!items) {free(owned);s->failed=true;return;}*queue=items;items[(*count)++]=owned;
}
void agent_session_steer(AgentSession *s,const char *text) {queue_message(s,&s->steering_queue,&s->steering_count,text);}
void agent_session_follow_up(AgentSession *s,const char *text) {queue_message(s,&s->followup_queue,&s->followup_count,text);}
void agent_session_abort(AgentSession *s) {s->abort_signaled=true;}
static void session_add_message(AgentSession *s,Message *m) {
    if(!m || s->history_count>=4096) {message_free(m);s->failed=true;return;}
    if(s->history_count>=s->history_cap) {
        size_t cap=s->history_cap*2;Message **messages=mem_reallocarray(s->history,cap,sizeof(*messages));
        if(!messages) {message_free(m);s->failed=true;return;}s->history=messages;s->history_cap=cap;
    }s->history[s->history_count++]=m;
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
                    char **items=mem_reallocarray(calls,(size_t)(call_count+1),sizeof(*items));
                    if(!items) {free(calls);s->failed=true;return false;}calls=items;
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

static void submit_one(AgentSession *s, const char *input) {
    if(!input || strnlen(input,ATTRACTOR_INPUT_LIMIT+1)>ATTRACTOR_INPUT_LIMIT) {s->failed=true;return;}
    s->state = SESSION_PROCESSING;
    session_add_message(s, message_user(input));
    emit(s, AGENT_EVT_USER_INPUT, input, NULL, NULL);

    drain_steering(s);

    /* Build tool definitions */
    ToolRegistry *reg = &s->profile->tool_registry;
    ToolDefinition **tool_defs=mem_calloc(reg->count,sizeof(*tool_defs));
    if(!tool_defs) {s->failed=true;s->state=SESSION_IDLE;return;}
    for(size_t i=0;i<reg->count;i++) tool_defs[i]=&reg->tools[i]->def;

    int round_count = 0;

    while (1) {
        /* Check limits */
        if (round_count >= (s->config.max_tool_rounds_per_input>0?s->config.max_tool_rounds_per_input:50)) {
            s->failed=true;emit(s, AGENT_EVT_TURN_LIMIT, "Round limit reached", NULL, NULL);
            break;
        }
        if (s->abort_signaled || s->failed) break;

        /* Build system prompt */
        char *wd = s->exec_env->working_directory(s->exec_env);
        char *plat = s->exec_env->platform(s->exec_env);
        if(!wd || !plat) {free(wd);free(plat);s->failed=true;break;}
        char env_info[1024];
        snprintf(env_info, sizeof(env_info),
                 "<environment>\nWorking directory: %s\nPlatform: %s\nModel: %s\n</environment>",
                 wd, plat, s->profile->model);
        free(wd); free(plat);

        char *sys_prompt = s->profile->build_system_prompt(env_info, NULL);
        if(!sys_prompt) {s->failed=true;break;}

        /* Build request */
        size_t msg_count = s->history_count + 1;
        Message **msgs = mem_calloc(msg_count, sizeof(Message *));
        if(!msgs) {free(sys_prompt);s->failed=true;break;}
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
            .cancel = &s->abort_signaled,
        };

        LlmError err = {0};
        LlmResponse *resp = llm_client_complete(s->llm_client, &req, &err);

        free(req.model);
        free(req.provider);
        message_free(msgs[0]); /* free system message */
        free(msgs);
        free(sys_prompt);

        if (!resp) {
            s->failed=true;emit(s, AGENT_EVT_ERROR, err.message ? err.message : "LLM call failed", NULL, NULL);
            llm_error_free(&err);
            break;
        }

        /* Record assistant turn */
        /* Clone the response message into history */
        Message *asst_msg = message_clone(resp->message);
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

            ToolExecutionContext execution={.tool=tool_registry_get(reg,tc->name),.env=s->exec_env,.default_timeout_ms=s->config.default_command_timeout_ms,.max_timeout_ms=s->config.max_command_timeout_ms};
            bool is_error=false;char *result_str=agent_active_tool(tc->arguments_json,&execution,&is_error);
            if(!result_str) {s->failed=true;break;}

            /* Truncate output for LLM (keep full for event) */
            emit(s, AGENT_EVT_TOOL_CALL_END, result_str, tc->name, tc->id);

            if(strlen(result_str)>50000) {
                char *shortened=str_ndup(result_str,50000);free(result_str);result_str=shortened;
                if(!result_str) {s->failed=true;break;}
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

    free(tool_defs);

    s->state = SESSION_IDLE;
    emit(s, AGENT_EVT_SESSION_END, NULL, NULL, NULL);
}

void agent_session_submit(AgentSession *s,const char *input) {
    if(!s || s->state==SESSION_PROCESSING || s->state==SESSION_CLOSED) return;
    const char *next=input;char *owned=NULL;
    for(size_t processed=0;processed<128 && next && !s->abort_signaled && !s->failed;processed++) {
        if(s->turn_count >= (size_t)(s->config.max_turns?s->config.max_turns:100)) {emit(s,AGENT_EVT_TURN_LIMIT,"Turn limit reached",NULL,NULL);break;}
        s->turn_count++;submit_one(s,next);free(owned);owned=NULL;next=NULL;
        if(s->followup_count) {
            owned=s->followup_queue[0];memmove(s->followup_queue,s->followup_queue+1,(s->followup_count-1)*sizeof(char *));s->followup_count--;next=owned;
        }
    }
    free(owned);
}

static bool core_arguments_valid(const JsonValue *args,const ToolDefinition *def) {
    JsonValue *schema=json_parse(def->parameters_json,NULL);if(!schema) return false;
    JsonValue *required=json_get(schema,"required"),*properties=json_get(schema,"properties");bool valid=args && args->type==JSON_OBJECT;
    if(required && required->type==JSON_ARRAY) for(size_t i=0;i<required->array.count;i++) {
        JsonValue *key=required->array.items[i];if(key->type!=JSON_STRING || !json_get(args,key->string)) valid=false;
    }
    if(valid) for(size_t i=0;i<args->object.count;i++) {
        JsonValue *v=args->object.values[i];const char *type=json_get_string(json_get(properties,args->object.keys[i]),"type");
        if(!type) {valid=false;break;}
        if(str_eq(type,"string") && v->type!=JSON_STRING) valid=false;
        if(str_eq(type,"boolean") && v->type!=JSON_BOOL) valid=false;
        if(str_eq(type,"integer") && (v->type!=JSON_NUMBER || !isfinite(v->number) || v->number<0 || v->number>INT_MAX || trunc(v->number)!=v->number)) valid=false;
    }
    json_free(schema);return valid;
}
char *agent_active_tool(const char *arguments_json,void *userdata,bool *is_error) {
    ToolExecutionContext *context=userdata;const char *category=NULL,*message=NULL;char *output=NULL;bool execution_error=false;
    if(is_error) *is_error=false;
    JsonValue *args=json_parse(arguments_json,NULL);
    if(!context || !context->tool || !context->tool->execute || !context->env) {category="configuration";message="Unknown tool or missing environment";}
    else if(!core_arguments_valid(args,&context->tool->def)) {category="arguments";message="Missing or invalid tool arguments";}
    else {
        if(str_eq(context->tool->def.name,"shell")) {
            int duration=json_get_int(args,"timeout_ms",context->default_timeout_ms);
            if(duration<=0 || context->max_timeout_ms<=0) {category="arguments";message="Invalid command timeout";}
            else json_object_set(args,"timeout_ms",json_new_number(duration>context->max_timeout_ms?context->max_timeout_ms:duration));
        }
        if(!category) {
            char *encoded=json_serialize(args);
            output=encoded?context->tool->execute(encoded,context->env,&execution_error):NULL;free(encoded);
            if(!output) {category="execution";message="Tool execution failed";}
            else if(execution_error) {category="execution";message=output;}
            else if(strlen(output)>ATTRACTOR_OUTPUT_LIMIT) {category="output_limit";message="Tool output exceeded limit";}
        }
    }
    JsonValue *result=json_new_object();json_object_set(result,"ok",json_new_bool(!category));
    if(category) {
        if(is_error) *is_error=true;
        JsonValue *error=json_new_object();json_object_set(error,"category",json_new_string(category));json_object_set(error,"message",json_new_string(message));json_object_set(result,"error",error);
    } else json_object_set(result,"output",json_new_string(output?output:""));
    free(output);json_free(args);char *encoded=json_serialize(result);json_free(result);return encoded;
}
