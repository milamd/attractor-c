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

static char *tool_run(const char *arguments_json,ToolExecutionContext *context,const char **category_out,const char **message_out);
static char *tool_envelope(const char *category,const char *message,const char *text);
static char *tool_envelope_truncated(const char *tool,const char *category,const char *message,const char *output,const ToolOutputLimit *limits,size_t limit_count);

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
static bool local_delete_file(ExecutionEnv *self,const char *path) {
    LocalEnvState *st=self->impl;if(!path || !*path) return false;
    const char *slash=strrchr(path,'/');const char *base=slash?slash+1:path;
    if(!*base || str_eq(base,".") || str_eq(base,"..")) return false;
    char *parent=slash?(slash==path?str_dup("/"):str_ndup(path,(size_t)(slash-path))):str_dup(".");if(!parent) return false;
    int fd=io_open_workspace(st->working_dir,parent,O_RDONLY|O_DIRECTORY,0,st->contained);free(parent);if(fd<0) return false;
    bool ok=unlinkat(fd,base,0)==0;close(fd);return ok;
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

static int name_compare(const void *a,const void *b) {return strcmp(*(char *const *)a,*(char *const *)b);}
typedef struct {size_t budget;bool truncated;StrBuf out;} Listing;
/* Lists one directory (sorted) and recurses into subdirectories while depth remains. */
static bool list_fd(int fd,const char *prefix,int depth,Listing *list) {
    int copy=dup(fd);if(copy<0) return false;DIR *dir=fdopendir(copy);if(!dir) {close(copy);return false;}
    char **names=NULL;size_t count=0,cap=0;bool ok=true;struct dirent *entry;
    while(ok && count<10000) {
        errno=0;entry=readdir(dir);if(!entry) {if(errno) ok=false;break;}
        if(str_eq(entry->d_name,".") || str_eq(entry->d_name,"..")) continue;
        if(count==cap) {size_t next=cap?cap*2:32;char **grown=mem_reallocarray(names,next,sizeof(*grown));if(!grown) {ok=false;break;}names=grown;cap=next;}
        if(!(names[count]=str_dup(entry->d_name))) ok=false;else count++;
    }
    if(closedir(dir)!=0) ok=false;
    if(ok) qsort(names,count,sizeof(*names),name_compare);
    for(size_t i=0;ok && i<count && !list->truncated;i++) {
        if(!list->budget) {list->truncated=true;strbuf_append_cstr(&list->out,"[... listing truncated]\n");break;}
        list->budget--;
        struct stat st;if(fstatat(fd,names[i],&st,AT_SYMLINK_NOFOLLOW)!=0) continue;
        bool is_dir=S_ISDIR(st.st_mode);
        strbuf_appendf(&list->out,"%s%s%s\n",prefix,names[i],is_dir?"/":"");
        if(is_dir && depth>1) {
            int child=openat(fd,names[i],O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(child<0) continue;
            StrBuf p;strbuf_init(&p);strbuf_appendf(&p,"%s%s/",prefix,names[i]);char *next=strbuf_detach(&p);
            ok=next && list_fd(child,next,depth-1,list);free(next);close(child);
        }
    }
    for(size_t i=0;i<count;i++) free(names[i]);
    free(names);return ok && !list->out.failed;
}
static char *local_list_directory(ExecutionEnv *self,const char *path,int depth) {
    LocalEnvState *st=self->impl;if(depth<1) depth=1;if(depth>10) depth=10;
    int fd=io_open_workspace(st->working_dir,path && *path?path:".",O_RDONLY|O_DIRECTORY,0,st->contained);if(fd<0) return NULL;
    Listing list={.budget=2000};strbuf_init_limit(&list.out,ATTRACTOR_OUTPUT_LIMIT);
    bool ok=list_fd(fd,"",depth,&list);close(fd);
    if(!ok) {strbuf_free(&list.out);return NULL;}return strbuf_detach(&list.out);
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
    env->delete_file = local_delete_file;
    env->list_directory = local_list_directory;
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
        tool_definition_free(&reg->tools[i].def);
    }
    free(reg->tools);
}

void tool_registry_register(ToolRegistry *reg, RegisteredTool *tool) {
    if(!tool || !tool->def.name || !tool->def.description || !tool->def.parameters_json || !tool->execute) {
        reg->failed=true;if(tool) {tool_definition_free(&tool->def);free(tool);}return;
    }
    /* Replace if exists */
    for (size_t i = 0; i < reg->count; i++) {
        if (str_eq(reg->tools[i].def.name, tool->def.name)) {
            tool_definition_free(&reg->tools[i].def);
            reg->tools[i] = *tool;
            free(tool);
            return;
        }
    }
    if (reg->count >= reg->cap) {
        size_t cap=reg->cap?reg->cap*2:8;
        RegisteredTool *tools=mem_reallocarray(reg->tools,cap,sizeof(*tools));
        if(!tools) {reg->failed=true;tool_definition_free(&tool->def);free(tool);return;}reg->tools=tools;reg->cap=cap;
    }
    reg->tools[reg->count++] = *tool;
    free(tool);
}

void tool_registry_unregister(ToolRegistry *reg, const char *name) {
    for (size_t i = 0; i < reg->count; i++) {
        if (str_eq(reg->tools[i].def.name, name)) {
            tool_definition_free(&reg->tools[i].def);
            memmove(&reg->tools[i], &reg->tools[i+1], (reg->count - i - 1) * sizeof(*reg->tools));
            reg->count--;
            return;
        }
    }
}

RegisteredTool *tool_registry_get(const ToolRegistry *reg, const char *name) {
    for (size_t i = 0; i < reg->count; i++) {
        if (str_eq(reg->tools[i].def.name, name))
            return &reg->tools[i];
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
    if (!replace_all && strstr(pos + 1, old_str)) {
        free(raw);
        json_free(args);
        return tool_error(is_error,"Error: old_string is not unique in file; add surrounding context or set replace_all");
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

static char *tool_list_dir(const char *args_json, ExecutionEnv *env,bool *is_error) {
    if(is_error) *is_error=false;
    if(!env || !env->list_directory) return tool_error(is_error,"Error: directory listing unsupported");
    JsonValue *args=json_parse(args_json,NULL);if(!args) return tool_error(is_error,"Error: invalid arguments");
    char *result=env->list_directory(env,json_get_string(args,"path"),json_get_int(args,"depth",1));
    json_free(args);return result?result:tool_error(is_error,"Error: directory not found");
}

static char *tool_read_many_files(const char *args_json, ExecutionEnv *env,bool *is_error) {
    if(is_error) *is_error=false;
    if(!env || !env->read_file) return tool_error(is_error,"Error: file reads unsupported");
    JsonValue *args=json_parse(args_json,NULL);JsonValue *paths=json_get(args,"paths");
    if(!paths || paths->type!=JSON_ARRAY || !paths->array.count || paths->array.count>100) {json_free(args);return tool_error(is_error,"Error: paths must list 1-100 files");}
    StrBuf out;strbuf_init_limit(&out,ATTRACTOR_OUTPUT_LIMIT);size_t read=0;
    for(size_t i=0;i<paths->array.count;i++) {
        const JsonValue *item=paths->array.items[i];
        if(item->type!=JSON_STRING) {strbuf_free(&out);json_free(args);return tool_error(is_error,"Error: paths must be strings");}
        char *content=env->read_file(env,item->string,0,2000);
        strbuf_appendf(&out,"==> %s <==\n%s\n",item->string,content?content:"Error: file not found");
        if(content) read++;free(content);
    }
    json_free(args);char *result=strbuf_detach(&out);
    if(!result) return tool_error(is_error,"Error: output too large");
    if(!read && is_error) *is_error=true;
    return result;
}

static void register_tool(ToolRegistry *reg,const char *name,const char *description,const char *schema,char *(*execute)(const char *,ExecutionEnv *,bool *)) {
    RegisteredTool *t=mem_calloc(1,sizeof(RegisteredTool));if(!t) {reg->failed=true;return;}
    t->def.name=str_dup(name);t->def.description=str_dup(description);t->def.parameters_json=str_dup(schema);t->execute=execute;
    tool_registry_register(reg,t);
}

void agent_register_core_tools(ToolRegistry *reg) {
    register_tool(reg,"read_file","Read a file from the filesystem. Returns line-numbered content.",
        "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},"
        "\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}},"
        "\"required\":[\"file_path\"]}",tool_read_file);
    register_tool(reg,"write_file","Write content to a file. Creates parent directories if needed.",
        "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},"
        "\"content\":{\"type\":\"string\"}},\"required\":[\"file_path\",\"content\"]}",tool_write_file);
    register_tool(reg,"edit_file","Replace an exact string occurrence in a file.",
        "{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},"
        "\"old_string\":{\"type\":\"string\"},\"new_string\":{\"type\":\"string\"},"
        "\"replace_all\":{\"type\":\"boolean\"}},\"required\":[\"file_path\",\"old_string\",\"new_string\"]}",tool_edit_file);
    register_tool(reg,"shell","Execute a shell command. Returns stdout, stderr, and exit code.",
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},"
        "\"timeout_ms\":{\"type\":\"integer\"},\"description\":{\"type\":\"string\"}},"
        "\"required\":[\"command\"]}",tool_shell);
    register_tool(reg,"grep","Search file contents using regex patterns.",
        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},"
        "\"path\":{\"type\":\"string\"},\"case_insensitive\":{\"type\":\"boolean\"},"
        "\"max_results\":{\"type\":\"integer\"}},\"required\":[\"pattern\"]}",tool_grep_fn);
    register_tool(reg,"glob","Find files matching a glob pattern.",
        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},"
        "\"path\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}",tool_glob_fn);
}

/*============================================================================
 * Provider Profiles
 *==========================================================================*/

/* Shared tail: environment block, then project docs (spec 6.1 layering). */
static char *finish_prompt(StrBuf *sb,const char *env_info,const char *project_docs) {
    if (env_info) { strbuf_append_cstr(sb, env_info); strbuf_append_cstr(sb, "\n\n"); }
    if (project_docs) { strbuf_append_cstr(sb, project_docs); }
    return strbuf_detach(sb);
}

/* codex-rs-aligned */
static char *openai_system_prompt(const char *env_info, const char *project_docs) {
    StrBuf sb; strbuf_init(&sb);
    strbuf_append_cstr(&sb,
        "You are a coding agent running in a terminal-based workspace. You and the user share the same "
        "workspace and collaborate to achieve the user's goals. Keep going until the task is completely "
        "resolved before ending your turn.\n\n"
        "# Tools\n"
        "- Use `shell` to run commands; prefer fast searches with `grep` and `glob`.\n"
        "- Read files with `read_file` before changing them.\n"
        "- Edit files with `apply_patch`. A patch starts with `*** Begin Patch` and ends with `*** End Patch`. "
        "Inside, use `*** Add File: <path>` (every line prefixed `+`), `*** Delete File: <path>`, or "
        "`*** Update File: <path>` (optionally followed by `*** Move to: <path>`) with `@@ <context>` hunks: "
        "lines prefixed ' ' are context, '-' are removed, '+' are added. Show 3 lines of context around changes.\n"
        "- Use `write_file` only to create new files without patch overhead.\n\n"
        "# Coding guidelines\n"
        "- Fix problems at the root cause, keep changes minimal and consistent with the codebase style.\n"
        "- Do not add license headers or inline comments unless asked. Run tests when they exist.\n\n");
    return finish_prompt(&sb, env_info, project_docs);
}

/* Claude Code-aligned */
static char *anthropic_system_prompt(const char *env_info, const char *project_docs) {
    StrBuf sb; strbuf_init(&sb);
    strbuf_append_cstr(&sb,
        "You are an interactive coding agent that helps users with software engineering tasks. "
        "Use the instructions below and the tools available to you to assist the user.\n\n"
        "# Tool usage\n"
        "- Read a file with `read_file` before editing it.\n"
        "- Edit with `edit_file`: `old_string` must match the file exactly (including indentation) and must be "
        "unique; include enough surrounding context, or set `replace_all` to change every occurrence.\n"
        "- Prefer editing existing files over creating new ones; use `write_file` only for new files.\n"
        "- Use `grep` and `glob` to search instead of shell commands like grep or find.\n"
        "- Use `shell` for builds, tests and git; quote paths that contain spaces.\n\n"
        "# Doing tasks\n"
        "- Understand the codebase conventions before changing code, and verify your work with tests.\n"
        "- Be concise. Do not add comments unless they are needed.\n\n");
    return finish_prompt(&sb, env_info, project_docs);
}

/* gemini-cli-aligned */
static char *gemini_system_prompt(const char *env_info, const char *project_docs) {
    StrBuf sb; strbuf_init(&sb);
    strbuf_append_cstr(&sb,
        "You are an interactive CLI agent specializing in software engineering tasks. Your primary goal is to "
        "help users safely and efficiently, adhering strictly to the following instructions and utilizing your "
        "available tools.\n\n"
        "# Core mandates\n"
        "- Rigorously follow existing project conventions; analyze surrounding code, tests and configuration first.\n"
        "- Follow instructions in GEMINI.md files, which hold project-specific context.\n"
        "- Never assume a library is available; check imports and configuration.\n\n"
        "# Workflow\n"
        "1. Understand: use `grep`, `glob`, `list_dir`, `read_file` and `read_many_files` to explore.\n"
        "2. Plan, then implement with `edit_file` (exact `old_string` replacement) and `write_file`.\n"
        "3. Verify with the project's tests and linters via `shell`.\n\n");
    return finish_prompt(&sb, env_info, project_docs);
}

static ProviderProfile *profile_new(const char *id,const char *model,int context_window,char *(*prompt)(const char *,const char *)) {
    ProviderProfile *p = mem_calloc(1, sizeof(ProviderProfile));
    if(!p) return NULL;
    p->id = str_dup(id);
    p->model = str_dup(model);
    p->context_window_size = context_window;
    p->build_system_prompt = prompt;
    tool_registry_init(&p->tool_registry);
    agent_register_core_tools(&p->tool_registry);
    return p;
}
static ProviderProfile *profile_done(ProviderProfile *p) {
    if(p && (!p->id || !p->model || p->tool_registry.failed)) {provider_profile_free(p);return NULL;}
    return p;
}

ProviderProfile *anthropic_profile_new(const char *model) {
    ProviderProfile *p = profile_new("anthropic", model ? model : "claude-sonnet-4-5", 200000, anthropic_system_prompt);
    if(p) p->default_command_timeout_ms = 120000; /* Claude Code convention (spec 3.5) */
    return profile_done(p);
}

ProviderProfile *openai_profile_new(const char *model) {
    ProviderProfile *p = profile_new("openai", model ? model : "gpt-5.2", 1047576, openai_system_prompt);
    if(p) {
        p->supports_reasoning = true;
        /* apply_patch replaces edit_file (spec 3.4); write_file stays for new files. */
        tool_registry_unregister(&p->tool_registry, "edit_file");
        agent_register_apply_patch(&p->tool_registry);
    }
    return profile_done(p);
}

ProviderProfile *gemini_profile_new(const char *model) {
    ProviderProfile *p = profile_new("gemini", model ? model : "gemini-3-flash-preview", 1048576, gemini_system_prompt);
    if(p) {
        register_tool(&p->tool_registry,"list_dir","List a directory's entries, sorted, with directories suffixed '/'.",
            "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"depth\":{\"type\":\"integer\"}}}",tool_list_dir);
        register_tool(&p->tool_registry,"read_many_files","Read several files at once; each is returned under a '==> path <==' header.",
            "{\"type\":\"object\",\"properties\":{\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"paths\"]}",tool_read_many_files);
    }
    return profile_done(p);
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

static void subagent_free(struct SubAgent *a);
void agent_session_free(AgentSession *s) {
    if (!s) return;
    if (s->started && s->state != SESSION_CLOSED) emit(s, AGENT_EVT_SESSION_END, NULL, NULL, NULL);
    for (size_t i = 0; i < s->subagent_count; i++) subagent_free(s->subagents[i]);
    free(s->subagents);
    free(s->project_docs);
    free(s->id);
    for (size_t i = 0; s->history && i < s->history_count; i++)
        message_free(s->history[i]);
    free(s->history);
    for (size_t i = 0; i < s->steering_count; i++)
        free(s->steering_queue[i]);
    free(s->steering_queue);
    for (size_t i = s->followup_head; i < s->followup_count; i++)
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
void agent_session_follow_up(AgentSession *s,const char *text) {
    if(s->followup_head==s->followup_count) s->followup_head=s->followup_count=0;
    queue_message(s,&s->followup_queue,&s->followup_count,text);
}
static char *dequeue_followup(AgentSession *s) {
    if(s->followup_head>=s->followup_count) return NULL;
    char *next=s->followup_queue[s->followup_head++];
    if(s->followup_head==s->followup_count) s->followup_head=s->followup_count=0;
    return next;
}
void agent_session_abort(AgentSession *s) {s->abort_signaled=true;}
static size_t message_chars(const Message *m) {
    size_t chars=0;
    for(size_t j=0;m && j<m->part_count;j++) {
        const ContentPart *p=m->parts[j];
        if(p->text) chars+=strlen(p->text);
        if(p->tool_call) chars+=strlen(str_safe(p->tool_call->name))+strlen(str_safe(p->tool_call->arguments_json));
        if(p->tool_result) chars+=strlen(str_safe(p->tool_result->content));
        if(p->thinking) chars+=strlen(str_safe(p->thinking->text));
    }
    return chars;
}
static void session_add_message(AgentSession *s,Message *m) {
    if(!m || s->history_count>=4096) {message_free(m);s->failed=true;return;}
    if(s->history_count>=s->history_cap) {
        size_t cap=s->history_cap*2;Message **messages=mem_reallocarray(s->history,cap,sizeof(*messages));
        if(!messages) {message_free(m);s->failed=true;return;}s->history=messages;s->history_cap=cap;
    }
    s->history_chars+=message_chars(m);
    s->history[s->history_count++]=m;
}

static void drain_steering(AgentSession *s) {
    for (size_t i = 0; i < s->steering_count; i++) {
        session_add_message(s, message_user(s->steering_queue[i]));
        emit(s, AGENT_EVT_STEERING_INJECTED, s->steering_queue[i], NULL, NULL);
        free(s->steering_queue[i]);
    }
    s->steering_count = 0;
}

/* Loop detection (spec 2.10): the last window tool calls, compared by name and
 * arguments, repeat with period 1, 2 or 3. Calls are collected newest first,
 * which does not change periodicity. */
static bool detect_loop(AgentSession *s) {
    int window = s->config.loop_detection_window;
    if (window <= 0) return false;
    const ToolCallData **calls = mem_calloc((size_t)window, sizeof(*calls));
    if (!calls) { s->failed = true; return false; }
    int call_count = 0;
    for (size_t i = s->history_count; i-- > 0 && call_count < window;) {
        Message *m = s->history[i];
        if (m->role != ROLE_ASSISTANT) continue;
        for (size_t j = m->part_count; j-- > 0 && call_count < window;)
            if (m->parts[j]->kind == CONTENT_TOOL_CALL && m->parts[j]->tool_call) calls[call_count++] = m->parts[j]->tool_call;
    }
    bool detected = false;
    for (int period = 1; call_count == window && period <= 3 && !detected; period++) {
        if (window % period) continue;
        detected = true;
        for (int i = period; i < window && detected; i++)
            detected = str_eq(calls[i]->name, calls[i - period]->name) && str_eq(calls[i]->arguments_json, calls[i - period]->arguments_json);
    }
    free(calls);
    return detected;
}

/* Context window awareness (spec 5.5): ~4 characters per token; warn past 80%. */
static void check_context_usage(AgentSession *s) {
    if (s->profile->context_window_size <= 0) return;
    size_t tokens = s->history_chars / 4, window = (size_t)s->profile->context_window_size;
    if (tokens * 5 <= window * 4) return;
    char warning[96];
    snprintf(warning, sizeof(warning), "Context usage at ~%zu%% of context window", (tokens * 100 + window / 2) / window);
    emit(s, AGENT_EVT_WARNING, warning, NULL, NULL);
}

#define PROJECT_DOCS_BUDGET 32768

/* Appends a regular text file within the remaining budget; false once full. */
static bool append_project_doc(StrBuf *b, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return true;
    FILE *f = fopen(path, "rb");
    if (!f) return true;
    size_t sep = b->len ? 2 : 0;
    size_t room = PROJECT_DOCS_BUDGET > b->len + sep ? PROJECT_DOCS_BUDGET - b->len - sep : 0;
    char *text = mem_alloc(room + 2);
    size_t n = text ? fread(text, 1, room + 1, f) : 0;
    fclose(f);
    if (!n || memchr(text, 0, n)) { free(text); return true; }
    text[n] = 0;
    bool fits = n <= room;
    if (sep) strbuf_append_cstr(b, "\n\n");
    strbuf_append(b, text, fits ? n : str_utf8_prefix(text, room));
    if (!fits) strbuf_append_cstr(b, "\n[Project instructions truncated at 32KB]");
    free(text);
    return fits && !b->failed;
}

/* Project instruction discovery (spec 6.5): each directory from the git root
 * (or the working directory outside a repository) down to the working
 * directory contributes AGENTS.md and then the profile's own file. */
static char *discover_project_docs(const char *profile_id, const char *working_dir) {
    char wd[PATH_MAX], path[PATH_MAX + 32];
    if (!working_dir || !realpath(working_dir, wd)) return NULL;
    size_t n = strlen(wd), root = n;
    for (size_t len = n;;) {
        struct stat st;
        snprintf(path, sizeof(path), "%.*s/.git", (int)len, wd);
        if (stat(path, &st) == 0) { root = len; break; }
        if (len <= 1) break;
        while (len > 1 && wd[len - 1] != '/') len--;
        if (len > 1) len--;
    }
    const char *own = str_eq(profile_id, "anthropic") ? "CLAUDE.md" : str_eq(profile_id, "gemini") ? "GEMINI.md"
                    : str_eq(profile_id, "openai") ? ".codex/instructions.md" : NULL;
    const char *names[] = {"AGENTS.md", own};
    StrBuf b; strbuf_init(&b);
    bool more = true;
    for (size_t end = root; more;) {
        for (size_t i = 0; i < 2 && more; i++) {
            if (!names[i]) continue;
            snprintf(path, sizeof(path), "%.*s/%s", (int)end, wd, names[i]);
            more = append_project_doc(&b, path);
        }
        if (end >= n) break;
        const char *slash = strchr(wd + end + 1, '/');
        end = slash ? (size_t)(slash - wd) : n;
    }
    if (!b.len) { strbuf_free(&b); return NULL; }
    return strbuf_detach(&b);
}

/*============================================================================
 * Subagents
 *==========================================================================*/

/* Sessions are single-threaded, so a subagent runs when the parent waits on
 * it: spawn_agent records the task, send_input queues follow-ups, and wait
 * runs everything pending and returns the result. */
struct SubAgent {
    char            *id;
    AgentSession    *session;
    ProviderProfile *profile; /* owned when the model was overridden, else NULL */
    char            *task;
    bool             started;
};

#define MAX_SUBAGENTS 16

static ToolDefinition subagent_tools[] = {
    {.name = "spawn_agent", .description = "Spawn a subagent to handle a scoped task autonomously. It shares this workspace and runs when you call wait.",
     .parameters_json = "{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\"},\"working_dir\":{\"type\":\"string\"},\"model\":{\"type\":\"string\"},\"max_turns\":{\"type\":\"integer\"}},\"required\":[\"task\"]}"},
    {.name = "send_input", .description = "Send a message to a subagent; it is processed when you next wait on the agent.",
     .parameters_json = "{\"type\":\"object\",\"properties\":{\"agent_id\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"}},\"required\":[\"agent_id\",\"message\"]}"},
    {.name = "wait", .description = "Run a subagent's pending work to completion and return its result.",
     .parameters_json = "{\"type\":\"object\",\"properties\":{\"agent_id\":{\"type\":\"string\"}},\"required\":[\"agent_id\"]}"},
    {.name = "close_agent", .description = "Terminate a subagent and release it.",
     .parameters_json = "{\"type\":\"object\",\"properties\":{\"agent_id\":{\"type\":\"string\"}},\"required\":[\"agent_id\"]}"},
};
#define SUBAGENT_TOOL_COUNT (sizeof(subagent_tools) / sizeof(*subagent_tools))

static bool offers_subagents(const AgentSession *s) { return s->depth < s->config.max_subagent_depth; }
static bool is_subagent_tool(const char *name) {
    for (size_t i = 0; i < SUBAGENT_TOOL_COUNT; i++) if (str_eq(name, subagent_tools[i].name)) return true;
    return false;
}

static void subagent_free(struct SubAgent *a) {
    if (!a) return;
    agent_session_free(a->session);
    provider_profile_free(a->profile);
    free(a->id); free(a->task); free(a);
}

static ProviderProfile *profile_with_model(const ProviderProfile *p, const char *model) {
    if (str_eq(p->id, "openai")) return openai_profile_new(model);
    if (str_eq(p->id, "gemini")) return gemini_profile_new(model);
    ProviderProfile *copy = anthropic_profile_new(model);
    if (copy && !str_eq(p->id, "anthropic")) {
        char *id = str_dup(p->id);
        if (!id) { provider_profile_free(copy); return NULL; }
        free(copy->id); copy->id = id;
    }
    return copy;
}

static size_t find_subagent(const AgentSession *s, const char *id) {
    for (size_t i = 0; id && i < s->subagent_count; i++) if (str_eq(s->subagents[i]->id, id)) return i;
    return SIZE_MAX;
}

static char *subagent_spawn(AgentSession *s, const JsonValue *args, const char **error) {
    const char *task = json_get_string(args, "task"), *dir = json_get_string(args, "working_dir"), *model = json_get_string(args, "model");
    int max_turns = json_get_int(args, "max_turns", 0);
    if (!task || !*task || max_turns < 0) { *error = "task is required and max_turns must be >= 0"; return NULL; }
    if (s->subagent_count >= MAX_SUBAGENTS) { *error = "Too many subagents; close one first"; return NULL; }
    struct SubAgent *a = mem_calloc(1, sizeof(*a));
    struct SubAgent **list = a ? mem_reallocarray(s->subagents, s->subagent_count + 1, sizeof(*list)) : NULL;
    if (!list) { free(a); *error = "Out of memory"; return NULL; }
    s->subagents = list;
    StrBuf t; strbuf_init(&t);
    if (dir && *dir) strbuf_appendf(&t, "Work only within the subdirectory '%s' of the workspace.\n\n", dir);
    strbuf_append_cstr(&t, task);
    a->task = strbuf_detach(&t);
    a->id = gen_uuid();
    if (model && *model && !str_eq(model, s->profile->model)) a->profile = profile_with_model(s->profile, model);
    SessionConfig config = s->config;
    config.max_turns = max_turns;
    a->session = a->task && a->id && (!model || !*model || str_eq(model, s->profile->model) || a->profile)
        ? agent_session_new(a->profile ? a->profile : s->profile, s->exec_env, s->llm_client, config) : NULL;
    if (!a->session) { subagent_free(a); *error = "Cannot create subagent session"; return NULL; }
    a->session->depth = s->depth + 1;
    s->subagents[s->subagent_count++] = a;
    StrBuf out; strbuf_init(&out);
    strbuf_appendf(&out, "{\"agent_id\":\"%s\",\"status\":\"running\"}", a->id);
    return strbuf_detach(&out);
}

static char *subagent_wait(struct SubAgent *a) {
    AgentSession *child = a->session;
    if (!a->started) { a->started = true; agent_session_submit(child, a->task); }
    else if (!child->failed) {
        char *next = dequeue_followup(child);
        if (next) {
            agent_session_submit(child, next);
            free(next);
        }
    }
    char *output = NULL; size_t turns = 0;
    for (size_t i = 0; i < child->history_count; i++) if (child->history[i]->role == ROLE_ASSISTANT) turns++;
    if (child->history_count && child->history[child->history_count - 1]->role == ROLE_ASSISTANT)
        output = message_text(child->history[child->history_count - 1]);
    bool success = !child->failed && output;
    JsonValue *result = json_new_object();
    json_object_set(result, "output", json_new_string(output ? output : ""));
    json_object_set(result, "success", json_new_bool(success));
    json_object_set(result, "turns_used", json_new_number((double)turns));
    json_object_set(result, "status", json_new_string(success ? "completed" : "failed"));
    free(output);
    char *encoded = json_serialize(result);
    json_free(result);
    return encoded;
}

/* Executes a subagent tool; on failure sets *error and returns NULL. */
static char *subagent_tool(AgentSession *s, const char *name, const char *arguments_json, const char **error) {
    JsonValue *args = json_parse(arguments_json, NULL);
    char *out = NULL;
    if (!args || args->type != JSON_OBJECT) { *error = "Invalid arguments"; json_free(args); return NULL; }
    if (str_eq(name, "spawn_agent")) out = subagent_spawn(s, args, error);
    else {
        size_t index = find_subagent(s, json_get_string(args, "agent_id"));
        if (index == SIZE_MAX) *error = "Unknown agent_id";
        else if (str_eq(name, "send_input")) {
            const char *message = json_get_string(args, "message");
            if (!message || !*message) *error = "message is required";
            else {
                agent_session_follow_up(s->subagents[index]->session, message);
                out = s->subagents[index]->session->failed ? NULL : str_dup("{\"status\":\"queued\"}");
                if (!out) *error = "Cannot queue message";
            }
        } else if (str_eq(name, "wait")) {
            out = subagent_wait(s->subagents[index]);
            if (!out) *error = "Out of memory";
        } else {
            subagent_free(s->subagents[index]);
            memmove(s->subagents + index, s->subagents + index + 1, (s->subagent_count - index - 1) * sizeof(*s->subagents));
            s->subagent_count--;
            out = str_dup("{\"status\":\"closed\"}");
        }
    }
    json_free(args);
    return out;
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
    size_t tool_count=reg->count+(offers_subagents(s)?SUBAGENT_TOOL_COUNT:0);
    ToolDefinition **tool_defs=mem_calloc(tool_count?tool_count:1,sizeof(*tool_defs));
    if(!tool_defs) {s->failed=true;s->state=SESSION_IDLE;return;}
    for(size_t i=0;i<reg->count;i++) tool_defs[i]=&reg->tools[i].def;
    for(size_t i=reg->count;i<tool_count;i++) tool_defs[i]=&subagent_tools[i-reg->count];

    int round_count = 0;
    bool fatal = false;

    while (1) {
        /* Check limits */
        /* Zero limits are unlimited (spec 2.2). */
        if (s->config.max_tool_rounds_per_input > 0 && round_count >= s->config.max_tool_rounds_per_input) {
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

        char *sys_prompt = s->profile->build_system_prompt(env_info, s->project_docs);
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
            .tool_count = tool_count,
            .temperature = -1,
            .top_p = -1,
            .reasoning_effort = s->config.reasoning_effort,
            .cancel = &s->abort_signaled,
        };

        LlmError err = {0};
        LlmResponse *resp = llm_client_complete_retry(s->llm_client, &req, 2, &err);

        free(req.model);
        free(req.provider);
        message_free(msgs[0]); /* free system message */
        free(msgs);
        free(sys_prompt);

        if (!resp) {
            /* Unrecoverable once retries are spent (spec 2.8). */
            s->failed=true;fatal=true;emit(s, AGENT_EVT_ERROR, err.message ? err.message : "LLM call failed", NULL, NULL);
            llm_error_free(&err);
            break;
        }

        /* Record assistant turn */
        /* Clone the response message into history */
        Message *asst_msg = message_clone(resp->message);
        session_add_message(s, asst_msg);
        check_context_usage(s);

        emit(s, AGENT_EVT_ASSISTANT_TEXT_END, resp->text, NULL, NULL);

        /* If no tool calls, done */
        if (resp->tool_call_count == 0) {
            llm_response_free(resp);
            break;
        }

        /* Execute tool calls */
        round_count++;
        int shell_default = s->profile->default_command_timeout_ms > 0 ? s->profile->default_command_timeout_ms : s->config.default_command_timeout_ms;
        if (shell_default > s->config.max_command_timeout_ms) shell_default = s->config.max_command_timeout_ms;
        for (size_t i = 0; i < resp->tool_call_count; i++) {
            ToolCall *tc = resp->tool_calls[i];
            emit(s, AGENT_EVT_TOOL_CALL_START, NULL, tc->name, tc->id);

            ToolExecutionContext execution={.tool=tool_registry_get(reg,tc->name),.env=s->exec_env,.default_timeout_ms=shell_default,.max_timeout_ms=s->config.max_command_timeout_ms};
            const char *category=NULL,*message=NULL;char *output=NULL;const char *limit_name=tc->name;
            if(offers_subagents(s) && is_subagent_tool(tc->name)) {
                const char *failure=NULL;output=subagent_tool(s,tc->name,tc->arguments_json,&failure);
                if(failure) {category="execution";message=failure;}
                limit_name="spawn_agent";
            } else {
                /* Abort cancels running commands unless the host wired its own flag. */
                const bool *host_cancel=s->exec_env->cancel;if(!host_cancel) s->exec_env->cancel=&s->abort_signaled;
                output=tool_run(tc->arguments_json,&execution,&category,&message);
                s->exec_env->cancel=host_cancel;
            }
            /* Full output goes to the event; the LLM sees the truncated envelope. */
            char *full=tool_envelope(category,message,output);
            if(full) emit(s, AGENT_EVT_TOOL_CALL_END, full, tc->name, tc->id);
            free(full);
            char *result_str=tool_envelope_truncated(limit_name,category,message,output,s->config.tool_output_limits,s->config.tool_output_limit_count);
            free(output);
            if(!result_str) {s->failed=true;break;}
            session_add_message(s, message_tool_result(tc->id, result_str, category!=NULL));
            free(result_str);
        }

        llm_response_free(resp);

        /* Drain steering between rounds */
        drain_steering(s);

        /* Loop detection */
        if (s->config.enable_loop_detection && detect_loop(s)) {
            char warning[160];
            snprintf(warning, sizeof(warning), "Loop detected: the last %d tool calls follow a repeating pattern. Try a different approach.", s->config.loop_detection_window);
            session_add_message(s, message_user(warning));
            emit(s, AGENT_EVT_LOOP_DETECTION, warning, NULL, NULL);
        }
    }

    free(tool_defs);

    s->state = s->abort_signaled || fatal ? SESSION_CLOSED : SESSION_IDLE;
    emit(s, AGENT_EVT_PROCESSING_END, NULL, NULL, NULL);
    if (s->state == SESSION_CLOSED) emit(s, AGENT_EVT_SESSION_END, NULL, NULL, NULL);
}

void agent_session_submit(AgentSession *s,const char *input) {
    if(!s || s->state==SESSION_PROCESSING || s->state==SESSION_CLOSED) return;
    if(!s->started) {
        s->started=true;
        char *wd=s->exec_env->working_directory(s->exec_env);
        s->project_docs=discover_project_docs(s->profile->id,wd);free(wd);
        emit(s,AGENT_EVT_SESSION_START,NULL,NULL,NULL);
    }
    const char *next=input;char *owned=NULL;
    for(size_t processed=0;processed<128 && next && !s->abort_signaled && !s->failed;processed++) {
        if(s->config.max_turns>0 && s->turn_count >= (size_t)s->config.max_turns) {emit(s,AGENT_EVT_TURN_LIMIT,"Turn limit reached",NULL,NULL);break;}
        s->turn_count++;submit_one(s,next);free(owned);
        owned=dequeue_followup(s);next=owned;
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
static const struct {const char *tool;int max_chars;bool head_tail;int max_lines;} default_tool_limits[]={
    {"read_file",50000,true,0},{"shell",30000,true,256},{"grep",20000,false,200},{"glob",20000,false,500},
    {"edit_file",10000,false,0},{"apply_patch",10000,false,0},{"write_file",1000,false,0},{"spawn_agent",20000,true,0},
};
/* First UTF-8 sequence start at or after byte i. */
static size_t utf8_forward(const char *s,size_t i) {while(s[i] && ((unsigned char)s[i]&0xc0)==0x80) i++;return i;}
char *agent_truncate_output(const char *output,const char *tool_name,const ToolOutputLimit *overrides,size_t override_count) {
    if(!output) return NULL;
    size_t max_chars=50000,max_lines=0;bool head_tail=true;
    for(size_t i=0;tool_name && i<sizeof(default_tool_limits)/sizeof(*default_tool_limits);i++) if(str_eq(default_tool_limits[i].tool,tool_name)) {
        max_chars=(size_t)default_tool_limits[i].max_chars;head_tail=default_tool_limits[i].head_tail;max_lines=(size_t)default_tool_limits[i].max_lines;break;
    }
    for(size_t i=0;tool_name && i<override_count;i++) if(str_eq(overrides[i].tool,tool_name)) {
        if(overrides[i].max_chars>0) max_chars=(size_t)overrides[i].max_chars;
        if(overrides[i].max_lines>0) max_lines=(size_t)overrides[i].max_lines;else if(overrides[i].max_lines<0) max_lines=0;
        break;
    }
    /* Step 1: character truncation always runs first (spec 5.3). */
    size_t length=strlen(output);char *chars=NULL;
    if(length>max_chars) {
        StrBuf b;strbuf_init(&b);size_t removed=length-max_chars;
        if(head_tail) {
            size_t half=max_chars/2,tail=utf8_forward(output,length-half);strbuf_append(&b,output,str_utf8_prefix(output,half));
            strbuf_appendf(&b,"\n\n[WARNING: Tool output was truncated. %zu characters were removed from the middle. The full output is available in the event stream. If you need to see specific parts, re-run the tool with more targeted parameters.]\n\n",removed);
            strbuf_append(&b,output+tail,length-tail);
        } else {
            size_t tail=utf8_forward(output,removed);
            strbuf_appendf(&b,"[WARNING: Tool output was truncated. First %zu characters were removed. The full output is available in the event stream.]\n\n",removed);
            strbuf_append(&b,output+tail,length-tail);
        }
        chars=strbuf_detach(&b);if(!chars) return NULL;output=chars;length=strlen(chars);
    }
    /* Step 2: line truncation, head/tail split, single pass each way. */
    size_t lines=1;
    if(max_lines) for(const char *p=output;(p=memchr(p,'\n',length-(size_t)(p-output)));p++) lines++;
    if(!max_lines || lines<=max_lines) return chars?chars:str_dup(output);
    size_t head_count=max_lines/2,tail_count=max_lines-head_count,head_end=0,tail_start=length;
    if(head_count) for(size_t seen=0;head_end<length;head_end++) if(output[head_end]=='\n' && ++seen==head_count) break;
    for(size_t seen=0;tail_start>0;tail_start--) if(output[tail_start-1]=='\n' && ++seen==tail_count) break;
    StrBuf b;strbuf_init(&b);strbuf_append(&b,output,head_end);
    strbuf_appendf(&b,"\n[... %zu lines omitted ...]\n",lines-head_count-tail_count);strbuf_append(&b,output+tail_start,length-tail_start);
    free(chars);return strbuf_detach(&b);
}
/* Runs a tool. On failure `category` is set and `message` is either a fixed
 * string or NULL to mean the (owned) output text. */
static char *tool_run(const char *arguments_json,ToolExecutionContext *context,const char **category_out,const char **message_out) {
    const char *category=NULL,*message=NULL;char *output=NULL;bool execution_error=false;
    JsonValue *args=json_parse(arguments_json,NULL);
    if(!context || !context->tool || !context->tool->execute || !context->env) {category="configuration";message="Unknown tool or missing environment";}
    else if(!core_arguments_valid(args,&context->tool->def)) {category="arguments";message="Missing or invalid tool arguments";}
    else {
        bool is_shell=str_eq(context->tool->def.name,"shell");
        if(is_shell) {
            int duration=json_get_int(args,"timeout_ms",context->default_timeout_ms);
            if(duration<=0 || context->max_timeout_ms<=0) {category="arguments";message="Invalid command timeout";}
            else json_object_set(args,"timeout_ms",json_new_number(duration>context->max_timeout_ms?context->max_timeout_ms:duration));
        }
        if(!category) {
            char *encoded=is_shell?json_serialize(args):NULL;
            const char *payload=is_shell?encoded:arguments_json;
            output=payload?context->tool->execute(payload,context->env,&execution_error):NULL;free(encoded);
            if(!output) {category="execution";message="Tool execution failed";}
            else if(execution_error) category="execution";
            else if(strlen(output)>ATTRACTOR_OUTPUT_LIMIT) {category="output_limit";message="Tool output exceeded limit";}
        }
    }
    json_free(args);*category_out=category;*message_out=message;return output;
}
static char *tool_envelope(const char *category,const char *message,const char *text) {
    JsonValue *result=json_new_object();json_object_set(result,"ok",json_new_bool(!category));
    if(category) {
        JsonValue *error=json_new_object();json_object_set(error,"category",json_new_string(category));json_object_set(error,"message",json_new_string(message?message:text));json_object_set(result,"error",error);
    } else json_object_set(result,"output",json_new_string(text?text:""));
    char *encoded=json_serialize(result);json_free(result);return encoded;
}
/* Envelope for the LLM: output text truncated per tool limits. Returns NULL on allocation failure. */
static char *tool_envelope_truncated(const char *tool,const char *category,const char *message,const char *output,const ToolOutputLimit *limits,size_t limit_count) {
    if(!output || (category && message)) return tool_envelope(category,message,output);
    char *shown=agent_truncate_output(output,tool,limits,limit_count);if(!shown) return NULL;
    char *encoded=tool_envelope(category,message,shown);free(shown);return encoded;
}
char *agent_active_tool(const char *arguments_json,void *userdata,bool *is_error) {
    ToolExecutionContext *context=userdata;const char *category=NULL,*message=NULL;
    char *output=tool_run(arguments_json,context,&category,&message);if(is_error) *is_error=category!=NULL;
    char *encoded=tool_envelope_truncated(context && context->tool?context->tool->def.name:NULL,category,message,output,NULL,0);free(output);return encoded;
}
