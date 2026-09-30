#include "sol/mir_runtime_cleanup.h"
#include "mir_runtime_cleanup_internal.h"
#include "mir_linkage_internal.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local size_t *scan_meter_work;
static _Thread_local size_t scan_meter_limit;
static _Thread_local bool scan_meter_failed;
static bool scan_meter_tick(void) {
    if (scan_meter_work != NULL) {
        if (*scan_meter_work == SIZE_MAX || *scan_meter_work >= scan_meter_limit) {
            scan_meter_failed = true;
            return false;
        }
        ++*scan_meter_work;
    }
    return true;
}
#define METER() scan_meter_tick()

typedef enum { DEAD, UNINITIALIZED, INITIALIZED, MAYBE } Storage;
typedef enum { HOLE_ABSENT, HOLE_MUST, HOLE_MAY } Hole;
typedef struct {
    const SolMirMaterialization *m; const SolMirMaterializedImage *image;
    size_t image_id, blocks, locals, places, width;
    Storage *storage; unsigned char *holes; size_t *temps, *temp_scopes, *snaps, *scopes, *regions, *handlers;
    size_t *td, *sd, *cd, *rd, *hd; unsigned char *known;
} Replay;
typedef struct { SolMirRuntimeCleanup *out; SolMirRuntimeCleanupUsage count;
    size_t *work; bool writing, simulate_write, failed; } Sink;
typedef struct { unsigned char *bytes; size_t capacity, used; } Workspace;



#ifdef SOL_MIR_PLAN_TEST_HOOKS
static _Thread_local bool force_build_scratch, force_persistent, force_validation_scratch;
static _Thread_local size_t build_scratch_attempts, persistent_attempts, validation_scratch_attempts;
static _Thread_local size_t fail_build_scratch_attempt, fail_persistent_attempt,
    fail_validation_scratch_attempt;
void sol_mir_runtime_cleanup_test_force_allocation_failure(bool value) { force_persistent=value; }
void sol_mir_runtime_cleanup_test_force_build_scratch_failure(bool value) { force_build_scratch=value; }
void sol_mir_runtime_cleanup_test_force_persistent_allocation_failure(bool value) { force_persistent=value; }
void sol_mir_runtime_cleanup_test_force_validation_scratch_failure(bool value) { force_validation_scratch=value; }
void sol_mir_runtime_cleanup_test_force_build_scratch_failure_attempt(size_t value) { fail_build_scratch_attempt=value; }
void sol_mir_runtime_cleanup_test_force_persistent_allocation_failure_attempt(size_t value) { fail_persistent_attempt=value; }
void sol_mir_runtime_cleanup_test_force_validation_scratch_failure_attempt(size_t value) { fail_validation_scratch_attempt=value; }
size_t sol_mir_runtime_cleanup_test_build_scratch_attempts(void) { return build_scratch_attempts; }
size_t sol_mir_runtime_cleanup_test_persistent_allocation_attempts(void) { return persistent_attempts; }
size_t sol_mir_runtime_cleanup_test_validation_scratch_attempts(void) { return validation_scratch_attempts; }
#endif
static bool add(size_t *a, size_t b) { if (b > SIZE_MAX - *a) return false; *a += b; return true; }
static bool meter_work(size_t *work) {
    if (work == NULL) return true;
    return work == scan_meter_work ? METER() : add(work, 1);
}
static bool mul(size_t a,size_t b,size_t *r) { if(a && b>SIZE_MAX/a)return false;*r=a*b;return true; }
static bool report(SolDiagnostics *d,const char *s) { if(d)sol_diagnostics_add(d,"SOL-MIR-RUNTIME-CLEANUP-001",SOL_SEVERITY_ERROR,(SolSpan){0},s);return false; }
static bool zeros(SolMirRuntimeCleanupLimits x) { return !x.max_events&&!x.max_actions&&!x.max_transitions&&!x.max_supplemental_sites&&!x.max_drop_paths&&!x.max_owned_bytes&&!x.max_build_scratch_bytes&&!x.max_build_work&&!x.max_validation_scratch_bytes&&!x.max_validation_work; }
static bool complete(SolMirRuntimeCleanupLimits x) { return x.max_events&&x.max_actions&&x.max_transitions&&x.max_supplemental_sites&&x.max_drop_paths&&x.max_owned_bytes&&x.max_build_scratch_bytes&&x.max_build_work&&x.max_validation_scratch_bytes&&x.max_validation_work; }
static bool empty(const SolMirRuntimeCleanup *c) { SolMirRuntimeCleanupUsage z={0}; return c&&c->conventions==NULL&&c->values==NULL&&!c->events&&!c->event_count&&!c->event_capacity&&!c->actions&&!c->action_count&&!c->action_capacity&&!c->transitions&&!c->transition_count&&!c->transition_capacity&&!c->supplemental_sites&&!c->supplemental_site_count&&!c->supplemental_site_capacity&&!c->drop_paths&&!c->drop_path_count&&!c->drop_path_capacity&&zeros(c->limits)&&!memcmp(&c->usage,&z,sizeof z); }
void sol_mir_runtime_cleanup_init(SolMirRuntimeCleanup *c) { if(c)memset(c,0,sizeof *c); }
void sol_mir_runtime_cleanup_free(SolMirRuntimeCleanup *c) { if(!c)return;free(c->events);free(c->actions);free(c->transitions);free(c->supplemental_sites);free(c->drop_paths);sol_mir_runtime_cleanup_init(c); }
SolMirRuntimeCleanupLimits sol_mir_runtime_cleanup_default_limits(void) { return (SolMirRuntimeCleanupLimits){.max_events=40000000,.max_actions=160000000,.max_transitions=160000000,.max_supplemental_sites=16000000,.max_drop_paths=160000000,.max_owned_bytes=1024u*1024u*1024u,.max_build_scratch_bytes=512u*1024u*1024u,.max_build_work=(size_t)4000000000ULL,.max_validation_scratch_bytes=1024u*1024u*1024u,.max_validation_work=(size_t)4000000000ULL}; }
static void *allocate(size_t n,size_t s) {
    if (!n || n > SIZE_MAX / s) return NULL;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    ++persistent_attempts;
    if (force_persistent || persistent_attempts == fail_persistent_attempt) return NULL;
#endif
    return calloc(n, s);
}
static void *scratch_allocate(size_t bytes) {
    if (!bytes) return NULL;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    ++build_scratch_attempts;
    if (force_build_scratch || build_scratch_attempts == fail_build_scratch_attempt) return NULL;
#endif
    return calloc(1, bytes);
}
void *sol_mir_runtime_cleanup_internal_validation_scratch_allocate(size_t bytes) {
    if (!bytes) return NULL;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    ++validation_scratch_attempts;
    if (force_validation_scratch || validation_scratch_attempts == fail_validation_scratch_attempt)
        return NULL;
#endif
    return calloc(1, bytes);
}
void sol_mir_runtime_cleanup_internal_validation_scratch_free(void *bytes) { free(bytes); }
static void *workspace_take(Workspace *w,size_t count,size_t size) {
    size_t bytes,at,align=_Alignof(max_align_t);
    if(!w||!mul(count,size,&bytes)||w->used>SIZE_MAX-(align-1))return NULL;
    at=(w->used+align-1)&~(align-1);
    if(at>w->capacity||bytes>w->capacity-at)return NULL;
    void *p=w->bytes+at;w->used=at+bytes;return p;
}
static bool source_for(const SolIr *ir,SolSpan span,SolMirRuntimeSource *out) { bool one=false;if(span.start>span.end)return false;for(size_t i=0;i<ir->file_count;i++){const SolIrSourceFile *f=&ir->files[i];if(span.start<f->aggregate_start||span.end>f->aggregate_end)continue;if(one)return false;*out=(SolMirRuntimeSource){i,span.start-f->aggregate_start,span.end-f->aggregate_start};one=true;}return one; }
static uint32_t failbit(SolMirRuntimeFailureCode c) { return c ? UINT32_C(1)<<((unsigned)c-1) : 0; }
static SolMirRuntimeFailureSiteId inherited(const SolMirRuntimeConventions *c,size_t owner,size_t block,size_t op) { for(size_t i=0;i<c->failure_site_count;i++){if(!METER())return SOL_MIR_RUNTIME_NONE;const SolMirRuntimeFailureSite *s=&c->failure_sites[i];if(s->owner==owner&&s->block==block&&s->instruction==op)return i;}return SOL_MIR_RUNTIME_NONE; }
static bool is_root(const SolMirMaterialization *m,size_t place,size_t local) { return place<m->place_count&&m->places[place].local==local&&!m->places[place].projections.count; }
static size_t root_place(const Replay *r,size_t local) { for(size_t p=0;p<r->places;p++){if(!METER())return SOL_MIR_RUNTIME_NONE;size_t x=r->image->places.offset+p;if(is_root(r->m,x,local))return x;}return SOL_MIR_RUNTIME_NONE; }
static SolMirRuntimeCleanupActionKind drop_kind(const SolMirMaterializedLocal *local) {
    return local->kind == SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
            || local->kind == SOL_MIR_MATERIALIZED_LOCAL_RECEIVER
        ? SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER
        : SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE;
}
/* Snapshots are identified by the immutable P3.2 operation plan, not by the
 * executable instruction number.  The plan in turn fixes image-local slot and
 * copy recipe. */
static size_t snapshot_plan(const SolMirOperations *ops, size_t image, size_t instruction) {
    for (size_t i = 0; i < ops->snapshot_count; ++i) {
        if (!METER()) return SOL_MIR_RUNTIME_NONE;
        if (ops->snapshots[i].image == image && ops->snapshots[i].instruction == instruction)
            return i;
    }
    return SOL_MIR_RUNTIME_NONE;
}
static bool has_hole(const Replay *r,const unsigned char *holes,size_t local) { for(size_t p=0;p<r->places;p++){if(!METER())return false;if(r->m->places[r->image->places.offset+p].local==local&&holes[p])return true;}return false; }
static void clear_local(const Replay *r,unsigned char *holes,size_t local) { for(size_t p=0;p<r->places;p++){if(!METER())return;if(r->m->places[r->image->places.offset+p].local==local)holes[p]=0;} }
static bool prefix(const SolMirMaterialization *m,size_t a,size_t b) { const SolMirMaterializedPlace *x=&m->places[a],*y=&m->places[b];if(x->local!=y->local||x->projections.count>y->projections.count)return false;for(size_t i=0;i<x->projections.count;i++){if(!METER())return false;const SolMirMaterializedProjection *p=&m->projections[x->projections.offset+i],*q=&m->projections[y->projections.offset+i];if(p->kind!=q->kind||p->source_field!=q->source_field||p->tuple_ordinal!=q->tuple_ordinal)return false;}return true; }
static void clear_covered(const Replay *r,unsigned char *holes,size_t place) { for(size_t p=0;p<r->places;p++){if(!METER())return;if(prefix(r->m,place,r->image->places.offset+p))holes[p]=0;} }
static bool stack_equal(const size_t *a,size_t an,const size_t *b,size_t bn) { return an==bn&&(!an||!memcmp(a,b,an*sizeof *a)); }
static bool merge_storage(Storage *to,const Storage *from,size_t n,bool *changed){for(size_t i=0;i<n;i++){Storage x=to[i];if(x!=from[i]){if(x==DEAD||from[i]==DEAD)return false;x=MAYBE;}if(x!=to[i]){to[i]=x;*changed=true;}}return true;}
static size_t stack_width(const Replay *r) { size_t w=r->image->temporaries.count; if(r->image->instructions.count>w)w=r->image->instructions.count; if(r->image->handlers.count>w)w=r->image->handlers.count; if(!w)w=1; return w; }
static void replay_free(Replay *r) { memset(r,0,sizeof *r); }
static bool replay_init(Replay *r,const SolMirMaterialization *m,size_t image,Workspace*w) { memset(r,0,sizeof *r);r->m=m;r->image=&m->images[image];r->image_id=image;r->blocks=r->image->blocks.count;r->locals=r->image->locals.count;r->places=r->image->places.count;r->width=stack_width(r);size_t n;
#define ALLOC(member,count,type) do { if((count)&&((member)=workspace_take(w,(count),sizeof(type)))==NULL)return false; } while(0)
 if(!mul(r->blocks,r->locals,&n))return false;ALLOC(r->storage,n,Storage);if(!mul(r->blocks,r->places,&n))return false;ALLOC(r->holes,n,unsigned char);if(!mul(r->blocks,r->width,&n))return false;ALLOC(r->temps,n,size_t);ALLOC(r->temp_scopes,n,size_t);ALLOC(r->snaps,n,size_t);ALLOC(r->scopes,n,size_t);ALLOC(r->regions,n,size_t);ALLOC(r->handlers,n,size_t);ALLOC(r->td,r->blocks,size_t);ALLOC(r->sd,r->blocks,size_t);ALLOC(r->cd,r->blocks,size_t);ALLOC(r->rd,r->blocks,size_t);ALLOC(r->hd,r->blocks,size_t);ALLOC(r->known,r->blocks,unsigned char);
#undef ALLOC
 return true; }
static Storage *st(const Replay*r,size_t b){return r->storage+b*r->locals;} static unsigned char *ho(const Replay*r,size_t b){return r->holes+b*r->places;} static size_t *ss(size_t *p,const Replay*r,size_t b){return p+b*r->width;}
/* Applies executable materialized effects.  The materializer has authenticated
 * all individual references; this replay still rejects impossible states. */
static bool apply_instruction(const Replay *r,const SolMirMaterializedInstruction *in,Storage *s,unsigned char *h,size_t *temp,size_t *temp_scope,size_t *td,size_t *snap,size_t *sd,size_t *scope,size_t *cd,size_t *region,size_t *rd,size_t *handler,size_t *hd) { const SolMirMaterialization*m=r->m;size_t local=SOL_MIR_MATERIALIZED_NONE;if(in->place<m->place_count)local=m->places[in->place].local;else if(in->local>=r->image->locals.offset&&in->local<r->image->locals.offset+r->locals)local=in->local; size_t li=local==SOL_MIR_MATERIALIZED_NONE?0:local-r->image->locals.offset;
#define PUSH(a,d,x) do{if(*(d)>=r->width)return false;(a)[(*(d))++]=(x);}while(0)
#define POP(a,d,x) do{if(!*(d)||(a)[*(d)-1]!=(x))return false;--*(d);}while(0)
 switch(in->kind){
 case SOL_MIR_INST_PARAMETER_LIVE: if(local==SOL_MIR_MATERIALIZED_NONE||s[li]!=DEAD)return false;s[li]=INITIALIZED;break;
 case SOL_MIR_INST_STORAGE_LIVE: if(local==SOL_MIR_MATERIALIZED_NONE||s[li]!=DEAD)return false;s[li]=UNINITIALIZED;break;
 case SOL_MIR_INST_DROP_IF_INITIALIZED: if(local==SOL_MIR_MATERIALIZED_NONE)return false;if(s[li]==DEAD)break;s[li]=UNINITIALIZED;clear_local(r,h,local);break;
 case SOL_MIR_INST_STORAGE_DEAD: if(local==SOL_MIR_MATERIALIZED_NONE||s[li]!=UNINITIALIZED)return false;s[li]=DEAD;break;
 case SOL_MIR_INST_LOAD_MOVE: if(local==SOL_MIR_MATERIALIZED_NONE||s[li]!=INITIALIZED)return false;if(!m->places[in->place].projections.count){s[li]=UNINITIALIZED;clear_local(r,h,local);}else{clear_covered(r,h,in->place);h[in->place-r->image->places.offset]=HOLE_MUST;}break;
 case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED: if(local==SOL_MIR_MATERIALIZED_NONE||s[li]!=INITIALIZED)return false;if(!m->places[in->place].projections.count){s[li]=UNINITIALIZED;clear_local(r,h,local);}else{clear_covered(r,h,in->place);h[in->place-r->image->places.offset]=HOLE_MUST;}break;
 case SOL_MIR_INST_STORE: if(local==SOL_MIR_MATERIALIZED_NONE)return false;if(!m->places[in->place].projections.count){if(s[li]!=UNINITIALIZED)return false;s[li]=INITIALIZED;clear_local(r,h,local);}else clear_covered(r,h,in->place);break;
 case SOL_MIR_INST_TEMPORARY_INIT:
  if(!*cd||*td>=r->width)return false;
  temp[*td]=in->temporary;temp_scope[*td]=scope[*cd-1];++*td;break;
 case SOL_MIR_INST_TEMPORARY_DROP: {size_t at=in->preserve_depth;if(at>=*td||temp[at]!=in->temporary)return false;memmove(temp+at,temp+at+1,(*td-at-1)*sizeof *temp);memmove(temp_scope+at,temp_scope+at+1,(*td-at-1)*sizeof *temp_scope);--*td;break;}
 case SOL_MIR_INST_CONSTRUCT: if(in->construct_operands.count>*td)return false;*td-=in->construct_operands.count;break;
 case SOL_MIR_INST_COMPOUND_UPDATE: if(!*td||temp[*td-1]!=in->previous)return false;--*td;break;
 case SOL_MIR_INST_CAPTURE_SNAPSHOT:PUSH(snap,sd,(size_t)(in-m->instructions));break;
 case SOL_MIR_INST_REGION_ENTER:PUSH(region,rd,in->source_statement);break;
 case SOL_MIR_INST_REGION_EXIT:POP(region,rd,in->source_statement);break;
 case SOL_MIR_INST_HANDLER_ENTER:PUSH(handler,hd,in->handler);break;
 case SOL_MIR_INST_HANDLER_EXIT:POP(handler,hd,in->handler);break;
 case SOL_MIR_INST_SCOPE_ENTER:PUSH(scope,cd,(size_t)(in-m->instructions));break;
 case SOL_MIR_INST_SCOPE_EXIT: if(!*cd)return false;{const SolMirMaterializedInstruction *e=&m->instructions[scope[*cd-1]];if(e->scope_kind!=in->scope_kind||e->scope_source!=in->scope_source)return false;--*cd;}break;
 default:break; }
#undef PUSH
#undef POP
 return true; }
static size_t edges(const SolMirMaterializedTerminator *t, size_t out[3]) {
    size_t n = 0;
#define E(x) do { if (t->x != SOL_MIR_MATERIALIZED_NONE) out[n++] = t->x; } while (0)
    switch (t->kind) {
        case SOL_MIR_TERM_GOTO: case SOL_MIR_TERM_BREAK: case SOL_MIR_TERM_CONTINUE: E(edge); break;
        case SOL_MIR_TERM_BRANCH: E(true_edge); E(false_edge); break;
        case SOL_MIR_TERM_INVOKE: case SOL_MIR_TERM_CHECK_REFINED: E(normal_edge); E(failure_edge); break;
        case SOL_MIR_TERM_PROPAGATE: E(value_edge); E(residual_edge); break;
        case SOL_MIR_TERM_CHECK_CONTRACT: E(satisfied_edge); E(violation_edge); E(failure_edge); break;
        default: break;
    }
#undef E
    return n;
}
static bool apply_terminator_temps(const Replay *r, const SolMirMaterializedTerminator *term, size_t *temp, size_t *depth) {
    size_t consumed = 0;
    if (term->kind == SOL_MIR_TERM_INVOKE) {
        consumed = term->call_kind == SOL_IR_CALL_CALLBACK;
        consumed += term->receiver.source_expression != SOL_IR_NONE && term->receiver.access == SOL_ACCESS_OWNED;
        for (size_t i = 0; i < term->arguments.count; ++i)
            consumed += r->m->call_arguments[term->arguments.offset + i].access == SOL_ACCESS_OWNED;
    } else if (term->kind == SOL_MIR_TERM_CHECK_REFINED || term->kind == SOL_MIR_TERM_PROPAGATE) consumed = 1;
    if (consumed > *depth) return false;
    (void)temp;
    *depth -= consumed;
    return true;
}
static bool compute_states(Replay*r,Workspace*w,size_t *work) { size_t entry=r->image->entry-r->image->blocks.offset;if(entry>=r->blocks)return false;r->known[entry]=1;bool changed=true;size_t pass=0,limit=r->blocks?(r->blocks*(r->locals+r->places+1)+1):1;while(changed&&pass++<limit){if(!METER())return false;changed=false;for(size_t b=0;b<r->blocks;b++){if(!METER())return false;if(!r->known[b])continue;if(!meter_work(work))return false;size_t mark=w->used;Storage *s=workspace_take(w,r->locals,sizeof *s);unsigned char*h=workspace_take(w,r->places,sizeof *h);size_t *tmp=workspace_take(w,r->width,sizeof *tmp),*tmpo=workspace_take(w,r->width,sizeof *tmpo),*sn=workspace_take(w,r->width,sizeof *sn),*sc=workspace_take(w,r->width,sizeof *sc),*re=workspace_take(w,r->width,sizeof *re),*ha=workspace_take(w,r->width,sizeof *ha);if((r->locals&&!s)||(r->places&&!h)||!tmp||!tmpo||!sn||!sc||!re||!ha){w->used=mark;return false;}if(r->locals)memcpy(s,st(r,b),r->locals*sizeof*s);if(r->places)memcpy(h,ho(r,b),r->places);size_t td=r->td[b],sd=r->sd[b],cd=r->cd[b],rd=r->rd[b],hd=r->hd[b];memcpy(tmp,ss(r->temps,r,b),td*sizeof*tmp);memcpy(tmpo,ss(r->temp_scopes,r,b),td*sizeof*tmpo);memcpy(sn,ss(r->snaps,r,b),sd*sizeof*sn);memcpy(sc,ss(r->scopes,r,b),cd*sizeof*sc);memcpy(re,ss(r->regions,r,b),rd*sizeof*re);memcpy(ha,ss(r->handlers,r,b),hd*sizeof*ha);const SolMirMaterializedBlock *bl=&r->m->blocks[r->image->blocks.offset+b];bool ok=true;for(size_t i=0;i<bl->instructions.count&&ok;i++){if(!meter_work(work)){ok=false;break;}ok=apply_instruction(r,&r->m->instructions[bl->instructions.offset+i],s,h,tmp,tmpo,&td,sn,&sd,sc,&cd,re,&rd,ha,&hd);}if(ok)ok=apply_terminator_temps(r,&bl->terminator,tmp,&td);size_t es[3],en=edges(&bl->terminator,es);for(size_t q=0;q<en&&ok;q++){if(!meter_work(work)){ok=false;break;}if(es[q]>=r->m->edge_count){ok=false;break;}size_t dest=r->m->edges[es[q]].block-r->image->blocks.offset;if(dest>=r->blocks){ok=false;break;}if(!r->known[dest]){r->known[dest]=1;if(r->locals)memcpy(st(r,dest),s,r->locals*sizeof*s);if(r->places)memcpy(ho(r,dest),h,r->places);r->td[dest]=td;r->sd[dest]=sd;r->cd[dest]=cd;r->rd[dest]=rd;r->hd[dest]=hd;memcpy(ss(r->temps,r,dest),tmp,td*sizeof*tmp);memcpy(ss(r->temp_scopes,r,dest),tmpo,td*sizeof*tmpo);memcpy(ss(r->snaps,r,dest),sn,sd*sizeof*sn);memcpy(ss(r->scopes,r,dest),sc,cd*sizeof*sc);memcpy(ss(r->regions,r,dest),re,rd*sizeof*re);memcpy(ss(r->handlers,r,dest),ha,hd*sizeof*ha);changed=true;}else {if(!meter_work(work)){ok=false;break;}ok=merge_storage(st(r,dest),s,r->locals,&changed);for(size_t p=0;p<r->places;p++){if(!METER()){ok=false;break;}unsigned char a=ho(r,dest)[p], incoming=h[p], x=a==incoming?a:HOLE_MAY;changed|=x!=a;ho(r,dest)[p]=x;}ok=ok&&stack_equal(ss(r->temps,r,dest),r->td[dest],tmp,td)&&stack_equal(ss(r->temp_scopes,r,dest),r->td[dest],tmpo,td)&&stack_equal(ss(r->snaps,r,dest),r->sd[dest],sn,sd)&&stack_equal(ss(r->scopes,r,dest),r->cd[dest],sc,cd)&&stack_equal(ss(r->regions,r,dest),r->rd[dest],re,rd)&&stack_equal(ss(r->handlers,r,dest),r->hd[dest],ha,hd);}}
w->used=mark;if(!ok)return false;}}return !changed; }
static bool sink_event(Sink*s,const SolMirRuntimeCleanupEvent *e) { if(s->writing){if(s->out->event_count>=s->out->event_capacity)return false;s->out->events[s->out->event_count++]=*e;}return add(&s->count.events,1) && meter_work(s->work); }
static bool sink_action(Sink*s,SolMirRuntimeCleanupActionKind k,unsigned f,size_t target,SolMirRecipeId recipe) {
    if (s->writing) {
        if (s->out->action_count >= s->out->action_capacity) return false;
        s->out->actions[s->out->action_count++] = (SolMirRuntimeCleanupAction){
            .kind=k,.flags=f,.target=target,.recipe=recipe,.drop_path=SOL_MIR_RUNTIME_NONE};
    }
    if (!add(&s->count.actions, 1)) return false;
    return meter_work(s->work);
}
static int projection_compare(const SolMirMaterialization *m, size_t a, size_t b) {
    const SolMirMaterializedPlace *x = &m->places[a], *y = &m->places[b];
    size_t n = x->projections.count < y->projections.count ? x->projections.count : y->projections.count;
    for (size_t i = 0; i < n; ++i) {
        if (!METER()) return 0;
        const SolMirMaterializedProjection *p = &m->projections[x->projections.offset + i];
        const SolMirMaterializedProjection *q = &m->projections[y->projections.offset + i];
        if (p->kind != q->kind) return p->kind < q->kind ? -1 : 1;
        if (p->source_field != q->source_field) return p->source_field < q->source_field ? -1 : 1;
        if (p->tuple_ordinal != q->tuple_ordinal) return p->tuple_ordinal < q->tuple_ordinal ? -1 : 1;
    }
    return x->projections.count == y->projections.count ? 0 : x->projections.count < y->projections.count ? -1 : 1;
}
static bool selected_hole(const Replay *r, const unsigned char *holes, size_t target, size_t candidate) {
    if (!holes[candidate - r->image->places.offset] || !prefix(r->m, target, candidate)) return false;
    for (size_t q = 0; q < r->places; ++q) {
        if (!METER()) return false;
        size_t other = r->image->places.offset + q;
        if (other != candidate && holes[q] && prefix(r->m, other, candidate)) return false;
    }
    return true;
}
static bool selected_hole_count(const Replay *r, const unsigned char *holes, size_t target,
    size_t *count_out) {
    size_t count = 0;
    for (size_t p = 0; p < r->places; ++p) {
        if (!METER()) return false;
        if (selected_hole(r, holes, target, r->image->places.offset + p)) ++count;
    }
    *count_out = count;
    return !scan_meter_failed;
}
static size_t selected_hole_at(const Replay *r, const unsigned char *holes, size_t target, size_t rank) {
    size_t best = SOL_MIR_RUNTIME_NONE;
    for (size_t p = 0; p < r->places; ++p) {
        if (!METER()) return SOL_MIR_RUNTIME_NONE;
        size_t candidate = r->image->places.offset + p;
        if (!selected_hole(r, holes, target, candidate)) continue;
        size_t before = 0;
        for (size_t q = 0; q < r->places; ++q) {
            if (!METER()) return SOL_MIR_RUNTIME_NONE;
            size_t other = r->image->places.offset + q;
            if (selected_hole(r, holes, target, other) && projection_compare(r->m, other, candidate) < 0) ++before;
        }
        if (before == rank) { best = candidate; break; }
    }
    return best;
}
static bool sink_drop_action(Sink *s, SolMirRuntimeCleanupActionKind kind, unsigned flags,
    size_t target, SolMirRecipeId recipe, const Replay *r, const unsigned char *holes) {
    size_t place = kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE ? target : root_place(r, target);
    if (place == SOL_MIR_RUNTIME_NONE || place >= r->m->place_count) return false;
    size_t root = root_place(r, r->m->places[place].local);
    size_t holes_count, path = s->count.drop_paths;
    if (!selected_hole_count(r, holes, place, &holes_count)) return false;
    if (!sink_action(s, kind, flags, target, recipe) || !add(&s->count.drop_paths, 1 + holes_count)) return false;
    if (s->writing || s->simulate_write) {
        for (size_t i = 0; i < holes_count; ++i) {
            size_t hole = selected_hole_at(r, holes, place, i);
            if (hole == SOL_MIR_RUNTIME_NONE) return false;
            if (!s->writing) continue;
            unsigned char state = holes[hole - r->image->places.offset];
            s->out->drop_paths[path + 1 + i] = (SolMirRuntimeCleanupDropPath){root, hole,
                {0, 0}, recipe, state == HOLE_MUST ? SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE
                    : SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL};
        }
    }
    if (s->writing) {
        if (path > s->out->drop_path_capacity || holes_count >= s->out->drop_path_capacity - path) return false;
        s->out->actions[s->out->action_count - 1].drop_path = path;
        s->out->drop_paths[path] = (SolMirRuntimeCleanupDropPath){root, place,
            {path + 1, holes_count}, recipe, (flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED)
                ? SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL : SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
    }
    return true;
}
static bool sink_transition(Sink*s,SolMirRuntimeCleanupEventId event,SolMirRuntimeCleanupOutcome outcome,size_t edge,size_t offset) { if(s->writing){if(s->out->transition_count>=s->out->transition_capacity)return false;s->out->transitions[s->out->transition_count]=(SolMirRuntimeCleanupTransition){.event=event,.outcome=outcome,.continuation=edge,.actions={offset,s->out->action_count-offset},.primary_failure_wins=true};s->out->transition_count++;}return add(&s->count.transitions,1) && meter_work(s->work); }
static bool sink_supplemental(Sink*s,const SolMirRuntimeCleanupSupplementalSite *site) { if(s->writing){if(s->out->supplemental_site_count>=s->out->supplemental_site_capacity)return false;s->out->supplemental_sites[s->out->supplemental_site_count++]=*site;}return add(&s->count.supplemental_sites,1) && meter_work(s->work); }
static size_t action_pos(const Sink*s) { return s->writing?s->out->action_count:s->count.actions; }
static size_t event_pos(const Sink*s) { return s->writing?s->out->event_count:s->count.events; }
/* Source cleanup slices are the authoritative lexical owner map.  Rebuild
 * the bucket from provenance instead of relying on materialized-local order. */
static bool scope_cleanup_slice(const Replay *r, size_t scope_id, SolIrSlice *slice,
    bool *forward) {
    const SolMirMaterializedInstruction *enter = &r->m->instructions[scope_id];
    const SolIr *ir = r->m->plan->program->ir;
    *slice = (SolIrSlice){0}; *forward = false;
    if (enter->scope_kind == SOL_MIR_SCOPE_BLOCK) {
        if (enter->scope_source >= ir->expression_count) return false;
        *slice = ir->expressions[enter->scope_source].as.block.cleanup;
    } else if (enter->scope_kind == SOL_MIR_SCOPE_MATCH_ARM) {
        if (enter->scope_source >= ir->arm_count) return false;
        *slice = ir->arms[enter->scope_source].cleanup; *forward = true;
    }
    return slice->offset <= ir->cleanup_local_count
        && slice->count <= ir->cleanup_local_count - slice->offset;
}
static bool emit_scope_roots(Sink *s, const Replay *r, Storage *state,
    unsigned char *holes, size_t scope_id) {
    const SolMirMaterialization *m = r->m;
    const SolIr *ir = m->plan->program->ir;
    SolIrSlice slice; bool forward;
    if (!scope_cleanup_slice(r, scope_id, &slice, &forward)) return false;
    for (size_t rank = 0; rank < slice.count; ++rank) {
        size_t index = forward ? rank : slice.count - 1 - rank;
        SolIrLocalId source = ir->cleanup_locals[slice.offset + index];
        for (size_t q = 0; q < r->locals; ++q) {
            size_t local = r->image->locals.offset + q;
            const SolMirMaterializedLocal *item = &m->locals[local];
            if (item->source_local != source || item->kind != SOL_MIR_MATERIALIZED_LOCAL_BODY
                || item->access != SOL_ACCESS_OWNED || state[q] == DEAD
                || state[q] == UNINITIALIZED) continue;
            unsigned flags = (state[q] == MAYBE || has_hole(r, holes, local))
                ? SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED : 0;
            if (!sink_drop_action(s, SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE,
                    flags, root_place(r, local), item->type, r, holes)) return false;
        }
    }
    return true;
}
static size_t active_handler(const Replay *r, const size_t *handlers, size_t hd,
    size_t source) {
    for (size_t i = hd; i; --i) {
        size_t handler = handlers[i - 1];
        if (handler < r->m->handler_count
            && r->m->handlers[handler].source_expression == source) return handler;
    }
    return SOL_MIR_RUNTIME_NONE;
}
static bool emit_implicit_cleanup(Sink *s, const SolMirRuntimeCleanup *owner,
    const Replay *r, Storage *state, unsigned char *holes, size_t *temps,
    size_t *temp_scopes, size_t td, size_t *snapshots, size_t sd,
    size_t *scopes, size_t cd, size_t *regions, size_t rd,
    size_t *handlers, size_t hd) {
    const SolMirMaterialization *m = r->m;
    for (size_t i = cd; i; --i) {
        size_t scope = scopes[i - 1];
        const SolMirMaterializedInstruction *enter = &m->instructions[scope];
        for (size_t q = td; q; --q) if (temp_scopes[q - 1] == scope) {
            size_t temporary = temps[q - 1];
            if (!sink_action(s, SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY, 0,
                    temporary, m->temporaries[temporary].type)) return false;
        }
        if (!emit_scope_roots(s, r, state, holes, scope)
            || !sink_action(s, SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE, 0,
                scope, SOL_MIR_RECIPE_NONE)) return false;
        if (enter->scope_kind == SOL_MIR_SCOPE_REGION) {
            if (!rd || regions[rd - 1] != enter->scope_source
                || !sink_action(s, SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION, 0,
                    enter->scope_source, SOL_MIR_RECIPE_NONE)) return false;
            --rd;
        } else if (enter->scope_kind == SOL_MIR_SCOPE_HANDLER) {
            size_t handler = active_handler(r, handlers, hd, enter->scope_source);
            if (handler == SOL_MIR_RUNTIME_NONE || !sink_action(s,
                    SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_HANDLER, 0, handler,
                    SOL_MIR_RECIPE_NONE)) return false;
            --hd;
        }
    }
    /* Every pending temporary belongs to an active scope. */
    for (size_t q = 0; q < td; ++q) {
        bool active = false;
        for (size_t i = 0; i < cd; ++i) active |= temp_scopes[q] == scopes[i];
        if (!active) return false;
    }
    for (size_t i = sd; i; --i) {
        const SolMirOperations *ops = &owner->conventions->concrete->operations;
        size_t plan = snapshot_plan(ops, r->image_id, snapshots[i - 1]);
        if (plan == SOL_MIR_RUNTIME_NONE || !sink_action(s,
                SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT, 0, plan,
                ops->snapshots[plan].recipe)) return false;
    }
    for (size_t ordinal = r->locals; ordinal; --ordinal) for (size_t i = 0; i < r->locals; ++i) {
        size_t local = r->image->locals.offset + i;
        const SolMirMaterializedLocal *item = &m->locals[local];
        if (item->kind != SOL_MIR_MATERIALIZED_LOCAL_PARAMETER || item->ordinal != ordinal - 1
            || item->access != SOL_ACCESS_OWNED || state[i] == DEAD || state[i] == UNINITIALIZED) continue;
        unsigned flags = (state[i] == MAYBE || has_hole(r, holes, local))
            ? SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED : 0;
        if (!sink_drop_action(s, SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER,
                flags, local, item->type, r, holes)) return false;
    }
    for (size_t i = 0; i < r->locals; ++i) {
        size_t local = r->image->locals.offset + i;
        const SolMirMaterializedLocal *item = &m->locals[local];
        if (item->kind != SOL_MIR_MATERIALIZED_LOCAL_RECEIVER || item->access != SOL_ACCESS_OWNED
            || state[i] == DEAD || state[i] == UNINITIALIZED) continue;
        unsigned flags = (state[i] == MAYBE || has_hole(r, holes, local))
            ? SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED : 0;
        if (!sink_drop_action(s, SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER,
                flags, local, item->type, r, holes)) return false;
    }
    return rd == 0 && hd == 0;
}
static bool implicit_arithmetic(const SolMirConcreteProgram*p,size_t instruction) { for(size_t i=0;i<p->operations.arithmetic_count;i++)if(p->operations.arithmetic[i].instruction==instruction&&p->operations.arithmetic[i].failures)return true;return false; }
static bool allocation_instruction(const SolMirRuntimeValues *v,
    const SolMirMaterialization *m, const SolMirMaterializedInstruction *in) {
    SolMirRecipeId recipe = SOL_MIR_RECIPE_NONE;
    if (in->kind == SOL_MIR_INST_CONST_TEXT || in->kind == SOL_MIR_INST_CONSTRUCT)
        recipe = in->type;
    else if (in->kind == SOL_MIR_INST_LOAD_COPY && in->place < m->place_count)
        recipe = m->places[in->place].final_type;
    return recipe < v->allocation_plan_count
        && v->allocation_plans[recipe].kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE;
}
static bool emit_implicit(Sink*s,const SolMirRuntimeCleanup *owner,const Replay*r,size_t block,size_t instruction,SolSpan span,bool allocation,Storage *stt,unsigned char*holes,size_t*tmp,size_t*tmpo,size_t td,size_t*snap,size_t sd,size_t*scope,size_t cd,size_t*region,size_t rd,size_t*handler,size_t hd) { SolMirRuntimeSource src;if(!source_for(owner->conventions->concrete->program.ir,span,&src))return false;SolMirRuntimeCleanupEvent e={SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION,SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT,r->image_id,block,instruction,src,inherited(owner->conventions,r->image_id,block,instruction),SOL_MIR_RUNTIME_NONE,{action_pos(s),0},{s->count.transitions,0},SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL,false,SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE};e.captures_failure_detail=true;size_t id=event_pos(s);if(allocation&&e.inherited_failure_site==SOL_MIR_RUNTIME_NONE){e.supplemental_site=s->count.supplemental_sites;if(!sink_supplemental(s,&(SolMirRuntimeCleanupSupplementalSite){id,src,failbit(SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT)|failbit(SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED)}))return false;}
 if(!sink_event(s,&e))return false;size_t at=action_pos(s);if(!sink_transition(s,id,SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,SOL_MIR_RUNTIME_NONE,at))return false;at=action_pos(s);if(!emit_implicit_cleanup(s,owner,r,stt,holes,tmp,tmpo,td,snap,sd,scope,cd,region,rd,handler,hd)||!sink_action(s,SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE,SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY,e.inherited_failure_site!=SOL_MIR_RUNTIME_NONE?e.inherited_failure_site:e.supplemental_site,SOL_MIR_RECIPE_NONE)||!sink_transition(s,id,SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE,SOL_MIR_RUNTIME_NONE,at))return false;if(s->writing){SolMirRuntimeCleanupEvent *x=&s->out->events[id];x->actions.count=s->out->action_count-x->actions.offset;x->transitions.count=s->out->transition_count-x->transitions.offset;}return true; }
static bool terminal_failure(SolMirTerminatorKind k){return k==SOL_MIR_TERM_PANIC||k==SOL_MIR_TERM_MATCH_FAILURE||k==SOL_MIR_TERM_UNREACHABLE||k==SOL_MIR_TERM_RESUME_FAILURE||k==SOL_MIR_TERM_CONTRACT_VIOLATION;}
static bool local_or_pending_call(const SolMirRuntimeCleanup *cleanup,
    const SolMirRuntimeCleanupEvent *event, const SolMirRuntimeCleanupTransition *transition) {
    const SolMirMaterialization *m = &cleanup->conventions->concrete->materialization;
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE
        || event->block >= m->block_count || event->inherited_failure_site >= cleanup->conventions->call_count
        || transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE) return false;
    const SolMirMaterializedTerminator *term = &m->blocks[event->block].terminator;
    const SolMirRuntimeCall *call = &cleanup->conventions->calls[event->inherited_failure_site];
    return term->kind == SOL_MIR_TERM_INVOKE && call->failure_site == event->inherited_failure_site
        && call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE && call->image == event->owner
        && call->block == event->block && call->call_kind == SOL_IR_CALL_FUNCTION
        && term->call_kind == SOL_IR_CALL_FUNCTION
        && call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL
        && call->normal_edge == term->normal_edge && call->failure_edge == term->failure_edge
        && transition->continuation == term->failure_edge;
}
 static bool emit_term(Sink*s,const SolMirRuntimeCleanup*owner,const Replay*r,size_t block,const SolMirMaterializedTerminator*t,Storage *stt,unsigned char *holes,size_t *tmp,size_t *tmpo,size_t td,size_t *snap,size_t sd,size_t *scope,size_t cd,size_t *region,size_t rd,size_t *handler,size_t hd){SolMirRuntimeSource src;if(!source_for(owner->conventions->concrete->program.ir,t->span,&src))return false;SolMirRuntimeCleanupEvent e={SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR,SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT,r->image_id,block,SOL_MIR_RUNTIME_NONE,src,inherited(owner->conventions,r->image_id,block,SOL_MIR_RUNTIME_NONE),SOL_MIR_RUNTIME_NONE,{action_pos(s),0},{s->count.transitions,0},SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL,false,SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE};e.captures_failure_detail=terminal_failure(t->kind)||t->kind==SOL_MIR_TERM_INVOKE||t->kind==SOL_MIR_TERM_CHECK_REFINED||t->kind==SOL_MIR_TERM_CHECK_CONTRACT;e.capture_detail_kind=t->kind==SOL_MIR_TERM_PANIC?SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT:SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE;size_t id=event_pos(s);if(!sink_event(s,&e))return false;size_t normal[3],n=edges(t,normal);bool is_failure=terminal_failure(t->kind);for(size_t i=0;i<n;i++){bool evaluation_failure=(t->kind==SOL_MIR_TERM_INVOKE&&normal[i]==t->failure_edge)||(t->kind==SOL_MIR_TERM_CHECK_REFINED&&normal[i]==t->failure_edge)||(t->kind==SOL_MIR_TERM_CHECK_CONTRACT&&normal[i]==t->failure_edge);bool violation=(t->kind==SOL_MIR_TERM_CHECK_CONTRACT&&normal[i]==t->violation_edge);size_t at=action_pos(s);if(!evaluation_failure&&!violation&&t->kind==SOL_MIR_TERM_INVOKE&&normal[i]==t->normal_edge)for(size_t q=0;q<t->writebacks.count;q++){const SolMirMaterializedWriteback*w=&r->m->writebacks[t->writebacks.offset+q];if(!sink_action(s,SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK,SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY,w->place,w->type))return false;}if(!evaluation_failure&&(t->kind==SOL_MIR_TERM_CHECK_REFINED||t->kind==SOL_MIR_TERM_CHECK_CONTRACT)&&!sink_action(s,SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT,normal[i]==t->satisfied_edge?SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY:0,t->source_obligation,SOL_MIR_RECIPE_NONE))return false;if(!sink_transition(s,id,(evaluation_failure||violation)?SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE:SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,normal[i],at))return false;}
 if(t->kind==SOL_MIR_TERM_RETURN||is_failure){size_t at=action_pos(s);if(is_failure&&!emit_implicit_cleanup(s,owner,r,stt,holes,tmp,tmpo,td,snap,sd,scope,cd,region,rd,handler,hd))return false;if(is_failure&&!sink_action(s,SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE,SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY,e.inherited_failure_site,SOL_MIR_RECIPE_NONE))return false;if(!sink_transition(s,id,is_failure?SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE:SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT,SOL_MIR_RUNTIME_NONE,at))return false;}
 if(s->writing){SolMirRuntimeCleanupEvent*x=&s->out->events[id];x->actions.count=s->out->action_count-x->actions.offset;x->transitions.count=s->out->transition_count-x->transitions.offset;}return true; }
static bool emit_cleanup_instruction(Sink *s, const SolMirRuntimeCleanup *owner,
    const Replay *r, size_t block, size_t instruction,
    const SolMirMaterializedInstruction *in, const Storage *state,
    const unsigned char *holes) {
    SolMirRuntimeCleanupActionKind kind;
    size_t target = SOL_MIR_RUNTIME_NONE;
    SolMirRecipeId recipe = SOL_MIR_RECIPE_NONE;
    unsigned flags = 0;
    switch (in->kind) {
        case SOL_MIR_INST_TEMPORARY_DROP:
            kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY;
            target = in->temporary; recipe = r->m->temporaries[target].type; break;
        case SOL_MIR_INST_DROP_IF_INITIALIZED: {
            size_t local = in->local;
            if (local < r->image->locals.offset || local >= r->image->locals.offset + r->locals)
                return false;
            Storage storage = state[local - r->image->locals.offset];
            if (storage == UNINITIALIZED) return true;
            if (storage == DEAD) return true;
            if (r->m->locals[local].access != SOL_ACCESS_OWNED) return true;
            kind = drop_kind(&r->m->locals[local]);
            target = kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER
                ? local : root_place(r, local);
            if (target == SOL_MIR_RUNTIME_NONE || (kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER
                    && root_place(r, local) == SOL_MIR_RUNTIME_NONE)) return true;
            recipe = r->m->locals[local].type;
            if (storage == MAYBE || has_hole(r, holes, local)) flags = SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED;
            break;
        }
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED: {
            size_t local = r->m->places[in->place].local;
            Storage storage = state[local - r->image->locals.offset];
            if (storage == UNINITIALIZED) return true;
            if (storage == DEAD) return true;
            kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE;
            target = in->place; recipe = r->m->places[target].final_type;
            if (storage == MAYBE || has_hole(r, holes, local)) flags = SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED;
            break;
        }
        case SOL_MIR_INST_SCOPE_EXIT:
            kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE; target = instruction; break;
        case SOL_MIR_INST_REGION_EXIT:
            kind = SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION; target = in->source_statement; break;
        default: return true;
    }
    SolMirRuntimeSource src;
    if (!source_for(owner->conventions->concrete->program.ir, in->span, &src)) return false;
    SolMirRuntimeCleanupEvent e = {SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION,
        SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT, r->image_id, block, instruction,
        src, SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE, {action_pos(s), 0},
        {s->count.transitions, 0}, SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL,false,SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE};
    size_t id = event_pos(s);
    if (!sink_event(s, &e) || !((kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE
            || kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER)
            ? sink_drop_action(s, kind, flags, target, recipe, r, holes)
            : sink_action(s, kind, flags, target, recipe))
        || !sink_transition(s, id, SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,
            SOL_MIR_RUNTIME_NONE, e.actions.offset)) return false;
    if (s->writing) {
        s->out->events[id].actions.count = s->out->action_count - e.actions.offset;
        s->out->events[id].transitions.count = 1;
    }
    return true;
}
static bool emit_image(Sink*s,const SolMirRuntimeCleanup*owner,size_t image,Workspace*w){Replay r;if(!replay_init(&r,&owner->conventions->concrete->materialization,image,w)||!compute_states(&r,w,s->work)){replay_free(&r);return false;}const SolMirMaterialization*m=r.m;for(size_t b=0;b<r.blocks;b++){if(!r.known[b])continue;size_t mark=w->used;Storage *state=workspace_take(w,r.locals,sizeof*state);unsigned char*holes=workspace_take(w,r.places,sizeof*holes);size_t*tmp=workspace_take(w,r.width,sizeof*tmp),*tmpo=workspace_take(w,r.width,sizeof*tmpo),*snap=workspace_take(w,r.width,sizeof*snap),*scope=workspace_take(w,r.width,sizeof*scope),*region=workspace_take(w,r.width,sizeof*region),*handler=workspace_take(w,r.width,sizeof*handler);if((r.locals&&!state)||(r.places&&!holes)||!tmp||!tmpo||!snap||!scope||!region||!handler){w->used=mark;replay_free(&r);return false;}if(r.locals)memcpy(state,st(&r,b),r.locals*sizeof*state);if(r.places)memcpy(holes,ho(&r,b),r.places);size_t td=r.td[b],sd=r.sd[b],cd=r.cd[b],rd=r.rd[b],hd=r.hd[b];memcpy(tmp,ss(r.temps,&r,b),td*sizeof*tmp);memcpy(tmpo,ss(r.temp_scopes,&r,b),td*sizeof*tmpo);memcpy(snap,ss(r.snaps,&r,b),sd*sizeof*snap);memcpy(scope,ss(r.scopes,&r,b),cd*sizeof*scope);memcpy(region,ss(r.regions,&r,b),rd*sizeof*region);memcpy(handler,ss(r.handlers,&r,b),hd*sizeof*handler);const SolMirMaterializedBlock*bl=&m->blocks[r.image->blocks.offset+b];bool ok=true;for(size_t i=0;i<bl->instructions.count&&ok;i++){size_t id=bl->instructions.offset+i;const SolMirMaterializedInstruction*in=&m->instructions[id];if(implicit_arithmetic(owner->conventions->concrete,id))ok=emit_implicit(s,owner,&r,r.image->blocks.offset+b,id,in->span,false,state,holes,tmp,tmpo,td,snap,sd,scope,cd,region,rd,handler,hd);if(ok&&allocation_instruction(owner->values,m,in))ok=emit_implicit(s,owner,&r,r.image->blocks.offset+b,id,in->span,true,state,holes,tmp,tmpo,td,snap,sd,scope,cd,region,rd,handler,hd);if(ok)ok=emit_cleanup_instruction(s,owner,&r,r.image->blocks.offset+b,id,in,state,holes);if(ok)ok=apply_instruction(&r,in,state,holes,tmp,tmpo,&td,snap,&sd,scope,&cd,region,&rd,handler,&hd);}if(ok)ok=emit_term(s,owner,&r,r.image->blocks.offset+b,&bl->terminator,state,holes,tmp,tmpo,td,snap,sd,scope,cd,region,rd,handler,hd);w->used=mark;if(!ok){replay_free(&r);return false;}}replay_free(&r);return true;}
static bool predicate_span(const SolMirConcreteProgram*p,size_t block,SolSpan*out){const SolMirOperations*o=&p->operations;const SolMirMaterialization*m=&p->materialization;if(block>=o->predicate_block_count)return false;size_t body=o->predicate_blocks[block].body;if(body>=o->predicate_body_count)return false;size_t context=o->predicate_bodies[body].context;if(context>=m->context_count)return false;size_t obligation=m->contexts[context].obligation;if(obligation>=p->program.ir->obligation_count)return false;size_t expression=p->program.ir->obligations[obligation].predicate;if(expression>=p->program.ir->expression_count)return false;*out=p->program.ir->expressions[expression].span;return true;}
static size_t predicate_edges(const SolMirPredicateTerminator *term, size_t result[2]) {
    size_t n = 0;
    switch (term->kind) {
        case SOL_MIR_PREDICATE_TERM_JUMP: result[n++] = term->edge; break;
        case SOL_MIR_PREDICATE_TERM_BRANCH: result[n++] = term->true_edge; result[n++] = term->false_edge; break;
        case SOL_MIR_PREDICATE_TERM_INVOKE: case SOL_MIR_PREDICATE_TERM_CHECK_REFINED:
            result[n++] = term->normal_edge; result[n++] = term->failure_edge; break;
        case SOL_MIR_PREDICATE_TERM_PROPAGATE:
            result[n++] = term->normal_edge;
            result[n++] = term->failure_edge;
            break;
        default: break;
    }
    return n;
}
static bool emit_predicates(Sink *s, const SolMirRuntimeCleanup *o) {
    const SolMirOperations *ops = &o->conventions->concrete->operations;
    for (size_t block = 0; block < ops->predicate_block_count; ++block) {
        const SolMirPredicateBlock *pb = &ops->predicate_blocks[block];
        SolSpan span;
        if (!predicate_span(o->conventions->concrete, block, &span)) return false;
        SolMirRuntimeSource src;
        if (!source_for(o->conventions->concrete->program.ir, span, &src)) return false;
        for (size_t q = 0; q < pb->instructions.count; ++q) {
            size_t instruction = pb->instructions.offset + q;
            const SolMirPredicateInstruction *in = &ops->predicate_instructions[instruction];
            bool allocation = in->kind == SOL_MIR_PREDICATE_INST_CONSTRUCT
                && in->recipe < o->values->allocation_plan_count
                && o->values->allocation_plans[in->recipe].kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE;
            bool fallible = in->failures != 0 || allocation;
            SolMirRuntimeCleanupEvent e = {SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION,
                fallible ? SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT : SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT,
                pb->body, block, instruction, src,
                inherited(o->conventions, pb->body, block, instruction), SOL_MIR_RUNTIME_NONE,
                {action_pos(s), 0}, {s->count.transitions, 0}, SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL,false,SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE};
            e.captures_failure_detail = fallible;
            size_t id = event_pos(s);
            if (allocation) {
                e.supplemental_site = s->count.supplemental_sites;
                if (!sink_supplemental(s, &(SolMirRuntimeCleanupSupplementalSite){id, src,
                    failbit(SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT) | failbit(SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED)})) return false;
            }
            if (!sink_event(s, &e)) return false;
            size_t at = action_pos(s);
            if (!sink_transition(s, id, SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,
                SOL_MIR_RUNTIME_NONE, at)) return false;
            if (fallible) {
                at = action_pos(s);
                if (!sink_transition(s, id, SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE,
                        SOL_MIR_RUNTIME_NONE, at)) return false;
            }
            if (s->writing) {
                s->out->events[id].actions.count = s->out->action_count - e.actions.offset;
                s->out->events[id].transitions.count = fallible ? 2 : 1;
            }
        }
        const SolMirPredicateTerminator *term = &pb->terminator;
        bool terminal_failure = term->kind == SOL_MIR_PREDICATE_TERM_FAILURE;
        SolMirRuntimeCleanupEvent e = {SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR,
            terminal_failure ? SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT : SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT,
            pb->body, block, SOL_MIR_RUNTIME_NONE, src,
            inherited(o->conventions, pb->body, block, SOL_MIR_RUNTIME_NONE), SOL_MIR_RUNTIME_NONE,
            {action_pos(s), 0}, {s->count.transitions, 0}, SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL,false,SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE};
        e.captures_failure_detail = terminal_failure || term->kind == SOL_MIR_PREDICATE_TERM_INVOKE || term->kind == SOL_MIR_PREDICATE_TERM_CHECK_REFINED;
        size_t id = event_pos(s);
        if (!sink_event(s, &e)) return false;
        size_t edge[2], count = predicate_edges(term, edge);
        for (size_t q = 0; q < count; ++q) {
            bool fail = (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE
                    || term->kind == SOL_MIR_PREDICATE_TERM_CHECK_REFINED)
                && edge[q] == term->failure_edge;
            size_t at = action_pos(s);
            if (!sink_transition(s, id, fail ? SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
                    : SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL, edge[q], at)) return false;
        }
        if (terminal_failure) {
            size_t at = action_pos(s);
            if (!sink_action(s, SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE,
                    SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY, e.inherited_failure_site,
                    SOL_MIR_RECIPE_NONE)
                || !sink_transition(s, id, SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE,
                    SOL_MIR_RUNTIME_NONE, at)) return false;
        }
        if (s->writing) {
            s->out->events[id].actions.count = s->out->action_count - e.actions.offset;
            s->out->events[id].transitions.count = s->out->transition_count - e.transitions.offset;
        }
    }
    return true;
}
static bool scan(Sink*s,const SolMirRuntimeCleanup*o,Workspace*w){
    size_t *saved_work = scan_meter_work, saved_limit = scan_meter_limit;
    bool saved_failed = scan_meter_failed;
    scan_meter_work = s->work; scan_meter_limit = o->limits.max_build_work;
    scan_meter_failed = false;
    bool ok = true;
    for(size_t i=0;i<o->conventions->concrete->materialization.image_count;i++){
        if(!METER()){ok=false;break;}
         w->used=0; if(w->capacity)memset(w->bytes,0,w->capacity);
        if(!emit_image(s,o,i,w)){ok=false;break;}
    }
    if(ok) ok=emit_predicates(s,o);
    bool failed = scan_meter_failed;
    scan_meter_work = saved_work; scan_meter_limit = saved_limit;
    scan_meter_failed = saved_failed;
    return ok && !failed;
}
static bool fit(const SolMirRuntimeCleanupUsage*u,const SolMirRuntimeCleanupLimits*l){return u->events<=l->max_events&&u->actions<=l->max_actions&&u->transitions<=l->max_transitions&&u->supplemental_sites<=l->max_supplemental_sites&&u->drop_paths<=l->max_drop_paths&&u->owned_bytes<=l->max_owned_bytes&&u->build_scratch_bytes<=l->max_build_scratch_bytes&&u->build_work<=l->max_build_work&&u->validation_scratch_bytes<=l->max_validation_scratch_bytes&&u->validation_work<=l->max_validation_work;}
/* Keep the census mechanically identical to workspace_take: every arena and
 * per-block replay scratch allocation advances an aligned offset, and each
 * temporary mark is restored before the next block.  This deliberately does
 * not reserve a guessed alignment gap. */
static bool scratch_take(size_t *used, size_t *peak, size_t count, size_t size) {
    size_t bytes, at, align = _Alignof(max_align_t);
    if (!mul(count, size, &bytes) || *used > SIZE_MAX - (align - 1)) return false;
    at = (*used + align - 1) & ~(align - 1);
    if (bytes > SIZE_MAX - at) return false;
    *used = at + bytes;
    if (*used > *peak) *peak = *used;
    return true;
}
static bool scratch_bytes(const SolMirRuntimeCleanup *o,size_t *out){
    size_t peak=0;
    for(size_t i=0;i<o->conventions->concrete->materialization.image_count;i++){
        const SolMirMaterializedImage*im=&o->conventions->concrete->materialization.images[i];
        size_t n=im->blocks.count,width=im->instructions.count,used=0,mark;
        if(width<im->temporaries.count)width=im->temporaries.count;
        if(width<im->handlers.count)width=im->handlers.count;
        if(!width)width=1;
        size_t nl,np,nw;
        if(!mul(n,im->locals.count,&nl)||!mul(n,im->places.count,&np)||!mul(n,width,&nw))return false;
        if((nl&&!scratch_take(&used,&peak,nl,sizeof(Storage)))
            ||(np&&!scratch_take(&used,&peak,np,sizeof(unsigned char))))return false;
        for(size_t q=0;q<6;q++)if(!scratch_take(&used,&peak,nw,sizeof(size_t)))return false;
        for(size_t q=0;q<5;q++)if(!scratch_take(&used,&peak,n,sizeof(size_t)))return false;
        if(!scratch_take(&used,&peak,n,sizeof(unsigned char)))return false;
        mark=used;
        if(!scratch_take(&used,&peak,im->locals.count,sizeof(Storage))
            ||!scratch_take(&used,&peak,im->places.count,sizeof(unsigned char)))return false;
        for(size_t q=0;q<6;q++)if(!scratch_take(&used,&peak,width,sizeof(size_t)))return false;
        used=mark;
    }
    *out=peak;return true;
}
static bool usage(SolMirRuntimeCleanup *o,Sink*s,Workspace*w) {
    if (!scan(s, o,w)) return false;
    /* Predict the write pass by executing the same emission traversal with a
     * counting sink.  Do not derive it by multiplication: projection/hole and
     * site searches are data-dependent. */
    w->used = 0;
    Sink predicted = {0};
    predicted.work = &predicted.count.build_work;
    if (!scan(&predicted, o, w)
        || predicted.count.events != s->count.events
        || predicted.count.actions != s->count.actions
        || predicted.count.transitions != s->count.transitions
        || predicted.count.supplemental_sites != s->count.supplemental_sites
        || predicted.count.drop_paths != s->count.drop_paths) return false;
    size_t x = 0;
    if (!mul(s->count.events, sizeof *o->events, &x) || !add(&s->count.owned_bytes, x)
        || !mul(s->count.actions, sizeof *o->actions, &x) || !add(&s->count.owned_bytes, x)
        || !mul(s->count.transitions, sizeof *o->transitions, &x) || !add(&s->count.owned_bytes, x)
        || !mul(s->count.supplemental_sites, sizeof *o->supplemental_sites, &x)
        || !add(&s->count.owned_bytes, x)) return false;
    if (!mul(s->count.drop_paths, sizeof *o->drop_paths, &x)
        || !add(&s->count.owned_bytes, x)) return false;
    Sink write_prediction = {0};
    write_prediction.work = &write_prediction.count.build_work;
    write_prediction.simulate_write = true;
    w->used = 0;
    if (!scan(&write_prediction, o, w)
        || write_prediction.count.events != s->count.events
        || write_prediction.count.actions != s->count.actions
        || write_prediction.count.transitions != s->count.transitions
        || write_prediction.count.supplemental_sites != s->count.supplemental_sites
        || write_prediction.count.drop_paths != s->count.drop_paths
        || !add(&s->count.build_work, predicted.count.build_work)
        || !add(&s->count.build_work, write_prediction.count.build_work)) return false;
    /* The independent validator performs the same replay traversal and its
     * owner comparisons.  It records its own counter; this preflight value is
     * the matching traversal budget, not a formula derived from build work. */
    if (!mul(write_prediction.count.build_work, 2, &s->count.validation_work)) return false;
    if(!scratch_bytes(o,&s->count.build_scratch_bytes))return false;
    s->count.validation_scratch_bytes = s->count.build_scratch_bytes;
    return true;
}
static SolMirRuntimeCleanupEdgeRole cleanup_role(const SolMirRuntimeCleanupEvent *event,
    const SolMirRuntimeCleanupTransition *transition,
    const SolMirMaterialization *m, const SolMirOperations *ops) {
    if (transition->continuation == SOL_MIR_RUNTIME_NONE)
        return transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT
            ? SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN
            : transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
                ? SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO
                : SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE;
    if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        && event->block < m->block_count) {
        const SolMirMaterializedTerminator *t = &m->blocks[event->block].terminator;
        if (t->kind == SOL_MIR_TERM_BRANCH)
            return transition->continuation == t->true_edge ? SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE : SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_FALSE;
        if (t->kind == SOL_MIR_TERM_INVOKE)
            return transition->continuation == t->normal_edge ? SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL : SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE;
        if (t->kind == SOL_MIR_TERM_CHECK_REFINED)
            return transition->continuation == t->normal_edge ? SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_SATISFIED : SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE;
        if (t->kind == SOL_MIR_TERM_PROPAGATE)
            return transition->continuation == t->value_edge ? SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_VALUE : SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_RESIDUAL;
        if (t->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
            if (transition->continuation == t->satisfied_edge) return SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_SATISFIED;
            return transition->continuation == t->violation_edge ? SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION : SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_FAILURE;
        }
    }
    if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR
        && event->block < ops->predicate_block_count) {
        const SolMirPredicateTerminator *term = &ops->predicate_blocks[event->block].terminator;
        if (term->kind == SOL_MIR_PREDICATE_TERM_BRANCH)
            return transition->continuation == term->true_edge
                ? SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE : SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_FALSE;
        if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE)
            return transition->continuation == term->normal_edge
                ? SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL : SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE;
        if (term->kind == SOL_MIR_PREDICATE_TERM_CHECK_REFINED)
            return transition->continuation == term->normal_edge
                ? SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_SATISFIED : SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE;
        if (term->kind == SOL_MIR_PREDICATE_TERM_PROPAGATE)
            return transition->continuation == term->normal_edge
                ? SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_VALUE
                : SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_RESIDUAL;
    }
    return SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO;
}
static size_t predicate_body_for_check(const SolMirOperations *ops,
    const SolMirRuntimeCleanupEvent *event) {
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR) return SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < ops->predicate_count; ++i)
        if (ops->predicates[i].image == event->owner && ops->predicates[i].block == event->block)
            return ops->predicates[i].body;
    return SOL_MIR_RUNTIME_NONE;
}
static SolMirRuntimeFailureSiteId predicate_result_site(const SolMirRuntimeConventions *c,
    size_t body) {
    for (size_t i = 0; i < c->failure_site_count; ++i) {
        const SolMirRuntimeFailureSite *site = &c->failure_sites[i];
        if (site->origin_kind == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
            && site->owner == body) return i;
    }
    return SOL_MIR_RUNTIME_NONE;
}
static void cleanup_finalize_metadata(SolMirRuntimeCleanup *cleanup) {
    const SolMirMaterialization *m = &cleanup->conventions->concrete->materialization;
    const SolMirOperations *ops = &cleanup->conventions->concrete->operations;
    for (size_t i = 0; i < cleanup->event_count; ++i) {
        SolMirRuntimeCleanupEvent *event = &cleanup->events[i];
        event->producer = event->origin == SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT
            ? (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
                ? SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_ARITHMETIC
                : SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_ARITHMETIC)
            : SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL;
        if (event->supplemental_site != SOL_MIR_RUNTIME_NONE)
            event->producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION;
        if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            && event->block < m->block_count) {
            switch (m->blocks[event->block].terminator.kind) {
                case SOL_MIR_TERM_INVOKE: event->producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE; break;
                case SOL_MIR_TERM_PANIC: event->producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_PANIC; break;
                case SOL_MIR_TERM_MATCH_FAILURE: event->producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_NO_MATCH; break;
                case SOL_MIR_TERM_UNREACHABLE: event->producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_UNREACHABLE; break;
                default: break;
            }
        }
        if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR
            && event->block < ops->predicate_block_count) {
            const SolMirPredicateTerminator *term = &ops->predicate_blocks[event->block].terminator;
            if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE)
                event->producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_INVOKE;
            else if (term->kind == SOL_MIR_PREDICATE_TERM_FAILURE
                && term->failure_kind == SOL_MIR_PREDICATE_FAILURE_NO_MATCH)
                event->producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_NO_MATCH;
            else if (term->kind == SOL_MIR_PREDICATE_TERM_RETURN)
                event->producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_RESULT;
        }
        for (size_t j = 0; j < event->transitions.count; ++j) {
            SolMirRuntimeCleanupTransition *t = &cleanup->transitions[event->transitions.offset + j];
            t->source_edge = t->continuation;
            bool predicate = event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
                || event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR;
            t->destination = t->continuation == SOL_MIR_RUNTIME_NONE ? SOL_MIR_RUNTIME_NONE
                : predicate && t->continuation < ops->predicate_edge_count
                    ? ops->predicate_edges[t->continuation].target
                    : !predicate && t->continuation < m->edge_count
                        ? m->edges[t->continuation].block : SOL_MIR_RUNTIME_NONE;
            t->edge_role = cleanup_role(event, t, m, ops);
            t->failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE;
            t->failure_site = SOL_MIR_RUNTIME_NONE;
            t->failure_mask = 0;
            if (t->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
                && event->inherited_failure_site != SOL_MIR_RUNTIME_NONE) {
                t->failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31;
                t->failure_site = event->inherited_failure_site;
                t->failure_mask = cleanup->conventions->failure_sites[event->inherited_failure_site].allowed_codes;
                if (local_or_pending_call(cleanup, event, t))
                    t->failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING;
            } else if (t->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
                && event->supplemental_site != SOL_MIR_RUNTIME_NONE) {
                t->failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33;
                t->failure_site = event->supplemental_site;
                t->failure_mask = cleanup->supplemental_sites[event->supplemental_site].allowed_codes;
            }
            t->contract_phase = SOL_CONTRACT_REQUIRES;
            t->contract_outcome = SOL_CONTRACT_OUTCOME_SUCCESS;
            if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
                && event->block < m->block_count) {
                const SolMirMaterializedTerminator *term = &m->blocks[event->block].terminator;
                t->contract_phase = term->contract_phase;
                t->contract_outcome = term->contract_outcome;
            } else if ((event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
                    || event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR)
                && event->owner < ops->predicate_body_count) {
                t->contract_phase = ops->predicate_bodies[event->owner].phase;
                t->contract_outcome = ops->predicate_bodies[event->owner].outcome;
            }
            /* A predicate false result is a violation, not an evaluation
             * failure.  Its primary identity is the P3.1 predicate-result
             * site, while a predicate call/arithmetic/no-match failure keeps
             * its already-pending originating site. */
            if (t->edge_role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION) {
                size_t body = predicate_body_for_check(ops, event);
                SolMirRuntimeFailureSiteId result = predicate_result_site(cleanup->conventions, body);
                if (body != SOL_MIR_RUNTIME_NONE && result != SOL_MIR_RUNTIME_NONE) {
                    t->failure_site = result;
                    t->failure_mask = cleanup->conventions->failure_sites[result].allowed_codes;
                    t->failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31;
                }
            }
            if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
                && event->block < m->block_count
                && m->blocks[event->block].terminator.kind == SOL_MIR_TERM_RESUME_FAILURE) {
                t->failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING;
                t->failure_site = SOL_MIR_RUNTIME_NONE;
                t->failure_mask = 0;
            }
        }
    }
    for (size_t i = 0; i < cleanup->action_count; ++i) {
        SolMirRuntimeCleanupAction *action = &cleanup->actions[i];
        if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT) {
            for (size_t j = 0; j < ops->snapshot_count; ++j) {
                const SolMirOperationSnapshotPlan *plan = &ops->snapshots[j];
                if (plan->instruction != action->target) continue;
                /* The plan authenticates both the capture slot and recipe; no
                 * source snapshot identifier survives into the policy. */
                action->target = j;
                action->recipe = plan->recipe;
                break;
            }
            continue;
        }
        /* Drop paths are emitted with their action while replay state is live. */
    }
}
SolMirRuntimeCleanupBuildOutcome sol_mir_runtime_cleanup_build(const SolMirRuntimeCleanupBuildRequest*r,SolMirRuntimeCleanup*out,SolDiagnostics*d){
 if(!r||!out||!r->conventions||!r->values||!empty(out)||(r->limits&&!zeros(*r->limits)&&!complete(*r->limits))){report(d,"invalid runtime cleanup build request or destination");return SOL_MIR_RUNTIME_CLEANUP_BUILD_INVALID_ARGUMENT;}
 if(r->values->conventions!=r->conventions||!sol_mir_runtime_conventions_validate(r->conventions,d)||!sol_mir_runtime_values_validate(r->values,d)){report(d,"runtime cleanup predecessors are invalid");return SOL_MIR_RUNTIME_CLEANUP_BUILD_INVALID_PREDECESSOR;}
 #ifdef SOL_MIR_PLAN_TEST_HOOKS
  build_scratch_attempts=0; persistent_attempts=0; validation_scratch_attempts=0;
#endif
  SolMirRuntimeCleanup x;sol_mir_runtime_cleanup_init(&x);x.conventions=r->conventions;x.values=r->values;x.limits=!r->limits||zeros(*r->limits)?sol_mir_runtime_cleanup_default_limits():*r->limits;
 size_t bytes;if(!scratch_bytes(&x,&bytes)||bytes>x.limits.max_build_scratch_bytes||bytes>x.limits.max_validation_scratch_bytes){report(d,"runtime cleanup scratch limit exceeded");return SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED;}
 Workspace workspace={(unsigned char*)scratch_allocate(bytes),bytes,0};if(bytes&&!workspace.bytes){if(d)d->allocation_failed=true;report(d,"runtime cleanup scratch allocation failed");return SOL_MIR_RUNTIME_CLEANUP_BUILD_ALLOCATION_FAILED;}
  Sink count={0};count.work=&count.count.build_work;if(!usage(&x,&count,&workspace)){free(workspace.bytes);report(d,"runtime cleanup replay resource census failed");return SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED;}x.usage=count.count;x.events=x.usage.events?(void *)(uintptr_t)UINT64_C(0x100000000):NULL;x.actions=x.usage.actions?(void *)(uintptr_t)UINT64_C(0x200000000):NULL;x.transitions=x.usage.transitions?(void *)(uintptr_t)UINT64_C(0x300000000):NULL;x.supplemental_sites=x.usage.supplemental_sites?(void *)(uintptr_t)UINT64_C(0x400000000):NULL;x.drop_paths=x.usage.drop_paths?(void *)(uintptr_t)UINT64_C(0x500000000):NULL;x.event_capacity=x.usage.events;x.action_capacity=x.usage.actions;x.transition_capacity=x.usage.transitions;x.supplemental_site_capacity=x.usage.supplemental_sites;x.drop_path_capacity=x.usage.drop_paths;size_t preflight_alias_work=0;if(!sol_mir_runtime_cleanup_internal_alias_work(&x,&preflight_alias_work)||!add(&x.usage.validation_work,preflight_alias_work)){free(workspace.bytes);report(d,"runtime cleanup alias-work census failed");return SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED;}x.events=NULL;x.actions=NULL;x.transitions=NULL;x.supplemental_sites=NULL;x.drop_paths=NULL;x.event_capacity=x.action_capacity=x.transition_capacity=x.supplemental_site_capacity=x.drop_path_capacity=0;if(!fit(&x.usage,&x.limits)){free(workspace.bytes);report(d,"runtime cleanup resource limit exceeded");return SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED;}
  x.events=allocate(x.usage.events,sizeof*x.events);if(x.usage.events&&!x.events)goto persistent_failed;
 x.actions=allocate(x.usage.actions,sizeof*x.actions);if(x.usage.actions&&!x.actions)goto persistent_failed;
  x.transitions=allocate(x.usage.transitions,sizeof*x.transitions);if(x.usage.transitions&&!x.transitions)goto persistent_failed;
  x.supplemental_sites=allocate(x.usage.supplemental_sites,sizeof*x.supplemental_sites);if(x.usage.supplemental_sites&&!x.supplemental_sites)goto persistent_failed;
  x.drop_paths=allocate(x.usage.drop_paths,sizeof*x.drop_paths);if(x.usage.drop_paths&&!x.drop_paths)goto persistent_failed;
   x.event_capacity=x.usage.events;x.action_capacity=x.usage.actions;x.transition_capacity=x.usage.transitions;x.supplemental_site_capacity=x.usage.supplemental_sites;x.drop_path_capacity=x.usage.drop_paths;workspace.used=0;size_t write_work=0;Sink write={.out=&x,.work=&write_work,.writing=true};if(!scan(&write,&x,&workspace)||x.usage.validation_work < preflight_alias_work || (x.usage.validation_work-preflight_alias_work) % 2 != 0 || write_work != (x.usage.validation_work-preflight_alias_work) / 2||write.count.events!=x.usage.events||write.count.actions!=x.usage.actions||write.count.transitions!=x.usage.transitions||write.count.supplemental_sites!=x.usage.supplemental_sites||write.count.drop_paths!=x.usage.drop_paths){free(workspace.bytes);sol_mir_runtime_cleanup_free(&x);report(d,"runtime cleanup replay construction failed");return SOL_MIR_RUNTIME_CLEANUP_BUILD_INTERNAL_FAILED;}x.drop_path_count=x.usage.drop_paths;cleanup_finalize_metadata(&x);free(workspace.bytes);SolMirRuntimeCleanupBuildOutcome validation=sol_mir_runtime_cleanup_internal_validate(&x,d);if(validation!=SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED){sol_mir_runtime_cleanup_free(&x);return validation;}*out=x;return SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;
persistent_failed:
 free(workspace.bytes);sol_mir_runtime_cleanup_free(&x);if(d)d->allocation_failed=true;
 report(d,"runtime cleanup persistent allocation failed");return SOL_MIR_RUNTIME_CLEANUP_BUILD_ALLOCATION_FAILED;}
static bool cleanup_same_source(SolMirRuntimeSource a, SolMirRuntimeSource b) {
    return a.file == b.file && a.start == b.start && a.end == b.end;
}
static const SolMirRuntimeCleanupTransition *cleanup_transition_for_role(
    const SolMirRuntimeCleanup *cleanup, const SolMirRuntimeCleanupEvent *event,
    SolMirRuntimeCleanupEdgeRole role) {
    const SolMirRuntimeCleanupTransition *found = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate =
            &cleanup->transitions[event->transitions.offset + i];
        if (candidate->edge_role != role) continue;
        if (found != NULL) return NULL;
        found = candidate;
    }
    return found;
}
static SolMirRuntimeFailureDetailKind cleanup_detail_kind(SolMirRuntimeFailureCode code) {
    return code == SOL_MIR_RUNTIME_FAILURE_PANIC ? SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT
        : code == SOL_MIR_RUNTIME_FAILURE_HOST_ERROR ? SOL_MIR_RUNTIME_FAILURE_DETAIL_HOST_BYTES
        : SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE;
}
static bool cleanup_detail_shape(SolMirRuntimeFailureDetailKind kind,
    SolMirRuntimeFailureCode code, size_t length) {
    if (kind == SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE) return length == 0;
    if (kind == SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT)
        return code == SOL_MIR_RUNTIME_FAILURE_PANIC;
    return kind == SOL_MIR_RUNTIME_FAILURE_DETAIL_HOST_BYTES
        && code == SOL_MIR_RUNTIME_FAILURE_HOST_ERROR
        && length <= SOL_MIR_RUNTIME_HOST_DETAIL_MAX;
}
bool sol_mir_runtime_cleanup_capture_detail(const SolMirRuntimeCleanup *cleanup,
    SolMirRuntimeCleanupEventId event_id, SolMirRuntimeCleanupEdgeRole role,
    const SolMirRuntimeFailureRecord *record, SolMirRuntimeCleanupDetail *out) {
    if (!cleanup || !record || !out || event_id >= cleanup->event_count
        || record->code <= SOL_MIR_RUNTIME_FAILURE_NONE
        || record->code > SOL_MIR_RUNTIME_FAILURE_HOST_ERROR
        || (record->length && !record->bytes)) return false;
    const SolMirRuntimeCleanupEvent *event = &cleanup->events[event_id];
    const SolMirRuntimeCleanupTransition *transition = cleanup_transition_for_role(cleanup, event, role);
    if (!transition || !event->captures_failure_detail
        || transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE
        || transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING
        || record->detail_kind != cleanup_detail_kind(record->code)
        || !cleanup_detail_shape(record->detail_kind, record->code, record->length)
        || (transition->failure_mask & failbit(record->code)) == 0) return false;
    SolMirRuntimeSource source;
    if (transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31
        || transition->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING) {
        if (transition->failure_site >= cleanup->conventions->failure_site_count) return false;
        source = cleanup->conventions->failure_sites[transition->failure_site].source;
    } else {
        if (transition->failure_site >= cleanup->supplemental_site_count) return false;
        source = cleanup->supplemental_sites[transition->failure_site].source;
    }
    if (!cleanup_same_source(record->source, source)) return false;
    size_t length = record->length;
    if (record->detail_kind == SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT
        && length > SOL_MIR_RUNTIME_HOST_DETAIL_MAX) length = SOL_MIR_RUNTIME_HOST_DETAIL_MAX;
    /* HOST_BYTES is already bounded; oversize host payloads are malformed, not
     * silently truncated.  Panic is the one diagnostic prefix policy. */
    if (record->detail_kind == SOL_MIR_RUNTIME_FAILURE_DETAIL_HOST_BYTES
        && length > SOL_MIR_RUNTIME_HOST_DETAIL_MAX) return false;
    SolMirRuntimeCleanupDetail captured = {record->code, record->source,
        record->detail_kind, length, {0}};
    if (length) memcpy(captured.bytes, record->bytes, length);
    if (record->detail_kind == SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT)
        captured.bytes[length] = '\0';
    *out = captured;
    return true;
}
typedef struct{char*p;size_t n,c;bool bad;}Buffer;static void put(Buffer*b,const char*f,...){if(b->bad)return;va_list a;va_start(a,f);va_list q;va_copy(q,a);int n=vsnprintf(NULL,0,f,q);va_end(q);if(n<0||(size_t)n>SIZE_MAX-b->n-1){b->bad=true;va_end(a);return;}size_t z=b->n+(size_t)n+1;if(z>b->c){size_t c=b->c?b->c:256;while(c<z){if(c>SIZE_MAX/2){b->bad=true;va_end(a);return;}c*=2;}char*p=realloc(b->p,c);if(!p){b->bad=true;va_end(a);return;}b->p=p;b->c=c;}(void)vsnprintf(b->p+b->n,b->c-b->n,f,a);va_end(a);b->n+=(size_t)n;}
static void digest_u64(SolMirLinkageSha256*s,uint64_t x){uint8_t b[8];for(size_t i=0;i<sizeof b;i++)b[i]=(uint8_t)(x>>(56-8*i));sol_mir_linkage_internal_sha256_write(s,b,sizeof b);}
static void digest_action(SolMirLinkageSha256*s,const SolMirRuntimeCleanupAction*a){digest_u64(s,a->kind);digest_u64(s,a->flags);digest_u64(s,a->target);digest_u64(s,a->recipe);digest_u64(s,a->drop_path);}
static bool cleanup_digest(const SolMirRuntimeCleanup*c,size_t event,SolMirLinkageDigest*out){const SolMirRuntimeCleanupEvent*e=&c->events[event];SolMirLinkageSha256 s;sol_mir_linkage_internal_sha256_init(&s);digest_u64(&s,e->kind);digest_u64(&s,e->origin);digest_u64(&s,e->producer);digest_u64(&s,e->captures_failure_detail);digest_u64(&s,e->capture_detail_kind);digest_u64(&s,e->owner);digest_u64(&s,e->block);digest_u64(&s,e->operation);digest_u64(&s,e->source.file);digest_u64(&s,e->source.start);digest_u64(&s,e->source.end);digest_u64(&s,e->inherited_failure_site);for(size_t i=0;i<e->transitions.count;i++){const SolMirRuntimeCleanupTransition*t=&c->transitions[e->transitions.offset+i];digest_u64(&s,t->outcome);digest_u64(&s,t->continuation);digest_u64(&s,t->primary_failure_wins);digest_u64(&s,t->edge_role);digest_u64(&s,t->source_edge);digest_u64(&s,t->destination);digest_u64(&s,t->failure_source);digest_u64(&s,t->failure_site);digest_u64(&s,t->failure_mask);digest_u64(&s,t->contract_phase);digest_u64(&s,t->contract_outcome);for(size_t q=0;q<t->actions.count;q++){const SolMirRuntimeCleanupAction*a=&c->actions[t->actions.offset+q];digest_action(&s,a);if(a->drop_path!=SOL_MIR_RUNTIME_NONE){const SolMirRuntimeCleanupDropPath*p=&c->drop_paths[a->drop_path];digest_u64(&s,p->root);digest_u64(&s,p->place);digest_u64(&s,p->holes.offset);digest_u64(&s,p->holes.count);digest_u64(&s,p->recipe);digest_u64(&s,p->liveness);}}}if(e->supplemental_site!=SOL_MIR_RUNTIME_NONE){const SolMirRuntimeCleanupSupplementalSite*x=&c->supplemental_sites[e->supplemental_site];digest_u64(&s,x->source.file);digest_u64(&s,x->source.start);digest_u64(&s,x->source.end);digest_u64(&s,x->allowed_codes);}return sol_mir_linkage_internal_sha256_finish(&s,out);}
static void render_digest(Buffer*b,const SolMirLinkageDigest*d){for(size_t i=0;i<SOL_MIR_LINKAGE_DIGEST_BYTES;i++)put(b,"%02x",d->bytes[i]);}
static bool cleanup_action_digest(const SolMirRuntimeCleanup *c,
    const SolMirRuntimeCleanupAction *action, SolMirLinkageDigest *out) {
    SolMirLinkageSha256 state;
    sol_mir_linkage_internal_sha256_init(&state);
    digest_action(&state, action);
    if (action->drop_path != SOL_MIR_RUNTIME_NONE) {
        const SolMirRuntimeCleanupDropPath *path = &c->drop_paths[action->drop_path];
        digest_u64(&state, path->root); digest_u64(&state, path->place);
        digest_u64(&state, path->holes.offset); digest_u64(&state, path->holes.count);
        digest_u64(&state, path->recipe); digest_u64(&state, path->liveness);
    }
    return sol_mir_linkage_internal_sha256_finish(&state, out);
}
static const char *action_name(SolMirRuntimeCleanupActionKind x) { static const char *n[] = {"writeback","check-contract","drop-temporary","drop-place","exit-scope","exit-region","exit-handler","drop-snapshot","drop-parameter","propagate-failure"}; return x <= SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE ? n[x] : "invalid"; }
static const char *role_name(SolMirRuntimeCleanupEdgeRole x) { static const char *n[] = {"goto","branch-true","branch-false","call-normal","call-failure","refined-satisfied","refined-violation","refined-failure","propagate-value","propagate-residual","contract-satisfied","contract-violation","contract-failure","return","terminal-failure"}; return x <= SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE ? n[x] : "invalid"; }
static const char *failure_source_name(SolMirRuntimeCleanupFailureSource x) { static const char *n[] = {"none","inherited-p31","supplemental-p33","pending","local-or-pending"}; return x <= SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING ? n[x] : "invalid"; }
static const char *producer_name(SolMirRuntimeCleanupProducerKind x) { static const char *n[] = {"control","image-arithmetic","image-invoke","image-panic","image-no-match","image-unreachable","predicate-arithmetic","predicate-invoke","predicate-no-match","predicate-result","supplemental-allocation"}; return x <= SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION ? n[x] : "invalid"; }
static const char *target_class(SolMirRuntimeCleanupActionKind x) { switch (x) { case SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK: return "place"; case SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT: return "obligation"; case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY: return "temporary"; case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE: return "place"; case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_SCOPE: return "scope"; case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION: return "region"; case SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_HANDLER: return "handler"; case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT: return "snapshot"; case SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER: return "parameter"; case SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE: return "failure-site"; } return "invalid"; }
static bool keyed_u64(const char *domain, uint64_t value, SolMirLinkageDigest *out) { SolMirLinkageSha256 s; sol_mir_linkage_internal_sha256_init(&s); sol_mir_linkage_internal_sha256_write(&s, domain, strlen(domain)); digest_u64(&s, value); return sol_mir_linkage_internal_sha256_finish(&s, out); }
static bool keyed_source(SolMirRuntimeSource source, SolMirLinkageDigest *out) { SolMirLinkageSha256 s; sol_mir_linkage_internal_sha256_init(&s); sol_mir_linkage_internal_sha256_write(&s, "source", 6); digest_u64(&s, source.file); digest_u64(&s, source.start); digest_u64(&s, source.end); return sol_mir_linkage_internal_sha256_finish(&s, out); }
bool sol_mir_runtime_cleanup_render(FILE*stream,const SolMirRuntimeCleanup*c){if(!stream||!sol_mir_runtime_cleanup_validate(c,NULL))return false;Buffer b={0};put(&b,"mir_runtime_cleanup\ndeclaration.kind=cleanup-policy cleanup-policy=true cleanup-execution=false\n");for(size_t i=0;i<c->event_count;i++){const SolMirRuntimeCleanupEvent*event=&c->events[i];SolMirLinkageDigest key,source_key;if(!cleanup_digest(c,i,&key)||!keyed_source(event->source,&source_key)){b.bad=true;break;}put(&b,"event key=");render_digest(&b,&key);put(&b," producer=%s capture-detail=%s source-key=",producer_name(event->producer),event->captures_failure_detail?"before-cleanup":"none");render_digest(&b,&source_key);put(&b," failure-precedence=first\n");if(event->supplemental_site!=SOL_MIR_RUNTIME_NONE){const SolMirRuntimeCleanupSupplementalSite *site=&c->supplemental_sites[event->supplemental_site];SolMirLinkageDigest site_key,site_source;if(!keyed_u64("supplemental-site",event->supplemental_site,&site_key)||!keyed_source(site->source,&site_source)){b.bad=true;break;}put(&b,"site key=");render_digest(&b,&site_key);put(&b," origin=supplemental source-key=");render_digest(&b,&site_source);put(&b," code-mask=%08x\n",site->allowed_codes);}for(size_t j=0;j<event->transitions.count;j++){const SolMirRuntimeCleanupTransition*t=&c->transitions[event->transitions.offset+j];SolMirLinkageDigest edge_key,destination_key,site_key;if(!keyed_u64("edge",t->source_edge,&edge_key)||!keyed_u64("destination",t->destination,&destination_key)||!keyed_u64("failure-site",t->failure_site,&site_key)){b.bad=true;break;}put(&b,"transition event=");render_digest(&b,&key);put(&b," role=%s edge-key=",role_name(t->edge_role));render_digest(&b,&edge_key);put(&b," destination-key=");render_digest(&b,&destination_key);put(&b," failure-source=%s failure-site-key=",failure_source_name(t->failure_source));render_digest(&b,&site_key);put(&b," code-mask=%08x flags=%s\n",t->failure_mask,t->primary_failure_wins?"primary-wins":"none");for(size_t q=0;q<t->actions.count;q++){const SolMirRuntimeCleanupAction*a=&c->actions[t->actions.offset+q];SolMirLinkageDigest action_key,target_key,recipe_key,path_key;if(!cleanup_action_digest(c,a,&action_key)||!keyed_u64("target",a->target,&target_key)||!keyed_u64("recipe",a->recipe,&recipe_key)||!keyed_u64("path",a->drop_path,&path_key)){b.bad=true;break;}put(&b,"action key=");render_digest(&b,&action_key);put(&b," kind=%s target=%s:",action_name(a->kind),target_class(a->kind));render_digest(&b,&target_key);put(&b," recipe-key=");render_digest(&b,&recipe_key);put(&b," guard=%s path-slice-key=",(a->flags&SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED)?"conditional":"definite");render_digest(&b,&path_key);put(&b,"\n");}if(b.bad)break;}if(b.bad)break;}bool ok=!b.bad&&fwrite(b.p,1,b.n,stream)==b.n;free(b.p);return ok;}
#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_runtime_cleanup_test_predicate_propagate_schema(
    const SolMirPredicateTerminator *term, size_t edges[2],
    SolMirRuntimeCleanupEdgeRole roles[2]) {
    if (term == NULL || edges == NULL || roles == NULL
        || term->kind != SOL_MIR_PREDICATE_TERM_PROPAGATE
        || term->edge != SOL_MIR_OPERATION_NONE
        || term->normal_edge == SOL_MIR_OPERATION_NONE
        || term->failure_edge == SOL_MIR_OPERATION_NONE
        || term->normal_edge == term->failure_edge
        || predicate_edges(term, edges) != 2) return false;
    SolMirPredicateBlock block = {0};
    SolMirOperations operations = {0};
    SolMirRuntimeCleanupEvent event = {0};
    SolMirRuntimeCleanupTransition transition = {0};
    block.terminator = *term;
    operations.predicate_blocks = &block;
    operations.predicate_block_count = 1;
    event.kind = SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR;
    transition.continuation = edges[0];
    roles[0] = cleanup_role(&event, &transition, NULL, &operations);
    transition.continuation = edges[1];
    roles[1] = cleanup_role(&event, &transition, NULL, &operations);
    return roles[0] == SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_VALUE
        && roles[1] == SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_RESIDUAL;
}
bool sol_mir_runtime_cleanup_test_reconstruct_usage(const SolMirRuntimeConventions*c,const SolMirRuntimeValues*v,const SolMirRuntimeCleanupLimits*l,SolMirRuntimeCleanupUsage*u){if(!c||!v||v->conventions!=c||!l||!u||!complete(*l))return false;SolMirRuntimeCleanup x;sol_mir_runtime_cleanup_init(&x);x.conventions=c;x.values=v;x.limits=*l;size_t bytes; if(!scratch_bytes(&x,&bytes))return false; Workspace w={(unsigned char*)scratch_allocate(bytes),bytes,0};if(bytes&&!w.bytes)return false;Sink s={0};s.work=&s.count.build_work;bool ok=usage(&x,&s,&w)&&fit(&s.count,l);free(w.bytes);if(!ok)return false;*u=s.count;return true;}
static bool occurrence_detail_valid(const SolMirRuntimeCleanupFailureOccurrence *x) {
    if (x->detail_length > SOL_MIR_RUNTIME_HOST_DETAIL_MAX) return false;
    return x->detail_kind == cleanup_detail_kind(x->code)
        && cleanup_detail_shape(x->detail_kind, x->code, x->detail_length);
}
/* Authenticate an owned occurrence independently of its selected transition.
 * This is what makes a resumed PENDING occurrence safe to pass onward. */
static bool authenticate_occurrence(const SolMirRuntimeCleanup *c,
    const SolMirRuntimeCleanupFailureOccurrence *x) {
    uint32_t mask;
    SolMirRuntimeSource source;
    if (!x || x->code <= SOL_MIR_RUNTIME_FAILURE_NONE
        || x->code > SOL_MIR_RUNTIME_FAILURE_HOST_ERROR || !occurrence_detail_valid(x)) return false;
    if (x->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31) {
        if (x->site >= c->conventions->failure_site_count) return false;
        mask = c->conventions->failure_sites[x->site].allowed_codes;
        source = c->conventions->failure_sites[x->site].source;
    } else if (x->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33) {
        if (x->site >= c->supplemental_site_count) return false;
        mask = c->supplemental_sites[x->site].allowed_codes;
        source = c->supplemental_sites[x->site].source;
    } else return false;
    return (mask & failbit(x->code)) != 0 && cleanup_same_source(x->source, source);
}
static bool occurrence_for_transition(const SolMirRuntimeCleanup *c,
    const SolMirRuntimeCleanupTransition *t,
    const SolMirRuntimeCleanupFailureOccurrence *x) {
    if (!authenticate_occurrence(c, x)) return false;
    return x->failure_source == t->failure_source && x->site == t->failure_site
        && (t->failure_mask & failbit(x->code)) != 0;
}
static bool occurrence_for_event_transition(const SolMirRuntimeCleanup *c,
    const SolMirRuntimeCleanupEvent *event, const SolMirRuntimeCleanupTransition *transition,
    const SolMirRuntimeCleanupFailureOccurrence *occurrence) {
    return occurrence_for_transition(c, transition, occurrence)
        && event->captures_failure_detail
        && occurrence->detail_kind == cleanup_detail_kind(occurrence->code)
        && cleanup_detail_shape(occurrence->detail_kind, occurrence->code,
            occurrence->detail_length);
}
bool sol_mir_runtime_cleanup_test_select(const SolMirRuntimeCleanup*c,
    const SolMirRuntimeCleanupTraceRequest *request, SolMirRuntimeCleanupAction *storage,
    size_t count, SolMirRuntimeCleanupTrace *trace) {
    if (!sol_mir_runtime_cleanup_validate(c, NULL) || !request || !trace
        || request->event >= c->event_count || (count && !storage)
        || request->liveness > SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL) return false;
    const SolMirRuntimeCleanupEvent *event = &c->events[request->event];
    const SolMirRuntimeCleanupTransition *selected = NULL;
    for (size_t i = 0; i < event->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate = &c->transitions[event->transitions.offset + i];
        if (candidate->edge_role != request->edge_role) continue;
        if (selected != NULL) return false;
        selected = candidate;
    }
    if (!selected || (storage != NULL && selected->actions.count > count)) return false;
    const SolMirRuntimeCleanupFailureOccurrence *primary = NULL;
    if (selected->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING) {
        if (request->produced || !request->pending || !authenticate_occurrence(c, request->pending)) return false;
        primary = request->pending;
    } else if (selected->failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING) {
        if ((request->produced == NULL) == (request->pending == NULL)) return false;
        if (request->produced) {
            if (!occurrence_for_event_transition(c, event, &(SolMirRuntimeCleanupTransition){
                    .failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31,
                    .failure_site = selected->failure_site, .failure_mask = selected->failure_mask},
                    request->produced)) return false;
            primary = request->produced;
        } else {
            if (!authenticate_occurrence(c, request->pending)) return false;
            primary = request->pending;
        }
    } else if (selected->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE) {
        if (request->pending || !request->produced
            || !occurrence_for_event_transition(c, event, selected, request->produced)) return false;
        primary = request->produced;
    } else if (request->produced || request->pending) return false;
    if (storage != NULL) for (size_t i = 0; i < selected->actions.count; ++i)
        storage[i] = c->actions[selected->actions.offset + i];
    SolMirRuntimeCleanupTrace out = {0};
    out.event = request->event; out.edge_role = request->edge_role; out.outcome = selected->outcome;
    out.liveness = request->liveness; out.has_primary = primary != NULL;
    if (primary) memcpy(&out.primary, primary, sizeof(out.primary));
    out.action_count = selected->actions.count; out.actions = storage;
    *trace = out;
    return true;
}
bool sol_mir_runtime_cleanup_test_trace(const SolMirRuntimeCleanup*c,
    const SolMirRuntimeCleanupTraceRequest *request, SolMirRuntimeCleanupAction *storage,
    size_t count, SolMirRuntimeCleanupTrace *trace) {
    return sol_mir_runtime_cleanup_test_select(c, request, storage, count, trace);
}
/* The precedence hook must not treat canonical event order as execution order.
 * Authenticate the selected primary edge against the materialized CFG before a
 * pending resume is allowed to carry its identity onward. */
static bool precedence_path_reaches(const SolMirMaterialization *m, size_t block,
    size_t target, size_t remaining) {
    if (block == target) return true;
    if (remaining == 0 || block >= m->block_count) return false;
    size_t outgoing[3], count = edges(&m->blocks[block].terminator, outgoing);
    for (size_t i = 0; i < count; ++i)
        if (outgoing[i] < m->edge_count
            && precedence_path_reaches(m, m->edges[outgoing[i]].block, target, remaining - 1))
            return true;
    return false;
}
/* This authentication is deliberately allocation-free: a bounded-depth graph
 * walk proves a simple CFG path to RESUME_FAILURE without inventing a pending
 * occurrence or allocating a visited bitmap. */
static bool precedence_reaches_pending(const SolMirRuntimeCleanup *c,
    const SolMirRuntimeCleanupPrecedenceInput *first,
    const SolMirRuntimeCleanupPrecedenceResume *resume) {
    const SolMirRuntimeCleanupEvent *origin = &c->events[first->event];
    const SolMirRuntimeCleanupEvent *target = &c->events[resume->event];
    const SolMirMaterialization *m = &c->conventions->concrete->materialization;
    if (first->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE
        || resume->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE
        || origin->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || target->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || origin->owner != target->owner || origin->block >= m->block_count
        || target->block >= m->block_count) return false;
    const SolMirRuntimeCleanupTransition *selected = NULL;
    for (size_t i = 0; i < origin->transitions.count; ++i) {
        const SolMirRuntimeCleanupTransition *candidate =
            &c->transitions[origin->transitions.offset + i];
        if (candidate->edge_role == first->edge_role) {
            if (selected != NULL) return false;
            selected = candidate;
        }
    }
    if (selected == NULL || selected->continuation == SOL_MIR_RUNTIME_NONE
        || selected->continuation >= m->edge_count) return false;
    if (m->blocks[target->block].terminator.kind != SOL_MIR_TERM_RESUME_FAILURE) return false;
    return precedence_path_reaches(m, m->edges[selected->continuation].block,
        target->block, m->block_count);
}

/* This test-only traversal deliberately has no allocator fallback.  Its
 * caller supplies a visited bitmap and FIFO, both bounded by the authenticated
 * materialized graph.  RESUME_FAILURE is a terminal for this proof: its
 * outgoing graph (if any is ever introduced) cannot make a normal contract
 * appear to be part of primary-failure cleanup. */
static bool precedence_path_before_resume(const SolMirMaterialization *m,
    const SolMirMaterializedImage *image, size_t start, size_t target,
    size_t *scratch, size_t scratch_count, bool *reaches_target,
    bool *reaches_resume) {
    if (!m || !image || !scratch || !reaches_target || !reaches_resume
        || m->block_count > SIZE_MAX / 2
        || m->block_count > SIZE_MAX / sizeof(*scratch)
        || scratch_count < m->block_count * 2
        || start >= m->block_count || target >= m->block_count
        || start < image->blocks.offset || start - image->blocks.offset >= image->blocks.count
        || target < image->blocks.offset || target - image->blocks.offset >= image->blocks.count)
        return false;
    size_t *seen = scratch, *queue = scratch + m->block_count;
    memset(seen, 0, m->block_count * sizeof(*seen));
    size_t head = 0, tail = 0;
    seen[start] = 1; queue[tail++] = start;
    *reaches_target = false; *reaches_resume = false;
    while (head < tail) {
        size_t block = queue[head++];
        if (block == target) *reaches_target = true;
        const SolMirMaterializedTerminator *term = &m->blocks[block].terminator;
        if (term->kind == SOL_MIR_TERM_RESUME_FAILURE) {
            *reaches_resume = true;
            continue;
        }
        size_t outgoing[3], count = edges(term, outgoing);
        for (size_t i = 0; i < count; ++i) {
            if (outgoing[i] >= m->edge_count) return false;
            size_t next = m->edges[outgoing[i]].block;
            if (next < image->blocks.offset || next - image->blocks.offset >= image->blocks.count)
                return false;
            if (!seen[next]) {
                if (tail >= m->block_count) return false;
                seen[next] = 1; queue[tail++] = next;
            }
        }
    }
    return true;
}

static bool precedence_contract_context(const SolMirRuntimeCleanup *c,
    const SolMirRuntimeCleanupEvent *origin, const SolMirRuntimeCleanupEvent *attempt,
    const SolMirRuntimeCleanupTransition *later_transition) {
    const SolMirMaterialization *m = &c->conventions->concrete->materialization;
    const SolMirOperations *ops = &c->conventions->concrete->operations;
    if (origin->owner >= m->image_count || attempt->owner != origin->owner
        || attempt->owner >= m->image_count || origin->block >= m->block_count
        || attempt->block >= m->block_count
        || m->images[origin->owner].source_callable
            != m->images[attempt->owner].source_callable
        || m->blocks[attempt->block].terminator.kind != SOL_MIR_TERM_CHECK_CONTRACT
        || later_transition->continuation
            != m->blocks[attempt->block].terminator.violation_edge) return false;
    const SolMirMaterializedImage *image = &m->images[origin->owner];
    size_t matches = 0;
    for (size_t i = 0; i < ops->predicate_count; ++i) {
        const SolMirOperationPredicatePlan *plan = &ops->predicates[i];
        if (plan->image != origin->owner || plan->block != attempt->block) continue;
        ++matches;
        if (plan->kind != SOL_MIR_OPERATION_PREDICATE_CONTRACT
            || plan->contract_phase != SOL_CONTRACT_ENSURES
            || plan->contract_outcome != SOL_CONTRACT_OUTCOME_FAILURE
            || plan->context < image->contexts.offset
            || plan->context - image->contexts.offset >= image->contexts.count
            || plan->context >= m->context_count
            || m->contexts[plan->context].obligation
                != m->blocks[attempt->block].terminator.source_obligation)
            return false;
    }
    return matches == 1;
}
bool sol_mir_runtime_cleanup_test_precedence(const SolMirRuntimeCleanup *c,
    const SolMirRuntimeCleanupPrecedenceInput *first,
    const SolMirRuntimeCleanupPrecedenceResume *resume,
    SolMirRuntimeCleanupFailureOccurrence *primary) {
    SolMirRuntimeCleanupTrace first_trace, resume_trace;
    SolMirRuntimeCleanupTraceRequest first_request, resume_request;
    if (!c || !first || !primary) return false;
    if (first->occurrence.failure_source == SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING)
        return false;
    first_request = (SolMirRuntimeCleanupTraceRequest){first->event, first->edge_role,
        &first->occurrence, NULL, first->liveness};
    if (!sol_mir_runtime_cleanup_test_select(c, &first_request, NULL, 0, &first_trace)
        || !first_trace.has_primary) return false;
    if (resume != NULL) {
        /* RESUME_FAILURE is transport only: it receives the authenticated
         * producer occurrence byte-for-byte and has no second occurrence. */
        resume_request = (SolMirRuntimeCleanupTraceRequest){resume->event, resume->edge_role,
            NULL, &first_trace.primary,
            resume->liveness};
        if (!sol_mir_runtime_cleanup_test_select(c, &resume_request, NULL, 0, &resume_trace)
            || !resume_trace.has_primary
            || memcmp(&first_trace.primary, &resume_trace.primary, sizeof(*primary)) != 0) return false;
        const SolMirRuntimeCleanupEvent *event = &c->events[resume->event];
        const SolMirRuntimeCleanupTransition *selected = NULL;
        for (size_t i = 0; i < event->transitions.count; ++i) {
            const SolMirRuntimeCleanupTransition *candidate =
                &c->transitions[event->transitions.offset + i];
            if (candidate->edge_role == resume->edge_role) selected = candidate;
        }
        /* A pending RESUME_FAILURE is transport, not a second producer. */
        if (selected == NULL || !selected->primary_failure_wins
            || selected->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING
            || !precedence_reaches_pending(c, first, resume)) return false;
    }
    memcpy(primary, &first_trace.primary, sizeof(*primary));
    return true;
}

bool sol_mir_runtime_cleanup_test_precedence_contract_later(
    const SolMirRuntimeCleanup *c, const SolMirRuntimeCleanupPrecedenceInput *first,
    const SolMirRuntimeCleanupAttemptedLater *later,
    size_t *reachability_scratch, size_t reachability_scratch_count,
    SolMirRuntimeCleanupPrecedenceResult *result) {
    SolMirRuntimeCleanupFailureOccurrence primary;
    SolMirRuntimeCleanupTrace trace;
    SolMirRuntimeCleanupTraceRequest request;
    if (!c || !first || !later || !result
        || !sol_mir_runtime_cleanup_test_precedence(c, first, NULL, &primary)) return false;
    request = (SolMirRuntimeCleanupTraceRequest){later->event, later->edge_role,
        &later->occurrence, NULL, later->liveness};
    if (!sol_mir_runtime_cleanup_test_select(c, &request, NULL, 0, &trace)
        || !trace.has_primary || first->event >= c->event_count
        || later->event >= c->event_count) return false;
    const SolMirRuntimeCleanupEvent *origin = &c->events[first->event];
    const SolMirRuntimeCleanupEvent *attempt = &c->events[later->event];
    const SolMirMaterialization *m = &c->conventions->concrete->materialization;
    if (origin->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || attempt->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || origin->block >= m->block_count || attempt->block >= m->block_count
        || m->blocks[origin->block].terminator.kind != SOL_MIR_TERM_INVOKE) return false;
    const SolMirMaterializedTerminator *primary_term = &m->blocks[origin->block].terminator;
    const SolMirRuntimeCleanupTransition *normal = cleanup_transition_for_role(c, origin,
        SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL);
    const SolMirRuntimeCleanupTransition *failure = cleanup_transition_for_role(c, origin,
        SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE);
    if (!normal || !failure || first->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE
        || normal->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
        || failure->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
        || normal->continuation != primary_term->normal_edge
        || failure->continuation != primary_term->failure_edge
        || normal->continuation == SOL_MIR_RUNTIME_NONE
        || failure->continuation == SOL_MIR_RUNTIME_NONE
        || normal->continuation >= m->edge_count || failure->continuation >= m->edge_count)
        return false;
    const SolMirRuntimeCleanupTransition *later_transition = cleanup_transition_for_role(c,
        attempt, later->edge_role);
    if (!later_transition
        || later_transition->edge_role != SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION
        || later_transition->contract_phase != SOL_CONTRACT_ENSURES
        || later_transition->contract_outcome != SOL_CONTRACT_OUTCOME_FAILURE
        || !occurrence_for_event_transition(c, attempt, later_transition, &later->occurrence))
        return false;
    if (!precedence_contract_context(c, origin, attempt, later_transition)) return false;
    const SolMirMaterializedImage *image = &m->images[origin->owner];
    bool normal_reaches_contract, ignored_resume, failure_reaches_contract, failure_reaches_resume;
    if (!precedence_path_before_resume(m, image, m->edges[normal->continuation].block,
            attempt->block, reachability_scratch, reachability_scratch_count,
            &normal_reaches_contract, &ignored_resume)
        || !normal_reaches_contract
        || !precedence_path_before_resume(m, image, m->edges[failure->continuation].block,
            attempt->block, reachability_scratch, reachability_scratch_count,
            &failure_reaches_contract, &failure_reaches_resume)
        || failure_reaches_contract || !failure_reaches_resume) return false;
    *result = (SolMirRuntimeCleanupPrecedenceResult){
        .has_primary = true, .primary = primary,
        .later_status = SOL_MIR_RUNTIME_CLEANUP_LATER_SUPPRESSED_BY_PRIMARY,
        .writeback_attempted = false, .ensures_attempted = false,
        .has_secondary = false,
    };
    return true;
}
bool sol_mir_runtime_cleanup_test_precedence_attempted_later(
    const SolMirRuntimeCleanup *c, const SolMirRuntimeCleanupPrecedenceInput *first,
    const SolMirRuntimeCleanupAttemptedLater *later,
    size_t *reachability_scratch, size_t reachability_scratch_count) {
    SolMirRuntimeCleanupPrecedenceResult result;
    return sol_mir_runtime_cleanup_test_precedence_contract_later(c, first, later,
        reachability_scratch, reachability_scratch_count, &result)
        && result.later_status == SOL_MIR_RUNTIME_CLEANUP_LATER_SUPPRESSED_BY_PRIMARY;
}
#endif
bool sol_mir_runtime_cleanup_internal_reconstruct(const SolMirRuntimeConventions*c,const SolMirRuntimeValues*v,const SolMirRuntimeCleanupLimits*l,SolMirRuntimeCleanupUsage*u){if(!c||!v||!l||!u||v->conventions!=c||!complete(*l))return false;SolMirRuntimeCleanup x;sol_mir_runtime_cleanup_init(&x);x.conventions=c;x.values=v;x.limits=*l;size_t bytes;if(!scratch_bytes(&x,&bytes))return false;Workspace w={(unsigned char*)scratch_allocate(bytes),bytes,0};if(bytes&&!w.bytes)return false;Sink s={0};s.work=&s.count.build_work;bool ok=usage(&x,&s,&w);free(w.bytes);if(!ok)return false;*u=s.count;return fit(u,l);}
