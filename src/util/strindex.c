#include "util/strindex.h"
#include "util/mem.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static size_t hash(const char *key) {
    uint64_t h=1469598103934665603u; /* FNV-1a */
    for(const unsigned char *p=(const unsigned char *)key;*p;p++) {h^=*p;h*=1099511628211u;}
    return (size_t)h;
}
void strindex_init(StrIndex *ix) {*ix=(StrIndex){0};}
void strindex_free(StrIndex *ix) {free(ix->keys);free(ix->values);strindex_init(ix);}
static size_t slot(const StrIndex *ix,const char *key) {
    size_t mask=ix->cap-1,i=hash(key)&mask;
    while(ix->keys[i] && strcmp(ix->keys[i],key)) i=(i+1)&mask;
    return i;
}
static bool grow(StrIndex *ix) {
    size_t cap;if(!size_mul(ix->cap?ix->cap:8,2,&cap)) return false;
    StrIndex next={.keys=mem_calloc(cap,sizeof(*next.keys)),.values=mem_calloc(cap,sizeof(*next.values)),.cap=cap};
    if(!next.keys || !next.values) {free(next.keys);free(next.values);return false;}
    for(size_t i=0;i<ix->cap;i++) if(ix->keys[i]) {
        size_t j=slot(&next,ix->keys[i]);next.keys[j]=ix->keys[i];next.values[j]=ix->values[i];next.count++;
    }
    strindex_free(ix);*ix=next;return true;
}
bool strindex_put(StrIndex *ix,const char *key,size_t value) {
    if(!key) return false;
    /* Keep load at or below one half so probes stay short. */
    if((ix->count+1)*2>ix->cap && !grow(ix)) return false;
    size_t i=slot(ix,key);
    if(!ix->keys[i]) {ix->keys[i]=key;ix->count++;}
    ix->values[i]=value;return true;
}
bool strindex_get(const StrIndex *ix,const char *key,size_t *value) {
    if(!ix || !ix->cap || !key) return false;
    size_t i=slot(ix,key);if(!ix->keys[i]) return false;
    if(value) *value=ix->values[i];return true;
}
