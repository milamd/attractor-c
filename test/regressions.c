#include "util/json.h"
#include "util/str.h"
#include "util/http.h"
#include "util/mem.h"
#include "util/io.h"
#include <stdint.h>
#include <math.h>
#include "agent/agent.h"
#include "attractor/engine.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

static char test_root[1024];
static LlmResponse *fake_complete(ProviderAdapter *self, const LlmRequest *req, LlmError *err) {
    (void)self; (void)req; (void)err;
    LlmResponse *r = calloc(1, sizeof(*r));
    r->message = message_assistant("ok"); r->text = str_dup("ok");
    return r;
}
static void json_truncated(void) {
    const char *inputs[] = {"\"\\", "\"\\u", "\"\\u0", "\"\\u00", "\"\\u000", "\"unterminated"};
    for (size_t i = 0; i < sizeof(inputs)/sizeof(*inputs); i++) {
        char *s = str_dup(inputs[i]); JsonValue *v = json_parse(s, NULL);
        assert(!v); free(s);
    }
}
static void stylesheet_truncated(void) {
    char *s = str_dup("* { broken"); char *err = NULL;
    Stylesheet *ss = stylesheet_parse(s, &err);
    assert(!ss && err); free(err); free(s);
}
static void ctx_alias(void) {
    PipelineContext c; ctx_init(&c); ctx_set(&c, "x", "value");
    ctx_set(&c, "x", ctx_get(&c, "x", "")); assert(str_eq(ctx_get(&c, "x", ""), "value"));
    ctx_set(&c, "x", ctx_get(&c, "x", "")+1); assert(str_eq(ctx_get(&c, "x", ""), "alue")); ctx_free(&c);
}
static void header_zero(void) {
    char buf[1] = {'x'}; assert(!http_header_get("X: value\r\n", "X", buf, 0)); assert(buf[0]=='x');
}
static void negative_retry(void) {
    LlmClient *c = llm_client_new(); ProviderAdapter *a = calloc(1,sizeof(*a));
    a->name = str_dup("fake"); a->complete = fake_complete; llm_client_add_provider(c,a);
    LlmError e = {0}; GenerateResult *r = llm_generate(c,"fake","hello",NULL,0,NULL,NULL,0,1,REASONING_NONE,"fake",-1,&e);
    assert(!r && e.code == LLM_ERR_CONFIG); llm_error_free(&e); llm_client_free(c);
}
static void empty_edit(void) {
    ExecutionEnv *env = local_exec_env_new(test_root); ToolRegistry reg; tool_registry_init(&reg); agent_register_core_tools(&reg);
    assert(env->write_file(env,"edit.txt","existing"));
    char *s = tool_registry_get(&reg,"edit_file")->execute("{\"file_path\":\"edit.txt\",\"old_string\":\"\",\"new_string\":\"x\",\"replace_all\":true}",env,NULL);
    assert(s && str_starts_with(s,"Error:")); free(s); tool_registry_free(&reg); exec_env_free(env);
}
static void fanin(void) {
    DotGraph *g = dot_parse("digraph G { merge [shape=tripleoctagon] }",NULL);
    PipelineRunner *r = pipeline_runner_new(g,test_root); pipeline_register_builtin_handlers(r);
    PipelineContext c; ctx_init(&c); ctx_set(&c,"parallel.results","[{\"node_id\":\"branch1\",\"outcome\":\"success\"}]");
    DotNode *n = dot_find_node(g,"merge"); Handler *h = handler_registry_resolve(&r->handler_reg,n);
    Outcome o = h->execute(n,&c,g,test_root,h->data); assert(o.status==STAGE_SUCCESS && str_eq(o.notes,"branch1"));
    outcome_free(&o); ctx_free(&c); pipeline_runner_free(r); dot_graph_free(g);
}
static int approvals;
static Outcome approval(const DotNode *n, PipelineContext *c,const DotGraph *g,const char *l,void *d) {
    (void)n;(void)c;(void)g;(void)l;(void)d; approvals++; return (Outcome){.status=STAGE_SUCCESS};
}
static void human_missing(void) {
    approvals=0;
    DotGraph *g=dot_parse("digraph G { start [shape=Mdiamond]; gate [shape=hexagon]; approve [type=probe]; exit [shape=Msquare]; start -> gate; gate -> approve [label=Yes]; approve -> exit; }",NULL);
    PipelineRunner *r=pipeline_runner_new(g,test_root); pipeline_register_builtin_handlers(r); handler_registry_register(&r->handler_reg,"probe",approval,NULL);
    Outcome o=pipeline_run(r); assert(o.status==STAGE_FAIL && approvals==0); outcome_free(&o); pipeline_runner_free(r); dot_graph_free(g);
}

static void strict_json(void) {
    const char *bad[]={"true x","01","-","1.","1e","1e999","[1,]","{\"a\":1,}","\"\\q\"","\"\\uD800\"","\"\\uDC00\"","\"\\u0000\"","{\"a\":1,\"a\":2}","\"line\nfeed\""};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) assert(!json_parse(bad[i],NULL));
    JsonValue *j=json_parse("{\"a\\\"b\":\"\\uD83D\\uDE00\",\"n\":9007199254740993,\"x\":3.141592653589793}",NULL);assert(j);
    char *encoded=json_serialize(j);assert(encoded && strstr(encoded,"9007199254740993"));
    JsonValue *r=json_parse(encoded,NULL);assert(r && str_eq(json_get_string(r,"a\"b"),"😀"));
    assert(json_get_number(r,"x",0)==json_get_number(j,"x",1));
    json_free(r);json_free(j);free(encoded);
    j=json_parse("{\"x\":2147483648,\"y\":1.5}",NULL);
    assert(json_get_int(j,"x",7)==7 && json_get_int(j,"y",7)==7);json_free(j);
    assert(!json_new_number(INFINITY));
    char deep[140];memset(deep,'[',70);memset(deep+70,']',69);deep[139]=0;assert(!json_parse(deep,NULL));
}
static void allocation_failures(void) {
    size_t result;assert(!size_add(SIZE_MAX,1,&result));assert(!size_mul(SIZE_MAX,2,&result));
    for(long fail=0;fail<40;fail++) {
        mem_fail_after(fail);
        JsonValue *j=json_parse("{\"a\":[1,2,3],\"b\":\"text\"}",NULL);
        char *s=j?json_serialize(j):NULL;free(s);json_free(j);
        mem_fail_after(-1);
    }
    StrBuf b;strbuf_init_limit(&b,8);strbuf_append_cstr(&b,"1234");
    char *original=b.data;mem_fail_after(0);strbuf_append(&b,"x",SIZE_MAX);
    assert(b.failed && b.data==original);assert(!strbuf_detach(&b));mem_fail_after(-1);
    strbuf_init(&b);strbuf_append_cstr(&b,"abc");strbuf_append(&b,b.data,b.len);assert(str_eq(b.data,"abcabc"));strbuf_free(&b);
}
static void bounded_io(void) {
    int fd[2];assert(pipe(fd)==0);assert(write(fd[1],"short",5)==5);close(fd[1]);
    FILE *f=fdopen(fd[0],"r");assert(f);size_t len=0;char *s=io_read_stream(f,8,&len);
    assert(s && len==5 && str_eq(s,"short"));free(s);fclose(f);
    f=tmpfile();assert(f);assert(fputs("too large",f)>=0);rewind(f);assert(!io_read_stream(f,3,NULL));fclose(f);
}

static int open_descriptor_count(void) {
    int count=0;for(int fd=0;fd<256;fd++) if(fcntl(fd,F_GETFD)>=0) count++;return count;
}
static void process_deadlines(void) {
    int descriptors=open_descriptor_count();
    ExecutionEnv *env=local_exec_env_new(test_root);assert(env);
    const char *commands[]={"sleep 3","exec 1>&- 2>&-; sleep 3","while :; do printf x; done","sleep 3 & wait"};
    for(size_t i=0;i<sizeof(commands)/sizeof(*commands);i++) {
        ExecResult *r=env->exec_command(env,commands[i],100,NULL);
        assert(r && r->timed_out && r->duration_ms<1000);exec_result_free(r);
    }
    ExecResult *r=env->exec_command(env,"printf SHOULD_NOT_EXECUTE",100,"/nonexistent/attractor-directory");
    assert(r && r->exit_code==-1 && (!r->stdout_buf || !*r->stdout_buf));exec_result_free(r);
    r=env->exec_command(env,"exit 7",100,NULL);assert(r && r->exit_code==7);exec_result_free(r);
    setenv("OPENAI_API_KEY","fake-test-key",1);setenv("CUSTOM_SECRET_TOKEN","fake-test-key",1);
    r=env->exec_command(env,"test -z \"$OPENAI_API_KEY\" && test -z \"$CUSTOM_SECRET_TOKEN\"",100,NULL);
    assert(r && r->exit_code==0);exec_result_free(r);unsetenv("OPENAI_API_KEY");unsetenv("CUSTOM_SECRET_TOKEN");exec_env_free(env);
    const char *argv[]={"/bin/sh","-c","printf output",NULL};
    for(long i=0;i<64;i++) {
        mem_fail_after(i);r=process_run(&(ProcessOptions){.argv=argv,.timeout_ms=100});mem_fail_after(-1);
        if(r && r->exit_code==0) assert(r->stdout_buf && r->stderr_buf && str_eq(r->stdout_buf,"output"));
        exec_result_free(r);
        assert(open_descriptor_count()==descriptors);
        int status;assert(waitpid(-1,&status,WNOHANG)==-1 && errno==ECHILD);
    }
}
static void secure_files(void) {
    ExecutionEnv *env=local_exec_env_new_policy(test_root,true);assert(env);
    assert(env->write_file(env,"quote'; printf INJECTED; #/file.txt","hello"));
    assert(!env->write_file(env,"../escaped.txt","bad"));
    char *s=env->read_raw(env,"quote'; printf INJECTED; #/file.txt");assert(str_eq(s,"hello"));free(s);
    char link[2048];snprintf(link,sizeof(link),"%s/link",test_root);assert(symlink("/tmp",link)==0);
    assert(!env->write_file(env,"link/escaped.txt","bad"));
    s=env->grep(env,"'; printf INJECTED; #",".",NULL,false,1);assert(!s || !strstr(s,"INJECTED"));free(s);
    s=env->glob(env,"'; printf INJECTED; #",".");assert(!s || !strstr(s,"INJECTED"));free(s);
    char *a=io_artifact_name("../A"),*b=io_artifact_name("../a");assert(a && b && strcmp(a,b) && !strchr(a,'/'));free(a);free(b);
    assert(io_artifact_write(test_root,"../outside","notes","safe"));exec_env_free(env);
}
static char *mock_raw(ExecutionEnv *self,const char *path) {(void)self;(void)path;return str_dup("abc abc");}
static bool mock_write(ExecutionEnv *self,const char *path,const char *content) {(void)self;(void)path;return str_eq(content,"xyz xyz");}
static void mock_edit(void) {
    ExecutionEnv env={.read_raw=mock_raw,.write_file=mock_write};ToolRegistry reg;tool_registry_init(&reg);agent_register_core_tools(&reg);
    char *s=tool_registry_get(&reg,"edit_file")->execute("{\"file_path\":\"virtual\",\"old_string\":\"abc\",\"new_string\":\"xyz\",\"replace_all\":true}",&env,NULL);
    assert(s && str_eq(s,"Replaced 2 occurrence(s)"));free(s);tool_registry_free(&reg);
}

static int stage_calls;
static Outcome fail_handler(const DotNode *n,PipelineContext *c,const DotGraph *g,const char *l,void *d) {
    (void)n;(void)c;(void)g;(void)l;(void)d;return (Outcome){.status=STAGE_FAIL,.failure_reason=str_dup("expected failure")};
}
static Outcome count_handler(const DotNode *n,PipelineContext *c,const DotGraph *g,const char *l,void *d) {
    (void)n;(void)c;(void)g;(void)l;(void)d;stage_calls++;return (Outcome){.status=STAGE_SUCCESS};
}
static void condition_syntax(void) {
    const char *bad[]={"outcome=fail ||","&& outcome=success","(outcome=success","outcome=","=success","outcome==success","outcome=success)","a & b","()","a(b)","a=\"broken","a=broken\""};
    Outcome o={.status=STAGE_SUCCESS};PipelineContext c={0};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) {assert(!condition_validate(bad[i]));assert(!evaluate_condition(bad[i],&o,&c));}
    assert(condition_validate("(outcome=success || x=y) && outcome!=fail"));
    assert(evaluate_condition("(outcome=success || x=y) && outcome!=fail",&o,&c));
}
static void routing_failures(void) {
    const char *graphs[]={
      "digraph G { start; exit; a [type=count]; forbidden [type=count]; start -> a; a -> forbidden [condition=\"outcome=fail\"]; forbidden -> exit; }",
      "digraph G { start; exit; a [type=fail]; after [type=count]; start -> a -> after -> exit; }",
      "digraph G { start; exit; a [type=count]; start -> a; a -> a; a -> exit [condition=\"outcome=fail\"]; }"
    };
    for(size_t i=0;i<sizeof(graphs)/sizeof(*graphs);i++) {
        DotGraph *g=dot_parse(graphs[i],NULL);assert(g);
        PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);
        handler_registry_register(&r->handler_reg,"count",count_handler,NULL);handler_registry_register(&r->handler_reg,"fail",fail_handler,NULL);
        r->max_iterations=8;stage_calls=0;Outcome o=pipeline_run(r);assert(o.status==STAGE_FAIL);
        if(i==0) assert(stage_calls==1);if(i==1) assert(stage_calls==0);if(i==2) assert(stage_calls==7);
        outcome_free(&o);pipeline_runner_free(r);dot_graph_free(g);
    }
    DotGraph *g=dot_parse("digraph G { start; exit; a [type=fail]; after [type=count]; start -> a; a -> after [condition=\"outcome=fail\"]; after -> exit; }",NULL);
    PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);handler_registry_register(&r->handler_reg,"count",count_handler,NULL);handler_registry_register(&r->handler_reg,"fail",fail_handler,NULL);
    Outcome o=pipeline_run(r);assert(o.status==STAGE_SUCCESS);outcome_free(&o);pipeline_runner_free(r);dot_graph_free(g);
}

static void crash_after_commit(const PipelineEvent *e,void *data) {
    (void)data;if(e->kind==PIPE_EVT_CHECKPOINT_SAVED && str_eq(e->node_id,"after")) _exit(0);
}
static void checkpoint_recovery(void) {
    DotGraph *g=dot_parse("digraph G { start; exit; a [type=fail]; after [type=count]; start -> a; a -> after [condition=\"outcome=fail\"]; after -> exit; }",NULL);assert(g);
    pid_t child=fork();assert(child>=0);
    if(child==0) {
        PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);
        handler_registry_register(&r->handler_reg,"fail",fail_handler,NULL);handler_registry_register(&r->handler_reg,"count",count_handler,NULL);
        pipeline_runner_on_event(r,crash_after_commit,NULL);Outcome o=pipeline_run(r);outcome_free(&o);_exit(3);
    }
    int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    char path[2048];snprintf(path,sizeof(path),"%s/checkpoint.json",test_root);
    Checkpoint cp={0};assert(checkpoint_load(&cp,path));assert(str_eq(cp.next_node,"after") && cp.history->array.count==2);
    assert(str_eq(json_get_string(json_get(cp.latest,"a"),"status"),"fail"));checkpoint_free(&cp);
    PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);
    handler_registry_register(&r->handler_reg,"fail",fail_handler,NULL);handler_registry_register(&r->handler_reg,"count",count_handler,NULL);
    stage_calls=0;Outcome o=pipeline_resume(r,path);assert(o.status==STAGE_SUCCESS && stage_calls==1);outcome_free(&o);
    assert(checkpoint_load(&cp,path) && cp.complete);checkpoint_free(&cp);
    o=pipeline_resume(r,path);assert(o.status==STAGE_SUCCESS && stage_calls==1);outcome_free(&o);
    assert(io_atomic_write(path,"{\"version\":1}",IO_ATOMIC));o=pipeline_resume(r,path);assert(o.status==STAGE_FAIL);outcome_free(&o);
    pipeline_runner_free(r);dot_graph_free(g);
}
static void atomic_faults(void) {
    char path[2048];snprintf(path,sizeof(path),"%s/checkpoint.json",test_root);assert(io_atomic_write(path,"old",IO_SYNC));
    const IoFault faults[]={IO_FAULT_WRITE,IO_FAULT_FLUSH,IO_FAULT_SYNC,IO_FAULT_REPLACE};
    for(size_t i=0;i<sizeof(faults)/sizeof(*faults);i++) {
        io_test_fault(faults[i]);assert(!io_atomic_write(path,"new",IO_SYNC));io_test_fault(IO_FAULT_NONE);
        char *s=io_read_text(path,8);assert(str_eq(s,"old"));free(s);
    }
    assert(io_atomic_write(path,"new",IO_SYNC));char *s=io_read_text(path,8);assert(str_eq(s,"new"));free(s);
}
static Outcome gate_attempt(const DotNode *n,PipelineContext *c,const DotGraph *g,const char *l,void *d) {
    (void)n;(void)g;(void)l;(void)d;
    bool tried=str_eq(ctx_get(c,"tried",""),"yes");ctx_set(c,"tried","yes");return (Outcome){.status=tried?STAGE_SUCCESS:STAGE_FAIL};
}
static void latest_gate(void) {
    DotGraph *g=dot_parse("digraph G { start; exit; gate [type=gate,goal_gate=true]; start -> gate; gate -> gate [condition=\"outcome=fail\"]; gate -> exit [condition=\"outcome=success\"]; }",NULL);
    PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);handler_registry_register(&r->handler_reg,"gate",gate_attempt,NULL);
    Outcome o=pipeline_run(r);assert(o.status==STAGE_SUCCESS);outcome_free(&o);pipeline_runner_free(r);dot_graph_free(g);
}

static size_t backend_calls;
static char *count_backend(CodergenBackend *self,const DotNode *node,const char *prompt,const PipelineContext *ctx) {
    (void)self;(void)prompt;(void)ctx;backend_calls++;return str_dup(node->id);
}
static void fixture_assertions(void) {
    const char *files[]={"simple","branching","conditions","parallel","styled","invalid"};
    const size_t expected[]={2,3,1,3,3,0};
    for(size_t i=0;i<sizeof(files)/sizeof(*files);i++) {
        char path[256];snprintf(path,sizeof(path),"test/%s.dot",files[i]);char *source=io_read_text(path,ATTRACTOR_INPUT_LIMIT);assert(source);
        DotGraph *g=dot_parse(source,NULL);free(source);assert(g);
        DiagnosticList diagnostics=validate_graph(g);
        if(i==5) {
            assert(diagnostic_list_has_errors(&diagnostics));bool reason=false;
            for(size_t k=0;k<diagnostics.count;k++) if(str_eq(diagnostics.items[k].rule,"start_node")) reason=true;
            assert(reason);diagnostic_list_free(&diagnostics);dot_graph_free(g);continue;
        }
        assert(!diagnostic_list_has_errors(&diagnostics));diagnostic_list_free(&diagnostics);
        transform_expand_variables(g);transform_apply_stylesheet(g);
        if(i==4) {assert(str_eq(dot_find_node(g,"implement")->llm_model,"claude-opus-4-6"));assert(str_eq(dot_find_node(g,"critical_review")->llm_provider,"openai"));}
        char logs[2048];snprintf(logs,sizeof(logs),"%s/%s",test_root,files[i]);
        PipelineRunner *r=pipeline_runner_new(g,logs);pipeline_register_builtin_handlers(r);
        CodergenBackend backend={.run=count_backend};pipeline_runner_set_backend(r,&backend);backend_calls=0;
        Outcome o=pipeline_run(r);if(o.status!=STAGE_SUCCESS || backend_calls!=expected[i]) fprintf(stderr,"fixture %s status %d calls %zu expected %zu reason %s\n",files[i],o.status,backend_calls,expected[i],str_safe(o.failure_reason));assert(o.status==STAGE_SUCCESS && backend_calls==expected[i]);outcome_free(&o);
        char checkpoint[4096];snprintf(checkpoint,sizeof(checkpoint),"%s/checkpoint.json",logs);Checkpoint cp={0};assert(checkpoint_load(&cp,checkpoint));assert(cp.complete);
        if(i==3) assert(str_eq(ctx_get(&cp.context,"parallel.fan_in.best_id",""),"branch1"));
        if(i==0) assert(str_eq(ctx_get(&cp.context,"response.report",""),"report"));
        checkpoint_free(&cp);pipeline_runner_free(r);dot_graph_free(g);
    }
}
static size_t a_calls,a2_calls,b_calls;
static Outcome isolated_branch(const DotNode *n,PipelineContext *c,const DotGraph *g,const char *l,void *d) {
    (void)g;(void)l;(void)d;
    if(str_eq(n->id,"a")) {a_calls++;ctx_set(c,"a.output","present");}
    if(str_eq(n->id,"a2")) {a2_calls++;assert(str_eq(ctx_get(c,"a.output",""),"present"));}
    if(str_eq(n->id,"b")) {b_calls++;assert(!ctx_get(c,"a.output",NULL));ctx_set(c,"b.output","present");}
    return (Outcome){.status=STAGE_SUCCESS};
}
static void complete_branches(void) {
    DotGraph *g=dot_parse("digraph G { start; exit; fork [shape=component,error_policy=continue]; merge [shape=tripleoctagon]; a [type=branch]; a2 [type=branch]; b [type=branch]; start -> fork; fork -> a; fork -> b; a -> a2 -> merge; b -> merge; merge -> exit; }",NULL);
    PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);handler_registry_register(&r->handler_reg,"branch",isolated_branch,NULL);
    a_calls=a2_calls=b_calls=0;Outcome o=pipeline_run(r);assert(o.status==STAGE_SUCCESS && a_calls==1 && a2_calls==1 && b_calls==1);outcome_free(&o);
    char path[2048];snprintf(path,sizeof(path),"%s/checkpoint.json",test_root);Checkpoint cp={0};assert(checkpoint_load(&cp,path));
    assert(str_eq(ctx_get(&cp.context,"a.output",""),"present") && str_eq(ctx_get(&cp.context,"b.output",""),"present"));
    checkpoint_free(&cp);pipeline_runner_free(r);dot_graph_free(g);
}
static Outcome conflicting_branch(const DotNode *n,PipelineContext *c,const DotGraph *g,const char *l,void *d) {
    (void)g;(void)l;(void)d;ctx_set(c,"shared",n->id);return (Outcome){.status=STAGE_SUCCESS};
}
static void branch_conflict(void) {
    DotGraph *g=dot_parse("digraph G { start; exit; fork [shape=component]; a [type=branch]; b [type=branch]; start -> fork; fork -> a; fork -> b; a -> exit; b -> exit; }",NULL);
    PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);handler_registry_register(&r->handler_reg,"branch",conflicting_branch,NULL);
    Outcome o=pipeline_run(r);assert(o.status==STAGE_FAIL);outcome_free(&o);pipeline_runner_free(r);dot_graph_free(g);
}
static void child_configuration(void) {
    char child_path[2048];snprintf(child_path,sizeof(child_path),"%s/child.dot",test_root);
    assert(io_atomic_write(child_path,"digraph Child { start; work; exit; start -> work -> exit; }",IO_ATOMIC));
    StrBuf source;strbuf_init(&source);strbuf_appendf(&source,"digraph Parent { start; exit; manager [type=\"stack.manager_loop\",stack.child_dotfile=\"%s\"]; start -> manager -> exit; }",child_path);
    DotGraph *g=dot_parse(source.data,NULL);strbuf_free(&source);assert(g);
    PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);CodergenBackend backend={.run=count_backend};pipeline_runner_set_backend(r,&backend);backend_calls=0;
    Outcome o=pipeline_run(r);assert(o.status==STAGE_SUCCESS && backend_calls==1);outcome_free(&o);pipeline_runner_free(r);
    assert(io_atomic_write(child_path,"digraph Child { start; exit; work [type=fail]; start -> work -> exit; }",IO_ATOMIC));
    char failed_logs[2048];snprintf(failed_logs,sizeof(failed_logs),"%s/failed",test_root);r=pipeline_runner_new(g,failed_logs);pipeline_register_builtin_handlers(r);handler_registry_register(&r->handler_reg,"fail",fail_handler,NULL);
    o=pipeline_run(r);assert(o.status==STAGE_FAIL);outcome_free(&o);pipeline_runner_free(r);dot_graph_free(g);
}

static size_t generated_rounds;
static bool expect_tool_error;
static LlmResponse *tool_loop_provider(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {
    (void)self;(void)err;
    if(generated_rounds++%2==1) {
        Message *last=req->messages[req->message_count-1];assert(last->role==ROLE_TOOL && last->parts[0]->tool_result->is_error==expect_tool_error);
        return fake_complete(self,req,err);
    }
    LlmResponse *r=calloc(1,sizeof(*r));r->message=message_assistant(NULL);r->message->part_count=1;r->message->parts=calloc(1,sizeof(ContentPart *));
    ContentPart *p=calloc(1,sizeof(*p));p->kind=CONTENT_TOOL_CALL;p->tool_call=calloc(1,sizeof(*p->tool_call));
    p->tool_call->id=str_dup("call");p->tool_call->name=str_dup("probe");p->tool_call->arguments_json=str_dup("{}");r->message->parts[0]=p;
    r->tool_call_count=1;r->tool_calls=calloc(1,sizeof(ToolCall *));ToolCall *call=calloc(1,sizeof(*call));
    call->id=str_dup("call");call->name=str_dup("probe");call->arguments_json=str_dup("{}");r->tool_calls[0]=call;r->finish_reason.reason=FINISH_TOOL_CALLS;return r;
}
static char *context_tool(const char *arguments,void *userdata,bool *is_error) {
    assert(str_eq(arguments,"{}"));*is_error=expect_tool_error;return str_dup(userdata);
}
static int middleware_order[8];static size_t middleware_count;
static LlmResponse *middleware_a(const LlmRequest *req,LlmResponse *(*next)(const LlmRequest *,void *),void *ctx) {
    middleware_order[middleware_count++]=1;LlmResponse *r=next(req,ctx);middleware_order[middleware_count++]=4;return r;
}
static LlmResponse *middleware_b(const LlmRequest *req,LlmResponse *(*next)(const LlmRequest *,void *),void *ctx) {
    middleware_order[middleware_count++]=2;LlmResponse *r=next(req,ctx);middleware_order[middleware_count++]=3;return r;
}
static void generation_contracts(void) {
    LlmClient *c=llm_client_new();ProviderAdapter *adapter=calloc(1,sizeof(*adapter));adapter->name=str_dup("fake");adapter->complete=tool_loop_provider;llm_client_add_provider(c,adapter);
    ActiveTool tool={.def={.name="probe",.description="probe",.parameters_json="{}"},.execute=context_tool};
    for(size_t i=0;i<20;i++) {
        tool.userdata=i%2?"session B":"session A";expect_tool_error=i%2==0;LlmError e={0};
        GenerateResult *r=llm_generate(c,"fake","hello",NULL,0,NULL,&tool,1,1,REASONING_NONE,"fake",0,&e);
        assert(r && r->response && str_eq(r->text,"ok"));generate_result_free(r);llm_error_free(&e);
    }
    adapter->complete=fake_complete;llm_client_add_middleware(c,middleware_a);llm_client_add_middleware(c,middleware_b);
    middleware_count=0;LlmError e={0};GenerateResult *r=llm_generate(c,"fake","hello",NULL,0,NULL,NULL,0,0,REASONING_NONE,"fake",0,&e);
    assert(r && middleware_count==4);for(size_t i=0;i<4;i++) assert(middleware_order[i]==(int)i+1);generate_result_free(r);
    LlmRequest request={.model="fake",.reasoning_effort=(ReasoningEffort)99};
    assert(!llm_client_complete(c,&request,&e) && e.code==LLM_ERR_CONFIG);
    ToolChoice choice={.mode=TOOL_CHOICE_NAMED};request.reasoning_effort=REASONING_NONE;request.tool_choice=&choice;
    assert(!llm_client_complete(c,&request,&e) && e.code==LLM_ERR_CONFIG);
    assert(llm_client_stream(c,NULL,NULL,NULL,&e)==-1 && e.code==LLM_ERR_STREAM);llm_error_free(&e);llm_client_free(c);
}
static int observed_timeout;
static ExecResult *mock_failed_exec(ExecutionEnv *self,const char *command,int timeout,const char *directory) {
    (void)self;(void)command;(void)directory;observed_timeout=timeout;ExecResult *r=calloc(1,sizeof(*r));r->exit_code=7;r->stdout_buf=str_dup("partial");return r;
}
static void structured_tool_errors(void) {
    ToolRegistry reg;tool_registry_init(&reg);agent_register_core_tools(&reg);ExecutionEnv env={.exec_command=mock_failed_exec};
    ToolExecutionContext context={.tool=tool_registry_get(&reg,"shell"),.env=&env,.default_timeout_ms=10,.max_timeout_ms=77};
    bool error=false;char *s=agent_active_tool("{\"command\":\"exit 7\",\"timeout_ms\":1000}",&context,&error);
    assert(error && observed_timeout==77);JsonValue *j=json_parse(s,NULL);assert(j && !json_get_bool(j,"ok",true));json_free(j);free(s);
    s=agent_active_tool("{}",&context,&error);assert(error);free(s);
    s=agent_active_tool("{\"command\":\"x\",\"timeout_ms\":1e100}",&context,&error);assert(error);free(s);
    env.exec_command=NULL;s=agent_active_tool("{\"command\":\"exit 0\"}",&context,&error);assert(s && error);free(s);tool_registry_free(&reg);
    ProviderProfile *profile=openai_profile_new("fake");assert(profile && !profile->supports_streaming && !profile->supports_parallel_tool_calls);provider_profile_free(profile);
}
static void parser_allocation_failures(void) {
    const char *source="digraph G { node [shape=box]; start [shape=Mdiamond]; exit [shape=Msquare]; subgraph cluster_code { a [prompt=hello]; b; a -> b; } start -> a; b -> exit; }";
    for(long i=0;i<320;i++) {
        mem_fail_after(i);char *err=NULL;DotGraph *g=dot_parse(source,&err);mem_fail_after(-1);dot_graph_free(g);free(err);
        mem_fail_after(i);Stylesheet *style=stylesheet_parse("* { llm_model: fake; } .code { llm_provider: fake; }",&err);mem_fail_after(-1);stylesheet_free(style);free(err);
    }
    for(long i=0;i<64;i++) {
        mem_fail_after(i);Message *m=message_user("hello");Message *copy=m?message_clone(m):NULL;mem_fail_after(-1);message_free(m);message_free(copy);
    }
}

typedef struct {pid_t pid;char url[128];} HttpFixture;
static HttpFixture http_fixture(const void *body,size_t length,int status,bool provider) {
    int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(bind(listener,(struct sockaddr *)&address,sizeof(address))==0 && listen(listener,1)==0);
    socklen_t size=sizeof(address);assert(getsockname(listener,(struct sockaddr *)&address,&size)==0);
    HttpFixture fixture={0};snprintf(fixture.url,sizeof(fixture.url),"http://127.0.0.1:%u",(unsigned)ntohs(address.sin_port));
    fixture.pid=fork();assert(fixture.pid>=0);
    if(fixture.pid==0) {
        signal(SIGPIPE,SIG_IGN);alarm(3);
        int client=accept(listener,NULL,NULL);assert(client>=0);close(listener);
        char request[65536];size_t used=0;char *content=NULL;size_t expected=0;
        for(;;) {
            assert(used<sizeof(request)-1);ssize_t n=read(client,request+used,sizeof(request)-1-used);assert(n>0);used+=(size_t)n;request[used]=0;
            content=strstr(request,"\r\n\r\n");
            if(content) {
                content+=4;char header[64];const char *value=http_header_get(request,"Content-Length",header,sizeof(header));
                expected=value?(size_t)strtoul(value,NULL,10):0;
                if(used-(size_t)(content-request)>=expected) break;
            }
        }
        if(provider) {
            assert(strstr(request,"fake-credential") && !strstr(request,"?key="));
            JsonValue *j=json_parse(content,NULL);assert(j && j->type==JSON_OBJECT);
            const char *model=json_get_string(j,"model");if(model) assert(str_eq(model,"fake\"model"));
            json_free(j);
        }
        char header[256];int count=snprintf(header,sizeof(header),"HTTP/1.1 %d Test\r\nContent-Length: %zu\r\nRetry-After: 0.2\r\nConnection: close\r\n\r\n",status,length);
        (void)write(client,header,(size_t)count);const char *cursor=body;size_t remaining=length;
        while(remaining) {ssize_t n=write(client,cursor,remaining);if(n<=0) break;cursor+=n;remaining-=(size_t)n;}
        close(client);_exit(0);
    }
    close(listener);return fixture;
}
static void http_fixture_wait(HttpFixture fixture) {
    int status;pid_t waited=waitpid(fixture.pid,&status,0);if(waited!=fixture.pid || !WIFEXITED(status) || WEXITSTATUS(status)!=0) fprintf(stderr,"HTTP fixture child status=%d waited=%ld expected=%ld\n",status,(long)waited,(long)fixture.pid);assert(waited==fixture.pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
}
static void local_transport(void) {
    const char binary[]={'a',0,'b','c'};HttpFixture fixture=http_fixture(binary,sizeof(binary),200,false);
    HttpRequest request={.url=fixture.url,.method="GET",.timeout_ms=1000};HttpResponse *r=http_request(&request);
    assert(r && r->body_len==sizeof(binary) && !memcmp(r->body,binary,sizeof(binary)));http_response_free(r);http_fixture_wait(fixture);
    fixture=http_fixture("too large",9,200,false);request.url=fixture.url;request.max_body_bytes=3;assert(!http_request(&request));http_fixture_wait(fixture);
    fixture=http_fixture("x",1,200,false);request.url=fixture.url;request.max_body_bytes=8;request.max_header_bytes=8;assert(!http_request(&request));http_fixture_wait(fixture);
    request=(HttpRequest){.url="file:///etc/passwd",.timeout_ms=100};assert(!http_request(&request));
    const char *payloads[]={
        "{\"id\":\"fake\",\"model\":\"fake\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"usage\":{\"input_tokens\":3,\"output_tokens\":2}}",
        "{\"id\":\"fake\",\"model\":\"fake\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}],\"usage\":{\"input_tokens\":3,\"output_tokens\":2}}",
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"ok\",\"thoughtSignature\":\"opaque\"}]},\"finishReason\":\"STOP\"}],\"usageMetadata\":{\"promptTokenCount\":3,\"candidatesTokenCount\":2}}"
    };
    const char *names[]={"anthropic","openai","gemini"};
    for(size_t i=0;i<3;i++) {
        fixture=http_fixture(payloads[i],strlen(payloads[i]),200,true);LlmClient *client=llm_client_new();
        ProviderAdapter *adapter=i==0?anthropic_adapter_new("fake-credential",fixture.url):i==1?openai_adapter_new("fake-credential",fixture.url):gemini_adapter_new("fake-credential",fixture.url);
        assert(adapter);llm_client_add_provider(client,adapter);Message *message=message_user("hello");
        ToolDefinition definition={.name="optional_description",.parameters_json="{}"};ToolDefinition *definitions[]={&definition};
        LlmRequest req={.model="fake\"model",.provider=(char *)names[i],.messages=&message,.message_count=1,.temperature=-1,.top_p=-1,.tools=definitions,.tool_count=1};LlmError error={0};
        LlmResponse *response=llm_client_complete(client,&req,&error);assert(response && str_eq(response->text,"ok") && response->usage.total_tokens==5);
        if(i==2) {assert(response->message->parts[0]->provider_metadata_json);Message *copy=message_clone(response->message);assert(copy && str_eq(copy->parts[0]->provider_metadata_json,response->message->parts[0]->provider_metadata_json));message_free(copy);}
        llm_response_free(response);http_fixture_wait(fixture);llm_client_free(client);message_free(message);llm_error_free(&error);
    }
    fixture=http_fixture("{bad",4,200,true);LlmClient *client=llm_client_new();llm_client_add_provider(client,openai_adapter_new("fake-credential",fixture.url));
    Message *message=message_user("hello");LlmRequest req={.model="fake\"model",.messages=&message,.message_count=1};LlmError error={0};
    assert(!llm_client_complete(client,&req,&error) && error.code==LLM_ERR_PROVIDER && !error.retryable);http_fixture_wait(fixture);llm_client_free(client);llm_error_free(&error);
    fixture=http_fixture("fake-credential",15,429,true);client=llm_client_new();llm_client_add_provider(client,openai_adapter_new("fake-credential",fixture.url));
    assert(!llm_client_complete(client,&req,&error) && error.retryable && error.retry_after==0.2 && !strstr(error.message,"fake-credential"));http_fixture_wait(fixture);
    llm_error_free(&error);llm_client_free(client);message_free(message);
}
static void bounded_fuzz(void) {
    uint32_t seed=0x12345678;const char alphabet[]="{}[]\"\\012ab :;()=-&|\n";
    for(size_t iteration=0;iteration<2000;iteration++) {
        char input[96];seed=seed*1664525u+1013904223u;size_t length=seed%95u;
        for(size_t i=0;i<length;i++) {seed=seed*1664525u+1013904223u;input[i]=alphabet[seed%(sizeof(alphabet)-1)];}input[length]=0;
        JsonValue *j=json_parse(input,NULL);char *encoded=j?json_serialize(j):NULL;free(encoded);json_free(j);
        char *error=NULL;DotGraph *g=dot_parse(input,&error);dot_graph_free(g);free(error);error=NULL;
        Stylesheet *style=stylesheet_parse(input,&error);stylesheet_free(style);free(error);(void)condition_validate(input);
    }
}
static void human_input(void) {
    const char *inputs[]={"", "invalid\n", "cancel\n", "invalid\nA\n"};
    int saved=dup(STDIN_FILENO);assert(saved>=0);
    for(size_t i=0;i<4;i++) {
        int pipes[2];assert(pipe(pipes)==0);assert(write(pipes[1],inputs[i],strlen(inputs[i]))==(ssize_t)strlen(inputs[i]));close(pipes[1]);assert(dup2(pipes[0],STDIN_FILENO)>=0);close(pipes[0]);
        DotGraph *g=dot_parse("digraph G { start; exit; gate [shape=hexagon,\"human.default_choice\"=A]; approve [type=probe]; start -> gate; gate -> approve [label=\"[A] Approve\"]; approve -> exit; }",NULL);
        PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);handler_registry_register(&r->handler_reg,"probe",approval,NULL);pipeline_runner_set_interviewer(r,console_interviewer_new());
        approvals=0;Outcome result=pipeline_run(r);assert(result.status==(i==3?STAGE_SUCCESS:STAGE_FAIL) && approvals==(i==3?1:0));outcome_free(&result);pipeline_runner_free(r);dot_graph_free(g);
    }
    assert(dup2(saved,STDIN_FILENO)>=0);close(saved);
}
static Outcome status_handler(const DotNode *node,PipelineContext *ctx,const DotGraph *graph,const char *logs,void *data) {
    (void)node;(void)ctx;(void)graph;(void)logs;return (Outcome){.status=*(StageStatus *)data};
}
static void outcome_routes(void) {
    StageStatus statuses[]={STAGE_PARTIAL_SUCCESS,STAGE_SKIPPED,STAGE_RETRY};
    for(size_t i=0;i<3;i++) {
        DotGraph *g=dot_parse("digraph G { start; exit; a [type=custom,max_retries=0]; start -> a -> exit; }",NULL);assert(g && !validate_or_raise(g,NULL));
        PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);handler_registry_register(&r->handler_reg,"custom",status_handler,&statuses[i]);
        Outcome result=pipeline_run(r);assert(result.status==(i==2?STAGE_FAIL:STAGE_SUCCESS));outcome_free(&result);
        StageStatus failure=STAGE_FAIL;handler_registry_register(&r->handler_reg,"exit",status_handler,&failure);
        if(i==0) {result=pipeline_run(r);assert(result.status==STAGE_FAIL);outcome_free(&result);}
        pipeline_runner_free(r);dot_graph_free(g);
    }
    StrBuf b;strbuf_init(&b);strbuf_append_cstr(&b,"digraph G { start; exit; choose [type=count]; start -> choose; ");
    for(size_t i=0;i<300;i++) strbuf_appendf(&b,"choose -> exit [condition=\"x=%zu\"];",i);
    strbuf_append_cstr(&b,"choose -> exit [condition=\"outcome=success\"]; }");char *source=strbuf_detach(&b);DotGraph *g=dot_parse(source,NULL);free(source);
    PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);handler_registry_register(&r->handler_reg,"count",count_handler,NULL);
    Outcome result=pipeline_run(r);assert(result.status==STAGE_SUCCESS);outcome_free(&result);pipeline_runner_free(r);dot_graph_free(g);
}
static void search_containment(void) {
    ExecutionEnv *env=local_exec_env_new_policy(test_root,true);assert(env);
    assert(env->write_file(env,"sub/space ü.txt","a needle\nError: legitimate output\n"));
    char *found=env->grep(env,"needle",".",NULL,false,10);assert(found && strstr(found,"sub/space ü.txt:1:a needle"));free(found);
    found=env->glob(env,"*.txt",".");assert(found && strstr(found,"sub/space ü.txt"));free(found);
    assert(!env->grep(env,"needle","../",NULL,false,10));assert(!env->glob(env,"*","../"));
    char link[2048];snprintf(link,sizeof(link),"%s/outside",test_root);assert(symlink("/private/tmp",link)==0);
    assert(!env->grep(env,"needle","outside",NULL,false,10));assert(!env->glob(env,"*","outside"));
    found=env->glob(env,"*;touch injected",".");assert(found && !*found);free(found);assert(!env->file_exists(env,"injected"));
    assert(env->write_file(env,"sub/cwd.txt","yes"));ExecResult *result=env->exec_command(env,"test -f cwd.txt",100,"sub");assert(result && result->exit_code==0);exec_result_free(result);
    ToolRegistry registry;tool_registry_init(&registry);agent_register_core_tools(&registry);
    ToolExecutionContext context={.tool=tool_registry_get(&registry,"shell"),.env=env,.default_timeout_ms=100,.max_timeout_ms=100};bool error=true;
    found=agent_active_tool("{\"command\":\"printf 'Error: legitimate output'\"}",&context,&error);assert(found && !error);free(found);
    tool_registry_free(&registry);exec_env_free(env);
}

static Outcome persistent_count(const DotNode *node,PipelineContext *ctx,const DotGraph *graph,const char *logs,void *data) {
    (void)ctx;(void)graph;(void)logs;(void)data;char path[2048];snprintf(path,sizeof(path),"%s/count-%s",test_root,node->id);
    char *old=io_read_text(path,64);long count=old?strtol(old,NULL,10):0;free(old);
    char text[64];snprintf(text,sizeof(text),"%ld",count+1);assert(io_atomic_write(path,text,IO_ATOMIC));return (Outcome){.status=STAGE_SUCCESS};
}
static long count_file(const char *node) {
    char path[2048];snprintf(path,sizeof(path),"%s/count-%s",test_root,node);char *text=io_read_text(path,64);long count=text?strtol(text,NULL,10):0;free(text);return count;
}
static void branch_crash(const PipelineEvent *event,void *userdata) {
    (void)userdata;if(event->kind==PIPE_EVT_CHECKPOINT_SAVED && str_eq(event->node_id,"a2")) _exit(0);
}
static void branch_recovery(void) {
    const char *source="digraph G { start; exit; fork [shape=component]; join [shape=tripleoctagon]; a [type=count]; a2 [type=count]; b [type=count]; start -> fork; fork -> a; fork -> b; a -> a2 -> join; b -> join; join -> exit; }";
    DotGraph *graph=dot_parse(source,NULL);assert(graph);pid_t child=fork();assert(child>=0);
    if(child==0) {
        PipelineRunner *runner=pipeline_runner_new(graph,test_root);pipeline_register_builtin_handlers(runner);handler_registry_register(&runner->handler_reg,"count",persistent_count,NULL);pipeline_runner_on_event(runner,branch_crash,NULL);
        Outcome result=pipeline_run(runner);outcome_free(&result);_exit(3);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    PipelineRunner *runner=pipeline_runner_new(graph,test_root);pipeline_register_builtin_handlers(runner);handler_registry_register(&runner->handler_reg,"count",persistent_count,NULL);
    char path[2048];snprintf(path,sizeof(path),"%s/checkpoint.json",test_root);Outcome result=pipeline_resume(runner,path);
    assert(result.status==STAGE_SUCCESS && count_file("a")==1 && count_file("a2")==1 && count_file("b")==1);outcome_free(&result);
    result=pipeline_run(runner);assert(result.status==STAGE_SUCCESS && count_file("a")==2 && count_file("a2")==2 && count_file("b")==2);outcome_free(&result);
    pipeline_runner_free(runner);dot_graph_free(graph);
}
static void nested_and_policies(void) {
    DotGraph *graph=dot_parse("digraph G { start; exit; outer [shape=component]; inner [shape=component]; ij [shape=tripleoctagon]; oj [shape=tripleoctagon]; a [type=count]; b [type=count]; c [type=count]; a2 [type=count]; start -> outer; outer -> inner; outer -> c; inner -> a; inner -> b; a -> ij; b -> ij; ij -> a2 -> oj; c -> oj; oj -> exit; }",NULL);
    PipelineRunner *runner=pipeline_runner_new(graph,test_root);pipeline_register_builtin_handlers(runner);handler_registry_register(&runner->handler_reg,"count",count_handler,NULL);stage_calls=0;
    Outcome result=pipeline_run(runner);assert(result.status==STAGE_SUCCESS && stage_calls==4);outcome_free(&result);pipeline_runner_free(runner);dot_graph_free(graph);
    const char *policies[]={"wait_all","first_success","quorum"};
    for(size_t i=0;i<3;i++) {
        StrBuf source;strbuf_init(&source);strbuf_appendf(&source,"digraph G { start; exit; fork [shape=component,join_policy=%s,error_policy=continue]; join [shape=tripleoctagon]; a [type=fail]; b [type=count]; c [type=count]; start -> fork; fork -> a; fork -> b; fork -> c; a -> join; b -> join; c -> join; join -> exit; }",policies[i]);
        char *text=strbuf_detach(&source);graph=dot_parse(text,NULL);free(text);runner=pipeline_runner_new(graph,test_root);pipeline_register_builtin_handlers(runner);
        handler_registry_register(&runner->handler_reg,"fail",fail_handler,NULL);handler_registry_register(&runner->handler_reg,"count",count_handler,NULL);stage_calls=0;
        result=pipeline_run(runner);assert(result.status==(i==0?STAGE_FAIL:STAGE_SUCCESS));assert(stage_calls==(i==1?1:2));outcome_free(&result);pipeline_runner_free(runner);dot_graph_free(graph);
    }
    graph=dot_parse("digraph G { start; exit; fork [shape=component]; join [shape=tripleoctagon]; a; b; start -> fork; fork -> a; fork -> b; a -> a; a -> join; b -> join; join -> exit; }",NULL);
    assert(graph && !validate_or_raise(graph,NULL));dot_graph_free(graph);
    graph=dot_parse("digraph G { start; exit; work [type=count]; start -> work -> exit; }",NULL);runner=pipeline_runner_new(graph,test_root);pipeline_register_builtin_handlers(runner);handler_registry_register(&runner->handler_reg,"count",count_handler,NULL);
    bool cancelled=true;runner->cancel=&cancelled;stage_calls=0;result=pipeline_run(runner);assert(result.status==STAGE_FAIL && stage_calls==0);outcome_free(&result);pipeline_runner_free(runner);dot_graph_free(graph);
}
static void construction_failures(void) {
    ExecutionEnv *env=local_exec_env_new(test_root);LlmClient *client=llm_client_new();ProviderProfile *profile=openai_profile_new("fake");assert(env && client && profile);
    for(long i=0;i<160;i++) {
        mem_fail_after(i);ProviderProfile *temporary=openai_profile_new("fake");mem_fail_after(-1);provider_profile_free(temporary);
        mem_fail_after(i);AgentSession *session=agent_session_new(profile,env,client,agent_default_config());mem_fail_after(-1);agent_session_free(session);
        mem_fail_after(i);ProviderAdapter *adapter=openai_adapter_new("fake-credential","https://example.invalid");LlmClient *temporary_client=llm_client_new();if(temporary_client) llm_client_add_provider(temporary_client,adapter);else if(adapter) {if(adapter->close) adapter->close(adapter);free(adapter->impl);free(adapter->name);free(adapter);}mem_fail_after(-1);llm_client_free(temporary_client);
    }
    provider_profile_free(profile);llm_client_free(client);exec_env_free(env);
    setenv("OPENAI_API_KEY","invalid\ncredential",1);assert(!llm_client_from_env());unsetenv("OPENAI_API_KEY");
}

static void checkpoint_validation(void) {
    DotGraph *graph=dot_parse("digraph G { start; exit; start -> exit; }",NULL);PipelineRunner *runner=pipeline_runner_new(graph,test_root);pipeline_register_builtin_handlers(runner);
    Outcome outcome=pipeline_run(runner);assert(outcome.status==STAGE_SUCCESS);outcome_free(&outcome);
    char path[2048];snprintf(path,sizeof(path),"%s/checkpoint.json",test_root);char *valid=io_read_text(path,ATTRACTOR_INPUT_LIMIT);assert(valid);
    Checkpoint cp={0};assert(checkpoint_load(&cp,path));char *run=str_dup(cp.run_id);assert(run);
    for(int variant=0;variant<6;variant++) {
        JsonValue *j=json_parse(valid,NULL);assert(j);
        if(variant==0) json_object_set(j,"version",json_new_number(3));
        if(variant==1) json_object_set(j,"complete",json_new_string("true"));
        if(variant==2) json_object_set(json_get(j,"context"),"bad",json_new_number(1));
        if(variant==3) json_object_set(j,"pending_node",json_new_string("start"));
        if(variant==4) json_object_set(json_array_get(json_get(j,"history"),0),"attempt",json_new_string("0"));
        if(variant==5) json_object_set(json_get(j,"final_outcome"),"notes",json_new_number(1));
        char *encoded=json_serialize(j);json_free(j);assert(encoded && io_atomic_write(path,encoded,IO_ATOMIC));free(encoded);
        assert(!checkpoint_load(&cp,path) && str_eq(cp.run_id,run));
    }
    assert(io_atomic_write(path,valid,IO_ATOMIC));
    for(long i=0;i<220;i++) {mem_fail_after(i);(void)checkpoint_load(&cp,path);mem_fail_after(-1);assert(str_eq(cp.run_id,run));}
    JsonValue *j=json_parse(valid,NULL);json_object_set(j,"complete",json_new_bool(false));json_object_set(j,"next_node",json_new_string("start"));json_object_set(j,"pending_node",json_new_string("start"));
    char *encoded=json_serialize(j);json_free(j);assert(io_atomic_write(path,encoded,IO_ATOMIC));free(encoded);outcome=pipeline_resume(runner,path);assert(outcome.status==STAGE_FAIL && strstr(outcome.failure_reason,"ambiguous"));outcome_free(&outcome);
    assert(io_atomic_write(path,valid,IO_ATOMIC));free(graph->label);graph->label=str_dup("changed graph");outcome=pipeline_resume(runner,path);assert(outcome.status==STAGE_FAIL && strstr(outcome.failure_reason,"mismatch"));outcome_free(&outcome);
    checkpoint_free(&cp);free(run);free(valid);pipeline_runner_free(runner);dot_graph_free(graph);
}
static Answer timeout_answer(Interviewer *self,const Question *question) {(void)self;(void)question;return (Answer){.kind=ANSWER_TIMEOUT,.option_index=-1};}
static void human_timeout(void) {
    int pipes[2],saved=dup(STDIN_FILENO);assert(saved>=0 && pipe(pipes)==0);assert(dup2(pipes[0],STDIN_FILENO)>=0);close(pipes[0]);
    Interviewer *interviewer=console_interviewer_new();Question question={.text="timeout",.timeout_ms=50};Answer answer=interviewer->ask(interviewer,&question);assert(answer.kind==ANSWER_TIMEOUT);answer_free(&answer);interviewer_free(interviewer);close(pipes[1]);assert(dup2(saved,STDIN_FILENO)>=0);close(saved);
    DotGraph *graph=dot_parse("digraph G { start; exit; gate [shape=hexagon,\"human.default_choice\"=A]; approve [type=probe]; start -> gate; gate -> approve [label=\"[A] Approve\"]; approve -> exit; }",NULL);
    PipelineRunner *runner=pipeline_runner_new(graph,test_root);pipeline_register_builtin_handlers(runner);handler_registry_register(&runner->handler_reg,"probe",approval,NULL);
    interviewer=calloc(1,sizeof(*interviewer));interviewer->ask=timeout_answer;pipeline_runner_set_interviewer(runner,interviewer);approvals=0;
    Outcome outcome=pipeline_run(runner);assert(outcome.status==STAGE_SUCCESS && approvals==1);outcome_free(&outcome);pipeline_runner_free(runner);dot_graph_free(graph);
}
static void filesystem_permissions(void) {
    const char *ids[]={"A","a","space ü","../escape","/absolute","-dash"};
    for(size_t i=0;i<sizeof(ids)/sizeof(*ids);i++) {
        assert(io_artifact_write(test_root,ids[i],"private.txt",ids[i]));char *safe=io_artifact_name(ids[i]);StrBuf path;strbuf_init(&path);strbuf_appendf(&path,"%s/%s/private.txt",test_root,safe);free(safe);char *name=strbuf_detach(&path);
        struct stat st;assert(stat(name,&st)==0 && (st.st_mode&0777)==0600);char *text=io_read_text(name,1024);assert(str_eq(text,ids[i]));free(text);free(name);
    }
    int directory=open(test_root,O_RDONLY|O_DIRECTORY);assert(directory>=0);FILE *file=fdopen(directory,"r");assert(file && !io_read_stream(file,100,NULL));fclose(file);
    bool cancel=true;const char *argv[]={"/bin/sh","-c","printf SHOULD_NOT_EXECUTE",NULL};ExecResult *result=process_run(&(ProcessOptions){.argv=argv,.cancel=&cancel,.timeout_ms=100});assert(result && result->cancelled && !*result->stdout_buf);exec_result_free(result);
    const DotEdge *edges[1];DotGraph *graph=dot_parse("digraph G { a -> b; a -> c; }",NULL);assert(dot_outgoing_edges(graph,"a",edges,1)==2 && dot_outgoing_edges(graph,"a",NULL,0)==2);dot_graph_free(graph);
}

typedef struct {const char *name; void (*run)(void); bool expected_failure;} Case;
static const Case cases[] = {
    {"checkpoint validation / allocation / ambiguity",checkpoint_validation,false}, {"human timeout policy",human_timeout,false}, {"artifact permissions / names / I/O errors",filesystem_permissions,false},
    {"branch committed recovery / new run",branch_recovery,false}, {"nested forks / join policies / cancellation",nested_and_policies,false}, {"session / provider construction failures",construction_failures,false},
    {"local HTTP transport / provider contracts",local_transport,false}, {"bounded parser fuzz",bounded_fuzz,false}, {"human EOF / invalid / cancellation",human_input,false}, {"partial / skipped / retry / large routing",outcome_routes,false}, {"recursive search containment",search_containment,false},
    {"generation ownership / userdata / middleware",generation_contracts,false}, {"structured tool errors / duration policy",structured_tool_errors,false}, {"parser / clone allocation failures",parser_allocation_failures,false},
    {"DOT fixture outcomes / counts",fixture_assertions,false}, {"complete isolated branches",complete_branches,false}, {"branch conflicts",branch_conflict,false}, {"child inherited backend / failures",child_configuration,false},
    {"checkpoint committed recovery",checkpoint_recovery,false}, {"checkpoint atomic fault injection",atomic_faults,false}, {"latest goal outcome",latest_gate,false},
    {"condition grammar",condition_syntax,false}, {"routing failure / recovery / budgets",routing_failures,false},
    {"process absolute deadlines / environment",process_deadlines,false}, {"secure files / injection",secure_files,false}, {"mock environment edit",mock_edit,false},
    {"strict JSON / Unicode / numbers",strict_json,false}, {"allocation failures",allocation_failures,false}, {"bounded stream I/O",bounded_io,false},
    {"truncated JSON",json_truncated,false}, {"truncated stylesheet",stylesheet_truncated,false},
    {"context aliases",ctx_alias,false}, {"zero header capacity",header_zero,false},
    {"negative retries",negative_retry,false}, {"empty edit",empty_edit,false},
    {"fan-in ownership",fanin,false}, {"missing interviewer",human_missing,false},
};
static void remove_tree(const char *path) {
    DIR *dir=opendir(path); if(!dir) {unlink(path); return;}
    struct dirent *entry;
    while((entry=readdir(dir))) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char child[2048]; snprintf(child,sizeof(child),"%s/%s",path,entry->d_name);
        struct stat st; if(lstat(child,&st)==0 && S_ISDIR(st.st_mode)) remove_tree(child); else unlink(child);
    }
    closedir(dir); rmdir(path);
}
int main(int argc,char **argv) {
    bool leak_check=argc==2 && str_eq(argv[1],"--leaks");
    bool inline_run=leak_check || (argc==2 && str_eq(argv[1],"--inline"));
    setvbuf(stdout,NULL,_IOLBF,0);
    int failures=0;
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);i++) {
        /* leaks --atExit stops fork children at exit; their waiting parent
         * cannot finish. These cases run under the ordinary sanitizer suite. */
        if(leak_check && (cases[i].run==local_transport || cases[i].run==checkpoint_recovery || cases[i].run==branch_recovery)) {
            printf("%s: SKIP (fork child exit instrumentation)\n",cases[i].name);continue;
        }
        strcpy(test_root,"/private/tmp/attractor-regression-XXXXXX"); assert(mkdtemp(test_root)); fflush(NULL);
        if(inline_run) {cases[i].run();printf("%s: PASS\n",cases[i].name);remove_tree(test_root);continue;}
        pid_t pid=fork(); assert(pid>=0);
        if(pid==0) {cases[i].run(); exit(0);}
        int status=0; bool finished=false;
        for(int tick=0;tick<500;tick++) {
            if(waitpid(pid,&status,WNOHANG)==pid) {finished=true;break;}
            struct timespec pause={0,10000000}; nanosleep(&pause,NULL);
        }
        if(!finished) {kill(pid,SIGKILL); waitpid(pid,&status,0);}
        bool passed=finished && WIFEXITED(status) && WEXITSTATUS(status)==0;
        printf("%s: %s%s\n",cases[i].name,passed?"PASS":"FAIL",cases[i].expected_failure?" (known defect)":"");
        if(!passed && !cases[i].expected_failure) failures++;
        remove_tree(test_root);
    }
    if(!failures) puts("All selected regressions passed");
    return failures?1:0;
}
