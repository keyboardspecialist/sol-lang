#include "sol/mir_runtime_host_abi.h"
#include "mir_runtime_host_abi_internal.h"
#include "mir_runtime_arena_internal.h"

#include <stdlib.h>
#include <string.h>

#ifndef SOL_MIR_PLAN_TEST_HOOKS
#define sol_mir_runtime_host_abi_observe_failure(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_outcome_message(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_shared_failure(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_propagation(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_value(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_unavailable_value(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_index(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_validator_rejection(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_compare_once(id,left,right,length,li,ri,eq) \
    memcmp((left),(right),(length))
#define sol_mir_runtime_host_abi_observe_ne(id,ln,left,rn,right) \
    ((left)!=(right))
#define sol_mir_runtime_host_abi_observe_ge(id,ln,left,rn,right) \
    ((left)>=(right))
#define sol_mir_runtime_host_abi_observe_gt(id,ln,left,rn,right) \
    ((left)>(right))
#define sol_mir_runtime_host_abi_observe_eq(id,ln,left,rn,right) \
    ((left)==(right))
#define sol_mir_runtime_host_abi_observe_fieldwise_row(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_capture_gap(...) ((void)0)
#define sol_mir_runtime_host_abi_observe_reject(id,expression) (expression)
#endif

#ifdef SOL_MIR_PLAN_TEST_HOOKS
#include <stdio.h>
static _Thread_local bool fail_validation_scratch;
void sol_mir_runtime_host_abi_test_force_validation_scratch_failure(bool value) { fail_validation_scratch=value; }
#endif

enum { HOST_FAILURE_MASK = UINT32_C(0x00008240) };
static bool bad(SolDiagnostics*d,const char*m){if(d)sol_diagnostics_add(d,"SOL-MIR-RUNTIME-HOST-ABI-002",SOL_SEVERITY_ERROR,(SolSpan){0},m);return false;}
#ifdef SOL_MIR_PLAN_TEST_HOOKS
static bool bad_at(const char *id,SolDiagnostics*d,const char*m){
    sol_mir_runtime_host_abi_observe_validator_rejection(id,m);
    return bad(d,m);
}
static void observe_usage_unavailable_from(size_t first){
    static const struct { const char *left,*right,*units; } operands[]={
        {"usage.capabilities","capability_count","records"},
        {"usage.entry_roots","entry_root_count","records"},
        {"usage.operations","operation_count","records"},
        {"usage.arguments","argument_count","records"},
        {"usage.formals","formal_count","records"},
        {"usage.shapes","shape_count","records"},
        {"usage.shape_cases","shape_case_count","records"},
        {"usage.requirements","requirement_count","records"},
        {"usage.grants","grants","records"},
        {"usage.owned_bytes","owned_bytes","bytes"},
        {"usage.build_scratch_bytes","build_scratch_bytes","bytes"},
        {"usage.validation_scratch_bytes","validation_scratch_bytes","bytes"},
        {"usage.validation_work","validation_work","operations"}
    };
    for(size_t i=first;i<sizeof(operands)/sizeof(operands[0]);i++){
        sol_mir_runtime_host_abi_observe_unavailable_value(
            operands[i].left,"not_reached",operands[i].units);
        sol_mir_runtime_host_abi_observe_unavailable_value(
            operands[i].right,"not_reached",operands[i].units);
    }
}
static void observe_capability_lineage_unavailable(unsigned line,
    const char *atom){
    static const struct { unsigned line; const char *atom,*name,*units; } skipped[]={
        {403,"source_root_bounds","capability.source_construct","index"},
        {403,"source_root_bounds","capability.parent","index"},
        {403,"source_root_bounds","expected.parent","index"},
        {403,"source_root_bounds","capability.private_recipe","recipe index"},
        {403,"source_root_bounds","expected.private_recipe","recipe index"},
        {403,"source_root_bounds","capability.private_value","value index"},
        {403,"source_root_bounds","expected.private_value","value index"},
        {404,"parent","capability.private_recipe","recipe index"},
        {404,"parent","expected.private_recipe","recipe index"},
        {404,"parent","capability.private_value","value index"},
        {404,"parent","expected.private_value","value index"},
        {406,"private_recipe","capability.private_value","value index"},
        {406,"private_recipe","expected.private_value","value index"},
        {408,"parent_bounds","capability.source_root","index"},
        {408,"parent_bounds","entry_root_count","count"},
        {408,"parent_bounds","parent.source","enum"},
        {408,"parent_bounds","private_source","enum"},
        {408,"parent_bounds","parent.source_root","index"},
        {408,"parent_bounds","capability.source_root","index"},
        {408,"parent_bounds","capability.private_recipe","recipe index"},
        {408,"parent_bounds","expected.private_recipe","recipe index"},
        {408,"parent_bounds","capability.private_value","value index"},
        {408,"parent_bounds","expected.private_value","value index"},
        {408,"source_root_bounds","parent.source","enum"},
        {408,"source_root_bounds","private_source","enum"},
        {408,"source_root_bounds","parent.source_root","index"},
        {408,"source_root_bounds","capability.source_root","index"},
        {408,"source_root_bounds","capability.private_recipe","recipe index"},
        {408,"source_root_bounds","expected.private_recipe","recipe index"},
        {408,"source_root_bounds","capability.private_value","value index"},
        {408,"source_root_bounds","expected.private_value","value index"},
        {409,"parent_private_source","parent.source_root","index"},
        {409,"parent_private_source","capability.source_root","index"},
        {409,"parent_private_source","capability.private_recipe","recipe index"},
        {409,"parent_private_source","expected.private_recipe","recipe index"},
        {409,"parent_private_source","capability.private_value","value index"},
        {409,"parent_private_source","expected.private_value","value index"},
        {410,"parent_source_root","capability.private_recipe","recipe index"},
        {410,"parent_source_root","expected.private_recipe","recipe index"},
        {410,"parent_source_root","capability.private_value","value index"},
        {410,"parent_source_root","expected.private_value","value index"},
        {411,"private_recipe","capability.private_value","value index"},
        {411,"private_recipe","expected.private_value","value index"},
        {414,"source_root","capability.parent","index"},
        {414,"source_root","expected.parent","index"},
        {414,"source_root","capability.private_recipe","recipe index"},
        {414,"source_root","expected.private_recipe","recipe index"},
        {414,"source_root","capability.private_value","value index"},
        {414,"source_root","source_value","value index"},
        {414,"parent","capability.private_recipe","recipe index"},
        {414,"parent","expected.private_recipe","recipe index"},
        {414,"parent","capability.private_value","value index"},
        {414,"parent","source_value","value index"},
        {414,"private_recipe_missing","capability.private_value","value index"},
        {414,"private_recipe_missing","source_value","value index"}
    };
    for(size_t i=0;i<sizeof(skipped)/sizeof(skipped[0]);i++)
        if(skipped[i].line==line&&strcmp(skipped[i].atom,atom)==0)
            sol_mir_runtime_host_abi_observe_unavailable_value(
                skipped[i].name,"not_reached",skipped[i].units);
}
#else
#define bad_at(id,d,m) bad(d,m)
#define observe_usage_unavailable_from(first) ((void)0)
#define observe_capability_lineage_unavailable(line,atom) ((void)0)
#endif
static _Thread_local bool measuring_validation_work;
static _Thread_local size_t observed_validation_work;
static _Thread_local SolMirRuntimeHostAbiWorkMeter *alias_meter;
static bool mul(size_t a,size_t b,size_t*out){if(a&&b>SIZE_MAX/a){
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    sol_mir_runtime_host_abi_observe_failure("H37.V.mul.18.overflow","validator-arithmetic","original checked multiplication rejected",NULL);
    sol_mir_runtime_host_abi_observe_value("mul.left","initialized","count",a);
    sol_mir_runtime_host_abi_observe_value("mul.right","initialized","count",b);
    sol_mir_runtime_host_abi_observe_unavailable_value("mul.destination","not_written","product");
#endif
    return false;}*out=a*b;return true;}
static bool add(size_t*a,size_t b){if(b>SIZE_MAX-*a){
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    sol_mir_runtime_host_abi_observe_failure("H37.V.add.19.overflow","validator-arithmetic","original checked addition rejected",NULL);
    if(a)sol_mir_runtime_host_abi_observe_value("add.left","initialized","bytes-or-count",*a);
    sol_mir_runtime_host_abi_observe_value("add.right","initialized","bytes-or-count",b);
    sol_mir_runtime_host_abi_observe_unavailable_value("add.destination","not_written","sum");
#endif
    return false;}*a+=b;return true;}
static bool complete(SolMirRuntimeHostAbiLimits l){
#ifdef SOL_MIR_PLAN_TEST_HOOKS
#define COMPLETE_MEMBER(member) if(sol_mir_runtime_host_abi_observe_reject( \
    "H37.V.complete.20." #member,!l.max_##member)) return false
    COMPLETE_MEMBER(capabilities); COMPLETE_MEMBER(entry_roots);
    COMPLETE_MEMBER(operations); COMPLETE_MEMBER(arguments);
    COMPLETE_MEMBER(formals); COMPLETE_MEMBER(shapes);
    COMPLETE_MEMBER(shape_cases); COMPLETE_MEMBER(requirements);
    COMPLETE_MEMBER(grants); COMPLETE_MEMBER(owned_bytes);
    COMPLETE_MEMBER(build_scratch_bytes); COMPLETE_MEMBER(build_work);
    COMPLETE_MEMBER(validation_scratch_bytes); COMPLETE_MEMBER(validation_work);
#undef COMPLETE_MEMBER
    return true;
#else
    return l.max_capabilities&&l.max_entry_roots&&l.max_operations&&l.max_arguments&&l.max_formals&&l.max_shapes&&l.max_shape_cases&&l.max_requirements&&l.max_grants&&l.max_owned_bytes&&l.max_build_scratch_bytes&&l.max_build_work&&l.max_validation_scratch_bytes&&l.max_validation_work;
#endif
}
static bool valid_range(const void*p,size_t n,size_t z){
    size_t bytes;
    if(!n){
        if(p==NULL)return true;
        sol_mir_runtime_host_abi_observe_failure("H37.V.valid_range.21.zero_count_pointer","validator-range","original zero-count pointer predicate rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("range.count","evaluated","records",n);
        sol_mir_runtime_host_abi_observe_value("range.pointer_is_null","evaluated","boolean",0);
        return false;
    }
    if(!p){
        sol_mir_runtime_host_abi_observe_failure("H37.V.valid_range.21.null_pointer","validator-range","original nonzero range null-pointer predicate rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("range.count","evaluated","records",n);
        sol_mir_runtime_host_abi_observe_value("range.pointer_is_null","evaluated","boolean",1);
        return false;
    }
    if(!mul(n,z,&bytes)){
        sol_mir_runtime_host_abi_observe_value("range.pointer_is_null","evaluated","boolean",0);
        return false;
    }
    uintptr_t start=(uintptr_t)p;
    if(start>UINTPTR_MAX-bytes){
        sol_mir_runtime_host_abi_observe_failure("H37.V.valid_range.21.address_overflow","validator-range","original pointer-end overflow predicate rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("range.pointer_is_null","evaluated","boolean",0);
        sol_mir_runtime_host_abi_observe_value("range.start","evaluated","uintptr",start);
        sol_mir_runtime_host_abi_observe_value("range.bytes","initialized","bytes",bytes);
        sol_mir_runtime_host_abi_observe_value("uintptr_max","initialized","uintptr",UINTPTR_MAX);
        return false;
    }
    return true;
}
static bool overlap(const void*a,size_t an,size_t az,const void*b,size_t bn,size_t bz){
    size_t ab,bb;bool left,right;
    if(!an||!bn)return false;
    if(!mul(an,az,&ab)||!mul(bn,bz,&bb))return true;
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    if(x>UINTPTR_MAX-ab){
        sol_mir_runtime_host_abi_observe_failure("H37.V.overlap.22.left_end_overflow","validator-alias","original left range-end overflow rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("left.start","evaluated","uintptr",x);
        sol_mir_runtime_host_abi_observe_value("left.bytes","initialized","bytes",ab);
        return true;
    }
    if(y>UINTPTR_MAX-bb){
        sol_mir_runtime_host_abi_observe_failure("H37.V.overlap.22.right_end_overflow","validator-alias","original right range-end overflow rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("right.start","evaluated","uintptr",y);
        sol_mir_runtime_host_abi_observe_value("right.bytes","initialized","bytes",bb);
        return true;
    }
    if(alias_meter&&!sol_mir_runtime_host_abi_work_tick(alias_meter))return true;
    if(alias_meter&&!sol_mir_runtime_host_abi_work_tick(alias_meter))return true;
    left=x<y+bb;right=y<x+ab;
    bool intersects=left&&right;
    if(intersects){
        sol_mir_runtime_host_abi_observe_failure("H37.V.overlap.22.intersection","validator-alias","original range intersection predicate rejected",NULL);
        sol_mir_runtime_host_abi_observe_value("left.start","evaluated","uintptr",x);
        sol_mir_runtime_host_abi_observe_value("left.bytes","initialized","bytes",ab);
        sol_mir_runtime_host_abi_observe_value("right.start","evaluated","uintptr",y);
        sol_mir_runtime_host_abi_observe_value("right.bytes","initialized","bytes",bb);
    }
    return intersects;
}
static bool aliases_host(const SolMirRuntimeHostAbi*o,const void*p,size_t n,size_t z){
    if(alias_meter&&!sol_mir_runtime_host_abi_work_tick(alias_meter))return true;
#define OWNER_OVERLAP(index,member,count,type) \
    if(overlap(o->member,o->count,sizeof(*o->member),p,n,z)){ \
        sol_mir_runtime_host_abi_observe_failure("H37.V.aliases_host.23.owner" #index,"validator-alias","original owner-arena overlap predicate rejected",NULL); \
        sol_mir_runtime_host_abi_observe_value("owner_arena_index","evaluated","index",index); \
        return true; \
    }
    OWNER_OVERLAP(0,capabilities,capability_count,SolMirRuntimeHostCapabilityPlan)
    OWNER_OVERLAP(1,entry_roots,entry_root_count,SolMirRuntimeHostEntryRoot)
    OWNER_OVERLAP(2,operations,operation_count,SolMirRuntimeHostOperation)
    OWNER_OVERLAP(3,arguments,argument_count,SolMirRuntimeHostArgument)
    OWNER_OVERLAP(4,formals,formal_count,SolMirRuntimeHostFormal)
    OWNER_OVERLAP(5,shapes,shape_count,SolMirRuntimeHostShape)
    OWNER_OVERLAP(6,shape_cases,shape_case_count,SolMirRuntimeHostShapeCase)
    OWNER_OVERLAP(7,requirements,requirement_count,SolMirRuntimeHostRequirement)
#undef OWNER_OVERLAP
    return false;
}
typedef struct { const SolMirRuntimeHostAbi *owner; SolMirRuntimeHostAbiWorkMeter *meter; } HostAliasContext;
static SolMirRuntimeTextGuardResult host_text_guard(uintptr_t address,uintptr_t *boundary,void *opaque) { HostAliasContext*x=opaque;const void*p[]={x->owner->capabilities,x->owner->entry_roots,x->owner->operations,x->owner->arguments,x->owner->formals,x->owner->shapes,x->owner->shape_cases,x->owner->requirements};size_t n[]={x->owner->capability_count,x->owner->entry_root_count,x->owner->operation_count,x->owner->argument_count,x->owner->formal_count,x->owner->shape_count,x->owner->shape_case_count,x->owner->requirement_count},z[]={sizeof(*x->owner->capabilities),sizeof(*x->owner->entry_roots),sizeof(*x->owner->operations),sizeof(*x->owner->arguments),sizeof(*x->owner->formals),sizeof(*x->owner->shapes),sizeof(*x->owner->shape_cases),sizeof(*x->owner->requirements)};uintptr_t next=UINTPTR_MAX;bool hit=false;for(size_t i=0;i<8;i++){size_t bytes;sol_mir_runtime_host_abi_observe_index(4,"owner_arena_index",i);if(!sol_mir_runtime_host_abi_work_tick(x->meter)||!mul(n[i],z[i],&bytes))return SOL_MIR_RUNTIME_TEXT_EXHAUSTED;if(!n[i])continue;uintptr_t start=(uintptr_t)p[i],end=start+bytes;hit|=address>=start&&address<end;if(start>address&&start<next)next=start;}if(hit)return SOL_MIR_RUNTIME_TEXT_OVERLAP;*boundary=next;return SOL_MIR_RUNTIME_TEXT_SAFE;}
static bool visit_host_alias(const void *pointer,size_t count,size_t size,void *context) {
    HostAliasContext *x=context;
    if(pointer==NULL&&count==0&&size==0)return sol_mir_runtime_host_abi_work_tick(x->meter);
    return sol_mir_runtime_host_abi_work_tick(x->meter)
        && !aliases_host(x->owner,pointer,count,size);
}
/* The shared raw census includes nested template/image MIR, source storage,
 * and symbolic text before local host records are traversed. */
static bool aliases_transitive(const SolMirRuntimeHostAbi*o,
    SolMirRuntimeHostAbiWorkMeter *meter) {
    HostAliasContext context={o,meter};
    SolMirRuntimeTextGuard saved_guard=sol_mir_runtime_text_guard;void *saved_context=sol_mir_runtime_text_guard_context;
    sol_mir_runtime_text_guard=host_text_guard;sol_mir_runtime_text_guard_context=&context;
    SolMirRuntimeArenaVisit result=sol_mir_runtime_visit_concrete_arenas(o->conventions->concrete,visit_host_alias,&context);
    sol_mir_runtime_text_guard=saved_guard;sol_mir_runtime_text_guard_context=saved_context;
    return result != SOL_MIR_RUNTIME_ARENA_VISIT_OK;
}
static bool kind(const SolMirRecipe*r,SolMirRuntimeHostArgumentKind*k){switch(r->kind){case SOL_MIR_RECIPE_INT64:*k=SOL_MIR_RUNTIME_HOST_ARGUMENT_INT64;return true;case SOL_MIR_RECIPE_BOOL:*k=SOL_MIR_RUNTIME_HOST_ARGUMENT_BOOL;return true;case SOL_MIR_RECIPE_TEXT:*k=SOL_MIR_RUNTIME_HOST_ARGUMENT_TEXT;return true;case SOL_MIR_RECIPE_UNIT:*k=SOL_MIR_RUNTIME_HOST_ARGUMENT_UNIT;return true;case SOL_MIR_RECIPE_OPTION:*k=SOL_MIR_RUNTIME_HOST_ARGUMENT_OPTION;return true;case SOL_MIR_RECIPE_RESULT:*k=SOL_MIR_RUNTIME_HOST_ARGUMENT_RESULT;return true;default:return false;}}
static bool text_is(const char *text,const char *want){return text&&want&&!strcmp(text,want);}
/* Match enum object representations against one declared-domain value before
 * either owner-supplied tag is loaded as an enum.  Besides avoiding invalid
 * enum reads, requiring both bytestrings to match the same member rejects an
 * identical unsupported tag. */
static bool capability_source_pair_equal(
    const SolMirRuntimeHostCapabilitySource *left,
    const SolMirRuntimeHostCapabilitySource *right) {
    const SolMirRuntimeHostCapabilitySource domain[]={
        SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT,
        SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED,
        SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE
    };
    for(size_t i=0;i<sizeof(domain)/sizeof(domain[0]);i++)
        if(memcmp(left,&domain[i],sizeof(domain[i]))==0
            &&memcmp(right,&domain[i],sizeof(domain[i]))==0)return true;
    return false;
}

/* Capability identity is the declared member tuple, never aggregate padding.
 * Keep this allocation-free and compare all inactive/sentinel coordinates too. */
static bool capability_fields_equal(
    const SolMirRuntimeHostCapabilityPlan *left,
    const SolMirRuntimeHostCapabilityPlan *right) {
    if(!left||!right||!capability_source_pair_equal(&left->source,&right->source))
        return false;
    return left->recipe==right->recipe
        &&left->source_construct==right->source_construct
        &&left->result==right->result
        &&left->temporary==right->temporary
        &&left->source_value==right->source_value
        &&left->source_temporary==right->source_temporary
        &&left->source_place==right->source_place
        &&left->source_root==right->source_root
        &&left->parent==right->parent
        &&left->private_recipe==right->private_recipe
        &&left->private_value==right->private_value;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_runtime_host_abi_test_capability_fields_equal(
    const SolMirRuntimeHostCapabilityPlan *left,
    const SolMirRuntimeHostCapabilityPlan *right) {
    return capability_fields_equal(left,right);
}

static bool capability_fields_equal_once(const char *id,
    const SolMirRuntimeHostCapabilityPlan *left,
    const SolMirRuntimeHostCapabilityPlan *right,size_t left_index,
    size_t right_index) {
    bool equal=capability_fields_equal(left,right);
    sol_mir_runtime_host_abi_observe_capability_comparison(id,left,right,
        sizeof(*left),left_index,right_index,equal);
    return equal;
}
#else
#define capability_fields_equal_once(id,left,right,left_index,right_index) \
    capability_fields_equal((left),(right))
#endif

static bool effect_is(const SolMirMaterialization*m,const SolMirMaterializedEffectAtom*a,const char*want){size_t n=strlen(want);return a->name.count==n&&a->name.offset<=m->effect_name_count&&n<=m->effect_name_count-a->name.offset&&!memcmp(m->effect_names+a->name.offset,want,n);}
static bool recipe_kind_is(const SolMirRepresentation*r,SolMirRecipeId id,SolMirRecipeKind kind){return id<r->recipe_count&&r->recipes[id].kind==kind;}
/* This deliberately reconstructs authority from the exact selected P2/P3.1
 * import and semantic requirement.  It must not inherit the builder's
 * spelling-based profile classification. */
static bool e3_host_requirement(const SolMirRuntimeHostAbi *o, size_t host_id,
    SolMirRuntimeHostAbiWorkMeter *meter) {
#define E3_REJECT(line) do { \
    sol_mir_runtime_host_abi_observe_validator_rejection( \
        "H37.V.e3_host_requirement." #line ".reject",NULL); \
    return false; \
} while (0)
    const SolMirLinkage *l=&o->conventions->concrete->linkage;
    const SolMirMaterialization *m=&o->conventions->concrete->materialization;
    const SolIr *ir=o->conventions->concrete->program.ir;
    const SolMirRepresentation *r=&o->conventions->concrete->representation;
    sol_mir_runtime_host_abi_observe_index(2,"host_index",host_id);
    if(host_id>=l->host_requirement_count){
        sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",host_id);
        sol_mir_runtime_host_abi_observe_value("host_requirement_count","initialized","count",l->host_requirement_count);
        E3_REJECT(56.host_bounds);
    }
    if(!ir){
        sol_mir_runtime_host_abi_observe_unavailable_value("program_ir","not_initialized","pointer");
        E3_REJECT(56.ir_null);
    }
    const SolMirLinkageHostRequirement *h=&l->host_requirements[host_id];
    if(h->import>=m->import_count){
        sol_mir_runtime_host_abi_observe_value("host.import","evaluated","index",h->import);
        sol_mir_runtime_host_abi_observe_value("import_count","initialized","count",m->import_count);
        E3_REJECT(58.import_bounds);
    }
    if(h->effects>=m->effect_row_count){
        sol_mir_runtime_host_abi_observe_value("host.effects","evaluated","index",h->effects);
        sol_mir_runtime_host_abi_observe_value("effect_row_count","initialized","count",m->effect_row_count);
        E3_REJECT(58.effect_bounds);
    }
    if(h->receiver>=r->recipe_count){
        sol_mir_runtime_host_abi_observe_value("host.receiver","evaluated","index",h->receiver);
        sol_mir_runtime_host_abi_observe_value("recipe_count","initialized","count",r->recipe_count);
        E3_REJECT(58.receiver_bounds);
    }
    const SolMirMaterializedImport *im=&m->imports[h->import];
    if(im->source_callable>=ir->callable_count){
        sol_mir_runtime_host_abi_observe_value("import.source_callable","evaluated","index",im->source_callable);
        sol_mir_runtime_host_abi_observe_value("callable_count","initialized","count",ir->callable_count);
        E3_REJECT(61.source_callable_bounds);
    }
    const SolIrCallable *callable=&ir->callables[im->source_callable];
    if(callable->owner>=ir->definition_count){
        sol_mir_runtime_host_abi_observe_value("callable.owner","evaluated","index",callable->owner);
        sol_mir_runtime_host_abi_observe_value("definition_count","initialized","count",ir->definition_count);
        E3_REJECT(63.owner_bounds);
    }
    if(callable->kind!=SOL_IR_CALLABLE_CAPABILITY){
        sol_mir_runtime_host_abi_observe_value("callable.kind","evaluated","enum",callable->kind);
        sol_mir_runtime_host_abi_observe_value("expected.callable_kind","initialized","enum",SOL_IR_CALLABLE_CAPABILITY);
        E3_REJECT(63.callable_kind);
    }
    if(callable->body!=SOL_IR_NONE){
        sol_mir_runtime_host_abi_observe_value("callable.body","evaluated","index",callable->body);
        sol_mir_runtime_host_abi_observe_value("expected.body","initialized","index",SOL_IR_NONE);
        E3_REJECT(63.callable_body);
    }
    if(callable->generic_parameters.count){
        sol_mir_runtime_host_abi_observe_value("callable.generic_parameters.count","evaluated","count",callable->generic_parameters.count);
        E3_REJECT(63.generic_parameters);
    }
    if(callable->effect_parameters.count){
        sol_mir_runtime_host_abi_observe_value("callable.effect_parameters.count","evaluated","count",callable->effect_parameters.count);
        E3_REJECT(63.effect_parameters);
    }
    if(callable->result_authority_kind!=SOL_IR_AUTHORITY_NONE){
        sol_mir_runtime_host_abi_observe_value("callable.result_authority_kind","evaluated","enum",callable->result_authority_kind);
        sol_mir_runtime_host_abi_observe_value("expected.result_authority_kind","initialized","enum",SOL_IR_AUTHORITY_NONE);
        E3_REJECT(63.result_authority_kind);
    }
    if(h->semantic_id.high!=ir->definitions[callable->owner].semantic_id.high){
        sol_mir_runtime_host_abi_observe_value("host.semantic_id.high","evaluated","integer",(size_t)h->semantic_id.high);
        sol_mir_runtime_host_abi_observe_value("definition.semantic_id.high","evaluated","integer",(size_t)ir->definitions[callable->owner].semantic_id.high);
        E3_REJECT(63.semantic_id_high);
    }
    if(h->semantic_id.low!=ir->definitions[callable->owner].semantic_id.low){
        sol_mir_runtime_host_abi_observe_value("host.semantic_id.low","evaluated","integer",(size_t)h->semantic_id.low);
        sol_mir_runtime_host_abi_observe_value("definition.semantic_id.low","evaluated","integer",(size_t)ir->definitions[callable->owner].semantic_id.low);
        E3_REJECT(63.semantic_id_low);
    }
    if(h->receiver_access!=SOL_ACCESS_SHARED){
        sol_mir_runtime_host_abi_observe_value("host.receiver_access","evaluated","enum",h->receiver_access);
        sol_mir_runtime_host_abi_observe_value("expected.receiver_access","initialized","enum",SOL_ACCESS_SHARED);
        E3_REJECT(63.receiver_access);
    }
    if(r->recipes[h->receiver].kind!=SOL_MIR_RECIPE_CAPABILITY){
        sol_mir_runtime_host_abi_observe_value("receiver.recipe_kind","evaluated","enum",r->recipes[h->receiver].kind);
        sol_mir_runtime_host_abi_observe_value("expected.recipe_kind","initialized","enum",SOL_MIR_RECIPE_CAPABILITY);
        E3_REJECT(63.receiver_kind);
    }
    if(r->recipes[h->receiver].concrete_definition!=callable->owner){
        sol_mir_runtime_host_abi_observe_value("receiver.concrete_definition","evaluated","index",r->recipes[h->receiver].concrete_definition);
        sol_mir_runtime_host_abi_observe_value("callable.owner","evaluated","index",callable->owner);
        E3_REJECT(63.receiver_definition);
    }
    const SolMirMaterializedEffectRow *row=&m->effect_rows[h->effects];
    if(row->atoms.count!=1){
        sol_mir_runtime_host_abi_observe_value("effect_row.atoms.count","evaluated","count",row->atoms.count);
        sol_mir_runtime_host_abi_observe_value("expected.effect_atom_count","initialized","count",1);
        E3_REJECT(73.atom_count);
    }
    if(row->atoms.offset>=m->effect_row_atom_count){
        sol_mir_runtime_host_abi_observe_value("effect_row.atoms.offset","evaluated","index",row->atoms.offset);
        sol_mir_runtime_host_abi_observe_value("effect_row_atom_count","initialized","count",m->effect_row_atom_count);
        E3_REJECT(73.atom_offset);
    }
    size_t atom_id=m->effect_row_atoms[row->atoms.offset];
    if(atom_id>=m->effect_atom_count){
        sol_mir_runtime_host_abi_observe_value("effect_atom_index","evaluated","index",atom_id);
        sol_mir_runtime_host_abi_observe_value("effect_atom_count","initialized","count",m->effect_atom_count);
        E3_REJECT(75.atom_bounds);
    }
    if(m->effect_atoms[atom_id].authority!=SOL_MIR_PLAN_EFFECT_AUTHORITY_RECEIVER){
        sol_mir_runtime_host_abi_observe_value("effect_atom.authority","evaluated","enum",m->effect_atoms[atom_id].authority);
        sol_mir_runtime_host_abi_observe_value("expected.effect_authority","initialized","enum",SOL_MIR_PLAN_EFFECT_AUTHORITY_RECEIVER);
        E3_REJECT(76.authority);
    }
    if(m->effect_atoms[atom_id].name.offset>m->effect_name_count){
        sol_mir_runtime_host_abi_observe_value("effect_name.offset","evaluated","index",m->effect_atoms[atom_id].name.offset);
        sol_mir_runtime_host_abi_observe_value("effect_name_count","initialized","count",m->effect_name_count);
        E3_REJECT(76.name_offset);
    }
    if(m->effect_atoms[atom_id].name.count>m->effect_name_count-m->effect_atoms[atom_id].name.offset){
        sol_mir_runtime_host_abi_observe_value("effect_name.count","evaluated","bytes",m->effect_atoms[atom_id].name.count);
        sol_mir_runtime_host_abi_observe_value("effect_name_available","evaluated","bytes",m->effect_name_count-m->effect_atoms[atom_id].name.offset);
        E3_REJECT(76.name_count);
    }
    if(h->parameters.offset>m->type_id_count){
        sol_mir_runtime_host_abi_observe_value("parameters.offset","evaluated","index",h->parameters.offset);
        sol_mir_runtime_host_abi_observe_value("type_id_count","initialized","count",m->type_id_count);
        E3_REJECT(80.parameter_offset);
    }
    if(h->parameters.count>m->type_id_count-h->parameters.offset){
        sol_mir_runtime_host_abi_observe_value("parameters.count","evaluated","count",h->parameters.count);
        sol_mir_runtime_host_abi_observe_value("parameter_types_available","evaluated","count",m->type_id_count-h->parameters.offset);
        E3_REJECT(80.parameter_count);
    }
    if(h->parameter_accesses.offset>m->access_count){
        sol_mir_runtime_host_abi_observe_value("parameter_accesses.offset","evaluated","index",h->parameter_accesses.offset);
        sol_mir_runtime_host_abi_observe_value("access_count","initialized","count",m->access_count);
        E3_REJECT(80.access_offset);
    }
    if(h->parameter_accesses.count!=h->parameters.count){
        sol_mir_runtime_host_abi_observe_value("parameter_accesses.count","evaluated","count",h->parameter_accesses.count);
        sol_mir_runtime_host_abi_observe_value("parameters.count","evaluated","count",h->parameters.count);
        E3_REJECT(80.access_count_mismatch);
    }
    if(h->parameter_accesses.count>m->access_count-h->parameter_accesses.offset){
        sol_mir_runtime_host_abi_observe_value("parameter_accesses.count","evaluated","count",h->parameter_accesses.count);
        sol_mir_runtime_host_abi_observe_value("parameter_accesses_available","evaluated","count",m->access_count-h->parameter_accesses.offset);
        E3_REJECT(80.access_count_bounds);
    }
    const char *capability=ir->definitions[callable->owner].name,*member=callable->name,*effect=NULL;
    SolMirRecipeKind parameter=SOL_MIR_RECIPE_INT64,result=SOL_MIR_RECIPE_INT64;size_t parameter_count=0;
    if(text_is(capability,"Console")&&text_is(member,"write")){effect="console.write";parameter=SOL_MIR_RECIPE_TEXT;result=SOL_MIR_RECIPE_UNIT;parameter_count=1;}
    else if(text_is(capability,"Arguments")&&text_is(member,"count")){effect="process.arguments.count";result=SOL_MIR_RECIPE_INT64;}
    else if(text_is(capability,"Arguments")&&text_is(member,"get")){effect="process.arguments.get";parameter=SOL_MIR_RECIPE_INT64;result=SOL_MIR_RECIPE_OPTION;parameter_count=1;}
    else if(text_is(capability,"Configuration")&&text_is(member,"read")){effect="configuration.read";parameter=SOL_MIR_RECIPE_TEXT;result=SOL_MIR_RECIPE_OPTION;parameter_count=1;}
    else E3_REJECT(92);
    if(!effect_is(m,&m->effect_atoms[atom_id],effect)||h->parameters.count!=parameter_count||!recipe_kind_is(r,h->result,result))E3_REJECT(93);
    if(result==SOL_MIR_RECIPE_OPTION){const SolMirRecipe*option=&r->recipes[h->result];bool text_payload=false;for(size_t i=0;i<option->variants.count;i++){sol_mir_runtime_host_abi_observe_index(4,"option_variant",i);if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirRecipeVariant*v=&r->variants[option->variants.offset+i];if(v->fields.count==1&&recipe_kind_is(r,r->fields[v->fields.offset].type,SOL_MIR_RECIPE_TEXT))text_payload=true;else if(v->fields.count>1)E3_REJECT(94);}if(!text_payload)E3_REJECT(94);}
    for(size_t i=0;i<h->parameters.count;i++){sol_mir_runtime_host_abi_observe_index(4,"parameter_index",i);if(!sol_mir_runtime_host_abi_work_tick(meter))return false;SolMirRecipeId recipe=m->type_ids[h->parameters.offset+i];if(!recipe_kind_is(r,recipe,parameter)||m->accesses[h->parameter_accesses.offset+i]!=SOL_ACCESS_OWNED)E3_REJECT(95);}
#undef E3_REJECT
    return true;
}
static int frozen_profile_kind(const SolMirRuntimeHostAbi *o,size_t host){
    const SolMirLinkage*l=&o->conventions->concrete->linkage;
    const SolMirMaterialization*m=&o->conventions->concrete->materialization;
    const SolIr*ir=o->conventions->concrete->program.ir;
    if(!ir||host>=l->host_requirement_count)return -1;
    const SolMirLinkageHostRequirement*h=&l->host_requirements[host];
    if(h->import>=m->import_count||m->imports[h->import].source_callable>=ir->callable_count)return -1;
    const SolIrCallable*c=&ir->callables[m->imports[h->import].source_callable];
    if(c->owner>=ir->definition_count)return -1;
    const char*owner=ir->definitions[c->owner].name;
    if(text_is(owner,"Console")&&text_is(c->name,"write"))return 0;
    if(text_is(owner,"Arguments")&&text_is(c->name,"count"))return 1;
    if(text_is(owner,"Arguments")&&text_is(c->name,"get"))return 2;
    if(text_is(owner,"Configuration")&&text_is(c->name,"read"))return 3;
    return -1;
}
static bool frozen_e6_surface(const SolMirRuntimeHostAbi*o,
    SolMirRuntimeHostAbiWorkMeter *meter){
#define FROZEN_REJECT(line) do { \
    sol_mir_runtime_host_abi_observe_validator_rejection( \
        "H37.V.frozen_e6_surface." #line ".reject",NULL); \
    return false; \
} while (0)
    const SolMirLinkage*l=&o->conventions->concrete->linkage;
    const SolMirRuntimeConventions*c=o->conventions;
    unsigned seen=0;size_t host_imports=0;
    if(l->host_requirement_count>4){sol_mir_runtime_host_abi_observe_value("host_requirement_count","evaluated","count",l->host_requirement_count);FROZEN_REJECT(119);}
    for(size_t i=0;i<c->import_count;i++){sol_mir_runtime_host_abi_observe_index(4,"import_index",i);if(!sol_mir_runtime_host_abi_work_tick(meter))return false;if(c->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST)++host_imports;}
    if(host_imports!=l->host_requirement_count){sol_mir_runtime_host_abi_observe_value("host_import_count","evaluated","count",host_imports);sol_mir_runtime_host_abi_observe_value("host_requirement_count","evaluated","count",l->host_requirement_count);FROZEN_REJECT(121);}
    for(size_t host=0;host<l->host_requirement_count;host++){sol_mir_runtime_host_abi_observe_index(0,"host_index",host);if(!sol_mir_runtime_host_abi_work_tick(meter))return false;
        size_t imports=0,signatures=0;int kind;
        if(!e3_host_requirement(o,host,meter))return false;
        kind=frozen_profile_kind(o,host);
        if(kind<0)FROZEN_REJECT(124);
        if(seen&(1u<<kind))FROZEN_REJECT(124);
        seen|=1u<<kind;
        for(size_t i=0;i<c->import_count;i++){sol_mir_runtime_host_abi_observe_index(4,"import_index",i);if(!sol_mir_runtime_host_abi_work_tick(meter))return false;if(c->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST&&c->imports[i].host==host)++imports;}
        for(size_t i=0;i<c->signature_count;i++){sol_mir_runtime_host_abi_observe_index(4,"signature_index",i);if(!sol_mir_runtime_host_abi_work_tick(meter))return false;if(c->signatures[i].origin==SOL_MIR_RUNTIME_SIGNATURE_HOST&&c->signatures[i].host==host)++signatures;}
        if(imports!=1||signatures!=1){sol_mir_runtime_host_abi_observe_value("matching_import_count","evaluated","count",imports);sol_mir_runtime_host_abi_observe_value("matching_signature_count","evaluated","count",signatures);FROZEN_REJECT(128);}
        for(size_t prior=0;prior<host;prior++){sol_mir_runtime_host_abi_observe_index(1,"prior_host_index",prior);if(!sol_mir_runtime_host_abi_work_tick(meter))return false;
            const SolMirRuntimeImport *left=NULL,*right=NULL;
            for(size_t i=0;i<c->import_count;i++){sol_mir_runtime_host_abi_observe_index(4,"import_index",i);if(!sol_mir_runtime_host_abi_work_tick(meter))return false;
                if(c->imports[i].kind!=SOL_MIR_RUNTIME_IMPORT_HOST)continue;
                if(c->imports[i].host==prior)left=&c->imports[i];
                if(c->imports[i].host==host)right=&c->imports[i];
            }
            if(sol_mir_runtime_host_abi_observe_reject(
                    "H37.V.frozen_e6_surface.136.left_import_missing",!left)){
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "duplicate.left_import","no_expected_object","row");
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "duplicate.right_import","not_reached","row predicate");
                sol_mir_runtime_host_abi_observe_capture_gap(
                    "duplicate identity operands unavailable: prior host import missing");
                FROZEN_REJECT(136);
            }
            if(sol_mir_runtime_host_abi_observe_reject(
                    "H37.V.frozen_e6_surface.136.right_import_missing",!right)){
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "duplicate.right_import","no_expected_object","row");
                sol_mir_runtime_host_abi_observe_capture_gap(
                    "duplicate identity operands unavailable: current host import missing");
                FROZEN_REJECT(136);
            }
            if(!sol_mir_runtime_host_abi_observe_compare_once(
                "H37.V.frozen_e6_surface.136.identity",&left->identity,
                &right->identity,sizeof(left->identity),prior,host,true)
                ||!sol_mir_runtime_host_abi_observe_compare_once(
                "H37.V.frozen_e6_surface.136.symbol",&left->symbol,
                &right->symbol,sizeof(left->symbol),prior,host,true))
                FROZEN_REJECT(136);
        }
    }
#undef FROZEN_REJECT
    return true;
}
static bool validate_shape(const SolMirRuntimeHostAbi*o,size_t id,unsigned char*active,
    unsigned char*seen,SolMirRuntimeHostAbiWorkMeter *meter){
#ifdef SOL_MIR_PLAN_TEST_HOOKS
#define SHAPE_REJECT(line) do { \
    sol_mir_runtime_host_abi_observe_validator_rejection( \
        "H37.V.validate_shape." #line ".reject",NULL); \
    if (diagnostic_shape_row) \
        sol_mir_runtime_host_abi_observe_fieldwise_row( \
            "H37.V.validate_shape." #line ".shape.row", \
            diagnostic_shape_row,sizeof(*diagnostic_shape_row),id); \
    else \
        sol_mir_runtime_host_abi_observe_capture_gap( \
            "shape row unavailable: original shape index is outside shape_count"); \
    return false; \
} while (0)
#define SHAPE_ACTIVE_REJECT(line) do { \
    active[s->recipe]=0; \
    sol_mir_runtime_host_abi_observe_validator_rejection( \
        "H37.V.validate_shape." #line ".reject",NULL); \
    if (diagnostic_shape_row) \
        sol_mir_runtime_host_abi_observe_fieldwise_row( \
            "H37.V.validate_shape." #line ".shape.row", \
            diagnostic_shape_row,sizeof(*diagnostic_shape_row),id); \
    else \
        sol_mir_runtime_host_abi_observe_capture_gap( \
            "shape row unavailable: original shape index is outside shape_count"); \
    return false; \
} while (0)
    const SolMirRuntimeHostShape *diagnostic_shape_row=NULL;
#else
#define SHAPE_REJECT(line) do { return false; } while (0)
#define SHAPE_ACTIVE_REJECT(line) do { active[s->recipe]=0; return false; } while (0)
#endif
    const SolMirRepresentation*r=&o->conventions->concrete->representation;
    sol_mir_runtime_host_abi_observe_index(4,"shape_index",id);
    if(!sol_mir_runtime_host_abi_work_tick(meter)){
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        sol_mir_runtime_host_abi_observe_capture_gap(
            "shape row not reached: work tick failed before bounds and row access");
#endif
        return false;
    }
    if(sol_mir_runtime_host_abi_observe_ge(
            "H37.V.validate_shape.144.shape_bounds","shape_index",id,
            "shape_count",o->shape_count))SHAPE_REJECT(144);
    const SolMirRuntimeHostShape*s=&o->shapes[id];SolMirRuntimeHostArgumentKind want;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    diagnostic_shape_row=s;
#endif
    if(sol_mir_runtime_host_abi_observe_reject(
            "H37.V.validate_shape.144.already_seen",seen[id]))SHAPE_REJECT(144);
    if(sol_mir_runtime_host_abi_observe_ge(
            "H37.V.validate_shape.146.recipe_bounds","shape.recipe",s->recipe,
            "recipe_count",r->recipe_count))SHAPE_REJECT(146);
    if(sol_mir_runtime_host_abi_observe_reject(
            "H37.V.validate_shape.146.recursive_cycle",active[s->recipe]))SHAPE_REJECT(146);
    if(sol_mir_runtime_host_abi_observe_reject(
            "H37.V.validate_shape.146.uninhabited",
            !r->recipes[s->recipe].inhabited))SHAPE_REJECT(146);
    if(sol_mir_runtime_host_abi_observe_reject(
            "H37.V.validate_shape.147.kind_lookup",
            !kind(&r->recipes[s->recipe],&want)))SHAPE_REJECT(147);
    if(sol_mir_runtime_host_abi_observe_ne(
            "H37.V.validate_shape.147.kind","shape.kind",(size_t)s->kind,
            "expected.kind",(size_t)want))SHAPE_REJECT(147);
    if(sol_mir_runtime_host_abi_observe_gt(
            "H37.V.validate_shape.148.cases_offset","cases.offset",s->cases.offset,
            "shape_case_count",o->shape_case_count))SHAPE_REJECT(148);
    if(sol_mir_runtime_host_abi_observe_gt(
            "H37.V.validate_shape.149.cases_extent","cases.count",s->cases.count,
            "cases.available",o->shape_case_count-s->cases.offset))SHAPE_REJECT(149);
    seen[id]=1;
    if(want!=SOL_MIR_RUNTIME_HOST_ARGUMENT_OPTION&&want!=SOL_MIR_RUNTIME_HOST_ARGUMENT_RESULT){
        if(sol_mir_runtime_host_abi_observe_ne(
                "H37.V.validate_shape.152.unexpected_cases","cases.count",
                s->cases.count,"expected.cases.count",0)){
            SHAPE_REJECT(152);
        }
        return true;
    }
    active[s->recipe]=1;
    if(sol_mir_runtime_host_abi_observe_ne(
            "H37.V.validate_shape.154.case_count","cases.count",s->cases.count,
            "recipe.variants.count",r->recipes[s->recipe].variants.count)){
        SHAPE_ACTIVE_REJECT(154);
    }
    for(size_t i=0;i<s->cases.count;i++){
        sol_mir_runtime_host_abi_observe_index(5,"shape_case_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(meter)){
            active[s->recipe]=0;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
            sol_mir_runtime_host_abi_observe_fieldwise_row(
                "H37.V.validate_shape.159.shape.row",s,sizeof(*s),id);
            sol_mir_runtime_host_abi_observe_unavailable_value(
                "shape_case.row","not_reached","bytes");
#endif
            return false;
        }
        const SolMirRuntimeHostShapeCase*c=&o->shape_cases[s->cases.offset+i];
        const SolMirRecipeVariant*v=&r->variants[r->recipes[s->recipe].variants.offset+i];
        if(sol_mir_runtime_host_abi_observe_ne(
                "H37.V.validate_shape.159.parent","case.parent",c->parent,
                "shape_index",id)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.validate_shape.159.ordinal","case.ordinal",c->ordinal,
                "variant.ordinal",v->ordinal)
            ||sol_mir_runtime_host_abi_observe_gt(
                "H37.V.validate_shape.159.field_count","variant.fields.count",
                v->fields.count,"maximum.fields.count",1)
            ||(v->fields.count==0&&sol_mir_runtime_host_abi_observe_ne(
                "H37.V.validate_shape.159.empty_child","case.child",c->child,
                "expected.child",SOL_MIR_RUNTIME_NONE))
            ||(v->fields.count==1&&(sol_mir_runtime_host_abi_observe_eq(
                 "H37.V.validate_shape.159.missing_child","case.child",c->child,
                 "none",SOL_MIR_RUNTIME_NONE)
                 ||sol_mir_runtime_host_abi_observe_ge(
                 "H37.V.validate_shape.159.child_bounds","case.child",c->child,
                 "shape_count",o->shape_count)
                 ||sol_mir_runtime_host_abi_observe_ne(
                 "H37.V.validate_shape.159.child_recipe","child.recipe",
                 o->shapes[c->child].recipe,"field.recipe",
                 r->fields[v->fields.offset].type)
                 ||sol_mir_runtime_host_abi_observe_ne(
                 "H37.V.validate_shape.159.child_access","child.access",
                 (size_t)o->shapes[c->child].access,"shape.access",
                 (size_t)s->access)))){
            sol_mir_runtime_host_abi_observe_fieldwise_row(
                "H37.V.validate_shape.159.shape-case.row",c,sizeof(*c),
                s->cases.offset+i);
            SHAPE_ACTIVE_REJECT(159);
        }
        if(v->fields.count==1
            &&!validate_shape(o,c->child,active,seen,meter)){
            active[s->recipe]=0;
            return false;
        }
    }
    active[s->recipe]=0;
#undef SHAPE_ACTIVE_REJECT
#undef SHAPE_REJECT
    return true;
}
static bool cleanup_mapping(const SolMirRuntimeHostAbi*o,const SolMirRuntimeHostRequirement*q){
    if(sol_mir_runtime_host_abi_observe_ge("H37.V.cleanup_mapping.168.event_bounds","requirement.event",q->event,"cleanup.event_count",o->cleanup->event_count)
        ||sol_mir_runtime_host_abi_observe_ge("H37.V.cleanup_mapping.168.transition_bounds","requirement.failure_transition",q->failure_transition,"cleanup.transition_count",o->cleanup->transition_count))return false;
    const SolMirRuntimeCleanupEvent*e=&o->cleanup->events[q->event];
    const SolMirRuntimeCleanupTransition*t=&o->cleanup->transitions[q->failure_transition];
    const SolMirRuntimeCall*c=&o->conventions->calls[q->call];
    if(sol_mir_runtime_host_abi_observe_ne("H37.V.cleanup_mapping.168.event","transition.event",t->event,"requirement.event",q->event)
        ||sol_mir_runtime_host_abi_observe_ne("H37.V.cleanup_mapping.168.edge_role","transition.edge_role",(size_t)t->edge_role,"expected.edge_role",SOL_MIR_RUNTIME_CLEANUP_EDGE_CALL_FAILURE)
        ||sol_mir_runtime_host_abi_observe_ne("H37.V.cleanup_mapping.168.failure_mask","transition.failure_mask",t->failure_mask,"expected.failure_mask",HOST_FAILURE_MASK)
        ||sol_mir_runtime_host_abi_observe_ne("H37.V.cleanup_mapping.168.failure_source","transition.failure_source",(size_t)t->failure_source,"expected.failure_source",SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_INHERITED_P31)
        ||sol_mir_runtime_host_abi_observe_ne("H37.V.cleanup_mapping.168.failure_site","transition.failure_site",t->failure_site,"call.failure_site",c->failure_site)
        ||sol_mir_runtime_host_abi_observe_ne("H37.V.cleanup_mapping.168.block","event.block",e->block,"call.block",c->block)
        ||sol_mir_runtime_host_abi_observe_ne("H37.V.cleanup_mapping.168.producer","event.producer",(size_t)e->producer,"expected.producer",(size_t)(c->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_IMAGE?SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE:SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_INVOKE)))return false;
    return true;
}
static size_t call_host(const SolMirRuntimeHostAbi*o,const SolMirRuntimeCall*c){const SolMirLinkage*l=&o->conventions->concrete->linkage;if(c->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_HOST)return c->host;if(c->target_kind==SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE&&c->table<l->table_entry_count&&l->table_entries[c->table].target_kind==SOL_MIR_LINKAGE_TARGET_HOST)return l->table_entries[c->table].host;return SOL_MIR_RUNTIME_NONE;}
static size_t image_for_instance(const SolMirMaterialization*m,size_t instance,
    SolMirRuntimeHostAbiWorkMeter *meter){for(size_t i=0;i<m->image_count;i++){
        sol_mir_runtime_host_abi_observe_index(4,"image_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(meter))return SOL_MIR_RUNTIME_NONE;
        if(m->images[i].instance==instance)return i;}return SOL_MIR_RUNTIME_NONE;}
static bool coordinate(const SolMirMaterialization*m,SolMirMaterializedTemporaryId temporary,
    SolMirMaterializedValueId*value,SolMirMaterializedPlaceId*place,
    SolMirRuntimeHostAbiWorkMeter *meter){size_t found=SOL_MIR_RUNTIME_NONE;
    if(temporary==SOL_MIR_RUNTIME_NONE)return false;for(size_t i=0;i<m->instruction_count;i++){
        sol_mir_runtime_host_abi_observe_index(4,"instruction_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirMaterializedInstruction*x=&m->instructions[i];if(x->kind!=SOL_MIR_INST_TEMPORARY_INIT||x->temporary!=temporary)continue;if(found!=SOL_MIR_RUNTIME_NONE)return false;found=x->left;}if(found>=m->value_count)return false;*value=found;*place=SOL_MIR_RUNTIME_NONE;const SolMirMaterializedValue*v=&m->values[found];if(v->instruction<m->instruction_count){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirMaterializedInstruction*x=&m->instructions[v->instruction];if(x->kind==SOL_MIR_INST_LOAD_COPY||x->kind==SOL_MIR_INST_LOAD_MOVE||x->kind==SOL_MIR_INST_LOAD_UPDATE)*place=x->place;}return true;}
static bool result_temporary_for(const SolMirMaterialization*m,SolMirMaterializedValueId result,SolMirMaterializedTemporaryId*temporary,SolMirRuntimeHostAbiWorkMeter *meter){size_t found=SOL_MIR_RUNTIME_NONE;for(size_t i=0;i<m->instruction_count;i++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirMaterializedInstruction*x=&m->instructions[i];if(x->kind!=SOL_MIR_INST_TEMPORARY_INIT||x->left!=result)continue;if(found!=SOL_MIR_RUNTIME_NONE)return false;found=x->temporary;}if(found==SOL_MIR_RUNTIME_NONE)return false;*temporary=found;return true;}
static size_t root_for_local(const SolMirRuntimeHostAbi*o,SolMirMaterializedLocalId local,SolMirRuntimeHostAbiWorkMeter *meter){const SolMirMaterialization*m=&o->conventions->concrete->materialization;const SolMirLinkage*l=&o->conventions->concrete->linkage;if(local>=m->local_count)return SOL_MIR_RUNTIME_NONE;for(size_t root=0;root<o->entry_root_count;root++){if(!sol_mir_runtime_host_abi_work_tick(meter))return SOL_MIR_RUNTIME_NONE;const SolMirRuntimeHostEntryRoot*r=&o->entry_roots[root];if(r->entry>=o->conventions->entry_count)continue;const SolMirRuntimeEntry*entry=&o->conventions->entries[r->entry];if(entry->callable>=l->callable_count)continue;size_t image=image_for_instance(m,l->callables[entry->callable].instance,meter);if(image==SOL_MIR_RUNTIME_NONE)continue;const SolMirMaterializedImage*x=&m->images[image];for(size_t i=0;i<x->locals.count;i++){if(!sol_mir_runtime_host_abi_work_tick(meter))return SOL_MIR_RUNTIME_NONE;size_t candidate=x->locals.offset+i;if(candidate==local&&m->locals[candidate].kind==SOL_MIR_MATERIALIZED_LOCAL_PARAMETER&&m->locals[candidate].ordinal==r->formal)return root;}}return SOL_MIR_RUNTIME_NONE;}

typedef enum {
    VALIDATOR_LINEAGE_OK,
    VALIDATOR_LINEAGE_INVALID,
    VALIDATOR_LINEAGE_NO_WORK
} ValidatorLineageResult;

typedef struct {
    size_t owner_id;
    size_t root_id;
    size_t constructor_id;
    bool constructor_origin;
} ValidatorLineageEndpoint;

static ValidatorLineageResult validator_lineage_charge(
    SolMirRuntimeHostAbiWorkMeter *meter) {
    return sol_mir_runtime_host_abi_work_tick(meter)
        ?VALIDATOR_LINEAGE_OK:VALIDATOR_LINEAGE_NO_WORK;
}

static bool validator_lineage_member(size_t begin,size_t count,size_t extent,
    size_t id) {
    return begin<=extent&&count<=extent-begin&&id>=begin&&id-begin<count;
}

static bool validator_lineage_view_storage_valid(
    const SolMirRuntimeHostAbi *owner,const SolMirMaterialization *m) {
    if(!owner||!owner->conventions||!owner->conventions->concrete||!m)
        return false;
    const SolMirRuntimeConventions *conventions=owner->conventions;
    const SolMirConcreteProgram *concrete=conventions->concrete;
    const SolMirOperations *operations=&concrete->operations;
    const SolMirLinkage *linkage=&concrete->linkage;
    const SolIr *ir=concrete->program.ir;
#define STORAGE(array,count) ((count)==0||(array)!=NULL)
    bool valid=STORAGE(m->images,m->image_count)
        &&STORAGE(m->types,m->type_count)&&STORAGE(m->locals,m->local_count)
        &&STORAGE(m->places,m->place_count)
        &&STORAGE(m->projections,m->projection_count)
        &&STORAGE(m->values,m->value_count)
        &&STORAGE(m->instructions,m->instruction_count)
        &&STORAGE(m->temporaries,m->temporary_count)
        &&STORAGE(m->blocks,m->block_count)
        &&STORAGE(m->loops,m->loop_count)
        &&STORAGE(operations->constructors,operations->constructor_count)
        &&STORAGE(operations->construct_operands,
            operations->construct_operand_count)
        &&STORAGE(owner->entry_roots,owner->entry_root_count)
        &&STORAGE(conventions->entries,conventions->entry_count)
        &&STORAGE(linkage->callables,linkage->callable_count)
        &&ir!=NULL&&STORAGE(ir->locals,ir->local_count);
#undef STORAGE
    return valid&&m->image_count<=m->image_capacity
        &&m->type_count<=m->type_capacity&&m->local_count<=m->local_capacity
        &&m->place_count<=m->place_capacity
        &&m->projection_count<=m->projection_capacity
        &&m->value_count<=m->value_capacity
        &&m->instruction_count<=m->instruction_capacity
        &&m->temporary_count<=m->temporary_capacity
        &&m->block_count<=m->block_capacity
        &&m->loop_count<=m->loop_capacity
        &&operations->constructor_count<=operations->constructor_capacity
        &&operations->construct_operand_count
            <=operations->construct_operand_capacity;
}

static ValidatorLineageResult validator_lineage_read_instruction(
    const SolMirMaterialization *m,const SolMirMaterializedImage *image,
    size_t id,SolMirRuntimeHostAbiWorkMeter *meter,
    const SolMirMaterializedInstruction **instruction) {
    if(!m->instructions||!validator_lineage_member(image->instructions.offset,
            image->instructions.count,m->instruction_count,id))
        return VALIDATOR_LINEAGE_INVALID;
    ValidatorLineageResult result=validator_lineage_charge(meter);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    *instruction=&m->instructions[id];
    return VALIDATOR_LINEAGE_OK;
}

static ValidatorLineageResult validator_lineage_read_block(
    const SolMirMaterialization *m,const SolMirMaterializedImage *image,
    size_t id,SolMirRuntimeHostAbiWorkMeter *meter,
    const SolMirMaterializedBlock **block) {
    if(!m->blocks||image->blocks.offset>m->block_count
        ||image->blocks.count>m->block_count-image->blocks.offset)
        return VALIDATOR_LINEAGE_INVALID;
    size_t found=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<image->blocks.count;i++){
        ValidatorLineageResult result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        size_t index=image->blocks.offset+i;
        if(m->blocks[index].id!=id)continue;
        if(found!=SOL_MIR_RUNTIME_NONE)return VALIDATOR_LINEAGE_INVALID;
        found=index;
    }
    if(found==SOL_MIR_RUNTIME_NONE)return VALIDATOR_LINEAGE_INVALID;
    *block=&m->blocks[found];
    return VALIDATOR_LINEAGE_OK;
}

static ValidatorLineageResult validator_lineage_block_is_looped(
    const SolMirMaterialization *m,const SolMirMaterializedImage *image,
    size_t block,SolMirRuntimeHostAbiWorkMeter *meter,bool *looped) {
    if(image->loops.offset>m->loop_count
        ||image->loops.count>m->loop_count-image->loops.offset)
        return VALIDATOR_LINEAGE_INVALID;
    *looped=false;
    for(size_t i=0;i<image->loops.count;i++){
        ValidatorLineageResult result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        const SolMirMaterializedLoop *row=&m->loops[image->loops.offset+i];
        if(row->header==block||row->condition==block
            ||row->body==block||row->backedge==block){
            *looped=true;return VALIDATOR_LINEAGE_OK;
        }
    }
    return VALIDATOR_LINEAGE_OK;
}

static ValidatorLineageResult validator_lineage_definition(
    const SolMirMaterialization *m,const SolMirMaterializedImage *image,
    size_t value_id,size_t block_id,size_t before,
    SolMirRuntimeHostAbiWorkMeter *meter,
    const SolMirMaterializedValue **value,
    const SolMirMaterializedInstruction **instruction,size_t *instruction_id) {
    if(!m->values||value_id>=m->value_count)return VALIDATOR_LINEAGE_INVALID;
    ValidatorLineageResult result=validator_lineage_charge(meter);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    const SolMirMaterializedValue *v=&m->values[value_id];
    if(v->kind!=SOL_MIR_VALUE_INSTRUCTION||v->block!=block_id
        ||v->instruction>=before
        ||!validator_lineage_member(image->values.offset,image->values.count,
            m->value_count,value_id)
        ||!validator_lineage_member(image->instructions.offset,
            image->instructions.count,m->instruction_count,v->instruction)
        ||v->source_definition>SIZE_MAX-image->instructions.offset
        ||image->instructions.offset+v->source_definition!=v->instruction)
        return VALIDATOR_LINEAGE_INVALID;
    const SolMirMaterializedInstruction *def;
    result=validator_lineage_read_instruction(m,image,v->instruction,meter,&def);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(def->result!=value_id||def->block!=block_id)
        return VALIDATOR_LINEAGE_INVALID;
    const SolMirMaterializedBlock *block_row;
    result=validator_lineage_read_block(m,image,block_id,meter,&block_row);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(!validator_lineage_member(block_row->instructions.offset,
            block_row->instructions.count,m->instruction_count,v->instruction))
        return VALIDATOR_LINEAGE_INVALID;
    size_t definitions=0,unique=SOL_MIR_RUNTIME_NONE;
    for(size_t candidate=0;candidate<m->instruction_count;candidate++){
        result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        if(m->instructions[candidate].result!=value_id)continue;
        if(++definitions>1)return VALIDATOR_LINEAGE_INVALID;
        unique=candidate;
    }
    if(definitions!=1||unique!=v->instruction)
        return VALIDATOR_LINEAGE_INVALID;
    *value=v;*instruction=def;*instruction_id=v->instruction;
    return VALIDATOR_LINEAGE_OK;
}

static ValidatorLineageResult validator_lineage_staging(
    const SolMirMaterialization *m,const SolMirMaterializedImage *image,
    size_t temporary,SolMirRuntimeHostAbiWorkMeter *meter,
    size_t *instruction_id,size_t *value_id) {
    if(!m->instructions||temporary==SOL_MIR_RUNTIME_NONE
        ||!validator_lineage_member(image->temporaries.offset,
            image->temporaries.count,m->temporary_count,temporary))
        return VALIDATOR_LINEAGE_INVALID;
    size_t found=SOL_MIR_RUNTIME_NONE,value=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<m->instruction_count;i++){
        ValidatorLineageResult result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        const SolMirMaterializedInstruction *x=&m->instructions[i];
        if(x->kind!=SOL_MIR_INST_TEMPORARY_INIT||x->temporary!=temporary)continue;
        if(found!=SOL_MIR_RUNTIME_NONE
            ||!validator_lineage_member(image->instructions.offset,
                image->instructions.count,m->instruction_count,i))
            return VALIDATOR_LINEAGE_INVALID;
        found=i;value=x->left;
    }
    if(found==SOL_MIR_RUNTIME_NONE||value>=m->value_count)
        return VALIDATOR_LINEAGE_INVALID;
    *instruction_id=found;*value_id=value;
    return VALIDATOR_LINEAGE_OK;
}

static ValidatorLineageResult validator_lineage_output_temp(
    const SolMirMaterialization *m,const SolMirMaterializedImage *image,
    size_t value,size_t block,size_t definition,size_t use_before,
    SolMirRuntimeHostAbiWorkMeter *meter,size_t *count,size_t *temporary) {
    if(!m->instructions)return VALIDATOR_LINEAGE_INVALID;
    size_t matches=0,found=SOL_MIR_RUNTIME_NONE;
    for(size_t i=0;i<m->instruction_count;i++){
        ValidatorLineageResult result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        const SolMirMaterializedInstruction *x=&m->instructions[i];
        if(x->kind!=SOL_MIR_INST_TEMPORARY_INIT||x->left!=value)continue;
        if(++matches>1
            ||!validator_lineage_member(image->instructions.offset,
                image->instructions.count,m->instruction_count,i)
            ||x->block!=block||i<=definition||i>use_before
            ||!validator_lineage_member(image->temporaries.offset,
                image->temporaries.count,m->temporary_count,x->temporary))
            return VALIDATOR_LINEAGE_INVALID;
        found=x->temporary;
    }
    *count=matches;*temporary=found;
    return VALIDATOR_LINEAGE_OK;
}

static ValidatorLineageResult validator_lineage_entry_root(
    const SolMirRuntimeHostAbi *o,const SolMirMaterialization *m,size_t local_id,
    SolMirRuntimeHostAbiWorkMeter *meter,size_t *root_out) {
    const SolMirConcreteProgram *c=o->conventions->concrete;
    const SolMirLinkage *linkage=&c->linkage;
    const SolIr *ir=c->program.ir;
    if(!m->locals||local_id>=m->local_count||!ir
        ||(o->entry_root_count&&!o->entry_roots)
        ||(o->conventions->entry_count&&!o->conventions->entries)
        ||(linkage->callable_count&&!linkage->callables))
        return VALIDATOR_LINEAGE_INVALID;
    ValidatorLineageResult result=validator_lineage_charge(meter);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    const SolMirMaterializedLocal *local=&m->locals[local_id];
    if(local->kind!=SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
        ||local->source_local>=ir->local_count)
        return VALIDATOR_LINEAGE_INVALID;
    result=validator_lineage_charge(meter);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(ir->locals[local->source_local].kind!=SOL_IR_LOCAL_PARAMETER)
        return VALIDATOR_LINEAGE_INVALID;
    size_t found=SOL_MIR_RUNTIME_NONE;
    for(size_t r=0;r<o->entry_root_count;r++){
        result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        const SolMirRuntimeHostEntryRoot *root=&o->entry_roots[r];
        if(root->entry>=o->conventions->entry_count)continue;
        result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        const SolMirRuntimeEntry *entry=&o->conventions->entries[root->entry];
        if(entry->callable>=linkage->callable_count)continue;
        result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        if(local->instance!=linkage->callables[entry->callable].instance
            ||local->ordinal!=root->formal)continue;
        if(found!=SOL_MIR_RUNTIME_NONE)return VALIDATOR_LINEAGE_INVALID;
        found=r;
    }
    if(found==SOL_MIR_RUNTIME_NONE)return VALIDATOR_LINEAGE_INVALID;
    *root_out=found;
    return VALIDATOR_LINEAGE_OK;
}

static ValidatorLineageResult validator_lineage_place(
    const SolMirMaterialization *m,const SolMirMaterializedImage *image,
    size_t id,size_t instance,SolMirRuntimeHostAbiWorkMeter *meter,
    size_t *local_out,bool whole) {
    if(!m->places||!m->locals||id>=m->place_count)
        return VALIDATOR_LINEAGE_INVALID;
    ValidatorLineageResult result=validator_lineage_charge(meter);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    const SolMirMaterializedPlace *p=&m->places[id];
    if(p->instance!=instance||p->local>=m->local_count
        ||!validator_lineage_member(image->locals.offset,image->locals.count,
            m->local_count,p->local)
        ||p->projections.offset>m->projection_count
        ||p->projections.count>m->projection_count-p->projections.offset
        ||(whole&&p->projections.count))return VALIDATOR_LINEAGE_INVALID;
    result=validator_lineage_charge(meter);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(m->locals[p->local].instance!=instance)return VALIDATOR_LINEAGE_INVALID;
    *local_out=p->local;
    return VALIDATOR_LINEAGE_OK;
}

static ValidatorLineageResult validator_lineage_touches_local(
    const SolMirMaterialization *m,const SolMirMaterializedImage *image,
    const SolMirMaterializedInstruction *x,size_t instance,
    SolMirRuntimeHostAbiWorkMeter *meter,size_t local,bool *changed) {
    *changed=false;
    switch(x->kind){
        case SOL_MIR_INST_STORAGE_LIVE:
        case SOL_MIR_INST_DROP_IF_INITIALIZED:
        case SOL_MIR_INST_STORAGE_DEAD:
            *changed=x->local==local;return VALIDATOR_LINEAGE_OK;
        case SOL_MIR_INST_LOAD_MOVE:
        case SOL_MIR_INST_LOAD_UPDATE:
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
        case SOL_MIR_INST_COMPOUND_UPDATE:{
            size_t target;
            ValidatorLineageResult result=validator_lineage_place(m,image,
                x->place,instance,meter,&target,false);
            if(result!=VALIDATOR_LINEAGE_OK)return result;
            *changed=target==local;return VALIDATOR_LINEAGE_OK;
        }
        default:return VALIDATOR_LINEAGE_OK;
    }
}

static ValidatorLineageResult validator_lineage_local_source(
    const SolMirMaterialization *m,const SolMirMaterializedImage *image,
    size_t instance,size_t block,size_t local,size_t load,size_t type,
    SolMirRuntimeHostAbiWorkMeter *meter,size_t *store_out,size_t *value_out) {
    if(local>=m->local_count||!m->locals||!m->types||type>=m->type_count)
        return VALIDATOR_LINEAGE_INVALID;
    ValidatorLineageResult local_type_charge=validator_lineage_charge(meter);
    if(local_type_charge!=VALIDATOR_LINEAGE_OK)return local_type_charge;
    if(m->locals[local].type!=type)return VALIDATOR_LINEAGE_INVALID;
    size_t store=SOL_MIR_RUNTIME_NONE,value=SOL_MIR_RUNTIME_NONE;
    for(size_t n=0;n<image->instructions.count;n++){
        size_t i=image->instructions.offset+n;
        ValidatorLineageResult result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        const SolMirMaterializedInstruction *x=&m->instructions[i];
        if(x->kind!=SOL_MIR_INST_STORE)continue;
        size_t target;
        result=validator_lineage_place(m,image,x->place,instance,meter,&target,false);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        if(target!=local)continue;
        result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        const SolMirMaterializedPlace *place=&m->places[x->place];
        if(x->type!=SOL_MIR_MATERIALIZED_NONE)
            return VALIDATOR_LINEAGE_INVALID;
        result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        if(place->root_type!=type)return VALIDATOR_LINEAGE_INVALID;
        result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        if(place->final_type!=type)
            return VALIDATOR_LINEAGE_INVALID;
        if(store!=SOL_MIR_RUNTIME_NONE||m->places[x->place].projections.count
            ||x->block!=block)return VALIDATOR_LINEAGE_INVALID;
        store=i;value=x->left;
    }
    if(store==SOL_MIR_RUNTIME_NONE||store>=load||value>=m->value_count)
        return VALIDATOR_LINEAGE_INVALID;
    const SolMirMaterializedBlock *block_row;
    ValidatorLineageResult result=validator_lineage_read_block(m,image,block,meter,
        &block_row);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(!validator_lineage_member(block_row->instructions.offset,
            block_row->instructions.count,m->instruction_count,store)
        ||!validator_lineage_member(block_row->instructions.offset,
            block_row->instructions.count,m->instruction_count,load))
        return VALIDATOR_LINEAGE_INVALID;
    bool looped=false;
    result=validator_lineage_block_is_looped(m,image,block,meter,&looped);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(looped)return VALIDATOR_LINEAGE_INVALID;

    size_t storage_live=SOL_MIR_RUNTIME_NONE;
    for(size_t n=0;n<image->instructions.count;n++){
        size_t i=image->instructions.offset+n;
        if(i>=store)break;
        result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        const SolMirMaterializedInstruction *x=&m->instructions[i];
        if(x->kind!=SOL_MIR_INST_STORAGE_LIVE||x->local!=local)continue;
        if(storage_live!=SOL_MIR_RUNTIME_NONE||x->block!=block)
            return VALIDATOR_LINEAGE_INVALID;
        storage_live=i;
    }
    if(storage_live==SOL_MIR_RUNTIME_NONE)return VALIDATOR_LINEAGE_INVALID;
    for(size_t i=storage_live+1;i<store;i++){
        const SolMirMaterializedInstruction *x;
        result=validator_lineage_read_instruction(m,image,i,meter,&x);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        bool changed=false;
        result=validator_lineage_touches_local(m,image,x,instance,meter,local,
            &changed);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        if(changed)return VALIDATOR_LINEAGE_INVALID;
    }
    const SolMirMaterializedValue *stored_value;
    const SolMirMaterializedInstruction *stored_definition;
    size_t definition_id;
    result=validator_lineage_definition(m,image,value,block,store,meter,
        &stored_value,&stored_definition,&definition_id);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    result=validator_lineage_charge(meter);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(stored_value->type!=type
        ||definition_id>=store||stored_definition->block!=block)
        return VALIDATOR_LINEAGE_INVALID;
    for(size_t i=store+1;i<load;i++){
        const SolMirMaterializedInstruction *x;
        result=validator_lineage_read_instruction(m,image,i,meter,&x);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        bool changed=false;
        result=validator_lineage_touches_local(m,image,x,instance,meter,local,
            &changed);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        if(changed)return VALIDATOR_LINEAGE_INVALID;
    }
    *store_out=store;*value_out=value;
    return VALIDATOR_LINEAGE_OK;
}

static ValidatorLineageResult validator_lineage_trace_plan(
    const SolMirRuntimeHostAbi *o,const SolMirMaterialization *m,size_t plan_id,
    SolMirRuntimeHostAbiWorkMeter *meter,ValidatorLineageEndpoint *endpoint) {
    const SolMirConcreteProgram *c=o->conventions->concrete;
    const SolMirOperations *ops=&c->operations;
    const SolIr *ir=c->program.ir;
    if(plan_id>=ops->constructor_count||!ops->constructors||!ir)
        return VALIDATOR_LINEAGE_INVALID;
    ValidatorLineageResult result=validator_lineage_charge(meter);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    const SolMirOperationConstructPlan *plan=&ops->constructors[plan_id];
    if(plan->kind!=SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
        ||plan->capability_source_operand>=plan->operands.count
        ||plan->operands.offset>ops->construct_operand_count
        ||plan->operands.count>ops->construct_operand_count-plan->operands.offset)
        return VALIDATOR_LINEAGE_INVALID;
    if(plan->image>=m->image_count)return VALIDATOR_LINEAGE_INVALID;
    result=validator_lineage_charge(meter);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    const SolMirMaterializedImage *image=&m->images[plan->image];
    if(image->instructions.offset>m->instruction_count
        ||image->instructions.count>m->instruction_count-image->instructions.offset
        ||image->blocks.offset>m->block_count
        ||image->blocks.count>m->block_count-image->blocks.offset
        ||image->locals.offset>m->local_count
        ||image->locals.count>m->local_count-image->locals.offset
        ||image->values.offset>m->value_count
        ||image->values.count>m->value_count-image->values.offset
        ||image->temporaries.offset>m->temporary_count
        ||image->temporaries.count>m->temporary_count-image->temporaries.offset)
        return VALIDATOR_LINEAGE_INVALID;
    const SolMirMaterializedInstruction *ctor;
    result=validator_lineage_read_instruction(m,image,plan->instruction,meter,&ctor);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(ctor->kind!=SOL_MIR_INST_CONSTRUCT
        ||ctor->construct_kind!=SOL_MIR_CONSTRUCT_CAPABILITY
        ||ctor->result!=plan->result||plan->result>=m->value_count
        ||plan->instruction==SIZE_MAX)return VALIDATOR_LINEAGE_INVALID;
    const SolMirMaterializedValue *ctor_value;
    const SolMirMaterializedInstruction *ctor_def;
    size_t ctor_def_id;
    result=validator_lineage_definition(m,image,plan->result,ctor->block,
        plan->instruction+1,meter,&ctor_value,&ctor_def,&ctor_def_id);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(ctor_def_id!=plan->instruction||ctor_value->type!=ctor->type
        ||ctor_def->kind!=SOL_MIR_INST_CONSTRUCT)return VALIDATOR_LINEAGE_INVALID;
    const SolMirMaterializedBlock *block_row;
    result=validator_lineage_read_block(m,image,ctor->block,meter,&block_row);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(!validator_lineage_member(block_row->instructions.offset,
            block_row->instructions.count,m->instruction_count,plan->instruction))
        return VALIDATOR_LINEAGE_INVALID;
    size_t operand_id=plan->operands.offset+plan->capability_source_operand;
    result=validator_lineage_charge(meter);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    const SolMirOperationConstructOperand *operand=&ops->construct_operands[operand_id];
    if(operand->temporary>=m->temporary_count)return VALIDATOR_LINEAGE_INVALID;
    size_t staging,value_id;
    result=validator_lineage_staging(m,image,operand->temporary,meter,&staging,
        &value_id);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(staging>=plan->instruction)return VALIDATOR_LINEAGE_INVALID;
    const SolMirMaterializedInstruction *init;
    result=validator_lineage_read_instruction(m,image,staging,meter,&init);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(init->block!=ctor->block||init->left!=value_id)
        return VALIDATOR_LINEAGE_INVALID;
    const SolMirMaterializedValue *initial_value;
    const SolMirMaterializedInstruction *initial_def;
    size_t initial_def_id;
    result=validator_lineage_definition(m,image,value_id,ctor->block,staging,
        meter,&initial_value,&initial_def,&initial_def_id);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    result=validator_lineage_charge(meter);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(initial_value->type!=initial_def->type||initial_def->result!=value_id
        ||m->temporaries[operand->temporary].type!=initial_value->type)
        return VALIDATOR_LINEAGE_INVALID;

    if(plan->capability_rule==SOL_MIR_OPERATION_CAPABILITY_ROOT_SOURCE){
        if(initial_def->kind!=SOL_MIR_INST_LOAD_COPY
            &&initial_def->kind!=SOL_MIR_INST_LOAD_MOVE)
            return VALIDATOR_LINEAGE_INVALID;
        size_t local;
        result=validator_lineage_place(m,image,initial_def->place,image->instance,
            meter,&local,true);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        if(local!=plan->inherited_root)return VALIDATOR_LINEAGE_INVALID;
        size_t root;
        result=validator_lineage_entry_root(o,m,local,meter,&root);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        if(initial_def->kind==SOL_MIR_INST_LOAD_COPY){
            if(initial_value->type>=m->type_count)return VALIDATOR_LINEAGE_INVALID;
            result=validator_lineage_charge(meter);
            if(result!=VALIDATOR_LINEAGE_OK)return result;
            if(!m->types[initial_value->type].is_copy)
                return VALIDATOR_LINEAGE_INVALID;
        }
        *endpoint=(ValidatorLineageEndpoint){root,root,
            SOL_MIR_RUNTIME_NONE,false};
        return VALIDATOR_LINEAGE_OK;
    }
    if(plan->capability_rule!=SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE)
        return VALIDATOR_LINEAGE_INVALID;

    size_t current=value_id,before=staging,transfer_count=0;
    for(size_t steps=0;steps<m->instruction_count;steps++){
        const SolMirMaterializedValue *value;
        const SolMirMaterializedInstruction *producer;
        size_t producer_id;
        result=validator_lineage_definition(m,image,current,ctor->block,before,
            meter,&value,&producer,&producer_id);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        if(producer->kind==SOL_MIR_INST_CONSTRUCT){
            size_t candidate=SOL_MIR_RUNTIME_NONE;
            size_t row_id=o->entry_root_count;
            size_t candidate_row=SOL_MIR_RUNTIME_NONE;
            for(size_t q=0;q<ops->constructor_count;q++){
                result=validator_lineage_charge(meter);
                if(result!=VALIDATOR_LINEAGE_OK)return result;
                const SolMirOperationConstructPlan *prior=&ops->constructors[q];
                bool related=prior->result==current||prior->instruction==producer_id;
                if(related){
                    if(prior->result!=current||prior->instruction!=producer_id
                        ||prior->image!=plan->image
                        ||prior->kind!=SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
                        ||q>=plan_id||producer_id>=before
                        ||candidate!=SOL_MIR_RUNTIME_NONE)
                        return VALIDATOR_LINEAGE_INVALID;
                    candidate=q;candidate_row=row_id;
                }
                if(prior->kind==SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
                    &&!add(&row_id,1))return VALIDATOR_LINEAGE_INVALID;
            }
            if(candidate==SOL_MIR_RUNTIME_NONE
                ||candidate_row==SOL_MIR_RUNTIME_NONE)
                return VALIDATOR_LINEAGE_INVALID;
            if(!transfer_count){
                size_t output_temps,output_temp;
                result=validator_lineage_output_temp(m,image,current,ctor->block,
                    producer_id,before,meter,&output_temps,&output_temp);
                if(result!=VALIDATOR_LINEAGE_OK)return result;
                if(output_temps!=1||output_temp!=operand->temporary)
                    return VALIDATOR_LINEAGE_INVALID;
            }
            *endpoint=(ValidatorLineageEndpoint){candidate_row,
                SOL_MIR_RUNTIME_NONE,candidate,true};
            return VALIDATOR_LINEAGE_OK;
        }
        if(producer->kind!=SOL_MIR_INST_LOAD_COPY
            &&producer->kind!=SOL_MIR_INST_LOAD_MOVE)
            return VALIDATOR_LINEAGE_INVALID;
        size_t local;
        result=validator_lineage_place(m,image,producer->place,image->instance,
            meter,&local,true);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        const SolMirMaterializedLocal *local_row=&m->locals[local];
        if(producer->type!=local_row->type||value->type!=local_row->type)
            return VALIDATOR_LINEAGE_INVALID;
        if(producer->kind==SOL_MIR_INST_LOAD_COPY){
            if(local_row->type>=m->type_count)return VALIDATOR_LINEAGE_INVALID;
            result=validator_lineage_charge(meter);
            if(result!=VALIDATOR_LINEAGE_OK)return result;
            if(!m->types[local_row->type].is_copy)return VALIDATOR_LINEAGE_INVALID;
        }
        if(local_row->kind==SOL_MIR_MATERIALIZED_LOCAL_PARAMETER){
            size_t root;
            result=validator_lineage_entry_root(o,m,local,meter,&root);
            if(result!=VALIDATOR_LINEAGE_OK)return result;
            *endpoint=(ValidatorLineageEndpoint){root,root,
                SOL_MIR_RUNTIME_NONE,false};
            return VALIDATOR_LINEAGE_OK;
        }
        if(local_row->kind!=SOL_MIR_MATERIALIZED_LOCAL_BODY
            ||local_row->source_local>=ir->local_count)
            return VALIDATOR_LINEAGE_INVALID;
        result=validator_lineage_charge(meter);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        const SolIrLocal *source=&ir->locals[local_row->source_local];
        if(source->kind!=SOL_IR_LOCAL_BINDING||source->mutable)
            return VALIDATOR_LINEAGE_INVALID;
        size_t store,next_value;
        result=validator_lineage_local_source(m,image,image->instance,ctor->block,
            local,producer_id,local_row->type,meter,&store,&next_value);
        if(result!=VALIDATOR_LINEAGE_OK)return result;
        current=next_value;before=store;++transfer_count;
    }
    (void)initial_def_id;
    return VALIDATOR_LINEAGE_INVALID;
}

static ValidatorLineageResult validator_resolve_base_parent(
    const SolMirRuntimeHostAbi *o,size_t constructor_index,
    SolMirRuntimeHostAbiWorkMeter *meter,size_t *parent_out,size_t *root_out) {
    if(!o||!o->conventions||!o->conventions->concrete||!meter
        ||!parent_out||!root_out)return VALIDATOR_LINEAGE_INVALID;
    const SolMirMaterialization *m=&o->conventions->concrete->materialization;
    const SolMirOperations *ops=&o->conventions->concrete->operations;
    if(!validator_lineage_view_storage_valid(o,m)
        ||constructor_index>=ops->constructor_count||!ops->constructors)
        return VALIDATOR_LINEAGE_INVALID;
    ValidatorLineageResult result=validator_lineage_charge(meter);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    if(ops->constructors[constructor_index].capability_rule
            !=SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE)
        return VALIDATOR_LINEAGE_INVALID;
    ValidatorLineageEndpoint first;
    result=validator_lineage_trace_plan(o,m,constructor_index,meter,&first);
    if(result!=VALIDATOR_LINEAGE_OK)return result;
    size_t expected_parent=first.owner_id,expected_root=first.root_id;
    if(first.constructor_origin){
        size_t cursor=first.constructor_id,ceiling=constructor_index;
        bool reached_root=false;
        for(size_t depth=0;depth<ops->constructor_count;depth++){
            if(cursor>=ceiling||cursor>=ops->constructor_count)
                return VALIDATOR_LINEAGE_INVALID;
            ceiling=cursor;
            result=validator_lineage_charge(meter);
            if(result!=VALIDATOR_LINEAGE_OK)return result;
            const SolMirOperationConstructPlan *ancestor=&ops->constructors[cursor];
            if(ancestor->capability_rule==SOL_MIR_OPERATION_CAPABILITY_PRIVATE_SOURCE)
                return VALIDATOR_LINEAGE_INVALID;
            ValidatorLineageEndpoint next;
            result=validator_lineage_trace_plan(o,m,cursor,meter,&next);
            if(result!=VALIDATOR_LINEAGE_OK)return result;
            if(!next.constructor_origin){
                expected_root=next.root_id;reached_root=true;break;
            }
            cursor=next.constructor_id;
        }
        if(!reached_root||expected_root==SOL_MIR_RUNTIME_NONE)
            return VALIDATOR_LINEAGE_INVALID;
    }
    *parent_out=expected_parent;*root_out=expected_root;
    return VALIDATOR_LINEAGE_OK;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
SolMirRuntimeHostAbiLineageTestOutcome
sol_mir_runtime_host_abi_test_validate_base_parent(
    const SolMirRuntimeHostAbi *owner,size_t constructor_index,size_t limit,
    size_t *parent,size_t *root,size_t *work_used){
    SolMirRuntimeHostAbiWorkMeter meter={limit,0};
    size_t resolved_parent=SIZE_MAX,resolved_root=SIZE_MAX;
    ValidatorLineageResult result=validator_resolve_base_parent(owner,
        constructor_index,&meter,&resolved_parent,&resolved_root);
    if(work_used)*work_used=meter.used;
    if(result==VALIDATOR_LINEAGE_OK){
        if(parent)*parent=resolved_parent;if(root)*root=resolved_root;
        return SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_SUCCEEDED;
    }
    return result==VALIDATOR_LINEAGE_NO_WORK
        ?SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_EXHAUSTED
        :SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_REJECTED;
}
#endif

static size_t call_image(const SolMirRuntimeHostAbi*o,const SolMirRuntimeCall*c,SolMirRuntimeHostAbiWorkMeter *meter){const SolMirLinkage*l=&o->conventions->concrete->linkage;size_t internal=SOL_MIR_RUNTIME_NONE;if(c->target_kind==SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL)internal=c->internal;else if(c->target_kind==SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE&&c->table<l->table_entry_count&&l->table_entries[c->table].target_kind==SOL_MIR_LINKAGE_TARGET_INTERNAL)internal=l->table_entries[c->table].internal;return internal<l->callable_count?image_for_instance(&o->conventions->concrete->materialization,l->callables[internal].instance,meter):SOL_MIR_RUNTIME_NONE;}
static bool owner_reachable(const SolMirRuntimeHostAbi*o,const unsigned char*images,const SolMirRuntimeCall*c){if(c->owner_kind==SOL_MIR_RUNTIME_CALL_OWNER_IMAGE)return c->image<o->conventions->concrete->materialization.image_count&&images[c->image];if(c->predicate>=o->conventions->concrete->operations.predicate_body_count)return false;const SolMirPredicateBody*b=&o->conventions->concrete->operations.predicate_bodies[c->predicate];return b->owner_kind==SOL_MIR_PREDICATE_OWNER_INSTANCE&&b->instance<o->conventions->concrete->materialization.image_count&&images[b->instance];}
static bool mark_images(const SolMirRuntimeHostAbi*o,size_t entry,unsigned char*images,
    SolMirRuntimeHostAbiWorkMeter *meter){const SolMirLinkage*l=&o->conventions->concrete->linkage;const SolMirMaterialization*m=&o->conventions->concrete->materialization;if(entry>=o->conventions->entry_count||o->conventions->entries[entry].callable>=l->callable_count)return false;size_t first=image_for_instance(m,l->callables[o->conventions->entries[entry].callable].instance,meter);if(first==SOL_MIR_RUNTIME_NONE)return false;images[first]=1;bool changed=true;while(changed){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;changed=false;for(size_t i=0;i<o->conventions->call_count;i++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirRuntimeCall*c=&o->conventions->calls[i];size_t target;if(!owner_reachable(o,images,c)||(target=call_image(o,c,meter))==SOL_MIR_RUNTIME_NONE||images[target])continue;images[target]=1;changed=true;}}return true;}
/* Independent provenance fixed point.  A host receiver may be a helper
 * formal, but it must have one and only one entry-root origin and must not be
 * a constructed/private/projection value. */
static bool exact_root_place(const SolMirMaterialization*m,const unsigned char*roots,
    size_t root,SolMirMaterializedPlaceId place,SolMirRuntimeHostAbiWorkMeter *meter){
    if(!sol_mir_runtime_host_abi_work_tick(meter))return false;return place<m->place_count
        &&!m->places[place].projections.count&&m->places[place].local<m->local_count
        &&roots[root*m->local_count+m->places[place].local];}
static bool operand_has_root(const SolMirRuntimeHostAbi*o,const unsigned char*roots,
    size_t root,const SolMirRuntimeOperand*x,SolMirRuntimeHostAbiWorkMeter *meter){const SolMirMaterialization*m=&o->conventions->concrete->materialization;
    if(!x)return false;if(x->value.kind==SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE)
        return exact_root_place(m,roots,root,x->value.id,meter);
    if(x->value.kind!=SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY)return false;
    SolMirMaterializedValueId value;SolMirMaterializedPlaceId place;
    return coordinate(m,x->value.id,&value,&place,meter)&&exact_root_place(m,roots,root,place,meter);}
static bool propagate_roots(const SolMirRuntimeHostAbi*o,size_t entry,const unsigned char*images,
    unsigned char*roots,SolMirRuntimeHostAbiWorkMeter *meter){const SolMirMaterialization*m=&o->conventions->concrete->materialization;
    const SolMirLinkage*l=&o->conventions->concrete->linkage;
    if(entry>=o->conventions->entry_count||o->conventions->entries[entry].callable>=l->callable_count)return false;
    size_t image=image_for_instance(m,l->callables[o->conventions->entries[entry].callable].instance,meter);
    if(image==SOL_MIR_RUNTIME_NONE)return false;
    for(size_t root=0;root<o->entry_root_count;root++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;if(o->entry_roots[root].entry==entry)
        for(size_t i=0;i<m->images[image].locals.count;i++){size_t local=m->images[image].locals.offset+i;
            if(!sol_mir_runtime_host_abi_work_tick(meter))return false;
            if(m->locals[local].kind==SOL_MIR_MATERIALIZED_LOCAL_PARAMETER&&m->locals[local].ordinal==o->entry_roots[root].formal)
                roots[root*m->local_count+local]=1;}}
    bool changed=true;while(changed){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;changed=false;for(size_t ci=0;ci<o->conventions->call_count;ci++){
        if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirRuntimeCall*c=&o->conventions->calls[ci];if(!owner_reachable(o,images,c))continue;
        size_t target=call_image(o,c,meter);if(target==SOL_MIR_RUNTIME_NONE)continue;
        const SolMirMaterializedImage*target_image_=&m->images[target];
        for(size_t oi=0;oi<c->operands.count;oi++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;const SolMirRuntimeOperand*x=&o->conventions->operands[c->operands.offset+oi];
            if(x->signature_slot>=o->conventions->signature_slot_count)continue;
            const SolMirRuntimeSignatureSlot*slot=&o->conventions->signature_slots[x->signature_slot];
            if(slot->role!=SOL_MIR_RUNTIME_SLOT_PARAMETER)continue;
            for(size_t li=0;li<target_image_->locals.count;li++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;size_t local=target_image_->locals.offset+li;
                if(m->locals[local].kind!=SOL_MIR_MATERIALIZED_LOCAL_PARAMETER||m->locals[local].ordinal!=slot->formal)continue;
                for(size_t root=0;root<o->entry_root_count;root++){if(!sol_mir_runtime_host_abi_work_tick(meter))return false;if(o->entry_roots[root].entry==entry
                    &&operand_has_root(o,roots,root,x,meter)&&!roots[root*m->local_count+local]){
                    roots[root*m->local_count+local]=1;changed=true;}}}}}}
    return true;}
static size_t receiver_origin(const SolMirRuntimeHostAbi*o,size_t entry,const unsigned char*roots,
    const SolMirRuntimeOperand*x,SolMirRuntimeHostAbiWorkMeter *meter){size_t found=SOL_MIR_RUNTIME_NONE;
    for(size_t root=0;root<o->entry_root_count;root++) {
        if(!sol_mir_runtime_host_abi_work_tick(meter))return SOL_MIR_RUNTIME_NONE;
        if(o->entry_roots[root].entry==entry&&operand_has_root(o,roots,root,x,meter)) {
            if(found!=SOL_MIR_RUNTIME_NONE)return SOL_MIR_RUNTIME_NONE;
            found=root;
        }
    }
    return found;
}
static bool requirements_complete(const SolMirRuntimeHostAbi*o,
    SolMirRuntimeHostAbiWorkMeter *meter){const SolMirMaterialization*m=&o->conventions->concrete->materialization;
    size_t bytes;if(!mul(o->entry_root_count,m->local_count,&bytes))return false;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if(!measuring_validation_work)
        sol_mir_runtime_host_abi_internal_allocation_attempts += 2;
#endif
    unsigned char*images=calloc(m->image_count?m->image_count:1,1);unsigned char*roots=calloc(bytes?bytes:1,1);
    if(!images||!roots){
        sol_mir_runtime_host_abi_observe_validator_rejection("H37.V.requirements_complete.244.scratch_allocation",NULL);
        sol_mir_runtime_host_abi_observe_value("image_count","initialized","records",m->image_count);
        sol_mir_runtime_host_abi_observe_value("root_bytes","initialized","bytes",bytes);
        free(images);free(roots);return false;
    }size_t expected=0;
    for(size_t entry=0;entry<o->conventions->entry_count;entry++){
        sol_mir_runtime_host_abi_observe_index(0,"entry_index",entry);
        memset(images,0,m->image_count?m->image_count:1);memset(roots,0,bytes?bytes:1);
        if(!sol_mir_runtime_host_abi_work_tick(meter)){free(images);free(roots);return false;}
        if(!mark_images(o,entry,images,meter)){
            sol_mir_runtime_host_abi_observe_validator_rejection("H37.V.requirements_complete.247.mark_images",NULL);
            sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",entry);
            free(images);free(roots);return false;
        }
        if(!propagate_roots(o,entry,images,roots,meter)){
            sol_mir_runtime_host_abi_observe_validator_rejection("H37.V.requirements_complete.247.propagate_roots",NULL);
            sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",entry);
            free(images);free(roots);return false;
        }
        for(size_t ci=0;ci<o->conventions->call_count;ci++){const SolMirRuntimeCall*c=&o->conventions->calls[ci];size_t host=call_host(o,c);
            sol_mir_runtime_host_abi_observe_index(1,"call_index",ci);
            if(!sol_mir_runtime_host_abi_work_tick(meter)){free(images);free(roots);return false;}
            if(!owner_reachable(o,images,c)||host==SOL_MIR_RUNTIME_NONE)continue;
            if(!c->operands.count){
                sol_mir_runtime_host_abi_observe_validator_rejection("H37.V.requirements_complete.251.no_operands",NULL);
                sol_mir_runtime_host_abi_observe_value("call_index","evaluated","index",ci);
                sol_mir_runtime_host_abi_observe_value("call.operands.count","evaluated","count",c->operands.count);
                free(images);free(roots);return false;
            }
            size_t root=receiver_origin(o,entry,roots,&o->conventions->operands[c->operands.offset],meter);
            if(root==SOL_MIR_RUNTIME_NONE){
                sol_mir_runtime_host_abi_observe_validator_rejection("H37.V.requirements_complete.253.receiver_origin",NULL);
                sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",entry);
                sol_mir_runtime_host_abi_observe_value("call_index","evaluated","index",ci);
                free(images);free(roots);return false;
            }bool found=false;
            for(size_t q=0;q<o->requirement_count;q++){sol_mir_runtime_host_abi_observe_index(2,"candidate_requirement_index",q);if(!sol_mir_runtime_host_abi_work_tick(meter)){free(images);free(roots);return false;}found|=o->requirements[q].entry==entry&&o->requirements[q].root==root
                &&o->requirements[q].operation<o->operation_count&&o->operations[o->requirements[q].operation].host==host&&o->requirements[q].call==ci;
            }
            if(!found){
                sol_mir_runtime_host_abi_observe_validator_rejection("H37.V.requirements_complete.257.requirement_missing",NULL);
                sol_mir_runtime_host_abi_observe_value("entry_index","evaluated","index",entry);
                sol_mir_runtime_host_abi_observe_value("root_index","evaluated","index",root);
                sol_mir_runtime_host_abi_observe_value("host_index","evaluated","index",host);
                sol_mir_runtime_host_abi_observe_value("call_index","evaluated","index",ci);
                sol_mir_runtime_host_abi_observe_value("matching_requirement_found","evaluated","boolean",0);
                sol_mir_runtime_host_abi_observe_value("requirement_count","initialized","count",o->requirement_count);
                free(images);free(roots);return false;
            }
            if(!sol_mir_runtime_host_abi_work_tick(meter)){free(images);free(roots);return false;}++expected;
        }
    }
    free(images);free(roots);
    if(expected!=o->requirement_count){
        sol_mir_runtime_host_abi_observe_validator_rejection("H37.V.requirements_complete.258.census_mismatch",NULL);
        sol_mir_runtime_host_abi_observe_value("expected_requirements","evaluated","records",expected);
        sol_mir_runtime_host_abi_observe_value("actual_requirements","evaluated","records",o->requirement_count);
        return false;
    }
    return true;
}

/* Keep validation accounting coupled to the records it actually authenticates,
 * rather than to a closed-form arena census.  In particular the nested grant
 * identity comparisons are charged as they are performed. */
static bool charge_usage_work(const SolMirRuntimeHostAbi *o,
    SolMirRuntimeHostAbiWorkMeter *meter) {
#define RECORDS(member) for(size_t i=0;i<o->member##_count;i++) { sol_mir_runtime_host_abi_observe_index(0,#member "_index",i); if(!sol_mir_runtime_host_abi_work_tick(meter)) return false; }
    RECORDS(capability); RECORDS(entry_root); RECORDS(operation); RECORDS(argument);
    RECORDS(formal); RECORDS(shape); RECORDS(shape_case);
#undef RECORDS
    for(size_t i=0;i<o->requirement_count;i++) {
        bool seen=false;
        sol_mir_runtime_host_abi_observe_index(0,"requirement_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(meter)) return false;
        for(size_t q=0;q<i;q++) {
            sol_mir_runtime_host_abi_observe_index(1,"prior_requirement_index",q);
            if(!sol_mir_runtime_host_abi_work_tick(meter)) return false;
            seen|=o->requirements[q].entry==o->requirements[i].entry
                &&o->requirements[q].root==o->requirements[i].root
                &&o->requirements[q].operation==o->requirements[i].operation;
        }
        if(!seen&&!sol_mir_runtime_host_abi_work_tick(meter)) return false;
    }
    return true;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
static SolMirRuntimeHostAbiBuildOutcome host_abi_validate_observed_body(
    const SolMirRuntimeHostAbi*o,SolDiagnostics*d){
#else
SolMirRuntimeHostAbiBuildOutcome sol_mir_runtime_host_abi_internal_validate(
    const SolMirRuntimeHostAbi*o,SolDiagnostics*d){
#endif
    /* Do not authenticate a predecessor until every one of our own ranges has
     * been proved disjoint from it.  This ordering is observable under the
     * restore-safe alias tests: a forged owner must never cause a predecessor
     * validator to dereference one of our mutable arenas. */
        if(
            sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.01",(!o))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.02",(!complete(o->limits)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.03",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.288.03.capability_count","capability_count",o->capability_count,"capability_capacity",o->capability_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.04",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.288.04.entry_root_count","entry_root_count",o->entry_root_count,"entry_root_capacity",o->entry_root_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.05",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.288.05.operation_count","operation_count",o->operation_count,"operation_capacity",o->operation_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.06",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.288.06.argument_count","argument_count",o->argument_count,"argument_capacity",o->argument_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.07",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.288.07.formal_count","formal_count",o->formal_count,"formal_capacity",o->formal_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.08",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.288.08.shape_count","shape_count",o->shape_count,"shape_capacity",o->shape_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.09",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.288.09.shape_case_count","shape_case_count",o->shape_case_count,"shape_case_capacity",o->shape_case_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.10",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.288.10.requirement_count","requirement_count",o->requirement_count,"requirement_capacity",o->requirement_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.11",(!valid_range(o->capabilities,o->capability_count,sizeof(*o->capabilities))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.12",(!valid_range(o->entry_roots,o->entry_root_count,sizeof(*o->entry_roots))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.13",(!valid_range(o->operations,o->operation_count,sizeof(*o->operations))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.14",(!valid_range(o->arguments,o->argument_count,sizeof(*o->arguments))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.15",(!valid_range(o->formals,o->formal_count,sizeof(*o->formals))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.16",(!valid_range(o->shapes,o->shape_count,sizeof(*o->shapes))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.17",(!valid_range(o->shape_cases,o->shape_case_count,sizeof(*o->shape_cases))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.288.18",(!valid_range(o->requirements,o->requirement_count,sizeof(*o->requirements))))
        ){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.288.1.reject",d,"malformed runtime host ABI owner");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    SolMirRuntimeHostAbiWorkMeter audit_meter={o->limits.max_validation_work,0};
    alias_meter=&audit_meter;
    {const void*arena[]={o->capabilities,o->entry_roots,o->operations,o->arguments,o->formals,o->shapes,o->shape_cases,o->requirements};const size_t count[]={o->capability_count,o->entry_root_count,o->operation_count,o->argument_count,o->formal_count,o->shape_count,o->shape_case_count,o->requirement_count};const size_t size[]={sizeof(*o->capabilities),sizeof(*o->entry_roots),sizeof(*o->operations),sizeof(*o->arguments),sizeof(*o->formals),sizeof(*o->shapes),sizeof(*o->shape_cases),sizeof(*o->requirements)};for(size_t i=0;i<8;i++)for(size_t j=i+1;j<8;j++)if(overlap(arena[i],count[i],size[i],arena[j],count[j],size[j])){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.291.1.reject",d,"runtime host ABI arenas overlap");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}}
        if(
            sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.292.01",(aliases_host(o,o,1,sizeof(*o))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.292.02",(!o->conventions))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.292.03",(!o->values))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.292.04",(!o->cleanup))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.292.05",(aliases_host(o,o->conventions,1,sizeof(*o->conventions))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.292.06",(aliases_host(o,o->values,1,sizeof(*o->values))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.292.07",(aliases_host(o,o->cleanup,1,sizeof(*o->cleanup))))
        ){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.292.1.reject",d,"runtime host ABI aliases owner or predecessor");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
#define EARLY_CONVENTIONS_ARENA(member,type,singular) if(aliases_host(o,o->conventions->member,o->conventions->singular##_count,sizeof(type))){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.293." #member ".reject",d,"runtime host ABI aliases convention arena");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(EARLY_CONVENTIONS_ARENA)
#undef EARLY_CONVENTIONS_ARENA
#define EARLY_VALUES_ARENA(member,type,singular) if(aliases_host(o,o->values->member,o->values->singular##_count,sizeof(type))){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.296." #member ".reject",d,"runtime host ABI aliases values arena");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    EARLY_VALUES_ARENA(recipe_operations,SolMirRuntimeRecipeOperations,recipe_operation) EARLY_VALUES_ARENA(allocation_plans,SolMirRuntimeAllocationPlan,allocation_plan) EARLY_VALUES_ARENA(copy_plans,SolMirRuntimeCopyPlan,copy_plan) EARLY_VALUES_ARENA(equality_plans,SolMirRuntimeEqualityPlan,equality_plan) EARLY_VALUES_ARENA(host_result_plans,SolMirRuntimeHostResultPlan,host_result_plan) EARLY_VALUES_ARENA(host_result_requirements,SolMirRuntimeHostResultRequirement,host_result_requirement) EARLY_VALUES_ARENA(ownership_plans,SolMirRuntimeOwnershipPlan,ownership_plan) EARLY_VALUES_ARENA(ownership_variants,SolMirRuntimeOwnershipVariant,ownership_variant) EARLY_VALUES_ARENA(owned_edges,SolMirRuntimeOwnedEdge,owned_edge)
#undef EARLY_VALUES_ARENA
#define EARLY_CLEANUP_ARENA(member,type,singular) if(aliases_host(o,o->cleanup->member,o->cleanup->singular##_count,sizeof(type))){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.299." #member ".reject",d,"runtime host ABI aliases cleanup arena");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    EARLY_CLEANUP_ARENA(events,SolMirRuntimeCleanupEvent,event) EARLY_CLEANUP_ARENA(actions,SolMirRuntimeCleanupAction,action) EARLY_CLEANUP_ARENA(transitions,SolMirRuntimeCleanupTransition,transition) EARLY_CLEANUP_ARENA(supplemental_sites,SolMirRuntimeCleanupSupplementalSite,supplemental_site) EARLY_CLEANUP_ARENA(drop_paths,SolMirRuntimeCleanupDropPath,drop_path)
#undef EARLY_CLEANUP_ARENA
        if(
            sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.302.01",(!o->conventions->concrete))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.302.02",(aliases_host(o,o->conventions->concrete,1,sizeof(*o->conventions->concrete))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.302.03",(aliases_host(o,&o->conventions->concrete->program,1,sizeof(o->conventions->concrete->program))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.302.04",(aliases_host(o,&o->conventions->concrete->materialization,1,sizeof(o->conventions->concrete->materialization))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.302.05",(aliases_host(o,&o->conventions->concrete->representation,1,sizeof(o->conventions->concrete->representation))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.302.06",(aliases_host(o,&o->conventions->concrete->layout,1,sizeof(o->conventions->concrete->layout))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.302.07",(aliases_host(o,&o->conventions->concrete->operations,1,sizeof(o->conventions->concrete->operations))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.302.08",(aliases_host(o,&o->conventions->concrete->linkage,1,sizeof(o->conventions->concrete->linkage))))
        ){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.302.1.reject",d,"runtime host ABI aliases transitive predecessor owner");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    if(aliases_transitive(o,&audit_meter)){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.303.1.reject",d,"runtime host ABI aliases transitive predecessor arena");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        if(
            sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.01",(!o))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.02",(!o->conventions))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.03",(!o->values))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.04",(!o->cleanup))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.05",(o->values->conventions!=o->conventions))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.06",(o->cleanup->conventions!=o->conventions))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.07",(o->cleanup->values!=o->values))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.08",(!sol_mir_runtime_conventions_validate(o->conventions,d)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.09",(!sol_mir_runtime_values_validate(o->values,d)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.10",(!sol_mir_runtime_cleanup_validate(o->cleanup,d)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.11",(!complete(o->limits)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.12",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.304.12.capability_count","capability_count",o->capability_count,"capability_capacity",o->capability_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.13",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.304.13.entry_root_count","entry_root_count",o->entry_root_count,"entry_root_capacity",o->entry_root_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.14",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.304.14.operation_count","operation_count",o->operation_count,"operation_capacity",o->operation_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.15",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.304.15.argument_count","argument_count",o->argument_count,"argument_capacity",o->argument_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.16",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.304.16.formal_count","formal_count",o->formal_count,"formal_capacity",o->formal_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.17",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.304.17.shape_count","shape_count",o->shape_count,"shape_capacity",o->shape_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.18",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.304.18.shape_case_count","shape_case_count",o->shape_case_count,"shape_case_capacity",o->shape_case_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.19",(sol_mir_runtime_host_abi_observe_ne("H37.V.sol_mir_runtime_host_abi_internal_validate.304.19.requirement_count","requirement_count",o->requirement_count,"requirement_capacity",o->requirement_capacity)))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.20",(!valid_range(o->capabilities,o->capability_count,sizeof(*o->capabilities))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.21",(!valid_range(o->entry_roots,o->entry_root_count,sizeof(*o->entry_roots))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.22",(!valid_range(o->operations,o->operation_count,sizeof(*o->operations))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.23",(!valid_range(o->arguments,o->argument_count,sizeof(*o->arguments))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.24",(!valid_range(o->formals,o->formal_count,sizeof(*o->formals))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.25",(!valid_range(o->shapes,o->shape_count,sizeof(*o->shapes))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.26",(!valid_range(o->shape_cases,o->shape_case_count,sizeof(*o->shape_cases))))
            ||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.304.27",(!valid_range(o->requirements,o->requirement_count,sizeof(*o->requirements))))
        ){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.304.1.reject",d,"malformed runtime host ABI owner");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    const void*arena[]={o->capabilities,o->entry_roots,o->operations,o->arguments,o->formals,o->shapes,o->shape_cases,o->requirements};const size_t count[]={o->capability_count,o->entry_root_count,o->operation_count,o->argument_count,o->formal_count,o->shape_count,o->shape_case_count,o->requirement_count};const size_t size[]={sizeof(*o->capabilities),sizeof(*o->entry_roots),sizeof(*o->operations),sizeof(*o->arguments),sizeof(*o->formals),sizeof(*o->shapes),sizeof(*o->shape_cases),sizeof(*o->requirements)};for(size_t i=0;i<8;i++)for(size_t j=i+1;j<8;j++)if(sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.305.arena_overlap",overlap(arena[i],count[i],size[i],arena[j],count[j],size[j]))){sol_mir_runtime_host_abi_observe_value("arena_left_index","evaluated","index",i);sol_mir_runtime_host_abi_observe_value("arena_right_index","evaluated","index",j);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.305.1.reject",d,"runtime host ABI arenas overlap");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    if(sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.306.01.conventions_alias",aliases_host(o,o->conventions,1,sizeof(*o->conventions)))||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.306.02.values_alias",aliases_host(o,o->values,1,sizeof(*o->values)))||sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.306.03.cleanup_alias",aliases_host(o,o->cleanup,1,sizeof(*o->cleanup)))){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.306.1.reject",d,"runtime host ABI aliases predecessor");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
#define PREDECESSOR_ARENA(member,type,singular) if(sol_mir_runtime_host_abi_observe_reject("H37.V.sol_mir_runtime_host_abi_internal_validate.307." #member ".alias",aliases_host(o,o->conventions->member,o->conventions->singular##_count,sizeof(type)))){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.307.1.reject",d,"runtime host ABI aliases convention arena");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(PREDECESSOR_ARENA)
#undef PREDECESSOR_ARENA
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if(fail_validation_scratch){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.311.1.reject",d,"runtime host ABI validation scratch allocation failed");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_ALLOCATION_FAILED;}
#endif
    alias_meter=NULL;
    const SolMirRepresentation*r=&o->conventions->concrete->representation;const SolMirLinkage*l=&o->conventions->concrete->linkage;
    if(!frozen_e6_surface(o,&audit_meter)){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.315.1.reject",d,"host surface is not the closed E6 identity table");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if(!measuring_validation_work)
        sol_mir_runtime_host_abi_internal_allocation_attempts += 2;
#endif
    unsigned char*active=calloc(r->recipe_count?r->recipe_count:1,1);unsigned char*seen=calloc(o->shape_count?o->shape_count:1,1);if(!active||!seen){free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.320.1.reject",d,"runtime host ABI validation scratch allocation failed");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_ALLOCATION_FAILED;}
    size_t roots=0;
    for(size_t e=0;e<o->conventions->entry_count;e++){
        sol_mir_runtime_host_abi_observe_index(0,"entry_index",e);
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        const SolMirRuntimeEntry*entry=&o->conventions->entries[e];
        if(entry->signature>=o->conventions->signature_count){free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.325.1.reject",d,"entry signature");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        const SolMirRuntimeSignature*s=&o->conventions->signatures[entry->signature];
        for(size_t q=0;q<s->slots.count;q++){
            sol_mir_runtime_host_abi_observe_index(1,"signature_slot_index",q);
            if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
            const SolMirRuntimeSignatureSlot*slot=&o->conventions->signature_slots[s->slots.offset+q];
            if(slot->role==SOL_MIR_RUNTIME_SLOT_PARAMETER&&slot->recipe<r->recipe_count&&r->recipes[slot->recipe].kind==SOL_MIR_RECIPE_CAPABILITY){
                if(roots>=o->entry_root_count||sol_mir_runtime_host_abi_observe_compare_once(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.331.entry-root.row",
                    &o->entry_roots[roots],
                    &(SolMirRuntimeHostEntryRoot){e,slot->formal,slot->recipe,entry->signature},
                    sizeof(*o->entry_roots),roots,e,false)){free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.331.1.reject",d,"entry root mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
                ++roots;
            }
        }
    }
    if(roots!=o->entry_root_count){free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.336.1.reject",d,"entry root census mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    /* Construction instances are authoritative: a capability plan may not be
     * replaced by another instance merely because its recipe is identical. */
    size_t capability=0;
    for(size_t i=0;i<o->entry_root_count;i++){
        sol_mir_runtime_host_abi_observe_index(0,"entry_root_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        const SolMirRuntimeHostEntryRoot*x=&o->entry_roots[i];
        SolMirRuntimeHostCapabilityPlan want={x->recipe,SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT,
            SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,
            SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,SOL_MIR_RUNTIME_NONE,i,
            SOL_MIR_RUNTIME_NONE,SOL_MIR_RECIPE_NONE,SOL_MIR_RUNTIME_NONE};
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        size_t observed_capability_index=capability;
#endif
         if(capability>=o->capability_count
            ||!capability_fields_equal_once(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.348.capability_fields_equal",
                &o->capabilities[capability++],&want,
                observed_capability_index,i)){
            free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.349.1.reject",d,"entry capability source mismatch");
            return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;
        }
    }
    const SolMirMaterialization*cap_m=&o->conventions->concrete->materialization;
    const SolMirOperations*cap_ops=&o->conventions->concrete->operations;
    for(size_t i=0;i<cap_ops->constructor_count;i++){
        sol_mir_runtime_host_abi_observe_index(0,"constructor_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        const SolMirOperationConstructPlan*p=&cap_ops->constructors[i];
        if(p->kind!=SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY)continue;
        if(p->capability_source_operand>=p->operands.count){free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.359.1.reject",d,"capability source selection");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        const SolMirOperationConstructOperand*operand=&cap_ops->construct_operands[p->operands.offset+p->capability_source_operand];
        SolMirMaterializedTemporaryId temporary;
        SolMirMaterializedValueId source_value;
        SolMirMaterializedPlaceId source_place;
        if(!result_temporary_for(cap_m,p->result,&temporary,&audit_meter)) temporary=SOL_MIR_RUNTIME_NONE;
        if(!coordinate(cap_m,operand->temporary,&source_value,&source_place,&audit_meter)){
            free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.366.1.reject",d,"capability materialization coordinate");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;
        }
        SolMirRuntimeHostCapabilitySource source=p->capability_rule==SOL_MIR_OPERATION_CAPABILITY_PRIVATE_SOURCE
            ?SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE
            :p->capability_rule==SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE
                ?SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED:SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT;
        size_t root=SOL_MIR_RUNTIME_NONE,parent=SOL_MIR_RUNTIME_NONE;
        if(source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT){
            root=root_for_local(o,p->inherited_root,&audit_meter);
            if(root==SOL_MIR_RUNTIME_NONE||source_place>=cap_m->place_count
                ||cap_m->places[source_place].local!=p->inherited_root
                ||cap_m->places[source_place].projections.count){free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.377.1.reject",d,"capability root coordinate");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
            parent=root;
        }else if(source==SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED){
            ValidatorLineageResult lineage=validator_resolve_base_parent(o,i,
                &audit_meter,&parent,&root);
            if(lineage==VALIDATOR_LINEAGE_NO_WORK){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
            if(lineage!=VALIDATOR_LINEAGE_OK
                ||parent==SOL_MIR_RUNTIME_NONE||root==SOL_MIR_RUNTIME_NONE){free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.387.1.reject",d,"unresolved capability parent");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        }
        SolMirRuntimeHostCapabilityPlan want={p->result_recipe,source,i,p->result,temporary,
            source_value,operand->temporary,source_place,root,parent,
            source==SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE?operand->recipe:SOL_MIR_RECIPE_NONE,
            source==SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE?source_value:SOL_MIR_RUNTIME_NONE};
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        size_t observed_capability_index=capability;
#endif
         if(capability>=o->capability_count
            ||!capability_fields_equal_once(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.394.capability_fields_equal",
                &o->capabilities[capability++],&want,
                observed_capability_index,i)){
            free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.395.1.reject",d,"constructed capability source mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;
        }
    }
    if(capability!=o->capability_count){free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.398.1.reject",d,"capability source census mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    for(size_t i=0;i<o->capability_count;i++){
        sol_mir_runtime_host_abi_observe_index(0,"capability_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        const SolMirRuntimeHostCapabilityPlan*x=&o->capabilities[i];
#define CAPABILITY_LINEAGE_REJECT(line,atom,message) do { \
    observe_capability_lineage_unavailable(line,#atom); \
    sol_mir_runtime_host_abi_observe_fieldwise_row( \
        "H37.V.sol_mir_runtime_host_abi_internal_validate." #line "." #atom ".capability.row", \
        x,sizeof(*x),i); \
    free(active);free(seen); \
    bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate." #line ".1.reject",d,message); \
    return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR; \
} while (0)
        if(x->source==SOL_MIR_RUNTIME_HOST_CAPABILITY_ROOT){
            if(sol_mir_runtime_host_abi_observe_ge(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.403.01.source_root_bounds",
                    "capability.source_root",x->source_root,"entry_root_count",
                    o->entry_root_count))
                CAPABILITY_LINEAGE_REJECT(403,source_root_bounds,"capability root lineage");
            size_t source_construct=x->source_construct;
            size_t expected_parent=source_construct==SOL_MIR_RUNTIME_NONE
                ?SOL_MIR_RUNTIME_NONE:x->source_root;
            if(sol_mir_runtime_host_abi_observe_ne(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.404.02.parent",
                    "capability.parent",x->parent,"expected.parent",expected_parent)){
                sol_mir_runtime_host_abi_observe_value("capability.source_construct",
                    "evaluated","index",source_construct);
                CAPABILITY_LINEAGE_REJECT(404,parent,"capability root lineage");
            }
            if(sol_mir_runtime_host_abi_observe_ne(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.406.03.private_recipe",
                    "capability.private_recipe",x->private_recipe,"none",
                    SOL_MIR_RECIPE_NONE))
                CAPABILITY_LINEAGE_REJECT(406,private_recipe,"capability root lineage");
            if(sol_mir_runtime_host_abi_observe_ne(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.406.04.private_value",
                    "capability.private_value",x->private_value,"none",
                    SOL_MIR_RUNTIME_NONE))
                CAPABILITY_LINEAGE_REJECT(406,private_value,"capability root lineage");
        }else if(x->source==SOL_MIR_RUNTIME_HOST_CAPABILITY_DERIVED){
            if(sol_mir_runtime_host_abi_observe_ge(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.408.01.parent_bounds",
                    "capability.parent",x->parent,"capability_index",i))
                CAPABILITY_LINEAGE_REJECT(408,parent_bounds,"capability derived lineage");
            if(sol_mir_runtime_host_abi_observe_ge(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.408.02.source_root_bounds",
                    "capability.source_root",x->source_root,"entry_root_count",
                    o->entry_root_count))
                CAPABILITY_LINEAGE_REJECT(408,source_root_bounds,"capability derived lineage");
            if(sol_mir_runtime_host_abi_observe_eq(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.409.03.parent_private_source",
                    "parent.source",(size_t)o->capabilities[x->parent].source,
                    "private_source",(size_t)SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE))
                CAPABILITY_LINEAGE_REJECT(409,parent_private_source,"capability derived lineage");
            if(sol_mir_runtime_host_abi_observe_ne(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.410.04.parent_source_root",
                    "parent.source_root",o->capabilities[x->parent].source_root,
                    "capability.source_root",x->source_root))
                CAPABILITY_LINEAGE_REJECT(410,parent_source_root,"capability derived lineage");
            if(sol_mir_runtime_host_abi_observe_ne(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.411.05.private_recipe",
                    "capability.private_recipe",x->private_recipe,"none",
                    SOL_MIR_RECIPE_NONE))
                CAPABILITY_LINEAGE_REJECT(411,private_recipe,"capability derived lineage");
            if(sol_mir_runtime_host_abi_observe_ne(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.411.06.private_value",
                    "capability.private_value",x->private_value,"none",
                    SOL_MIR_RUNTIME_NONE))
                CAPABILITY_LINEAGE_REJECT(411,private_value,"capability derived lineage");
        }else if(x->source==SOL_MIR_RUNTIME_HOST_CAPABILITY_PRIVATE_SOURCE){
            if(sol_mir_runtime_host_abi_observe_ne(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.414.01.source_root",
                    "capability.source_root",x->source_root,"none",SOL_MIR_RUNTIME_NONE))
                CAPABILITY_LINEAGE_REJECT(414,source_root,"capability private lineage");
            if(sol_mir_runtime_host_abi_observe_ne(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.414.02.parent",
                    "capability.parent",x->parent,"none",SOL_MIR_RUNTIME_NONE))
                CAPABILITY_LINEAGE_REJECT(414,parent,"capability private lineage");
            if(sol_mir_runtime_host_abi_observe_eq(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.414.03.private_recipe_missing",
                    "capability.private_recipe",x->private_recipe,"none",
                    SOL_MIR_RECIPE_NONE))
                CAPABILITY_LINEAGE_REJECT(414,private_recipe_missing,"capability private lineage");
            if(sol_mir_runtime_host_abi_observe_ne(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.414.04.private_value",
                    "capability.private_value",x->private_value,"source_value",
                    x->source_value))
                CAPABILITY_LINEAGE_REJECT(414,private_value,"capability private lineage");
        }else{
            sol_mir_runtime_host_abi_observe_failure(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.415.01.source_class",
                "validator","original capability lineage class predicate rejected",NULL);
            sol_mir_runtime_host_abi_observe_value("capability.source","evaluated",
                "enum",(size_t)x->source);
            CAPABILITY_LINEAGE_REJECT(415,source_class,"capability lineage class");
        }
#undef CAPABILITY_LINEAGE_REJECT
    }
    size_t formal=0;
    for(size_t h=0;h<l->host_requirement_count;h++){
        sol_mir_runtime_host_abi_observe_index(0,"host_index",h);
        if(!e3_host_requirement(o,h,&audit_meter)){free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.419.1.reject",d,"host profile is outside frozen E3 surface");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        const SolMirLinkageHostRequirement*host=&l->host_requirements[h];
        if(h>=o->operation_count){free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.421.1.reject",d,"operation census");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        const SolMirRuntimeHostOperation*x=&o->operations[h];
        size_t import=SOL_MIR_RUNTIME_NONE,sig=SOL_MIR_RUNTIME_NONE;
        for(size_t i=0;i<o->conventions->import_count;i++){
            sol_mir_runtime_host_abi_observe_index(1,"import_index",i);
            if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
            if(o->conventions->imports[i].kind==SOL_MIR_RUNTIME_IMPORT_HOST&&o->conventions->imports[i].host==h)import=i;
        }
        for(size_t i=0;i<o->conventions->signature_count;i++){
            sol_mir_runtime_host_abi_observe_index(1,"signature_index",i);
            if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
            if(o->conventions->signatures[i].origin==SOL_MIR_RUNTIME_SIGNATURE_HOST&&o->conventions->signatures[i].host==h)sig=i;
        }
        if(sol_mir_runtime_host_abi_observe_eq(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.01.import_is_none",
                "import",import,"none",SOL_MIR_RUNTIME_NONE)
            ||sol_mir_runtime_host_abi_observe_eq(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.02.signature_is_none",
                "signature",sig,"none",SOL_MIR_RUNTIME_NONE)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.03.operation_host",
                "operation.host",x->host,"host_requirement",h)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.04.operation_import",
                "operation.import_id",x->import_id,"selected_import",import)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.05.operation_signature",
                "operation.signature",x->signature,"selected_signature",sig)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.06.operation_receiver",
                "operation.receiver",x->receiver,"host.receiver",host->receiver)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.07.receiver_access",
                "operation.receiver_access",(size_t)x->receiver_access,
                "host.receiver_access",(size_t)host->receiver_access)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.08.operation_result",
                "operation.result",x->result,"host.result",host->result)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.09.operation_result_plan",
                "operation.result_plan",x->result_plan,"host.result",host->result)
            ||sol_mir_runtime_host_abi_observe_ge(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.10.host_result_bounds",
                "host.result",host->result,"host_result_plan_count",
                o->values->host_result_plan_count)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.11.result_class",
                "operation.result_class",(size_t)x->result_class,
                "selected.result_class",(size_t)o->values->host_result_plans[host->result].classification)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.12.effects",
                "operation.effects",x->effects,"host.effects",host->effects)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.13.failure_mask",
                "operation.failure_mask",x->failure_mask,"expected.failure_mask",
                HOST_FAILURE_MASK)
            ||sol_mir_runtime_host_abi_observe_compare_once(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.identity",
                &x->identity,&o->conventions->imports[import].identity,
                sizeof(x->identity),h,import,false)
            ||sol_mir_runtime_host_abi_observe_compare_once(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.symbol",
                &x->symbol,&o->conventions->imports[import].symbol,
                sizeof(x->symbol),h,import,false)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.16.formals_offset",
                "operation.formals.offset",x->formals.offset,"formal_cursor",formal)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.17.formals_count",
                "operation.formals.count",x->formals.count,
                "host.parameters.count",host->parameters.count)){
            sol_mir_runtime_host_abi_observe_fieldwise_row(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.432.host-operation.row",
                x,sizeof(*x),h);
            free(active);free(seen);
            bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.432.1.reject",d,"host surface mismatch");
            return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;
        }
        for(size_t q=0;q<host->parameters.count;q++){
            sol_mir_runtime_host_abi_observe_index(1,"parameter_index",q);
            if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
            if(sol_mir_runtime_host_abi_observe_ge(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.435.formal_bounds",
                    "formal_index",formal,"formal_count",o->formal_count)){
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "formal.recipe","not_reached","recipe index");
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "expected.recipe","not_reached","recipe index");
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "formal.access","not_reached","access mode");
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "expected.access","not_reached","access mode");
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "formal.shape","not_reached","shape index");
                sol_mir_runtime_host_abi_observe_capture_gap(
                    "formal row unavailable: formal index is outside formal_count");
                free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.435.1.reject",d,"host formal or E3 shape mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
            if(sol_mir_runtime_host_abi_observe_ne(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.435.formal_recipe",
                    "formal.recipe",o->formals[formal].recipe,"expected.recipe",
                    o->conventions->concrete->materialization.type_ids[host->parameters.offset+q])){
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "formal.access","not_reached","access mode");
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "expected.access","not_reached","access mode");
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "formal.shape","not_reached","shape index");
                sol_mir_runtime_host_abi_observe_fieldwise_row(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.435.formal.row",
                    &o->formals[formal],sizeof(o->formals[formal]),formal);
                free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.435.1.reject",d,"host formal or E3 shape mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
            if(sol_mir_runtime_host_abi_observe_ne(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.435.formal_access",
                    "formal.access",(size_t)o->formals[formal].access,
                    "expected.access",(size_t)o->conventions->concrete->materialization.accesses[host->parameter_accesses.offset+q])){
                sol_mir_runtime_host_abi_observe_unavailable_value(
                    "formal.shape","not_reached","shape index");
                sol_mir_runtime_host_abi_observe_fieldwise_row(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.435.formal.row",
                    &o->formals[formal],sizeof(o->formals[formal]),formal);
                free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.435.1.reject",d,"host formal or E3 shape mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
            if(sol_mir_runtime_host_abi_observe_reject(
                    "H37.V.sol_mir_runtime_host_abi_internal_validate.435.shape_rejection",
                    !validate_shape(o,o->formals[formal].shape,active,seen,&audit_meter))){
                free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.435.1.reject",d,"host formal or E3 shape mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
            ++formal;
        }
    }
    for(size_t i=0;i<o->shape_count;i++){
        sol_mir_runtime_host_abi_observe_index(0,"shape_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        if(!seen[i]){free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.441.1.reject",d,"unconsumed or disconnected host shape");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    }
    if(o->operation_count!=l->host_requirement_count||formal!=o->formal_count){free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.443.1.reject",d,"host formal census mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    for(size_t i=0;i<o->shape_count;i++){
        sol_mir_runtime_host_abi_observe_index(0,"shape_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){free(active);free(seen);return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        if(!seen[i]){free(active);free(seen);bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.446.1.reject",d,"orphan host shape");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    }
    free(active);free(seen);
    /* The flat argument census deliberately mirrors formal order.  It gives
     * max_arguments an independently authenticated meaning while recursive
     * selection remains owned by the formal's shape root. */
    if(o->argument_count!=o->formal_count){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.452.1.reject",d,"host argument census mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    for(size_t i=0;i<o->argument_count;i++){sol_mir_runtime_host_abi_observe_index(0,"argument_index",i);SolMirRuntimeHostArgumentKind want;
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.454.1.reject",d,"runtime host ABI validation work exhausted");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        const SolMirRuntimeHostArgument*x=&o->arguments[i];
        const SolMirRuntimeHostFormal*f=&o->formals[i];
        if(sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.460.recipe",
                "argument.recipe",x->recipe,"formal.recipe",f->recipe)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.460.access",
                "argument.access",(size_t)x->access,"formal.access",(size_t)f->access)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.460.children_count",
                "children.count",x->children.count,"expected.children.count",0)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.460.children_offset",
                "children.offset",x->children.offset,"expected.children.offset",0)
            ||sol_mir_runtime_host_abi_observe_ge(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.460.recipe_bounds",
                "argument.recipe",x->recipe,"recipe_count",r->recipe_count)
            ||sol_mir_runtime_host_abi_observe_reject(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.460.kind_lookup",
                !kind(&r->recipes[x->recipe],&want))
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.460.kind",
                "argument.kind",(size_t)x->kind,"expected.kind",(size_t)want)){
            sol_mir_runtime_host_abi_observe_fieldwise_row("H37.V.sol_mir_runtime_host_abi_internal_validate.460.argument.row",x,sizeof(*x),i);
            bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.460.1.reject",d,"host argument descriptor mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;
        }
    }
    for(size_t i=0;i<o->requirement_count;i++){
        sol_mir_runtime_host_abi_observe_index(0,"requirement_index",i);
        if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.464.1.reject",d,"runtime host ABI validation work exhausted");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
        const SolMirRuntimeHostRequirement*q=&o->requirements[i];
        if(sol_mir_runtime_host_abi_observe_ge(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.466.entry_bounds",
                "requirement.entry",q->entry,"entry_count",o->conventions->entry_count)
            ||sol_mir_runtime_host_abi_observe_ge(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.466.root_bounds",
                "requirement.root",q->root,"entry_root_count",o->entry_root_count)
            ||sol_mir_runtime_host_abi_observe_ge(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.466.operation_bounds",
                "requirement.operation",q->operation,"operation_count",o->operation_count)
            ||sol_mir_runtime_host_abi_observe_ge(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.466.call_bounds",
                "requirement.call",q->call,"call_count",o->conventions->call_count)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.466.entry_root_owner",
                "entry_root.entry",o->entry_roots[q->root].entry,
                "requirement.entry",q->entry)
            ||sol_mir_runtime_host_abi_observe_ne(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.466.call_host",
                "call.host",call_host(o,&o->conventions->calls[q->call]),
                "operation.host",o->operations[q->operation].host)
            ||sol_mir_runtime_host_abi_observe_reject(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.466.cleanup_mapping",
                !cleanup_mapping(o,q))){
            sol_mir_runtime_host_abi_observe_fieldwise_row("H37.V.sol_mir_runtime_host_abi_internal_validate.466.requirement.row",q,sizeof(*q),i);
            bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.466.1.reject",d,"entry requirement transition mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        for(size_t j=0;j<i;j++){
            sol_mir_runtime_host_abi_observe_index(1,"prior_requirement_index",j);
            if(!sol_mir_runtime_host_abi_work_tick(&audit_meter)){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.468.1.reject",d,"runtime host ABI validation work exhausted");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}
            if(!sol_mir_runtime_host_abi_observe_compare_once(
                "H37.V.sol_mir_runtime_host_abi_internal_validate.469.requirement.duplicate",
                q,&o->requirements[j],sizeof(*q),i,j,true)){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.469.1.reject",d,"duplicate entry requirement");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
        }
    }
    SolMirRuntimeHostAbiWorkMeter validation_meter={o->limits.max_validation_work-audit_meter.used,0};
    if(!charge_usage_work(o,&validation_meter)||!requirements_complete(o,&validation_meter)){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.473.1.reject",d,"entry requirement closure mismatch");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
    size_t bytes=0,t;if(!mul(o->capability_count,sizeof(*o->capabilities),&t)||!add(&bytes,t)||!mul(o->entry_root_count,sizeof(*o->entry_roots),&t)||!add(&bytes,t)||!mul(o->operation_count,sizeof(*o->operations),&t)||!add(&bytes,t)||!mul(o->argument_count,sizeof(*o->arguments),&t)||!add(&bytes,t)||!mul(o->formal_count,sizeof(*o->formals),&t)||!add(&bytes,t)||!mul(o->shape_count,sizeof(*o->shapes),&t)||!add(&bytes,t)||!mul(o->shape_case_count,sizeof(*o->shape_cases),&t)||!add(&bytes,t)||!mul(o->requirement_count,sizeof(*o->requirements),&t)||!add(&bytes,t)){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.474.1.reject",d,"runtime host ABI usage overflow");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}size_t grants=0;for(size_t i=0;i<o->requirement_count;i++){if(!sol_mir_runtime_host_abi_work_tick(&validation_meter)){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.474.2.reject",d,"runtime host ABI validation work exhausted");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}bool seen_grant=false;for(size_t q=0;q<i;q++){if(!sol_mir_runtime_host_abi_work_tick(&validation_meter)){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.474.3.reject",d,"runtime host ABI validation work exhausted");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_RESOURCE_EXHAUSTED;}seen_grant|=o->requirements[q].entry==o->requirements[i].entry&&o->requirements[q].root==o->requirements[i].root&&o->requirements[q].operation==o->requirements[i].operation;}if(!seen_grant)++grants;}const SolMirMaterialization*m=&o->conventions->concrete->materialization;size_t root_bits,grant_bits,root_scratch,shape_scratch=0,validation_roots=0;if(!mul(o->entry_root_count,m->local_count,&root_bits)||!mul(o->entry_root_count,l->host_requirement_count,&grant_bits)||!mul(o->entry_root_count,sizeof(*o->entry_roots),&root_scratch)||!add(&root_scratch,m->image_count)||!add(&root_scratch,root_bits)||!add(&root_scratch,grant_bits)||!add(&shape_scratch,r->recipe_count)||!add(&shape_scratch,o->shape_count)||!add(&validation_roots,m->image_count)||!add(&validation_roots,root_bits)){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.474.4.reject",d,"runtime host ABI scratch overflow");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}size_t build_scratch=(r->recipe_count?r->recipe_count:1)>root_scratch?(r->recipe_count?r->recipe_count:1):root_scratch;size_t validation_scratch=shape_scratch>validation_roots?shape_scratch:validation_roots;size_t total_validation_work=audit_meter.used;if(!add(&total_validation_work,validation_meter.used)){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.474.5.reject",d,"runtime host ABI validation work overflow");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}if(measuring_validation_work){observed_validation_work=total_validation_work;return SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED;}
#define USAGE_MISMATCH(index,id,left_name,left_value,right_name,right_value) do { \
    if(sol_mir_runtime_host_abi_observe_ne(id,left_name,left_value, \
            right_name,right_value)){ \
        observe_usage_unavailable_from((index)+1); \
        sol_mir_runtime_host_abi_observe_fieldwise_row( \
            "H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.usage.row", \
            &o->usage,sizeof(o->usage),0); \
        bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.reject", \
            d,"runtime host ABI usage mismatch"); \
        return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR; \
    } \
} while(0)
        USAGE_MISMATCH(0,"H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.01.capabilities",
            "usage.capabilities",o->usage.capabilities,"capability_count",o->capability_count);
        USAGE_MISMATCH(1,"H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.02.entry_roots",
            "usage.entry_roots",o->usage.entry_roots,"entry_root_count",o->entry_root_count);
        USAGE_MISMATCH(2,"H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.03.operations",
            "usage.operations",o->usage.operations,"operation_count",o->operation_count);
        USAGE_MISMATCH(3,"H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.04.arguments",
            "usage.arguments",o->usage.arguments,"argument_count",o->argument_count);
        USAGE_MISMATCH(4,"H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.05.formals",
            "usage.formals",o->usage.formals,"formal_count",o->formal_count);
        USAGE_MISMATCH(5,"H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.06.shapes",
            "usage.shapes",o->usage.shapes,"shape_count",o->shape_count);
        USAGE_MISMATCH(6,"H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.07.shape_cases",
            "usage.shape_cases",o->usage.shape_cases,"shape_case_count",o->shape_case_count);
        USAGE_MISMATCH(7,"H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.08.requirements",
            "usage.requirements",o->usage.requirements,"requirement_count",o->requirement_count);
        USAGE_MISMATCH(8,"H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.09.grants",
            "usage.grants",o->usage.grants,"grants",grants);
        USAGE_MISMATCH(9,"H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.10.owned_bytes",
            "usage.owned_bytes",o->usage.owned_bytes,"owned_bytes",bytes);
        USAGE_MISMATCH(10,"H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.11.build_scratch_bytes",
            "usage.build_scratch_bytes",o->usage.build_scratch_bytes,
            "build_scratch_bytes",build_scratch);
        USAGE_MISMATCH(11,"H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.12.validation_scratch_bytes",
            "usage.validation_scratch_bytes",o->usage.validation_scratch_bytes,
            "validation_scratch_bytes",validation_scratch);
        USAGE_MISMATCH(12,"H37.V.sol_mir_runtime_host_abi_internal_validate.474.6.13.validation_work",
            "usage.validation_work",o->usage.validation_work,
            "validation_work",total_validation_work);
#undef USAGE_MISMATCH
        if(
            sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.01.capabilities","usage.capabilities",o->usage.capabilities,"limit.capabilities",o->limits.max_capabilities)
            ||sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.02.entry_roots","usage.entry_roots",o->usage.entry_roots,"limit.entry_roots",o->limits.max_entry_roots)
            ||sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.03.operations","usage.operations",o->usage.operations,"limit.operations",o->limits.max_operations)
            ||sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.04.arguments","usage.arguments",o->usage.arguments,"limit.arguments",o->limits.max_arguments)
            ||sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.05.formals","usage.formals",o->usage.formals,"limit.formals",o->limits.max_formals)
            ||sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.06.shapes","usage.shapes",o->usage.shapes,"limit.shapes",o->limits.max_shapes)
            ||sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.07.shape_cases","usage.shape_cases",o->usage.shape_cases,"limit.shape_cases",o->limits.max_shape_cases)
            ||sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.08.requirements","usage.requirements",o->usage.requirements,"limit.requirements",o->limits.max_requirements)
            ||sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.09.grants","usage.grants",o->usage.grants,"limit.grants",o->limits.max_grants)
            ||sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.10.owned_bytes","usage.owned_bytes",o->usage.owned_bytes,"limit.owned_bytes",o->limits.max_owned_bytes)
            ||sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.11.build_scratch_bytes","usage.build_scratch_bytes",o->usage.build_scratch_bytes,"limit.build_scratch_bytes",o->limits.max_build_scratch_bytes)
            ||sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.12.build_work","usage.build_work",o->usage.build_work,"limit.build_work",o->limits.max_build_work)
            ||sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.13.validation_scratch_bytes","usage.validation_scratch_bytes",o->usage.validation_scratch_bytes,"limit.validation_scratch_bytes",o->limits.max_validation_scratch_bytes)
            ||sol_mir_runtime_host_abi_observe_gt("H37.V.sol_mir_runtime_host_abi_internal_validate.475.14.validation_work","usage.validation_work",o->usage.validation_work,"limit.validation_work",o->limits.max_validation_work)
        ){bad_at("H37.V.sol_mir_runtime_host_abi_internal_validate.475.1.reject",d,"runtime host ABI usage exceeds owner limit");return SOL_MIR_RUNTIME_HOST_ABI_BUILD_INVALID_PREDECESSOR;}
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    sol_mir_runtime_host_abi_internal_record_validation_work(audit_meter.used,
        validation_meter.used);
#endif
    return SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED;
}
#ifdef SOL_MIR_PLAN_TEST_HOOKS
SolMirRuntimeHostAbiBuildOutcome sol_mir_runtime_host_abi_internal_validate(
    const SolMirRuntimeHostAbi *o, SolDiagnostics *d) {
    SolMirRuntimeHostAbiObservationContext *context=
        sol_mir_runtime_host_abi_observation_context;
    if (!context) return host_abi_validate_observed_body(o,d);
    bool previous_validator_active=context->validator_active;
    context->validator_active=true;
    SolMirRuntimeHostAbiBuildOutcome result=
        host_abi_validate_observed_body(o,d);
    context->validator_active=previous_validator_active;
    sol_mir_runtime_host_abi_observe_validator_result(
        context->measurement_active?"measurement":"final",result);
    return result;
}
#endif
bool sol_mir_runtime_host_abi_validate(const SolMirRuntimeHostAbi*o,SolDiagnostics*d){return sol_mir_runtime_host_abi_internal_validate(o,d)==SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED;}
bool sol_mir_runtime_host_abi_internal_measure_validation_work(
    const SolMirRuntimeHostAbi *o, size_t *work) {
    if (!o || !work) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
        sol_mir_runtime_host_abi_observe_failure("H37.V.sol_mir_runtime_host_abi_internal_measure_validation_work.485.arguments","measurement-entry","original measurement argument check rejected",NULL);
#endif
        return false;
    }
    measuring_validation_work=true;
    observed_validation_work=0;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    SolMirRuntimeHostAbiObservationContext *context=
        sol_mir_runtime_host_abi_observation_context;
    bool previous_measurement_active=context?context->measurement_active:false;
    if (context) context->measurement_active=true;
#endif
    SolMirRuntimeHostAbiBuildOutcome result=
        sol_mir_runtime_host_abi_internal_validate(o,NULL);
    measuring_validation_work=false;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (context) context->measurement_active=previous_measurement_active;
#endif
    if (result!=SOL_MIR_RUNTIME_HOST_ABI_BUILD_SUCCEEDED) return false;
    *work=observed_validation_work;
    return true;
}
