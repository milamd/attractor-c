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
void strindex_free(StrIndex *ix) {free(ix->slots);strindex_init(ix);}
static size_t slot(const StrIndex *ix,const char *key,size_t h) {
    size_t mask=ix->cap-1,i=h&mask;
    while(ix->slots[i].key && (ix->slots[i].hash!=h || strcmp(ix->slots[i].key,key))) i=(i+1)&mask;
    return i;
}
static bool grow(StrIndex *ix) {
    size_t cap;if(!size_mul(ix->cap?ix->cap:8,2,&cap)) return false;
    StrIndexSlot *slots=mem_calloc(cap,sizeof(*slots));
    if(!slots) return false;
    size_t mask=cap-1;
    for(size_t i=0;i<ix->cap;i++) if(ix->slots[i].key) {
        size_t j=ix->slots[i].hash&mask;
        while(slots[j].key) j=(j+1)&mask;
        slots[j]=ix->slots[i];
    }
    free(ix->slots);ix->slots=slots;ix->cap=cap;return true;
}
bool strindex_put(StrIndex *ix,const char *key,size_t value) {
    if(!key) return false;
    /* Keep load at or below one half so probes stay short. */
    if((ix->count+1)*2>ix->cap && !grow(ix)) return false;
    size_t h=hash(key),i=slot(ix,key,h);
    if(!ix->slots[i].key) {ix->slots[i].key=key;ix->slots[i].hash=h;ix->count++;}
    ix->slots[i].value=value;return true;
}
bool strindex_get(const StrIndex *ix,const char *key,size_t *value) {
    if(!ix || !ix->cap || !key) return false;
    size_t h=hash(key),i=slot(ix,key,h);if(!ix->slots[i].key) return false;
    if(value) *value=ix->slots[i].value;return true;
}
