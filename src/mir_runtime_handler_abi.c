#include "sol/mir_runtime_handler_abi.h"
#include "mir_runtime_handler_abi_internal.h"
#include "mir_linkage_internal.h"

#include <stdlib.h>
#include <string.h>

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static _Thread_local bool fail_build_scratch, fail_persistent, fail_validation;
static _Thread_local size_t allocation_attempts;
void sol_mir_runtime_handler_abi_test_force_build_scratch_allocation_failure(bool x) { fail_build_scratch=x; }
void sol_mir_runtime_handler_abi_test_force_persistent_allocation_failure(bool x) { fail_persistent=x; }
void sol_mir_runtime_handler_abi_test_force_validation_scratch_failure(bool x) { fail_validation=x; }
size_t sol_mir_runtime_handler_abi_test_allocation_attempts(void) { return allocation_attempts; }
void sol_mir_runtime_handler_abi_test_reset_allocation_attempts(void) { allocation_attempts=0; }
bool sol_mir_runtime_handler_abi_internal_test_validation_failure(void) { return fail_validation; }
#endif

static bool add(size_t *a,size_t b) { if(b>SIZE_MAX-*a)return false; *a+=b; return true; }
static bool mul(size_t a,size_t b,size_t *r) { if(a&&b>SIZE_MAX/a)return false; *r=a*b; return true; }
static bool build_tick(size_t *work,size_t limit) { return add(work,1)&&*work<=limit; }
static uint64_t mix64(uint64_t x,uint64_t y) { return (x^y)+UINT64_C(0x9e3779b97f4a7c15)+(x<<6)+(x>>2); }
static bool report(SolDiagnostics*d,const char*m) { if(d)sol_diagnostics_add(d,"SOL-MIR-RUNTIME-HANDLER-ABI-001",SOL_SEVERITY_ERROR,(SolSpan){0},m); return false; }
static bool zero(SolMirRuntimeHandlerAbiLimits x) { return !x.max_frames&&!x.max_marker_references&&!x.max_cleanup_exits&&!x.max_stack_depth&&!x.max_test_work&&!x.max_owned_bytes&&!x.max_build_scratch_bytes&&!x.max_build_work&&!x.max_validation_scratch_bytes&&!x.max_validation_work; }
static bool full(SolMirRuntimeHandlerAbiLimits x) { return x.max_frames&&x.max_marker_references&&x.max_cleanup_exits&&x.max_stack_depth&&x.max_test_work&&x.max_owned_bytes&&x.max_build_scratch_bytes&&x.max_build_work&&x.max_validation_scratch_bytes&&x.max_validation_work; }
static bool empty(const SolMirRuntimeHandlerAbi *o) { SolMirRuntimeHandlerAbiUsage u={0}; return o&&!o->conventions&&!o->values&&!o->cleanup&&!o->host_abi&&!o->frames&&!o->exit_markers&&!o->cleanup_exits&&!o->frame_count&&!o->exit_marker_count&&!o->cleanup_exit_count&&!o->frame_capacity&&!o->exit_marker_capacity&&!o->cleanup_exit_capacity&&zero(o->limits)&&!memcmp(&o->usage,&u,sizeof u)&&!o->authentication; }

void sol_mir_runtime_handler_abi_init(SolMirRuntimeHandlerAbi*o) { if(o)memset(o,0,sizeof *o); }
void sol_mir_runtime_handler_abi_free(SolMirRuntimeHandlerAbi*o) { if(!o)return; free(o->frames); free(o->exit_markers); free(o->cleanup_exits); sol_mir_runtime_handler_abi_init(o); }
SolMirRuntimeHandlerAbiLimits sol_mir_runtime_handler_abi_default_limits(void) { return (SolMirRuntimeHandlerAbiLimits){4000000,16000000,16000000,4000000,(size_t)4000000000ULL,1024u*1024u*1024u,256u*1024u*1024u,(size_t)4000000000ULL,256u*1024u*1024u,(size_t)4000000000ULL}; }

static void *allocate(size_t n,size_t z,bool scratch) {
    if(!n||n>SIZE_MAX/z)return NULL;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    ++allocation_attempts;
    if((scratch&&fail_build_scratch)||(!scratch&&fail_persistent))return NULL;
#else
    (void)scratch;
#endif
    return calloc(n,z);
}
void *sol_mir_runtime_handler_abi_internal_validation_scratch(size_t n,size_t z) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    ++allocation_attempts;
    if (fail_validation) return NULL;
#endif
    return n && z && n <= SIZE_MAX / z ? calloc(n,z) : NULL;
}
static bool operation_equal(const SolMirMaterializedOperationKey*a,const SolMirMaterializedOperationKey*b) { return a->target_kind==b->target_kind&&a->instance==b->instance&&a->import==b->import&&a->receiver==b->receiver&&a->root==b->root&&a->effects==b->effects; }
static bool operation_dispatch_equal(const SolMirMaterializedOperationKey*a,const SolMirMaterializedOperationKey*b) { return a->target_kind==b->target_kind&&a->instance==b->instance&&a->import==b->import&&a->receiver==b->receiver&&a->effects==b->effects; }
static bool source_for(const SolIr*ir,SolSpan span,SolMirRuntimeSource*out,size_t *work,size_t limit) { bool found=false,unique=true;SolMirRuntimeSource source={0};if(!ir||!out||span.start>span.end||span.end>ir->source_length)return false; for(size_t i=0;i<ir->file_count;i++){if(!build_tick(work,limit))return false;const SolIrSourceFile*f=&ir->files[i];if(span.start>=f->aggregate_start&&span.end<=f->aggregate_end){if(found)unique=false;else source=(SolMirRuntimeSource){i,span.start-f->aggregate_start,span.end-f->aggregate_start};found=true;}}if(found&&unique)*out=source;return found&&unique; }
static bool linkage_for(const SolMirRuntimeConventions*c,SolMirMaterializedBindingId binding,SolMirLinkageTargetKind *kind,size_t *target,size_t *work,size_t limit) { const SolMirLinkage*l=&c->concrete->linkage;size_t found=SOL_MIR_RUNTIME_NONE;for(size_t i=0;i<l->binding_count;i++){if(!build_tick(work,limit))return false;if(l->bindings[i].binding==binding){if(found!=SOL_MIR_RUNTIME_NONE)return false;found=i;}}if(found==SOL_MIR_RUNTIME_NONE)return false;*kind=l->bindings[found].target_kind;*target=*kind==SOL_MIR_LINKAGE_TARGET_INTERNAL?l->bindings[found].internal:l->bindings[found].host;return true; }
static bool signature_for(const SolMirRuntimeConventions*c,SolMirLinkageTargetKind kind,size_t target,SolMirRuntimeSignatureId*out,size_t *work,size_t limit) { size_t found=SOL_MIR_RUNTIME_NONE;for(size_t i=0;i<c->signature_count;i++){if(!build_tick(work,limit))return false;const SolMirRuntimeSignature*s=&c->signatures[i];bool yes=kind==SOL_MIR_LINKAGE_TARGET_INTERNAL?s->origin==SOL_MIR_RUNTIME_SIGNATURE_INTERNAL&&s->internal==target:s->origin==SOL_MIR_RUNTIME_SIGNATURE_HOST&&s->host==target;if(yes){if(found!=SOL_MIR_RUNTIME_NONE)return false;found=i;}}if(found==SOL_MIR_RUNTIME_NONE)return false;*out=found;return true; }
static bool enter_for(const SolMirMaterialization*m,size_t handler,size_t*enter,size_t *work,size_t limit) { *enter=SOL_MIR_RUNTIME_NONE;for(size_t i=0;i<m->instruction_count;i++){if(!build_tick(work,limit))return false;const SolMirMaterializedInstruction*x=&m->instructions[i];if(x->handler==handler&&x->kind==SOL_MIR_INST_HANDLER_ENTER){if(*enter!=SOL_MIR_RUNTIME_NONE)return false;*enter=i;}}return *enter!=SOL_MIR_RUNTIME_NONE; }
static bool transition_for_action(const SolMirRuntimeCleanup*c,size_t action,size_t*out,size_t *work,size_t limit) { size_t found=SOL_MIR_RUNTIME_NONE;for(size_t t=0;t<c->transition_count;t++){if(!build_tick(work,limit))return false;const SolMirRuntimeCleanupTransition*x=&c->transitions[t];if(action<x->actions.offset||action-x->actions.offset>=x->actions.count)continue;if(found!=SOL_MIR_RUNTIME_NONE)return false;found=t;}if(found==SOL_MIR_RUNTIME_NONE)return false;*out=found;return true; }
static bool usage(SolMirRuntimeHandlerAbi*o,size_t build_scratch,size_t build_work) { size_t a,b=0;if(!mul(o->frame_count,sizeof *o->frames,&a)||!add(&b,a)||!mul(o->exit_marker_count,sizeof *o->exit_markers,&a)||!add(&b,a)||!mul(o->cleanup_exit_count,sizeof *o->cleanup_exits,&a)||!add(&b,a))return false;o->usage=(SolMirRuntimeHandlerAbiUsage){o->frame_count,o->frame_count+o->exit_marker_count+o->cleanup_exit_count,o->cleanup_exit_count,o->frame_count,o->frame_count,b,build_scratch,build_work,0,0};return o->usage.frames<=o->limits.max_frames&&o->usage.marker_references<=o->limits.max_marker_references&&o->usage.cleanup_exits<=o->limits.max_cleanup_exits&&o->usage.stack_depth<=o->limits.max_stack_depth&&o->usage.test_work<=o->limits.max_test_work&&o->usage.owned_bytes<=o->limits.max_owned_bytes&&o->usage.build_scratch_bytes<=o->limits.max_build_scratch_bytes&&o->usage.build_work<=o->limits.max_build_work; }
/* Count the exact write pass before allocating persistent owner storage. */
static bool predicted_write_work(const SolMirRuntimeConventions*c,const SolMirRuntimeCleanup*cleanup,size_t *out) { const SolMirMaterialization*m=&c->concrete->materialization;const SolMirOperations*ops=&c->concrete->operations;size_t work=0,binding_scans,signature_scans;if(!mul(c->concrete->linkage.binding_count,2,&binding_scans)||!mul(c->signature_count,2,&signature_scans))return false;for(size_t i=0;i<ops->handler_count;i++){if(!add(&work,1)||!add(&work,m->instruction_count)||!add(&work,binding_scans)||!add(&work,signature_scans)||!add(&work,m->plan->program->ir->file_count)||!add(&work,m->instruction_count)||!add(&work,cleanup->action_count))return false;for(size_t a=0;a<cleanup->action_count;a++)if(cleanup->actions[a].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_HANDLER&&cleanup->actions[a].target==ops->handlers[i].handler&&!add(&work,cleanup->transition_count))return false;}*out=work;return true; }
static bool predicted_build_work(const SolMirRuntimeConventions*c,const SolMirRuntimeCleanup*cleanup,size_t *out) { const SolMirMaterialization*m=&c->concrete->materialization;const SolMirOperations*ops=&c->concrete->operations;size_t work=0,write_work=0,markers=0,exits=0,seal_work=0;for(size_t i=0;i<ops->handler_count;i++){if(!add(&work,1)||!add(&work,m->instruction_count)||!add(&work,cleanup->action_count))return false;for(size_t q=0;q<m->instruction_count;q++)if(m->instructions[q].handler==ops->handlers[i].handler&&m->instructions[q].kind==SOL_MIR_INST_HANDLER_EXIT&&!add(&markers,1))return false;for(size_t a=0;a<cleanup->action_count;a++)if(cleanup->actions[a].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_HANDLER&&cleanup->actions[a].target==ops->handlers[i].handler&&!add(&exits,1))return false;}SolMirRuntimeHandlerAbi owner={.frame_count=ops->handler_count,.exit_marker_count=markers,.cleanup_exit_count=exits};return predicted_write_work(c,cleanup,&write_work)&&sol_mir_runtime_handler_abi_internal_seal_work(&owner,&seal_work)&&add(&work,write_work)&&add(&work,seal_work)&&(*out=work,true); }
#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_runtime_handler_abi_test_predict_build_work(const SolMirRuntimeConventions*c,const SolMirRuntimeCleanup*cleanup,size_t*out) { return c&&cleanup&&out&&predicted_build_work(c,cleanup,out); }
#endif

SolMirRuntimeHandlerAbiBuildOutcome sol_mir_runtime_handler_abi_build(const SolMirRuntimeHandlerAbiBuildRequest*r,SolMirRuntimeHandlerAbi*out,SolDiagnostics*d) {
    if(!r||!out||!r->conventions||!r->values||!r->cleanup||!r->host_abi||!empty(out)||(r->limits&&!zero(*r->limits)&&!full(*r->limits))){report(d,"invalid runtime handler ABI build request or destination");return SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_INVALID_ARGUMENT;}
    if(r->values->conventions!=r->conventions||r->cleanup->conventions!=r->conventions||r->cleanup->values!=r->values||r->host_abi->conventions!=r->conventions||r->host_abi->values!=r->values||r->host_abi->cleanup!=r->cleanup||!sol_mir_runtime_conventions_validate(r->conventions,d)||!sol_mir_runtime_values_validate(r->values,d)||!sol_mir_runtime_cleanup_validate(r->cleanup,d)||!sol_mir_runtime_host_abi_validate(r->host_abi,d)){report(d,"runtime handler ABI predecessors are invalid");return SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_INVALID_PREDECESSOR;}
    SolMirRuntimeHandlerAbi x;sol_mir_runtime_handler_abi_init(&x);x.conventions=r->conventions;x.values=r->values;x.cleanup=r->cleanup;x.host_abi=r->host_abi;x.limits=!r->limits||zero(*r->limits)?sol_mir_runtime_handler_abi_default_limits():*r->limits;
    const SolMirMaterialization*m=&r->conventions->concrete->materialization;const SolMirOperations*ops=&r->conventions->concrete->operations;
    size_t exits=0,markers=0,work=0,scratch=0,predicted_total=0;size_t *seen=NULL;
    if(!mul(ops->handler_count,sizeof *seen,&scratch))goto exhausted;
    if(!predicted_build_work(r->conventions,r->cleanup,&predicted_total)||predicted_total>x.limits.max_build_work)goto exhausted;
    for(size_t i=0;i<ops->handler_count;i++){
        if(!build_tick(&work,x.limits.max_build_work))goto exhausted;
        if(i>=m->handler_count)goto invalid;
        for(size_t q=0;q<m->instruction_count;q++){if(!build_tick(&work,x.limits.max_build_work))goto exhausted;if(m->instructions[q].handler==ops->handlers[i].handler&&m->instructions[q].kind==SOL_MIR_INST_HANDLER_EXIT&&!add(&markers,1))goto exhausted;}
        for(size_t a=0;a<r->cleanup->action_count;a++){if(!build_tick(&work,x.limits.max_build_work))goto exhausted;if(r->cleanup->actions[a].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_HANDLER&&r->cleanup->actions[a].target==ops->handlers[i].handler&&!add(&exits,1))goto exhausted;}
    }
    x.frame_count=x.frame_capacity=ops->handler_count;x.exit_marker_count=x.exit_marker_capacity=markers;x.cleanup_exit_count=x.cleanup_exit_capacity=exits;
    if(work>predicted_total||!usage(&x,scratch,predicted_total)||x.usage.marker_references>x.limits.max_marker_references)goto exhausted;
    if(ops->handler_count){seen=allocate(ops->handler_count,sizeof *seen,true);if(!seen)goto allocation;}
    x.frames=allocate(x.frame_count,sizeof *x.frames,false);x.exit_markers=allocate(x.exit_marker_count,sizeof *x.exit_markers,false);x.cleanup_exits=allocate(x.cleanup_exit_count,sizeof *x.cleanup_exits,false);
    if((x.frame_count&&!x.frames)||(x.exit_marker_count&&!x.exit_markers)||(x.cleanup_exit_count&&!x.cleanup_exits))goto allocation;
    x.frame_count=x.exit_marker_count=x.cleanup_exit_count=0;
    for(size_t i=0;i<ops->handler_count;i++){
        if(!build_tick(&work,x.limits.max_build_work))goto exhausted;
        const SolMirOperationHandlerPlan*p=&ops->handlers[i];
        if(p->handler>=m->handler_count||seen[p->handler]||(p->frame_parent!=SOL_MIR_OPERATION_NONE&&p->frame_parent>=i)||p->root_match!=SOL_MIR_OPERATION_ROOT_TOKEN_EQUAL){report(d,"handler frame order");goto invalid;}
        seen[p->handler]=1;const SolMirMaterializedHandler*h=&m->handlers[p->handler];SolMirLinkageTargetKind sk,pk;size_t st,pt,enter;
        if(p->source_binding!=h->source_binding||p->provider_binding!=h->provider_binding||!operation_equal(&p->operation,&h->operation)||p->effects!=h->operation.effects){report(d,"handler P2 fields");goto invalid;}
        if(!enter_for(m,p->handler,&enter,&work,x.limits.max_build_work)){report(d,"handler enter");goto invalid;}
        if(!linkage_for(r->conventions,h->source_binding,&sk,&st,&work,x.limits.max_build_work)){report(d,"handler source linkage");goto invalid;}
        if(!linkage_for(r->conventions,h->provider_binding,&pk,&pt,&work,x.limits.max_build_work)||pk!=SOL_MIR_LINKAGE_TARGET_INTERNAL||h->provider_binding>=m->binding_count||m->bindings[h->provider_binding].target_kind!=SOL_MIR_MATERIALIZED_TARGET_INSTANCE){report(d,"handler provider must be bodyful internal");goto invalid;}
        SolMirRuntimeSignatureId ss,ps;SolMirRuntimeSource source;
        if((sk!=SOL_MIR_LINKAGE_TARGET_HOST&&sk!=SOL_MIR_LINKAGE_TARGET_INTERNAL)
            ||!signature_for(r->conventions,sk,st,&ss,&work,x.limits.max_build_work)
            ||(sk==SOL_MIR_LINKAGE_TARGET_HOST
                &&(st>=r->host_abi->operation_count
                    ||r->host_abi->operations[st].host!=st
                    ||r->host_abi->operations[st].signature!=ss
                    ||r->host_abi->operations[st].effects!=h->operation.effects))
            ||(sk==SOL_MIR_LINKAGE_TARGET_INTERNAL
                &&(st>=r->conventions->concrete->linkage.callable_count
                    ||r->conventions->signatures[ss].origin!=SOL_MIR_RUNTIME_SIGNATURE_INTERNAL))
            ||!signature_for(r->conventions,pk,pt,&ps,&work,x.limits.max_build_work)||r->conventions->signatures[ps].origin!=SOL_MIR_RUNTIME_SIGNATURE_INTERNAL||r->conventions->signatures[ps].effects>=m->effect_row_count||m->effect_rows[r->conventions->signatures[ps].effects].atoms.count||h->provider>=m->place_count||m->places[h->provider].final_type>=r->conventions->concrete->representation.recipe_count||!source_for(r->conventions->concrete->program.ir,h->span,&source,&work,x.limits.max_build_work)){report(d,"handler source/provider P3 provenance");goto invalid;}
        size_t marker_start=x.exit_marker_count,cleanup_start=x.cleanup_exit_count;
        for(size_t q=0;q<m->instruction_count;q++){if(!build_tick(&work,x.limits.max_build_work))goto exhausted;const SolMirMaterializedInstruction*z=&m->instructions[q];if(z->handler==p->handler&&z->kind==SOL_MIR_INST_HANDLER_EXIT)x.exit_markers[x.exit_marker_count++]=(SolMirRuntimeHandlerExitMarker){i,q,p->image,z->block};}
        if(x.exit_marker_count==marker_start)goto invalid;
        for(size_t a=0;a<r->cleanup->action_count;a++){if(!build_tick(&work,x.limits.max_build_work))goto exhausted;if(r->cleanup->actions[a].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_EXIT_HANDLER&&r->cleanup->actions[a].target==p->handler){size_t t;if(!transition_for_action(r->cleanup,a,&t,&work,x.limits.max_build_work))goto invalid;x.cleanup_exits[x.cleanup_exit_count++]=(SolMirRuntimeHandlerCleanupExit){i,a,t};}}
        x.frames[x.frame_count++]=(SolMirRuntimeHandlerFramePlan){p->handler,source,h->source_binding,h->operation,ss,p->root_match,h->authority,h->operation.effects,h->provider_binding,h->provider,m->places[h->provider].final_type,p->provider_access,pt,ps,enter,{marker_start,x.exit_marker_count-marker_start},{cleanup_start,x.cleanup_exit_count-cleanup_start},p->frame_parent==SOL_MIR_OPERATION_NONE?SOL_MIR_RUNTIME_NONE:p->frame_parent};
    }
    free(seen);seen=NULL;
    if(x.frame_count!=x.frame_capacity||x.exit_marker_count!=x.exit_marker_capacity||x.cleanup_exit_count!=x.cleanup_exit_capacity)goto invalid;
    size_t seal_work=0;
    if(!sol_mir_runtime_handler_abi_internal_seal_work(&x,&seal_work)
        ||!add(&work,seal_work)||work!=predicted_total||work>x.limits.max_build_work
        ||!usage(&x,scratch,work))goto exhausted;
    x.usage.validation_work=0;x.usage.validation_scratch_bytes=scratch;
    if(x.usage.validation_scratch_bytes>x.limits.max_validation_scratch_bytes)goto exhausted;
    /* Measurement does not compare authentication; the live seal below
     * authenticates the completed usage, including its measured work. */
    x.authentication=1;
    SolMirRuntimeHandlerAbiBuildOutcome measured
        =sol_mir_runtime_handler_abi_internal_measure_validation_work(&x,
            &x.usage.validation_work,d);
    if(measured!=SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED){
        if(measured==SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_ALLOCATION_FAILED)goto allocation;
        if(measured==SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_RESOURCE_EXHAUSTED)goto exhausted;
        goto invalid;
    }
    SolMirRuntimeHandlerAbiWorkMeter seal_meter={x.limits.max_build_work,
        work-seal_work,false};
    uint64_t authentication=0;
    if(!sol_mir_runtime_handler_abi_internal_seal_metered(&x,&seal_meter,
            &authentication)||seal_meter.used!=work)goto exhausted;
    x.authentication=authentication;
    SolMirRuntimeHandlerAbiBuildOutcome valid=sol_mir_runtime_handler_abi_internal_validate(&x,d);
    if(valid!=SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED){if(valid==SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_ALLOCATION_FAILED)goto allocation;goto invalid;}
    *out=x;return SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED;
invalid: free(seen);sol_mir_runtime_handler_abi_free(&x);report(d,"runtime handler ABI relation is malformed");return SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_INVALID_PREDECESSOR;
exhausted: free(seen);sol_mir_runtime_handler_abi_free(&x);report(d,"runtime handler ABI resource limit exceeded");return SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_RESOURCE_EXHAUSTED;
allocation: free(seen);sol_mir_runtime_handler_abi_free(&x);if(d)d->allocation_failed=true;report(d,"runtime handler ABI allocation failed");return SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_ALLOCATION_FAILED;
}

bool sol_mir_runtime_handler_abi_validate(const SolMirRuntimeHandlerAbi*o,SolDiagnostics*d) { return sol_mir_runtime_handler_abi_internal_validate(o,d)==SOL_MIR_RUNTIME_HANDLER_ABI_BUILD_SUCCEEDED; }
typedef struct { char text[768]; } Line;
static int compare_line(const void*a,const void*b) { return strcmp(((const Line*)a)->text,((const Line*)b)->text); }
static void hex_digest(char out[65],const SolMirLinkageDigest*d) { static const char h[]="0123456789abcdef";for(size_t i=0;i<32;i++){out[i*2]=h[d->bytes[i]>>4];out[i*2+1]=h[d->bytes[i]&15u];}out[64]=0; }
static bool key_recipe(const SolMirRuntimeHandlerAbi*o,SolMirRecipeId r,char out[65]) { SolMirLinkageDigest d;SolMirLinkageWorkMeter w={0,SIZE_MAX,false};if(!sol_mir_linkage_internal_recipe_key(&o->conventions->concrete->linkage,r,&d,&w))return false;hex_digest(out,&d);return true; }
static uint64_t stable_bytes(uint64_t x,const void *p,size_t n) { const unsigned char *b=p;for(size_t i=0;i<n;i++)x=mix64(x,b[i]);return x; }
static bool instance_digest(const SolMirRuntimeHandlerAbi*o,size_t instance,SolMirLinkageDigest*out) { const SolMirLinkage*l=&o->conventions->concrete->linkage;size_t found=SOL_MIR_RUNTIME_NONE;for(size_t i=0;i<l->callable_count;i++)if(l->callables[i].instance==instance){if(found!=SOL_MIR_RUNTIME_NONE)return false;found=i;}if(found==SOL_MIR_RUNTIME_NONE)return false;*out=l->callables[found].instance_key;return true; }
static uint64_t digest_key(const SolMirLinkageDigest*d) { return stable_bytes(UINT64_C(0x696e7374616e6365),d->bytes,sizeof d->bytes); }
/* Places are keyed by their owning callable instance, local semantic role, and
 * projection ordinal sequence.  Source-field IDs are intentionally absent. */
static uint64_t place_key(const SolMirRuntimeHandlerAbi*o,size_t place) { const SolMirMaterialization*m=&o->conventions->concrete->materialization;if(place>=m->place_count)return 0;const SolMirMaterializedPlace*p=&m->places[place];if(p->local>=m->local_count||p->projections.offset>m->projection_count||p->projections.count>m->projection_count-p->projections.offset)return 0;const SolMirMaterializedLocal*l=&m->locals[p->local];SolMirLinkageDigest owner;if(!instance_digest(o,l->instance,&owner))return 0;uint64_t x=digest_key(&owner);x=mix64(x,l->kind);x=mix64(x,l->ordinal);for(size_t i=0;i<p->projections.count;i++){const SolMirMaterializedProjection*q=&m->projections[p->projections.offset+i];x=mix64(x,q->kind);x=mix64(x,q->tuple_ordinal);}return x; }
static uint64_t effect_key(const SolMirRuntimeHandlerAbi*o,size_t effects,uint64_t root) { const SolMirMaterialization*m=&o->conventions->concrete->materialization;if(effects>=m->effect_row_count)return 0;const SolMirMaterializedEffectRow*r=&m->effect_rows[effects];if(r->atoms.offset>m->effect_row_atom_count||r->atoms.count>m->effect_row_atom_count-r->atoms.offset)return 0;uint64_t x=mix64(UINT64_C(0x6566666563746b79),root);for(size_t i=0;i<r->atoms.count;i++){size_t id=m->effect_row_atoms[r->atoms.offset+i];if(id>=m->effect_atom_count)return 0;const SolMirMaterializedEffectAtom*a=&m->effect_atoms[id];if(a->name.offset>m->effect_name_count||a->name.count>m->effect_name_count-a->name.offset)return 0;x=mix64(x,a->authority);x=mix64(x,a->ordinal);x=stable_bytes(x,m->effect_names+a->name.offset,a->name.count);}return x; }
static uint64_t signature_key(const SolMirRuntimeHandlerAbi*o,size_t id,uint64_t root) { const SolMirRuntimeConventions*c=o->conventions;if(id>=c->signature_count)return 0;const SolMirRuntimeSignature*s=&c->signatures[id];if(s->slots.offset>c->signature_slot_count||s->slots.count>c->signature_slot_count-s->slots.offset)return 0;char recipe[65];if(!key_recipe(o,s->result,recipe))return 0;uint64_t x=stable_bytes(mix64(UINT64_C(0x7369676e61747572),s->origin),recipe,64);for(size_t i=0;i<s->slots.count;i++){const SolMirRuntimeSignatureSlot*q=&c->signature_slots[s->slots.offset+i];if(!key_recipe(o,q->recipe,recipe))return 0;x=stable_bytes(mix64(mix64(x,q->role),q->access),recipe,64);}return mix64(x,effect_key(o,s->effects,root)); }
static const SolMirRuntimeImport*source_import(const SolMirRuntimeHandlerAbi*o,size_t signature) { const SolMirRuntimeConventions*c=o->conventions;if(signature>=c->signature_count)return NULL;const SolMirRuntimeSignature*s=&c->signatures[signature];if(s->origin!=SOL_MIR_RUNTIME_SIGNATURE_HOST)return NULL;for(size_t i=0;i<c->import_count;i++)if(c->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST&&c->imports[i].host==s->host)return &c->imports[i];return NULL; }
static bool source_target_key(const SolMirRuntimeHandlerAbi*o,const SolMirRuntimeHandlerFramePlan*f,uint64_t*out,char text[65],const char **kind) { SolMirLinkageTargetKind target;size_t id,work=0;SolMirLinkageDigest digest;if(!linkage_for(o->conventions,f->source_binding,&target,&id,&work,SIZE_MAX))return false;if(target==SOL_MIR_LINKAGE_TARGET_HOST){const SolMirRuntimeImport*im=source_import(o,f->source_signature);if(!im)return false;digest=im->identity;*kind="host";}else if(target==SOL_MIR_LINKAGE_TARGET_INTERNAL){if(id>=o->conventions->concrete->linkage.callable_count)return false;digest=o->conventions->concrete->linkage.callables[id].instance_key;*kind="internal";}else return false;hex_digest(text,&digest);*out=digest_key(&digest);return *out!=0; }
static bool handler_ordinal(const SolMirRuntimeHandlerAbi*o,const SolMirRuntimeHandlerFramePlan*f,size_t*out) { const SolMirMaterialization*m=&o->conventions->concrete->materialization;if(f->handler>=m->handler_count)return false;size_t instance=m->handlers[f->handler].parent;for(size_t i=0;i<m->image_count;i++){const SolMirMaterializedImage*image=&m->images[i];if(image->instance!=instance)continue;if(f->handler<image->handlers.offset||f->handler-image->handlers.offset>=image->handlers.count)return false;*out=f->handler-image->handlers.offset;return true;}return false; }
bool sol_mir_runtime_handler_abi_render(FILE*s,const SolMirRuntimeHandlerAbi*o) { if(!s||!sol_mir_runtime_handler_abi_validate(o,NULL))return false;size_t n=1;if(!add(&n,o->frame_count)||!add(&n,o->exit_marker_count)||!add(&n,o->cleanup_exit_count)||n>SIZE_MAX/sizeof(Line))return false;Line*lines=calloc(n,sizeof *lines);uint64_t*keys=calloc(o->frame_count?o->frame_count:1,sizeof *keys);if(!lines||!keys){free(lines);free(keys);return false;}size_t at=0;(void)snprintf(lines[at++].text,sizeof lines[0].text,"runtime-handler-abi handler-abi=true handler-execution=false\n");for(size_t i=0;i<o->frame_count;i++){const SolMirRuntimeHandlerFramePlan*f=&o->frames[i];char provider[65],recipe[65],source[65];const char*source_kind;size_t ordinal;uint64_t source_key,provider_place=place_key(o,f->provider_place),authority=place_key(o,f->authority_root);if(!source_target_key(o,f,&source_key,source,&source_kind)||!provider_place||!authority||!handler_ordinal(o,f,&ordinal)||!key_recipe(o,f->provider_recipe,recipe)||f->provider_internal>=o->conventions->concrete->linkage.callable_count){free(keys);free(lines);return false;}hex_digest(provider,&o->conventions->concrete->linkage.callables[f->provider_internal].instance_key);uint64_t parent=f->parent==SOL_MIR_RUNTIME_NONE?0:keys[f->parent];keys[i]=mix64(mix64(mix64(mix64(source_key,authority),effect_key(o,f->effects,authority)),provider_place),signature_key(o,f->provider_signature,authority));keys[i]=mix64(keys[i],ordinal);keys[i]=mix64(keys[i],parent);if(snprintf(lines[at++].text,sizeof lines[0].text,"frame key=%016llx source-kind=%s source-target=%s effect=%016llx root=%016llx provider-target=%s provider-recipe=%s provider-place=%016llx provider-signature=%016llx lexical-ordinal=%zu parent=%016llx\n",(unsigned long long)keys[i],source_kind,source,(unsigned long long)effect_key(o,f->effects,authority),(unsigned long long)authority,provider,recipe,(unsigned long long)provider_place,(unsigned long long)signature_key(o,f->provider_signature,authority),ordinal,(unsigned long long)parent)>=(int)sizeof lines[0].text){free(keys);free(lines);return false;}}
    for(size_t i=0;i<o->exit_marker_count;i++){const SolMirRuntimeHandlerExitMarker*x=&o->exit_markers[i];if(x->frame>=o->frame_count){free(keys);free(lines);return false;}if(snprintf(lines[at++].text,sizeof lines[0].text,"marker frame=%016llx ordinal=%zu\n",(unsigned long long)keys[x->frame],i-o->frames[x->frame].exit_markers.offset)>=(int)sizeof lines[0].text){free(keys);free(lines);return false;}}
    for(size_t i=0;i<o->cleanup_exit_count;i++){const SolMirRuntimeHandlerCleanupExit*x=&o->cleanup_exits[i];if(x->frame>=o->frame_count){free(keys);free(lines);return false;}if(snprintf(lines[at++].text,sizeof lines[0].text,"cleanup-exit frame=%016llx ordinal=%zu\n",(unsigned long long)keys[x->frame],i-o->frames[x->frame].cleanup_exits.offset)>=(int)sizeof lines[0].text){free(keys);free(lines);return false;}}
    free(keys);if(at!=n){free(lines);return false;}qsort(lines+1,n-1,sizeof *lines,compare_line);size_t bytes=0;for(size_t i=0;i<n;i++)if(!add(&bytes,strlen(lines[i].text))){free(lines);return false;}char*b=malloc(bytes?bytes:1);if(!b){free(lines);return false;}at=0;for(size_t i=0;i<n;i++){size_t z=strlen(lines[i].text);memcpy(b+at,lines[i].text,z);at+=z;}bool ok=fwrite(b,1,bytes,s)==bytes;free(b);free(lines);return ok; }

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static uint64_t mix_stack(uint64_t x,uint64_t y) { return mix64(x,y); }
static uint64_t stack_seal(const SolMirRuntimeHandlerAbi*o,const SolMirRuntimeHandlerActivationStack*s) { uint64_t x=mix_stack(UINT64_C(0x68616e646c6572),o->authentication);x=mix_stack(x,s->count);x=mix_stack(x,s->visible_count);for(size_t i=0;i<s->count;i++){x=mix_stack(x,s->frames[i].frame);x=mix_stack(x,s->frames[i].parent);x=mix_stack(x,s->frames[i].root);x=mix_stack(x,s->frames[i].identity);}return x; }
static uint64_t activation_identity(const SolMirRuntimeHandlerAbi*o,const SolMirRuntimeHandlerFramePlan*f,uint64_t root) { uint64_t x=mix_stack(o->authentication,root);x=mix_stack(x,f->authority_root);x=mix_stack(x,f->provider_place);x=mix_stack(x,f->provider_recipe);x=mix_stack(x,f->provider_access);x=mix_stack(x,f->provider_internal);return x; }
static bool stack_valid(const SolMirRuntimeHandlerAbi*o,const SolMirRuntimeHandlerActivationStack*s) { if(!o||!s||s->count>s->capacity||s->visible_count>s->count||(s->capacity&&!s->frames)||s->count>o->limits.max_stack_depth||s->count>o->limits.max_test_work)return false;if(!s->count)return !s->seal&&s->visible_count==0;for(size_t i=0;i<s->count;i++){const SolMirRuntimeHandlerActivation*a=&s->frames[i];if(a->frame>=o->frame_count||a->parent!=o->frames[a->frame].parent||!a->root||a->identity!=activation_identity(o,&o->frames[a->frame],a->root)||(i&&a->parent!=s->frames[i-1].frame)||(!i&&a->parent!=SOL_MIR_RUNTIME_NONE))return false;}return s->seal==stack_seal(o,s); }
static uint64_t selection_seal(const SolMirRuntimeHandlerAbi*o,const SolMirRuntimeHandlerActivationStack*s,const SolMirRuntimeHandlerSelection*x) { uint64_t z=stack_seal(o,s);z=mix_stack(z,x->active_index);z=mix_stack(z,x->frame);z=mix_stack(z,x->hidden_count);return z; }
static void reseal(const SolMirRuntimeHandlerAbi*o,SolMirRuntimeHandlerActivationStack*s) { s->seal=s->count?stack_seal(o,s):0; }
void sol_mir_runtime_handler_abi_test_stack_init(SolMirRuntimeHandlerActivationStack*s,SolMirRuntimeHandlerActivation*f,size_t n) { if(s)*s=(SolMirRuntimeHandlerActivationStack){.frames=f,.capacity=n}; }
bool sol_mir_runtime_handler_abi_test_enter(const SolMirRuntimeHandlerAbi*o,SolMirRuntimeHandlerActivationStack*s,size_t frame,uint64_t root,bool provider_ok) { if(!sol_mir_runtime_handler_abi_validate(o,NULL)||!s||!provider_ok||!root||!stack_valid(o,s)||s->count!=s->visible_count||frame>=o->frame_count||s->count>=s->capacity||s->count>=o->limits.max_stack_depth)return false;size_t parent=o->frames[frame].parent;if((parent==SOL_MIR_RUNTIME_NONE&&s->count)||(parent!=SOL_MIR_RUNTIME_NONE&&(!s->count||s->frames[s->count-1].frame!=parent)))return false;const SolMirRuntimeHandlerFramePlan*f=&o->frames[frame];s->frames[s->count++]=(SolMirRuntimeHandlerActivation){frame,parent,root,activation_identity(o,f,root)};s->visible_count=s->count;reseal(o,s);return true; }
SolMirRuntimeHandlerSelectOutcome sol_mir_runtime_handler_abi_test_select(const SolMirRuntimeHandlerAbi*o,const SolMirRuntimeHandlerActivationStack*s,const SolMirRuntimeHandlerDispatch*d,SolMirRuntimeHandlerSelection*out) { if(out)*out=(SolMirRuntimeHandlerSelection){.outcome=SOL_MIR_RUNTIME_HANDLER_SELECT_INVALID,.frame=SOL_MIR_RUNTIME_NONE};if(!sol_mir_runtime_handler_abi_validate(o,NULL)||!s||!d||!d->root||!stack_valid(o,s)||s->visible_count>o->limits.max_test_work)return SOL_MIR_RUNTIME_HANDLER_SELECT_INVALID;for(size_t i=s->visible_count;i;i--){size_t id=s->frames[i-1].frame;if(id>=o->frame_count)return SOL_MIR_RUNTIME_HANDLER_SELECT_INVALID;const SolMirRuntimeHandlerFramePlan*f=&o->frames[id];if(s->frames[i-1].root==d->root&&d->signature==f->source_signature&&operation_dispatch_equal(&d->operation,&f->source_operation)){if(out){*out=(SolMirRuntimeHandlerSelection){SOL_MIR_RUNTIME_HANDLER_SELECT_MATCH,i-1,id,s->visible_count-(i-1),0};out->hidden_seal=selection_seal(o,s,out);}return SOL_MIR_RUNTIME_HANDLER_SELECT_MATCH;}}if(out)out->outcome=SOL_MIR_RUNTIME_HANDLER_SELECT_NO_MATCH;return SOL_MIR_RUNTIME_HANDLER_SELECT_NO_MATCH; }
bool sol_mir_runtime_handler_abi_test_hide(const SolMirRuntimeHandlerAbi*o,SolMirRuntimeHandlerActivationStack*s,SolMirRuntimeHandlerSelection*x) { if(!sol_mir_runtime_handler_abi_validate(o,NULL)||!s||!x||!stack_valid(o,s)||x->outcome!=SOL_MIR_RUNTIME_HANDLER_SELECT_MATCH||x->active_index>=s->visible_count||x->hidden_count!=s->visible_count-x->active_index||s->frames[x->active_index].frame!=x->frame||x->hidden_seal!=selection_seal(o,s,x))return false;s->visible_count=x->active_index;reseal(o,s);x->hidden_seal=selection_seal(o,s,x);return true; }
bool sol_mir_runtime_handler_abi_test_restore(const SolMirRuntimeHandlerAbi*o,SolMirRuntimeHandlerActivationStack*s,const SolMirRuntimeHandlerSelection*x,bool provider_succeeded) { size_t prior_visible; (void)provider_succeeded;if(!sol_mir_runtime_handler_abi_validate(o,NULL)||!s||!x||!stack_valid(o,s)||x->outcome!=SOL_MIR_RUNTIME_HANDLER_SELECT_MATCH||x->active_index>=s->count||x->hidden_count> s->count-x->active_index||s->frames[x->active_index].frame!=x->frame||s->visible_count!=x->active_index||x->hidden_seal!=selection_seal(o,s,x))return false;prior_visible=x->active_index+x->hidden_count;s->visible_count=prior_visible;reseal(o,s);return true; }
static bool pop(const SolMirRuntimeHandlerAbi*o,SolMirRuntimeHandlerActivationStack*s,size_t frame,bool marker,size_t id) { if(!sol_mir_runtime_handler_abi_validate(o,NULL)||!s||!stack_valid(o,s)||s->count!=s->visible_count||!s->count||frame>=o->frame_count||s->frames[s->count-1].frame!=frame)return false;const SolMirRuntimeHandlerFramePlan*f=&o->frames[frame];bool ok=false;for(size_t i=0;marker&&i<f->exit_markers.count;i++){size_t q=f->exit_markers.offset+i;if(q>=o->exit_marker_count)return false;ok|=o->exit_markers[q].instruction==id;}for(size_t i=0;!marker&&i<f->cleanup_exits.count;i++){size_t q=f->cleanup_exits.offset+i;if(q>=o->cleanup_exit_count)return false;ok|=o->cleanup_exits[q].action==id;}if(!ok)return false;--s->count;s->visible_count=s->count;reseal(o,s);return true; }
bool sol_mir_runtime_handler_abi_test_exit_marker(const SolMirRuntimeHandlerAbi*o,SolMirRuntimeHandlerActivationStack*s,size_t f,size_t marker) { return pop(o,s,f,true,marker); }
bool sol_mir_runtime_handler_abi_test_exit_cleanup(const SolMirRuntimeHandlerAbi*o,SolMirRuntimeHandlerActivationStack*s,size_t f,size_t action) { return pop(o,s,f,false,action); }
#endif
