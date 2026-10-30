#include "util/json.h"
#include "util/str.h"
#include "util/http.h"
#include "util/mem.h"
#include "util/io.h"
#include <stdint.h>
#include <math.h>
#include "agent/agent.h"
#include "attractor/engine.h"
#include "attractor/agent_backend.h"
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
static LlmResponse *tool_call_response(const char *name,const char *args) {
    LlmResponse *r=calloc(1,sizeof(*r));r->message=message_assistant(NULL);r->message->part_count=1;r->message->parts=calloc(1,sizeof(ContentPart *));
    ContentPart *p=calloc(1,sizeof(*p));p->kind=CONTENT_TOOL_CALL;p->tool_call=calloc(1,sizeof(*p->tool_call));
    p->tool_call->id=str_dup("call");p->tool_call->name=str_dup(name);p->tool_call->arguments_json=str_dup(args);r->message->parts[0]=p;
    r->tool_call_count=1;r->tool_calls=calloc(1,sizeof(ToolCall *));ToolCall *call=calloc(1,sizeof(*call));
    call->id=str_dup("call");call->name=str_dup(name);call->arguments_json=str_dup(args);r->tool_calls[0]=call;r->finish_reason.reason=FINISH_TOOL_CALLS;return r;
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
    assert(s && str_eq(s,"Replaced 2 occurrence(s)"));free(s);
    bool error=false;s=tool_registry_get(&reg,"edit_file")->execute("{\"file_path\":\"virtual\",\"old_string\":\"abc\",\"new_string\":\"xyz\"}",&env,&error);
    assert(s && error && strstr(s,"not unique"));free(s);tool_registry_free(&reg);
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
static char *count_backend(CodergenBackend *self,const DotNode *node,const char *prompt,const PipelineContext *ctx,const char *stage_dir,Outcome *outcome) {
    (void)self;(void)prompt;(void)ctx;(void)stage_dir;(void)outcome;backend_calls++;return str_dup(node->id);
}
static char *verdict_backend(CodergenBackend *self,const DotNode *node,const char *prompt,const PipelineContext *ctx,const char *stage_dir,Outcome *outcome) {
    (void)self;(void)prompt;(void)ctx;(void)stage_dir;
    if(str_eq(node->id,"review")) {outcome->status=STAGE_FAIL;outcome->failure_reason=str_dup("tests failed");}
    if(str_eq(node->id,"pick")) outcome->preferred_label=str_dup("Fix");
    return str_dup(node->id);
}
static Outcome retry_handler(const DotNode *n,PipelineContext *c,const DotGraph *g,const char *l,void *d) {
    (void)n;(void)c;(void)g;(void)l;(void)d;stage_calls++;return (Outcome){.status=STAGE_RETRY};
}
static void stylesheet_selectors(void) {
    char *err=NULL;
    Stylesheet *ss=stylesheet_parse("* { llm_provider: p; } box { llm_model: m1; } .b { llm_model: m2; } .b { llm_model: m3; } #y { reasoning_effort: low; }",&err);
    assert(ss && !err);
    DotGraph *g=dot_parse("digraph G { x [shape=box]; y [shape=box,class=\"a, b\"]; z [shape=ellipse,class=\"ab\"]; w [shape=box,llm_model=own] }",NULL);
    stylesheet_apply(ss,g);
    const DotNode *x=dot_find_node(g,"x"),*y=dot_find_node(g,"y"),*z=dot_find_node(g,"z"),*w=dot_find_node(g,"w");
    assert(str_eq(x->llm_model,"m1") && str_eq(x->llm_provider,"p") && !x->reasoning_effort);
    assert(str_eq(y->llm_model,"m3") && str_eq(y->reasoning_effort,"low"));
    assert(!z->llm_model && str_eq(z->llm_provider,"p") && str_eq(w->llm_model,"own"));
    stylesheet_free(ss);dot_graph_free(g);
    assert(!stylesheet_parse("bad-shape! { llm_model: m; }",&err) && err);free(err);
}
static void retry_defaults(void) {
    const char *graphs[]={"digraph G { a }","digraph G { graph [default_max_retries=4]; a }","digraph G { graph [default_max_retry=3]; a }"};
    const int expected[]={0,4,3};
    for(size_t i=0;i<3;i++) {DotGraph *g=dot_parse(graphs[i],NULL);assert(g && g->default_max_retry==expected[i]);dot_graph_free(g);}
    DotGraph *g=dot_parse("digraph G { start; exit; a [type=again]; start -> a -> exit; }",NULL);
    PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);handler_registry_register(&r->handler_reg,"again",retry_handler,NULL);
    stage_calls=0;Outcome o=pipeline_run(r);assert(stage_calls==1);outcome_free(&o);pipeline_runner_free(r);dot_graph_free(g);
}
static const char *status_payload;
static char *status_writer(CodergenBackend *self,const DotNode *node,const char *prompt,const PipelineContext *ctx,const char *stage_dir,Outcome *outcome) {
    (void)self;(void)prompt;(void)ctx;(void)outcome;
    char path[2048];snprintf(path,sizeof(path),"%s/status.json",stage_dir);
    if(status_payload && str_eq(node->id,"review")) assert(io_atomic_write(path,status_payload,IO_ATOMIC));
    return str_dup(node->id);
}
static void status_file_contract(void) {
    const char *graph="digraph G { start; exit; review [shape=box]; fix [type=count]; start -> review; review -> exit [label=Ship,weight=9]; review -> fix [label=Fix]; fix -> exit; }";
    const char *payloads[]={"{\"outcome\":\"partial_success\",\"preferred_label\":\"Fix\",\"notes\":\"lint\",\"context_updates\":{\"verdict\":\"bad\"}}",NULL,"{not json"};
    const int expected_calls[]={1,0,0};const StageStatus expected_status[]={STAGE_SUCCESS,STAGE_SUCCESS,STAGE_FAIL};
    for(size_t i=0;i<3;i++) {
        DotGraph *g=dot_parse(graph,NULL);assert(g);char logs[1200];snprintf(logs,sizeof(logs),"%s/run%zu",test_root,i);
        PipelineRunner *r=pipeline_runner_new(g,logs);pipeline_register_builtin_handlers(r);handler_registry_register(&r->handler_reg,"count",count_handler,NULL);
        /* A stale file from an earlier run must not leak into this attempt. */
        char *dir=io_artifact_name("review");char stale[2048];snprintf(stale,sizeof(stale),"%s/%s",logs,dir);free(dir);
        assert(io_mkdirs(stale));strcat(stale,"/status.json");assert(io_atomic_write(stale,"{\"outcome\":\"fail\",\"preferred_label\":\"Fix\"}",IO_ATOMIC));
        CodergenBackend backend={.run=status_writer};pipeline_runner_set_backend(r,&backend);status_payload=payloads[i];stage_calls=0;
        Outcome o=pipeline_run(r);assert(o.status==expected_status[i] && stage_calls==expected_calls[i]);outcome_free(&o);
        if(i==0) {
            char path[2048];snprintf(path,sizeof(path),"%s/checkpoint.json",logs);Checkpoint cp={0};
            assert(checkpoint_load(&cp,path) && str_eq(ctx_get(&cp.context,"verdict",""),"bad"));checkpoint_free(&cp);
        }
        pipeline_runner_free(r);dot_graph_free(g);
    }
}
static void codergen_outcomes(void) {
    const char *graphs[]={
      "digraph G { start; exit; review [shape=box]; fix [type=count]; start -> review; review -> exit [condition=\"outcome=success\"]; review -> fix [condition=\"outcome=fail\"]; fix -> exit; }",
      "digraph G { start; exit; pick [shape=box]; fix [type=count]; start -> pick; pick -> exit [label=Ship,weight=9]; pick -> fix [label=Fix]; fix -> exit; }"
    };
    for(size_t i=0;i<sizeof(graphs)/sizeof(*graphs);i++) {
        DotGraph *g=dot_parse(graphs[i],NULL);assert(g);
        PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);handler_registry_register(&r->handler_reg,"count",count_handler,NULL);
        CodergenBackend backend={.run=verdict_backend};pipeline_runner_set_backend(r,&backend);stage_calls=0;
        Outcome o=pipeline_run(r);assert(o.status==STAGE_SUCCESS && stage_calls==1);outcome_free(&o);
        if(i==0) {
            char *dir=io_artifact_name("review");char path[2048];snprintf(path,sizeof(path),"%s/%s/status.json",test_root,dir);free(dir);
            char *s=io_read_text(path,65536);assert(s && strstr(s,"\"fail\"") && strstr(s,"tests failed"));free(s);
        } else {
            char *dir=io_artifact_name("pick");char path[2048];snprintf(path,sizeof(path),"%s/%s/status.json",test_root,dir);free(dir);
            char *s=io_read_text(path,65536);assert(s && strstr(s,"\"preferred_label\":\"Fix\""));free(s);
        }
        pipeline_runner_free(r);dot_graph_free(g);
    }
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
    /* Spec 3.8: branch context changes stay in the branch; only parallel.results reaches the parent. */
    assert(!ctx_get(&cp.context,"a.output",NULL) && !ctx_get(&cp.context,"b.output",NULL));
    JsonValue *results=json_parse(ctx_get(&cp.context,"parallel.results","null"),NULL);assert(results && results->type==JSON_ARRAY && results->array.count==2);json_free(results);
    for(size_t i=0;i<cp.context.count;i++) assert(!strstr(cp.context.keys[i],".output"));
    checkpoint_free(&cp);pipeline_runner_free(r);dot_graph_free(g);
}
static Outcome conflicting_branch(const DotNode *n,PipelineContext *c,const DotGraph *g,const char *l,void *d) {
    (void)g;(void)l;(void)d;ctx_set(c,"shared",n->id);return (Outcome){.status=STAGE_SUCCESS};
}
static void branch_conflict(void) {
    DotGraph *g=dot_parse("digraph G { start; exit; fork [shape=component]; a [type=branch]; b [type=branch]; start -> fork; fork -> a; fork -> b; a -> exit; b -> exit; }",NULL);
    PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);handler_registry_register(&r->handler_reg,"branch",conflicting_branch,NULL);
    Outcome o=pipeline_run(r);assert(o.status==STAGE_SUCCESS);outcome_free(&o);
    char path[2048];snprintf(path,sizeof(path),"%s/checkpoint.json",test_root);Checkpoint cp={0};assert(checkpoint_load(&cp,path) && !ctx_get(&cp.context,"shared",NULL));
    checkpoint_free(&cp);pipeline_runner_free(r);dot_graph_free(g);
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
static int probe_calls,probe_runs;
static LlmResponse *always_probe(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {(void)self;(void)req;(void)err;probe_calls++;return tool_call_response("probe","{}");}
static char *run_probe(const char *arguments,void *userdata,bool *is_error) {(void)arguments;(void)userdata;(void)is_error;probe_runs++;return str_dup("done");}
static void tool_round_budget(void) {
    LlmClient *c=llm_client_new();ProviderAdapter *adapter=calloc(1,sizeof(*adapter));adapter->name=str_dup("fake");adapter->complete=always_probe;llm_client_add_provider(c,adapter);
    ActiveTool active={.def={.name="probe",.parameters_json="{}"},.execute=run_probe},passive={.def={.name="probe",.parameters_json="{}"}};
    const struct {ActiveTool *tool;int rounds,calls,runs;} cases_[]={{&active,0,1,0},{&active,2,3,2},{&passive,5,1,0}};
    for(size_t i=0;i<3;i++) {
        probe_calls=probe_runs=0;LlmError e={0};
        GenerateResult *r=llm_generate(c,"fake","hello",NULL,0,NULL,cases_[i].tool,1,cases_[i].rounds,REASONING_NONE,"fake",0,&e);
        assert(r && r->tool_call_count==1 && str_eq(r->tool_calls[0]->name,"probe") && probe_calls==cases_[i].calls && probe_runs==cases_[i].runs);
        generate_result_free(r);llm_error_free(&e);
    }
    llm_client_free(c);
}
static void generation_contracts(void) {
    LlmClient *c=llm_client_new();ProviderAdapter *adapter=calloc(1,sizeof(*adapter));adapter->name=str_dup("fake");adapter->complete=tool_loop_provider;llm_client_add_provider(c,adapter);
    ActiveTool tool={.def={.name="probe",.description="probe",.parameters_json="{}"},.execute=context_tool};
    for(size_t i=0;i<20;i++) {
        tool.userdata=i%2?"session B":"session A";expect_tool_error=i%2==0;LlmError e={0};
        GenerateResult *r=llm_generate(c,"fake","hello",NULL,0,NULL,&tool,1,1,REASONING_NONE,"fake",0,&e);
        assert(r && r->response && str_eq(r->text,"ok"));
        /* Per-step history: the tool round, then the final answer (spec 4.3). */
        assert(r->step_count==2 && r->steps[0].response->tool_call_count==1 && r->steps[0].tool_result_count==1 && r->steps[1].tool_result_count==0);
        assert(str_eq(r->steps[0].tool_results[0]->content,tool.userdata) && r->steps[0].tool_results[0]->is_error==expect_tool_error && str_eq(r->steps[0].tool_results[0]->tool_call_id,"call"));
        assert(r->response==r->steps[1].response && r->total_usage.input_tokens==r->steps[0].response->usage.input_tokens+r->steps[1].response->usage.input_tokens);
        generate_result_free(r);llm_error_free(&e);
    }
    adapter->complete=fake_complete;llm_client_add_middleware(c,middleware_a);llm_client_add_middleware(c,middleware_b);
    middleware_count=0;LlmError e={0};GenerateResult *r=llm_generate(c,"fake","hello",NULL,0,NULL,NULL,0,0,REASONING_NONE,"fake",0,&e);
    assert(r && middleware_count==4);for(size_t i=0;i<4;i++) assert(middleware_order[i]==(int)i+1);generate_result_free(r);
    LlmRequest request={.model="fake",.reasoning_effort=(ReasoningEffort)99};
    assert(!llm_client_complete(c,&request,&e) && e.code==LLM_ERR_CONFIG);
    ToolChoice choice={.mode=TOOL_CHOICE_NAMED};request.reasoning_effort=REASONING_NONE;request.tool_choice=&choice;
    assert(!llm_client_complete(c,&request,&e) && e.code==LLM_ERR_CONFIG);
    assert(!llm_client_stream(c,NULL,NULL,NULL,&e) && e.code==LLM_ERR_CONFIG);llm_error_free(&e);llm_client_free(c);
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
            json_free(j);char saved[2048];snprintf(saved,sizeof(saved),"%s/request.json",test_root);assert(io_atomic_write(saved,content,IO_ATOMIC));
            content[-4]=0;snprintf(saved,sizeof(saved),"%s/request.headers",test_root);assert(io_atomic_write(saved,request,IO_ATOMIC));
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
    llm_error_free(&error);llm_client_free(client);
    /* Ambiguous statuses are refined by the provider's error message. */
    const char *context_body="{\"error\":{\"type\":\"invalid_request_error\",\"message\":\"prompt is too long: maximum context length is 200000\"}}";
    fixture=http_fixture(context_body,strlen(context_body),400,true);client=llm_client_new();llm_client_add_provider(client,anthropic_adapter_new("fake-credential",fixture.url));
    req.provider="anthropic";assert(!llm_client_complete(client,&req,&error) && error.code==LLM_ERR_CONTEXT_LENGTH && !error.retryable && strstr(error.message,"maximum context length"));
    http_fixture_wait(fixture);llm_error_free(&error);llm_client_free(client);message_free(message);
}
static void gemini_call_ids(void) {
    const char *payload="{\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":{\"name\":\"a\",\"args\":{}}},{\"functionCall\":{\"name\":\"b\",\"args\":{}}}]},\"finishReason\":\"STOP\"}]}";
    char ids[4][64];Message *message=message_user("hello");
    for(size_t turn=0;turn<2;turn++) {
        HttpFixture fixture=http_fixture(payload,strlen(payload),200,true);LlmClient *client=llm_client_new();llm_client_add_provider(client,gemini_adapter_new("fake-credential",fixture.url));
        LlmRequest req={.model="fake",.provider="gemini",.messages=&message,.message_count=1,.temperature=-1,.top_p=-1};LlmError error={0};
        LlmResponse *r=llm_client_complete(client,&req,&error);assert(r && r->tool_call_count==2);
        for(size_t i=0;i<2;i++) {assert(str_starts_with(r->tool_calls[i]->id,"call_"));snprintf(ids[turn*2+i],sizeof(ids[0]),"%s",r->tool_calls[i]->id);}
        llm_response_free(r);http_fixture_wait(fixture);llm_client_free(client);llm_error_free(&error);
    }
    for(size_t i=0;i<4;i++) for(size_t j=i+1;j<4;j++) assert(!str_eq(ids[i],ids[j]));
    message_free(message);
}
static void generate_prompt_xor_messages(void) {
    LlmClient *c=llm_client_new();ProviderAdapter *adapter=calloc(1,sizeof(*adapter));adapter->name=str_dup("fake");adapter->complete=fake_complete;llm_client_add_provider(c,adapter);
    Message *m=message_user("hello");LlmError e={0};
    assert(!llm_generate(c,"fake","hello",&m,1,NULL,NULL,0,0,REASONING_NONE,"fake",0,&e) && e.code==LLM_ERR_CONFIG);llm_error_free(&e);
    GenerateResult *r=llm_generate(c,"fake",NULL,&m,1,NULL,NULL,0,0,REASONING_NONE,"fake",0,&e);assert(r && str_eq(r->text,"ok"));generate_result_free(r);
    message_free(m);llm_client_free(c);
}
static bool has_diagnostic(const DiagnosticList *dl,const char *rule,DiagSeverity severity) {
    for(size_t i=0;i<dl->count;i++) if(str_eq(dl->items[i].rule,rule) && dl->items[i].severity==severity) return true;
    return false;
}
static void validator_severities(void) {
    /* Orphans and unknown types warn; the unknown type falls back to codergen. */
    DotGraph *g=dot_parse("digraph G { start [shape=Mdiamond]; exit [shape=Msquare]; orphan [shape=box,prompt=\"x\"]; "
                          "odd [type=\"custom.kind\",prompt=\"x\"]; weird [shape=egg,prompt=\"x\"]; start -> odd -> weird -> exit; }",NULL);
    DiagnosticList dl=validate_graph(g);
    assert(!diagnostic_list_has_errors(&dl) && has_diagnostic(&dl,"reachability",DIAG_WARNING) && has_diagnostic(&dl,"type_known",DIAG_WARNING));
    diagnostic_list_free(&dl);
    PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);
    assert(str_eq(handler_registry_resolve(&r->handler_reg,dot_find_node(g,"odd"))->type_name,"codergen"));
    Outcome o=pipeline_run(r);assert(o.status==STAGE_SUCCESS);outcome_free(&o);pipeline_runner_free(r);dot_graph_free(g);
    /* Exactly one terminal node. */
    g=dot_parse("digraph G { start [shape=Mdiamond]; a [shape=Msquare]; b [shape=Msquare]; start -> a; start -> b; }",NULL);
    dl=validate_graph(g);assert(has_diagnostic(&dl,"terminal_node",DIAG_ERROR));diagnostic_list_free(&dl);dot_graph_free(g);
}
static void checkpoint_progress(void) {
    DotGraph *g=dot_parse("digraph G { start [shape=Mdiamond]; a [shape=box,prompt=\"x\"]; exit [shape=Msquare]; start -> a -> exit; }",NULL);
    PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);Outcome o=pipeline_run(r);assert(o.status==STAGE_SUCCESS);outcome_free(&o);
    char path[1100];snprintf(path,sizeof(path),"%s/checkpoint.json",test_root);
    char *text=io_read_text(path,1<<20);JsonValue *j=json_parse(text,NULL);free(text);JsonValue *done=json_get(j,"completed_nodes");
    assert(str_eq(json_get_string(j,"current_node"),"exit") && done && done->type==JSON_ARRAY && done->array.count==3);
    assert(str_eq(done->array.items[0]->string,"start") && str_eq(done->array.items[1]->string,"a") && str_eq(done->array.items[2]->string,"exit"));json_free(j);
    Checkpoint cp={0};assert(checkpoint_load(&cp,path) && str_eq(cp.current_node,"exit") && cp.completed_nodes->array.count==3);checkpoint_free(&cp);
    pipeline_runner_free(r);dot_graph_free(g);
}
static int restart_calls,restart_failures;
static Outcome restart_handler(const DotNode *node,PipelineContext *ctx,const DotGraph *graph,const char *logs,void *data) {
    (void)node;(void)ctx;(void)graph;(void)logs;(void)data;return (Outcome){.status=restart_calls++<restart_failures?STAGE_FAIL:STAGE_SUCCESS};
}
static JsonValue *load_checkpoint_json(const char *dir) {
    char path[1200];snprintf(path,sizeof(path),"%s/checkpoint.json",dir);char *text=io_read_text(path,1<<20);JsonValue *j=text?json_parse(text,NULL):NULL;free(text);return j;
}
static void loop_restart(void) {
    DotGraph *g=dot_parse("digraph G { start [shape=Mdiamond]; work [type=flaky]; exit [shape=Msquare]; start -> work; "
                          "work -> exit [condition=\"outcome=success\"]; work -> work [condition=\"outcome=fail\",loop_restart=true]; }",NULL);
    PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);handler_registry_register(&r->handler_reg,"flaky",restart_handler,NULL);
    restart_calls=0;restart_failures=1;Outcome o=pipeline_run(r);assert(o.status==STAGE_SUCCESS && restart_calls==2 && str_eq(r->logs_root,test_root));outcome_free(&o);
    /* The first run ends at the restart; the second has its own log directory and starts at the edge target. */
    JsonValue *first=load_checkpoint_json(test_root);assert(json_get_bool(first,"complete",false) && json_get(first,"completed_nodes")->array.count==2);json_free(first);
    char fresh[1100];snprintf(fresh,sizeof(fresh),"%s/restart-1",test_root);JsonValue *second=load_checkpoint_json(fresh);
    JsonValue *done=json_get(second,"completed_nodes");assert(done && done->array.count==2 && str_eq(done->array.items[0]->string,"work") && str_eq(done->array.items[1]->string,"exit"));json_free(second);
    /* Restarts are bounded. */
    r->max_restarts=2;restart_calls=0;restart_failures=100;o=pipeline_run(r);assert(o.status==STAGE_FAIL && restart_calls==3 && strstr(o.failure_reason,"Restart"));outcome_free(&o);
    pipeline_runner_free(r);dot_graph_free(g);
}
static int callback_asks;
static Answer pick_alpha(const Question *q,void *userdata) {
    assert(q->type==QUESTION_MULTIPLE_CHOICE && q->option_count==2);(*(int *)userdata)++;
    return (Answer){.kind=ANSWER_SELECTION,.value=str_dup("A"),.option_index=-1};
}
static void interviewers(void) {
    const char *source="digraph G { start [shape=Mdiamond]; gate [shape=hexagon,label=\"Pick\"]; a; b; exit [shape=Msquare]; "
                       "start -> gate; gate -> a [label=\"[A] Alpha\"]; gate -> b [label=\"[B] Beta\"]; a -> exit; b -> exit; }";
    /* Queue: answers in order, then SKIPPED once empty. */
    const char *answers[]={"B"};Interviewer *queue=queue_interviewer_new(answers,1);assert(queue);
    DotGraph *g=dot_parse(source,NULL);PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);pipeline_runner_set_interviewer(r,queue);
    Outcome o=pipeline_run(r);assert(o.status==STAGE_SUCCESS);outcome_free(&o);
    JsonValue *cp=load_checkpoint_json(test_root);JsonValue *done=json_get(cp,"completed_nodes");assert(done->array.count==4 && str_eq(done->array.items[2]->string,"b"));json_free(cp);
    Question q={.text="again",.type=QUESTION_YES_NO};Answer empty=queue->ask(queue,&q);assert(empty.kind==ANSWER_SKIPPED);answer_free(&empty);
    o=pipeline_run(r);assert(o.status==STAGE_FAIL);outcome_free(&o);pipeline_runner_free(r);
    /* Callback: delegated with its userdata. */
    callback_asks=0;r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);pipeline_runner_set_interviewer(r,callback_interviewer_new(pick_alpha,&callback_asks));
    o=pipeline_run(r);assert(o.status==STAGE_SUCCESS && callback_asks==1);outcome_free(&o);
    cp=load_checkpoint_json(test_root);done=json_get(cp,"completed_nodes");assert(str_eq(done->array.items[2]->string,"a"));json_free(cp);
    pipeline_runner_free(r);dot_graph_free(g);
}
static void tag_prompts(DotGraph *g,void *userdata) {
    (*(int *)userdata)++;
    for(size_t i=0;i<g->node_count;i++) if(g->nodes[i].prompt) {
        StrBuf b;strbuf_init(&b);strbuf_appendf(&b,"%s+%d",g->nodes[i].prompt,*(int *)userdata);free(g->nodes[i].prompt);g->nodes[i].prompt=strbuf_detach(&b);
    }
}
static void break_graph(DotGraph *g,void *userdata) {(void)userdata;g->failed=true;}
static void custom_transforms(void) {
    DotGraph *g=dot_parse("digraph G { start [shape=Mdiamond]; a [shape=box,prompt=\"p\"]; exit [shape=Msquare]; start -> a -> exit; }",NULL);
    PipelineRunner *r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);int first=0,second=10;
    assert(pipeline_runner_register_transform(r,tag_prompts,&first) && pipeline_runner_register_transform(r,tag_prompts,&second));
    Outcome o=pipeline_run(r);assert(o.status==STAGE_SUCCESS);outcome_free(&o);
    /* Registration order, applied once across runs and resumes. */
    assert(str_eq(dot_find_node(g,"a")->prompt,"p+1+11"));
    char path[1100];snprintf(path,sizeof(path),"%s/checkpoint.json",test_root);o=pipeline_resume(r,path);outcome_free(&o);
    o=pipeline_run(r);assert(o.status==STAGE_SUCCESS && first==1 && second==11);outcome_free(&o);pipeline_runner_free(r);
    r=pipeline_runner_new(g,test_root);pipeline_register_builtin_handlers(r);assert(pipeline_runner_register_transform(r,break_graph,NULL));
    o=pipeline_run(r);assert(o.status==STAGE_FAIL);outcome_free(&o);pipeline_runner_free(r);dot_graph_free(g);
}
static void error_classification(void) {
    const struct {int status;const char *message;LlmErrorCode code;} cases_[]={
        {400,"Model gpt-x does not exist",LLM_ERR_NOT_FOUND},{400,"Resource Not Found",LLM_ERR_NOT_FOUND},{400,"Invalid key supplied",LLM_ERR_AUTH},
        {422,"Unauthorized",LLM_ERR_AUTH},{400,"maximum Context Length exceeded",LLM_ERR_CONTEXT_LENGTH},{400,"too many tokens",LLM_ERR_CONTEXT_LENGTH},
        {400,"blocked by content filter",LLM_ERR_CONTENT_FILTER},{400,"SAFETY",LLM_ERR_CONTENT_FILTER},{400,"bad field",LLM_ERR_INVALID_REQUEST},
        {418,"too many tokens",LLM_ERR_CONTEXT_LENGTH},{400,NULL,LLM_ERR_INVALID_REQUEST},{429,"safety",LLM_ERR_RATE_LIMIT},{500,"not found",LLM_ERR_SERVER},{401,"bad field",LLM_ERR_AUTH},
    };
    for(size_t i=0;i<sizeof(cases_)/sizeof(*cases_);i++) assert(llm_error_classify(cases_[i].status,cases_[i].message)==cases_[i].code);
}
static void tool_result_grouping(void) {
    const char *payloads[]={
        "{\"id\":\"fake\",\"model\":\"fake\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"usage\":{\"input_tokens\":3,\"output_tokens\":2}}",
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"ok\"}]},\"finishReason\":\"STOP\"}],\"usageMetadata\":{\"promptTokenCount\":3,\"candidatesTokenCount\":2}}"
    };
    const char *names[]={"anthropic","gemini"},*list_keys[]={"messages","contents"},*part_keys[]={"content","parts"};
    for(size_t i=0;i<2;i++) {
        LlmResponse *calls=tool_call_response("a","{}");Message *assistant=message_clone(calls->message);llm_response_free(calls);
        ContentPart **parts=realloc(assistant->parts,2*sizeof(*parts));assert(parts);assistant->parts=parts;
        LlmResponse *second=tool_call_response("b","{}");parts[1]=second->message->parts[0];second->message->part_count=0;llm_response_free(second);
        free(parts[1]->tool_call->id);parts[1]->tool_call->id=str_dup("call2");assistant->part_count=2;
        Message *messages[]={message_user("go"),assistant,message_tool_result("call","A",false),message_tool_result("call2","B",false)};
        HttpFixture fixture=http_fixture(payloads[i],strlen(payloads[i]),200,true);LlmClient *client=llm_client_new();
        llm_client_add_provider(client,i==0?anthropic_adapter_new("fake-credential",fixture.url):gemini_adapter_new("fake-credential",fixture.url));
        LlmRequest req={.model="fake\"model",.provider=(char *)names[i],.messages=messages,.message_count=4,.temperature=-1,.top_p=-1};LlmError error={0};
        LlmResponse *response=llm_client_complete(client,&req,&error);assert(response);llm_response_free(response);http_fixture_wait(fixture);
        char path[2048];snprintf(path,sizeof(path),"%s/request.json",test_root);char *body=io_read_text(path,1<<20);JsonValue *j=json_parse(body,NULL);free(body);
        JsonValue *list=json_get(j,list_keys[i]);assert(list && list->array.count==3);
        assert(json_get(list->array.items[2],part_keys[i])->array.count==2);
        json_free(j);llm_client_free(client);for(size_t k=0;k<4;k++) message_free(messages[k]);
    }
}
static char *saved_request(const char *suffix) {char path[2048];snprintf(path,sizeof(path),"%s/request.%s",test_root,suffix);return io_read_text(path,1<<20);}
static void anthropic_caching(void) {
    const char *payload="{\"id\":\"fake\",\"model\":\"fake\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"usage\":{\"input_tokens\":3,\"output_tokens\":2}}";
    const char *options[]={NULL,"{\"anthropic\":{\"auto_cache\":false,\"beta_headers\":[\"interleaved-thinking-2025-05-14\"]},\"openai\":{\"x\":1}}"};
    for(size_t i=0;i<2;i++) {
        HttpFixture fixture=http_fixture(payload,strlen(payload),200,true);LlmClient *client=llm_client_new();llm_client_add_provider(client,anthropic_adapter_new("fake-credential",fixture.url));
        Message *messages[]={message_system("be brief"),message_user("hi"),message_assistant("hello"),message_user("again")};
        ToolDefinition t1={.name="a",.description="a",.parameters_json="{\"type\":\"object\"}"},t2={.name="b",.description="b",.parameters_json="{\"type\":\"object\"}"};ToolDefinition *tools[]={&t1,&t2};
        LlmRequest req={.model="fake\"model",.provider="anthropic",.messages=messages,.message_count=4,.temperature=-1,.top_p=-1,.tools=tools,.tool_count=2,.provider_options_json=(char *)options[i]};
        LlmError e={0};LlmResponse *r=llm_client_complete(client,&req,&e);if(!r) fprintf(stderr,"caching: %s\n",str_safe(e.message));assert(r);llm_response_free(r);http_fixture_wait(fixture);
        char *body=saved_request("json"),*headers=saved_request("headers");JsonValue *j=json_parse(body,NULL);assert(j);
        if(i==0) {
            JsonValue *system=json_get(j,"system");assert(system && system->type==JSON_ARRAY && json_get(system->array.items[0],"cache_control"));
            JsonValue *tl=json_get(j,"tools");assert(!json_get(tl->array.items[0],"cache_control") && json_get(tl->array.items[1],"cache_control"));
            JsonValue *ms=json_get(j,"messages");JsonValue *last=json_get(ms->array.items[ms->array.count-1],"content");
            assert(json_get(last->array.items[last->array.count-1],"cache_control") && !json_get(json_get(ms->array.items[0],"content")->array.items[0],"cache_control"));
            assert(strstr(headers,"anthropic-beta: prompt-caching-2024-07-31"));
        } else {
            assert(!strstr(body,"cache_control") && strstr(headers,"interleaved-thinking-2025-05-14") && !strstr(headers,"prompt-caching"));
        }
        json_free(j);free(body);free(headers);for(size_t k=0;k<4;k++) message_free(messages[k]);llm_error_free(&e);llm_client_free(client);
    }
}
static const char *object_reply;
static LlmResponse *object_provider(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {
    (void)self;(void)err;assert(req->response_format && str_eq(req->response_format->type,"json_schema") && strstr(req->response_format->json_schema,"age"));
    LlmResponse *r=calloc(1,sizeof(*r));r->message=message_assistant(object_reply);r->text=str_dup(object_reply);return r;
}
static void generate_object(void) {
    LlmClient *c=llm_client_new();ProviderAdapter *a=calloc(1,sizeof(*a));a->name=str_dup("fake");a->complete=object_provider;llm_client_add_provider(c,a);
    const char *schema="{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"age\":{\"type\":\"integer\"},\"tags\":{\"type\":\"array\",\"items\":{\"enum\":[\"a\",\"b\"]}}},\"required\":[\"name\",\"age\"]}";
    const char *good[]={"{\"name\":\"Alice\",\"age\":30}","```json\n{\"name\":\"Alice\",\"age\":30,\"tags\":[\"a\"]}\n```"};
    for(size_t i=0;i<2;i++) {
        object_reply=good[i];LlmError e={0};GenerateResult *r=llm_generate_object(c,"fake","extract",NULL,schema,"fake",0,&e);
        assert(r && r->output && str_eq(json_get_string(r->output,"name"),"Alice") && json_get_int(r->output,"age",0)==30 && r->text && r->step_count==1 && r->steps[0].response==r->response);generate_result_free(r);llm_error_free(&e);
    }
    const char *bad[]={"{\"name\":\"Alice\"}","not json","{\"name\":\"Alice\",\"age\":30.5}","{\"name\":\"Alice\",\"age\":1,\"tags\":[\"c\"]}","[1]"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        object_reply=bad[i];LlmError e={0};assert(!llm_generate_object(c,"fake","extract",NULL,schema,"fake",0,&e) && e.code==LLM_ERR_NO_OBJECT);llm_error_free(&e);
    }
    LlmError e={0};assert(!llm_generate_object(c,"fake","extract",NULL,"[not a schema","fake",0,&e) && e.code==LLM_ERR_CONFIG);llm_error_free(&e);
    llm_client_free(c);
}
static void response_format_translation(void) {
    const char *payloads[]={
        "{\"id\":\"fake\",\"model\":\"fake\",\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"structured_output\",\"input\":{\"name\":\"A\"}}],\"usage\":{\"input_tokens\":3,\"output_tokens\":2}}",
        "{\"id\":\"fake\",\"model\":\"fake\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"name\\\":\\\"A\\\"}\"}]}],\"usage\":{\"input_tokens\":3,\"output_tokens\":2}}",
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"{\\\"name\\\":\\\"A\\\"}\"}]},\"finishReason\":\"STOP\"}],\"usageMetadata\":{\"promptTokenCount\":3,\"candidatesTokenCount\":2}}"
    };
    const char *names[]={"anthropic","openai","gemini"};
    for(size_t i=0;i<3;i++) {
        HttpFixture fixture=http_fixture(payloads[i],strlen(payloads[i]),200,true);LlmClient *client=llm_client_new();
        llm_client_add_provider(client,i==0?anthropic_adapter_new("fake-credential",fixture.url):i==1?openai_adapter_new("fake-credential",fixture.url):gemini_adapter_new("fake-credential",fixture.url));
        Message *m=message_user("hi");ResponseFormat format={.type="json_schema",.json_schema="{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}}}",.strict=true};
        LlmRequest req={.model="fake\"model",.provider=(char *)names[i],.messages=&m,.message_count=1,.temperature=-1,.top_p=-1,.response_format=&format};LlmError e={0};
        LlmResponse *r=llm_client_complete(client,&req,&e);if(!r) fprintf(stderr,"format %s: %s\n",names[i],str_safe(e.message));
        assert(r && r->tool_call_count==0);JsonValue *out=json_parse(r->text,NULL);assert(str_eq(json_get_string(out,"name"),"A"));json_free(out);
        llm_response_free(r);http_fixture_wait(fixture);
        char *body=saved_request("json");JsonValue *j=json_parse(body,NULL);free(body);assert(j);
        if(i==0) {assert(str_eq(json_get_string(json_get(j,"tool_choice"),"name"),"structured_output"));}
        if(i==1) {JsonValue *f=json_get(json_get(j,"text"),"format");assert(str_eq(json_get_string(f,"type"),"json_schema") && json_get(f,"schema") && json_get_bool(f,"strict",false));}
        if(i==2) {JsonValue *g=json_get(j,"generationConfig");assert(str_eq(json_get_string(g,"responseMimeType"),"application/json") && json_get(g,"responseSchema"));}
        json_free(j);message_free(m);llm_error_free(&e);llm_client_free(client);
    }
}
typedef struct {StreamEventType types[64];size_t count;StrBuf text;char *tool_args;int stop_after;} StreamLog;
static bool record_stream(const StreamEvent *event,void *userdata) {
    StreamLog *log=userdata;if(log->count<64) log->types[log->count++]=event->type;
    if(event->type==STREAM_EVT_TEXT_DELTA) strbuf_append_cstr(&log->text,event->delta);
    if(event->type==STREAM_EVT_TOOL_CALL_END) {free(log->tool_args);log->tool_args=str_dup(event->tool_call->arguments_json);assert(str_eq(event->tool_call->name,"probe"));}
    if(event->type==STREAM_EVT_FINISH) assert(event->response && str_eq(event->response->text,"Hello world"));
    return !(log->stop_after && (int)log->count>=log->stop_after);
}
static size_t count_type(const StreamLog *log,StreamEventType type) {size_t n=0;for(size_t i=0;i<log->count;i++) n+=log->types[i]==type;return n;}
static void streaming(void) {
    const char *streams[]={
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"m\",\"model\":\"fake\",\"usage\":{\"input_tokens\":3,\"output_tokens\":0}}}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\n"
        "event: ping\ndata: {\"type\":\"ping\"}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\" world\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"probe\",\"input\":{}}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"x\\\":\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"1}\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":5}}\n\n"
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
        "event: response.created\ndata: {\"type\":\"response.created\"}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\" world\"}\n\n"
        "data: {\"type\":\"response.output_text.done\"}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"probe\"}}\n\n"
        "data: {\"type\":\"response.function_call_arguments.delta\",\"delta\":\"{\\\"x\\\":1}\"}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"probe\",\"arguments\":\"{\\\"x\\\":1}\"}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"r\",\"model\":\"fake\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"Hello world\"}]},{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"probe\",\"arguments\":\"{\\\"x\\\":1}\"}],\"usage\":{\"input_tokens\":3,\"output_tokens\":5}}}\n\n",
        "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"Hello\"}]}}]}\r\n\r\n"
        "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\" world\"}]}}]}\n\n"
        "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"functionCall\":{\"name\":\"probe\",\"args\":{\"x\":1}}}]},\"finishReason\":\"STOP\"}],\"usageMetadata\":{\"promptTokenCount\":3,\"candidatesTokenCount\":5}}\n\n"
    };
    const char *names[]={"anthropic","openai","gemini"};
    for(size_t i=0;i<3;i++) for(int stop=0;stop<2;stop++) {
        HttpFixture fixture=http_fixture(streams[i],strlen(streams[i]),200,true);LlmClient *client=llm_client_new();
        llm_client_add_provider(client,i==0?anthropic_adapter_new("fake-credential",fixture.url):i==1?openai_adapter_new("fake-credential",fixture.url):gemini_adapter_new("fake-credential",fixture.url));
        Message *m=message_user("hi");LlmRequest req={.model="fake\"model",.provider=(char *)names[i],.messages=&m,.message_count=1,.temperature=-1,.top_p=-1};
        LlmError e={0};StreamLog log={.stop_after=stop?3:0};strbuf_init(&log.text);
        LlmResponse *r=llm_client_stream(client,&req,record_stream,&log,&e);
        if(stop) {assert(!r && e.code==LLM_ERR_ABORT && log.count==3);}
        else {
            if(!r) fprintf(stderr,"stream %s: %s\n",names[i],str_safe(e.message));
            assert(r && str_eq(r->text,"Hello world") && r->tool_call_count==1 && str_eq(r->tool_calls[0]->name,"probe") && r->usage.output_tokens==5);
            JsonValue *args=json_parse(r->tool_calls[0]->arguments_json,NULL);assert(json_get_int(args,"x",0)==1);json_free(args);
            assert(log.types[0]==STREAM_EVT_STREAM_START && log.types[log.count-1]==STREAM_EVT_FINISH);
            assert(count_type(&log,STREAM_EVT_TEXT_START)==1 && count_type(&log,STREAM_EVT_TEXT_DELTA)==2 && count_type(&log,STREAM_EVT_TEXT_END)==1);
            assert(count_type(&log,STREAM_EVT_TOOL_CALL_START)==1 && count_type(&log,STREAM_EVT_TOOL_CALL_END)==1 && count_type(&log,STREAM_EVT_ERROR)==0);
            char *text=strbuf_detach(&log.text);assert(str_eq(text,"Hello world"));free(text);
            JsonValue *tool_args=json_parse(log.tool_args,NULL);assert(json_get_int(tool_args,"x",0)==1);json_free(tool_args);
            char *body=saved_request("json");assert(i==2 || strstr(body,"\"stream\":true"));free(body);
            if(i==2) {char *headers=saved_request("headers");assert(strstr(headers,":streamGenerateContent?alt=sse"));free(headers);}
        }
        strbuf_free(&log.text);free(log.tool_args);llm_response_free(r);http_fixture_wait(fixture);message_free(m);llm_error_free(&e);llm_client_free(client);
    }
    HttpFixture fixture=http_fixture("{}",2,429,true);LlmClient *client=llm_client_new();llm_client_add_provider(client,openai_adapter_new("fake-credential",fixture.url));
    Message *m=message_user("hi");LlmRequest req={.model="fake\"model",.messages=&m,.message_count=1,.temperature=-1,.top_p=-1};LlmError e={0};StreamLog log={0};strbuf_init(&log.text);
    assert(!llm_client_stream(client,&req,record_stream,&log,&e) && e.code==LLM_ERR_RATE_LIMIT && e.retryable && count_type(&log,STREAM_EVT_ERROR)==1);
    strbuf_free(&log.text);http_fixture_wait(fixture);message_free(m);llm_error_free(&e);llm_client_free(client);
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
        DotGraph *g=dot_parse("digraph G { start; exit; a [type=custom,max_retries=0]; start -> a -> exit; }",NULL);assert(g && validate_or_raise(g,NULL));
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
static int flaky_calls;
static LlmResponse *flaky_provider(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {
    (void)self;(void)req;
    if(flaky_calls++==0) {err->code=LLM_ERR_RATE_LIMIT;err->retryable=true;err->retry_after=-1;err->message=str_dup("rate limited");return NULL;}
    return fake_complete(self,req,err);
}
static double retry_delays[8];static int retry_count;
static void record_retry(const LlmError *error,int attempt,double delay,void *userdata) {(void)error;(void)userdata;assert(attempt==retry_count);retry_delays[retry_count++]=delay;}
static double flaky_hint;static int flaky_failures;
static LlmResponse *hinted_provider(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {
    if(flaky_calls++<flaky_failures) {err->code=LLM_ERR_RATE_LIMIT;err->retryable=true;err->retry_after=flaky_hint;err->message=str_dup("rate limited");return NULL;}
    return fake_complete(self,req,err);
}
static void retry_policy(void) {
    LlmClient *c=llm_client_new();ProviderAdapter *a=calloc(1,sizeof(*a));a->name=str_dup("fake");a->complete=hinted_provider;llm_client_add_provider(c,a);
    assert(c->retry_policy.base_delay==1.0 && c->retry_policy.max_delay==60.0 && c->retry_policy.backoff_multiplier==2.0 && c->retry_policy.jitter);
    c->retry_policy=(RetryPolicy){.base_delay=0.01,.max_delay=0.03,.backoff_multiplier=2.0,.jitter=true,.on_retry=record_retry};
    Message *m=message_user("hi");LlmRequest req={.model="fake",.provider="fake",.messages=&m,.message_count=1,.temperature=-1,.top_p=-1};LlmError e={0};
    flaky_calls=retry_count=0;flaky_failures=3;flaky_hint=-1;LlmResponse *r=llm_client_complete_retry(c,&req,3,&e);
    assert(r && flaky_calls==4 && retry_count==3);llm_response_free(r);
    const double bases[]={0.01,0.02,0.03};
    for(int i=0;i<3;i++) assert(retry_delays[i]>=bases[i]*0.5-1e-9 && retry_delays[i]<=bases[i]*1.5+1e-9);
    flaky_calls=retry_count=0;flaky_failures=1;flaky_hint=0.02;r=llm_client_complete_retry(c,&req,3,&e);
    assert(r && retry_count==1 && retry_delays[0]==0.02);llm_response_free(r);
    flaky_calls=retry_count=0;flaky_hint=10;r=llm_client_complete_retry(c,&req,3,&e);
    assert(!r && flaky_calls==1 && retry_count==0 && e.retry_after==10);
    llm_error_free(&e);message_free(m);llm_client_free(c);
}
static void session_retry(void) {
    ExecutionEnv *env=local_exec_env_new(test_root);LlmClient *client=llm_client_new();ProviderAdapter *a=calloc(1,sizeof(*a));
    a->name=str_dup("anthropic");a->complete=flaky_provider;llm_client_add_provider(client,a);
    ProviderProfile *profile=anthropic_profile_new("fake");AgentSession *s=agent_session_new(profile,env,client,agent_default_config());
    flaky_calls=0;agent_session_submit(s,"hello");
    assert(!s->failed && flaky_calls==2 && s->history[s->history_count-1]->role==ROLE_ASSISTANT);
    agent_session_free(s);provider_profile_free(profile);llm_client_free(client);exec_env_free(env);
}
static void truncation_limits(void) {
    char *big=malloc(40001);memset(big,'a',40000);big[0]='H';big[39999]='T';big[40000]=0;
    char *s=agent_truncate_output(big,"shell",NULL,0);
    assert(s && s[0]=='H' && s[strlen(s)-1]=='T' && strstr(s,"[WARNING: Tool output was truncated. 10000 characters were removed from the middle."));free(s);
    s=agent_truncate_output(big,"write_file",NULL,0);
    assert(s && str_starts_with(s,"[WARNING: Tool output was truncated. First 39000 characters were removed.") && s[strlen(s)-1]=='T');free(s);
    ToolOutputLimit tight={.tool="read_file",.max_chars=10,.max_lines=0};
    s=agent_truncate_output(big,"read_file",&tight,1);assert(s && strstr(s,"39990 characters"));free(s);free(big);
    StrBuf lines;strbuf_init(&lines);for(int i=0;i<1000;i++) strbuf_appendf(&lines,"%d\n",i);char *text=strbuf_detach(&lines);text[strlen(text)-1]=0;
    s=agent_truncate_output(text,"shell",NULL,0);
    assert(s && str_starts_with(s,"0\n1\n") && strstr(s,"\n127\n[... 744 lines omitted ...]\n872\n") && str_ends_with(s,"\n999"));free(s);
    s=agent_truncate_output(text,"read_file",NULL,0);assert(str_eq(s,text));free(s);free(text);
    s=agent_truncate_output("short","shell",NULL,0);assert(str_eq(s,"short"));free(s);
}
static size_t full_event_length;
static void record_tool_end(const AgentEvent *event,void *userdata) {(void)userdata;if(event->kind==AGENT_EVT_TOOL_CALL_END) full_event_length=strlen(event->data);}
static int big_read_calls;
static LlmResponse *big_read_provider(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {
    if(big_read_calls++==0) return tool_call_response("read_file","{\"file_path\":\"big.txt\"}");
    Message *last=req->messages[req->message_count-1];assert(last->role==ROLE_TOOL);
    JsonValue *v=json_parse(last->parts[0]->tool_result->content,NULL);assert(v);
    const char *output=json_get_string(v,"output");assert(output && strlen(output)<51000 && strstr(output,"[WARNING: Tool output was truncated."));json_free(v);
    return fake_complete(self,req,err);
}
static void session_truncation(void) {
    ExecutionEnv *env=local_exec_env_new(test_root);char *big=malloc(200001);memset(big,'b',200000);big[200000]=0;assert(env->write_file(env,"big.txt",big));free(big);
    LlmClient *client=llm_client_new();ProviderAdapter *a=calloc(1,sizeof(*a));a->name=str_dup("anthropic");a->complete=big_read_provider;llm_client_add_provider(client,a);
    ProviderProfile *profile=anthropic_profile_new("fake");AgentSession *s=agent_session_new(profile,env,client,agent_default_config());
    agent_session_on_event(s,record_tool_end,NULL);big_read_calls=0;full_event_length=0;agent_session_submit(s,"read it");
    assert(!s->failed && big_read_calls==2 && full_event_length>200000);
    agent_session_free(s);provider_profile_free(profile);llm_client_free(client);exec_env_free(env);
}
static int writer_calls;
static LlmResponse *writer_provider(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {
    if(writer_calls++==0) {
        char *text=message_text(req->messages[req->message_count-1]);char expected[1200];snprintf(expected,sizeof(expected),"%s/status.json",test_root);
        assert(text && strstr(text,expected) && strstr(text,"preferred_label"));free(text);
        return tool_call_response("write_file","{\"file_path\":\"out.txt\",\"content\":\"made\"}");
    }
    return fake_complete(self,req,err);
}
static LlmResponse *denied_provider(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {
    (void)self;(void)req;err->code=LLM_ERR_AUTH;err->message=str_dup("bad key");return NULL;
}
static void agent_backend(void) {
    ExecutionEnv *env=local_exec_env_new(test_root);LlmClient *client=llm_client_new();ProviderAdapter *a=calloc(1,sizeof(*a));
    a->name=str_dup("anthropic");a->complete=writer_provider;llm_client_add_provider(client,a);
    CodergenBackend *b=agent_codergen_backend_new(client,env,"fake","anthropic");assert(b);
    DotGraph *g=dot_parse("digraph G { work [shape=box] }",NULL);const DotNode *node=dot_find_node(g,"work");
    PipelineContext c;ctx_init(&c);Outcome o={0};writer_calls=0;
    char *text=b->run(b,node,"write the file",&c,test_root,&o);assert(str_eq(text,"ok") && o.status==STAGE_SUCCESS && writer_calls==2);free(text);
    char *written=env->read_raw(env,"out.txt");assert(str_eq(written,"made"));free(written);
    a->complete=denied_provider;text=b->run(b,node,"again",&c,test_root,&o);
    assert(!text && o.failure_reason && strstr(o.failure_reason,"bad key"));outcome_free(&o);
    ctx_free(&c);dot_graph_free(g);agent_codergen_backend_free(b);llm_client_free(client);exec_env_free(env);
}
static void sigterm_grace(void) {
    ExecutionEnv *env=local_exec_env_new(test_root);
    ExecResult *r=env->exec_command(env,"sleep 30",100,NULL);assert(r && r->timed_out && r->duration_ms<1000);exec_result_free(r);
    r=env->exec_command(env,"trap '' TERM; sleep 30",100,NULL);
    assert(r && r->timed_out && r->duration_ms>=2000 && r->duration_ms<3500);exec_result_free(r);exec_env_free(env);
}
static void abort_on_tool_start(const AgentEvent *event,void *userdata) {if(event->kind==AGENT_EVT_TOOL_CALL_START) agent_session_abort(userdata);}
static LlmResponse *sleeper_provider(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {
    (void)self;(void)req;(void)err;return tool_call_response("shell","{\"command\":\"sleep 30\",\"timeout_ms\":60000}");
}
static void session_abort(void) {
    ExecutionEnv *env=local_exec_env_new(test_root);LlmClient *client=llm_client_new();ProviderAdapter *a=calloc(1,sizeof(*a));
    a->name=str_dup("anthropic");a->complete=sleeper_provider;llm_client_add_provider(client,a);
    ProviderProfile *profile=anthropic_profile_new("fake");AgentSession *s=agent_session_new(profile,env,client,agent_default_config());
    agent_session_on_event(s,abort_on_tool_start,s);time_t started=time(NULL);agent_session_submit(s,"sleep");
    assert(time(NULL)-started<3 && s->state==SESSION_CLOSED && !env->cancel);
    agent_session_free(s);provider_profile_free(profile);llm_client_free(client);exec_env_free(env);
}
static void expect_file(ExecutionEnv *env,const char *path,const char *content) {
    char *s=env->read_raw(env,path);if(!str_eq(s,content)) fprintf(stderr,"%s: [%s]\n",path,str_safe(s));assert(str_eq(s,content));free(s);
}
static void apply_patch_v4a(void) {
    ExecutionEnv *env=local_exec_env_new(test_root);bool error=false;
    assert(env->write_file(env,"a.txt","one\ntwo\nthree\nfour\n") && env->write_file(env,"b.txt","x\n"));
    char *s=agent_apply_patch(env,
        "*** Begin Patch\n*** Add File: dir/new.txt\n+hello\n+world\n*** Delete File: b.txt\n"
        "*** Update File: a.txt\n*** Move to: moved.txt\n@@ one\n two\n-three\n+THREE\n four\n*** End Patch\n",&error);
    assert(s && !error && strstr(s,"A dir/new.txt") && strstr(s,"D b.txt") && strstr(s,"M moved.txt"));free(s);
    expect_file(env,"moved.txt","one\ntwo\nTHREE\nfour\n");expect_file(env,"dir/new.txt","hello\nworld\n");
    assert(!env->file_exists(env,"a.txt") && !env->file_exists(env,"b.txt"));
    /* Multiple hunks; trailing whitespace and smart quotes match fuzzily. */
    assert(env->write_file(env,"c.py","def f():\n    x = 1  \n    return x\n\ndef g():\n    y = \xe2\x80\x9cq\xe2\x80\x9d\n    return y\n"));
    s=agent_apply_patch(env,"*** Begin Patch\n*** Update File: c.py\n@@ def f():\n-    x = 1\n+    x = 2\n@@ def g():\n-    y = \"q\"\n+    y = 'q'\n     return y\n*** End Patch\n",&error);
    assert(s && !error);free(s);
    expect_file(env,"c.py","def f():\n    x = 2\n    return x\n\ndef g():\n    y = 'q'\n    return y\n");
    /* End-of-file anchoring picks the last occurrence; pure additions append. */
    assert(env->write_file(env,"d.txt","x\nend\nx\nend\n"));
    s=agent_apply_patch(env,"*** Begin Patch\n*** Update File: d.txt\n@@\n x\n-end\n+END\n*** End of File\n@@\n+tail\n*** End Patch\n",&error);
    assert(s && !error);free(s);expect_file(env,"d.txt","x\nend\nx\nEND\ntail\n");
    /* A failing hunk leaves every file untouched. */
    s=agent_apply_patch(env,"*** Begin Patch\n*** Add File: z.txt\n+z\n*** Update File: moved.txt\n@@\n-missing\n+x\n*** End Patch\n",&error);
    assert(s && error && strstr(s,"moved.txt"));free(s);assert(!env->file_exists(env,"z.txt"));
    const char *bad[]={"*** Update File: moved.txt\n","*** Begin Patch\n*** Delete File: none.txt\n*** End Patch\n",
        "*** Begin Patch\n*** Add File: moved.txt\n+x\n*** End Patch\n","*** Begin Patch\n*** Frobnicate: x\n*** End Patch\n",
        "*** Begin Patch\n*** Update File: ../escape.txt\n@@\n+x\n*** End Patch\n"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) {s=agent_apply_patch(env,bad[i],&error);assert(s && error);free(s);}
    expect_file(env,"moved.txt","one\ntwo\nTHREE\nfour\n");exec_env_free(env);
}
static char *mock_wd(ExecutionEnv *self) {(void)self;return str_dup("/mock");}
static char *mock_platform(ExecutionEnv *self) {(void)self;return str_dup("darwin");}
static bool has_tool(ProviderProfile *p,const char *name) {return tool_registry_get(&p->tool_registry,name)!=NULL;}
static int shell_calls;
static LlmResponse *bare_shell_provider(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {
    if(shell_calls++==0) return tool_call_response("shell","{\"command\":\"true\"}");
    return fake_complete(self,req,err);
}
static void provider_profiles(void) {
    ProviderProfile *o=openai_profile_new("m"),*a=anthropic_profile_new("m"),*g=gemini_profile_new("m");assert(o && a && g);
    assert(has_tool(o,"apply_patch") && !has_tool(o,"edit_file") && has_tool(o,"write_file") && has_tool(o,"shell"));
    assert(has_tool(a,"edit_file") && !has_tool(a,"apply_patch"));
    assert(has_tool(g,"edit_file") && has_tool(g,"list_dir") && has_tool(g,"read_many_files") && !has_tool(g,"apply_patch"));
    char *po=o->build_system_prompt("ENV",NULL),*pa=a->build_system_prompt("ENV",NULL),*pg=g->build_system_prompt("ENV",NULL);
    assert(strstr(po,"apply_patch") && strstr(pa,"old_string") && strstr(pg,"GEMINI.md") && strstr(po,"ENV") && strstr(pa,"ENV") && strstr(pg,"ENV"));
    free(po);free(pa);free(pg);
    assert(a->default_command_timeout_ms==120000 && o->default_command_timeout_ms==0);
    /* The Anthropic profile's 120s shell default applies when timeout_ms is omitted. */
    ExecutionEnv env={.exec_command=mock_failed_exec,.working_directory=mock_wd,.platform=mock_platform};
    LlmClient *client=llm_client_new();ProviderAdapter *ad=calloc(1,sizeof(*ad));ad->name=str_dup("anthropic");ad->complete=bare_shell_provider;llm_client_add_provider(client,ad);
    AgentSession *s=agent_session_new(a,&env,client,agent_default_config());shell_calls=0;observed_timeout=0;agent_session_submit(s,"go");
    assert(observed_timeout==120000);agent_session_free(s);llm_client_free(client);
    /* Gemini directory and multi-file tools. */
    ExecutionEnv *local=local_exec_env_new(test_root);
    assert(local->write_file(local,"tree/a.txt","A") && local->write_file(local,"tree/sub/b.txt","B") && local->write_file(local,"tree/sub/deep/c.txt","C"));
    bool error=false;char *out=tool_registry_get(&g->tool_registry,"list_dir")->execute("{\"path\":\"tree\",\"depth\":2}",local,&error);
    assert(out && !error && strstr(out,"a.txt") && strstr(out,"sub/") && strstr(out,"b.txt") && strstr(out,"deep/") && !strstr(out,"c.txt"));free(out);
    out=tool_registry_get(&g->tool_registry,"read_many_files")->execute("{\"paths\":[\"tree/a.txt\",\"tree/sub/b.txt\"]}",local,&error);
    assert(out && !error && strstr(out,"tree/a.txt") && strstr(out,"A") && strstr(out,"tree/sub/b.txt"));free(out);
    exec_env_free(local);provider_profile_free(o);provider_profile_free(a);provider_profile_free(g);
}
static int parent_step,child_calls;static char spawned_id[128];
static bool offers(const LlmRequest *req,const char *name) {for(size_t i=0;i<req->tool_count;i++) if(str_eq(req->tools[i]->name,name)) return true;return false;}
static char *last_tool_output(const LlmRequest *req) {
    Message *m=req->messages[req->message_count-1];assert(m->role==ROLE_TOOL);
    JsonValue *v=json_parse(m->parts[0]->tool_result->content,NULL);char *out=str_dup(json_get_string(v,"output"));json_free(v);assert(out);return out;
}
static LlmResponse *subagent_provider(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {
    if(!offers(req,"spawn_agent")) {
        assert(!offers(req,"wait"));child_calls++;char *input=message_text(req->messages[req->message_count-1]);
        StrBuf b;strbuf_init(&b);strbuf_appendf(&b,"child:%s",input);free(input);char *text=strbuf_detach(&b);
        LlmResponse *r=calloc(1,sizeof(*r));r->message=message_assistant(text);r->text=text;return r;
    }
    char args[512];char *out=parent_step>=1 && parent_step<=4?last_tool_output(req):NULL;LlmResponse *r=NULL;
    switch(parent_step++) {
        case 0: r=tool_call_response("spawn_agent","{\"task\":\"inspect\"}");break;
        case 1: {JsonValue *v=json_parse(out,NULL);snprintf(spawned_id,sizeof(spawned_id),"%s",str_safe(json_get_string(v,"agent_id")));assert(str_eq(json_get_string(v,"status"),"running"));json_free(v);
                 assert(*spawned_id && child_calls==0);snprintf(args,sizeof(args),"{\"agent_id\":\"%s\",\"message\":\"more\"}",spawned_id);r=tool_call_response("send_input",args);break;}
        case 2: snprintf(args,sizeof(args),"{\"agent_id\":\"%s\"}",spawned_id);r=tool_call_response("wait",args);break;
        case 3: {JsonValue *v=json_parse(out,NULL);assert(v && str_eq(json_get_string(v,"output"),"child:more") && json_get_bool(v,"success",false) && json_get_int(v,"turns_used",0)==2);json_free(v);
                 assert(child_calls==2);snprintf(args,sizeof(args),"{\"agent_id\":\"%s\"}",spawned_id);r=tool_call_response("close_agent",args);break;}
        case 4: assert(strstr(out,"closed"));snprintf(args,sizeof(args),"{\"agent_id\":\"%s\"}",spawned_id);r=tool_call_response("wait",args);break;
        default: {Message *m=req->messages[req->message_count-1];assert(m->parts[0]->tool_result->is_error);r=fake_complete(self,req,err);}
    }
    free(out);return r;
}
static void subagents(void) {
    ExecutionEnv *env=local_exec_env_new(test_root);LlmClient *client=llm_client_new();ProviderAdapter *a=calloc(1,sizeof(*a));
    a->name=str_dup("anthropic");a->complete=subagent_provider;llm_client_add_provider(client,a);
    ProviderProfile *profile=anthropic_profile_new("fake");AgentSession *s=agent_session_new(profile,env,client,agent_default_config());
    parent_step=child_calls=0;agent_session_submit(s,"delegate");
    assert(!s->failed && parent_step==6);
    agent_session_free(s);provider_profile_free(profile);llm_client_free(client);exec_env_free(env);
}
static int unlimited_calls;
static LlmResponse *many_rounds_provider(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {
    if(unlimited_calls++<60) return tool_call_response("glob","{\"pattern\":\"*.none\"}");
    return fake_complete(self,req,err);
}
static void unlimited_limits(void) {
    ExecutionEnv *env=local_exec_env_new(test_root);LlmClient *client=llm_client_new();ProviderAdapter *a=calloc(1,sizeof(*a));
    a->name=str_dup("anthropic");a->complete=many_rounds_provider;llm_client_add_provider(client,a);
    ProviderProfile *profile=anthropic_profile_new("fake");SessionConfig config=agent_default_config();config.max_turns=0;config.max_tool_rounds_per_input=0;
    AgentSession *s=agent_session_new(profile,env,client,config);
    unlimited_calls=0;agent_session_submit(s,"go");assert(!s->failed && unlimited_calls==61);
    for(int i=0;i<120;i++) agent_session_submit(s,"again");assert(!s->failed && s->turn_count==121);
    agent_session_free(s);provider_profile_free(profile);llm_client_free(client);exec_env_free(env);
}
static int loop_calls,loop_events;static bool loop_alternate;
static LlmResponse *loop_provider(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {
    if(loop_calls>=12) return fake_complete(self,req,err);
    char args[64];int i=loop_calls++;
    if(loop_alternate) return i%2?tool_call_response("grep","{\"pattern\":\"b\"}"):tool_call_response("glob","{\"pattern\":\"a\"}");
    snprintf(args,sizeof(args),"{\"pattern\":\"*.%d\"}",i);return tool_call_response("glob",args);
}
static void count_loop_events(const AgentEvent *event,void *userdata) {(void)userdata;if(event->kind==AGENT_EVT_LOOP_DETECTION) loop_events++;}
static void loop_signatures(void) {
    ExecutionEnv *env=local_exec_env_new(test_root);LlmClient *client=llm_client_new();ProviderAdapter *a=calloc(1,sizeof(*a));
    a->name=str_dup("anthropic");a->complete=loop_provider;llm_client_add_provider(client,a);ProviderProfile *profile=anthropic_profile_new("fake");
    for(int alternate=0;alternate<2;alternate++) {
        AgentSession *s=agent_session_new(profile,env,client,agent_default_config());agent_session_on_event(s,count_loop_events,NULL);
        loop_calls=loop_events=0;loop_alternate=alternate;agent_session_submit(s,"go");
        assert(!s->failed && (alternate?loop_events>0:loop_events==0));agent_session_free(s);
    }
    provider_profile_free(profile);llm_client_free(client);exec_env_free(env);
}
static AgentEventKind seen_events[32];static size_t seen_count;
static void record_kinds(const AgentEvent *event,void *userdata) {(void)userdata;if(seen_count<32) seen_events[seen_count++]=event->kind;}
static size_t count_kind(AgentEventKind kind) {size_t n=0;for(size_t i=0;i<seen_count;i++) n+=seen_events[i]==kind;return n;}
static void session_lifecycle(void) {
    ExecutionEnv *env=local_exec_env_new(test_root);LlmClient *client=llm_client_new();ProviderAdapter *a=calloc(1,sizeof(*a));
    a->name=str_dup("anthropic");a->complete=fake_complete;llm_client_add_provider(client,a);ProviderProfile *profile=anthropic_profile_new("fake");
    AgentSession *s=agent_session_new(profile,env,client,agent_default_config());agent_session_on_event(s,record_kinds,NULL);seen_count=0;
    agent_session_submit(s,"one");agent_session_submit(s,"two");
    assert(seen_events[0]==AGENT_EVT_SESSION_START && seen_events[1]==AGENT_EVT_USER_INPUT && count_kind(AGENT_EVT_SESSION_START)==1);
    assert(count_kind(AGENT_EVT_PROCESSING_END)==2 && seen_events[seen_count-1]==AGENT_EVT_PROCESSING_END && !count_kind(AGENT_EVT_SESSION_END) && s->state==SESSION_IDLE);
    agent_session_free(s);assert(count_kind(AGENT_EVT_SESSION_END)==1 && seen_events[seen_count-1]==AGENT_EVT_SESSION_END);
    /* An unrecoverable LLM error closes the session exactly once. */
    a->complete=denied_provider;s=agent_session_new(profile,env,client,agent_default_config());agent_session_on_event(s,record_kinds,NULL);seen_count=0;
    agent_session_submit(s,"one");assert(s->state==SESSION_CLOSED && count_kind(AGENT_EVT_ERROR)==1 && count_kind(AGENT_EVT_SESSION_END)==1 && seen_events[seen_count-1]==AGENT_EVT_SESSION_END);
    agent_session_free(s);assert(count_kind(AGENT_EVT_SESSION_END)==1);
    provider_profile_free(profile);llm_client_free(client);exec_env_free(env);
}
static char warning_text[128];
static void record_warning(const AgentEvent *event,void *userdata) {(void)userdata;if(event->kind==AGENT_EVT_WARNING) snprintf(warning_text,sizeof(warning_text),"%s",event->data);}
static void context_warning(void) {
    ExecutionEnv *env=local_exec_env_new(test_root);LlmClient *client=llm_client_new();ProviderAdapter *a=calloc(1,sizeof(*a));
    a->name=str_dup("anthropic");a->complete=fake_complete;llm_client_add_provider(client,a);ProviderProfile *profile=anthropic_profile_new("fake");
    profile->context_window_size=100;AgentSession *s=agent_session_new(profile,env,client,agent_default_config());agent_session_on_event(s,record_warning,NULL);
    /* ~4 characters per token: 5+2 characters stay under 80%; +340+2 reach 87%. */
    char input[341];memset(input,'x',340);input[340]=0;*warning_text=0;agent_session_submit(s,"short");assert(!*warning_text);
    agent_session_submit(s,input);assert(str_eq(warning_text,"Context usage at ~87% of context window"));
    agent_session_free(s);provider_profile_free(profile);llm_client_free(client);exec_env_free(env);
}
static char *captured_system;
static LlmResponse *capture_system_provider(ProviderAdapter *self,const LlmRequest *req,LlmError *err) {
    free(captured_system);captured_system=message_text(req->messages[0]);return fake_complete(self,req,err);
}
static void project_docs(void) {
    char sub[1100];snprintf(sub,sizeof(sub),"%s/repo/sub",test_root);
    ExecutionEnv *root=local_exec_env_new(test_root);
    assert(root->write_file(root,"repo/.git/HEAD","ref") && root->write_file(root,"repo/AGENTS.md","ROOT-AGENTS") && root->write_file(root,"repo/CLAUDE.md","ROOT-CLAUDE"));
    assert(root->write_file(root,"repo/GEMINI.md","ROOT-GEMINI") && root->write_file(root,"repo/sub/AGENTS.md","SUB-AGENTS") && root->write_file(root,"AGENTS.md","ABOVE-GIT-ROOT"));
    ExecutionEnv *env=local_exec_env_new(sub);LlmClient *client=llm_client_new();ProviderAdapter *a=calloc(1,sizeof(*a));
    a->name=str_dup("anthropic");a->complete=capture_system_provider;llm_client_add_provider(client,a);ProviderProfile *profile=anthropic_profile_new("fake");
    AgentSession *s=agent_session_new(profile,env,client,agent_default_config());agent_session_submit(s,"go");
    char *agents=strstr(captured_system,"ROOT-AGENTS"),*claude=strstr(captured_system,"ROOT-CLAUDE"),*nested=strstr(captured_system,"SUB-AGENTS");
    assert(agents && claude && nested && agents<claude && claude<nested && !strstr(captured_system,"ROOT-GEMINI") && !strstr(captured_system,"ABOVE-GIT-ROOT"));
    agent_session_free(s);
    /* Oversized instructions stop at the 32KB budget with a marker. */
    char *big=malloc(40001);memset(big,'d',40000);big[40000]=0;assert(root->write_file(root,"repo/CLAUDE.md",big));free(big);
    s=agent_session_new(profile,env,client,agent_default_config());agent_session_submit(s,"go");
    assert(strstr(captured_system,"[Project instructions truncated at 32KB]") && !strstr(captured_system,"SUB-AGENTS") && strlen(captured_system)<34000);
    agent_session_free(s);free(captured_system);captured_system=NULL;
    provider_profile_free(profile);llm_client_free(client);exec_env_free(env);exec_env_free(root);
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

static void json_scaling(void) {
    /* Quadratic duplicate-key scans or one-slot growth exceed the case deadline. */
    StrBuf b;strbuf_init(&b);strbuf_append_cstr(&b,"{");
    for(size_t i=0;i<200000;i++) strbuf_appendf(&b,"%s\"k%zu\":%zu",i?",":"",i,i);
    strbuf_append_cstr(&b,"}");char *object=strbuf_detach(&b);assert(object);
    JsonValue *v=json_parse(object,NULL);assert(v && v->object.count==200000 && json_get_int(v,"k199999",-1)==199999);json_free(v);
    object[strlen(object)-1]=0;StrBuf dup;strbuf_init(&dup);strbuf_appendf(&dup,"%s,\"k0\":1}",object);free(object);
    char *duplicate=strbuf_detach(&dup);const char *err=NULL;assert(!json_parse(duplicate,&err) && str_eq(err,"duplicate key"));free(duplicate);
    strbuf_init(&b);strbuf_append_cstr(&b,"[");for(size_t i=0;i<1000000;i++) strbuf_append_cstr(&b,i?",0.5":"0.5");strbuf_append_cstr(&b,"]");
    char *array=strbuf_detach(&b);v=json_parse(array,NULL);free(array);assert(v && v->array.count==1000000);
    char *encoded=json_serialize(v);assert(encoded);free(encoded);json_free(v);
    JsonValue *built=json_new_object();for(size_t i=0;i<50000;i++) {char key[32];snprintf(key,sizeof(key),"b%zu",i);assert(json_object_set(built,key,json_new_number(1)));}
    assert(json_object_set(built,"b7",json_new_number(2)) && built->object.count==50000 && json_get_int(built,"b7",0)==2);json_free(built);
}
static void dot_scaling(void) {
    StrBuf b;strbuf_init(&b);strbuf_append_cstr(&b,"digraph G { start [shape=Mdiamond]; exit [shape=Msquare]; start -> n0;");
    for(size_t i=0;i+1<9000;i++) for(size_t k=1;k<=10 && i+k<9000;k++) strbuf_appendf(&b," n%zu -> n%zu;",i,i+k);
    strbuf_append_cstr(&b," n8999 -> exit; }");char *source=strbuf_detach(&b);assert(source);
    char *err=NULL;DotGraph *g=dot_parse(source,&err);free(source);assert(g && !err && g->node_count==9002 && g->edge_count==89947);
    struct timespec began,ended;clock_gettime(CLOCK_MONOTONIC,&began);
    assert(validate_or_raise(g,NULL));assert(str_eq(dot_find_node(g,"n4500")->id,"n4500"));
    char *join_err=NULL;DotNode *fork=dot_find_node(g,"n0");assert(!dot_parallel_join(g,fork,&join_err));free(join_err);
    clock_gettime(CLOCK_MONOTONIC,&ended);assert(ended.tv_sec-began.tv_sec<2);dot_graph_free(g);
    /* Repeated node statements must not multiply scoped default attributes. */
    strbuf_init(&b);strbuf_append_cstr(&b,"digraph G { node [");
    for(size_t i=0;i<200;i++) strbuf_appendf(&b,"%sd%zu=v",i?",":"",i);
    strbuf_append_cstr(&b,"]; x [label=one];");for(size_t i=0;i<5000;i++) strbuf_append_cstr(&b," x;");
    strbuf_append_cstr(&b," x [label=two]; }");source=strbuf_detach(&b);assert(source);
    g=dot_parse(source,&err);free(source);assert(g && !err);
    DotNode *x=dot_find_node(g,"x");assert(x && x->attr_count==201 && str_eq(x->label,"two") && str_eq(dot_node_attr(x,"label",NULL),"two"));dot_graph_free(g);
    strbuf_init(&b);strbuf_append_cstr(&b,"digraph G { x [");for(size_t i=0;i<5000;i++) strbuf_appendf(&b,"%sa%zu=v",i?",":"",i);
    strbuf_append_cstr(&b,"]; }");source=strbuf_detach(&b);g=dot_parse(source,&err);free(source);assert(!g && err);free(err);
}
static bool utf8_ok(const char *s) {JsonValue *v=json_new_string(s);bool ok=v!=NULL;json_free(v);return ok;}
static void utf8_truncation(void) {
    const char *s="a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"; /* a, e-acute, euro, emoji */
    assert(str_utf8_prefix(s,0)==0 && str_utf8_prefix(s,1)==1 && str_utf8_prefix(s,2)==1 && str_utf8_prefix(s,3)==3);
    assert(str_utf8_prefix(s,5)==3 && str_utf8_prefix(s,6)==6 && str_utf8_prefix(s,9)==6 && str_utf8_prefix(s,10)==10 && str_utf8_prefix(s,99)==10);
    StrBuf b;strbuf_init(&b);for(size_t i=0;i<300;i++) strbuf_append_cstr(&b,"\xe2\x82\xac");strbuf_append_cstr(&b,"x");char *value=strbuf_detach(&b);
    PipelineContext c;ctx_init(&c);ctx_set(&c,"k",value);free(value);
    const char *modes[]={"truncate","compact"};
    for(size_t i=0;i<2;i++) {char *text=ctx_to_preamble(&c,modes[i]);assert(text && utf8_ok(text));free(text);}
    ctx_free(&c);
}
static void base_url_policy(void) {
    const char *rejected[]={"http://example.com","http://127.0.0.1.example.com","ftp://127.0.0.1","example.com","http://localhost@evil.test"};
    for(size_t i=0;i<sizeof(rejected)/sizeof(*rejected);i++) assert(!anthropic_adapter_new("k",rejected[i]) && !openai_adapter_new("k",rejected[i]) && !gemini_adapter_new("k",rejected[i]));
    const char *accepted[]={"https://api.example.com","http://127.0.0.1:8080","http://localhost","http://[::1]:9/"};
    for(size_t i=0;i<sizeof(accepted)/sizeof(*accepted);i++) {
        ProviderAdapter *a=openai_adapter_new("k",accepted[i]);assert(a);a->close(a);free(a->impl);free(a->name);free(a);
    }
}

typedef struct {const char *name; void (*run)(void); bool expected_failure;} Case;
static const Case cases[] = {
    {"codergen backend outcomes",codergen_outcomes,false}, {"status.json file contract",status_file_contract,false}, {"agent session retries transient errors",session_retry,false},
    {"tool output truncation limits",truncation_limits,false}, {"session truncates tool output for the LLM only",session_truncation,false}, {"agent session codergen backend",agent_backend,false},
    {"parallel tool results grouped per turn",tool_result_grouping,false}, {"retry budget defaults / keys",retry_defaults,false},
    {"stylesheet shape / multi-class selectors",stylesheet_selectors,false}, {"tool round budget / passive tools",tool_round_budget,false}, {"retry policy backoff / jitter / Retry-After",retry_policy,false},
    {"SIGTERM grace before SIGKILL",sigterm_grace,false}, {"session abort kills commands / closes",session_abort,false},
    {"apply_patch v4a",apply_patch_v4a,false}, {"provider-aligned profiles / tools",provider_profiles,false},
    {"Anthropic prompt caching / provider options",anthropic_caching,false}, {"subagents spawn / send / wait / close / depth",subagents,false}, {"zero turn / round limits are unlimited",unlimited_limits,false}, {"loop detection signatures / cycles",loop_signatures,false}, {"session lifecycle events",session_lifecycle,false}, {"error classification by message",error_classification,false}, {"Gemini unique tool call IDs",gemini_call_ids,false}, {"generate rejects prompt with messages",generate_prompt_xor_messages,false}, {"validator severities / handler fallback",validator_severities,false}, {"checkpoint current / completed nodes",checkpoint_progress,false}, {"loop_restart fresh runs / budget",loop_restart,false}, {"queue / callback interviewers",interviewers,false}, {"custom transforms",custom_transforms,false}, {"context window warning",context_warning,false}, {"project instruction discovery",project_docs,false},
    {"generate_object parse / validate",generate_object,false}, {"response_format provider translation",response_format_translation,false}, {"streaming events / final response",streaming,false},
    {"JSON large containers",json_scaling,false}, {"DOT large graphs / attribute bounds",dot_scaling,false},
    {"UTF-8 safe truncation",utf8_truncation,false}, {"provider base URL transport policy",base_url_policy,false},
    {"checkpoint validation / allocation / ambiguity",checkpoint_validation,false}, {"human timeout policy",human_timeout,false}, {"artifact permissions / names / I/O errors",filesystem_permissions,false},
    {"branch committed recovery / new run",branch_recovery,false}, {"nested forks / join policies / cancellation",nested_and_policies,false}, {"session / provider construction failures",construction_failures,false},
    {"local HTTP transport / provider contracts",local_transport,false}, {"bounded parser fuzz",bounded_fuzz,false}, {"human EOF / invalid / cancellation",human_input,false}, {"partial / skipped / retry / large routing",outcome_routes,false}, {"recursive search containment",search_containment,false},
    {"generation ownership / userdata / middleware",generation_contracts,false}, {"structured tool errors / duration policy",structured_tool_errors,false}, {"parser / clone allocation failures",parser_allocation_failures,false},
    {"DOT fixture outcomes / counts",fixture_assertions,false}, {"complete isolated branches",complete_branches,false}, {"branch writes stay isolated",branch_conflict,false}, {"child inherited backend / failures",child_configuration,false},
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
        if(leak_check && (cases[i].run==local_transport || cases[i].run==checkpoint_recovery || cases[i].run==branch_recovery ||
                           cases[i].run==tool_result_grouping || cases[i].run==anthropic_caching || cases[i].run==response_format_translation || cases[i].run==streaming || cases[i].run==gemini_call_ids)) {
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
