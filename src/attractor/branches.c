#include "attractor/dot_parser.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
static size_t index_of(const DotGraph *g,const char *id) {
    const DotNode *node=dot_find_node(g,id);return node?(size_t)(node-g->nodes):SIZE_MAX;
}
static bool all_paths(const DotAdjacency *a,size_t node,size_t join,unsigned char *colors,size_t depth) {
    if(node==join) return true;
    if(depth>128 || colors[node]==1) return false;
    if(colors[node]==2) return true;
    colors[node]=1;
    if(a->offset[node]==a->offset[node+1]) return false;
    for(size_t k=a->offset[node];k<a->offset[node+1];k++) {
        size_t to=a->target[k];
        if(to==SIZE_MAX || !all_paths(a,to,join,colors,depth+1)) return false;
    }
    colors[node]=2;return true;
}
const DotNode *dot_parallel_join(const DotGraph *g,const DotNode *fork,char **error) {
    if(error) *error=NULL;size_t n=g->node_count;
    size_t *dist=mem_calloc(n,sizeof(*dist)),*queue=mem_calloc(n,sizeof(*queue));
    size_t *hits=mem_calloc(n,sizeof(*hits)),*score=mem_calloc(n,sizeof(*score));
    unsigned char *colors=mem_calloc(n,sizeof(*colors));size_t *owner=mem_calloc(n,sizeof(*owner));
    const DotNode *result=NULL;const char *reason="Parallel allocation failed";size_t branches=0;
    DotAdjacency adj={0};size_t origin=index_of(g,fork->id);
    if(!dist || !queue || !hits || !score || !colors || !owner || !dot_adjacency_build(g,&adj)) goto done;
    if(origin==SIZE_MAX) {reason="Unknown parallel fork";goto done;}
    for(size_t e=adj.offset[origin];e<adj.offset[origin+1];e++) {
        branches++;for(size_t i=0;i<n;i++) dist[i]=SIZE_MAX;
        size_t start=adj.target[e];if(start==SIZE_MAX) {reason="Unknown branch target";goto done;}
        size_t head=0,tail=0;queue[tail++]=start;dist[start]=0;
        while(head<tail) {
            size_t current=queue[head++];hits[current]++;score[current]+=dist[current];
            for(size_t k=adj.offset[current];k<adj.offset[current+1];k++) {
                size_t to=adj.target[k];if(to!=SIZE_MAX && dist[to]==SIZE_MAX) {dist[to]=dist[current]+1;queue[tail++]=to;}
            }
        }
    }
    if(branches<2) {reason="Parallel fork requires at least two branches";goto done;}
    size_t join=SIZE_MAX;
    for(size_t i=0;i<n;i++) if(hits[i]==branches && i!=origin &&
        (str_eq(dot_node_role(&g->nodes[i]),"parallel.fan_in") || str_eq(dot_node_role(&g->nodes[i]),"exit"))) {
        if(join==SIZE_MAX || score[i]<score[join] || (score[i]==score[join] && strcmp(g->nodes[i].id,g->nodes[join].id)<0)) join=i;
    }
    if(join==SIZE_MAX) {reason="Parallel branches require a common fan-in or terminal join";goto done;}
    size_t branch=0;
    for(size_t e=adj.offset[origin];e<adj.offset[origin+1];e++) {
        branch++;for(size_t i=0;i<n;i++) colors[i]=0;
        if(!all_paths(&adj,adj.target[e],join,colors,0)) {reason="Branch cycle, excessive depth, or path outside join region";goto done;}
        for(size_t i=0;i<n;i++) if(colors[i]) {
            if(owner[i] && owner[i]!=branch) {reason="Branches overlap before their join";goto done;}
            owner[i]=branch;
        }
    }
    result=&g->nodes[join];
done:
    free(dist);free(queue);free(hits);free(score);free(colors);free(owner);dot_adjacency_free(&adj);
    if(!result && error) *error=str_dup(reason);return result;
}
