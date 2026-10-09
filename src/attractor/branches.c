#include "attractor/dot_parser.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
static size_t index_of(const DotGraph *g,const char *id) {
    for(size_t i=0;i<g->node_count;i++) if(str_eq(g->nodes[i].id,id)) return i;return SIZE_MAX;
}
static bool all_paths(const DotGraph *g,size_t node,size_t join,unsigned char *colors,size_t depth) {
    if(node==join) return true;
    if(depth>128 || colors[node]==1) return false;
    if(colors[node]==2) return true;
    colors[node]=1;size_t edges=0;
    for(size_t i=0;i<g->edge_count;i++) if(str_eq(g->edges[i].from,g->nodes[node].id)) {
        size_t to=index_of(g,g->edges[i].to);edges++;
        if(to==SIZE_MAX || !all_paths(g,to,join,colors,depth+1)) return false;
    }
    if(!edges) return false;colors[node]=2;return true;
}
const DotNode *dot_parallel_join(const DotGraph *g,const DotNode *fork,char **error) {
    if(error) *error=NULL;size_t n=g->node_count;
    size_t *dist=mem_calloc(n,sizeof(*dist)),*queue=mem_calloc(n,sizeof(*queue));
    size_t *hits=mem_calloc(n,sizeof(*hits)),*score=mem_calloc(n,sizeof(*score));
    unsigned char *colors=mem_calloc(n,sizeof(*colors));size_t *owner=mem_calloc(n,sizeof(*owner));
    const DotNode *result=NULL;const char *reason="Parallel allocation failed";size_t branches=0;
    if(!dist || !queue || !hits || !score || !colors || !owner) goto done;
    for(size_t e=0;e<g->edge_count;e++) if(str_eq(g->edges[e].from,fork->id)) {
        branches++;for(size_t i=0;i<n;i++) dist[i]=SIZE_MAX;
        size_t start=index_of(g,g->edges[e].to);if(start==SIZE_MAX) {reason="Unknown branch target";goto done;}
        size_t head=0,tail=0;queue[tail++]=start;dist[start]=0;
        while(head<tail) {
            size_t current=queue[head++];hits[current]++;score[current]+=dist[current];
            for(size_t k=0;k<g->edge_count;k++) if(str_eq(g->edges[k].from,g->nodes[current].id)) {
                size_t to=index_of(g,g->edges[k].to);if(to!=SIZE_MAX && dist[to]==SIZE_MAX) {dist[to]=dist[current]+1;queue[tail++]=to;}
            }
        }
    }
    if(branches<2) {reason="Parallel fork requires at least two branches";goto done;}
    size_t join=SIZE_MAX;
    for(size_t i=0;i<n;i++) if(hits[i]==branches && !str_eq(g->nodes[i].id,fork->id) &&
        (str_eq(dot_node_role(&g->nodes[i]),"parallel.fan_in") || str_eq(dot_node_role(&g->nodes[i]),"exit"))) {
        if(join==SIZE_MAX || score[i]<score[join] || (score[i]==score[join] && strcmp(g->nodes[i].id,g->nodes[join].id)<0)) join=i;
    }
    if(join==SIZE_MAX) {reason="Parallel branches require a common fan-in or terminal join";goto done;}
    size_t branch=0;
    for(size_t e=0;e<g->edge_count;e++) if(str_eq(g->edges[e].from,fork->id)) {
        branch++;for(size_t i=0;i<n;i++) colors[i]=0;
        size_t start=index_of(g,g->edges[e].to);
        if(!all_paths(g,start,join,colors,0)) {reason="Branch cycle, excessive depth, or path outside join region";goto done;}
        for(size_t i=0;i<n;i++) if(colors[i]) {
            if(owner[i] && owner[i]!=branch) {reason="Branches overlap before their join";goto done;}
            owner[i]=branch;
        }
    }
    result=&g->nodes[join];
done:
    free(dist);free(queue);free(hits);free(score);free(colors);free(owner);
    if(!result && error) *error=str_dup(reason);return result;
}
