#include "attractor/engine.h"
#include "util/str.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
typedef struct {
    const char *s;size_t pos,depth;bool valid;
    const Outcome *outcome;const PipelineContext *context;
} ConditionParser;
static void space(ConditionParser *p) {while(isspace((unsigned char)p->s[p->pos])) p->pos++;}
static const char *status_name(StageStatus s) {
    const char *names[]={"success","partial_success","retry","fail","skipped"};
    return s>=STAGE_SUCCESS && s<=STAGE_SKIPPED?names[s]:"invalid";
}
static const char *resolve(ConditionParser *p,const char *key) {
    if(str_eq(key,"outcome")) return status_name(p->outcome->status);
    if(str_eq(key,"preferred_label")) return str_safe(p->outcome->preferred_label);
    return ctx_get(p->context,str_starts_with(key,"context.")?key+8:key,"");
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
    if(start==p->pos) {p->valid=false;return false;}
    char *key=str_ndup(p->s+start,p->pos-start);if(!key) {p->valid=false;return false;}
    space(p);bool neq=false;
    if(p->s[p->pos]=='!' && p->s[p->pos+1]=='=') {neq=true;p->pos+=2;}
    else if(p->s[p->pos]=='=') p->pos++;
    else {
        const char *v=resolve(p,key);bool result=*v && !str_eq(v,"false") && !str_eq(v,"0");free(key);return result;
    }
    space(p);start=p->pos;
    while(p->s[p->pos] && !strchr("&|()=<>!",p->s[p->pos])) p->pos++;
    char *rhs=str_ndup(p->s+start,p->pos-start);if(!rhs) {free(key);p->valid=false;return false;}
    char *trimmed=str_trim(rhs);if(!*trimmed) p->valid=false;
    size_t len=strlen(trimmed);
    if(strchr(trimmed,'"') && (len<2 || trimmed[0]!='"' || trimmed[len-1]!='"' || memchr(trimmed+1,'"',len-2))) p->valid=false;
    if(len>=2 && trimmed[0]=='"' && trimmed[len-1]=='"') {trimmed[len-1]=0;trimmed++;}
    bool result=str_eq(resolve(p,key),trimmed);free(rhs);free(key);return neq?!result:result;
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
