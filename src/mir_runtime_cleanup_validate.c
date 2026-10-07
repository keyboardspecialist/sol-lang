#include "sol/mir_runtime_cleanup.h"
#include "mir_runtime_cleanup_internal.h"
#include "mir_runtime_arena_internal.h"

#include <stdint.h>
#include <stdio.h>
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
static _Thread_local size_t *alias_meter_work;
static _Thread_local size_t alias_meter_limit;
static _Thread_local bool alias_meter_exhausted;
static bool alias_meter_tick(void) {
    if (alias_meter_work == NULL) return true;
    if (*alias_meter_work == SIZE_MAX || *alias_meter_work >= alias_meter_limit) {
        alias_meter_exhausted=true; return false;
    }
    ++*alias_meter_work; return true;
}

/* This file deliberately has its own CFG replay.  Do not call construction or
 * its usage reconstruction here: validation must detect a valid-shaped but
 * incorrectly emitted cleanup owner. */


static bool validation_bad(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL)
        sol_diagnostics_add(diagnostics, "SOL-MIR-RUNTIME-CLEANUP-002",
            SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool validation_mul(size_t a, size_t b, size_t *result) {
    if (a != 0 && b > SIZE_MAX / a) return false;
    *result = a * b;
    return true;
}

static bool validation_range(const void *pointer, size_t count, size_t size) {
    size_t bytes;
    return count == 0 ? pointer == NULL
        : pointer != NULL && validation_mul(count, size, &bytes)
        && (uintptr_t)pointer <= UINTPTR_MAX - bytes;
}

static bool validation_overlaps(const void *a, size_t ac, size_t as,
    const void *b, size_t bc, size_t bs) {
    size_t ab, bb;
    if (ac == 0 || bc == 0) return false;
    if (!validation_mul(ac, as, &ab) || !validation_mul(bc, bs, &bb)) return true;
    uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
    if (ap > UINTPTR_MAX - ab || bp > UINTPTR_MAX - bb) return true;
    if (!alias_meter_tick() || !alias_meter_tick()) return true;
    bool left=ap < bp + bb, right=bp < ap + ab;
    return left && right;
}

static bool validation_full_limits(SolMirRuntimeCleanupLimits limits) {
    return limits.max_events && limits.max_actions && limits.max_transitions
        && limits.max_supplemental_sites && limits.max_drop_paths && limits.max_owned_bytes
        && limits.max_build_scratch_bytes && limits.max_build_work
        && limits.max_validation_scratch_bytes && limits.max_validation_work;
}

static bool validation_add(size_t *value, size_t addend) {
    if (addend > SIZE_MAX - *value) return false;
    *value += addend;
    return true;
}


typedef struct { const void *pointer; size_t count, size; } ValidationRange;

/* Predecessor validators establish their internal raw ranges before this walks
 * the complete borrowed graph.  Every cleanup arena is compared with every
 * owned predecessor arena, including all transitive concrete stages. */
typedef struct { const ValidationRange *owned; size_t count; bool measurement; } CleanupAliasContext;
static _Thread_local bool cleanup_alias_measurement;
static SolMirRuntimeTextGuardResult cleanup_text_guard(uintptr_t address,
    uintptr_t *boundary, void *opaque) {
    const CleanupAliasContext *x=opaque; uintptr_t next=UINTPTR_MAX; bool overlap=false;
    for(size_t i=0;i<x->count;i++) { size_t bytes;
        if(!alias_meter_tick()||!validation_mul(x->owned[i].count,x->owned[i].size,&bytes)) return SOL_MIR_RUNTIME_TEXT_EXHAUSTED;
        if(!x->owned[i].count) continue;
        uintptr_t start=(uintptr_t)x->owned[i].pointer,end=start+bytes;
        if(!x->measurement) { overlap |= address>=start && address<end;
            if(start>address&&start<next) next=start; }
    }
    if(overlap)return SOL_MIR_RUNTIME_TEXT_OVERLAP;
    *boundary=x->measurement ? UINTPTR_MAX : next;return SOL_MIR_RUNTIME_TEXT_SAFE;
}
static bool validation_against_cleanup(const void *pointer,size_t count,size_t size,
    void *context) {
    const CleanupAliasContext *x=context;
    if (!alias_meter_tick()) return false;
    if(pointer==NULL&&count==0&&size==0)return true;
    if (!sol_mir_runtime_arena_range(pointer,count,size)) return false;
    for (size_t i=0;i<x->count;++i) {
        if (!sol_mir_runtime_arena_range(x->owned[i].pointer,x->owned[i].count,
                x->owned[i].size)) return false;
        bool overlap=validation_overlaps(x->owned[i].pointer,x->owned[i].count,
            x->owned[i].size,pointer,count,size);
        if (!x->measurement && overlap) return false;
        if (alias_meter_exhausted) return false;
    }
    return true;
}

/* Use raw capacities throughout: the local owner was range-authenticated before
 * this scan, and its predecessor headers are authenticated before typed local
 * record traversal begins. */
static bool validation_predecessor_alias(const SolMirRuntimeCleanup *cleanup) {
    ValidationRange owned[] = {
        {cleanup->events, cleanup->event_capacity, sizeof(*cleanup->events)},
        {cleanup->actions, cleanup->action_capacity, sizeof(*cleanup->actions)},
        {cleanup->transitions, cleanup->transition_capacity, sizeof(*cleanup->transitions)},
        {cleanup->supplemental_sites, cleanup->supplemental_site_capacity, sizeof(*cleanup->supplemental_sites)},
        {cleanup->drop_paths, cleanup->drop_path_capacity, sizeof(*cleanup->drop_paths)},
    };
    CleanupAliasContext context={owned,sizeof(owned)/sizeof(*owned),cleanup_alias_measurement};
    SolMirRuntimeTextGuard saved_guard=sol_mir_runtime_text_guard;
    void *saved_context=sol_mir_runtime_text_guard_context;
    sol_mir_runtime_text_guard=cleanup_text_guard;
    sol_mir_runtime_text_guard_context=&context;
#define CLEANUP_ALIAS_RETURN(value) do { sol_mir_runtime_text_guard=saved_guard; sol_mir_runtime_text_guard_context=saved_context; return (value); } while (0)
    for (size_t i=0;i<context.count;++i) {
        if (!alias_meter_tick()) CLEANUP_ALIAS_RETURN(true);
        for (size_t j=i+1;j<context.count;++j) {
            if (!alias_meter_tick()) CLEANUP_ALIAS_RETURN(true);
            if (!validation_against_cleanup(owned[j].pointer,owned[j].count,
                    owned[j].size,&(CleanupAliasContext){&owned[i],1,context.measurement})) CLEANUP_ALIAS_RETURN(true);
        }
    }
#define AGAINST(pointer, count, type) \
    if (!validation_against_cleanup((pointer),(count),sizeof(type),&context)) CLEANUP_ALIAS_RETURN(true)
    const SolMirRuntimeConventions *runtime=cleanup->conventions;
    const SolMirRuntimeValues *values=cleanup->values;
    AGAINST(runtime,1,SolMirRuntimeConventions); AGAINST(values,1,SolMirRuntimeValues);
#define RUNTIME(member,type,singular) AGAINST(runtime->member,runtime->singular##_capacity,type);
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(RUNTIME)
#undef RUNTIME
    AGAINST(values->recipe_operations,values->recipe_operation_capacity,SolMirRuntimeRecipeOperations);
    AGAINST(values->allocation_plans,values->allocation_plan_capacity,SolMirRuntimeAllocationPlan);
    AGAINST(values->copy_plans,values->copy_plan_capacity,SolMirRuntimeCopyPlan);
    AGAINST(values->equality_plans,values->equality_plan_capacity,SolMirRuntimeEqualityPlan);
    AGAINST(values->host_result_plans,values->host_result_plan_capacity,SolMirRuntimeHostResultPlan);
    AGAINST(values->host_result_requirements,values->host_result_requirement_capacity,SolMirRuntimeHostResultRequirement);
    AGAINST(values->ownership_plans,values->ownership_plan_capacity,SolMirRuntimeOwnershipPlan);
    AGAINST(values->ownership_variants,values->ownership_variant_capacity,SolMirRuntimeOwnershipVariant);
    AGAINST(values->owned_edges,values->owned_edge_capacity,SolMirRuntimeOwnedEdge);
    if (sol_mir_runtime_visit_concrete_arenas(runtime->concrete,
            validation_against_cleanup,&context) != SOL_MIR_RUNTIME_ARENA_VISIT_OK) CLEANUP_ALIAS_RETURN(true);
#undef AGAINST
    CLEANUP_ALIAS_RETURN(false);
#undef CLEANUP_ALIAS_RETURN
}

bool sol_mir_runtime_cleanup_internal_alias_work(const SolMirRuntimeCleanup *cleanup,
    size_t *out) {
    if (!cleanup || !out) return false;
    size_t work=0;
    alias_meter_work=&work; alias_meter_limit=SIZE_MAX; alias_meter_exhausted=false;
    cleanup_alias_measurement=true;
    bool invalid=validation_predecessor_alias(cleanup);
    cleanup_alias_measurement=false;
    alias_meter_work=NULL;
    if (invalid || alias_meter_exhausted) return false;
    *out=work; return true;
}
#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_runtime_cleanup_test_alias_work(const SolMirRuntimeCleanup *cleanup,
    size_t *out) { return sol_mir_runtime_cleanup_internal_alias_work(cleanup,out); }
#endif

typedef enum { DEAD, UNINITIALIZED, INITIALIZED, MAYBE } IndependentStorage;
typedef enum { INDEPENDENT_HOLE_ABSENT, INDEPENDENT_HOLE_MUST, INDEPENDENT_HOLE_MAY } IndependentHole;
typedef struct {
    const SolMirMaterialization *m; const SolMirMaterializedImage *image;
    size_t image_id, blocks, locals, places, width;
    IndependentStorage *storage; unsigned char *holes;
    size_t *temps, *temp_scopes, *snaps, *scopes, *regions, *handlers;
    size_t *td, *sd, *cd, *rd, *hd; unsigned char *known;
} IndependentReplay;
typedef struct {
    const SolMirRuntimeCleanup *owner;
    SolMirRuntimeCleanupUsage count;
    size_t event_index, action_index, transition_index, supplemental_index;
    SolMirRuntimeCleanupEvent current_event;
    size_t *work;
    /* The builder's first two logical phases count a drop action and its
     * selected-hole search, but do not enumerate the emitted hole records.
     * Its write phase does.  Keep that distinction here so build_work is a
     * validator-owned reconstruction of all three phases. */
    bool have_event, failed, check_drop_paths, step_pass;
} IndependentSink;
typedef struct { unsigned char *bytes; size_t capacity, used; } ValidationWorkspace;


static bool add(size_t *a, size_t b) { return validation_add(a, b); }
static bool meter_work(size_t *work) {
    if (work == NULL) return true;
    return work == scan_meter_work ? METER() : add(work, 1);
}
static bool mul(size_t a, size_t b, size_t *r) { return validation_mul(a, b, r); }

static void *workspace_take(ValidationWorkspace *w,size_t count,size_t size) {
    size_t bytes,at,align=_Alignof(max_align_t);
    if(!w||!mul(count,size,&bytes)||w->used>SIZE_MAX-(align-1))return NULL;
    at=(w->used+align-1)&~(align-1);
    if(at>w->capacity||bytes>w->capacity-at)return NULL;
    void *p=w->bytes+at;w->used=at+bytes;return p;
}
static bool source_for(const SolIr *ir,SolSpan span,SolMirRuntimeSource *out) { bool one=false;if(span.start>span.end)return false;for(size_t i=0;i<ir->file_count;i++){const SolIrSourceFile *f=&ir->files[i];if(span.start<f->aggregate_start||span.end>f->aggregate_end)continue;if(one)return false;*out=(SolMirRuntimeSource){i,span.start-f->aggregate_start,span.end-f->aggregate_start};one=true;}return one; }
static uint32_t failbit(SolMirRuntimeFailureCode c) { return c ? UINT32_C(1)<<((unsigned)c-1) : 0; }
static SolMirRuntimeFailureSiteId inherited(const SolMirRuntimeConventions *c,size_t owner,size_t block,size_t op) { for(size_t i=0;i<c->failure_site_count;i++){if(!METER())return SOL_MIR_RUNTIME_NONE;const SolMirRuntimeFailureSite *s=&c->failure_sites[i];if(s->origin_kind!=SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP&&s->origin_kind!=SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP&&s->owner==owner&&s->block==block&&s->instruction==op)return i;}return SOL_MIR_RUNTIME_NONE; }
static SolMirRuntimeFailureSiteId step_site(const SolMirRuntimeConventions*c,
    SolMirRuntimeFailureOriginKind origin,size_t owner,size_t block,size_t op){
    size_t found=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<c->failure_site_count;i++){
        if(!METER())return SOL_MIR_RUNTIME_NONE;
        const SolMirRuntimeFailureSite*s=&c->failure_sites[i];
        if(s->origin_kind!=origin||s->owner!=owner||s->block!=block||s->instruction!=op)continue;
        if(found!=SOL_MIR_RUNTIME_NONE||s->allowed_codes!=failbit(SOL_MIR_RUNTIME_FAILURE_STEP_LIMIT))return SOL_MIR_RUNTIME_NONE;
        found=i;
    }
    return found;
}
/* Keep the independent replay aligned with construction: a qualified
 * predicate-result site crosses owner arenas only at epilogue GOTO. */
static SolMirRuntimeFailureSiteId control_inherited(const SolMirRuntimeConventions *c,
    const SolMirMaterialization *m, const SolMirMaterializedImage *image, size_t owner,
    size_t block, const SolMirMaterializedTerminator *term) {
    SolMirRuntimeFailureSiteId site = inherited(c, owner, block, SOL_MIR_RUNTIME_NONE);
    if (site >= c->failure_site_count
        || c->failure_sites[site].origin_kind != SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT)
        return site;
    const SolMirOperations *operations = &c->concrete->operations;
    size_t body = c->failure_sites[site].owner;
    if (body >= operations->predicate_body_count
        || (operations->predicate_bodies[body].outcome != SOL_CONTRACT_OUTCOME_SUCCESS
            && operations->predicate_bodies[body].outcome != SOL_CONTRACT_OUTCOME_FAILURE))
        return site;
    return (term->kind == SOL_MIR_TERM_GOTO || term->kind == SOL_MIR_TERM_BREAK
            || term->kind == SOL_MIR_TERM_CONTINUE)
            && term->edge < m->edge_count && m->edges[term->edge].block == image->contract_epilogue
        ? site : SOL_MIR_RUNTIME_NONE;
}
static bool is_root(const SolMirMaterialization *m,size_t place,size_t local) { return place<m->place_count&&m->places[place].local==local&&!m->places[place].projections.count; }
static size_t root_place(const IndependentReplay *r,size_t local) { for(size_t p=0;p<r->places;p++){if(!METER())return SOL_MIR_RUNTIME_NONE;size_t x=r->image->places.offset+p;if(is_root(r->m,x,local))return x;}return SOL_MIR_RUNTIME_NONE; }
static SolMirRuntimeCleanupActionKind drop_kind(const SolMirMaterializedLocal *local) {
    return local->kind == SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
            || local->kind == SOL_MIR_MATERIALIZED_LOCAL_RECEIVER
        ? SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER
        : SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE;
}
static size_t snapshot_plan(const SolMirOperations *ops, size_t image, size_t instruction) {
    for (size_t i = 0; i < ops->snapshot_count; ++i) {
        if (!METER()) return SOL_MIR_RUNTIME_NONE;
        if (ops->snapshots[i].image == image && ops->snapshots[i].instruction == instruction)
            return i;
    }
    return SOL_MIR_RUNTIME_NONE;
}
static bool has_hole(const IndependentReplay *r,const unsigned char *holes,size_t local) { for(size_t p=0;p<r->places;p++){if(!METER())return false;if(r->m->places[r->image->places.offset+p].local==local&&holes[p])return true;}return false; }
static void clear_local(const IndependentReplay *r,unsigned char *holes,size_t local) { for(size_t p=0;p<r->places;p++){if(!METER())return;if(r->m->places[r->image->places.offset+p].local==local)holes[p]=0;} }
static bool prefix(const SolMirMaterialization *m,size_t a,size_t b) { const SolMirMaterializedPlace *x=&m->places[a],*y=&m->places[b];if(x->local!=y->local||x->projections.count>y->projections.count)return false;for(size_t i=0;i<x->projections.count;i++){if(!METER())return false;const SolMirMaterializedProjection *p=&m->projections[x->projections.offset+i],*q=&m->projections[y->projections.offset+i];if(p->kind!=q->kind||p->source_field!=q->source_field||p->tuple_ordinal!=q->tuple_ordinal)return false;}return true; }
static void clear_covered(const IndependentReplay *r,unsigned char *holes,size_t place) { for(size_t p=0;p<r->places;p++){if(!METER())return;if(prefix(r->m,place,r->image->places.offset+p))holes[p]=0;} }
static bool stack_equal(const size_t *a,size_t an,const size_t *b,size_t bn) { return an==bn&&(!an||!memcmp(a,b,an*sizeof *a)); }
static bool merge_storage(IndependentStorage *to,const IndependentStorage *from,size_t n,bool *changed){for(size_t i=0;i<n;i++){IndependentStorage x=to[i];if(x!=from[i]){if(x==DEAD||from[i]==DEAD)return false;x=MAYBE;}if(x!=to[i]){to[i]=x;*changed=true;}}return true;}
static size_t stack_width(const IndependentReplay *r) { size_t w=r->image->temporaries.count; if(r->image->instructions.count>w)w=r->image->instructions.count; if(r->image->handlers.count>w)w=r->image->handlers.count; if(!w)w=1; return w; }
static void replay_free(IndependentReplay *r) { memset(r,0,sizeof *r); }
static bool replay_init(IndependentReplay *r,const SolMirMaterialization *m,size_t image,ValidationWorkspace*w) { memset(r,0,sizeof *r);r->m=m;r->image=&m->images[image];r->image_id=image;r->blocks=r->image->blocks.count;r->locals=r->image->locals.count;r->places=r->image->places.count;r->width=stack_width(r);size_t n;
#define ALLOC(member,count,type) do { if((count)&&((member)=workspace_take(w,(count),sizeof(type)))==NULL)return false; } while(0)
 if(!mul(r->blocks,r->locals,&n))return false;ALLOC(r->storage,n,IndependentStorage);if(!mul(r->blocks,r->places,&n))return false;ALLOC(r->holes,n,unsigned char);if(!mul(r->blocks,r->width,&n))return false;ALLOC(r->temps,n,size_t);ALLOC(r->temp_scopes,n,size_t);ALLOC(r->snaps,n,size_t);ALLOC(r->scopes,n,size_t);ALLOC(r->regions,n,size_t);ALLOC(r->handlers,n,size_t);ALLOC(r->td,r->blocks,size_t);ALLOC(r->sd,r->blocks,size_t);ALLOC(r->cd,r->blocks,size_t);ALLOC(r->rd,r->blocks,size_t);ALLOC(r->hd,r->blocks,size_t);ALLOC(r->known,r->blocks,unsigned char);
#undef ALLOC
 return true; }
static IndependentStorage *st(const IndependentReplay*r,size_t b){return r->storage+b*r->locals;} static unsigned char *ho(const IndependentReplay*r,size_t b){return r->holes+b*r->places;} static size_t *ss(size_t *p,const IndependentReplay*r,size_t b){return p+b*r->width;}
/* Applies executable materialized effects.  The materializer has authenticated
 * all individual references; this replay still rejects impossible states. */
static bool apply_instruction(const IndependentReplay *r,const SolMirMaterializedInstruction *in,IndependentStorage *s,unsigned char *h,size_t *temp,size_t *temp_scope,size_t *td,size_t *snap,size_t *sd,size_t *scope,size_t *cd,size_t *region,size_t *rd,size_t *handler,size_t *hd) { const SolMirMaterialization*m=r->m;size_t local=SOL_MIR_MATERIALIZED_NONE;if(in->place<m->place_count)local=m->places[in->place].local;else if(in->local>=r->image->locals.offset&&in->local<r->image->locals.offset+r->locals)local=in->local; size_t li=local==SOL_MIR_MATERIALIZED_NONE?0:local-r->image->locals.offset;
#define PUSH(a,d,x) do{if(*(d)>=r->width)return false;(a)[(*(d))++]=(x);}while(0)
#define POP(a,d,x) do{if(!*(d)||(a)[*(d)-1]!=(x))return false;--*(d);}while(0)
 switch(in->kind){
 case SOL_MIR_INST_PARAMETER_LIVE: if(local==SOL_MIR_MATERIALIZED_NONE||s[li]!=DEAD)return false;s[li]=INITIALIZED;break;
 case SOL_MIR_INST_STORAGE_LIVE: if(local==SOL_MIR_MATERIALIZED_NONE||s[li]!=DEAD)return false;s[li]=UNINITIALIZED;break;
 case SOL_MIR_INST_DROP_IF_INITIALIZED: if(local==SOL_MIR_MATERIALIZED_NONE)return false;if(s[li]==DEAD)break;s[li]=UNINITIALIZED;clear_local(r,h,local);break;
 case SOL_MIR_INST_STORAGE_DEAD: if(local==SOL_MIR_MATERIALIZED_NONE||s[li]!=UNINITIALIZED)return false;s[li]=DEAD;break;
 case SOL_MIR_INST_LOAD_MOVE: if(local==SOL_MIR_MATERIALIZED_NONE||s[li]!=INITIALIZED)return false;if(!m->places[in->place].projections.count){s[li]=UNINITIALIZED;clear_local(r,h,local);}else{clear_covered(r,h,in->place);h[in->place-r->image->places.offset]=INDEPENDENT_HOLE_MUST;}break;
 case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED: if(local==SOL_MIR_MATERIALIZED_NONE||s[li]!=INITIALIZED)return false;if(!m->places[in->place].projections.count){s[li]=UNINITIALIZED;clear_local(r,h,local);}else{clear_covered(r,h,in->place);h[in->place-r->image->places.offset]=INDEPENDENT_HOLE_MUST;}break;
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
static bool apply_terminator_temps(const IndependentReplay *r, const SolMirMaterializedTerminator *term, size_t *temp, size_t *depth) {
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
static bool compute_states(IndependentReplay*r,ValidationWorkspace*w,size_t *work) { size_t entry=r->image->entry-r->image->blocks.offset;if(entry>=r->blocks)return false;r->known[entry]=1;bool changed=true;size_t pass=0,limit=r->blocks?(r->blocks*(r->locals+r->places+1)+1):1;while(changed&&pass++<limit){if(!METER())return false;changed=false;for(size_t b=0;b<r->blocks;b++){if(!METER())return false;if(!r->known[b])continue;if(!meter_work(work))return false;size_t mark=w->used;IndependentStorage *s=workspace_take(w,r->locals,sizeof *s);unsigned char*h=workspace_take(w,r->places,sizeof *h);size_t *tmp=workspace_take(w,r->width,sizeof *tmp),*tmpo=workspace_take(w,r->width,sizeof *tmpo),*sn=workspace_take(w,r->width,sizeof *sn),*sc=workspace_take(w,r->width,sizeof *sc),*re=workspace_take(w,r->width,sizeof *re),*ha=workspace_take(w,r->width,sizeof *ha);if((r->locals&&!s)||(r->places&&!h)||!tmp||!tmpo||!sn||!sc||!re||!ha){w->used=mark;return false;}if(r->locals)memcpy(s,st(r,b),r->locals*sizeof*s);if(r->places)memcpy(h,ho(r,b),r->places);size_t td=r->td[b],sd=r->sd[b],cd=r->cd[b],rd=r->rd[b],hd=r->hd[b];memcpy(tmp,ss(r->temps,r,b),td*sizeof*tmp);memcpy(tmpo,ss(r->temp_scopes,r,b),td*sizeof*tmpo);memcpy(sn,ss(r->snaps,r,b),sd*sizeof*sn);memcpy(sc,ss(r->scopes,r,b),cd*sizeof*sc);memcpy(re,ss(r->regions,r,b),rd*sizeof*re);memcpy(ha,ss(r->handlers,r,b),hd*sizeof*ha);const SolMirMaterializedBlock *bl=&r->m->blocks[r->image->blocks.offset+b];bool ok=true;for(size_t i=0;i<bl->instructions.count&&ok;i++){if(!meter_work(work)){ok=false;break;}ok=apply_instruction(r,&r->m->instructions[bl->instructions.offset+i],s,h,tmp,tmpo,&td,sn,&sd,sc,&cd,re,&rd,ha,&hd);}if(ok)ok=apply_terminator_temps(r,&bl->terminator,tmp,&td);size_t es[3],en=edges(&bl->terminator,es);for(size_t q=0;q<en&&ok;q++){if(!meter_work(work)){ok=false;break;}if(es[q]>=r->m->edge_count){ok=false;break;}size_t dest=r->m->edges[es[q]].block-r->image->blocks.offset;if(dest>=r->blocks){ok=false;break;}if(!r->known[dest]){r->known[dest]=1;if(r->locals)memcpy(st(r,dest),s,r->locals*sizeof*s);if(r->places)memcpy(ho(r,dest),h,r->places);r->td[dest]=td;r->sd[dest]=sd;r->cd[dest]=cd;r->rd[dest]=rd;r->hd[dest]=hd;memcpy(ss(r->temps,r,dest),tmp,td*sizeof*tmp);memcpy(ss(r->temp_scopes,r,dest),tmpo,td*sizeof*tmpo);memcpy(ss(r->snaps,r,dest),sn,sd*sizeof*sn);memcpy(ss(r->scopes,r,dest),sc,cd*sizeof*sc);memcpy(ss(r->regions,r,dest),re,rd*sizeof*re);memcpy(ss(r->handlers,r,dest),ha,hd*sizeof*ha);changed=true;}else {if(!meter_work(work)){ok=false;break;}ok=merge_storage(st(r,dest),s,r->locals,&changed);for(size_t p=0;p<r->places;p++){if(!METER()){ok=false;break;}unsigned char a=ho(r,dest)[p], incoming=h[p], x=a==incoming?a:INDEPENDENT_HOLE_MAY;changed|=x!=a;ho(r,dest)[p]=x;}ok=ok&&stack_equal(ss(r->temps,r,dest),r->td[dest],tmp,td)&&stack_equal(ss(r->temp_scopes,r,dest),r->td[dest],tmpo,td)&&stack_equal(ss(r->snaps,r,dest),r->sd[dest],sn,sd)&&stack_equal(ss(r->scopes,r,dest),r->cd[dest],sc,cd)&&stack_equal(ss(r->regions,r,dest),r->rd[dest],re,rd)&&stack_equal(ss(r->handlers,r,dest),r->hd[dest],ha,hd);}}
w->used=mark;if(!ok)return false;}}return !changed; }
static SolMirRuntimeCleanupProducerKind independent_producer(
    const SolMirRuntimeCleanup *owner, const SolMirRuntimeCleanupEvent *event) {
    const SolMirMaterialization *m = &owner->conventions->concrete->materialization;
    const SolMirOperations *ops = &owner->conventions->concrete->operations;
    if (event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP)
        return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_STEP_METER;
    if (event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_INVOKE_CALLABLE)
        return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CALLABLE_CONSTRUCTION;
    if (event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_PROPAGATE_RESIDUAL)
        return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PROPAGATION_RESIDUAL;
    if (event->supplemental_site != SOL_MIR_RUNTIME_NONE)
        return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION;
    if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        && event->block < m->block_count) {
        switch (m->blocks[event->block].terminator.kind) {
            case SOL_MIR_TERM_INVOKE: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE;
            case SOL_MIR_TERM_PANIC: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_PANIC;
            case SOL_MIR_TERM_MATCH_FAILURE: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_NO_MATCH;
            case SOL_MIR_TERM_UNREACHABLE: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_UNREACHABLE;
            default: break;
        }
    }
    if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR
        && event->block < ops->predicate_block_count) {
        const SolMirPredicateTerminator *term = &ops->predicate_blocks[event->block].terminator;
        if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE)
            return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_INVOKE;
        if (term->kind == SOL_MIR_PREDICATE_TERM_FAILURE
            && term->failure_kind == SOL_MIR_PREDICATE_FAILURE_NO_MATCH)
            return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_NO_MATCH;
        if (term->kind == SOL_MIR_PREDICATE_TERM_RETURN)
            return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_RESULT;
    }
    if (event->origin == SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT)
        return event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
            ? SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_ARITHMETIC
            : SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_ARITHMETIC;
    return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL;
}
static bool sink_event(IndependentSink *s, const SolMirRuntimeCleanupEvent *event) {
    if (s->have_event || s->event_index >= s->owner->event_count) return false;
    s->current_event = *event;
    s->current_event.producer = independent_producer(s->owner, event);
    s->have_event = true;
    return add(&s->count.events, 1) && meter_work(s->work);
}
static bool sink_action(IndependentSink *s, SolMirRuntimeCleanupActionKind kind,
    unsigned flags, size_t target, SolMirRecipeId recipe) {
    if (s->action_index >= s->owner->action_count) return false;
    SolMirRuntimeCleanupAction expected = {.kind=kind,.flags=flags,.target=target,.recipe=recipe,
        .drop_path=SOL_MIR_RUNTIME_NONE};
    if (memcmp(&expected, &s->owner->actions[s->action_index], sizeof(expected)) != 0) return false;
    ++s->action_index;
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
static bool selected_hole(const IndependentReplay *r, const unsigned char *holes, size_t target, size_t candidate) {
    if (!holes[candidate - r->image->places.offset] || !prefix(r->m, target, candidate)) return false;
    for (size_t q = 0; q < r->places; ++q) {
        if (!METER()) return false;
        size_t other = r->image->places.offset + q;
        if (other != candidate && holes[q] && prefix(r->m, other, candidate)) return false;
    }
    return true;
}
static bool selected_hole_count(const IndependentReplay *r, const unsigned char *holes,
    size_t target, size_t *count_out) {
    size_t count = 0;
    for (size_t p = 0; p < r->places; ++p) {
        if (!METER()) return false;
        if (selected_hole(r, holes, target, r->image->places.offset + p)) ++count;
    }
    *count_out = count;
    return !scan_meter_failed;
}
static size_t selected_hole_at(const IndependentReplay *r, const unsigned char *holes, size_t target, size_t rank) {
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
        if (before == rank) return candidate;
    }
    return SOL_MIR_RUNTIME_NONE;
}
static bool sink_drop_action(IndependentSink *s, SolMirRuntimeCleanupActionKind kind, unsigned flags,
    size_t target, SolMirRecipeId recipe, const IndependentReplay *r, const unsigned char *holes) {
    size_t place = kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE ? target : root_place(r, target);
    if (place == SOL_MIR_RUNTIME_NONE || place >= r->m->place_count || s->action_index >= s->owner->action_count) return false;
    size_t holes_count;
    if (!selected_hole_count(r, holes, place, &holes_count)) return false;
    size_t root = root_place(r, r->m->places[place].local);
    size_t path = s->count.drop_paths;
    SolMirRuntimeCleanupAction expected = {kind, flags, target, recipe,
        s->check_drop_paths ? path : SOL_MIR_RUNTIME_NONE};
    if (s->owner->actions[s->action_index].kind != expected.kind
        || s->owner->actions[s->action_index].flags != expected.flags
        || s->owner->actions[s->action_index].target != expected.target
        || s->owner->actions[s->action_index].recipe != expected.recipe
        || (s->check_drop_paths && s->owner->actions[s->action_index].drop_path
            != expected.drop_path)) return false;
    if (s->check_drop_paths) {
        if (path >= s->owner->drop_path_count) return false;
        SolMirRuntimeCleanupDropPath main = {root, place, {path + 1, holes_count}, recipe,
            (flags & SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED) ? SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL
                : SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE};
        if (memcmp(&main, &s->owner->drop_paths[path], sizeof(main)) != 0) return false;
        for (size_t i = 0; i < holes_count; ++i) {
            size_t hole = selected_hole_at(r, holes, place, i);
            if (hole == SOL_MIR_RUNTIME_NONE) return false;
            unsigned char state = holes[hole - r->image->places.offset];
            SolMirRuntimeCleanupDropPath expected_hole = {root, hole, {0, 0}, recipe,
                state == INDEPENDENT_HOLE_MUST ? SOL_MIR_RUNTIME_CLEANUP_DROP_DEFINITE : SOL_MIR_RUNTIME_CLEANUP_DROP_CONDITIONAL};
            if (path + 1 + i >= s->owner->drop_path_count
                || memcmp(&expected_hole, &s->owner->drop_paths[path + 1 + i], sizeof(expected_hole)) != 0) return false;
        }
    }
    ++s->action_index;
    if (!add(&s->count.actions, 1) || !add(&s->count.drop_paths, 1 + holes_count)) return false;
    return meter_work(s->work);
}
static bool sink_transition(IndependentSink *s, SolMirRuntimeCleanupEventId event,
    SolMirRuntimeCleanupOutcome outcome, size_t edge, size_t offset) {
    if (s->transition_index >= s->owner->transition_count || offset > s->action_index) return false;
    SolMirRuntimeCleanupTransition expected = s->owner->transitions[s->transition_index];
    expected.event = event; expected.outcome = outcome; expected.continuation = edge;
    expected.actions = (SolMirRuntimeSlice){offset, s->action_index - offset};
    expected.primary_failure_wins = true;
    if (memcmp(&expected, &s->owner->transitions[s->transition_index], sizeof(expected)) != 0)
        return false;
    ++s->transition_index;
    return add(&s->count.transitions, 1) && meter_work(s->work);
}
static bool sink_supplemental(IndependentSink *s,
    const SolMirRuntimeCleanupSupplementalSite *site) {
    if (s->supplemental_index >= s->owner->supplemental_site_count
        || memcmp(site, &s->owner->supplemental_sites[s->supplemental_index], sizeof(*site)) != 0)
        return false;
    ++s->supplemental_index;
    return add(&s->count.supplemental_sites, 1) && meter_work(s->work);
}
static bool sink_finish_event(IndependentSink *s, size_t id) {
    if (!s->have_event || id != s->event_index) return false;
    s->current_event.actions.count = s->action_index - s->current_event.actions.offset;
    s->current_event.transitions.count = s->transition_index - s->current_event.transitions.offset;
    if (memcmp(&s->current_event, &s->owner->events[s->event_index],
            sizeof(s->current_event)) != 0) return false;
    ++s->event_index;
    s->have_event = false;
    return true;
}
static size_t action_pos(const IndependentSink *s) { return s->action_index; }
static size_t event_pos(const IndependentSink *s) { return s->event_index; }
/* Source cleanup slices are the authoritative lexical owner map.  Rebuild
 * the bucket from provenance instead of relying on materialized-local order. */
/* Independently rebuild each lexical bucket from source-local provenance.  This
 * intentionally does not share the builder's bucket helper or owner state. */
static bool independent_scope_bucket(const IndependentReplay *r, size_t scope_id, SolIrSlice *slice,
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
static bool emit_independent_scope_bucket(IndependentSink *s, const IndependentReplay *r, IndependentStorage *state,
    unsigned char *holes, size_t scope_id) {
    const SolMirMaterialization *m = r->m;
    const SolIr *ir = m->plan->program->ir;
    SolIrSlice slice; bool forward;
    if (!independent_scope_bucket(r, scope_id, &slice, &forward)) return false;
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
static size_t active_handler(const IndependentReplay *r, const size_t *handlers, size_t hd,
    size_t source) {
    for (size_t i = hd; i; --i) {
        size_t handler = handlers[i - 1];
        if (handler < r->m->handler_count
            && r->m->handlers[handler].source_expression == source) return handler;
    }
    return SOL_MIR_RUNTIME_NONE;
}
/* Reconstruct snapshot destruction directly from the independently replayed
 * capture stack.  This intentionally does not call a construction helper. */
static bool emit_independent_snapshot_cleanup(IndependentSink *s,
    const SolMirRuntimeCleanup *owner, const IndependentReplay *r,
    const size_t *captures, size_t depth) {
    const SolMirOperations *operations = &owner->conventions->concrete->operations;
    for (size_t i = depth; i; --i) {
        size_t plan = snapshot_plan(operations, r->image_id, captures[i - 1]);
        if (plan == SOL_MIR_RUNTIME_NONE || !sink_action(s,
                SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT, 0, plan,
                operations->snapshots[plan].recipe)) return false;
    }
    return true;
}
static bool emit_implicit_cleanup(IndependentSink *s, const SolMirRuntimeCleanup *owner,
    const IndependentReplay *r, IndependentStorage *state, unsigned char *holes, size_t *temps,
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
        if (!emit_independent_scope_bucket(s, r, state, holes, scope)
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
    while (hd != 0) {
        size_t handler = handlers[--hd];
        if (handler >= m->handler_count || !sink_action(s,
                SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_HANDLER, 0, handler,
                SOL_MIR_RECIPE_NONE)) return false;
    }
    while (rd != 0) {
        size_t region = regions[--rd];
        if (!sink_action(s, SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_REGION, 0,
                region, SOL_MIR_RECIPE_NONE)) return false;
    }
    if (!emit_independent_snapshot_cleanup(s, owner, r, snapshots, sd)) return false;
    for (size_t ordinal = r->locals; ordinal; --ordinal) for (size_t i = 0; i < r->locals; ++i) {
        size_t local = r->image->locals.offset + i;
        const SolMirMaterializedLocal *item = &m->locals[local];
        if (item->kind != SOL_MIR_MATERIALIZED_LOCAL_PARAMETER || item->ordinal != ordinal - 1
            || item->access != SOL_ACCESS_OWNED || state[i] == DEAD || state[i] == UNINITIALIZED) continue;
        unsigned flags = (state[i] == MAYBE || has_hole(r, holes, local))
            ? SOL_MIR_RUNTIME_CLEANUP_ACTION_GUARDED : 0;
        if (root_place(r, local) == SOL_MIR_RUNTIME_NONE
            && m->types[item->type].is_copy) continue;
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
        if (root_place(r, local) == SOL_MIR_RUNTIME_NONE
            && m->types[item->type].is_copy) continue;
        if (!sink_drop_action(s, SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER,
                flags, local, item->type, r, holes)) return false;
    }
    return true;
}
static bool implicit_arithmetic(const SolMirConcreteProgram*p,size_t instruction) { for(size_t i=0;i<p->operations.arithmetic_count;i++)if(p->operations.arithmetic[i].instruction==instruction&&p->operations.arithmetic[i].failures)return true;return false; }
static bool allocation_instruction(const SolMirRuntimeValues *v,
    const SolMirMaterialization *m, const SolMirMaterializedInstruction *in) {
    if (in->kind == SOL_MIR_INST_FUNCTION_VALUE) return true;
    SolMirRecipeId recipe = SOL_MIR_RECIPE_NONE;
    if (in->kind == SOL_MIR_INST_CONST_TEXT || in->kind == SOL_MIR_INST_CONSTRUCT)
        recipe = in->type;
    else if (in->kind == SOL_MIR_INST_LOAD_COPY && in->place < m->place_count)
        recipe = m->places[in->place].final_type;
    return recipe < v->allocation_plan_count
        && v->allocation_plans[recipe].kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE;
}
static bool pattern_copy_requires_runtime(SolMirCopyKind kind) {
    return kind == SOL_MIR_COPY_TEXT || kind == SOL_MIR_COPY_AGGREGATE
        || kind == SOL_MIR_COPY_WRAPPER;
}
static bool allocation_pattern_instruction(const SolMirRuntimeValues *values,
    const SolMirOperations *ops, size_t instruction) {
    for (size_t i = 0; i < ops->pattern_extraction_count; ++i) {
        const SolMirOperationPatternExtraction *plan = &ops->pattern_extractions[i];
        if (plan->instruction != instruction || !pattern_copy_requires_runtime(plan->copy_kind)
            || plan->result_recipe >= values->allocation_plan_count) continue;
        return values->allocation_plans[plan->result_recipe].kind
            != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE;
    }
    return false;
}
static bool emit_implicit(IndependentSink*s,const SolMirRuntimeCleanup *owner,const IndependentReplay*r,size_t block,size_t instruction,SolSpan span,bool allocation,IndependentStorage *stt,unsigned char*holes,size_t*tmp,size_t*tmpo,size_t td,size_t*snap,size_t sd,size_t*scope,size_t cd,size_t*region,size_t rd,size_t*handler,size_t hd) { SolMirRuntimeSource src;if(!source_for(owner->conventions->concrete->program.ir,span,&src))return false;SolMirRuntimeCleanupEvent e={SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION,SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION,SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT,r->image_id,block,instruction,SOL_MIR_RUNTIME_NONE,src,inherited(owner->conventions,r->image_id,block,instruction),SOL_MIR_RUNTIME_NONE,{action_pos(s),0},{s->count.transitions,0},SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL,false,SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE};e.captures_failure_detail=true;size_t id=event_pos(s);if(allocation&&e.inherited_failure_site==SOL_MIR_RUNTIME_NONE){e.supplemental_site=s->count.supplemental_sites;if(!sink_supplemental(s,&(SolMirRuntimeCleanupSupplementalSite){id,src,failbit(SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT)|failbit(SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED)}))return false;}
 if(!sink_event(s,&e))return false;size_t at=action_pos(s);if(!sink_transition(s,id,SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,SOL_MIR_RUNTIME_NONE,at))return false;at=action_pos(s);if(!emit_implicit_cleanup(s,owner,r,stt,holes,tmp,tmpo,td,snap,sd,scope,cd,region,rd,handler,hd)||!sink_action(s,SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE,SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY,e.inherited_failure_site!=SOL_MIR_RUNTIME_NONE?e.inherited_failure_site:e.supplemental_site,SOL_MIR_RECIPE_NONE)||!sink_transition(s,id,SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE,SOL_MIR_RUNTIME_NONE,at))return false;return sink_finish_event(s, id); }
static bool emit_step(IndependentSink*s,const SolMirRuntimeCleanup*owner,
    const IndependentReplay*r,SolMirRuntimeCleanupEventKind kind,size_t block,
    size_t operation,SolSpan span,IndependentStorage*state,unsigned char*holes,
    size_t*temps,size_t*temp_scopes,size_t td,size_t*snapshots,size_t sd,
    size_t*scopes,size_t cd,size_t*regions,size_t rd,size_t*handlers,size_t hd){
    SolMirRuntimeSource source;
    size_t site=step_site(owner->conventions,SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP,
        r->image_id,block,operation);
    if(site==SOL_MIR_RUNTIME_NONE||!source_for(owner->conventions->concrete->program.ir,span,&source))return false;
    SolMirRuntimeCleanupEvent event={kind,SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP,
        SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT,r->image_id,block,operation,
        SOL_MIR_RUNTIME_NONE,source,site,SOL_MIR_RUNTIME_NONE,{action_pos(s),0},
        {s->count.transitions,0},SOL_MIR_RUNTIME_CLEANUP_PRODUCER_STEP_METER,true,
        SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE};
    size_t id=event_pos(s),at=action_pos(s);
    if(!sink_event(s,&event)||!sink_transition(s,id,SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,SOL_MIR_RUNTIME_NONE,at))return false;
    at=action_pos(s);
    if(!emit_implicit_cleanup(s,owner,r,state,holes,temps,temp_scopes,td,snapshots,sd,
            scopes,cd,regions,rd,handlers,hd)
        ||!sink_action(s,SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE,
            SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY,site,SOL_MIR_RECIPE_NONE)
        ||!sink_transition(s,id,SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE,SOL_MIR_RUNTIME_NONE,at))return false;
    return sink_finish_event(s,id);
}
static bool terminal_failure(SolMirTerminatorKind k){return k==SOL_MIR_TERM_PANIC||k==SOL_MIR_TERM_MATCH_FAILURE||k==SOL_MIR_TERM_UNREACHABLE||k==SOL_MIR_TERM_RESUME_FAILURE||k==SOL_MIR_TERM_CONTRACT_VIOLATION;}
/* This intentionally reconstructs the refinement chain independently of the
 * builder.  In particular, numeric image/predicate block coincidence is not a
 * valid join. */
static SolMirRuntimeFailureSiteId validation_refined_result_site(
    const SolMirRuntimeConventions *conventions, size_t image, size_t block,
    const SolMirMaterializedTerminator *term) {
    const SolMirConcreteProgram *concrete = conventions->concrete;
    const SolMirMaterialization *m = &concrete->materialization;
    const SolMirOperations *operations = &concrete->operations;
    const SolIr *ir = concrete->program.ir;
    if (term->kind != SOL_MIR_TERM_CHECK_REFINED || image >= m->image_count
        || block >= m->block_count || term->source_obligation >= ir->obligation_count
        || term->source_expression >= ir->expression_count
        || term->representation >= m->temporary_count
        || term->result >= m->value_count) return SOL_MIR_RUNTIME_NONE;
    const SolMirOperationPredicatePlan *selected = NULL;
    for (size_t i = 0; i < operations->predicate_count; ++i) {
        if (!METER()) return SOL_MIR_RUNTIME_NONE;
        const SolMirOperationPredicatePlan *candidate = &operations->predicates[i];
        if (candidate->image != image || candidate->block != block) continue;
        if (selected != NULL) return SOL_MIR_RUNTIME_NONE;
        selected = candidate;
    }
    if (selected == NULL || selected->kind != SOL_MIR_OPERATION_PREDICATE_REFINEMENT
        || selected->context >= m->context_count
        || selected->body >= operations->predicate_body_count
        || selected->representation != term->representation || selected->result != term->result
        || selected->input_recipe != m->temporaries[term->representation].type
        || selected->result_recipe != m->values[term->result].type) return SOL_MIR_RUNTIME_NONE;
    const SolMirPlanContext *context = &m->contexts[selected->context];
    const SolMirPredicateBody *body = &operations->predicate_bodies[selected->body];
    const SolIrObligation *obligation = &ir->obligations[term->source_obligation];
    const SolMirMaterializedImage *materialized_image = &m->images[image];
    if (selected->context < materialized_image->contexts.offset
        || selected->context - materialized_image->contexts.offset
            >= materialized_image->contexts.count
        || context->kind != SOL_MIR_PLAN_CONTEXT_REFINEMENT
        || context->target_kind != SOL_MIR_PLAN_TARGET_INSTANCE
        || context->instance != image || context->obligation != term->source_obligation
        || context->source_block != m->blocks[block].source_block
        || context->definition != term->source_definition
        || context->source.expression != term->source_expression
        || obligation->owner_kind != SOL_CONTRACT_OWNER_TYPE
        || obligation->owner != term->source_definition
        || body->owner_kind != SOL_MIR_PREDICATE_OWNER_INSTANCE
        || body->instance != image || body->context != selected->context
        || body->refinement_self_recipe != selected->input_recipe
        || body->output_recipe != selected->output_recipe) return SOL_MIR_RUNTIME_NONE;
    size_t return_coordinate = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < body->blocks.count; ++i) {
        if (!METER()) return SOL_MIR_RUNTIME_NONE;
        size_t candidate = body->blocks.offset + i;
        if (candidate >= operations->predicate_block_count
            || operations->predicate_blocks[candidate].body != selected->body)
            return SOL_MIR_RUNTIME_NONE;
        if (operations->predicate_blocks[candidate].terminator.kind
                != SOL_MIR_PREDICATE_TERM_RETURN) continue;
        if (return_coordinate != SOL_MIR_RUNTIME_NONE) return SOL_MIR_RUNTIME_NONE;
        return_coordinate = candidate;
    }
    SolMirRuntimeSource expected_source;
    if (return_coordinate == SOL_MIR_RUNTIME_NONE
        || obligation->predicate >= ir->expression_count
        || !source_for(ir, ir->expressions[obligation->predicate].span, &expected_source))
        return SOL_MIR_RUNTIME_NONE;
    size_t result = SOL_MIR_RUNTIME_NONE;
    uint32_t expected_mask = failbit(SOL_MIR_RUNTIME_FAILURE_REFINEMENT_VIOLATION);
    for (size_t i = 0; i < conventions->failure_site_count; ++i) {
        if (!METER()) return SOL_MIR_RUNTIME_NONE;
        const SolMirRuntimeFailureSite *site = &conventions->failure_sites[i];
        if (site->origin_kind != SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
            || site->owner != selected->body || site->block != return_coordinate
            || site->instruction != SOL_MIR_RUNTIME_NONE
            || site->source.file != expected_source.file
            || site->source.start != expected_source.start
            || site->source.end != expected_source.end
            || site->allowed_codes != expected_mask) continue;
        if (result != SOL_MIR_RUNTIME_NONE) return SOL_MIR_RUNTIME_NONE;
        result = i;
    }
    return result;
}

static bool emit_term(IndependentSink *s, const SolMirRuntimeCleanup *owner,
    const IndependentReplay *r, size_t block, const SolMirMaterializedTerminator *t,
    IndependentStorage *stt, unsigned char *holes, size_t *tmp, size_t *tmpo,
    size_t td, size_t *snap, size_t sd, size_t *scope, size_t cd, size_t *region,
    size_t rd, size_t *handler, size_t hd) {
    SolMirRuntimeSource src;
    if (!source_for(owner->conventions->concrete->program.ir, t->span, &src)) return false;
    SolMirRuntimeFailureSiteId inherited_site = control_inherited(owner->conventions,
        r->m, r->image, r->image_id, block, t);
    if (t->kind == SOL_MIR_TERM_CHECK_REFINED) {
        inherited_site = validation_refined_result_site(owner->conventions,
            r->image_id, block, t);
        if (inherited_site == SOL_MIR_RUNTIME_NONE) return false;
    }
    SolMirRuntimeCleanupEvent e = {SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR,
        SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION, SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT,
        r->image_id, block, SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE, src, inherited_site,
        SOL_MIR_RUNTIME_NONE, {action_pos(s), 0}, {s->count.transitions, 0},
        SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL, false,
        SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE};
    e.captures_failure_detail = terminal_failure(t->kind) || t->kind == SOL_MIR_TERM_INVOKE
        || t->kind == SOL_MIR_TERM_CHECK_REFINED || t->kind == SOL_MIR_TERM_CHECK_CONTRACT;
    e.capture_detail_kind = t->kind == SOL_MIR_TERM_PANIC
        ? SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT : SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE;
    size_t id = event_pos(s);
    if (!sink_event(s, &e)) return false;
    size_t normal[3], n = edges(t, normal);
    bool is_failure = terminal_failure(t->kind);
    for (size_t i = 0; i < n; ++i) {
        bool evaluation_failure = (t->kind == SOL_MIR_TERM_INVOKE && normal[i] == t->failure_edge)
            || (t->kind == SOL_MIR_TERM_CHECK_REFINED && normal[i] == t->failure_edge)
            || (t->kind == SOL_MIR_TERM_CHECK_CONTRACT && normal[i] == t->failure_edge);
        bool violation = t->kind == SOL_MIR_TERM_CHECK_CONTRACT
            && normal[i] == t->violation_edge;
        size_t at = action_pos(s);
        if (!evaluation_failure && !violation && t->kind == SOL_MIR_TERM_INVOKE
            && normal[i] == t->normal_edge)
            for (size_t q = 0; q < t->writebacks.count; ++q) {
                const SolMirMaterializedWriteback *w = &r->m->writebacks[t->writebacks.offset + q];
                if (!sink_action(s, SOL_MIR_RUNTIME_CLEANUP_ACTION_WRITEBACK,
                        SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY, w->place, w->type)) return false;
            }
        if (!evaluation_failure && (t->kind == SOL_MIR_TERM_CHECK_REFINED
                || t->kind == SOL_MIR_TERM_CHECK_CONTRACT)
            && !sink_action(s, SOL_MIR_RUNTIME_CLEANUP_ACTION_CHECK_CONTRACT,
                normal[i] == t->satisfied_edge
                    ? SOL_MIR_RUNTIME_CLEANUP_ACTION_NORMAL_ONLY : 0,
                t->source_obligation, SOL_MIR_RECIPE_NONE)) return false;
        SolMirRuntimeCleanupOutcome transition_outcome = evaluation_failure || violation
            ? SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
            : SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL;
        if (!sink_transition(s, id, transition_outcome, normal[i], at)) return false;
        if (t->kind == SOL_MIR_TERM_CHECK_REFINED && evaluation_failure
            && !sink_transition(s, id, SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE,
                normal[i], at)) return false;
    }
 if(t->kind==SOL_MIR_TERM_RETURN||is_failure){size_t at=action_pos(s);if(t->kind==SOL_MIR_TERM_RETURN&&!emit_independent_snapshot_cleanup(s,owner,r,snap,sd))return false;if(is_failure&&!emit_implicit_cleanup(s,owner,r,stt,holes,tmp,tmpo,td,snap,sd,scope,cd,region,rd,handler,hd))return false;if(is_failure&&!sink_action(s,SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE,SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY,e.inherited_failure_site,SOL_MIR_RECIPE_NONE))return false;if(!sink_transition(s,id,is_failure?SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE:SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT,SOL_MIR_RUNTIME_NONE,at))return false;}
 return sink_finish_event(s, id); }
static bool emit_cleanup_instruction(IndependentSink *s, const SolMirRuntimeCleanup *owner,
    const IndependentReplay *r, size_t block, size_t instruction,
    const SolMirMaterializedInstruction *in, const IndependentStorage *state,
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
            IndependentStorage storage = state[local - r->image->locals.offset];
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
            IndependentStorage storage = state[local - r->image->locals.offset];
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
        SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION, SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT, r->image_id, block, instruction, SOL_MIR_RUNTIME_NONE,
        src, SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE, {action_pos(s), 0},
        {s->count.transitions, 0}, SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL,false,SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE};
    size_t id = event_pos(s);
    if (!sink_event(s, &e) || !((kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE
            || kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER)
            ? sink_drop_action(s, kind, flags, target, recipe, r, holes)
            : sink_action(s, kind, flags, target, recipe))
        || !sink_transition(s, id, SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,
            SOL_MIR_RUNTIME_NONE, e.actions.offset)) return false;
    return sink_finish_event(s, id);
}
static size_t pre_callable_plan(const SolMirOperations *ops, size_t site) {
    size_t found = SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < ops->callable_count; ++i) {
        if (!METER()) return SOL_MIR_RUNTIME_NONE;
        if (ops->callables[i].semantic_site != site) continue;
        if (found != SOL_MIR_RUNTIME_NONE) return SOL_MIR_RUNTIME_NONE;
        found = i;
    }
    return found;
}

static bool pre_operation_event(IndependentSink *s, const SolMirRuntimeCleanup *owner,
    const IndependentReplay *r, size_t block, const SolMirMaterializedTerminator *term,
    IndependentStorage *state, unsigned char *holes, size_t *temps, size_t *temp_scopes,
    size_t td, size_t *snapshots, size_t sd, size_t *scopes, size_t cd,
    size_t *regions, size_t rd, size_t *handlers, size_t hd) {
    const SolMirOperations *ops = &owner->conventions->concrete->operations;
    const SolMirMaterialization *m = r->m;
    SolMirRuntimeCleanupPhase phase;
    SolMirRuntimeCleanupProducerKind producer;
    size_t operation = SOL_MIR_RUNTIME_NONE, semantic_site = SOL_MIR_RUNTIME_NONE;
    SolMirRuntimeSource source;
    bool needed = false;
    if (term->kind == SOL_MIR_TERM_INVOKE
        && term->callable_site != SOL_MIR_MATERIALIZED_NONE
        && term->callable_site < m->semantic_site_count) {
        size_t callable = pre_callable_plan(ops, term->callable_site);
        if (callable != SOL_MIR_RUNTIME_NONE
            && ops->callables[callable].kind == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
            && ops->callables[callable].function_recipe < owner->values->allocation_plan_count
            && owner->conventions->concrete->layout.types[ops->callables[callable].function_recipe].object_kind
                == SOL_MIR_LAYOUT_OBJECT_CALLABLE) {
            phase = SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_INVOKE_CALLABLE;
            producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CALLABLE_CONSTRUCTION;
            semantic_site = term->callable_site;
            const SolMirProgramSource *semantic = &m->semantic_sites[semantic_site].source;
            source = (SolMirRuntimeSource){semantic->file, semantic->start, semantic->end};
            needed = true;
        }
    } else if (term->kind == SOL_MIR_TERM_PROPAGATE) {
        for (size_t i = 0; i < ops->propagation_count; ++i) {
            const SolMirOperationPropagationPlan *plan = &ops->propagations[i];
            if (plan->image != r->image_id || plan->block != block) continue;
            if (needed || plan->residual_recipe >= owner->values->allocation_plan_count
                || owner->values->allocation_plans[plan->residual_recipe].kind
                    == SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE) return false;
            phase = SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_PROPAGATE_RESIDUAL;
            producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PROPAGATION_RESIDUAL;
            operation = i;
            if (!source_for(owner->conventions->concrete->program.ir, term->span, &source))
                return false;
            needed = true;
        }
    }
    if (!needed) return true;
    SolMirRuntimeCleanupEvent event = {
        SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR, phase,
        SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT, r->image_id, block, operation,
        semantic_site, source, SOL_MIR_RUNTIME_NONE, s->count.supplemental_sites,
        {action_pos(s), 0}, {s->count.transitions, 0}, producer, true,
        SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE,
    };
    size_t id = event_pos(s);
    if (!sink_supplemental(s, &(SolMirRuntimeCleanupSupplementalSite){id, source,
            failbit(SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED)
            | failbit(SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT)})
        || !sink_event(s, &event)) return false;
    size_t at = action_pos(s);
    if (!sink_transition(s, id, SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,
            SOL_MIR_RUNTIME_NONE, at)) return false;
    at = action_pos(s);
    if (!emit_implicit_cleanup(s, owner, r, state, holes, temps, temp_scopes, td,
            snapshots, sd, scopes, cd, regions, rd, handlers, hd)
        || !sink_action(s, SOL_MIR_RUNTIME_CLEANUP_ACTION_PROPAGATE_FAILURE,
            SOL_MIR_RUNTIME_CLEANUP_ACTION_FAILURE_ONLY, event.supplemental_site,
            SOL_MIR_RECIPE_NONE)
        || !sink_transition(s, id, SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE,
            SOL_MIR_RUNTIME_NONE, at)) return false;
    return sink_finish_event(s, id);
}

static bool emit_image(IndependentSink*s,const SolMirRuntimeCleanup*owner,size_t image,ValidationWorkspace*w){IndependentReplay r;if(!replay_init(&r,&owner->conventions->concrete->materialization,image,w)||!compute_states(&r,w,s->work)){replay_free(&r);return false;}const SolMirMaterialization*m=r.m;for(size_t b=0;b<r.blocks;b++){if(!r.known[b])continue;size_t mark=w->used;IndependentStorage *state=workspace_take(w,r.locals,sizeof*state);unsigned char*holes=workspace_take(w,r.places,sizeof*holes);size_t*tmp=workspace_take(w,r.width,sizeof*tmp),*tmpo=workspace_take(w,r.width,sizeof*tmpo),*snap=workspace_take(w,r.width,sizeof*snap),*scope=workspace_take(w,r.width,sizeof*scope),*region=workspace_take(w,r.width,sizeof*region),*handler=workspace_take(w,r.width,sizeof*handler);if((r.locals&&!state)||(r.places&&!holes)||!tmp||!tmpo||!snap||!scope||!region||!handler){w->used=mark;replay_free(&r);return false;}if(r.locals)memcpy(state,st(&r,b),r.locals*sizeof*state);if(r.places)memcpy(holes,ho(&r,b),r.places);size_t td=r.td[b],sd=r.sd[b],cd=r.cd[b],rd=r.rd[b],hd=r.hd[b];memcpy(tmp,ss(r.temps,&r,b),td*sizeof*tmp);memcpy(tmpo,ss(r.temp_scopes,&r,b),td*sizeof*tmpo);memcpy(snap,ss(r.snaps,&r,b),sd*sizeof*snap);memcpy(scope,ss(r.scopes,&r,b),cd*sizeof*scope);memcpy(region,ss(r.regions,&r,b),rd*sizeof*region);memcpy(handler,ss(r.handlers,&r,b),hd*sizeof*handler);const SolMirMaterializedBlock*bl=&m->blocks[r.image->blocks.offset+b];bool ok=true;for(size_t i=0;i<bl->instructions.count&&ok;i++){size_t id=bl->instructions.offset+i;const SolMirMaterializedInstruction*in=&m->instructions[id];ok=!s->step_pass||emit_step(s,owner,&r,SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION,r.image->blocks.offset+b,id,in->span,state,holes,tmp,tmpo,td,snap,sd,scope,cd,region,rd,handler,hd);if(ok&&!s->step_pass&&implicit_arithmetic(owner->conventions->concrete,id))ok=emit_implicit(s,owner,&r,r.image->blocks.offset+b,id,in->span,false,state,holes,tmp,tmpo,td,snap,sd,scope,cd,region,rd,handler,hd);if(ok&&!s->step_pass&&(allocation_instruction(owner->values,m,in)||(in->kind==SOL_MIR_INST_PATTERN_VALUE&&allocation_pattern_instruction(owner->values,&owner->conventions->concrete->operations,id))))ok=emit_implicit(s,owner,&r,r.image->blocks.offset+b,id,in->span,true,state,holes,tmp,tmpo,td,snap,sd,scope,cd,region,rd,handler,hd);if(ok&&!s->step_pass)ok=emit_cleanup_instruction(s,owner,&r,r.image->blocks.offset+b,id,in,state,holes);if(ok)ok=apply_instruction(&r,in,state,holes,tmp,tmpo,&td,snap,&sd,scope,&cd,region,&rd,handler,&hd);}if(ok&&s->step_pass)ok=emit_step(s,owner,&r,SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR,r.image->blocks.offset+b,SOL_MIR_RUNTIME_NONE,bl->terminator.span,state,holes,tmp,tmpo,td,snap,sd,scope,cd,region,rd,handler,hd);if(ok&&!s->step_pass)ok=pre_operation_event(s,owner,&r,r.image->blocks.offset+b,&bl->terminator,state,holes,tmp,tmpo,td,snap,sd,scope,cd,region,rd,handler,hd);if(ok&&!s->step_pass)ok=emit_term(s,owner,&r,r.image->blocks.offset+b,&bl->terminator,state,holes,tmp,tmpo,td,snap,sd,scope,cd,region,rd,handler,hd);w->used=mark;if(!ok){replay_free(&r);return false;}}replay_free(&r);return true;}
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
static bool emit_predicate_step(IndependentSink*s,const SolMirRuntimeCleanup*owner,
    SolMirRuntimeCleanupEventKind kind,size_t body,size_t block,size_t operation,
    SolMirRuntimeSource source){
    size_t site=step_site(owner->conventions,SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP,
        body,block,operation);
    if(site==SOL_MIR_RUNTIME_NONE)return false;
    SolMirRuntimeCleanupEvent event={kind,SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP,
        SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT,body,block,operation,
        SOL_MIR_RUNTIME_NONE,source,site,SOL_MIR_RUNTIME_NONE,{action_pos(s),0},
        {s->count.transitions,0},SOL_MIR_RUNTIME_CLEANUP_PRODUCER_STEP_METER,true,
        SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE};
    size_t id=event_pos(s),at=action_pos(s);
    return sink_event(s,&event)
        && sink_transition(s,id,SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL,SOL_MIR_RUNTIME_NONE,at)
        && sink_transition(s,id,SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE,SOL_MIR_RUNTIME_NONE,at)
        && sink_finish_event(s,id);
}
static bool emit_predicates(IndependentSink *s, const SolMirRuntimeCleanup *o) {
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
            if (s->step_pass) {
                if (!emit_predicate_step(s,o,
                        SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION,
                        pb->body,block,instruction,src)) return false;
                continue;
            }
            bool allocation = in->kind == SOL_MIR_PREDICATE_INST_CONSTRUCT
                && in->recipe < o->values->allocation_plan_count
                && o->values->allocation_plans[in->recipe].kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE;
            bool fallible = in->failures != 0 || allocation;
            SolMirRuntimeCleanupEvent e = {SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION,
                SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION, fallible ? SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT : SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT,
                pb->body, block, instruction, SOL_MIR_RUNTIME_NONE, src,
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
            if (!sink_finish_event(s, id)) return false;
        }
        const SolMirPredicateTerminator *term = &pb->terminator;
        if (s->step_pass) {
            if (!emit_predicate_step(s,o,
                    SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR,
                    pb->body,block,SOL_MIR_RUNTIME_NONE,src)) return false;
            continue;
        }
        bool terminal_failure = term->kind == SOL_MIR_PREDICATE_TERM_FAILURE;
        SolMirRuntimeCleanupEvent e = {SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR,
            SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION, terminal_failure ? SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT : SOL_MIR_RUNTIME_CLEANUP_ORIGIN_EXPLICIT,
            pb->body, block, SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE, src,
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
        if (!sink_finish_event(s, id)) return false;
    }
    return true;
}
static bool scan(IndependentSink*s,const SolMirRuntimeCleanup*o,ValidationWorkspace*w,
    size_t work_limit, bool *work_exhausted){
    size_t *saved_work = scan_meter_work, saved_limit = scan_meter_limit;
    bool saved_failed = scan_meter_failed;
    scan_meter_work = s->work; scan_meter_limit = work_limit;
    scan_meter_failed = false;
    bool ok = true;
    for (unsigned pass = 0; pass < 2 && ok; ++pass) {
        s->step_pass = pass != 0;
        for(size_t i=0;i<o->conventions->concrete->materialization.image_count;i++){
            if(!METER()){ok=false;break;}
             w->used=0; if(w->capacity)memset(w->bytes,0,w->capacity);
            if(!emit_image(s,o,i,w)){ok=false;break;}
        }
        if(ok) ok=emit_predicates(s,o);
    }
    s->step_pass = false;
    bool failed = scan_meter_failed;
    scan_meter_work = saved_work; scan_meter_limit = saved_limit;
    scan_meter_failed = saved_failed;
    if (work_exhausted != NULL) *work_exhausted = failed;
    return ok && !failed;
}

static bool independent_scratch_take(size_t *used, size_t *peak, size_t count, size_t size) {
    size_t bytes, at, align = _Alignof(max_align_t);
    if (!mul(count, size, &bytes) || *used > SIZE_MAX - (align - 1)) return false;
    at = (*used + align - 1) & ~(align - 1);
    if (bytes > SIZE_MAX - at) return false;
    *used = at + bytes;
    if (*used > *peak) *peak = *used;
    return true;
}
static bool independent_scratch_bytes(const SolMirRuntimeCleanup *cleanup, size_t *out) {
    size_t peak = 0;
    const SolMirMaterialization *m = &cleanup->conventions->concrete->materialization;
    for (size_t i = 0; i < m->image_count; ++i) {
        const SolMirMaterializedImage *im = &m->images[i];
        size_t n = im->blocks.count, width = im->instructions.count, used = 0, mark;
        if (width < im->temporaries.count) width = im->temporaries.count;
        if (width < im->handlers.count) width = im->handlers.count;
        if (!width) width = 1;
        size_t nl, np, nw;
        if (!mul(n, im->locals.count, &nl) || !mul(n, im->places.count, &np)
            || !mul(n, width, &nw)) return false;
        if ((nl && !independent_scratch_take(&used, &peak, nl, sizeof(IndependentStorage)))
            || (np && !independent_scratch_take(&used, &peak, np, sizeof(unsigned char)))) return false;
        for (size_t q = 0; q < 6; ++q)
            if (!independent_scratch_take(&used, &peak, nw, sizeof(size_t))) return false;
        for (size_t q = 0; q < 5; ++q)
            if (!independent_scratch_take(&used, &peak, n, sizeof(size_t))) return false;
        if (!independent_scratch_take(&used, &peak, n, sizeof(unsigned char))) return false;
        mark = used;
        if (!independent_scratch_take(&used, &peak, im->locals.count, sizeof(IndependentStorage))
            || !independent_scratch_take(&used, &peak, im->places.count, sizeof(unsigned char))) return false;
        for (size_t q = 0; q < 6; ++q)
            if (!independent_scratch_take(&used, &peak, width, sizeof(size_t))) return false;
        used = mark;
    }
    *out = peak;
    return true;
}
static bool independent_usage(const SolMirRuntimeCleanup *cleanup,
    SolMirRuntimeCleanupUsage *usage) {
    size_t bytes = 0, x, scratch;
#define BYTES(count, pointer) do { \
    if (!mul((count), sizeof(*(pointer)), &x) || !add(&bytes, x)) return false; \
} while (0)
    BYTES(cleanup->event_count, cleanup->events);
    BYTES(cleanup->action_count, cleanup->actions);
    BYTES(cleanup->transition_count, cleanup->transitions);
    BYTES(cleanup->supplemental_site_count, cleanup->supplemental_sites);
    BYTES(cleanup->drop_path_count, cleanup->drop_paths);
#undef BYTES
    if (!independent_scratch_bytes(cleanup, &scratch)) return false;
    *usage = (SolMirRuntimeCleanupUsage){.events=cleanup->event_count,.actions=cleanup->action_count,
        .transitions=cleanup->transition_count,.supplemental_sites=cleanup->supplemental_site_count,
        .drop_paths=cleanup->drop_path_count,.owned_bytes=bytes,.build_scratch_bytes=scratch,
        .validation_scratch_bytes=scratch};
    return true;
}
static bool independent_replay_matches(const SolMirRuntimeCleanup *cleanup,
    ValidationWorkspace *workspace, SolMirRuntimeCleanupUsage *usage,
    size_t initial_validation_work, bool *work_exhausted) {
    /* Reconstruct all three builder phases with this file's replay and
     * counters.  The first two are the census and emission-prediction passes;
     * the third is the write pass and therefore enumerates drop holes.  Do not
     * use the published owner usage as an input: either a smaller or larger
     * in-limit ledger must be rejected. */
    IndependentSink census = {.owner = cleanup, .check_drop_paths = false};
    census.work = &census.count.build_work;
    bool exhausted = false;
    if (!scan(&census, cleanup, workspace, cleanup->limits.max_build_work, &exhausted)
        || census.have_event || census.event_index != cleanup->event_count
        || census.action_index != cleanup->action_count
        || census.transition_index != cleanup->transition_count
        || census.supplemental_index != cleanup->supplemental_site_count) {
        if (work_exhausted != NULL) *work_exhausted = exhausted;
        return false;
    }
    workspace->used = 0;
    IndependentSink prediction = {.owner = cleanup, .check_drop_paths = false};
    prediction.work = &prediction.count.build_work;
    if (!scan(&prediction, cleanup, workspace, cleanup->limits.max_build_work, &exhausted)
        || prediction.have_event || prediction.event_index != cleanup->event_count
        || prediction.action_index != cleanup->action_count
        || prediction.transition_index != cleanup->transition_count
        || prediction.supplemental_index != cleanup->supplemental_site_count) {
        if (work_exhausted != NULL) *work_exhausted = exhausted;
        return false;
    }
    workspace->used = 0;
    IndependentSink write = {.owner = cleanup, .check_drop_paths = true};
    write.work = &write.count.build_work;
    if (!scan(&write, cleanup, workspace, cleanup->limits.max_build_work, &exhausted)
        || write.have_event || write.event_index != cleanup->event_count
        || write.action_index != cleanup->action_count
        || write.transition_index != cleanup->transition_count
        || write.supplemental_index != cleanup->supplemental_site_count
        || census.count.build_work < cleanup->event_count) {
        if (work_exhausted != NULL) *work_exhausted = exhausted;
        return false;
    }
    usage->build_work = census.count.build_work;
    if (!add(&usage->build_work, prediction.count.build_work)
        || !add(&usage->build_work, write.count.build_work)) return false;

    /* Validation itself performs two complete owner comparisons.  Its budget
     * remains independently reconstructed rather than inferred from build. */
    workspace->used = 0;
    size_t combined_validation_work=initial_validation_work;
    size_t first_validation_work=combined_validation_work;
    IndependentSink sink = {.owner = cleanup, .check_drop_paths = true};
    sink.work = &combined_validation_work;
    if (!scan(&sink, cleanup, workspace, cleanup->limits.max_validation_work, &exhausted)
        || sink.have_event || sink.event_index != cleanup->event_count
        || sink.action_index != cleanup->action_count
        || sink.transition_index != cleanup->transition_count
        || sink.supplemental_index != cleanup->supplemental_site_count
        || combined_validation_work < first_validation_work
        || combined_validation_work-first_validation_work < cleanup->event_count) {
        if (work_exhausted != NULL) *work_exhausted = exhausted;
        return false;
    }
    workspace->used = 0;
    IndependentSink validation_prediction = {.owner = cleanup, .check_drop_paths = true};
    validation_prediction.work = &combined_validation_work;
    if (!scan(&validation_prediction, cleanup, workspace,
            cleanup->limits.max_validation_work, &exhausted) || validation_prediction.have_event
        || validation_prediction.event_index != cleanup->event_count
        || validation_prediction.action_index != cleanup->action_count
        || validation_prediction.transition_index != cleanup->transition_count
        || validation_prediction.supplemental_index != cleanup->supplemental_site_count) {
        if (work_exhausted != NULL) *work_exhausted = exhausted;
        return false;
    }
    usage->validation_work = combined_validation_work;
    if (work_exhausted != NULL) *work_exhausted = false;
    return true;
}

static SolMirRuntimeCleanupEdgeRole validation_role(const SolMirRuntimeCleanupEvent *event,
    const SolMirRuntimeCleanupTransition *transition, const SolMirMaterialization *m,
    const SolMirOperations *ops, size_t ordinal) {
    if (event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP
        && transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE)
        return SOL_MIR_RUNTIME_CLEANUP_EDGE_STEP_FAILURE;
    if (event->phase != SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
        && transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL)
        return SOL_MIR_RUNTIME_CLEANUP_EDGE_PRE_OPERATION_READY;
    if (transition->continuation == SOL_MIR_RUNTIME_NONE)
        return transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_EXIT
            ? SOL_MIR_RUNTIME_CLEANUP_EDGE_RETURN
            : transition->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_NORMAL
                ? SOL_MIR_RUNTIME_CLEANUP_EDGE_GOTO
                : SOL_MIR_RUNTIME_CLEANUP_EDGE_TERMINAL_FAILURE;
    if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        && event->block < m->block_count) switch (m->blocks[event->block].terminator.kind) {
        case SOL_MIR_TERM_BRANCH:
            return transition->continuation == m->blocks[event->block].terminator.true_edge
                ? SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_TRUE : SOL_MIR_RUNTIME_CLEANUP_EDGE_BRANCH_FALSE;
        case SOL_MIR_TERM_INVOKE:
            return transition->continuation == m->blocks[event->block].terminator.normal_edge
                ? SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_NORMAL : SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE;
        case SOL_MIR_TERM_CHECK_REFINED:
            if (ordinal == 0 && transition->continuation
                    == m->blocks[event->block].terminator.normal_edge)
                return SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_SATISFIED;
            return ordinal == 1 && transition->continuation
                    == m->blocks[event->block].terminator.failure_edge
                ? SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_VIOLATION
                : SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE;
        case SOL_MIR_TERM_PROPAGATE:
            return transition->continuation == m->blocks[event->block].terminator.value_edge
                ? SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_VALUE : SOL_MIR_RUNTIME_CLEANUP_EDGE_PROPAGATE_RESIDUAL;
        case SOL_MIR_TERM_CHECK_CONTRACT:
            return transition->continuation == m->blocks[event->block].terminator.satisfied_edge
                ? SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_SATISFIED
                : transition->continuation == m->blocks[event->block].terminator.violation_edge
                    ? SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION
                    : SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_FAILURE;
        default: break;
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
static size_t validation_predicate_body_for_check(const SolMirOperations *ops,
    const SolMirRuntimeCleanupEvent *event) {
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR) return SOL_MIR_RUNTIME_NONE;
    for (size_t i = 0; i < ops->predicate_count; ++i)
        if (ops->predicates[i].image == event->owner && ops->predicates[i].block == event->block)
            return ops->predicates[i].body;
    return SOL_MIR_RUNTIME_NONE;
}
static SolMirRuntimeFailureSiteId validation_predicate_result_site(
    const SolMirRuntimeConventions *conventions, size_t body) {
    for (size_t i = 0; i < conventions->failure_site_count; ++i)
        if (conventions->failure_sites[i].origin_kind
                == SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT
            && conventions->failure_sites[i].owner == body) return i;
    return SOL_MIR_RUNTIME_NONE;
}
static bool validation_in(SolMirPlanSlice slice, size_t id, size_t count) {
    return slice.offset <= count && slice.count <= count - slice.offset
        && id >= slice.offset && id - slice.offset < slice.count;
}
/* P3.3 permits unequal callable recipe IDs only for the already materialized,
 * exact unbound field route.  Reconstruct it here instead of treating type or
 * arena identity as a coercion proof. */
static bool validation_callable_hole_signature(const SolMirConcreteProgram *concrete,
    const SolMirRuntimeConventions *conventions, const SolMirRuntimeSignature *signature, SolMirRecipeId producer,
    SolMirRecipeId field, SolMirRecipeId load, SolMirRecipeId callee) {
    const SolMirRepresentation *r = &concrete->representation;
    const SolMirRecipeId all[] = {producer, field, load, callee};
    if (signature->function_recipe != callee || signature->slots.offset > conventions->signature_slot_count
        || signature->slots.count > conventions->signature_slot_count - signature->slots.offset
        || producer >= r->recipe_count) return false;
    const SolMirRecipe *exact = &r->recipes[producer];
    if (exact->kind != SOL_MIR_RECIPE_FUNCTION || exact->storage != SOL_MIR_STORAGE_CALLABLE_HANDLE
        || exact->is_copy || exact->copy_kind != SOL_MIR_COPY_FORBIDDEN
        || exact->parameters.count != exact->parameter_accesses.count
        || exact->result >= r->recipe_count || exact->effects >= r->materialization->effect_row_count
        || signature->slots.count != exact->parameters.count || signature->result != exact->result
        || signature->effects != exact->effects) return false;
    for (size_t i = 1; i < sizeof(all) / sizeof(*all); ++i) {
        if (all[i] >= r->recipe_count) return false;
        const SolMirRecipe *candidate = &r->recipes[all[i]];
        if (candidate->kind != SOL_MIR_RECIPE_FUNCTION
            || candidate->storage != SOL_MIR_STORAGE_CALLABLE_HANDLE || candidate->is_copy
            || candidate->copy_kind != SOL_MIR_COPY_FORBIDDEN
            || candidate->parameters.count != exact->parameters.count
            || candidate->parameter_accesses.count != exact->parameter_accesses.count
            || candidate->result != exact->result || candidate->effects != exact->effects) return false;
    }
    for (size_t i = 0; i < exact->parameters.count; ++i) {
        size_t parameter = exact->parameters.offset + i;
        size_t access = exact->parameter_accesses.offset + i;
        size_t slot = signature->slots.offset + i;
        if (parameter >= r->recipe_id_count || access >= r->access_count
            || slot >= conventions->signature_slot_count
            || conventions->signature_slots[slot].role != SOL_MIR_RUNTIME_SLOT_PARAMETER
            || conventions->signature_slots[slot].formal != i
            || conventions->signature_slots[slot].recipe != r->recipe_ids[parameter]
            || conventions->signature_slots[slot].access != r->accesses[access]) return false;
        for (size_t j = 1; j < sizeof(all) / sizeof(*all); ++j) {
            const SolMirRecipe *candidate = &r->recipes[all[j]];
            size_t candidate_parameter = candidate->parameters.offset + i;
            size_t candidate_access = candidate->parameter_accesses.offset + i;
            if (candidate_parameter >= r->recipe_id_count || candidate_access >= r->access_count
                || r->recipe_ids[candidate_parameter] != r->recipe_ids[parameter]
                || r->accesses[candidate_access] != r->accesses[access]) return false;
        }
    }
    return true;
}
static bool validation_callable_hole_route(const SolMirConcreteProgram *concrete,
    const SolMirRuntimeConventions *conventions,
    size_t image_id, size_t block, const SolMirMaterializedTerminator *term,
    const SolMirMaterializedSemanticSite *site, const SolMirOperationCallablePlan *plan,
    const SolMirRuntimeSignature *signature) {
    const SolMirMaterialization *m = &concrete->materialization;
    if (image_id >= m->image_count || block >= m->block_count || term->binding >= m->binding_count
        || site->binding >= m->binding_count || site->parent != image_id
        || site->kind != SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE
        || site->producer_kind != SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION) return false;
    const SolMirMaterializedImage *image = &m->images[image_id];
    if (!validation_in(image->temporaries, term->callee, m->temporary_count)
        || !validation_in(image->instructions, site->instruction, m->instruction_count)
        || site->block != m->instructions[site->instruction].block
        || block != site->block) return false;
    const SolMirMaterializedBinding *function = &m->bindings[site->binding];
    const SolMirMaterializedBinding *invoke = &m->bindings[term->binding];
    const SolMirMaterializedInstruction *producer = &m->instructions[site->instruction];
    if (producer->kind != SOL_MIR_INST_FUNCTION_VALUE
        || producer->result == SOL_MIR_MATERIALIZED_NONE
        || producer->source_expression != function->source.expression
        || producer->function_callable != function->symbolic_callable
        || producer->type != site->produced_function_type
        || function->target_kind != SOL_MIR_MATERIALIZED_TARGET_INSTANCE
        || function->target_kind != invoke->target_kind || function->instance != invoke->instance
        || function->import != invoke->import || function->instance >= m->plan->instance_count
        || m->plan->instances[function->instance].callable != producer->function_callable
        || plan->target_kind != function->target_kind || plan->target_instance != function->instance
        || plan->target_import != function->import || plan->function_recipe != producer->type) return false;
    const SolIr *ir = m->plan->program->ir;
    if (producer->function_callable >= ir->callable_count
        || ir->callables[producer->function_callable].kind != SOL_IR_CALLABLE_FUNCTION
        || ir->callables[producer->function_callable].generic_parameters.count != 0
        || ir->callables[producer->function_callable].effect_parameters.count != 0
        || ir->callables[producer->function_callable].effect_parameter != SOL_IR_NONE
        || m->plan->instances[function->instance].receiver != SOL_MIR_PLAN_NONE
        || site->captured_receiver_kind != SOL_MIR_MATERIALIZED_RECEIVER_NONE
        || site->captured_receiver_type != SOL_MIR_MATERIALIZED_NONE
        || site->captured_receiver_roots.count != 0) return false;
    size_t initializer = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = image->instructions.offset; i < image->instructions.offset + image->instructions.count; ++i)
        if (m->instructions[i].kind == SOL_MIR_INST_TEMPORARY_INIT
            && m->instructions[i].temporary == term->callee) {
            if (initializer != SOL_MIR_MATERIALIZED_NONE) return false;
            initializer = i;
        }
    if (initializer == SOL_MIR_MATERIALIZED_NONE || m->instructions[initializer].left >= m->value_count
        || m->values[m->instructions[initializer].left].kind != SOL_MIR_VALUE_INSTRUCTION) return false;
    size_t callback_load = m->values[m->instructions[initializer].left].instruction;
    if (!validation_in(image->instructions, callback_load, m->instruction_count)) return false;
    size_t load_id = callback_load, callback_store = SOL_MIR_MATERIALIZED_NONE;
    const SolMirMaterializedInstruction *load = &m->instructions[load_id];
    if ((load->kind != SOL_MIR_INST_LOAD_COPY && load->kind != SOL_MIR_INST_LOAD_MOVE)
        || load->result != m->instructions[initializer].left || !validation_in(image->places, load->place, m->place_count)) return false;
    if (m->places[load->place].projections.count == 0) {
        size_t store = SOL_MIR_MATERIALIZED_NONE;
        for (size_t i = image->instructions.offset; i < image->instructions.offset + image->instructions.count; ++i)
            if (m->instructions[i].kind == SOL_MIR_INST_STORE
                && validation_in(image->places, m->instructions[i].place, m->place_count)
                && m->places[m->instructions[i].place].local == m->places[load->place].local) {
                if (store != SOL_MIR_MATERIALIZED_NONE) return false;
                store = i;
            }
        if (store == SOL_MIR_MATERIALIZED_NONE || m->instructions[store].left >= m->value_count
            || m->values[m->instructions[store].left].kind != SOL_MIR_VALUE_INSTRUCTION) return false;
        load_id = m->values[m->instructions[store].left].instruction;
        if (!validation_in(image->instructions, load_id, m->instruction_count)) return false;
        load = &m->instructions[load_id]; callback_store = store;
    }
    if (load->kind != SOL_MIR_INST_LOAD_MOVE || load->result == SOL_MIR_MATERIALIZED_NONE
        || m->places[load->place].projections.count != 1) return false;
    size_t projection = m->places[load->place].projections.offset;
    if (projection >= m->projection_count || m->projections[projection].kind != SOL_IR_PROJECTION_FIELD) return false;
    size_t pair_local = m->places[load->place].local;
    if (pair_local >= m->local_count || m->locals[pair_local].source_local >= ir->local_count
        || ir->locals[m->locals[pair_local].source_local].kind != SOL_IR_LOCAL_BINDING
        || ir->locals[m->locals[pair_local].source_local].mutable) return false;
    size_t pair_store = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = image->instructions.offset; i < image->instructions.offset + image->instructions.count; ++i)
        if (m->instructions[i].kind == SOL_MIR_INST_STORE && validation_in(image->places,
                m->instructions[i].place, m->place_count)
            && m->places[m->instructions[i].place].local == pair_local) {
            if (pair_store != SOL_MIR_MATERIALIZED_NONE) return false;
            pair_store = i;
        }
    if (pair_store == SOL_MIR_MATERIALIZED_NONE) return false;
    size_t construct_id = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = image->instructions.offset; i < image->instructions.offset + image->instructions.count; ++i)
        if (m->instructions[i].kind == SOL_MIR_INST_CONSTRUCT
            && m->instructions[i].result == m->instructions[pair_store].left) {
            if (construct_id != SOL_MIR_MATERIALIZED_NONE) return false;
            construct_id = i;
        }
    if (construct_id == SOL_MIR_MATERIALIZED_NONE) return false;
    const SolMirMaterializedInstruction *construct = &m->instructions[construct_id];
    if (construct->construct_kind != SOL_MIR_CONSTRUCT_RECORD
        || construct->type != m->places[load->place].root_type) return false;
    size_t operand = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = 0; i < construct->construct_operands.count; ++i) {
        size_t id = construct->construct_operands.offset + i;
        if (id >= m->construct_operand_count || m->construct_operands[id].formal != m->projections[projection].source_field) continue;
        if (operand != SOL_MIR_MATERIALIZED_NONE) return false;
        operand = id;
    }
    if (operand == SOL_MIR_MATERIALIZED_NONE) return false;
    size_t operand_init = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = image->instructions.offset; i < image->instructions.offset + image->instructions.count; ++i)
        if (m->instructions[i].kind == SOL_MIR_INST_TEMPORARY_INIT
            && m->instructions[i].temporary == m->construct_operands[operand].temporary) {
            if (operand_init != SOL_MIR_MATERIALIZED_NONE) return false;
            operand_init = i;
        }
    if (operand_init == SOL_MIR_MATERIALIZED_NONE || m->instructions[operand_init].left >= m->value_count
        || m->values[m->instructions[operand_init].left].kind != SOL_MIR_VALUE_INSTRUCTION) return false;
    size_t exact_load = m->values[m->instructions[operand_init].left].instruction;
    if (!validation_in(image->instructions, exact_load, m->instruction_count)) return false;
    const SolMirMaterializedInstruction *exact = &m->instructions[exact_load];
    if (exact->kind != SOL_MIR_INST_LOAD_MOVE || exact->result != m->instructions[operand_init].left
        || !validation_in(image->places, exact->place, m->place_count)
        || m->places[exact->place].projections.count != 0) return false;
    size_t exact_local = m->places[exact->place].local, exact_store = SOL_MIR_MATERIALIZED_NONE;
    if (exact_local >= m->local_count || m->locals[exact_local].source_local >= ir->local_count
        || ir->locals[m->locals[exact_local].source_local].kind != SOL_IR_LOCAL_BINDING
        || ir->locals[m->locals[exact_local].source_local].mutable) return false;
    for (size_t i = image->instructions.offset; i < image->instructions.offset + image->instructions.count; ++i)
        if (m->instructions[i].kind == SOL_MIR_INST_STORE && validation_in(image->places,
                m->instructions[i].place, m->place_count)
            && m->places[m->instructions[i].place].local == exact_local) {
            if (exact_store != SOL_MIR_MATERIALIZED_NONE || m->instructions[i].left != producer->result) return false;
            exact_store = i;
        }
    return exact_store != SOL_MIR_MATERIALIZED_NONE
        && validation_callable_hole_signature(concrete, conventions, signature, producer->type,
            m->construct_operands[operand].type, load->type, m->temporaries[term->callee].type)
        && producer->block == block && m->instructions[exact_store].block == block
        && exact->block == block && m->instructions[operand_init].block == block
        && construct->block == block && m->instructions[pair_store].block == block
        && load->block == block && m->instructions[initializer].block == block
        && site->instruction < exact_store && exact_store < exact_load && exact_load < operand_init
        && operand_init < construct_id && construct_id < pair_store && pair_store < load_id
        && (callback_store == SOL_MIR_MATERIALIZED_NONE
            || (load_id < callback_store && callback_store < callback_load))
        && callback_load < initializer;
}
static bool validation_local_or_pending_call(const SolMirRuntimeCleanup *cleanup,
    const SolMirRuntimeCleanupEvent *event, const SolMirRuntimeCleanupTransition *transition) {
    const SolMirMaterialization *m = &cleanup->conventions->concrete->materialization;
    const SolMirConcreteProgram *concrete = cleanup->conventions->concrete;
    if (event->inherited_failure_site >= cleanup->conventions->call_count
        || transition->outcome != SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE) return false;
    if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR) {
        const SolMirOperations *operations = &concrete->operations;
        if (event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_INVOKE
            || event->block >= operations->predicate_block_count) return false;
        const SolMirPredicateTerminator *term
            = &operations->predicate_blocks[event->block].terminator;
        const SolMirRuntimeCall *call
            = &cleanup->conventions->calls[event->inherited_failure_site];
        if (term->kind != SOL_MIR_PREDICATE_TERM_INVOKE
            || call->failure_site != event->inherited_failure_site
            || call->owner_kind != SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE
            || call->predicate != event->owner || call->block != event->block
            || call->call_kind != term->call_kind
            || call->normal_edge != term->normal_edge || call->failure_edge != term->failure_edge
            || transition->continuation != term->failure_edge) return false;
        if (call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL)
            return true;
        if (call->target_kind != SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE
            || call->table >= concrete->linkage.table_entry_count
            || call->signature >= cleanup->conventions->signature_count) return false;
        const SolMirLinkageTableEntry *entry
            = &concrete->linkage.table_entries[call->table];
        const SolMirRuntimeSignature *signature
            = &cleanup->conventions->signatures[call->signature];
        return entry->target_kind == SOL_MIR_LINKAGE_TARGET_INTERNAL
            && signature->origin == SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE
            && cleanup->conventions->failure_sites[event->inherited_failure_site].allowed_codes
                == failbit(SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT);
    }
    if (event->kind != SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
        || event->producer != SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE
        || event->block >= m->block_count)
        return false;
    const SolMirMaterializedTerminator *term = &m->blocks[event->block].terminator;
    const SolMirRuntimeCall *call = &cleanup->conventions->calls[event->inherited_failure_site];
    if (!(term->kind == SOL_MIR_TERM_INVOKE && call->failure_site == event->inherited_failure_site
        && call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE && call->image == event->owner
        && call->block == event->block
        && call->normal_edge == term->normal_edge && call->failure_edge == term->failure_edge
        && transition->continuation == term->failure_edge)) return false;
    if (call->call_kind == term->call_kind
        && call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL)
        return call->target_kind == SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL;
    if (call->call_kind != SOL_IR_CALL_CALLBACK || term->call_kind != SOL_IR_CALL_CALLBACK
        || call->target_kind != SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE) return false;
    if (call->table >= concrete->linkage.table_entry_count
        || call->signature >= cleanup->conventions->signature_count
        || term->callable_site >= m->semantic_site_count) return false;
    const SolMirLinkageTableEntry *entry = &concrete->linkage.table_entries[call->table];
    const SolMirRuntimeSignature *signature = &cleanup->conventions->signatures[call->signature];
    if (entry->target_kind != SOL_MIR_LINKAGE_TARGET_INTERNAL
        || signature->origin != SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE
        || cleanup->conventions->failure_sites[event->inherited_failure_site].allowed_codes
            != failbit(SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT)) return false;
    size_t found = 0;
    for (size_t i = 0; i < concrete->operations.callable_count; ++i) {
        const SolMirOperationCallablePlan *plan = &concrete->operations.callables[i];
        if (plan->semantic_site != term->callable_site || i >= concrete->linkage.callable_value_count
            || concrete->linkage.callable_values[i].table != call->table) continue;
        if (plan->target_kind != SOL_MIR_MATERIALIZED_TARGET_INSTANCE) return false;
        if (plan->function_recipe != signature->function_recipe
            && !validation_callable_hole_route(concrete, cleanup->conventions, event->owner,
                event->block, term, &m->semantic_sites[term->callable_site], plan, signature))
            return false;
        size_t target = SOL_MIR_RUNTIME_NONE;
        for (size_t q = 0; q < concrete->linkage.callable_count; ++q)
            if (concrete->linkage.callables[q].instance == plan->target_instance) {
                if (target != SOL_MIR_RUNTIME_NONE) return false;
                target = q;
            }
        if (target == SOL_MIR_RUNTIME_NONE || entry->internal != target) return false;
        ++found;
    }
    return found == 1;
}
static bool validation_metadata(const SolMirRuntimeCleanup *cleanup) {
    const SolMirMaterialization *m = &cleanup->conventions->concrete->materialization;
    const SolMirOperations *ops = &cleanup->conventions->concrete->operations;
    for (size_t i = 0; i < cleanup->event_count; ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[i];
        SolMirRuntimeCleanupProducerKind producer = event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_STEP
            ? SOL_MIR_RUNTIME_CLEANUP_PRODUCER_STEP_METER
            : event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_INVOKE_CALLABLE
            ? SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CALLABLE_CONSTRUCTION
            : event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_PRE_PROPAGATE_RESIDUAL
                ? SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PROPAGATION_RESIDUAL
            : event->origin == SOL_MIR_RUNTIME_CLEANUP_ORIGIN_IMPLICIT
            ? (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
                ? SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_ARITHMETIC
                : SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_ARITHMETIC)
            : SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL;
        if (event->supplemental_site != SOL_MIR_RUNTIME_NONE
            && event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION)
            producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION;
        if (event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
            && event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
            && event->block < m->block_count) {
            switch (m->blocks[event->block].terminator.kind) {
                case SOL_MIR_TERM_INVOKE: producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE; break;
                case SOL_MIR_TERM_PANIC: producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_PANIC; break;
                case SOL_MIR_TERM_MATCH_FAILURE: producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_NO_MATCH; break;
                case SOL_MIR_TERM_UNREACHABLE: producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_UNREACHABLE; break;
                default: break;
            }
        }
        if (event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
            && event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR
            && event->block < ops->predicate_block_count) {
            const SolMirPredicateTerminator *term = &ops->predicate_blocks[event->block].terminator;
            if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE)
                producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_INVOKE;
            else if (term->kind == SOL_MIR_PREDICATE_TERM_FAILURE
                && term->failure_kind == SOL_MIR_PREDICATE_FAILURE_NO_MATCH)
                producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_NO_MATCH;
            else if (term->kind == SOL_MIR_PREDICATE_TERM_RETURN)
                producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_RESULT;
        }
        if (event->producer != producer) return false;
        for (size_t j = 0; j < event->transitions.count; ++j) {
            const SolMirRuntimeCleanupTransition *t = &cleanup->transitions[event->transitions.offset + j];
            uint32_t mask = 0;
            SolMirRuntimeCleanupFailureSource failure_source =
                SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_NONE;
            SolContractClauseKind phase = SOL_CONTRACT_REQUIRES;
            SolContractOutcomeKind outcome = SOL_CONTRACT_OUTCOME_SUCCESS;
            if (event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
                && event->block < m->block_count) {
                phase = m->blocks[event->block].terminator.contract_phase;
                outcome = m->blocks[event->block].terminator.contract_outcome;
            } else if ((event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
                    || event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR)
                && event->owner < ops->predicate_body_count) {
                phase = ops->predicate_bodies[event->owner].phase;
                outcome = ops->predicate_bodies[event->owner].outcome;
            }
            bool predicate = event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION
                || event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR;
            size_t destination = t->continuation == SOL_MIR_RUNTIME_NONE ? SOL_MIR_RUNTIME_NONE
                : predicate && t->continuation < ops->predicate_edge_count
                    ? ops->predicate_edges[t->continuation].target
                    : !predicate && t->continuation < m->edge_count
                        ? m->edges[t->continuation].block : SOL_MIR_RUNTIME_NONE;
            size_t failure_site = SOL_MIR_RUNTIME_NONE;
            if (t->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
                && event->inherited_failure_site != SOL_MIR_RUNTIME_NONE) {
                failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31;
                failure_site = event->inherited_failure_site;
                mask = cleanup->conventions->failure_sites[failure_site].allowed_codes;
                if (validation_local_or_pending_call(cleanup, event, t))
                    failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_LOCAL_OR_PENDING;
            } else if (t->outcome == SOL_MIR_RUNTIME_CLEANUP_OUTCOME_FAILURE
                && event->supplemental_site != SOL_MIR_RUNTIME_NONE) {
                failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33;
                failure_site = event->supplemental_site;
                mask = cleanup->supplemental_sites[failure_site].allowed_codes;
            }
            SolMirRuntimeCleanupEdgeRole role = validation_role(event, t, m, ops, j);
            if (role == SOL_MIR_RUNTIME_CLEANUP_EDGE_CONTRACT_VIOLATION) {
                size_t body = validation_predicate_body_for_check(ops, event);
                SolMirRuntimeFailureSiteId result = validation_predicate_result_site(cleanup->conventions, body);
                if (body != SOL_MIR_RUNTIME_NONE && result != SOL_MIR_RUNTIME_NONE) {
                    failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31;
                    failure_site = result;
                    mask = cleanup->conventions->failure_sites[result].allowed_codes;
                }
            }
            if (role == SOL_MIR_RUNTIME_CLEANUP_EDGE_REFINED_FAILURE) {
                failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING;
                failure_site = SOL_MIR_RUNTIME_NONE;
                mask = 0;
            }
            if (event->phase == SOL_MIR_RUNTIME_CLEANUP_PHASE_AT_OPERATION
                && event->kind == SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR
                && event->block < m->block_count
                && m->blocks[event->block].terminator.kind == SOL_MIR_TERM_RESUME_FAILURE) {
                failure_source = SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_PENDING;
                failure_site = SOL_MIR_RUNTIME_NONE;
                mask = 0;
            }
            if (t->source_edge != t->continuation || t->destination != destination
                || t->edge_role != role
                || t->failure_source != failure_source || t->failure_site != failure_site
                || t->failure_mask != mask || t->contract_phase != phase
                || t->contract_outcome != outcome) return false;
        }
    }
    return true;
}

SolMirRuntimeCleanupBuildOutcome sol_mir_runtime_cleanup_internal_validate(
    const SolMirRuntimeCleanup *cleanup, SolDiagnostics *diagnostics) {
    if (cleanup == NULL || cleanup->conventions == NULL || cleanup->values == NULL
        || cleanup->values->conventions != cleanup->conventions || !validation_full_limits(cleanup->limits)) {
        validation_bad(diagnostics, "runtime cleanup owner is invalid");
        return SOL_MIR_RUNTIME_CLEANUP_BUILD_INVALID_PREDECESSOR;
    }
    /* No cleanup range is dereferenced until its raw shape is valid. */
    if (cleanup->event_count != cleanup->event_capacity || cleanup->action_count != cleanup->action_capacity
        || cleanup->transition_count != cleanup->transition_capacity
        || cleanup->supplemental_site_count != cleanup->supplemental_site_capacity
        || cleanup->drop_path_count != cleanup->drop_path_capacity
        || !validation_range(cleanup->events, cleanup->event_capacity, sizeof(*cleanup->events))
        || !validation_range(cleanup->actions, cleanup->action_capacity, sizeof(*cleanup->actions))
        || !validation_range(cleanup->transitions, cleanup->transition_capacity, sizeof(*cleanup->transitions))
        || !validation_range(cleanup->supplemental_sites, cleanup->supplemental_site_capacity,
            sizeof(*cleanup->supplemental_sites))
        || !validation_range(cleanup->drop_paths, cleanup->drop_path_capacity,
            sizeof(*cleanup->drop_paths))) {
        validation_bad(diagnostics, "runtime cleanup arena range is invalid");
        return SOL_MIR_RUNTIME_CLEANUP_BUILD_INTERNAL_FAILED;
    }
    /* The raw shared census structurally checks borrowed descriptors and
     * rejects local overlap before either predecessor authentication or local
     * typed-record traversal. */
    size_t alias_work=0;
    alias_meter_work=&alias_work; alias_meter_limit=cleanup->limits.max_validation_work;
    alias_meter_exhausted=false;
    bool alias_invalid=validation_predecessor_alias(cleanup);
    alias_meter_work=NULL;
    if (alias_meter_exhausted) {
        validation_bad(diagnostics, "runtime cleanup validation work limit exceeded");
        return SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED;
    }
    if (alias_invalid) {
        validation_bad(diagnostics, "runtime cleanup predecessor or alias is invalid");
        return SOL_MIR_RUNTIME_CLEANUP_BUILD_INVALID_PREDECESSOR;
    }
    if (!sol_mir_runtime_conventions_validate(cleanup->conventions, diagnostics)
        || !sol_mir_runtime_values_validate(cleanup->values, diagnostics)) {
        validation_bad(diagnostics, "runtime cleanup predecessor is invalid");
        return SOL_MIR_RUNTIME_CLEANUP_BUILD_INVALID_PREDECESSOR;
    }
    /* Validate every slice before replay dereferences an action, transition, or path. */
    for (size_t i = 0; i < cleanup->event_count; ++i)
        if (cleanup->events[i].actions.offset > cleanup->action_count
            || cleanup->events[i].actions.count > cleanup->action_count - cleanup->events[i].actions.offset
            || cleanup->events[i].transitions.offset > cleanup->transition_count
            || cleanup->events[i].transitions.count > cleanup->transition_count - cleanup->events[i].transitions.offset) {
            validation_bad(diagnostics, "runtime cleanup event slice is invalid");
            return SOL_MIR_RUNTIME_CLEANUP_BUILD_INTERNAL_FAILED;
        }
    for (size_t i = 0; i < cleanup->transition_count; ++i)
        if (cleanup->transitions[i].actions.offset > cleanup->action_count
            || cleanup->transitions[i].actions.count > cleanup->action_count - cleanup->transitions[i].actions.offset) {
            validation_bad(diagnostics, "runtime cleanup transition slice is invalid");
            return SOL_MIR_RUNTIME_CLEANUP_BUILD_INTERNAL_FAILED;
        }
    for (size_t i = 0; i < cleanup->action_count; ++i) {
        const SolMirRuntimeCleanupAction *a = &cleanup->actions[i];
        if ((a->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE
                || a->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER)
            ? a->drop_path >= cleanup->drop_path_count : a->drop_path != SOL_MIR_RUNTIME_NONE) {
            validation_bad(diagnostics, "runtime cleanup action path is invalid");
            return SOL_MIR_RUNTIME_CLEANUP_BUILD_INTERNAL_FAILED;
        }
    }
    for (size_t i = 0; i < cleanup->drop_path_count; ++i)
        if (cleanup->drop_paths[i].holes.offset > cleanup->drop_path_count
            || cleanup->drop_paths[i].holes.count > cleanup->drop_path_count - cleanup->drop_paths[i].holes.offset) {
            validation_bad(diagnostics, "runtime cleanup drop path slice is invalid");
            return SOL_MIR_RUNTIME_CLEANUP_BUILD_INTERNAL_FAILED;
        }
    SolMirRuntimeCleanupUsage expected_usage;
    if (!independent_usage(cleanup, &expected_usage)
        || expected_usage.validation_scratch_bytes > cleanup->limits.max_validation_scratch_bytes) {
        validation_bad(diagnostics, "runtime cleanup validation resource limit exceeded");
        return SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED;
    }
    if (cleanup->usage.validation_scratch_bytes > cleanup->limits.max_validation_scratch_bytes) {
        validation_bad(diagnostics, "runtime cleanup validation resource limit exceeded");
        return SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED;
    }
    void *scratch = sol_mir_runtime_cleanup_internal_validation_scratch_allocate(
        cleanup->usage.validation_scratch_bytes);
    if (cleanup->usage.validation_scratch_bytes != 0 && scratch == NULL) {
        if (diagnostics != NULL) diagnostics->allocation_failed = true;
        validation_bad(diagnostics, "runtime cleanup validation scratch allocation failed");
        return SOL_MIR_RUNTIME_CLEANUP_BUILD_ALLOCATION_FAILED;
    }
    ValidationWorkspace workspace = {(unsigned char *)scratch,
        cleanup->usage.validation_scratch_bytes, 0};
    bool work_exhausted = false;
    bool matches = independent_replay_matches(cleanup, &workspace, &expected_usage,
        alias_work, &work_exhausted);
    if (matches) {
        const SolMirOperations *ops = &cleanup->conventions->concrete->operations;
        for (size_t i = 0; i < cleanup->action_count; ++i) {
            const SolMirRuntimeCleanupAction *action = &cleanup->actions[i];
            if (action->kind == SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT
                && (action->target >= ops->snapshot_count
                    || action->recipe != ops->snapshots[action->target].recipe)) {
                matches = false;
                break;
            }
        }
    }
    if (matches && !validation_metadata(cleanup)) matches = false;
    sol_mir_runtime_cleanup_internal_validation_scratch_free(scratch);
    if (work_exhausted) {
        validation_bad(diagnostics, "runtime cleanup validation work limit exceeded");
        return SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED;
    }
    if (cleanup->usage.build_work > cleanup->limits.max_build_work
        || cleanup->usage.validation_work > cleanup->limits.max_validation_work) {
        validation_bad(diagnostics, "runtime cleanup validation resource limit exceeded");
        return SOL_MIR_RUNTIME_CLEANUP_BUILD_RESOURCE_EXHAUSTED;
    }
    if (!matches || memcmp(&expected_usage, &cleanup->usage, sizeof(expected_usage)) != 0) {
        validation_bad(diagnostics, "runtime cleanup CFG replay does not match owner arenas");
        return SOL_MIR_RUNTIME_CLEANUP_BUILD_INTERNAL_FAILED;
    }
    return SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;
}

bool sol_mir_runtime_cleanup_validate(const SolMirRuntimeCleanup *cleanup,
    SolDiagnostics *diagnostics) {
    return sol_mir_runtime_cleanup_internal_validate(cleanup, diagnostics)
        == SOL_MIR_RUNTIME_CLEANUP_BUILD_SUCCEEDED;
}
