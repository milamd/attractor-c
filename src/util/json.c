#include "util/json.h"
#include "util/str.h"
#include "util/mem.h"
#include "util/io.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>
#include <errno.h>
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif
#ifndef ATTRACTOR_JSON_DEPTH
#define ATTRACTOR_JSON_DEPTH 64u
#endif

typedef struct {const char *s; size_t pos,depth; const char *err;} Parser;
static void ws(Parser *p) {while(p->s[p->pos] && strchr(" \r\n\t",p->s[p->pos])) p->pos++;}
static JsonValue *alloc_value(JsonType t) {
    JsonValue *v=mem_calloc(1,sizeof(*v)); if(v) v->type=t; return v;
}
static bool utf8_valid(const char *s) {
    const unsigned char *p=(const unsigned char *)s;
    while(*p) {
        unsigned cp=*p++; size_t n=0; unsigned min=0;
        if(cp<0x80) continue;
        if(cp>=0xc2 && cp<=0xdf) {cp &=31;n=1;min=0x80;}
        else if(cp>=0xe0 && cp<=0xef) {cp &=15;n=2;min=0x800;}
        else if(cp>=0xf0 && cp<=0xf4) {cp &=7;n=3;min=0x10000;}
        else return false;
        for(size_t i=0;i<n;i++) {if((*p & 0xc0)!=0x80) return false; cp=(cp<<6)|(*p++ & 63);}
        if(cp<min || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return false;
    }
    return true;
}
static bool hex4(Parser *p,unsigned *out) {
    *out=0;
    for(size_t i=0;i<4;i++) {
        unsigned char c=(unsigned char)p->s[p->pos]; unsigned digit;
        if(c>='0' && c<='9') digit=(unsigned)(c-'0');
        else if(c>='a' && c<='f') digit=(unsigned)(c-'a'+10);
        else if(c>='A' && c<='F') digit=(unsigned)(c-'A'+10);
        else return false;
        p->pos++; *out=(*out<<4)|digit;
    }
    return true;
}
static void append_codepoint(StrBuf *b,unsigned cp) {
    char s[4];size_t n;
    if(cp<0x80) {s[0]=(char)cp;n=1;}
    else if(cp<0x800) {s[0]=(char)(0xc0|(cp>>6));s[1]=(char)(0x80|(cp&63));n=2;}
    else if(cp<0x10000) {s[0]=(char)(0xe0|(cp>>12));s[1]=(char)(0x80|((cp>>6)&63));s[2]=(char)(0x80|(cp&63));n=3;}
    else {s[0]=(char)(0xf0|(cp>>18));s[1]=(char)(0x80|((cp>>12)&63));s[2]=(char)(0x80|((cp>>6)&63));s[3]=(char)(0x80|(cp&63));n=4;}
    strbuf_append(b,s,n);
}
static char *string_raw(Parser *p) {
    if(p->s[p->pos]!='"') {p->err="expected string";return NULL;}
    p->pos++; StrBuf b;strbuf_init(&b);
    while(p->s[p->pos] && p->s[p->pos]!='"') {
        unsigned char c=(unsigned char)p->s[p->pos++];
        if(c<0x20) {p->err="control character in string";goto fail;}
        if(c=='\\') {
            char e=p->s[p->pos]; if(!e) {p->err="incomplete escape";goto fail;} p->pos++;
            switch(e) {
                case '"': c='"';break; case '\\':c='\\';break;case '/':c='/';break;
                case 'b':c='\b';break;case 'f':c='\f';break;case 'n':c='\n';break;case 'r':c='\r';break;case 't':c='\t';break;
                case 'u': {
                    unsigned cp,lo;
                    if(!hex4(p,&cp)) {p->err="invalid Unicode escape";goto fail;}
                    if(cp>=0xd800 && cp<=0xdbff) {
                        if(p->s[p->pos]!='\\' || p->s[p->pos+1]!='u') {p->err="missing low surrogate";goto fail;}
                        p->pos+=2;
                        if(!hex4(p,&lo) || lo<0xdc00 || lo>0xdfff) {p->err="invalid low surrogate";goto fail;}
                        cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);
                    } else if(cp>=0xdc00 && cp<=0xdfff) {p->err="unpaired surrogate";goto fail;}
                    if(!cp) {p->err="embedded NUL unsupported at C-string boundary";goto fail;}
                    append_codepoint(&b,cp);continue;
                }
                default:p->err="invalid escape";goto fail;
            }
        }
        char ch=(char)c;strbuf_append(&b,&ch,1);
    }
    if(p->s[p->pos]!='"') {p->err="unterminated string";goto fail;} p->pos++;
    char *s=strbuf_detach(&b);if(!s) {p->err="allocation/size limit";return NULL;}
    if(!utf8_valid(s)) {free(s);p->err="invalid UTF-8";return NULL;}return s;
fail:strbuf_free(&b);return NULL;
}
static JsonValue *value(Parser *p);
static JsonValue *number(Parser *p) {
    size_t begin=p->pos;
    if(p->s[p->pos]=='-') p->pos++;
    if(p->s[p->pos]=='0') p->pos++;
    else {
        if(p->s[p->pos]<'1' || p->s[p->pos]>'9') goto invalid;
        while(isdigit((unsigned char)p->s[p->pos])) p->pos++;
    }
    if(p->s[p->pos]=='.') {
        p->pos++;if(!isdigit((unsigned char)p->s[p->pos])) goto invalid;
        while(isdigit((unsigned char)p->s[p->pos])) p->pos++;
    }
    if(p->s[p->pos]=='e' || p->s[p->pos]=='E') {
        p->pos++;if(p->s[p->pos]=='+' || p->s[p->pos]=='-') p->pos++;
        if(!isdigit((unsigned char)p->s[p->pos])) goto invalid;
        while(isdigit((unsigned char)p->s[p->pos])) p->pos++;
    }
    char *lexeme=str_ndup(p->s+begin,p->pos-begin);if(!lexeme) return NULL;
    locale_t loc=newlocale(LC_NUMERIC_MASK,"C",NULL);if(!loc) {free(lexeme);return NULL;}
    errno=0;double n=strtod_l(lexeme,NULL,loc);freelocale(loc);
    if(!isfinite(n) || errno==ERANGE) {free(lexeme);p->err="number out of range";return NULL;}
    JsonValue *v=alloc_value(JSON_NUMBER);if(!v) {free(lexeme);return NULL;}
    v->number=n;v->number_text=lexeme;return v;
invalid:p->err="invalid number";return NULL;
}
static JsonValue *container(Parser *p,bool object) {
    if(p->depth>=ATTRACTOR_JSON_DEPTH) {p->err="nesting limit";return NULL;}
    p->depth++;p->pos++; JsonValue *v=alloc_value(object?JSON_OBJECT:JSON_ARRAY);if(!v) {p->depth--;return NULL;}
    ws(p);char close=object?'}':']';if(p->s[p->pos]==close) {p->pos++;p->depth--;return v;}
    for(;;) {
        ws(p);char *key=NULL;
        if(object) {
            key=string_raw(p);if(!key) goto fail;
            if(json_get(v,key)) {free(key);p->err="duplicate key";goto fail;}
            ws(p);if(p->s[p->pos]!=':') {free(key);p->err="expected colon";goto fail;}p->pos++;
        }
        JsonValue *child=value(p);if(!child) {free(key);goto fail;}
        bool ok=object?json_object_set(v,key,child):json_array_push(v,child);free(key);
        if(!ok) {p->err="allocation/size limit";goto fail;}
        ws(p);if(p->s[p->pos]==close) {p->pos++;break;}
        if(p->s[p->pos]!=',') {p->err="expected comma or closing delimiter";goto fail;}p->pos++;
    }
    p->depth--;return v;
fail:p->depth--;json_free(v);return NULL;
}
static JsonValue *value(Parser *p) {
    ws(p); char c=p->s[p->pos];
    if(c=='{') return container(p,true);if(c=='[') return container(p,false);
    if(c=='"') {
        char *s=string_raw(p);if(!s) return NULL;JsonValue *v=alloc_value(JSON_STRING);
        if(!v) {free(s);return NULL;}v->string=s;return v;
    }
    if(c=='-' || isdigit((unsigned char)c)) return number(p);
    if(!strncmp(p->s+p->pos,"true",4)) {p->pos+=4;return json_new_bool(true);}
    if(!strncmp(p->s+p->pos,"false",5)) {p->pos+=5;return json_new_bool(false);}
    if(!strncmp(p->s+p->pos,"null",4)) {p->pos+=4;return json_new_null();}
    p->err="unexpected token";return NULL;
}
JsonValue *json_parse(const char *s,const char **err) {
    if(err) *err=NULL;
    if(!s || strnlen(s,ATTRACTOR_INPUT_LIMIT+1)>ATTRACTOR_INPUT_LIMIT) {if(err) *err="null/oversized input";return NULL;}
    Parser p={.s=s};JsonValue *v=value(&p);ws(&p);
    if(v && s[p.pos]) {json_free(v);v=NULL;p.err="trailing input";}
    if(!v && err) *err=p.err?p.err:"allocation failure";return v;
}
void json_free(JsonValue *v) {
    if(!v) return;free(v->number_text);
    if(v->type==JSON_STRING) free(v->string);
    else if(v->type==JSON_ARRAY) {for(size_t i=0;i<v->array.count;i++) json_free(v->array.items[i]);free(v->array.items);}
    else if(v->type==JSON_OBJECT) {
        for(size_t i=0;i<v->object.count;i++) {free(v->object.keys[i]);json_free(v->object.values[i]);}
        free(v->object.keys);free(v->object.values);
    }free(v);
}
JsonValue *json_get(const JsonValue *v,const char *key) {
    if(!v || v->type!=JSON_OBJECT || !key) return NULL;
    for(size_t i=0;i<v->object.count;i++) if(str_eq(v->object.keys[i],key)) return v->object.values[i];return NULL;
}
const char *json_get_string(const JsonValue *v,const char *k) {JsonValue *c=json_get(v,k);return c && c->type==JSON_STRING?c->string:NULL;}
double json_get_number(const JsonValue *v,const char *k,double def) {JsonValue *c=json_get(v,k);return c && c->type==JSON_NUMBER?c->number:def;}
int json_get_int(const JsonValue *v,const char *k,int def) {
    double n=json_get_number(v,k,NAN);return isfinite(n) && n>=INT_MIN && n<=INT_MAX && trunc(n)==n?(int)n:def;
}
bool json_get_bool(const JsonValue *v,const char *k,bool def) {JsonValue *c=json_get(v,k);return c && c->type==JSON_BOOL?c->boolean:def;}
JsonValue *json_array_get(const JsonValue *v,size_t i) {return v && v->type==JSON_ARRAY && i<v->array.count?v->array.items[i]:NULL;}
JsonValue *json_new_object(void) {return alloc_value(JSON_OBJECT);}
JsonValue *json_new_array(void) {return alloc_value(JSON_ARRAY);}
JsonValue *json_new_null(void) {return alloc_value(JSON_NULL);}
JsonValue *json_new_string(const char *s) {
    if(s && !utf8_valid(s)) return NULL;
    JsonValue *v=alloc_value(JSON_STRING);if(!v) return NULL;v->string=str_dup(s?s:"");
    if(!v->string) {json_free(v);return NULL;}return v;
}
JsonValue *json_new_number(double n) {if(!isfinite(n)) return NULL;JsonValue *v=alloc_value(JSON_NUMBER);if(v) v->number=n;return v;}
JsonValue *json_new_bool(bool b) {JsonValue *v=alloc_value(JSON_BOOL);if(v) v->boolean=b;return v;}
bool json_object_set(JsonValue *v,const char *key,JsonValue *child) {
    if(!v || v->type!=JSON_OBJECT || !key || !child || child==v || child->failed) goto fail;
    for(size_t i=0;i<v->object.count;i++) if(str_eq(v->object.keys[i],key)) {
        if(v->object.values[i]!=child) {json_free(v->object.values[i]);v->object.values[i]=child;}return true;
    }
    size_t n;if(!size_add(v->object.count,1,&n)) goto fail;
    char *owned=str_dup(key);if(!owned) goto fail;
    char **keys=mem_reallocarray(v->object.keys,n,sizeof(*keys));
    if(!keys) {free(owned);goto fail;}v->object.keys=keys;
    JsonValue **values=mem_reallocarray(v->object.values,n,sizeof(*values));
    if(!values) {free(owned);goto fail;}v->object.values=values;
    keys[n-1]=owned;values[n-1]=child;v->object.count=n;return true;
fail:if(v) v->failed=true;if(child!=v) json_free(child);return false;
}
bool json_array_push(JsonValue *v,JsonValue *child) {
    size_t n;if(!v || v->type!=JSON_ARRAY || !child || child==v || child->failed || !size_add(v->array.count,1,&n)) goto fail;
    JsonValue **items=mem_reallocarray(v->array.items,n,sizeof(*items));if(!items) goto fail;
    v->array.items=items;items[n-1]=child;v->array.count=n;return true;
fail:if(v) v->failed=true;if(child!=v) json_free(child);return false;
}
static void encode_string(const char *s,StrBuf *b) {
    strbuf_append_cstr(b,"\"");
    for(const unsigned char *p=(const unsigned char *)s;p && *p;p++) {
        switch(*p) {
            case '"':strbuf_append_cstr(b,"\\\"");break;case '\\':strbuf_append_cstr(b,"\\\\");break;
            case '\n':strbuf_append_cstr(b,"\\n");break;case '\r':strbuf_append_cstr(b,"\\r");break;case '\t':strbuf_append_cstr(b,"\\t");break;
            default:if(*p<32) strbuf_appendf(b,"\\u%04x",(unsigned)*p);else strbuf_append(b,(const char *)p,1);
        }
    }strbuf_append_cstr(b,"\"");
}
static void serialize(const JsonValue *v,StrBuf *b,size_t depth) {
    if(!v || v->failed || depth>ATTRACTOR_JSON_DEPTH) {b->failed=true;return;}
    switch(v->type) {
        case JSON_NULL:strbuf_append_cstr(b,"null");break;
        case JSON_BOOL:strbuf_append_cstr(b,v->boolean?"true":"false");break;
        case JSON_NUMBER:
            if(!isfinite(v->number)) {b->failed=true;break;}
            if(v->number_text) strbuf_append_cstr(b,v->number_text);
            else strbuf_appendf(b,"%.17g",v->number);break;
        case JSON_STRING:encode_string(v->string,b);break;
        case JSON_ARRAY:
            strbuf_append_cstr(b,"[");for(size_t i=0;i<v->array.count;i++) {if(i) strbuf_append_cstr(b,",");serialize(v->array.items[i],b,depth+1);}strbuf_append_cstr(b,"]");break;
        case JSON_OBJECT:
            strbuf_append_cstr(b,"{");for(size_t i=0;i<v->object.count;i++) {
                if(i) strbuf_append_cstr(b,",");encode_string(v->object.keys[i],b);strbuf_append_cstr(b,":");serialize(v->object.values[i],b,depth+1);
            }strbuf_append_cstr(b,"}");break;
    }
}
char *json_serialize(const JsonValue *v) {
    locale_t loc=newlocale(LC_NUMERIC_MASK,"C",NULL);if(!loc) return NULL;
    locale_t old=uselocale(loc);StrBuf b;strbuf_init(&b);serialize(v,&b,0);
    uselocale(old);freelocale(loc);return strbuf_detach(&b);
}
