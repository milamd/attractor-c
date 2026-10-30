#include "attractor/engine.h"
#include "util/str.h"
#include <ctype.h>
#include <string.h>
typedef struct {
    const char *s;size_t pos,depth;bool valid;
    const Outcome *outcome;const PipelineContext *context;
} ConditionParser;
static void space(ConditionParser *p) {while(isspace((unsigned char)p->s[p->pos])) p->pos++;}
static bool span_eq(const char *s,size_t len,const char *lit) {
    return strlen(lit)==len && memcmp(s,lit,len)==0;
}
static const char *status_name(StageStatus s) {
    const char *names[]={"success","partial_success","retry","fail","skipped"};
    return s>=STAGE_SUCCESS && s<=STAGE_SKIPPED?names[s]:"invalid";
}
static const char *resolve(ConditionParser *p,const char *key,size_t len) {
    if(span_eq(key,len,"outcome")) return status_name(p->outcome->status);
    if(span_eq(key,len,"preferred_label")) return str_safe(p->outcome->preferred_label);
    if(len>=8 && memcmp(key,"context.",8)==0) {key+=8;len-=8;}
    if(!p->context) return "";
    for(size_t i=0;i<p->context->count;i++)
        if(span_eq(key,len,p->context->keys[i])) return p->context->values[i];
    return "";
}
static bool expression(ConditionParser *p);
static bool primary(ConditionParser *p) {
    space(p);
    if(p->s[p->pos]=='(') {
        if(p->depth++>=64) {p->valid=false;return false;}p->pos++;
        bool result=expression(p);space(p);
        if(p->s[p->pos]!=')') p->valid=false;else p->pos++;
        p->depth--;return result;
    }
    size_t start=p->pos;
    while(isalnum((unsigned char)p->s[p->pos]) || (p->s[p->pos] && strchr("_.-",p->s[p->pos]))) p->pos++;
    size_t key_len=p->pos-start;
    if(!key_len) {p->valid=false;return false;}
    const char *key=p->s+start;
    space(p);bool neq=false;
    if(p->s[p->pos]=='!' && p->s[p->pos+1]=='=') {neq=true;p->pos+=2;}
    else if(p->s[p->pos]=='=') p->pos++;
    else {
        const char *v=resolve(p,key,key_len);
        return *v && !str_eq(v,"false") && !str_eq(v,"0");
    }
    space(p);start=p->pos;
    while(p->s[p->pos] && !strchr("&|()=<>!",p->s[p->pos])) p->pos++;
    const char *rhs=p->s+start;size_t len=p->pos-start;
    while(len && isspace((unsigned char)rhs[len-1])) len--;
    if(!len) p->valid=false;
    if(memchr(rhs,'"',len) && (len<2 || rhs[0]!='"' || rhs[len-1]!='"' || memchr(rhs+1,'"',len-2))) p->valid=false;
    if(len>=2 && rhs[0]=='"' && rhs[len-1]=='"') {rhs++;len-=2;}
    bool result=span_eq(rhs,len,resolve(p,key,key_len));return neq?!result:result;
}
static bool conjunction(ConditionParser *p) {
    bool result=primary(p);
    while(p->valid) {
        space(p);if(p->s[p->pos]!='&' || p->s[p->pos+1]!='&') break;
        p->pos+=2;bool rhs=primary(p);result=result && rhs;
    }return result;
}
static bool expression(ConditionParser *p) {
    bool result=conjunction(p);
    while(p->valid) {
        space(p);if(p->s[p->pos]!='|' || p->s[p->pos+1]!='|') break;
        p->pos+=2;bool rhs=conjunction(p);result=result || rhs;
    }return result;
}
static bool parse_condition(const char *condition,const Outcome *outcome,const PipelineContext *ctx,bool *valid) {
    if(!condition || !*condition) {*valid=true;return true;}
    if(strlen(condition)>65536) {*valid=false;return false;}
    ConditionParser p={.s=condition,.valid=true,.outcome=outcome,.context=ctx};
    bool result=expression(&p);space(&p);*valid=p.valid && !p.s[p.pos];return *valid && result;
}
bool condition_validate(const char *condition) {
    Outcome o={.status=STAGE_SUCCESS};PipelineContext c={0};bool valid;
    parse_condition(condition,&o,&c,&valid);return valid;
}
bool evaluate_condition(const char *condition,const Outcome *outcome,const PipelineContext *ctx) {
    if(!outcome || !ctx) return false;bool valid;return parse_condition(condition,outcome,ctx,&valid);
}
